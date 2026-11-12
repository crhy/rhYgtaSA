#!/bin/bash
# Build the self-hosted dependency bundle: one folder, one SHA256SUMS, one torrent,
# one magnet (task s14; rhY's rule: we host what users cannot fetch themselves).
#
#   usage: make-release.sh OUTDIR [options]
#
# What it does, in order:
#   1. reads the manifest through the ONE reader this repo has (fetch-deps.sh
#      --dump), so a manifest typo stops here with exit 2 instead of producing a
#      bundle of the wrong files;
#   2. collects every entry whose source is a local file we host - a file that
#      exists in a --stage directory (default vendor/, vendor/characters/) under
#      the manifest `name` or `upstream_name`, or named exactly by `local_file`;
#   3. writes OUTDIR/<bundle>/ with the archives, SHA256SUMS, the manifest copy and
#      a README, then OUTDIR/<bundle>.torrent, .magnet.txt and .bundle.json;
#   4. verifies itself: `sha256sum -c` on its own SHA256SUMS, and tools/torrent.py
#      check, which re-derives the info hash from the bytes on disk - a magnet you
#      did not verify is a link to whatever somebody else uploaded;
#   5. prints the magnet, and with --update-manifest fills the sha256/size/magnet
#      slots of a manifest (the https mirror slot stays TBD: there is no host yet).
#
# Licences decide what may be collected, not convenience: only `redistribute: "yes"`
# entries are bundled by default. --include-ask / --include-no widen that, and both
# demand --permission-note "who gave permission, when, for what", which is written
# into bundle.json and the bundle README so the permission travels with the files
# (docs/CHARACTERS.md section 9: MixMods forbids re-upload; the two LibertyCity Peds
# Vary authors answered "No" / "under no circumstances").
#
# options
#   --manifest PATH      manifest to read   (default deps/manifest.json)
#   --version VER        bundle version     (default the manifest's "updated")
#   --name NAME          bundle folder name (default rhygtasa-deps-<version>)
#   --stage DIR          where the files we host are staged (repeatable)
#   --trackers FILE      announce URLs, one per line (default deps/trackers.txt)
#   --torrent-tool T     auto | mktorrent | transmission-create | python (default auto)
#   --piece-length N     bytes, power of two (default: sized to the bundle)
#   --include-ask        also bundle redistribute: "ask" entries
#   --include-no         also bundle redistribute: "no" entries
#   --permission-note T  required with the two flags above
#   --update-manifest P  write sha256/size/magnet of the collected entries into P
#   --write-manifest     shorthand for --update-manifest <the --manifest path>
#   --force              allow OUTDIR inside the repo but outside build/
#   --quiet              less chatter
#
# env: RHYS_STAGE  colon-separated stage dirs, prepended to the defaults
#
# exit: 0 bundle built and verified, 1 nothing collectible / a check failed,
#       2 usage or manifest error.
set -uo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MANIFEST="$REPO/deps/manifest.json"
TRACKERS="$REPO/deps/trackers.txt"
OUTDIR="" VERSION="" NAME="" TOOL=auto PLEN=0 INCLUDE_ASK=0 INCLUDE_NO=0 NOTE=""
UPDATE_MANIFEST="" FORCE=0 QUIET=0 WRITE_MANIFEST=0
STAGES=()
add_stage() { [ -n "${1:-}" ] && STAGES+=("$1"); return 0; }
add_stage "$REPO/vendor"
add_stage "$REPO/vendor/characters"
if [ -n "${RHYS_STAGE:-}" ]; then
  IFS=: read -ra _envstage <<< "$RHYS_STAGE"
  for _s in "${_envstage[@]}"; do add_stage "$_s"; done
fi

usage() { awk 'NR>1 && /^set -uo/{exit} {sub(/^# ?/,""); print}' "${BASH_SOURCE[0]}"; }

