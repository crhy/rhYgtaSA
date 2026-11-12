#!/usr/bin/env bash
# Install the "GTA San Andreas RTX" desktop launcher for THIS machine.
#
# The repo copy (launcher/gtasa-rtx.desktop) carries the Exec=/Path= of the machine that
# developed it, so it is a template, not a file you can copy. This writes the same entry
# with your paths into ~/.local/share/applications/ - no sudo, nothing outside your home.
#
# Usage: scripts/install-desktop-entry.sh [GAME_DIR]
#          --name NAME        desktop entry name (default: GTA San Andreas RTX)
#          --desktop-dir DIR  where to write (default: ~/.local/share/applications)
#          --remove           remove the entry instead of writing it
#
# The icon still launches launcher/launch-rtx.sh, which builds and installs the current
# rhyremix plugin before starting the game, so a desktop click can never run yesterday's
# code; a failed build or deploy still reaches you as a notification (see rhy_notify in
# launcher/launch-rtx.sh), because a desktop click has no terminal.
set -euo pipefail

HERE="$(cd "$(dirname "$0")/.." && pwd)"
GAME=""
NAME="GTA San Andreas RTX"
DIR="${XDG_DATA_HOME:-$HOME/.local/share}/applications"
REMOVE=0
while [ $# -gt 0 ]; do
  case "$1" in
    --name)       shift; NAME="${1:?--name needs a value}" ;;
    --desktop-dir) shift; DIR="${1:?--desktop-dir needs a value}" ;;
    --remove)     REMOVE=1 ;;
    -h|--help)    sed -n '2,17p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *)            GAME="$1" ;;
  esac
  shift
done

entry="$DIR/gtasa-rtx.desktop"

if [ "$REMOVE" = 1 ]; then
  if [ -f "$entry" ]; then rm -f "$entry"; echo "removed $entry"
  else echo "nothing to remove: no $entry"; fi
  exit 0
fi

[ -x "$HERE/launcher/launch-rtx.sh" ] || {
  echo "no executable launcher at $HERE/launcher/launch-rtx.sh - is this the repo?" >&2; exit 1; }

# The launcher itself falls back to its own folder when it is not in a game folder, so an
# empty GAME_DIR means "the game folder is where launch-rtx.sh lives" (the installed copy).
if [ -z "$GAME" ]; then
  GAME="$HERE"
  echo "no GAME_DIR given: using $GAME (pass your game folder if the game is elsewhere)"
fi
[ -f "$GAME/gta-sa.exe" ] || {
  echo "no gta-sa.exe in $GAME - that is not a GTA SA install; pass the game folder:" >&2
  echo "  scripts/install-desktop-entry.sh /path/to/your/GTASA-RTX" >&2; exit 1; }
# A desktop entry is launched from an arbitrary working directory, so both paths in it
# have to be absolute.
GAME="$(cd "$GAME" && pwd)"

mkdir -p "$DIR"
# Exec points at the launcher with the game folder in the environment: one launcher, one
# game folder, and no path baked into the launcher copy in the game folder.
cat > "$entry" <<EOF
[Desktop Entry]
Type=Application
Name=$NAME
Comment=GTA SA with RTX Remix path tracing (Proton) - builds and installs the current rhyremix plugin first
Exec=env GTASA_RTX_DIR=$GAME RHYS_ASI_REPO=$HERE $HERE/launcher/launch-rtx.sh
Path=$HERE
Icon=applications-games
Terminal=false
Categories=Game;
X-GNOME-UsesNotifications=true
EOF
chmod 644 "$entry"

# Make the entry executable where the desktop environment requires it, and refresh the
# menu cache if the tool is installed. Both are best-effort: a missing desktop-file-utils
# must not fail the install.
chmod +x "$entry" 2>/dev/null || true
command -v update-desktop-database >/dev/null 2>&1 && update-desktop-database "$DIR" 2>/dev/null || true
command -v desktop-file-validate   >/dev/null 2>&1 && desktop-file-validate "$entry" \
  && echo "entry validates" || echo "entry written (desktop-file-validate not run)"

echo "wrote $entry"
echo "  Name : $NAME"
echo "  game : $GAME"
echo "  runs : $HERE/launcher/launch-rtx.sh"
echo "Look for '$NAME' in your applications menu. Undo: scripts/install-desktop-entry.sh --remove"
