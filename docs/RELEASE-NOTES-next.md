# Release notes — next build

For **players**. This is what is new since the last build you ran, what we would like you to test,
what is still known to be rough, and how to send back what you saw. Nothing here asks you to read
the source; the commands are the same two steps as in the [README](../README.md).

The full change history, release by release, is in [CHANGELOG.md](../CHANGELOG.md). The technical
detail behind any line below is in [docs/DEVELOPING.md](DEVELOPING.md) and
[docs/FINDINGS.md](FINDINGS.md).

---

## What you need

Unchanged: **GTA San Andreas 1.0 US** (Step 1 of the README), **GTASA-Remix** and **RTX Remix 1.6.1**
in the game folder, an **RTX** card with about 8 GiB free, NVIDIA driver 580 or newer, and
`Proton - Experimental` + `SteamLinuxRuntime_4` in Steam. Updating an existing install is one command
in the checkout:

```
./apply.sh "<game>"        # re-running it is a no-op; it names any file it could not patch
"<game>/launch-rtx.sh"     # builds the plugin, installs it, then starts the game
```

If something in the update looks wrong, everything is reversible
([README: Uninstall / rollback](../README.md#uninstall--rollback)).

---

## What's new

### Street lamps

- **Each lamp is lit once.** The asset pack's lamp models carried their own light prims (18 of them
  across the 12 lamp models) on top of our lamp list, so every street lamp was lit twice and night
  read about twice as bright. The installer now removes the pack's copies and keeps the original as
  `.orig-with-lights`; our plugin lights the lamps. It takes effect on the launch after an install,
  because Remix reads the assets at start-up.
- **Real fixture sizes.** A lamp's glowing sphere used to be 15 game units — about a **30 metre**
  ball for a housing the size of a lunchbox, which is why light pools read as shapeless haze and
  volumetrics gave a fog wall instead of shafts. It is now the size of the real luminaire
  (≈ 0.2 units for a cobra head), with the output scaled up by the same area factor so the street
  does not go dark. Expect **defined pools with a visible falloff**, and expect night to read a
  little brighter at the same settings.
- **Sodium looks like sodium.** Street lamps are gas-discharge lamps, and a discharge does not have
  a colour temperature that draws the right picture: the old 2100 K setting put a third of the light
  in the deep red, so sodium read as a dim incandescent bulb. Warm lamps are now built from the real
  sodium emission lines (yellow-orange, no red halo, no blue); cool lamps from mercury lines.
- **Lamps switch at twilight, not at a clock time** — the sun's elevation, which is what a real
  photocell reads.

### Sun, moon and sky

- The sun and moon are now computed from real astronomy for Los Santos' latitude and the in-game
  date: sunrise and sunset match the US Naval Observatory tables to about three minutes, and the red
  of a low sun comes from the path length through the atmosphere rather than from a tuned orange.
- The moon has a **phase** (it was permanently full) and is placed by lunar position, so a midsummer
  full moon sits at a believable height instead of near the zenith.
- **Summer time**: a July sunset now lands near 20:05 game time instead of 19:05.
- The sky's brightness now follows the sun at runtime, so night is actually dark and lamps can
  compete with it. Two `rtx.conf` values were changed and nothing else: `rtx.emissiveIntensity`
  10 → 1.0 (shopfront windows were being rendered at 30000) and `rtx.skyBrightness` 3 → 1.0.

### Traffic signals

- **The light now sits on the signal head, in the colour the game is showing.** The plugin reads the
  head position and aspect colour from the game's own drawing code instead of guessing from the pole
  position, so on a mast-arm signal the head hanging out over the road is the one that glows, and
  red is red, green is distinctly blue-green, amber sits between.
- **Lens colours are real glass**, integrated across the actual red / amber / green passbands —
  a single "red" wavelength such as 620 nm still carries green, which is how the old reds came out
  orange.
- A head the game stops drawing keeps its light for two seconds, so one dropped frame is not a dark
  intersection.
- **A safety switch was added in the newest build (not yet played-tested — see Known issues).** The
  patch is only installed when the game's code at the patch site is exactly what we expect; otherwise
  the plugin logs why and falls back to the old pole-position mode. `source=list` puts the old mode
  back by hand at any time.

### Water

- Three switchable water materials instead of the pack's one: `tools/water.sh realistic` (open
  coast, real seawater absorption), `calm` (bay / harbour, gentle), `pack` (the pack's original,
  always a safe way back). `tools/water.sh status` says which is live.
