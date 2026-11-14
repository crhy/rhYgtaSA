#!/usr/bin/env python3
"""Generate config/rtx.conf.nopops from config/rtx.conf (n01, step 4).

Everything this does is (a) prepend a comment block, (b) change three existing
values, (c) append six keys. Every other byte - including all eight texture-hash
lists and every look key - is copied through, which is what
scripts/rtxprofile-selftest.sh hashes and tests/test_rtx_nopops.py asserts.

  python3 tools/rtx-nopops.py            write the profile
  python3 tools/rtx-nopops.py --check    exit nonzero if the shipped file is not
                                         what this generator produces (so the
                                         profile in config/ cannot drift from its
                                         documented provenance)
"""
import re
import sys

SRC = "config/rtx.conf"
DST = "config/rtx.conf.nopops"

HEADER = """# rhyremix "no pops" A/B profile - n01. Install with:
#   tools/rtxprofile.sh nopops [GAME_DIR]        undo: tools/rtxprofile.sh restore [GAME_DIR]
#
# This is config/rtx.conf with ONLY the temporal keys changed: the keys that decide WHEN a
# lighting change lands, not what the light looks like. It exists to be A/B'd against
# `current` with scripts/lighting-test.sh (the drive-through of the 2026-11-12 test session),
# because the tour takes every shot at a standstill and therefore cannot see a transition.
#
# Evidence for everything below: .strata/notes-n01-lighting-no-pops-renderer.md step 2, which
# lists the keys of config/rtx.conf with their values in <game>/rtx.conf.orig (the runtime's own
# first-launch file), the verdict of tools/rtxkeycheck.py against config/rtx-runtime-keys.txt,
# and the tooltip each key carries in the runtime's strings.
#
# Which temporal path is live here, from <game>/rtx-remix/logs/remix-dxvk.log:
#   "Integrate Indirect Mode: Spatially Hashed Radiance Cache - activated"  (SHaRC)
#   "NVIDIA DLSS-NR support query failed (0xbad00010)"                      (no Ray Reconstruction)
#   "[RTX Neural Radiance Cache] Selected Ultra preset mode."               (trained, not queried)
# So the live time path is SHaRC + the RTXDI (rtx.di.*) direct-light resampler + the volumetric
# accumulator + the final temporal AA. The rtx.restirGI.* block the shipped file carries belongs
# to an integrator this install does not run.
#
# CHANGED - nine keys, all of them temporal:
#   rtx.di.enableTemporalReuse = True              "Whether to apply temporal reuse."
#   rtx.di.enableTemporalBiasCorrection = True     "Corrects bias caused by temporal reprojection."
#       Direct light is what this plugin publishes (one distant light per body, the lamps, the
#       signal heads). Reusing samples across frames is what makes a change converge over frames
#       instead of arriving at full strength in one.
#   rtx.spatiallyHashedRadianceCache.maxNumFramesToAccumulate = 16
#       "Maximum number of frames accumulated in each SHaRC entry. Higher values reduce noise but
#        respond more slowly to lighting changes."
#   rtx.volumetrics.maxAccumulationFrames = 16
#       "The number of frames to accumulate volume lighting samples over, maximum of 254. Large
#        values result in greater image stability at the cost of potentially more temporal lag."
#       (rtx.enableVolumetricLighting = True is in this file, so light shafts are on the table.)
#   rtx.temporalAA.newFrameWeight = 0.35
#       "The maximum amount of the current frame to use as part of the temporal anti-aliasing
#        process ... closer to 0 will result in better image stability ... closer to 1 will
#        result in more responsive results (less ghosting)." The one whole-image lever here.
#   rtx.autoExposure.autoExposureSpeed = 0.5
#       "Average exposure changing speed (in units per second) when the image changes." Inert
#       while rtx.autoExposure.enabled stays False; set so that if exposure is ever switched on
#       it ramps instead of snapping.
#   rtx.restirGI.historyDiscardStrength             10 -> 2
#   rtx.restirGI.useAdaptiveTemporalHistory     False -> True
#   rtx.restirGI.validateVisibilityChange         True -> False
#       "The sensitivity of discarding history. Higher values discard more history." / "Adjust
#        temporal history length based on frame rate." / "Remove samples when visibility has
#        changed." These are the amplifiers s28 named plus their sibling: at 10 with adaptive
#        history off, a lighting change throws the temporal history away and lands at full
#        strength the next frame. They are INERT while SHaRC is the integration mode, and are
#        aligned anyway so the A/B covers them and the file is right if the integrator changes.
#
# DELIBERATELY NOT CHANGED:
#   rtx.demodulate.enableDirectLightBoilingFilter stays False. Its tooltip is "Boiling filter
#       removing direct light sample when its luminance is too high" - every light this plugin
#       publishes is direct light, and tour shots 02-04 of 2026-11-12 are sodium lamps at night:
#       the filter would remove the brightest samples of exactly the shots rhY called good.
#   rtx.di.permutationSamplingNthFrame is carried through unchanged. n01 left it at
#       1 ("a noise knob, not a lighting-step knob"); n04 moved the SHIPPED value to 0,
#       because "Apply permutation sampling when (frameIdx % this == 0), 0 means off"
#       re-randomises which lights a pixel draws every frame and over the 400-550 lamp
#       pool this plugin keeps live that is finding A. nopops inherits whatever the
#       shipped file says, so the A/B stays a temporal-only diff.
#   rtx.usePostFilter / rtx.postFilterThreshold stay False / 10 (an NRC training-clamp pair).
#   Every look key is byte-identical to config/rtx.conf, including the n16 tonemap block
#       (tonemappingMode 0 = Global, tonemap.exposureBias -0.5, localtonemap.shadows 1.0,
#       localtonemap.highlights 2.0, tonemap.finalizeWithACES True). n16 changed the shipped
#       localtonemap.shadows from 2.5 to 1.0 - it amplifies a step in direct light, and at 2.5 it
#       lifted the floor - and this profile inherits whatever the shipped file says, so the A/B
#       stays a temporal-only diff either way. Also inherited unchanged: skyBrightness,
#       emissiveIntensity, translucentMaterial.normalIntensity, sceneScale, zUp,
#       ignoreGameDirectionalLights, graphicsPreset, and all eight texture-hash lists.
#       scripts/rtxprofile-selftest.sh hashes the lists and asserts the look keys and
#       graphicsPreset come through unchanged.
#   The n24 night keys are inherited the same way (fallbackLightMode 0 + fallbackLightRadiance
#       0,0,0 with the old type/radius commented out, enableTransmissionApproximationInIndirectRays
#       False, tonemap.shadowContrast 0.4, bloom.luminanceThreshold 1.0). None of them is
#       temporal: they decide what the night is lit BY, not when a change lands, so this A/B
#       must not move them - and a nopops run that shows a different night than `current` is a
#       bug in this file, not a finding.
#   WHY n16 MATTERS FOR THIS A/B: with rtx.tonemappingMode = 0 the local tonemapper is not
#       dispatched at all (rtx_local_tone_mapping.cpp:144), so the whole-image curve in
#       rtx_tone_mapping.cpp is what runs, and that curve is mixed with the previous frame's at
#       exp(-0.07) - about 14 frames to settle (tonemapping_tone_curve.comp.slang, "Temporal
#       filtering"). That is a second, independent smoothing of a lighting change, on top of the
#       nine temporal keys this profile moves. Read the A/B with that in mind: if a step now lags,
#       it may be the curve, not the reservoirs.
#   The n17 atmosphere block is carried through byte-identical (enableFogRemap True, the four
#       fogRemap bounds 400/4000 m in and 2/6 m out, fogRemapColorMultiscatteringScale 0, and the
#       fixed rtx.volumetrics.enableFogMaxDistanceRemap). It is a look key, so it is not part of
#       this A/B - but it is the one thing in the shipped profile whose value can move DURING a
#       session without the plugin touching it: the density follows the game's own fog end
#       (data/timecyc.dat FarClp), which SA cross-fades between weather types, and the runtime
#       re-derives sigma from the frame's fog state (rtx_global_volumetrics.cpp:470-540). A scripted
#       instant weather change therefore moves the medium in one frame, and plugin/src/smooth.cpp
#       cannot rate-limit it: rtxconf.cpp refuses every "volumetrics." key at runtime (kUnsafeKeys,
#       :245-252 - pushing that subsystem mid-run killed the bridge server once). If a haze pop on a
#       weather change shows up in this A/B, the fix is the game's data (timecyc rows), not a key
#       here. Density arithmetic: python3 tools/fog-budget.py config/rtx.conf.nopops.
#
# HOW TO READ THE A/B: scripts/lighting-test.sh, driving under the lamps at dusk with
# [clock] apply=1. Two things the plugin cannot fix from its own side: the shadow-direction jump
# when a distant-light handle is replaced, and the ambient step when rtx.skyBrightness moves. If
# a change now lags visibly (a lamp taking a quarter second to reach full brightness), lower
# maxNumFramesToAccumulate and maxAccumulationFrames first and newFrameWeight last: the last one
# is the whole-image blend and is also the one that can ghost when the camera moves.
#
# Read the runtime's own log after the first launch of any profile:
#   grep -A80 'Effective RtxOption values' <game>/rtx-remix/logs/remix-dxvk.log
# The graphics-preset layer (rtx.graphicsPreset = 1, not Custom) sits ABOVE rtx.conf for the keys
# it sets: if a key here does not read as changed in the Remix UI, the preset owns it.
#
"""

