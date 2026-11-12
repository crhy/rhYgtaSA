#!/bin/bash
# Fetch and verify every dependency listed in a deps manifest (deps/manifest.json).
#
# usage: fetch-deps.sh MANIFEST DEST [options]
#
#   MANIFEST  JSON manifest (schema 1): per file name, size, sha256, source page,
#             author, licence/credit, target path in the game folder, and one or
#             more urls. Kinds: mirror (our https host), torrent/magnet (our
#             torrent), direct (the author's own file link, not our host) and
#             source (the author's page - credit + the manual fallback, never
#             auto-fetched, because it is an HTML page, not the archive).
#   DEST      directory the archives land in (apply.sh uses REPO/build/deps,
#             git-ignored). Never the game folder: the game folder only ever sees
#             a file whose sha256 matched.
#
# options
#   --only id[,id]      only these entries
#   --group name[,...]  only these groups (base | characters | vegetation | ...)
#   --verify-only       no network: just check what is already in DEST
#   --force             re-download even when the hash already matches
#   --no-torrent        ignore magnet:/torrent urls (https only)
#   --insecure          allow entries whose sha256 is "TBD" (default: refuse)
#   --plan              print "name\x1ftarget\x1funpack" for every entry whose
#                       file is present and verified - this is what apply.sh unpacks
#   --list              print the manifest (id, group, name, hash state, target)
#   --dump              print the validated reader rows (\x1f separated) and exit -
#                       this is how tools/make-release.sh reads the manifest without
#                       becoming a second reader
#   --quiet             no progress lines, only warnings/errors
#
# environment (used by the tests, harmless otherwise)
#   RHYS_ARIA2C  path to aria2c (default: aria2c on PATH)
#   RHYS_CURL    path to curl    (default: curl on PATH)
#
# How a file is fetched, in the order the manifest lists its urls:
#   magnet:/torrent  -> aria2c, the only thing that speaks them. No aria2c -> a
#                       clear error naming the package to install, not a stack trace.
#   https://         -> aria2c when present (resumable, multi-connection), else
#                       curl -fL -C - (also resumable).
# The finished file is renamed into place only after its sha256 matched, so a half
# file is always <name>.part and a wrong file is always <name>.bad-sha256.
# An entry with no usable url is not a failure: it prints the author's page as a
# manual download, which is exactly the state our hosts are in today for the packs
# that forbid re-upload (docs/CHARACTERS.md section 9).
#
# exit: 0 everything selected is present+verified (or a documented manual
#       download), 1 at least one entry failed, 2 usage or manifest error.
set -uo pipefail

ARIA2C="${RHYS_ARIA2C:-aria2c}"
CURL="${RHYS_CURL:-curl}"
SHA_TOOL="$(command -v sha256sum || true)"

usage() {
  echo "usage: fetch-deps.sh MANIFEST DEST [--only id[,...]] [--group g[,...]]"
  echo "                       [--verify-only] [--force] [--no-torrent] [--insecure]"
  echo "                       [--plan] [--list] [--quiet]"
  echo "       MANIFEST: deps/manifest.json (schema 1); DEST: where archives land"
}

MANIFEST="" DEST="" ONLY="" GROUPSEL="" VERIFY_ONLY=0 FORCE=0 NO_TORRENT=0 INSECURE=0
PLAN=0 LIST=0 DUMP=0 QUIET=0
while [ $# -gt 0 ]; do
  case "$1" in
    --only)        [ $# -ge 2 ] || { echo "fetch-deps: --only needs a value" >&2; exit 2; }; ONLY="$2"; shift 2 ;;
    --group)       [ $# -ge 2 ] || { echo "fetch-deps: --group needs a value" >&2; exit 2; }; GROUPSEL="$2"; shift 2 ;;
    --verify-only) VERIFY_ONLY=1; shift ;;
    --force)       FORCE=1; shift ;;
    --no-torrent)  NO_TORRENT=1; shift ;;
    --insecure)    INSECURE=1; shift ;;
    --plan)        PLAN=1; shift ;;
    --list)        LIST=1; shift ;;
    --dump)        DUMP=1; shift ;;
    --quiet|-q)    QUIET=1; shift ;;
    --help|-h)     usage; exit 0 ;;
    --*)           echo "fetch-deps: unknown option: $1" >&2; usage >&2; exit 2 ;;
    *)             if [ -z "$MANIFEST" ]; then MANIFEST="$1"; elif [ -z "$DEST" ]; then DEST="$1";
                     else echo "fetch-deps: too many arguments (last was $1)" >&2; exit 2; fi; shift ;;
  esac
