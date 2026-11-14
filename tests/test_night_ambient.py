#!/usr/bin/env python3
"""n24 gate: at night every photon in the scene comes from a source you could point at.

    python3 -m pytest -q tests/test_night_ambient.py

This is the narrower retry of the parked n10 (`30a1a43`, docs/FINDINGS.md "Night realism:
what n05 and n10 measured and did **not** ship"). n10's four causes of rhY's V1 ("a FAKE
night ambient everywhere - light from no source", .strata/test-2026-11-12c.md) were the local
tonemapper, the sky probe's ambient ceiling, the fallback light welded to the camera, and a
distant light pointing up out of the ground. Cause 1 shipped as n16 (`rtx.tonemappingMode =
0`); this file gates causes 2, 3 and 4 plus the V4 glow and the one taste number, on today's
tree (n16's tonemap block, n17's atmosphere, n22's exposure anchor, n23's lamps at 0.4).

What is checked, and where the numbers come from:

  A. the sources - no fallback light anywhere (mode 0 + zero radiance, the shape commented
     out), and the night ambient capped at 0.25x the moon light's own published radiance
     ([sky] ambient_cap_mult, enforced in plugin/src/solar.cpp skyAmbientCap);
  B. the chain - Global tone mapping (n16, regression guard), rtx.tonemap.shadowContrast 0.4
     and rtx.bloom.luminanceThreshold 1.0 (both measured here), no transmission shortcut
     through the sea;
  C. the ratio - tools/night_ambient.py's own numbers, scene-linear and after the tone chain;
  D. negative controls - the shipped profile put back the way it was must FAIL the ratio, so
     the assertions above cannot be passing because the scene got brighter;
  E. the three shipped profiles carry the n24 keys identically, so switching profile cannot
     change what the night is lit by.

The plugin half of the change (the cap, the direction clamp, and the fact that the cap moves
a TARGET that then goes through the s28 rate limiters) is asserted natively in
tests/solar_check.cpp; this file cannot see the plugin, it sees the config the plugin is
given and the scene that config produces.
"""
import os
import re
import sys
import unittest

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, "tools"))

import night_ambient as na  # noqa: E402

CONF = os.path.join(REPO, "config", "rtx.conf")
PERF = os.path.join(REPO, "config", "rtx.conf.perf")
NOP = os.path.join(REPO, "config", "rtx.conf.nopops")
INI = os.path.join(REPO, "config", "rhyremix-production.ini")

# what n24 ships, in every profile (docs/CHANGELOG.md, docs/FINDINGS.md)
N24_KEYS = {
    "rtx.fallbackLightMode": "0",                  # the runtime's "Never"
    "rtx.fallbackLightRadiance": "0, 0, 0",        # and nothing armed if the mode flips
    "rtx.enableTransmissionApproximationInIndirectRays": "False",   # runtime default
    "rtx.tonemap.shadowContrast": "0.4",           # the taste number, measured below
    "rtx.bloom.luminanceThreshold": "1.0",         # runtime default 0.25
}


def conf_raw(path):
    with open(path, encoding="utf-8", errors="replace") as f:
        return f.read()


