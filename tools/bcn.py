#!/usr/bin/env python3
"""BC1/BC3/BC4/BC5 block encoders and a DDS writer, numpy only - and decoders to check them.

Why this file exists: RTX Remix replacement textures are block-compressed, and this box has no
BC encoder. Measured 2026-11-13 (n20, .strata/notes-n20-hd-pbr-textures.md):

  * Pillow 13.0 *reads* BC1-BC7 but its DDS writer ignores `rawmode`: saving with
    rawmode="BC7" produces a DX10 header with dxgiFormat 64 (R8G8B8A8_UNORM) and a lossless
    round-trip, i.e. uncompressed RGBA.
  * ImageMagick 7 ignores `-define dds-compression=BC7|BC5|DXT5|...` too: every variant of a
    256x256 gradient came back the same 87536-byte file with the same round-trip error.

So the encoders are written here. They implement the formats exactly as the Direct3D 11
documentation defines the blocks (the same layout libgpuarray/crunch/bc7enc use):

  BC4 (R8-ish, 8 bytes/block)  a0,a1 (u8) + 16 x 3-bit selectors, LSB-first from bit 16.
      selector 0 -> a0, 1 -> a1, 2..7 interpolate; when a0 > a1 the table is
      {(5a0+3a1)/8,(3a0+5a1)/8,(7a0+a1)/8,(6a0+2a1)/8,(4a0+4a1)/8,(2a0+6a1)/8},
      otherwise it is {(5a1+3a0)/8,...,(a0+7a1)/8} and 6,7 are 0 and 255.
  BC5 = two BC4 blocks (R in the first 8 bytes, G in the second).
  BC1 (4 bytes/block)  c0,c1 (RGB565) + 16 x 2-bit selectors, LSB-first from bit 32.
      c0 > c1 gives the four colours {c0,c1,(2c0+c1)/3,(c0+2c1)/3}; c0 <= c1 swaps the
      last two for {c0,c1,(c0+c1)/2,transparent-black}, which we never use (opaque).
  BC3 / DXT5 (16 bytes/block) = a BC4 block for alpha, then a BC1 block for colour.

BC7 is deliberately NOT here: its mode table (mode 6 = 1 subset, 7-bit endpoints + p-bits,
6-bit selectors, delta-encoded endpoints) is easy to get subtly wrong and there is no reference
encoder on this box to check against. The pack's own albedos are BC7 (DXGI 98/99), but BC3 costs
the same 1 byte/pixel and our albedos are 4x-upscaled copies of the game's own DXT1/DXT5 rasters,
which are already four-colours-per-4x4-block - so BC3 loses nothing the source had. See
docs/TEXTURES.md "Formats".

usage (library):
    from bcn import encode_bc4, encode_bc5, encode_bc3, write_dds, DXGI
    write_dds(path, encode_bc3(rgba, mips=True), w, h, DXGI["BC3_UNORM"], mips=9)

The block sizes are fixed at 4x4, so width and height must be multiples of 4 (DDS blocks cover
the whole image; the pack's textures are all powers of two, and so are SA's).
"""
from __future__ import annotations

import struct

import numpy as np

DXGI = {
    "BC3_UNORM": 71, "BC3_UNORM_SRGB": 72,
    "BC4_UNORM": 80, "BC4_SNORM": 81,
    "BC5_UNORM": 83, "BC5_SNORM": 84,
    "BC7_UNORM": 98, "BC7_UNORM_SRGB": 99,
    "R8G8B8A8_UNORM": 29, "R8G8B8A8_UNORM_SRGB": 30,
}
DXGI_NAME = {v: k for k, v in DXGI.items()}

# ------------------------------------------------------------------ interpolation tables

