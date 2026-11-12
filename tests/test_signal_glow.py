#!/usr/bin/env python3
"""n02b gate: a lit signal has TWO lights - the bulb at the lens AND the glow.

rhY, 2026-11-12 08:50: "We wanted the glow from the traffic light and the bulb.
It should be both like real life." The 2026-11-12 00:48 run had an ambient glow
and no bulb; s30 fixed the bulb and removed the ambient twice over (our light's
flux 304 -> 67, the game's own 15.0-unit corona -> 0.45), and n02 moved that one
light onto the lens without ever adding a second one.

The numbers half is held down natively in tests/signals_check.cpp (rhySignalGlow,
rhyLuminaireFlux: bulb small + hot, glow big + soft, same colour, flux 304 under
a street luminaire's 3473). This file holds down the wiring half, which is the
part that can rot silently:

  * the two knobs exist, with the measured defaults (3 m, 304 flux) and the
    street-luminaire ceiling (3473) compiled in, in config.cpp, in config.h, in
    the ini the plugin writes itself and in both shipped inis;
  * BOTH light paths (list mode and the game's heads) create a bulb AND a glow,
    draw both and destroy both - a glow that outlives its bulb is the
    2026-11-12 report back again, and a bulb with no glow is what s30 shipped;
  * the glow's hash cannot collide with its own bulb's;
  * the log counts both halves, so a run can be checked from rhyremix.log alone;
  * docs/NEXT-TEST.md tells rhY to look for both.

Run: python3 -m pytest -q tests/test_signal_glow.py   (or python3 -m unittest)
"""
import os
import re
import unittest

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CFG_CPP = os.path.join(REPO, "plugin", "src", "config.cpp")
CFG_H = os.path.join(REPO, "plugin", "src", "config.h")
HEADS_H = os.path.join(REPO, "plugin", "src", "signalheads.h")
HEADS_CPP = os.path.join(REPO, "plugin", "src", "signalheads.cpp")
TRAFFIC = os.path.join(REPO, "plugin", "src", "trafficlights.cpp")
PROD_INI = os.path.join(REPO, "config", "rhyremix-production.ini")
TEST_INI = os.path.join(REPO, "config", "rhyremix-test.ini")
DOC = os.path.join(REPO, "docs", "NEXT-TEST.md")


def read(path):
    with open(path, encoding="utf-8", errors="replace") as f:
        return f.read()


def flat(text):
    """Whitespace-normalised: the assertions below are about the code, not its
    line breaks, so a reformat must not break the gate."""
    return " ".join(text.split())


def ini_kv(text, section):
    """{key: value} of the active (uncommented) lines of one ini section."""
    out, cur = {}, None
    for raw in text.splitlines():
        line = raw.strip()
        m = re.match(r"^\[([a-z_]+)\]$", line)
        if m:
            cur = m.group(1)
            continue
        m = re.match(r"^([a-z_0-9A-Z]+)=(.*)$", line)
        if m and cur == section:
            out[m.group(1)] = m.group(2).strip()
    return out


def kini_defaults(cfg_src):
    """The kIniDefaults text the plugin writes when no rhyremix.ini exists."""
    m = re.search(r"static const char\* const kIniDefaults =(.*?);\n", cfg_src, re.S)
    assert m, "kIniDefaults not found in config.cpp"
    text = "".join(re.findall(r'"([^"]*)"', m.group(1))).replace("\\r\\n", "\n")
    return text


def calls(flat_src, fn):
    """Every call to fn(...) in whitespace-normalised source, as strings.
    `rhyCreateLight` is not a `createLight` call, hence the lookbehind."""
    out = []
    for m in re.finditer(r"(?<!rhy)" + re.escape(fn) + r"\(", flat_src):
        depth, i = 0, m.end() - 1
        while i < len(flat_src):
            if flat_src[i] == "(":
                depth += 1
            elif flat_src[i] == ")":
                depth -= 1
                if depth == 0:
                    break
            i += 1
        out.append(flat_src[m.start():i + 1])
    return out


