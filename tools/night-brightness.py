#!/usr/bin/env python3
"""n05: the two brightness numbers rhY's 2026-11-12b test asked for, measured.

    usage: tools/night-brightness.py GAME_DIR [--since TIME] [options]

Finding C asked for "nights significantly darker than daytime even with the street
lights on", with a number attached: **a night shot's mean luma <= 25 % of the noon
shot's, at the same stop**. Finding B asked for the sunset to stop going gray when
the camera faces the sun, which needs a number too, because "gray" is otherwise
unfalsifiable. This tool prints both, from the tour's own shots, and exits nonzero
when the target is missed - so the next session either clears it or names the stop
that did not.

How the shots are found: exactly the way tools/tour-report.py finds them (manifest
entry paired to the nearest shot file by timestamp), so `--since` behaves the same
way and the same 16 stops come back.

How day and night are told apart: the tour's stop names carry the clock
(`00-baseline-noon-grove`, `01-under-lamp-grove-2100`, `09-grove-dawn-0530`).
`noon` is 12:00; a four-digit token that reads as a time is the exact clock. A stop
is night at 20:00..04:59, day at 10:00..14:59, and twilight (19:xx, 05:xx-06:xx) is
printed but not scored - the sunset stops have the sun 10 degrees up and the ambient
at its full day value, and demanding night darkness of a sunset is not a test. The "same stop" partner is the other shot whose name
shares the most location words (`grove` in both examples above); `--reference NAME`
overrides the reference shot when the tour has no noon stop for the place.

What "gray" is measured as (finding B). The two sunset stops are the same place,
25 degrees of camera yaw apart, so the difference IS the defect:

  * sky-band spread   p95-p05 of the top third of the frame. A washed sky is a
    slab: the 2026-11-12 10:22 run measured 102.2 facing away against 47.7 facing
    the sun.
  * bright share      % of pixels at luma >= 191. The frame losing its bright end
    is the local tonemapper's crush branch: 4.48 % against 0.03 %.
  * sky gradient      mean |grad| of the top third: 5.10 against 1.54.
  * black floor       p01 of the whole frame. It FALLS facing the sun (19.9 ->
    10.7), which is what rules out bloom and in-scattering: an additive veil can
    only raise it.

PASS for the sunset pair means the facing-the-sun shot keeps at least
--keep (default 0.70) of the facing-away shot's sky spread and at least
--keep-bright (default 0.25) of its bright share. Those two are the numbers to
beat, taken from the broken run and stated so the next one can be compared to them.

What the lamp term is (n23). Every night stop also prints its **lit-pixel fraction**:
the share of pixels more than 30 luma above the frame's own p10 floor, with the mean of
the lit and the unlit part next to it. That is the number that says whether a night frame
is its lamps or its ambient - the 2026-11-12 10:22 under-lamp stop measured **72.0 %** lit
(96.0 against 15.1), which is why finding C is a lamp-flux fix and not an ambient fix.
`--lit-max N` turns it into a gate too (0 = report only, the default: rhY never set a
number for it, so the tool must not invent one that fails a good run).

`--predict S` prints the n05 linear model next to each night stop: taking the lamps to S
of their flux moves mean_lit and leaves mean_dark alone, so
mean(S) = mean_dark + (lit/100) * S * (mean_lit - mean_dark). Against the 2026-11-12
10:22 under-lamp stop (72.0 % lit, 96.0 lit, 15.1 dark, noon 128.0) S=0.4 predicts 38.4 =
30.0 % of noon. It is a prediction printed so the next run can be compared with it, and
it says nothing about the renderer's exposure - which is exactly why the measured line
above it is the one that gates.

This is a GATE, not a report: it exits 1 when a scored stop misses `--target`, when a
`--lit-max` is exceeded, when the sunset pair goes GRAY, and when the run has no scored
night stop at all (a tour that lost its 21:00 stops must not be able to pass by saying
nothing). Exit 2 is a usage/IO problem: no screenshots, no manifest, no such stop name.

Nothing outside GAME_DIR/screenshots/report-* is written; the game folder is only
read. Tests: tests/test_night_brightness.py runs it against a fake tree.
"""
import argparse
import importlib.util
import os
import re
import sys

