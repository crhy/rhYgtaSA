#!/usr/bin/env python3
"""s32: which texture hashes belong in rtx.conf's `rtx.hairCardTextures` (alpha-cutout cards).

    usage: tools/vegetation_hashes.py PATH [--pattern PAT] [--min-cut 5] [--emit KEY]
                                          [--verbose] [--check-conf CONF]

PATH is models/gta3.img, a .txd, or a folder of .txd files (whatever
tools/remix_texhash.py.iter_txd_bytes accepts).

Why: the hillside vegetation of GTA SA is alpha-tested *cards* (procobj/vegepart billboards:
gta_proc_bush, gta_proc_rushes, veg_fuzzyplant, tree1..3, ...). In RTX Remix those draws come out
as soft scribbled lines rather than leaves. The runtime has a category for exactly this -
`rtx.hairCardTextures` ("Textures in this category will preserve fine texture detail, render as
cutouts instead of alpha blends, and disable backface culling") - and the opacity-micromap
tooltip names the same scene ("a ground densely covered with grass blades ... multiple trees with
a lot of foliage"). Nothing in the pack or in rtx.conf puts a single vegetation texture in it.

How a texture is judged (measured, not guessed): the mip-0 alpha is decoded per format
(DXT1/DXT3/DXT5 blocks, A8R8G8B8, A4R4G4B4, A1R5G5B5) and a texture counts as a *card* when it
has real cutout content: at least --min-cut % fully transparent texels or at least --min-cut %
in the mid band (1..254). Fully opaque bark/dirt/ground textures are left alone, because tagging
them would make them alpha-tested for no reason.

Note on the format: many vegetation rasters report as "16bit" through
tools/remix_texhash.py because their fourcc sits in the raster header (offset -12 from the pixel
data) rather than in the 32-byte window that module scans; they are DXT3/DXT1. We read the
fourcc from the raster header as a fallback. The *hash* is unaffected (it is over the size word's
bytes), so the hashes printed here are the ones rtx.conf uses.

  --pattern PAT    only dictionaries whose name matches this regex (default: the vegetation set)
  --min-cut N      transparency threshold in percent of texels (default 5)
  --emit KEY       print one rtx.conf line `KEY = 0x..., 0x...` (e.g. --emit rtx.hairCardTextures)
  --check-conf C   report which of the chosen hashes already appear in some list of rtx.conf C
  --verbose        print the per-texture table
"""
import argparse
import os
import re
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import remix_texhash as R  # noqa: E402

# Dictionaries that hold the alpha-tested foliage cards. This is an allow-list, not a name
# guess: it is the set of texture dictionaries referenced by the procedural-vegetation objects
# of the game (generic/*_cipl objects named genVEG_*, i.e. what populates the hillsides, plus the
# vegepart foliage objects), minus the ones in that set that are rock/junk/underwater textures
# (badlands, gta_rockcuntry, gtarock_deserts, underwater, junkpiles). Verified against
# models/gta3.img: all 40 exist and hold 190 textures, 119 of which pass the cutout test below.
# Do not widen this with a loose prefix: `veg.*` also matches the Las Vegas building dictionaries
# (vegasbuild, vegasneon, ...) and `.*grass.*` matches tikigrass, which is pool/concrete/wall
# textures. --pattern takes any regex if you want to try a wider set.
VEG_RE = (r"^(gta_proc.*|gta_cactus|gta_potplants.*|gta_brokentrees|gta_deserttrees|gta_tree_.*|"
          r"gtatree.*|tree[123].*|veg_fuzzyplant|veg_leaves.*|vegtresshi.*|lod_veg|kbplantssmz.*|"
          r"canopy)$")

BLOCK4 = (b"DXT1", b"DXT3", b"DXT5")


def fourcc_of(t, blob):
    """The fourcc, including the ones remix_texhash's scan misses (raster-header position)."""
    if t.fourcc in BLOCK4:
        return t.fourcc
    cand = blob[t.off - 16:t.off - 12]
    return cand if cand in BLOCK4 else b""


