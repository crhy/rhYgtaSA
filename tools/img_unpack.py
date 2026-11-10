#!/usr/bin/env python3
"""Unpack a GTA SA VER2 .img archive into a folder (ModLoader reads <name>.img/ folders like archives).
usage: img_unpack.py <archive.img> <out_dir>"""
import os, struct, sys
src, out = sys.argv[1], sys.argv[2]
with open(src, "rb") as f:
    magic, n = struct.unpack("<4sI", f.read(8))
    if magic != b"VER2":
        sys.exit(f"{src}: not a VER2 img ({magic!r})")
    entries = [struct.unpack("<IHH24s", f.read(32)) for _ in range(n)]
    os.makedirs(out, exist_ok=True)
    seen = set()
    for off, stream_sz, arch_sz, raw in entries:
        name = raw.split(b"\0", 1)[0].decode("latin1")
        size = (arch_sz or stream_sz) * 2048
        f.seek(off * 2048)
        data = f.read(size)
        if len(data) != size:
            sys.exit(f"{src}: short read for {name}")
        key = name.lower()
        if key in seen:  # later duplicates win in the game, keep the same rule
            print(f"duplicate {name}, later entry kept")
        seen.add(key)
        with open(os.path.join(out, name), "wb") as o:
            o.write(data)
print(f"{src}: {n} entries -> {out}")
