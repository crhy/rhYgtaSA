# What is installed, what replaces what

The install is three independent layers stacked on top of each other. A symptom
like "everything is too bright" or "no light casts a shadow" is usually one
layer doing the same job as another, not a bug in any single one.

```
layer 1  ASI loaders      gta-sa.exe  <- scripts/*.asi  (C/C++ hooks, run in-process)
layer 2  MoonLoader       moonloader/*.lua  (Lua scripts, also in-process)
layer 3  RTX Remix        rtx-remix/mods/*  + rtx.conf  (the renderer itself)
```

`rhyremix.asi` sits in layer 1 and talks to layer 3 through the Remix C API
(`scripts/rhyremix.asi` -> `rtx-remix/bridge32.log` -> `remix-dxvk.log`).

## The trap that explains "too bright"

Three of these mods light the same lamp twice, and none of them know about the
others:

| what lights the scene | where it comes from | on by default |
|---|---|---|
| the game's own sun | `rtx.ignoreGameDirectionalLights = True` in `rtx.conf` | **off** (already suppressed) |
| the Remix asset pack's lamp meshes | 2174 `.usd` files under `rtx-remix/mods/gameReadyAssets` | **on** |
| the Lua light manager | `moonloader/SARemiv_LightMan.lua` moves the lamp meshes | on unless disabled |
| the Lua sun mod | `moonloader/SARemix_Sun.lua` (script_name `SARemix_Real_Sun`) | **on** |
| our API lights | `scripts/rhyremix.asi` `[streetlights]` / `[trafficlights]` / `[sun]` | on |

So with the stock install plus our plugin you get **two suns and two sets of
lamps**. `rtx.autoExposure.enabled = False` in `rtx.conf` means nothing
compensates for it: what you see is the raw sum.

`./deploy.sh` now follows the ini instead of leaving the Lua mods alone:
`[streetlights] enabled=1` disables the Lua light manager, `[sun] enabled=1`
disables the Lua sun mod. Override with `--lua-on/--lua-off`, `--sun-on/--sun-off`.

## Inventory

### Layer 1 - ASI loaders and C++ mods (`scripts/`, game root)

| file | what it does | relation to our work |
|---|---|---|
| `scripts/GTASA.WidescreenFix.asi` | FOV/aspect fix, HUD repositioning | keep; no overlap |
| `scripts/modloader.asi` | loads the `modloader/` plugin set below | keep |
| `MoonLoader.asi` (game root) | hosts layer 2 | keep, but see the Lua rows below |
| `SilentPatchSA.asi` (game root) | bug fixes (crash fixes, audio) | keep |
| `scripts/rhyremix.asi` | **ours**: street lamps, traffic lights, sun, moon, gamma guard | replaces the Lua light manager and the Lua sun mod |

### Layer 2 - MoonLoader scripts (`moonloader/`)

MoonLoader loads every `*.lua` in that folder. GTASA-Remix itself ships its
disabled scripts as `*.dat`, which is the trick `deploy.sh` uses too. Watch the
naming: **`SARemiv_*.dat` is a disabled script, `SARemix_*.dat` is a settings
file** the scripts read at startup. Renaming the wrong one breaks a mod's
settings, not the mod.

| file | script_name | what it does | replaced by | how to disable |
|---|---|---|---|---|
| `SARemiv_LightMan.lua` | `SARemix_Light_Manager` | turns street lamps on/off by hour by moving the lamp meshes; keeps state in `SARemix_Light_Manager.dat` | **`rhyremix.asi` `[streetlights]`** - same job through the Remix API, no mesh moving | `./deploy.sh --lua-off` (renames to `.dat`) |
| `SARemix_Sun.lua` | `SARemix_Real_Sun` | sun/moon disc objects (15890/15891), light direction, imgui debug window; state in `SARemix_Real_Sun.dat` | **`rhyremix.asi` `[sun]` + `[moon]`** - one Remix distant light per body | `./deploy.sh --sun-off` |
| `SARemix_Real_Sun.dat` | - | settings file for the script above, **not** a script | - | do not touch |
| `SARemix_Light_Manager.dat` | - | settings file for the light manager | - | do not touch |
| `SARemix_HBAO.lua` | `SARemix_HBAO` | draws the text "SA REMIX HEALTH BAR ALWAYS ON STARTED" once per frame for 15 s, then a zero-size dot. It is a trigger for a Remix shader mod, not an AO implementation | nothing in our plugin; harmless | rename to `.dat` if you want the on-screen text gone |
| `SARemix_VTK.lua` | `SARemix_Vehicle_Toolkit` | per-vehicle material overrides (chrome, glass, mirror, black plastic) driven by an imgui window; reads `SARemix_VTK.conf` (`Mod List = Vanilla`) and `moonloader/SARemix/texture/*.png` | nothing in our plugin; keep | rename to `.dat` |
| `SARemix_VTK_Vanilla.dat` | - | a second, disabled VTK variant (517 KB) | - | already disabled |
| `SARemix_Shot.lua` | `SARemix_Shot` | **ours**: watches F12 through MoonLoader's `isPs2KeyboardKeyJustPressed` and writes `<game>/screenshots/pending/rhyshot_N.req`; reads `SARemix_Shot.conf` (`key=123`) | nothing - this is the one Lua script the project keeps, because only MoonLoader can read the game's key state without hardcoding an address | `./deploy.sh --shotkey-off` |
| `SARemix_Shot.conf` | - | key / cooldown / on-off for the script above | - | do not touch |
| `lib/`, `libstd/`, `luajit/`, `luarocks/` | - | MoonAdditions (`mad`), imgui, vkeys, ffi, memory | keep | - |

