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
# Free-VRAM preflight. This exists because of a specific, expensive confusion:
# on 2026-11-11 a local LLM server (Strata, --resident-experts --kv-resident
# 32768) was holding 15,494 of the 4070 Ti SUPER's 16,376 MiB. GTA SA RTX Remix
# then showed a pure white window, never got as far as remixapi_InitializeLibrary,
# and eventually died with a CPU exception in the bridge - and every log looked
# like a lighting bug. Three config rollbacks and four wasted launches later the
# answer was just "the GPU was full". Remix has no way to say that, so say it here.
#
# Override with RHY_SKIP_VRAM_CHECK=1 if you know better than this.
if [ "${RHY_SKIP_VRAM_CHECK:-0}" != 1 ] && command -v nvidia-smi > /dev/null 2>&1; then
  read -r vram_used vram_total <<< "$(nvidia-smi \
      --query-gpu=memory.used,memory.total --format=csv,noheader,nounits 2>/dev/null \
      | head -1 | tr ',' ' ')"
  if [ -n "$vram_total" ]; then
    vram_free=$((vram_total - vram_used))
    # 6 GiB is not a measured minimum; it is a generous floor. Remix's runtime
    # plus the 2174-file GTASA-Remix asset pack at 1080p sits well above 8 GiB,
    # so anything under this has already failed in practice.
    if [ "$vram_free" -lt 6144 ]; then
      echo "WARNING: only ${vram_free} MiB of ${vram_total} MiB VRAM is free."
      echo "         RTX Remix needs most of the card; with less than this the"
      echo "         runtime renders a white window, never reaches"
      echo "         remixapi_InitializeLibrary, and then dies with a CPU"
      echo "         exception in the bridge. Every log looks like a mod bug."
      echo "         These processes hold the VRAM:"
      nvidia-smi --query-compute-apps=pid,used_memory --format=csv,noheader 2>/dev/null \
        | while IFS=, read -r pid mem; do
            name=$(ps -o comm= -p "$pid" 2>/dev/null)
            printf '           pid %-8s %-10s  %s\n' "$pid" "$mem" "$name"
            # The agent serving this project's assistant runs as a local LLM
            # server, and it pins the model plus its KV cache in VRAM on
            # purpose. It cannot be restarted from inside the game, and the
            # assistant cannot test the game while it is itself being served.
            # Naming it here is what stopped three config rollbacks and four
            # wasted launches on 2026-11-11.
            case "$name" in
              strata*) echo "                 ^ this is the local LLM server that is"
                       echo "                   serving the assistant. Stop it, or lower"
                       echo "                   --kv-resident / --max-context, to free VRAM." ;;
            esac
          done
      echo "         Free it, or set RHY_SKIP_VRAM_CHECK=1 to launch anyway."
      exit 1
    fi
  fi
fi

cd "$R" && "$RUNTIME" --verb=waitforexitandrun -- "$PROTON" waitforexitandrun ./gta-sa.exe
rc=$?
[ -n "$WATCHER" ] && kill "$WATCHER" 2>/dev/null
exit "$rc"
