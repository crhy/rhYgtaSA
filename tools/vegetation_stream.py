#!/usr/bin/env python3
"""n21: what a vegetation pack costs the streaming system — measured before any launch.

    usage: vegetation_stream.py GAME_DIR [--pack DIR] [--out-json FILE] [--top N]
                                     [--no-laa] [--quiet]

Why: docs/VEGETATION.md cause 2 is an argument about megabytes ("190.5 MB pinned by
KeepLoaded=1 in a 2 GB process"), and every number in it was produced by hand in
.strata/notes-s03-vegetation-hang.md. This is that measurement as a tool, so the same
question can be asked again of a different pack, a different streaming ini, or after LAA,
and so tests/test_vegetation_stream.py can hold it to its fixtures.

What it does, all read-only:

  1. reads the game's .img archives (models/*.img, VER2) -> name -> archived bytes;
  2. reads the pack folder (modloader-style loose files: <pack>/gta3.img/*.dff|*.txd,
     <pack>/models/**) -> the same names, which OVERRIDE the archive entries (that is how
     ModLoader works, and it is why the pack's cost is not the archive's cost);
  3. reads the streaming ini(s) (ImprovedStreaming.ini [RangeN] Start/End/Enabled/KeepLoaded,
     MixSets.ini StreamMemory, ImprovedStreaming.ini StreamMemoryForced /
     RemoveUnusedWhenPercent) and the exe's Large Address Aware bit (tools/laa.py);
  4. resolves every model id inside every enabled range through the game's IDE files
     (data/maps/generic/vegepart.ide, procobj.ide: `id, model, txd, ...`) and sums the
     unique .dff + unique .txd bytes, vanilla and with the pack applied.

Output: the pinned footprint (vanilla vs pack), the pack's whole-archive delta, the
effective streaming cap and the address space it sits in, and a verdict line. Nothing is
written unless --out-json names a file.
"""
import argparse
import glob
import json
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import laa as laa_mod  # noqa: E402   (tools/laa.py: the LAA bit reader)

IDE_FILES = ("data/maps/generic/vegepart.ide", "data/maps/generic/procobj.ide")
IMG_MAGIC = b"VER2"


def img_entries(path):
    """VER2 .img archive -> {name_lower: (name, bytes)}. Bytes = archived size (what streams)."""
    out = {}
    with open(path, "rb") as f:
        head = f.read(8)
        if head[:4] != IMG_MAGIC:
            return out
        n = struct.unpack("<I", head[4:8])[0]
        for _ in range(n):
            raw = f.read(32)
            if len(raw) < 32:
                break
            off, stream_sz, arch_sz, name = struct.unpack("<IHH24s", raw)
            nm = name.split(b"\0", 1)[0].decode("latin1")
            if not nm:
                continue
            size = (arch_sz or stream_sz) * 2048
            out[nm.lower()] = (nm, size)      # later duplicates win, same rule as img_unpack.py
    return out


def loose_files(root):
    """A modloader pack folder -> {name_lower: (name, bytes)} from its loose files.

    ModLoader reads `<mod>/gta3.img/foo.dff` and `<mod>/models/grass/foo.txd` as if they were
    entries of that archive, so the file name is the key and the folder structure only says
    which archive it belongs to.
    """
    out = {}
    for dirpath, _dirs, files in os.walk(root):
        for fn in files:
            p = os.path.join(dirpath, fn)
            try:
                size = os.path.getsize(p)
            except OSError:
                continue
            out[fn.lower()] = (fn, size)
    return out


def ide_rows(text):
    """IDE text -> [(id, model, txd)]. Lines are `id, name, txd, flags...`; comments and the
    header line are skipped, a line whose first field is not an integer is skipped."""
    rows = []
    for line in text.splitlines():
        line = line.split(";")[0].split("#")[0].strip()
        if not line:
            continue
        parts = [p.strip() for p in line.split(",")]
        if len(parts) < 3 or not re.fullmatch(r"\d+", parts[0]):
            continue
        rows.append((int(parts[0]), parts[1].lower(), parts[2].lower()))
    return rows


def ranges_from_ini(text):
    """ImprovedStreaming.ini -> [{name,start,end,enabled,keep_loaded}]. Section-scoped."""
    out, cur = [], None
    for line in text.splitlines():
        line = line.split(";")[0].split("#")[0].strip()
        if not line:
            continue
        m = re.fullmatch(r"\[(.*)\]", line)
        if m:
            cur = {"name": m.group(1).strip(), "start": None, "end": None,
                   "enabled": None, "keep_loaded": None}
            if cur["name"].lower().startswith("range"):
                out.append(cur)
            else:
                cur = None
            continue
        if cur is None or "=" not in line:
            continue
        k, v = [x.strip() for x in line.split("=", 1)]
        kl = k.lower()
        if kl.startswith("keep"):              # KeepLoaded
            kl = "keep_loaded"
        if kl in ("start", "end", "enabled", "keep_loaded"):
            try:
                cur[kl] = int(v)
            except ValueError:
                pass
    return out


