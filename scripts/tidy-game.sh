#!/bin/bash
# Park the parts of the GTASA-RTX install that nothing loads any more in
# <game>/OLD/<stamp>/, keeping the relative path so --restore can put them back.
#
#   scripts/tidy-game.sh              dry run: prints what would move and why
#   scripts/tidy-game.sh --apply      do it
#   scripts/tidy-game.sh --restore    print what the newest OLD/<stamp>/ would give back
#   scripts/tidy-game.sh --restore --apply
#   scripts/tidy-game.sh --restore=2026-11-10-140211 --apply
#                         restore that specific snapshot
#   scripts/tidy-game.sh --repo       also clean this repo's scratch files
#   scripts/tidy-game.sh [GAME_DIR]   another install
#
# Nothing is deleted. OLD/ is inert: MoonLoader only scans moonloader/*.lua,
# ModLoader only scans modloader/<name>/<name>*.asi, and the Remix bridge only
# scans rtx-remix/mods/.
set -euo pipefail

HERE="$(cd "$(dirname "$0")/.." && pwd)"
R="/media/rhy/data/Gamez/GTASA-RTX"
APPLY=0; RESTORE=""; WANT_REPO=0
for a in "$@"; do
  case "$a" in
    --apply) APPLY=1 ;;
    --restore) RESTORE=latest ;;
    --restore=*) RESTORE="${a#--restore=}" ;;
    --repo) WANT_REPO=1 ;;
    *) R="$a" ;;
  esac
done
[ -f "$R/rtx.conf" ] && [ -d "$R/scripts" ] || { echo "not a GTASA-RTX install: $R"; exit 1; }

# Anything under these prefixes is the game, the renderer or our own payload:
# moving it would change what runs, so the script refuses instead.
protected() {
  case "$1" in
    rtx-remix/mods*|rtx-remix/logs*|.trex*|scripts*|data*|models*|audio*|anim*|movies*|text*|ReadMe*|cleo/cleo_*|cleo/CLEO+*|cleo/NewOpcodes*|moonloader/lib*|moonloader/luajit*|moonloader/luarocks*) return 0 ;;
    *) return 1 ;;
  esac
}

# relpath|why.  Every entry was checked against the loader that would use it.
MOVES=(
  "moonloader/SARemiv_LightMan.dat|disabled Lua light manager, replaced by rhyremix.asi [streetlights]"
  "moonloader/SARemix_Sun.dat|disabled Lua sun mod, replaced by rhyremix.asi [sun]/[moon]"
  "moonloader/SARemix_VTK_Vanilla.dat|disabled second VTK variant, SARemix_VTK.lua is the one that loads"
  "modloader/VehFuncs|listed in modloader.ini IgnoreMods, so ModLoader never loads it"
  "rtx-remix/captures|capture from an earlier session; the bridge only loads rtx-remix/mods/"
  "logs/launch-1.out|stale launcher output"
  "logs/launch-2.out|stale launcher output"
  "logs/launch-3.out|stale launcher output"
  "logs/launch-4.out|stale launcher output"
  "cleo.log|stale CLEO+ log, rewritten on the next run"
  "metrics.txt|empty DXVK metrics file"
  "modloader/Improved Streaming/ImprovedStreaming.log|stale ModLoader log"
  "modloader/ImVehFt/ImVehFtFix.log|stale ModLoader log"
  "modloader/MixSets/MixSets.log|stale ModLoader log"
)

# Deliberately NOT moved, printed so the choice is auditable.
KEEP=(
  "moonloader/SARemix_Light_Manager.dat|settings file of the light manager, needed if --lua-on is used"
  "moonloader/SARemix_Real_Sun.dat|settings file of the sun mod, needed if --sun-on is used"
  "moonloader/SARemix_VTK.conf|read by the active SARemix_VTK.lua"
  "moonloader/SARemix|vehicle textures read by the active SARemix_VTK.lua"
  "modloader/SARemix|Remix geometry (inverse_pyramid.col, LOD_cone_*.dff)"
  "modloader/Remix_fix|ini overrides; its MixSets.ini differs from the live one"
  "moonloader/SARemix_HBAO.lua|still a .lua, so MoonLoader loads it (harmless: only mad.draw_text)"
  "moonloader/SARemix_VTK.lua|active vehicle material mod"
  "moonloader/SARemix_Shot.lua|in-game F12 screenshot hotkey (writes a marker, never touches D3D9)"
  "moonloader/SARemix_Shot.conf|key choice for that hotkey"
)

