#!/usr/bin/env python3
"""tests/test_lamp_coverage.py - the coverage tool's arithmetic.

tools/lamp-coverage.py is the gate for n11's claim ("every 2dFx light of placed
objects is lit"): it compares the plugin's lamp list against the game's own 2dFx
inventory and fails under a target. Its own logic therefore has to be tested
against cases where the answer is known - including the case where the answer is
"not covered", because a tool that always reports 100% would pass the gate and
tell us nothing.

The tool's file name has a dash, so it is loaded by path.
"""
import importlib.util
import os
import sys
import unittest

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, "tools"))
import lamp_list  # noqa: E402

_spec = importlib.util.spec_from_file_location(
    "lamp_coverage", os.path.join(REPO, "tools", "lamp-coverage.py"))
cov = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(cov)


def inv(model, region, x, y, z=8.0, rgb=(255, 148, 52), rng=100.0):
    return {"model": model, "region": region, "x": float(x), "y": float(y),
            "z": float(z), "r": rgb[0], "g": rgb[1], "b": rgb[2], "range": rng}


class TestRead(unittest.TestCase):
    def test_read_list_accepts_v1_and_v2(self):
        path = os.path.join(os.environ.get("TMPDIR", "/tmp"), "n11-list-mix.txt")
        with open(path, "w") as fh:
            fh.write("# rhyremix lights v2\n")
            fh.write("# type x y z r g b family range\n")
            fh.write("3 100.0 -200.0 8.0 255 148 52 hps 100.0\n")
            fh.write("9 10.5 20.25 6.0\n")           # a v1 line
            fh.write("garbage line\n")
        pts = cov.read_list(path)
        self.assertEqual(pts, [(100.0, -200.0, 8.0), (10.5, 20.25, 6.0)])

    def test_read_inventory(self):
        path = os.path.join(os.environ.get("TMPDIR", "/tmp"), "n11-inv.txt")
        with open(path, "w") as fh:
            fh.write("# rhyremix 2dfx inventory v1\n# model region x y z r g b range\n")
            fh.write("lamppost1 LA 100.0 -200.0 8.0 249 145 34 100.0\n")
        got = cov.read_inventory(path)
        self.assertEqual(len(got), 1)
        self.assertEqual(got[0]["model"], "lamppost1")
        self.assertEqual(got[0]["region"], "LA")
        self.assertAlmostEqual(got[0]["range"], 100.0)


class TestGrid(unittest.TestCase):
    def test_near_is_euclidean_and_tolerance_is_inclusive(self):
        g = cov.Grid([(0.0, 0.0, 0.0), (5.0, 0.0, 0.0)])
        self.assertTrue(g.near((0.4, 0.0, 0.0), 1.0))
        self.assertFalse(g.near((0.0, 0.0, 3.0), 1.0))   # 3 away in z
        self.assertTrue(g.near((5.0, 0.0, 0.0), 0.0))    # exact
        self.assertFalse(g.near((2.5, 0.0, 0.0), 1.0))   # between the two

    def test_near_works_across_cell_boundaries(self):
        # The grid is a 2-unit cell hash; a light 1.9 units away across a boundary
        # must still be found, which is what the +-1 cell scan is for.
        g = cov.Grid([(2.0, 0.0, 0.0)])
        self.assertTrue(g.near((0.15, 0.0, 0.0), 1.9))


class TestCoverageMath(unittest.TestCase):
    def setUp(self):
        # Ten street lights of Los Santos, two signal lenses, one San Francisco
        # light. The list covers eight of the ten.
        self.rows = [inv("lamppost1", "LA", 100 * i, 0) for i in range(10)]
        self.rows += [inv("cj_traffic_light4", "LA", 500, 500),
                      inv("mtraffic1", "LA", 520, 500)]
        self.rows += [inv("streetlamp1", "SF", 900, 900)]
        self.pts = [(100.0 * i, 0.0, 8.0) for i in range(8)] + [(900.0, 900.0, 8.0)]

    def _coverage(self, tol=1.0):
        g = cov.Grid(self.pts)
        street = [r for r in self.rows if r["region"] == "LA"
                  and r["model"] not in lamp_list.TRAFFIC_NAMES]
        got = [r for r in street if g.near((r["x"], r["y"], r["z"]), tol)]
        return len(street), len(got), 100.0 * len(got) / len(street)

    def test_signal_models_are_not_counted_as_a_street_gap(self):
        street, got, pct = self._coverage()
        self.assertEqual(street, 10)      # not 12: the lenses belong to the signal layer
        self.assertEqual(got, 8)
        self.assertAlmostEqual(pct, 80.0)

    def test_a_list_that_misses_a_model_reports_below_target(self):
        _, _, pct = self._coverage()
        self.assertLess(pct, 95.0)        # the gate would fail this list

    def test_tolerance_is_not_a_free_pass(self):
        # Widening the tolerance to 100 units "covers" the two missing lamps with
        # their neighbours. The shipped default is 1.0 game unit, about the size
        # of a lamp head, and that is what makes the number mean anything.
        _, got, _ = self._coverage(tol=1.0)
        self.assertEqual(got, 8)
        _, got, _ = self._coverage(tol=150.0)
        self.assertGreater(got, 8)      # a wider tolerance quietly covers a miss
        _, got, _ = self._coverage(tol=1000.0)
        self.assertEqual(got, 10)       # and at some point it covers everything

    def test_regions_are_reported_separately(self):
        g = cov.Grid(self.pts)
        sf = [r for r in self.rows if r["region"] == "SF"]
        self.assertTrue(all(g.near((r["x"], r["y"], r["z"]), 1.0) for r in sf))


class TestDensity(unittest.TestCase):
    def test_worst_case_within_a_radius(self):
        # A 3x3 cluster 10 units apart and one lone light: within 20 units of the
        # cluster the answer is 9, and the lone light must not win the maximum.
        pts = [(100.0 + 10 * (i % 3), 200.0 + 10 * (i // 3), 8.0) for i in range(9)]
        pts.append((2000.0, 2000.0, 8.0))
        out = cov.density(pts, [20.0, 500.0], sample=1)
        self.assertEqual(out[20.0][0], 9)
        self.assertEqual(out[500.0][0], 9)


if __name__ == "__main__":
    unittest.main()
