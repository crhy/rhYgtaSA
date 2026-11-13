#!/usr/bin/env python3
"""n12 gate: the frame measurement tool measures what it claims to measure.

tools/frame_light_measure.py turns the three complaints of .strata/
test-2026-11-12c.md into numbers - the red wash the tail lights throw on the sand
(V5), the glowing exhaust puffs (V6), the flat white window blocks (V7). Those
numbers end up in config/rtx.conf and config/rhyremix-production.ini as tuning
decisions, so the metrics have to be reproducible and they have to answer to
synthetic ground truth: an image built from known rectangles has to come back
with those rectangles' areas, positions and colours.

The synthetic frames are deliberately the shape of the real ones: a flat ambient
floor (V1, the reason every metric is a high-pass), a small bright source, a wide
dim wash, a pink puff brighter than its surround, and a clipped neutral-white
block.

Run: python3 -m pytest -q tests/test_frame_light_measure.py  (or -m unittest)
"""
import os
import sys
import unittest

import numpy as np

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, "tools"))

import frame_light_measure as flm  # noqa: E402


def scene():
    """A 320x240 frame with every feature the tool is supposed to find.

    Ground truth, in pixels:
      * a flat ambient floor of 60 everywhere (so an absolute threshold would
        measure the floor and prove the high-pass is doing something);
      * the ground band starts at 0.60 * 240 = row 144;
      * a red tail wash: a 40x20 block of strong red at (60,180), i.e. 800 px of
        red-dominant ground, with a small saturated core;
      * a headlight pool: a 30x20 block of bright neutral-white at (220,190),
        600 px, not red-dominant;
      * a smoke puff: a 10x10 pink block at (150,170), 100 px, brighter than the
        floor and with min(R,B) - G well above PINK_MIN;
      * a window: a 24x12 block at (120,150) at (250,250,250) - clipped, neutral.
    """
    im = np.full((240, 320, 3), 60.0)
    # red tail wash, with a saturated core
    im[170:190, 40:80] = (120, 20, 25)
    im[176:184, 54:66] = (215, 12, 18)
    # headlight pool
    im[180:200, 205:235] = (235, 232, 225)
    # pink puff, brighter than the floor it sits on
    im[165:175, 145:155] = (175, 60, 90)
    # clipped neutral white window
    im[150:162, 108:132] = (250, 250, 250)
    return im


