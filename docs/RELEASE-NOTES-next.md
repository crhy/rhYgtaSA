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
- **A lamp that is in range is on, whichever way you happen to be looking.** Two things used to work
  against that. The per-layer light budget was a fixed count, so in a dense district (the worst spot
  on the map has 408 lamps within 300 m of you) the farthest lamps lost their light while they were
  still 200 m away and got it back a moment later — a street light blinking for no reason. The budget
  now only trims the far edge of the fade band, and the shipped counts cover the whole map. And traffic
  signals went dark when you turned your back on them, because the game only reports a signal that it
  is drawing: a junction you walked away from lost its bulbs and glows two seconds later and got them
  back when you looked again. Signals now follow your distance, like street lamps. What to notice:
  spinning the camera at night should change nothing about which lamps are lit. If a lamp ever goes out
  while it is still in front of you, the plugin log says so — `in_drop=` in the 10-second
  `streetlights:` line must stay 0.
- **Less boiling when you rotate the camera.** Whether a given pixel draws a given lamp out of the
  several hundred that are lit is a random choice the renderer makes every frame, and the shipped
  settings re-shuffled that choice every frame with only four candidates per pixel. With the lamp pool
  now covering the whole streamed area, the profile gives it eight candidates, three times the memory
  of the previous choice, and stops the re-shuffle. Expect the light from a lamp to hold steady while
  you turn, instead of crawling. If your frame rate matters more, `tools/rtxprofile.sh perf` is the
  cheaper variant of the same settings.

### Sun, moon and sky

- The sun and moon are now computed from real astronomy for Los Santos' latitude and the season:
  sunrise and sunset match the US Naval Observatory tables to about three minutes, and the red
  of a low sun comes from the path length through the atmosphere rather than from a tuned orange.
- **Time of day runs smoothly.** The clock in the game only offers a new minute every two seconds,
  and the two bytes next to it that look like a date are not one, so the sun used to step by a
  quarter of a degree every two seconds — and every so often jump 20 to 40 degrees and straight
  back, because the plugin was alternating between the real season and a garbage one. It now runs on
  one continuous clock: the sun moves the way the sky moves, the season stays the same all session,
  and a script that writes the clock (the tour mod does it twice per stop) can no longer make time
  run backwards.
- **A tour no longer breaks the sky.** While the tour mod pins the clock at a stop, the sun holds
  still instead of wobbling; when the tour deliberately jumps two hours ahead, the world changes
  over about a second rather than one frame.
- The moon has a **phase** (it was permanently full) and is placed by lunar position, so a midsummer
  full moon sits at a believable height instead of near the zenith.
- **Summer time**: a July sunset now lands near 20:05 game time instead of 19:05.
- **Shadow edges creep instead of jumping.** A sun that moves has to be re-made in the renderer,
  and until now that happened once the shadow was already a whole degree out of date — twice the
  width of the sun's own disc — every eight seconds or so. It now happens on a schedule, four times
  finer, and never faster than a fixed ceiling no matter what the game clock does. With the clock
  pinned, the sun is not re-made at all.
- The renderer has settings of its own that decide **when** a lighting change lands rather than what
  it looks like. There is now a switch for exactly those, and it changes nothing else — not the
  grade, not the exposure, not one texture of the asset pack:
  `tools/rtxprofile.sh nopops "<game>"`, back with `restore`.
- The sky's brightness now follows the sun at runtime, so night is actually dark and lamps can
  compete with it. Two `rtx.conf` values were changed and nothing else: `rtx.emissiveIntensity`
  10 → 1.0 (shopfront windows were being rendered at 30000) and `rtx.skyBrightness` 3 → 1.0.
- **The brightness of the whole picture now follows the sun too.** The new grading profile fixes the
  renderer's exposure at one number for the whole frame — good at noon, wrong at night, because the
  file that carries it cannot know what time it is. The plugin now drives that same number from the
  sun's height: the noon value is unchanged (still −0.5 EV, so the daytime look is exactly what the
  grading change was meant to test), and at 21:00 the frame gets twice the light it would otherwise
  have had, arriving over at least twenty seconds rather than in one frame. If night now reads too
  bright or too dark, the knob is `[tonemap] night_ev` in `rhyremix.ini` (edit it while the game
  runs); `[tonemap] apply=0` stops the plugin writing it — and if an earlier run already wrote a
  value, the Remix menu's own **Global Exposure** control sets it back, because that is the same
  place the plugin writes. **We have not seen this in game yet** —
  the numbers behind it are measurements of the old frames, and the first live run is what tells us
  whether the night shots got better.
