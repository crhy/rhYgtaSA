#!/usr/bin/env bash
# n09b: stub test for the exit-hang cleanup inside launcher/launch-rtx.sh.
#
#   bash scripts/launch-cleanup-selftest.sh [--keep]
#
# Why this exists: on 2026-11-12 the game hung twice after Quit. The second time (11:48, started
# straight from launcher/launch-rtx.sh) BOTH gta-sa.exe (18 % CPU) and NvRemixBridge.exe (406 % CPU,
# 3.6 GB of VRAM) kept running for sixteen minutes, and the launcher - which waits for gta-sa.exe -
# never returned, so nothing cleaned up and the evidence had to be collected by hand. The launcher
# now ends the wine tree itself: NvRemixBridge.exe, then Proton's `wineserver -k` for this prefix,
# and it watches for the state while the game runs instead of only after it.
#
# The launcher is not runnable here (it deploys the plugin, checks the GPU and starts Proton), so
# this test sources exactly the block between the "n09b cleanup (BEGIN)" and "(END)" markers and
# drives those functions against stub processes: fake gta-sa.exe / NvRemixBridge.exe (copies of
# /bin/sleep, so comm matches the way the real wine processes' comm does) and a fake
# files/bin/wineserver that logs its arguments instead of killing anything.
#
# Cases: A the cleanup ends both processes and calls wineserver -k with the right WINEPREFIX;
#        B nothing running is not an error;
#        C the watchdog fires when the window is destroyed (the Quit case) and writes evidence;
#        D a game whose window is alive is NEVER touched (the false-positive case: alt-tab, pause);
#        E no window probe at all: cannot tell, so do nothing;
#        F the game exited but the bridge did not (the 10:39 case) - fires without any window probe.
#
# Nothing here touches the game folder, wine, the GPU or Strata. Ends "ALL PASS" or "N FAILED".
set -uo pipefail
REPO="$(cd "$(dirname "$0")/.." && pwd)"
ROOT="$REPO/build/tmp/n09b-cleanup"
OUT="$REPO/build/tmp/launch-cleanup-selftest"
KEEP=0
for a in "$@"; do case "$a" in --keep) KEEP=1 ;; *) echo "usage: $0 [--keep]"; exit 2 ;; esac; done

rm -rf "$ROOT" "$OUT"
mkdir -p "$ROOT/game/logs" "$ROOT/game/scripts" "$ROOT/game/moonloader" \
  "$ROOT/game/rtx-remix/logs" "$ROOT/proton/files/bin" "$ROOT/pfx" "$OUT"
pass=0; fail=0
ok()  { pass=$((pass+1)); echo "PASS  $*"; }
bad() { fail=$((fail+1)); echo "FAIL  $*"; }
chk() { local rc=$1; shift; if [ "$rc" = 0 ]; then ok "$*"; else bad "$*"; fi; }
alive() { kill -0 "$1" 2>/dev/null; }
# gone <pid>: a zombie counts as gone. The stubs are children of this script, so after the cleanup
correctly kills one its /proc entry can outlive it until the shell reaps it - `kill -0` and a bare
`[ -e /proc/$pid ]` both say "still there" for a process that is dead.
gone() {
  local line st
  [ -r "/proc/$1/stat" ] || return 0
  line=""; read -r line < "/proc/$1/stat" || return 0
  line="${line##*) }"; st="${line%% *}"
  [ "$st" = Z ]
}
deada() { [ ! -e "/proc/$1" ] || gone "$1"; }
grepq() { grep -q -e "$1" "$2" 2>/dev/null; }

# --------------------------------------------------------------- the block ---
BLOCK="$OUT/cleanup-block.sh"
if ! sed -n '/# --- n09b cleanup (BEGIN)/,/# --- n09b cleanup (END)/p' \
       "$REPO/launcher/launch-rtx.sh" > "$BLOCK"; then
  echo "cannot extract the n09b cleanup block from launcher/launch-rtx.sh"; exit 1
fi
[ -s "$BLOCK" ] || { echo "the n09b cleanup block is empty"; exit 1; }
grepq "end_wine_tree()" "$BLOCK" || { echo "no end_wine_tree() in the block"; exit 1; }
grepq "watch_run_end()" "$BLOCK" || { echo "no watch_run_end() in the block"; exit 1; }

