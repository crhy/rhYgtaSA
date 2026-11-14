#!/usr/bin/env bash
# n20: self-test for tools/textures.sh + tools/pbrify.py + tools/texture_rank.py + tools/bcn.py.
# Runs against a FAKE game tree and a tiny generated texture set under build/tmp; it never writes
# to a real install and only ever reads the game folder (rule 1).
#
#   bash scripts/textures-selftest.sh [--keep]
#
# What it proves, in order:
#   1  the tour cameras really are read out of the game's own tour script (parse_tour_stops on
#      <game>/moonloader/SARemix_Tour.lua finds the 16 stops, and the built-in fallback table
#      agrees with them);
#   2  the BC encoders round-trip: encode -> our decoder -> a second, spec-literal decoder, and
#      the legacy DXT5 file is decoded correctly by Pillow (an independent implementation);
#   3  pbrify generates a real set: BC3 albedo / BC5 normal / BC4 rough+height, DX10 or legacy
#      header, full mip chain, and the albedo's mean colour matches the source raster;
#   4  the generated layer is valid USD and every @./...dds@ path it names exists;
#   5  textures.sh on -> on -> off against a fake game tree: the sublayer lands once, the files
#      land, and after `off` every file in the tree hashes back to the pre-`on` manifest
#      (byte-identical round-trip, no state dir, no layer file, no texture folder);
#   6  the guard cases: `off` with no state dir but a hand-added sublayer removes the line and
#      leaves mod.usda parseable; `on` twice does not duplicate the sublayer.
# Ends with "ALL PASS" or "N FAILED".
set -uo pipefail
REPO="$(cd "$(dirname "$0")/.." && pwd)"
GAME="${RHYS_GAME_DIR:-/media/rhy/data/Gamez/GTASA-RTX}"
FAKE="$REPO/build/tmp/tex-fakegame"
WORK="$REPO/build/tmp/tex-selftest"
TOOL="$REPO/tools/textures.sh"
KEEP=0
for a in "$@"; do case "$a" in --keep) KEEP=1 ;; *) echo "usage: $0 [--keep]"; exit 2 ;; esac; done

pass=0; fail=0
ok()  { pass=$((pass+1)); echo "PASS  $*"; }
bad() { fail=$((fail+1)); echo "FAIL  $*"; }
chk() { local rc=$1; shift; if [ "$rc" = 0 ]; then ok "$*"; else bad "$*"; fi; }
run() { local label="$1"; shift; echo "-- $label"; "$@" > "$WORK/$label.log" 2>&1; local rc=$?
        echo "   rc=$rc (log: ${WORK#"$REPO"/}/$label.log)"; return $rc; }

# Unlike the other self-tests this one reads the real game folder (read-only, rule 1): the tour
# cameras come from the game's own SARemix_Tour.lua, the DXT1 cross-decode uses the game's own
# raster, and the generation step generates from the game's own rasters - a synthetic stand-in
# would test nothing about the formats. Say what is missing instead of failing 20 checks later.
if [ ! -f "$GAME/models/gta3.img" ] || [ ! -f "$GAME/moonloader/SARemix_Tour.lua" ]; then
  echo "FAIL  this selftest reads $GAME (models/gta3.img, moonloader/SARemix_Tour.lua) and it is"
  echo "      not there. Point RHYS_GAME_DIR at the install and re-run - it never writes to it."
  exit 1
fi

rm -rf "$WORK"; mkdir -p "$WORK"

echo "== 1. tour cameras come from the game's own tour script"
python3 - "$GAME" > "$WORK/stops.log" 2>&1 <<'PY'
import os, sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath("tools")) or ".", "tools"))
sys.path.insert(0, "tools")
import texture_rank as TR
lua = os.path.join(sys.argv[1], "moonloader", "SARemix_Tour.lua")
text = open(lua, encoding="utf-8", errors="replace").read()
stops = TR.parse_tour_stops(text)
assert len(stops) >= 16, "only %d stops parsed from %s" % (len(stops), lua)
names = [s[0] for s in stops]
for want in ("00-baseline-noon-grove", "13-ped-crowd-pershing-noon"):
    assert want in names, "%s not among the parsed stops %s" % (want, names)
