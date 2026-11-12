import struct
import sys
import unittest

sys.path.insert(0, "tools")
import traffic_list  # noqa: E402


class TestIde(unittest.TestCase):
    SAMPLE = """# dynamic.ide
1283, MTraffic1, 0, 85.0, 12
1284, MTraffic2, 0, 85.0, 12
1315, trafficlight1, 0, 64.0, 8
# comment line
bad line here
1350, CJ_TRAFFIC_LIGHT4, 0, 60.0, 6
"""

    def test_ids_names_radii(self):
        ide = traffic_list.parse_ide_from_text(self.SAMPLE)
        self.assertEqual(sorted(ide), [1283, 1284, 1315, 1350])
        self.assertEqual(ide[1315][0], "trafficlight1")
        self.assertAlmostEqual(ide[1283][1], 85.0)

    def test_names_are_lowercased(self):
        ide = traffic_list.parse_ide_from_text(self.SAMPLE)
        self.assertEqual(ide[1350][0], "cj_traffic_light4")


class TestBinaryIpl(unittest.TestCase):
    def blob(self, records):
        start = 0x20
        parts = [b"bnry", struct.pack("<I", len(records)),
                 b"\0" * (0x1C - 8), struct.pack("<I", start)]
        for pos, rot, model, interior, lod in records:
            parts.append(struct.pack("<3f", *pos))
            parts.append(struct.pack("<4f", *rot))
            parts.append(struct.pack("<3i", model, interior, lod))
        return b"".join(parts)

    def test_two_instances(self):
        data = self.blob([
            ((2495.0, -1680.0, 3.5), (0.0, 0.0, 0.0, 1.0), 1283, 0, 0),
            ((100.5, 20.25, 4.0), (0.0, 0.7071, 0.0, 0.7071), 1350, 0, 1),
        ])
        out = list(traffic_list.parse_binary_ipl(data))
        self.assertEqual(len(out), 2)
        self.assertAlmostEqual(out[0][3][1], -1680.0)
        self.assertEqual(out[0][0], 1283)
        self.assertEqual(out[1][1], 0)
        self.assertEqual(out[1][2], 1)

    def test_not_a_binary_ipl(self):
        self.assertEqual(list(traffic_list.parse_binary_ipl(b"VER2 junk")), [])

    def test_truncated_records_stop(self):
        data = self.blob([((1.0, 2.0, 3.0), (0.0, 0.0, 0.0, 1.0), 1, 0, 0)])
        out = list(traffic_list.parse_binary_ipl(data + b"\0" * 10))
        self.assertEqual(len(out), 1)


class TestDedupe(unittest.TestCase):
    def test_lod_copy_dropped(self):
        insts = [(1283, (10.0, 20.0, 3.0)), (1283, (10.05, 20.0, 3.0)),
                 (1283, (40.0, 20.0, 3.0))]
        self.assertEqual(len(traffic_list.dedupe(insts)), 2)

    def test_different_model_kept(self):
        insts = [(1283, (10.0, 20.0, 3.0)), (1284, (10.0, 20.0, 3.0))]
        self.assertEqual(len(traffic_list.dedupe(insts)), 2)

    def test_order_preserved(self):
        insts = [(1283, (1.0, 2.0, 3.0)), (1283, (9.0, 2.0, 3.0))]
        kept = traffic_list.dedupe(insts)
        self.assertAlmostEqual(kept[0][1][0], 1.0)
        self.assertAlmostEqual(kept[1][1][0], 9.0)


class TestHeadz(unittest.TestCase):
    def test_pole_models(self):
        # 85 game units = 8.5 m bounding radius -> head at 7.65 m
        self.assertAlmostEqual(traffic_list.headz(85.0), 7.65)

    def test_small_head(self):
        self.assertAlmostEqual(traffic_list.headz(50.0), 4.5)


class TestHeadzFor(unittest.TestCase):
    # The lamp table's exact head offsets, and the .ide radii to match on.
    TABLE = {
        1350: [(-2.1, 0.1, 7.6)],    # cj_traffic_light4, r=60
        1351: [(-1.75, 0.0, 6.0)],   # cj_traffic_light5, r=60
        3855: [(-2.1, 0.12, 7.65)],  # gay_traffic_light, r=50
    }
    RADII = {1350: ("cj_traffic_light4", 60.0),
             1351: ("cj_traffic_light5", 60.0),
             3855: ("gay_traffic_light", 50.0)}

    def test_exact_table_offset_wins(self):
        self.assertAlmostEqual(
            traffic_list.headz_for(1350, 60.0, self.TABLE, self.RADII), 7.6)

    def test_uncovered_model_uses_the_closest_radius(self):
        # trafficlight1 (r=64) is a street traffic light: it belongs with the
        # 60-unit heads, not with the radius*0.09 estimate (5.76), which put the
        # glow 2 units below the head the game draws.
        self.assertAlmostEqual(
            traffic_list.headz_for(1315, 64.0, self.TABLE, self.RADII), 7.6)

    def test_radius_tie_goes_to_the_higher_head(self):
        # cj_traffic_light3 (r=60) ties between the two 60-unit table models.
        self.assertAlmostEqual(
            traffic_list.headz_for(1352, 60.0, self.TABLE, self.RADII), 7.6)

    def test_no_table_falls_back_to_the_radius_estimate(self):
        self.assertAlmostEqual(traffic_list.headz_for(1315, 64.0, {}, self.RADII), 5.76)

    def test_model_with_no_offsets_falls_back(self):
        table = dict(self.TABLE)
        table[1315] = []
        self.assertAlmostEqual(
            traffic_list.headz_for(1315, 64.0, table, self.RADII), 7.6)


