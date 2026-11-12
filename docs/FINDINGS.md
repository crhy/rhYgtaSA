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
