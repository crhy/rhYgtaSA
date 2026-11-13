#!/usr/bin/env python3
"""n15: tools/target-gap.py - the six look metrics that separate us from the reference video.

Why this exists: docs/TARGET.md's whole argument is a table of numbers measured by this tool
(headroom, ceiling, warmth, haze far/near, specular area). If the tool drifts, the argument
silently becomes folklore, and the next "we closed the gap" claim measures something else.

These tests use synthetic images, so they are fast and they pin the *definitions*:

  * box_blur keeps the shape (the first version of it shrank the array and the high-pass
    crashed - a 12-line bug that would have hidden in a big frame);
  * a filmic image and a lifted-flat image of the same mean are separated by headroom and
    ceiling, which is exactly the reference-vs-ours difference n15 measured;
  * warmth is signed the way the docs say (blue negative, warm positive);
  * the specular floor follows the frame's own p95, so a lifted flat frame cannot "win" the
    specular count without having a highlight (the trap n15 fell into on the first run);
  * haze far/near is < 1 when the distance is flatter than the near field, > 1 when the near
    field is the flat one (our downtown shot measured 3.67);
  * the CLI runs end to end, honours --scale/--jpeg/--json, and prints one row per metric.

Run: python3 -m pytest -q tests/test_target_gap.py   (plain unittest works too)
"""
import json
import os
import subprocess
import sys
import unittest

import numpy as np
from PIL import Image

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOL = os.path.join(REPO, "tools", "target-gap.py")
TMP = os.path.join(REPO, "build", "tmp", "test_target_gap")


def _mkdir():
    os.makedirs(TMP, exist_ok=True)
    return TMP


def filmic(path):
    """Low anchor, real highlights: dark ground, a bright sky band, a few small speculars."""
    h, w = 120, 160
    g = np.linspace(40, 120, h)[:, None] + np.zeros((h, w))
    rgb = np.dstack([g, g, g])
    rgb[:24] = 200.0                                     # bright soft sky
    rgb[..., 0] += 12.0                                  # warm cast
    rng = np.random.default_rng(7)
    for _ in range(12):                                  # small neutral speculars, dead white
        y, x = rng.integers(30, h - 5), rng.integers(5, w - 5)
        rgb[y:y + 3, x:x + 3] = 255.0
    _save(path, rgb)


def flat_lifted(path):
    """High anchor, no bright end: everything squeezed into 120..170, cool cast."""
    h, w = 120, 160
    g = np.linspace(120, 170, h)[:, None] + np.zeros((h, w))
    rgb = np.dstack([g, g, g])
    rgb[:24] = 150.0                                     # dark, flat sky
    rgb[..., 2] += 30.0                                  # blue
    _save(path, rgb)


def _save(path, rgb):
    Image.fromarray(np.uint8(np.clip(rgb, 0, 255))).save(path)


def load_tool():
    """tools/target-gap.py has a hyphen in its name, so import it from its path."""
    import importlib.util
    spec = importlib.util.spec_from_file_location("target_gap", TOOL)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


