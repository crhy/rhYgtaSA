#!/usr/bin/env python3
"""s18 — compute RTX Remix's texture hash for a GTA SA .txd texture, offline.

Why this exists: Remix keys everything texture-related to a 64-bit hash of the texture *data* —
`rtx.conf` lists them (`rtx.animatedWaterTextures`, `rtx.particleTextures`, `rtx.uiTextures`,
`rtx.ignoreTextures`, ...), and replacement materials are prims named `mat_<HASH>` in the pack's
USD. Until now the only way to get one was the in-game developer menu (Alt+X, hover, "Hash:").
This tool reproduces the hash from the game's own .txd bytes, so e.g. `config/mirror-hashes.txt`
(s05) can be filled without running the game.

What Remix hashes (dxvk-remix 1.6.1 = the runtime in .trex/d3d9.dll of the RTX build; the game's
d3d9.dll is only the bridge client that forwards CreateTexture/LockRect to it):

    src/d3d9/d3d9_common_texture.cpp, D3D9CommonTexture::SetupForRtxFrom():

        // Use subresource 0 for hashing
        constexpr uint32_t subresource = 0;
        const auto& buffer = source->m_buffers[subresource];
        ...
        const bool useObsoleteHashMethod = NeedsUpload(subresource) &&
            RtxOptions::useObsoleteHashOnTextureUpload();
        XXH64_hash_t imageHash;
        if (unlikely(useObsoleteHashMethod))
            imageHash = XXH64(buffer->mapPtr(0), buffer->info().size, 0);
        else
            imageHash = XXH3_64bits(buffer->mapPtr(0), buffer->info().size);
        m_image->setHash(imageHash);

    So: one xxHash over the CPU staging buffer of **subresource 0 = layer 0, mip 0 only** (the
    buffer is `GetMipSize(0)` bytes: `align(elementSize * blockCount.width, 4) * blockCount.height`,
    i.e. mip 0 with rows aligned to 4 bytes — no mips, no format/size/flags mixed in, no name).
    Two variants, and this tool prints both:
      * XXH3_64bits(data)                       — the current method
      * XXH64(data, seed 0)                     — "obsolete" method, still the default
                                                 (rtx.useObsoleteHashOnTextureUpload = True in
                                                 src/util/config/config.cpp) for textures that
                                                 Remix had to upload (MANAGED/SYSTEMMEM pool).
    Which branch runs is decided by NeedsUpload(0), and m_needsUpload is still empty when
    CreateTexture sets the texture up (d3d9_common_texture.h:515), so for .txd textures GTA SA takes
    the XXH3 branch — measured, not assumed: XXH3 is what matches rtx.conf and the pack's
    mat_<HASH> prims, XXH64 never does (see the validation table in
    .strata/notes-s18-remix-texture-hash.md).

    When it happens: at the first FlushImage() of subresource 0 (d3d9_device.cpp:4912 ->
    SetupForRtx()), i.e. from the pixels the game last wrote into the staging buffer before the
    first upload, and SetupForRtxFrom() keeps that hash forever (it returns early if one is set).
    Textures the game *generates* instead of loading (the sea, the skybox, some UI) therefore hash
    over generated pixels, which no offline tool can know.

    The bundled xxHash is 0.8.0 (src/util/xxHash/xxhash.h, XXH_VERSION_MAJOR/MINOR/RELEASE =
    0/8/0); the implementations below are a transcription of that same file, cross-checked against a
    C build of it over 317 buffers (see the notes). Note XXH3's avalanche in 0.8.0 multiplies by
    0x165667919E3779F9 and xorshifts 37/32 - not by XXH_PRIME64_3 with 37/28, which is XXH64's.

usage:
  tools/remix_texhash.py TXD [NAME ...]        # one texture, or all of them if NAME is omitted
  tools/remix_texhash.py DIR_OR_IMG            # every .txd in a folder / inside a VER2 .img
  tools/remix_texhash.py DIR_OR_IMG --conf /media/rhy/data/Gamez/GTASA-RTX/rtx.conf --only-matches
                                             # just the textures whose hash is in one of rtx.conf's lists
  tools/remix_texhash.py TXD --hash 0x59296793798A7318   # which texture in this file has this hash?

A .txd path may also be a VER2 .img archive (models/gta3.img, ...) or a folder to scan.
Exit status: 0, or 1 if a named texture was not found / a file could not be parsed.
"""

