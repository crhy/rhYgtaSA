# NEXT-TEST — the next in-game session, in this order

Ten steps, one variable each; `$G` = the game folder (step 0). Each: run -> look -> report -> rollback. Reference tour
2026-11-12 00:49; send back `report.md`, `rhyremix.log`, `sheet-*.png`; baselines: [STATUS.md](STATUS.md).
## 0. preflight — 30 s, changes nothing
```bash
G=/media/rhy/data/Gamez/GTASA-RTX
python3 tools/laa.py $G/gta-sa.exe --check
bash tools/rtxprofile.sh status
cp plugin/lua/SARemix_Tour.lua $G/moonloader/SARemix_Tour.lua
```
Look: `Large Address Aware SET` (step 6 needs it); `profile : unknown` = rtx.conf is hand-tuned. Rollback: none.
## 1. baseline tour — nothing new
```bash
bash scripts/test-session.sh --tour
python3 tools/tour-report.py $G --since 21:30    # 21:30 = the minute you started
grep -m1 'config: tonemap exposure' $G/scripts/rhyremix.log
```
Look: launcher prints `plugin : <today's build>`, `traffic: ... source=game`, `water : remix-ref`, and the log has
`remix: remixapi_InitializeLibrary v0.6.5 -> 0` (without it the card was not free). Report: `paired | 16 of 16 manifest
entries`; to beat (STATUS): 00 128.6, 03 26.3 / 6.6 % black, 11 6.8 / 29.2 %, 13 134.2. Rollback: none.
## 2. lamps that ignore the camera — finding A, the priority one (n04)
```bash
bash scripts/abtest-lights.sh lights on $G && bash tools/rtxprofile.sh current $G
```
At night, somewhere dense (Columbus pier, 21:00): settle 5 s, then **spin the camera twice without moving CJ**, then walk.
Look: the lit set may not change during the spin; a lamp you walk away from dims and stays dim, a signal you turn your back
on keeps its brightness. Report: `grep -E 'streetlights:|trafficlights\[' $G/scripts/rhyremix.log | tail -20` — `in_drop=`
**0 in every line**, `active` constant. Rollback: `bash tools/rtxprofile.sh restore $G`.
## 3. signal heads: the bulb AND the glow (s30 + n02 + n02b)
```bash
grep -E 'signalhook|trafficlights\[|config: traffic' $G/scripts/rhyremix.log | tail -6
```
Look: at each lit head **both** — a bright bulb on the lens AND a soft red/amber/green glow on pole, wall and road. Report:
the log wants an `ok bulb` AND an `ok glow` per lit signal; stops 06/07 green pixels above luma 200 must appear (none on
2026-11-12). Rollback: live ini `[trafficlights] glow_flux=0` = bulb only, `corona_radius=0` = game flare, `source=list`.
## 4. water — the research variant is the DEFAULT now (s02, s32, n07, n13)
```bash
bash tools/water.sh status $G             # expect: remix-ref, keys ok
bash tools/water.sh realistic $G
bash tools/water.sh calm $G
bash tools/water.sh default $G --force    # back to the default (the A/B was explicit)
```
Look: the sea at Santa Maria (stops 10/11) - no speckle, the horizon a line, not noise; **n13 installs `remix-ref` on every
start** (your pick: `.rhys-water/state`). Report: 11-water mean up from 6.8, black down from 29.2 %. Rollback:
`bash tools/water.sh pack $G` + `cp $G/rtx.conf.bak-before-n13 $G/rtx.conf`.
## 5. the 60 fps profile (s22) + the DLSS 5 probe (n19) + the frame tail (n08)
```bash
bash tools/rtxprofile.sh perf $G
bash tools/rtxprofile.sh dlss5 $G && grep -i DLSS-NR $G/rtx-remix/logs/remix-dxvk.log
python3 tools/perf-report.py $G/logs/perf/NvRemixBridge_*.csv --log $G/scripts/rhyremix.log --series
```
Look: MangoHud - ~60 fps at 1080p, nothing under ~45 in Pershing or on the hill. Report: every stop's mean within ~10 % of
step 1; that grep still prints `0xbad00010` (DLSS 5 unavailable, n19); beat **spikes/min after 300 s** — 90.6 (00:48) vs
**187.8** (10:22); keep `logs/perf/gpu-*.csv`. Rollback: `bash tools/rtxprofile.sh restore $G`.
## 6. vegetation: the pack, its cards and its typo (s03, s16, s32, n21)
```bash
bash tools/vegetation.sh check $G
bash tools/vegetation.sh on $G --step 6
python3 tools/vegetation_edge_measure.py $G/screenshots/shots/*.png --name vegetation
```
Look: stop 14-vegetation-vinewood-noon, then two minutes on the hillside: cards read as leaves, not scribbled lines; **no
transparent or missing trees**; no freeze in 60 s. Report: `check` prints the shim pair `lod_veg_firs.txd=lod_veg_fist.txd`;
stop 14 `mid_alpha` under 17.63 %, ramp under 2.59 px, mean/sat up from 168.1 / 0.262. Rollback: `bash tools/vegetation.sh off $G`.
## 7. character packs (s17)
```bash
bash tools/characters.sh on $G
```
Look: stop 13-ped-crowd-pershing-noon — faces and hands at 2 m. Report: stop 13 sat up (step 1: 0.264), mean within ~10 %
of 134.2. Rollback: `bash tools/characters.sh off $G` (`on` refuses until the packs are staged).
## 8. HD PBR on the 40 surfaces you look at (n20 → n20b)
```bash
bash tools/textures.sh status $G
bash tools/textures.sh on $G --generate      # 35 sets / 140 DDS / 92 MB, built once into build/textures
```
Look: Pershing Square at noon (stop 13) and the Grove sidewalk (stop 0) — the stucco of `tanstucco1_LA`, the marble of
`badmarb1_LAn` and the kerbs must carry relief and a dry matte response, not step 1's painted flatness, and the crossing
lines must still be there ([TEXTURES.md](TEXTURES.md)). Report: stops 00/13 mean/sat vs step 1, plus VRAM (`gpu-*.csv`) —
+91 MB. Rollback: `bash tools/textures.sh off $G` restores `mod.usda` byte-exact.
## 9. every lamp the game declares, and its bulb (n11)
```bash
python3 tools/lamp_list.py --game $G --out build/rhyremix_lights.txt --inventory build/rhyremix_2dfx.txt
python3 tools/lamp-coverage.py --list build/rhyremix_lights.txt --inventory build/rhyremix_2dfx.txt --region LA --min 95
bash scripts/test-session.sh --tour
```
Look: the Columbus pier / boardwalk at night (378,-1964 and 836,-1855) — the double-globe posts, coast posts and the
freeway sign lamps lit, each in **its own** colour (sodium on the lampposts, cool white on the globe posts), and each head
carries a bright point at the bulb, not just a pool. Report: the `PASS coverage` line and the log's
`N active (M with a lit bulb)`. Rollback: ini `[streetlights] head_glow=0`; `lamp_list.py --format v1` = the old table list.
