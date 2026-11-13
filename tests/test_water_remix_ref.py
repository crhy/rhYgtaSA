#!/usr/bin/env python3
"""n07 — tests for the research-derived water variant and the rtx.conf keys that go with it.

None of this needs the game folder. Four layers:

  1. config/water/remix-ref.usda is a valid variant (tools/water_check.py against
     config/water/pack.usda, the in-repo byte-identical copy of the pack's own file) and carries
     the exact numbers the research picked, in BOTH prims;
  2. tools/water.sh `remix-ref` against a fake game tree: it generates its own sheet at the
     research tilt (not the 'soft' one), installs the USD, `status` names it, `pack` restores byte
     for byte, `--dry-run` writes nothing;
  3. the rtx.conf keys water.sh WRITES for the variant (n13 - it used to print them) are keys the
     runtime actually has, no fork-only key is among them, and the write really lands in a live
     rtx.conf: in place, exactly once, with a pristine backup, and idempotent on a second run;
     (tools/rtxkeycheck.py against config/rtx-runtime-keys.txt) - a printed key that the runtime
     does not know is a silent no-op, which is the failure mode that audit exists for;
  4. the shipped profiles carry the n07 water keys and the opacity-micromap guard rails, carry
     BOTH water hashes in rtx.animatedWaterTextures, and do NOT carry the fork-only shoreline keys
     the research documents as unavailable in Remix 1.6.1.

Run: python3 -m pytest -q tests/test_water_remix_ref.py   (or python3 -m unittest)
"""
import os
import re
import subprocess
import sys
import unittest

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.join(HERE, "..")
sys.path.insert(0, os.path.join(REPO, "tools"))
sys.path.insert(0, HERE)
import rtxkeycheck  # noqa: E402
import water_normal_soft as W  # noqa: E402
from test_water_soft import bc5_dds, decode_dds_rgba, rb, synthetic_planes  # noqa: E402

SEA_HASH = "0x59296793798A7318"          # the sea texture the game generates
CLEAR_HASH = "0xA75A0B41F2454F04"        # the game's waterclear256, the second water material
WATER_KEYS = [
    "rtx.translucentMaterial.animatedWaterEnable",
    "rtx.translucentMaterial.animatedWaterSecondaryNormalLodBias",
    "rtx.rayreconstruction.invalidateHistoryForAnimatedWater",
    "rtx.rayreconstruction.combineSpecularAlbedo",
    "rtx.volumetrics.enableTranslucentShadows",
]
OMM_KEYS = [
    "rtx.opacityMicromap.cache.maxVidmemSizePercentage",
    "rtx.opacityMicromap.building.conservativeEstimation.maxTexelTapsPerMicroTriangle",
    "rtx.opacityMicromap.buildRequests.enableAnimatedInstances",
    "rtx.opacityMicromap.building.splitBillboardGeometry",
]
# keys xoxor4d's GTA IV RTX ships for its shoreline fade, which live in a FORK's runtime: writing
# them into our profile would do nothing at all, so the profile must not contain them.
FORK_ONLY = ["rtx.water.shoreCutDepth", "rtx.water.shoreFadeWidth", "rtx.water.shoreFadeDistance",
             "rtx.water.objectFadeWidth", "rtx.atmosphere.waterHorizonFadeStartMeters"]


def conf_keys(path):
    """{key: value} of the active (non-comment) assignments of an rtx.conf-style file."""
    out = {}
    for line in open(path, encoding="utf-8"):
        m = re.match(r"^\s*(rtx\.[A-Za-z0-9_.]+)\s*=\s*(.*)$", line)
        if m and not line.lstrip().startswith("#"):
            out[m.group(1)] = m.group(2).strip()
    return out


