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
| haze: far/near local contrast | 0.46 – 1.18 | 1.22 – **3.67** | our near field has 2–4× **less** detail than the distance: flat ground under the camera, and no aerial perspective — **fog shipped by n17, after-numbers owed, see §6** |
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
| **T1** PBR materials | the pack's PBR maps **plus DLSS 5's learned materials** on an RTX 5090 | the same pack, counted exactly by n18's `tools/pack_pbr_census.py`: **2363** `mat_<HASH>` materials, of which **174** have a replacement albedo (147 normal, 121 rough, 71 height, **13 metal** keyed to a game texture); ~**2190** are legacy, and `rtx_materials.cpp` shows a legacy material can only carry a **constant** roughness — a map is impossible | **(a) shipped by n18:** `rtx.legacyMaterial.roughnessConstant = 0.65` in all three profiles — one step from the runtime default 0.7 onto the matte end of the pack's own measured paving band (wall 0.649, asphalt 0.634, pavement 0.607, ground 0.584; per-map median 0.582), while `rtx.opaqueMaterial.roughnessScale/Bias` are refused because they would rescale the pack's 121 rough maps too; (b) offline PBR-ify pass (normal+roughness from albedo) for the top ground/road textures by hash — the list is `build/tmp/n18/pack-uncovered.txt` (2197 hashes, ranked by USD references), and we already read and write DDS in `tools/water_normal_soft.py`; (c) probe `rtx.dlssNeuralRendering` and record the verdict | (a) 40 min · (b) 3–4 h · (c) 45 min | pack is **MIT** — patchable and re-hostable with credit; Proper Shaders is proprietary, link only |
| **T2** HD geometry / textures | an "8K texture pack" claim; the pack itself is 2K/4K; Remix 1.6 adds OpenEXR/HDR skyboxes and ComfyUI 2.0 AI texture pipelines | the pack's 2K albedo set (164 of 345 at 2048 px), `rtx.texturemanager.budgetPercentageOfAvailableVram = 40` on a 16 GB card, no HDR sky | measure the texture budget and the streaming cost of the pack's 4K set; HDR sky is pack/Toolkit work, not ours | 1–2 h to measure; the assets are upstream | MIT pack; the runtime zip is NVIDIA's EULA (`redistribute: ask` in `deps/manifest.json`) |
| **T3** atmosphere | Remix volumetrics; DLSS 5's foliage light scattering + volumetric control mask | `rtx.volumetrics.enable = True` and the froxel keys — but **no fog-remap keys at all**, so San Andreas' fixed-function fog never became volumetric (and the profile's `rtx.enableFogMaxDistanceRemap = False` was a dead name, so the option had always run at its default) | **shipped by n17:** `rtx.volumetrics.enableFogRemap = True`, input bounds `fogRemapMaxDistanceMin/MaxMeters = 400 / 4000` (the game's own `FarClp` range in `data/timecyc.dat`), output bounds `fogRemapTransmittanceMeasurementDistanceMin/MaxMeters = 2 / 6` (3.4–10.3 km of air), `fogRemapColorMultiscatteringScale = 0`, and the dead key renamed to `rtx.volumetrics.enableFogMaxDistanceRemap = True`. `enableHeterogeneousFog` deliberately left commented out. Targets far/near 0.5–0.9 and sky luma ≥ 150 are **not both reachable with this block** — see §6 and FINDINGS (n17) | 60–90 min + one tour | none (runtime knobs) |
| **T4** grading | warm filmic with real roll-off (ceiling 1–4 %, headroom 2.4–3.5), partly DLSS 5's tone model | `rtx.tonemap.finalizeWithACES = True`, but `tonemappingMode` left at its default **Local**, `localtonemap.shadows = 2.5`, auto-exposure off → headroom 1.4-1.8, ceiling ≤ 0.03 %, warmth −67…+10 | **shipped by n16:** `rtx.tonemappingMode = 0` (Global — not `rtx.tonemap.tonemappingMode`, which is not an option), `rtx.tonemap.exposureBias = -0.5` as the anchor, `localtonemap.shadows 2.5 → 1.0` and `highlights 4 → 2.0` for the Local A/B, `finalizeWithACES` kept. Targets: ceiling 0.5–3 %, headroom ≥ 2.0, warmth ≥ +15, warm share ≥ 70 % — **not yet measured**, see §6 | 60–90 min + measure | none (runtime knobs) |

**KEEP WHAT WORKS still applies.** Every one of these tasks must re-measure the shots the 2026-11-12
report called good (the sodium lamps at night, the sunset on the pier, the dawn) and show they did
not move except where the fix intends. `tools/target-gap.py` prints the same table for any set of
files, so "before" and "after" are two commands.

