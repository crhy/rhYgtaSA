# NEXT-TEST - the next in-game session, in this order

Ten steps, one variable each; `$G` = the game folder (step 0). Each: run -> look -> report -> rollback. Send back
`report.md`, `rhyremix.log` (+ `.prev`), `sheet-*.png`; long version [TESTING.md](TESTING.md).
## 0. preflight - 30 s, changes nothing
```bash
G=/media/rhy/data/Gamez/GTASA-RTX
python3 tools/laa.py $G/gta-sa.exe --check
bash tools/rtxprofile.sh status
diff -q plugin/lua/SARemix_Tour.lua $G/moonloader/SARemix_Tour.lua
```
Look: `Large Address Aware SET` (step 7 needs it); `profile : unknown` = hand-tuned rtx.conf; `diff` prints nothing. Rollback: none.
## 1. baseline tour - nothing new
```bash
bash scripts/test-session.sh --tour
python3 tools/tour-report.py $G --since 21:30    # 21:30 = the minute you started
grep -m1 'config: tonemap exposure' $G/scripts/rhyremix.log
```
Look: launcher prints `plugin : <today's build>`, `traffic: ... source=game`, `water : remix-ref`, the log has
`remix: remixapi_InitializeLibrary v0.6.5 -> 0` (no line = the card was not free). Report: `paired | 16 of 16`; to
beat (STATUS): 00 128.6, 03 26.3 / 6.6 % black, 11 6.8 / 29.2 %, 13 134.2. Rollback: none.
**s28 fade** - `grep -m2 'streetlights:' $G/scripts/rhyremix.log`: `steps=`, `created=`, `retired=`, `deferred=` climb
gradually, never the whole active count in one line; `in_drop=` **0 always**; `player_gone_frames=` may be non-zero.
**s29 clock** - `grep -m2 'sky: ' $G/scripts/rhyremix.log`: `tod=` has decimals, ~5.0 game-min per 10 s line; `ms/min=`
1670-2030; `torn=` a few; `jitter=` climbing while `elev=` holds one direction is the fix working; `reanchor=` ~0;
`wraps=` 1/game day; `cal: accepted=0`; `cloud=0.00`.
**n23 lamps down** - `python3 tools/night-brightness.py $G --target 25`: 21:00 stops <= 25 % of their noon frame (57.3 % today).
**n24 night sources** - `grep -m1 'published: ' $G/scripts/rhyremix.log`: at night `sky=0.01xx (moon cap, asked 0.0700)`, not 0.0700
## 2. lamps that ignore the camera - finding A, the priority one (n04)
```bash
bash scripts/abtest-lights.sh lights on $G && bash tools/rtxprofile.sh current $G
```
At night, somewhere dense (Columbus pier, 21:00): settle 5 s, then **spin the camera twice without moving CJ**.
Look: the lit set may not change during the spin; a lamp you walk away from dims and stays dim; a signal you turn your
back on keeps its brightness. Report: step 1's s28 line, over the spin - `in_drop=` **0 in every line**, `active`
constant. Rollback: `bash tools/rtxprofile.sh restore $G`.
## 3. signal heads: the bulb AND the glow (s30 + n02 + n02b)
```bash
grep -E 'signalhook|trafficlights\[|config: traffic' $G/scripts/rhyremix.log | tail -6
```
Look: at each lit head **both** - a bright bulb on the lens AND a soft glow on pole, wall and road. Report: an `ok bulb`
AND an `ok glow` per lit signal; stops 06/07 green pixels above luma 200 must appear (none 2026-11-12). Rollback: live
ini `[trafficlights] glow_flux=0` = bulb only, `corona_radius=0` = game flare, `source=list`.
## 4. water - the research variant is the DEFAULT now (s02, s32, n07, n13)
```bash
bash tools/water.sh realistic $G
bash tools/water.sh calm $G
bash tools/water.sh default $G --force    # back to the default (the A/B was explicit)
```
Look: the sea at Santa Maria (stops 10/11) - no speckle, the horizon a line; **n13 installs `remix-ref` on every start**.
Report: 11-water mean up from 6.8, black down from 29.2 %. Rollback: `bash tools/water.sh pack $G` +
`cp $G/rtx.conf.bak-before-n13 $G/rtx.conf`.
## 5. the 60 fps profile (s22) and the frame tail (n08)
```bash
bash tools/rtxprofile.sh perf $G
python3 tools/perf-report.py $G/logs/perf/NvRemixBridge_*.csv --log $G/scripts/rhyremix.log --series
```
Look: MangoHud - ~60 fps at 1080p, nothing under ~45 in Pershing or on the hill. Report: every stop's mean within ~10 %
of step 1; beat **spikes/min after 300 s** - 90.6 (00:48) vs **187.8** (10:22). Rollback: `bash tools/rtxprofile.sh restore $G`.
## 6. vegetation: the pack, its cards and its typo (s03, s16, s32, n21)
```bash
bash tools/vegetation.sh check $G && bash tools/vegetation.sh on $G --step 6
```
Look: stop 14-vegetation-vinewood-noon, then 2 min on the hillside: cards read as leaves, not lines;
**no transparent or missing trees**; no freeze in 60 s. Report: `check` prints the shim pair `lod_veg_firs.txd=lod_veg_fist.txd`,
stop 14 mean/sat up from 168.1 / 0.262. Rollback: `bash tools/vegetation.sh off $G`.
## 7. character packs (s17) - one pack is staged, and it is not the set
```bash
bash tools/characters.sh on $G && bash tools/characters.sh status $G
```
Look: stop 13-ped-crowd-pershing-noon - faces and hands at 2 m. Report: `status` says `criminal-peds-vary STAGED yes`
(`Criminal Peds Vary` + `_criminal-peds-vary-extras`), the other five `no`, so `on` refuses - 5 of 6 not staged;
`--partial` installs it; it still needs `PedFuncs` + `ModelVariations` and the two hand `cp`s (CHARACTERS.md 12).
Stop 13 sat up (0.264), mean within ~10 % of 134.2. Rollback: `bash tools/characters.sh off $G`.
## 8. HD PBR on the 40 surfaces you look at (n20 -> n20b)
```bash
bash tools/textures.sh on $G --generate      # 35 sets / 140 DDS / 92 MB, built once into build/textures
```
Look: Pershing Square at noon (stop 13) and the Grove sidewalk (stop 0) - `tanstucco1_LA` stucco, `badmarb1_LAn` marble
and the kerbs must carry relief, not step 1's flatness. Report: stops 00/13 mean/sat vs step 1,
VRAM +91 MB. Rollback: `bash tools/textures.sh off $G` restores `mod.usda` byte-exact.
## 9. every lamp the game declares, and its bulb (n11)
```bash
bash scripts/test-session.sh --tour
```
Look: the Columbus pier / boardwalk at night - the double-globe posts, coast posts and freeway sign lamps lit, each in **its own**
colour (sodium posts, cool white globes), each head a bright point at the bulb. Report: the log's
`N active (M with a lit bulb)`; LA coverage: the gate (`PASS coverage`). Rollback: `[streetlights] head_glow=0` ini.
