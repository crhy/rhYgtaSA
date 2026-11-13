#!/bin/bash
# Launch a GTA San Andreas RTX Remix install with Proton (outside Steam), DLSS via NVAPI.
# Usage: GTASA_RTX_DIR=/path/to/GTASA-RTX ./launch-rtx.sh   (defaults to this script's directory)
#
# It always builds and installs the current rhYgtaSA plugin first, so a desktop click can
# never run yesterday's code. Environment knobs, all optional:
#   GTASA_RTX_NO_DEPLOY=1   skip the build+install (run whatever is in the game folder)
#   RHYS_DRY_RUN=1          do the deploy and the state report, then stop before the game
#   RHYS_WATER=<variant>    install that water variant (tools/water.sh) before launch and record it
#                           as an explicit choice: remix-ref|soft|realistic|calm|pack. With RHYS_WATER
#                           UNSET the launcher makes sure the DEFAULT variant (n13: remix-ref) is
#                           installed - a no-op when it already is, and it never overwrites a variant
#                           you picked on purpose (that choice lives in <game>/.rhys-water/state).
#   RHYS_RTX_KNOBS=1        add the measured rtx.conf lines that are still missing (backs up first)
#   RHY_SKIP_VRAM_CHECK=1   launch even with the card full (see the preflight below)
#   RHYS_SHOT_WATCH=0       do not start the F12 screenshot watcher
# n09b (the Quit hang: a gta-sa.exe / NvRemixBridge.exe that outlive their window):
#   RHYS_EXIT_GRACE=N       seconds a game may live without its window/bridge before the launcher
#                           ends the whole wine tree (default 20; 0 = in-run watchdog off, the
#                           after-the-game cleanup still runs)
#   RHYS_EXIT_POLL=N        watchdog poll interval in seconds (default 2)
#   RHYS_WINDOW_PROBE=CMD   command that exits 0 while the game's window exists (default: wmctrl -l
#                           with shot.sh's title filter; skipped when there is no DISPLAY/wmctrl)
#   RHYS_BRIDGE_TIMEOUTS=1  ask .trex/bridge.conf to keep the bridge's IPC timeouts on (backs the
#                           file up first; the runtime overrides this, it is an A/B switch)
R="${GTASA_RTX_DIR:-$(cd "$(dirname "$0")" && pwd)}"
[ -f "$R/gta-sa.exe" ] || R="/media/rhy/data/Gamez/GTASA-RTX"  # script not living in the game folder
# Selftest guard, same shape as the one in apply.sh/deploy.sh: scripts/water-default-selftest.sh
# exports RHYREMIX_INSTALLER_SANDBOX=1 and puts a marker in its scratch install. Without this the
# fallback above means a bug in that test would point the launcher - which installs water variants
# and edits rtx.conf - at a REAL game folder.
if [ "${RHYREMIX_INSTALLER_SANDBOX:-0}" = 1 ] && [ ! -f "$R/.fake-install-marker" ]; then
  echo "refusing to run against $R: sandbox mode needs $R/.fake-install-marker"; exit 1
