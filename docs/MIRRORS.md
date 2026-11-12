# Rear-view mirrors that really reflect (RTX Remix)

GTA SA does not have mirrors, it has **paint**: the mirror glass is a couple of triangles inside the
car's body atomic, textured with a pre-baked "reflection" strip from `models/generic/vehicle.txd`.
Under RTX Remix every surface is path traced, so nothing about the geometry has to change — the
surface just needs a perfectly smooth metallic material and Remix reflects what is really behind it
(behind CJ: the road, the traffic, the cop car you finally notice).

This is the same trick the GTASA-Remix pack already uses for the mirrors of its own re-authored
taxi/police meshes: `def Material "Mirror"` in `rtx-remix/mods/gameReadyAssets/meshes/Vehicles.usda`
is `AperturePBR_Opacity` with `metallic_constant 1`, `reflection_roughness_constant 0`, albedo
`(1, 0.99999, 0.99999)`. Stock cars get nothing, because the pack's override is bound to its own
meshes, not to the game's material for the mirror texture.

`tools/mirrors.py` writes the missing piece:
`<game>/rtx-remix/mods/gameReadyAssets/rhy_mirrors.usda`, one
`over "RootNode" { over "Looks" { over "mat_<HASH>" { over "Shader" { ... } } } }` per texture hash,
sublayered into `mod.usda`.

---

## 1. Collect the hashes — 2 minutes, in game

Remix computes the texture hash itself when the game uploads the texture, so it cannot be read out
of the `.txd` files; you read it off the running game once and it never changes.

1. Start the game with Remix (`launcher/launch-rtx.sh` / the `gtasa-rtx` launcher), get into any car.
   Stand outside and look at the door mirror, or drive in 1st person and look at the wing mirror.
2. Open the **RTX Remix developer menu** — default **Alt+X** (`rtx.remixMenuKeyBinds`; it is not
   overridden in this `rtx.conf`, so the default applies).
