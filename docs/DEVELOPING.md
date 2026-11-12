# Developing rhYgtaSA

Everything in here is the developer half of what used to sit in `README.md`. The README is now the
two-step user guide; this file is what you read **after** the game runs, when you are changing code,
lighting numbers or the install itself.

Related: [FINDINGS.md](FINDINGS.md) (logs, dead ends, measurements),
[TESTING.md](TESTING.md) (what to photograph and which knob to turn),
[MODS-REPLACED.md](MODS-REPLACED.md) (what is installed, what replaces what, `rtx.conf` knobs),
[VEGETATION.md](VEGETATION.md) (streaming memory / Large Address Aware),
[CHARACTERS.md](CHARACTERS.md) (ped mods, download list, licence reality),
[MIRRORS.md](MIRRORS.md) (rear-view mirror materials),
[notes/linux-modding-notes.md](notes/linux-modding-notes.md) (Proton, downgrade, controllers, 60 FPS).

## Build

```
cmake -S plugin -B build -DCMAKE_TOOLCHAIN_FILE=plugin/toolchain-mingw32.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build            # or: bash plugin/build.sh
```

Needs `cmake` and MinGW-w64 for i686 (`i686-w64-mingw32-gcc`, `-g++`, `-windres`). Output is
`build/rhyremix.asi`, a 32-bit PE DLL; `scripts/check.sh p01` fails the build if it links the MinGW
runtime DLLs or imports `d3d9.dll` statically (the Remix API must be reached through
`GetModuleHandle`/`GetProcAddress` on the bridge client).

Install the build into a game folder with `./deploy.sh`:

```
./deploy.sh --build --prod "<game>"     # shipped lighting profile; default when no ini exists yet
./deploy.sh --build --test "<game>"     # bisect ladder: probe + moon on, street/traffic off
./deploy.sh --force-ini ...             # overwrite the live ini (your A/B knobs) - think first
./deploy.sh --lua-off|--lua-on          # the Lua light manager (auto: follows [streetlights] enabled)
./deploy.sh --sun-off|--sun-on          # the Lua sun mod    (auto: follows [sun] enabled)
./deploy.sh --shotkey-off|--shotkey-on  # moonloader/SARemix_Shot.lua (F12 marker)
./deploy.sh --aftermath                 # devk.enableAftermath = True in <game>/dvk.conf
./deploy.sh --rtx-probe                 # report which rtx.* keys this Remix build actually accepts
./deploy.sh --rtx-conf|--rtx-conf-exp   # install config/rtx.conf(.experimental)
./deploy.sh --no-launcher               # used by launch-rtx.sh itself
```

`deploy.sh` also generates the two data files on a fresh clone — `build/rhyremix_lights.txt`
(7002 lamps) and `build/rhyremix_traffic.txt` (1522 signals) — from the game's own `gta3.img` +
`.ide` + `.ipl` plus the pack's lamp table under `<game>/moonloader/`. They are generated, not in
git. It copies asi + ini + lists into `<game>/scripts/`, backs up the outgoing asi under
`build/tmp/`, refuses an asi that is not `PE32 ... i386`, and prints the md5 of the build and of the
deployed file: they must match before you trust a screenshot.

The gate is `bash scripts/check.sh <phase>`; it must end with "All checks passed."
The installer's own end-to-end test (fake install, no game, no GPU, no writes outside
`build/tmp/`) is `bash scripts/installer-selftest.sh`.
The README gate for this file's sibling user guide is `bash scripts/check-readme.sh`.

Before calling anything done, run the whole sweep in one command:

```
bash scripts/check-all.sh          # ends "ALL GATES PASSED", or lists the failures
```

It runs `plugin/build.sh`, the three native test binaries (`tests/lamps_check.cpp` with
`lamps.cpp`+`solar.cpp`, `tests/solar_check.cpp` alone because it `#include`s `solar.cpp`,
`tests/signals_check.cpp` with `signalheads.cpp`+`lamps.cpp`+`solar.cpp`, all host `g++ -std=c++17
-Wall -Wextra -Iplugin/src` into `build/tmp/`), every `tests/test_*.py`, `scripts/installer-selftest.sh` and
`bash -n` on every `*.sh` in the tree except `build/`. One PASS/FAIL line per check, logs under
`build/tmp/check-all/`. It is not a replacement for `check.sh <phase>`, which asserts the
per-phase invariants (imports, ini keys, which files are compiled in); check-all.sh is the
"nothing regressed anywhere" pass, and it never touches the game folder, wine or the GPU.

