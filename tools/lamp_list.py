#!/usr/bin/env python3
"""List every street-lamp light in the GTA SA world for rhyremix.asi.

usage: lamp_list.py --game <GAME_DIR> --out <file> [--table <path>] [--no-conjugate]

The lamp table defaults to <GAME_DIR>/moonloader/SARemix_Light_Manager.dat, the
third-party mod's own file: it is NOT in this repo (see .gitignore), pass --table
only when it lives somewhere else.

Sources:
  * lamp table (Lua text, parsed with regexes, never executed): model id -> light type + offsets
  * the .ide files under <GAME_DIR>/data/maps/**/*.ide (model NAME -> model id, and
    the model's bounding radius); text IPLs name models, binary IPLs use ids
  * text IPLs under <GAME_DIR>/data/maps/**/*.ipl (model by NAME)
  * binary ("bnry") IPLs stored inside <GAME_DIR>/models/gta3.img (model by numeric id)
Output: one line per light: "type x y z", header "# rhyremix lights v1".

The .ide id lookup is not decoration: the position of a model inside the gta3.img
index is NOT its model id (img entry 1226 is cen_bit_10.dff, while model id 1226
is lamppost3), so resolving a text IPL's model name through the img index picks
the wrong model and silently drops or invents a lamp.
"""
import argparse
import os
import re
import struct
import sys

# ---------------------------------------------------------------- lamp table

def parse_lamp_table(text):
    """Return {model_id: (type, [(x, y, z), ...])} from the Lua table text."""
    lamps = {}
    # top-level entries: exactly 2-space indent before [id] = {
    for m in re.finditer(r"^  \[(\d+)\] = \{(.*?)^  \},", text, re.S | re.M):
        mid = int(m.group(1))
        body = m.group(2)
        t = re.search(r'\["type"\]\s*=\s*(\d+)', body)
        if not t:
            continue
        typ = int(t.group(1))
        if not (1 <= typ <= 12):
            continue
        pm = re.search(r'\["pos"\]\s*=\s*\{(.*)\}', body, re.S)
        offsets = []
        if pm:
            for om in re.finditer(r"\[\d+\]\s*=\s*\{(.*?)\}", pm.group(1), re.S):
                blk = om.group(1)
                g = re.search(r'\["y"\]\s*=\s*(-?[\d.eE+-]+)', blk)
                gx = re.search(r'\["x"\]\s*=\s*(-?[\d.eE+-]+)', blk)
                gz = re.search(r'\["z"\]\s*=\s*(-?[\d.eE+-]+)', blk)
                if g and gx and gz:
                    offsets.append((float(gx.group(1)), float(g.group(1)), float(gz.group(1))))
        lamps[mid] = (typ, offsets)
    return lamps

# ---------------------------------------------------------------- .ide files

def parse_ide_from_text(text):
    """Return {model_id: (name, radius)} for one .ide file's contents.

    Line format: id, NAME, txd_group, radius, size.  The radius is the model's
    bounding radius in CENTIMETRES: lamppost3 is 62 (a 0.62 m pole), a traffic
    light head is 60, a traffic cone is 100.  That is what makes it useful here
    - a model the lamp table does not cover can be matched to the table model
    with the closest radius, and the table's own head offset then applies.
    """
    out = {}
    for raw in text.splitlines():
        line = raw.split("#", 1)[0].strip()
        parts = [p.strip() for p in line.split(",")]
        if len(parts) < 4:
            continue
        try:
            mid = int(parts[0])
            radius = float(parts[3])
        except ValueError:
            continue
        out[mid] = (parts[1].lower(), radius)
    return out


def parse_ide(path):
    with open(path, "r", encoding="latin1", errors="replace") as fh:
        return parse_ide_from_text(fh.read())

# ---------------------------------------------------------------- quaternions

def quat_mul(a, b):
    """Hamilton product a*b, quaternions as (x, y, z, w)."""
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return (
        aw * bx + ax * bw + ay * bz - az * by,
        aw * by + ay * bw + az * bx - ax * bz,
        aw * bz + az * bw + ax * by - ay * bx,
        aw * bw - ax * bx - ay * by - az * bz,
    )

def rotate(v, q):
    """Rotate vector v=(x,y,z) by quaternion q=(x,y,z,w): sandwich q*v*conj(q)."""
    qv = quat_mul(q, (v[0], v[1], v[2], 0.0))
    r = quat_mul(qv, (-q[0], -q[1], -q[2], q[3]))
    return (r[0], r[1], r[2])

# ---------------------------------------------------------------- text IPLs

