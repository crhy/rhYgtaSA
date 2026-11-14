#!/usr/bin/env python3
"""n20 - turn the game's own rasters into high-res PBR sets (albedo/normal/rough/height) for
RTX Remix, with no external texture pack.

usage:
  tools/pbrify.py --game <GAME_DIR> --list <rank.json> --out <dir>
                  [--scale 4] [--max-size 2048] [--min-size 256] [--only name,name]
                  [--report report.json] [--usda NAME] [--dry-run]

Why the albedo comes from the game and not from a photograph (this is the design decision of
the whole tool): SA's road textures carry the road *markings* - `crossing_law`,
`dt_road_stoplinea`, `crossing_law2/3` are the zebra crossings and stop lines, and the map's
UVs are authored for those rasters. Replacing such an albedo with a CC0 photograph of asphalt
erases the markings and breaks the art direction, and the pack we already run
(Hemry81/GTASA-Remix, MIT) shows what the photographic route costs: 1152 replacement maps, and
docs/TARGET.md still measures the surfaces as flat. So:

  albedo   = the game's own raster, decoded (DXT1/DXT3/DXT5/32-bit/16-bit), 4x upscaled
  height   = the relief already baked into that raster (see derive_height)
  normal   = the gradient of that height, scaled by the material class
  rough    = the material class's roughness, modulated by the local detail
  metal    = 1 for the metal class, 0 everywhere else (SA has no metal maps; the pack sets
             `inputs:metallic_texture = @@`, i.e. empty, for its own road materials)

Everything is deterministic: no neural upscaler, no randomness. RemasterBlaster
(https://github.com/morteza323/RemasterBlaster, MIT) documents why that matters for this game -
"Real-ESRGAN ... turns asphalt into wet plastic" - and it is the same reason the pack's own
albedos read as plastic under Remix's lighting.

The output is a Remix replacement set in the format the installed pack uses (measured census in
.strata/notes-n20-hd-pbr-textures.md): BC3/DXT5 albedo, BC5 normal, BC4 rough and height, DX10
header, full mip chain. tools/bcn.py holds the encoders (Pillow 13 and ImageMagick 7 both
refuse to encode BCn on this box - measured, see that file's docstring).

The USD layer this writes is the same shape tools/mirrors.py writes and docs/MIRRORS.md
documents, and the same shape the pack's own material layers use:

    over "RootNode" { over "Looks" { over "mat_<HASH>" { over "Shader" { ... } } } }
"""
from __future__ import annotations

import argparse
import glob
import json
import os
import re
import struct
import sys

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bcn                                    # noqa: E402
import remix_texhash as RT                    # noqa: E402
from texture_rank import CLASSES, classify    # noqa: E402

# --------------------------------------------------------------------- source decoding

def raster_header(data, tex):
    """(w, h, fourcc, depth, size, offset) from the raster header of one .txd entry.

    tools/remix_texhash.py finds the raster bytes (its hashes match all 32948 textures of this
    game) but looks for the fourcc in the 32 bytes *before* the raster header; in SA's own
    dictionaries the fourcc sits inside the header, at +4 of it (measured on lae2roads.txd:
    `00 02 00 00 "DXT1" 0001 0001 10 01 04 08 00800000` = type/flags, fourcc, w=256, h=256,
    depth=0x10, ..., size=32768). Read it from there as well.
    """
    off = tex.off
    size = tex.size
    w, h = tex.w, tex.h
    fourcc = tex.fourcc
    # the header is the 20 bytes before the raster: type(4) fourcc(4) w(2) h(2) depth(1) ... size(4)
    head = data[max(0, off - 20):off]
    if len(head) == 20:
        cand = head[4:8]
        if cand in (b"DXT1", b"DXT2", b"DXT3", b"DXT4", b"DXT5"):
            fourcc = cand
        w2, h2 = struct.unpack_from("<HH", head, 8)
        if w2 and h2 and abs(w2 - w) <= 1 and abs(h2 - h) <= 1:
            w, h = w2, h2
    return w, h, fourcc, tex.depth, size, off


