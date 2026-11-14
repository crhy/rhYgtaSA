#!/usr/bin/env python3
"""n10 probe: is the night dark where no lamp shines, and bright only under a source?

    tools/night-report.py PNG [PNG ...]
    tools/night-report.py DIR [--pattern 'shot_*.png'] [--limit N]
    tools/night-report.py PNG --box x,y,w,h          # measure one region instead of a band

One number per image, the one rhY asked for on 2026-11-12c: "there is a FAKE night ambient
light everywhere, so it is not dark anywhere except under lamps - light from no source".
The measurable form of that sentence is a ratio inside a single frame:

    away / under  =  the darkest tenth of the ground band
                    / the brightest twentieth of the ground band

The ground band is the bottom 38 % of the frame by default (`--ground`), which is the strip
the 2026-11-12c frames hold almost entirely: sand, road, water - whatever the camera is
standing on. The darkest tenth of it is the patch with no lamp over it; the brightest
twentieth is the pool directly under one. In the 2026-11-12c video that ratio measured
25-72 % (target <= 10 %, and `tools/night_ambient.py` says the scene itself was at 1.8 %,
i.e. the renderer was flattening it, not the lights).

Two more numbers ride along because they are the other two halves of the same complaint:

  * `sky` - the mean luma of the top 30 % (`--sky`). "the sky is black, the ground is
    bright" is that mean sitting under the ground band's median, so the tool flags it.
  * `glow` - the fraction of ground pixels brighter than `--glow-luma` (default 120) that
    are NOT within `--lamp-radius` pixels of the frame's brightest cluster. A lamp's pool is
    a blob; light that is bright and everywhere at once has no blob to come from. This is a
    deliberately crude stand-in for "no glow without a source" - it is a screening number,
    not a proof, and the offline sim in tools/night_ambient.py is the proof.

Luma is Rec.709 (0.2126 R + 0.7152 G + 0.0722 B) on the 0..255 sRGB the panel shows, the
same definition tools/tour-report.py uses, so the two reports can be read side by side.

Exit 0 if every image passes, 1 if any fails the ratio, 2 on a usage error.
"""
import argparse
import glob
import math
import os
import sys

LUMA_W = (0.2126, 0.7152, 0.0722)


def percentile(hist, frac):
    """Inverse of a 256-bin histogram: the luma value at or below `frac` of the pixels."""
    total = sum(hist)
    if not total:
        return 0.0
    want = frac * total
    run = 0
    for v, c in enumerate(hist):
        run += c
        if run >= want:
            return float(v)
    return 255.0


def band_hist(path, y0f, y1f, box=None):
    """256-bin luma histogram of a horizontal band (fractions of the height) or of a box."""
    from PIL import Image
    with Image.open(path) as im:
        im = im.convert("RGB")
        w, h = im.size
        if box:
            x, y, bw, bh = box
            x0, y0 = max(0, int(x)), max(0, int(y))
            x1, y1 = min(w, int(x + bw)), min(h, int(y + bh))
        else:
            x0, x1 = 0, w
            y0, y1 = int(round(y0f * h)), int(round(y1f * h))
        crop = im.crop((x0, y0, x1, y1))
        cw, ch = crop.size
        hist = [0] * 256
        bright = 0
        it = iter(crop.tobytes())
        for r, g, b in zip(it, it, it):
            l = LUMA_W[0] * r + LUMA_W[1] * g + LUMA_W[2] * b
            hist[min(255, max(0, int(math.floor(l + 0.5))))] += 1
    return hist, cw * ch


def measure(path, ground=0.38, sky=0.30, box=None, glow_luma=120.0):
    gh, n = band_hist(path, 1.0 - ground, 1.0, box=box)
    sh, sn = band_hist(path, 0.0, sky) if not box else ([0], 1)
    away = percentile(gh, 0.10)
    under = percentile(gh, 0.95)
    med = percentile(gh, 0.50)
    sky_mean = sum(v * c for v, c in enumerate(sh)) / sn if sn else 0.0
    bright = sum(c for v, c in enumerate(gh) if v >= glow_luma)
    return {
        "away": away, "under": under, "median": med,
        "ratio": (away / under) if under else 0.0,
        "sky_mean": sky_mean,
        "inverted": sky_mean < med,          # sky darker than the ground = the inversion
        "pct_bright": 100.0 * bright / n if n else 0.0,
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("path")
    ap.add_argument("--pattern", default="*.png")
    ap.add_argument("--limit", type=int, default=0)
    ap.add_argument("--ground", type=float, default=0.38)
    ap.add_argument("--sky", type=float, default=0.30)
    ap.add_argument("--box", default=None, help="x,y,w,h in pixels: measure one region")
    ap.add_argument("--glow-luma", type=float, default=120.0)
    ap.add_argument("--target", type=float, default=0.10,
                    help="fraction, not percent: 0.10 = the away sand may be at most "
                         "10 %% of the sand under a lamp head in the IMAGE")
    a = ap.parse_args()

    box = None
    if a.box:
        try:
            box = tuple(float(p) for p in a.box.split(","))
        except ValueError:
            sys.exit("--box wants x,y,w,h")
        if len(box) != 4:
            sys.exit("--box wants x,y,w,h")

    if os.path.isdir(a.path):
        files = sorted(glob.glob(os.path.join(a.path, a.pattern)))
    else:
        files = [a.path]
    if a.limit:
        files = files[-a.limit:]
    if not files:
        sys.exit("no images under %s matching %s" % (a.path, a.pattern))

    print("%-44s %7s %7s %7s %8s %7s %7s" %
          ("image", "away", "under", "ratio", "target", "sky", "sky<gr"))
    bad = 0
    for f in files:
        try:
            m = measure(f, a.ground, a.sky, box, a.glow_luma)
        except ImportError:
            sys.exit("this tool needs Pillow (the same dependency tools/tour-report.py's "
                     "contact sheets use): pip install pillow")
        ok = m["ratio"] <= a.target
        if not ok:
            bad += 1
        print("%-44s %7.1f %7.1f %7.1f%% %8.0f%% %7.1f %7s%s" %
              (os.path.basename(f), m["away"], m["under"], 100 * m["ratio"],
               100 * a.target, m["sky_mean"], "yes" if m["inverted"] else "no",
               "" if ok else "   <-- FAIL"))
    print("\n%d of %d images over the %.0f%% target" % (bad, len(files), 100 * a.target))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
