#!/bin/bash
# Install the freshly built plugin into the GTASA-RTX install so the game runs our code.
# Usage: ./deploy.sh [--build] [--prod|--test] [--force-ini] [--lua-off|--lua-on] [--no-launcher] [GAME_DIR]
#   --build       run plugin/build.sh first (also done automatically when build/ is empty)
#   --prod        install config/rhyremix-production.ini (the shipped lighting
#                 profile: everything on, physically-grounded sun/moon/sky).
#                 This is the default when there is no rhyremix.ini yet.
#   --test        install config/rhyremix-test.ini (bisect ladder: probe + moon on,
#                 streetlights and traffic lights off, lights on at every hour)
#   --force-ini   overwrite an existing rhyremix.ini with whichever ini was chosen
#   --lua-off     rename moonloader/SARemiv_LightMan.lua -> .dat (disable the Lua light manager)
#   --lua-on      undo that rename
#                 (default: auto - disabled exactly when [streetlights] enabled=1)
#   --sun-off     rename moonloader/SARemix_Sun.lua -> .dat (disable the Lua sun mod)
#   --sun-on      undo that rename
#                 (default: auto - the Lua sun is disabled exactly when the ini's
#                 [sun] enabled=1, because two suns stacked is the "too bright" look)
#   --shotkey-off disable moonloader/SARemix_Shot.lua (the in-game F12 screenshot
#                 hotkey); --shotkey-on installs/refreshes it. Default: on.
#   moonloader/SARemix_Tour.lua (the photo tour, n03) is refreshed on EVERY run and is
#                 NOT covered by --shotkey-off: it only acts when screenshots/tour.req
#                 exists, and a stale copy silently drops tour stops (see below).
#   --no-launcher do not refresh launch-rtx.sh in the game folder (used by the launcher itself)
#   --aftermath   add devk.enableAftermath = True to <game>/dvk.conf (names the shader on a GPU crash)
#   --rtx-probe   write rhyremix-rtx.conf with the candidate rtx.* key list and set
#                 [sky] probe=1, so one run reports which rtx.* settings this Remix
#                 build actually accepts instead of silently ignoring a wrong name
#   --rtx-conf    install config/rtx.conf (conservative: the two measured changes)
#   --rtx-conf-exp install config/rtx.conf.experimental (adds six unobserved ones)
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
R="/media/rhy/data/Gamez/GTASA-RTX"
TEST=0; PROD=0; FORCE_INI=0; BUILD=0; LUA=auto; SUN=auto; LAUNCHER=1; AFTERMATH=0; SHOTKEY=on; RTXPROBE=0; RTXCONF=0
for a in "$@"; do
  case "$a" in
    --build) BUILD=1 ;;
    --test) TEST=1; PROD=0 ;;
    --prod) PROD=1; TEST=0 ;;
    --force-ini) FORCE_INI=1 ;;
    --lua-off) LUA=off ;;
    --lua-on) LUA=on ;;
    --sun-off) SUN=off ;;
    --sun-on) SUN=on ;;
    --shotkey-off) SHOTKEY=off ;;
    --shotkey-on) SHOTKEY=on ;;
    --no-launcher) LAUNCHER=0 ;;
    --aftermath) AFTERMATH=1 ;;
    --rtx-probe) RTXPROBE=1 ;;
    --rtx-conf) RTXCONF=1 ;;
    --rtx-conf-exp) RTXCONF=2 ;;
    *) R="$a" ;;
  esac
done
[ -f "$R/rtx.conf" ] && [ -d "$R/scripts" ] || { echo "not a GTASA-RTX install: $R"; exit 1; }
# Selftest guard (see apply.sh): scripts/installer-selftest.sh runs this against a
# scratch tree with a marker in it, and this refuses anything else in that mode -
# deploy.sh otherwise defaults to the real game folder.
if [ "${RHYREMIX_INSTALLER_SANDBOX:-}" = 1 ] && [ ! -f "$R/.fake-install-marker" ]; then
  echo "refusing to deploy into $R: sandbox mode needs $R/.fake-install-marker"; exit 1
fi
# Large Address Aware check - warn only. deploy.sh runs on every game start (via
# launch-rtx.sh) and must not rewrite the exe there; apply.sh is what sets the
# flag. Without it the 32-bit process gets ~2 GB of address space for the game,
# the Remix bridge client and a 1900 MB streaming cap at once (docs/VEGETATION.md
# cause 2, the V Vegetation Mix freeze), so it is worth shouting about.
if [ -f "$R/gta-sa.exe" ]; then
  python3 "$HERE/tools/laa.py" "$R/gta-sa.exe" --check \
    || echo "WARN: $R/gta-sa.exe is not Large Address Aware - fix it with: ./apply.sh $R   (or python3 $HERE/tools/laa.py $R/gta-sa.exe --set)"
