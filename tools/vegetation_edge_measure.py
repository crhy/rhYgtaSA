#!/usr/bin/env python3
"""n21: do the leaves have hard edges or soft ones — measured on the tour shots and on the textures.

    usage: vegetation_edge_measure.py IMAGE [IMAGE ...] [--name PAT] [--json OUT]
                                        [--band LO HI] [--radius N] [--hue LO HI]
            vegetation_edge_measure.py --txd PATH [PATH ...] [--pattern RE]

Two questions, two modes, one number each:

  Frames (the tour's vegetation stop, e.g. stop `14-vegetation-vinewood-noon`).
  A screenshot has no alpha channel, so "mid-alpha" is measured where it is still visible:
  at a leaf edge the pixel is a mix of leaf and background. Take the leaf colour (median of
  foliage pixels whose whole neighbourhood is foliage) and the background colour (median of the
  non-foliage pixels right outside that region), project every pixel of the edge zone onto
  that axis, and count the ones that land strictly between the two ends. A hard alpha cutout
  puts almost nothing in between (one pixel jumps from leaf to background); an alpha *blend*
  puts a wide band of half-covered pixels there — which is exactly the "scribbled grey stroke"
  rhY described (docs/VEGETATION.md, s32). So this number should FALL when
  `rtx.hairCardTextures` takes effect, and `edge_width_px` with it.
    mid_alpha_pct   % of edge-zone pixels whose leaf<->background projection is in --band
    ramp_width_px   mean run length of those pixels along a scanline (1.0 = a hard cut)
    foliage_pct     % of the frame the green mask claims (so a shot that changed composition
                    is visible instead of quietly moving every other number)

  Textures (`--txd`): the same quantity at its source — the mip-0 alpha of the foliage
  dictionaries, decoded by tools/vegetation_hashes.py. This is the fraction of texels that an
  alpha test at any threshold has to decide about, i.e. how much of the card is soft gradient.

The green mask is HSV hue in --hue (default 60..175 deg) with sat>=0.12 and val>=0.08, gated
so grey tarmac is never "foliage" and a saturated green car is only counted where it is next
to real foliage (the edge zone is what is measured, not the mask interior).
"""
import argparse
import json
import os
import re
import sys

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import remix_texhash as R          # noqa: E402
import vegetation_hashes as V      # noqa: E402  (alpha_of / stats / fourcc_of)

SAT_MIN = 0.12
VAL_MIN = 0.08


def rgb_to_hsv(a):
    """a: float array (H,W,3) in 0..1 -> (h deg 0..360, s 0..1, v 0..1)."""
    mx = a.max(axis=2)
    mn = a.min(axis=2)
    d = mx - mn
    h = np.zeros_like(mx)
    nz = d > 0
    r, g, b = a[..., 0], a[..., 1], a[..., 2]
    idx = nz & (mx == r)
    h[idx] = (((g - b) / np.where(d == 0, 1, d)) % 6)[idx]
    idx = nz & (mx == g) & (h == 0)
    h[idx] = (((b - r) / np.where(d == 0, 1, d)) + 2)[idx]
    idx = nz & (mx == b) & (h == 0)
    h[idx] = (((r - g) / np.where(d == 0, 1, d)) + 4)[idx]
    h = np.where(h < 0, h + 6, h) * 60.0
    s = np.where(mx == 0, 0, d / np.where(mx == 0, 1, mx))
    return h, s, mx


def shift(mask, dy, dx):
    """Shift a boolean mask, filling with False (no scipy dependency)."""
    out = np.zeros_like(mask)
    h, w = mask.shape
    ys, xs = slice(max(0, dy), h + min(0, dy)), slice(max(0, dx), w + min(0, dx))
    yd, xd = slice(max(0, -dy), h + min(0, -dy)), slice(max(0, -dx), w + min(0, -dx))
    out[ys, xs] = mask[yd, xd]
    return out


def grow(mask, radius):
    out = mask.copy()
    for _ in range(max(0, radius)):
        out = (out | shift(out, 1, 0) | shift(out, -1, 0) | shift(out, 0, 1) | shift(out, 0, -1))
    return out