# the built-in table must agree with the file for the daytime stops the ranking uses
by_name = {s[0]: s for s in stops}
for label, cam, look in TR.DEFAULT_STOPS:
    if label in by_name:
        _n, c2, l2 = by_name[label]
        assert max(abs(a - b) for a, b in zip(cam, c2)) < 1e-6, label
        assert max(abs(a - b) for a, b in zip(look, l2)) < 1e-6, label
print("%d stops parsed; the built-in table matches the script" % len(stops))
PY
chk $? "parse_tour_stops reads the real tour script and the fallback table agrees"

echo "== 2. the BCn encoders, cross-checked against other implementations"
run bcnc python3 - "$GAME" <<'PY'
import os, subprocess, sys
sys.path.insert(0, "tools")
import numpy as np
import bcn
from PIL import Image
W = H = 64
work = "build/tmp/tex-selftest"
rng = np.random.default_rng(11)
def rmse(a, b):
    return float(((a.astype(float) - b.astype(float)) ** 2).mean() ** 0.5)
g = np.clip(np.linspace(0, 255, W)[None, :] + rng.normal(0, 6, (H, W)), 0, 255).astype("uint8")
# (a) the RGTC alpha ladder, straight from EXT_texture_compression_rgtc: denominator 7
t = bcn._alpha_table(np.array([255]), np.array([0]))[0]
assert list(t) == [255, 0, 218, 182, 145, 109, 72, 36], list(t)
t = bcn._alpha_table(np.array([0]), np.array([255]))[0]
assert list(t) == [0, 255, 51, 102, 153, 204, 0, 255], list(t)   # the six-value mode
# (b) BC1/BC3: Pillow reads our file bit-exactly, with a varying alpha
al = np.clip(np.linspace(0, 255, H)[:, None] + rng.normal(0, 10, (H, W)), 0, 255).astype("uint8")
rgba = np.dstack([g, 255 - g, np.roll(g, 8, 1), al]).astype("uint8")
e3 = bcn.encode_bc3(rgba, mips=False)
d3 = bcn.decode_bc3(e3, W, H)
assert rmse(d3[:, :, :3], rgba[:, :, :3]) < 12, rmse(d3[:, :, :3], rgba[:, :, :3])
assert rmse(d3[:, :, 3], al) < 3, rmse(d3[:, :, 3], al)
open(work + "/x_bc3.dds", "wb").write(bcn.write_dds("-", e3, W, H, bcn.DXGI["BC3_UNORM"],
                                                    mips=1, fourcc_legacy=b"DXT5"))
P = np.asarray(Image.open(work + "/x_bc3.dds"))
assert rmse(P[:, :, :3], d3[:, :, :3]) < 1.0, ("Pillow vs ours, rgb", rmse(P[:, :, :3], d3[:, :, :3]))
assert rmse(P[:, :, 3], d3[:, :, 3]) < 1.0, ("Pillow vs ours, alpha", rmse(P[:, :, 3], d3[:, :, 3]))
# (c) a spec-literal read of one BC1 block, written from the D3D11 block layout: all 16
#     selectors of block 0 against the four-entry colour table, not just the first pixel.
#     (n20b: the first version of this check compared selector 1 against the 2/3 mix, which is
#     the c1 entry, not that one - it failed on a correct encoder.)
blk = e3[8:24]
c0 = int.from_bytes(blk[0:2], "little"); c1 = int.from_bytes(blk[2:4], "little")
def unq(v, sh, bits):
    v = (v >> sh) & ((1 << bits) - 1)
    return (v << (8 - bits)) | (v >> (2 * bits - 8))
