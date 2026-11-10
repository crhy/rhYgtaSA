# GTA San Andreas modding on Linux (Steam + Proton): notes

Lessons from building two GTA SA setups on Linux in November 2026: a "realism" build (Proper Shaders, Proper Ragdoll,
RoSA Evolved, V Vegetation Mix) and an RTX Remix build (this repo). Proton Experimental, NVIDIA RTX 4070 Ti SUPER, driver 610.

## Downgrading the Steam version
- Almost every plugin (ModLoader, CLEO, Proper Ragdoll, Proper Shaders, MoonLoader) needs the **1.0 US** exe. The Steam 3.0 exe
  makes Proper Ragdoll log "unsupported executable" and ModLoader show "unknown game".
- The common "SA Downgrader for Steam/RGSC" (`install.bat` + `jptch.exe` + `SA DOWNGRADE/*.diff`) runs fine under plain Wine with no
  display: `DISPLAY= wine jptch.exe <old> "SA DOWNGRADE/<NAME>.diff" <old>.down`. Watch the case of file names: the `.bat` uses lower
  case, Linux does not ignore case. The result is the 1.0 US "compact" exe (5,189,632 bytes); `main.scm` 3,079,599, `script.img` 581,632.
- Back up the Steam files first, and **never "Verify integrity"** in Steam afterwards: it restores the 3.0 files.
- **Steam-version saves do not work on 1.0.** Loading one gives CLEO "Opcode [0802] not found in script 'intman'" (the saved script
  position points into the Steam main.scm) and interiors break. Start a new game.
- The game is 32-bit: use the 32-bit Ultimate ASI Loader (`vorbisFile.dll` works under Proton with no DLL override). A 64-bit
  `dinput8.dll` in the folder does nothing.
- Large-Address-Aware: setting bit 0x20 of the exe's PE characteristics (offset `e_lfanew + 22`) gives the game 4 GB, needed for
  big HD texture packs. One bit, keep a backup.

## Downloads
- MixMods files are hosted on sharemods.com behind a Cloudflare challenge: scripts cannot fetch them, download in a browser.
  GitHub-hosted parts (SilentPatch, CLEO 5, CLEO+, ModLoader, Ultimate ASI Loader, Remix runtime) can be scripted.
- RoSA Evolved: the free public build is a separate, unlocked post on the author's Patreon (no payment needed). Reuploads elsewhere
  are not allowed by the author. It needs the *RoSA build* of Proper Fixes, loaded above RoSA in ModLoader.

## Proper Shaders build under Proton (open problems)
- **Proper Fixes (RoSA build) + Proper Shaders 09-26 deadlock while loading** (~90 % bar, right after `water.dat`).
  Proton's `winedbg` (run with the game's prefix: `WINEPREFIX=<compatdata>/pfx <proton>/files/bin/wine winedbg`, then
  `attach <pid>` / `bt all`) shows the main thread spinning in `CdStreamGetStatus` (0x4063E0), called from ModLoader's `std.data`,
  called from `ProperShaders.asi`; ModLoader's streaming thread sits idle (a lost wake-up).
  Ruled out: ModLoader 0.3.9 vs 0.3.10, SilentPatch, Proper Streaming, its Texture Bridge, the LAA flag, `ProperFixes.asi`,
  Proper Fixes' `Misc` folder. Unpacking Proper Fixes' `.img` archives into folders instead crashes `std.stream` (runs out of model
  indexes). Without Proper Shaders, Proper Fixes loads. Only the RoSA build of Proper Fixes was tested.
  Untested ideas: GE-Proton or Proton 9, a newer Proper Shaders build.
- **Proper Streaming's Texture Bridge** (`ProperStreaming64.exe`, textures held by a 64-bit helper) hangs loading under Proton:
  set `[TexBridge] Enabled=0` and `BridgeTextures=0`.
- ModLoader's `std.dmaudio` plugin fails to start (affects sound-replacement mods).
- Widescreen Fix's SMAA duplicates Proper Shaders' anti-aliasing: set `SMAA = 0` in `GTASA.WidescreenFix.ini`.

## 60 FPS and an FPS counter
- Install **Framerate Vigilante** before going above 30 FPS (physics, swimming and missions break otherwise).
- MangoHud (`apt install mangohud mangohud:i386`) with a per-exe config `~/.config/MangoHud/wine-gta-sa.conf`
  (`fps_limit=60`, `toggle_hud=Shift_R+F12` to hide it while recording); Steam launch option `MANGOHUD=1 %command%`;
  turn the in-game Frame Limiter off. Example config: `config/MangoHud-wine-gta-sa.conf`.

## Controllers
- A DualShock through Proton's hidraw path deadlocked Wine's xinput lock against GInput + the Remix bridge.
  `PROTON_PREFER_SDL=1 PROTON_ENABLE_HIDRAW=0` fixed it; GInput then gives right-stick camera and PlayStation button icons.
  Outside Steam, Steam Input does not apply.

## RTX Remix build
See the main [README](../../README.md) and [FINDINGS](../FINDINGS.md). Key points:
- Run Proton inside `SteamLinuxRuntime_4` (`_v2-entry-point --verb=waitforexitandrun -- proton waitforexitandrun gta-sa.exe`);
  plain `proton run` outside the runtime had no audio and the Remix bridge server never started.
- DLSS works with `PROTON_ENABLE_NVAPI=1`; DLSS frame generation and Reflex do not (frame generation hard-crashes).
- Remix 0.5.4 (the version GTASA-Remix was authored on) crashes before the loading screen on driver 610 under Proton; 1.6.1 works.
- Remix 1.6.1's default asset hash rule (`positions,indices,geometrydescriptor`) matches GTASA-Remix's assets. Do not pin
  `rtx.geometryAssetHashRuleString` to a longer rule.
- `rtx.enableReplacementAssets = False` is required for Remix's scene capture; captures need the `captures/meshes` folder to exist.
