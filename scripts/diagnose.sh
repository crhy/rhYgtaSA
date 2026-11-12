#!/usr/bin/env bash
# Read-only verdict on the one question the last session could not answer:
# the plugin said "60 active lights" while bridge64.log said every draw failed.
# Reads both logs and the deployed ini, changes nothing.
# Usage: scripts/diagnose.sh [GAME_DIR]
set -uo pipefail
GAME="${1:-/media/rhy/data/Gamez/GTASA-RTX}"
INI="$GAME/scripts/rhyremix.ini"
LOG="$GAME/scripts/rhyremix.log"

# rhyremix.log lines are prefixed "[timestamp] ", so never anchor at ^ .
ts_grep() { grep -hE "$1" "$LOG" 2>/dev/null | tail -n "${2:-4}"; }

[ -f "$LOG" ] || { echo "no plugin log at $LOG - the game has not run this build yet"; exit 1; }

say() { printf '\n== %s ==\n' "$1"; }

say "ini (what the running build was configured with)"
if [ -f "$INI" ]; then
  grep -nE '^\[|^(enabled|max_lights|max_total_lights|probe|probe_follow|probe_scale|probe_hash|probe_sweep|radiance_scale|bulb_radius|bulb_radiance|glow_radius|glow_flux|corona_radius)=' "$INI" || true
else
  echo "no ini at $INI"
fi

say "remix counters (live / create_fail / draw_fail / budget_refused)"
counters=$(ts_grep 'remix: live=' 3)
if [ -n "$counters" ]; then echo "$counters"; else
  echo "none: this log predates the counters, rebuild and rerun"
fi

say "active vs drawn (active>0 with drawn=0 means the lights exist only in the plugin)"
# Both trafficlights shapes: the list mode prints "trafficlights: N active", the s01/s19 corona
# hook prints "trafficlights[source=game]: N heads lit". Matching only the first made the module
# look absent on exactly the runs where the hook was live.
ts_grep '(streetlights|trafficlights)(\[[^]]*\])?: [0-9]+ (active|heads lit)' 4 || echo "no module lines"

say "signal heads (s01/s19/s30/n02/n02b: the patch, the bulb AND the glow, the hook's counters, the source decision; reentrant must stay 0)"
ts_grep 'signalhook: |trafficlights\source=game\]|corona radius|bulb radius=|glow radius=|ok glow |ok bulb |with a glow|hook live but no head|head source =|loaded [0-9]+ lights' 8 || \
  echo "no signal lines: no signalised intersection in range, daytime with night_only=1, or source=list"
# n02b: a lit signal is two lights. "N heads lit (M with a glow)" with M < N names a glow
# create that failed (frame budget or max_total_lights), not a missing feature.
glowlag=$(ts_grep 'heads lit \([0-9]+ with a glow\)|[0-9]+ active \([0-9]+ with a glow\)' 1 | \
          sed -nE 's/.*lit \(([0-9]+) with a glow\).*/\1/p; s/.*active \(([0-9]+) with a glow\).*/\1/p' | tail -1)
headsn=$(ts_grep 'heads lit \([0-9]+ with a glow\)|[0-9]+ active \([0-9]+ with a glow\)' 1 | \
          sed -nE 's/.*: ([0-9]+) heads lit.*/\1/p; s/.*: ([0-9]+) active.*/\1/p' | tail -1)
if [ -n "$glowlag" ] && [ -n "$headsn" ] && [ "$glowlag" -lt "$headsn" ]; then
  echo "  ^ $headsn lit signals but only $glowlag have a glow: the glow create is being refused"
  echo "    (frame budget or max_total_lights). 'budget_stop=' and 'deferred=' on the same line say which."
fi
if ts_grep 'glow radius=0.00 flux=0' 1; then
  echo "  ^ glow_flux=0 in the ini: bulb only. The 2026-11-12 look wants flux=304 (the default)."
fi
if ts_grep 'head source = list' 1; then
  echo "  ^ the hook was given up on: a signal was inside the game's own 50 m corona range and still no head"
  echo "    appeared. 'nearest signal X m' on that line is the number to believe; 79.1 m is the default spawn,"
  echo "    where the game draws no signal corona either, and then the hook is kept, not dropped."
fi
if ts_grep 'are v1 .x y z headz. and stay on the mast' 1; then
  echo "  ^ the installed rhyremix_traffic.txt is the old four-column file: regenerate it with"
  echo "    tools/traffic_list.py (v2 adds heading + lens offset, which is what puts the bulb on the lens)."