class TestHeading(unittest.TestCase):
    """The IPL instance rotation, as the yaw the v2 file ships.

    Every traffic instance in gta3.img carries a pure Z-axis quaternion (measured
    over all 1524 raw instances: x = y = 0, |q| = 1), so a yaw is the whole
    rotation - which is what makes a heading column plus a model-local offset
    enough to put a light on a lens.
    """

    def qz(self, deg):
        """A pure Z-axis quaternion for a yaw of deg degrees."""
        import math
        h = math.radians(deg) / 2.0
        return (0.0, 0.0, math.sin(h), math.cos(h))

    def test_cardinal_yaws(self):
        for deg in (0.0, 90.0, 180.0, 270.0):
            self.assertAlmostEqual(traffic_list.heading_deg(self.qz(deg)), deg, 1)

    def test_wraps_to_0_360(self):
        self.assertAlmostEqual(traffic_list.heading_deg(self.qz(-90.0)), 270.0, 1)

    def test_identity(self):
        self.assertAlmostEqual(traffic_list.heading_deg((0.0, 0.0, 0.0, 1.0)), 0.0, 1)

    def test_the_real_mix_of_the_shipped_file(self):
        # 1304 of the 1522 instances are cardinal, 42 are 45-degree, 176 other:
        # the column is real data, not a constant.
        self.assertAlmostEqual(traffic_list.heading_deg(self.qz(33.75)), 33.7, 1)  # round() is half-to-even


class TestRotateYaw(unittest.TestCase):
    OFFSET = (-2.1, 0.1, 7.6)     # cj_traffic_light4's head, model-local

    def test_cardinal_rotations(self):
        want = {0.0: (-2.1, 0.1), 90.0: (-0.1, -2.1),
                180.0: (2.1, -0.1), 270.0: (0.1, 2.1)}
        for deg, (x, y) in want.items():
            out = traffic_list.rotate_yaw(self.OFFSET, deg)
            self.assertAlmostEqual(out[0], x, 3)
            self.assertAlmostEqual(out[1], y, 3)
            self.assertAlmostEqual(out[2], 7.6, 6)   # a yaw leaves z alone

    def test_rotation_is_rigid(self):
        import math
        for deg in (0.0, 17.0, 90.0, 137.5, 359.0):
            out = traffic_list.rotate_yaw(self.OFFSET, deg)
            self.assertAlmostEqual(math.sqrt(sum(v * v for v in out)),
                                   math.sqrt(sum(v * v for v in self.OFFSET)), 6)

    def test_the_sideways_part_is_what_the_v1_file_lost(self):
        import math
        x, y, _z = self.OFFSET
        self.assertAlmostEqual(math.hypot(x, y), 2.1024, 3)


class TestLensOffsetFor(unittest.TestCase):
    TABLE = {
        1350: [(-2.1, 0.1, 7.6)],    # cj_traffic_light4, r=60
        1351: [(-1.75, 0.0, 6.0)],   # cj_traffic_light5, r=60
        3855: [(-2.1, 0.12, 7.65)],  # gay_traffic_light, r=50
    }
    RADII = {1350: ("cj_traffic_light4", 60.0),
             1351: ("cj_traffic_light5", 60.0),
             3855: ("gay_traffic_light", 50.0)}

    def test_a_covered_model_gets_its_own_offset(self):
        self.assertEqual(traffic_list.lens_offset_for(1350, 60.0, self.TABLE,
                                                      self.RADII),
                         (-2.1, 0.1, 7.6))

    def test_a_nearby_radius_borrows_the_offset(self):
        # trafficlight1 (r=64): 4 units from the 60-unit models, the same lamp
        # family, so the sideways arm is worth keeping.
        self.assertEqual(traffic_list.lens_offset_for(1315, 64.0, self.TABLE,
                                                      self.RADII),
                         (-2.1, 0.1, 7.6))

    def test_a_distant_radius_does_not_borrow_the_arm(self):
        # mtraffic1 (r=85) is 25 units out: a height can be borrowed across that,
        # a 2 m mast arm cannot. None means "keep the pole x/y".
        self.assertIsNone(traffic_list.lens_offset_for(1283, 85.0, self.TABLE,
                                                       self.RADII))
        # ... and the gap is a stated number, not a mood
        self.assertIsNotNone(traffic_list.lens_offset_for(1283, 85.0, self.TABLE,
                                                          self.RADII, 25.0))

    def test_a_radius_tie_takes_the_head_furthest_off_the_mast(self):
        # cj_traffic_light3 (r=60) ties with both 60-unit table models; 1350's
        # head is 2.10 m sideways, 1351's 1.75 m.
        self.assertEqual(traffic_list.lens_offset_for(1352, 60.0, self.TABLE,
                                                      self.RADII),
                         (-2.1, 0.1, 7.6))

    def test_no_table_no_offset(self):
        self.assertIsNone(traffic_list.lens_offset_for(1315, 64.0, {}, self.RADII))

    def test_the_offset_is_the_highest_head_of_the_model(self):
        table = {1350: [(-1.0, 0.0, 4.0), (-2.1, 0.1, 7.6)]}
        self.assertEqual(traffic_list.lens_offset_for(1350, 60.0, table,
                                                      {1350: ("x", 60.0)}),
                         (-2.1, 0.1, 7.6))

    def test_headz_and_the_offset_agree_on_z(self):
        for mid, radius in ((1350, 60.0), (1315, 64.0), (1352, 60.0)):
            off = traffic_list.lens_offset_for(mid, radius, self.TABLE, self.RADII)
            self.assertAlmostEqual(off[2],
                                   traffic_list.headz_for(mid, radius, self.TABLE,
                                                          self.RADII))


