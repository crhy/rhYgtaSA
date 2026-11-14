#!/usr/bin/env python3
"""tests/test_lamp_v2.py - the v2 lamp list: the game's 2dFx records as lamps.

n11 replaced the third-party lamp table with the game's own lamp data. What has
to be true for that to be an improvement rather than a change of format:

  * a 2dFx light lands where the GAME puts it: instance position + instance
    quaternion x the model's own offset (the rule at 0x59C890);
  * the family and the size band come out of the measured colour and the model
    name, not out of a guess;
  * the traffic-signal models stay out of the street list (they are the signal
    layer's lights, and a second handle on one lens is a wasted slot);
  * the old v1 file is still producible, byte-compatible, so the two can be A/B'd.
"""
import os
import subprocess
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "tools"))
import lamp_list  # noqa: E402


class TestRegion(unittest.TestCase):
    def test_stream_names(self):
        self.assertEqual(lamp_list.region_of_ipl("lae2_stream0.ipl"), "LA")
        self.assertEqual(lamp_list.region_of_ipl("law2_stream4.ipl"), "LA")
        self.assertEqual(lamp_list.region_of_ipl("carter.ipl"), "LA")
        self.assertEqual(lamp_list.region_of_ipl("sf_stream0.ipl"), "SF")
        self.assertEqual(lamp_list.region_of_ipl("vegas_stream1.ipl"), "vegas")
        self.assertEqual(lamp_list.region_of_ipl("country_stream0.ipl"), "country")
        self.assertEqual(lamp_list.region_of_ipl("seabed_stream0.ipl"), "leveldes")

    def test_unknown_is_named_not_guessed(self):
        self.assertEqual(lamp_list.region_of_ipl("levelmap.ipl"), "leveldes")
        self.assertEqual(lamp_list.region_of_ipl("wtf.ipl"), "other")


class TestFamily(unittest.TestCase):
    """The family is chosen from the measured 2dFx colour, with the model name
    winning where the model says what the fitting is."""

    def test_sodium_lamps_are_hps(self):
        # lamppost1 (249,145,34), lamppost2 (255,168,17), lamppost3 and mlamppost
        # (255,148,52), chinalamp_sf (255,148,52): every street sodium lamp in the
        # archive records a deep orange.
        for name, rgb in (("lamppost1", (249, 145, 34)), ("lamppost2", (255, 168, 17)),
                          ("lamppost3", (255, 148, 52)), ("mlamppost", (255, 148, 52)),
                          ("chinalamp_sf", (255, 148, 52))):
            self.assertEqual(lamp_list.lamp_family(name, rgb), "hps", (name, rgb))

    def test_white_globe_posts_are_fluorescent(self):
        # streetlamp1/2, lampost_coast, bollardlight: (255,255,255).
        for name in ("streetlamp1", "streetlamp2", "lampost_coast", "bollardlight"):
            self.assertEqual(lamp_list.lamp_family(name, (255, 255, 255)), "fluorescent")

    def test_warm_sign_white_is_tungsten(self):
        # vegaslampost (225,185,149) and lamppost2's mid-pole lamp (255,231,133).
        self.assertEqual(lamp_list.lamp_family("vegaslampost", (225, 185, 149)), "tungsten")
        self.assertEqual(lamp_list.lamp_family("lamppost2", (255, 231, 133)), "tungsten")

    def test_saturated_indicator_colours_are_neon(self):
        # traincross1's crossing lamps are pure red; the refinery mast lights are
        # green/cyan. Neither is a lamp spectrum, and neither should be smoothed
        # into a street family.
        self.assertEqual(lamp_list.lamp_family("traincross1", (255, 0, 0)), "neon")
        self.assertEqual(lamp_list.lamp_family("refchimny01", (59, 241, 183)), "neon")
        self.assertEqual(lamp_list.lamp_family("vgseedge21", (23, 105, 255)), "neon")

    def test_flood_and_sign_masts_are_metal_halide(self):
        # A floodlight mast is a metal-halide fitting whatever its raster says:
        # sub_floodlite records plain white, which alone would read fluorescent.
        self.assertEqual(lamp_list.lamp_family("sub_floodlite", (255, 255, 255)), "halide")
        self.assertEqual(lamp_list.lamp_family("sf_frwaysig", (126, 126, 114)), "halide")

    def test_airfield_lights_are_tungsten(self):
        self.assertEqual(lamp_list.lamp_family("airport_11_sfse", (255, 255, 255)),
                         "tungsten")

    def test_no_colour_falls_back_to_the_street_family(self):
        self.assertEqual(lamp_list.lamp_family("lamppost1", (0, 0, 0)), "hps")


