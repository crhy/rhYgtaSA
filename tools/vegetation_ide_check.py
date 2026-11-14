#!/usr/bin/env python3
"""n21: does a vegetation pack's IDE data resolve, before a launch can find out the hard way?

    usage: vegetation_ide_check.py PACK_DIR [--game GAME_DIR] [--ide PATH]...
                                     [--emit-shims] [--near N] [--quiet]

Why: docs/VEGETATION.md cause 1 is one broken reference. The pack's `vegepart.ide` re-points
ids 784-788 (`lod_redwoodgrp`, `lod_vbg_fir_co`, `lod_largefurs05/06/07`) at a texture
dictionary named `lod_veg_firs`, and the pack ships `lod_veg_fist.txd` — one letter off. The
pack's readme says to delete the two IDE files "to avoid errors of models without textures",
which is the author admitting the same thing. Nobody had checked the *rest* of the two files,
and that is what this tool does: every dictionary named by every IDE line the pack ships,
resolved against everything that would actually be loaded.

Sources of dictionaries (in load order, later ones cannot undo earlier ones but here we only
care about "does the name exist anywhere"):
  * the pack's own loose files (`<pack>/gta3.img/*.txd`, `<pack>/models/**/*.txd`);
  * the game's archives (`GAME/models/*.img` entries) and loose `GAME/models/**.txd`;
  * every other modloader mod's loose files (`GAME/modloader/*/...`), unless --no-mods.

Output: one line per missing dictionary, with the closest provided name (difflib) when there
is one — that is how a typo gets named instead of guessed at. `--emit-shims` prints
`missing=provided` pairs for exactly those near-misses, which is the input
tools/vegetation.sh step 5 uses to write the corrected file.

exit: 0 nothing missing, 1 at least one missing dictionary, 2 usage / unreadable input.
"""
import argparse
import difflib
import os
import re
import struct
import sys

IMG_MAGIC = b"VER2"


def ide_rows(path):
    """IDE text file -> [(id, model, txd, lineno)]. Same rule as vegetation_stream.py."""
    out = []
    try:
        text = open(path, encoding="utf-8", errors="replace").read()
    except OSError as e:
        print("vegetation_ide_check: cannot read %s: %s" % (path, e), file=sys.stderr)
        return out
    for i, line in enumerate(text.splitlines(), 1):
        line = line.split(";")[0].split("#")[0].strip()
        if not line:
            continue
        parts = [p.strip() for p in line.split(",")]
        if len(parts) < 3 or not re.fullmatch(r"\d+", parts[0]):
            continue
        out.append((int(parts[0]), parts[1], parts[2], i))
    return out


def img_names(path):
    """VER2 .img -> set of entry names (lowercase, with extension)."""
    out = set()
    try:
        with open(path, "rb") as f:
            head = f.read(8)
            if head[:4] != IMG_MAGIC:
                return out
            n = struct.unpack("<I", head[4:8])[0]
            for _ in range(n):
                raw = f.read(32)
                if len(raw) < 32:
                    break
                nm = struct.unpack("<IHH24s", raw)[3].split(b"\0", 1)[0].decode("latin1")
                if nm:
                    out.add(nm.lower())
    except OSError:
        return out
    return out


def provided_for_pack(pack):
    """Loose .txd/.dff names the pack itself provides."""
    out = set()
    for dirpath, _d, files in os.walk(pack):
        for fn in files:
            if fn.lower().endswith((".txd", ".dff")):
                out.add(fn.lower())
    return out