import argparse
import os
import re
import struct
import sys

# --------------------------------------------------------------------------------------------
# xxHash 0.8.0 (the copy bundled in dxvk-remix 1.6.1), pure python, 64-bit variants only.
# Everything is masked to 64 bits; multiplication is mod 2**64 like the C code's uint64_t.
# --------------------------------------------------------------------------------------------

M64 = 0xFFFFFFFFFFFFFFFF

P64_1 = 0x9E3779B185EBCA87
P64_2 = 0xC2B2AE3D27D4EB4F
P64_3 = 0x165667B19E3779F9
P64_4 = 0x85EBCA77C2B2AE63
P64_5 = 0x27D4EB2F165667C5
P32_1 = 0x9E3779B1
P32_2 = 0x85EBCA77
P32_3 = 0xC2B2AE3D

# XXH3_kSecret[] from the bundled xxhash.h (XXH_SECRET_DEFAULT_SIZE = 192).
KSECRET = bytes([
    0xb8, 0xfe, 0x6c, 0x39, 0x23, 0xa4, 0x4b, 0xbe, 0x7c, 0x01, 0x81, 0x2c, 0xf7, 0x21, 0xad, 0x1c,
    0xde, 0xd4, 0x6d, 0xe9, 0x83, 0x90, 0x97, 0xdb, 0x72, 0x40, 0xa4, 0xa4, 0xb7, 0xb3, 0x67, 0x1f,
    0xcb, 0x79, 0xe6, 0x4e, 0xcc, 0xc0, 0xe5, 0x78, 0x82, 0x5a, 0xd0, 0x7d, 0xcc, 0xff, 0x72, 0x21,
    0xb8, 0x08, 0x46, 0x74, 0xf7, 0x43, 0x24, 0x8e, 0xe0, 0x35, 0x90, 0xe6, 0x81, 0x3a, 0x26, 0x4c,
    0x3c, 0x28, 0x52, 0xbb, 0x91, 0xc3, 0x00, 0xcb, 0x88, 0xd0, 0x65, 0x8b, 0x1b, 0x53, 0x2e, 0xa3,
    0x71, 0x64, 0x48, 0x97, 0xa2, 0x0d, 0xf9, 0x4e, 0x38, 0x19, 0xef, 0x46, 0xa9, 0xde, 0xac, 0xd8,
    0xa8, 0xfa, 0x76, 0x3f, 0xe3, 0x9c, 0x34, 0x3f, 0xf9, 0xdc, 0xbb, 0xc7, 0xc7, 0x0b, 0x4f, 0x1d,
    0x8a, 0x51, 0xe0, 0x4b, 0xcd, 0xb4, 0x59, 0x31, 0xc8, 0x9f, 0x7e, 0xc9, 0xd9, 0x78, 0x73, 0x64,
    0xea, 0xc5, 0xac, 0x83, 0x34, 0xd3, 0xeb, 0xc3, 0xc5, 0x81, 0xa0, 0xff, 0xfa, 0x13, 0x63, 0xeb,
    0x17, 0x0d, 0xdd, 0x51, 0xb7, 0xf0, 0xda, 0x49, 0xd3, 0x16, 0x55, 0x26, 0x29, 0xd4, 0x68, 0x9e,
    0x2b, 0x16, 0xbe, 0x58, 0x7d, 0x47, 0xa1, 0xfc, 0x8f, 0xf8, 0xb8, 0xd1, 0x7a, 0xd0, 0x31, 0xce,
    0x45, 0xcb, 0x3a, 0x8f, 0x95, 0x16, 0x04, 0x28, 0xaf, 0xd7, 0xfb, 0xca, 0xbb, 0x4b, 0x40, 0x7e,
])

