#!/usr/bin/env python3
"""n20 - which surface textures actually fill the frame, measured from the game's own data.

usage:
  tools/texture_rank.py --game <GAME_DIR> [--out build/tmp/n20/rank.json] [--top 40]
                        [--radius 200] [--cone 45] [--sensitivity] [--pack-census]
                        [--stops LABEL[,LABEL]] [--csv FILE]

Why not "look at the screenshots": a screenshot says what is bright, not what texture it is.
The game's data says exactly which geometry stands where, so this tool measures the same
question from that side and reports it as a screen-share estimate:

  1. every map instance in the world - the text IPLs under <game>/data/maps/**/*.ipl plus the
     binary ("bnry") IPLs inside <game>/models/gta3.img (same reader and the same .ide id
     resolution as tools/lamp_list.py: the position in the .img index is NOT the model id);
  2. keep the instances inside a cone around each tour camera's look direction, inside a
     radius - the cameras are the 16 stops of <game>/moonloader/SARemix_Tour.lua, read as text
     (never executed), so the numbers describe the shots in screenshots/shots/ that
     docs/TARGET.md measures;
  3. project each kept instance's triangles onto the view direction and divide by d^2, i.e.
     sum the SOLID ANGLE each surface subtends - "share of the frame" in physical terms - and
     charge it to the textures its model's materials use.

What that is NOT: it is not a render. There is no occlusion (a wall does not hide the block
behind it), no per-triangle material split (SA's map geometry stores triangles as 4-byte
records whose 4th byte is 0xff in every model checked, so the per-triangle material index is
only reachable through librw's binMesh decoder - measured on lae2_roads89: the geometry struct
is 7392 bytes = 900 + 900 + 24 + 2784 + 2784, which closes with two index arrays and one morph
target and leaves no room for a material array), and no UV area. So a model's triangles are
split evenly over its distinct textures, and the honest claim is "the textures that cover the
most geometry in front of the tour cameras", which is what "most visible surfaces" means for a
texture replacement. --sensitivity re-ranks at several cone angles and radii and prints the
Jaccard overlap of the top-N sets, so the reader can see how much the answer depends on the
parameters.

Output: JSON (default build/tmp/n20/rank.json) with one entry per texture:
  name, txd (where it was found), hash (Remix's XXH3 over mip 0, tools/remix_texhash.py),
  class (material class, drives roughness in tools/pbrify.py), score, share %, models, tris.
"""
from __future__ import annotations

import argparse
import collections
import glob
import json
import math
import os
import re
import struct
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lamp_list as LL                      # noqa: E402
import remix_texhash as RT                  # noqa: E402

# --------------------------------------------------------------------- tour cameras

# The 16 stops of the tour as of 2026-11-13, used when the game folder is not readable
# (tests, and the fake tree the self-tests build). Same numbers as
# <game>/moonloader/SARemix_Tour.lua; parse_tour_stops() is what reads the real file.
DEFAULT_STOPS = [
    ("00-baseline-noon-grove", (2440.0, -1672.0, 16.0), (2500.0, -1668.0, 13.5)),
    ("01-under-lamp-grove-2100", (2465.5, -1682.0, 16.5), (2471.7, -1673.7, 13.5)),
    ("02-grove-30m-pools-2100", (2438.0, -1671.0, 15.5), (2500.0, -1668.0, 13.5)),
    ("03-sodium-lamp-closeup-2100", (2467.0, -1679.0, 18.5), (2471.7, -1673.7, 19.8)),
    ("04-tungsten-shopfront-2100", (2300.0, -1652.0, 15.8), (2309.8, -1643.5, 14.8)),
    ("05-burger-shot-marina-2100", (776.0, -1623.5, 14.0), (795.0, -1628.3, 15.0)),
    ("06-signals-close-2100", (1036.0, -1394.0, 16.0), (1061.6, -1416.1, 15.0)),
    ("07-signals-cross-street-2100", (1070.0, -1432.0, 16.0), (1052.8, -1385.5, 15.0)),
    ("08a-sunset-facing-east-1905", (368.0, -2042.0, 10.0), (600.0, -2000.0, 30.0)),
    ("08b-sunset-facing-west-1905", (372.0, -2040.0, 10.0), (100.0, -2060.0, 15.0)),
    ("09-grove-dawn-0530", (2438.0, -1671.0, 15.5), (2500.0, -1668.0, 13.5)),
    ("10-water-santa-maria-noon", (376.0, -1963.9, 8.4), (150.0, -1963.9, 2.0)),
    ("11-water-santa-maria-2100", (376.0, -1963.9, 8.4), (150.0, -1963.9, 2.0)),
    ("12-car-mirror-grove-1300", (2495.5, -1674.5, 14.0), (2487.0, -1676.5, 13.3)),
    ("13-ped-crowd-pershing-noon", (1479.0, -1653.5, 14.4), (1479.5, -1626.0, 13.6)),
    ("14-vegetation-vinewood-noon", (1296.0, -916.0, 42.5), (1330.0, -888.0, 41.0)),
]