The C++ is warning-clean and stays that way (s13): `plugin/CMakeLists.txt` carries
`-Wall -Wextra` permanently, the three native test compiles pass the same flags, and
`check-all.sh`'s `warncheck()` turns a `warning:` line in a compile log into a FAIL even when
the compiler exited 0. No `-Werror` (a new compiler release must not turn the build red on a
false positive) and no blanket `#pragma`/cast silencers: the two warnings that were there -
a `FARPROC` to Remix prototype cast in `remix.cpp`, and a third `lampsShouldBeOn()` argument
that was never read - were fixed at the source. `i686-w64-mingw32-g++ -fanalyzer` over all 15
plugin sources is clean too; it is the stand-in for cppcheck, which is not installed here.

## What it fixes
| Problem on Linux / Remix 1.6.1 | Cause | Fix |
|---|---|---|
| Game freezes on start, controller dead | Wine `xinput` lock deadlock (GInput + Remix bridge input hooks, DualShock over hidraw) | `PROTON_PREFER_SDL=1 PROTON_ENABLE_HIDRAW=0` |
| No audio / bridge server never starts | Proton run outside the Steam runtime | launcher runs Proton inside `SteamLinuxRuntime_4` |
| DLSS missing | NGX needs NVAPI | `PROTON_ENABLE_NVAPI=1`; DLSS frame generation off (hangs the bridge under Proton) |
| White window after first frame | Remix's new GUI input method under Wine | `rtx.useNewGuiInputMethod = False` |
| Sun, street lights, vehicle materials dead (MoonLoader scripts die) | `getCharCoordinates`/`isCharInAnyCar` opcodes throw; scripts start before CJ exists | `moonloader/lib/sa_ped_safe.lua` (memory reads) + wait for the player |
| World too bright with the plugin on | the Lua sun mod and the Lua light manager stay loaded next to our API sun/lamps, so the scene is lit twice (`rtx.autoExposure.enabled = False` hides nothing) | `deploy.sh` disables each Lua mod exactly when the ini enables our replacement |
| Street lights never appear | Stale `SARemiv_LightMan.lua` uses model ids from before 5 cloud models were added to `remix.ide` (+5 shift) | ids fixed |
| Street lights flicker forever | random pause timer re-triggers the warm-up flicker | lights stay on after warm-up |
| GPU crash (`VK_ERROR_DEVICE_LOST`) when lights spawn | ~1000 lights declare `inputs:shaping:focus`/`focusTint` without a value (NaN in 1.6) | `tools/usd_light_defaults.py` gives defaults |
| Crash: "Light prims anchored to a mesh replacement must also include actual meshes" | USD references point at the author's `D:/Games/...` | relative paths |
| World washed out, yet lamps cast no pool of light | our `LightInstance` radius/radiance were the wrong order of magnitude | radius/radiance calibrated with a probe light; see the units correction below |
| Lamp light pools read as shapeless haze; volumetrics give a fog wall | `radius=15` game units is a **30 metre** emitting sphere for a lunchbox-sized housing, and our own docs said it was 15 cm | `fixture_size=1` uses real luminaire sizes with radiance rescaled by `(radius_ref/radius)^2` to hold total flux (`plugin/src/lamps.cpp`) |
| Sodium street lamps look like dim incandescent bulbs | they were given a colour temperature (`kelt_warm=2100`). A gas discharge has no useful colour temperature: HPS is the 589 nm sodium D doublet, and a 2000 K Planck curve puts a third of its energy in deep red where sodium has almost none | `lamp_warm=hps` / `lamp_cold=mercury` build the colour from documented atomic emission lines |
| Shopfront windows blown to white | the asset pack's own emissive materials reach `emissive_intensity = 3000`, and `rtx.emissiveIntensity = 10` multiplied that to 30000 | `rtx.emissiveIntensity = 1.0` |
| Traffic signal reds look orange | the lens colours were hand-picked triples; a single "red" wavelength like 620 nm still carries a lot of green | colours integrated from real lens passbands (red 610-645 nm, amber 585-605, green 515-565) |
| Game launches to a white window that never initialises, logs look like a mod bug | VRAM already held by another process (a local LLM server pinned 15.5 of 16.4 GiB) | `launch-rtx.sh` preflights free VRAM and names the process holding it |
| `Ctrl+PrintScreen` kills the Remix server (CPU crash `0xe06d7363`) | the game's `SetGammaRamp` is the last bridge command queued before the server dies | ASI swallows `SetGammaRamp` (vtable entry 21), `block_gamma=1` |
| Vehicle toolkit can't load | missing `SARemix_DebugPrinter` module; old `SARemiv_VTK.lua` uses wrong ids | stub module; use `SARemix_VTK.lua` |
| Gray square under CJ | game's real-time ped shadow render target | MixSets `UseHighPedShadows = 0` |
| Dim lamp heads | emissive material scaled to 0 | brighter emissive plate under the lamp housing |

