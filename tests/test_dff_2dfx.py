#!/usr/bin/env python3
"""tests/test_dff_2dfx.py - the 2dFx reader that n11's whole lamp list comes from.

tools/dff_2dfx.py decodes the chunk gta-sa.exe registers as 0x0253F2F8. Everything
downstream - the lamp list, the coverage number, the per-bulb colour in the plugin -
is only as right as this parser, so the tests are about the FRAME (does the record
list close exactly, is the tag the low byte) as much as about the fields.

The synthetic chunks are built here rather than lifted out of a file so a change to
the parser that breaks the frame fails even on a machine with no game mounted; the
game-backed class then pins the same parser to the real archive.
"""
import os
import struct
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "tools"))
import dff_2dfx  # noqa: E402

CH = dff_2dfx.CH_2DFX


def chunk(typ, payload):
    """An RW chunk header + payload."""
    return struct.pack("<III", typ, len(payload), 0x18030000) + payload


def record(x, y, z, tagword, payload):
    return struct.pack("<3fII", x, y, z, tagword, len(payload)) + payload


def light_payload(rgb=(255, 148, 52, 200), rng=100.0, f8=12.0, f12=2.0, f16=8.0,
                  flags=b"\x00\x00\x00A", text=b"coronastar\x00shad_exp\x00"):
    """A tag-0 payload padded to the 80 bytes every light record of this game has."""
    body = (bytes(rgb) + struct.pack("<f", rng) + struct.pack("<3f", f8, f12, f16)
            + flags + text)
    assert len(body) <= 80, len(body)
    return body + b"\x00" * (80 - len(body))


def chunk_of(records):
    return chunk(CH, struct.pack("<I", len(records)) + b"".join(records))


class TestRecordFrame(unittest.TestCase):
    def test_two_records_close_and_tags_are_the_low_byte(self):
        data = chunk_of([
            record(-1.40, 0.22, 3.67, 0x00000000, light_payload()),
            record(0.0, 0.0, 0.0, 0x00000009, b"\x01\x00\x00\x00" + b"\x00" * 8),
        ])
        po = 12
        recs, ok = dff_2dfx.parse_2dfx(data, po, len(data) - po)
        self.assertTrue(ok, "the record list must end exactly at the end of the chunk")
        self.assertEqual([r["tag"] for r in recs], [0, 9])
        # A tag read from the TOP byte would call the second record a light too,
        # which is the bug that made every palm tree in Los Santos a lamp.
        self.assertNotIn("r", recs[1])
        self.assertEqual(recs[1]["size"], 12)

    def test_light_fields(self):
        data = chunk_of([record(-0.44, 0.05, 3.20, 0, light_payload())])
        recs, ok = dff_2dfx.parse_2dfx(data, 12, len(data) - 12)
        self.assertTrue(ok)
        r = recs[0]
        for got, want in zip((r["x"], r["y"], r["z"]), (-0.44, 0.05, 3.20)):
            self.assertAlmostEqual(got, want, places=5)  # stored as f32
        self.assertEqual((r["r"], r["g"], r["b"], r["a"]), (255, 148, 52, 200))
        self.assertAlmostEqual(r["range"], 100.0)
        self.assertAlmostEqual(r["f8"], 12.0)
        self.assertAlmostEqual(r["f12"], 2.0)
        self.assertAlmostEqual(r["f16"], 8.0)
        self.assertEqual(r["flags"], (0, 0, 0, 0x41))
        # payload_text() scans the payload for printable runs rather than
        # guessing field offsets, so a printable flag byte (0x41 = 'A') glues
        # itself to the first name. The names are still there.
        self.assertTrue(any("coronastar" in t for t in r["text"]), r["text"])
        self.assertTrue(any("shad_exp" in t for t in r["text"]), r["text"])

    def test_truncated_payload_is_reported_not_silently_accepted(self):
        data = chunk_of([record(0, 0, 0, 0, light_payload())])
        # Chop 7 bytes off the chunk: the list no longer closes.
        recs, ok = dff_2dfx.parse_2dfx(data, 12, len(data) - 12 - 7)
        self.assertFalse(ok)

    def test_count_that_lies_is_rejected(self):
        body = struct.pack("<I", 3) + record(0, 0, 0, 0, light_payload())
        data = chunk(CH, body)
        recs, ok = dff_2dfx.parse_2dfx(data, 12, len(data) - 12)
        self.assertFalse(ok)
        self.assertEqual(len(recs), 1)

    def test_empty_chunk(self):
        data = chunk(CH, struct.pack("<I", 0))
        recs, ok = dff_2dfx.parse_2dfx(data, 12, len(data) - 12)
        self.assertTrue(ok)
        self.assertEqual(recs, [])


