#!/usr/bin/env bash
# s17: the character packs as ONE switchable step - put the recommended combination from
# docs/CHARACTERS.md into a GTASA-RTX modloader in the documented order, and take exactly it
# out again.
#
#   tools/characters.sh on     [GAME_DIR] [--pack NAME ...] [--stage DIR ...]
#                             [--partial] [--drop-ped-spec]
#   tools/characters.sh off    [GAME_DIR]
#   tools/characters.sh status [GAME_DIR]
#
#   --pack NAME      only these packs. NAME is a manifest id ("proper-player-retex"), the
#                    modloader folder name ("Proper Player Retex"), or a path to a staged
#                    folder/archive. Repeat the flag; the install order stays the documented
#                    one no matter what order you name them in.
#   --stage DIR      where to look for staged packs (repeatable). Default:
#                    /media/rhy/data/Gamez/GTAmods/x/characters/stage, then REPO/vendor/characters.
#                    A stage dir may hold either an unpacked folder in modloader layout
#                    ("Proper Player Retex/") or the archive the manifest names
#                    ("proper-player-retex-3.2-pc.zip"), which is unpacked with tools/unpack.sh.
#   --partial        install the packs that ARE staged instead of refusing (default: refuse,
#                    so a half set is never mistaken for the documented combination)
#   --drop-ped-spec  also move modloader/Ped Spec out (docs/CHARACTERS.md 8.5: it changes the
#                    D3D material state and does nothing under Remix). `off` moves it back.
#
# Which packs, in which order, with which priority: deps/manifest.json, group "characters",
# read through the one manifest reader this repo has (tools/fetch-deps.sh --dump). That order
# is docs/CHARACTERS.md section 5's install order:
#   Proper Characters Retex HD (40) -> Proper Player Retex (60) -> PedFuncs
#   -> Emergency Peds Vary (50) -> Criminal Peds Vary (50) -> Restored Unused Peds (45)
# PedFuncs is installed automatically when a selected pack lists it in "requires" (the two
# Peds Vary packs do). Priorities are written into modloader.ini's [Profiles.Default.Priority]
# section - never appended at EOF, where they would land in whatever section comes last - and
# modloader.ini is backed up once, pristine, inside the state dir.
#
# Everything `on` does is recorded in <game>/modloader/.rhys-characters/ (a dot-dir, which
# ModLoader does not scan as a mod - it keeps its own .data/.profiles there): the pack list with
# its sources, the files it copied, one pristine backup of every file it edited, and anything it
# moved out. `off` deletes only the folders `on` created, puts every backup back byte-for-byte,
# moves back what it moved and removes that state dir, so an on/off round-trip leaves the game
# folder exactly as it was. A pack folder that was NOT created by this tool is never touched.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
GAME_DEFAULT="/media/rhy/data/Gamez/GTASA-RTX"
STAGE_DEFAULT="/media/rhy/data/Gamez/GTAmods/x/characters/stage"
MANIFEST="$REPO/deps/manifest.json"
GROUP="characters"
PED_SPEC="modloader/Ped Spec"

TMP="$REPO/build/tmp/characters"
mkdir -p "$TMP"

usage() { sed -n '2,44p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; }
die()   { echo "characters.sh: $*" >&2
          echo "usage: tools/characters.sh {on|off|status} [GAME_DIR] [--pack NAME ...] [--stage DIR ...] [--partial] [--drop-ped-spec]" >&2
          exit 1; }

action=""; game=""; partial=0; drop_pedspec=0
declare -a WANT=() STAGES=()
while [ $# -gt 0 ]; do
  case "$1" in
    on|off|status)
      [ -z "$action" ] || die "two actions given ('$action' and '$1')"
      action="$1"; shift ;;
    --pack)
      [ $# -ge 2 ] || die "--pack needs a pack id, a modloader folder name or a path"
      WANT+=("$2"); shift 2 ;;
    --stage)
      [ $# -ge 2 ] || die "--stage needs a directory"
      STAGES+=("$2"); shift 2 ;;
    --partial)      partial=1; shift ;;
    --drop-ped-spec) drop_pedspec=1; shift ;;
    -h|--help)      usage; exit 0 ;;
    *)
      [ -z "$game" ] || die "unexpected argument '$1'"
      game="$1"; shift ;;
  esac
