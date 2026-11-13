# TARGET — the video we want to beat, the measured gap, and the plan

Written by task **n15** (2026-11-12 night). rhY's rule for it:
*"We want our GTA to look better than this one."*

This page is deliberately honest about two things: what the reference actually is (a lot less
knowable than it looks), and which parts of its look we can buy with settings and assets versus
which parts come from hardware we do not own.

---

## 1. The reference

| | |
|---|---|
| video | **"GTA San Andreas DLSS 5 2026 Graphics Are INSANE! RTX 5090 DLSS 5 4K Max"** by DubStepZz |
| url | https://www.youtube.com/watch?v=BjWhv_gfjAk |
| published | 2026-11-12 09:15 (PT), 56:29 long, 16 k views when we read it |
| frame we measured | `build/tmp/target/dubstepzz-rtx5090-helipad.webp` — downtown Los Santos helipad, a Hydra, CJ in a tux, sunrise (06:20) |
| their machine | RTX 5090, 4K, "Max / Ultra", "Technology: DLSS 5" (their own description) |

### What the video tells us about its setup: almost nothing

* The description is SEO boilerplate. It names the game, the GPU, the resolution and "DLSS 5" —
  **no mod, no pack, no preset, no settings**. Its keyword list is GTA 6 spam.
* The "**4 products**" under the description are **YouTube's automatic shopping shelf**, not mod
  links: a PS5 Slim on StockX, an Xbox Series X bundle, a DualSense on Dell and an Xbox Elite
  controller on GameStop (they are `merchandiseItemRenderer` entries in the page's own data).
* The channel's About and Posts pages carry no links and no mod list. Its comment section is full
  of people asking the same question we did — *"Please share the mod list you've been using"*,
  *"Is this a mod?"*, *"How did you make this??"* — and the channel has never answered.

So the setup has to be **inferred**, and the inference is:

* The channel films both bases: one sibling video's description says
  *"Game: Grand Theft Auto: San Andreas – The Definitive Edition"*
  (https://www.youtube.com/watch?v=Ijm1NFWOvnE), while the reference video says plain
  *"Grand Theft Auto: San Andreas"* and its sibling
  https://www.youtube.com/watch?v=2PIZWpuepKA promises *"DLSS 5 / 8K Texture Pack / Ray Tracing
  Lighting … while keeping the classic San Andreas experience"*.
* **Classic SA + RTX Remix is the only combination that has all three** (ray-traced reflections,
  texture packs, and DLSS 5): RTX Remix **1.6** integrates DLSS 5 as a rendering stage
  (https://github.com/NVIDIAGameWorks/rtx-remix/releases, tag `remix-1.6.1`, 2026-10-23:
  *"DLSS 5 Integration: Integrated DLSS 3D-Guided Neural Rendering and added live artistic
  controls"*). The Definitive Edition has no ray tracing at all.
* The community's SA Remix setup is one pack: **https://github.com/Hemry81/GTASA-Remix** (MIT,
  10.3 GB, last pushed 2025-09-20). The benchmark channels that made the well-known SA path-tracing
  videos link that pack and nothing else
  (https://www.youtube.com/watch?v=o91lq7tVS0c, https://www.youtube.com/watch?v=EhRw1uSoG1c).
  **That is the same pack we run.**

**Conclusion: we are already running their base.** The gap is not "which mod did they install".

## 2. Where their look actually comes from

DLSS 5 is not an upscaler. NVIDIA's own description
(https://www.nvidia.com/en-us/geforce/news/dlss5-breakthrough-in-visual-fidelity-for-games/,
2026-03-16, and the paper page https://research.nvidia.com/labs/adlr/DLSS5/):

> a **one-step pixel-space diffusion model** … conditioned on the current rendered frame, engine
> motion vectors, carried temporal state, and artistic-direction values … the first DLSS technology
> to **generate the final displayed appearance** rather than reconstruct a higher-cost reference
> output … runs locally as a rendering stage … **on GeForce RTX 50 Series GPUs**.

It invents the things real photos have and games fake: *"subsurface scattering in skin and light
scattering through foliage"*. That is a large part of T1 (materials), T3 (atmosphere) and T4
(grading) arriving for free on someone else's frame.

**And it is not available to us.** This box is an **RTX 4070 Ti SUPER, driver 610.57.04**
(`nvidia-smi`); DLSS 5 is RTX 50-series only, and our own perf profile already records the runtime
answering `NVIDIA DLSS-NR support query failed (0xbad00010)` under this Proton prefix. The keys do
exist in our runtime (1.6.1) — `rtx.dlssNeuralRendering`, `intensity`, `structuralStrength`,
`toneStrength`, `model` (A/B/C), `useAutoMask`, `skinStructureStrength`, `enableHighlightRecovery`,
`highlightRecoveryThresholds`, `enableVolumetricControlMask` — task **n19** probes whether any of
them do something here, and the answer goes in the docs either way.

So the plan is: **take the look through the renderer we do control** — materials, fog, grading —
and never pretend a DLSS 5 switch will do it for us.

## 3. The measured gap

`tools/target-gap.py` measures six families the same way on both sides. Everything is resized to
640×360 and round-tripped through JPEG q72 first, because the reference is a compressed 1280×720
YouTube frame and our shots are lossless 1920×1080 PNGs — compression alone moves the local-contrast
and specular numbers more than most lighting changes do.

Reference = 5 frames from that channel (the 06:20 helipad frame, the target video's 25 %/50 %
keyframes and thumbnail, plus the 8K-texture video's thumbnail).
Ours = the four **daytime** stops of the 2026-11-12 10:22 tour: `pershing-noon` (downtown, the
direct comparison), `grove-noon`, `vinewood-noon`, `carmirror-1300`.

| metric | reference | ours (daytime) | what it means |
|---|---|---|---|
| mid-tone anchor p50 | 65 – 142 (≈ 108) | 123 – **170** (≈ 147) | our whole image sits ~40 luma too high |
| top of the curve p99.9 | 227 – 255 | 224 – 246 | we never get there |
| **headroom p99.9 / p50** | **1.79 – 3.49** | **1.41 – 1.81** | the single biggest number in the table |
| ceiling % (luma ≥ 252) | 0.01 – 3.95 % | 0.003 – 0.027 % | 100–1000× fewer bright pixels — we have no highlights |
| warmth mean(R−B), mid-tones | **+18.7 … +48.3** | **−67.2 … +10.2** | every reference frame is warm; three of our four are blue |
| warm-hue share of mid-tones | 81 – 95 % | 6 – 53 % | same story, hue-wise |
| saturation (mid-tones) | 0.17 – 0.41 | 0.20 – 0.47 | we are more saturated exactly where they are not |
| sky luma / sky saturation | 160–199 / **0.13–0.26** | 92–128 / **0.28–0.61** | their sky is bright and pale; ours is dark and deep blue (in the downtown shot the sky is *darker* than the street) |
| haze: far/near local contrast | 0.46 – 1.18 | 1.22 – **3.67** | our near field has 2–4× **less** detail than the distance: flat ground under the camera, and no aerial perspective |
| local contrast (normalised) | 0.137 – 0.294 | 0.131 – 0.214 | not the gap |
| specular area ‰ (luma ≥ p95) | 2.9 – 12.9 | 4.0 – 27.6 | we score *higher* while having no real highlights — our "bright" pixels are lifted flat sky |

Two extra facts worth keeping:

* The reference's haze ratio ≈ 1.0 means **it has no aerial perspective either** — a viewer in its
  own comments says so in Russian (*"terrible that there is no haze and the whole map is laid out
  like the palm of your hand … this DLSS5 only hurts San Andreas"*). Beating it there is cheap.
* Our own diagnosis of the flat bright end already exists in
  [FINDINGS.md](FINDINGS.md) (n05/n10: `rtx.tonemappingMode` defaults to **Local**, whose
  mip-pyramid fit crushes the bright end and is view-dependent; `rtx.localtonemap.shadows = 2.5`
  lifts the floor). n15 confirms it on a **daytime** frame, where nobody had looked.

## 4. The gap table (T1–T4)

| # | what they use | what we have | concrete fix | effort | licence / hosting |
|---|---|---|---|---|---|
| **T1** PBR materials | the pack's PBR maps **plus DLSS 5's learned materials** on an RTX 5090 | the same pack: **2334** `mat_<HASH>` materials, of which only ~300 have maps (315 `_normal`, 280 `_rough`, 185 `_height`, **32 `_metal`**); the rest get the runtime's default legacy BRDF | (a) turn the runtime's own material knobs: `rtx.legacyMaterial.roughnessConstant` / `.metallicConstant` / `.roughnessAnisotropy`, `rtx.opaqueMaterial.roughnessScale/roughnessBias/metallicScale/metallicBias`; (b) offline PBR-ify pass (normal+roughness from albedo) for the top ground/road textures by hash — we already read and write DDS in `tools/water_normal_soft.py`; (c) probe `rtx.dlssNeuralRendering` and record the verdict | (a) 40 min · (b) 3–4 h · (c) 45 min | pack is **MIT** — patchable and re-hostable with credit; Proper Shaders is proprietary, link only |
| **T2** HD geometry / textures | an "8K texture pack" claim; the pack itself is 2K/4K; Remix 1.6 adds OpenEXR/HDR skyboxes and ComfyUI 2.0 AI texture pipelines | the pack's 2K albedo set (164 of 345 at 2048 px), `rtx.texturemanager.budgetPercentageOfAvailableVram = 40` on a 16 GB card, no HDR sky | measure the texture budget and the streaming cost of the pack's 4K set; HDR sky is pack/Toolkit work, not ours | 1–2 h to measure; the assets are upstream | MIT pack; the runtime zip is NVIDIA's EULA (`redistribute: ask` in `deps/manifest.json`) |
| **T3** atmosphere | Remix volumetrics; DLSS 5's foliage light scattering + volumetric control mask | `rtx.volumetrics.enable = True` and the froxel keys — but **no fog-remap keys at all**, so San Andreas' fixed-function fog never becomes volumetric | `rtx.volumetrics.enableFogRemap = True` + `fogRemapMaxDistanceMin/MaxMeters` + `fogRemapTransmittanceMeasurementDistanceMin/MaxMeters` + `fogRemapColorMultiscatteringScale` (and try `enableHeterogeneousFog`); target far/near 0.5–0.9 and sky luma ≥ 150 | 60–90 min + one tour | none (runtime knobs) |
| **T4** grading | warm filmic with real roll-off (ceiling 1–4 %, headroom 2.4–3.5), partly DLSS 5's tone model | `rtx.tonemap.finalizeWithACES = True`, but `tonemappingMode` left at its default **Local**, `localtonemap.shadows = 2.5`, auto-exposure off → headroom 1.4-1.8, ceiling ≤ 0.03 %, warmth −67…+10 | **shipped by n16:** `rtx.tonemappingMode = 0` (Global — not `rtx.tonemap.tonemappingMode`, which is not an option), `rtx.tonemap.exposureBias = -0.5` as the anchor, `localtonemap.shadows 2.5 → 1.0` and `highlights 4 → 2.0` for the Local A/B, `finalizeWithACES` kept. Targets: ceiling 0.5–3 %, headroom ≥ 2.0, warmth ≥ +15, warm share ≥ 70 % — **not yet measured**, see §6 | 60–90 min + measure | none (runtime knobs) |

**KEEP WHAT WORKS still applies.** Every one of these tasks must re-measure the shots the 2026-11-12
report called good (the sodium lamps at night, the sunset on the pier, the dawn) and show they did
not move except where the fix intends. `tools/target-gap.py` prints the same table for any set of
files, so "before" and "after" are two commands.

## 5. The plan, in queue order

| task | gap | one line | state |
|---|---|---|---|
| **n16-tonemap-highlights** | T4 | Global tonemap + an exposure anchor that lets highlights exist; the biggest measured number in the table | **config shipped, after-numbers owed** (see §6) |
| **n17-fog-aerial-perspective** | T3 | map San Andreas' fixed-function fog onto Remix volumetrics so distance fades instead of staying crisp | queued — and read §6 of FINDINGS first: `rtx.enableFogMaxDistanceRemap` in our profiles is a **dead key**, the option is `rtx.volumetrics.enableFogMaxDistanceRemap` and it has been running at its default True |
| **n18-legacy-material-brdf** | T1a | give the ~2000 materials that have no PBR maps a sane roughness/metallic instead of the runtime default | queued |
| **n19-dlss5-neural-probe** | T1c/T4 | find out whether `rtx.dlssNeuralRendering` does anything on an RTX 4070 Ti SUPER under Proton, and write the answer down either way | queued |

## 6. Re-measuring this at any time

```
python3 tools/target-gap.py \
    build/tmp/target/dubstepzz-rtx5090-helipad.webp \
    "<game>/screenshots/shots/shot_2026-11-12_10-27-48.png" \
    --label reference --label ours-pershing-noon --json build/tmp/n15/gap.json
```

Options: `--scale WxH` (default 640x360, `0` = native), `--jpeg Q` (default 72, `0` = no
round-trip). Use the same options on both sides or the comparison is meaningless. The tool's
docstring defines every metric and its constants; `tests/test_target_gap.py` keeps the definitions
from drifting.

### The n16 run: the tonemap change is in the tree and has never been rendered

n16 shipped `rtx.tonemappingMode = 0`, `rtx.tonemap.exposureBias = -0.5`,
`rtx.localtonemap.shadows 2.5 → 1.0` and `rtx.localtonemap.highlights 4 → 2.0` in
`config/rtx.conf`, `config/rtx.conf.perf` and `config/rtx.conf.nopops`, and it could **not** measure
them: the header rule of every task in this repo forbids running the game, wine or Steam, and a
tonemap change only becomes a number once the runtime draws a frame. So the after-numbers below are
owed by whoever runs the next tour. Nothing else in this page changes; this section is the protocol.

1. Install the profile and take the tour (the 16-stop tour, so the shots line up with the baselines):
   `tools/rtxprofile.sh perf <game>` then `scripts/shot.sh` / the tour as usual.
2. Daytime, against `build/tmp/n15/gap.json` (the before side is also in `build/tmp/n16/gap.json`):
   ```
   python3 tools/target-gap.py <the 5 reference frames> <the 4 new daytime shots> \
       --json build/tmp/n16/gap-after.json
   ```
   Targets (docs/TARGET.md §4 T4): **ceiling 0.5-3 %, headroom >= 2.0, p50 100-130, warmth >= +15,
   warm-hue share >= 70 %**.
3. KEEP WHAT WORKS, first thing, before tuning anything else — `build/tmp/n16/good-before.json` is
   the baseline (02 p50 17.4 / 03 p50 7.0 / 04 p50 63.6 / 08a 100.6 / 08b 86.3 / 09 27.8):
   ```
   python3 tools/target-gap.py <the six new good shots> --json build/tmp/n16/good-after.json
   ```
4. Abort ladder, in this order, one change per run:
   * night shots darker than the baseline (02/03 below p50 17.4 / 7.0, or % black above 34/52) →
     `rtx.tonemap.exposureBias = 0`, keep the mode change, re-run;
   * p50 still above 130 at noon with the anchor at 0 → `rtx.tonemap.exposureBias = -0.5` again and
     move the curve instead: `rtx.tonemap.shadowMinSlope` / `shadowContrast` (both default 0);
   * sun, chrome or wet asphalt posterised (ceiling > 3 % with a hard clip, `% clip` climbing while
     `roll-off band %` falls) → try `rtx.useLegacyACES = False` (the refined ACES; the default True
     is the variant whose tooltip says it "over-saturates the highlights");
   * the frame still re-exposes when the camera turns, or the whole image brightens when the sky is
     in frame → that is the histogram fit itself: put `rtx.tonemappingMode = 1` back with
     `shadows = 1.0 / highlights = 2.0` and compare — that pair is why the local values were retuned
     instead of left at 2.5 / 4.0;
   * if a good shot moves badly and none of the above fixes it, **the change is wrong, not the
     shot**: `tools/rtxprofile.sh current <game>` and the n16 block comes out of the three profiles.
5. What to look at with your own eyes, in this order: whether the sun, chrome and wet patches are
   **bright** rather than grey; whether the far skyline is paler than the near buildings; whether the
   daytime frame still looks like San Andreas at noon and not like a blue-tinted flat render; and
   whether the sodium lamps at 21:00 still read (they are the ones this change can break).

One more thing to check on that first run, because it is free: the runtime's own dump of what the
layers resolved. `grep -A80 'Effective RtxOption values' <game>/rtx-remix/logs/remix-dxvk.log` should
show `rtx.tonemappingMode = 0` and `rtx.tonemap.exposureBias = -0.5`. If a key is missing there, the
layer below it is not being read — and `tools/rtx-option-audit.py` is the tool that says whether the
name in the file is a name the runtime knows at all.

After those, the two bigger bets are recorded but not scheduled: an offline **PBR-ify pass**
(generate normal/roughness for the most-seen ground and road textures, patch the pack's material
layer with a backup, MIT lets us) and a **texture-budget pass** (what the pack's 4K set costs on
16 GB, and whether the 40 % texture-manager budget is the right number). A third one came out of
n16: a **static EV anchor cannot serve noon and 21:00 at once** — the plugin already pushes
`rtx.skyBrightness` per frame (`plugin/src/sun.cpp:542`) and `remixapi_SetConfigVariable` takes any
real option at runtime, so a time-of-day `rtx.tonemap.exposureBias` is the shape of the fix, but it
needs the n16 numbers first. Queued as `.strata/n22-daynight-exposure-anchor.md` (immediately after
n16 in `.strata/night-queue.txt`), with the night numbers it has to beat and the rate-limiter rule
written into it.