class TestVariantFile(unittest.TestCase):
    USD = os.path.join(REPO, "config", "water", "remix-ref.usda")

    def setUp(self):
        self.assertTrue(os.path.isfile(self.USD), "config/water/remix-ref.usda is missing")
        self.text = open(self.USD, encoding="utf-8").read()

    def test_it_validates_against_the_packs_own_material(self):
        r = subprocess.run([sys.executable, os.path.join(REPO, "tools", "water_check.py"),
                            self.USD, os.path.join(REPO, "config", "water", "pack.usda")],
                           capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("all declared by the mdl", r.stdout)

    def test_the_research_numbers_are_there_in_both_prims(self):
        want = {
            "transmittance_color": r"\(0\.62, 0\.8, 0\.8\)",       # gta4-rtx's shipped water tint
            "transmittance_measurement_distance": r"4 \(",         # ... at 4 m
            "normalmap_texture": r"@\.\/water_normal_ref\.dds@",   # the regenerated sheet
            "sprite_sheet_fps": r"12 \(",                          # 1.67 s loop, UV scroll does the rest
            "thin_walled": r"0 \(",                                # every project surveyed uses 0
            "use_diffuse_layer": r"0 \(",
            "encoding": r"0 \(",                                   # octahedral, the only workable decode
        }
        for name, pat in want.items():
            got = re.findall(r"inputs:%s\s*=\s*(.+)" % name, self.text)
            self.assertEqual(len(got), 2, "%s: expected one line per prim, got %r" % (name, got))
            for v in got:
                self.assertRegex(v.strip(), pat, "%s = %s is not the researched value" % (name, v))
        ior = re.findall(r"inputs:ior_constant\s*=\s*([0-9.]+)", self.text)
        self.assertEqual(len(ior), 2)
        self.assertTrue(all(abs(float(v) - 1.33) < 1e-4 for v in ior), ior)

    def test_the_research_is_cited_in_the_file(self):
        urls = set(re.findall(r"https?://[^\s)\"]+", self.text))
        self.assertGreaterEqual(len(urls), 3,
                                "the variant must name where its numbers come from: %s" % sorted(urls))
        self.assertIn("notes-n07-water-research.md", self.text)


class TestWaterShVariant(unittest.TestCase):
    """tools/water.sh remix-ref against a fake game tree (same fake tree as test_water_soft)."""

    def setUp(self):
        import tempfile
        self.tmp = tempfile.TemporaryDirectory()
        self.wdir = os.path.join(self.tmp.name, "rtx-remix", "mods", "gameReadyAssets",
                                 "texture", "Water")
        os.makedirs(self.wdir)
        for name in ("Water_materials.usda", "Water_materials.usda.orig"):
            with open(os.path.join(self.wdir, name), "wb") as fh:
                fh.write(rb(os.path.join(REPO, "config", "water", "realistic.usda")))
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

    def test_remix_ref_generates_its_own_sheet_and_installs(self):
        r = self.sh("remix-ref")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("installed water variant 'remix-ref'", r.stdout)
        self.assertTrue(os.path.exists(os.path.join(self.wdir, "water_normal_ref.dds")),
                        "the variant points at water_normal_ref.dds; it must exist before the USD does")
        self.assertNotIn("water_normal_soft.dds", os.listdir(self.wdir),
                         "remix-ref must not generate the 'soft' sheet")
        cur = rb(os.path.join(self.wdir, "Water_materials.usda"))
        self.assertIn(b"water_normal_ref.dds", cur)
        self.assertIn(b"transmittance_color = (0.62, 0.8, 0.8)", cur)
        self.assertIn("remix-ref (n07: research numbers + regenerated normal sheet)",
                      self.sh("status").stdout)

    def test_generated_sheet_lands_on_the_researched_tilt(self):
        r = self.sh("remix-ref")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        w, h, dxgi, a = decode_dds_rgba(os.path.join(self.wdir, "water_normal_ref.dds"))
        self.assertEqual(w // 20 * 20, w, "20 frames side by side is what sprite_sheet_cols says")
        n = W.decode_octa_rg(a[..., 0], a[..., 1])
        tilt = W.tilt_deg(n)
        self.assertLessEqual(float(np.median(tilt)), 9.0,
                             "target 6 deg: the sheet must be calmer than the pack's 44 deg")
        self.assertLess(float(np.percentile(tilt, 95)), 20.0)

    def test_pack_restores_byte_for_byte_and_dry_run_writes_nothing(self):
        self.assertEqual(self.sh("pack").returncode, 0)
        self.assertEqual(rb(os.path.join(self.wdir, "Water_materials.usda")), self.orig)
        d = self.sh("remix-ref", dry=True)
        self.assertEqual(d.returncode, 0, d.stdout + d.stderr)
        self.assertNotIn("water_normal_ref.dds", os.listdir(self.wdir))
        self.assertEqual(rb(os.path.join(self.wdir, "Water_materials.usda")), self.orig)
        self.assertIn("+ cp", d.stdout)

    def test_variants_coexist_and_soft_still_works(self):
        self.assertEqual(self.sh("remix-ref").returncode, 0)
        self.assertEqual(self.sh("soft").returncode, 0)
        self.assertEqual(self.sh("remix-ref").returncode, 0)
        for name in ("water_normal_ref.dds", "water_normal_soft.dds"):
            self.assertTrue(os.path.exists(os.path.join(self.wdir, name)), name)
        self.assertIn("already there", self.sh("remix-ref").stdout)
        self.assertIn("remix-ref (n07: research numbers + regenerated normal sheet)",
                      self.sh("status").stdout)
        self.assertEqual(self.sh("soft").returncode, 0)
        self.assertIn("soft (s32: regenerated normal sheet)", self.sh("status").stdout)
        self.assertEqual(self.sh("pack").returncode, 0)
        self.assertEqual(rb(os.path.join(self.wdir, "Water_materials.usda")), self.orig)


class TestWrittenRtxConfKeysAreReal(unittest.TestCase):
    """n13: water.sh no longer PRINTS the rtx.conf lines that go with a variant, it WRITES them.

    So the thing to audit is what it writes: every key in its `key_table()` and every hash in its
    `hash_require()` must be a key this runtime actually has, no fork-only key may appear, and the
    write must really land in a live rtx.conf - in place, once, with a backup, and idempotently.
    """

    def written(self):
        src = open(os.path.join(REPO, "tools", "water.sh"), encoding="utf-8").read()
        tbl = re.search(r"key_table\(\) \{(.*?)\n\}\n", src, re.S)
        self.assertTrue(tbl, "no key_table() in tools/water.sh - what writes the rtx.conf keys now?")
        keys = set(re.findall(r"^(rtx\.[A-Za-z0-9_.]+)\t", tbl.group(1), re.M))
        hr = re.search(r"hash_require\(\) \{(.*?)\n\}\n", src, re.S)
        self.assertTrue(hr, "no hash_require() in tools/water.sh")
        hashes = set(re.findall(r"(0x[0-9A-Fa-f]{16})", hr.group(1)))
        return keys, hashes

    def test_written_keys_pass_the_runtime_audit(self):
        keys, hashes = self.written()
        self.assertGreaterEqual(len(keys), 6, "key_table() names too few keys: %s" % sorted(keys))
        dotted, leaves = rtxkeycheck.load_keys(os.path.join(REPO, "config", "rtx-runtime-keys.txt"))
        unknown = [k for k in sorted(keys) if rtxkeycheck.verdict(k, dotted, leaves)[0] is None]
        self.assertEqual(unknown, [], "water.sh writes keys this runtime does not have: %s" % unknown)
        self.assertEqual(hashes, {SEA_HASH, CLEAR_HASH},
                         "the required hashes must be exactly the two water surfaces")

    def test_no_fork_only_key_is_written(self):
        keys, _ = self.written()
        src = open(os.path.join(REPO, "tools", "water.sh"), encoding="utf-8").read()
        for k in FORK_ONLY:
            self.assertNotIn(k, keys, "key_table() writes %s, which our 1.6.1 does not know" % k)
            self.assertNotIn(k, src, "tools/water.sh mentions the fork-only key %s" % k)

    def test_the_keys_really_are_written_to_a_live_rtx_conf(self):
        import tempfile
        tmp = tempfile.TemporaryDirectory()
        try:
            g = tmp.name
            wdir = os.path.join(g, "rtx-remix", "mods", "gameReadyAssets", "texture", "Water")
            os.makedirs(wdir)
            for name in ("Water_materials.usda", "Water_materials.usda.orig"):
                with open(os.path.join(wdir, name), "wb") as fh:
                    fh.write(rb(os.path.join(REPO, "config", "water", "realistic.usda")))
            R, G = synthetic_planes()
            bc5_dds(os.path.join(wdir, "water_normal.dds"), R, G)
            # a live-looking rtx.conf: the list WITHOUT the second water hash, no animated-water keys,
            # and one key present with the wrong value plus a trailing comment to be kept
            conf = os.path.join(g, "rtx.conf")
            with open(conf, "w", encoding="utf-8", newline="") as fh:
                fh.write("rtx.animatedWaterTextures = %s, 0x00FAD6ECF37BFBDB\r\n" % SEA_HASH)
                fh.write("rtx.translucentMaterial.normalIntensity = 0.35  # s02, hand-tuned\r\n")
            before = open(conf, "rb").read()
            r = subprocess.run(["bash", os.path.join(REPO, "tools", "water.sh"), "remix-ref", g],
                               capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            text = open(conf, encoding="utf-8", newline="").read()
            keys, hashes = self.written()
            for k in keys:
                self.assertIn("%s = " % k, text, "water.sh did not write %s" % k)
            for h in hashes:
                self.assertEqual(len(re.findall(re.escape(h), text)), 1, "%s listed twice" % h)
            self.assertEqual(len(re.findall(r"(?m)^rtx\.animatedWaterTextures = ", text)), 1,
                             "the hash list must stay ONE line, extended, never duplicated")
            self.assertIn("normalIntensity = 1.0  # s02, hand-tuned", text,
                          "a rewritten value must keep the rest of its line")
            self.assertTrue(os.path.exists(os.path.join(g, ".rhys-water", "rtx.conf.pristine")),
                            "the pre-change rtx.conf must be kept")
            self.assertEqual(open(os.path.join(g, ".rhys-water", "rtx.conf.pristine"), "rb").read(),
                             before, "the pristine backup must hold the bytes we replaced")
            r2 = subprocess.run(["bash", os.path.join(REPO, "tools", "water.sh"), "remix-ref", g],
                                capture_output=True, text=True)
            self.assertEqual(r2.returncode, 0, r2.stdout + r2.stderr)
            self.assertEqual(open(conf, encoding="utf-8", newline="").read(), text,
                             "a second run must change nothing")
            self.assertIn("already carries every requested water key", r2.stdout)
        finally:
            tmp.cleanup()


class TestShippedProfiles(unittest.TestCase):
    def test_both_water_surfaces_are_classified_as_animated_water(self):
        lists = {}
        for name in ("rtx.conf", "rtx.conf.perf"):
            line = [l for l in open(os.path.join(REPO, "config", name), encoding="utf-8")
                    if l.startswith("rtx.animatedWaterTextures =")][0]
            hashes = [h.strip() for h in line.split("=", 1)[1].strip().split(",")]
            self.assertEqual(len(hashes), len(set(hashes)), "%s has duplicate hashes" % name)
            self.assertEqual(hashes, sorted(hashes, key=lambda h: int(h, 16)),
                             "%s: the hash list must stay sorted" % name)
            self.assertIn(SEA_HASH, hashes)
            self.assertIn(CLEAR_HASH, hashes, "%s: waterclear256 must be in the water category" % name)
            lists[name] = hashes
        self.assertEqual(lists["rtx.conf"], lists["rtx.conf.perf"],
                         "the two profiles must classify the same water")

    def test_the_n07_water_and_omm_keys_are_in_both_profiles(self):
        for name in ("rtx.conf", "rtx.conf.perf"):
            got = conf_keys(os.path.join(REPO, "config", name))
            for k in WATER_KEYS + OMM_KEYS:
                self.assertIn(k, got, "%s does not carry %s" % (name, k))
            self.assertEqual(got["rtx.translucentMaterial.normalIntensity"], "1.0",
                             "%s: the regenerated sheets need strength 1.0, not s02's 0.35" % name)
        shipped = conf_keys(os.path.join(REPO, "config", "rtx.conf"))
        self.assertEqual(shipped["rtx.enableDirectTranslucentShadows"], "True")
        self.assertNotIn("rtx.enableDirectTranslucentShadows",
                         conf_keys(os.path.join(REPO, "config", "rtx.conf.perf")),
                         "the perf profile deliberately skips the ray-costing shadow key")

    def test_no_fork_only_shoreline_keys_are_shipped(self):
        for name in ("rtx.conf", "rtx.conf.perf", "rtx.conf.nopops"):
            text = open(os.path.join(REPO, "config", name), encoding="utf-8").read()
            for k in FORK_ONLY:
                self.assertNotIn("\n%s =" % k, text,
                                 "%s writes %s, which stock Remix 1.6.1 does not know" % (name, k))

    def test_every_key_of_the_perf_profile_is_known_to_the_runtime(self):
        dotted, leaves = rtxkeycheck.load_keys(os.path.join(REPO, "config", "rtx-runtime-keys.txt"))
        got = conf_keys(os.path.join(REPO, "config", "rtx.conf.perf"))
        unknown = [k for k in got if rtxkeycheck.verdict(k, dotted, leaves)[0] is None]
        self.assertEqual(unknown, [], "unknown keys in rtx.conf.perf: %s" % unknown)

    def test_the_nopops_profile_still_matches_its_generator(self):
        r = subprocess.run([sys.executable, os.path.join(REPO, "tools", "rtx-nopops.py"), "--check"],
                           capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)


if __name__ == "__main__":
    unittest.main(verbosity=2)
