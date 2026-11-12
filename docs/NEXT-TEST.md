# NEXT-TEST — the next in-game session, in this order

Six steps, one variable each, from `~/rhYgtaSA`; `$G` is the game folder (set in step 0). Each step: run -> look ->
read the report -> know the rollback. Reference numbers: the 2026-11-12 00:49 tour (16 stops), newest in the manifest.
After any step `bash scripts/diagnose.sh` re-reads the logs and changes nothing. Send back `report.md` + `sheet-*.png`
from `$G/screenshots/report-<start>/`, `$G/scripts/rhyremix.log`, and if it died `rtx-remix/logs/crash-upload.log` + `bridge64.log`.

## 0. preflight — 30 s, changes nothing
```bash
G=/media/rhy/data/Gamez/GTASA-RTX
python3 tools/laa.py $G/gta-sa.exe --check
bash tools/rtxprofile.sh status
cp plugin/lua/SARemix_Tour.lua $G/moonloader/SARemix_Tour.lua
```
Look: `Large Address Aware SET` (step 5 depends on it) and `profile : unknown` — expected, the live rtx.conf is the
hand-tuned file (shipped + the three `rtx.opacityMicromap.*` keys).
Rollback: none for the `cp` — the repo copy holds the 16 stops with the s31 fixes to 05/10/11/12/13/14, and
**nothing in `deploy.sh` installs it**: without this line steps 5 and 6 produce no numbers at all.

## 1. baseline tour — nothing new
```bash
bash scripts/test-session.sh --tour
python3 tools/tour-report.py $G --since 21:30    # 21:30 = the minute you started; --since picks the newest run in the manifest
```
Look: the launcher prints `plugin : <today's build>` and `traffic: ... source=game`; then `$G/scripts/rhyremix.log` must
contain `remix: remixapi_InitializeLibrary v0.6.5 -> 0` — without it the card was not free (docs/TESTING.md).
Report: `paired | 16 of 16 manifest entries`, `run | 2026-11-12 00:49:09 .. end of file`; numbers to beat: 00-baseline
128.6, 03-sodium-lamp 26.3 / 6.6 % black, 11-water-2100 6.8 / 29.2 %, 12-car-mirror 145.1, 13-ped 134.2, 14-veg 168.1.
Rollback: nothing changed — this is the reference run.

## 2. signal heads: bulbs, not a green street (s01 + s19 + s30)
```bash
grep -E 'signalhook|trafficlights\[|config: traffic' $G/scripts/rhyremix.log | tail -6
```
Look: a **small bright bulb at each lens** - not a glow around the pole, not a green wash on road and walls. Red red,
green blue-green, amber between. The sodium-lamp stops (0-5) must be unchanged: they are the control.
Log: `| bulb radius=0.100 radiance=1200 nits (fixture_size=1) corona_radius=0.45`, `signalhook: patched 0x0049DD4C ...
corona resized to a bulb`, `corona radius 15.00 -> 0.45 game units at 0x0049DCDA`, `head light ok ... radius=0.10
flux=67 (bulb: lens nits)`, `N heads lit ... hook: calls=N records=N dropped=0 bad=0 reentrant=0`. `calls>0 records=0`
is our bug, not a quiet street (after 20 s it lights the list instead); `the bytes at 0x49DD4C are not 'call 0x6FC580'`
too - the bytes there are `E8 2F E8 25 00`.
Report: stops 06/07 green 5.711 % / 21.158 % on 2026-11-11 (we created **no** signal lights) and 33.4 % / 3.9 % on
2026-11-12 (60 list lights, 5400 nits) - both with **zero green pixels above luma 200**: wash, no bulb; s30 inverts that.
Rollback: live ini edits under `[trafficlights]`. `corona_radius=0` puts the game's own 15 m flare back,
`bulb_radiance=0` turns our signal lights off, `source=list` takes the hook out; undo by deleting the line.

## 3. water — realistic vs calm (s02)
```bash
bash tools/water.sh realistic $G
bash tools/water.sh calm $G
```
Look: the sea at Santa Maria (stops 10/11, since s31 on the pier deck at (378.5,-1963.9), not on the wet sand) — no
binary black/white speckle, foam reads as foam, the horizon is a line and not a wall of noise. `realistic` is already
installed (sha-checked), so this step is the A/B.
Report: 11-water-santa-maria-2100 mean up from 6.8 and black down from 29.2 % — that is
`rtx.translucentMaterial.normalIntensity = 0.35` working; calm vs realistic moves the mean a few points, never doubles it.
Rollback: `bash tools/water.sh pack $G` (the pack's own material).

## 4. the 60 fps profile (s22)
```bash
bash tools/rtxprofile.sh perf $G
```
Look: MangoHud — ~60 fps at 1080p, no dip under ~45 in Pershing Square or on the Vinewood hill. One frame of GI lag when
a lamp switches is this profile's price, not a bug.
Report: every stop's mean within ~10 % of step 1 (SHaRC/NRC to Medium, `pathMinBounces` 1, froxel grid /2, texture budget
40 %, OMM cache 768 MB behind 2 GB free). A stop that moves >20 % names a key that changed the look, not the frame rate.
Rollback: `bash tools/rtxprofile.sh restore $G` (byte-exact).

## 5. vegetation, now that the exe is LAA (s03, s16)
```bash
bash tools/vegetation.sh on $G
```
Look: stop 14-vegetation-vinewood-noon (since s31 the lawn at (1300,-920), trees at 8-35 m), then two minutes on the
hillside — no freeze, no hitch, `rhyremix.log` keeps its ~10 s cadence. `on` rewrites the OMM keys to 256/3072: expected,
`off` puts them back.
Report: stop 14 mean and sat above step 1's (168.1 / 0.262, more leaves in frame), and no `stop 14 failed:` line in the
manifest.
Rollback: `bash tools/vegetation.sh off $G` (hashes back byte-exact).

## 6. character packs (s17)
```bash
bash tools/characters.sh on $G
```
Look: stop 13-ped-crowd-pershing-noon — faces and hands at 2 m; since s31 it looks along the bench walkway and settles
25 s, because peds arrive over tens of seconds — and CJ: the pack's PBR is keyed to his four texture hashes, so a retex
can leave him matte; write the number down, do not fix it here (that is s25).
Report: stop 13 sat up (clothing; step 1: 0.264) and mean within ~10 % of 134.2; a big mean drop on CJ is the s25 symptom.
Rollback: `bash tools/characters.sh off $G`.
Note: `on` refuses today — nothing is staged in `/media/rhy/data/Gamez/GTAmods/x/characters/stage` (measured 2026-11-12)
and it prints the six packs and their author pages rather than install half a set. Stage them, then re-run this step.