while [ $# -gt 0 ]; do
  case "$1" in
    --manifest)        [ $# -ge 2 ] || { echo "make-release: --manifest needs a value" >&2; exit 2; }; MANIFEST="$2"; shift 2 ;;
    --version)         [ $# -ge 2 ] || { echo "make-release: --version needs a value" >&2; exit 2; }; VERSION="$2"; shift 2 ;;
    --name)            [ $# -ge 2 ] || { echo "make-release: --name needs a value" >&2; exit 2; }; NAME="$2"; shift 2 ;;
    --stage)           [ $# -ge 2 ] || { echo "make-release: --stage needs a value" >&2; exit 2; }; add_stage "$2"; shift 2 ;;
    --trackers)        [ $# -ge 2 ] || { echo "make-release: --trackers needs a value" >&2; exit 2; }; TRACKERS="$2"; shift 2 ;;
    --torrent-tool)    [ $# -ge 2 ] || { echo "make-release: --torrent-tool needs a value" >&2; exit 2; }; TOOL="$2"; shift 2 ;;
    --piece-length)    [ $# -ge 2 ] || { echo "make-release: --piece-length needs a value" >&2; exit 2; }; PLEN="$2"; shift 2 ;;
    --include-ask)     INCLUDE_ASK=1; shift ;;
    --include-no)      INCLUDE_NO=1; shift ;;
    --permission-note) [ $# -ge 2 ] || { echo "make-release: --permission-note needs a value" >&2; exit 2; }; NOTE="$2"; shift 2 ;;
    --update-manifest) [ $# -ge 2 ] || { echo "make-release: --update-manifest needs a value" >&2; exit 2; }; UPDATE_MANIFEST="$2"; shift 2 ;;
    --write-manifest)  WRITE_MANIFEST=1; shift ;;
    --force)           FORCE=1; shift ;;
    --quiet|-q)        QUIET=1; shift ;;
    --help|-h)         usage; exit 0 ;;
    --*)               echo "make-release: unknown option: $1" >&2; usage >&2; exit 2 ;;
    *)                 if [ -z "$OUTDIR" ]; then OUTDIR="$1"; else echo "make-release: too many arguments (last was $1)" >&2; exit 2; fi; shift ;;
  esac
done
[ -n "$OUTDIR" ] || { usage >&2; exit 2; }
# --write-manifest means "the manifest I passed", so it resolves after parsing, whatever order the flags came in
[ "$WRITE_MANIFEST" = 1 ] && UPDATE_MANIFEST="$MANIFEST"
case "$TOOL" in auto|mktorrent|transmission-create|python) ;; *)
  echo "make-release: --torrent-tool must be auto|mktorrent|transmission-create|python" >&2; exit 2 ;; esac
[ -f "$MANIFEST" ] || { echo "make-release: no such manifest: $MANIFEST" >&2; exit 2; }
[ -f "$TRACKERS" ] || { echo "make-release: no trackers file: $TRACKERS - a torrent with no tracker is a note to nobody" >&2; exit 2; }
if [ "$INCLUDE_ASK" = 1 ] || [ "$INCLUDE_NO" = 1 ]; then
  [ -n "$NOTE" ] || { echo "make-release: --include-ask/--include-no needs --permission-note \"who said yes, when, for what\"" >&2
                      echo "                 (docs/CHARACTERS.md section 9: the licence is not ours to waive)" >&2; exit 2; }
fi
if [ "$PLEN" != 0 ] && [ $((PLEN & (PLEN - 1))) != 0 ]; then
  echo "make-release: --piece-length must be a power of two (16384, 65536, ...)" >&2; exit 2
fi

say() { [ "$QUIET" = 1 ] || echo "$@"; }
warn() { echo "$@" >&2; }

# A bundle holds the real archives. Inside the repo that is only safe under build/
# (git-ignored); anywhere else it is one careless `git add` away from committing
# other people's mods, so it needs --force. The check runs before anything is
# created, so a refused OUTDIR does not leave a stray directory behind.
if [ -d "$OUTDIR" ]; then
  ABS_OUT="$(cd "$OUTDIR" && pwd)"
else
  _parent="$(dirname "$OUTDIR")"; _base="$(basename "$OUTDIR")"
  if [ -d "$_parent" ]; then ABS_OUT="$(cd "$_parent" && pwd)/$_base"; else ABS_OUT="$OUTDIR"; fi
fi
case "$ABS_OUT" in
  "$REPO/build"|"$REPO/build/"*) : ;;
  "$REPO"|"$REPO/"*)
    if [ "$FORCE" != 1 ]; then
      echo "make-release: OUTDIR $ABS_OUT is inside the repo but not under build/ - the bundle would" >&2
      echo "              be a commitable copy of other people's mods (--force if you know better)" >&2
      exit 2
    fi ;;
esac
mkdir -p "$ABS_OUT" || { echo "make-release: cannot create OUTDIR: $ABS_OUT" >&2; exit 2; }
OUTDIR="$(cd "$ABS_OUT" && pwd)"