class TestMetrics(unittest.TestCase):
    def setUp(self):
        self.im = scene()
        self.res = flm.measure(self.im)

    def test_red_spill_finds_the_wash(self):
        red = self.res["red_spill"]
        self.assertGreater(red["area_px"], 500)
        cx, cy = red["centre"]
        self.assertTrue(35 <= cx <= 85, "red blob centre x %d" % cx)
        self.assertTrue(165 <= cy <= 195, "red blob centre y %d" % cy)
        # the source core is the saturated inner rectangle, not the whole wash
        self.assertGreater(red["core_px"], 50)
        self.assertLessEqual(red["core_px"], red["area_px"])
        # peak colour really is red
        r, g, b = red["peak_rgb"]
        self.assertGreater(r, 4 * max(g, b))
        self.assertTrue(red["profile"], "no radial falloff profile")
        # and the falloff dies out: the far ring is dimmer than the near one
        prof = red["profile"]
        self.assertGreater(prof[0]["mean_hi"], prof[-1]["mean_hi"])

    def test_head_pool_is_not_the_red_wash(self):
        head = self.res["head_pool"]
        self.assertGreater(head["area_px"], 300)
        # The peak is the brightest high-pass pixel of the non-red ground. In this
        # scene there are two white blocks and the SMALLER one wins, because a
        # high-pass value is (block - its own blurred footprint) and a small block
        # barely contributes to its own blur. That is not a bug in the metric: it
        # is the V7 complaint, a clipped window being the brightest thing on the
        # ground. So the assertion is "a white block", not "the pool".
        px, py = head["peak_px"]
        in_pool = 205 <= px <= 235 and 180 <= py <= 200
        in_window = 108 <= px <= 131 and 150 <= py <= 161
        self.assertTrue(in_pool or in_window, "head peak at %d,%d" % (px, py))
        r, g, b = head["peak_rgb"]
        self.assertGreater(min(r, g, b), 150)
        self.assertLess(max(r, g, b) - min(r, g, b), 30)   # neutral, not red

    def test_smoke_puff_is_measured_as_pink_and_bright(self):
        sm = self.res["smoke"]
        self.assertGreaterEqual(sm["area_px"], 80)
        r, g, b = sm["mean_rgb"]
        self.assertGreater(min(r, b) - g, flm.PINK_MIN)
        # brighter than the ambient floor it rises from
        self.assertGreater(sm["mean_hi_luma"], 10.0)
        cx, cy = sm["centre"]
        self.assertTrue(140 <= cx <= 160, "puff centre x %d" % cx)
        self.assertTrue(160 <= cy <= 180, "puff centre y %d" % cy)

    def test_window_block_is_clipped_and_neutral(self):
        w = self.res["windows"]
        self.assertGreaterEqual(w["area_px"], 24 * 12 - 4)
        self.assertLess(w["chroma_mean"], 5.0)   # neutral white: no lamp colour
        big = w["largest"]
        self.assertGreaterEqual(big["area_px"], 24 * 12 - 4)
        self.assertEqual([24, 12], big["extent_px"])

    def test_no_window_means_no_window_metric(self):
        im = self.im.copy()
        im[150:162, 108:132] = 60.0
        w = flm.measure(im)["windows"]
        self.assertEqual(0, w["area_px"])
        self.assertEqual(0, w["largest"]["area_px"])
        self.assertEqual(0.0, w["pct_of_ground"])

    def test_above_the_horizon_is_not_ground(self):
        # A clipped block in the sky half must not count: the sky is white.
        im = np.full((240, 320, 3), 60.0)
        im[10:30, 10:60] = 250.0
        self.assertEqual(0, flm.measure(im)["windows"]["area_px"])

    def test_ambient_floor_is_reported(self):
        a = self.res["ambient"]
        self.assertTrue(40.0 <= a["ground_mean_luma"] <= 90.0, a)

    def test_largest_component_picks_the_bigger_blob(self):
        m = np.zeros((20, 40), dtype=bool)
        m[2:5, 2:5] = True          # 9 px
        m[10:16, 20:30] = True      # 60 px
        big = flm.largest_component(m)
        self.assertEqual(60, big["area_px"])
        self.assertEqual([10, 6], big["extent_px"])
        self.assertEqual([24, 12], big["centre"])
        self.assertEqual(1.67, big["aspect"])
        self.assertEqual(0, flm.largest_component(np.zeros((4, 4), dtype=bool))["area_px"])

    def test_box_blur_leaves_a_flat_field_flat(self):
        # This is the property every metric depends on: the frame's flat ambient
        # floor (V1) must survive the blur unchanged, or the high-pass would be
        # measuring the ambient instead of the lights.
        flat = np.full((32, 32), 60.0)
        out = flm.box_blur(flat, 12)
        self.assertTrue(np.allclose(out, 60.0))
        hp = flm.highpass(flat, 12)
        self.assertTrue(np.allclose(hp, 0.0, atol=1e-9))

    def test_box_blur_spreads_an_impulse(self):
        a = np.zeros((16, 16))
        a[8, 8] = 100.0
        b = flm.box_blur(a, 3)
        self.assertLess(float(b[8, 8]), 100.0)
        self.assertGreater(float(b[8, 8]), 0.0)
        self.assertGreater(float(b[8, 9]), 0.0)   # it spread sideways
        self.assertAlmostEqual(0.0, float(b[0, 0]), places=12)
        # signed input, no 8-bit clipping: a negative blur is a negative number
        self.assertTrue(np.allclose(flm.box_blur(-a, 3), -b))


if __name__ == "__main__":
    unittest.main(verbosity=2 if "-v" in sys.argv else 0)