def measure_frame(path, hue=(60.0, 175.0), radius=3, band=(0.2, 0.8)):
    img = np.asarray(Image.open(path).convert("RGB"), dtype=np.float64) / 255.0
    h, s, v = rgb_to_hsv(img)
    foliage = (h >= hue[0]) & (h <= hue[1]) & (s >= SAT_MIN) & (v >= VAL_MIN)
    if foliage.sum() < 64:
        return {"image": path, "foliage_pct": 100.0 * foliage.mean(),
                "mid_alpha_pct": None, "edge_width_px": None,
                "note": "green mask found <64 pixels - no foliage in this frame"}
    inner = foliage & ~grow(~foliage, 1)                 # foliage with no non-foliage neighbour
    ring = grow(foliage, radius) & ~foliage              # background hugging the foliage
    leaf = np.median(img[inner], axis=0) if inner.sum() else np.median(img[foliage], axis=0)
    bg = np.median(img[ring], axis=0) if ring.sum() else np.median(img[~foliage], axis=0)
    axis = leaf - bg
    denom = float((axis * axis).sum())
    # the edge zone: within `radius` of the foliage boundary, on either side of it
    edge = (grow(foliage, radius) & ~foliage) | (foliage & grow(~foliage, radius))
    if denom <= 0 or edge.sum() < 16:
        return {"image": path, "foliage_pct": 100.0 * foliage.mean(),
                "mid_alpha_pct": None, "edge_width_px": None,
                "note": "leaf/background colours coincide or the edge zone is tiny"}
    t = ((img - bg) @ axis) / denom
    t = np.clip(t, 0.0, 1.0)
    te = t[edge]
    mid = float(((te >= band[0]) & (te <= band[1])).mean())
    # the soft ramp itself: edge pixels that are neither leaf nor background
    ramp = np.zeros_like(edge)
    ramp[edge] = (te >= band[0]) & (te <= band[1])
    # edge width: mean run length of ramp pixels along horizontal scanlines
    runs = []
    for r in range(ramp.shape[0]):
        line = ramp[r]
        cur = 0
        for val in line:
            if val:
                cur += 1
            elif cur:
                runs.append(cur)
                cur = 0
        if cur:
            runs.append(cur)
    return {"image": path, "shape": list(img.shape[:2]),
            "foliage_pct": round(100.0 * float(foliage.mean()), 3),
            "edge_px": int(edge.sum()), "ramp_px": int(ramp.sum()),
            "leaf_rgb": [round(float(x) * 255, 1) for x in leaf],
            "bg_rgb": [round(float(x) * 255, 1) for x in bg],
            "mid_alpha_pct": round(100.0 * mid, 3),
            "edge_width_px": round(float(np.mean(runs)), 3) if runs else None,
            "band": list(band), "radius": radius, "hue": list(hue)}


def measure_txd(path, pattern=None):
    """Per-texture alpha census of .txd file(s) -> aggregate mid-alpha fraction."""
    rx = re.compile(pattern, re.I) if pattern else None
    rows, tot = [], np.zeros(3)
    for label, blob in R.iter_txd_bytes(path):
        base = os.path.basename(label.split(":")[-1]).rsplit(".", 1)[0]
        if rx and not rx.match(base):
            continue
        try:
            texs = R.parse_txd(blob, label)
        except ValueError:
            continue                       # not a texture dictionary after all
        for t in texs:
            raw = t.d3d9_mip0(blob)
            al = V.alpha_of(t, raw, V.fourcc_of(t, blob))
            if al is None:
                continue
            z, m, o = V.stats(al)
            rows.append((base, t.name, t.w, t.h, 100 * z, 100 * m, 100 * o))
            n = float(al.size)
            tot += np.array([z * n, m * n, o * n])
    if not rows:
        return {"path": path, "textures": 0, "note": "no texture with a decodable alpha"}
    n = tot.sum()
    return {"path": path, "textures": len(rows),
            "texels": int(n),
            "alpha0_pct": round(100 * tot[0] / n, 3),
            "mid_alpha_pct": round(100 * tot[1] / n, 3),
            "opaque_pct": round(100 * tot[2] / n, 3),
            "worst": sorted(rows, key=lambda r: -r[5])[:10]}


def main(argv=None):
    ap = argparse.ArgumentParser(prog="vegetation_edge_measure.py", description=__doc__.splitlines()[0])
    ap.add_argument("paths", nargs="*")
    ap.add_argument("--txd", action="store_true", help="measure .txd alpha instead of frames")
    ap.add_argument("--pattern", default=None, help="with --txd: only dictionaries matching this regex")
    ap.add_argument("--name", default=None, help="only frames whose file name matches this regex")
    ap.add_argument("--band", default="0.2,0.8", help="the 'in between' window of the leaf<->bg axis")
    ap.add_argument("--radius", type=int, default=3, help="edge-zone half-width in pixels (default 3)")
    ap.add_argument("--hue", default="60,175", help="green mask hue range in degrees")
    ap.add_argument("--json", default=None)
    a = ap.parse_args(argv)
    if not a.paths:
        ap.error("give at least one IMAGE, or --txd PATH")
    lo, hi = [float(x) for x in a.band.split(",")]
    hue = tuple(float(x) for x in a.hue.split(","))

    out = []
    if a.txd:
        for p in a.paths:
            r = measure_txd(p, a.pattern)
            out.append(r)
            print("%s: %d texture(s), alpha0 %.1f%%  mid %.1f%%  opaque %.1f%%"
                  % (p, r.get("textures", 0), r.get("alpha0_pct", 0),
                     r.get("mid_alpha_pct", 0), r.get("opaque_pct", 0)))
            for w in r.get("worst", [])[:5]:
                print("    %-18s %-18s %4dx%-4d  a=0 %5.1f%%  mid %5.1f%%  opaque %5.1f%%"
                      % (w[0], w[1], w[2], w[3], w[4], w[5], w[6]))
    else:
        rx = re.compile(a.name) if a.name else None
        for p in a.paths:
            if rx and not rx.search(os.path.basename(p)):
                continue
            r = measure_frame(p, hue=hue, radius=a.radius, band=(lo, hi))
            out.append(r)
            if r.get("mid_alpha_pct") is None:
                print("%s: %s" % (p, r.get("note")))
            else:
                print("%s: foliage %.2f%%  mid_alpha %.2f%%  ramp_width %s px  (%d edge px, leaf %s vs bg %s)"
                      % (os.path.basename(p), r["foliage_pct"], r["mid_alpha_pct"],
                         ("-" if r["edge_width_px"] is None else "%.2f" % r["edge_width_px"]),
                         r["edge_px"], r["leaf_rgb"], r["bg_rgb"]))
    if a.json:
        with open(a.json, "w") as f:
            json.dump(out, f, indent=1)
    return 0


if __name__ == "__main__":
    sys.exit(main())