done
[ -n "$action" ] || { usage; exit 2; }
game="${game:-$GAME_DEFAULT}"
[ "${#STAGES[@]}" -gt 0 ] || STAGES=("$STAGE_DEFAULT" "$REPO/vendor/characters")
[ -d "$game/modloader" ] || die "no modloader dir in '$game' - that is not a GTASA-RTX install (pass GAME_DIR as the second argument, default $GAME_DEFAULT)"
# Same sandbox guard apply.sh/deploy.sh/tools/vegetation.sh use:
# scripts/characters-selftest.sh exports RHYREMIX_INSTALLER_SANDBOX=1 and marks its scratch
# tree, so a bug in that test can never point this script at a real game folder.
if [ "${RHYREMIX_INSTALLER_SANDBOX:-}" = 1 ] && [ ! -f "$game/.fake-install-marker" ]; then
  die "refusing to touch $game: sandbox mode needs $game/.fake-install-marker"
fi

STATE="$game/modloader/.rhys-characters"
INI="modloader/modloader.ini"

# --------------------------------------------------------------- manifest -----
# One reader for the whole repo (tools/fetch-deps.sh validates the manifest and emits
# \x1f-separated rows: id group prio name size sha unpack redist page target urls requires ...).
[ -f "$MANIFEST" ] || die "no manifest at $MANIFEST"
declare -a ID=() PRIO=() ARCH=() UNPACKK=() TARGET=() REQ=() PAGE=()
row=()
while IFS= read -r line; do
  IFS=$'\x1f' read -r -a row <<< "$line"
  ID+=("${row[0]}"); PRIO+=("${row[2]}"); ARCH+=("${row[3]}"); UNPACKK+=("${row[6]}")
  TARGET+=("${row[9]}"); PAGE+=("${row[8]}")
  REQ+=("${row[11]}")
done < <(bash "$REPO/tools/fetch-deps.sh" "$MANIFEST" --dump 2>"$TMP/dump.err" \
         | awk -F$'\x1f' -v g="$GROUP" '$2 == g')
[ "${#ID[@]}" -gt 0 ] || die "no group '$GROUP' entries in $MANIFEST ($(tail -n 2 "$TMP/dump.err" | tr '\n' ' '))"

idx_of() { # <id> -> index in the arrays, or -1
  local i
  for i in "${!ID[@]}"; do [ "${ID[$i]}" = "$1" ] && { echo "$i"; return 0; }; done
  echo -1
}
base_of() { printf '%s\n' "${1##*/}"; }   # modloader/Proper Player Retex -> Proper Player Retex

# ------------------------------------------------------------- pack selection --
declare -a SEL=()             # indices, in manifest order
declare -A AUTO=()            # index -> the pack that pulled it in (a "requires" dependency)
declare -A SELECTED=()
resolve_want() { # <name> -> index, matching id, target basename (case-insensitive) or a path
  local w="$1" i lc wl
  for i in "${!ID[@]}"; do
    [ "${ID[$i]}" = "$w" ] && { echo "$i"; return 0; }
  done
  lc="$(printf '%s' "$w" | tr 'A-Z' 'a-z')"
  for i in "${!ID[@]}"; do
    wl="$(printf '%s' "$(base_of "${TARGET[$i]}")" | tr 'A-Z' 'a-z')"
    [ "$wl" = "$lc" ] && { echo "$i"; return 0; }
  done
  echo -1
}
if [ "${#WANT[@]}" = 0 ]; then
  for i in "${!ID[@]}"; do SEL+=("$i"); SELECTED["$i"]=1; done
else
  for w in "${WANT[@]}"; do
    i="$(resolve_want "$w")"
    if [ "$i" = -1 ] && { [ "${w:0:1}" = "/" ] || [ -e "$w" ]; }; then
      die "--pack '$w' looks like a path; name the pack (id or modloader folder name) and point --stage at the folder that contains it - the pack list comes from $MANIFEST"
    fi
    [ "$i" != -1 ] || die "unknown pack '$w'; the '$GROUP' packs are: ${ID[*]}"
    SELECTED["$i"]=1
  done
  for i in "${!ID[@]}"; do [ -n "${SELECTED[$i]:-}" ] && SEL+=("$i"); done
