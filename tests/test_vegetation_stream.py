#!/usr/bin/env python3
"""n21 — tests for tools/vegetation_stream.py (what a vegetation pack costs the streaming system).

Everything runs on fixtures under a temp dir: a hand-built VER2 .img, two IDE files, an
ImprovedStreaming.ini with the same shape as the live one, and a pack folder of loose files.
The numbers are asserted exactly, because the whole point of the tool is that docs/VEGETATION.md
cause 2 can be re-argued on numbers instead of memory.
"""
import json
import os
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools"))
import vegetation_stream as S  # noqa: E402

REPO = os.path.join(os.path.dirname(__file__), "..")
MB = 1024 * 1024


def write_img(path, entries):
    """entries: [(name, bytes)] -> a VER2 archive with 2048-byte sectors, uncompressed."""
    n = len(entries)
    header = struct.pack("<4sI", b"VER2", n)
    sector_of = []
    off = ((len(header) + 32 * n) + 2047) // 2048
    first = off
    for _nm, data in entries:
        sector_of.append(off)
        off += (len(data) + 2047) // 2048
    dirblob = b""
    for (nm, data), so in zip(entries, sector_of):
        nsec = (len(data) + 2047) // 2048
        dirblob += struct.pack("<IHH24s", so, nsec, nsec, nm.encode()[:24].ljust(24, b"\0"))
    body = b""
    for (nm, data), so in zip(entries, sector_of):
        pad = b"\0" * (((len(data) + 2047) // 2048) * 2048 - len(data))
        body += data + pad
    head_pad = b"\0" * (first * 2048 - len(header) - len(dirblob))
    with open(path, "wb") as f:
        f.write(header + dirblob + head_pad + body)
    return n


class TestParsers(unittest.TestCase):
    def test_img_entries(self):
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "gta3.img")
            write_img(p, [("tree_a.dff", b"x" * 3000), ("lod_veg.txd", b"y" * 2048)])
            e = S.img_entries(p)
            self.assertEqual(sorted(e), ["lod_veg.txd", "tree_a.dff"])
            self.assertEqual(e["tree_a.dff"][1], 4096)      # 3000 bytes -> 2 sectors, sector-rounded
            self.assertEqual(e["lod_veg.txd"][1], 2048)

    def test_img_entries_rejects_other_magics(self):
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "x.img")
            open(p, "wb").write(b"COMP" + b"\0" * 100)
            self.assertEqual(S.img_entries(p), {})

    def test_ide_rows_skip_comments_and_headers(self):
        rows = S.ide_rows("""; vegepart
deftype 1000 tree 1
615, Elmtree_hism, tree1, 150, 2097156
784, lod_redwoodgrp, lod_veg_firs, 600, 2097284   ; the typo
""")
        self.assertEqual(rows, [(615, "elmtree_hism", "tree1"), (784, "lod_redwoodgrp", "lod_veg_firs")])

    def test_ranges_are_section_scoped(self):
        rs = S.ranges_from_ini("""
[Settings]
Start = 1
End = 2
[Range4]
Start = 615
End = 792
Enabled = 1
KeepLoaded = 1
[Range5]
Start = 800
End = 906
Enabled = 0
""")
        self.assertEqual(len(rs), 2)
        self.assertEqual((rs[0]["start"], rs[0]["end"], rs[0]["enabled"], rs[0]["keep_loaded"]),
                         (615, 792, 1, 1))
        self.assertEqual(rs[1]["enabled"], 0)

    def test_ini_key_section_and_comments(self):
        text = "[System]\nStreamMemory = 1900   ; raised by the installer\n"
        self.assertEqual(S.ini_key(text, "streammemory"), "1900")
        self.assertEqual(S.ini_key(text, "StreamMemory", "Other"), None)
        self.assertEqual(S.ini_key(text, "Absent"), None)


