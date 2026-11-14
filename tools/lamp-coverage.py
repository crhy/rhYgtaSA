#!/usr/bin/env python3
"""How much of the game's own lamp data our lamp list actually covers.

usage: lamp-coverage.py --list build/rhyremix_lights.txt
                        (--inventory build/rhyremix_2dfx.txt | --game <GAME_DIR>)
                        [--region LA] [--tol 1.0] [--min 95] [--density 300,380,120]

WHAT IT COUNTS
--------------
The ground truth is the 2dFx inventory: every LIGHT record of every .dff in
<game>/models/gta3.img, at every place an IPL puts that model (tools/lamp_list.py
--inventory writes it; --game builds it here). Those are the bulbs the GAME
itself says exist. The list is what rhyremix.asi lights. A 2dFx light counts as
covered when the list has a light within --tol game units of it (3D).

Traffic-signal models (lamp_list.TRAFFIC_NAMES) are reported separately, not as
a street-list gap: trafficlights.cpp and signalheads.cpp already put a bulb on
every one of those lenses from the signal list, and a second light on the same
lens would only burn a slot.

Coverage is reported per region (LA = Los Santos, the region the >= 95% target is
set for) and per model, so a missing model is a line you can act on, not a
number. Exit status is non-zero when the target region is below --min.
"""
import argparse
import collections
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lamp_list  # noqa: E402


def read_inventory(path):
    """The inventory file lamp_list.py --inventory writes."""
    out = []
    with open(path, "r", encoding="latin1") as fh:
        for raw in fh:
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            p = line.split()
            if len(p) < 9:
                continue
            out.append({"model": p[0], "region": p[1],
                        "x": float(p[2]), "y": float(p[3]), "z": float(p[4]),
                        "r": int(p[5]), "g": int(p[6]), "b": int(p[7]),
                        "range": float(p[8])})
    return out


def read_list(path):
    """The plugin's light list, v1 ("type x y z") or v2 ("type x y z r g b family range")."""
    pts = []
    with open(path, "r", encoding="latin1") as fh:
        for raw in fh:
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            p = line.split()
            if len(p) < 4:
                continue
            try:
                pts.append((float(p[1]), float(p[2]), float(p[3])))
            except ValueError:
                continue
    return pts


