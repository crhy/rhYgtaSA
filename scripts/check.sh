#!/bin/bash
# Gate for rhYgtaSA. Usage: bash scripts/check.sh <phase>   (p01 ...)
set -uo pipefail
cd "$(dirname "$0")/.."
P=${1:?phase}; fail=0
ok(){ echo "ok   $*"; }; bad(){ echo "FAIL $*"; fail=1; }
done_(){ [ $fail = 0 ] && echo "All checks passed." || echo "GATE FAILED"; exit $fail; }
command -v shellcheck >/dev/null && { shellcheck -S warning apply.sh launcher/launch-rtx.sh && ok "shellcheck scripts" || bad "shellcheck"; } || echo "skip shellcheck (not installed)"
if [ "$P" = p01 ] || [[ "$P" > p01 ]]; then
  rm -rf build/gate && timeout 600 cmake -S plugin -B build/gate -DCMAKE_TOOLCHAIN_FILE=plugin/toolchain-mingw32.cmake -DCMAKE_BUILD_TYPE=Release >/dev/null \
    && timeout 600 cmake --build build/gate >build/gate.log 2>&1 && ok "plugin builds" || bad "plugin build failed (see build/gate.log)"
  A=$(find build/gate -maxdepth 2 -name 'rhyremix.asi' | head -1)
  [ -n "$A" ] && file "$A" | grep -q 'PE32 executable.*(DLL).*Intel i386' && ok "rhyremix.asi is a 32-bit DLL" || bad "rhyremix.asi missing or not PE32 DLL"
  if [ -n "$A" ]; then
    deps=$(i686-w64-mingw32-objdump -p "$A" | awk '/DLL Name:/{print tolower($3)}' | sort -u | tr '\n' ' ')
    echo "     imports: $deps"
    echo "$deps" | grep -qE 'libstdc|libgcc|libwinpthread' && bad "asi depends on MinGW runtime DLLs" || ok "no MinGW runtime DLL deps"
    echo "$deps" | grep -q 'd3d9.dll' && bad "asi imports d3d9.dll statically (must use GetModuleHandle/GetProcAddress)" || ok "no static d3d9 import"
  fi
  grep -q "warning:" build/gate.log && echo "     (compiler warnings present: $(grep -c 'warning:' build/gate.log))"
  grep -q "vsnprintf" plugin/src/log.cpp && ok "logger uses vsnprintf" || bad "logger must use vsnprintf for va_list"
  grep -q "0xC97C28" plugin/src/*.cpp && ok "uses the game device pointer" || bad "0xC97C28 not used"
  grep -q "0xB7CD98" plugin/src/game.cpp && grep -q "rhyPlayerPos" plugin/src/streetlights.cpp && ok "player position shared via game.cpp" || bad "player position not shared via game.cpp"
  grep -q "remixapi_InitializeLibrary" plugin/src/*.cpp && ok "loads Remix API" || bad "remixapi_InitializeLibrary not referenced"
  grep -q "DrawLightInstance" plugin/src/*.cpp && ok "draws a light" || bad "DrawLightInstance not used"
fi
if [ "$P" = p02a ] || [[ "$P" > p02a ]]; then
  timeout 300 python3 -m unittest discover -s tests >build/unittest.log 2>&1 && ok "unit tests pass" || bad "unit tests fail (build/unittest.log)"
  timeout 600 python3 tools/lamp_list.py --game /media/rhy/data/Gamez/GTASA-RTX --out build/gate-lights.txt >build/lamp_list.log 2>&1 && ok "lamp_list.py runs" || bad "lamp_list.py failed (build/lamp_list.log)"
  ! git ls-files --error-unmatch data/SARemix_Light_Manager.dat >/dev/null 2>&1 && ok "the third-party lamp table stays out of the repo (read from the game dir)" || bad "the repo ships a copy of someone else's mod file"
  n=$(grep -vc '^#' build/gate-lights.txt 2>/dev/null || echo 0); [ "$n" -ge 1000 ] && ok "$n lights" || bad "only $n lights"
  g=$(awk '!/^#/{dx=$2-2495; dy=$3+1680; if (dx*dx+dy*dy<90000) c++} END{print c+0}' build/gate-lights.txt 2>/dev/null); [ "${g:-0}" -ge 5 ] && ok "$g lights near Grove Street" || bad "only ${g:-0} lights near Grove Street"
  bad_t=$(awk '!/^#/ && ($1<1 || $1>12){c++} END{print c+0}' build/gate-lights.txt 2>/dev/null); [ "$bad_t" = 0 ] && ok "types 1-12" || bad "$bad_t lines with bad type"
fi
if [ "$P" = p02b ] || [[ "$P" > p02b ]]; then
  grep -q "rhyremix.ini" plugin/src/*.cpp && ok "reads rhyremix.ini" || bad "no rhyremix.ini"
  grep -q "rhyremix_lights.txt" plugin/src/*.cpp && ok "reads the light list" || bad "no rhyremix_lights.txt"
  grep -q "coneAngleDegrees" plugin/src/*.cpp && ok "spot shaping" || bad "no shaping"
  grep -q "rhyDelayDestroy" plugin/src/streetlights.cpp && ok "retires far lights instead of destroying them mid-frame" || bad "streetlights.cpp never retires lights"
  grep -q "pos\[2\] + 2.0f" plugin/src/*.cpp && bad "CJ test light still present" || ok "test light removed"
fi
if [ "$P" = p03 ] || [[ "$P" > p03 ]]; then
  timeout 600 python3 tools/traffic_list.py --game /media/rhy/data/Gamez/GTASA-RTX --out build/gate-traffic.txt >build/traffic_list.log 2>&1 && ok "traffic_list.py runs" || bad "traffic_list.py failed (build/traffic_list.log)"
  n=$(grep -vc '^#' build/gate-traffic.txt 2>/dev/null || echo 0); [ "$n" -ge 1000 ] && ok "$n traffic lights" || bad "only $n traffic lights"
  bad_t=$(awk '!/^#/ && NF!=4{c++} END{print c+0}' build/gate-traffic.txt 2>/dev/null); [ "${bad_t:-0}" = 0 ] && ok "traffic lines are x y z headz" || bad "$bad_t malformed traffic lines"
  g=$(awk '!/^#/{dx=$1-2495; dy=$2+1680; if (dx*dx+dy*dy<90000) c++} END{print c+0}' build/gate-traffic.txt 2>/dev/null); [ "${g:-0}" -ge 5 ] && ok "$g traffic lights near Grove Street" || bad "only ${g:-0} traffic lights near Grove Street"
  grep -q "rhyremix_traffic.txt" plugin/src/*.cpp && ok "reads rhyremix_traffic.txt" || bad "no rhyremix_traffic.txt"
  grep -q "trafficlights" plugin/src/config.cpp && ok "rhyremix.ini has a [trafficlights] section" || bad "no [trafficlights] config"
  grep -q "rhyDelayDestroy" plugin/src/trafficlights.cpp && ok "retires stale traffic lights" || bad "trafficlights.cpp never retires lights"
  grep -q "0xB70153" plugin/src/game.cpp && grep -q "rhyGameHour" plugin/src/trafficlights.cpp && ok "traffic lights read the game clock via game.cpp" || bad "trafficlights.cpp ignores the game clock"
  grep -q "trafficlights.cpp" plugin/CMakeLists.txt && ok "trafficlights.cpp is compiled in" || bad "trafficlights.cpp not in CMakeLists"
fi
if [ "$P" = p04 ] || [[ "$P" > p04 ]]; then
  grep -q "kSetGammaRampEntry = 21" plugin/src/main.cpp && ok "SetGammaRamp guarded at vtable entry 21" || bad "no SetGammaRamp guard"
  grep -q "block_gamma" plugin/src/config.cpp && ok "gamma guard is switchable" || bad "gamma guard not configurable"
  grep -q "DestroyLight" plugin/src/delayed.cpp && ok "delayed.cpp destroys handles" || bad "no delayed destroy implementation"
  grep -q "rhyFlushDelayed" plugin/src/main.cpp && ok "pending handles flushed per frame" || bad "pending handles never flushed"
  grep -q "delayed.cpp" plugin/CMakeLists.txt && ok "delayed.cpp compiled in" || bad "delayed.cpp not in CMakeLists"
  grep -q "probe_radiance" plugin/src/config.cpp && ok "probe light calibration knobs" || bad "no probe light knobs"
  grep -q "probe.cpp" plugin/CMakeLists.txt && ok "probe.cpp compiled in" || bad "probe.cpp not in CMakeLists"
  grep -q "probe_hash" plugin/src/config.cpp && grep -q "probe_hash" plugin/src/probe.cpp && ok "probe light controls its LightInfo hash" || bad "probe light hash unhandled (only hash 0 has never shown up)"
  grep -q "probe_sweep" plugin/src/probe.cpp && ok "probe sweeps radiance/radius by x10 steps" || bad "probe cannot bracket the visible order of magnitude"
  grep -q "probe_scale" plugin/src/probe.cpp && ok "probe tests the coordinate space (game units vs Remix metres)" || bad "probe ignores probe_scale"
  grep -q "bridge64" plugin/src/probe.cpp && ok "probe points at bridge64.log for server-side rejections" || bad "probe does not mention bridge64.log"
  grep -q "probe_shape" plugin/src/probe.cpp && grep -q "probe_shape" plugin/src/config.cpp && ok "probe can be bare, sphere, disk or cylinder" || bad "probe cannot change light type"
  grep -q "REMIXAPI_STRUCT_TYPE_LIGHT_INFO_CYLINDER_EXT" plugin/src/probe.cpp && ok "probe covers the cylinder type the header offers" || bad "probe misses the cylinder type"
  # diagnose.sh: the two logs were read by hand and disagreed; one script must answer it
  [ -x scripts/diagnose.sh ] && ok "diagnose.sh is executable" || bad "scripts/diagnose.sh missing or not executable"
  grep -q "Invalid light handle" scripts/diagnose.sh && ok "diagnose reads the bridge log our log cannot see" || bad "diagnose ignores the bridge log"
  grep -q "active vs drawn" scripts/diagnose.sh && ok "diagnose states the active-vs-drawn question" || bad "diagnose does not ask active vs drawn"
  grep -q "sharing one timestamp" scripts/diagnose.sh && ok "diagnose separates one bad light from a rejected set" || bad "diagnose cannot tell one bad light from a rejected set"
  grep -q "no plugin log" scripts/diagnose.sh && ok "diagnose says so when the game has not run" || bad "diagnose assumes a log exists"
  grep -qE "set -uo pipefail" scripts/diagnose.sh && ok "diagnose survives a grep with no matches" || bad "diagnose dies when a grep finds nothing"
  grep -q "rhyConfigReloadIfChanged" plugin/src/config.cpp && grep -q "rhyConfigReloadIfChanged" plugin/src/main.cpp && ok "rhyremix.ini re-read live" || bad "rhyremix.ini is only read at startup"
  grep -q "game.cpp" plugin/CMakeLists.txt && ok "game.cpp compiled in" || bad "game.cpp not in CMakeLists"
  # Every light the plugin makes goes through one wrapper, so the wrapper can
  # count what the bridge actually accepted.  A module that calls api->CreateLight
  # directly is invisible to that count, which is how "60 active lights, none
  # visible" stayed unexplained for so long.
  grep -q "rhyCreateLight" plugin/src/remix.cpp && ok "light calls centralised in remix.cpp" || bad "light calls not centralised"
  grep -q "rhyCreateLight" plugin/src/streetlights.cpp && grep -q "rhyCreateLight" plugin/src/probe.cpp && grep -q "rhyCreateLight" plugin/src/sun.cpp && ok "streetlights, probe and sun use the wrapper" || bad "a module calls CreateLight directly"
  grep -q "rhyLiveLights" plugin/src/main.cpp && ok "live handle count logged every second" || bad "live handle count not logged"
  grep -q "max_total_lights" plugin/src/config.cpp && grep -q "rhySetLightBudget" plugin/src/main.cpp && ok "total live light cap wired from the ini" || bad "no total live light cap"
  grep -q "g_cfgRev" plugin/src/streetlights.cpp && ok "a module disabled by a broken bridge retries on the next ini edit" || bad "a disabled module stays disabled forever"
  grep -q "bridge64" plugin/src/probe.cpp && ok "probe points at bridge64.log for the real reason" || bad "probe does not point at bridge64.log"
  awk -F= '/^radius=/{r=$2} /^radiance_big=/{rb=$2} END{exit !(r>=5 && r<=200 && rb>0 && rb<=50)}' config/rhyremix-test.ini && ok "test ini radius/radiance are in game units" || bad "test ini radiance/radius are the wrong order of magnitude"
  grep -q "radius=15" plugin/src/config.cpp && ok "built-in radius default is in game units" || bad "built-in radius default is wrong"
  grep -q "block_gamma" config/rhyremix-test.ini && ok "test ini sets block_gamma" || bad "test ini missing block_gamma"
  grep -q "sceneScale" README.md && ok "README documents the sceneScale unit conversion" || bad "README does not explain units"
fi
if [ "$P" = p05 ] || [[ "$P" > p05 ]]; then
  grep -q "sun.cpp" plugin/CMakeLists.txt && ok "sun.cpp compiled in" || bad "sun.cpp not in CMakeLists"
  grep -q "rhySunFrame" plugin/src/main.cpp && ok "sun drawn every frame" || bad "rhySunFrame never called"
  grep -q "REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DISTANT_EXT" plugin/src/sun.cpp && ok "uses the distant-light EXT" || bad "no distant light struct"
  grep -q "beamRadiance(moon" plugin/src/sun.cpp && grep -q "lunarPosition" plugin/src/sun.cpp && ok "moon radiance comes from the moon's own body (its own elevation)" || bad "moon would light by day, not by night"
  grep -q "moon_enabled" plugin/src/config.cpp && ok "moon is configurable" || bad "no moon config"
  grep -q "sun_step_deg" plugin/src/config.cpp && ok "sun rebuild step is configurable" || bad "no sun rebuild step"
  grep -qE "^\[sun\]" config/rhyremix-test.ini && grep -qE "^\[moon\]" config/rhyremix-test.ini && ok "test ini has [sun] and [moon]" || bad "test ini lacks [sun]/[moon]"
  grep -q "sun-off" deploy.sh && ok "deploy can disable the Lua sun" || bad "deploy cannot disable the Lua sun"
  grep -q "sun" scripts/abtest-lights.sh && ok "abtest can toggle the sun" || bad "no sun A/B switch"
  grep -q "MODS-REPLACED" README.md && ok "README links the mod inventory" || bad "README does not link the mod inventory"
fi
if [ "$P" = p06 ] || [[ "$P" > p06 ]]; then
  [ -x scripts/tidy-game.sh ] && ok "tidy-game.sh is executable" || bad "scripts/tidy-game.sh missing or not executable"
  grep -q -- '--apply' scripts/tidy-game.sh && grep -q 'dry run' scripts/tidy-game.sh && ok "tidy defaults to a dry run" || bad "tidy does not default to a dry run"
  grep -q 'rtx-remix/mods\*' scripts/tidy-game.sh && ok "tidy protects the renderer and our payload" || bad "tidy has no protected-prefix guard"
  grep -qE 'rm -rf "\$R' scripts/tidy-game.sh && bad "tidy deletes inside the game folder" || ok "tidy never deletes in the game folder"
  grep -q 'restore' scripts/tidy-game.sh && ok "tidy can undo itself" || bad "tidy cannot restore"
  grep -q 'OLD/\*/moonloader' deploy.sh && ok "deploy --lua-on/--sun-on can pull a parked .dat back from OLD/" || bad "deploy cannot restore a parked script"
  grep -q 'IgnoreMods' scripts/tidy-game.sh && ok "VehFuncs move re-checks modloader.ini" || bad "VehFuncs move trusts a stale assumption"
  grep -q 'OLD/' docs/MODS-REPLACED.md && ok "mod inventory documents OLD/" || bad "mod inventory does not document OLD/"
  grep -q 'tidy-game.sh' README.md && ok "README documents the tidy step" || bad "README does not document the tidy step"
  [ -x scripts/shot.sh ] && ok "shot.sh is executable" || bad "scripts/shot.sh missing or not executable"
  grep -q 'PrintScreen' scripts/shot.sh && ok "shot.sh explains why the game's own screenshot is avoided" || bad "shot.sh does not explain the crash path"
  grep -qx 'screenshots/' .gitignore && ok "captured shots stay out of git" || bad "screenshots/ is not ignored"
  grep -q 'shot.sh' README.md && ok "README points at the crash-free capture path" || bad "README does not mention shot.sh"
  grep -q 'shot.sh' docs/FINDINGS.md && ok "findings record the capture workaround" || bad "findings do not record the capture workaround"