TOOLS = os.path.dirname(os.path.abspath(__file__))
_spec = importlib.util.spec_from_file_location(
    "tourreport", os.path.join(TOOLS, "tour-report.py"))
tr = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(tr)

try:
    import numpy as np
except ImportError:                                       # pragma: no cover
    np = None

NIGHT_FROM_HOUR = 20          # 20:00 .. 04:59 is night for the tour's names
NIGHT_TO_HOUR = 4
DAY_FROM_HOUR = 10
DAY_TO_HOUR = 14
HIGH_LUMA = 191               # "the bright end of the frame", as tour-report reads luma
WORD = re.compile(r"[a-z]+")
WORDISH = re.compile(r"[A-Za-z0-9]+")
NAMED_CLOCK = {"noon": (12, 0), "dawn": (5, 30), "dusk": (19, 5), "midnight": (0, 0)}


def clock_of(name):
    """The clock a tour stop name carries -> (hour, minute) or None.

    The tour writes the clock into the stop name, at either end: `01-under-lamp-
    grove-2100`, `00-baseline-noon-grove`, `12-car-mirror-grove-1300`. A four-digit
    token that reads as a time wins over a word, because `09-grove-dawn-0530` carries
    both and the digits are the exact one. A stop number (`08a`) is never four digits.
    """
    toks = WORDISH.findall(name.strip().lower())
    for t in toks:
        if len(t) == 4 and t.isdigit():
            h, m = int(t[:2]), int(t[2:])
            if h <= 23 and m <= 59:
                return h, m
    for t in toks:
        if t in NAMED_CLOCK:
            return NAMED_CLOCK[t]
    return None


def day_class(name):
    """'day', 'night', 'twilight' or None, from the clock in the stop name.

    Twilight (19:xx, 05:xx-06:xx) is scored informationally, not against the night
    target: the tour's sunset stops have the sun 10 degrees up and the ambient at
    its full day value, and calling them "night" would demand a darkness that no
    sunset has.
    """
    c = clock_of(name)
    if not c:
        return None
    h = c[0]
    if DAY_FROM_HOUR <= h <= DAY_TO_HOUR:
        return "day"
    if h >= NIGHT_FROM_HOUR or h <= NIGHT_TO_HOUR:
        return "night"
    if h == 19 or 5 <= h <= 6:
        return "twilight"
    return None


def is_night(name):
    return day_class(name) == "night"


def is_day(name):
    return day_class(name) == "day"


def location_words(name):
    """The place part of a stop name: words only, no stop number, no clock token."""
    c = clock_of(name)
    clock_tok = None
    if c:
        clock_tok = "%04d" % (c[0] * 100 + c[1])
    out = set()
    for w in WORD.findall(name.strip().lower()):
        if len(w) <= 3 or w == clock_tok:
            continue
        if w in NAMED_CLOCK:
            continue
        out.add(w)
    return out


LIT_STEP = 30.0        # luma above the frame's own floor that counts as "lit by something"


