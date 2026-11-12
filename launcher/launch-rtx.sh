#!/bin/bash
# Launch a GTA San Andreas RTX Remix install with Proton (outside Steam), DLSS via NVAPI.
# Usage: GTASA_RTX_DIR=/path/to/GTASA-RTX ./launch-rtx.sh   (defaults to this script's directory)
#
# It always builds and installs the current rhYgtaSA plugin first, so a desktop click can
# never run yesterday's code. Environment knobs, all optional:
#   GTASA_RTX_NO_DEPLOY=1   skip the build+install (run whatever is in the game folder)
#   RHYS_DRY_RUN=1          do the deploy and the state report, then stop before the game
#   RHYS_WATER=calm|realistic|pack   install that water variant (tools/water.sh) before launch
#   RHYS_RTX_KNOBS=1        add the measured rtx.conf lines that are still missing (backs up first)
#   RHY_SKIP_VRAM_CHECK=1   launch even with the card full (see the preflight below)
#   RHYS_SHOT_WATCH=0       do not start the F12 screenshot watcher
R="${GTASA_RTX_DIR:-$(cd "$(dirname "$0")" && pwd)}"
[ -f "$R/gta-sa.exe" ] || R="/media/rhy/data/Gamez/GTASA-RTX"  # script not living in the game folder
# Where the plugin source lives. apply.sh writes <game>/rhys-asi-repo with the checkout it
# installed from, so an installed game folder finds its own repo; RHYS_ASI_REPO wins over that,
# and the hardcoded default is only the path this repo happened to be developed at.
REPO="${RHYS_ASI_REPO:-/home/rhy/rhYgtaSA}"
if [ ! -f "$REPO/deploy.sh" ] && [ -f "$R/rhys-asi-repo" ]; then
  REPO="$(head -1 "$R/rhys-asi-repo")"
fi

# A desktop click has no terminal, so without this a failed build or a failed deploy is
# invisible: the game just starts with an old plugin. Keep stdout AND keep a log.
if [ "${RHYS_DRY_RUN:-0}" = 1 ]; then
  mkdir -p "$REPO/build/tmp"
  RUNLOG="$REPO/build/tmp/launch-dryrun-$(date +%Y%m%d-%H%M%S).log"
else
  mkdir -p "$R/logs"
  RUNLOG="$R/logs/launch-$(date +%Y%m%d-%H%M%S).log"
fi
exec > >(tee -a "$RUNLOG") 2>&1
echo "launcher: $0"
echo "game dir: $R"
echo "log file: $RUNLOG"

rhy_notify() {   # desktop users cannot see stdout; the VRAM block below already does this
  local msg="$1"
  if command -v zenity >/dev/null 2>&1; then zenity --warning --title="GTA SA RTX" --text="$msg" 2>/dev/null
  elif command -v notify-send >/dev/null 2>&1; then notify-send -u critical "GTA SA RTX" "$msg"; fi
}

if [ "${GTASA_RTX_NO_DEPLOY:-0}" != 1 ] && [ -f "$REPO/deploy.sh" ]; then
  # --prod: the shipped lighting profile when there is no rhyremix.ini yet. No --force-ini:
  # an existing rhyremix.ini (your live A/B knobs) is never overwritten.
  bash "$REPO/deploy.sh" --build --prod --no-launcher "$R" \
    || { echo "WARN: plugin deploy failed, starting with whatever is already installed"
         rhy_notify "rhyremix plugin deploy FAILED - the game will run the previously installed plugin.
See $RUNLOG"; }
elif [ "${GTASA_RTX_NO_DEPLOY:-0}" != 1 ]; then
  # A fresh install has no asi in <game>/scripts/ yet, so without this the game would start
  # with no plugin at all and no explanation.
  echo "WARN: no deploy.sh at $REPO - the plugin was NOT built or installed."
  echo "      Point the launcher at your checkout: RHYS_ASI_REPO=/path/to/rhYgtaSA $0"
  echo "      (or re-run ./apply.sh \"$R\" from the checkout, which writes $R/rhys-asi-repo)"
  rhy_notify "rhyremix plugin not found: the launcher could not locate the rhYgtaSA checkout.
Set RHYS_ASI_REPO=/path/to/rhYgtaSA and start again. See $RUNLOG"
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
              strata*) echo "                 ^ this is the local LLM server (Strata). Its"
                       echo "                   expert cache fills the card on purpose; stop"
                       echo "                   it while you play (context size is not it)." ;;
            esac
          done
      echo "         Free it, or set RHY_SKIP_VRAM_CHECK=1 to launch anyway."
      if [ ! -t 1 ]; then   # started from the desktop: nobody sees stdout
        msg="GTA SA RTX did not start: only ${vram_free} MiB of ${vram_total} MiB VRAM is free.
