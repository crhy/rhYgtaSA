#!/usr/bin/env python3
"""List every street-lamp light in the GTA SA world for rhyremix.asi.

usage: lamp_list.py --game <GAME_DIR> --out <file> [--format v2|v1] [--inventory FILE]
                    [--table <path>] [--no-conjugate] [--report-missing]

THE TWO SOURCES, AND WHY v2 IS THE DEFAULT (n11)
------------------------------------------------
v2 (default) reads the GAME's own lamp data: every .dff in <game>/models/gta3.img
that carries a 2dFx LIGHT record (tools/dff_2dfx.py), placed by an IPL. The bulb
position, the colour and the fitting's own range come out of the model, so a lamp
the game draws gets a light whether or not some third party wrote it down.
Measured on this install: 145 models carry 2dFx LIGHT records (944 records), 112
of them are placed by an IPL, and the instances make 18691 world lights.

v1 (--format v1) is the old list: the SARemix Light Manager table
(<game>/moonloader/SARemix_Light_Manager.dat, a third-party Lua file, parsed with
regexes and never executed), which has 22 model ids. It covers 12042 of those
18691 lights = 64.4%; the other 6649 are lamp posts the game lights and our list
did not (finding D/E of the 2026-11-12 video review, V2 of the Verona Beach
video). --format v1 reproduces the old file byte for byte, which is the
compatibility proof in tests/test_lamp_list.py and the way to A/B the two.

Output v2: header "# rhyremix lights v2", then one line per light
        "type x y z r g b family range"
        type    = the size band the plugin's rhyStreetFixture() wants (1..12, band
                  = (type-1)%6/2: 0 cobra-head, 1 middle, 2 post-top/bollard)
        x y z   = the world position of the BULB: the instance transform applied to
                  the model's own 2dFx offset (same rule the game applies at 0x59C890)
        r g b   = the 2dFx colour bytes, the game's own colour for that bulb
        family  = the lamp family name the plugin knows (hps = high-pressure
                  sodium, tungsten, fluorescent, halide, mercury, neon)
        range   = the 2dFx record's own range float in game units (0 when the model
                  does not carry one)
Output v1: header "# rhyremix lights v1", then "type x y z".

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

try:  # the 2dFx reader lives next to this file
    import dff_2dfx
except ImportError:  # run from elsewhere: tools/ is not on sys.path
    dff_2dfx = None

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

# ---------------------------------------------------------------- 2dFx: the game's own lamp data

# The nine models the traffic-signal layer owns - the same list traffic_list.py
# writes its signal file from. Their 2dFx records are signal LENSES (measured:
# cj_traffic_light4 carries seven, three of them the red/amber/green lenses at
# (-3.24,-0.05,5.62/5.37/5.10)), and trafficlights.cpp + signalheads.cpp already
# put a bulb on every one of them from the signal list. Leaving them in the
# street list would burn a second light slot on the same lens.
TRAFFIC_NAMES = {
    "mtraffic1", "mtraffic2", "mtraffic3", "mtraffic4",
    "trafficlight1",
    "cj_traffic_light3", "cj_traffic_light4", "cj_traffic_light5",
    "gay_traffic_light",
}


def region_of_ipl(name):
    """Region of a "bnry" IPL inside gta3.img, from its stream name.

    Measured over all 164 .ipl entries of this install: the streaming names are
    la*/sf*/vegas*/country* plus the ocean floor and level map. Text IPLs get
    their region from the directory they live in (data/maps/LA, /SF, /vegas,
    /country, /interior, /leveldes).
    """
    n = name.lower()
    if n.startswith("la") or n == "carter.ipl":
        return "LA"
    if n.startswith("sf"):
        return "SF"
    if n.startswith("vegas"):
        return "vegas"
    if n.startswith(("country", "countn2", "truthsfarm")):
        return "country"
    if n.startswith(("seabed", "barriers", "levelmap")):
        return "leveldes"
    return "other"


def collect_instances(game):
    """(instances, ide) over EVERY IPL instance of the world.

    instances is [(model_id, pos, rot, region, source), ...] with interior != 0
    dropped (those are inside a building and the street layer never lights them);
    ide is {model_id: (name, radius)} from every .ide next to the .ipl files.
    The gta3.img index is read in one go before any entry is seeked: reading the
    index and the entries interleaved loses the index position and silently
    returns four instances instead of 36227.
    """
    maps = os.path.join(game, "data", "maps")
    ide = {}
    for root, _dirs, files in os.walk(maps):
        for fn in sorted(files):
            if fn.lower().endswith(".ide"):
                for mid, (name, radius) in parse_ide(os.path.join(root, fn)).items():
                    ide[mid] = (name, radius)
    name2id = {}
    for mid, (name, _radius) in ide.items():
        name2id.setdefault(name, mid)

    instances = []
    for root, _dirs, files in os.walk(maps):
        for fn in sorted(files):
            if not fn.lower().endswith(".ipl"):
                continue
            path = os.path.join(root, fn)
            rel = os.path.relpath(path, maps)
            region = rel.split(os.sep)[0]
            with open(path, "r", encoding="latin1", errors="replace") as ih:
                for name, interior, pos, rot, _lod in parse_text_ipl(ih.read()):
                    if interior != 0:
                        continue
                    mid = name2id.get(name.lower())
                    if mid is None:
                        continue
                    instances.append((mid, pos, rot, region, rel))

    img_path = os.path.join(game, "models", "gta3.img")
    with open(img_path, "rb") as f:
        magic, n = struct.unpack("<4sI", f.read(8))
        if magic != b"VER2":
            raise ValueError(f"{img_path}: not VER2")
        entries = [struct.unpack("<IHH24s", f.read(32)) for _ in range(n)]
        for off, sz, sz2, raw in entries:
            name = raw.split(b"\0", 1)[0].decode("latin1")
            if not name.lower().endswith(".ipl"):
                continue
            data = read_entry(f, off, sz, sz2)
            region = region_of_ipl(name)
            for model, interior, _lod, pos, rot in parse_binary_ipl(data):
                if interior != 0:
                    continue
                instances.append((model, pos, rot, region, name))
    return instances, ide


def dff_lights(game, names):
    """{model_name: [2dFx light records]} for the named .dff entries of gta3.img."""
    if dff_2dfx is None:
        raise SystemExit("tools/dff_2dfx.py must sit next to lamp_list.py")
    res, _stats = dff_2dfx.scan_img(os.path.join(game, "models", "gta3.img"),
                                    want=set(names))
    return res


# A real fitting's lamp type is a property of the fitting, and its 2dFx colour is
# the strongest evidence the game leaves for it: the deep orange of a sodium
# street lamp, the neutral white of a fluorescent globe post, the warm white of a
# tungsten sign lamp. The name rules come first where the model says what it is
# (a floodlight mast is a metal-halide fitting whatever its raster says).
_BIG = re.compile(r"flood|highmast|stadium|crane|frway|rooflite|roofmast|bigsfnlite", re.I)
_SMALL = re.compile(r"bollard|bench|cross|billbd|billboard|neon", re.I)
_HALIDE = re.compile(r"flood|highmast|stadium|crane|frway|rooflite|roofmast|bigsfnlite|sign", re.I)
_TUNGSTEN = re.compile(r"airport|helipad|runway|airpr|carrier|ship|freighter", re.I)


def lamp_family(name, rgb):
    """The lamp family for one 2dFx light: 'hps', 'tungsten', 'fluorescent',
    'halide', 'mercury' or 'neon'.

    Colour first, because that IS the spectrum the game chose for this bulb:
      neutral white (blue >= 0.9 x red, channels within a quarter of each other)
        -> fluorescent: the white globe posts of the boardwalk and the pier rail.
      warm white (red high, blue still over half of red) -> tungsten.
      deep orange (red high, green 45-85% of red, blue under half) -> hps, the
        sodium street lamp, which is what most of Los Santos is.
      anything else saturated (pure red, green, blue, magenta) -> neon: signage,
        crossing signals, refinery obstruction lights.
    Then the model name, because a floodlight mast is a metal-halide fitting and
    an airfield edge light is a tungsten one whatever its recorded colour looks
    like. A record with no colour at all (0,0,0) falls back to sodium, which is
    what the street it stands on is made of.
    """
    r, g, b = (float(v) for v in rgb[:3])
    mx = max(r, g, b)
    if mx < 40.0:
        return "hps"
    if _HALIDE.search(name or ""):
        return "halide"
    if _TUNGSTEN.search(name or ""):
        return "tungsten"
    if re.search(r"neon", name or "", re.I):
        return "neon"
    spread = mx - min(r, g, b)
    if spread <= 0.25 * mx:                       # achromatic
        return "fluorescent" if b >= 0.9 * r else "tungsten"
    if r >= 0.9 * mx and 0.45 * mx <= g <= 0.85 * mx and b <= 0.55 * mx:
        return "hps"
    if r >= 0.85 * mx and b >= 0.45 * mx:
        return "tungsten"
    return "neon"


def size_band(name, fx_range):
    """0 cobra-head, 1 middle, 2 post-top/bollard, from the 2dFx range float and
    the model name. Measured ranges: 62 (vegaslampost), 100 (the LA lampposts),
    150 (lampost_coast), 180 (bollardlight), 300 (lamppost2's globes), 450-900
    (masts and airfields). The name rules fix the two cases the range gets
    backwards: a floodlight mast at range 100 and a bollard at range 180."""
    if _BIG.search(name or "") or (fx_range or 0) >= 200.0:
        return 0
    if _SMALL.search(name or "") or 0 < (fx_range or 0) <= 70.0:
        return 2
    return 1


def world_2dfx_lights(instances, ide, lights):
    """Every 2dFx light of every placed object, in world coordinates.

    position = instance position + instance quaternion x the model's own 2dFx
    offset, which is the rule the game applies to a 2dFx entry at 0x59C890
    (out = m->right*e.x + m->up*e.y + m->at*e.z + m->pos). The quaternion is
    conjugated first, exactly as the traffic list does for a lens offset.
    """
    out = []
    for mid, pos, rot, region, source in instances:
        name = ide.get(mid, ("", 0.0))[0]
        recs = lights.get(name)
        if not recs:
            continue
        q = (-rot[0], -rot[1], -rot[2], rot[3])
        for r in recs:
            rx, ry, rz = rotate((r["x"], r["y"], r["z"]), q)
            out.append({
                "model": name, "model_id": mid, "region": region, "source": source,
                "x": pos[0] + rx, "y": pos[1] + ry, "z": pos[2] + rz,
                "r": r.get("r", 0), "g": r.get("g", 0), "b": r.get("b", 0),
                "range": r.get("range", 0.0), "flags": r.get("flags") or [],
                "family": lamp_family(name, (r.get("r", 0), r.get("g", 0), r.get("b", 0))),
                "band": size_band(name, r.get("range", 0.0)),
            })
    return out


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

def load_table(args):
    """The third-party lamp table (Lua text, parsed with regexes, never executed).

    v1 needs it; v2 does not, so a missing file is only fatal for --format v1.
    """
    path = args.table or os.path.join(args.game, "moonloader",
                                      "SARemix_Light_Manager.dat")
    if not os.path.exists(path):
        return None
    with open(path, "r", encoding="latin1") as tf:
        return parse_lamp_table(tf.read())


def write_v1(args, instances, lamps):
    """The old list: one line per lamp-table offset, "type x y z"."""
    table_inst = [(mid, pos, rot) for mid, pos, rot, _reg, _src in instances
                  if mid in lamps]
    print(f"table instances: {len(table_inst)}")

    # Drop LOD-only duplicates before the offsets are applied.
    kept, dupes = dedupe_positions([(pos, (model, rot)) for model, pos, rot in table_inst])
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
    write_list(args.out, out_lines)
    print(f"lights written: {len(out_lines) - 1}")
    print(f"lights within 300 m (2D) of (2495, -1680): {near}")
    return lights


def write_v2(args, inv):
    """The 2dFx list: one line per light the GAME says exists.

        type x y z r g b family range

    Traffic-signal models are left out: the signal layer already puts a bulb on
    every one of those lenses (see TRAFFIC_NAMES).
    """
    street = [i for i in inv if i["model"] not in TRAFFIC_NAMES]
    print(f"2dFx lights kept for the street layer: {len(street)} "
          f"(signal models dropped: {len(inv) - len(street)})")
    kept, dup_out = dedupe_positions([((i["x"], i["y"], i["z"]), i) for i in street])
    print(f"duplicate light positions dropped: {dup_out}")

    out_lines = [
        "# rhyremix lights v2",
        "# type x y z r g b family range",
        "#   type   size band (rhyStreetFixture: band = (type-1)%6/2)",
        "#   r g b  the 2dFx colour of this bulb, straight out of the model",
        "#   family the lamp family (hps = high-pressure sodium, tungsten,",
        "#          fluorescent, halide, mercury, neon)",
        "#   range  the 2dFx record's own range float, game units",
    ]
    near = 0
    for pos, i in kept:
        out_lines.append(f"{1 + 2 * i['band']} {pos[0]:.3f} {pos[1]:.3f} {pos[2]:.3f} "
                         f"{i['r']} {i['g']} {i['b']} {i['family']} {i['range']:.1f}")
        dx, dy = pos[0] - 2495.0, pos[1] + 1680.0
        if dx * dx + dy * dy < 300.0 * 300.0:
            near += 1
    write_list(args.out, out_lines)
    print(f"lights written: {len(out_lines) - len(out_lines[:7])}")
    print(f"lights within 300 m (2D) of (2495, -1680): {near}")
    return kept


def write_list(path, lines):
    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    with open(path, "w") as ofh:
        ofh.write("\n".join(lines) + "\n")


def write_inventory(path, inv):
    """The 2dFx world inventory, one light per line: the input of lamp-coverage.py."""
    write_list(path, ["# rhyremix 2dfx inventory v1",
                      "# model region x y z r g b range"] +
               [f"{i['model']} {i['region']} {i['x']:.3f} {i['y']:.3f} {i['z']:.3f} "
                f"{i['r']} {i['g']} {i['b']} {i['range']:.1f}" for i in inv])
    print(f"2dFx inventory written: {len(inv)} lights -> {path}")


def report_missing(inv, lamps, ide):
    """Every 2dFx-lit model the third-party lamp table does not cover, with counts."""
    per_model = {}
    for i in inv:
        per_model.setdefault(i["model"], [0, i["model_id"], 0])
        per_model[i["model"]][0] += 1
        if i["model"] not in TRAFFIC_NAMES:
            per_model[i["model"]][2] += 1
    covered = sum(1 for i in inv if i["model_id"] in (lamps or {}))
    print(f"lamp table model ids: {len(lamps or {})}")
    print(f"2dFx-lit models placed by IPL: {len(per_model)}; "
          f"2dFx light instances: {len(inv)}; covered by the table: {covered} "
          f"({100.0 * covered / max(1, len(inv)):.1f}%)")
    missing = sorted(((v[0], v[2], k, v[1]) for k, v in per_model.items()
                      if v[1] not in (lamps or {})), reverse=True)
    print(f"2dFx-lit models with NO table entry: {len(missing)}")
    for total, street, name, mid in missing[:40]:
        print(f"  {name:<26s} id={mid:<6d} 2dfx_lights={total:<6d} "
              f"street_lights={street:<6d} .ide_radius={ide.get(mid, ('', 0))[1]}")
    return missing


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--game", required=True, help="game directory")
    ap.add_argument("--table", default=None,
                    help="SARemix_Light_Manager.dat (Lua table); default is "
                         "<game>/moonloader/SARemix_Light_Manager.dat")
    ap.add_argument("--out", required=True, help="output light list")
    ap.add_argument("--format", choices=("v2", "v1"), default="v2",
                    help="v2 (default) comes from the game's own 2dFx records; "
                         "v1 is the old third-party lamp table list")
    ap.add_argument("--inventory", default=None,
                    help="also write the 2dFx world inventory here "
                         "(what tools/lamp-coverage.py compares against)")
    ap.add_argument("--no-conjugate", dest="conjugate", action="store_false", default=True,
                    help="do not conjugate instance quaternions before rotating offsets")
    ap.add_argument("--report-missing", action="store_true",
                    help="list 2dFx-lit models that have instances but no lamp table entry")
    args = ap.parse_args()

    instances, ide = collect_instances(args.game)
    print(f"IPL instances (interior 0): {len(instances)}; .ide models: {len(ide)}")

    names = {ide.get(mid, ("", 0.0))[0] for mid, _p, _r, _reg, _s in instances}
    lights = dff_lights(args.game, {n for n in names if n})
    inv = world_2dfx_lights(instances, ide, lights)
    print(f"models with 2dFx LIGHT records placed by IPL: "
          f"{len({i['model'] for i in inv})}; 2dFx light instances: {len(inv)}")

    if args.inventory:
        write_inventory(args.inventory, inv)

    lamps = load_table(args)
    if args.report_missing:
        report_missing(inv, lamps, ide)

    if args.format == "v1":
        if not lamps:
            sys.exit(f"--format v1 needs the lamp table; not found at "
                     f"{args.table or '<game>/moonloader/SARemix_Light_Manager.dat'}. "
                     "It ships with the GTASA-Remix pack and is deliberately not "
                     "copied into this repo.")
        write_v1(args, instances, lamps)
    else:
        write_v2(args, inv)


if __name__ == "__main__":
    main()
