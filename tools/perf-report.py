#!/usr/bin/env python3
"""Summarise a MangoHud CSV the way n08 needs it: is the frame pacing steady, and what is
standing next to the frames that were not?

    usage: tools/perf-report.py CSV [CSV ...]
           [--window 300] [--spike-ms 25] [--top 8] [--series]
           [--gpu-csv nvidia-smi.csv] [--log rhyremix.log] [--t0 'YYYY-MM-DD HH:MM:SS']
           [--label NAME] [--json]

MangoHud writes one row per presented frame (`$G/logs/perf/NvRemixBridge_*.csv`):

    fps,frametime,cpu_load,cpu_power,gpu_load,cpu_temp,gpu_temp,gpu_core_clock,gpu_mem_clock,
    gpu_vram_used,gpu_power,ram_used,swap_used,process_rss,cpu_mhz,elapsed

Two facts about that file decide the tool's shape, both measured on the 2026-11-12 runs:

  * `fps` is not an independent measurement: fps == 1000 / frametime for every row (75.7727 vs
    13.1974 ms). So the frametime column is the signal and fps is its reciprocal - which is why
    the bad tail of fps is the bad tail of frametime read backwards, and why "p1/p0.1 lows" are
    reported for fps and p99/p99.9 for frametime as the same frames seen twice.
  * `gpu_vram_used`, `gpu_load` and `process_rss` are DEAD for this process: one single value
    through the whole run (0.0243416 GB, 0, 4723 KB) in both the 00:48 and the 10:22 CSV, because
    MangoHud is profiling the 32-bit bridge client while the VRAM belongs to NvRemixBridge.exe.
    The tool says "dead column" instead of printing a meaningless number, and takes VRAM from the
    nvidia-smi trace instead (`--gpu-csv`, the `gpu-*.csv` next to the MangoHud file - start one with
    `nvidia-smi --query-gpu=timestamp,memory.used,memory.total,utilization.gpu,temperature.gpu,
    power.draw,clocks.current.sm,clocks_event_reasons.active --format=csv -l 1 > gpu-<stamp>.csv &`).

`elapsed` is nanoseconds since MangoHud started, monotonic, and its span equals the sum of the
frametime column to the millisecond (980.5 s both ways in the 10:22 run, 1458.7 s in the 00:48 one),
so time-bucketing uses elapsed and not a row index. It is NOT the frametime column re-integrated
row by row: a per-row delta differs from that row's frametime by 0.10 ms median and up to 786 ms
(MangoHud stamps the two at different points in the frame), and the unit is ns, not ms - the n14
note in docs/FINDINGS.md read 980501836637/1e6 = 980501 as "not monotonic", which it is. The wall clock comes from
the file name (`NvRemixBridge_2026-11-12_10-22-54.csv`), which is the second MangoHud's log was
created - measured: it is the same second as the plugin's `remixapi_InitializeLibrary` line in
rhyremix.log (00:48:44.501 and 10:22:54.384 for the two runs), so `--log` can line the two up to
better than a second without any clock in the CSV.

What it prints, per file: frames, span, fps mean/median/p1/p0.1, frametime mean/median/stddev/
p99/p99.9/max, spikes over `--spike-ms` as a count and a per-minute rate, and the same numbers for
the first `--window` seconds vs the rest (n08 step 2: "first 5 min vs later"). Two or more files
additionally get a delta table. `--log` adds the per-minute rhyremix.log counters (light creates,
live light count, handle swaps, player-gone) beside the per-minute spike counts, and lists the
nearest log event before each of the `--top` worst frames.
"""
import argparse
import csv
import datetime as dt
import json
import math
import os
import re
import statistics
import sys

# MangoHud's own header line. The file starts with a 2-line system block (os,cpu,gpu,ram,...)
# and the data header is the line that begins with these two names.
DATA_HEADER = "fps,frametime"

# Columns that turned out to be constant in every run we have (see the module docstring).
DEAD_SUSPECTS = ("gpu_vram_used", "gpu_load", "process_rss", "cpu_power", "gpu_power")

TS_RE = re.compile(r"(\d{4})-(\d{2})-(\d{2})[_ ](\d{2})[-:](\d{2})[-:](\d{2})")
LOG_TS_RE = re.compile(r"^\[(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2})\.(\d{3})\]\s*(.*)$")