done
if [ -z "$MANIFEST" ] || { [ -z "$DEST" ] && [ "$DUMP" = 0 ]; }; then usage >&2; exit 2; fi
[ -f "$MANIFEST" ] || { echo "fetch-deps: no such manifest: $MANIFEST" >&2; exit 2; }
if [ "$DUMP" = 0 ]; then
  if [ -e "$DEST" ] && [ ! -d "$DEST" ]; then echo "fetch-deps: DEST is not a directory: $DEST" >&2; exit 2; fi
  mkdir -p "$DEST" || { echo "fetch-deps: cannot create DEST: $DEST" >&2; exit 2; }
  DEST="$(cd "$DEST" && pwd)"
fi

say() { [ "$QUIET" = 1 ] || echo "$@"; }
warn() { echo "$@" >&2; }

# ---------------------------------------------------------------- manifest ----
# One reader for the whole repo: python validates the manifest and emits one
# \x1f-separated line per entry. A manifest error is exit 2 - a typo in a sha256
# or a missing key must stop the installer, not download the wrong file.
# NOTE: this block is a bash single-quoted string - no apostrophes inside it.
READER_PY='
import json, re, sys
REQ = ["id","name","size","sha256","source_page","author","license","redistribute","target","urls"]
KINDS = {"mirror","torrent","magnet","direct","source"}
# mirror/torrent/magnet/direct are fetchable; "source" is the author page - it is
# credit plus the manual fallback, never auto-fetched (it is an HTML page).
UNPACK = {"auto","7z","zip","rar","tar","gz","none"}
REDIST = {"yes","no","ask"}
ID_RE = re.compile(r"^[a-z0-9][a-z0-9._-]*$")
SHA_RE = re.compile(r"^[0-9a-f]{64}$")

def die(msg):
    sys.stderr.write("manifest: %s\n" % msg)
    sys.exit(2)

try:
    with open(sys.argv[1], encoding="utf-8") as f:
        doc = json.load(f)
except Exception as e:
    die("%s: not valid JSON: %s" % (sys.argv[1], e))
if not isinstance(doc, dict):
    die("top level must be an object")
if doc.get("schema") != 1:
    die("unsupported schema %r (this fetch-deps.sh reads schema 1)" % (doc.get("schema"),))
files = doc.get("files")
if not isinstance(files, list) or not files:
    die("files must be a non-empty list")

