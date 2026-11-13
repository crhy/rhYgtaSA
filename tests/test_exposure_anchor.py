#!/usr/bin/env python3
"""n22 gate: the sun-driven exposure anchor is wired, bounded, named, and documented.

The C++ side (tests/exposure_check.cpp, tests/smooth_check.cpp) proves the curve and the
rate bound. This file proves the things a C++ test cannot see, and every one of them has a
failure mode this repo has already been bitten by once:

1. the key the plugin pushes is an option this runtime actually has - checked against
   config/rtx-options-1.6.1.tsv, the runtime's own table (tools/rtxkeycheck.py's strings
   check would happily accept rtx.tonemap.exposureBiasX if the category and the leaf both
   appear in d3d9.dll, which is exactly how n16's task-file key would have done nothing);
2. the plugin really pushes it, from the SUN ELEVATION and not from the clock;
3. the plugin never writes the player's own slider (rtx.userBrightness /
   rtx.userBrightnessEVRange - doubled names, RtxOptionFlags::UserSetting, user.conf sits
   above rtx.conf) and never switches rtx.tonemappingMode at runtime;
4. the rate bound in the shipped ini is inside the floors compiled into exposure.cpp, and
   those floors are inside the runtime's own +/-4 EV slider span;
5. the native test is actually in the gate (a test nobody runs is a comment);
6. the docs say what is measured and what is owed: the after-numbers come from a live run,
   because rule 1 of every task here forbids running the game.

Run: python3 -m pytest -q tests/test_exposure_anchor.py   (scripts/check-all.sh runs it)
"""
import os
import re
import sys
import unittest

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, "tools"))

from importlib import import_module  # noqa: E402

_audit = import_module("rtx-option-audit")  # the module name has a dash

TABLE = os.path.join(REPO, "config", "rtx-options-1.6.1.tsv")
INI = os.path.join(REPO, "config", "rhyremix-production.ini")
CONF = os.path.join(REPO, "config", "rtx.conf")
EXPO_H = os.path.join(REPO, "plugin", "src", "exposure.h")
EXPO_CPP = os.path.join(REPO, "plugin", "src", "exposure.cpp")
SUN = os.path.join(REPO, "plugin", "src", "sun.cpp")
RTXCONF = os.path.join(REPO, "plugin", "src", "rtxconf.cpp")
CHECKALL = os.path.join(REPO, "scripts", "check-all.sh")
CHANGELOG = os.path.join(REPO, "CHANGELOG.md")
FINDINGS = os.path.join(REPO, "docs", "FINDINGS.md")
TARGET = os.path.join(REPO, "docs", "TARGET.md")
NEXTTEST = os.path.join(REPO, "docs", "NEXT-TEST.md")

KEY = "rtx.tonemap.exposureBias"

# names the plugin must never send at runtime, whatever else it does
FORBIDDEN = (
    "rtx.userBrightness",           # really rtx.userBrightness.userBrightness
    "rtx.userBrightnessEVRange",    # really …EVRange.userBrightnessEVRange
    "rtx.tonemappingMode",          # switching mode re-arms the local pyramid
    "rtx.postfx.exposureFraction",  # a motion-blur shutter, not an exposure
)


def read(path):
    with open(path, encoding="utf-8", errors="replace") as fh:
        return fh.read()


def ini_section(path, section):
    out, cur = {}, None
    for raw in read(path).splitlines():
        line = raw.strip()
        m = re.match(r"^\[([a-z_]+)\]$", line)
        if m:
            cur = m.group(1)
            continue
        m = re.match(r"^([a-z_0-9A-Z]+)=(.*)$", line)
        if m and cur == section:
            out[m.group(1)] = m.group(2).strip()
    return out


def rtx_set_keys(src):
    """Every rtx.* key literal handed to rhyRtxSet / rhyRtxSetFloat in this source."""
    return set(re.findall(r"\brhyRtxSet(?:Float)?\s*\(\s*\"(rtx\.[A-Za-z0-9_.]+)\"", src))


def constant(src, name):
    m = re.search(r"\b%s\s*=\s*(-?[0-9.]+)f?\s*;" % re.escape(name), src)
    assert m, "%s not found" % name
    return float(m.group(1))


