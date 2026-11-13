#!/usr/bin/env python3
"""n13: make <game>/rtx.conf carry the keys a water variant needs - idempotently, with backups.

usage:
    tools/rtx_water_keys.py --file <game>/rtx.conf [--set key=value]... [--hash 0xHASH]...
                            [--backup-dir <game>/.rhys-water] [--dry-run] [--check] [--quiet]

Why this exists: `tools/water.sh` used to PRINT the rtx.conf lines that go with a variant and
leave the user to paste them, and the live install proved what happens then - G/rtx.conf had
`rtx.translucentMaterial.normalIntensity` and the hash list but none of the animated-water keys,
so the runtime's own water path (UV scroll + the second, LOD-blurred normal sample, gated on
`animatedWaterEnable && surface.isAnimatedWater`) never ran on any surface, on any variant.

Rules, in the order the file is walked:

  * `--set key=value` for a key that is ABSENT  -> appended once, under one marked comment block.
  * `--set key=value` for a key that is PRESENT -> the value is rewritten in place. Everything
    else on the line is kept: indentation, the `=`, a trailing `# comment`, the line ending
    (these files are CRLF; the live one is mixed). A key written twice gets every copy set to
    the same value and the duplication is reported - a duplicated key is a real hazard, the
    runtime reads the last one.
  * `--hash 0x...`   -> that hash must be a member of `rtx.animatedWaterTextures`. Missing ones
    are appended to the END of the existing list line. Nothing is ever removed and no second
    list line is ever written: those 1246-character lists are the asset pack's, and a duplicate
    key line is how a pack breaks (see tools/rtxprofile.sh's header for the same rule).
  * Nothing is written unless something actually changes. When it is, the previous bytes go to
    `<backup-dir>/rtx.conf.prev` (every change) and, once, to `<backup-dir>/rtx.conf.pristine`.
  * `--check` writes nothing, prints one line per key/hash and exits 1 if the file does not yet
    satisfy the request. `--dry-run` prints the changes it would make and exits 0.

Exit codes: 0 satisfied (or applied), 1 `--check` found something missing, 2 bad usage / unreadable
file / the hash list key does not exist and cannot be created safely.
"""
import argparse
import os
import re
import shutil
import sys

LIST_KEY = "rtx.animatedWaterTextures"
MARK = "rhygtaSA n13: the rtx.conf keys the water variant needs (written by tools/water.sh)"

KEY_RE = re.compile(r"^([ \t]*)([A-Za-z_][A-Za-z0-9_.]*)([ \t]*=[ \t]*)(.*)$")


