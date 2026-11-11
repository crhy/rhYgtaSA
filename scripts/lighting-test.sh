#!/bin/bash
# One lighting experiment, end to end, with an OBJECTIVE pass/fail.
#
# Why this exists: two runs in a row showed a pure white screen and the only
# report of it was "the screen is white". That is not something to debug by
# asking. This script measures the mean pixel brightness of the frame the game
# is actually presenting, so "white" becomes a number, and it changes exactly
# one variable per run so a failure points at something.
#
# Usage:
#   scripts/lighting-test.sh <variant> [GAME_DIR]
#
# Variants (each differs from 'baseline' by ONE thing):
#   baseline        known-good rtx.conf, no runtime rtx.* pushing at all
#   runtime-sky     baseline + the plugin pushes rtx.skyBrightness/emissiveIntensity
#   autoexp-on      baseline + rtx.autoExposure.enabled = True, EV values untouched
#   autoexp-evneg   baseline + autoExposure on AND evMinValue = -4 (the value I
#                   invented; the shipped file had 0, which is why it is suspect)
#   tuned-rtxconf   the whole config/rtx.conf profile, no runtime pushing
#
# These two are the regression bisect, and they are the ones to run first after
# anything breaks, because they separate "my code" from "the install":
#   good2203        the .asi that last reached gameplay (2026-11-10 21:55 build,
#                   ran 8 minutes), the test ini, and the untouched rtx.conf
#   newasi-oldini   today's .asi, same test ini, same rtx.conf
#
# The script always restores rtx.conf and rhyremix.ini on exit, including on
# Ctrl-C, so a failed or interrupted run never leaves the install broken.
set -uo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
VARIANT="${1:-baseline}"
R="${2:-/media/rhy/data/Gamez/GTASA-RTX}"
export DISPLAY="${DISPLAY:-:0}"
LOG="$R/scripts/rhyremix.log"
INI="$R/scripts/rhyremix.ini"

# How long to wait for the renderer to come up, and how long to then watch it.
BOOT_WAIT="${BOOT_WAIT:-210}"
WATCH_SECS="${WATCH_SECS:-45}"

GOOD_ASI="$HERE/build/tmp/deploy-backup-20261111-015830/rhyremix.asi"
NEW_ASI="$HERE/build/rhyremix.asi"
ASI="$NEW_ASI"
INI_SRC=""

case "$VARIANT" in
  baseline|runtime-sky) RTXCOPY="" ;;                 # keep the install's rtx.conf
  autoexp-on|autoexp-evneg|tuned-rtxconf) RTXCOPY="$HERE/config/rtx.conf" ;;
  good2203)      RTXCOPY="$R/rtx.conf.orig"; ASI="$GOOD_ASI"; INI_SRC="$HERE/config/rhyremix-test.ini" ;;
  newasi-oldini) RTXCOPY="$R/rtx.conf.orig"; ASI="$NEW_ASI"; INI_SRC="$HERE/config/rhyremix-test.ini" ;;
  *) echo "unknown variant: $VARIANT"; exit 2 ;;
esac

if [ ! -f "$ASI" ]; then
  echo "missing asi for variant $VARIANT: $ASI"; exit 2
fi
cp "$ASI" "$R/scripts/rhyremix.asi"
echo "== asi: $(md5sum "$ASI" | cut -c1-8)  $(stat -c '%s bytes, %y' "$ASI")"
if [ -n "$INI_SRC" ]; then
  cp "$INI_SRC" "$INI"
  echo "== ini: $INI_SRC"
fi

# --- apply the one variable under test ------------------------------------
ini_set() { # ini_set <section> <key> <value>
  awk -v sec="$1" -v k="$2" -v v="$3" '
    BEGIN{insec=0; done=0}
    /^[ \t]*\[/{ if(insec && !done){ print k"="v; done=1 } insec=0 }
    /^[ \t]*\['"$1"'\]/{ insec=1; print; next }
    { if(insec && $0 ~ "^[ \t]*"k"[ \t]*="){ print k"="v; done=1; next } print }
    END{ if(insec && !done) print k"="v }' "$INI" > "$INI.tmp" && mv "$INI.tmp" "$INI"
}

cp "$INI" /tmp/rhyremix.ini.testbak
[ -n "$RTXCOPY" ] && cp "$RTXCOPY" "$R/rtx.conf"
case "$VARIANT" in
  baseline)      ini_set sky apply 0 ;;
  runtime-sky)   ini_set sky apply 1 ;;
  autoexp-on)    sed -i 's/^rtx\.autoExposure\.enabled = .*/rtx.autoExposure.enabled = True/' "$R/rtx.conf" ;;
  autoexp-evneg) sed -i 's/^rtx\.autoExposure\.enabled = .*/rtx.autoExposure.enabled = True/; s/^rtx\.autoExposure\.evMinValue = .*/rtx.autoExposure.evMinValue = -4/' "$R/rtx.conf" ;;