fi
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
WVAR=""
if [ -f "$REPO/tools/water.sh" ]; then
  # 'installed variant:' is the label water.sh prints; n13 added 'installed by exact bytes', which is
  # the one that cannot be fooled by an edited variant (the sha256 match, not a texture-name grep).
  WSTATUS="$(bash "$REPO/tools/water.sh" status "$R" 2>/dev/null || true)"
  WDESC="$(sed -n 's/^  installed variant: //p' <<<"$WSTATUS" | head -1)"
  echo "water  : ${WDESC:-not found (no Water_materials.usda in this install?)}"
  WVAR="$(sed -n 's/^  installed by exact bytes  : //p' <<<"$WSTATUS" | head -1)"
  [ -n "$WVAR" ] || WVAR="$WDESC"
  grep -q 'translucentMaterial.normalIntensity' "$R/rtx.conf" \
    || echo "water  : rtx.conf has no rtx.translucentMaterial.normalIntensity - the water step below writes it (with the value the live variant needs) and backs the file up in $R/.rhys-water/"
  if [ -n "${RHYS_WATER:-}" ]; then
    echo "water  : RHYS_WATER=$RHYS_WATER is set -> installing it and recording it as an explicit choice"
    RHYS_WHY="launcher RHYS_WATER=$RHYS_WATER" bash "$REPO/tools/water.sh" "$RHYS_WATER" "$R" \
      || echo "WARN: water variant not installed"
  else
    echo "water  : RHYS_WATER unset -> making sure the default variant is installed (n13: remix-ref)."
    echo "         No-op when it is already live; a variant recorded in .rhys-water/state as an"
    echo "         explicit choice is never overwritten (tools/water.sh default $R --force does)."
    RHYS_WHY="launcher default (n13)" bash "$REPO/tools/water.sh" default "$R" \
      || echo "WARN: the default water variant was not installed"
  fi
fi
# RHYS_RTX_KNOBS=1 predates n13: it used to be the only way the measured water line ever reached
# rtx.conf. tools/water.sh now installs that line - and the rest of the animated-water block - with
# the value the live variant needs, so this only fires when the key is still absent. It uses the
# variant-aware value too: 0.35 only fits the pack's own 44 deg sheet, a regenerated sheet
# (soft, remix-ref) needs 1.0 because the map already carries the softening.
if [ "${RHYS_RTX_KNOBS:-0}" = 1 ] && [ -f "$R/rtx.conf" ] \
   && ! grep -q 'translucentMaterial.normalIntensity' "$R/rtx.conf"; then
  knob=1.0
  case " ${WVAR:-} " in *" realistic "*|*" calm "*) knob=0.35 ;; esac
  cp -n "$R/rtx.conf" "$R/rtx.conf.bak-before-rhy-knobs"
  { echo '# rhygtaSA s02/n13: the pack water normal sheet has a 44-46 deg median facet tilt (measured),';
    echo '# which reads as binary black/white under path tracing. 0.35 brings it to ~12 deg; a variant';
    echo '# with a regenerated sheet (soft, remix-ref) needs 1.0 - the map carries the softening.';
    echo "rtx.translucentMaterial.normalIntensity = $knob"; } >> "$R/rtx.conf"
  echo "rtx.conf: added rtx.translucentMaterial.normalIntensity = $knob (live variant: ${WVAR:-unknown}; backup: rtx.conf.bak-before-rhy-knobs)"
fi

# What the bridge was configured with, reported every run (n09b). The bridge server turns its own
# IPC timeouts OFF the moment the game window stops being the foreground window, and its exit
# callback only fires when the client PROCESS exits - so a client that neither exits nor sends
# anything is waited for forever (docs/FINDINGS.md, "the game hung on Quit").
if [ -f "$R/.trex/bridge.conf" ]; then
  echo "bridge : .trex/bridge.conf: $(grep -vE '^\s*(#|$)' "$R/.trex/bridge.conf" | tr '\n' ' ')"
  if grep -qiE '^\s*disableTimeouts\s*=\s*False' "$R/.trex/bridge.conf"; then
    echo "bridge : disableTimeouts = False is set (the bridge should keep timing out on a dead client)"
  else
    echo "bridge : no 'disableTimeouts = False': when the window goes inactive the bridge disables"
    echo "         its own timeouts and waits for the client forever (RHYS_BRIDGE_TIMEOUTS=1 asks"
    echo "         for it; the runtime overrides it, see docs/FINDINGS.md)"
  fi
