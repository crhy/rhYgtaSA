#!/usr/bin/env python3
"""Read the 2dFx light records out of a GTA SA .dff (RenderWare clump).

usage: dff_2dfx.py --game <GAME_DIR> [--model NAME]... [--stats] [--json out.json]

WHY THIS FILE EXISTS
--------------------
Every lamp the game itself knows about carries its bulb position, colour and
size INSIDE the model: GTA SA's 2dFx records. Our lamp list used to come only
from the SARemix Light Manager table (a third-party Lua file with one entry per
model id), so any model that table does not mention is a lamp post the game
draws with a dark bulb - findings D and E of the 2026-11-12 video review, and
V2 of the Verona Beach / pier video. This module reads the game's own data
instead of a third party's copy of it.

THE FORMAT, AS THE GAME ITSELF DEFINES IT
-----------------------------------------
The chunk id is not guessed: gta-sa.exe registers a stream handler for chunk
0x0253F2F8, and the record frame below is what closes byte-for-byte on every
one of the 1591 .dff of gta3.img that carries the chunk (see --stats): a parser
that only returns records when the record list ends exactly at the end of the
chunk cannot be quietly off by a field. The tag is the LOW byte of the third
word, measured: each tag value pairs with exactly one payload size over the
whole archive (0/80, 1/24, 3/56, 6/44, 7/88, 8/4, 9/12, 10/40), and the game's
own code reads the record's first four payload bytes as a colour - the signal
head loop at 0x49DC75 compares entry+0x10 against 200 and entry+0x11 against
100, and entry+0x10 is exactly payload+0x00, the red byte.

    chunk 0x0253F2F8 payload:
        u32 recordCount
        record[recordCount]:
            f32 x, y, z          model-local position
            u32 tagword          low byte = tag; 0 = light, 1 = particle,
                                 3 = particle+matrix, 6 = object,
                                 7 = road-sign text, 8 = entry index,
                                 9 = entry/exit point, 10 = mover
            u32 payloadSize
            u8  payload[payloadSize]

    tag 0 (LIGHT) payload, the only one this module decodes:
        +0x00 u8 r,g,b,a         colour. The alpha byte is a constant 0xC8=200
                                 across the lamp models, which is what makes the
                                 byte order (and not a packed 0xRRGGBB int)
                                 certain.
        +0x04 f32 range          the record's own range in game units: 62 for
                                 the small bollard/post-top lamps, 100 for the
                                 Los Santos lampposts, 150-300 for the wide
                                 roadway and pier fittings, 0 for the records
                                 that only carry a corona.
        +0x08 f32 f8, +0x0c f32 f12, +0x10 f32 f16
                                 three more floats the file does not name.
                                 Measured values are small enums, not points:
                                 f8 in {0, 5, 12, 18} (0 exactly where the model
                                 draws no corona), f12 in {1, 2, 2.5, 4},
                                 f16 in {0, 8}. They are NOT a focus point: a
                                 focus would vary with the record's position,
                                 and these do not.
        +0x14 u8[4]              flags
        +0x18 ...                then NUL-terminated ASCII runs at
                                 model-dependent offsets (corona texture,
                                 shadow texture, the material the bulb sits in).
                                 They are NOT fixed-offset fields - two lamp
                                 models disagree by a byte - so this module does
                                 not guess them: payload_text() returns the runs.

The 76-byte light payload variant the game also accepts is parsed the same way;
payloads of other tags are counted but not decoded.
"""
import argparse
import json
import os
import struct
import sys

CH_2DFX = 0x0253F2F8

TAG_NAMES = {
    0: "light",
    1: "particle",
    2: "particle2",
    3: "particle_matrix",
    4: "tag4",
    5: "tag5",
    6: "object",
    7: "sign_text",
    8: "entry_index",
    9: "entry_point",
    10: "mover",
}

# Light payloads the game's own reader accepts; the parser decodes any light
# record at least this long and reports the size so a new variant shows up.
LIGHT_PAYLOAD_SIZES = (80, 76)


