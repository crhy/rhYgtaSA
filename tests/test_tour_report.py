#!/usr/bin/env python3
"""Tests for tools/tour-report.py — pairing a tour manifest with its shot files (task s23).

Why this test exists: the tour writes `screenshots/tour-manifest.txt` from Lua (`os.date`) and
the screenshot itself is written a second later by scripts/shot-watch.sh from the *file* clock,
so the only thing joining a manifest line to a PNG is its timestamp, at second resolution, with
manual F12 shots sitting in the same folder. A report that paired them wrongly would put the
numbers of the wrong stop under a name - which is exactly how a lighting bug gets "confirmed"
by a screenshot that is not the one being discussed. So pairing, the objective numbers and the
"nearest rhyremix.log line" lookup are all tested here against a fake tree.

Everything runs on synthetic PNGs, a hand-written manifest and a fake log under
REPO/build/tmp/tour-report-selftest/ (rule 2: temp files never in /tmp). The real game folder
is never opened.
"""
import datetime as dt
import importlib.util
import os
import shutil
import unittest
from PIL import Image

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
SPEC = importlib.util.spec_from_file_location("tour_report",
                                              os.path.join(REPO, "tools", "tour-report.py"))
T = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(T)

TMP = os.path.join(REPO, "build", "tmp", "tour-report-selftest")

MANIFEST = """2026-11-11 09:05:53  tour start, 6 stops
2026-11-11 09:06:08  shot 01 00-baseline-noon-grove
2026-11-11 09:06:25  shot 02 06-signals-close-2100
2026-11-11 09:06:42  shot 03 09-grove-dawn-0530
2026-11-11 09:06:59  shot 04 10-water-santa-maria-noon
2026-11-11 09:07:16  shot 05 11-water-santa-maria-2100
2026-11-11 09:07:33  shot 06 12-the-shot-that-never-landed
2026-11-11 09:07:36  tour done
"""

LOG = """---- log starts ----
[2026-11-11 09:05:59.100] streetlights: 320 active, 320 drawn this frame (hour=21, 7002 in list, kept=320 new=0) | remix: live=321 create_fail=0 draw_fail=0 budget_stop=0
[2026-11-11 09:06:01.000] trafficlights: 39 active, 39 drawn this frame (green=18 amber=5 red=16, 0 awaiting destroy, 1522 in list, hour=21, phase_swaps=4) | remix: live=360 create_fail=0 draw_fail=0 budget_stop=0
[2026-11-11 09:06:07.500] sky: sun elev=-31.0 az=341.9 airMass=0.00 T=0.000/0.000/0.000 daylight=0.00 | moon elev=+31.0 az=161.9 phase=1.00 | lamps=on (hour=21:00 doy=186 month=47 day=3) | rtx: set=4 unknown=0 badval=0
[2026-11-11 09:06:08.100] streetlights: CreateLight ok #4957 hash=4957 pos=2489.9,-1656.0,19.8 radiance=12000.00,10124.71,0.00 radius=0.15 cone=120.0/0.45 focus=0.00
[2026-11-11 09:07:00.000] streetlights: 320 active, 320 drawn this frame (hour=21, 7002 in list, kept=320 new=0) | remix: live=321 create_fail=0 draw_fail=0 budget_stop=0
[2026-11-11 09:07:01.000] trafficlights: 39 active, 39 drawn this frame (green=18 amber=5 red=16, 0 awaiting destroy, 1522 in list, hour=21, phase_swaps=8) | remix: live=360 create_fail=0 draw_fail=0 budget_stop=0
[2026-11-11 09:07:05.000] trafficlights[source=game]: 7 heads lit, 7 drawn this frame (green=2 amber=1 red=4, 7 coronas registered this frame, colour_swaps=3 expired=2) | remix: live=379 create_fail=0 draw_fail=0 budget_stop=0 | hook: calls=412 records=412 dropped=0 bad=0 reentrant=0
[2026-11-11 09:07:02.000] sky: sun elev=-31.2 az=343.0 airMass=0.00 T=0.000/0.000/0.000 daylight=0.00 | moon elev=+31.2 az=163.0 phase=1.00 | lamps=on (hour=21:05 doy=186 month=63 day=3) | rtx: set=4 unknown=0 badval=0
not a log line, the plugin wrote something odd
"""


