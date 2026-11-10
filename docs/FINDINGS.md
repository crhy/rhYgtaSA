# Findings: GTA SA + RTX Remix on Linux (2026-11-09)

Working log from getting GTASA-Remix running under Proton. Paths refer to the author's machine.

## Status 2026-11-09 evening
- Runs via GTASA-RTX/launch-rtx.sh (Steam Linux Runtime 4 + Proton Experimental, own prefix ~/.local/share/gtasa-rtx-pfx). Desktop + menu launcher.
- DLSS works (rhY: solid 60 FPS). DLSS frame generation hard-crashes under Proton: keep off. Reflex unsupported under Proton.
- VehFuncs ignored (needs MixMods OLA build: "Unknown limit adjuster" then crash). GInput deadlocked xinput_cs with hidraw; retrying with PROTON_PREFER_SDL=1 PROTON_ENABLE_HIDRAW=0.
- SARemix MoonLoader scripts (Sun, Light Manager, VTK) die on opcodes 00A0/00DF -> lighting flat/blue. Testing without CLEO 5 (cleo.asi parked in GTAmods/removed-from-game/rtx-parked). SARemix_VTK.lua parked (missing SARemix_DebugPrinter module; SARemiv_VTK.lua is the working copy).
- Gray square under CJ: MixSets UseHighPedShadows=0 (static) to stop the real-time shadow render target; untested.
- Water + leftover marker models need texture hashes tagged in Remix developer menu (rtx.animatedWaterTextures / rtx.ignoreTextures).
- 2026-11-09 17:15: CLEO was NOT the MoonLoader cause. Real cause: MoonLoader's built-in PLAYER_PED handle is invalid here (Compact exe?);
  getCharCoordinates(PLAYER_PED) throws. Patched SARemix_Sun.lua, SARemiv_LightMan.lua, SARemiv_VTK.lua: wait for isPlayerPlaying, then
  PLAYER_PED = getPlayerChar(PLAYER_HANDLE); Sun also loadAllModelsNow + doesCharExist guard. Originals in removed-from-game/rtx-parked/lua-orig.
  CLEO still parked (can come back if not needed for this). Gray square + controls (GInput via PROTON_PREFER_SDL=1, HIDRAW=0) confirmed fixed by rhY.
- user.conf never written: rhY's in-menu tags not saved yet.
- 17:25 RTX gta-sa.exe now its own no-LAA copy (hardlink broken): suspected MoonLoader 0.27 opcode-output crash with LAA.
- 18:10: sa_ped_safe.lua (memory reads) stops all MoonLoader crashes. rtx.conf pinned rtx.geometryAssetHashRuleString = positions,indices,texcoords,geometrydescriptor,vertexlayout
  (1.6.1 default adds vertexshader; SARemix authored on 0.5.x) -> CJ house light appeared; one Remix crash seen after it (watch).
  25 project textures referenced but missing from the GitHub repo (harmless). LightMan: loadAllModelsNow after requestModel + [dbg] counts every 10 s.
- 22:15: Remix 0.5.4 (project's own runtime) crashes before loading screen on driver 610/Proton -> back on 1.6.1 (0.5.4 parked).
  LightMan ran (91 objects, 13 lights) then VK_ERROR_DEVICE_LOST twice. Suspect: 1034 lights with unset inputs:shaping:focus/focusTint
  + empty ies:file -> NaN in 1.6. Fixed all 3150 with tools/usd_light_defaults.py (backups in removed-from-game/rtx-parked/usda-orig). LightMan re-enabled.
- 22:35: MY ERROR: the pinned hash rule (positions,indices,texcoords,geometrydescriptor,vertexlayout) was wrong. 1.6.1 default asset rule is
  positions,indices,geometrydescriptor (same as 0.5.4) -> removed the pin. Capture (Ctrl+Shift+P, needs replacements off) exported 417 meshes,
  0 matched project hashes under the wrong rule. Capture hotkey kept in rtx.conf. Earlier 'no lights' on default rule = LightMan was crashing.
- 22:45 ROOT CAUSE street lights: repo ships stale SARemiv_LightMan.lua whose anchor model ids 15892-15903 predate 5 cloud models added to
  remix.ide (anchors now 15897-15908) -> it spawned clouds/wrong cones. Shifted ids +5. Same staleness: SARemiv_VTK.lua (headlights 15916..)
  replaced by newer SARemix_VTK.lua (headlight 15923, matches ide) + stub lib/SARemix_DebugPrinter.lua + player-wait/sa_ped_safe patch.
  Default hash rule confirmed by capture: 271 project meshes matched. Replacements re-enabled.
- 23:10: Absolute D:/ paths fixed (nullObject.usd etc.) -> street lights stable + cast light. LightMan flicker loop disabled (pause 1e9),
  search radius 400->1200 (limited by lamp streaming; full fix = static lamp list from IPLs = Strata task). Street-light emissive x60,
  NullObject plates scale 1 and -0.25 m below housing. Light extents 50->300 m, headlight intensity x3 (test for "CJ goes dark near wall").
  Capture hotkey removed. Doors (gym, Idlewood Cluckin' Bell) not enterable: likely early-story locks (save at start, $371) - unconfirmed.
  Backups: removed-from-game/rtx-parked/{usda-orig,usda-pre-range,*.before-*}.
