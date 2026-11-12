#!/usr/bin/env python3
"""List every traffic light in the GTA SA world for rhyremix.asi.

usage: traffic_list.py --game <GAME_DIR> --out <file>

Sources:
  * the .ide files under <GAME_DIR>/data/maps (model id -> name, bounding radius)
  * the lamp table (<GAME_DIR>/moonloader/SARemix_Light_Manager.dat): the exact offset of a
    traffic light head above its instance origin, for the models it covers (that file
    ships with the mod pack and is not copied into this repo)
  * binary ("bnry") IPLs stored inside <GAME_DIR>/models/gta3.img (model by id, position,
    rotation quaternion)
Output: header "# rhyremix traffic v2", then one line per light:
        "x y z headz heading lensx lensy lensz"
        headz   = height of the lamp head above the instance origin, game units
                  (column 4 is unchanged from v1, so an old reader still gets it)
        heading = the instance yaw in degrees, from the IPL quaternion
        lens*   = the head offset in the MODEL's own frame; the plugin rotates it by
                  the heading and adds it to the instance origin, which is exactly
                  what CTrafficLights does with a head offset (0x59C890 in gta-sa.exe:
                  out = m->right*e.x + m->up*e.y + m->at*e.z + m->pos)
        -> the light lands on the lens instead of on the mast. For cj_traffic_light4
           (720 of the 1522 instances) the head is 2.10 m sideways of the pole.
  --v1 writes the old four-column "x y z headz" file, byte for byte what shipped
  before n02; the plugin reads both.
"""
import argparse
import math
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


# ---------------------------------------------------------------- heading

def heading_deg(rot):
    """Yaw in degrees (0..360) of an IPL instance quaternion (x, y, z, w).

    Every traffic-light instance in gta3.img carries a pure Z-axis quaternion
    (measured over all 1524 raw instances: x = y = 0 and |q| = 1 for every one),
    so the yaw IS the rotation and the model's "right" column is (cos, sin, 0).
    That is the column the game multiplies a head offset by at 0x59C890, which is
    why a heading column plus a model-local offset reproduces where the game puts
    a head.
    """
    x, y, z, w = rot
    fx = 1.0 - 2.0 * (y * y + z * z)
    fy = 2.0 * (x * y + w * z)
    # Round first, then fold: rounding 359.96 straight to "360.0" would put a
    # heading outside the 0..360 the plugin's cos/sin are handed (and read as a
    # different number than 0.0 in the file for the same lamp).
    h = round(math.degrees(math.atan2(fy, fx)), 1) % 360.0
    return 0.0 if h >= 360.0 else h


def rotate_yaw(offset, heading):
    """Rotate a model-local (x, y, z) offset by a heading in degrees.

    Reporting only - the plugin does the same thing in rhySignalLensPos(), and
    tests/signals_check.cpp asserts the two agree. z is untouched, because the
    rotation is a yaw.
    """
    rad = math.radians(heading)
    c, s = math.cos(rad), math.sin(rad)
    x, y, z = offset
    return (x * c - y * s, x * s + y * c, z)

def read_entry(f, off, sz, sz2):
    size = (sz2 or sz) * 2048
    f.seek(off * 2048)
    return f.read(size)

# ---------------------------------------------------------------- dedupe / head height

def dedupe(instances):
    """Drop LOD duplicates: same model id, position within 0.1 game units.

    Each instance is (model, pos) or (model, pos, extra) - n02 carries the IPL
    rotation through as `extra`, and it is passed through untouched. Returns the
    instances in input order.
    """
    CELL = 0.1
    grid = {}
    kept = []
    for inst in instances:
        model, pos = inst[0], inst[1]
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
            kept.append(inst)
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