## Lights: brightness, casting, screenshots (p04)

**Units correction, read this before touching a radius.** One GTA SA game unit is
about **one metre**, and light positions and radii handed to `CreateLight` are in
game units. The earlier claim in the README - "1 game unit = 1 cm in the Remix
world, so `radius=15` is 15 cm" - was wrong and cost a lot: it made a 30 metre
emitting sphere look like a correctly-sized lamp head, so nobody thought to fix
it. Two independent measurements settle it:

- `probe_scale=1` puts a probe light on a lamp post; `probe_scale=0.01` does not.
  `rtx.sceneScale = 0.01` applies to the converted scene, not to `LightInstance`
  fields (see [FINDINGS.md](FINDINGS.md)).
- The traffic signal data puts a signal head 7.6 units above its road plane, and
  a real signal head hangs about 6 m up.

The mod's own `Light_Manager.lua` uses `radius = 15` for a lamp whose head sits
~20 game units up, and `radiance = 1` for a traffic light. Those are *game-unit*
numbers, i.e. a 15 metre ball.

The earlier plugin build sent `radius = 1.5` and `radiance = 400`.  A 1.5-unit
sphere is 1.5 m, so it cannot cast a pool of light on a road ("lights don't cast"),
while a radiance of 400 at that radius floods everything nearby ("world too bright").
Both were wrong in the same direction: radius divided by ~10, radiance multiplied
by ~100.  Defaults were `radius = 15` (street) / `6` (traffic), matching the mod's own `Light_Manager.lua`, and radiance
`3 / 1.8 / 1` by lamp size band.

**Those radii are now superseded** by `fixture_size=1`, which uses the real
fixture size (0.2 game units for a cobra-head luminaire, 0.1 for a 200 mm signal
lens) and multiplies radiance by `(radius_ref/radius)^2` so the lamp puts out the
same total light. That decoupling is the only reason the radius could be fixed at
all: shrinking the sphere without compensating takes the street's light with it.

### Physical light sources

`plugin/src/lamps.cpp` holds the physics, and `tests/lamps_check.cpp` (30 checks,
plain `g++`, no game needed) pins it down:

- **Discharge lamps are line spectra, not blackbodies.** Colours are built from
  documented atomic emission lines: HPS 1.000/0.844/0.000, mercury
  0.755/1.000/0.602, metal halide 0.732/1.000/0.793, LED 0.872/1.000/0.762.
  Tungsten is the one family where a colour temperature *is* the right model.
- **Traffic signal lenses are filters.** Their colour comes from integrating the
  glass passband, which is what makes a red lens red instead of orange.
- **Size and output are separate numbers.** `rhyRadianceForFlux()` keeps total
  emitted power constant when the fixture is resized.

See [TESTING.md](TESTING.md) for what to look at and which knob to turn.

To calibrate instead of guessing, set `probe=1` in `rhyremix.ini`: one white light
is drawn every frame at `probe_x/y/z`, ignoring hour and distance, and `rhyremix.log`
prints the exact radiance/radius being sent.  Dial `probe_radiance` by factors of
10 (invisible -> up, blown out -> down) and `probe_radius` until it looks right, then
copy those numbers into `radius` / `radiance_*` and set `probe=0`.

Light handles retired by distance or by a traffic phase change are destroyed at a
frame boundary (`plugin/src/delayed.cpp`), never in the middle of a frame that is
still drawing other lights — and a module flushes its own pending destroys BEFORE
it creates, because a recreated lamp has the same handle hash as the one it replaces
(`hash = pos.x*3 + pos.y*7 + pos.z*11 + 1000003*model`), so a destroy that lands
after the create kills the new light. Tags are namespaced per module (`kTagStreet`,
`kTagTraffic`, …) so flushing one module can never destroy another module's light.

### Why the street lights blinked (p08)