# --------------------------------------------------------------- the stubs ---
for n in gta-sa.exe NvRemixBridge.exe; do cp /bin/sleep "$ROOT/game/$n"; done
# a wineserver that is merely running (the real one is killed by `wineserver -k`, which this stub
# binary only logs), and the binary the launcher is expected to call.
printf '#!/usr/bin/env bash\nwhile :; do sleep 1; done\n' > "$ROOT/pfx/wineserver"
chmod +x "$ROOT/pfx/wineserver"
cat > "$ROOT/proton/files/bin/wineserver" <<EOF
#!/usr/bin/env bash
echo "wineserver \$* WINEPREFIX=\${WINEPREFIX:-unset}" >> "$OUT/wineserver.log"
exit 0
EOF
chmod +x "$ROOT/proton/files/bin/wineserver"

# the logs save_exit_evidence tails, with the 2026-11-12 lines in them
printf '[2026-11-12 11:51:10.018] remix: live=401 create_fail=0 draw_fail=0\n' > "$ROOT/game/scripts/rhyremix.log"
printf '[11:51:10.521] info:  Client window became inactive, disabling timeouts for bridge server...\n' \
  > "$ROOT/game/rtx-remix/logs/bridge64.log"
printf '[11:48:41.760] warn:  [RTX Opacity Micromap] Instance requires more texel taps\n' \
  > "$ROOT/game/rtx-remix/logs/remix-dxvk.log"

# Every stub process we start is recorded so cleanup can never lose one.
: > "$OUT/all.pids"
# A previous run that was interrupted leaves its stubs behind (the binary is deleted, the process is
# not). Kill only processes whose command line names THIS stub tree - never anything by name, which
# could be a game someone is actually playing.
for d in /proc/[0-9]*; do
  p="${d#/proc/}"; [ "$p" = "$$" ] && continue
  raw="$( { tr '\0' ' ' < "$d/cmdline"; } 2>/dev/null )"
  case "$raw" in *"$ROOT/game/"*) kill -9 "$p" 2>/dev/null ;; esac
done
start_game() {   # <secs> -> gta-sa.exe and NvRemixBridge.exe pids on stdout
  # >/dev/null matters: a background child that keeps the command substitution's stdout open would
  # keep $(start_game) waiting for the game, which is exactly the launcher bug being tested here.
  "$ROOT/game/gta-sa.exe" "${1:-600}" >/dev/null 2>&1 & echo $! >> "$OUT/all.pids"; GM=$!
  "$ROOT/game/NvRemixBridge.exe" "${1:-600}" >/dev/null 2>&1 & echo $! >> "$OUT/all.pids"; BR=$!
  printf '%s %s\n' "$GM" "$BR"
}
start_wineserver() {   # a running wineserver that names THIS prefix (argv carries the path)
  "$ROOT/pfx/wineserver" -p "$ROOT/pfx/pfx" 600 >/dev/null 2>&1 & echo $! >> "$OUT/all.pids"
  printf '%s\n' "$!"
}
clear_pids() {
  # End the previous case's stubs first: a fake gta-sa.exe left running would be seen as a live game
  # by the next case (that is exactly the signal the watchdog acts on).
  kill $(cat "$OUT/all.pids" 2>/dev/null) 2>/dev/null
  sleep 1
  kill -9 $(cat "$OUT/all.pids" 2>/dev/null) 2>/dev/null
  : > "$OUT/all.pids"; : > "$OUT/wineserver.log"; rm -f "$OUT/window"; rm -rf "$OUT/hang"
  rm -f "$ROOT"/game/logs/exit-hang-*.txt
}

# load_case <grace> <probe>: source the block with this case's knobs (the block reads RHYS_* from
# the environment, exactly as launcher/launch-rtx.sh does when a user sets them)
load_case() {
  export RHYS_EXIT_GRACE="$1" RHYS_EXIT_POLL=1 RHYS_EXIT_TERM_WAIT=2 RHYS_WINDOW_PROBE="$2"
  source "$BLOCK"
}
# The block's own variables, pointed at the stub tree instead of a real install.
point_at_stubs() {
  R="$ROOT/game"
  PROTON="$ROOT/proton/proton"
  PROTON_DIR="$ROOT/proton"
  WINE_PFX="$ROOT/pfx/pfx"
}

