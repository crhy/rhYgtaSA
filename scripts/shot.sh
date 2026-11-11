#!/usr/bin/env bash
# shot.sh - take a screenshot of the running game without touching its own
# PrintScreen path.
#
#   Ctrl+PrintScreen inside GTA SA goes through GDI + D3D9 (CheckDeviceFormat,
#   CreateDeviceResources) and the Remix bridge's server dies in the middle of
#   that sequence (see docs/FINDINGS.md and .strata/notes-p04-brightness-crash.md).
#   Grabbing the composited window over X11 never enters the game at all, and it
#   captures the image *after* Remix's tone mapping - which is exactly what you
#   want when judging "is the world too bright" or "does this lamp cast".
#
# Usage:
#   scripts/shot.sh                 # grab the GTA window into screenshots/
#   scripts/shot.sh --name gta      # window title filter
#   scripts/shot.sh --out /tmp/x.png
#   scripts/shot.sh --watch 5       # grab every 5 s until you Ctrl-C
#   scripts/shot.sh --trigger       # no capture: drop the same marker file the
#                                     in-game F12 hotkey writes, so shot-watch.sh
#                                     grabs it (useful over ssh / from a script)
#
# Needs DISPLAY (an X11 session) and either ImageMagick's `import` or `scrot`.
# The title filter can also match a terminal whose title contains the cwd
# (~/rhYgtaSA contains "gta"); such windows are skipped by window class.
set -euo pipefail

here="$(cd "$(dirname "$0")/.." && pwd)"
out_dir="$here/screenshots"
filter="gta|grand theft auto|gta_sa"
game="${GTASA_RTX_DIR:-/media/rhy/data/Gamez/GTASA-RTX}"
interval=""
explicit=""
trigger=0

while [ $# -gt 0 ]; do
  case "$1" in
    --name) filter="$2"; shift 2 ;;
    --out) explicit="$2"; shift 2 ;;
    --watch) interval="$2"; shift 2 ;;
    --game) game="$2"; shift 2 ;;
    --trigger) trigger=1; shift ;;
    -h|--help) sed -n '2,24p' "$0"; exit 0 ;;
    *) echo "unknown: $1" >&2; exit 2 ;;
  esac
done

if [ "$trigger" = 1 ]; then
  pending="$game/screenshots/pending"
  mkdir -p "$pending"
  marker="$pending/rhyshot_manual_$(date +%s).req"
  : > "$marker" && echo "$marker"
  exit 0
fi

if [ -z "${DISPLAY:-}" ]; then  # (checked after --trigger, which needs no X)
  echo "no DISPLAY in this environment; run this from a terminal inside your desktop session" >&2
  exit 1
fi

# A terminal window whose title is "user@host:~/rhYgtaSA" matches the 'gta' filter
# (see ~/.bashrc's xterm title block), so a title match alone can grab the chat
# instead of the game. Drop windows whose X class is a terminal emulator.
is_terminal_window() {
  local c
  c="$(xprop -id "$1" %c 2>/dev/null)"
  case " $c " in
    *erminal*|*xterm*|*Xterm*|*kitty*|*Kitty*|*alacritty*|*Alacritty*|*foot*|*Foot*|*WeTerm*|*Konsole*|*mintty*|*mintty*) return 0 ;;
  esac
  return 1
}

grab() {
  local id=""
  if command -v wmctrl >/dev/null 2>&1; then
    local wid
    while read -r wid; do
      [ -n "$wid" ] || continue
      if is_terminal_window "$wid"; then
        echo "shot: skipping terminal window $wid (title matches '$filter' by accident)" >&2
        continue
      fi
      id="$wid"   # last non-terminal match wins
    done < <(wmctrl -l 2>/dev/null | tr -d '\r' | grep -iE -- "$filter" | cut -d' ' -f1)
  fi
  if command -v import >/dev/null 2>&1; then
    if [ -n "$id" ]; then import -window "$id" "$1"
    else
      echo "no window matching '$filter'; grabbing the whole screen" >&2
      import -window root "$1"
    fi
  elif command -v scrot >/dev/null 2>&1; then
    scrot -o "$1"
  else
    echo "need ImageMagick (import) or scrot" >&2
    return 1
  fi
}

one() {
  local stamp dest
  stamp="$(date +%Y-%m-%d_%H-%M-%S)"
  if [ -n "$explicit" ] && [ -z "$interval" ]; then dest="$explicit"
  else
    dest="$out_dir/$(basename "${explicit:-game}_$stamp.png")"
  fi
  mkdir -p "$(dirname "$dest")"
  grab "$dest"
  [ -s "$dest" ] || { echo "capture produced an empty file; is the window on screen?" >&2; return 1; }
  echo "$dest"
}

if [ -n "$interval" ]; then
  while :; do one; sleep "$interval"; done
else
  one
fi
