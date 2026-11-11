#!/bin/bash
# Install the freshly built plugin into the GTASA-RTX install so the game runs our code.
# Usage: ./deploy.sh [--build] [--test] [--force-ini] [--lua-off|--lua-on] [--no-launcher] [GAME_DIR]
#   --build       run plugin/build.sh first (also done automatically when build/ is empty)
#   --test        install config/rhyremix-test.ini (bisect ladder: probe + moon on,
#                 streetlights and traffic lights off, lights on at every hour)
#   --force-ini   overwrite an existing rhyremix.ini with that test ini
#   --lua-off     rename moonloader/SARemiv_LightMan.lua -> .dat (disable the Lua light manager)
#   --lua-on      undo that rename
#                 (default: auto - disabled exactly when [streetlights] enabled=1)
#   --sun-off     rename moonloader/SARemix_Sun.lua -> .dat (disable the Lua sun mod)
#   --sun-on      undo that rename
#                 (default: auto - the Lua sun is disabled exactly when the ini's
#                 [sun] enabled=1, because two suns stacked is the "too bright" look)
#   --shotkey-off disable moonloader/SARemix_Shot.lua (the in-game F12 screenshot
#                 hotkey); --shotkey-on installs/refreshes it. Default: on.
#   --no-launcher do not refresh launch-rtx.sh in the game folder (used by the launcher itself)
#   --aftermath   add devk.enableAftermath = True to <game>/dvk.conf (names the shader on a GPU crash)
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
R="/media/rhy/data/Gamez/GTASA-RTX"
TEST=0; FORCE_INI=0; BUILD=0; LUA=auto; SUN=auto; LAUNCHER=1; AFTERMATH=0; SHOTKEY=on
for a in "$@"; do
  case "$a" in
    --build) BUILD=1 ;;
    --test) TEST=1 ;;
    --force-ini) FORCE_INI=1 ;;
    --lua-off) LUA=off ;;
    --lua-on) LUA=on ;;
    --sun-off) SUN=off ;;
    --sun-on) SUN=on ;;
    --shotkey-off) SHOTKEY=off ;;
    --shotkey-on) SHOTKEY=on ;;
    --no-launcher) LAUNCHER=0 ;;
    --aftermath) AFTERMATH=1 ;;
    *) R="$a" ;;
  esac
done
[ -f "$R/rtx.conf" ] && [ -d "$R/scripts" ] || { echo "not a GTASA-RTX install: $R"; exit 1; }
ASI="$HERE/build/rhyremix.asi"
LIST="$HERE/build/rhyremix_lights.txt"
TLIST="$HERE/build/rhyremix_traffic.txt"
if [ "$BUILD" = 1 ] || [ ! -f "$ASI" ]; then
  echo "== building plugin"
  timeout 600 bash "$HERE/plugin/build.sh" >/dev/null || { echo "build failed"; exit 1; }
fi
if [ ! -f "$TLIST" ]; then
  echo "== generating traffic light list"
  timeout 600 python3 "$HERE/tools/traffic_list.py" --game "$R" --out "$TLIST" >/dev/null || { echo "traffic_list.py failed"; exit 1; }
fi
# The lists are generated from the game's own data (gta3.img + .ide + .ipl, and
# the mod pack's lamp table under <game>/moonloader/), so they are not in git:
# a fresh clone generates them here instead of failing on a missing file.
if [ ! -f "$LIST" ]; then
  echo "== generating street light list"
  timeout 900 python3 "$HERE/tools/lamp_list.py" --game "$R" --out "$LIST" >/dev/null || { echo "lamp_list.py failed"; exit 1; }
fi
[ -f "$ASI" ] && [ -f "$LIST" ] || { echo "missing $ASI or $LIST"; exit 1; }
file "$ASI" | grep -q 'PE32.*i386' || { echo "$ASI is not a 32-bit DLL, refusing"; exit 1; }

if [ -f "$R/scripts/rhyremix.asi" ] && ! cmp -s "$ASI" "$R/scripts/rhyremix.asi"; then
  B="$HERE/build/tmp/deploy-backup-$(date +%Y%m%d-%H%M%S)"
  mkdir -p "$B"
  cp -p "$R/scripts/rhyremix.asi" "$B/"
  echo "== previous asi saved to ${B#"$HERE"/}"
fi
cp "$ASI" "$R/scripts/rhyremix.asi"
cp "$LIST" "$R/scripts/rhyremix_lights.txt"
cp "$TLIST" "$R/scripts/rhyremix_traffic.txt"
echo "== installed rhyremix.asi + rhyremix_lights.txt + rhyremix_traffic.txt into $R/scripts"

INI="$R/scripts/rhyremix.ini"
if [ "$TEST" = 1 ]; then
  if [ -f "$INI" ] && [ "$FORCE_INI" != 1 ]; then
    echo "== keeping your existing rhyremix.ini (use --force-ini to replace it)"
  else
    cp "$HERE/config/rhyremix-test.ini" "$INI"
    echo "== wrote test ini (radius/radiance in game units, block_gamma=1, probe=1)"
  fi
elif [ ! -f "$INI" ]; then
  echo "== no ini yet: the plugin writes its own defaults on first run"
fi

