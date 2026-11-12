#!/usr/bin/env bash
# s15: stub test for scripts/test-session.sh.
#
#   bash scripts/test-session-selftest.sh [--keep]
#
# test-session.sh stops the Strata server/engine, waits for the GPU, runs the launcher and
# restarts Strata. Testing that for real would take this machine's assistant down mid-task and
# boot a 32-bit Windows game, so everything here is fake and lives under build/tmp/s15-stubs/:
# a fake serve/server.py, engine/strata and memory-watchdog process, a strata-pi-send decoy,
# fake gta-sa.exe / NvRemixBridge.exe / wineserver leftovers, a fake nvidia-smi whose answer
# changes between calls, and a starter that only logs its argv.
#
# The run is additionally wrapped in the script's own sandbox guard
# (RHYS_TEST_SESSION_SANDBOX=<stub dir>), so even a bug in the matching could not signal a
# process outside the stub tree; the last-but-one case proves the guard bites. The final check
# proves the REAL Strata server and engine are still alive after every case.
#
# Nothing here touches the game folder, wine, the GPU or Strata. Ends "ALL PASS" or "N FAILED".
set -uo pipefail
REPO="$(cd "$(dirname "$0")/.." && pwd)"
ROOT="$REPO/build/tmp/s15-stubs"
OUT="$REPO/build/tmp/test-session-selftest"
KEEP=0
for a in "$@"; do case "$a" in --keep) KEEP=1 ;; *) echo "usage: $0 [--keep]"; exit 2 ;; esac; done

rm -rf "$ROOT" "$OUT" "$REPO/build/tmp/s15-outside"
mkdir -p "$ROOT"/bin/serve "$ROOT"/bin/engine "$ROOT"/game/screenshots \
  "$ROOT"/pfx "$ROOT"/other-pfx "$ROOT"/proj "$OUT"
pass=0; fail=0
ok()  { pass=$((pass+1)); echo "PASS  $*"; }
bad() { fail=$((fail+1)); echo "FAIL  $*"; }
chk() { local rc=$1; shift; if [ "$rc" = 0 ]; then ok "$*"; else bad "$*"; fi; }
alive() { kill -0 "$1" 2>/dev/null; }
dead()  { ! alive "$1"; }
grepq() { grep -q -e "$1" "$2" 2>/dev/null; }

# ------------------------------------------------------------------- stubs --
# A fake long-running process. It must NOT exec, so its own path stays in /proc/<pid>/cmdline.
idle() { printf '#!/usr/bin/env bash\nwhile :; do sleep 1; done\n' > "$1"; chmod +x "$1"; }
idle "$ROOT/bin/serve/server.py"
idle "$ROOT/bin/engine/strata"
idle "$ROOT/bin/memory-watchdog"
idle "$ROOT/bin/strata-pi-send"                    # must survive: the assistant's own helper
mkdir -p "$REPO/build/tmp/s15-outside/serve"
idle "$REPO/build/tmp/s15-outside/serve/server.py" # outside the sandbox root: must survive

# The leftovers carry the exact process names the script kills. gta-sa.exe and
# NvRemixBridge.exe are real binaries (a copy of sleep) so both the argv[0] and the comm
# lookup are exercised; wineserver has to carry the prefix in its command line, which sleep
# would choke on, so that one is a script (its argv[0] is the interpreter - the "any entry's
# basename" rule is what finds it, exactly as with a wine-launched binary).
for n in gta-sa.exe NvRemixBridge.exe; do cp /bin/sleep "$ROOT/game/$n"; done
idle "$ROOT/pfx/wineserver"
idle "$ROOT/other-pfx/wineserver"

# fake nvidia-smi: the card is full on the first query and free on every later one, so the
# wait loop has to poll again; the counter file makes that observable.
cat > "$ROOT/bin/nvidia-smi" <<EOF
#!/usr/bin/env bash
n=\$(( \$(cat "$OUT/smi-count" 2>/dev/null || echo 0) + 1 )); echo \$n > "$OUT/smi-count"
case "\${1:-}" in
  --query-gpu=memory.used*) if [ "\$n" -le 1 ]; then echo 15828; else echo 900; fi ;;
  --query-compute-apps=*)   echo "4242, 15494 MiB" ;;
  *) echo "" ;;
esac
EOF
cat > "$ROOT/bin/nvidia-smi-full" <<EOF
#!/usr/bin/env bash
case "\${1:-}" in
  --query-gpu=memory.used*) echo 15828 ;;
  --query-compute-apps=*)   echo "4242, 15494 MiB" ;;
  *) echo "" ;;