# the daytime stops: the ones a texture change is judged on (docs/TARGET.md measures daytime)
DAY_STOPS = ["00-baseline-noon-grove", "12-car-mirror-grove-1300",
             "13-ped-crowd-pershing-noon", "14-vegetation-vinewood-noon"]

STOP_RE = re.compile(
    r'\{\s*"([^"]+)"\s*,\s*\d+\s*,\s*\d+\s*,'                       # name, hour, minute
    r'\s*(-?[\d.]+)\s*,\s*(-?[\d.]+)\s*,\s*(-?[\d.]+)\s*,'          # CJ x y z
    r'\s*(-?[\d.]+)\s*,\s*(-?[\d.]+)\s*,\s*(-?[\d.]+)\s*,'          # camera x y z
    r'\s*(-?[\d.]+)\s*,\s*(-?[\d.]+)\s*,\s*(-?[\d.]+)')             # look-at x y z


def parse_tour_stops(lua_text):
    """[(name, camera, look-at)] from the tour script's `stops` table. Text only, never run."""
    out = []
    for m in STOP_RE.finditer(lua_text):
        g = [float(x) for x in m.groups()[1:]]
        out.append((m.group(1), tuple(g[3:6]), tuple(g[6:9])))
    return out


def load_stops(game):
    path = os.path.join(game, "moonloader", "SARemix_Tour.lua")
    try:
        with open(path, encoding="utf-8", errors="replace") as fh:
            stops = parse_tour_stops(fh.read())
        if stops:
            return stops, path
    except OSError:
        pass
    return list(DEFAULT_STOPS), "<built-in table>"


# --------------------------------------------------------------------- .dff materials

def chunks(data, off, end):
    while off + 12 <= end:
        typ, ln, ver = struct.unpack_from("<III", data, off)
        if typ == 0 or ln == 0 or off + 12 + ln > end:
            return
        yield typ, off + 12, ln, off + 12 + ln
        off += 12 + ln


def _find(data, typ, off=0, end=None, out=None):
    if out is None:
        out = []
    end = len(data) if end is None else end
    for t, po, ln, nxt in chunks(data, off, end):
        if t == typ:
            out.append((po, ln, nxt))
        _find(data, typ, po, nxt, out)
    return out


def geoms(data):
    """[(flags, ntri, nvert, payload_offset)] for the geometry structs of a .dff.

    Same byte accounting as tools/dff_stats.py: the struct header is
    flags(u32) ntri(i32) nvert(i32) nmorph(i32), and SA's map geometry all carries
    flags = 0x1002f (hasNormals|hasTexCoords|hasBinMesh|...).
    """
    out = []
    for po, ln, nxt in _find(data, 0x00000001):
        if ln < 16:
            continue
        flags, ntri, nvert, nmorph = struct.unpack_from("<Iiii", data, po)
        if 0 < nvert < 500000 and 0 <= ntri < 1000000 and 1 <= nmorph <= 8:
            out.append((flags, ntri, nvert, po + 12, ln - 16))
    return out