class TestFindChunks(unittest.TestCase):
    def test_finds_a_2dfx_chunk_nested_in_the_clump(self):
        inner = chunk_of([record(0, 0, 0, 0, light_payload())])
        # clump -> geometry -> mat -> 2dFx, the nesting the real files use.
        data = chunk(0x00000010, chunk(0x0000000F, chunk(0x0000000A, inner)))
        found = dff_2dfx.find_2dfx_chunks(data)
        self.assertEqual(len(found), 1)
        po, ln = found[0]
        recs, ok = dff_2dfx.parse_2dfx(data, po, ln)
        self.assertTrue(ok)
        self.assertEqual(recs[0]["tag"], 0)

    def test_no_chunk_nothing_found(self):
        self.assertEqual(dff_2dfx.find_2dfx_chunks(chunk(0x00000010, b"\x00" * 16)), [])

    def test_lights_of_dff_keeps_only_tag_zero(self):
        data = chunk_of([
            record(0, 0, 0, 0, light_payload()),
            record(0, 0, 0, 7, b"\x00" * 88),
            record(0, 0, 0, 9, b"\x00" * 12),
        ])
        self.assertEqual(len(dff_2dfx.lights_of_dff(data)), 1)


GAME = "/media/rhy/data/Gamez/GTASA-RTX"


@unittest.skipUnless(os.path.isdir(GAME), f"no game folder at {GAME}")
class TestAgainstTheRealArchive(unittest.TestCase):
    """The parser pinned to gta3.img. These numbers are the ones the notes and
    FINDINGS quote; if one of them moves, the parser or the game changed."""

    @classmethod
    def setUpClass(cls):
        cls.img = os.path.join(GAME, "models", "gta3.img")
        cls.entries = dff_2dfx.img_entries(cls.img)
        res, stats = dff_2dfx.scan_img(cls.img, stats=True)
        cls.by_model = res
        cls.stats = stats

    def test_every_2dfx_chunk_closes_exactly(self):
        # 1591 of the 12239 .dff carry the chunk, and the record list closes on
        # every single one. A parser that is one byte wrong cannot do that.
        self.assertGreater(self.stats["chunks"], 1500)
        self.assertEqual(self.stats["chunks"], self.stats["closed"],
                         "some 2dFx chunk did not parse to its exact end")

    def test_light_record_count(self):
        self.assertEqual(sum(len(v) for v in self.by_model.values()), 944)

    def test_lamppost_records_match_the_lamp_table_offsets(self):
        # The table's lamppost3 offset (-1.30,0.17,3.71) and the model's own 2dFx
        # bulb (-1.40,0.22,3.67) are 0.11 m apart: the table was written from this
        # data, which is what makes the offsets it does have trustworthy.
        recs = self.by_model["lamppost3"]
        self.assertEqual(len(recs), 1)
        r = recs[0]
        self.assertAlmostEqual(r["x"], -1.40, delta=0.02)
        self.assertAlmostEqual(r["z"], 3.67, delta=0.02)
        self.assertEqual((r["r"], r["g"], r["b"]), (255, 148, 52))
        self.assertAlmostEqual(r["range"], 100.0)

    def test_lamppost2_carries_three_lights_and_the_table_only_two(self):
        # 244 of the 732 Los Santos lamppost2 lights the old list missed are this
        # third record. The model has always had it.
        recs = self.by_model["lamppost2"]
        self.assertEqual(len(recs), 3)
        zs = sorted(round(r["z"], 2) for r in recs)
        self.assertEqual(zs, [3.90, 5.32, 5.32])

    def test_signal_lens_colours_are_the_three_phases(self):
        recs = self.by_model["cj_traffic_light4"]
        cols = {(r["r"], r["g"], r["b"]) for r in recs}
        self.assertIn((255, 0, 0), cols)
        self.assertIn((255, 128, 0), cols)
        self.assertIn((0, 255, 0), cols)


if __name__ == "__main__":
    unittest.main()
