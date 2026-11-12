// tests/shutdown_check.cpp - validates plugin/src/shutdown.cpp (n09).
//
// Build:
//   g++ -std=c++17 -Wall -Wextra -Iplugin/src -o build/tmp/shuttc 
//       tests/shutdown_check.cpp plugin/src/shutdown.cpp
//   ./build/tmp/shuttc
//
// The 2026-11-12 exit hang (build/tmp/exit-hang-20261112.txt, timeline in
// .strata/notes-n09-exit-hang.md): rhY quit from the menu, the bridge server and the window went
// away, and gta-sa.exe then spun for two minutes while our plugin kept drawing 302 lights into a
// bridge client whose server no longer existed - and logged nothing about any of it.
//
// What is under test is the DECISION, not the reads: when may the plugin stop calling the bridge?
// Two failure modes matter and both have to stay impossible:
//
//   * declaring the bridge gone over something ordinary - alt-tab, a resolution change, one frame
//     where Present failed, one light the Remix server refuses - which would take the world's
//     lighting out for the rest of the session;
//   * never declaring it at all, which is what happened on 2026-11-12.
//
// The last test is the honest limit of this file: on 2026-11-12 the game stopped calling Present
// 340 ms before its own exit path began, so an in-process tracker that is fed from Present never
// runs again. That is why scripts/test-session.sh grew a watchdog (step 4 of the task); the test
// asserts the limitation instead of pretending it away.

#include <cstdio>
#include <string>

#include "../plugin/src/shutdown.h"

static int g_fail = 0;
static int g_checks = 0;

static void ok(const char* what, bool cond) {
    ++g_checks;
    if (!cond) {
        ++g_fail;
        std::printf("FAIL  %s\n", what);
    } else {
        std::printf("ok    %s\n", what);
    }
}

// The shipped defaults (plugin/src/config.cpp, [shutdown]).
static RhyShutdownLimits shipped() {
    RhyShutdownLimits l;
    l.presentMs = 1500;
    l.deviceMs = 3000;
    l.windowMs = 750;
    l.callsMs = 4000;
    return l;
}

static RhyShutdownSignals none() {
    RhyShutdownSignals s;
    s.presentFailed = false;
    s.deviceLost = false;
    s.windowGone = false;
    s.callsFailing = false;
    return s;
}

// Run `frames` frames of `sig` at 16 ms per frame starting at `from`, tracker and limits as given.
static bool runFrames(RhyShutdownTrack& t, const RhyShutdownLimits& lim,
                      const RhyShutdownSignals& sig, unsigned frames, unsigned from) {
    bool fired = false;
    for (unsigned i = 0; i < frames && !fired; ++i) {
        fired = rhyShutdownUpdate(t, sig, lim, from + i * 16u);
    }
    return fired;
}

// ------------------------------------------------------------------ tests

static void testTransientSignals() {
    std::printf("\n== one bad frame is not a dead bridge ==\n");
    const RhyShutdownLimits lim = shipped();
    RhyShutdownTrack t;
    rhyShutdownTrackInit(t);

    RhyShutdownSignals s = none();
    s.presentFailed = true;
    ok("a single failed Present changes nothing",
       !rhyShutdownUpdate(t, s, lim, 10000u));
    ok("and the tracker says it is only suspect", rhyShutdownSuspect(t) && !rhyShutdownDead(t));

    s.presentFailed = false;
    ok("one good frame clears the Present timer",
       !rhyShutdownUpdate(t, s, lim, 10016u) && !rhyShutdownSuspect(t));

    // 94 frames of failure = 1504 ms, but with a good frame in the middle: the timers must not add up.
    RhyShutdownSignals f = none();
    f.presentFailed = true;
    ok("900 ms of failing Presents is still not death",
       !runFrames(t, lim, f, 56u, 10032u));           // 56*16 = 896 ms
    s.presentFailed = false;
    rhyShutdownUpdate(t, s, lim, 10928u);              // the gap
    ok("and the timer restarted after the gap",
       !runFrames(t, lim, f, 90u, 10944u));            // 1440 ms, still under 1500

    // Continuous failure reaches the window.
    ok("a Present that keeps failing for 1500 ms ends it",
       runFrames(t, lim, f, 10u, 12384u));             // 12384 + 96 = 12480 -> held 1552 ms
    ok("and the reason is present", (t.deadReasons & kRhyShutPresent) != 0u);
    char why[64];
    rhyShutdownReasonText(t.deadReasons, why, sizeof why);
    ok("the reason text names it", std::string(why) == "present");
}