class Grid:
    """A 1-unit grid over the list, so coverage is O(1) per light instead of O(n)."""

    def __init__(self, pts, cell=2.0):
        self.cell = cell
        self.g = {}
        for x, y, z in pts:
            self.g.setdefault((int(x // cell), int(y // cell), int(z // cell)),
                              []).append((x, y, z))

    def near(self, p, tol):
        c = self.cell
        cx, cy, cz = (int(p[0] // c), int(p[1] // c), int(p[2] // c))
        span = int(tol // c) + 1
        if span <= 3:
            # The normal case (tol is about a lamp head, the cell is 2 units): walk
            # the 7x7x7 block of cells around the query.
            cells = [self.g.get((gx, gy, gz))
                     for gx in range(cx - span, cx + span + 1)
                     for gy in range(cy - span, cy + span + 1)
                     for gz in range(cz - span, cz + span + 1)]
            cells = [v for v in cells if v]
        else:
            # A tolerance many cells wide: walking the cube of cell coordinates
            # would be span^3 lookups (a 1000-unit tolerance is a billion), so walk
            # the cells that exist instead. This path is only for experiments.
            cells = [v for k, v in self.g.items()
                     if abs(k[0] - cx) <= span and abs(k[1] - cy) <= span
                     and abs(k[2] - cz) <= span]
        best = None
        for q in cells:
            for pt in q:
                d = (pt[0] - p[0]) ** 2 + (pt[1] - p[1]) ** 2 + (pt[2] - p[2]) ** 2
                if d <= tol * tol and (best is None or d < best):
                    best = d
        return best is not None


def density(pts, radii, sample=8):
    """Worst case number of list lights within each radius of a list light.

    This is the number that decides max_lights / head_glow_max: a cap below the
    worst case inside the radius evicts lamps the player is standing under, which
    is finding A (n04). Sampling every `sample`th light is what the plugin itself
    does at load, so the two numbers are comparable.
    """
    out = {}
    cell = 64.0
    grid = {}
    for x, y, _z in pts:
        grid.setdefault((int(x // cell), int(y // cell)), []).append((x, y))
    for r in radii:
        worst = 0
        worst_at = None
        for i in range(0, len(pts), sample):
            p = pts[i]
            n = 0
            cx, cy = int(p[0] // cell), int(p[1] // cell)
            span = int(r // cell) + 1
            for gx in range(cx - span, cx + span + 1):
                for gy in range(cy - span, cy + span + 1):
                    for q in grid.get((gx, gy), ()):
                        if (q[0] - p[0]) ** 2 + (q[1] - p[1]) ** 2 <= r * r:
                            n += 1
            if n > worst:
                worst, worst_at = n, p
        out[r] = (worst, worst_at)
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--list", required=True, help="the plugin's light list (v1 or v2)")
    ap.add_argument("--inventory", help="2dFx inventory written by lamp_list.py --inventory")
    ap.add_argument("--game", help="game dir: build the inventory instead of reading one")
    ap.add_argument("--region", default="LA",
                    help="the region the target applies to (LA = Los Santos); "
                         "'all' covers every region")
    ap.add_argument("--tol", type=float, default=1.0,
                    help="how close a list light must be to count as covering a 2dFx one")
    ap.add_argument("--min", type=float, default=95.0,
                    help="the coverage target for --region; below it the tool fails")
    ap.add_argument("--density", default="",
                    help="comma separated radii: worst case lights within each")
    ap.add_argument("--show-missing", type=int, default=25)
    a = ap.parse_args()

    if a.inventory:
        inv = read_inventory(a.inventory)
    elif a.game:
        instances, ide = lamp_list.collect_instances(a.game)
        names = {ide.get(mid, ("", 0.0))[0] for mid, _p, _r, _reg, _s in instances}
        inv = lamp_list.world_2dfx_lights(
            instances, ide, lamp_list.dff_lights(a.game, {n for n in names if n}))
    else:
        sys.exit("give --inventory or --game")

    pts = read_list(a.list)
    grid = Grid(pts)
    print(f"list {a.list}: {len(pts)} lights")
    print(f"2dFx inventory: {len(inv)} lights")

    regions = collections.Counter(i["region"] for i in inv)
    print("2dFx lights by region: " + ", ".join(
        f"{k}={v}" for k, v in sorted(regions.items(), key=lambda kv: -kv[1])))

    target = None
    for region, total in sorted(regions.items(), key=lambda kv: -kv[1]):
        rows = [i for i in inv if i["region"] == region]
        street = [i for i in rows if i["model"] not in lamp_list.TRAFFIC_NAMES]
        cov = [i for i in street if grid.near((i["x"], i["y"], i["z"]), a.tol)]
        pct = 100.0 * len(cov) / max(1, len(street))
        sig = len(rows) - len(street)
        print(f"  {region:<9s} 2dfx={total:<6d} signal_layer={sig:<6d} "
              f"street={len(street):<6d} covered={len(cov):<6d} coverage={pct:6.2f}%")
        if region == a.region or a.region == "all":
            target = (region, len(street), len(cov), pct)

    if target is None:
        sys.exit(f"no 2dFx lights in region {a.region!r}")
    region, total, cov, pct = target
    print(f"\nTARGET region {region}: {cov}/{total} = {pct:.2f}% "
          f"(target >= {a.min:.1f}%, tolerance {a.tol} game units)")

    miss = collections.Counter()
    for i in inv:
        if i["region"] != region and a.region != "all":
            continue
        if i["model"] in lamp_list.TRAFFIC_NAMES:
            continue
        if not grid.near((i["x"], i["y"], i["z"]), a.tol):
            miss[i["model"]] += 1
    if miss:
        print(f"uncovered 2dFx models in {region}: {len(miss)}")
        for name, n in miss.most_common(a.show_missing):
            print(f"  {name:<26s} {n}")
    else:
        print(f"no uncovered 2dFx model in {region}")

    if a.density:
        radii = [float(x) for x in a.density.split(",") if x.strip()]
        for r, (worst, at) in sorted(density(pts, radii).items()):
            where = "" if at is None else f" (worst at {at[0]:.0f},{at[1]:.0f})"
            print(f"density: worst case {worst} list lights within {r:g} game units{where}")

    if pct + 1e-9 < a.min:
        print(f"FAIL coverage {pct:.2f}% < {a.min:.1f}% in {region}")
        return 1
    print(f"PASS coverage {pct:.2f}% >= {a.min:.1f}% in {region}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