ids, rows = set(), []
for i, e in enumerate(files):
    if not isinstance(e, dict):
        die("files[%d] is not an object" % i)
    where = "files[%d]" % i
    if isinstance(e.get("id"), str) and e.get("id"):
        where = "files[%d] id=%s" % (i, e["id"])
    for k in REQ:
        if k not in e:
            die("%s: missing required key %r" % (where, k))
    eid, name = e["id"], e["name"]
    if not isinstance(eid, str) or not ID_RE.match(eid or ""):
        die("%s: id %r must match %s" % (where, eid, ID_RE.pattern))
    if eid in ids:
        die("%s: duplicate id %r" % (where, eid))
    ids.add(eid)
    if (not isinstance(name, str) or not name or "/" in name or name in (".", "..")
            or any(c.isspace() for c in name)):
        die("%s: name %r must be a plain file name (no /, no whitespace)" % (where, name))
    if (e["size"] is not None and (not isinstance(e["size"], int) or isinstance(e["size"], bool)
                                   or e["size"] <= 0)):
        die("%s: size must be null or a positive integer of bytes" % where)
    sha = e["sha256"]
    if sha != "TBD" and not (isinstance(sha, str) and SHA_RE.match(sha)):
        die("%s: sha256 %r must be TBD or 64 hex characters" % (where, sha))
    page = e["source_page"]
    if page != "TBD" and not (isinstance(page, str) and page.startswith(("http://", "https://"))):
        die("%s: source_page %r must be TBD or an http(s) url" % (where, page))
    for k in ("author", "license"):
        if not isinstance(e[k], str) or not e[k].strip():
            die("%s: %s must be a non-empty string (credit is not optional)" % (where, k))
    if e["redistribute"] not in REDIST:
        die("%s: redistribute must be one of yes/no/ask" % where)
    target = e["target"]
    if not isinstance(target, str):
        die("%s: target must be a string (empty = the installer does not unpack it)" % where)
    if target:
        if (target.startswith("/") or ".." in target.split("/") or target.endswith("/")
                or "\t" in target or "\n" in target or target != target.strip()):
            die("%s: target %r must be a relative game-folder path without .. or tabs/newlines" % (where, target))
    urls = e["urls"]
    if not isinstance(urls, list) or not urls:
        die("%s: urls must be a non-empty list" % where)
    spec = []
    for u in urls:
        if not isinstance(u, dict) or "kind" not in u or "url" not in u:
            die("%s: every url needs {kind,url}" % where)
        if u["kind"] not in KINDS:
            die("%s: url kind %r must be one of %s" % (where, u["kind"], "|".join(sorted(KINDS))))
        url = u["url"]
        if not isinstance(url, str) or not url or any(c.isspace() for c in url):
            die("%s: url %r must not contain whitespace (percent-encode magnets)" % (where, url))
        spec.append("%s=%s" % (u["kind"], url))
    unpack = e.get("unpack", "auto")
    if unpack not in UNPACK:
        die("%s: unpack %r must be one of %s" % (where, unpack, "|".join(sorted(UNPACK))))
    req = e.get("requires", [])
    if not isinstance(req, list) or any(not isinstance(r, str) for r in req):
        die("%s: requires must be a list of ids" % where)
    # two fields the release bundle needs (tools/make-release.sh): the file name the
    # author used, so a staged download can be matched to the entry, and an explicit
    # local path for a file we host. "TBD" upstream_name = nobody wrote it down.
    up = e.get("upstream_name", "")
    if not isinstance(up, str) or up == "TBD":
        up = ""
    loc = e.get("local_file", "")
    if not isinstance(loc, str):
        die("%s: local_file must be a string (path of the file we host)" % where)
    rows.append((eid, str(e.get("group") or ""), str(e.get("priority") or ""), name,
                 str(e["size"]) if e["size"] is not None else "", sha, unpack,
                 e["redistribute"], page, target, " ".join(spec), ",".join(req), up, loc))

for r in rows:
    for dep in r[11].split(",") if r[11] else []:
        if dep not in ids:
            die("id=%s: requires unknown id %r" % (r[0], dep))

# the transport to bash is one line per entry with \x1f between fields (tab would
# collapse empty fields in `read`), so no field may contain a control character
for r in rows:
    for f in r:
        if any(ord(c) < 0x20 or c == "\x7f" for c in f):
            die("id=%s: field %r contains a control character" % (r[0], f))

for r in rows:
    sys.stdout.write("\x1f".join(r) + "\n")
'

ROWS_OUT="$(python3 -c "$READER_PY" "$MANIFEST")" || exit 2
if [ -z "$ROWS_OUT" ]; then echo "fetch-deps: manifest has no entries: $MANIFEST" >&2; exit 2; fi
mapfile -t ROWS <<< "$ROWS_OUT"
if [ "$DUMP" = 1 ]; then printf '%s\n' "${ROWS[@]}"; exit 0; fi

