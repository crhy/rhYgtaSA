#!/usr/bin/env python3
"""Measure the car-light / particle / window symptoms in the 2026-11-12c video frames.

    python3 tools/frame_light_measure.py build/tmp/video-2026-11-12 --frames 4-40

Three numbers per frame, all of them the ones .strata/test-2026-11-12c.md complains about:

  red_spill   the red wash the tail lights throw on the sand (V5). A tail bulb is a few
              square centimetres of red glass; what matters on screen is how much ground it
              colours and how far that colour reaches, so the metric is the AREA of
              red-dominant ground and the DISTANCE at which it dies out, not the peak.
  head_pool   the bright pool the headlights make on the ground (V5). Same idea: area,
              equivalent radius, peak.
  smoke       the exhaust puffs (V6). A puff of soot at night is a dim grey scatterer; a
              pink-white puff that is brighter than the ground under a street lamp is a
              self-lit emissive sheet. So the metric is the puff's mean RGB, its
              pink-ness (min(R,B) - G) and its luminance against the local background.
  windows     the flat white blocks (V7). Every channel at or above CLIP_MIN is a pixel the
              tonemapper has clipped: no texture, no colour, a block. The metrics are the
              clipped area, the size of the biggest connected block (a window is a few
              hundred pixels wide at this distance; a whole facade is not) and the chroma
              of the clipped pixels - a real lit window carries its lamp's colour, an
              overbright emissive has none.

Everything is measured on a HIGH-PASS image (image minus a wide Gaussian blur of itself),
because the whole frame carries a flat ambient floor (V1) and an absolute threshold would
measure that floor instead of the light. The blur radius is the "how wide is a light
effect" scale: anything narrower than it is a light, anything wider is the ambient.

Output: one line per frame, a summary block, and with --json a machine-readable copy.
"""
import argparse
import glob
import json
import os
import re
import sys

import numpy as np
from PIL import Image

# --- tuning constants, all of them in 8-bit sRGB units of the frame -----------------
BLUR_RADIUS = 40      # wider than this is ambient, narrower is a light
BLOB_MIN = 12.0       # high-pass value that counts as "this pixel is lit by something"
CORE_MIN = 60.0       # high-pass value that counts as "this pixel is the source itself"
GROUND_ROW = 0.60     # the ground band starts here (fraction of the frame height)
PINK_MIN = 15.0       # min(R,B) - G that makes a bright pixel read as pink
BRIGHT_MIN = 110      # 8-bit value above which a pixel can be a glowing puff
CLIP_MIN = 235        # every channel at/above this: the tonemapper has clipped to white


def load(path):
    return np.asarray(Image.open(path).convert("RGB")).astype(np.float64)


def box_blur(channel, radius):
    """Separable box blur, pure numpy, works on signed float channels (no 8-bit clipping).

    Two passes approximate a Gaussian well enough for "what is wider than a light" and keep
    the tool dependency-free and exactly reproducible in the test.

    The border handling matters and is asserted in tests/test_frame_light_measure.py: the
    window is clipped at the edge and the divisor is the number of samples actually taken.
    Dividing by the full window width instead (the obvious cumsum shortcut) darkens the
    border of a flat field by up to half, and the high-pass then reports a bright band along
    every edge of the frame - which, measured at radius 40 on a 1080p frame, is bigger than
    the light effects being measured.
    """
    out = np.asarray(channel, dtype=np.float64)
    for axis in (0, 1):
        n = out.shape[axis]
        c = np.cumsum(out, axis=axis, dtype=np.float64)
        i = np.arange(n)
        hi = np.minimum(i + radius, n - 1)
        lo = np.maximum(i - radius, 0)
        hi_v = np.take(c, hi, axis=axis)
        lo_m1 = np.maximum(lo - 1, 0)
        lo_v = np.take(c, lo_m1, axis=axis)
        shape = [1] * out.ndim
        shape[axis] = n
        # c[lo-1] is the sum strictly before the window; for lo == 0 there is
        # nothing before it, and lo_m1 clamps to c[0] - a[0], which the mask below
        # then discards.
        diff = np.where((lo > 0).reshape(shape), hi_v - lo_v, hi_v)
        cnt = (hi - lo + 1).reshape(shape)
        out = diff / cnt
    return out