fi
# RHYS_BRIDGE_TIMEOUTS=1: ask the bridge to keep its IPC timeouts on. Backed up, appended, and
# honest about what the runtime's own documentation says (bridge.defaults.conf, disableTimeouts /
# infiniteRetries: "this setting may be changed dynamically at runtime based on application state,
# therefore changing the default in the settings does not have much of an effect overall"). It is
# the A/B switch for the Quit hang, not the fix - the fix is the cleanup block below.
if [ "${RHYS_BRIDGE_TIMEOUTS:-0}" = 1 ] && [ -f "$R/.trex/bridge.conf" ] \
   && ! grep -qiE '^\s*disableTimeouts\s*=' "$R/.trex/bridge.conf"; then
  cp -n "$R/.trex/bridge.conf" "$R/.trex/bridge.conf.bak-before-rhy-timeouts"
  cat >> "$R/.trex/bridge.conf" <<'CONF'
# rhygtaSA n09b: keep the bridge's client-liveness timeouts on. On 2026-11-12 11:51 the last line
# NvRemixBridge.exe ever wrote was "Client window became inactive, disabling timeouts for bridge
# server...", and it then sat at 406 % CPU for 16 minutes because nothing was timing out any more.
# bridge.defaults.conf says the runtime flips this itself when the window goes inactive, so this may
# not stick - the launcher's own cleanup is what actually ends such a run.
disableTimeouts = False
infiniteRetries = False
CONF
  echo "bridge : added disableTimeouts/infiniteRetries = False to .trex/bridge.conf (backup: .trex/bridge.conf.bak-before-rhy-timeouts)"
fi

if [ "${RHYS_DRY_RUN:-0}" = 1 ]; then
  echo "RHYS_DRY_RUN=1: everything above was applied, the game was NOT started."
  exit 0
fi

# --- n09b: end the run even when the game refuses to end ---------------------------------
# Two hangs, same shape, both started from THIS script (build/tmp/exit-hang-20261112.txt and
# -b.txt):
#   10:39 run - rhY quit from the menu. NvRemixBridge.exe exited, the window disappeared, and
#             gta-sa.exe spun at 66 % CPU for two minutes.
#   11:48 run - same Quit, but this time the bridge stayed too: gta-sa.exe 18 % CPU and
#             NvRemixBridge.exe 406 % CPU (two geometry-proces threads and two main threads at
#             100 % each) holding 3.6 GB of VRAM for SIXTEEN MINUTES, and the last line the bridge
#             ever wrote was "Client window became inactive, disabling timeouts for bridge
#             server..." - the bridge's own client-liveness timeout was switched off by the window
#             going inactive, so it never noticed the client had left. Its exit callback only fires
#             when the client PROCESS exits, and the client process never exits: that is the hang.
# `waitforexitandrun` waits for gta-sa.exe, so while either process spins this script never returns
# and nothing cleans up: the card stays full, the save file stays locked, and (through
# scripts/test-session.sh) the assistant stays dead. So the launcher ends the tree itself:
# NvRemixBridge.exe, then Proton's `wineserver -k` for THIS prefix - which is what takes
# gta-sa.exe with it - and it watches for the state while the game runs, not only after.
#
# RHYS_EXIT_GRACE=0 turns the in-run watchdog off; the after-the-game cleanup always runs.

# --- n09b cleanup (BEGIN): scripts/launch-cleanup-selftest.sh sources exactly this block. ---
EXIT_GRACE="${RHYS_EXIT_GRACE:-20}"       # seconds a game may live without its window/bridge
EXIT_POLL="${RHYS_EXIT_POLL:-2}"
EXIT_TERM_WAIT="${RHYS_EXIT_TERM_WAIT:-5}"
WIN_PROBE="${RHYS_WINDOW_PROBE:-}"        # command that exits 0 while the game's window exists
WINE_PFX="${STEAM_COMPAT_DATA_PATH:-$HOME/.local/share/gtasa-rtx-pfx}"; WINE_PFX="${WINE_PFX%/}/pfx"   # Proton's prefix is <compatdata>/pfx
PROTON_DIR="$(dirname "${PROTON:-/nonexistent-proton/proton}")"