def parse_dff(data):
    """([texture names], [(area_m2, tris, normals, areas)]) for one .dff.

    The texture names are the first binSTRING of each binMATERIAL chunk. The geometry is read
    from the struct's payload, whose size closes exactly as

        4*ntri + 4*ntri + 24 + 12*nvert + 12*nvert

    (two 4-byte triangle records per triangle, then the morph-target header, the positions and
    the normals). Measured on lae2_roads89 (225/232 -> 7392), pershingsq1_lan (718/1072 ->
    31496), lamppost3 (46/76 -> 2216), telgrphpole02 (274/276 -> 8840). The first record is
    [v0][v1][v2][flag] with u8 indices and flag 0x00/0xff (a strip restart marker, not a
    material index: every model checked has 2-5 materials and the byte is only ever 0 or 255),
    which reproduces the model's real extent and area - a lamppost3 bbox of 2.9 x 0.3 x 7.8 m,
    lae2_roads89 51 x 41 x 0.4 m and 1224 m2, pershingsq1_lan 90 x 68 x 6.7 m and 6008 m2 for a
    6120 m2 plaza. So the positions and the triangle soup are trustworthy even though the
    per-triangle material index is not (see the module docstring).
    """
    tex = []
    for po, ln, nxt in _find(data, 0x00000007):                 # binMATERIAL
        for po2, ln2, _n2 in _find(data, 0x00000002, po, nxt):  # binSTRING
            name = data[po2:po2 + ln2].split(b"\0", 1)[0].decode("latin1", "replace")
            if len(name) > 2 and not name.startswith("\0"):
                if name not in tex:
                    tex.append(name)
                break
    out = []
    for flags, ntri, nvert, po, payload in geoms(data):
        if payload != 8 * ntri + 24 + 24 * nvert:
            continue                       # an odd geometry we cannot account for: skip it
        idx = np.frombuffer(data, np.uint8, 4 * ntri, po).reshape(ntri, 4)
        pos = np.frombuffer(data, np.float32, 3 * nvert, po + 8 * ntri + 24).reshape(nvert, 3)
        nrm = np.frombuffer(data, np.float32, 3 * nvert, po + 8 * ntri + 24 + 12 * nvert)
        nrm = nrm.reshape(nvert, 3)
        if idx[:, :3].max() >= nvert or not np.isfinite(pos).all():
            continue
        if np.abs(pos).max() > 1e5:
            # a geometry whose vertex coordinates are not metres (a stream we mis-read): the
            # byte accounting above says it should fit, so refuse rather than rank on garbage
            continue
        a, b, c = pos[idx[:, 0]], pos[idx[:, 1]], pos[idx[:, 2]]
        n = np.cross(b - a, c - a)
        areas = 0.5 * np.linalg.norm(n, axis=1)
        if not np.isfinite(areas).all():
            continue
        nrm_tri = np.where(areas[:, None] > 0, n / np.maximum(np.linalg.norm(n, axis=1)[:, None], 1e-9),
                           0.0)
        out.append((float(areas.sum()), int(ntri), nrm_tri, areas))
    return tex, out


# --------------------------------------------------------------------- material classes

