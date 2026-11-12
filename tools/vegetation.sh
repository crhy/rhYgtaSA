#!/usr/bin/env bash
# s16: put the "V Vegetation Mix" pack into a GTASA-RTX modloader with the fixes from
# docs/VEGETATION.md applied as one ranked, switchable set of steps - and take it all out again.
#
#   tools/vegetation.sh on     [GAME_DIR] [--step N]   install the pack + fixes 1..N (default 3)
#   tools/vegetation.sh off    [GAME_DIR]              remove exactly what `on` added, restore backups
#   tools/vegetation.sh status [GAME_DIR]              what is installed, which fixes are live
#
#   --pack DIR   pack folder to install (default: the staged copy, see PACK_DEFAULT below)
#   --step N     0..4; 0 = the pack as its author shipped it
#
# The steps are the causes in docs/VEGETATION.md, in that doc's own "Test order":
#   1  cause 1: leave the pack's data/maps/generic/{procobj,vegepart}.ide OUT. They re-point
#      44 entries at dictionaries this install does not have (ids 784-788 want lod_veg_firs,
#      which exists nowhere), and the install that runs the pack fine has them deleted.
#   2  cause 2: streaming caps. StreamMemory = 1024 in both MixSets.ini copies (was 1900), and
#      in both ImprovedStreaming.ini copies StreamMemoryForced = 1024,
#      RemoveUnusedWhenPercent = 70, [Range4] Enabled = 0 and [Range5] Enabled = 0 - those two
#      KeepLoaded=1 ranges pin ~190 MB of this pack's vegetation in a 32-bit process.
#   3  cause 3: rtx.conf opacity micromap budget: maxBudgetSizeMB = 256 (was 1024),
#      minFreeVidmemMBToNotAllocate = 3072 (was 1536).
#   4  the pack bisect, a TEST not a fix: also leave the two heaviest dictionaries out
#      (gta3.img/insanity_pines.txd 72 MB, gta3.img/gta_tree_boak.txd 19 MB). Those trees
#      then have no textures; it is how you find the single file that hangs the run.
#
# Large Address Aware (the other half of cause 2) is apply.sh's job; this script only reports
# the flag and never rewrites the exe.
#
# Everything `on` does is recorded in <game>/modloader/.rhys-vegetation/ (a dot-dir, which
# ModLoader does not scan as a mod - it keeps its own .data/.profiles there): the list of files
# it copied, one pristine backup of every file a fix edits, and the step number. `off` deletes
# the pack folder it created, puts every backup back byte-for-byte and removes that state dir,
# so an on/off round-trip leaves the game folder exactly as it was.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
GAME_DEFAULT="/media/rhy/data/Gamez/GTASA-RTX"
PACK_DEFAULT="/media/rhy/data/Gamez/GTAmods/removed-from-game/rtx-copy/V Vegetation Mix"
STEP_DEFAULT=3
STEP_MAX=4

# pack-relative paths each step leaves out
IDE_FILES=("data/maps/generic/procobj.ide" "data/maps/generic/vegepart.ide")
HEAVY_FILES=("gta3.img/insanity_pines.txd" "gta3.img/gta_tree_boak.txd")

TMP="$REPO/build/tmp/vegetation"
mkdir -p "$TMP"

usage() { sed -n '2,33p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; }
die()   { echo "vegetation.sh: $*" >&2; echo "usage: tools/vegetation.sh {on|off|status} [GAME_DIR] [--step N] [--pack DIR]" >&2; exit 1; }

action=""; game=""; pack="$PACK_DEFAULT"; step="$STEP_DEFAULT"; step_given=0
while [ $# -gt 0 ]; do
  case "$1" in
    on|off|status)
      [ -z "$action" ] || die "two actions given ('$action' and '$1')"
      action="$1"; shift ;;
    --step)
      [ $# -ge 2 ] || die "--step needs a number 0..$STEP_MAX"
      case "$2" in ''|*[!0-9]*) die "--step must be an integer 0..$STEP_MAX, got '$2'" ;; esac
      [ "$2" -le "$STEP_MAX" ] || die "--step $2 is above the last step ($STEP_MAX)"
      step="$2"; step_given=1; shift 2 ;;
    --pack)
      [ $# -ge 2 ] || die "--pack needs a directory"
      pack="$2"; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *)
      [ -z "$game" ] || die "unexpected argument '$1'"
      game="$1"; shift ;;
  esac