RTX Remix needs most of the card. Using it now:
$(nvidia-smi --query-compute-apps=pid,used_memory --format=csv,noheader 2>/dev/null | while IFS=, read -r pid mem; do printf '%s %s (pid %s)\n' "$(ps -o comm= -p "$pid" 2>/dev/null)" "$mem" "$pid"; done)
Close it (Strata: stop the server) and start the game again."
        if command -v zenity >/dev/null 2>&1; then zenity --error --title="GTA SA RTX" --text="$msg" 2>/dev/null
        elif command -v notify-send >/dev/null 2>&1; then notify-send -u critical "GTA SA RTX" "$msg"; fi
      fi
      exit 1
    fi
  fi
fi

# F12 inside the game only writes a marker file (moonloader/SARemix_Shot.lua);
# this watcher is the half that actually grabs the window, because the game's
# own PrintScreen path takes the Remix bridge down. Started only now, so an
# early exit above cannot leave it running.
WATCHER=""
if [ "${RHYS_SHOT_WATCH:-1}" = 1 ] && [ -f "$REPO/scripts/shot-watch.sh" ]; then
  bash "$REPO/scripts/shot-watch.sh" --game "$R" &
  WATCHER=$!
fi
# --- what is actually installed right now (reported, only changed if you asked) ---------
# Written because "did my fix reach the game?" was being answered by guessing on 2026-11-11.
if [ -f "$R/scripts/rhyremix.asi" ]; then
  echo "plugin : $(ls -la --time-style=+%F_%T "$R/scripts/rhyremix.asi" | awk '{print $6, $5" bytes"}')"
else
  echo "plugin : MISSING ($R/scripts/rhyremix.asi) - the deploy above failed"
fi
if [ -f "$R/scripts/rhyremix.ini" ]; then
  if grep -q '^source=' "$R/scripts/rhyremix.ini"; then
    echo "traffic: ini sets source=$(sed -n 's/^source=//p' "$R/scripts/rhyremix.ini" | head -1)"
  else
    echo "traffic: ini has no source= key -> plugin default source=game (s01 corona hook)"
  fi
fi
if [ -f "$REPO/tools/water.sh" ]; then
  echo "water  : $(bash "$REPO/tools/water.sh" status "$R" 2>/dev/null | sed -n 's/^installed: //p')"
  grep -q 'translucentMaterial.normalIntensity' "$R/rtx.conf" \
    || echo "water  : rtx.conf has no rtx.translucentMaterial.normalIntensity (s02: 0.35 is the
           measured fix for the black/white sea; RHYS_RTX_KNOBS=1 adds it, or add it by hand)"
fi
if [ -n "${RHYS_WATER:-}" ] && [ -f "$REPO/tools/water.sh" ]; then
  echo "water  : installing variant '$RHYS_WATER'"
  bash "$REPO/tools/water.sh" "$RHYS_WATER" "$R" || echo "WARN: water variant not installed"
fi
if [ "${RHYS_RTX_KNOBS:-0}" = 1 ] && [ -f "$R/rtx.conf" ] \
   && ! grep -q 'translucentMaterial.normalIntensity' "$R/rtx.conf"; then
  cp -n "$R/rtx.conf" "$R/rtx.conf.bak-before-rhy-knobs"
  echo '# rhygtaSA s02: the pack water normal sheet has a 46 deg median facet tilt (measured),
# which reads as binary black/white under path tracing. 0.35 brings it to ~12 deg.' >> "$R/rtx.conf"
  echo 'rtx.translucentMaterial.normalIntensity = 0.35' >> "$R/rtx.conf"
  echo "rtx.conf: added rtx.translucentMaterial.normalIntensity = 0.35 (backup: rtx.conf.bak-before-rhy-knobs)"
fi

if [ "${RHYS_DRY_RUN:-0}" = 1 ]; then
  echo "RHYS_DRY_RUN=1: everything above was applied, the game was NOT started."
  exit 0
fi

cd "$R" && "$RUNTIME" --verb=waitforexitandrun -- "$PROTON" waitforexitandrun ./gta-sa.exe
rc=$?
[ -n "$WATCHER" ] && kill "$WATCHER" 2>/dev/null
exit "$rc"
