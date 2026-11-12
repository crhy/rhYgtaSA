# Rear-view mirrors that really reflect (RTX Remix)

GTA SA does not have mirrors, it has **paint**: the mirror glass is a handful of triangles on the
car's door atomic, textured with the same atlases the rest of the car uses. Under RTX Remix every
surface is path traced, so nothing about the geometry has to change — the surface just needs a
perfectly smooth metallic material and Remix reflects what is really behind it (behind CJ: the road,
the traffic, the cop car you finally notice).

**What s21 measured, and what it means for this document.** The obvious lever — override the Remix
material of the texture the mirror samples — does not work on stock cars, because no GTA SA texture
is used by mirror glass alone (`config/mirror-hashes.txt` is therefore empty and
`tools/mirrors.py` currently refuses to write anything):

| texture | slot in the material | models | materials | triangles | also used by |
|---|---|---|---|---|---|
| `xvehicleenv128` `0x45DF1EA60BB202B5` | env / reflection | 179 | 4197 | 205 828 | body, all doors, bonnet, boot, bumpers, windscreens, bikes, boats |
| `vehicleenvmap128` `0x6BAFC5F6AEE0A6B5` | env / reflection | 81 | 495 | 19 589 | body, doors, chassis underside, forks, handlebars, boats |
| `vehiclelights128` `0x5AD92DBA0F2D1BCA` | **diffuse (the mirror's own texture)** | 163 | 1146 | 11 779 | every head/tail/indicator lens |
| `vehiclegeneric256` `0xEA8646F3A1CDD659` | diffuse (some cars) | 198 | 4737 | 219 419 | the whole generic body paint |

(209 stock vehicle `.dff` of `models/gta3.img`, i.e. every model named in `data/vehicles.ide`; the
50 `.dff` under `modloader/` are SARemix props. Method and full tables:
`.strata/notes-s21-mirror-hash-check.md`.)

So sections 1–5 below are the **texture-hash route, kept as the reference for a texture that really
is mirror-only** (a modded/re-authored mesh can have one). For the stock cars the route that works is
the **mesh override of section 7** — which is also what NVIDIA's pack does for its own re-authored
taxi/police mirrors: `def Material "Mirror"` in
`rtx-remix/mods/gameReadyAssets/meshes/Vehicles.usda` is `AperturePBR_Opacity` with
`metallic_constant 1`, `reflection_roughness_constant 0`, albedo `(1, 0.99999, 0.99999)`, bound to
that mesh's mirror prims, not to a game texture.

`tools/mirrors.py` writes the texture-hash part:
`<game>/rtx-remix/mods/gameReadyAssets/rhy_mirrors.usda`, one
`over "RootNode" { over "Looks" { over "mat_<HASH>" { over "Shader" { ... } } } }` per **verified**
texture hash, sublayered into `mod.usda`.

---

## 1. Get the hashes — offline (`tools/remix_texhash.py`, s18)

Remix's texture hash is a plain xxHash over the pixels the game uploads, so it **can** be computed
from the `.txd` files, no running game needed:

```bash
python3 tools/remix_texhash.py /media/rhy/data/Gamez/GTASA-RTX/models/generic/vehicle.txd xvehicleenv128
# xvehicleenv128  128x128 A8R8G8B8 mip0=65536 bytes  xxh3=45DF1EA60BB202B5  xxh64=C69E8B0981F09694
```

It hashes exactly what Remix hashes — `XXH3_64bits` (xxHash 0.8.0, the copy bundled in dxvk-remix)
over **mip 0** of the texture, nothing else — and it reads both loose `.txd` files and the `.txd`
entries inside a `.img` archive. Proof that the algorithm is right: it reproduces **55 of the 167
hashes in the live `rtx.conf`** (`rtx.particleTextures`, `rtx.decalTextures`, `rtx.uiTextures`, …)
and **164 of the 207 hash-named replacement textures** that NVIDIA's own pack ships, e.g.
`vehicledash32` → `texture/Vehicles/CBFFF905D20D2591_albedo.dds`. Derivation, code references and
the full table: `.strata/notes-s18-remix-texture-hash.md`. Omit the name for a whole dictionary; `--conf <game>/rtx.conf` marks every hash that
appears in Remix's texture lists and `--only-matches` prints only those (the comfortable way to scan
all of `models/`); `--hash 0x…` asks "which texture in this file has this hash?"; and a directory or
`.img` path searches everything under it.

