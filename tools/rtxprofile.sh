#!/usr/bin/env bash
# s22: switch the RTX Remix renderer between the shipped rtx.conf and the 60 fps profile,
# without ever losing the file the renderer last wrote.
#
#   tools/rtxprofile.sh perf     [GAME_DIR]   install config/rtx.conf.perf   (the 60 fps profile)
#   tools/rtxprofile.sh current  [GAME_DIR]   install config/rtx.conf        (this repo's shipped file)
#   tools/rtxprofile.sh orig     [GAME_DIR]   install <game>/rtx.conf.orig   (the runtime's first-launch file)
#   tools/rtxprofile.sh status   [GAME_DIR]   which profile is live, and what it says
#   tools/rtxprofile.sh restore  [GAME_DIR]   put back exactly the bytes that were there before
#   tools/rtxprofile.sh check                 offline key audit of the repo's rtx.conf* profiles
#
# Why a switcher and not an editor: rtx.conf is read once, before the first frame exists, and
# it also carries the asset pack's texture-hash lists (decal/ui/skyBox/particle/animatedWater/
# lightmap/ignore). Hand-editing those 1246-character lines is how a pack breaks. So the
# profiles are whole files in config/, and this script only ever replaces <game>/rtx.conf and
# keeps its own backups:
#
#   <game>/.rhys-rtxprofile/rtx.conf.pristine   the first file that was ever here, written once
#   <game>/.rhys-rtxprofile/rtx.conf.prev       the file that was here before the last switch
#   <game>/.rhys-rtxprofile/state               profile name + sha256 of the live file
#
# `orig` prefers <game>/rtx.conf.orig (deploy.sh makes it, the runtime also writes it); if that
# is missing the pristine snapshot is used, so a profile switch is always reversible even on an
# install that never ran deploy.sh.
#
# Nothing here runs the game, the bridge or wine. `check` compares every key of the repo's
# profiles against config/rtx-runtime-keys.txt, the option-name vocabulary generated from
# `strings` on <game>/.trex/d3d9.dll (see tools/rtx-runtime-keys.py): a key the runtime does
# not know is not an error, it is silently ignored, which is the failure mode this avoids.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
GAME_DEFAULT="/media/rhy/data/Gamez/GTASA-RTX"
STATE_DIR=".rhys-rtxprofile"

usage() { sed -n '2,26p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; }
die()   { echo "rtxprofile.sh: $*" >&2; echo "usage: tools/rtxprofile.sh {perf|current|orig|status|restore|check} [GAME_DIR]" >&2; exit 1; }

sha() { sha256sum "$1" | awk '{print $1}'; }

# key = value lines only (comments, blanks and CR dropped), truncated for display
kv() {
  grep -v '^[[:space:]]*#' "$1" | tr -d '\r' | grep '=' | sed 's/[[:space:]]*$//' | cut -c1-72 | sort
}

profile_src() { # $1 = profile, $2 = game dir; prints source file or nothing
  case "$1" in
    perf)    echo "$REPO/config/rtx.conf.perf" ;;
    current) echo "$REPO/config/rtx.conf" ;;
    orig)
      if [ -f "$2/rtx.conf.orig" ]; then echo "$2/rtx.conf.orig"
      elif [ -f "$2/$STATE_DIR/rtx.conf.pristine" ]; then echo "$2/$STATE_DIR/rtx.conf.pristine"
      else echo "" ; fi ;;
  esac
}

action="${1:-}"; game="${2:-$GAME_DEFAULT}"
case "$action" in ''|-h|--help|help) usage; exit 0 ;; esac

if [ "$action" = check ]; then
  # The perf profile is the one held to the rule. config/rtx.conf is audited too, but it is
  # the shipped file and carries two legacy spellings this build does not know
  # (rtx.enableVolumetricLighting, rtx.minReplacementTextureMipMapLevel - grep -c on
  # .trex/d3d9.dll returns 0), which is exactly what the audit is for.
  python3 "$REPO/tools/rtxkeycheck.py" --keys "$REPO/config/rtx-runtime-keys.txt" \
    "$REPO/config/rtx.conf.perf" || die "config/rtx.conf.perf has keys this runtime does not know"
  echo "-- shipped config/rtx.conf, for comparison (unknown keys there are dead settings):"
  python3 "$REPO/tools/rtxkeycheck.py" --keys "$REPO/config/rtx-runtime-keys.txt" \
    "$REPO/config/rtx.conf" || echo "   ^ expected: the two legacy keys, see s22 notes"
  exit 0
