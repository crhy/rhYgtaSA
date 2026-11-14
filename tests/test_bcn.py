#!/usr/bin/env python3
"""n20 - the BCn encoders in tools/bcn.py, checked against the published block layouts.

No game folder, no GPU: every case here is a hand-built block or a synthetic image. The
cross-decodes against Pillow and ffmpeg live in scripts/textures-selftest.sh (they need the
game's own .txd data for the interesting case); this file pins the arithmetic that made those
cross-checks pass, because every one of them was found by a mismatch:

  * the RGTC alpha ladder has denominator 7 (EXT_texture_compression_rgtc), not 8;
  * BC1's four-opaque mode is selected by comparing the PACKED endpoint words;
  * blocks()/unblocks() must transpose (block row, y in block) with (block col), or the image
    comes out scrambled in 4x4 tiles while encode and decode still agree with each other.
"""
import os
import struct
import sys

import numpy as np
import pytest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools"))
import bcn  # noqa: E402


def rmse(a, b):
    return float(((np.asarray(a, float) - np.asarray(b, float)) ** 2).mean() ** 0.5)


# ------------------------------------------------------------------ the alpha ladder

def test_rgtc_eight_value_ladder_is_over_seven():
    """(6a0+a1)/7 ... from the RGTC appendix; an /8 ladder was the original bug."""
    t = bcn._alpha_table(np.array([255]), np.array([0]))[0]
    assert list(t) == [255, 0, 218, 182, 145, 109, 72, 36]


def test_rgtc_six_value_ladder_when_a0_le_a1():
    t = bcn._alpha_table(np.array([0]), np.array([255]))[0]
    assert list(t) == [0, 255, 51, 102, 153, 204, 0, 255]
    t = bcn._alpha_table(np.array([100]), np.array([100]))[0]
    assert list(t)[:2] == [100, 100]


def test_alpha_table_endpoints_are_exact():
    """With a0 > a1 the ladder runs from a0 down to a1 and never leaves that range."""
    a0 = np.arange(16, 256, 16, dtype=np.uint8)
    a1 = np.arange(0, 240, 16, dtype=np.uint8)
    t = bcn._alpha_table(a0, a1)
    assert np.array_equal(t[:, 0], a0) and np.array_equal(t[:, 1], a1)
    assert (t[:, 2] > t[:, 3]).all() and (t[:, 3] > t[:, 7]).all()
    assert (t <= np.maximum(a0[:, None], a1[:, None])).all()
    assert (t >= np.minimum(a0[:, None], a1[:, None])).all()


# ------------------------------------------------------------------ BC1 colour blocks

def test_bc1_block_decodes_to_the_spec_table():
    """c0=0xF800, c1=0x001F, selectors 0,1,2,3 -> the four published colours."""
    sel = 0
    for i, v in enumerate([0, 1, 2, 3] * 4):
        sel |= v << (2 * i)
    blk = struct.pack("<HHI", 0xF800, 0x001F, sel)
    out = bcn._decode_color(blk, 4, 4, offset=0, stride=8)
    # 565 endpoints expand by replicating the top bits (31 -> 255), the Direct3D convention
    assert out[0, :4].tolist() == [[255, 0, 0], [0, 0, 255], [170, 0, 85], [85, 0, 170]]


def test_bc1_three_colour_mode_makes_index_three_transparent():
    """c0 <= c1 packed: index 3 is black with alpha 0 - SA's DXT1 punch-through alpha."""
    sel = 0
    for i, v in enumerate([3, 3, 3, 3] * 4):
        sel |= v << (2 * i)
    blk = struct.pack("<HHI", 0x001F, 0xF800, sel)
    rgba = bcn.decode_dxt1(blk, 4, 4)
    assert (rgba[:, :, 3] == 0).all()
    assert (rgba[:, :, :3] == 0).all()


def test_bc1_mode_is_chosen_on_the_packed_words_not_the_rgb_sum():
    """Green has the biggest RGB sum but the smallest packed word, so it must go second."""
    c0 = np.array([[0, 255, 0]], np.int32)
    c1 = np.array([[255, 0, 0]], np.int32)
    q0, q1 = bcn._order(c0, c1)
    assert bcn._pack565(q0)[0] > bcn._pack565(q1)[0]
    assert q0[0, 0] == 255                      # red first, even though green is "brighter"


# ------------------------------------------------------------------ block order

def test_blocks_unblocks_inverse_is_exact_on_a_non_square_image():
    a = np.arange(8 * 12 * 3, dtype="uint8").reshape(8, 12, 3)
    assert np.array_equal(bcn.unblocks(bcn.blocks(a), 8, 12, 3), a)


def test_blocks_are_row_major_in_4x4_tiles():
    """The first stream block must be the top-left 4x4, the 5th the one below it."""
    a = np.zeros((8, 8, 1), np.uint8)
    a[0, 0, 0] = 7
    a[4, 0, 0] = 9
    b = bcn.blocks(a)
    assert b[0, 0, 0] == 7 and b[2, 0, 0] == 9       # blocks: (0,0), (0,1), (1,0), (1,1)


