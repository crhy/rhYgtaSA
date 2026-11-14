#!/usr/bin/env python3
"""n20 - add or remove one `subLayers` entry in a Remix `mod.usda`, and check the layer text.

This is tools/mirrors.py's patcher generalised to any layer name (mirrors.py keeps its own
hard-wired LAYER constant; nothing here changes it). Used by tools/textures.sh:

  tools/usd_switch.py add    <mod.usda> rhy_textures.usda [first|last]
  tools/usd_switch.py remove <mod.usda> rhy_textures.usda
  tools/usd_switch.py check  <file.usda>            # bracket/input-name sanity, no write
  tools/usd_switch.py has    <mod.usda> rhy_textures.usda

It prints one line and exits 0; it writes only for add/remove, and only when something changes.
The bracket check is the same one tools/mirrors.py runs before it installs a layer, because a
USD syntax error in a sublayer does not stop Remix: it silently loads nothing.
"""
from __future__ import annotations

import argparse
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mirrors import MDL_INPUTS, bracket_scan, validate     # noqa: E402


def entry_line(layer, indent):
    return "\n%s    @./%s@" % (indent, layer)


def add_sublayer(text, layer, position="last"):
    """(new_text, changed, why): put @./layer@ into mod.usda's subLayers list."""
    needle = "@./%s@" % layer
    if needle in text:
        return text, False, "sublayer already present"
    m = re.search(r"(\n(\s*)subLayers\s*=\s*\[)(.*?)(\])", text, re.S)
    if not m:
        raise ValueError("no subLayers = [ ... ] block: refusing to guess where to put the layer")
    head, indent, items, tail = m.group(1), m.group(2), m.group(3), m.group(4)
    entry = entry_line(layer, indent)
    if position == "first":
        new_items = entry + "," + items
    else:
        stripped = items.rstrip()
        trail = items[len(stripped):]
        comma = "" if stripped.endswith(",") else ","
        new_items = stripped + comma + entry + trail
    return text[:m.start()] + head + new_items + tail + text[m.end():], True, "sublayer added (%s)" % position


def remove_sublayer(text, layer):
    """(new_text, changed, why): drop the layer's entry, fixing the comma it leaves behind."""
    needle = "@./%s@" % layer
    lines = text.splitlines(True)
    out, changed = [], False
    for line in lines:
        if needle in line:
            changed = True
            if not line.rstrip().endswith(",") and out:
                prev = out[-1].rstrip()
                if prev.endswith(","):
                    out[-1] = prev[:-1] + ("\n" if out[-1].endswith("\n") else "")
            continue
        out.append(line)
    return "".join(out), changed, ("sublayer line removed" if changed else "no sublayer present")


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0], add_help=True)
    ap.add_argument("action", choices=["add", "remove", "check", "has"])
    ap.add_argument("path")
    ap.add_argument("layer", nargs="?", default="")
    ap.add_argument("position", nargs="?", default="last", choices=["first", "last"])
    a = ap.parse_args(argv)
    if a.action in ("add", "remove", "has") and not a.layer:
        raise SystemExit("%s needs a layer name" % a.action)
    with open(a.path, encoding="utf-8") as fh:
        text = fh.read()
    if a.action == "check":
        validate(text, os.path.basename(a.path), need_mat=True)
        print("%s: ok (%d mat_ override%s)"
              % (a.path, text.count('over "mat_'), "s" if text.count('over "mat_') != 1 else ""))
        return 0
    if a.action == "has":
        print("yes" if ("@./%s@" % a.layer) in text else "no")
        return 0
    new, changed, why = (add_sublayer(text, a.layer, a.position) if a.action == "add"
                         else remove_sublayer(text, a.layer))
    if changed:
        validate(new, os.path.basename(a.path), need_mat=False)
        with open(a.path, "w", encoding="utf-8") as fh:
            fh.write(new)
    print("%s: %s" % (a.path, why))
    return 0


if __name__ == "__main__":
    sys.exit(main())