class OptionName(unittest.TestCase):
    def test_the_key_is_a_real_option(self):
        opts = _audit.load_table(TABLE)
        self.assertIn(KEY, opts, "%s is not an option of the runtime we ship" % KEY)
        # float, and the runtime defaults it to 0 (so -0.5 is OUR number, not NVIDIA's)
        self.assertEqual(opts[KEY]["type"].lower(), "float",
                         "%s is a %s, and the plugin formats a float" % (KEY, opts[KEY]["type"]))
        self.assertAlmostEqual(float(opts[KEY]["default"]), 0.0, places=6)

    def test_the_wrong_names_are_still_wrong(self):
        """Negative control: the doubled names must stay absent, or the check is vacuous."""
        opts = _audit.load_table(TABLE)
        for bad in ("rtx.userBrightness", "rtx.userBrightnessEVRange",
                    "rtx.tonemap.tonemappingMode"):
            self.assertNotIn(bad, opts, "%s suddenly IS an option - re-read the audit" % bad)
        for real in ("rtx.userBrightness.userBrightness",
                     "rtx.userBrightnessEVRange.userBrightnessEVRange",
                     "rtx.tonemappingMode"):
            self.assertIn(real, opts, "the table lost %s - it was regenerated wrong" % real)


class Wiring(unittest.TestCase):
    def test_the_plugin_pushes_the_key(self):
        sent = rtx_set_keys(read(SUN))
        self.assertIn(KEY, sent,
                      "sun.cpp no longer pushes %s: the curve exists but nobody sends it" % KEY)

    def test_the_input_is_the_sun_not_the_clock(self):
        sun = read(SUN)
        self.assertRegex(sun, r"rhyExposureAnchorStep\(\s*g_exposure,\s*p,\s*g_sunElev,\s*dt\s*\)",
                         "the exposure follower must be driven by g_sunElev (solar elevation)")
        self.assertNotRegex(sun, r"rhyExposureAnchorStep\([^)]*(hour|minute|g_tod)",
                            "the exposure follower must not read the clock")
        expo = read(EXPO_CPP)
        self.assertIn("skyDaylightFactor", expo,
                      "exposure.cpp must blend on solar.cpp's elevation curve, not its own")

    def test_it_never_touches_the_players_slider_or_the_mode(self):
        offenders = []
        for path in (SUN, RTXCONF, EXPO_CPP, os.path.join(REPO, "plugin", "src", "config.cpp")):
            for key in rtx_set_keys(read(path)):
                if key in FORBIDDEN:
                    offenders.append("%s: %s" % (os.path.basename(path), key))
        self.assertEqual(offenders, [],
                         "the plugin sends keys it must not send: " + ", ".join(offenders))

    def test_the_undo_switch_exists_and_is_honoured(self):
        sun = read(SUN)
        self.assertRegex(sun, r"if\s*\(\s*!\s*c\.tonemap_apply\s*\)",
                         "[tonemap] apply=0 must stop the push (that line is the undo)")

    def test_the_probe_list_carries_the_key(self):
        """A live run with [sky] probe=1 should record the name we now depend on."""
        self.assertIn('{"%s"' % KEY, read(RTXCONF),
                      "%s is not in the probe list" % KEY)


class Bounds(unittest.TestCase):
    def setUp(self):
        self.ini = ini_section(INI, "tonemap")
        self.src = read(EXPO_CPP)

    def test_ini_is_inside_the_compiled_floors(self):
        default = constant(self.src, "kDefaultEvPerSec")
        lo = constant(self.src, "kMinEvPerSec")
        hi = constant(self.src, "kMaxEvPerSec")
        self.assertLessEqual(lo, default)
        self.assertLessEqual(default, hi)
        self.assertLessEqual(hi, 0.25, "the ceiling itself must stay a ramp, not a pop")
        asked = float(self.ini["max_ev_per_sec"])
        self.assertLessEqual(asked, 0.05,
                             "the shipped bound is the task's 0.05 EV/s (>= 20 s for the swing)")
        self.assertLessEqual(lo, asked)
        self.assertLessEqual(asked, hi)

    def test_the_ev_clamp_is_inside_the_runtime_slider(self):
        ui_lo = constant(self.src, "kUiMinEv")
        ui_hi = constant(self.src, "kUiMaxEv")
        hard_lo = constant(self.src, "kHardMinEv")
        hard_hi = constant(self.src, "kHardMaxEv")
        self.assertEqual((ui_lo, ui_hi), (-4.0, 4.0),
                         "the Remix UI's span is DragFloat(..., -4, 4): keep it literal")
        self.assertLessEqual(hard_lo, ui_lo)
        self.assertLessEqual(ui_hi, hard_hi)
        self.assertLessEqual(abs(hard_hi), 8.0)
        self.assertLessEqual(float(self.ini["min_ev"]), ui_lo + 1e-9)
        self.assertLessEqual(ui_hi - 1e-9, float(self.ini["max_ev"]))

    def test_the_shipped_anchors_are_the_shipped_numbers(self):
        self.assertAlmostEqual(float(self.ini["day_ev"]), -0.5)
        self.assertAlmostEqual(float(self.ini["night_ev"]), 0.5)
        # the swing has to stay a small move inside the UI's span, per the task
        self.assertLessEqual(abs(float(self.ini["night_ev"]) - float(self.ini["day_ev"])), 1.0)

    def test_the_band_matches_the_sky_module(self):
        """The ambient and the exposure must agree about when twilight is."""
        sky = ini_section(INI, "sky")
        self.assertEqual(self.ini["dusk_deg"], sky["dusk_deg"])
        self.assertEqual(self.ini["dawn_deg"], sky["dawn_deg"])


