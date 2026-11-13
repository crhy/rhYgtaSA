# Developing rhYgtaSA

Everything in here is the developer half of what used to sit in `README.md`. The README is now the
two-step user guide; this file is what you read **after** the game runs, when you are changing code,
lighting numbers or the install itself.

Related: [FINDINGS.md](FINDINGS.md) (logs, dead ends, measurements),
[STATUS.md](STATUS.md) (every reported finding: done / partly / measured / open, and who owns it),
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

It runs `plugin/build.sh`, the native test binaries (`tests/lamps_check.cpp` with
`lamps.cpp`+`solar.cpp`, `tests/solar_check.cpp` alone because it `#include`s `solar.cpp`,
`tests/signals_check.cpp`, `tests/smooth_check.cpp`, `tests/always_on_check.cpp`,
`tests/tod_check.cpp`, `tests/handle_check.cpp`, `tests/vehicle_lights_check.cpp`,
`tests/shutdown_check.cpp`, all host `g++ -std=c++17 -Wall -Wextra -Iplugin/src` into
`build/tmp/`), every `tests/test_*.py`, `scripts/installer-selftest.sh`,
`scripts/vegetation-selftest.sh`, `scripts/characters-selftest.sh`, `scripts/test-session-selftest.sh`,
`scripts/launch-cleanup-selftest.sh`, `scripts/rtxprofile-selftest.sh`,
`scripts/water-default-selftest.sh` (n13: the water default through `apply.sh` and a `RHYS_DRY_RUN=1`
launcher run, against a fake install) and `bash -n` on every `*.sh` in the tree except `build/`. One
PASS/FAIL line per check, logs under `build/tmp/check-all/`. It is not a replacement for
`check.sh <phase>`, which asserts the per-phase invariants (imports, ini keys, which files are
compiled in); check-all.sh is the "nothing regressed anywhere" pass, and it never touches the
game folder, wine or the GPU.

Since s15 there is one command for the part that cannot be tested here - the session where the
assistant's own LLM server has to leave the GPU so the game can run:

```
bash scripts/test-session.sh --tour        # Strata off, wait for the card, launcher, clean up, Strata on
```