def parse_text_ipl(text):
    """Yield instances between an `inst` line and the next `end`.
    Fields: id, modelname, interior, posX, posY, posZ, rotX, rotY, rotZ, rotW, lod."""
    in_inst = False
    for raw in text.splitlines():
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        if line == "inst":
            in_inst = True
            continue
        if line == "end":
            in_inst = False
            continue
        if not in_inst:
            continue
        parts = [p.strip() for p in line.split(",")]
        if len(parts) < 11:
            continue
        try:
            name = parts[1]
            interior = int(float(parts[2]))
            pos = (float(parts[3]), float(parts[4]), float(parts[5]))
            rot = (float(parts[6]), float(parts[7]), float(parts[8]), float(parts[9]))
            lod = int(float(parts[10]))
        except ValueError:
            continue
        yield (name, interior, pos, rot, lod)

# ---------------------------------------------------------------- img / binary IPLs

def read_img_index(img_path):
    """VER2 index: list of (id, name, offset, size, size2); size in 2048-byte sectors."""
    with open(img_path, "rb") as f:
        magic, n = struct.unpack("<4sI", f.read(8))
        if magic != b"VER2":
            raise ValueError(f"{img_path}: not a VER2 img ({magic!r})")
        entries = []
        for i in range(n):
            off, sz, sz2, raw = struct.unpack("<IHH24s", f.read(32))
            name = raw.split(b"\0", 1)[0].decode("latin1")
            entries.append((i, name, off, sz, sz2))
        return f, entries

def parse_binary_ipl(data):
    """Parse a "bnry" IPL blob. Yields (model, interior, lod, pos, rot)."""
    if not data.startswith(b"bnry"):
        return
    count = struct.unpack_from("<I", data, 0x04)[0]
    start = struct.unpack_from("<I", data, 0x1C)[0]
    for k in range(count):
        base = start + k * 40
        pos = struct.unpack_from("<3f", data, base)
        rot = struct.unpack_from("<4f", data, base + 12)
        model, interior, lod = struct.unpack_from("<3i", data, base + 28)
        yield (model, interior, lod, pos, rot)

def read_entry(f, off, sz, sz2):
    size = (sz2 or sz) * 2048
    f.seek(off * 2048)
    return f.read(size)

# ---------------------------------------------------------------- dedupe

