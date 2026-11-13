#!/usr/bin/env python3
"""n14: every commit since 2026-11-11 must be named in CHANGELOG.md.

Why this exists: rhY's rule (2026-11-12) is "document EVERYTHING", and the n14 sweep found
that five finished tasks (s28, s31, s32, s27, n12) had no CHANGELOG entry at all and that no
commit after `e19fde6` was named by hash. A doc sweep is only durable if something re-checks
it, so this file is the scripted half of the n14 gate: it walks `git log` over **every ref**
(main, the `failed/*` parked branches and the `wip/*` aborted one) and requires each commit's
short hash to appear in CHANGELOG.md.

It deliberately checks hashes rather than task names: task slugs are a convention of the local
task queue, hashes are the repository's own identifiers, and a parked branch is exactly the
case where a "task name" grep would have been quietly satisfied by the retry that replaced it.

Run: python3 -m pytest -q tests/test_changelog_coverage.py   (scripts/check-all.sh runs it)
"""
import os
import re
import subprocess
import unittest

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CHANGELOG = os.path.join(REPO, "CHANGELOG.md")
STATUS = os.path.join(REPO, "docs", "STATUS.md")

# The date the documentation rule starts from (task n14: "everything since 2026-11-11").
SINCE = "2026-11-11"

# Commits that are documentation-only noise for a reader (nothing to describe beyond the
# message itself) still have to be named - the point of the check is that the changelog is
# the whole record, not a summary of the interesting parts.


def git(*args):
    return subprocess.run(["git", "-C", REPO] + list(args), capture_output=True,
                          text=True, check=True).stdout


def commits_since():
    """(short hash, subject) for every commit reachable from any ref since SINCE."""
    out = git("log", "--all", "--since=" + SINCE, "--pretty=%h%x1f%s")
    rows = []
    for line in out.splitlines():
        if "\x1f" not in line:
            continue
        h, subj = line.split("\x1f", 1)
        rows.append((h.strip(), subj.strip()))
    return rows


class ChangelogCoverage(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not os.path.isdir(os.path.join(REPO, ".git")):
            raise unittest.SkipTest("not a git checkout")
        cls.rows = commits_since()
        with open(CHANGELOG, encoding="utf-8") as fh:
            cls.text = fh.read()

    def test_git_log_found_commits(self):
        self.assertGreater(len(self.rows), 10,
                           "git log --all --since=%s found almost nothing" % SINCE)

    def test_every_commit_is_named(self):
        missing = [h + " " + s for h, s in self.rows if h not in self.text]
        self.assertEqual(missing, [],
                         "CHANGELOG.md does not name these commits:\n  " + "\n  ".join(missing))

    def test_parked_and_aborted_commits_are_named(self):
        """The failed/* and wip/* branches are the easy ones to forget."""
        refs = git("for-each-ref", "--format=%(refname:short)",
                   "refs/heads/failed", "refs/heads/wip").split()
        self.assertGreaterEqual(len(refs), 3, "expected the parked branches to still exist")
        missing = []
        for ref in refs:
            for line in git("log", "-1", "--pretty=%h%x1f%s", ref).splitlines():
                h, subj = line.split("\x1f", 1)
                if h not in self.text:
                    missing.append("%s (%s): %s" % (h, ref, subj))
        self.assertEqual(missing, [],
                         "parked/aborted work missing from CHANGELOG.md:\n  " + "\n  ".join(missing))

    def test_every_unreleased_section_names_a_commit(self):
        """A section in the unreleased block that names no commit cannot be checked."""
        start = self.text.index("## [Unreleased]")
        end = self.text.index("## [0.8.0]")
        body = self.text[start:end]
        parts = re.split(r"(?m)^### ", body)[1:]
        bad = [p.splitlines()[0] for p in parts if not re.search(r"`[0-9a-f]{7,40}`", p)]
        self.assertEqual(bad, [],
                         "sections naming no commit anywhere:\n  " + "\n  ".join(bad))


class StatusDoc(unittest.TestCase):
    """docs/STATUS.md is the public answer to 'what is still open, and who owns it'."""

    def test_exists_and_names_the_parked_branches(self):
        self.assertTrue(os.path.isfile(STATUS), "docs/STATUS.md is missing")
        with open(STATUS, encoding="utf-8") as fh:
            text = fh.read()
        for ref in git("for-each-ref", "--format=%(refname:short)",
                       "refs/heads/failed", "refs/heads/wip").split():
            self.assertIn(ref, text, "docs/STATUS.md does not mention " + ref)

    def test_names_the_test_sessions_it_summarises(self):
        with open(STATUS, encoding="utf-8") as fh:
            text = fh.read()
        for stamp in ("2026-11-12 00:48", "2026-11-12 10:22", "2026-11-12 23:42"):
            self.assertIn(stamp, text, "docs/STATUS.md does not name the " + stamp + " session")


if __name__ == "__main__":
    unittest.main()
