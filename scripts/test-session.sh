#!/usr/bin/env bash
# One command for rhY's test session: Strata off, game on, Strata back.
#
#   scripts/test-session.sh [--tour] [--project DIR] [--game DIR]
#
# Why this exists: the Strata engine serves this repo's assistant with
# --resident-experts --kv-resident 32768 --max-context 262144 and pins model + KV cache in
# VRAM on purpose - 15,494 of the 4070 Ti SUPER's 16,376 MiB (docs/TESTING.md). RTX Remix
# needs most of the card, so while Strata runs the game renders a white window, never
# reaches remixapi_InitializeLibrary and dies with a CPU exception in the bridge. The
# launcher already refuses to start in that state and names the process; this script is the
# other half: free the card, run the test, give the assistant back.
#
# What it does, in order:
#   1. stop the Strata server, the engine and the starter's memory-watchdog (SIGTERM, then
#      SIGKILL anything left) - the watchdog is stopped too because it is the one thing that
#      could put the model back on the card while the game is loading
#   2. wait until nvidia-smi really reports < 1 GiB used
#   3. --tour: create <game>/screenshots/tour.req so SARemix_Tour.lua runs once
#   4. run launcher/launch-rtx.sh in the FOREGROUND (deploy first, Ctrl-C works, you see
#      the log); its exit code is this script's exit code. While it runs, a watchdog watches
#      for the n09 exit hang: a gta-sa.exe that is still running after its bridge process and/or
#      its window has gone away. That is what happened on 2026-11-12 10:39 - rhY quit from the
#      menu, NvRemixBridge.exe exited and the window disappeared, and gta-sa.exe then spun in
#      user space for two minutes with nothing in any log about it. The watchdog logs it, writes
#      the evidence file (threads + the tails of rhyremix.log / moonloader.log / remix-dxvk.log,
#      the same shape as build/tmp/exit-hang-20261112.txt), kills the game and lets the run end.
#   5. after the game exits: kill leftovers - gta-sa.exe, NvRemixBridge.exe and the wineserver
#      of THIS prefix - so a crashed run cannot hold the card or the save file
#   6. restart Strata with the starter, for the same --project DIR
#
# Killing by PID, not by pkill -f. The word "strata" is everywhere on this machine: the pi
# window runs ~/.local/state/strata-opencode-starter/run/pi___rhYgtaSA___Strata.sh, the
# assistant's own helper is ~/.local/bin/strata-pi-send, and this script's own command line
# contains the starter path as soon as you pass --starter/--project. `pkill -f strata` would
# take the session that is driving this repo down with it. So PIDs are collected from
# /proc/<pid>/cmdline against path-shaped needles (an argv entry ending in /serve/server.py,
# /engine/strata or /memory-watchdog), and this script plus its whole parent chain is skipped
# explicitly.
#
# Test knobs (used by scripts/test-session-selftest.sh; leave them unset for real use):
#   RHYS_STRATA_STARTER   starter to restart Strata with   (default ~/.local/bin/strata-opencode-starter)
#   RHYS_LAUNCHER         launcher to run in the foreground (default launcher/launch-rtx.sh)
#   RHYS_GAME_DIR         game folder (default /media/rhy/data/Gamez/GTASA-RTX)
#   RHYS_PREFIX           wine prefix whose wineserver may be killed
#   RHYS_NVIDIA_SMI       nvidia-smi to poll                 (default: nvidia-smi)
#   RHYS_STRATA_SERVER_MATCH / RHYS_STRATA_ENGINE_MATCH      the two cmdline needles
#   RHYS_VRAM_LIMIT_MIB   free the card below this many MiB used (default 1024)
#   RHYS_VRAM_WAIT        seconds to wait for that            (default 120)
#   RHYS_STOP_WAIT        seconds a SIGTERM gets before SIGKILL (default 25)
#   RHYS_HANG_GRACE       seconds a game may outlive its window/bridge before it is called hung
#                         (default 20; 0 = watchdog off)
#   RHYS_HANG_POLL        watchdog poll interval in seconds (default 2)
#   RHYS_WINDOW_PROBE     command that exits 0 while the game's window exists (default: wmctrl -l
#                         with shot.sh's title filter, skipped when there is no DISPLAY/wmctrl)
#   RHYS_HANG_EVIDENCE    folder for the exit-hang evidence file (default build/tmp)
#   RHYS_TEST_SESSION_SANDBOX=<dir>  guard: refuse to run unless starter/launcher/smi/game
#                                    live under <dir>, and only signal processes whose
#                                    cmdline contains <dir>. A stub test can then never
#                                    touch a real Strata.
set -uo pipefail
REPO="$(cd "$(dirname "$0")/.." && pwd)"

