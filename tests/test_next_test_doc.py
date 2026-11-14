#!/usr/bin/env python3
"""s26 gate: every command in docs/NEXT-TEST.md is a command that exists in this repo.

docs/NEXT-TEST.md is the page rhY runs the game from, so its failure mode is not "a sentence is
wrong", it is "the reader types something that does not exist". This test is the scripted half of
the s26 gate:

1. one screen - the task says one page, so the file is bounded in lines, bytes and columns;
2. every command line (fenced blocks, and inline `code` spans that start with a command word) is
   tokenised with shlex and checked stage by stage:
   * a repo path (scripts/, tools/, launcher/, plugin/, config/, docs/, tests/, deps/, data/,
     apply.sh, deploy.sh) must exist here, wherever it appears in the line;
   * a subcommand (`perf`, `on`, `restore`, ...) must appear in that script's own usage block, so
     a renamed or dropped action fails the doc;
   * a `--flag` must appear in that script's source, so `--tour` cannot drift from the script;
   * anything else must be on the small allowlist of external commands AND be on PATH;
3. structure: every step section has a command block and the three things the task asked for
   (Look / Report / Rollback), and the six subjects the task named are all present;
4. n03: the log fields the finished tasks produced - the s28 fade counters, the s29 clock counters -
   and the state of the character staging are required BY NAME, with the grep that produces them.
   s29's own notes left this page alone ("deliberately not touched") and the page quietly carried
   none of what s28/s29 fixed; a page rhY runs the session from must not be able to lose a check
   that way again. The tour script is checked, not copied: deploy.sh installs it now;
5. the doc must not tell the reader to start the game through launcher/launch-rtx.sh directly -
   the whole point of scripts/test-session.sh is that it frees the VRAM first and gives Strata
   back afterwards (docs/TESTING.md).

Placeholders (`$G`, `<start>`) and game paths are not repo paths and are not checked for
existence; they are only checked for shape.
"""
import os
import re
import shlex
import shutil
import sys
import unittest

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DOC = os.path.join(REPO, "docs", "NEXT-TEST.md")

# "one screen": a page you read without scrolling twice.
MAX_LINES = 90
MAX_BYTES = 6000
MAX_COLS = 132

INTERP = {"bash", "sh", "python3", "python", "env"}
EXTERNAL = {"bash", "sh", "python3", "python", "cp", "mv", "sed", "grep", "egrep", "tail", "head",
            "cat", "ls", "sha256sum", "git", "nvidia-smi", "printf", "echo", "rg", "diff", "tee"}
REPO_TOP = ("scripts/", "tools/", "launcher/", "plugin/", "config/", "docs/", "tests/", "deps/", "data/")
ROOT_SCRIPTS = ("apply.sh", "deploy.sh")

# the six subjects of the task, as the commands that exercise them
REQUIRED = [
    "scripts/test-session.sh --tour",
    "tools/tour-report.py",
    "source=list",                                    # the signal-heads rollback
    "tools/water.sh realistic",
    "tools/water.sh calm",
    "tools/water.sh pack",
    "tools/rtxprofile.sh perf",
    "tools/rtxprofile.sh restore",
    "tools/vegetation.sh on",
    "tools/vegetation.sh off",
    "tools/characters.sh on",
    "tools/characters.sh off",
]

USAGE_LINES = 45      # the header comment every switcher keeps its action list in

# n03: the log fields the finished tasks added and the page did not carry. s29's notes said
# docs/NEXT-TEST.md was "deliberately not touched", and after that the page carried none of the s28
# fade counters or the s29 clock counters - the reader was run from the page and never asked to look
# at the two things those tasks fixed. Each field is required BY NAME, so deleting half a line fails
# the same way as deleting the line. Long version and the meaning of every field: docs/TESTING.md.
REQUIRED_FIELDS = [
    # s28: the fade traffic on the streetlights/trafficlights lines (TESTING.md "fade counters")
    "steps=", "created=", "retired=", "deferred=", "player_gone_frames=",
    # s29: the clock counters on the sky: line (TESTING.md "clock counters")
    "tod=", "ms/min=", "torn=", "jitter=", "reanchor=", "wraps=", "cal: accepted=0", "cloud=",
    # n03 step 4: the characters step has to name the pack that IS staged and what it still wants
    "Criminal Peds Vary", "_criminal-peds-vary-extras", "PedFuncs", "ModelVariations",
    # n03 step 2: deploy.sh installs the tour script, so the page names the repo file it must match
    "plugin/lua/SARemix_Tour.lua",
]

# ...and the grep that produces them, so the fields cannot rot into a list of words nobody runs
REQUIRED_GREPS = ["grep -m2 'streetlights:'", "grep -m2 'sky: '"]


def read(path):
    with open(path, encoding="utf-8") as f:
        return f.read()


def fenced_blocks(src):
    return re.findall(r"```[^\n]*\n(.*?)```", src, re.S)