- **Distance is allowed to fade.** San Andreas draws its own fog (a start distance and an end
  distance per hour and weather, in `data/timecyc.dat`), but with volumetrics on the renderer throws
  that fog away and replaces it with its own air — which had never been configured, so the air was
  344 km clear and the whole map sat there crisp to the horizon. The profiles now hand that fog to
  the volumetric system: the game's own end distances (400–4000 m) set how thick the air is, which
  comes out at **10.3 km of visibility on a clear day and 3.4 km when the game asks for fog** — the
  skyline loses a third to a half of its contrast while the pavement under your feet keeps 98 % of
  its. Two honest warnings. **We have not seen this in game yet**, and the arithmetic says the haze
  will be milder than the reference video's unless we make the air denser. And the same haze darkens
  the sky a little, because in this renderer the haze only glows within about 20 m of you — so if the
  sky goes dull, that is this change, and the knob is the density
  (`fogRemapTransmittanceMeasurementDistanceMaxMeters`, 6 → 12 → 20 to back it off, 4 → 3 → 2 to
  lean in), not a colour key. `python3 tools/fog-budget.py config/rtx.conf` prints exactly what a
  number does before you try it.

### Materials

- **The materials the asset pack never redrew now have a measured roughness.** Counting the pack
  read-only (`tools/pack_pbr_census.py`) says it defines **2363** materials and ships a replacement
  colour texture for **174** of them - and the renderer's own source shows that everything else is
  drawn with one constant roughness for the whole game, with no way to give it a roughness map at
  all. So that constant is now a measured number instead of the runtime's guess: the pack's own
  roughness maps average **0.58-0.65** for paving, walls and ground (per-map median 0.582), while the
  runtime's default was **0.7** - mattier than anything the pack's authors chose for the same
  surfaces. The profiles now run `rtx.legacyMaterial.roughnessConstant = 0.65`: one step from the
  default, landing on the matte end of the pack's own range rather than inventing gloss. **Not seen
  in game yet**; if the paving starts reading wet, that one line goes back to 0.7
  (`tools/rtxprofile.sh current <game>`), and the real fix - per-material roughness for the ~2190
  uncovered materials - is the next task, with its work list already written
  (`build/tmp/n18/pack-uncovered.txt`).
