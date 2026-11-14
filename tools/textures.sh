#!/usr/bin/env bash
# n20: put the generated high-res PBR surface set into the installed RTX Remix pack, and take it
# out again without leaving a byte behind.
#
#   tools/textures.sh on      [GAME_DIR] [--generate] [--scale N] [--max-size N] [--keep-covered]
#   tools/textures.sh off     [GAME_DIR]
#   tools/textures.sh status  [GAME_DIR]
#   tools/textures.sh generate [GAME_DIR]        (build the cache only, touch nothing else)
#
# What "on" installs: <cache>/rhy_textures.usda -> <game>/rtx-remix/mods/gameReadyAssets/
# rhy_textures.usda, the DDS maps -> .../texture/rhy_pbr/, and one `@./rhy_textures.usda@` entry
# appended to mod.usda's subLayers (last, so it composes on top of the pack's own Materials.usda
# - the same install shape tools/mirrors.py and docs/MIRRORS.md use, and the only shape that was
# ever verified in-game here). The layer overrides `mat_<HASH>` for the 40 textures
# config/texture-pbr.json names, measured by tools/texture_rank.py.
#
# Everything `on` writes is recorded in
# <game>/rtx-remix/mods/gameReadyAssets/.rhys-textures/ (a dot-dir: the runtime scans the mods
# directory for layers, and a dot-dir is the convention this repo already uses for its own state)
# - files.txt lists every file it copied, mod.usda.bak is the pristine copy of the file the
# sublayer edit touches. `off` deletes exactly those files, restores the backup byte-for-byte and
# removes the state dir, so an on/off round-trip leaves the game folder as it was.
#
# The DDS files are never written into the game folder by a build or a test: they are generated
# once into REPO/build/textures (tools/pbrify.py, from the game's own rasters) and copied.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
GAME_DEFAULT="/media/rhy/data/Gamez/GTASA-RTX"
# overridable so scripts/textures-selftest.sh can run the whole thing against a tiny generated
# set and a fake game tree instead of the 40-texture cache and a real install
CACHE="${TEXTURES_CACHE:-$REPO/build/textures}"
LIST="${TEXTURES_LIST:-$REPO/config/texture-pbr.json}"
LAYER="rhy_textures.usda"
TEXDIR="texture/rhy_pbr"
STATE_DIR=".rhys-textures"

usage() { sed -n '2,25p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; }
die() { echo "textures.sh: $*" >&2; usage; exit 1; }

action=""; game=""; generate=0; scale=4; max_size=2048; keep_covered=0
while [ $# -gt 0 ]; do
  case "$1" in
    on|off|status|generate)
      [ -z "$action" ] || die "two actions given ('$action' and '$1')"
      action="$1"; shift ;;
    --generate) generate=1; shift ;;
    --scale) [ $# -ge 2 ] || die "--scale needs a number"; scale="$2"; shift 2 ;;
    --max-size) [ $# -ge 2 ] || die "--max-size needs a number"; max_size="$2"; shift 2 ;;
    --keep-covered) keep_covered=1; shift ;;
    -h|--help) usage; exit 0 ;;
    *) [ -z "$game" ] || die "two game folders given ('$game' and '$1')"
       game="$1"; shift ;;
  esac
done
[ -n "$action" ] || die "no action"
[ -n "$game" ] || game="$GAME_DEFAULT"

ASSETS="$game/rtx-remix/mods/gameReadyAssets"
MOD_USDA="$ASSETS/mod.usda"
STATE="$ASSETS/$STATE_DIR"

do_generate() {
  [ -f "$LIST" ] || die "no selection list at $LIST"
  echo "generating into $CACHE (scale x$scale, max ${max_size}px)"
  local extra=()
  if [ "$keep_covered" = 1 ]; then extra=(--keep-covered); fi
  python3 "$REPO/tools/pbrify.py" --game "$game" --list "$LIST" --out "$CACHE" \
      --scale "$scale" --max-size "$max_size" --texdir "$TEXDIR" ${extra[@]+"${extra[@]}"} \
      --report "$CACHE/report.json"
}

cache_ready() { [ -f "$CACHE/$LAYER" ] && [ -d "$CACHE/$TEXDIR" ] && [ -n "$(ls -A "$CACHE/$TEXDIR" 2>/dev/null)" ]; }