# Roughness / metallic per material class. The numbers are the usual measured ranges for these
# materials (asphalt and concrete are very rough dielectrics, polished metal is not) and the
# task's own starting values (asphalt ~0.85, glass ~0.05, metal ~0.3 + metallic); docs/TEXTURES.md
# lists where each one comes from. `strength` is how much normal detail the class gets.
# Roughness / metallic / normal strength per material class. The numbers are NOT invented: they
# are the medians of the installed pack's own maps (Hemry81/GTASA-Remix, MIT), measured per
# category with `tools/pbrify.py --calibrate` - e.g. asphalt rough 0.588 +- 0.107 and normal
# std 39.1/255, wall 0.681 / 24.8, pavement 0.655 / 25.7, ground 0.735 / 27.0, windows 0.060 /
# 0.7, metal 0.507 / 17.0, rock 0.680 / 48.2 (build/tmp/n20/pack-calibration.txt). `strength`
# is then set so our derived normal maps land on the same std (measured relation: std ~=
# 16.8 * strength with the amplitude in tools/pbrify.py derive_normal). Where the pack has no
# category we use the closest one it does have. docs/TEXTURES.md has the table.
CLASSES = {
    "asphalt":   dict(rough=0.59, rough_var=0.11, metal=0.0, strength=2.3),
    "concrete":  dict(rough=0.51, rough_var=0.05, metal=0.0, strength=0.9),
    "pavement":  dict(rough=0.66, rough_var=0.09, metal=0.0, strength=1.6),
    "kerb":      dict(rough=0.62, rough_var=0.08, metal=0.0, strength=1.4),
    "roadmark":  dict(rough=0.55, rough_var=0.05, metal=0.0, strength=0.5),
    "wall":      dict(rough=0.68, rough_var=0.07, metal=0.0, strength=1.5),
    "brick":     dict(rough=0.68, rough_var=0.09, metal=0.0, strength=1.7),
    "stucco":    dict(rough=0.70, rough_var=0.07, metal=0.0, strength=1.6),
    "glass":     dict(rough=0.06, rough_var=0.06, metal=0.0, strength=0.05,
                      shader="translucent"),
    "metal":     dict(rough=0.51, rough_var=0.04, metal=1.0, strength=1.0),
    "roof":      dict(rough=0.69, rough_var=0.13, metal=0.0, strength=1.5),
    "rock":      dict(rough=0.68, rough_var=0.09, metal=0.0, strength=2.9),
    "ground":    dict(rough=0.74, rough_var=0.06, metal=0.0, strength=1.6),
    "dirt":      dict(rough=0.75, rough_var=0.06, metal=0.0, strength=1.6),
    "sand":      dict(rough=0.66, rough_var=0.05, metal=0.0, strength=1.7),
    "grass":     dict(rough=0.74, rough_var=0.06, metal=0.0, strength=1.5),
    "wood":      dict(rough=0.54, rough_var=0.06, metal=0.0, strength=1.0),
    "tile":      dict(rough=0.42, rough_var=0.08, metal=0.0, strength=0.5),
    "paint":     dict(rough=0.28, rough_var=0.10, metal=0.0, strength=0.6),
    "other":     dict(rough=0.55, rough_var=0.08, metal=0.0, strength=1.2),
}

CLASS_WORDS = [
    ("glass", ("glass", "window", "windscreen", "mirror_g")),
    ("metal", ("metal", "steel", "iron", "alu", "chrome", "railway", "fence_m")),
    ("roadmark", ("stopline", "crossing", "roadline", "road_arrow", "markings", "zebra")),
    ("asphalt", ("tarmac", "asphalt", "road", "highway", "hiway", "freeway", "motorway",
                 "street", "runway")),          # "hiway" is SA's own abbreviation (cos_hiwayout)
    ("concrete", ("concrete", "cement", "conc", "slab")),
    ("kerb", ("kerb", "curb", "gutter")),
    ("pavement", ("pavement", "paving", "pave", "sidewalk", "footpath", "flagstone", "walkway",
                  "plaza", "marb")),
    ("brick", ("brick", "blockwork")),
    ("stucco", ("stucco", "render", "plaster")),
    ("wall", ("wall", "walj", "fence", "brickwall")),
    ("roof", ("roof", "tile_r", "shingle", "slate")),
    ("rock", ("rock", "stone", "cliff", "boulder")),
    ("sand", ("sand", "beach")),
    ("grass", ("grass", "lawn", "turf", "meadow", "hedge")),
    ("dirt", ("dirt", "mud", "earth", "soil")),
    ("ground", ("ground", "land", "grnd", "floor", "alley", "yard")),
    ("wood", ("wood", "plank", "timber", "deck")),
    ("tile", ("tile", "ceramic", "terrazzo")),
    ("paint", ("paint", "emissive", "sign")),
]


def classify(name, models=()):
    """Material class of a texture: its own name first, then the names of the models that use it.

    SA's texture names are descriptive (plaintarmac1, tanstucco1_LA, dockpave_256) but not all
    of them are (sjmlahus28, pershing1_LAn). The model names carry the same information
    (lae2_roads89, pershingsq1_lan, cunte_house1), so they are the fallback.
    """
    for probe in (name.lower(), " ".join(models)):
        for cls, words in CLASS_WORDS:
            if any(w in probe for w in words):
                return cls
    return "other"


# the surfaces the task names: roads, pavements, kerbs, walls, building glass, terrain ground
SURFACE_CLASSES = ("asphalt", "concrete", "pavement", "kerb", "roadmark", "wall", "brick",
                   "stucco", "glass", "roof", "rock", "ground", "dirt", "sand", "grass", "tile")


def is_lod(name):
    n = name.lower()
    return n.startswith("lod") or "_lod" in n or "lod_" in n


# --------------------------------------------------------------------- the world index