`moonloader.log` is the authority on what actually loaded:
`Loading script "..."` / `Loaded successfully` lines. Anything not listed there
is not running.

### Layer 3 - the Remix renderer

| file | what it does |
|---|---|
| `rtx.conf` | every RTX/MagicAI setting (see below) |
| `rtx-remix/mods/gameReadyAssets/**` | 2174 `.usd` material/light overrides for the whole game |
| `rtx-remix/mods/RTXIO/**`, `RTXV/**` | RTX IO / vertex-remap mods |
| `dvk.conf` | DXVK-Remix settings (`devk.enableAftermath` names the shader on a GPU crash) |
| `tools/usd_light_defaults.py` | ours: writes sane `inputs:shaping:focus` defaults into `.usd` files; NaN shaping was the cause of the earlier `VK_ERROR_DEVICE_LOST` |

## Known broken interactions (measured, not guessed)

* **`D3D9Texture2D::QueryInterface: Unknown interface query 794950f2-adfc-458a-905e-10a10b0b503b`** in `remix-dxvk.log`. That GUID is MoonAdditions' private interface. dxvk-remix does not implement it, so every `mad.*` overlay call (the HBAO text, VTK's on-screen state, the sun mod's debug text) runs against a stub. Lua mods that *only* draw text through `mad` are effectively no-ops; ones that also poke memory (both print `sa_ped_safe active`) still work.
* **3066 `Legacy light attribute detected`** warnings in `remix-dxvk.log`, e.g.
  `/RootNode/meshes/mesh_D475E4169EA58195/Disco_Big_Light_01.radius`. The asset
  pack was authored against an older Remix attribute naming (`radius`,
  `intensity`, `color`, `enableColorTemperature` as mesh attributes). remix-main
  still reads them but flags them; if a lamp looks wrong, check its attribute
  names against the current schema before touching our plugin.
* **`[RTX] InstanceManager: detected unsupported quad index layout for billboard
  creation`** and **`Trying to bind a texture to a mesh without UVs`** - the
  sun/moon discs and some sprites are quads the raytracer cannot billboard.
* **`[RTX-Compatibility-Info] Trying to raytrace but not detecting a valid camera`**
  on the first frames, before the camera is established.

## `rtx.conf` knobs that decide brightness and "does it cast"