case "$action" in
  generate)
    do_generate
    echo "All checks passed."
    exit 0 ;;

  on)
    [ -d "$ASSETS" ] || die "no $ASSETS - is $game the game folder?"
    [ -f "$MOD_USDA" ] || die "no $MOD_USDA - the Remix pack is not installed there"
    if [ "$generate" = 1 ] || ! cache_ready; then do_generate; fi
    cache_ready || die "cache at $CACHE is empty after generating"
    mkdir -p "$STATE" "$ASSETS/$TEXDIR"
    : > "$STATE/files.txt"
    n=0; bytes=0
    for f in "$CACHE/$TEXDIR"/*.dds; do
      dst="$ASSETS/$TEXDIR/$(basename "$f")"
      cp -f "$f" "$dst"
      echo "$dst" >> "$STATE/files.txt"
      n=$((n+1)); bytes=$((bytes + $(stat -c%s "$dst")))
    done
    dst="$ASSETS/$LAYER"
    cp -f "$CACHE/$LAYER" "$dst"
    echo "$dst" >> "$STATE/files.txt"
    bytes=$((bytes + $(stat -c%s "$dst")))
    python3 "$REPO/tools/usd_switch.py" check "$dst" >/dev/null
    if [ ! -f "$STATE/mod.usda.bak" ]; then
      cp -p "$MOD_USDA" "$STATE/mod.usda.bak"
    fi
    python3 "$REPO/tools/usd_switch.py" add "$MOD_USDA" "$LAYER" last
    echo "installed $n DDS maps + $LAYER into $ASSETS"
    echo "  $(awk 'BEGIN{printf "%.1f MB on disk, %.1f MB of texture memory with mips", '"$bytes"'/1e6, '"$bytes"'/1e6}')"
    echo "  state: $STATE (files.txt, mod.usda.bak)"
    echo "All checks passed."
    ;;

  off)
    [ -d "$ASSETS" ] || die "no $ASSETS - is $game the game folder?"
    if [ ! -d "$STATE" ]; then
      # no state dir: either nothing was ever installed, or someone installed it by hand. Take
      # out what carries this tool's own names (the layer file and its sublayer line) and say so;
      # the DDS are only removed when files.txt recorded them, so a hand-copy is never guessed at.
      echo "no state dir ($STATE): removing this tool's layer by name only"
      if [ -f "$MOD_USDA" ] && [ "$(python3 "$REPO/tools/usd_switch.py" has "$MOD_USDA" "$LAYER")" = yes ]; then
        python3 "$REPO/tools/usd_switch.py" remove "$MOD_USDA" "$LAYER"
      fi
      if [ -f "$ASSETS/$LAYER" ]; then
        rm -f "$ASSETS/$LAYER"
        echo "removed $ASSETS/$LAYER"
      fi
      echo "All checks passed."
      exit 0
    fi
    removed=0
    while IFS= read -r f; do
      [ -f "$f" ] && rm -f "$f" && removed=$((removed+1))
    done < "$STATE/files.txt"
    rmdir "$ASSETS/$TEXDIR" 2>/dev/null || true
    rmdir "$ASSETS/texture" 2>/dev/null || true   # only succeeds when it is empty (the pack's own
                                                  # texture/ in a real install is not)
    if [ -f "$STATE/mod.usda.bak" ] && [ -f "$MOD_USDA" ]; then
      cp -f "$STATE/mod.usda.bak" "$MOD_USDA"
      echo "mod.usda restored from $STATE/mod.usda.bak"
    elif [ -f "$MOD_USDA" ]; then
      python3 "$REPO/tools/usd_switch.py" remove "$MOD_USDA" "$LAYER"
    fi
    rm -rf "$STATE"
    echo "removed $removed file(s); state dir deleted"
    echo "All checks passed."
    ;;

  status)
    [ -d "$ASSETS" ] || die "no $ASSETS - is $game the game folder?"
    installed=0
    [ -f "$ASSETS/$LAYER" ] && installed=1
    live="no"
    [ -f "$MOD_USDA" ] && live="$(python3 "$REPO/tools/usd_switch.py" has "$MOD_USDA" "$LAYER")"
    ndds=$(ls "$ASSETS/$TEXDIR"/*.dds 2>/dev/null | wc -l || echo 0)
    bytes=0
    if [ -d "$ASSETS/$TEXDIR" ]; then
      bytes=$(du -sb "$ASSETS/$TEXDIR" 2>/dev/null | cut -f1)
      case "$bytes" in ''|*[!0-9]*) bytes=0 ;; esac
    fi
    echo "layer $LAYER present: $installed; referenced by mod.usda: $live"
    echo "DDS installed: $ndds ($((bytes / 1000000)) MB on disk)"
    echo "cache: $(cache_ready && echo "$CACHE ready ($(ls "$CACHE/$TEXDIR"/*.dds | wc -l) maps)" || echo "not generated")"
    echo "selection: $LIST ($(python3 -c "import json,sys;print(len(json.load(open(sys.argv[1]))['textures']))" "$LIST" 2>/dev/null || echo '?') textures)"
    if [ -d "$STATE" ]; then
      echo "state dir: $STATE ($(wc -l < "$STATE/files.txt") file(s), backup: $([ -f "$STATE/mod.usda.bak" ] && echo yes || echo no))"
    else
      echo "state dir: none (nothing installed by this tool)"
    fi
    ;;
esac
