#!/usr/bin/env python3
"""n23 gate: tools/night-brightness.py measures finding C's number and FAILS the run that
misses it, and the numbers the task ships are the numbers the files carry.

    python3 -m pytest -q tests/test_night_brightness.py

Everything runs against a fake game tree under build/tmp/ made of solid-band PNGs whose
means and lit fractions are known exactly, so the arithmetic (night mean as a share of the
noon mean at the same stop; the lit/unlit split the lamp model runs on; the sky-band and
bright-end ratios of the sunset pair) is checked against numbers we chose, not against a
screenshot. The real 2026-11-12 10:22 run - 57.3 % under a lamp, 72.0 % lit pixels - is
what the thresholds were tuned on, but it is never read here (rule 1: the game folder is
read-only and the tests must run without it).

What is pinned:
  * the clock a stop name carries (noon, HHMM at either end, dawn+0530) and the
    day/night/twilight classes it implies - the target is a NIGHT target, so a
    sunset stop must not be scored as one;
  * the same-stop partner is found by the location words, not by the stop number;
  * a night stop at 23 % of noon passes and one at 70 % fails, naming the stop;
  * a run with NO night stop fails too: a table with no rows is not a pass;
  * the lit-pixel fraction and the mean_lit / mean_dark split come out of an image whose
    bands we drew, and --predict reproduces the n05 model on them;
  * a flat sky band facing the sun is reported GRAY, a structured pair is not;
  * the shipped numbers: [streetlights] radiance_scale=0.4, bulb_radiance=840,
    glow_flux=304 kept, [trafficlights] radiance_scale=1.0, [vehiclelights]
    radiance_scale documented at 1.0, and the three places the street knob used to
    reach through;
  * what this task did NOT touch: n16's rtx.tonemappingMode = 0 / localtonemap pair and
    n22's [tonemap] night_ev=0.5, which is the number the next task decides.
"""
import importlib.util
import io
import os
import re
import sys
import unittest

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FAKE = os.path.join(REPO, "build", "tmp", "n23-fake-tree")
FAKE_PASS = os.path.join(REPO, "build", "tmp", "n23-fake-pass")
FAKE_NONIGHT = os.path.join(REPO, "build", "tmp", "n23-fake-no-night")

_spec = importlib.util.spec_from_file_location(
    "nightbrightness", os.path.join(REPO, "tools", "night-brightness.py"))
nb = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(nb)

try:
    from PIL import Image
    HAVE_PIL = True
except ImportError:                                       # pragma: no cover
    HAVE_PIL = False


def write_shot(shots, stamp, top, bottom, size=(48, 36)):
    """A two-band image: `top` fills the top third, `bottom` the rest, both grey."""
    w, h = size
    im = Image.new("L", (w, h))
    px = im.load()
    for y in range(h):
        v = top if y < h // 3 else bottom
        for x in range(w):
            px[x, y] = v
    path = os.path.join(shots, "shot_%s.png" % stamp)
    im.save(path)
    return path