def lens_offset_for(model, radius, table_heads, radii, max_gap=10.0):
    """Full model-local head offset (x, y, z) for one traffic light model, or None.

    The lamp table carries THREE numbers per head, and the shipped headz column
    throws two of them away: cj_traffic_light4's head is at (-2.10, +0.10, +7.60)
    in the model's own frame, so the lens sits 2.10 m sideways of the mast, and a
    light at pole+(0,0,7.60) is 2.10 m from the lens it is supposed to be a bulb
    on. Rotating that offset by the instance heading is the game's own rule for a
    head offset (0x59C890), so the sideways part is worth keeping.

    Same borrowing rule as headz_for, with a bound the height does not need: a
    model the table does not cover takes the offset of the covered model with the
    closest .ide radius, but only when that gap is <= max_gap. A height can be
    borrowed across a 25-unit radius gap; a 2 m sideways arm cannot - mtraffic1
    (r=85) is a different lamp from cj_traffic_light4 (r=60), so it keeps the pole
    x/y and only the borrowed height. Ties go to the model with the larger
    sideways offset, which is the one whose head is furthest off the mast.
    """
    offs = table_heads.get(model)
    if offs:
        return tuple(round(v, 3) for v in max(offs, key=lambda o: o[2]))
    best = None
    for mid, other in table_heads.items():
        if mid not in radii or not other:
            continue
        d = abs(radii[mid][1] - radius)
        if d > max_gap:
            continue
        off = max(other, key=lambda o: o[2])
        lateral = math.hypot(off[0], off[1])
        if (best is None or d < best[0]
            or (d == best[0] and lateral > best[2])):
            best = (d, mid, lateral, off)
    if best is None:
        return None
    return tuple(round(v, 3) for v in best[3])

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
    ap.add_argument("--v1", action="store_true",
                    help="write the old four-column 'x y z headz' file instead of "
                         "the v2 eight-column one (A/B: the plugin reads both)")
    ap.add_argument("--lens-gap", type=float, default=10.0,
                    help="largest .ide radius gap across which a model may borrow "
                         "another model's sideways lens offset (default 10)")
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
        for model, interior, _lod, pos, rot in parse_binary_ipl(data):
            if model in radii and interior == 0:
                instances.append((model, pos, rot))
                per_model[model] = per_model.get(model, 0) + 1
    f.close()
    print("instances: " + ", ".join(f"{mid} {n}" for mid, n in sorted(per_model.items())))

    # 3. drop LOD duplicates: same model, position within 0.1
    kept = dedupe(instances)

    # 4. the lens offset per model, once: None means "no number to trust", and
    # that model's lines keep the pole x/y and the borrowed height.
    lens = {}
    for mid, (_name, radius) in radii.items():
        lens[mid] = lens_offset_for(mid, radius, table_heads, radii, args.lens_gap)
    print("lens offsets (model-local, from the lamp table or borrowed by radius):")
    for mid in sorted(radii):
        nm, radius = radii[mid]
        off = lens[mid]
        print(f"  {mid:5d} {nm:18s} r={radius:5.1f} "
              + (f"lens=({off[0]:+.2f},{off[1]:+.2f},{off[2]:.2f}) "
                 f"sideways={math.hypot(off[0], off[1]):.2f} m"
                 if off else "no offset: the light stays on the mast"))

    out_lines = ["# rhyremix traffic v1" if args.v1 else "# rhyremix traffic v2"]
    if not args.v1:
        out_lines.append(
            "# x y z headz heading lensx lensy lensz - headz is the v1 column 4; "
            "heading is the IPL instance yaw in degrees; lens* is the head offset in "
            "the model's own frame, rotated by the heading and added to x,y,z by the "
            "plugin (lens_offset=1 in rhyremix.ini)")
    near = 0
    with_lens = 0
    max_move = 0.0
    for model, pos, rot in kept:
        _name, radius = radii[model]
        hz = headz_for(model, radius, table_heads, radii)
        off = lens[model]
        if off:
            with_lens += 1
            wx, wy, _wz = rotate_yaw(off, heading_deg(rot))
            max_move = max(max_move, math.hypot(wx, wy))
        if args.v1:
            out_lines.append(f"{pos[0]:.3f} {pos[1]:.3f} {pos[2]:.3f} {hz:.2f}")
        else:
            lx, ly, lz = off if off else (0.0, 0.0, hz)
            out_lines.append(
                f"{pos[0]:.3f} {pos[1]:.3f} {pos[2]:.3f} {hz:.2f} "
                f"{heading_deg(rot):.1f} {lx:+.3f} {ly:+.3f} {lz:+.3f}")
        dx, dy = pos[0] - 2495.0, pos[1] + 1680.0
        if dx * dx + dy * dy < args.near * args.near:
            near += 1

    os.makedirs(os.path.dirname(args.out) or ".", exist_ok=True)
    with open(args.out, "w") as ofh:
        ofh.write("\n".join(out_lines) + "\n")
    print(f"traffic lights written: {len(out_lines) - (2 if not args.v1 else 1)}")
    if not args.v1:
        print(f"lines carrying a lens offset: {with_lens} of {len(kept)} "
              f"(furthest light moved sideways off the mast: {max_move:.2f} m)")
    print(f"lights within {args.near:.0f} m (2D) of (2495, -1680): {near}")

if __name__ == "__main__":
    main()