STRIPE_LEN = 64
SECRET_CONSUME_RATE = 8
ACC_NB = 8
MIDSIZE_STARTOFFSET = 3
MIDSIZE_LASTOFFSET = 17
SECRET_SIZE_MIN = 136          # XXH3_SECRET_SIZE_MIN, used by the 129..240 path
SECRET_LASTACC_START = 7
SECRET_MERGEACCS_START = 11


def _rotl(x, r):
    return ((x << r) | (x >> (64 - r))) & M64


def _le64(b, i):
    return int.from_bytes(b[i:i + 8], "little")


def _le32(b, i):
    return int.from_bytes(b[i:i + 4], "little")


def _mul128_fold64(a, b):
    """XXH3_mul128_fold64: 64x64 -> 128, then lo ^ hi."""
    r = a * b
    return ((r & M64) ^ ((r >> 64) & M64)) & M64


def _avalanche64(x):
    """XXH3_avalanche (xxHash 0.8.0 as bundled by Remix): xorshift 37, * K, xorshift 32.

    Note the multiplier is 0x165667919E3779F9 - *not* XXH_PRIME64_3 (0x165667B19E3779F9),
    which is what the older docs/other implementations use. Getting this wrong changes every
    hash over 8 bytes, so it is worth the comment.
    """
    x ^= x >> 37
    x = (x * 0x165667919E3779F9) & M64
    x ^= x >> 32
    return x


def _rrmxmx(h, length):
    """XXH3_rrmxmx."""
    h ^= (_rotl(h, 49) ^ _rotl(h, 24)) & M64
    h = (h * 0x9FB21C651E98DF25) & M64
    h ^= ((h >> 35) + length) & M64
    h = (h * 0x9FB21C651E98DF25) & M64
    return (h ^ (h >> 28)) & M64


def _xxh64_round(acc, lane):
    """XXH64_round(acc, lane) = rotl(acc + mul(lane, PRIME64_2), 31) * PRIME64_1."""
    return (_rotl((acc + lane * P64_2) & M64, 31) * P64_1) & M64


def _xxh64_avalanche(h):
    """XXH64_avalanche."""
    h ^= h >> 33
    h = (h * P64_2) & M64
    h ^= h >> 29
    h = (h * P64_3) & M64
    h ^= h >> 32
    return h


def xxh64(data, seed=0):
    """XXH64(data, len, seed) — the "obsolete" Remix hash (seed 0)."""
    n = len(data)
    if n >= 32:
        acc1 = (seed + P64_1 + P64_2) & M64
        acc2 = (seed + P64_2) & M64
        acc3 = seed
        acc4 = (seed - P64_1) & M64
        i = 0
        while i + 32 <= n:
            acc1 = _xxh64_round(acc1, _le64(data, i))
            acc2 = _xxh64_round(acc2, _le64(data, i + 8))
            acc3 = _xxh64_round(acc3, _le64(data, i + 16))
            acc4 = _xxh64_round(acc4, _le64(data, i + 24))
            i += 32
        acc = (_rotl(acc1, 1) + _rotl(acc2, 7) + _rotl(acc3, 12) + _rotl(acc4, 18)) & M64
        for a in (acc1, acc2, acc3, acc4):
            # XXH64_mergeRound(acc, val): val = XXH64_round(0, val); acc ^= val;
            # acc = acc * PRIME64_1 + PRIME64_4
            acc = ((acc ^ _xxh64_round(0, a)) * P64_1 + P64_4) & M64
    else:
        acc = (seed + P64_5) & M64
        i = 0
    acc = (acc + n) & M64
    while i + 8 <= n:
        acc = (_rotl(acc ^ _xxh64_round(0, _le64(data, i)), 27) * P64_1 + P64_4) & M64
        i += 8
    if i + 4 <= n:
        # XXH_PROCESS4_64: h64 ^= (u64)get32 * PRIME64_1; rotl 23, * PRIME64_2 + PRIME64_3
        acc = (_rotl((acc ^ ((_le32(data, i) * P64_1) & M64)) & M64, 23) * P64_2 + P64_3) & M64
        i += 4
    while i < n:
        # XXH_PROCESS1_64: h64 ^= byte * PRIME64_5; rotl 11, * PRIME64_1
        acc = (_rotl((acc ^ (data[i] * P64_5)) & M64, 11) * P64_1) & M64
        i += 1
    return _xxh64_avalanche(acc)