done
[ -n "$action" ] || { usage; exit 2; }
game="${game:-$GAME_DEFAULT}"
pack="${pack%/}"
[ -d "$game/modloader" ] || die "no modloader dir in '$game' - that is not a GTASA-RTX install (pass GAME_DIR as the second argument, default $GAME_DEFAULT)"
# Same sandbox guard apply.sh/deploy.sh use: scripts/vegetation-selftest.sh exports
# RHYREMIX_INSTALLER_SANDBOX=1 and marks its scratch tree, so a bug in that test can
# never point this script at a real game folder.
if [ "${RHYREMIX_INSTALLER_SANDBOX:-}" = 1 ] && [ ! -f "$game/.fake-install-marker" ]; then
  die "refusing to touch $game: sandbox mode needs $game/.fake-install-marker"
fi

STATE="$game/modloader/.rhys-vegetation"
PACK_NAME="$(basename "$pack")"
DEST="$game/modloader/$PACK_NAME"

# --------------------------------------------------------------- ini helpers --
# These are .ini/rtx.conf key edits, not seds: the same key appears in several
# sections (Enabled in every [RangeN]), the value may carry a trailing comment
# that must survive, and a key the file has never had has to be appended.
ini_get() { # <file> <key> [section] -> value on stdout (comment stripped), empty if absent
  local f="$1" key="$2" sec="${3:-}"
  [ -f "$f" ] || return 0
  awk -v wk="$key" -v ws="$sec" '
    function trim(s){ sub(/^[ \t]+/,"",s); sub(/[ \t]+$/,"",s); return s }
    function val(s,  c){ c=length(s); for (j=1;j<=c;j++){ ch=substr(s,j,1); if (ch=="#"||ch==";") return substr(s,1,j-1) } return s }
    /^[ \t]*\[/ { sec=trim($0); next }
    {
      if (ws != "" && sec != "[" ws "]") next
      if ($0 ~ /^[ \t]*[;#]/) next
      i = index($0, "="); if (i == 0) next
      if (tolower(trim(substr($0,1,i-1))) == tolower(wk)) { print trim(val(substr($0,i+1))); exit }
    }' "$f"
}

ini_set() { # <file> <key> <value> [section] - replace in place, append if the key is missing
  local f="$1" key="$2" val="$3" sec="${4:-}" tmp="$TMP/ini.$$"
  awk -v wk="$key" -v ws="$sec" -v wv="$val" '
    function trim(s){ sub(/^[ \t]+/,"",s); sub(/[ \t]+$/,"",s); return s }
    /^[ \t]*\[/ { sec=trim($0); print; next }
    {
      if ((ws == "" || sec == "[" ws "]") && $0 !~ /^[ \t]*[;#]/) {
        i = index($0, "=")
        if (i > 0 && tolower(trim(substr($0,1,i-1))) == tolower(wk)) {
          rest = substr($0, i+1); c = 0
          for (j = 1; j <= length(rest); j++) { ch = substr(rest,j,1); if (ch=="#"||ch==";") { c = j; break } }
          print trim(substr($0,1,i-1)) " = " wv (c ? "   " substr(rest,c) : "")
          done = 1; next
        }
      }
      print
    }
    END { if (!done) print wk " = " wv "   # set by tools/vegetation.sh (docs/VEGETATION.md)" }
  ' "$f" > "$tmp"
  cat "$tmp" > "$f"          # keep the inode and the mode; only the bytes change
  rm -f "$tmp"
}

# ------------------------------------------------------------- step bookkeeping --
withheld() { # pack-relative paths the current step leaves out of the install
  [ "$step" -ge 1 ] && printf '%s\n' "${IDE_FILES[@]}"
  [ "$step" -ge 4 ] && printf '%s\n' "${HEAVY_FILES[@]}"
  return 0
}
is_withheld() { local rel="$1"; withheld | grep -qxF -- "$rel"; }

backup_once() { # <rel-path-in-game> - one pristine copy, never overwritten
  local rel="$1"
  if grep -qxF -- "$rel" "$STATE/backups.txt" 2>/dev/null; then return 0; fi
  mkdir -p "$STATE/backups/$(dirname "$rel")"
  cp -p "$game/$rel" "$STATE/backups/$rel"
  printf '%s\n' "$rel" >> "$STATE/backups.txt"
}

edits=0
set_key() { # <rel-path-in-game> <key> <value> [section]
  local rel="$1" key="$2" val="$3" sec="${4:-}" cur
  if [ ! -f "$game/$rel" ]; then echo "   skip  $rel: not in this install"; return 0; fi
  cur="$(ini_get "$game/$rel" "$key" "$sec")"
  if [ "$cur" = "$val" ]; then echo "   ok    $rel: ${sec:+[$sec] }$key = $val (already)"; return 0; fi
  backup_once "$rel"
  ini_set "$game/$rel" "$key" "$val" "$sec"
  echo "   fix   $rel: ${sec:+[$sec] }$key ${cur:-<absent>} -> $val"
  edits=$((edits + 1))
}

apply_fixes() {
  if [ "$step" -ge 2 ]; then
    for rel in "modloader/MixSets/MixSets.ini" "modloader/Remix_fix/MixSets.ini"; do
      set_key "$rel" StreamMemory 1024
    done
    for rel in "modloader/Improved Streaming/ImprovedStreaming.ini" "modloader/Remix_fix/ImprovedStreaming.ini"; do
      set_key "$rel" StreamMemoryForced 1024
      set_key "$rel" RemoveUnusedWhenPercent 70
      set_key "$rel" Enabled 0 Range4
      set_key "$rel" Enabled 0 Range5
    done
  fi
  if [ "$step" -ge 3 ]; then
    set_key "rtx.conf" rtx.opacityMicromap.cache.maxBudgetSizeMB 256
    set_key "rtx.conf" rtx.opacityMicromap.cache.minFreeVidmemMBToNotAllocate 3072
  fi
}

# ----------------------------------------------------------------- reporting --
laa_line() {
  local exe="$game/gta-sa.exe"
  [ -f "$exe" ] || { echo "gta-sa.exe : not in this install (LAA not checked)"; return 0; }
  if python3 "$REPO/tools/laa.py" "$exe" --check >/dev/null 2>&1; then
    echo "gta-sa.exe : Large Address Aware set (cause 2 needs this; apply.sh sets it)"
  else
    echo "gta-sa.exe : NOT Large Address Aware - run: ./apply.sh $game   (or tools/laa.py '$exe' --set)"
  fi
}

fix_state() { # what the inis/rtx.conf say right now, installed or not
  echo "streaming (cause 2):"
  for rel in "modloader/MixSets/MixSets.ini" "modloader/Remix_fix/MixSets.ini"; do
    printf '  %-46s StreamMemory = %s\n' "$rel" "$(ini_get "$game/$rel" StreamMemory)"
  done
  for rel in "modloader/Improved Streaming/ImprovedStreaming.ini" "modloader/Remix_fix/ImprovedStreaming.ini"; do
    printf '  %-46s StreamMemoryForced = %s, RemoveUnusedWhenPercent = %s, Range4 = %s, Range5 = %s\n' "$rel" \
      "$(ini_get "$game/$rel" StreamMemoryForced)" "$(ini_get "$game/$rel" RemoveUnusedWhenPercent)" \
      "$(ini_get "$game/$rel" Enabled Range4)" "$(ini_get "$game/$rel" Enabled Range5)"
  done
  echo "rtx.conf (cause 3):"
  printf '  %-46s maxBudgetSizeMB = %s, minFreeVidmemMBToNotAllocate = %s\n' rtx.conf \
    "$(ini_get "$game/rtx.conf" rtx.opacityMicromap.cache.maxBudgetSizeMB)" \
    "$(ini_get "$game/rtx.conf" rtx.opacityMicromap.cache.minFreeVidmemMBToNotAllocate)"
}

# ---------------------------------------------------------------------- on ----
do_on() {
  [ -d "$pack" ] || die "no pack at '$pack' (--pack DIR, default $PACK_DEFAULT)"
  [ -d "$pack/gta3.img" ] || die "'$pack' has no gta3.img/ - that is not the vegetation pack"
  local n_ide_missing=0
  for f in "${IDE_FILES[@]}"; do [ -f "$pack/$f" ] || n_ide_missing=$((n_ide_missing + 1)); done
  if [ "$n_ide_missing" = "${#IDE_FILES[@]}" ]; then
    echo "note: this pack has no ${IDE_FILES[0]} / ${IDE_FILES[1]} - step 1 is a no-op here"
  fi
  if [ -d "$DEST" ] && [ ! -d "$STATE" ]; then
    die "$DEST already exists but was not installed by this tool - remove it by hand (or point --pack at the folder it came from); 'off' only ever removes what 'on' added"
  fi

  mkdir -p "$STATE"
  printf '%s\n' "$step" > "$STATE/step"
  printf '%s\n' "$PACK_NAME" > "$STATE/pack"
  printf '%s\n' "$pack" > "$STATE/packsrc"
  [ -f "$STATE/added.txt" ] || : > "$STATE/added.txt"
  [ -f "$STATE/backups.txt" ] || : > "$STATE/backups.txt"

  echo "== $PACK_NAME -> $DEST (step $step$([ "$step_given" = 1 ] || echo ', default'))"
  local n_new=0 n_same=0 n_out=0 rel
  mkdir -p "$DEST"
  while IFS= read -r -d '' f; do
    rel="${f#"$pack"/}"
    if is_withheld "$rel"; then
      n_out=$((n_out + 1))
      if [ -f "$DEST/$rel" ]; then rm -f "$DEST/$rel"; echo "   out   $rel (step $step leaves it out)"; fi
      continue
    fi
    if [ -f "$DEST/$rel" ] && cmp -s "$f" "$DEST/$rel"; then n_same=$((n_same + 1)); continue; fi
    mkdir -p "$DEST/$(dirname "$rel")"
    cp -p "$f" "$DEST/$rel"
    n_new=$((n_new + 1))
  done < <(find "$pack" -type f -print0 | sort -z)
  # added.txt is exactly what is in the folder we created - the pack dir is ours, so
  # listing it beats tracking copies (a step change can take files back out).
  ( cd "$DEST" && find . -type f -printf '%P\n' ) | sort > "$STATE/added.txt"
  find "$DEST" -depth -type d -empty -delete 2>/dev/null || true
  echo "   $n_new copied, $n_same already there, $n_out left out by the steps"

  echo "== fixes (docs/VEGETATION.md, steps 1..$step)"
  apply_fixes
  echo "   $edits key(s) changed ($(wc -l < "$STATE/backups.txt") file(s) backed up in .rhys-vegetation/backups)"
  laa_line
  echo "done. Start with: $game/launch-rtx.sh - then read the first 60 s at Grove Street (docs/VEGETATION.md 'Test order')."
  echo "Roll everything back with: tools/vegetation.sh off '$game'"
}

# ---------------------------------------------------------------------- off ---
do_off() {
  if [ ! -d "$STATE" ]; then
    if [ -d "$DEST" ]; then
      die "$DEST was not installed by this tool, so 'off' will not delete it - remove it by hand"
    fi
    echo "$PACK_NAME is not installed in $game (no $STATE) - nothing to undo"
    return 0
  fi
  local name rel
  name="$(cat "$STATE/pack" 2>/dev/null || printf '%s' "$PACK_NAME")"
  echo "== removing $(wc -l < "$STATE/added.txt" 2>/dev/null || echo 0) file(s) this tool copied into modloader/$name"
  if [ -d "$game/modloader/$name" ]; then rm -rf "$game/modloader/$name"; fi
  if [ -f "$STATE/backups.txt" ]; then
    while IFS= read -r rel; do
      [ -n "$rel" ] || continue
      [ -f "$STATE/backups/$rel" ] || { echo "   WARN: no backup for $rel - left as is"; continue; }
      cp -p "$STATE/backups/$rel" "$game/$rel"
      echo "   restored $rel"
    done < "$STATE/backups.txt"
  fi
  rm -rf "$STATE"
  echo "state dir removed: $game is back to its pre-'on' bytes."
}

# --------------------------------------------------------------------- status -
do_status() {
  echo "game dir : $game"
  echo "pack src : $pack"
  echo "install  : $DEST"
  if [ -d "$STATE" ]; then
    local s; s="$(cat "$STATE/step" 2>/dev/null || echo '')"
    case "$s" in ''|*[!0-9]*) ;; *) step="$s" ;; esac   # report the withheld list for the step actually used
    echo "installed: yes, by this tool at --step ${s:-?} (source $(cat "$STATE/packsrc" 2>/dev/null || echo '?'))"
    echo "  files copied : $(wc -l < "$STATE/added.txt" 2>/dev/null || echo 0)"
    echo "  files withheld:"; withheld | sed 's/^/    /'
    echo "  backups kept : $(wc -l < "$STATE/backups.txt" 2>/dev/null || echo 0)"
    sed 's/^/    /' "$STATE/backups.txt" 2>/dev/null
  elif [ -d "$DEST" ]; then
    echo "installed: yes, but NOT by this tool (no $STATE) - 'off' will not touch it"
  else
    echo "installed: no"
  fi
  echo "steps: 1 drop the pack's procobj/vegepart .ide | 2 streaming caps | 3 rtx.conf opacity micromap | 4 also drop the 2 heaviest .txd (test only; default $STEP_DEFAULT)"
  fix_state
  laa_line
}

case "$action" in
  on)     do_on ;;
  off)    do_off ;;
  status) do_status ;;
esac
