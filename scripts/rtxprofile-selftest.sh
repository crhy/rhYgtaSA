#!/usr/bin/env bash
# s22 gate for tools/rtxprofile.sh: a fake game tree under build/tmp/, never the real one.
#
# What it proves, in order:
#   1  the key audit passes for config/rtx.conf.perf against the runtime's own option-name
#      vocabulary (config/rtx-runtime-keys.txt, generated from `strings` on .trex/d3d9.dll),
#      and the same audit still FAILS on the two legacy keys in config/rtx.conf - the audit is
#      not vacuous.
#   2  perf -> current and perf -> restore are BYTE-IDENTICAL round-trips (sha256 of the whole
#      file, not a diff), and orig installs the runtime's first-launch file byte for byte.
#   3  backups: a pristine copy is written once and survives repeated switches; prev always
#      holds the previous bytes.
#   4  the switcher refuses a directory with no rtx.conf, and refuses anything inside the repo.
#   5  the perf profile really changes the keys it claims to, and keeps every texture-hash
#      list the asset pack needs.
#   6  n01: the nopops A/B profile installs byte-for-byte and restores byte-for-byte, changes
#      exactly the nine temporal keys it documents, and leaves every look key and texture list
#      of config/rtx.conf alone. Its provenance (tools/rtx-nopops.py) is re-checked too.
set -uo pipefail
cd "$(dirname "$0")/.."
REPO="$PWD"

T="build/tmp/rtxprofile-selftest"
G="$T/game"
rm -rf "$T"; mkdir -p "$G/rtx-remix/logs"

pass=0; fails=()
ok()   { pass=$((pass+1)); echo "PASS $1"; }
bad()  { fails+=("$1"); echo "FAIL $1${2:+  ($2)}"; }
sha()  { sha256sum "$1" | awk '{print $1}'; }

# --- fake install: the shipped rtx.conf, plus a synthetic runtime first-launch file ---------
cp config/rtx.conf "$G/rtx.conf"
sed 's/rtx.emissiveIntensity = .*/rtx.emissiveIntensity = 10/' config/rtx.conf > "$G/rtx.conf.orig"
ORIG=$(sha "$G/rtx.conf")
PERF=$(sha config/rtx.conf.perf)
NOP=$(sha config/rtx.conf.nopops)
TOOL="tools/rtxprofile.sh"

# --- 1 key audit --------------------------------------------------------------------------
if $TOOL check > "$T/check.log" 2>&1; then ok "rtxprofile.sh check: every key of rtx.conf.perf is in the runtime's strings"; else bad "rtxprofile.sh check" "see $T/check.log"; fi
if grep -q "rtx.conf.perf: [1-9][0-9]* keys, 0 unknown" "$T/check.log"; then ok "audit counted the perf keys and found 0 unknown"; else bad "audit did not report 0 unknown for the perf profile"; fi
if grep -q "rtx.enableVolumetricLighting" "$T/check.log"; then ok "audit still flags the legacy key in config/rtx.conf (negative control)"; else bad "audit is vacuous: legacy key not flagged"; fi

# --- 1b the n16 option-table audit ----------------------------------------------------------
# tools/rtxkeycheck.py greps the binary's strings, so it accepts rtx.<category>.<leaf> whenever both
# halves appear somewhere - rtx.tonemap.tonemappingMode (not an option; the option is
# rtx.tonemappingMode) passes it as "leaf+category". tools/rtx-option-audit.py checks the exact
# names against the runtime's own auto-generated table (config/rtx-options-1.6.1.tsv).
if python3 tools/rtx-option-audit.py --quiet config/rtx.conf.perf > "$T/optaudit.log" 2>&1; then
  ok "option-table audit: every key of rtx.conf.perf names a real 1.6.1 option (or is a documented dead key)"
else
  bad "option-table audit on rtx.conf.perf" "see $T/optaudit.log"
fi
printf 'rtx.tonemap.tonemappingMode = 0\nrtx.tonemappingMode = 0\n' > "$T/fakekey.conf"
if python3 tools/rtx-option-audit.py --quiet "$T/fakekey.conf" >/dev/null 2>&1; then
  bad "option-table audit is vacuous: rtx.tonemap.tonemappingMode was accepted"
else
  ok "option-table audit rejects rtx.tonemap.tonemappingMode, the name the n16 task file used"
