#!/usr/bin/env python3
"""Measure the six look metrics that separate our shots from the reference video.

    usage: tools/target-gap.py IMG [IMG ...] [--label NAME] [--scale WxH] [--jpeg Q]
           tools/target-gap.py --ours GAME_DIR --ref IMG [--since ...] (see --help epilog)

Why this tool exists (.strata/n15-target-research.md): rhY wants our game to look better than
the DubStepZz "GTA SA DLSS 5 / RTX 5090 / 4K max" showcase, and nobody can eyeball a difference
that is spread over a whole frame. Every number below is computed the same way for the reference
frame and for our own tour shots, so a gap is a number and a fix can be aimed at it.

The six metric families (names used in docs/TARGET.md):

  hist        Rec.709 luma histogram: mean, p05/p50/p95/p99/p99.9, max, %black (<= BLACK_MAX),
              %clip (>= CLIP_MIN).
  rolloff     how the top of the curve behaves. A filmic curve spends many pixels in the
              ROLL_LOW..ROLL_HIGH band and almost none exactly at the ceiling; a hard clip
              piles up at 255. Reported: band share, ceiling share (>= CEIL_MIN), and
              headroom = p99.9 / p50 (how much brighter the brightest 0.1 % is than the median).
  colour      mean HSV saturation overall and in the mid-tone band (p20..p80 of luma), plus
              warmth = mean(R - B) over the mid-tones (a warmth proxy that does not need a
              colour-temperature fit) and the share of mid-tone pixels whose hue is warm
              (HUE_WARM_LO..HUE_WARM_HI degrees, red/amber).
  local       local contrast: RMS of the high-pass image (channel minus a box blur of radius
              BLUR_RADIUS at the measured scale), normalised by mean luma so a brighter frame
              does not automatically win, and the median Michelson contrast over BLOCK x BLOCK
              tiles.
  haze        aerial perspective. In a street view distance grows with the row, so the frame is
              cut into a NEAR band (bottom) and a FAR band (just under the sky), local contrast
              and saturation are measured in both, and the ratios far/near are reported. A frame
              with real haze keeps far/near contrast high-ish (0.4-0.8) because the haze flattens
              the distance; a frame with no haze has far/near near 1.0 (everything equally crisp)
              and usually a black sky. The sky band's own luma and saturation are reported too,
              because "bright soft sky" vs "flat black sky" is half of the atmosphere question.
  spec        share of the frame that reads as a specular highlight: a pixel whose high-pass
              value is at least SPEC_HP, whose luma is at least max(SPEC_LUMA, the frame's own
              p95), and whose saturation is at most SPEC_SAT (a highlight is bright, local and
              near-neutral). The p95 floor is what makes the number comparable between a frame
              anchored at p50=110 and one anchored at p50=170: without it, a lifted flat frame
              "wins" the specular count without having a single real highlight - measured on
              the 2026-11-12 comparison, where our noon downtown shot scored HIGHER than the
              reference while having 100x less ceiling. Area share in per-mille, plus the count
              of connected blobs and the largest blob's area.

Normalisation, and why it is not optional: the reference is a YouTube frame (1280x720 or
1319x744, H.264/VP9-compressed, chroma subsampled) and our shots are 1920x1080 lossless PNGs.
Compression and resolution change local contrast and the specular count by more than most
lighting changes do, so by default every image is resized to --scale (default 640x360) and
round-tripped through JPEG at --jpeg (default 72) before measuring. Pass --jpeg 0 to skip the
round-trip and --scale 0 to measure at native size; both are honest only when applied to
every image in the same run.

Output: one block per image, then a comparison table (one row per metric, one column per image)
and, with --json, the whole thing machine-readable.
"""
import argparse
import glob
import json
import os
import sys

import numpy as np
from PIL import Image