usage() {
  sed -n '2,12p' "$0" | sed 's/^# \{0,1\}//'
  cat <<'EOF'

options:
  --tour            arm the in-game photo tour (touch <game>/screenshots/tour.req)
  --project DIR     project to restart Strata for (default: this repo; also $RHYS_STRATA_PROJECT)
  --game DIR        game folder (default: $RHYS_GAME_DIR or /media/rhy/data/Gamez/GTASA-RTX)
  -h, --help        this text
EOF
}

# ------------------------------------------------------------------ options --
TOUR=0
PROJECT="${RHYS_STRATA_PROJECT:-$REPO}"
GAME="${RHYS_GAME_DIR:-${GTASA_RTX_DIR:-/media/rhy/data/Gamez/GTASA-RTX}}"
while [ $# -gt 0 ]; do
  case "$1" in
    --tour)        TOUR=1 ;;
    --project)     PROJECT="${2:?--project needs a directory}"; shift ;;
    --project=*)   PROJECT="${1#--project=}" ;;
    --game)        GAME="${2:?--game needs a directory}"; shift ;;
    --game=*)      GAME="${1#--game=}" ;;
    -h|--help)     usage; exit 0 ;;
    *) echo "test-session: unknown option '$1'" >&2; usage; exit 2 ;;
  esac
  shift
done

STARTER="${RHYS_STRATA_STARTER:-$HOME/.local/bin/strata-opencode-starter}"
LAUNCHER="${RHYS_LAUNCHER:-$REPO/launcher/launch-rtx.sh}"
PREFIX="${RHYS_PREFIX:-${STEAM_COMPAT_DATA_PATH:-$HOME/.local/share/gtasa-rtx-pfx}}"
SMI="${RHYS_NVIDIA_SMI:-nvidia-smi}"
SERVER_MATCH="${RHYS_STRATA_SERVER_MATCH:-serve/server.py}"
ENGINE_MATCH="${RHYS_STRATA_ENGINE_MATCH:-engine/strata}"
# The starter's own watchdog takes the server's PID as an argument and is the one thing that
# could bring the model back onto the card while the game is starting, so it goes down too.
WATCHDOG_MATCH="${RHYS_STRATA_WATCHDOG_MATCH:-memory-watchdog}"
VRAM_LIMIT_MIB="${RHYS_VRAM_LIMIT_MIB:-1024}"
VRAM_WAIT="${RHYS_VRAM_WAIT:-120}"
STOP_WAIT="${RHYS_STOP_WAIT:-25}"
# n09: the exit-hang watchdog. A game that is still running this long after its bridge process
# and/or its window is gone is not playing any more, it is spinning; on 2026-11-12 that cost two
# minutes of waiting and the evidence had to be collected by hand.
HANG_GRACE="${RHYS_HANG_GRACE:-20}"
HANG_POLL="${RHYS_HANG_POLL:-2}"
WINDOW_PROBE="${RHYS_WINDOW_PROBE:-}"
EVIDENCE_DIR="${RHYS_HANG_EVIDENCE:-$REPO/build/tmp}"
SANDBOX="${RHYS_TEST_SESSION_SANDBOX:-}"

log() { printf '[test-session] %s\n' "$*"; }
die() { log "FATAL: $*"; exit 1; }