fi
deadinfo=$(python3 - <<'PY'
import sys
tab = {}
for line in open("config/rtx-options-1.6.1.tsv", encoding="utf-8"):
    if not line.startswith("#"):
        tab[line.split("\t")[0]] = 1
dead = [l.split("\t")[0] for l in open("config/rtx-dead-keys.txt", encoding="utf-8")
        if not l.startswith("#") and l.strip()]
live = [k for k in dead if k in tab]
# 4 since n17: rtx.enableFogMaxDistanceRemap was renamed to the real option
# rtx.volumetrics.enableFogMaxDistanceRemap, so it left the allow-list. If a future task fixes one of
# the four, delete its line here and lower this number; if one is ADDED, say why in the file.
print("%d %s" % (len(dead), "OK" if not live and len(dead) == 4 else "WRONG:" + (",".join(live) or "count %d" % len(dead))))
PY
)
[ "$deadinfo" = "4 OK" ] && ok "the dead-key allow-list is exactly the 4 documented keys and every one is still dead" || bad "dead-key allow-list is wrong ($deadinfo)"

# --- 2/3 switching and round-trips --------------------------------------------------------
$TOOL status "$G" > "$T/status0.log" 2>&1
if grep -q "^profile  : current" "$T/status0.log"; then ok "status identifies the shipped file as 'current'"; else bad "status did not identify 'current'"; fi

$TOOL perf "$G" > "$T/perf.log" 2>&1 || bad "perf switch exited nonzero"
[ "$(sha "$G/rtx.conf")" = "$PERF" ] && ok "perf installed the perf profile byte for byte" || bad "perf did not install the perf profile"
[ -f "$G/.rhys-rtxprofile/rtx.conf.pristine" ] && [ "$(sha "$G/.rhys-rtxprofile/rtx.conf.pristine")" = "$ORIG" ] \
  && ok "pristine backup holds the original bytes" || bad "pristine backup missing or wrong"
[ -f "$G/.rhys-rtxprofile/rtx.conf.prev" ] && [ "$(sha "$G/.rhys-rtxprofile/rtx.conf.prev")" = "$ORIG" ] \
  && ok "prev backup holds the previous bytes" || bad "prev backup missing or wrong"

$TOOL restore "$G" > "$T/restore.log" 2>&1 || bad "restore exited nonzero"
[ "$(sha "$G/rtx.conf")" = "$ORIG" ] && ok "perf -> restore is byte-identical" || bad "perf -> restore changed the file"

$TOOL perf "$G" >/dev/null 2>&1
$TOOL current "$G" > "$T/current.log" 2>&1 || bad "current switch exited nonzero"
[ "$(sha "$G/rtx.conf")" = "$ORIG" ] && ok "perf -> current round-trip is byte-identical" || bad "perf -> current changed the file"

$TOOL orig "$G" > "$T/orig.log" 2>&1 || bad "orig switch exited nonzero"
[ "$(sha "$G/rtx.conf")" = "$(sha "$G/rtx.conf.orig")" ] && ok "orig installs rtx.conf.orig byte for byte" || bad "orig did not install rtx.conf.orig"
$TOOL restore "$G" >/dev/null 2>&1
[ "$(sha "$G/rtx.conf")" = "$ORIG" ] && ok "orig -> restore is byte-identical" || bad "orig -> restore changed the file"

# repeated switches must not churn the pristine copy; restore is a ONE-STEP undo, so after two
# perf switches it puts the perf file back again and `current` is what returns the original
$TOOL perf "$G" >/dev/null 2>&1; $TOOL perf "$G" >/dev/null 2>&1
[ "$(sha "$G/.rhys-rtxprofile/rtx.conf.pristine")" = "$ORIG" ] && ok "pristine copy survives repeated switches" || bad "pristine copy was overwritten"
[ "$(sha "$G/.rhys-rtxprofile/rtx.conf.prev")" = "$PERF" ] && ok "prev holds the last switch source" || bad "prev is wrong after repeated switches"
$TOOL restore "$G" >/dev/null 2>&1
[ "$(sha "$G/rtx.conf")" = "$PERF" ] && ok "restore is a one-step undo" || bad "restore did not undo one step"
$TOOL current "$G" >/dev/null 2>&1
[ "$(sha "$G/rtx.conf")" = "$ORIG" ] && ok "current returns the original bytes after repeated switches" || bad "current did not return the original bytes"