esac
EOF
cat > "$ROOT/bin/starter" <<EOF
#!/usr/bin/env bash
echo "starter \$*" >> "$OUT/starter.log"
EOF
chmod +x "$ROOT/bin/nvidia-smi" "$ROOT/bin/nvidia-smi-full" "$ROOT/bin/starter"

# the launcher stub: logs that it ran, leaves the usual mess behind, exits after 2 s
cat > "$ROOT/launcher" <<EOF
#!/usr/bin/env bash
echo "launcher \$*" >> "$OUT/launcher.log"
"$ROOT/game/gta-sa.exe" 600 &          echo \$! >> "$OUT/leftover.pids"; echo \$! >> "$OUT/all.pids"
"$ROOT/game/NvRemixBridge.exe" 600 &   echo \$! >> "$OUT/leftover.pids"; echo \$! >> "$OUT/all.pids"
"$ROOT/pfx/wineserver" -p "$ROOT/pfx" & echo \$! >> "$OUT/leftover.pids"; echo \$! >> "$OUT/all.pids"
"$ROOT/other-pfx/wineserver" -p "$ROOT/other-pfx" & echo \$! >> "$OUT/other.pids"; echo \$! >> "$OUT/all.pids"
sleep 2
exit 0
EOF
# a game that exits immediately (the crash-in-the-first-seconds case)
printf '#!/usr/bin/env bash\necho "launcher-fast $*" >> "%s/launcher.log"\nexit 0\n' "$OUT" > "$ROOT/launcher-fast"
# a launcher that fails: its exit code must come through and Strata must still come back
printf '#!/usr/bin/env bash\necho "launcher-fail $*" >> "%s/launcher.log"\nexit 3\n' "$OUT" > "$ROOT/launcher-fail"
chmod +x "$ROOT"/launcher "$ROOT"/launcher-fast "$ROOT"/launcher-fail

# ------------------------------------------------------ the real Strata, first --
# Read-only: remember what is really running so the last check can prove none of it died.
real_strata_pids() {
  local d pid e raw
  for d in /proc/[0-9]*; do
    pid="${d#/proc/}"
    raw="$( { tr '\0' '\n' < "$d/cmdline"; } 2>/dev/null )" || continue
    while IFS= read -r e; do
      case "$e" in */serve/server.py|*/engine/strata) printf '%s\n' "$pid"; break ;; esac
    done <<< "$raw"
  done
}
REAL_PIDS="$(real_strata_pids)"
echo "== real Strata pids before the run: ${REAL_PIDS//$'\n'/ } (all must be alive afterwards)"

# every stub we start goes in here, so cleanup can never lose one
: > "$OUT/all.pids"
start_stub() { "$1" & local p=$!; echo "$p" >> "$OUT/all.pids"; echo "$p" >> "$OUT/stub.pids"; }
clear_pids() { : > "$OUT/stub.pids"; : > "$OUT/leftover.pids"; : > "$OUT/other.pids"; }

# run_case <log> <launcher> <nvidia-smi> <args...> -> prints the exit code
run_case() {
  local log="$1" launcher="$2" smi="$3"; shift 3
  RHYS_TEST_SESSION_SANDBOX="$ROOT" \
  RHYS_STRATA_STARTER="$ROOT/bin/starter" \
  RHYS_LAUNCHER="$launcher" \
  RHYS_GAME_DIR="$ROOT/game" \
  RHYS_PREFIX="$ROOT/pfx" \
  RHYS_NVIDIA_SMI="$smi" \
  RHYS_VRAM_WAIT=20 RHYS_STOP_WAIT=5 \
    timeout 120 bash "$REPO/scripts/test-session.sh" "$@" > "$OUT/$log" 2>&1
  echo $?
}

