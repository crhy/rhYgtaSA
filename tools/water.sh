#!/usr/bin/env bash
# s02/s32/n07: switch the GTA SA RTX Remix water material between variants, with an A/B switch.
#
#   usage: tools/water.sh {status|remix-ref|soft|realistic|calm|pack} [GAME_DIR] [--dry-run]
#
#   remix-ref  (n07, the research answer) 'realistic' with the numbers the other RTX Remix projects
#              ship and a REGENERATED sheet at a research-derived tilt: transmittance (0.62,0.8,0.8)
#              @ 4 m (xoxor4d's GTA IV RTX default water material), normalmap -> ./water_normal_ref.dds
#              generated at --target-tilt 6 (median facet tilt 44 -> 6 deg, |grad| 21.3 -> 1.0
#              deg/texel), sprite_sheet_fps 12. It also needs the rtx.conf block install() prints:
#              both water hashes in rtx.animatedWaterTextures, so the runtime's own water path (UV
#              scroll + a second, LOD-blurred normal sample) runs on both surfaces. Research log with
#              every URL: .strata/notes-n07-water-research.md.
#
#   soft       (s32, what we recommended before the research) 'realistic' plus a REGENERATED normal sheet:
#              inputs:normalmap_texture -> ./water_normal_soft.dds, which this script generates
#              on demand with tools/water_normal_soft.py from the pack's own water_normal.dds
#              (the pack's file is never written over). Measured on the pack's sheet: median
#              facet tilt 44 deg, 32 % of texels over 60 deg, |grad tilt| 21.3 deg/texel, tilt
#              decorrelated after 10 px of a 512 px frame. Generated sheet: 12 deg, 0 %,
#              2.2 deg/texel, 17 px of a 128 px frame. That is the fix for the "speckled
#              white/black sheet": the mdl has no normal-strength/roughness/UV-scale input, so
#              the map is the only place the high frequencies can be removed (see
#              .strata/notes-s32-water-vegetation.md).
#   realistic  (s02) same material, the pack's own sheet, transmittance (0.05,0.22,0.45) @ 20 m,
#              i.e. Jerlov I clear ocean, IOR 1.33, diffuse layer off, 12 fps, encoding pinned.
#   calm       'realistic' plus thin_walled = 1 (the sea behaves as a single-sided surface).
#   pack       the GTASA-Remix pack's own material, byte for byte (the control).
#
# Why a variant file and not a patch: the pack's material is a 15 KB text USD
# (Water_materials.usda) that overrides the Shader prim of the two water materials
# (mat_59296793798A7318 = the sea texture the game generates, mat_A75A0B41F2454F04 = the game's
# waterclear256). Every parameter we want is an input of AperturePBR_Translucent.mdl, so the
# variant is a whole file and switching is a rename.
#
# It never edits rtx.conf; it prints the rtx.conf lines that go with the variant you chose,
# because the normal-strength knob (rtx.translucentMaterial.normalIntensity) lives there and
# multiplies whatever the map does - with 'soft' it should be back at 1.0, with 'realistic'
# 0.35 is what s02 measured as good.
#
# The original file is kept once, as Water_materials.usda.orig, and `pack` restores it byte for
# byte. Everything else is a copy of a file in REPO/config/water/.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
GAME="${2:-/media/rhy/data/Gamez/GTASA-RTX}"
DRY=0
for a in "$@"; do [ "$a" = "--dry-run" ] && DRY=1; done
WATER_DIR="$GAME/rtx-remix/mods/gameReadyAssets/texture/Water"
MATERIAL="$WATER_DIR/Water_materials.usda"
ORIG="$WATER_DIR/Water_materials.usda.orig"
SOFT_DDS="$WATER_DIR/water_normal_soft.dds"
REF_DDS="$WATER_DIR/water_normal_ref.dds"
SOFT_SRC="$WATER_DIR/water_normal.dds"

die() { echo "water.sh: $*" >&2; exit 1; }
note() { echo "  $*"; }
run() { echo "  + $*"; [ "$DRY" = 1 ] || "$@"; }

[ -d "$WATER_DIR" ] || die "no water directory: $WATER_DIR (pass the game dir as argument 2)"
[ -f "$MATERIAL" ] || die "no water material: $MATERIAL"

ensure_orig() {
  if [ ! -f "$ORIG" ]; then
    die "no $ORIG - copy the pack's Water_materials.usda to that name first
  (cp '$MATERIAL' '$ORIG'); water.sh refuses to guess which file was the original"
  fi
}

# The generated-sheet variants point at a texture this script makes; make sure it exists before the
# USD does. sheet_spec <variant> prints "<out.dds>|<target tilt deg>" for the variants that need one
# (and nothing for the ones that use the pack's own sheet).
sheet_spec() {
  case "$1" in
    soft)      echo "$SOFT_DDS|12" ;;
    remix-ref) echo "$REF_DDS|6" ;;
    *)         echo "" ;;
  esac
}