Four separate causes, all ours, all visible in the numbers:

1. **The global cap starved the lamps.** 100 street + 60 traffic + probe + sun +
   moon = 163 requested handles against `max_total_lights = 64`. The cap is
   first-come-first-served, so which lamps exist depends on request order, and a
   lamp that loses its slot one frame and wins it back the next is a blink. The
   cap is now off by default (`max_total_lights = 0`).
2. **A refused slot switched the module off for the session.** A cap refusal is a
   per-frame condition; only a light the Remix server itself rejects disables a
   module now (`rhyCreateLight` reports the reason).
3. **Selection had one radius.** A lamp at 100.4 units with `max_distance = 100`
   is in or out on the player's sub-unit movement, and every trip is a destroy
   plus a create. There are now two radii — enter at `max_distance`, leave at
   `exit_distance` — plus `sticky_extra` slots reserved for lamps already lit, so
   the count cap cannot evict a lamp that is already on screen. Traffic lights
   churned for a second reason: the phase key changed every 10 s and rebuilt every
   light in range; `phase_near` keeps the phase from moving lights at all.
4. **The per-module count has to cover the radius, or the cap blinks too.**
   Selection is nearest-first, so a `max_lights` smaller than the number of lamps
   inside `exit_distance` evicts lamps that are already lit, and they come back
   on the next pass — the same blink by another route. Measured around Grove
   Street (2495,-1680): 222 lamps within 300 units, ~350 within 380, 541 within
   500, so the test profile pairs `max_distance=300 / exit_distance=380` with
   `max_lights=320 + sticky_extra=90`. The old 60 was cutting the world dark at
   ~140 units, which is the "lights do not reach far enough" half of the report.

The lamp list itself was wasting slots: 7371 lines for 7002 lamps, because the
same physical lamp is instanced by more than one model id (a pole and its LOD) and
some table entries carry several offsets that land on one point. `tools/lamp_list.py`
dedups on position now, resolves model names through the `.ide` files instead of
guessing an id from the `gta3.img` index, and `--report-missing` lists the lamp-named
models the lamp table has no entry for instead of leaving them silently dark.

Read the log like this: `remix: live=N create_fail=N draw_fail=N budget_refused=N`
and `streetlights: N active, M drawn this frame`. `budget_refused > 0` means the
request exceeds what you allowed; `active > 0, drawn = 0` means the lights exist in
the plugin and nowhere else. `scripts/diagnose.sh "<game>"` reads both logs and the
deployed ini and prints that verdict without changing anything.

`Ctrl+PrintScreen` crashed the **Remix server** (`type=CPU`, id `0xe06d7363`), not
the GPU.  The last bridge commands before the server dies are
`IDirect3DSwapChain9_Present`, `IDirect3DDevice9_SetGammaRamp`,
`IDirect3DDevice9_CheckDeviceFormat`, and `SetGammaRamp` is the only one the ASI
makes (vtable entry 21, verified against the MinGW-w64 `d3d9` header; `moonloader.dll`
is the caller).  `block_gamma=1` patches that entry to a no-op returning `S_OK`; the
gamma ramp is cosmetic under Remix anyway (`rtx.tonemap.finalizeWithACES = True`).
Whether the bridge's screenshot path is simply buggy is still open -- `block_gamma=0`
plus `scripts/abtest-lights.sh gamma off` gives the A/B in one run each way.

Toggle the switches without rebuilding:

```
bash scripts/abtest-lights.sh lights off   # no lights at all: is a symptom ours?
bash scripts/abtest-lights.sh lights on    # lights back on
bash scripts/abtest-lights.sh gamma off    # forward SetGammaRamp again
bash scripts/abtest-lights.sh sun off      # our distant sun/moon off, Lua sun back on
bash scripts/abtest-lights.sh status       # show what is currently set
```

## Sun and moon (p05)

`SARemix_Sun.lua` (script_name `SARemix_Real_Sun`) is a Lua mod: it moves the
sun/moon disc objects (15890/15891) and keeps its own settings in
`SARemix_Real_Sun.dat`. It does **not** create a Remix light, and `rtx.conf`
sets `rtx.ignoreGameDirectionalLights = True`, so the sky bodies light nothing
while the asset pack's emissives and our lamps do. Running it next to our plugin
doubles the daylight: `deploy.sh` renames it to `.dat` whenever the ini has
`[sun] enabled=1`.