# A sandbox run must be fully inside its tree, otherwise a typo in the self-test would
# signal the real Strata (which is what kills this very session).
if [ -n "$SANDBOX" ]; then
  for p in "$STARTER" "$LAUNCHER" "$GAME" "$SMI"; do
    case "$p" in */*) : ;; *) continue ;; esac   # a bare command name (nvidia-smi) is not a path
    case "$p" in
      "$SANDBOX"/*) : ;;
      *) die "RHYS_TEST_SESSION_SANDBOX=$SANDBOX but '$p' is outside it" ;;
    esac
  done
fi
[ -x "$STARTER" ] || die "no starter to bring Strata back with: $STARTER (set RHYS_STRATA_STARTER)"
[ -f "$LAUNCHER" ] || die "no launcher: $LAUNCHER (set RHYS_LAUNCHER)"
command -v "$SMI" >/dev/null 2>&1 || [ -x "$SMI" ] || log "note: '$SMI' not found, the VRAM wait will be skipped"

# ---------------------------------------------------------------- pids by cmdline --
# Never signal ourselves or anything above us: the pi window's wrapper and mate-terminal are
# in this chain, and the wrapper's name contains "Strata".
protected() {   # protected -> newline separated pids we must never signal
  local pid=$$ s np
  while [ -n "$pid" ] && [ "$pid" != 0 ] && [ "$pid" != 1 ]; do
    printf '%s\n' "$pid"
    s="$(cat "/proc/$pid/stat" 2>/dev/null)" || break
    s="${s##*) }"; set -- $s; np="${2:-}"     # after "(comm)" comes: state ppid
    pid="$np"
  done
}
PROTECTED="$(protected)"
is_protected() { grep -qxF "$1" <<<"$PROTECTED"; }

# /proc reads race with processes exiting; the group redirect is what keeps bash's own
# "No such file or directory" out of the log (a plain 2>/dev/null on the command does not).
ARR=()
read_argv() {   # read_argv <pid> -> global ARR (the argv), non-zero if the pid vanished
  ARR=()
  mapfile -d '' -t ARR < <( { cat "/proc/$1/cmdline"; } 2>/dev/null )
  [ ${#ARR[@]} -gt 0 ]
}

# pids_matching <path suffix>: a process matches when one WHOLE argv entry ends in /<suffix>.
# Whole entries, not a substring of the joined command line: on this machine the text of the
# task that asked for this script sits in the command line of the running strata-pi-send
# process ("...its server is serve/server.py + engine/strata..."), so a substring match - or
# pkill -f - would have signalled the assistant's own helper. Requiring a path-like entry
# ("/home/rhy/Strata/serve/server.py", "/home/rhy/Strata/engine/strata") cannot match prose.
pids_matching() {
  local needle="$1" d pid e joined
  for d in /proc/[0-9]*; do
    pid="${d#/proc/}"
    is_protected "$pid" && continue
    read_argv "$pid" || continue
    joined="${ARR[*]}"
    if [ -n "$SANDBOX" ]; then
      case "$joined" in *"$SANDBOX"*) : ;; *) continue ;; esac
    fi
    for e in "${ARR[@]}"; do
      case "$e" in *"/$needle") printf '%s\n' "$pid"; break ;; esac
    done
  done
}

# stop_pids <label> <pid...>: SIGTERM, wait STOP_WAIT, SIGKILL the rest.
stop_pids() {
  local label="$1"; shift
  [ $# -gt 0 ] || { log "$label: nothing running"; return 0; }
  log "$label: SIGTERM to pid(s) $*"
  kill -TERM "$@" 2>/dev/null
  local waited=0 alive=()
  while [ "$waited" -lt "$STOP_WAIT" ]; do
    alive=()
    for p in "$@"; do kill -0 "$p" 2>/dev/null && alive+=("$p"); done
    [ ${#alive[@]} -eq 0 ] && { log "$label: stopped"; return 0; }
    sleep 1; waited=$((waited + 1))
  done
  log "$label: still alive after ${STOP_WAIT}s (pids ${alive[*]}) - SIGKILL"
  kill -KILL "${alive[@]}" 2>/dev/null
  waited=0
  while [ "$waited" -lt 10 ]; do
    alive=()
    for p in "$@"; do kill -0 "$p" 2>/dev/null && alive+=("$p"); done
    [ ${#alive[@]} -eq 0 ] && { log "$label: stopped"; return 0; }
    sleep 1; waited=$((waited + 1))
  done
  log "$label: WARNING pids ${alive[*]} are still there (uninterruptible?)"
  return 1
}

stop_strata() {
  local sp ep wp
  sp="$(pids_matching "$SERVER_MATCH")"
  ep="$(pids_matching "$ENGINE_MATCH")"
  wp="$(pids_matching "$WATCHDOG_MATCH")"
  log "strata: server pids [${sp//$'\n'/ }] engine pids [${ep//$'\n'/ }] watchdog pids [${wp//$'\n'/ }]"
  # server first: it owns the engine child and shuts it down on a clean exit.
  stop_pids "strata server" ${sp:-}
  ep="$(pids_matching "$ENGINE_MATCH")"
  stop_pids "strata engine" ${ep:-}
  wp="$(pids_matching "$WATCHDOG_MATCH")"
  stop_pids "strata watchdog" ${wp:-}
  STOPPED_STRATA=1
}

# ------------------------------------------------------------------- vram ---
vram_used_mib() {   # highest memory.used across GPUs, empty if nvidia-smi cannot answer
  "$SMI" --query-gpu=memory.used --format=csv,noheader,nounits 2>/dev/null \
    | tr -d ' \r' | sort -rn | head -1
}
show_vram_holders() {
  "$SMI" --query-compute-apps=pid,used_memory --format=csv,noheader 2>/dev/null | while IFS=, read -r pid mem; do
    printf '           pid %-8s %-12s %s\n' "$pid" "$mem" "$(ps -o comm= -p "$pid" 2>/dev/null)"
  done
}
wait_for_vram() {
  local used waited=0
  while :; do
    used="$(vram_used_mib)"
    if [ -z "$used" ]; then
      log "vram: '$SMI' gave no answer - continuing, launch-rtx.sh has its own preflight"
      return 0
    fi
    [ "$used" -lt "$VRAM_LIMIT_MIB" ] && { log "vram: ${used} MiB used (< ${VRAM_LIMIT_MIB} MiB) - the card is free"; return 0; }
    [ "$waited" -ge "$VRAM_WAIT" ] && {
      log "vram: still ${used} MiB used after ${VRAM_WAIT}s. These hold it:"
      show_vram_holders
      return 1
    }
    log "vram: ${used} MiB used, waiting (t=${waited}s)"
    sleep 2; waited=$((waited + 2))
  done
}

# --------------------------------------------------------------- leftovers ---
# Exact process names, matched on argv[0]'s basename (comm is truncated to 15 chars, which
# would cut "NvRemixBridge.exe" to "NvRemixBridge.e"). wineserver is only fair game when its
# command line names THIS prefix: other prefixes belong to other games.
GAME_PROCS=(gta-sa.exe NvRemixBridge.exe)
pids_named() {
  local want="$1" d pid comm joined
  for d in /proc/[0-9]*; do
    pid="${d#/proc/}"
    is_protected "$pid" && continue
    read_argv "$pid" || continue
    # Same sandbox rule as pids_matching: a stub test can never signal a real game.
    joined="${ARR[*]}"
    if [ -n "$SANDBOX" ]; then
      case "$joined" in *"$SANDBOX"*) : ;; *) continue ;; esac
    fi
    case "${ARR[0]##*/}" in
      "$want") printf '%s\n' "$pid"; continue ;;
    esac
    # argv[0] is the interpreter for a script and a Windows path for some wine processes,
    # so comm is the second way in. comm is truncated at 15 chars: NvRemixBridge.exe ->
    # NvRemixBridge.e, hence the prefix test.
    comm="$( { cat "/proc/$pid/comm"; } 2>/dev/null )"
    [ -n "$comm" ] || continue
    if [ "$comm" = "$want" ] || [ "$comm" = "${want:0:15}" ]; then
      printf '%s\n' "$pid"
    fi
  done
}
pids_prefix_wineserver() {
  local d pid e joined
  for d in /proc/[0-9]*; do
    pid="${d#/proc/}"
    is_protected "$pid" && continue
    read_argv "$pid" || continue
    joined="${ARR[*]}"
    case "$joined" in *"$PREFIX"*) : ;; *) continue ;; esac
    if [ -n "$SANDBOX" ]; then
      case "$joined" in *"$SANDBOX"*) : ;; *) continue ;; esac
    fi
    # wineserver is only fair game when its command line names THIS prefix: other prefixes
    # belong to other games. Any entry's basename counts, argv[0] is not always it.
    for e in "${ARR[@]}"; do
      case "${e##*/}" in wineserver) printf '%s\n' "$pid"; break ;; esac
    done
  done
}
kill_leftovers() {
  local n pids
  for n in "${GAME_PROCS[@]}"; do
    pids="$(pids_named "$n")"
    stop_pids "leftover $n" ${pids:-}
  done
  pids="$(pids_prefix_wineserver)"
  stop_pids "wineserver of $PREFIX" ${pids:-}
}

