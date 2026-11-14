#!/usr/bin/env bash
# One command that runs every gate in this repo.
#
#   bash scripts/check-all.sh
#
# scripts/check.sh <phase> is the per-phase gate and stays the thing CI-style work goes
# through; this file is the "am I done with everything" sweep: the cross-compiled plugin
# build, the native test binaries, every python test file, the installer self-test,
# installer, the vegetation switch self-test (s16: an on/off/status round-trip against the same fake
# tree, every file in it sha256'd before and after), the character-pack switch self-test (s17:
# the six "characters" packs of deps/manifest.json staged synthetically, installed in the
# documented order with their priorities written inside [Profiles.Default.Priority], and an
# on/off round-trip that hashes back byte-identical), the test-session stub test (s15: fake
# Strata/game/nvidia-smi under build/tmp, never the real ones), the rtx profile audit +
# switch round-trip (s22: every key of config/rtx.conf.perf looked up in the option-name
# vocabulary generated from `strings` on the runtime's d3d9.dll, then perf/current/orig/restore
# against a fake game tree, hashing the whole file back), the water-by-default round-trip (n13:
# a fresh fake install - the pack's own water material, an rtx.conf with every water key removed -
# must end up on the remix-ref variant after one RHYS_DRY_RUN=1 launcher run, must stay
# byte-identical on the next run, and must never lose a variant that was chosen explicitly; the
# installer half of the same feature is inside the installer self-test), the HD PBR surface switch
# (n20/n20b: our own BC4/BC5/BC3 encoders cross-checked against Pillow, ffmpeg and the spec block
# layout, the generated DDS checked against the installed pack's own format census, and a
# textures.sh on/on/off round-trip against a fake game tree hashed back to its starting bytes) and
# a syntax check of every shell script we ship. One PASS/FAIL line per check,
# then either the list of failures or "ALL GATES PASSED".
#
# Since s13 the C++ also has to compile without a single warning: the plugin carries
# -Wall -Wextra in plugin/CMakeLists.txt and the native test compiles below pass them on
# the command line, and warncheck() turns a "warning:" line in a compile log into a FAIL
# even when the compiler exited 0.
#
# Nothing here touches the game folder, wine or the GPU: the native tests are host-side
# g++ binaries, the python tests use fake paths, and installer-selftest.sh installs into
# a fake tree under build/tmp/ (vegetation-selftest.sh reuses that same tree). All output
# goes to build/tmp/check-all/. The two exceptions read the game folder and never write
# to it: the HD PBR gate (n20b) and the lamp coverage gate (n11, which regenerates the
# street-lamp list from the game's own 2dFx records into build/).
set -uo pipefail
cd "$(dirname "$0")/.."

LOGDIR=build/tmp/check-all
rm -rf "$LOGDIR"
mkdir -p "$LOGDIR" build/tmp

pass=0
fails=()

# run <slug> <label> <timeout-seconds> <command...>
run() {
  local slug=$1 label=$2 tmo=$3 log rc
  shift 3
  log="$LOGDIR/$slug.log"
  timeout "$tmo" "$@" >"$log" 2>&1
  rc=$?
  if [ "$rc" = 0 ]; then
    pass=$((pass + 1))
    echo "PASS $label"
  else
    fails+=("$label [$slug] (exit $rc, log: $log)")
    echo "FAIL $label  (exit $rc, log: $log)"
    tail -n 5 "$log" | sed 's/^/       /'
  fi
}

# warncheck <slug> <label>: run() only looks at the exit code, but a compile that
# succeeds while printing warnings is not good enough any more (s13). Must be called
# right after the run of the same slug; it turns that PASS into a FAIL.
warncheck() {
  local slug=$1 label=$2 log n
  log="$LOGDIR/$slug.log"
  [ -f "$log" ] || return 0
  if ! grep -q "warning:" "$log"; then
    return 0
  fi
  n=$(grep -c "warning:" "$log")
  pass=$((pass - 1))
  fails+=("$label compiled with $n warning(s) [$slug] (log: $log)")
  echo "FAIL $label compiled with $n warning(s)  (log: $log)"
  grep "warning:" "$log" | head -n 8 | sed 's/^/       /'
}