restore() {
  local stamp="$1"
  if [ "$stamp" = latest ]; then
    for d in "$R"/OLD/*/; do
      case "$(basename "$d")" in [0-9][0-9][0-9][0-9]-*) stamp="$(basename "$d")" ;; esac
    done
  fi
  [ -n "$stamp" ] && [ -d "$R/OLD/$stamp" ] || { echo "no OLD/ snapshot in $R"; exit 1; }
  echo "== restoring OLD/$stamp into $R"
  find "$R/OLD/$stamp" -mindepth 1 -type f -print0 |
  while IFS= read -r -d '' f; do
    local rel="${f#"$R/OLD/$stamp/"}"
    [ "$rel" = MANIFEST.tsv ] && continue
    if protected "$rel"; then
      echo "  SKIP (protected) $rel"; continue
    fi
    if [ -e "$R/$rel" ]; then
      echo "  SKIP (already there) $rel"; continue
    fi
    if [ "$APPLY" != 1 ]; then echo "  WOULD RESTORE $rel"; continue; fi
    mkdir -p "$(dirname "$R/$rel")"
    mv "$f" "$R/$rel"
    echo "  RESTORE $rel"
  done
  if [ "$APPLY" != 1 ]; then
    echo "== dry run: nothing moved. Add --apply to restore."
    exit 0
  fi
  find "$R/OLD/$stamp" -depth -type d -empty -delete
  if [ -d "$R/OLD/$stamp" ]; then
    echo "== partial restore: leftovers still in $R/OLD/$stamp"
  else
    echo "== snapshot fully restored and removed"
  fi
}

if [ -n "$RESTORE" ]; then restore "$RESTORE"; exit 0; fi

STAMP="$(date +%Y-%m-%d-%H%M%S)"
OLDROOT="$R/OLD/$STAMP"
moved=0; skipped=0

do_move() {
  local rel="$1" why="$2" size
  if protected "$rel"; then
    echo "  SKIP (protected) $rel"; skipped=$((skipped + 1)); return
  fi
  if [ ! -e "$R/$rel" ]; then
    echo "  skip (absent)      $rel"; skipped=$((skipped + 1)); return
  fi
  # A disabled MoonLoader script is a .dat with no .lua of the same name. If the
  # .lua is back the mod is live again and must not be parked.
  case "$rel" in
    moonloader/*.dat)
      if [ -f "${R}/${rel%.dat}.lua" ]; then
        echo "  SKIP (script is live: ${rel%.dat}.lua) $rel"; skipped=$((skipped + 1)); return
      fi ;;
  esac
  # Only park VehFuncs while ModLoader really ignores it.
  if [ "$rel" = "modloader/VehFuncs" ]; then
    if ! awk '/^\[Profiles\..*\.IgnoreMods\]/{f=1;next} /^\[/{f=0} f && /^VehFuncs/{ok=1} END{exit !ok}' "$R/modloader/modloader.ini"; then
      echo "  SKIP (modloader.ini no longer ignores VehFuncs) $rel"; skipped=$((skipped + 1)); return
    fi
  fi
  # Only park the capture folder while no config points at it.
  if [ "$rel" = "rtx-remix/captures" ]; then
    if grep -qi 'capture' "$R/rtx.conf" "$R/.trex/bridge.conf" 2>/dev/null; then
      echo "  SKIP (a config mentions captures) $rel"; skipped=$((skipped + 1)); return
    fi
  fi
  size=$(du -sh "$R/$rel" 2>/dev/null | cut -f1)
  echo "  MOVE  $rel  ($size) - $why"
  moved=$((moved + 1))
  if [ "$APPLY" = 1 ]; then
    mkdir -p "$(dirname "$OLDROOT/$rel")"
    mv "$R/$rel" "$OLDROOT/$rel"
    printf '%s\t%s\n' "$rel" "$why" >> "$OLDROOT/MANIFEST.tsv"
  fi
}

echo "== $R"
for m in "${MOVES[@]}"; do do_move "${m%%|*}" "${m#*|}"; done
echo
echo "== kept on purpose"
for k in "${KEEP[@]}"; do
  [ -e "$R/${k%%|*}" ] && echo "  KEEP  ${k%%|*} - ${k#*|}"
done
echo
if [ "$APPLY" = 1 ] && [ "$moved" -gt 0 ]; then
  echo "== $moved item(s) moved into $OLDROOT (MANIFEST.tsv lists them)"
  echo "   undo with: scripts/tidy-game.sh --restore"
elif [ "$APPLY" = 1 ]; then
  echo "== nothing to move"
else
  echo "== dry run: $moved item(s) would move, $skipped skipped. Add --apply to do it."
fi

if [ "$WANT_REPO" = 1 ]; then
  echo
  echo "== repo scratch in $HERE"
  for f in build-p01-send.json build-p02a-lamp-list-send.json build-p02b-plugin-streetlights-send.json build-queue.log; do
    [ -f "$HERE/$f" ] || continue
    if [ "$APPLY" = 1 ]; then
      mkdir -p "$HERE/OLD/repo-scratch"
      mv "$HERE/$f" "$HERE/OLD/repo-scratch/"; echo "  MOVE  $f"
    else
      echo "  WOULD MOVE $f"
    fi
  done
  # deploy.sh keeps one backup per changed build; three is enough to bisect with.
  n=0
  for d in "$HERE"/build/tmp/deploy-backup-*/; do
    n=$((n + 1))
    if [ "$n" -le 3 ]; then continue; fi
    if [ "$APPLY" = 1 ]; then rm -rf "$d"; echo "  PRUNE ${d#"$HERE"/}"; else echo "  WOULD PRUNE ${d#"$HERE"/}"; fi
  done
fi