fi
if [ "$P" = p07 ] || [[ "$P" > p07 ]]; then
  [ -f plugin/lua/SARemix_Shot.lua ] && ok "in-game hotkey script is in the repo" || bad "plugin/lua/SARemix_Shot.lua missing"
  grep -q 'isPs2KeyboardKeyJustPressed' plugin/lua/SARemix_Shot.lua && ok "hotkey reads the game's own key state (no game addresses)" || bad "hotkey does not use MoonLoader's key state API"
  grep -q 'screenshots/pending' plugin/lua/SARemix_Shot.lua && ok "hotkey only writes a marker file" || bad "hotkey does not write the pending marker"
  grep -v '^\s*--' plugin/lua/SARemix_Shot.lua | grep -qE 'd3d9|D3D9|PrintScreen' && bad "hotkey code touches D3D9" || ok "hotkey stays out of D3D9 (comments only)"
  [ -f config/SARemix_Shot.conf ] && grep -q '^key=' config/SARemix_Shot.conf && ok "hotkey key is configurable" || bad "config/SARemix_Shot.conf missing key="
  [ -x scripts/shot-watch.sh ] && ok "shot-watch.sh is executable" || bad "scripts/shot-watch.sh missing or not executable"
  grep -q 'shot.sh' scripts/shot-watch.sh && grep -q 'pending' scripts/shot-watch.sh && ok "watcher turns markers into window grabs" || bad "watcher does not reuse shot.sh"
  grep -q -- '--trigger' scripts/shot.sh && ok "shot.sh can fire the same marker from a terminal" || bad "shot.sh has no --trigger"
  grep -q 'shot-watch.sh' launcher/launch-rtx.sh && ok "launcher starts the watcher" || bad "launcher does not start the watcher"
  grep -q 'kill "$WATCHER"' launcher/launch-rtx.sh && ok "launcher stops the watcher on exit" || bad "launcher would leave the watcher running"
  grep -q 'SARemix_Shot.lua' deploy.sh && grep -q -- '--shotkey-off' deploy.sh && ok "deploy installs and can disable the hotkey" || bad "deploy does not manage the hotkey"
  grep -q 'screenshots/pending' deploy.sh && ok "deploy creates the marker folder the Lua needs" || bad "deploy does not create screenshots/pending"
  grep -q 'F12' README.md && ok "README documents the hotkey" || bad "README does not document F12"
  grep -q 'SARemix_Shot' docs/MODS-REPLACED.md && ok "mod inventory explains the one Lua script kept" || bad "mod inventory does not mention the kept Lua script"
