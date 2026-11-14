#!/usr/bin/env python3
"""n01 gate: config/rtx.conf.nopops changes the temporal keys and nothing else.

The whole point of the A/B profile is that a player can switch it and attribute what they
see to the transition, not to the look. That only holds if the file differs from
config/rtx.conf in exactly the keys its own header documents, and if every one of those keys
is a name this build of the renderer actually knows - an unknown key in rtx.conf is silently
ignored, which is how a "tuned" setting does nothing for a week (s22 built
tools/rtxkeycheck.py for exactly that).

Four checks:

1. provenance: the shipped file is byte-for-byte what tools/rtx-nopops.py generates from
   config/rtx.conf, so the profile cannot drift from the diff it claims;
2. the key diff is exactly the nine documented temporal keys, in the documented direction;
3. no look key, sampling key or texture-hash list differs - those are what "the same look"
   means, and the texture lists are the asset pack (s22's rule);
4. every key the profile sets is known to the runtime's own name vocabulary, and the two
   names that look like temporal keys but are not (rtx.restirGI.temporalHistoryLength,
   rtx.autoExposure.evRange) are still unknown - the negative control that keeps this check
   from being vacuous.
"""
import os
import re
import subprocess
import sys
import unittest

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, "tools"))

SHIP = os.path.join(REPO, "config", "rtx.conf")
NOP = os.path.join(REPO, "config", "rtx.conf.nopops")
KEYS = os.path.join(REPO, "config", "rtx-runtime-keys.txt")

KEYRE = re.compile(r"^\s*(rtx\.[A-Za-z0-9_.]+)\s*=\s*(.*)$")

# the nine keys the profile's own header lists, with the value it must set
WANT = {
    "rtx.di.enableTemporalReuse": "True",
    "rtx.di.enableTemporalBiasCorrection": "True",
    "rtx.spatiallyHashedRadianceCache.maxNumFramesToAccumulate": "16",
    "rtx.volumetrics.maxAccumulationFrames": "16",
    "rtx.temporalAA.newFrameWeight": "0.35",
    "rtx.autoExposure.autoExposureSpeed": "0.5",
    "rtx.restirGI.historyDiscardStrength": "2",
    "rtx.restirGI.useAdaptiveTemporalHistory": "True",
    "rtx.restirGI.validateVisibilityChange": "False",
}

# Keys whose value decides what the scene LOOKS like, or which textures the asset pack gets.
# A temporal profile must not touch one of them; the texture lists are checked byte for byte.
LOOK = [
    "rtx.localtonemap.shadows", "rtx.skyBrightness", "rtx.emissiveIntensity",
    "rtx.tonemap.finalizeWithACES", "rtx.translucentMaterial.normalIntensity",
    "rtx.tonemappingMode", "rtx.tonemap.exposureBias", "rtx.localtonemap.highlights",
    "rtx.sceneScale", "rtx.zUp", "rtx.ignoreGameDirectionalLights",
    "rtx.graphicsPreset", "rtx.pathMaxBounces", "rtx.pathMinBounces",
    "rtx.usePostFilter", "rtx.postFilterThreshold",
    "rtx.demodulate.enableDirectLightBoilingFilter",
    "rtx.di.permutationSamplingNthFrame", "rtx.autoExposure.enabled",
    "rtx.neeCache.enableModeAfterFirstBounce", "rtx.russianRouletteMode",
    # n17: the atmosphere. Look keys, so the temporal profile inherits them untouched - but they
    # are the one thing here whose value can move during a session (the density follows the game's
    # own fog end, which SA cross-fades). See the generator's header.
    "rtx.volumetrics.enableFogRemap", "rtx.volumetrics.enableFogMaxDistanceRemap",
    "rtx.volumetrics.fogRemapMaxDistanceMinMeters", "rtx.volumetrics.fogRemapMaxDistanceMaxMeters",
    "rtx.volumetrics.fogRemapTransmittanceMeasurementDistanceMinMeters",
    "rtx.volumetrics.fogRemapTransmittanceMeasurementDistanceMaxMeters",
    "rtx.volumetrics.fogRemapColorMultiscatteringScale",
]
TEXTURES = [
    "rtx.decalTextures", "rtx.ignoreTextures", "rtx.uiTextures", "rtx.skyBoxTextures",
    "rtx.particleTextures", "rtx.animatedWaterTextures", "rtx.lightmapTextures",
    "rtx.hairCardTextures",
]