def write_ramp_shot(shots, stamp, size=(48, 36)):
    """A shot whose sky band has structure (a 0..255 ramp) - the 'facing away' look."""
    w, h = size
    im = Image.new("L", (w, h))
    px = im.load()
    for y in range(h):
        if y < h // 3:
            v = int(255.0 * y / max(1, h // 3))
        else:
            v = 128
        for x in range(w):
            px[x, y] = v
    path = os.path.join(shots, "shot_%s.png" % stamp)
    im.save(path)
    return path


def fake_tree(root, manifest_lines, shots):
    shots_dir = os.path.join(root, "screenshots", "shots")
    if not os.path.isdir(shots_dir):
        os.makedirs(shots_dir)
    with open(os.path.join(root, "screenshots", "tour-manifest.txt"), "w") as fh:
        fh.write("\n".join(manifest_lines) + "\n")
    for s in shots:
        s(shots_dir)


def run_main(argv):
    """nb.main(argv) with stdout captured. -> (exit code, output)."""
    buf = io.StringIO()
    real = sys.stdout
    sys.stdout = buf
    try:
        rc = nb.main(argv)
    finally:
        sys.stdout = real
    return rc, buf.getvalue()


class TestNaming(unittest.TestCase):
    def test_clock_in_the_stop_name(self):
        self.assertEqual(nb.clock_of("00-baseline-noon-grove"), (12, 0))
        self.assertEqual(nb.clock_of("01-under-lamp-grove-2100"), (21, 0))
        self.assertEqual(nb.clock_of("12-car-mirror-grove-1300"), (13, 0))
        self.assertEqual(nb.clock_of("08a-sunset-facing-west-1905"), (19, 5))
        # digits win over the word: 09-grove-dawn-0530 is 05:30, not the 05:30 default
        self.assertEqual(nb.clock_of("09-grove-dawn-0530"), (5, 30))
        # a stop number is not a clock
        self.assertIsNone(nb.clock_of("14-vegetation-vinewood"))

    def test_classes(self):
        self.assertEqual(nb.day_class("00-baseline-noon-grove"), "day")
        self.assertEqual(nb.day_class("12-car-mirror-grove-1300"), "day")
        self.assertEqual(nb.day_class("01-under-lamp-grove-2100"), "night")
        self.assertEqual(nb.day_class("11-water-santa-maria-2100"), "night")
        # sunset is not night: the tour's 19:05 stops have the sun 10 degrees up
        self.assertEqual(nb.day_class("08b-sunset-facing-west-1905"), "twilight")
        self.assertEqual(nb.day_class("09-grove-dawn-0530"), "twilight")
        self.assertIsNone(nb.day_class("14-vegetation-vinewood"))

    def test_location_words(self):
        a = nb.location_words("01-under-lamp-grove-2100")
        b = nb.location_words("00-baseline-noon-grove")
        self.assertIn("grove", a & b)
        self.assertNotIn("2100", a)
        self.assertNotIn("noon", b)


class TestAgainstAFakeTree(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not HAVE_PIL:
            raise unittest.SkipTest("PIL is required for the image fixtures")
        lines = [
            "2026-11-14 09:00:00  tour start, 5 stops",
            "2026-11-14 09:00:05  shot 01 00-baseline-noon-grove",
            "2026-11-14 09:00:22  shot 02 01-under-lamp-grove-2100",
            "2026-11-14 09:00:39  shot 03 05-burger-shot-marina-2100",
            "2026-11-14 09:00:56  shot 04 08a-sunset-facing-east-1905",
            "2026-11-14 09:01:13  shot 05 08b-sunset-facing-west-1905",
            "2026-11-14 09:01:15  tour done",
        ]

        def make(shots):
            write_shot(shots, "2026-11-14_09-00-05", 128, 128)     # noon, mean 128
            write_shot(shots, "2026-11-14_09-00-22", 100, 10)      # 1/3 lit, mean 40
            write_shot(shots, "2026-11-14_09-00-39", 90, 90)       # night, 70 % of noon
            write_ramp_shot(shots, "2026-11-14_09-00-56")          # away: sky has structure
            write_shot(shots, "2026-11-14_09-01-13", 120, 120)     # into the sun: flat slab

        fake_tree(FAKE, lines, [make])

    def ratios(self, out):
        return {l.split()[0]: float(l.split("%")[0].split()[-1])
                for l in out.splitlines() if "% of" in l and "mean=" in l}

    def test_night_ratios_and_exit_code(self):
        rc, text = run_main([FAKE, "--since", "2026-11-14 09:00",
                             "--sunset", "08a-sunset-facing-east-1905",
                             "08b-sunset-facing-west-1905"])
        r = self.ratios(text)
        # 40/128 = 31.2 % misses the 25 % target, 90/128 = 70.3 % misses it harder
        self.assertAlmostEqual(r["01-under-lamp-grove-2100"], 31.2, delta=0.6)
        self.assertAlmostEqual(r["05-burger-shot-marina-2100"], 70.3, delta=0.6)
        self.assertEqual(rc, 1)
        self.assertIn("OVER", text)
        self.assertIn("05-burger-shot-marina-2100 at", text)
        # the sunset pair: a flat sky band facing the sun is the gray defect
        self.assertIn("sky_spread", text)
        self.assertIn("GRAY", text)

    def test_the_lit_split_and_the_model_come_out_of_the_image(self):
        # The 1/3-lit fixture: floor p10 = 10, lit = the top third, mean_lit 100, dark 10.
        rc, text = run_main([FAKE, "--since", "2026-11-14 09:00", "--predict", "0.4"])
        self.assertEqual(rc, 1)
        self.assertIn("lit  33.3 % of the frame", text)
        self.assertIn("mean_lit= 100.0 mean_dark=  10.0", text)
        # mean_dark + (lit/100) * S * (mean_lit - mean_dark) = 10 + 0.333*0.4*90 = 22.0
        self.assertIn("predicted with the lamps at 0.40: mean   22.0", text)
        # and --lit-max turns that same number into a gate
        rc, text = run_main([FAKE, "--since", "2026-11-14 09:00", "--lit-max", "25"])
        self.assertEqual(rc, 1)
        self.assertIn("has 33.3 % lit pixels (max 25)", text)

    def test_a_quiet_night_and_a_structured_sunset_pass(self):
        lines = [
            "2026-11-14 09:00:00  tour start, 4 stops",
            "2026-11-14 09:00:05  shot 01 00-baseline-noon-grove",
            "2026-11-14 09:00:22  shot 02 01-under-lamp-grove-2100",
            "2026-11-14 09:00:39  shot 03 08a-sunset-facing-east-1905",
            "2026-11-14 09:00:56  shot 04 08b-sunset-facing-west-1905",
            "2026-11-14 09:00:58  tour done",
        ]

        def make(shots):
            write_shot(shots, "2026-11-14_09-00-05", 128, 128)
            write_shot(shots, "2026-11-14_09-00-22", 16, 16)        # 12.5 % of noon
            write_ramp_shot(shots, "2026-11-14_09-00-39")           # away
            write_ramp_shot(shots, "2026-11-14_09-00-56")           # into the sun: same structure

        fake_tree(FAKE_PASS, lines, [make])
        rc, text = run_main([FAKE_PASS, "--since", "2026-11-14 09:00",
                             "--sunset", "08a-sunset-facing-east-1905",
                             "08b-sunset-facing-west-1905"])
        self.assertEqual(rc, 0, text)
        self.assertIn("all brightness targets met", text)

    def test_a_run_with_no_night_stop_does_not_pass_quietly(self):
        """The gate half of "make it fail a run that misses": a tour that lost its 21:00
        stops has nothing to fail, printed a clean table, and exited 0 on the parked
        version. That is a report, not a gate."""
        lines = [
            "2026-11-14 09:00:00  tour start, 2 stops",
            "2026-11-14 09:00:05  shot 01 00-baseline-noon-grove",
            "2026-11-14 09:00:22  shot 02 14-vegetation-vinewood-noon",
            "2026-11-14 09:00:24  tour done",
        ]

        def make(shots):
            write_shot(shots, "2026-11-14_09-00-05", 128, 128)
            write_shot(shots, "2026-11-14_09-00-22", 120, 120)

        fake_tree(FAKE_NONIGHT, lines, [make])
        rc, text = run_main([FAKE_NONIGHT, "--since", "2026-11-14 09:00"])
        self.assertEqual(rc, 1)
        self.assertIn("no night stop", text)

    def test_same_stop_partner_beats_the_run_reference(self):
        # 10-water-santa-maria-noon is 171.7 in the real run; a night water stop must
        # be compared with IT, not with the 128 baseline.
        rows = [("10-water-santa-maria-noon", None, {"mean": 171.7}, None),
                ("00-baseline-noon-grove", None, {"mean": 128.0}, None)]
        partner, score = nb.best_partner("11-water-santa-maria-2100", rows)
        self.assertTrue(score)
        self.assertEqual(partner[0], "10-water-santa-maria-noon")


class TestShippedNumbers(unittest.TestCase):
    """The numbers the task asked for, in the files that ship them."""

    def read(self, rel):
        with open(os.path.join(REPO, rel), encoding="utf-8", errors="replace") as fh:
            return fh.read()

    def ini_section(self, ini, section):
        """The lines of one real section, header to next header. The header prose names
        every section in the file, so the search is line-anchored; a section written as
        ';[vehiclelights]' (documented but disabled) counts, and is returned with its
        ';' prefixes so a caller can tell an active key from a documented one."""
        lines = ini.splitlines()
        head = None
        for i, line in enumerate(lines):
            if re.match(r"^;?\[%s\]$" % re.escape(section), line.strip()):
                head = i
        self.assertIsNotNone(head, "the shipped ini has no [%s] section" % section)
        out = []
        for line in lines[head + 1:]:
            if re.match(r"^;?\[[a-z_]+\]$", line.strip()):
                break
            out.append(line)
        return "\n".join(out)

    def test_ini_lamp_and_bulb_numbers(self):
        ini = self.read("config/rhyremix-production.ini")
        # street lamps: one knob, 40 % of the flux, and the bands keep their ratios
        self.assertIn("radiance_scale=0.4", self.ini_section(ini, "streetlights"))
        self.assertIn("radiance_big=2.0", self.ini_section(ini, "streetlights"))
        # signal bulb: 1200 x 0.7 = 840, and the glow kept exactly as it is
        tl = self.ini_section(ini, "trafficlights")
        self.assertIn("bulb_radiance=840", tl)
        self.assertIn("glow_flux=304", tl)
        self.assertIn("glow_radius=3", tl)
        # the signals' own scale, so the street knob cannot take the glow down
        self.assertIn("radiance_scale=1.0", tl)
        # the vehicle layer's own scale, documented at the neutral value
        veh = self.ini_section(ini, "vehiclelights")
        self.assertIn(";radiance_scale=1.0", veh)

    def test_the_renderer_side_is_untouched(self):
        """n16's tonemap numbers and n22's night anchor are NOT this task's to move.
        n05's rtx.localtonemap.highlights = 1.5 must not come back on top of them."""
        for rel in ("config/rtx.conf", "config/rtx.conf.perf", "config/rtx.conf.nopops"):
            conf = self.read(rel)
            self.assertIn("rtx.tonemappingMode = 0", rel + "\n" + conf)
            self.assertIn("rtx.localtonemap.shadows = 1.0", rel + "\n" + conf)
            self.assertIn("rtx.localtonemap.highlights = 2.0", rel + "\n" + conf)
            self.assertNotIn("rtx.localtonemap.highlights = 1.5", rel + "\n" + conf)
        ini = self.read("config/rhyremix-production.ini")
        self.assertIn("night_ev=0.5", self.ini_section(ini, "tonemap"))
        self.assertIn("day_ev=-0.5", self.ini_section(ini, "tonemap"))

    def test_config_cpp_carries_the_defaults_and_the_clamps(self):
        cfg = self.read("plugin/src/config.cpp")
        # the compiled defaults are the shipped numbers, in the code AND in the ini the
        # plugin writes when no ini exists
        self.assertIn('iniFloat("radiance_scale", 0.4f)', cfg)
        self.assertIn('"radiance_scale=0.4\\r\\n"', cfg)
        self.assertIn('"bulb_radiance=840\\r\\n"', cfg)
        self.assertIn('iniFloatSec("trafficlights", "radiance_scale",\n                                           1.0f)', cfg)
        self.assertIn('iniFloatSec("vehiclelights", "radiance_scale", 1.0f)', cfg)
        # and both new keys are clamped the way the street one is
        self.assertIn("c.traffic_radiance_scale = 1.0f;  // 0 or garbage = the neutral multiplier", cfg)
        self.assertIn("c.traffic_radiance_scale > 100.0f", cfg)
        self.assertIn("c.veh_radiance_scale > 100.0f", cfg)

    def test_the_street_knob_no_longer_reaches_the_other_two_layers(self):
        tl = self.read("plugin/src/trafficlights.cpp")
        self.assertIn("c.traffic_radiance_scale", tl)
        self.assertNotIn("c.traffic_radius_ref, c.radiance_scale", tl)
        self.assertNotIn("kRhySignalGlowFluxCap, c.radiance_scale", tl)
        vc = self.read("plugin/src/vehiclecorona.cpp")
        self.assertIn("cfg->veh_radiance_scale", vc)
        self.assertNotIn("t.radianceScale = cfg->radiance_scale", vc)
        # the street lamps still ride the one knob this task moved
        sl = self.read("plugin/src/streetlights.cpp")
        self.assertIn("c.radiance_scale", sl)


if __name__ == "__main__":
    sys.path.insert(0, REPO)
    unittest.main(verbosity=2)