It stops the Strata server/engine by PID read out of `/proc/*/cmdline` (never `pkill -f`: the
word "strata" is in the pi window's own command line, and the needles are matched against whole
argv entries because the text of a task can end up in a running process's command line), waits
until `nvidia-smi` really reports under 1 GiB used, runs `launcher/launch-rtx.sh` in the
foreground, kills the leftovers a crashed run leaves (`gta-sa.exe`, `NvRemixBridge.exe`, the
prefix's `wineserver`) and restarts the starter for the same `--project`. Its test is
`bash scripts/test-session-selftest.sh`: fake server/engine/watchdog processes, fake leftovers,
a fake `nvidia-smi` and a starter that only logs its argv, all under `build/tmp/s15-stubs/`, run
through the script's own `RHYS_TEST_SESSION_SANDBOX` guard so a bug in the matching cannot
signal a real process. It never starts the game and never stops the Strata serving this session.

Since s16 the vegetation retry is one switchable command instead of a page of shell (the causes
and their ranking live in `docs/VEGETATION.md`):

```
tools/vegetation.sh on     /path/to/GTASA-RTX --step 3   # install the pack + causes 1..3
tools/vegetation.sh status /path/to/GTASA-RTX            # which step is live, and what the inis say now
tools/vegetation.sh off    /path/to/GTASA-RTX            # back to the pre-`on` bytes
```

The steps are that doc's ranking: 1 = the pack's `procobj.ide`/`vegepart.ide` left out of the
install, 2 = the streaming caps in the two `MixSets.ini` and two `ImprovedStreaming.ini` copies
(`StreamMemory`/`StreamMemoryForced` 1024, `RemoveUnusedWhenPercent` 70, `[Range4]`/`[Range5]`
`Enabled = 0`), 3 = the `rtx.opacityMicromap` budget in `rtx.conf`, 4 = also drop the two
heaviest `.txd` (the bisect - it costs textures, so it is opt-in; the default is 3). Everything
it copies or edits is recorded in `<game>/modloader/.rhys-vegetation/` - the list of files it
added plus one pristine backup per edited file - so `off` restores the tree byte-for-byte and
leaves no state behind, and a pack folder the user installed by hand is refused rather than
deleted. Its test is `bash scripts/vegetation-selftest.sh`: the installer self-test's fake tree
under `build/tmp/`, `status -> off -> on --step 3 -> on --step 4 -> on --step 2 -> off`, a
synthetic pack plus the real staged 192 MB pack when it is present (`--no-real-pack` skips it),
and `sha256sum` of every file in the tree before and after the round-trip. It never writes
outside `build/tmp/`, and `RHYREMIX_INSTALLER_SANDBOX=1` + `.fake-install-marker` guard it the
same way as `apply.sh`/`deploy.sh`.

Since s17 the character packs are one switchable command too (the shortlist, the order and the
priorities live in `docs/CHARACTERS.md` §5/§8, the machine-readable copy in `deps/manifest.json`
group `characters`):

```
tools/characters.sh on     /path/to/GTASA-RTX            # the staged packs, in the documented order
tools/characters.sh status /path/to/GTASA-RTX            # what is staged/installed, priorities, lost Remix materials
tools/characters.sh off    /path/to/GTASA-RTX            # back to the pre-`on` bytes
```

`--pack NAME` narrows the set (manifest id, modloader folder name or a path) and `--stage DIR`
says where packs are staged; a pack that `requires` another one (the Peds Vary packs need
PedFuncs) pulls it in by itself. Priorities are written **inside** `[Profiles.Default.Priority]`
of `modloader.ini` — the writer is section-aware and CRLF-aware, because ModLoader's own ini is
CRLF with one hand-edited LF line in it — and the state dir `modloader/.rhys-characters/` holds
the pack list, the copied files, one pristine backup per edited file and anything moved out
(`--drop-ped-spec` moves `modloader/Ped Spec/` rather than deleting it). `on` refuses to install
half a set (nothing is staged yet: `GTAmods/x/characters/stage` does not exist) and refuses to
merge into a pack folder it did not create; `off` refuses to delete one either. Its test is
`bash scripts/characters-selftest.sh`: the same fake tree, a synthetic six-pack stage dir, the
documented order, the priority lines landing in the right section, subset/dependency/missing-stage
behaviour, the guards, and `sha256sum` of every file before and after — the on/off round-trip has
to hash back byte-identical.

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
| Sea renders as flat black/white speckle, and the water never animates | the pack's normal sheet has a 44° median facet tilt, and the runtime's water path is gated on `animatedWaterEnable && surface.isAnimatedWater` — no installer ever wrote those keys | the `remix-ref` variant (regenerated 6° sheet) is installed by `apply.sh` and by every launcher start, and `tools/rtx_water_keys.py` writes its `rtx.conf` block in place with a backup (n13); `tools/water.sh status` says what is live |
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
   on the next pass — the same blink by another route. Measured over the whole
   7002-lamp list rather than around one street: the worst point on the map has
   **408 lamps within 300 units** of a point and **552 within 380** (median 149 /
   234), so the old `max_lights=320 + sticky_extra=90` did not cover it and the
   2026-11-12 10:22 log shows the active set jumping +193/−193 and +136/+221/−357
   while the player walked. n04 fixed the rule as well as the number:
   `rhySelectLights` takes a `hardRadius` (= `max_distance` from both modules) and
   a lamp strictly inside it is taken before the cap is consulted, so the cap may
   only trim the fade band. The shipped counts are `max_lights=560 +
   sticky_extra=140` (the equilibrium live set is `max_lights`, so that is what has
   to exceed 552) and `[trafficlights] max_lights=90` (worst case 150 signals
   inside `traffic_exit_distance`). The old 60 was cutting the world dark at ~140
   units, which is the "lights do not reach far enough" half of the report. The
   plugin prints the worst case it finds in the real file at load, and counts
   `in_drop=` in the 10 s line: lamps inside `max_distance` left without a light.
5. **A signal head used to exist only while the game drew it.** The game registers
   a signal corona from its renderer, so the set of heads that reach
   `rhySignalHeadsTake()` is the camera frustum, and `head_ttl_ms` expired heads on
   it: turn your back on a junction and its bulb and glow faded out 2 s later and
   were created again when you looked back — six handle changes a second in the
   10:22 run (`expired=790 retired=817 steps=5669`). The records are now folded
   into a memory (position, last colour, last tick) and `rhyHeadKeepAlive()` keeps
   a stale head by **player distance**; it is dropped only beyond
   `traffic_exit_distance`, or beyond `traffic_max_distance` when it is stale, which
   is the case where the entity pointer really has gone.

### Why the whole world changed lighting at once (s28)

rhY's number one complaint was not the blink above: it was that lighting changes
**instantly** — "the whole world goes dark, then comes back" while driving. The
2026-11-12 log says how instantly (see FINDINGS, same date): the street layer went
between ~1 and ~360 live handles 20 times in 25 minutes, `rtx.skyBrightness` stepped
0.02 → 1.00 nine times, and the computed sun elevation stepped up to 104.5 degrees
between two samples.

The mechanism is a Remix constraint, not a bug: a light's radiance and direction are
fixed at `CreateLight` time, so *any* change to a light is a destroy plus a create,
and every path in the plugin did all of them in one frame. The fix is a time layer,
`plugin/src/smooth.{h,cpp}`, which is pure C++ (no `windows.h`, no Remix structs) so
the whole model runs natively in `tests/smooth_check.cpp`:

* **`RhyFade`** — a 0..1 gain envelope per light. `rhyFadeAdvance()` moves the target
  gain at `1/ramp_seconds` per second; `rhyFadeApply()` moves the *published* gain —
  the radiance the next handle actually carries — by at most `fade step`, and returns
  create / recreate / retire. A handle is only retired once the light is already dark,
  so "destroy" is never a step. Ramps are clamped to ≥ 0.5 s by `rhyFadeInit()`;
  `rhyFadeSetRamp()` is the one exception, used for a traffic signal's colour
  cross-fade, where the two handles (different hash per phase, so both can be live)
  ramp in opposite directions and the total output stays constant.
* **`rhyDistanceGain()`** — the `max_distance`..`exit_distance` band that p08 added
  for hysteresis is now also the fade band: gain is 1 inside `max_distance` and ramps
  to 0 at `exit_distance`, so the hard cut-off happens where the light is already
  dark. `rhySelectLights()` returns that gain along with the id, so the fade is part
  of the selection rather than an afterthought. The caps are unchanged.
* **`RhyRate`** — a rate-limited follower (multiplicative limit plus an absolute
  allowance) for the values that are not lights: `rtx.skyBrightness`,
  `rtx.emissiveIntensity`, and the sun/moon radiance channels. The sky geometry is
  still computed once a second, but the *published* value now advances every frame.
* **`rhyDirLimit()`** — a slerp with an angular rate cap, so the sun's direction
  slews to wherever the game clock put it instead of teleporting.
* **`rhyBeginFrame(n)` / `rhyCreateBudgetLeft()` / `rhySpendCreateSlot()`** in
  `plugin/src/remix.cpp` — a per-frame budget on handle changes. A fade costs one
  create per light per step, so 320 lamps cost ~1600 creates; the budget spreads
  them over frames. Destroys count too, which matters: without it a finished fade
  retired every light in the same frame (the test measured a 42-of-204 aggregate drop
  in one frame that way). A create refused by the budget reports `reason == 3`, which
  is a retry-next-frame condition, not the bridge failing.
* **`rhyPlayerPosGrace()`** in `plugin/src/game.cpp` — the ped pointer at `0xB7CD98`
  read 0 for 13 seconds in that run, and every module took its "no player" branch,
  which used to mean destroy everything. Now the last known position is held for
  `player_grace_ms`, and when the grace really does expire the layers fade out.

The knobs are the `[fade]` section of `rhyremix.ini` (`in_ms`, `out_ms`, `step`,
`creates_per_frame`, `player_grace_ms`, `sky_rel_per_sec`, `sky_abs_per_sec`,
`sun_rel_per_sec`, `sun_deg_per_sec`, `phase_fade_ms`, and since n01 `handle_cadence_ms`,
`handle_cadence_min_ms`, `handle_step_deg`, `handle_rel_lum`), and the floors are enforced
in code: a fade under 500 ms, a step over 0.5, or a rate limit under 1.05x/s is
corrected and logged, so a typo cannot bring the instant switch back.

What s28 deliberately did **not** change: the renderer-side amplifiers of a lighting step
(`rtx.restirGI.historyDiscardStrength = 10`, `rtx.localtonemap.shadows = 2.5` in
`config/rtx.conf`). Retuning them without an in-game A/B trades a lighting pop for a
denoiser artifact. n01 did the measurement s28 skipped and found that the two
`rtx.restirGI.*` keys s28 named belong to an integrator this install does not even run;
see "When the sun's handle is replaced" below.

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

## How the clock reaches the light (s29)

rhY's number two complaint, same session as the pops above: **"time of day flickers and does not
progress smoothly"**. s28 had already bounded what is *published*; this was about what is
*sampled*, and the distinction is the whole finding - a rate limiter cannot make a sun that
alternates between two calendars look like one day. `docs/FINDINGS.md` has the measurements; the
mechanism is in `plugin/src/tod.{h,cpp}`, again pure C++ so a whole simulated day runs natively in
`tests/tod_check.cpp`.

* **`rhyTodUpdate(rawHour, rawMinute, tickMs)`** — the clock. It keeps an anchor (a whole game
  minute plus the tick it was read at) and publishes `minuteOfDay = anchor + elapsed /
  msPerGameMinute`, so the time moves every frame instead of every ~2000 ms. `msPerGameMinute`
  self-calibrates: every accepted forward tick contributes `elapsed / 1` to an EMA (clamped to
  200..60000 ms), seeded from `[clock] ms_per_game_minute`, so the interpolation uses the tick
  length this machine actually runs at (the log measured p50 2015 ms, min 1674). `rhyTodReseed()`
  is what an ini edit calls: it changes the seed, never the time.
* **Monotonicity is a rule, not a hope.** A backwards raw move within `[clock] jitter_tolerance_min`
  is counted as `jitter` and ignored - the time keeps advancing on its own anchor. A bigger one is
  only believed after it *persists* for `[clock] reanchor_ms`, which is what separates a tour's
  `setTimeOfDay()` pin (51 of ~400 logged sample pairs went backwards) from a mission genuinely
  setting the time. Forward moves are always followed: the clock must not disagree with the game,
  and the s28 limiters are what ramp the world. The 1440 wrap bumps `day`, so `absoluteMinute`
  never decreases.
* **Torn reads.** Hour and minute are two byte loads, and SA writes them in two steps, so a sample
  between the writes reads `6:59 -> 7:00` as **6:00**. The repair tries `±60` against the expected
  minute, but only when the previous sample's minute field was 59 - the shape of an actual roll.
  Without that gate a deliberate `setTimeOfDay(13,0)` gets "repaired" an hour off its target, which
  the test caught and the re-anchor path recovers from in 2 s anyway.
* **`rhyCalendarUpdate(month, day, neededStable)`** — the season. `month@0xB70158` and
  `day@0xB70154` are not a calendar (the month byte walks +16, the day sits at 3, and the first
  probe dump of those words read `0 0`), so they are trusted only after 30 consecutive identical
  reads at the 1 Hz keyframe rate and only if the day does not step by more than one. A real SA
  date holds for a game day (~48 real minutes); the counter moves every ~10 s. Until it qualifies,
  `[sun] day_of_year` is used, so the session has one season instead of two.
* **`RhyWeather`** — turbidity and `[sky] cloud` are followed at `[sky] turbidity_per_sec` /
  `cloud_per_sec`, because `rhyConfigReloadIfChanged()` re-reads the ini live and turbidity goes
  straight into the sun's colour. `rhyCloudTransmission()` dims the direct beam and
  `rhyCloudAmbientGain()` gives part of it back to the sky; `cloud=0` keeps both at exactly 1.0.
  This is also where a real `CWeather` read would go - there is no verified offset for one, and
  `rtx.ignoreGameDirectionalLights = True` already means the game's own blend never reaches the
  renderer.
* **`solarPositionF()` / `lunarPositionF()`** in `plugin/src/solar.cpp` take a **float minute of
  day**; the integer versions are still there for the tests and the legacy path. `sun.cpp` calls the
  float versions every frame, so the sun target is the curve the 0.25 deg staircase was sampling.

The knobs are `[clock] ms_per_game_minute`, `jitter_tolerance_min`, `reanchor_ms`,
`calendar_stable_reads` (0 = never trust the game's date) and `[sky] cloud`, `turbidity_per_sec`,
`cloud_per_sec`. The log line carries the proof: `tod=… ms/min=… ticks=… torn=… jitter=…
reanchor=… wraps=… bad=…` and `cal: stable=… accepted=… rejected=…`.

What is deliberately **not** changed: the in-game calendar stays unused unless it behaves
like a date - with the shipped default it never does, which is the correct outcome for
this build, not a fallback. The other thing s29 left alone, the distant-light rebuild
threshold (`[sun] step_deg = 1.0`, its note T7), is closed by n01 in the next section.

## When the sun's handle is replaced: the cadence, and the renderer's temporal keys (n01)

s28 stopped the plugin from *sending* a step, and rhY's 2026-11-12 report still said the
world changes at once. Two things were left: s29's own open note T7 (the sun and moon
handles were still replaced on a **threshold**) and the renderer's own temporal settings,
which s28 had named and not measured.

A distant light's direction and radiance are baked at `CreateLight`, so a moving sun is a
sequence of destroy+create pairs and each pair is a hard step in shadow direction. The old
rule in `plugin/src/sun.cpp` was `movedDeg > [sun] step_deg || 5% luminance`:

* the step is as big as the threshold: the handle is replaced exactly when it is a whole
  degree out of date, and the sun's disc is 0.53 deg - every rebuild moved the penumbra by
  twice the sun's width;
* the period follows the body's rate: ~8 s while the clock runs normally (1 deg at
  0.125 deg/s), ~85 ms while the direction limiter is slewing, ~17 ms at the steepest point
  of a radiance ramp. The tour's `setTimeOfDay()` pins walk through all three in one stop.

`rhyCadence*` in `plugin/src/smooth.{h,cpp}` replaces the trigger with a **clock**:

* `rhyCadenceInit(c, maxStepDeg, maxRelLum, minMs, maxMs)` - the budgets and the bounds.
  Out-of-range values are corrected rather than honoured, because a budget of 0 means
  "rebuild every frame" and the floor is what protects the bridge.
* `rhyCadenceObserve(c, degMoved, relLumMoved, dt)` - feed what the s28 limiters
  **published** this frame, not what they allow. At rest the sun moves 0.002 deg per frame
  while the limiter allows 12 deg/s; cadencing off the configured limit would rebuild every
  body at the churn floor forever. Smoothed over ~0.25 s, so one frame of a clock jump does
  not set the period for the next minute and a body that stopped moving drifts back to it.
* `rhyCadenceIntervalMs()` - the period: `maxStepDeg / degPerSec` and `maxRelLum /
  relPerSec`, floored at `handle_cadence_min_ms` and capped at `handle_cadence_ms`, which is
  the fixed cadence a still body keeps.
* `rhyCadenceSetGap()` + `rhyCadenceDue()` - the period is derived from a *rate*, and a rate
  that collapses (a ramp reaching its target) would otherwise leave a handle already off by
  more than the budget sitting there for a whole period; measured at 0.84 of its own
  luminance in `tests/handle_check.cpp`. The gap trigger cannot fire before the floor, so
  the step bound is `max(budget, rate x minMs)` either way.
* `rhyCadenceDue()` does not consume the due state: a rebuild the frame's create budget
  refuses is retried next frame. A rule that forgot a refused rebuild would be a threshold
  rule wearing a clock.
* A period whose handle already carries the published values costs no bridge call
  (`cad.skips`): with `[sun] force_hour` pinned, the sun creates nothing at all.

`rhyAngleDeg()` is in the same file for the same reason: `acos(dot)` on a float dot product
0.002 deg from 1 returns noise (it reported 0.02 deg for a real 0.002 deg step, 10x over),
and a cadence driven by that noise sits at its churn floor forever.

The knobs are `[fade] handle_cadence_ms` (2000), `handle_cadence_min_ms` (150),
`handle_step_deg` (0.5, about the sun's disc; 0 falls back to the body's own `step_deg`),
`handle_rel_lum` (0.05). The measured trade, from `tests/handle_check.cpp`, which runs the
same simulated sky through both rules:

| sky | old threshold rule | the cadence |
|---|---|---|
| a normal morning | 0.12 creates/s, one step 1.00 deg, every 8.2 s | 0.51 creates/s, one step 0.25 deg, every 2.0 s |
| the 104.5 deg clock jump, slew window | 8.1 creates/s, 1.20 deg | 5.2 creates/s, 1.80 deg (the floor binds) |
| the tour's clock pin | 0.46 creates/s, spacing 85 ms .. 8.2 s | 1.12 creates/s, spacing 153 ms .. 2.0 s |
| twilight's 300x radiance ramp | ~10 creates/s while it lasts, spacing down to 17 ms | bounded at 6.7 creates/s |
| a pinned clock | one rebuild per threshold crossing | 0 creates, 30 periods skipped |

Four times finer direction steps in the normal case, bounded churn in every case, and one
honest regression: during a steep radiance ramp the luminance step is what the churn floor
allows - up to 0.84 of itself near the 0.02 night floor, where `[fade] sky_abs_per_sec`
makes small values fall fastest in **relative** terms. In absolute terms that step is
0.08 -> 0.03, a sun that has already set.

The renderer half is a file, not code: `config/rtx.conf.nopops`, generated by
`tools/rtx-nopops.py` from `config/rtx.conf` and switchable with
`tools/rtxprofile.sh nopops` (undo with `restore`). It changes nine temporal keys and
nothing else - `tests/test_rtx_nopops.py` asserts exactly that, and
`scripts/rtxprofile-selftest.sh` hashes the eight texture-hash lists and the look keys to
prove it. Why those nine, and why `rtx.demodulate.enableDirectLightBoilingFilter` is not
one of them, is the table in FINDINGS ("the renderer's own time knobs").

## Two writes to one game function: the signal hook and the bulb (s30)

`CTrafficLights::DisplayActualLight` (0x49DAB0) registers every lit signal head
with `CCoronas::RegisterCorona`, and the plugin now writes to that one function
twice, from one `VirtualProtect` on code page `0x49D000`:

| address | what | gate |
|---|---|---|
| `0x49DD4C` | `E8 rel32` (`call 0x6FC580`) -> `E9` to the recorder stub | `[trafficlights] source=game` + the call decodes to `0x6FC580` + `0x6FC580` is the corona-type thunk + `add esp,0x54` at `0x49DD54` |
| `0x49DCDA` | the `imm32` of `push 15.0` at `0x49DCD9` - the corona **radius** the game gives each head | `corona_radius > 0` + the five bytes at `0x49DCD9` are `68 00 00 70 41` |

Both are decided by pure functions in `plugin/src/signalheads.cpp`
(`rhySignalPatchCheck`, `rhySignalCoronaPatchCheck`, `rhySignalCoronaRadius`) so
`tests/signals_check.cpp` can exercise every refusal, and `tests/test_signal_hook.py`
asserts the same bytes against the real `gta-sa.exe` - a game update or a SilentPatch
that moves that code turns the test red before it turns the game red.

The install path is a two-step walk, and the reason it must be is a bug worth
remembering: `rhySignalPatchCheck(source, NULL, NULL, NULL, NULL)` used to answer
a null-window call with `kRhyPatchCallSite` ("the bytes are wrong"), the install
path called it exactly that way to ask the source question, and so the exe was
never read and the hook never installed - on any build. The null-window answer is
now `kRhyPatchNoBytes` ("go and read them"), and the walk is asserted step by step.

The other half of s30 is the size of the light we own. `rhySignalBulb()` is the
only place that decides it, and both the head path and the list path call it, so
the fallback cannot change the look:

- `fixture_size=1` (default): radius = `rhySignalRadius(fixture_scale)` (0.10 m
  for a 200 mm lens) or `bulb_radius`, radiance = `bulb_radiance` **nits at that
  radius**, no `(radius_ref/radius)^2`.
- `fixture_size=0`: the old flux-preserving blob (`rhyRadianceForFlux`), because
  there the ini's radius is a brightness dial and shrinking it must not darken
  the street.

The multiplier is the trap: it is correct for a street luminaire and, applied to
a 0.10 m lens, turned `radiance=1.5` into 5400 nits - the output of the old six
metre ball in a marble, sixty times over. `tests/signals_check.cpp` now bounds a
signal's flux under 5% of a mid street luminaire's, computed from the same
`rhyStreetFixture()`/`rhyRadianceForFlux()` the plugin uses.

## Where a signal light goes when the hook is not the source (n02)

Two decisions live in this area, and both were wrong in ways the numbers could
reach offline.

**1. Whether the hook is the source.** `rhyHeadsHealthy()` used to answer yes/no
on one fact - "the hook has recorded nothing for 20 s" - and s30 justified that
with "the patched call site was never reached". It was reached (the 2026-11-11
13:04 crash's stack held a real `CEntity*` and head index 1), and the sample was
taken 79.1 m from the nearest of the 1522 list entries while the game's own
corona range is 50.0 m (`push 0x42480000` at `0x49DCF3`, tested in the callee at
`0x6FC22E`, i.e. after our patch point, fading 35.0 m -> 50.0 m). So the sample
proved nothing and the session spent its whole run on the list. The function now
returns one of four states (`kRhyHeadSourceGame` / `Grace` / `Far` /
`kRhyHeadSourceList`) from `(records, tick, now, graceMs, nearestSignalM,
coronaRangeM)`, `rhyHeadKeepsHook()` turns that into the light layer, and the
module logs the decision, the distance and the counters once when it changes.
The distance comes from `rhyNearestSignal2D()` over the same `g_xy` array the
selection uses; the range is `kRhySignalCoronaRangeM` in `signalheads.h`, and
`tests/test_signal_hook.py` reads 50.0 / 35.0 / 1-15 out of the exe's `.rdata`
so the constant cannot drift from the game.

**2. Where a list-mode light goes.** The list file used to be four columns - a
pole and a height - and the lamp table it was generated from carries a full
model-local head offset (`cj_traffic_light4`: `-2.10, +0.10, +7.60`), so the
bulb sat 2.10 m sideways of the lens. `tools/traffic_list.py` now writes

```
# rhyremix traffic v2
x y z headz heading lensx lensy lensz
```

`heading` is the IPL instance yaw in degrees (every traffic instance in
`gta3.img` carries a pure Z-axis quaternion, so a yaw is the whole rotation),
`lens*` is the offset in the model's own frame, and the plugin rotates and adds:
`rhySignalLensPos()` in `plugin/src/signalheads.cpp`, the same rule the game
applies to a head offset at `0x59C890`. `lens_offset=0`, or a v1 four-column
line, gives the old `x,y,z+headz` exactly - and `--v1` regenerates the old file
byte for byte, which `tests/test_traffic_list.py` asserts against the deployed
`scripts/rhyremix_traffic.txt`. Offsets are only borrowed across a `.ide` radius
gap of 10 or less, which is why `mtraffic1`/`mtraffic2` (562 of 1522, gap 25)
keep the pole x/y.

`RhyTrafficLine`, `rhyTrafficParseLine` and `rhyTrafficLightPos` live in
`signalheads.h/.cpp`, not `trafficlights.h/.cpp`, on purpose: `trafficlights.cpp`
needs the Remix API headers and cannot be linked by a native test, and the
parsing plus the position math is exactly what `tests/signals_check.cpp` has to
measure (the lens position at four headings, the 2.10 m it recovers, and that a
v1 line does not move).

## A lit signal is two lights: the bulb and the glow (n02b)

rhY's correction on the 2026-11-12 report: the ambient glow around a signal was
**wanted**; the bulbs were the missing half ("both like real life"). s30 had
removed the ambient on the way to the bulb - our light's flux 304 -> 67, the
game's own 15.0-unit corona -> 0.45 - and n02 never added a second light, so a
lit signal became a bright dot that colours nothing. The rule now, in one place:

- `rhySignalGlow()` (`signalheads.*`) is the glow's only decision point, the
  mirror of `rhySignalBulb()`. Same `rhySignalLensRgb(phase)` colour, and the
  radiance is **solved from a flux**: `base = glow_flux / (4*pi*r^2 * mean(col))`.
  Defaults `glow_radius=3` m, `glow_flux=304` - the flux the 2026-11-12 00:48
  light measured (`4*pi*(0.10)^2 * mean(lens x 5400 nits)`) - and
  `rhyLuminaireFlux()` derives the ceiling (3473, one mid street luminaire) that
  `config.cpp` clamps to. Solving for flux rather than radiance also keeps the
  spill's strength constant through the red/amber/green cycle.
- `trafficlights.cpp` carries **two handles per signal** (`SignalState` /
  `HeadState`: `handle` + `glow`), and `createLight`/`createHeadLight` take a
  `kind` (`kSignalBulb`/`kSignalGlow`) that changes only the spec and the hash
  (`kRhySignalGlowHashBias`, above both the traffic and the street hash ranges).
  One fade envelope drives both, so they cannot disagree about phase or gain;
  retire and recreate destroy both under the same tag; a missing glow is re-added
  on its own without destroying the bulb (destroying a working light to attach a
  second one is the pop s28 removed). `glow_flux=0` means no second handle at all.
- The 10 s summary logs `N heads lit (N with a glow), N bulbs + N glows drawn`,
  and `scripts/diagnose.sh` flags it when the glow count lags the head count.

The bounds live in `tests/signals_check.cpp` (bulb small + hot, glow big + soft,
same colour in all three phases, flux 304 under a luminaire's 3473) and the
wiring in `tests/test_signal_glow.py` (both paths, both halves, drawn and
destroyed together, no hash collision, the ini keys and the test doc).

## A car's lamps come from the game's corona store (n12)

`plugin/src/vehiclecorona.{h,cpp}` (game-facing) and `plugin/src/vehiclelights.{h,cpp}` (pure C++,
the whole decision, tested by `tests/vehicle_lights_check.cpp`).

`CVehicle` keeps no lamp list. What the game does every frame is call `CCoronas::RegisterCorona`
(thunk `0x6FC580`), and the store it fills is the only place the vehicle's light dummies are
published in world space:

```
0xC3E038  dword   live corona count, 0..64
0xC3E000  11 sprite pointers, indexed by the sprite slot handed to the thunk (4 = coronaheadlightline,
                  2 = coronamoon, the rear lamps' sprite)
0xC3E058  entry 0, stride 0x3C, 64 entries:
    +0x00 x,y,z   +0x0C key (entity + head index)   +0x10 texture pointer
    +0x14 strength +0x1C range                      +0x28 radius
    +0x2C r,g,b,a                                   +0x38 linked entity
```

Reading it (page-validated, read-only — nothing is patched here, unlike the signal hook) beats
re-deriving the dummies from the vehicle pool. Three details that are not obvious from the data:

- the key is `entity + head index` and the index reaches **7**, so `key & ~3` splits one car into
  two groups; the rule is "keys within 16 bytes of each other", and a group whose lamps do not fit
  in `max_spread` (6 m) is refused — without that, a red shopfront becomes a parked car's tail lamp;
- front lamps are identifiable by sprite pointer, rear lamps only by hue, and "braking" only by a
  brighter red (`brake_red_min`), because the game writes the 2dFx colour per light state and the
  store carries no brake flag;
- a headlamp with no axis is **dropped**, not aimed at nothing: `forward` comes from the car's own
  head-centroid minus tail-centroid, falling back to the entity matrix's `at` column.

Radiometry follows the plugin's flux convention (flux = 4·π·r²·L, one street luminaire = 3473
units) with the beam's geometric concentration `2/(1-cos θ)` applied, because
`light_shaping.slangh`'s shaping is a 0..1 **mask** and nothing renormalises it — a cone-cut light
without that factor is 21x dimmer than the same lamp unshaped. A Remix light's position, direction
and radiance are baked at `CreateLight`, so a moving lamp is rebuilt only when it drifted more than
`move_step`, turned more than 10 degrees, changed filament, or has a fade step due — and never more
often than `cadence_ms`.

## Shutdown: noticing the game is ending (n09)

`plugin/src/shutdown.{h,cpp}` owns one question — *is the Remix bridge still worth calling?* — and
it owns it in plain C++ so `tests/shutdown_check.cpp` can drive every path without a game process:
the device reads arrive as `RhyShutdownSignals`, the ini's persistence windows as
`RhyShutdownLimits`, and every tick is a `unsigned` compared wrap-safely.

* Four signals, four windows (`rhyremix.ini [shutdown]`): `present_ms=1500`, `device_ms=3000`,
  `window_ms=750`, `calls_ms=4000`. A signal must be on **continuously** — a gap clears that
  signal's own timer, because alt-tab and a resolution change look like `present` + `device`.
* `calls` (our own climbing `create_fail + draw_fail`) **never ends the session on its own**: one
  light the Remix server refuses makes those counters climb for a whole session, and
  `streetlights.cpp`'s `g_apiBroken` is the mechanism for that. It only fires with another signal on.
* Death **latches**. `rhyRemixShutdown()` makes `rhyRemixApi()` return null, which is the guard every
  module already has, so no module has to know what shutdown is; `rhyRemixSkippedCalls()` counts what
  was not called. `rhySignalHookUninstall()` puts the game's five bytes back at `0x49DD4C`, from the
  render thread only (the same thread that executes that instruction), and `install()` refuses to
  re-patch afterwards (`kSignalHookShutdown`).
* The window handle comes from `IDirect3DDevice9::GetCreationParameters` (vtable 9), read once at
  hook install: the game hands `Present` no `hFocusWindow`, and hardcoding a window class or a game
  address is the kind of assumption this plugin has been burned by before.
* `DLL_PROCESS_DETACH` logs through `rhyLogDetach()`, which takes no mutex — at detach another thread
  can have been terminated holding it, and the whole complaint was that the run had no last line.
* `stop_present_after_ms` defaults to 0. It is the experiment, not the fix: the 2026-11-12 evidence
  puts the spin in the game's reset path, and this switch is what proves or disproves that on the
  next run.

### Ending the run from outside (n09b)

The tracker above needs the game to call `Present` again, and on 2026-11-12 it never did — twice. The
second hang (11:48, `build/tmp/exit-hang-20261112-b.txt`) also showed that the *bridge* does not end
itself: `NvRemixBridge.exe` waits on an exit callback for the client process (which is the spinning
game) and its IPC timeout was switched off by the window going inactive, so it burned 406 % CPU and
3.6 GB of VRAM for sixteen minutes while `launcher/launch-rtx.sh` sat in `waitforexitandrun`.

So the launcher owns the outside half now, in a block between `n09b cleanup (BEGIN)` and `(END)`
that `scripts/launch-cleanup-selftest.sh` sources and drives against stub processes (33 checks):
`pids_named` (comm or argv[0], by pid — never `pkill -f`, whose pattern this script's own command
line would match), `window_state` (0/1/2, "cannot tell" never fires anything), `end_wine_tree`
(bridge, then `WINEPREFIX=<compatdata>/pfx wineserver -k` **only if a wineserver for this prefix is
running** — invoking `wineserver` with none up starts one just to kill it — then a sweep by pid/name) and
`watch_run_end` (armed only after the window has been seen). Two rules worth keeping: a signal that
cannot be measured must not be guessed at, and the cleanup after the wait returns is unconditional —
`RHYS_EXIT_GRACE=0` stops the watchdog, never the cleanup.

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
- `tests/test_changelog_coverage.py` (n14) walks `git log --all --since=2026-11-11` and fails if a
  commit — including the ones parked on `failed/*` and `wip/*` — is not named in
  [../CHANGELOG.md](../CHANGELOG.md), or if [STATUS.md](STATUS.md) stops naming the parked branches
  and the test sessions it summarises. When you park, abort or land a task, the changelog and that
  page are part of the change, not an afterthought.
- `coneAngleDegrees` in the Remix API is the **half** angle (`rtx_remix_api.cpp:544` takes its
  cosine). The street lamps' `cone_angle=120` is therefore a 240-degree flood, which is only
  sensible for a luminaire aimed at the road; a car's 50-degree beam is `cone=25`.