- **Two knobs we deliberately did *not* turn.** `rtx.opaqueMaterial.roughnessScale/Bias` would have
  rescaled the pack's 121 real roughness maps along with the defaults, flattening the only variation
  the pack has (and the runtime's own tooltip says they are for debugging). And
  `rtx.legacyMaterial.metallicConstant` stays at its 0.1 default: only 13 game textures have a metal
  map (those average 0.382), so 0.1 is wrong in both directions - but lowering it also removes real
  highlights, so it gets its own step and its own measurement.

### DLSS 5 (the reference video's trick) — measured, and the answer is "not on this box"

- **Nothing to enable.** The showcase look everyone is chasing is largely **DLSS 5** (DLSS 3D-Guided
  Neural Rendering), and RTX Remix 1.6.1 does ship the switches - eleven of them under
  `rtx.dlssNeuralRendering.*`, plus the 266 MB model (`<game>/.trex/nvngx_dlssnr.dll`). This box will
  not run it, and the proof is not a guess: the runtime asks the driver whether DLSS-NR is supported
  on **every** start, whatever your config says, and the answer in the log is
  `NVIDIA DLSS-NR support query failed: (code: 0xbad00010, info:
  NVSDK_NGX_Result_FAIL_UnsupportedParameter)` - RTX 4070 Ti SUPER, driver 610.57.04, below the
  616.92 baseline Remix 1.6 itself asks for. So **no shipped profile carries a DLSS-NR key**, and the
  look is bought with materials, fog and grading instead (n16-n18, n22).
- **A probe exists for the day that changes:** `bash tools/rtxprofile.sh dlss5 <game>` installs the
  60 fps profile plus the eleven keys at the runtime's own defaults, and
  `bash tools/rtxprofile.sh current <game>` puts the shipped file back. It cannot break startup: with
  the capability refused, the renderer never activates the pass, allocates nothing for it, and would
  switch the option off by itself if activation were attempted. Re-check the verdict after any driver
  update with `grep -iE 'DLSS-NR|Neural' <game>/rtx-remix/logs/remix-dxvk.log` - no config change
  needed. Details: [docs/TARGET.md](TARGET.md) §2 and §6, [docs/FINDINGS.md](FINDINGS.md) (n19).
- **One naming trap worth knowing when you edit `rtx.conf` yourself:** the switch is
  `rtx.dlssNeuralRendering.enable`. The bare `rtx.dlssNeuralRendering` is the option's *category* - a
  word the renderer recognises, so it keeps the line and ignores it, and the strings-based key checker
  even passes it. `python3 tools/rtx-option-audit.py <your file>` is the check that catches it.

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
- **When the fallback runs, the bulb now sits on the lens instead of the mast.** The signal list used
  to carry a pole position and a head height, so a list-mode light hung 1.75–2.10 m sideways from the
  lens it was meant to be a bulb on — the lamp table the height came from always carried that sideways
  part, and the file threw it away. The list now also carries each signal's rotation and its head
  offset, and the plugin turns the offset by the rotation before placing the light: **960 of the 1522
  signals** move onto the lens. The other 562 (the big `mtraffic1`/`mtraffic2` masts) stay on the mast,
  because no measured offset covers them. `lens_offset=0` puts the old position back; an old
  four-column list file still works unchanged.
- **A session that starts far from a signal no longer gives up on the head hook.** The plugin used to
  switch to the list for the whole run if the hook had recorded nothing for 20 seconds — and the
  default spawn is 79 m from the nearest signal, while the game itself draws no signal glow beyond 50 m,
  so that test could never pass. It now only falls back when a signal is actually within that 50 m and
  still nothing appeared, and it logs the distance and the decision once (`head source = ...`).
- **A lit signal is a bulb AND a glow.** Your correction on the last test round: the soft glow around
  a signal was wanted, the bulbs were missing — "both like real life". The newest build puts both on
  every lit head at the same spot, in the same colour, switching together: a small very bright bulb
  the size of the lens (what you look at), and a soft glow that carries the signal's colour a few
  metres onto the pole, the wall, the road and the peds under it. The glow's strength is the one you
  had on 2026-11-12 (measured, not re-tuned by hand), spread over a bigger sphere instead of squeezed
  into the lens, and it is capped so a signal can never out-light the street lamp over its head. It
  keeps its strength through the red/amber/green cycle and only changes colour. `glow_flux=0` in
  `rhyremix.ini` gives the bulb-only look back for comparison; `glow_radius` sets how far the spill
  reaches.

### Car lights, exhaust smoke and lit windows

- **A car's lamps are the size of lamps.** Until now the only light a car put into the world was the
  game's own glow sprite, and the game draws one of those at a radius of 15 game units - a **30
  metre** billboard per bulb, which is why a pair of tail lamps painted the sand red for metres and
  why the front of the car did nothing at all (the game's headlight beam sprite is excluded from the
  raytraced scene, so nothing came with it). The plugin now reads where the game itself puts each
  lamp and adds a light of the right size at each one: a reflector the size of a hand in a
  **50-degree beam** aimed along the car's own axis, and red lenses the size of a fist at the rear,
  brighter when the brakes are on. A headlamp is now the brightest thing inside its beam and a tail
  lamp is a lamp, not a flood - about 1/170 of the street lamp over its head, which is what a real
  rear position lamp is next to a street light.
- **Exhaust smoke is smoke.** The renderer turns any glow-style sprite into a self-lit surface, so a
  puff of soot was emitting light instead of scattering it - the pink-white plumes of the last test
  round, measured brighter than the road under a street lamp. That self-glow is now a third of what
  it was, so the scene's lights decide how bright a plume is; three of the game's smoke textures were
  also in no renderer category at all and are now listed as particles.
- **Lit windows stop being white blocks.** Some building windows were clipping to pure neutral
  white - no texture, no colour, a block. The emissive gain is now capped, harder by day than at
  night, which is where the measurement was. This is one global dial: per-window control lives in the
  asset pack's materials, not in a config file, so if a sign or a neon loses its punch the fix is in
  the pack.

All three are tunable and switchable: `[vehiclelights]` in `rhyremix.ini` (`enabled=0` turns the car
lamps off for comparison, `head_flux` / `tail_flux` / `brake_flux` set the strengths), and
`rtx.emissiveBlendOverrideEmissiveIntensity` in the renderer profile (back to its `0.2` default to
see the old glowing puffs).

### Water

- **The good water is now the default, and you do not have to install it** (n13): `apply.sh` and every
  game start put the researched `remix-ref` material in place and write the `rtx.conf` keys that go
  with it, backing the file up first. `tools/water.sh --water pack` at install time, `--no-water`, or
  `RHYS_WATER=<variant>` at any start picks something else, and a variant you chose on purpose is
  remembered (`<game>/.rhys-water/state`) and never quietly switched back.
- Four switchable water materials instead of the pack's one: `remix-ref` (the default: the numbers the
  other RTX Remix projects ship, plus a regenerated wave sheet), `realistic` (open coast, real
  seawater absorption), `calm` (bay / harbour, gentle), `pack` (the pack's original, always a safe way
  back). `tools/water.sh status "<game>"` says which is live, who asked for it, and which `rtx.conf`
  keys are still missing.