fi
# dependencies: a selected pack that "requires" another one pulls it in, in manifest order
for i in "${!ID[@]}"; do
  [ -n "${SELECTED[$i]:-}" ] || continue
  IFS=',' read -r -a deps <<< "${REQ[$i]}"
  for d in "${deps[@]}"; do
    [ -n "$d" ] || continue
    j="$(idx_of "$d")"
    [ "$j" != -1 ] || { echo "note: ${ID[$i]} requires '$d', which is not in group '$GROUP' of $MANIFEST"; continue; }
    if [ -z "${SELECTED[$j]:-}" ]; then
      SELECTED["$j"]=1; AUTO["$j"]="${ID[$i]}"
    fi
  done
done
SEL=()
for i in "${!ID[@]}"; do [ -n "${SELECTED[$i]:-}" ] && SEL+=("$i"); done

# ------------------------------------------------------- staged pack lookup ----
SRC=()      # resolved staged source per selected index ("" = not staged)
find_stage() { # <index> -> source path on stdout, empty if not staged
  local i="$1" name dir cand
  name="$(base_of "${TARGET[$i]}")"
  for dir in "${STAGES[@]}"; do
    [ -d "$dir" ] || continue
    for cand in "$dir/$name" "$dir/${ID[$i]}"; do
      [ -d "$cand" ] && { printf '%s\n' "$cand"; return 0; }
    done
    # case-insensitive folder match, then the archive the manifest names
    while IFS= read -r cand; do
      [ -n "$cand" ] && { printf '%s\n' "$cand"; return 0; }
    done < <(find "$dir" -mindepth 1 -maxdepth 1 -type d -iname "$name" 2>/dev/null)
    for cand in "$dir/${ARCH[$i]}" "$dir/${ID[$i]}.zip" "$dir/${ID[$i]}.rar" "$dir/${ID[$i]}.7z"; do
      [ -f "$cand" ] && { printf '%s\n' "$cand"; return 0; }
    done
  done
  return 1
}
unpack_how() { # <index> <file> -> unzip|unrar|7z
  local i="$1" f="${2,,}"
  case "$f" in
    *.zip) echo unzip ;; *.rar) echo unrar ;; *.7z) echo 7z ;;
    *) case "${UNPACKK[$i]}" in rar) echo unrar ;; 7z) echo 7z ;; *) echo unzip ;; esac ;;
  esac
}
resolve_sources() {
  local i src how out
  for i in "${SEL[@]}"; do
    src="$(find_stage "$i" || true)"
    if [ -z "$src" ]; then SRC[$i]=""; continue; fi
    if [ -d "$src" ]; then SRC[$i]="$src"; continue; fi
    # an archive: unpack it once into REPO/build/tmp (never into the game folder)
    out="$TMP/unpacked/${ID[$i]}"
    rm -rf "$out"; mkdir -p "$out"
    how="$(unpack_how "$i" "$src")"
    bash "$REPO/tools/unpack.sh" "$how" "$src" "$out" >/dev/null \
      || die "could not unpack $src ($how) for ${ID[$i]}"
    SRC[$i]="$out"
  done
}