- Measured, not guessed: the pack's water normal sheet has a **median facet tilt of 46°**, which is
  why the sea reads as binary black/white. The USD variants help; the change that actually kills it
  is one line in `rtx.conf`, and `RHYS_RTX_KNOBS=1` adds it (backed up first):
  `rtx.translucentMaterial.normalIntensity = 0.35`.

### Mirrors, characters, vegetation

- **Rear-view mirrors really reflect.** SA paints its mirrors — no stock car has a mirror surface —
  so a Remix material (metallic 1, roughness 0.02) is applied to the mirror texture by hash. The
  hashes are now computed offline by `tools/remix_texhash.py`, so this no longer needs you to read a
  hash out of the running game. `tools/mirrors.py --dry-run` reports what it would change.
- **Character packs are one switchable step**: `tools/characters.sh on|off|status` installs the
  recommended set in the right order and removes exactly what it added. The doc lists which of the
  pack's CJ material overrides stop applying after a CJ retex and how to re-point them.
- **Vegetation is one switchable step too**: `tools/vegetation.sh on|off|status [--step N]` installs
  "V Vegetation Mix" with the fixes for the freeze it caused, one ranked step at a time.

### Install, launch, and getting the card back

- **The launcher refuses to start when the GPU is already full** and tells you which process holds
  the card (a desktop dialog when you start from an icon). Remix renders a white window and then
  crashes in a way that looks exactly like a mod bug; that cost days before it became a check.
- **One command for a test session** when something else is holding the GPU:
  `bash scripts/test-session.sh --tour` frees the card, runs the game, cleans up afterwards, and
  brings the other program back.
- **Large Address Aware** is set on `gta-sa.exe` by the installer (the 32-bit process carries the
  game, the Remix bridge client and the streaming cap at once); the launcher warns if it is missing.
- **A desktop icon**: `scripts/install-desktop-entry.sh "<game>"` — it runs the same launcher, so a
  click still builds and installs the current code and still tells you when it failed.
- **F12 takes the screenshot** (never `Ctrl+PrintScreen`, which takes the renderer down). Fixed in
  this cycle: the hotkey was looking in the wrong folder and reading the key code in the wrong
  namespace, so every F12 silently did nothing.

---

## What to test

The short version: **run the tour, then send the shots and the log.**

```
bash scripts/test-session.sh --tour          # or: start the game and press F12 where you like
```

With `--tour`, the game walks 16 fixed photo stops (a noon baseline at Grove Street, then under a lamp at 21:00, the same
street 30 m away, a sodium lamp close up, a tungsten shopfront, a Burger Shot, two traffic-signal
views, sunset facing east and west, dawn, the sea at Santa Maria at noon and at night, a car's
rear-view mirror, a pedestrian crowd in Pershing Square, the Vinewood hillside) and grabs one shot
per stop. The names land in `<game>/screenshots/tour-manifest.txt`, the pictures in
`<game>/screenshots/shots/`.

Before looking at the pictures, check three lines in `<game>/scripts/rhyremix.log`:

```
remix: remixapi_InitializeLibrary v0.6.5 -> 0
config: fixture geometry real luminaire sizes (street big radius 15.00 -> 0.20 game units, ...)
signalhook: patched 0x49DD4C (was 'call 0x6FC580') -> ...
```

No `remixapi_InitializeLibrary` line means the renderer never came up — that is an environment
problem (VRAM, leftover processes), not a lighting problem. No `signalhook: patched` line means the
signals are running in the old mode, and the reason is in the line next to it.

What each shot should prove, and the knobs to A/B if it does not, are in
[docs/TESTING.md](TESTING.md). The short list of what we most want to see:

1. **A defined pool of light under a lamp**, with the pole separable from the ground under it — and
   separate pools, not one continuous wash, when you move 30 m down the street.
2. **Sodium lamps yellow-orange with no red halo**, visibly different from a tungsten shopfront.
3. **Every signal head lit where it is**, in the colour the game is showing, from the close view *and*
   from the cross street.
4. **The traffic-signal hook counters** in the log:
   `hook: calls=… records=… dropped=… bad=… reentrant=…` — `reentrant` must stay 0, a rising `bad`
   is a symptom to report rather than a reason to stop playing.
5. **The mirror shot**: does the mirror show the road that is actually behind the car? If it frames an
   empty curb, that stop's coordinates are the ones to nudge — parked cars are placed by the compiled
   script, so their position is not readable from the game's data files.
6. **Water**: try `tools/water.sh realistic` first, then `calm`, then `pack`.

---

## Known issues