ensure_gen_dds() {
  local spec out tilt
  spec="$(sheet_spec "$1")"
  [ -n "$spec" ] || return 0
  out="${spec%|*}"; tilt="${spec##*|}"
  if [ -f "$out" ]; then
    note "generated sheet already there: $out"
    return 0
  fi
  [ -f "$SOFT_SRC" ] || die "'$1' needs the pack's sheet to generate from: $SOFT_SRC is missing"
  echo "  generating $(basename "$out") from $(basename "$SOFT_SRC") (target median tilt $tilt deg):"
  if [ "$DRY" = 1 ]; then
    echo "  + python3 $REPO/tools/water_normal_soft.py '$SOFT_SRC' '$out' --target-tilt $tilt"
  else
    python3 "$REPO/tools/water_normal_soft.py" "$SOFT_SRC" "$out" --target-tilt "$tilt" |
      sed 's/^/    /' || die "water_normal_soft.py failed"
    [ -s "$out" ] || die "generated $out is empty"
  fi
}

status() {
  echo "== $MATERIAL"
  python3 - "$MATERIAL" "$ORIG" <<'PY'
import re, sys, hashlib, os
path, orig = sys.argv[1], sys.argv[2]
def grab(p, name):
    if not os.path.exists(p):
        return None
    t = open(p, encoding="utf-8", errors="replace").read()
    m = re.findall(r'inputs:%s\s*=\s*([^(\n]+)' % name, t)
    return [v.strip() for v in m] or None
cur = {k: grab(path, k) for k in ("use_diffuse_layer", "transmittance_color",
                                  "transmittance_measurement_distance", "sprite_sheet_fps",
                                  "thin_walled", "encoding", "normalmap_texture")}
for k, v in cur.items():
    print("  %-34s %s" % (k, " / ".join(v) if v else "-"))
name = "unknown"
if os.path.exists(path) and os.path.exists(orig) and \
        hashlib.sha256(open(path, "rb").read()).hexdigest() == \
        hashlib.sha256(open(orig, "rb").read()).hexdigest():
    name = "pack (the GTASA-Remix pack's own material)"
elif cur.get("normalmap_texture") and any("water_normal_ref" in v for v in cur["normalmap_texture"]):
    name = "remix-ref (n07: research numbers + regenerated normal sheet)"
elif cur.get("normalmap_texture") and any("water_normal_soft" in v for v in cur["normalmap_texture"]):
    name = "soft (s32: regenerated normal sheet)"
elif cur.get("thin_walled") and cur["thin_walled"][0] == "1":
    name = "calm (s02)"
elif cur.get("use_diffuse_layer") and cur["use_diffuse_layer"][0] == "0":
    name = "realistic (s02)"
print("  installed variant: %s" % name)
print("  original kept: %s" % ("yes" if os.path.exists(orig) else "NO - run nothing until this is fixed"))
PY
}

install() {
  local src="$REPO/config/water/$1.usda"
  [ -f "$src" ] || die "no variant file $src"
  ensure_orig
  python3 "$REPO/tools/water_check.py" "$src" "$ORIG" || die "variant $1 does not validate"
  ensure_gen_dds "$1"
  run cp "$src" "$MATERIAL"
  echo "installed water variant '$1'."
  echo "next: restart the game (Remix loads the mod's USD at boot), then tools/tour.sh --water-only."
  echo "rtx.conf lines that go with '$1' (water.sh does not touch rtx.conf):"
  case "$1" in
    remix-ref)
      echo "  rtx.translucentMaterial.normalIntensity = 1.0                 # the map carries the softening"
      echo "  rtx.translucentMaterial.animatedWaterEnable = True            # UV scroll + second normal sample"
      echo "  rtx.translucentMaterial.animatedWaterSecondaryNormalLodBias = 2.0   # >0 blurs the second layer"
      echo "  rtx.animatedWaterTextures = ..., 0x59296793798A7318, 0xA75A0B41F2454F04   # both water surfaces"
      echo "  rtx.rayreconstruction.invalidateHistoryForAnimatedWater = False # keep history: the sheet no longer re-randomises"
      echo "  rtx.rayreconstruction.combineSpecularAlbedo = True             # NVIDIA's fix for blotchy water reflections" ;;
    soft)      echo "  rtx.translucentMaterial.normalIntensity = 1.0   # the map already carries the softening" ;;
    realistic) echo "  rtx.translucentMaterial.normalIntensity = 0.35  # s02: 1.0 is boiling, 0.2 is glass" ;;
    calm)      echo "  rtx.translucentMaterial.normalIntensity = 0.35" ;;
    pack)      echo "  rtx.translucentMaterial.normalIntensity = 1.0   # the pack's default" ;;
  esac
}

case "${1:-status}" in
  status)   status ;;
  remix-ref) install remix-ref ;;
  soft)     install soft ;;
  realistic) install realistic ;;
  calm)     install calm ;;
  pack)     ensure_orig; run cp "$ORIG" "$MATERIAL"; echo "restored the pack's water material byte for byte." ;;
  *)        sed -n '2,4p' "$0"; exit 2 ;;
esac
