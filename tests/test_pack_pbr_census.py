#!/usr/bin/env python3
"""tests for tools/pack_pbr_census.py (n18).

The census is the evidence behind `rtx.legacyMaterial.roughnessConstant = 0.65` in all three
profiles, so the counting has to be right without a 10 GB pack in the test fixture: this builds a
tiny fake pack under a temp dir and asserts the numbers it must produce.

What is pinned here, and why each one exists:

  * **hash and suffix parsing** - the pack names its replacement maps `<HASH>_<suffix>.dds`, also
    spells some suffixes out (`_roughness`, `_color`), and mixes in hand-named maps
    (`rail_base_albedo.dds`) that belong to its own geometry and must NOT be counted as coverage.
  * **the material scan reads bytes** - n15 counted 2334 materials with `grep -rhoE 'mat_...'`,
    which prints "binary file matches" instead of the matches for binary .usd crates. The fixture
    hides one material name inside a crate full of NULs; a scan that reads text would miss it, and
    the census would repeat n15's off-by-29.
  * **coverage** - covered means "a `<HASH>_albedo.dds` exists for that `mat_<HASH>`". Bare
    `<HASH>.dds` copies (778 of them in the real pack) are the game's own un-remastered textures and
    must be counted separately, not as coverage.
  * **case-insensitive layer resolution** - the pack says `texture/Asphalt/...` against an on-disk
    `texture/asphalt/` and the runtime loads it anyway (Proton), so the census reports that as a
    mismatch, not a missing file; a genuinely missing layer must still be reported as missing.
  * **the decode pass** - a `_rough` map is raw data, so texel/255 IS the perceptual roughness the
    renderer uses. A fake map with a known gradient must decode to exactly those numbers, or the
    shipped 0.65 would be defended by a broken measurement.
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, "tools"))
import pack_pbr_census as P  # noqa: E402

TOOL = os.path.join(REPO, "tools", "pack_pbr_census.py")

H1 = "07A2BD3EF6FA6ABE"       # covered: albedo + normal + rough + height + metal
H2 = "41AE107F814A307D"       # covered: albedo + rough (roughness spelled out on one map)
H3 = "1CB126F1AE5A3A08"       # named only inside a binary crate; no maps -> uncovered
H4 = "26626F3D0B9542FD"       # bare <HASH>.dds copy only -> uncovered


def write_dds(path, values):
    """An 8x8 single-channel DDS whose texels are `values` (row-major), written by Pillow."""
    from PIL import Image
    px = Image.new("L", (8, 8))
    px.putdata(values)
    px.save(path, "DDS")          # Pillow writes it uncompressed; its reader gives the same values
    return path


def make_pack(root):
    """The fake pack. Layout mirrors the real one: texture/<category>/, Materials.usda on top."""
    tex = os.path.join(root, "texture")
    os.makedirs(os.path.join(tex, "asphalt"))
    os.makedirs(os.path.join(tex, "ground"))
    os.makedirs(os.path.join(root, "meshes"))

    # asphalt: one fully remastered material. The rough map is a 0..255 ramp in row-major order,
    # so its mean is exactly 127.5 and its p05/p95 land on the ramp.
    a = os.path.join(tex, "asphalt")
    write_dds(os.path.join(a, "%s_albedo.dds" % H1), [10] * 64)
    write_dds(os.path.join(a, "%s_normal.dds" % H1), [128] * 64)
    write_dds(os.path.join(a, "%s_rough.dds" % H1), [v % 256 for v in range(64)])
    write_dds(os.path.join(a, "%s_height.dds" % H1), [v * 4 % 256 for v in range(64)])
    write_dds(os.path.join(a, "%s_metal.dds" % H1), [200] * 64)
    # a hand-named map of the pack's own geometry: must not count as coverage of anything
    write_dds(os.path.join(a, "rail_base_albedo.dds"), [7] * 64)

    # ground: covered, with one suffix spelled out, and a bare copy of a fourth hash
    g = os.path.join(tex, "ground")
    write_dds(os.path.join(g, "%s_albedo.dds" % H2), [20] * 64)
    write_dds(os.path.join(g, "%s_roughness.dds" % H2), [250] * 64)   # "_roughness" == rough
    write_dds(os.path.join(g, "%s.dds" % H4), [99] * 64)              # bare, un-remastered

    # the material layers. Note the reference to texture/Asphalt/... (capital A) against an on-disk
    # directory named asphalt: the runtime resolves that case-insensitively, we must too.
    with open(os.path.join(tex, "asphalt", "Asphalt_materials.usda"), "w") as fh:
        fh.write('#usda 1.0\ndef Scope {\n'
                 '  over "mat_%s" {\n'
                 '    inputs:inputs:diffuse_texture = @./%s_albedo.dds@\n'
                 '    outputs.outputs = outputs.material_scope_outputs.OMLSurface\n  }\n}\n'
                 % (H1, H1))
    with open(os.path.join(tex, "ground", "Ground_materials.usda"), "w") as fh:
        fh.write('#usda 1.0\ndef Scope { over "mat_%s" { '
                 'inputs:inputs:diffuse_texture = @./%s_albedo.dds@ } }\n' % (H2, H2))
    with open(os.path.join(root, "Materials.usda"), "w") as fh:
        fh.write('#usda 1.0\ndef Material "Materials" {\n'
                 '  over "Root" {\n'
                 '    def Material "M" {\n'
                 '      token inputs:materialReference = </Materials/Asphalt>\n'
                 '    }\n'
                 '  }\n'
                 '  subLayers = [@./texture/Asphalt/Asphalt_materials.usda@,\n'
                 '               @./texture/ground/Ground_materials.usda@,\n'
                 '               @./texture/Wall/Wall_materials.usda@]\n}\n')
    # a binary crate: NUL-padded bytes with a material name in the string table. `grep -r` without
    # -a reports "binary file matches" for this file and loses the name; the census must not.
    crate = os.path.join(root, "meshes", "prop.usd")
    with open(crate, "wb") as fh:
        fh.write(b"\x00\x00usd\x00" + b"\x00" * 200)
        fh.write(b"Looks\x00mat_" + H3.encode() + b"\x00mat_" + H4.encode()
                 + b"\x00" + b"\xff" * 128)
    return root


class Census(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not shutil.which("python3"):
            raise unittest.SkipTest("no python3")
        try:
            import numpy  # noqa: F401
            from PIL import Image  # noqa: F401
        except Exception:
            raise unittest.SkipTest("the decode pass needs numpy and Pillow")
        cls.root = tempfile.mkdtemp(prefix="n18-census-")
        make_pack(cls.root)
        cls.out = os.path.join(cls.root, "_out")
        os.makedirs(cls.out)
        cls.json_path = os.path.join(cls.out, "census.json")
        r = subprocess.run([sys.executable, TOOL, cls.root, "--sample", "1",
                            "--json", cls.json_path,
                            "--list-prefix", os.path.join(cls.out, "pack")],
                           capture_output=True, text=True)
        assert r.returncode == 0, r.stdout + r.stderr
        cls.stdout = r.stdout
        cls.data = json.load(open(cls.json_path))

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.root, ignore_errors=True)

    # --- parsing -----------------------------------------------------------------------------
    def test_suffix_and_hash_parsing(self):
        self.assertEqual(P.suffix_of("%s_rough.dds" % H1), "rough")
        self.assertEqual(P.suffix_of("%s_roughness.dds" % H2), "rough")     # spelled out
        self.assertEqual(P.suffix_of("Wood_002H_WhitePaint_rough.dds"), "rough")
        self.assertIsNone(P.suffix_of("rail_base.dds"))
        self.assertEqual(P.hash_of("%s_albedo.dds" % H1), H1)
        self.assertEqual(P.hash_of("%s_roughness.dds" % H2), H2)
        self.assertEqual(P.hash_of("%s.dds" % H4), H4)                      # bare copy still keyed
        self.assertIsNone(P.hash_of("rail_base_albedo.dds"))                # hand-named
        self.assertIsNone(P.hash_of("Wood_002H_WhitePaint_rough.dds"))      # hand-named
        self.assertIsNone(P.hash_of("Asphalt_01H_rough.dds"))               # not a hash

    def test_case_insensitive_resolution(self):
        # the pack's own case trap: Asphalt on paper, asphalt on disk
        hit = P.resolve_case_insensitive(self.root, os.path.join(self.root, "texture/Asphalt"))
        self.assertIsNotNone(hit)
        self.assertTrue(hit.endswith(os.path.join("texture", "asphalt")), hit)
        # and a path that is really absent stays absent
        self.assertIsNone(P.resolve_case_insensitive(self.root,
                              os.path.join(self.root, "texture/Nope/Nope_materials.usda")))

    # --- census and coverage ------------------------------------------------------------------
    def test_material_scan_reads_binary_crates(self):
        """H3/H4 exist only inside a NUL-filled .usd; a text-mode scan loses them (n15's off-by-29)."""
        self.assertEqual(self.data["materials"], 4, self.stdout)
        unc = open(os.path.join(self.out, "pack-uncovered.txt")).read()
        self.assertIn(H3, unc)
        self.assertIn(H4, unc)

    def test_coverage_counts(self):
        self.assertEqual(self.data["covered"], 2)          # H1, H2
        self.assertEqual(self.data["uncovered"], 2)        # H3 (no maps), H4 (bare copy only)
        self.assertEqual(self.data["map_hashes_with_albedo"], 2)
        self.assertEqual(self.data["bare_copies"], 1)      # H4
        self.assertEqual(self.data["hand_named"], 1)       # rail_base_albedo
        self.assertEqual(self.data["maps"]["rough"], 2)    # _rough + _roughness
        self.assertEqual(self.data["maps"]["metal"], 1)
        self.assertEqual(self.data["maps_by_hash"]["metal"], 1)

    def test_layer_case_mismatch_is_not_reported_as_missing(self):
        self.assertEqual(self.data["case_mismatch"], 1, self.stdout)   # texture/Asphalt/...
        self.assertEqual(self.data["missing_layers"], 1, self.stdout)  # texture/Wall/...

    # --- decode ------------------------------------------------------------------------------
    def test_roughness_decode_is_the_raw_value(self):
        """The asphalt rough map is the ramp 0..63: mean 31.5/255, and the value IS the roughness."""
        self.assertEqual(self.data["rough"]["n"], 2)
        maps = {os.path.basename(m["path"]): m for m in self.data["rough"]["maps"]}
        ramp = maps["%s_rough.dds" % H1]
        self.assertAlmostEqual(ramp["mean"] / 255.0, 31.5 / 255.0, places=4)
        self.assertAlmostEqual(ramp["min"] / 255.0, 0.0, places=4)
        self.assertAlmostEqual(ramp["max"] / 255.0, 63.0 / 255.0, places=4)
        flat = maps["%s_roughness.dds" % H2]
        self.assertAlmostEqual(flat["mean"] / 255.0, 250.0 / 255.0, places=4)
        self.assertAlmostEqual(flat["p95"] - flat["p05"], 0.0, places=6)   # a flat map has no spread

    def test_height_and_metal_are_decoded(self):
        self.assertEqual(self.data["height"]["n"], 1)
        self.assertEqual(self.data["height"]["flat"], 0)     # the fake height map ramps 0..252
        self.assertEqual(self.data["metal"]["n"], 1)

    # --- the list the next task is handed ------------------------------------------------------
    def test_uncovered_list_carries_reference_counts(self):
        path = os.path.join(self.out, "pack-uncovered.txt")
        rows = [l.split("\t") for l in open(path).read().split("\n") if l.strip()]
        self.assertEqual(len(rows), 2)
        for row in rows:
            self.assertEqual(len(row), 3, row)               # hash, references, category
            self.assertGreaterEqual(int(row[1]), 1)
        covered = open(os.path.join(self.out, "pack-covered.txt")).read().split()
        self.assertEqual(sorted(covered), sorted([H1, H2]))

    def test_no_decode_still_counts(self):
        r = subprocess.run([sys.executable, TOOL, self.root, "--no-decode"],
                           capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("unique mat_<HASH> 4", r.stdout)
        self.assertNotIn("_rough maps decoded", r.stdout)   # the decode pass is the slow half
        self.assertEqual(self.data["dds"], 9, self.stdout)

    def test_bad_usage_exits_2(self):
        r = subprocess.run([sys.executable, TOOL, os.path.join(self.root, "nope")],
                           capture_output=True, text=True)
        self.assertEqual(r.returncode, 2)


if __name__ == "__main__":
    unittest.main()