# (existing key, old value, new value) - each must appear exactly once.
VALUE_CHANGES = [
    ("rtx.restirGI.historyDiscardStrength", "10", "2"),
    ("rtx.restirGI.useAdaptiveTemporalHistory", "False", "True"),
    ("rtx.restirGI.validateVisibilityChange", "True", "False"),
]

# Keys this profile adds. Every one of them is in config/rtx-runtime-keys.txt:
# python3 tools/rtxkeycheck.py --keys config/rtx-runtime-keys.txt config/rtx.conf.nopops
NEW_KEYS = [
    ("rtx.di.enableTemporalReuse", "True"),
    ("rtx.di.enableTemporalBiasCorrection", "True"),
    ("rtx.spatiallyHashedRadianceCache.maxNumFramesToAccumulate", "16"),
    ("rtx.volumetrics.maxAccumulationFrames", "16"),
    ("rtx.temporalAA.newFrameWeight", "0.35"),
    ("rtx.autoExposure.autoExposureSpeed", "0.5"),
]

NEW_BLOCK = (
    "\r\n\r\n# n01: the live temporal keys the shipped profile never wrote (see the header).\r\n"
    + "".join(k + " = " + v + "\r\n" for k, v in NEW_KEYS)
)


def render():
    raw = open(SRC, "rb").read().decode("utf-8")
    for key, old, new in VALUE_CHANGES:
        pat = r"(?m)^(\s*" + re.escape(key) + r"\s*=\s*)" + re.escape(old) + r"(\s*)$"
        hits = re.findall(pat, raw)
        if len(hits) != 1:
            sys.exit("%s = %s found %d times in %s" % (key, old, len(hits), SRC))
        raw = re.sub(pat, r"\g<1>" + new + r"\g<2>", raw)
    return (HEADER + raw + NEW_BLOCK).encode("utf-8")


def main():
    check = "--check" in sys.argv[1:]
    data = render()
    if check:
        try:
            have = open(DST, "rb").read()
        except OSError as e:
            sys.exit("--check: %s" % e)
        if have != data:
            sys.exit("--check: %s is not what tools/rtx-nopops.py generates "
                     "(%d bytes there, %d here)" % (DST, len(have), len(data)))
        print("--check: %s matches tools/rtx-nopops.py (%d value changes, %d new keys)"
              % (DST, len(VALUE_CHANGES), len(NEW_KEYS)))
        return
    open(DST, "wb").write(data)
    print("wrote %s from %s: %d value changes, %d new keys"
          % (DST, SRC, len(VALUE_CHANGES), len(NEW_KEYS)))


if __name__ == "__main__":
    main()