# --- constants, all in 8-bit sRGB units unless the name says otherwise ----------------
BLACK_MAX = 8          # luma at or below this counts as black
CLIP_MIN = 245         # luma at or above this counts as clipped
ROLL_LOW, ROLL_HIGH = 200, 249   # the filmic roll-off band
CEIL_MIN = 252         # luma at or above this is the ceiling pile-up
MID_LO, MID_HI = 20, 80          # mid-tone percentile band for colour stats
HUE_WARM_LO, HUE_WARM_HI = 0, 70  # degrees counted as warm (red -> yellow)
BLUR_RADIUS = 6        # high-pass radius at the measured scale (see --scale)
BLOCK = 16             # tile size for the Michelson contrast median
SPEC_HP = 26.0         # high-pass value that makes a pixel "locally bright"
SPEC_LUMA = 150        # and this bright in absolute terms
SPEC_SAT = 0.30        # and this neutral
NEAR_BAND = (0.72, 0.95)   # fraction of height: the near/ground band
FAR_BAND = (0.30, 0.52)    # fraction of height: just under the sky, the distance
SKY_BAND = (0.00, 0.22)    # fraction of height: the sky band
LUMA_W = (0.2126, 0.7152, 0.0722)


def load_rgb(path):
    return np.asarray(Image.open(path).convert("RGB")).astype(np.float64)


def normalise(rgb, scale, jpeg_q, tmp_path):
    """Resize to `scale` (0 = keep) and round-trip through JPEG at jpeg_q (0 = skip)."""
    im = Image.fromarray(np.uint8(np.clip(rgb, 0, 255)))
    if scale:
        im = im.resize(scale, Image.LANCZOS)
    if jpeg_q:
        im.save(tmp_path, "JPEG", quality=jpeg_q)
        im = Image.open(tmp_path).convert("RGB")
    return np.asarray(im).astype(np.float64)


def luma(rgb):
    return rgb @ np.array(LUMA_W)


def hsv_sat(rgb):
    mx = rgb.max(axis=2)
    mn = rgb.min(axis=2)
    with np.errstate(invalid="ignore", divide="ignore"):
        s = np.where(mx > 0, (mx - mn) / mx, 0.0)
    return s, mx, mn


def hue_deg(rgb, mx, mn):
    """Hue in degrees, only meaningful where mx > mn."""
    r, g, b = rgb[..., 0], rgb[..., 1], rgb[..., 2]
    d = mx - mn
    h = np.zeros_like(mx)
    with np.errstate(invalid="ignore", divide="ignore"):
        rr = np.where(mx > 0, (mx - r) / d, 0.0)
        gg = np.where(mx > 0, (mx - g) / d, 0.0)
        bb = np.where(mx > 0, (mx - b) / d, 0.0)
    h = np.where(mx == r, bb - gg, h)
    h = np.where((mx != r) & (mx == g), 2.0 + rr - bb, h)
    h = np.where((mx != r) & (mx != g), 4.0 + gg - rr, h)
    h = (h * 60.0) % 360.0
    return np.where(d > 0, h, -1.0)


def box_blur(ch, radius):
    """Separable box blur via cumulative sums; edge-clamped, float-safe, same shape in/out."""
    if radius <= 0:
        return ch
    n = 2 * radius + 1
    p = np.pad(ch, ((radius, radius), (0, 0)), mode="edge")
    c = np.concatenate([np.zeros((1, p.shape[1])), np.cumsum(p, axis=0)], axis=0)
    out = (c[n:] - c[:-n]) / n          # exactly ch.shape[0] rows
    p = np.pad(out, ((0, 0), (radius, radius)), mode="edge")
    c = np.concatenate([np.zeros((p.shape[0], 1)), np.cumsum(p, axis=1)], axis=1)
    return (c[:, n:] - c[:, :-n]) / n   # exactly ch.shape


def highpass_rms(gray, radius=BLUR_RADIUS):
    hp = gray - box_blur(gray, radius)
    return float(np.sqrt(np.mean(hp * hp))), hp