def build_index(game, verbose=True):
    """[(model_name, (x,y,z), quaternion(x,y,z,w))] for every map instance in the world."""
    ide = {}
    for p in sorted(glob.glob(os.path.join(game, "data", "maps", "**", "*.ide"), recursive=True)):
        ide.update(LL.parse_ide(p))
    inst = []
    for p in sorted(glob.glob(os.path.join(game, "data", "maps", "**", "*.ipl"), recursive=True)):
        with open(p, encoding="latin1", errors="replace") as fh:
            for name, _interior, pos, rot, _lod in LL.parse_text_ipl(fh.read()):
                inst.append((name.lower(), pos, rot))
    n_text = len(inst)
    img = os.path.join(game, "models", "gta3.img")
    if os.path.exists(img):
        _ents = LL.read_img_index(img)[1]
        with open(img, "rb") as fh:
            for e in _ents:
                if not e[1].lower().endswith(".ipl"):
                    continue
                blob = LL.read_entry(fh, e[2], e[3], e[4])
                for model, _interior, _lod, pos, _rot in LL.parse_binary_ipl(blob):
                    ent = ide.get(model)             # parse_ide gives {id: (name, radius_cm)}
                    if ent:
                        inst.append((ent[0], pos, rot))
    if verbose:
        print("instances: %d text IPL + %d binary IPL = %d, %d distinct models"
              % (n_text, len(inst) - n_text, len(inst), len(set(n for n, _p, _q in inst))))
    return inst


def texture_index(game, want=None, verbose=True):
    """{texture_name_lower: (txd_label, hash_upper_hex, w, h)} for the textures of the game.

    Scans the loose .txd under models/ and every .txd inside models/gta3.img, gta_int.img and
    player.img. `want` (a set of names) stops the scan early per file. The hash is Remix's:
    XXH3_64bits over mip 0 exactly as the D3D9 device receives it (tools/remix_texhash.py, s18).
    """
    out = {}
    files = []
    mdir = os.path.join(game, "models")
    for root, _dirs, names in os.walk(mdir):
        for n in names:
            if n.lower().endswith(".txd"):
                files.append(os.path.join(root, n))
    imgs = [os.path.join(mdir, x) for x in ("gta3.img", "gta_int.img", "player.img", "cutscene.img")]
    for img in imgs:
        if not os.path.exists(img):
            continue
        with open(img, "rb") as fh:
            for e in LL.read_img_index(img)[1]:
                if e[1].lower().endswith(".txd"):
                    files.append((img, e))
    for f in files:
        if isinstance(f, tuple):
            img, e = f
            with open(img, "rb") as fh:
                blob = LL.read_entry(fh, e[2], e[3], e[4])
            label = "%s:%s" % (os.path.basename(img), e[1])
        else:
            with open(f, "rb") as fh:
                blob = fh.read()
            label = os.path.relpath(f, game)
        try:
            texs = RT.parse_txd(blob, label)
        except ValueError:
            continue
        for t in texs:
            key = t.name.lower()
            if want is not None and key not in want:
                continue
            if key in out:
                continue
            try:
                h = RT.xxh3_64bits(t.d3d9_mip0(blob))
            except Exception:
                continue
            out[key] = (label, "%016X" % h, t.w, t.h)
    if verbose:
        print("texture hashes resolved: %d" % len(out))
    return out


# --------------------------------------------------------------------- ranking