echo "== plugin (cross-compiled 32-bit asi) =="
run build "plugin/build.sh -> build/rhyremix.asi" 600 bash plugin/build.sh
warncheck build "plugin/build.sh"

echo "== native tests (host g++, no game) =="
# lamps.cpp + solar.cpp are the physics under test; solar_check.cpp #includes solar.cpp
# itself, so it must be compiled alone or the symbols would be defined twice.
# -Wall -Wextra matches the plugin's own flags so a warning cannot hide in the test build.
run nat-lamps-cc "compile lamps_check" 600 \
  g++ -std=c++17 -Wall -Wextra -Iplugin/src tests/lamps_check.cpp plugin/src/lamps.cpp plugin/src/solar.cpp -o build/tmp/lc
warncheck nat-lamps-cc "compile lamps_check"
run nat-lamps "tests/lamps_check.cpp (lamps.cpp + solar.cpp)" 120 build/tmp/lc
run nat-solar-cc "compile solar_check" 600 \
  g++ -std=c++17 -Wall -Wextra -Iplugin/src tests/solar_check.cpp -o build/tmp/sc
warncheck nat-solar-cc "compile solar_check"
run nat-solar "tests/solar_check.cpp (includes solar.cpp)" 120 build/tmp/sc
run nat-signals-cc "compile signals_check" 600 \
  g++ -std=c++17 -Wall -Wextra -Iplugin/src tests/signals_check.cpp plugin/src/signalheads.cpp plugin/src/lamps.cpp plugin/src/solar.cpp -o build/tmp/sigc
warncheck nat-signals-cc "compile signals_check"
run nat-signals "tests/signals_check.cpp (signalheads.cpp + lamps.cpp + solar.cpp)" 120 build/tmp/sigc
# s28: the "lighting must never change instantly" bounds. smooth.cpp is pure C++ (no
# Windows, no Remix), so the whole fade/rate-limit/selection model runs here against
# the sequences the 2026-11-12 test log actually recorded: a 50x ambient step, a clock
# that jumped 104 degrees of sun elevation, a whole street appearing in one frame, and
# a player ped pointer that read 0 for 13 seconds.
run nat-smooth-cc "compile smooth_check" 600 \
  g++ -std=c++17 -Wall -Wextra -Iplugin/src tests/smooth_check.cpp plugin/src/smooth.cpp plugin/src/game.cpp -o build/tmp/smoothc
warncheck nat-smooth-cc "compile smooth_check"
run nat-smooth "tests/smooth_check.cpp (smooth.cpp fades/rate limits/budget + game.cpp player grace)" 120 build/tmp/smoothc
# n04 (finding A of test-2026-11-12b, the top-priority one): a lamp in the streamed
# area exists all the time, and nothing about it may depend on which way the camera
# points. Selection (smooth.cpp) and signal-head lifetime (signalheads.cpp) are pure
# C++, so a 360-degree camera sweep, a 300 s walk and a 240 s drive through the
# worst-case lamp density measured on the real 7002-lamp list run here, and the
# assertion is that the light set does not notice any of it.
run nat-alwayson-cc "compile always_on_check" 600 \
  g++ -std=c++17 -Wall -Wextra -Iplugin/src tests/always_on_check.cpp plugin/src/smooth.cpp plugin/src/signalheads.cpp plugin/src/lamps.cpp plugin/src/solar.cpp -o build/tmp/alwaysonc
warncheck nat-alwayson-cc "compile always_on_check"
run nat-alwayson "tests/always_on_check.cpp (n04: selection and head lifetime are distance-only)" 300 build/tmp/alwaysonc
# s29: "time of day flickers and does not progress smoothly". tod.cpp is the time
# axis - pure C++, no Windows, no Remix - so a whole simulated 24 h day runs here at
# 60 fps, sampled both the way sun.cpp did it before s29 and the way it does it now,
# against the clock and calendar byte sequences the 2026-11-12 log recorded.
run nat-tod-cc "compile tod_check" 600 \
  g++ -std=c++17 -Wall -Wextra -Iplugin/src tests/tod_check.cpp plugin/src/tod.cpp plugin/src/solar.cpp plugin/src/smooth.cpp -o build/tmp/todc
