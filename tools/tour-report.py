#!/usr/bin/env python3
"""Turn a finished photo tour into labelled contact sheets plus an objective report.

    usage: tools/tour-report.py GAME_DIR [--since TIME] [options]

GAME_DIR is the game folder (the one holding `screenshots/` and `scripts/rhyremix.log`).
The tour (`plugin/lua/SARemix_Tour.lua`) appends `shot NN <name>` lines to
`screenshots/tour-manifest.txt` a moment before `scripts/shot-watch.sh` writes
`screenshots/shots/shot_<YYYY-MM-DD>_<HH-MM-SS>.png`, so the two halves are joined by time,
not by name: `pair()` matches each manifest entry to the nearest shot file within `--tol`
seconds, and every shot file is used at most once. Files that no manifest entry claims are
the manual F12 shots and are listed separately instead of being silently dropped.

What it writes, all inside `GAME_DIR/screenshots/report-<tour start>/`:

  * `sheet-NN.png` - labelled contact sheets, `--per-sheet` (default 4) shots per sheet,
    each tile captioned with its number, name and headline numbers. PIL if available,
    otherwise ImageMagick `montage`.
  * `report.md` - one table for the whole tour and one block per shot with objective
    numbers only (no eyeballing): mean luma and p05/p50/p95 (Rec.709 luma, 0..255),
    % clipped white (max channel >= WHITE_CLIP), % black (max channel <= BLACK_LEVEL),
    mean HSV saturation, and for the signal shots the fraction of red / green / amber
    pixels (hue bands below, gated on saturation and value so grey asphalt is never green).
    Under each shot the `rhyremix.log` lines nearest that second, one per category:
    `lights` (streetlights summary), `signals` (trafficlights summary, i.e. the
    green/amber/red counts the plugin actually pushed) and `sky` (sun elevation, daylight,
    lamps on/off, game clock). Since n03 both `scripts/rhyremix.log` and its rotated
    `rhyremix.log.prev` are read: the launcher moves the log when a session pushes it over
    its size cap, so the run being reported can be the one that just moved.

`--since TIME` restricts the run to the tail of a manifest (a tour may be run several times
a day): `YYYY-MM-DD HH:MM[:SS]`, `YYYY-MM-DD`, `HH:MM[:SS]` (date taken from the manifest)
or a raw epoch seconds. A manifest is append-only - the tour appends to one `tour-manifest.txt` -
so it can hold several runs, each opened by a `tour start` line. With `--since`,
`split_sections()` cuts the manifest at those lines and `pick_section()` reports the run the cut
asks for - the run whose lines span it, else the first run starting at or after it - and a bare
`HH:MM` is read on the newest manifest date. So `--since 00:48` on a manifest holding a
2026-11-11 run and a 2026-11-12 00:49 run reports the 2026-11-12 run - it used to report the
11th, because the cut was taken on the date of the first entry and the report directory was named
after the first `tour start` in the file. A cut that falls inside one run still keeps only that
run's tail, named after its first surviving entry.

Nothing outside the report folder is written, and the game folder is never required: the
tests run it against a fake tree under `build/tmp/`.

exit: 0 report written, 1 nothing to report (no manifest entry paired with a file),
2 usage / no screenshots directory.
"""
import argparse
import bisect
import datetime as dt
import math
import os
import re
import shutil
import subprocess
import sys
from collections import namedtuple

try:
    import numpy as np
except ImportError:                                    # metrics fall back to pure python
    np = None

# ---------------------------------------------------------------- objective numbers ---
# Luma is Rec.709 (what a display's own brightness meter weighs), reported on 0..255.
LUMA_W = (0.2126, 0.7152, 0.0722)
WHITE_CLIP = 250      # a channel at/above this is clipped; we test max(R,G,B)
BLACK_LEVEL = 4       # max(R,G,B) at/below this is a crushed black pixel
SAT_MIN = 0.45        # a "coloured" pixel (signal heads) must be this saturated...
VAL_MIN = 0.25        # ...and this bright, otherwise grey tarmac reads as a hue
HUE_RED_MAX = 15.0    # deg: red is < 15 or >= 345
HUE_AMBER_MAX = 50.0  # deg: amber/orange is 15..50
HUE_GREEN_MIN = 75.0  # deg: green is 75..170
HUE_GREEN_MAX = 170.0
# Flags for the report: a shot that is obviously fighting the exposure.
FLAG_WHITE = 2.0      # % of clipped white pixels
FLAG_BLACK = 40.0     # % of black pixels
FLAG_DARK = 24.0      # mean luma
FLAG_BLOWN = 224.0    # mean luma

SHOT_LINE_RE = re.compile(r"^shot\s+(\d+)\s+(\S+)\s*$")
TS_RE = re.compile(r"^(\d{4})-(\d{2})-(\d{2})[ T](\d{2}):(\d{2}):(\d{2})")
SHOT_FILE_RE = re.compile(
    r"^(?:shot_|rhyremix_|shot)?_?(\d{4})-(\d{2})-(\d{2})[_ ](\d{2})-(\d{2})-(\d{2})\.(png|jpe?g|bmp|tif|tiff)$",
    re.IGNORECASE)