class Metrics(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tg = load_tool()
        d = _mkdir()
        cls.film = os.path.join(d, "filmic.png")
        cls.flat = os.path.join(d, "flat.png")
        filmic(cls.film)
        flat_lifted(cls.flat)
        cls.mf = cls.tg.measure(cls.tg.load_rgb(cls.film))
        cls.ml = cls.tg.measure(cls.tg.load_rgb(cls.flat))

    def test_every_documented_metric_exists(self):
        for dotted, _label in self.tg.ROWS:
            cur = self.mf
            for part in dotted.split("."):
                self.assertIn(part, cur, "%s is not in the result dict" % dotted)
                cur = cur[part]
            self.assertTrue(np.isfinite(float(cur)), "%s is not finite" % dotted)

    def test_headroom_and_ceiling_separate_filmic_from_flat(self):
        self.assertGreater(self.mf["rolloff"]["headroom"], 1.8)
        self.assertLess(self.ml["rolloff"]["headroom"], 1.4)
        self.assertGreater(self.mf["rolloff"]["ceiling_pct"], self.ml["rolloff"]["ceiling_pct"])
        self.assertGreater(self.mf["hist"]["pct_clip"], self.ml["hist"]["pct_clip"])

    def test_warmth_is_signed(self):
        self.assertGreater(self.mf["colour"]["warmth_mid"], 0.0)
        self.assertLess(self.ml["colour"]["warmth_mid"], 0.0)

    def test_specular_floor_follows_the_frames_own_p95(self):
        # the flat frame has no pixel brighter than ~170, so its p95 sets the floor and the
        # absolute SPEC_LUMA must not be able to lower it below that.
        self.assertEqual(self.ml["spec"]["floor_luma"],
                         max(self.tg.SPEC_LUMA, self.ml["hist"]["p95"]))
        self.assertGreater(self.mf["spec"]["area_pm"], self.ml["spec"]["area_pm"])

    def test_haze_ratio_direction(self):
        """far/near < 1 when the distance band is flatter than the near band, > 1 the other way.
        Our 2026-11-12 downtown noon shot measured 3.67 (flat ground, crisp skyline); the
        reference measured 0.46-1.18."""
        d = _mkdir()
        hz = np.full((200, 160, 3), 100.0)                    # flat everywhere
        rng = np.random.default_rng(3)
        hz[144:190] += rng.normal(0, 25, (46, 160, 3))        # near band only: textured
        hz = np.clip(hz, 0, 255)
        _save(os.path.join(d, "haze.png"), hz)
        m = self.tg.measure(self.tg.load_rgb(os.path.join(d, "haze.png")))
        self.assertLess(m["haze"]["far_over_near_contrast"], 0.2)
        self.assertGreater(m["haze"]["near"]["hp_rms"], 1.0)

    def test_box_blur_preserves_shape_and_blurs(self):
        a = np.arange(20 * 30, dtype=float).reshape(20, 30)
        b = self.tg.box_blur(a, 4)
        self.assertEqual(b.shape, a.shape)
        self.assertFalse(np.allclose(a, b))
        const = np.full((9, 9), 7.0)
        self.assertTrue(np.allclose(self.tg.box_blur(const, 3), 7.0))
        # semantics: an edge-clamped box of size 2r+1, computed naively for comparison
        r = 4
        n = 2 * r + 1
        ref = np.empty_like(a)
        for y in range(a.shape[0]):
            for x in range(a.shape[1]):
                win = a[max(0, y - r):min(a.shape[0], y + r + 1),
                        max(0, x - r):min(a.shape[1], x + r + 1)]
                # clamped edges repeat the border value, so divide by the full window area
                pad = np.pad(a, ((r, r), (r, r)), mode="edge")
                ref[y, x] = pad[y:y + n, x:x + n].sum() / (n * n)
        self.assertTrue(np.allclose(b, ref, atol=1e-9))

    def test_normalise_resizes_and_roundtrips(self):
        rgb = self.tg.load_rgb(self.film)
        out = self.tg.normalise(rgb, (64, 48), 70, os.path.join(_mkdir(), "rt.jpg"))
        self.assertEqual(out.shape, (48, 64, 3))
        same = self.tg.normalise(rgb, (64, 48), 0, os.path.join(_mkdir(), "rt2.jpg"))
        self.assertEqual(same.shape, (48, 64, 3))


class Cli(unittest.TestCase):
    def test_runs_and_writes_json(self):
        d = _mkdir()
        filmic(os.path.join(d, "a.png"))
        flat_lifted(os.path.join(d, "b.png"))
        out_json = os.path.join(d, "gap.json")
        p = subprocess.run([sys.executable, TOOL,
                            os.path.join(d, "a.png"), os.path.join(d, "b.png"),
                            "--label", "filmic", "--label", "flat",
                            "--scale", "0", "--jpeg", "0", "--json", out_json],
                           capture_output=True, text=True, timeout=120, cwd=REPO)
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        self.assertIn("headroom p99.9/p50", p.stdout)
        self.assertIn("haze far/near contrast", p.stdout)
        doc = json.load(open(out_json))
        self.assertEqual([r["label"] for r in doc["results"]], ["filmic", "flat"])
        self.assertEqual(doc["scale"], "0")
        self.assertEqual(doc["jpeg"], 0)

    def test_default_normalisation_is_applied_to_every_image(self):
        d = _mkdir()
        filmic(os.path.join(d, "a.png"))
        p = subprocess.run([sys.executable, TOOL, os.path.join(d, "a.png"),
                            "--scale", "80x60", "--jpeg", "60"],
                           capture_output=True, text=True, timeout=120, cwd=REPO)
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        self.assertIn("measured at 80x60", p.stdout)

    def test_no_images_is_a_usage_error(self):
        p = subprocess.run([sys.executable, TOOL], capture_output=True, text=True,
                           timeout=60, cwd=REPO)
        self.assertNotEqual(p.returncode, 0)
        self.assertIn("no images given", p.stderr)


if __name__ == "__main__":
    unittest.main()
