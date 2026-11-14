# NEXT-TEST — the next in-game session, in this order

Nine steps, one variable each; `$G` = the game folder (step 0). Each: run -> look -> report -> rollback. Reference tour:
2026-11-12 00:49. `scripts/diagnose.sh` re-reads logs; send back `report.md`, `rhyremix.log`, `sheet-*.png`. Baselines and the
after-numbers each task owes: the "what to test next" list in [STATUS.md](STATUS.md).
## 0. preflight — 30 s, changes nothing
```bash
G=/media/rhy/data/Gamez/GTASA-RTX
python3 tools/laa.py $G/gta-sa.exe --check
bash tools/rtxprofile.sh status
cp plugin/lua/SARemix_Tour.lua $G/moonloader/SARemix_Tour.lua
```
Look: `Large Address Aware SET` (step 6 needs it); `profile : unknown` = rtx.conf is hand-tuned.
Rollback: none for the `cp`.
## 1. baseline tour — nothing new
```bash
bash scripts/test-session.sh --tour
python3 tools/tour-report.py $G --since 21:30    # 21:30 = the minute you started
grep -m1 'config: tonemap exposure' $G/scripts/rhyremix.log
```
Look: launcher prints `plugin : <today's build>`, `traffic: ... source=game`, `water : remix-ref`, and the log has
`remix: remixapi_InitializeLibrary v0.6.5 -> 0` (without it the card was not free). Report: `paired | 16 of 16 manifest entries`;
to beat (STATUS): 00 128.6, 03 26.3 / 6.6 % black, 11 6.8 / 29.2 %, 13 134.2. The dump must carry
`day=-0.50 EV night=+0.50 EV`, `enableFogRemap = 1`, `roughnessConstant = 0.65`. Rollback: none.
## 2. lamps that ignore the camera — finding A, the priority one (n04)
```bash
bash scripts/abtest-lights.sh lights on $G && bash tools/rtxprofile.sh current $G
```
At night, somewhere dense (Columbus pier, 21:00): settle 5 s, then **spin the camera twice without moving CJ**, then
walk a minute and drive a lit street. Look: which lamps are lit may not change during the spin; a lamp you walk away from dims
and stays dim, a signal you turn your back on keeps its brightness and colour. Report:
`grep -E 'streetlights:|trafficlights\[' $G/scripts/rhyremix.log | tail -20` — `in_drop=` **0 in every line**, `active` constant.
Rollback: `bash tools/rtxprofile.sh restore $G`.
## 3. signal heads: the bulb AND the glow (s30 + n02 + n02b)
```bash
grep -E 'signalhook|trafficlights\[|config: traffic' $G/scripts/rhyremix.log | tail -6
```
Look: at each lit head **both** — a bright bulb on the lens AND a soft red/amber/green glow on pole, wall and road, not a bare
dot; the log wants an `ok bulb` AND an `ok glow` per lit signal. Report: stops 06/07 — green pixels above luma 200 must
appear (the bulb; none on 2026-11-12) and the road/wall green stay near that run's level. Rollback: live ini `[trafficlights]`:
`glow_flux=0` = bulb only, `corona_radius=0` = game flare, `source=list`.
## 4. water — the research variant is the DEFAULT now (s02, s32, n07, n13)
```bash
bash tools/water.sh status $G             # expect: remix-ref, keys ok
bash tools/water.sh realistic $G
bash tools/water.sh calm $G
bash tools/water.sh default $G --force    # back to the default (the A/B was explicit)
```
Look: the sea at Santa Maria (stops 10/11) - no black/white speckle, the horizon a line, not noise. **n13 installs
`remix-ref`, its keys and hashes on every start**, your pick in `.rhys-water/state`. Report: 11-water mean up from 6.8, black
down from 29.2 %. Rollback: `bash tools/water.sh pack $G` (sticks) + `cp $G/rtx.conf.bak-before-n13 $G/rtx.conf`.
## 5. the 60 fps profile (s22) + the DLSS 5 probe (n19) + the frame tail (n08)
```bash
bash tools/rtxprofile.sh perf $G
bash tools/rtxprofile.sh dlss5 $G && grep -i DLSS-NR $G/rtx-remix/logs/remix-dxvk.log
python3 tools/perf-report.py $G/logs/perf/NvRemixBridge_*.csv --log $G/scripts/rhyremix.log --series
```
Look: MangoHud - ~60 fps at 1080p, nothing under ~45 in Pershing or on the hill. Report: every stop's mean within ~10 % of
step 1 (>20 % = a look change, not fps); that grep must still print `0xbad00010` (DLSS 5 unavailable, n19); and beat
**spikes/min after 300 s** — 90.6 (00:48) against **187.8** (10:22). Keep `logs/perf/gpu-*.csv` (only VRAM source).
Rollback: `bash tools/rtxprofile.sh restore $G`.
## 6. vegetation: the pack, its cards and its typo (s03, s16, s32, n21)
```bash
bash tools/vegetation.sh check $G
bash tools/vegetation.sh on $G --step 6
python3 tools/vegetation_edge_measure.py $G/screenshots/shots/*.png --name vegetation
```
Look: stop 14-vegetation-vinewood-noon, then two minutes on the hillside: cards read as leaves and blades, not scribbled
lines; **no transparent or missing trees**; no freeze in the first 60 s. Report: `check` prints the shim pair
`lod_veg_firs.txd=lod_veg_fist.txd` (`--step 5` fixes it) and the pinned MB; at stop 14 `mid_alpha` below **17.63 %**,
ramp below **2.59 px**, mean/sat up from 168.1 / 0.262. Rollback: `bash tools/vegetation.sh off $G`.
## 7. character packs (s17)
```bash
bash tools/characters.sh on $G
```
Look: stop 13-ped-crowd-pershing-noon — faces and hands at 2 m; CJ's PBR is keyed to four texture hashes, so a retex can
leave him matte (s25). Report: stop 13 sat up (clothing; step 1: 0.264), mean within ~10 % of 134.2. Rollback:
`bash tools/characters.sh off $G` (`on` refuses until the packs are staged).
## 8. HD PBR on the 40 surfaces you look at (n20 → n20b)
```bash
bash tools/textures.sh status $G
bash tools/textures.sh on $G --generate      # 35 sets / 140 DDS / 92 MB, built once into build/textures
```
Look: Pershing Square at noon (stop 13) and the Grove sidewalk (stop 0) — the stucco of `tanstucco1_LA`, the marble of
`badmarb1_LAn` and the kerbs must carry relief and a dry matte response, not step 1's painted flatness, and the crossing and
stop lines must still be there (the albedo is the game's own raster, 4x upscaled; why and what the pack already covers better:
[TEXTURES.md](TEXTURES.md)). Report: stops 00/13 mean/sat vs step 1, plus frame time and VRAM (`gpu-*.csv`, step 5) — +91 MB
on a card using 3.5-4.4 GB. Rollback: `bash tools/textures.sh off $G` deletes its files and restores `mod.usda` byte-exact.
