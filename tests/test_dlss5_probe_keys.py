#!/usr/bin/env python3
"""n19 gate: the DLSS 5 probe file names real options, at the runtime's own defaults, and stays non-shipped.

DLSS 5 (`rtx.dlssNeuralRendering.*`) is the single biggest item in docs/TARGET.md §2 - the reference
video's look is largely this - and the answer on this box is "the driver refuses it"
(`NVIDIA DLSS-NR support query failed: (code: 0xbad00010, info:
NVSDK_NGX_Result_FAIL_UnsupportedParameter)`, docs/FINDINGS.md n19). Two failure modes are specific
to a probe like this, and neither is a compile error:

  * **the wrong name.** `rtx.dlssNeuralRendering` is the option CATEGORY; the switch is
    `rtx.dlssNeuralRendering.enable` (RTX_OPTION("rtx.dlssNeuralRendering", bool, enable, false) in
    src/dxvk/rtx_render/rtx_dlss_neural_rendering.h, and config/rtx-options-1.6.1.tsv line 193).
    The category string IS in .trex/d3d9.dll's strings, so tools/rtxkeycheck.py - the audit the n19
    task file names - verdicts it "dotted" and passes it. A probe written with that spelling would
    run, change nothing, and be read as a hardware answer. So the probe is audited against the
    runtime's OWN option table too (tools/rtx-option-audit.py), and this file asserts the category
    name is still not an option so that check cannot go vacuous.
  * **a hopeful key.** The verdict is "unavailable", so no shipped profile may carry a
    `dlssNeuralRendering` key: not config/rtx.conf, not .perf, not .nopops, and nothing in the
    launcher/installer may install the probe by itself.

Everything else here is staleness insurance: the probe's values are the runtime's documented
defaults, so if a runtime update moves a default (or renames a leaf) the probe stops being a
measurement of the stock effect and this test says so offline.

Run: python3 -m pytest -q tests/test_dlss5_probe_keys.py  (or -m unittest)
"""
import os
import re
import sys
import unittest

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, "tools"))

PROBE = os.path.join(REPO, "config", "rtx.conf.dlss5")
TABLE = os.path.join(REPO, "config", "rtx-options-1.6.1.tsv")
DEAD = os.path.join(REPO, "config", "rtx-dead-keys.txt")
VOCAB = os.path.join(REPO, "config", "rtx-runtime-keys.txt")
PERF = os.path.join(REPO, "config", "rtx.conf.perf")
SHIPPED = [os.path.join(REPO, "config", n)
           for n in ("rtx.conf", "rtx.conf.perf", "rtx.conf.nopops")]

# The eleven keys of the probe, spelled out so a revert that deletes one fails the gate rather
# than quietly passing it. Values are what the probe writes; None = "the runtime's own default".
N19 = {
    "rtx.dlssNeuralRendering.enable": "True",
    "rtx.dlssNeuralRendering.intensity": "1",
    "rtx.dlssNeuralRendering.structuralStrength": "0.7",
    "rtx.dlssNeuralRendering.toneStrength": "0.3",
    "rtx.dlssNeuralRendering.model": "0",
    "rtx.dlssNeuralRendering.useAutoMask": "True",
    "rtx.dlssNeuralRendering.skinStructureStrength": "0.5",
    "rtx.dlssNeuralRendering.enableHighlightRecovery": "True",
    "rtx.dlssNeuralRendering.highlightRecoveryThresholds": "1, 8, 0.75, 0.99",
    "rtx.dlssNeuralRendering.enableVolumetricControlMask": "True",
    "rtx.dlssNeuralRendering.showVramUsage": "True",
}
# the two deviations from the runtime's defaults: the switch itself, and the diagnostic whose
# tooltip says "Enable before activating the feature"
DEVIATE = {"rtx.dlssNeuralRendering.enable", "rtx.dlssNeuralRendering.showVramUsage"}

# the category, which reads like the switch and is not one
CATEGORY = "rtx.dlssNeuralRendering"

# paths that could install a profile without being told to
AUTO = ["launcher", "apply.sh", "deploy.sh", "plugin", "scripts/test-session.sh",
        "scripts/abtest-lights.sh", "scripts/lighting-test.sh", "config/rhyremix-production.ini"]

KEYRE = re.compile(r"^\s*(rtx\.[A-Za-z0-9_.]+)\s*=\s*(.*?)\s*(?:#.*)?$")
HASHRE = re.compile(r"0x[0-9A-Fa-f]{16}")


def parse(path):
    out = {}
    with open(path, encoding="utf-8", errors="replace") as fh:
        for line in fh:
            m = KEYRE.match(line)
            if m:
                out[m.group(1)] = m.group(2).strip()
    return out


def table():
    opts = {}
    with open(TABLE, encoding="utf-8") as fh:
        for line in fh:
            if line.startswith("#") or not line.strip():
                continue
            p = line.rstrip("\n").split("\t")
            if len(p) >= 5:
                opts[p[0]] = {"type": p[1], "default": p[2], "min": p[3], "max": p[4]}
    return opts