clog() { printf '[cleanup] %s\n' "$*"; }   # stdout is tee'd into $RUNLOG, so this is logged twice

# is_alive <pid>: false for a zombie. `kill -0` succeeds on a zombie and it never goes away on its
# own (only its parent reaps it), so a SIGTERM that worked would otherwise look like a hang here.
is_alive() {
  local line st
  [ -r "/proc/$1/stat" ] || return 1
  line=""; read -r line 2>/dev/null < "/proc/$1/stat" || return 1
  line="${line##*) }"          # the state is the first field after "(comm)", and comm may contain ')'
  st="${line%% *}"
  [ "$st" != Z ]
}

# pids_named <exact name>: comm (truncated at 15 chars, so NvRemixBridge.e) or argv[0]'s basename.
# By PID, never pkill -f: this script's own command line contains every path it knows about.
# No forks: `read` is a builtin and the redirect is done by the shell, because this scans /proc every
# EXIT_POLL seconds for the whole game session - one `cat`/`tr` per process per poll would burn a
# core next to the game.
pids_named() {
  local want="$1" d pid comm a0
  for d in /proc/[0-9]*; do
    pid="${d#/proc/}"
    [ "$pid" = "$$" ] && continue
    comm=""; read -r comm 2>/dev/null < "$d/comm" || continue
    case "$comm" in "$want"|"${want:0:15}") is_alive "$pid" && printf '%s\n' "$pid"; continue ;; esac
    a0=""; IFS= read -r -d '' a0 2>/dev/null < "$d/cmdline"
    case "${a0##*/}" in "$want") is_alive "$pid" && printf '%s\n' "$pid" ;; esac
  done
}

# window_state -> 0 present, 1 gone, 2 cannot tell. wmctrl lists minimised windows, so "gone" means
# destroyed - which is what Quit does, and what an alt-tab does NOT do.
window_state() {
  if [ -n "$WIN_PROBE" ]; then
    bash -c "$WIN_PROBE" >/dev/null 2>&1 && return 0
    return 1
  fi
  [ -n "${DISPLAY:-}" ] || return 2
  command -v wmctrl >/dev/null 2>&1 || return 2
  wmctrl -l 2>/dev/null | tr -d '\r' | grep -qiE 'gta|grand theft auto' && return 0
  return 1
}

# pids_prefix_wineserver: the wineserver of THIS prefix only - other prefixes belong to other games.
# An argv entry whose basename is wineserver, and an argv entry naming this prefix. Same no-fork rule
# as pids_named.
pids_prefix_wineserver() {
  local d pid e is_ws=0 has_pfx=0
  for d in /proc/[0-9]*; do
    pid="${d#/proc/}"
    [ "$pid" = "$$" ] && continue
    is_ws=0; has_pfx=0
    while IFS= read -r -d '' e; do
      case "${e##*/}" in wineserver) is_ws=1 ;; esac
      case "$e" in *"${WINE_PFX%/pfx}"*) has_pfx=1 ;; esac
      [ "$is_ws" = 1 ] && [ "$has_pfx" = 1 ] && break
    done 2>/dev/null < "$d/cmdline"
    [ "$is_ws" = 1 ] && [ "$has_pfx" = 1 ] && printf '%s\n' "$pid"
  done
}

find_wineserver() {
  local c
  for c in "$PROTON_DIR/files/bin/wineserver" "$PROTON_DIR/files/wineserver"; do
    [ -x "$c" ] && { printf '%s\n' "$c"; return 0; }
  done
  c="$(command -v wineserver 2>/dev/null)"
  [ -n "$c" ] && printf '%s\n' "$c"
  return 0
}

