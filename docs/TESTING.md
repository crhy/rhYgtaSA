# Testing the lighting changes

**The short version is [NEXT-TEST.md](NEXT-TEST.md)**: one page, six steps, one command each, with
the rollback and the report numbers next to every step. This file is the long version - why each log
line matters, and which knob fixes which symptom.

## Why this test is not run by the assistant

The assistant working on this repo is served by a local LLM server (`strata`,
`--resident-experts --kv-resident 32768 --max-context 262144`) which pins its
model and KV cache in VRAM on purpose: **15,494 of the 4070 Ti SUPER's 16,376
MiB**. RTX Remix needs most of the card. So while the assistant is running, the
game cannot run, and there is no way for the assistant to fix that from inside
the game.

This is not a hypothetical. On 2026-11-11 the assistant did not know this and
spent three config rollbacks and four launches chasing a "white screen then
crash", reading the bridge's CPU exception as a lighting bug. It was VRAM
starvation. `launch-rtx.sh` now refuses to start and names the process, so this
cannot cost time again.

**So: stop the LLM server, run the test, send back the screenshots and the log.**

```bash
# what holds the VRAM, and how much
nvidia-smi --query-gpu=memory.used,memory.total --format=csv,noheader
nvidia-smi --query-compute-apps=pid,used_memory --format=csv,noheader
```

Roughly 8 GiB free is enough to start; more is better for the asset pack.

Since s15 the whole dance is one command, from a terminal (not from the assistant, which is
the thing that has to leave the card):

```bash
bash scripts/test-session.sh --tour     # Strata off -> wait for the card -> launcher -> clean up -> Strata back
```

Since n09 the same command also watches for the **exit hang**: if `gta-sa.exe` is still running 20 s
after its `NvRemixBridge.exe` or its window has gone away (the 2026-11-12 10:39 Quit, see
[FINDINGS.md](FINDINGS.md)), it is logged, dumped to `build/tmp/exit-hang-<stamp>.txt` (threads with
STAT/%CPU/WCHAN plus the tails of `scripts/rhyremix.log`, `moonloader/moonloader.log` and
`rtx-remix/logs/remix-dxvk.log`), killed, and the run continues so Strata comes back. Tune it with
`RHYS_HANG_GRACE` (0 turns the watchdog off); `scripts/rhyremix.log` should by then contain
`shutdown: bridge gone (…)` lines of its own.

Since n09b the **launcher itself** does the same thing, so a run started by clicking the desktop
entry (or by `launcher/launch-rtx.sh` directly, which is how the 11:48 hang happened) also ends:
after `gta-sa.exe` returns it ends `NvRemixBridge.exe` and runs Proton's `wineserver -k` for the
prefix, and while the game runs it watches for the window being destroyed (or the bridge outliving
`gta-sa.exe`) for `RHYS_EXIT_GRACE` s (default 20) and then ends the whole wine tree, writing
`<game>/logs/exit-hang-<stamp>.txt` first. If a run ever hangs again, send that file **and**
`<game>/rtx-remix/logs/bridge64.log`: the line `Client window became inactive, disabling timeouts
for bridge server...` is what says the bridge had stopped noticing the client had left.
`RHYS_BRIDGE_TIMEOUTS=1` is the A/B that asks `.trex/bridge.conf` to keep those timeouts on
(`docs/FINDINGS.md` says why that may not stick); `RHYS_EXIT_GRACE=0` turns the launcher's watchdog
off without disabling the after-exit cleanup.

`--tour` writes `<game>/screenshots/tour.req` first, so `SARemix_Tour.lua` runs the fixed
stop list (names and order in `screenshots/tour-manifest.txt`, one run per flag file) instead
of you walking to every shot with F12. Since n03 **`deploy.sh` installs that script** into
`moonloader/` on every run (it used to install only `SARemix_Shot.lua`, so the copy in a game
folder was whatever a human had `cp`'d by hand - which is how the 2026-11-12 00:49 session ran
the stale 13-stop tour while the repo had 16, and stops 12/13/14 produced no shots at all). The
check is `diff -q plugin/lua/SARemix_Tour.lua "$G/moonloader/SARemix_Tour.lua"`: it prints
nothing when the game has the repo's bytes, and `docs/NEXT-TEST.md` step 0 runs it. The list ends
with the three s15 subjects: the
rear-view mirror of the car parked at the Grove Street curb (docs/MIRRORS.md), a pedestrian
close-up in Pershing Square at noon, and the Mulholland/Vinewood hillside vegetation (the s03
streaming hang). If the mirror shot frames an empty curb, that stop's numbers are the ones to
nudge in `plugin/lua/SARemix_Tour.lua` - parked street cars come from the compiled main
script, so their position is not readable out of the game's data files.

When the tour is done, one more command turns the folder into something you can judge instead
of flip through:

```bash
python3 tools/tour-report.py /media/rhy/data/Gamez/GTASA-RTX      # or --since 21:00 for the last run
```

It joins `screenshots/tour-manifest.txt` to `screenshots/shots/*.png` by timestamp (the manifest
line and the file are written a second apart, so it matches by nearest time, never by name), and
writes `<game>/screenshots/report-<tour start>/`: labelled contact sheets, 4 shots each, and
`report.md` with objective numbers per shot - Rec.709 mean/percentile luma, % clipped white,
% black, mean saturation, and for the signal stops the fraction of red/amber/green pixels -
next to the `scripts/rhyremix.log` lines nearest each shot (streetlights, trafficlights with
their green/amber/red counts, and the sky line with sun elevation and the game clock). That is
how "the lamp looks dim" becomes "mean luma 26, 6.7 % crushed black, 320/320 drawn".

---

## Step 1 - deploy the conservative config

