# Changelog

All notable changes to **rhYgtaSA** — GTA San Andreas (1.0 US, 32-bit) under **RTX Remix 1.6.1** on
Linux/Proton, plus `rhyremix.asi`, the native plugin that drives the renderer through the Remix C API.

The format follows [Keep a Changelog 1.1.0](https://keepachangelog.com/en/1.1.0/) and the versions
follow [Semantic Versioning](https://semver.org/). Each release is grouped by feature — lighting,
sun and sky, traffic signals, water, installer and launcher, tools, testing and docs.

Two things to know before reading:

- **The version numbers are a reading aid, not git tags.** Nothing in this repository is tagged yet;
  every heading names the commit(s) that carry it, and `git log --oneline` is the authoritative
  record. The phase names in the text (`p01`…`p08`, `s01`…`s32`, `n01`…`n16`) are the work items
  recorded in `.strata/notes-*.md`; `tests/test_changelog_coverage.py` fails if a commit on any branch
  since 2026-11-11 is not named here.
- **"Fixed" usually means "measured first".** Most entries name the number or the log line that
  settled the question; [docs/FINDINGS.md](docs/FINDINGS.md) has the evidence and the dead ends,
  [docs/DEVELOPING.md](docs/DEVELOPING.md) the fix-by-fix table.

## [Unreleased] — 2026-11-12

Work in the tree, **not yet run in game**. Player-facing consequences are in
[docs/RELEASE-NOTES-next.md](docs/RELEASE-NOTES-next.md); what each of rhY's reported findings is
waiting on is [docs/STATUS.md](docs/STATUS.md).

### Lighting (s28) — the whole world changed its lighting at once (`129f2aa`)

- **Fixed (s28): every lighting change was one frame wide, whatever the cause.** Measured over the
  2026-11-12 00:48 run (`rhyremix.log` lines 3543-5350, 1470 s): the active street set went
  `1→360` and `381→1` twenty times over the run, `rtx.skyBrightness` stepped `0.02 → 1.00` nine
  times, a signal colour change was a destroy plus a create at full radiance (766 of them), and the
  sun handle was rebuilt the moment its direction moved a degree. One mechanism explains all of it:
  a Remix light's radiance is fixed at `CreateLight`, so "fade" only exists if someone sequences the
  creates — and no path did. `plugin/src/smooth.{h,cpp}` is that someone: fade envelopes
  (`rhyFade*`), a distance gain that ramps 1 → 0 across the hysteresis band, a per-frame handle-change
  budget, and rate limiters (`RhyRate`, `rhyDirLimit`) for the ambient, the exposure and the sun.
  **Undo:** every floor is an ini key in `[fade]` (`in_ms`, `out_ms`, `step`, `creates_per_frame`,
  `player_grace_ms`, `sky_rel_per_sec`, `sun_rel_per_sec`, `sun_deg_per_sec`); `check-all` asserts
  they cannot be talked out of.
- **Fixed (s28): the biggest destroy of all was a ped pointer that read 0.** `rhyPlayerPos()` was
  false whenever `*0xB7CD98 == 0`, which happened for 13 s at 01:09:20 (and at every wasted/busted
  moment), and "no player" used to mean "destroy every light". `rhyPlayerPosGrace()`
  (`plugin/src/game.cpp`) holds the last known position for `player_grace_ms` (8000) and then *fades*
  the layers out instead; `player_gone_frames=` in the 10 s line is the counter.
- **Fixed (s28): the distance cut-off, the 30-frame re-selection and the destroy-before-create.**
  Lamps at `max_distance` used to lose their light at full radiance; the band to `exit_distance` is
  now the fade band. Re-selecting everything every 30 frames made streamed-in lamps arrive at once;
  creation is nearest-first inside the frame budget. A re-create destroyed the old handle before the
  create could fail, so a cap or budget refusal left the lamp dark with a stale handle being drawn —
  `rhyCanCreateLight()` pre-checks room and a failed create clears the handle.
- **Fixed (s28): the signal phase change is a cross-fade now** (`phase_fade_ms`): the old colour
  ramps down while the new one ramps up, instead of one frame of green replacing one frame of red.
- **Changed (s28): the selection scan.** "Is this lamp already lit?" was a linear scan of the active
  list per candidate — 7002 × 320 comparisons per pass on the render thread. `active` is documented
  as sorted and searched with `std::binary_search`: **184 µs** for a 7002-lamp pass (asserted < 5 ms).
- **Tests (s28):** `tests/smooth_check.cpp`, run by `scripts/check-all.sh` as `nat-smooth` — the
  sequences the 2026-11-12 log actually recorded (a 50× ambient step, a 104° clock jump, a whole
  street appearing in one frame, a ped pointer at 0 for 13 s) with the bounds: street off → dark
  1.07 s, back on → steady 0.88 s, ambient 0.02 → 1.00 in ≥ 0.5 s with ≤ 0.014 per frame, sun ≤
  0.202 deg/frame after a 110° jump, aggregate step ≤ 5 %, creates/frame ≤ budget.

### Street lamps and signal heads (n04) — lights stopped coming and going with the camera (`8c6a198`)

- **Fixed (n04): the light-count cap was a distance rule wearing a count, and it was switching off
  lamps that were still in front of the player.** `rhySelectLights` sorted every candidate by
  distance and kept `max_lights + sticky_extra` of them, so in a dense district the farthest lamps
  lost their light while they were still 200 m away and got it back on the next pass. Measured over
  the whole 7002-lamp list: the worst point on the map has **408 lamps within 300 units** of a point
  and **552 within 380** (median 149 / 234) — against a shipped cap of 320+90 = 410. The 2026-11-12
  10:22 run shows the result: the active set jumping **+193/−193** at 10:25:04 and
  **+136/+221/−357** at 10:26:54-10:27:14 while the player walked. `rhySelectLights` now takes a
  `hardRadius` (both modules pass `max_distance`): a lamp strictly inside it is taken before the cap
  is consulted, so **the cap may only trim the fade band** between `max_distance` and
  `exit_distance`, where the lamp's gain is already ramping to 0. The shipped counts were raised to
  match the map instead of fighting it: `max_lights 320 → 560`, `sticky_extra 90 → 140`, signal
  `max_lights 60 → 90`. `hardRadius = 0` keeps the old behaviour and the new test keeps it as a
  control.

- **Fixed (n04): a traffic signal head no longer goes dark when you turn your back on it.** The game
  registers a signal corona from its renderer, so the set of registered heads *is* the camera
  frustum, and `head_ttl_ms=2000` expired heads on it: every bulb and glow on a junction faded out 2
  s after the camera turned away and was created again when it turned back. That was the only light
  in this plugin whose existence depended on camera direction, and it was running at six handle
  changes a second — `expired=790`, `retired=817`, `steps=5669` in the 970 s of the 10:22 run, for at
  most 19 heads. The records are now folded into a memory (position + last colour + last tick) and a
  head is kept by **player distance**: `rhyHeadKeepAlive()` keeps a stale head with the colour it last
  showed while it is within `traffic_exit_distance`, and drops it only beyond it (or beyond
  `traffic_max_distance`, where a stale head is a freed entity rather than a signal we looked away
  from). `head_ttl_ms` is a staleness marker, not a destroy trigger.

- **Added (n04): two field counters for the guarantee, and a load-time measurement of the lamp file.**
  Both layers print `in_drop=N` in the 10-second line: the cumulative number of lamps that had a
  light, were inside `max_distance`, and were left without one by the next selection. It should be 0
  for a whole run. At load the street module scans the real list and prints
  `streetlights: worst case 408 lamps within max_distance=300 of a point (cap
  max_lights+sticky_extra=700): the cap covers the radius`, so the ini comment's number is checkable
  in `rhyremix.log` instead of asserted offline.

- **Changed (n04): the eight `rtx.conf` keys that decide whether a pixel is allowed to draw a lamp.
  For our lights, culling was never the problem** — the 1.6.1 source shows an API light is never
  frustum-culled (`LightManager` marks inside/outside frustum only for its own game-detected lights,
  `rtx_light_manager.cpp:151-183`, and erases only those, `:110-141`; `CreateLight` lights live in
  `m_externalLights` and die only on `DestroyLight`, `:795-810`). What is view-dependent is the
  sampling: with `rtx.di.initialSampleCount = 4` and the 401 live lights that run measured, each
  pixel draws 4 random candidates out of 401 for direct light, held by reuse with
  `maxHistoryLength = 4` frames — and our shipped file set
  `rtx.di.permutationSamplingNthFrame = 1`, re-randomising that lottery every frame. `config/rtx.conf`
  now sets permutation **0**, `di.initialSampleCount 8`, `di.maxHistoryLength 12`,
  `risLightSampleCount 16`, `volumetrics.temporalReuseMaxSampleCount 8`,
  `volumetrics.restirGridGuardBandFactor 1.5`, `antiCulling.object True`, `antiCulling.light True`
  (+ `numFramesToExtendLightLifetime 1000` pinned); `config/rtx.conf.perf` carries the cheaper
  variant, and `config/rtx.conf.nopops` was regenerated from the new base. **`max_lights` and
  `rtx.di.initialSampleCount` move together** — a bigger pool with 4 samples dilutes every lamp — and
  both files say so.

- **Fixed (n04): two rtx.conf key notes in the s28 table were wrong.** `rtx.enableCulling` is
  front/back-face culling of opaque objects, not frustum culling (we set it False for a different
  reason, and it stays False); `rtx.uniqueObjectDistance` defaults to **300**, not 1000, and is "how
  far an object may move in one frame and still be the same object", not a view distance. Corrected in
  `docs/FINDINGS.md`.

- **Added (n04): `tests/always_on_check.cpp`, run by `scripts/check-all.sh` as `nat-alwayson`** (35
  checks). A 360° camera sweep over 624 lamps surrounding the player: **0 handle changes in the 10 s
  after settling**, and the wanted set identical from pass to pass — with a control in the same test
  that prints what a 60° frustum rule would have re-decided (207 of those lamps), so the test is known
  to be able to see churn. A 300 s walk at 1.4 m/s and a 240 s drive at 15 m/s through a field built
  to the measured worst point: no lamp inside `max_distance` is ever destroyed, none is created there
  after the initial fill, and a lamp that has settled at full gain is never touched again. Plus the
  cap rule (500 near + 200 band lamps, cap 100 → n04 keeps 500, the old rule kept 100), the settled
  handle (60 s standing: 175 creates, 0 changes in the last 55 s) and the head rule (stale+near kept,
  stale+far dropped, cap 20 over 200 heads inside the radius keeps all 200).

### Vehicle lamps, particles and window glow (n12) — a car's lamp was a 30 m billboard (`404f2cd`)

- **Added (n12): the car's own lamps, read from the game's corona store.** Nothing in this plugin lit
  a vehicle: the front lamps were the game's `coronaheadlightline` sprite (which `rtx.ignoreTextures`
  removes from the raytraced scene, so the road in front of the car had no light at all) and the rear
  lamps were `coronamoon` in no category at all — a 4 cm bulb reaching the renderer as a 30 m red
  ball. `plugin/src/vehiclecorona.{h,cpp}` reads the store the game itself fills (base `0xC3E058`,
  stride `0x3C`, count `0xC3E038`, max 64: position, key, sprite, strength, range, radius, rgba,
  linked entity) page-validated and read-only — nothing is patched; `plugin/src/vehiclelights.{h,cpp}`
  is the pure-C++ decision core: classify head (the sprite pointer the game resolved for slot 4) vs
  tail (red-dominant), group by key proximity (the key is `entity + head index`, and the index reaches
  7, so `key & ~3` splits one car in two), aim the beam along the car's own front-to-rear axis, and
  create a **0.09 m reflector in a 50° cone (flux 950, ≈5000 cd)** plus **0.05 m red lenses (tail 20,
  brake 100)**. **Undo:** `[vehiclelights]` in the ini (`enabled`, `max_distance`, `max_lights`,
  `cadence_ms`, `move_step`, `brake_red_min`, the fluxes) — `enabled=0` removes the layer.
- **Measured (n12): the numbers behind V5/V6/V7** (`tools/frame_light_measure.py`, on a high-pass
  image, because the whole frame carried an ambient floor): red ground spill 6.9k-17.5k px at an
  equivalent radius of 47-75 px = **1.5-2.5 m per bulb**; the headlight pool 35k-119k px with no
  compact core; exhaust puffs mean RGB **[172, 57, 80]**, `min(R,B)-G = 21.4`, 31-96 luma above the
  road under a street lamp; clipped-white windows 0.30-0.40 % of the ground band, biggest block
  ~963 px, **chroma 1.7/255** — neutral white, no texture, no lamp colour, i.e. an emissive term and
  not a lit room. A tool bug was found on the way: `box_blur` divided the border window by the full
  window width, so a flat field darkened at the edges and the high-pass reported a bright band along
  every frame edge, bigger than the effects being measured.
- **Changed (n12, V6): exhaust stopped being a light source.** `rtx_instance_manager.cpp:1132-1138`
  forces the emissive intensity of any emissive-blended sprite to
  `rtx.emissiveBlendOverrideEmissiveIntensity` (default **0.2**) with the sprite's own colour as the
  emissive — which is exactly the pink of the report. Shipped: **0.06** in `config/rtx.conf`,
  `.perf`, `.experimental` and (generated) `.nopops`, plus the three smoke sprites that were in no
  category at all added to `rtx.particleTextures` (`smoke5 0xC70DC799C5BFFE65`,
  `smoke4 0x3F66C5D5F91C3708`, `bullethitsmoke1 0x2D558BC31F67FD1D`). **Undo:** the value back to
  `0.2`, or delete the three hashes; `enableEmissiveBlendEmissiveOverride = False` was rejected
  because it also turns off the glow of every corona, beam and UI sprite in the game.
- **Changed (n12, V7): the emissive cap follows the sun.** `rtx.emissiveIntensity` is the only
  emissive scale in 1.6.1 (`rtx_scene_manager.cpp:1771/1870/1913`) and there is no per-texture
  radiometry category, so the cap is the global dial the sky module already pushes: `emissive_day`
  **1.0 → 0.5**, `emissive_night` **1.0 → 0.8** (day cut harder, because that is where the clipping
  is measured). **Undo:** the two `[sky]` keys back to 1.0.
- **Tests (n12):** `tests/vehicle_lights_check.cpp` (42 checks: fixtures, the cone's geometric
  concentration `2/(1-cos θ)`, classification, pairing, aim, radiometry, brake, distance gate, cap,
  hash stability), `tests/test_vehicle_profile_keys.py` (9: every new key exists in the runtime's own
  option vocabulary, the category hashes are 16-hex tokens, no duplicates, the ini block equals the
  compiled defaults), `tests/test_frame_light_measure.py` (10: synthetic ground truth per metric).

### Water and vegetation (n07) — the water setup the other RTX Remix projects actually use (`913b7e4`)

- **Added (n07): `tools/water.sh remix-ref`, a water variant built from other projects' shipped
  numbers rather than from guessing.** `AperturePBR_Translucent` with transmittance (0.62, 0.8, 0.8)
  at 4 m (xoxor4d's GTA IV RTX default water material), IOR 1.33, thin-walled off, a 12 fps flipbook,
  and a regenerated wave sheet at a 6 deg median facet tilt (the pack's sheet measures 44 deg, with
  32 % of texels over 60 deg and a 21.3 deg/texel gradient; NVIDIA's own Minecraft mod ships an ocean
  at `slope_rms 0.0325` ~ 2.2 deg). `tools/water.sh pack` still restores the pack's file byte for byte.
- **Fixed (n07): the runtime's own water path never ran on the game's second water surface.**
  `translucent_surface_material_interaction.slangh:62` gates it on `animatedWaterEnable &&
  surface.isAnimatedWater`, and lines 73-83 show what that buys - the sprite-sheet cell's UV scrolled
  by `animatedWaterPrimaryNormalMotion` plus a second normal sample at
  `animatedWaterSecondaryNormalLodBias` ("blurred, resulting in lower frequency waves"). The sea
  texture was in `rtx.animatedWaterTextures`; `waterclear256` (`0xA75A0B41F2454F04`) was not, so both
  profiles now carry it (62 hashes, sorted, identical in `rtx.conf` and `rtx.conf.perf`).
- **Added (n07): the renderer keys the water research points at** - `combineSpecularAlbedo = True`
  (NVIDIA's Minecraft mod: "without it, shaded water reflects blotchy sky-coloured smears instead of
  the scene"), `invalidateHistoryForAnimatedWater = False` (the sheet no longer re-randomises per
  frame, so keep the history), `animatedWaterSecondaryNormalLodBias = 2.0`,
  `volumetrics.enableTranslucentShadows = True`, and `enableDirectTranslucentShadows = True` in the
  look profile only; `rtx.translucentMaterial.normalIntensity` moved 0.35 -> 1.0 because the map now
  carries the softening. The GTA IV project's shoreline fade (`rtx.water.shore*`) is documented as a
  **fork-runtime** feature: measured unknown in our 1.6.1 dll, so it is not written anywhere.
- **Measured (n07): why the water edges flicker.** `$G/data/water.dat` is text - 307 polygons (301
  quads + 6 triangles) that butt exactly edge-to-edge in **5969** pairs and overlap in only **2**
  (`water1.dat`: 267 / 4586 / 1). So SA's sea is one coplanar sheet cut into exactly-butted polygons,
  not GTA IV's stack of coincident quads; a ray on a shared edge can hit neither polygon, and a
  perfect-mirror translucent lobe flips that pixel between sea and seabed. Stock 1.6.1 has no hit-
  tolerance or depth-fade key, so what shipped reduces the contrast of the flicker; the seam itself
  needs rebuilt geometry or a fork runtime, and the docs say so.
- **Added (n07): opacity-micromap guard rails pinned in both profiles**
  (`cache.maxVidmemSizePercentage 0.15`,
  `building.conservativeEstimation.maxTexelTapsPerMicroTriangle 64`,
  `buildRequests.enableAnimatedInstances = False`, `building.splitBillboardGeometry = True`) - the
  values both xoxor4d profiles ship, which the 1.6.1 source shows are also the defaults; the caps our
  own "Out of memory budget" device loss and "more texel taps than allowed" lines refer to are now
  written down where they cannot drift. Two-sided foliage needed no new key: alpha-tested geometry is
  exempt from triangle culling by definition (`rtx_options.h:933`), and the hair-card category already
  disables backface culling.
- **Tests (n07): `tests/test_water_remix_ref.py`** - the variant validates against the pack's own file
  and carries the researched numbers in both prims; `water.sh` generates its own sheet at the
  researched tilt, names the variant in `status`, restores byte-for-byte and writes nothing on
  `--dry-run`; every rtx.conf key `water.sh` prints passes the runtime key audit; both profiles carry
  the water and OMM keys and both water hashes; and no fork-only key is shipped (negative control).

### Water and vegetation (s32) — the pack's wave sheet, and 119 foliage cards that were in no category (`1769fa1`)

- **Added (s32): `tools/water.sh soft`, a regenerated wave sheet instead of the pack's.** The pack's
  `Water_materials.usda` normal sheet measures a **44° median facet tilt** with a 21.3 deg/texel
  gradient, decorrelated after 10 px of a 512 px frame — which is the 2 px speckle period on screen
  (rhY's "white/black speckled sheet"). The mdl has no normal-strength, roughness or UV input to turn
  down, so `tools/water_normal_soft.py` regenerates the sheet: **12° median tilt, 2.2 deg/texel,
  ~7 facets per frame**, BC5, mip chain and frame layout kept. `realistic` / `calm` / `pack` stay as
  the A/B, and `pack` restores the original bytes.
- **Added (s32): the foliage cards are tagged as cutouts.** None of the grass/bush card textures was
  in any `rtx.conf` category list, so Remix blended them into the "scribbled lines" of the report.
  `tools/vegetation_hashes.py` walks the procobj/vegepart dictionaries the game's vegetation actually
  references (an allow-list on purpose: `veg.*` also matches the Las Vegas building dictionaries and
  `.*grass.*` matches `tikigrass`, which is pool and concrete-wall textures) and tags only the ones
  with real cutout content (≥ 5 % fully transparent or ≥ 5 % mid-alpha texels): **119 of 190**.
  `config/rtx.conf` and `config/rtx.conf.perf` now carry them as `rtx.hairCardTextures`, plus the
  `rtx.opacityMicromap.*` keys the live file had and the shipped profile did not (so
  `rtxprofile.sh current` used to drop them). **Undo:** delete that one line; the risk of a long line
  (2402 chars) is "nothing changed", not corruption, and the tool prints a 45-hash fallback.
- **Tests (s32):** `tests/test_water_soft.py` (octahedral round-trip — which caught a real bug: the
  map needs the `s = 1/(|x|+|y|+z)` scale, without it a 30° normal comes back as 45° — strength and
  decimate monotonicity, a synthetic BC5 sheet through an independent BC4 encoder and back, the
  `water.sh` round-trip on a fake tree) and `tests/test_vegetation_hashes.py` (the allow-list, an
  in-memory `.txd` where only the DXT3 card is tagged, and the two profiles' lists being equal).

### Water (n13) — the research variant is now the default everywhere (`1af994f`)

- **Fixed (n13): nothing installed the water we had built.** n07 shipped `remix-ref`, s32 shipped
  `soft`, and the live install was still on `soft` with an `rtx.conf` that carried **none** of the
  animated-water keys: measured before this change, `G/rtx.conf` (72 lines, hand-tuned) had only
  `rtx.animatedWaterTextures` (61 hashes, `0xA75A0B41F2454F04` missing) and
  `rtx.translucentMaterial.normalIntensity = 1.0`. `apply.sh` never called `tools/water.sh`,
  `launcher/launch-rtx.sh` called it only when `RHYS_WATER` was set, and `water.sh` **printed** the
  rtx.conf lines that go with a variant instead of writing them. So the runtime's own water path —
  gated on `animatedWaterEnable && surface.isAnimatedWater` — had never run on rhY's install, on any
  variant. rhY 2026-11-12: "make sure we get the new water by default".
- **Added (n13): `tools/water.sh default`, and a state file that records who chose what.**
  `apply.sh` and the launcher now call `default`, which installs `$RHYS_WATER_DEFAULT`
  (default: `remix-ref`), is a no-op when it is already live, and **never overwrites a variant a
  person picked**: the choice lives in `<game>/.rhys-water/state`
  (`variant=… mode=default|explicit requested_by=… material_sha256=… rtx_conf_sha256=… when=…`).
  `water.sh soft`, `RHYS_WATER=soft`, `apply.sh --water soft` and `water.sh pack` all record
  `mode=explicit`, and every later default run says so and leaves it alone;
  `water.sh default <game> --force` overrides it once. Variants are now identified by **sha256**,
  not by grepping texture names — the old heuristic reported the pack's own material as "unknown"
  as soon as the `.orig` snapshot was missing.
- **Added (n13): `tools/rtx_water_keys.py` — the rtx.conf half of a variant is installed, not suggested.**
  Absent keys are appended under one marked block; a present key has its **value rewritten in place**
  (indentation, a trailing `# comment` and the line's own CRLF/LF kept — the shipped profiles are
  CRLF, the live file is mixed); required hashes are appended to the **end of the existing**
  `rtx.animatedWaterTextures` line, never as a second list line and never removing one. It writes
  nothing when nothing differs, and keeps `<game>/.rhys-water/rtx.conf.pristine` (once) and
  `.prev` (every change). `--check` reports the state without writing; `water.sh status` now prints
  which keys are missing and what the default variant would add.
- **Added (n13): `apply.sh --water remix-ref|soft|realistic|calm|pack` and `--no-water`** (default:
  `remix-ref`, run after the pack is patched in; an unknown variant is refused before anything is
  written). `RHYS_RTX_KNOBS=1` in the launcher stopped being a 0.35 machine: it now writes the value
  the **live variant** needs (1.0 for a regenerated sheet, 0.35 only for the pack's own 44 deg one).
- **Fixed (n13): the launcher's water report had been silently empty since n07** — it grepped
  `installed: ` while `water.sh status` had renamed the label to `installed variant: `. It now reads
  the exact-bytes line. Added the `RHYREMIX_INSTALLER_SANDBOX` guard to `launch-rtx.sh`: the script
  falls back to `/media/rhy/data/Gamez/GTASA-RTX` when its own folder has no `gta-sa.exe`, so without
  that guard a broken dry-run test could have installed water into a real game folder.
- **Applied (n13): the live install now has it.** `G/rtx.conf` was backed up as
  `rtx.conf.bak-before-n13` first; one `water.sh default $G` run installed `remix-ref`
  (`sha256 2a6b13e8…`), generated `water_normal_ref.dds` (44 → 6 deg median facet tilt, 21.3 →
  1.0 deg/texel, 0.7 s), added the five missing keys and the missing hash, and left the rest of the
  hand-tuned file alone: `diff rtx.conf.bak-before-n13 rtx.conf` is one extended list line plus one
  12-line block, and the runtime key audit still reports the same **2** pre-existing legacy keys
  (`rtx.enableVolumetricLighting`, `rtx.minReplacementTextureMipMapLevel`) before and after.
  A second run changed nothing. `config/rtx.conf`, `.perf` and `.nopops` were checked to already carry
  all six water keys and both hashes, so a profile switch keeps the water setup. One thing this task
  deliberately did **not** do: `<game>/launch-rtx.sh` in the live install is the copy an older
  `apply.sh` put there (before n09b and n13), so a desktop click there does not yet re-ensure the
  default water. One `./apply.sh "$G"` updates it (it is a no-op for everything else); step 5 only
  authorised the water path.
- **Undo:** `bash tools/water.sh pack "$G"` restores the pack's material byte for byte
  (`28560263…`) and records the rollback as an explicit choice, so the default path will not undo it
  again; the config half is `bash tools/rtxprofile.sh restore "$G"`, `cp "$G/rtx.conf.bak-before-n13"
  "$G/rtx.conf"`, or the copies in `<game>/.rhys-water/`. Nothing else about the install changed.
- **Tests (n13):** `scripts/water-default-selftest.sh` (new, in `scripts/check-all.sh`) builds a fresh
  fake install — the pack's own material, and an `rtx.conf` generated from `config/rtx.conf` with all
  six water keys deleted and both hashes pulled out of the list — and proves that one
  `RHYS_DRY_RUN=1` launcher run produces `remix-ref` + the keys + the backups, that the next run
  changes no byte (material, rtx.conf, state, sheet all re-hashed), that `RHYS_WATER=soft` is
  recorded as explicit and survives every later default run, that `--force` and `water.sh pack`
  behave, and that the sandbox guard refuses a marker-less folder. 47 checks, 0 failed.
  `scripts/installer-selftest.sh` grew a real `Water/` directory in its fake tree (pack material +
  the pack's 7 MB BC5 sheet, copied read-only) and went from **44 to 78 checks**: remix-ref installed
  by `apply.sh`, the `.orig` snapshot taken automatically and equal to `config/water/pack.usda`, the
  sheet generated, the state file naming `apply.sh`, exactly one `animatedWaterTextures` line and one
  `normalIntensity` assignment, a pristine rtx.conf backup, and `--water pack` / `--no-water` /
  an unknown `--water` value all honoured.

### Traffic signals (s30, n02, n02b) (`bd7fdf4`, `ee59b87`, `ba4c2c3`; the s19 hook work is `e3d57a3`)

- **Added (n02b): a lit signal is two lights again — the bulb at the lens AND the glow around it.**
  rhY, 2026-11-12 08:50: *"We wanted the glow from the traffic light and the bulb. It should be both
  like real life."* The 2026-11-12 00:48 run had the ambient and no bulb; s30 fixed the bulb and
  removed the ambient twice over — our light's flux went **304 → 67** (radiance 5400 → 1200 nits at
  the same 0.10 m sphere) and the game's own signal corona went from **15.0 to 0.45 game units**
  (area ÷1111) — and n02 moved that one light 2.10 m onto the lens without ever adding a second one.
  Now `rhySignalGlow()` adds a soft sphere at the same lens: `glow_radius=3` m carrying
  `glow_flux=304`, the flux the 2026-11-12 00:48 glow measured, so the spill onto the pole, the wall,
  the road and the peds is the strength rhY saw and wants at 1/900th the surface brightness. It is
  solved for flux, not radiance, so the spill keeps its strength through the red/amber/green cycle
  and only changes colour; it takes the same lens colour (`rhySignalLensRgb`), the same position and
  the same fade envelope as the bulb, and `config.cpp` caps it at **3473** — one mid street
  luminaire's flux, which `rhyLuminaireFlux()` reproduces from `radiance_big=2.0`, `radius_ref=15`
  and the shipped 2100 K / 5000 K lamp colours (3462 measured, s30's 3473). 304 is 8.8 % of that, so
  the reference strength needed no take-down. Both handles are created, drawn and destroyed together
  in both light paths, the glow's hash carries a fixed bias so it cannot collide with its own bulb's,
  and the 10 s summary now logs `N heads lit (N with a glow), N bulbs + N glows drawn`.
  `glow_flux=0` is the bulb-only A/B. Tests: `tests/signals_check.cpp` 150 → 166,
  `tests/test_signal_glow.py` new (17 checks).

- **Fixed (n02b): the claim in the 2026-11-12 report that finding 4 was "no green street flood".**
  It was worded wrong: the ambient glow rhY saw is WANTED, the bulbs are what is missing. Corrected
  in `.strata/test-2026-11-12.md` and in the signal step of `docs/NEXT-TEST.md`, which now asks for
  a bulb AND a glow at every lit head.

- **Fixed (n02): the claim that "the patched call site was never reached", and the source decision
  that was built on it.** The two runs that installed the hook (2026-11-11 12:04, 13:03) logged
  `records=0`, and `plugin/src/signalheads.h` turned that into "never reached" — which then flipped
  any session that starts away from a signal onto the list for its whole run. The numbers say
  otherwise: those log lines carry no `calls=` counter (s19 added it later), the 13:04 run's Wine
  trace shows the stub entered with a real `CEntity*` (0x178DEFB4) and head index 1, and the game's
  own **50.0 m** corona range — `push 0x42480000` at `0x49DCF3`, tested inside the callee at
  `0x6FC22E` (`fld [esp+0x4c]; fmul [esp+0x4c]; fcompp; jnp 0x6FC4BB`), fading from 35.0 m
  (`0x85F070`) to 50.0 m (`0x858B40`) at 1/15 per metre (`0x863E0C`) — is applied *after* our patch
  point, so it never gated the recorder. What it does gate is what the game draws, and the player was
  **79.1 m** from the nearest of the 1522 list entries: the game was drawing no signal-head corona
  there either, so `records=0` proved nothing.
- **Changed (n02): `rhyHeadsHealthy()` is range-aware.** It takes the 2D distance to the nearest
  entry of the loaded list (`rhyNearestSignal2D`) and the game's range, and answers one of four
  states — recorded, grace still open, nothing inside the range (keep the hook: the sample proves
  nothing), or a signal was inside the range and still nothing appeared (fall back to the list). The
  decision, the distance and the counters are logged once: `trafficlights: head source = … (nearest
  signal 79.1 m, the game's own corona range 50 m, …)`. `source=list` stays the kill switch; the
  grace is the new ini key `head_grace_ms=20000` (0 = decide immediately).
- **Added (n02): list-mode bulbs move from the mast onto the lens.** The lamp table that supplies the
  list's `headz` column always carried a *three* number head offset, and the file kept only the
  height: `cj_traffic_light4`'s head is at `(-2.10, +0.10, +7.60)` in the model's own frame, so the
  light sat **2.10 m sideways** from the lens. `tools/traffic_list.py` now writes
  `# rhyremix traffic v2` — `x y z headz heading lensx lensy lensz` — where `heading` is the IPL
  instance yaw (all 1524 raw traffic instances carry a pure Z-axis quaternion, so a yaw is exact) and
  `lens*` is the model-local offset; the plugin rotates it by the heading and adds it
  (`rhySignalLensPos`), the same rule the game applies to a head offset at `0x59C890`. **960 of the
  1522** signals get an offset (the three table models plus `cj_traffic_light3` and `trafficlight1`,
  borrowed across radius gaps of 0 and 4); `mtraffic1`/`mtraffic2` (562) keep the pole x/y because a
  25-unit radius gap does not justify borrowing a mast arm. New key `lens_offset=1` (0 = the old
  position).
- **Kept (n02): the old file format reads byte for byte.** A four-column line parses to `heading 0,
  lens (0,0,headz)` and lands exactly where it landed before; regenerating with `--v1` reproduces the
  deployed `scripts/rhyremix_traffic.txt` byte for byte (`tests/test_traffic_list.py`).
- **Measured (n02): the game's 2DFX head records are not in the game files.** The head loop in
  `DisplayActualLight` is bounded by the model's 2DFX entry count (`0x49DBDD
  movzx eax,BYTE PTR [edi+0xd]` … `0x49DC01 jle 0x49DE2C`, past the corona call), and a walk of every
  entry of `models/gta3.img` (12964 `.dff`, 164 `.ipl`) finds no chunk with id `0x00010105` anywhere,
  as does a search of the whole exe for the ASCII `2dFx` magic (0 hits). So the offsets cannot be read
  from the game, and the list file has to carry them — which is what the two entries above do. The
  28 call sites of the corona thunk, and the one that sits in `DisplayActualLight`, are enumerated in
  `build/tmp/n02/corona_sites.py` and pinned in `tests/test_signal_hook.py`.
- **Tests (n02):** `tests/signals_check.cpp` 124 → 150 checks (the four health states against the
  79.1 m spawn and a 14.4 m tour stop, the nearest-signal distance, v1/v2 line parsing, the lens
  position at 0/90/180/270 degrees and the 2.10 m it recovers); `tests/test_traffic_list.py` 15 → 35
  (heading from a quaternion, rigid yaw rotation, the lens-offset borrowing rule and its radius gap,
  the generated v2 file against the deployed v1 one); `tests/test_signal_hook.py` 28 → 33 (the
  `push 50.0` at `0x49DCF3`, the 2DFX-count branch that skips the call, the callee's distance test at
  `0x6FC22E`, and the 35.0/50.0/1-15 floats read out of `.rdata`).
- **Known (n02): one stale gate assertion.** `scripts/check.sh` line 45 asserts the v1 column count
  (`awk '!/^#/ && NF!=4'`) on the generated traffic list and now reports `FAIL 1522 malformed traffic
  lines` for the eight-column file. Header rule 4 forbids editing `scripts/check.sh`, so it is
  recorded instead: the assertion needs `NF!=4 && NF!=8`. `scripts/check-all.sh` — the gate this task
  names — passes.
- **Fixed (s30): the corona hook had not been installable since s19.** `rhySignalHookInstall()`
  asked the source question by calling `rhySignalPatchCheck(source, NULL, NULL, NULL, NULL)`, and
  that function answered a null-byte-window call with `kRhyPatchCallSite` — "the bytes are wrong".
  The install path read that as a verdict, never read the exe, and fell back to `source=list` on
  every build of the game. The 2026-11-12 00:48 run logs exactly that: *"the bytes at 0x49DD4C are
  not 'call 0x6FC580'"* about an exe whose bytes at `0x49DD4C` are `E8 2F E8 25 00` = `call
  0x6FC580` (verified against `gta-sa.exe`, md5 `85662cbb…`). The null-window answer is now its own
  value, `kRhyPatchNoBytes` = "go and read the bytes", and the install walk is asserted step by
  step in `tests/signals_check.cpp`. Until this fix, no run had ever lit a head: every signal light
  since s01 sat on the pole.
- **Fixed (s30): the green flood, which was two separate things.** Measured on the tour's signal
  stops (1920×1080, `build/tmp/s30/`): 10–39% of the frame saturated green, spread over every
  block, and **not one green pixel above luma 200** in any of the four shots — a huge area at
  moderate radiance, i.e. ambient, never a bulb. The 2026-11-11 tour, where the plugin created
  **zero** traffic lights, still shows 10–30% green, so most of it is the game's own doing: each
  lit head is registered with a corona of **radius 15.0** — a 30 m additive pure-green flare, drawn
  by the Remix bridge as emissive geometry. Our own lights were the second half: `fixture_size=1`
  scaled `radiance=1.5` by `(radius_ref 6.0 / 0.10)²` = ×3600, so each 0.1 m sphere published
  **5400 nits** (log 00:48:56: `radiance=9.64,5400.00,1845.57 radius=0.10`) — the whole output of
  the old six-metre blob squeezed into a marble, sixty times over.
- **Added (s30): `corona_radius=0.45`** resizes the game's own flare by rewriting one immediate —
  `push 0x41700000` at `0x49DCD9`, the 15.0-unit radius argument of the same `RegisterCorona` call
  we already hook — so the visible green becomes a lens-sized bulb instead of a street-wide wash
  (area ÷ 1111). Same kill-switch discipline as the hook: gated on `source=game`, the five bytes
  are checked against `68 00 00 70 41` before anything is written, `corona_radius=0` puts the game's
  flare back, and `tests/test_signal_hook.py` asserts those bytes against the real exe.
  `rhySignalCoronaApply()` is idempotent and re-runs on every `rhyremix.ini` edit, so the A/B needs
  no restart.
- **Added (s30): the bulb model** — `rhySignalBulb()` in `plugin/src/signalheads.cpp`, used by both
  the head path and the list path so they cannot drift. With `fixture_size=1` the radius is the
  real lens (0.10 m) and `[trafficlights] bulb_radiance=1200` **is** the lens luminance in nits at
  that radius, with no `(radius_ref/radius)²`; the flux-preserving rule stays for `fixture_size=0`,
  where the user is driving a blob on purpose. One signal now puts out under 5% of one street
  luminaire's flux (asserted against `rhyStreetFixture()` in the native test; the old 5400-nit bulb
  failed that bound by 2×). New keys: `bulb_radius` (0 = the lens from `lamps.h`), `bulb_radiance`,
  `corona_radius`.
- **Added (s30): a safety net for the corona resize.** Shrinking the game's flare while our hook
  lights nothing would leave the heads dark — worse than the flood. `rhyHeadsHealthy()` falls back
  to the list lights when the hook has been live for 20 s without one record (the 2026-11-11 13:03
  run installed the patch and read `records=0`), and the log line says so and points at
  `corona_radius=0`.
- **Tests (s30):** `tests/signals_check.cpp` 89 → 124 checks (the install walk step by step, the
  corona push recognised/refused, the bulb's radius, radiance, flux bound and off switches, head
  position on the lens); `tests/test_signal_hook.py` 25 → 28 (the `push 15.0` at `0x49DCD9` in the
  real exe, the no-bytes-vs-bad-bytes regression, the corona patch's own gates).
- **Added:** a kill switch inside `rhySignalHookInstall` (`plugin/src/signalhook.cpp`, decision logic
  in `plugin/src/signalheads.cpp`): the code patch is installed only when `[trafficlights]
  source=game` **and** the call site `0x49DD4C` still decodes to `call 0x6FC580` **and** `0x6FC580`
  is still the corona-type thunk **and** the caller's `add esp,0x54` sits at `0x49DD54`. Every
  refusal logs which check failed and quotes the bytes it actually found, then falls back to
  `source=list` — a different game build can no longer be patched by accident.
- **Fixed:** a bad pointer handed to the corona recorder is now a dropped record instead of a crash
  (`bad` counter), and re-entrancy is counted (`reentrant`, must stay 0) instead of silently
  re-entering the recorder. The per-frame page cache is cleared each frame, so a cached page check
  can no longer outlive the frame it was made in.
- **Added:** the hook's own counters in the per-10 s log line —
  `hook: calls=… records=… dropped=… bad=… reentrant=…` — which is the whole diagnosis of the
  hook in one line (explained in [docs/TESTING.md](docs/TESTING.md)).
- **Changed:** the recorder is now a thin shim over `rhyCollectHead()`, so the head-collection logic
  is testable without a game; the detour stub itself is byte-identical to the third build (verified
  by disassembly — the review found it correct, what changed is everything around it).
- **Tests:** `tests/signals_check.cpp` 89 native checks (kill switch, every recorder path against
  fake memory, page cache) and `tests/test_signal_hook.py` 25 checks (stub encoding at every stack
  alignment, the real exe's bytes, no-throw discipline).

### Sun and sky (s29) — time of day stopped flickering (`9e67d91`)

- **Fixed:** the sun no longer swings tens of degrees and back. The two bytes next to the game clock
  (`month@0xB70158`, `day@0xB70154`) are not a calendar — the month byte is a counter walking +16 —
  and `gameDayOfYear()` accepted them whenever they looked like one: 20 of 409 logged sky samples
  used such a date, and 31 consecutive-sample pairs moved the sun by up to 43.4 degrees (median
  23.7) and straight back. `rhyCalendarUpdate()` now requires `[clock] calendar_stable_reads` (30,
  i.e. 30 s of a still date) identical reads and a day that does not step, so the season stays
  `[sun] day_of_year` for the session.
- **Added:** `plugin/src/tod.cpp`, the time axis. The clock bytes only offer a new minute every
  ~2000 ms (measured p50 2015) and the sun moves 0.25 deg per game minute, so the target was a
  staircase; `rhyTodUpdate()` publishes a continuous game minute (`anchor + elapsed/msPerGameMinute`,
  self-calibrated), repairs the torn hour/minute read that made the clock walk backwards 59 minutes
  at every hour roll, and refuses backwards moves that do not persist — the tour's
  `setTimeOfDay()` pins made 51 of ~400 logged sample pairs go backwards. The only allowed decrease
  is the midnight wrap.
- **Changed:** the sun and moon are computed **every frame** at that continuous minute
  (`solarPositionF()` / `lunarPositionF()` take a float minute of day) instead of once a second at a
  whole minute; the observer (latitude, season, DST) stays a 1 Hz keyframe. `sun_force_hour` no
  longer reads back the bytes it wrote, which had put the plugin and the game's own clock update in a
  write fight.
- **Added:** `[sky] cloud` (0 = clear, so the shipped look is unchanged) and a weather follower that
  rate-limits turbidity and cloud cover (`[sky] turbidity_per_sec`, `cloud_per_sec`): an ini edit
  ramps the sun's colour instead of stepping it between two frames, and an overcast dims the direct
  beam (×0.10) while giving part of it back to the ambient (×1.60).
- **Added:** the clock's own counters in the 10 s log line — `tod=… ms/min=… ticks=… torn=…
  jitter=… reanchor=… wraps=… bad=…` and `cal: stable=… accepted=… rejected=…` — explained in
  [docs/TESTING.md](docs/TESTING.md).
- **Tests:** `tests/tod_check.cpp`, 58 native checks, including a **whole simulated 24 h day at
  60 fps** run twice — once with the old sampling, once with the new, with the tour's clock pins and
  a weather front in both. The sun used to move up to **3566 deg per game minute** between frames
  with 2881 season changes and 186 lamp flips in a day; now **0.21** (the real sky is 0.25), with 0,
  0 and 2.

### Lighting (n01) — the steps that survived s28, and the renderer's own time knobs (`ad323a9`)

- **Fixed (s29 note T7): the sun and moon handles are replaced on a clock now, not on a
  threshold.** The rule was `movedDeg > [sun] step_deg || 5% luminance`, which fires exactly when
  the error has grown to the whole threshold — a whole degree, twice the sun's 0.53 deg disc — at
  a period that follows the body's rate: 8.2 s while the clock runs normally, 85 ms while the
  direction limiter is slewing, 17 ms at the steepest point of a radiance ramp. `rhyCadence*`
  (`plugin/src/smooth.{h,cpp}`) rebuilds every `[fade] handle_cadence_ms` (2000), shortened only
  as far as needed that no rebuild carries more than `handle_step_deg` (0.5, about the sun's disc)
  of direction or `handle_rel_lum` (0.05) of luminance, and never below
  `handle_cadence_min_ms` (150, the bridge-churn floor). Measured on the same simulated sky
  through both rules: a normal morning goes from 0.12 creates/s at **1.00 deg** per rebuild to
  0.51 creates/s at **0.25 deg**; the 104.5 deg clock jump's slew from 8.1 to 5.2 creates/s;
  twilight's radiance ramp from ~10 creates/s (spacing down to 17 ms) to a bounded 6.7. One
  honest regression, stated where it can be read: during a steep ramp the luminance step is what
  the floor allows (up to 0.84 of itself near the 0.02 night floor, where `sky_abs_per_sec` makes
  small values fall fastest in relative terms — in absolute terms 0.08 → 0.03, a sun that has
  already set).
- **Fixed:** a period whose handle already carries the published values costs no bridge call, and
  a rebuild the frame's create budget refuses stays due instead of being forgotten — both of which
  were threshold behaviour in disguise. With `[sun] force_hour` pinned the sun now creates
  **nothing** (log: `rebuilds=0 … skip=30`).
- **Fixed:** angles are measured with `rhyAngleDeg()` (`atan2(|a×b|, a·b)`) instead of
  `acos(a·b)`. At rest the sun moves 0.002 deg per frame and `acos` on a float dot product that
  close to 1 returned 0.02 deg — 10x over — which pinned every body at its churn floor.
- **Added:** `[fade] handle_cadence_ms`, `handle_cadence_min_ms`, `handle_step_deg`,
  `handle_rel_lum`, all clamped in `config.cpp` (a floor above the period, a zero budget or an
  absurd bound is corrected rather than honoured), and the log lines that prove them:
  `config: fade … handle=2000ms(min 150ms, <0.50 deg, <5.0% lum)` at startup and
  `rebuilds=… cad=…ms step<=… skip=…` every 10 s.
- **Added (the renderer half): `config/rtx.conf.nopops`** — `config/rtx.conf` with nine temporal
  keys changed and nothing else, generated by `tools/rtx-nopops.py` and switchable with
  `tools/rtxprofile.sh nopops` / `restore` / `status`. The measurement that picked them is in
  [docs/FINDINGS.md](docs/FINDINGS.md), “The renderer's own time knobs”: this install's indirect
  integrator is **SHaRC**, not ReSTIR GI, so the `rtx.restirGI.*` block s28 named as the amplifier
  is inert here, and the live time knobs are the RTXDI temporal-reuse pair, the SHaRC and
  volumetric accumulation windows and the TAA new-frame weight. Every look key and all eight
  texture-hash lists are byte-identical to the shipped profile.
- **Tests:** `tests/handle_check.cpp`, 42 native checks — the same simulated sky (a normal morning,
  the measured 104.5 deg clock jump, the tour's `setTimeOfDay()` pin, twilight's 300x radiance
  ramp, a pinned clock) run through the old rule and the cadence, compared on creates/s, one step's
  size and the evenness of the spacing; `tests/test_rtx_nopops.py`, 6 checks (the profile is what
  its generator produces, the diff is exactly the nine keys, no look key or texture list moves,
  every key is known to this runtime and the two names that *look* like temporal keys are still
  unknown); `tests/test_ini_keys.py`, 7 checks (every key the shipped ini sets is read by
  `config.cpp`, every documented `[clock]`/`[fade]` default is the compiled default, and
  `kIniDefaults` agrees with both); `scripts/rtxprofile-selftest.sh` 37 → 73 checks (the nopops
  round-trip byte-for-byte, the nine-key diff pinned, the look keys and texture lists hashed).

### Shutdown (n09) — the game hung for two minutes after Quit (`cb0aac8`; the first attempt is parked as `08dc373`)

- **Fixed (n09): the plugin now notices the game is ending, and stops touching a dead Remix bridge.**
  rhY, 2026-11-12 10:39: quit from the menu, `NvRemixBridge.exe` exited, the window disappeared, and
  `gta-sa.exe` then sat there for ~2 min with its main thread in state **R** at 66 % CPU until it was
  killed — with `rhyremix.log` still printing `remix: live=383 …` every second and **no shutdown line
  at all** (evidence: `build/tmp/exit-hang-20261112.txt`). What the plugin was doing in that last
  second: drawing **302 street lights + 4 signal lights per frame** into a bridge client whose server
  process had already exited, with the `0x49DD4C` signal-head code patch still live. What the
  timeline rules out: our init thread (long since returned, and bounded at 120 s), the MoonLoader
  scripts (terminated 300 ms *before* the spin), and our own `Present` hook re-entering — our log
  stops at 10:39:14.652, the game's own exit path (`Unloading…`, `Session terminated.`) starts at
  10:39:14.989 and the last renderer line of the run is DXVK's `Failed to recreate swap chain`, so
  the spin is the game / bridge client retrying a device reset, which no ASI sitting behind
  `Present` can end. What is ours, and now fixed: the calls into the dead bridge, the missing
  shutdown log, and the two minutes of nobody noticing.
- **Added:** `plugin/src/shutdown.{h,cpp}` — a pure-C++ state machine for "is the bridge still worth
  calling?". Four signals, each with its own **persistence window** so ordinary things cannot turn
  the world's lighting off: `Present` failing (1500 ms), `TestCooperativeLevel` saying lost
  (3000 ms — alt-tab says that too), the device's own window no longer being a window (750 ms, read
  once from `IDirect3DDevice9::GetCreationParameters`, vtable 9, because the game hands `Present` no
  HWND), and our own climbing `create_fail + draw_fail` counters (4000 ms, and **never on their
  own** — that pattern is one light the server refuses, which `streetlights.cpp` already handles).
  A signal must hold continuously (a gap resets its own timer), death **latches**, and
  `DLL_PROCESS_DETACH` is instant. `tests/shutdown_check.cpp`, 44 checks, including the honest limit:
  a tracker fed from `Present` cannot fire when the game stops presenting, which is exactly what
  happened — hence the shell watchdog below.
- **Added:** one latch in `remix.cpp` (`rhyRemixShutdown()` → `rhyRemixApi()` returns null) covers
  every `CreateLight` / `DrawLightInstance` / `DestroyLight` / `SetConfigVariable` in the plugin,
  because every one of them already funnels through that pointer and every module already starts
  with `if (!api …) return;`. Skipped calls are counted (`rhyRemixSkippedCalls()`).
- **Added:** the `shutdown:` log lines that were missing — `shutdown: suspect (…)`,
  `shutdown: bridge gone (reasons=… signals_now=… first_signal=+Nms held=…ms uptime=…ms)`, one
  `shutdown: still running N ms after the bridge went away (…)` every `shutdown_log_ms`, and
  `shutdown: DLL_PROCESS_DETACH after N ms (…)` written without the log mutex (`rhyLogDetach()`,
  because at detach another thread can have died holding it). At the latch the signal-head detour at
  `0x49DD4C` is **unpatched** (`rhySignalHookUninstall()` restores the five original bytes it saved),
  so the game's teardown runs its own code, and the init thread's device poll stops early.
- **Added:** `rhyremix.ini [shutdown]` — `present_ms / device_ms / window_ms / calls_ms / log_ms /
  stop_present_after_ms`, documented in `config/rhyremix-production.ini` and covered by
  `tests/test_ini_keys.py` like `[clock]` and `[fade]`. `stop_present_after_ms` defaults to **0 =
  always forward `Present`**; setting it is the A/B that says whether the spin is inside the bridge
  client's `Present` (fixable from here) or inside the game's reset loop (not).
- **Added:** `scripts/test-session.sh` now runs an **exit-hang watchdog** next to the launcher: once
  it has seen the game up (bridge process alive or window present), a `gta-sa.exe` still running
  `RHYS_HANG_GRACE` s (default 20) after its bridge process **or** its window went away is logged,
  evidenced (`build/tmp/exit-hang-<stamp>.txt`: `ps -T` threads with STAT/%CPU/WCHAN, per-task state,
  and the tails of `scripts/rhyremix.log`, `moonloader/moonloader.log`, `rtx-remix/logs/remix-dxvk.log`
  — the same shape as the hand-made 2026-11-12 file), killed, and the run continues so Strata comes
  back. `scripts/test-session-selftest.sh` 34 → 46 checks: a game that outlives its bridge, a game
  that exits normally although its bridge died first (must NOT be called a hang), and a window that
  vanishes while the bridge lives.

### Exit hang, part 2 (n09b) — the launcher now ends the wine tree (`cb0aac8`)

- **Fixed (n09b): `launcher/launch-rtx.sh` ends `NvRemixBridge.exe` and the prefix's `wineserver`,
  during the run and after it.** The second hang, 11:48 the same day (`build/tmp/exit-hang-20261112-b.txt`,
  started straight from the launcher): after Quit, **both** `gta-sa.exe` (18 % CPU) and
  `NvRemixBridge.exe` (406 % CPU — two `geometry-proces` threads and two main threads at 100 % each,
  `dxvk-cs` 11 %) kept running for **sixteen minutes** holding 3.6 GB of VRAM, and the launcher was
  still inside `waitforexitandrun`, so its cleanup never ran. The bridge's own last line was
  `Client window became inactive, disabling timeouts for bridge server...`: its exit callback only
  fires when the client *process* exits (that process is the spinning game) and its IPC timeout is
  switched off by the window going inactive. `end_wine_tree` SIGTERMs/SIGKILLs
  `NvRemixBridge.exe` by pid and then runs Proton's `files/bin/wineserver -k` with
  `WINEPREFIX=<compatdata>/pfx` — which is what takes `gta-sa.exe` with it — and a sweep by
  pid/name covers a wineserver that `-k` did not take, or a Proton install with no wineserver where
  it should be. The `-k` is only issued when a wineserver for this prefix is actually running:
  invoking `wineserver` with no server up would start one just to kill it. It runs after the wait returns (so a clean exit cleans up too) and
  `watch_run_end` runs during it: window destroyed (armed only after the window was seen; `wmctrl`
  lists minimised windows, so alt-tab is not "gone") or `gta-sa.exe` gone while the bridge lives,
  for `RHYS_EXIT_GRACE` s (default 20, `0` = watchdog off, the after-exit cleanup always runs) →
  log, `<game>/logs/exit-hang-<stamp>.txt`, end the tree. New:
  `scripts/launch-cleanup-selftest.sh`, 33 checks against stub processes, run by `check-all.sh`.
- **Finding (n09b): there is no `bridge.conf` option that reliably keeps the timeout on.**
  `.trex/bridge.defaults.conf` documents `disableTimeouts` (default True) and `infiniteRetries` as
  the two the bridge flips itself "when the game window becomes inactive", and says of both that they
  "may be changed dynamically at runtime based on application state, therefore changing the default
  in the settings does not have much of an effect overall" — i.e. exactly when we need them on, the
  runtime turns them off again. `RHYS_BRIDGE_TIMEOUTS=1` writes `disableTimeouts = False` +
  `infiniteRetries = False` into `.trex/bridge.conf` (backup: `bridge.conf.bak-before-rhy-timeouts`)
  as the A/B, and the launcher now reports what that file says on every start.
- **Fixed (gate): `scripts/test-session-selftest.sh`'s real-Strata check is no longer a race.** That
  check — not the shutdown code — is what parked n09: 45 of 46 checks passed and the last one failed
  because a *real* Strata pid died mid-run (rhY took Strata down for the 11:48 game run two minutes
  later). It now fails only if no real `serve/server.py` / `engine/strata` is running afterwards at
  all, which is the thing the sandbox guard is there to protect and which the stub starter in the
  test cannot bring back.

### Tools (s18, s22, s23) (`e3d57a3`, `67c6e70`)

- **Named by the n15 sweep:** `e3d57a3` ("Strata s18-s20", 2026-11-11 22:23) is the commit that
  carries s18 (`tools/remix_texhash.py`, `config/mirror-hashes.txt`), s19 (the `signalhook.cpp` /
  `signalheads.cpp` hardening and `tests/signals_check.cpp`) and s20 (the first big CHANGELOG
  rewrite). `tests/test_changelog_coverage.py` had been failing on it since n14 - it was the one
  commit on main that no section named; naming it here is the whole fix.

- **Added:** `tools/remix_texhash.py` — decodes a `.txd` / `gta3.img` raster the way D3D9 receives
  it and prints the Remix texture hash. The hash is `XXH3_64bits(mip-0 raster bytes)` (xxHash 0.8.0
  as bundled in [dxvk-remix](https://github.com/NVIDIAGameWorks/dxvk-remix) 1.6.1, default secret;
  `XXH64(data, 0)` only for textures that still need a CPU upload), taken once at the first upload
  of subresource 0. It reproduces **55 of the 167** hashes listed in `rtx.conf` and **164 of the
  207** hash-named replacement textures the asset pack ships; the ones it cannot reproduce are
  textures the game generates itself (sea, sky, some UI).
- **Changed:** `config/mirror-hashes.txt` now carries both mirror hashes live —
  `xvehicleenv128 = 0x45DF1EA60BB202B5` (independently confirmed: the pack's own
  `SARemix_OriginalVehiclesPBR.usda` overrides `mat_45DF1EA60BB202B5` as glass) and
  `vehicleenvmap128 = 0x6BAFC5F6AEE0A6B5` — so `tools/mirrors.py --dry-run` reports
  "2 material override(s)" and the mirror step no longer needs an in-game `Alt+X` hash read
  ([docs/MIRRORS.md](docs/MIRRORS.md)).
- **Tests:** `tests/test_remix_texhash.py` (19 checks: the python xxHash implementations against
  published xxHash test vectors, a synthetic `.txd` with 32-bit / 16-bit / DXT5 rasters, the
  `rtx.conf` hash-list parser; the real-game table check skips when the game is not installed).
- **Added (s22):** `tools/rtxprofile.sh {perf|current|orig|status|restore|check} [GAME_DIR]` and
  `config/rtx.conf.perf`, a 60 fps profile for the 4070 Ti SUPER 16 GB install. The audit tooling
  is the durable part: `tools/rtx-runtime-keys.py` turns `strings` on `<game>/.trex/d3d9.dll`
  into `config/rtx-runtime-keys.txt` (246 dotted option paths + 1345 bare option names), and
  `tools/rtxkeycheck.py` grades every key of a profile as `dotted` / `leaf+category` / `leaf`.
  That found two keys the shipped `rtx.conf` has been carrying for weeks which **this build does
  not contain at all** — `grep -c` on the 282 MB runtime and on every other `.trex` module returns
  0 for `rtx.enableVolumetricLighting` and `rtx.minReplacementTextureMipMapLevel`; the name this
  runtime has is `rtx.volumetrics.enable`. The log of the last run also settles what is actually
  running: `[RTX] Integrate Indirect Mode: Spatially Hashed Radiance Cache - activated`, so the
  eleven `rtx.restirGI.*` keys in `rtx.conf` are inert tuning, and the profile's biggest lever is
  `rtx.spatiallyHashedRadianceCache.qualityPreset = 0` (tooltip: "Lower presets reduce update
  tracing cost but slow cache convergence and lighting response"). Switching keeps
  `<game>/.rhys-rtxprofile/rtx.conf.{prev,pristine}`; `perf`→`current` and `perf`→`restore` hash
  back byte-identical, on a copy of the real file as well as in the fake tree.
- **Tests (s22):** `scripts/rtxprofile-selftest.sh` (37 checks, wired into `scripts/check-all.sh`):
  the key audit plus a negative control (the two legacy keys must still fail), byte-identical
  round-trips through `perf`/`current`/`orig`/`restore`, refusal to write outside a game dir or
  inside the repo, and a check that every texture-hash list and every look-critical key survives
  the switch.
- **Added (s23):** `tools/tour-report.py GAME_DIR [--since TIME]` — the tour's output becomes a
  judgement instead of a folder of 1920x1080 PNGs. It joins `screenshots/tour-manifest.txt` to
  `screenshots/shots/*.png` **by time** (the Lua `note()` line and the file are written a second
  apart, at second resolution, with manual F12 shots sitting in the same directory), then writes
  `<game>/screenshots/report-<tour start>/`: labelled contact sheets, 4 shots each, and `report.md`
  with objective numbers per shot — Rec.709 mean luma plus p05/p50/p95, % clipped white
  (`max(R,G,B) >= 250`), % black (`<= 4`), mean HSV saturation, and for the signal stops the
  fraction of red / amber / green pixels (hue bands gated on S>=0.45 and V>=0.25, so grey tarmac
  never reads as a signal) — each shot followed by the `rhyremix.log` line nearest it in each of
  three categories: `streetlights: N active, N drawn this frame`, `trafficlights: … (green=…
  amber=… red=…)` and `sky: sun elev=… lamps=… (hour=…)`. On the real 2026-11-11 tour: **13 of 13**
  manifest entries paired, 18 of 31 shot files correctly left as unclaimed manual shots, and the
  numbers say what the eye only guessed — `03-sodium-lamp-closeup-2100` mean luma 26.2 with 6.7 %
  crushed black, `11-water-santa-maria-2100` mean 6.1 with 40.8 % black, and the two signal stops
  at 2.24 %/0.09 % red, 23.0 %/2.4 % amber, 5.7 %/21.2 % green.
- **Tests (s23):** `tests/test_tour_report.py` (27 checks on synthetic PNGs, a hand-written
  manifest and a fake log under `build/tmp/`): manifest parsing, filename timestamps and the
  mtime fallback, pairing (tolerance, one file cannot be claimed twice — the closer entry wins,
  unclaimed files listed), every `--since` form, the metric definitions on flat grey/white/black
  images, signal fractions (and that dim or desaturated head pixels count 0 %), the pure-python
  fallback agreeing with the numpy path, nearest-log-line selection with the per-category window,
  and one end-to-end run: report-dir naming from the `tour start` event, `ceil(n/4)` sheets of the
  right pixel size, and the markdown rows.

### Mirrors (s21) — the two "mirror hashes" were never mirror-only (`67c6e70`)

- **Fixed:** `config/mirror-hashes.txt` is **empty on purpose**. s21 parsed every stock vehicle
  `.dff` (209 models of `models/gta3.img`, i.e. everything named in `data/vehicles.ide`, plus the 50
  `.dff` under `modloader/`) with a real RenderWare clump parser and recorded, per material, which
  *slot* each texture sits in and which atomic/frame it hangs on:
  `xvehicleenv128` is the env/reflection slot of **4197 materials on 179 of 209 models** (body
  `chassis`, all four doors ok+damaged, bonnet, boot, bumpers, windscreens, bikes, boats) and
  `vehicleenvmap128` the same on 81 models — **205 828 and 19 589 triangles** respectively, and
  neither is ever a diffuse or specular texture. The entry above (s18) is therefore superseded:
  `tools/mirrors.py` with those hashes would have made a mirror out of every painted car panel.
- **Added:** the mirror geometry was located geometrically instead — car-space frame chain, then "the
  small cluster that sticks out past the front-door skin" — and its own diffuse texture is
  `vehiclelights128` (`0x5AD92DBA0F2D1BCA`), which is also every head/tail/indicator lens (1146
  materials, 163 models). No GTA SA texture is mirror-only, so `docs/MIRRORS.md` gains section 7:
  override the mirror **mesh** (`mesh_<MESHHASH>`, the pack's own recipe for its re-authored
  taxi/police mirrors), with the measured geometry to look for (4–8 triangles, ≈0.04 m², |x|
  1.05–1.3 m, z 0.2–0.6 m, just ahead of the front door).
- **Changed:** `tools/mirrors.py` now refuses any hash whose line in the list file does not carry
  `# verified-mirror-only`, prints why, and exits with an explanation instead of writing a layer
  when nothing is verified; `--hash` needs the marker in the same argument or
  `--verified-mirror-only`.
- **Tests:** `tests/test_mirrors.py` (9 checks against a fake game tree: unmarked hash refused,
  marked hash accepted, mixed file, `--hash` marker rule, shipped config yields zero overrides,
  generated layer contains only verified hashes); `tests/test_remix_texhash.py` now asserts the
  config carries no hash and that the three texture hashes in the s21 table still compute to the
  documented values. Method, parsers and the full usage tables: `.strata/notes-s21-mirror-hash-check.md`.

### Photo tour (s31) — five stops that photographed nothing (`295b2ac`)

- **Fixed (s31): the tour stops rhY could not judge.** All five were re-placed from the game's own
  IPL data rather than by eye: `05-burger-shot` had its camera inside the `burggrnd1_law` wall; the
  `10/11-water-santa-maria` stops dropped CJ **into** the sea (shot 11 is underwater and black) and
  now stand on the pier deck at the bench (378.46, -1963.88, z 7.21); `12-car-mirror` had no car, so
  it now spawns a `sadler` (543) at the Grove Street kerb; `13-ped-crowd` looks along the bench
  walkway with a 25 s settle and creates no ped on purpose (`create_char` is one of the opcodes that
  crash this build); `14-vegetation` moved to the densest green spot in the hills (1300, -920):
  20 trees + 8 lawns within 45 m. **Undo:** `git revert 295b2ac`, or keep the previous
  `$G/moonloader/SARemix_Tour.lua`.
- **Fixed (s31): `tools/tour-report.py --since` re-reported yesterday's run.** A bare `HH:MM` took
  its date from the first manifest line and the report directory from the first `tour start`, so on
  2026-11-12 it printed the 2026-11-11 section again. `split_sections()` / `pick_section()` now cut
  the manifest at its `tour start` lines and report the run the cut asks for; the old tail semantics
  are kept. `tests/test_tour_report.py` 29 → 36 checks (two runs in one manifest, bare clock, full
  timestamp, older run, no `--since`).
- **Measured (s31): the 2026-11-12 tour, all 16 stops** (`--since 00:48` → 16 of 29 entries paired,
  4 sheets, 7 unclaimed manual shots): mean luma / % black — 00-baseline 128.6/0.0, 01-under-lamp
  75.4/6.5, 03-sodium 26.3/6.6, 06-signals-close 76.7/1.5 (green 33.4 %), 08a-sunset-east 107.6,
  08b-sunset-west 78.2, 10-water-noon 144.2, **11-water-2100 6.8/29.2 (the only flagged shot)**,
  12-car-mirror 145.1 (sat 0.166), 13-ped 134.2 (0.264), 14-vegetation 168.1 (0.262). Those are the
  numbers `docs/NEXT-TEST.md` asks the next run to beat.

### Character packs (s27) — one of the six packs is actually fetchable (`d4706b9`)

- **Changed (s27): `deps/manifest.json` now says what is and is not obtainable.** `criminal-peds-vary`
  filled in (sha256 `9b267df95116bfc4…`, 205,557,524 B, 2026-08-10 build, with the licence caveat
  spelled out: the gtainside copy is an unauthorised re-upload, recorded for reproducibility,
  `redistribute` stays `no`); `upstream_name` and the exact download-button URLs recorded for all six,
  so a human can finish what a bot cannot. **Undo:** the manifest is data only — `git revert d4706b9`.
- **Measured (s27): why the other five are not fetched.** Four MixMods packs are behind a Cloudflare
  Turnstile on sharemods, one behind a LibertyCity login/queue; the GitHub mirror of PedFuncs has
  source only (MIT, no release). The gtainside "Original Peds in HD" parts were measured and rejected
  in favour of Proper Characters Retex HD (547 MB of mesh+texture replacements vs 22 MB of textures
  only), and Criminal Peds Vary turns out to need ModelVariations + PedFuncs, which one manifest
  target cannot carry. `docs/CHARACTERS.md` §7/§12 carry the table and the hand-download URLs.

### Testing (s26) — one page for the next in-game session (`2f67e4a`)

- **Added:** [docs/NEXT-TEST.md](docs/NEXT-TEST.md), the run order for the session that has been
  waiting since the handoff: preflight (LAA check, `rtxprofile.sh status`, install the current tour
  script) → baseline tour with nothing new → signal heads → water realistic vs calm →
  `tools/rtxprofile.sh perf` → `tools/vegetation.sh on` (now that the exe is LAA) →
  `tools/characters.sh on`. Each step is one command, what to look at, what `tools/tour-report.py`
  should show (against the 2026-11-11 09:06 numbers), and a one-line rollback. It also records what
  the install actually looks like today: live `rtx.conf` matches no profile (`profile : unknown`),
  water is already `realistic`, no vegetation/character state dirs, and `characters.sh on` refuses
  because nothing is staged.
- **Tests (s26):** `tests/test_next_test_doc.py` (8 checks) is the scripted half of the gate: every
  command line and inline rollback in the doc is tokenised and resolved — repo paths must exist,
  subcommands must appear in that script's own usage block, `--flags` in its source, anything else
  must be allowlisted and on PATH — plus the one-screen bounds and the requirement that the doc send
  the reader through `scripts/test-session.sh` rather than the launcher. Negative controls (renamed
  tool, invented action, invented flag, unknown command, direct launcher use, over-long doc,
  missing rollback) all fail it.
- **Fixed:** `tools/tour-report.py` matched only `trafficlights: N active` as a signal log line, so
  on the source=game runs — the mode the signal test is about — the report's signals column was
  blank. It now also accepts `trafficlights[source=game]: N heads lit … hook: calls=…` (both shapes
  are in the real 2026-11-11 log: 303 list lines, 4 source=game). `scripts/diagnose.sh` had the same
  blind spot and gains a read-only "signal heads" section printing the patch line and the hook
  counters. `tests/test_tour_report.py` 27 → 29 checks.
- **Found:** the game's `moonloader/SARemix_Tour.lua` is the old 13-stop version and **nothing in
  `deploy.sh` or `apply.sh` installs it** — only `SARemix_Shot.lua` is copied. A `--tour` run today
  therefore produces no numbers for the mirror, pedestrian or vegetation stops. The doc carries a
  one-line `cp` as the preflight; the durable fix (a deploy step for the tour script) belongs in the
  queue.

### Parked and aborted work — tried, measured, **not** in the shipped tree

Four attempts did not pass `scripts/check-all.sh` and were parked on their own branch; two more never
started. Their measurements are in [docs/FINDINGS.md](docs/FINDINGS.md) and their status in
[docs/STATUS.md](docs/STATUS.md); none of their config or plugin changes is live.

- **Parked (n09, `08dc373` on `failed/n09-exit-hang-114604`): the first exit-hang fix.** The shutdown
  state machine and the watchdog were right — 45 of 46 selftest checks passed — and the one failure
  was `scripts/test-session-selftest.sh`'s *real-Strata* guard losing a race (a real Strata pid died
  mid-run because the game session was about to start). Re-tried as n09b (`cb0aac8`), where that check
  was rewritten to fail only if no Strata is running afterwards at all.
- **Parked (n05, `6eafd4d` on `failed/n05-night-brightness-185753`): nights darker, lamps down, sunset
  gray.** The work is complete and its numbers are recorded (lamps `radiance_scale 1.0 → 0.4`, bulb
  `1200 → 840` with a new `[trafficlights] radiance_scale` so the street knob stopped reaching the
  signals, `rtx.localtonemap.highlights = 1.5`, a night ramp of `rtx.localtonemap.shadows` 2.5 → 1.0,
  `tools/night-brightness.py`); the gate failed on `scripts/test-session-selftest.sh` **timing out**
  (exit 124), not on any of these. Nothing shipped.
- **Parked (n10, `30a1a43` on `failed/n10-night-ambient-realism-172920`): the fake night ambient.**
  Same story: the analysis stands (the local tonemapper, the sky probe's ambient ceiling, the fallback
  light welded to the camera and a distant light pointing up out of the ground), the plugin gained
  `solarClampLightDir()` and `skyAmbientCap()`, and the gate failed on the same selftest (exit 1).
  Nothing shipped.
- **Aborted (n06, `48d93d5` on `wip/n06-pier-and-sign-lights-1922`): pier lamp heads and highway-sign
  bulbs.** Interrupted by a reboot mid-task; the commit is a WIP snapshot, never gated, and its
  findings (D and E) are still open.
- **Stopped with no commit (n11): every lamp in the world lit (V2, findings D/E).** The plan and the
  reading are in `.strata/notes-n11-every-lamp-lit.md`; the queue recorded `rc=2: no changes`. The
  2dfx reader, the world inventory and the coverage tool were never written, so the boardwalk and pier
  lamps are still dark.
- **Never started (n03, n08).** `n03-next-session-ready` (deploy the tour script, bound the logs,
  rewrite NEXT-TEST) and `n08-fps-steadiness` (frame pacing, finding G) are still in the queue with no
  notes file. The measurement half of n08 was done during the n14 documentation sweep and is recorded
  in [docs/FINDINGS.md](docs/FINDINGS.md), "Frame pacing: what the 2026-11-12 MangoHud logs say"; the
  fix is still nobody's.

### Docs and handoff (`df0ec29`, `50f6583`, `35e5d8d`)

- **Added:** the 2026-11-12 handoff note (state, queue, what was waiting on an in-game test), the
  self-hosted-deps product rule, and a queue update for s24-s26. These three commits touch
  `docs/notes/` only; the handoff has since been replaced by [docs/STATUS.md](docs/STATUS.md) and the
  personal/session material moved out of the public tree (n14).

### Documentation (n14) — everything since 2026-11-11 is now in the repo (this commit)

- **Added:** [docs/STATUS.md](docs/STATUS.md) — every finding rhY reported in the three 2026-11-12
  test sessions with a status (done / partly / open), the commit or task that owns it, the parked
  tasks with their failure reasons, and what to test next.
- **Added:** the gaps this sweep found and closed in [CHANGELOG.md](CHANGELOG.md): s28, s31, s32, s27
  and n12 had **no entry at all**, and no commit after `e19fde6` was named by hash. Every section
  above now names its commit(s), and `tests/test_changelog_coverage.py` (new, in
  `scripts/check-all.sh`) fails if a commit on any branch since 2026-11-11 is not named here.
- **Added:** [docs/FINDINGS.md](docs/FINDINGS.md) "Frame pacing" (the MangoHud numbers behind finding
  G, and the fact that our light churn does not correlate with the spikes) and "Night realism: what
  n05 and n10 measured and did not ship".
- **Changed:** [README.md](README.md) gained a feature list, the water-variant table, a lighting-model
  summary and a test-flow section; `docs/notes/` lost the personal/session material (the local model
  setup, the queue runners, the memory-graph frontmatter) — the repo is public.
- **Undo:** `git revert` this commit; nothing outside the docs and one new test file changes.

### Look target (n15) — what the reference video actually is, and how big the gap is (on top of `555c23c`)

- **Added:** [docs/TARGET.md](docs/TARGET.md) — the reference (DubStepZz,
  https://www.youtube.com/watch?v=BjWhv_gfjAk, 2026-11-12), what it does and does not tell us
  (its "4 products" are YouTube's shopping shelf: a PS5 on StockX, an Xbox bundle, a DualSense on
  Dell, an Elite controller on GameStop — **no mod list, ever, on that channel**), why the base is
  classic SA + RTX Remix + the same Hemry81 pack we already run, and the measured gap table.
- **Added:** `tools/target-gap.py` (+ `tests/test_target_gap.py`) — six look families measured the
  same way on a YouTube frame and on our own lossless tour shots: histogram, highlight roll-off,
  saturation/warmth, local contrast, haze (far/near local contrast), specular share. Everything is
  resized to 640x360 and JPEG-round-tripped first, because compression moves those numbers more than
  most lighting changes do.
- **Measured (the gap, 5 reference frames vs our 4 daytime tour shots of 2026-11-12 10:22):**
  headroom p99.9/p50 **1.41–1.81 ours vs 1.79–3.49 theirs**; ceiling pixels (luma ≥ 252)
  0.003–0.027 % vs 0.01–3.95 %; mid-tone anchor p50 ≈ 147 vs ≈ 108; warmth mean(R−B)
  −67…+10 vs **+18.7…+48.3 in every reference frame**; sky 92–128 luma at saturation 0.28–0.61 vs
  160–199 at 0.13–0.26; haze far/near 1.22–3.67 vs 0.46–1.18. Local contrast is **not** the gap.
- **Measured (why):** DLSS 5 is not an upscaler — it is a one-step pixel-space diffusion stage that
  *generates the final displayed appearance* (https://research.nvidia.com/labs/adlr/DLSS5/), it is
  integrated in **RTX Remix 1.6** (our runtime: `rtx.dlssNeuralRendering`, `intensity`,
  `structuralStrength`, `toneStrength`, `model` A/B/C, `enableHighlightRecovery`,
  `enableVolumetricControlMask` — all present in the 1.6.1 option table), and it is **RTX 50-series
  only**: this box is an RTX 4070 Ti SUPER on driver 610.57.04 and the runtime already answers
  `NVIDIA DLSS-NR support query failed (0xbad00010)`. The reference's biggest advantage is hardware
  we do not have, so the plan wins T1/T3/T4 with materials, fog and grading instead.
- **Measured (the pack):** the GTASA-Remix install is **2334** `mat_<HASH>` materials with only
  315 `_normal` / 280 `_rough` / 185 `_height` / **32 `_metal`** maps — one material in seven has a
  roughness map, metallic barely exists. That is the mechanical cause of the flat matte look (T1).
- **Added:** four task files `.strata/n16-tonemap-highlights.md`, `n17-fog-aerial-perspective.md`,
  `n18-legacy-material-brdf.md`, `n19-dlss5-neural-probe.md`, queued in `.strata/night-queue.txt`
  immediately after `n15-target-research`.
- **Changed:** `deps/manifest.json` gained `proper-shaders` (MixMods/Junior_Djjr: deferred rendering,
  volumetric PBR clouds, PBR dynamic water, PBR shadermaps — the best public description of what T1+T3
  done properly means for SA; **proprietary supporter tiers, `redistribute: no`, a reference and not a
  dependency**, because it rewrites the same D3D9 path our Remix bridge owns), and the `gtasa-remix-pack`
  entry now records what GitHub says (MIT, last pushed 2025-09-20, 10.3 GB).
- **Undo:** delete `docs/TARGET.md`, `tools/target-gap.py`, `tests/test_target_gap.py` and the four
  `.strata/n1[6-9]-*.md` files, drop the four queue lines and the two manifest changes; no behaviour,
  config or plugin code changed in this task.

### Grading (n16) — the daytime frame had no bright end: the tonemapper is now Global (on top of `a0cd0f4`, shipped in `b1afcc2`)

- **Changed (the look, all three profiles):** `config/rtx.conf`, `config/rtx.conf.perf` and
  `config/rtx.conf.nopops` now carry `rtx.tonemappingMode = 0` (Global; the runtime default is
  **1 = Local**), `rtx.tonemap.exposureBias = -0.5` (EV anchor; auto exposure is off in every
  profile, so it is a hard number: `exposureFactor = 2^(exposureBias + userEVBias)`,
  `rtx_tone_mapping.cpp:234`), `rtx.localtonemap.shadows 2.5 → 1.0` and
  `rtx.localtonemap.highlights 4 (never set) → 2.0`, and `rtx.tonemap.finalizeWithACES = True` is
  kept. **Why:** the local tonemapper renders three synthetic exposures (×2^shadows, ×1, ×2^-highlights
  — with our 2.5 and the default 4 that is a **90:1** spread), weights them per pixel by a gaussian
  around 0.5 gamma luminance, mip-averages them to `rtx.localtonemap.mip = 3` and applies the fitted
  line as one multiplier on R, G and B — its own comment: "a crude multiplier on all RGB channels.
  This is... generally pretty wrong". That is a local normalisation toward mid grey: it lifts the
  floor, pulls the ceiling down, and re-solves around whatever the camera happens to include. It is
  the mechanism docs/FINDINGS.md already named for the night frames; n15 measured it at noon
  (headroom p99.9/p50 **1.41–1.81**, ceiling **0.003–0.027 %**, p50 **123–170** against the
  reference's 1.79–3.49 / 0.01–3.95 % / 65–142). The global pass instead fits one curve to the
  frame's histogram, pins linear 0.18 to 0.18, maps the top of the range to white and mixes the curve
  with the previous frame's at `exp(-0.07)` (~14 frames), so a lighting change converges instead of
  snapping — which is also a second smoothing path for n01's "no pops" A/B.
- **Fixed (a key that would have made the whole task do nothing):** the task file asked for
  `rtx.tonemap.tonemappingMode`. **That option does not exist** — the name is `rtx.tonemappingMode`
  (`RTX_OPTION("rtx", TonemappingMode, tonemappingMode, TonemappingMode::Local, …)`,
  `rtx_options.h:1181`; the config name is `category + "." + name`, `rtx_option.h:267`), and a key
  that names no option is kept in the file and ignored (`rtx_option_layer.cpp:416-421`). Same trap,
  two more keys the task suggested: the brightness slider's real names are
  `rtx.userBrightness.userBrightness` and `rtx.userBrightnessEVRange.userBrightnessEVRange` (the
  short spellings are only category strings — and our own plugin already recorded the runtime
  warning `'rtx.userBrightness' is not an RtxOption` in `plugin/src/rtxconf.cpp:147-160`), both carry
  `RtxOptionFlags::UserSetting` so they belong in `user.conf`, which sits **above** `rtx.conf`; and
  `rtx.postfx.exposureFraction` is not an exposure knob at all — it is the shutter fraction of the
  post-FX motion blur (`rtx_postFx.cpp:127,294`, default 0.4). None of the three is shipped.
- **Added (tooling, because `tools/rtxkeycheck.py` could not catch any of that):**
  `tools/rtx-option-audit.py` + `config/rtx-options-1.6.1.tsv` — the runtime's **own** option table
  (name / type / default / min / max), fetched from the `remix-1.6.1` tag of
  https://github.com/NVIDIAGameWorks/dxvk-remix (MIT) where `RtxOptions.md` is auto-generated by the
  runtime itself and a runtime unit test fails if it drifts. `rtxkeycheck.py` greps the binary's
  strings, so it grades `rtx.tonemap.tonemappingMode` as "ok (leaf+category)"; the new audit answers
  "does an option with this exact name exist, and what is its default?". `config/rtx-dead-keys.txt`
  carries the keys our profiles keep on purpose, each with its reason.
- **Measured (found, deliberately **not** fixed):** the new audit flags **three more dead keys our
  own profiles have been carrying** — `rtx.enableFogMaxDistanceRemap` (the option is
  `rtx.volumetrics.enableFogMaxDistanceRemap`, default **True**, so fog remap has been running at its
  default, not at our False) and `rtx.antiCulling.object` / `rtx.antiCulling.light` (the options are
  `…​.enable`, default **False**, so n04's anti-culling has never been on). They are annotated in
  place and listed in `config/rtx-dead-keys.txt`, and **not renamed**: making them live is a fog
  change (n17 owns fog) and a TLAS/VRAM decision (`numObjectsToKeep` default 10000 on a card that
  already ate one OOM device loss). docs/FINDINGS.md, "Dead keys".
- **Measured (before, so the after-run is one command):** `build/tmp/n16/gap.json` (the 5 reference
  frames + our 4 daytime shots, reproducing n15's numbers) and `build/tmp/n16/good-before.json` —
  the shots the 2026-11-12 report called good, from the same tour: `02-grove-30m-pools-2100`
  p50 17.4 / headroom 8.6, `03-sodium-lamp-closeup-2100` p50 7.0 / 28.2,
  `04-tungsten-shopfront-2100` p50 63.6 / 3.1, `08a-sunset-east` p50 100.6 / 2.3,
  `08b-sunset-west` p50 86.3 / 2.1, `09-grove-dawn` p50 27.8 / 4.0. Those night numbers are the risk
  of this change stated plainly: a −0.5 EV anchor sized for noon is too much at 21:00, and the
  abort ladder is in docs/TARGET.md §6.
- **Not measured, and that is written into the config files:** this task could not render — the
  header's rule 1 forbids running the game, wine or Steam, and a tonemap change only becomes a
  number once the runtime draws a frame. Keys, defaults and mechanism are measured (the runtime's own
  table and source); the direction of each knob is derived from that source; the after-numbers are
  owed by the next live run, whose exact command and abort criteria are docs/TARGET.md §6.
- **Tests:** `tests/test_rtx_option_audit.py` (the table is the runtime's, the fake names are still
  rejected, the dead-key list stays exactly 5 and every entry stays dead, all three profiles ship the
  same five tonemap values, no profile writes the UserSetting brightness keys or the motion-blur
  shutter) and `scripts/rtxprofile-selftest.sh` gained the option-table audit + its negative control;
  `tests/test_rtx_nopops.py` now lists the tonemap keys among the look keys the temporal A/B may not
  move.
- **Added:** `.strata/n22-daynight-exposure-anchor.md` (queued in `.strata/night-queue.txt`
  immediately after n16) — the follow-up this task created: one static EV anchor cannot serve noon
  (p50 147) and 21:00 (p50 7-17) at once, and the plugin is the only thing that knows the sun's
  elevation. It carries the numbers it has to beat and the rule it must not break (push it through
  `plugin/src/smooth.cpp`'s rate limiters, or an exposure change becomes a pop).
- **Undo:** `tools/rtxprofile.sh current <game>` puts the previous `rtx.conf` back byte for byte, or
  delete the n16 block (five keys) from the three profiles. The tooling undoes independently: delete
  `tools/rtx-option-audit.py`, `config/rtx-options-1.6.1.tsv`, `config/rtx-dead-keys.txt`,
  `tests/test_rtx_option_audit.py` and the three selftest lines.

### Grading (n22) — one EV anchor could not serve noon and 21:00: the exposure now follows the sun (on top of `b1afcc2`)

**Not yet run in game.** Everything measured below is a before-number, a fact about the runtime's own
option table, or a number the plugin's own code computes; the pixel side is owed by the next **live
run** (rule 1 of every task here forbids running the game, wine or Steam, and an exposure change only
becomes a pixel once the runtime draws a frame). Protocol: [docs/TARGET.md](docs/TARGET.md) §6.

- **Added (plugin): `rtx.tonemap.exposureBias` is now driven from the sun's elevation at runtime.**
  n16's `-0.5` is one number for the whole frame in Global mode
  (`exposureFactor = 2^(exposureBias + userEVBias)`, `rtx_tone_mapping.cpp:234`) and rtx.conf is a
  file: sized to pull p50 147 → ~110 at noon it takes a quarter of the light off a 21:00 frame that
  already sits at p50 7-17 with 34-52 % black (`build/tmp/n16/good-before.json`). New module
  `plugin/src/exposure.{h,cpp}` (pure C++, like `tod.cpp`/`shutdown.cpp`) blends
  `night_ev → day_ev` over solar elevation with `skyDaylightFactor`, and `sun.cpp` pushes the result
  through `remixapi_SetConfigVariable` the way it already pushes `rtx.skyBrightness`.
  **Noon is a no-op by construction:** `day_ev = -0.5` is the same number the three profiles carry,
  so the daytime grade cannot move and the noon numbers the live run takes are still n16's; only the
  night end moves (21:00 gets `+0.5` EV = 2x the light, inside the runtime's own
  `DragFloat("Global Exposure", -4, 4)` span, `rtx_tone_mapping.cpp:134`).
- **Fixed (no pops): an exposure change is rate limited.** New primitive `RhySignedRate`
  (`plugin/src/smooth.cpp`) — `RhyRate` cannot carry it: its bound is multiplicative and it clamps to
  ≥ 0, and an EV is signed with an absolute unit (measured in `tests/smooth_check.cpp`: `RhyRate`
  toward −0.5 EV publishes 0.0 forever). Shipped bound `[tonemap] max_ev_per_sec = 0.05` EV/s, so the
  full swing takes ≥ 20 s; the floors (0.005..0.25 EV/s, EV clamp −8..+8, band must run upward) are in
  `rhyExposureClamp()` and cannot be talked out of from the ini. Measured: the sun at the real clock
  rate moves the anchor at ≤ 0.0004 EV/s, so the bound only ever bites on a clock that jumps — the
  2026-11-12 log recorded 104.5° of sun elevation between two samples, and `SARemix_Tour.lua` pins
  the clock per stop (13:00 → 21:00 in one frame steps the target the whole 1.0 EV; the published
  value still takes 19.9 s).
- **Added (config): a `[tonemap]` section** in `config/rhyremix-production.ini` — `apply` (the undo
  switch), `day_ev`, `night_ev`, `dusk_deg`/`dawn_deg` (same band as `[sky]`), `max_ev_per_sec`,
  `min_ev`/`max_ev`; written by `kIniDefaults`, logged as `config: tonemap exposure …`, and the
  10 s sky line now carries `exposure=… EV (target …, max step …)`.
- **Deliberately not touched:** `rtx.userBrightness` / `rtx.userBrightnessEVRange` (the real names are
  doubled, they carry `RtxOptionFlags::UserSetting`, they are the Remix UI's Brightness slider, and
  `user.conf` sits **above** `rtx.conf` — `rtx_option_layer.cpp:386-397`), `rtx.tonemappingMode` at
  runtime (switching mode re-arms the local pyramid and changes the look discontinuously at the
  switch point) and `rtx.postfx.exposureFraction` (the motion-blur shutter, n16's audit).
- **Tests:** `tests/exposure_check.cpp` (new, run by `check-all` as `nat-expo`) — equal elevation on
  the way up and down gives the same EV (19:30 ↔ 06:24), the pair symmetric about solar noon agrees,
  noon equals the profile's −0.5, the floors hold, a whole simulated game day at the real clock rate
  never exceeds the bound and lands back where it started, and a pinned clock does not step the
  exposure; `tests/smooth_check.cpp` gained the `RhySignedRate` bounds (68 checks);
  `tests/test_exposure_anchor.py` (new) checks the key against the runtime's own option table, that
  the plugin pushes it from `g_sunElev` and never from the clock, that it never sends the four
  forbidden names, that the ini sits inside the compiled floors, and that the docs say which numbers
  are measured and which are owed; `tests/test_ini_keys.py` now documents `[tonemap]` and pins
  `day_ev == rtx.conf`'s static anchor.
- **Fixed (docs gate):** `CHANGELOG.md` now names `b1afcc2` — n16's own commit, which its entry could
  not know while it was being written, and which `tests/test_changelog_coverage.py` requires. The
  gate was red on `main` when n22 started (`79 checks passed, 1 failed`); it is green again.
- **Fixed (docs, found while writing the undo for this):** `plugin/src/rtxconf.h` promised a
  `rhyremix --reset-rtx` command that exists nowhere in the tree. The comment now says what is real —
  the user layer outlives the run, `tools/rtxprofile.sh restore` does not touch it, and where the
  runtime's own user-layer file lands is a question for a live run (no `user.conf` exists in the game
  tree as of this task).
- **Undo:** `[tonemap] apply=0` in `rhyremix.ini` (one line, no rebuild) stops the plugin writing the
  key, so rtx.conf's static −0.5 is what a fresh install gets. One caveat that belongs to the layer,
  not to the feature: `SetConfigVariable` writes the runtime's **user** layer, which outranks
  `rtx.conf` and outlives the run, so after a run that pushed +0.5 the file's −0.5 does not come back
  by itself — run once with `apply=1` at a noon stop (the plugin re-publishes −0.5) or set it with the
  Remix UI's Global Exposure control, which writes the same layer. `tools/rtxprofile.sh restore`
  restores `rtx.conf`, not this layer. (While `apply=1` the plugin owns the value every frame it is
  running, which is why the persistence is only a problem on the way out.)
  To take the feature out of the tree: delete `plugin/src/exposure.{h,cpp}`,
  `tests/exposure_check.cpp`, `tests/test_exposure_anchor.py`, the `[tonemap]` blocks in
  `config/rhyremix-production.ini` and `config.cpp`'s `kIniDefaults`, the `applyExposureAnchor()` call
  in `sun.cpp`, the `RhySignedRate` block in `smooth.{h,cpp}`, and the `nat-expo` lines in
  `scripts/check-all.sh`.

## [0.8.0] — 2026-11-11 (`e19fde6`)

### Installer and launcher

- **Added:** `scripts/test-session.sh [--tour] [--project DIR]` — one command for a test run when
  the GPU is held by something else: it stops that process by PIDs read out of `/proc` (never
  `pkill -f` a pattern that can match its own command line), waits until `nvidia-smi` reports under
  1 GiB used, optionally arms the photo tour, runs the launcher in the foreground, then kills the
  leftovers by exact process name (`gta-sa.exe`, `NvRemixBridge`, the prefix's `wineserver`) and
  brings the other process back. Tested entirely with stubs (`scripts/test-session-selftest.sh`);
  it never runs the game itself.
- **Added:** `tools/characters.sh {on|off|status} [GAME_DIR] [--pack NAME …]` — the six
  `characters` packs of [deps/manifest.json](deps/manifest.json) installed into `modloader/` in the
  documented order (Proper Characters Retex HD → Proper Player Retex → PedFuncs → Emergency Peds
  Vary → Criminal Peds Vary → Restored Unused Peds), priorities written inside
  `[Profiles.Default.Priority]`, `PedFuncs` pulled in automatically by a pack's `requires`,
  `modloader.ini` backed up pristine once. `off` removes exactly what `on` added.

### Tools

- **Added:** `tools/vegetation.sh {on|off|status} [GAME_DIR] [--step N]` — the "V Vegetation Mix"
  pack plus the ranked fixes from [docs/VEGETATION.md](docs/VEGETATION.md) as switchable steps:
  1 leave the pack's `procobj.ide`/`vegepart.ide` out, 2 streaming caps (MixSets `StreamMemory`,
  ImprovedStreaming ranges 4/5), 3 the `rtx.conf` opacity-micromap budget, 4 the heaviest-`.txd`
  bisect. State and one pristine backup per edited file live in `modloader/.rhys-vegetation/`, and
  an `on`/`off` round-trip leaves the tree byte-identical (51 checks).
- **Docs:** [docs/CHARACTERS.md](docs/CHARACTERS.md) §6 now lists every Remix material override a
  character pack kills — CJ's three Looks `mat_0AB982F62FEF3381` / `mat_1AB00AD812498A17` /
  `mat_C23DD21AE7ED34B1` plus the shared `8645663DE7AFBB81` normal/rough pair, and the two
  pedestrian Looks — with both re-pointing routes. Ryder, Kendl and `mat_DA50E6D8BA27A905` survive.

### Testing and docs

- **Added:** three photo-tour stops in `plugin/lua/SARemix_Tour.lua` for the subjects the pack's own
  material work targets: the rear-view mirror of the car parked at the Grove Street curb, a
  pedestrian close-up in Pershing Square at noon, and the Mulholland / Vinewood hillside vegetation.
- **Gate:** `scripts/check-all.sh` now also runs the vegetation and character-pack switch
  self-tests (45 checks, 0 failed).

## [0.7.0] — 2026-11-11 (`bc91fa4`)

### Tools

- **Added:** `scripts/check-all.sh` — one command that runs every gate in the repo (cross-compiled
  plugin build, the three native test binaries, every `tests/test_*.py`, the installer, vegetation
  and character self-tests, the test-session stub test, and `bash -n` on every shipped shell
  script), one PASS/FAIL line each, ending `ALL GATES PASSED`.
- **Added:** `tools/make-release.sh OUTDIR` + `tools/torrent.py` — builds the bundle we host
  ourselves: collects only the manifest entries whose licence allows re-upload, writes
  `SHA256SUMS`, makes a `.torrent` (`mktorrent` if installed, otherwise a stdlib bencode builder)
  with the trackers in [deps/trackers.txt](deps/trackers.txt), and prints the magnet link **after**
  re-deriving the info hash from the bytes on disk. Nothing is staged yet, so today it correctly
  reports every pack as not staged.
- **Changed:** [deps/manifest.json](deps/manifest.json) entries carry the magnet slot for the
  bundle, with the https mirror slots still `TBD`.

### Testing and docs

- **Changed:** the plugin and the native tests now build with `-Wall -Wextra` permanently, with zero
  warnings; `warncheck()` in `check-all.sh` turns any `warning:` line into a failure even when the
  compiler exited 0. The two warnings that were there were fixed at the source (a `FARPROC`-to-
  prototype cast in `plugin/src/remix.cpp`, an unread third `lampsShouldBeOn()` argument), not
  silenced; `i686-w64-mingw32-g++ -fanalyzer` over all plugin sources is clean too.

## [0.6.0] — 2026-11-11 (`03ea257`)

### Traffic signals

- **Added:** `[trafficlights] source=game` (now the default). The plugin hooks the one instruction
  in `CTrafficLights::DisplayActualLight` that registers a signal head's corona (call site
  `0x49DD4C` → `CCoronas` at `0x6FC580`), so each head's **real position** and the **aspect colour
  the game is actually showing** arrive for free — including a head hanging out over the road on a
  mast-arm signal — instead of being guessed from the pole position in the lamp list. A head the
  game stops registering keeps its light for `head_ttl_ms` (2 s), so one dropped frame is not a dark
  intersection.
- **Changed:** `source=list` keeps the pre-s01 behaviour (one light per pole, colour from the local
  cycle) and is the automatic fallback whenever the patch cannot be installed; the log line says
  which mode is live.
- **Fixed:** two in-game crashes of the first two hook builds — a `jmp` over a `call` does not come
  with the return address the deleted `call` would have pushed, and the recorder's stack
  realignment. Both post-mortems, with disassembly, are in
  [docs/FINDINGS.md](docs/FINDINGS.md).
- **Fixed:** `g_cfgRev` was only updated in one branch, so after any ini edit the config was
  re-read — and a failed hook re-installed and re-logged — 60 times a second.

### Water

- **Added:** `tools/water.sh {calm|realistic|pack|status|revert}` and the three variants
  `config/water/{calm,realistic,pack}.usda`. Measured first: `water_normal.dds` is a
  10240×512 BC5, 20-frame sheet whose octahedral decode gives a **median facet tilt of 46°** (43 %
  of facets over 60°) — that, not the encoding flag, is why the sea read as binary black/white.
  The variants pin `encoding = 0`, slow the sprite-sheet loop, and set Jerlov coastal absorption
  (realistic: σ = 0.150/0.076/0.040 m⁻¹; calm: a thin-walled 3 m sheet).
- **Added:** `tools/water_check.py` validates a variant before it is installed (text USD, balanced
  braces, same prim paths as the original, only inputs the material's `.mdl` declares); `water.sh`
  refuses a variant that fails and never edits `rtx.conf` — it prints the
  `rtx.translucentMaterial.normalIntensity = 0.35` line that actually kills the noise, because that
  knob lives in `rtx.conf` and not in the USD.

### Installer and launcher

- **Added:** `tools/laa.py` — read, set and clear **Large Address Aware** on `gta-sa.exe`.
  `apply.sh` now sets it (the downgrader's exe is not LAA, and that one 32-bit process carries the
  game, the Remix bridge client and the streaming cap at once); `deploy.sh` warns on every launch
  when the flag is missing, and `gta-sa.exe.noLAA-backup` keeps the pre-change exe.
- **Added:** `scripts/installer-selftest.sh` — installs into a fake game tree under `build/tmp/`,
  runs `apply.sh` twice and requires the second run to be a no-op; `apply.sh`/`deploy.sh` refuse to
  run in sandbox mode against anything that is not the marked fake tree, so a bug in the test can
  never point the installer at a real game folder.
- **Added:** `scripts/install-desktop-entry.sh` writes a `.desktop` entry pointing at your paths
  (no sudo); it runs the same launcher, so a desktop click still builds and installs the current
  code and still reports a failure.

### Tools

- **Added:** `tools/mirrors.py` + `config/mirror-hashes.txt` + [docs/MIRRORS.md](docs/MIRRORS.md):
  SA paints its mirrors — no stock car has a mirror atomic — so the fix is a Remix material
  (metallic 1, roughness 0.02, white albedo, no normal map) keyed to the mirror texture's hash,
  sublayered into the pack's `mod.usda` exactly once, with backup and `--remove` restoring
  byte-identical.
- **Added:** `deps/manifest.json` + `tools/fetch-deps.sh` — every mod a user would otherwise
  download by hand, with author, licence and sha256; fetched with `aria2c` (torrent or https,
  resumable) and a plain `curl` fallback, verified before anything reaches the game folder.
  `apply.sh --with-deps [--deps-group characters]` uses it; packs that may not be re-uploaded are
  linked, never mirrored.
- **Added:** `tools/txd_sizes.py` (texture-dictionary measurement, the numbers behind
  [docs/VEGETATION.md](docs/VEGETATION.md)) and `tools/winedbg-hang.sh` (attach to a hung
  Proton/wine process and capture stacks).

### Testing and docs

- **Added:** [docs/VEGETATION.md](docs/VEGETATION.md) — why "V Vegetation Mix" freezes the RTX copy
  and not the plain one: three ranked causes with evidence (the pack's own `procobj.ide` /
  `vegepart.ide` re-point 44 entries at dictionaries this install does not have, and 5 LOD models
  point at a `.txd` that exists nowhere; the 1900 MB streaming cap plus two `KeepLoaded=1`
  vegetation ranges pin ~190 MB in a non-LAA 2 GB process; 11.7× more alpha-tested foliage through
  Remix's opacity-micromap path), the test order, what was ruled out, and how to capture the next
  freeze.
- **Added:** [docs/CHARACTERS.md](docs/CHARACTERS.md) — the peds in the RTX copy are stock 1.0, the
  pack authors PBR for CJ's four materials and two pedestrian materials only, so the fix is
  game-side and texture-first; ranked shortlist, verified download links, the licence reality.
- **Changed:** `README.md` is now a two-step player guide (downgrade to 1.0 US, then one installer
  command) with requirements, troubleshooting, rollback and full credits; all developer material
  moved to [docs/DEVELOPING.md](docs/DEVELOPING.md). `bash scripts/check-readme.sh` gates it: repo
  paths exist, links answer, no personal paths.

## [0.5.1] — 2026-11-11 (`0499882`, `b95796e`, `b1e7808`)

### Tools

- **Fixed:** the photo tour froze the player. Teleporting out of an interior crashed opcode `00A1`,
  and the script died with CJ locked and the HUD hidden. It now waits until CJ is outdoors
  (interior 0), runs each stop in `pcall`, and restores control, HUD, radar and camera however it
  ends.
- **Fixed:** the tour moved CJ by writing his matrix position directly (`CPlaceable::m_matrix` at
  `ped+0x14`, position at `matrix+0x30`) because the ped opcodes throw under MoonLoader 0.27 in this
  build — the same reason `moonloader/lib/sa_ped_safe.lua` exists.
- **Fixed:** the F12 hotkey never fired. `key=0x7B` in `SARemix_Shot.conf` is a Windows virtual-key
  code, but `isPs2KeyboardKeyJustPressed` takes the game's PS2 key numbers, so the value was read in
  the wrong namespace; the script uses `isKeyJustPressed`.
- **Changed:** the water stops moved to the open beach at Santa Maria instead of under the pier, and
  noon and night shots of the sea were added.

## [0.5.0] — 2026-11-11 (`3418647`)

### Lighting

- **Changed:** lamp families and signal lenses now use measured **CIE 1931 chromaticities converted
  to linear sRGB**. High-pressure sodium is `(1, 0.25, 0)` — the deep sodium orange; a line sum on a
  hue approximation had given green > 0.75, i.e. yellow. Signal green is mapped to the gamut edge
  along its own hue so it stays blue-green.
- **Fixed:** `colorTemperatureToRgb()` is Tanner Helland's fit, not Kim et al.'s, and it returns
  **display-encoded** values while Remix radiance is linear — it now decodes. Only the lamps use it;
  the sun's Beer-Lambert path is unchanged.

### Sun and sky

- **Added:** `[sun] dst=1` (default): Los Angeles summer time, +1 h between roughly day 70 and day
  307, so a July sunset lands near 20:05 game time instead of 19:05.

### Installer and launcher

- **Fixed:** a low-VRAM refusal from a desktop launch was silent. `launcher/launch-rtx.sh` now shows
  a dialog (`zenity`, else `notify-send`) naming the process that holds the card, and the screenshot
  watcher starts only **after** the check, so an early exit no longer leaves it running.

### Testing and docs

- **Tests:** lamps 30/30, solar 37/37 — the colour assertions follow the measured values.

## [0.4.1] — 2026-11-11 (`fc862cf`)

### Lighting

- **Added:** `tools/strip_pack_lamp_lights.py` removes the 18 `SphereLight` prims that
  [GTASA-Remix](https://github.com/Hemry81/GTASA-Remix) puts inside its 12 lamp models
  (`SARemix/SARemix_StreetLights.usda`), because our 7002-entry lamp list lights the same poles —
  every street lamp was lit twice and night read twice as bright. Idempotent, keeps
  `<file>.orig-with-lights`, `--restore` puts them back; run by `deploy.sh` on every launch and by
  `apply.sh`. Remix reads USD at start-up, so it takes effect on the next launch.

### Tools

- **Fixed:** every F12 failed silently. `getWorkingDirectory()` in MoonLoader is
  `<game>/moonloader`, so the hotkey was looking for `moonloader/screenshots/pending`.
- **Added:** `plugin/lua/SARemix_Tour.lua` — with `screenshots/tour.req` present it walks the
  [docs/TESTING.md](docs/TESTING.md) shots (fixed camera, set time, clear weather), requests one
  grab per stop and writes `screenshots/tour-manifest.txt`; runs once per flag file.

## [0.4.0] — 2026-11-11 (`88e8cd1`)

The turning point: every lighting knob had been set by eye against a screenshot, and the image kept
coming out wrong. This release replaces the guessed numbers with computed ones — and corrects the
units error in our own docs that had made the biggest bug invisible.

### Lighting

- **Fixed (units):** one GTA SA game unit is about **one metre**, and light positions *and* radii
  handed to `CreateLight` are in game units. The README and the ini both claimed `radius=15` was
  15 cm at `rtx.sceneScale = 0.01`; it is a **thirty metre** emitting sphere for a lunchbox-sized
  luminaire — the shapeless light pools and the volumetric fog wall. Settled two ways: `probe_scale=1`
  lands a probe light on the lamp post while `0.01` does not, and the signal data puts a head 7.6
  units above its road where a real one hangs about 6 m up.
- **Added:** `fixture_size=1` — real fixture geometry (0.2 game units for a cobra-head luminaire,
  0.1 for a 200 mm signal lens) with `rhyRadianceForFlux()` scaling radiance by
  `(radius_ref/radius)²` so total emitted power is unchanged. That decoupling is the only reason the
  radius could be fixed at all: shrinking the sphere without compensating takes the street's light
  with it.
- **Added:** discharge lamps are **line spectra, not blackbodies**. Colours are built from documented
  atomic emission lines: HPS is the 589.0/589.6 nm D doublet (linear sRGB 1.000/0.844/0.000),
  mercury 0.755/1.000/0.602 (the 546.1 nm line dominates), plus metal halide, LED and neon. The old
  `kelt_warm=2100` gave 1.000/0.537/0.055 — a third of its energy in the deep red where sodium has
  almost none, which is why sodium read as a dim incandescent bulb. Tungsten stays on kelvin because
  it is a genuine thermal source.
- **Added:** traffic signal lens colours integrated from real passbands (red 610–645 nm, amber
  585–605, green 515–565) instead of hand-picked triples; sampling a single "red" wavelength such as
  620 nm gives orange because the eye's response there still carries green. A test asserts the
  band-integrated red is redder than that sample. Signals stay **unshaped on purpose**: a real module
  beams down the approach, but the list carries no heading, and a wrongly-aimed cone would darken
  every signal seen from the cross street.

### Sun and sky

- **Added:** `plugin/src/solar.cpp` — an astronomical sun and moon: Spencer's Earth-orbit equation,
  ecliptic-to-equatorial conversion, azimuth by `atan2`, Kasten–Young air mass, Beer-Lambert
  per-channel extinction, Kim et al. colour temperature. Sunrise and sunset for Los Santos match
  USNO to about three minutes, and the red of a low sun falls out of the path length (5° elevation →
  air mass 10.3 → transmittance R 0.158 / G 0.072 / B 0.020) instead of being painted on.
- **Fixed:** three real bugs the model exposed — an azimuth cosine term that was wrong only near
  sunrise/sunset, a moon placed by shifting the clock 12 hours (which puts a midsummer full moon at
  77° instead of 33°), and a Planck fit mixing 0–1 and 0–255 conventions.

### Installer and launcher

- **Added:** `config/rtx.conf` (`--rtx-conf`) changes exactly **two** values, both measured:
  `rtx.emissiveIntensity` 10 → 1.0 (the pack's own emissive materials already reach
  `emissive_intensity = 3000`, so ×10 rendered shopfront windows at 30000) and `rtx.skyBrightness`
  3 → 1.0 (the sky is driven by the solar model now, so the static boost double-counted).
  `config/rtx.conf.experimental` (`--rtx-conf-exp`) keeps the six changes that have never been
  observed running, so a bad image can still be attributed.
- **Added:** a VRAM preflight in `launcher/launch-rtx.sh`: under 6 GiB free it refuses to start and
  names the process holding the card. Four launches and three config rollbacks had been spent on a
  "white window then CPU exception in the bridge" that was a local LLM server holding 15,494 of
  16,376 MiB — every log looked like a mod bug.

### Tools

- **Added:** `scripts/lighting-test.sh` measures mean screen brightness every 10 s, so "the screen
  is white" becomes a number, and always restores `rtx.conf`, the ini and the shipped `.asi` on
  exit — including on Ctrl-C.

### Testing and docs

- **Tests:** 30 lamps / 37 solar, native `g++`, no game needed.

## [0.3.1] — 2026-11-10 (`ef2078c`)

### Installer and launcher

- **Fixed:** a fresh clone had nothing to install. The lamp and signal lists are derived from the
  game's own data, so they stay out of git; `deploy.sh` already generated the traffic list and now
  generates the street list the same way, and the build step no longer rebuilds the `.asi` just
  because a list is absent.

### Tools

- **Changed:** `tools/lamp_list.py` and `tools/traffic_list.py` default `--table` to
  `<game>/moonloader/SARemix_Light_Manager.dat` — that file belongs to the asset pack, so the repo
  reads it in place instead of redistributing it. The gate asserts both facts: a clone that cannot
  install, or that ships someone else's mod file, fails.

## [0.3.0] — 2026-11-10 (`86a5ca9`)

Answers the report "lights blink, some are missing, and they stop being visible far too soon". It
also settles the open question from the previous phase: a light handed game-unit coordinates **is**
visible, so `rtx.sceneScale = 0.01` does not apply to light positions.

### Lighting

- **Fixed:** four causes of the blinking, all ours.
  1. `max_total_lights=64` against 163 requested handles: first-come-first-served starvation, in an
     order that changes with the player. The cap is off by default.
  2. A refused slot set the same disabled flag as a bridge rejection, so one bad frame switched the
     street lights off for the session. `rhyCreateLight` now reports *why* (1 = server, 2 = our cap)
     and only reason 1 disables.
  3. A retired light's destroy could land **after** its replacement's create: the pending map was
     keyed by a handle hash that is identical for a recreated lamp. `rhyFlushDelayedTag(tag)` now
     runs before a module creates, and tags are namespaced per module.
  4. One selection radius: a lamp on the boundary was created at one re-selection and destroyed at
     the next. Now enter at `max_distance`, leave at `exit_distance`, with `sticky_extra` slots
     reserved for lamps already lit.
- **Fixed (range):** "not visible far enough" is a **count** problem, not a radius problem —
  selection is nearest-first, so a `max_lights` smaller than the lamps inside `exit_distance` evicts
  lamps that are already lit. Measured around Grove Street: 222 lamps within 300 units, ~350 within
  380, 541 within 500, so the test profile pairs 300/380 with `max_lights=320` + `sticky_extra=90`.
  The old 60 cut the world dark at ~140 units.
- **Added:** the game-clock read (`*(uint8_t*)0xB70153` hours, `0xB70152` minutes) that the lighting
  modules key off, and the sun/moon distant lights, probe calibration rig and the p02–p07 work the
  fixes above depend on.

### Traffic signals

- **Fixed:** the phase key changed every 10 s and rebuilt every light in range, so signals churned.
  `phase_near` keeps a phase change from moving lights.

### Tools

- **Added:** `tools/lamp_list.py` and `tools/traffic_list.py` generate both lists from the game's own
  `gta3.img`, `.ide`, `.ipl` and the pack's lamp table. Model ids resolve through the `.ide` files,
  not the `gta3.img` entry index (the four "text-IPL lamps" the old guess produced sat at positions
  no lamp post occupies, and are gone); dedupe keys on position alone — a pole is instanced by more
  than one model id — which took 7371 lines to 7002 real lamps, i.e. 360 light slots were being
  burned by duplicates; `--report-missing` counts the lamp table's gaps instead of hiding them;
  signal heads use the table's exact offsets (7.6 / 6.0 / 7.65) instead of `radius*0.09`, which put
  the glow ~2 units below the head.
- **Added:** the install and diagnosis tooling still in use today: `deploy.sh`, `scripts/shot.sh`,
  `scripts/shot-watch.sh`, `scripts/diagnose.sh`, `scripts/abtest-lights.sh`,
  `scripts/tidy-game.sh`, `plugin/lua/SARemix_Shot.lua`, and the crash-free capture path (the ASI
  swallows `SetGammaRamp`, `block_gamma=1`, because the game's own `Ctrl+PrintScreen` took the Remix
  server down with CPU crash `0xe06d7363`).
- **Fixed:** the 70-argument config log line had drifted by one argument (19 `-Wformat` warnings,
  log values printed misaligned); it is now three lines.

### Testing and docs

- **Added:** [docs/MODS-REPLACED.md](docs/MODS-REPLACED.md) — which mod does what, which of them the
  plugin replaces, and the `OLD/` tidy-up inventory.
- **Tests:** 25 unit tests (`.ide` parsing, position dedup, head-height matching, `.ipl` parsing);
  the phase gate reports 112 checks.

## [0.2.0] — 2026-11-09/10 (`1cbde45`, `e170ed2`, `09cdfc0`)

### Lighting

- **Added:** `rhyremix.asi` — the native 32-bit plugin skeleton: a `Present` hook on the game's
  D3D9 device (`*(IDirect3DDevice9**)0xC97C28`, which in the RTX build belongs to the Remix bridge
  client `d3d9.dll`), the Remix C API reached through that bridge's exported
  `remixapi_InitializeLibrary`, a read of the player ped (`*(void**)0xB7CD98`) so the test light can
  follow CJ, and one test light.
- **Fixed:** the logger — `vsnprintf` for a `va_list` (the previous version formatted garbage), and
  the log now lands next to the `.asi` as `scripts/rhyremix.log`.
- **Changed:** the test light is brighter and follows CJ: at `rtx.sceneScale = 0.01` a 0.5-unit
  light is effectively invisible, which is how it was first noticed.

## [0.1.0] — 2026-11-09 (`4d90488`, `9f25f27`)

The starting point: the fixes that make [GTASA-Remix](https://github.com/Hemry81/GTASA-Remix) and
RTX Remix 1.6.1 run at all under Proton.

### Installer and launcher

- **Added:** `launcher/launch-rtx.sh` — Proton run **inside** `SteamLinuxRuntime_4` (outside it the
  bridge server never starts and there is no audio), `PROTON_ENABLE_NVAPI=1` for DLSS/NGX,
  `PROTON_PREFER_SDL=1 PROTON_ENABLE_HIDRAW=0` for the Wine `xinput` deadlock that froze the game on
  start with a DualShock over hidraw, and the MangoHud-wine config.
- **Added:** `apply.sh` + `patches/gtasa-remix-linux.patch` — the Linux patch set for the pack, with
  a backup folder filled once so a re-run cannot overwrite the pristine pre-patch copies.
- **Fixed:** `rtx.useNewGuiInputMethod = False` — without it the Remix menu does not respond under
  Wine and the window goes white after the first frame.
- **Fixed:** MoonLoader scripts dying at start (`getCharCoordinates` / `isCharInAnyCar` throw under
  MoonLoader 0.27, and scripts started before CJ existed) — `moonloader/lib/sa_ped_safe.lua` reads
  memory instead, and the scripts wait for the player.
- **Fixed:** street lights that never appeared (the stale `SARemiv_LightMan.lua` used model ids from
  before five cloud models were added to `remix.ide`, a +5 shift) and a flicker loop that re-triggered
  the warm-up flicker forever.
- **Fixed:** `VK_ERROR_DEVICE_LOST` when lights spawned — ~1000 lights declared
  `inputs:shaping:focus` / `focusTint` with no value (NaN in 1.6), now defaulted by
  `tools/usd_light_defaults.py`.
- **Fixed:** the crash "Light prims anchored to a mesh replacement must also include actual meshes" —
  the pack's USD references pointed at the author's `D:/Games/...`, now relative paths.
- **Fixed:** the pack's vehicle toolkit could not load: it needs a `SARemix_DebugPrinter` module that
  the pack does not ship (stub added), and the old `SARemiv_VTK.lua` used wrong ids — use
  `SARemix_VTK.lua`.
- **Fixed:** a gray square under CJ (MixSets' real-time ped shadow render target) and dim lamp heads
  (emissive plate scaled to 0).

### Tools

- **Added:** `tools/img_unpack.py`, `tools/lua_check.py`, `tools/vtidx.py`,
  `tools/usd_light_defaults.py`, and the `SARemix_DebugPrinter` stub module the pack's vehicle
  toolkit needs.

### Testing and docs

- **Added:** [docs/FINDINGS.md](docs/FINDINGS.md) (logs, measurements, dead ends) and
  [docs/notes/linux-modding-notes.md](docs/notes/linux-modding-notes.md) — the 1.0 US downgrade,
  Proton issues, the Proper Shaders deadlock, 60 FPS, controllers, RTX on Linux generally.
