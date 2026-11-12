#!/bin/bash
# End-to-end dry run of the installer against a FAKE GTASA-RTX tree.
#
#   usage: bash scripts/installer-selftest.sh [--fake-only] [--keep]
#
#     --fake-only  only (re)build build/tmp/fakegame, do not run the installers
#     --keep       keep build/tmp/fakegame after the run instead of removing it
#
# It rebuilds a minimal fake install under build/tmp/fakegame from the real game
# folder (read-only: nothing here ever writes to /media/rhy/data/Gamez/GTASA-RTX),
# runs apply.sh and deploy.sh against it twice, and checks the result:
# patch applied, Large Address Aware set on a fake 32-bit PE, the pack's lamp
# lights stripped, launcher installed, backups made once, second run a no-op.
# Ends with "ALL PASS" or "N FAILED".
set -uo pipefail
REPO="$(cd "$(dirname "$0")/.." && pwd)"
GAME="${RHYS_GAME_DIR:-/media/rhy/data/Gamez/GTASA-RTX}"
FAKE="$REPO/build/tmp/fakegame"
OUT="$REPO/build/tmp/installer-selftest"
PATCH="$REPO/patches/gtasa-remix-linux.patch"
MARKER=".fake-install-marker"
FAKE_ONLY=0; KEEP=0
for a in "$@"; do case "$a" in --fake-only) FAKE_ONLY=1 ;; --keep) KEEP=1 ;; *) echo "usage: $0 [--fake-only] [--keep]"; exit 2 ;; esac; done
mkdir -p "$REPO/build/tmp"
rm -rf "$OUT"; mkdir -p "$OUT"
pass=0; fail=0
ok()  { pass=$((pass+1)); echo "PASS  $*"; }
bad() { fail=$((fail+1)); echo "FAIL  $*"; }
chk() { if [ "$1" = 0 ]; then shift; ok "$*"; else shift; bad "$*"; fi; }   # chk RC "desc" -> RC already compared
yes() { [ "$1" = 0 ] && ok "$2" || bad "$2"; }