fi
if [ "$P" = p08 ] || [[ "$P" > p08 ]]; then
  grep -q "rhyFlushDelayedTag" plugin/src/delayed.h && grep -q "rhyFlushDelayedTag" plugin/src/streetlights.cpp && grep -q "rhyFlushDelayedTag" plugin/src/trafficlights.cpp && ok "a retired light's destroy lands before its replacement is created" || bad "destroy-after-create still possible (same hash, new light dies)"
  grep -q "kTagStreet" plugin/src/delayed.h && grep -q "kTagTraffic" plugin/src/trafficlights.cpp && ok "destroy tags are namespaced per module" || bad "destroy tags are not namespaced"
  grep -q "exit_distance" plugin/src/config.h && grep -q "exit_distance" plugin/src/streetlights.cpp && grep -q "sticky_extra" plugin/src/streetlights.cpp && ok "street light selection has hysteresis" || bad "street lights still pop in and out at one radius"
  grep -q "traffic_exit_distance" plugin/src/trafficlights.cpp && grep -q "traffic_phase_near" plugin/src/trafficlights.cpp && ok "traffic lights: hysteresis plus phase_near" || bad "traffic lights recreate every light on every phase change"
  grep -q "int\* reason" plugin/src/remix.h && grep -q "reason == 2" plugin/src/streetlights.cpp && grep -q "reason == 2" plugin/src/trafficlights.cpp && ok "a cap refusal does not disable a module" || bad "a refused slot still switches the lights off for the session"
  grep -q "max_total_lights=0" plugin/src/config.cpp && ok "no global light cap by default" || bad "the global cap is still on by default"
  python3 - config/rhyremix-test.ini <<'PY' && ok "test ini: two radii, no cap, generous count" || bad "test ini still caps or starves the lights"