selected() {   # $1 id  $2 group
  local w
  if [ -n "$ONLY" ]; then
    for w in ${ONLY//,/ }; do [ "$w" = "$1" ] && return 0; done
    return 1
  fi
  if [ -n "$GROUPSEL" ]; then
    for w in ${GROUPSEL//,/ }; do [ "$w" = "$2" ] && return 0; done
    return 1
  fi
  return 0
}

# --------------------------------------------------------------- checksums ----
sha256_of() {   # $1 file -> hex on stdout
  if [ -n "$SHA_TOOL" ]; then
    "$SHA_TOOL" "$1" 2>/dev/null | cut -d' ' -f1
  else
    python3 -c 'import hashlib,sys
h = hashlib.sha256()
with open(sys.argv[1], "rb") as f:
    for b in iter(lambda: f.read(1 << 20), b""):
        h.update(b)
print(h.hexdigest())' "$1"
  fi
}
GOT_SHA=""
matches() {   # $1 file $2 expected -> 0 ok, non-0 otherwise (GOT_SHA holds what we saw)
  GOT_SHA=""
  [ -f "$1" ] || return 2
  GOT_SHA="$(sha256_of "$1")"
  [ -n "$GOT_SHA" ] || { GOT_SHA=""; return 2; }
  [ "$GOT_SHA" = "$2" ]
}

# ---------------------------------------------------------------- download ----
have_aria2c() { command -v "$ARIA2C" >/dev/null 2>&1; }

dl_aria2() {   # $1 url $2 final path
  say "GET    $(basename "$2") via aria2c: $1"
  "$ARIA2C" --dir="$(dirname "$2")" --out="$(basename "$2")" \
            --continue=true --allow-overwrite=false --auto-file-renaming=false \
            --check-integrity=false --seed-time=0 --summary-interval=0 \
            --console-timer=30s "$1" >&2
}

dl_curl() {   # $1 url $2 final path
  local part="$2.part" rc
  say "GET    $(basename "$2") via curl: $1"
  "$CURL" -fL --connect-timeout 20 --retry 3 --retry-delay 1 -C - \
          --progress-bar -o "$part" "$1" >&2
  rc=$?
  if [ "$rc" = 33 ]; then                 # server refused the Range request: start over
    say "WARN   $(basename "$2"): $1 does not support resume - restarting from byte 0"
    "$CURL" -fL --connect-timeout 20 --retry 3 --retry-delay 1 \
            --progress-bar -o "$part" "$1" >&2
    rc=$?
  fi
  [ "$rc" = 0 ] && mv -f "$part" "$2"
  return "$rc"
}

fetch_url() {   # $1 kind $2 url $3 final path
  case "$1" in
    magnet|torrent)
      if have_aria2c; then dl_aria2 "$2" "$3"; return $?; fi
      warn "FAIL   $(basename "$3"): a $1 url needs aria2c, and it is not installed (looked for '$ARIA2C')."
      warn "       install it (Debian/Ubuntu: apt install aria2 | Fedora: dnf install aria2 |"
      warn "       Arch: pacman -S aria2) or use this entry's https mirror instead."
      return 1 ;;
    *)
      if have_aria2c; then
        dl_aria2 "$2" "$3" && return 0
        say "WARN   $(basename "$3"): aria2c failed on $2 - falling back to curl"
      fi
      dl_curl "$2" "$3"; return $? ;;
  esac
}

# --------------------------------------------------------------- one entry ----
N_OK=0 N_PRESENT=0 N_MANUAL=0 N_FAIL=0 N_SKIP=0
FAILED_IDS=""

plan_line() { [ "$PLAN" = 1 ] && [ -n "$3" ] && printf '%s\x1f%s\x1f%s\n' "$1" "$3" "$2"; }