def flat(path, rgb, size=(40, 24)):
    Image.new("RGB", size, rgb).save(path)
    return path


def signal_image(path, size=(100, 100)):
    """1% red, 1% amber, 2% green head on flat grey tarmac: the grey must never count."""
    im = Image.new("RGB", size, (128, 128, 128))
    for box, rgb in (((0, 0, 10, 10), (200, 20, 20)),      # red
                     ((20, 0, 30, 10), (230, 150, 20)),    # amber / sodium
                     ((40, 0, 60, 10), (30, 180, 40))):    # green
        for y in range(box[1], box[3]):
            for x in range(box[0], box[2]):
                im.putpixel((x, y), rgb)
    im.save(path)
    return path


def fake_tree(root):
    """screenshots/shots + tour-manifest.txt + scripts/rhyremix.log, the way the tour leaves them"""
    shots = os.path.join(root, "screenshots", "shots")
    os.makedirs(shots, exist_ok=True)
    with open(os.path.join(root, "screenshots", "tour-manifest.txt"), "w", encoding="utf-8") as f:
        f.write(MANIFEST)
    os.makedirs(os.path.join(root, "scripts"), exist_ok=True)
    with open(os.path.join(root, "scripts", "rhyremix.log"), "w", encoding="utf-8") as f:
        f.write(LOG)
    named = {
        "shot_2026-11-11_09-06-08.png": ((128, 128, 128), (40, 24)),   # 01 baseline: flat grey
        "shot_2026-11-11_09-06-25.png": None,                          # 02 signal head image
        "shot_2026-11-11_09-06-42.png": ((255, 255, 255), (40, 24)),   # 03 blown white
        "shot_2026-11-11_09-06-59.png": ((0, 0, 0), (40, 24)),         # 04 crushed black
        "shot_2026-11-11_09-07-16.png": ((10, 120, 200), (40, 24)),    # 05 water blue
        "shot_2026-11-11_08-45-46.png": ((128, 128, 128), (40, 24)),   # manual F12, no manifest line
    }
    for name, spec in named.items():
        p = os.path.join(shots, name)
        if spec is None:
            signal_image(p, (100, 100))
        else:
            flat(p, spec[0], spec[1])
    return root


class TempCase(unittest.TestCase):
    def setUp(self):
        shutil.rmtree(TMP, ignore_errors=True)
        os.makedirs(TMP, exist_ok=True)
        self.game = fake_tree(os.path.join(TMP, "game"))
        self.shots = os.path.join(self.game, "screenshots", "shots")

    def tearDown(self):
        shutil.rmtree(TMP, ignore_errors=True)


class TestManifest(TempCase):
    def test_shot_lines_become_entries_and_events_are_kept(self):
        shots, events = T.parse_manifest(os.path.join(self.game, "screenshots", "tour-manifest.txt"))
        self.assertEqual([s.seq for s in shots], [1, 2, 3, 4, 5, 6])
        self.assertEqual(shots[1].name, "06-signals-close-2100")
        self.assertEqual(shots[0].dt, dt.datetime(2026, 11, 11, 9, 6, 8))
        self.assertEqual(events[0][1], "tour start, 6 stops")
        self.assertEqual(events[-1][1], "tour done")

    def test_line_without_a_timestamp_is_an_event_not_a_crash(self):
        path = os.path.join(TMP, "odd-manifest.txt")
        with open(path, "w", encoding="utf-8") as f:
            f.write("no timestamp here\n2026-11-11 09:06:08  shot 07 ok-name\n")
        shots, events = T.parse_manifest(path)
        self.assertEqual([s.name for s in shots], ["ok-name"])
        self.assertEqual(events[0], (None, "no timestamp here"))


class TestShotFiles(TempCase):
    def test_filename_timestamp(self):
        files = T.shot_files(self.shots)
        self.assertEqual(len(files), 6)
        self.assertEqual(files[0].dt, dt.datetime(2026, 11, 11, 8, 45, 46))
        self.assertEqual(files[1].dt, dt.datetime(2026, 11, 11, 9, 6, 8))

    def test_unparsable_name_falls_back_to_mtime(self):
        p = os.path.join(self.shots, "manual-f12.png")
        flat(p, (9, 9, 9))
        t = dt.datetime(2026, 11, 11, 9, 6, 30).timestamp()
        os.utime(p, (t, t))
        got = {f.name: f for f in T.shot_files(self.shots)}
        self.assertEqual(got["manual-f12.png"].dt, dt.datetime(2026, 11, 11, 9, 6, 30))

    def test_non_images_are_ignored(self):
        with open(os.path.join(self.shots, "rhytour_01.req"), "w", encoding="utf-8") as f:
            f.write("leftover marker\n")
        self.assertEqual(len(T.shot_files(self.shots)), 6)


