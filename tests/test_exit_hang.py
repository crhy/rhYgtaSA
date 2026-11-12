#!/usr/bin/env python3
"""n09 gate: the exit-hang fix is wired end to end.

The bug (2026-11-12 10:39, build/tmp/exit-hang-20261112.txt): rhY quit from the menu, the Remix
bridge server and the game window went away, and gta-sa.exe spun for two minutes while the plugin
kept drawing 302 lights per frame into a bridge client whose server no longer existed - and logged
nothing about any of it. The second hang the same day (11:48, build/tmp/exit-hang-20261112-b.txt)
was worse: the bridge did NOT exit, its last line was "Client window became inactive, disabling
timeouts for bridge server...", and both processes held the GPU for sixteen minutes.

The behaviour itself is asserted numerically in tests/shutdown_check.cpp (the persistence windows,
the latching, the wraparound) and in scripts/test-session-selftest.sh cases F/G/H (the watchdog).
This file pins the WIRING, because every one of those pieces is invisible to the others:

  * the state machine has to be compiled into the plugin, not just tested;
  * the latch has to sit in remix.cpp, where every Remix call already funnels through - a module
    that calls api->CreateLight directly would keep calling a dead bridge;
  * the log lines that were missing have to exist, including at DLL_PROCESS_DETACH;
  * the code patch at 0x49DD4C has to be taken back out, and not put back in;
  * the shell watchdog has to be started around the launcher and stopped after it, and the
    selftest has to cover both the hang and the clean exit (a watchdog that fires on a normal exit
    is worse than none);
  * and (n09b) the LAUNCHER has to end the wine tree, because `waitforexitandrun` waits for a game
    that never exits - the second hang on 2026-11-12 kept BOTH gta-sa.exe and NvRemixBridge.exe
    spinning for sixteen minutes with the launcher still waiting, holding 3.6 GB of VRAM.

Run: python3 -m pytest -q tests/test_exit_hang.py   (or python3 -m unittest)
"""
import os
import re
import unittest

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def read(*parts):
    with open(os.path.join(REPO, *parts), encoding="utf-8", errors="replace") as f:
        return f.read()


