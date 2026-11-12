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
helper Lua modules, the Proton bridge setting and `launch-rtx.sh` into the game folder, and turns off
MixSets' real-time ped shadow (a gray square under CJ under Remix).

- Re-running it is a **no-op**: every patched file is dry-run forward and in reverse first, so a file
  is applied, skipped as already present, or — if you edited it — left alone with a warning that
  names it.
- `./apply.sh "<game>" --with-deps` also fetches the mods you would otherwise download by hand
  (the character packs in [deps/manifest.json](deps/manifest.json), each with its sha256, author and
  licence). Our own hosts are not up yet, so today it prints the author's page for each pack instead
  of downloading it; nothing unverified ever reaches the game folder.
- What we host ourselves is built by `tools/make-release.sh build/release`: one folder of archives,
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
table), checks free VRAM, starts Proton inside the Steam runtime with the settings Remix and a
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
  installed), `RHYS_DRY_RUN=1` (deploy and report, do not start the game), `RHYS_WATER=calm|realistic|pack`,
  `RHYS_RTX_KNOBS=1` (add the measured `rtx.conf` water line, backing it up first),
  `RHY_SKIP_VRAM_CHECK=1`, `RHYS_SHOT_WATCH=0`.
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
  tools/rtxprofile.sh current "/path/to/GTASA-RTX"   # this repo's shipped profile
  tools/rtxprofile.sh status  "/path/to/GTASA-RTX"
  tools/rtxprofile.sh restore "/path/to/GTASA-RTX"  # the exact bytes that were there before
  ```
  Every switch keeps `<game>/.rhys-rtxprofile/rtx.conf.prev` and one never-overwritten
  `rtx.conf.pristine`; `orig` puts back the runtime's own first-launch file. Evidence and the
  per-key reasoning: `.strata/notes-s22-60fps-budget.md`.
- `nopops` is the other kind of switch: the same shipped profile with **only** the keys that
  decide *when* a lighting change lands (temporal reuse, accumulation windows, the TAA blend
  weight), and not one look key or texture list changed. Use it when the lighting still changes
  all at once while the plugin's own log says it faded: run `scripts/lighting-test.sh` once each
  way and compare the transitions, not the still shots.

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
| sea renders as flat black/white | the pack's water normal sheet; `RHYS_RTX_KNOBS=1` adds the measured `rtx.translucentMaterial.normalIntensity = 0.35` |
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
