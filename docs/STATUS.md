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
| 4b | the bulb is a little too bright (session 2, C) | **open** | n05 parked `6eafd4d` (`bulb_radiance 1200 → 840`, new `[trafficlights] radiance_scale`) | `tools/night-brightness.py` |
| 5 | water still looks terrible (white/black speckle; black underwater) | **partly** | s32 `1769fa1` (`water.sh soft`, sheet 44° → 12°), n07 `913b7e4` (`remix-ref`), n13 `1af994f` (installed by default + its `rtx.conf` keys). The **edge flicker** is not fixable in 1.6.1: the sea is 307 exactly-butted coplanar polygons (~6000 seams) and the runtime has no hit tolerance | stop 11 mean up from 6.8, black down from 29.2 % |
| 6 | vegetation looks like scribbled lines | **partly** | s32 `1769fa1` tagged 119 card textures as `rtx.hairCardTextures` — never seen in game | stop 14 mean/sat up from 168.1 / 0.262; if identical to the baseline, the line was not parsed |
| 7 | tour shots were unusable (camera in a wall, CJ dropped into the sea, no car, no peds; `--since` re-reported yesterday) | **done** | s31 `295b2ac` (five stops re-placed from the IPL; `split_sections`/`pick_section`) | `paired \| 16 of 16 manifest entries`, no `stop N failed:` |

## Session 2 — 2026-11-12 10:22-10:45 (build `ba4c2c3` + `water.sh soft` + 119 hair-card hashes)

| | finding | status | owner | how the next run checks it |
|---|---|---|---|---|
| A | **lamps flicker in and out as the camera rotates**; must be on for the whole streamed area | **done** (unverified) | n04 `8c6a198` — the count cap may only trim the fade band (`hardRadius`), signal heads kept by player distance, and the eight `rtx.conf` sampling keys | a 360° camera spin must change nothing; `in_drop=0` in every 10 s line; `tests/always_on_check.cpp` |
| B | facing the sun at sunset the frame goes **gray**; facing away it is perfect | **measured**, not shipped | n05 parked `6eafd4d`: `rtx.tonemappingMode` defaults to **Local**, whose crushed branch (×2⁻⁴) plus a per-tile grey multiplier over a mip pyramid removes the bright end. Fix was `rtx.localtonemap.highlights = 1.5` | sky-band p95−p05 (102 → 48 today), % pixels ≥ luma 191 (4.5 % → 0.03 %) |
| C | street lamps too bright, nights not dark enough, bulb a little too bright, glow good | **measured**, not shipped | n05 parked `6eafd4d`: `radiance_scale 1.0 → 0.4`, bulb 840, night ramp of `rtx.localtonemap.shadows` 2.5 → 1.0. Measured: under a lamp the night frame is **57.3 % of noon** (target ≤ 25 %) | `tools/night-brightness.py --target 25` (the tool is on the parked branch) |
| D | pier/boardwalk lamp **heads** are dark: no lit bulb, no pool under them | **open** | n06 aborted `48d93d5`, n11 stopped with no commit | count lit heads at the pier; the 2dfx inventory n11 planned does not exist yet |
| E | highway-sign bulbs do not light; the sign's words are separate textures **casting shadows** | **open** | n06 aborted `48d93d5` (the `rtx.decalTextures` route was never tried) | photograph one sign at night |
| F | water still bad and flickers on some edges; foliage still bad | **partly** | see session 1 rows 5 and 6, plus n07 `913b7e4` (the research and the animated-water keys) | stop 10/11, stop 14 |
| G | fps a little less steady than the previous build, stabilising as he played | **measured**, not fixed | measured in the n14 sweep (FINDINGS, "Frame pacing"): **76.9 → 137.2 spikes/min** between the two runs, and the spikes do **not** correlate with our light churn (r ≈ −0.05). n08, the task that was supposed to fix it, never started | `logs/perf/NvRemixBridge_*.csv` + `logs/perf/gpu-*.csv` |
| H | research how other Remix projects do water and foliage | **done** | n07 `913b7e4` (Portal RTX / GTA IV / Minecraft / GTA 2 table in FINDINGS) | — |
| + | the game hung ~2 min (and once 16 min) after Quit | **done** (unverified) | n09 parked `08dc373` → n09b `cb0aac8`: `plugin/src/shutdown.cpp` latch + the launcher ends the wine tree | `shutdown: suspect / bridge gone` in the log; the game must die within `RHYS_EXIT_GRACE` (20 s) |