class TestShutdownStateMachine(unittest.TestCase):
    def setUp(self):
        self.h = read("plugin", "src", "shutdown.h")
        self.c = read("plugin", "src", "shutdown.cpp")
        self.main = read("plugin", "src", "main.cpp")
        self.remix = read("plugin", "src", "remix.cpp")

    def test_the_state_machine_is_built_into_the_plugin(self):
        self.assertIn("src/shutdown.cpp", read("plugin", "CMakeLists.txt"))
        self.assertIn("shutdown.h", self.main)

    def test_every_signal_has_its_own_persistence_window(self):
        for bit in ("kRhyShutPresent", "kRhyShutDevice", "kRhyShutWindow",
                    "kRhyShutCalls", "kRhyShutDetach"):
            self.assertIn(bit, self.h)
            self.assertIn(bit, self.c)
        for field in ("presentMs", "deviceMs", "windowMs", "callsMs"):
            self.assertIn(field, self.h)
            self.assertIn(field, self.c)

    def test_our_own_failing_calls_need_company(self):
        # The rule that keeps one bad light from switching the bridge off for the session.
        self.assertRegex(self.c, r"company")
        self.assertIn("kRhyShutPresent | kRhyShutDevice | kRhyShutWindow", self.c)

    def test_death_latches_and_ticks_are_wrap_safe(self):
        self.assertRegex(self.c, r"if \(t\.deadTick\) \{")   # already dead: only count
        self.assertIn("static_cast<int32_t>(nowMs - thenMs)", self.c)

    def test_the_latch_is_in_remix_cpp_not_in_a_module(self):
        # rhyRemixApi() is the single door: every module starts its frame with `if (!api ...)`.
        self.assertRegex(self.remix,
                         r"rhyRemixApi\(\)\s*\{\s*return \(g_ready && !g_dead\)")
        for fn in ("rhyCreateLight", "rhyDestroyLight", "rhyDrawLight"):
            body = self.remix.split(fn, 1)[1][:900]
            self.assertIn("g_dead", body, "%s does not check the latch" % fn)
        self.assertIn("g_skipped", self.remix)

    def test_no_module_calls_the_bridge_api_directly(self):
        # A module holding the interface pointer would keep calling a dead bridge.
        for mod in ("streetlights.cpp", "trafficlights.cpp", "sun.cpp", "probe.cpp",
                    "rtxconf.cpp", "delayed.cpp"):
            src = read("plugin", "src", mod)
            self.assertNotRegex(src, r"api->(CreateLight|DestroyLight|DrawLightInstance)\(",
                                "%s calls the Remix API without going through remix.cpp" % mod)

    def test_the_shutdown_log_lines_exist(self):
        for line in ("shutdown: suspect", "shutdown: bridge gone",
                     "shutdown: still running", "shutdown: DLL_PROCESS_DETACH",
                     "shutdown watch:"):
            self.assertIn(line, self.main)
        self.assertIn("rhyLogDetach", read("plugin", "src", "log.h"))
        self.assertIn("rhyLogDetach", read("plugin", "src", "log.cpp"))

    def test_dll_process_detach_is_handled(self):
        self.assertIn("DLL_PROCESS_DETACH", self.main)
        self.assertIn("rhyShutdownDetach", self.main)

    def test_the_window_handle_comes_from_the_device(self):
        # The game hands Present no HWND; GetCreationParameters (vtable 9) is the only honest way.
        self.assertIn("kGetCreationParametersEntry = 9", self.main)
        self.assertIn("kTestCooperativeLevelEntry = 3", self.main)
        self.assertIn("hFocusWindow", self.main)
        self.assertIn("IsWindow(", self.main)

    def test_the_init_wait_stays_bounded_and_stops_on_shutdown(self):
        self.assertIn("for (int i = 0; i < 1200; ++i)", self.main)
        self.assertRegex(self.main, r"rhyShutdownDead\(g_shut\)[\s\S]{0,200}return 0;")

    def test_nothing_waits_on_another_thread(self):
        # The notes claim the plugin has one bounded thread and no joins; keep that claim true.
        src = "".join(read("plugin", "src", f) for f in os.listdir(os.path.join(REPO, "plugin", "src"))
                      if f.endswith(".cpp"))
        for forbidden in ("WaitForSingleObject", "std::thread", "atexit(", "pthread_join"):
            self.assertNotIn(forbidden, src)


class TestSignalHookIsRemoved(unittest.TestCase):
    def setUp(self):
        self.h = read("plugin", "src", "signalhook.h")
        self.c = read("plugin", "src", "signalhook.cpp")
        self.main = read("plugin", "src", "main.cpp")

    def test_the_original_bytes_are_saved_and_put_back(self):
        self.assertIn("g_savedCallSite", self.c)
        self.assertIn("rhySignalHookUninstall", self.h)
        self.assertRegex(self.c, r"std::memcpy\(slot, g_savedCallSite")

    def test_the_hook_is_not_re_installed_after_shutdown(self):
        self.assertIn("kSignalHookShutdown", self.h)
        self.assertIn("kSignalHookShutdown", self.c)

    def test_the_shutdown_path_unpatches(self):
        self.assertIn("rhySignalHookUninstall()", self.main)