fi

[ -d "$game" ]   || die "no such game dir: $game"
[ -f "$game/rtx.conf" ] || die "no rtx.conf in $game (this is not a GTASA-RTX install, or the profile dir is wrong)"
case "$game" in
  "$REPO"|"$REPO"/*) die "refusing to touch the repo; GAME_DIR must be the game folder" ;;
esac

live="$game/rtx.conf"
state="$game/$STATE_DIR"
mkdir -p "$state"

case "$action" in
  status)
    cur=$(sha "$live")
    active="unknown"
    for p in perf current orig; do
      src=$(profile_src "$p" "$game")
      [ -n "$src" ] && [ "$(sha "$src")" = "$cur" ] && active="$p"
    done
    echo "game dir : $game"
    echo "rtx.conf : $cur  ($(wc -l < "$live") lines)"
    echo "profile  : $active"
    [ -f "$state/state" ] && { echo "last write: $(cat "$state/state")"; }
    [ -f "$state/rtx.conf.pristine" ] && echo "pristine : $(sha "$state/rtx.conf.pristine")"
    echo "-- the knobs the profiles differ on --"
    for k in rtx.graphicsPreset rtx.pathMaxBounces rtx.pathMinBounces \
             rtx.spatiallyHashedRadianceCache.qualityPreset rtx.neuralRadianceCache.qualityPreset \
             rtx.volumetrics.enable rtx.volumetrics.froxelGridResolutionScale \
             rtx.texturemanager.budgetPercentageOfAvailableVram \
             rtx.opacityMicromap.enable rtx.opacityMicromap.cache.maxBudgetSizeMB \
             rtx.opacityMicromap.cache.minFreeVidmemMBToNotAllocate rtx.dlfg.enable; do
      printf '  %-56s %s\n' "$k" "$(grep -m1 -E "^[[:space:]]*$k[[:space:]]*=" "$live" | sed 's/.*=[[:space:]]*//' || true)"
    done
    exit 0
    ;;
  restore)
    [ -f "$state/rtx.conf.prev" ] || die "nothing to restore (no $STATE_DIR/rtx.conf.prev)"
    cp "$state/rtx.conf.prev" "$live"
    echo "== restored the file that was live before the last switch: $(sha "$live")"
    exit 0
    ;;
  perf|current|orig) ;;
  *) die "unknown action '$action'" ;;
esac

src=$(profile_src "$action" "$game")
[ -n "$src" ] && [ -f "$src" ] || die "no source for profile '$action' (looked in config/ and $game)"

# backups first: pristine once, prev every time
[ -f "$state/rtx.conf.pristine" ] || cp -p "$live" "$state/rtx.conf.pristine"
cp -p "$live" "$state/rtx.conf.prev"

# what would be lost: keys in the live file that the profile does not carry
lost=$(comm -23 <(kv "$live" | cut -d= -f1 | sort -u) <(kv "$src" | cut -d= -f1 | sort -u))
if [ -n "$lost" ]; then
  echo "== keys in the live rtx.conf that '$action' does not carry (the runtime will use its default):"
  sed 's/^/   /' <<< "$lost"
fi

cp "$src" "$live"
echo "== rtx.conf <- $src"
echo "== installed sha256 $(sha "$live") (source $(sha "$src"))"
echo "$action $(sha "$live") $(date -Is)" > "$state/state"

echo "-- key changes applied --"
changes=$(diff <(kv "$state/rtx.conf.prev") <(kv "$live") | grep -E '^[<>]' || true)
if [ -n "$changes" ]; then sed 's/^/   /' <<< "$changes"; else echo "   (none)"; fi
echo
echo "launch, then confirm what the renderer actually took:"
echo "  grep -A80 'Effective RtxOption values' '$game/rtx-remix/logs/remix-dxvk.log'"
echo "undo: tools/rtxprofile.sh restore $game"