CHAN = ((0, 11, 5), (1, 5, 6), (2, 0, 5))
tab = []
for _c, sh, bits in CHAN:
    a, b = unq(c0, sh, bits), unq(c1, sh, bits)
        # BC1: c0 > c1 gives the 2/3 and 1/3 mixes; c0 <= c1 gives a 1/2 mix and transparent black
    tab.append([a, b, (2 * a + b) // 3, (a + 2 * b) // 3] if c0 > c1 else [a, b, (a + b) // 2, 0])
idx = int.from_bytes(blk[4:8], "little")
px = bcn.blocks(bcn.decode_bc3(e3, W, H)[:, :, :3])[0]        # block 0: (16, 3), pixel i = 4*y+x
bad = []
for y in range(4):
    for x in range(4):
        s = (idx >> (2 * (4 * y + x))) & 0b11
        for c, _sh, _bits in CHAN:
            if abs(int(px[4 * y + x, c]) - tab[c][s]) > 1:
                bad.append((x, y, c, s, int(px[4 * y + x, c]), tab[c][s]))
assert not bad, ("BC1 block 0 vs the spec table", hex(c0), hex(c1), bad[:4])
asel = (int.from_bytes(e3[0:8], "little") >> 16) & 0b111
a0, a1 = int(e3[0]), int(e3[1])
# the RGTC alpha ladder again, in full: eight entries when a0 > a1, six (plus 0 and 255) when
# a0 <= a1. (n20b: the first version interpolated a0 -> a1 linearly, which is not the spec: the
# two endpoints are entries 0 and 1 and the mixes start at entry 2.)
if a0 > a1:
    ladder = [a0, a1] + [((7 - i) * a0 + i * a1) // 7 for i in range(2, 7)] + [0, 255]
elif a0 == a1:
    ladder = [a0, a1, a0, a0, a0, a0, 0, 255]
else:
    ladder = [a0, a1] + [((5 - i) * a0 + i * a1) // 5 for i in range(2, 5)] + [0, 255]
want = ladder[asel]
got_a = int(bcn.decode_bc3(e3, W, H)[0, 0, 3])
assert abs(got_a - want) <= 1, ("alpha ladder", asel, a0, a1, want, got_a)
# (d) BC4/BC5: our decode against ffmpeg's, if ffmpeg is installed (it is on the box this was
#     developed on; the check is skipped, not failed, without it)
rg = np.dstack([g, np.roll(g, 4, 0)])
e5 = bcn.encode_bc5(rg, mips=False); d5 = bcn.decode_bc5(e5, W, H)
e4 = bcn.encode_bc4(g, mips=False); d4 = bcn.decode_bc4(e4, W, H)
assert rmse(d5, rg) < 4, rmse(d5, rg)
assert rmse(d4, g) < 4, rmse(d4, g)
if subprocess.run(["bash", "-c", "command -v ffmpeg"], capture_output=True).returncode == 0:
    for name, blob, fmt, want in (("bc4", e4, 80, g), ("bc5", e5, 83, rg)):
        open("%s/x_%s.dds" % (work, name), "wb").write(
            bcn.write_dds("-", blob, W, H, fmt, mips=1))
        subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-i",
                        "%s/x_%s.dds" % (work, name), "%s/x_%s.png" % (work, name)],
                       check=True)
        ff = np.asarray(Image.open("%s/x_%s.png" % (work, name)).convert("RGB"))
        ours = d4[:, :, None] if name == "bc4" else d5   # decode_bc4 is (h,w), decode_bc5 (h,w,2)
        got = ff[:, :, :1] if name == "bc4" else ff[:, :, :2]
        assert rmse(ours, got) < 1.0, (name, "ours vs ffmpeg", rmse(ours, got))
        print("%s: our decoder and ffmpeg agree" % name)
else:
    print("ffmpeg not installed: the BC4/BC5 cross-decode was skipped")
# (e) the game's own DXT1: our decoder against Pillow, bit-exact
import remix_texhash as RT, lamp_list as LL
img_path = os.path.join(sys.argv[1], "models", "gta3.img")
assert os.path.isfile(img_path), "no %s (the selftest checks this up front)" % img_path
img = {e[1].lower(): e for e in LL.read_img_index(img_path)[1]}
with open(os.path.join(sys.argv[1], "models", "gta3.img"), "rb") as fh:
    blob = LL.read_entry(fh, *[img["lae2roads.txd"][i] for i in (2, 3, 4)])
tex = [t for t in RT.parse_txd(blob, "lae2roads.txd") if t.name.lower() == "sidewgrass1"][0]
raw = blob[tex.off:tex.off + tex.size]
open(work + "/x_dxt1.dds", "wb").write(bcn.write_dds("-", raw, tex.w, tex.h, 71, mips=1,
                                                     fourcc_legacy=b"DXT1"))
P1 = np.asarray(Image.open(work + "/x_dxt1.dds"))
O1 = bcn.decode_dxt1(raw, tex.w, tex.h)
assert rmse(P1[:, :, :3], O1[:, :, :3]) < 1.0, ("game DXT1, Pillow vs ours",
                                                rmse(P1[:, :, 3], O1[:, :, 3]))
# (f) headers: DX10 for BC5/BC4, legacy fourcc for the albedo, mip chains everywhere
for name, blob, dxgi in (("normal", e5, bcn.DXGI["BC5_UNORM"]), ("rough", e4, bcn.DXGI["BC4_UNORM"])):
    fn = "%s/x_head_%s.dds" % (work, name)
    open(fn, "wb").write(bcn.write_dds("-", blob, W, H, dxgi, mips=len(bcn.mip_sizes(W, H))))
    hdr, lv = bcn.read_dds(fn)
    assert hdr["dxgi"] == dxgi and hdr["mips"] == len(bcn.mip_sizes(W, H)), hdr
    assert hdr["pfflags"] & 0x2004 == 0x2004, hex(hdr["pfflags"])
    assert len(lv) == hdr["mips"] and lv[-1][0] == 1
    assert hdr["linear"] == ((W + 3) // 4) * ((H + 3) // 4) * bcn.block_bytes(dxgi), hdr["linear"]
hdr, lv = bcn.read_dds(work + "/x_bc3.dds")
assert hdr["fourcc"] == b"DXT5" and hdr["dxgi"] is None
# (g) blocks()/unblocks() are exact inverses on a non-square image (the 4x4-tile scramble bug)
a = np.arange(8 * 12 * 3, dtype="uint8").reshape(8, 12, 3)
assert np.array_equal(bcn.unblocks(bcn.blocks(a), 8, 12, 3), a)
print("bc3/bc5/bc4: spec tables, Pillow and ffmpeg cross-decodes, headers, mips: ok")
PY
chk $? "BC3/BC5/BC4 encode+decode, the RGTC ladder, and the Pillow/ffmpeg cross-decodes"

echo "== 3. pbrify generates a real set"
mkdir -p "$FAKE"
python3 - "$GAME" "$WORK" > "$WORK/mklist.log" 2>&1 <<'PY'
import json, os, sys
game, work = sys.argv[1], sys.argv[2]
json.dump({"textures": [
    {"name": "tanstucco1_LA", "class": "stucco", "hash": "0x2C79166AA87BCB31", "w": 128, "h": 128,
     "txd": "gta3.img:pershingsq.txd", "share": 6.19, "area": 306, "models": 2, "pack_has": None},
    {"name": "dockpave_256", "class": "pavement", "hash": "0x0000000000000000", "w": 256, "h": 256,
     "txd": "gta3.img:sw_ware01.txd", "share": 1.0, "area": 100, "models": 1, "pack_has": None},
]}, open(os.path.join(work, "list.json"), "w"))
PY
chk $? "built a two-texture selection list"
run gen python3 tools/pbrify.py --game "$GAME" --list "$WORK/list.json" --out "$WORK/cache" \
      --texdir texture/rhy_pbr --scale 4 --max-size 512 --report "$WORK/cache/report.json"
chk $? "pbrify generated the set"
run checkdds python3 - "$WORK" "$GAME" <<'PY'
import json, os, sys
sys.path.insert(0, "tools")
import bcn, pbrify
work, game = sys.argv[1], sys.argv[2]
rep = json.load(open(os.path.join(work, "cache", "report.json")))
assert len(rep["sets"]) == 2, rep
for e in rep["sets"]:
    h = e["hash"]
    for suf, dxgi in (("albedo", None), ("normal", 83), ("rough", 80), ("height", 80)):
        hdr, lv = bcn.read_dds(os.path.join(work, "cache", "texture", "rhy_pbr", "%s_%s.dds" % (h, suf)))
        assert hdr["mips"] == len(bcn.mip_sizes(hdr["w"], hdr["h"])), (h, suf, hdr["mips"])
        assert len(lv) == hdr["mips"] and lv[-1][0] == 1, (h, suf, len(lv))
        if dxgi is not None:
            assert hdr["dxgi"] == dxgi, (h, suf, hdr["dxgi"])
        else:
            assert hdr["fourcc"] == b"DXT5", (h, suf, hdr["fourcc"])
    lab, blob, tex = pbrify.find_texture(game, e["name"], e["txd"])
    assert tex is not None, "cannot find %s again" % e["name"]
    src = pbrify.decode_rgba(blob, tex)
    lum_src = float((0.2126 * src[:, :, 0] + 0.7152 * src[:, :, 1] + 0.0722 * src[:, :, 2]).mean())
    hdr, lv = bcn.read_dds(os.path.join(work, "cache", "texture", "rhy_pbr", "%s_albedo.dds" % h))
    dec = bcn.decode_bc3(b"".join(x[2] for x in lv), hdr["w"], hdr["h"])
    lum_out = float((0.2126 * dec[:, :, 0] + 0.7152 * dec[:, :, 1] + 0.0722 * dec[:, :, 2]).mean())
    assert abs(lum_out - lum_src) < 8.0, (e["name"], lum_src, lum_out)
    hdrn, lv2 = bcn.read_dds(os.path.join(work, "cache", "texture", "rhy_pbr", "%s_normal.dds" % h))
    nrm = bcn.decode_bc5(b"".join(x[2] for x in lv2), hdrn["w"], hdrn["h"])
    assert 118 < nrm[:, :, 0].mean() < 138 and 118 < nrm[:, :, 1].mean() < 138, nrm[:, :, :2].mean(0)
    assert 5 < nrm[:, :, 0].std() < 60, nrm[:, :, 0].std()
    assert hdr["w"] >= src.shape[1] and hdr["h"] >= src.shape[0], (e["name"], hdr["w"], src.shape)
print("formats, mip chains, albedo fidelity and normal statistics: ok")
PY
chk $? "the generated DDS are BC3/BC5/BC4 with mip chains, albedo mean preserved, normal sane"

echo "== 4. the generated layer"
run usd python3 tools/usd_switch.py check "$WORK/cache/rhy_textures.usda"
chk $? "the generated layer is valid USD with mat_ overrides"
run paths python3 - "$WORK" <<'PY'
import os, re, sys
work = sys.argv[1]
text = open(os.path.join(work, "cache", "rhy_textures.usda")).read()
refs = set(re.findall(r"@\./([^@\n]+\.dds)@", text))
assert refs, "the layer names no DDS"
missing = [r for r in refs if not os.path.isfile(os.path.join(work, "cache", r))]
assert not missing, "layer names files that were not written: %s" % missing
assert text.count('over "mat_') == 2, text.count('over "mat_')
assert "AperturePBR_Opaque" in text
print("%d asset paths, all present on disk" % len(refs))
PY
chk $? "every asset path in the layer exists and both materials are overridden"

echo "== 5. on/off round-trip against a fake game tree"
rm -rf "$FAKE"
mkdir -p "$FAKE/rtx-remix/mods/gameReadyAssets/materials" "$FAKE/moonloader"
cat > "$FAKE/rtx-remix/mods/gameReadyAssets/mod.usda" <<'EOF'
#usda 1.0
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
EOF
printf '#usda 1.0\nover "RootNode" { over "Looks" { over "mat_DEADBEEFDEADBEEF" { over "Shader" { } } } }\n' \
  > "$FAKE/rtx-remix/mods/gameReadyAssets/Materials.usda"
printf 'placeholder\n' > "$FAKE/rtx-remix/mods/gameReadyAssets/materials/AperturePBR_Opaque.mdl"
printf 'placeholder\n' > "$FAKE/rtx-remix/mods/gameReadyAssets/materials/AperturePBR_Translucent.mdl"
find "$FAKE" -type f -exec sha256sum {} \; | sort > "$WORK/before.txt"

export TEXTURES_CACHE="$WORK/cache"
export TEXTURES_LIST="$WORK/list.json"
run status0 bash "$TOOL" status "$FAKE"
grep -q "present: 0" "$WORK/status0.log" && grep -q "referenced by mod.usda: no" "$WORK/status0.log"
chk $? "status reports nothing installed before on"
run on bash "$TOOL" on "$FAKE"
chk $? "on"
grep -q "sublayer added" "$WORK/on.log"; chk $? "on added the sublayer to mod.usda"
[ "$(python3 tools/usd_switch.py has "$FAKE/rtx-remix/mods/gameReadyAssets/mod.usda" rhy_textures.usda)" = yes ]
chk $? "mod.usda references the layer"
n=$(ls "$FAKE/rtx-remix/mods/gameReadyAssets/texture/rhy_pbr"/*.dds 2>/dev/null | wc -l)
[ "$n" = 8 ]; chk $? "8 DDS installed (2 sets x 4 maps), got $n"
run check2 python3 tools/usd_switch.py check "$FAKE/rtx-remix/mods/gameReadyAssets/rhy_textures.usda"
chk $? "the installed layer passes the USD check"
run on2 bash "$TOOL" on "$FAKE"
[ "$(grep -c "rhy_textures.usda@" "$FAKE/rtx-remix/mods/gameReadyAssets/mod.usda")" = 1 ]
chk $? "running on twice does not duplicate the sublayer"
run status1 bash "$TOOL" status "$FAKE"
grep -q "referenced by mod.usda: yes" "$WORK/status1.log"; chk $? "status reports the layer live"
run off bash "$TOOL" off "$FAKE"
chk $? "off"
find "$FAKE" -type f -exec sha256sum {} \; | sort > "$WORK/after.txt"
diff -u "$WORK/before.txt" "$WORK/after.txt" > "$WORK/roundtrip.diff"; chk $? "the fake tree hashes back byte-identically"
[ ! -d "$FAKE/rtx-remix/mods/gameReadyAssets/.rhys-textures" ]; chk $? "no state dir left behind"
[ ! -d "$FAKE/rtx-remix/mods/gameReadyAssets/texture/rhy_pbr" ]; chk $? "no texture/rhy_pbr left behind"
run off2 bash "$TOOL" off "$FAKE"
chk $? "off twice is a no-op, not an error"

echo "== 6. guards"
python3 tools/usd_switch.py add "$FAKE/rtx-remix/mods/gameReadyAssets/mod.usda" rhy_textures.usda last >/dev/null
cp "$WORK/cache/rhy_textures.usda" "$FAKE/rtx-remix/mods/gameReadyAssets/rhy_textures.usda"
run off3 bash "$TOOL" off "$FAKE"
chk $? "off with no state dir still removes a hand-added sublayer"
[ "$(python3 tools/usd_switch.py has "$FAKE/rtx-remix/mods/gameReadyAssets/mod.usda" rhy_textures.usda)" = no ]
chk $? "the hand-added sublayer is gone"
python3 - "$FAKE" <<'PY' > "$WORK/parse.log" 2>&1
import re, sys
text = open(sys.argv[1] + "/rtx-remix/mods/gameReadyAssets/mod.usda").read()
assert "@./SARemix.usda@," in text and "@./Materials.usda@" in text, text
stack = []
for c in re.sub(r"@[^@]*@", "", re.sub(r"#[^\n]*", "", text)):
    if c in "([{":
        stack.append(c)
    elif c in ")]}":
        assert stack and stack[-1] == {")": "(", "]": "[", "}": "{"}[c], text
        stack.pop()
assert not stack, text
print("mod.usda still parses")
PY
chk $? "mod.usda is still balanced after the hand-added case"
find "$FAKE" -type f -exec sha256sum {} \; | sort > "$WORK/after2.txt"
diff -u "$WORK/before.txt" "$WORK/after2.txt" > "$WORK/roundtrip2.diff"; chk $? "and the tree still hashes back to the start"

[ "$KEEP" = 1 ] || { rm -rf "$FAKE"; rm -rf "$WORK"; }
echo
if [ "$fail" = 0 ]; then echo "ALL PASS ($pass checks)"; else echo "$fail FAILED, $pass passed"; exit 1; fi
