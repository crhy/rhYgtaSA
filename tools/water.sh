#!/usr/bin/env bash
# s02/s32/n07/n13: switch the GTA SA RTX Remix water material between variants - and since n13,
# make the research variant the DEFAULT everywhere, together with the rtx.conf keys it needs.
#
#   usage: tools/water.sh {status|default|remix-ref|soft|realistic|calm|pack} [GAME_DIR]
#                         [--dry-run] [--no-keys] [--force]
#
# n13 (rhY: "make sure we get the new water by default"). Three things changed:
#
#   1. `default` installs $RHYS_WATER_DEFAULT (default: remix-ref) and is what apply.sh and
#      launch-rtx.sh now call. It is a no-op when that variant is already live.
#   2. The choice is RECORDED, in <game>/.rhys-water/state:
#         variant=remix-ref  mode=default|explicit  requested_by=...  material_sha256=...  when=...
#      mode=explicit means a person named the variant (`water.sh soft`, `RHYS_WATER=soft`,
#      `apply.sh --water soft`): the default path never overwrites it, ever. `--force` does, once.
#      Pre-n13 installs have no state file, so their first `default` run adopts the default and
#      says so out loud.
#   3. water.sh now APPLIES the rtx.conf keys that go with the variant (tools/rtx_water_keys.py)
#      instead of printing them and hoping: idempotent, one marked append block for absent keys,
#      values rewritten in place, missing hashes appended to the existing
#      rtx.animatedWaterTextures list, and the previous bytes kept in
#      <game>/.rhys-water/rtx.conf.pristine (once) and .prev (every change). `--no-keys` skips it.
#      Before this, G/rtx.conf had normalIntensity and the hash list but NONE of the animated-water
#      keys, so the runtime's own water path never ran on any surface, on any variant.
#
# Variants (whole text USDs in REPO/config/water/, so switching is a rename - see "Why a variant
# file and not a patch" below):
#
#   remix-ref  (n07, the research answer, the n13 default) 'realistic' with the numbers the other
#              RTX Remix projects ship and a REGENERATED sheet at a research-derived tilt:
#              transmittance (0.62,0.8,0.8) @ 4 m (xoxor4d's GTA IV RTX default water material),
#              normalmap -> ./water_normal_ref.dds generated at --target-tilt 6 (median facet tilt
#              44 -> 6 deg, |grad| 21.3 -> 1.0 deg/texel), sprite_sheet_fps 12. It also needs the
#              rtx.conf block this script now installs: both water hashes in
#              rtx.animatedWaterTextures, so the runtime's own water path (UV scroll + a second,
#              LOD-blurred normal sample) runs on both surfaces. Research log with every URL:
#              .strata/notes-n07-water-research.md.
#
#   soft       (s32, what we recommended before the research) 'realistic' plus a REGENERATED normal
#              sheet: inputs:normalmap_texture -> ./water_normal_soft.dds, which this script
#              generates on demand with tools/water_normal_soft.py from the pack's own
#              water_normal.dds (the pack's file is never written over). Measured on the pack's
#              sheet: median facet tilt 44 deg, 32 % of texels over 60 deg, |grad tilt|
#              21.3 deg/texel, tilt decorrelated after 10 px of a 512 px frame. Generated sheet:
#              12 deg, 0 %, 2.2 deg/texel, 17 px of a 128 px frame. That is the fix for the
#              "speckled white/black sheet": the mdl has no normal-strength/roughness/UV-scale
#              input, so the map is the only place the high frequencies can be removed (see
#              .strata/notes-s32-water-vegetation.md).
#   realistic  (s02) same material, the pack's own sheet, transmittance (0.05,0.22,0.45) @ 20 m,
#              i.e. Jerlov I clear ocean, IOR 1.33, diffuse layer off, 12 fps, encoding pinned.
#   calm       'realistic' plus thin_walled = 1 (the sea behaves as a single-sided surface).
#   pack       the GTASA-Remix pack's own material, byte for byte (the control, and the rollback).
#
# Why a variant file and not a patch: the pack's material is a 15 KB text USD
# (Water_materials.usda) that overrides the Shader prim of the two water materials
# (mat_59296793798A7318 = the sea texture the game generates, mat_A75A0B41F2454F04 = the game's
# waterclear256). Every parameter we want is an input of AperturePBR_Translucent.mdl, so the
# variant is a whole file and switching is a rename.
#
# The original file is kept once, as Water_materials.usda.orig, and `pack` restores it byte for
# byte. A fresh install has no such snapshot; it is taken automatically ONLY when the live file is
# byte-identical to config/water/pack.usda (provable), and water.sh still refuses to guess
# otherwise. Everything else is a copy of a file in REPO/config/water/.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEFAULT_VARIANT="${RHYS_WATER_DEFAULT:-remix-ref}"
# The header comment IS the usage text (one place to keep honest). Note the single rule: a rule that
# rewrites $0 and a second rule that tests /^#/ cannot be split, or the first line printed stops the
# print (tests/test_water_soft.py checks that this output contains "usage:").
usage() { awk 'NR>1 { if (!/^#/) exit; sub(/^# ?/, ""); print }' "${BASH_SOURCE[0]}"; }

