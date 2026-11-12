# Characters: upgrading CJ and the pedestrians in the RTX copy

Plan only — **nothing was downloaded and nothing was written to the game folder.**
Measured facts are marked *(measured)*, everything else is what the mod page says *(reported)*.
Research date: 2026-11-11. Notes: `.strata/notes-s04-characters.md`.
Since s17 the install/rollback in §8/§10 is a script: `tools/characters.sh on|off|status` (§8.1).

---

## 1. Where the peds are today (measured in `GTASA-RTX`)

| layer | what touches peds | effect under Remix |
|---|---|---|
| `models/player.img` | stock 1.0 US: 542 entries, 66,738,176 B, hardlink count 2 (never modified). CJ = `player_face.txd` 18 KB, `player_torso.txd` 264 KB, `player_legs.txd` 788 KB, `player_feet.txd` 395 KB + ~250 clothing `.dff`/`.txd` pairs *(measured)* | the albedo Remix sees for CJ |
| `models/gta3.img` | stock 1.0 US: VER2, 16,316 entries (12,964 dff / 2,759 txd), 940,064,768 B. Biggest TXD inside is 2.5 MB, so no HD ped retex is baked in; the 2.4 MB growth vs `GTAmods/backup-steam-3.0` is the 1.0 downgrade diff (`SA DOWNGRADE/gta3.img.diff`) *(measured)* | the albedo Remix sees for every NPC |
| `modloader/*` | FramerateVigilante, GInputSA, Improved Streaming, ImVehFt, MixSets, OLA, **Ped Spec**, Proper Ragdoll, Remix_fix, SARemix. No ped `.dff`/`.txd` anywhere; `modloader.ini` ignores `VehFuncs` *(measured)* | only **Ped Spec** (`ped_spec.asi`, `fSpecColor 0.15`, `iSpecPower 40`) is ped-related, and it is a D3D material tweak → see §2 |
| Remix pack `rtx-remix/mods/gameReadyAssets` | `Meshes.usda` replaces Vehicles, Creek, props, Rail, Buildings, Airport, Weapons — **no ped meshes at all**. `Materials.usda` → `texture/characters/Character_materials.usda` + `texture/pedestrians/Pedestrian_materials.usda` *(measured)* | hand-authored PBR for a handful of peds only |

The pack's ped material coverage, read out of the `.usda` files *(measured)*:

| usda | materials overridden | albedo size |
|---|---|---|
| `characters/CJ/CJ.usda` | `over "mat_0AB982F62FEF3381"`, `over "mat_1AB00AD812498A17"`, `over "mat_C23DD21AE7ED34B1"` — **three** Looks, not four: CJ's face / torso / legs (or feet), i.e. three of the four `player_*.txd`. The fourth hash `8645663DE7AFBB81` exists only as the *shared normal/rough source* (`8645663DE7AFBB81_normal.dds` + `_rough.dds`, also used by `mat_0AB982F62FEF3381`); its `_albedo.dds` is on disk but no Look references it *(re-measured 2026-11-12, §6)* | 1024² (one of them 512×1024) |
| `characters/Ryder/Ryder.usda` | 1 body material + glasses | 4096² / 1024² |
| `characters/Kendl/Kendl.usda` | 1 material | 4096² |
| `characters/Big_Smoke/Big_Smoke.usda` | **empty stub** (`#usda 1.0` only) | — |
| `characters/Character_materials.usda` | `mat_DA50E6D8BA27A905` | 512² |
| `pedestrians/Pedestrian_materials.usda` | `mat_E6E920AB99F5FC23`, `mat_A3537FCA9CCB9E69` — **two** generic pedestrian materials | 1024² |

So: CJ, Ryder, Kendl and 2 pedestrian materials have real PBR; the other ~130 NPC peds, all gangs, cops,
civilians get Remix's auto material on top of 2004-era 128–256 px textures. That is why peds look like the
worst part of the game right now, and why the fix is game-side (better albedo/mesh), not pack-side.

Hard constraint *(measured)*: `GTASA-RTX/gta-sa.exe` is PE32 i386 with `Characteristics = 0x10f` →
**LARGE_ADDRESS_AWARE is not set** (deliberate, see `docs/FINDINGS.md` 2026-11-09 17:25). ~2 GB of address
space for game + Remix bridge + pack + textures. This is the same limit that made `V Vegetation Mix` hang
this copy (`docs/VEGETATION.md`), and it rules out the 800 MB-class ped packs by default.

---

## 2. How Remix sees a ped mod (the compatibility rule)

1. Remix captures what the game draws and matches it to the pack by **hash**: `mat_<HASH>` nodes under
   `RootNode/Looks`. The pack's authored albedo/normal/roughness only apply while that hash still matches.
2. Therefore **any game-side `.txd`/`.dff` replacement of a ped breaks the pack's override for that ped**
   and it falls back to Remix's default material (base colour from the captured texture, no authored
   normal/roughness). This is not a bug — it is the trade: you get a far better albedo, you lose the
   pack's hand-made PBR for that specific ped until the override is re-pointed.
3. Conversely, the pack has **no ped mesh replacements**, so a higher-poly ped mesh breaks nothing in the
   pack; and the geometry hash rule (default `positions,indices,geometrydescriptor`, `docs/FINDINGS.md`
   2026-11-09 22:35) only matters for the meshes the pack does replace.
4. Anything that works by changing D3D material state or shaders is a **no-op under Remix**: Proper
   Shaders, SkyGfx, ENB/ReShade, and the already-installed **Ped Spec** (`ped_spec.asi`). Ped Spec should
   be dropped from the RTX copy: it can only add noise (it changes the material the game submits) and it
   buys nothing here.
5. What actually buys image quality under Remix, in order: **albedo resolution + art quality**, then mesh
   silhouette, then (only via the pack) normal/roughness. Normal/specular maps shipped inside a game-side
   mod are ignored — Remix does not read them.

Practical consequence for the shortlist: prefer **texture-only, same-model, ModLoader-folder** packs
(nothing in the pack breaks that the pack actually uses, except CJ's materials — three Looks plus a
shared normal/rough pair, §6), and treat
"new mesh + new textures" packs as a bigger win only if we are ready to re-capture CJ in Remix afterwards.