def dedupe_positions(entries, tol=0.1):
    """Drop entries whose position duplicates one already kept; returns (kept, dropped).

    entries is [(pos, payload), ...] and kept keeps the input order.  The key is
    the position ALONE, not (model, position): the same physical lamp is instanced
    by more than one model id (full detail and its LOD, or a district's own copy),
    and one lamp table entry can carry several offsets that land on the same point
    (sub_floodlite has three (0,0,0) offsets).  Every duplicate line costs a whole
    light slot at runtime, so 360 of the 7362 lines the old list had were the same
    point twice.
    """
    CELL = tol
    grid = {}
    kept = []
    dropped = 0
    for pos, payload in entries:
        cx, cy, cz = int(pos[0] // CELL), int(pos[1] // CELL), int(pos[2] // CELL)
        dup = False
        for gx in (cx - 1, cx, cx + 1):
            for gy in (cy - 1, cy, cy + 1):
                for gz in (cz - 1, cz, cz + 1):
                    for kx, ky, kz in grid.get((gx, gy, gz), ()):
                        if (abs(kx - pos[0]) <= tol and abs(ky - pos[1]) <= tol
                                and abs(kz - pos[2]) <= tol):
                            dup = True
        if dup:
            dropped += 1
            continue
        grid.setdefault((cx, cy, cz), []).append(pos)
        kept.append((pos, payload))
    return kept, dropped

# ---------------------------------------------------------------- main

def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--game", required=True, help="game directory")
    ap.add_argument("--table", default=None,
                    help="SARemix_Light_Manager.dat (Lua table); default is "
                         "<game>/moonloader/SARemix_Light_Manager.dat")
    ap.add_argument("--out", required=True, help="output light list")
    ap.add_argument("--no-conjugate", dest="conjugate", action="store_false", default=True,
                    help="do not conjugate instance quaternions before rotating offsets")
    ap.add_argument("--report-missing", action="store_true",
                    help="list lamp-named models that have instances but no lamp table entry")
    args = ap.parse_args()

    if args.table is None:
        args.table = os.path.join(args.game, "moonloader",
                                  "SARemix_Light_Manager.dat")
    if not os.path.exists(args.table):
        sys.exit(f"lamp table not found: {args.table}\n"
                 "It ships with the GTASA-Remix pack under <game>/moonloader/ and is "
                 "deliberately not copied into this repo; pass --table if it is elsewhere.")
    with open(args.table, "r", encoding="latin1") as tf:
        lamps = parse_lamp_table(tf.read())
    print(f"lamp ids found: {len(lamps)}")

    img_path = os.path.join(args.game, "models", "gta3.img")
    with open(img_path, "rb") as f:
        magic, n = struct.unpack("<4sI", f.read(8))
        if magic != b"VER2":
            sys.exit(f"{img_path}: not VER2")
        entries = [struct.unpack("<IHH24s", f.read(32)) for _ in range(n)]

        # model NAME -> model id, from every .ide next to the .ipl files.  See
        # the module docstring: the img index position is not the model id.
        ide = {}
        maps_dir = os.path.join(args.game, "data", "maps")
        for root, _dirs, files in os.walk(maps_dir):
            for fn in sorted(files):
                if not fn.lower().endswith(".ide"):
                    continue
                for mid, (name, radius) in parse_ide(os.path.join(root, fn)).items():
                    ide[mid] = (name, radius)
        name2id = {}
        for mid, (name, _radius) in ide.items():
            name2id.setdefault(name, mid)
        print(f".ide models: {len(ide)}")

        instances = []  # (model_id, pos, rot)

        # text IPLs
        text_lamps = 0
        for root, _dirs, files in os.walk(maps_dir):
            for fn in sorted(files):
                if not fn.lower().endswith(".ipl"):
                    continue
                with open(os.path.join(root, fn), "r", encoding="latin1", errors="replace") as ih:
                    for name, interior, pos, rot, _lod in parse_text_ipl(ih.read()):
                        mid = name2id.get(name.lower())
                        if mid is not None and mid in lamps and interior == 0:
                            instances.append((mid, pos, rot))
                            text_lamps += 1
        print(f"text-IPL lamps: {text_lamps}")

        # binary IPLs inside gta3.img (read in place, never extracted)
        binary_lamps = 0
        per_model = {}
        for i, (off, sz, sz2, raw) in enumerate(entries):
            name = raw.split(b"\0", 1)[0].decode("latin1")
            if not name.lower().endswith(".ipl"):
                continue
            data = read_entry(f, off, sz, sz2)
            if not data.startswith(b"bnry"):
                continue
            for model, interior, _lod, pos, rot in parse_binary_ipl(data):
                if interior != 0:
                    continue
                per_model[model] = per_model.get(model, 0) + 1
                if model in lamps:
                    instances.append((model, pos, rot))
                    binary_lamps += 1
        print(f"binary-IPL lamps: {binary_lamps}")

        if args.report_missing:
            # A lamp model the table does not cover is a lamp post the game draws
            # with no light on it.  Everything else about the list is complete,
            # so this is the one gap worth printing every time the list is built.
            lampish = re.compile(r"lamp|light|lite", re.I)
            notlamp = re.compile(r"tree|shard|box|_int|int_|car|head|tail|number|sign", re.I)
            missing = {}
            for mid, count in per_model.items():
                if mid in lamps:
                    continue
                name = ide.get(mid, ("?%d" % mid, 0.0))[0]
                if not lampish.search(name) or notlamp.search(name):
                    continue
                missing[mid] = (name, count)
            print(f"lamp-named models with no table entry: {len(missing)}")
            for mid in sorted(missing, key=lambda m: -missing[m][1]):
                name, count = missing[mid]
                near = min(lamps, key=lambda t: abs(ide.get(t, ("", 0.0))[1]
                                                    - ide.get(mid, ("", 0.0))[1]))
                print(f"  id={mid:<6d} {name:<24s} r={ide.get(mid, ('', 0))[1]:<7} "
                      f"instances={count:<5d} closest table model: {near} "
                      f"({ide.get(near, ('', 0))[0]})")

    # Drop LOD-only duplicates before the offsets are applied.
    kept, dupes = dedupe_positions([(pos, (model, rot)) for model, pos, rot in instances])
    print(f"duplicate instances dropped: {dupes}")

    out_lines = ["# rhyremix lights v1"]
    near = 0
    # Dedup the LIGHTS too, for the reason in dedupe_positions: several offsets of
    # one table entry can land on the same point.
    lights = []
    for pos, (model, rot) in kept:
        typ, offsets = lamps[model]
        q = (-rot[0], -rot[1], -rot[2], rot[3]) if args.conjugate else rot
        for ox, oy, oz in offsets:
            rx, ry, rz = rotate((ox, oy, oz), q)
            wx, wy, wz = pos[0] + rx, pos[1] + ry, pos[2] + rz
            lights.append(((wx, wy, wz), typ))
    lights, dup_out = dedupe_positions(lights)
    print(f"duplicate light positions dropped: {dup_out}")
    for (wx, wy, wz), typ in lights:
        out_lines.append(f"{typ} {wx:.3f} {wy:.3f} {wz:.3f}")
        dx, dy = wx - 2495.0, wy + 1680.0
        if dx * dx + dy * dy < 300.0 * 300.0:
            near += 1

    os.makedirs(os.path.dirname(args.out) or ".", exist_ok=True)
    with open(args.out, "w") as ofh:
        ofh.write("\n".join(out_lines) + "\n")
    print(f"lights written: {len(out_lines) - 1}")
    print(f"lights within 300 m (2D) of (2495, -1680): {near}")

if __name__ == "__main__":
    main()
