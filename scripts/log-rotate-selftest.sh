#!/usr/bin/env bash
# n03: the logs the tools read are bounded now, and the tools still see everything after a rotation.
#
#   bash scripts/log-rotate-selftest.sh [--keep]
#
# Why this exists (.strata/notes-s26-perf-checklist.md follow-ups 2 and 3, measured in $G on
# 2026-11-14):
#   scripts/rhyremix.log   1,088,220 bytes, appended to on every run since 2026-11-10, never rotated
#                          (the rhyremix.log.prev next to it, 3,378,593 bytes, is hand-made)
#   logs/steam-12120.log   51,958,320 bytes from ONE PROTON_LOG=1 run
# Both are read whole by scripts/diagnose.sh and tools/tour-report.py, so every session made the next
# diagnosis slower. launcher/launch-rtx.sh now:
#   * before the game starts: moves an over-cap log to <name>.prev (the plugin log whole, the Proton
#     trace keeping its tail) and restarts the live file at 0;
#   * after the run: trims the Proton trace IN PLACE, because docs/TESTING.md tells the reader to
#     grep that exact path;
#   * RHYS_PROTON_LOG=0: does not ask Proton for the trace at all.
#
# That creates the failure mode this test is about: a rotation can move the session you are reporting
# on out of the file the tools read. So the test
#   A checks the launcher's shape (the block is extractable, both logs have call sites, and the calls
#     sit where they must: after the RHYS_DRY_RUN exit, before the game starts, trim after it),
#   B/C/D/E drive the rotation and the trim against a fake tree (cap honoured, whole-file move for
#     the plugin log, tail-only for the Proton trace, exactly one .prev, no-op under the cap),
#   F runs the launcher's Proton-log policy block with the knob on and off,
#   G runs scripts/diagnose.sh against the rotated tree and requires EVERY line it claims back, in
#     the right order (a line from the live log must beat an older one from the .prev), and
#   H runs tools/tour-report.py against it and requires all three log categories back under a shot.
#
# Nothing here touches the game folder, wine or the GPU: the fake tree is build/tmp/log-rotate.
# Ends "ALL PASS" or "N FAILED".
set -uo pipefail
REPO="$(cd "$(dirname "$0")/.." && pwd)"
ROOT="$REPO/build/tmp/log-rotate"
FAKE="$ROOT/game"
OUT="$ROOT/out"
KEEP=0
for a in "$@"; do case "$a" in --keep) KEEP=1 ;; *) echo "usage: $0 [--keep]"; exit 2 ;; esac; done

rm -rf "$ROOT"; mkdir -p "$FAKE/scripts" "$FAKE/logs" "$FAKE/rtx-remix/logs" \
  "$FAKE/screenshots/shots" "$OUT"
pass=0; fail=0
ok()  { pass=$((pass+1)); echo "PASS  $*"; }
bad() { fail=$((fail+1)); echo "FAIL  $*"; }
chk() { local rc=$1; shift; if [ "$rc" = 0 ]; then ok "$*"; else bad "$*"; fi; }
sha() { sha256sum "$1" 2>/dev/null | awk '{print $1}'; }
sz()  { stat -c%s "$1" 2>/dev/null || echo 0; }
lineno() { grep -n -- "$1" "$2" 2>/dev/null | head -1 | cut -d: -f1; }

