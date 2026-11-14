#!/usr/bin/env python3
"""n20 - the parts of tools/texture_rank.py that are pure functions.

The ranking itself needs the game's .ipl/.dff data and is exercised by
scripts/textures-selftest.sh; what is pinned here is the vocabulary the ranking is filtered
through (the material classes and their calibrated numbers, the class inference from texture
and model names, the LOD filter) and the tour-camera reader, on a synthetic tour script.
"""
import os
import sys

import pytest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools"))
import texture_rank as TR    # noqa: E402


# --------------------------------------------------------------------- the class table

def test_every_class_has_the_four_numbers_pbrify_needs():
    for cls, v in TR.CLASSES.items():
        assert set(v) >= {"rough", "rough_var", "metal", "strength"}, cls
        assert 0.02 <= v["rough"] <= 1.0, cls
        assert 0.0 <= v["rough_var"] <= 0.5, cls
        assert v["metal"] in (0.0, 1.0), cls
        assert 0.0 <= v["strength"] <= 4.0, cls
        assert v["rough"] + v["rough_var"] <= 1.02, cls


def test_the_classes_the_task_names_are_all_present_and_surface_classes_are_a_subset():
    for cls in ("asphalt", "concrete", "pavement", "kerb", "roadmark", "wall", "brick",
                "stucco", "glass", "roof", "rock", "ground", "dirt", "sand", "grass", "tile"):
        assert cls in TR.CLASSES
        assert cls in TR.SURFACE_CLASSES
    assert "other" not in TR.SURFACE_CLASSES
    assert set(TR.SURFACE_CLASSES) <= set(TR.CLASSES)


def test_glass_is_the_sharpest_and_the_only_translucent_one():
    assert TR.CLASSES["glass"]["rough"] < 0.1
    assert TR.CLASSES["glass"]["strength"] < 0.1
    assert TR.CLASSES["glass"]["shader"] == "translucent"
    assert TR.CLASSES["metal"]["metal"] == 1.0
    for cls in TR.CLASSES:
        if cls != "metal":
            assert TR.CLASSES[cls]["metal"] == 0.0, cls


def test_roughness_order_matches_the_pack_calibration():
    """Measured on the installed pack (tools/pbrify.py --calibrate): glass < concrete < asphalt
    < wall < ground."""
    order = ["glass", "tile", "concrete", "metal", "asphalt", "wall", "ground"]
    vals = [TR.CLASSES[c]["rough"] for c in order]
    assert vals == sorted(vals), list(zip(order, vals))


# --------------------------------------------------------------------- name classification

@pytest.mark.parametrize("name,cls", [
    ("plaintarmac1", "asphalt"),
    ("ws_rooftarmac1", "asphalt"),
    ("dt_road_stoplinea", "roadmark"),
    ("crossing_law", "roadmark"),
    ("dockpave_256", "pavement"),
    ("badmarb1_LAn", "pavement"),
    ("tanstucco1_LA", "stucco"),
    ("stonewalls1_la", "wall"),
    ("brickred2", "brick"),
    ("frostedglass256128", "glass"),
    ("comptroof4", "roof"),
    ("cj_slatedwood", "roof"),          # slate wins over wood: it is a roof texture
    ("sidewgrass1", "grass"),
    ("heliconcrete", "concrete"),
    ("metal3_128", "metal"),
    ("sjmlahus28", "other"),
    ("cos_hiwayout_256", "asphalt"),
    ("alleygroundb256", "ground"),
    ("compfence1_lae", "wall"),
    ("roucghstonebrtb", "rock"),
])
def test_texture_names_classify(name, cls):
    assert TR.classify(name) == cls


def test_model_names_are_the_fallback_when_the_texture_name_says_nothing():
    assert TR.classify("sjmlahus28", models=("lae2_roads89",)) == "asphalt"
    assert TR.classify("tex01", models=("pershingsq1_lan",)) in ("pavement", "concrete", "tile",
                                                                 "asphalt", "other")
    assert TR.classify("tex01", models=("cunte_house1",)) in ("wall", "stucco", "brick", "other")


