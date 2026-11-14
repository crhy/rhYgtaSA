# GTA San Andreas with RTX Remix on Linux

![GTA San Andreas rendered by RTX Remix under Proton on Linux: path-traced lighting, DLSS](screenshots/current.png)

**GTA San Andreas** (2004, 32-bit) rendered by **NVIDIA RTX Remix 1.6.1** — path-traced global
illumination, DLSS, real street lamps, traffic signals, sun and moon — running under **Proton** on
Linux. Tested on an RTX 4070 Ti SUPER, NVIDIA driver 610, Proton Experimental, Steam Linux Runtime 4:
solid 60 FPS with DLSS.

For a player this is **two steps**: downgrade the game to 1.0 US, then run one installer command.
Everything else — the Proton environment, the DLSS/NVAPI and controller settings, the plugin build,
the lamp and traffic-signal data generated from your own game files, the screenshot hotkey, the
free-VRAM check — is done by the scripts in this repo.

This repo contains **only our fixes** (patches, an `.asi` plugin, helper scripts, tools, launcher).
It does not include the game, NVIDIA's Remix runtime, or GTASA-Remix's assets — get those from their
original sources. Our code is MIT; see [THIRD_PARTY.md](THIRD_PARTY.md) and
[docs/MODS-REPLACED.md](docs/MODS-REPLACED.md) for what is installed and what our plugin replaces.
What changed over time is in [CHANGELOG.md](CHANGELOG.md), release by release; what the newest build
means for you as a player is [docs/RELEASE-NOTES-next.md](docs/RELEASE-NOTES-next.md).

## What it does

One native plugin (`rhyremix.asi`, 32-bit, cross-compiled with MinGW-w64) plus the scripts that
install and test it. What is in the box today:

