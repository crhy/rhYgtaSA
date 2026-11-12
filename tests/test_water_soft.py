#!/usr/bin/env python3
"""s32 — tests for tools/water_normal_soft.py and the 'soft' variant of tools/water.sh.

Three layers, none of which needs the game folder:
  1. the octahedral codec: encode_octa/decode_octa_rg must round-trip (the octahedral map is not
     "store x,y" - without the 1/(|x|+|y|+z) scale a 30 deg normal comes back as 45 deg, which is
     exactly the bug this test caught while the tool was written);
  2. a synthetic BC5 sheet (20 frames, encoded here with an independent BC4 writer) through the
     tool: the output must be a loadable DDS with the same frame count, a median tilt at the
     target, and a much smaller tilt gradient;
  3. tools/water.sh against a fake game tree: `soft` generates the sheet and installs the USD,
     `status` names the variant, `pack` restores the original byte for byte.
"""
import os
import struct
import subprocess
import sys
import tempfile
import unittest

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.join(HERE, "..")
sys.path.insert(0, os.path.join(REPO, "tools"))
import water_normal_soft as W  # noqa: E402

FRAMES = 20
FW = 16                      # px per frame in the synthetic sheet


def bc4_block(vals):
    """16 uint8 -> an 8-byte BC4 block (independent of the decoder in the tool)."""
    a0, a1 = int(max(vals)), int(min(vals))
    lut = [a0, a1] + [(6 * a0 + a1) // 7, (5 * a0 + 2 * a1) // 7, (4 * a0 + 3 * a1) // 7,
                      (3 * a0 + 4 * a1) // 7, (2 * a0 + 5 * a1) // 7, (a0 + 6 * a1) // 7]
    codes = [min(range(8), key=lambda i: abs(lut[i] - v)) for v in vals]
    bits = 0
    for j, c in enumerate(codes):
        bits |= (c & 7) << (3 * j)
    return bytes([a0, a1] + [(bits >> (8 * i)) & 255 for i in range(6)])


def bc5_dds(path, R, G):
    """Write a DX10/BC5 DDS holding the two given (H,W) uint8 planes."""
    h, w = R.shape
    out = []
    for by in range(h // 4):
        for bx in range(w // 4):
            blk = lambda p: [int(p[by * 4 + j, bx * 4 + i]) for j in range(4) for i in range(4)]
            out.append(bc4_block(blk(R)) + bc4_block(blk(G)))
    pixels = b"".join(out)
    hdr = (b"DDS " + struct.pack("<IIIIIII", 124, 0x1 | 0x2 | 0x4 | 0x1000, h, w, 0, 1, 0) +
           b"\0" * 44 + struct.pack("<II4sIIIII", 32, 0x40 | 0x2, b"DX10", 0, 0, 0, 0, 0) +
           struct.pack("<IIIII", 0x401008, 0, 0, 0, 0))
    dx10 = struct.pack("<IIIII", W.DXGI_BC5, 3, 0, 1, 0)
    assert len(hdr) == 128 and len(dx10) == 20, (len(hdr), len(dx10))
    with open(path, "wb") as fh:
        fh.write(hdr + dx10 + pixels)
    return len(pixels)


def synthetic_planes(h=FW, w=FW * FRAMES, seed=7):
    rng = np.random.RandomState(seed)
    R = rng.randint(0, 256, size=(h, w)).astype(np.uint8)
    G = rng.randint(0, 256, size=(h, w)).astype(np.uint8)
    return R, G


def decode_dds_rgba(path):
    w, h, dxgi, payload = W.read_dds(path)
    a = np.frombuffer(payload[:w * h * 4], dtype=np.uint8).reshape(h, w, 4)
    return w, h, dxgi, a


def rb(path):
    with open(path, "rb") as fh:
        return fh.read()


def mip_chain(w, h):
    """The mip sizes the tool writes: box filter by 2, repeating the last row/column if odd."""
    out = [(w, h)]
    while (w, h) != (1, 1):
        w, h = max(1, (w + 1) // 2), max(1, (h + 1) // 2)
        out.append((w, h))
    return out


class TestCodec(unittest.TestCase):
    def test_octa_round_trip(self):
        rng = np.random.RandomState(3)
        v = rng.normal(size=(3, 4000))
        v[2] = np.abs(v[2])
        n = v / np.linalg.norm(v, axis=0, keepdims=True)
        R, G = W.encode_octa(n)
        back = W.decode_octa_rg(R, G)
        ang = np.degrees(np.arccos(np.clip((n * back).sum(0), -1, 1)))
        self.assertLess(float(np.percentile(ang, 99)), 1.0)      # 8-bit octahedral is ~0.3 deg

    def test_strength_lowers_tilt_monotonically(self):
        rng = np.random.RandomState(5)
        v = rng.normal(size=(3, 2000))
        v[2] = np.abs(v[2])
        n = v / np.linalg.norm(v, axis=0, keepdims=True)
        med = [float(np.median(W.tilt_deg(W.apply_strength(n, f)))) for f in (1.0, 0.5, 0.25, 0.0)]
        self.assertEqual(med, sorted(med, reverse=True))
        self.assertAlmostEqual(med[0], float(np.median(W.tilt_deg(n))), places=6)
        self.assertAlmostEqual(med[-1], 0.0, places=3)

    def test_decimate_keeps_mean(self):
        ex = np.arange(64, dtype=np.float64).reshape(8, 8)
        ey = np.zeros_like(ex)
        a, b = W.decimate_xy(ex, ey, 2)
        self.assertEqual(a.shape, (4, 4))
        self.assertAlmostEqual(float(a.mean()), float(ex.mean()), places=9)


class TestSheet(unittest.TestCase):
    def test_tool_softens_synthetic_sheet(self):
        with tempfile.TemporaryDirectory() as d:
            src, dst = os.path.join(d, "water_normal.dds"), os.path.join(d, "soft.dds")
            R, G = synthetic_planes()
            bc5_dds(src, R, G)
            n0 = W.decode_octa_rg(R, G)
            t0 = W.tilt_deg(n0)
            self.assertGreater(float(np.median(t0)), 30.0)       # the "bad" sheet we are fixing
            r = subprocess.run([sys.executable, os.path.join(REPO, "tools", "water_normal_soft.py"),
                                src, dst, "--target-tilt", "12"], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertIn("median facet tilt", r.stdout)
            w, h, dxgi, a = decode_dds_rgba(dst)
            self.assertEqual((w, h), (FW * FRAMES // 4, FW // 4))
            self.assertIsNone(dxgi)                              # classic header, RGBA8
            self.assertTrue((a[..., 3] == 255).all())
            n1 = W.decode_octa_rg(a[..., 0], a[..., 1])
            t1 = W.tilt_deg(n1)
            # the target is a ceiling, not a set point: decimating a noise field can already land
            # below it (then the strength factor stays 1.0 and the map is simply smoother)
            self.assertLessEqual(float(np.median(t1)), 14.0)
            self.assertLess(float(np.median(t1)), float(np.median(t0)) / 3.0)
            self.assertLess(float(np.percentile(t1, 95)), 25.0)
            grad = lambda m: 0.5 * (np.abs(np.diff(m, 1, axis=1)).mean() +
                                    np.abs(np.diff(m, 1, axis=0)).mean())
            self.assertLess(grad(t1), grad(t0) / 3.0)
            # the frame layout the USD relies on (sprite_sheet_cols = 20) must survive
            self.assertEqual(w // FRAMES, FW // 4)
            dds = rb(dst)
            chain = mip_chain(w, h)
            self.assertEqual(struct.unpack_from("<I", dds, 28)[0], len(chain))
            self.assertEqual(len(dds), 128 + sum(a * b * 4 for a, b in chain))

    def test_rejects_non_bc5(self):
        with tempfile.TemporaryDirectory() as d:
            src, dst = os.path.join(d, "a.dds"), os.path.join(d, "b.dds")
            open(src, "wb").write(b"DDS " + struct.pack("<I", 124) + b"\0" * 120 + b"\0" * 64)
            r = subprocess.run([sys.executable, os.path.join(REPO, "tools", "water_normal_soft.py"),
                                src, dst], capture_output=True, text=True)
            self.assertEqual(r.returncode, 2)
            self.assertIn("expected 83", r.stderr)


class TestWaterSh(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.wdir = os.path.join(self.tmp.name, "rtx-remix", "mods", "gameReadyAssets",
                                 "texture", "Water")
        os.makedirs(self.wdir)
        # the fake "pack" material: our realistic variant stands in for it (same prim paths)
        mat = os.path.join(REPO, "config", "water", "realistic.usda")
        for name in ("Water_materials.usda", "Water_materials.usda.orig"):
            with open(os.path.join(self.wdir, name), "wb") as fh:
                fh.write(rb(mat))
        R, G = synthetic_planes()
        bc5_dds(os.path.join(self.wdir, "water_normal.dds"), R, G)
        self.orig = rb(os.path.join(self.wdir, "Water_materials.usda"))

    def tearDown(self):
        self.tmp.cleanup()

    def sh(self, action, dry=False):
        argv = ["bash", os.path.join(REPO, "tools", "water.sh"), action, self.tmp.name]
        if dry:
            argv.append("--dry-run")
        return subprocess.run(argv, capture_output=True, text=True)

    def test_soft_then_pack_round_trip(self):
        r = self.sh("soft")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertTrue(os.path.exists(os.path.join(self.wdir, "water_normal_soft.dds")))
        cur = rb(os.path.join(self.wdir, "Water_materials.usda"))
        self.assertIn(b"water_normal_soft.dds", cur)
        self.assertNotEqual(cur, self.orig)
        s = self.sh("status")
        self.assertIn("soft (s32: regenerated normal sheet)", s.stdout)
        r = self.sh("pack")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertEqual(rb(os.path.join(self.wdir, "Water_materials.usda")), self.orig)
        self.assertIn("pack (the GTASA-Remix pack's own material)", self.sh("status").stdout)

    def test_soft_is_idempotent_and_dry_run_writes_nothing(self):
        r = self.sh("soft")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        stamp = os.stat(os.path.join(self.wdir, "water_normal_soft.dds")).st_mtime_ns
        r2 = self.sh("soft")
        self.assertEqual(r2.returncode, 0, r2.stdout + r2.stderr)
        self.assertIn("already there", r2.stdout)
        self.assertEqual(os.stat(os.path.join(self.wdir, "water_normal_soft.dds")).st_mtime_ns,
                         stamp)
        before = sorted(os.listdir(self.wdir))
        d = self.sh("realistic", dry=True)
        self.assertEqual(d.returncode, 0, d.stdout + d.stderr)
        self.assertEqual(sorted(os.listdir(self.wdir)), before)
        self.assertIn("+ cp", d.stdout)

    def test_dry_run_of_soft_writes_nothing(self):
        d = self.sh("soft", dry=True)
        self.assertEqual(d.returncode, 0, d.stdout + d.stderr)
        self.assertNotIn("water_normal_soft.dds", os.listdir(self.wdir))
        self.assertEqual(rb(os.path.join(self.wdir, "Water_materials.usda")), self.orig)

    def test_unknown_variant_exits_2(self):
        r = self.sh("bogus")
        self.assertEqual(r.returncode, 2)
        self.assertIn("usage:", r.stdout + r.stderr)


if __name__ == "__main__":
    unittest.main(verbosity=2)