def inline_spans(src):
    """`code` spans outside fenced blocks - the rollbacks live there."""
    prose = re.sub(r"```.*?```", "", src, flags=re.S)
    return re.findall(r"`([^`\n]+)`", prose)


def repo_path_tokens(text):
    """Repo-relative paths named anywhere in `text`, not the ones hanging off $G/ or a game path."""
    out = []
    for m in re.finditer(r"(?<![\w/$..-])((?:%s)[\w./-]+)" % "|".join(t.rstrip("/") for t in REPO_TOP), text):
        p = m.group(1).rstrip(".,;:!?)]*'\"`")
        if p not in out:
            out.append(p)
    for name in ROOT_SCRIPTS:
        if re.search(r"(?<![\w/$.-])%s" % re.escape(name), text) and name not in out:
            out.append(name)
    return out


def stages(cmd):
    """One command line -> [[token, ...], ...], split on top-level pipes."""
    lex = shlex.shlex(cmd, posix=False, punctuation_chars=True)
    lex.whitespace_split = True
    try:
        toks = list(lex)
    except ValueError:
        return []
    out, cur = [], []
    for t in toks:
        if t in ("|", "||", ";", "&&"):
            if cur:
                out.append(cur)
            cur = []
        else:
            cur.append(t)
    if cur:
        out.append(cur)
    return out


def unquote(tok):
    if len(tok) >= 2 and tok[0] == tok[-1] and tok[0] in "'\"":
        return tok[1:-1]
    return tok


def looks_like_command(span):
    first = span.strip().split()[:1]
    if not first:
        return False
    w = first[0]
    return (w in INTERP or w in EXTERNAL or w.startswith(REPO_TOP) or w in ROOT_SCRIPTS)


class TestOneScreen(unittest.TestCase):
    def setUp(self):
        self.assertTrue(os.path.isfile(DOC), "docs/NEXT-TEST.md does not exist")
        self.src = read(DOC)
        self.lines = self.src.splitlines()

    def test_fits_on_one_screen(self):
        self.assertLessEqual(len(self.lines), MAX_LINES,
                             "docs/NEXT-TEST.md is %d lines, the task says one screen (<= %d)"
                             % (len(self.lines), MAX_LINES))
        self.assertLessEqual(len(self.src.encode()), MAX_BYTES,
                             "docs/NEXT-TEST.md is %d bytes (<= %d)" % (len(self.src.encode()), MAX_BYTES))
        wide = [(i + 1, len(l)) for i, l in enumerate(self.lines) if len(l) > MAX_COLS]
        self.assertEqual(wide, [], "lines wider than %d columns: %s" % (MAX_COLS, wide))

    def test_every_step_has_the_four_things(self):
        sections = re.split(r"\n## ", self.src)[1:]
        self.assertGreaterEqual(len(sections), 6, "expected at least 6 step sections")
        for sec in sections:
            title = sec.splitlines()[0]
            self.assertIn("```", sec, "%r has no command block" % title)
            # the preflight changes nothing, so it has no tour-report line; every real step does
            words = ("Look", "Rollback") if title.startswith("0.") else ("Look", "Report", "Rollback")
            for word in words:
                self.assertIn(word + ":", sec, "%r is missing '%s:'" % (title, word))

    def test_the_six_subjects_are_named(self):
        for needle in REQUIRED:
            self.assertIn(needle, self.src, "docs/NEXT-TEST.md never mentions %r" % needle)

    def test_the_log_fields_the_last_tasks_added_are_still_there(self):
        """n03: the page lost the s28 fade counters and the s29 clock counters by never gaining them.
        They are the in-game half of those two fixes, so they are required by name, and so is the
        grep that prints them."""
        missing = [f for f in REQUIRED_FIELDS if f not in self.src]
        self.assertEqual(missing, [], "docs/NEXT-TEST.md does not carry these: %s" % missing)
        missing = [g for g in REQUIRED_GREPS if g not in self.src]
        self.assertEqual(missing, [], "docs/NEXT-TEST.md names the fields but not the grep that "
                                     "produces them: %s" % missing)

    def test_the_tour_script_is_checked_not_copied(self):
        """n03: deploy.sh installs plugin/lua/SARemix_Tour.lua, so the page must verify the install
        instead of asking for a hand `cp`. That `cp` was the only thing ever keeping a game folder's
        tour script current, and it is how the 2026-11-12 00:49 session ran 13 stops while the repo
        had 16 (notes-s26 gap 1)."""
        self.assertNotIn("cp plugin/lua/SARemix_Tour.lua", self.src,
                         "the page still tells the reader to install the tour script by hand")
        self.assertIn("diff -q plugin/lua/SARemix_Tour.lua", self.src,
                      "the page does not check that the installed tour script matches the repo's")

    def test_the_characters_step_describes_the_real_stage_dir(self):
        """n03: the page said "nothing is staged" while the stage dir held Criminal Peds Vary. The
        step has to say what IS staged and what that pack still needs, or rhY reads it, types `on`,
        gets a refusal and learns nothing."""
        self.assertNotIn("nothing is staged", self.src,
                         "the page still claims the character stage dir is empty")
        self.assertIn("on` refuses", self.src,
                      "the page no longer says what `characters.sh on` does with a half-staged set")

    def test_it_does_not_send_the_reader_to_the_launcher_directly(self):
        # scripts/test-session.sh is the one that frees the VRAM and brings Strata back.
        self.assertNotIn("launcher/launch-rtx.sh", self.src)