TOUR_START_RE = re.compile(r"tour start", re.IGNORECASE)
TOUR_DONE_RE = re.compile(r"tour done", re.IGNORECASE)

# rhyremix.log: "[2026-11-11 08:07:00.671] streetlights: 320 active, 320 drawn this frame ..."
LOG_TS_RE = re.compile(r"^\[(\d{4})-(\d{2})-(\d{2})[ T](\d{2}):(\d{2}):(\d{2})(?:\.(\d{1,6}))?\]\s*(.*)$")
LOG_CATEGORIES = (
    ("lights", re.compile(r"^streetlights:\s+\d+\s+active")),
    # s01/s19 print a DIFFERENT shape when the corona hook is live:
    #   trafficlights[source=game]: 7 heads lit, 7 drawn this frame (...) | hook: calls=... records=...
    # Matching only the old "trafficlights: N active" line left the signals column blank exactly on
    # the signal-head test (both shapes are in the real 2026-11-11 log: 303 list lines, 4 source=game).
    ("signals", re.compile(r"^trafficlights(\[[^\]]*\])?:\s+\d+\s+(active|heads lit)")),
    ("sky", re.compile(r"^sky:")),
)

ShotEntry = namedtuple("ShotEntry", "seq name dt raw")
ShotFile = namedtuple("ShotFile", "path dt name")
LogLine = namedtuple("LogLine", "dt text")


def log(msg):
    print(msg, file=sys.stderr)


# ----------------------------------------------------------------------- manifest ---
def parse_manifest(path):
    """-> (shots, events). A manifest line is '<local time>  <text>'; only 'shot NN name'
    becomes a shot, everything else (tour start / done / failures) is kept as an event."""
    shots, events = [], []
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for lineno, line in enumerate(f, 1):
            line = line.rstrip("\n")
            if not line.strip():
                continue
            m = TS_RE.match(line)
            if not m:
                events.append((None, line))
                log("tour-report: %s:%d: no timestamp, kept as an event" % (path, lineno))
                continue
            when = dt.datetime(*(int(g) for g in m.groups()))
            text = line[m.end():].strip()
            sm = SHOT_LINE_RE.match(text)
            if sm:
                shots.append(ShotEntry(int(sm.group(1)), sm.group(2), when, line))
            else:
                events.append((when, text))
    shots.sort(key=lambda s: s.dt)
    return shots, events


def parse_since(spec, manifest_date=None):
    """--since TIME -> naive datetime, or None for 'no filter'. Accepts a full timestamp,
    a bare date, a bare clock time or raw epoch seconds. `manifest_date` is what a bare clock
    time is read on: one datetime, or the list of manifest entry times (then the newest date in it
    that is not after the newest entry wins - see split_sections)."""
    s = (spec or "").strip().replace("_", " ").replace("T", " ")
    s = re.sub(r"\s+", " ", s)
    if not s or s.lower() in ("none", "all", "0"):
        return None
    if re.fullmatch(r"\d{9,11}(?:\.\d+)?", s):          # epoch seconds
        return dt.datetime.fromtimestamp(float(s))
    for fmt in ("%Y-%m-%d %H:%M:%S", "%Y-%m-%d %H:%M", "%Y-%m-%d"):
        try:
            return dt.datetime.strptime(s, fmt)
        except ValueError:
            pass
    m = re.fullmatch(r"(\d{2}):(\d{2})(?::(\d{2}))?", s)
    if m:
        if isinstance(manifest_date, (list, tuple)) and manifest_date:
            latest = max(manifest_date)
            cands = sorted({d.replace(hour=int(m.group(1)), minute=int(m.group(2)),
                                     second=int(m.group(3) or 0), microsecond=0)
                            for d in manifest_date})
            not_after = [c for c in cands if c <= latest]
            return not_after[-1] if not_after else cands[0]
        base = manifest_date or dt.datetime.now()
        return base.replace(hour=int(m.group(1)), minute=int(m.group(2)),
                            second=int(m.group(3) or 0), microsecond=0)
    raise ValueError("cannot read --since %r (try 'YYYY-MM-DD HH:MM:SS', 'HH:MM' or epoch)" % spec)


def split_sections(shots, events):
    """Cut a manifest into runs -> [(start, end, shots, events), ...].

    A run starts at a `tour start` event and holds every shot entry and event up to the next one;
    `end` is the next run's start, or None for the last run. Anything before the first `tour start`
    (a manifest cut off mid-run, an older format) comes back as a leading section with start None.
    The 2026-11-12 manifest holds the 2026-11-11 run and the 2026-11-12 run one after the other,
    because the tour appends to one file: without this cut both runs land in one report and the
    report directory is named after the older run.
    """
    marks = sorted(w for w, text in events if w and TOUR_START_RE.search(text))
    if not marks:
        return [(None, None, shots, events)]
    secs = []
    pre_s = [s for s in shots if s.dt < marks[0]]
    pre_e = [ev for ev in events if ev[0] is None or ev[0] < marks[0]]
    if pre_s or pre_e:
        secs.append((None, marks[0], pre_s, pre_e))
    for i, m in enumerate(marks):
        nxt = marks[i + 1] if i + 1 < len(marks) else None
        keep = lambda d: m <= d < nxt if nxt else d >= m
        secs.append((m, nxt, [s for s in shots if keep(s.dt)],
                     [ev for ev in events if ev[0] is not None and keep(ev[0])]))
    return secs