def test_classify_never_raises_on_odd_names():
    for n in ("", " ", "###", "a" * 200, "\u00e9\u00fc"):
        assert TR.classify(n) in TR.CLASSES


# --------------------------------------------------------------------- LOD filter

@pytest.mark.parametrize("name,lod", [
    ("lod1_pershingsq1", True), ("pershingsq1_lol", False), ("x_lod2", True),
    ("seabed_lodc", True), ("tree01", False), ("LODA_thing", True),
])
def test_is_lod(name, lod):
    assert TR.is_lod(name) is lod


# --------------------------------------------------------------------- tour cameras

FAKE_LUA = '''
local stops = {
  { "00-baseline-noon-grove", 12, 0, 2440.0, -1672.0, 16.0, 2440.0, -1672.0, 16.0,
    2500.0, -1668.0, 13.5, "text" },
  { "13-ped-crowd-pershing-noon", 12, 30, 1479.0, -1653.5, 14.4, 1479.0, -1653.5, 14.4,
    1479.5, -1626.0, 13.6, "text" },
  { "not-a-stop", 7, 15, 1.0, 2.0, 3.0 },
}
'''


def test_parse_tour_stops_reads_camera_and_look_at():
    stops = TR.parse_tour_stops(FAKE_LUA)
    assert [s[0] for s in stops] == ["00-baseline-noon-grove", "13-ped-crowd-pershing-noon"]
    assert stops[0][1] == (2440.0, -1672.0, 16.0)
    assert stops[1][2] == (1479.5, -1626.0, 13.6)


def test_the_built_in_table_covers_every_day_stop():
    by_name = {s[0]: s for s in TR.DEFAULT_STOPS}
    for name in TR.DAY_STOPS:
        assert name in by_name
    assert len(TR.DAY_STOPS) == 4


def test_load_stops_falls_back_to_the_built_in_table(tmp_path):
    stops, src = TR.load_stops(str(tmp_path))
    assert src == "<built-in table>" and len(stops) == len(TR.DEFAULT_STOPS)
    lua = tmp_path / "moonloader"
    lua.mkdir()
    (lua / "SARemix_Tour.lua").write_text(FAKE_LUA, encoding="utf-8")
    stops, src = TR.load_stops(str(tmp_path))
    assert "SARemix_Tour.lua" in src and len(stops) == 2


# ------------------------------------------------- the shipped selection list (n20b)

def test_the_shipped_selection_list_feeds_pbrify():
    """config/texture-pbr.json is the input of tools/pbrify.py; it has to be shaped right.

    This is the seam that broke silently once already: tools/texture_rank.py wrote the class as
    "class_" (a Python keyword), so a rank.json straight into `pbrify --list` raised KeyError.
    The selection list in the tree uses "class", and pbrify now accepts both.
    """
    import json
    repo = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
    data = json.load(open(os.path.join(repo, "config", "texture-pbr.json")))
    tex = data["textures"]
    assert len(tex) == 40, "the task measured 40 surfaces"
    assert data["cone"] and data["radius"] and len(data["stops"]) == 4
    for e in tex:
        assert e["class"] in TR.CLASSES, e
        assert e["name"] and e["txd"], e
        assert e["hash"].startswith("0x") and len(e["hash"]) == 18, e
        assert int(e["w"]) > 0 and int(e["h"]) > 0, e
        assert isinstance(e["pack_has"], list), e       # measured against the pack (n20b)
    assert len(set(e["hash"] for e in tex)) == len(tex), "two textures, one Remix hash"
    covered = sum(1 for e in tex if e["pack_has"])     # measured against the installed pack
    assert covered == 5, "the pack covers %d of the 40, not 5" % covered


def test_the_rank_writer_names_the_key_pbrify_reads():
    """tools/texture_rank.py must emit "class", not "class_" (grep-level guard)."""
    src = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools",
                            "texture_rank.py"), encoding="utf-8").read()
    assert '"class"' in src or '**{"class"' in src
    assert 'e["class_"]' not in src