class TestSessionWatchdog(unittest.TestCase):
    def setUp(self):
        self.s = read("scripts", "test-session.sh")
        self.t = read("scripts", "test-session-selftest.sh")

    def test_the_watchdog_exists_and_is_bounded_by_an_ini_free_knob(self):
        self.assertIn("watch_exit_hang()", self.s)
        self.assertIn('RHYS_HANG_GRACE', self.s)
        self.assertIn("RHYS_WINDOW_PROBE", self.s)

    def test_it_is_started_around_the_launcher_and_stopped_after(self):
        self.assertRegex(self.s, r"watch_exit_hang &\n\s*WATCHDOG=\$!")
        start = self.s.index("watch_exit_hang &")
        stop = self.s.index('kill "$WATCHDOG"', start)
        run = self.s.index('bash "$LAUNCHER"', start)
        self.assertLess(run, stop, "the watchdog must outlive neither the launcher nor the script")

    def test_it_arms_only_after_the_game_was_seen_up(self):
        # Otherwise "the bridge never started" (the full-VRAM case of 2026-11-11) reads as a hang.
        self.assertIn("seen_up", self.s)

    def test_it_saves_the_same_evidence_as_the_hand_made_file(self):
        self.assertIn("exit-hang-", self.s)
        for part in ("ps -T", "rhyremix.log", "moonloader.log", "remix-dxvk.log", "/task/"):
            self.assertIn(part, self.s)

    def test_it_kills_and_continues(self):
        self.assertRegex(self.s, r'stop_pids "hung game gta-sa.exe"')

    def test_the_selftest_covers_hang_clean_exit_and_window(self):
        for launcher in ("launcher-hang", "launcher-exit", "launcher-window"):
            self.assertIn(launcher, self.t)
        for case in ("case F", "case G", "case H"):
            self.assertIn(case, self.t)
        # the false-positive case must assert absence, not presence
        self.assertIn("a clean exit was NOT called a hang", self.t)

    def test_the_sandbox_guard_covers_the_game_processes_too(self):
        # pids_named is what the watchdog kills, so it must respect RHYS_TEST_SESSION_SANDBOX.
        body = self.s.split("pids_named()", 1)[1][:700]
        self.assertIn("SANDBOX", body)

    def test_the_real_strata_check_tolerates_someone_else_restarting_it(self):
        # What actually parked n09: the selftest's own code passed all 45 other checks, and the real
        # Strata pid died mid-run because rhY took Strata down for a game run two minutes later. The
        # belt has to be "a real Strata is running afterwards", not "the same pids".
        self.assertIn("real_strata_pids", self.t)
        self.assertIn("somebody else restarted it", self.t)