## 5. The plan, in queue order

| task | gap | one line | state |
|---|---|---|---|
| **n16-tonemap-highlights** | T4 | Global tonemap + an exposure anchor that lets highlights exist; the biggest measured number in the table | **config shipped, after-numbers owed** (see §6) |
| **n17-fog-aerial-perspective** | T3 | map San Andreas' fixed-function fog onto Remix volumetrics so distance fades instead of staying crisp | **config shipped, after-numbers owed** (see §6). The dead key it inherited (`rtx.enableFogMaxDistanceRemap`) is fixed; the medium is 10.3 km of air in clear weather instead of 344 km |
| **n18-legacy-material-brdf** | T1a | give the ~2190 materials that have no PBR maps a sane roughness instead of the runtime default | **config shipped, after-numbers owed** (see §6). Census, the decoded roughness of the pack's own maps, and the uncovered-hash list: FINDINGS (n18) and `build/tmp/n18/` |
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

### The n17 run: the fog remap is in the tree and has never been rendered

n17 put the atmosphere in `config/rtx.conf`, `config/rtx.conf.perf` and `config/rtx.conf.nopops`:
`rtx.volumetrics.enableFogRemap = True`, `enableFogMaxDistanceRemap = True`, the input bounds
`fogRemapMaxDistanceMin/MaxMeters = 400 / 4000`, the output bounds
`fogRemapTransmittanceMeasurementDistanceMin/MaxMeters = 2 / 6`, and
`fogRemapColorMultiscatteringScale = 0`. Like n16 and n22 it could **not** measure the result
(rule 1), so this subsection is the protocol. The before side is `build/tmp/n17/gap.json` (daytime)
and `build/tmp/n17/good-before.json` (night / sunset / dawn / the two Santa Maria water stops, which
are the pier material — there is no pier *video* in the repo or the game folder to take frames from).

0. Confirm the layer resolved, free, before anything else:
   ```
   grep -A120 'Effective RtxOption values' <game>/rtx-remix/logs/remix-dxvk.log | grep -i fogremap
   ```
   It must show `enableFogRemap = 1` and the four bounds. If it does not, either the profile was not
   installed (`tools/rtxprofile.sh current <game>`) or someone picked a **Volumetric Preset** in the
   Remix UI, which forces `enableFogRemap = False` (`rtx_global_volumetrics.cpp:313`).
1. Daytime, against the before file:
   ```
   python3 tools/target-gap.py <the 5 reference frames> <the 4 new daytime shots> \
       --json build/tmp/n17/gap-after.json
   ```
   Predicted (arithmetic, `build/tmp/n17/predicted.txt`): far/near 1.71 / 2.38 / 3.67 / 1.22 →
   roughly 1.2 / 1.7 / 2.6 / 0.85 with the far band at 1 km. The 0.5–0.9 target needs the ladder
   below, and it also needs the near band to stop being featureless — the ratio has two sides.
2. KEEP WHAT WORKS, before tuning: the six good shots plus the two water stops against
   `build/tmp/n17/good-before.json`. What must not move badly: the sodium-lamp night shots
   (02 p50 17.4 / 03 p50 7.0 / 04 p50 63.6), the sunset (08a 100.6 / 08b 86.3), and the pier-deck
   noon shot 10 (p50 149.7, sky luma 142.9 — the one this change can darken).