def chunks(data, off, end):
    """Yield (type, payload_off, len, end) for the RW chunk headers at [off,end).

    Stops at the first header that does not fit, which is how a clump's chunk
    list ends. Version words are ignored: GTA writes 0x1803FFFF everywhere.
    """
    while off + 12 <= end:
        typ, ln, ver = struct.unpack_from("<III", data, off)
        if typ == 0 or ln == 0 or off + 12 + ln > end:
            return
        yield typ, off + 12, ln, off + 12 + ln
        off += 12 + ln


def find_2dfx_chunks(data):
    """Every 2dFx chunk payload in a .dff, wherever the clump nests it."""
    out = []

    def walk(off, end, depth):
        if depth > 12:
            return
        for typ, po, ln, ce in chunks(data, off, end):
            if typ == CH_2DFX:
                out.append((po, ln))
            walk(po, ce, depth + 1)

    walk(0, len(data), 0)
    return out


def parse_2dfx(data, po, ln):
    """Decode one 2dFx chunk payload. Returns (records, ok) with ok=True only
    when the record list ended exactly at the end of the chunk - the self-check
    described in the docstring."""
    if ln < 4:
        return [], False
    count = struct.unpack_from("<I", data, po)[0]
    p = po + 4
    end = po + ln
    recs = []
    for _ in range(count):
        if p + 20 > end:
            return recs, False
        x, y, z = struct.unpack_from("<3f", data, p)
        tagword = struct.unpack_from("<I", data, p + 12)[0]
        size = struct.unpack_from("<I", data, p + 16)[0]
        if size > 65536 or p + 20 + size > end:
            return recs, False
        pay = data[p + 20:p + 20 + size]
        # The tag is the LOW byte of the third word: measured over all 1591
        # 2dFx chunks of gta3.img, the word is never bigger than 0xff and each
        # tag value pairs with exactly one payload size (0/80, 1/24, 3/56,
        # 6/44, 7/88, 8/4, 9/12, 10/40). Reading the top byte instead makes
        # every record look like a light, which is how a first version of this
        # parser ended up calling every palm tree in Los Santos a lamp.
        rec = {"tag": tagword & 0xFF, "tagword": tagword, "x": x, "y": y,
               "z": z, "size": size}
        if rec["tag"] == 0 and size >= 8:
            r, g, b, a = struct.unpack_from("<4B", data, p + 20)
            rec.update(r=r, g=g, b=b, a=a)
            if size >= 20:
                rec["range"] = struct.unpack_from("<f", data, p + 24)[0]
                rec["f8"] = struct.unpack_from("<f", data, p + 28)[0]
                rec["f12"] = struct.unpack_from("<f", data, p + 32)[0]
                rec["f16"] = struct.unpack_from("<f", data, p + 36)[0]
            if size >= 24:
                rec["flags"] = struct.unpack_from("<4B", data, p + 36)
            rec["text"] = payload_text(pay)
        recs.append(rec)
        p += 20 + size
    return recs, p == end


def payload_text(pay):
    """The printable NUL-terminated runs of a record payload, in order.

    The names (corona texture, shadow texture, material) sit at model-dependent
    offsets, so they are returned as a list rather than claimed to be fields.
    Runs shorter than 3 characters are dropped: they are padding between fields.
    """
    runs, cur = [], b""
    for byte in pay + b"\x00":
        if byte and 32 <= byte < 127:
            cur += bytes([byte])
        else:
            if len(cur) >= 3:
                runs.append(cur.decode("latin1"))
            cur = b""
    return runs


def lights_of_dff(data):
    """The tag-0 (light) records of one .dff: [{x,y,z,r,g,b,a,radius,...}].

    A .dff with no 2dFx chunk, or whose records do not close, gives []. That is
    deliberate: a half-parsed lamp is worse than a missing one, and the caller
    reports the miss.
    """
    out = []
    for po, ln in find_2dfx_chunks(data):
        recs, ok = parse_2dfx(data, po, ln)
        if not ok:
            return []
        out.extend(r for r in recs if r.get("tag") == 0)
    return out