# ------------------------------------------------------------- ini helpers ----
ini_get_sec() { # <file> <section> <key> -> value (comment stripped) or empty
  local f="$1" sec="$2" key="$3"
  [ -f "$f" ] || return 0
  awk -v ws="$sec" -v wk="$key" '
    function trim(s){ sub(/\r+$/,"",s); sub(/^[ \t]+/,"",s); sub(/[ \t]+$/,"",s); return s }
    { n++; L[n]=$0 }
    END {
      hdr="[" ws "]"; start=0
      for (i=1;i<=n;i++) if (trim(L[i])==hdr) { start=i; break }
      if (!start) exit
      end=n; for (i=start+1;i<=n;i++) if (trim(L[i]) ~ /^\[/) { end=i-1; break }
      for (i=start+1;i<=end;i++) {
        s=L[i]; if (s ~ /^[ \t]*[;#]/) continue
        p=index(s,"=")
        if (p>0 && tolower(trim(substr(s,1,p-1)))==tolower(wk)) {
          v=substr(s,p+1)
          for (j=1;j<=length(v);j++){ c=substr(v,j,1); if (c=="#"||c==";") { v=substr(v,1,j-1); break } }
          print trim(v); exit
        }
      }
    }' "$f"
}

ini_set_sec() { # <file> <section> <key> <value> - replace in the section, insert in the
               # section if missing, create the section if the file has none; never append
               # the key at EOF, where it would belong to the last section instead
  local f="$1" sec="$2" key="$3" val="$4" tmp="$TMP/ini.$$"
  awk -v ws="$sec" -v wk="$key" -v wv="$val" '
    function trim(s){ sub(/\r+$/,"",s); sub(/^[ \t]+/,"",s); sub(/[ \t]+$/,"",s); return s }
    { n++; L[n]=$0 }
    END {
      hdr="[" ws "]"; start=0
      for (i=1;i<=n;i++) if (trim(L[i])==hdr) { start=i; break }
      # ModLoader writes its ini with CRLF; keep whatever the file already uses so an
      # untouched line never changes bytes.
      eol=(n && L[n] ~ /\r$/) ? "\r" : ""
      if (!start) {
        for (i=1;i<=n;i++) print L[i]
        if (n && trim(L[n]) != "") print ""
        print hdr eol; print wk "=" wv eol; exit
      }
      eol=(L[start] ~ /\r$/) ? "\r" : ""
      end=n; for (i=start+1;i<=n;i++) if (trim(L[i]) ~ /^\[/) { end=i-1; break }
      hit=0; hp=0
      for (i=start+1;i<=end;i++) {
        s=L[i]; if (s ~ /^[ \t]*[;#]/) continue
        p=index(s,"=")
        if (p>0 && tolower(trim(substr(s,1,p-1)))==tolower(wk)) { hit=i; hp=p; break }
      }
      if (hit) {
        rest=substr(L[hit],hp+1); c=0
        for (j=1;j<=length(rest);j++){ ch=substr(rest,j,1); if (ch=="#"||ch==";") { c=j; break } }
        cm = c ? substr(rest,c) : ""; sub(/\r+$/,"",cm)
        L[hit]=trim(substr(L[hit],1,hp-1)) "=" wv (cm ? "   " cm : "") eol
      } else {
        at=start
        for (i=start+1;i<=end;i++) if (trim(L[i]) != "") at=i
        L[at]=L[at] "\n" wk "=" wv eol
      }
      for (i=1;i<=n;i++) print L[i]
    }' "$f" > "$tmp"
  cat "$tmp" > "$f"          # keep the inode and the mode; only the bytes change
  rm -f "$tmp"
}

backup_once() { # <rel-path-in-game> - one pristine copy, never overwritten
  local rel="$1"
  if grep -qxF -- "$rel" "$STATE/backups.txt" 2>/dev/null; then return 0; fi
  mkdir -p "$STATE/backups/$(dirname "$rel")"
  cp -p "$game/$rel" "$STATE/backups/$rel"
  printf '%s\n' "$rel" >> "$STATE/backups.txt"
}

state_add() { # <state-file> <line> - append once; a re-run must never forget what an
              # earlier run recorded, or 'off' would leave part of it behind
  local f="$1" line="$2"
  [ -f "$f" ] || : > "$f"
  grep -qxF -- "$line" "$f" || printf '%s\n' "$line" >> "$f"
}

# ------------------------------------------------- the Remix material warning --
# Measured from the pack's .usda files (docs/CHARACTERS.md 1 and 6): Remix matches a ped by
# material hash, so a game-side .txd replacement drops the pack's authored PBR for that ped.
material_note() {
  local i any=0
  for i in "${SEL[@]}"; do
    case "${ID[$i]}" in
      proper-player-retex)
        any=1
        echo "   Proper Player Retex replaces CJ's player_*.txd -> the pack's CJ overrides stop"
        echo "   matching: mat_0AB982F62FEF3381, mat_1AB00AD812498A17, mat_C23DD21AE7ED34B1"
        echo "   (and the shared 8645663DE7AFBB81 normal/rough pair). Re-point them: docs/CHARACTERS.md 6."
        ;;
      proper-characters-retex-hd)
        any=1
        echo "   Proper Characters Retex HD replaces 133 NPC .txd -> the two generic pedestrian"
        echo "   overrides stop matching: mat_E6E920AB99F5FC23, mat_A3537FCA9CCB9E69"
        echo "   (texture/pedestrians/Pedestrian_materials.usda). Re-point them: docs/CHARACTERS.md 6."
        ;;
      emergency-peds-vary|criminal-peds-vary)
        any=1
        echo "   ${ID[$i]} adds ped ids and re-textures existing ones (firefighters / gang members),"
        echo "   so any pack override for those peds stops matching while it is installed."
        ;;
      restored-unused-peds)
        any=1
        echo "   restored-unused-peds only re-adds cut ped models: no existing ped texture is replaced,"
        echo "   so no pack override is lost."
        ;;
    esac
  done
  [ "$any" = 1 ] && echo "   Ryder (mat_389470BE6FE18A28 + glasses) and Kendl (mat_B10446A2B08526A8)"
  [ "$any" = 1 ] && echo "   stay intact - none of these packs retexes them."
  return 0
}

laa_line() {
  local exe="$game/gta-sa.exe"
  [ -f "$exe" ] || { echo "gta-sa.exe : not in this install (memory ceiling not checked)"; return 0; }
  if python3 "$REPO/tools/laa.py" "$exe" --check >/dev/null 2>&1; then
    echo "gta-sa.exe : Large Address Aware set - the 2 GB ceiling the ped packs bump into is lifted"
  else
    echo "gta-sa.exe : NOT Large Address Aware - 2 GB for game + Remix + ped textures; run ./apply.sh '$game'"
  fi
}

# ---------------------------------------------------------------------- on ----
do_on() {
  # guard first: never merge into, or later delete, a pack folder we did not create
  local i tgt missing=0 have=0
  for i in "${SEL[@]}"; do
    tgt="$game/${TARGET[$i]}"
    if [ -d "$tgt" ] && ! (grep -qxF -- "${TARGET[$i]}" "$STATE/folders.txt" 2>/dev/null); then
      die "${TARGET[$i]} already exists but was not installed by this tool - remove it by hand, or 'off' will never undo it safely"
    fi
  done
  resolve_sources
  echo "== staged packs (looked in: ${STAGES[*]})"
  for i in "${SEL[@]}"; do
    if [ -n "${SRC[$i]}" ]; then
      have=$((have + 1))
      echo "   ${ID[$i]}  <- ${SRC[$i]}${AUTO[$i]:+ (pulled in: ${ID[$i]} is required by ${AUTO[$i]})}"
    else
      missing=$((missing + 1))
      echo "   ${ID[$i]}  NOT STAGED - put the unpacked pack or ${ARCH[$i]} in ${STAGES[0]}/, or fetch it from ${PAGE[$i]}"
    fi
  done
  if [ "$missing" -gt 0 ]; then
    if [ "$partial" != 1 ]; then
      die "$missing of ${#SEL[@]} selected pack(s) are not staged (nothing was installed). Stage them, or use --partial to install only what is there."
    fi
    echo "   --partial: installing $have of ${#SEL[@]}"
  fi
  [ "$have" -gt 0 ] || die "nothing staged to install - 'on' did nothing"
  # a dependency that is required but not staged: install the pack anyway (--partial said so)
  # but say out loud what will not work
  for i in "${SEL[@]}"; do
    [ -n "${SRC[$i]}" ] || continue
    IFS=',' read -r -a deps <<< "${REQ[$i]}"
    for d in "${deps[@]}"; do
      [ -n "$d" ] || continue
      j="$(idx_of "$d")"
      [ "$j" = -1 ] && continue
      [ -n "${SRC[$j]:-}" ] && continue
      echo "   WARN ${ID[$i]} requires ${ID[$j]}, which is not staged: it will load, but the ped"
      echo "        variant system (the whole point of the pack) will not work without it"
    done
  done

  mkdir -p "$STATE"
  # never truncate: an earlier run may have installed packs this run does not name, and
  # 'off' has to undo those too
  [ -f "$STATE/packs.txt" ]   || : > "$STATE/packs.txt"
  [ -f "$STATE/folders.txt" ] || : > "$STATE/folders.txt"
  [ -f "$STATE/backups.txt" ] || : > "$STATE/backups.txt"
  [ -f "$STATE/moved.txt" ]   || : > "$STATE/moved.txt"
  printf '%s\n' "${STAGES[*]}" > "$STATE/stage.txt"

  echo "== install order (docs/CHARACTERS.md 5 = the order $GROUP is listed in $MANIFEST)"
  local n_new=0 n_same=0
  for i in "${SEL[@]}"; do
    [ -n "${SRC[$i]}" ] || continue
    local src="${SRC[$i]%/}" dest="$game/${TARGET[$i]}" rel
    echo "   ${TARGET[$i]}${PRIO[$i]:+ (priority ${PRIO[$i]})}"
    mkdir -p "$dest"
    state_add "$STATE/folders.txt" "${TARGET[$i]}"
    state_add "$STATE/packs.txt" "${ID[$i]}	${TARGET[$i]}	$src"
    while IFS= read -r -d '' f; do
      rel="${f#"$src"/}"
      if [ -f "$dest/$rel" ] && cmp -s "$f" "$dest/$rel"; then n_same=$((n_same + 1)); continue; fi
      mkdir -p "$dest/$(dirname "$rel")"
      cp -p "$f" "$dest/$rel"
      n_new=$((n_new + 1))
    done < <(find "$src" -type f -print0 | sort -z)
    if [ -z "$(find "$dest" -type f -print -quit)" ]; then
      die "$src contains no files - '$dest' is empty; that is not a pack"
    fi
  done
  # added.txt = exactly what is inside the folders we created (a re-run can only converge)
  : > "$STATE/added.txt"
  while IFS= read -r tgt; do
    [ -d "$game/$tgt" ] || continue
    ( cd "$game/$tgt" && find . -type f -printf '%P\n' ) | sed "s|^|$tgt/|" >> "$STATE/added.txt"
  done < "$STATE/folders.txt"
  sort -o "$STATE/added.txt" "$STATE/added.txt"
  echo "   $n_new file(s) copied, $n_same already identical"

  echo "== priorities in $INI ([Profiles.Default.Priority], higher wins on a name clash)"
  if [ -f "$game/$INI" ]; then
    for i in "${SEL[@]}"; do
      [ -n "${SRC[$i]}" ] || continue
      [ -n "${PRIO[$i]}" ] || { echo "   ${ID[$i]}: no priority in the manifest (a plugin, not a replacement)"; continue; }
      local name cur
      name="$(base_of "${TARGET[$i]}")"
      cur="$(ini_get_sec "$game/$INI" "Profiles.Default.Priority" "$name")"
      if [ "$cur" = "${PRIO[$i]}" ]; then echo "   $name = ${PRIO[$i]} (already)"; continue; fi
      backup_once "$INI"
      ini_set_sec "$game/$INI" "Profiles.Default.Priority" "$name" "${PRIO[$i]}"
      echo "   $name ${cur:-<unset>} -> ${PRIO[$i]}"
    done
    echo "   $(wc -l < "$STATE/backups.txt") file(s) backed up pristine in .rhys-characters/backups"
  else
    echo "   no $INI in this install - skipped; ModLoader's default priority is 50 for every pack"
  fi

  if [ "$drop_pedspec" = 1 ]; then
    if [ -d "$game/$PED_SPEC" ] && ! grep -qxF -- "$PED_SPEC" "$STATE/moved.txt" 2>/dev/null; then
      mkdir -p "$STATE/moved/$(dirname "$PED_SPEC")"
      mv "$game/$PED_SPEC" "$STATE/moved/$PED_SPEC"
      state_add "$STATE/moved.txt" "$PED_SPEC"
      echo "== moved $PED_SPEC out (docs/CHARACTERS.md 8.5: it changes D3D material state and does nothing under Remix)"
    elif [ -d "$game/$PED_SPEC" ]; then
      echo "== $PED_SPEC already moved by this tool"
    else
      echo "== no $PED_SPEC in this install"
    fi
  fi

  echo "== what this costs the Remix pack's authored materials"
  material_note
  laa_line
  echo "done. Restart the game once, then check modloader/modloader.log for the 'Removing imported"
  echo "model file at index' lines and the Remix developer menu for CJ's new material hashes."
  echo "Roll everything back with: tools/characters.sh off '$game'"
}

# ---------------------------------------------------------------------- off ---
do_off() {
  local tgt
  if [ ! -d "$STATE" ]; then
    local found=0
    for i in "${!ID[@]}"; do [ -d "$game/${TARGET[$i]}" ] && { found=1; echo "   ${TARGET[$i]} exists but was not installed by this tool"; }; done
    [ "$found" = 0 ] && { echo "no character packs installed by this tool in $game (no $STATE) - nothing to undo"; return 0; }
    die "'off' only removes what 'on' added; the folders above stay as they are"
  fi
  if [ -f "$STATE/folders.txt" ]; then
    while IFS= read -r tgt; do
      [ -n "$tgt" ] || continue
      if [ -d "$game/$tgt" ]; then
        echo "   removing $tgt ($(find "$game/$tgt" -type f | wc -l) file(s))"
        rm -rf "${game:?}/${tgt:?}"
      fi
    done < "$STATE/folders.txt"
  fi
  if [ -f "$STATE/backups.txt" ]; then
    while IFS= read -r rel; do
      [ -n "$rel" ] || continue
      [ -f "$STATE/backups/$rel" ] || { echo "   WARN: no backup for $rel - left as is"; continue; }
      cp -p "$STATE/backups/$rel" "$game/$rel"
      echo "   restored $rel"
    done < "$STATE/backups.txt"
  fi
  if [ -f "$STATE/moved.txt" ]; then
    while IFS= read -r rel; do
      [ -n "$rel" ] || continue
      [ -d "$STATE/moved/$rel" ] || continue
      mkdir -p "$(dirname "$game/$rel")"
      mv "$STATE/moved/$rel" "$game/$rel"
      echo "   put back $rel"
    done < "$STATE/moved.txt"
  fi
  rm -rf "$STATE"
  echo "state dir removed: $game is back to its pre-'on' bytes."
}

# --------------------------------------------------------------------- status -
do_status() {
  echo "game dir : $game"
  echo "stage    : ${STAGES[*]}"
  echo "manifest : $MANIFEST (group $GROUP, that order is the install order)"
  echo "state    : $STATE"
  local i name cur inst staged stg
  printf '%-26s %-9s %-34s %-14s %s\n' 'PACK' 'PRIO' 'TARGET' 'STAGED' 'INSTALLED'
  for i in "${!ID[@]}"; do
    name="$(base_of "${TARGET[$i]}")"
    staged="$(find_stage "$i" || true)"
    stg="no"; [ -n "$staged" ] && stg="yes"
    if [ -d "$game/${TARGET[$i]}" ]; then
      if grep -qxF -- "${TARGET[$i]}" "$STATE/folders.txt" 2>/dev/null; then inst="by this tool"; else inst="HAND-INSTALLED"; fi
    else
      inst="no"
    fi
    printf '%-26s %-9s %-34s %-14s %s\n' "${ID[$i]}" "${PRIO[$i]:--}" "${TARGET[$i]}" \
      "$stg" "$inst"
  done
  for i in "${!ID[@]}"; do
    staged="$(find_stage "$i" || true)"
    [ -n "$staged" ] && printf '  staged at  %-26s %s\n' "${ID[$i]}" "$staged"
  done
  if [ -d "$STATE" ]; then
    echo "installed by this tool: $(wc -l < "$STATE/folders.txt" 2>/dev/null || echo 0) pack folder(s)," \
         "$(wc -l < "$STATE/added.txt" 2>/dev/null || echo 0) file(s)," \
         "$(wc -l < "$STATE/backups.txt" 2>/dev/null || echo 0) backup(s)"
    [ -s "$STATE/moved.txt" ] && { echo "moved out by this tool:"; sed 's/^/  /' "$STATE/moved.txt"; }
  fi
  echo "priorities now in $INI:"
  for i in "${!ID[@]}"; do
    cur="$(ini_get_sec "$game/$INI" "Profiles.Default.Priority" "$(base_of "${TARGET[$i]}")")"
    printf '  %-34s %s\n' "$(base_of "${TARGET[$i]}")" "${cur:-<default 50>}"
  done
  echo "what this costs the Remix pack's authored materials:"
  material_note
  laa_line
}

case "$action" in
  on)     do_on ;;
  off)    do_off ;;
  status) do_status ;;
esac