## Session 3 — 2026-11-12 23:42-23:56 video, Verona Beach / Santa Maria pier (build `ba4c2c3`)

| | what the video shows | status | owner |
|---|---|---|---|
| V1 | fake flat night ambient: sand, palms, walls and peds lit evenly green-grey, no source; the sky is black and the ground bright | **measured**, not shipped | n10 parked `30a1a43`: four mechanisms — the local tonemapper (90:1 squeeze), `rtx.skyBrightness` as an ambient ceiling 15× the moon, the fallback light welded to the camera, and a distant light pointing **up** out of the ground. Scene ratio was already 1.77 %; the renderer was throwing it away |
| V2 | boardwalk and pier lamps dark: no glowing head, no pool | **open** | n11 (no commit), n06 aborted `48d93d5` |
| V3 | exposure changes with view direction: the sand jumps brighter/darker in 3 s of turning | **measured**, not shipped | n10 parked `30a1a43` — ground band 88.6 → 146.6 → 96.2 luma with no light changing; the local tonemapper's mip-pyramid fit is view-dependent by construction |
| V4 | water and the pier's underside glow teal by themselves | **measured**, not shipped | n10 parked `30a1a43`: `rtx.enableTransmissionApproximationInIndirectRays = True` lets indirect rays pass through the sea, plus bloom's 0.25 threshold |
| V5 | tail lamps throw wide red blobs; headlights a small weak pool | **done** (unverified) | n12 `404f2cd`: the car's own dummies from the corona store → 0.09 m reflector in a 50° cone + 0.05 m red lenses |
| V6 | exhaust smoke renders as bright pink/white glowing puffs | **done** (unverified) | n12 `404f2cd`: `rtx.emissiveBlendOverrideEmissiveIntensity 0.2 → 0.06` + the three smoke sprites into `rtx.particleTextures` |
| V7 | some windows are flat overbright white blocks | **partly** | n12 `404f2cd` cut the **global** emissive (`emissive_day 0.5`, `emissive_night 0.8`); per-window control belongs in the pack's `mat_<HASH>` materials, which this repo does not touch |

---

## The quality target (n15, 2026-11-12) — "look better than the DLSS 5 showcase"