warncheck nat-tod-cc "compile tod_check"
run nat-tod "tests/tod_check.cpp (tod.cpp clock/calendar/weather + solar.cpp + smooth.cpp)" 300 build/tmp/todc
# n01 (closes s29 note T7): WHEN the sun/moon distant-light handle is replaced. The
# same simulated sky runs through the old threshold rule and through the cadence in
# smooth.cpp, and the two are compared on creates/s, the size of one step and the
# evenness of the spacing - the three numbers that decide whether a player sees a
# shadow direction jump. The sky sequences are the ones the 2026-11-12 log recorded:
# a normal morning, the 104.5 degree clock jump, the tour's clock pin, twilight's
# 300x radiance ramp, and a pinned clock.
run nat-handle-cc "compile handle_check" 600 \
  g++ -std=c++17 -Wall -Wextra -Iplugin/src tests/handle_check.cpp plugin/src/smooth.cpp -o build/tmp/handlec
warncheck nat-handle-cc "compile handle_check"
run nat-handle "tests/handle_check.cpp (n01 handle cadence: old threshold rule vs clock)" 120 build/tmp/handlec
# n12 (V5 of the 2026-11-12c report): the car's own lamps. vehiclelights.cpp is pure
# C++ - the corona record is a plain struct, so the game's light dummies can be built
# by hand and the assertions are physical: which corona became which lamp, which way
# the beam points (from the car's own front/rear dummies), how wide it is (40-60 deg),
# and how much light it carries (a low beam's ~5000 peak intensity against a street
# luminaire's 868, a rear position lamp's ~25 cd against a 30 m emissive billboard).
run nat-veh-cc "compile vehicle_lights_check" 600 \
  g++ -std=c++17 -Wall -Wextra -Iplugin/src tests/vehicle_lights_check.cpp plugin/src/vehiclelights.cpp plugin/src/lamps.cpp plugin/src/solar.cpp -o build/tmp/vehc
warncheck nat-veh-cc "compile vehicle_lights_check"
run nat-veh "tests/vehicle_lights_check.cpp (n12: corona records -> sized head and tail lamps)" 120 build/tmp/vehc
# n09 (the 2026-11-12 exit hang): WHEN the plugin is allowed to stop calling the Remix bridge.
# shutdown.cpp is pure C++ - the device reads are arguments - so every path runs here: a one-frame
# Present failure, an alt-tab that comes back, an alt-tab that does not, Quit (the window goes first),
# our own failing create/draw counters with and without company, DLL_PROCESS_DETACH, a GetTickCount
# wrap, and the honest limit of a tracker that is fed from Present (on 2026-11-12 the game stopped
# presenting 340 ms before its exit path, so the shell watchdog is the other half).
run nat-shut-cc "compile shutdown_check" 600 \
  g++ -std=c++17 -Wall -Wextra -Iplugin/src tests/shutdown_check.cpp plugin/src/shutdown.cpp -o build/tmp/shuttc
warncheck nat-shut-cc "compile shutdown_check"
run nat-shut "tests/shutdown_check.cpp (n09 shutdown detection: windows, latching, wrap)" 120 build/tmp/shuttc
# n22 (docs/TARGET.md §6, the follow-up n16 queued): the renderer's exposure anchor.
# exposure.cpp is pure C++ (the curve, its floors, the follower), so the whole claim of
# the change runs here: the anchor is a function of SOLAR ELEVATION (equal elevation on
# the way up and on the way down gives the same EV), at full daylight it equals the -0.5
# the three profiles carry (so noon is a no-op and the daytime numbers stay n16's), the
# floors cannot be talked out of, and over a whole simulated game day - and across the
# tour's pinned clock jumps - the published value never moves faster than
# [tonemap] max_ev_per_sec. What this CANNOT do is say what the frame looks like: that
# needs a live run (rule 1), and docs/TARGET.md §6 is the protocol for it.
run nat-expo-cc "compile exposure_check" 600 \
  g++ -std=c++17 -Wall -Wextra -Iplugin/src tests/exposure_check.cpp plugin/src/exposure.cpp plugin/src/solar.cpp plugin/src/smooth.cpp -o build/tmp/expc