# ----------------------------------------------------------------------------- parsing

def parse_wallclock(path):
    """Wall clock of the first row, from the file name (second resolution). None if absent."""
    m = TS_RE.search(os.path.basename(path))
    if not m:
        return None
    y, mo, d, hh, mm, ss = (int(x) for x in m.groups())
    return dt.datetime(y, mo, d, hh, mm, ss)


def read_mangohud(path):
    """-> dict with parallel lists: frametime (ms), fps, elapsed (ns), plus the system block."""
    with open(path, encoding="utf-8", errors="replace") as fh:
        lines = fh.read().splitlines()
    heads = [i for i, l in enumerate(lines) if l.startswith(DATA_HEADER)]
    if not heads:
        raise SystemExit("%s: no '%s' header line - not a MangoHud CSV?" % (path, DATA_HEADER))
    hi = heads[-1]
    rows = list(csv.DictReader(lines[hi:]))
    ft, fps, el = [], [], []
    other = {}
    for r in rows:
        try:
            f = float(r["frametime"])
            e = int(r["elapsed"])
        except (TypeError, ValueError):
            continue
        ft.append(f)
        fps.append(float(r["fps"]) if r.get("fps") else 1000.0 / f if f else 0.0)
        el.append(e)
        for k in DEAD_SUSPECTS:
            v = r.get(k)
            if v not in (None, ""):
                other.setdefault(k, set()).add(v)
    if not ft:
        raise SystemExit("%s: header found but no numeric rows" % path)
    span = (el[-1] - el[0]) / 1e9
    return {
        "path": path,
        "label": os.path.basename(path),
        "frametime": ft,
        "fps": fps,
        "elapsed": el,
        "span_s": span,
        "t0": parse_wallclock(path),
        "system": lines[:hi],
        "constants": {k: sorted(v, key=lambda s: float(s)) for k, v in other.items()},
    }


def read_nvidia_smi(path):
    """The launcher's `nvidia-smi --query-gpu=timestamp,memory.used,... -l 1` CSV.
    -> dict(times, mib, util_pct, watts, sm_mhz, throttle_reasons)."""
    with open(path, encoding="utf-8", errors="replace") as fh:
        rows = list(csv.reader(fh))
    if not rows:
        raise SystemExit("%s: empty" % path)
    header = [h.strip() for h in rows[0]]
    idx = {}
    for i, h in enumerate(header):
        low = h.lower()
        if low.startswith("timestamp"):
            idx["ts"] = i
        elif low.startswith("memory.used"):
            idx["mib"] = i
        elif low.startswith("utilization.gpu"):
            idx["util"] = i
        elif low.startswith("power.draw"):
            idx["watts"] = i
        elif low.startswith("clocks.current.sm"):
            idx["sm"] = i
        elif low.startswith("clocks_event_reasons"):
            idx["reason"] = i
    out = {"path": path, "ts": [], "mib": [], "util": [], "watts": [], "sm": [], "reason": []}
    for r in rows[1:]:
        if len(r) <= max(idx.values()):
            continue
        try:
            out["mib"].append(float(r[idx["mib"]].split()[0]))
        except (ValueError, IndexError, KeyError):
            continue
        for key in ("ts", "util", "watts", "sm", "reason"):
            if key in idx:
                out[key].append(r[idx[key]].strip())
    if not out["mib"]:
        raise SystemExit("%s: no memory.used rows" % path)
    return out


def read_rhyremix_log(path):
    """Split rhyremix.log into runs ('---- log starts ----' / 'rhyremix start') and pull the
    per-line counters n08 cares about out of each line. -> list of run dicts."""
    runs = []
    cur = None
    with open(path, encoding="utf-8", errors="replace") as fh:
        for raw in fh:
            if raw.startswith("---- log starts"):
                cur = None
                continue
            m = LOG_TS_RE.match(raw.rstrip("\n"))
            if not m:
                continue
            stamp, _ms, body = m.groups()
            when = dt.datetime.strptime(stamp, "%Y-%m-%d %H:%M:%S")
            if body.startswith("rhyremix start"):
                cur = {"start": when, "events": [], "end": None}
                runs.append(cur)
            if cur is None:
                continue
            kind = classify_log_line(body)
            if kind:
                cur["events"].append({"t": when, "kind": kind, "text": body,
                                      "live": _first_int(r"live=(\d+)", body),
                                      "creates": _first_int(r"creates_this_frame=(\d+)", body),
                                      "pending": _first_int(r"pending_destroy=(\d+)", body),
                                      "due": _first_int(r"due_destroy=(\d+)", body),
                                      "deferred": _first_int(r"deferred_destroy=(\d+)", body),
                                      "new": _first_int(r"new=(\d+)", body),
                                      "retired": _first_int(r"retired=(\d+)", body),
                                      "gone": _first_int(r"player_gone_frames=(\d+)", body)})
            cur["end"] = when
    return runs


