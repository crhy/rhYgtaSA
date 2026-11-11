#!/usr/bin/env python3
"""List every traffic light in the GTA SA world for rhyremix.asi.

usage: traffic_list.py --game <GAME_DIR> --out <file>

Sources:
  * the .ide files under <GAME_DIR>/data/maps (model id -> name, bounding radius)
  * the lamp table (<GAME_DIR>/moonloader/SARemix_Light_Manager.dat): the exact height of a
    traffic light head above its instance origin, for the models it covers (that file
    ships with the mod pack and is not copied into this repo)
  * binary ("bnry") IPLs stored inside <GAME_DIR>/models/gta3.img (model by id)
Output: header "# rhyremix traffic v1", then one line per light:
        "x y z headz"   (headz = height of the lamp head above the instance origin,
                        in game units; the plugin adds it to z when placing the
                        light, so a wrong headz is a glow in the middle of the
                        pole instead of in the head)
"""
import argparse
import os
import re
import struct
import sys

try:  # the lamp table parser, shared with the street-lamp list tool
    import lamp_list
except ImportError:  # run from elsewhere: tools/ is not on sys.path
    lamp_list = None

# Traffic light models (SA 1.0 US/EN). Names come from the .ide files; the ids
# are the gta3.img index positions, which is what the binary IPLs reference.
TRAFFIC_NAMES = {
    "mtraffic1", "mtraffic2", "mtraffic3", "mtraffic4",
    "trafficlight1",
    "cj_traffic_light3", "cj_traffic_light4", "cj_traffic_light5",
    "gay_traffic_light",
}

# ---------------------------------------------------------------- .ide files