3. Ladder, one line per run, re-deriving the density with
   `python3 tools/fog-budget.py config/rtx.conf` before each:
   * distance still too crisp → `fogRemapTransmittanceMeasurementDistanceMaxMeters` 6 → 4 (6.9 km)
     → 3 (5.2 km) → 2 (3.4 km). Near-band cost at 50 m only goes 0.981 → 0.945, so the ground under
     the camera stays safe all the way down; the price is the sky (T at 4000 m: 0.219 → 0.103).
   * sky too dark, or night washed grey → the other direction: 6 → 12 (20.6 km) → 20 (34 km), then
     `enableFogRemap = False` for a clean A/B. There is no glow to turn off first: the ambient term
     is already 0, so the density is the only knob this block gives.
   * haze that reads grey instead of blue, or distance that should **glow** → that is in-scatter,
     not extinction: `rtx.volumetrics.froxelMaxDistanceMeters` 20 → 200–1000 m (same grid memory,
     coarser cells, and it changes the pack's lamp cones), and colour means a fixed
     `rtx.volumetrics.transmittanceColor` with `d_meas` re-derived — **never**
     `enableFogColorRemap`, whose σ = −ln(game fog colour)/d_meas puts the street inside 50 m at
     1e-9 transmittance for a dark fog colour.
   * a good shot moved badly and none of the above fixes it → **the change is wrong, not the shot**:
     delete the six keys of the n17 block (the `enableFogMaxDistanceRemap = True` line can stay, it
     is the default) or `tools/rtxprofile.sh current <game>`.
4. Eyes, in this order: whether the far skyline is paler than the near buildings **without** the
   pavement under the camera going soft; whether the sky went darker (it should, and how much is the
   whole argument of the ladder); whether the 21:00 lamps still read; whether the sea at stop 10 still
   has a horizon.
5. If the day/night split turns out to matter (bright haze at noon, dim haze at night): the remap is
   time-of-day blind because the pack's `FarClp` is 4000 m at every hour, and it cannot be gated from
   the plugin — `plugin/src/rtxconf.cpp` refuses every `volumetrics.` key at runtime (the 2026-11-11
   bridge crash). The honest route is the game's own data: write `FarClp` per hour into
   `data/timecyc.dat` and the medium follows the clock with no plugin involvement. That is a
   data-pack task (apply.sh territory), not a config edit.


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
name in the file is a name the runtime knows at all. **One correction since n22:** the exposureBias
line in that dump is only -0.5 while the plugin is not pushing — see the n22 subsection below.

### The n22 run: the exposure anchor now follows the sun (also never rendered)

n22 put the same option under plugin control: `plugin/src/exposure.{h,cpp}` blends
`[tonemap] night_ev → day_ev` over **solar elevation** and `sun.cpp` pushes it through
`remixapi_SetConfigVariable` next to `rtx.skyBrightness`, rate-limited at
`[tonemap] max_ev_per_sec = 0.05` EV/s. Like n16, it could not measure the result — rule 1 — so the
after-numbers below are owed by the same live run that owes n16's. What the code does prove, in
`tests/exposure_check.cpp` (table in `build/tmp/n22/curve.txt`): at 13:00 and 19:05 the plugin
publishes **-0.5**, i.e. exactly what `rtx.conf` carries, so **the four daytime shots and the two
sunset shots are unchanged by n22**; at 21:00 it publishes **+0.5** (2x the light) and at 05:30
**+0.089** (1.5x). Read the n16 protocol above with that split in mind.

1. Nothing to install for this: it is plugin behaviour, on by default, and the profile switch does
   not touch it. Confirm it is live first, cheaply:
   ```
   grep -m3 'config: tonemap exposure' <game>/rhyremix.log
   grep -m3 'exposure=' <game>/rhyremix.log | tail -1
   ```
   The first line is the ini's effective numbers (`day=-0.50 EV night=+0.50 EV … slew=0.050 EV/s`);
   the second is the 10 s sky line, `exposure=+0.500 EV (target +0.500, max step 0.005)` at a night
   stop. `exposure=` missing or stuck at -0.5 at 21:00 means the push is not happening.
2. Then the same two measurements as above, unchanged: the six good shots against
   `build/tmp/n16/good-before.json`, the four daytime shots against `build/tmp/n16/gap.json`. The
   daytime side is predicted unchanged; if it moved, the plugin is pushing at a daytime stop and
   that is a bug, not a look.
3. The knob to argue with is `[tonemap] night_ev` in `rhyremix.ini` (edit it while the game runs —
   the ini is re-read live — and the anchor walks to the new value at <= 0.05 EV/s):
   * night still darker than the baseline (02 p50 < 17.4 or > 34.5 % black, 03 < 7.0 / > 52.2 %) →
     raise it: `night_ev=1.0`, re-run;
   * night blown, lamps blooming, the sky line grey instead of black → lower it: `night_ev=0.0`,
     which keeps the machinery and removes the swing;
   * anything else → `apply=0` in `rhyremix.ini` stops the plugin writing the key from then on.
     One caveat, and it is the layer's, not the feature's: `SetConfigVariable` writes the runtime's
     **user** layer, which outranks `rtx.conf` and outlives the run — so if an earlier run pushed
     +0.5, `apply=0` alone does not bring the static −0.5 back. Either run once with `apply=1` at a
     noon stop (the plugin re-publishes −0.5, which is the same value the file wants anyway), or put
     the number back with the Remix UI's own Global Exposure control, which writes that same layer.
     `tools/rtxprofile.sh restore` puts `rtx.conf` back, not this layer.
4. The dump check, corrected: at a **21:00** stop the effective value should read **+0.5**, not
   -0.5, because the plugin writes the user layer, which outranks `rtx.conf`. At 13:00 it should
   read -0.5 — the same number from two directions, which is the no-op property in the log.

### The n18 run: the legacy-material BRDF constant is in the tree and has never been rendered

n18 put one live key in `config/rtx.conf`, `config/rtx.conf.perf` and `config/rtx.conf.nopops`:
`rtx.legacyMaterial.roughnessConstant = 0.65` (the runtime default is **0.7**). It could not render
it (rule 1), so this subsection is the protocol; the before side is `build/tmp/n18/gap.json`
(daytime, five reference frames) and `build/tmp/n18/good-before.json` (the six good shots plus the
two Santa Maria water stops).

Why one number is worth a task: `tools/pack_pbr_census.py` counted the pack read-only — **2363**
`mat_<HASH>` materials, **174** with a replacement albedo — and `src/dxvk/rtx_render/rtx_materials.cpp`
(`LegacyMaterialData::as()`, tag `remix-1.6.1`) shows a legacy material gets constants plus the
game's colour texture and nothing else: **no roughness map, no normal map, no per-texture anything**.
So ~2190 materials share one perceptual roughness, and the pack's own maps for the same surface
classes average 0.58–0.65 (wall 0.649, asphalt 0.634, pavement 0.607, ground 0.584, buildings 0.503,
props 0.419, floor 0.434). 0.65 is that band's matte end, one step from the default.

1. Confirm the layer resolved, free, before anything else:
   ```
   grep -A200 'Effective RtxOption values' <game>/rtx-remix/logs/remix-dxvk.log | grep -i legacymaterial
   ```
   It must show `rtx.legacyMaterial.roughnessConstant = 0.65`. If it shows 0.7, the profile was not
   installed (`tools/rtxprofile.sh current <game>`) or the Remix UI's Material section overwrote it
   into the user layer, which outranks `rtx.conf`.
2. Daytime, against the before file:
   ```
   python3 tools/target-gap.py <the 5 reference frames> <the 4 new daytime shots> \
       --json build/tmp/n18/gap-after.json
   ```
   Aim at the **blob** columns, not the area column: before, ours is 43–479 blobs / 128–1192 px
   largest against the reference's 67–213 / 64–324, while our *area* permille (4.0–27.6) already
   exceeds the reference's 2.9–12.9 because our bright pixels are lifted flat sky, not highlights.
   A good run makes the blobs more numerous-but-local and the largest blob smaller than 1192 while
   local contrast (0.131–0.214, reference 0.137–0.294) goes up. **The brake is the ceiling row:
   0.003–0.027 % before, and past 3 % this change is wrong.**
3. KEEP WHAT WORKS, before tuning: the six good shots against `build/tmp/n18/good-before.json`.
   What must not move: 02 p50 17.4 / 03 p50 7.0 / 04 p50 63.6 / 08a 100.6 / 08b 86.3 / 09 27.8, and
   above all their **ceiling, which is 0.000 % in every one of them** — a night shot that gains a
   sheen is the failure mode this task was warned about, and it shows up there first.
4. Ladder, one line per run:
   * paving/walls still read as one matte sheet, blobs still below the reference band →
     `0.65 → 0.62` (the pack's paving mean) `→ 0.58` (its overall median and its ground mean);
   * night gains a sheen, or noon ceiling > 3 % → back to `0.7`, and the fix belongs in the pack's
     material layer, not in this file;
   * roads look wet while walls/roofs/vegetation look wrong → that is the **uniformity**, not the
     value: there is no per-texture legacy knob (source, above), so it is the PBR-ify pass —
     `build/tmp/n18/pack-uncovered.txt` is the ranked list and [MIRRORS.md](MIRRORS.md) §7 the
     `over "RootNode" { over "Looks" { over "mat_<HASH>" } }` recipe;
     (`build/` is git-ignored: `tools/pack_pbr_census.py <pack> --no-decode --list-prefix \\
     build/tmp/n18/pack` rebuilds both lists in eight seconds;)
   * want a second constant for ground without touching the pack → the runtime has one:
     `rtx.terrainBaker.material.properties.roughnessConstant` (0.7) with `.roughnessAnisotropy` /
     `.metallicConstant`, gated by `rtx.terrainBaker.enableBaking` (True) and driven by
     `rtx.terrainTextures` hashes. SA blends two ground textures per world sector, which is what the
     baker exists for; it bakes a texture per frame, so it is a data + VRAM task, not a config line.
5. Eyes, in this order: whether the paving under the camera has **patches** of different gloss
   (it will not — that is the point of the next task) while the pack's own remastered surfaces stay
   as they were; whether chrome, glass and car paint are unchanged (they are replaced materials, so
   they must be); and whether the 21:00 sodium lamps still sit on a dark road.

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
