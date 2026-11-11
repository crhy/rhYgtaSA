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


if __name__ == "__main__":
    unittest.main()
