#!/usr/bin/env python3
r"""n18: census an RTX Remix asset pack - which materials have PBR maps, and what those maps say.

    usage: tools/pack_pbr_census.py PACK_DIR [--json OUT] [--list-prefix PREFIX]
                                      [--no-decode] [--sample N] [--top N]

Why this exists (docs/TARGET.md §4 T1, task n18): the GTASA-Remix pack defines **2334**
`mat_<HASH>` materials and ships maps for ~300 of them (345 `_albedo`, 315 `_normal`, 280 `_rough`,
185 `_height`, **32 `_metal`**). Everything else is drawn with the runtime's *default legacy BRDF*:
one constant roughness (0.7), one constant metallic (0.1), the game's own colour texture, and by
`src/dxvk/rtx_render/rtx_materials.cpp` (tag remix-1.6.1, `LegacyMaterialData::as()`) nothing else -
a legacy material cannot carry a roughness or normal map at all. So the first question of the
materials task is not "which knob do we turn" but **what does the pack actually cover, and what
roughness do its own maps carry** - the second half is what turns a shipped constant from a guess
into a measurement.

What it reports (everything read-only; nothing is written except --json / --list-prefix):

  census     files, .dds, .usda/.usd, unique `mat_<HASH>` names in the USD layers (crates are
             scanned as bytes - most of the pack's mesh layers are binary .usd).
  coverage   a material is *covered* when the pack holds `<HASH>_albedo.dds` for its `mat_<HASH>`:
             the pack's own convention (`over "mat_1CB126F1AE5A3A08"` binds
             `@./1CB126F1AE5A3A08_albedo.dds@`, see texture/pavement/Pavement_materials.usda).
             Covered / uncovered counts overall and per category.
  maps       per-category counts of albedo / normal / rough / height / metal / emissive.
  roughness  decoded mip 0 of every `_rough` map (Pillow's DDS plugin reads BC4/BC5/BC1/BC7):
             per-map mean / median / p05 / p95, per category, and the glossiest and mattest maps.
             A `_rough` map is raw (non-colour) data, so texel/255 IS the perceptual roughness.
             The same numbers are printed for `_metal`.
  height     decoded mip 0 of every `_height` map: the relief it actually carries (p95-p05 in
             8-bit units) and how many maps are effectively flat - the number that decides whether
             "185 height maps" means 185 materials with micro-relief.
  layers     `subLayers` entries that do not resolve byte-exactly on this filesystem but do resolve
             case-insensitively. The pack was authored on Windows (`@./texture/Asphalt/...@`
             against an on-disk `texture/asphalt/`) and the runtime resolves it case-insensitively
             under Proton (notes-n20: the log loads `...\texture\Wall\*.dds` from a directory named
             `wall`), so this is NOT a bug to fix - it is a trap for our own offline tools, which
             have to resolve the same way, and it is why this tool lowercases categories.
  starting   the uncovered `mat_<HASH>` hashes with the number of USD layers referencing each,
             written to `<list-prefix>-uncovered.txt` (covered ones to `-covered.txt`): the input
             list for the offline PBR-ify pass.

Exit 0 on success, 2 on bad usage. Decoding every map of a 10 GB pack takes a couple of minutes;
--no-decode gives the structural census in a second, and --sample N strides the decoded pixels
(default 8: every 8th row and column, still ~65 k texels for a 2048² map).
"""
import argparse
import json
import os
import re
import sys
from collections import Counter, defaultdict

MATRE = re.compile(rb"mat_([0-9A-Fa-f]{16})")
HEX16 = re.compile(rb"[0-9A-Fa-f]{16}")
SUBLAYER = re.compile(r"subLayers\s*=\s*\[(.*?)\]", re.S)
ASSETREF = re.compile(r"@([^@\n]+)@")
SUFFIXES = ("albedo", "normal", "rough", "height", "metal", "emissive", "opacity", "orm")
# suffixes the pack also spells out in full or borrows from the source library
ALT = {"roughness": "rough", "normaldx": "normal", "nor_dx": "normal", "displacement": "height",
       "color": "albedo", "diff": "albedo", "basecolor": "albedo", "diffuse": "albedo"}