esac

restore() {
  pkill -f launch-rtx.sh 2>/dev/null
  pkill -f gta-sa.exe 2>/dev/null
  sleep 2
  pkill -9 -f NvRemixBridge 2>/dev/null
  pkill -9 -f gta-sa.exe 2>/dev/null
  cp /tmp/rhyremix.ini.testbak "$INI"
  [ -f "$R/rtx.conf.orig" ] && cp "$R/rtx.conf.orig" "$R/rtx.conf"
  # Leave the shipped .asi in place, not whichever one the test installed, so an
  # interrupted run cannot leave the install running a bisect binary.
  cp "$HERE/build/rhyremix.asi" "$R/scripts/rhyremix.asi" 2>/dev/null
  echo "== restored rtx.conf, rhyremix.ini and the shipped rhyremix.asi"
}
trap restore EXIT INT TERM

mv -f "$LOG" "$LOG.test-prev" 2>/dev/null
echo "== variant=$VARIANT  (DISPLAY=$DISPLAY)"
echo "== the screen will be taken by the game for about $((BOOT_WAIT + WATCH_SECS))s"
( cd "$R" && nohup timeout "$((BOOT_WAIT + WATCH_SECS + 30))" ./launch-rtx.sh > /tmp/lighting-test-launch.log 2>&1 & )

# --- watch: brightness over time, plus what our own log says --------------
# Brightness is sampled from the whole screen. The game is fullscreen, so the
# frame it presents IS the screen. A white screen reads mean ~1.0.
echo "== sampling brightness every 10s"
samples=()
for ((t = 20; t <= BOOT_WAIT + WATCH_SECS; t += 10)); do
  sleep 10
  if ! pgrep -f gta-sa.exe > /dev/null; then
    echo "   t=${t}s  GAME EXITED (crashed or quit)"
    break
  fi
  if DISPLAY=:0 import -window root /tmp/lt.png 2>/dev/null; then
    read -r mean max <<< "$(identify -format '%[fx:mean] %[fx:maxima]' /tmp/lt.png 2>/dev/null)"
    samples+=("$t:$mean:$max")
    flag=$(awk -v m="$mean" 'BEGIN{ if(m>0.93) print "<== WHITE"; else if(m<0.03) print "<== BLACK"; else print "" }')
    printf '   t=%3ss  mean=%.3f  max=%.3f  %s\n' "$t" "$mean" "$max" "$flag"
  fi
done

# --- verdict --------------------------------------------------------------
reached=""
grep -q 'remixapi_InitializeLibrary' "$LOG" 2>/dev/null && reached="remix-init"
grep -qE 'streetlights: [0-9]+ active' "$LOG" 2>/dev/null && reached="$reached,gameplay"
live=$(grep -oE 'live=[0-9]+' "$LOG" 2>/dev/null | tail -1)

# Separate lines, no nested quoting: the awk programs below contain parentheses
# and question marks, and nesting them inside $( ) inside " " is how this script
# used to fail to parse.
skyline=$(grep -o 'sky: sun elev=[^ ]*' "$LOG" 2>/dev/null | tail -1)
rtxline=$(grep -o 'rtxconf: applied [0-9]*' "$LOG" 2>/dev/null | tail -1)
worst=-1
for s in "${samples[@]}"; do

  rest=${s#*:}
  mean=${rest%%:*}
  worst=$(awk -v a="$worst" -v b="$mean" 'BEGIN{print (b>a)?b:a}')
done

is_white=no
if [ "$worst" != "-1" ]; then
  is_white=$(awk -v w="$worst" 'BEGIN{print (w>0.93)?"yes":"no"}')
fi

echo
echo "== VERDICT variant=$VARIANT"
echo "   renderer:  ${reached:-never came up}"
echo "   lights:    ${live:-none}"
echo "   sky line:  ${skyline:-none}"
echo "   rtxconf:   ${rtxline:-nothing pushed}"
echo "   brightest sample: ${worst}"
if [ "$is_white" = yes ]; then
  echo "   RESULT: WHITE SCREEN"
elif [ -z "$reached" ]; then
  echo "   RESULT: renderer never came up"
else
  echo "   RESULT: rendered, no white screen"
fi