def parse_ide_from_text(text):
    """Return {model_id: (name, radius)} for one .ide file's contents.

    Line format: id, NAME, txd_group, radius, size   (radius in game units,
    10 units = 1 metre).
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

# ---------------------------------------------------------------- img / IPLs

def read_img_index(img_path):
    f = open(img_path, "rb")
    magic, n = struct.unpack("<4sI", f.read(8))
    if magic != b"VER2":
        raise ValueError(f"{img_path}: not a VER2 img ({magic!r})")
    entries = [struct.unpack("<IHH24s", f.read(32)) for _ in range(n)]
    return f, entries

def parse_binary_ipl(data):
    """Yield (model, interior, lod, pos, rot) from a "bnry" IPL blob."""
    if not data.startswith(b"bnry"):
        return
    count = struct.unpack_from("<I", data, 0x04)[0]
    start = struct.unpack_from("<I", data, 0x1C)[0]
    for k in range(count):
        base = start + k * 40
        if base + 40 > len(data):
            break
        pos = struct.unpack_from("<3f", data, base)
        rot = struct.unpack_from("<4f", data, base + 12)
        model, interior, lod = struct.unpack_from("<3i", data, base + 28)
        yield (model, interior, lod, pos, rot)

def read_entry(f, off, sz, sz2):
    size = (sz2 or sz) * 2048
    f.seek(off * 2048)
    return f.read(size)

# ---------------------------------------------------------------- dedupe / head height

def dedupe(instances):
    """Drop LOD duplicates: same model id, position within 0.1 game units.

    Returns the instances in input order.
    """
    CELL = 0.1
    grid = {}
    kept = []
    for model, pos in instances:
        cx, cy, cz = int(pos[0] // CELL), int(pos[1] // CELL), int(pos[2] // CELL)
        dup = False
        for gx in (cx - 1, cx, cx + 1):
            for gy in (cy - 1, cy, cy + 1):
                for gz in (cz - 1, cz, cz + 1):
                    bucket = grid.get((model, gx, gy, gz))
                    if bucket is None:
                        continue
                    for kx, ky, kz in bucket:
                        if (abs(kx - pos[0]) <= 0.1 and abs(ky - pos[1]) <= 0.1
                            and abs(kz - pos[2]) <= 0.1):
                            dup = True
        if not dup:
            grid.setdefault((model, cx, cy, cz), []).append(pos)
            kept.append((model, pos))
    return kept

def headz(radius):
    """Height of the lamp head above the instance origin, estimated from the
    model's bounding radius.  Kept as the fallback for models the lamp table
    does not cover; see headz_for, which prefers the table's exact offset.
    """
    return round(radius * 0.09, 2)


def headz_for(model, radius, table_heads, radii):
    """Head height for one traffic light model.

    The lamp table gives the exact offset for the traffic light models it covers
    (cj_traffic_light4's head is 7.6 units up, cj_traffic_light5's 6.0, gay_traffic
    _light's 7.65).  For a model the table does not cover, use the offset of the
    covered model with the closest .ide radius: the .ide radius is the model's
    bounding radius in centimetres, so trafficlight1 (64) belongs with cj_traffic
    _light4 (60), not with a 50-unit bollard. Ties go to the higher head.
    """
    if not table_heads:
        return headz(radius)
    offs = table_heads.get(model)
    if offs:
        return round(max(z for _x, _y, z in offs), 2)
    best = None
    for mid, other in table_heads.items():
        if mid not in radii or not other:
            continue
        d = abs(radii[mid][1] - radius)
        z = max(zz for _x, _y, zz in other)
        if best is None or d < best[0] or (d == best[0] and z > best[2]):
            best = (d, mid, z)
    if best is None:
        return headz(radius)
    return round(best[2], 2)

# ---------------------------------------------------------------- main

def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--game", required=True, help="game directory")
    ap.add_argument("--out", required=True, help="output traffic light list")
    ap.add_argument("--near", type=float, default=300.0,
                    help="also report how many lights fall within this 2D radius of the test spot")
    ap.add_argument("--table", default=None,
                    help="SARemix_Light_Manager.dat, for exact head heights "
                         "(default: <game>/moonloader/SARemix_Light_Manager.dat)")
    args = ap.parse_args()

    # Exact head heights, when the lamp table is readable. Without it every head
    # is an estimate from the bounding radius, which put traffic light heads
    # about 2 units below where the game draws them.
    table_path = args.table
    if table_path is None:
        table_path = os.path.join(args.game, "moonloader",
                                  "SARemix_Light_Manager.dat")
    table_heads = {}
    if lamp_list is not None and os.path.isfile(table_path):
        with open(table_path, "r", encoding="latin1") as tf:
            for mid, (_typ, offsets) in lamp_list.parse_lamp_table(tf.read()).items():
                table_heads[mid] = offsets
        print(f"lamp table head heights: {len(table_heads)} models")
    else:
        print(f"no lamp table at {table_path}: head heights are radius estimates")

    # 1. model id -> radius, from every .ide under data/maps
    radii = {}
    maps_dir = os.path.join(args.game, "data", "maps")
    for root, _dirs, files in os.walk(maps_dir):
        for fn in sorted(files):
            if not fn.lower().endswith(".ide"):
                continue
            for mid, (name, radius) in parse_ide(os.path.join(root, fn)).items():
                if name in TRAFFIC_NAMES:
                    radii[mid] = (name, radius)
    print("traffic models found: " + ", ".join(
        f"{mid} {name} r={radius}" for mid, (name, radius) in sorted(radii.items())))
    if not radii:
        sys.exit("no traffic light models found in any .ide")

    # 2. instances, from the binary IPLs inside gta3.img (read in place)
    img_path = os.path.join(args.game, "models", "gta3.img")
    f, entries = read_img_index(img_path)
    per_model = {}
    instances = []
    for i, (off, sz, sz2, raw) in enumerate(entries):
        name = raw.split(b"\0", 1)[0].decode("latin1")
        if not name.lower().endswith(".ipl"):
            continue
        data = read_entry(f, off, sz, sz2)
        if not data.startswith(b"bnry"):
            continue
        for model, interior, _lod, pos, _rot in parse_binary_ipl(data):
            if model in radii and interior == 0:
                instances.append((model, pos))
                per_model[model] = per_model.get(model, 0) + 1
    f.close()
    print("instances: " + ", ".join(f"{mid} {n}" for mid, n in sorted(per_model.items())))

    # 3. drop LOD duplicates: same model, position within 0.1
    kept = dedupe(instances)

    out_lines = ["# rhyremix traffic v1"]
    near = 0
    for model, pos in kept:
        _name, radius = radii[model]
        out_lines.append(f"{pos[0]:.3f} {pos[1]:.3f} {pos[2]:.3f} "
                         f"{headz_for(model, radius, table_heads, radii):.2f}")
        dx, dy = pos[0] - 2495.0, pos[1] + 1680.0
        if dx * dx + dy * dy < args.near * args.near:
            near += 1

    os.makedirs(os.path.dirname(args.out) or ".", exist_ok=True)
    with open(args.out, "w") as ofh:
        ofh.write("\n".join(out_lines) + "\n")
    print(f"traffic lights written: {len(out_lines) - 1}")
    print(f"lights within {args.near:.0f} m (2D) of (2495, -1680): {near}")

if __name__ == "__main__":
    main()