DRY=0; FORCE=0; NOKEYS=0; POS=()
for a in "$@"; do
  case "$a" in
    --dry-run) DRY=1 ;;
    --force)   FORCE=1 ;;
    --no-keys) NOKEYS=1 ;;
    -h|--help) usage; exit 0 ;;
    *)         POS+=("$a") ;;
  esac
done
ACTION="${POS[0]:-status}"
GAME="${POS[1]:-/media/rhy/data/Gamez/GTASA-RTX}"

WATER_DIR="$GAME/rtx-remix/mods/gameReadyAssets/texture/Water"
MATERIAL="$WATER_DIR/Water_materials.usda"
ORIG="$WATER_DIR/Water_materials.usda.orig"
SOFT_DDS="$WATER_DIR/water_normal_soft.dds"
REF_DDS="$WATER_DIR/water_normal_ref.dds"
SOFT_SRC="$WATER_DIR/water_normal.dds"
STATE_DIR="$GAME/.rhys-water"
STATE="$STATE_DIR/state"

die() { echo "water.sh: $*" >&2; exit 1; }
note() { echo "  $*"; }
run() { echo "  + $*"; [ "$DRY" = 1 ] || "$@"; }
sha() { if [ -f "$1" ]; then sha256sum "$1" | awk '{print $1}'; fi; return 0; }   # "" + rc 0 if absent

[ -d "$WATER_DIR" ] || die "no water directory: $WATER_DIR (pass the game dir as argument 2)"
[ -f "$MATERIAL" ] || die "no water material: $MATERIAL"

# ------------------------------------------------------------------ identity --
# A variant is identified by exact bytes, not by grepping its texture name: that heuristic is how
# 'soft' and 'remix-ref' became indistinguishable the moment one of them was edited.
variant_src() {   # the file that WOULD be installed for a variant ("" = not a variant)
  case "$1" in
    pack)              [ -f "$ORIG" ] && echo "$ORIG" || echo "$MATERIAL" ;;
    remix-ref|soft|realistic|calm) echo "$REPO/config/water/$1.usda" ;;
    *)                 echo "" ;;
  esac
}
installed_variant() {   # which variant the live file IS, by sha256; "" for anything else
  local l v s
  l="$(sha "$MATERIAL")"
  for v in pack remix-ref soft realistic calm; do
    s="$(sha "$(variant_src "$v")")"
    [ -n "$s" ] && [ "$s" = "$l" ] && { echo "$v"; return 0; }
  done
  echo ""
}

# --------------------------------------------------------------------- state --
state_get() { [ -f "$STATE" ] && sed -n "s/^$1=//p" "$STATE" | head -1 || true; }
write_state() {   # <variant> <mode=default|explicit>
  local v="$1" mode="$2"
  if [ "$DRY" = 1 ]; then note "would record variant=$v mode=$mode in ${STATE#"$GAME"/}"; return 0; fi
  mkdir -p "$STATE_DIR"
  {
    echo "variant=$v"
    echo "mode=$mode"
    echo "requested_by=${RHYS_WHY:-tools/water.sh $ACTION}"
    echo "material_sha256=$(sha "$MATERIAL")"
    echo "rtx_conf_sha256=$(sha "$GAME/rtx.conf")"
    echo "when=$(date -Is)"
  } > "$STATE"
  note "recorded in ${STATE#"$GAME"/}: variant=$v mode=$mode (requested_by=${RHYS_WHY:-tools/water.sh $ACTION})"
}