# --- 4 refusals ---------------------------------------------------------------------------
mkdir -p "$T/notagame"
$TOOL perf "$T/notagame" >/dev/null 2>&1 && bad "accepted a dir with no rtx.conf" || ok "refuses a dir with no rtx.conf"
$TOOL perf "$REPO" >/dev/null 2>&1 && bad "accepted a path inside the repo" || ok "refuses to write inside the repo"

# --- 5 what the profile actually changes --------------------------------------------------
$TOOL perf "$G" > /dev/null 2>&1
get() { grep -m1 -E "^[[:space:]]*$2[[:space:]]*=" "$1" | tr -d '\r' | sed 's/.*=[[:space:]]*//'; }
[ "$(get "$G/rtx.conf" rtx.pathMinBounces)" = "1" ] && ok "pathMinBounces 0 -> 1" || bad "pathMinBounces not raised to 1"
[ "$(get "$G/rtx.conf" rtx.spatiallyHashedRadianceCache.qualityPreset)" = "0" ] && ok "SHaRC qualityPreset = 0 (Medium)" || bad "SHaRC preset missing"
[ "$(get "$G/rtx.conf" rtx.neuralRadianceCache.qualityPreset)" = "0" ] && ok "NRC qualityPreset = 0 (Medium)" || bad "NRC preset missing"
[ "$(get "$G/rtx.conf" rtx.volumetrics.froxelGridResolutionScale)" = "2" ] && ok "froxel grid resolution scale = 2" || bad "froxel scale missing"
[ "$(get "$G/rtx.conf" rtx.texturemanager.budgetPercentageOfAvailableVram)" = "40" ] && ok "texture VRAM budget 50% -> 40%" || bad "texture budget missing"
[ "$(get "$G/rtx.conf" rtx.opacityMicromap.cache.maxBudgetSizeMB)" = "768" ] && [ "$(get "$G/rtx.conf" rtx.opacityMicromap.cache.minFreeVidmemMBToNotAllocate)" = "2048" ] \
  && ok "OMM caps tightened (1024->768 MB, 1536->2048 MB free)" || bad "OMM caps wrong"
[ "$(get "$G/rtx.conf" rtx.volumetrics.enable)" = "True" ] && ok "volumetrics kept on under the name this build knows" || bad "volumetrics key wrong"
for k in decalTextures ignoreTextures uiTextures skyBoxTextures particleTextures animatedWaterTextures lightmapTextures; do
  a=$(grep -m1 "rtx.$k = " "$G/rtx.conf" | sha256sum)
  c=$(grep -m1 "rtx.$k = " config/rtx.conf | tr -d '\r' | sha256sum)
  [ "$a" = "$c" ] && ok "texture list rtx.$k carried through unchanged" || bad "texture list rtx.$k changed"
done
for k in sceneScale zUp skyBrightness emissiveIntensity localtonemap.shadows translucentMaterial.normalIntensity \
         tonemappingMode tonemap.exposureBias localtonemap.highlights tonemap.finalizeWithACES; do
  a=$(get "$G/rtx.conf" "rtx.$k"); b=$(get config/rtx.conf "rtx.$k")
  [ "$a" = "$b" ] && ok "look-critical rtx.$k unchanged ($a)" || bad "look-critical rtx.$k changed ($b -> $a)"
done

# --- 6 the n01 temporal A/B profile -------------------------------------------------------
if python3 tools/rtx-nopops.py --check > "$T/nopops-gen.log" 2>&1; then
  ok "config/rtx.conf.nopops is what tools/rtx-nopops.py generates from config/rtx.conf"
else
  bad "config/rtx.conf.nopops drifted from tools/rtx-nopops.py" "see $T/nopops-gen.log"
fi

$TOOL current "$G" > /dev/null 2>&1        # so prev holds the shipped bytes again
$TOOL nopops "$G" > "$T/nopops.log" 2>&1 || bad "nopops switch exited nonzero"
[ "$(sha "$G/rtx.conf")" = "$NOP" ] && ok "nopops installed the A/B profile byte for byte" || bad "nopops did not install the A/B profile"
$TOOL status "$G" > "$T/status-nopops.log" 2>&1
if grep -q "^profile  : nopops" "$T/status-nopops.log"; then ok "status identifies the live file as 'nopops'"; else bad "status did not identify 'nopops'"; fi
$TOOL restore "$G" > /dev/null 2>&1
[ "$(sha "$G/rtx.conf")" = "$ORIG" ] && ok "nopops -> restore is byte-identical" || bad "nopops -> restore changed the file"
$TOOL current "$G" > /dev/null 2>&1
[ "$(sha "$G/rtx.conf")" = "$ORIG" ] && ok "nopops -> current round-trip is byte-identical" || bad "nopops -> current changed the file"