class TestCommands(unittest.TestCase):
    def setUp(self):
        self.src = read(DOC)
        self.cmds = []
        for block in fenced_blocks(self.src):
            for line in block.splitlines():
                line = line.strip()
                if line and not line.startswith("#"):
                    self.cmds.append(line)
        for span in inline_spans(self.src):
            if looks_like_command(span):
                self.cmds.append(span.strip())
        self.assertGreaterEqual(len(self.cmds), 12,
                                "only %d command lines found - the extraction broke" % len(self.cmds))

    def usage(self, script):
        return "\n".join(read(os.path.join(REPO, script)).splitlines()[:USAGE_LINES])

    def test_repo_paths_named_in_the_doc_exist(self):
        missing = []
        for p in repo_path_tokens(self.src):
            if not os.path.exists(os.path.join(REPO, p)):
                missing.append(p)
        self.assertEqual(missing, [], "the doc points at paths that are not in the repo: %s" % missing)

    def test_every_command_resolves(self):
        unknown = []
        for cmd in self.cmds:
            for stage in stages(cmd):
                toks = [unquote(t) for t in stage]
                while toks and re.match(r"^[A-Za-z_][A-Za-z0-9_]*=", toks[0]):
                    toks.pop(0)                     # G=/media/... style assignments
                if not toks:
                    continue
                script = None
                for t in toks:
                    if t.startswith("-") or t in INTERP:
                        continue
                    script = t
                    break
                if script is None:
                    continue
                if script.startswith(REPO_TOP) or script in ROOT_SCRIPTS:
                    if not os.path.isfile(os.path.join(REPO, script)):
                        unknown.append("%s  [%s]" % (script, cmd))
                    continue
                if script in EXTERNAL:
                    if not shutil.which(script):
                        unknown.append("%s is not on PATH  [%s]" % (script, cmd))
                    continue
                if script.startswith(("/", "$", "<")) or "<" in script:
                    continue                        # game folder / placeholder, not ours
                unknown.append("%s is neither a repo script nor an allowed command  [%s]" % (script, cmd))
        self.assertEqual(unknown, [], "unresolvable commands in the doc:\n  " + "\n  ".join(unknown))

    def test_subcommands_are_real_actions_of_their_script(self):
        bad = []
        for cmd in self.cmds:
            for stage in stages(cmd):
                toks = [unquote(t) for t in stage]
                script = next((t for t in toks if not t.startswith("-") and t not in INTERP), None)
                if not script or not (script.startswith(REPO_TOP) or script in ROOT_SCRIPTS):
                    continue
                path = os.path.join(REPO, script)
                if not os.path.isfile(path):
                    continue                        # reported by test_every_command_resolves
                usage = self.usage(script)
                after = toks[toks.index(script) + 1:]
                prev_took_value = False
                for t in after:
                    if t.startswith("-"):
                        prev_took_value = False
                        continue
                    if t.startswith(("/", "$", "<")) or "/" in t or not re.match(r"^[a-z][a-z0-9_-]*$", t):
                        prev_took_value = False     # a path, a placeholder or a number
                        continue
                    if prev_took_value:             # the value of a flag that takes one
                        prev_took_value = False
                        continue
                    if not re.search(r"\b%s\b" % re.escape(t), usage):
                        bad.append("%s: '%s' is not in %s's usage block" % (cmd, t, script))
        self.assertEqual(bad, [], "\n  ".join(bad))

    def test_flags_exist_in_the_script_they_are_passed_to(self):
        bad = []
        for cmd in self.cmds:
            for stage in stages(cmd):
                toks = [unquote(t) for t in stage]
                script = next((t for t in toks if not t.startswith("-") and t not in INTERP), None)
                if not script or not (script.startswith(REPO_TOP) or script in ROOT_SCRIPTS):
                    continue
                path = os.path.join(REPO, script)
                if not os.path.isfile(path):
                    continue
                src = read(path)
                for t in toks:
                    if t.startswith("--") and t not in src:
                        bad.append("%s: %s never mentions %s" % (cmd, script, t))
        self.assertEqual(bad, [], "\n  ".join(bad))


if __name__ == "__main__":
    sys.path.insert(0, REPO)
    unittest.main(verbosity=2)