def img_entries(img_path):
    """{name_without_extension: (file_offset, size)} for a VER2 .img."""
    with open(img_path, "rb") as f:
        magic, n = struct.unpack("<4sI", f.read(8))
        if magic != b"VER2":
            raise ValueError(f"{img_path}: not a VER2 img ({magic!r})")
        out = {}
        for i in range(n):
            off, sz, sz2, raw = struct.unpack("<IHH24s", f.read(32))
            name = raw.split(b"\0", 1)[0].decode("latin1")
            out[name.rsplit(".", 1)[0].lower()] = (name, off * 2048,
                                                   (sz2 or sz) * 2048)
        return out


def read_dff(img_path, off, size):
    with open(img_path, "rb") as f:
        f.seek(off)
        return f.read(size)


def scan_img(img_path, want=None, stats=False):
    """{model_name_without_extension: [light records]} for the .dff entries.

    `want` (a set of lowercase names) limits which entries are read at all: the
    archive is 940 MB and reading it entry by entry is a seek per model, so the
    caller that already knows which models its IPLs place should say so.
    """
    entries = img_entries(img_path)
    res, st = {}, {"dffs": 0, "dfls": 0, "chunks": 0, "closed": 0, "tags": {},
                   "lightsizes": {}}
    for name, (fname, off, size) in sorted(entries.items()):
        low = fname.lower()
        if not low.endswith(".dff"):
            continue
        if want is not None and name not in want:
            continue
        data = read_dff(img_path, off, size)
        st["dffs"] += 1
        found = find_2dfx_chunks(data)
        if not found:
            continue
        for po, ln in found:
            st["chunks"] += 1
            recs, ok = parse_2dfx(data, po, ln)
            if ok:
                st["closed"] += 1
            for r in recs:
                t = TAG_NAMES.get(r["tag"], "tag%d" % r["tag"])
                st["tags"][t] = st["tags"].get(t, 0) + 1
                if r["tag"] == 0:
                    st["lightsizes"][r["size"]] = st["lightsizes"].get(r["size"], 0) + 1
        lights = lights_of_dff(data)
        if lights:
            res[name] = lights
            st["dfls"] += 1
    return res, st


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--game", required=True)
    ap.add_argument("--model", action="append", default=[],
                    help="lowercase model name (no .dff), repeatable")
    ap.add_argument("--stats", action="store_true",
                    help="scan every .dff and report the record-tag histogram")
    ap.add_argument("--json", help="write the whole scan as json")
    a = ap.parse_args()

    img = os.path.join(a.game, "models", "gta3.img")
    want = set(m.lower() for m in a.model) if a.model else None
    res, st = scan_img(img, want=want, stats=a.stats)
    print(f".dff scanned: {st['dffs']}, 2dFx chunks: {st['chunks']}, "
          f"closing exactly: {st['closed']}")
    print("record tags: " + ", ".join(f"{k}={v}" for k, v in
                                      sorted(st["tags"].items(), key=lambda kv: -kv[1])))
    print("light payload sizes: " + ", ".join(f"{k}B={v}" for k, v in
                                              sorted(st["lightsizes"].items())))
    print(f"models with 2dFx LIGHT records: {len(res)}")
    print(f"2dFx light records: {sum(len(v) for v in res.values())}")
    if a.json:
        with open(a.json, "w") as fh:
            json.dump(res, fh)
        print(f"wrote {a.json}")
    for name in sorted(res, key=lambda n: -len(res[n]))[:40]:
        recs = res[name]
        r0 = recs[0]
        print(f"  {name:<28s} {len(recs):3d} lights  rgb={r0.get('r')},{r0.get('g')},{r0.get('b')}"
              f" a={r0.get('a')} radius={r0.get('radius', 0):.2f} "
              f"pos=({r0['x']:.2f},{r0['y']:.2f},{r0['z']:.2f}) "
              f"flags={r0.get('flags')} text={r0.get('text')}")


if __name__ == "__main__":
    main()
