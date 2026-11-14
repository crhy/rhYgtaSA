#!/usr/bin/env python3
"""n08 gate for tools/perf-report.py: the numbers that finding G was decided on.

finding G of the 2026-11-12 10:22 report is "FPS a little less steady than previous builds".
Steadiness is a statement about the TAIL, and a tail is easy to get wrong, so this file feeds
the tool synthetic MangoHud CSVs whose answer is known exactly and checks what it prints:

  * spike counting against --spike-ms, as a count and a per-minute rate (the two runs of
    2026-11-12: 1869/76.9 per min against 2242/137.2 per min);
  * the "first window vs after it" split, which is where the regression actually lives
    (24.0 -> 22.4 spikes/min in the first 300 s, 90.6 -> 187.8 after it);
  * percentiles in the right direction, and fps read as the reciprocal of frametime (the
    real CSVs satisfy fps == 1000/frametime for every row, so p1/p0.1 of fps and p99/p99.9
    of frametime are the same frames seen twice);
  * the dead-column report: gpu_vram_used / gpu_load / process_rss are constant in the real
    files because MangoHud profiles the 32-bit bridge client, so VRAM has to come from the
    launcher's nvidia-smi trace instead;
  * the nvidia-smi trace match by wall clock (the 00:47 trace stops before the game launches,
    so the 00:48 run must get NO VRAM numbers rather than wrong ones);
  * the rhyremix.log alignment and its counters, including the n08 ones (pending_destroy /
    due_destroy / deferred_destroy) that found the unbounded destroy flush.

Run: python3 -m pytest -q tests/test_perf_report.py   (or python3 -m unittest)
"""
import datetime as dt
import json
import os
import subprocess
import sys
import tempfile
import unittest

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOL = os.path.join(REPO, "tools", "perf-report.py")

HEADER = ("fps,frametime,cpu_load,cpu_power,gpu_load,cpu_temp,gpu_temp,gpu_core_clock,"
          "gpu_mem_clock,gpu_vram_used,gpu_power,ram_used,swap_used,process_rss,cpu_mhz,"
          "elapsed")


def write_mangohud(path, frametimes, vram=None, load=None, rss=None, t_start_ns=0):
    """A MangoHud CSV shaped like the real one: a system block, the data header, one row per
    frame. fps is written as 1000/frametime because that is what the real files contain."""
    vram = vram if vram is not None else 0.0243416
    load = load if load is not None else 0
    rss = rss if rss is not None else 4723
    lines = [
        "os,Linux 6.8",
        "cpu,AMD Ryzen 7 5800X3D",
        "gpu,NVIDIA GeForce RTX 4070 Ti SUPER",
        "ram,32768 MB",
        HEADER,
    ]
    el = t_start_ns
    for f in frametimes:
        fps = 1000.0 / f if f else 0.0
        lines.append("%.4f,%.4f,0,0,%d,0,0,2505,15001,%.7f,0,12000,0,%d,3800,%d"
                     % (fps, f, load, vram, rss, el))
        el += int(f * 1e6)
    with open(path, "w", encoding="utf-8") as fh:
        fh.write("\n".join(lines) + "\n")


def steady(n, ms=16.67):
    return [ms] * n


