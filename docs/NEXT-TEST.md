# NEXT-TEST — the next in-game session, in this order

Seven steps, one variable each, from `~/rhYgtaSA`; `$G` is the game folder (step 0). Each: run -> look -> report -> rollback.
Reference tour: 2026-11-12 00:49. `scripts/diagnose.sh` re-reads logs; send back `report.md`, `rhyremix.log`, `sheet-*.png`.
## 0. preflight — 30 s, changes nothing
```bash
G=/media/rhy/data/Gamez/GTASA-RTX
python3 tools/laa.py $G/gta-sa.exe --check
bash tools/rtxprofile.sh status
cp plugin/lua/SARemix_Tour.lua $G/moonloader/SARemix_Tour.lua
```
Look: `Large Address Aware SET` (step 6 needs it) and `profile : unknown` (the live rtx.conf is hand-tuned).
Rollback: none for the `cp`.
## 1. baseline tour — nothing new
```bash
bash scripts/test-session.sh --tour
python3 tools/tour-report.py $G --since 21:30    # 21:30 = the minute you started
grep -m1 'config: tonemap exposure' $G/scripts/rhyremix.log
```
Look: the launcher prints `plugin : <today's build>`, `traffic: ... source=game` and `water : remix-ref`; `rhyremix.log` must
contain `remix: remixapi_InitializeLibrary v0.6.5 -> 0` — without it the card was not free. Report: `paired | 16 of 16
manifest entries`; numbers to beat: 00-baseline 128.6, 03-sodium 26.3 / 6.6 % black, 11-water 6.8 / 29.2 %, 13-ped 134.2.
Also the grading run n16+n22 owe (`tools/target-gap.py`, docs/TARGET.md §6): `tonemap` must read
`day=-0.50 EV night=+0.50 EV`, the 10 s sky line `exposure=+0.500 EV` at a 21:00 stop. Night wrong -> edit `[tonemap]
night_ev` live, rollback `apply=0`. Rollback: nothing changed — this is the reference run.

## 2. lamps that ignore the camera — finding A, the priority one (n04)
```bash
bash scripts/abtest-lights.sh lights on $G && bash tools/rtxprofile.sh current $G
```
At night, somewhere dense (Columbus pier, Vinewood 21:00): settle 5 s, then **spin the camera two full circles without
moving CJ**; then walk a minute and drive a lit street at 60 km/h.
Look: nothing about which lamps are lit may change during the spin. A lamp you walk away from dims and stays dim; a signal you
turn your back on keeps its brightness and its colour.
Report: `grep -E 'streetlights:|trafficlights\[' $G/scripts/rhyremix.log | tail -20` — `in_drop=` must be **0 in every line**,
`active` constant through the spin; the load line `worst case N lamps ... cap 700` says if the cap covers you.
Rollback: `bash tools/rtxprofile.sh restore $G`.

## 3. signal heads: the bulb AND the glow (s30 + n02 + n02b)
```bash
grep -E 'signalhook|trafficlights\[|config: traffic' $G/scripts/rhyremix.log | tail -6
```
Look: at each lit head **both** — a small bright bulb on the lens, AND a soft red/amber/green glow colouring pole, wall and road
(rhY: "both like real life"), not a bare dot. Log: an `ok bulb` AND an `ok glow` line per lit signal. Report: stops 06/07 —
green pixels above luma 200 must appear (the bulb; none on 2026-11-12) and the road/wall green
stay near the 2026-11-12 level (the glow). Rollback: live ini edits under `[trafficlights]`: `glow_flux=0` = bulb only,
`corona_radius=0` = the game's own 15 m flare, `source=list` = no hook.

## 4. water — the research variant is the DEFAULT now (s02, s32, n07, n13)
```bash
bash tools/water.sh status $G             # expect: remix-ref, keys ok
bash tools/water.sh realistic $G
bash tools/water.sh calm $G
bash tools/water.sh default $G --force    # back to the default (the A/B was explicit)
```
Look: the sea at Santa Maria (stops 10/11, pier deck (378.5,-1963.9)) — no black/white speckle, foam as foam, the horizon a line
not a noise wall. `remix-ref` is n07's sheet (facet tilt 44 -> 6 deg, transmittance (0.62,0.8,0.8) @ 4 m, 12 fps flipbook), and
**n13 installs it plus the animated-water keys and both water hashes on every start and in `apply.sh`**;
your pick is remembered in `.rhys-water/state`. Edge flicker: the sea is ~6000 coplanar seams (`$G/data/water.dat`); 1.6.1's
answer is the calmer map + kept history + `combineSpecularAlbedo`. Report: 11-water mean up from 6.8, black down from 29.2 %.
Rollback: `bash tools/water.sh pack $G` (sticks) + `cp $G/rtx.conf.bak-before-n13 $G/rtx.conf`.

## 5. the 60 fps profile (s22)
```bash
bash tools/rtxprofile.sh perf $G
```
Look: MangoHud — ~60 fps at 1080p, no dip under ~45 in Pershing Square or on the hill. Report: every stop's mean within ~10 % of
step 1. A stop that moves >20 % names a look change, not fps. Rollback: `bash tools/rtxprofile.sh restore $G`.

## 6. vegetation: the pack, its cards, its typo, its wind shader (s03, s16, s32, n21)
```bash
bash tools/vegetation.sh check $G
bash tools/vegetation.sh on $G --step 6
python3 tools/vegetation_edge_measure.py $G/screenshots/shots/*.png --name vegetation --json build/tmp/veg.json
```
Look: stop 14-vegetation-vinewood-noon (the lawn at (1300,-920), trees at 8-35 m), then two minutes on the hillside. Cards must
read as leaves and blades, not scribbled lines; **no transparent or missing trees** (the pack is v2.00, wind-adapted; this
install has no SkyGfx Extended / Proper Fixes v2 — `check` names it); no freeze in the first 60 s.
Report: `check` prints the shim pair `lod_veg_firs.txd=lod_veg_fist.txd` (the pack's one-letter typo, fixed by `--step 5`) and
the pinned MB (92.0 without the pack's IDEs, 187.5 with). At stop 14, `mid_alpha` below **17.63 %** and ramp below **2.59 px**
(2026-11-12 10:28 baseline), mean/sat up from 168.1 / 0.262. Wind needs Proper Fixes v2 or SkyGfx Extended: 1.6.1 has
no `rtx.*wind*` key. Rollback: `bash tools/vegetation.sh off $G` (every byte back).
## 7. character packs (s17)
```bash
bash tools/characters.sh on $G
```
Look: stop 13-ped-crowd-pershing-noon — faces and hands at 2 m; CJ's PBR is keyed to his four texture hashes, so a retex can
leave him matte (s25). Report: stop 13 sat up (clothing; step 1: 0.264) and mean within ~10 % of
134.2; a big mean drop on CJ is s25's symptom. Rollback: `bash tools/characters.sh off $G`. `on` refuses until packs are staged
under `GTAmods/x/characters/stage`.