entry() {   # id group prio name size sha unpack redist page target urls requires
  local id="$1" group="$2" name="$4" size="$5" sha="$6" unpack="$7" redist="$8"
  local page="$9" target="${10}" urls="${11}"
  local dest="$DEST/$name"
  local kind url spec

  if ! selected "$id" "$group"; then N_SKIP=$((N_SKIP+1)); return 0; fi

  if [ "$LIST" = 1 ]; then
    printf '%s\t%s\t%s\t%s\t%s\n' "$id" "$group" "$name" \
      "$([ "$sha" = TBD ] && echo 'sha256:TBD' || echo 'sha256:set')" "$target"
    return 0
  fi

  # already there and correct? the idempotent path: no network at all
  if [ "$sha" != TBD ] && [ -f "$dest" ] && [ "$FORCE" = 0 ]; then
    if matches "$dest" "$sha"; then
      say "OK     $id: $name already in DEST, sha256 verified"
      N_PRESENT=$((N_PRESENT+1)); plan_line "$name" "$unpack" "$target"; return 0
    fi
    warn "WARN   $id: $name in DEST has the WRONG sha256 (got $GOT_SHA, wanted $sha) - kept as $name.bad-sha256, re-downloading"
    mv -f "$dest" "$dest.bad-sha256"
  fi

  # which urls can we actually use? "source" is a page, not a file, so it never
  # counts here - that is what turns an entry we do not host yet into a MANUAL line.
  local usable=()
  for spec in $urls; do
    kind="${spec%%=*}"; url="${spec#*=}"
    [ "$url" = TBD ] && continue
    case "$kind" in magnet|torrent) [ "$NO_TORRENT" = 1 ] && continue ;; esac
    case "$kind" in mirror|torrent|magnet|direct) usable+=("$kind $url") ;; esac
  done
  if [ "${#usable[@]}" = 0 ]; then
    say "MANUAL $id: nothing hosted yet (redistribute: $redist) - download '$name' by hand from $page and put it in $DEST"
    N_MANUAL=$((N_MANUAL+1)); return 0
  fi

  if [ "$VERIFY_ONLY" = 1 ]; then
    if [ "$sha" = TBD ]; then
      warn "FAIL   $id: sha256 is TBD in the manifest, so $name cannot be verified (--insecure to allow)"
      N_FAIL=$((N_FAIL+1)); FAILED_IDS="$FAILED_IDS $id"; return 0
    fi
    if matches "$dest" "$sha"; then
      say "OK     $id: $name verified"
      N_OK=$((N_OK+1)); plan_line "$name" "$unpack" "$target"
    else
      warn "FAIL   $id: $name is not in DEST (or does not match sha256 $sha)"
      N_FAIL=$((N_FAIL+1)); FAILED_IDS="$FAILED_IDS $id"
    fi
    return 0
  fi

  if [ "$sha" = TBD ] && [ "$INSECURE" = 0 ]; then
    warn "FAIL   $id: sha256 is TBD in the manifest - refusing to fetch unverified bytes for a game folder."
    warn "       hash the file once we host it (sha256sum $name) and fill deps/manifest.json, or pass --insecure."
    N_FAIL=$((N_FAIL+1)); FAILED_IDS="$FAILED_IDS $id"; return 0
  fi

  local got=0 tried=0 k u
  for spec in "${usable[@]}"; do
    read -r k u <<< "$spec"
    tried=$((tried+1))
    if fetch_url "$k" "$u" "$dest"; then got=1; break; fi
  done
  if [ "$got" != 1 ]; then
    warn "FAIL   $id: $name: all $tried url(s) failed. Last resort: download it by hand from $page into $DEST."
    [ -f "$dest.part" ] && warn "       partial download kept in $name.part - running this again resumes it."
    N_FAIL=$((N_FAIL+1)); FAILED_IDS="$FAILED_IDS $id"; return 0
  fi

  if [ "$sha" = TBD ]; then
    if [ -n "$size" ] && [ -f "$dest" ]; then
      local actual; actual="$(stat -c%s "$dest" 2>/dev/null || stat -f%z "$dest")"
      [ "$actual" = "$size" ] || warn "WARN   $id: $name is $actual bytes, the manifest says $size"
    fi
    warn "WARN   $id: $name fetched UNVERIFIED (--insecure, sha256 TBD in the manifest)"
    N_OK=$((N_OK+1)); plan_line "$name" "$unpack" "$target"; return 0
  fi

  if matches "$dest" "$sha"; then
    say "OK     $id: $name fetched, sha256 verified"
    N_OK=$((N_OK+1)); plan_line "$name" "$unpack" "$target"
  else
    warn "FAIL   $id: $name sha256 MISMATCH: got $GOT_SHA, wanted $sha"
    warn "       the file was NOT installed; it is kept as $name.bad-sha256 for whoever runs the mirror"
    mv -f "$dest" "$dest.bad-sha256"
    N_FAIL=$((N_FAIL+1)); FAILED_IDS="$FAILED_IDS $id"
  fi
}

for row in "${ROWS[@]}"; do
  IFS=$'\x1f' read -r _id _group _prio _name _size _sha _unpack _redist _page _target _urls _req _up _loc <<< "$row"
  entry "$_id" "$_group" "$_prio" "$_name" "$_size" "$_sha" "$_unpack" "$_redist" "$_page" "$_target" "$_urls" "$_req"
done

if [ "$LIST" = 1 ]; then exit 0; fi
say "deps: $N_PRESENT present+verified, $N_OK fetched, $N_MANUAL manual download needed, $N_FAIL failed, $N_SKIP not selected (DEST $DEST)"
if [ "$N_FAIL" != 0 ]; then
  echo "deps: FAILED:$FAILED_IDS" >&2
  echo "deps: nothing that failed was installed; docs/CHARACTERS.md section 7 has the manual links" >&2
  exit 1
fi
exit 0