warncheck nat-expo-cc "compile exposure_check"
run nat-expo "tests/exposure_check.cpp (n22: the exposure anchor follows the sun, bounded)" 120 build/tmp/expc
# n08 (finding G, "FPS a little less steady"): the tail, not the average. The median frame of
# both 2026-11-12 runs is 16.68 ms - exactly the 60 Hz budget - so any work the plugin owns has
# to be spread over frames or it becomes a spike. smooth.cpp's drain queue and resync schedule
# are pure C++, so the two bounds are asserted here: no frame destroys more than
# [fade] destroys_per_frame handles (the runs measured that queue at 320 and 205, all of them
# released inside one Present), and the street and signal list scans never share a frame.
run nat-drain-cc "compile drain_spread_check" 600 \
  g++ -std=c++17 -Wall -Wextra -Iplugin/src tests/drain_spread_check.cpp plugin/src/smooth.cpp -o build/tmp/drainc
warncheck nat-drain-cc "compile drain_spread_check"
run nat-drain "tests/drain_spread_check.cpp (n08: per-frame destroy bound + resync phase)" 120 build/tmp/drainc

echo "== lamp coverage (n11: the game's own 2dFx lights vs our lamp list; reads the game folder, writes only build/) =="
# n11 (rhY's 2026-11-12c V2: "boardwalk/beach lamps are dark: no lit bulb, no pool
# of light under them"). The ground truth for "which lamps exist" is not a third
# party's table: it is the 2dFx LIGHT records inside the game's own .dff, placed by
# the game's own IPLs. tools/dff_2dfx.py reads them, tools/lamp_list.py turns them
# into the list the plugin loads, and tools/lamp-coverage.py measures the list
# against the inventory. Measured: the old lamp table covered 2617 of the 3536 Los
# Santos street lights (74.0%); the 2dFx list covers 3536 (100%). The gate is 95%
# so a regression that drops a whole model class fails loudly, and the density
# numbers the caps are set from get printed every run instead of being remembered.
run lamps-gen "tools/lamp_list.py -> build/rhyremix_lights.txt + build/rhyremix_2dfx.txt" 900 \
  python3 tools/lamp_list.py --game /media/rhy/data/Gamez/GTASA-RTX \
    --out build/rhyremix_lights.txt --inventory build/rhyremix_2dfx.txt --report-missing
run lamps-cov "tools/lamp-coverage.py: LA coverage >= 95% of the game's own 2dFx lights" 600 \
  python3 tools/lamp-coverage.py --list build/rhyremix_lights.txt \
    --inventory build/rhyremix_2dfx.txt --region LA --min 95 --density 300,380,120

echo "== python tests =="
# Every file runs on its own: they are unittest-style and do sys.path.insert(0, "tools"),
# so cwd must be the repo root and a crash in one file must not hide the others.
if command -v pytest >/dev/null 2>&1 && python3 -c 'import pytest' >/dev/null 2>&1; then
  pypkg=pytest
else
  pypkg=unittest
fi
for f in tests/test_*.py; do
  [ -e "$f" ] || { fails+=("no tests/test_*.py found") ; echo "FAIL no python tests found"; continue; }
  slug="py-$(basename "$f" .py)"
  case "$pypkg" in
    pytest)   run "$slug" "pytest $f" 120 python3 -m pytest -q "$f" ;;
    unittest) run "$slug" "unittest $f" 120 python3 -m unittest discover -s tests -p "$(basename "$f")" ;;
  esac
done

