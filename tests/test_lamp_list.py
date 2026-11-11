import math
import sys
import unittest

sys.path.insert(0, "tools")
import lamp_list  # noqa: E402


class TestRotate(unittest.TestCase):
    def test_z90_rotates_x_to_y(self):
        # 90 degrees about Z: q = (0, 0, sin45, cos45)
        s = math.sin(math.radians(45))
        q = (0.0, 0.0, s, s)
        x, y, z = lamp_list.rotate((1.0, 0.0, 0.0), q)
        self.assertAlmostEqual(x, 0.0, places=6)
        self.assertAlmostEqual(y, 1.0, places=6)
        self.assertAlmostEqual(z, 0.0, places=6)

    def test_identity(self):
        x, y, z = lamp_list.rotate((1.0, 2.0, 3.0), (0.0, 0.0, 0.0, 1.0))
        self.assertAlmostEqual(x, 1.0)
        self.assertAlmostEqual(y, 2.0)
        self.assertAlmostEqual(z, 3.0)


class TestLampTable(unittest.TestCase):
    SAMPLE = """
return {
  [1231] = {
    ["type"] = 9,
    ["pos"] = {
      [1] = {
        ["y"] = 0,
        ["x"] = -0.36000001430511,
        ["z"] = 2.6500000953674,
      },
      [2] = {
        ["y"] = 0,
        ["x"] = 0.55000001192093,
        ["z"] = 2.6500000953674,
      },
    },
  },
  [1226] = {
    ["type"] = 4,
    ["pos"] = {
      [1] = {
        ["y"] = 0.17499999701977,
        ["x"] = -1.2999999523163,
        ["z"] = 3.710000038147,
      },
    },
  },
}
"""

    def test_two_lamps(self):
        lamps = lamp_list.parse_lamp_table(self.SAMPLE)
        self.assertEqual(sorted(lamps), [1226, 1231])
        self.assertEqual(lamps[1231][0], 9)
        self.assertEqual(len(lamps[1231][1]), 2)
        self.assertAlmostEqual(lamps[1231][1][0][0], -0.36, places=6)  # x
        self.assertAlmostEqual(lamps[1231][1][0][2], 2.65, places=6)   # z
        self.assertEqual(lamps[1226][0], 4)
        self.assertEqual(len(lamps[1226][1]), 1)
        self.assertAlmostEqual(lamps[1226][1][0][1], 0.175, places=6)  # y


class TestTextIPL(unittest.TestCase):
    SAMPLE = """# IPL generated from Max file
inst
16586, lod_cen_bit_10, 0, -21.453125, 1991.375, 21.3203125, 0, 0, 0, 1, -1
16587, streetlmp1, 0, 2475.5, -1650.25, 3.5, 0, 0, 0.7071068, 0.7071068, 0  # comment
16588, streetlmp2, 1024, 100, 200, 5, 0, 0, 0, 1, 0
end
cull
1, 2
end
"""

    def test_three_lines(self):
        insts = list(lamp_list.parse_text_ipl(self.SAMPLE))
        self.assertEqual(len(insts), 3)
        name, interior, pos, rot, lod = insts[0]
        self.assertEqual(name, "lod_cen_bit_10")
        self.assertEqual(interior, 0)
        self.assertAlmostEqual(pos[0], -21.453125)
        self.assertEqual(rot, (0.0, 0.0, 0.0, 1.0))
        self.assertEqual(lod, -1)
        self.assertEqual(insts[1][0], "streetlmp1")
        self.assertAlmostEqual(insts[1][3][2], 0.7071068)
        self.assertEqual(insts[2][1], 1024)  # interior of third


class TestIde(unittest.TestCase):
    SAMPLE = """# lampposts and a bit of everything else
1226, lamppost3, 0, 62.0, 4
1215, bollardlight, 0, 40.0, 2
# comment
1226, lamppost3, 0, 62.0, 4
broken line
"""

    def test_ids_names_radii(self):
        ide = lamp_list.parse_ide_from_text(self.SAMPLE)
        self.assertEqual(sorted(ide), [1215, 1226])
        self.assertEqual(ide[1226][0], "lamppost3")
        self.assertAlmostEqual(ide[1226][1], 62.0)

    def test_names_are_lowercased_for_lookup(self):
        ide = lamp_list.parse_ide_from_text("1226, LAMPPost3, 0, 62.0, 4")
        self.assertEqual(ide[1226][0], "lamppost3")


class TestDedupePositions(unittest.TestCase):
    def test_same_point_from_two_models_keeps_one(self):
        # The old key was (model, position), so a lamp instanced by two model ids
        # produced two lines and burned two light slots for one lamp post.
        entries = [((10.0, 20.0, 3.0), (1226, "a")), ((10.04, 20.0, 3.0), (1227, "b"))]
        kept, dropped = lamp_list.dedupe_positions(entries)
        self.assertEqual(len(kept), 1)
        self.assertEqual(dropped, 1)

    def test_offsets_landing_on_one_point_keep_one(self):
        entries = [((5.0, 5.0, 4.5), 11), ((5.0, 5.0, 4.5), 11), ((5.0, 5.0, 5.5), 11)]
        kept, dropped = lamp_list.dedupe_positions(entries)
        self.assertEqual([p for p, _ in kept], [(5.0, 5.0, 4.5), (5.0, 5.0, 5.5)])
        self.assertEqual(dropped, 1)

    def test_far_apart_lamps_are_kept(self):
        entries = [((10.0, 20.0, 3.0), 1), ((13.0, 20.0, 3.0), 1)]
        kept, dropped = lamp_list.dedupe_positions(entries)
        self.assertEqual(len(kept), 2)
        self.assertEqual(dropped, 0)

    def test_input_order_is_preserved(self):
        entries = [((1.0, 2.0, 3.0), "first"), ((9.0, 2.0, 3.0), "second")]
        kept, _dropped = lamp_list.dedupe_positions(entries)
        self.assertEqual([payload for _pos, payload in kept], ["first", "second"])


if __name__ == "__main__":
    unittest.main()
