#!/usr/bin/env python3
"""n17 gate for tools/fog-budget.py - the arithmetic behind the fog-remap numbers.

The shipped atmosphere is six numbers in three profiles. What makes them defensible is not a
screenshot (rule 1 forbids rendering here) but the arithmetic in tools/fog-budget.py, which is a
transcription of the runtime's own code:

  normalized = (fogEnd - MaxDistanceMin) / (MaxDistanceMax - MaxDistanceMin)   # NOT clamped
  d_meas     = normalized * (TransMax - TransMin) + TransMin
  sigma      = -ln(srgb_to_linear(transmittanceColor)) / d_meas ; T(x) = exp(-sigma x)
  visibility = 3.912 / sigma

so these tests pin the transcription (sRGB, the missing clamp, Koschmieder) and the three claims
the config comments make: the runtime default is no atmosphere at all, the shipped bounds are
weather-scale and never eat the near band, and enableFogColorRemap is a density bomb.
"""
import importlib.util
import math
import os
import subprocess
import sys
import unittest

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOL = os.path.join(REPO, "tools", "fog-budget.py")


def load():
    spec = importlib.util.spec_from_file_location("fog_budget", TOOL)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


fb = load()


class TestMath(unittest.TestCase):
    def test_srgb_to_linear(self):
        self.assertAlmostEqual(fb.srgb_to_linear(1.0), 1.0, places=6)
        self.assertAlmostEqual(fb.srgb_to_linear(0.0), 0.0, places=9)
        self.assertAlmostEqual(fb.srgb_to_linear(0.5), 0.21404, places=5)
        self.assertAlmostEqual(fb.srgb_to_linear(0.04045), 0.04045 / 12.92, places=9)
        # the value the whole block is built on
        self.assertAlmostEqual(-math.log(fb.srgb_to_linear(0.999)), 0.002276, places=6)

    def test_the_runtime_default_is_no_atmosphere(self):
        """transmittanceColor 0.999 measured at 200 m: 344 km of air. This is why n15 measured
        far/near local contrast 1.22-3.67 - there was nothing between the bands."""
        sig = fb.sigma_per_m((0.999, 0.999, 0.999), 200.0)[0]
        self.assertAlmostEqual(sig, 1.138e-5, delta=1e-8)
        self.assertGreater(fb.visibility_km(sig), 300.0)
        self.assertGreater(math.exp(-sig * 4000.0), 0.95)

    def test_the_remap_does_not_clamp(self):
        """rtx_global_volumetrics.cpp:517 divides and never clamps, so the runtime's own default
        bounds (1 / 40 m) against SA's 4000 m fog end extrapolate to a medium THINNER than the
        default they replace. That is the trap this task had to avoid."""
        n, d = fb.remap_d_meas(4000.0, 1.0, 40.0, 20.0, 100.0)
        self.assertAlmostEqual(n, (4000.0 - 1.0) / 39.0, places=3)
        self.assertGreater(d, 8000.0)
        self.assertGreater(fb.visibility_km(fb.sigma_per_m((0.999,) * 3, d)[0]), 700.0)

    def test_the_shipped_bounds_are_weather_scale_and_never_touch_the_near_band(self):
        """400..4000 m in, 2..6 m out: 10.3 km at the game's clear-weather fog end, 3.4 km at the
        densest, and >= 94 % of the light kept inside 50 m at every one of them."""
        rows = {}
        for fog_end in (400.0, 800.0, 1500.0, 4000.0):
            n, d = fb.remap_d_meas(fog_end, 400.0, 4000.0, 2.0, 6.0)
            self.assertGreaterEqual(n, 0.0)
            self.assertLessEqual(n, 1.0)
            sig = fb.sigma_per_m((0.999, 0.999, 0.999), d)[0]
            rows[fog_end] = (sig, fb.visibility_km(sig), math.exp(-sig * 50.0),
                             math.exp(-sig * 2000.0))
        self.assertAlmostEqual(rows[4000.0][1], 10.3, delta=0.1)
        self.assertAlmostEqual(rows[400.0][1], 3.4, delta=0.1)
        for fog_end, (sig, vis, near, far) in rows.items():
            self.assertGreater(near, 0.94, "near band at fog end %g" % fog_end)
            self.assertLess(far, 0.50, "far band at fog end %g" % fog_end)

    def test_fog_colour_remap_is_a_density_bomb(self):
        """Why rtx.volumetrics.enableFogColorRemap stays off: sigma is -ln(colour)/d_meas, and the
        game's fog colour is not near-white at every hour. A dark fog colour at the shipped d_meas
        makes the street within 50 m vanish."""
        for colour in (0.6, 0.3, 0.1):
            sig = fb.sigma_per_m((colour,) * 3, 6.0)[0]
            self.assertGreater(sig, 0.1)
            self.assertLess(math.exp(-sig * 50.0), 1e-3)

    def test_profile_parsing_fills_the_161_defaults(self):
        p = fb.parse_profile(os.path.join(REPO, "config", "rtx.conf"))
        self.assertIs(p["rtx.volumetrics.enableFogRemap"], True)
        self.assertEqual(p["rtx.volumetrics.fogRemapMaxDistanceMaxMeters"], 4000.0)
        # not written anywhere: the 1.6.1 default, from the same table the audit uses
        self.assertEqual(p["rtx.volumetrics.transmittanceMeasurementDistanceMeters"], 200.0)
        self.assertEqual(p["rtx.volumetrics.transmittanceColor"], (0.999, 0.999, 0.999))
        self.assertEqual(p["rtx.sceneScale"], 0.01)


class TestCli(unittest.TestCase):
    def test_reports_the_shipped_profile(self):
        r = subprocess.run([sys.executable, os.path.join("tools", "fog-budget.py"),
                            "config/rtx.conf"], cwd=REPO, capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("enableFogRemap=True", r.stdout)
        self.assertIn("10.3 km", r.stdout)
        self.assertNotIn("outside the input bounds", r.stdout)

    def test_flags_a_fog_end_the_bounds_do_not_cover(self):
        r = subprocess.run([sys.executable, os.path.join("tools", "fog-budget.py"),
                            "config/rtx.conf", "--fog-end", "9000"],
                           cwd=REPO, capture_output=True, text=True)
        self.assertEqual(r.returncode, 0)
        self.assertIn("outside the input bounds", r.stdout)

    def test_a_profile_without_the_block_reports_the_default_medium(self):
        """What every profile before n17 was: no fog keys, so the defaults, so 343.8 km of air."""
        tmp = os.path.join(REPO, "build", "tmp", "n17", "no-fog-block.conf")
        os.makedirs(os.path.dirname(tmp), exist_ok=True)
        with open(tmp, "w", encoding="utf-8") as f:
            f.write("rtx.sceneScale = 0.01\nrtx.volumetrics.enable = True\n")
        r = subprocess.run([sys.executable, os.path.join("tools", "fog-budget.py"), tmp],
                           cwd=REPO, capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("remap OFF", r.stdout)
        self.assertIn("343.8 km", r.stdout)


if __name__ == "__main__":
    unittest.main(verbosity=2)