```bash
cd ~/rhYgtaSA
./deploy.sh --build --prod --force-ini --rtx-conf /media/rhy/data/Gamez/GTASA-RTX
```

`--rtx-conf` installs `config/rtx.conf`, which changes **exactly two** values
from the shipped file:

| key | shipped | now | why |
|---|---|---|---|
| `rtx.emissiveIntensity` | 10 | 1.0 | the asset pack's own emissive materials already go to `emissive_intensity = 3000`; x10 rendered shopfront windows at 30000 |
| `rtx.skyBrightness` | 3 | 1.0 | the sky is now driven by the solar model, so the static 3x boost is double-counting |

Everything else in `rtx.conf` is untouched. Do **not** use
`--rtx-conf-exp` on the first run: it adds six more changes that have never been
observed running, and if the image is wrong you will not know which of the eight
caused it.

## Step 2 - launch, and check the log before looking at the picture

```bash
cd /media/rhy/data/Gamez/GTASA-RTX
./launch-rtx.sh
tail -f scripts/rhyremix.log
```

Three lines decide whether the run is worth reading the rest of:

```
remix: remixapi_InitializeLibrary v0.6.5 -> 0
config: lamp source cool=mercury warm=hps (kelvin fallback 5000/2100) -> ...
config: fixture geometry real luminaire sizes (street big radius 15.00 -> 0.20 game units, signal 6.00 -> 0.10), radiance x5625 to hold flux, fixture_size=1 scale=1.00 ref=15.00
```

- **No `remixapi_InitializeLibrary` line** means the renderer never came up. That
  is an environment problem (VRAM, leftover processes), not a lighting problem.
  Stop here and fix that first.
- **`fixture geometry ... 15.00 -> 0.20`** is the new physical source size. If it
  says `ini radius (legacy blob)` then `fixture_size` is not reaching the plugin.
- **`radiance x5625`** is the flux compensation. It looks alarming and is
  correct: the sphere's area fell by (15/0.2)^2, so radiance rises by the same
  factor and total emitted power is unchanged.

Then, once you are in a city at night:

```
streetlights: N active, N drawn this frame
sky: sun elev=... rad=... | moon elev=... rad=... (hour=...)
```

For the traffic signals (s01, s30, n02, n02b) these lines decide everything:

```
config: traffic=1/1 source=game ... head_forward=0.15 head_grace=20000ms lens_offset=1 | bulb radius=0.100 radiance=1200 nits (fixture_size=1) corona_radius=0.45 (resizes the game's 15.0 flare) | glow radius=3.00 flux=304 (cap 3473 = one street luminaire)
signalhook: patched 0x0049DD4C (was 'call 0x6FC580') -> stub at 0x65d55000, cap 256 heads per frame, corona resized to a bulb
signalhook: signal-head corona radius 15.00 -> 0.45 game units at 0x0049DCDA (the game's own 30 m green flare becomes a lens-sized bulb; corona_radius=0 puts it back)
trafficlights: loaded 1522 lights from rhyremix_traffic.txt (960 carry the v2 heading/lens columns, 562 are v1 'x y z headz' and stay on the mast)
trafficlights: #0 at -269.8,1088.1,18.7 head z=26.3 heading=0 lens=(-2.10,+0.10,7.60) -> light -271.9,1088.2,26.3
trafficlights: head source = hook: no signal inside the game's own corona range, so nothing to record (nearest signal 79.1 m, the game's own corona range 50 m, grace 20000 ms, 21000 ms since the hook went live) | hook: calls=0 records=0 dropped=0 bad=0 reentrant=0
trafficlights: head light ok bulb id=... phase=0 hash=... pos=-269.83,1088.12,26.30 radiance=2.16,1200.00,410.32 gain=1.00 radius=0.10 flux=67 (bulb: lens nits)
trafficlights: head light ok glow id=... phase=0 hash=... pos=-269.83,1088.12,26.30 radiance=0.01,6.00,2.05 gain=1.00 radius=3.00 flux=304 (glow: soft spill at the head)
trafficlights[source=game]: 7 heads lit (7 with a glow), 7 bulbs + 7 glows drawn this frame (green=2 amber=1 red=4, 7 coronas registered this frame, colour_swaps=3 expired=2) | ... | hook: calls=412 records=412 dropped=0 bad=0 reentrant=0
```

The `loaded ... (N carry the v2 heading/lens columns, M are v1 ...)` line says
which file the plugin actually read: a v1 `rhyremix_traffic.txt` (four columns)
puts every list-mode light on the mast, a v2 one (eight columns, what
`tools/traffic_list.py` writes now) puts it on the lens. The `#0 at ...` line
shows both: `head z=` is the old pole-plus-height position, `-> light` is where
the light really went after the heading and the lens offset.

The `head source =` line is n02's, logged once whenever the answer changes. The
hook is only given up on when a signal of the loaded list is inside the game's
own corona range (**50 m**: the call site pushes 50.0 at 0x49DCF3 and the corona
function tests it at 0x6FC22E, *after* our patch point, fading from 35 m), the
`head_grace_ms` window has run out and still no record appeared. A session that
starts 79.1 m from the nearest pole - the default spawn - keeps the hook and
says so, because the game draws no signal-head corona out there either and
`records=0` proves nothing. `source=list` in the ini is still the hard kill
switch.

The `hook:` counters are the whole diagnosis of the s19 hardening: `calls` is
how often the patched instruction ran, `records` how many heads we kept,
`dropped` how many the 256-per-frame cap refused, `bad` how many carried a
pointer that failed the page check, and `reentrant` how many arrived while the
recorder was already running (it must never be non-zero: it would mean the hook
fires from two places at once).