def ini_key(text, key, section=None):
    """One ini key (case-insensitive), optionally inside [section]; None when absent."""
    cur, want = None, (section or "").lower()
    for line in text.splitlines():
        line = line.split(";")[0].split("#")[0].strip()
        if not line:
            continue
        m = re.fullmatch(r"\[(.*)\]", line)
        if m:
            cur = m.group(1).strip().lower()
            continue
        if want and cur != want:
            continue
        if "=" not in line:
            continue
        k, v = [x.strip() for x in line.split("=", 1)]
        if k.lower() == key.lower():
            return v
    return None


def resolve(names, vanilla, pack):
    """{name_lower} -> (bytes_vanilla, bytes_pack, source) with the pack overriding.

    IDE columns carry bare names (`aw_streettree1`, `lod_veg`); the archives and the pack
    folders carry extensions (`.dff`, `.txd`), so both spellings are tried.
    """
    tot_v = tot_p = 0
    missing, from_pack = [], []
    for n in names:
        key = n
        if "." not in n:
            # an IDE model column is a .dff, a txd column is a .txd; try the extension, then
            # the bare name (some IDEs already carry it)
            for suf in (".dff", ".txd"):
                if (n + suf) in vanilla or (n + suf) in pack:
                    key = n + suf
                    break
        v = vanilla.get(key)
        p = pack.get(key)
        if p:
            from_pack.append(key)
        vb = v[1] if v else 0
        pb = p[1] if p else vb
        if not v and not p:
            missing.append(key)
        tot_v += vb
        tot_p += pb
    return tot_v, tot_p, sorted(missing), sorted(from_pack)


def mb(x):
    return x / (1024.0 * 1024.0)