class TestLauncherEndsTheRun(unittest.TestCase):
    """n09b: the launcher ends the wine tree, because `waitforexitandrun` waits for a game that
    never exits. The 11:48 run on 2026-11-12 left gta-sa.exe AND NvRemixBridge.exe spinning for
    sixteen minutes (406 % CPU, 3.6 GB VRAM) with the launcher still sitting in its wait - so the
    cleanup has to live in the launcher, not only in scripts/test-session.sh."""

    def setUp(self):
        self.l = read("launcher", "launch-rtx.sh")
        self.t = read("scripts", "launch-cleanup-selftest.sh")

    def test_the_block_is_extractable_and_selftested(self):
        self.assertIn("# --- n09b cleanup (BEGIN)", self.l)
        self.assertIn("# --- n09b cleanup (END)", self.l)
        self.assertIn("n09b cleanup (BEGIN)", self.t)
        self.assertIn("source \"$BLOCK\"", self.t)
        for fn in ("end_wine_tree", "watch_run_end", "pids_named", "window_state",
                   "find_wineserver", "stop_named", "save_exit_evidence"):
            self.assertIn(fn + "()", self.l)
        self.assertIn("case F", self.t)   # the bridge-outliving-the-game case

    def test_it_ends_the_bridge_and_the_prefix_wineserver(self):
        body = self.l.split("end_wine_tree()", 1)[1][:1900]
        self.assertIn('stop_named "NvRemixBridge.exe"', body)
        self.assertIn('"$ws" -k', body)
        self.assertIn('WINEPREFIX="$WINE_PFX"', body)
        # Proton's own wineserver, and the prefix is <compatdata>/pfx
        self.assertIn('"$PROTON_DIR/files/bin/wineserver"', self.l)
        self.assertIn('WINE_PFX="${STEAM_COMPAT_DATA_PATH', self.l)
        self.assertIn('/pfx"', self.l)
        self.assertIn('stop_named "gta-sa.exe"', body)   # the fallback if wineserver is missing
        self.assertIn('stop_named "wineserver of', body)  # ... and if -k did not take
        # -k only when a wineserver for this prefix is actually running: calling wineserver with no
        # server up would start one just to kill it.
        self.assertIn("pids_prefix_wineserver", self.l)
        self.assertRegex(body, r"if \[ -n \"\$wp\" \] && \[ -n \"\$ws\" \]")

    def test_it_runs_after_the_wait_and_watches_during_it(self):
        wait = self.l.index('"$PROTON" waitforexitandrun')
        self.assertLess(wait, self.l.index('end_wine_tree "the launcher', wait),
                        "a clean exit must clean up too")
        start = self.l.index("watch_run_end &")
        self.assertLess(start, wait, "the watchdog must be running while the launcher waits")
        self.assertLess(wait, self.l.index('kill "$RUNWATCH"', wait))
        self.assertIn('if [ "$EXIT_GRACE" != 0 ]', self.l)   # RHYS_EXIT_GRACE=0 = watchdog off

    def test_the_watchdog_needs_to_see_the_game_up_first(self):
        # A window that has not appeared yet, or a bridge that never started (the full-VRAM case of
        # 2026-11-11), is not a hang.
        self.assertIn("seen_window", self.l)
        self.assertIn('[ "$seen_window" = 1 ] && [ "$win" = 1 ]', self.l)
        self.assertIn("return 2", self.l)   # "cannot tell" never fires on its own

    def test_it_kills_by_pid_never_by_pattern(self):
        # pkill -f would match this script's own command line, which contains every path it knows.
        self.assertNotRegex(self.l, r"(?m)^\s*pkill\b")
        self.assertIn("for d in /proc/[0-9]*", self.l)

    def test_the_bridge_conf_knob_is_opt_in_and_backed_up(self):
        # bridge.defaults.conf: disableTimeouts / infiniteRetries are flipped by the runtime itself
        # when the window goes inactive, so this is an A/B switch, not the fix.
        self.assertIn("RHYS_BRIDGE_TIMEOUTS", self.l)
        self.assertIn("disableTimeouts = False", self.l)
        self.assertIn("infiniteRetries = False", self.l)
        self.assertIn("bridge.conf.bak-before-rhy-timeouts", self.l)
        self.assertRegex(self.l, r'\[ "\$\{RHYS_BRIDGE_TIMEOUTS:-0\}" = 1 \]')  # off by default

    def test_the_process_scan_is_cheap_and_zombie_safe(self):
        # The watchdog scans /proc every RHYS_EXIT_POLL seconds for the whole session: one `cat`/`tr`
        # per process per poll is a core burned next to the game, so the reads are shell builtins.
        body = self.l.split("pids_named()", 1)[1][:900]
        self.assertNotIn("$( { cat", body)
        self.assertNotIn("tr '\\0'", body)
        # and a zombie is not a running game: kill -0 succeeds on it and it never goes away by itself
        self.assertIn('!= Z', self.l)
        self.assertIn("is_alive", body)

    def test_the_sweep_runs_it(self):
        self.assertIn("launch-cleanup-selftest.sh", read("scripts", "check-all.sh"))


class TestGates(unittest.TestCase):
    def test_the_native_test_exists_and_runs_in_the_sweep(self):
        self.assertTrue(os.path.exists(os.path.join(REPO, "tests", "shutdown_check.cpp")))
        sweep = read("scripts", "check-all.sh")
        self.assertIn("nat-shut-cc", sweep)
        self.assertIn("nat-shut", sweep)

    def test_the_ini_section_is_documented(self):
        ini = read("config", "rhyremix-production.ini")
        self.assertIn("[shutdown]", ini)
        for key in ("present_ms", "device_ms", "window_ms", "calls_ms", "log_ms",
                    "stop_present_after_ms"):
            self.assertRegex(ini, r"(?m)^;?%s=" % key)

    def test_the_shipped_default_never_drops_present(self):
        cfg = read("plugin", "src", "config.cpp")
        self.assertRegex(cfg, r'stop_present_after_ms", 0\)')

    def test_the_docs_record_it(self):
        self.assertIn("hung on Quit", read("docs", "FINDINGS.md"))
        self.assertIn("Shutdown (n09)", read("CHANGELOG.md"))
        self.assertIn("exit hang", read("docs", "TESTING.md"))


if __name__ == "__main__":
    unittest.main(verbosity=2)