def split_value(rest):
    """('1.0', '') or ('True', '  # why') - a '#' only starts a comment after whitespace, and the
    whitespace in front of it belongs to the comment, so rewriting a value keeps the spacing."""
    i = rest.find("#")
    if i > 0 and rest[i - 1] in " \t":
        j = i
        while j > 0 and rest[j - 1] in " \t":
            j -= 1
        return rest[:j].rstrip(), rest[j:]
    return rest.strip(), ""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--file", required=True)
    ap.add_argument("--set", action="append", default=[], metavar="KEY=VALUE")
    ap.add_argument("--hash", action="append", default=[], metavar="0xHASH")
    ap.add_argument("--backup-dir", default="")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--quiet", action="store_true")
    a = ap.parse_args()

    want = []
    for kv in a.set:
        if "=" not in kv:
            print("rtx_water_keys.py: --set needs key=value, got %r" % kv, file=sys.stderr)
            return 2
        k, v = kv.split("=", 1)
        want.append((k.strip(), v.strip()))
    hashes = [h.strip() for h in a.hash if h.strip()]
    if not want and not hashes:
        print("rtx_water_keys.py: nothing requested (use --set/--hash)", file=sys.stderr)
        return 2
    for h in hashes:
        if not re.fullmatch(r"0x[0-9A-Fa-f]{16}", h):
            print("rtx_water_keys.py: --hash %r is not a 0x + 16 hex-digit texture hash" % h,
                  file=sys.stderr)
            return 2

    try:
        raw = open(a.file, "rb").read().decode("utf-8", "replace")
    except OSError as e:
        print("rtx_water_keys.py: cannot read %s: %s" % (a.file, e), file=sys.stderr)
        return 2
    # newline-preserving split: these files are CRLF, the live one mixes CRLF and LF, and a
    # profile switch must not silently reformat the asset pack's lists.
    lines = raw.split("\n")
    crlf = sum(1 for ln in lines if ln.endswith("\r"))
    eol = "\r\n" if crlf * 2 > len([ln for ln in lines if ln.strip()]) else "\n"

    have = {}            # key -> list of (line index, value, comment)
    for i, ln in enumerate(lines):
        body = ln[:-1] if ln.endswith("\r") else ln
        if body.lstrip().startswith("#"):
            continue
        m = KEY_RE.match(body)
        if m:
            v, c = split_value(m.group(4))
            have.setdefault(m.group(2), []).append((i, v, c))

    actions, missing = [], []

    for k, v in want:
        occ = have.get(k) or []
        if not occ:
            actions.append(("append", k, v))
            missing.append("%s = %s (absent)" % (k, v))
            continue
        if len(occ) > 1:
            actions.append(("note", k, "written %d times in the file; setting every copy" % len(occ)))
        bad = [o for o in occ if o[1] != v]
        if bad:
            for i, old, c in bad:
                actions.append(("set", k, v, old, c, i))
            missing.append("%s = %s (is %s)" % (k, v, " / ".join(sorted({o[1] for o in bad}))))
        elif not a.quiet:
            print("ok   %s = %s" % (k, v))

    if hashes:
        occ = have.get(LIST_KEY) or []
        if not occ:
            actions.append(("newlist", LIST_KEY, ", ".join(hashes)))
            missing.append("%s: the key is absent; it would be created with only the required "
                           "hashes - the pack's own list is NOT in this file" % LIST_KEY)
        else:
            i, val, c = occ[-1]
            cur = [t.strip() for t in val.split(",") if t.strip()]
            add = [h for h in hashes if h not in cur]
            if add:
                actions.append(("hashes", LIST_KEY, add, i, val))
                missing.append("%s: missing %s" % (LIST_KEY, " ".join(add)))
            elif not a.quiet:
                print("ok   %s already lists %s" % (LIST_KEY, " ".join(hashes)))

    if not actions:
        print("rtx.conf already carries every requested water key: %s" % a.file)
        return 0
    if a.check:
        for m in missing:
            print("needs %s" % m)
        return 1

    for act in actions:
        if act[0] == "set":
            _, k, v, old, c, i = act
            print("set  %s = %s   (was %s)%s" % (k, v, old, c and "  [kept its comment]" or ""))
        elif act[0] == "append":
            print("add  %s = %s" % (act[1], act[2]))
        elif act[0] == "hashes":
            print("add  %s: %s appended to the existing list (line %d, %d hashes stay)"
                  % (LIST_KEY, " ".join(act[2]), act[3] + 1, len(act[4].split(","))))
        elif act[0] == "newlist":
            print("add  %s = %s   (WARNING: the pack's list is not in this file)"
                  % (LIST_KEY, act[2]))
        elif act[0] == "note":
            print("note %s: %s" % (act[1], act[2]))
    if a.dry_run:
        print("dry run: %s NOT changed" % a.file)
        return 0

    # apply
    for act in actions:
        if act[0] == "set":
            _, k, v, old, c, i = act
            body = lines[i][:-1] if lines[i].endswith("\r") else lines[i]
            cr = "\r" if lines[i].endswith("\r") else ""
            m = KEY_RE.match(body)
            lines[i] = "%s%s%s%s%s%s" % (m.group(1), m.group(2), m.group(3), v, c, cr)
        elif act[0] == "hashes":
            _, k, add, i, val = act
            body = lines[i][:-1] if lines[i].endswith("\r") else lines[i]
            cr = "\r" if lines[i].endswith("\r") else ""
            m = KEY_RE.match(body)
            v, c = split_value(m.group(4))     # hashes go before a trailing comment, never after it
            lines[i] = "%s%s%s%s, %s%s%s" % (m.group(1), m.group(2), m.group(3),
                                             v.rstrip(), ", ".join(add), c, cr)
    appends = [act for act in actions if act[0] in ("append", "newlist")]
    if appends:
        while lines and lines[-1].strip() == "":
            lines.pop()
        block = ["", "# --- %s ---" % MARK]
        block += ["#   %s = %s" % (a[1], a[2]) for a in appends]
        block += ["%s = %s" % (a[1], a[2]) for a in appends]
        lines.extend(block)
        lines.append("")          # a config file ends with a newline, even if it did not before

    if a.backup_dir:
        os.makedirs(a.backup_dir, exist_ok=True)
        pr = os.path.join(a.backup_dir, "rtx.conf.pristine")
        pv = os.path.join(a.backup_dir, "rtx.conf.prev")
        if not os.path.exists(pr):
            shutil.copy2(a.file, pr)
            print("backup %s (written once)" % pr)
        shutil.copy2(a.file, pv)
        print("backup %s (the bytes this change replaced)" % pv)

    tmp = a.file + ".rhytmp"
    with open(tmp, "wb") as f:
        f.write("\n".join(lines).encode("utf-8"))
    os.replace(tmp, a.file)
    print("wrote %s (%d water keys applied)" % (a.file, len(actions)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
