#!/usr/bin/env python3
"""s32 — tests for tools/vegetation_hashes.py (the rtx.hairCardTextures list).

Two layers, no game folder needed:
  1. the allow-list: the default pattern must accept the dictionaries that really hold foliage
     cards and must reject the ones a loose prefix would drag in (vegasbuild/vegasneon are Las
     Vegas *buildings*, tikigrass is pool/concrete/wall textures);
  2. the alpha rule, on a .txd built in memory: a DXT3 card with soft alpha is tagged, a fully
     opaque A8R8G8B8 bark texture is not, and the emitted rtx.conf line is one sorted line of
     0x%016X hashes.
"""
import os
import re
import struct
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools"))
import remix_texhash as R  # noqa: E402
import vegetation_hashes as V  # noqa: E402

REPO = os.path.join(os.path.dirname(__file__), "..")


def dxt3_block(alpha_nibbles, colour=b"\0\0\0\0\0\0\0\0"):
    """16 alpha nibbles (row-major) + 8 bytes of colour -> a 16-byte DXT3 block."""
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
    return bytes(out) + colour


def a8r8g8b8(pixels):
    return b"".join(struct.pack("<4B", *p) for p in pixels)


def txd_bytes(textures):
    """textures: [(name, w, h, fourcc, depth, pixels)] -> the bytes of one .txd."""
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


class TestAllowList(unittest.TestCase):
    def test_accepts_foliage_dictionaries(self):
        for n in ("gta_proc_rushes", "gta_proc_grassland", "gta_procflowers", "lod_veg",
                  "tree1prc", "veg_fuzzyplant", "kbplantssmz2", "gta_tree_palm", "canopy"):
            self.assertTrue(re.match(V.VEG_RE, n), n)

    def test_rejects_lookalikes(self):
        for n in ("vegasbuild", "vegasneon", "dyn_vegas", "tikigrass", "streetlights",
                  "treeshad", "badlands", "underwater", "junkpiles", "grasshouse",
                  "vegashse3"):
            self.assertFalse(re.match(V.VEG_RE, n), n)


class TestAlphaRule(unittest.TestCase):
    def setUp(self):
        # a grass card: half the texels fully transparent, a quarter soft, the rest opaque
        soft = [0] * 8 + [8] * 4 + [15] * 4
        blocks = b"".join(dxt3_block(soft) for _ in range(4))          # 8x8 = 4 blocks
        bark = a8r8g8b8([(60, 90, 40, 255)] * 64)                      # 8x8, no transparency
        wall = b"\0\0\0\0\0\0\0\0" * 4                                 # DXT1, 4-colour, no holes
        self.txd = txd_bytes([("grasscard", 8, 8, b"DXT3", 16, blocks),
                              ("bark", 8, 8, b"", 32, bark),
                              ("wall", 8, 8, b"DXT1", 16, wall)])
        self.tmp = tempfile.TemporaryDirectory()
        self.path = os.path.join(self.tmp.name, "gta_proc_test.txd")
        with open(self.path, "wb") as fh:
            fh.write(self.txd)

    def tearDown(self):
        self.tmp.cleanup()

    def run_tool(self, *extra):
        r = subprocess.run([sys.executable, os.path.join(REPO, "tools", "vegetation_hashes.py"),
                            self.path] + list(extra), capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        return r.stdout

    def test_alpha_decode_of_dxt3_card(self):
        texes = R.parse_txd(self.txd, "test")
        card = [t for t in texes if t.name == "grasscard"][0]
        fc = V.fourcc_of(card, self.txd)
        self.assertEqual(fc, b"DXT3")           # remix_texhash reads it as "16bit"; we do not
        a = V.alpha_of(card, card.d3d9_mip0(self.txd), fc)
        self.assertEqual(int((a == 0).sum()), 32)
        self.assertEqual(int(((a > 0) & (a < 255)).sum()), 16)
        self.assertEqual(int((a == 255).sum()), 16)

    def test_only_the_card_is_tagged(self):
        out = self.run_tool("--verbose")
        line = [l for l in out.splitlines() if l.startswith("rtx.")]
        self.assertEqual(len(line), 0)          # no --emit -> no line
        self.assertIn("1 card texture(s) in 1 dictionary(ies) out of 3 texture(s)", out)
        rows = [l for l in out.splitlines() if "grasscard" in l]
        self.assertEqual(len(rows), 1)
        self.assertIn("50.00", rows[0])         # 50 % fully transparent
        self.assertIn("25.00", rows[0])         # 25 % mid
        self.assertIn("bark", out.split("not tagged")[1])
        self.assertIn("wall", out.split("not tagged")[1])

    def test_emit_line_shape_and_threshold(self):
        out = self.run_tool("--emit", "rtx.hairCardTextures")
        line = [l for l in out.splitlines() if l.startswith("rtx.hairCardTextures = ")][0]
        hashes = line.split(" = ", 1)[1].split(", ")
        self.assertEqual(len(hashes), 1)
        self.assertRegex(hashes[0], r"^0x[0-9A-F]{16}$")
        self.assertEqual(hashes, sorted(hashes))
        # raising the threshold past the card's 50 % transparent texels drops it to nothing
        out2 = self.run_tool("--min-cut", "60", "--emit", "rtx.hairCardTextures")
        self.assertIn("0 card texture(s)", out2)


class TestAgainstShippedProfile(unittest.TestCase):
    def test_shipped_line_is_a_superset_of_the_tool_default(self):
        """config/rtx.conf carries the 119-hash line; the tool's default allow-list must be the
        thing that produced it (this test does not need the game: it only checks the file)."""
        conf = os.path.join(REPO, "config", "rtx.conf")
        lines = [l.strip() for l in open(conf, encoding="utf-8", errors="replace")
                 if l.startswith("rtx.hairCardTextures")]
        self.assertEqual(len(lines), 1)
        hashes = lines[0].split(" = ", 1)[1].split(", ")
        self.assertEqual(len(hashes), 119)
        self.assertTrue(all(re.match(r"^0x[0-9A-F]{16}$", h) for h in hashes))
        self.assertEqual(hashes, sorted(hashes))
        perf = [l.strip() for l in open(os.path.join(REPO, "config", "rtx.conf.perf"),
                                        encoding="utf-8", errors="replace")
                if l.startswith("rtx.hairCardTextures")]
        self.assertEqual(perf, lines)


if __name__ == "__main__":
    unittest.main(verbosity=2)