class TestSources(unittest.TestCase):
    """A. nothing may light the ground without a source in the world."""

    def setUp(self):
        self.conf = na.parse_conf(CONF)
        self.ini = na.parse_ini(INI)
        self.sc = na.Scene(self.ini, self.conf, na.pier_lamps())

    def test_no_fallback_light(self):
        """rtx_light_manager.cpp:249-330 hangs a light on the camera when the mode says
        so; the runtime's own tooltip (config/rtx-options-1.6.1.tsv:299) asks for Never in
        a production build. Mode 0, radiance 0, and the camera-sphere shape gone."""
        self.assertEqual(int(self.conf.get("rtx.fallbackLightMode", 1)), 0,
                         "rtx.fallbackLightMode must be 0 (Never): a camera-attached "
                         "sphere light is the definition of light from no source")
        rad = self.conf.get("rtx.fallbackLightRadiance", (1.6, 1.8, 2.0))
        if isinstance(rad, float):
            rad = (rad,) * 3
        self.assertEqual(tuple(round(x, 6) for x in rad), (0.0, 0.0, 0.0),
                         "rtx.fallbackLightRadiance must be 0,0,0 as well: mode 0 is "
                         "inert only while the runtime's default 1.6,1.8,2 is not left "
                         "armed for a mode switch in the Remix UI, which writes the user "
                         "layer that outranks rtx.conf")
        raw = conf_raw(CONF)
        for dead in ("rtx.fallbackLightType", "rtx.fallbackLightRadius"):
            self.assertIsNone(re.search(r"^\s*" + dead + r"\s*=", raw, re.M),
                              "%s is still written in the shipped profile: it is the "
                              "camera-sphere shape, and leaving it live re-arms it the "
                              "moment anyone touches the mode" % dead)

    def test_fallback_term_is_exactly_zero(self):
        """The menu / first-seconds case: no lamps in the world, and the shipped profile
        still adds nothing."""
        self.assertEqual(self.sc.fallback_term(lights_present=False), 0.0,
                         "with no lamps in the world the shipped profile must add "
                         "nothing: that is the case the fallback light used to fill")

    def test_the_old_fallback_shape_out_lights_the_moon(self):
        """The measurement that justifies the key, kept as a live number rather than a
        quote: the shape this repo shipped until n24."""
        conf = dict(self.conf)
        conf["rtx.fallbackLightMode"] = 1
        conf["rtx.fallbackLightType"] = 1
        conf["rtx.fallbackLightRadius"] = 3.0
        conf["rtx.fallbackLightRadiance"] = (1.6, 1.8, 2.0)
        old = na.Scene(self.ini, conf, na.pier_lamps())
        got = old.fallback_term(lights_present=False)
        self.assertGreater(got / old.moon_term(), 8.0,
                           "the old sphere (radius 3 at 6 m, radiance 1.6,1.8,2.0) should "
                           "out-light the moon 8:1; it gives %.3f against %.4f = %.1fx"
                           % (got, old.moon_term(), got / old.moon_term()))

    def test_night_ambient_is_capped_against_the_moon(self):
        """Cause 2. rtx.skyBrightness multiplies a probe rasterized in the game's colour
        format (rtx.skyForceHDR = False), so its radiance is <= 1.0 and the key is an
        ambient CEILING. The plugin caps that ceiling at [sky] ambient_cap_mult x the moon
        light's published radiance; the ini is not allowed to ask for more than the cap."""
        sky = self.ini.get("sky", {})
        mult = float(sky.get("ambient_cap_mult", 0.0))
        self.assertGreater(mult, 0.0,
                           "[sky] ambient_cap_mult must be set: with 0 the rule is off and "
                           "the night ambient answers to nothing")
        self.assertLessEqual(mult, 0.5,
                             "the ceiling is a fraction of the only real source; above 0.5 "
                             "the ambient is back to being the majority of the light on "
                             "open sand (measured: 0.07 of 0.1215 = 57.6 % before n24)")
        moon_rad = self.sc.moon_published_radiance()
        asked = float(sky.get("night", 0.0)) + float(sky.get("moon", 0.0))
        self.assertGreater(asked, mult * moon_rad,
                           "the cap must actually bind on the shipped ini, or it is "
                           "documenting a rule that never runs: asked %.4f, cap %.4f"
                           % (asked, mult * moon_rad))
        self.assertLessEqual(self.sc.sky_term(), mult * moon_rad + 1e-9,
                             "the sim's published ambient must sit at or under the cap")

    def test_the_sky_is_a_minority_of_the_light_on_open_sand(self):
        """The number rhY was looking at: how much of the radiance on sand with no lamp
        near it comes from "sky" with nothing above it."""
        moon = self.sc.moon_term()
        sky = self.sc.sky_term()
        share = sky / (sky + moon)
        self.assertLessEqual(share, 0.35,
                             "%.0f %% of the light on open sand is the sky ambient; before "
                             "n24 it was 57.6 %%" % (100 * share))

    def test_uncapped_the_ambient_is_above_the_moon(self):
        """The pre-n24 state, asserted so the cap cannot be defended by a number that no
        longer describes the tree: rtx.conf's 1.0 against a moon publishing 0.0669."""
        self.assertGreater(float(self.conf.get("rtx.skyBrightness", 0.0)),
                           10.0 * self.sc.moon_published_radiance(),
                           "rtx.conf's startup rtx.skyBrightness is still the day value "
                           "(the plugin pushes the live one within 100 ms); if this moved, "
                           "the 15x number in docs/FINDINGS.md needs re-measuring")
        ini = dict(self.ini)
        ini["sky"] = dict(ini["sky"], ambient_cap_mult=0.0)
        unc = na.Scene(ini, self.conf, na.pier_lamps())
        self.assertGreaterEqual(unc.sky_term(), unc.moon_published_radiance(),
                               "with the cap off the ini's night ambient must again be at "
                               "or above the moon's own radiance - that is the bug")

    def test_moon_is_one_weak_cool_disc(self):
        moon = self.ini.get("moon", {})
        self.assertEqual(int(moon.get("enabled", 0)), 1)
        self.assertLessEqual(float(moon.get("radiance", 1.0)), 0.15,
                             "[moon] radiance is relative to [sun] radiance 6.0; above "
                             "0.15 the moon is a second sun")
        self.assertGreaterEqual(float(moon.get("radiance", 0.0)), 0.03,
                                "the moon must stay bright enough to be the night's only "
                                "natural source (below ~0.03 the sand goes to black and "
                                "the denoiser has nothing to reconstruct)")
        col = moon.get("color", (1.0, 1.0, 1.0))
        self.assertGreater(col[2], col[0], "moonlight is ~4100 K: blue channel above red")
        ang = float(moon.get("angular_diameter", 0.0))
        self.assertGreaterEqual(ang, 0.4)
        self.assertLessEqual(ang, 0.8)  # the real disc is 0.52-0.53 degrees

    def test_moon_is_weak_against_the_lamps(self):
        """The other ratio rhY looks at: moon-lit sand against sand under a lamp head.
        n23 moved this one by taking the lamps to 0.4 (the pool under a head 3.96 -> 1.58),
        so the threshold is 0.05 now, not n10's 0.02: the moon is 1:31 of a lamp pool here
        where the real full moon against a 25 lux street lamp is about 1:100."""
        (ux, uy), _ = na.metric_points(self.sc.lamps)
        under_lamp = self.sc.lamp_term(ux, uy, self.sc.sand_z)
        self.assertGreater(under_lamp, 0.0)
        ratio = self.sc.moon_term() / under_lamp
        self.assertLessEqual(ratio, 0.05,
                             "moon against the lamp pool under a head is %.4f" % ratio)