def with_spikes(n, ms, spikes, big=60.0, at=None):
    """n frames of `ms`, `spikes` of them replaced by `big` ms. By default they are spread
    through the run; `at` is an explicit list of frame indices instead."""
    out = [ms] * n
    if at is not None:
        for i in at:
            out[i] = big
    elif spikes:
        step = max(1, n // spikes)
        for i in range(0, min(n, spikes * step), step):
            out[i] = big
    return out


def run_tool(args, expect=0):
    proc = subprocess.run([sys.executable, TOOL] + args, capture_output=True, text=True)
    if expect is not None and proc.returncode != expect:
        raise AssertionError("tool exited %d (want %s)\n%s\n%s"
                             % (proc.returncode, expect, proc.stdout, proc.stderr))
    return proc.stdout


class TestPerfReport(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = self.tmp.name

    def tearDown(self):
        self.tmp.cleanup()

    def csv(self, name, frametimes, **kw):
        path = os.path.join(self.dir, name)
        write_mangohud(path, frametimes, **kw)
        return path

    # ---------------------------------------------------------------- the tail

    def test_spike_count_and_rate(self):
        """2400 frames at 60 fps = 40 s. 40 frames over 25 ms in B, 4 in A."""
        a = self.csv("NvRemixBridge_2026-11-12_00-00-00.csv",
                     with_spikes(2400, 16.67, 4, big=40.0))
        b = self.csv("NvRemixBridge_2026-11-12_10-00-00.csv",
                     with_spikes(2400, 16.67, 40, big=40.0))
        out = json.loads(run_tool([a, b, "--json", "--top", "0"]))
        ra, rb = out["runs"]
        self.assertEqual(ra["frames"], 2400)
        self.assertEqual(rb["frames"], 2400)
        whole_a = [w for w in ra["windows"] if w["window"] == "whole run"][0]
        whole_b = [w for w in rb["windows"] if w["window"] == "whole run"][0]
        self.assertEqual(whole_a["spikes"], 4)
        self.assertEqual(whole_b["spikes"], 40)
        # 40 s of run = 2/3 of a minute, so the rate is spikes / (40/60)
        self.assertAlmostEqual(whole_b["spikes_per_min"], 40 * 60.0 / whole_b["span_s"], places=3)
        self.assertGreater(whole_b["spikes_per_min"], whole_a["spikes_per_min"])
        # the mean barely moves while the tail doubles: that IS finding G
        self.assertLess(abs(whole_b["ft_mean"] - whole_a["ft_mean"]), 1.0)
        self.assertGreater(whole_b["ft_stddev"], whole_a["ft_stddev"])
        self.assertGreater(whole_b["ft_p99"], whole_a["ft_p99"])

    def test_window_split_first_vs_after(self):
        """The regression lives after the first window: 2 spikes early, 38 late - which is
        exactly how the 2026-11-12 runs differ (90.6 against 187.8 spikes/min after 300 s)."""
        late = list(range(3600, 6000, 63))          # all after the first 60 s
        early = [10, 200]
        a = self.csv("NvRemixBridge_2026-11-12_00-00-00.csv",
                     with_spikes(6000, 16.67, 0, big=90.0, at=early + late))
        out = json.loads(run_tool([a, "--json", "--window", "60", "--top", "0"]))
        rows = {w["window"]: w for w in out["runs"][0]["windows"]}
        self.assertIn("first 60 s", rows)
        self.assertIn("after 60 s", rows)
        self.assertEqual(rows["first 60 s"]["spikes"], len(early))
        self.assertEqual(rows["after 60 s"]["spikes"], len(late))
        self.assertAlmostEqual(rows["first 60 s"]["span_s"], 60.0, places=1)
        self.assertLess(rows["first 60 s"]["spikes_per_min"],
                        rows["after 60 s"]["spikes_per_min"])

    def test_percentiles_are_ordered(self):
        a = self.csv("NvRemixBridge_2026-11-12_00-00-00.csv",
                     with_spikes(3000, 16.67, 60, big=100.0))
        out = json.loads(run_tool([a, "--json", "--top", "0"]))
        w = [x for x in out["runs"][0]["windows"] if x["window"] == "whole run"][0]
        self.assertAlmostEqual(w["ft_median"], 16.67, places=2)
        self.assertLess(w["ft_median"], w["ft_p99"])
        self.assertLessEqual(w["ft_p99"], w["ft_p999"])
        self.assertLessEqual(w["ft_p999"], w["ft_max"])
        self.assertEqual(w["ft_max"], 100.0)

    def test_percentiles_rank_a_graded_tail(self):
        """Same file shape, tail written out explicitly so the ordering is checkable."""
        frames = [16.67] * 3000
        for i in range(0, 3000, 75):        # 40 frames at 40 ms
            frames[i] = 40.0
        for i in range(30, 3000, 158):      # 19 frames at 100 ms
            frames[i] = 100.0
        frames[1500] = 786.8                # one 2026-11-12 style hitch
        a = self.csv("NvRemixBridge_2026-11-12_00-00-00.csv", frames)
        out = json.loads(run_tool([a, "--json", "--top", "0"]))
        w = [x for x in out["runs"][0]["windows"] if x["window"] == "whole run"][0]
        self.assertAlmostEqual(w["ft_median"], 16.67, places=2)
        self.assertLess(w["ft_p99"], w["ft_p999"])
        self.assertLess(w["ft_p999"], w["ft_max"])
        self.assertEqual(w["ft_max"], 786.8)
        # fps is the reciprocal of frametime, so its low points are the same frames
        self.assertLessEqual(w["fps_p01"], w["fps_p1"])
        self.assertLess(w["fps_p1"], w["fps_mean"])

    def test_spike_threshold_is_the_flag(self):
        """--spike-ms decides what counts: the same file, two thresholds, two answers."""
        a = self.csv("NvRemixBridge_2026-11-12_00-00-00.csv",
                     with_spikes(1200, 16.67, 20, big=40.0))
        lo = json.loads(run_tool([a, "--json", "--spike-ms", "25", "--top", "0"]))
        hi = json.loads(run_tool([a, "--json", "--spike-ms", "50", "--top", "0"]))
        wl = [x for x in lo["runs"][0]["windows"] if x["window"] == "whole run"][0]
        wh = [x for x in hi["runs"][0]["windows"] if x["window"] == "whole run"][0]
        self.assertEqual(wl["spikes"], 20)
        self.assertEqual(wh["spikes"], 0)

    # ---------------------------------------------------------------- dead columns

    def test_dead_vram_column_is_reported_not_graphed(self):
        """gpu_vram_used constant through the run = the MangoHud client cannot read it."""
        a = self.csv("NvRemixBridge_2026-11-12_00-00-00.csv", steady(600))
        out = json.loads(run_tool([a, "--json", "--top", "0"]))
        dead = out["runs"][0]["dead_columns"]
        for key in ("gpu_vram_used", "gpu_load", "process_rss"):
            self.assertIn(key, dead)
        text = run_tool([a, "--top", "0"])
        self.assertIn("dead column", text)

    def test_vram_from_nvidia_smi_only_when_it_overlaps(self):
        """A gpu-*.csv that stops before the run starts must not be used (the real 00:47 trace
        died five rows in, before the game launched)."""
        a = self.csv("NvRemixBridge_2026-11-12_00-48-44.csv", steady(6000))   # 100 s
        b = self.csv("NvRemixBridge_2026-11-12_10-22-54.csv", steady(6000))   # 100 s
        gpu = os.path.join(self.dir, "gpu-20261112-102247.csv")
        with open(gpu, "w", encoding="utf-8") as fh:
            fh.write("timestamp, memory.used [MiB], memory.total [MiB], utilization.gpu [%], "
                     "temperature.gpu, power.draw [W], clocks.current.sm [MHz], "
                     "clocks_event_reasons.active\n")
            t0 = dt.datetime(2026, 11, 12, 10, 22, 47)
            # nvidia-smi's own format: slashes in the date, units after the numbers, one
            # sample per second (the launcher runs `nvidia-smi --query-gpu=... -l 1`)
            for i in range(100):
                when = t0 + dt.timedelta(seconds=i)
                fh.write("%s.%03d, %d MiB, 16376 MiB, %d %%, 46, 160.00 W, 2790 MHz, "
                         "0x0000000000000000\n"
                         % (when.strftime("%Y/%m/%d %H:%M:%S"), i % 1000,
                            3500 + 5 * i, 40 + i % 20))
        out = json.loads(run_tool([a, b, "--json", "--gpu-csv", gpu, "--top", "0"]))
        ra, rb = out["runs"]
        self.assertNotIn("vram_mib", ra)          # 00:48 run: no overlapping trace
        self.assertIn("vram_mib", rb)            # 10:22 run: the trace covers it
        self.assertEqual(rb["vram_mib"]["samples"], 100)
        self.assertEqual(rb["vram_mib"]["peak"], 3500 + 5 * 99)
        self.assertAlmostEqual(rb["vram_mib"]["mean"],
                               sum(3500 + 5 * i for i in range(100)) / 100.0, places=3)
        text = run_tool([a, b, "--gpu-csv", gpu, "--top", "0"])
        self.assertIn("no VRAM", text)

    # ---------------------------------------------------------------- the log

    def test_log_block_matching_and_counters(self):
        """Two plugin runs in one log; the CSV must pick its own block, and the n08 destroy
        counters must come through (they are what found the unbounded flush)."""
        a = self.csv("NvRemixBridge_2026-11-12_00-48-44.csv", steady(6000))
        log = os.path.join(self.dir, "rhyremix.log")
        with open(log, "w", encoding="utf-8") as fh:
            fh.write("---- log starts 2026-11-12 00:48:33 ----\n")
            fh.write("[2026-11-12 00:48:44.501] rhyremix start 0.1\n")
            for m in range(3):
                fh.write("[2026-11-12 00:%02d:50.000] remix: live=%d create_fail=0 draw_fail=0 "
                         "budget_refused=0 | pending_destroy=%d\n" % (48 + m, 300 + m, 40 * m))
            fh.write("---- log starts 2026-11-12 10:22:46 ----\n")
            fh.write("[2026-11-12 10:22:54.384] rhyremix start 0.1\n")
            fh.write("[2026-11-12 10:23:10.000] remix: live=321 create_fail=0 draw_fail=0 "
                     "budget_refused=0 | pending_destroy=205 due_destroy=173 deferred_destroy="
                     "173 creates_this_frame=24 deferred=0 | player: lost_events=0 gone_ms=0\n")
            fh.write("[2026-11-12 10:23:20.000] streetlights: 320 active, 7002 in list\n")
        out = json.loads(run_tool([a, "--json", "--log", log, "--top", "0"]))
        block = out["runs"][0]["log_block"]
        self.assertTrue(block["start"].startswith("2026-11-12T00:48:44"))
        per = out["runs"][0]["log_per_minute"]
        self.assertEqual(per["0"]["live"], 300)
        self.assertEqual(per["2"]["pending"], 80)

        b = self.csv("NvRemixBridge_2026-11-12_10-22-54.csv", steady(6000))
        out = json.loads(run_tool([b, "--json", "--log", log, "--top", "0"]))
        block = out["runs"][0]["log_block"]
        self.assertTrue(block["start"].startswith("2026-11-12T10:22:54"))
        per = out["runs"][0]["log_per_minute"]
        self.assertEqual(per["0"]["live"], 321)
        self.assertEqual(per["0"]["pending"], 205)
        self.assertEqual(per["0"]["due"], 173)
        self.assertEqual(per["0"]["creates"], 24)

    def test_worst_frames_are_attributed_to_a_log_event(self):
        # one 786.8 ms frame at +15 s, the 2026-11-12 runs' worst frame of both runs
        a = self.csv("NvRemixBridge_2026-11-12_00-48-44.csv",
                     with_spikes(1800, 16.67, 0, big=786.8, at=[900]))
        log = os.path.join(self.dir, "rhyremix.log")
        with open(log, "w", encoding="utf-8") as fh:
            fh.write("[2026-11-12 00:48:44.501] rhyremix start 0.1\n")
            fh.write("[2026-11-12 00:48:56.000] sun: CreateLight ok dir=-0.88 radiance=4.5\n")
        text = run_tool([a, "--log", log, "--top", "1"])
        self.assertIn("786.8 ms", text)
        self.assertIn("sun: CreateLight ok", text)

    # ---------------------------------------------------------------- failures

    def test_not_a_mangohud_csv(self):
        path = os.path.join(self.dir, "NvRemixBridge_2026-11-12_00-00-00.csv")
        with open(path, "w", encoding="utf-8") as fh:
            fh.write("something,else\n1,2\n")
        proc = subprocess.run([sys.executable, TOOL, path], capture_output=True, text=True)
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("not a MangoHud CSV", proc.stdout + proc.stderr)

    def test_wallclock_from_the_file_name(self):
        a = self.csv("NvRemixBridge_2026-11-12_10-22-54.csv", steady(60))
        out = json.loads(run_tool([a, "--json", "--top", "0"]))
        self.assertEqual(out["runs"][0]["t0"], "2026-11-12T10:22:54")
        # a file name without a stamp is still usable, it just cannot be aligned
        c = os.path.join(self.dir, "frames.csv")
        write_mangohud(c, steady(60))
        out = json.loads(run_tool([c, "--json", "--top", "0"]))
        self.assertIsNone(out["runs"][0]["t0"])


if __name__ == "__main__":
    unittest.main()