# ------------------------------------------------------------------ the .orig --
ensure_orig() {
  [ -f "$ORIG" ] && return 0
  if [ "$(sha "$MATERIAL")" = "$(sha "$REPO/config/water/pack.usda")" ]; then
    note "no Water_materials.usda.orig yet, and the live file is byte-identical to"
    note "config/water/pack.usda - so it IS the pack's material: snapshotting it as the .orig."
    run cp "$MATERIAL" "$ORIG"
    return 0
  fi
  die "no $ORIG - and the live file is not the pack's material byte for byte, so water.sh will not
  guess which file was the original. Put the pack's Water_materials.usda at '$ORIG' and re-run."
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

# ------------------------------------------------- the rtx.conf keys (n13) -----
# What each variant needs in <game>/rtx.conf. normalIntensity MULTIPLIES whatever the map does, so
# it belongs to the variant: 1.0 when the sheet already carries the softening (remix-ref, soft,
# pack), 0.35 when the pack's own 44 deg sheet is in place (realistic, calm: s02 measured 1.0
# boiling and 0.2 glass). The animated-water block is remix-ref's: without it the runtime never
# classifies the surface as animated water, so no UV scroll and no second LOD-blurred normal.
key_table() {
  case "$1" in
    remix-ref) cat <<'EOF'
rtx.translucentMaterial.normalIntensity	1.0
rtx.translucentMaterial.animatedWaterEnable	True
rtx.translucentMaterial.animatedWaterSecondaryNormalLodBias	2.0
rtx.opaqueMaterial.layeredWaterNormalEnable	True
rtx.rayreconstruction.invalidateHistoryForAnimatedWater	False
rtx.rayreconstruction.combineSpecularAlbedo	True
EOF
    ;;
    soft)      printf 'rtx.translucentMaterial.normalIntensity\t1.0\n' ;;
    realistic) printf 'rtx.translucentMaterial.normalIntensity\t0.35\n' ;;
    calm)      printf 'rtx.translucentMaterial.normalIntensity\t0.35\n' ;;
    pack)      printf 'rtx.translucentMaterial.normalIntensity\t1.0\n' ;;
  esac
}
hash_require() {   # hashes that must be listed in rtx.animatedWaterTextures
  case "$1" in
    remix-ref) printf '0x59296793798A7318\n0xA75A0B41F2454F04\n' ;;
  esac
}

run_key_tool() {   # run_key_tool <variant> [extra python args]
  local v="$1"; shift
  [ -f "$GAME/rtx.conf" ] || { note "no $GAME/rtx.conf - the rtx.conf half was skipped"; return 0; }
  local args=( --file "$GAME/rtx.conf" ) k val h
  while IFS=$'\t' read -r k val; do [ -n "$k" ] && args+=( --set "$k=$val" ); done <<< "$(key_table "$v")"
  for h in $(hash_require "$v"); do args+=( --hash "$h" ); done
  [ -n "$(key_table "$v")$(hash_require "$v")" ] || { note "'$v' needs no rtx.conf keys"; return 0; }
  if [ "$DRY" = 1 ]; then
    python3 "$REPO/tools/rtx_water_keys.py" "${args[@]}" --dry-run "$@"
  else
    python3 "$REPO/tools/rtx_water_keys.py" "${args[@]}" --backup-dir "$STATE_DIR" "$@"
  fi
}

apply_keys() {
  local v="$1"
  if [ "$NOKEYS" = 1 ]; then note "rtx.conf keys: skipped (--no-keys); '$v' wants: $(key_table "$v" | tr '\n' ' ')"; return 0; fi
  echo "rtx.conf keys for '$v':"
  run_key_tool "$v" | sed 's/^/  /'
}

# ------------------------------------------------------------------ actions --
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
  local v; v="$(installed_variant)"
  [ -n "$v" ] && echo "  installed by exact bytes  : $v"
  if [ -f "$STATE" ]; then
    echo "  state (${STATE#"$GAME"/})       : $(tr '\n' ' ' < "$STATE" | sed 's/  */ /g')"
  else
    echo "  state (${STATE#"$GAME"/})       : none yet - the first 'default' run writes it"
  fi
  local d
  for d in "$REF_DDS" "$SOFT_DDS"; do
    [ -f "$d" ] && echo "  generated sheet         : $(basename "$d") ($(stat -c%s "$d") bytes)"
  done
  v="${v:-$(state_get variant)}"
  [ -n "$v" ] || v="$DEFAULT_VARIANT"
  echo "  rtx.conf keys for $v:"
  run_key_tool "$v" --check 2>&1 | sed 's/^/    /' || true
  if [ "$v" != "$DEFAULT_VARIANT" ]; then
    echo "  and what the default variant '$DEFAULT_VARIANT' would add on top:"
    run_key_tool "$DEFAULT_VARIANT" --check 2>&1 | sed 's/^/    /' || true
  fi
  echo "  default variant: $DEFAULT_VARIANT (RHYS_WATER_DEFAULT overrides it; 'default' never"
  echo "  overwrites a variant recorded as mode=explicit - use --force for that)"
}