# -------------------------------------------------------------------- shot files ---
def shot_files(dirname):
    """Every image in dirname, with the timestamp from its name (the tour / shot.sh name
    them shot_YYYY-MM-DD_HH-MM-SS.png); a name we cannot read falls back to mtime."""
    out = []
    for name in sorted(os.listdir(dirname)):
        path = os.path.join(dirname, name)
        if not os.path.isfile(path):
            continue
        if not name.lower().endswith((".png", ".jpg", ".jpeg", ".bmp", ".tif", ".tiff")):
            continue
        m = SHOT_FILE_RE.match(name)
        if m:
            when = dt.datetime(*[int(g) for g in m.groups()[:6]])
        else:
            when = dt.datetime.fromtimestamp(os.path.getmtime(path))
        out.append(ShotFile(path, when, name))
    out.sort(key=lambda s: s.dt)
    return out


def pair(shots, files, tol=8.0):
    """Join manifest entries to files by time. Every (entry, file) pair within `tol` seconds
    is a candidate, cheapest first, and neither side is used twice - so two shots 17 s apart
    can never fight over one file. -> (pairs, unpaired_files)"""
    cands = []
    for i, s in enumerate(shots):
        for j, f in enumerate(files):
            d = abs((f.dt - s.dt).total_seconds())
            if d <= tol:
                cands.append((d, i, j))
    cands.sort()
    take = {}
    used_f = set()
    for _d, i, j in cands:
        if i in take or j in used_f:
            continue
        take[i] = j
        used_f.add(j)
    pairs = [(s, files[take[i]] if i in take else None) for i, s in enumerate(shots)]
    return pairs, [f for j, f in enumerate(files) if j not in used_f]


# ----------------------------------------------------------------------- metrics ---
def _hist_stats(hist):
    """(mean, p05, p50, p95) of a 256-bin histogram of 8-bit luma. Shared by the numpy path and
    the plain-python path so the two cannot drift apart: luma is quantised to 8 bit first, which
    is what a display meter does anyway."""
    total = int(sum(hist))
    if not total:
        return 0.0, 0.0, 0.0, 0.0
    mean = float(sum(i * c for i, c in enumerate(hist))) / total
    wants = (0.05 * total, 0.50 * total, 0.95 * total)
    out, acc, k = [], 0, 0
    for i, c in enumerate(hist):
        acc += c
        while k < 3 and acc >= wants[k]:
            out.append(float(i))
            k += 1
    while len(out) < 3:
        out.append(255.0)
    return mean, out[0], out[1], out[2]


def _hue_deg(r, g, b, mx, mn):
    """HSV hue in degrees, vectorised. r/g/b/mx/mn are float32 arrays (0..255)."""
    d = mx - mn
    h = np.zeros(mx.shape, dtype=np.float32)
    done = d <= 0
    with np.errstate(invalid="ignore", divide="ignore"):
        sel = (~done) & (mx == r)
        h[sel] = ((g[sel] - b[sel]) / d[sel]) % 6
        sel = (~done) & (mx == g) & (h == 0)
        h[sel] = (b[sel] - r[sel]) / d[sel] + 2
        sel = (~done) & (mx == b) & (h == 0)
        h[sel] = (r[sel] - g[sel]) / d[sel] + 4
    return h * 60.0