def _mix16b(data, ioff, secret, soff):
    lo = _le64(data, ioff)
    hi = _le64(data, ioff + 8)
    return _mul128_fold64((lo ^ _le64(secret, soff)) & M64, (hi ^ _le64(secret, soff + 8)) & M64)


def _len_0to16(data, n, secret):
    if n > 8:                                    # XXH3_len_9to16_64b
        bf1 = (_le64(secret, 24) ^ _le64(secret, 32)) & M64
        bf2 = (_le64(secret, 40) ^ _le64(secret, 48)) & M64
        lo = (_le64(data, 0) ^ bf1) & M64
        hi = (_le64(data, n - 8) ^ bf2) & M64
        acc = (n + int.from_bytes(lo.to_bytes(8, "little"), "big") + hi + _mul128_fold64(lo, hi)) & M64
        return _avalanche64(acc)
    if n >= 4:                                   # XXH3_len_4to8_64b
        in1 = _le32(data, 0)
        in2 = _le32(data, n - 4)
        bf = (_le64(secret, 8) ^ _le64(secret, 16)) & M64
        in64 = (in2 + (in1 << 32)) & M64
        return _rrmxmx(in64 ^ bf, n)
    if n:                                        # XXH3_len_1to3_64b
        c1, c2, c3 = data[0], data[n >> 1], data[n - 1]
        combined = ((c1 << 16) | (c2 << 24) | c3 | (n << 8)) & 0xFFFFFFFF
        bf = (_le32(secret, 0) ^ _le32(secret, 4)) & M64
        return _xxh64_avalanche(combined ^ bf)
    return _xxh64_avalanche(_le64(secret, 56) ^ _le64(secret, 64))


def _len_17to128(data, n, secret):
    acc = (n * P64_1) & M64
    if n > 32:
        if n > 64:
            if n > 96:
                acc = (acc + _mix16b(data, 48, secret, 96)) & M64
                acc = (acc + _mix16b(data, n - 64, secret, 112)) & M64
            acc = (acc + _mix16b(data, 32, secret, 64)) & M64
            acc = (acc + _mix16b(data, n - 48, secret, 80)) & M64
        acc = (acc + _mix16b(data, 16, secret, 32)) & M64
        acc = (acc + _mix16b(data, n - 32, secret, 48)) & M64
    acc = (acc + _mix16b(data, 0, secret, 0)) & M64
    acc = (acc + _mix16b(data, n - 16, secret, 16)) & M64
    return _avalanche64(acc)


def _len_129to240(data, n, secret):
    acc = (n * P64_1) & M64
    rounds = n // 16
    for i in range(8):
        acc = (acc + _mix16b(data, 16 * i, secret, 16 * i)) & M64
    acc = _avalanche64(acc)
    for i in range(8, rounds):
        acc = (acc + _mix16b(data, 16 * i, secret, 16 * (i - 8) + MIDSIZE_STARTOFFSET)) & M64
    acc = (acc + _mix16b(data, n - 16, secret, SECRET_SIZE_MIN - MIDSIZE_LASTOFFSET)) & M64
    return _avalanche64(acc)