install() {   # install <variant> <mode=default|explicit>
  local v="$1" mode="$2" src
  src="$(variant_src "$v")"
  [ -n "$src" ] || die "unknown water variant '$v' ({status|default|remix-ref|soft|realistic|calm|pack})"
  ensure_orig
  src="$(variant_src "$v")"          # re-resolve: 'pack' may have just been snapshotted
  if [ "$v" != pack ]; then
    [ -f "$src" ] || die "no variant file $src"
    python3 "$REPO/tools/water_check.py" "$src" "$(variant_src pack)" || die "variant $v does not validate"
  fi
  ensure_gen_dds "$v"
  if [ "$(sha "$src")" = "$(sha "$MATERIAL")" ]; then
    echo "'$v' is already the live water material (sha256 $(sha "$src")): the USD was not touched."
  else
    run cp "$src" "$MATERIAL"
    echo "installed water variant '$v' -> ${MATERIAL#"$GAME"/}"
  fi
  apply_keys "$v"
  # Record the choice, but only when the record is not already exactly this: a re-run must not
  # rewrite the state file, or apply.sh's second run stops being a byte-identical no-op.
  if [ "$(state_get variant)" != "$v" ] || [ "$(state_get mode)" != "$mode" ]; then
    write_state "$v" "$mode"
  else
    note "state already records variant=$v mode=$mode - left as it is"
  fi
  echo "next: restart the game (Remix loads the mod's USD at boot); the sea stops are 10/11 of the"
  echo "photo tour (scripts/test-session.sh --tour). Rollback: bash tools/water.sh pack $GAME"
  echo "(the rtx.conf half of the rollback: bash tools/rtxprofile.sh restore $GAME, or the copies in"
  echo " ${STATE_DIR#"$GAME"/}: rtx.conf.pristine / rtx.conf.prev)"
}

do_default() {   # the n13 path: the default variant, unless a person chose otherwise
  local want="$DEFAULT_VARIANT" rec mode cur
  [ -n "$(variant_src "$want")" ] || die "RHYS_WATER_DEFAULT='$want' is not a variant"
  rec="$(state_get variant)"; mode="$(state_get mode)"
  if [ "$mode" = explicit ] && [ "$FORCE" != 1 ]; then
    cur="$(installed_variant)"; [ -n "$cur" ] || cur="$rec"
    if [ -n "$rec" ] && [ "$cur" = "$rec" ]; then
      echo "water: keeping '$rec' - you asked for it explicitly"
      echo "  (${STATE#"$GAME"/}: mode=explicit, requested_by=$(state_get requested_by))"
      echo "  the default ('$want') never overwrites a variant a person picked."
      echo "  want the default back? bash tools/water.sh default $GAME --force"
      apply_keys "$rec"
      return 0
    fi
    echo "water: the state file says '$rec' was chosen explicitly, but the live material matches"
    echo "  none of our variants (someone edited it by hand) - leaving it exactly as it is."
    echo "  bash tools/water.sh $want $GAME --force installs over it."
    return 0
  fi
  [ "$mode" = explicit ] && note "--force: overriding the explicit choice '$rec' with '$want'"
  [ -f "$STATE" ] || note "no ${STATE#"$GAME"/} yet: this install never had a variant chosen on purpose,"
  [ -f "$STATE" ] || note "so the n13 default ('$want') applies. bash tools/water.sh pack $GAME is the rollback."
  install "$want" default
}

case "$ACTION" in
  status)    status ;;
  default)   do_default ;;
  remix-ref|soft|realistic|calm)
             export RHYS_WHY="${RHYS_WHY:-tools/water.sh $ACTION}"
             install "$ACTION" explicit ;;
  pack)      export RHYS_WHY="${RHYS_WHY:-tools/water.sh pack}"
             install pack explicit ;;
  *)         usage; exit 2 ;;
esac
