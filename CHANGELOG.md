# Changelog

All notable changes to **rhYgtaSA** — GTA San Andreas (1.0 US, 32-bit) under **RTX Remix 1.6.1** on
Linux/Proton, plus `rhyremix.asi`, the native plugin that drives the renderer through the Remix C API.

The format follows [Keep a Changelog 1.1.0](https://keepachangelog.com/en/1.1.0/) and the versions
follow [Semantic Versioning](https://semver.org/). Each release is grouped by feature — lighting,
sun and sky, traffic signals, water, installer and launcher, tools, testing and docs.

Two things to know before reading:

- **The version numbers are a reading aid, not git tags.** Nothing in this repository is tagged yet;
  every heading names the commit(s) that carry it, and `git log --oneline` is the authoritative
  record. The phase names in the text (`p01`…`p08`, `s01`…`s19`) are the work items recorded in
  `.strata/notes-*.md`.
- **"Fixed" usually means "measured first".** Most entries name the number or the log line that
  settled the question; [docs/FINDINGS.md](docs/FINDINGS.md) has the evidence and the dead ends,
  [docs/DEVELOPING.md](docs/DEVELOPING.md) the fix-by-fix table.

## [Unreleased] — 2026-11-12

Work in the tree, **not yet run in game**. Player-facing consequences are in
[docs/RELEASE-NOTES-next.md](docs/RELEASE-NOTES-next.md).

### Traffic signals

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

### Sun and sky (s29) — time of day stopped flickering

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

### Lighting (n01) — the steps that survived s28, and the renderer's own time knobs

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

### Tools

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

### Mirrors (s21) — the two "mirror hashes" were never mirror-only

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

### Testing (s26) — one page for the next in-game session

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