# --------------------------------------------------------- manifest, once ----
ROWS_OUT="$(bash "$REPO/tools/fetch-deps.sh" "$MANIFEST" --dump)" || exit 2
[ -n "$ROWS_OUT" ] || { echo "make-release: manifest has no entries: $MANIFEST" >&2; exit 2; }
mapfile -t ROWS <<< "$ROWS_OUT"
if [ -z "$VERSION" ]; then
  VERSION="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1], encoding="utf-8")).get("updated") or "")' "$MANIFEST")"
  [ -n "$VERSION" ] || VERSION="$(date -u +%Y-%m-%d)"
fi
case "$VERSION" in *[!A-Za-z0-9._-]*) echo "make-release: --version must be a plain slug (got $VERSION)" >&2; exit 2 ;; esac
[ -n "$NAME" ] || NAME="rhygtasa-deps-$VERSION"
BUNDLE="$OUTDIR/$NAME"
TORRENT="$OUTDIR/$NAME.torrent"
MAGNET_TXT="$OUTDIR/$NAME.magnet.txt"
BUNDLE_JSON="$OUTDIR/$NAME.bundle.json"
[ "$BUNDLE" != "$OUTDIR" ] || { echo "make-release: bundle name is empty" >&2; exit 2; }
say "make-release: manifest $MANIFEST | version $VERSION | stages: ${STAGES[*]}"

# ------------------------------------------------------------- collect set ---
rm -rf -- "$BUNDLE"
mkdir -p -- "$BUNDLE" || { echo "make-release: cannot create $BUNDLE" >&2; exit 2; }
N_YES=0 N_SKIP=0 N_FAIL=0 FAILED=""
COLLECTED=()          # one record per hosted file: id \x1f name \x1f size \x1f sha256 \x1f redist \x1f source

find_staged() {       # $1 name $2 upstream_name $3 local_file -> path on stdout, 1 if none
  local cand d
  if [ -n "$3" ]; then                 # local_file names one exact path: no stage scan
    [ -f "$3" ] && { echo "$3"; return 0; }
    [ -f "$REPO/$3" ] && { echo "$REPO/$3"; return 0; }
    return 1
  fi
  for cand in "$1" "$2"; do
    [ -n "$cand" ] || continue
    for d in "${STAGES[@]}"; do
      [ -f "$d/$cand" ] && { echo "$d/$cand"; return 0; }
    done
  done
  return 1
}

sha256_of() { sha256sum "$1" 2>/dev/null | cut -d" " -f1; }
field() { printf '%s' "$1" | cut -d"$US" -f"$2"; }
US=$'\x1f'

for row in "${ROWS[@]}"; do
  IFS="$US" read -r id _group _prio name _size sha _unpack redist _page _target _urls _req up loc <<< "$row"
  allowed=1 why=""
  case "$redist" in
    yes) : ;;
    ask) [ "$INCLUDE_ASK" = 1 ] || { allowed=0; why="redistribute: ask (needs --include-ask --permission-note)"; } ;;
    no)  [ "$INCLUDE_NO"  = 1 ] || { allowed=0; why="redistribute: no (the author said no - docs/CHARACTERS.md section 9)"; } ;;
  esac
  if [ "$allowed" != 1 ]; then say "SKIP   $id: $why"; N_SKIP=$((N_SKIP+1)); continue; fi

  src="$(find_staged "$name" "$up" "$loc")" || {
    say "SKIP   $id: not staged - no $name${up:+, no $up} in: ${STAGES[*]}"
    N_SKIP=$((N_SKIP+1)); continue
  }
  got="$(sha256_of "$src")"
  if [ -z "$got" ]; then warn "FAIL   $id: cannot read $src"; N_FAIL=$((N_FAIL+1)); FAILED="$FAILED $id"; continue; fi
  if [ "$sha" != TBD ] && [ "$sha" != "$got" ]; then
    warn "FAIL   $id: staged $src has sha256 $got, the manifest says $sha. A bundle of the wrong file is worse than no bundle."
    N_FAIL=$((N_FAIL+1)); FAILED="$FAILED $id"; continue
  fi
  bytes="$(stat -c%s "$src" 2>/dev/null || stat -f%z "$src")"
  cp -p -- "$src" "$BUNDLE/$name" || { warn "FAIL   $id: cannot copy $src"; N_FAIL=$((N_FAIL+1)); FAILED="$FAILED $id"; continue; }
  chmod u+w -- "$BUNDLE/$name"
  if [ "$(sha256_of "$BUNDLE/$name")" != "$got" ]; then
    warn "FAIL   $id: the copy in the bundle does not match its source (a full disk?)"
    rm -f -- "$BUNDLE/$name"; N_FAIL=$((N_FAIL+1)); FAILED="$FAILED $id"; continue
  fi
  say "HOST   $id: $name ($bytes bytes, sha256 ${got:0:16}...)"
  COLLECTED+=("$id$US$name$US$bytes$US$got$US$redist$US$src")
  N_YES=$((N_YES+1))