# ------------------------------------------------------------------ the fake log --
# One line per claim scripts/diagnose.sh makes, each carrying a unique n03=<name> marker: the marker
# has to come back out of the TOOL, not just out of the file. Plus filler, so the rotation is real.
LOG="$FAKE/scripts/rhyremix.log"
{
  echo '---- log starts ----'
  # five older summary lines, deliberately FIRST in the file and oldest in time: diagnose.sh's
  # sections print `tail -n N` of the matches, so they also test that the newest matches still win
  # across two files (section G moves this whole file into rhyremix.log.prev).
  for i in 1 2 3 4 5; do
    echo "[2026-11-14 09:05:4$i.000] streetlights: $((290+i)) active, $((290+i)) drawn this frame (hour=21, in_drop=0) | remix: live=$((290+i)) create_fail=0 draw_fail=0 n03=old$i"
  done
  echo '[2026-11-14 09:05:59.100] config: fade in=700ms out=900ms step=0.25 creates/frame=24 player_grace=8000ms sky=3.00x+0.25 per s sun=3.00x 12.0 deg/s phase_fade=250ms n03=fade'
  echo '[2026-11-14 09:06:00.000] streetlights: worst case 408 lamps within max_distance=300 of a point (cap max_lights+sticky_extra=700): the cap covers the radius n03=worstcase'
  echo '[2026-11-14 09:06:01.000] streetlights: 318 active, 318 drawn this frame (hour=21, 7002 in list, kept=310 new=8 steps=14 created=6 retired=4 in_drop=0 player_gone_frames=0) | remix: live=320 create_fail=0 draw_fail=0 budget_stop=0 deferred=37 n03=lights'
  echo '[2026-11-14 09:06:02.000] trafficlights[source=game]: 7 heads lit (7 with a glow), 7 drawn this frame (green=2 amber=1 red=4, colour_swaps=3 expired=2) | remix: live=379 create_fail=0 draw_fail=0 budget_stop=0 | hook: calls=412 records=412 dropped=0 bad=0 reentrant=0 n03=signals'
  echo '[2026-11-14 09:06:03.000] signalhook: patched 0x0049DD4C (was '"'"'call 0x006FC580'"'"') -> stub at 0x78764FC0, cap 256 heads per frame n03=hook'
  echo '[2026-11-14 09:06:04.000] trafficlights: head light ok bulb radius=0.10 flux=67 pos=1040.0,-1392.0,20.1 colour=0.0018,1,0.34 n03=bulb'
  echo '[2026-11-14 09:06:04.100] trafficlights: head light ok glow radius=3.00 flux=304 pos=1040.0,-1392.0,20.1 colour=0.0018,1,0.34 n03=glow'
  echo '[2026-11-14 09:06:05.000] trafficlights: head source = hook: no signal inside the game'"'"'s own corona range, so nothing to record n03=headsource'
  echo '[2026-11-14 09:06:06.000] streetlights: rhyremix.ini rejected [fade] step=0 - keeping the last good value, disabled until the next edit n03=gaveup'
  echo '[2026-11-14 09:06:07.000] probe: sweep radiance x1..x1000 at 1040.0,-1392.0,20.1 scale=0.01 n03=probe'
  echo '[2026-11-14 09:06:08.000] sky: sun elev=+41.3 az=187.9 airMass=1.53 T=0.712/0.584/0.431 daylight=1.00 | moon elev=-12.4 az=95.1 phase=0.42 | lamps=off (clock=13:07 tod=787.35 min ms/min=2013 doy=186 month=0 day=0) | tod: ticks=394 torn=2 jitter=17 reanchor=0 wraps=0 bad=0 | cal: stable=0 accepted=0 rejected=394 | cloud=0.00 | published: sun 4.28,3.51,2.59 sky=1.000 emissive=1.000 rebuilds=12/4 | rtx: set=41 unknown=0 badval=0 n03=sky'
  for i in $(seq 1 60); do
    echo "[2026-11-14 09:06:20.000] filler line $i to push this log over the rotation cap so the rotation is real, not simulated: n03filler=$i"
  done
} > "$LOG"
printf '[streetlights]\nenabled=1\nmax_lights=640\n\n[fade]\nin_ms=700\nout_ms=900\n' > "$FAKE/scripts/rhyremix.ini"
printf 'create ok\nInvalid light handle 0x10\n' > "$FAKE/rtx-remix/logs/bridge64.log"