# ------------------------------------------------------- A: the cleanup itself --
clear_pids
echo "== case A: end_wine_tree ends the bridge and kills the prefix"
read -r GM BR <<<"$(start_game 600)"
WS="$(start_wineserver)"
load_case 20 "test -f $OUT/window"; point_at_stubs
end_wine_tree "test: the launcher's wait returned" > "$OUT/caseA.log" 2>&1
sleep 1
chk "$(deada $GM && echo 0 || echo 1)" "A: gta-sa.exe is gone"
chk "$(deada $BR && echo 0 || echo 1)" "A: NvRemixBridge.exe is gone"
chk "$(deada $WS && echo 0 || echo 1)" "A: the prefix's wineserver is gone too"
grepq "wineserver -k WINEPREFIX=$ROOT/pfx/pfx" "$OUT/wineserver.log"; chk $? "A: Proton's wineserver was called as '-k' with WINEPREFIX=<compatdata>/pfx"
grepq "ending the run - NvRemixBridge.exe $BR, gta-sa.exe $GM, wineserver $WS" "$OUT/caseA.log"; chk $? "A: the log names all three"
grepq "SIGTERM to pid(s) $BR" "$OUT/caseA.log"; chk $? "A: the bridge was ended first"
grepq "bridge64.log last line: .*disabling timeouts" "$OUT/caseA.log"; chk $? "A: and quotes the bridge's own last line"

# ------------------------------------------------------- B: nothing to end -----
clear_pids
echo "== case B: end_wine_tree with nothing running"
load_case 20 "test -f $OUT/window"; point_at_stubs
end_wine_tree "test: clean exit" > "$OUT/caseB.log" 2>&1; rcB=$?
chk "$([ $rcB = 0 ] && echo 0 || echo 1)" "B: exits 0 (rc=$rcB)"
grepq "nothing to end" "$OUT/caseB.log"; chk $? "B: says there was nothing to end"
chk "$([ ! -s "$OUT/wineserver.log" ] && echo 0 || echo 1)" "B: it did NOT run 'wineserver -k' with no server up (that would start one just to kill it)"
grepq "no wineserver for $ROOT/pfx/pfx is running" "$OUT/caseB.log"; chk $? "B: and says so"

# ------------------------------------- C: the watchdog, window destroyed -------
clear_pids; : > "$OUT/window"
echo "== case C: the game outlives its window (the Quit hang)"
read -r GM BR <<<"$(start_game 600)"
WS="$(start_wineserver)"
load_case 3 "test -f $OUT/window"; point_at_stubs
watch_run_end > "$OUT/caseC.log" 2>&1 &
WD=$!
sleep 2          # the watchdog must have seen the window up before it may use that signal
rm -f "$OUT/window"
wait $WD 2>/dev/null; rcC=$?
sleep 1
chk "$([ $rcC = 0 ] && echo 0 || echo 1)" "C: the watchdog ended its own loop (rc=$rcC)"
grepq "the game's window is gone" "$OUT/caseC.log"; chk $? "C: it says the window is gone"
grepq "n09 exit hang" "$OUT/caseC.log"; chk $? "C: and calls it the exit hang"
chk "$(deada $GM && echo 0 || echo 1)" "C: the windowless gta-sa.exe was killed"
chk "$(deada $BR && echo 0 || echo 1)" "C: and its NvRemixBridge.exe too"
chk "$(deada $WS && echo 0 || echo 1)" "C: and the prefix's wineserver"
chk "$([ -z "$(pids_named gta-sa.exe)$(pids_named NvRemixBridge.exe)" ] && echo 0 || echo 1)" \
    "C: a killed process is not counted as a running game (zombie: pids_named still sees its comm)"
