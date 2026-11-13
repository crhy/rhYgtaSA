# NEXT-TEST — the next in-game session, in this order

Seven steps, one variable each, from `~/rhYgtaSA`; `$G` is the game folder (set in step 0). Each step: run -> look -> read the
report -> know the rollback. Reference: the 2026-11-12 00:49 tour (16 stops). After any step `bash scripts/diagnose.sh` re-reads
the logs and changes nothing. Send back `report.md`, `sheet-*.png` from `$G/screenshots/report-<start>/`, `rhyremix.log`.
## 0. preflight — 30 s, changes nothing
```bash
G=/media/rhy/data/Gamez/GTASA-RTX
python3 tools/laa.py $G/gta-sa.exe --check
bash tools/rtxprofile.sh status
cp plugin/lua/SARemix_Tour.lua $G/moonloader/SARemix_Tour.lua
```
Look: `Large Address Aware SET` (step 6 depends on it) and `profile : unknown` — expected, the live rtx.conf is the hand-tuned
file. The `cp` matters: **nothing in `deploy.sh` installs the tour script**, so without it steps 6 and 7 produce no numbers.
Rollback: none for the `cp`.
## 1. baseline tour — nothing new
```bash
bash scripts/test-session.sh --tour
python3 tools/tour-report.py $G --since 21:30    # 21:30 = the minute you started
```
Look: the launcher prints `plugin : <today's build>` and `traffic: ... source=game`; `rhyremix.log` must contain `remix:
remixapi_InitializeLibrary v0.6.5 -> 0` — without it the card was not free. Report: `paired | 16 of 16 manifest entries`;
numbers to beat: 00-baseline 128.6, 03-sodium 26.3 / 6.6 % black, 11-water 6.8 / 29.2 %, 13-ped 134.2, 14-veg 168.1.
Rollback: nothing changed — this is the reference run.

## 2. lamps that ignore the camera — finding A, the priority one (n04)
```bash
bash scripts/abtest-lights.sh lights on $G && bash tools/rtxprofile.sh current $G
```
At night somewhere dense (Columbus pier, Vinewood 21:00): let it settle 5 s, then **spin the camera a full circle twice without
moving CJ**; then walk a minute and drive a lit street at 60 km/h.
Look: nothing about which lamps are lit may change during the spin. A lamp you walk away from dims and stays dim; a signal you
turn your back on keeps its brightness and its colour.
Report: `grep -E 'streetlights:|trafficlights\[' $G/scripts/rhyremix.log | tail -20` — `in_drop=` must be **0 in every line**,
`active` constant through the spin. The load line `streetlights: worst case N lamps within max_distance=300 ... cap 700` says
whether the cap covers where you are. Rollback: `bash tools/rtxprofile.sh restore $G`.

## 3. signal heads: the bulb AND the glow (s30 + n02 + n02b)
```bash
grep -E 'signalhook|trafficlights\[|config: traffic' $G/scripts/rhyremix.log | tail -6
```
Look: at each lit head **both** — a small bright bulb on the lens, AND a soft red/amber/green glow colouring pole, wall, road
and peds a few metres around it (rhY: "both like real life"). Not a 30 m wash, not a bare dot. Log: `glow radius=3.00
flux=304 (cap 3473 = one street luminaire)`, an `ok bulb` AND an `ok glow` line per lit signal, `N heads lit (N with a glow),
N bulbs + N glows drawn`. Report: stops 06/07 — green pixels above luma 200 must appear (the bulb; none on 2026-11-12) and the
road/wall green stay near the 2026-11-12 level (the glow, wanted). Rollback: live ini edits under `[trafficlights]`:
`glow_flux=0` = bulb only, `corona_radius=0` = the game's own 15 m flare, `source=list` = no hook.

## 4. water — the research variant, then the A/B (s02, s32, n07)
```bash
bash tools/water.sh remix-ref $G
bash tools/water.sh realistic $G
bash tools/water.sh calm $G
```
Look: the sea at Santa Maria (stops 10/11, pier deck (378.5,-1963.9)) — no black/white speckle, foam reads as foam, the horizon
a line not a wall of noise. `remix-ref` is n07 (research: notes-n07 in .strata/): a regenerated sheet (median facet
tilt 44 deg -> 6 deg), transmittance (0.62,0.8,0.8) @ 4 m, a 12 fps flipbook, and both water hashes now in
`rtx.animatedWaterTextures`, which turns on the runtime's UV scroll + second LOD-blurred normal. Seam flicker: the sea is 307
exactly-butted coplanar polygons, ~6000 seams (`$G/data/water.dat`); 1.6.1's answer is the calmer map + kept history +
`combineSpecularAlbedo` — GTA IV's shoreline fade needs a fork runtime (`rtx.water.*` is not in our dll).
Report: 11-water-santa-maria-2100 mean up from 6.8, black down from 29.2 %. Rollback: `bash tools/water.sh pack $G`; both
profiles set `rtx.translucentMaterial.normalIntensity = 1.0` (0.35 for `realistic`/`calm`).

## 5. the 60 fps profile (s22)
```bash
bash tools/rtxprofile.sh perf $G
```
Look: MangoHud — ~60 fps at 1080p, no dip under ~45 in Pershing Square or on the hill. Report: every stop's mean within ~10 % of
step 1. A stop that moves >20 % names a key that changed the look, not the fps. Rollback: `bash tools/rtxprofile.sh restore $G`.

## 6. vegetation: hair cards, then the LAA test (s03, s16, s32)
```bash
bash tools/vegetation.sh on $G
python3 tools/vegetation_hashes.py $G/models/gta3.img --emit rtx.hairCardTextures
```
Look: stop 14-vegetation-vinewood-noon (the lawn at (1300,-920), trees at 8-35 m), then two minutes on the hillside — the cards
must read as leaves and blades, not soft scribbled lines, and no freeze or hitch. The shipped rtx.conf tags 119 foliage card
textures as `rtx.hairCardTextures` (cutouts, not alpha blends). If stop 14 is *identical* to
step 1 the 2402-char line was not parsed. Report: stop 14 mean and sat above step 1's (168.1 / 0.262), no `stop 14 failed:`.
Rollback: `bash tools/vegetation.sh off $G` (hashes back byte-exact); the hair cards: delete that line.
## 7. character packs (s17)
```bash
bash tools/characters.sh on $G
```
Look: stop 13-ped-crowd-pershing-noon — faces and hands at 2 m; CJ: the pack's PBR is keyed to his four texture hashes, so a
retex can leave him matte (note it, that is s25). Report: stop 13 sat up (clothing; step 1: 0.264) and mean within ~10 % of
134.2; a big mean drop on CJ is s25's symptom. Rollback: `bash tools/characters.sh off $G`. `on` refuses until packs are staged
in `/media/rhy/data/Gamez/GTAmods/x/characters/stage`.