3. **Hover the mouse pointer over the mirror on screen.** The *Texture Info* popup appears with
   `Texture`, its size and `Hash: 0x…` — that is the number you want.
   **Middle-click** (or press the popup's *Copy Texture hash* button) to put it on the clipboard.
   (The menu's own hint: "Hover over an object on screen, or an icon in the grid below.
   Left click to assign categories. Middle click to copy a texture hash.")
4. If the hover is fiddly, go to the texture grid instead (**Step 1: Categorize Textures**): it shows
   a thumbnail of every texture the game uploaded with its size. Look for
   **128 x 128** and these two thumbnails:
   * `xvehicleenv128` — a hard **3-band** strip: dark brown at the top, a light blue-grey band in
     the middle, dark brown at the bottom. Only 13 distinct colours. **This is the mirror glass.**
   * `vehicleenvmap128` — a smooth **sky-to-ground** gradient (blue at the top, pale in the middle,
     brown, then black). The fake environment reflection; add it only if the first one is not enough.
   Hovering a thumbnail also shows its hash, and middle-click copies it.
5. Paste the hash (or both) into `config/mirror-hashes.txt` — one `0x… + 16 hex digits` per line,
   keep the trailing `# comment` that says which texture it is.

Do **not** add these, they are other car parts and would turn into mirrors:
`vehicleshatter128` (cracked-window overlay), `vehiclegrunge256` (dirt), `vehiclescratch64`,
`vehiclespecdot64`, `vehicletyres128`, `vehiclesteering128`, `vehicledash32`,
`vehiclelights128` / `vehiclelightson128`, `vehiclepoldecals128`, `carplate`, `carpback`,
`plateback1..3`, `platecharset`, and the per-car `<car>92interior128` / `<car>92wheel*` /
`remap<car>body256`.

## 2. Run the tool

```bash
cd ~/rhYgtaSA
python3 tools/mirrors.py                      # GAME_DIR defaults to /media/rhy/data/Gamez/GTASA-RTX
python3 tools/mirrors.py --dry-run --print    # show the USD without touching the game
python3 tools/mirrors.py --hash 0xDEADBEEFDEADBEEF   # one-off, without editing the config
python3 tools/mirrors.py --remove             # undo
```

What it does, in order:

* reads the hashes from `config/mirror-hashes.txt` (plus any `--hash`), refuses if there are none;
* generates `rhy_mirrors.usda` and **validates it before writing**: first line `#usda 1.0`,
  balanced braces/brackets/parens (comments, strings and `@asset@` paths skipped), and only inputs
  that `materials/AperturePBR_Opacity.mdl` actually declares;
* backs `mod.usda` up **once** to `mod.usda.bak-rhy-mirrors` (never overwritten) and inserts
  `@./rhy_mirrors.usda@` into `mod.usda`'s `subLayers` — running it twice leaves exactly one entry;
* prints `All checks passed.`

USD composes `subLayers` strongest-first, so the layer goes in **first**, ahead of the pack's
`SARemix.usda` vehicle materials. If a test in game shows the pack still wins, use
`--position last` (and tell the notes).

`--remove` deletes `rhy_mirrors.usda` and restores `mod.usda` from the backup; if the backup is
gone it strips just our sublayer line instead. Either way `mod.usda` ends up byte-identical.

## 3. What the material is, and why those numbers

```
over "mat_<HASH>"
{
    over "Shader"
    {
        uniform asset info:mdl:sourceAsset = @./materials/AperturePBR_Opacity.mdl@
        uniform token info:mdl:sourceAsset:subIdentifier = "AperturePBR_Opacity"
        color3f inputs:diffuse_color_constant = (1, 1, 1)
        float inputs:metallic_constant = 1
        float inputs:reflection_roughness_constant = 0.02
        asset inputs:diffuse_texture = @@
        asset inputs:metallic_texture = @@
        asset inputs:reflectionroughness_texture = @@
        asset inputs:normalmap_texture = @@
    }
}
```

* `metallic_constant 1` — a perfect conductor: the albedo becomes the *specular tint* and the
  diffuse term disappears, which is what a mirror is.
* `reflection_roughness_constant 0.02` — not 0, so the reflection has a hair of blur; 0 reads as a
  perfect razor mirror and can shimmer on thin geometry. `--roughness 0` for absolute sharpness,
  `0.05` if the mirror geometry is rougher than you liked.
* albedo `(1, 1, 1)` — no colour cast on the reflection (the stock texture is a brownish/greyish
  gradient, which would tint the reflection).
* the four `= @@` lines clear the game's albedo / metallic / roughness / **normal** maps: no normal
  map means no fake bumps on the glass, which is what makes the reflection read as flat glass.
* `AperturePBR_Opacity` (not `_Translucent`) — the mirror is opaque geometry; the pack's own
  `Mirror` material uses the same mdl.

## 4. Check it in game

1. Restart the game (the pack's USD is read at startup; `mod.usda` also has a "Reload Enhancements"
   button in the developer menu, which usually picks it up without a restart).
2. Look at the mirror: you should see the road/traffic behind the car.
3. If nothing changed: the hash is wrong or the pack wins — re-hover the mirror and compare the
   hash, then try `--position last`.
4. If the **windscreen or a window** turned into a mirror, you added `vehicleenvmap128` (it is also
   used for glass on some cars): remove that line, keep `xvehicleenv128`.
5. If the mirror looks like a *white* mirror rather than a reflection, the mdl override did not
   take: check that `rtx-remix/mods/gameReadyAssets/materials/AperturePBR_Opacity.mdl` exists.

Nothing here touches `rtx.conf`; the mirror textures must **not** be in `rtx.ignoreTextures`
(otherwise Remix never builds a material for them). Check with
`grep -n ignoreTextures <game>/rtx.conf` and look for your hash.

## 5. Rollback

```bash
python3 tools/mirrors.py --remove
# or by hand:
cp <game>/rtx-remix/mods/gameReadyAssets/mod.usda.bak-rhy-mirrors \
   <game>/rtx-remix/mods/gameReadyAssets/mod.usda
rm <game>/rtx-remix/mods/gameReadyAssets/rhy_mirrors.usda
```

## 6. Evidence behind the candidate list

Measured on this install (2026-11-11, details in `.strata/notes-s05-mirrors.md`):

* No stock vehicle model has a mirror atomic — the atomics of `admiral/sultan/stallion/banshee/
  cabbie/infernus/sabre/taxi/tractor/mower/bmx` are `*_dummy/_ok/_dam`, `chassis`, `windscreen_*`,
  `ug_*`, `wheel`. The mirror lives in the body atomic.
* No texture in the 3983 TXDs of `gta3.img`/`gta_int.img`/`player.img`/`cutscene.img` is named like
  a car mirror; the only `*mirror*` textures are interior props.
* Every car DFF's material list references the same generic set from `models/generic/vehicle.txd`;
  decoding that TXD shows `xvehicleenv128` (13 colours, 3-band) and `vehicleenvmap128`
  (sky→ground) are the two painted-reflection textures, while `vehicleshatter128` is the
  cracked-glass overlay (mean alpha 93) and the rest are tyre/steering/dash/lights/plates.
