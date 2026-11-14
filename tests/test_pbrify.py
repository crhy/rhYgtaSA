#!/usr/bin/env python3
"""n20 - the map derivation in tools/pbrify.py, on synthetic rasters (no game folder).

What is being pinned here is the part that is a design decision rather than a format: the
albedo keeps the game's own colour and markings, the height field comes from the raster's own
detail, the normal map stays near flat with a class-scaled spread, and the roughness stays
inside the class's calibrated range. The DDS writing itself is tests/test_bcn.py.
"""
import os
import sys
import types

import numpy as np
import pytest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools"))
import bcn        # noqa: E402
import pbrify     # noqa: E402
from texture_rank import CLASSES    # noqa: E402


def fake_tex(w, h, raster, fourcc=None, depth=0):
    """A remix_texhash.Tex-shaped object plus the 20-byte raster header pbrify reads."""
    head = bytearray(20)
    head[0:4] = b"\x00\x00\x00\x00"
    head[4:8] = fourcc or b"\x00\x00\x00\x00"
    head[8:12] = np.uint16([w, h]).tobytes()
    head[12] = depth
    head[16:20] = np.uint32(len(raster)).tobytes()
    data = bytes(head) + bytes(raster)
    tex = types.SimpleNamespace(name="synthetic", off=len(head), size=len(raster), w=w, h=h,
                                depth=depth, fourcc=fourcc)
    return data, tex


# --------------------------------------------------------------------- source decoding

def test_bgra_raster_decodes_to_rgba():
    px = np.zeros((4, 4, 4), np.uint8)
    px[:, :, 0], px[:, :, 1], px[:, :, 2], px[:, :, 3] = 10, 20, 30, 255      # B,G,R,A
    data, tex = fake_tex(4, 4, px.tobytes())
    out = pbrify.decode_rgba(data, tex)
    assert out.shape == (4, 4, 4)
    assert out[0, 0].tolist() == [30, 20, 10, 255]


def test_depth16_raster_decodes_as_1555_and_stays_opaque():
    a = np.zeros((4, 4), np.uint16)
    a[:, :] = (31 << 10) | (63 << 5) | 31                      # white, alpha bit clear
    data, tex = fake_tex(4, 4, a.tobytes(), depth=16)
    out = pbrify.decode_rgba(data, tex)
    assert out[0, 0][:3].tolist() == [255, 255, 255]
    assert (out[:, :, 3] == 255).all()                        # depth-16 with no alpha channel


def test_a_block_format_we_do_not_decode_is_refused_not_guessed():
    data, tex = fake_tex(8, 8, b"\0" * 64, fourcc=b"DX10")
    with pytest.raises(ValueError):
        pbrify.decode_rgba(data, tex)


def test_a_palettised_raster_is_refused():
    data, tex = fake_tex(8, 8, b"\0" * 64, depth=8)
    with pytest.raises(ValueError):
        pbrify.decode_rgba(data, tex)


# --------------------------------------------------------------------- map derivation

def test_upscale_keeps_the_mean_colour():
    src = np.full((32, 32, 4), 137, np.uint8)
    src[:, :, 3] = 255
    big = pbrify.upscale(src, 128)
    assert big.shape == (128, 128, 4)
    assert abs(float(big[:, :, 0].mean()) - 137.0) < 1.0


def test_target_size_is_a_power_of_two_inside_the_bounds():
    assert pbrify.target_size(256, 256, 4, 2048, 256) == 1024
    assert pbrify.target_size(512, 512, 4, 2048, 256) == 2048      # clamped
    assert pbrify.target_size(64, 64, 4, 2048, 256) == 256         # floored
    assert pbrify.target_size(512, 256, 4, 2048, 256) == 2048      # longest edge drives it


