#!/usr/bin/env bash
# s02: switch GTA SA (RTX Remix) water material between the variants in REPO/config/water/.
#
#   tools/water.sh calm       [GAME_DIR]   gentle, shallow water (bay/harbour)
#   tools/water.sh realistic  [GAME_DIR]   moderate open-coast water
#   tools/water.sh pack       [GAME_DIR]   the pack's original material (Water_materials.usda.orig)
#   tools/water.sh status     [GAME_DIR]   which variant is currently installed
#   tools/water.sh revert     [GAME_DIR]   same as `pack`, kept for muscle memory
#
# GAME_DIR defaults to the installed RTX pack. The pack's original file is preserved as
# Water_materials.usda.orig next to the material; it is written once and never overwritten,
# so `pack`/`revert` is always a safe way back.
#
# The script validates the variant first (tools/water_check.py: text USD, balanced braces,
# same prim paths as the original, only inputs the mdl declares) and refuses to install a
# variant that fails. It never edits rtx.conf; it only prints the rtx.conf lines that go with
# the variant, because the normal-strength knob lives there and not in the USD.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
GAME_DEFAULT="/media/rhy/data/Gamez/GTASA-RTX"

action="${1:-}"
game="${2:-$GAME_DEFAULT}"

if [ -z "$action" ]; then
    sed -n '2,12p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
    exit 2
fi

matdir="$game/rtx-remix/mods/gameReadyAssets/texture/Water"
target="$matdir/Water_materials.usda"
orig="$matdir/Water_materials.usda.orig"

if [ ! -d "$matdir" ]; then
    echo "water.sh: no water material dir: $matdir" >&2
    echo "          pass the game dir as the second argument (default: $GAME_DEFAULT)" >&2
    exit 1
fi
if [ ! -f "$target" ] && [ ! -f "$orig" ]; then
    echo "water.sh: neither $target nor $orig exists - nothing to switch." >&2
    exit 1
fi

# keep the pack's original exactly once, never overwrite it
if [ ! -f "$orig" ]; then
    cp -n "$target" "$orig"
    echo "saved the pack's original to $orig"
fi

status() {
    local live="none (file differs from every variant)"
    for v in calm realistic pack; do
        if cmp -s "$REPO/config/water/$v.usda" "$target" 2>/dev/null; then live="$v"; fi
    done
    echo "game dir : $game"
    echo "material : $target"
    echo "installed: $live"
    if cmp -s "$orig" "$target"; then echo "           (identical to $orig)"; fi
}

case "$action" in
status)
    status
    exit 0
    ;;
revert)
    action="pack"
    ;;
calm|realistic|pack)
    ;;
*)
    echo "water.sh: unknown action '$action' (calm|realistic|pack|status|revert)" >&2
    exit 2
    ;;
esac

src="$REPO/config/water/$action.usda"
if [ ! -f "$src" ]; then
    echo "water.sh: missing variant $src" >&2
    exit 1
fi

python3 "$REPO/tools/water_check.py" "$src" "$orig" || {
    echo "water.sh: refusing to install $src (validation failed)" >&2
    exit 1
}

cp "$src" "$target"
echo "installed water variant '$action' -> $target"
status

# suggested (not applied) rtx.conf normal strength for this variant; see the notes for the
# tilt-vs-factor table that these numbers come from
case "$action" in
    calm)      ni="0.2  # median facet tilt ~6 deg (glassy, sheltered)" ;;
    realistic) ni="0.35 # median facet tilt ~12 deg (moderate chop)" ;;
    pack)      ni="0.5  # median facet tilt ~17 deg, still steep" ;;
esac

cat <<EOF

Restart the game (or reload the mod in the Remix runtime) to pick it up.

The USD cannot soften the normals: AperturePBR_Translucent.mdl has no normal-strength,
roughness or UV-scale input. The runtime does, in rtx.conf - NOT written by this script,
check the exact spelling in the Remix UI (Material Options > PBR Material Modifiers):
  rtx.translucentMaterial.normalIntensity = $ni
      # the pack's water_normal.dds decodes to a median facet tilt of 46 deg as-is
  rtx.translucentMaterial.animatedWaterSecondaryNormalLodBias = 2   # 2nd normal sample from a
      # blurrier mip -> lower frequency waves (runtime's own layered-water detail layer)
See REPO/.strata/notes-s02-water.md for the measurements and the key descriptions.
EOF
