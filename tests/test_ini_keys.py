#!/usr/bin/env python3
"""n01 gate: the ini's documented knobs and the code's knobs cannot drift apart.

config/rhyremix-production.ini is the file deploy.sh installs, and plugin/src/config.cpp is
the only thing that reads it. Two failure modes this catches, both of which have happened in
this repo in some form:

  * a knob is renamed, merged or dropped in config.cpp and the ini keeps carrying it - the
    ini then silently does nothing, and the next debugging session starts from a fiction;
  * the ini documents a default that is not the default ("creates_per_frame=24" next to a
    compiled default of 40), so the number a reader tunes is not the number the code starts
    from.

So: every active key of the shipped ini has to be a key config.cpp actually asks the Windows
ini API for; every key the [clock] and [fade] documentation blocks name has to exist AND carry
the value config.cpp compiles; every [clock]/[fade] key config.cpp parses has to be documented
there; and the kIniDefaults string the plugin writes when no ini exists has to agree with the
same compiled defaults.

Run: python3 -m pytest -q tests/test_ini_keys.py   (or python3 -m unittest)
"""
import os
import re
import sys
import unittest

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
INI = os.path.join(REPO, "config", "rhyremix-production.ini")
CFG = os.path.join(REPO, "plugin", "src", "config.cpp")
HDR = os.path.join("plugin", "src", "config.h")

# The sections this repo documents in the shipped ini. [fade] is s28's ramp/budget layer plus
# n01's handle cadence; [clock] is s29's time axis; [shutdown] is n09's exit detection;
# [tonemap] is n22's sun-driven exposure anchor (its floors are in plugin/src/exposure.h).
DOCUMENTED = ("clock", "fade", "shutdown", "tonemap")

CALL = re.compile(r"\bini(Int|Float|String|U64|Rgb)(Sec)?\s*\(")
LIT = re.compile(r'"([^"]*)"')


def read(path):
    with open(path, encoding="utf-8", errors="replace") as f:
        return f.read()


def args_at(src, open_paren):
    """The text between '(' and its matching ')' - calls are not always followed by ';'"""
    depth = 0
    for i in range(open_paren, len(src)):
        if src[i] == "(":
            depth += 1
        elif src[i] == ")":
            depth -= 1
            if depth == 0:
                return src[open_paren + 1:i]
    return ""


def code_defaults():
    """{(section, key): default-literal} for every iniXxx() call in config.cpp.

    The section-less helpers (iniInt/iniFloat/iniString/iniU64/iniRgb) are wrappers around the
    "streetlights" section - see plugin/src/config.cpp - so that is what they map to. iniRgb has
    no default at all (it leaves the triple alone when the key is absent), so it maps to None.
    """
    src = read(CFG)
    out = {}
    for m in CALL.finditer(src):
        kind, is_sec = m.group(1), m.group(2)
        args = args_at(src, m.end() - 1)
        lits = LIT.findall(args)
        if is_sec:
            if len(lits) < 2:
                continue
            section, key = lits[0], lits[1]
        else:
            if not lits:
                continue
            section, key = "streetlights", lits[0]
        # the default is whatever follows the key literal (nothing, for iniRgb)
        after = args.split('"%s"' % key, 1)[1]
        default = after[1:].strip() if after.startswith(",") else ""
        if kind == "Rgb":
            default = None
        out.setdefault((section, key), default)
    return out


def ini_sections(path, commented=False):
    """Parse an ini into {section: {key: value}}.

    commented=True also picks up the documented-but-disabled lines: a section written as
    ';[fade]' with its keys as ';in_ms=700', which is how the shipped ini documents a default
    without changing it.
    """
    out, cur = {}, None
    for raw in read(path).splitlines():
        line = raw.strip()
        sec = key = None
        m = re.match(r"^\[([a-z_]+)\]$", line)
        if m:
            sec = m
        else:
            m = re.match(r"^([a-z_0-9A-Z]+)=(.*)$", line)
            if m:
                key = m
        if commented:
            m = re.match(r"^;\[([a-z_]+)\]$", line)
            if m:
                sec = m
            else:
                m = re.match(r"^;([a-z_0-9A-Z]+)=(.*)$", line)
                if m:
                    key = m
        if sec is not None:
            cur = sec.group(1)
            out.setdefault(cur, {})
            continue
        if key is not None and cur is not None:
            out[cur][key.group(1)] = key.group(2).strip()
    return out


def kinidefaults():
    """The [clock]/[fade] blocks of the kIniDefaults string the plugin writes itself."""
    src = read(CFG)
    m = re.search(r"static const char\* const kIniDefaults =(.*?);\n", src, re.S)
    assert m, "kIniDefaults not found in config.cpp"
    # The C source spells the newlines as the two-character escapes \r\n, so turn them
    # back into real line breaks before parsing.
    text = "".join(LIT.findall(m.group(1))).replace("\\r\\n", "\n")
    out, cur = {}, None
    for line in text.split("\n"):
        sec = re.match(r"^\[([a-z_]+)\]$", line.strip())
        key = re.match(r"^([a-z_0-9A-Z]+)=(.*)$", line.strip())
        if sec:
            cur = sec.group(1)
            out.setdefault(cur, {})
        elif key and cur is not None:
            out[cur][key.group(1)] = key.group(2).strip()
    return out