# exactly the nine documented keys differ, and they differ in the documented direction
diffcount=$(python3 - <<'PY'
import re
KEY = re.compile(r"^\s*(rtx\.[A-Za-z0-9_.]+)\s*=\s*(.*)$")
def load(p):
    d = {}
    for line in open(p, encoding="utf-8", errors="replace"):
        m = KEY.match(line)
        if m:
            d[m.group(1)] = m.group(2).strip()
    return d
a, b = load("config/rtx.conf"), load("config/rtx.conf.nopops")
diff = {k for k in set(a) | set(b) if a.get(k) != b.get(k)}
want = {
    "rtx.di.enableTemporalReuse", "rtx.di.enableTemporalBiasCorrection",
    "rtx.spatiallyHashedRadianceCache.maxNumFramesToAccumulate",
    "rtx.volumetrics.maxAccumulationFrames", "rtx.temporalAA.newFrameWeight",
    "rtx.autoExposure.autoExposureSpeed", "rtx.restirGI.historyDiscardStrength",
    "rtx.restirGI.useAdaptiveTemporalHistory", "rtx.restirGI.validateVisibilityChange",
}
print("%d %s" % (len(diff), "OK" if diff == want else "WRONG:" + ",".join(sorted(diff - want | want - diff))))
PY
)
[ "$diffcount" = "9 OK" ] && ok "nopops changes exactly the 9 documented temporal keys" || bad "nopops key diff is wrong ($diffcount)"
for kv in "rtx.restirGI.historyDiscardStrength 2" "rtx.restirGI.useAdaptiveTemporalHistory True" \
          "rtx.restirGI.validateVisibilityChange False" "rtx.di.enableTemporalReuse True" \
          "rtx.di.enableTemporalBiasCorrection True" \
          "rtx.spatiallyHashedRadianceCache.maxNumFramesToAccumulate 16" \
          "rtx.volumetrics.maxAccumulationFrames 16" "rtx.temporalAA.newFrameWeight 0.35" \
          "rtx.autoExposure.autoExposureSpeed 0.5"; do
  set -- $kv
  $TOOL nopops "$G" > /dev/null 2>&1
  v=$(get "$G/rtx.conf" "$1")
  [ "$v" = "$2" ] && ok "nopops sets $1 = $2" || bad "$1 is '$v', expected $2"
done
$TOOL current "$G" > /dev/null 2>&1
# the look keys and the texture lists must be untouched by a temporal profile
for k in sceneScale zUp skyBrightness emissiveIntensity localtonemap.shadows \
         translucentMaterial.normalIntensity graphicsPreset pathMaxBounces \
         demodulate.enableDirectLightBoilingFilter di.permutationSamplingNthFrame \
         usePostFilter postFilterThreshold autoExposure.enabled; do
  a=$(get config/rtx.conf.nopops "rtx.$k"); b=$(get config/rtx.conf "rtx.$k")
  [ "$a" = "$b" ] && ok "nopops leaves the look/sampling key rtx.$k alone ($a)" || bad "nopops changed rtx.$k ($b -> $a)"
done
for k in decalTextures ignoreTextures uiTextures skyBoxTextures particleTextures animatedWaterTextures lightmapTextures hairCardTextures; do
  a=$(grep -m1 "rtx.$k = " config/rtx.conf.nopops | tr -d '\r' | sha256sum)
  c=$(grep -m1 "rtx.$k = " config/rtx.conf | tr -d '\r' | sha256sum)
  [ "$a" = "$c" ] && ok "nopops carries the texture list rtx.$k byte for byte" || bad "nopops altered the texture list rtx.$k"
done

echo
echo "$pass checks passed, ${#fails[@]} failed"
if [ "${#fails[@]}" -ne 0 ]; then echo "FAILURES:"; for f in "${fails[@]}"; do echo "  - $f"; done; exit 1; fi
echo "RTX PROFILE SELFTEST OK"
