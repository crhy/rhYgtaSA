## DLSS 5 on this box: the keys exist, the driver refuses them (2026-11-13, n19)

The question from [TARGET.md](TARGET.md) §2: does `rtx.dlssNeuralRendering` (DLSS 3D-Guided Neural
Rendering = DLSS 5) do **anything** on an RTX 4070 Ti SUPER under this Proton prefix, or is the
reference video's advantage simply hardware we do not own? Answer: **it does nothing here, and the
runtime already says so in its own log on a run where the feature was never asked for.** Nothing was
run for this task (rule 1: no game, no wine, no Steam); the evidence is the last real run of this
install, the runtime's option table, its source at tag `remix-1.6.1`, and `strings` on the shipped
binary.

**1. The switch is `rtx.dlssNeuralRendering.enable`, not `rtx.dlssNeuralRendering`.** The task file
names the latter, and it is the option **category**. `config/rtx-options-1.6.1.tsv:193-203` (the
runtime's own auto-generated table) and the source agree —
`RTX_OPTION("rtx.dlssNeuralRendering", bool, enable, false, "Enable DLSS 3D-Guided Neural Rendering.\n");`
(`src/dxvk/rtx_render/rtx_dlss_neural_rendering.h`). This matters because the audit the task names,
`tools/rtxkeycheck.py`, **accepts** the category spelling — the string is in `.trex/d3d9.dll`'s
strings (`build/tmp/n15/trex-d3d9.strings.txt:87759`), so the verdict is `dotted`:

```
$ printf 'rtx.dlssNeuralRendering = True\n' > build/tmp/n19-category-only.conf
$ python3 tools/rtxkeycheck.py --keys config/rtx-runtime-keys.txt build/tmp/n19-category-only.conf
ok   rtx.dlssNeuralRendering                              dotted
build/tmp/n19-category-only.conf: 1 keys, 0 unknown
$ python3 tools/rtx-option-audit.py build/tmp/n19-category-only.conf
FAIL  build/tmp/n19-category-only.conf:1 rtx.dlssNeuralRendering  - no option of this runtime has this leaf
```

A probe written with the category name would have run, changed nothing, and then been read as a
hardware answer. Same trap as n16's `rtx.tonemap.tonemappingMode`; `tools/rtx-option-audit.py` is
again the audit that catches it, and `tests/test_dlss5_probe_keys.py` keeps that negative control
alive.

**2. The runtime's own answer, from a run that never enabled it.**
`<game>/rtx-remix/logs/remix-dxvk.log` (2026-11-12 11:48, the last real run of this install):

```
195:[11:48:12.372] info:    Driver: 610.57.4
327:[11:48:12.631] info:  [RTX info] Neural Radiance Cache: supported
328:[11:48:13.146] warn:  NVIDIA DLSS-NR support query failed: (code: 0xbad00010, info: NVSDK_NGX_Result_FAIL_UnsupportedParameter)
329:[11:48:13.168] err:   Failed to initialize the Vulkan device as a Reflex low latency device: Not implemented in current driver installation.
```

`grep -c dlssNeuralRendering` over every `*.conf*` in the game folder: **0** — no file of ours, and
no file the runtime wrote, mentions the feature. So line 328 is not a consequence of our config. It
is unconditional: `NGXContext::initialize()` calls `checkDlssNeuralRenderingSupport(tempParams)`
immediately after `NVSDK_NGX_VULKAN_GetCapabilityParameters` (`rtx_ngx_wrapper.cpp:226`), and
`initialize()` runs the first time **any** NGX context is created — for this install that is Ray
Reconstruction (`rtx.enableRayReconstruction` defaults to True and every profile carries
`rtx.useDenoiser = True`). Every run asks the question; this box answers it the same way every time.

**3. What the failure code means, exactly.** `checkDlssNeuralRenderingSupport`
(`rtx_ngx_wrapper.cpp:411-417`) asks for **one** parameter first:

```cpp
int needsUpdatedDriver = 0;
NVSDK_NGX_Result result = params->Get(NVSDK_NGX_Parameter_DLSSNR_NeedsUpdatedDriver, &needsUpdatedDriver);
if (NVSDK_NGX_FAILED(result)) { Logger::warn("NVIDIA DLSS-NR support query failed: ", ...); return false; }
```

`0xbad00010` = `NVSDK_NGX_Result_FAIL_UnsupportedParameter`: the NGX in this driver (the binary
carries `.../NGX/core/rel_310_9/source/lib/nvsdk_ngx_vulkan_lib.cpp`, strings line 450569) does not
know the DLSS-NR capability parameter **at all**. We never reach the `NeedsUpdatedDriver` answer,
nor `NVSDK_NGX_Parameter_DLSSNR_Available`, nor the driver-version message the same function can
print (`NVIDIA DLSS-NR cannot be loaded due to an outdated driver. Minimum driver version required:
X.Y.`, strings line 426783) — so this box cannot even be told *which* driver NVIDIA would want. The
adjacent facts: `nvidia-smi` says `NVIDIA GeForce RTX 4070 Ti SUPER, 610.57.04, 16376 MiB`, and
Remix 1.6's own release notes (`build/tmp/n15/gh-remix-rel.json`, tag `remix-1.6.1`) state
**"Driver Baseline: Raised the minimum NVIDIA driver requirement to 616.92"** — we are below the
runtime's own floor and far below whatever DLSS-NR wants. NVIDIA describes DLSS 5 as running "on
GeForce RTX 50 Series GPUs" ([the paper page](https://research.nvidia.com/labs/adlr/DLSS5/),
[the announcement](https://www.nvidia.com/en-us/geforce/news/dlss5-breakthrough-in-visual-fidelity-for-games/)).

**4. It is not a missing-asset problem.** The model ships:
`<game>/.trex/nvngx_dlssnr.dll` = **266 424 944 bytes**, sitting next to `nvngx_dlss.dll`,
`nvngx_dlssd.dll` (RR) and `nvngx_dlssg.dll` (frame generation). The runtime also has the whole
feature wired: the option block (`rtx.dlssNeuralRendering.*`, 11 options), the NGX parameter names
(`DLSSNR.Width` … `DLSSNR.Enabled`, strings 426694-426713), the resources
(`DLSS-NR Control Mask texture` 97836, `DLSS-NR input` 97939, `DLSS-NR output` 97940), the status
export `remixinternal_GetDlssNeuralRenderingStatus` (452874) and the failure vocabulary
(`NVIDIA DLSS-NR not supported` 426778, `DLSS-NR activation failed; disabling DLSS-NR.` 428123).
Weights present + capability query refused = a driver/hardware gate.

**5. Why turning it on is inert rather than dangerous** (this is what made the probe safe to build).
`DlssNeuralRendering::isEnabled()` is
`enable() && (!isDlssNeuralRenderingSupportChecked() || supportsDlssNeuralRendering())`
(`rtx_dlss_neural_rendering.cpp:55-59`); `RtxPass::onFrameBegin` copies that into `isActive()`
(`rtx_resources.h:637-670`). With the support flag false, `Resources::updateDlssNeuralRenderingResources()`
(`rtx_resources.cpp:540-558`) allocates **no** control mask and **no** NR output, and
`RtxContext::dispatchDlssNR()` returns at its first line (`rtx_context.cpp:1897-1903`). If
activation were ever attempted and failed, the runtime turns the option off itself:
`onActivation()` → `disableAfterFailure("DLSS-NR activation failed; disabling DLSS-NR.")` →
`DlssNeuralRendering::enable.setDeferred(false)` (`rtx_dlss_neural_rendering.cpp:61-68, 285-289`).
A feature that cannot initialise cannot break startup here — no frame cost, no VRAM, no crash.
One side note for the tonemap work: `rtx.dlssNeuralRendering.enableHighlightRecovery` /
`.highlightRecoveryThresholds` are read by `DxvkToneMapping::dispatchFastToneMapping`
(`rtx_tone_mapping.cpp:384-388`), which only runs on the DLSS-NR path — so those two keys are inert
in every profile of ours, and n16's tonemap block does not interact with them.

**6. What was built anyway, and what is NOT shipped.** `config/rtx.conf.dlss5` —
`config/rtx.conf.perf` plus the eleven DLSS-NR keys at the runtime's own documented defaults
(`enable = True`, `model = 0`, `showVramUsage = True` because its tooltip says "Enable before
activating the feature") — installable with `bash tools/rtxprofile.sh dlss5 [GAME_DIR]` and one
command back (`current` / `restore`). It exists so the probe is a real, reversible experiment on any
box where the answer might change (a newer driver, a 50-series card), not so this box pretends.
**No shipped profile carries a DLSS-NR key** (`config/rtx.conf`, `.perf`, `.nopops` are all clean);
`tests/test_dlss5_probe_keys.py` and `scripts/rtxprofile-selftest.sh` §7 enforce that offline.

**7. The one sub-question this task could not close.** Whether a driver ≥ 616.92 would expose
DLSS-NR on **Ada** (4070 Ti SUPER) or whether the refusal is the GPU, not the driver: the failure is
`FAIL_UnsupportedParameter` on the capability parameter, which does not distinguish them, and
upgrading the driver is not this task's call. It is answerable in ten seconds after any driver
update, with no config change at all:
`grep -i 'DLSS-NR' <game>/rtx-remix/logs/remix-dxvk.log` — if the warn line changes to
`NVIDIA DLSS-NR not available on this hardware/platform. Available: 0, …` (strings 426789) the
driver now knows the feature and the GPU is the blocker; if the line disappears, the feature is
supported and `tools/rtxprofile.sh dlss5` becomes worth a real run.

**8. The consequence, stated plainly.** The reference's single biggest advantage is unavailable on
this hardware, so T1 (materials), T3 (atmosphere) and T4 (grading) are won with the renderer we do
control — n18's legacy BRDF constant, n17's fog remap, n16/n22's tonemap and exposure anchor — and
not with a DLSS 5 switch. Do not leave a hopeful key in a shipped config.

## The materials the pack never touched, counted and decoded (2026-11-13, n18)

T1a of [TARGET.md](TARGET.md): ~2000 of the pack's materials have no PBR maps, so they render with
the runtime's default legacy BRDF. Nothing was run in the game; everything below is the read-only
pack, the runtime's own option table, its source at tag `remix-1.6.1`, and `tools/target-gap.py`
on the 2026-11-12 10:22 tour. New tool: **`tools/pack_pbr_census.py`** (census + coverage + decoded
roughness/metal/height distributions + the uncovered-hash list).

**1. The pack's real coverage is ~7 %, and n15's material count was 29 short.**
`tools/pack_pbr_census.py <game>/rtx-remix/mods/gameReadyAssets`
(full output `build/tmp/n18/census.txt`, JSON `build/tmp/n18/census.json`): 5211 files, 2111 `.dds`,
3065 `.usda/.usd`, **2363** distinct `mat_<HASH>` materials. n15's notes say 2334 because
`grep -rhoE 'mat_[0-9A-Fa-f]{16}'` prints "binary file matches" instead of the matches for the 2174
binary crates — the scan has to read bytes (`grep -a` or this tool). Maps keyed by a `<HASH>`, i.e.
bound to a *game* texture: **174 albedo, 147 normal, 121 rough, 71 height, 13 metal, 37 emissive**.
The rest of the 2111 `.dds` are 724 hand-named maps belonging to the pack's own new geometry
(`rail_base_albedo.dds`, `SpotLight_01_albedo.dds`, …) and **778 bare `<HASH>.dds` copies of the
game's own un-remastered textures** — the pack ships the original texture for 778 surfaces it never
redrew. So "2334 materials, ~300 with maps" is right in spirit and wrong in kind: **174 game
textures have a replacement albedo, and ~2190 materials are legacy.**

**2. A legacy material cannot carry a roughness map. The runtime's own source says so.**
`src/dxvk/rtx_render/rtx_materials.cpp` (tag `remix-1.6.1`, MIT; kept at
`build/tmp/n18/rtx_materials.cpp`): `LegacyMaterialData::as()` copies `LegacyMaterialDefaults` into
an `OpaqueMaterialData` — `setRoughnessConstant`, `setMetallicConstant`, `setAnisotropyConstant`,
`setAlbedoConstant`, `setOpacityConstant`, the thin-film and emission flags — and then adds only the
game's colour texture (`setAlbedoOpacityTexture(getColorTexture())`), a secondary texture, the
sampler override and the alpha rules. **The roughness of ~2190 materials is one float for the whole
game**, and no `rtx.conf` key makes it per-texture. Two names that read like they do and do not:
`rtx.demodulate.demodulateRoughness` (True, offset 0.1, "Demodulate roughness to improve specular
details") is a **denoiser front-end** term — `pass/demodulate/demodulate.comp.slang` scales the
noisy specular radiance by `getRoughnessDemodulationFactor(perceptualRoughness, offset)` before NRD
packing — and `rtx.rayreconstruction.demodulateRoughness` is DLSS-RR only, which does not
initialise on this prefix. Neither puts variation into a material.

**3. The runtime's default is *mattier* than anything the pack chose for the same surface classes.**
Decoding mip 0 of every `_rough` map (a roughness map is raw data, so texel/255 IS the perceptual
roughness) with `tools/pack_pbr_census.py --sample 8`: per-map mean **median 0.582**, p05 0.095,
p95 0.861; inside a single map the spread (p95−p05) is **0.220** at the median. By category (mean of
the per-map means): wall 0.649, asphalt 0.634, pavement 0.607, ground 0.584, buildings 0.503, props
0.419, floor 0.434, metal 0.382, vehicles 0.247, windows 0.060, marble 0.079, carpet 0.783, fabric
0.738. The runtime's default `rtx.legacyMaterial.roughnessConstant` is **0.7**
(`config/rtx-options-1.6.1.tsv`, the runtime's own table; `metallicConstant` 0.1, `anisotropy` 0).
So the gap is not "the default is too glossy" — it is that one number, with no normal map and no
variation, stands in for the pack's 0.58–0.65 paving band, its 0.50 buildings and its 0.06 glass.
Shipped: **`rtx.legacyMaterial.roughnessConstant = 0.65`** in all three profiles — one step from the
default, landing on the matte end of the pack's own paving band, not on an invented gloss. The
ladder (0.62 = the pack's paving mean, 0.58 = its overall median and its ground mean) and the abort
condition (night 02/03 ceiling above 0.000 %) are in `config/rtx.conf` and TARGET §6.

**4. The pack's 191 `_height` maps are real, and its metal coverage is 13 textures.** Height maps:
median amplitude (p95−p05, 8-bit) **84**, p05 20, max 255, and only **3 of 191 are effectively
flat** — the remastered materials have micro-relief, which is exactly what the legacy ones cannot
have. `_metal`: 32 files, 13 of them keyed to a game texture, per-map mean median 0.382. So
`rtx.legacyMaterial.metallicConstant = 0.1` (the default) is wrong in both directions — too
metallic for asphalt, nowhere near the pack's real metal — and it is not free to lower either:
metallic 0.1 with a bright albedo carries real specular energy. Left at the default, deliberately,
as the next one-value step.

**5. Six of the pack's own USD sub-layers point at files this install does not have.**
`Materials.usda → SARemix/SARemix_Vehicles_Meshes.usda`, `meshes/CJHouse/outdoor.usda →
CJ_House_outdoor_01/02/03.usd`, `texture/Vehicles/vehicles_materials.usda →
Planes/Plane_materials.usda` and `Trains/Train_materials.usda`. Those materials therefore fall back
to whatever the layer below them says, and the bridge log (2026-11-12 11:48) never mentions them.
Distinct from the 18 sub-layers whose **case** does not match the on-disk directories
(`@./texture/Asphalt/…@` against `texture/asphalt/`), which do load: the runtime resolves paths
case-insensitively under Proton (notes-n20 read the same fact out of the log). The rule for our own
offline tools is the one `tools/pack_pbr_census.py` follows: resolve USD references
case-insensitively, and treat "the file is not there byte-exactly" as a question, not a bug.

**6. Before-numbers, measured; after-numbers, owed.** `build/tmp/n18/gap.json` (5 reference frames +
the four daytime stops) reproduces n15/n17's numbers exactly: specular area 3.5–12.9 ‰ (reference)
against 4.0–27.6 ‰ (ours), blobs 67–213 against 43–479, largest blob 64–324 against 128–1192,
local contrast 0.137–0.294 against 0.131–0.214, ceiling 0.010–3.95 % against 0.003–0.027 %. Note
the direction: our specular *area* already beats the reference while our ceiling is 100–1000×
smaller, because our "bright" pixels are lifted flat sky — so this change is aimed at **blobs and
largest blob** (real, local, near-neutral highlights), not at the permille row. `KEEP WHAT WORKS`
baseline `build/tmp/n18/good-before.json` (02 p50 17.4 / 03 7.0 / 04 63.6 / 08a 100.6 / 08b 86.3 /
09 27.8, all with ceiling 0.000 %). The after-numbers need a run, which rule 1 forbids here —
[TARGET.md §6](TARGET.md) is the protocol.

**7. What the next task (the offline PBR-ify pass) gets.** `build/tmp/n18/pack-uncovered.txt`:
**2197** `mat_<HASH>` hashes with no replacement albedo, each with the number of USD layers that
reference it and the category of that layer — meshes 765, SARemix 674, SARemix+vehicles 608, props
44, ground 11, buildings 8, asphalt 5, pavement 5, wall 5 … (the vehicle and interior blocks are
large but rarely on screen; the ground/road/wall blocks are small and dominate a daytime frame).
`build/tmp/n18/pack-covered.txt` is the 166 already covered, so a pass never redoes them. `build/` is git-ignored, so if it is cleaned the two lists come back in eight seconds:
`python3 tools/pack_pbr_census.py "<game>/rtx-remix/mods/gameReadyAssets" --no-decode \
    --list-prefix build/tmp/n18/pack --json build/tmp/n18/census.json` Two
routes the census makes visible: patch the pack's material layer per material (MIT; the
`over "RootNode" { over "Looks" { over "mat_<HASH>" } }` recipe is in
[MIRRORS.md](MIRRORS.md) §7, and `tools/water_normal_soft.py` plus n20's `tools/bcn.py` prove we can
write BC4/BC5/BC7), or use the runtime's own **second** legacy constant —
`rtx.terrainBaker.material.properties.roughnessConstant` (0.7) with
`.roughnessAnisotropy` / `.metallicConstant`, gated by `rtx.terrainBaker.enableBaking` (True) and
driven by the `rtx.terrainTextures` hash list. GTA SA blends two ground textures per world sector,
which is exactly the case the Terrain Baker exists for, so that is the only place a per-class legacy
roughness exists without touching the pack — unshipped here because it needs the hash list
(`tools/remix_texhash.py` computes those hashes offline) and bakes a texture per frame.

## Vegetation that reads as real plants: the pack, the typo, the wind shader (2026-11-13, n21)

MUST-HAVE #2 of `.strata/target-2026-11-12.md`. Nothing was run in the game; every number below
comes from the staged pack and the read-only game folder, with three new tools
(`tools/vegetation_ide_check.py`, `tools/vegetation_stream.py`,
`tools/vegetation_edge_measure.py`) and `tools/dff_stats.py`.

**1. The staged pack is v2.00, the wind-shader build.** The 7z in `GTAmods/`
(`v-vegetation-mix-with-wind-shader_1793477728_486060.7z`, 75,255,432 B) and the staged folder
`GTAmods/removed-from-game/rtx-copy/V Vegetation Mix` (338 files, 190.4 MB) agree on all 336 file
names and sizes. Its readme: v2.00 (2026-10-31) "Los Modelos han sido Adaptados a Wind Shader".
The 2026-11-11 freeze therefore happened with wind-adapted geometry.

**2. Cause 1 of docs/VEGETATION.md is one letter.** Of 278 IDE lines the pack ships, exactly one
dictionary name resolves to nothing: `lod_veg_firs` (vegepart ids 784-788) while the pack ships
`lod_veg_fist.txd`. `tools/vegetation_ide_check.py` names it and emits the pair; step 5 of
`tools/vegetation.sh` writes the missing file instead of deleting 278 lines of correct data.

**3. The pinned-streaming number depends on those IDEs.** Same ImprovedStreaming ranges
(`[Range4] 615-792`, `[Range5] 800-906`, both `KeepLoaded=1`), two answers:

| which IDEs map the ids | Range4 | Range5 | pinned |
|---|---|---|---|
| the game's vanilla IDEs (steps 1-3, 6) | 7.4 → 73.9 MB | 2.3 → 18.1 MB | 9.8 → **92.0 MB** |
| the pack's own IDEs (step 0 / step 5) | 5.2 → 162.6 MB | 2.5 → 24.9 MB | 7.6 → **187.5 MB** |

s03's 190.5 MB is the second row. With LAA now set (4096 MB address space) and the effective cap
`max(1900, 1024) = 1900 MB`, the pinned pack is 4.8 % (92 MB) or 9.9 % (187.5 MB) of the cap and
the cap is 46 % of the address space: **cause 2 is no longer the prime suspect** for the freeze.

**4. New cause: the pack needs a wind shader the RTX copy does not have.**
https://www.mixmods.com.br/2022/10/wind-project/ (discontinued, folded into Proper Fixes v2;
"Necessário SkyGfx Extended v1.5 ou mais novo! Senão os modelos ficarão transparentes e bugados"),
https://www.mixmods.com.br/2021/06/sa-skygfx/ (Extended v1.5, 2022-10-03, added the wind shader;
Extended has published source), https://www.mixmods.com.br/2026/06/sa-proper-fixes/ (v2.1.1: wind
on all vegetation, LOD on every tree, random tree rotation, grass colour fix, ~30 % less RAM).
`$G/modloader/` has none of Essentials Pack / Proper Fixes / SkyGfx / Proper Shaders. And Remix
1.6.1 has no fallback: `cut -f1 config/rtx-options-1.6.1.tsv | grep -i wind` → **no key** (its
animation options are water and particles), so wind is game-side only. Coupling:
`rtx.opacityMicromap.buildRequests.enableAnimatedInstances = False` is pinned in our profiles, so
vertex-shader sway will not rebuild opacity micromaps.

**5. What the pack costs the RT pipeline, in triangles** (286 `.dff` vs their vanilla twins,
extracted from `$G/models/gta3.img`): vertices 56,745 → 1,003,148 (17.7×), triangles 42,312 →
765,921 (18.1×), bytes 3.06 → 35.68 MB (11.7×). `sm_firtbshg` 192 → 21,590 triangles,
`lod_redwoodgrp` (an LOD) 4 → 2,857. All alpha-tested foliage — the workload
`rtx.opacityMicromap.enable` and the "more texel taps than allowed" log line name.

**6. Installing the pack silently un-does s32's hair-card fix.** The pack contributes **234**
alpha-cutout card textures (`tools/vegetation_hashes.py --pattern @pack`), of which only **5**
hashes are among the 119 vanilla hashes in `config/rtx.conf`. Step 6 joins them (348 hashes,
6958 chars, one line). That line is safe: the config parser uses `std::getline(stream, line)` and
parses a hash set with `while (std::getline(ss, s, ','))` — no fixed buffer, no element cap
(dxvk-remix `remix-1.6.1`, `src/util/config/config.cpp:1058`, `:1205`).

**7. Leaf edge quality, measured.** Frame metric (calibrated on synthetic ramps: hard 1-px step →
0.00 % mid, 2-px ramp → 16.67 % / 1.0 px, 4-px ramp → 50.0 % / 3.0 px):

```
stop 14-vegetation-vinewood-noon 2026-11-12 10:28:05: foliage 17.97%  mid_alpha 17.63%  ramp 2.59 px
stop 14 (00:53 tour)             2026-11-12 00:53:39: foliage 37.48%  mid_alpha 11.17%  ramp 1.85 px
stop 00-baseline-noon-grove      2026-11-12 10:23:37: foliage 32.11%  mid_alpha 13.16%  ramp 2.46 px
```

At the source (mip-0 alpha): vanilla foliage dictionaries 190 textures — 45.5 % alpha 0 / **14.6 %
mid** / 39.9 % opaque; the pack 343 textures — 44.1 / **4.0** / 51.9. Its tree cards are harder
than vanilla's; its grass (`plant1`) is 57.8 / **36.3** / 5.9, the softest set in either install,
which is where a blend still reads as strokes.

**8. Translucency is not a config key.** Of the 28 texture-category `hash set` options in the
1.6.1 table none is "foliage"; the thin-wall/subsurface path (`rtx.subsurface.enableThinOpaque`,
`.enableTransmission`, `rtx.minOpaqueDiffuseTransmissionLobeSamplingProbability`) is per-material.
Translucent leaves need `mat_<HASH>` USD prims keyed to the hashes step 6 already prints — a
separate task. Two-sided leaves need nothing extra: `rtx.hairCardTextures` disables backface
culling by itself.

## One exposure anchor cannot serve noon and 21:00: driving `rtx.tonemap.exposureBias` from the sun (2026-11-13, n22)

Task n22 (`.strata/n22-daynight-exposure-anchor.md`), the follow-up n16 queued. n16 shipped
`rtx.tonemappingMode = 0` + `rtx.tonemap.exposureBias = -0.5` in all three profiles; in Global mode
that anchor **is** the exposure of the whole frame, and rtx.conf is a file while the game clock is
not. This section is what n22 measured, what it built, and - explicitly - what it could not measure.

### First: does the n16 after-measurement exist? No

The task's own stop condition was "take the n16 after-numbers from the live run first; if the night
shots did not move badly, this task may be unnecessary". Checked:

* `build/tmp/n16/` contains only `gap.json` and `good-before.json`, both written by n16 as **before**
  files; there is no `gap-after.json` / `good-after.json` anywhere in `build/`;
* the newest frame in `<game>/screenshots/shots/` is `shot_2026-11-12_11-51-04.png`, while n16's
  config block was committed at **2026-11-13 01:26** - so no frame has ever been drawn with
  `tonemappingMode = 0`;
* `.strata/night-status.txt` shows no test session between n16 and n22.

So the night side of n16 is **unmeasured, not measured-and-fine**, and the before numbers say the risk
is real (below). Building the curve is justified; claiming to have measured its effect is not.

### The mechanism, in one line each (1.6.1 source, already cited in the n16 section above)

`exposureFactor = 2^(rtx.tonemap.exposureBias + userEVBias)` (`rtx_tone_mapping.cpp:234`), auto
exposure off in every profile we ship, so the bias is a hard EV number; the Remix UI's own control is
`DragFloat("Global Exposure", &rtx.tonemap.exposureBias, -4, 4)` (`rtx_tone_mapping.cpp:134`), which
is where the ±4 clamp in `plugin/src/exposure.cpp` comes from. The option is real and settable at
runtime: `config/rtx-options-1.6.1.tsv` lists `rtx.tonemap.exposureBias`, float, default **0** (so
-0.5 is our number, not NVIDIA's), and `tests/test_exposure_anchor.py` fails if that name ever stops
being an option - the same audit that caught n16's task file writing `rtx.tonemap.tonemappingMode`.

### The before side, measured (the risk, not a target)

`build/tmp/n16/good-before.json` (2026-11-12 10:22 tour, Local tonemapping + `shadows = 2.5`):
noon p50 123-170 with 0.28-1.79 % black; 02 grove pools 21:00 p50 **17.4** / 34.5 % black; 03 sodium
lamp closeup 21:00 p50 **7.0** / 52.2 % black; 09 grove dawn 05:30 p50 27.8 / 8.0 % black. The night
frames' headroom (8.6-28) comes *from* the local tonemapper n16 switched off. A -0.5 EV anchor sized
for noon takes a quarter of the light off a frame already sitting there.

### What the plugin now publishes, computed by its own code (not a rendered frame)

`tests/exposure_check.cpp` (output in `build/tmp/n22/curve.txt`), shipped `[tonemap]` defaults, the
plugin's own solar model at day-of-year 186 / DST on:

| tour stop | pinned clock | sun elevation | anchor EV | vs the static -0.5 | light |
|---|---|---|---|---|---|
| 00-baseline-noon-grove | 13:00 | +78.7° | -0.500 | +0.000 | x1.00 |
| 12-car-mirror-grove | 13:00 | +78.7° | -0.500 | +0.000 | x1.00 |
| 08a/08b sunset | 19:05 | +11.0° | -0.500 | +0.000 | x1.00 |
| 09-grove-dawn | 05:30 | -3.8° | +0.089 | +0.589 | x1.50 |
| 01-07, 11 night stops | 21:00 | -10.0° | +0.500 | +1.000 | x2.00 |

Three facts fall out of that table and they are the design:

* **Noon is untouched.** Where the sun is above the band the plugin publishes exactly the -0.5 the
  profiles carry, so the daytime numbers the next live run takes are n16's numbers, unchanged by
  n22. The sunset stops (19:05, +11°) are also untouched - the two shots the report called good at
  p50 100.6 / 86.3 do not move.
* **The input is the sky, not the dial.** Equal elevation gives equal EV: 19:30 (+6.19°) and the
  morning minute that reaches the same elevation (06:24) get the same number to 1e-5, and the pair
  symmetric about solar noon agrees within 0.01 EV. Note what this does *not* say: 05:30 and 19:30
  are **not** the same sky at this latitude and season (the model's own sky: solar noon 12:57 at
  +78.73°, sunrise 05:52, geometric sunset 20:04), so 05:30 is
  still below the horizon at -3.8° and gets +0.089 EV while 19:30 is at +6.2° and gets -0.5. That is
  the point of using elevation - a clock rule would have given them the same number, which is wrong.
* **The rate bound is for the clock, not for the sun.** A whole simulated game day at the measured
  clock rate (2 s per game minute) moves the anchor at <= **0.0004 EV/s**, twenty-five times under the
  0.05 EV/s bound. What the bound exists for is a clock that jumps: the 2026-11-12 log recorded 104.5°
  of sun elevation between two samples, and `plugin/lua/SARemix_Tour.lua` pins the clock per stop, so
  13:00 -> 21:00 steps the *target* the whole 1.0 EV in one frame. Through the follower the published
  value still takes 19.9 s to get there, with a biggest single-frame move of 0.005 EV.

### Why a second rate limiter (`RhySignedRate`) and not `RhyRate`

An EV is signed and its unit is absolute. `RhyRate`'s bound is multiplicative and it clamps `cur` and
`target` to >= 0, so it cannot carry a value that crosses zero - measured in `tests/smooth_check.cpp`:
`rhyRateStep` toward -0.5 EV publishes **0.0 forever**. The signed sibling is a straight slew limit
(`|move| <= maxPerSec * dt`, plus a 0.005 EV deadband snap at the end of a ramp), and its floors live
in `rhyExposureClamp()`: `max_ev_per_sec` refused below 0.005 and above 0.25 EV/s, the EV clamp
repaired if inverted and never allowed outside ±8, the twilight band required to run upward and to
have width. Same rule as `[fade] in_ms >= 500`: a floor cannot be talked out of from the ini.

### Not measured (and what would settle it)

**Nothing about how a frame looks with the curve on.** Rule 1 of every task in this repo forbids
running the game, wine or Steam, and an exposure change only becomes a pixel once the runtime draws a
frame. The next **live run** owes: the six good shots against `build/tmp/n16/good-before.json` (the
abort line: 02 below p50 17.4 or above 34.5 % black, 03 below 7.0 / above 52.2 %), the four daytime
shots against `build/tmp/n16/gap.json` (which n22 predicts are unchanged), and
`grep -A80 'Effective RtxOption values' <game>/rtx-remix/logs/remix-dxvk.log` showing the plugin's
value rather than rtx.conf's -0.5 at a 21:00 stop. The knob to argue with is `[tonemap] night_ev`:
0.5 if the night frames come back blown, 0.0 to undo the swing while keeping the machinery, and
`apply=0` to undo the whole change. The protocol is docs/TARGET.md §6.

## The tonemapper, and the keys our own profiles had been writing wrong (2026-11-13, n16)

Task n16 (docs/TARGET.md §4 T4): the daytime frame has no bright end. Two findings came out of it —
one about the renderer, one about **our own config files**.

### Two key audits, and why the old one was not enough

`tools/rtxkeycheck.py` (s22) greps the strings of `<game>/.trex/d3d9.dll`. It answers "do the
category and the leaf of this name appear somewhere in the binary?" — which for a nested key is not
a name check at all. `tools/rtx-option-audit.py` (new) checks the exact name against
`config/rtx-options-1.6.1.tsv`, the runtime's **own** option table: `RtxOptions.md` at the
`remix-1.6.1` tag of https://github.com/NVIDIAGameWorks/dxvk-remix (MIT), auto-generated by Remix
itself with `DXVK_DOCUMENTATION_WRITE_RTX_OPTIONS_MD=1` (`rtx_options.h:1368`) and kept honest by a
runtime unit test (`tests/rtx/unit/test_documentation.cpp`). Its name column is
`RtxOptionImpl::getFullName() = category + "." + name` (`rtx_option.h:267`) — i.e. exactly the string
`rtx.conf` has to carry. Why that matters: a key that names no option is **not** an error, it is
kept in the file and ignored (`rtx_option_layer.cpp:416-421`, "Keep rtx keys that no option
manages").

| name | `rtxkeycheck.py` (strings) | the 1.6.1 option table | what the runtime really does |
|---|---|---|---|
| `rtx.tonemap.tonemappingMode` (the name the n16 task file used) | **ok, "leaf+category"** | **not an option** | the line is ignored; the option is `rtx.tonemappingMode`, int, **default 1 = Local** (`rtx_options.h:1181`) |
| `rtx.userBrightness` | ok, "dotted" | not an option | the option is `rtx.userBrightness.userBrightness`, int 0-100, default 50 — and it carries `RtxOptionFlags::UserSetting`, so it is the Remix UI's Brightness slider and lives in `user.conf`, which sits **above** `rtx.conf` (`rtx_option_layer.cpp:386-397` offers to migrate it out). Our own plugin already recorded the live warning: "`'rtx.userBrightness' is not an RtxOption …'`" (`plugin/src/rtxconf.cpp:147-160`) |
| `rtx.userBrightnessEVRange` | ok, "dotted" | not an option | `rtx.userBrightnessEVRange.userBrightnessEVRange`, float, default 3 |
| `rtx.postfx.exposureFraction` | ok, "dotted" | **an option, but not an exposure knob** | it is the shutter fraction of the post-FX **motion blur**: `rtx_postFx.cpp:127` `DragFloat("Exposure Fraction", …)` inside the motion-blur block, `:294 postFxArgs.exposureFraction`, default 0.4, tooltip "Simulate the camera exposure, the longer exposure will cause stronger motion blur" |

The EV arithmetic, from the same source, is what makes the anchor a number instead of a guess:
`userEVBias = (userBrightness - 50)/100 × userBrightnessEVRange` (`rtx_options.h:1514-1516`), and
both tonemappers apply it — `rtx_tone_mapping.cpp:234` (global) and
`rtx_local_tone_mapping.cpp:178,270` (local) — so the slider is a signed ±1.5 EV trim around 50.

### Dead keys our own profiles have been carrying (found, deliberately not fixed)

The new audit ran over `config/rtx.conf`, `.perf`, `.nopops`, `.experimental` and found three keys
that are **not options in 1.6.1**, on top of the two legacy spellings s22 already documented:

| our line | the real option | consequence |
|---|---|---|
| `rtx.enableFogMaxDistanceRemap = False` | `rtx.volumetrics.enableFogMaxDistanceRemap`, default **True** | fog max-distance remap has been running at its **default**, not at our False, in every run we ever measured. n17 (fog / aerial perspective) has to know that before it tunes fog |
| `rtx.antiCulling.object = True` | `rtx.antiCulling.object.enable`, default **False** | n04's anti-culling — "a lamp post that leaves the frustum leaves the TLAS, so a visibility ray to an off-screen lamp passes through it" — **has never been on**. `rtx.antiCulling.light = True` likewise (`rtx.antiCulling.light.enable`), which also makes `rtx.antiCulling.light.numFramesToExtendLightLifetime` (a real key, 600/1000 in our files) inert |

They are annotated in place and listed with their reasons in `config/rtx-dead-keys.txt`, and **not
renamed here**: turning the fog line live would switch a fog feature *off* (n17 owns fog), and
turning anti-culling on keeps up to `rtx.antiCulling.object.numObjectsToKeep = 10000` instances and
1000 lights in the TLAS — a VRAM decision on a card that already ate one opacity-micromap OOM device
loss (docs/VEGETATION.md). That is a perf task's call, made with a frame-time and VRAM measurement,
not a look task's. `tests/test_rtx_option_audit.py` pins the list at exactly five entries and fails
if any of them stops being dead, so the hole can only shrink.

### What the two tonemappers actually do (1.6.1 source, read in full this time)

**Local (`rtx.tonemappingMode = 1`, what every run we have ever taken used).**
`luminance.comp.slang` renders three synthetic exposures per pixel — `x·2^shadows`, `x`,
`x·2^-highlights`; with our shipped `shadows = 2.5` and the never-set default `highlights = 4` that
is **x5.66 / x1 / x0.0625, a 90:1 spread**. `exposure_weight.comp.slang` weights the three per pixel
by a gaussian around **0.5 gamma luminance** (`sigmaSq = rtx.localtonemap.exposurePreferenceSigma²`,
default 4, offset 0), the pair is mip-averaged to `rtx.localtonemap.mip = 3` (1/8 of the frame), and
`final_combine.comp.slang` fits a line `A·L + B` per tile and applies
`finalMultiplier = (A·L+B)/L` as **one multiplier on R, G and B** — the runtime's own comment: "a
crude multiplier on all RGB channels. This is... generally pretty wrong" — then ACES over it. That is
a local normalisation toward mid grey: it lifts the floor, pulls the ceiling down, and is re-solved
around whatever is in frame (n10's view-dependent exposure, and the mechanism the "Night realism"
section below already names).

**Global (`rtx.tonemappingMode = 0`).** One exposure number for the frame,
`2^(rtx.tonemap.exposureBias + userEVBias)` (`rtx_tone_mapping.cpp:234`; auto exposure is off in
every profile we ship, so it is a hard EV). `tonemapping_tone_curve.comp.slang` fits a noise-aware
curve (Eilertsen/Mantiuk/Unger) to the scene histogram over `toneCurveMinStops..toneCurveMaxStops`
(-24..8, 256 bins), **pins linear 0.18 to 0.18**, maps the top of the range to log2 output 0 (white),
clamps any lift to `rtx.tonemap.maxExposureIncrease` (5 stops), blurs the curve over 12 bins (1.5
stops) and mixes it with the previous frame's at `exp(-0.07)` — about **14 frames to settle**. Then
`finalizeWithACES` runs ACESFilm (`rtx.useLegacyACES` default True, the variant whose own tooltip
says it "over-saturates the highlights").

Two operational facts that fall out of that and matter for every future lighting task:

* **The graphics-preset layer does not own the tonemap keys.** `updateGraphicsPresets`
  (`rtx_options.cpp:410-620`) writes path bounces, denoiser flags, sparse rendering, volumetrics
  quality, stochastic alpha blend and `postFx.enable` — and no tonemap, exposure or brightness key.
  So with `rtx.graphicsPreset = 1` (High) the tonemap keys in `rtx.conf` are ours, unlike the perf
  keys s22 had to fight the preset for.
* **The global curve is a second smoothing path.** ~14 frames of temporal filtering on the whole
  image, independent of the nine temporal keys `config/rtx.conf.nopops` moves. n01's "no pops" A/B
  has to be read with that in mind — and the curve is fit to the frame's histogram, so Global mode is
  view-dependent in a *different* way (whole-frame, over ~0.25 s) than Local mode was (spatial, this
  frame).

Honest caveat: the runtime's own tooltip argues the other way — "Local tonemapping can result in
better preservation of highlights and shadows in scenes with high amounts of dynamic range whereas
global tonemapping may have to comprimise between over or underexposure". Our measurement says the
shipped Local configuration crushes the bright end at noon. That disagreement is exactly why the
after-numbers below are owed, and why the profiles keep the Local values (`shadows 1.0`,
`highlights 2.0`) sane instead of leaving the 90:1 squeeze in place for the A/B.

### Measured: the before side, and the risk

`tools/target-gap.py`, 640x360 + JPEG q72, the same normalisation as n15. `build/tmp/n16/gap.json`
reproduces n15's daytime table exactly (headroom 1.41-1.81, ceiling 0.003-0.027 %, p50 123-170,
warmth -67.2..+10.2, warm share 6.0-53.4 %) against the five reference frames (1.79-3.49,
0.010-3.95 %, 65.0-142.2, +18.7..+48.3, 81-95 %).

`build/tmp/n16/good-before.json` is the KEEP-WHAT-WORKS baseline, same tour, same live config
(the 2026-11-12 10:22 run), for the shots the report called good:

| shot | p05 | p50 | p99.9 | headroom | % black | ceiling % | warmth | warm share |
|---|---|---|---|---|---|---|---|---|
| 02-grove-30m-pools-2100 | 2.1 | **17.4** | 150.1 | 8.6 | 34.5 | 0.000 | +26.9 | 74.3 % |
| 03-sodium-lamp-closeup-2100 | 2.9 | **7.0** | 197.2 | 28.2 | 52.2 | 0.000 | +8.9 | 38.6 % |
| 04-tungsten-shopfront-2100 | 3.1 | 63.6 | 199.2 | 3.1 | 14.1 | 0.000 | +91.6 | 94.2 % |
| 08a-sunset-facing-east-1905 | 37.9 | 100.6 | 231.9 | 2.3 | 0.12 | 0.000 | -19.5 | 46.8 % |
| 08b-sunset-facing-west-1905 | 15.8 | 86.3 | 181.5 | 2.1 | 0.67 | 0.000 | -61.2 | 10.2 % |
| 09-grove-dawn-0530 | 6.5 | 27.8 | 111.4 | 4.0 | 8.0 | 0.000 | -27.0 | 1.9 % |

Read that as the risk of the change, not as a target: the good night frames live at p50 7-17 with
34-52 % black, and their headroom (8.6-28) comes **from** the local tonemapper lifting the little
light there is. A -0.5 EV anchor sized for a noon frame, applied to a 21:00 frame whose exposure is
now a fixed number, will darken them further. The abort ladder in docs/TARGET.md §6 puts the night
shots first: if 02-04 go darker than these numbers, the anchor comes back to 0 before anything else
does, and if the mode change alone still breaks them, the mode change is what reverts — "if a good
shot moves badly, the change is wrong, not the shot".

**Not measured:** anything after the change. The header's rule 1 forbids running the game, wine or
Steam, and a tonemap change only becomes a number once the runtime draws a frame. The keys, their
defaults and the mechanism above are measured; the direction of each knob is derived from the
runtime's source; the after-numbers are owed by the next live run.

## The reference video we are told to beat: what it is, and the measured gap (2026-11-12, n15)

rhY's quality target (2026-11-12): *"We want our GTA to look better than this one."* The video is
**"GTA San Andreas DLSS 5 2026 Graphics Are INSANE! RTX 5090 DLSS 5 4K Max"** by DubStepZz,
https://www.youtube.com/watch?v=BjWhv_gfjAk, published 2026-11-12 09:15 PT, 56:29. The plan lives in
[TARGET.md](TARGET.md); this section is the evidence.

**It publishes nothing about its setup.** The description is SEO boilerplate (game / GPU /
resolution / "Technology: DLSS 5") with GTA 6 keywords. The "**4 products**" under it are YouTube's
automatic shopping shelf, read out of the page's own `productListRenderer`: a PS5 Slim (StockX), an
Xbox Series X bundle (rock30games), a DualSense (Dell) and an Xbox Elite controller (GameStop). The
channel's About and Posts pages have no links and no mod list, and its comments are full of
"Please share the mod list you've been using" / "Is this a mod?" / "How did you make this??".
One commenter describes the defect we measured, in Russian: *"terrible that there is no haze and the
whole map is laid out like the palm of your hand ... this DLSS5 only hurts San Andreas"*.

**The base is ours.** A sibling upload names the Definitive Edition
(https://www.youtube.com/watch?v=Ijm1NFWOvnE), but the reference says plain "Grand Theft Auto: San
Andreas" and its sibling https://www.youtube.com/watch?v=2PIZWpuepKA promises "8K Texture Pack /
Ray Tracing Lighting ... while keeping the classic San Andreas experience". Classic SA + RTX Remix is
the only combination with texture packs *and* ray tracing *and* DLSS 5, and the SA Remix community is
one pack: **https://github.com/Hemry81/GTASA-Remix** (MIT, 10,329,625 KB, last pushed 2025-09-20) —
the two benchmark channels that made the well-known SA path-tracing videos link that pack and nothing
else (https://www.youtube.com/watch?v=o91lq7tVS0c, https://www.youtube.com/watch?v=EhRw1uSoG1c).
We already run it.

**DLSS 5 is a generative rendering stage, and we cannot have it.** NVIDIA
(https://www.nvidia.com/en-us/geforce/news/dlss5-breakthrough-in-visual-fidelity-for-games/,
2026-03-16; https://research.nvidia.com/labs/adlr/DLSS5/): "a one-step pixel-space diffusion model ...
conditioned on the current rendered frame, engine motion vectors, carried temporal state, and
artistic-direction values ... the first DLSS technology to generate the final displayed appearance ...
on GeForce RTX 50 Series GPUs", supplying exactly the things a game cannot author — "subsurface
scattering in skin and light scattering through foliage". **RTX Remix 1.6 integrates it**
(https://github.com/NVIDIAGameWorks/rtx-remix/releases, `remix-1.6.1`, 2026-10-23) and our runtime is
1.6.1, so the keys are in the option table (`strings` on `<game>/.trex/d3d9.dll`, 282,883,696 B):
`rtx.dlssNeuralRendering`, `intensity`, `structuralStrength`, `toneStrength`, `model` (0 A / 1 B /
2 C), `useAutoMask`, `skinStructureStrength`, `enableHighlightRecovery` ("recover highlights
compressed by ... **LDR-domain processing**"), `highlightRecoveryThresholds`,
`enableVolumetricControlMask`, `showVramUsage`. But this box is an **RTX 4070 Ti SUPER, driver
610.57.04** (`nvidia-smi`), Remix 1.6 raised the driver baseline to 616.92, and our own perf profile
already carries the runtime's answer: `NVIDIA DLSS-NR support query failed (0xbad00010)`. Task n19
probes it; the plan does not depend on the answer.

**The numbers** (`tools/target-gap.py`, everything at 640x360 after a JPEG q72 round-trip, because
the reference is a compressed 1280x720 YouTube frame and our shots are lossless 1920x1080 PNGs;
5 reference frames vs the 4 daytime stops of the 2026-11-12 10:22 tour, `build/tmp/n15/gap.json`):

| | reference | ours | |
|---|---|---|---|
| headroom p99.9/p50 | 1.79 / 2.32 / 2.37 / 2.51 / 3.49 | 1.41 / 1.60 / 1.64 / 1.81 | the biggest gap |
| ceiling % (luma ≥ 252) | 0.010 – 3.95 | 0.003 – 0.027 | 100–1000× fewer bright pixels |
| p50 | 65 – 142 | 123 – 170 | our anchor is ~40 luma too high |
| warmth mean(R−B) mid-tones | +18.7 … +48.3 | −67.2 … +10.2 | they are warm in every frame; three of our four are blue |
| warm-hue share | 81 – 95 % | 6 – 53 % | |
| sky luma / sat | 160–199 / 0.13–0.26 | 92–128 / 0.28–0.61 | bright pale sky vs dark deep-blue sky |
| haze far/near local contrast | 0.46 – 1.18 | 1.22 – 3.67 | our near field has 2–4× less detail than the skyline |
| local contrast (norm) | 0.137 – 0.294 | 0.131 – 0.214 | **not** the gap |
| specular ‰ (luma ≥ p95) | 2.9 – 12.9 | 4.0 – 27.6 | we score higher while having no real highlights |

The flat bright end is the same mechanism n05/n10 measured at night (section above): `tonemappingMode`
defaults to **Local**, and the 1.6.1 tooltip says so — "The tonemapping type to use, 0 for Global,
1 for Local (Default) ... Local tonemapping ... uses spatially-local parameters determined by regions
of the rendered image". n15 shows it is costing us the daytime frame too.

**The pack census** (read-only, `rtx-remix/mods/gameReadyAssets`): 5211 files, 2111 `.dds`,
**2334 unique `mat_<HASH>` materials**, suffix census 345 `_albedo` / 315 `_normal` / 280 `_rough` /
185 `_height` / **32 `_metal`**; albedo 164×2048 px, 85×1024, 37×4096. One material in seven has a
roughness map and metallic almost does not exist, which is why most of San Andreas renders with the
runtime's default legacy BRDF (task n18).

**Also recorded so nobody re-does it:** `html.duckduckgo.com` answers 0 results for GTA queries;
Bing answers GTA V spam; `libertycity.net/search` needs JS; YouTube storyboard sheets
(`i.ytimg.com/sb/<id>/storyboard3_L4/M0.jpg?sqp=rs\|…`) return **403** from this box, so the only
extra reference frames are `i.ytimg.com/vi/<id>/maxresdefault.jpg` and `maxres1..3.jpg`.
And the other way to get this look on classic SA, **https://www.mixmods.com.br/2026/11/sa-proper-shaders/**
(Junior_Djjr): full deferred rendering, per-pixel dynamic lights, volumetric PBR clouds, PBR dynamic
water with caustics/god rays, volumetric fog + SSGI/GTAO AO, and PBR shadermaps (normal/roughness/
metallic) with a bundled pack for hundreds of common textures — **proprietary supporter tiers**, still
on RenderWare/DX9, ~90 FPS on an RTX 3060. It is the best public spec for T1+T3, and it is a reference
only: it rewrites the same D3D9 path our Remix bridge owns.

# Findings: GTA SA + RTX Remix on Linux (2026-11-09)

Working log from getting GTASA-Remix running under Proton. Paths refer to the author's machine.

## Status 2026-11-09 evening
- Runs via GTASA-RTX/launch-rtx.sh (Steam Linux Runtime 4 + Proton Experimental, own prefix ~/.local/share/gtasa-rtx-pfx). Desktop + menu launcher.
- DLSS works (rhY: solid 60 FPS). DLSS frame generation hard-crashes under Proton: keep off. Reflex unsupported under Proton.
- VehFuncs ignored (needs MixMods OLA build: "Unknown limit adjuster" then crash). GInput deadlocked xinput_cs with hidraw; retrying with PROTON_PREFER_SDL=1 PROTON_ENABLE_HIDRAW=0.
- SARemix MoonLoader scripts (Sun, Light Manager, VTK) die on opcodes 00A0/00DF -> lighting flat/blue. Testing without CLEO 5 (cleo.asi parked in GTAmods/removed-from-game/rtx-parked). SARemix_VTK.lua parked (missing SARemix_DebugPrinter module; SARemiv_VTK.lua is the working copy).
- Gray square under CJ: MixSets UseHighPedShadows=0 (static) to stop the real-time shadow render target; untested.
- Water + leftover marker models need texture hashes tagged in Remix developer menu (rtx.animatedWaterTextures / rtx.ignoreTextures).
- 2026-11-09 17:15: CLEO was NOT the MoonLoader cause. Real cause: MoonLoader's built-in PLAYER_PED handle is invalid here (Compact exe?);
  getCharCoordinates(PLAYER_PED) throws. Patched SARemix_Sun.lua, SARemiv_LightMan.lua, SARemiv_VTK.lua: wait for isPlayerPlaying, then
  PLAYER_PED = getPlayerChar(PLAYER_HANDLE); Sun also loadAllModelsNow + doesCharExist guard. Originals in removed-from-game/rtx-parked/lua-orig.
  CLEO still parked (can come back if not needed for this). Gray square + controls (GInput via PROTON_PREFER_SDL=1, HIDRAW=0) confirmed fixed by rhY.
- user.conf never written: rhY's in-menu tags not saved yet.
- 17:25 RTX gta-sa.exe now its own no-LAA copy (hardlink broken): suspected MoonLoader 0.27 opcode-output crash with LAA.
- 18:10: sa_ped_safe.lua (memory reads) stops all MoonLoader crashes. rtx.conf pinned rtx.geometryAssetHashRuleString = positions,indices,texcoords,geometrydescriptor,vertexlayout
  (1.6.1 default adds vertexshader; SARemix authored on 0.5.x) -> CJ house light appeared; one Remix crash seen after it (watch).
  25 project textures referenced but missing from the GitHub repo (harmless). LightMan: loadAllModelsNow after requestModel + [dbg] counts every 10 s.
- 22:15: Remix 0.5.4 (project's own runtime) crashes before loading screen on driver 610/Proton -> back on 1.6.1 (0.5.4 parked).
  LightMan ran (91 objects, 13 lights) then VK_ERROR_DEVICE_LOST twice. Suspect: 1034 lights with unset inputs:shaping:focus/focusTint
  + empty ies:file -> NaN in 1.6. Fixed all 3150 with tools/usd_light_defaults.py (backups in removed-from-game/rtx-parked/usda-orig). LightMan re-enabled.
- 22:35: MY ERROR: the pinned hash rule (positions,indices,texcoords,geometrydescriptor,vertexlayout) was wrong. 1.6.1 default asset rule is
  positions,indices,geometrydescriptor (same as 0.5.4) -> removed the pin. Capture (Ctrl+Shift+P, needs replacements off) exported 417 meshes,
  0 matched project hashes under the wrong rule. Capture hotkey kept in rtx.conf. Earlier 'no lights' on default rule = LightMan was crashing.
- 22:45 ROOT CAUSE street lights: repo ships stale SARemiv_LightMan.lua whose anchor model ids 15892-15903 predate 5 cloud models added to
  remix.ide (anchors now 15897-15908) -> it spawned clouds/wrong cones. Shifted ids +5. Same staleness: SARemiv_VTK.lua (headlights 15916..)
  replaced by newer SARemix_VTK.lua (headlight 15923, matches ide) + stub lib/SARemix_DebugPrinter.lua + player-wait/sa_ped_safe patch.
  Default hash rule confirmed by capture: 271 project meshes matched. Replacements re-enabled.
- 23:10: Absolute D:/ paths fixed (nullObject.usd etc.) -> street lights stable + cast light. LightMan flicker loop disabled (pause 1e9),
  search radius 400->1200 (limited by lamp streaming; full fix = static lamp list from IPLs = Strata task). Street-light emissive x60,
  NullObject plates scale 1 and -0.25 m below housing. Light extents 50->300 m, headlight intensity x3 (test for "CJ goes dark near wall").
  Capture hotkey removed. Doors (gym, Idlewood Cluckin' Bell) not enterable: likely early-story locks (save at start, $371) - unconfirmed.
  Backups: removed-from-game/rtx-parked/{usda-orig,usda-pre-range,*.before-*}.

## Status 2026-11-10 (p04-p06: brightness, casting, screenshots, install hygiene)

- `VK_ERROR_DEVICE_LOST` is gone. The crash now reported by Sentry is a **CPU** crash
  of the `dxvk-remix` server (`code=0xe06d7363 type=CPU`), reached through
  `IDirect3DSwapChain9_Present -> SetGammaRamp -> CheckDeviceFormat`. `SetGammaRamp`
  is vtable index 21 of `IDirect3DDevice9`; our gamma guard was index 17 (`Present`).
  `block_gamma=1` (default) forwards `SetGammaRamp` instead of running the game's
  gamma path, which removes our contribution to that sequence. Mitigated, not root-caused.
- **`Ctrl+PrintScreen` is the game's own screenshot path** (GDI + D3D9 surface
  creation), not Remix's capture hotkey (`Ctrl+Shift+P`, which exports meshes/materials
  into `rtx-remix/captures/`). Judge brightness with `scripts/shot.sh` instead: it grabs
  the composited X11 window, i.e. the post-tonemap image, and never enters the game.
- **Brightness is a three-layer problem.** Our lamps are already dim in the test profile
  (`radius=15` = 15 cm at `rtx.sceneScale=0.01`, `radiance_big=2`). If the world is still
  blown out, the levers are `rtx.emissiveIntensity=10` (multiplies every emissive material
  in the 2174-file asset pack) and `rtx.localtonemap.shadows=2.5` (lifts the shadow floor,
  which is what makes lights look like they do not cast). `rtx.autoExposure.enabled=False`
  means nothing normalises the image. Verify with `scripts/abtest-lights.sh lights off`.
- **`SARemix_Sun.lua` was running next to our own sun** in the last session
  (`moonloader.log` shows it loaded). Two suns = double brightness. `deploy.sh` now
  disables it automatically when `[sun] enabled=1`.
- Loader scan rules, used to decide what is safe to park: MoonLoader scans
  `moonloader/*.lua` only, ModLoader scans `modloader/<name>/<name>*.asi` and honours
  `[Profiles.Default.IgnoreMods]` (`VehFuncs` is listed there, so it never loaded), the
  Remix bridge scans `rtx-remix/mods/` only. Therefore `<game>/OLD/` is inert by construction.
- `SARemix_HBAO.lua` is a text-only `mad` mod, and `mad`'s private interface GUID
  `794950f2-adfc-458a-905e-10a10b0b503b` is not implemented by dxvk-remix
  (`D3D9Texture2D::QueryInterface: Unknown interface query`), so it is a no-op.
- `scripts/tidy-game.sh` parks 14 items (192 MB of `rtx-remix/captures/`, the disabled
  Lua scripts, `modloader/VehFuncs`, stale logs) under `OLD/<stamp>/` with a
  `MANIFEST.tsv`; dry run by default, `--restore --apply` undoes it.

## Status 2026-11-10 (p07: F12 screenshot hotkey)

- `GetAsyncKeyState` is a dead end under Wine: `win32u.so` has
  `NtUserGetAsyncKeyState`, but it calls the input driver's
  `get_async_keyboard_state`, and **no** driver in this Wine build provides it
  (grep over `x86_64-unix/*.so` only hits the caller). It would return 0 forever.
- `SetWindowClass` (the trick GTA SA's Lua ASI mods use to catch hotkeys) is
  not present in any Wine library either, so a window-class hook is not available.
- MoonLoader does expose the game's own key state: `MoonLoader.asi` exports
  `isPs2KeyboardKeyPressed`, `isPs2KeyboardKeyJustPressed`, `setGameKeyState`.
  That is why the hotkey is 30 lines of Lua instead of native code: reading the
  same state natively would mean hardcoding a `gta_sa.exe` address.
- The Lua half (`plugin/lua/SARemix_Shot.lua` -> `moonloader/`) only writes
  `<game>/screenshots/pending/rhyshot_N.req`. The outside half
  (`scripts/shot-watch.sh`, started and killed by `launch-rtx.sh`) turns each
  marker into a `scripts/shot.sh` window grab under
  `<game>/screenshots/shots/`. Neither half touches D3D9, so the bridge crash
  path (`CheckDeviceFormat` / `CreateDeviceResources` / `SetGammaRamp`) is never
  entered.
- `scripts/shot.sh --trigger` writes the same marker from a terminal, so the
  capture path can be tested without the game responding to a key.


## Status 2026-11-10 (p08: "60 active lights" vs nothing on screen)

- **The plugin's own log was lying by omission.** `streetlights: 60 active lights`
  counted our bookkeeping (`g_active.size()`), not anything the renderer accepted.
  `bridge64.log` in the same session had 3460 `Invalid light handle!` lines and one
  `[RemixApi_CreateLight] Remix API call failed!`. 3460 / 60 lights = ~58 frames:
  the module was drawing handles the Remix server had never taken, every frame, and
  nothing in `rhyremix.log` said so.
- **Every light call now goes through `rhyCreateLight` / `rhyDrawLight` /
  `rhyDestroyLight` in `plugin/src/remix.cpp`.** The wrapper keeps three counters
  and `main.cpp` logs them once a second:
  `remix: live=N create_fail=N draw_fail=N budget_refused=N | pending_destroy=N`.
  `live` is handles the bridge still holds for us. `draw_fail` climbing while
  `live` stays flat is the signature of "the bridge handed me a handle for a light
  the server never accepted". A module that gets a rejected draw disables itself
  and says so, instead of failing 60 times a frame forever.
- **`active` vs `drawn` is now in the module log lines.** `streetlights: N active,
  M drawn this frame`. `active>0, drawn=0` means the lights exist in the plugin
  and nowhere else. That single distinction is what was missing.
- **A rejected module retries on the next `rhyremix.ini` edit** (`g_cfgRev`
  compared against `rhyConfigRevision()`), so tuning the ini from a terminal
  re-arms the module without restarting the game. Before this, one bad frame
  permanently disabled streetlights for the session.
- **`max_total_lights` (default 64) caps live handles across all modules.** Each
  module already capped itself without knowing about the others, so the test
  profile asked for 60 + 40 + sun + moon + probe = 103 lights at once. Whether
  the Remix renderer has a budget and what it is remains unmeasured; the cap
  stops us from finding out by overflowing it.
- **`LightInfo::hash` is an opaque `uint64_t` with no documented constraint.**
  The probe was the only light we ever created with `hash=0`, and the only one
  that never showed up; every light that has worked here carries a non-zero hash.
  Correlation, not a documented rule -- `probe_hash` is a knob so a run can test
  it alone.
- **The test profile is now a bisect ladder, not everything-on.**
  `config/rhyremix-test.ini` ships with streetlights and traffic lights **off**:
  probe (1 point light) + moon (1 distant light). Rungs: probe -> +moon ->
  +streetlights -> +trafficlights. A distant light and a point light are different
  code paths, so if the probe shows and the moon does not, the fault is in the
  distant-light parameters and not in the bridge.
- **The probe is a calibration rig, not decoration.** It follows the player
  (`probe_follow=1`, `probe_dz=2`) so "it is somewhere else" is not a variable;
  `probe_scale` answers the coordinate-space question (1 = game units like the lamp
  list, 0.01 = Remix metres per `rtx.sceneScale`); `probe_cone=0` keeps it an
  unshaped sphere so the cone is not a second suspect; `probe_sweep=1` walks
  radiance through x1, x10, x100, x1000 (4 s each) so one run brackets the visible
  order of magnitude instead of guessing.


## Status 2026-11-10b (the bridge log, counted: the lamps were never the failing ones)

- Counting `bridge64.log` instead of eyeballing it: `create_failed=1`,
  `invalid_handle=3476`, and those 3476 draws are spread over **3472 distinct
  timestamps, at most 2 per timestamp**. One handle drawn once per frame for ~60
  seconds -- that is the probe, occasionally two while a recreated probe waited
  for its delayed destroy. If the 60 streetlights had been invalid we would see
  ~60 invalid draws per timestamp.
- The single `CreateLight` failure is at `14:38:36.418`, the exact timestamp of
  the `probe:` line in `rhyremix.log`. So: **the probe was the only light the
  server rejected, and the lamp lights were created and drawn without a single
  error.**
- Consequence, and it changes the plan: the streetlights and traffic lights are
  already reaching the renderer. The remaining question is not handles, budgets
  or hashes -- it is **why a successfully drawn point light contributes no
  visible pixels**. Prime suspect is coordinate space: `rtx.sceneScale=0.01`
  means Remix works in metres, and we hand it positions in game units. A light
  at game-unit (2489, -1655, 19) read as metres sits ~2.5 km from the player:
  created fine, drawn fine, invisible. That is exactly what `probe_scale`
  (1 vs 0.01) decides, one run each.
- The distant light is the control group for that question: it carries a
  direction, not a position, so it is immune to the units of the world. If the
  moon lights the scene and the probe does not, the fault is position space.
- The counters in `main.cpp` will now show this directly: expect
  `draw_fail=0` with the new build. A non-zero `draw_fail` after this build
  means something new broke, not the old probe.
- Kept from 2026-11-10 (still worth having, no longer the lead): the
  `active`/`drawn` split, the create/draw/destroy wrappers, `max_total_lights`,
  and the probe as a calibration rig.


## Status 2026-11-10c (the blink, counted: the lamps render, four of our own bugs made them flicker)

The report that settled the open question from the previous section: street lights
DO appear, they blink, some are missing, and they stop being visible far too soon
to keep the scene together. So the coordinate-space hypothesis is dead for the
lamps — a light handed game-unit (2489, -1655, 19) is visible from the pavement.
`probe_scale=1` is right and `rtx.sceneScale=0.01` does not apply to light positions.
What is left are three bugs of our own, all visible in the numbers:

- **The global cap starved the lamps.** The test profile asked for 100 street +
  60 traffic + probe + sun + moon = 163 live handles and `max_total_lights` was
  64. The cap is first-come-first-served, so which lights exist depends on the
  order they were requested, and the order changes with the player. That is
  "some are missing" and it is also most of the blink: a light that loses its
  slot this frame and wins it back next frame is a blink. Fix: the cap is off
  by default (`max_total_lights=0`).
- **The per-module count has to cover the radius, or the cap blinks too.**
  Selection is nearest-first, so a `max_lights` smaller than the number of lamps
  inside `exit_distance` evicts lamps that are already lit, and they come back
  on the next pass — the same blink by another route. Measured around Grove
  Street (2495,-1680): 222 lamps within 300 units, ~350 within 380, 541 within
  500. The test profile therefore pairs `max_distance=300 / exit_distance=380`
  with `max_lights=320 + sticky_extra=90` (410 live at most, enough to cover the
  whole 380-unit band), and traffic keeps `250/300` with 60 (39 within 300). The
  old 60 was cutting the world dark at ~140 units: that is the "lights do not
  reach far enough to keep the scene together" half of the report, and it is a
  count problem, not a radius problem.
- **A refused slot switched the whole module off for the session.** `budget_refused`
  set the same `g_disabled` flag as a rejected draw. A cap refusal is a
  per-frame condition, not a broken code path. Fix: `rhyCreateLight` now reports
  WHY (1 = server refused, 2 = our cap) and only reason 1 disables; reason 2 retries
  next frame.
- **A retired light's destroy could land after its replacement was created.**
  `rhyDelayDestroy` keyed the pending map by handle hash, and the hash is
  `pos.x*3 + pos.y*7 + pos.z*11 + 1000003*model` — identical for a lamp that is
  recreated at the same position. The flush ran at the top of the frame, so the
  sequence was: create the replacement, then destroy the old handle, and if the
  bridge keys lights by that hash the new light died with it. It stayed dark until
  some other reselection rebuilt it: a blink that lasts seconds, not frames. Fix:
  `rhyFlushDelayedTag(tag)` runs before a module creates, and the tags are namespaced
  (`kTagStreet`, `kTagTraffic`, …) so one module's flush can never destroy another
  module's light — the old `rhyFlushDelayed(0)` destroyed every pending light in the
  process, including the probe's.
- **Selection had one radius, so lamps churned at the boundary.** A lamp at 100.4
  units with `max_distance=100` is in or out depending on the player's sub-unit
  movement, and each trip is a destroy plus a create. Fix: two radii — enter at
  `max_distance`, leave at `exit_distance` (400/500 in the test profile) — plus
  `sticky_extra` slots reserved for lamps already lit, so the count cap cannot
  evict a lamp that is already on screen.
- **Traffic lights were rebuilt every 10 seconds.** The phase key changes on every
  clock tick, and the old code destroyed and recreated every light in range when it
  did. Fix: `phase_near=60` — the phase only moves lights whose colour changes
  (red/green/yellow), and the amber-yellow of the all-red phase is not worth a
  rebuild; hysteresis keeps the set itself stable.
- **The lamp list burned 360 slots on duplicates.** The dedup key was
  `(model, position)`, so a lamp instanced by two model ids (full detail and its
  LOD) produced two lines, and one table entry with several offsets that land on
  the same point (sub_floodlite has three (0,0,0) offsets) produced three. 7371
  lines, 7002 real lamps. Fix: `dedupe_positions` keys on position alone, applied
  both to instances and to the emitted points.
- **`lamp_list.py` guessed model ids from the img index.** It mapped a name's
  position in `gta3.img` to an id; the real id comes from the `.ide` files, and
  the four "text-IPL lamps" it found were at positions no lamp post occupies
  (they are `lamppost1`/`lamppost2`/`streetlmp1`/`streetlmp2` model names, which
  no `.ide` in the game defines). Fix: parse every `.ide`, look the name up, and
  only accept an id that the lamp table recognises. Those four lines are gone.
- **Traffic light heads were 2 units too low.** `headz = radius*0.09` put the
  glow at 5.76 units for a 64-unit trafficlight1; the lamp table gives the exact
  head offset for the models it covers (cj_traffic_light4 7.6, cj_traffic_light5
  6.0, gay_traffic_light 7.65). Fix: use the table's offset for the model, else the
  offset of the covered model with the closest `.ide` radius (the radius is the
  model's bounding radius in centimetres, so trafficlight1 (64) belongs with
  cj_traffic_light4 (60), not with a 50-unit bollard).
- **What is still missing, and now counted instead of guessed.** `--report-missing`
  lists lamp-named models the lamp table has no entry for: `trafficlight1` (136
  instances) and `cj_traffic_light3` (20) are traffic lights and are already covered
  by `traffic_list.py`; `vegasairportlight` (34), `sf_rooflite` (32),
  `vgsstriptlights1` (26), `a51_light1_` (47) and a dozen one-off SFO/Las Vegas
  props are not street lamps and are left alone on purpose. `lamppost1`/`lamppost2`
  are instanced 100 times each in `LEVEL_0.ipl` and are not in the lamp table either
  — worth a look if lamp posts near those instances read as "missing".

What a good run should look like: `remix: live=~380 create_fail=0 draw_fail=0
budget_refused=0`, `streetlights: N active, N drawn this frame` with N≈300-320 near
Grove Street, and no `Invalid light handle!` lines in `bridge64.log`.

## Small light sources: what is actually in the scene (2026-11-11)

Audited every artificial light source rather than just the street lamps, because
"the lights look wrong" was being debugged one knob at a time.

**The asset pack already lights most of this, and we were doubling it.**
Inventory of `rtx-remix/mods`, 891 `.usda` files:

| prim type | count | what it is |
|---|---|---|
| `SphereLight` | 2036 | street luminaires, vehicle lights, interior fixtures |
| `RectLight` | 67 | shopfront glazing and interior ceiling strips |
| `DiskLight` | 10 | signage |
| `DistantLight` | 1 | `SARemix_Sun.usda`, the Lua sun mod's asset |

`SARemix/SARemix_StreetLights.usda` defines the lamp models themselves:
`Spot_Light_{B,M,S}_{Cold,Warm}`, each carrying **two** SphereLights
(`Light_Focus` intensity 3000-6000, `Light_Normal` 1500-3000, colour temperature
4500/6500 K). Those are templates instantiated wherever the game places a lamp,
so **every street lamp in the world is already lit by the asset pack** - and our
7002-entry `rhyremix_lights.txt` puts a second light on top of it. Two lights per
post is not just twice the brightness, it is twice the shadow and GI cost, which
is money straight out of the 60 FPS budget.

Shopfronts are covered too: 39 files under `Light/` (`Burger_Shot`, `Pizza`,
`Barber_Shop`, `Binco`, `Ammu_Nation`, `House_01/02`, `CJ_House`, `Ganton_Gym`,
`Disco_Light`, `Airport_Terminal`, ...) with `RectLight` prims. A `RectLight` is
exactly the right primitive for a window, so **we should not add our own lights
there** - a point light in front of a glowing window double-counts the same
surface and destroys the falloff.

**The gain stacking is the washed-out look.** Emissive materials in the pack carry
`inputs:emissive_intensity` up to 3000, and light prims up to 30000. Our
`rtx.conf` had `rtx.emissiveIntensity = 10`, so a shopfront window renders at
30000. That multiplier is applied on top of values the mod authors already tuned.

**Fixing the radius was blocked on a units error in our own docs.** One GTA SA
game unit is about one metre - measured from the traffic signal data, where a
signal head sits 7.6 units above its road plane and a real one hangs about 6 m up.
`README.md` and the ini both said "radius=15 is 15 cm at `rtx.sceneScale=0.01`".
It is not: `probe_scale=1` was verified to mean light positions are in game units,
so `radius=15` is a **thirty metre sphere** for a lamp housing the size of a
lunchbox. That is the shapeless-haze light pools, and it is why volumetrics gave a
fog wall instead of shafts. Nobody could shrink it because shrinking the sphere
without compensating radiance takes the street's light with it.

Fixed by separating the two: `fixture_size=1` gives the real housing size
(0.2 game units for a cobra-head luminaire, 0.1 for a 200 mm signal lens) and
`rhyRadianceForFlux()` scales radiance by `(radius_ref/radius)^2` so total emitted
power is unchanged. `fixture_scale` is the escape hatch if the small spheres turn
out to band in the path tracer.

**Discharge lamps do not have a colour temperature.** `plugin/src/lamps.cpp` builds
lamp colours from documented atomic emission lines instead:

| source | linear sRGB | note |
|---|---|---|
| high-pressure sodium | 1.000, 0.844, 0.000 | sodium D doublet at 589.0/589.6 nm |
| mercury vapour | 0.755, 1.000, 0.602 | 546.1 nm green and 435.8 nm blue dominate |
| metal halide | 0.732, 1.000, 0.793 | lines plus a real continuum |
| white LED | 0.872, 1.000, 0.762 | 450 nm pump plus phosphor band |
| tungsten 2856 K | 1.000, 0.676, 0.390 | a thermal source, so kelvin IS correct |

The sodium row is the point. Its *correlated* temperature is ~2000 K, and the old
profile used `kelt_warm=2100`, which gives 1.000, 0.537, 0.055 - a third of that
energy sits below 650 nm in the deep red, where a real sodium lamp has almost
none. Setting kelvin for a discharge lamp makes it look like a dim incandescent
bulb. `lamp_warm=hps` / `lamp_cold=mercury` replace the dial.

**Traffic signal lenses are filters, not sources**, so their colour comes from
integrating the glass passband (red 610-645 nm, amber 585-605, green 515-565)
rather than sampling one wavelength. Sampling 620 nm gives orange, because the
eye's response there still carries a lot of green; a real red lens has cut the
green out. Result: red 1.000/0.269/0.000, amber 1.000/0.769/0.000, green
0.429/1.000/0.000. Tests in `tests/lamps_check.cpp` assert the band-integrated
red is redder than the single-wavelength sample, which is what pins that down.

**Known gap, deliberately not faked.** A real signal module is strongly
directional - visor plus refractor throw a narrow beam down the approach. As of
n02 `rhyremix_traffic.txt` does carry the instance heading (v2: `x y z headz
heading lensx lensy lensz`, and the light now sits on the lens the offset points
to), but a heading is not a lens normal: it is the model's yaw, and nothing
measured says which way inside the model the visor faces - the game's own 2DFX
entry carries a position and no direction either (0x59C890 rotates three floats
and adds the translation). A cone aimed the wrong way would put a dark face on
every signal seen from the cross street, which is worse than an unshaped light,
so the shaping stays off and the heading is used only to place the bulb.

## Traffic signal heads: where the game itself puts the light (2026-11-11, s01)

The lamp list gives a pole position and a guessed head height. The game knows
where the heads are, because it draws a corona on each one every frame. Reading
that off the game is cheaper and more correct than guessing, so s01 hooks it.
Evidence for every address below is in `.strata/notes-s01-signal-heads.md`
(objdump walks of the PE plus two small esp-tracking scripts in `build/tmp/`).

**`CTrafficLights::DisplayActualLight(CEntity*)` = 0x49DAB0** (cdecl, one stack
argument; five callers in the renderer at 0x536960..0x536b6x, each guarded by a
model-id comparison). It looks up the model's **2DFX light entries** (count at
`CModelInfo+0xD`, entries through the thiscall getter at **0x4C4C70**), turns the
selected entry's position offset into a world position with **0x59C890**
(`out = m->right*e.x + m->up*e.y + m->at*e.z + m->pos`, i.e. matrix columns at
+0x00/+0x10/+0x20 and translation at +0x30), and registers a corona for the head
whose colour index matches the intersection's current aspect.

**The corona call is at 0x49DD4C**: `call 0x6FC580`, cleaned with `add $0x54,%esp`
(21 arguments). 0x6FC580 is a thunk that maps argument 10 (corona type) through a
table and tail-calls 0x6FC180, which writes the 60-byte corona store at
0xC3E064: position from argument 7 at store+0x58, and r/g/b from arguments
3/4/5 at store+0x84/0x85/0x86 (argument 6 = 0xff alpha). Hooking that one call
site rather than 0x6FC580 gives traffic heads only, and still has `esi` = the
entity and `ebx` = the head index in registers, which is the stable id.

**The game's own aspect colours** are byte tables indexed by the head's colour
index: 0x8A6214 red = {0,255,255}, 0x8A6218 green = {255,128,0}, 0xA9AD94 blue =
{0,0,0} (the first two are in `.data` and can be read straight out of the file;
the third is BSS). So phase 0 = green, 1 = amber, 2 = red - the same numbering
`rhySignalLensRgb()` uses - scaled by the global factor `[0xB7C4E4]*[0x858CA8]`.
The plugin matches the recorded colour against those three rather than indexing
the tables, so a mod or settings profile that moves the global factor cannot
turn a green head red.

**The heading gap is now confirmed at the source, not just in our list.** The
2DFX entry that supplies the head position carries a position offset and nothing
directional (0x59C890 rotates three floats and adds the translation), so there is
no lens normal to be had from the game's data either. `head_normal=1` uses the
entity matrix's "at" axis as an experiment; the default leaves the light exactly
on the corona position.

## A `jmp` over a `call` does not come with a return address (2026-11-11, s01 crash)

The signal-head hook replaces `call 0x6FC580` at 0x49DD4C with `jmp stub`. That
one word is a trap that cost a crash: a `call` pushes the return address, a
`jmp` does not, so inside the stub `esp` points at **argument 0**, not at a
return address. The first version read the argument block at `esp+40` (36 saved
registers + 4 for a return address that was never pushed) and jumped to the
callee without supplying one. The callee then read every argument one dword
late and its `ret` popped argument 0 - `lea eax,[ebx+esi]`, entity + head index,
a small integer - as the return address. The game's call chain is one dword off
from there, and execution wanders: the first version produced a fault at a
small integer eip inside the render pass. (An earlier draft of this section
blamed it for a "runaway corona-store clear loop" as well; that was wrong, see
"The 478 faults at 0x6FAAE0 are Open Limit Adjuster's lazy commit" below.)

The rule for hand-written detours over a call site: `lea` the arguments from
`esp + <bytes the stub itself pushed>`, and `push <call site + 5>` before the
tail `jmp`. Then the callee's `ret` lands on the instruction after the patched
call and the caller's own `add $0x54,%esp` balances the stack again.

Why it survived review: the stub was checked by reading its bytes out of
`objdump -d build/rhyremix.asi` against the *source's* arithmetic, which was the
thing in question. `tests/test_signal_hook.py` now asserts the encoding itself
(`lea eax,[esp+0x24]`, the push of 0x49DD51 before the tail jump) against the
compiled image, and fails against the crashing build.

**Corrected the same evening, and it is worth keeping the correction in view:**
the "runaway clear loop" is *not* caused by the detour at all. The 13:03 run
(fixed stub, deployed 13:03:33) shows the identical 478 writes at 0x6FAAE0,
`eax` starting at 0x0E153028 and `ecx` at 0x7F6F - and in that run the hook had
not been entered once: the plugin's own counters read `records=0 dropped=0
bad=0` at 13:03:58, and the Wine trace has no exception at the stub's address
until 43243.606, sixteen seconds *after* the storm. Two different stubs,
byte-for-byte different, same storm with the same register values. It is not
us, and it is not a runaway either. See "The stack realignment was the crash"
below for what our crash actually was, and the next section for the storm.

## The 478 faults at 0x6FAAE0 are Open Limit Adjuster's lazy commit (s08)

`0x6FAAE0` is the body of the clear loop at the tail of `CCoronas::Init`
(0x6FAA70, single caller 0x5BA306 inside `CGame::processLoading`, between the
"Setup game variables" and "Ped Init" progress messages). The loop is
`movl $0,(eax); add eax,0x3C; dec ecx; jne` over the 64-entry, 60-byte corona
store at 0xC3E064, and `eax`/`ecx` are loaded from **immediates** at 0x6FAACE
(`mov eax,0xC3E064`, operand at 0x6FAACF) and 0x6FAAD3 (`mov ecx,0x40`, operand
at 0x6FAAD4). So the storm's `eax=0x0E153028`, `ecx=0x7F6F` means those two
immediates were rewritten in memory - and they were: `modloader/OLA/III.VC.SA.
LimitAdjuster.asi` (`Coronas = unlimited`) has a patch table whose first entry
is 0x6FAACF and registers 0x6FAAD4 in code (`push $0x6faad4` at its +0x310d7).
The store now lives in a 32623-entry heap block (0x7F6F * 0x3C = 1,957,380
bytes) that the mod **reserves but does not commit** - "it grows when the game
needs more, and costs nothing until then".

The 478 faults are that reservation being committed one page at a time. All 478
Wine records share `esp=010bf5cc ebp=010bf660 ebx=ffffffff edx=00000420`; `eax`
walks 0x0E153028 -> 0x0E33001C (exactly (0x7F6F-0x3C)*0x3C) and `ecx` 0x7F6F ->
0x3C, and the per-fault `eax` delta is 0xFF0 = 68*0x3C (0x102C = 69*0x3C when
the alignment needs it) - one fault per page, on the first loop address in each
page, 478 pages for 478 faults. The handler is the mod's own:
`AddVectoredExceptionHandler(1, +0x2c470)`, and that function checks for
`0xC0000005`, looks the faulting address up in a table of ranges it owns and
calls `VirtualAlloc(page & ~0xfff, 0x1000, MEM_COMMIT, PAGE_READWRITE)`. The
write then succeeds, the loop finishes normally, `CCoronas::Init` returns to the
level loader and the game carries on. Cost: ~10 ms per level load and 478
first-chance exceptions in any Wine trace. Nothing to fix in rhyremix; set
`Coronas = 0` in the mod's ini if the trace noise gets in the way.

What *was* fatal in both crashes is a different thing and worth remembering:
modloader unloads Limit Adjuster during teardown (Wine 43243.969) but leaves its
vectored handler registered, so the next exception - our recorder's fault at
43244.199 - makes Wine jump to unmapped memory at 0x77F5C470 (the s01 notes
mislabelled this "the Remix bridge's handler"; the bridge client is at
0x7A560000) about 650 times, each round burning 0x880 bytes of stack, until
`err:virtual:virtual_setup_exception stack overflow`. A long chain of faults at
one address inside a mod's range is a dangling handler, not the crash: check
`grep -i unload logs/steam-12120.log` before believing it.

## The stack realignment was the crash (2026-11-11 13:04:44, s01 crash #2)

The 12:31 build fixed the return address and still crashed in the same place,
~30 s after entering the level. The fault was ours again, in the same stub, one
instruction later:

```
Unhandled exception: page fault on read access to 0xc48f9a27 in wow64 32-bit code (0x7876500f)
0x7876500f rhyremix.asi+0x1500f: movl 0x18(%ebx), %ecx     <- rhySignalRecordCorona+0x1f, args[6]
EIP:7876500f ESP:010bf4fc EBP:010bf51c EAX:78764ff0 EBX:c48f9a0f ECX:bebb9cff EDX:41a3ca0a
Backtrace: =>0 0x7876500f in rhyremix.asi (+0x1500f)  1 0x78764fd5 in rhyremix.asi (+0x14fd5)
```

Frame 1 is the stub's `call *%eax`, so the hook *was* reached - with real
arguments. Decoding the stack dump against the stub gives the whole story:

| address | value | what it is |
|---|---|---|
| `0x010BF538` | `0x010BF54C` | pusha's saved-esp slot, so the call site's esp was **0x010BF54C** |
| `0x010BF528` | `0x00200246` | pushfl |
| `0x010BF51C` | `0x010BF54C` | arg1 as pushed: the argument block, = entry esp |
| `0x010BF520` | `0x178DEFB4` | arg2: a real CEntity* |
| `0x010BF524` | `0x00000001` | arg3: head index 1 |
| `0x010BF510` | `0xC48F9A0F` | what the recorder read as arg1 (the float -575.2) |
| `0x010BF514` | `0x41A3CA0A` | what it read as arg2 (the float 20.47) |
| `0x010BF518` | `0x005540B2` | what it read as arg3 |

The stub pushed the three arguments and *then* did `and $-16,%esp`. The call
site's esp was 0x010BF54C, so `esp-48 = 0x010BF51C` is not 16-aligned: the
realignment moved esp down by 12 bytes, below the arguments that had just been
pushed, and the callee read its parameters from 0x010BF510/14/18 - the game's
own stack, three floats. `args[6]` then meant a read at 0xC48F9A27, which is not
mapped. That is the fault address in the log, exactly.

Two rules for hand-written detours, then:

- Realign the stack **before** anything is pushed onto it, and address the
  caller's argument block off a register the realignment cannot move (`mov
  ebp,esp; and $-16,esp; lea 36(%ebp),%eax`).
- Never assume the *game's* esp is aligned. A renderer's call site sits at
  whatever depth the code above it left the stack; here it was 12 off.

The fix is `pusha; pushfl; mov ebp,esp; and $-16,esp; push 0; lea 36(%ebp),%eax;
push ebx; push esi; push eax; call recorder; mov esp,ebp; popfl; popa; push
0x49DD51; jmp 0x6FC580` - the padding also puts a 16-aligned esp at the `call`.
`tests/test_signal_hook.py` now simulates the stub's esp for every alignment of
the call site and asserts the three argument pushes land exactly where a cdecl
callee reads them; it passes against the fixed image and fails against both
crashing builds. A pure-C++ test cannot see any of this.

The recorder also stopped trusting its arguments: every pointer it follows
(the argument block, the CVector, the entity matrix, the matrix's "at" axis) is
checked against the page table (`VirtualQuery`, 8-entry page cache, plus a pure
`rhyRangePlausible()` that rejects 0xC48F9A0F on its own) before it is read, and
the hook now counts `calls` separately from `records`, so "the game never reached
the patched instruction" and "it reached it and we rejected the record" are
distinguishable in the log.

## A Remix texture hash is just xxHash over mip 0 (2026-11-12, s18)

Every `mat_<HASH>` prim in the pack, every list in `rtx.conf`
(`rtx.particleTextures`, `rtx.decalTextures`, `rtx.animatedWaterTextures`, `rtx.uiTextures`,
`rtx.ignoreTextures`, …) and the `Hash: 0x…` shown by the Alt+X menu is one 64-bit hash of the
texture's pixels. `src/d3d9/d3d9_common_texture.cpp` of dxvk-remix 1.6.1 (the runtime in
`.trex/d3d9.dll`; the game's own `d3d9.dll` is only the bridge client) says exactly what goes in:

```cpp
constexpr uint32_t subresource = 0;                 // "Use subresource 0 for hashing"
const auto& buffer = source->m_buffers[subresource];
const bool useObsoleteHashMethod = NeedsUpload(subresource) && RtxOptions::useObsoleteHashOnTextureUpload();
imageHash = useObsoleteHashMethod ? XXH64(buffer->mapPtr(0), buffer->info().size, 0)
                                  : XXH3_64bits(buffer->mapPtr(0), buffer->info().size);
```

* **mip 0 only**, one layer: `buffer->info().size = GetMipSize(0)`, i.e. `align(elementSize *
  width, 4) * height`. No mips, no name, no format/size/usage (those feed the *separate* descriptor
  hash, and only for render targets).
* **when**: the hash is taken once, at the first `FlushImage()` of subresource 0
  (`d3d9_device.cpp:4912` → `SetupForRtx()`), from whatever the CPU buffer holds at that moment;
  `SetupForRtxFrom()` returns early if a hash is already set. So it is the pixels the game last put
  into the staging buffer before the first upload — for a `.txd` texture, exactly the `.txd` bytes.
* **which variant** — `XXH3_64bits`, unseeded, from the xxHash 0.8.0 copy in `src/util/xxHash/` with
  the default 192-byte `kSecret`. `rtx.useObsoleteHashOnTextureUpload` defaults to True, but
  `m_needsUpload` is still empty at `CreateTexture` time, so for `.txd` textures the `XXH64` branch
  is never the one the game takes — measured, not assumed: XXH3 matches, XXH64 never does.
* Careful with the constant: XXH3's avalanche in 0.8.0 multiplies by `0x165667919E3779F9` and
  xorshifts 37/32, while XXH64's avalanche uses `XXH_PRIME64_3 = 0x165667B19E3779F9`. Mixing them
  up produces hashes that look fine and match nothing.

`tools/remix_texhash.py` now reimplements that (pure python xxHash + a RenderWare `.txd`/`.img`
reader) and reproduces **55 of the 167 hashes in the live `rtx.conf`** and **164 of the 207
hash-named replacement textures** the pack ships — e.g. `waterclear256` →
`0x59296793798A7318` (the `mat_…` the pack's `Water_materials.usda` overrides) and `vehicledash32`
→ `texture/Vehicles/CBFFF905D20D2591_albedo.dds`. So the hashes s05 needed for the mirrors
(`xvehicleenv128 = 0x45DF1EA60BB202B5`, also present in the pack as `mat_45DF1EA60BB202B5`, and
`vehicleenvmap128 = 0x6BAFC5F6AEE0A6B5`) no longer require running the game.

**s21 correction, and it is a dead end worth recording.** Those two hashes are computed correctly but
they were the wrong targets: parsing the material tree of all 209 stock vehicle `.dff` shows both sit
in the **env/reflection slot** of the whole car — `xvehicleenv128` on 4197 materials across 179
models (body, doors, bonnet, boot, bumpers, windscreens, bikes, boats; 205 828 triangles),
`vehicleenvmap128` on 495 materials across 81 — and neither is ever a diffuse texture. The mirror
glass's own diffuse is `vehiclelights128 = 0x5AD92DBA0F2D1BCA`, which is also every head/tail/
indicator lens (1146 materials, 163 models). **No GTA SA texture is mirror-only**, so a `mat_<HASH>`
"mirror material" cannot be done by texture at all; the mesh route is in
[MIRRORS.md](MIRRORS.md) section 7 and the scan behind all of this in
`.strata/notes-s21-mirror-hash-check.md`. A texture hash tells you *which texture*, never *which
surface of which model* — that was the trap.

Two things the .txd side taught us, both of which break a naive reader over all 32949 textures of
the game:

* the raster **size word is authoritative**, not `width*height*depth/8`: the 1360 depth-32 textures
  store `w*h*4` bytes but the 31588 depth-16 ones store `w*h` bytes, and hashing the stored bytes is
  what reproduces the game's hash for both.
* SA stores **mip 0 only**, so the stored raster *is* the hashed buffer — there is no mip chain to
  rebuild. One texture (`models/txd/outro.txd`, chunk version `0x1003ffff`) has no size word at all;
  the tool refuses it rather than guess, because guessing the extent means guessing what the game
  uploads.

The 112 `rtx.conf` hashes that do **not** come from any `.txd` are the interesting half: they are
textures the game **creates and fills itself** (the sea — only `waterclear256` of 61
`animatedWaterTextures` is a real texture — and all 12 `skyBoxTextures`), so the bytes that get
hashed are generated pixels, not file bytes. `rtx.recomputeTextureHashOnWrite` (default **false**,
unset in this `rtx.conf`) is the knob that would additionally drop a hash when the game LockRects
mip 0 — and even when it is on, hashes listed in `rtx.terrainTextures` / `rtx.lightmapTextures` /
`rtx.ignoreTextures` / `rtx.ignoreBakedLightingTextures` are deliberately kept stable
(`d3d9_device.cpp:4635`, "these hashes are configured as stable identities for RTX draw-call
setup"). For generated textures the Alt+X read-out is still the only way, which is why
`docs/MIRRORS.md` keeps both recipes.

## The signal hook, re-read against the disassembly (2026-11-12, s19)

Before rhY's third in-game test, `plugin/src/signalhook.cpp` was walked
instruction by instruction against `gta-sa.exe` (md5
`85662cbb5aa3df8251ad7664f7a42929`). The geometry, in one place:

- `CTrafficLights::DisplayActualLight` (0x49DAB0) runs its loop at
  `esp = F-0xAC` (`sub esp,0x9c` + four pushes) and the 21 pushes of the corona
  call end at **`esp = F-0x100` at the call site 0x49DD4C**. 0x100 is a multiple
  of 16, so the call site has the frame's alignment: the crash log's
  `esp = 0x010BF54C` is 12 mod 16, i.e. **not** aligned, and the stub's
  `and $-16,%esp` is mandatory.
- The patched instruction is exactly 5 bytes (`E8 rel32`), the next is
  `mov eax,[esi+0x14]` at 0x49DD51, and the caller cleans with
  `add esp,0x54` at 0x49DD54 — then writes its own locals at `[esp+0x2c..0x34]`.
  A stub that leaves esp skewed by 4 bytes corrupts the loop, not just itself.
- The callee 0x6FC580 is a thunk (`movzx edx,[esp+0x28]` = argument 9, table
  0xC3E000, `jmp 0x6FC180`). The real function touches arguments 0..20 and
  nothing beyond: highest read is `[esp+0x80]` after its own `push ebx`, i.e.
  84 bytes, which is the length the recorder validates. It also **writes back
  into argument 5** (alpha) at 0x6FC2D0 — a hook placed after the call would
  read a modified argument.
- With argument 1 (the `CEntity*`) null, as it is here, the callee takes the
  path that uses argument 6 as a **world** `CVector*` and dereferences it with
  no check at all. Our page checks protect a pointer the game itself does not
  check.
- The naked stub's `.eh_frame` FDE is empty (`pc=stub..stub+0x29`, all
  `DW_CFA_nop`), so it inherits the CIE rule "return address = [esp]". Any
  exception or SEH unwind crossing the stub resumes on a data value. The
  recorder therefore allocates nothing, throws nothing, logs nothing and calls
  nothing — now enforced by a test, not by a comment.

What the review found missing, and is now in place: the caller's
`add esp,0x54` is checked (a mod that changes the argument count leaves the
call site intact and the stack unbalanced); the install is gated on
`[trafficlights] source=game` **inside** `rhySignalHookInstall()`, so no call
path can patch without it; the call site is decoded rather than compared
byte-for-byte; the patch site is read through the same pointer checks as the
records; the recorder refuses re-entrancy instead of interleaving; and the page
cache is cleared once a frame, because a page cached readable in one frame can
be decommitted in the next. The decision logic (`rhySignalPatchCheck`,
`rhyCollectHead`, the page cache) moved to `signalheads.cpp`, where
`tests/signals_check.cpp` runs it against fake memory: 89 native checks and 25
python checks, the latter including the real exe's bytes at 0x49DD4C /
0x49DD51 / 0x49DD54 / 0x6FC580, so a game update turns the test red before it
turns the game red. Full stack-depth tables in
`.strata/notes-s19-signal-hook-hardening.md`.

## Lighting changed instantly, everywhere at once (2026-11-12, s28)

rhY's number one report from the 2026-11-12 session: **"the whole-world lighting
changes INSTANTLY while running or driving"** — a street that is lit in one frame
and not in the next, a world that goes dark and comes back. Not the p08 blink
(alternating, at one radius, while nearly stationary): this is a **step**, and it is
the largest lighting change the plugin can make.

### Measured, from `scripts/rhyremix.log` lines 3543-5350 (00:48:33 → 01:13:03, 1470 s)

| what | number |
|------|--------|
| handle creates / destroys | 3574 / 3223 (2.43 / 2.19 per second) |
| one-second samples where the live handle count changed | 281 of 1446 (19%) |
| **mass events** (`|Δlive| ≥ 100`, i.e. the whole street layer appearing or vanishing between two samples) | **20** — `00:48:56 1→360`, `00:48:58 360→41`, `00:49:11 363→42`, `00:51:26 381→1`, `01:09:31 1→110`, `01:10:04 33→353`, … |
| per re-selection pass, new lights | `[320, 0, 320, 0, …, 139, 0, 109, 320, 320, …]` — always nothing or the whole set. Never a partial fade. |
| `daylight` (→ `rtx.skyBrightness`) steps > 0.05 | 17 in 10 s samples, **9 of them > 0.5**, five of them the full `0.00 ↔ 1.00` = a **50× ambient step in one frame** |
| sun elevation step between samples | max **104.5 deg**, 22 samples > 5 deg |
| lamps on/off flips | 8 (each one a 320-light step) |
| `rtx:` setting pushes for the whole run | 92 — each one a step, not a ramp |
| finding 3 ("whole world dark on a cab roof") | `live=1` (only the sun left, every lamp destroyed) from **01:09:20 to 01:09:33**, and again at `00:51:26 381→1`; both times the traffic module logged `0 active … 381 awaiting destroy` with `night_only=0`, which is only reachable through the "no player ped" branch |

### Why every one of them is instant

One constraint explains all of it: **`CreateLight` fixes a light's radiance, colour
and direction; there is no way to change a live handle.** So every lighting change in
the plugin is a destroy plus a create, and every path did all of them inside one
frame:

* `destroyAll()` fired on the day/night flip, on the module being off, and on
  `rhyPlayerPos()` returning false — 320 handles gone between two frames.
* Selection ran every `resync_frames=30` frames, so everything that streamed in or
  out over the last half second appeared at once, at full radiance.
* The distance cut-off was a hard edge: a lamp arrived at `max_distance` at full
  radiance and left at `exit_distance` at full radiance.
* A traffic phase change was a destroy + create: green `(0.0018, 1, 0.34)` → red
  `(1, 0, 0)` is a ~3× luminance step in one frame (766 of them this run).
* The sun/moon handle was destroyed the instant its luminance reached 0 and rebuilt
  whenever it moved > 1° or brightened > 10% — each rebuild a hard step.
* `rtx.skyBrightness` and `rtx.emissiveIntensity` were pushed as computed, once a
  second, so every value in the world was a 1 Hz step function; and the game clock
  this run jumped by up to **1372 game-minutes** between samples (9 jumps > 30
  game-min), which stepped the sun, the ambient and the lamp on/off decision
  together.
* `rhyPlayerPos()` returns false whenever the ped pointer at `0xB70153`'s neighbour
  `0xB7CD98` reads 0 — no grace, no last-known position, no vehicle fallback — and
  that false is the trigger for the biggest destroy of all.

### What fixed it

A time layer, `plugin/src/smooth.{h,cpp}`, pure C++ so it runs natively
(`tests/smooth_check.cpp`, 43 checks): per-light fade envelopes whose *published*
gain moves at most `fade step` per handle change; the `max_distance`..`exit_distance`
hysteresis band reused as a **fade band** (`rhyDistanceGain`, returned by
`rhySelectLights`); rate-limited followers for the ambient, exposure and sun
radiance; an angular slew limit for the sun direction; a per-frame budget on handle
changes that counts destroys as well as creates; and `rhyPlayerPosGrace()`, which
holds the last known position for `player_grace_ms` and then *fades* the layers out.
`destroyAll()` no longer exists in either light module.

Two bugs the native test caught that no amount of staring at the game would have:

1. `rhyFadeDue()` and `rhyFadeApply()` disagreed about when a step was due, so a lamp
   whose remaining gap was smaller than one step was never published again — it sat
   at 0.774 of its target forever, and a fade-out never reached dark and never
   retired its handle.
2. Retires did not count against the per-frame budget, so when a fade-out finished,
   every remaining light was destroyed in the same frame: a **42-of-204 drop in total
   lit output in one frame**, i.e. the original bug, reintroduced by the fix.

The bounds are asserted numerically rather than by eye: with the shipped `[fade]`
values, driving at 54 km/h through a 900-lamp field while the street switches on,
off, and loses its player pointer, the **largest one-frame change in the world's
total lit output is 2.1% of itself**, no single lamp moves by more than one step, no
handle is retired from more than one step of brightness, and every layer transition
takes ≥ 0.5 s (measured 0.88-1.07 s).

### What was deliberately left alone

`rtx.restirGI.historyDiscardStrength = 10` (max: the denoiser throws away its
temporal history when lighting changes, so a change lands at full strength the next
frame) and `rtx.localtonemap.shadows = 2.5` (a strong local-contrast boost that
amplifies a step in direct light) both make a lighting step *more* visible. They are
the next lever, but retuning them without an in-game A/B trades a lighting pop for a
denoiser artifact, and the plugin no longer emits a step large enough to trigger the
discard path. `scripts/lighting-test.sh` is the harness if that A/B is run.

## Time of day flickered: two calendars, one staircase (2026-11-12, s29)

rhY's number two report from the same session: **"time of day flickers and does not
progress smoothly"**. s28 had already stopped the *handle* pops (destroy/create bursts,
one-frame layer swaps, 50x ambient steps), and its rate limiters were still doing their
job - which is the useful framing for this one: **s28 smoothed what is published, s29 had
to fix what is sampled.** A rate limiter cannot make a sun that alternates between two
calendars look like one day.

### Measured, from `scripts/rhyremix.log` (all 9 sessions, 409 `sky:` samples at 10 s)

| what | number |
|------|--------|
| wall time per game minute (320 sample pairs) | p50 **2015 ms**, p90 2023, min 1674 - the clock offers one new minute every ~2 s, and the sun moves **0.25 deg** per game minute, so the target was a 0.25 deg staircase |
| geometry pass rate | 1 Hz - *slower* than the 2 s tick, so every other pass re-published a value that had not changed |
| samples whose `day_of_year` came from the game's calendar bytes | **20 of 409 (4.9%)** - the rest used the ini's 186 |
| consecutive-sample pairs with the clock within 12 game minutes but a different `day_of_year` | **31** - sun elevation moved up to **43.4 deg**, median **23.7 deg**, 17 of them over 20 deg, and back again on the next sample |
| example (2026-11-11) | `08:33:05 doy=186 el=+70.8` -> `08:33:15 doy=62 el=+39.2` -> `08:33:25 doy=186 el=+69.0`: **A/B/A in 20 s**, and two of those pairs also moved `daylight` by >0.05 and flipped `lamps=on/off` |
| consecutive-sample pairs where the clock went **backwards** | **51 of ~400** (biggest -1435 game min); 18 jumped forward by >12 min (biggest +1372) |
| the tour's clock pin, 00:49:40 -> 00:51:20 | clock reads 1260,1265,1262,1261,1264,1260,1265,... and the sun elevation wobbles -10.0,-10.8,-10.3,-10.1,-10.6,-10.0 at 10 s |

### Why

`plugin/src/sun.cpp` is the only time-of-day reader in the plugin, and it read the game
bytes directly:

* `gameClock()` loaded two bytes - hour at `0xB70153`, minute at `0xB70152` - and
  `solarPosition()` took **ints**, so the sun was a function of a whole-minute clock.
* `gameDayOfYear()` read `month@0xB70158` / `day@0xB70154` and accepted them whenever
  they *looked* like a date. They are not one: the "month" byte is a counter that walks
  25, 41, 57, ... +16 per sample, and "day" sits at 3. Every time that counter passed
  through 1..12 the sanity check passed and the season became 62 / 246 / 337 / 276 / 34
  instead of 186 - **the flicker is literally two calendars alternating.** (The first
  probe dump of those two words read `0 0`, which is how we know they are not a
  `CTheClock` layout we can trust.)
* hour and minute were two separate loads, so a sample between the game's two writes
  gave `6:59 -> 7:00` as **6:00**: the time walked backwards 59 game minutes and forward
  again. Invisible at a 10 s sampling rate, and exactly a "value goes backwards" case.
* `SARemix_Tour.lua:110,116` calls `setTimeOfDay()` twice per stop (trainers and missions
  do the same), which is the sawtooth above. The plugin re-read whatever byte was there.
* `sun_force_hour` was worse: `gameClock()` wrote the forced hour/minute into the game's
  bytes and read them back in the same call, so the plugin (1 Hz) and the game's own
  clock update (every frame) alternated writes - a self-inflicted A/B/A/B. Not active in
  these runs (`force=-1:-1`).
* there is **no game weather input at all**, and there cannot be a visible one:
  `rtx.ignoreGameDirectionalLights = True` (`config/rtx.conf`) already cuts the game's
  own directional light, so every time-of-day value on screen is one we re-derive. The
  only weather-ish knob is `[sky] turbidity`, which the ini reloader pushed into
  `beamRadiance()` unsmoothed - editing the ini stepped the sun's colour between frames.

### What fixed it

One new pure module, `plugin/src/tod.{h,cpp}` (no `windows.h`, no Remix, no game
pointers, so a whole simulated day can run natively), plus `solarPositionF()` /
`lunarPositionF()` in `plugin/src/solar.cpp`, which take a **float minute of day**:

* **continuous time.** `rhyTodUpdate()` keeps an anchor (whole game minute + tick) and
  publishes `minuteOfDay = anchor + elapsed / msPerGameMinute`, with `msPerGameMinute`
  self-calibrated from the observed ticks (EMA, clamped, seeded from `[clock]
  ms_per_game_minute`). The bodies are evaluated **every frame** at that minute, so the
  sun target is the curve the staircase was sampling. The observer (latitude, season,
  DST) stays a 1 Hz keyframe: it changes on the scale of a game hour.
* **monotonic.** The published time never decreases. A backwards move inside
  `[clock] jitter_tolerance_min` (8) is rejected as pin noise and the time keeps
  advancing; a bigger one is honoured only after it *persists* for `[clock]
  reanchor_ms` (2000), so a tour pin never walks the sun backwards while a deliberate
  jump to 13:00 is still followed. The only allowed decrease is the 1440 wrap, which
  bumps the day index so `absoluteMinute` is monotone.
* **torn reads repaired**, gated on the previous sample's minute field being 59 - the
  shape of an actual roll. Without that gate a deliberate `setTimeOfDay(13,0)` was
  "repaired" an hour off its target, which is worse than the bug.
* **the calendar must earn trust.** `rhyCalendarUpdate()` accepts the bytes only after
  `[clock] calendar_stable_reads` (**30**, i.e. 30 s of a still date) identical reads,
  and only if the day did not step by more than one. A real SA date holds for a whole
  game day (~48 real minutes at 2000 ms per game minute), the measured counter moves
  every ~10 s: the window separates them by ~100x, and the day-step rule is the second
  line of defence. Until it qualifies, the ini's `[sun] day_of_year` is used - one season
  for the session, not two.
* **weather is followed, not pushed.** `RhyWeather` rate-limits turbidity and a new
  `[sky] cloud` (default 0, so the shipped look is unchanged) at `turbidity_per_sec` /
  `cloud_per_sec`; cloud dims the direct beam (x0.10 at full overcast) and gives part of
  it back to the ambient (x1.60). This is also where a real `CWeather` read would be
  plugged in - there is no verified offset for one, and the game's own blend never
  reaches the renderer anyway.
* `sun_force_hour` no longer reads back what it wrote, so the plugin and the game cannot
  alternate.

### Measured, in the test rather than in the game

`tests/tod_check.cpp` (58 checks, wired into `scripts/check-all.sh`) runs a **whole
simulated 24 h day at 60 fps** - 172 810 frames - twice: once with the s28 sampling and
once with s29, with the tour's clock pins and a weather front in both. Everything
downstream of the sampling (the s28 rate limiters, the sky ambient, the lamp decision) is
identical in the two runs, so the difference is the sampling alone:

```
old: sun moves up to  3566.28 deg per game minute | worst frame   29.738 deg |
     morning reversals  5123.50 deg | season changes 2881 | lamp flips 186 | time backwards 4
new: sun moves up to     0.21 deg per game minute | worst frame   23.612 deg |
     morning reversals     0.00 deg | season changes    0 | lamp flips   2 | time backwards 0
```

0.21 deg per game minute is the real sky (the sun's own maximum here is ~0.25 at this
latitude in July). The 23.6 deg "worst frame" in the new row is the tour's *deliberate*
two-hour jump at the end of a stop: the clock follows it and the s28 limiters ramp it,
which is the division of labour the two steps established.

### What was deliberately left alone

The distant-light handle is still rebuilt when the sun moves more than
`[sun] step_deg` (1.0) or changes luminance by 5% - s28's threshold, bounded and
monotone, so it is a fine staircase rather than a flicker (the sun's disc is 0.53 deg;
making the step finer costs one `CreateLight` per body per frame, ~120/s of bridge churn
against the 2.43/s the log measured). And the in-game calendar is still **not used**
unless it behaves like a calendar: with the shipped default it never does, so the season
comes from `rhyremix.ini`. If a future build has a real `CTheClock` date, the counters
`cal: stable=... accepted=... rejected=...` in the 10 s log line are how that gets
noticed.

## Signal heads: a green wash, no bulbs, and a hook that could not install (2026-11-12, s30)

rhY's report: *"the traffic lights are not small bright bulbs, they are ambient
green over the whole street"*. Three things had to be measured before that could
be answered, because the two candidate causes point at different files.

### The hook had not installed since s19, so no run had ever lit a head

`scripts/rhyremix.log`, 2026-11-12 00:48:44, twice:

```
trafficlights: source=game requested but the hook was not installed: the bytes at
0x49DD4C are not 'call 0x6FC580'. Falling back to source=list; ...
```

against an ini that asks for the hook (`config: traffic=1/1 source=game ...`), and
against an exe that is fine: `gta-sa.exe` md5 `85662cbb5aa3df8251ad7664f7a42929`,
bytes at `0x49DD4C` = `E8 2F E8 25 00` = `call 0x6FC580` (PE section walk, and
`tests/test_signal_hook.py` asserts the same bytes). There is no
`signalhook: 0x0049DD4C does not call ...` detail line in the log, which is what
gave it away: that branch never ran.

The cause is in our code, and it is a two-part bug where each half looks fine:

```c
int why = rhySignalPatchCheck(cfg->traffic_source == 1, nullptr, nullptr,
                              nullptr, nullptr);   // "the source gate on its own"
...
if (why == kRhyPatchOk) { /* read the exe, check the bytes */ }
```

and `rhySignalPatchCheck` answered a null-window call with `kRhyPatchCallSite`,
"the call site bytes are wrong". So the source gate answered *bad bytes*, the
byte-reading block was skipped on every build, and the fallback logged a byte
mismatch that had never been checked. `kRhyPatchNoBytes` ("ask again with the
bytes") is now a separate answer, and the install walk is asserted step by step
in `tests/signals_check.cpp` — the old test suite had a check that *pinned* the
wrong behaviour (`"a null byte window is refused rather than read"` expected
`kRhyPatchCallSite`), which is why this survived s19's own review.

Consequence for the whole s01..s19 line of work: `head light ok` appears **zero**
times in that log, `N heads lit` appears zero times, and the hook counters never
print. Every signal light in every screenshot so far has been a list-mode light
at `pole x,y,z + headz`.

> **Corrected by n02, same day.** Two sentences in this section and in
> `plugin/src/signalheads.h` went further than the evidence: that the two runs
> which *did* install the hook (2026-11-11 12:04 and 13:03, `records=0`) showed
> "the patched call site was never reached". They did not. Those log lines carry
> no `calls=` counter at all - s19 added it afterwards - and the 13:04 run's Wine
> trace shows the stub entered with a real `CEntity*` (0x178DEFB4) and head index
> 1, i.e. the call site **was** reached and our own prologue handed the recorder
> garbage. What the 12 seconds at the spawn do show is a distance: 79.1 m to the
> nearest of the 1522 poles, against the game's own 50.0 m corona range, so the
> game was drawing no signal corona there either and `records=0` proves nothing
> about the hook. The health test that used that sentence now asks the distance
> question first; see "Where the lens is" below.

### The wash is mostly the game's own corona, and it is huge

Measuring the tour's two signal stops (`build/tmp/s30/{shotmetrics,greenmap,bulbs,pairs}.py`,
1920×1080, green = `G-max(R,B) > 12` with saturation > 0.45):

| shot | our signal lights | green pixels | brightest green pixel | green AND luma>=200 |
|---|---|---|---|---|
| 12 Nov 00-51-06, stop 06 | 60 live | 815,471 = 39.3% | luma 185, RGB (175,199,71) | **0** |
| 11 Nov 09-07-50, stop 06 | none | 207,503 = 10.0% | luma 191 | **0** |
| 11 Nov 09-08-07, stop 07 | none | 627,200 = 30.2% | luma 190 | **0** |
| 12 Nov 00-51-23, stop 07 | 60 live | 155,359 = 7.5% | luma 198 | **0** |

Two facts do the work. First: **no green pixel in any shot reaches luma 200**,
while the frames' brightest clusters are near-white sodium pools — the green
never concentrates, so there is no bulb to see. Second: the 2026-11-11 tour
created **no traffic lights at all** (hook installed, `records=0`) and still
shows 10-30% green at the same stops with the same framing (the baseline stop
differs by mean |dluma| 1.4 between the two tours). So a large part of the flood
is not ours.

What it is, from the call site we already hook: the game registers every lit head
with `CCoronas::RegisterCorona`, colour (0,255,0)/(255,128,0)/(255,0,0), alpha
0xff, **radius 15.0 game units** and range 50.0. A 30 m additive pure-green
sprite per head, drawn by the Remix bridge as emissive geometry, is ambient by
construction: huge area, moderate radiance per unit area. The radius is an
immediate in the caller:

```
0x49dcd9  68 00 00 70 41   push $0x41700000   ; 15.0f -> argument 18
0x49dcf3  68 00 00 48 42   push $0x42480000   ; 50.0f -> argument 8 (range)
```

so the size of the game's own bulb is one `imm32` at `0x49DCDA`, in the same code
page as the call site — one `VirtualProtect`, two patches.

### Our lights were the second half: 5400 nits in a 0.1 m sphere

```
trafficlights: CreateLight ok #1098 phase 0 ... radiance=9.64,5400.00,1845.57 radius=0.10
```

`5400 = radiance 1.5 × (radius_ref 6.0 / 0.10)²` — the flux-preserving multiplier
from `lamps.h`. That rule is right for a street luminaire, where the 15 m sphere
was a brightness dial and shrinking it must keep the street lit. Applied to a
signal it means "the output of a six-metre blob, squeezed into a marble": sixty
of them near a player light the street, and a light is not visible geometry, so
they still do not read as bulbs.

The bulb model (`rhySignalBulb()`, used by both the head and the list path):
radius = the real lens (0.10 m), `bulb_radiance` = the lens luminance **in nits at
that radius**, no `(ref/r)²`. At the shipped 1200 nits one signal emits 67 flux
units against a mid street luminaire's 3473 — under 5%, which is roughly the
real ratio (a signal lens is ~10² lm, a street luminaire ~10⁴). The old bulb
emitted 304, twice over the bound the test now enforces.

### What is still open, and what the knobs are for

Whether the corona's `radius` argument maps to the sprite's world size 1:1 is
**not yet verified in game** — it is the obvious reading of the disassembly and
the obvious knob, not a measured fact. `corona_radius` is therefore an A/B dial
(clamped 0.05..1.2 m, `0` = leave the game's code alone) and `bulb_radiance`
covers the rest. The safety net is `rhyHeadsHealthy()`: if the hook is live but
has recorded no head within 20 s, the list lights carry the layer instead, so a
shrunken flare can never be left alone in the dark. (n02 made that test
range-aware - a session 79.1 m from the nearest signal keeps the hook, because
the game draws no head corona out there either - and moved the list-mode light
from the mast onto the lens; see "Where the lens is" below.)

## The renderer's own time knobs, measured: which rtx.conf keys decide *when* a change lands (2026-11-12, n01)

s28 named two renderer-side amplifiers and deliberately did not touch them
(`rtx.restirGI.historyDiscardStrength = 10`, `rtx.localtonemap.shadows = 2.5`). n01 did the
measurement, offline and read-only, and the first result is that s28 aimed at a block this
install does not run.

How the columns below were produced: **current** is `config/rtx.conf` (the profile
`tools/rtxprofile.sh current` installs) and `config/rtx.conf.perf`; **runtime default** is
`<game>/rtx.conf.orig`, the runtime's own first-launch file (59 keys; `deploy.sh` copies
whatever `rtx.conf` holds before it ever installs one of ours, and this copy carries none of
the keys we added later - no `hairCardTextures`, no opacity-micromap caps, no water line - so
it is the runtime's own writing). Where a key is absent there, the compiled default is **not**
recoverable offline: this build's `strings` dump states a default for only a handful of
options, and `remix-dxvk.log`'s "Effective RtxOption values" block prints only the keys the
config layers actually set. **known** is the verdict of `tools/rtxkeycheck.py` against
`config/rtx-runtime-keys.txt` (the option-name vocabulary generated from `strings` on
`.trex/d3d9.dll` by `tools/rtx-runtime-keys.py`): `dotted` (the whole path is in the binary),
`leaf+category` (the leaf name and the parent path both are), `leaf` (only the bare name), or
unknown - and an unknown key is silently ignored, which is the failure mode this audit exists
to catch.

Which temporal path is live, from `<game>/rtx-remix/logs/remix-dxvk.log` of the 2026-11-12 run:

```
  87: [RTX Neural Radiance Cache] Selected Ultra preset mode.
3720: RenderPass Integrate Indirect Raytrace Mode: Trace Ray (RGS)
3721: [RTX] Integrate Indirect Mode: Spatially Hashed Radiance Cache - activated
 327: NVIDIA DLSS-NR support query failed (0xbad00010)      -> no Ray Reconstruction
 328: Reflex low-latency device: Not implemented
```

So the integrator is **SHaRC**, not ReSTIR GI, and the whole `rtx.restirGI.*` block the
shipped file carries - including `historyDiscardStrength`, the key s28 called the amplifier -
belongs to an integrator that is not running. `config/rtx.conf.perf` already said so in its
own footer ("the `rtx.restirGI.*` block is carried unchanged but is inert while the indirect
mode is SHaRC"); s28's notes did not carry that forward.

### temporal - keys that change *when* a lighting change lands

| key | ship | runtime default | known | live? | what the runtime's own tooltip says |
|---|---|---|---|---|---|
| `rtx.restirGI.historyDiscardStrength` | 10 | 10 | leaf+category | **inert** | "The sensitivity of discarding history. Higher values discard more history." 10 is the maximum: on a lighting change the whole temporal history is thrown away, so the change lands at full strength the next frame |
| `rtx.restirGI.useAdaptiveTemporalHistory` | False | False | leaf+category | inert | "Adjust temporal history length based on frame rate." |
| `rtx.restirGI.validateVisibilityChange` | True | True | leaf+category | inert | "Remove samples when visibility has changed." |
| `rtx.restirGI.boilingFilterMinThreshold` / `...RemoveReservoirThreshold` | 15 / 30 | 15 / 30 | leaf+category | inert | direct/indirect sample removal thresholds, not time |
| `rtx.di.permutationSamplingNthFrame` | 1 | 1 | leaf+category | **live** | "Apply permutation sampling when (frameIdx % this == 0), 0 means off." 1 = every frame: the direct-light sample set is re-permuted every frame |
| `rtx.di.enableTemporalReuse` / `enableTemporalBiasCorrection` | **absent** | not written | leaf+category | **live** | "Whether to apply temporal reuse." / "Corrects bias caused by temporal reprojection." The RTXDI direct-light resampler is the layer every light this plugin publishes goes through, and neither key was ever written |
| `rtx.demodulate.enableDirectLightBoilingFilter` | False | False | leaf+category | **live** | "Boiling filter removing direct light sample when its luminance is too high." Left False on purpose: every light the plugin publishes is direct light, and tour shots 02-04 are sodium lamps at night - the filter would remove the brightest samples of exactly the good shots |
| `rtx.spatiallyHashedRadianceCache.maxNumFramesToAccumulate` | **absent** | not written (the preset layer sets it) | leaf+category | **live** (SHaRC) | "Maximum number of frames accumulated in each SHaRC entry. Higher values reduce noise but respond more slowly to lighting changes." |
| `rtx.spatiallyHashedRadianceCache.combineUpdateAndQuery` | absent | Medium = on | leaf+category | live | "queries then read the cache resolved in the previous frame, which delays lighting changes by a frame. Medium enables this option. High and Ultra disable it." |
| `rtx.volumetrics.maxAccumulationFrames` | absent | not written | leaf+category | live (`enableVolumetricLighting = True`) | "The number of frames to accumulate volume lighting samples over, maximum of 254. Large values result in greater image stability at the cost of potentially more temporal lag." |
| `rtx.temporalAA.newFrameWeight` | absent | not written | **dotted** | live | "The maximum amount of the current frame to use as part of the temporal anti-aliasing process ... closer to 0 will result in better image stability ... closer to 1 will result in more responsive results (less ghosting)." |
| `rtx.autoExposure.enabled` / `evMinValue` / `autoExposureSpeed` | False / 0 / **absent** | False / 0 / not written | leaf+category | live (disabled) | exposure never adapts, so nothing in the renderer smooths a radiance step for the player's eye; `autoExposureSpeed` ("Average exposure changing speed (in units per second) when the image changes") is nowhere in any profile we ship |
| `rtx.graphicsPreset` | 1 | 1 | leaf+category | **live, and it sits above rtx.conf** | "Overall rendering preset, higher presets result in higher image quality". The preset layer sets the SHaRC/NRC/denoiser temporal keys; a key we write is overridden if the preset owns it, which is why the Remix UI has to be on Custom for some of these to bite |
| `rtx.useDenoiser` | absent (perf: True) | not written | leaf | live | "Denoising is important for filtering the raw noisy ray traced signal ... at the cost of some potential spatial/temporal artifacts (ghosting, boiling, blurring)" |
| `rtx.neeCache.enableModeAfterFirstBounce` | 2 | 2 | leaf+category | live | "NEE Cache enable mode on a second and higher bounces." A cache of direct-light evaluation for indirect rays: how fast a change reaches indirect light. Not changed |
| `rtx.usePostFilter` / `postFilterThreshold` | False / 10 | False / 10 | leaf / leaf | live | "Clamps a pixel when its luminance exceeds x times of the average." Noise control, not a time control |

Names that **look** like keys and are not: `rtx.restirGI.temporalHistoryLength` and
`rtx.autoExposure.evRange`. Both appear in the binary as shader/permutation argument names
(`ToneMappingAutoExposureArgs_natural`, the `reSTIRGI*` permutation list) and
`tools/rtxkeycheck.py` reports them unknown - so writing them into `rtx.conf` is how a
"tuned" setting does nothing for a week. `rtx.accumulation.maxAccumulationFrames` also passes
the audit, but the tooltip sits in the volumetrics/froxel block, so `rtx.volumetrics.` is the
spelling to use.

### look - measured, written down, not shipped changed

`rtx.localtonemap.shadows = 2.5` is a local-contrast boost, so it *amplifies* a step in
direct light; lowering it would visibly flatten the shadow edges of the shots rhY called good
(02-04 sodium lamps, mean 75.4 / 37.0 / 26.3 with 6.5 / 7.9 / 6.6 % black, and 09/10 sunset at
107.6 / 78.2 with 0 % black - `tools/tour-report.py` over that run, `build/tmp/n01/`). It
stays 2.5 in every profile we ship, `nopops` included. Same for `skyBrightness`,
`emissiveIntensity`, `tonemap.finalizeWithACES`, `translucentMaterial.normalIntensity`,
`sceneScale`, `zUp`, `ignoreGameDirectionalLights` and the eight texture-hash lists:
`scripts/rtxprofile-selftest.sh` hashes the lists and asserts the look keys come through
unchanged by the temporal profile.

### what the A/B file changes

`config/rtx.conf.nopops` (from `tools/rtx-nopops.py`, `tools/rtxprofile.sh nopops`) changes
exactly nine keys: the two RTXDI temporal-reuse pair, the SHaRC accumulation window (16), the
volumetric accumulation window (16), the TAA new-frame weight (0.35), the exposure ramp speed
(0.5, inert while exposure is off), and the three inert ReSTIR GI history keys aligned
(10 -> 2, False -> True, True -> False). `tests/test_rtx_nopops.py` pins that set, and
`scripts/rtxprofile-selftest.sh` proves the byte-for-byte round-trip. The A/B is
`scripts/lighting-test.sh`, not the tour: the tour takes every shot at a standstill, so it can
see the look and cannot see a transition.

## Where the lens is: the hook sample, the game's 50 m, and the list's missing 2.10 m (2026-11-12, n02)

rhY's retry of finding 4: *"no light has ever been placed at a lens in any run"*.
True, and the reason s30 gave for it does not survive the numbers. Everything
below is offline evidence from `objdump -d -j .text gta-sa.exe` (md5
`85662cbb5aa3df8251ad7664f7a42929`) and a chunk walk of `models/gta3.img`; the
scripts are `build/tmp/n02/{corona_sites,dffchunks,measure}.py` and the numbers
are pinned in `tests/test_signal_hook.py::TestSignalDistanceInRealExe`.

**Every call to the corona thunk.** 28 call sites call `0x6FC580`. Exactly one of
them, **0x49DD4C**, is inside `CTrafficLights::DisplayActualLight` (0x49DAB0,
2928 bytes, five callers at 0x536987/0x5369D3/0x536A0D/0x536A68/0x536B5A, all
inside the renderer function that starts at 0x535FA0 and dispatches on
`movswl 0x22(%esi)` = `CEntity::m_model_index`). The other 27 sit in 16 other
functions (0x455720, 0x455E20, 0x4560E0, 0x47E070, 0x48B570, 0x53AF00, 0x6AAB50,
0x6B9250, 0x6C58E0, 0x6DD130, 0x6E0A50, 0x6E1780, 0x6E1A60, 0x7174F0, 0x73A530),
which is why patching the thunk itself would have lit police beacons, flares and
every other corona in the game.

**The 50.0 m range is applied inside the callee, after our patch point.** The
call site pushes `0x42480000` (50.0f) at **0x49DCF3** as argument 8. The real
corona function tests it at `0x6FC22E`: `fld [esp+0x4c]; fmul [esp+0x4c];
fcompp; fnstsw; test ah,0x5; jnp 0x6FC4BB` - squared distance against squared
range, give up beyond it. Then it fades: full alpha below **35.0 m** (the float
at 0x85F070), zero at **50.0 m** (0x858B40), `1/15` per metre between them
(0x863E0C). All of that runs *after* the instruction we detour, so the range
never gated our recorder - it gates what the **game** draws. That is the fact
that turns the 2026-11-11 samples inside out: the player was **79.1 m** from the
nearest of the 1522 poles in both runs, so the game was drawing no signal-head
corona at all, and `records=0` there is what the game's own range predicts. The
places where the game's range does allow a head are the tour's signal stops
(CJ at 1040,-1392 and 1068,-1428: four signals inside 50 m, nearest 14.4 m and
13.5 m), and no run has ever logged a head there. `rhyHeadsHealthy()` now takes
the nearest-signal distance and this range, and only falls back to the list when
a signal is inside the range, the grace has run out and still nothing appeared;
a signal-free session keeps the hook and logs why.

**Whether a head exists at all is the model's 2DFX count.** `0x49DBDD
movzx eax,BYTE PTR [edi+0xd]` (CModelInfo+0xD) ... `0x49DC01 jle 0x49DE2C`, and
0x49DE2C is past the corona call: a model with no 2DFX entries never reaches
0x49DD4C. Scanning every entry of `models/gta3.img` (16316 entries: 12964 `.dff`,
164 `.ipl`) finds **no** chunk with id `0x00010105` anywhere in it, and the exe
contains no ASCII `2dFx` magic (0 hits in the whole file), so the 2DFX records
are not readable out of the game files - which is what the 13:04 crash's head
index of 1 makes interesting (the loop did run, at least twice, for one of the
five model ids this function accepts) and what makes chasing them the wrong task.
The list file has to carry the lens itself.

> **Corrected by n11 (2026-11-14).** The two scans are right and the conclusion is
> wrong: SA does not use the GTA3 `2dFx` chunk id `0x00010105`, it uses its own,
> **`0x0253F2F8`**, and the records are in **1591 of the 12239 `.dff`** of this
> archive. `tools/dff_2dfx.py` reads them (the record list closes byte-for-byte at
> the end of the chunk in all 1591), and the lens positions they carry agree with
> the lamp table to 0.1-0.2 m - `cj_traffic_light4`'s three lens records are at
> (-3.24,-0.05,5.62/5.37/5.10) in red/amber/green, and the game's own loop at
> `0x49DC75` reads those colour bytes as the phase index. Chasing them was not the
> wrong task; it was the task that turned the street lamp list from a 22-model
> table into the game's own 18691 lights. See "Every lamp the game declares" below.

**It was carrying a pole.** The lamp table (`moonloader/SARemix_Light_Manager.dat`,
the same file the shipped `headz` column already comes from) gives a *three*
number head offset per model, and the v1 file kept only the third:

| model id | name | .ide radius | head offset, model-local | instances |
|---|---|---|---|---|
| 1350 | cj_traffic_light4 | 60 | (-2.10, +0.10, +7.60) | 720 |
| 1351 | cj_traffic_light5 | 60 | (-1.75, 0.00, +6.00) | 61 |
| 3855 | gay_traffic_light | 50 | (-2.10, +0.12, +7.65) | 23 |
| 1352 | cj_traffic_light3 | 60 | borrowed from 1350 (radius tie) | 20 |
| 1315 | trafficlight1 | 64 | borrowed from 1350 (gap 4) | 136 |
| 1283/1284 | mtraffic1/mtraffic2 | 85 | refused: radius gap 25 | 510/53 |

So the lens sits **1.75-2.10 m sideways** of the mast for 960 of the 1522
instances (63%), and a light at `pole + (0,0,headz)` was that far from the lens
it is supposed to be a bulb on. The direction is available and exact: all 1524
raw traffic instances in the "bnry" IPLs carry a **pure Z-axis quaternion**
(measured: x = y = 0 and |q| = 1 for every one), so a yaw is the whole rotation,
and rotating the offset by it is the same rule the game applies to a 2DFX entry
at 0x59C890 (`out = m->right*e.x + m->up*e.y + m->at*e.z + m->pos`).

**What shipped.** `# rhyremix traffic v2`, eight columns:

```
x y z headz heading lensx lensy lensz
```

`heading` is the instance yaw in degrees, `lens*` the head offset in the model's
own frame; the plugin rotates and adds (`rhySignalLensPos`, `lens_offset=1`).
Column 4 is still `headz` and still equals `lensz`, and `--v1` writes the old
four-column file: regenerating with `--v1` reproduces the deployed
`scripts/rhyremix_traffic.txt` **byte for byte**, which is the compatibility
proof in `tests/test_traffic_list.py::TestGeneratedFile`. A v1 line parsed by the
new plugin gets `heading 0, lens (0,0,headz)` and lands exactly where it landed
before. mtraffic1/2 (562 instances) keep the pole x/y because borrowing a 2 m arm
across a 25-unit radius gap is not justified; they keep the borrowed height, as
before.

**One stale gate.** `scripts/check.sh` line 45 asserts
`awk '!/^#/ && NF!=4'` on the generated traffic list, i.e. the v1 column count,
and now reports `FAIL 1522 malformed traffic lines` for the eight-column file.
Rule 4 of the header says never edit `scripts/check.sh`, so it is recorded here
instead: the assertion needs `NF!=4 && NF!=8`, and `scripts/check-all.sh` - the
gate this task's Gate line names - passes.

## The glow was wanted: what s30 removed and n02b puts back (2026-11-12, n02b)

rhY's correction, 2026-11-12 08:50, on the wording of his own finding 4:

> "We wanted the glow from the traffic light and the bulb. It should be both
> like real life."

The 2026-11-12 report line had been read as "no green street flood", and s30
acted on that reading. The glow rhY saw at the signal stops is not the defect;
the missing bulbs are. So the question for n02b was not "how dim is the signal
now" but "what exactly did the two commits take away, and what did the 00:48
glow measure?"

### What the 00:48 run had, and what each commit did to it

Two glows were on screen at every signal. Measured, with the file:line of each
number (the 00:48 build is the s29 tree, `9e67d91`; s30 is `bd7fdf4`, n02 is
`ee59b87`):

| | 2026-11-12 00:48 | s30 | n02 (HEAD before n02b) |
|---|---|---|---|
| our light, radius | 0.10 m (`9e67d91:trafficlights.cpp:375-377`) | 0.10 m, `bulb_radius` added | 0.10 m |
| our light, radiance | 5400 nits (`:379-380`, `rhyRadianceForFlux(1.5, 0.10, 6.0)`) | **1200 nits** (`signalheads.cpp:391-396`) | 1200 |
| our light, flux (green) | **303.9** (amber 269.2, red 226.2) | **67.5** — a 4.5× cut | 67.5 |
| our light, colour | `rhySignalLensRgb` passbands | same | same |
| our light, position | pole `x,y,z+headz` (`:402-404`) | pole | **the lens**, up to 2.10 m sideways |
| lights created per signal | 1 | 1 | 1 |
| the game's corona radius | **15.0** game units (imm32 at `0x49DCDA`) | **0.45** (`signalhook.cpp:278-330`) | 0.45 |
| the game's corona area | 1× | **÷1111** | ÷1111 |

So s30 cut our light's output by 4.5 and the game's flare by three orders of
magnitude in area, and n02 moved the remaining light onto the lens. Neither ever
added a second light. A 0.10 m / 1200-nit / flux-67 sphere is a lens you can look
at and it colours nothing: a point light is not visible geometry, it only lights
what is near it. That is the "bulb, no glow" half of the complaint, and it is the
mirror image of the "glow, no bulb" rhY had reported.

### The numbers the glow is built from

* Reference strength: the flux our own light had at 00:48 — **304** (green; 269
  amber, 226 red), `4π(0.10)²·mean(lens colour × 5400 nits)`. That is the look
  rhY is asking to keep, so it is the default, not a value picked by eye.
* Ceiling: one mid street luminaire, the same `4πr²·mean(radiance)` convention.
  With the shipped ini (`radius_ref=15`, `radiance_big=2.0`, `kelt_warm=2100`,
  `kelt_cold=5000`, `rhyStreetFixture(1)` r=0.15) that is **2413** warm, **4510**
  cold, **3462** for the warm+cold mix the street actually carries — s30's 3473.
  304 is 8.8 % of it, so the reference strength needs no take-down; the cap is
  there for a hand-edited `glow_flux`.
* Shape: "spill a few metres, strongest near the head" → a **3.0 m** sphere at
  the lens, radiance solved *from* the flux (`base = flux / (4πr²·mean(colour))`
  = 6.0 nits green at 3 m, against the bulb's 1200 at 0.10 m). Same flux as
  2026-11-12 over 900× the area: soft, near-field, and it never out-drives the
  sodium lamp over the intersection.
* Solving for flux rather than radiance also means the three phases carry the
  same flux (the old light did not: 304/269/226, because its radiance was
  constant). The spill under a signal therefore changes colour through the cycle,
  not strength.

The game's own flare stays at 0.45 (that is what makes the head read as a bulb);
the ambient is now ours, a few metres wide, the same colour as the lens, cycling
with it. `glow_flux=0` is the A/B back to bulb-only.

## Lights that came and went with the camera: a count that was a distance rule, a corona set that was the frustum, and a 400-light lottery (2026-11-12, n04)

rhY's finding A, and the one he put first:

> "Lights flicker in and out of existence when the camera ROTATES. Facing a light
> it is off, facing away it comes on suddenly. Walking/driving down the pier,
> lights switch on as he approaches. Street/pier lamps must be on ALL the time
> (the whole streamed area), never culled by view direction, never popping with
> distance."   (`.strata/test-2026-11-12b.md`, finding A)

The sentence has three claims in it and they have three different causes. Answering
it needed the runtime's own source, because every key name in this renderer lies:
`rtx.antiCulling.light` sounds like the switch for "off-screen lights still light"
and, for the lights this plugin makes, it does nothing at all.

### 1. What the 10:22-10:45 run (the run finding A was written from) says

`<game>/scripts/rhyremix.log` lines 5352-6673: 2026-11-12 10:22:46 → 10:39:05,
970 s, 972 one-second `remix: live=` samples, 98 ten-second module lines.

| measure | value |
|---|---|
| bridge live lights | min 1, max 629; pinned at 401-415 for minutes while the street layer was on |
| street `active` per 10 s | 0 or 320 for most of the run; 8 jumps ≥ 20 handles: ±320 at the day/night flips, **+193/−193 at 10:25:04**, −320, **+136/+221/−357 at 10:26:54-10:27:14**, +320 at twilight |
| `kept=320 new=0` | in 70 of the 98 samples: with the layer on and the player standing, the set was already stable |
| street handle traffic | `steps` (fade re-creates) cumulative **42** for the whole run |
| **10:33:35-10:39:05** — rhY walking the pier at night, `lamps=on` | `active` 320 constant, `new=0`, `created=0`, no retire: **our street handles did not change at all while he was watching them flicker** |
| signal heads | `heads lit` 0..19 (mean 4.3), `coronas registered this frame` 0..12, `colour_swaps=850`, `expired=790`, `retired=817`, `steps=5669` → **5.8 handle changes a second** |

Two rows decide the shape of the fix. The street layer was already steady in the
plugin during the minutes he calls unsteady — so the pier symptom is not our
selection. And the signal lights were churning six times a second — that one *is*
ours, and it is the only light in the plugin that has ever looked at the camera.

### 2. The inventory: every place a light can be skipped, destroyed or dimmed

| # | mechanism | ours? |
|---|---|---|
| A1 | **the count was a distance rule.** `rhySelectLights` sorted every candidate by distance from the player and kept `cap + sticky_extra` of them, so a lamp inside `max_distance` lost its light when the 320 nearest changed. Measured over the whole 7002-lamp list: **408 lamps within 300 units** of a point at the worst place on the map, 552 within 380 (median 149 / 234) — against a shipped cap of 320+90 = 410 | yes, `smooth.cpp` |
| A2 | re-selection every 30 frames, so an eviction arrives as a batch of hundreds | yes |
| A3 | **every gain step is destroy+create of the same hash.** The runtime keys API lights by handle (`rtx_light_manager.cpp:795`) and gives a new light `kNewLightIdx` and a fresh identity (`:392`, `:445-457`), so its RTXDI reservoirs and denoiser history restart from zero. A re-create is a pop by itself, whatever the radiance step | yes, and unavoidable: radiance is baked at `CreateLight` |
| A4 | **a signal head existed only while the game registered a corona for it.** The game registers a corona from its renderer, so the registered set *is* the camera frustum; `head_ttl_ms=2000` then faded the bulb and glow out and retired them. Turn your back on a junction: dark 2 s later; look back: created again | **yes — the only true view-direction light** |
| A5 | `min_distance` pinned a lamp's gain to 0 (a re-create under CJ's feet). Default 0, inert | yes |
| A6 | layer off (day, disabled, bridge refused, player pointer gone) → whole-layer fade-out. s28's half, already ramps | yes |
| A7 | **no camera-direction selection exists anywhere in the plugin**: selection receives only the player ped position, never the camera; no frustum test, no screen-size sort, no "N most visible". So "facing away it comes on" cannot be our selection | — |

And in the runtime (source in `build/tmp/s18/dxvk-remix-remix-1.6.1/`):

* **An API light is never frustum-culled.** `LightManager::garbageCollection()` marks
  inside/outside frustum only for `getLightTable()`, the *game-detected* lights
  (`rtx_light_manager.cpp:151-183`), and `garbageCollectionInternal()` erases only
  from `m_lights` / `m_externallyTrackedLights` (`:110-141`). Lights made by
  `CreateLight` live in `m_externalLights` and disappear only on `DestroyLight`
  (`:795-810`); their per-frame presence is `addExternalLightInstance()` = our
  `DrawLightInstance` (`:857-865`). **For our lamps "off-screen lights still light"
  was already true.** What is view-dependent is not their existence but whether a
  pixel ever draws one.
* That is a sampling question, and the numbers are small: `rtx.di.initialSampleCount`
  = **4** — "the number of lights randomly selected from the global pool to consider
  when selecting a light with RTXDI" — over a pool of 401 live lights. About a 1%
  lottery per lamp per frame, held together by reuse with
  `rtx.di.maxHistoryLength` = **4** frames. Rotate the camera, disocclude, the
  reservoirs die, the street has to be re-found by the lottery. The same lottery
  runs for indirect rays (`rtx.risLightSampleCount` = 7) and for volumetrics
  (`initialRISSampleCount` = 32, reservoir clamped to
  `temporalReuseMaxSampleCount` = **2**).
* `rtx.di.permutationSamplingNthFrame`: runtime default **0**, our shipped
  `config/rtx.conf` set **1** — "apply permutation sampling when
  (frameIdx % this == 0)": a per-frame re-randomisation of which lights a pixel
  draws. n01 left it alone calling it a noise knob; over a 400-lamp pool it is
  finding A.
* The runtime's own view levers, all off by default: `rtx.antiCulling.object`
  ("extends lifetime of objects that go outside the camera frustum"),
  `rtx.antiCulling.light`, `numFramesToExtendLightLifetime` 1000, `fovScale` 1,
  `farPlaneScale` 10, `enableInfinityFarFrustum` false. They apply to game lights
  and instances — but the lamp-post *geometry* does leave the TLAS with the camera,
  which changes the shadows and visibility of an off-screen lamp.
* `rtx.volumetrics.restirGridGuardBandFactor` = 1.1: "an extended part of the
  viewing frustum for which we should calculate lighting information for, even
  though they are technically offscreen. This helps reduce noise in cases where the
  camera is moving around." The runtime's own answer to the complaint.
* Two s28 key notes were wrong and are corrected here: `rtx.enableCulling` is
  front/back-face culling of opaque objects, not frustum culling; and
  `rtx.uniqueObjectDistance` defaults to **300**, not 1000, and is "how far an
  object may move in one frame and still be the same object", not a view distance.

### 3. The fix

**Ours**

* `rhySelectLights(..., float hardRadius)`: a lamp strictly inside `hardRadius` is
  taken before the cap is consulted at all, so **the cap may only trim the band
  between `max_distance` and `exit_distance`**. Both lamp modules pass
  `max_distance`. `hardRadius = 0` keeps the old behaviour, which the tests keep as
  a control.
* The shipped counts now cover the radius instead of fighting it:
  `max_lights 320 → 560`, `sticky_extra 90 → 140` (560+140 = 700 ≥ the 552-lamp
  worst case inside `exit_distance`), signal `max_lights 60 → 90` (≥ the 150
  worst case inside `traffic_exit_distance`). Radii unchanged: 300/380 and 250/300.
* Signal heads are kept by **player distance**, not by corona registration: the
  records are folded into a memory (position + last colour + last tick), a head
  that went stale keeps its light with the colour it last showed — invisible by
  definition, and the frame the player looks back the game re-registers it and the
  cross-fade takes over — and it is dropped only beyond `exit_distance`
  (`rhyHeadKeepAlive`). `head_ttl_ms` is a staleness marker now, not a destroy
  trigger.
* Two field counters, printed in the 10-second line: `in_drop=N` on both layers is
  the number of lamps inside `max_distance` that a re-selection left without a
  light. It should stay 0 for a whole run. And at load the street module prints the
  worst case it can find in the real file:
  `streetlights: worst case 408 lamps within max_distance=300 of a point (cap
  max_lights+sticky_extra=700): the cap covers the radius`.

**The renderer** (`config/rtx.conf`, mirrored cheaper in `config/rtx.conf.perf`;
every key verified against `config/rtx-runtime-keys.txt` by `tools/rtxkeycheck.py`):

| key | was | now | why |
|---|---|---|---|
| `rtx.di.permutationSamplingNthFrame` | 1 | **0** | stop re-drawing which lights exist at a pixel, every frame |
| `rtx.di.initialSampleCount` | 4 (default) | **8** | the pool is 400-550 lamps; 4 candidates is a lottery |
| `rtx.di.maxHistoryLength` | 4 (default) | **12** | a lamp's direct light survives a camera rotation |
| `rtx.risLightSampleCount` | 7 (default) | **16** | the same lottery for indirect rays — finding D's "no illumination from the bulb" |
| `rtx.volumetrics.temporalReuseMaxSampleCount` | 2 (default) | **8** | a light shaft carried by two samples boils |
| `rtx.volumetrics.restirGridGuardBandFactor` | 1.1 (default) | **1.5** | light the off-screen band, the runtime's own advice |
| `rtx.antiCulling.object` | False (default) | **True** | keep lamp posts and gantries in the TLAS when off screen, so an off-screen lamp's light does not leak and the post does not pop |
| `rtx.antiCulling.light` | False (default) | **True** | same for the game's own converted lights (ours were never culled) |
| `rtx.antiCulling.light.numFramesToExtendLightLifetime` | 1000 (default) | 1000, written out | pins the A/B; the tooltip warns against setting it low |

The pool got bigger and the sampling had to keep up: **`max_lights` and
`rtx.di.initialSampleCount` move together.** That coupling is written in both the
ini and the profile, because either one alone makes the picture worse — a bigger
pool with 4 samples dilutes every lamp, and more samples against a cap that
evicts lamps buys samples for lights that are not there.

Not set, with the reason: `rtx.antiCulling.object.enableInfinityFarFrustum` (keeps
the whole city in the TLAS — a VRAM cost nobody has measured here),
`farPlaneScale`/`fovScale` (the cheaper version of the same frustum, left at
defaults), `rtx.enableCulling` (wrong key, see above).

### 4. How it is tested

`tests/always_on_check.cpp`, run by `scripts/check-all.sh` as `nat-alwayson`
(35 checks). The camera is given a role on purpose, and the assertion is that the
light set does not notice:

* **a 360° camera sweep** with 624 lamps all around the player: after settling,
  **0 handle changes in 10 s** and the wanted set identical from pass to pass. The
  control in the same test prints what a 60° frustum rule would have re-decided
  (207 of those lamps), so the test is known to be able to see churn.
* **a 300 s walk at 1.4 m/s and a 240 s drive at 15 m/s** through a field built to
  the measured worst point (421 lamps within 300, 549 within 380): **no lamp inside
  `max_distance` is ever destroyed**, none is created there after the initial fill,
  and a lamp that has settled at full gain is **never touched again** — 0 events in
  300 s of walking.
* **the cap rule**: 500 near + 200 band lamps with a cap of 100 → the n04 rule
  keeps all 500, the old rule kept 100.
* **a settled lamp is one handle**: 60 s standing still, 175 live lamps, 175
  creates, and 0 handle changes in the last 55 s.
* **heads**: stale+near is kept, stale+far is dropped, staleness alone is never a
  reason to destroy a light, and a head cap of 20 over 200 heads inside the radius
  keeps all 200 (the old rule kept 20).

What the test does *not* claim: that a lamp crossing from the fade band into the
radius never has its handle re-created. It does, several times, because radiance is
baked at `CreateLight` and a ramp is a sequence of re-creates — that is s28's
anti-pop design and `tests/smooth_check.cpp` pins it. The n04 guarantee is the
narrower one: nothing inside the radius is created, destroyed, or touched after it
has settled.

### 5. Still open

* The rtx.conf half is a source-read plus the runtime's own tooltips, not a
  screenshot A/B. `scripts/lighting-test.sh` cannot see it (every shot is at a
  standstill) and neither can `scripts/abtest-lights.sh` (it swaps `rhyremix.ini`,
  not `rtx.conf`). The field test for finding A is the new `in_drop=0` in the log
  plus a scripted camera rotation, which the pack does not have yet.
* `max_lights` 560 means up to ~700 live lights against the 401 the 10:22 run
  carried. The fps cost is unmeasured; `config/rtx.conf.perf` is the conservative
  variant and the ini comment says to lower `max_distance`, not the cap.
* A head we cannot see keeps the colour it last showed. If the game changes a
  signal's phase while it is off screen, our light is wrong until the player looks
  back — invisible, but it is a lie, and the list-mode clock phase
  (`rhyTrafficPhase`) is the honest alternative if it ever matters.

## The game hung on Quit (2026-11-12, n09 + n09b)

`build/tmp/exit-hang-20261112.txt` is the first event, sampled by hand at 10:40 while it was
happening. Read as a timeline it says more than it looked like at the time:

```
10:38:55 … 10:39:14.652   rhyremix.log: "remix: live=383 …" every second; streetlights
                          "302 active, 302 drawn this frame"; player_gone_frames=693
10:39:14.652              LAST plugin line. hkPresent is never entered again.
10:39:14.989 / .997       moonloader: "Unloading…" / "Session terminated."  ← the game's exit path
10:39:15.011              remix-dxvk.log: err: D3D9SwapChainEx: Failed to recreate swap chain
10:39:15 → 10:41           gta-sa.exe alive, main thread 7716 state R 66 % CPU, window gone,
                           NvRemixBridge.exe already exited, nothing further in any log.
```

* **What it is not.** Not our init thread (it had returned; it is bounded at 120 × 100 ms and joins
  nothing — there is no `WaitForSingleObject`, no `std::thread` and no `atexit` anywhere in the
  plugin). Not a MoonLoader script loop: `SARemix_Shot.lua`'s `while true do wait(0) end` yields to
  the game and MoonLoader terminated every script 300 ms *before* the spin started. Not our `Present`
  hook looping either: our own once-a-second line stops at 10:39:14.652, and the game went on to run
  its own shutdown (unloading MoonLoader) after that, so the main thread was in game code, not stuck
  inside our hook.
* **What it is.** The exit path after the swap chain could not be recreated: `IDirect3DDevice9::Reset`
  (vtable 16 — the one device call in that path we do not wrap) failing against a window that Quit
  already destroyed, retried forever in user space. An ASI that never gets another `Present` cannot
  end that loop.
* **What was still ours to fix.** Until the latch added in n09, the plugin kept drawing 302 street
  lights and 4 signal lights per frame into the bridge client `d3d9.dll` whose server process had
  exited, kept the `0x49DD4C` code patch live through the game's model teardown, and logged nothing:
  there was no `DLL_PROCESS_DETACH` branch at all, which is why "no shutdown line at all" was the
  headline of the report. Detection is now `plugin/src/shutdown.cpp` (Present failures,
  `TestCooperativeLevel`, `IsWindow()` on the HWND from `GetCreationParameters`, our own failing
  call counters — each with a persistence window, because alt-tab looks like two of them), the latch
  is `rhyRemixShutdown()` in `remix.cpp`, and the log says `shutdown: suspect` →
  `shutdown: bridge gone (…)` → `shutdown: still running N ms …` → `shutdown: DLL_PROCESS_DETACH …`.
* **The part an in-process tracker can never do.** If the game stops calling `Present`, nothing of
  ours runs again — so the *outside* half has to end the run: `scripts/test-session.sh`'s watchdog
  (n09) and, since n09b, `launcher/launch-rtx.sh`'s own cleanup. A `gta-sa.exe` still running 20 s
  after its bridge process or its window is gone gets logged, evidenced (the same thread/log dump as
  the hand-made file above, into `build/tmp/exit-hang-<stamp>.txt` or `<game>/logs/exit-hang-<stamp>.txt`)
  and killed.

### The second hang, 11:48: the bridge never noticed the client had left (n09b)

The same Quit, started straight from `launcher/launch-rtx.sh`, `build/tmp/exit-hang-20261112-b.txt`:

```
11:51:07 … 11:51:10.018   rhyremix.log: "remix: live=401 …" every second, 320 street lights drawn
11:51:10.521              bridge64.log: "Client window became inactive, disabling timeouts for
                          bridge server..."      ← the last line the bridge ever wrote
11:51 → 12:07 (16 min)    gta-sa.exe 18 % CPU AND NvRemixBridge.exe 406 % CPU: two geometry-proces
                          threads and two main threads at 100 % each, dxvk-cs 11 %, 3.6 GB of VRAM
                          held; the launcher still sitting in `waitforexitandrun`, so nothing cleaned up
```

* **Why the bridge did not exit on its own.** `NvRemixBridge.exe` registers an exit callback on the
  client process (`RegisterExitCallback()`, and its own log says "The client process has unexpectedly
  exited, shutting down server as well!") — that only fires when the client *process* exits, and the
  client process is the spinning game. Its other liveness mechanism is the IPC timeout, and the
  window going inactive switches that off: `bridge.defaults.conf` documents `disableTimeouts`
  (default True) as "used internally for when the game window becomes inactive because the user
  switches to another process", and `infiniteRetries` the same way. Both are flipped **at runtime**,
  and the file says of both: "this setting may be changed dynamically at runtime based on application
  state, therefore changing the default in the settings does not have much of an effect overall".
  So there is a `bridge.conf` option that *asks* for timeouts to stay on
  (`disableTimeouts = False`, `infiniteRetries = False`; `launcher/launch-rtx.sh`
  `RHYS_BRIDGE_TIMEOUTS=1` writes them and backs the file up), but the runtime's own documentation
  says it will override them when the window goes inactive — which is exactly the moment we need
  them. **There is no bridge.conf setting that reliably keeps the timeout on for a destroyed window.**
  `server.shutdownTimeout`/`server.shutdownRetries` only bound the bridge's *graceful* shutdown, and
  `commandTimeout`/`commandRetries` (1000 ms × 300 = 5 min) are what `disableTimeouts` overrides.
* **So the launcher ends the tree.** `launcher/launch-rtx.sh` waits for `gta-sa.exe`, which is the
  process that never exits, so its own cleanup could never run. It now has
  `end_wine_tree` (SIGTERM/SIGKILL `NvRemixBridge.exe` by pid, then Proton's
  `files/bin/wineserver -k` with `WINEPREFIX=<compatdata>/pfx`, which is what takes `gta-sa.exe` with
  it, then a sweep by name in case the wineserver was not found) called **after** the wait returns,
  plus `watch_run_end` running **during** it: the game's window destroyed (armed only after the
  window was seen, and `wmctrl` lists minimised windows so alt-tab is not "gone"), or
  `gta-sa.exe` gone while `NvRemixBridge.exe` lives, for `RHYS_EXIT_GRACE` s (default 20) → log,
  evidence file, end the tree. `scripts/launch-cleanup-selftest.sh` drives those functions against
  stub processes (33 checks: the cleanup, the nothing-to-exit case, the window-gone hang, a live
  window that must never be touched, no-probe-must-not-guess, and the bridge-outliving-the-game case).
* **Still open, and how to settle it.** Whether the CPU was burning inside the bridge client's
  `Present`, inside the game's reset loop, or inside the bridge server's geometry threads (the 11:48
  sample says the bridge was the busiest by far, and it had no client to serve). Two switches answer
  it: `[shutdown] stop_present_after_ms=1500` stops forwarding `Present` once the bridge has been
  gone that long — if the game exits with that set, the spin was in the bridge client; and
  `RHYS_BRIDGE_TIMEOUTS=1`, which tests whether the config default survives the runtime's own flip.
  Either way the launcher now ends the run in 20 s instead of rhY waiting sixteen minutes.

## Water and foliage, read from the other RTX Remix projects (2026-11-12, n07)

rhY's report (`.strata/test-2026-11-12b.md`) said the water "still looks bad, edges flicker" even
with the s32 sheet, and asked how the other Remix projects do water and foliage. The research log
with every URL is `.strata/notes-n07-water-research.md`; this is the part that changes behaviour.

### The edges: measured, and it is not what the GTA IV project fights

`$G/data/water.dat` is a text file (first line `processed`, then one water polygon per line, four
corners x seven floats). Parsed: **307 polygons (301 quads + 6 triangles), 5969 pairs that butt
exactly edge-to-edge, and only 2 pairs that overlap at all** (`water1.dat`: 267 polygons, 4586
seams, 1 overlap). So SA's sea is not GTA IV's stack of coincident quads — xoxor4d's GTA IV RTX
comments on "the water quads the game stacks 0.2 mm apart (water.dat, some piers) stay coincident"
and fixes that with a depth-probe shoreline fade; ours is one coplanar sheet cut into exactly-butted
polygons. A ray that lands on a shared edge can hit neither polygon — the failure gta4-rtx's own
water API names ("a single probe can slip through cracks and T-junctions between separate meshes") —
and a perfect-mirror translucent lobe (the mdl has no roughness input) turns that into a pixel
flipping between "sea" and "whatever is under the sea". The pack's sheet (median facet tilt 44 deg,
21.3 deg/texel gradient) put a maximum-contrast facet on every one of those seams.

Stock 1.6.1 has no key that widens a triangle's hit tolerance and no depth-based shoreline fade, so
what shipped is the contrast reduction: a 6 deg sheet, the runtime's second LOD-blurred normal on
both water surfaces, temporal history kept, and `rtx.rayreconstruction.combineSpecularAlbedo = True`
(NVIDIA's own line in the Path-traced Minecraft Remix mod: "without it, shaded water reflects blotchy
sky-coloured smears instead of the scene"). The real fixes — a fork runtime with `rtx.water.shore*`,
or one rebuilt water mesh — are out of reach here, and saying so is part of the finding.

### What the projects that ship water actually do

| project | the part that matters |
|---|---|
| NVIDIA `rtx-remix-minecraft` (the only NVIDIA mod with readable water code) | translucent via the Remix API (IOR 1.33, transmittance 0.9/0.95/1.0 @ 10 m, thin wall off); **generates** the wave sheet (12x10 flipbook, 20 fps, one frame = 10 m, `slope_rms 0.0325` ≈ 2.2 deg median tilt, BC5 + full mips, octahedral with +Y down the image); underwater fog is a separate volumetric config; `combineSpecularAlbedo = true`; opacity micromaps off for a high-volume terrain workload |
| `xoxor4d/gta4-rtx` (GTA IV) | water rebuilt every frame as one Remix-API mesh; default material IOR 1.33, transmittance **(0.62, 0.8, 0.8) @ 4 m**; shoreline fade in a **fork** runtime (`rtx.water.shoreCutDepth 0.02`, `shoreFadeWidth 0.1`, `shoreHeightScale 4`, `shoreFadeDistance 10`) |
| `gebdag/gta2-rtx-remix` (GTA 2) | a generated 12x10 @ 30 fps flipbook over a 10 m tile, BC5 octahedral; **world scale set by the mesh's UV density** (one repeat per 10 m), not by the material; transmittance (0.45,0.75,0.55) @ 1 m, because they author a sand riverbed for the water to absorb against; all twelve animated tile hashes get the same material |
| `xoxor4d/p2-rtx` (Portal 2) | stock runtime; `enableStochasticAlphaBlend = True` fully tuned, `forceCutoutAlpha = 0.5`, `hairCardTextures`, opacity micromaps heavily budgeted, per-map `triangle_cull_on/off.conf` |
| `Ekozmaster/NFSU2-RTX-Remix` | turns the runtime's layered water **off** (`opaqueMaterial.layeredWaterNormalEnable = False`) and uses a negative normal strength (-0.108) |

The runtime side of the same story, from the 1.6.1 source: `translucent_surface_material_interaction.slangh:62`
gates the whole water path on `animatedWaterEnable && surface.isAnimatedWater`, and lines 73-83 show
the two mechanisms — the sprite-sheet cell's UV scrolled by `animatedWaterPrimaryNormalMotion`, and a
second normal sample at `animatedWaterSecondaryNormalLodBias` ("blurred, resulting in lower frequency
waves"). A water surface that is not in `rtx.animatedWaterTextures` gets none of it: the sea hash
`0x59296793798A7318` was in the list, the game's other water texture `waterclear256`
(`0xA75A0B41F2454F04`) was not.

### Foliage: what the research says we do not need, and what it says to pin

Two-sided foliage needs no key: `rtx_options.h:933` — "Enable front/backface culling for opaque
objects. Objects with alpha blend or alpha test are not culled" — and the hair-card category itself
disables backface culling. `rtx.forceCutoutAlpha` is real in 1.6.1 but only drives the
`remix_category:alpha_blend_to_cutout` USD category, which has no rtx.conf hash list in this build.
NVIDIA's Minecraft leaf-shimmer block (`spatiallyHashedRadianceCache.queryForThinOpaquePrimaryView`,
`subsurface.*`) is **not** in our runtime's strings, so it cannot be copied. What both xoxor4d
profiles do carry, and what both our profiles now pin, is the opacity-micromap budget and tap guard
(`cache.maxVidmemSizePercentage 0.15`,
`building.conservativeEstimation.maxTexelTapsPerMicroTriangle 64` — NVIDIA's comment: "Set to 64 as a
safer cap. 512 has been found to cause a timeout. Any micro triangles requiring more texel taps will
be tagged as Opaque Unknown"), `buildRequests.enableAnimatedInstances = False` and
`building.splitBillboardGeometry = True` ("splits billboard geometry … to quads for higher reuse",
which is exactly what procobj grass is). Measured against the 1.6.1 source those are also the
defaults, so the change is a pin: the caps our own "Out of memory budget" device loss refers to are
now written down where they cannot drift.

## The water we had already built had never reached the install (2026-11-12, n13)

rhY's ask was one line: "make sure we get the new water by default". The answer was not a material -
n07 had shipped `remix-ref` and s32 had shipped `soft` - it was a hole in the pipeline, and measuring
the live install is what showed it.

### The before state, measured on `/media/rhy/data/Gamez/GTASA-RTX`

| what | measured |
|---|---|
| live water material | `soft` (`sha256 961a4d9a…`, equal to `config/water/soft.usda`); `Water_materials.usda.orig` = `28560263…` = `config/water/pack.usda`, so the pack's file was still recoverable |
| `water_normal_ref.dds` | **absent** - the sheet `remix-ref` points at had never been generated there |
| `G/rtx.conf` | 72 lines, hand-tuned, 8588 B. Water lines present: `rtx.animatedWaterTextures` with **61** hashes (`0x59296793798A7318` yes, **`0xA75A0B41F2454F04` no**) and `rtx.translucentMaterial.normalIntensity = 1.0` |
| the animated-water keys | `animatedWaterEnable`, `animatedWaterSecondaryNormalLodBias`, `layeredWaterNormalEnable`, `rayreconstruction.invalidateHistoryForAnimatedWater`, `rayreconstruction.combineSpecularAlbedo`: **all five absent** |
| who installs a variant | `apply.sh`: never called `water.sh`. `launcher/launch-rtx.sh`: only when `RHYS_WATER` was set. `water.sh`: printed the rtx.conf lines and wrote none of them |

Consequence, straight from the 1.6.1 source quoted in the n07 section above: the whole water path is
gated on `animatedWaterEnable && surface.isAnimatedWater`, and `animatedWaterEnable` was never written
to that install's `rtx.conf`. So the UV scroll and the second, LOD-blurred normal sample had never run
there **on any variant** - not `pack`, not `realistic`, not `calm`, not `soft`, not `remix-ref`. The
speckle work (s32, n07) had been reaching the material and not the renderer.

### The three places the default had to be installed, and the one that had to be remembered

`apply.sh` (with `--water <variant>` / `--no-water` as the opt-out), `launcher/launch-rtx.sh` (with
`RHYS_WATER` as the explicit override), and `water.sh default` as the shared path. The part that was
easy to get wrong is the second half of rhY's sentence: a default that overwrites a choice the user
made is just a new kind of surprise. So the choice is data, not behaviour:
`<game>/.rhys-water/state` holds `variant=… mode=default|explicit requested_by=…
material_sha256=… rtx_conf_sha256=… when=…`, and `default` refuses to touch anything recorded as
`mode=explicit` (`--force` once, and `water.sh pack` counts as an explicit choice, which is what makes
the rollback stick). Writing the state file only when the record actually differs is what keeps a
second `apply.sh` a byte-identical no-op - a `when=` timestamp written every run would have broken the
installer's own idempotence check.

Identifying the live variant by **sha256** instead of by grepping `normalmap_texture` also fixed a
measurement bug: the old heuristic reported the pack's own material as `unknown` as soon as the `.orig`
snapshot was missing, and `water.sh` refused to install anything at all in that state (it demanded the
snapshot). A fresh install has no snapshot, so the installer path could not have worked; it is now
taken automatically **only** when the live bytes equal `config/water/pack.usda`, which is provable, and
refused otherwise.

### The live run (the one write this task was allowed to make to the game folder)

`G/rtx.conf` was copied to `G/rtx.conf.bak-before-n13` first, then one `water.sh default $G`:

* `water_normal_ref.dds` generated from the pack's 10240x512 BC5 sheet in **0.7 s**: median facet tilt
  **44.0 → 6.0 deg**, p95 **90.0 → 9.3 deg**, texels over 60 deg **32.01 % → 0.00 %**, gradient
  **21.34 → 1.00 deg/texel**, u-decorrelation **10 px of 512 → 17 px of 128**, sheet
  10240x512 BC5 → 2560x128 RGBA8 with 13 mips.
* material → `remix-ref` (`2a6b13e8…`), transmittance distance 20 → 4 m, transmittance colour →
  (0.62, 0.8, 0.8), `normalmap_texture` → `./water_normal_ref.dds`.
* `rtx.conf`: five keys appended under one marked block, `0xA75A0B41F2454F04` appended to the end of
  the existing 61-hash list line (still **one** line), `normalIntensity` untouched because it already
  read 1.0. `diff rtx.conf.bak-before-n13 rtx.conf` = one changed line + 12 added lines, nothing else.
* runtime key audit (`tools/rtxkeycheck.py` against `config/rtx-runtime-keys.txt`): **2 unknown keys
  before and 2 after** - the same two pre-existing legacy spellings
  (`rtx.enableVolumetricLighting`, `rtx.minReplacementTextureMipMapLevel`); every key n13 wrote is
  `leaf+category` or `dotted` in the runtime's own strings. Key count **64 → 69** (the five appended
  keys; extending the hash list adds none). For reference the shipped profiles audit as
  `config/rtx.conf` 84 keys / 2 unknown, `rtx.conf.perf` 87 / **0**, `rtx.conf.nopops` 90 / 2.
* A second `water.sh default $G` changed nothing: material, `rtx.conf`, the sheet and the state file
  all re-hashed identical.

### What the shipped profiles already had

`config/rtx.conf`, `config/rtx.conf.perf` and `config/rtx.conf.nopops` were checked key by key: all
six water keys are present with the values the variant table uses (`normalIntensity 1.0`,
`animatedWaterEnable True`, `animatedWaterSecondaryNormalLodBias 2.0`, `layeredWaterNormalEnable True`,
`invalidateHistoryForAnimatedWater False`, `combineSpecularAlbedo True`) and both hashes are in all
three list lines. So `tools/rtxprofile.sh perf|nopops|current` keeps a `remix-ref` install working -
the water setup does not live only in the live file. `rtx.enableDirectTranslucentShadows` is
deliberately not in `perf` (its tooltip is quoted in that file) and is not part of the water table.

### Tests, as a count

`scripts/installer-selftest.sh` **44 → 78 checks** (its fake tree now carries a real `Water/`: the
pack's material and the pack's 7 MB BC5 sheet, copied read-only, so sheet generation runs for real);
`scripts/water-default-selftest.sh` **47 checks**, new and in `scripts/check-all.sh`, whose fake
install starts from an `rtx.conf` with all six water keys deleted and both hashes pulled out of the
list - so "the keys are there afterwards" is a result, not a given.

## A car's lamp is a 30 m billboard: the corona store, and the one runtime path behind V5, V6 and V7 (2026-11-12, n12)

`.strata/test-2026-11-12c.md` lists three complaints that look unrelated — tail lights throwing
red blobs on the sand, exhaust puffs glowing pink, building windows as flat white blocks. They
are three views of two facts: the game publishes a vehicle's lamps as **corona sprites of radius
15.0 game units**, and the runtime turns any additive/semi blend into an **emissive surface with
a forced emissive intensity**.

### The game side: where a car's lamps actually live

`CVehicle` keeps no lamp list we could read. What it does every frame is call
`CCoronas::RegisterCorona` (thunk `0x6FC580`, real body `0x6FC180`), and that store is readable
without patching anything:

```
0xC3E038  dword   live corona count, 0..64          (0x6FC35D inc, 0x6FC36F store)
0xC3E000  11 sprite pointers, indexed by the sprite index the thunk is handed
                     (`movzbl 0x28(%esp),%edx; mov 0xc3e000(,%edx,4),%eax`);
                     the init loop at 0x6FAA8B loads the names from 0x8D4950, stride 0x1A:
                     0/1 coronastar, 2 coronamoon, 3 coronareflect, 4 coronaheadlightline,
                     9 coronaringb
0xC3E058  entry 0, stride 0x3C, 64 entries          (0x6FC39F lea 0xc3e058(%ecx),%eax)
    +0x00 float x,y,z      +0x0C uint32 key (entity + head index)   +0x10 uint32 texture
    +0x14 float strength   +0x1C float range                        +0x28 float radius
    +0x2C uchar r,g,b,a
```

The six `RegisterCorona` calls in the vehicle render path — `0x6E0D01`, `0x6E0DF7`, `0x6E219F`,
`0x6E2297`, `0x6E253E`, `0x6E2633` — all read the model at `vehicle+0x384` and the class at
`+0x594`, push sprite **4** (`coronaheadlightline`) or **1** (`coronastar`), a bluish-white
colour (`0xa0,0xa0,0x8c` / `0x96,0xc3,0xc3`), **radius 15.0**, and the vehicle as the link
argument. The red rear lamps come from the model's 2dFx entries instead (the loop at `0x53B5A8`
walks 40-byte `C2dFx` entries, pushes sprite **2** `coronamoon`, strength 35.0, **radius 15.0**,
and the entry's colour, read per light state from the three bytes at `0x8CCFF0`). So: front lamps
are identifiable by sprite, rear lamps only by hue. And the key is `entity + head_index` with the
index reaching **7** (`0x6E219A` pushes `esi+7`), which is why grouping is "keys within 16 bytes",
not `key & ~3`.

Two consequences for V5, both measured rather than assumed: `coronaheadlightline`
(`0x4DBD066761A29F5B`) is already in `rtx.ignoreTextures`, so the headlight beam sprite is
**removed from the raytraced scene** and nothing lights the road in front of the car — that is
"headlights a small weak pool". And the rear lamps' 15-unit red billboard is in no category at
all, so it is rendered as emissive geometry and, with `rtx.enableSeparateUnorderedApproximations`
(default true), it also *lights* the surroundings — a 4 cm bulb reaching the renderer as a 30 m
red ball. That is "too wide/strong for 2 small bulbs".

### The runtime side: one code path, three symptoms

`rtx_instance_manager.cpp:882` sets `emissiveBlend = isBlendTypeEmissive(blendType)`; lines
1132-1138 then take any such draw call whose material uses the legacy alpha state and **force**
its emissive intensity to `rtx.emissiveBlendOverrideEmissiveIntensity` (default **0.2**) with the
albedo as the emissive colour. `rtx_scene_manager.cpp:1771` multiplies every material's emissive
by the global `rtx.emissiveIntensity`. So:

* **V6, exhaust puffs** — a smoke sprite is an alpha/additive blend, so it is not smoke, it is a
  self-lit sheet whose colour is its own texture. The runtime's own tooltip says what the knob is
  for: "Adjust this if particles for example look overly bright globally."
* **V5, the red wash** — the tail corona sprite rides the same path.
* **V7, windows** — the asset pack's authored emissives, scaled only by the one global dial.
  There is **no per-texture emissive category in 1.6.1**: every `*Textures` option in
  `rtx_options.h` is a geometry/material category, not radiometry. Per-material control belongs
  in the pack's `mat_<HASH>` materials, which this task does not touch.

### The measurements (tools/frame_light_measure.py, build/tmp/video-2026-11-12)

| metric | what the frames say |
|---|---|
| red spill on the sand | 6.9k-17.5k px, equivalent radius 47-75 px, source core up to 1572 px |
| headlight pool | 35k-119k px of bright non-red ground, peak high-pass 168-201 |
| smoke puffs | 90-719 px, mean RGB **[172, 57, 80]**, pink-ness min(R,B)-G **21.4**, high-pass luminance 31-96 **above the ground they rise from** |
| clipped-white windows | **0.30-0.40 %** of the ground band has all three channels ≥ 235; biggest connected block **850-963 px** (~46×24, a window pane); chroma **1.7/255** at mean RGB [252,252,252] |

The window row is the one that decides V7: neutral white with no chroma and no texture is an
emissive term above the tonemapper's knee, not a lit window. A real lit window carries its lamp's
spectrum (chroma 10-40 for the pack's warm interiors) and shows its mullions.

### What changed

* `plugin/src/vehiclelights.{h,cpp}` (pure) and `plugin/src/vehiclecorona.{h,cpp}` (game +
  bridge): read the corona store page-validated, pair one vehicle's dummies, aim the beam along
  the car's own front-to-rear axis, and create a **0.09 m reflector in a 50-degree cone**
  (`coneAngleDegrees` is the **half** angle — `rtx_remix_api.cpp:544` takes its cosine, which is
  why the street lamps' `cone_angle=120` is a 240-degree flood) plus **0.05 m red lenses**, brake
  filament brighter. Radiance comes from flux with the cone's geometric concentration
  `2/(1-cos θ)` applied, because `light_shaping.slangh`'s shaping is a 0..1 **mask** and nothing
  renormalizes it — a cone-cut light without that factor is 21x dimmer than the same lamp unshaped.
* `rtx.emissiveBlendOverrideEmissiveIntensity = 0.06` in every shipped profile (from the 0.2
  default): a 3.3x cut, so the scene's lights decide how bright a plume is while the sprite keeps
  enough glow to read as one.
* `rtx.particleTextures` gained the three smoke sprites that were in no category at all —
  `smoke5 0xC70DC799C5BFFE65`, `smoke4 0x3F66C5D5F91C3708` (the textures of the fx systems the
  game itself calls `smoke50lit` / `smoke30lit`) and `bullethitsmoke1 0x2D558BC31F67FD1D`.
  Coronas were considered for that list and left out: the Particle category does not touch the
  emissive term, so it would be a wide change with no measured benefit.
* `emissive_day 1.0 → 0.5`, `emissive_night 1.0 → 0.8` in the shipped ini — day cut harder,
  because that is where the clipping is measured.

## Frame pacing: what the 2026-11-12 MangoHud logs actually say (2026-11-12, measured during the n14 docs sweep; finding G, task n08 never ran)

rhY, 2026-11-12 10:45 (finding G): *"FPS a little less steady than previous builds; stabilised a
bit as he played."* Both runs' MangoHud CSVs are still in `<game>/logs/perf/`, so the sentence can
be measured instead of remembered. Numbers from
`NvRemixBridge_2026-11-12_{00-48-44,10-22-54,11-45-15,11-48-17}.csv` (script kept in
`build/tmp/n14/fps.py`, throwaway):

| run | build | samples | wall | fps mean / p01 / p05 / p50 / p95 | frametime mean ± sd | frametime p01 / p50 / p95 / max | > 25 ms | > 50 ms |
|---|---|---|---|---|---|---|---|---|
| 00:48:44 | s21-s23 + NEXT-TEST | 82 305 | 1459 s | 57.9 / 36.1 / 41.9 / 60.0 / 63.2 | 17.72 ± 4.21 ms | 12.54 / 16.68 / 23.85 / **786.8** | 1869 = **76.9/min** | 40 |
| 10:22:54 | `ba4c2c3` (n02b) + `water.sh soft` + 119 hair cards | 56 041 | 980 s | 58.7 / 36.3 / 41.2 / 60.0 / 64.0 | 17.50 ± 4.67 ms | 12.23 / 16.68 / 24.30 / **787.0** | 2242 = **137.2/min** | 35 |
| 11:45:15 | n09 test run | 8 342 | 144 s | 60.8 / 32.4 / 45.5 / 60.0 / 72.4 | 17.26 ± 9.93 ms | 9.74 / 16.67 / 21.98 / 803.8 | 211 = 87.9/min | 12 |
| 11:48:17 | n09b test run | 9 764 | 173 s | 59.7 / 28.8 / 40.2 / 60.0 / 73.9 | 17.69 ± 9.37 ms | 9.46 / 16.68 / 24.90 / 807.4 | 477 = 165.7/min | 15 |

What the table says:

* **The mean is not the complaint; the tail is.** Both long runs sit at 60.0 fps median (the
  present is capped) and both have a p01 of ~36 fps. The difference rhY felt is the spike
  population: **76.9 → 137.2 spikes per minute**, 1.78x, between the 00:48 and the 10:22 build.
* **"Stabilised as he played" is real.** Split each run at its first 5 minutes: 00:48 goes
  sd 6.80 → 3.14 ms, 10:22 goes 6.70 → 3.30 ms. The first five minutes of a session - streaming,
  shader and asset warm-up, the Remix cache filling - carry most of the variance.
* **Every run has exactly one enormous stall: 787-807 ms.** One frame, at the start. That is the
  first presented frame of a Remix session (bridge handshake plus the first frame of geometry
  processing), not a gameplay hitch, and it is why "worst frametime" is a useless number here.
* **The spikes are not our light churn.** Bucketing the 10:22 run into 98 bins of 10 s (the time
  axis rebuilt by summing frametime - see the caveat below) and taking the deltas of the plugin's
  own cumulative counters: `corr(spikes, new lamps) = -0.041`, `corr(spikes, retired) = -0.075`,
  `corr(spikes, fade steps) = -0.047`. The worst window, 320-360 s into the run
  (10:28:10-10:28:50, 227-355 spikes per 10 s, mean frametime 25.4-26.0 ms) has
  `created=0 steps=0` in `rhyremix.log`; the busiest churn windows (418 and 423 retires at 170 s
  and 260 s) have 2 and 3 spikes. Whatever the hitching is, it is not the plugin spending its
  create budget.
* **The card is not the limit at these settings.** `logs/perf/gpu-20261112-102247.csv`
  (nvidia-smi, 1 s): VRAM max **4418 MiB** of 16 376, GPU utilisation mean **48 %**, power max
  **194 W** of a 285 W card. A 4070 Ti SUPER idling at half utilisation while the game holds 60
  fps is waiting on something other than tracing throughput - which is consistent with the spike
  distribution being CPU/streaming-side.
* **Two logging gotchas for whoever writes `tools/perf-report.py` (n08).** (1) MangoHud's
  `gpu_vram_used` column reads ~0.02 for these runs: it is sampled for `NvRemixBridge`, and the
  memory is held by the bridge client in the game process - VRAM has to come from the
  `gpu-*.csv` nvidia-smi file. (2) The `elapsed` column is **not monotonic** (values up to
  980 501 in a 980 s run), so any per-minute bucketing built on it is garbage; sum the frametime
  column instead. The 00:47 pre-flight `gpu-*.csv` also shows why the launcher refuses to start
  and `scripts/test-session.sh` stops the local model first: **15 825 MiB** was already in use.

**Not measured here:** which change between the two builds costs the spikes. n08's plan (bisect
s28-n02b, the hair cards, the water by reading diffs, then spread our updates and propose
`config/rtx.conf.perf` keys) is still open; this section only settles what the symptom is and
rules our light churn out.

## Night realism: what n05 and n10 measured and did **not** ship (2026-11-12, parked as `6eafd4d` and `30a1a43`)

Both tasks failed the gate on `scripts/test-session-selftest.sh` (exit 124 and exit 1 - see
"Gate" in `.strata/notes-n05-night-brightness.md` and `.strata/notes-n10-night-ambient-realism.md`),
not on their analysis. Their measurements are the best evidence in this repo for findings B, C,
V1, V3 and V4, so they are recorded here; **none of their config or plugin changes is live.**

### Facing the sun at sunset turns the frame gray (B)

Which tour stop faces the sun, from the tour script's own camera/target pairs: `08a` looks
azimuth 94° (due east), `08b` 266°; `rhyremix.log` at those seconds says
`sun elev=+10.4 az=290.6` - so **08b is into the sun**, 25° off its axis, and 08a has its back to
it. Same two shots, both runs, 1920x1080:

| measure | 08a away | 08b into the sun | 00:49 run away / into |
|---|---|---|---|
| whole-frame mean luma | 102.8 | **75.5** | 107.6 / 78.2 |
| p01 / p05 (the black floor) | 19.9 / 33.7 | **10.7 / 15.8** | 16.7 / 36.4 |
| sky band p95-p05 (structure) | 102.2 | **47.7** | 102.8 / 47.5 |
| sky band mean gradient | 5.10 | **1.54** | 5.08 / 1.50 |
| % pixels at luma >= 191 | 4.48 % | **0.03 %** | 5.11 % / 0.16 % |
| p99.9 luma | 224.8 | 182.8 | 228.4 / 194.4 |

The frame loses its **bright end and its structure**, and its black floor falls too - which is
what rules out an additive veil (bloom halos, in-scattering) and auto-exposure
(`rtx.autoExposure.enabled = False` in the shipped file, the live file and the bridge's own
"Effective RtxOption values" dump). What is left is the **local tonemapper**: `rtx.tonemappingMode`
is never written by any profile we ship and defaults to 1 = Local (`rtx_options.h:1181`), in which
mode the global tonemapper never runs (`rtx_context.cpp:1951`) so every `rtx.tonemap.*` key we set
- including `finalizeWithACES` - is inert. `DxvkLocalToneMapping` renders three synthetic
exposures (`shadows = 2^rtx.localtonemap.shadows`, ours 2.5 = x5.66; `highlights =
2^-rtx.localtonemap.highlights`, never set = 4.0 = x0.0625, a 90:1 spread), weights them per pixel
through a mip pyramid to `rtx.localtonemap.mip = 3`, fits a straight line per tile and applies it
as **one multiplier on R, G and B** - their own source comment calls this "a crude multiplier on
all RGB channels. This is... generally pretty wrong" - then runs ACES over it. Put the sun in the
frame and the crushed branch wins over the tiles it touches, the pyramid carries that fit across
the frame, and a grey multiplier is exactly the reported symptom.

n05's shipped change would have been `rtx.localtonemap.highlights = 1.5` (spread 90:1 → 16:1) in
`config/rtx.conf`, `.perf` and `.nopops`. It is not in the tree.

### The night was not too bright because of an ambient light (V1, V3)

n10 measured the scene, not the screenshot: sand 150 units from any lamp is at **1.8 %** of the
sand under a lamp head in scene-linear radiance - the light budget was already right. The video
(build `ba4c2c3`, 23:42-23:56, 30 frames at 1920x1080) showed 25-72 %, i.e. the renderer was
throwing the ratio away four ways:

1. **The local tonemapper again** - a 90x squeeze of exactly the shadow/highlight pair, and a
   mip-pyramid fit that is re-solved around whatever is in frame, which is the view-dependent
   exposure of V3: the ground band of the video goes 88.6 → 146.6 → 96.2 luma in about three
   seconds of turning with no light changing at all.
2. **The sky probe's ceiling.** `rtx.skyBrightness` multiplies a probe rasterized in the game's
   colour format (`rtx.skyForceHDR = False`), so the key *is* an ambient ceiling: profile 1.0
   against a moon publishing **0.065** (`rhyremix.log` 11:50:17 `published: ... sky=0.046`) - an
   ambient 15x the only real source at night.
3. **The fallback light.** `rtx_light_manager.cpp:249-330`: mode 1 (the default) creates it when
   no lights are present, and `rtx.fallbackLightType = 1` makes it a **sphere of
   `rtx.fallbackLightRadius` at the camera position** - our profile carried `type=1, radius=3`,
   which at 6 m out-lights the moon 8:1 in menus and in the first seconds of a run.
4. **A distant light pointing up.** `rhyremix.log` 11:48:30: the sun handle
   `radiance=0.0376 dir=0.2206,-0.8386,+0.4981` with the sun at **-31.6°** - dir z > 0 is light
   coming up through the ground, because the radiance ramp-down keeps the handle alive tens of
   seconds after the body sets. n10's fix was two pure-C++ guards, `solarClampLightDir()` and
   `skyAmbientCap()`, neither of which is in the tree.

Cleared as causes, with the evidence: **vertex colour** cannot add light
(`opaque_surface_material_interaction.slangh:703-718` divides the vertex colour by its own maximum
first, so the albedo multiplier is in `[vertexColorStrength, 1]`) - what survives of it is the hue
of SA's baked July prelighting; **emissive** - `grep -c -i emissive` is 0 in the pack's wood,
ground and sand material USDs; **auto exposure** - already off, and the view-dependence was the
pyramid, not a follower.

V4 (the sea and the pier underside glowing teal by themselves) has one cheap mechanism in 1.6.1:
`rtx.enableTransmissionApproximationInIndirectRays = True` lets indirect rays keep their direction
through the sea, so the sky and the lamp pools arrive under the pier; bloom's default
`luminanceThreshold = 0.25` adds the halo.

**What a retry has to decide** is one taste number, not an unknown: n10's sim puts moon-lit sand at
~10/255 with `rtx.tonemap.shadowContrast = 0.25`, ~20/255 at 0.0 and ~4/255 at 0.5, and n05's
linear model puts the under-lamp night frame at 10.6-30.0 % of noon with the lamps at 40 % of their
flux (measured today: 57.3 %). Both branches carry the tools (`tools/night-brightness.py`,
`tools/night_ambient.py`, `tools/night-report.py`); neither is in the tree.

### What n23 shipped for finding C, and what it still owes a live run (2026-11-14)

n23 re-derived the **lighting** subset of the parked n05 on today's tree (`main`, HEAD `9ba6e08`)
and left the renderer side to n16/n22/n24. Nothing was run in game (rule 1), so the "before" is the
2026-11-12 10:22 tour and the "after" is owed by the next session.

**Before, measured with `tools/night-brightness.py` (the tool is in the tree now, and it fails the
run that misses):** under a lamp the 21:00 frame is **57.3 %** of its own noon frame (73.3 against
128.0) where the target is ≤ 25 %, and **72.0 %** of that frame's pixels are lit — floor p10 8.4,
mean of the lit part 96.0, mean of the unlit part 15.1. The other lamp stops: 30 m of pools 27.1 %
(36.4 % lit), sodium close-up 20.8 % (26.6 % lit). The no-lamp water stop is already 15.7 %. So the
night was bright because the frame **is** the lamp pool, not because of an ambient — which is what
n10's scene-linear measurement (1.8 % of under-lamp sand 150 units from a lamp) already said.

**The model, stated in the open** (linear in display luma, the same one n05 used, now printed by
`--predict`): `mean(s) = mean_dark + (lit/100)·s·(mean_lit − mean_dark)`. With the numbers above,
`s = 0.4` predicts 38.4 = **30.0 %** of noon with nothing else moving, and the pools/close-up stops
land at 15.5 % / 11.9 %.

**Shipped:** `[streetlights] radiance_scale 1.0 → 0.4` (one knob; the three `radiance_*` bands keep
their ratios, so the sodium/mercury balance is untouched); `[trafficlights] bulb_radiance 1200 →
840` ("a little too bright" = 30 %) with `glow_flux=304` and `glow_radius=3` **not moved**; and two
decouplings that had to come first, because the street knob reached through them —
`[trafficlights] radiance_scale` (ships 1.0; `trafficlights.cpp:411,423` used to pass the street
value into both `rhySignalBulb` and `rhySignalGlow`, so 0.4 would have taken 60 % of the glow rhY
asked to keep) and `[vehiclelights] radiance_scale` (ships 1.0; `vehiclecorona.cpp:367` used to hand
it to `RhyVehicleTuning`, so 0.4 would have knocked the headlamps n12 sized from a measured ~5000
peak intensity down to 40 % of it — finding V5 called those headlights *a small weak pool*).

**A side effect worth knowing, measured rather than noticed:** `glow_flux=304` is capped against one
street luminaire's flux (3473, computed from `radiance_big=2.0`/`radius_ref=15`, unscaled). With the
lamps at 0.4 a real luminaire puts out 1389, so the signal glow goes from **8.8 % to 21.9 %** of a
lamp's output. That is deliberate — finding C names the glow as the half to keep — but it is the one
number in the change that moves without anyone touching its key, and the live A/B should expect the
signal stops (53.5 % / 46.8 % today) to fall far less than the lamp stops.

**What it still owes.** The 57.3 % was rendered with `rtx.tonemappingMode` at its Local default, so
the local tonemapper's per-tile floor lift was in it; n16 shipped mode = 0, which removes that lift
(the low end of n05's band, 10.6 %), while n22's `[tonemap] day_ev=-0.5 night_ev=+0.5` puts **+1.0 EV
over noon** on a 21:00 frame (the high end, ≈42 % if it lands fully). Those two are the renderer's,
not ours, and n23 did not touch them. If the live run still misses 25 % with the lamps at 0.4, the
number to move is `[tonemap] night_ev`, and the number to move it **to** is **0.0** (a 0.5 EV
take-down, ≈×1.41 less at night, which the same model puts at ≈21 %); −0.5 would be the full 1 EV.
`tools/night-brightness.py --target 25` is the gate that says which world we are in.

### What n24 measured and shipped for V1/V3/V4 (2026-11-14)

The retry of the parked `30a1a43`, on today's tree. Nothing was run in game (rule 1), so the
"before" is the 2026-11-12c video and the sim, and the "after" is owed by the next live run. The
four causes above are re-measured here one by one; **cause 1 is already shipped** (n16,
`rtx.tonemappingMode = 0`, `config/rtx.conf:368`), which is also what kills V3's view-dependent
pyramid fit.

**The scene, today** (`python3 tools/night_ambient.py`, the pier layout of n10's sim, sand 150.3
units from the nearest lamp head, n23's lamps at 0.4, n22's `night_ev +0.5` applied):

| number | before n24 | after |
|---|---|---|
| moon light's published radiance (luminance, 31° elevation, full) | 0.0669 | 0.0669 |
| published ambient (`[sky] night+moon`) | **0.0700 = 1.05× the moon** | 0.0163 (capped) |
| share of open-sand radiance that is "sky" | **57.6 %** | 24 % |
| fallback sphere (mode 1, type 1, radius 3) at 6 m, no lamps in world | **0.4430 = 8.6× the moon** | 0 |
| scene ratio away/under | 7.12 % | **4.10 %** |
| image ratio after the tone chain | **50.6 %** (away 129/255) | **7.4 %** (away 19/255) |

Cause 1 being shipped did **not** fix the ratio: with the curve at its defaults the global tone
curve plus n22's +0.5 EV night anchor still flattened a 7.1 % scene into a 50.6 % image. That is
not the local pyramid coming back — it is the histogram fit lifting the dark end of a dark frame,
and `rtx.tonemap.shadowContrast` is the key that owns it.

**Cause 2, the ambient ceiling — shipped as a rule, not a constant.** `solar.cpp skyAmbientCap()`
caps the pushed `rtx.skyBrightness` at `[sky] ambient_cap_mult = 0.25` × the moon light's published
radiance once `daylight ≤ 0.05`; day and twilight pass through, `0` turns the rule off. Sweeping the
multiple in the sim: M=1.0 → image 49.8 %, M=0.5 → 40.6 %, **M=0.25 → 36.3 %**, M=0.1 → 33.4 %,
M=0 → 30.9 % — 0.25 captures 90 % of the available gain and leaves the sky band at 87/255 instead of
black, which is what the denoiser reconstructs from. It is a **ceiling**, not a prediction: the probe
is rasterized in the game's colour format (`rtx.skyForceHDR = False`) so its radiance is ≤ 1.0 and the
real ambient is usually below the cap. The cap is applied to the **target**, before `rhyRateStep`, so
the published value can only ramp: `tests/solar_check.cpp` feeds the shipped `[fade]` numbers
(`sky_rel_per_sec 3.0`, `sky_abs_per_sec 0.25`) at a 60 fps frame and gets 0.0700 → 0.0166 in **11
frames (0.18 s)** with no frame taking more than a quarter of it. A side effect worth knowing: the
ambient stops following the clock and starts following the moon — over a July night the ceiling runs
**0.0070 (18:00) … 0.0175 (midnight)** (`tests/solar_check.cpp`, at the test's turbidity 0.12; the
shipped ini's 0.15 puts midnight at 0.0163, which is the number the sim and the tables above use), and with the moon below the horizon it is 0 (the 0.001 floor
in `sun.cpp` is all that is left). A moonless, lampless night is dark; that is the finding, and
`ambient_cap_mult=0` is the switch back.

**Cause 3, the fallback light — shipped off.** `rtx.fallbackLightMode = 0` (the runtime's *Never*) and
`rtx.fallbackLightRadiance = 0, 0, 0` in `config/rtx.conf`, `.perf`, `.nopops` (regenerated), `.dlss5`
and `.experimental`; `fallbackLightType = 1` / `fallbackLightRadius = 3` are commented out with the
measurement. The 8:1 in the task file is 8.6:1 measured: radiance 1.6,1.8,2.0 (luminance 1.786) as a
3 m sphere at the camera, 6 m from the sand, is 0.4430 against the moon's 0.0515, and with it in the
world the **scene** ratio alone goes 7.12 % → **26.3 %** with both metric points clipping to 255. The
runtime's own tooltip for the mode says the same thing in one sentence ("this option should be set to
Never for 'production' builds of Remix creations", `rtx-options-1.6.1.tsv:299`).

**Cause 4, dir z > 0 — shipped as a clamp plus the existing retire path.** `solar.cpp
solarClampLightDir()` flattens any light direction with z > 0 onto the horizon and renormalises; it
runs on both bodies' `wantDir` and again on `pubDir` after `rhyDirLimit`, so the handle the bridge
receives can never be the logged `dir=0.2206,-0.8386,+0.4981` at sun −31.6°. The radiance target is
already 0 below −1° elevation (`relativeAirMass` returns 0), so the handle **dims through the existing
ramp and `updateDistant`'s `!(pubLum > 1e-4f) && !(wantLum > 1e-4f)` branch retires it** — no change
to `smooth.cpp`, so n01's cadence budgets and `tests/handle_check.cpp` are untouched. `tests/
solar_check.cpp` asserts the invariant over all 1440 minutes of a day for both bodies, and that a body
above the horizon is never touched.

**V4, measured rather than tasted.** The 60 frames of `build/tmp/video-2026-11-12` (1920×1080, build
`ba4c2c3`): a teal-chroma scan (B > R+12, G > R+4, luma ≥ 90) puts the glow in **y 841..941** of 1080
at a median **0.30 %** of the frame and mean luma **132/255**; `tools/night-report.py --box
0,841,1920,100`: p10 median **15.5/255** against p95 median **141/255**, so the band's own p10/p95
ratio is **9.7 %** by median (range 2.2-53.3 % over the 60 frames) and 7.9 % of its pixels are at or
above luma 120 - the glow is a bright minority of the band, not its floor. For the whole frame the
same tool measures the bottom-38 % ground band at **21.1-42.4 %** (median 33.3 %) with **43 of 60**
frames over the 10 % target: that is the V1 number the next live run has to move. In the display-referred **linear** space the bloom threshold is
compared against (bloom runs after the tone curve, before the sRGB encode): **10.31 %** of the band is
above the shipped 0.25 and blooms at full weight, **2.48 %** above 0.5, and only **0.23 %** is at or
above 1.0; the band's mean linear luminance is 0.1005. So `rtx.bloom.luminanceThreshold = 1.0` cuts
the fully-blooming area of that band **45×** while the clipped lamp heads and the sun disc still bloom.
And `rtx.enableTransmissionApproximationInIndirectRays` goes back to the runtime's own **False**
(`rtx-options-1.6.1.tsv:280`): the `True` came from `88e8cd1`, not from any measurement in this repo,
and its tooltip — "Translucent objects hit by indirect rays will not alter ray direction, just change
the ray throughput" — is exactly the mechanism that hands the sky and the lamp pools to the pier
underside.

**The taste number, decided with arithmetic.** `rtx.tonemap.shadowContrast = 0.4`, with
`shadowContrastEnd` left at its default 0. The curve pass subtracts
`shadowContrast × max(0, shadowContrastEnd − log2 L)` per bin (`tonemapping_tone_curve.comp.slang:198`;
the `pow(...)` line in `tonemapping_apply_tonemapping.comp.slang:79` is inside
`reinhardToneMapper`, which the dynamic tone mapper we use never reaches — worth knowing before
someone reads it as a second shadow lever). With `end = 0`, "values above this are unaffected" means
the noon ground at log2 **+0.36** (after n22's `day_ev −0.5`) is untouched by construction; only
material below linear 1.0 gets the extra contrast, and a noon shadow at linear 0.3 pays 0.70 stops.
On the sim's night metric points:

| shadowContrast | away sand | sky band | image ratio |
|---|---|---|---|
| 0.00 (runtime default) | 92/255 | 87 | 36.3 % |
| 0.20 | 45 | 41 | 17.7 % |
| 0.30 | 30 | 27 | 11.8 % |
| **0.40 (shipped)** | **19** | **17** | **7.5 %** |
| 0.50 | 11 | 9 | 4.3 % |
| 0.60 | 6 | 5 | 2.2 % |

0.4 is the value because it is inside n10's ≤ 10 % target with margin **and** leaves the moon-lit sand
at 19/255, which is what the denoiser and the eye need; 0.6 is not, at 6/255. `end = −1` instead of 0
buys half a stop of noon shadow for 3 points of ratio (10.7 % at 0.40) — not worth two numbers where
one has a measurement. What this still owes the live run: the sim's frame histogram is synthetic, so
the *direction* of the key is source-derived and the *landing point* is the next tour's number. The
A/B ladder is in the profile comment: 0.3, then 0.2, if the night comes back crushed; and if the night
is still too bright overall, the number n23 named is still `[tonemap] night_ev → 0.0`, not this one.

## Aerial perspective: what the game's fog is, what the runtime does with it, and the six numbers between (2026-11-13, n17)

docs/TARGET.md §4 **T3** ("distance must fade"). Nothing was run in the game — rule 1 — so this
section separates three things the repo has been mixing up: what the **game** draws, what the
**runtime** does with it, and what **we** can therefore change. Every file:line is the 1.6.1 source
(`build/tmp/n16-dxvk/dxvk-remix-remix-1.6.1`, upstream `NVIDIAGameWorks/dxvk-remix`, MIT) or the
read-only game folder.

**1. San Andreas' fog is two timecycle columns, and the pack's values are enormous.**
`data/timecyc.dat` (101,910 B, 2026-11-09, header "Custom Time Cycle for RTX-Remix only created By
Discord Hemry") has 52 columns per row; column 28 is `FarClp`, column 29 is `FogSt`, 8 rows per
weather type. Census (metres, which is what SA's units are):

| weather types | FarClp | FogSt range |
|---|---|---|
| the LA / SF / Vegas / countryside / desert sets | **4000** | −22 … +200 |
| EXTRASUNNY_SMOG_LA | 800 (5 AM) / 4000 | 10 … 100 |
| RAINY_SF / RAINY_COUNTRYSIDE / UNDERWATER | 4000 | 5 … 188 |
| FOGGY_SF | 4000 | −200 … −30 |
| SANDSTORM_DESERT | 4000 | −111 |

The tour pins weather 1 = SUNNY_LA (`moonloader/SARemix_Tour.lua:180 forceWeatherNow(1)`), whose
daytime rows are **FarClp 4000 / FogSt 100 m at every hour**. Vanilla `data/timecycp.dat` uses
FarClp 400–1150 m. So the daytime shots the haze metric runs on were taken with a fog that was
meant to finish at 4 km — an almost clear atmosphere by design, and the reason "the whole map is
laid out like the palm of your hand" is a property of the *game*, not only of the renderer.

**2. Our plugin never touches fog, and its "weather" is not the game's.** `main.cpp` hooks two
vtable entries (`Present`, `SetGammaRamp`); `grep -rn "SetRenderState\|D3DRS\|fog" plugin/src` finds
a comment and one key name inside the never-send list. `plugin/src/tod.h` states the boundary
outright: "there is no verified offset for the game's CWeather, and `rtx.ignoreGameDirectionalLights
= True` means the game's own timecycle/weather blend never reaches the renderer anyway".
`rhyWeatherInit/Update` (`tod.cpp:244-300`) smooths ini `sky.cloud` / `sky.turbidity` into the sun's
extinction and the sky ambient (`sun.cpp:696-710`); `solar.cpp` has no weather or fog at all. Two
separate atmospheres: n17 moves only the renderer's, and the plugin's cloud knob cannot change the
game's fog.

**3. The runtime captures the fog per draw call and then throws it away.** `setFogState`
(`d3d9_rtx_utils.cpp:227`) copies `D3DRS_FOGEND` into `fogState.end` and `1/(end−start)` into
`fogState.scale`; `SceneManager::submitDrawState` (`rtx_scene_manager.cpp:657-680`) keeps the
**first** non-`D3DFOG_NONE` state of the frame as `m_fog`. `rtx.enableFog` (default **True**) would
re-apply it as depth fog in the composite (`rtx_composite.cpp:406-421`, `applyFog` at
`composite/comp.slang:648`) — but `applyFog` starts with
`if (cb.volumeArgs.enable) return 1.0f; // Volumetric fog is applied in volume integration`
(`composite.comp.slang:653`), and `volumeArgs.enable = enable() && canUsePhysicalFog` with
`rtx.volumetrics.enable` default **True** (`rtx_global_volumetrics.cpp:576`). The density test at
`:435-452` passes for any linear fog SA sets (its `approximateDensity` uses `fogState.scale`, not
the fog start, so it disagrees with its own comment and lands at ~0.0017 for every SA row — always
under the 0.065 threshold). Net effect: **the game's fog has not been drawn at all**, and the medium
that replaced it is the untouched default — `transmittanceColor 0.999` measured at
`transmittanceMeasurementDistanceMeters 200` — which is σ = 1.14e-5 /m, i.e. **344 km of air**.
That is the mechanism behind n15's far/near local contrast 1.22–3.67.

**4. `enableFogRemap` is the bridge, and its bounds are in metres.** With
`rtx.volumetrics.enableFogRemap` (default **False**) and `enableFogMaxDistanceRemap` (default
**True**), LINEAR fog maps the captured end distance into the medium
(`rtx_global_volumetrics.cpp:470-546`):

```
normalized = (fogEnd − fogRemapMaxDistanceMinMeters) / (Max − Min)      # NOT clamped
d_meas     = normalized * (TransMax − TransMin) + TransMin
σ          = −ln(srgb_to_linear(transmittanceColor)) / d_meas ; T(x) = exp(−σ x)
```

`getMeterToWorldUnitScale() = 100 * rtx.sceneScale` (`rtx_options.h:1473`) and our profiles carry
`rtx.sceneScale = 0.01`, so the scale is **1.0** and the bounds are SA metres. The missing clamp is
a trap: with the runtime's own defaults (1 / 40 m) SA's 4000 m gives `normalized = 102` and
`d_meas = 8200 m` — a medium *thinner* than the nothing it replaces. `tools/fog-budget.py` now
computes all of this from a profile, and `tests/test_fog_budget.py` pins it.

**5. What n17 ships, and the density in weather-report units.** Six keys in all three profiles
(`config/rtx.conf`, `.perf`, `.nopops`): input bounds **400 / 4000 m** (bracketing both timecycle
files), output bounds **2 / 6 m**, `enableFogRemap = True`, `enableFogMaxDistanceRemap = True`
(explicit, it is the default), `fogRemapColorMultiscatteringScale = 0`.

| game fog end | d_meas | σ (1/m) | visibility | T@50 m | T@500 m | T@1 km | T@2 km | T@4 km |
|---|---|---|---|---|---|---|---|---|
| 4000 m (clear, every daytime row) | 6.00 | 3.79e-4 | **10.3 km** | 0.981 | 0.827 | 0.684 | 0.468 | 0.219 |
| 1500 m | 3.22 | 7.06e-4 | 5.5 km | 0.965 | 0.702 | 0.493 | 0.243 | 0.059 |
| 800 m (5 AM smog) | 2.44 | 9.31e-4 | 4.2 km | 0.955 | 0.628 | 0.394 | 0.155 | 0.024 |
| 400 m (densest the game asks for) | 2.00 | 1.14e-3 | 3.4 km | 0.945 | 0.566 | 0.320 | 0.103 | 0.011 |
| *before n17 (no remap, d_meas 200)* | 200 | 1.14e-5 | 344 km | 0.999 | 0.994 | 0.989 | 0.977 | 0.956 |

Visibility is Koschmieder's 3.912/σ, the distance at which a black object's contrast falls to 2 %.
The near band (≤ 50 m) keeps ≥ 94.5 % of its contrast at every rung and 98.1 % in clear weather —
"haze must not blur the ground under the camera" satisfied by construction, because a homogeneous
Beer-Lambert medium only bites at hundreds of metres.

**6. Two findings that change the plan, not just the config.**

*The haze metric has two sides.* Our far/near is > 1 because the **near** band is featureless flat
ground (near hp_rms 7.5–24.2) while the skyline is crisp (27.7–35.1) — `build/tmp/n17/gap.json`.
Extinction moves one side. Predicted after (`build/tmp/n17/predicted.txt`, the same exponential
applied to the measured bands): pershing-noon 3.67 → 2.56 with the far band at 1 km, 1.75 at 2 km.
The 0.5–0.9 target needs TransMax 2–3 m (3.4–5.2 km of air, i.e. SA's *foggy* end, not its clear
end) **and** a near band with detail in it — which is T1 (n18/n20, materials and textures), not this
task.

*The haze row and the sky row are in direct conflict in 1.6.1.* The same exponential attenuates the
sky: SA's sky is tagged (`rtx.skyBoxTextures`, `config/rtx.conf:51`) and the composite multiplies
sky by the volume's transmittance (`composite.comp.slang:627-636`), so at the shipped value the
skybox — drawn at the far clip, 4000 m — arrives at 0.219 of its radiance instead of 0.956. The
target wants sky luma ≥ 150 while our sky is already 92–128. Real atmospheres pay for extinction with
in-scatter; this runtime cannot, at distance: the volume's in-scatter comes from the light pool
inside `froxelMaxDistanceMeters` (**20 m**) plus `multiScatteringEstimate = fogState.color *
fogRemapColorMultiscatteringScale`, whose own tooltip says it is "just a simple ambient term … may
not look very good" and which n17 sets to **0** because it is the fog-that-lights-the-night knob.
The next knob for the sky is `froxelMaxDistanceMeters` (20 → 200–1000 m: the froxel grid's memory
does not change, its cells get coarser), and it is a separate step.

**7. Knobs that are hazardous, inert, or both.**

* `rtx.volumetrics.enableFogColorRemap` (default False) is a **density knob wearing a colour mask**:
  σ = −ln(colour)/d_meas, so a mid-grey game fog colour (0.6 sRGB) gives 0.19 /m and a dark one
  (0.1) 0.77 /m — transmittance at 50 m would be 1e-9. Not shippable without re-deriving d_meas.
* `rtx.volumetrics.enableAtmosphere` and its four parameters are **inert in 1.6.1**: the C++ fills
  them (`rtx_global_volumetrics.cpp:618-625`) but no shader in the tree reads `enableAtmosphere`,
  `atmosphereHeight`, `planetCenter`, `atmosphereRadiusSquared` or
  `maxAttenuationDistanceForNoAtmosphere` (only `volume_args.h` declares them). Do not spend a
  session on them.
* `rtx.volumetrics.enableHeterogeneousFog` (default False) is commented out in the config with its
  starting values: its noise field is sampled inside the march (`sampleDensityField`,
  `composite.comp.slang:390`) with defaults sized for a room — `noiseFieldInitialFrequencyPerMeter 8`
  (12 cm cells), `noiseFieldSubStepSizeMeters 10`, `noiseFieldOctaves 2`. For SA the starting point
  is ~0.002 /m (500 m cells), sub-step 50, octaves 3. Its helper
  `rtx/algorithm/volume_composite_helpers.slangh` is **not in the public 1.6.1 tarball**
  (`https://raw.githubusercontent.com/NVIDIAGameWorks/dxvk-remix/remix-1.6.1/src/dxvk/shaders/rtx/algorithm/volume_composite_helpers.slangh`
  → 404), so `calcVolumetricAttenuation` / `sampleDensityField` cannot be audited offline.
* A preset picked in the Remix UI **turns the remap off**: `DxvkGlobalVolumetricsPass::setPreset()`
  ends with `rtx.volumetrics.enableFogRemap.setDeferred(false)` (`rtx_global_volumetrics.cpp:313`),
  and the presets are the UI's "Volumetric Preset" dropdown. Our profiles do not set
  `rtx.graphicsPreset` to a volumetric preset, but a session that touches that dropdown silently
  loses the atmosphere.
* The runtime's own **Portal RTX profile** (`src/util/config/config.cpp:842-846`) writes
  `rtx.volumetrics.fogRemapMaxDistanceMin/Max` and `…TransmittanceMeasurementDistanceMin/Max` —
  **without** the `Meters` suffix. Those four strings are why `config/rtx-runtime-keys.txt` carries
  dotted fog names that are not options (lines 232-236) and why `tools/rtxkeycheck.py` accepts both
  spellings. Conversely `rtxkeycheck.py` FAILs on `rtx.enableFog` and `rtx.fogColorScale`, which
  *are* options: the strings vocabulary has false negatives as well as false positives, and
  `tools/rtx-option-audit.py` against `config/rtx-options-1.6.1.tsv` is the authority.

**8. The dead key n16 left for this task is fixed.** `rtx.enableFogMaxDistanceRemap = False` had
been in all three profiles since the runtime's first launch and is not an option of 1.6.1; it is
now `rtx.volumetrics.enableFogMaxDistanceRemap = True` (the default, so behaviour is unchanged) and
`config/rtx-dead-keys.txt` drops it, 5 entries → 4. The dead name stays in
`tests/test_rtx_option_audit.py`'s FAKE negative controls so the audit cannot go soft on it.

**9. Measured or not.** Measured: the timecycle census, the option names and defaults, the mechanism
(source), the before numbers (`build/tmp/n17/gap.json`, `good-before.json`), the density arithmetic
(`tools/fog-budget.py`, 9 tests). Predicted: the after numbers (`build/tmp/n17/predicted.txt`) —
rule 1 forbids running the game, wine or Steam, and a fog change only becomes a number once the
runtime draws a frame. The live protocol, the ladder in both directions and the abort conditions are
[TARGET.md §6](TARGET.md).

## Frame pacing, bisected: the tail, the hair cards, and the one burst we did own (2026-11-14, n08)

finding G of the 2026-11-12 10:22 report: *"FPS a little less steady than previous builds;
stabilised a bit as he played."* [The n14 section](#frame-pacing-what-the-2026-11-12-mangohud-logs-actually-say-2026-11-12-measured-during-the-n14-docs-sweep-finding-g-task-n08-never-ran)
settled what the symptom is and ruled our light churn out; this section is the bisect, the fix and
the tool that made both reproducible: `tools/perf-report.py` (11 tests in
`tests/test_perf_report.py`).

```
python3 tools/perf-report.py \
    <game>/logs/perf/NvRemixBridge_2026-11-12_00-48-44.csv \
    <game>/logs/perf/NvRemixBridge_2026-11-12_10-22-54.csv \
    --log <game>/scripts/rhyremix.log \
    --gpu-csv <game>/logs/perf/gpu-20261112-102247.csv --series --top 8
```

**1. The average got better and the tail got worse. That is the whole finding.**

| window | metric | 00:48:44 (`2f67e4a`) | 10:22:54 (`ba4c2c3`) | B − A |
|---|---|---|---|---|
| whole run | frametime mean | 17.724 ms | 17.496 ms | **−0.228** |
| whole run | frametime median | 16.679 ms | 16.676 ms | −0.003 |
| whole run | frametime sd | 4.210 ms | 4.671 ms | **+0.461** |
| whole run | frametime p99 / p99.9 | 27.73 / 41.67 ms | 27.56 / 42.08 ms | −0.17 / +0.41 |
| whole run | frames > 25 ms | 1869 = **76.9/min** | 2242 = **137.2/min** | **1.78x** |
| whole run | fps p1 / p0.1 | 36.07 / 24.00 | 36.28 / 23.77 | +0.22 / −0.23 |
| first 300 s | spikes/min | 24.00 | 22.40 | **−1.60** |
| after 300 s | spikes/min | 90.56 | **187.81** | **+97.25** |

The median of both runs is 16.68 ms, which is exactly the 60 Hz budget: the present is capped at
60 and the game thread spends essentially all of it. A frame that has no headroom is what a spike
is made *of* — so the question is never "what is the average cost" but "what lands on top of a
full frame, and how often". The first five minutes are not worse in the new build (22.4 against
24.0/min); everything after minute 5 is (187.8 against 90.6). "Stabilised as he played" is the
same fact seen from the inside: both runs calm down after the warm-up, and the new one then degrades
again, twice as hard.

**2. Not the card, not VRAM.** `nvidia-smi` at 1 s: 1048 samples over the 10:22 run, 986 of them
while the game actually held the GPU (memory.used > 1 GB). Over those: VRAM mean **4007 MiB**, peak
**4418 MiB** of 16 376; utilisation mean **49.8 %**, median 52 %, p95 63 %, max 100 %; power mean
**150.8 W**, max 194 W of a ~285 W card; SM clock at or above 2700 MHz in 878 of the 986 samples
(max 2805) with `clocks_event_reasons.active` = 0x0 in 867 samples and 0x1 (GPU idle) in 181 - never
a power, thermal or voltage throttle. A card at half utilisation holding 60 fps is waiting on
something else. The 00:48 run has **no** usable trace: `gpu-20261112-004710.csv` has six rows and
stops before the game launches, and its 15 825 MiB belongs to whatever held the GPU before -
`perf-report.py` matches a trace to a run by wall clock and prints "no VRAM source" rather than
borrowing the wrong one.

**3. Two corrections to the n14 note, both learned writing the tool.** (a) MangoHud's `elapsed` is
**monotonic** and is in **nanoseconds**: first 18 890 131, last 980 501 836 637, span 980.483 s,
which equals `sum(frametime)` = 980.5 s to the millisecond (same check on the 00:48 file: 1458.7 s
both ways). n14 divided by 1e6 and read 980 501 as a broken value. (b) It is *not* the frametime
column re-integrated: a per-row delta differs from that row's frametime by 0.10 ms median, 2.3 ms
at p90 and 786 ms at the worst, because MangoHud stamps the two at different points. Bucket on
`elapsed`, quote frametime per row. (c) `gpu_vram_used`, `gpu_load` and `process_rss` are constant
for the whole run in both files (0.0243416 GB, 0, 4723 KB) — MangoHud is inside the 32-bit bridge
client — so the tool calls them dead columns instead of graphing them.

**4. What actually changed between the two builds.** The plugin diff `2f67e4a..ba4c2c3` is nine
commits (s28, s29, s30, s31, s32, s27, n01, n02, n02b) and ~4100 inserted lines. The *config* diff is
two lines, and the game folder keeps its own backup so it is not a guess
(`<game>/.rhys-water/rtx.conf.pristine`, mtime 2026-11-12 10:22):

```
- rtx.translucentMaterial.normalIntensity = 0.35
+ rtx.translucentMaterial.normalIntensity = 1.0
+ rtx.hairCardTextures = <119 hashes>
```

plus the water material USD swapped to the `soft` variant by `tools/water.sh soft` — which makes the
water sheet *smaller*, not bigger: the pack's `water_normal.dds` is 512 px frames / 6 990 900 bytes,
the generated `water_normal_soft.dds` is 128 px frames / 1 747 812 bytes. So the water half of the
delta is a look change and a VRAM saving; the hair-card half is the one with a cost mechanism.

**5. Our per-frame work, measured — and why there is no spatial cache in this task.** A host
benchmark against the real lists (`build/tmp/n08/bench_select.cpp`, throwaway):

| what | cost |
|---|---|
| `rhySelectLights` over the 7002-lamp street list (rIn 300, rOut 380, cap 320+90, hardRadius 300) | **12 µs** mean, 27-38 µs worst, 216 lamps chosen |
| `rhySelectLights` over the 1522-entry signal list | **8.5 µs** mean, 21-27 µs worst, 248 chosen |
| the init worst-case-density scan in `streetlights::loadList()` (every 8th lamp x all, 6.1 M pairs) | **1.0 ms**, one time |

Both scans ran every 30 frames, on the **same** frame, so the plugin's periodic burst was ~20 µs
every 0.5 s — 0.004 % of the render thread's time, about 0.7 µs of a 16 670 µs frame. A grid cache
would take ~15 µs of that. It is not
nothing, it is **not** what makes a 25 ms frame, and shipping a cache for it would be a change whose
only evidence is that it felt like the task asked for one. So the measured answer goes in this table
and the cache does not go in the tree.

**6. The spike series does not follow us.** Per-minute buckets of the two runs
(`--series`, `--log`):

| | 00:48 run | 10:22 run |
|---|---|---|
| r(spikes/min, live lights) | **−0.37** | **−0.52** |
| r(spikes/min, creates in the frame) | n/a (older build) | −0.22 |
| r(spikes/min, handles pending destroy) | +0.04 | −0.13 |
| worst minute | 276 spikes, live 110 | **1098** spikes, live 101 |
| that minute | 01:08:44-01:09:44, 8.7 % of its frames | 10:27:54-10:28:54, **41.9 %** of its frames, the tour's teleport stretch |

The 10:22 run's worst minute has 42 % of its frames over 25 ms while the plugin holds ~100 handles,
has no street lamps lit, and makes no creates. The correlation with our live count is *negative* in
both runs, because the minutes with the most lights (night, standing still) are the calmest.

**7. The one burst we did own: the destroy flush.** s28 bounded creates
(`[fade] creates_per_frame=24`) and left the other half of a light's life unbounded:
`rhyFlushDelayed()` destroyed **every** due handle inside one `Present`. The two runs' own heartbeat
lines measured that queue:

| | 00:48 run | 10:22 run |
|---|---|---|
| `pending_destroy` max | **320** | **205** |
| `pending_destroy` mean over heartbeats | 0.95 | 1.76 |
| heartbeats with anything pending | 138 of 1447 | 232 of 972 |
| `creates_this_frame` max | (no such counter in that build) | 24 (the budget holds) |

Fix: `plugin/src/delayed.cpp` now drains through `smooth.h`'s `RhyDrainQueue` at most
`[fade] destroys_per_frame` (default **32**) per frame, oldest-due first; a 320-handle retirement
takes 10 frames = 0.17 s, which is inside `destroy_delay_ms` (500 ms, the delay that exists to
protect the bridge) and outside one frame. `rhyFlushDelayedTag()` stays unbounded on purpose: it is
a correctness rule (the replacement light has the destroyed handle's hash), it covers one index, and
deferring it is what makes a lamp blink out and stay out. `tests/drain_spread_check.cpp` asserts the
bound, the drain length, the order, the `GetTickCount()` wrap and the `0` escape hatch.

**8. The second thing we owned: both layers scanned on the same frame.** `streetlights` and
`trafficlights` both used `frame % 30 == 1`, so one frame every 30 carried the 7002-lamp scan *and*
the 1522-entry scan *and* up to 24 creates. `rhyResyncDue(frame, period, phase)` gives the signal
layer half a period of phase; the test asserts that phase 0 reproduces the old rule exactly (so the
street layer keeps the frame a layer first appears on) and that two modules on the same period never
share a frame over 100 000 frames. This is 20 µs, and it is in the tree because "always the same
frame" is how a periodic hitch becomes visible, not because 20 µs is a frame.

**9. The worst frame of both runs is the same frame.** 786.8 ms at +13.7 s (00:48) and 787.0 ms at
+13.7 s (10:22) — identical to 0.2 ms across two builds, and in both it sits in the window where the
street set comes up (`live` goes 1 → 361 by +15 s in the 00:48 log, 2 → 321 by +15.4 s in the 10:22
one). The 10:22 build had the create budget and the 00:48 build did not, and the frame did not move:
so it is a one-time renderer cost, not a per-create cost, and `creates_per_frame` is not the lever
for it. It is 0.05 % of a 24-minute run; it is also the reason "worst frametime" is a useless number
and why this task quotes p99/p99.9 and spikes/min instead.

**10. The renderer-side mechanism, from the runtime's own source**
(`build/tmp/n16-dxvk/dxvk-remix-remix-1.6.1`, the 1.6.1 tree n16 fetched):

| where | what it does |
|---|---|
| `rtx_types.cpp:412` | `rtx.hairCardTextures` sets `InstanceCategories::HairCards` on the draw call |
| `rtx_instance_manager.cpp:721-735` | that category **forces an alpha test** on the material (`forceHairCardAlphaTest`) |
| `rtx_instance_manager.cpp:86-88` | and sets `VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR` — double-sided |
| `rtx_options.h:215-218` | the documented effect: "preserve fine texture detail, render as cutouts instead of alpha blends, and disable backface culling"; and `hairCardMipBias`, **default −32.0** — always the sharpest mip, and no profile or live `rtx.conf` sets it |
| `rtx_opacity_micromap_manager.cpp:1005-1030` | an **opaque** instance with an albedo/opacity texture and texcoords is an **opacity-micromap candidate** — which is exactly what an alpha-tested cutout card becomes |
| `rtx_opacity_micromap_manager.cpp:1441-1445` | a candidate whose textures are not resident is **deferred and retried** — the work lands right after a stream-in |
| `rtx_opacity_micromap_manager.cpp:1323-1330` | "Per-frame processing budget: limits how many non-deferred candidates are fully processed to spread work across frames and **avoid spikes** when many deferrals expire at once" |
| `rtx_opacity_micromap_manager.h:118-122` | the budgets: `maxMicroTrianglesToBake/ToBuildMillionPerSecond = 60*5 = 300`, "Parameterized to <1% FPS overhead on 4090. Baking: 2 mil ~ 0.15 ms. Building: 10 mil ~ 0.04 ms" |
| `rtx_opacity_micromap_manager.h:172-176` | `conservativeEstimation.maxTrianglesToCalculateTexelDensityForPerFrame = 1536`: "2*1024 translates to ~0.65ms overhead per frame on a i9-12900K. Setting it to 1K to be at <0.35 ms max/**spike** overhead per frame" |
| `rtx_opacity_micromap_manager.h:82,91,93` | `maxRequests = 5000` staged, `minNumFramesRequested = 5`, `minNumRequests = 10` |

So the 10:22 run switched 119 vegetation materials from alpha-blended (no OMM, culled) to
alpha-tested cutouts (OMM candidates, double-sided, sharpest mip), and the runtime's own words for
that work are "spread across multiple frames", "budget per second", "deferred until resident" and
"spike". That is the right shape of explanation for a tail that grows after minute 5 and clusters
where the memory steps up. MangoHud's `ram_used` column is one of the columns that is **not** dead:
system RAM used grows 12.90 → 34.31 GB across the 00:48 session and 12.88 → 31.04 GB across the
10:22 one, and the 10:22 curve is where it steepens exactly where its tail is: minute 5 = 16.2 GB,
minute 6 = 20.0, minute 10 = 29.1 (the 00:48 curve takes the same 13 minutes to go 15.7 → 24.0). VRAM
over the same window goes 1785 MiB → 4296 MiB (`nvidia-smi`, first and last in-session sample).
Swap sits at 2.06-2.09 GB the whole way in both runs, so this is cache growth, not thrash.

**11. What is proposed, and what is deliberately not.** `config/rtx.conf.perf` (and
`config/rtx.conf.dlss5`, which is perf plus the probe) gains six OMM work bounds: build and bake
300 → **150** M micro-triangles/s, the texel-density pass 1536 → **512** triangles/frame (NVIDIA's
own scaling: ~0.12-0.17 ms instead of ~0.35 ms in the worst frame), and a staged request must be
seen for **30** frames and **30** uses before it is built, with at most **2000** staged. Those are
the parts of the mechanism that are a per-frame cost and change no pixel: an OMM arrives a few
frames later, and until then the geometry renders the way it rendered in the 00:48 run.

Not in the profile: `rtx.opacityMicromap.enable = False` (the A/B that would decide whether the OMM
path is the cost at all — it changes how a cutout shades, so it belongs in a run with someone
looking), `rtx.hairCardMipBias = 0` (the −32 default is what keeps strand detail, which is the point
of n21), and `numFramesAtStartToBuildWithHighWorkload`/`highWorkloadMultiplier` (the opposite
direction, and the worst frame of both runs is a startup frame).

**12. Measured and predicted.** Measured here: every frametime, spike, VRAM, utilisation, log-counter
and native-cost number above, from files already on disk, plus the two plugin-side bounds
(`tests/drain_spread_check.cpp`, 29 checks). **Predicted:** that the six OMM keys buy back the tail.
Rule 1 forbids running the game here, and the Remix log of the 10:22 run was overwritten by the
11:45/11:48 runs, so the OMM statistics of that run (staged/baked/built counts) are gone and the
mechanism is source-and-timing evidence, not a counter. [docs/NEXT-TEST.md](NEXT-TEST.md) asks for
the A/B: same tour, `rtxprofile.sh perf`, and one extra leg with
`rtx.opacityMicromap.enableBuilding = False` to settle it either way.

## The surfaces you actually look at: what the game's data says is on screen, and what the pack already covers (2026-11-14, n20 → n20b)

rhY's MUST-HAVE #1. n20 built the pipeline and was parked on two documentation gate failures
(`a96ec15`); n20b brought it onto `main`, finished it and measured what it had only asserted. The
user-facing page is [TEXTURES.md](TEXTURES.md); this is the evidence, including the three places
where n20's own tests were wrong and the code was right.

**1. The pack's replacement textures are BC7/BC5/BC4 with DX10 headers and mip chains, and the
runtime complains when they are missing.** Census of the 1152 `.dds` under
`<game>/rtx-remix/mods/gameReadyAssets/texture/` (header parse, this session):

| DXGI / legacy format | files | used by the pack for |
|---|---|---|
| DX10 / 80 = BC4_UNORM | 445 | `_rough`, `_height` |
| DX10 / 83 = BC5_UNORM | 251 | `_normal` |
| DX10 / 98 + 99 = BC7_UNORM(_SRGB) | 246 + 173 | `_albedo` |
| legacy DXT1/DXT3/DXT5, BC5U, rgb32 | 37 | odds and ends |

`<game>/rtx-remix/logs/remix-dxvk.log` (2026-11-12 11:48 run) logs
`A suboptimal replacement texture detected: … Please make sure all replacement textures have
mip-maps.` for `texture/Wall/*.dds` and `texture/Rail/railway_*.dds`, whose headers say
`mipCount = 1`. So the target format for anything we generate is settled by the pack itself, and a
mip chain is not optional. Our own set writes BC3 albedo instead of BC7 — see 6.

**2. The runtime resolves mod asset paths case-insensitively under Proton.** `Materials.usda` says
`@./texture/Asphalt/Asphalt_materials.usda@` while the directory on this ext4 volume is
`texture/asphalt/`, and the same log loads `…\texture\Wall\BADC201F23379105_normal.dds` from
`texture/wall/`. Checked because a case-sensitive filesystem would have silently dropped 18 of the
pack's 23 material layers. It does not, so the pack's capitalisation mismatch is harmless — and a
second folder under `rtx-remix/mods/` is enumerated (`Adding asset search path: z:…\mods\…`), which
is why our own state lives in a **dot**-dir there and is not scanned as a layer.

**3. SA's map geometry does not expose a per-triangle material index.** The triangle array is
4-byte records (`[v0][v1][v2][0xff]`, two such arrays per geometry) and the 4th byte is `0xff` in
every triangle of every model checked. Measured on `lae2_roads89` (225 tris, 232 verts, 5
materials): the geometry struct is **7392 bytes = 900 + 900 + 24 + 2784 + 2784**, which closes
exactly with two 4-byte index arrays and one morph target of positions+normals — there is no
material array to read. Recovering it means reimplementing librw's binMesh/binUVN decoder, so
`tools/texture_rank.py` splits a model's triangles **evenly over its distinct textures** and says
so in its own docstring and in the JSON `note` field.

**4. The ranking, measured.** 9315 instances in the text IPLs under `data/maps/**/*.ipl` + 36516 in
the 164 binary ("bnry") IPLs inside `models/gta3.img` = **45831 instances, 11612 distinct models**
(the `.ide` id resolution of `tools/lamp_list.py`: the position in the `.img` index is *not* the
model id). Model → textures comes from the `binMATERIAL` chunks (type 0x00000007) of the model's
`.dff`, the texture name being the first `binSTRING` inside it (verified on three LA map models:
`lae2_roads89` → sidewalkgrass1/2/3 + plaintarmac1 + dt_road_stoplinea; `lawroads_law14` →
dockpave_256, snpedtest1, crossing_law; `pershingsq1_lan` → sjmlahus28, pershing1_LAn, tanstucco1_LA,
badmarb1_LAn). With the four daytime tour cameras (45° cone, 200 m): **109 models, 291 textures with
geometry, top 40 = 40.9 % of the solid angle in the cones, 11 700 m²**. Re-running the tool on
2026-11-14 reproduced the shipped 40 in the same order. Sensitivity (Jaccard against the shipped
setting): 0.67–0.86 at cone 35°, 0.78–0.95 at cone 55°, 1.00 at the shipped 45°/200 m — the set
moves by 1–8 of 40 across the whole 3×3 grid, so the answer is not a knife-edge, but it *is*
parameter-dependent and the parameters are recorded in `config/texture-pbr.json`.

**5. The pack already covers 5 of the 40, at better than we can make them.** Our layer is installed
**last** in `mod.usda`'s `subLayers`, so it would win — and would be a downgrade:

| texture | the pack's own files | ours |
|---|---|---|
| `sidewgrass1` `0x8A04C4005EC0D1C2` | 2048² albedo (12 mips), normal (12), rough (1), height (1) | 1024² derived |
| `sidewgrass3` `0xA1A030CB1C532B19` | 2048² albedo (12), normal (1), rough (1), height (1) | 1024² |
| `sidewgrass2` `0xB53827C8E12BB227` | 2048² albedo (12), normal (1), rough (1), height (1) | 1024² |
| `gb_nastybar03` `0x032123631B5B198B` | 2048² albedo (12), normal (1), rough (1), height (1) | 1024² |
| `dt_road` `0x7FF5EBF9BAD3765F` | **4096²** albedo + normal + rough + height, all 13 mips | 2048² |

`pbrify.pack_coverage()` therefore skips them (35 overrides, not 40; 33.6 MB of VRAM not spent on
making five surfaces worse), and `--keep-covered` puts them back for an A/B. Separately: **10 of the
40 have a `mat_<HASH>` override in the pack's layers** (900 distinct `mat_` names across the pack's
`.usda` files) but only these 5 have DDS files — the other five (`badmarb1_LAn`, `yardgrass1`,
`grasspatch_64hv`, `backalley1_lae`, `ws_rooftarmac1`) get a material from the pack with the game's
own texture, which is exactly the population n18's census counted.

**6. There is no BCn encoder on this box, so `tools/bcn.py` is ours.** Pillow 13 *decodes* BCn but
its DDS writer emits uncompressed RGBA whatever `rawmode` says — measured: `rawmode="BC7"` produces
a DX10 header with DXGI **64** (R8G8B8A8) and a bit-exact round-trip, i.e. it silently ignored the
request. ImageMagick 7 refuses too. `tools/bcn.py` writes BC1/BC3/BC4/BC5 (and BC7 mode 6, unused
by the shipped set) and decodes them; `scripts/textures-selftest.sh` cross-checks it against Pillow,
against **ffmpeg**'s DDS decoder, against the game's own DXT1 rasters, and against the D3D block
layout written out of the spec.

**7. Three of n20's own encoder assertions were wrong, and the encoder was right.** Found while
making the self-test pass on `main` (it had never been run: the parked commit did not wire it into
`check-all.sh`). All three were fixed in the **test**, not the tool, because ffmpeg, Pillow and the
spec all agreed with the tool:

* the BC1 check compared selector **1** against the 2/3 mix. Selector 1 is the `c1` endpoint; the
  2/3 mix is selector 2 when `c0 > c1`. The check now walks all 16 selectors of block 0 against the
  full four-entry table (and the `c0 <= c1` variant);
* the alpha-ladder check interpolated `a0 → a1` linearly (`((7-i)·a0 + i·a1)/7` for every `i`). The
  RGTC spec puts the endpoints at entries 0 and 1 and starts the 7-way mixes at entry 2 — entry 1 of
  an `a0=255, a1=0` block is **0**, not 218. The same file's own first check
  (`[255, 0, 218, 182, 145, 109, 72, 36]`) had the spec right;
* the ffmpeg cross-decode compared `decode_bc4`'s **(h, w)** array against ffmpeg's **(h, w, 1)**,
  which numpy broadcast to (h, w, h): an RMSE of 105 between two decoders that agree **bit for
  bit** (verified directly: 0.0).

The lesson is the one s13 already wrote about warnings: a check that has never run is not a check.
`scripts/check-all.sh` now runs `scripts/textures-selftest.sh` (24 checks) as `textures`.

**8. VRAM, measured rather than planned.** 35 sets, 140 DDS, **91.9 MB on disk and 91.9 MB in
texture memory** (BCn is stored compressed, so those are the same number — no
"small-on-disk/huge-in-memory" trap). Per output size: 512² = 1.05 MB, 1024² = 4.19 MB,
2048² = 16.8 MB, 512×256 = 0.52 MB, 128×256 = 0.13 MB. The game's own rasters for the same 35
materials, mip chains included, are **1.08 MB**, so the layer is **+90.8 MB** — against the
3.5–4.4 GB of 16.4 GB the running game already uses (n08, above): ~0.6 % of the card. All 140 files
carry a full mip chain (verified by decoding the headers, not by trusting the writer).

**9. What the generated maps actually contain, decoded back out.** Roughness means by class
(wall 0.657, grass 0.721, pavement 0.649, asphalt 0.555, roof 0.650, concrete 0.506, ground 0.699,
roadmark 0.555, glass 0.041, brick 0.630, stucco 0.694) sit inside the bands the class table
declares, and the normal-map std (14–35/255 for the rough classes, 0.8 for glass) sits on the pack's
own range (its asphalt normal measures 21). Albedo fidelity against the source raster: mean absolute
luminance shift **2.75** luma, worst 4.05, mean alpha 252.6 — the 4× LANCZOS pass does not repaint
the surface, which is the whole reason the albedo is the game's and not a photograph.

**10. The class numbers are the pack's, not ours.** `python3 tools/pbrify.py --game <GAME>
--calibrate` decodes a sample of the pack's own maps per category: roughness medians asphalt 0.588,
wall 0.681, pavement 0.655, ground 0.735, roof 0.685, concrete 0.514, metal 0.507, windows 0.060,
Marble 0.079, carpet 0.832; normal-map std 21–48 for the rough categories, 0.7–1.8 for windows,
marble and plastic. n20's first draft of `CLASSES` had asphalt 0.85 from the literature; the shipped
table (asphalt 0.59, wall 0.68, pavement 0.66, ground 0.74, glass 0.06) is the pack's own numbers,
which is also the only way this layer can sit next to the pack's 121 rough maps without a visible
step.

**11. One integration bug the tests had not reached.** `tools/texture_rank.py` wrote the class as
`class_` (because `class=` is a keyword in a `dict(...)` call), while `tools/pbrify.py` read
`e["class"]` — so the documented regeneration path, `rank.json` straight into `pbrify --list`,
raised `KeyError`. The writer now emits `"class"` through the `**{"class": …}` spelling, `pbrify`
accepts either, and `tests/test_texture_rank.py` pins the shape of the shipped
`config/texture-pbr.json` (40 entries, unique Remix hashes, every class known, `pack_has` measured)
so the seam cannot go quiet again.

**12. Not measured.** Nothing here has been rendered (rule 1). The claim "kerb edges and stucco
carry relief and a dry matte response" is a prediction from the map statistics in 9;
[TEXTURES.md](TEXTURES.md) and [NEXT-TEST.md](NEXT-TEST.md) step 8 say what to look at and what to
bring back.

## Every lamp the game declares: the 2dFx records, and the head that has to glow (2026-11-14, n11)

rhY's 2026-11-12c review, V2, at the Verona Beach pier (00:31-00:44 of the video): *"boardwalk/beach
lamps are dark: no lit bulb, no pool of light under them"*. The n06 follow-up had already put a hand-
written table under the pier lamps. n11 asked the prior question - how do we know which lamps exist -
and found that the game says so, per bulb, inside the models.

**1. The chunk is `0x0253F2F8`, and it is everywhere.** Walking every chunk of every `.dff` in
`gta3.img` (12239 `.dff`) finds it in **1591** of them. The record frame:

```
payload: u32 recordCount, then record[recordCount]
    f32 x, y, z        model-local position
    u32 tagword        LOW byte = tag
    u32 payloadSize
    u8  payload[payloadSize]
```

The self-check is the frame, not a magic string: the record list ends **exactly** at the end of the
chunk in all 1591 (`scan_img` counts `chunks` and `closed` and they are equal). A parser one byte off
cannot do that. The tag pairs with exactly one payload size across the whole archive - 0/80 (944
records), 1/24 (49), 3/56 (692), 6/44 (77), 7/88 (489), 8/4 (30), 9/12 (14138), 10/40 (5) - which is
what makes the tag's position decidable rather than guessed.

**2. The tag is the LOW byte, and getting it wrong invents lamps.** Reading `tagword >> 24` makes
every record a light (tagwords are all ≤ 0xff), and the first draft of the parser produced 46k
"lights" including every palm tree and parking-lot surface in Los Santos. 18691 is the honest number.

**3. The first four payload bytes are a colour, and the game says so.** The signal-head loop at
`0x49DC75` computes `edi = (e[0x10] > 200) ? (100 > e[0x11] ? 1 : 2) : 0`, and `entry+0x10` is
`payload+0x00`. `cj_traffic_light4`'s three lens records are (255,0,0), (255,128,0), (0,255,0), which
that expression maps to 1, 2, 0 - red, amber, green, the game's own phase indices. The alpha byte is
a constant `0xC8` on every lamp record measured, which rules out a packed `0xRRGGBB` int.

**4. `+0x04` is a range; `+0x08/0x0c/0x10` are not a focus point.** Measured ranges: 62
(`vegaslampost`), 100 (the LA lampposts - 16479 of the 18691 instances), 150 (`lampost_coast`), 180
(`bollardlight`), 300 (`lamppost2`'s globes), 450-900 (airport, rail-bridge and refinery masts). The
three following floats are small enums, not a point: `f8` ∈ {0,5,12,18} and it is **0 exactly where
the model draws no corona**, `f12` ∈ {0,0.7,0.8,1,2,2.5,4}, `f16` ∈ {0,6,8}. A focus point would move
with the record; these do not. The names after them (`coronastar`, `shad_exp`, `Col Sphere`) sit at
model-dependent offsets - `lamppost2` and `lamppost3` disagree by a byte - so the reader returns the
printable runs instead of pretending they are fields.

**5. The inventory.** 145 models carry light records (944 records); **112** of them are placed by an
IPL; the instances multiply them to **18691** world lights: LA 6552, vegas 6025, SF 4997, country
1117. Region comes from the IPL - the directory for text IPLs, the stream name for the 164 "bnry" IPLs
inside `gta3.img`. At the pier points n06 named ((378,-1964) and (836,-1855)) the lamps are
`lamppost1` (28/144 instances), `lamppost2` (13/31), `lamppost3` (28/19), `lampost_coast` (3/0),
`streetlamp1/2`, plus the freeway sign lamps `lasrnway1..5_las` and the motel sign `lamotsig1_la`.

**6. The third-party table covers 64.4 % of that, and the Los Santos street number is 74.0 %.**
`moonloader/SARemix_Light_Manager.dat` has **22** model ids, 15 of which are 2dFx-lit models.
`tools/lamp-coverage.py` (tolerance 1.0 game unit, 3D):

| region | 2dFx lights | signal layer | street lights | covered by the old list | coverage |
|---|---|---|---|---|---|
| LA | 6552 | 3016 | 3536 | 2617 | **74.01 %** |
| vegas | 6025 | 3206 | 2819 | 1383 | 49.06 % |
| SF | 4997 | 2965 | 2032 | 1360 | 66.93 % |
| country | 1117 | 539 | 578 | 341 | 59.00 % |

36 models are uncovered in Los Santos alone. The biggest single one is `lamppost2`: the model carries
**three** lights - two globes at (±2.98, 0.01, 5.32) range 300 and a mid-pole lamp at
(0.04,-0.05,3.90) range 100 - and the table has two offsets, so **244** of the 732 Los Santos
`lamppost2` lights were never lit. That is the double-globe boardwalk post in the video, and it is a
missing *bulb* on a lamp post the list already had.

**7. Where the table does exist, it was written from this data - approximately.** Table offset vs the
model's own 2dFx bulb: `lamppost1` (-0.50,0,3.08) vs (-0.44,0.05,3.20); `lamppost3` (-1.30,0.17,3.71)
vs (-1.40,0.22,3.67); `lamppost2` (±3.00,0,5.50) vs (±2.98,0.01,5.32) - 0.11-0.19 m apart, close
enough to be trustworthy and far enough that the 2dFx position is the one to use. And the table can
be plainly wrong where nobody looked: `sub_floodlite` carries three offsets, all `(0,0,0)`, while the
model's two lights are at (±1.52, 0.11, 13.89) - a 14 m mast's lamps at the base of the pole.

**8. The game's colour and the physics agree, which is the cross-check that both are right.** The
2dFx bytes are display values; linearised (`rhySrgbToLinear`), `lamppost3`'s (255,148,52) is
(1.000, 0.297, 0.034) against the published HPS chromaticity (0.530, 0.415) → (1.000, 0.254, 0.003).
`lamppost1` (249,145,34) → (1.000, 0.299, 0.017). A raster author and a lighting reference, naming the
same lamp to within a few percent. Where the bytes are plain white (`streetlamp1/2`, `lampost_coast`,
`bollardlight`) they carry no spectrum at all, and that is exactly where the fitting has to decide -
hence the new `fluorescent` family (F40D daylight tube, x=0.313 y=0.337) rather than a guess from the
ini's one cold triple.

**9. One light cannot be both halves of "no lit bulb, no pool of light".** n02b already wrote the
reason for signals: *"a point light is not visible geometry, it only lights what is near it"*. The
luminaire light is a 0.15-0.20 m sphere in a 120-125 degree cone **pointing at the road**: it makes the
pool and it cannot make the head, because the head is where it sits and its own housing is outside the
cone. So the head gets its own light: 0.12/0.10/0.07 m by fitting band, **unshaped** (a bulb shines
every way), carrying `head_radiance` **nits at its own radius**. Not flux-preserved, and the size of
that choice is the s30 lesson restated: flux-preserving 1200 nits from a 0.1 m bulb to the 15-unit
reference is **27 000 000** - a flood lamp wearing a bulb.

**10. The head layer is near-field, and that is a measured bound, not a guess.** Worst case number of
list lights within a radius of a point, over the 8962-light list (sampled every 8th lamp, printed by
the plugin at load and by `lamp-coverage.py --density`): **174** within 120, **484** within 300, **673**
within 380. So the head band is 120..160 with a 200-slot cap, and the luminaire band keeps 300..380
with 560+140 (which n04 measured as 552 on the old 7002-lamp list - the new list is denser, and 700
still covers it). Both layers run the same `rhySelectLights` hysteresis, the same fade envelope, the
same per-frame create budget, and each has its own hash namespace and delayed-destroy tag, so a bulb's
destroy-before-create can never retire the housing under it.

**11. Not measured.** Whether the head light reads as a lit bulb from the seat of a bike. The
mechanism is the one n02b shipped for signal lenses and rhY accepted ("We wanted the glow ... It
should be both like real life"), and the game's own corona for the bulb (`coronastar`, the `f8` size
float 12/18 in the record) still draws the sprite on top of it. [NEXT-TEST.md](NEXT-TEST.md) step 9 is
the leg that settles it, with `head_glow=0` as the A/B.

## The logs the tools read were unbounded, and the tour script nobody installed (2026-11-14, n03)

Two things that only show up when you go back to a machine weeks later: a log that has been growing
since the first session, and a script in the game folder that no installer owns. Both were follow-ups
somebody had already written down (`.strata/notes-s26-perf-checklist.md`, follow-ups 1-3) and neither
had an owner. Measured in `$G` on 2026-11-14, read-only:

| thing | measured | what it cost |
|---|---|---|
| `scripts/rhyremix.log` | **1,088,220 bytes**, mtime 2026-11-12 11:51, appended to on every run since 2026-11-10 | `scripts/diagnose.sh` and `tools/tour-report.py` scan it whole, so every session made the next diagnosis slower |
| `scripts/rhyremix.log.prev` | 3,378,593 bytes, mtime 2026-11-10 22:03 | **hand-made**: nothing in the repo had ever rotated the log, so the only `.prev` on the machine was one a human moved |
| `scripts/rhyremix.log.crash1`, `.test-prev` | 18,782 / 1,962 bytes | the same, by hand, twice more: the naming is folklore, not a policy |
| `logs/steam-12120.log` | **51,958,320 bytes** (`du -sh $G/logs` 64 M) from ONE run | `launcher/launch-rtx.sh` exported `PROTON_LOG=1` unconditionally and nothing ever bounded or removed the file |
| `moonloader/SARemix_Tour.lua` | 13,829 bytes, mtime 2026-11-12 10:22, sha256 `0eb48b8c…` = the repo's | the *symptom* of notes-s26 gap 1 (the 2026-11-12 00:49 session ran the stale **13-stop** tour while the repo had 16) was gone only because a human had run the `cp` that `docs/NEXT-TEST.md` step 0 asked for |
| `grep -n Tour deploy.sh` | **no hits**; deploy.sh copies `SARemix_Shot.lua` and nothing else | the *cause* was still there: no installer owned the tour script, so a fresh install - or any install after a stop list changed - silently ran the old one |

**The mechanism is the same in both rows: nobody owned the file's lifecycle.** A log file that is only
ever opened in append mode grows forever, and a Lua script that only ever reaches a game folder through
a copy-paste in a document is the same problem one level up - the document is the deployment. That is
also why the tour gap survived being *noticed*: s26 fixed the symptom in the doc (a preflight `cp`) and
queued the durable fix, and the queue is not a gate.

**Where the bound went, and why not in the plugin.** The task offered `plugin/src/log.cpp` or
`launcher/launch-rtx.sh`. The launcher won on three grounds: (1) one place has to own **both** logs and
the plugin cannot touch the Proton trace at all - Proton writes `logs/steam-<appid>.log` from its own
process on the wine side; (2) a session boundary is the honest cut - `log.cpp` would rename a file it
has open, on a thread inside the process that is itself the thing that crashes, while the launcher moves
a file that is by definition finished with, and the live log then restarts at 0, which makes "which run
is this line from" answerable; (3) testability - blocks of the launcher are sourced and driven against a
fake tree (`scripts/launch-cleanup-selftest.sh` already does it), while `log.cpp` is MinGW-only with no
host harness, so a rotation bug there would be found by the game, on a run that costs an hour.

**What the bound is.** Before the game starts (and never on a `RHYS_DRY_RUN=1` run, which must not eat
the log of the session being reported on): a log over `RHYS_LOG_MAX_BYTES` (8 MiB) moves to
`<name>.prev` - exactly one generation - and the live file restarts at 0. The plugin log moves **whole**,
because the tools read it line by line and a `.prev` that begins mid-line is a lost line; the Proton
trace keeps only its last `RHYS_PROTON_LOG_KEEP` (2 MiB), because one run of it is ~50 MB of wine debug
nobody reads except by grepping the end. After the run the trace is trimmed **in place** instead:
`docs/TESTING.md`'s Open-Limit-Adjuster row and `docs/FINDINGS.md`'s unload grep both name
`logs/steam-12120.log`, so the trace has to still be *at that path*, just not 52 MB of it.
`RHYS_PROTON_LOG=0` is the only cap that is 0 bytes.

**The cost, paid on purpose.** A rotation can move the run you are reporting on out of the file the
tools read, so `scripts/diagnose.sh` and `tools/tour-report.py` now read `rhyremix.log` **and**
`rhyremix.log.prev`, oldest first, which keeps `tail -n` meaning "the newest N matches" across both.
`scripts/log-rotate-selftest.sh` (52 checks) is the proof: it rotates a fake tree whose log carries one
marked line per claim, then requires all nine lines `diagnose.sh` claims back, requires the newest live
line to beat an older `.prev` line, and requires all three `LOG_CATEGORIES` to still appear under a shot.
Four readers were deliberately left on the live file alone - `tools/perf-report.py --log` (it takes an
explicit path, splits runs on `---- log starts ----` and aligns plugin events to the CSV clock of the
run being measured), `scripts/check-rtx-probe.sh`, `scripts/lighting-test.sh`, and the launcher's own
`save_exit_evidence` tails - because all four ask about the run that just happened.

**Not measured.** Whether 8 MiB is the right cap: the largest single session log on this machine is
1,088,220 bytes, so 8 MiB is roughly eight sessions of the 2026-11-12 shape, and the number is a knob,
not a claim. And whether the launcher's own `logs/launch-*.log` (7 files, 1.6-6.2 KB each, unbounded in
count) needs the same treatment - recorded, not fixed, because 7 KB per start is not what made the
diagnosis slow.
