# Why `V Vegetation Mix` freezes the RTX copy (it runs in the non-RTX build)

Investigated 2026-11-11, evidence only — the game was not run. Measurements are reproducible with
`tools/txd_sizes.py` (new in this task) and the file lists in `.strata/notes-s03-vegetation-hang.md`.

Paths: RTX copy `G=/media/rhy/data/Gamez/GTASA-RTX`; the install that runs the pack
`S=/media/rhy/data/SteamLibrary/steamapps/common/Grand Theft Auto San Andreas`;
the pack as removed from the RTX copy `R=/media/rhy/data/Gamez/GTAmods/removed-from-game/rtx-copy/V Vegetation Mix`.

## What the freeze actually was

`scripts/rhyremix.log` is the only surviving artifact of the freeze run (it is appended, everything else is
rewritten per launch). Its run markers give: runs ended 08:11:41, 08:25:31, 08:42:01, 08:55:29,
**09:03:08.495**, 09:19:06. The freeze run started 09:02:48 and its last lines are

```
[09:03:07.841] streetlights: 320 active, 320 drawn this frame ... new=320 | remix: live=321
[09:03:07.841] trafficlights: 39 active, 39 drawn this frame ... | remix: live=360
[09:03:08.495] remix: live=360 create_fail=0 draw_fail=0 budget_refused=0
[09:03:08.495] sun: CreateLight ok dir=-0.8876,-0.1810,-0.4235 ... (hour=8:00)
```

The plugin logs a `remix: live=` heartbeat every second; it stops dead, so the game's `Present` loop stopped
at 09:03:08.5. The game clock read 0:00 at 09:02:56 and hour=8 at 09:03:07, so **the save loaded about one
second before the hang** — the hang is in the first frames of streaming around the player
(Grove Street, 2489.9,-1656.0), which is exactly where vegetation streams in.

Caveat worth remembering: `modloader.log`, `remix-dxvk.log`, `moonloader.log` and `ImprovedStreaming.log`
all carry **09:05 timestamps**, i.e. the run *after* the pack was deleted (the pack dir was removed from
`G/modloader/` at 09:04). The surviving `modloader.log` has no `V Vegetation Mix` lines at all. See
"Capturing the next freeze" at the end.

## The pack, measured

| | value |
|---|---|
| files in `gta3.img/` | 335 (286 `.dff`, 48 `.txd`, 1 `.col`) + `models/grass/plant1.txd` + 2 IDE files, 192 MB |
| of those, replacing an entry in `G/models/gta3.img` (16316 entries) | **317**; 18 are new `.txd` (`bsor_*` ×15, `insanity_des`, `insanity_pines`, `lod_veg_fist`) |
| replaced `.dff` bytes | 3.06 MB vanilla → **35.68 MB** (11.7×). Worst: `sm_firtbshg` 0.02→1.99, `sm_fir_tallgroup` 0.02→1.96, `vbg_fir_copse` 0.03→1.94 MB |
| replaced `.txd` bytes | 4.72 MB vanilla → **61.20 MB**; the 18 new `.txd` add **102.1 MB** |
| textures (txd header parse) | **49 `.txd`, 325 textures, 163.5 MB on disk, 151.0 MB in RAM** with mip chains |
| heaviest dictionary | `insanity_pines.txd`: 72.0 MB on disk / **60.8 MB RAM, 59 textures**, eight of them **1024×4096 DXT1** (2.80 MB each). Then `gta_tree_boak` 19.4 MB, `bsor_tree` 6.6, `gta_tree_palm` 5.5 MB |
| footprint of the two `KeepLoaded=1` vegetation ranges (IDs 615–792 + 800–906, unique `.dff` + unique `.txd`) | **190.5 MB with the pack vs 7.4 MB vanilla** |

## Cause 1 (most likely): the pack's own `procobj.ide` / `vegepart.ide` are live in the RTX copy

