#!/bin/bash
# Apply the rhYgtaSA Linux/Proton fixes to a GTA SA RTX install that already has GTASA-Remix (Hemry81) + RTX Remix 1.6.1 in it.
# Usage: ./apply.sh /path/to/GTASA-RTX
set -euo pipefail
R="${1:?usage: apply.sh /path/to/GTASA-RTX}"; HERE="$(cd "$(dirname "$0")" && pwd)"
[ -f "$R/rtx.conf" ] && [ -d "$R/rtx-remix/mods/gameReadyAssets" ] && [ -d "$R/moonloader" ] || { echo "not a GTASA-Remix install: $R"; exit 1; }
B="$R/rhYgtaSA-backup-$(date +%Y%m%d-%H%M%S)"; mkdir -p "$B"
echo "== backing up files the patch touches to $B"
grep '^+++ ' "$HERE/patches/gtasa-remix-linux.patch" | sed -E 's/^\+\+\+ "?b\///; s/"?\t.*$//; s/"$//' | while IFS= read -r f; do
  [ -f "$R/$f" ] && { mkdir -p "$B/$(dirname "$f")"; cp -p "$R/$f" "$B/$f"; }; done
echo "== patching GTASA-Remix files (scripts, light USDs, rtx.conf)"
patch -p1 -d "$R" --forward --binary < "$HERE/patches/gtasa-remix-linux.patch"
echo "== new helper modules"
mkdir -p "$R/moonloader/lib"; cp "$HERE"/moonloader/lib/*.lua "$R/moonloader/lib/"
echo "== stale upstream script (SARemiv_VTK.lua uses pre-cloud model ids) -> backup"
[ -f "$R/moonloader/SARemiv_VTK.lua" ] && mv "$R/moonloader/SARemiv_VTK.lua" "$B/"
echo "== Proton bridge setting"; cp "$HERE/config/bridge.conf" "$R/.trex/bridge.conf"
echo "== MixSets: static ped shadows (real-time shadow = gray square under CJ in Remix)"
[ -f "$R/modloader/MixSets/MixSets.ini" ] && sed -i -E 's/^(UseHighPedShadows\s*=\s*)-1/\10/' "$R/modloader/MixSets/MixSets.ini"
echo "== launcher"; cp "$HERE/launcher/launch-rtx.sh" "$R/"; chmod +x "$R/launch-rtx.sh"
echo "done. Start with: $R/launch-rtx.sh"