static void testQuitSequence() {
    std::printf("\n== the 2026-11-12 Quit: window first, then everything ==\n");
    const RhyShutdownLimits lim = shipped();
    RhyShutdownTrack t;
    rhyShutdownTrackInit(t);

    // 693 frames of "the player ped pointer is 0" (the log's player_gone_frames) with a healthy
    // device: that is the menu, and it must not end the session.
    ok("693 frames without a player ped is not a shutdown",
       !runFrames(t, lim, none(), 693u, 0u));

    // The window is destroyed at t=11088 (693 frames * 16 ms). Present still works for a bit.
    RhyShutdownSignals s = none();
    s.windowGone = true;
    ok("the window going away does not end it on the spot",
       !rhyShutdownUpdate(t, s, lim, 11088u));
    ok("750 ms of no window does", runFrames(t, lim, s, 48u, 11104u));  // 11104+47*16 = 11856
    ok("reason is window", t.deadReasons == kRhyShutWindow);
    ok("and the elapsed time is the hold, not the whole run",
       rhyShutdownElapsed(11856u, 11088u) == 768u);

    // Latched: the game presents 200 more healthy frames, nothing revives.
    RhyShutdownSignals good = none();
    ok("a healthy frame after the latch does not revive the bridge",
       !runFrames(t, lim, good, 200u, 12000u) && rhyShutdownDead(t));
    ok("and those frames are counted for the log", t.framesAfterDead == 200u);
}

static void testDeviceLostIsNotEnough() {
    std::printf("\n== alt-tab: TestCooperativeLevel says lost, the window is fine ==\n");
    const RhyShutdownLimits lim = shipped();
    RhyShutdownTrack t;
    rhyShutdownTrackInit(t);

    RhyShutdownSignals s = none();
    s.deviceLost = true;
    ok("2.9 s of a lost device is still the alt-tab path",
       !runFrames(t, lim, s, 180u, 0u));               // 2880 ms, device window is 3000
    ok("the window signal is what would end it, and it is off", !rhyShutdownDead(t));
    s.deviceLost = false;
    ok("and when the device comes back the session continues",
       !runFrames(t, lim, none(), 600u, 2896u) && !rhyShutdownDead(t));
    ok("the suspect frames were still recorded", t.suspectFrames >= 180u);
    ok("and the seen mask keeps the history for the post-mortem",
       (t.seen & kRhyShutDevice) != 0u);

    // The same alt-tab that never comes back: Presents fail along with it, and the present window
    // (1500 ms, the shortest of the three) is what ends the session.
    RhyShutdownTrack u;
    rhyShutdownTrackInit(u);
    RhyShutdownSignals p = none();
    p.deviceLost = true;
    p.presentFailed = true;
    ok("1.4 s of failed Presents is still recoverable", !runFrames(u, lim, p, 87u, 0u));
    ok("1.5 s of them is not", runFrames(u, lim, p, 10u, 1392u));
    ok("and the reason names the signal that crossed its window first",
       u.deadReasons == kRhyShutPresent);
    ok("while the log still gets the whole picture from the on mask",
       (u.on & (kRhyShutPresent | kRhyShutDevice)) == (kRhyShutPresent | kRhyShutDevice));
}

static void testCallsNeedCompany() {
    std::printf("\n== our own failing calls are a bad light, not a dead bridge ==\n");
    const RhyShutdownLimits lim = shipped();
    RhyShutdownTrack t;
    rhyShutdownTrackInit(t);

    // One light the server refuses: draw_fail climbs every frame, for a whole session.
    RhyShutdownSignals s = none();
    s.callsFailing = true;
    ok("40 s of failing draws alone never ends the session",
       !runFrames(t, lim, s, 2500u, 0u) && !rhyShutdownDead(t));

    // Now the window goes away too: the same failing calls become evidence.
    s.windowGone = true;
    ok("with the window gone as well, the failing calls end it",
       runFrames(t, lim, s, 60u, 40000u));
    ok("the rule that fired is the calls one",
       t.deadReasons == kRhyShutCalls);
    ok("and the signal that gave it company is still on the line",
       (t.on & kRhyShutWindow) != 0u);
}