done

if [ "$N_FAIL" != 0 ]; then
  rm -rf -- "$BUNDLE"
  warn "make-release: FAILED:$FAILED - nothing published (the half-built bundle was removed); fix the stage or the manifest first"
  exit 1
fi
if [ "$N_YES" = 0 ]; then
  rm -rf -- "$BUNDLE"
  warn "make-release: nothing to bundle - no manifest entry has a staged file we may host."
  warn "              stage the archives in ${STAGES[*]} (docs/CHARACTERS.md section 7), and remember"
  warn "              that redistribute: no stays unbundled until the authors say yes in writing."
  exit 1
fi

# ------------------------------------------------- SHA256SUMS, README, copy ---
NAMES=()
for rec in "${COLLECTED[@]}"; do NAMES+=("$(field "$rec" 2)"); done
( cd "$BUNDLE" && sha256sum -- "${NAMES[@]}" > SHA256SUMS ) || { warn "make-release: sha256sum failed"; exit 1; }
if ! ( cd "$BUNDLE" && sha256sum -c --quiet SHA256SUMS ); then
  warn "make-release: SHA256SUMS does not verify against the bundle we just wrote"
  exit 1
fi
say "SUMS   $(wc -l < "$BUNDLE/SHA256SUMS") lines in $BUNDLE/SHA256SUMS verify (sha256sum -c)"
cp -p -- "$MANIFEST" "$BUNDLE/manifest.json"
{
  echo "rhygtaSA dependency bundle $VERSION"
  echo
  echo "The archives a GTA SA + RTX Remix user would otherwise download by hand, in one"
  echo "place, with the manifest that describes them (manifest.json: author, licence,"
  echo "source page, where it goes in the game folder) and the hash of every file here"
  echo "(SHA256SUMS). Verify before installing anything:"
  echo
  echo "    sha256sum -c SHA256SUMS"
  echo
  echo "The magnet link and the torrent info hash are NOT inside this folder on purpose:"
  echo "a file inside a torrent cannot contain that torrent's own hash, because the hash"
  echo "covers these bytes. They are in $(basename "$TORRENT"), $(basename "$MAGNET_TXT")"
  echo "and $(basename "$BUNDLE_JSON"), next to this folder."
  echo
  echo "Licence (manifest.json -> redistribute): only entries the authors allow us to"
  echo "mirror are in here."
  [ -n "$NOTE" ] && echo "Permission for the wider set: $NOTE"
  echo "Install with the repo installer (apply.sh / deploy.sh): one folder per pack under"
  echo "modloader/, never merged, never into the game's own data/."
} > "$BUNDLE/README.txt"

# ------------------------------------------------------------- the torrent ---
TOTAL=0
for rec in "${COLLECTED[@]}"; do TOTAL=$((TOTAL + $(field "$rec" 3))); done
if [ "$PLEN" = 0 ]; then
  PLEN="$(python3 -c 'import sys; sys.path.insert(0, sys.argv[1]); import torrent; print(torrent.piece_length_for(int(sys.argv[2])))' "$REPO/tools" "$TOTAL")"
fi
EXP="$(python3 -c 'import math,sys; print(int(math.log2(int(sys.argv[1]))))' "$PLEN")"

have() { command -v "$1" >/dev/null 2>&1; }
case "$TOOL" in
  auto)
    if have mktorrent; then TOOL=mktorrent
    elif have transmission-create; then TOOL=transmission-create
    else TOOL=python; fi ;;
  mktorrent)           have mktorrent || { echo "make-release: mktorrent is not installed" >&2; exit 2; } ;;
  transmission-create) have transmission-create || { echo "make-release: transmission-create is not installed" >&2; exit 2; } ;;
