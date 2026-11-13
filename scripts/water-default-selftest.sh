#!/usr/bin/env bash
# n13 gate for "the new water is the DEFAULT": the LAUNCHER half, against a fake install.
#
#   usage: bash scripts/water-default-selftest.sh [--keep]
#
# scripts/installer-selftest.sh proves the installer installs remix-ref; this one proves the other
# half of rhY's ask ("make sure we get the new water by default"), which only the launcher can
# prove, because the launcher is what a desktop click runs:
#
#   1  the fake install starts as a fresh one: the pack's own water material, and an rtx.conf with
#      EVERY water key removed (config/rtx.conf minus the water block, both hashes pulled out of
#      rtx.animatedWaterTextures). The negative control is that `water.sh status` says so.
#   2  RHYS_DRY_RUN=1 with RHYS_WATER UNSET -> remix-ref is installed, its sheet generated, its
#      rtx.conf keys applied with a backup, and the choice recorded as mode=default.
#   3  the same dry run again changes not one byte (material, rtx.conf, state, sheets).
#   4  RHYS_WATER=soft -> soft is installed and recorded as mode=explicit; every later dry run
#      with RHYS_WATER unset keeps soft and says why. That is the "never overwrite a variant the
#      user picked explicitly" requirement.
#   5  `default --force` overrides it once; `water.sh pack` is a byte-for-byte rollback and then
#      sticks against the default path too.
#   6  the launcher's sandbox guard: with RHYREMIX_INSTALLER_SANDBOX=1 a folder without
#      .fake-install-marker is refused - which matters because launch-rtx.sh falls back to the
#      real game folder when its own directory has no gta-sa.exe, and a broken test must never be
#      able to install water into a real install. The real game folder is hashed before/after.
#
# Nothing here runs the game, wine, Proton or the GPU: RHYS_DRY_RUN=1 stops the launcher before the
# Proton line, GTASA_RTX_NO_DEPLOY=1 skips the build, RHY_SKIP_VRAM_CHECK=1 skips nvidia-smi,
# RHYS_SHOT_WATCH=0 skips the screenshot watcher, and STEAM_COMPAT_DATA_PATH points at build/tmp so
# even the prefix directory is scratch. All output goes to build/tmp/water-default-selftest/.
set -uo pipefail
cd "$(dirname "$0")/.."
REPO="$PWD"
GAME="${RHYS_GAME_DIR:-/media/rhy/data/Gamez/GTASA-RTX}"
T="build/tmp/water-default-selftest"
G="$T/game"
W="$G/rtx-remix/mods/gameReadyAssets/texture/Water"
KEEP=0
for a in "$@"; do case "$a" in --keep) KEEP=1 ;; *) echo "usage: $0 [--keep]"; exit 2 ;; esac; done

rm -rf "$T"; mkdir -p "$W" "$G/scripts"
pass=0; fails=()
ok()  { pass=$((pass+1)); echo "PASS $1"; }
bad() { fails+=("$1"); echo "FAIL $1${2:+  ($2)}"; }
sha() { sha256sum "$1" 2>/dev/null | awk '{print $1}'; }
state() { sed -n "s/^$1=//p" "$G/.rhys-water/state" 2>/dev/null | head -1; }
keyval() { grep -m1 -E "^[[:space:]]*$2[[:space:]]*=" "$1" 2>/dev/null | tr -d '\r' | sed 's/.*=[[:space:]]*//'; }

# ---------------------------------------------------------------- the fake install --
# The material: config/water/pack.usda IS the pack's own file byte for byte (that equality is what
# lets tools/water.sh snapshot a fresh install's material without guessing), so a fresh install can
# be built from the repo alone.
cp config/water/pack.usda "$W/Water_materials.usda"
: > "$G/gta-sa.exe"                       # the launcher refuses a folder that is not an install
printf 'fake install - scripts/water-default-selftest.sh scratch, never a real game folder\n' > "$G/.fake-install-marker"
# The normal sheet: the pack's real 10240x512 BC5 sheet when the game folder is readable; otherwise
# a synthetic 320x16 BC5 sheet (20 frames of 16x16) so the generation path still runs end to end.
if [ -f "$GAME/rtx-remix/mods/gameReadyAssets/texture/Water/water_normal.dds" ]; then
  cp -p "$GAME/rtx-remix/mods/gameReadyAssets/texture/Water/water_normal.dds" "$W/water_normal.dds"
  SHEET="the pack's real sheet"