stop_named() {   # <label> <pid...>: SIGTERM, EXIT_TERM_WAIT, then SIGKILL the rest
  local label="$1"; shift
  [ $# -gt 0 ] || return 0
  clog "$label: SIGTERM to pid(s) $*"
  kill -TERM "$@" 2>/dev/null
  local waited=0 alive=() p
  while [ "$waited" -lt "$EXIT_TERM_WAIT" ]; do
    alive=()
    for p in "$@"; do is_alive "$p" && alive+=("$p"); done
    [ ${#alive[@]} -eq 0 ] && { clog "$label: stopped"; return 0; }
    sleep 1; waited=$((waited + 1))
  done
  clog "$label: still running after ${EXIT_TERM_WAIT}s (pids ${alive[*]}) - SIGKILL"
  kill -KILL "${alive[@]}" 2>/dev/null
  waited=0
  while [ "$waited" -lt 10 ]; do
    alive=()
    for p in "$@"; do is_alive "$p" && alive+=("$p"); done
    [ ${#alive[@]} -eq 0 ] && { clog "$label: killed"; return 0; }
    sleep 1; waited=$((waited + 1))
  done
  clog "$label: WARNING pids ${alive[*]} are still there (uninterruptible?)"
  return 1
}

# save_exit_evidence <reason>: the same shape as build/tmp/exit-hang-20261112*.txt, which on
# 2026-11-12 both times had to be collected by hand while the processes were still spinning.
save_exit_evidence() {
  local reason="$1" f p lg
  f="$R/logs/exit-hang-$(date +%Y%m%d-%H%M%S).txt"
  {
    date
    echo "reason: $reason"
    echo
    echo "== threads (TID STAT %CPU WCHAN COMMAND) =="
    printf '  TID STAT %%CPU WCHAN                          COMMAND\n'
    for p in $(pids_named gta-sa.exe) $(pids_named NvRemixBridge.exe); do
      ps -T -p "$p" -o tid=,stat=,%cpu=,wchan:34=,comm= 2>/dev/null | sed 's/^/ /'
    done
    for lg in "$R/scripts/rhyremix.log" "$R/moonloader/moonloader.log" \
              "$R/rtx-remix/logs/remix-dxvk.log" "$R/rtx-remix/logs/bridge64.log"; do
      echo
      echo "== last 25 lines of $lg =="
      if [ -f "$lg" ]; then tail -n 25 "$lg"; else echo "(not present)"; fi
    done
  } > "$f" 2>/dev/null
  clog "evidence written to $f"
}

# end_wine_tree <reason>: the bridge first (it is the one holding the VRAM and the CPU), then
# `wineserver -k` for THIS prefix, which is the supported way to end every process in it - the
# spinning gta-sa.exe included. The -k is only issued when a wineserver for this prefix is actually
# running: invoking wineserver with no server up would start one just to kill it. Then a sweep by
# name, in case Proton's wineserver was not found or did not take.
end_wine_tree() {
  local reason="$1" bp gp ws wp
  bp="$(pids_named NvRemixBridge.exe)"; gp="$(pids_named gta-sa.exe)"; wp="$(pids_prefix_wineserver)"
  if [ -z "$bp$gp$wp" ]; then
    clog "$reason: nothing to end (no NvRemixBridge.exe, no gta-sa.exe, no wineserver for $WINE_PFX)"
  else
    clog "$reason: ending the run - NvRemixBridge.exe ${bp:-none}, gta-sa.exe ${gp:-none}, wineserver ${wp:-none}"
  fi
  [ -n "$bp" ] && stop_named "NvRemixBridge.exe" $bp
  ws="$(find_wineserver)"
  if [ -n "$wp" ] && [ -n "$ws" ]; then
    clog "$reason: WINEPREFIX=$WINE_PFX '$ws' -k (this is what takes gta-sa.exe with it)"
    WINEPREFIX="$WINE_PFX" "$ws" -k 2>&1 | sed 's/^/[cleanup] wineserver: /'
  elif [ -n "$wp" ]; then
    clog "$reason: no wineserver binary under $PROTON_DIR/files - signalling the running one directly"
  else
    clog "$reason: no wineserver for $WINE_PFX is running - nothing to shut down in the prefix"
  fi
  gp="$(pids_named gta-sa.exe)"; bp="$(pids_named NvRemixBridge.exe)"; wp="$(pids_prefix_wineserver)"
  [ -n "$gp" ] && stop_named "gta-sa.exe" $gp
  [ -n "$bp" ] && stop_named "NvRemixBridge.exe" $bp
  [ -n "$wp" ] && stop_named "wineserver of $WINE_PFX" $wp
  # What the bridge itself said about the client it is waiting for - the line that tells the next
  # reader whether the bridge's own timeout was off (2026-11-12 11:51) or never armed at all.
  if [ -f "$R/rtx-remix/logs/bridge64.log" ]; then
    clog "bridge64.log last line: $(tail -n 1 "$R/rtx-remix/logs/bridge64.log" 2>/dev/null)"
  fi
}

# watch_run_end: the in-run half. Two signals, each armed only after its own "the game is up" was
# seen, so a slow start or a bridge that never came up (the full-VRAM case of 2026-11-11) is not a
# hang:
#   * the game's window is destroyed (wmctrl; armed once the window has been seen) - this is the
#     Quit case, and it is the only one that catches a game that is spinning with its bridge alive;
#   * gta-sa.exe is gone while NvRemixBridge.exe is still running - the 10:39 case, seen from here.
# With no DISPLAY/wmctrl the first signal cannot be measured; the after-the-game cleanup below still
# runs whenever the launcher's own wait returns.
watch_run_end() {
  local seen_window=0 over_since=0 win gp bp over waited reason
  while :; do
    gp="$(pids_named gta-sa.exe)"; bp="$(pids_named NvRemixBridge.exe)"
    window_state; win=$?
    [ "$win" = 0 ] && seen_window=1
    over=0; reason=""
    if [ "$seen_window" = 1 ] && [ "$win" = 1 ]; then
      over=1; reason="the game's window is gone"
    elif [ -n "$bp" ] && [ -z "$gp" ]; then
      over=1; reason="gta-sa.exe exited but NvRemixBridge.exe is still running"
    fi
    if [ "$over" = 1 ]; then
      if [ "$over_since" = 0 ]; then
        over_since=$(date +%s)
        clog "watch: $reason - ending the run in ${EXIT_GRACE}s unless it comes back"
      fi
      waited=$(( $(date +%s) - over_since ))
      if [ "$waited" -ge "$EXIT_GRACE" ]; then
        clog "watch: $reason for ${waited}s: this is the n09 exit hang, ending it from outside"
        save_exit_evidence "$reason after ${waited}s"
        end_wine_tree "watch: $reason after ${waited}s"
        return 0
      fi
    else
      over_since=0
    fi
    sleep "$EXIT_POLL"
  done
}
# --- n09b cleanup (END) -------------------------------------------------------------------

RUNWATCH=""
if [ "$EXIT_GRACE" != 0 ]; then
  clog "watch: a gta-sa.exe still running ${EXIT_GRACE}s after its window is destroyed, or outliving its NvRemixBridge.exe, gets logged, evidenced and killed (RHYS_EXIT_GRACE=0 turns this off)"
  watch_run_end &
  RUNWATCH=$!
fi

cd "$R" && "$RUNTIME" --verb=waitforexitandrun -- "$PROTON" waitforexitandrun ./gta-sa.exe
rc=$?
[ -n "$RUNWATCH" ] && { kill "$RUNWATCH" 2>/dev/null; wait "$RUNWATCH" 2>/dev/null; }
[ -n "$WATCHER" ] && kill "$WATCHER" 2>/dev/null
end_wine_tree "the launcher's wait returned (gta-sa.exe rc=$rc)"
exit "$rc"