def num(lit):
    """A C++ default literal -> float, or None. Handles 2000.0f, 7ull, 1e-4f, (int)x."""
    s = lit.strip().strip('"')
    s = re.sub(r"(f|ull|u|ll)$", "", s)
    try:
        return float(s)
    except ValueError:
        return None


def same(inival, cdef):
    a, b = num(inival), num(cdef)
    if a is not None and b is not None:
        return abs(a - b) <= 1e-6 * max(1.0, abs(b))
    return inival.strip('"') == cdef.strip('"')


class TestIniKeys(unittest.TestCase):
    def setUp(self):
        self.code = code_defaults()
        self.ini = ini_sections(INI)
        self.ini_doc = ini_sections(INI, commented=True)

    def test_config_parses_what_the_shipped_ini_sets(self):
        missing = []
        for section, kv in self.ini.items():
            for key in kv:
                if (section, key) not in self.code:
                    missing.append("[%s] %s" % (section, key))
        self.assertEqual(missing, [],
                         "config.cpp never reads these keys of the shipped ini: "
                         + ", ".join(sorted(missing)))

    def test_documented_defaults_are_the_compiled_defaults(self):
        bad = []
        for section in DOCUMENTED:
            for key, val in self.ini_doc.get(section, {}).items():
                if self.code.get((section, key)) is None:
                    bad.append("[%s] %s=%s is documented but config.cpp has no such key"
                               % (section, key, val))
                elif not same(val, self.code[(section, key)]):
                    bad.append("[%s] %s: ini says %s, config.cpp defaults to %s"
                               % (section, key, val, self.code[(section, key)]))
        self.assertEqual(bad, [], "\n  ".join(bad))

    def test_every_fade_and_clock_knob_is_documented(self):
        missing = []
        for section in DOCUMENTED:
            documented = set(self.ini_doc.get(section, {}))
            for (sec, key) in self.code:
                if sec == section and key not in documented:
                    missing.append("[%s] %s" % (section, key))
        self.assertEqual(missing, [],
                         "config.cpp has knobs no reader is told about: "
                         + ", ".join(sorted(missing)))

    def test_plugin_written_defaults_match_the_code(self):
        kd = kinidefaults()
        bad = []
        for section in DOCUMENTED:
            for key, val in kd.get(section, {}).items():
                if self.code.get((section, key)) is None:
                    bad.append("[%s] %s=%s is written by kIniDefaults but never read"
                               % (section, key, val))
                elif not same(val, self.code[(section, key)]):
                    bad.append("[%s] %s: kIniDefaults writes %s, config.cpp defaults to %s"
                               % (section, key, val, self.code[(section, key)]))
        self.assertEqual(bad, [], "\n  ".join(bad))

    def test_the_new_cadence_knobs_are_all_there(self):
        # n01's four knobs are the point of the [fade] block; assert them by name so a
        # reformat of the ini cannot quietly drop them.
        for key in ("handle_cadence_ms", "handle_cadence_min_ms", "handle_step_deg",
                    "handle_rel_lum"):
            self.assertIn(("fade", key), self.code, "config.cpp has no [fade] %s" % key)
            self.assertIn(key, self.ini_doc.get("fade", {}),
                          "config/rhyremix-production.ini does not document %s" % key)
            self.assertIn(key, kinidefaults().get("fade", {}),
                          "kIniDefaults does not write %s" % key)

    def test_the_tonemap_anchor_knobs_are_all_there(self):
        # n22: the [tonemap] block is the undo switch plus the curve's endpoints. If
        # any of these disappears the feature becomes un-disableable, which is the
        # one thing an unmeasured runtime write to the user layer must not be.
        for key in ("apply", "day_ev", "night_ev", "dusk_deg", "dawn_deg",
                    "max_ev_per_sec", "min_ev", "max_ev"):
            self.assertIn(("tonemap", key), self.code, "config.cpp has no [tonemap] %s" % key)
            self.assertIn(key, self.ini_doc.get("tonemap", {}),
                          "config/rhyremix-production.ini does not document %s" % key)
            self.assertIn(key, kinidefaults().get("tonemap", {}),
                          "kIniDefaults does not write %s" % key)

    def test_the_n23_layer_scales_are_parsed_and_clamped(self):
        """n23 (finding C): [streetlights] radiance_scale went 1.0 -> 0.4, and the two
        layers that knob used to reach through (the signal bulb/glow and the vehicle
        lamps) got radiance_scale keys of their own.

        Three ways this rots, all of them silent: a key in the ini that config.cpp never
        asks for (the line does nothing and the next debugging session starts from a
        fiction); a scale with no clamp (a 0 or negative value reaches Remix as a dark or
        inverted light instead of the neutral multiplier); and an ini number that is not
        the compiled default, so a plugin running without an ini looks different from the
        one deploy.sh installs."""
        for section in ("streetlights", "trafficlights", "vehiclelights"):
            self.assertIn((section, "radiance_scale"), self.code,
                          "config.cpp never reads [%s] radiance_scale" % section)
        # the shipped ini carries the numbers n23 ships, and they ARE the compiled defaults
        self.assertEqual(self.ini["streetlights"]["radiance_scale"], "0.4")
        self.assertTrue(same(self.ini["streetlights"]["radiance_scale"],
                             self.code[("streetlights", "radiance_scale")]),
                        "[streetlights] radiance_scale: ini %s, code %s"
                        % (self.ini["streetlights"]["radiance_scale"],
                           self.code[("streetlights", "radiance_scale")]))
        self.assertEqual(self.ini["trafficlights"]["radiance_scale"], "1.0")
        self.assertTrue(same(self.ini["trafficlights"]["radiance_scale"],
                             self.code[("trafficlights", "radiance_scale")]))
        # the vehicle section is documented-but-disabled, so it is read as a comment line
        self.assertEqual(self.ini_doc["vehiclelights"]["radiance_scale"], "1.0")
        self.assertTrue(same(self.ini_doc["vehiclelights"]["radiance_scale"],
                             self.code[("vehiclelights", "radiance_scale")]))
        # and every one of the three is clamped: >0 or the neutral value, and a ceiling
        cfg = read(CFG)
        for var, neutral in (("c.radiance_scale", "0.4f"),
                             ("c.traffic_radiance_scale", "1.0f"),
                             ("c.veh_radiance_scale", "1.0f")):
            self.assertIn("!(%s > 0.0f)" % var, cfg,
                          "%s is not guarded against 0 or negative" % var)
            self.assertIn("%s = %s;" % (var, neutral), cfg,
                          "%s does not fall back to %s" % (var, neutral))
            self.assertIn("%s > 100.0f" % var, cfg, "%s has no ceiling" % var)
        # the bulb's take-down lives in bulb_radiance, so the glow cannot inherit it
        self.assertEqual(self.ini["trafficlights"]["bulb_radiance"], "840")
        self.assertEqual(self.ini["trafficlights"]["glow_flux"], "304")

    def test_the_day_anchor_equals_the_profile_anchor(self):
        """n22's no-op-at-noon property, checked where both sides are visible.

        config/rtx.conf carries rtx.tonemap.exposureBias = -0.5 (n16). The plugin's
        [tonemap] day_ev must be the SAME number, or the plugin would move the
        daytime grade under a run that thinks it is measuring n16.
        """
        conf = read(os.path.join(REPO, "config", "rtx.conf"))
        m = re.search(r"^\s*rtx\.tonemap\.exposureBias\s*=\s*(-?[0-9.]+)\s*$",
                      conf, re.M)
        self.assertIsNotNone(m, "config/rtx.conf no longer sets rtx.tonemap.exposureBias")
        self.assertAlmostEqual(num(self.code[("tonemap", "day_ev")]), float(m.group(1)),
                               msg="[tonemap] day_ev must equal rtx.conf's static anchor")

    def test_cadence_bounds_are_ordered_in_the_code(self):
        # handle_cadence_min_ms must stay below handle_cadence_ms, and the direction budget
        # must not be looser than the body's own [sun] step_deg, or the cadence is just the
        # old rule with a new name.
        self.assertLess(num(self.code[("fade", "handle_cadence_min_ms")]),
                        num(self.code[("fade", "handle_cadence_ms")]))
        self.assertLessEqual(num(self.code[("fade", "handle_step_deg")]),
                             num(self.code[("sun", "step_deg")]))

    def test_config_h_declares_what_config_cpp_parses(self):
        hdr = read(os.path.join(REPO, HDR))
        missing = [k for k in ("handle_cadence_ms", "handle_cadence_min_ms", "handle_step_deg",
                               "handle_rel_lum", "clock_ms_per_game_minute", "clock_jitter_min",
                               "clock_reanchor_ms", "clock_calendar_stable_reads",
                               "tonemap_apply", "tonemap_day_ev", "tonemap_night_ev",
                               "tonemap_dusk_deg", "tonemap_dawn_deg",
                               "tonemap_max_ev_per_sec", "tonemap_min_ev", "tonemap_max_ev")
                   if not re.search(r"\b%s\b" % k, hdr)]
        self.assertEqual(missing, [], "config.h does not declare: " + ", ".join(missing))


if __name__ == "__main__":
    sys.path.insert(0, REPO)
    unittest.main(verbosity=2)