def band_metrics(path):
    """The gray numbers AND the lamp-term split. None if numpy is missing (the night
    half still works). The lit/unlit decomposition is the one n05's model runs on:
    floor = p10 of the whole frame, a pixel is lit when it is LIT_STEP luma above that
    floor, and mean = (1-f)*mean_dark + f*mean_lit. Taking the lamps to s of their flux
    moves mean_lit and leaves mean_dark alone, which is the whole prediction."""
    if np is None:
        return None
    from PIL import Image
    im = np.asarray(Image.open(path).convert("RGB"), dtype=np.float32)
    h = im.shape[0]
    lum = 0.2126 * im[..., 0] + 0.7152 * im[..., 1] + 0.0722 * im[..., 2]
    sky = lum[: h // 3]
    gy, gx = np.gradient(sky)
    floor = float(np.percentile(lum, 10))
    lit = lum > floor + LIT_STEP
    return {
        "sky_spread": float(np.percentile(sky, 95) - np.percentile(sky, 5)),
        "sky_grad": float(np.sqrt(gx * gx + gy * gy).mean()),
        "bright_pct": 100.0 * float((lum >= HIGH_LUMA).mean()),
        "p01": float(np.percentile(lum, 1)),
        "floor": floor,
        "lit_pct": 100.0 * float(lit.mean()),
        "mean_lit": float(lum[lit].mean()) if lit.any() else floor,
        "mean_dark": float(lum[~lit].mean()) if (~lit).any() else floor,
    }


def collect(game, since, tol):
    """-> [(name, path, metrics, band)] for the run `since` selects."""
    man = os.path.join(game, "screenshots", "tour-manifest.txt")
    shots_dir = os.path.join(game, "screenshots", "shots")
    if not os.path.isdir(shots_dir):
        sys.stderr.write("night-brightness: no screenshots directory %s\n" % shots_dir)
        return None
    if not os.path.exists(man):
        sys.stderr.write("night-brightness: no manifest %s\n" % man)
        return None
    entries, events = tr.parse_manifest(man)
    secs = tr.split_sections(entries, events)
    cut = tr.parse_since(since, entries[0].dt if entries else None) if since else None
    sec = tr.pick_section(secs, cut) if cut else (secs[-1] if secs else (None, None, entries, events))
    picked = sec[2] if sec else []
    files = tr.shot_files(shots_dir)
    pairs, _unused = tr.pair(picked, files, tol=tol)
    out = []
    for entry, f in pairs:
        if not f:
            continue
        out.append((entry.name, f.path, tr.metrics(f.path), band_metrics(f.path)))
    return out


def best_partner(name, pool):
    """The shot in `pool` (rows, i.e. (name, path, metrics, band)) sharing the most
    location words with `name`. Ties go to the earlier stop, which is the tour's own
    order and therefore the more predictable one."""
    mine = location_words(name)
    best, score = None, 0
    for row in pool:
        shared = len(mine & location_words(row[0]))
        if shared > score:
            best, score = row, shared
    return best, score


def main(argv=None):
    ap = argparse.ArgumentParser(prog="tools/night-brightness.py")
    ap.add_argument("game_dir")
    ap.add_argument("--since", default="", help="tour start, as tools/tour-report.py --since")
    ap.add_argument("--tol", type=float, default=8.0, help="manifest/file pairing tolerance")
    ap.add_argument("--target", type=float, default=25.0,
                    help="night mean as a %% of the noon mean (finding C: 25)")
    ap.add_argument("--reference", default="", help="reference (day) shot name override")
    ap.add_argument("--lit-max", type=float, default=0.0,
                    help="fail a night stop whose lit-pixel %% is above this (0 = report only)")
    ap.add_argument("--predict", type=float, default=0.0, metavar="S",
                    help="also print the lamp-flux model with the lamps at S (0 = off)")
    ap.add_argument("--sunset", nargs=2, metavar=("FACING_AWAY", "FACING_SUN"),
                    default=None, help="the two sunset stop names, in that order")
    ap.add_argument("--keep", type=float, default=0.70,
                    help="fraction of the away shot's sky spread the sun-facing shot must keep")
    ap.add_argument("--keep-bright", type=float, default=0.25,
                    help="fraction of the away shot's bright share it must keep")
    a = ap.parse_args(argv)

    rows = collect(a.game_dir, a.since, a.tol)
    if rows is None:
        return 2
    if not rows:
        sys.stderr.write("night-brightness: no shot paired with a manifest entry\n")
        return 1

    day = [r for r in rows if is_day(r[0])]
    night = [r for r in rows if is_night(r[0])]
    twilight = [r for r in rows if day_class(r[0]) == "twilight"]
    ref = None
    if a.reference:
        ref = next((r for r in rows if r[0] == a.reference), None)
        if ref is None:
            sys.stderr.write("night-brightness: no shot named %r in this run\n" % a.reference)
            return 2
    if not ref and day:
        # The tour's own baseline stop if it has one, else the first day stop in
        # tour order. Not the brightest day shot: the water stop at noon is 172 and
        # would set a target no night street could ever pass.
        ref = next((r for r in day if "baseline" in r[0]), day[0])

    fails = []
    print("== finding C: night mean vs noon mean (target <= %.0f %%) ==" % a.target)
    if ref is None:
        print("  no day stop in this run: pass --reference NAME, or run the tour with its noon stop")
        fails.append("no day reference shot")
    else:
        print("  reference: %-30s mean=%6.1f  (%.0f%% ceiling = %.1f)"
              % (ref[0], ref[2]["mean"], a.target, a.target / 100.0 * ref[2]["mean"]))
        if not night:
            # A run whose night stops are missing is not a pass. It is the failure mode
            # of a gate that prints a table: no rows, no complaint, exit 0.
            fails.append("no night stop (20:00..04:59) in this run to score")
        for name, path, m, band in sorted(night, key=lambda r: r[0]):
            partner, score = best_partner(name, day)
            base, label = (partner[2]["mean"], "same stop " + partner[0]) if score else \
                          (ref[2]["mean"], "run reference")
            ratio = 100.0 * m["mean"] / base if base else 0.0
            verdict = "ok" if ratio <= a.target else "OVER"
            if ratio > a.target:
                fails.append("%s at %.1f %% of %s" % (name, ratio, label))
            print("  %-30s mean=%6.1f  %6.1f %% of %6.1f  %-4s (%s)"
                  % (name, m["mean"], ratio, base, verdict, label))
            if band:
                print("  %-30s       lit %5.1f %% of the frame  floor=%5.1f  "
                      "mean_lit=%6.1f mean_dark=%6.1f%s"
                      % ("", band["lit_pct"], band["floor"], band["mean_lit"],
                         band["mean_dark"],
                         "" if not a.lit_max else
                         ("  lit OVER" if band["lit_pct"] > a.lit_max else "  lit ok")))
                if a.lit_max and band["lit_pct"] > a.lit_max:
                    fails.append("%s has %.1f %% lit pixels (max %.0f)"
                                 % (name, band["lit_pct"], a.lit_max))
            if a.predict > 0.0 and band:
                pred = band["mean_dark"] + (band["lit_pct"] / 100.0) * a.predict * \
                    (band["mean_lit"] - band["mean_dark"])
                print("  %-30s       predicted with the lamps at %.2f: mean %6.1f = %5.1f %% "
                      "(model only: no exposure change)"
                      % ("", a.predict, pred, 100.0 * pred / base if base else 0.0))
        for name, path, m, _band in sorted(twilight, key=lambda r: r[0]):
            partner, score = best_partner(name, day)
            base, label = (partner[2]["mean"], "same stop " + partner[0]) if score else \
                          (ref[2]["mean"], "run reference")
            ratio = 100.0 * m["mean"] / base if base else 0.0
            print("  %-30s mean=%6.1f  %6.1f %% of %6.1f  twilight, not scored (%s)"
                  % (name, m["mean"], ratio, base, label))

    if a.sunset:
        away_name, sun_name = a.sunset
        away = next((r for r in rows if r[0] == away_name), None)
        sun = next((r for r in rows if r[0] == sun_name), None)
        print("\n== finding B: facing the sun vs facing away (%s / %s) ==" % (away_name, sun_name))
        if not away or not sun:
            print("  missing shot: %s%s"
                  % ("" if away else away_name, "" if (away and sun) else " " + sun_name))
            fails.append("sunset pair not found")
        elif away[3] is None or sun[3] is None:
            print("  numpy/PIL not available: the gray metrics cannot be computed")
            fails.append("numpy missing")
        else:
            a_b, s_b = away[3], sun[3]
            for key, want in (("sky_spread", a.keep), ("sky_grad", a.keep),
                              ("bright_pct", a.keep_bright), ("p01", 0.5)):
                ratio = (s_b[key] / a_b[key]) if a_b[key] else 1.0
                verdict = "ok" if ratio >= want else "GRAY"
                if ratio < want:
                    fails.append("%s falls to %.2f of the away value (must keep %.2f)"
                                 % (key, ratio, want))
                print("  %-12s away=%8.2f  into the sun=%8.2f  ratio %5.2f (keep %.2f) %s"
                      % (key, a_b[key], s_b[key], ratio, want, verdict))
            print("  whole-frame mean away=%6.1f into the sun=%6.1f"
                  % (away[2]["mean"], sun[2]["mean"]))

    print()
    if fails:
        print("night-brightness: %d problem(s):" % len(fails))
        for f in fails:
            print("  - " + f)
        return 1
    print("night-brightness: all brightness targets met")
    return 0


if __name__ == "__main__":
    sys.exit(main())
