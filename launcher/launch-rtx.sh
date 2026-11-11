#!/bin/bash
# Launch a GTA San Andreas RTX Remix install with Proton (outside Steam), DLSS via NVAPI.
# Usage: GTASA_RTX_DIR=/path/to/GTASA-RTX ./launch-rtx.sh   (defaults to this script's directory)
# Builds and installs the rhYgtaSA plugin first, so the game always runs the current code
# (GTASA_RTX_NO_DEPLOY=1 skips that).
R="${GTASA_RTX_DIR:-$(cd "$(dirname "$0")" && pwd)}"
[ -f "$R/gta-sa.exe" ] || R="/media/rhy/data/Gamez/GTASA-RTX"  # script not living in the game folder
REPO="${RHYS_ASI_REPO:-/home/rhy/rhYgtaSA}"
if [ "${GTASA_RTX_NO_DEPLOY:-0}" != 1 ] && [ -f "$REPO/deploy.sh" ]; then
  bash "$REPO/deploy.sh" --build --no-launcher "$R" \
    || echo "WARN: plugin deploy failed, starting with whatever is already installed"
fi
STEAM="${STEAM_ROOT:-$HOME/.local/share/Steam}"
PROTON="${PROTON:-$STEAM/steamapps/common/Proton - Experimental/proton}"
RUNTIME="${RUNTIME:-$STEAM/steamapps/common/SteamLinuxRuntime_4/_v2-entry-point}"
export STEAM_COMPAT_DATA_PATH="${STEAM_COMPAT_DATA_PATH:-$HOME/.local/share/gtasa-rtx-pfx}"
export STEAM_COMPAT_CLIENT_INSTALL_PATH="$STEAM"
export SteamAppId=12120 SteamGameId=12120
export PROTON_ENABLE_NVAPI=1                       # DLSS / NGX
export PROTON_PREFER_SDL=1 PROTON_ENABLE_HIDRAW=0  # DualShock: avoids an xinput deadlock with GInput + the Remix bridge
export WINEDLLOVERRIDES="d3d9=n,b"                 # load the Remix bridge d3d9.dll
export DXVK_NVAPI_LOG_LEVEL=info PROTON_LOG=1 PROTON_LOG_DIR="$R/logs"
[ -f "$HOME/.config/MangoHud/wine-gta-sa.conf" ] && export MANGOHUD=1 MANGOHUD_CONFIGFILE="$HOME/.config/MangoHud/wine-gta-sa.conf"
mkdir -p "$STEAM_COMPAT_DATA_PATH" "$R/logs" "$R/screenshots/pending"
# F12 inside the game only writes a marker file (moonloader/SARemix_Shot.lua);
# this watcher is the half that actually grabs the window, because the game's
# own PrintScreen path takes the Remix bridge down.
WATCHER=""
if [ "${RHYS_SHOT_WATCH:-1}" = 1 ] && [ -f "$REPO/scripts/shot-watch.sh" ]; then
  bash "$REPO/scripts/shot-watch.sh" --game "$R" &
  WATCHER=$!
fi
cd "$R" && "$RUNTIME" --verb=waitforexitandrun -- "$PROTON" waitforexitandrun ./gta-sa.exe
rc=$?
[ -n "$WATCHER" ] && kill "$WATCHER" 2>/dev/null
exit "$rc"