class TestFootprint(unittest.TestCase):
    def setUp(self):
        self.d = tempfile.mkdtemp()
        self.game = os.path.join(self.d, "game")
        os.makedirs(os.path.join(self.game, "models"))
        os.makedirs(os.path.join(self.game, "data/maps/generic"))
        os.makedirs(os.path.join(self.game, "modloader/Improved Streaming"))
        os.makedirs(os.path.join(self.game, "modloader/MixSets"))
        write_img(os.path.join(self.game, "models/gta3.img"), [
            ("tree_a.dff", b"\0" * (1 * MB)),
            ("tree_b.dff", b"\0" * (1 * MB)),
            ("lod_a.dff", b"\0" * (1024 * 1024 // 2)),
            ("lod_veg.txd", b"\0" * (2 * MB)),
        ])
        with open(os.path.join(self.game, "data/maps/generic/vegepart.ide"), "w") as f:
            f.write("615, tree_a, lod_veg, 150, 0\n616, tree_b, lod_veg, 150, 0\n"
                    "617, lod_a, lod_veg, 600, 0\n")
        open(os.path.join(self.game, "data/maps/generic/procobj.ide"), "w").write("\n")
        with open(os.path.join(self.game, "modloader/Improved Streaming/ImprovedStreaming.ini"), "w") as f:
            f.write("[Settings]\nStreamMemoryForced = 1024\nRemoveUnusedWhenPercent = 80\n"
                    "[Range4]\nStart = 615\nEnd = 617\nEnabled = 1\nKeepLoaded = 1\n"
                    "[Range5]\nStart = 900\nEnd = 906\nEnabled = 0\n")
        with open(os.path.join(self.game, "modloader/MixSets/MixSets.ini"), "w") as f:
            f.write("[System]\nStreamMemory = 1900\n")
        self.pack = os.path.join(self.d, "pack/V Vegetation Mix")
        os.makedirs(os.path.join(self.pack, "gta3.img"))
        for nm, mb in (("tree_a.dff", 20), ("tree_b.dff", 10), ("lod_a.dff", 5),
                       ("new_dict.txd", 30)):
            open(os.path.join(self.pack, "gta3.img", nm), "wb").write(b"\0" * (mb * MB))

    def tearDown(self):
        import shutil
        shutil.rmtree(self.d, ignore_errors=True)

    def run_tool(self, *extra):
        cmd = [sys.executable, os.path.join(REPO, "tools", "vegetation_stream.py"), self.game] + list(extra)
        import subprocess
        p = subprocess.run(cmd, capture_output=True, text=True)
        return p

    def test_pinned_footprint_vanilla_and_pack(self):
        out = os.path.join(self.d, "r.json")
        p = self.run_tool("--pack", self.pack, "--quiet", "--out-json", out)
        self.assertEqual(p.returncode, 0, p.stderr)
        r = json.load(open(out))
        # vanilla: 3 .dff (1+1+0.5 MB) + lod_veg.txd (2 MB) = 4.5 MB
        self.assertAlmostEqual(r["pinned_vanilla_mb"], 4.5, places=2)
        # pack: 20 + 10 + 5 MB of .dff + the same 2 MB dictionary = 37 MB
        self.assertAlmostEqual(r["pinned_pack_mb"], 37.0, places=2)
        self.assertEqual(r["cap_mb"], 1900)          # max(1900, 1024)
        self.assertEqual(r["address_space_mb"], 2048)  # no exe -> not LAA

    def test_disabled_range_is_not_priced(self):
        out = os.path.join(self.d, "r2.json")
        ini = os.path.join(self.game, "modloader/Improved Streaming/ImprovedStreaming.ini")
        txt = open(ini).read().replace("[Range4]\nStart = 615\nEnd = 617\nEnabled = 1",
                                       "[Range4]\nStart = 615\nEnd = 617\nEnabled = 0")
        open(ini, "w").write(txt)
        p = self.run_tool("--pack", self.pack, "--quiet", "--out-json", out)
        self.assertEqual(p.returncode, 0, p.stderr)
        r = json.load(open(out))
        self.assertEqual(r["pinned_pack_mb"], 0.0)
        self.assertEqual(r["ranges"], [])

    def test_pack_totals_and_new_names(self):
        out = os.path.join(self.d, "r3.json")
        self.run_tool("--pack", self.pack, "--quiet", "--out-json", out)
        r = json.load(open(out))
        self.assertEqual(r["pack_replaces"], 3)             # tree_a, tree_b, lod_a
        self.assertEqual(r["pack_new"], ["new_dict.txd"])   # nothing provides it today
        self.assertAlmostEqual(r["pack_total_mb_pack"], 65.0, places=2)

    def test_no_pack_means_vanilla_only(self):
        out = os.path.join(self.d, "r4.json")
        self.run_tool("--quiet", "--out-json", out)
        r = json.load(open(out))
        self.assertAlmostEqual(r["pinned_vanilla_mb"], 4.5, places=2)
        self.assertAlmostEqual(r["pinned_pack_mb"], 4.5, places=2)

    def test_human_output_names_the_verdict(self):
        p = self.run_tool("--pack", self.pack)
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertIn("pinned by KeepLoaded=1: 4.5 MB vanilla -> 37.0 MB with the pack", p.stdout)
        self.assertIn("VERDICT", p.stdout)


if __name__ == "__main__":
    unittest.main()