# ------------------------------------------------------------- the exit hang --
# n09 (2026-11-12 10:39, .strata/notes-n09-exit-hang.md). The plugin cannot fix the spin itself -
# the game stopped calling Present 340 ms before its own exit path began, so nothing inside the
# process runs again - but everything OUTSIDE it can: notice the game outliving its bridge/window,
# write down what the process was doing at that moment, kill it, and let the run finish so Strata
# comes back. Without this, kill_leftovers below never runs, because the launcher never returns.
#
# n09b: launcher/launch-rtx.sh now watches the same thing itself (and ends the whole wine tree with
# Proton's `wineserver -k`), because the second hang on 2026-11-12 - 11:48, both gta-sa.exe AND
# NvRemixBridge.exe spinning for 16 minutes - was started straight from the launcher, with no
# test-session.sh in front of it. The two watchdogs are independent on purpose: whichever of the two
# entry points is in play, something ends the run, and the one here also covers a launcher that is
# stuck somewhere other than its wait.
#
# "Over" means the bridge process is gone OR the game's window is gone. The watchdog is armed only
# after it has seen the game UP (the bridge alive, or the window present), so the 2026-11-11 case
# where the bridge never started at all (the card was full) is not mistaken for a hang.

bridge_running() { [ -n "$(pids_named NvRemixBridge.exe)" ]; }