- Measured, not guessed: the pack's water normal sheet has a **median facet tilt of 44-46°**, which is
  why the sea reads as binary black/white. The USD variants help; the change that actually kills it is
  the map plus one `rtx.conf` line, and the installer now writes it with the value the live variant
  needs: `rtx.translucentMaterial.normalIntensity = 1.0` when the sheet was regenerated (44° → 6°),
  `0.35` when the pack's own sheet is in place. `RHYS_RTX_KNOBS=1` still adds it by hand if you prefer
  (backed up first).

### Mirrors, characters, vegetation

- **Rear-view mirrors: measured, and deliberately not changed.** SA paints its mirrors, and the
  obvious fix — a Remix material (metallic 1, roughness 0.02) keyed to the mirror *texture* — turned
  out to be wrong: parsing all 209 stock vehicle `.dff` shows `xvehicleenv128` / `vehicleenvmap128`
  are the reflection map of the whole car body (4197 materials on 179 cars / 495 on 81), and the
  mirror glass itself uses `vehiclelights128`, which is also every car light. No stock texture is
  mirror-only, so `config/mirror-hashes.txt` is empty and `tools/mirrors.py` refuses unverified
  hashes rather than re-materialising your car. [docs/MIRRORS.md](docs/MIRRORS.md) section 7 documents
  the route that does work — overriding the mirror **mesh** — and the geometry to pick.
- **Character packs are one switchable step**: `tools/characters.sh on|off|status` installs the
  recommended set in the right order and removes exactly what it added. The doc lists which of the
  pack's CJ material overrides stop applying after a CJ retex and how to re-point them.
- **Vegetation is one switchable step too**: `tools/vegetation.sh on|off|status [--step N]` installs
  "V Vegetation Mix" with the fixes for the freeze it caused, one ranked step at a time.

### Frame pacing — "fps a little less steady than the previous build" (n08)

- **What was actually measured first.** Both 2026-11-12 sessions kept a MangoHud log, so the sentence
  became numbers before anything was changed. The **average frame got better** between the two builds
  (17.72 → 17.50 ms) and the **hitches got worse**: frames taking more than 25 ms went from 1869 to
  2242, i.e. 77 → 137 per minute. Split at the first five minutes, which is what "stabilised as he
  played" means: the beginning was already fine (24 → 22 per minute) and everything after minute 5 is
  where the regression lives (91 → 188 per minute). The card was not the limit in either run — half
  utilisation, never throttled, 4.4 GB of 16.4 GB of video memory used at worst.
- **Fixed: the plugin could release hundreds of lights in a single frame.** Since the anti-pop work,
  one frame has been limited in how many lights it may *create*; nothing limited how many it may
  *release*. The two runs' own logs measured that queue at 320 and 205 handles, all of them released
  inside one frame. It is now bounded at 32 per frame (`[fade] destroys_per_frame` in
  `scripts/rhyremix.ini`; `0` puts the old behaviour back), which spreads a whole street layer going
  out over about a sixth of a second instead of one frame. The two lamp layers also no longer
  re-check their lists on the same frame.
- **Not blamed on us, on the evidence:** the hitch minutes are the minutes the plugin is quietest.
  The worst minute of the 10:22 run has 1098 slow frames while the plugin held ~100 lights, had no
  street lamps lit and created nothing; across both runs the correlation between slow frames and our
  live light count is *negative*. What that run added, on the renderer side, was 119 vegetation
  textures tagged as hair cards (the foliage work) — that tag turns those materials into alpha-tested
  cutouts, and cutouts are what the runtime builds opacity micromaps for, work it budgets per second
  and defers until the textures have streamed in. That is where the slow frames cluster.
