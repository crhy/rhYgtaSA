# STATUS — what rhY reported, and what owns it

Written by the n14 documentation sweep (2026-11-12). One page that answers: **which of the
things rhY reported in the 2026-11-12 test sessions are fixed, which are only measured, and
which are still open** — with the commit or task that owns each, and what the next run has to
look at.

Statuses mean exactly this:

| | |
|---|---|
| **done** | the change is committed and gated (`scripts/check-all.sh`); it has **not** been seen in game yet |
| **partly** | one half of the finding is committed, the other half is not |
| **measured** | the cause is pinned down with numbers and a test, but nothing shipped (the work is parked) |
| **open** | nobody has worked on it |

Nothing on this page is verified in game until rhY's next session says so. The run order for
that session is [NEXT-TEST.md](NEXT-TEST.md); the evidence behind every row is
[FINDINGS.md](FINDINGS.md); the change history is [../CHANGELOG.md](../CHANGELOG.md).

---

## Session 1 — 2026-11-12 00:48-01:13 (16-stop tour, build `67c6e70`+`2f67e4a`)

rhY's goal for the day: *every light must come from a real source; no fake ambient, no popping.*

| # | finding | status | owner | how the next run checks it |
|---|---|---|---|---|
| 1 | the whole world's lighting changes **instantly** while running or driving (most important) | **done** | s28 `129f2aa` (`plugin/src/smooth.cpp`: fade envelopes, create budget, rate limiters) + n01 `ad323a9` (handle cadence) | `rhyremix.log`: `steps=`/`created=` bounded, `in_drop=0`; `tests/smooth_check.cpp`, `tests/handle_check.cpp` |
| 2 | time of day flickers and does not progress smoothly | **done** | s29 `9e67d91` (`plugin/src/tod.cpp`, calendar gate, per-frame solar position) | the `tod=` / `cal:` fields in the 10 s line; sun moves ≤ 0.25 deg/game-minute |
| 3 | the world went dark while riding on a taxi roof, then came back | **done** (unverified) | s28 `129f2aa` — `rhyPlayerPosGrace()`: the ped pointer reading 0 no longer destroys the light set | `player_gone_frames=` non-zero with no dark frame; `player_grace_ms=8000` |
| 4 | traffic-signal **bulbs missing**; the ambient glow is **wanted** ("both like real life") | **done** (unverified) | s30 `bd7fdf4` (hook install fix, bulb), n02 `ee59b87` (bulb moved 2.10 m onto the lens), n02b `ba4c2c3` (glow put back, flux 304) | one `ok bulb` **and** one `ok glow` line per lit head; `N heads lit (N with a glow)` |
| 4b | the bulb is a little too bright (session 2, C) | **in tree, awaiting a run** | n23 (this tree): `bulb_radiance 1200 → 840` (−30 %), and a `[trafficlights] radiance_scale` (ships 1.0) so the take-down cannot leak into the glow — the street knob used to reach the bulb and the glow through `trafficlights.cpp:411,423` | the log's `config: trafficlights … radiance=840 nits … scale=1.00`; `tools/night-brightness.py` on stops 06/07 (53.5 % / 46.8 % today) |
| 5 | water still looks terrible (white/black speckle; black underwater) | **partly** | s32 `1769fa1` (`water.sh soft`, sheet 44° → 12°), n07 `913b7e4` (`remix-ref`), n13 `1af994f` (installed by default + its `rtx.conf` keys). The **edge flicker** is not fixable in 1.6.1: the sea is 307 exactly-butted coplanar polygons (~6000 seams) and the runtime has no hit tolerance | stop 11 mean up from 6.8, black down from 29.2 % |
| 6 | vegetation looks like scribbled lines; rhY MUST-HAVE #2 ("reads as real plants") | **partly** | s32 `1769fa1` tagged 119 card textures as `rtx.hairCardTextures`; n21 (this tree) adds the pack's own 234 cards (`vegetation.sh --step 6`), the `lod_veg_firs.txd` shim (`--step 5`), the streaming/IDE/edge tools and the wind-shader dependency finding — none seen in game yet | stop 14 `mid_alpha` **below 17.63 %** and ramp **below 2.59 px** (`tools/vegetation_edge_measure.py`); mean/sat up from 168.1 / 0.262; no transparent trees (cause 4) |
| 7 | tour shots were unusable (camera in a wall, CJ dropped into the sea, no car, no peds; `--since` re-reported yesterday) | **done** | s31 `295b2ac` (five stops re-placed from the IPL; `split_sections`/`pick_section`) | `paired \| 16 of 16 manifest entries`, no `stop N failed:` |