# the tour: two shots and the manifest lines that name them, so tour-report.py has somewhere to
# print the nearest log lines - the half that would silently go blank after a rotation.
cat > "$FAKE/screenshots/tour-manifest.txt" <<'MAN'
2026-11-14 09:05:55  tour start, 2 stops
2026-11-14 09:06:08  shot 01 00-baseline-noon-grove
2026-11-14 09:06:20  shot 02 06-signals-close-2100
2026-11-14 09:06:22  tour done
MAN
python3 - "$FAKE/screenshots/shots" <<'PY'
import os, sys
from PIL import Image                      # the same dependency tests/test_tour_report.py uses
d = sys.argv[1]
Image.new("RGB", (40, 24), (90, 90, 90)).save(os.path.join(d, "shot_2026-11-14_09-06-08.png"))
Image.new("RGB", (40, 24), (10, 200, 30)).save(os.path.join(d, "shot_2026-11-14_09-06-20.png"))
PY
touch -d "2026-11-14 09:06:08" "$FAKE/screenshots/shots/shot_2026-11-14_09-06-08.png"
touch -d "2026-11-14 09:06:20" "$FAKE/screenshots/shots/shot_2026-11-14_09-06-20.png"

# the Proton log: one run's worth of wine debug, far over the cap. Its LAST line is the marker the
# tail checks look for.
python3 - "$FAKE/logs/steam-12120.log" <<'PY'
import sys
with open(sys.argv[1], "w") as f:
    for i in range(4000):
        f.write("002c:fixme:win:SetLayeredWindowAttributes 0x1002f6 (shape 0) stub: 0x%x\n" % i)
PY

BEFORE_LOG=$(sz "$LOG"); BEFORE_PTON=$(sz "$FAKE/logs/steam-12120.log")
ORIG_LOG_SHA=$(sha "$LOG")

# ------------------------------------------------------------- A: the launcher ---
LAUNCH="$REPO/launcher/launch-rtx.sh"
BLOCK="$OUT/rotate-block.sh"
sed -n '/# --- log bounds (BEGIN)/,/# --- log bounds (END)/p' "$LAUNCH" > "$BLOCK"
[ -s "$BLOCK" ]; chk $? "the log-bounds block is extractable from launcher/launch-rtx.sh"
grep -q 'rhy_rotate_log()' "$BLOCK"; chk $? "the block defines rhy_rotate_log"
grep -q 'rhy_trim_log()' "$BLOCK"; chk $? "the block defines rhy_trim_log"
bash -n "$LAUNCH"; chk $? "launcher/launch-rtx.sh parses"