def metrics(path):
    """Objective numbers for one image. -> dict with size, mean/p05/p50/p95 luma (0..255),
    pct_white / pct_black (of pixels), mean_sat (0..1) and pct_red / pct_amber / pct_green
    (only the signal stops print the last three). This is the numpy path; _metrics_pure() is the
    same definition without numpy. Everything stays uint8/float32: this build of numpy is ~300x
    slower on float64 arrays of 2 M pixels (1.7 s vs 5 ms for the luma dot product), which was
    24 s per 1920x1080 shot."""
    from PIL import Image
    with Image.open(path) as im:
        im = im.convert("RGB")
        w, h = im.size
        if np is None:
            return _metrics_pure(im, w, h)
        a = np.asarray(im, dtype=np.uint8)                 # h x w x 3
    px = a.reshape(-1, 3)
    lum = np.dot(px.astype(np.float32), np.asarray(LUMA_W, dtype=np.float32))
    lum8 = np.clip(np.floor(lum + 0.5), 0, 255).astype(np.uint8)
    mean, p05, p50, p95 = _hist_stats(np.bincount(lum8, minlength=256).tolist())
    mx = a.max(axis=2).astype(np.float32)
    mn = a.min(axis=2).astype(np.float32)
    sat = np.zeros(mx.shape, dtype=np.float32)
    np.divide(mx - mn, mx, out=sat, where=mx > 0)          # HSV S; 0 where the pixel is black
    lit = mx > 0
    chroma = (sat >= SAT_MIN) & (mx >= VAL_MIN * 255.0)
    hue = _hue_deg(a[..., 0].astype(np.float32), a[..., 1].astype(np.float32),
                   a[..., 2].astype(np.float32), mx, mn)
    n = float(mx.size)
    return {
        "width": w, "height": h, "pixels": int(n),
        "mean": mean, "p05": p05, "p50": p50, "p95": p95,
        "pct_white": 100.0 * float((mx >= WHITE_CLIP).sum()) / n,
        "pct_black": 100.0 * float((mx <= BLACK_LEVEL).sum()) / n,
        "mean_sat": float(sat[lit].mean()) if lit.any() else 0.0,
        "pct_red": 100.0 * float((chroma & ((hue < HUE_RED_MAX) | (hue >= 360 - HUE_RED_MAX))).sum()) / n,
        "pct_amber": 100.0 * float((chroma & (hue >= HUE_RED_MAX) & (hue < HUE_AMBER_MAX)).sum()) / n,
        "pct_green": 100.0 * float((chroma & (hue >= HUE_GREEN_MIN) & (hue < HUE_GREEN_MAX)).sum()) / n,
    }


def _metrics_pure(im, w, h):
    """Same definitions as metrics(), one pass over the pixels, no numpy. Reads the raw RGB
    bytes (getdata() is deprecated in Pillow and slow per pixel) and feeds the same 256-bin luma
    histogram to _hist_stats(), so mean and percentiles come out identical to the numpy path."""
    lumah = [0] * 256
    nwhite = nblack = 0
    sat_sum = sat_n = 0.0
    nred = namber = ngreen = 0
    total = 0
    it = iter(im.tobytes())          # mode is RGB: 3 bytes per pixel
    for r, g, b in zip(it, it, it):
        total += 1
        l = LUMA_W[0] * r + LUMA_W[1] * g + LUMA_W[2] * b
        lumah[min(255, max(0, int(math.floor(l + 0.5))))] += 1
        mx, mn = max(r, g, b), min(r, g, b)
        if mx >= WHITE_CLIP:
            nwhite += 1
        if mx <= BLACK_LEVEL:
            nblack += 1
        if mx > 0:
            s = (mx - mn) / mx
            sat_sum += s
            sat_n += 1
            if s >= SAT_MIN and mx >= VAL_MIN * 255.0:
                d = mx - mn
                if mx == r:
                    hue = ((g - b) / d) % 6 * 60.0
                elif mx == g:
                    hue = ((b - r) / d + 2) * 60.0
                else:
                    hue = ((r - g) / d + 4) * 60.0
                if hue < HUE_RED_MAX or hue >= 360 - HUE_RED_MAX:
                    nred += 1
                elif hue < HUE_AMBER_MAX:
                    namber += 1
                elif HUE_GREEN_MIN <= hue < HUE_GREEN_MAX:
                    ngreen += 1
    mean, p05, p50, p95 = _hist_stats(lumah)
    return {
        "width": w, "height": h, "pixels": total,
        "mean": mean, "p05": p05, "p50": p50, "p95": p95,
        "pct_white": 100.0 * nwhite / total, "pct_black": 100.0 * nblack / total,
        "mean_sat": sat_sum / sat_n if sat_n else 0.0,
        "pct_red": 100.0 * nred / total, "pct_amber": 100.0 * namber / total,
        "pct_green": 100.0 * ngreen / total,
    }


def is_signal_shot(name, mode="auto"):
    """`auto` = only the stops whose name says signal/traffic (06-signals-close, ...)."""
    if mode == "all":
        return True
    if mode == "none":
        return False
    return bool(re.search(r"signal|traffic", name, re.IGNORECASE))


def flags(m):
    out = []
    if m["pct_white"] >= FLAG_WHITE:
        out.append("clipping %.1f%% white" % m["pct_white"])
    if m["pct_black"] >= FLAG_BLACK:
        out.append("crushed %.0f%% black" % m["pct_black"])
    if m["mean"] < FLAG_DARK:
        out.append("dark (mean %.0f)" % m["mean"])
    if m["mean"] > FLAG_BLOWN:
        out.append("blown (mean %.0f)" % m["mean"])
    return out


# --------------------------------------------------------------------------- log ---
def log_files(path):
    """The log plus its rotated sibling, oldest first (n03).

    launcher/launch-rtx.sh moves rhyremix.log to rhyremix.log.prev when a session pushes it over its
    size cap, so the run being reported can live entirely in the .prev - starting the game again
    between the tour and the report is enough to do that. Both files are read; nearest() picks by
    timestamp, so an older .prev line only wins when it really is the nearest one."""
    out = []
    prev = path + ".prev"
    if os.path.isfile(prev):
        out.append(prev)
    if os.path.isfile(path):
        out.append(path)
    return out