def highpass(channel, radius=BLUR_RADIUS):
    """channel minus a wide blur of itself: light effects survive, the ambient floor does not."""
    return channel - box_blur(channel, radius)


def ground_mask(shape, row=GROUND_ROW):
    h = shape[0]
    m = np.zeros(shape[:2], dtype=bool)
    m[int(row * h):, :] = True
    return m


def blob_stats(hi, mask, rgb=None):
    """Area / centroid / equivalent radius / peak of the pixels of `hi` selected by `mask`."""
    sel = mask & (hi > BLOB_MIN)
    n = int(sel.sum())
    out = {"area_px": n, "peak_hi": float(hi[sel].max()) if n else 0.0,
           "core_px": int((mask & (hi > CORE_MIN)).sum())}
    if n:
        ys, xs = np.nonzero(sel)
        out["centre"] = [int(xs.mean()), int(ys.mean())]
        out["equiv_radius_px"] = round(float(np.sqrt(n / np.pi)), 1)
        out["extent_px"] = [int(xs.max() - xs.min() + 1), int(ys.max() - ys.min() + 1)]
        k = int(np.argmax(hi[ys, xs]))
        out["peak_px"] = [int(xs[k]), int(ys[k])]
        out["peak_rgb"] = [int(v) for v in rgb[ys[k], xs[k]]] if rgb is not None else [0, 0, 0]
    else:
        out["centre"] = [0, 0]
        out["equiv_radius_px"] = 0.0
        out["extent_px"] = [0, 0]
        out["peak_px"] = [0, 0]
        out["peak_rgb"] = [0, 0, 0]
    return out


def radial_profile(hi, centre, mask, radii=(10, 20, 40, 80, 160, 320)):
    """Mean high-pass value in rings around `centre`: the falloff of the effect on screen."""
    h, w = hi.shape[:2]
    ys, xs = np.mgrid[0:h, 0:w]
    d = np.sqrt((xs - centre[0]) ** 2 + (ys - centre[1]) ** 2)
    prof = []
    lo = 0.0
    for r in radii:
        ring = mask & (d > lo) & (d <= r)
        prof.append({"r": r, "mean_hi": round(float(hi[ring].mean()), 2) if ring.sum() else 0.0,
                     "peak_hi": round(float(hi[ring].max()), 2) if ring.sum() else 0.0,
                     "px": int(ring.sum())})
        lo = r
    return prof


def largest_component(mask):
    """Size, bounding box and aspect of the biggest 4-connected blob in `mask`.

    A pure-python flood fill over the selected pixels: the masks this tool feeds it are
    fractions of a percent of the frame, so it is microseconds, and it keeps the tool
    free of a scipy dependency a reviewer would have to install to reproduce a number.
    """
    ys, xs = np.nonzero(mask)
    if not len(ys):
        return {"area_px": 0, "extent_px": [0, 0], "centre": [0, 0], "aspect": 0.0}
    todo = set(zip(ys.tolist(), xs.tolist()))
    best = (0, None)
    while todo:
        seed = todo.pop()
        stack = [seed]
        blob = [seed]
        while stack:
            y, x = stack.pop()
            for ny, nx in ((y - 1, x), (y + 1, x), (y, x - 1), (y, x + 1)):
                if (ny, nx) in todo:
                    todo.discard((ny, nx))
                    stack.append((ny, nx))
                    blob.append((ny, nx))
        if len(blob) > best[0]:
            best = (len(blob), blob)
    n, blob = best
    by = [p[0] for p in blob]
    bx = [p[1] for p in blob]
    w = max(bx) - min(bx) + 1
    h = max(by) - min(by) + 1
    return {"area_px": n, "extent_px": [w, h],
            "centre": [int(np.mean(bx)), int(np.mean(by))],
            "aspect": round(w / float(max(1, h)), 2)}