CREATE_RE = re.compile(r"CreateLight ok")


def classify_log_line(body):
    """One label per interesting plugin line; None for chatter we do not want to count."""
    sub = body.split(":", 1)[0].strip() if ":" in body else ""
    if CREATE_RE.search(body):
        if "streetlights" in body:
            return "create:street"
        if "trafficlights" in body:
            return "create:signal"
        if body.startswith("sun:"):
            return "handle:sun"
        if body.startswith("moon:"):
            return "handle:moon"
        if "vehicle" in body:
            return "create:vehicle"
        return "create:" + (sub or "?")
    if body.startswith("remix: live="):
        return "heartbeat"
    if body.startswith("streetlights:"):
        return "status:street"
    if body.startswith("trafficlights"):
        return "status:signal"
    if body.startswith("sky:"):
        return "status:sky"
    if body.startswith("signalhook:"):
        return "signalhook"
    if "fading" in body and "lights out" in body:
        return "fade-out"
    if "create_fail" in body and re.search(r"create_fail=[1-9]", body):
        return "remix-fail"
    if body.startswith("config:") or body.startswith("clock:") or body.startswith("rtxconf:"):
        return "config"
    return None


def _first_int(pattern, text):
    m = re.search(pattern, text)
    return int(m.group(1)) if m else None


# ----------------------------------------------------------------------------- statistics

def percentile(sorted_vals, q):
    """Linear-interpolated percentile, q in 0..1 (numpy's default, so the numbers here match
    what numpy would have given - and this stays dependency-free)."""
    if not sorted_vals:
        return float("nan")
    if len(sorted_vals) == 1:
        return sorted_vals[0]
    pos = (len(sorted_vals) - 1) * q
    lo = math.floor(pos)
    hi = math.ceil(pos)
    if lo == hi:
        return sorted_vals[lo]
    return sorted_vals[lo] + (sorted_vals[hi] - sorted_vals[lo]) * (pos - lo)


def stats(frametime, fps=None):
    """The n08 metric set for one slice of a run."""
    if fps is None:
        fps = [1000.0 / f for f in frametime if f > 0]
    ft = sorted(frametime)
    fp = sorted(fps)
    n = len(ft)
    out = {
        "frames": n,
        "ft_mean": statistics.fmean(ft),
        "ft_median": percentile(ft, 0.5),
        "ft_stddev": statistics.stdev(ft) if n > 1 else 0.0,
        "ft_p99": percentile(ft, 0.99),
        "ft_p999": percentile(ft, 0.999),
        "ft_max": ft[-1],
        "fps_mean": statistics.fmean(fps),
        "fps_median": percentile(fp, 0.5),
        "fps_p1": percentile(fp, 0.01),
        "fps_p01": percentile(fp, 0.001),
        "fps_min": fp[0],
        "fps_max": fp[-1],
    }
    return out


def spike_list(run, spike_ms):
    """Frames slower than spike_ms, as (elapsed_s, frametime_ms) pairs."""
    out = []
    base = run["elapsed"][0]
    for e, f in zip(run["elapsed"], run["frametime"]):
        if f > spike_ms:
            out.append(((e - base) / 1e9, f))
    return out


def slice_run(run, lo_s, hi_s):
    """A copy of the run limited to lo_s <= elapsed < hi_s (seconds from the first row)."""
    base = run["elapsed"][0]
    lo, hi = base + lo_s * 1e9, base + hi_s * 1e9
    ft, fps, el = [], [], []
    for e, f, p in zip(run["elapsed"], run["frametime"], run["fps"]):
        if lo <= e < hi:
            el.append(e)
            ft.append(f)
            fps.append(p)
    if not ft:
        return None
    return {"path": run["path"], "label": run["label"], "frametime": ft, "fps": fps,
            "elapsed": el, "span_s": (el[-1] - el[0]) / 1e9, "t0": run["t0"],
            "constants": run["constants"]}