def load_log(path):
    """-> {category: [(dt, text) sorted]} for the three categories we report, plus the
    total number of lines read. Missing file -> empty dict (the report says so). A rotated
    rhyremix.log.prev is read too (n03): see log_files()."""
    cats = {name: [] for name, _ in LOG_CATEGORIES}
    total = 0
    for f in log_files(path):
        with open(f, "r", encoding="utf-8", errors="replace") as fh:
            for line in fh:
                total += 1
                m = LOG_TS_RE.match(line.rstrip("\n"))
                if not m:
                    continue
                g = m.groups()
                micro = int((g[6] or "0").ljust(6, "0"))
                when = dt.datetime(*[int(x) for x in g[:6]], microsecond=micro)
                text = g[7]
                for name, rx in LOG_CATEGORIES:
                    if rx.match(text):
                        cats[name].append(LogLine(when, text))
    for v in cats.values():
        v.sort(key=lambda e: e.dt)
    return cats, total


def nearest(cats, category, when, window=30.0):
    """The log line of `category` closest to `when`, kept only inside `window` seconds.
    -> (LogLine, delta_seconds) or (None, None)."""
    lines = cats.get(category) or []
    if not lines:
        return None, None
    keys = [e.dt for e in lines]
    i = bisect.bisect_left(keys, when)
    best = None
    for j in (i - 1, i):
        if 0 <= j < len(lines):
            d = (lines[j].dt - when).total_seconds()
            if best is None or abs(d) < abs(best[1]):
                best = (lines[j], d)
    if best is None or abs(best[1]) > window:
        return None, None
    return best


# ----------------------------------------------------------------------- sheets ---
def _font(px):
    from PIL import ImageFont
    for path in ("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
                 "/usr/share/fonts/dejavu/DejaVuSans.ttf",
                 "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf"):
        if os.path.exists(path):
            try:
                return ImageFont.truetype(path, px)
            except OSError:
                pass
    for attempt in (lambda: ImageFont.truetype("DejaVuSans.ttf", px),
                    lambda: ImageFont.load_default(px),
                    ImageFont.load_default):
        try:
            return attempt()
        except (OSError, TypeError, AttributeError):
            continue
    return None