def provided_for_game(game, with_mods=True, exclude=None):
    """Everything the game would load: archive entries, loose model files, other mods.

    `exclude` is a folder to skip (tools/vegetation.sh passes its own install dir, so the
    files IT generated cannot mask the pack's own missing dictionary).
    """
    excl = os.path.realpath(exclude) if exclude else None
    out = set()
    mdir = os.path.join(game, "models")
    for dirpath, _d, files in os.walk(mdir):
        for fn in files:
            if fn.lower().endswith(".img"):
                out |= img_names(os.path.join(dirpath, fn))
            elif fn.lower().endswith((".txd", ".dff")):
                out.add(fn.lower())
    if with_mods:
        mods = os.path.join(game, "modloader")
        for entry in sorted(os.listdir(mods)) if os.path.isdir(mods) else []:
            if entry.startswith("."):
                continue
            for dirpath, _d, files in os.walk(os.path.join(mods, entry)):
                if excl and os.path.realpath(dirpath).startswith(excl):
                    continue
                for fn in files:
                    if fn.lower().endswith((".txd", ".dff")):
                        out.add(fn.lower())
    return out


def default_ides(pack):
    """The pack's own IDE files (data/**/*.ide)."""
    out = []
    for dirpath, _d, files in os.walk(pack):
        for fn in files:
            if fn.lower().endswith(".ide"):
                out.append(os.path.join(dirpath, fn))
    return sorted(out)


def main(argv=None):
    ap = argparse.ArgumentParser(prog="vegetation_ide_check.py", description=__doc__.splitlines()[0])
    ap.add_argument("pack")
    ap.add_argument("--game", default=None, help="game folder (adds its archives, loose models, other mods)")
    ap.add_argument("--ide", action="append", default=None, help="IDE file to check (default: every .ide in the pack)")
    ap.add_argument("--emit-shims", action="store_true", help="print `missing=provided` for near-miss typos")
    ap.add_argument("--near", type=int, default=3, help="how many close names to suggest (default 3)")
    ap.add_argument("--no-mods", action="store_true", help="do not count other modloader mods as providers")
    ap.add_argument("--exclude", default=None,
                    help="folder to skip when collecting providers (tools/vegetation.sh: its own install)")
    ap.add_argument("--quiet", action="store_true")
    a = ap.parse_args(argv)

    if not os.path.isdir(a.pack):
        print("vegetation_ide_check: no such pack folder: %s" % a.pack, file=sys.stderr)
        return 2
    ides = a.ide or default_ides(a.pack)
    if not ides:
        print("vegetation_ide_check: no .ide files in %s (nothing to check)" % a.pack, file=sys.stderr)
        return 2

    have = provided_for_pack(a.pack)
    if a.game:
        have |= provided_for_game(a.game, with_mods=not a.no_mods, exclude=a.exclude)
    pool = sorted(have)

    missing = {}          # txd name -> [(ide, id, model, line)]
    checked = 0
    for path in ides:
        for mid, model, txd, lineno in ide_rows(path):
            checked += 1
            if not txd or txd in ("-", "-1", "none"):
                continue
            key = txd.lower()
            if "." not in key:
                key += ".txd"
            if key not in have:
                missing.setdefault(key, []).append((os.path.relpath(path, a.pack), mid, model, lineno))

    if not a.quiet:
        print("pack      : %s" % a.pack)
        print("IDE files : %s" % ", ".join(os.path.relpath(p, a.pack) for p in ides))
        print("checked   : %d IDE line(s), %d distinct dictionary name(s) available to load"
              % (checked, len(have)))
    if not missing:
        if not a.quiet:
            print("OK: every dictionary these IDEs name is provided by the pack, the game or another mod")
        return 0

    print("MISSING %d dictionary name(s) — these models would load without textures:" % len(missing))
    shims = []
    for txd in sorted(missing):
        rows = missing[txd]
        base = txd[:-4] if txd.endswith(".txd") else txd
        close = [c for c in difflib.get_close_matches(base, [p[:-4] for p in pool
                                                             if p.endswith(".txd")], a.near, 0.7)]
        print("  %-20s %d model(s): %s" % (txd, len(rows),
                                           ", ".join("%s (%s id %d line %d)" % (m, os.path.basename(i), mid, ln)
                                                     for i, mid, m, ln in rows[:6])))
        if close:
            print("      closest provided: %s" % ", ".join(close))
            shims.append((txd, close[0] + ".txd"))
    if a.emit_shims:
        print()
        for dst, src in shims:
            print("%s=%s" % (dst, src))
    return 1


if __name__ == "__main__":
    sys.exit(main())