def rank(game, stops, radius=200.0, cone_deg=45.0, verbose=True):
    """{texture: {...}} - the surface each texture covers in front of the tour cameras.

    For every instance inside the cone, the model's triangles are projected onto the view
    direction and divided by d^2: that is the solid angle the surface subtends, which is what
    "share of the frame" means (times the frame's own solid angle). Per-triangle normals come
    from the geometry, the instance rotation from the IPL quaternion. The model's area is split
    evenly over its distinct textures (no per-triangle material index, see the docstring).
    `area` is the world m2 in the cone, `solid` the summed solid angle in steradians.
    """
    inst = build_index(game, verbose=verbose)
    by_model = collections.defaultdict(list)
    for name, pos, quat in inst:
        by_model[name].append((pos, quat))
    img_path = os.path.join(game, "models", "gta3.img")
    img = {}
    if os.path.exists(img_path):
        img = {e[1].lower(): e for e in LL.read_img_index(img_path)[1]}
    fh = open(img_path, "rb") if img else None
    cache = {}

    def model_info(name):
        if name not in cache:
            info = ([], [])
            e = img.get(name) or img.get(name + ".dff")
            if e:
                info = parse_dff(LL.read_entry(fh, e[2], e[3], e[4]))
            cache[name] = info
        return cache[name]

    res = collections.defaultdict(lambda: dict(solid=0.0, area=0.0, models=0, tris=0,
                                               per_stop={}, used_by=[]))
    kept_models = 0
    for label, cam, look in stops:
        fwd = [look[i] - cam[i] for i in range(3)]
        ln = math.sqrt(sum(x * x for x in fwd)) or 1.0
        fwd = np.array([x / ln for x in fwd])
        cosmin = math.cos(math.radians(cone_deg))
        for name, entries in by_model.items():
            if is_lod(name):
                continue
            tex, gs = model_info(name)
            if not tex or not gs:
                continue
            solid = 0.0
            area = 0.0
            hits = 0
            for pos, quat in entries:
                off = np.array([pos[0] - cam[0], pos[1] - cam[1], pos[2] - cam[2]])
                d = float(np.linalg.norm(off))
                if d > radius or d < 1e-3:
                    continue
                if float(np.dot(off, fwd)) / d < cosmin:
                    continue
                # the view direction in the model's own frame: cheaper than rotating every normal
                v = LL.rotate(tuple(-off / d), tuple(quat))
                for g_area, g_tris, nrm, areas in gs:
                    solid += float(np.dot(areas, np.abs(np.dot(nrm, v)))) / (d * d)
                area += sum(g_area for g_area, _t, _n, _a in gs)
                hits += 1
            if not hits:
                continue
            kept_models += 1
            for t in tex:
                r = res[t.lower()]
                r["solid"] += solid / len(tex)
                r["area"] += area / len(tex)
                r["models"] += hits
                r["tris"] += sum(g_tris for _a, g_tris, _n, _x in gs) * hits / len(tex)
                r["per_stop"][label] = r["per_stop"].get(label, 0.0) + solid / len(tex)
                # which models use it: SA model names say what the surface IS (lae2_roads89,
                # cunte_house1, ...), which is a better class signal than the texture name alone
                if len(r["used_by"]) < 12:
                    r["used_by"].append(name)
    if fh:
        fh.close()
    total = sum(r["solid"] for r in res.values()) or 1.0
    for r in res.values():
        r["share"] = 100.0 * r["solid"] / total
    if verbose:
        print("models in the cones: %d, textures with geometry: %d" % (kept_models, len(res)))
    return res