# MoonLoader only loads moonloader/*.lua, so renaming to .dat disables a script
# without deleting it (GTASA-Remix itself ships its disabled scripts that way).
moon_off() {  # $1 = script base name, $2 = flag that re-enables it
  local l="$R/moonloader/$1.lua" d="$R/moonloader/$1.dat"
  if [ -f "$l" ]; then mv "$l" "$d"; echo "== $1.lua -> .dat (undo: ./deploy.sh $2)"; fi
}
moon_on() {
  local l="$R/moonloader/$1.lua" d="$R/moonloader/$1.dat"
  # tidy-game.sh may have parked the .dat under OLD/, so look there too.
  if [ ! -f "$d" ]; then
    for p in "$R"/OLD/*/moonloader/"$1".dat; do
      [ -f "$p" ] || continue
      mv "$p" "$d"
      echo "== restored $1.dat from OLD/ (parked by tidy-game.sh)"
      break
    done
  fi
  if [ -f "$d" ]; then mv "$d" "$l"; echo "== $1.dat -> .lua (Lua script active again)"; fi
}
case "$LUA" in
  off) moon_off SARemiv_LightMan --lua-on; [ -f "$R/moonloader/SARemiv_LightMan.lua" ] || echo "== Lua light manager already disabled" ;;
  on)  moon_on SARemiv_LightMan; [ -f "$R/moonloader/SARemiv_LightMan.lua" ] || echo "== Lua light manager already active" ;;
esac
# Two suns at once (the Lua mod's sun object + our Remix distant light) is exactly
# the "everything is blown out at noon" symptom, so the default follows the ini
# instead of leaving the Lua mod alone.
# The Lua light manager moves the game's own lamp meshes, which the Remix asset
# pack already turns into lights; with our API lights on top of that every lamp
# burns twice. So the default follows the ini instead of leaving it alone.
if [ "$LUA" = auto ]; then
  v=$(awk -F= 'BEGIN{v=1} /^\[/{s=($1=="[streetlights]")} s && /^enabled/{v=$2} END{print v+0}' "$INI" 2>/dev/null || echo 1)
  if [ "${v:-1}" = 1 ]; then LUA=off; else LUA=on; fi
  echo "== [streetlights] enabled=$v in $INI -> Lua light manager will be $LUA (override: --lua-on / --lua-off)"
fi
if [ "$SUN" = auto ]; then
  v=$(awk -F= 'BEGIN{v=1} /^\[/{s=($1=="[sun]")} s && /^enabled/{v=$2} END{print v+0}' "$INI" 2>/dev/null || echo 1)
  if [ "${v:-1}" = 1 ]; then SUN=off; else SUN=on; fi
  echo "== [sun] enabled=$v in $INI -> Lua sun will be $SUN (override: --sun-on / --sun-off)"
fi
case "$SUN" in
  off)
    # SARemix_Sun.lua is the current sun mod in the install; SARemix_Real_Sun.lua
    # is the older one, disabled here too in case it gets re-enabled.
    moon_off SARemix_Sun --sun-on
    moon_off SARemix_Real_Sun --sun-on
    [ -f "$R/moonloader/SARemix_Sun.lua" ] || echo "== Lua sun mod already disabled" ;;
  on)
    moon_on SARemix_Sun
    moon_on SARemix_Real_Sun
    [ -f "$R/moonloader/SARemix_Sun.lua" ] || echo "== Lua sun mod already active" ;;
esac

# The screenshot hotkey is the one Lua script this project keeps: only MoonLoader
# can read the game's key state, and it never touches D3D9 - it just drops a
# marker file that scripts/shot-watch.sh (started by launch-rtx.sh) turns into a
# window grab.
if [ "$SHOTKEY" = on ]; then
  cp "$HERE/plugin/lua/SARemix_Shot.lua" "$R/moonloader/SARemix_Shot.lua"
  if [ -f "$R/moonloader/SARemix_Shot.conf" ] && [ "$FORCE_INI" != 1 ]; then
    : # keep the user's key choice
  else
    cp "$HERE/config/SARemix_Shot.conf" "$R/moonloader/SARemix_Shot.conf"
  fi
  mkdir -p "$R/screenshots/pending" "$R/screenshots/shots"
  echo "== shot hotkey: F12 in game -> $R/screenshots/pending -> screenshots/shots/"
else
  moon_off SARemix_Shot --shotkey-on
fi

if [ "$AFTERMATH" = 1 ]; then
  D="$R/dvk.conf"
  if [ -f "$D" ] && grep -q 'devk.enableAftermath' "$D"; then
    echo "== dvk.conf already has devk.enableAftermath"
  else
    if [ -f "$D" ]; then
      cp "$D" "$D.bak"
      { cat "$D"; echo 'devk.enableAftermath = True'; } > "$D.tmp"
    else
      echo 'devk.enableAftermath = True' > "$D.tmp"
    fi
    mv "$D.tmp" "$D"
    echo "== dvk.conf: devk.enableAftermath = True (GPU crashes will name the shader)"
  fi
fi

if [ "$LAUNCHER" = 1 ]; then
  # Only when the content differs: never rewrite a script that is currently running.
  if [ -f "$HERE/launcher/launch-rtx.sh" ] && ! cmp -s "$HERE/launcher/launch-rtx.sh" "$R/launch-rtx.sh"; then
    cp "$HERE/launcher/launch-rtx.sh" "$R/launch-rtx.sh"
    chmod +x "$R/launch-rtx.sh"
    echo "== refreshed launch-rtx.sh in the game folder"
  fi
fi

echo "== deployed:"
ls -la "$R/scripts/rhyremix.asi" "$R/scripts/rhyremix_lights.txt" "$R/scripts/rhyremix_traffic.txt"
[ -f "$INI" ] && ls -la "$INI"
if [ -f "$HERE/build/rhyremix.asi" ] && command -v md5sum >/dev/null 2>&1; then
  # Two runs of the same log can look identical while a different build was
  # loaded; this is the one number that tells them apart.
  echo "== md5 build=$(md5sum "$HERE/build/rhyremix.asi" | cut -d' ' -f1) deployed=$(md5sum "$R/scripts/rhyremix.asi" | cut -d' ' -f1) (must match)"
fi
echo
echo "run:  $R/launch-rtx.sh"
echo "watch: tail -f $R/scripts/rhyremix.log"