echo "== installer =="
run installer "scripts/installer-selftest.sh" 600 bash scripts/installer-selftest.sh

echo "== vegetation switch (tools/vegetation.sh on/off/status against a fake tree) =="
run vegetation "scripts/vegetation-selftest.sh" 600 bash scripts/vegetation-selftest.sh

echo "== character packs (tools/characters.sh on/off/status against a fake tree) =="
run characters "scripts/characters-selftest.sh" 600 bash scripts/characters-selftest.sh

echo "== test-session (stubs only: no game, no GPU, no real Strata) =="
run testsess "scripts/test-session-selftest.sh" 300 bash scripts/test-session-selftest.sh

echo "== launcher exit cleanup (stub gta-sa.exe / NvRemixBridge.exe / wineserver, no game, no wine) =="
# n09b: the half that actually ends the Quit hang. launcher/launch-rtx.sh waits for gta-sa.exe, so a
# game that spins after Quit also stops the launcher from ever cleaning up; the cleanup block inside
# it is sourced and driven against stub processes by scripts/launch-cleanup-selftest.sh.
run launchcleanup "scripts/launch-cleanup-selftest.sh" 300 bash scripts/launch-cleanup-selftest.sh

echo "== rtx profiles (config/rtx.conf.perf keys audited against the runtime's own option names; tools/rtxprofile.sh round-trip on a fake tree) =="
run rtxprofile "scripts/rtxprofile-selftest.sh" 300 bash scripts/rtxprofile-selftest.sh

echo "== water by default (tools/water.sh + apply.sh + a launcher dry run against a fake install; no game, no GPU, no wine) =="
# n13 (rhY: "make sure we get the new water by default"). The installer half lives inside
# scripts/installer-selftest.sh (remix-ref installed by apply.sh, its rtx.conf keys applied with a
# backup, --water pack / --no-water honoured). This is the launcher half: one RHYS_DRY_RUN=1 start
# on a fresh fake install must produce remix-ref + its rtx.conf keys, the next start must change no
# byte, and a variant someone chose explicitly must survive every later default run.
run waterdefault "scripts/water-default-selftest.sh" 300 bash scripts/water-default-selftest.sh

echo "== HD PBR surfaces (tools/textures.sh + pbrify + our BCn encoders; reads the game folder, writes only build/tmp) =="
# n20/n20b (rhY's MUST-HAVE #1): the 40 most visible map surfaces get albedo/normal/rough/height
# maps generated from the game's own rasters. The BCn encoders are ours because Pillow 13 and
# ImageMagick 7 both refuse to encode BCn here, so they are cross-checked against Pillow and
# ffmpeg and against the D3D block layouts written out of the spec; the generated DDS are checked
# against the format census of the installed pack (BC3/BC5/BC4, DX10 headers, full mip chains);
# and the install round-trip runs against a fake game tree, hashed byte-for-byte.
run textures "scripts/textures-selftest.sh" 600 bash scripts/textures-selftest.sh

echo "== shell syntax (bash -n) =="
# Every .sh in the tree except generated output (build/, rule 2) and git internals. A
# script added this session is checked, including this one; git ls-files would miss the
# locally-excluded helpers under .git/info/exclude, so this is a plain find.
shfiles=$(find . -path ./.git -prune -o -path ./build -prune -o -type f -name '*.sh' -print \
          | sed 's|^\./||' | sort)
if [ -z "$shfiles" ]; then
  fails+=("no *.sh files found for bash -n")
  echo "FAIL no shell scripts found"
fi
while IFS= read -r s; do
  [ -n "$s" ] || continue
  run "syntax-$(echo "$s" | tr '/.' '__')" "bash -n $s" 30 bash -n "$s"
done <<< "$shfiles"

echo
echo "$pass checks passed, ${#fails[@]} failed"
if [ "${#fails[@]}" -ne 0 ]; then
  echo "FAILURES:"
  for f in "${fails[@]}"; do echo "  - $f"; done
  exit 1
fi
echo "ALL GATES PASSED"