def pack_census(game):
    """{hash_upper_hex: category} for every material the Remix pack already replaces."""
    root = os.path.join(game, "rtx-remix", "mods", "gameReadyAssets", "texture")
    out = {}
    if not os.path.isdir(root):
        return out
    for d in sorted(os.listdir(root)):
        p = os.path.join(root, d)
        if not os.path.isdir(p):
            continue
        for fn in os.listdir(p):
            m = re.match(r"^([0-9A-Fa-f]{16})_(albedo|normal|rough|height|metal)\.dds$", fn)
            if m:
                out.setdefault(m.group(1).upper(), set()).add(d)
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0], add_help=True)
    ap.add_argument("--game", required=True)
    ap.add_argument("--out", default="build/tmp/n20/rank.json")
    ap.add_argument("--top", type=int, default=40)
    ap.add_argument("--radius", type=float, default=200.0)
    ap.add_argument("--cone", type=float, default=45.0)
    ap.add_argument("--stops", default=",".join(DAY_STOPS),
                    help="comma list of stop names, or 'all' (default: the four daytime stops)")
    ap.add_argument("--sensitivity", action="store_true")
    ap.add_argument("--all-classes", action="store_true",
                    help="rank every texture, not only the surface classes (roads/pavement/wall/...)")
    ap.add_argument("--pack-census", action="store_true")
    ap.add_argument("--csv", default=None)
    ap.add_argument("--quiet", action="store_true")
    a = ap.parse_args(argv)

    stops, src = load_stops(a.game)
    if a.stops.strip() != "all":
        want = set(x.strip() for x in a.stops.split(","))
        stops = [s for s in stops if s[0] in want]
    print("tour cameras: %d stops from %s" % (len(stops), src))
    res = rank(a.game, stops, radius=a.radius, cone_deg=a.cone, verbose=not a.quiet)
    order = sorted(res.items(), key=lambda kv: -kv[1]["solid"])
    if not a.all_classes:
        order = [(n, r) for n, r in order if classify(n, r["used_by"]) in SURFACE_CLASSES]
    top = order[:a.top]
    census = pack_census(a.game) if a.pack_census else {}

    hashes = texture_index(a.game, want=set(n for n, _ in top), verbose=not a.quiet)
    entries = []
    for name, r in top:
        info = hashes.get(name)
        # the key is "class", not "class_": tools/pbrify.py reads e["class"] and the selection
        # list in config/texture-pbr.json uses it. `class=` is a keyword so it has to be spelled
        # through the ** dict form (n20b: it was written as class_, which made the documented
        # regeneration path - rank.json straight into pbrify --list - raise KeyError).
        e = dict(name=name, solid=round(r["solid"], 4), area=round(r["area"], 1),
                 share=round(r["share"], 4), models=int(r["models"]),
                 tris=int(r["tris"]), used_by=r["used_by"],
                 **{"class": classify(name, r["used_by"])},
                 per_stop={k: round(v, 4) for k, v in sorted(r["per_stop"].items())})
        if info:
            e.update(txd=info[0], hash="0x" + info[1], w=info[2], h=info[3])
            e["pack_has"] = sorted(census.get(info[1], [])) if census else None
        else:
            e.update(txd=None, hash=None, w=None, h=None, pack_has=None)
        entries.append(e)

    data = dict(tool="tools/texture_rank.py", game=a.game, radius=a.radius, cone=a.cone,
                classes=("all" if a.all_classes else list(SURFACE_CLASSES)),
                stops=[s[0] for s in stops], total_textures=len(res),
                note=("score = solid angle (steradians) of the geometry inside the camera cone, "
                      "projected onto the view direction and split evenly over the model's "
                      "textures; no occlusion, no per-triangle material split"),
                textures=entries)
    os.makedirs(os.path.dirname(a.out) or ".", exist_ok=True)
    with open(a.out, "w") as fh:
        json.dump(data, fh, indent=1)
    print("wrote %s (%d textures ranked, top %d written)" % (a.out, len(res), len(entries)))
    print("\n%-26s %8s %9s %7s  %-9s %-9s %s"
          % ("texture", "share%", "m2", "models", "class", "size", "remix hash"))
    for e in entries:
        print("%-26s %8.3f %9.0f %7d  %-9s %-9s %s"
              % (e["name"][:26], e["share"], e["area"], e["models"], e["class"],
                 "%dx%d" % (e["w"], e["h"]) if e["w"] else "?", e["hash"] or "-"))
    if a.csv:
        with open(a.csv, "w") as fh:
            fh.write("name,class,solid,area,share,models,tris,txd,hash,w,h\n")
            for e in entries:
                fh.write("%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n" % (
                    e["name"], e["class"], e["solid"], e["area"], e["share"], e["models"],
                    e["tris"], e["txd"] or "", e["hash"] or "", e["w"] or "", e["h"] or ""))
        print("wrote %s" % a.csv)

    if a.sensitivity:
        print("\nsensitivity of the top-%d set (Jaccard overlap against the chosen setting):" % a.top)
        base = set(e["name"] for e in entries)
        for cone in (35.0, 45.0, 55.0):
            for rad in (100.0, 200.0, 300.0):
                r2 = rank(a.game, stops, radius=rad, cone_deg=cone, verbose=False)
                o2 = sorted(r2.items(), key=lambda kv: -kv[1]["solid"])
                if not a.all_classes:
                    o2 = [(n, r) for n, r in o2 if classify(n, r["used_by"]) in SURFACE_CLASSES]
                s2 = set(n for n, _ in o2[:a.top])
                j = len(base & s2) / max(1, len(base | s2))
                print("  cone %4.1f deg  radius %5.0f m  -> %2d/%d of the top %d, Jaccard %.2f"
                      % (cone, rad, len(base & s2), len(base), a.top, j))
    return 0


if __name__ == "__main__":
    sys.exit(main())