# ------------------------------------------------------- case A: normal path ----
# Strata up, --tour, a game that runs 2 s and leaves four leftovers behind.
clear_pids; : > "$OUT/stub.pids"; : > "$OUT/starter.log"; : > "$OUT/launcher.log"; : > "$OUT/smi-count"
start_stub "$ROOT/bin/serve/server.py";   SRV=$(tail -1 "$OUT/stub.pids")
start_stub "$ROOT/bin/engine/strata";     ENG=$(tail -1 "$OUT/stub.pids")
start_stub "$ROOT/bin/memory-watchdog";   WD=$(tail -1 "$OUT/stub.pids")
start_stub "$ROOT/bin/strata-pi-send";    PISEND=$(tail -1 "$OUT/stub.pids")
start_stub "$REPO/build/tmp/s15-outside/serve/server.py"; OUTSIDE=$(tail -1 "$OUT/stub.pids")
echo "== case A: normal run with --tour (server=$SRV engine=$ENG watchdog=$WD decoy=$PISEND outside=$OUTSIDE)"
rcA=$(run_case caseA.log "$ROOT/launcher" "$ROOT/bin/nvidia-smi" --tour --project "$ROOT/proj")
sleep 1
chk "$([ "$rcA" = 0 ] && echo 0 || echo 1)" "A: exits 0 when the launcher exits 0 (rc=$rcA)"
if dead "$SRV" && dead "$ENG"; then ok "A: the fake server AND engine are gone"; else bad "A: the fake server AND engine are gone"; fi
chk "$(dead "$WD" && echo 0 || echo 1)" "A: the starter's memory-watchdog is gone too"
chk "$(alive "$PISEND" && echo 0 || echo 1)" "A: the strata-pi-send decoy survived (the needles are path-shaped, not 'strata')"
chk "$(alive "$OUTSIDE" && echo 0 || echo 1)" "A: a serve/server.py outside the sandbox root survived"
chk "$([ "$(cat "$OUT/smi-count" 2>/dev/null || echo 0)" -ge 2 ] && echo 0 || echo 1)" "A: nvidia-smi was polled again until the card was free ($(cat "$OUT/smi-count" 2>/dev/null || echo 0) calls)"
grepq "900 MiB used" "$OUT/caseA.log"; chk $? "A: the log shows the wait ending at 900 MiB"
[ -f "$ROOT/game/screenshots/tour.req" ]; chk $? "A: --tour created game/screenshots/tour.req"
grepq "launcher " "$OUT/launcher.log"; chk $? "A: the launcher stub ran"
grepq "exited with 0" "$OUT/caseA.log"; chk $? "A: the script waited for the launcher in the foreground"
lk=0; while read -r p; do [ -n "$p" ] || continue; alive "$p" && { echo "   still alive: $p ($(cat /proc/$p/comm 2>/dev/null))"; lk=1; }; done < "$OUT/leftover.pids"
chk "$([ "$lk" = 0 ] && echo 0 || echo 1)" "A: gta-sa.exe, NvRemixBridge.exe and this prefix's wineserver were killed"
lk=0; while read -r p; do [ -n "$p" ] || continue; alive "$p" || { echo "   killed although it is another prefix: $p"; lk=1; }; done < "$OUT/other.pids"
chk "$([ "$lk" = 0 ] && echo 0 || echo 1)" "A: the wineserver of a DIFFERENT prefix was left alone"
grepq "--project $ROOT/proj" "$OUT/starter.log"; chk $? "A: Strata was restarted for the same project"
grepq "--no-dialog" "$OUT/starter.log"; chk $? "A: with --no-dialog, as the starter needs"

# ------------------------------------------- case B: the game exits at once -----
clear_pids; rm -f "$ROOT/game/screenshots/tour.req"
: > "$OUT/starter.log"; : > "$OUT/launcher.log"; : > "$OUT/smi-count"
start_stub "$ROOT/bin/serve/server.py"; SRV=$(tail -1 "$OUT/stub.pids")
start_stub "$ROOT/bin/engine/strata";   ENG=$(tail -1 "$OUT/stub.pids")
# leftovers of a run that died in its first seconds, already stuck before we start
"$ROOT/game/gta-sa.exe" 600 &           echo $! >  "$OUT/leftover.pids"
"$ROOT/pfx/wineserver" -p "$ROOT/pfx" & echo $! >> "$OUT/leftover.pids"
echo "== case B: the game exits immediately (gta-sa.exe / wineserver already stuck)"
rcB=$(run_case caseB.log "$ROOT/launcher-fast" "$ROOT/bin/nvidia-smi" --project "$ROOT/proj")
sleep 1
chk "$([ "$rcB" = 0 ] && echo 0 || echo 1)" "B: exits 0 (rc=$rcB)"
if dead "$SRV" && dead "$ENG"; then ok "B: Strata stopped even though the game was gone in an instant"; else bad "B: Strata stopped even though the game was gone in an instant"; fi
lk=0; while read -r p; do [ -n "$p" ] || continue; alive "$p" && { echo "   still alive: $p"; lk=1; }; done < "$OUT/leftover.pids"
chk "$([ "$lk" = 0 ] && echo 0 || echo 1)" "B: the leftovers of the dead run were cleaned up"
[ ! -f "$ROOT/game/screenshots/tour.req" ]; chk $? "B: without --tour no tour.req was created"
grepq "--project $ROOT/proj" "$OUT/starter.log"; chk $? "B: Strata restarted again"