def measure(im):
    r, g, b = im[..., 0], im[..., 1], im[..., 2]
    mx = np.maximum(np.maximum(r, g), b)
    lum = 0.2126 * r + 0.7152 * g + 0.0722 * b
    gnd = ground_mask(im.shape)

    # 1. tail-light spill: red-dominant ground, high-passed
    redness = r - np.maximum(g, b)
    red_hi = highpass(redness)
    red = blob_stats(red_hi, gnd, im)
    red["profile"] = radial_profile(red_hi, red["centre"], gnd) if red["area_px"] else []

    # 2. headlight pool: bright ground that is NOT red-dominant
    lum_hi = highpass(lum)
    not_red = redness < 25.0
    head = blob_stats(lum_hi, gnd & not_red, im)
    head["profile"] = (radial_profile(lum_hi, head["centre"], gnd & not_red)
                       if head["area_px"] else [])

    # 3. particles: bright pink pixels anywhere, plus how much brighter than their surround
    pink = (np.minimum(r, b) - g > PINK_MIN) & (mx > BRIGHT_MIN)
    smoke = {"area_px": int(pink.sum()),
             "mean_rgb": [round(float(v), 1) for v in im[pink].mean(0)] if pink.sum() else [0, 0, 0],
             "peak_rgb": [int(v) for v in im[pink].max(0)] if pink.sum() else [0, 0, 0]}
    if pink.sum():
        lum_hi_pink = lum_hi[pink]
        smoke["mean_hi_luma"] = round(float(lum_hi_pink.mean()), 2)
        smoke["peak_hi_luma"] = round(float(lum_hi_pink.max()), 2)
        ys, xs = np.nonzero(pink)
        smoke["centre"] = [int(xs.mean()), int(ys.mean())]
        smoke["pink_mean"] = round(float((np.minimum(r, b) - g)[pink].mean()), 2)
    else:
        smoke["mean_hi_luma"] = 0.0
        smoke["peak_hi_luma"] = 0.0
        smoke["centre"] = [0, 0]
        smoke["pink_mean"] = 0.0

    # 4. windows (V7): clipped near-white blocks below the horizon. A lit window IS a light
    #    source and may be bright; what "flat white block" means is that the emissive term
    #    alone saturates the tonemapper, so the block has no texture and no colour. The
    #    metrics are therefore the clipped area, the size of the biggest clipped block (a
    #    window is a few hundred pixels wide at this distance, a whole facade is not), and
    #    the chroma of the clipped pixels: an authored emissive white has none, a real lit
    #    window carries its lamp's spectrum.
    clipmask = (np.minimum(np.minimum(r, g), b) >= CLIP_MIN) & gnd
    win = {"area_px": int(clipmask.sum()),
           "pct_of_ground": round(100.0 * clipmask.sum() / max(1, gnd.sum()), 3)}
    if win["area_px"]:
        win["chroma_mean"] = round(
            float((mx - np.minimum(np.minimum(r, g), b))[clipmask].mean()), 2)
        win["mean_rgb"] = [round(float(v), 1) for v in im[clipmask].mean(0)]
        win["largest"] = largest_component(clipmask)
    else:
        win["chroma_mean"] = 0.0
        win["mean_rgb"] = [0, 0, 0]
        win["largest"] = {"area_px": 0, "extent_px": [0, 0], "centre": [0, 0],
                          "aspect": 0.0}

    # 5. the flat ambient floor, for context: ground luma and how uniform it is
    gl = lum[gnd]
    ambient = {"ground_mean_luma": round(float(gl.mean()), 2),
               "ground_p95_luma": round(float(np.percentile(gl, 95)), 2)}

    return {"red_spill": red, "head_pool": head, "smoke": smoke, "windows": win,
            "ambient": ambient}


def frame_index(name):
    m = re.search(r"(\d+)", os.path.basename(name))
    return int(m.group(1)) if m else -1