| setting | value in this install | effect |
|---|---|---|
| `rtx.sceneScale` | `0.01` | **1 game unit = 1 cm.** A light radius of `1` is a 1 cm bulb; `15` is 15 cm. This is why the first builds looked like nothing was lit. |
| `rtx.zUp` | `True` | Remix treats GTA's Z as up; our plugin passes world coords through unchanged |
| `rtx.ignoreGameDirectionalLights` | `True` | the game's sun is gone; only the asset pack's lights and our `[sun]`/`[moon]` remain |
| `rtx.emissiveIntensity` | `10` | multiplies every emissive material in the 2174-file asset pack. First knob to turn down if the world is blown out. |
| `rtx.skyBrightness` | `1.0` in the file, **pushed by the plugin every 100 ms** | ambient sky; raises the floor of every shadow. Editing the file does nothing past the first push - at night the plugin publishes `[sky] night+moon`, capped since n24 at `[sky] ambient_cap_mult` (0.25) x the moon light's own radiance. See [TESTING.md](TESTING.md) |
| `rtx.autoExposure.enabled` | `False` | nothing normalises the image; over-lit stays over-lit |
| `rtx.localtonemap.shadows` | `2.5` | lifts shadow floors - a direct cause of "lights don't cast" |
| `rtx.fallbackLightMode` / `fallbackLightRadiance` | **`0` / `0, 0, 0`** (n24; the runtime's defaults are `1` = NoLightsPresent and `1.6, 1.8, 2`) | the light the runtime hangs on the camera when a scene has no lights - menus, the first seconds of a run. The old shape (`fallbackLightType = 1` sphere, `fallbackLightRadius = 3`, both now commented out in the profiles) delivers **0.4430** at 6 m against the moon's **0.0515**: it out-lights the night's only real source **8.6:1**. The runtime's own tooltip asks for `Never` in a production build |
| `rtx.tonemap.shadowContrast` | **`0.4`** (n24; runtime default `0`) | the global curve's shadow push: moon-lit sand 150 units from any lamp goes 92/255 -> **19/255** and the night's away/under image ratio 36 % -> **7.5 %**. `shadowContrastEnd` stays at its default 0, which is what keeps noon out of it |
| `rtx.bloom.luminanceThreshold` | **`1.0`** (n24; runtime default `0.25`) | what is bright enough to bloom. At 0.25, **10.31 %** of the sea/pier band of the 2026-11-12 frames bloomed at full weight - the V4 halo; at 1.0 only the 0.23 % that is genuinely clipped does |
| `rtx.enableTransmissionApproximationInIndirectRays` | **`False`** (n24; the runtime default, and what `88e8cd1` had set to `True`) | `True` lets indirect rays keep their direction **through** the sea instead of bending - the sky and the lamp pools arrive under the pier |
| `rtx.pathMaxBounces` | `2` | GI depth; shadows come from the direct-light pass, so a light that "doesn't cast" is usually radius/radiance, not bounces |
| `rtx.enableVolumetricLighting` | `True` | light cones become visible; a too-large radius shows up as a fog wall |

## How to A/B a single mod without rebuilding

```
scripts/abtest-lights.sh status                 # what is on now
scripts/abtest-lights.sh lights off             # our lamps only
scripts/abtest-lights.sh sun off                # our distant sun/moon off
scripts/abtest-lights.sh gamma off              # forward SetGammaRamp again
scripts/abtest-lights.sh probe on               # one calibration light at probe_x/y/z
```

Each switch edits the deployed `scripts/rhyremix.ini` in place, keeps the previous
ini as `rhyremix.ini.abtest-prev`, and prints what the comparison proves.

## Tidying the install: `OLD/`

`scripts/tidy-game.sh` parks things nothing loads any more under
`<game>/OLD/<timestamp>/`, keeping the relative path so `--restore` can put them
back. It is **dry-run by default** and never deletes anything.

```
scripts/tidy-game.sh              # print the plan
scripts/tidy-game.sh --apply      # do it
scripts/tidy-game.sh --restore    # put the newest snapshot back
scripts/tidy-game.sh --repo       # also clean this repo's scratch files
```

Why `OLD/` is inert: MoonLoader only scans `moonloader/*.lua`, ModLoader only
scans `modloader/<name>/<name>*.asi`, and the Remix bridge only scans
`rtx-remix/mods/`. A folder called `OLD/` at the game root is not read by any of
them.

| moved | why it is safe |
|---|---|
| `moonloader/SARemiv_LightMan.dat` | disabled light manager; `[streetlights]` does the job. Its settings file `SARemix_Light_Manager.dat` stays, so `--lua-on` still works (it also pulls the `.dat` back out of `OLD/`) |
| `moonloader/SARemix_Sun.dat` | disabled sun mod; `[sun]`/`[moon]` do the job. `SARemix_Real_Sun.dat` stays for the same reason |
| `moonloader/SARemix_VTK_Vanilla.dat` | a second, already-disabled VTK variant (517 KB) |
| `modloader/VehFuncs/` | `modloader.ini` lists `VehFuncs` under `[Profiles.Default.IgnoreMods]`, so ModLoader never loads it. The script re-checks that line before moving it |
| `rtx-remix/captures/` | 192 MB capture from an earlier session. The bridge loads `rtx-remix/mods/`; no config points at `captures/`, and the script refuses to move it if one ever does |
| `logs/launch-*.out`, `cleo.log`, `metrics.txt`, `modloader/*/[Name].log` | runtime logs, rewritten on the next run |

| kept | why |
|---|---|
| `moonloader/SARemix_Light_Manager.dat`, `SARemix_Real_Sun.dat` | settings files, needed if a Lua mod is re-enabled |
| `moonloader/SARemix/`, `SARemix_VTK.conf`, `SARemix_VTK.lua`, `SARemix_HBAO.lua` | active scripts and the textures they read |
| `moonloader/SARemix_Shot.lua`, `SARemix_Shot.conf` | the in-game half of the F12 screenshot hotkey; `scripts/shot-watch.sh` is the outside half |
| `modloader/SARemix/` | Remix geometry (`inverse_pyramid.col`, `LOD_cone_*.dff`) |
| `modloader/Remix_fix/` | ini overrides; its `MixSets.ini` differs from the live one |
| `rtx-remix/mods/`, `rtx-remix/logs/`, `.trex/`, `scripts/` | the renderer, the bridge and our own payload - the script refuses to touch these prefixes |
