#!/usr/bin/env python3
"""n21 — tests for tools/vegetation_edge_measure.py (leaf edge quality, frames and textures).

The frame metric is only meaningful if it separates a hard alpha cutout from an alpha blend, so
the frames here are synthetic and known: leaf-coloured stripes on a background, with a ramp of
1 / 2 / 4 pixels at every edge. The assertions are the ordering and the widths, not a magic
number. The texture side is checked against a .txd built in memory whose alpha census is exact.
"""
import json
import os
import struct
import subprocess
import sys
import unittest

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools"))
import remix_texhash as R            # noqa: E402
import vegetation_edge_measure as E  # noqa: E402

REPO = os.path.join(os.path.dirname(__file__), "..")
TOOL = os.path.join(REPO, "tools", "vegetation_edge_measure.py")
BG = (51, 61, 61)
LEAF = (82, 97, 64)


def stripe_image(path, ramp, h=120, w=240):
    """Leaf stripes 24 px wide every 48 px, each edge softened over `ramp` pixels."""
    img = np.zeros((h, w, 3), dtype=np.float64)
    img[:, :] = np.array(BG, dtype=np.float64)
    for x0 in range(12, w, 48):
        for x in range(x0, x0 + 24):
            t = 1.0
            d = min(x - x0, x0 + 23 - x)
            if ramp > 1 and d < ramp:
                t = (d + 1) / float(ramp)
            img[:, x] = np.array(BG) * (1 - t) + np.array(LEAF) * t
    Image.fromarray(img.astype(np.uint8)).save(path)
    return path


def dxt3_block(alpha_nibbles):
    out = bytearray()
    for row in range(4):
        lo = mid = 0
        for col in range(4):
            n = alpha_nibbles[row * 4 + col] & 15
            if col < 2:
                lo |= n << (4 * col)
            else:
                mid |= n << (4 * (col - 2))
        out += bytes([lo, mid])
    return bytes(out) + b"\0" * 8


def txd_bytes(textures):
    """textures: [(name, w, h, fourcc, depth, pixels)] -> the bytes of one .txd (as in
    tests/test_vegetation_hashes.py — duplicated on purpose, tests must stand alone)."""
    subs = [struct.pack("<III", R.CH_NATIVE, 4, 0x1803FFFF) + struct.pack("<I", 0)]
    for name, w, h, fourcc, depth, px in textures:
        raster = (struct.pack("<I", 0x300) + (fourcc or b"\0\0\0\0") + struct.pack("<HH", w, h) +
                  struct.pack("<BB", depth, 1) + struct.pack("<H", 0x0904) +
                  struct.pack("<I", len(px)))
        body = (struct.pack("<III", R.CH_NATIVE, 0, 0x1803FFFF) +
                struct.pack("<II", 0x08, 32) + name.encode().ljust(32, b"\0") +
                b"\0" * 32 + raster + px)
        subs.append(struct.pack("<III", R.CH_TEX, len(body), 0x1803FFFF) + body)
    subs.append(struct.pack("<III", 0x03, 0, 0x1803FFFF))
    content = b"".join(subs)
    return struct.pack("<III", R.CH_TEXDICT, len(content), 0x1803FFFF) + content


class TestFrames(unittest.TestCase):
    def setUp(self):
        import tempfile
        self.d = tempfile.mkdtemp()

    def tearDown(self):
        import shutil
        shutil.rmtree(self.d, ignore_errors=True)

    def p(self, name):
        return os.path.join(self.d, name)

    def test_hard_cut_has_no_mid_alpha_band(self):
        r = E.measure_frame(stripe_image(self.p("hard.png"), 1))
        self.assertEqual(r["mid_alpha_pct"], 0.0)
        self.assertIsNone(r["edge_width_px"])            # no ramp pixels at all
        self.assertGreater(r["foliage_pct"], 15.0)

    def test_soft_edges_raise_both_numbers(self):
        a = E.measure_frame(stripe_image(self.p("r2.png"), 2))
        b = E.measure_frame(stripe_image(self.p("r4.png"), 4))
        self.assertGreater(a["mid_alpha_pct"], 0.0)
        self.assertGreater(b["mid_alpha_pct"], a["mid_alpha_pct"])
        self.assertEqual(a["edge_width_px"], 1.0)        # one ramp pixel per edge inside the zone
        self.assertEqual(b["edge_width_px"], 3.0)        # three of the four ramp pixels

    def test_grey_frame_is_not_foliage(self):
        img = np.full((80, 80, 3), 120, dtype=np.uint8)
        Image.fromarray(img).save(self.p("grey.png"))
        r = E.measure_frame(self.p("grey.png"))
        self.assertIsNone(r["mid_alpha_pct"])
        self.assertIn("no foliage", r["note"])

    def test_cli_and_json(self):
        stripe_image(self.p("hard.png"), 1)
        stripe_image(self.p("r4.png"), 4)
        out = self.p("out.json")
        p = subprocess.run([sys.executable, TOOL, self.p("hard.png"), self.p("r4.png"),
                            "--json", out], capture_output=True, text=True)
        self.assertEqual(p.returncode, 0, p.stderr)
        rows = json.load(open(out))
        self.assertEqual(len(rows), 2)
        self.assertIn("mid_alpha_pct", rows[0])

    def test_name_filter(self):
        stripe_image(self.p("hard.png"), 1)
        stripe_image(self.p("other.png"), 4)
        p = subprocess.run([sys.executable, TOOL, self.p("hard.png"), self.p("other.png"),
                            "--name", "hard"], capture_output=True, text=True)
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertIn("hard.png", p.stdout)
        self.assertNotIn("other.png", p.stdout)


class TestTextures(unittest.TestCase):
    def setUp(self):
        import tempfile
        self.d = tempfile.mkdtemp()
        # one 4x4 DXT3 card: 8 texels alpha 15 (=255), 4 at 8 (=136), 4 at 0
        nib = [15] * 8 + [8] * 4 + [0] * 4
        card = dxt3_block(nib)
        # one opaque 4x4 A8R8G8B8 bark texture
        bark = b"".join(struct.pack("<4B", 200, 120, 60, 255) for _ in range(16))
        self.txd = os.path.join(self.d, "gta_proc_bush.txd")
        open(self.txd, "wb").write(txd_bytes([("card", 4, 4, b"DXT3", 16, card),
                                              ("bark", 4, 4, b"", 32, bark)]))

    def tearDown(self):
        import shutil
        shutil.rmtree(self.d, ignore_errors=True)

    def test_alpha_census_is_exact(self):
        r = E.measure_txd(self.txd)
        self.assertEqual(r["textures"], 2)
        self.assertEqual(r["texels"], 32)
        self.assertAlmostEqual(r["alpha0_pct"], 100.0 * 4 / 32, places=3)
        self.assertAlmostEqual(r["mid_alpha_pct"], 100.0 * 4 / 32, places=3)
        self.assertAlmostEqual(r["opaque_pct"], 100.0 * 24 / 32, places=3)

    def test_pattern_selects_dictionaries(self):
        self.assertEqual(E.measure_txd(self.txd, "^nope$").get("textures"), 0)
        self.assertEqual(E.measure_txd(self.txd, "^gta_proc").get("textures"), 2)

    def test_a_non_txd_does_not_crash_the_census(self):
        junk = os.path.join(self.d, "not_a_txd.txd")
        open(junk, "wb").write(b"fake plant1\n")
        r = E.measure_txd(junk)
        self.assertEqual(r["textures"], 0)


if __name__ == "__main__":
    unittest.main()