class TestPairing(TempCase):
    def test_pairs_by_time_and_lists_the_unclaimed_files(self):
        shots, _ = T.parse_manifest(os.path.join(self.game, "screenshots", "tour-manifest.txt"))
        files = T.shot_files(self.shots)
        pairs, extra = T.pair(shots, files, 8.0)
        self.assertEqual([p[1].name if p[1] else None for p in pairs],
                         ["shot_2026-11-11_09-06-08.png", "shot_2026-11-11_09-06-25.png",
                          "shot_2026-11-11_09-06-42.png", "shot_2026-11-11_09-06-59.png",
                          "shot_2026-11-11_09-07-16.png", None])
        self.assertEqual([f.name for f in extra], ["shot_2026-11-11_08-45-46.png"])

    def test_tolerance_rejects_a_far_file(self):
        entry = [T.ShotEntry(1, "x", dt.datetime(2026, 11, 11, 9, 6, 8), "")]
        files = [T.ShotFile("a.png", dt.datetime(2026, 11, 11, 9, 6, 20), "a.png")]
        pairs, extra = T.pair(entry, files, 8.0)
        self.assertIsNone(pairs[0][1])
        self.assertEqual(len(extra), 1)
        pairs, extra = T.pair(entry, files, 15.0)
        self.assertEqual(pairs[0][1], files[0])
        self.assertEqual(extra, [])

    def test_one_file_cannot_be_claimed_twice_the_closer_entry_wins(self):
        entries = [T.ShotEntry(1, "a", dt.datetime(2026, 11, 11, 9, 6, 8), ""),
                   T.ShotEntry(2, "b", dt.datetime(2026, 11, 11, 9, 6, 11), "")]
        files = [T.ShotFile("a.png", dt.datetime(2026, 11, 11, 9, 6, 10), "a.png")]
        pairs, extra = T.pair(entries, files, 8.0)
        self.assertIsNone(pairs[0][1])
        self.assertEqual(pairs[1][1], files[0])
        self.assertEqual(extra, [])


class TestSince(unittest.TestCase):
    def test_the_accepted_forms(self):
        base = dt.datetime(2026, 11, 11, 9, 6, 8)
        self.assertEqual(T.parse_since("2026-11-11 21:00:00"), dt.datetime(2026, 11, 11, 21, 0, 0))
        self.assertEqual(T.parse_since("2026-11-11T21:00"), dt.datetime(2026, 11, 11, 21, 0, 0))
        self.assertEqual(T.parse_since("2026-11-11"), dt.datetime(2026, 11, 11, 0, 0, 0))
        self.assertEqual(T.parse_since("21:30", base), dt.datetime(2026, 11, 11, 21, 30, 0))
        self.assertEqual(T.parse_since("21:30:15", base), dt.datetime(2026, 11, 11, 21, 30, 15))
        self.assertEqual(T.parse_since(str(dt.datetime(2026, 11, 11, 21, 0).timestamp()), base),
                         dt.datetime(2026, 11, 11, 21, 0))
        self.assertIsNone(T.parse_since(""))
        self.assertIsNone(T.parse_since(None))

    def test_a_bogus_form_is_a_usage_error(self):
        with self.assertRaises(ValueError):
            T.parse_since("half past nine")

    def test_a_bare_clock_time_with_the_manifest_times_uses_the_newest_date(self):
        """s31: a manifest can hold two days of runs. The date of its first line is then the
        wrong date for a bare HH:MM - that is what made --since 00:48 re-report 2026-11-11."""
        dates = [dt.datetime(2026, 11, 11, 9, 6, 8), dt.datetime(2026, 11, 11, 9, 9, 32),
                 dt.datetime(2026, 11, 12, 0, 49, 24), dt.datetime(2026, 11, 12, 0, 53, 39)]
        self.assertEqual(T.parse_since("00:48", dates), dt.datetime(2026, 11, 12, 0, 48, 0))
        # a clock time that the newest date cannot mean falls back to the newest date that can,
        # and never to a date after the last entry
        self.assertEqual(T.parse_since("09:00", dates), dt.datetime(2026, 11, 11, 9, 0, 0))
        self.assertEqual(T.parse_since("09:00", []), T.parse_since("09:00", None))


