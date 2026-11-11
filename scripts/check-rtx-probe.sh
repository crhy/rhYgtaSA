#!/bin/bash
# Cross-reference what rhyremix SENT against what RTX Remix's renderer actually
# accepted, and print the honest verdict per key.
#
# Why this exists: remixapi_SetConfigVariable returns SUCCESS for keys that the
# renderer then refuses. Measured on Remix 1.6.1 - a run sent 60 keys, the client
# reported 60 accepted, and the server logged "is not an RtxOption or a key in a
# registered dynamic namespace" for twelve of them. Anyone reading only the
# plugin's log would conclude those twelve settings were applied, and would then
# spend a day wondering why a change to rtx.userBrightness did nothing.
#
# Usage:
#   scripts/check-rtx-probe.sh [GAME_DIR]
#     reads scripts/rhyremix.log and rtx-remix/logs/remix-dxvk.log
#
# Run with [sky] probe=1 in rhyremix.ini first (see ./deploy.sh --rtx-probe),
# then run this.
set -uo pipefail
R="${1:-/media/rhy/data/Gamez/GTASA-RTX}"
PLUGIN_LOG="$R/scripts/rhyremix.log"
SERVER_LOG="$R/rtx-remix/logs/remix-dxvk.log"

for f in "$PLUGIN_LOG" "$SERVER_LOG"; do
  if [ ! -f "$f" ]; then
    echo "missing log: $f" >&2
    echo "launch the game once first (launch-rtx.sh), with [sky] probe=1 for a key audit" >&2
    exit 2
  fi
done

# Keys the server explicitly said it does not take at runtime.
rejected=$(grep -o "'rtx\.[A-Za-z0-9_.]*' is not an RtxOption" "$SERVER_LOG" \
           | sed "s/'//g; s/ is not an RtxOption//" | sort -u)

# Keys the plugin sent.
sent=$(grep -o 'rtxconf probe: rtx\.[A-Za-z0-9_.]*' "$PLUGIN_LOG" \
       | awk '{print $3}' | sort -u)
# ... plus keys pushed from rhyremix-rtx.conf, which log a line only when refused.
sent_file=$(grep -oE "rtxconf: (refusing to push|.*rejected: unknown key) '?rtx\.[A-Za-z0-9_.]*" "$PLUGIN_LOG" \
            | grep -oE 'rtx\.[A-Za-z0-9_.]*' | sort -u)

all=$(printf '%s\n%s\n%s\n' "$sent" "$rejected" "$sent_file" | grep -E '^rtx\.' | sort -u)

if [ -z "$all" ]; then
  echo "no rtxconf probe lines in $PLUGIN_LOG"
  echo "enable the audit with:  ./deploy.sh --rtx-probe   then launch once"
  exit 1
fi

runtime_ok=0
startup_only=0
printf '%-56s %s\n' 'KEY' 'VERDICT'
printf '%-56s %s\n' '---' '-------'
while IFS= read -r k; do
  [ -z "$k" ] && continue
  if echo "$rejected" | grep -qx -- "$k"; then
    printf '%-56s %s\n' "$k" 'STARTUP ONLY - put it in rtx.conf, runtime push does nothing'
    startup_only=$((startup_only + 1))
  else
    printf '%-56s %s\n' "$k" 'accepted by the renderer at runtime'
    runtime_ok=$((runtime_ok + 1))
  fi
done <<< "$all"

echo
echo "== $runtime_ok settable at runtime, $startup_only startup-only"
echo
echo "Reminder: 'accepted at runtime' is not the same as 'safe at runtime'. Keys that"
echo "switch a renderer subsystem (graphicsPreset, raytraceModePreset, qualityDLSS,"
echo "enableRayReconstruction, neuralRadianceCache.*, volumetrics.*, viewDistance)"
echo "reconfigure the renderer while it is rendering, and a run that pushed them"
echo "white-screened and then took the bridge down with 0xe06d7363. Those belong in"
echo "rtx.conf. See plugin/src/rtxconf.cpp, rhyRtxKeyIsUnsafe."