| gap | what the measurement says | status | owner |
|---|---|---|---|
| **T4 grading** | headroom p99.9/p50 1.41–1.81 vs the reference's 1.79–3.49; ceiling pixels 0.003–0.027 % vs up to 3.95 %; warmth −67…+10 vs +18.7…+48.3 | **config shipped by n16 + plugin anchor shipped by n22, after-numbers owed** — neither has ever been rendered (rule 1 forbids running the game here) | n16 shipped `rtx.tonemappingMode = 0` + `rtx.tonemap.exposureBias = -0.5` + `localtonemap.shadows 1.0` / `highlights 2.0` in all three profiles; n22 drives that same option from solar elevation at runtime (`[tonemap] day_ev -0.5 → night_ev +0.5`, ≤ 0.05 EV/s), so noon is unchanged and 21:00 gets 2× the light. The run and the abort ladder are [TARGET.md §6](TARGET.md); the mechanism is FINDINGS ("The tonemapper…", "One exposure anchor cannot serve noon and 21:00") |
| **T3 atmosphere** | haze far/near local contrast 1.22–3.67 vs 0.46–1.18; sky 92–128 luma at saturation 0.28–0.61 vs 160–199 at 0.13–0.26 | **open** | n17 (`rtx.volumetrics.enableFogRemap` and its bounds — we ship volumetrics but no fog remap at all) |
| **T1 materials** | the pack has 2334 materials and only 315 `_normal` / 280 `_rough` / **32 `_metal`** maps: ~1 in 7 gets a real BRDF | **open** | n18 (the runtime's `rtx.legacyMaterial` / `rtx.opaqueMaterial` numbers), then an offline PBR-ify pass |
| **T2 HD assets** | pack albedo is mostly 2048 px; the "8K pack" claim is the channel's, not verifiable | **measured** | census in FINDINGS; the asset work is upstream (MIT pack) |
| **DLSS 5 itself** | the keys exist in our 1.6.1 runtime (`rtx.dlssNeuralRendering` + artistic controls), but DLSS 5 is RTX 50-series only and this box is an RTX 4070 Ti SUPER on 610.57.04, already logging `NVIDIA DLSS-NR support query failed (0xbad00010)` | **open** (probe) | n19 — the answer goes in the docs either way; nothing here depends on it |

The reference, what it publishes (nothing) and the full table are in
[TARGET.md](TARGET.md); the evidence is in [FINDINGS.md](FINDINGS.md), "The reference video we are
told to beat" and "The tonemapper, and the keys our own profiles had been writing wrong". That second
section also carries a warning for T3: three keys our own profiles have been carrying are **not
options of 1.6.1** (`rtx.enableFogMaxDistanceRemap`, `rtx.antiCulling.object`,
`rtx.antiCulling.light`) and have therefore never done anything — listed with their reasons in
`config/rtx-dead-keys.txt`, deliberately not renamed.

## Parked and aborted work

| branch | task | why it is not in the tree |
|---|---|---|
| `failed/n09-exit-hang-114604` (`08dc373`) | n09 exit hang | 45 of 46 selftest checks passed; the failure was `scripts/test-session-selftest.sh`'s real-Strata guard losing a race. Redone as n09b (`cb0aac8`), which is in the tree |
| `failed/n05-night-brightness-185753` (`6eafd4d`) | n05 nights and sunset gray | `scripts/test-session-selftest.sh` **timed out** (exit 124). The lighting work itself was complete and is described in FINDINGS |
| `failed/n10-night-ambient-realism-172920` (`30a1a43`) | n10 fake night ambient | same selftest failed (exit 1). Its four causes and its numbers are recorded in FINDINGS |
| `wip/n06-pier-and-sign-lights-1922` (`48d93d5`) | n06 pier heads and sign bulbs | aborted for a reboot mid-task; a WIP snapshot, never gated |
| *(no branch)* | n11 every lamp lit | the queue recorded `rc=2: no changes` — the plan was written, no code |
| *(no branch)* | n03 next-session-ready, n08 fps steadiness | still in the queue, never started (no notes file) |

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
6. **Frame pacing (finding G)** — keep `logs/perf/*.csv`; the baseline to beat is 76.9 spikes/min
   (2026-11-12 00:48) and the regression to explain is 137.2/min (10:22).
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

## How this page is kept honest

`tests/test_changelog_coverage.py` (run by `scripts/check-all.sh`) fails if a commit on any branch
since 2026-11-11 is not named in [../CHANGELOG.md](../CHANGELOG.md), if an Unreleased section names
no commit, or if this page stops naming the parked branches and the three test sessions.

Known doc debt, for the next sweep: [VEGETATION.md](VEGETATION.md), [MIRRORS.md](MIRRORS.md) and
[NEXT-TEST.md](NEXT-TEST.md) still name this machine's install paths, and NEXT-TEST is at the exact
limit its own test allows (89/90 lines, 5997/6000 bytes), so it cannot grow until n03 restructures
it.