else
  echo "WARN: no $R/gta-sa.exe in $R - cannot check Large Address Aware"
fi
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
# The lists are generated from the game's own data, so they are not in git: a
# fresh clone generates them here instead of failing on a missing file. Since n11
# the street list comes from the 2dFx LIGHT records inside gta3.img's .dff placed
# by the .ipl files (tools/dff_2dfx.py) - every bulb the GAME declares, 8962 of
# them, each with its own colour. The traffic list still comes from the .ipl files
# plus the mod pack's lamp table under <game>/moonloader/, which is where a signal
# head's offset above its pole lives.
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
# rhyremix lights the street lamps; the asset pack's own lamp SphereLights would
# light each one a second time (idempotent; --restore in the tool undoes it).
python3 "$HERE/tools/strip_pack_lamp_lights.py" "$R" || echo "WARN: could not remove the pack's duplicate lamp lights"

# The runtime rtx.* pushes. Kept as a separate file rather than baked into the
# ini because it is a list of renderer settings, not mod parameters, and because
# a wrong key is rejected by the bridge and logged - so it wants to be a file you
# can edit and re-run without touching the lighting parameters.
if [ -f "$HERE/config/rhyremix-rtx.conf" ]; then
  cp "$HERE/config/rhyremix-rtx.conf" "$R/scripts/rhyremix-rtx.conf"
  echo "== installed rhyremix-rtx.conf (renderer settings pushed at runtime)"
fi

# The renderer's own startup settings. rtx.conf is read once, before the first
# frame, and it is the only supported place for the settings that reconfigure a
# renderer subsystem - see config/rhyremix-rtx.conf for why pushing those at
# runtime took the bridge down once. Opt-in because rtx.conf also holds the
# per-install texture hash lists, and a user may have edited it by hand.
if [ "$RTXCONF" != 0 ]; then
  if [ -f "$R/rtx.conf" ] && [ ! -f "$R/rtx.conf.orig" ]; then
    cp "$R/rtx.conf" "$R/rtx.conf.orig"
    echo "== original rtx.conf saved as rtx.conf.orig"
  fi
  if [ "$RTXCONF" = 2 ]; then
    SRC="$HERE/config/rtx.conf.experimental"
    echo "== installed rtx.conf.experimental: the two conservative changes PLUS"
    echo "   autoExposure on (evMin -4), localtonemap.shadows 1.0, pathMaxBounces 3,"
    echo "   pathMinBounces 1. None of those six have ever been observed running. If"
    echo "   the image looks wrong, undo this first: ./deploy.sh --rtx-conf $R"
  else
    SRC="$HERE/config/rtx.conf"
    echo "== installed rtx.conf: exactly two changes from the shipped file -"
    echo "   emissiveIntensity 10 -> 1.0 (the asset pack's own emissives reach 3000,"
    echo "   so x10 rendered shopfronts at 30000) and skyBrightness 3 -> 1.0 (the sky"
    echo "   now comes from the solar model, not a static boost)"
  fi
  cp "$SRC" "$R/rtx.conf"
fi

INI="$R/scripts/rhyremix.ini"
if [ "$TEST" = 1 ] || [ "$PROD" = 1 ]; then
  SRC="$HERE/config/rhyremix-production.ini"
  LABEL="production"
  if [ "$TEST" = 1 ]; then SRC="$HERE/config/rhyremix-test.ini"; LABEL="test"; fi
  if [ -f "$INI" ] && [ "$FORCE_INI" != 1 ]; then
    echo "== keeping your existing rhyremix.ini (use --force-ini to replace it)"
  else
    cp "$SRC" "$INI"
    echo "== wrote $LABEL ini (physically-grounded sun/moon/sky, block_gamma=1)"
  fi
elif [ ! -f "$INI" ]; then
  # First run on a fresh install: ship the production profile rather than let the
  # plugin write its built-in defaults, because the production profile is the one
  # whose numbers were measured.
  cp "$HERE/config/rhyremix-production.ini" "$INI"
  echo "== no ini yet: wrote the production profile"
fi

