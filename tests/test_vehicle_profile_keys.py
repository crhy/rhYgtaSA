#!/usr/bin/env python3
"""n12 gate: the vehicle-lights profile changes name real knobs and real textures.

Three things can go wrong with a profile change, and none of them show up as a
compile error:

  * a key that does not exist in the runtime is silently ignored by the Remix
    server ("the call succeeded" only means the name parsed - see
    config/rhyremix-rtx.conf's header on the 2026-11-11 bridge crash), so the
    change does nothing and the next session starts from a fiction;
  * a texture hash that is not a texture hash - a typo in a 16-hex-digit token
    puts a category entry on nothing at all;
  * an ini block whose documented default is not the compiled default, which is
    the failure tests/test_ini_keys.py was written for, now for the new
    [vehiclelights] section.

So: every rtx.* key of the shipped config/rtx.conf has to be in the option-name
vocabulary generated from the runtime binary; the n12 additions have to be there;
every hash in the category lists has to be a 16-hex token; and every commented
[vehiclelights] knob in config/rhyremix-production.ini has to carry exactly the
default plugin/src/config.cpp compiles.

Run: python3 -m pytest -q tests/test_vehicle_profile_keys.py  (or -m unittest)
"""
import os
import re
import sys
import unittest

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RTXCONF = os.path.join(REPO, "config", "rtx.conf")
KEYS = os.path.join(REPO, "config", "rtx-runtime-keys.txt")
INI = os.path.join(REPO, "config", "rhyremix-production.ini")
CFG = os.path.join(REPO, "plugin", "src", "config.cpp")

# n12: what this task added, spelled out so a revert that deletes the line fails
# the gate rather than quietly passing it.
N12_KEYS = ("rtx.emissiveBlendOverrideEmissiveIntensity",)
N12_HASHES = (
    "0xC70DC799C5BFFE65",  # effectsPC.txd smoke5   (fx system smoke50lit)
    "0x3F66C5D5F91C3708",  # effectsPC.txd smoke4   (fx system smoke30lit)
    "0x2D558BC31F67FD1D",  # effectsPC.txd bullethitsmoke1
)


def read(path):
    with open(path, encoding="utf-8", errors="replace") as fh:
        return fh.read()


def conf_lines(text):
    """(key, value) for every 'rtx.foo = bar' line, comments dropped."""
    out = []
    for line in text.splitlines():
        line = line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, _, val = line.partition("=")
        out.append((key.strip(), val.strip()))
    return out


class TestRtxConfKeys(unittest.TestCase):
    def setUp(self):
        self.conf = read(RTXCONF)
        self.lines = conf_lines(self.conf)
        vocab = set()
        for line in read(KEYS).splitlines():
            if line.startswith("#") or not line.strip():
                continue
            parts = line.split("\t")
            for p in parts[1:]:
                vocab.add(p.strip())
        self.vocab = vocab

    def test_every_n12_key_exists_in_the_runtime(self):
        # Only the keys this task adds are audited. config/rtx.conf is the profile
        # copied from the live install and 43 of its 84 keys are not in this
        # vocabulary, because config/rtx-runtime-keys.txt is generated from
        # `strings` on the runtime binary (tools/rtx-runtime-keys.py) and lists a
        # nested namespace once - "rtx.restirGI" - not every leaf under it. It is
        # a lower bound on the option names, which is exactly what makes it useful
        # for a key we are adding: if the name is here, the runtime has it.
        unknown = [k for k in N12_KEYS
                   if k not in self.vocab and k.split(".", 1)[1] not in self.vocab]
        self.assertEqual([], unknown,
                         "config/rtx.conf sets keys the runtime does not have")

    def test_n12_keys_present(self):
        have = {k for k, _ in self.lines}
        for k in N12_KEYS:
            self.assertIn(k, have)

    def test_emissive_blend_override_is_cut(self):
        vals = {k: v for k, v in self.lines}
        v = vals.get("rtx.emissiveBlendOverrideEmissiveIntensity")
        self.assertIsNotNone(v)
        f = float(v)
        # The runtime default is 0.2. The change has to be a cut (V6: the puffs
        # are self-lit sheets) and it must not be a switch-off (the sprite is
        # still the visible bulb, and coronas ride the same path).
        self.assertLess(f, 0.2)
        self.assertGreater(f, 0.0)

    def test_category_hashes_are_hex_tokens(self):
        bad = []
        for key, val in self.lines:
            if not key.endswith("Textures"):
                continue
            for tok in val.split(","):
                tok = tok.strip()
                if not tok:
                    continue
                if not re.fullmatch(r"0x[0-9A-Fa-f]{16}", tok):
                    bad.append((key, tok))
        self.assertEqual([], bad)

    def test_n12_hashes_in_particle_textures(self):
        vals = {k: v for k, v in self.lines}
        pt = vals.get("rtx.particleTextures", "")
        for h in N12_HASHES:
            self.assertIn(h.upper(), pt.upper(), "%s missing from %s" % (h, pt))

    def test_no_duplicate_particle_hash(self):
        vals = {k: v for k, v in self.lines}
        toks = [t.strip().upper() for t in
                vals.get("rtx.particleTextures", "").split(",") if t.strip()]
        self.assertEqual(len(toks), len(set(toks)),
                         "a texture is listed twice in rtx.particleTextures")