class TestDedupeCarriesTheRotation(unittest.TestCase):
    def test_three_tuples_pass_through(self):
        insts = [(1350, (10.0, 20.0, 3.0), (0.0, 0.0, 0.0, 1.0)),
                 (1350, (10.05, 20.0, 3.0), (0.0, 0.0, 0.7071, 0.7071)),
                 (1350, (40.0, 20.0, 3.0), (0.0, 0.0, 1.0, 0.0))]
        kept = traffic_list.dedupe(insts)
        self.assertEqual(len(kept), 2)
        self.assertEqual(kept[0][2], (0.0, 0.0, 0.0, 1.0))
        self.assertEqual(kept[1][2], (0.0, 0.0, 1.0, 0.0))


GAME = "/media/rhy/data/Gamez/GTASA-RTX"


class TestGeneratedFile(unittest.TestCase):
    """End to end, against the read-only game folder: the v2 file the plugin will
    read, and the rule that a v1 file still means exactly what it meant before.
    Skipped when the game folder is not mounted."""

    @classmethod
    def setUpClass(cls):
        import os
        import subprocess
        import sys
        if not os.path.isdir(GAME):
            raise unittest.SkipTest(f"no game folder at {GAME}")
        cls.lines = {}
        for tag, extra in (("v2", []), ("v1", ["--v1"])):
            out = os.path.join(os.environ.get("TMPDIR", "/tmp"),
                               f"n02-traffic-{tag}.txt")
            r = subprocess.run(
                [sys.executable, os.path.join("tools", "traffic_list.py"),
                 "--game", GAME, "--out", out] + extra,
                capture_output=True, text=True)
            assert r.returncode == 0, r.stdout + r.stderr
            with open(out) as fh:
                cls.lines[tag] = [ln.split() for ln in fh.read().splitlines()
                                  if ln.strip() and not ln.startswith("#")]

    def test_v2_header_and_eight_columns(self):
        self.assertEqual(len(self.lines["v2"]), len(self.lines["v1"]))
        self.assertTrue(all(len(f) == 8 for f in self.lines["v2"]),
                        "a v2 line must carry x y z headz heading lensx lensy lensz")
        self.assertTrue(all(len(f) == 4 for f in self.lines["v1"]))

    def test_the_first_four_columns_are_unchanged(self):
        """The compatibility rule: a v1 reader (or lens_offset=0) sees the same
        file it saw before n02, line for line in the same order."""
        self.assertEqual([f[:4] for f in self.lines["v2"]], self.lines["v1"])

    def test_headings_are_a_real_mix(self):
        heads = {round(float(f[4])) for f in self.lines["v2"]}
        self.assertGreater(len(heads), 4,
                           f"only {len(heads)} distinct headings: the column is "
                           "not carrying the IPL rotations")
        self.assertTrue(all(0.0 <= float(f[4]) < 360.0 for f in self.lines["v2"]))

    def test_the_lens_offset_is_the_lamp_tables(self):
        # cj_traffic_light4 (720 of the 1522 instances) is covered by the table.
        offs = {tuple(round(float(v), 2) for v in f[5:8]) for f in self.lines["v2"]}
        self.assertIn((-2.1, 0.1, 7.6), offs)
        # and no line moves the light more than the widest arm in the table
        for f in self.lines["v2"]:
            self.assertLessEqual(abs(float(f[5])) + abs(float(f[6])), 3.0)

    def test_headz_equals_the_offset_height(self):
        for f in self.lines["v2"]:
            self.assertAlmostEqual(float(f[3]), float(f[7]), 2)


if __name__ == "__main__":
    unittest.main()