# window_state -> 0 present, 1 gone, 2 cannot tell. The default probe reuses shot.sh's title
# filter; with no DISPLAY or no wmctrl it answers "cannot tell", which never fires the watchdog on
# its own (the bridge signal still does).
window_state() {
  if [ -n "$WINDOW_PROBE" ]; then
    if bash -c "$WINDOW_PROBE" >/dev/null 2>&1; then return 0; fi
    return 1
  fi
  [ -n "${DISPLAY:-}" ] || return 2
  command -v wmctrl >/dev/null 2>&1 || return 2
  wmctrl -l 2>/dev/null | tr -d '\r' | grep -qiE 'gta|grand theft auto|gta_sa' && return 0
  return 1
}

# save_exit_hang <pid-list> <waited-seconds> <bridge-state> <window-state>
save_exit_hang() {
  local gpids="$1" waited="$2" bstate="$3" wstate="$4" f p t
  mkdir -p "$EVIDENCE_DIR" 2>/dev/null
  f="$EVIDENCE_DIR/exit-hang-$(date +%Y%m%d-%H%M%S).txt"
  {
    date
    echo "gta-sa.exe still running ${waited}s after its window/bridge went away"
    echo "bridge_running=${bstate} window_state=${wstate} (0 present, 1 gone, 2 unknown)"
    echo
    echo "== threads (TID STAT %CPU WCHAN COMMAND) =="
    printf '  TID STAT %%CPU WCHAN                          COMMAND\n'
    for p in $gpids; do
      ps -T -p "$p" -o tid=,stat=,%cpu=,wchan:34=,comm= 2>/dev/null | sed 's/^/ /'
    done
    echo
    echo "== per-task state (path comm state utime stime) =="
    for p in $gpids; do
      for t in /proc/$p/task/*; do
        [ -d "$t" ] || continue
        printf '%s %s %s %s %s\n' "$t" "$(cat "$t/comm" 2>/dev/null)" \
          "$(awk '{print $3}' "$t/stat" 2>/dev/null)" \
          "$(awk '{print $14}' "$t/stat" 2>/dev/null)" \
          "$(awk '{print $15}' "$t/stat" 2>/dev/null)"
      done
    done
    for lg in "$GAME/scripts/rhyremix.log" "$GAME/moonloader/moonloader.log" \
              "$GAME/rtx-remix/logs/remix-dxvk.log"; do
      echo
      echo "== last 25 lines of ${lg#$REPO/} =="
      if [ -f "$lg" ]; then tail -n 25 "$lg"; else echo "(not present)"; fi
    done
  } > "$f" 2>/dev/null
  log "game: evidence written to $f"
}

# watch_exit_hang: runs as a background subshell around the launcher.
watch_exit_hang() {
  local gpids seen_up=0 over_since=0 bridge win over waited
  while :; do
    gpids="$(pids_named gta-sa.exe)"
    if [ -z "$gpids" ]; then
      # The game is gone: that is an exit, not a hang, whatever the bridge and window do.
      over_since=0
      sleep "$HANG_POLL"
      continue
    fi
    bridge=0; bridge_running && bridge=1
    window_state; win=$?
    if [ "$bridge" = 1 ] || [ "$win" = 0 ]; then
      seen_up=1
    fi
    over=0
    [ "$bridge" = 0 ] && over=1
    [ "$win" = 1 ] && over=1
    if [ "$over" = 1 ] && [ "$seen_up" = 1 ]; then
      [ "$over_since" = 0 ] && over_since=$(date +%s)
      waited=$(( $(date +%s) - over_since ))
      if [ "$waited" -ge "$HANG_GRACE" ]; then
        log "game: STILL RUNNING ${waited}s after its window/bridge went away (bridge=${bridge} window_state=${win}) - the n09 exit hang"
        save_exit_hang "$gpids" "$waited" "$bridge" "$win"
        stop_pids "hung game gta-sa.exe" $gpids
        return 0
      fi
    else
      over_since=0
    fi
    sleep "$HANG_POLL"
  done
}

# ----------------------------------------------------------------- strata ---
STOPPED_STRATA=0
restart_strata() {
  [ "${STRATA_RESTARTED:-0}" = 1 ] && return 0
  log "strata: restarting for project $PROJECT"
  # setsid + nohup: the starter opens the server and the pi window; it must outlive this
  # script and the terminal it was started from.
  if command -v setsid >/dev/null 2>&1; then
    setsid "$STARTER" --project "$PROJECT" --no-dialog >/dev/null 2>&1 </dev/null &
  else
    nohup "$STARTER" --project "$PROJECT" --no-dialog >/dev/null 2>&1 </dev/null &
  fi
  disown 2>/dev/null
  STRATA_RESTARTED=1
}
on_exit() {
  local rc=$?
  [ "$STOPPED_STRATA" = 1 ] || return 0
  [ "${STRATA_RESTARTED:-0}" = 1 ] && return 0
  log "exiting without a restart (rc=$rc) - bringing Strata back anyway"
  restart_strata
}
trap on_exit EXIT

# ------------------------------------------------------------------- run ----
log "plan: stop Strata -> wait for < ${VRAM_LIMIT_MIB} MiB VRAM -> $LAUNCHER -> clean up -> $STARTER --project $PROJECT --no-dialog"
[ "$TOUR" = 1 ] && log "tour: will create $GAME/screenshots/tour.req"
[ -n "$SANDBOX" ] && log "sandbox: only processes and paths under $SANDBOX may be touched"

stop_strata
if ! wait_for_vram; then
  log "the card never came free - the game was NOT started"
  restart_strata
  exit 1
fi

if [ "$TOUR" = 1 ]; then
  [ -d "$GAME" ] || die "--tour: game folder $GAME does not exist"
  mkdir -p "$GAME/screenshots" 2>/dev/null
  : > "$GAME/screenshots/tour.req" || die "cannot write $GAME/screenshots/tour.req"
  log "tour armed"
fi

log "game: running the launcher in the foreground (Ctrl-C stops the game, then Strata comes back)"
# n09: the exit-hang watchdog runs alongside the launcher. It is stopped the moment the launcher
# returns, so it can never signal a leftover after the run - that part stays kill_leftovers' job.
WATCHDOG=""
if [ "$HANG_GRACE" != 0 ]; then
  log "watch: watching for the n09 exit hang (a gta-sa.exe still running ${HANG_GRACE}s after its window/bridge is gone gets logged, evidenced and killed); evidence -> $EVIDENCE_DIR"
  watch_exit_hang &
  WATCHDOG=$!
fi
bash "$LAUNCHER"
garc=$?
if [ -n "$WATCHDOG" ]; then
  kill "$WATCHDOG" 2>/dev/null
  wait "$WATCHDOG" 2>/dev/null
fi
log "game: the launcher exited with $garc"

kill_leftovers
restart_strata
exit "$garc"