Evidence
- `diff -rq` between the pack in the working install `S/modloader/V Vegetation Mix` and the frozen copy
  `R` returns exactly two differences:
  `Only in R/data/maps/generic: procobj.ide` and `Only in R/data/maps/generic: vegepart.ide`.
  They were deleted in the non-RTX install (task g04, following the pack's readme) and never re-added.
- Against the RTX copy's vanilla IDEs: `procobj.ide` 83/119 lines changed, `vegepart.ide` 86/183 changed,
  **no id changes**, but 10 + 34 texture-dictionary re-pointings — that is what pulls the 18 new
  dictionaries (102 MB, incl. the 72 MB `insanity_pines.txd`) into the game at all.
- Those IDEs contain a **reference to a dictionary that does not exist**: ids 784–788
  (`lod_redwoodgrp`, `lod_vbg_fir_co`, `lod_largefurs06/07/05`) point at `lod_veg_firs`, while the pack ships
  `lod_veg_fist.txd` (extra `t`) and Proper Fixes ships `LODvegetation.txd`. Nothing anywhere provides
  `lod_veg_firs.txd`. The pack's readme names the symptom: delete the two IDEs "to avoid errors of models
  without textures", and it states the pack **requires Essentials Pack + Proper Fixes** — the RTX copy has
  neither.
- The flags column also gains bit `0x200000` on ~85 entries, but vanilla already sets it on 160/171
  `vegepart` entries, so the flag itself is normal for vegetation and not the suspect.

Exact change to try (matches the known-working install, one move, fully reversible):

```bash
mkdir -p /media/rhy/data/Gamez/GTAmods/removed-from-game/rtx-copy/V\ Vegetation\ Mix/data/maps/generic
mv "/media/rhy/data/Gamez/GTASA-RTX/modloader/V Vegetation Mix/data/maps/generic/procobj.ide" \
   "/media/rhy/data/Gamez/GTASA-RTX/modloader/V Vegetation Mix/data/maps/generic/vegepart.ide" \
   "/media/rhy/data/Gamez/GTAmods/removed-from-game/rtx-copy/V Vegetation Mix/data/maps/generic/"
```

(If the pack is re-installed into the RTX copy, do this before the first launch.)

## Cause 2: streaming memory and the 2 GB address space of a non-LAA 32-bit process

Evidence
- RTX copy: `modloader/MixSets/MixSets.ini` `StreamMemory = 1900` (confirmed applied: `MixSets.log`
  prints `System: StreamMemory = 1900`), and `modloader/Improved Streaming/ImprovedStreaming.ini`
  `StreamMemoryForced = 1024` — that key never lowers a higher value, so the effective cap is **1900 MB**.
  `RemoveUnusedWhenPercent = 80` means unloading only starts above 1520 MB.
- Same ini preloads and **pins** vegetation: `[Range4] Start=615 End=792 KeepLoaded=1` and
  `[Range5] Start=800 End=906 KeepLoaded=1`, both `Enabled=1`. Measured footprint of those ranges:
  **190.5 MB with the pack vs 7.4 MB vanilla**, never released.
- `gta-sa.exe` in the RTX copy has `Characteristics = 0x010f`: **no `IMAGE_FILE_LARGE_ADDRESS_AWARE`**, so
  Wine gives it ~2 GB of address space, and the same process carries the Remix bridge client
  (`d3d9.dll` in the game folder). A 1900 MB streaming cap in that process is a loaded gun; the pack is what
  fires it (151 MB of textures plus 35.7 MB of geometry, one 72 MB single streamed file).
  (**Fixed 2026-11-11 — see "Applied" below.**)
- The non-RTX install is the control: it has no MixSets, and its streaming mod (Proper Streaming) is listed
  in `[Profiles.Default.IgnoreMods]` (`modloader.log`: `Ignoring mod at "modloader\proper streaming\"`), so
  its cap comes from Open Limit Adjuster `MemoryAvailable = auto` → **384 MB**. Same pack, same exe, no LAA,
  no freeze. (The task text called it "Improved Streaming"; on disk it is Proper Streaming, and it is off.)
- Precedent in this repo's own history: `GTAmods/.strata/PLAN.md` — an earlier Proton deadlock had the main
  thread spinning in `CdStreamGetStatus` (0x4063E0) with the `std.stream` thread idle, i.e. a streaming wait.

Exact change to try (both MixSets.ini copies in the RTX copy are identical; `Remix_fix/` overrides the mods'
own inis, so change both):

```ini
; modloader/MixSets/MixSets.ini        and  modloader/Remix_fix/MixSets.ini
StreamMemory = 1024          ; was 1900

; modloader/Improved Streaming/ImprovedStreaming.ini   (and Remix_fix/ImprovedStreaming.ini)
StreamMemoryForced = 1024
RemoveUnusedWhenPercent = 70

[Range4]
Enabled = 0                  ; vegetation 615-792: was KeepLoaded=1 (≈190 MB pinned with this pack)
[Range5]
Enabled = 0                  ; procedural objects 800-906
```

### Applied: Large Address Aware is now set (2026-11-11)

`G/gta-sa.exe` is `Characteristics = 0x012f` — `IMAGE_FILE_LARGE_ADDRESS_AWARE` set with
`tools/laa.py` (the pre-LAA exe is kept once as `G/gta-sa.exe.noLAA-backup`, still `0x010f`). Check:

```bash
python3 /home/rhy/rhYgtaSA/tools/laa.py /media/rhy/data/Gamez/GTASA-RTX/gta-sa.exe --check   # exit 0 = set
```

End users get the exe from the downgrader, which is not LAA, so `apply.sh` now sets the flag itself
(one-time backup, refuses anything that is not a 32-bit PE) and `deploy.sh` **warns** on every run when it is
missing — it never rewrites the exe, because deploy runs from `launch-rtx.sh` at game start.

Two things this does **not** change:
- It only doubles the address space (2 GB → 4 GB under Wine); the 1900 MB `StreamMemory` cap and the pinned
  vegetation ranges are still there, so the ini edits below remain the next thing to try.
- `docs/FINDINGS.md` (2026-11-09 17:25) once made the exe a *no*-LAA copy because LAA was suspected of a
  MoonLoader 0.27 opcode-output crash. That suspicion is dead: the same file records the real cause an hour
  later (invalid `PLAYER_PED` handle, fixed by the `getPlayerChar` patch + `sa_ped_safe.lua`), and the
  address-space cost of not being LAA is what this section is about.

## Cause 3: what the extra foliage costs the RTX Remix runtime (opacity micromaps, acceleration structures)

Evidence
- The pack multiplies the vegetation geometry the RT runtime must accelerate: 286 `.dff` go from 3.06 MB to
  35.68 MB (11.7×), with single meshes at ~2 MB (`sm_firtbshg`, `sm_fir_tallgroup`, `vbg_fir_copse`), and it
  adds 325 alpha-tested foliage textures (151 MB).
- `G/rtx.conf` carries the scar of exactly this failure mode, lines 61–67:
  `# 2026-11-11: GPU device loss came 20 s after "[RTX Opacity Micromap] Out of memory budget".`
  `rtx.opacityMicromap.enable = True`, `rtx.opacityMicromap.cache.minFreeVidmemMBToNotAllocate = 1536`,
  `rtx.opacityMicromap.cache.maxBudgetSizeMB = 1024` (revert file `rtx.conf.bak-before-omm-off`). Opacity
  micromaps exist to speed up **alpha-tested foliage** — the pack is nothing else.
- The good run's `remix-dxvk.log` already complains about that path: `[RTX Opacity Micromap] Instance
  requires more texel taps to resolve opacity than allowed`, `detected unsupported quad index layout for
  billboard creation`, `Trying to bind a texture to a mesh without UVs`. And `rtx.enableCulling = False`
  means every instance is submitted to the RT pipeline.
- The freeze signature (everything stops, no error, black screen) matches a GPU-side stall/device loss as
  well as a CPU-side streaming wait; the surviving logs cannot separate them, because the freeze run's
  `remix-dxvk.log` was overwritten. That is what `tools/winedbg-hang.sh` is for.

Exact change to try (in `G/rtx.conf`; keep the comment block, it is the reason these lines exist):

```ini
rtx.opacityMicromap.cache.maxBudgetSizeMB = 256                    ; was 1024
rtx.opacityMicromap.cache.minFreeVidmemMBToNotAllocate = 3072      ; was 1536
; if it still hangs, take the tested-off state (it costs fps):
rtx.opacityMicromap.enable = False
```

and, to bisect the pack itself, drop the two heaviest dictionaries out of the RTX install
(this leaves those trees untextured — it is a test, not a fix):

```bash
mkdir -p /media/rhy/data/Gamez/GTAmods/removed-from-game/rtx-copy/V\ Vegetation\ Mix/gta3.img
mv "/media/rhy/data/Gamez/GTASA-RTX/modloader/V Vegetation Mix/gta3.img/insanity_pines.txd" \
   "/media/rhy/data/Gamez/GTASA-RTX/modloader/V Vegetation Mix/gta3.img/gta_tree_boak.txd" \
   "/media/rhy/data/Gamez/GTAmods/removed-from-game/rtx-copy/V Vegetation Mix/gta3.img/"
```

## Test order

**One command does this whole ladder** (s16): `tools/vegetation.sh on <game> --step N` installs
the staged pack (`/media/rhy/data/Gamez/GTAmods/removed-from-game/rtx-copy/V Vegetation Mix`)
into `<game>/modloader/` with causes 1..N applied — step 1 leaves the two IDEs out, step 2 is
the streaming caps, step 3 the opacity-micromap budget, step 4 the pack bisect — `status` prints
which of them are live, and `off` puts every byte back (state + one pristine backup per edited
file under `<game>/modloader/.rhys-vegetation/`; test: `scripts/vegetation-selftest.sh`). The
commands below are what each step really changes, kept because they are what you check by eye.

0. **LAA is already applied** — `gta-sa.exe` is `0x012f` (`tools/laa.py`, backup `gta-sa.exe.noLAA-backup`),
   `apply.sh` sets it and `deploy.sh` warns if it is missing. Start every retest from that state: if the exe
   came from a fresh downgrader run, re-run `./apply.sh` (or `tools/laa.py <exe> --set`) before reading a
   freeze as "cause 1/2/3".
1. **Cause 1 first** — move the two IDE files (one command, makes the RTX install byte-identical to the
   working non-RTX install in that respect). Launch, load the same save, wait 60 s at Grove Street.
2. If it still freezes: **cause 2** — `StreamMemory = 1024` in both MixSets inis and `Enabled = 0` for
   ImprovedStreaming `[Range4]`/`[Range5]`. Launch again.
3. If it still freezes: **capture before restarting** — run `tools/winedbg-hang.sh` during the freeze and
   copy the four logs (see below). The backtrace decides between the two remaining families:
   a frame inside `CdStream*` / `CStreaming::` / `cpool` allocation → cause 2 (address space);
   a frame inside the bridge / `nvremix` / Vulkan wait, or `remix-dxvk.log` showing
   `Out of memory budget` / `device lost` → cause 3.
4. Only then **cause 3** ini edits (`rtx.opacityMicromap.*`), and finally the pack bisect (drop
   `insanity_pines.txd` + `gta_tree_boak.txd`, then re-add half the `.dff` set) to find the single file.
5. Whatever works, re-test the pack's own requirement: the readme wants **Proper Fixes** installed. It is
   currently in `S`'s IgnoreMods and absent from the RTX copy, and the RTX copy's `modloader/` has no
   Proper Fixes — so "install the pack as its author intends" is a legitimate alternative to dropping files.

## Ruled out (with evidence)

- **Our plugin.** The good run at 09:05:40.651 did the identical burst (`streetlights: 320 active`,
  `trafficlights: 39 active`, `remix: live=360`, then `sun: CreateLight ok`) and survived.
- **The "GPU already full" failure mode** from `launcher/launch-rtx.sh`: the freeze run reached
  `remixapi_InitializeLibrary v0.6.5 -> 0` at 09:02:56, which that failure never gets to.
- **Filename collisions with other RTX mods.** None of the pack's 335 names appears in any other
  `G/modloader/` folder (FramerateVigilante, GInputSA, Improved Streaming, ImVehFt, MixSets, OLA, Ped Spec,
  Proper Ragdoll, Remix_fix, SARemix).
- **Name clashes with the GTASA-Remix asset pack.** It keys replacements by content hash
  (`over "mesh_BBA98D7DC51F8566"` in `rtx-remix/mods/gameReadyAssets/meshes/props.usda`); grepping
  `mod.usda`, `SARemix*.usda` and `meshes/*.usda` for `gta_tree_palm`, `veg_palmkb`, `bsor_`, `insanity`
  returns nothing. Consequence: any Remix replacement keyed to the old vegetation geometry simply stops
  matching once the pack replaces the `.dff` — a visual regression, not a conflict.
- **The `Removing imported model file at index 384..388` lines** at the end of the surviving
  `modloader.log`: those ids are `clothes01` in `data/default.ide` (20000–20003 are SARemix `remix.ide`),
  and they appear in the healthy run too. Normal streaming churn.

## Capturing the next freeze

ModLoader, DXVK and MoonLoader rewrite their logs at launch, so a restart destroys the freeze evidence —
that is why only `rhyremix.log` survived this time. After a freeze, **before** starting the game again:

```bash
G=/media/rhy/data/Gamez/GTASA-RTX; D=/home/rhy/rhYgtaSA/build/freeze-$(date +%Y%m%d-%H%M%S); mkdir -p "$D"
cp -a "$G/modloader/modloader.log" "$G/modloader/Improved Streaming/ImprovedStreaming.log" \
      "$G/modloader/MixSets/MixSets.log" "$G/scripts/rhyremix.log" \
      "$G/rtx-remix/logs/remix-dxvk.log" "$G/rtx-remix/logs/bridge32.log" \
      "$G/rtx-remix/logs/bridge64.log" "$G/moonloader/moonloader.log" "$D"/
bash /home/rhy/rhYgtaSA/tools/winedbg-hang.sh      # while it is still hung: backtraces + address-space snapshot
```