esac
TR_CSV="$(tr '\n' "," < "$TRACKERS" | sed "s/,\$//")"
COMMENT="rhygtaSA dependency bundle $VERSION - verify SHA256SUMS before installing"
rm -f -- "$TORRENT"
case "$TOOL" in
  mktorrent) mktorrent -a "$TR_CSV" -l "$EXP" -n "$NAME" -o "$TORRENT" -d -c "$COMMENT" "$BUNDLE" ;;
  transmission-create)
    # -p is the piece size in KiB in transmission-create; not installed on this box,
    # so it is untested here - and the check below would catch a wrong piece length.
    targs=(); while IFS= read -r t; do [ -n "$t" ] && targs+=(-t "$t"); done < "$TRACKERS"
    transmission-create -p "$((PLEN/1024))" -n "$NAME" -o "$TORRENT" -c "$COMMENT" "${targs[@]}" "$BUNDLE" ;;
  python) python3 "$REPO/tools/torrent.py" make "$BUNDLE" -o "$TORRENT" -T "$TRACKERS" \
            --name "$NAME" --piece-length "$PLEN" --no-date --comment "$COMMENT" ;;
esac
[ -f "$TORRENT" ] || { warn "make-release: $TOOL produced no torrent file"; exit 1; }
say "TORRENT $TORRENT via $TOOL ($PLEN byte pieces)"

# The gate: the info hash has to recompute from the bytes on disk, not be trusted
# because a tool printed it.
if ! python3 "$REPO/tools/torrent.py" check "$TORRENT" --dir "$BUNDLE" -T "$TRACKERS"; then
  warn "make-release: the torrent does not verify against the bundle - not publishing a magnet for it"
  exit 1
fi
INFOHASH="$(python3 "$REPO/tools/torrent.py" show "$TORRENT" --json | python3 -c 'import json,sys; print(json.load(sys.stdin)["info_hash"])')"
[ -n "$INFOHASH" ] || { warn "make-release: could not read the info hash back"; exit 1; }
python3 "$REPO/tools/torrent.py" magnet "$TORRENT" -T "$TRACKERS" > "$MAGNET_TXT"
MAGNET="$(cat "$MAGNET_TXT")"

# bundle.json sits OUTSIDE the hashed folder: it names the info hash, and the info
# hash covers the folder's bytes.
printf '%s\n' "${COLLECTED[@]}" | python3 -c '
import json, sys, time
version, name, info_hash, magnet, torrent, tool, plen, trackers, note = sys.argv[1:10]
files = []
for line in sys.stdin:
    f = line.rstrip("\n").split("\x1f")
    files.append({"id": f[0], "name": f[1], "size": int(f[2]), "sha256": f[3],
                  "redistribute": f[4], "staged_from": f[5]})
print(json.dumps({
    "version": version, "bundle": name, "info_hash": info_hash, "magnet": magnet,
    "torrent": torrent, "torrent_tool": tool, "piece_length": int(plen),
    "trackers_file": trackers, "permission_note": note,
    "built_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
    "bytes": sum(f["size"] for f in files), "files": files}, indent=2))
' "$VERSION" "$NAME" "$INFOHASH" "$MAGNET" "$TORRENT" "$TOOL" "$PLEN" "$TRACKERS" "$NOTE" > "$BUNDLE_JSON" \
  || { warn "make-release: could not write $BUNDLE_JSON"; exit 1; }
say "META   $BUNDLE_JSON (info hash $INFOHASH)"

# --------------------------------------- manifest slots (the task step 3) ----
if [ -n "$UPDATE_MANIFEST" ]; then
  # Fill the slots the bundle actually creates: sha256, size and the magnet url of
  # every collected entry. The https mirror slot stays TBD on purpose - there is no
  # web host yet, the magnet is what exists. tools/manifest-update.py edits the text
  # so the hand-written manifest keeps its formatting and the diff stays reviewable;
  # it refuses to write anything that does not parse, and the reader gets the last word.
  printf '%s\n' "${COLLECTED[@]}" | python3 "$REPO/tools/manifest-update.py" "$UPDATE_MANIFEST" \
      --magnet "$MAGNET" --records - || { warn "make-release: could not update $UPDATE_MANIFEST"; exit 1; }
  if ! bash "$REPO/tools/fetch-deps.sh" "$UPDATE_MANIFEST" --dump >/dev/null; then
    warn "make-release: $UPDATE_MANIFEST no longer validates - look at it with git diff"
    exit 1
  fi
fi

say "make-release: $N_YES hosted, $N_SKIP skipped (licence / not staged), $N_FAIL failed"
echo "BUNDLE   $BUNDLE"
echo "TORRENT  $TORRENT"
echo "INFOHASH $INFOHASH"
echo "MAGNET   $MAGNET"
exit 0
