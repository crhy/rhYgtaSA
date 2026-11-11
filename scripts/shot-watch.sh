#!/usr/bin/env bash
# shot-watch.sh - the outside half of the in-game F12 screenshot hotkey.
#
#   moonloader/SARemix_Shot.lua (in game)  writes  <game>/screenshots/pending/rhyshot_N.req
#   this loop (outside game)             grabs the composited window for each marker
#
# The grab goes over X11, so it never enters the game's PrintScreen path (the
# one that kills the Remix bridge) and it captures the image after Remix's tone
# mapping - which is what you want when judging brightness or whether a lamp
# casts. launcher/launch-rtx.sh starts and stops this around the game.
#
# Usage: scripts/shot-watch.sh [--game DIR] [--interval 0.2]
set -euo pipefail

here="$(cd "$(dirname "$0")/.." && pwd)"
game="${GTASA_RTX_DIR:-/media/rhy/data/Gamez/GTASA-RTX}"
interval=0.2

while [ $# -gt 0 ]; do
  case "$1" in
    --game) game="$2"; shift 2 ;;
    --interval) interval="$2"; shift 2 ;;
    -h|--help) sed -n '2,14p' "$0"; exit 0 ;;
    *) echo "unknown: $1" >&2; exit 2 ;;
  esac
done

pending="$game/screenshots/pending"
shots="$game/screenshots/shots"
mkdir -p "$pending" "$shots"

if [ -z "${DISPLAY:-}" ]; then
  echo "shot-watch: no DISPLAY, the F12 hotkey will have nothing to grab" >&2
  exit 1
fi

echo "shot-watch: watching $pending (press F12 in game)"
while :; do
  for f in "$pending"/*.req; do
    [ -e "$f" ] || continue
    out="$shots/shot_$(date +%Y-%m-%d_%H-%M-%S).png"
    if "$here/scripts/shot.sh" --out "$out"; then
      echo "shot-watch: $out"
    else
      echo "shot-watch: capture failed (window off screen?)" >&2
    fi
    rm -f "$f"
  done
  sleep "$interval"
done