def alpha_of(t, raw, fourcc):
    """-> uint8 array of alpha values, or None when the format carries no alpha."""
    if fourcc == b"DXT3":
        b = np.frombuffer(raw, dtype=np.uint8).reshape(-1, 16)
        lo = b[:, 0:8]
        a4 = np.stack([(lo >> (4 * k)) & 15 for k in range(2)], axis=2).reshape(len(b), 16)
        return (a4 * 17).astype(np.uint8)
    if fourcc == b"DXT5":
        b = np.frombuffer(raw, dtype=np.uint8).reshape(-1, 16)
        a0 = b[:, 0].astype(np.uint16)
        a1 = b[:, 1].astype(np.uint16)
        bits = np.zeros((len(b), 48), dtype=np.uint8)
        for i in range(6):
            byte = b[:, 2 + i].astype(np.uint16)
            for k in range(8):
                bits[:, 8 * i + k] = (byte >> k) & 1
        codes = np.zeros((len(b), 16), dtype=np.uint8)
        for i in range(16):
            v = np.zeros(len(b), dtype=np.uint16)
            for k in range(3):
                v = (v << 1) | bits[:, 3 * i + k]
            codes[:, i] = v
        lut0 = np.stack([a0, a1, (6 * a0 + a1) // 7, (5 * a0 + 2 * a1) // 7, (4 * a0 + 3 * a1) // 7,
                         (3 * a0 + 4 * a1) // 7, (2 * a0 + 5 * a1) // 7, (a0 + 6 * a1) // 7], axis=1)
        lut1 = np.stack([a0, a1, (4 * a0 + a1) // 5, (3 * a0 + 2 * a1) // 5, (2 * a0 + 3 * a1) // 5,
                         (a0 + 4 * a1) // 5, np.zeros_like(a0), np.full_like(a0, 255)], axis=1)
        lut = np.where((a0 > a1)[:, None], lut0, lut1).astype(np.uint8)
        return lut[np.arange(len(b))[:, None], codes]
    if fourcc == b"DXT1":
        b = np.frombuffer(raw, dtype=np.uint8).reshape(-1, 8)
        c0 = b[:, 0:2].copy().view("<u2").reshape(len(b)).astype(np.uint32)
        c1 = b[:, 2:4].copy().view("<u2").reshape(len(b)).astype(np.uint32)
        def rgb565(v):
            r, g, bb = (v >> 11) & 31, (v >> 5) & 63, v & 31
            return np.stack([(r << 3) | (r >> 2), (g << 2) | (g >> 4), (bb << 3) | (bb >> 2)], -1)
        col0, col1 = rgb565(c0), rgb565(c1)
        three = (c0 > c1)          # 3-colour block when c0 > c1 (the 565 ints)
        idx = b[:, 4:]
        codes = np.stack([(idx >> (2 * k)) & 3 for k in range(4)], axis=2).reshape(len(b), 16)
        a = np.full((len(b), 16), 255, dtype=np.uint8)
        a[three] = np.where(codes[three] == 3, 0, 255)     # 3-colour mode: code 3 is transparent
        return a
    if t.depth == 32 and not t.fourcc and t.size == t.w * t.h * 4:
        return np.frombuffer(raw, dtype=np.uint8).reshape(-1, 4)[:, 3]
    if t.depth == 16 and not t.fourcc and t.size == t.w * t.h * 2:
        v = np.frombuffer(raw, dtype="<u2")
        if t.format_name() == "A4R4G4B4":
            return ((v >> 12) & 15).astype(np.uint8) * 17
        if t.format_name() == "A1R5G5B5":
            return (((v >> 15) & 1) * 255).astype(np.uint8)
    return None


def stats(a):
    n = float(a.size)
    z = float((a == 0).sum()) / n
    m = float(((a > 0) & (a < 255)).sum()) / n
    o = float((a == 255).sum()) / n
    return z, m, o


def main(argv=None):
    ap = argparse.ArgumentParser(prog="vegetation_hashes.py", description=__doc__.splitlines()[0])
    ap.add_argument("path")
    ap.add_argument("--pattern", default=VEG_RE)
    ap.add_argument("--min-cut", type=float, default=5.0)
    ap.add_argument("--emit", default=None)
    ap.add_argument("--check-conf", default=None)
    ap.add_argument("--verbose", action="store_true")
    a = ap.parse_args(argv)

    rx = re.compile(a.pattern, re.I)
    rows, skipped = [], []
    for label, blob in R.iter_txd_bytes(a.path):
        base = os.path.basename(label.split(":")[-1]).rsplit(".", 1)[0]
        if not rx.match(base):
            continue
        for t in R.parse_txd(blob, label):
            raw = t.d3d9_mip0(blob)
            fc = fourcc_of(t, blob)
            al = alpha_of(t, raw, fc)
            h = "%016X" % R.xxh3_64bits(raw)
            if al is None:
                skipped.append((base, t.name, "no alpha decoded (%s)" % (fc.decode() or t.format_name())))
                continue
            z, m, o = stats(al)
            card = (100 * z >= a.min_cut) or (100 * m >= a.min_cut)
            (rows if card else skipped).append(
                (base, t.name, t.w, t.h, fc.decode() or t.format_name(), 100 * z, 100 * m, 100 * o, h,
                 "" if card else "opaque"))
    rows.sort(key=lambda r: (r[0], r[1]))
    if a.verbose:
        print("%-20s %-18s %-9s %-5s %7s %7s %7s  %s" %
              ("dictionary", "texture", "wxh", "fmt", "a=0%", "0<a<255", "a=255%", "hash"))
        for r in rows:
            print("%-20s %-18s %-9s %-5s %7.2f %7.2f %7.2f  0x%s" %
                  (r[0], r[1], "%dx%d" % (r[2], r[3]), r[4], r[5], r[6], r[7], r[8]))
        print()
        print("not tagged (no cutout content / no alpha):")
        for s in skipped:
            print("   %-20s %-18s %s" % (s[0], s[1], s[-1] if len(s) == 3 else s[-1]))

    hashes = sorted({r[8] for r in rows})
    dicts = sorted({r[0] for r in rows})
    print("# %d card texture(s) in %d dictionary(ies) out of %d texture(s) scanned in those "
          "dictionaries" % (len(hashes), len(dicts), len(rows) + len(skipped)))
    print("# dictionaries: " + ", ".join(dicts))
    if a.check_conf:
        conf = R.read_conf(a.check_conf)
        have = {h: conf.get(h, []) for h in hashes if conf.get(h)}
        print("# already in an rtx.conf list: %d" % len(have))
        for h, keys in sorted(have.items()):
            print("#   0x%s  %s" % (h, ", ".join(keys)))
    if a.emit:
        print("%s = %s" % (a.emit, ", ".join("0x" + h for h in hashes)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