if [ "$RTXPROBE" = 1 ]; then
  # Ask the bridge which rtx.* keys it knows. NOTE: this writes real values into
  # a live renderer, and the return code does not validate the key - the server
  # logs the truth. Read it with scripts/check-rtx-probe.sh.
  awk 'BEGIN{done=0} /^\[sky\]/{insec=1; print; next} /^\[/{if(insec&&!done){print "probe=1"; done=1} insec=0; print; next} {if(insec&&/^probe[ ]*=/){print "probe=1"; done=1; next} print} END{if(!done) print "probe=1"}' \
    "$INI" > "$INI.tmp" && mv "$INI.tmp" "$INI"
  echo "== [sky] probe=1: audit the result with scripts/check-rtx-probe.sh"
  echo "   (the client accepts every key; only remix-dxvk.log says which ones work)"
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
# The Lua light manager moves the game's own lamp meshes, which the Remix asset
# pack already turns into lights; with our API lights on top of that every lamp
# burns twice. So the default follows the ini instead of leaving it alone.
# NOTE the order: the decision has to be made before the case that acts on it.
# It used to sit below, so with the default (auto) neither branch matched and
# SARemiv_LightMan.lua stayed loaded next to our own lamps - the dry run in
# scripts/installer-selftest.sh is what caught that.
if [ "$LUA" = auto ]; then
  v=$(awk -F= 'BEGIN{v=1} /^\[/{s=($1=="[streetlights]")} s && /^enabled/{v=$2} END{print v+0}' "$INI" 2>/dev/null || echo 1)
  if [ "${v:-1}" = 1 ]; then LUA=off; else LUA=on; fi
  echo "== [streetlights] enabled=$v in $INI -> Lua light manager will be $LUA (override: --lua-on / --lua-off)"
fi
case "$LUA" in
  off) moon_off SARemiv_LightMan --lua-on; [ -f "$R/moonloader/SARemiv_LightMan.lua" ] || echo "== Lua light manager already disabled" ;;
  on)  moon_on SARemiv_LightMan; [ -f "$R/moonloader/SARemiv_LightMan.lua" ] || echo "== Lua light manager already active" ;;
esac
# Two suns at once (the Lua mod's sun object + our Remix distant light) is exactly
# the "everything is blown out at noon" symptom, so the default follows the ini
# instead of leaving the Lua mod alone.
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

# n03 (notes-s26 gap 1, follow-up 1): the photo tour is the other Lua script this repo ships, and
# until now NOTHING installed it. The only copy in a game folder was one a human had `cp`'d in by
# hand - which is what docs/NEXT-TEST.md step 0 asked for. That is how the 2026-11-12 00:49 session
# ran the stale 13-stop tour while the repo had 16 stops: stops 12/13/14 (mirror, ped crowd,
# vegetation) produced no shots at all, so two steps of the checklist had no numbers.
#
# Same source tree, same moonloader/ path and the same rules as the SARemix_Shot.lua copy above:
# the .lua is always the repo's bytes (--force-ini has only ever protected config files -
# SARemix_Shot.lua itself is copied unconditionally), and a launcher dry run (RHYS_DRY_RUN=1) runs
# deploy.sh, so it is installed there too. cmp -s first because deploy.sh runs on EVERY game start
# (launch-rtx.sh calls it) and must not rewrite a Lua file that MoonLoader may have loaded - the
# same reason launch-rtx.sh itself is only copied when it differs.
# Deliberately NOT tied to --shotkey-off: SARemix_Tour.lua only acts when
# <game>/screenshots/tour.req exists (its own header says so), so installing it cannot change a
# normal session, and a game folder that cannot run the tour cannot run the checklist either.
TOUR_SRC="$HERE/plugin/lua/SARemix_Tour.lua"
TOUR_DST="$R/moonloader/SARemix_Tour.lua"
if [ ! -f "$TOUR_SRC" ]; then
  echo "== WARN: $TOUR_SRC is missing - the tour script was NOT installed; the game keeps whatever stop list it has"
elif [ -f "$TOUR_DST" ] && cmp -s "$TOUR_SRC" "$TOUR_DST"; then
  echo "== tour: moonloader/SARemix_Tour.lua already current (sha256 $(sha256sum "$TOUR_DST" | cut -c1-16), $(wc -c < "$TOUR_DST") bytes)"
else
  cp "$TOUR_SRC" "$TOUR_DST"
  echo "== tour: plugin/lua/SARemix_Tour.lua -> moonloader/SARemix_Tour.lua (sha256 $(sha256sum "$TOUR_DST" | cut -c1-16), $(wc -c < "$TOUR_DST") bytes)"
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
ls -la "$R/scripts/rhyremix.asi" "$R/scripts/rhyremix_lights.txt" "$R/scripts/rhyremix_traffic.txt" \
       "$R/moonloader/SARemix_Tour.lua" 2>/dev/null || true
[ -f "$INI" ] && ls -la "$INI"
if [ -f "$HERE/build/rhyremix.asi" ] && command -v md5sum >/dev/null 2>&1; then
  # Two runs of the same log can look identical while a different build was
  # loaded; this is the one number that tells them apart.
  echo "== md5 build=$(md5sum "$HERE/build/rhyremix.asi" | cut -d' ' -f1) deployed=$(md5sum "$R/scripts/rhyremix.asi" | cut -d' ' -f1) (must match)"
fi
echo
echo "run:  $R/launch-rtx.sh"
echo "watch: tail -f $R/scripts/rhyremix.log"