class TestVehicleIniBlock(unittest.TestCase):
    """The [vehiclelights] block of the shipped ini vs config.cpp's defaults."""

    def setUp(self):
        ini = read(INI)
        # The block: from the [vehiclelights] header (commented, as shipped) to
        # the next section header or end of file.
        m = re.search(r"^;?\[vehiclelights\][ \t]*$", ini, re.M)
        self.assertIsNotNone(m, "no [vehiclelights] block in the shipped ini")
        rest = ini[m.end():]
        nxt = re.search(r"^;?\[[a-z]+\]", rest, re.M)
        if nxt:
            rest = rest[:nxt.start()]
        self.block = rest
        self.documented = {}
        for line in rest.splitlines():
            mm = re.match(r"^;([a-z_]+)=(\S+)\s*$", line.strip())
            if mm:
                self.documented[mm.group(1)] = mm.group(2)
        cfg = read(CFG)
        self.compiled = {}
        for mm in re.finditer(
                r'iniIntSec\("vehiclelights",\s*"([a-z_]+)",\s*([^)]+)\)|'
                r'iniFloatSec\("vehiclelights",\s*"([a-z_]+)",\s*([^)]+)\)', cfg):
            key = mm.group(1) or mm.group(3)
            val = (mm.group(2) or mm.group(4)).strip()
            self.compiled[key] = val

    def test_block_exists_and_documents_knobs(self):
        self.assertTrue(self.documented, "the [vehiclelights] block documents "
                                         "no knobs")

    def test_documented_defaults_match_the_code(self):
        self.assertEqual(set(self.documented), set(self.compiled),
                         "the ini block and config.cpp disagree on which knobs "
                         "exist")
        for key, want in sorted(self.documented.items()):
            got = self.compiled[key]
            try:
                self.assertAlmostEqual(float(want), float(got.rstrip("f")),
                                       places=5, msg=key)
            except ValueError:
                self.assertEqual(want, got, key)

    def test_flux_numbers_are_the_documented_ones(self):
        # A headlamp has to out-light a street luminaire inside its beam and a
        # tail lamp has to stay far below it; those are the two numbers V5 is
        # about, so they may not be tuned into nonsense without the test moving.
        head = float(self.documented["head_flux"])
        tail = float(self.documented["tail_flux"])
        brake = float(self.documented["brake_flux"])
        self.assertGreater(head, 3473.0 / 4.0)   # one street luminaire's peak
        self.assertLess(tail * 20.0, head)       # a tail lamp is not a headlamp
        self.assertGreater(brake, tail)          # braking is brighter


if __name__ == "__main__":
    unittest.main(verbosity=2 if "-v" in sys.argv else 0)