TWO_RUN_MANIFEST = """2026-11-11 09:03:09  waiting: step outside to start the tour
2026-11-11 09:05:53  tour start, 2 stops
2026-11-11 09:06:08  shot 01 00-baseline-noon-grove
2026-11-11 09:06:25  shot 02 05-burger-shot-marina-2100
2026-11-11 09:06:28  tour done
2026-11-12 00:48:57  waiting: step outside to start the tour
2026-11-12 00:49:09  tour start, 2 stops
2026-11-12 00:49:24  shot 01 00-baseline-noon-grove
2026-11-12 00:49:41  shot 02 14-vegetation-vinewood-noon
2026-11-12 00:53:41  tour done
"""


class TestTwoRunsInOneManifest(TempCase):
    """s31: the tour appends to one tour-manifest.txt, so the 2026-11-12 run sat behind the
    2026-11-11 run in the same file. --since 00:48 then took its date from the first entry, and
    the report directory was named after the first tour start: the 11th was reported again and
    the 12th never was."""

    def setUp(self):
        shutil.rmtree(TMP, ignore_errors=True)
        os.makedirs(TMP, exist_ok=True)
        self.game = fake_tree(os.path.join(TMP, "game"))
        self.shots = os.path.join(self.game, "screenshots", "shots")
        with open(os.path.join(self.game, "screenshots", "tour-manifest.txt"), "w",
                  encoding="utf-8") as f:
            f.write(TWO_RUN_MANIFEST)
        flat(os.path.join(self.shots, "shot_2026-11-12_00-49-24.png"), (10, 120, 200), (40, 24))
        flat(os.path.join(self.shots, "shot_2026-11-12_00-49-41.png"), (20, 90, 30), (40, 24))
        self.manifest = os.path.join(self.game, "screenshots", "tour-manifest.txt")

    def tearDown(self):
        shutil.rmtree(TMP, ignore_errors=True)

    def report(self, *argv):
        rc = T.main(["--no-sheets"] + list(argv) + [self.game])
        self.assertIn(rc, (0, 1))
        return rc

    def test_split_sections_cuts_the_manifest_at_every_tour_start(self):
        shots, events = T.parse_manifest(self.manifest)
        secs = T.split_sections(shots, events)
        # the "waiting" line before the first tour start is a preamble section, not lost
        self.assertEqual([s[0] for s in secs],
                         [None, dt.datetime(2026, 11, 11, 9, 5, 53),
                          dt.datetime(2026, 11, 12, 0, 49, 9)])
        self.assertEqual([len(s[2]) for s in secs], [0, 2, 2])
        self.assertEqual(secs[1][1], secs[2][0])            # a run ends where the next starts
        self.assertIsNone(secs[2][1])
        self.assertEqual([s.name for s in secs[2][2]],
                         ["00-baseline-noon-grove", "14-vegetation-vinewood-noon"])
        self.assertEqual([t for _w, t in secs[0][3]], ["waiting: step outside to start the tour"])
        # a manifest with no tour start line at all is one section
        self.assertEqual(len(T.split_sections(shots, [])), 1)

    def test_since_a_bare_clock_time_reports_the_newest_run_only(self):
        self.report("--since", "00:48")
        new = os.path.join(self.game, "screenshots", "report-2026-11-12_00-49-09", "report.md")
        old = os.path.join(self.game, "screenshots", "report-2026-11-11_09-05-53")
        self.assertTrue(os.path.isfile(new), "the report went to the newest run")
        self.assertFalse(os.path.isdir(old), "the 2026-11-11 run is not reported again")
        text = open(new, encoding="utf-8").read()
        self.assertIn("14-vegetation-vinewood-noon", text)
        self.assertNotIn("05-burger-shot-marina-2100", text)
        self.assertIn("2026-11-12 00:49:09", text)          # the run window is in the header

    def test_since_a_full_timestamp_picks_the_run_it_falls_inside(self):
        self.report("--since", "2026-11-12 00:50")
        text = open(os.path.join(self.game, "screenshots", "report-2026-11-12_00-49-09",
                                 "report.md"), encoding="utf-8").read()
        self.assertIn("14-vegetation-vinewood-noon", text)
        self.assertNotIn("05-burger-shot-marina-2100", text)

    def test_since_the_older_run_still_reports_the_older_run(self):
        self.report("--since", "2026-11-11 09:05")
        text = open(os.path.join(self.game, "screenshots", "report-2026-11-11_09-05-53",
                                 "report.md"), encoding="utf-8").read()
        self.assertIn("05-burger-shot-marina-2100", text)
        self.assertNotIn("14-vegetation-vinewood-noon", text)

    def test_pick_section_the_cut_between_two_runs_belongs_to_neither(self):
        shots, events = T.parse_manifest(self.manifest)
        secs = T.split_sections(shots, events)
        pick = lambda spec: T.pick_section(secs, T.parse_since(spec, [s.dt for s in shots]))
        self.assertEqual(pick("00:48")[0], dt.datetime(2026, 11, 12, 0, 49, 9))
        self.assertEqual(pick("2026-11-11 09:06")[0], dt.datetime(2026, 11, 11, 9, 5, 53))
        self.assertEqual(pick("2026-11-11")[0], dt.datetime(2026, 11, 11, 9, 5, 53))
        self.assertEqual(pick("2026-11-12 00:50")[0], dt.datetime(2026, 11, 12, 0, 49, 9))
        self.assertIsNone(pick("2026-11-13"))

    def test_without_since_the_whole_file_is_still_one_report(self):
        self.report()
        text = open(os.path.join(self.game, "screenshots", "report-2026-11-11_09-05-53",
                                 "report.md"), encoding="utf-8").read()
        self.assertIn("05-burger-shot-marina-2100", text)
        self.assertIn("14-vegetation-vinewood-noon", text)