def _accumulate_512(acc, data, doff, secret, soff):
    for i in range(ACC_NB):
        data_val = _le64(data, doff + 8 * i)
        data_key = (data_val ^ _le64(secret, soff + 8 * i)) & M64
        acc[i ^ 1] = (acc[i ^ 1] + data_val) & M64          # swap adjacent lanes
        acc[i] = (acc[i] + ((data_key & 0xFFFFFFFF) * (data_key >> 32))) & M64


def _scramble_acc(acc, secret, soff):
    for i in range(ACC_NB):
        key64 = _le64(secret, soff + 8 * i)
        a = acc[i] ^ (acc[i] >> 47)
        a = ((a ^ key64) * P32_1) & M64
        acc[i] = a


def _hash_long(data, n, secret):
    acc = [P32_3, P64_1, P64_2, P64_3, P64_4, P32_2, P64_5, P32_1]
    secret_size = len(secret)
    stripes_per_block = (secret_size - STRIPE_LEN) // SECRET_CONSUME_RATE
    block_len = STRIPE_LEN * stripes_per_block
    nb_blocks = (n - 1) // block_len
    for b in range(nb_blocks):
        for s in range(stripes_per_block):
            _accumulate_512(acc, data, b * block_len + s * STRIPE_LEN, secret, s * SECRET_CONSUME_RATE)
        _scramble_acc(acc, secret, secret_size - STRIPE_LEN)
    nb_stripes = ((n - 1) - (block_len * nb_blocks)) // STRIPE_LEN
    for s in range(nb_stripes):
        _accumulate_512(acc, data, nb_blocks * block_len + s * STRIPE_LEN, secret, s * SECRET_CONSUME_RATE)
    _accumulate_512(acc, data, n - STRIPE_LEN, secret, secret_size - STRIPE_LEN - SECRET_LASTACC_START)
    # XXH3_mergeAccs
    result = (n * P64_1) & M64
    for i in range(4):
        a0 = (acc[2 * i] ^ _le64(secret, SECRET_MERGEACCS_START + 16 * i)) & M64
        a1 = (acc[2 * i + 1] ^ _le64(secret, SECRET_MERGEACCS_START + 16 * i + 8)) & M64
        result = (result + _mul128_fold64(a0, a1)) & M64
    return _avalanche64(result)


def xxh3_64bits(data):
    """XXH3_64bits(data, len) with the default secret and seed 0."""
    n = len(data)
    if n <= 16:
        return _len_0to16(data, n, KSECRET)
    if n <= 128:
        return _len_17to128(data, n, KSECRET)
    if n <= 240:
        return _len_129to240(data, n, KSECRET)
    return _hash_long(data, n, KSECRET)


# --------------------------------------------------------------------------------------------
# GTA SA texture dictionaries (RenderWare texture dictionary chunks)
# --------------------------------------------------------------------------------------------
#
# Measured on this game's files (see .strata/notes-s18-remix-texture-hash.md): a chunk header is
# (u32 type, u32 length[, u32 version 0x1803ffff / 0x1003ffff]); `length` counts the body that
# follows the header. The name chunk is the exception: its header is only (u32 type, u32 length).
#
#   0x16 TextureDictionary
#        0x01  u32 texture count
#        0x15  Texture (one chunk per texture)
#              0x01  native data
#                    0x09 (0x08 in older dictionaries)  name[32] + 32 bytes + raster header + raster
#
# Inside that last chunk, measured byte by byte on particle.txd / vehicle.txd / fonts.txd:
#   name_start + 0   char name[32]
#   name_start + 32  32 bytes; for block-compressed rasters the fourcc "DXT1"/"DXT3"/"DXT5" is in here
#   name_start + 72  u16 width, u16 height
#   name_start + 76  u8 depth (32 or 16), u8 1, u8 4, u8 flags
#   name_start + 80  u32 raster size (absent in the older 0x1003ffff dictionaries)
#   name_start + 84  the raster itself
# SA's dictionaries store **mip 0 only**, which is convenient: mip 0 is exactly what Remix hashes.
# The size word is what to trust, not depth: measured over all 32949 textures of this game the
# raster is width*height*4 for the 1360 depth-32 textures but width*height for the 31588 depth-16
# ones, and hashing the stored bytes reproduces the game's hash in both cases. No block-compressed
# texture exists in the vanilla game; the DXT branches are there for ModLoader packs that ship them.