def test_height_field_is_normalised_and_follows_detail():
    flat = np.full((64, 64), 128.0)
    assert np.allclose(pbrify.derive_height(flat, CLASSES["asphalt"]), 0.5)
    # a coarse pattern survives the low-pass split, so it becomes relief
    noisy = np.clip(128 + 40 * np.sin(np.linspace(0, 6 * np.pi, 64))[None, :], 0, 255)
    h = pbrify.derive_height(noisy, CLASSES["asphalt"])
    assert 0.0 <= h.min() and h.max() <= 1.0
    assert h.max() - h.min() > 0.5
    assert abs(float(h.mean()) - 0.5) < 0.15


def test_normal_map_is_near_flat_and_scales_with_the_class():
    h = np.full((64, 64), 0.5)
    n = pbrify.derive_normal(h, 2.0)
    assert np.all(n == 128)
    rng = np.random.default_rng(5)
    h = np.clip(0.5 + 0.2 * rng.normal(size=(64, 64)), 0, 1)
    soft = pbrify.derive_normal(h, 0.05)
    hard = pbrify.derive_normal(h, 2.3)
    assert abs(float(soft[:, :, 0].mean()) - 128) < 3
    assert abs(float(hard[:, :, 0].mean()) - 128) < 6
    assert soft[:, :, 0].std() < hard[:, :, 0].std()
    assert hard[:, :, 0].std() < 60


def test_roughness_stays_inside_the_class_range_and_tracks_detail():
    rng = np.random.default_rng(6)
    h = np.clip(0.5 + 0.25 * rng.normal(size=(64, 64)), 0, 1)
    lum = np.full((64, 64), 120.0)
    for cls in CLASSES:
        r = pbrify.derive_rough(lum, h, CLASSES[cls])
        c = CLASSES[cls]
        assert r.min() >= c["rough"] - c["rough_var"] - 1e-6, cls
        assert r.max() <= c["rough"] + c["rough_var"] + 1e-6, cls
        assert 0.02 <= r.min() and r.max() <= 1.0, cls


def test_build_set_produces_four_maps_at_the_target_size():
    src = np.dstack([np.full((64, 64), 90, np.uint8), np.full((64, 64), 95, np.uint8),
                     np.full((64, 64), 100, np.uint8), np.full((64, 64), 255, np.uint8)])
    src[:, :, 0] = (90 + 30 * np.sin(np.linspace(0, 30 * np.pi, 64))[None, :]).astype("uint8")
    albedo, normal, rough, hmap = pbrify.build_set(src, CLASSES["asphalt"], 4, 2048, 256)
    assert albedo.shape == (256, 256, 4)
    assert normal.shape == (256, 256, 2) and rough.shape == (256, 256) and hmap.shape == (256, 256)
    assert abs(float(albedo[:, :, 0].mean()) - float(src[:, :, 0].mean())) < 3.0


def test_write_set_writes_four_bc_files_with_mips(tmp_path):
    albedo = np.full((32, 32, 4), 120, np.uint8)
    albedo[:, :, 3] = 255
    normal = np.full((32, 32, 2), 128, np.uint8)
    rough = np.full((32, 32), 200, np.uint8)
    hmap = np.full((32, 32), 128, np.uint8)
    sizes = pbrify.write_set(str(tmp_path), "ABCDEF0123456789", albedo, normal, rough, hmap)
    assert set(sizes) == {"albedo", "normal", "rough", "height"}
    for suffix, dxgi in (("albedo", None), ("normal", 83), ("rough", 80), ("height", 80)):
        hdr, levels = bcn.read_dds(str(tmp_path / ("ABCDEF0123456789_%s.dds" % suffix)))
        assert hdr["w"] == hdr["h"] == 32
        assert hdr["mips"] == len(levels) == len(bcn.mip_sizes(32, 32))
        if dxgi is not None:
            assert hdr["dxgi"] == dxgi
        else:
            assert hdr["fourcc"] == b"DXT5"


# --------------------------------------------------------------------- the USD layer