def contact_sheet(pairs, out_path, per_sheet=4, cell=(640, 360), caption_h=46):
    """One labelled sheet. PIL: each shot is thumbnailed into the cell, centred on a dark
    backing, with two caption lines under it. -> (rows, cols) actually used."""
    from PIL import Image, ImageDraw
    cols = 1 if per_sheet <= 2 else (2 if per_sheet <= 4 else 3)
    rows = (len(pairs) + cols - 1) // cols
    cw, chh = cell
    pad = 8
    sheet = Image.new("RGB", (cols * (cw + pad) + pad, rows * (chh + caption_h + pad) + pad), (16, 16, 18))
    draw = ImageDraw.Draw(sheet)
    f1, f2 = _font(17), _font(15)
    for k, (entry, sf, m) in enumerate(pairs):
        r, c = divmod(k, cols)
        x = pad + c * (cw + pad)
        y = pad + r * (chh + caption_h + pad)
        if sf is not None:
            with Image.open(sf.path) as im:
                im = im.convert("RGB")
                im.thumbnail((cw, chh))
                sheet.paste(im, (x + (cw - im.width) // 2, y + (chh - im.height) // 2))
        label = "%s %s" % (("%02d" % entry.seq) if entry else "--", entry.name if entry else sf.name)
        num = ("mean %.0f  white %.2f%%  black %.0f%%  sat %.2f"
               % (m["mean"], m["pct_white"], m["pct_black"], m["mean_sat"])) if m else "no image"
        draw.text((x + 2, y + chh + 3), label[:70], fill=(235, 235, 235), font=f1)
        draw.text((x + 2, y + chh + 24), num[:80], fill=(170, 175, 180), font=f2)
    sheet.save(out_path)
    return rows, cols


def contact_sheet_imagemagick(pairs, out_path, per_sheet=4, cell=(640, 360)):
    """Fallback when PIL is not importable: montage with a label per tile."""
    args = ["montage", "-tile", "%dx%d" % (min(per_sheet, 4), 1),
            "-geometry", "%dx%d+6+6" % cell, "-background", "#101014", "-fill", "white",
            "-pointsize", "13"]
    for entry, sf, _m in pairs:
        label = ("%02d %s" % (entry.seq, entry.name)) if entry else sf.name
        args += ["-label", label]
        args.append(sf.path if sf else "")
        if not sf:
            args.pop()
    args.append(out_path)
    subprocess.run(args, check=True)


def make_sheets(paired, out_dir, per_sheet=4, cell=(640, 360)):
    """-> list of (sheet filename, [(entry, shotfile, metrics), ...])"""
    have_pil = True
    try:
        import PIL  # noqa: F401
    except ImportError:
        have_pil = False
    if not have_pil and shutil.which("montage") is None:
        log("tour-report: neither PIL nor ImageMagick montage found, skipping sheets")
        return []
    sheets = []
    for n in range(0, len(paired), per_sheet):
        chunk = paired[n:n + per_sheet]
        name = "sheet-%02d.png" % (n // per_sheet + 1)
        path = os.path.join(out_dir, name)
        if have_pil:
            contact_sheet(chunk, path, per_sheet=per_sheet, cell=cell)
        else:
            contact_sheet_imagemagick(chunk, path, per_sheet=per_sheet, cell=cell)
        sheets.append((name, chunk))
        log("tour-report: wrote %s (%d shots)" % (path, len(chunk)))
    return sheets


# ------------------------------------------------------------------------ report ---
def fmt(v, nd=1):
    return ("%." + str(nd) + "f") % v


def render_report(pairs, sheets, events, cats, log_total, meta):
    L = []
    L.append("# Tour report - %s" % meta["game"])
    L.append("")
    L.append("generated %s by `tools/tour-report.py` (tolerance %.1f s, log window %.0f s)"
             % (meta["generated"], meta["tol"], meta["window"]))
    L.append("")
    L.append("| | |")
    L.append("|---|---|")
    L.append("| manifest | `%s` (%d shot entries, %d events) |" % (meta["manifest"], len(pairs), len(events)))
    if meta.get("run_start"):
        L.append("| run | `%s` .. `%s` (%d run(s) in the manifest) |"
                 % (meta["run_start"], meta.get("run_end") or "end of file", meta.get("n_runs", 1)))
    L.append("| shots dir | `%s` (%d image files, %d not claimed by the manifest) |"
             % (meta["shots_dir"], meta["n_files"], len(meta["unpaired_files"])))
    L.append("| paired | %d of %d manifest entries |" % (meta["n_paired"], len(pairs)))
    L.append("| log | `%s` (%d lines read from %d file(s): %s) |"
             % (meta["log"], log_total, meta.get("n_log_files", 1),
                ", ".join("%s %d" % (c, len(cats.get(c, []))) for c, _ in LOG_CATEGORIES)))
    L.append("| sheets | %s |" % (", ".join("`%s`" % s for s, _ in sheets) or "none"))
    L.append("")
    L.append("Luma is Rec.709 quantised to 8 bit (0..255); mean and percentiles are taken over "
             "that, which is what a display meter reports. `white` = pixels with max(R,G,B) >= %d, "
             "`black` = max(R,G,B) <= %d, `sat` = mean HSV saturation of the non-black pixels. "
             "RGB fractions only for the signal stops (hue bands: red <%.0f/>=345, "
             "amber %.0f..%.0f, green %.0f..%.0f, S>=%.2f V>=%.2f)."
             % (WHITE_CLIP, BLACK_LEVEL, HUE_RED_MAX, HUE_RED_MAX, HUE_AMBER_MAX,
                HUE_GREEN_MIN, HUE_GREEN_MAX, SAT_MIN, VAL_MIN))
    L.append("")

    L.append("## All shots")
    L.append("")
    L.append("| # | time | name | mean | p05 | p50 | p95 | white% | black% | sat | red% | amber% | green% | sheet |")
    L.append("|---|---|---|---|---|---|---|---|---|---|---|---|---|---|")
    sheet_of = {}
    for si, (_name, chunk) in enumerate(sheets, 1):
        for entry, _sf, _m in chunk:
            if entry is not None:
                sheet_of[entry.seq] = si
    for entry, sf, m in pairs:
        sig = is_signal_shot(entry.name, meta["signals"])
        cells = ["%02d" % entry.seq, entry.dt.strftime("%Y-%m-%d %H:%M:%S"), "`%s`" % entry.name]
        if m is None:
            cells += ["missing"] + ["-"] * 10
        else:
            cells += [fmt(m["mean"]), fmt(m["p05"], 0), fmt(m["p50"], 0), fmt(m["p95"], 0),
                      fmt(m["pct_white"], 2), fmt(m["pct_black"], 1), fmt(m["mean_sat"], 3)]
            cells += ([fmt(m["pct_red"], 3), fmt(m["pct_amber"], 3), fmt(m["pct_green"], 3)]
                      if sig else ["-", "-", "-"])
        cells.append("`sheet-%02d`" % sheet_of[entry.seq] if entry.seq in sheet_of else "-")
        L.append("| " + " | ".join(cells) + " |")
    L.append("")

    if events:
        L.append("## Tour events (from the manifest)")
        L.append("")
        for when, text in events:
            L.append("- `%s` %s" % (when.strftime("%H:%M:%S") if when else "??:??", text))
        L.append("")

    if meta["unpaired_files"]:
        L.append("## Shot files the manifest does not claim (manual F12 shots)")
        L.append("")
        for f in meta["unpaired_files"]:
            L.append("- `%s` (%s)" % (f.name, f.dt.strftime("%Y-%m-%d %H:%M:%S")))
        L.append("")

    L.append("## Per shot")
    for entry, sf, m in pairs:
        L.append("")
        L.append("### %02d `%s` - %s" % (entry.seq, entry.name, entry.dt.strftime("%Y-%m-%d %H:%M:%S")))
        L.append("")
        if sf is None:
            L.append("No screenshot file within %.1f s of this manifest line - the shot was "
                     "requested but never landed (shot-watch.sh not running, or the file was "
                     "moved)." % meta["tol"])
            continue
        L.append("file: `%s` (%dx%d, %.2f MB)" % (sf.path, m["width"], m["height"],
                                                  os.path.getsize(sf.path) / 1e6))
        L.append("")
        L.append("| mean | p05 | p50 | p95 | white% | black% | sat |")
        L.append("|---|---|---|---|---|---|---|")
        L.append("| %s | %s | %s | %s | %s | %s | %s |" % (
            fmt(m["mean"]), fmt(m["p05"], 0), fmt(m["p50"], 0), fmt(m["p95"], 0),
            fmt(m["pct_white"], 2), fmt(m["pct_black"], 1), fmt(m["mean_sat"], 3)))
        if is_signal_shot(entry.name, meta["signals"]):
            L.append("")
            L.append("signal pixels: red %s%%, amber %s%%, green %s%%"
                     % (fmt(m["pct_red"], 3), fmt(m["pct_amber"], 3), fmt(m["pct_green"], 3)))
        fl = flags(m)
        if fl:
            L.append("")
            L.append("flags: " + "; ".join(fl))
        L.append("")
        L.append("nearest rhyremix.log lines (window %.0f s):" % meta["window"])
        L.append("")
        got = False
        for name, _rx in LOG_CATEGORIES:
            line, delta = nearest(cats, name, entry.dt, meta["window"])
            if line is None:
                L.append("- %s: none within %.0f s" % (name, meta["window"]))
                continue
            got = True
            L.append("- %s (%+.1f s): `%s`" % (name, delta, line.text))
        if not got and log_total == 0:
            L.append("- the log was empty or missing, so nothing to line up")
    L.append("")
    return "\n".join(L)


# -------------------------------------------------------------------------- main ---
def parse_cell(spec):
    m = re.fullmatch(r"(\d{2,5})x(\d{2,5})", (spec or "").lower())
    if not m:
        raise argparse.ArgumentTypeError("cell must look like 640x360")
    return (int(m.group(1)), int(m.group(2)))


def pick_section(sections, since):
    """Which run `--since` asks for, or None to fall back to filtering single entries.

    First the run whose own lines span `since` - a run ends at its `tour done`, not at the next
    `tour start`, so the gap between two runs belongs to neither of them; otherwise the first run
    that starts at or after it. `--since 00:48` on a manifest whose first run ended at 09:06 on
    2026-11-11 and whose second started at 00:49:09 on 2026-11-12 therefore picks the second.
    """
    for start, end, shots_, evs in sections:
        if start is None:
            continue
        # A run spans its shots and its `tour done` line - not the "waiting" line of the next run,
        # which sits in this section and would otherwise make the gap between runs part of it.
        span = [s.dt for s in shots_] + [start]
        span += [w for w, text in evs if w and TOUR_DONE_RE.search(text)]
        last = max(span)
        if start <= since <= last:
            return (start, end, shots_, evs)
    later = [sec for sec in sections if sec[0] is not None and sec[0] >= since]
    return later[0] if later else None


def main(argv=None):
    ap = argparse.ArgumentParser(prog="tools/tour-report.py",
                                 description="contact sheets + objective report for a photo tour")
    ap.add_argument("game_dir", help="game folder (contains screenshots/ and scripts/)")
    ap.add_argument("--since", help="only manifest entries at/after this time "
                                    "(YYYY-MM-DD HH:MM[:SS] | YYYY-MM-DD | HH:MM | epoch)")
    ap.add_argument("--manifest", help="default GAME_DIR/screenshots/tour-manifest.txt")
    ap.add_argument("--shots-dir", help="default GAME_DIR/screenshots/shots")
    ap.add_argument("--log", help="default GAME_DIR/scripts/rhyremix.log")
    ap.add_argument("--out", help="default GAME_DIR/screenshots/report-<tour start>")
    ap.add_argument("--tol", type=float, default=8.0, help="max |file time - manifest time| (s)")
    ap.add_argument("--window", type=float, default=30.0, help="max |log line - shot| (s)")
    ap.add_argument("--per-sheet", type=int, default=4, help="shots per contact sheet")
    ap.add_argument("--cell", type=parse_cell, default=(640, 360), help="tile size, WxH px")
    ap.add_argument("--signals", choices=("auto", "all", "none"), default="auto",
                    help="which shots get the red/amber/green fractions (default: name says so)")
    ap.add_argument("--no-sheets", action="store_true", help="report.md only")
    args = ap.parse_args(argv)

    game = os.path.abspath(args.game_dir)
    shots_dir = args.shots_dir or os.path.join(game, "screenshots", "shots")
    manifest = args.manifest or os.path.join(game, "screenshots", "tour-manifest.txt")
    logfile = args.log or os.path.join(game, "scripts", "rhyremix.log")
    if not os.path.isdir(game):
        log("tour-report: no such game folder: %s" % game)
        return 2
    if not os.path.isdir(shots_dir):
        log("tour-report: no screenshots dir: %s (run the tour, or pass --shots-dir)" % shots_dir)
        return 2
    if args.per_sheet < 1:
        log("tour-report: --per-sheet must be >= 1")
        return 2

    if os.path.isfile(manifest):
        entries, events = parse_manifest(manifest)
    else:
        entries, events = [], []
        log("tour-report: no manifest at %s - falling back to shot files only" % manifest)

    since = None
    try:
        since = parse_since(args.since, [e.dt for e in entries] or None)
    except ValueError as e:
        log("tour-report: %s" % e)
        return 2
    sections = split_sections(entries, events)
    runs = [sec for sec in sections if sec[0] is not None]
    all_shots = sum(len(sec[2]) for sec in sections)
    run_start = run_end = None
    if since is not None:
        sec = pick_section(sections, since)
        if sec is not None:
            run_start, run_end, sec_shots, sec_events = sec
            tail = [e for e in sec_shots if e.dt >= since]
            if sec_shots and tail and since > sec_shots[0].dt:
                # A cut inside a run keeps that run's tail, and the report directory stays named
                # after the first entry that survived it (what --since did before runs were cut).
                entries, events = tail, [ev for ev in sec_events
                                         if ev[0] is not None and ev[0] >= since]
                run_start = run_end = None
                log("tour-report: --since %s -> the tail of the run started %s: %d of %d entries"
                    % (args.since, sec[0], len(entries), all_shots))
            else:
                # The cut is before the run (or after its last shot): it selects the run itself.
                entries, events = sec_shots, sec_events
                log("tour-report: --since %s -> the run started %s: %d of %d entries, "
                    "%d run(s) in the manifest" % (args.since, run_start, len(entries),
                                                   all_shots, len(runs)))
        else:
            # No run at/after the cut: fall back to filtering entries, which is what a manifest
            # that was cut off mid-run needs.
            entries = [e for e in entries if e.dt >= since]
            events = [ev for ev in events if ev[0] is None or ev[0] >= since]
            log("tour-report: --since %s -> %d entries, no 'tour start' at/after it"
                % (args.since, len(entries)))

    files = shot_files(shots_dir)
    lo, hi = run_start or since, run_end
    if lo is not None or hi is not None:
        files = [f for f in files
                 if (lo is None or f.dt >= lo) and (hi is None or f.dt < hi)]

    if not entries:
        # no manifest (or everything filtered out): report on the files alone, in file order
        entries = [ShotEntry(i + 1, f.name.rsplit(".", 1)[0], f.dt, "")
                   for i, f in enumerate(files)]
    pairs, unpaired_files = pair(entries, files, args.tol)
    paired = [p for p in pairs if p[1] is not None]
    if not paired:
        log("tour-report: nothing to report - %d manifest entries, %d shot files, "
            "no pair within %.1f s" % (len(entries), len(files), args.tol))
        return 1

    start = run_start
    if start is None:
        for when, text in events:
            if when and TOUR_START_RE.search(text):
                start = when
                break
    start = start or paired[0][0].dt
    out_dir = args.out or os.path.join(game, "screenshots", "report-" + start.strftime("%Y-%m-%d_%H-%M-%S"))
    os.makedirs(out_dir, exist_ok=True)

    cats, log_total = load_log(logfile)
    n_log_files = len(log_files(logfile))
    if n_log_files > 1:
        log("tour-report: reading %s plus its rotated %s.prev (n03: the launcher rotates the log "
            "when a session pushes it over its cap, so the run being reported can be the one that "
            "just moved)" % (logfile, logfile))
    measured = []
    for entry, sf in pairs:
        m = metrics(sf.path) if sf is not None else None
        measured.append((entry, sf, m))
    paired_m = [t for t in measured if t[1] is not None]

    sheets = [] if args.no_sheets else make_sheets(paired_m, out_dir, args.per_sheet, args.cell)
    meta = {
        "game": game, "manifest": manifest, "shots_dir": shots_dir, "log": logfile,
        "generated": dt.datetime.now().strftime("%Y-%m-%d %H:%M:%S"),
        "tol": args.tol, "window": args.window, "signals": args.signals,
        "n_files": len(files), "n_paired": len(paired), "unpaired_files": unpaired_files,
        "run_start": run_start.strftime("%Y-%m-%d %H:%M:%S") if run_start else None,
        "run_end": run_end.strftime("%Y-%m-%d %H:%M:%S") if run_end else None,
        "n_runs": len(runs), "n_log_files": n_log_files,
    }
    text = render_report(measured, sheets, events, cats, log_total, meta)
    report = os.path.join(out_dir, "report.md")
    with open(report, "w", encoding="utf-8") as f:
        f.write(text)
    log("tour-report: %s (%d shots paired, %d sheets)" % (report, len(paired), len(sheets)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