import configparser, sys
c = configparser.ConfigParser(comment_prefixes=(";", "#"))
c.read(sys.argv[1])
s, t = c["streetlights"], c["trafficlights"]
sys.exit(0 if (int(s["exit_distance"]) > int(s["max_distance"])
            and int(s["max_lights"]) >= 120 and int(s["max_total_lights"]) == 0
            and int(s["sticky_extra"]) > 0
            and int(t["exit_distance"]) > int(t["max_distance"])
            and int(t["phase_near"]) > 0) else 1)
PY
  d=$(awk '!/^#/{k=$2" "$3" "$4; if (seen[k]++) c++} END{print c+0}' build/gate-lights.txt 2>/dev/null); [ "${d:-1}" = 0 ] && ok "no duplicate light positions" || bad "${d:-?} duplicate light positions burn light slots"
  n=$(grep -vc '^#' build/gate-lights.txt 2>/dev/null || echo 0); [ "$n" -ge 6500 ] && ok "$n lamps after dedup" || bad "only $n lamps after dedup"
  grep -q "parse_ide_from_text" tools/lamp_list.py && ok "lamp model names resolve through .ide, not the img index" || bad "lamp_list.py still guesses model ids from the img index"
  h=$(awk '!/^#/{print $4}' build/gate-traffic.txt 2>/dev/null | sort -u | wc -l); [ "${h:-0}" -ge 2 ] && ok "traffic head heights come from the lamp table ($h distinct)" || bad "every traffic light head is the same radius guess"
  grep -q "report-missing" tools/lamp_list.py && ok "the lamp table's gaps are reported, not silent" || bad "missing lamp models stay invisible"
  grep -q "lamp_list.py" deploy.sh && grep -q "traffic_list.py" deploy.sh && ok "deploy generates the lists, so a fresh clone can install" || bad "the lists are generated files that nothing generates"
  grep -qi "blink" README.md && ok "README explains the blink" || bad "README does not explain the blink"
  grep -qi "max_total_lights" docs/FINDINGS.md && ok "findings record the cap that caused it" || bad "findings do not record the cap"