fi
if ts_grep 'the bytes at 0x49DD4C are not' 2; then
  echo "  ^ on the shipped exe that line is a plugin bug, not a game change: the bytes there are E8 2F E8 25 00 (fixed in s30)"
fi

say "modules that gave up (they retry on the next rhyremix.ini edit)"
ts_grep 'rejected|disabled until|refusing a light' 6 || echo "none"

say "probe (the calibration rig: what it asked for, and when)"
ts_grep 'probe: ' 6 || echo "no probe lines: set probe=1 in the ini"

say "sky (sun and moon are the control: different code path from a point light)"
ts_grep 'sky: ' 2 || echo "no sky lines"

# Every bridge log, not just the newest: bridge32.log and bridge64.log coexist
# and the interesting one is whichever has failures.
say "bridge logs: the reason the server rejected things, which our log never shows"
shopt -s nullglob
bridges=("$GAME"/rtx-remix/logs/bridge*.log)
if [ ${#bridges[@]} -eq 0 ]; then
  echo "no bridge log under $GAME/rtx-remix/logs/"
else
  for b in "${bridges[@]}"; do
    printf '%s: create_failed=%s invalid_handle=%s api_failed=%s\n' \
      "$(basename "$b")" \
      "$(grep -c 'CreateLight.*failed' "$b" 2>/dev/null || true)" \
      "$(grep -c 'Invalid light handle' "$b" 2>/dev/null || true)" \
      "$(grep -c 'Remix API call failed' "$b" 2>/dev/null || true)"
  done
  for b in "${bridges[@]}"; do
    grep -hE 'failed|Invalid|error' "$b" 2>/dev/null | tail -3 | sed "s|^|$(basename "$b"): |"
  done
  # How many invalid draws share one timestamp: 1 means a single handle drawn
  # every frame (one bad light), N means N lights are bad. This is the difference
  # between "fix one light's parameters" and "the whole set was rejected".
  for b in "${bridges[@]}"; do
    n=$(grep -c 'Invalid light handle' "$b" 2>/dev/null || true)
    if [ "${n:-0}" -gt 0 ]; then
      printf '%s: max invalid draws sharing one timestamp = %s\n' \
        "$(basename "$b")" \
        "$(grep 'Invalid light handle' "$b" | cut -d']' -f1 | uniq -c |
         sort -rn | head -1 | awk '{print $1}')"
    fi  done
fi

# The verdict, in the order the rungs of the ladder answer them.
say "verdict"
last=$(echo "$counters" | tail -1)
live=$(printf '%s' "$last" | sed -n 's/.*live=\([0-9]*\).*/\1/p')
draw=$(printf '%s' "$last" | sed -n 's/.*draw_fail=\([0-9]*\).*/\1/p')
bridge_bad=0
for b in "${bridges[@]}"; do
  n=$(grep -c 'Invalid light handle' "$b" 2>/dev/null || true)
  bridge_bad=$((bridge_bad + ${n:-0}))
done
if [ -z "$live" ]; then
  echo "no counter lines: this log is from a build before the wrappers - rebuild and rerun"
elif [ "${draw:-0}" -gt 0 ] || [ "$bridge_bad" -gt 0 ]; then
  per_ts=$(grep -h 'Invalid light handle' "${bridges[@]}" 2>/dev/null | cut -d']' -f1 | uniq -c | sort -rn | head -1 | awk '{print $1}')
  echo "the bridge handed us handles for lights the Remix server never accepted."
  echo "live=$live draw_fail=$draw bridge_invalid=$bridge_bad max_per_timestamp=${per_ts:-0}"
  if [ "${per_ts:-0}" -le 2 ]; then
    echo "but only ~1 handle per frame is bad: ONE light is rejected, the rest are"
    echo "being drawn fine. Fix that light's parameters (the probe line above), not"
    echo "the whole set - the lamps are not the failing ones."
  else
    echo "Next rung: probe alone (streetlights/traffic off). If the probe also fails,"
    echo "the parameters are wrong, not the count. If the probe shows and the modules"
    echo "do not, lower max_lights / max_total_lights: the renderer has a budget we"
    echo "have not measured."
  fi
elif [ "${live:-0}" -eq 0 ]; then
  echo "no live handles: every module is off, or every create was refused."
else
  echo "no rejected draws in this log. A light that is created, drawn and still"
  echo "invisible is a units or brightness question, not a handle question:"
  echo "rtx.sceneScale=0.01 means Remix works in metres while the lamp list is in"
  echo "game units, so a lamp can be created and drawn 2.5 km away. probe_scale=0.01"
  echo "vs 1 decides that in one run each; probe_sweep then walks radiance x1..x1000."
fi