def _alpha_table(a0, a1):
    """The 8 values a BC4 / BC5 / DXT5-alpha block decodes to, from its two endpoints.

    Straight from the Khronos RGTC specification (EXT_texture_compression_rgtc, Appendix,
    fetched into .strata/notes-n20-hd-pbr-textures.md): eight values when a0 > a1, six when
    a0 <= a1, and the interpolation denominator is SEVEN, not eight -

        code 0..7 (a0 > a1): a0, a1, (6a0+a1)/7, (5a0+2a1)/7, (4a0+3a1)/7,
                             (3a0+4a1)/7, (2a0+5a1)/7, (a0+6a1)/7
        code 0..7 (a0 <= a1): a0, a1, (4a0+a1)/5, (3a0+2a1)/5, (2a0+3a1)/5,
                              (a0+4a1)/5, 0, 255

    An earlier version used /8 weights (mis-remembered from the D3D docs); encode and decode
    agreed with each other, and only decoding our own BC4/BC5 output with ffmpeg and Pillow -
    and regressing the pack's own BC5 files, which fit the /7 ladder with a 0.3 residual -
    showed it. The pack's normal maps decode to a mean of 126.9 with /7 and 105 with /8, which
    is the difference between a flat normal map and a broken one.
    """
    a0 = a0.astype(np.int32)
    a1 = a1.astype(np.int32)
    eight = a0 > a1
    out = np.empty((a0.shape[0], 8), dtype=np.int32)
    out[:, 0] = a0
    out[:, 1] = a1
    out[:, 2] = np.where(eight, (6 * a0 + a1) // 7, (4 * a0 + a1) // 5)
    out[:, 3] = np.where(eight, (5 * a0 + 2 * a1) // 7, (3 * a0 + 2 * a1) // 5)
    out[:, 4] = np.where(eight, (4 * a0 + 3 * a1) // 7, (2 * a0 + 3 * a1) // 5)
    out[:, 5] = np.where(eight, (3 * a0 + 4 * a1) // 7, (a0 + 4 * a1) // 5)
    out[:, 6] = np.where(eight, (2 * a0 + 5 * a1) // 7, 0)
    out[:, 7] = np.where(eight, (a0 + 6 * a1) // 7, 255)
    return out


def _color_table(c0, c1, p0, p1):
    """The 4 RGB colours a BC1 block decodes to.

    c0/c1 are the int32 (n,3) RGB565-rounded endpoint colours, p0/p1 the packed u16 values.
    The mode is decided by the PACKED comparison (the Direct3D spec compares the two 16-bit
    endpoint words, not their RGB sums - getting that wrong makes an encoder and a decoder
    disagree on whole blocks, which is exactly what a cross-check against Pillow showed):
    p0 > p1 gives the four-opaque mode, otherwise index 2 is the midpoint and index 3 is black
    with alpha 0, which is SA's DXT1 "punch-through" alpha.
    """
    four = (p0 > p1)[:, None]
    out = np.empty((c0.shape[0], 4, 3), dtype=np.int32)
    out[:, 0] = c0
    out[:, 1] = c1
    out[:, 2] = np.where(four, (2 * c0 + c1) // 3, (c0 + c1) // 2)
    out[:, 3] = np.where(four, (c0 + 2 * c1) // 3, 0)
    return out


def _rgb565(v):
    """Round-trip uint8 (n,3) through RGB565 the way the hardware does it."""
    v = np.clip(v, 0, 255).astype(np.int32)
    q = (v * 248 + 128) // 256                      # 5/6/5 bits
    out = np.empty_like(q)
    out[:, 0] = (q[:, 0] >> 3) * 8 + (q[:, 0] >> 3) // 4   # rrrrr -> rrrrrrrr
    out[:, 1] = (q[:, 1] >> 2) * 4 + (q[:, 1] >> 2) // 16
    out[:, 2] = (q[:, 2] >> 3) * 8 + (q[:, 2] >> 3) // 4
    return out


def _order(c0q, c1q):
    """Put the endpoint with the larger PACKED u16 first: that is what selects four-opaque mode."""
    swap = _pack565(c0q) <= _pack565(c1q)
    return np.where(swap[:, None], c1q, c0q), np.where(swap[:, None], c0q, c1q)


def _pack565(v):
    v = np.clip(v, 0, 255).astype(np.int32)
    return (v[:, 0] >> 3) << 11 | (v[:, 1] >> 2) << 5 | (v[:, 2] >> 3)


# ------------------------------------------------------------------ block splitting

def blocks(arr):
    """(h, w, c) uint8 -> (nblocks, 16, c), row-major 4x4 blocks, image size a multiple of 4.

    The image's own axis order is (block_row, y_in_block, block_col, x_in_block); the block
    stream's is (block_row, block_col, y_in_block, x_in_block), so the transpose is the job.
    Getting it wrong scrambles the picture by 4x4 tiles - and because encode and decode were both
    wrong in the same way, only a cross-decode against Pillow caught it.
    """
    if arr.ndim == 2:
        arr = arr[:, :, None]
    h, w, c = arr.shape
    if h % 4 or w % 4:
        raise ValueError("BCn needs a size that is a multiple of 4, got %dx%d" % (w, h))
    return (arr.reshape(h // 4, 4, w // 4, 4, c)
                .transpose(0, 2, 1, 3, 4).reshape(-1, 16, c))


def unblocks(flat, h, w, c):
    """The inverse of blocks(): (nblocks, 16, c) -> (h, w, c)."""
    return (flat.reshape(h // 4, w // 4, 4, 4, c)
                .transpose(0, 2, 1, 3, 4).reshape(h, w, c))


# ------------------------------------------------------------------ BC4 / BC5

def _encode_alpha_planes(vals, search=True):
    """vals (n,16) uint8 -> (bytes(n,8), decoded (n,16) uint8)."""
    vals = vals.astype(np.int32)
    lo, hi = vals.min(axis=1), vals.max(axis=1)
    cands = [(hi, lo)]
    if search:                                   # a tiny endpoint search: shrinking the
        cands += [(np.maximum(hi - 1, lo), lo),  # endpoints often beats plain min/max on
                  (hi, np.minimum(lo + 1, hi)),  # smooth gradients (mips, normal maps)
                  (np.maximum(hi - 2, lo), np.minimum(lo + 2, hi))]
    best_err, best = None, None
    for a0, a1 in cands:
        tab = _alpha_table(a0, a1)
        idx = np.abs(vals[:, :, None] - tab[:, None, :]).argmin(axis=2)
        err = ((tab[np.arange(len(a0))[:, None], idx] - vals) ** 2).sum(axis=1)
        if best_err is None:
            best_err, best = err, (a0, a1, idx)
        else:
            keep = err < best_err
            a0b, a1b, idxb = best
            a0, a1 = np.where(keep, a0, a0b), np.where(keep, a1, a1b)
            idx = np.where(keep[:, None], idx, idxb)
            best_err = np.where(keep, err, best_err)
            best = (a0, a1, idx)
    a0, a1, idx = best
    word = a0.astype(np.uint64) | (a1.astype(np.uint64) << 8)
    for i in range(16):
        word |= idx[:, i].astype(np.uint64) << (16 + 3 * i)
    packed = np.stack([(word >> (8 * b)).astype(np.uint8) for b in range(8)], axis=1)
    tab = _alpha_table(a0, a1)
    dec = tab[np.arange(len(a0))[:, None], idx].astype(np.uint8)
    return packed, dec


def encode_bc4(gray, mips=False):
    """(h,w) uint8 -> BC4 bytes (mip chain appended when mips)."""
    return _encode_chain(np.asarray(gray), ("BC4",), mips)


def encode_bc5(rg, mips=False):
    """(h,w,2) uint8 -> BC5 bytes (two BC4 planes per block)."""
    return _encode_chain(np.asarray(rg), ("BC5",), mips)


def decode_bc4(data, w, h):
    return unblocks(_decode_alpha_planes(data, 0, w, h), h, w, 1)[:, :, 0]


def decode_bc5(data, w, h):
    r = unblocks(_decode_alpha_planes(data, 0, w, h, stride=16), h, w, 1)[:, :, 0]
    g = unblocks(_decode_alpha_planes(data, 8, w, h, stride=16), h, w, 1)[:, :, 0]
    return np.dstack([r, g])


def _decode_alpha_planes(data, offset, w, h, stride=8):
    n = (w // 4) * (h // 4)
    d = np.frombuffer(data, dtype=np.uint8).reshape(-1, stride)[:n, offset:offset + 8]
    a0, a1 = d[:, 0].astype(np.int32), d[:, 1].astype(np.int32)
    tab = _alpha_table(a0, a1)
    idx = np.empty((n, 16), dtype=np.int32)
    for i in range(16):
        bit = 16 + 3 * i
        byte, sh = bit // 8, bit % 8
        v = d[:, byte].astype(np.int32) >> sh
        if sh > 5:
            v |= (d[:, byte + 1].astype(np.int32) << (8 - sh)) & 0b111
        idx[:, i] = v & 0b111
    return tab[np.arange(n)[:, None], idx].astype(np.uint8)


# ------------------------------------------------------------------ BC1 / BC3

def _encode_color(px):
    """px (n,16,3) int32 -> (bytes(n,8), decoded (n,16,3) int32)."""
    n = px.shape[0]
    # endpoints: the two most distant colours in the block, then one Lloyd refinement pass.
    d = ((px[:, :, None, :] - px[:, None, :, :]) ** 2).sum(axis=3)
    i, j = np.dstack(np.unravel_index(d.reshape(n, -1).argmax(axis=1), (16, 16)))[0].T
    c0, c1 = px[np.arange(n), i], px[np.arange(n), j]
    for _ in range(2):
        c0q, c1q = _rgb565(c0), _rgb565(c1)
        c0q, c1q = _order(c0q, c1q)            # keep packed c0 > c1 (the four-opaque mode)
        tab = _color_table(c0q, c1q, _pack565(c0q), _pack565(c1q))
        idx = ((px[:, :, None, :] - tab[:, None, :, :]) ** 2).sum(axis=3).argmin(axis=2)
        # re-estimate the endpoints from the pixels that actually use them (one Lloyd step)
        for mi, ci in ((0, "c0"), (1, "c1")):
            m = idx == mi
            cnt = m.sum(axis=1)
            mean = np.where(m[:, :, None], px, 0).sum(axis=1) / np.maximum(cnt, 1)[:, None]
            use = (cnt > 0)[:, None]
            if ci == "c0":
                c0 = np.where(use, mean, c0)
            else:
                c1 = np.where(use, mean, c1)
    c0q, c1q = _order(_rgb565(c0), _rgb565(c1))
    tab = _color_table(c0q, c1q, _pack565(c0q), _pack565(c1q))
    idx = ((px[:, :, None, :] - tab[:, None, :, :]) ** 2).sum(axis=3).argmin(axis=2)
    p0, p1 = _pack565(c0q).astype(np.uint64), _pack565(c1q).astype(np.uint64)
    word = p0 | (p1 << 16)
    for i in range(16):
        word |= idx[:, i].astype(np.uint64) << (32 + 2 * i)
    packed = np.stack([(word >> (8 * b)).astype(np.uint8) for b in range(8)], axis=1)
    dec = tab[np.arange(n)[:, None], idx]
    return packed, dec


def _decode_color(data, w, h, offset=0, stride=16):
    n = (w // 4) * (h // 4)
    d = np.frombuffer(data, dtype=np.uint8).reshape(-1, stride)[:n, offset:offset + 8]
    p0 = d[:, 0].astype(np.int32) | (d[:, 1].astype(np.int32) << 8)
    p1 = d[:, 2].astype(np.int32) | (d[:, 3].astype(np.int32) << 8)

    def unq(p, shift, bits):
        """Unpack one 565 field and expand it to 8 bits by replicating its top bits
        (the Direct3D convention): 5 bits -> (v<<3)|(v>>2), 6 bits -> (v<<2)|(v>>4)."""
        v = (p >> shift) & ((1 << bits) - 1)
        return (v << (8 - bits)) | (v >> (2 * bits - 8))

    c0 = np.stack([unq(p0, 11, 5), unq(p0, 5, 6), unq(p0, 0, 5)], axis=1)
    c1 = np.stack([unq(p1, 11, 5), unq(p1, 5, 6), unq(p1, 0, 5)], axis=1)
    tab = _color_table(c0, c1, p0, p1)
    idx = np.empty((n, 16), dtype=np.int32)
    for i in range(16):
        bit = 32 + 2 * i
        byte, sh = bit // 8, bit % 8
        v = d[:, byte].astype(np.int32) >> sh
        if sh > 6:
            v |= (d[:, byte + 1].astype(np.int32) << (8 - sh)) & 0b11
        idx[:, i] = v & 0b11
    return tab[np.arange(n)[:, None], idx].astype(np.uint8)


def encode_bc3(rgba, mips=False):
    """(h,w,4) uint8 -> BC3/DXT5 bytes (mip chain appended when mips)."""
    return _encode_chain(np.asarray(rgba), ("BC3",), mips)


def decode_dxt1(data, w, h):
    """DXT1/BC1 (8 bytes/block) -> RGBA (h,w,4); index 3 of a c0 <= c1 block is transparent."""
    rgb = unblocks(_decode_color(data, w, h, offset=0, stride=8), h, w, 3)
    n = (w // 4) * (h // 4)
    d = np.frombuffer(data, dtype=np.uint8).reshape(-1, 8)[:n]
    p0 = d[:, 0].astype(np.int32) | (d[:, 1].astype(np.int32) << 8)
    p1 = d[:, 2].astype(np.int32) | (d[:, 3].astype(np.int32) << 8)
    opaque = (p0 > p1)          # the spec compares the packed endpoint words
    a = np.full(rgb.shape[:2], 255, dtype=np.uint8)
    idx = _color_indices(data, w, h, offset=0, stride=8)
    trans = np.broadcast_to((~opaque)[:, None], idx.shape) & (idx == 3)
    a[unblocks(trans, h, w, 1)[:, :, 0]] = 0
    return np.dstack([rgb, a])


def _color_indices(data, w, h, offset=0, stride=16):
    n = (w // 4) * (h // 4)
    d = np.frombuffer(data, dtype=np.uint8).reshape(-1, stride)[:n, offset:offset + 8]
    idx = np.empty((n, 16), dtype=np.int32)
    for i in range(16):
        bit = 32 + 2 * i
        byte, sh = bit // 8, bit % 8
        v = d[:, byte].astype(np.int32) >> sh
        if sh > 6:
            v |= (d[:, byte + 1].astype(np.int32) << (8 - sh)) & 0b11
        idx[:, i] = v & 0b11
    return idx


def decode_bc3(data, w, h):
    rgb = unblocks(_decode_color(data, w, h, offset=8), h, w, 3)
    a = unblocks(_decode_alpha_planes(data, 0, w, h, stride=16), h, w, 1)[:, :, 0]
    return np.dstack([rgb, a])


# ------------------------------------------------------------------ the chain driver

def _encode_chain(arr, kinds, mips):
    kind = kinds[0]
    out = bytearray()
    w, h = arr.shape[1], arr.shape[0]
    lvl = arr
    while True:
        out += _encode_level(kind, _pad4(lvl))
        if not mips or (w == 1 and h == 1):
            break
        w, h = max(1, w // 2), max(1, h // 2)
        lvl = _box(lvl, w, h)
    return bytes(out)


def _pad4(arr):
    """A BCn block always covers 4x4 texels, so a mip level smaller than that still stores a
    whole block (the pack's 4096^2 files have 13 levels and their byte counts say exactly this).
    Replicate the edges so the block's colours stay representative."""
    three = arr.ndim == 3
    a = arr if three else arr[:, :, None]
    if a.shape[0] < 4:
        a = a[np.clip(np.arange(4), 0, a.shape[0] - 1), :]
    if a.shape[1] < 4:
        a = a[:, np.clip(np.arange(4), 0, a.shape[1] - 1)]
    return a if three else a[:, :, 0]


def _encode_level(kind, arr):
    b = blocks(arr)
    if kind == "BC4":
        return _encode_alpha_planes(b[:, :, 0])[0].tobytes()
    if kind == "BC5":
        r = _encode_alpha_planes(b[:, :, 0])[0]
        g = _encode_alpha_planes(b[:, :, 1])[0]
        return np.concatenate([r, g], axis=1).reshape(len(r), 16).tobytes()
    if kind == "BC3":
        col = _encode_color(b[:, :, :3])[0]
        alp = _encode_alpha_planes(b[:, :, 3])[0]
        return np.concatenate([alp, col], axis=1).reshape(len(col), 16).tobytes()
    raise ValueError("unknown kind %r" % kind)


def _box(arr, w, h):
    """2x box filter, one step. Albedo is filtered in the stored (sRGB-encoded) space, which
    is what the game's own mip chains and the pack's DDS files do."""
    three = arr.ndim == 3
    a = arr if three else arr[:, :, None]
    a = a.astype(np.float64)
    H, W, C = a.shape
    if H % 2:
        a = np.concatenate([a, a[-1:]], axis=0)
    if W % 2:
        a = np.concatenate([a, a[:, -1:]], axis=1)
    out = a.reshape(a.shape[0] // 2, 2, a.shape[1] // 2, 2, C).mean(axis=(1, 3))
    out = out[:h, :w]
    return out if three else out[:, :, 0]


def mip_sizes(w, h):
    sizes = []
    while True:
        sizes.append((w, h))
        if w == 1 and h == 1:
            return sizes
        w, h = max(1, w // 2), max(1, h // 2)


def mip_bytes(w, h, block_bytes_=16):
    """Bytes of a whole mip chain of 4x4 blocks (what a DDS of this size costs in VRAM)."""
    return sum(((mw + 3) // 4) * ((mh + 3) // 4) * block_bytes_
               for mw, mh in mip_sizes(w, h))


# ------------------------------------------------------------------ DDS

DDS_FLAGS = dict(ALLOCATION=0x1, HEIGHT=0x2, WIDTH=0x4, PITCH=0x8, PIXELFORMAT=0x1000,
                 MIPMAPCOUNT=0x20000, LINEARSIZE=0x80000)
DDPF = dict(FOURCC=0x4, RGB=0x40, DX10=0x2000)   # DDPF_DX10 is 0x2000, not 0x200


def block_bytes(dxgi):
    return {71: 16, 72: 16, 80: 8, 81: 8, 83: 16, 84: 16, 98: 16, 99: 16}[dxgi]


def write_dds(path, data, w, h, dxgi, mips=1, fourcc_legacy=None):
    """Write a 2D DDS. `data` is the mip chain, largest first, each level block-aligned.

    Offsets are the published ones: "DDS " at 0, dwSize=124 at 4, dwFlags at 8, height at 12,
    width at 16, dwPitchOrLinearSize at 20, depth at 24, mipCount at 28, the 32-byte pixel
    format at 76 (its dwFlags at 80 and its fourCC at 84), dwCaps at 108, and for the DX10
    variant the 20-byte DDS_HEADER_DXT10 at 128. The pack writes DX10 for everything except a
    handful of legacy DXT1/DXT3/DXT5 files; fourcc_legacy writes the old header instead.
    """
    bb = block_bytes(dxgi)
    levels = mip_sizes(w, h)[:mips] if mips else mip_sizes(w, h)
    hdr = bytearray(148)
    hdr[0:4] = b"DDS "
    struct.pack_into("<I", hdr, 4, 124)                                   # dwSize
    flags = (DDS_FLAGS["ALLOCATION"] | DDS_FLAGS["HEIGHT"] | DDS_FLAGS["WIDTH"]
             | DDS_FLAGS["PIXELFORMAT"] | DDS_FLAGS["LINEARSIZE"])
    if len(levels) > 1:
        flags |= DDS_FLAGS["MIPMAPCOUNT"]
    struct.pack_into("<I", hdr, 8, flags)
    struct.pack_into("<I", hdr, 12, h)
    struct.pack_into("<I", hdr, 16, w)
    # dwPitchOrLinearSize holds the byte size of the LARGEST level for compressed formats
    # (the DDS spec, and what the pack writes: 4194304 for a 2048x2048 BC5)
    struct.pack_into("<I", hdr, 20, ((w + 3) // 4) * ((h + 3) // 4) * bb)   # dwPitchOrLinearSize
    struct.pack_into("<I", hdr, 24, 0)                                   # dwDepth
    struct.pack_into("<I", hdr, 28, len(levels))
    struct.pack_into("<I", hdr, 76, 32)                                  # ddspf.dwSize
    if fourcc_legacy:
        struct.pack_into("<I", hdr, 80, DDPF["FOURCC"])
        hdr[84:88] = fourcc_legacy
    else:
        struct.pack_into("<I", hdr, 80, DDPF["FOURCC"] | DDPF["DX10"])
        hdr[84:88] = b"DX10"
        struct.pack_into("<I", hdr, 128, dxgi)                           # DDS_HEADER_DXT10
        struct.pack_into("<I", hdr, 132, 3)                              # TEXTURE2D
        struct.pack_into("<I", hdr, 136, 0)                              # miscFlag
        struct.pack_into("<I", hdr, 140, 1)                              # arraySize
        struct.pack_into("<I", hdr, 144, 0)                              # miscFlags2 = RGB
    struct.pack_into("<I", hdr, 108, 0x1008 if len(levels) > 1 else 0x1000)   # caps: TEXTURE|COMPLEX
    blob = bytes(hdr[:128 + (20 if not fourcc_legacy else 0)]) + data
    if path == "-":
        return blob
    with open(path, "wb") as fh:
        fh.write(blob)
    return blob


def read_dds(path):
    """(header dict, [(w, h, bytes), ...]) for every mip level of a DDS file."""
    with open(path, "rb") as fh:
        d = fh.read()
    if d[:4] != b"DDS ":
        raise ValueError("%s: not a DDS file" % path)
    flags, h, w, lin, depth, mips = struct.unpack_from("<IIIIII", d, 8)
    pfflags = struct.unpack_from("<I", d, 80)[0]
    fourcc = d[84:88]
    # the pack's own DX10 files set only DDPF_FOURCC (0x4) and not DDPF_DX10 (0x2000), so the
    # fourCC string is the reliable test; write_dds sets both bits
    dx10 = (pfflags & DDPF["DX10"]) or fourcc == b"DX10"
    dxgi = struct.unpack_from("<I", d, 128)[0] if dx10 else None
    bb = None
    if dxgi is not None:
        bb = block_bytes(dxgi)
    else:
        bb = {b"DXT1": 8, b"DXT3": 16, b"DXT5": 16}.get(fourcc)
    head = 128 + (20 if dx10 else 0)
    out, off = [], head
    for mw, mh in mip_sizes(w, h)[:max(1, mips)]:
        n = ((mw + 3) // 4) * ((mh + 3) // 4) * (bb or 1)
        out.append((mw, mh, d[off:off + n]))
        off += n
    hdr = dict(w=w, h=h, mips=max(1, mips), fourcc=fourcc, dxgi=dxgi,
               dxgi_name=DXGI_NAME.get(dxgi), block_bytes=bb, linear=lin, flags=flags,
               pfflags=pfflags, bytes=len(d))
    return hdr, out