def decode_rgba(data, tex):
    """RGBA uint8 (h, w, 4) for one .txd raster. Raises ValueError if we cannot decode it."""
    w, h, fourcc, depth, size, off = raster_header(data, tex)
    raw = data[off:off + size]
    if fourcc == b"DXT1":
        return bcn.decode_dxt1(raw, w, h)
    if fourcc in (b"DXT5", b"DXT2", b"DXT4"):
        return bcn.decode_bc3(raw, w, h)
    if fourcc == b"DXT3":
        # explicit 4-bit alpha: decode the colour block as BC1 and read the alpha nibbles
        rgb = bcn.unblocks(bcn._decode_color(raw, w, h, offset=8, stride=16), h, w, 3)
        n = (w // 4) * (h // 4)
        d = np.frombuffer(raw, np.uint8).reshape(-1, 16)[:n, 0:8]
        a = np.empty((n, 16), np.uint8)
        for i in range(16):
            byte, sh = i // 2, (i % 2) * 4
            v = (d[:, byte] >> sh) & 0x0F
            a[:, i] = (v * 17).astype(np.uint8)
        return np.dstack([rgb, bcn.unblocks(a, h, w, 1)[:, :, 0]])
    if fourcc:
        raise ValueError("%s: block format %r is not decoded here" % (tex.name, fourcc))
    bpp = size // max(1, w * h)
    if bpp == 4:
        px = np.frombuffer(raw, np.uint8).reshape(h, w, 4)
        return px[:, :, [2, 1, 0, 3]].copy()               # BGRA in memory -> RGBA
    if bpp == 2:
        a = np.frombuffer(raw, np.uint16).reshape(h, w)
        if depth == 16:
            # RenderWare A1R5G5B5; SA's own rasters are opaque, so bit 15 is the alpha flag
            r = ((a >> 10) & 31) * 255.0 / 31
            g = ((a >> 5) & 31) * 255.0 / 31
            b = (a & 31) * 255.0 / 31
            al = ((a >> 15) & 1) * 255.0
            if al.min() == al.max() == 0:
                al = np.full_like(al, 255.0)               # depth-16 with no alpha channel
            return np.dstack([r, g, b, al]).astype(np.uint8)
        raise ValueError("%s: %d bpp at depth %d is not decoded here" % (tex.name, bpp, depth))
    if bpp == 1:
        raise ValueError("%s: palettised raster (1 byte/px); its palette chunk is not read here"
                         % tex.name)
    raise ValueError("%s: cannot work out the raster format (%d bytes for %dx%d)"
                     % (tex.name, size, w, h))


# --------------------------------------------------------------------- map derivation

def upscale(rgba, target):
    """LANCZOS to a target edge length (both edges scale by the same factor, powers of two)."""
    h, w = rgba.shape[:2]
    f = target / max(w, h)
    if abs(f - 1.0) < 1e-6:
        return rgba
    nh, nw = max(4, int(round(h * f))), max(4, int(round(w * f)))
    im = Image.fromarray(rgba).resize((nw, nh), Image.LANCZOS)
    return np.asarray(im)


def _box_blur(plane, r):
    """Separable box blur of radius r via an integral image (fast, deterministic, no scipy)."""
    h, w = plane.shape
    k = 2 * r + 1
    pad = np.pad(plane, r + 1, mode="reflect")
    it = np.zeros((pad.shape[0] + 1, pad.shape[1] + 1), dtype=np.float64)
    it[1:, 1:] = pad.cumsum(0).cumsum(1)
    # original pixel (y, x) sits at pad (y+r+1, x+r+1); its box is rows y+1 .. y+2r+1 inclusive,
    # and with the leading zero row/col that sum is it[y+2r+2] - it[y+1]
    a = it[k + 1:k + 1 + h]
    b = it[1:h + 1]
    out = (a[:, k + 1:k + 1 + w] - b[:, k + 1:k + 1 + w] - a[:, 1:w + 1] + b[:, 1:w + 1])
    return out / float(k * k)


def derive_height(lum, cls):
    """A height field from the albedo's own luminance.

    SA's environment rasters are pre-lit: the creases of a pavement, the aggregate in asphalt
    and the mortar between bricks are already drawn into them as dark lines and bright specks.
    So the *high-frequency* part of the luminance is a relief map of the surface, and the
    low-frequency part is its large-scale shape. height = 0.5 + (low + hi * hi_gain) normalised,
    with the class's `strength` deciding how much of the high-frequency detail becomes relief.
    This is a heuristic and it is presented as one; it is not a photogrammetric height map.
    """
    r = max(2, lum.shape[0] // 64)
    lo = _box_blur(lum, r)
    hi = lum - lo
    hi = _box_blur(hi, 1)
    h = lo + float(cls["strength"]) * 1.5 * hi
    lo2, hi2 = float(h.min()), float(h.max())
    if hi2 - lo2 < 1e-6:
        return np.full_like(lum, 0.5)
    return (h - lo2) / (hi2 - lo2)


def derive_normal(height, strength):
    """Tangent-space normal from the height field (Sobel), BC5-ready: R = x, G = y, 128 flat.

    The gradient is recentred (so a one-sided slope in the height field does not tilt the whole
    map) and normalised by its own 95th percentile, then scaled by an amplitude of
    0.25 * strength: asphalt (strength 1.6) ends up with a normal-map standard deviation of
    about 20/255, glass (0.2) with about 2/255. Normalising by a percentile instead of the
    maximum keeps a single stray spike from flattening the rest of the map, which is what the
    pack's own BC5 maps look like too (measured: std of the pack's asphalt normal is 21).
    """
    kx = np.array([[-1, 0, 1], [-2, 0, 2], [-1, 0, 1]], float) / 4.0
    ky = np.array([[-1, -2, -1], [0, 0, 0], [1, 2, 1]], float) / 4.0
    gx = _convolve(height, kx)
    gy = _convolve(height, ky)
    gx = gx - np.median(gx)
    gy = gy - np.median(gy)
    for g in (gx, gy):
        p = np.percentile(np.abs(g), 95)
        if p > 1e-9:
            g /= p
    amp = 0.25 * float(strength)
    r = np.clip(128 + amp * gx * 127, 0, 255).astype(np.uint8)
    g = np.clip(128 - amp * gy * 127, 0, 255).astype(np.uint8)
    return np.dstack([r, g])


def _convolve(plane, k):
    """3x3 convolution with edge replication (numpy only, no scipy on this box)."""
    p = np.pad(plane, 1, mode="edge")
    out = np.zeros_like(plane)
    for dy in range(3):
        for dx in range(3):
            if k[dy, dx]:
                out += k[dy, dx] * p[dy:dy + plane.shape[0], dx:dx + plane.shape[1]]
    return out


def derive_rough(lum, height, cls):
    """Roughness: the class's base value, modulated by how much local detail there is.

    Physically the right direction: the same material is glossier where it is smooth and worn
    and rougher where it is detailed (asphalt aggregate, mortar lines). `rough_var` is the
    half-width of that modulation, so the result stays inside the class's measured range.
    """
    detail = np.abs(height - 0.5)
    detail = _box_blur(detail, max(1, height.shape[0] // 128))
    lo, hi = float(detail.min()), float(detail.max())
    t = (detail - lo) / (hi - lo) if hi - lo > 1e-6 else np.full_like(detail, 0.5)
    v = cls["rough"] + cls["rough_var"] * (2 * t - 1)
    return np.clip(v, 0.02, 1.0)


# --------------------------------------------------------------------- the set writer

def next_pow2(n):
    p = 1
    while p < n:
        p *= 2
    return p


def target_size(w, h, scale, max_size, min_size):
    return int(max(min_size, min(max_size, next_pow2(scale * max(w, h)))))


def src_mip_bytes(tex):
    """What the game's own raster costs in VRAM, mip chain included - the number the
    replacement has to be compared against (the task asks for a VRAM *delta*, not a total).

    tex.size is the mip-0 payload: measured on lae2roads.txd, `sidewgrass1` is 256x256 DXT1
    with size 32768 = (256/4)^2 blocks x 8 bytes. So the per-block size falls out of it, and
    the mip chain is the usual x4/3 (SA's rasters carry one: the runtime's own complaint about
    mip-less replacement maps is in docs/TEXTURES.md). One albedo against our four maps.
    """
    nb = max(1, ((tex.w + 3) // 4) * ((tex.h + 3) // 4))
    per_block = max(1.0, float(round(tex.size / nb)))
    return int(bcn.mip_bytes(tex.w, tex.h, per_block))


def build_set(rgba, cls, scale, max_size, min_size):
    """(albedo_rgba, normal_rg, rough_u8, height_u8) at the target size."""
    big = upscale(rgba, target_size(rgba.shape[1], rgba.shape[0], scale, max_size, min_size))
    lum = (0.2126 * big[:, :, 0] + 0.7152 * big[:, :, 1] + 0.0722 * big[:, :, 2])
    height = derive_height(lum, cls)
    normal = derive_normal(height, float(cls["strength"]))
    rough = (255 * derive_rough(lum, height, cls)).astype(np.uint8)
    hmap = (255 * height).astype(np.uint8)
    return big, normal, rough, hmap


def write_set(outdir, hash_hex, albedo, normal, rough, hmap):
    """Four DDS files, each with a full mip chain. Returns {suffix: (bytes, w, h)}."""
    w, h = albedo.shape[:2]
    mips = len(bcn.mip_sizes(w, h))
    out = {}
    jobs = [("albedo", bcn.encode_bc3(albedo, mips=True), bcn.DXGI["BC3_UNORM"], b"DXT5"),
            ("normal", bcn.encode_bc5(normal, mips=True), bcn.DXGI["BC5_UNORM"], None),
            ("rough", bcn.encode_bc4(rough, mips=True), bcn.DXGI["BC4_UNORM"], None),
            ("height", bcn.encode_bc4(hmap, mips=True), bcn.DXGI["BC4_UNORM"], None)]
    for suffix, blob, dxgi, legacy in jobs:
        path = os.path.join(outdir, "%s_%s.dds" % (hash_hex, suffix))
        bcn.write_dds(path, blob, w, h, dxgi, mips=mips, fourcc_legacy=legacy)
        out[suffix] = (os.path.getsize(path), w, h)
    return out


# --------------------------------------------------------------------- the USD layer

BODY = '''#usda 1.0
# {layer} - generated by REPO/tools/pbrify.py from the game's own rasters. Do not edit by hand:
# change REPO/config/texture-pbr.json (the measured top-{count} surfaces, tools/texture_rank.py)
# and re-run. Every map is derived from the texture it replaces (4x upscale of the game's raster
# + height/normal/roughness from it), so the road markings and the art direction survive; the
# formats are the ones the installed pack already uses (BC3 albedo, BC5 normal, BC4 rough and
# height, full mip chains - docs/TEXTURES.md).
(
    customLayerData = {{
        dictionary omni_layer = {{
            dictionary locked = {{
            }}
            dictionary muteness = {{
            }}
        }}
    }}
    upAxis = "Z"
)

over "RootNode"
{{
    over "Looks"
    {{
{blocks}    }}
}}
'''

BLOCK = '''        over "mat_{hash}"  # {name} ({cls}, {w}x{h} -> {size})
        {{
            over "Shader"
            {{
                uniform asset info:mdl:sourceAsset = @./materials/AperturePBR_{shader}.mdl@
                uniform token info:mdl:sourceAsset:subIdentifier = "AperturePBR_{shader}"
                asset inputs:diffuse_texture = @./{dir}/{hash}_albedo.dds@
                asset inputs:normalmap_texture = @./{dir}/{hash}_normal.dds@
                asset inputs:reflectionroughness_texture = @./{dir}/{hash}_rough.dds@
                asset inputs:height_texture = @./{dir}/{hash}_height.dds@
                float inputs:displace_in = {displace}
                float inputs:reflection_roughness_constant = {rough}
                float inputs:metallic_constant = {metal}
                asset inputs:metallic_texture = @@
            }}
        }}

'''


def render_usda(entries, texdir, layer="rhy_textures.usda"):
    blocks = []
    for e in entries:
        cls = CLASSES[e["class"]]
        shader = "Translucent" if cls.get("shader") == "translucent" else "Opaque"
        blocks.append(BLOCK.format(hash=e["hash"], name=e["name"], cls=e["class"],
                                   w=e["w"], h=e["h"], size=e["size"], dir=texdir,
                                   shader=shader, rough=round(cls["rough"], 3),
                                   metal=cls["metal"], displace=0.002 if cls["strength"] >= 1.2 else 0.0))
    return BODY.format(layer=layer, blocks="".join(blocks), count=len(entries))


# --------------------------------------------------------------------- main

def open_txd(game, label):
    """The bytes of one texture dictionary from a rank.json `txd` label ("models/x.txd" or
    "gta3.img:NAME.txd"), or None."""
    import lamp_list as LL
    if not label:
        return None
    if ":" in label:                                   # inside an .img archive
        img, txd = label.split(":", 1)
        path = os.path.join(game, "models", os.path.basename(img))
        if not os.path.exists(path):
            return None
        for e in LL.read_img_index(path)[1]:
            if e[1].lower() == txd.lower():
                with open(path, "rb") as fh:
                    return LL.read_entry(fh, e[2], e[3], e[4])
        return None
    path = os.path.join(game, label)
    if os.path.isfile(path):
        with open(path, "rb") as fh:
            return fh.read()
    return None


def find_texture(game, name, label=None):
    """(txd_label, bytes, Tex) for one texture name: the dictionary rank.json named first, then
    every loose .txd whose file name is the texture name, then a full scan."""
    import lamp_list as LL
    want = name.lower()
    order = [label] if label else []
    for root, _dirs, files in os.walk(os.path.join(game, "models")):
        for fn in files:
            if fn.lower().endswith(".txd") and fn.lower()[:-4] == want:
                order.append(os.path.relpath(os.path.join(root, fn), game))
    imgs = [os.path.join(game, "models", x) for x in ("gta3.img", "gta_int.img", "player.img")]
    for img in imgs:
        if not os.path.exists(img):
            continue
        for e in LL.read_img_index(img)[1]:
            if e[1].lower().endswith(".txd") and e[1].lower()[:-4] == want:
                order.append("%s:%s" % (os.path.basename(img), e[1]))
    for root, _dirs, files in os.walk(os.path.join(game, "models")):
        for fn in files:
            if fn.lower().endswith(".txd"):
                order.append(os.path.relpath(os.path.join(root, fn), game))
    seen = set()
    for lab in order:
        if not lab or lab in seen:
            continue
        seen.add(lab)
        blob = open_txd(game, lab)
        if blob is None:
            continue
        try:
            for t in RT.parse_txd(blob, lab):
                if t.name.lower() == want:
                    return lab, blob, t
        except ValueError:
            continue
    return None, None, None


def pack_coverage(game, hash_hex):
    """{suffix: (path, w, h, mips)} for the replacement maps the installed Remix pack already
    ships for this Remix hash, from every folder under <game>/rtx-remix/mods/*/texture/.

    This is the VRAM/regression guard of the whole task (n20b): 5 of the 40 measured surfaces
    are already covered by the pack at 2048^2 or 4096^2 with photographic albedos, and our
    derived 1024^2 maps would be a **downgrade** on top of them - our layer is installed last
    in mod.usda's subLayers, so last one wins. Default policy: do not generate those; name them
    in the report and let --keep-covered override it for an A/B.
    """
    out = {}
    root = os.path.join(game, "rtx-remix", "mods")
    if not os.path.isdir(root):
        return out
    for texdir in sorted(glob.glob(os.path.join(root, "*", "texture"))):
        for p in glob.glob(os.path.join(texdir, "**", "%s_*.dds" % hash_hex), recursive=True):
            suf = os.path.basename(p).rsplit("_", 1)[-1].split(".")[0]
            try:
                hdr, _lv = bcn.read_dds(p)
            except Exception:
                continue
            prev = out.get(suf)
            if prev is None or hdr["w"] * hdr["h"] > prev[1] * prev[2]:
                out[suf] = (p, hdr["w"], hdr["h"], hdr["mips"])
    return out


def pack_calibration(game, sample=12):
    """{category: {rough, rough_std, normal_std, normal_mean, albedo_mean}} measured from the
    installed Remix pack. This is where the numbers in texture_rank.CLASSES come from."""
    import collections
    root = os.path.join(game, "rtx-remix", "mods", "gameReadyAssets", "texture")
    out = collections.defaultdict(lambda: collections.defaultdict(list))
    if not os.path.isdir(root):
        return out
    for cat in sorted(os.listdir(root)):
        d = os.path.join(root, cat)
        if not os.path.isdir(d):
            continue
        for suf in ("rough", "normal", "albedo"):
            for f in sorted(glob.glob(os.path.join(d, "*_%s.dds" % suf)))[:sample]:
                try:
                    im = Image.open(f)
                    im.load()
                except Exception:
                    continue
                a = np.asarray(im)
                if a.ndim == 2:
                    a = a[:, :, None]
                flat = a.reshape(-1, a.shape[-1]).astype(np.float64)
                if suf == "rough":
                    out[cat]["rough"].append(flat[:, 0].mean() / 255.0)
                    out[cat]["rough_std"].append(flat[:, 0].std() / 255.0)
                elif suf == "normal":
                    out[cat]["normal_std"].append(flat[:, :2].std())
                    out[cat]["normal_mean"].append(flat[:, :2].mean())
                else:
                    out[cat]["albedo"].append(flat[:, :3].mean())
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0], add_help=True)
    ap.add_argument("--calibrate", action="store_true",
                    help="print the installed pack's roughness/normal statistics per category "
                         "(the calibration the material classes use) and exit")
    ap.add_argument("--game", required=True)
    ap.add_argument("--list", help="rank.json from tools/texture_rank.py")
    ap.add_argument("--out", help="directory for the DDS files and the layer")
    ap.add_argument("--scale", type=int, default=4)
    ap.add_argument("--max-size", type=int, default=2048)
    ap.add_argument("--min-size", type=int, default=256)
    ap.add_argument("--only", default=None, help="comma list of texture names")
    ap.add_argument("--report", default=None)
    ap.add_argument("--usda", default="rhy_textures.usda")
    ap.add_argument("--texdir", default="texture/rhy_pbr")
    ap.add_argument("--keep-covered", action="store_true",
                    help="generate a set even where the installed pack already ships maps at "
                         "the same or a higher resolution (default: skip them, see pack_coverage)")
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args(argv)

    if a.calibrate:
        cal = pack_calibration(a.game)
        print("%-14s %5s %7s %8s %8s %8s %8s"
              % ("category", "n", "rough", "rstd", "nstd", "nmean", "albedo"))
        for cat in sorted(cal):
            o = cal[cat]
            med = lambda k: float(np.median(o[k])) if o.get(k) else float("nan")
            print("%-14s %5d %7.3f %8.3f %8.1f %8.1f %8.1f"
                  % (cat, len(o.get("rough", [])), med("rough"), med("rough_std"),
                     med("normal_std"), med("normal_mean"), med("albedo")))
        return 0

    if not a.list:
        raise SystemExit("--list is required unless --calibrate is given")
    with open(a.list) as fh:
        data = json.load(fh)
    entries = data["textures"]
    for e in entries:
        # rank.json wrote the class as "class_" before n20b (`class=` is a Python keyword, so the
        # writer had to spell it); accept either spelling so an old rank file still feeds this tool.
        if "class" not in e:
            e["class"] = e.get("class_") or "other"
    if a.only:
        keep = set(x.strip().lower() for x in a.only.split(","))
        entries = [e for e in entries if e["name"].lower() in keep]

    texout = os.path.join(a.out, a.texdir)
    if not a.dry_run:
        os.makedirs(texout, exist_ok=True)
    plan, report, skipped = [], [], []
    for e in entries:
        if not e.get("hash"):
            skipped.append((e["name"], "no remix hash (texture not found in the game data)"))
            continue
        cls = CLASSES.get(e["class"], CLASSES["other"])
        hh = e["hash"].replace("0x", "").upper()
        tgt = target_size(e["w"], e["h"], a.scale, a.max_size, a.min_size)
        cov = pack_coverage(a.game, hh)
        if not a.keep_covered and all(k in cov for k in ("albedo", "normal", "rough")) \
                and cov["albedo"][1] >= tgt and cov["albedo"][2] >= tgt:
            skipped.append((e["name"],
                            "the installed pack already covers it: %s %dx%d (mips %d); ours would "
                            "be %dx%d - keep with --keep-covered"
                            % (os.path.relpath(cov["albedo"][0], os.path.join(a.game, "rtx-remix")),
                               cov["albedo"][1], cov["albedo"][2], cov["albedo"][3], tgt, tgt)))
            continue
        plan.append(e)
        if a.dry_run:
            print("%-24s %-9s %-10s -> %-6s %s" % (e["name"], e["class"],
                                                   "%dx%d" % (e["w"], e["h"]),
                                                   tgt, e["hash"]))
            continue
        label, blob, tex = find_texture(a.game, e["name"], e.get("txd"))
        if tex is None:
            skipped.append((e["name"], "not found in any .txd"))
            continue
        try:
            rgba = decode_rgba(blob, tex)
        except ValueError as ex:
            skipped.append((e["name"], str(ex)))
            continue
        albedo, normal, rough, hmap = build_set(rgba, cls, a.scale, a.max_size, a.min_size)
        sizes = write_set(texout, hh, albedo, normal, rough, hmap)
        total = sum(v[0] for v in sizes.values())
        report.append(dict(name=e["name"], hash=hh, remix_hash=e["hash"], cls=e["class"],
                           src="%dx%d" % (tex.w, tex.h), out="%dx%d" % sizes["albedo"][1:3],
                           src_fmt=(tex.fourcc or b"uncompressed").decode("ascii", "replace"),
                           txd=label, pack={k: "%dx%d/m%d" % v[1:4] for k, v in cov.items()},
                           files={k: v[0] for k, v in sizes.items()}, bytes=total,
                           src_vram=src_mip_bytes(tex),
                           vram=bcn.mip_bytes(sizes["albedo"][1], sizes["albedo"][2], 16)
                                + bcn.mip_bytes(sizes["albedo"][1], sizes["albedo"][2], 16)
                                + 2 * bcn.mip_bytes(sizes["albedo"][1], sizes["albedo"][2], 8)))
        e["size"] = sizes["albedo"][1]
        print("%-24s %-9s %-10s -> %-7s %s  %6.1f MB"
              % (e["name"], e["class"], "%dx%d" % (tex.w, tex.h),
                 "%dx%d" % sizes["albedo"][1:3], e["hash"], total / 1e6))

    if not a.dry_run:
        bad = set(x[0] for x in skipped)
        usable = [dict(name=e["name"], hash=e["hash"].replace("0x", "").upper(),
                       **{"class": e["class"], "w": e["w"], "h": e["h"],
                          "size": target_size(e["w"], e["h"], a.scale, a.max_size, a.min_size)})
                  for e in plan if e["name"] not in bad]
        with open(os.path.join(a.out, a.usda), "w") as fh:
            fh.write(render_usda(usable, a.texdir, a.usda))
        print("wrote %s (%d material override(s))" % (os.path.join(a.out, a.usda), len(usable)))
    if skipped:
        print("skipped:")
        for n, why in skipped:
            print("  %-24s %s" % (n, why))
    if report:
        tot = sum(r["bytes"] for r in report)
        vram = sum(r["vram"] for r in report)
        src = sum(r["src_vram"] for r in report)
        print("\n%d sets, %.1f MB on disk, %.1f MB of texture memory with mip chains"
              % (len(report), tot / 1e6, vram / 1e6))
        print("   the game's own rasters for these surfaces: %.1f MB -> %.1f MB "
              "(%+.1f MB, %.1fx)"
              % (src / 1e6, vram / 1e6, (vram - src) / 1e6, vram / max(1, src)))
        if a.report:
            with open(a.report, "w") as fh:
                json.dump(dict(sets=report, disk_bytes=tot, vram_bytes=vram), fh, indent=1)
            print("wrote %s" % a.report)
    return 0


if __name__ == "__main__":
    sys.exit(main())
