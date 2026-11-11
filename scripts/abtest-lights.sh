#!/usr/bin/env bash
# A/B the light, gamma-guard, probe and sun switches in the deployed rhyremix.ini
# without rebuilding: same build, one setting changed, one run each way.
# Usage: scripts/abtest-lights.sh <lights|gamma|probe|sun|all> <on|off> [GAME_DIR]
#        scripts/abtest-lights.sh status [GAME_DIR]
set -euo pipefail
GAME="${3:-/media/rhy/data/Gamez/GTASA-RTX}"
INI="$GAME/scripts/rhyremix.ini"
WHAT="${1:?usage: abtest-lights.sh <lights|gamma|probe|sun|all> <on|off> | status}"
STATE="${2:-}"
[ -f "$INI" ] || { echo "no ini at $INI (run ./deploy.sh --test --force-ini first)"; exit 1; }

show() {
  grep -nE '^\[|^(enabled|block_gamma|probe|probe_radiance|probe_radius|radius|radiance_big|radiance_middle|radiance_small|traffic_enabled|traffic_max_lights)=' "$INI" || true
}

if [ "$WHAT" = status ]; then show; exit 0; fi
[ -n "$STATE" ] || { echo "usage: abtest-lights.sh <lights|gamma|probe|sun|all> <on|off>"; exit 1; }

# Section-aware rewrite: the same key name appears in several sections
# ([general] enabled, [trafficlights] enabled, [sun] enabled, [moon] enabled),
# so a plain sed on "enabled=" would hit the wrong one.
apply() {
  python3 - "$INI" "$WHAT" "$STATE" <<'PY'
import sys, re
path, what, state = sys.argv[1], sys.argv[2], sys.argv[3]
val = '1' if state == 'on' else '0'
if state not in ('on', 'off'):
    sys.exit('state must be on or off')

# (section, key) pairs each switch controls
targets = {
    'lights': [('streetlights', 'enabled')],
    'gamma':  [('streetlights', 'block_gamma')],
    'probe':  [('streetlights', 'probe')],
    'sun':    [('sun', 'enabled'), ('moon', 'enabled')],
}[what]

lines = open(path).read().splitlines(True)
sec = 'streetlights'    # keys before any [section] belong to the default one
out, seen = [], []
for line in lines:
    s = line.strip()
    if s.startswith('[') and s.endswith(']'):
        sec = s[1:-1].lower()
    else:
        eq = s.find('=')
        if eq > 0:
            key = s[:eq].strip()
            for ts, tk in targets:
                if sec == ts and key == tk:
                    line = f'{key}={val}\n'
                    seen.append(f'{sec}.{key}={val}')
    out.append(line)
open(path, 'w').writelines(out)
if not seen:
    sys.exit('no matching key found for ' + what)
print('set ' + ', '.join(seen))
PY
}

cp "$INI" "$INI.abtest-prev"
apply
echo
echo "now active:"
show
echo
echo "restart the game, then compare. Previous ini saved as $INI.abtest-prev"
case "$WHAT" in
  lights) echo "lights off: if a symptom (too bright, no cast, crash) survives with no"
          echo "          lights at all, it is not caused by our light code." ;;
  gamma)  echo "gamma off: SetGammaRamp is forwarded again. If Ctrl+PrintScreen now"
          echo "        crashes and block_gamma=1 did not, the guard is not the cause." ;;
  probe)  echo "probe on: one white light at probe_x/y/z every frame. Invisible -> radiance"
          echo "        too low; blown out -> too high. Dial by factors of 10." ;;
  sun)    echo "sun off: no distant light from the ASI. If the scene is still too bright"
          echo "       at night, the light is coming from the game's own sun or the lamps." ;;
esac