Both hashes in that example are **not** mirror-only (see the table at the top) and are *not* in
`config/mirror-hashes.txt`; the command is only a demonstration of the tool. What the scan says
about them:

| texture | what it really is |
|---|---|
| `xvehicleenv128` — hard 3-band strip (brown / light blue-grey / brown), 13 colours | the env/reflection slot of 4197 materials on 179 of 209 cars: body, doors, bonnet, boot, bumpers, windscreens |
| `vehicleenvmap128` — smooth sky-to-ground gradient, 256 colours | the same slot on the 81 cars that use it instead |

The pack's `SARemix_OriginalVehiclesPBR.usda` overrides `mat_45DF1EA60BB202B5` with translucent glass
(`ior_constant 1.6`, `thin_walled 1`) — which is a hint that Remix does key a material on its env
slot too, i.e. exactly the surface you do **not** want to re-materialise as a mirror.

### When you do have to read a hash in game

Textures the game **generates or writes into** instead of loading as a raster — the sea, the skybox,
some UI — hash over the generated pixels, so no offline tool can know them (that is what the other
112 of rtx.conf's 167 hashes are). For those:

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
   a thumbnail of every texture the game uploaded with its size, so look for **128 x 128** and the
   two thumbnails in the table above. Hovering a thumbnail also shows its hash, and middle-click
   copies it.
5. Paste the hash into `config/mirror-hashes.txt` — one `0x… + 16 hex digits` per line, and the line
   **must** carry the marker `# verified-mirror-only` plus a label, e.g.
   `0x1234ABCD5678EF90   # verified-mirror-only  re-authored taxi mirror glass`. `tools/mirrors.py`
   refuses every hash whose line lacks that marker, because `mat_<HASH>` re-materialises *every*
   material sampling the texture. Before adding the marker, check the texture really is mirror-only:
   the grid's thumbnail shows the whole texture, and if it is an atlas of lights / tyres / body
   panels it is not. `remix_texhash.py --hash` tells you which `.txd` texture a hash belongs to.

Do **not** add these; s21 measured what they cover, and every one of them is more than a mirror:
`xvehicleenv128` (env slot of 4197 materials on 179 cars — the whole painted body), `vehicleenvmap128`
(495 materials on 81 cars), `vehiclelights128` (the mirror's own diffuse **and** every light lens,
1146 materials on 163 cars), `vehiclegeneric256` (the generic body paint, 4737 materials),
`vehicleshatter128` (cracked-window overlay), `vehiclegrunge256` (dirt), `vehiclescratch64`,
`vehiclespecdot64`, `vehicletyres128`, `vehiclesteering128`, `vehicledash32`,
`vehiclelightson128`, `vehiclepoldecals128`, `carplate`, `carpback`, `plateback1..3`,
`platecharset`, and the per-car `<car>92interior128` / `<car>92wheel*` / `remap<car>body256`.

## 2. Run the tool

```bash
cd ~/rhYgtaSA
python3 tools/mirrors.py                      # GAME_DIR defaults to /media/rhy/data/Gamez/GTASA-RTX
python3 tools/mirrors.py --dry-run --print    # show the USD without touching the game
python3 tools/mirrors.py --hash 0xDEADBEEFDEADBEEF   # one-off, without editing the config
python3 tools/mirrors.py --remove             # undo
```

What it does, in order:

* reads the hashes from `config/mirror-hashes.txt` (plus any `--hash`) — **only lines marked
  `# verified-mirror-only` count**, everything else is printed as `refused:` and skipped — and stops
  with an explanation when nothing is left (which is the state of the shipped config: no stock
  texture is mirror-only, so use section 7);
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
4. If the **windscreen, a headlight or a whole door** turned into a mirror, you used a shared
   texture — that is what `xvehicleenv128` / `vehicleenvmap128` / `vehiclelights128` are. Remove the
   line and use section 7 instead.
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

Measured on this install (2026-11-11, details in `.strata/notes-s05-mirrors.md`; re-measured with a
real RenderWare parser in s21, `.strata/notes-s21-mirror-hash-check.md`):

* No stock vehicle model has a mirror atomic **or a mirror frame**: across the 209 stock `.dff`
  (402 distinct frame names) not one frame matches `*mirror*`, `*chrome*`, `*glass*` or `*window*`.
  `wing_lf_ok/_dam/_dummy` etc. are SA's *fenders*, `windscreen_*` the glass, `ug_*` the under-garage
  extras. The mirror lives inside the front-door atomic.
* Locating it geometrically (car-space transform chain, then "small cluster that sticks out past the
  door skin and sits ahead of the door centre") finds it on 64 of the 209 models: 4–8 triangles,
  ≈ 0.04 m², |x| 1.05–1.3 m, z 0.2–0.6 m, just ahead of the front door. Its textures:
  diffuse `vehiclelights128` (76 of 86 clusters), env `xvehicleenv128` (36) / `vehicleenvmap128` (23)
  / none (27), spec `vehiclespecdot64` (62).
* Every car DFF's material list references the same generic set from `models/generic/vehicle.txd`;
  decoding that TXD shows `xvehicleenv128` (13 colours, 3-band) and `vehicleenvmap128` (sky→ground)
  are the two painted-reflection textures — but they sit in the **env slot of the whole car**, not in
  a mirror material, and neither is ever a diffuse texture. `vehicleshatter128` is the cracked-glass
  overlay (mean alpha 93), the rest are tyre/steering/dash/lights/plates.
* `modloader/` ships 50 `.dff`, all SARemix props (`remix_cloud*`, `remix_sun`, `LOD_cone_*`,
  `remix_tl_*`, …): no vehicle mesh, and none of them uses either env texture.

## 7. What to do instead: override the mirror **mesh** (s21's recommendation)

Since no texture isolates the mirror, the override has to be bound to geometry. Remix keys meshes the
same way it keys textures: the prim is named `mesh_<MESHHASH>` under the model's `RootNode`, and an
`over` block on that prim replaces its material — which is exactly how the pack binds its own
`def Material "Mirror"` to the mirror prims of the re-authored taxi/police meshes in
`meshes/Vehicles.usda`.

Recipe:

1. **Find the mesh hash in game** (mesh hashes are not derivable offline — they hash the geometry,
   not a raster): start the game with Remix, stand outside a car and look at the door mirror, open
   the developer menu (Alt+X), hover the mirror, and the *Mesh/Prim Info* popup gives the hash; the
   same objects appear in the menu's mesh grid, where middle-click copies it.
2. **Write the override** — same shader block as section 3, but bound to the mesh prim instead of
   `mat_<HASH>`:

   ```usda
   over "RootNode"
   {
       over "Looks"
       {
           over "mesh_<MESHHASH>"
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
       }
   }
   ```

   Put it in a layer of its own and sublayer it into `mod.usda` **first** (USD composes strongest
   first), the way `tools/mirrors.py` does for the texture layer; `--remove`-style rollback is the
   same two files.
3. **Which prims to pick** (measured, so the hover is not guesswork): the mirror is a prim of 4–8
   triangles, ≈ 0.04 m², bounding box ≈ 0.25 × 0.11 × 0.17 m, centred at |x| 1.05–1.3 m, ~0.1–0.6 m
   ahead of the front-door centre, z 0.2–0.6 m above the car origin. On `admiral` it is the 4-triangle
   cluster at (∓1.15, 0.62, 0.27) on the `door_lf_ok` / `door_rf_ok` atomics.
4. **Scale it up**: the same cluster exists on the 64 models listed in the notes; per-model mesh
   hashes have to be read in game (one hover per model), so start with the cars you actually drive —
   or with the pack's re-authored taxi/police meshes, which already have the `Mirror` material.

Not automated here on purpose: `tools/mirrors.py` only handles texture-keyed overrides, and there is
nothing verified to write. If a mesh-hash round in game proves the recipe, the honest next step is a
small sibling tool that takes `mesh_<HASH>` lines from a config file and emits the layer — same
validation, same sublayer logic, marker rule replaced by "read the hash in game and check the
thumbnail".