def main(argv=None):
    ap = argparse.ArgumentParser(prog="vegetation_stream.py", description=__doc__.splitlines()[0])
    ap.add_argument("game")
    ap.add_argument("--pack", default=None, help="pack folder to price (default: none = vanilla only)")
    ap.add_argument("--ide", action="append", default=None,
                    help="IDE file(s) to map ids to names (default: the game's vegepart+procobj)")
    ap.add_argument("--out-json", default=None)
    ap.add_argument("--top", type=int, default=8)
    ap.add_argument("--no-laa", action="store_true", help="pretend the exe is not LAA (worst case)")
    ap.add_argument("--quiet", action="store_true")
    a = ap.parse_args(argv)

    game = a.game.rstrip("/")
    vanilla = {}
    for img in sorted(glob.glob(os.path.join(game, "models", "*.img"))):
        vanilla.update(img_entries(img))
    for txd in glob.glob(os.path.join(game, "models", "**", "*.txd"), recursive=True):
        vanilla.setdefault(os.path.basename(txd).lower(),
                           (os.path.basename(txd), os.path.getsize(txd)))
    pack = loose_files(a.pack) if a.pack else {}

    # ---- the streaming config as it stands
    ini_paths = [p for p in (
        os.path.join(game, "modloader/Improved Streaming/ImprovedStreaming.ini"),
        os.path.join(game, "modloader/Remix_fix/ImprovedStreaming.ini"),
        os.path.join(game, "modloader/MixSets/MixSets.ini"),
        os.path.join(game, "modloader/Remix_fix/MixSets.ini"),
    ) if os.path.isfile(p)]
    streams, caps = [], []
    for p in ini_paths:
        text = open(p, encoding="utf-8", errors="replace").read()
        if "ImprovedStreaming" in p or "StreamMemoryForced" in text:
            for r in ranges_from_ini(text):
                r["ini"] = p
                streams.append(r)
            caps.append((p, "StreamMemoryForced", ini_key(text, "StreamMemoryForced")))
            caps.append((p, "RemoveUnusedWhenPercent", ini_key(text, "RemoveUnusedWhenPercent")))
        if "MixSets" in p:
            caps.append((p, "StreamMemory", ini_key(text, "StreamMemory")))

    # ---- id -> model/txd names, through the game's IDEs
    ide_paths = a.ide or [os.path.join(game, f) for f in IDE_FILES]
    rows = []
    for p in ide_paths:
        if os.path.isfile(p):
            rows.extend(ide_rows(open(p, encoding="utf-8", errors="replace").read()))
    by_id = {}
    for mid, model, txd in rows:
        by_id.setdefault(mid, []).append((model, txd))

    report = {"game": game, "pack": a.pack or "", "ranges": [], "caps": [],
              "pack_delta_mb": None, "pinned_vanilla_mb": 0.0, "pinned_pack_mb": 0.0}

    if not a.quiet:
        print("game : %s   (%d .img entries)" % (game, len(vanilla)))
        if a.pack:
            print("pack : %s   (%d loose files, %s MB)"
                  % (a.pack, len(pack), "%.1f" % mb(sum(s for _n, s in pack.values()))))
        print("streaming inis: %s" % (", ".join(os.path.relpath(p, game) for p in ini_paths) or "none"))
        for p, k, v in caps:
            print("   %-46s %s = %s" % (os.path.relpath(p, game), k, v if v is not None else "<absent>"))
    cap_vals = [int(v) for _p, k, v in caps if k in ("StreamMemory", "StreamMemoryForced") and v]
    cap = max(cap_vals) if cap_vals else None

    seen_ranges = set()
    for r in streams:
        if r["enabled"] == 0 or r["start"] is None or r["end"] is None:
            if not a.quiet:
                print("range %-8s %s-%s: disabled" % (r["name"], r["start"], r["end"]))
            continue
        dffs, txds = [], []
        for mid in range(r["start"], r["end"] + 1):
            for model, txd in by_id.get(mid, ()):
                dffs.append(model)
                if txd and txd != "-1":
                    txds.append(txd)
        key = (r["name"], tuple(sorted(set(dffs) | set(txds))))
        if key in seen_ranges:
            continue
        seen_ranges.add(key)
        dv, dp, miss, fromp = resolve(set(dffs), vanilla, pack)
        tv, tp, tmiss, tfromp = resolve(set(txds), vanilla, pack)
        ent = {"range": r["name"], "start": r["start"], "end": r["end"],
               "keep_loaded": r["keep_loaded"], "models": len(set(dffs)),
               "dicts": len(set(txds)), "dff_mb_vanilla": mb(dv), "dff_mb_pack": mb(dp),
               "txd_mb_vanilla": mb(tv), "txd_mb_pack": mb(tp),
               "total_mb_vanilla": mb(dv + tv), "total_mb_pack": mb(dp + tp),
               "missing": miss + tmiss, "replaced_by_pack": len(fromp) + len(tfromp)}
        report["ranges"].append(ent)
        if r["keep_loaded"] == 1:
            report["pinned_vanilla_mb"] += ent["total_mb_vanilla"]
            report["pinned_pack_mb"] += ent["total_mb_pack"]
        if not a.quiet:
            print("range %-8s %s-%s  keep_loaded=%s  %d models + %d dictionaries"
                  % (r["name"], r["start"], r["end"], r["keep_loaded"], ent["models"], ent["dicts"]))
            print("   vanilla %8.1f MB   pack %8.1f MB   (+%.1f MB)  replaced by pack: %d"
                  % (ent["total_mb_vanilla"], ent["total_mb_pack"],
                     ent["total_mb_pack"] - ent["total_mb_vanilla"], ent["replaced_by_pack"]))
            if miss + tmiss:
                print("   not found in any archive or the pack (%d): %s"
                      % (len(miss + tmiss), ", ".join((miss + tmiss)[:a.top])))

    # the pack's own cost, whole archive
    if pack:
        pv, pp, miss, fromp = resolve(set(pack), vanilla, pack)
        report["pack_total_mb_vanilla"] = mb(pv)
        report["pack_total_mb_pack"] = mb(pp)
        report["pack_replaces"] = len([n for n in pack if n in vanilla])
        report["pack_new"] = sorted(n for n in pack if n not in vanilla)
        if not a.quiet:
            print("pack total: %s MB on disk (%.1f MB of the names it replaces in the archives)"
                  % ("%.1f" % mb(pp), mb(pv)))
            print("   replaces %d archive entry(ies), adds %d new name(s)"
                  % (report["pack_replaces"], len(report["pack_new"])))

    # address space
    exe = os.path.join(game, "gta-sa.exe")
    is_laa = False
    if os.path.isfile(exe) and not a.no_laa:
        try:
            is_laa = laa_mod.Pe(open(exe, "rb").read()).laa
        except laa_mod.PeError:
            is_laa = False
    addr_mb = 4096 if is_laa else 2048
    report["laa"] = is_laa
    report["address_space_mb"] = addr_mb
    report["cap_mb"] = cap
    if not a.quiet:
        print("exe : %s Large Address Aware -> %d MB of address space for the whole process"
              % ("IS" if is_laa else "is NOT", addr_mb))
        print("pinned by KeepLoaded=1: %.1f MB vanilla -> %.1f MB with the pack"
              % (report["pinned_vanilla_mb"], report["pinned_pack_mb"]))
        if cap:
            pct = 100.0 * report["pinned_pack_mb"] / cap
            print("effective streaming cap: %d MB (the higher of StreamMemory/StreamMemoryForced;"
                  " unloading starts at RemoveUnusedWhenPercent)" % cap)
            print("VERDICT: pinned vegetation is %.1f%% of the cap, the cap is %.0f%% of the address space%s"
                  % (pct, 100.0 * cap / addr_mb,
                     "  -> the cap is the binding limit, not the pinned pack"
                     if cap <= addr_mb * 0.6 else "  -> the cap can outrun the address space"))
    if a.out_json:
        with open(a.out_json, "w") as f:
            json.dump(report, f, indent=1, sort_keys=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