class TestMetrics(TempCase):
    def test_flat_grey_is_exactly_its_own_value(self):
        m = T.metrics(flat(self.game + "/grey.png", (128, 128, 128), (20, 20)))
        self.assertEqual((m["width"], m["height"], m["pixels"]), (20, 20, 400))
        self.assertAlmostEqual(m["mean"], 128.0, places=3)
        self.assertEqual((m["p05"], m["p50"], m["p95"]), (128.0, 128.0, 128.0))
        self.assertEqual((m["pct_white"], m["pct_black"], m["mean_sat"]), (0.0, 0.0, 0.0))
        self.assertEqual((m["pct_red"], m["pct_amber"], m["pct_green"]), (0.0, 0.0, 0.0))

    def test_white_and_black_clipping(self):
        w = T.metrics(flat(self.game + "/w.png", (255, 255, 255), (10, 10)))
        b = T.metrics(flat(self.game + "/b.png", (0, 0, 0), (10, 10)))
        self.assertEqual((w["pct_white"], w["pct_black"]), (100.0, 0.0))
        self.assertAlmostEqual(w["mean"], 255.0, places=3)
        self.assertEqual((b["pct_white"], b["pct_black"]), (0.0, 100.0))
        self.assertEqual(b["mean"], 0.0)
        self.assertEqual(b["mean_sat"], 0.0)          # no lit pixel to measure

    def test_luma_weights_are_rec709(self):
        m = T.metrics(flat(self.game + "/r.png", (255, 0, 0), (4, 4)))
        # mean/percentiles are taken over the 8-bit quantised luma: 255*0.2126 = 54.21 -> 54
        self.assertEqual(m["mean"], 54.0)
        self.assertAlmostEqual(255 * T.LUMA_W[0], 54.213, places=3)
        self.assertAlmostEqual(m["mean"], 255 * T.LUMA_W[0], delta=1.0)

    def test_signal_head_fractions_and_that_grey_is_not_a_colour(self):
        m = T.metrics(signal_image(self.game + "/sig.png", (100, 100)))
        self.assertAlmostEqual(m["pct_red"], 1.0, places=3)
        self.assertAlmostEqual(m["pct_amber"], 1.0, places=3)
        self.assertAlmostEqual(m["pct_green"], 2.0, places=3)

    def test_dim_or_desaturated_head_pixels_are_not_counted(self):
        # same hues as above but below VAL_MIN / SAT_MIN: a signal that is not lit reads as 0%
        im = Image.new("RGB", (20, 20), (128, 128, 128))
        for y in range(10):
            for x in range(10):
                im.putpixel((x, y), (30, 3, 3))        # red hue, but v = 30/255 < VAL_MIN
            for x in range(10, 20):
                im.putpixel((x, y), (120, 110, 100))   # warm-ish, but s = 10/120 < SAT_MIN
        p = self.game + "/dim.png"
        im.save(p)
        m = T.metrics(p)
        self.assertEqual((m["pct_red"], m["pct_amber"], m["pct_green"]), (0.0, 0.0, 0.0))

    def test_pure_python_fallback_agrees_with_numpy(self):
        if T.np is None:
            self.skipTest("numpy not installed")
        p = signal_image(self.game + "/both.png", (70, 40))
        with Image.open(p) as im:
            im = im.convert("RGB")
            slow = T._metrics_pure(im, im.width, im.height)
        fast = T.metrics(p)
        for k in ("mean", "p05", "p50", "p95", "pct_white", "pct_black", "mean_sat",
                  "pct_red", "pct_amber", "pct_green"):
            self.assertAlmostEqual(fast[k], slow[k], places=3, msg=k)

    def test_signal_selection_and_flags(self):
        self.assertTrue(T.is_signal_shot("06-signals-close-2100"))
        self.assertFalse(T.is_signal_shot("00-baseline-noon-grove"))
        self.assertTrue(T.is_signal_shot("00-baseline-noon-grove", "all"))
        self.assertFalse(T.is_signal_shot("06-signals-close-2100", "none"))
        self.assertEqual(T.flags({"pct_white": 0.0, "pct_black": 1.0, "mean": 128.0}), [])
        self.assertEqual(len(T.flags({"pct_white": 7.5, "pct_black": 55.0, "mean": 12.0})), 3)


