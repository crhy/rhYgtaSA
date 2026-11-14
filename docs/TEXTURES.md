# HD PBR textures for the surfaces you actually look at (n20 → n20b)

rhY's MUST-HAVE #1 (`.strata/target-2026-11-12.md`): *"better TEXTURES — high-res PBR (albedo +
normal + roughness, metal where real) for the most visible surfaces first: roads, pavements,
walls, building glass, terrain/grass ground."*

n20 built the machinery and was parked on an unrelated gate failure (`a96ec15` on
`failed/n20-hd-pbr-textures-183453`); n20b finished it. What is in the tree now:

| | |
|---|---|
| **what it is** | 35 replacement texture sets — albedo, normal, roughness, height — for the 40 map surfaces that own the most screen in our own tour shots, generated from the game's own rasters |
| **the switch** | `bash tools/textures.sh on\|off\|status\|generate [GAME_DIR]` |
| **cost** | 140 DDS, **91.9 MB on disk = 91.9 MB of texture memory** with mip chains (the game's own rasters for the same 35 materials: 1.08 MB) |
| **formats** | BC3/DXT5 albedo, BC5 normal, BC4 rough + height, DX10 headers for the BC4/BC5, **full mip chain on all 140 files** — the format census of the installed pack, measured in FINDINGS |
| **what it is not** | nothing here has been seen in game yet (rule 1: this box does not run the game), and it is not a photographic texture pack — see "Where the pixels come from" |

The gap this attacks is **T1** in [TARGET.md](TARGET.md): the installed pack
([Hemry81/GTASA-Remix](https://github.com/Hemry81/GTASA-Remix), MIT) ships 1152 replacement maps
over 23 categories, and `tools/pack_pbr_census.py` (n18) counted **2363** `mat_<HASH>` materials
with a replacement albedo for only **174** of them — so ~2190 materials render with the runtime's
default legacy BRDF, which `rtx_materials.cpp` proves can only ever be a *constant* roughness.
n18 set that constant (`rtx.legacyMaterial.roughnessConstant = 0.65`); this task gives real maps to
the surfaces that are biggest on screen.

---

## The switch

```bash
bash tools/textures.sh status  "/media/rhy/data/Gamez/GTASA-RTX"
bash tools/textures.sh on      "/media/rhy/data/Gamez/GTASA-RTX"      # installs; generates the cache first if missing
bash tools/textures.sh off     "/media/rhy/data/Gamez/GTASA-RTX"      # removes exactly what `on` added
bash tools/textures.sh generate "/media/rhy/data/Gamez/GTASA-RTX"     # rebuild build/textures, touch nothing else
```

`on` does three things, all of them recorded:

1. generates (once) into `build/textures/` — never into the game folder — with
   `tools/pbrify.py`, then copies `texture/rhy_pbr/*.dds` and `rhy_textures.usda` into
   `<game>/rtx-remix/mods/gameReadyAssets/`;
2. appends `@./rhy_textures.usda@` as the **last** entry of `mod.usda`'s `subLayers`, so it
   composes on top of the pack's own `Materials.usda` — the same install shape `tools/mirrors.py`
   and [MIRRORS.md](MIRRORS.md) verified in game, and the only one ever verified here;
3. keeps its state in `<game>/rtx-remix/mods/gameReadyAssets/.rhys-textures/` (a dot-dir, which the
   runtime does not scan as a layer): `files.txt` lists every file it copied, `mod.usda.bak` is the
   pristine copy of the one file the sublayer edit touches.

`off` deletes exactly those files, restores `mod.usda` from the backup and removes the state dir.
`scripts/textures-selftest.sh` proves the round-trip by hashing a whole fake game tree before and
after: **byte-identical**, no state dir, no `texture/rhy_pbr/`, and `off` twice is a no-op. If the
layer was added by hand (no state dir), `off` still removes the layer file and the sublayer line by
name, and never guesses at DDS files it did not copy.

Each texture becomes one material override in the layer — the same
`over "RootNode" { over "Looks" { over "mat_<HASH>" { over "Shader" { … } } } }` shape the pack and
[MIRRORS.md](MIRRORS.md) use — with `AperturePBR_Opaque` for everything except the glass class,
which gets `AperturePBR_Translucent`. `<HASH>` is Remix's own texture hash: XXH3 over mip 0 of the
raster, `tools/remix_texhash.py` (s18), which is how the runtime names the material.

---

## Which 40 surfaces — measured, not guessed

A screenshot says what is bright, not which texture it is. `tools/texture_rank.py` measures the
same question from the game's data instead:

* **every map instance in the world**: 9315 from the text IPLs under `<game>/data/maps/**/*.ipl`
  plus 36516 from the 164 binary ("bnry") IPLs inside `models/gta3.img` = **45831 instances,
  11612 distinct models** (the reader is `tools/lamp_list.py`'s, which resolves binary-IPL model
  *ids* through the `.ide` files — the position in the `.img` index is not the model id);
* **the cameras are the tour's own**: the 16 stops are parsed as text out of
  `<game>/moonloader/SARemix_Tour.lua` (never executed). The four **daytime** stops are the ones
  that matter for textures — `00-baseline-noon-grove`, `12-car-mirror-grove-1300`,
  `13-ped-crowd-pershing-noon`, `14-vegetation-vinewood-noon` — the same shots
  [TARGET.md](TARGET.md) §1 measures against the reference video;
* **the score is a solid angle**: each instance inside a 45° cone around the look direction and
  inside 200 m contributes the solid angle its triangles subtend (area projected onto the view
  direction, divided by d²), charged to the textures its model's materials use.

That gives **291 textures with geometry in the cones**, of which the **top 40 carry 40.9 % of the
solid angle of everything in those cones** (11 700 m² of surface). The top of the list is the list
this task ships:

| texture | share % | m² | models | class | source | Remix hash |
|---|---|---|---|---|---|---|
| `tanstucco1_LA` | 6.19 | 306 | 2 | stucco | 128² | `0x2C79166AA87BCB31` |
| `sjmlahus28` | 6.18 | 259 | 2 | grass | 128² | `0x41B324AB431388CD` |
| `badmarb1_LAn` | 6.18 | 259 | 1 | pavement | 256² | `0xB15C60D43F2DB8A1` |
| `heliconcrete` | 1.23 | 571 | 8 | concrete | 256² | `0x1074B67934B68C3C` |
| `plaintarmac1` | 0.93 | 487 | 3 | asphalt | 128² | `0x80E05D87A13FA659` |
| `dt_road_stoplinea` | 0.87 | 232 | 4 | roadmark | 512² | `0xC0AC95F7EDBE3D04` |
| `pavetilealley256128` | 0.87 | 547 | 2 | pavement | 128² | `0x47848CE98557BD6A` |
| `comptroof4` | 0.83 | 306 | 5 | roof | 128² | `0x141CAB5C9292E0AA` |
| `sidewgrass1/3/2` | 0.80 each | 154 | 3 | grass | 256² | `0x8A04C400…`, `0xA1A030CB…`, `0xB53827C8…` |
| `stonewalls1_LA`, `retainwall1` | 0.76 each | 445 | 1 | wall | 128² | `0xC6AD685E…`, `0x0B2DE7B6…` |

The full 40 are `config/texture-pbr.json` (name, Remix hash, source size, class, share, and the
pack's own files for the same hash — see the coverage guard below). Classes: 11 grass, 9 wall,
5 asphalt, 4 pavement, 3 roof, 2 concrete, 2 ground, and one each of stucco, roadmark, glass,
brick.

**What the measurement is not, stated plainly.** There is no occlusion (a wall does not hide the
block behind it) and no per-triangle material split: SA's map geometry stores triangles as 4-byte
records whose 4th byte is `0xff` in every model checked, so the per-triangle material index is only
reachable through librw's binMesh decoder. Measured on `lae2_roads89`: the geometry struct is
7392 bytes = 900 + 900 + 24 + 2784 + 2784, which closes with two 4-byte index arrays and one morph
target and leaves no room for a material array. So a model's triangles are split **evenly** over
its distinct textures, and the honest claim is *"the textures that cover the most geometry in front
of the tour cameras"*. `--sensitivity` re-ranks at nine settings so the reader can see how much the
answer depends on the parameters:

| cone / radius | 100 m | 200 m (shipped) | 300 m |
|---|---|---|---|
| 35° | 32/40 (Jaccard 0.67) | 37/40 (0.86) | 36/40 (0.82) |
| 45° (shipped) | 35/40 (0.78) | **40/40 (1.00)** | 39/40 (0.95) |
| 55° | 33/40 (0.70) | 33/40 (0.70) | 32/40 (0.67) |

The set is stable in the direction that matters (a wider cone or a longer radius changes 1–8 of the
40); the ranking is dominated by *which models* are in the cone, not by how their triangles split.

---

## Where the pixels come from — and why not a photograph

**The albedo is always the game's own raster.** SA's road textures carry the road *markings*:
`dt_road_stoplinea` is the stop lines, `crossing_law*` the zebra crossings, and the map's UVs are
authored for those rasters. Replace such an albedo with a CC0 photograph of asphalt and you erase
the markings and break the art direction. So:

```
albedo   = the game's raster, decoded (DXT1/DXT3/DXT5/32-bit/16-bit), LANCZOS 4x, capped at 2048
height   = the relief already baked into that raster (its high-frequency luminance)
normal   = the gradient of that height (Sobel), scaled by the material class
rough    = the class's roughness, modulated by the local detail
metal    = 1 for the metal class, 0 everywhere else (the pack itself sets `inputs:metallic_texture
           = @@`, i.e. empty, for its road materials)
```

Everything is deterministic — no neural upscaler, no randomness. That is a deliberate choice and it
has a citation: [RemasterBlaster](https://github.com/morteza323/RemasterBlaster) (MIT, the most
serious public toolkit for exactly this problem: a Flux img2img pipeline with ~90 filename
categories and TXD rebuilding) documents the failure mode in its own README — *"Real-ESRGAN …
turns asphalt into wet plastic"*. Wet plastic is what the installed pack's photographic albedos
already read as under Remix's lighting, and [TARGET.md](TARGET.md) §1 measures it.

Licensed sources we looked at and did **not** ship, with the reason:

| source | licence | why it is not the albedo here |
|---|---|---|
| [Hemry81/GTASA-Remix](https://github.com/Hemry81/GTASA-Remix) | MIT | it *is* the SA PBR set (1152 BC4/BC5/BC7 maps, 23 categories) and we already run it; its **coverage** is the gap, not its quality |
| [RemasterBlaster](https://github.com/morteza323/RemasterBlaster) | MIT | needs a Flux model and a large VRAM budget; the right citation for the technique, not runnable inside rule 1 |
| [SA Proper Shaders](https://www.mixmods.com.br/2026/11/sa-proper-shaders/) (Junior_Djjr) | proprietary | bundles PBR shader maps for hundreds of common textures, but rewrites the same D3D9 path the Remix bridge owns; `redistribute: no` in `deps/manifest.json` (`proper-shaders`) |
| [ambientcg.com](https://ambientcg.com) / [polyhaven.com/textures](https://polyhaven.com/textures) | CC0 | photographic, so they may drive **normal/roughness detail** on top of a game albedo; they may not replace it (markings). Listed in `deps/manifest.json` for that future step |

---

## The pack-coverage guard: 5 of the 40 are deliberately **not** generated

Our layer is installed **last** in `mod.usda`'s `subLayers`, so last one wins. Measured against the
installed pack (`config/texture-pbr.json`'s `pack_has` field, `tools/pbrify.py pack_coverage()`),
five of the 40 already have the pack's own maps, at a resolution ours would be a downgrade from:

| texture | the pack already ships | ours would be |
|---|---|---|
| `sidewgrass1` `0x8A04C4005EC0D1C2` | 2048² albedo (mips 12) + normal + rough + height | 1024² derived |
| `sidewgrass3` `0xA1A030CB1C532B19` | 2048² albedo (mips 12) + normal + rough + height | 1024² derived |
| `sidewgrass2` `0xB53827C8E12BB227` | 2048² albedo (mips 12) + normal + rough + height | 1024² derived |
| `gb_nastybar03` `0x032123631B5B198B` | 2048² albedo (mips 12) + normal + rough + height | 1024² derived |
| `dt_road` `0x7FF5EBF9BAD3765F` | **4096²** albedo + normal + rough + height, all with mips | 2048² derived |

So `pbrify` skips them by default and says so; the layer carries **35** overrides, not 40, and the
skip saves 33.6 MB of VRAM for five surfaces that would have got worse. `--keep-covered`
(`tools/textures.sh on --keep-covered`) generates them anyway for an A/B.

Two side observations from the same census, both in FINDINGS: four of those five
(`sidewgrass1/2/3`, `gb_nastybar03`) ship their `_rough` and `_height` with **`mipCount = 1`** —
three of them their `_normal` too — which is exactly the class of file the runtime logs
`A suboptimal replacement texture detected … Please make sure all replacement textures have
mip-maps.` about; and the runtime resolves mod asset paths **case-insensitively** under Proton —
`Materials.usda` says `@./texture/Asphalt/…@` while the directory on this ext4 volume is
`texture/asphalt/`, and it still loads.

---

## Formats and VRAM

| map | format | header | why |
|---|---|---|---|
| `_albedo` | **BC3 / DXT5** | legacy `DXT5` fourcc | the alpha channel survives (SA's foliage and fences use it) and BC3 is what the pack's own legacy files use |
| `_normal` | **BC5_UNORM** | DX10 | two channels, the pack's own choice for 251 of its 1152 maps |
| `_rough`, `_height` | **BC4_UNORM** | DX10 | one channel, the pack's choice for 445 of them |

**Not BC7.** The plan said BC7; what ships is BC3, because there is no BCn encoder on this box:
Pillow 13 *reads* BCn but its DDS writer emits uncompressed RGBA whatever `rawmode` says (measured:
`rawmode="BC7"` produces a DX10 header with DXGI 64 = R8G8B8A8 and a lossless round-trip), and
ImageMagick 7 refuses too. `tools/bcn.py` is therefore ours — BC1/BC3/BC4/BC5 plus a BC7
mode-6 encoder — and `scripts/textures-selftest.sh` cross-checks it against Pillow, against
ffmpeg's DDS decoder, against the game's own DXT1 rasters, and against the D3D block layout
written out of the spec (all 16 selectors of a block, and the full RGTC alpha ladder). BC7 mode-6
is in the encoder but is not used by the shipped set; switching the albedo to BC7 is a one-line
change in `write_set` once someone wants the extra quality.

Every file carries a **full mip chain** (140/140 verified in the self-test and again in the
measurement pass), because the runtime complains about mip-less replacements.

VRAM, from `build/textures/report.json` (`tools/pbrify.py --report`):

| output size | sets | one set (4 maps, mips) | total |
|---|---|---|---|
| 512² | 19 | 1.05 MB | 19.9 MB |
| 1024² | 13 | 4.19 MB | 54.5 MB |
| 2048² | 1 | 16.8 MB | 16.8 MB |
| 512×256, 128×256 | 2 | 0.52 / 0.13 MB | 0.6 MB |
| **35 sets** | | | **91.9 MB on disk = 91.9 MB in memory** |

The same 35 materials cost the game **1.08 MB** as it ships them (mip chains included), so the
layer is **+90.8 MB**. For scale, n08's measurement of the running game
([FINDINGS.md](FINDINGS.md), "Frame pacing") is 3.5–4.4 GB of the card's 16.4 GB: this is ~0.6 % of
the card, ~2 % of what the game already uses. BCn is stored compressed, so disk bytes and VRAM bytes
are the same number — there is no "smaller on disk, bigger in memory" trap here.

The maps themselves, measured after decoding them back (not the intent, the result):

| class | n | roughness mean (min…max) | normal-map std (/255) | albedo luma |
|---|---|---|---|---|
| wall | 9 | 0.657 (0.61…0.75) | 22.4 | 131 |
| grass | 7 | 0.721 (0.68…0.80) | 23.1 | 97 |
| pavement | 4 | 0.649 (0.57…0.75) | 23.9 | 124 |
| asphalt | 4 | 0.555 (0.48…0.70) | 35.1 | 106 |
| roof | 3 | 0.650 (0.56…0.82) | 22.5 | 69 |
| concrete | 2 | 0.506 (0.46…0.56) | 14.1 | 130 |
| ground | 2 | 0.699 (0.68…0.80) | 24.2 | 140 |
| stucco / brick | 2 | 0.694 / 0.630 | 24.9 / 25.0 | 173 / 111 |
| roadmark | 1 | 0.555 (0.50…0.60) | 8.0 | 141 |
| glass | 1 | 0.041 (0.02…0.12) | 0.8 | 82 |

Albedo fidelity: mean absolute luminance shift against the source raster **2.75** luma (worst
4.05), mean alpha 252.6 — the upscaling does not repaint the surface.

---

## The material classes, and where their numbers came from

`tools/texture_rank.py`'s `CLASSES` table drives roughness, metallic and normal strength per
surface class, and it is **calibrated on the installed pack**, not invented: `python3
tools/pbrify.py --game <GAME> --calibrate` decodes a sample of the pack's own `_rough`/`_normal`
maps per category and prints the medians (asphalt 0.588, wall 0.681, pavement 0.655, ground 0.735,
concrete 0.514, roof 0.685, metal 0.507, windows 0.060, marble 0.079, normal-map std 21–48 for the
rough categories, ~1 for glass and marble). The shipped table then sits on those medians:

```
asphalt 0.59 ±0.11  strength 2.3      wall 0.68 ±0.07   strength 1.5     glass 0.06 ±0.06  metal 0   strength 0.05
concrete 0.51 ±0.05  strength 0.9     brick 0.68 ±0.09  strength 1.7     metal 0.51 ±0.04  metal 1   strength 1.0
pavement 0.66 ±0.09  strength 1.6     stucco 0.70 ±0.07 strength 1.6     rock 0.68 ±0.09   strength 2.9
roadmark 0.55 ±0.05  strength 0.5     roof 0.69 ±0.13   strength 1.5     ground 0.74 ±0.06 strength 1.6
```

`strength` is how much of the raster's own high-frequency detail becomes relief; `rough_var` is the
half-width of the detail modulation, so the decoded map stays inside the class's measured band
(the table above is the proof). Glass gets `displace_in = 0` and no relief; metal is the only class
with `metallic_constant = 1`.

---

## Regenerating, and changing the selection

```bash
python3 tools/texture_rank.py --game "$G" --out build/tmp/n20b/rank.json --top 40 \
        --pack-census --sensitivity          # re-measure the 40 (13 s, reads the game folder)
python3 tools/pbrify.py --game "$G" --list config/texture-pbr.json --out build/textures \
        --scale 4 --max-size 2048 --report build/textures/report.json     # 39 s, writes build/ only
bash tools/textures.sh on "$G"               # copies them in
```

`config/texture-pbr.json` is the checked-in selection: edit it (or point `--list` at a fresh
`rank.json`) and re-run. `tools/textures.sh` honours `TEXTURES_CACHE` and `TEXTURES_LIST` so the
self-test can run the whole switch against a two-texture synthetic set. Re-measuring on 2026-11-14
reproduced the shipped 40 in the same order, so the selection is reproducible from the game data.

---

## What this does **not** claim

* **Not seen in game.** Rule 1: nothing here has been rendered. The look is a hypothesis;
  [NEXT-TEST.md](NEXT-TEST.md) step 8 is the leg that tests it, and the numbers to compare are
  stops `00-baseline-noon-grove` and `13-ped-crowd-pershing-noon`.
* **Not a relief map.** The height field is derived from the albedo's own high-frequency energy —
  SA's environment rasters are pre-lit, so the creases are already drawn in them. It is a
  heuristic, and `derive_height`'s docstring says so.
* **Not a metallic map.** Only the `metal` class sets `metallic_constant = 1`; none of the 40 is
  metal, and the pack keys metal to only 13 materials in the whole game (n18's census).
* **Not a fix for the pack's mip-less `_rough`/`_height` files** — those are the pack's, and we do
  not touch files we did not copy.
* **Not a bigger list.** 40 surfaces is what the task asked for; the same tools rank 291 in the
  cones and the whole game's 2363 `mat_<HASH>` materials (n18's census), and `--top 100` is one flag.

## What tests it

* `scripts/textures-selftest.sh` (24 checks, run by `scripts/check-all.sh` as `textures`): the tour
  cameras really come from the game's tour script; the encoders against Pillow, ffmpeg, the game's
  own DXT1 and the spec block layout; the generated set against the pack's format census (BC3/BC5/
  BC4, mip chains, albedo mean preserved, normal statistics); the layer as USD with every asset
  path present; `on → on → off` against a fake game tree hashed byte-for-byte; and the guards
  (`off` with no state dir, `on` twice does not duplicate the sublayer).
* `tests/test_bcn.py` (the encoders and the DDS headers), `tests/test_pbrify.py` (the map
  derivation, the USD layer, the pack-coverage guard, the VRAM delta), `tests/test_texture_rank.py`
  (the class vocabulary, the camera reader, and the shipped `config/texture-pbr.json` itself),
  `tests/test_usd_switch.py` (the `subLayers` edit round-trip).