---

## 3. Candidates researched (pages read with curl; ≥5 peds, ≥3 CJ)

### Pedestrians / NPCs

| # | mod | date / version | size | author | what it replaces | install requirements | redistribution |
|---|---|---|---|---|---|---|---|
| P1 | **Proper Characters Retex HD** — [mixmods](https://www.mixmods.com.br/2022/07/sa-proper-characters-retex-hd-peds-remaster/) | updated 2022-07-24 | 22 MB | Jéssica Natália (help: Junior_Djjr, Jão, OjTrue) | textures only (`.txd`) of **133 peds** incl. all girlfriends; faces re-mastered with AI upscale + manual redraw, original look preserved | ModLoader folder; no plugin; Improved Streaming recommended (already installed) | MixMods is the primary source; re-upload not allowed → link out |
| P2 | **Emergency Peds Vary** — [LibertyCity 246900](https://libertycity.net/files/gta-san-andreas/246900-emergency-peds-vary.html) | 2026-11-07 | 71 MB (71,431,946 B) | wizvich, Muks, FCVP71, SethShaddix, Area 69, Thiago555aug | **adds 50 new** police/paramedic/firefighter/sheriff peds, ~10 texture variants each; adds the unused beta sheriff `dsher`; originals untouched except firefighters | ModLoader folder; the author recommends pairing it with Original Peds Vary (which needs PedFuncs) | page's permissions block: re-upload **No**, edits/convert **only with permission** |
| P3 | **Criminal Peds Vary** — [LibertyCity 240761](https://libertycity.net/files/gta-san-andreas/240761-criminal-peds-vary.html) | 2026-08-09, updated 2026-11-03 | 196 MB (205,560,101 B) | wizvich | **adds 27 new** Families/Ballas + 17 new members of the other gangs, 15–20 texture variants each; new Angels of Death gang; recolours SF Rifa to match T-Bone | ModLoader folder; page lists no hard requirement, but the "Peds Vary" family is built on the PedFuncs remap system (its sibling [Authentic Peds Vary (For PedFuncs)](https://libertycity.net/files/gta-san-andreas/241335-authentic-peds-vary-for-pedfuncs.html) says so explicitly) → assume [PedFuncs](https://www.mixmods.com.br/2022/07/pedfuncs/) v0.5.1 | re-upload **No**, edits/convert **only with permission** |
| P4 | **Pedestrian Overhaul** — [LibertyCity 246336](https://libertycity.net/files/gta-san-andreas/246336-pedestrian-overhaul.html) | 2026-10-30 | 177 MB (177,533,125 B) | Komeiji Koishi | 90+ new pedestrians, each with custom textures; uses model swapping (author says the implementation is rough, some models missing) | drop `modloader` folder in game root; **requires Essentials Pack + Open Limit Adjuster + PedFuncs + Model Variations** | author says free to redistribute/modify (but the base models are not theirs) |
| P5 | **GTA Trilogy Peds Pack** — [mixmods](https://www.mixmods.com.br/2022/08/sa-gta-trilogy-peds-pack-skins/) | updated 2022-08-22 | 800 MB | Jéssica Natália (help: Junior_Djjr) | converts **all** SA:DE pedestrians 1:1 (same rigging, textures baked to fake normal maps, separate fingers/facial bones) | **requires PedFuncs ≥ 0.5**; author warns against pre-loading all peds (heavy textures) | re-upload **not allowed**, share the link only |
| P6 | **Original Peds Vary** — [mixmods](https://www.mixmods.com.br/2021/03/original-peds-vary-peds-originais-variando-pedfuncs/) | 2021-03-05 | 6 MB / **87 MB HD-upscale build** / 28 MB extended | pack: Thiago555aug; HD upscale: Jéssica Natália | replaces 154 peds with ≥3 texture variants each (extended version: all peds, 3,747 textures) | **requires PedFuncs** | MixMods primary source, no re-upload |
| P7 | **Extended and Enhanced Gangs V4.0** — [LibertyCity 230037](https://libertycity.net/files/gta-san-andreas/230037-extended-and-enhanced-gangs-v4-0.html) | 2025-10-18 | 14 MB | TheoTTG, Junior_Djjr, _F_, MrFinger, jahmshut, Kevin, Cleoude | improves existing gangs, fixes character models, more territory life | CLEO-based (page mentions CLEO/ModLoader/Open Limit) — CLEO is currently parked in the RTX copy | no permissions block on the page |
| P8 | **Restored Unused Peds** — [LibertyCity 179162](https://libertycity.net/files/gta-san-andreas/179162-restored-unused-peds-unoffical-update.html) / [mixmods](https://www.mixmods.com.br/2022/07/sa-restore-unused-peds/) | 2022-08-13 | 0.5 MB | Jéssica Natália, Junior_Djjr, falsenail | re-adds cut ped models | ModLoader folder, no plugin | MixMods |
| P9 | **RoSA Evolved v1.5** — [mixmods](https://www.mixmods.com.br/2023/07/rosa-project-evolved-remaster-texturas-hd/) | updated 2025-12-05, monthly cadence | large (map pack) | Jéssica Natália | whole-game HD texture remaster; **bonus: all pedestrians AI-upscaled** | OLA `MemoryAvailable=1024` recommended; already on disk at `GTAmods/removed-from-game/RoSA Project Evolved`, currently in `IgnoreMods` | base public, newest builds are Patreon early access; no re-upload |

### CJ / player

| # | mod | date / version | size | author | what it replaces | install requirements | redistribution |
|---|---|---|---|---|---|---|---|
| C1 | **Proper Player Retex v3.2** — [mixmods 2024/12](https://www.mixmods.com.br/2024/12/proper-player-retex/) | updated 2025-12-05 | **120 MB** PC (150 MB Android) | Jéssica Natália, Junior_Djjr, OjTrue, Zeneric, Jão, François Declasse | **every CJ texture in HD** (skin, faces, all clothes, tattoos redrawn), **same original model**, includes mobile hands (separate fingers); other CJ clothing mods still work if they are 1024 px | ModLoader folder; no plugin; delete an older version first | MixMods primary source, no re-upload |
| C2 | **Carl Johnson Remastered (INSANITY CJ)** — [mixmods](https://www.mixmods.com.br/2018/06/carl-johnson-remastered-insanity-cj/) | 2018 (author wants to restart, Patreon) | 570 MB (63 MB 7z) | Ezekiel | full re-creation of CJ's head/face + clothes + accessories + haircuts, works in cutscenes with facial expressions | ModLoader folder; **incomplete wardrobe** (that is why C3 exists) | hosted on ShareMods/Google Drive by MixMods; no re-upload |
| C3 | **CJ Insanity + Upscale COMPLETE** — [mixmods](https://www.mixmods.com.br/2021/06/cj-insanity-upscale-complete-todas-as-roupas-em-hd/) | 2021-06-06 (2024 note) | 185 MB | pack: Jéssica Natália, Junior_Djjr; base: Ezekiel + CJ Upscale HD; incl. Forgotten Clothes, Mobile Hands | mixes C2's head with HD-upscaled versions of the missing clothes/hair so the whole wardrobe exists | ModLoader folder; author calls it a rushed temporary mix, clothes mismatch each other | MixMods, no re-upload |
| C4 | **HD CJ** — [LibertyCity 167687](https://libertycity.net/files/gta-san-andreas/167687-hd-cj.html) | 2021-12-16 | 54 MB | diegoforfun | replaces CJ's model + textures, adds wardrobe items (masks) | ModLoader / img replacement | no permissions block |
| C5 | **15 different CJ skins** — [LibertyCity 217972](https://libertycity.net/files/gta-san-andreas/217972-15-different-cj-skins.html) | 2024-12-27 | 2.25 MB | ClementeJackson124 | 15 new CJ outfits built from internal game files (ped clothes, croupier, pimp, police) — variety, not quality | ModLoader folder, PC + Android | no permissions block |
| C6 | **Proper Tattoos Retex HD** — [mixmods](https://www.mixmods.com.br/2022/04/proper-tattoos-retex-hd/) | 2022-04 | — | Jéssica Natália | HD redraw of CJ's tattoos (already inside C1 for the classic game) | ModLoader folder | MixMods, no re-upload |

---

## 4. Remix verdict per candidate

| mod | what Remix keeps | what breaks / costs | verdict |
|---|---|---|---|
| P1 Proper Characters Retex HD | 133 peds get a genuinely HD albedo → Remix's auto material finally has something sharp to work with | the pack's 2 generic pedestrian materials (`mat_E6E920…`, `mat_A3537…`) stop matching for the retexed peds; 22 MB, no plugin, no mesh change | **do it** |
| P2 Emergency Peds Vary | 50 new uniformed peds with ~10 variants — kills the "one cop texture everywhere" look | new ped ids ⇒ `ped.dat`/`cped.ide` edits inside the modloader folder; the variant system assumes PedFuncs; no-reupload licence | **do it** (with PedFuncs) |
| P3 Criminal Peds Vary | same, for gangs; best-rated of the family (9.8/10, 49 votes, 1.5 K downloads) | 196 MB of 1 MB+ TXDs against a 2 GB no-LAA process; PedFuncs dependency | **do it, but measure memory first** |
| P4 Pedestrian Overhaul | 90+ new civillian peds | needs **four** prerequisites (Essentials Pack, OLA, PedFuncs, Model Variations) — that is not a two-step install any more; author admits rough implementation | hold |
| P5 GTA Trilogy Peds Pack | the best NPC silhouettes (separate fingers, facial bones) | 800 MB + PedFuncs + the author's own warning about pre-loading; with a no-LAA 32-bit process this is the pack most likely to reproduce the `docs/VEGETATION.md` hang | hold (optional experiment, one at a time) |
| P6 Original Peds Vary (87 MB HD build) | cheap variety over the stock peds | needs PedFuncs; overlaps P2/P3 | optional |
| P7 Extended and Enhanced Gangs | gang life/territory | needs CLEO, which is parked in this copy; overlaps P3 | skip for now |
| P8 Restored Unused Peds | free bits of lost content, 0.5 MB | nothing | cheap extra |
| P9 RoSA Evolved | HD map/vehicle textures + AI-upscaled peds as a bonus | already on disk but parked because of the Proper Fixes/Proper Shaders deadlock history (`GTAmods/.strata/PLAN.md`); under Remix the shader half is irrelevant, the textures are not | separate decision, not part of this shortlist |
| C1 Proper Player Retex v3.2 | CJ's face/skin/clothes/tattoos at 1024 px, same skeleton ⇒ animations, cutscenes, gang IDs, barbershop/skin selectors all keep working, no plugin | the pack's three CJ `mat_*` overrides stop matching ⇒ CJ loses the authored normal/roughness until we re-point them (§6) | **do it — top pick** |
| C2 / C3 Insanity CJ | a much better CJ head sculpt, facial expressions in cutscenes | old, wardrobe incomplete (C3 is a stopgap mix), 570/185 MB, and it also breaks the three CJ overrides | phase 2, after C1 works |
| C4 HD CJ | nothing better than C1/C2 in quality terms | model replacement with unknown rigging/wardrobe state, 2021 | skip |
| C5 15 CJ skins | outfit variety | does not improve quality | optional extra |

---

## 5. Ranked shortlist (top 3)

1. **Proper Player Retex v3.2 (C1)** — biggest visible gain per MB and the least risk. It changes only
   textures, keeps the stock CJ skeleton (so nothing in the game's ped plumbing moves), needs no plugin,
   is complete for the classic PC game, and was updated 2025-12-05. Under Remix it replaces the 2004
   18–788 KB CJ textures with 1024 px art, which is exactly the axis Remix rewards (§2). Cost: the pack's
   four CJ material hashes must be re-pointed (§6) to get CJ's authored normal/roughness back.
2. **Proper Characters Retex HD (P1)** — the NPC counterpart: 133 peds, textures only, 22 MB, no plugin,
   deliberately lore-friendly ("the same original GTA SA faces, in high definition"). It is the only ped
   pack that improves the *majority* of NPCs without adding a single new ped id, so it cannot conflict
   with anything else on this list.
3. **Emergency Peds Vary (P2) + Criminal Peds Vary (P3)** — one pack for cops/medics/firefighters, one for
   the gangs: ~100 new peds and 15–20 texture variants each, both from the same author (wizvich), both
   updated in November 2026, both explicitly "no ped feels out of place". This is what actually removes the
   repetition that makes the RTX streets look cheap. Cost: they are the only picks that pull in one extra
   plugin, [PedFuncs v0.5.1](https://www.mixmods.com.br/2022/07/pedfuncs/) (a single ModLoader `.asi`,
   open source, no CLEO/VehFuncs dependency), and P3's 196 MB must be measured against the no-LAA 2 GB
   budget (§1).

Install order if all three are taken: **P1 → C1 → PedFuncs → P2 → P3**, one restart per step, so a hang or
a memory problem names itself.

---

## 6. Which pack materials stop applying, and how to re-point them (the one real piece of work)

Remix matches a ped by **material hash**, so a game-side `.txd` replacement silently drops the pack's
authored PBR for that ped (§2.2). What that means per pack, with the exact node names re-read from the
`.usda` files on 2026-11-12 *(measured, read-only)*:

| pack | overrides that stop applying | where they live |
|---|---|---|
| **C1 Proper Player Retex** | `mat_0AB982F62FEF3381` (1024²), `mat_1AB00AD812498A17` (512×1024), `mat_C23DD21AE7ED34B1` (1024²) — the three CJ Looks — plus the shared `8645663DE7AFBB81_normal/rough.dds` pair they borrow from | `rtx-remix/mods/gameReadyAssets/texture/characters/CJ/CJ.usda` |
| **P1 Proper Characters Retex HD** | `mat_E6E920AB99F5FC23`, `mat_A3537FCA9CCB9E69` (1024²) — the two generic pedestrian Looks, for whichever of the 133 retexed peds used them | `…/texture/pedestrians/Pedestrian_materials.usda` |
| **P2 Emergency Peds Vary** | the two pedestrian Looks again for the firefighters it re-textures; the new cop/medic/sheriff ids never had overrides | same file |
| **P3 Criminal Peds Vary** | same, for the gang members it re-textures | same file |
| **P8 Restored Unused Peds** | nothing — it only re-adds cut ped models, it replaces no existing texture | — |
| not touched by any of these | `mat_389470BE6FE18A28` + `mat_1A74DD726E1EDCFE` + `mat_D355E496381564E1` (Ryder, 4096² + 1024² glasses), `mat_B10446A2B08526A8` (Kendl, 4096²), `mat_DA50E6D8BA27A905` (512²) | `…/texture/characters/{Ryder,Kendl,Character_materials}.usda` |

Two corrections to §1 as first written *(measured 2026-11-12)*: `CJ.usda` contains **three** `over
"mat_…"` blocks, not four — `8645663DE7AFBB81` is only the file-name prefix of the normal/rough maps the
CJ Looks share (its `_albedo.dds` sits in the folder unreferenced) — and `Big_Smoke.usda` is an empty
stub, so Big Smoke never had an override to lose.

Getting the authored PBR back after C1, two ways:

* **Cheap** (no capture, works the same evening): open the Remix developer/replacement menu, look at CJ,
  read the new `mat_<HASH>` names, and point each at the pack's existing
  `texture/characters/CJ/8645663DE7AFBB81_normal.dds` / `…_rough.dds` (they were derived from the same
  original art, so they still fit the new albedo). Keep the mod's albedo as `inputs:diffuse_texture`.
* **Proper** (survives a pack update): capture with replacements off (re-add the capture hotkey to
  `rtx.conf` — it was removed again on 2026-11-09 22:45, `docs/FINDINGS.md`), read the new
  `mat_<HASH>` names out of the capture, and add one `over "mat_<HASH>"` block per CJ material to
  `CJ.usda`, copied from the existing blocks with only the node name changed. Keep the old blocks too —
  the old hashes simply stop matching, they cost nothing. The block shape is:

  ```text
  over "mat_<NEWHASH>"
  {
      over "Shader"
      {
          asset inputs:diffuse_texture = @./<NEWHASH>_albedo.dds@ ( … )   # or the mod's own albedo
          asset inputs:normalmap_texture = @./8645663DE7AFBB81_normal.dds@ ( … )
          asset inputs:reflectionroughness_texture = @./8645663DE7AFBB81_rough.dds@ ( … )
      }
  }
  ```

  Same recipe for the two pedestrian Looks in `Pedestrian_materials.usda` after P1/P2/P3.

`tools/characters.sh on` and `… status` print this list (which hashes die for the packs actually
installed), so nobody has to remember it — see §8.1.

Do **not** re-enable the `rtx.geometryAssetHashRuleString` pin (it was wrong, `docs/FINDINGS.md`
2026-11-09 22:35).

---

## 7. Exact download links (all verified 2026-11-11 with `curl -sI`)

Checked 2026-11-11 with
`curl -sI -A 'Mozilla/5.0 … Chrome/120' -o /dev/null -w '%{http_code} %{redirect_url}' <url>`
(all 31 links in this file; **every one returned 200 or 30x**, none 4xx/5xx; raw output kept in
`build/tmp/doclinkcheck.txt`):

```
200  https://www.mixmods.com.br/2022/07/sa-proper-characters-retex-hd-peds-remaster/
200  https://www.mixmods.com.br/2024/12/proper-player-retex/
200  https://www.mixmods.com.br/2018/06/carl-johnson-remastered-insanity-cj/
200  https://www.mixmods.com.br/2021/06/cj-insanity-upscale-complete-todas-as-roupas-em-hd/
200  https://www.mixmods.com.br/2022/08/sa-gta-trilogy-peds-pack-skins/
200  https://www.mixmods.com.br/2021/03/original-peds-vary-peds-originais-variando-pedfuncs/
200  https://www.mixmods.com.br/2022/07/pedfuncs/
200  https://www.mixmods.com.br/2023/07/rosa-project-evolved-remaster-texturas-hd/
200  https://www.mixmods.com.br/2022/04/proper-tattoos-retex-hd/
200  https://www.mixmods.com.br/2022/07/sa-restore-unused-peds/
301 -> /2026/02/improved-streaming/    https://www.mixmods.com.br/2022/04/improved-streaming/
301 -> /2026/07/improved-streaming/    https://www.mixmods.com.br/2026/02/improved-streaming/
301 -> /2018/01/modloader/             https://www.mixmods.com.br/2015/01/SA-Modloader.html
301 -> /2026/05/modloader/             https://www.mixmods.com.br/2018/01/modloader/
301 -> /2015/02/open-limit-adjuster/   https://www.mixmods.com.br/2014/09/iiivcsa-open-limit-adjuster.html
301 -> /2022/10/open-limit-adjuster/   https://www.mixmods.com.br/2015/02/open-limit-adjuster/
200  https://libertycity.net/files/gta-san-andreas/240761-criminal-peds-vary.html
200  https://libertycity.net/files/gta-san-andreas/246820-new-females.html
200  https://libertycity.net/files/gta-san-andreas/230037-extended-and-enhanced-gangs-v4-0.html
200  https://libertycity.net/files/gta-san-andreas/179162-restored-unused-peds-unoffical-update.html
200  https://libertycity.net/files/gta-san-andreas/241335-authentic-peds-vary-for-pedfuncs.html
200  https://libertycity.net/files/gta-san-andreas/243071-expanded-peds-gangs-original-peds-vary.html
200  https://libertycity.net/files/gta-san-andreas/167687-hd-cj.html
200  https://libertycity.net/files/gta-san-andreas/217972-15-different-cj-skins.html
200  https://libertycity.net/files/gta-san-andreas/skins/skin-packs/
200  https://libertycity.net/files/246900-emergency-peds-vary.html
200  https://libertycity.net/files/246336-pedestrian-overhaul.html
301 -> /files/gta-san-andreas/246900-emergency-peds-vary.html   https://libertycity.net/files/246900-emergency-peds-vary.html
301 -> /files/gta-san-andreas/246336-pedestrian-overhaul.html   https://libertycity.net/files/246336-pedestrian-overhaul.html
301 -> /files/gta-san-andreas/240761-criminal-peds-vary.html    https://libertycity.net/file/240761
```

MixMods moves its permalinks (the `improved-streaming`, `modloader`, `open-limit-adjuster` chains above),
so the installer should hand the user the *oldest* URL it quotes and let the browser follow the redirect.

Short form for the three picks (use these in the installer's prompt):

* CJ: `https://www.mixmods.com.br/2024/12/proper-player-retex/` → "GTA San Andreas (clássico) – PC", 120 MB
* NPCs: `https://www.mixmods.com.br/2022/07/sa-proper-characters-retex-hd-peds-remaster/` → 22 MB
* Variety: `https://libertycity.net/files/gta-san-andreas/246900-emergency-peds-vary.html` (71 MB) and
  `https://libertycity.net/files/gta-san-andreas/240761-criminal-peds-vary.html` (196 MB)
* Plugin for the variety packs: `https://www.mixmods.com.br/2022/07/pedfuncs/` → PedFuncs v0.5.1

### What rhY has to download by hand

Everything on the list, actually — but these specifically because our side cannot get the file:

Re-checked by s27 on 2026-11-12 (curl, one attempt per host, no fighting a captcha). The exact file
name is now known for every one of these, because it is written in the download link:

| file | why we can't fetch it | the button we would need to click |
|---|---|---|
| Proper Player Retex v3.2 (PC, 120 MB) | sharemods serves a Cloudflare **Turnstile** widget to the `op=download2` POST (measured: the POST returns the same page with `cf-turnstile`, no bytes) | `https://sharemods.com/dckdhp4ir86q/Proper_Player_Retex.7z.html` (the page also offers `Proper_Player_Retex_for_Android.7z` and `SADE_-_Proper_Player_Retex.7z` — take the first) |
| Proper Characters Retex HD (22 MB) | same | `https://sharemods.com/eo1e9wvp3ank/SA_-_Proper_Characters_Retex_HD.7z.html` |
| PedFuncs v0.5.1 | same | `https://sharemods.com/fkum0x0mhyu6/PedFuncs.7z.html` |
| Restored Unused Peds (0.5 MB) | same | `https://sharemods.com/qcdn2us7y045/SA_-_Restored_Unused_Peds.7z.html` — **not** gtainside 194343, that is a different mod (§12.3) |
| Emergency Peds Vary (71 MB, `.rar`) | LibertyCity's button is JS (`data-href=/index.php?do=files&op=showfile&v2=0&lid=246900`, `data-download-token=""`) and that endpoint 302s a logged-out curl back to the file page | `https://libertycity.net/files/gta-san-andreas/246900-emergency-peds-vary.html` → file `emergency-peds-vary_1793981681_509472.rar`, dated 2026-11-06 |
| Criminal Peds Vary, **the 2026-11-03 build** | same | `https://libertycity.net/files/gta-san-andreas/240761-criminal-peds-vary.html` → `criminal-peds-vary_1793725831_688713.rar`, 205,560,101 B. The **2026-08-10 build is already fetched** (§12.1) |

Alternate hosts were checked and mostly do not exist: gtainside has no Proper Retex / Peds Vary
except Criminal Peds Vary (§12.1), LibertyCity has no MixMods re-upload, and `JuniorDjjr/PedFuncs`
has no GitHub releases (source only, MIT, needs `DK22Pac/plugin-sdk` to build).

Drop them into `REPO/vendor/characters/` (create it; add to `.gitignore`) with their original filenames.
That directory is staging only — it is never committed and never shipped.

---

## 8. Installing into the RTX copy (what the installer will do; not run in this task)

ModLoader replaces game files **by name at load time** (`modloader/modloader.log` shows
`Removing imported model file at index …`), so nothing in `models/*.img` is ever patched — which is what
makes this reversible.

```
GTASA-RTX/modloader/
  Proper Characters Retex HD/     <- unpack the 22 MB archive here, keep its internal layout
  Proper Player Retex/            <- the 120 MB archive; delete any older version of it first
  PedFuncs/PedFuncs.asi           <- only if the Peds Vary packs are used
  Emergency Peds Vary/
  Criminal Peds Vary/
```

1. Unpack each archive into its own folder under `modloader/` (never merge two packs into one folder:
   same-name files would silently win by priority).
2. If a pack ships `data/…` `.ide`/`.dat` edits (the Peds Vary packs do), keep them inside that pack's
   folder — ModLoader merges them; do not copy anything into the game's `data/`.
3. Set explicit priorities in `modloader/modloader.ini` so the order is deterministic, e.g.
   ```ini
   [Profiles.Default.Priority]
   Proper Characters Retex HD=40
   Proper Player Retex=60
   Emergency Peds Vary=50
   Criminal Peds Vary=50
   ```
   (default is 50; higher wins on a name clash — CJ's `player_*.txd` must come from Proper Player Retex.)
4. Keep `Improved Streaming` on (already installed) and **do not** turn on "preload all peds": the MixMods
   author warns against it for heavy ped packs, and we have no LAA headroom (§1).
5. Remove `modloader/Ped Spec/` (§2.4) — it does nothing under Remix and only muddies the material state.
6. Restart once per pack. Then check `modloader/modloader.log` for the `Removing imported model file at
   index …` lines and the Remix developer menu for CJ's new material hashes (§6).

### Implemented (s17): `tools/characters.sh` — the whole set as one switchable step

The list above is a script now, so "install the character packs" and "undo them" are one command each:

```bash
tools/characters.sh on     [GAME_DIR] [--pack NAME ...] [--stage DIR ...] [--partial] [--drop-ped-spec]
tools/characters.sh off    [GAME_DIR]
tools/characters.sh status [GAME_DIR]
```

* **Which packs, in which order, at which priority**: `deps/manifest.json`, group `characters`, read
  through the repo's one manifest reader (`tools/fetch-deps.sh --dump`). That order is §5's install
  order (NPC retex → CJ retex → PedFuncs → Emergency → Criminal → Restored Unused Peds), and the
  priorities are the §8.3 numbers. `--pack` takes a subset (manifest id, modloader folder name, or a
  path); the order stays the documented one however you name them.
* **PedFuncs is pulled in automatically** when a selected pack lists it in `requires` (the two Peds Vary
  packs do), and `--partial` prints a warning instead of installing them without it.
* **Where packs come from**: a stage dir, default
  `/media/rhy/data/Gamez/GTAmods/x/characters/stage`, then `REPO/vendor/characters`. A stage dir holds
  either an unpacked folder in modloader layout (`Proper Player Retex/`) or the archive the manifest
  names (`proper-player-retex-3.2-pc.zip`), unpacked with `tools/unpack.sh`. **One of six is staged
  today** *(s27, 2026-11-12: `stage/Criminal Peds Vary/` = 390 files / 670 MB from the gtainside
  archive, plus `stage/_criminal-peds-vary-extras/` for the two pieces one target folder cannot carry;
  `raw/part1..7` are still the gtainside "Original Peds in HD" parts, §12, not the §5 shortlist)*, so
  `on` refuses and names each pack, the folder to put it in and the author's page — it never installs
  half a set and calls it the recommended combination.
* **Priorities land inside `[Profiles.Default.Priority]`**, never appended at EOF (where they would fall
  into whatever section comes last), the file's own comments and its mixed CRLF/LF line endings survive,
  and `modloader.ini` is backed up pristine once, in the state dir.
* **State**: `modloader/.rhys-characters/` (a dot-dir ModLoader does not scan) records the pack folders,
  the copied files, the pristine backups and anything moved out (`--drop-ped-spec` *moves*
  `modloader/Ped Spec/` there instead of deleting it). `off` deletes only the folders `on` created,
  restores every backup byte-for-byte, moves Ped Spec back and removes the state dir; a pack folder it
  did not create is never touched, `on` and `off` both refuse it. `scripts/characters-selftest.sh` proves
  the round-trip hashes back identical against a fake tree under `build/tmp/` (it is part of
  `scripts/check-all.sh`), and the sandbox guard means it cannot be pointed at a real install by accident.
* **It also prints §6** — which `mat_<HASH>` overrides the installed packs knock out — on `on` and `status`.

---

## 9. Fitting the two-step end-user installer (and the licence reality)

End users must still get it down to: **1) downgrade to 1.0 US, 2) run our installer.**

Redistribution is the blocker, and it is not negotiable:

* Proper Player Retex, Proper Characters Retex, PedFuncs, Trilogy Peds Pack, Original Peds Vary —
  MixMods is the primary source and forbids re-upload ("share the same download link").
* Emergency Peds Vary and Criminal Peds Vary — the authors answered "No" to re-upload and "Under no
  circumstances" to using the assets, on the LibertyCity page.

So the installer must **not** bundle the archives. Design that keeps it two steps:

1. `deploy.sh --characters` (new flag, same script that already installs `rhyremix.asi`) prints the five
   links from §7 and stops.
2. The user drops the downloaded archives into `<game>/rhyremix-incoming/` (or
   `REPO/vendor/characters/` when rhY is preparing a machine) and re-runs the installer.
3. The installer then unpacks each archive into `modloader/<pack name>/`, writes the
   `[Profiles.Default.Priority]` block, removes `Ped Spec`, and prints what it did.
   Detection is by archive name pattern (`*proper-player-retex*`, `*emergency-peds-vary*`, …).
4. If the incoming folder is empty, the installer still installs the plugin and simply reports
   `characters: not installed (5 files missing)` — the game keeps working exactly as today.

That is still "downgrade, run installer" for the user, with one extra paste of downloaded files, and it
keeps every author's terms intact. If rhY ever wants a single-file bundle, the only legal route is written
permission from Jéssica Natália (MixMods) and wizvich (LibertyCity) — worth asking, not assumed.

### Implemented (s09): `deps/manifest.json` + `tools/fetch-deps.sh`

The design above is now a file and a script, so "we host everything" is a checklist instead of an intention:

* `deps/manifest.json` (schema 1) lists every file a user would download by hand — the five packs in §7, the
  README step-1 mod list (ModLoader, SilentPatch, OLA, MixSets, Improved Streaming, Ped Spec, ImVehFt,
  MoonLoader, Moon ImGui, MoonAdditions, GInput, Framerate Vigilante, Proper Ragdoll), the 1.0 US downgrader,
  GTASA-Remix, the RTX Remix 1.6.1 runtime, and the vegetation pack with its Essentials Pack + Proper Fixes
  requirement. Per entry: `name`, `size`, `sha256`, `source_page`, `author`, `license`, `redistribute`
  (yes/no/ask), `target` in the game folder, and `urls` of kind `mirror`/`torrent`/`magnet`/`direct`/`source`.
* `tools/fetch-deps.sh MANIFEST DEST` fetches them (aria2c for torrent/magnet, curl for https), resumable, and
  only names a file after its sha256 matched — a half file stays `<name>.part`, a wrong file becomes
  `<name>.bad-sha256`. `--plan` prints what may be unpacked; `apply.sh --with-deps` unpacks exactly that into
  `modloader/<pack>/` via `tools/unpack.sh` (one folder per pack, one top-level folder flattened).
* `redistribute: "no"` entries keep `mirror`/`torrent` at `TBD` on purpose: MixMods forbids re-upload and the two
  Peds Vary authors answered "No" / "under no circumstances" (§9 above), so until there is written permission the
  fetcher prints the author's page as a manual download instead of mirroring them. `sha256: "TBD"` likewise stops
  a fetch — unverified bytes never reach a game folder.
* Tests: `tests/test_fetch_deps.py` (36) against a local `http.server` — sha mismatch, resume after a cut
  connection, a server that refuses ranges, missing/broken aria2c, `TBD` urls and hashes, and manifest validation.

### Implemented (s14): the bundle we host ourselves (`tools/make-release.sh`)

"We host what users cannot fetch themselves" needs a bundle, a hash list and a torrent. They exist:

* `tools/make-release.sh OUTDIR` reads the manifest through the same reader `fetch-deps.sh` uses
  (`--dump`), collects every entry whose file is staged in `vendor/` (or `--stage DIR`, matched by `name`
  or `upstream_name`), and writes `OUTDIR/rhygtasa-deps-<version>/` with the archives, `SHA256SUMS`, a copy
  of the manifest and a README; next to it the `.torrent`, the `.magnet.txt` and `bundle.json`. The magnet
  is never inside the folder: a file cannot contain the hash that covers it.
* One torrent for the whole folder, trackers from `deps/trackers.txt` (one announce URL per line, no
  comments; the three listed answered a UDP connect on 2026-11-11). `mktorrent` is used when installed,
  `transmission-create` or `tools/torrent.py` otherwise, and the result is **verified**: `tools/torrent.py
  check` re-derives the info hash from the bytes on disk (piece by piece) before a magnet is printed.
* Licences still decide: only `redistribute: "yes"` is collected; `--include-ask` / `--include-no` widen it
  and require `--permission-note "who said yes, when, for what"`, which is written into the bundle README
  and `bundle.json` so the permission travels with the files. A staged file whose hash contradicts the
  manifest stops the release (exit 1) instead of publishing the wrong bytes.
* `--update-manifest` writes `sha256`, `size` and the `magnet` of every collected entry back into
  `deps/manifest.json` (`tools/manifest-update.py` edits the text, so the diff stays reviewable); the https
  `mirror` slots stay `TBD` until there is a web host. The five §7 packs keep no magnet slot at all.
* Nothing is bundled today because nothing is staged: `vendor/` does not exist, and the
  `GTAmods/x/characters/stage` folder named in the task does not exist either (what is on disk is
  `x/characters/raw/part1..7/` and `dl/characters/*.7z`, none of them the §7 archives). GTASA-Remix alone is
  ~10 GB (GitHub reports 10329625 KB), which is why the transport is a torrent at all.
* Tests: `tests/test_make_release.py` (24) on a fake set in `build/tmp` — the info hash is recomputed by an
  independent bencode/piece verifier in the test, `sha256sum -c` verifies, `fetch-deps.sh` installs the
  bundle over `file://` and a local `http.server`, a tampered file breaks the check, and the licence rules
  are pinned.

---

## 10. Rollback

Everything lives in `modloader/`, so rollback is deletion, never restore-from-backup:

* One command (s17): `tools/characters.sh off <game>` — it deletes exactly the pack folders
  `on` created, restores `modloader.ini` from the pristine copy in `modloader/.rhys-characters/backups/`,
  moves `modloader/Ped Spec/` back if `--drop-ped-spec` had moved it, and removes the state dir.
  `scripts/characters-selftest.sh` checks that this leaves the tree byte-identical.
* One pack: move `modloader/<pack>/` out (e.g. to `GTAmods/removed-from-game/rtx-parked/`), restart.
* Everything: move the five pack folders out, delete the `[Profiles.Default.Priority]` lines, put
  `modloader/Ped Spec/` back if it was kept, restart. The game is back to stock 1.0 peds, because
  `models/player.img` and `models/gta3.img` were never touched *(measured: player.img hardlink intact)*.
* Softer switch without deleting: `modloader.ini` → `[Profiles.Default.IgnoreMods]` with the pack name, or
  priority `0` (the ini's own comment says priority 0 = ignored).
* If CJ looks flat after C1 (lost authored PBR, §2.2), the fallback is to remove C1 and keep P1 — the pack's
  three CJ `mat_*` overrides start matching again by themselves.
* Keep the pre-change state documented in `docs/MODS-REPLACED.md` (inventory table) as part of the change.

---

## 12. s27 (2026-11-12): what is actually downloaded, and the gtainside HD ped parts

### 12.1 Downloaded and verified

| pack | from | bytes | sha256 | archive test |
|---|---|---|---|---|
| Criminal Peds Vary (2026-08-10 build) | `https://files.gtainside.com/202608/V8fzGui8_Criminal_Peds_Vary.rar` (page `/sanandreas/skins/other-skins-skin-packs/220708-criminal-peds-vary`, "Added 08/09/2026, Last update 08/10/2026", author wizvich) | 205,557,524 | `9b267df95116bfc495ea4c3d43a3adcfcee4bd51a66dd13c1f25bc1e4f640f65` | `unrar t` OK, 606 entries |

GTAinside prints the file URL in clear text on its `/download` page and `files.gtainside.com` serves
it to plain `curl -L` — that is how the seven "Original Peds in HD" parts got here too. **Licence
note:** the LibertyCity page answers "No" to re-upload, so gtainside's copy is itself an unauthorised
re-upload. We record the url so the fetch is reproducible; `redistribute` stays `no` and the pack is
not bundled. LibertyCity's current file is 2,577 B larger (205,560,101 B, dated 2026-11-03) and stays
a hand-download.

What the archive contains *(measured after extract)*: `modloader/Criminal Peds Vary/` (390 files,
670 MB, `peds.ide` + `pedgrp.dat` + `cargrp.dat` + 11 gang folders), **a second modloader folder**
`modloader/Expaned gang wars/` (`ExGangWars.asi` + `.ini`, "or any mod that enables gang9 and gang10
to spawn"), and `ModelVariations/ModelVariations_Peds.ini`, which must replace the file that comes
with MixMods' ModelVariations. One manifest `target` = one folder, so the last two are staged at
`GTAmods/x/characters/stage/_criminal-peds-vary-extras/` with a README naming the two `cp` commands.
The pack's own `1. IMPORTANT!!!.txt` requires **PedFuncs, ModelVariations, Silent ASI Loader, OLA and
ModLoader**; measured in the RTX copy: ModLoader (`modloader.asi`), `modloader/OLA` and
`SilentPatchSA.asi` (which carries the ASI loader) are there, **PedFuncs and ModelVariations are not**.

### 12.2 The gtainside "Original Peds in HD" parts vs Proper Characters Retex HD

`dl/characters/gtainside_original_peds_in_hd_part{1..7}.7z` (34.4/30.4/33.9/32.9/31.2/29.2/18.1 MB),
extracted to `x/characters/raw/part1..7/`, measured 2026-11-12:

* 584 model files = **278 `.dff` + 278 `.txd`**, 278 unique ped names, no name appears in two parts.
* **All 558 names exist in the stock `models/gta3.img`** (checked entry-by-entry against the TOC:
  558/558) → it is a pure name-replacement of existing stock peds: no new ped id, no `.ide`/`.dat`.
* It replaces **meshes as well as textures**, and both are much heavier than stock:
  TXDs 361.9 MB vs 39.7 MB for the same names (**9.1×**), DFFs 184.9 MB vs 23.4 MB (**7.9×**).
  Biggest: `cat.txd` 5,079,920 B, `gangrl3.txd` 4,138,120 B, `vhfyst3.txd` 3,573,572 B — above the
  2.5 MB largest-TXD-inside-gta3.img figure of §1.
* `tools/txd_sizes.py` parses the stock TXDs but finds **0 textures in every pack TXD**: the chunk
  stream has the same RW shape (`ff ff 03 18`, raster chunk type `0x15`) but a raster header our
  reader does not recognise, i.e. an old re-encoding by 2015-era tooling. So their RAM footprint
  cannot be measured with our tooling; the 9.1× on-disk factor is the proxy.
* The gtainside page is titled **"Original HD Peds Pack (Part 1)"** and its file URL is
  `https://files.gtainside.com/201508/1472304724_original peds in hd part 1.7z` — a 2015/2016-era
  upload (path month 201508, token timestamp 1472304724 = 2016-08-27), not a current pack.

Overlap with P1: both are name-replacements of stock peds in `gta3.img`, so with both installed
ModLoader priority decides per file (P1 is 40, the gtainside parts would default to 50 → the
gtainside `.txd` would win unless the priorities say otherwise). The exact name intersection cannot
be computed until P1 is on disk; P1 covers 133 peds textures-only, these cover 278 peds mesh+texture.

**Recommendation: Proper Characters Retex HD (P1), and leave the gtainside parts parked in `raw/`.**
Why: (a) P1 changes no geometry, so the Remix geometry-hash rule and §6's material story stay exactly
as documented, while 278 replaced meshes move every one of those peds to a new mesh; (b) 22 MB vs
547 MB of replacements (361.9 MB TXD + 184.9 MB DFF) in a 32-bit process — this is precisely the
`docs/VEGETATION.md` failure mode; (c) 2022 art (AI upscale + manual redraw, deliberately
lore-faithful) beats a 2015 re-sculpting of 2005 models, which is as likely to *hurt* the silhouette
as improve it; (d) P1's TXDs are measurable by our own tools and the pack needs no plugin. If anyone
ever wants the gtainside parts, they need their own manifest entries (one folder per part, or one
merged folder) and a commit-size measurement first.

### 12.3 A trap: gtainside's "Restored Unused Peds" is not P8

gtainside 194343 `restored-unused-peds` → `https://files.gtainside.com/202401/1705784655_unused-peds.rar`,
8,569 B, sha256 `ec95d71c8a7cbf80d9cd796a3610a7edb08afafab99e3a2d7130803e96c52c68`, `unrar t` OK.
Contents: `data/peds.ide`, `data/pedgrp.dat`, `ReadMe.txt`, two YouTube/Discord links — a **data-only**
mod by "Dragomazing" that adds `VHFYST`, `LVPDM1` and the `IMYST` voice, whose ReadMe says "DO NOT
RE-UPLOAD TO ANY WEBSITE … WITHOUT MY PERMISSION". No cut ped models, so it is not the MixMods P8
pack; extracted to `x/characters/raw/restored-unused-peds-gtainside-194343/` for the record and **not
staged**. gtainside 188830 "restored unused peds esrgan upscaled" is the same author's texture set,
also not P8.

---

## 11. What to measure after each step

* `modloader/modloader.log` — the pack loaded, model indices replaced.
* Remix developer menu — CJ's material hashes changed; the pack's authored maps are (not) applied.
* Process commit size of `gta-sa.exe` (no LAA, §1) before/after each pack; treat >1.6 GB commit as a stop
  signal, and bisect the same way as `docs/VEGETATION.md` if the loader hangs.
* One F12 screenshot (`./deploy.sh --shotkey-on`) per step, same spot in Grove Street, same hour, for the
  before/after record.