def dead_keys():
    """config/rtx-dead-keys.txt: keys our profiles keep on purpose, each with its reason.
    The probe inherits them from rtx.conf.perf, so it is allowed exactly this set - the same rule
    tools/rtx-option-audit.py applies, and the same rule tests/test_rtx_option_audit.py keeps short."""
    out = set()
    with open(DEAD, encoding="utf-8") as fh:
        for line in fh:
            if line.startswith("#") or not line.strip():
                continue
            out.add(line.split("\t")[0])
    return out


def vocab():
    dotted, leaves = set(), set()
    with open(VOCAB, encoding="utf-8") as fh:
        for line in fh:
            if line.startswith("#"):
                continue
            kind, _, name = line.rstrip("\n").partition("\t")
            if kind == "dotted":
                dotted.add(name)
            elif kind == "leaf":
                leaves.add(name)
    return dotted, leaves


class TestProbeKeysAreReal(unittest.TestCase):
    def setUp(self):
        self.opts = table()
        self.probe = parse(PROBE)

    def test_the_probe_file_exists_and_is_a_whole_profile(self):
        self.assertTrue(os.path.isfile(PROBE), "config/rtx.conf.dlss5 is missing")
        self.assertGreater(len(self.probe), 100,
                           "config/rtx.conf.dlss5 is not a whole profile: rtxprofile.sh replaces "
                           "rtx.conf wholesale, so a fragment would delete the pack's texture lists")

    def test_every_key_of_the_probe_is_an_option_or_a_documented_dead_key(self):
        bad = sorted(k for k in self.probe if k not in self.opts)
        allowed = dead_keys()
        self.assertEqual([k for k in bad if k not in allowed], [],
                         "the probe carries keys that are neither options nor documented dead "
                         "keys (config/rtx-dead-keys.txt): %s" % bad)
        # perf's two anti-culling names are inherited on purpose; the probe may not add dead keys
        perf_bad = sorted(k for k in parse(PERF) if k not in self.opts)
        self.assertEqual(bad, perf_bad,
                         "the probe added a dead key rtx.conf.perf does not carry: %s" %
                         sorted(set(bad) ^ set(perf_bad)))
        # and none of them may be a DLSS-NR key: the probe's own block has to be real options
        for key in N19:
            self.assertIn(key, self.opts, "%s is not an option of this runtime" % key)

    def test_every_key_of_the_probe_is_in_the_runtime_strings(self):
        from rtxkeycheck import verdict
        dotted, leaves = vocab()
        bad = []
        for key in self.probe:
            kind, why = verdict(key, dotted, leaves)
            if kind is None:
                bad.append("%s (%s)" % (key, why))
        self.assertEqual(bad, [], "the probe sets keys this runtime's binary does not name: %s" % bad)

    def test_the_probe_carries_all_eleven_dlss_nr_keys(self):
        have = {k for k in self.probe if k.startswith(CATEGORY)}
        self.assertEqual(have, set(N19), "the probe's DLSS-NR key set changed")

    def test_the_probe_writes_the_switch_and_not_the_category(self):
        self.assertIn("rtx.dlssNeuralRendering.enable", self.probe)
        self.assertNotIn(CATEGORY, self.probe,
                         "%s = ... is the CATEGORY, not an option: the runtime keeps the line and "
                         "ignores it, and the probe would measure nothing" % CATEGORY)

    def test_the_category_is_in_the_strings_but_is_not_an_option(self):
        """The negative control. If a future runtime makes the category a real option, delete this
        test with a comment - but until then it is the reason the option-table audit exists."""
        dotted, _ = vocab()
        self.assertIn(CATEGORY, dotted,
                      "the category name is no longer in the runtime's strings: the vocabulary file "
                      "is stale, regenerate it with tools/rtx-runtime-keys.py")
        self.assertNotIn(CATEGORY, self.opts,
                         "%s became a real option - re-read the runtime before trusting this guard"
                         % CATEGORY)
        from rtxkeycheck import verdict
        self.assertIsNotNone(verdict(CATEGORY, *vocab())[0],
                             "rtxkeycheck changed - re-check whether the second audit is needed")

    def test_the_strings_audit_alone_would_have_passed_the_wrong_name(self):
        """Why this file exists: the audit the n19 task file names is not sufficient on its own."""
        from rtxkeycheck import verdict
        kind, _ = verdict(CATEGORY, *vocab())
        self.assertEqual(kind, "dotted")