- **The traffic-signal hook has not been tested in game yet.** It patches a game instruction, and the
  two earliest builds crashed the game a few seconds after the level loaded; both causes were found
  and fixed, and the newest build adds a kill switch that refuses to patch anything it does not
  recognise, plus counters that make a bad run diagnosable. It is still the first thing to suspect if
  the game dies shortly after entering the level, and the way to take it out is one line:
  `[trafficlights] source=list` in `<game>/scripts/rhyremix.ini` (live, no restart).
- **The vegetation pack is still pending.** "V Vegetation Mix" froze the RTX copy; the three ranked
  causes are written up in [docs/VEGETATION.md](VEGETATION.md) and the fixes are now switchable
  steps, but no combination has been confirmed working in game yet. Install one step per run, and
  expect the heaviest step (4) to leave some trees without textures on purpose.
- **Water variants are a material change, not a fix.** The three variants change the material inputs;
  the black/white surface comes from the pack's normal sheet, and only the `rtx.conf`
  `normalIntensity` line (or a regenerated normal map) addresses it. The world-space size of one
  frame of the animated sheet is still unmeasured, so the ripples may also be too small, not only
  too steep.
- **Signals are unshaped on purpose.** A real signal module beams down the approach, but our signal
  list carries no heading; a wrongly-aimed cone would put a dark face on every signal seen from the
  cross street, which is worse than an unshaped point.
- **Some lamp posts are dark by design.** The lamp table the pack ships has no entry for a dozen
  one-off airport / roof / strip props; `python3 tools/lamp_list.py --report-missing` lists them. If
  a lamp post near a `lamppost1` / `lamppost2` instance is dark, that is the reason.
- **The screenshot crash is mitigated, not root-caused.** The plugin blocks the game's gamma-ramp call
  that used to precede the renderer's CPU crash, and F12 grabs the composited window instead of going
  through D3D9. If a crash still happens with `block_gamma=1`, it is in the bridge, and we want the
  log.
- **Leave DLSS frame generation off** — it hangs the bridge under Proton. DLSS itself is on.
- **Never "Verify integrity of game files"** in Steam after the downgrade: it restores the 3.0 files
  and breaks every mod in this stack. And do not load a Steam-version save on a 1.0 install.
- **25 textures referenced by the asset pack are missing from its repository** — a few surfaces will
  stay plain until the pack fixes them upstream.

---

## How to report

Three things, in this order. They are what makes the next round fast: the log contains the exact
radius, radiance and cone angle every light was created with, so a picture plus that log is enough
to diagnose without guessing.

1. **The shots.** Press **F12** in game (not `Ctrl+PrintScreen`). They land in
   `<game>/screenshots/shots/`. Name them by what you saw ("06-signals-close", or "signal red looks
   orange"), and include one shot of the thing that looks right too — a comparison is worth more
   than a complaint.
2. **The plugin log**, `<game>/scripts/rhyremix.log` — the whole file, not an excerpt. It starts each
   run with the config it actually loaded, and every 10 s prints the light counts and the hook
   counters. If the run crashed, also send `<game>/rtx-remix/logs/bridge64.log` and, if present,
   `<game>/rtx-remix/logs/crash-upload.log`.
3. **One paragraph of context**: what you changed (which ini line, which water variant, which pack
   step), what you expected, what you got, and whether `scripts/abtest-lights.sh lights off` changes
   it — that last one separates "our plugin" from "the asset pack" in one run.

Then send it the way you already talk to us: an issue on <https://github.com/crhy/rhYgtaSA>, or the
same channel you send screenshots to. Please do **not** attach the game folder or the save: nothing in a report
needs them, and the log names the install path anyway.

Useful one-liners before you send:

```
bash scripts/diagnose.sh "<game>"                 # verdict from the plugin log, bridge log and ini
bash scripts/abtest-lights.sh status "<game>"     # what is currently set
bash scripts/abtest-lights.sh lights off "<game>" # our lights off: a blowout that stays is the pack
```

---

## Rollback

```
cp -a "<game>/rhYgtaSA-backup/." "<game>/"        # pristine pre-patch copies back
tools/water.sh pack "<game>"                      # the pack's water material
tools/characters.sh off "<game>"                  # character packs out again
tools/vegetation.sh off "<game>"                  # vegetation pack out, every edited byte back
tools/mirrors.py --remove "<game>"                # mirror material out
rm "<game>/scripts/rhyremix.asi"                  # our plugin only
```

`rhYgtaSA-backup/` is filled once, so it still holds the untouched files after any number of
re-runs.
