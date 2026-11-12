#!/usr/bin/env python3
"""s32: regenerate the pack's water normal sheet so the sea stops reading as black/white speckle.

    usage: tools/water_normal_soft.py SRC_DDS OUT_DDS [--target-tilt 12] [--decimate 4]
                                              [--frames 20] [--report]

Why this exists (measurements in .strata/notes-s32-water-vegetation.md):
the pack's `water_normal.dds` is a 10240x512 BC5 sheet of 20 frames of 512x512, octahedral.
Decoded exactly the way AperturePBR_Normal.mdl decodes it (lookup_float2 -> unorm octahedral),
its facet tilt has a median of ~54 deg and 42 % of texels over 60 deg, and the tilt field is
decorrelated after ~9 px along u. A perfect-mirror translucent material (the mdl has no
roughness input) turns that into a per-pixel black/white sheet: the 2026-11-12 water tour shot
measures a high-pass residual whose autocorrelation drops below 0.5 at lag 2 px.
`rtx.translucentMaterial.normalIntensity` (the runtime's `factor`) only shrinks the tilt; it
cannot remove the high frequencies, and the mdl has no normal-strength / roughness / UV-scale
input at all. So the fix has to be the map.

What this tool does, in order:
  1. decode mip 0 of the BC5 sheet (two BC4 blocks per 4x4 block) to unit normals;
  2. low-pass + decimate the octahedral xy field by --decimate (a proper area average, so the
     surviving waves are the low-frequency ones and nothing new is invented);
  3. blend every normal toward the flat normal with the mdl's own blend
     n' = normalize(n*f + (1-f)*up) - the same math the runtime applies for normalIntensity -
     with f chosen by bisection so the median tilt lands on --target-tilt;
  4. re-encode octahedral (unorm, 2 channels used) and write an RGBA8 DDS with a full mip chain,
     same frame layout (20 frames side by side), so sprite_sheet_cols/rows/fps stay valid.

The output is a *new* file next to the original; the pack's water_normal.dds is never touched.
BC5 cannot be re-encoded here, and the mdl's octahedral path reads only two channels
(`tex::lookup_float2`), so RGBA8 is the safe, universally-loadable format for the same data.
Exit 0 on success, 2 on bad usage / unreadable source.
"""
import argparse
import struct
import sys

import numpy as np

DDS_MAGIC = b"DDS "
DXGI_BC5 = 83


