#!/usr/bin/env python3
"""Fill the slots deps/manifest.json reserves for a release bundle (task s14 step 3).

The reader half of the manifest lives inside tools/fetch-deps.sh and there must
stay exactly one of those. This is the writer half, used by tools/make-release.sh
after it has hashed the files it collected: for every entry the bundle hosts it
sets `sha256` and `size` from the real bytes and points the `magnet` url at the
bundle torrent. The https `mirror` slot is deliberately left TBD - there is no
web host yet, the magnet is what actually exists.

It edits the *text*, not a reparsed-and-redumped document: deps/manifest.json is
hand-written, ordered so a human can read the licence column, and a json.dump
round trip would reflow all 25 entries into an unreviewable diff. So the entry
spans are found with a brace/string scanner, only the three values are touched,
and the result must parse as JSON before the file is replaced (and make-release.sh
runs the reader over it afterwards).

    usage: manifest-update.py MANIFEST --magnet MAGNET [--records FILE]

records: one per line, id \x1f name \x1f size \x1f sha256 \x1f redistribute \x1f source
(the same records tools/make-release.sh collected); "-" or omitted = stdin.

exit: 0 updated (or nothing to do), 1 the update would produce invalid JSON or an
entry is missing, 2 usage.
"""
import argparse
import json
import os
import re
import sys


def match_span(text, i):
    """text[i] is '{' or '[' -> index just past its match (string-aware)."""
    depth, instr, esc = 0, False, False
    while i < len(text):
        c = text[i]
        if instr:
            if esc:
                esc = False
            elif c == "\\":
                esc = True
            elif c == '"':
                instr = False
        elif c == '"':
            instr = True
        elif c in "[{":
            depth += 1
        elif c in "]}":
            depth -= 1
            if depth == 0:
                return i + 1
        i += 1
    raise ValueError("unbalanced brackets at offset %d" % i)


def files_array(text):
    m = re.search(r"\"files\"\s*:\s*\[", text)
    if not m:
        raise ValueError('no "files" array')
    start = m.end() - 1
    return start, match_span(text, start)


def entry_spans(text):
    """[(start, end)] of every object inside the top-level files array."""
    a, end = files_array(text)
    out, pos = [], a + 1
    while pos < end - 1:
        while pos < end - 1 and (text[pos].isspace() or text[pos] == ","):
            pos += 1
        if pos >= end - 1:
            break
        if text[pos] != "{":
            raise ValueError("expected an entry object at offset %d" % pos)
        o = match_span(text, pos)
        out.append((pos, o))
        pos = o
    return out


SCALAR = re.compile(r"null|true|false|-?\d+(?:\.\d+)?|\"(?:\\.|[^\"\\])*\"")


def value_span(block, key):
    """(start, end) of the *value* of "key" inside one entry object. Brackets are
    matched, scalars are taken as one token - a value is not always an object."""
    m = re.search(r"\"%s\"\s*:\s*" % re.escape(key), block)
    if not m:
        return None
    i = m.end()
    if i < len(block) and block[i] in "{[":
        return i, match_span(block, i)
    s = SCALAR.match(block, i)
    if not s:
        return None
    return i, s.end()


def set_scalar(block, key, new_text):
    span = value_span(block, key)
    if span is None:
        return block, False
    return block[:span[0]] + new_text + block[span[1]:], True


def set_magnet(block, magnet):
    """Replace the url of the entry's magnet slot, or add the slot after the first
    fetchable url. Works whether the manifest writes urls inline or one per line."""
    span = value_span(block, "urls")
    if span is None:
        return block, False
    arr = block[span[0]:span[1]]
    obj = re.search(r"\{[^{}]*\"kind\"\s*:\s*\"magnet\"[^{}]*\}", arr, re.S)
    if obj:
        new = re.sub(r"\"url\"\s*:\s*\"[^\"]*\"", lambda m: json.dumps(magnet), obj.group(0))
        return block[:span[0]] + arr[:obj.start()] + new + arr[obj.end():] + block[span[1]:], True
    first = re.search(r"\{[^{}]*\"kind\"\s*:\s*\"(mirror|torrent|direct)\"[^{}]*\}", arr, re.S)
    if not first:
        return block, False
    line_start = arr.rfind("\n", 0, first.start()) + 1
    indent = re.match(r"[ \t]*", arr[line_start:first.start()]).group(0)
    add = ",\n%s{ \"kind\": \"magnet\", \"url\": %s }" % (indent, json.dumps(magnet))
    at = span[0] + first.end()
    return block[:at] + add + block[at:], True


def main(argv=None):
    ap = argparse.ArgumentParser(prog="manifest-update.py", description=__doc__.splitlines()[0])
    ap.add_argument("manifest")
    ap.add_argument("--magnet", required=True, help="magnet link of the bundle torrent")
    ap.add_argument("--records", default="-", help="id\\x1fname\\x1fsize\\x1fsha256\\x1f... lines ('-' = stdin)")
    args = ap.parse_args(argv)

    if not os.path.isfile(args.manifest):
        sys.stderr.write("manifest-update: no such file: %s\n" % args.manifest)
        return 2
    src = sys.stdin if args.records == "-" else open(args.records, encoding="utf-8")
    updates = {}
    for line in src:
        f = line.rstrip("\n").split("\x1f")
        if len(f) >= 4 and f[0]:
            updates[f[0]] = {"size": f[2], "sha256": f[3]}
    if args.records != "-":
        src.close()
    if not updates:
        print("manifest-update: no records on stdin, nothing to do")
        return 0

    with open(args.manifest, encoding="utf-8") as fh:
        text = fh.read()
    try:
        spans = entry_spans(text)
    except ValueError as e:
        sys.stderr.write("manifest-update: %s: %s\n" % (args.manifest, e))
        return 1

    seen, out, all_ids = set(), [], []
    for start, end in spans:
        block = text[start:end]
        m = re.search(r"\"id\"\s*:\s*\"([^\"]+)\"", block)
        eid = m.group(1) if m else None
        if eid:
            all_ids.append(eid)
        if eid in updates:
            seen.add(eid)
            block, ok1 = set_scalar(block, "sha256", json.dumps(updates[eid]["sha256"]))
            block, ok2 = set_scalar(block, "size", updates[eid]["size"])
            block, ok3 = set_magnet(block, args.magnet)
            if not (ok1 and ok2 and ok3):
                sys.stderr.write("manifest-update: entry %s is missing sha256/size/urls - refusing to guess\n" % eid)
                return 1
            out.append((start, end, block))
    missing = sorted(set(updates) - set(all_ids))
    if missing:
        sys.stderr.write("manifest-update: no entry with id %s in %s\n" % (", ".join(missing), args.manifest))
        return 1

    new = text
    for start, end, block in reversed(out):
        new = new[:start] + block + new[end:]
    try:
        json.loads(new)
    except Exception as e:
        sys.stderr.write("manifest-update: the update would break %s (%s) - left untouched\n"
                         % (args.manifest, e))
        return 1
    tmp = args.manifest + ".tmp"
    with open(tmp, "w", encoding="utf-8") as fh:
        fh.write(new)
    os.replace(tmp, args.manifest)
    print("MANIFEST %s: sha256/size/magnet set for %d entr%s (mirror slots stay TBD: no https host yet)"
          % (args.manifest, len(out), "y" if len(out) == 1 else "ies"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