def walk(root):
    for dirpath, dirnames, filenames in os.walk(root):
        for fn in filenames:
            yield os.path.join(dirpath, fn)


def stem_of(path):
    base = os.path.basename(path)
    return base[:-4] if base.lower().endswith(".dds") else base


def suffix_of(path):
    """'rough' for 07A2BD3EF6FA6ABE_rough.dds / rail_roughness.dds; None if unrecognised."""
    low = stem_of(path).lower()
    for s in SUFFIXES:
        if low.endswith("_" + s):
            return s
    for a, s in ALT.items():
        if low.endswith("_" + a):
            return s
    return None


def hash_of(path):
    """The 16-hex texture hash a map belongs to, or None for a hand-named map (rail_base_albedo)."""
    stem = stem_of(path)
    low = stem.lower()
    for s in list(SUFFIXES) + list(ALT):
        if low.endswith("_" + s):
            stem = stem[: -(len(s) + 1)]
            break
    tok = stem.rsplit("_", 1)[-1]
    return tok.upper() if re.fullmatch(r"[0-9A-Fa-f]{16}", tok) else None


def category_of(path, root):
    rel = os.path.relpath(path, root).replace("\\", "/")
    parts = rel.split("/")
    if len(parts) >= 2 and parts[0] == "texture":
        return parts[1].lower()
    return parts[0].lower()


def decode_stats(path, sample):
    """Strided mip-0 statistics of a single-channel map, or None if it will not decode."""
    try:
        import numpy as np
        from PIL import Image
        with Image.open(path) as im:
            arr = np.asarray(im)
    except Exception:
        return None
    if arr.ndim == 3:                       # RGB(A): the maps we care about carry one channel
        arr = arr[..., 0]
    if sample > 1:
        arr = arr[::sample, ::sample]
    a = arr.reshape(-1).astype(np.float64)
    if a.size == 0:
        return None
    q = np.percentile(a, [5, 50, 95])
    return dict(n=int(a.size), mean=float(a.mean()), median=float(q[1]), p05=float(q[0]),
                p95=float(q[2]), min=float(a.min()), max=float(a.max()))


def pct(vals, p):
    if not vals:
        return None
    v = sorted(vals)
    i = min(len(v) - 1, max(0, int(round(p / 100.0 * (len(v) - 1)))))
    return v[i]


