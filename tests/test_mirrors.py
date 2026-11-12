#!/usr/bin/env python3
"""Tests for tools/mirrors.py — the s21 rule that a hash needs `# verified-mirror-only`.

Why this test exists: s18 shipped `xvehicleenv128` and `vehicleenvmap128` as "the mirror textures".
s21 measured every stock vehicle .dff and found they are the environment/reflection map of the
whole car (4197 materials on 179 of 209 models, and 495 on 81), never a diffuse texture, while the
mirror glass itself uses `vehiclelights128` — the shared light-lens atlas. A `mat_<HASH>` override
for either would have re-materialised every painted panel. The tool therefore only accepts a hash
whose line carries the marker, and the shipped config carries no hash at all.

Everything runs against a fake game tree under a temp dir; the real game folder is never opened.
"""
import os
import shutil
import sys
import tempfile
import unittest

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
sys.path.insert(0, os.path.join(REPO, "tools"))
import mirrors as M  # noqa: E402

MOD_USDA = '''#usda 1.0
(
    subLayers = [
        @./SARemix.usda@
    ]
)
'''


def fake_game(root):
    """the minimum of rtx-remix/mods/gameReadyAssets mirrors.py insists on seeing"""
    assets = os.path.join(root, "rtx-remix", "mods", "gameReadyAssets")
    os.makedirs(os.path.join(assets, "materials"))
    with open(os.path.join(assets, "mod.usda"), "w", encoding="utf-8") as f:
        f.write(MOD_USDA)
    with open(os.path.join(assets, "materials", "AperturePBR_Opacity.mdl"), "w", encoding="utf-8") as f:
        f.write("# fake: mirrors.py only checks that this exists\n")
    return assets


class TestMarker(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="mirrors-selftest-")
        self.assets = fake_game(self.tmp)

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def config(self, text):
        path = os.path.join(self.tmp, "hashes.txt")
        with open(path, "w", encoding="utf-8") as f:
            f.write(text)
        return path

    # --- read_hashes: the marker rule -------------------------------------------------
    def test_unmarked_hash_is_refused(self):
        """the exact line s18 shipped must no longer be usable"""
        cfg = self.config("0x45DF1EA60BB202B5   # xvehicleenv128 (128x128) - mirror glass\n")
        self.assertEqual(M.read_hashes(cfg, []), [])

    def test_marked_hash_is_accepted_and_label_kept(self):
        cfg = self.config("0x45DF1EA60BB202B5   # verified-mirror-only  mirror glass, nothing else\n")
        entries = M.read_hashes(cfg, [])
        self.assertEqual([h for h, _ in entries], ["45DF1EA60BB202B5"])
        self.assertEqual(entries[0][1], "mirror glass, nothing else")

    def test_marker_is_case_insensitive_and_comment_may_lead(self):
        cfg = self.config("0x6BAFC5F6AEE0A6B5   # Verified-Mirror-Only after a note\n"
                          "# a comment line 0x1111111111111111 is not a hash line at all\n")
        self.assertEqual([h for h, _ in M.read_hashes(cfg, [])], ["6BAFC5F6AEE0A6B5"])

    def test_mixed_file_keeps_only_verified_lines(self):
        cfg = self.config("0x45DF1EA60BB202B5   # xvehicleenv128 - whole-car env map, NOT mirror-only\n"
                          "0xABCDEF0123456789   # verified-mirror-only  re-authored mirror mesh texture\n")
        self.assertEqual([h for h, _ in M.read_hashes(cfg, [])], ["ABCDEF0123456789"])

    def test_hash_flag_needs_marker_too(self):
        self.assertEqual(M.read_hashes(None, ["0x45DF1EA60BB202B5"]), [])
        self.assertEqual([h for h, _ in M.read_hashes(None, ["0x45DF1EA60BB202B5"],
                                                     extra_verified=True)],
                         ["45DF1EA60BB202B5"])
        self.assertEqual([h for h, _ in M.read_hashes(
            None, ["0x45DF1EA60BB202B5 # verified-mirror-only checked in game"])],
            ["45DF1EA60BB202B5"])

    # --- the shipped state ------------------------------------------------------------
    def test_shipped_config_carries_no_hash(self):
        """s21's conclusion: no GTA SA texture is mirror-only, so the list is empty on purpose."""
        cfg = os.path.join(REPO, "config", "mirror-hashes.txt")
        self.assertEqual(M.read_hashes(cfg, []), [])
        text = open(cfg, encoding="utf-8").read()
        self.assertIn(M.MARKER, text)
        for bad in ("45DF1EA60BB202B5", "6BAFC5F6AEE0A6B5"):
            self.assertNotIn(bad, [line.split("#")[0].strip()
                                   for line in text.splitlines()
                                   if line.strip() and not line.startswith("#")])

    # --- end to end, fake tree --------------------------------------------------------
    def test_dry_run_with_a_verified_hash_writes_one_override(self):
        cfg = self.config("0xABCDEF0123456789   # verified-mirror-only  test texture\n")
        argv = [sys.argv[0], self.tmp, "--config", cfg, "--dry-run"]
        old, sys.argv = sys.argv, argv
        try:
            self.assertEqual(M.main(), 0)
        finally:
            sys.argv = old
        self.assertFalse(os.path.exists(os.path.join(self.assets, M.LAYER)))
        self.assertEqual(open(os.path.join(self.assets, "mod.usda"), encoding="utf-8").read(),
                         MOD_USDA)

    def test_dry_run_with_the_shipped_config_refuses(self):
        argv = [sys.argv[0], self.tmp, "--dry-run"]
        old, sys.argv = sys.argv, argv
        try:
            with self.assertRaises(SystemExit) as ctx:
                M.main()
        finally:
            sys.argv = old
        self.assertIn(M.MARKER, str(ctx.exception))

    def test_generated_layer_only_uses_verified_hashes(self):
        cfg = self.config("0xABCDEF0123456789   # verified-mirror-only  test texture\n"
                          "0x45DF1EA60BB202B5   # xvehicleenv128\n")
        entries = M.read_hashes(cfg, [])
        text = M.render(entries, "0.02")
        M.validate(text, "layer", need_mat=True)
        self.assertIn('over "mat_ABCDEF0123456789"', text)
        self.assertNotIn("45DF1EA60BB202B5", text)


if __name__ == "__main__":
    unittest.main()