dry=$(lineno 'RHYS_DRY_RUN=1: everything above was applied' "$LAUNCH")
launch=$(lineno 'waitforexitandrun -- ' "$LAUNCH")
rot1=$(lineno 'rhy_rotate_log "\$R/scripts/rhyremix.log"' "$LAUNCH")
rot2=$(lineno 'rhy_rotate_log "\$R/logs/steam-' "$LAUNCH")
trim=$(lineno 'rhy_trim_log "\$R/logs/steam-' "$LAUNCH")
[ -n "$dry" ] && [ -n "$launch" ] && [ -n "$rot1" ] && [ -n "$rot2" ] && [ -n "$trim" ]
chk $? "both rotate calls and the trim call exist (dry-run exit $dry, rotate $rot1/$rot2, launch $launch, trim $trim)"
[ "$dry" -lt "$rot1" ] && [ "$rot1" -lt "$launch" ] && [ "$launch" -lt "$trim" ]
chk $? "rotate happens after the RHYS_DRY_RUN exit and before the game starts; the trim after the run"
[ "$(grep -c 'rhy_rotate_log "\$R/' "$LAUNCH")" = 2 ]; chk $? "exactly two rotate call sites: the plugin log and the Proton trace"
# the block is SOURCED here, exactly like scripts/launch-cleanup-selftest.sh sources the n09b
# cleanup block: the launcher itself cannot run in a test (it deploys, checks the GPU and starts
# Proton), so its functions are driven directly against the fake tree.
. "$BLOCK"
[ "$(type -t rhy_rotate_log)" = function ] && [ "$(type -t rhy_trim_log)" = function ]
chk $? "the extracted block defines rhy_rotate_log and rhy_trim_log as functions"

echo "== A: under the cap nothing rotates"
RHYS_LOG_MAX_BYTES=$((BEFORE_LOG + 1000)) rhy_rotate_log "$LOG" 0 > "$OUT/under-cap.log" 2>&1
chk $? "rhy_rotate_log exits 0"
[ ! -e "$LOG.prev" ] && ok "no .prev created for a log under the cap" || bad "a log under the cap was rotated anyway"
[ "$(sz "$LOG")" = "$BEFORE_LOG" ]; chk $? "the under-cap log is untouched ($BEFORE_LOG bytes)"
grep -q 'logrotate' "$OUT/under-cap.log" && bad "it reported a rotation that did not happen" || ok "it stayed quiet"
RHYS_LOG_MAX_BYTES=$((BEFORE_PTON + 1000)) rhy_trim_log "$FAKE/logs/steam-12120.log" 2000 > "$OUT/under-cap-trim.log" 2>&1
[ "$(sz "$FAKE/logs/steam-12120.log")" = "$BEFORE_PTON" ] && ok "rhy_trim_log is quiet under the cap too" \
  || bad "rhy_trim_log trimmed a log that was under the cap"

echo "== B: over the cap both logs rotate, each into one .prev"
export RHYS_LOG_MAX_BYTES=4000 RHYS_PROTON_LOG_KEEP=2000
rhy_rotate_log "$LOG" 0 > "$OUT/rotate.log" 2>&1
rhy_rotate_log "$FAKE/logs/steam-12120.log" "$RHYS_PROTON_LOG_KEEP" >> "$OUT/rotate.log" 2>&1
cat "$OUT/rotate.log"
[ "$(sz "$LOG")" = 0 ]; chk $? "rhyremix.log restarts at 0 bytes (the plugin appends to it)"
[ -f "$LOG.prev" ]; chk $? "rhyremix.log.prev exists"
[ "$(sha "$LOG.prev")" = "$ORIG_LOG_SHA" ] \
  && ok "the plugin log moved WHOLE: .prev is byte-identical to the $BEFORE_LOG-byte original" \
  || bad "rhyremix.log.prev is not the original file byte for byte (a line got cut)"
[ "$(sz "$FAKE/logs/steam-12120.log")" = 0 ]; chk $? "steam-12120.log restarts at 0 bytes"
[ "$(sz "$FAKE/logs/steam-12120.log.prev")" -le 2000 ] \
  && ok "the Proton log's .prev is capped: $(sz "$FAKE/logs/steam-12120.log.prev") bytes kept of $BEFORE_PTON" \
  || bad "the Proton .prev is $(sz "$FAKE/logs/steam-12120.log.prev") bytes, over the 2000-byte keep"
tail -c 200 "$FAKE/logs/steam-12120.log.prev" | grep -q 'stub: 0xf9f' \
  && ok "what was kept is the TAIL of the Proton log (its last line, 0xf9f, is in it)" \
  || bad "the Proton .prev is not the tail of the run"
grep -c 'logrotate:' "$OUT/rotate.log" | grep -qx 2; chk $? "both rotations reported themselves"

echo "== C: a second rotation is a no-op (one .prev, no churn)"
prev_sha=$(sha "$LOG.prev")
rhy_rotate_log "$LOG" 0 > "$OUT/rotate2.log" 2>&1
[ ! -s "$OUT/rotate2.log" ] && ok "the second call is silent (an empty live log is under the cap)" \
  || bad "the second call rotated anyway: $(cat "$OUT/rotate2.log")"
[ "$(sha "$LOG.prev")" = "$prev_sha" ]; chk $? "rhyremix.log.prev unchanged by the second call (still exactly one .prev)"
[ "$(find "$FAKE" -name '*.prev' | wc -l)" = 2 ]; chk $? "exactly two .prev files in the tree (one per log, no pile-up)"
[ ! -e "$LOG.prev.part" ] && [ ! -e "$FAKE/logs/steam-12120.log.prev.part" ]
chk $? "no .part left behind by the tail path"

echo "== D: rhy_trim_log keeps the trace at the path the docs point at"
# after a run the trace must still be logs/steam-12120.log (docs/TESTING.md's OLA row, docs/FINDINGS.md's
# unload grep both name that path), just not 52 MB of it.
printf 'head of the trace: the part nobody reads\n' > "$FAKE/logs/steam-99.log"
python3 - "$FAKE/logs/steam-99.log" <<'PY'
import sys
with open(sys.argv[1], "a") as f:
    for i in range(4000):
        f.write("0120:err:module:attach_case try to load L\"C:\\\\windows\\\\system32\\\\noshell.so\" err 1780 0x%x\n" % i)
PY
big=$(sz "$FAKE/logs/steam-99.log")
rhy_trim_log "$FAKE/logs/steam-99.log" 2000 > "$OUT/trim.log" 2>&1
cat "$OUT/trim.log"
[ -f "$FAKE/logs/steam-99.log" ]; chk $? "the trimmed trace is still at logs/steam-99.log (no .prev, no move)"
[ "$(sz "$FAKE/logs/steam-99.log")" -le 2000 ] \
  && ok "it is now $(sz "$FAKE/logs/steam-99.log") bytes of the $big the run wrote" \
  || bad "rhy_trim_log left $(sz "$FAKE/logs/steam-99.log") bytes"
tail -c 120 "$FAKE/logs/steam-99.log" | grep -q 'err 1780 0xf9f' \
  && ok "what survived is the END of the run (its last line is there)" || bad "the trim did not keep the tail"
grep -q 'head of the trace' "$FAKE/logs/steam-99.log" && bad "the trim kept the head too" || ok "the head is gone, as intended"
grep -q 'logtrim:' "$OUT/trim.log"; chk $? "the trim said what it dropped, with the numbers"
[ ! -e "$FAKE/logs/steam-99.log.prev" ]; chk $? "the trim created no .prev (that is the rotate's job)"
[ ! -e "$FAKE/logs/steam-99.log.trim.part" ]; chk $? "no .trim.part left behind"
rm -f "$FAKE/logs/steam-99.log"

echo "== E: the Proton-log policy block (RHYS_PROTON_LOG)"
PBLOCK="$OUT/proton-block.sh"
sed -n '/# --- proton log policy (BEGIN)/,/# --- proton log policy (END)/p' "$LAUNCH" > "$PBLOCK"
[ -s "$PBLOCK" ]; chk $? "the proton-log policy block is extractable"
# sourced in a subshell: the block exports, and a subshell is where that has to stay
got=$( unset PROTON_LOG; SteamAppId=12120; . "$PBLOCK" >"$OUT/proton-on.log" 2>&1; printf '%s' "${PROTON_LOG:-unset}")
[ "$got" = "1" ]; chk $? "default: PROTON_LOG=1 (docs/TESTING.md's OLA row still gets its trace)"
got=$( unset PROTON_LOG; SteamAppId=12120; RHYS_PROTON_LOG=0; . "$PBLOCK" >"$OUT/proton-off.log" 2>&1; printf '%s' "${PROTON_LOG:-unset}")
[ "$got" = "unset" ]; chk $? "RHYS_PROTON_LOG=0: PROTON_LOG is not exported at all (the only cap that is 0 bytes)"
grep -q 'PROTON_LOG is NOT set' "$OUT/proton-off.log"; chk $? "and it says so in the run log, not silently"

echo "== F: scripts/diagnose.sh still finds every line it claims, after the rotation"
bash "$REPO/scripts/diagnose.sh" "$FAKE" > "$OUT/diagnose.log" 2>&1
chk $? "scripts/diagnose.sh exits 0 against the rotated fake tree (the live log is 0 bytes)"
grep -q "^reading: $LOG.prev $LOG$" "$OUT/diagnose.log" \
  && ok "it says it is reading rhyremix.log AND rhyremix.log.prev, oldest first" \
  || bad "it did not report reading both logs: $(grep -m1 '^reading:' "$OUT/diagnose.log")"
missing=0
# the nine lines diagnose.sh's own ts_grep calls ask for. Two more (config: fade, worst case) sit in
# the log as realistic content but are not claims, so they are not required.
for m in lights signals hook bulb glow headsource gaveup probe sky; do
  grep -q "n03=$m" "$OUT/diagnose.log" || { echo "   diagnose.sh lost the $m line"; missing=$((missing+1)); }
done
[ "$missing" = 0 ]; chk $? "all 9 lines diagnose.sh claims to print come back after the rotation"
grep -q 'heads lit (7 with a glow)' "$OUT/diagnose.log"; chk $? "the s30/n02b glow-lag line is still parsed (7 lit, 7 with a glow)"
grep -qE 'remix: live=320 create_fail=0 draw_fail=0' "$OUT/diagnose.log"; chk $? "the remix counters section is still filled from the rotated log"
grep -q 'bridge_invalid=1' "$OUT/diagnose.log"; chk $? "the verdict is still computed from the rotated log (bridge_invalid=1)"

echo "== G: a new session's line beats the rotated one (the order is oldest first)"
{
  echo '---- log starts ----'
  echo '[2026-11-14 09:30:00.000] streetlights: 400 active, 400 drawn this frame (hour=22, in_drop=0) | remix: live=400 create_fail=0 draw_fail=0 n03=new1'
} > "$LOG"
bash "$REPO/scripts/diagnose.sh" "$FAKE" > "$OUT/diagnose2.log" 2>&1
chk $? "diagnose.sh exits 0 with a live log AND a .prev"
grep -q 'n03=new1' "$OUT/diagnose2.log"; chk $? "the newest line, from the live log, is shown"
grep -q 'n03=old5' "$OUT/diagnose2.log"; chk $? "the newest .prev lines are still reachable (old5 is inside the tail window)"
grep -q 'n03=old1' "$OUT/diagnose2.log" && bad "the OLDEST .prev line outranked the new session's line" \
  || ok "the oldest .prev line lost to the new one (tail -n means newest, across both files)"

echo "== H: tools/tour-report.py still finds all three log categories"
missing=0
python3 "$REPO/tools/tour-report.py" "$FAKE" --no-sheets --out "$OUT/report" > "$OUT/tour-report.log" 2>&1
chk $? "tools/tour-report.py exits 0 against the rotated fake tree"
grep -q 'plus its rotated' "$OUT/tour-report.log" \
  && ok "it says out loud that it also read rhyremix.log.prev" || bad "it did not report reading the .prev"
R="$OUT/report/report.md"
[ -f "$R" ]; chk $? "report.md written"
grep -qE 'read from 2 file\(s\)' "$R" \
  && ok "the report header counts 2 log files: $(grep -m1 '| log |' "$R")" \
  || bad "the report header does not count the rotated file: $(grep -m1 '| log |' "$R")"
missing=0
# one pattern per LOG_CATEGORIES entry: which line wins is nearest is the tool's business, that the
# category comes back at all from a rotated tree is the claim being tested.
declare -A WANT=( [lights]='n03=lights|n03=old[1-5]' [signals]='n03=signals' [sky]='n03=sky' )
for c in lights signals sky; do
  grep -qE "${WANT[$c]}" "$R" || { echo "   tour-report lost the $c category"; missing=$((missing+1)); }
done
[ "$missing" = 0 ]; chk $? "all three LOG_CATEGORIES still appear under a shot after the rotation"
grep -q 'nearest rhyremix.log lines' "$R"; chk $? "the per-shot log section is present"

echo "== I: the real game folder was never opened"
case "$*" in *GTASA-RTX*) bad "a game folder was passed to this test";; *) ok "no game folder argument anywhere";; esac

[ "$KEEP" = 1 ] || rm -rf "$ROOT"
echo
echo "$pass checks passed, $fail failed"
[ "$fail" = 0 ] && echo "ALL PASS" || echo "$fail FAILED"
exit $((fail > 0))
