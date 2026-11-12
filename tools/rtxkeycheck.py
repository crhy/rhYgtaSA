#!/usr/bin/env python3
"""Check every key of an rtx.conf-style file against the runtime's own name vocabulary.

Usage:
    tools/rtxkeycheck.py [--keys config/rtx-runtime-keys.txt] [--quiet] FILE [FILE...]

Exit 0 when every key in every file is known to the runtime, 1 otherwise. The vocabulary
comes from tools/rtx-runtime-keys.py (i.e. from `strings` on <game>/.trex/d3d9.dll), so this
is the offline version of what the bridge would say in remix-dxvk.log: an unknown key in
rtx.conf is not an error, it is simply ignored by the renderer - which is how a "tuned"
setting ends up doing nothing for a week.

Verdicts, strongest first:
    dotted         the full "rtx.a.b" path appears in the runtime's strings
    leaf+category  the category "rtx.a" and the name "b" both appear (how nested options are
                   stored: name in the option table, category as its own line)
    leaf           only the bare name appears - fine for top-level "rtx.b" keys, weaker below
"""
import argparse
import re
import sys

KEYRE = re.compile(r"^\s*(rtx\.[A-Za-z0-9_.]+)\s*=")


def load_keys(path):
    dotted, leaves = set(), set()
    with open(path, "r") as fh:
        for line in fh:
            if line.startswith("#"):
                continue
            kind, _, name = line.rstrip("\n").partition("\t")
            if kind == "dotted":
                dotted.add(name)
            elif kind == "leaf":
                leaves.add(name)
    return dotted, leaves


def verdict(key, dotted, leaves):
    if key in dotted:
        return "dotted", ""
    parts = key.split(".")
    leaf = parts[-1]
    parent = ".".join(parts[:-1])
    if leaf in leaves:
        if len(parts) == 2:
            return "leaf", ""
        if parent in dotted:
            return "leaf+category", ""
        return "leaf", "category %s not seen" % parent
    return None, "no %r in the runtime's strings" % leaf


def check_file(path, dotted, leaves, quiet):
    bad = 0
    seen = []
    with open(path, "r") as fh:
        for n, line in enumerate(fh, 1):
            m = KEYRE.match(line)
            if not m:
                continue
            key = m.group(1)
            kind, why = verdict(key, dotted, leaves)
            seen.append((key, kind, why))
            if kind is None:
                bad += 1
                print("FAIL %s:%d %s  (%s)" % (path, n, key, why))
            elif not quiet:
                print("ok   %-52s %s%s" % (key, kind, (" - " + why) if why else ""))
    return bad, seen


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--keys", default="config/rtx-runtime-keys.txt")
    ap.add_argument("--quiet", action="store_true")
    ap.add_argument("files", nargs="+")
    a = ap.parse_args()
    dotted, leaves = load_keys(a.keys)
    total = 0
    for f in a.files:
        bad, seen = check_file(f, dotted, leaves, a.quiet)
        print("%s: %d keys, %d unknown" % (f, len(seen), bad))
        total += bad
    if total:
        print("UNKNOWN KEYS: %d" % total)
        return 1
    print("all keys known to this runtime")
    return 0


if __name__ == "__main__":
    sys.exit(main())