class TestChain(unittest.TestCase):
    """B. the chain must not flatten the ratio the sources produced, and must not add
    glow that has no source (V4)."""

    def setUp(self):
        self.conf = na.parse_conf(CONF)

    def test_global_tonemapping(self):
        """n16's, kept as a regression guard: the local pyramid divides a shadow/highlight
        ratio by 2^(shadows+highlights) and re-solves it around whatever is in frame (V3)."""
        self.assertEqual(int(self.conf.get("rtx.tonemappingMode", 1)), 0)
        self.assertAlmostEqual(float(self.conf.get("rtx.localtonemap.shadows", 2.0)), 1.0,
                               msg="n16 took the shadow lift to 1.0 (x2) from the 2.5 (x5.66) "
                                   "this repo carried and the runtime's default 2; it is inert "
                                   "while mode = 0, and this is the value a Local A/B starts "
                                   "from", places=3)

    def test_shadow_contrast_is_the_shipped_number(self):
        """The taste number, with the arithmetic in docs/FINDINGS.md: 0.0 -> away sand
        92/255 and a 36 % ratio, 0.4 -> 19/255 and 7.5 %, 0.6 -> 6/255 (nothing left for
        the denoiser)."""
        self.assertAlmostEqual(float(self.conf.get("rtx.tonemap.shadowContrast", 0.0)),
                               0.4,
                               msg="rtx.tonemap.shadowContrast is n24's taste number; the "
                                   "A/B ladder is in the profile comment", places=3)
        self.assertNotIn("rtx.tonemap.shadowContrastEnd", self.conf,
                         "shadowContrastEnd stays at the runtime's default 0: that is what "
                         "keeps the noon ground (above linear 1.0) untouched by the key")

    def test_no_transmission_shortcut_and_no_bloom_halo(self):
        """V4: the sea handing the sky and the lamp pools straight up into the pier
        underside, and bloom haloeing a tenth of the sea band at the 0.25 default."""
        self.assertIs(self.conf.get("rtx.enableTransmissionApproximationInIndirectRays",
                                    True), False,
                      "indirect rays must bend through the sea, not pass through it: that "
                      "is the pier underside glowing from below (and False is the "
                      "runtime's own default)")
        self.assertGreaterEqual(float(self.conf.get("rtx.bloom.luminanceThreshold", 0.25)),
                                1.0, "bloom only for what is genuinely over-bright: 10.31 % "
                                     "of the sea band of the 2026-11-12 frames is above the "
                                     "0.25 default and blooms at full weight, 0.23 % is at "
                                     "or above 1.0")

    def test_exposure_is_fixed_and_clamped_if_not(self):
        """n10 cleared auto exposure as a cause and pinned the keys anyway; n22 drives the
        EV anchor from the sun instead. Both still have to hold. The two metering keys are
        checked against the runtime's own DEFAULTS rather than against our profiles, because
        Median / no-centre-metering are what 1.6.1 already does and writing them again would
        be a second writer for a value nobody tunes."""
        self.assertIs(self.conf.get("rtx.autoExposure.enabled", True), False)
        self.assertAlmostEqual(float(self.conf.get("rtx.autoExposure.evMinValue", 0.0)),
                               0.0, msg="the shipped EV floor", places=3)
        defaults = {}
        with open(os.path.join(REPO, "config", "rtx-options-1.6.1.tsv"),
                  encoding="utf-8") as f:
            for line in f:
                if line.startswith("#"):
                    continue
                p = line.rstrip("\n").split("\t")
                if len(p) >= 3:
                    defaults[p[0]] = p[2]
        self.assertEqual(defaults.get("rtx.autoExposure.exposureAverageMode"), "1",
                         "the runtime's metering mode moved off Median ('more stable for "
                         "extreme pixel values') - re-read the auto-exposure clearing in "
                         "docs/FINDINGS.md before trusting it again")
        self.assertEqual(defaults.get("rtx.autoExposure.exposureCenterMeteringEnabled"),
                         "False", "a centre-weighted meter is a bright object in the "
                                  "middle of the frame setting the exposure of the whole "
                                  "frame; it is off by default and we write nothing")
        tm = na.parse_ini(INI).get("tonemap", {})
        self.assertEqual(int(tm.get("apply", 0)), 1,
                         "the night EV anchor is what the sim's exposure reads; with "
                         "apply=0 the sim's --ev has to be passed by hand")