## Session 2 — 2026-11-12 10:22-10:45 (build `ba4c2c3` + `water.sh soft` + 119 hair-card hashes)

| | finding | status | owner | how the next run checks it |
|---|---|---|---|---|
| A | **lamps flicker in and out as the camera rotates**; must be on for the whole streamed area | **done** (unverified) | n04 `8c6a198` — the count cap may only trim the fade band (`hardRadius`), signal heads kept by player distance, and the eight `rtx.conf` sampling keys | a 360° camera spin must change nothing; `in_drop=0` in every 10 s line; `tests/always_on_check.cpp` |
| B | facing the sun at sunset the frame goes **gray**; facing away it is perfect | **in tree (n16), awaiting a run** | n05 parked `6eafd4d` found it — `rtx.tonemappingMode` defaults to **Local**, whose crushed branch (×2⁻⁴) plus a per-tile grey multiplier over a mip pyramid removes the bright end. n16 `b1afcc2` shipped the root fix (`rtx.tonemappingMode = 0`, Global, + ACES); n05's `rtx.localtonemap.highlights = 1.5` is **not** re-applied on top of n16's 2.0 | sky-band p95−p05 (102 → 48 measured), % pixels ≥ luma 191 (4.5 % → 0.03 %); `tools/night-brightness.py --sunset 08a-sunset-facing-east-1905 08b-sunset-facing-west-1905` |
| C | street lamps too bright, nights not dark enough, bulb a little too bright, glow good | **in tree, awaiting a run** | n23 (this tree) re-derived the lighting half of the parked n05: `[streetlights] radiance_scale 1.0 → 0.4`, bulb 840, **glow kept** (`glow_flux=304`, `glow_radius=3`) and decoupled from the street knob, `[vehiclelights] radiance_scale=1.0` so the headlamps do not ride the lamp take-down. The `rtx.localtonemap.shadows` ramp is **not** shipped — n16 turned the local tonemapper off, so that lever is gone. Measured before: **57.3 % of noon** under a lamp, **72.0 %** of the frame lit; the model puts 0.4 at 30.0 % | `python3 tools/night-brightness.py $G --target 25 --predict 0.4` (exits non-zero on the stop that misses); if it still misses, the number to move is `[tonemap] night_ev` → 0.0, not the lamps |
| D | pier/boardwalk lamp **heads** are dark: no lit bulb, no pool under them | **in tree, awaiting a run** | n11 (this tree): the lamp list is the game's own 2dFx light records (`tools/dff_2dfx.py` → 8962 lamps, LA coverage **74.0 % → 100 %**), each bulb keeps the colour the game recorded, and every lamp gets a second small unshaped light at the bulb (`[streetlights] head_glow`) | NEXT-TEST step 9 at the pier; `tools/lamp-coverage.py --region LA --min 95` prints `PASS coverage`; the log's `N active (M with a lit bulb)` |
| E | highway-sign bulbs do not light; the sign's words are separate textures **casting shadows** | **open** | n06 aborted `48d93d5` (the `rtx.decalTextures` route was never tried) | photograph one sign at night |
| F | water still bad and flickers on some edges; foliage still bad | **partly** | see session 1 rows 5 and 6, plus n07 `913b7e4` (the research and the animated-water keys) | stop 10/11, stop 14 |
| G | fps a little less steady than the previous build, stabilising as he played | **ours bounded, renderer half awaiting a run** | n08 (this tree). Measured with `tools/perf-report.py`: the mean got *better* (17.72 → 17.50 ms) and the tail got worse (76.9 → 137.2 spikes/min; first 300 s 24.0 → 22.4, after 300 s **90.6 → 187.8**). Our churn is ruled out twice (r = −0.37 / −0.52 against the spike series; the 10:22 run's worst minute has 1098 spikes with ~100 live lights and no creates; our whole periodic burst measures 20 µs). What we did own and fixed: the destroy flush (320 / 205 handles released inside one `Present`) is now `[fade] destroys_per_frame=32`, and the two lamp layers no longer scan on the same frame. The renderer half — 119 hair-card cutouts becoming opacity-micromap candidates — is bounded by six new `rtx.conf.perf` keys and needs the A/B | `python3 tools/perf-report.py <csv> --log rhyremix.log --gpu-csv <gpu.csv> --series`: **after-300 s spikes/min** is the number to bring back (187.8 → under 90.6); `config: fade … destroys/frame=32`; `remix: … due_destroy=/deferred_destroy=` |
| H | research how other Remix projects do water and foliage | **done** | n07 `913b7e4` (Portal RTX / GTA IV / Minecraft / GTA 2 table in FINDINGS) | — |
| + | the game hung ~2 min (and once 16 min) after Quit | **done** (unverified) | n09 parked `08dc373` → n09b `cb0aac8`: `plugin/src/shutdown.cpp` latch + the launcher ends the wine tree | `shutdown: suspect / bridge gone` in the log; the game must die within `RHYS_EXIT_GRACE` (20 s) |

## Session 3 — 2026-11-12 23:42-23:56 video, Verona Beach / Santa Maria pier (build `ba4c2c3`)

| | what the video shows | status | owner |
|---|---|---|---|
| V1 | fake flat night ambient: sand, palms, walls and peds lit evenly green-grey, no source; the sky is black and the ground bright | **in tree, awaiting a run** | n24 (this tree) ships causes 2-4 of the four n10 parked in `30a1a43`: the night ambient capped at `[sky] ambient_cap_mult = 0.25` x the moon light's own published radiance (it was 0.0700 against a moon of 0.0669 - 57.6 % of open-sand light from "sky"), the fallback light off in every profile (a 3 m sphere at the camera, **8.6:1 against the moon**), and `solarClampLightDir()` so a distant light can never shine up out of the ground. Cause 1, the local tonemapper (90:1 squeeze), shipped as n16. Sim: scene ratio 4.1 %, image ratio **7.4 %** (was 50.6 %) |
| V2 | boardwalk and pier lamps dark: no glowing head, no pool | **in tree, awaiting a run** | n11 (this tree: the 2dFx lamp list + the head light), n06 aborted `48d93d5` |
| V3 | exposure changes with view direction: the sand jumps brighter/darker in 3 s of turning | **in tree, awaiting a run** | n16 shipped the cause: `rtx.tonemappingMode = 0` takes the local mip-pyramid fit (ground band 88.6 -> 146.6 -> 96.2 luma with no light changing) out of the chain; n24 keeps the night's dark end with `rtx.tonemap.shadowContrast = 0.4` instead of a view-dependent floor. `tests/test_night_ambient.py` fails if mode comes back or the lift goes above 1.0 |
| V4 | water and the pier's underside glow teal by themselves | **in tree, awaiting a run** | n24 (this tree): `rtx.enableTransmissionApproximationInIndirectRays = False` (the runtime's own default; the `True` came from `88e8cd1` unmeasured) and `rtx.bloom.luminanceThreshold = 1.0` - measured on the 60 frames, **10.31 %** of the sea/pier band is above the 0.25 default and blooms at full weight, **0.23 %** is at or above 1.0 |
| V5 | tail lamps throw wide red blobs; headlights a small weak pool | **done** (unverified) | n12 `404f2cd`: the car's own dummies from the corona store → 0.09 m reflector in a 50° cone + 0.05 m red lenses |
| V6 | exhaust smoke renders as bright pink/white glowing puffs | **done** (unverified) | n12 `404f2cd`: `rtx.emissiveBlendOverrideEmissiveIntensity 0.2 → 0.06` + the three smoke sprites into `rtx.particleTextures` |
| V7 | some windows are flat overbright white blocks | **partly** | n12 `404f2cd` cut the **global** emissive (`emissive_day 0.5`, `emissive_night 0.8`); per-window control belongs in the pack's `mat_<HASH>` materials, which this repo does not touch |