The `radius=` / `flux=` pair on a `head light ok` or `CreateLight ok` line is the
s30 bulb: the emitting sphere's radius in game units and its total output. At the
shipped defaults a signal is `radius=0.10 flux=67` where a mid street luminaire
is `radius=0.15 flux=3473`; a signal whose flux comes out near a lamp's is what
makes a street go green instead of showing heads.

n02b: every lit signal logs **two** of those lines, `bulb` and `glow`, at the same
position with the same colour and gain - the bulb is the head you look at
(`radius=0.10 flux=67`), the glow is the light that colours the pole, the wall,
the road and the peds (`radius=3.00 flux=304`, the flux the 2026-11-12 00:48 glow
measured, capped at one street luminaire's 3473). The 10 s summary says
`N heads lit (N with a glow), N bulbs + N glows drawn`: **fewer glows than heads
names a create that failed**, and `glow_flux=0` (bulb-only) is the A/B that tells
you what the glow is doing on a given street.

- **No `signalhook: patched` line**, and instead
  `trafficlights: source=game requested but the hook was not installed: ...` means
  the plugin is running the old pole-position mode (`source=list`). The reason is
  in that line; it is a different game build or a code page that would not take
  the patch. Since s19 the install is refused unless `[trafficlights]
  source=game` **and** the bytes at 0x49DD4C decode to `call 0x6FC580` **and**
  0x6FC580 is still the corona-type thunk **and** the caller's `add esp,0x54` is
  where it must be; the reason names which one, and the extra log lines
  (`signalhook: 0x49DD4C does not call ...`, `... is not the corona-type thunk`,
  `... does not hold 'add esp,0x54'`) quote the bytes it actually found. Since
  s30 a `does not call 0x6FC580` line can only mean the bytes really are
  different: before that the plugin refused to read them at all and logged that
  reason about a correct exe, on every run from s19 to 2026-11-12 - which is why
  no screenshot so far has ever shown a head light.
- **`head source = list: a signal was inside the game's own corona range and
  still no head appeared`** (n02, replacing s30's `the corona hook is installed
  but has recorded no signal head in 20000 ms`): the patch is in, a signal was
  within 50 m of the player for the whole grace window, and the hook handed us
  nothing, so the module is lighting the list instead and the intersection is not
  dark. The line carries the distance and the hook counters: `calls=0` there
  means the patched instruction never ran, `calls>0 records=0` means it ran and
  we dropped what it gave us.
- **`head source = hook: no signal inside the game's own corona range, so nothing
  to record`**: nothing is wrong, the player is simply far from every signal in
  the list (the default spawn is 79.1 m out). The hook stays live. To judge the
  hook, drive to a signalised intersection - the tour's stops at 1040,-1392 and
  1068,-1428 have four signals inside 50 m.
- **`0 heads lit`** with `calls=0` means the hook is in but the game never
  reached it - the player is not near a signalised intersection, or it is daytime
  with `night_only=1`. With `calls>0` but `records=0` the hook *is* firing and we
  are throwing what it hands us away; that is a bug in us, not a quiet scene.
- **`dropped`** climbing means the hook is recording more than 256 heads a frame.
- **`bad`** climbing means the hook was handed a pointer that is not readable
  memory. It used to be a crash (2026-11-11 13:04:44); now it is a dropped
  record, so a rising `bad` is a symptom to report, not a reason to stop playing.

### The logs themselves (n03): what the launcher rotates, and what reads both

Two files used to grow without limit, and both are read by the tools above:
`scripts/rhyremix.log` (1,088,220 bytes on 2026-11-14, appended to on every run
since 2026-11-10, never rotated - the 3.4 MB `rhyremix.log.prev` next to it was made
by hand) and `logs/steam-12120.log` (51,958,320 bytes from ONE `PROTON_LOG=1` run).
`launcher/launch-rtx.sh` now bounds them:

| when | what |
|---|---|
| before the game starts (never on a `RHYS_DRY_RUN=1` run) | a log over `RHYS_LOG_MAX_BYTES` (default 8 MiB) moves to `<name>.prev` - exactly one generation - and the live file restarts at 0. `rhyremix.log` moves **whole** (a `.prev` that begins mid-line is a lost line); the Proton trace keeps only its last `RHYS_PROTON_LOG_KEEP` (default 2 MiB) |
| after the run | the Proton trace is trimmed **in place** to that same keep, because `docs/TESTING.md` and `docs/FINDINGS.md` tell you to grep `logs/steam-12120.log` - the file has to stay at that path, just not 52 MB of it |
| `RHYS_PROTON_LOG=0` | no Proton trace at all (the only cap that is 0 bytes); default is on, because the Open-Limit-Adjuster row below needs it |

`scripts/diagnose.sh` and `tools/tour-report.py` read `rhyremix.log` **and**
`rhyremix.log.prev`, oldest first, so `tail -n` still means "the newest N matches" and
a rotation cannot hide the run you are reporting on; `diagnose.sh` prints `reading:`
with both paths and the tour report header says `N lines read from 2 file(s)`. Four
readers deliberately still look at the live file only, because they ask about the run
that just happened: `tools/perf-report.py --log` (it takes an explicit path and splits
runs on the `---- log starts ----` marker), `scripts/check-rtx-probe.sh`,
`scripts/lighting-test.sh`, and the launcher's `exit-hang-*.txt` evidence tails. The
proof is `scripts/log-rotate-selftest.sh` (52 checks, in `check-all`): it rotates a
fake tree whose log carries one marked line per claim, then requires every line
`diagnose.sh` claims back and all three tour-report log categories.

### The fade counters (s28): proof that lighting changes over time instead of at once

One config line at startup says what the plugin will and will not do:

```
config: fade in=700ms out=900ms step=0.25 creates/frame=24 player_grace=8000ms sky=3.00x+0.25 per s sun=3.00x 12.0 deg/s phase_fade=250ms
```

and the module lines now carry the fade traffic:

```
streetlights: 318 active, 318 drawn this frame, 0 awaiting destroy (hour=21, 7002 in list, kept=310 new=8 steps=14 created=6 retired=4 in_drop=0 player_gone_frames=0) | remix: live=320 create_fail=0 draw_fail=0 budget_stop=0 deferred=37
```

| field | what "correct" looks like while driving |
|---|---|
| `new=` per 10 s log line | a handful, not 320. The old build logged `new=320` or `new=0` and nothing in between — that all-or-nothing was the pop |
| `steps=` | the fade traffic: how many handles were re-created to move a lamp one step. Non-zero during a transition, ~0 when nothing is changing |
| `created=` / `retired=` | similar and gradual. `retired` should never spike to the whole active count in one line |
| `deferred=` | the frame budget doing its job: fade steps that did not fit moved to the next frame. It climbing is normal; it is what keeps `created=` under `creates/frame` |
| `player_gone_frames=` | frames where the ped pointer read 0. Non-zero is normal (wasted/busted, interiors, riding on a vehicle); the lights now ride through it |
| `in_drop=` (n04) | **0, always.** The cumulative count of lamps that had a light, were inside `max_distance`, and were left without one by the next selection - a street light switched off while it is still in front of you. It is the number finding A of 2026-11-12 is about; if it climbs, the cap is smaller than the lamp density where you were |
| `live=` on the `remix:` line | **no jumps of 100+.** The 2026-11-12 run had 20 such jumps in 25 minutes (`1->360`, `381->1`); that number going to zero is the whole point of s28 |

To check it by eye: at night, drive down a long lit street at speed and watch the
lamps ahead of you. They should **brighten as you approach and dim as you leave**,
and a street should never arrive all at once. Then flip day to night by editing
`[sun] force_hour` in `rhyremix.ini` while the game runs: the street should take
about a second to come up or go down, not one frame.

### n04: lamps that do not care which way the camera points

Two new lines in the log answer finding A of the 2026-11-12b session ("lights
flicker in and out of existence when the camera rotates; facing a light it is off,
facing away it comes on").

At load, the street module measures the lamp file against the ini and prints it:

```
streetlights: worst case 408 lamps within max_distance=300 of a point (cap max_lights+sticky_extra=700): the cap covers the radius
```

If that line ever says the cap will trim, `max_lights` in `rhyremix.ini` is smaller
than the lamp density around you and lamps are being dropped while visible. Lower
`max_distance`, not `max_lights`.

Every 10 s, both layers print `in_drop=` (see the table above). The test for
finding A is then mechanical:

1. At night, stand somewhere dense - the pier at Columbus, or Vinewood. Let the
   street settle (the first couple of seconds create a lot).
2. **Spin the camera a full circle** on the spot, twice, with the mouse. Do not
   move CJ.
3. Look at the log. `in_drop` must not have moved, `new=` should be 0 or a handful
   (only lamps that came into range as the camera... did not move, so: 0), and
   `active` should be the same number in every line of those 20 seconds.
4. Then walk the pier for a minute. `in_drop` still 0; `new=` a few per line as
   lamps enter the fade band; `steps=` non-zero (that is the fade, not a pop).

What to *see*: a lamp you walk away from dims and never comes back while it is
behind you; a lamp you turn your back on stays exactly as bright as it was. Signal
heads are the ones that used to do the opposite - the 2026-11-12 10:22 run expired
790 heads and retired 817 in 970 seconds because the game only registers a signal
corona for what it draws. If a signal bulb fades out when you turn away and comes
back when you look again, that regression is back; `expired=` climbing while
`in_drop=` stays 0 is what that looks like in the log.

The renderer half of finding A is in `rtx.conf`, not in this log: the eight keys
listed in `docs/FINDINGS.md` (permutation sampling off, more light candidates per
pixel, longer reservoir life, anti-culling on). No script can see them - every tour
shot is taken at a standstill - so the A/B is: `tools/rtxprofile.sh current`, spin
the camera in a dense street at night, then `tools/rtxprofile.sh perf` and repeat.
What should change is the *boiling* of lamp light as you rotate, not where the
lamps are.

### The clock counters (s29): proof that time of day moves instead of stepping

The `sky:` line now carries the clock's own bookkeeping:

```
sky: sun elev=+41.3 az=187.9 airMass=1.53 T=0.712/0.584/0.431 daylight=1.00 | moon elev=-12.4 az=95.1 phase=0.42 | lamps=off (clock=13:07 tod=787.35 min ms/min=2013 doy=186 month=0 day=0) | tod: ticks=394 torn=2 jitter=17 reanchor=0 wraps=0 bad=0 | cal: stable=0 accepted=0 rejected=394 | cloud=0.00 | published: sun 4.28,3.51,2.59 sky=1.000 emissive=1.000 rebuilds=12/4 | rtx: set=41 unknown=0 badval=0
```

| field | what "correct" looks like |
|---|---|
| `tod=` | the game minute the sky is being computed at, **with decimals**. It must change every log line, by about 5.0 between two 10 s lines (10 s / 2013 ms). If it only ever shows `.00`, the interpolation is not running |
| `ms/min=` | what the plugin measured for one game minute on this machine: 1670-2030 ms is normal (the 2026-11-12 log measured p50 2015). It starts at `[clock] ms_per_game_minute` and converges |
| `clock=` | the raw bytes, unchanged, for comparison with `tod=`. `clock=13:07 tod=787.35` is consistent (13*60+7 = 787) |
| `torn=` | hour/minute reads that landed between the game's two byte writes and were repaired. A few per hour is normal; it is the count of times the clock would otherwise have jumped an hour backwards |
| `jitter=` | backwards clock moves that were refused because they were small - the tour's `setTimeOfDay()` pins, mostly. Non-zero during a tour is expected and is the fix working |
| `reanchor=` | deliberate jumps the game actually made (a mission setting the time). Should be a small number, and it must not climb every sample - that would mean the plugin and the game are fighting over the clock |
| `wraps=` | midnight crossings. One per game day (about every 48 real minutes) |
| `bad=` | clock bytes out of range (hour > 23, minute > 59). Should stay 0 |
| `doy=` / `month=` / `day=` | the season in use, and the raw calendar bytes. `month=0 day=0` with `doy=186` is the **expected** state: the game's date bytes are a counter, not a date, so `[sun] day_of_year` is used. `cal: accepted=` should stay 0 |
| `cal: rejected=` | how often the calendar bytes were refused. It climbing while `accepted=` stays 0 is the alternating-calendar bug being held down - that alternation was the 20-40 degree sun swing in the 2026-11-12 log |
| `cloud=` | the smoothed overcast (0..1). 0.00 is clear sky and the shipped default; it only moves if you edit `[sky] cloud`, and it ramps rather than stepping |
| `rebuilds=` | how many times each body's handle was re-created (sun/moon). Since n01 this is a **clock**, not a threshold: at the shipped `[fade] handle_cadence_ms=2000` expect about one per body every two seconds while the sun is up, and 0 with `[sun] force_hour` pinned |

### The handle cadence (n01): proof that the shadow direction moves in small steps

A Remix distant light's direction is baked at `CreateLight`, so a moving sun is a sequence of
handle rebuilds and each rebuild is a hard step in shadow direction. The startup line now
states the schedule:

```
config: fade in=700ms out=900ms step=0.25 creates/frame=24 player_grace=8000ms sky=3.00x+0.25 per s sun=3.00x 12.0 deg/s phase_fade=250ms handle=2000ms(min 150ms, <0.50 deg, <5.0% lum)
```

and the `published:` part of the `sky:` line carries what the schedule actually did:

```
published: sun 4.28,3.51,2.59 sky=1.000 emissive=1.000 rebuilds=6/6 cad=2000/2000ms step<=0.25/0.25 skip=0/0
```

| field | what "correct" looks like |
|---|---|
| `cad=` | the period in effect for each body, in ms. `2000/2000` is the fixed cadence at rest. It shortens when the published direction or radiance is actually moving (a clock jump drives it to the 150 ms floor) and must come back within a second or two of the motion stopping. If it sits at 150 while nothing is moving, the rate estimator is being fed noise - that was the `acos()` bug and `rhyAngleDeg()` is the fix |
| `step<=` | the largest direction step one rebuild has actually carried this session, in degrees. At rest it must stay near 0.25 (0.125 deg/s x 2 s) and never exceed `[fade] handle_step_deg` (0.5) unless the body is slewing; the old threshold rule sat at 1.00-1.20 permanently, which is twice the sun's 0.53 deg disc |
| `skip=` | periods where the live handle already carried the published values, so no bridge call was made. With `[sun] force_hour` pinned this climbs and `rebuilds=` stays 0 - that is the proof the cadence is a clock and not a queue |

To check it by eye: at noon, with a long shadow falling across a wall, watch the shadow edge
for a minute. Before n01 it stayed still and then jumped about a degree, every eight seconds.
It should now creep. Then pin the clock (`[sun] force_hour=20`, save): the shadow should walk
to its new place over several seconds, and `cad=` should drop to 150 while it walks and back
to 2000 when it arrives.

To check it by eye: stand still and watch the sun's `elev=` across consecutive
`sky:` lines. It should move by ~0.2 degrees per game minute in the same direction
all morning; the 2026-11-12 log had it swing up to 43.4 degrees between two lines and
back again. `scripts/rhyremix.log` from a tour run is the interesting one, because
`SARemix_Tour.lua` writes the clock twice per stop - `jitter=` climbing while `elev=`
keeps moving one way is exactly what s29 is for.

### n12: the car's own lamps (findings V5, V6, V7)

The plugin now lights a car's head, tail and brake lamps from the light dummies the game itself
publishes. Once a second:

```
vehiclelights: coronas=41/64 head_tex=4DBD066761A29F5B lamps=6 vehicles=3 live=6 creates=18 budget_skips=0
```

| field | what "correct" looks like |
|---|---|
| `coronas=N/M` | N entries read from the game's corona store this frame, M the store's capacity (64). `coronas=0/0` means the store was not where this build puts it, so the module is reading nothing |
| `head_tex=` | the sprite pointer the game resolved for its headlamp slot. `head_tex=00000000` = not resolved, so no headlamp will appear (the red lenses still will) |
| `lamps=` / `vehicles=` | lamps created over the cars in range - a head pair and a tail pair per car. A lit shopfront should never show up here; that is what the 6 m `max_spread` rule is for |
| `budget_skips=` | rebuilds the frame's create budget refused. Climbing at speed is normal |

What to look at: at night a car with its headlights on should throw a **cone with a visible edge**
onto the road 20-30 m ahead (the shipped cone is 50 degrees across), and its tail lamps should
colour the ground within a metre or two - not a 30 m red blob. Braking should read **brighter, not
wider**.

Knobs, in `[vehiclelights]` in `rhyremix.ini`: `enabled`, `max_distance`, `max_lights`,
`cadence_ms` (120 - how often a moving lamp may be rebuilt, because a Remix light's position and
aim are baked at create time), `move_step` (0.8 m) and `brake_red_min` (220; brake detection is a
hue threshold because the corona store carries no brake flag - `0` turns it off).

The two renderer halves, for an A/B in `rtx.conf`: `rtx.emissiveBlendOverrideEmissiveIntensity`
(shipped **0.06**, runtime default 0.2) is the exhaust plume's self-glow - put it back to 0.2 and
the puffs light themselves again; `[sky] emissive_day / emissive_night` (shipped **0.5 / 0.8**) is
the flat-white window cap - 1.0 / 1.0 is the old behaviour. None of the three has been seen in game
yet: rows V5-V7 of [STATUS.md](STATUS.md) are what the next session decides.

## Step 3 - what to photograph, and what each one proves

Take shots with **F12** in game (fixed 2026-11-11: before that every F12 failed silently, see below); they land in
`/media/rhy/data/Gamez/GTASA-RTX/screenshots/pending` and `scripts/shot-watch.sh`
(started by the launcher) moves them to `screenshots/shots/`.

Set the clock first if you want a specific time of day - in
`scripts/rhyremix.ini`, `[sky]`:

```ini
force_hour=21
force_minute=0
```

`force_hour=-1` gives the real in-game clock back. Editing `rhyremix.ini` at all
makes the plugin re-read it live, which is how every knob below can be A/B'd
without restarting the game.

| # | Shot | What it proves | What "correct" looks like |
|---|---|---|---|
| 1 | Grove Street, ~21:00, standing under a lamp | the fixture-size fix | a **defined pool** on the pavement with a falloff, not a uniform glow ball. The pole and the ground under it should be separable |
| 2 | Same street, 30 m away, no lamp near | lamp spacing reads as separate pools | distinct pools with dark between them. One continuous wash means the source is still too big or too bright |
| 3 | Any warm (sodium) lamp close up | the spectrum fix | **yellow-orange**, no red halo, no blue. Compare against a 2000 K incandescent in shot 4 |
| 4 | An interior or tungsten-lit shopfront | thermal vs discharge | tungsten should be visibly **redder/deeper** than the sodium street lamp. If they look the same colour the family mapping is not reaching the lights |
| 5 | A Burger Shot or Pizza at night | the emissive gain fix | window readable as a bright rectangle with a **visible falloff onto the pavement**. Blown white with no readable interior means `emissiveIntensity` is still high |
| 6 | A signalised intersection, close, all three aspects visible | the signal-head fix (s01) + the bulb (s30) + the glow (n02b) | **every head is lit where it is and in the colour the game is showing it**: the light sits at the head, not at the top of the pole, and on a mast-arm signal the head hanging out over the road is the one that glows. Each head matches its own corona - red is **red**, not orange; green is distinctly **blue-green**; amber sits between. **Both halves**: a small bright point at the lens (the bulb - bright pixels, not a soft blob) AND a few metres of that same colour on the pole, the wall, the road and any ped under it (the glow, wanted: rhY "both like real life"). A street-wide wash, or a bright dot that colours nothing, are both wrong |
| 7 | Same intersection from the cross street | the unshaped-light limitation | both faces readable, and the face pointing at you shows the same aspect the game's corona shows. If one face goes dark, that is the missing lens direction (see below) - not a wrong position |
| 8 | Sunset, facing east | the solar model + Beer-Lambert | horizon glow strongly red/orange, zenith still blue. The red comes from path length, it is not tuned |
| 9 | Any street at ~05:30 with lamps still on | the twilight switching | lamps on before the sun clears the horizon, off shortly after |

### The renderer's temporal A/B (n01): is the step ours or the renderer's?

If the plugin's own numbers above look right and the world still changes at once, the step is
being made by the renderer's temporal filters, not by us. `config/rtx.conf.nopops` is
`config/rtx.conf` with nine temporal keys changed and **nothing** else - no look key, no
texture list (`tests/test_rtx_nopops.py` and `scripts/rtxprofile-selftest.sh` enforce that).

```bash
tools/rtxprofile.sh nopops  "/path/to/GTASA-RTX"      # the temporal A/B
tools/rtxprofile.sh current "/path/to/GTASA-RTX"      # the shipped profile
tools/rtxprofile.sh status  "/path/to/GTASA-RTX"      # which is live, and what it says
tools/rtxprofile.sh restore "/path/to/GTASA-RTX"      # the exact bytes that were there before
```

Run the **same** drive twice, `scripts/lighting-test.sh`, one profile each way, and compare at
the moments the transition happens: dusk with `[clock]` untouched, driving under the lamps.
The tour cannot answer this question - it takes every shot standing still, so it measures the
look and never a transition.

Two things to keep in mind while reading the A/B:

* `rtx.graphicsPreset = 1` is not Custom, and the preset layer sits **above** `rtx.conf` for
  the keys it sets. If a key does not read as changed in the Remix UI, the preset owns it.
* The live indirect integrator on this install is SHaRC (`remix-dxvk.log`: "Integrate Indirect
  Mode: Spatially Hashed Radiance Cache - activated"), so the whole `rtx.restirGI.*` block -
  including `historyDiscardStrength`, the key s28 named - is inert here. It is aligned in the
  A/B file anyway, but do not read a change in behaviour as coming from it.

If a change now lags (a lamp taking a quarter second to reach full brightness), lower
`rtx.spatiallyHashedRadianceCache.maxNumFramesToAccumulate` and
`rtx.volumetrics.maxAccumulationFrames` first, and `rtx.temporalAA.newFrameWeight` last: the
last one is the whole-image blend and is also the one that can ghost when the camera moves.

## Step 4 - if something is wrong, these are the knobs

All in `scripts/rhyremix.ini`. Edit, save, and the plugin re-reads it within a
few seconds - no restart.

| Symptom | Change |
|---|---|
| Night too bright overall | `[streetlights] radiance_scale=0.5` |
| Light pools still soft/huge | `[streetlights] fixture_scale=2.0` is the *wrong* direction - instead check `fixture_size=1` is set |
| Sparkles / fireflies / banding around lamps | `[streetlights] fixture_scale=5.0` (1 m source: still 15x tighter than the old blob, far gentler on Monte Carlo noise) |
| Lamps too dim | `radiance_big=3.0` |
| Sodium still looks wrong | `lamp_warm=hps` - or try `lamp_warm=tungsten` to see the difference the line spectrum is making |
| Cool lamps too blue | `lamp_cold=halide` (metal halide is much more neutral than mercury) |
| Shopfronts blown out | `rtx.conf`: `rtx.emissiveIntensity = 0.5` |
| Sky too bright at night | `rtx.conf`: `rtx.skyBrightness = 0.5` |
| Signals too bright, or the street around them goes green | `[trafficlights] bulb_radiance=600` (the lens luminance in nits; 0 turns the signal lights off), `bulb_radius` if the sphere itself is too big, and `glow_flux=150` (or `0`) for the spill - the log's `flux=` on the `glow` line is the number to compare with a street lamp's 3473 |
| A signal is a bright dot that colours nothing around it (no glow on pole/wall/road) | n02b: `[trafficlights] glow_radius` (metres of spill, default 3) and `glow_flux` (default 304 = the 2026-11-12 00:48 look, capped at 3473 = one street luminaire). The log must show a `glow` line per lit signal and `N heads lit (N with a glow)` with the two numbers equal |
| The glow around a head is still a street-wide wash, or has vanished altogether | `[trafficlights] corona_radius` is the radius we leave the game's own signal corona, which ships at 15.0 = a 30 m flare: 0.45 default, 1.0 for a bigger halo, 0 to leave the game's code alone. Our own spill is `glow_radius`/`glow_flux`, not this |
| A head is lit in a colour the game is not showing, or the lights land on the pole | `[trafficlights] source=list` puts back the pre-s01 behaviour (one light per list line, colour from a local cycle) and the log line says why the hook is off |
| A list-mode signal sits on the mast instead of on the lens | `[trafficlights] lens_offset=0` puts the old `x,y,z+headz` back; with 1 (the default) the light goes to `instance + rotate(heading, lens)`. It only moves when the file carries the v2 columns - the `loaded N lights ... (M carry the v2 heading/lens columns)` line says how many do, and mtraffic1/2 (562 of the 1522) stay on the mast on purpose: no measured offset covers an 85-unit model |
| Game dies a few seconds after entering the level, `rhyremix.log` stops mid-frame, `rtx-remix/logs/crash-upload.log` says `type=CPU` | the signal-head hook is the first thing to suspect (it patches game code). `[trafficlights] source=list` takes it out; the log line then says `source=list, no code hook`. Fixed 2026-11-11: the stub had to supply the return address the deleted `call` would have pushed - see docs/FINDINGS.md |
| A Wine trace (`logs/steam-12120.log`) full of `c0000005` at `eip=0x6FAAE0` during the level load | **not a crash and not ours**: Open Limit Adjuster's `Coronas = unlimited` moves the corona store to a reserved heap block and commits it page by page from its own vectored handler, so `CCoronas::Init`'s clear loop faults once per page - 478 first-chance exceptions, ~10 ms, handled. `Coronas = 0` in `modloader/OLA/III.VC.SA.LimitAdjuster.ini` makes them go away. See docs/FINDINGS.md |
| Signals go out when you look away and come back slowly | `[trafficlights] head_ttl_ms=4000` (the game only registers the heads it draws, and a head with no corona for 2 s loses its light) |
| A street still appears or disappears in one frame | `[fade] creates_per_frame` is the frame's handle-change budget; raise it (48) if the fade feels sluggish, but a pop means it is already at 0 (unlimited) — set it back to 24. `in_ms`/`out_ms` are the ramp lengths and cannot go under 500 |
| Fades feel slow when driving fast | `[fade] step=0.4` (bigger brightness steps) or `creates_per_frame=48`; both trade smoothness for speed, and the log's `deferred=` tells you which one is binding |
| A traffic signal's colour change looks like a slow cross-fade | `[fade] phase_fade_ms=0` puts back the instant change (a real signal is instant); the light arriving and leaving still fades |
| Lights still vanish for a second on a cab roof / after wasting | `[fade] player_grace_ms=20000` holds the last player position longer. `player_gone_frames=` in the log says how long the pointer is actually unreadable |
| The sun jumps, or the sky changes season by itself | look at `doy=` and `cal:` on the `sky:` line. If `doy=` ever differs from `[sun] day_of_year`, the game's date bytes passed the stability test - raise `[clock] calendar_stable_reads` (30 = 30 s of a still date) or set `calendar_stable_reads=0` to ignore them entirely |
| Time of day still steps instead of sliding | `tod=` must show decimals and change every line. If it does not, `[clock] ms_per_game_minute` is far from the real tick length - `ms/min=` in the log is what this machine actually measures |
| The tour makes the sky jump backwards | `jitter=` should absorb it; if `reanchor=` climbs every sample instead, `[clock] jitter_tolerance_min` (8) is smaller than the tour's pin - raise it, or `[clock] reanchor_ms` (2000) so a real jump takes longer to be believed |
| The sun's shadow edge still jumps instead of creeping | `cad=` and `step<=` on the `sky:` line decide whether this is ours. `step<=` near 1.0 means the cadence is not running (check the `config: fade ... handle=` startup line); `step<=` near 0.25 and the jump is the renderer's - try `tools/rtxprofile.sh nopops` |
| Lighting changes now arrive late (a lamp takes a quarter second to reach full brightness) | `[fade] handle_cadence_ms=1000` and `handle_cadence_min_ms=100` on our side; on the renderer side `tools/rtxprofile.sh current` (the A/B's accumulation windows are what add the lag) |
| Head light sits just inside the lens instead of on it | `[trafficlights] head_normal=1 head_forward=0.15` - the 2DFX entry has no direction, so this uses the model's own "at" axis and may point the wrong way; it is a knob to A/B, not a fix |
| The plugin log is huge, the tools take forever, or the run's lines have "disappeared" | n03: they are in `scripts/rhyremix.log.prev` - the launcher moves it there when the live log passes `RHYS_LOG_MAX_BYTES` (8 MiB) and keeps exactly one generation. `diagnose.sh` and `tour-report.py` read both; `grep -h . "$G"/scripts/rhyremix.log.prev \| tail -40` is the manual version. Raise the cap with `RHYS_LOG_MAX_BYTES=…`, and `logs/steam-12120.log` (52 MB a run) with `RHYS_PROTON_LOG_KEEP` or `RHYS_PROTON_LOG=0` |
| 60 fps most of the time, but the run hitches more than the previous build, especially after ~5 minutes | n08. Ours: `[fade] destroys_per_frame=32` bounds the handles one frame may release (the 2026-11-12 runs measured 320 and 205 released inside a single frame); `0` puts the old one-frame flush back. The renderer's half is the six opacity-micromap work bounds `tools/rtxprofile.sh perf` installs (build/bake 150 M micro-triangles/s, texel-density 512/frame, 2000 staged requests behind 30-frame/30-use filters). Measure it, don't feel it: `python3 tools/perf-report.py <game>/logs/perf/NvRemixBridge_<stamp>.csv --log <game>/scripts/rhyremix.log --gpu-csv <game>/logs/perf/gpu-<stamp>.csv --series` — the number is **spikes/min after the first 300 s** (90.6 in the good run, 187.8 in the regressed one), and `due_destroy=`/`deferred_destroy=` on the `remix:` line say whether our bound is biting |

## Step 5 - send back

```bash
cp /media/rhy/data/Gamez/GTASA-RTX/scripts/rhyremix.log /tmp/rhyremix.log
# n03: if the launcher rotated the log during/before the run, send the other half too:
cp /media/rhy/data/Gamez/GTASA-RTX/scripts/rhyremix.log.prev /tmp/rhyremix.log.prev 2>/dev/null
grep -c 'Invalid light handle' /media/rhy/data/Gamez/GTASA-RTX/rtx-remix/logs/bridge64.log
# frame pacing (n08): the MangoHud CSV and the nvidia-smi trace, then the report
# (start the trace BEFORE the run - nothing in this repo starts it for you:
#  nvidia-smi --query-gpu=timestamp,memory.used,memory.total,utilization.gpu,temperature.gpu,
#    power.draw,clocks.current.sm,clocks_event_reasons.active --format=csv -l 1 \
#    > "$G/logs/perf/gpu-$(date +%Y%m%d-%H%M%S).csv" &)
cp /media/rhy/data/Gamez/GTASA-RTX/logs/perf/NvRemixBridge_*.csv /tmp/
cp /media/rhy/data/Gamez/GTASA-RTX/logs/perf/gpu-*.csv /tmp/
python3 tools/perf-report.py /tmp/NvRemixBridge_<stamp>.csv --log /tmp/rhyremix.log \
    --gpu-csv /tmp/gpu-<stamp>.csv --series --top 8
```

The log is what makes the next round fast: it contains the exact radii,
radii and cone angles every created light was given, so a picture plus that log
is enough to diagnose without guessing. The two `logs/perf/` files are what make
a pacing report possible: MangoHud's own `gpu_vram_used` column is dead for this
process (it samples the 32-bit bridge client), so VRAM has to come from the
launcher's `gpu-*.csv` (the name the 2026-11-12 runs used) — start one yourself with the
`nvidia-smi --query-gpu=… -l 1` line above.

## Reverting

```bash
# rtx.conf back to exactly what shipped with the mod
cp /media/rhy/data/Gamez/GTASA-RTX/rtx.conf.orig /media/rhy/data/Gamez/GTASA-RTX/rtx.conf

# physical fixture sizes off (back to the 15 game unit sphere) - ini only, live
sed -i 's/^fixture_size=1/fixture_size=0/' /media/rhy/data/Gamez/GTASA-RTX/scripts/rhyremix.ini

# line-spectra off, back to the kelvin dial
sed -i 's/^lamp_warm=hps/lamp_warm=/; s/^lamp_cold=mercury/lamp_cold=/' \
    /media/rhy/data/Gamez/GTASA-RTX/scripts/rhyremix.ini
```

`fixture_size=0` and empty `lamp_warm`/`lamp_cold` reproduce the previous
behaviour exactly, so any difference between the two states is attributable.

---

## What is NOT covered by this test

**Street lamps are lit once, by rhyremix (decided 2026-11-11).** The asset
pack's lamp models carried their own SphereLights
(`SARemix/SARemix_StreetLights.usda`: `Spot_Light_*` with two each,
`Sphere_Light_*` with one, 18 in all) on top of our 7002-entry list, so every
lamp was lit twice and night read twice as bright. rhY chose to keep ours (real
fixture sizes, sodium/mercury colours, twilight switching): `deploy.sh` and
`apply.sh` run `tools/strip_pack_lamp_lights.py`, which removes the pack's 18
lights and keeps the original as `.orig-with-lights` (`--restore` puts it back).
Remix reads USD at start-up, so it takes effect on the next launch.

**Traffic signals are unshaped on purpose.** A real signal module throws a narrow
beam down the approach, but `rhyremix_traffic.txt` carries only `x y z headz` -
no heading. A wrongly-aimed cone would put a dark face on every signal seen from
the cross street, which is worse than an unshaped light. Fixing it means adding a
heading column derived from the IPL placement rotation.

**Shopfronts are left to the asset pack deliberately.** Its 67 `RectLight` prims
across 39 `Light/*.usda` setups are the correct primitive for a window; adding a
point light in front of an already-glowing window double-counts the same surface
and destroys the falloff. Only the gain was touched (`emissiveIntensity`).
