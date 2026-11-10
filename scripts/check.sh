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
  grep -q "remixapi_InitializeLibrary" plugin/src/*.cpp && ok "loads Remix API" || bad "remixapi_InitializeLibrary not referenced"
  grep -q "DrawLightInstance" plugin/src/*.cpp && ok "draws a light" || bad "DrawLightInstance not used"
fi
done_