grepq "wineserver -k WINEPREFIX=$ROOT/pfx/pfx" "$OUT/wineserver.log"; chk $? "C: with 'wineserver -k'"
ef=$(ls "$ROOT"/game/logs/exit-hang-*.txt 2>/dev/null | head -1)
chk "$([ -n "$ef" ] && echo 0 || echo 1)" "C: evidence was written (${ef:-none})"
if [ -n "$ef" ]; then
  grepq "== threads" "$ef"; chk $? "C: the evidence has the thread table"
  grepq "rhyremix.log" "$ef"; chk $? "C: it tails the plugin log"
  grepq "disabling timeouts for bridge server" "$ef"; chk $? "C: and the bridge log line that explains the hang"
fi

# --------------------------------- D: a live window is never a hang ------------
clear_pids; : > "$OUT/window"
echo "== case D: the window stays (alt-tab / pause) - the watchdog must not fire"
read -r GM BR <<<"$(start_game 600)"
load_case 2 "test -f $OUT/window"; point_at_stubs
watch_run_end > "$OUT/caseD.log" 2>&1 &
WD=$!
sleep 8
kill $WD 2>/dev/null; wait $WD 2>/dev/null
chk "$(alive $GM && echo 0 || echo 1)" "D: a game with a window was left running (grace was 2s)"
chk "$(alive $BR && echo 0 || echo 1)" "D: and its bridge too"
chk "$([ ! -s "$OUT/wineserver.log" ] && echo 0 || echo 1)" "D: wineserver was never called"
chk "$([ -z "$(ls "$ROOT"/game/logs/exit-hang-*.txt 2>/dev/null)" ] && echo 0 || echo 1)" "D: no evidence file was written"

# --------------------------- E: no window probe at all -> cannot tell ----------
clear_pids
echo "== case E: no DISPLAY, no wmctrl, no probe: do not guess"
read -r GM BR <<<"$(start_game 600)"
load_case 2 ""; point_at_stubs
DISPLAY= watch_run_end > "$OUT/caseE.log" 2>&1 &
WD=$!
sleep 8
kill $WD 2>/dev/null; wait $WD 2>/dev/null
chk "$(alive $GM && echo 0 || echo 1)" "E: an unmeasurable window never fires the watchdog"
chk "$(alive $BR && echo 0 || echo 1)" "E: nothing was killed"

# ------------------------- F: the bridge outlives the game ---------------------
clear_pids
echo "== case F: gta-sa.exe exits, NvRemixBridge.exe does not (the 10:39 case)"
"$ROOT/game/gta-sa.exe" 2 >/dev/null 2>&1 & GM=$!; echo $GM >> "$OUT/all.pids"
"$ROOT/game/NvRemixBridge.exe" 600 >/dev/null 2>&1 & BR=$!; echo $BR >> "$OUT/all.pids"
WS="$(start_wineserver)"
wait $GM 2>/dev/null      # reap it: a zombie still has a /proc entry, and pids_named would still
                          # call it a running game - which is exactly what the launcher must not do
load_case 3 ""; point_at_stubs
DISPLAY= watch_run_end > "$OUT/caseF.log" 2>&1 &
WD=$!
wait $WD 2>/dev/null; rcF=$?
sleep 1
chk "$([ $rcF = 0 ] && echo 0 || echo 1)" "F: the watchdog ended its own loop (rc=$rcF)"
grepq "gta-sa.exe exited but NvRemixBridge.exe is still running" "$OUT/caseF.log"; chk $? "F: it names the surviving bridge"
chk "$(deada $BR && echo 0 || echo 1)" "F: the leftover bridge was killed"
grepq "wineserver -k" "$OUT/wineserver.log"; chk $? "F: and the prefix was shut down"

# ------------------------------------------------------------------ cleanup ---
kill $(cat "$OUT/all.pids" 2>/dev/null) 2>/dev/null
sleep 1
kill -9 $(cat "$OUT/all.pids" 2>/dev/null) 2>/dev/null
wait 2>/dev/null
[ "$KEEP" = 1 ] || rm -rf "$ROOT"

echo
echo "$pass checks passed, $fail failed"
[ "$fail" = 0 ] && echo "ALL PASS" || echo "$fail FAILED"
exit $((fail > 0))