else
  python3 - "$W/water_normal.dds" <<'PY'
import struct, sys
# BC5_UNORM, DX10 header, 320x16 = 20 frames of 16x16. Every 4x4 block: two BC4 sub-blocks with
# endpoints (255, 0) and all 16 codes pointing at LUT entry 3 (=145) for R and entry 4 (=109) for G,
# i.e. a flat-ish octahedral field with a real, measurable tilt - enough to exercise decode,
# decimate, the strength bisection and the RGBA8+mips writer.
w, h = 320, 16
lut_idx = (3, 4)
def bc4():
    b = bytearray([255, 0])
    bits = [lut_idx[0]] * 16
    v = 0
    for i, c in enumerate(bits):
        v |= c << (3 * i)
    for i in range(6):
        b.append((v >> (8 * i)) & 0xFF)
    return bytes(b)
blk = bc4() + bc4()
mip0 = b"".join(blk for _ in range((w // 4) * (h // 4)))
hdr = bytearray(124)
hdr[0:4] = b"DDS "
struct.pack_into("<I", hdr, 4, 124)
struct.pack_into("<II", hdr, 8, 0x1000 | 8, 0x4000 | 0x8)   # HEIGHT|WIDTH | PIXELFORMAT|HEIGHT|WIDTH
struct.pack_into("<II", hdr, 12, h, w)
struct.pack_into("<I", hdr, 80, 32)
struct.pack_into("<I", hdr, 84, 0x4)                        # fourcc DXT10 (DX10)
dxgi = bytearray(20)
struct.pack_into("<IIII", dxgi, 0, 83, 3, 0x4, 0)           # BC5_UNORM, RESOURCE_2D, no flags, 1 mip
open(sys.argv[1], "wb").write(bytes(hdr) + bytes(dxgi) + mip0)
PY
  SHEET="a synthetic BC5 sheet (the game folder is not readable here)"
fi
# The rtx.conf: the shipped profile with the water block taken out again, so "the keys are there"
# afterwards is a result and not a given.
python3 - <<PY
import re
keys = {"rtx.translucentMaterial.normalIntensity", "rtx.translucentMaterial.animatedWaterEnable",
        "rtx.translucentMaterial.animatedWaterSecondaryNormalLodBias",
        "rtx.opaqueMaterial.layeredWaterNormalEnable",
        "rtx.rayreconstruction.invalidateHistoryForAnimatedWater",
        "rtx.rayreconstruction.combineSpecularAlbedo"}
out = []
for line in open("config/rtx.conf", newline="").read().split("\\n"):
    m = re.match(r"^\\s*(rtx\\.[A-Za-z0-9_.]+)\\s*=", line)
    if m and m.group(1) in keys:
        continue
    if m and m.group(1) == "rtx.animatedWaterTextures":
        line = re.sub(r",\\s*0x(59296793798A7318|A75A0B41F2454F04)", "", line)
    out.append(line)
open("$G/rtx.conf", "w", newline="").write("\\n".join(out))
PY
RTX0="$(sha "$G/rtx.conf")"

launcher() {   # launcher <logname> [ENV=VAL ...]
  local log="$T/$1.log"; shift
  env RHYREMIX_INSTALLER_SANDBOX=1 RHYS_DRY_RUN=1 GTASA_RTX_NO_DEPLOY=1 RHY_SKIP_VRAM_CHECK=1 \
      RHYS_SHOT_WATCH=0 STEAM_COMPAT_DATA_PATH="$T/pfx" GTASA_RTX_DIR="$G" RHYS_ASI_REPO="$REPO" \
      "$@" timeout 180 bash launcher/launch-rtx.sh > "$log" 2>&1
  return $?
}
snap_game() { find "$GAME" -maxdepth 1 -printf '%p %y %s %T@\n' 2>/dev/null | sort; }
snap_game > "$T/game-before.txt"

# ------------------------------------------------------------------- 1 negative control --
bash tools/water.sh status "$G" > "$T/status0.log" 2>&1
grep -q 'installed by exact bytes  : pack' "$T/status0.log" \
  && ok "a fresh fake install starts on the pack's own material (identified by sha256)" \
  || bad "the fake install did not start on the pack's material: $(sed -n 's/^  installed.*//p' "$T/status0.log" | tr '\n' '|')"
grep -q 'needs rtx.translucentMaterial.animatedWaterEnable' "$T/status0.log" \
  && ok "status reports the animated-water keys as MISSING before anything runs (negative control)" \
  || bad "status did not report the missing keys: $(grep -c . "$T/status0.log") lines"
grep -q 'needs rtx.animatedWaterTextures: missing' "$T/status0.log" \
  && ok "status reports the water hashes as missing from the pack's list: $(sed -n "s/^ *needs rtx.animatedWaterTextures: //p" "$T/status0.log" | head -1)" \
  || bad "status did not report the missing hashes"
[ "$(grep -E '^[[:space:]]*rtx\.animatedWaterTextures[[:space:]]*=' "$G/rtx.conf" | grep -oE '0x(59296793798A7318|A75A0B41F2454F04)' | wc -l)" = 0 ] \
  && ok "the fake rtx.conf really has neither water hash in its list (negative control holds)" \
  || bad "the fake rtx.conf still carries a water hash"
grep -q 'state (.rhys-water/state)       : none yet' "$T/status0.log" \
  && ok "a fresh install has no water state file yet" || bad "the fresh install already has a state file"

# ------------------------------------------------------------------- 2 the default path --
launcher dry1
[ $? = 0 ] && ok "launcher dry run (RHYS_WATER unset) exits 0" || bad "launcher dry run failed" "see $T/dry1.log"
grep -q "RHYS_WATER unset -> making sure the default variant is installed" "$T/dry1.log" \
  && ok "the launcher says it is applying the DEFAULT, not waiting for RHYS_WATER" \
  || bad "the launcher did not say it was applying the default"
[ "$(sha "$W/Water_materials.usda")" = "$(sha config/water/remix-ref.usda)" ] \
  && ok "remix-ref is live after one desktop-equivalent start ($SHEET)" \
  || bad "remix-ref is not live after the default dry run"
[ -s "$W/water_normal_ref.dds" ] && ok "the regenerated sheet was generated on the way ($(stat -c%s "$W/water_normal_ref.dds") bytes)" \
  || bad "water_normal_ref.dds was not generated"
[ "$(state variant)" = remix-ref ] && [ "$(state mode)" = default ] \
  && ok "state: variant=remix-ref mode=default" || bad "state is wrong: $(tr '\n' ' ' < "$G/.rhys-water/state" 2>/dev/null)"
grep -q 'requested_by=launcher default' "$G/.rhys-water/state" \
  && ok "state names the launcher as the requester" || bad "state does not name the requester"
for k in 'rtx.translucentMaterial.animatedWaterEnable:True' \
         'rtx.translucentMaterial.animatedWaterSecondaryNormalLodBias:2.0' \
         'rtx.opaqueMaterial.layeredWaterNormalEnable:True' \
         'rtx.rayreconstruction.invalidateHistoryForAnimatedWater:False' \
         'rtx.rayreconstruction.combineSpecularAlbedo:True' \
         'rtx.translucentMaterial.normalIntensity:1.0'; do
  key="${k%%:*}"; want="${k##*:}"
  got="$(keyval "$G/rtx.conf" "$key")"
  [ "$got" = "$want" ] && ok "rtx.conf now has $key = $want" || bad "$key is '$got', expected $want"
done
for h in 0x59296793798A7318 0xA75A0B41F2454F04; do
  n=$(grep -E '^[[:space:]]*rtx\.animatedWaterTextures[[:space:]]*=' "$G/rtx.conf" | grep -o -- "$h" | wc -l)
  [ "$n" = 1 ] && ok "the animatedWaterTextures list carries $h exactly once" || bad "$h listed $n times"
done
[ "$(grep -cE '^[[:space:]]*rtx\.animatedWaterTextures[[:space:]]*=' "$G/rtx.conf")" = 1 ] \
  && ok "the 1246-character hash list is still ONE line" || bad "the hash list was duplicated"
[ -f "$G/.rhys-water/rtx.conf.pristine" ] && [ "$(sha "$G/.rhys-water/rtx.conf.pristine")" = "$RTX0" ] \
  && ok "the pre-change rtx.conf is kept byte for byte in .rhys-water/rtx.conf.pristine" \
  || bad "the pristine rtx.conf backup is missing or wrong"
[ -f "$G/.rhys-water/rtx.conf.prev" ] && ok ".rhys-water/rtx.conf.prev exists (one-step undo)" || bad "no prev backup"

# ------------------------------------------------------------------- 3 idempotence --
M0="$(sha "$W/Water_materials.usda")"; R0="$(sha "$G/rtx.conf")"; S0="$(sha "$G/.rhys-water/state")"
D0="$(sha "$W/water_normal_ref.dds")"
launcher dry2
[ $? = 0 ] && ok "second launcher dry run exits 0" || bad "second launcher dry run failed" "see $T/dry2.log"
grep -q "already the live water material" "$T/dry2.log" && ok "the second run reports the variant as already installed" \
  || bad "the second run did not report it as already installed"
grep -q "already carries every requested water key" "$T/dry2.log" \
  && ok "the second run reports the rtx.conf keys as already set" || bad "the second run re-touched rtx.conf"
[ "$(sha "$W/Water_materials.usda")" = "$M0" ] && [ "$(sha "$G/rtx.conf")" = "$R0" ] \
  && [ "$(sha "$G/.rhys-water/state")" = "$S0" ] && [ "$(sha "$W/water_normal_ref.dds")" = "$D0" ] \
  && ok "the second run changed no byte: material, rtx.conf, state and sheet all hash the same" \
  || bad "a second default run changed something"

# ------------------------------------------------------------------- 4 an explicit choice --
launcher dry3 RHYS_WATER=soft
[ $? = 0 ] && ok "launcher dry run with RHYS_WATER=soft exits 0" || bad "RHYS_WATER=soft dry run failed" "see $T/dry3.log"
[ "$(sha "$W/Water_materials.usda")" = "$(sha config/water/soft.usda)" ] \
  && ok "RHYS_WATER=soft installed soft" || bad "RHYS_WATER=soft did not install soft"
[ "$(state variant)" = soft ] && [ "$(state mode)" = explicit ] \
  && ok "state: variant=soft mode=explicit (a person asked)" || bad "the explicit choice was not recorded: $(tr '\n' ' ' < "$G/.rhys-water/state" 2>/dev/null)"
grep -q 'requested_by=launcher RHYS_WATER=soft' "$G/.rhys-water/state" \
  && ok "state names RHYS_WATER as the requester" || bad "state does not name RHYS_WATER"
launcher dry4
[ $? = 0 ] && ok "launcher dry run after that exits 0" || bad "post-explicit dry run failed" "see $T/dry4.log"
[ "$(sha "$W/Water_materials.usda")" = "$(sha config/water/soft.usda)" ] \
  && ok "the default path did NOT overwrite the explicit 'soft'" || bad "the default path overwrote an explicit choice"
grep -q "keeping 'soft'" "$T/dry4.log" && ok "the launcher says it is keeping the explicit choice" \
  || bad "the launcher did not say it was keeping the explicit choice"
[ "$(state mode)" = explicit ] && ok "the state file still says mode=explicit" || bad "the state file was downgraded"

# ------------------------------------------------------------------- 5 force and rollback --
bash tools/water.sh default "$G" --force > "$T/force.log" 2>&1
[ $? = 0 ] && ok "tools/water.sh default --force exits 0" || bad "default --force failed" "see $T/force.log"
[ "$(sha "$W/Water_materials.usda")" = "$(sha config/water/remix-ref.usda)" ] \
  && ok "--force brings the default back, once" || bad "--force did not install the default"
[ "$(state mode)" = default ] && ok "after --force the state is mode=default again" || bad "--force left the mode wrong"
bash tools/water.sh pack "$G" > "$T/pack.log" 2>&1
[ "$(sha "$W/Water_materials.usda")" = "$(sha config/water/pack.usda)" ] \
  && ok "tools/water.sh pack restores the pack's material byte for byte" || bad "the rollback is not byte-for-byte"
[ "$(state variant)" = pack ] && [ "$(state mode)" = explicit ] \
  && ok "the rollback is recorded as an explicit choice, so it sticks" || bad "the rollback was not recorded as explicit"
launcher dry5
[ "$(sha "$W/Water_materials.usda")" = "$(sha config/water/pack.usda)" ] \
  && ok "and the launcher's default path leaves a deliberate 'pack' alone" || bad "the launcher undid the rollback"

# ------------------------------------------------------------------- 6 refusals and safety --
bash tools/water.sh default "$T/notagame" > "$T/notagame.log" 2>&1
[ $? != 0 ] && ok "water.sh refuses a directory with no water material" || bad "water.sh accepted a non-install"
bash tools/water.sh watery "$G" > "$T/badvariant.log" 2>&1
[ $? != 0 ] && ok "water.sh refuses an unknown variant" || bad "water.sh accepted an unknown variant"
mkdir -p "$T/nomarker"; : > "$T/nomarker/gta-sa.exe"
launcher nomarker GTASA_RTX_DIR="$T/nomarker"
grep -q 'fake-install-marker' "$T/nomarker.log" \
  && ok "the launcher refuses a sandbox run against a folder without the marker (a broken test cannot reach a real install)" \
  || bad "the launcher's sandbox guard did not fire: $(head -3 "$T/nomarker.log" | tr '\n' ' ')"
[ "$(sha "$W/Water_materials.usda")" = "$(sha config/water/pack.usda)" ] \
  && ok "and that refusal changed nothing in the scratch install" || bad "the refused run still wrote to the scratch install"
bash tools/water.sh default "$G" --dry-run --force > "$T/dryrun.log" 2>&1
grep -q '+ cp .*config/water/remix-ref.usda' "$T/dryrun.log" \
  && ok "water.sh --dry-run prints the plan (the cp it would do)" || bad "water.sh --dry-run printed no plan: $(head -3 "$T/dryrun.log" | tr '\n' ' ')"
[ "$(sha "$W/Water_materials.usda")" = "$(sha config/water/pack.usda)" ] && [ "$(sha "$G/rtx.conf")" = "$R0" ] \
  && ok "water.sh --dry-run wrote nothing: material and rtx.conf still hash as before" \
  || bad "water.sh --dry-run wrote to the install"
snap_game > "$T/game-after.txt"
cmp -s "$T/game-before.txt" "$T/game-after.txt" && ok "$GAME untouched (sizes and mtimes identical)" \
  || bad "$GAME was touched by this test"

echo
echo "$pass checks passed, ${#fails[@]} failed"
if [ "${#fails[@]}" -ne 0 ]; then echo "FAILURES:"; for f in "${fails[@]}"; do echo "  - $f"; done; exit 1; fi
echo "WATER DEFAULT SELFTEST OK"
[ "$KEEP" = 1 ] || rm -rf "$T"