- **So the 60 fps profile now bounds that work** (`bash tools/rtxprofile.sh perf "<game>"`): half the
  micromap build/bake budget, a quarter of the per-frame measuring pass, and a staged request must be
  on screen for half a second before it is worth building. Nothing in that list changes how anything
  looks. What is deliberately *not* in the profile: turning micromaps off, and changing the hair-card
  mip bias — both change how the hillside renders, so they are offered as one-line A/B legs in
  [docs/NEXT-TEST.md](NEXT-TEST.md) rather than shipped as defaults.
- **New tool: `tools/perf-report.py`.** Give it the MangoHud CSVs from `<game>/logs/perf/` and it
  prints frames, fps mean/median/p1/p0.1, frametime mean/median/spread/p99/p99.9/max, slow-frame
  counts per minute, the first-five-minutes-vs-after split, and an A/B table when you give it two
  runs. `--log rhyremix.log` puts the plugin's own counters beside the spike series and names the
  last plugin event before each of the worst frames; `--gpu-csv` takes the `gpu-*.csv` nvidia-smi
  because MangoHud cannot read the video memory of this process (its own column is a constant, and
  the report says so instead of graphing it). **Keep both `logs/perf/` files after every run.**
- **The one enormous startup stall is still there and is not a hitch:** both runs contain exactly one
  ~787 ms frame, 13.7 s in, in both builds, at the moment the street light set comes up. It is one
  frame per session; it is why "worst frametime" is not a useful number here and why the report
  quotes p99 and slow-frames-per-minute instead.

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
- **Quitting the game now quits.** Twice on 2026-11-12, choosing Quit left the game (and once also
  the Remix bridge, at 400 % CPU and 3.6 GB of video memory) spinning for minutes, with the launcher
  still waiting and the card never coming back. The launcher now ends the whole game setup itself:
  if the game's window has been gone for 20 s, or the bridge is still running after the game stopped,
  it writes `<game>/logs/exit-hang-<stamp>.txt` (what every thread was doing, plus the last lines of
  the four logs) and shuts the bridge and the Wine prefix down. `RHYS_EXIT_GRACE=0` turns that watch
  off. The plugin also logs what it saw — `shutdown: bridge gone (…)` in
  `<game>/scripts/rhyremix.log` — and stops sending light updates into a renderer that has gone.
  If a run ever hangs again, that evidence file plus `<game>/rtx-remix/logs/bridge64.log` is what we
  need.
- **F12 takes the screenshot** (never `Ctrl+PrintScreen`, which takes the renderer down). Fixed in
  this cycle: the hotkey was looking in the wrong folder and reading the key code in the wrong
  namespace, so every F12 silently did nothing.

---

## What to test

The short version: **run the tour, then send the shots and the log.** The step-by-step session
order, one variable per step with a rollback for each, is [docs/NEXT-TEST.md](NEXT-TEST.md); what
each reported problem is still waiting on is [docs/STATUS.md](STATUS.md).

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
5. **The mirror shot**: on a stock car the mirror is painted (a pre-baked gradient strip), so it will
   *not* show the road behind you — that is expected until the mesh override of
   [docs/MIRRORS.md](docs/MIRRORS.md) section 7 is done. What this shot is for is the framing: if the
   mirror frames an empty curb, that stop's coordinates are the ones to nudge — parked cars are placed
   by the compiled script, so their position is not readable from the game's data files.
6. **Water**: the default (`remix-ref`) is already installed — look at the open sea first and say
   whether the speckle is gone. For the A/B, `tools/water.sh realistic "<game>"` then
   `tools/water.sh default "<game>" --force`; `pack` is the way back to the pack's own material.
7. **Transitions, not still shots.** The tour photographs a standstill, so it cannot see a lighting
   change. For that, run `bash scripts/lighting-test.sh` — a drive through lit streets at dusk — once
   with the shipped profile and once with `tools/rtxprofile.sh nopops "<game>"`, and say whether the
   world still arrives all at once. In the log, the line to look at is
   `published: … rebuilds=… cad=…ms step<=… skip=…`: if `step<=` is near 0.25 while the world still
   pops, the step is the renderer's, and that is the answer the A/B is after.
8. **Night brightness, after the exposure change.** At the 21:00 stops (under a lamp, the sodium
   close-up, the signals) say whether the street is readable without being washed out, and whether the
   sky is still black. In the log, the 10-second line now carries `exposure=+0.500 EV (target +0.500,
   max step 0.005)` at a night stop and `exposure=-0.500` at a noon stop; if it reads −0.5 at 21:00 the
   plugin is not pushing and that is a bug to report, not a look to describe.