class TestSimulatedRatio(unittest.TestCase):
    """C. the numbers. Same sim, same config, asserted so they cannot rot."""

    @classmethod
    def setUpClass(cls):
        cls.ini = na.parse_ini(INI)
        cls.conf = na.parse_conf(CONF)
        cls.lamps = na.pier_lamps()
        cls.sc = na.Scene(cls.ini, cls.conf, cls.lamps)
        (ux, uy), (ax, ay) = na.metric_points(cls.lamps)
        cls.under = cls.sc.sand_radiance(ux, uy)
        cls.away = cls.sc.sand_radiance(ax, ay)
        cls.frame = na.frame_luminances(cls.sc, (ux, uy), (ax, ay))
        cls.binlog, cls.curve = na.tone_curve_for(cls.conf, cls.frame)
        cls.ev = na.night_ev(cls.ini, cls.conf)
        cls.exposure = 2.0 ** cls.ev

        def shown(v):
            return na.display_luminance(cls.binlog, cls.curve, cls.conf, v, cls.exposure)

        cls.away8, cls.under8 = shown(cls.away), shown(cls.under)
        cls.sky8 = shown(cls.sc.sky_term())

    def test_metric_points_are_far_apart(self):
        nearest = min(((x - 2560.0) ** 2 + (y - -1500.0) ** 2)
                      for (_t, x, y, _z) in self.lamps)
        self.assertGreater(nearest ** 0.5, 100.0)

    def test_scene_ratio_under_10_percent(self):
        r = self.away / self.under
        self.assertLessEqual(r, 0.10,
                             "sand away from lamps is %.1f %% of sand under one in the "
                             "scene: the light budget itself is wrong" % (100 * r))

    def test_image_ratio_under_10_percent(self):
        r = self.away8 / self.under8 if self.under8 else 1.0
        self.assertLessEqual(r, 0.10,
                             "the shipped chain delivers %.1f %% - the 2026-11-12c video "
                             "measured 25-72 %%" % (100 * r))

    def test_chain_does_not_flatten_more_than_threefold(self):
        scene = self.away / self.under
        image = self.away8 / self.under8 if self.under8 else 1.0
        self.assertLessEqual(image, 3.0 * scene,
                             "the tone chain moved the ratio by %.1fx; the local pyramid "
                             "alone moves it 90x, and with shadowContrast at its default 0 "
                             "this chain moved it 8.8x" % (image / scene))

    def test_night_is_not_black(self):
        """The other failure direction, and the reason 0.4 is the taste number and 0.6 is
        not: a night with nothing in it gives the denoiser no signal."""
        self.assertGreater(self.away8 * 255.0, 10.0,
                           "moon-lit sand away from the lamps renders at %.1f/255"
                           % (self.away8 * 255.0))
        self.assertGreater(self.sky8 * 255.0, 8.0,
                           "the sky band renders at %.1f/255" % (self.sky8 * 255.0))
        self.assertLess(self.away8, self.under8)

    def test_the_old_profile_would_break_it(self):
        """Negative control D: the fallback light back, the cap off and shadowContrast at
        the runtime default - i.e. this repo's tree one commit earlier - must fail the
        ratio, so the assertions above are not passing by accident."""
        conf = dict(self.conf)
        conf["rtx.fallbackLightMode"] = 1
        conf["rtx.fallbackLightType"] = 1
        conf["rtx.fallbackLightRadius"] = 3.0
        conf["rtx.fallbackLightRadiance"] = (1.6, 1.8, 2.0)
        conf["rtx.tonemap.shadowContrast"] = 0.0
        ini = dict(self.ini)
        ini["sky"] = dict(ini["sky"], ambient_cap_mult=0.0)
        sc = na.Scene(ini, conf, self.lamps)
        (ux, uy), (ax, ay) = na.metric_points(self.lamps)
        under = sc.sand_radiance(ux, uy, lights_present=False)
        away = sc.sand_radiance(ax, ay, lights_present=False)
        frame = na.frame_luminances(sc, (ux, uy), (ax, ay))
        binlog, curve = na.tone_curve_for(conf, frame)
        a8 = na.display_luminance(binlog, curve, conf, away, self.exposure)
        u8 = na.display_luminance(binlog, curve, conf, under, self.exposure)
        self.assertGreater(away / under, 0.20,
                           "the fallback sphere alone should take the SCENE ratio over "
                           "20 %%; it gave %.1f %%" % (100 * away / under))
        self.assertGreater(a8 / u8 if u8 else 1.0, 0.25,
                           "and the image ratio back into the 25-72 %% the video showed; "
                           "it gave %.1f %%" % (100 * a8 / u8 if u8 else 0.0))

    def test_local_tonemapper_would_break_it(self):
        """Second negative control (n10's): the runtime default Local mode with the old
        2.5 shadow lift flattens the same scene."""
        conf = dict(self.conf)
        conf["rtx.tonemappingMode"] = 1
        conf["rtx.localtonemap.shadows"] = 2.5
        conf["rtx.localtonemap.highlights"] = 4.0
        squeezed = min(1.0, (self.away / self.under) * na.local_tonemap_bounds(conf))
        self.assertGreater(squeezed, 0.20,
                           "the local pyramid should flatten this scene's ratio to well "
                           "over 10 %%; it gave %.1f %%" % (100 * squeezed))