CH_TEXDICT = 0x16
CH_TEX = 0x15
CH_NATIVE = 0x01
CH_NAMES = (0x08, 0x09)

# RW/D3D raster fourcc -> (block width, block height, bytes per block)
BLOCK_FMTS = {b"DXT1": (4, 4, 8), b"DXT2": (4, 4, 8), b"DXT3": (4, 4, 16),
              b"DXT4": (4, 4, 16), b"DXT5": (4, 4, 16)}


class Tex:
    """One texture of a .txd: its name, size, format and the mip-0 bytes in the file.

    `note` is empty for a raster the game hands to D3D9 unchanged; otherwise it says why the
    hash cannot be taken from the .txd bytes (the game converts such rasters on upload).
    """

    def __init__(self, name, w, h, fourcc, depth, off, size, note=""):
        self.name = name
        self.w = w
        self.h = h
        self.fourcc = fourcc          # b"DXT1"/b"DXT5"/... or b"" for uncompressed
        self.depth = depth            # bits per pixel as stored (32, 16, ...)
        self.size = size              # raster bytes in the file (mip 0)
        self.off = off
        self.note = note

    @property
    def end(self):
        return self.off + self.size

    def format_name(self):
        if self.fourcc:
            return self.fourcc.decode("latin1")
        if self.depth == 32:
            return "A8R8G8B8"
        return "%dbit" % self.depth

    def d3d9_mip0(self, data):
        """The bytes of mip 0 exactly as the D3D9 device receives them.

        Remix hashes `m_buffers[0]`, whose size is GetMipSize(0) =
        align(elementSize * blockCount.width, 4) * blockCount.height: mip 0 with every row
        aligned to 4 bytes. For the sizes SA uses that is byte-identical to the raster in the
        .txd (DXT data is already whole 4x4 blocks, and SA's widths are multiples of 4), so
        normally this is just data[off:off+size]; rows that are not 4-byte aligned get padded.
        """
        raw = data[self.off:self.off + self.size]
        if self.fourcc:
            bw, bh, nblk = BLOCK_FMTS[self.fourcc]
            row = (self.w + bw - 1) // bw * nblk
        else:
            # bytes per pixel as stored, which the depth byte does not always give (see _parse_tex)
            n = self.w * self.h
            bpp = self.size // n if n and self.size % n == 0 else max(1, self.depth // 8)
            row = self.w * bpp
        aligned = (row + 3) // 4 * 4
        if aligned == row or not raw:
            return raw
        out = bytearray()
        for y in range(self.h):
            out += raw[y * row:(y + 1) * row]
            out += b"\0" * (aligned - row)
        return bytes(out)


def parse_txd(data, where="?"):
    """[Tex] for the bytes of one .txd file. Raises ValueError if it is not one."""
    if len(data) < 12:
        raise ValueError("%s: too small for a texture dictionary" % where)
    typ, length, ver = struct.unpack_from("<III", data, 0)
    if typ != CH_TEXDICT:
        raise ValueError("%s: top chunk is %#x, not a texture dictionary (%#x)" % (where, typ, CH_TEXDICT))
    out = []
    off, end = 12, min(len(data), 12 + length)
    while off + 12 <= end:
        typ, length, ver = struct.unpack_from("<III", data, off)
        body, body_end = off + 12, off + 12 + length
        if body_end > end:
            break
        if typ == CH_TEX:
            out.extend(_parse_tex(data, body, body_end, where))
        off = body_end
    return out


