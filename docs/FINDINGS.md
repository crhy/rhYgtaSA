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
directional - visor plus refractor throw a narrow beam down the approach - but
`rhyremix_traffic.txt` carries only `x y z headz`, no heading. A cone aimed the
wrong way would put a dark face on every signal seen from the cross street, which
is worse than an unshaped light. The honest fix is a heading column derived from
the IPL placement rotation.