| area | what you get | how it is decided |
|---|---|---|
| **Sun, moon, sky** | a sun and a moon that are where San Andreas' latitude, season and the game's clock put them, with the sky's ambient following them | `plugin/src/solar.cpp` (physics) + `plugin/src/tod.cpp` (one continuous, never-backwards game minute) |
| **Street lamps** | every lamp in the game's own lamp table and IPL files - 7002 of them - lit at its real fixture size, in sodium or mercury colour, switching at twilight instead of at a clock time | `plugin/src/lamps.cpp`, `tools/lamp_list.py`, generated per install from your own game files |
| **Traffic signals** | a bulb on the lens **and** a few metres of its colour around the head, taking the colour from the game's own signal corona records | `plugin/src/signalhook.cpp` (read-only hook) + `signalheads.cpp`, with the generated `rhyremix_traffic.txt` as the fallback |
| **Vehicle lamps** | a car's head, tail and brake lamps as real lamps - a 0.09 m reflector in a 50-degree cone and 0.05 m red lenses - aimed along the car's own axis | `plugin/src/vehiclecorona.cpp` reads the game's corona store; `vehiclelights.cpp` sizes it |
| **Never a pop** | no lighting change is one frame wide: lamps fade in and out, the ambient and the sun are rate-limited, and a handle change is budgeted per frame | `plugin/src/smooth.cpp`, floors in `[fade]`, asserted in `tests/smooth_check.cpp` |
| **Water** | five variants, `remix-ref` by default, installed by the installer and re-ensured on every start, with the `rtx.conf` keys each variant needs | `tools/water.sh`, `tools/rtx_water_keys.py`, `config/water/*.usda` |
| **Vegetation** | the pack's 234 foliage card textures tagged as cutouts (`rtx.hairCardTextures`) instead of being blended into scribbled lines, its mis-named LOD dictionary fixed, and its real cost measured (18x the triangles, 92-188 MB pinned) | `tools/vegetation.sh` (`check` / `--step 5` / `--step 6`), `tools/vegetation_hashes.py`, `tools/vegetation_ide_check.py`, `tools/vegetation_stream.py`, `tools/vegetation_edge_measure.py` |
| **Frame rate** | a 60 fps profile and a temporal A/B profile, switchable without hand-editing `rtx.conf`; the 60 fps profile now also bounds the runtime's opacity-micromap work, which is where the 2026-11-12 frame tail clusters, and the plugin bounds its own per-frame handle releases | `tools/rtxprofile.sh`, `config/rtx.conf.perf`, `config/rtx.conf.nopops`, `tools/perf-report.py` (MangoHud CSV → spikes/min, A/B), `[fade] destroys_per_frame` |
| **DLSS 5, measured (n19)** | the reference video's single biggest advantage does **not** run here: the runtime's own capability query fails on every run (`NVIDIA DLSS-NR support query failed (0xbad00010)`, driver 610.57.04 on an RTX 4070 Ti SUPER, model DLL present), so nothing is shipped for it and the look is bought with materials, fog and grading instead | `config/rtx.conf.dlss5` + `tools/rtxprofile.sh` (the `dlss5` action: a reversible probe, **not** a shipped profile), `tests/test_dlss5_probe_keys.py`; verdict and re-check: [docs/TARGET.md](docs/TARGET.md) §2/§6, FINDINGS (n19) |
| **Grading** | one tone curve for the whole frame instead of a per-region normalisation, with an exposure anchor that follows the sun, so the sun, chrome and wet patches can be bright instead of grey and 21:00 is not darkened by a noon number | `rtx.tonemappingMode = 0` + `rtx.tonemap.exposureBias = -0.5` in all three profiles (n16), and `[tonemap] day_ev → night_ev` driven off solar elevation at runtime (n22); **not yet seen in game** - the run and the abort ladder are in [docs/TARGET.md](docs/TARGET.md) §6) |
| **Materials** | the ~2190 materials the asset pack never gave PBR maps get a *measured* BRDF instead of the runtime's guess: `rtx.legacyMaterial.roughnessConstant = 0.65`, taken from the roughness of the pack's own maps (paving 0.58-0.65, per-map median 0.582) - and a census that proves no per-texture knob for them exists | `tools/pack_pbr_census.py` (coverage, decoded rough/metal/height, the uncovered-hash list) + the n18 block in all three profiles; **not yet seen in game** - [docs/TARGET.md](docs/TARGET.md) §6 |
| **Textures (n20 → n20b, rhY's MUST-HAVE #1)** | the 40 map surfaces that own the most screen in our own tour shots get real albedo + normal + roughness + height maps, generated from the game's own rasters (4x, deterministic - a photograph would erase the road markings) and installed as a Remix replacement layer on top of the pack. 35 sets, 140 DDS, **91.9 MB of VRAM** against the 1.08 MB the game spends on those materials today; the 5 the pack already covers at 2048²/4096² are skipped by design | `tools/textures.sh` (`on` / `off` / `status` / `generate`), `tools/pbrify.py` (the generator, `--calibrate`, `--report` = the VRAM delta), `tools/bcn.py` (our BC1/3/4/5 encoders - Pillow and ImageMagick cannot), `tools/texture_rank.py` (the screen-share ranking), `config/texture-pbr.json`; [docs/TEXTURES.md](docs/TEXTURES.md), FINDINGS (n20b); **not yet seen in game** - NEXT-TEST step 8 |
| **Looking at it** | an F12 screenshot hotkey that never touches D3D9, a 16-stop photo tour, and a reporter that turns the tour into numbers | `scripts/shot-watch.sh`, `plugin/lua/SARemix_Tour.lua`, `tools/tour-report.py` |
| **Safety** | every install step reversible, the plugin stops touching a dead Remix bridge, and the launcher ends the Wine tree if the game hangs on Quit | `apply.sh` backups, `plugin/src/shutdown.cpp`, `launcher/launch-rtx.sh` |

What is still rough is written down, not hidden: [docs/STATUS.md](docs/STATUS.md) lists every
reported finding with its status and the commit or task that owns it.

We also keep a quality target rather than a vibe: [docs/TARGET.md](docs/TARGET.md) names the reference
video we want to beat, measures the gap with `tools/target-gap.py` (histogram, highlight roll-off,
warmth, local contrast, haze, specular share), and says plainly which parts of that look come from
hardware we do not own - so the plan is about materials, fog and grading instead of a switch that
cannot run here.

## Requirements

| | |
|---|---|
| GPU | NVIDIA **RTX** (Remix is RTX-only). 8 GB VRAM works, 12 GB is comfortable. The launcher refuses to start with less than 6 GiB free and tells you which process is holding the card. |
| Driver | NVIDIA 580 or newer (tested 610). |
| OS | Any Linux with **Steam**. You need `Proton - Experimental` and `SteamLinuxRuntime_4`, both installed by Steam under Settings → Compatibility → Steam Play. |
| Game | **GTA San Andreas** on Steam (Step 1 downgrades it to 1.0 US). Roughly 10 GB free in the game folder for the Remix asset pack. |
| Packages | `git curl python3 cmake mingw-w64 unzip p7zip-full aria2 zenity` — plus `mangohud` if you want an FPS counter. The launcher builds the plugin on first run, so `cmake` and the MinGW-w64 i686 compilers are needed once. |

Debian/Ubuntu:

```
sudo apt install git curl python3 cmake mingw-w64 unzip p7zip-full aria2 zenity
```

## Step 1 — downgrade the game to 1.0 US

Every ASI and Lua mod in this stack needs the **1.0 US** executable: on the Steam 3.0 exe ModLoader
prints "unknown game" and the lighting mods never start.

1. Back the game folder up first — the downgrade replaces files:
   ```
   cp -a "<steam>/steamapps/common/GTASA" "<steam>/steamapps/common/GTASA-3.0-backup"
   ```
2. Download the **"RGSC/Steam to v1" SA downgrader** by Tomasak from
   <https://downgraders.rockstarvision.com/> (the same downgrader
   [GTASA-Remix](https://github.com/Hemry81/GTASA-Remix) recommends). It is `install.bat` +
   `jptch.exe` + `SA DOWNGRADE/*.diff`, and it runs under plain Wine with no display. Extract it
   into the game folder and either run the batch file, or patch file by file:
   ```
   cd "<game>"
   DISPLAY= wine jptch.exe gta-sa.exe "SA DOWNGRADE/<name>.diff" gta-sa.exe.down
   ```
   Watch the case of the file names: the `.bat` uses lower case and Linux does not ignore case.
   The result is the 1.0 US "compact" exe (5,189,632 bytes), `main.scm` 3,079,599, `script.img`
   581,632 — the installer checks nothing about these numbers, but if they are far off the
   downgrade did not take.
   Prefer a native Linux tool? [GTA SA Open Downgrader](https://github.com/xxanqw/gtasa-open-downgrader)
   (MIT, AppImage) converts the current Steam release to 1.0 US and has its own Large-Address-Aware
   switch.
3. Two rules after the downgrade: **never "Verify integrity of game files"** in Steam (it restores
   the 3.0 files and undoes the downgrade), and **do not load a Steam-version save** on a 1.0
   install — start a new game.

## Step 2 — run the installer (one command)

Put the two base packs in the game folder first (the installer checks for them and stops if they are
missing):

- [GTASA-Remix](https://github.com/Hemry81/GTASA-Remix) (Hemry, MIT) — copy its files into the game
  folder and follow its README's mod list (ModLoader, SilentPatch, Open Limit Adjuster, MixSets,
  Improved Streaming, Ped Spec, ImVehFt, MoonLoader 0.27 + Moon ImGui + MoonAdditions).
- The **RTX Remix 1.6.1** runtime,
  [`rtx-remix-for-x86-games-release-remix-1.6.1.zip`](https://github.com/NVIDIAGameWorks/rtx-remix/releases/download/remix-1.6.1/rtx-remix-for-x86-games-release-remix-1.6.1.zip)
  from [NVIDIA's releases](https://github.com/NVIDIAGameWorks/rtx-remix/releases) — replace the
  pack's `d3d9.dll` and `.trex/` with it. Remix 0.5.4 (the version the pack was authored on) crashes
  before the loading screen on a current driver under Proton; 1.6.1 works.

Then:

```
git clone https://github.com/crhy/rhYgtaSA.git
cd rhYgtaSA
./apply.sh "/path/to/your/GTASA-RTX"
```

`apply.sh` backs every file it touches **once** into `<game>/rhYgtaSA-backup/`, patches the pack for
Linux, sets **Large Address Aware** on `gta-sa.exe` (the downgrader's exe is not LAA, and that one
32-bit process also carries the Remix bridge client — see
[docs/VEGETATION.md](docs/VEGETATION.md)), removes the pack's duplicate lamp lights, installs the
researched water material (n13: `remix-ref`, together with the `rtx.conf` keys it needs), the helper
Lua modules, the Proton bridge setting and `launch-rtx.sh` into the game folder, and turns off
MixSets' real-time ped shadow (a gray square under CJ under Remix).

- Re-running it is a **no-op**: every patched file is dry-run forward and in reverse first, so a file
  is applied, skipped as already present, or — if you edited it — left alone with a warning that
  names it.
- **Water comes installed** (n13, rhY: "make sure we get the new water by default"). The default is
  `remix-ref` — the n07 research variant: transmittance (0.62, 0.8, 0.8) @ 4 m and a regenerated
  normal sheet (44° → 6° median facet tilt) — and the `rtx.conf` keys that go with it are written for
  you, with the previous bytes kept in `<game>/.rhys-water/`. Opt out with
  `./apply.sh "<game>" --water soft|realistic|calm|pack` or `--no-water`. Whatever you pick is
  recorded in `<game>/.rhys-water/state`, and a variant you chose on purpose is never overwritten by
  a later default run. `bash tools/water.sh status "<game>"` says which variant is live, who asked
  for it and which `rtx.conf` keys are still missing; `bash tools/water.sh pack "<game>"` is the
  byte-for-byte rollback.
- `./apply.sh "<game>" --with-deps` also fetches the mods you would otherwise download by hand
  (the character packs in [deps/manifest.json](deps/manifest.json), each with its sha256, author and
  licence). Our own hosts are not up yet, so today it prints the author's page for each pack instead
  of downloading it; nothing unverified ever reaches the game folder.
- What we host ourselves is built by `tools/make-release.sh` into `build/release`: one folder of archives,
  `SHA256SUMS`, one torrent for the whole folder and its magnet link — the info hash is re-derived from
  the bytes on disk before the magnet is printed, and only packs whose licence allows re-upload are ever
  collected. Nothing is staged yet, so it currently reports every pack as not staged
  ([docs/CHARACTERS.md](docs/CHARACTERS.md) section 9).

## Play

```
"<game>/launch-rtx.sh"        # run it from anywhere: it works out which install it is in
```

That is the copy `apply.sh` installed in your game folder. On every start it builds the current
plugin and installs it (`build/rhyremix.asi` → `<game>/scripts/`, and on a fresh clone it generates
the lamp and traffic-signal lists from your own `gta3.img`, `.ide`, `.ipl` and the pack's lamp
table), checks free VRAM, makes sure the default water variant is installed (a variant you chose
explicitly is left alone), starts Proton inside the Steam runtime with the settings Remix and a
DualShock need, and starts the F12 screenshot watcher. Then start a new game.

It finds the checkout it was installed from in `<game>/rhys-asi-repo` (a one-line file `apply.sh`
writes); if you move the checkout later, point the launcher at it with `RHYS_ASI_REPO`.

- Log files: `<game>/logs/launch-<timestamp>.log` (launcher) and `<game>/scripts/rhyremix.log`
  (plugin). The launcher prints which plugin file it is running and its md5, so "did my change
  reach the game?" is never a guess.
- Desktop icon:
  ```
  scripts/install-desktop-entry.sh "/path/to/your/GTASA-RTX"
  ```
  writes `~/.local/share/applications/gtasa-rtx.desktop` pointing at your paths (no sudo). It starts
  the same `launcher/launch-rtx.sh`, so a desktop click still builds and installs the current code,
  and a failure still reaches you as a desktop notification instead of a silent old build.
- Knobs, all optional, set before the command: `GTASA_RTX_DIR` (which install to run),
  `RHYS_ASI_REPO` (which checkout to build from), `GTASA_RTX_NO_DEPLOY=1` (run what is already
  installed), `RHYS_DRY_RUN=1` (deploy and report, do not start the game),
  `RHYS_WATER=remix-ref|soft|realistic|calm|pack` (install that water variant and record it as an
  explicit choice; **with it unset the launcher makes sure the default, `remix-ref`, is installed**,
  which is a no-op when it already is and never overwrites a variant you picked on purpose),
  `RHYS_RTX_KNOBS=1` (add the measured `rtx.conf` water line, with the value the live variant needs,
  backing the file up first),
  `RHY_SKIP_VRAM_CHECK=1`, `RHYS_SHOT_WATCH=0`, `RHYS_EXIT_GRACE=N` (seconds a game may outlive its
  window before the launcher ends the Wine prefix; `0` = watch off), `RHYS_BRIDGE_TIMEOUTS=1` (ask
  `.trex/bridge.conf` to keep the bridge's client timeouts on; see `docs/FINDINGS.md`).
- To run the repo copy of the launcher against another install:
  ```
  GTASA_RTX_DIR="/path/to/your/GTASA-RTX" bash launcher/launch-rtx.sh
  ```
- If the frame rate is the problem rather than the lighting, switch the renderer's startup
  settings wholesale instead of hand-editing `rtx.conf` (that file also carries the asset pack's
  texture-hash lists, and one mistyped 1246-character line breaks the pack):
  ```
  tools/rtxprofile.sh check                      # audit: does every key exist in this runtime?
  tools/rtxprofile.sh perf    "/path/to/GTASA-RTX"   # the 60 fps profile (config/rtx.conf.perf)
  tools/rtxprofile.sh nopops  "/path/to/GTASA-RTX"   # the temporal A/B (config/rtx.conf.nopops)
  tools/rtxprofile.sh dlss5   "/path/to/GTASA-RTX"   # the DLSS 5 probe (config/rtx.conf.dlss5) - not shipped
  tools/rtxprofile.sh current "/path/to/GTASA-RTX"   # this repo's shipped profile
  tools/rtxprofile.sh status  "/path/to/GTASA-RTX"
  tools/rtxprofile.sh restore "/path/to/GTASA-RTX"  # the exact bytes that were there before

  # n08: what the frame pacing actually was, from the MangoHud CSV the run wrote
  python3 tools/perf-report.py "$G/logs/perf/NvRemixBridge_<stamp>.csv" \
      --log "$G/scripts/rhyremix.log" --gpu-csv "$G/logs/perf/gpu-<stamp>.csv" --series --top 8
  # two CSVs and you also get an A/B delta table; --json for the machine-readable form.
  # MangoHud's gpu_vram_used / gpu_load / process_rss columns are constant for this process
  # (it samples the 32-bit bridge client), so VRAM comes from the gpu-*.csv nvidia-smi trace
  # (start one before the run: nvidia-smi --query-gpu=timestamp,memory.used,memory.total,
  #  utilization.gpu,temperature.gpu,power.draw,clocks.current.sm,clocks_event_reasons.active \
  #  --format=csv -l 1 > "$G/logs/perf/gpu-$(date +%Y%m%d-%H%M%S).csv" &)
  ```
  Every switch keeps `<game>/.rhys-rtxprofile/rtx.conf.prev` and one never-overwritten
  `rtx.conf.pristine`; `orig` puts back the runtime's own first-launch file. Evidence and the
  per-key reasoning: `.strata/notes-s22-60fps-budget.md`.
- If a setting you put in `rtx.conf` does nothing, that is a normal failure mode: the runtime keeps
  a key it does not recognise and ignores it. `tools/rtxkeycheck.py` only checks that the words
  appear in the renderer; `tools/rtx-option-audit.py` checks the **exact** name and prints the
  runtime's own default for it, against `config/rtx-options-1.6.1.tsv` (the option table the runtime
  generates for itself, fetched from the matching upstream tag). It found four names in our own files
  that were never options - see [docs/FINDINGS.md](docs/FINDINGS.md), "Dead keys". A second failure
  mode is subtler and n19 hit it head-on: an option's **category** name is also a string in the
  binary, so `rtx.dlssNeuralRendering = True` passes the strings audit and does nothing - the switch
  is `rtx.dlssNeuralRendering.enable`:
  ```
  python3 tools/rtx-option-audit.py config/rtx.conf.perf        # audit a profile
  python3 tools/rtx-option-audit.py --show rtx.tonemappingMode  # type + default of one key
  ```
- What an atmosphere block actually does to the air is arithmetic, and
  `tools/fog-budget.py` is that arithmetic transcribed from the 1.6.1 source: the fog-remap mapping
  (including the clamp the runtime does **not** apply), Beer-Lambert
  (`σ = −ln(linear(transmittanceColor)) / d_meas`) and Koschmieder (`visibility = 3.912 / σ`), with
  the unit scale (`100 × rtx.sceneScale`, so the bounds are SA metres). It turns six numbers into
  "10.3 km of air at noon, 3.4 km at the game's densest fog, and never more than 2 % of contrast lost
  inside 50 m" - which is how `config/rtx.conf`'s n17 block is defended without a screenshot:
  ```
  python3 tools/fog-budget.py config/rtx.conf
  python3 tools/fog-budget.py --bounds 400 4000 2 6 --color 0.6,0.6,0.6   # what-if
  ```
- What the asset pack actually covers is countable without running anything, and
  `tools/pack_pbr_census.py` counts it: every `mat_<HASH>` material in the pack's USD layers (it
  reads the binary crates as bytes, which plain `grep -r` silently skips), which of them have a
  `<HASH>_albedo/_normal/_rough/_height/_metal` map, and - decoded from mip 0, texel/255 being the
  perceptual roughness - what roughness the pack's own maps carry, per category. That is how the
  shipped legacy roughness in `config/rtx.conf` is defended without a screenshot, and
  `build/tmp/n18/pack-uncovered.txt` is the ranked work list for the offline PBR-ify pass:
  ```
  python3 tools/pack_pbr_census.py "<game>/rtx-remix/mods/gameReadyAssets" --json build/tmp/n18/census.json
  python3 tools/pack_pbr_census.py "<game>/rtx-remix/mods/gameReadyAssets" --no-decode   # structure only
  ```
- `nopops` is the other kind of switch: the same shipped profile with **only** the keys that
  decide *when* a lighting change lands (temporal reuse, accumulation windows, the TAA blend
  weight), and not one look key or texture list changed. Use it when the lighting still changes
  all at once while the plugin's own log says it faded: run `scripts/lighting-test.sh` once each
  way and compare the transitions, not the still shots.

## The lighting model, in one screen

Four rules, and they are the reason the pictures look the way they do:

1. **A light's radiance is fixed when it is created.** Remix has no "set brightness": a fade is a
   sequence of destroy + create. So the plugin sequences them - `plugin/src/smooth.cpp` owns the
   envelopes, the rate limiters and the per-frame budget for handle changes, and every floor is an
   ini key in `[fade]`.
2. **Distance decides which lamps exist; the camera never does.** There is no frustum test, no
   screen-size sort and no "N most visible lamps" anywhere in the plugin. The count cap may only
   trim the fade band beyond `max_distance`, and a traffic signal keeps its light by player
   distance because the game only reports the signals it is drawing. The other half of that is in
   the renderer: which lights a pixel is allowed to draw out of a ~400-light pool is
   `rtx.di.initialSampleCount`, `rtx.di.maxHistoryLength`, `rtx.di.permutationSamplingNthFrame`
   and the `rtx.antiCulling.*` keys in `config/rtx.conf`.
3. **Time runs on one clock.** The game's clock bytes offer a new minute every ~2000 ms and the
   sun moves 0.25 degrees per game minute, so reading them directly makes the sun a staircase; the
   two bytes next to them that look like a date are not one. `tod.cpp` publishes one continuous,
   self-calibrated game minute, and the sun and moon are recomputed every frame at it.
4. **Every light has a source you could point at.** No ambient fill, no fake glow: the ambient is
   the sky probe scaled by the daylight factor, the moon is one distant light, a signal's glow is
   solved for flux so it keeps its strength through the colour cycle, and a car's lamps come from
   the car's own light dummies. Where a renderer effect cannot be attributed to a source (the local
   tonemapper's view-dependent exposure, the fallback light, transmission through the sea) it is
   measured and written up in [docs/FINDINGS.md](docs/FINDINGS.md).

## Water variants

`bash tools/water.sh status "<game>"` says what is live, who asked for it and which `rtx.conf`
keys are missing. The default is **`remix-ref`**, installed by `apply.sh` and re-ensured by the
launcher; a variant you choose on purpose is recorded in `<game>/.rhys-water/state` and never
overwritten by a later default run.

| variant | what it is | when to use it |
|---|---|---|
| `remix-ref` | **the default.** The number set the other Remix projects ship (transmittance (0.62, 0.8, 0.8) @ 4 m, IOR 1.33, a 12 fps flipbook) over a regenerated wave sheet: median facet tilt 44° → 6° | unless you are A/B-testing |
| `soft` | the pack's material with a regenerated sheet at 12° - the least change that stops the speckle | if `remix-ref` reads too transparent |
| `realistic` | the pack's material, sheet softened to 20° | the old default |
| `calm` | sheet decimated further, for a flat sea | screenshot work |
| `pack` | the pack's own bytes, byte for byte | the rollback; also records an explicit choice so the default will not undo it |

The `rtx.conf` half of a variant - the animated-water keys and the two water texture hashes - is
installed with it by `tools/rtx_water_keys.py`, with `rtx.conf.pristine` and `rtx.conf.prev` kept
in `<game>/.rhys-water/`. Why the sea still flickers on some edges (307 exactly-butted coplanar
polygons, ~6000 seams, no hit tolerance in 1.6.1) is in [docs/FINDINGS.md](docs/FINDINGS.md).

## HD PBR textures for the surfaces you actually look at

```bash
bash tools/textures.sh status  "/path/to/GTASA-RTX"
bash tools/textures.sh on      "/path/to/GTASA-RTX"    # generates once into build/textures, then installs
bash tools/textures.sh off     "/path/to/GTASA-RTX"    # exactly the bytes it added are removed
```

`on` copies 35 texture sets (albedo / normal / roughness / height, BC3 + BC5 + BC4, full mip chains,
**91.9 MB**) and one USD layer into `<game>/rtx-remix/mods/gameReadyAssets/`, appending the layer as
the last `subLayers` entry of `mod.usda` so it composes over the pack's own materials. `off` deletes
exactly the files it listed in `…/gameReadyAssets/.rhys-textures/files.txt` and restores `mod.usda`
from the backup kept there - `scripts/textures-selftest.sh` hashes a whole fake install before and
after and requires it to come back byte-identical.

Two decisions worth knowing before you look at the result, both argued out in
[docs/TEXTURES.md](docs/TEXTURES.md): the **albedo is the game's own raster**, 4x upscaled, because
SA's road textures carry the road markings and the map UVs are authored for them (a photographic
replacement erases the crossings - and the neural upscaler route is what turns asphalt into wet
plastic); and the **5 surfaces the pack already covers at 2048²/4096² are skipped**, because our
layer composes last and would replace a photographic 2048² albedo with a derived 1024² one.
`--keep-covered` puts them back for an A/B. Which 40 surfaces, measured from 45831 map instances at
the four daytime tour cameras, and what that measurement cannot see (no occlusion, no per-triangle
material split in SA's geometry), is the same page; the roughness numbers are the pack's own
(`python3 tools/pbrify.py --game "$G" --calibrate`).

## Test flow

For a player, "test" means one command and one report:

```
bash scripts/test-session.sh --tour          # frees the card, runs the game and the 16-stop tour, then puts things back
python3 tools/tour-report.py "<game>" --since 21:30
bash scripts/diagnose.sh "<game>"            # reads the logs, changes nothing
```

That writes `<game>/screenshots/report-<start>/report.md` plus labelled contact sheets: mean luma,
p05/p50/p95, % clipped white, % black, saturation, and for the signal stops the red/amber/green
pixel fractions - each shot followed by the plugin log line nearest it. The step-by-step session
order, with what to look at and the rollback for each step, is
[docs/NEXT-TEST.md](docs/NEXT-TEST.md); what each open finding is waiting on is
[docs/STATUS.md](docs/STATUS.md). Everything the repo itself considers a gate is one command:

```
bash scripts/check-all.sh                    # build + native tests + python tests + every self-test
```

## Troubleshooting

**"Only N MiB of VRAM is free" (a desktop popup, and the game does not start).**
RTX Remix needs most of the card. With less than 6 GiB free the runtime shows a white window, never
reaches `remixapi_InitializeLibrary`, and dies with a CPU exception in the bridge — and every log
looks like a mod bug. The launcher names the processes holding the VRAM; close them, or set
`RHY_SKIP_VRAM_CHECK=1` if you know better.

**White window, black sea, or a white screen after the first frame.**

| what you see | what it is |
|---|---|
| white window that never initialises | VRAM (see above), or Proton started outside the Steam runtime — the launcher runs it inside `SteamLinuxRuntime_4`, which is also why there is audio |
| white window right after the first frame | Remix's new GUI input method under Wine; the patch sets `rtx.useNewGuiInputMethod = False` |
| sea renders as flat black/white | the pack's water normal sheet (44° median facet tilt). The default `remix-ref` variant replaces it with a regenerated one and installs the `rtx.conf` keys with it; `bash tools/water.sh status "<game>"` says what is live, `RHYS_RTX_KNOBS=1` adds `rtx.translucentMaterial.normalIntensity` with the value that variant needs (1.0 for a regenerated sheet, 0.35 for the pack's own) |
| game freezes on start, controller dead | the Wine `xinput` deadlock with a DualShock over hidraw; the launcher sets `PROTON_PREFER_SDL=1 PROTON_ENABLE_HIDRAW=0` |
| DLSS missing | NGX needs NVAPI; the launcher sets `PROTON_ENABLE_NVAPI=1`. Leave DLSS **frame generation** off — it hangs the bridge under Proton. |

**Screenshots: press F12, never Ctrl+PrintScreen.**
The game's own `Ctrl+PrintScreen` takes the Remix server down (CPU crash `0xe06d7363`). Instead, F12
in game writes a request that the watcher turns into a grab of the composited window — post-tonemap,
which is the image you are actually judging — and the pictures land in `<game>/screenshots/shots/`.
Nothing in that path touches D3D9. Change the key in `<game>/moonloader/SARemix_Shot.conf`
(`key=123` is F12), turn it off with `./deploy.sh --shotkey-off "<game>"`, or fire the same request
from a terminal with `bash scripts/shot.sh --trigger --game "<game>"`.

**The Remix menu.** `Alt+X` opens the RTX Remix menu in game (material and effect editing); the
Remix debug UI is `Alt+Y`. Under Wine the menu only responds because the patch sets
`rtx.useNewGuiInputMethod = False` — if the hotkeys do nothing, check that setting first.

**Lighting changes fade, they do not pop.** A Remix light's radiance is fixed at `CreateLight` time,
so "fading a street in" means a sequence of creates — and without a limit, that sequence is one
frame. The `[fade]` section of `rhyremix.ini` sets the floors: `in_ms`/`out_ms` how long a lamp takes
to come up or go down (never under 500 ms), `step` the largest brightness change one light may carry,
`creates_per_frame` how many handle changes a single frame may spend (destroys count too),
`player_grace_ms` how long the last known player position is held when the game's ped pointer reads 0
before the layers fade out instead of being destroyed, and `sky_rel_per_sec`/`sun_rel_per_sec`/
`sun_deg_per_sec` the rate limits on the ambient, exposure, sun radiance and sun direction. The
numbers are asserted in `tests/smooth_check.cpp` (`bash scripts/check-all.sh` runs it): no lighting
value the plugin publishes may move by more than a small amount in one frame, whatever the game clock
or the renderer does underneath. See `docs/FINDINGS.md`, "Lighting changed instantly, everywhere at
once".

**The exposure anchor follows the sun, not the clock.** With the tonemapper in Global mode (n16) the
exposure of the whole frame is one number, `2^(rtx.tonemap.exposureBias + userEVBias)`, and a number
in `rtx.conf` cannot be -0.5 at noon and +0.5 at 21:00 - measured, the noon shots sit at p50 123-170
while the shots the report called good at night sit at p50 7-17 with 34-52 % of their pixels black.
So the plugin pushes that same option from the sun's **elevation** (`plugin/src/exposure.cpp`,
pushed by `sun.cpp` next to `rtx.skyBrightness`): `[tonemap] day_ev = -0.5` at full daylight -
deliberately the same number the profiles carry, so noon is untouched and only the night end moves -
up to `[tonemap] night_ev = +0.5` at night, across the same twilight band as `[sky]`, at most
`max_ev_per_sec = 0.05` EV per second (the full swing takes >= 20 s: an exposure change that is one
frame wide is the whole screen brightening). It never writes the Remix UI's Brightness slider
(`rtx.userBrightness*`: user settings, and `user.conf` outranks `rtx.conf`) and never switches
`rtx.tonemappingMode` at runtime. Undo is `[tonemap] apply=0` (the last value written stays in the
runtime's user layer, which outlives the run — the Remix menu's Global Exposure control sets it back;
docs/TARGET.md §6), and the knob to argue with after a live run is `night_ev`. Asserted in `tests/exposure_check.cpp` (equal elevation gives equal EV, noon
equals the profile's anchor, a whole simulated day stays inside the rate bound) and
`tests/smooth_check.cpp`; see `docs/FINDINGS.md`, "One exposure anchor cannot serve noon and 21:00".

**A lamp in the streamed area is on, and the camera does not get a vote.** Selection is by distance
from CJ, never by what the screen is pointed at: there is no frustum test, no screen-size sort and
no "N most visible lamps" anywhere in the plugin. Two rules keep that true. The count is a budget
for the fade band, not a distance: a lamp inside `[streetlights] max_distance` is never the one
`max_lights` drops, whatever the density around you (the worst point on the real 7002-lamp map has
408 lamps within 300 units and 552 within 380, so the shipped `max_lights=560` + `sticky_extra=140`
covers the map and the plugin prints the worst case it finds at load). And a traffic signal keeps its
light by distance too: the game only registers a signal corona for what it draws, so "the corona
stopped" used to mean "dark two seconds later" when you turned your back on a junction. The log
proves both: `in_drop=0` in every 10-second line means no lamp inside the radius was ever left
without a light, and `tests/always_on_check.cpp` runs a 360-degree camera sweep, a 300 s walk and a
240 s drive through the measured worst-case density and asserts the light set does not notice. The
other half of that finding is in the renderer, not the plugin - which lights a pixel is allowed to
draw out of a 400-light pool is `rtx.di.initialSampleCount`, `rtx.di.maxHistoryLength`,
`rtx.di.permutationSamplingNthFrame` and the `rtx.antiCulling.*` keys in `config/rtx.conf`. See
`docs/FINDINGS.md`, "Lights that came and went with the camera".

**Time of day runs on a continuous clock.** The game's clock bytes only offer a new minute every
~2000 ms, and the sun moves 0.25 degrees per game minute, so reading those bytes directly makes the
sun a staircase; and the two bytes next to the clock that look like a date are not one (the "month"
byte is a counter). `plugin/src/tod.cpp` turns the bytes into one continuous, never-backwards game
minute - interpolated between ticks, self-calibrated to the tick length this machine actually runs
at, with torn hour/minute reads repaired and a script pinning the clock (`SARemix_Tour.lua` does it
twice per stop) absorbed as noise instead of walked backwards - and the sun and moon are then
recomputed every frame at that minute instead of once a second at a whole minute. The season comes
from `[sun] day_of_year` unless the game's date proves it is a date (`[clock] calendar_stable_reads`
identical reads, one per second). `[clock]` holds `ms_per_game_minute`, `jitter_tolerance_min`,
`reanchor_ms` and `calendar_stable_reads`; `[sky] cloud` (0 = clear) and the `*_per_sec` follow rates
make a weather change a ramp. The bounds are asserted in `tests/tod_check.cpp`, which replays a whole
simulated 24 h day at 60 fps through the old sampling and the new one: the sun used to move up to
3566 degrees per game minute between frames, now 0.21 (the real sky is 0.25). See `docs/FINDINGS.md`,
"Time of day flickered: two calendars, one staircase".

**Too bright, lamps dark, or lamps blink.** First find out whether it is ours at all:

```
bash scripts/abtest-lights.sh lights off "/path/to/your/GTASA-RTX"   # our lights off: a blowout that stays is the asset pack
bash scripts/abtest-lights.sh status   "/path/to/your/GTASA-RTX"     # what is currently set
bash scripts/diagnose.sh               "/path/to/your/GTASA-RTX"
```

`diagnose.sh` reads the plugin log, the bridge log and the deployed ini and prints the verdict:
`active > 0, drawn = 0` means the lights exist in the plugin and nowhere else. The brightness story
(one game unit ≈ one metre, `rtx.sceneScale`, fixture size vs output, why the street lights blinked)
and every ini knob are in [docs/DEVELOPING.md](docs/DEVELOPING.md); what to photograph and which knob
to turn is [docs/TESTING.md](docs/TESTING.md).

**Something else looks broken.** [docs/FINDINGS.md](docs/FINDINGS.md) is the write-up with logs and
dead ends, [docs/MODS-REPLACED.md](docs/MODS-REPLACED.md) says which mod does what and which of them
our plugin replaces, and
[docs/notes/linux-modding-notes.md](docs/notes/linux-modding-notes.md) covers Proton, the downgrade,
controllers and 60 FPS on Linux generally.

## Uninstall / rollback

Everything the installer did is reversible, and it never deletes anything.

```
cp -a "<game>/rhYgtaSA-backup/." "<game>/"          # the pristine pre-patch copies back
./deploy.sh --lua-on --sun-on "<game>"              # the Lua light manager and sun mod back on
scripts/tidy-game.sh --restore --apply "<game>"    # undo a tidy-game.sh move (dry run: drop --restore --apply)
python3 tools/laa.py "<game>/gta-sa.exe" --clear    # clear Large Address Aware again
rm "<game>/scripts/rhyremix.asi" "<game>/scripts/rhyremix.ini"   # our plugin only
```

`rhYgtaSA-backup/` is filled **once**, so it still holds the untouched pre-patch files after any
number of re-runs; `gta-sa.exe.noLAA-backup` is the exe before the LAA bit. Packs fetched with
`--with-deps` live in their own `<game>/modloader/<pack>/` folder — delete that folder. To go back to
the store version of the game, restore the Step 1 backup (or "Verify integrity", which also restores
the 3.0 files and breaks every mod).

To tidy instead of undo — park what nothing loads any more (disabled Lua scripts, an old capture,
stale logs) without deleting it:

```
scripts/tidy-game.sh "<game>"                    # print the plan (dry run is the default)
scripts/tidy-game.sh --apply "<game>"            # move it to <game>/OLD/<timestamp>/
```

`OLD/` is inert: MoonLoader, ModLoader and the Remix bridge never scan it. The exact move/keep list
is in [docs/MODS-REPLACED.md](docs/MODS-REPLACED.md).

## Credits

The game, the renderer and the mods belong to other people; this repo is the glue. Everything we use
or fetch, with its author:

| what | who | where |
|---|---|---|
| GTA San Andreas | Rockstar Games | not distributed here |
| **GTASA-Remix** — the Remix asset pack this all sits on | Hemry (Hemry81) and contributors, MIT | <https://github.com/Hemry81/GTASA-Remix> |
| **RTX Remix** 1.6.1 runtime | NVIDIA | <https://github.com/NVIDIAGameWorks/rtx-remix/releases> |
| **SA downgrader** (Steam/RGSC → 1.0 US) | Tomasak | <https://downgraders.rockstarvision.com/> |
| GTA SA Open Downgrader (Linux alternative) | xxanqw, MIT | <https://github.com/xxanqw/gtasa-open-downgrader> |
| ModLoader, GInput, Open Limit Adjuster, Improved Streaming, Ped Spec, PedFuncs, Restore Unused Peds, Essentials Pack, Proper Fixes | Junior_Djjr (MixMods) | <https://www.mixmods.com.br/2018/01/modloader/> · <https://www.mixmods.com.br/2021/01/iii-vc-sa-ginput-melhor-suporte-para-controles/> · <https://www.mixmods.com.br/2015/02/open-limit-adjuster/> · <https://www.mixmods.com.br/2022/04/improved-streaming/> · <https://www.mixmods.com.br/2015/02/ped-spec-iluminacao-specular-nas-pessoas-como-no-mobile/> · <https://www.mixmods.com.br/2022/07/pedfuncs/> · <https://www.mixmods.com.br/2022/07/sa-restore-unused-peds/> |
| SilentPatch | ThirteenAG (page: MixMods) | <https://www.mixmods.com.br/2026/08/sa-silentpatch/> |
| MixSets | L4mp3 (page: MixMods) | <https://www.mixmods.com.br/2022/03/sa-mixsets/> |
| ImVehFt (improved vehicle features) | Zarrr (page: MixMods) | <https://www.mixmods.com.br/2020/01/imvehft-improved-vehicle-features/> |
| MoonLoader, Moon ImGui, MoonAdditions, Framerate Vigilante | FYP, BlastHack | <https://www.mixmods.com.br/2020/10/moonloader/> · <https://www.mixmods.com.br/2019/08/imgui-interface-grafica-para-moonloader/> · <https://www.mixmods.com.br/2017/08/moonadditions/> · <https://www.mixmods.com.br/2022/08/iii-vc-sa-framerate-vigilante/> |
| Proper Characters / Proper Player retex, Restore Unused Peds (art) | Jessica Natalia, Junior_Djjr, OjTrue, Zeneric, Jao, Francois Declasse, falsenail | <https://www.mixmods.com.br/2022/07/sa-proper-characters-retex-hd-peds-remaster/> · <https://www.mixmods.com.br/2024/12/proper-player-retex/> |
| Emergency Peds Vary, Criminal Peds Vary | wizvich, Muks, FCVP71, SethShaddix, Area 69, Thiago555aug | <https://libertycity.net/files/gta-san-andreas/246900-emergency-peds-vary.html> · <https://libertycity.net/files/gta-san-andreas/240761-criminal-peds-vary.html> |
| Ultimate ASI Loader | ThirteenAG | <https://github.com/ThirteenAG/Ultimate-ASI-Loader> |
| Proton (Windows compatibility layer we launch through) | Valve | <https://github.com/ValveSoftware/Proton> |
| MangoHud (optional FPS overlay) | flightlessmango and contributors | <https://github.com/flightlessmango/MangoHud> |

MixMods and LibertyCity files are free to use but **not** to re-upload, so they are linked, never
mirrored — [deps/manifest.json](deps/manifest.json) records the licence of every one of them.
Our fixes are MIT licensed; patched GTASA-Remix files remain © Hemry under MIT — see
[THIRD_PARTY.md](THIRD_PARTY.md).

## Working on the plugin

[docs/DEVELOPING.md](docs/DEVELOPING.md) has the build, the `deploy.sh` options, the fix-by-fix table,
the light units correction, the sun/moon internals and the open problems. The gate is
`bash scripts/check.sh <phase>`; the user guide itself is checked by `bash scripts/check-readme.sh`.