class TestGlowKnobs(unittest.TestCase):
    def setUp(self):
        self.cfg = read(CFG_CPP)
        self.hdr = read(CFG_H)

    def test_config_parses_both_knobs_with_the_measured_defaults(self):
        # 3 m of spill, 304 flux: the 2026-11-12 00:48 glow, measured
        # (5400 nits over a 0.10 m sphere = 4*pi*r^2*mean = 304).
        self.assertRegex(flat(self.cfg),
                         r'iniFloatSec\("trafficlights", "glow_radius", 3\.0f\)')
        self.assertRegex(flat(self.cfg),
                         r'iniFloatSec\("trafficlights", "glow_flux", 304\.0f\)')

    def test_the_ceiling_is_compiled_in_and_clamped(self):
        # 3473 = one mid street luminaire's flux (rhyLuminaireFlux reproduces it
        # from radiance_big=2.0 / radius_ref=15 / the shipped 2100 K + 5000 K
        # pair). A signal must not out-light the lamp over its head.
        self.assertRegex(flat(self.cfg), r"traffic_glow_flux > 3473\.0f")
        self.assertRegex(flat(self.cfg), r"traffic_glow_radius > 20\.0f")
        self.assertIn("0 = no glow light, bulb only", self.cfg)

    def test_config_h_declares_them(self):
        for name in ("traffic_glow_radius", "traffic_glow_flux"):
            self.assertRegex(self.hdr, r"\b%s\b" % name)

    def test_the_ini_the_plugin_writes_and_both_shipped_inis_agree(self):
        kd = ini_kv(kini_defaults(self.cfg), "trafficlights")
        self.assertEqual(kd.get("glow_radius"), "3")
        self.assertEqual(kd.get("glow_flux"), "304")
        for path in (PROD_INI, TEST_INI):
            kv = ini_kv(read(path), "trafficlights")
            self.assertEqual(kv.get("glow_radius"), "3", "%s: glow_radius" % path)
            self.assertEqual(kv.get("glow_flux"), "304", "%s: glow_flux" % path)

    def test_the_config_log_line_echoes_them(self):
        self.assertIn("glow radius=%.2f flux=%.0f", self.cfg)
        self.assertIn("c.traffic_glow_radius, c.traffic_glow_flux", flat(self.cfg))


class TestGlowMath(unittest.TestCase):
    def setUp(self):
        self.h = read(HEADS_H)
        self.c = read(HEADS_CPP)

    def test_the_reference_and_the_ceiling_are_named_numbers(self):
        self.assertRegex(self.h, r"kRhySignalGlowFluxRef = 304\.0f")
        self.assertRegex(self.h, r"kRhySignalGlowFluxCap = 3473\.0f")
        self.assertRegex(self.h, r"kRhySignalGlowRadius = 3\.0f")

    def test_the_glow_is_solved_for_flux_not_for_radiance(self):
        body = flat(self.c)[flat(self.c).index("void rhySignalGlow"):]
        self.assertRegex(body, r"flux / \(4\.0f \* 3\.14159265358979f \* radius \* "
                               r"radius \* colMean\)")

    def test_the_glow_uses_the_same_lens_colour_as_the_bulb(self):
        body = self.c[self.c.index("void rhySignalGlow"):
                      self.c.index("int rhySignalPatchCheck")]
        self.assertIn("rhySignalLensRgb(phase, col)", body)

    def test_the_luminaire_flux_helper_exists(self):
        self.assertIn("float rhyLuminaireFlux(float radianceAtRef, float radiusRef, "
                      "const float* rgb)", self.h)
        self.assertIn("rhyLuminaireFlux", self.c)