def windows(run, window_s):
    """[('whole', run), ('first Ns', ...), ('after Ns', ...)] - n08's 'first 5 min vs later'."""
    out = [("whole run", run)]
    if run["span_s"] > window_s:
        first = slice_run(run, 0.0, window_s)
        later = slice_run(run, window_s, run["span_s"] + 1.0)
        if first:
            out.append(("first %d s" % window_s, first))
        if later:
            out.append(("after %d s" % window_s, later))
    return out


def per_minute(run, spike_ms):
    """[(minute, frames, fps_mean, ft_mean, spikes)] over the whole run."""
    base = run["elapsed"][0]
    buckets = {}
    for e, f in zip(run["elapsed"], run["frametime"]):
        m = int((e - base) // 60e9)
        b = buckets.setdefault(m, {"frames": 0, "ft": 0.0, "spikes": 0})
        b["frames"] += 1
        b["ft"] += f
        if f > spike_ms:
            b["spikes"] += 1
    rows = []
    for m in sorted(buckets):
        b = buckets[m]
        rows.append((m, b["frames"], 1000.0 * b["frames"] / max(b["ft"], 1e-9),
                     b["ft"] / b["frames"], b["spikes"]))
    return rows


def log_per_minute(logrun, t0):
    """{minute: {kind: count}} plus the max live light count per minute, aligned to the CSV.

    n08: `pending`/`due` are the two counters that found the unbounded destroy flush (the
    2026-11-12 runs reached 320 and 205 handles pending, all of them released inside one
    Present). `due` only exists in the n08 plugin build; an older log leaves it at 0."""
    if t0 is None or not logrun:
        return {}
    out = {}
    for ev in logrun["events"]:
        sec = (ev["t"] - t0).total_seconds()
        if sec < 0:
            continue
        m = int(sec // 60)
        b = out.setdefault(m, {"count": {}, "live": 0, "creates": 0, "gone": 0,
                               "pending": 0, "due": 0})
        b["count"][ev["kind"]] = b["count"].get(ev["kind"], 0) + 1
        if ev["live"] is not None:
            b["live"] = max(b["live"], ev["live"])
        if ev["creates"]:
            b["creates"] += ev["creates"]
        if ev.get("pending"):
            b["pending"] = max(b["pending"], ev["pending"])
        if ev.get("due"):
            b["due"] = max(b["due"], ev["due"])
        if ev["gone"]:
            b["gone"] = max(b["gone"], ev["gone"])
    return out


def pick_log_run(logruns, t0, window_s=180.0):
    """The log block that belongs to this CSV: the last 'rhyremix start' at or before t0+2s."""
    if t0 is None or not logruns:
        return None
    best = None
    for r in logruns:
        delta = (t0 - r["start"]).total_seconds()
        if -2.0 <= delta <= window_s and (best is None or delta < (t0 - best["start"]).total_seconds()):
            best = r
    return best


def nearest_event(spikes_s, logrun, t0):
    """The last log event at or before each spike's wall clock, in the order the spikes were
    asked for (None if the log is unusable). The sweep walks the spikes in time order, so the
    answer is keyed by offset rather than by position."""
    if not logrun or t0 is None:
        return [None] * len(spikes_s)
    evs = sorted(logrun["events"], key=lambda e: e["t"])
    near = {}
    i = 0
    last = None
    for s in sorted(spikes_s):
        target = t0 + dt.timedelta(seconds=s)
        while i < len(evs) and evs[i]["t"] <= target:
            last = evs[i]
            i += 1
        near[s] = last
    return [near[s] for s in spikes_s]


def pearson(xs, ys):
    """Correlation coefficient, None when a series is flat (a flat series explains nothing)."""
    n = len(xs)
    if n < 3 or n != len(ys):
        return None
    mx = statistics.fmean(xs)
    my = statistics.fmean(ys)
    sxx = sum((x - mx) ** 2 for x in xs)
    syy = sum((y - my) ** 2 for y in ys)
    if sxx <= 0 or syy <= 0:
        return None
    return sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / math.sqrt(sxx * syy)


# ----------------------------------------------------------------------------- reporting

def parse_smi_ts(text):
    """'2026/11/12 10:22:47.243' -> datetime (nvidia-smi's own format)."""
    if not text:
        return None
    for fmt in ("%Y/%m/%d %H:%M:%S.%f", "%Y/%m/%d %H:%M:%S"):
        try:
            return dt.datetime.strptime(text, fmt)
        except ValueError:
            continue
    return None


def gpu_for_run(gpus, run):
    """The nvidia-smi trace that overlaps this CSV's wall clock (n08: only the 10:22 run has
    one - the 00:47 trace died five rows in, before the game even launched, and its 15825 MiB
    belongs to whatever held the GPU before, not to this run)."""
    if not gpus:
        return None
    if run["t0"] is None:
        return gpus[0] if len(gpus) == 1 else None
    start, stop = run["t0"], run["t0"] + dt.timedelta(seconds=run["span_s"])
    best = None
    for g in gpus:
        a = parse_smi_ts(g["ts"][0]) if g.get("ts") else None
        b = parse_smi_ts(g["ts"][-1]) if g.get("ts") else None
        if a is None or b is None or b < start or a > stop:
            continue
        overlap = (min(b, stop) - max(a, start)).total_seconds()
        g["coverage"] = overlap / max(run["span_s"], 1e-9)
        if best is None or g["coverage"] > best["coverage"]:
            best = g
    return best


def fmt(v, places=2):
    if v is None or (isinstance(v, float) and math.isnan(v)):
        return "-"
    return ("%." + str(places) + "f") % v


def metric_rows(run, spike_ms, window_s):
    rows = []
    for label, part in windows(run, window_s):
        st = stats(part["frametime"], part["fps"])
        sp = spike_list(part, spike_ms)
        minutes = part["span_s"] / 60.0
        st["window"] = label
        st["span_s"] = part["span_s"]
        st["spikes"] = len(sp)
        st["spikes_per_min"] = (len(sp) / minutes) if minutes > 0 else 0.0
        st["over_2x"] = sum(1 for _t, f in sp if f > 2 * spike_ms)
        rows.append(st)
    return rows


COLUMNS = [
    ("frames", "frames", 0),
    ("span_s", "span s", 1),
    ("fps_mean", "fps mean", 2),
    ("fps_median", "fps med", 2),
    ("fps_p1", "fps p1", 2),
    ("fps_p01", "fps p0.1", 2),
    ("ft_mean", "ft mean ms", 3),
    ("ft_median", "ft med", 3),
    ("ft_stddev", "ft sd", 3),
    ("ft_p99", "ft p99", 2),
    ("ft_p999", "ft p99.9", 2),
    ("ft_max", "ft max", 1),
    ("spikes", "spikes", 0),
    ("spikes_per_min", "spikes/min", 2),
]


def columns(spike_ms):
    """COLUMNS with the spike threshold spelled out in the header."""
    out = []
    for key, title, places in COLUMNS:
        if key == "spikes":
            title = ">%g ms" % spike_ms
        out.append((key, title, places))
    return out


def print_run(run, spike_ms, window_s, top, gpus, logruns, series):
    gpu = gpu_for_run(gpus, run)
    print("== %s" % run["label"])
    if run["t0"]:
        print("   starts %s (from the file name), span %.1f s, %d frames, %.1f frames/s"
              % (run["t0"].strftime("%Y-%m-%d %H:%M:%S"), run["span_s"], len(run["frametime"]),
                 len(run["frametime"]) / max(run["span_s"], 1e-9)))
    else:
        print("   no wall clock in the file name; span %.1f s, %d frames"
              % (run["span_s"], len(run["frametime"])))
    rows = metric_rows(run, spike_ms, window_s)
    cols = columns(spike_ms)
    head = "%-14s" % "window" + "".join("%12s" % title for _k, title, _p in cols)
    print(head)
    for r in rows:
        print("%-14s" % r["window"] + "".join("%12s" % fmt(r[k], p) for k, _t, p in cols))

    # vram: the honest answer about the MangoHud column, then nvidia-smi if we have it
    for key in ("gpu_vram_used", "gpu_load", "process_rss"):
        vals = run["constants"].get(key) or []
        if len(vals) <= 1:
            print("   %-14s dead column: one value (%s) for the whole run - MangoHud in the "
                  "32-bit bridge client cannot read it" % (key, vals[0] if vals else "n/a"))
        else:
            print("   %-14s %s .. %s (%d distinct)" % (key, vals[0], vals[-1], len(vals)))
    if gpu:
        mib = gpu["mib"]
        print("   nvidia-smi   %s: %d samples, VRAM used mean %s MiB, peak %s MiB, "
              "first->last %s -> %s MiB (%.0f%% of this run covered)"
              % (os.path.basename(gpu["path"]), len(mib), fmt(statistics.fmean(mib), 0),
                 fmt(max(mib), 0), fmt(mib[0], 0), fmt(mib[-1], 0),
                 100.0 * gpu.get("coverage", 1.0)))
        if gpu.get("ts"):
            print("                window %s .. %s" % (gpu["ts"][0], gpu["ts"][-1]))
        reasons = {}
        for r in gpu.get("reason", []):
            reasons[r] = reasons.get(r, 0) + 1
        if len(reasons) > 1:
            print("                clock event reasons: " +
                  ", ".join("%s x%d" % (k, v) for k, v in sorted(reasons.items(),
                                                                 key=lambda kv: -kv[1])))
    elif gpus:
        print("   nvidia-smi   none of the %d --gpu-csv traces overlap %s (that run has no VRAM "
              "source: its gpu-*.csv stopped before the game launched)"
              % (len(gpus), run["t0"] if run["t0"] else run["label"]))

    if series:
        print("   per minute: minute frames fps_mean ft_mean spikes")
        for m, frames, fpsm, ftm, sp in per_minute(run, spike_ms):
            print("     %3d %8d %9s %9s %6d" % (m, frames, fmt(fpsm, 1), fmt(ftm, 2), sp))

    if top:
        sp = spike_list(run, spike_ms)
        sp.sort(key=lambda x: -x[1])
        near = nearest_event([s for s, _f in sp[:top]], pick_log_run(logruns, run["t0"]),
                             run["t0"])
        print("   worst %d frames over %g ms:" % (min(top, len(sp)), spike_ms))
        for (s, f), ev in zip(sp[:top], near):
            evtxt = ""
            if ev:
                evtxt = "  <- %s %s" % (ev["t"].strftime("%H:%M:%S"),
                                        ev["text"][:78].replace("\n", " "))
            print("     %8.1f s  %8.1f ms%s" % (s, f, evtxt))

    if logruns:
        lr = pick_log_run(logruns, run["t0"])
        if lr is None:
            print("   --log: no 'rhyremix start' block matches %s" % run["t0"])
        else:
            lm = log_per_minute(lr, run["t0"])
            pm = {m: sp for m, _f, _fps, _ft, sp in per_minute(run, spike_ms)}
            kinds = sorted({k for b in lm.values() for k in b["count"]})
            keep = [k for k in kinds if k.startswith("create") or k.startswith("handle")
                    or k == "fade-out" or k == "remix-fail"]
            print("   log block %s .. %s; per minute (spikes vs plugin events)"
                  % (lr["start"].strftime("%H:%M:%S"),
                     lr["end"].strftime("%H:%M:%S") if lr["end"] else "?"))
            print("     minute spikes live creates pending " +
                  " ".join("%14s" % k for k in keep))
            mins = sorted(set(list(lm) + list(pm)))
            spk, live, cre, pend = [], [], [], []
            for m in mins:
                b = lm.get(m, {"count": {}, "live": 0, "creates": 0, "pending": 0})
                print("     %6d %6d %6s %8s %7s " % (m, pm.get(m, 0), b["live"], b["creates"],
                                                     b["pending"]) +
                      " ".join("%14d" % b["count"].get(k, 0) for k in keep))
                spk.append(pm.get(m, 0))
                live.append(b["live"])
                cre.append(b["creates"])
                pend.append(b["pending"])
            r_live = pearson(spk, live)
            r_cre = pearson(spk, cre)
            r_pend = pearson(spk, pend)
            print("     r(spikes/min, live lights) = %s   r(spikes/min, creates) = %s   "
                  "r(spikes/min, handles pending destroy) = %s"
                  % (fmt(r_live, 2), fmt(r_cre, 2), fmt(r_pend, 2)))
            print("     worst plugin frame of the run: live=%d creates=%d pending_destroy=%d"
                  % (max(live) if live else 0, max(cre) if cre else 0,
                     max(pend) if pend else 0))


def print_compare(runs, spike_ms, window_s):
    """A/B delta table: same metric, one row per metric, one column per window."""
    a, b = runs[0], runs[1]
    ra, rb = metric_rows(a, spike_ms, window_s), metric_rows(b, spike_ms, window_s)
    print("== A/B: %s" % a["label"])
    print("   vs  %s" % b["label"])
    print("%-24s %14s %14s %12s" % ("window metric", "A", "B", "B - A"))
    for key, title, places in columns(spike_ms):
        for row_a, row_b in zip(ra, rb):
            if row_a["window"] != row_b["window"]:
                continue
            d = row_b[key] - row_a[key]
            print("%-24s %14s %14s %12s"
                  % (row_a["window"] + " " + title,
                     fmt(row_a[key], places), fmt(row_b[key], places),
                     ("+" if d > 0 else "") + fmt(d, places)))


def as_json(runs, spike_ms, window_s, gpus, logruns):
    out = []
    for run in runs:
        entry = {"path": run["path"], "label": run["label"],
                 "t0": run["t0"].isoformat() if run["t0"] else None,
                 "span_s": run["span_s"], "frames": len(run["frametime"]),
                 "windows": metric_rows(run, spike_ms, window_s),
                 "dead_columns": [k for k, v in run["constants"].items() if len(v) <= 1],
                 "per_minute": per_minute(run, spike_ms)}
        g = gpu_for_run(gpus, run)
        if g:
            entry["vram_mib"] = {"mean": statistics.fmean(g["mib"]), "peak": max(g["mib"]),
                                 "first": g["mib"][0], "last": g["mib"][-1],
                                 "samples": len(g["mib"])}
        lr = pick_log_run(logruns, run["t0"]) if logruns else None
        if lr:
            entry["log_block"] = {"start": lr["start"].isoformat(),
                                  "events": len(lr["events"])}
            entry["log_per_minute"] = {str(k): v for k, v in log_per_minute(lr, run["t0"]).items()}
        out.append(entry)
    return json.dumps({"runs": out, "spike_ms": spike_ms, "window_s": window_s}, indent=1)


def main(argv=None):
    ap = argparse.ArgumentParser(prog="tools/perf-report.py",
                                 description=__doc__.splitlines()[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("csv", nargs="+", help="MangoHud CSV(s); two or more get an A/B table")
    ap.add_argument("--window", type=float, default=300.0,
                    help="seconds of the 'first N s' split (default 300 = 5 min)")
    ap.add_argument("--spike-ms", type=float, default=25.0,
                    help="a frame slower than this is a spike (default 25 ms)")
    ap.add_argument("--top", type=int, default=8, help="worst frames to list (0 = none)")
    ap.add_argument("--series", action="store_true", help="print the per-minute series")
    ap.add_argument("--gpu-csv", action="append", default=[],
                    help="nvidia-smi CSV from the same run (VRAM, clocks); repeatable, matched "
                         "to each CSV by wall clock")
    ap.add_argument("--log", help="rhyremix.log; aligns plugin events to the CSV clock")
    ap.add_argument("--t0", help="override the CSV wall clock, 'YYYY-MM-DD HH:MM:SS'")
    ap.add_argument("--json", action="store_true", help="machine-readable output")
    args = ap.parse_args(argv)

    runs = []
    for path in args.csv:
        r = read_mangohud(path)
        if args.t0:
            r["t0"] = dt.datetime.strptime(args.t0, "%Y-%m-%d %H:%M:%S")
        runs.append(r)
    gpu = [read_nvidia_smi(p) for p in args.gpu_csv]
    logruns = read_rhyremix_log(args.log) if args.log else []

    if args.json:
        print(as_json(runs, args.spike_ms, args.window, gpu, logruns))
        return 0
    for r in runs:
        print_run(r, args.spike_ms, args.window, args.top, gpu, logruns, args.series)
        print()
    if len(runs) >= 2:
        print_compare(runs, args.spike_ms, args.window)
    return 0


if __name__ == "__main__":
    sys.exit(main())
