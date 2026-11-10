#!/bin/bash
# Launch a GTA San Andreas RTX Remix install with Proton (outside Steam), DLSS via NVAPI.
# Usage: GTASA_RTX_DIR=/path/to/GTASA-RTX ./launch-rtx.sh   (defaults to this script's directory)
R="${GTASA_RTX_DIR:-$(cd "$(dirname "$0")" && pwd)}"
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
mkdir -p "$STEAM_COMPAT_DATA_PATH" "$R/logs"
cd "$R" && exec "$RUNTIME" --verb=waitforexitandrun -- "$PROTON" waitforexitandrun ./gta-sa.exe