def _parse_tex(data, body, body_end, where):
    """[Tex] inside one 0x15 chunk: a 0x01 native-data chunk holding name + raster."""
    typ, length, ver = struct.unpack_from("<III", data, body)
    if typ != CH_NATIVE:
        raise ValueError("%s: texture chunk starts with %#x, expected native data %#x" % (where, typ, CH_NATIVE))
    nb = body + 12 + 8                    # name chunk header is (type, length): no version word
    t_name, _l = struct.unpack_from("<II", data, nb - 8)
    if t_name not in CH_NAMES:
        raise ValueError("%s: expected a name chunk (%s) at %#x, found %#x" %
                         (where, "/".join(hex(x) for x in CH_NAMES), nb - 8, t_name))
    name = data[nb:nb + 32].split(b"\0")[0].decode("latin1", "replace")
    if nb + 84 > body_end:
        raise ValueError("%s: %s: name chunk does not fit its parent" % (where, name))
    pre = data[nb + 32:nb + 64]           # the 32 bytes before the raster header: fourcc for DXT
    fourcc = b""
    for f in BLOCK_FMTS:
        if f in pre:
            fourcc = f
            break
    w, h = struct.unpack_from("<HH", data, nb + 72)
    depth = data[nb + 76]
    size_field = struct.unpack_from("<I", data, nb + 80)[0]
    # The size word is authoritative: hashing exactly those bytes reproduces the hashes of rtx.conf
    # for all 32948 textures of this game. Note it is *not* always width*height*depth/8 - the 31588
    # "16 bit" rasters (depth byte 16) are stored as width*height bytes and that is what Remix
    # hashes - so never "correct" it from the depth byte.
    if 0 < size_field <= body_end - (nb + 84):
        return [Tex(name, w, h, fourcc, depth, nb + 84, size_field)]
    # Older dictionaries (chunk version 0x1003ffff, e.g. models/txd/outro.txd) have no size word:
    # the u32 there is already pixel data. Anything we would conclude about the extent of that
    # raster is a guess about what the game uploads, so refuse instead of inventing a hash.
    return [Tex(name, w, h, fourcc, depth, nb + 84, 0,
                note="its raster header has no usable size word (the field reads %d for a %dx%d %s "
                     "raster): an older/odd dictionary, refusing to guess what the game uploads"
                     % (size_field, w, h, "palettised" if depth == 8 else "%dbit" % depth))]


def iter_txd_bytes(path):
    """(label, bytes) for every texture dictionary at `path`: a .txd, a VER2 .img, or a folder."""
    if os.path.isdir(path):
        for root, _dirs, files in os.walk(path):
            for fn in sorted(files):
                if fn.lower().endswith(".txd"):
                    f = os.path.join(root, fn)
                    yield (f, open(f, "rb").read())
        return
    data = open(path, "rb").read()
    if data[:4] == b"VER2":
        n = struct.unpack_from("<I", data, 4)[0]
        for i in range(n):
            off, stream_sz, arch_sz, raw = struct.unpack_from("<IHH24s", data, 8 + 32 * i)
            name = raw.split(b"\0", 1)[0].decode("latin1")
            if not name.lower().endswith(".txd"):
                continue
            size = (arch_sz or stream_sz) * 2048
            blob = data[off * 2048:off * 2048 + size]
            if len(blob) < size:
                raise ValueError("%s: %s: short read" % (path, name))
            yield ("%s:%s" % (path, name), blob)
    else:
        yield (path, data)


# --------------------------------------------------------------------------------------------
# rtx.conf hash lists, to check our result against something the game really produced
# --------------------------------------------------------------------------------------------

CONF_LIST_RE = re.compile(r"^\s*(rtx\.[A-Za-z0-9_.]+)\s*=\s*(.*)$")