def michelson_median(gray, block=BLOCK):
    h, w = gray.shape
    h2, w2 = (h // block) * block, (w // block) * block
    if h2 == 0 or w2 == 0:
        return 0.0
    t = gray[:h2, :w2].reshape(h2 // block, block, w2 // block, block)
    mx = t.max(axis=(1, 3))
    mn = t.min(axis=(1, 3))
    with np.errstate(invalid="ignore", divide="ignore"):
        c = np.where(mx + mn > 0, (mx - mn) / (mx + mn), 0.0)
    return float(np.median(c))


def band(rows, h):
    return int(round(h * rows[0])), int(round(h * rows[1]))


def measure(rgb):
    h, w, _ = rgb.shape
    g = luma(rgb)
    sat, mx, mn = hsv_sat(rgb)
    hue = hue_deg(rgb, mx, mn)
    out = {"size": [w, h]}

    # hist
    gl = g.ravel()
    out["hist"] = {
        "mean": float(gl.mean()),
        "p05": float(np.percentile(gl, 5)),
        "p50": float(np.percentile(gl, 50)),
        "p95": float(np.percentile(gl, 95)),
        "p99": float(np.percentile(gl, 99)),
        "p999": float(np.percentile(gl, 99.9)),
        "max": float(gl.max()),
        "pct_black": float((gl <= BLACK_MAX).mean() * 100.0),
        "pct_clip": float((gl >= CLIP_MIN).mean() * 100.0),
    }

    # rolloff
    out["rolloff"] = {
        "band_pct": float(((gl >= ROLL_LOW) & (gl <= ROLL_HIGH)).mean() * 100.0),
        "ceiling_pct": float((gl >= CEIL_MIN).mean() * 100.0),
        "headroom": float(out["hist"]["p999"] / max(out["hist"]["p50"], 1.0)),
    }

    # colour
    lo, hi = np.percentile(gl, MID_LO), np.percentile(gl, MID_HI)
    mid = (g >= lo) & (g <= hi)
    warm = mid & (hue >= HUE_WARM_LO) & (hue <= HUE_WARM_HI)
    out["colour"] = {
        "sat_mean": float(sat.mean()),
        "sat_mid": float(sat[mid].mean()) if mid.any() else 0.0,
        "warmth_mid": float((rgb[..., 0] - rgb[..., 2])[mid].mean()) if mid.any() else 0.0,
        "warm_share_pct": float(warm.sum() / max(mid.sum(), 1) * 100.0),
    }

    # local contrast
    rms, hp = highpass_rms(g)
    out["local"] = {
        "hp_rms": rms,
        "hp_rms_norm": rms / max(out["hist"]["mean"], 1.0),
        "michelson": michelson_median(g),
    }

    # haze / aerial perspective
    nb0, nb1 = band(NEAR_BAND, h)
    fb0, fb1 = band(FAR_BAND, h)
    sb0, sb1 = band(SKY_BAND, h)
    def band_stats(r0, r1):
        sub = g[r0:r1]
        subhp = hp[r0:r1]
        subs = sat[r0:r1]
        return {
            "luma": float(sub.mean()),
            "hp_rms": float(np.sqrt(np.mean(subhp * subhp))),
            "sat": float(subs.mean()),
        }
    near, far, sky = band_stats(nb0, nb1), band_stats(fb0, fb1), band_stats(sb0, sb1)
    out["haze"] = {
        "near": near, "far": far, "sky": sky,
        "far_over_near_contrast": far["hp_rms"] / max(near["hp_rms"], 1e-6),
        "far_over_near_sat": far["sat"] / max(near["sat"], 1e-6),
        "sky_minus_far_luma": sky["luma"] - far["luma"],
    }

    # specular
    p95 = out["hist"]["p95"]
    spec = (hp >= SPEC_HP) & (g >= max(SPEC_LUMA, p95)) & (sat <= SPEC_SAT)
    nblobs, blargest = _components(spec)
    out["spec"] = {
        "area_pm": float(spec.mean() * 1000.0),
        "floor_luma": float(max(SPEC_LUMA, p95)),
        "blobs": int(nblobs),
        "largest_blob_px": int(blargest),
    }
    return out


def _components(mask):
    """Cheap 4-connected component count + largest area (scanline union-find is overkill
    here: the mask is sparse and the frame is small)."""
    h, w = mask.shape
    seen = np.zeros((h, w), bool)
    count = 0
    biggest = 0
    for y in range(0, h, 2):
        for x in range(0, w, 2):
            if not mask[y, x] or seen[y, x]:
                continue
            count += 1
            stack = [(y, x)]
            seen[y, x] = True
            area = 0
            while stack:
                cy, cx = stack.pop()
                area += 4
                for dy, dx in ((2, 0), (-2, 0), (0, 2), (0, -2)):
                    ny, nx = cy + dy, cx + dx
                    if 0 <= ny < h and 0 <= nx < w and mask[ny, nx] and not seen[ny, nx]:
                        seen[ny, nx] = True
                        stack.append((ny, nx))
            biggest = max(biggest, area)
    return count, biggest


ROWS = [
    ("hist.mean", "mean luma"),
    ("hist.p05", "p05"),
    ("hist.p50", "p50"),
    ("hist.p95", "p95"),
    ("hist.p999", "p99.9"),
    ("hist.pct_black", "% black"),
    ("hist.pct_clip", "% clip"),
    ("rolloff.band_pct", "roll-off band %"),
    ("rolloff.ceiling_pct", "ceiling %"),
    ("rolloff.headroom", "headroom p99.9/p50"),
    ("colour.sat_mid", "sat (mid)"),
    ("colour.warmth_mid", "warmth R-B"),
    ("colour.warm_share_pct", "warm share %"),
    ("local.hp_rms_norm", "local contrast (norm)"),
    ("local.michelson", "Michelson median"),
    ("haze.near.hp_rms", "near local contrast"),
    ("haze.far.hp_rms", "far local contrast"),
    ("haze.far_over_near_contrast", "haze far/near contrast"),
    ("haze.far_over_near_sat", "haze far/near sat"),
    ("haze.sky.luma", "sky luma"),
    ("haze.sky.sat", "sky sat"),
    ("spec.area_pm", "specular area permille"),
    ("spec.floor_luma", "specular luma floor"),
    ("spec.blobs", "specular blobs"),
    ("spec.largest_blob_px", "largest specular blob"),
]


def get(d, dotted):
    cur = d
    for part in dotted.split("."):
        cur = cur[part]
    return cur


def fmt(v):
    if isinstance(v, float):
        return f"{v:.3f}" if abs(v) < 10 else f"{v:.1f}"
    return str(v)


def main(argv=None):
    ap = argparse.ArgumentParser(description="Measure the reference-vs-ours look gap.")
    ap.add_argument("images", nargs="*", help="image files (reference or ours)")
    ap.add_argument("--label", action="append", default=[],
                    help="label for the images, in the same order (repeatable)")
    ap.add_argument("--scale", default="640x360", help="WxH to measure at, 0 = native")
    ap.add_argument("--jpeg", type=int, default=72, help="JPEG quality round-trip, 0 = none")
    ap.add_argument("--json", help="also write machine-readable results here")
    args = ap.parse_args(argv)

    scale = None
    if args.scale != "0":
        w, h = args.scale.lower().split("x")
        scale = (int(w), int(h))

    if not args.images:
        ap.error("no images given")

    os.makedirs("build/tmp", exist_ok=True)
    tmp = os.path.join("build/tmp", "target-gap-roundtrip.jpg")

    results = []
    for i, path in enumerate(args.images):
        lab = args.label[i] if i < len(args.label) else os.path.basename(path)
        rgb = load_rgb(path)
        rgb = normalise(rgb, scale, args.jpeg, tmp)
        m = measure(rgb)
        m["label"] = lab
        m["source"] = path
        results.append(m)
        print(f"== {lab}  ({path}, measured at {m['size'][0]}x{m['size'][1]}, "
              f"jpeg={args.jpeg}, scale={args.scale})")
        for dotted, name in ROWS:
            print(f"   {name:28s} {fmt(get(m, dotted))}")
        print()

    names = [r["label"] for r in results]
    print("==" + "=" * 12 + "".join(f"{n[:22]:>24s}" for n in names))
    for dotted, name in ROWS:
        line = f"{name:28s}" + "".join(f"{fmt(get(r, dotted)):>24s}" for r in results)
        print(line)

    if args.json:
        with open(args.json, "w") as fh:
            json.dump({"scale": args.scale, "jpeg": args.jpeg, "results": results}, fh, indent=1)
        print(f"\nwrote {args.json}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
