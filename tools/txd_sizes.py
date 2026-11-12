#!/usr/bin/env python3
"""Measure the textures inside GTA SA .txd files (RenderWare texture dictionaries).

usage: txd_sizes.py <dir_or_file> [...] [--top N]

SA .txd layout used here (verified against the V Vegetation Mix pack):
  every texture entry starts with the name chunk marker
      FF FF 03 18 09 00 00 00 06 11 00 00 <name:32 bytes>
  and the raster header follows a few bytes later as
      <fourcc:4> <u16 width> <u16 height> ...
  fourcc is "DXT1"/"DXT3"/"DXT5" for compressed textures (the on-disk bytes are
  the same bytes the game keeps in RAM), other fourccs mean uncompressed.

Output: per-file texture count, max size, and the uncompressed-in-RAM byte total
(mip chain included), plus a pack-wide total.
"""
import os
import re
import struct
import sys

NAME_MARK = re.compile(re.escape(b"\xff\xff\x03\x18\x09\x00\x00\x00\x06\x11\x00\x00"))
FOURCC = re.compile(rb"(DXT1|DXT2|DXT3|DXT4|DXT5|CUST)")
BPP = {"DXT1": 8, "DXT2": 8, "DXT3": 16, "DXT4": 16, "DXT5": 16}  # per 4x4 block


def mip_bytes(w, h, block_bytes):
    """Bytes of a compressed texture including its whole mip chain."""
    total = 0
    while w >= 1 and h >= 1:
        total += max(1, (w + 3) // 4) * max(1, (h + 3) // 4) * block_bytes
        if w == 1 and h == 1:
            break
        w = max(1, w // 2)
        h = max(1, h // 2)
    return total


def parse(path):
    data = open(path, "rb").read()
    out = []
    for m in NAME_MARK.finditer(data):
        name = data[m.end():m.end() + 32].split(b"\0", 1)[0].decode("latin1")
        f = FOURCC.search(data, m.end(), min(len(data), m.end() + 0x100))
        if not f:
            out.append((name, 0, 0, "?", 0))
            continue
        w, h = struct.unpack_from("<HH", data, f.end())
        cc = f.group(1).decode()
        out.append((name, w, h, cc, mip_bytes(w, h, BPP[cc]) if cc in BPP else 0))
    return out


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    top = 20
    for a in sys.argv[1:]:
        if a.startswith("--top"):
            top = int(a.split("=", 1)[1]) if "=" in a else 20
    files = []
    for a in args:
        if os.path.isdir(a):
            for root, _, names in os.walk(a):
                for n in names:
                    if n.lower().endswith(".txd"):
                        files.append(os.path.join(root, n))
        elif a.lower().endswith(".txd"):
            files.append(a)
    rows = []
    for f in sorted(files):
        tex = parse(f)
        rows.append((f, os.path.getsize(f), tex))
    total_tex = sum(len(t) for _, _, t in rows)
    total_ram = sum(b for _, _, t in rows for *_, b in t)
    total_disk = sum(s for _, s, _ in rows)
    print(f"{len(rows)} .txd files, {total_tex} textures, "
          f"{total_disk/1e6:.1f} MB on disk, {total_ram/1e6:.1f} MB in RAM (mips incl.)")
    print(f"\nbiggest {top} by RAM:")
    for f, size, tex in sorted(rows, key=lambda r: -sum(b for *_, b in r[2]))[:top]:
        big = sorted(tex, key=lambda t: -t[4])[:3]
        desc = ", ".join(f"{n} {w}x{h} {c}" for n, w, h, c, _ in big)
        print(f"  {sum(b for *_, b in tex)/1e6:8.2f} MB  {len(tex):3d} tex  "
              f"{os.path.basename(f):28s}  {desc}")
    worst = [(n, w, h, c, b, os.path.basename(f)) for f, _, t in rows for n, w, h, c, b in t]
    worst.sort(key=lambda r: -r[4])
    print("\nbiggest single textures in the whole set:")
    for n, w, h, c, b, fn in worst[:15]:
        print(f"  {b/1e6:7.2f} MB  {w:5d}x{h:<5d} {c}  {n}  ({fn})")
    unk = [(n, fn) for f, _, t in rows for n, w, h, c, b in t if c == "?"]
    if unk:
        print(f"\n{len(unk)} textures with no recognised fourcc (uncompressed?):")
        for n, fn in unk[:10]:
            print(f"  {n} ({fn})")


if __name__ == "__main__":
    main()