`plugin/src/sun.cpp` replaces it with one Remix **distant** light per body:

* direction comes from the game's own sun angle, so the shadow direction is
  unchanged; the moon uses the negated direction, and its altitude is `-md[2]`
  (getting that sign wrong would light the world by day instead of by night)
* radiance fades with elevation (`fade_pow`, `min_elevation`), so sunrise and
  sunset do not blow out the sky
* the handle is only rebuilt when the direction moves more than `step_deg`
  (default 2 degrees) - recreating a distant light every frame would churn the
  Remix server
* `[moon]` mirrors `[sun]` and is independent, so the moon can be off while the
  sun is on

[MODS-REPLACED.md](MODS-REPLACED.md) lists every mod in the install, what replaces it, and the
`rtx.conf` settings that decide brightness and whether a light casts.

## Open problems (ours, measured)

- Street lights come from a static lamp list (`build/rhyremix_lights.txt`, 7002 lamps
  extracted from the game's own data), not from streaming, so a dark lamp post is a
  data gap in that list or in the lamp table — see `tools/lamp_list.py --report-missing`.
- Traffic signals cast red/amber/green Remix lights from 1522 positions extracted from
  `gta3.img`, but the colour follows a simulated per-light cycle, not the state the game
  is actually showing — see `../.strata/notes-p03-trafficlights.md`. Controlled by
  `[trafficlights]` in `rhyremix.ini`.
- **Some lamp posts are still dark on purpose.** `python3 tools/lamp_list.py --report-missing`
  lists the lamp-named models the lamp table (the mod pack's
  `<game>/moonloader/SARemix_Light_Manager.dat`, read in place, not copied into this repo) has no entry for:
  `vegasairportlight`, `sf_rooflite`, `vgsstriptlights1`, `a51_light1_` and a dozen one-off
  SFO / Las Vegas props are not street lamps and are left alone; `lamppost1` / `lamppost2`
  (100 instances each in `LEVEL_0.ipl`) are not in the table either, so if a lamp post near
  those instances reads as missing, that is the reason and the table needs an entry for it.
- **Correct radius/radiance for Remix lights is not yet measured**, only bounded
  (game units, radiance ~1-3). Use the probe light to pin it down before tuning
  anything else. The test profile ships with `probe=1`: one white calibration light
  at `probe_x/y/z`. If it is visible and casts, the API path works and the rest is
  tuning; turn it off with `scripts/abtest-lights.sh probe off` once calibrated.
- **If the world is still blown out with our lights dimmed**, the source is the
  asset pack, not our plugin: `rtx.emissiveIntensity = 10` and
  `rtx.localtonemap.shadows = 2.5` in `rtx.conf` (see [MODS-REPLACED.md](MODS-REPLACED.md)).
  Confirm with `scripts/abtest-lights.sh lights off` - if the blowout stays, our
  lights are not the cause.
- **The screenshot crash is mitigated, not root-caused.** `block_gamma=1` removes the
  ASI's own contribution to the bridge command sequence that ends in
  `0xe06d7363`; if it still crashes with `block_gamma=1`, the bug is in the bridge.
  The game's own `Ctrl+PrintScreen` goes through GDI + D3D9 (`CheckDeviceFormat`,
  `CreateDeviceResources`) and the bridge's server dies inside that sequence, so
  for judging brightness and casting use `scripts/shot.sh` instead: it grabs the
  composited X11 window (post-tonemap, which is the image you are evaluating) and
  never enters the game. `scripts/shot.sh --watch 5` records the light state over
  time; images land in `screenshots/`.
- Headlight beam tilt under Remix 1.6 is being tuned.
- 25 textures referenced by GTASA-Remix are missing from its repository.

## Repo notes that bite

- `screenshots/` is git-ignored (and `scripts/check.sh` asserts that it stays ignored). The screen
  embedded at the top of `README.md` is a local capture: publish it (or drop the line) before
  pointing anyone at the README from GitHub.
- `data/SARemix_Light_Manager.dat` is a file from the GTASA-Remix pack and stays out of git; the
  tools read it straight out of `<game>/moonloader/`.
- Generated, never committed: `build/rhyremix_lights.txt`, `build/rhyremix_traffic.txt`,
  `vendor/`, `build/`.
- `scripts/check.sh` is the gate and is not to be edited. `bash scripts/check-readme.sh` is the
  s11 gate for the user guide: every repo path mentioned in README exists, every link answers
  200/30x, no personal paths.