class GateAndDocs(unittest.TestCase):
    def test_no_doc_promises_a_command_that_does_not_exist(self):
        """n22 found rtxconf.h promising `rhyremix --reset-rtx`, which is nowhere in the tree.

        An undo that does not exist is worse than no undo: the next person runs it, gets nothing,
        and concludes the layer is not the problem. The name may appear exactly once in the repo,
        in the comment that says it does not exist.
        """
        hits = []
        for root in ("docs", "plugin", "config", "scripts", "tools", "launcher"):
            for dirpath, dirnames, filenames in os.walk(os.path.join(REPO, root)):
                dirnames[:] = [d for d in dirnames if d != "__pycache__"]
                for fn in filenames:
                    p = os.path.join(dirpath, fn)
                    if "--reset-rtx" in read(p):
                        hits.append(os.path.relpath(p, REPO))
        for extra in ("README.md",):
            if "--reset-rtx" in read(os.path.join(REPO, extra)):
                hits.append(extra)
        # The change history may name it (it records the fix); nothing a reader follows may.
        allowed = {os.path.join("plugin", "src", "rtxconf.h"), "CHANGELOG.md"}
        bad = [h for h in hits if h not in allowed]
        self.assertEqual(bad, [],
                         "`--reset-rtx` is named as if it were a command in: %s" % bad)
        hdr = read(os.path.join(REPO, "plugin", "src", "rtxconf.h"))
        self.assertRegex(hdr, r"--reset-rtx[^\n]*does not exist",
                         "the one place that names it must say it does not exist")

    def test_the_native_test_is_in_the_gate(self):
        sh = read(CHECKALL)
        self.assertIn("tests/exposure_check.cpp", sh,
                      "exposure_check.cpp is not run by scripts/check-all.sh")
        self.assertIn("plugin/src/exposure.cpp", sh,
                      "check-all.sh does not compile exposure.cpp into the native test")

    def test_the_docs_name_the_change_and_what_is_owed(self):
        log = read(CHANGELOG)
        m = re.search(r"(?m)^### .*n22.*$", log)
        self.assertIsNotNone(m, "CHANGELOG.md has no n22 section")
        heading = m.group(0)
        tail = log[m.end():]
        stop = re.search(r"(?m)^#{2,3} ", tail)
        n22 = heading + (tail[:stop.start()] if stop else tail)
        for part in ("exposureBias", "tonemap"):
            self.assertIn(part, n22, "the n22 CHANGELOG section does not mention " + part)
        self.assertRegex(n22, r"`[0-9a-f]{7,40}`", "the n22 section names no commit")

        fin = read(FINDINGS)
        self.assertIn("n22", fin, "docs/FINDINGS.md does not cover n22")
        self.assertIn(KEY, fin, "docs/FINDINGS.md does not name " + KEY)

        tgt = read(TARGET)
        self.assertIn("[tonemap]", tgt, "docs/TARGET.md §6 does not give the [tonemap] protocol")
        self.assertIn("night_ev", tgt, "docs/TARGET.md §6 does not name the knob to argue with")

        nxt = read(NEXTTEST)
        self.assertIn("tonemap", nxt, "docs/NEXT-TEST.md does not tell rhY what to test")

    def test_the_docs_say_the_after_numbers_come_from_a_live_run(self):
        """Rule 1 forbids rendering here, so the docs must not imply we measured it."""
        for path in (FINDINGS, TARGET, CHANGELOG):
            text = read(path)
            self.assertRegex(text, r"(?i)live run",
                             "%s never says the numbers come from a live run" % path)


if __name__ == "__main__":
    unittest.main()