class TestLog(unittest.TestCase):
    def setUp(self):
        self.path = os.path.join(TMP, "rhyremix.log")
        os.makedirs(TMP, exist_ok=True)
        with open(self.path, "w", encoding="utf-8") as f:
            f.write(LOG)
        self.cats, self.total = T.load_log(self.path)

    def test_only_the_summary_lines_are_candidates(self):
        self.assertEqual(len(self.cats["lights"]), 2)     # the CreateLight line is not one
        self.assertEqual(len(self.cats["signals"]), 3)    # two list-mode lines + the source=game one
        self.assertEqual(len(self.cats["sky"]), 2)
        self.assertGreater(self.total, 0)

    def test_nearest_line_per_category_with_its_delta(self):
        when = dt.datetime(2026, 11, 11, 9, 6, 8)
        line, d = T.nearest(self.cats, "lights", when, 30.0)
        self.assertTrue(line.text.startswith("streetlights: 320 active"))
        self.assertAlmostEqual(d, -8.9, places=3)
        line, d = T.nearest(self.cats, "sky", when, 30.0)
        self.assertIn("sun elev=-31.0", line.text)
        self.assertAlmostEqual(d, -0.5, places=3)
        line, _d = T.nearest(self.cats, "signals", when, 30.0)
        self.assertIn("green=18 amber=5 red=16", line.text)

    def test_window_and_missing_category(self):
        when = dt.datetime(2026, 11, 11, 9, 6, 8)
        self.assertEqual(T.nearest(self.cats, "lights", when, 5.0), (None, None))
        self.assertEqual(T.nearest({}, "lights", when, 30.0), (None, None))

    def test_the_source_game_hook_line_counts_as_a_signal_line(self):
        """s01/s19 print 'trafficlights[source=game]: N heads lit', not 'N active'.

        That is the mode docs/NEXT-TEST.md step 2 is about, so a blank signals column there
        would read as 'the heads never lit' when it only means the reader missed the line.
        """
        when = dt.datetime(2026, 11, 11, 9, 7, 6)
        line, d = T.nearest(self.cats, "signals", when, 30.0)
        self.assertIn("heads lit", line.text)
        self.assertIn("reentrant=0", line.text)
        self.assertAlmostEqual(d, -1.0, places=3)

    def test_missing_log_is_not_fatal(self):
        cats, total = T.load_log(os.path.join(TMP, "nope.log"))
        self.assertEqual(total, 0)
        self.assertEqual(set(cats), {"lights", "signals", "sky"})