class TestProfilesAgree(unittest.TestCase):
    """An A/B profile must change when the light lands, not what the night is lit by."""

    KEYS = sorted(set(N24_KEYS) | {
        "rtx.tonemappingMode", "rtx.localtonemap.shadows", "rtx.skyBrightness",
        "rtx.emissiveIntensity", "rtx.autoExposure.enabled",
    })

    def test_every_shipped_profile_carries_them_identically(self):
        base = na.parse_conf(CONF)
        for path in (PERF, NOP):
            other = na.parse_conf(path)
            for k in self.KEYS:
                self.assertIn(k, other, "%s does not carry %s" % (os.path.basename(path), k))
                self.assertEqual(base[k], other[k],
                                 "%s sets %s to %r, config/rtx.conf sets %r"
                                 % (os.path.basename(path), k, other[k], base[k]))

    def test_the_n24_values_are_the_shipped_ones(self):
        base = na.parse_conf(CONF)
        for k, want in N24_KEYS.items():
            got = base.get(k)
            if isinstance(got, bool) or isinstance(got, str):
                self.assertEqual(str(got), want, "%s is %r, n24 ships %r" % (k, got, want))
            elif isinstance(got, tuple):
                wantv = tuple(float(x) for x in want.split(","))
                self.assertEqual(tuple(got), wantv,
                                 "%s is %r, n24 ships %r" % (k, got, want))
            else:
                self.assertAlmostEqual(float(got), float(want), places=6,
                                       msg="%s is %r, n24 ships %r" % (k, got, want))

    def test_the_fallback_shape_is_commented_out_everywhere(self):
        for path in (CONF, PERF, NOP):
            raw = conf_raw(path)
            for dead in ("rtx.fallbackLightType", "rtx.fallbackLightRadius"):
                self.assertIsNone(re.search(r"^\s*" + dead + r"\s*=", raw, re.M),
                                  "%s still writes %s" % (os.path.basename(path), dead))


if __name__ == "__main__":
    unittest.main()
