# NEXT-TEST — the next in-game session, in this order

Six steps, one variable each, from `~/rhYgtaSA`; `$G` is the game folder (set in step 0). Each step: run -> look ->
read the report -> know the rollback. Reference numbers are the 2026-11-11 09:06 tour. After any step
`bash scripts/diagnose.sh` re-reads the logs and changes nothing. Send back `report.md` + `sheet-*.png` from
`$G/screenshots/report-<start>/`, `$G/scripts/rhyremix.log`, and if it died `rtx-remix/logs/crash-upload.log` + `bridge64.log`.

## 0. preflight — 30 s, changes nothing
```bash
G=/media/rhy/data/Gamez/GTASA-RTX
python3 tools/laa.py $G/gta-sa.exe --check
bash tools/rtxprofile.sh status
cp plugin/lua/SARemix_Tour.lua $G/moonloader/SARemix_Tour.lua
```
Look: `Large Address Aware SET` (step 5 depends on it) and `profile : unknown` — expected, the live rtx.conf is the
hand-tuned file (shipped + the three `rtx.opacityMicromap.*` keys).
Rollback: none for the `cp` — the repo copy is the same 13 stops plus the three s15 stops (mirror, ped, vegetation) and
**nothing in `deploy.sh` installs it**, so without this line steps 5 and 6 produce no numbers at all.

## 1. baseline tour — nothing new
```bash
bash scripts/test-session.sh --tour
python3 tools/tour-report.py $G --since 21:30    # 21:30 = the minute you started; without --since the old tour is re-reported
```
Look: the launcher prints `plugin : <today's build>` and `traffic: ... source=game`; then `$G/scripts/rhyremix.log` must
contain `remix: remixapi_InitializeLibrary v0.6.5 -> 0` — without it the card was not free and nothing on screen means
anything (docs/TESTING.md).
Report: `paired | 16 of 16 manifest entries`, and the 2026-11-11 numbers again: 00-baseline mean 128.7,
03-sodium-lamp 26.2 mean / 6.7 % black, 11-water-2100 6.1 / 40.8 % black. Stops 12/13/14 have no history: their numbers
*become* the baseline for steps 2-6.
Rollback: nothing changed — this is the reference run.

## 2. signal heads (s01, hardened by s19)
```bash
grep -E 'signalhook|trafficlights\[source=game\]' $G/scripts/rhyremix.log | tail -3
```
Look: each head lit *at the head*, not at the top of the pole; on a mast-arm signal the head over the road glows; red is
red, green blue-green, amber between. Log: `signalhook: patched 0x0049DD4C` and
`trafficlights[source=game]: N heads lit, N drawn ... hook: calls=N records=N dropped=0 bad=0 reentrant=0` — `reentrant`
stays 0; `calls>0` with `records=0` is our bug, not a quiet street; a rising `bad` is a symptom, not a reason to stop.
Report: stops 06/07 red/amber/green near 2026-11-11 — 2.241/23.045/5.711 % and 0.093/2.429/21.158 %. A blank signals
column means the log line never landed, not that the heads were dark.
Rollback: `sed -i '/^\[trafficlights\]/a source=list' $G/scripts/rhyremix.ini` (live, no restart; undo by deleting that line).

## 3. water — realistic vs calm (s02)
```bash
bash tools/water.sh realistic $G
bash tools/water.sh calm $G
```
Look: the sea at Santa Maria (stops 10/11) — no binary black/white speckle, foam reads as foam, the horizon is a line
and not a wall of noise. `realistic` is already installed (sha-checked), so this step is the A/B.
Report: 11-water-santa-maria-2100 mean up from 6.1 and black down from 40.8 % — that is
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
Look: stop 14-vegetation-vinewood-noon, then two minutes on the hillside — no freeze, no hitch, and `rhyremix.log` keeps
its ~10 s cadence. `on` also rewrites the OMM keys to 256/3072: expected, and `off` puts them back.
Report: stop 14 mean and sat above step 1's stop 14 (more leaves in frame), and no `stop 14 failed:` line in the manifest.
Rollback: `bash tools/vegetation.sh off $G` (hashes back byte-exact).

## 6. character packs (s17)
```bash
bash tools/characters.sh on $G
```
Look: stop 13-ped-crowd-pershing-noon — faces and hands at 2 m — and CJ: the pack's PBR is keyed to his four texture
hashes, so a retex can leave him matte; write the number down, do not fix it here (that is s25).
Report: stop 13 sat up (clothing), mean within ~10 % of step 1; a big mean drop on CJ is the s25 symptom.
Rollback: `bash tools/characters.sh off $G`.
Note: `on` refuses today — nothing is staged in `/media/rhy/data/Gamez/GTAmods/x/characters/stage` (measured 2026-11-12)
and it prints the six packs and their author pages rather than install half a set. Stage them, then re-run this step.
