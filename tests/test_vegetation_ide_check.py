#!/usr/bin/env python3
"""n21 — tests for tools/vegetation_ide_check.py (does a pack's IDE data resolve?).

The real defect this tool exists for is one letter: the staged V Vegetation Mix names
`lod_veg_firs` for vegepart ids 784-788 and ships `lod_veg_fist.txd`. The fixtures below
rebuild that shape (plus a dictionary that is genuinely absent, so a near-miss is not
invented), and check the exit codes and the --emit-shims / --exclude behaviour that
tools/vegetation.sh step 5 drives.
"""
import os
import struct
import subprocess
import sys
import tempfile
import unittest

REPO = os.path.join(os.path.dirname(__file__), "..")
TOOL = os.path.join(REPO, "tools", "vegetation_ide_check.py")


def write_img(path, names):
    n = len(names)
    header = struct.pack("<4sI", b"VER2", n)
    off = ((len(header) + 32 * n) + 2047) // 2048
    blob = b""
    for nm in names:
        blob += struct.pack("<IHH24s", off, 1, 1, nm.encode()[:24].ljust(24, b"\0"))
    body = b"\0" * (2048 * len(names))
    with open(path, "wb") as f:
        f.write(header + blob + b"\0" * (off * 2048 - len(header) - len(blob)) + body)


class Test(unittest.TestCase):
    def setUp(self):
        self.d = tempfile.mkdtemp()
        self.pack = os.path.join(self.d, "pack/V Vegetation Mix")
        os.makedirs(os.path.join(self.pack, "gta3.img"))
        os.makedirs(os.path.join(self.pack, "data/maps/generic"))
        for nm in ("lod_veg_fist.txd", "lod_veg.txd", "bsor_tree.txd", "tree_a.dff"):
            open(os.path.join(self.pack, "gta3.img", nm), "wb").write(b"\0" * 16)
        with open(os.path.join(self.pack, "data/maps/generic/vegepart.ide"), "w") as f:
            f.write("; vegepart\n"
                    "783, lod_vegvbbigbrn, lod_veg, 600, 2097284\n"
                    "784, lod_redwoodgrp, lod_veg_firs, 600, 2097284\n"
                    "790, sm_fir_tallgroup, insanity_pines, 299, 2138116\n")
        with open(os.path.join(self.pack, "data/maps/generic/procobj.ide"), "w") as f:
            f.write("800, tree_a, bsor_tree, 150, 0\n")
        self.game = os.path.join(self.d, "game")
        os.makedirs(os.path.join(self.game, "models"))
        os.makedirs(os.path.join(self.game, "modloader/SARemix/gta3.img"))
        write_img(os.path.join(self.game, "models/gta3.img"),
                  ["tree1.txd", "gta_tree_pine.txd", "some_model.dff"])
        open(os.path.join(self.game, "modloader/SARemix/gta3.img/insanity_pines.txd"), "wb").write(b"\0")

    def tearDown(self):
        import shutil
        shutil.rmtree(self.d, ignore_errors=True)

    def run_tool(self, *extra):
        return subprocess.run([sys.executable, TOOL, self.pack] + list(extra),
                              capture_output=True, text=True)

    def test_finds_the_one_letter_typo_and_suggests_the_pair(self):
        p = self.run_tool("--game", self.game, "--emit-shims")
        self.assertEqual(p.returncode, 1)                 # something is missing
        self.assertIn("lod_veg_firs.txd", p.stdout)
        self.assertIn("closest provided: lod_veg_fist", p.stdout)
        self.assertIn("lod_veg_firs.txd=lod_veg_fist.txd", p.stdout)
        self.assertIn("id 784", p.stdout)

    def test_dictionaries_provided_by_the_game_or_another_mod_are_not_reported(self):
        p = self.run_tool("--game", self.game)
        # insanity_pines.txd comes from modloader/SARemix/, so it must not be listed as missing;
        # the only name left is the pack's own typo.
        self.assertNotIn("insanity_pines.txd", p.stdout)
        self.assertIn("MISSING 1 dictionary name(s)", p.stdout)

    def test_without_the_game_only_the_pack_counts(self):
        p = self.run_tool("--emit-shims")
        self.assertIn("MISSING 2 dictionary name(s)", p.stdout)   # lod_veg_firs + insanity_pines
        self.assertIn("lod_veg_firs.txd=lod_veg_fist.txd", p.stdout)
        self.assertNotIn("insanity_pines.txd=insanity_pines", p.stdout)

    def test_a_far_name_gets_no_shim(self):
        # insanity_pines has no close provided name -> reported, but never shimmed
        p = self.run_tool("--emit-shims")
        self.assertIn("insanity_pines.txd", p.stdout)
        self.assertNotIn("insanity_pines.txd=", p.stdout)

    def test_ok_when_everything_resolves(self):
        open(os.path.join(self.pack, "gta3.img/lod_veg_firs.txd"), "wb").write(b"\0")
        open(os.path.join(self.pack, "gta3.img/insanity_pines.txd"), "wb").write(b"\0")
        p = self.run_tool("--game", self.game)
        self.assertEqual(p.returncode, 0)
        self.assertIn("OK: every dictionary", p.stdout)

    def test_exclude_stops_our_own_install_from_masking_the_defect(self):
        dest = os.path.join(self.d, "game/modloader/V Vegetation Mix")
        os.makedirs(os.path.join(dest, "gta3.img"))
        open(os.path.join(dest, "gta3.img/lod_veg_firs.txd"), "wb").write(b"\0")
        p = self.run_tool("--game", self.game)
        self.assertEqual(p.returncode, 0)                 # the shim we wrote makes it resolve
        p = self.run_tool("--game", self.game, "--exclude", dest)
        self.assertEqual(p.returncode, 1)                 # but the pack itself is still broken
        self.assertIn("lod_veg_firs.txd", p.stdout)

    def test_no_ide_files_is_a_usage_error(self):
        empty = os.path.join(self.d, "empty")
        os.makedirs(empty)
        p = subprocess.run([sys.executable, TOOL, empty], capture_output=True, text=True)
        self.assertEqual(p.returncode, 2)

    def test_missing_pack_is_a_usage_error(self):
        p = subprocess.run([sys.executable, TOOL, os.path.join(self.d, "nope")],
                           capture_output=True, text=True)
        self.assertEqual(p.returncode, 2)


if __name__ == "__main__":
    unittest.main()