9. **DLSS 5 (n19) - one grep, nothing to install.** `grep -iE 'DLSS-NR|Neural'
   "$G/rtx-remix/logs/remix-dxvk.log"` should still show the `0xbad00010` support-query failure next
   to `[RTX info] Neural Radiance Cache: supported`: NRC works here, DLSS-NR does not, and they are
   different NGX features. If that warn line ever disappears after a driver update, the probe becomes
   worth a run (`bash tools/rtxprofile.sh dlss5 $G`, a noon street and a close character, then
   `bash tools/rtxprofile.sh current $G`); if it turns into "not available on this hardware/platform"
   then the GPU is the gate and the question is closed for good.
10. **Frame pacing (n08) — two files and one command, no looking.** Keep `<game>/logs/perf/`
   (MangoHud's `NvRemixBridge_<stamp>.csv` **and** the `gpu-<stamp>.csv` nvidia-smi trace — start one
   with the command in [docs/NEXT-TEST.md](NEXT-TEST.md) §5; MangoHud cannot
   read this process's video memory, so the second file is the only VRAM source) and run
   `python3 tools/perf-report.py <the csv> --log <game>/scripts/rhyremix.log --gpu-csv <the gpu csv>
   --series`. The number that decides it is **spikes/min after the first 300 s**: 90.6 was the good
   run, 187.8 the regressed one, and the 60 fps profile's new micromap bounds are aimed at that
   column. In the log, `config: fade … destroys/frame=32` confirms the build, and
   `remix: … due_destroy=N deferred_destroy=M` says whether the new bound is actually biting (always 0
   means the burst never happened in that session). If the hillside hitches worse with
   `rtx.opacityMicromap.enableBuilding = False` added, that is the answer the A/B is after — say which
   way it moved.

---

## Known issues

- **The traffic-signal hook has not been tested in game yet.** It patches a game instruction, and the
  two earliest builds crashed the game a few seconds after the level loaded; both causes were found
  and fixed, and the newest build adds a kill switch that refuses to patch anything it does not
  recognise, plus counters that make a bad run diagnosable. It is still the first thing to suspect if
  the game dies shortly after entering the level, and the way to take it out is one line:
  `[trafficlights] source=list` in `<game>/scripts/rhyremix.ini` (live, no restart).
  To judge whether it works at all, be near a signal: the log's `head source = ...` line now says
  how far the nearest one is, and the two tour stops (Marina, 1040,-1392 and 1068,-1428) have four
  signals inside the distance the game itself draws them at. A run that starts at the Grove Street
  spawn tells you nothing — the nearest signal there is 79 m away.
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
- **Some lamp heads are still dark.** The boardwalk and pier lamps (the white double-globe posts by
  the motel, the lamp row along the pier rail) have no glowing bulb and no pool under them, and the
  bulbs above the highway signs do not light at all. Those lamps are not in the lamp table this
  plugin reads, and the task that was going to build the list from the game's own 2dfx data was
  parked twice; the sign text also renders as separate textures that cast shadows, which it should
  not. Nothing is fixed there yet - it is the largest remaining gap in the night picture.
- **Facing the sun at sunset still goes gray, and night is still too evenly lit.** Both causes are
  measured (the renderer's *local* tonemapper: three synthetic exposures, a per-tile grey multiplier
  applied over a mip pyramid, which removes the bright end of the frame and re-solves as you turn),
  and the fixes are written down, but the two tasks that carried them were parked on an unrelated
  gate failure and their `rtx.conf` changes are **not** in this build. If you see it, photograph it:
  a shot into the sun and one away from it, at the same second, is exactly the evidence that would
  finish the job.
- **Frame pacing got a little worse between two builds on 2026-11-12** (76.9 → 137.2 frame spikes
  per minute over the two long runs), and it is not our light updates: the spikes do not line up with
  the plugin's create/destroy traffic. Keep `logs/perf/*.csv` after each run - that is what makes the
  next round of this measurable.
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
tools/water.sh pack "<game>"                      # the pack's water material (and it stays: recorded as your choice)
cp "<game>/.rhys-water/rtx.conf.pristine" "<game>/rtx.conf"   # the rtx.conf from before the water keys were added
tools/characters.sh off "<game>"                  # character packs out again
tools/vegetation.sh off "<game>"                  # vegetation pack out, every edited byte back
tools/mirrors.py --remove "<game>"                # mirror material layer out (none exists by default)
rm "<game>/scripts/rhyremix.asi"                  # our plugin only
```

`rhYgtaSA-backup/` is filled once, so it still holds the untouched files after any number of
re-runs.
