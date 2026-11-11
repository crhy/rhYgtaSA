# Testing the lighting changes

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
| 6 | A signalised intersection, close, all three aspects visible | the lens fix | red is **red**, not orange; green is distinctly **blue-green**; amber sits between. Radii should be small points, not glowing blobs |
| 7 | Same intersection from the cross street | the unshaped-light limitation | both faces readable. If one face goes dark, that is the missing heading column (see below) |
| 8 | Sunset, facing east | the solar model + Beer-Lambert | horizon glow strongly red/orange, zenith still blue. The red comes from path length, it is not tuned |
| 9 | Any street at ~05:30 with lamps still on | the twilight switching | lamps on before the sun clears the horizon, off shortly after |

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
| Signals too bright | `[trafficlights] radiance=0.8` |

## Step 5 - send back

```bash
cp /media/rhy/data/Gamez/GTASA-RTX/scripts/rhyremix.log /tmp/rhyremix.log
grep -c 'Invalid light handle' /media/rhy/data/Gamez/GTASA-RTX/rtx-remix/logs/bridge64.log
```

The log is what makes the next round fast: it contains the exact radii,
radii and cone angles every created light was given, so a picture plus that log
is enough to diagnose without guessing.

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