# ------------------------------------------- case C: the launcher fails ---------
clear_pids; : > "$OUT/starter.log"; : > "$OUT/stub.pids"
start_stub "$ROOT/bin/serve/server.py"; SRV=$(tail -1 "$OUT/stub.pids")
echo "== case C: the launcher exits 3"
rcC=$(run_case caseC.log "$ROOT/launcher-fail" "$ROOT/bin/nvidia-smi" --project "$ROOT/proj")
sleep 1
chk "$([ "$rcC" = 3 ] && echo 0 || echo 1)" "C: the launcher's exit code is the script's exit code (rc=$rcC)"
grepq "--project $ROOT/proj" "$OUT/starter.log"; chk $? "C: Strata came back even after a failed game run"

# ------------------------------------- case D: the VRAM never frees -> no game --
clear_pids; : > "$OUT/starter.log"; : > "$OUT/launcher.log"; : > "$OUT/stub.pids"
start_stub "$ROOT/bin/serve/server.py"; SRV=$(tail -1 "$OUT/stub.pids")
echo "== case D: nvidia-smi never reports a free card"
rcD=$(run_case caseD.log "$ROOT/launcher" "$ROOT/bin/nvidia-smi-full" --project "$ROOT/proj")
chk "$([ "$rcD" = 1 ] && echo 0 || echo 1)" "D: exits 1 when the card stays full (rc=$rcD)"
chk "$([ ! -s "$OUT/launcher.log" ] && echo 0 || echo 1)" "D: the launcher was NOT run"
grepq "4242" "$OUT/caseD.log"; chk $? "D: the log names what still holds the VRAM"
grepq "--project $ROOT/proj" "$OUT/starter.log"; chk $? "D: Strata was restarted instead of leaving the assistant dead"

# ------------------------------------------------- case E: the sandbox guard ----
clear_pids; : > "$OUT/stub.pids"; : > "$OUT/launcher.log"
start_stub "$ROOT/bin/serve/server.py"; SRV=$(tail -1 "$OUT/stub.pids")
echo "== case E: sandbox guard (a launcher from outside the stub tree)"
RHYS_TEST_SESSION_SANDBOX="$ROOT" RHYS_STRATA_STARTER="$ROOT/bin/starter" \
RHYS_LAUNCHER="$REPO/launcher/launch-rtx.sh" RHYS_GAME_DIR="$ROOT/game" \
RHYS_NVIDIA_SMI="$ROOT/bin/nvidia-smi" \
  timeout 30 bash "$REPO/scripts/test-session.sh" > "$OUT/caseE.log" 2>&1
rcE=$?
chk "$([ "$rcE" != 0 ] && echo 0 || echo 1)" "E: refuses to run with a launcher outside the sandbox root (rc=$rcE)"
grepq "outside it" "$OUT/caseE.log"; chk $? "E: and says why"
chk "$(alive "$SRV" && echo 0 || echo 1)" "E: it stopped nothing while refusing"

# ------------------------------------------------------------------ cleanup ---
kill $(cat "$OUT/all.pids" "$OUT/leftover.pids" "$OUT/other.pids" 2>/dev/null) 2>/dev/null
sleep 1
kill -9 $(cat "$OUT/all.pids" "$OUT/leftover.pids" "$OUT/other.pids" 2>/dev/null) 2>/dev/null
wait 2>/dev/null
rm -rf "$REPO/build/tmp/s15-outside"
[ "$KEEP" = 1 ] || rm -rf "$ROOT"

# ------------------------------------------------------ the real Strata, after -
echo "== the real Strata after every case"
rbad=0
for p in $REAL_PIDS; do
  alive "$p" || { echo "   DEAD: pid $p"; rbad=1; }
done
chk "$([ "$rbad" = 0 ] && echo 0 || echo 1)" "the real Strata server/engine are still running ($(echo "$REAL_PIDS" | wc -w) pids)"

echo
echo "$pass checks passed, $fail failed"
[ "$fail" = 0 ] && echo "ALL PASS" || echo "$fail FAILED"
exit $((fail > 0))