def read_conf(path):
    """{HASH(upper hex, 16): [rtx.conf key, ...]}.

    Remix writes every list on one line; a hand-edited file may wrap it after a comma, so a value
    ending in ',' keeps reading (only then - an empty value must not swallow the next key).
    """
    out = {}
    with open(path, encoding="utf-8", errors="replace") as f:
        lines = f.read().splitlines()
    i = 0
    while i < len(lines):
        m = CONF_LIST_RE.match(lines[i])
        i += 1
        if not m:
            continue
        key, value = m.group(1), m.group(2)
        while value.rstrip().endswith(",") and i < len(lines):
            value += " " + lines[i]
            i += 1
        for tok in re.split(r"[,\s]+", value):
            t = tok.strip().lower().replace("0x", "")
            if re.fullmatch(r"[0-9a-f]{16}", t):
                out.setdefault(t.upper(), []).append(key)
    return out


# --------------------------------------------------------------------------------------------

def fmt_tex(t):
    return "%-24s %4dx%-4d %-8s mip0=%d bytes" % (t.name, t.w, t.h, t.format_name(), t.size)


def main(argv=None):
    ap = argparse.ArgumentParser(
        prog="remix_texhash.py", description=__doc__.splitlines()[0],
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__.split("usage:")[1].split("\n\n")[0] if "usage:" in __doc__ else "")
    ap.add_argument("path", help=".txd file, a folder of them, or a VER2 .img archive")
    ap.add_argument("names", nargs="*", help="texture name(s); default: every texture")
    ap.add_argument("--conf", help="an rtx.conf to compare the hashes against (marks matches)")
    ap.add_argument("--hash", action="append", default=[],
                    help="also report whether this 0x... hash is produced here (repeatable)")
    ap.add_argument("--only-matches", action="store_true",
                    help="print only textures whose hash matches --conf/--hash (what makes a full "
                         "scan of models/ readable)")
    ap.add_argument("--quiet", action="store_true", help="one line per texture, no format detail")
    args = ap.parse_args(argv)

    want = {}
    if args.conf:
        want.update(read_conf(args.conf))
    for h in args.hash:
        t = h.strip().lower().replace("0x", "")
        if not re.fullmatch(r"[0-9a-f]{16}", t):
            raise SystemExit("--hash %r is not 16 hex digits" % h)
        want.setdefault(t.upper(), ["--hash"])
    wanted_names = {n.lower() for n in args.names}

    shown = matched = skipped = 0
    failed = False
    seen_names = set()
    for label, blob in iter_txd_bytes(args.path):
        try:
            texes = parse_txd(blob, label)
        except ValueError as e:
            print("skip: %s" % e)
            failed = True
            continue
        for t in texes:
            seen_names.add(t.name.lower())
            if wanted_names and t.name.lower() not in wanted_names:
                continue
            if t.note:
                skipped += 1
                if not args.only_matches:
                    print("%s  cannot hash offline: %s" % (fmt_tex(t), t.note))
                continue
            mip0 = t.d3d9_mip0(blob)
            h3 = "%016X" % xxh3_64bits(mip0)
            h6 = "%016X" % xxh64(mip0, 0)
            hits = sorted(set(want.get(h3, []) + want.get(h6, [])))
            if hits:
                matched += 1
            if args.only_matches and not hits:
                continue
            tag = ("   <== " + ", ".join(hits)) if hits else ""
            if args.quiet:
                print("%s %s %s%s" % (h3, h6, t.name, tag))
            else:
                print("%s  xxh3=%s  xxh64=%s%s" % (fmt_tex(t), h3, h6, tag))
            shown += 1
    missing = sorted(wanted_names - seen_names)
    if missing:
        print("not found: %s" % ", ".join(missing))
        failed = True
    print("%d texture(s) shown, %d match the %d hash(es) given%s"
          % (shown, matched, len(want), (", %d skipped (raster not hashed)" % skipped) if skipped else ""))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