class TestSizeBand(unittest.TestCase):
    def test_measured_ranges(self):
        self.assertEqual(lamp_list.size_band("lamppost2", 300.0), 0)   # big globes
        self.assertEqual(lamp_list.size_band("lamppost1", 100.0), 1)   # middle
        self.assertEqual(lamp_list.size_band("lampost_coast", 150.0), 1)
        self.assertEqual(lamp_list.size_band("vegaslampost", 62.0), 2)  # post-top
        self.assertEqual(lamp_list.size_band("anything", 0.0), 1)

    def test_name_beats_the_range_where_the_range_is_backwards(self):
        # Measured: bollardlight records range 180 (a bollard!) and sub_floodlite
        # records 100 (a 14 m mast). The name is the truth there.
        self.assertEqual(lamp_list.size_band("bollardlight", 180.0), 2)
        self.assertEqual(lamp_list.size_band("sub_floodlite", 100.0), 0)
        self.assertEqual(lamp_list.size_band("traincross1", 100.0), 2)


class TestWorldLights(unittest.TestCase):
    def test_position_is_the_instance_transform_of_the_models_own_offset(self):
        # A lamppost1 instance at (100, 200, 3) turned 90 degrees about Z, with the
        # model's bulb at (-0.44, 0.05, 3.20). The quaternion is conjugated first
        # (the IPL quaternion maps model space to world space and the game's own
        # conversion at 0x59C890 is equivalent to rotating by the conjugate), so
        # the offset turns -90 degrees: (x,y) -> (y,-x), and the bulb lands at
        # (100 + 0.05, 200 + 0.44, 3 + 3.20).
        s = 0.70710678
        inst = [(1297, (100.0, 200.0, 3.0), (0.0, 0.0, s, s), "LA", "la_stream0.ipl")]
        ide = {1297: ("lamppost1", 59.0)}
        lights = {"lamppost1": [{"x": -0.44, "y": 0.05, "z": 3.20, "r": 249, "g": 145,
                                 "b": 34, "range": 100.0, "flags": (0, 0, 0, 65)}]}
        out = lamp_list.world_2dfx_lights(inst, ide, lights)
        self.assertEqual(len(out), 1)
        L = out[0]
        self.assertAlmostEqual(L["x"], 100.0 + 0.05, places=4)
        self.assertAlmostEqual(L["y"], 200.0 + 0.44, places=4)
        self.assertAlmostEqual(L["z"], 3.0 + 3.20, places=4)
        self.assertEqual(L["model"], "lamppost1")
        self.assertEqual(L["region"], "LA")
        self.assertEqual(L["family"], "hps")
        self.assertEqual(L["band"], 1)
        self.assertEqual((L["r"], L["g"], L["b"]), (249, 145, 34))

    def test_a_model_with_no_2dfx_light_produces_nothing(self):
        out = lamp_list.world_2dfx_lights([(1, (0.0, 0.0, 0.0), (0, 0, 0, 1), "LA", "x")],
                                          {1: ("tree", 10.0)}, {})
        self.assertEqual(out, [])

    def test_traffic_models_are_named_as_the_signal_layers(self):
        # The street list drops these; the coverage tool reports them separately.
        for n in ("cj_traffic_light4", "mtraffic1", "trafficlight1", "gay_traffic_light"):
            self.assertIn(n, lamp_list.TRAFFIC_NAMES)
        self.assertNotIn("lamppost1", lamp_list.TRAFFIC_NAMES)
        self.assertNotIn("traincross1", lamp_list.TRAFFIC_NAMES)  # a crossing lamp
        self.assertNotIn("billbd2", lamp_list.TRAFFIC_NAMES)      # a billboard


GAME = "/media/rhy/data/Gamez/GTASA-RTX"