# ---------------------------------------------------------------- fake tree --
# Only the files apply.sh/deploy.sh read or patch. Text/usda/conf are copied for
# real (~2.5 MB); the big binaries are placeholders of the right name, and
# gta-sa.exe is a generated minimal PE32 i386 image so tools/laa.py accepts it.
# The copied patch targets are then reverse-patched, which turns the tree into a
# PRE-install GTASA-Remix: that is what a user has when they run apply.sh.
build_fake() {
  rm -rf "$FAKE"
  mkdir -p "$FAKE"/moonloader/lib "$FAKE"/scripts "$FAKE"/.trex \
           "$FAKE"/modloader/MixSets "$FAKE"/rtx-remix/mods/gameReadyAssets
  # every file the patch touches
  grep -h '^+++ ' "$PATCH" | sed -E 's/^\+\+\+ "?b\///; s/"?\t.*$//; s/"$//' > "$OUT/patch-files.txt"
  while IFS= read -r f; do
    src="$GAME/$f"
    case "$f" in
      # the live rtx.conf has hand knobs on top of the patch; rtx.conf.orig is the
      # shipped file with only the patch on it, so it reverse-patches cleanly
      rtx.conf) src="$GAME/rtx.conf.orig" ;;
      # the live lamp file has had its SphereLights stripped; this backup copy is
      # the only one in the install that still has them, and it keeps the drift
      # (12 of its 47 hunks no longer match the patch) so the dry run surfaces it
      rtx-remix/mods/gameReadyAssets/SARemix/SARemix_StreetLights.usda)
        src="$GAME/rtx-remix/mods/gameReadyAssets/SARemix/SARemix_StreetLights.usda.orig-with-lights" ;;
    esac
    if [ -f "$src" ]; then
      mkdir -p "$FAKE/$(dirname "$f")"; cp -p "$src" "$FAKE/$f"
    else
      case "$f" in
        moonloader/SARemiv_LightMan.lua|moonloader/SARemix_Sun.lua) : ;;  # parked as .dat under OLD/, copied below
        *) echo "   no source for $f" ;;
      esac
    fi
  done < "$OUT/patch-files.txt"
  # the two Lua scripts tidy-game.sh parked under OLD/ - a fresh install has them
  # as .lua in moonloader/
  for n in SARemiv_LightMan SARemix_Sun; do
    [ -f "$FAKE/moonloader/$n.lua" ] && continue
    for p in "$GAME"/OLD/*/moonloader/"$n".dat "$GAME"/moonloader/"$n".dat; do
      [ -f "$p" ] || continue; cp -p "$p" "$FAKE/moonloader/$n.lua"; break
    done
  done
  # other files the installers read or rename
  cp -p "$GAME/modloader/MixSets/MixSets.ini" "$FAKE/modloader/MixSets/" 2>/dev/null
  cp -p "$GAME/moonloader/SARemix_Real_Sun.dat" "$FAKE/moonloader/" 2>/dev/null
  # a fresh GTASA-Remix copy still ships the stale VTK script (wrong model ids);
  # apply.sh moves it out of moonloader/
  printf -- '-- stale upstream copy: pre-cloud model ids, replaced by SARemix_VTK.lua\n' > "$FAKE/moonloader/SARemiv_VTK.lua"
  # big binaries: placeholders of the right name
  : > "$FAKE/d3d9.dll"; : > "$FAKE/MoonLoader.asi"; : > "$FAKE/modloader.asi"; : > "$FAKE/lua51.dll"
  # gta-sa.exe: a real (tiny) 32-bit PE, characteristics 0x010f = NOT Large Address
  # Aware, exactly what the downgrader gives the user
  python3 - "$FAKE/gta-sa.exe" <<'PY'
import struct, sys
lfanew, optsize = 0x100, 0xE0          # e_lfanew: where "PE\0\0" sits, as in a real exe
dos = bytearray(lfanew)
dos[0:2] = b"MZ"
struct.pack_into("<I", dos, 0x3C, lfanew)
coff = struct.pack("<HHIIIHH", 0x14C, 0, 0, 0, 0, optsize, 0x010F)  # i386, not LAA
opt = bytearray(optsize)
struct.pack_into("<H", opt, 0, 0x10B)      # PE32 (not PE32+)
struct.pack_into("<I", opt, 16, 0x1000)    # AddressOfEntryPoint
struct.pack_into("<I", opt, 28, 0x400000)  # ImageBase
struct.pack_into("<I", opt, 32, 0x1000)    # SizeOfImage
struct.pack_into("<H", opt, 68, 2)         # Subsystem: GUI
open(sys.argv[1], "wb").write(bytes(dos) + b"PE\0\0" + coff + bytes(opt))
PY
  chmod +x "$FAKE/gta-sa.exe"
  # turn the copied (post-patch) files back into the pre-patch state a user has
  mkdir -p "$OUT/chunks"
  awk -v out="$OUT/chunks" '/^diff -ruN /{n++; f=sprintf("%s/%03d.patch", out, n)} {if (f) print > f}' "$PATCH"
  local rev=0 kept=0
  for c in "$OUT"/chunks/*.patch; do
    f=$(grep -m1 '^+++ ' "$c" | sed -E 's/^\+\+\+ "?b\///; s/"?\t.*$//; s/"$//')
    [ -f "$FAKE/$f" ] || continue
    if patch -p1 -d "$FAKE" --dry-run -R --batch --binary < "$c" >/dev/null 2>&1; then
      patch -p1 -d "$FAKE" -R --batch --binary < "$c" >/dev/null && rev=$((rev+1))
    else
      kept=$((kept+1)); echo "   left as-is (drifted since the patch was made): $f"
    fi
  done
  echo "   fake tree: $(find "$FAKE" -type f | wc -l) files, $(du -sh "$FAKE" | cut -f1), $rev reverse-patched to pre-install state, $kept drifted"
  printf 'fake install - scripts/installer-selftest.sh scratch, never a real game folder\n' > "$FAKE/$MARKER"
}

# ------------------------------------------------------------------- checks --
nfiles() { find "$FAKE" -name "$1" | wc -l; }
manifest() { (cd "$FAKE" && find . -type f -print0 | sort -z | xargs -0 sha256sum); }
sphere() { local n; n=$(grep -c 'def SphereLight' "$1" 2>/dev/null); echo "${n:-0}"; }
# deploy.sh renames the Lua mods it disables to .dat, so a check that wants the
# patched text has to look at whichever name it has now
lua() { local f="$FAKE/moonloader/$1"; [ -f "$f" ] || f="${f%.lua}.dat"; cat "$f" 2>/dev/null; }
laa_chars() { python3 -c 'import struct,sys;d=open(sys.argv[1],"rb").read();o=struct.unpack_from("<I",d,0x3C)[0];print(hex(struct.unpack_from("<H",d,o+22)[0]))' "$1"; }

echo "== building the fake install in $FAKE"
build_fake
[ "$FAKE_ONLY" = 1 ] && exit 0
export RHYREMIX_INSTALLER_SANDBOX=1

# the real game folder must come back untouched (read-only for us)
snap_game() { find "$GAME" -maxdepth 1 -printf '%p %y %s %T@\n' 2>/dev/null | sort; }
snap_game > "$OUT/game-before.txt"

run() { # run <label> <cmd...>
  local label="$1"; shift
  echo "-- $label"
  ( "$@" ) > "$OUT/$label.log" 2>&1
  local rc=$?
  echo "   rc=$rc (log: ${OUT#"$REPO"/}/$label.log)"
  return $rc
}

echo "== run 1"
run apply1 bash "$REPO/apply.sh" "$FAKE"; yes $? "apply.sh run 1 exits 0"
run deploy1 bash "$REPO/deploy.sh" "$FAKE"; yes $? "deploy.sh run 1 exits 0"
manifest > "$OUT/manifest-1.txt"

echo "== run 2 (must be a no-op)"
run apply2 bash "$REPO/apply.sh" "$FAKE"; yes $? "apply.sh run 2 exits 0"
run deploy2 bash "$REPO/deploy.sh" "$FAKE"; yes $? "deploy.sh run 2 exits 0"
manifest > "$OUT/manifest-2.txt"

echo "== the patch"
grep -q 'RTX fix' <<<"$(lua SARemiv_LightMan.lua)"; chk $? "moonloader/SARemiv_LightMan.lua carries the patch (RTX fix)"
grep -q 'id = 15897' <<<"$(lua SARemiv_LightMan.lua)"; chk $? "the stale lamp model ids were shifted +5 (15892 -> 15897)"
grep -q 'isPlayerPlaying(PLAYER_HANDLE)' <<<"$(lua SARemix_Sun.lua)"; chk $? "moonloader/SARemix_Sun.lua carries the patch (waits for CJ)"
grep -q 'rtx.useNewGuiInputMethod = False' "$FAKE/rtx.conf"; chk $? "rtx.conf got the patch (useNewGuiInputMethod = False)"
grep -q 'inputs:shaping:focus = 0' "$FAKE/rtx-remix/mods/gameReadyAssets/Light/StreetLight.usda"; chk $? "a Light/*.usda from the patch was rewritten"
[ -z "$(find "$FAKE" \( -name '*.rej' -o -name '*.orig' \) -print -quit)" ]; chk $? "no .rej / .orig garbage left in the game folder"
grep -q 'WARN.*SARemix/SARemix_StreetLights.usda' "$OUT/apply1.log"; chk $? "the one drifted file is reported as a warning, not an abort"

echo "== Large Address Aware"
python3 "$REPO/tools/laa.py" "$FAKE/gta-sa.exe" --check >/dev/null 2>&1; chk $? "tools/laa.py --check says the fake gta-sa.exe is LAA"
[ "$(laa_chars "$FAKE/gta-sa.exe")" = "0x12f" ]; chk $? "characteristics 0x010f -> 0x012f (only the LAA bit changed)"
[ "$(nfiles 'gta-sa.exe.noLAA-backup')" = 1 ]; chk $? "exactly one gta-sa.exe.noLAA-backup after two runs"
[ "$(laa_chars "$FAKE/gta-sa.exe.noLAA-backup")" = "0x10f" ]; chk $? "that backup still holds the pristine non-LAA exe"
grep -q 'not Large Address Aware' "$OUT/deploy1.log" && bad "deploy.sh warned about a missing LAA flag it should not have" || ok "deploy.sh did not warn: apply.sh had already set the flag"

echo "== pack lamp lights"
s="$FAKE/rtx-remix/mods/gameReadyAssets/SARemix/SARemix_StreetLights.usda"
[ "$(sphere "$s")" = 0 ]; chk $? "the pack's SphereLights are gone from SARemix/SARemix_StreetLights.usda (were 18)"
[ "$(sphere "$s.orig-with-lights")" = 18 ]; chk $? "the pre-strip copy keeps all 18 SphereLights"
[ "$(nfiles '*.orig-with-lights')" = 1 ]; chk $? "exactly one pre-strip backup after two runs"
grep -q 'already removed' "$OUT/apply2.log"; chk $? "the second run reports the strip step as already done"

echo "== launcher and the rest of the install"
[ -x "$FAKE/launch-rtx.sh" ]; chk $? "launch-rtx.sh installed and executable"
cmp -s "$REPO/launcher/launch-rtx.sh" "$FAKE/launch-rtx.sh"; chk $? "it is byte-identical to launcher/launch-rtx.sh"
cmp -s "$REPO/config/bridge.conf" "$FAKE/.trex/bridge.conf"; chk $? ".trex/bridge.conf installed"
for m in sa_ped_safe.lua SARemix_DebugPrinter.lua; do [ -f "$FAKE/moonloader/lib/$m" ]; chk $? "moonloader/lib/$m installed"; done
grep -qE '^UseHighPedShadows[ ]*=[ ]*0' "$FAKE/modloader/MixSets/MixSets.ini"; chk $? "MixSets UseHighPedShadows set to 0 (no gray square under CJ)"
[ ! -f "$FAKE/moonloader/SARemiv_VTK.lua" ] && [ -f "$FAKE/rhYgtaSA-backup/moonloader/SARemiv_VTK.lua" ]; chk $? "the stale SARemiv_VTK.lua moved to the backup, not deleted"
[ -f "$FAKE/scripts/rhyremix.asi" ] && cmp -s "$REPO/build/rhyremix.asi" "$FAKE/scripts/rhyremix.asi"; chk $? "scripts/rhyremix.asi == build/rhyremix.asi"
for f in rhyremix.ini rhyremix_lights.txt rhyremix_traffic.txt rhyremix-rtx.conf; do [ -f "$FAKE/scripts/$f" ]; chk $? "scripts/$f installed"; done
[ -f "$FAKE/moonloader/SARemiv_LightMan.dat" ] && [ ! -f "$FAKE/moonloader/SARemiv_LightMan.lua" ]; chk $? "the Lua light manager is disabled (.dat) - our plugin lights the lamps"
[ -f "$FAKE/moonloader/SARemix_Sun.dat" ] && [ ! -f "$FAKE/moonloader/SARemix_Sun.lua" ]; chk $? "the Lua sun mod is disabled (.dat) - our plugin owns the sun"
[ -f "$FAKE/moonloader/SARemix_Shot.lua" ] && [ -f "$FAKE/moonloader/SARemix_Shot.conf" ]; chk $? "the F12 screenshot hotkey is installed"
[ -d "$FAKE/screenshots/pending" ] && [ -d "$FAKE/screenshots/shots" ]; chk $? "screenshots/pending and screenshots/shots exist"

echo "== backups created once"
[ "$(find "$FAKE" -maxdepth 1 -name 'rhYgtaSA-backup*' -type d | wc -l)" = 1 ]; chk $? "exactly one rhYgtaSA-backup dir after two runs"
grep -q 'id = 15892' "$FAKE/rhYgtaSA-backup/moonloader/SARemiv_LightMan.lua" && ! grep -q 'RTX fix' "$FAKE/rhYgtaSA-backup/moonloader/SARemiv_LightMan.lua"; chk $? "the backup holds the PRE-patch script, not the patched one"
! grep -q 'rtx.useNewGuiInputMethod' "$FAKE/rhYgtaSA-backup/rtx.conf"; chk $? "the backup rtx.conf is the pre-patch one"
missing=0
while IFS= read -r f; do [ -f "$FAKE/rhYgtaSA-backup/$f" ] || { echo "   not backed up: $f"; missing=$((missing+1)); }; done < "$OUT/patch-files.txt"
[ "$missing" = 0 ]; chk $? "every file the patch touches is backed up ($(grep -c . "$OUT/patch-files.txt") files)"

echo "== the sandbox guard (a broken selftest can never hit a real install)"
mkdir -p "$OUT/no-marker"/scripts "$OUT/no-marker"/moonloader "$OUT/no-marker"/rtx-remix/mods/gameReadyAssets
: > "$OUT/no-marker/rtx.conf"
if RHYREMIX_INSTALLER_SANDBOX=1 bash "$REPO/apply.sh" "$OUT/no-marker" >/dev/null 2>&1; then
  bad "apply.sh ran against a marker-less folder in sandbox mode"
else
  ok "apply.sh refuses a folder without .fake-install-marker in sandbox mode"
fi
if RHYREMIX_INSTALLER_SANDBOX=1 bash "$REPO/deploy.sh" "$OUT/no-marker" >/dev/null 2>&1; then
  bad "deploy.sh ran against a marker-less folder in sandbox mode"
else
  ok "deploy.sh refuses a folder without .fake-install-marker in sandbox mode"
fi
rm -rf "$OUT/no-marker"

echo "== idempotence"
cmp -s "$OUT/manifest-1.txt" "$OUT/manifest-2.txt"; chk $? "run 2 changed no file in the game folder ($(wc -l < "$OUT/manifest-1.txt") files hashed)"
diff "$OUT/manifest-1.txt" "$OUT/manifest-2.txt" | head -5
snap_game > "$OUT/game-after.txt"
cmp -s "$OUT/game-before.txt" "$OUT/game-after.txt"; chk $? "$GAME untouched (sizes and mtimes identical)"

[ "$KEEP" = 1 ] || rm -rf "$FAKE"
echo
echo "$pass checks passed, $fail failed"
[ "$fail" = 0 ] && echo "ALL PASS" || echo "$fail FAILED"
exit $((fail > 0))