def test_render_usda_overrides_every_material_with_the_right_shader():
    entries = [
        dict(name="road", hash="AA", **{"class": "asphalt"}, w=256, h=256, size=1024),
        dict(name="glass", hash="BB", **{"class": "glass"}, w=128, h=128, size=512),
    ]
    text = pbrify.render_usda(entries, "texture/rhy_pbr")
    assert text.startswith("#usda 1.0")
    assert text.count('over "mat_') == 2
    assert "AperturePBR_Translucent" in text and "AperturePBR_Opaque" in text
    for h in ("AA", "BB"):
        for suf in ("albedo", "normal", "rough", "height"):
            assert "@./texture/rhy_pbr/%s_%s.dds@" % (h, suf) in text
    assert "inputs:metallic_constant = 0.0" in text
    assert "inputs:reflection_roughness_constant = 0.59" in text      # asphalt, pack-calibrated
    assert "inputs:displace_in = 0.002" in text                       # asphalt has relief
    stack = []
    for c in text.replace('"', "").replace("@", ""):
        if c in "([{":
            stack.append(c)
        elif c in ")]}":
            assert stack and stack[-1] == {")": "(", "]": "[", "}": "{"}[c]
            stack.pop()
    assert not stack


def test_glass_gets_no_relief_and_metal_only_for_the_metal_class():
    text = pbrify.render_usda([dict(name="g", hash="G1", **{"class": "glass"},
                                    w=64, h=64, size=256)], "t")
    assert "inputs:displace_in = 0.0" in text
    text = pbrify.render_usda([dict(name="m", hash="M1", **{"class": "metal"},
                                    w=64, h=64, size=256)], "t")
    assert "inputs:metallic_constant = 1.0" in text


# --------------------------------------------------------- the pack-coverage guard (n20b)
#
# 5 of the 40 measured surfaces are already covered by the installed Remix pack at 2048^2 or
# 4096^2 with photographic maps, and our layer is installed LAST in mod.usda's subLayers, so
# generating those five would replace a 2048^2 albedo with a derived 1024^2 one: a regression
# and 33 MB of wasted VRAM. pbrify therefore checks the pack first. These tests pin that policy
# (and the VRAM delta the report carries, which docs/TEXTURES.md quotes).

HASH = "0123456789ABCDEF"


def fake_pack(game, size=512, mips=9, hash_hex=HASH):
    """A fake <game>/rtx-remix/mods/gameReadyAssets/texture/pavement/ set of pack maps.

    Only the headers matter (pack_coverage reads the header), so the payload is one block.
    """
    d = os.path.join(str(game), "rtx-remix", "mods", "gameReadyAssets", "texture", "pavement")
    os.makedirs(d, exist_ok=True)
    for suf in ("albedo", "normal", "rough", "height"):
        dxgi = bcn.DXGI["BC5_UNORM"] if suf == "normal" else bcn.DXGI["BC4_UNORM"]
        bcn.write_dds(os.path.join(d, "%s_%s.dds" % (hash_hex, suf)), b"\0" * 16, size, size,
                      dxgi, mips=mips)
    return d


def test_pack_coverage_reports_what_the_pack_already_ships(tmp_path):
    fake_pack(tmp_path, size=512, mips=9)
    cov = pbrify.pack_coverage(str(tmp_path), HASH)
    assert set(cov) == {"albedo", "normal", "rough", "height"}
    assert cov["albedo"][1:4] == (512, 512, 9)
    assert pbrify.pack_coverage(str(tmp_path), "FFFFFFFFFFFFFFFF") == {}
    assert pbrify.pack_coverage(str(tmp_path / "no-game-here"), HASH) == {}