class TestProbeValues(unittest.TestCase):
    def setUp(self):
        self.opts = table()
        self.probe = parse(PROBE)

    def test_the_defaults_the_probe_claims_are_still_the_runtime_defaults(self):
        for key in N19:
            self.assertIn(key, self.opts, "%s is not an option of this runtime any more" % key)
        self.assertEqual(self.opts["rtx.dlssNeuralRendering.enable"]["default"], "False")
        self.assertEqual(self.opts["rtx.dlssNeuralRendering.intensity"]["default"], "1")
        self.assertEqual(self.opts["rtx.dlssNeuralRendering.structuralStrength"]["default"], "0.7")
        self.assertEqual(self.opts["rtx.dlssNeuralRendering.toneStrength"]["default"], "0.3")
        self.assertEqual(self.opts["rtx.dlssNeuralRendering.model"]["default"], "0")
        self.assertEqual(self.opts["rtx.dlssNeuralRendering.useAutoMask"]["default"], "True")
        self.assertEqual(self.opts["rtx.dlssNeuralRendering.skinStructureStrength"]["default"], "0.5")
        self.assertEqual(self.opts["rtx.dlssNeuralRendering.enableHighlightRecovery"]["default"], "True")
        self.assertEqual(self.opts["rtx.dlssNeuralRendering.enableVolumetricControlMask"]["default"], "True")
        self.assertEqual(self.opts["rtx.dlssNeuralRendering.showVramUsage"]["default"], "False")
        self.assertEqual(self.opts["rtx.dlssNeuralRendering.highlightRecoveryThresholds"]["default"],
                         "1, 8, 0.75, 0.99")

    def test_every_probe_value_is_the_documented_default_except_the_two_switches(self):
        for key, want in N19.items():
            got = self.probe.get(key)
            self.assertIsNotNone(got, "%s missing from the probe" % key)
            if key in DEVIATE:
                self.assertEqual(got, "True", "%s must be True in the probe" % key)
            else:
                self.assertEqual(got, self.opts[key]["default"],
                                 "%s is %r, the runtime default is %r: the probe is supposed to ask "
                                 "for the stock effect, not a tuned one" % (key, got, self.opts[key]["default"]))

    def test_the_switch_is_on_and_the_model_is_A(self):
        self.assertEqual(self.probe.get("rtx.dlssNeuralRendering.enable"), "True")
        self.assertEqual(self.probe.get("rtx.dlssNeuralRendering.model"), "0")

    def test_the_floats_are_inside_the_runtime_s_own_ranges(self):
        for key in N19:
            spec = self.opts[key]
            if not spec["min"] and not spec["max"]:
                continue
            val = float(self.probe[key])
            if spec["min"]:
                self.assertGreaterEqual(val, float(spec["min"]), key)
            if spec["max"]:
                self.assertLessEqual(val, float(spec["max"]), key)

    def test_the_thresholds_line_is_four_numbers(self):
        """Config::parseOptionValue(Vector4) splits on ',' and needs exactly four floats
        (src/util/config/config.cpp:1328-1349); anything else is a silent parse failure."""
        toks = [t.strip() for t in
                self.probe["rtx.dlssNeuralRendering.highlightRecoveryThresholds"].split(",")]
        self.assertEqual(len(toks), 4)
        nums = [float(t) for t in toks]
        self.assertLess(nums[0], nums[1])       # HDR smoothstep edges
        self.assertLess(nums[2], nums[3])       # LDR smoothstep edges


class TestProbeStaysNonShipped(unittest.TestCase):
    def test_no_shipped_profile_carries_a_dlss_nr_key(self):
        for path in SHIPPED:
            self.assertNotIn(CATEGORY, parse(path),
                             "%s carries a DLSS-NR key. n19's verdict is that the feature cannot "
                             "initialise on this GPU/driver, so a hopeful key in a shipped config is "
                             "a lie - docs/FINDINGS.md (n19)" % os.path.basename(path))

    def test_the_probe_differs_from_perf_by_exactly_the_eleven_keys(self):
        a, b = parse(PERF), parse(PROBE)
        diff = {k for k in set(a) | set(b) if a.get(k) != b.get(k)}
        self.assertEqual(diff, set(N19),
                         "the probe is not 'perf plus DLSS-NR' any more: %s" %
                         ",".join(sorted(diff ^ set(N19))))

    def test_the_probe_keeps_every_pack_texture_list_of_the_perf_profile(self):
        """rtxprofile.sh replaces <game>/rtx.conf wholesale, so a probe profile that lost a hash
        list would break the asset pack the same way a hand edit would."""
        lists = [k for k in parse(PERF) if k.endswith("Textures")]
        self.assertGreaterEqual(len(lists), 7, "no texture lists found - re-check the parser")
        a, b = parse(PERF), parse(PROBE)
        for key in lists:
            self.assertEqual(sorted(HASHRE.findall(a[key])),
                             sorted(HASHRE.findall(b[key])),
                             "%s differs between rtx.conf.perf and the probe" % key)

    def test_nothing_installs_the_probe_by_itself(self):
        hits = []
        for rel in AUTO:
            path = os.path.join(REPO, rel)
            if os.path.isfile(path):
                files = [path]
            elif os.path.isdir(path):
                files = [os.path.join(dp, f) for dp, _, fs in os.walk(path) for f in fs
                         if f.endswith((".sh", ".py", ".cpp", ".h", ".ini", ".lua"))]
            else:
                continue
            for f in files:
                try:
                    text = open(f, encoding="utf-8", errors="replace").read()
                except OSError:
                    continue
                if "dlss5" in text or "dlssNeuralRendering" in text:
                    hits.append(os.path.relpath(f, REPO))
        self.assertEqual(hits, [],
                         "the probe is referenced by something that could apply it automatically: %s"
                         % hits)


if __name__ == "__main__":
    unittest.main(verbosity=2 if "-v" in sys.argv else 0)