---

## The quality target (n15, 2026-11-12) — "look better than the DLSS 5 showcase"

| gap | what the measurement says | status | owner |
|---|---|---|---|
| **T4 grading** | headroom p99.9/p50 1.41–1.81 vs the reference's 1.79–3.49; ceiling pixels 0.003–0.027 % vs up to 3.95 %; warmth −67…+10 vs +18.7…+48.3 | **config shipped by n16 + plugin anchor shipped by n22, after-numbers owed** — neither has ever been rendered (rule 1 forbids running the game here) | n16 shipped `rtx.tonemappingMode = 0` + `rtx.tonemap.exposureBias = -0.5` + `localtonemap.shadows 1.0` / `highlights 2.0` in all three profiles; n22 drives that same option from solar elevation at runtime (`[tonemap] day_ev -0.5 → night_ev +0.5`, ≤ 0.05 EV/s), so noon is unchanged and 21:00 gets 2× the light. The run and the abort ladder are [TARGET.md §6](TARGET.md); the mechanism is FINDINGS ("The tonemapper…", "One exposure anchor cannot serve noon and 21:00") |
| **T3 atmosphere** | haze far/near local contrast 1.22–3.67 vs 0.46–1.18; sky 92–128 luma at saturation 0.28–0.61 vs 160–199 at 0.13–0.26 | **config shipped by n17, after-numbers owed** — never rendered (rule 1) | n17 shipped `rtx.volumetrics.enableFogRemap = True` + input bounds 400/4000 m (the game's own `FarClp` in `data/timecyc.dat`) + output bounds 2/6 m (3.4–10.3 km of air, was 344 km) + `fogRemapColorMultiscatteringScale = 0` in all three profiles, and fixed the dead `rtx.enableFogMaxDistanceRemap`. Predicted far/near 3.67 → ~2.6 at 1 km (`build/tmp/n17/predicted.txt`); the 0.5–0.9 target also needs the near band to have detail (T1) and the sky target needs in-scatter, which 1.6.1 only has inside 20 m — see FINDINGS (n17) and [TARGET.md §6](TARGET.md) |
| **T1 materials** | counted by n18's `tools/pack_pbr_census.py`: **2363** `mat_<HASH>` materials, a replacement albedo for **174** of them (147 normal, 121 rough, 71 height, **13 metal** keyed to a game texture) — so ~**2190** render with the runtime's default legacy BRDF, which `rtx_materials.cpp` proves can only be a **constant** roughness, never a map | **config shipped by n18 + the first 35 real maps shipped by n20b, after-numbers owed** — never rendered (rule 1) | n18 shipped `rtx.legacyMaterial.roughnessConstant = 0.65` in all three profiles: one step from the runtime default 0.7 onto the matte end of the pack's own decoded paving band (wall 0.649, asphalt 0.634, pavement 0.607, ground 0.584; per-map median 0.582), and refused `rtx.opaqueMaterial.roughnessScale/Bias` because they would rescale the pack's 121 rough maps too. **n20b then generated the maps the census said were missing** for the 40 surfaces with the most screen time: `bash tools/textures.sh on`, 35 sets (albedo/normal/rough/height, BC3/BC5/BC4, mip chains, 91.9 MB), the 5 the pack already covers better skipped by design — [TEXTURES.md](TEXTURES.md), FINDINGS (n20 → n20b), NEXT-TEST step 8. The next step is the same pipeline over the next 100 ranks (`--top 100`) |
| **T2 HD assets** | pack albedo is mostly 2048 px; the "8K pack" claim is the channel's, not verifiable | **measured** | census in FINDINGS; the asset work is upstream (MIT pack) |
| **DLSS 5 itself** | the keys exist in our 1.6.1 runtime (`rtx.dlssNeuralRendering.*`, 11 options), but the runtime's own capability query fails on this box: `NVIDIA DLSS-NR support query failed: (code: 0xbad00010, info: NVSDK_NGX_Result_FAIL_UnsupportedParameter)` — RTX 4070 Ti SUPER, driver 610.57.04, below Remix 1.6's own 616.92 baseline, while the model DLL (`<game>/.trex/nvngx_dlssnr.dll`, 266 MB) is present | **measured — unavailable** (n19) | n19: the query runs on **every** run (`rtx_ngx_wrapper.cpp:226`), so the log line above *is* the measurement; the probe profile `config/rtx.conf.dlss5` (`tools/rtxprofile.sh dlss5`) stays non-shipped and no shipped profile may carry a DLSS-NR key (`tests/test_dlss5_probe_keys.py`). Re-check after any driver update with `grep -iE 'DLSS-NR|Neural' <game>/rtx-remix/logs/remix-dxvk.log` — [TARGET.md §6](TARGET.md). Nothing here depends on it: T1/T3/T4 are won with materials, fog and grading |

The reference, what it publishes (nothing) and the full table are in
[TARGET.md](TARGET.md); the evidence is in [FINDINGS.md](FINDINGS.md), "The reference video we are
told to beat", "The tonemapper, and the keys our own profiles had been writing wrong" and
"Aerial perspective…" (n17). The second of those carried a warning for T3: three keys our own
profiles had been carrying are **not options of 1.6.1** (`rtx.enableFogMaxDistanceRemap`,
`rtx.antiCulling.object`, `rtx.antiCulling.light`) and had therefore never done anything. n17 fixed
the fog one; the two anti-culling ones stay deliberately unrenamed (enabling them keeps up to 10000
instances in the TLAS — a VRAM call). The list with its reasons is `config/rtx-dead-keys.txt`.

## Parked and aborted work

| branch | task | why it is not in the tree |
|---|---|---|
| `failed/n09-exit-hang-114604` (`08dc373`) | n09 exit hang | 45 of 46 selftest checks passed; the failure was `scripts/test-session-selftest.sh`'s real-Strata guard losing a race. Redone as n09b (`cb0aac8`), which is in the tree |
| `failed/n05-night-brightness-185753` (`6eafd4d`) | n05 nights and sunset gray | `scripts/test-session-selftest.sh` **timed out** (exit 124). The lighting work itself was complete and is described in FINDINGS |
| `failed/n10-night-ambient-realism-172920` (`30a1a43`) | n10 fake night ambient | same selftest failed (exit 1). Its four causes and its numbers are recorded in FINDINGS |
| `failed/n20-hd-pbr-textures-183453` (`a96ec15`) | n20 HD PBR textures (MUST-HAVE #1) | 87 of 89 checks passed; failed `tests/test_next_test_doc.py` (its NEXT-TEST rewrite broke the one-screen limits) and `tests/test_changelog_coverage.py`. **Finished by n20b, which is in the tree**: `tools/texture_rank.py`, `tools/pbrify.py`, `tools/bcn.py`, `tools/textures.sh`, `tools/usd_switch.py`, `config/texture-pbr.json` and the five test files, plus [TEXTURES.md](TEXTURES.md) and the measurements n20 never got to make |
| `failed/n03-next-session-ready-235920` (`b3a68cd`) | n03 next-session-ready (deploy the tour script, bound the logs, rewrite NEXT-TEST) | **redone in this tree (n03, uncommitted as of 2026-11-14)**: `deploy.sh` installs `moonloader/SARemix_Tour.lua` on every run, the launcher bounds `scripts/rhyremix.log` and `logs/steam-12120.log`, `scripts/diagnose.sh` + `tools/tour-report.py` read the rotated pair, and NEXT-TEST carries the s28 fade counters and the s29 clock line. Re-checked **read-only** (`git show` into a scratch tree at `build/tmp/n03/parked/`, no checkout, no branch change): that parked branch's own `scripts/log-rotate-selftest.sh` passes **32/32** against its own launcher, and its `docs/NEXT-TEST.md` (87 lines / 5990 bytes) passes `tests/test_next_test_doc.py` **as it stands at HEAD, 8/8** — against this task's extended test it fails only the two assertions added here (the grep that produces the fade counters, and saying what `on` does with a half-staged set). So the park note's "the doc test refused its rewrite" was not the doc test; what is red around this task is `tests/test_changelog_coverage.py`, which had not been told that n11 (`9b98f96`) was committed |
| `wip/n06-pier-and-sign-lights-1922` (`48d93d5`) | n06 pier heads and sign bulbs | aborted for a reboot mid-task; a WIP snapshot, never gated |
| *(done)* | n11 every lamp lit | no longer here: it ran and is in the tree — `tools/dff_2dfx.py` (the 2dFx chunk reader), `tools/lamp-coverage.py` (the coverage gate), `lamp_list.py --format v2` as the default, the `fluorescent` family, and the `[streetlights] head_*` bulb layer. The n06 branch `48d93d5` stays parked: its pier table is superseded by the game's own data |
| *(done)* | n08 fps steadiness (finding G) | no longer here: it ran and is in the tree as `1c479fa` (`tools/perf-report.py`, `[fade] destroys_per_frame`, the resync phase split, six OMM bounds in `rtx.conf.perf`) |

If one of these is retried, the parked branch is the starting point for the analysis, **not** for
the tree: each was parked on an unrelated gate failure and its config edits were never reviewed in
game.

## What to test next

1. **The n04 guarantee (finding A)** — a 360° camera spin at night must change nothing about which
   lamps are lit, and `in_drop=` must be 0 in every 10-second line. This is the one rhY ranked
   first and it has never been seen in game.
2. **The exit hang** — quit from the menu; the game must be gone within `RHYS_EXIT_GRACE` (20 s)
   and `rhyremix.log` must end with the `shutdown:` lines.
3. **Vehicle lamps (V5)** — `vehiclelights: coronas=N/M head_tex=… lamps=N vehicles=V` in the log;
   `head_tex=00000000` means the sprite table was not resolved and no headlamp will appear.
4. **Water by default (n13)** — the launcher must print `water : remix-ref` on a fresh start, and
   `bash tools/water.sh status "<game>"` must say `keys ok`.
5. **Hair cards (finding 6/F)** — if stop 14 is byte-identical to the baseline run, the
   `rtx.hairCardTextures` line was not parsed; the 45-hash fallback is in the s32 notes.
6. **Frame pacing (finding G)** — keep `logs/perf/*.csv` **and** the launcher's `logs/perf/gpu-*.csv`
   (MangoHud's own VRAM column is dead for this process), then run
   `python3 tools/perf-report.py <the two CSVs> --log $G/scripts/rhyremix.log --gpu-csv <gpu.csv> --series`.
   The baselines to beat are 76.9 spikes/min (2026-11-12 00:48) and the regression to explain is
   137.2/min (10:22) — and specifically the **after-300 s** column, 90.6 against 187.8/min. The n08
   legs are in NEXT-TEST §5, including the `rtx.opacityMicromap.enableBuilding = False` A/B that
   settles the renderer half.
7. **The n16 tonemap (T4)** — the profiles now run the **Global** tonemapper with a −0.5 EV anchor,
   and nobody has seen it. Take the 16-stop tour, then run the two `tools/target-gap.py` commands in
   [TARGET.md §6](TARGET.md): the daytime numbers have to move toward ceiling 0.5–3 % / headroom
   ≥ 2.0 / p50 100–130, and the six "good" shots must not move badly
   (`build/tmp/n16/good-before.json` is the baseline — 02 p50 17.4, 03 p50 7.0). Check the night
   shots **first**: they are what this change can break, and the ladder says what to undo if they do.
8. **The n22 exposure anchor** — same run, one extra check. `grep 'config: tonemap exposure'` shows
   `day=-0.50 EV night=+0.50 EV slew=0.050 EV/s`, and the 10 s sky line must read
   `exposure=+0.500 EV` at a 21:00 stop and `exposure=-0.500` at a noon stop (the noon side is a
   no-op by design, so the daytime shots are predicted unchanged — if they moved, the plugin is
   pushing when it should not). Night too dark: raise `[tonemap] night_ev`; night blown: `night_ev=0`;
   undo all of it: `apply=0`. Details: [TARGET.md §6](TARGET.md), FINDINGS (n22).
9. **The n17 atmosphere (T3)** — the profiles now turn the game's fog into the volumetric medium:
   10.3 km of air at noon instead of 344 km. First check it is live at all —
   `grep -i fogremap` in the `Effective RtxOption values` dump of `remix-dxvk.log` must show
   `enableFogRemap = 1` (a **Volumetric Preset** picked in the Remix UI turns it off). Then the two
   `tools/target-gap.py` commands in [TARGET.md §6](TARGET.md): the four daytime shots against
   `build/tmp/n17/gap.json` (far/near has to come down — 3.67 → ~2.6 is the prediction), and the six
   good shots plus the two water stops against `build/tmp/n17/good-before.json`. Look with your eyes
   at the skyline versus the pavement under the camera, and at the **sky**: it will get darker, and
   how much darker is the whole argument about how dense to make the air.
   `python3 tools/fog-budget.py config/rtx.conf` re-derives every number in that decision.
10. **The n18 materials (T1)** — the profiles now set `rtx.legacyMaterial.roughnessConstant = 0.65`
    (runtime default 0.7) because ~2190 of the pack's 2363 materials have no PBR maps at all and that
    one float is their whole BRDF. Check the `Effective RtxOption values` dump shows `0.65`, then run
    the two `tools/target-gap.py` commands of [TARGET.md §6](TARGET.md) against
    `build/tmp/n18/gap.json` and `build/tmp/n18/good-before.json`. Judge the **blob** columns, not the
    area column (our specular area already beats the reference for the wrong reason — lifted flat sky):
    more, smaller, more local highlights, local contrast up from 0.131–0.214, **ceiling still nowhere
    near 3 %**, and the six good shots' ceiling still 0.000 %. Paving reads wet at night → the line
    goes back to 0.7 and the work moves to the pack's material layer.

11. **DLSS 5 (n19) — nothing to install, one grep.** The answer on this box is "the driver refuses
    it", and the runtime proves it on every run without any config of ours mentioning the feature:
    `grep -iE 'DLSS-NR|Neural' "$G/rtx-remix/logs/remix-dxvk.log"` must still show
    `NVIDIA DLSS-NR support query failed: (code: 0xbad00010, …)` next to
    `[RTX info] Neural Radiance Cache: supported` (NRC works here; DLSS-NR does not — they are
    different NGX features). If that warn line ever disappears after a driver update, the probe is
    worth a real run: `bash tools/rtxprofile.sh dlss5 $G`, two stops (a noon street and a close
    character), then `bash tools/rtxprofile.sh current $G`. Protocol and the driver-vs-GPU question:
    [TARGET.md §6](TARGET.md), evidence in FINDINGS (n19).

12. **The HD PBR surfaces (n20b, MUST-HAVE #1)** — `bash tools/textures.sh on "$G"` puts 35
    generated albedo/normal/rough/height sets over the 40 surfaces that own the most screen in our
    own tour shots (the ranking, its limits and the 5 surfaces the pack covers better:
    [TEXTURES.md](TEXTURES.md)). Look at stop 13 (Pershing noon) and stop 0 (Grove noon): the stucco
    of `tanstucco1_LA`, the marble of `badmarb1_LAn` and the kerbs should carry relief and a dry matte
    response, and the crossing and stop lines must still be there — if the markings moved, the albedo
    route is wrong, not the roughness. Report: stops 00/13 mean and sat against step 1, and the frame
    time plus `logs/perf/gpu-*.csv` (the layer is +91 MB of texture memory on a card that was using
    3.5-4.4 GB). Too glossy or too matte is a `CLASSES` number in `tools/texture_rank.py`, not a
    re-bake of everything: change it, `tools/textures.sh generate`, look again. Undo: `bash
    tools/textures.sh off "$G"` (byte-exact `mod.usda`, no files left).

## How this page is kept honest

`tests/test_changelog_coverage.py` (run by `scripts/check-all.sh`) fails if a commit on any branch
since 2026-11-11 is not named in [../CHANGELOG.md](../CHANGELOG.md), if an Unreleased section names
no commit, or if this page stops naming the parked branches and the three test sessions.

Known doc debt, for the next sweep: [VEGETATION.md](VEGETATION.md), [MIRRORS.md](MIRRORS.md),
[TEXTURES.md](TEXTURES.md) and [NEXT-TEST.md](NEXT-TEST.md) still name this machine's install paths,
and NEXT-TEST sits inside the limits its own test sets (88/90 lines, ~5.9/6.0 KB — n20b fitted the
textures step in by moving detail to [TEXTURES.md](TEXTURES.md) and this page), so it can grow again
only by trimming something else until n03 restructures it.
