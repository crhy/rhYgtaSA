# rhYgtaSA — GTA San Andreas RTX Remix on Linux (Proton)

Fixes to run **GTA San Andreas** with **NVIDIA RTX Remix 1.6.1** (path tracing + DLSS) on **Linux through Proton**, on top of
Hemry's [GTASA-Remix](https://github.com/Hemry81/GTASA-Remix) project. Tested on an RTX 4070 Ti SUPER, NVIDIA 610, Proton Experimental,
Steam Linux Runtime 4: solid 60 FPS with DLSS, path-traced sun, street, building and vehicle lights.

This repo contains **only our fixes** (patches, helper scripts, tools, launcher). It does not include the game, NVIDIA's Remix
runtime, or GTASA-Remix's assets — get those from their original sources.

## What it fixes
| Problem on Linux / Remix 1.6.1 | Cause | Fix |
|---|---|---|
| Game freezes on start, controller dead | Wine `xinput` lock deadlock (GInput + Remix bridge input hooks, DualShock over hidraw) | `PROTON_PREFER_SDL=1 PROTON_ENABLE_HIDRAW=0` |
| No audio / bridge server never starts | Proton run outside the Steam runtime | launcher runs Proton inside `SteamLinuxRuntime_4` |
| DLSS missing | NGX needs NVAPI | `PROTON_ENABLE_NVAPI=1`; DLSS frame generation off (hangs the bridge under Proton) |
| White window after first frame | Remix's new GUI input method under Wine | `rtx.useNewGuiInputMethod = False` |
| Sun, street lights, vehicle materials dead (MoonLoader scripts die) | `getCharCoordinates`/`isCharInAnyCar` opcodes throw; scripts start before CJ exists | `moonloader/lib/sa_ped_safe.lua` (memory reads) + wait for the player |
| Street lights never appear | Stale `SARemiv_LightMan.lua` uses model ids from before 5 cloud models were added to `remix.ide` (+5 shift) | ids fixed |
| Street lights flicker forever | random pause timer re-triggers the warm-up flicker | lights stay on after warm-up |
| GPU crash (`VK_ERROR_DEVICE_LOST`) when lights spawn | ~1000 lights declare `inputs:shaping:focus`/`focusTint` without a value (NaN in 1.6) | `tools/usd_light_defaults.py` gives defaults |
| Crash: "Light prims anchored to a mesh replacement must also include actual meshes" | USD references point at the author's `D:/Games/...` | relative paths |
| Vehicle toolkit can't load | missing `SARemix_DebugPrinter` module; old `SARemiv_VTK.lua` uses wrong ids | stub module; use `SARemix_VTK.lua` |
| Gray square under CJ | game's real-time ped shadow render target | MixSets `UseHighPedShadows = 0` |
| Dim lamp heads | emissive material scaled to 0 | brighter emissive plate under the lamp housing |

Full write-up with logs and dead ends: [docs/FINDINGS.md](docs/FINDINGS.md).

## Install
1. GTA SA **1.0 US** (downgrade the Steam version), with ModLoader, SilentPatch, Open Limit Adjuster, MixSets, Improved Streaming,
   Ped Spec, ImVehFt, MoonLoader 0.27 + Moon ImGui + MoonAdditions (see the GTASA-Remix README).
2. Copy [GTASA-Remix](https://github.com/Hemry81/GTASA-Remix) into the game folder, then replace its runtime with
   **RTX Remix 1.6.1** `rtx-remix-for-x86-games-release-remix-1.6.1.zip` (`d3d9.dll` + `.trex/`) from NVIDIA's releases.
3. `./apply.sh /path/to/your/GTASA-RTX` — backs up every file it touches, applies `patches/gtasa-remix-linux.patch`,
   adds the helper modules, the Proton bridge setting and the launcher.
4. Run `/path/to/your/GTASA-RTX/launch-rtx.sh` (env vars at the top let you point at your Steam/Proton paths).

Do **not** use Steam-version saves on a 1.0 install, and never "Verify integrity" in Steam (it undoes the downgrade).

## Known issues
- Street lights only appear once the game streams the lamp post in (a static lamp list is in progress).
- Headlight beam tilt under Remix 1.6 is being tuned.
- 25 textures referenced by GTASA-Remix are missing from its repository.

## Credits
Hemry and the GTASA-Remix contributors (MIT); NVIDIA RTX Remix; MixMods/Junior_Djjr (MixSets, ModLoader ecosystem); FYP & BlastHack (MoonLoader).
Our fixes are MIT licensed. Patched GTASA-Remix files remain © Hemry under MIT — see [THIRD_PARTY.md](THIRD_PARTY.md).