static void testZeroWindowsAndDetach() {
    std::printf("\n== a window of 0, and DLL_PROCESS_DETACH ==\n");
    RhyShutdownLimits lim = shipped();
    lim.windowMs = 0;
    RhyShutdownTrack t;
    rhyShutdownTrackInit(t);
    RhyShutdownSignals s = none();
    s.windowGone = true;
    ok("window_ms=0 means one frame is enough", rhyShutdownUpdate(t, s, lim, 500u));

    RhyShutdownTrack d;
    rhyShutdownTrackInit(d);
    ok("detach on a fresh tracker ends it at once", rhyShutdownDetach(d, 700u));
    ok("and names detach", d.deadReasons == kRhyShutDetach);
    ok("a second detach does not re-fire", !rhyShutdownDetach(d, 900u));

    RhyShutdownTrack e;
    rhyShutdownTrackInit(e);
    runFrames(e, shipped(), (RhyShutdownSignals{false, true, false, false}), 10u, 0u);
    ok("detach keeps the signals that were already on in the reason mask",
       rhyShutdownDetach(e, 160u) &&
       (e.deadReasons & kRhyShutDetach) && (e.deadReasons & kRhyShutDevice));
}

static void testWraparound() {
    std::printf("\n== GetTickCount() wraps at ~49.7 days ==\n");
    const RhyShutdownLimits lim = shipped();
    RhyShutdownTrack t;
    rhyShutdownTrackInit(t);
    RhyShutdownSignals s = none();
    s.windowGone = true;

    const unsigned near = 0xFFFFF000u;   // 4096 ms before the wrap
    ok("the signal starts just before the wrap", !rhyShutdownUpdate(t, s, lim, near));
    // 1000 ms later the tick has wrapped to 0x000003E8.
    ok("and it still expires after the wrap", rhyShutdownUpdate(t, s, lim, near + 1000u));
    ok("with the right elapsed time", rhyShutdownElapsed(near + 1000u, near) == 1000u);
    ok("a clock that went backwards past the wrap reads as 0, never as 49 days",
       rhyShutdownElapsed(near, near + 1000u) == 0u);
}

static void testReasonText() {
    std::printf("\n== the log line ==\n");
    char buf[64];
    ok("no bits -> empty text",
       rhyShutdownReasonText(0, buf, sizeof buf)[0] == '\0');
    rhyShutdownReasonText(kRhyShutWindow | kRhyShutPresent, buf, sizeof buf);
    ok("two bits are joined in a fixed order", std::string(buf) == "present+window");
    rhyShutdownReasonText(kRhyShutDetach | kRhyShutCalls | kRhyShutDevice, buf, sizeof buf);
    ok("three bits too", std::string(buf) == "device+calls+detach");
    rhyShutdownReasonText(kRhyShutDetach | kRhyShutCalls | kRhyShutDevice, buf, 10);
    ok("a small buffer truncates but stays terminated and inside the cap",
       std::string(buf).size() < 10u && std::string(buf).size() > 0u);
}

static void testTheLimitOfAnInProcessTracker() {
    std::printf("\n== what this cannot do (why the shell watchdog exists) ==\n");
    // The real 2026-11-12 sequence: the game stops calling Present at 10:39:14.652, its own exit
    // path runs 340 ms later, and the process then spins for two minutes. A tracker fed from
    // Present is never updated again, so it can never declare anything.
    const RhyShutdownLimits lim = shipped();
    RhyShutdownTrack t;
    rhyShutdownTrackInit(t);
    RhyShutdownSignals s = none();
    s.windowGone = true;
    rhyShutdownUpdate(t, s, lim, 1000u);   // one frame sees the window gone
    ok("one frame of evidence, then no more frames at all", !rhyShutdownDead(t));
    ok("and the tracker is still not dead two minutes later, because nothing runs it",
       !rhyShutdownDead(t));
    // DLL_PROCESS_DETACH would fire on a clean exit - which is exactly the case the 2026-11-12 log
    // was missing - but a spinning process never reaches it.
    ok("a clean exit would get its detach line", rhyShutdownDetach(t, 127000u));
}

int main() {
    testTransientSignals();
    testQuitSequence();
    testDeviceLostIsNotEnough();
    testCallsNeedCompany();
    testZeroWindowsAndDetach();
    testWraparound();
    testReasonText();
    testTheLimitOfAnInProcessTracker();
    std::printf("\n%d checks, %d failures\n", g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}
