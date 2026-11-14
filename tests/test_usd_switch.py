#!/usr/bin/env python3
"""n20 - tools/usd_switch.py: the one-line `subLayers` edit every Remix layer in this repo needs.

A USD syntax error in a sublayer does not stop Remix: it loads nothing and says nothing, so the
validator here is the gate. These cases run on synthetic mod.usda text in a tmp dir.
"""
import os
import sys

import pytest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools"))
import usd_switch as US    # noqa: E402

MOD = '''#usda 1.0
(
    customLayerData = {
        string lightspeed_game_name = "SA Remix"
        string lightspeed_layer_type = "replacement"
    }
    subLayers = [
        @./SARemix.usda@,
        @./Materials.usda@
    ]
    upAxis = "Z"
)
'''

LAYER = "rhy_textures.usda"


@pytest.fixture()
def mod(tmp_path):
    p = tmp_path / "mod.usda"
    p.write_text(MOD, encoding="utf-8")
    return str(p)


def test_add_appends_one_entry_and_leaves_the_others(mod):
    new, changed, why = US.add_sublayer(open(mod).read(), LAYER)
    assert changed and "sublayer added" in why
    assert "@./SARemix.usda@," in new and "@./Materials.usda@," in new
    assert new.count("@./%s@" % LAYER) == 1
    US.validate(new, "mod.usda")


def test_add_is_idempotent(mod):
    once, _, _ = US.add_sublayer(open(mod).read(), LAYER)
    twice, changed, why = US.add_sublayer(once, LAYER)
    assert not changed and why == "sublayer already present"
    assert once == twice


def test_add_first_puts_the_layer_before_the_pack(mod):
    new, _, _ = US.add_sublayer(open(mod).read(), LAYER, "first")
    assert new.index("@./%s@" % LAYER) < new.index("@./SARemix.usda@")
    US.validate(new, "mod.usda")


def test_remove_round_trips_to_the_original(mod):
    text = open(mod).read()
    new, changed, _ = US.add_sublayer(text, LAYER)
    back, changed2, _ = US.remove_sublayer(new, LAYER)
    assert changed and changed2
    assert back == text, repr(back)


def test_remove_of_a_last_entry_does_not_leave_a_dangling_comma(mod):
    new, _, _ = US.add_sublayer(open(mod).read(), LAYER)
    back, _, _ = US.remove_sublayer(new, LAYER)
    assert "Materials.usda@," not in back
    US.validate(back, "mod.usda")


def test_remove_when_absent_reports_it(mod):
    back, changed, why = US.remove_sublayer(MOD, LAYER)
    assert not changed and why == "no sublayer present"
    assert back == MOD


def test_a_mod_without_a_sublayers_block_is_refused(mod):
    bad = '#usda 1.0\n(\n    upAxis = "Z"\n)\n'
    with pytest.raises(ValueError):
        US.add_sublayer(bad, LAYER)


def test_an_entry_hidden_in_a_comment_still_counts_as_present(mod):
    """`add` is a plain substring test on purpose: Remix strips comments, but a duplicated entry
    is worse than a missed one."""
    commented = MOD.replace("    upAxis", "    # @./%s@\n    upAxis" % LAYER)
    _, changed, why = US.add_sublayer(commented, LAYER)
    assert not changed and why == "sublayer already present"


def test_cli_add_remove_and_has(mod, capsys):
    assert US.main(["add", mod, LAYER, "last"]) == 0
    assert "sublayer added" in capsys.readouterr().out
    assert US.main(["has", mod, LAYER]) == 0
    assert capsys.readouterr().out.strip() == "yes"
    assert US.main(["remove", mod, LAYER]) == 0
    assert "sublayer line removed" in capsys.readouterr().out
    assert US.main(["has", mod, LAYER]) == 0
    assert capsys.readouterr().out.strip() == "no"
    assert open(mod).read() == MOD


def test_cli_check_accepts_a_generated_layer(tmp_path):
    import pbrify
    text = pbrify.render_usda([dict(name="road", hash="AA11", **{"class": "asphalt"},
                                    w=256, h=256, size=1024)], "texture/rhy_pbr")
    p = tmp_path / "rhy_textures.usda"
    p.write_text(text, encoding="utf-8")
    assert US.main(["check", str(p)]) == 0


def test_cli_check_rejects_a_broken_layer(tmp_path):
    p = tmp_path / "broken.usda"
    p.write_text('#usda 1.0\nover "RootNode" { over "Looks" { over "mat_AA" {\n', encoding="utf-8")
    with pytest.raises(ValueError):
        US.main(["check", str(p)])


def test_cli_check_rejects_an_input_the_mdl_does_not_declare(tmp_path):
    p = tmp_path / "bad_input.usda"
    p.write_text('#usda 1.0\nover "RootNode" { over "Looks" { over "mat_AA" { over "Shader" {\n'
                 '    float inputs:not_an_input = 1.0\n} } } }\n', encoding="utf-8")
    with pytest.raises(ValueError):
        US.main(["check", str(p)])