class TestEndToEnd(TempCase):
    def run_main(self, *extra):
        return T.main([self.game] + list(extra))

    def test_report_dir_sheets_and_markdown(self):
        self.assertEqual(self.run_main("--cell", "64x36", "--per-sheet", "4"), 0)
        out = os.path.join(self.game, "screenshots", "report-2026-11-11_09-05-53")
        self.assertTrue(os.path.isdir(out))
        self.assertEqual(sorted(n for n in os.listdir(out) if n.startswith("sheet-")),
                         ["sheet-01.png", "sheet-02.png"])
        with Image.open(os.path.join(out, "sheet-01.png")) as im:
            self.assertEqual(im.size, (2 * (64 + 8) + 8, 2 * (36 + 46 + 8) + 8))
        with Image.open(os.path.join(out, "sheet-02.png")) as im:   # 1 shot left over -> 1 row
            self.assertEqual(im.size, (2 * (64 + 8) + 8, 1 * (36 + 46 + 8) + 8))
        md = open(os.path.join(out, "report.md"), encoding="utf-8").read()
        for needle in ("00-baseline-noon-grove", "06-signals-close-2100",
                       "signal pixels: red 1.000%, amber 1.000%, green 2.000%",
                       "green=18 amber=5 red=16", "sun elev=-31.0",
                       "streetlights: 320 active", "tour start, 6 stops",
                       "shot_2026-11-11_08-45-46.png",
                       "No screenshot file within 8.0 s"):
            self.assertIn(needle, md)
        self.assertIn("| 01 | 2026-11-11 09:06:08 | `00-baseline-noon-grove` | 128.0", md)
        self.assertIn("| 03 | 2026-11-11 09:06:42 | `09-grove-dawn-0530` | 255.0 | 255 | 255 | 255 | 100.00 |", md)
        # the non-signal stops must not pretend to have colour numbers
        self.assertIn("| 01 | 2026-11-11 09:06:08 | `00-baseline-noon-grove` | 128.0 | 128 | 128 | 128 | 0.00 | 0.0 | 0.000 | - | - | - |", md)

    def test_since_restricts_the_run_and_renames_the_report_dir(self):
        self.assertEqual(self.run_main("--since", "09:06:30", "--no-sheets"), 0)
        out = os.path.join(self.game, "screenshots", "report-2026-11-11_09-06-42")
        md = open(os.path.join(out, "report.md"), encoding="utf-8").read()
        self.assertIn("09-grove-dawn-0530", md)
        self.assertNotIn("00-baseline-noon-grove", md)
        self.assertNotIn("tour start, 6 stops", md)      # the event is before --since

    def test_no_sheets_writes_only_the_markdown(self):
        self.assertEqual(self.run_main("--no-sheets"), 0)
        out = os.path.join(self.game, "screenshots", "report-2026-11-11_09-05-53")
        self.assertEqual(os.listdir(out), ["report.md"])

    def test_missing_shots_dir_is_a_usage_error(self):
        self.assertEqual(T.main([os.path.join(TMP, "not-a-game")]), 2)

    def test_nothing_pairable_is_reported_as_such(self):
        self.assertEqual(T.main([self.game, "--since", "2026-11-12 09:00:00"]), 1)

    def test_no_manifest_falls_back_to_the_files_alone(self):
        os.remove(os.path.join(self.game, "screenshots", "tour-manifest.txt"))
        self.assertEqual(self.run_main("--no-sheets"), 0)
        out = os.path.join(self.game, "screenshots", "report-2026-11-11_08-45-46")
        md = open(os.path.join(out, "report.md"), encoding="utf-8").read()
        self.assertIn("shot_2026-11-11_09-06-08", md)

    def test_imagemagick_fallback_sheet_is_written(self):
        """the sheet path used when PIL is not importable (montage exists here, so test it)"""
        if shutil.which("montage") is None:
            self.skipTest("ImageMagick montage not installed")
        shots, _ = T.parse_manifest(os.path.join(self.game, "screenshots", "tour-manifest.txt"))
        files = T.shot_files(self.shots)
        pairs = [(e, f, T.metrics(f.path))
                 for e, f in T.pair(shots, files, 8.0)[0] if f is not None][:2]
        out = os.path.join(TMP, "montage-sheet.png")
        T.contact_sheet_imagemagick(pairs, out, per_sheet=4, cell=(64, 36))
        with Image.open(out) as im:
            self.assertGreater(im.width, 64)
            self.assertGreater(im.height, 36)


if __name__ == "__main__":
    unittest.main()