class TestTwoLightsPerSignal(unittest.TestCase):
    """The wiring: both paths, both halves, drawn and destroyed together."""

    def setUp(self):
        self.src = read(TRAFFIC)
        self.f = flat(self.src)

    def test_both_create_functions_take_a_kind(self):
        self.assertRegex(self.f, r"createLight\(int index, int phase, const RhyConfig& c,"
                                 r" float gain, int kind\)")
        self.assertRegex(self.f, r"createHeadLight\(uint64_t id, int phase, const float\* "
                                 r"pos, const RhyConfig& c, float gain, int kind\)")
        self.assertIn("enum SignalLightKind { kSignalBulb = 0, kSignalGlow = 1 }", self.f)

    def test_the_list_path_creates_a_bulb_and_a_glow(self):
        cs = calls(self.f, "createLight")
        self.assertEqual(sum(1 for c in cs if c.endswith("kSignalBulb)")), 2,
                         "list recreate + new light: two bulb creates")
        self.assertEqual(sum(1 for c in cs if c.endswith("kSignalGlow)")), 3,
                         "recreate + new light + the glow-only repair: three")

    def test_the_head_path_creates_a_bulb_and_a_glow(self):
        cs = calls(self.f, "createHeadLight")
        self.assertEqual(sum(1 for c in cs if c.endswith("kSignalBulb)")), 2)
        self.assertEqual(sum(1 for c in cs if c.endswith("kSignalGlow)")), 3,
                         "recreate + new head + the glow-only repair")
        # and never a kind-less call: every create says which half it is
        self.assertEqual(len(cs), 6)
        self.assertEqual(len(calls(self.f, "createLight")), 6)

    def test_both_handles_are_drawn_and_destroyed(self):
        self.assertEqual(self.f.count("rhyDrawLight(it->second.glow)"), 2,
                         "the list loop and the head loop must both draw the glow")
        self.assertEqual(self.f.count("rhyDrawLight(it->second.handle)"), 2)
        self.assertGreaterEqual(self.f.count("rhyDelayDestroy(it->second.glow,"), 4,
                                "retire and recreate must destroy the glow too")
        self.assertGreaterEqual(self.f.count("rhyDelayDestroy(it->second.handle,"), 4)

    def test_a_glow_never_outlives_its_bulb(self):
        # every glow create is guarded on the bulb handle existing, and on the
        # glow being wanted at all (glow_flux=0 = bulb only, no dark handle)
        guarded = re.findall(r"if \(([\w.>-]+)(?: && ![\w.>-]+)? && glowWanted\(\*cfg\)",
                             self.f)
        self.assertEqual(len(guarded), 6,
                         "six glow creates (recreate, new, repair) x (list, head): %r"
                         % guarded)
        self.assertTrue(all("handle" in g or g == "h" for g in guarded),
                        "every one is guarded on the bulb handle: %r" % guarded)
        self.assertIn("static bool glowWanted(const RhyConfig& c) { return "
                      "c.traffic_glow_flux > 0.0f; }", self.f)
        self.assertIn("it->second.handle && !it->second.glow && glowWanted(*cfg)",
                      self.f)   # the glow-only repair, no bulb destroy

    def test_the_glow_hash_cannot_collide_with_its_bulb(self):
        self.assertIn("kRhySignalGlowHashBias", read(HEADS_H))
        self.assertEqual(self.f.count("kRhySignalGlowHashBias"), 2,
                         "the list path and the head path each bias the glow hash")

    def test_the_log_counts_both_halves(self):
        self.assertIn("%u heads lit (%u with a glow)", self.src)
        self.assertIn("%u active (%u with a glow)", self.src)
        self.assertIn('kind == kSignalGlow ? "glow" : "bulb"', self.f)


class TestTestDocSaysBulbAndGlow(unittest.TestCase):
    def test_the_signal_step_asks_for_both(self):
        doc = read(DOC)
        # n04 put finding A first and renumbered the steps, so the signal step is
        # found by its title, not its number: what this guards is that the signal
        # step asks for the bulb AND the glow, not which step number it got.
        m = re.search(r"\n## \d+\.[^\n]*signal heads[^\n]*\n", doc, re.I)
        self.assertTrue(m, "docs/NEXT-TEST.md has no 'signal heads' step")
        rest = doc[m.end():]
        end = rest.index("\n## ") if "\n## " in rest else len(rest)
        step = rest[:end].lower()
        self.assertIn("bulb", step, "docs/NEXT-TEST.md's signal step lost the bulb")
        self.assertIn("glow", step, "docs/NEXT-TEST.md's signal step lost the glow")
        self.assertRegex(
            step,
            r"bulb[^.]{0,300}and[^.]{0,120}glow|glow[^.]{0,300}and[^.]{0,120}bulb",
            "step 2 must ask for the bulb AND the glow, not one of them")


if __name__ == "__main__":
    unittest.main(verbosity=2)