def parse_frames(spec, indices):
    if not spec:
        return indices
    out = []
    for part in spec.split(","):
        part = part.strip()
        if not part:
            continue
        if "-" in part:
            a, bb = part.split("-", 1)
            out += [i for i in indices if int(a) <= i <= int(bb)]
        else:
            out += [i for i in indices if i == int(part)]
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("dir", help="directory holding fNNN.png frames")
    ap.add_argument("--frames", default="", help="e.g. 4-40 or 8,15,49 (default: every frame)")
    ap.add_argument("--json", help="also write the full result here")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args(argv)

    files = sorted(glob.glob(os.path.join(args.dir, "*.png")), key=frame_index)
    if not files:
        print(f"no png frames in {args.dir}", file=sys.stderr)
        return 1
    by_idx = {frame_index(f): f for f in files}
    want = parse_frames(args.frames, sorted(by_idx))
    if not want:
        print(f"--frames {args.frames!r} matched nothing", file=sys.stderr)
        return 1

    results = {}
    for i in want:
        res = measure(load(by_idx[i]))
        results[i] = res
        if not args.quiet:
            rs, hp, sm = res["red_spill"], res["head_pool"], res["smoke"]
            print(f"f{i:03d} red: area={rs['area_px']:6d}px r={rs['equiv_radius_px']:6.1f} "
                  f"peak_hi={rs['peak_hi']:6.1f} core={rs['core_px']:5d} rgb={rs['peak_rgb']} | "
                  f"head: area={hp['area_px']:6d}px r={hp['equiv_radius_px']:6.1f} "
                  f"peak_hi={hp['peak_hi']:6.1f} rgb={hp['peak_rgb']} | "
                  f"smoke: area={sm['area_px']:5d}px rgb={sm['mean_rgb']} "
                  f"pink={sm['pink_mean']:5.1f} hi_luma={sm['mean_hi_luma']:6.2f} | "
                  f"win: clip={res['windows']['area_px']:5d}px "
                  f"({res['windows']['pct_of_ground']:5.3f}%) "
                  f"big={res['windows']['largest']['area_px']:5d}px "
                  f"{res['windows']['largest']['extent_px']} "
                  f"chroma={res['windows']['chroma_mean']:5.1f}")
    if args.json:
        with open(args.json, "w") as fh:
            json.dump(results, fh, indent=1)

    # summary: the numbers the task asks for, as one line each
    def agg(key, sub):
        vals = [results[i][key][sub] for i in want]
        return (min(vals), round(float(np.mean(vals)), 1), max(vals))

    print()
    for key, label in (("red_spill", "red spill on the sand"),
                       ("head_pool", "headlight pool on the sand")):
        a = agg(key, "area_px")
        r = agg(key, "equiv_radius_px")
        p = agg(key, "peak_hi")
        c = agg(key, "core_px")
        print(f"{label:28s} area px  min/mean/max {a[0]}/{a[1]}/{a[2]}   "
              f"equiv radius {r[0]}/{r[1]}/{r[2]}   peak hi-pass {p[0]}/{p[1]}/{p[2]}   "
              f"source-core px {c[0]}/{c[1]}/{c[2]}")
    sm = [results[i]["smoke"] for i in want]
    print(f"{'smoke / particle puffs':28s} area px  min/mean/max "
          f"{min(s['area_px'] for s in sm)}/"
          f"{round(float(np.mean([s['area_px'] for s in sm])), 1)}/"
          f"{max(s['area_px'] for s in sm)}   "
          f"mean RGB {np.mean([s['mean_rgb'] for s in sm], axis=0).round(1)}   "
          f"pink min(R,B)-G {round(float(np.mean([s['pink_mean'] for s in sm])), 1)}")
    wn = [results[i]["windows"] for i in want]
    print(f"{'clipped-white windows':28s} ground %%  min/mean/max "
          f"{min(w['pct_of_ground'] for w in wn):.3f}/"
          f"{round(float(np.mean([w['pct_of_ground'] for w in wn])), 3)}/"
          f"{max(w['pct_of_ground'] for w in wn):.3f}   "
          f"biggest block {max(w['largest']['area_px'] for w in wn)} px   "
          f"chroma {round(float(np.mean([w['chroma_mean'] for w in wn])), 1)}   "
          f"mean RGB {np.mean([w['mean_rgb'] for w in wn], axis=0).round(1)}")
    if args.json:
        print(f"json: {args.json}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