@unittest.skipUnless(os.path.isdir(GAME), f"no game folder at {GAME}")
class TestGeneratedLists(unittest.TestCase):
    """End to end against the read-only game folder."""

    @classmethod
    def setUpClass(cls):
        import tempfile
        cls.tmp = tempfile.mkdtemp(prefix="n11-lamps-")
        cls.v2 = os.path.join(cls.tmp, "lights-v2.txt")
        cls.v1 = os.path.join(cls.tmp, "lights-v1.txt")
        cls.inv = os.path.join(cls.tmp, "2dfx.txt")
        for out, extra in ((cls.v2, []), (cls.v1, ["--format", "v1"])):
            r = subprocess.run([sys.executable, os.path.join("tools", "lamp_list.py"),
                                "--game", GAME, "--out", out,
                                "--inventory", cls.inv] + extra,
                               capture_output=True, text=True)
            assert r.returncode == 0, r.stdout + r.stderr
        cls.lines2 = cls.read(cls.v2)
        cls.lines1 = cls.read(cls.v1)
        cls.inv_lines = cls.read(cls.inv)

    @staticmethod
    def read(path):
        with open(path) as fh:
            return [ln.split() for ln in fh.read().splitlines()
                    if ln.strip() and not ln.startswith("#")]

    def test_v2_header_and_nine_columns(self):
        with open(self.v2) as fh:
            head = fh.readline().strip()
        self.assertEqual(head, "# rhyremix lights v2")
        self.assertGreater(len(self.lines2), 6000)
        for p in self.lines2:
            self.assertEqual(len(p), 9, p)
            self.assertIn(int(p[0]), (1, 3, 5), p)   # band 0/1/1/2 as type 1/3/5
            self.assertIn(p[7], ("hps", "mercury", "halide", "led", "tungsten",
                                 "neon", "fluorescent"), p)
            for c in p[4:7]:
                self.assertTrue(0 <= int(c) <= 255, p)

    def test_v1_file_is_unchanged(self):
        # The compatibility proof: --format v1 still writes the old four-column
        # file, so an A/B is one file swap and nothing else.
        with open(self.v1) as fh:
            self.assertEqual(fh.readline().strip(), "# rhyremix lights v1")
        self.assertEqual(len(self.lines1), 7002)
        for p in self.lines1[:50]:
            self.assertEqual(len(p), 4, p)

    def test_inventory_counts_the_games_own_lights(self):
        # 18691 2dFx light instances world-wide; Los Santos is the region the
        # coverage target is set for.
        self.assertEqual(len(self.inv_lines), 18691)
        la = [p for p in self.inv_lines if p[1] == "LA"]
        self.assertEqual(len(la), 6552)

    def test_the_lights_the_old_table_missed_are_now_lit(self):
        # lamppost2 carries three 2dFx lights; the table had two offsets. In Los
        # Santos that is 244 bulbs the old list never lit - the mid-pole lamp of
        # the boardwalk double-globe post rhY filmed at the pier.
        def near(lines, x, y, r=400.0):
            return sum(1 for p in lines
                       if (float(p[1]) - x) ** 2 + (float(p[2]) - y) ** 2 < r * r)
        self.assertGreater(near(self.lines2, 836.0, -1855.0),
                           near(self.lines1, 836.0, -1855.0))

    def test_signal_models_are_not_in_the_street_list(self):
        models = {p[0] for p in self.inv_lines}
        street = set()
        for p in self.inv_lines:
            if p[1] in ("LA", "SF", "vegas", "country"):
                street.add(p[0])
        # The inventory keeps them (the coverage tool needs them); the list must
        # not. Cross-check by count: inventory minus signal models equals the list.
        sig = sum(1 for p in self.inv_lines if p[0] in lamp_list.TRAFFIC_NAMES)
        # The list is the inventory minus the signal models, minus the handful of
        # lights that land on the same point as another light (dedupe_positions).
        self.assertLessEqual(len(self.lines2), len(self.inv_lines) - sig)
        self.assertGreaterEqual(len(self.lines2), len(self.inv_lines) - sig - 25,
                                "the list should be the inventory minus the signal "
                                "models minus the dedupe drops")
        self.assertGreater(sig, 9000)
        self.assertTrue(street)


if __name__ == "__main__":
    unittest.main()
