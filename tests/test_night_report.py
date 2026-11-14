#!/usr/bin/env python3
"""n10: tools/night-report.py measures the thing rhY complained about, so it has to measure
it on images whose answer is known before anyone points it at a game.

    python3 -m pytest -q tests/test_night_report.py

Three synthetic frames, each built so the answer is fixed by construction:

  * a night frame - dark ground with one bright pool: away/under is small, it passes;
  * a flat frame - the ground is one value everywhere: away/under is 1, it fails, and that
    is exactly the "not dark anywhere except under lamps" shape the tool exists to catch;
  * the inversion frame - the sky band brighter than the ground band, which is the other
    half of rhY's sentence ("the sky is black, the ground is bright").

The percentiles are read off the same 256-bin Rec.709 histogram tools/tour-report.py uses,
so a number here is comparable with a number in a tour report.
"""
import importlib.util
import os
import shutil
import unittest

from PIL import Image

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
SPEC = importlib.util.spec_from_file_location("night_report",
                                              os.path.join(REPO, "tools", "night-report.py"))
nr = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(nr)

TMP = os.path.join(REPO, "build", "tmp", "night-report-selftest")   # rule 2: never /tmp


def frame(path, sky=20, ground=8, pool=0, pool_luma=200, w=200, h=100):
    """Top 30 % sky, bottom 38 % ground at `ground` luma, with a `pool`-fraction-wide bright
    block inside the ground band (0 = no lamp in frame)."""
    im = Image.new("RGB", (w, h), (sky, sky, sky))
    y0 = int(round(0.62 * h))
    for y in range(y0, h):
        for x in range(w):
            im.putpixel((x, y), (ground, ground, ground))
    if pool:
        pw = max(1, int(pool * w))
        for y in range(y0, h):
            for x in range(w - pw, w):
                im.putpixel((x, y), (pool_luma, pool_luma, pool_luma))
    im.save(path)
    return path


class TestNightReport(unittest.TestCase):
    def setUp(self):
        shutil.rmtree(TMP, ignore_errors=True)
        os.makedirs(TMP)
        self.tmp = TMP

    def path(self, name):
        return os.path.join(self.tmp, name)

    def test_night_frame_passes(self):
        p = frame(self.path("night.png"), sky=20, ground=8, pool=0.08)
        m = nr.measure(p)
        self.assertAlmostEqual(m["away"], 8.0, places=0)
        self.assertAlmostEqual(m["under"], 200.0, places=0)
        self.assertLessEqual(m["ratio"], 0.10)
        self.assertFalse(m["inverted"])

    def test_flat_frame_fails(self):
        """The failure shape: one value across the whole ground band. No pool, no shadow -
        the 2026-11-12c video sat here (25-72 %)."""
        p = frame(self.path("flat.png"), sky=20, ground=100, pool=0)
        m = nr.measure(p)
        self.assertGreater(m["ratio"], 0.90)

    def test_inversion_is_flagged(self):
        """rhY: "the sky is black, the ground is bright". The flag is the sky band's mean
        sitting under the ground band's median - which at night, with a moon in the sky, is
        the wrong way round."""
        p = frame(self.path("inv.png"), sky=12, ground=100, pool=0.05)
        m = nr.measure(p)
        self.assertTrue(m["inverted"], "sky mean %.1f under ground median %.1f is the "
                                       "inversion and must be flagged"
                        % (m["sky_mean"], m["median"]))
        ok = frame(self.path("ok.png"), sky=60, ground=8, pool=0.05)
        self.assertFalse(nr.measure(ok)["inverted"])

    def test_box_measures_one_region(self):
        p = frame(self.path("box.png"), sky=20, ground=8, pool=0.5, pool_luma=240)
        whole = nr.measure(p)
        self.assertAlmostEqual(whole["away"], 8.0, places=0)     # the unlit half is in frame
        pool_only = nr.measure(p, box=(100, 70, 100, 30))        # only the lit half
        self.assertAlmostEqual(pool_only["away"], 240.0, places=0)
        self.assertAlmostEqual(pool_only["under"], 240.0, places=0)

    def test_percentile_helper(self):
        h = [0] * 256
        h[10] = 90
        h[200] = 10
        self.assertEqual(nr.percentile(h, 0.10), 10.0)
        self.assertEqual(nr.percentile(h, 0.95), 200.0)
        self.assertEqual(nr.percentile([0] * 256, 0.5), 0.0)


if __name__ == "__main__":
    unittest.main()