def run_main(tmp_path, monkeypatch, pack_size=None, keep_covered=False):
    """pbrify.main() over a one-texture list; find_texture is patched to a synthetic raster."""
    import json as _json
    game, out = tmp_path / "game", tmp_path / "cache"
    if pack_size:
        fake_pack(game, size=pack_size)
    px = np.full((32, 32, 4), 90, np.uint8)
    px[:, :, 3] = 255
    data, tex = fake_tex(32, 32, px.tobytes())
    monkeypatch.setattr(pbrify, "find_texture",
                        lambda game, name, label=None: ("fake.txd", data, tex))
    lst = tmp_path / "list.json"
    lst.write_text(_json.dumps({"textures": [dict(name="road", hash="0x" + HASH,
                                                  **{"class": "asphalt"}, w=32, h=32)]}))
    argv = ["--game", str(game), "--list", str(lst), "--out", str(out),
            "--texdir", "texture/rhy_pbr", "--min-size", "256",
            "--report", os.path.join(str(out), "report.json")]
    if keep_covered:
        argv.append("--keep-covered")
    assert pbrify.main(argv) == 0
    rep = os.path.join(str(out), "report.json")
    return _json.load(open(rep)) if os.path.isfile(rep) else None


def test_a_surface_the_pack_already_covers_is_skipped(tmp_path, monkeypatch, capsys):
    rep = run_main(tmp_path, monkeypatch, pack_size=512)
    assert rep is None, "a set was generated on top of the pack's own 512^2 maps: %r" % rep
    assert not os.path.isdir(str(tmp_path / "cache" / "texture" / "rhy_pbr")) or \
        not os.listdir(str(tmp_path / "cache" / "texture" / "rhy_pbr"))
    assert "already covers it" in capsys.readouterr().out


def test_keep_covered_generates_it_anyway(tmp_path, monkeypatch):
    rep = run_main(tmp_path, monkeypatch, pack_size=512, keep_covered=True)
    assert rep and len(rep["sets"]) == 1
    assert rep["sets"][0]["pack"]["albedo"].startswith("512x512")


def test_a_pack_map_smaller_than_ours_does_not_suppress_us(tmp_path, monkeypatch):
    rep = run_main(tmp_path, monkeypatch, pack_size=128)          # ours is 256^2 (min-size)
    assert rep and len(rep["sets"]) == 1


def test_the_report_carries_the_vram_delta_against_the_game_raster(tmp_path, monkeypatch):
    rep = run_main(tmp_path, monkeypatch)
    s = rep["sets"][0]
    # 32x32 32-bit: 64 bytes per 4x4 block, whole mip chain
    assert s["src_vram"] == bcn.mip_bytes(32, 32, 64)
    assert s["vram"] == (bcn.mip_bytes(256, 256, 16) * 2 + bcn.mip_bytes(256, 256, 8) * 2)
    assert s["vram"] > s["src_vram"]
    assert rep["vram_bytes"] == sum(x["vram"] for x in rep["sets"])
    assert rep["disk_bytes"] == sum(x["bytes"] for x in rep["sets"])


def test_a_rank_file_that_spells_the_class_class_still_feeds_pbrify(tmp_path, monkeypatch):
    """An old rank.json (key "class_") must not break the tool."""
    import json as _json
    game, out = tmp_path / "game", tmp_path / "cache"
    px = np.full((32, 32, 4), 90, np.uint8)
    px[:, :, 3] = 255
    data, tex = fake_tex(32, 32, px.tobytes())
    monkeypatch.setattr(pbrify, "find_texture",
                        lambda game, name, label=None: ("fake.txd", data, tex))
    lst = tmp_path / "rank.json"
    lst.write_text(_json.dumps({"textures": [dict(name="road", hash="0x" + HASH, class_="asphalt",
                                                  w=32, h=32)]}))
    assert pbrify.main(["--game", str(game), "--list", str(lst), "--out", str(out),
                        "--texdir", "texture/rhy_pbr", "--min-size", "256",
                        "--report", os.path.join(str(out), "report.json")]) == 0
    rep = _json.load(open(os.path.join(str(out), "report.json")))
    assert rep["sets"][0]["cls"] == "asphalt"