def test_blocks_reject_a_size_that_is_not_a_multiple_of_four():
    with pytest.raises(ValueError):
        bcn.blocks(np.zeros((6, 6, 3), np.uint8))


# ------------------------------------------------------------------ round trips

def test_bc3_round_trip_is_close_and_keeps_the_alpha():
    rng = np.random.default_rng(7)
    h = w = 32
    g = np.clip(np.linspace(0, 255, w)[None, :] + rng.normal(0, 8, (h, w)), 0, 255).astype("uint8")
    al = np.clip(np.linspace(0, 255, h)[:, None] + rng.normal(0, 8, (h, w)), 0, 255).astype("uint8")
    rgba = np.dstack([g, 255 - g, np.roll(g, 5, 1), al]).astype("uint8")
    enc = bcn.encode_bc3(rgba, mips=False)
    assert len(enc) == (h // 4) * (w // 4) * 16
    dec = bcn.decode_bc3(enc, w, h)
    assert rmse(dec[:, :, :3], rgba[:, :, :3]) < 12
    assert rmse(dec[:, :, 3], al) < 3


def test_bc4_and_bc5_round_trips():
    rng = np.random.default_rng(9)
    h = w = 32
    g = np.clip(np.linspace(0, 255, w)[None, :] + rng.normal(0, 8, (h, w)), 0, 255).astype("uint8")
    d4 = bcn.decode_bc4(bcn.encode_bc4(g, mips=False), w, h)
    assert rmse(d4, g) < 4
    rg = np.dstack([g, np.roll(g, 3, 0)])
    d5 = bcn.decode_bc5(bcn.encode_bc5(rg, mips=False), w, h)
    assert rmse(d5, rg) < 4


def test_a_flat_normal_map_encodes_to_128():
    flat = np.full((16, 16, 2), 128, np.uint8)
    dec = bcn.decode_bc5(bcn.encode_bc5(flat, mips=False), 16, 16)
    assert np.allclose(dec, 128)


def test_mip_chain_length_matches_the_file_size():
    h = w = 64
    g = np.full((h, w), 40, np.uint8)
    enc = bcn.encode_bc4(g, mips=True)
    assert len(enc) == bcn.mip_bytes(w, h, 8)
    assert bcn.mip_sizes(w, h)[-1] == (1, 1)


# ------------------------------------------------------------------ the DDS container

def test_write_then_read_dx10_header(tmp_path):
    w = h = 16
    blob = bcn.write_dds("-", bcn.encode_bc5(np.zeros((h, w, 2), np.uint8), mips=True),
                         w, h, bcn.DXGI["BC5_UNORM"], mips=len(bcn.mip_sizes(w, h)))
    assert blob[:4] == b"DDS "
    assert struct.unpack_from("<I", blob, 4)[0] == 124
    assert struct.unpack_from("<I", blob, 80)[0] == 0x4 | 0x2000      # FOURCC | DX10
    assert blob[84:88] == b"DX10"
    assert struct.unpack_from("<I", blob, 128)[0] == bcn.DXGI["BC5_UNORM"]
    assert struct.unpack_from("<I", blob, 20)[0] == ((w + 3) // 4) * ((h + 3) // 4) * 16
    path = str(tmp_path / "header.dds")
    with open(path, "wb") as fh:
        fh.write(blob)
    hdr, levels = bcn.read_dds(path)
    assert hdr["dxgi"] == bcn.DXGI["BC5_UNORM"] and hdr["dxgi_name"] == "BC5_UNORM"
    assert hdr["mips"] == len(levels) == len(bcn.mip_sizes(w, h))
    assert [lw for lw, lh, _ in levels] == [mw for mw, mh in bcn.mip_sizes(w, h)]


def test_legacy_fourcc_header_is_read_as_legacy():
    w = h = 8
    blob = bcn.write_dds("-", bcn.encode_bc3(np.zeros((h, w, 4), np.uint8), mips=False),
                         w, h, bcn.DXGI["BC3_UNORM"], mips=1, fourcc_legacy=b"DXT5")
    assert blob[84:88] == b"DXT5"
    assert struct.unpack_from("<I", blob, 80)[0] == 0x4
    assert len(blob) == 128 + (w // 4) * (h // 4) * 16


def test_read_dds_understands_the_packs_flagless_dx10_files(tmp_path):
    """The installed pack sets only DDPF_FOURCC (0x4) on its DX10 files, not DDPF_DX10."""
    w = h = 8
    blob = bytearray(bcn.write_dds("-", bcn.encode_bc4(np.zeros((h, w), np.uint8), mips=False),
                                   w, h, bcn.DXGI["BC4_UNORM"], mips=1))
    struct.pack_into("<I", blob, 80, 0x4)                 # exactly what the pack does
    path = str(tmp_path / "packlike.dds")
    with open(path, "wb") as fh:
        fh.write(bytes(blob))
    hdr, levels = bcn.read_dds(path)
    assert hdr["fourcc"] == b"DX10" and hdr["dxgi"] == bcn.DXGI["BC4_UNORM"]
    assert len(levels) == 1 and len(levels[0][2]) == (8 // 4) * (8 // 4) * 8