fi
if [ "$P" = p09 ] || [[ "$P" > p09 ]]; then
  # s28: lighting must never change instantly. The assertions here are about the
  # SHAPE of the code (a fade, a rate limit, a per-frame budget), because the
  # behaviour itself is asserted numerically in tests/smooth_check.cpp, which
  # check-all.sh compiles and runs.
  grep -q "src/smooth.cpp" plugin/CMakeLists.txt && ok "smooth.cpp is part of the plugin" || bad "smooth.cpp is not built"
  [ -f tests/smooth_check.cpp ] && ok "the no-pop bounds have a native test" || bad "tests/smooth_check.cpp missing"
  grep -q "rhyFadeAdvance" plugin/src/streetlights.cpp && grep -q "rhyFadeApply" plugin/src/streetlights.cpp && grep -q "rhySelectLights" plugin/src/streetlights.cpp && ok "street lamps fade up and down (gain comes from the selection)" || bad "street lamps still appear and vanish at full radiance"
  ! grep -q "void destroyAll" plugin/src/streetlights.cpp && ! grep -q "void destroyAll" plugin/src/trafficlights.cpp && ok "no path destroys a whole light layer in one frame" || bad "a destroy-everything path is still reachable"
  grep -q "rhyFadeSetRamp" plugin/src/trafficlights.cpp && grep -q "phase_fade_ms" plugin/src/config.cpp && ok "a signal colour change cross-fades between two hashes" || bad "signal colours still jump in one frame"
  grep -q "rhyRateStep" plugin/src/sun.cpp && grep -q "rhyDirLimit" plugin/src/sun.cpp && ok "sun radiance, sun direction and sky brightness are rate limited" || bad "sun/sky values are still published raw"
  grep -q "rhyBeginFrame" plugin/src/main.cpp && grep -q "rhyCreateBudgetLeft" plugin/src/streetlights.cpp && grep -q "rhySpendCreateSlot" plugin/src/streetlights.cpp && ok "handle changes are budgeted per frame, destroys included" || bad "a whole layer can still change inside one frame"
  grep -q "rhyPlayerPosGrace" plugin/src/game.cpp && grep -q "rhyPlayerPosGrace" plugin/src/streetlights.cpp && grep -q "rhyPlayerPosGrace" plugin/src/trafficlights.cpp && ok "a lost player ped holds position, then fades the layers out" || bad "losing the player pointer still destroys every light"
  grep -q "fade_in_ms < 500" plugin/src/config.cpp && grep -q "fade_out_ms < 500" plugin/src/config.cpp && ok "rhyremix.ini cannot ask for a sub-half-second fade" || bad "the ini can still request an instant switch"
  grep -q "\[fade\]" plugin/src/config.cpp && ok "the fade knobs live in rhyremix.ini" || bad "no [fade] section in the ini defaults"
  grep -qi "fade" README.md && ok "README documents the fades" || bad "README does not mention them"
  grep -qi "instantly" docs/FINDINGS.md && ok "findings record the instant-change diagnosis" || bad "findings do not record it"
fi
done_
