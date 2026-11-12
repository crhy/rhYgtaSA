#!/usr/bin/env python3
"""Measure GTA SA .dff (RenderWare clump) geometry: bones, vertices, triangles.

usage: dff_stats.py <dir_or_file.dff> [...] [--top N] [--csv out.csv]

Geometry chunk layout (librw GeoStreamData, verified against stock 1.0 US peds in
GTASA-RTX/models/gta3.img — the byte accounting below closes exactly):

    u32 flags            (0x10036 for a normal skinned ped)
    i32 numTriangles
    i32 numVertices
    i32 numMorphTargets
    [4 bytes per vertex]              vertex colours, only when flags & PRELIT
    [8 bytes per vertex per set]      texture coordinates
    [8 bytes per triangle]            (u16 v0, u16 v1 | u16 v2, u16 matId)
    per morph target:
        16 bytes bounding sphere, i32 hasVertices, i32 hasNormals,
        [12 bytes per vertex] positions, [12 bytes per vertex] normals

The number of UV sets is solved from the payload size, so a "clean" parse means the
file's own byte count agrees with the header — that is the self-check.
"""
import os
import struct
import sys


def chunks(data, off, end):
    while off + 12 <= end:
        typ, ln, ver = struct.unpack_from("<III", data, off)
        if typ == 0 or ln == 0 or off + 12 + ln > end:
            return
        yield typ, off + 12, ln, off + 12 + ln
        off += 12 + ln


def parse_geometry_struct(data, po, ln):
    """Parse a geometry ID_STRUCT payload; return dict or None."""
    if ln < 16:
        return None
    flags, ntri, nvert, nmorph = struct.unpack_from("<Iiii", data, po)
    if not (0 < nvert < 500000 and 0 <= ntri < 1000000 and 1 <= nmorph <= 8):
        return None
    tris = 8 * ntri
    per_vert = 12 * nvert                            # positions, then normals
    morph = 24 * nmorph                              # sphere + hasVertices + hasNormals
    # vertex colours (PRELIT) and the UV-set count are solved from the payload size
    # instead of trusted from the flags: only one combination makes the bytes close.
    for colors in (0, 4 * nvert):
        body = ln - (16 + colors + tris + morph) - 2 * per_vert * nmorph
        if body >= 0 and nvert and body % (8 * nvert) == 0 and body // (8 * nvert) <= 8:
            uvsets = body // (8 * nvert)
            return dict(flags=flags, tris=ntri, verts=nvert, morph=nmorph,
                        uvsets=uvsets, colors=bool(colors), payload=ln,
                        total=16 + colors + tris + morph + uvsets * 8 * nvert
                              + 2 * per_vert * nmorph, fits=True)
    return None


def parse(path):
    data = open(path, "rb").read()
    bones = 0
    geos = []
    for typ, po, ln, ce in chunks(data, 0, len(data)):
        if typ != 16:                                   # clump
            continue
        for t2, p2, l2, c2 in chunks(data, po, ce):
            if t2 == 14:                                # frames
                for t3, p3, l3, _ in chunks(data, p2, p2 + l2):
                    if t3 == 1 and l3 >= 4:
                        bones = struct.unpack_from("<I", data, p3)[0]
                    break
            elif t2 == 26:                              # geometry
                for t3, p3, l3, _ in chunks(data, p2, p2 + l2):
                    if t3 == 15:                        # extension
                        for t4, p4, l4, _ in chunks(data, p3, p3 + l3):
                            if t4 == 1:
                                g = parse_geometry_struct(data, p4, l4)
                                if g:
                                    geos.append(g)
                if not geos:                            # non-bin layout
                    g = parse_geometry_struct(data, p2, l2)
                    if g:
                        geos.append(g)
    return bones, geos


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    top = 15
    for a in sys.argv[1:]:
        if a.startswith("--top"):
            top = int(a.split("=", 1)[1]) if "=" in a else 15
    files = []
    for a in args:
        if os.path.isdir(a):
            for root, _, names in os.walk(a):
                for n in names:
                    if n.lower().endswith(".dff"):
                        files.append(os.path.join(root, n))
        elif a.lower().endswith(".dff"):
            files.append(a)
    rows = []
    bad = []
    for f in sorted(files):
        bones, geos = parse(f)
        v = sum(g["verts"] for g in geos)
        t = sum(g["tris"] for g in geos)
        ok = bool(geos) and all(g["fits"] for g in geos)
        if not ok:
            bad.append(f)
        rows.append((f, os.path.getsize(f), bones, len(geos), v, t, ok))
    print(f"{len(rows)} .dff files, {len(bad)} did not parse cleanly")
    if bad:
        for f in bad[:10]:
            print("   unparsed:", f)
    tv = sum(r[4] for r in rows)
    tt = sum(r[5] for r in rows)
    tb = sum(r[2] for r in rows)
    n = max(1, len(rows))
    print(f"totals: {tv:,} vertices, {tt:,} triangles, {tb:,} bones; "
          f"mean {tv/n:,.0f} verts / {tt/n:,.0f} tris per model")
    print(f"\nbiggest {top} by triangles:")
    for f, size, bones, ng, v, t, ok in sorted(rows, key=lambda r: -r[5])[:top]:
        print(f"  {t:7,d} tris {v:7,d} verts {bones:4d} bones {ng} geo "
              f"{size/1024:8.1f} KB  {os.path.basename(f)}{'' if ok else '  (unparsed)'}")
    if "--csv" in sys.argv:
        out = sys.argv[sys.argv.index("--csv") + 1]
        with open(out, "w") as o:
            o.write("file,bytes,bones,geometries,vertices,triangles,ok\n")
            for r in rows:
                o.write(",".join(str(x) for x in r) + "\n")
        print(f"\nwrote {out}")


if __name__ == "__main__":
    main()