# ----------------------------------------------------------------------------- decode ---
def bc4_planes(block):
    """(n,8) LUT + (n,16) 3-bit codes -> (n,16) values, per the BC4/BC5 alpha block."""
    a0 = block[:, 0].astype(np.uint16)
    a1 = block[:, 1].astype(np.uint16)
    bits = np.zeros((len(block), 48), dtype=np.uint8)
    for i in range(6):
        byte = block[:, 2 + i].astype(np.uint16)
        for k in range(8):
            bits[:, 8 * i + k] = (byte >> k) & 1
    codes = np.zeros((len(block), 16), dtype=np.uint8)
    for i in range(16):
        v = np.zeros(len(block), dtype=np.uint16)
        for k in range(3):
            v = (v << 1) | bits[:, 3 * i + k]
        codes[:, i] = v
    lut0 = np.stack([a0, a1, (6 * a0 + a1) // 7, (5 * a0 + 2 * a1) // 7, (4 * a0 + 3 * a1) // 7,
                     (3 * a0 + 4 * a1) // 7, (2 * a0 + 5 * a1) // 7, (a0 + 6 * a1) // 7], axis=1)
    lut1 = np.stack([a0, a1, (4 * a0 + a1) // 5, (3 * a0 + 2 * a1) // 5, (2 * a0 + 3 * a1) // 5,
                     (a0 + 4 * a1) // 5, np.zeros_like(a0), np.full_like(a0, 255)], axis=1)
    lut = np.where((a0 > a1)[:, None], lut0, lut1).astype(np.uint8)
    return lut[np.arange(len(block))[:, None], codes]


def read_dds(path):
    """-> (w, h, dxgi_or_None, mip0_bytes). Only the mip-0 payload is returned."""
    data = open(path, "rb").read()
    if data[:4] != DDS_MAGIC:
        raise ValueError("%s: not a DDS file (magic %r)" % (path, data[:4]))
    hdr = 4 + int.from_bytes(data[4:8], "little")
    h = int.from_bytes(data[12:16], "little")
    w = int.from_bytes(data[16:20], "little")
    fourcc = data[84:88]
    off, dxgi = hdr, None
    if fourcc == b"DX10":
        dxgi = int.from_bytes(data[off:off + 4], "little")
        off += 20
    return w, h, dxgi, data[off:]


def decode_octa_rg(R, G):
    """unorm octahedral (R,G) -> unit normals (3,H,W), exactly AperturePBR_Normal.mdl's decode."""
    x = 2 * (R / 255.0) - 1
    y = 2 * (G / 255.0) - 1
    tx = (x + y) / 2
    ty = (x - y) / 2
    z = 1 - np.abs(tx) - np.abs(ty)
    n = np.stack([tx, ty, np.maximum(z, 0.0)])
    return n / np.sqrt((n * n).sum(axis=0, keepdims=True) + 1e-12)


def encode_octa(n):
    """unit normals -> (R,G) unorm octahedral, the exact inverse of decode_octa_rg.

    The octahedral map is not "store x,y": decode() re-derives z = 1-|tx|-|ty| and then
    normalises, so the encoded pair must be scaled by s = 1/(|x|+|y|+z) for the round trip to
    land back on n (without s a 30 deg normal comes back as 45 deg)."""
    x, y, z = n[0], n[1], n[2]
    s = 1.0 / (np.abs(x) + np.abs(y) + z + 1e-12)
    R = np.clip(s * (x + y) * 0.5 + 0.5, 0, 1) * 255.0
    G = np.clip(s * (x - y) * 0.5 + 0.5, 0, 1) * 255.0
    return R.astype(np.uint8), G.astype(np.uint8)


def tilt_deg(n):
    return np.degrees(np.arccos(np.clip(n[2], -1.0, 1.0)))


# ----------------------------------------------------------------------------- process ---
def octa_xy(n):
    """unit normals -> the snorm octahedral pair (ex,ey) in [-1,1] (linear, filterable)."""
    return np.stack([n[0] + n[1], n[0] - n[1]])


def xy_to_n(ex, ey):
    tx, ty = ex / 2.0, ey / 2.0
    z = np.maximum(1 - np.abs(tx) - np.abs(ty), 0.0)
    n = np.stack([tx, ty, z])
    return n / np.sqrt((n * n).sum(axis=0, keepdims=True) + 1e-12)


def half(a):
    """Box-filter a 2-D field down by 2, repeating the last row/column when the size is odd."""
    h, w = a.shape
    if h % 2:
        a = np.concatenate([a, a[-1:]], axis=0)
    if w % 2:
        a = np.concatenate([a, a[:, -1:]], axis=1)
    h, w = a.shape
    return a.reshape(h // 2, 2, w // 2, 2).mean(axis=(1, 3))


def decimate_xy(ex, ey, k):
    """Area-average the octahedral field by k (k=1 keeps the size)."""
    if k <= 1:
        return ex, ey
    h, w = ex.shape
    h2, w2 = h // k, w // k
    out = []
    for f in (ex, ey):
        out.append(f[:h2 * k, :w2 * k].reshape(h2, k, w2, k).mean(axis=(1, 3)))
    return out


def apply_strength(n, f):
    """The mdl's own blend: normalize(n*f + (1-f)*up)."""
    m = np.stack([n[0] * f, n[1] * f, n[2] * f + (1.0 - f)])
    return m / np.sqrt((m * m).sum(axis=0, keepdims=True) + 1e-12)


def autocorr_u(m, maxlag=32):
    m = m - m.mean()
    e = float((m * m).sum())
    return [float((m[:, :-l] * m[:, l:]).sum()) / e for l in range(1, maxlag + 1)]


def first_half(ac):
    for i, v in enumerate(ac):
        if v < 0.5:
            return i + 1
    return None


# ------------------------------------------------------------------------------- write ---
def write_dds_rgba8(path, rgba_mips):
    """Classic (non-DX10) DDS header, RGBA8, full mip chain. rgba_mips[0] is (H,W,4)."""
    h, w = rgba_mips[0].shape[:2]
    linear = w * 4
    flags = 0x1 | 0x2 | 0x4 | 0x1000 | 0x20000          # caps|height|width|pixelformat|mipmap
    pf = struct.pack("<II4sIIIII", 32, 0x40 | 0x1, b"\0\0\0\0", 32,
                     0x000000FF, 0x0000FF00, 0x00FF0000, 0xFF000000)
    assert len(pf) == 32, len(pf)
    caps = 0x1000 | 0x8 | 0x400000                       # texture|complex|mipmap
    hdr = (DDS_MAGIC + struct.pack("<IIIIIII", 124, flags, h, w, linear, 0, len(rgba_mips)) +
           b"\0" * 44 + pf +
           struct.pack("<IIIII", caps, 0, 0, 0, 0))
    assert len(hdr) == 128, len(hdr)
    with open(path, "wb") as fh:
        fh.write(hdr)
        for m in rgba_mips:
            fh.write(m.astype(np.uint8).tobytes())
    return hdr


# -------------------------------------------------------------------------------- main ---
def main(argv=None):
    ap = argparse.ArgumentParser(prog="water_normal_soft.py", description=__doc__.splitlines()[0])
    ap.add_argument("src", help="the pack's water_normal.dds (BC5, 20 frames side by side)")
    ap.add_argument("out", help="output DDS (RGBA8, same frame layout)")
    ap.add_argument("--target-tilt", type=float, default=12.0,
                    help="median facet tilt to reach, deg (default 12 = what s02 measured for "
                         "rtx.translucentMaterial.normalIntensity = 0.35)")
    ap.add_argument("--decimate", type=int, default=4, help="linear decimation of the sheet (default 4)")
    ap.add_argument("--frames", type=int, default=20, help="columns in the sheet (default 20)")
    ap.add_argument("--report", action="store_true", help="print the before/after numbers")
    a = ap.parse_args(argv)

    w, h, dxgi, payload = read_dds(a.src)
    if dxgi != DXGI_BC5:
        print("water_normal_soft.py: %s is dxgi=%s, expected 83 (BC5_UNORM)" % (a.src, dxgi),
              file=sys.stderr)
        return 2
    fw = w // a.frames
    if fw * a.frames != w or fw % max(a.decimate, 1):
        print("water_normal_soft.py: %dx%d with %d frames is not divisible by --decimate %d"
              % (w, h, a.frames, a.decimate), file=sys.stderr)
        return 2
    blocks = np.frombuffer(payload[:(w // 4) * (h // 4) * 16], dtype=np.uint8).reshape(-1, 16)
    R = bc4_planes(blocks[:, 0:8]).reshape(h // 4, w // 4, 4, 4).reshape(h, w)
    G = bc4_planes(blocks[:, 8:16]).reshape(h // 4, w // 4, 4, 4).reshape(h, w)
    n0 = decode_octa_rg(R, G)
    t0 = tilt_deg(n0)
    ac0 = autocorr_u(octa_xy(n0)[0])

    k = max(1, int(a.decimate))
    ex, ey = octa_xy(n0)
    ex, ey = decimate_xy(ex, ey, k)
    n1 = xy_to_n(ex, ey)

    lo, hi = 0.0, 1.0
    for _ in range(24):
        f = (lo + hi) / 2
        if float(np.median(tilt_deg(apply_strength(n1, f)))) > a.target_tilt:
            hi = f
        else:
            lo = f
    f = (lo + hi) / 2
    n2 = apply_strength(n1, f)
    t2 = tilt_deg(n2)
    R2, G2 = encode_octa(n2)
    h2, w2 = R2.shape

    mips = []
    cur = np.dstack([R2, G2, np.zeros_like(R2), np.full_like(R2, 255)])
    mips.append(cur)
    exx, eyy = ex.copy(), ey.copy()
    while (exx.shape[0], exx.shape[1]) != (1, 1):
        exx, eyy = half(exx), half(eyy)
        mr, mg = encode_octa(apply_strength(xy_to_n(exx, eyy), f))
        mips.append(np.dstack([mr, mg, np.zeros_like(mr), np.full_like(mr, 255)]))
    write_dds_rgba8(a.out, mips)

    ac2 = autocorr_u(octa_xy(n2)[0])
    def grad(m):
        return 0.5 * (np.abs(np.diff(m, axis=1)).mean() + np.abs(np.diff(m, axis=0)).mean())
    g0, g2 = grad(t0), grad(t2)
    print("wrote %s: %dx%d RGBA8, %d mips, %d frames of %dx%d, strength factor f=%.3f"
          % (a.out, w2, h2, len(mips), a.frames, w2 // a.frames, h2, f))
    print("  median facet tilt  %6.1f deg -> %6.1f deg   (target %.1f)"
          % (float(np.median(t0)), float(np.median(t2)), a.target_tilt))
    print("  tilt p95           %6.1f deg -> %6.1f deg" % (float(np.percentile(t0, 95)),
                                                           float(np.percentile(t2, 95))))
    print("  tilt > 60 deg      %6.2f%%  -> %6.2f%%" % (100 * float((t0 > 60).mean()),
                                                        100 * float((t2 > 60).mean())))
    print("  |grad tilt|        %6.2f deg/texel -> %6.2f deg/texel  (this is what makes the"
          " surface read as per-pixel noise)" % (g0, g2))
    l0, l2 = first_half(ac0) or 33, first_half(ac2) or 33
    print("  u decorrelation    %d px of a %d px frame -> %d px of a %d px frame"
          "   (independent facets along u per frame: %d -> %d)"
          % (l0, fw, l2, w2 // a.frames, max(1, fw // l0), max(1, (w2 // a.frames) // l2)))
    print("  sheet              %dx%d BC5 -> %dx%d RGBA8" % (w, h, w2, h2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
