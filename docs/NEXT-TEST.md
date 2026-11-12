# NEXT-TEST — the next in-game session, in this order

Six steps, one variable each, from `~/rhYgtaSA`; `$G` is the game folder (set in step 0). Each step: run -> look -> read the
report -> know the rollback. Reference numbers: the 2026-11-12 00:49 tour (16 stops), newest in the manifest. After any step
`bash scripts/diagnose.sh` re-reads the logs and changes nothing. Send back `report.md`, `sheet-*.png` from
`$G/screenshots/report-<start>/` and `$G/scripts/rhyremix.log`.

## 0. preflight — 30 s, changes nothing
```bash
G=/media/rhy/data/Gamez/GTASA-RTX
python3 tools/laa.py $G/gta-sa.exe --check
bash tools/rtxprofile.sh status
cp plugin/lua/SARemix_Tour.lua $G/moonloader/SARemix_Tour.lua
```
Look: `Large Address Aware SET` (step 5 depends on it) and `profile : unknown` — expected, the live rtx.conf is the hand-tuned
file and lacks the s32 hair-card line. The `cp` matters: **nothing in `deploy.sh` installs the tour script**, so without it
steps 5 and 6 produce no numbers. Rollback: none for the `cp`.

## 1. baseline tour — nothing new
```bash
bash scripts/test-session.sh --tour
python3 tools/tour-report.py $G --since 21:30    # 21:30 = the minute you started; --since picks the newest run
```
Look: the launcher prints `plugin : <today's build>` and `traffic: ... source=game`; `rhyremix.log` must contain `remix:
remixapi_InitializeLibrary v0.6.5 -> 0` — without it the card was not free (docs/TESTING.md). Report: `paired | 16 of 16
manifest entries`; numbers to beat: 00-baseline 128.6, 03-sodium-lamp 26.3 / 6.6 % black, 11-water-2100 6.8 / 29.2 %,
12-car-mirror 145.1, 13-ped 134.2, 14-veg 168.1. Rollback: nothing changed — this is the reference run.

## 2. signal heads: the bulb AND the glow (s30 + n02 + n02b)
```bash
grep -E 'signalhook|trafficlights\[|config: traffic' $G/scripts/rhyremix.log | tail -6
```
Look: at each lit head **both** — a small bright bulb on the lens, AND a soft red/amber/green glow colouring pole, wall, road
and peds a few metres around the head (rhY: "both like real life"). Not a 30 m wash, not a bare dot. Stops 0-5 (sodium) are
the control. Log: `glow radius=3.00 flux=304 (cap 3473 = one street luminaire)`, an `ok bulb` line AND an `ok glow` line per
lit signal, `N heads lit (N with a glow), N bulbs + N glows drawn`, `signalhook: patched 0x0049DD4C`, `corona radius 15.00 ->
0.45`. Report: stops 06/07 — green pixels above luma 200 must appear (the bulb; none on 2026-11-12) and the road/wall green
stay near the 2026-11-12 level (the glow, wanted). Rollback: live ini edits under `[trafficlights]`: `glow_flux=0` = bulb only,
`bulb_radiance=0` = no signal lights, `corona_radius=0` = the game's own 15 m flare, `source=list` = no hook.

## 3. water — the regenerated normal sheet, then the A/B (s02, s32)
```bash
bash tools/water.sh soft $G
bash tools/water.sh realistic $G
bash tools/water.sh calm $G
```
Look: the sea at Santa Maria (stops 10/11, pier deck (378.5,-1963.9)) — no binary black/white speckle, foam reads as foam, the
horizon a line not a wall of noise. `soft` is the s32 fix: `inputs:normalmap_texture` points at a sheet this repo regenerates
from the pack's own (median facet tilt 44 deg -> 12 deg, 32 % over 60 deg -> 0 %, tilt gradient 21.3 -> 2.2 deg/texel,
decorrelated after 10 px of a 512 px frame -> 17 px of a 128 px frame); the pack's map is never written over. `realistic` /
`calm` are the A/B, `pack` the control — the mdl has no normal-strength input, so the map is the only place left. Report:
11-water-santa-maria-2100 mean up from 6.8, black down from 29.2 %; the sea's high-pass residual (autocorrelation under 0.5 at
lag 2 px on 2026-11-12) must coarsen — that period is the defect. Rollback: `bash tools/water.sh pack $G`. With `soft` set
`rtx.translucentMaterial.normalIntensity = 1.0` — the live 0.35 multiplies the softening twice and turns the sea to glass.

## 4. the 60 fps profile (s22)
```bash
bash tools/rtxprofile.sh perf $G
```
Look: MangoHud — ~60 fps at 1080p, no dip under ~45 in Pershing Square or on the hill. Report: every stop's mean within ~10 %
of step 1 (SHaRC/NRC Medium, `pathMinBounces` 1, froxel grid /2, texture budget 40 %, OMM cache 768 MB behind 2 GB free). A
stop that moves >20 % names a key that changed the look, not the fps. Rollback: `bash tools/rtxprofile.sh restore $G`
(byte-exact). Both shipped profiles now carry the hair-card line.

## 5. vegetation: hair cards, then the LAA test (s03, s16, s32)
```bash
bash tools/vegetation.sh on $G
python3 tools/vegetation_hashes.py $G/models/gta3.img --emit rtx.hairCardTextures
```
Look: stop 14-vegetation-vinewood-noon (the lawn at (1300,-920), trees at 8-35 m), then two minutes on the hillside — the
cards must read as leaves and blades, not soft scribbled lines, and no freeze or hitch, `rhyremix.log` keeping its ~10 s
cadence. The shipped rtx.conf now tags 119 foliage card textures as `rtx.hairCardTextures` (cutouts instead of alpha blends);
none was in any list before and their alpha is soft (`gras03Si`: 76.5 % transparent / 20.8 % mid / 2.7 % opaque). If stop 14
is *identical* to step 1 the 2402-char line was not parsed — retry the 45-hash procobj subset (notes file). `on` rewrites the
OMM keys to 256/3072; `off` puts them back. Report: stop 14 mean and sat above step 1's (168.1 / 0.262, more leaves in frame),
and no `stop 14 failed:` line. Rollback: `bash tools/vegetation.sh off $G` (hashes back byte-exact); the hair cards: delete
that one line.

## 6. character packs (s17)
```bash
bash tools/characters.sh on $G
```
Look: stop 13-ped-crowd-pershing-noon — faces and hands at 2 m; CJ: the pack's PBR is keyed to his four texture hashes, so a
retex can leave him matte (note it, that is s25). Report: stop 13 sat up (clothing; step 1: 0.264) and mean within ~10 % of
134.2; a big mean drop on CJ is s25's symptom. Rollback: `bash tools/characters.sh off $G`. `on` refuses until packs are staged
in `/media/rhy/data/Gamez/GTAmods/x/characters/stage`.