def parse(path):
    out = {}
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            m = KEYRE.match(line)
            if m:
                out[m.group(1)] = m.group(2).strip()
    return out


class TestRtxNopops(unittest.TestCase):
    def setUp(self):
        self.ship = parse(SHIP)
        self.nop = parse(NOP)

    def test_generated_from_the_shipped_profile(self):
        r = subprocess.run([sys.executable, os.path.join("tools", "rtx-nopops.py"), "--check"],
                           cwd=REPO, capture_output=True, text=True)
        self.assertEqual(r.returncode, 0,
                         "config/rtx.conf.nopops is not what tools/rtx-nopops.py generates: "
                         + (r.stdout + r.stderr).strip())

    def test_exactly_the_documented_temporal_keys_differ(self):
        diff = {k for k in set(self.ship) | set(self.nop) if self.ship.get(k) != self.nop.get(k)}
        self.assertEqual(diff, set(WANT),
                         "unexpected key diff: +%s -%s"
                         % (",".join(sorted(diff - set(WANT))) or "none",
                            ",".join(sorted(set(WANT) - diff)) or "none"))
        for key, val in WANT.items():
            self.assertEqual(self.nop.get(key), val, "%s should be %s" % (key, val))

    def test_no_look_key_or_sampling_key_moves(self):
        moved = [k for k in LOOK if self.ship.get(k) != self.nop.get(k)]
        self.assertEqual(moved, [], "the temporal profile changed look/sampling keys: "
                                    + ", ".join(moved))

    def test_texture_lists_are_byte_identical(self):
        def lines(path):
            with open(path, encoding="utf-8", errors="replace") as f:
                return [l.rstrip("\r\n") for l in f]
        a, b = lines(SHIP), lines(NOP)
        bad = []
        for key in TEXTURES:
            la = [l for l in a if l.strip().startswith(key + " ")]
            lb = [l for l in b if l.strip().startswith(key + " ")]
            if la != lb or not la:
                bad.append(key)
        self.assertEqual(bad, [], "texture category lists differ or missing: " + ", ".join(bad))

    def test_every_key_it_sets_is_known_to_this_runtime(self):
        from rtxkeycheck import load_keys, verdict
        dotted, leaves = load_keys(KEYS)
        unknown = []
        for key in self.nop:
            if verdict(key, dotted, leaves)[0] is None:
                unknown.append(key)
        # config/rtx.conf itself carries two legacy spellings this build dropped (s22); the
        # A/B profile inherits them and nothing else, so the rule is "no more than the ship".
        ship_unknown = [k for k in self.ship if verdict(k, dotted, leaves)[0] is None]
        self.assertEqual(sorted(unknown), sorted(ship_unknown),
                         "nopops unknown keys %s, shipped %s" % (unknown, ship_unknown))

    def test_the_fake_temporal_names_are_still_unknown(self):
        """Negative control: the audit is not vacuous.

        rtx.restirGI.temporalHistoryLength and rtx.autoExposure.evRange read like the keys
        this profile is about, and both appear in the binary as shader argument names - but
        they are not options, and writing them into rtx.conf does nothing.
        """
        from rtxkeycheck import load_keys, verdict
        dotted, leaves = load_keys(KEYS)
        for fake in ("rtx.restirGI.temporalHistoryLength", "rtx.autoExposure.evRange",
                     "rtx.denoiser.indirectMode"):
            self.assertIsNone(verdict(fake, dotted, leaves)[0],
                              "%s is now known - is the vocabulary stale, or did someone "
                              "start trusting a guess?" % fake)
        # and the real ones this profile relies on are known
        for real in WANT:
            self.assertIsNotNone(verdict(real, dotted, leaves)[0],
                                 "%s is not a key this runtime knows" % real)


if __name__ == "__main__":
    unittest.main(verbosity=2)