def resolve_case_insensitive(start, target):
    """Resolve `target` from `start` matching every path component case-insensitively.

    Returns the resolved path when it exists, else None. This is what the runtime does under
    Proton, and what our own tools must do to follow a pack's USD references at all.
    """
    rel = os.path.relpath(target, start)
    cur = start
    for part in rel.split(os.sep):
        if os.path.exists(os.path.join(cur, part)):
            cur = os.path.join(cur, part)
            continue
        try:
            ents = os.listdir(cur)
        except OSError:
            return None
        hit = next((e for e in ents if e.lower() == part.lower()), None)
        if hit is None:
            return None
        cur = os.path.join(cur, hit)
    return cur if os.path.exists(cur) else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("pack", help="the pack root, e.g. <game>/rtx-remix/mods/gameReadyAssets")
    ap.add_argument("--json", help="write the whole report here (JSON)")
    ap.add_argument("--list-prefix", help="write <prefix>-covered.txt / <prefix>-uncovered.txt")
    ap.add_argument("--no-decode", action="store_true", help="skip the DDS decode pass")
    ap.add_argument("--sample", type=int, default=8, help="pixel stride for the decode pass")
    ap.add_argument("--top", type=int, default=15, help="rows per table")
    a = ap.parse_args()

    root = a.pack
    if not os.path.isdir(root):
        print("not a directory: %s" % root, file=sys.stderr)
        return 2

    allfiles = list(walk(root))
    dds = sorted(p for p in allfiles if p.lower().endswith(".dds"))
    usds = sorted(p for p in allfiles if p.lower().endswith((".usda", ".usd")))

    # --- material names, from the USD layers (crates scanned as bytes) ------------------------
    # n15 counted these with `grep -rhoE 'mat_[0-9A-Fa-f]{16}'`, which SKIPS binary crates (grep
    # prints "binary file matches" instead of the matches without -a): the real number is 2363, not
    # 2334. Reading the bytes here is the same scan with that bug removed.
    mat_refs = Counter()          # HASH -> number of USD layers that mention it
    mat_layers = defaultdict(set)
    usd_tokens = set()            # every 16-hex token anywhere in the USD layers
    for p in usds:
        try:
            with open(p, "rb") as fh:
                blob = fh.read()
        except OSError:
            continue
        for m in set(MATRE.findall(blob)):
            mat_refs[m.decode().upper()] += 1
            mat_layers[m.decode().upper()].add(p)
        usd_tokens.update(t.decode().upper() for t in HEX16.findall(blob))

    # --- maps on disk -------------------------------------------------------------------------
    by_hash = defaultdict(dict)   # HASH -> {suffix: path}
    percat = defaultdict(Counter)
    odd = []
    for p in dds:
        sfx, h = suffix_of(p), hash_of(p)
        if sfx:
            percat[category_of(p, root)][sfx] += 1
        if h:
            by_hash[h][sfx or "unnamed"] = p
        else:
            odd.append(p)

    # A material is covered when the pack holds `<HASH>_albedo.dds` under the SAME hash as the
    # `mat_<HASH>` prim. That is the pack's convention (texture/pavement/Pavement_materials.usda:
    # `over "mat_1CB126F1AE5A3A08"` binds `@./1CB126F1AE5A3A08_albedo.dds@`) but not universal -
    # texture/buildings/buildings_materials.usda binds 40 maps that way and 2 to a different hash -
    # so the honest coverage number is "maps keyed by hash" vs "materials", both printed below.
    covered = sorted(h for h in mat_refs if "albedo" in by_hash.get(h, {}))
    uncovered = sorted(h for h in mat_refs if h not in covered)
    map_hashes = sorted(by_hash)
    map_albedo = sorted(h for h in map_hashes if "albedo" in by_hash[h])
    map_orphan = sorted(h for h in map_hashes if h not in usd_tokens)
    mat_cat = {h: ",".join(sorted({category_of(l, root) for l in mat_layers[h]})) or "?"
               for h in mat_refs}
    cov_by_cat, unc_by_cat = Counter(), Counter()
    for h in mat_refs:
        (cov_by_cat if h in covered else unc_by_cat)[mat_cat[h]] += 1

    # --- decode pass --------------------------------------------------------------------------
    rough_stats, metal_stats, height_stats = [], [], []
    rough_by_cat, metal_by_cat = defaultdict(list), defaultdict(list)
    if not a.no_decode:
        for kind, store, catstore in (("rough", rough_stats, rough_by_cat),
                                      ("metal", metal_stats, metal_by_cat)):
            for p in dds:
                if suffix_of(p) != kind:
                    continue
                st = decode_stats(p, a.sample)
                if st:
                    st["path"] = os.path.relpath(p, root)
                    st["category"] = category_of(p, root)
                    store.append(st)
                    catstore[st["category"]].append(st["mean"] / 255.0)
        for p in dds:
            if suffix_of(p) != "height":
                continue
            st = decode_stats(p, a.sample)
            if st:
                st["path"] = os.path.relpath(p, root)
                st["category"] = category_of(p, root)
                height_stats.append(st)

    # --- layer resolution (the case trap) ------------------------------------------------------
    case_mismatch, missing = [], []
    for p in usds:
        if not p.lower().endswith(".usda"):
            continue
        try:
            text = open(p, "r", errors="replace").read()
        except OSError:
            continue
        for m in SUBLAYER.finditer(text):
            for ref in ASSETREF.findall(m.group(1)):
                tgt = os.path.normpath(os.path.join(os.path.dirname(p), ref))
                if os.path.exists(tgt):
                    continue
                real = resolve_case_insensitive(root, tgt)
                (case_mismatch if real else missing).append(
                    (os.path.relpath(p, root), ref,
                     os.path.relpath(real, root) if real else None))

    # --- report -------------------------------------------------------------------------------
    print("pack: %s" % root)
    print("files %d, dds %d, usda/usd %d, unique mat_<HASH> %d"
          % (len(allfiles), len(dds), len(usds), len(mat_refs)))
    print("maps by suffix: %s" % ", ".join(
        "%s %d" % (s, sum(c[s] for c in percat.values())) for s in SUFFIXES))
    print("  of those, keyed by a <HASH> (i.e. bound to a game texture): %s"
          % ", ".join("%s %d" % (s, sum(1 for h in by_hash if s in by_hash[h]))
                      for s in SUFFIXES))
    print("  bare <HASH>.dds copies (the pack's own un-remastered textures): %d"
          % sum(1 for h in by_hash if set(by_hash[h]) == {"unnamed"}))
    print("hand-named maps (no <HASH>_<suffix>): %d" % len(odd))
    print("map hashes on disk: %d (%d with an albedo map, %d never referenced by any USD layer)"
          % (len(map_hashes), len(map_albedo), len(map_orphan)))
    print("coverage: %d of the %d mat_<HASH> names carry their own <HASH>_albedo.dds, %d do not"
          % (len(covered), len(mat_refs), len(uncovered)))
    print("          -> the pack remasters ~%d game textures out of ~%d materials (%.1f %%); the "
          "rest are legacy materials" % (len(map_albedo), len(mat_refs),
                                         100.0 * len(map_albedo) / max(1, len(mat_refs))))

    print("\ncategory (of the defining layer)          covered  uncovered")
    for c, n in unc_by_cat.most_common(a.top):
        print("  %-38s %7d %9d" % (c, cov_by_cat.get(c, 0), n))

    print("\ncategory                 albedo normal  rough height  metal")
    for c in sorted(percat, key=lambda c: -percat[c]["albedo"]):
        n = percat[c]
        print("  %-24s %6d %6d %6d %6d %5d"
              % (c, n["albedo"], n["normal"], n["rough"], n["height"], n["metal"]))

    if rough_stats:
        means = [s["mean"] / 255.0 for s in rough_stats]
        meds = [s["median"] / 255.0 for s in rough_stats]
        spread = [(s["p95"] - s["p05"]) / 255.0 for s in rough_stats]
        print("\n_rough maps decoded: %d  (texel/255 = perceptual roughness)" % len(rough_stats))
        print("  per-map mean:    min %.3f  p05 %.3f  median %.3f  p95 %.3f  max %.3f"
              % (min(means), pct(means, 5), pct(means, 50), pct(means, 95), max(means)))
        print("  per-map median:  min %.3f  median %.3f  max %.3f"
              % (min(meds), pct(meds, 50), max(meds)))
        print("  variation inside one map (p95-p05): median %.3f  p90 %.3f"
              % (pct(spread, 50), pct(spread, 90)))
        print("  per category (mean of the per-map means):")
        for c in sorted(rough_by_cat, key=lambda c: -len(rough_by_cat[c])):
            v = rough_by_cat[c]
            print("    %-22s n=%-4d mean %.3f  median %.3f  min %.3f  max %.3f"
                  % (c, len(v), sum(v) / len(v), pct(v, 50), min(v), max(v)))
        print("  glossiest 5: %s" % ", ".join(
            "%s %.2f" % (os.path.basename(s["path"]), s["mean"] / 255)
            for s in sorted(rough_stats, key=lambda s: s["mean"])[:5]))
        print("  mattest  5: %s" % ", ".join(
            "%s %.2f" % (os.path.basename(s["path"]), s["mean"] / 255)
            for s in sorted(rough_stats, key=lambda s: -s["mean"])[:5]))
    if metal_stats:
        means = [s["mean"] / 255.0 for s in metal_stats]
        print("\n_metal maps decoded: %d  per-map mean: min %.3f  median %.3f  max %.3f"
              % (len(metal_stats), min(means), pct(means, 50), max(means)))
    if height_stats:
        amp = [s["p95"] - s["p05"] for s in height_stats]
        print("\n_height maps decoded: %d  amplitude (p95-p05, 8-bit units): median %.0f  p05 %.0f  "
              "max %.0f; %d maps effectively flat (<8)"
              % (len(height_stats), pct(amp, 50), pct(amp, 5), max(amp),
                 sum(1 for x in amp if x < 8)))

    if case_mismatch:
        print("\nsubLayers that resolve only case-insensitively: %d (harmless for the runtime, "
              "a trap for offline tools)" % len(case_mismatch))
        for p, ref, real in case_mismatch[:a.top]:
            print("  %s -> %s   (on disk: %s)" % (p, ref, real))
    if missing:
        print("subLayers that do not resolve at all: %d" % len(missing))
        for p, ref, _ in missing[:a.top]:
            print("  %s -> %s" % (p, ref))

    if a.list_prefix:
        with open(a.list_prefix + "-covered.txt", "w") as fh:
            fh.write("\n".join(covered) + ("\n" if covered else ""))
        with open(a.list_prefix + "-uncovered.txt", "w") as fh:
            for h in sorted(uncovered, key=lambda h: -mat_refs[h]):
                fh.write("%s\t%d\t%s\n" % (h, mat_refs[h], mat_cat[h]))
        print("\nwrote %s-covered.txt (%d) and %s-uncovered.txt (%d: hash, number of USD layers "
              "referencing it, category of that layer)" % (a.list_prefix, len(covered),
                                                           a.list_prefix, len(uncovered)))

    if a.json:
        os.makedirs(os.path.dirname(a.json) or ".", exist_ok=True)
        means = [s["mean"] / 255.0 for s in rough_stats]
        with open(a.json, "w") as fh:
            json.dump(dict(
                pack=os.path.abspath(root), sample=a.sample, decode=not a.no_decode,
                files=len(allfiles), dds=len(dds), usd=len(usds), materials=len(mat_refs),
                maps={s: sum(c[s] for c in percat.values()) for s in SUFFIXES},
                hand_named=len(odd), covered=len(covered), uncovered=len(uncovered),
                map_hashes=len(map_hashes), map_hashes_with_albedo=len(map_albedo),
                map_hashes_orphan=len(map_orphan), usd_hex_tokens=len(usd_tokens),
                maps_by_hash={s: sum(1 for h in by_hash if s in by_hash[h]) for s in SUFFIXES},
                bare_copies=sum(1 for h in by_hash if set(by_hash[h]) == {"unnamed"}),
                per_category_maps={c: dict(percat[c]) for c in percat},
                uncovered_by_category=dict(unc_by_cat), covered_by_category=dict(cov_by_cat),
                rough=dict(n=len(rough_stats), mean_median=pct(means, 50),
                           mean_min=min(means) if means else None,
                           mean_max=max(means) if means else None,
                           p05=pct(means, 5), p95=pct(means, 95),
                           per_category={c: dict(n=len(v), mean=sum(v) / len(v),
                                                 median=pct(v, 50))
                                         for c, v in rough_by_cat.items()},
                           maps=rough_stats),
                metal=dict(n=len(metal_stats),
                           median=pct([s["mean"] / 255 for s in metal_stats], 50)),
                height=dict(n=len(height_stats),
                            flat=sum(1 for s in height_stats if (s["p95"] - s["p05"]) < 8)),
                case_mismatch=len(case_mismatch), missing_layers=len(missing),
            ), fh, indent=1)
        print("json: %s" % a.json)
    return 0


if __name__ == "__main__":
    sys.exit(main())
