// tests/drain_spread_check.cpp - n08: the work the plugin owns must never be a
// burst, because the frame it lands on has no headroom.
//
// Build (this is what scripts/check-all.sh runs):
//   g++ -std=c++17 -Wall -Wextra -Iplugin/src tests/drain_spread_check.cpp
//       plugin/src/smooth.cpp -o build/tmp/drainc
//   ./build/tmp/drainc
//
// WHY THESE ARE THE CHECKS (finding G of the 2026-11-12 10:22 report: "FPS a
// little less steady than previous builds")
// ---------------------------------------------------------------------------
// tools/perf-report.py over the two MangoHud CSVs of 2026-11-12 says the problem
// is the tail, not the average:
//
//   00:48 run (2f67e4a)  frametime mean 17.72 ms, sd 4.21, 1869 frames > 25 ms = 76.9/min
//   10:22 run (ba4c2c3)  frametime mean 17.50 ms, sd 4.67, 2242 frames > 25 ms = 137.2/min
//
// and the MEDIAN of both runs is 16.68 ms - exactly the 60 Hz budget. A frame
// that is already at the budget turns into a spike when anything else lands on
// it, so the plugin's job is to make sure its own work is spread out even when
// the queue behind it is full. Two things in the plugin were not:
//
//   * rhyFlushDelayed() destroyed EVERY due handle in one frame. The runs' own
//     logs measured that queue at 320 (00:48) and 205 (10:22) handles.
//   * the street and the signal layer re-scanned their lists on the same frame
//     (both used `frame % 30 == 1`): 11.7 us (7002 lamps) + 8.0 us (1522 signals)
//     landing together, every 30 frames, forever.
//
// smooth.h's RhyDrainQueue and rhyResyncDue are the fix, and this file asserts
// the two properties that matter: no frame processes more than the bound, and two
// modules with the same period and different phases never run together. The
// measured microsecond costs are in docs/FINDINGS.md - they are not asserted
// here, because a timing assertion on a shared machine is a flaky test.

#include <cstdint>
#include <cstdio>
#include <vector>

#include "../plugin/src/smooth.h"

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

// ------------------------------------------------------------------ the queue

// 320 handles retired at once (a whole street layer fading out: the 00:48 run's
// measured worst) and every one of them becomes due at the same tick.
static void testBoundPerFrame() {
    RhyDrainQueue<int> q;
    const unsigned due = 1000u;
    for (int i = 0; i < 320; ++i) {
        q.add(due, i);
    }
    ok("the queue reports all 320 as due", q.dueCount(due + 1u) == 320u);

    unsigned frame = 0;
    unsigned maxInFrame = 0;
    unsigned total = 0;
    int out[64];
    while (!q.empty()) {
        const size_t n = q.takeDue(due + 1u, 32u, out, 64);
        if (n > maxInFrame) {
            maxInFrame = static_cast<unsigned>(n);
        }
        total += static_cast<unsigned>(n);
        ++frame;
        if (frame > 100u) {
            break;  // the bound must not stall the drain, only spread it
        }
    }
    std::printf("      320 handles due at once, destroys_per_frame=32: "
                "worst frame %u, drained in %u frames\n", maxInFrame, frame);
    ok("no frame destroys more than the bound", maxInFrame <= 32u);
    ok("everything still gets destroyed", total == 320u);
    ok("a 320-handle retirement costs at most 10 frames", frame <= 10u);
    // 10 frames at 60 fps = 167 ms: inside destroy_delay_ms (500 ms default), so
    // spreading cannot delay a handle past the delay that protects the bridge.
    ok("the drain stays inside destroy_delay_ms", frame * 16u <= 500u);
}

// The pre-n08 behaviour is still reachable, and it is what 0 puts back.
static void testUnboundedEscapeHatch() {
    RhyDrainQueue<int> q;
    for (int i = 0; i < 320; ++i) {
        q.add(10u, i);
    }
    int out[512];
    const size_t n = q.takeDue(11u, 0u, out, 512);  // perFrame 0 = no bound
    ok("perFrame=0 flushes the whole queue in one frame (the old behaviour)",
       n == 320u && q.empty());
}

// Order: the handle that has been waiting longest goes first, so a bounded drain
// cannot starve an old handle behind a stream of new ones.
static void testOldestDueFirst() {
    RhyDrainQueue<int> q;
    for (int i = 0; i < 5; ++i) {
        q.add(100u + static_cast<unsigned>(i) * 10u, i);
    }
    q.add(50u, 999);  // arrives last, due first
    int out[8];
    const size_t n = q.takeDue(200u, 3u, out, 8);
    ok("a bounded take returns exactly the bound", n == 3u);
    ok("the earliest-due item is taken first", out[0] == 999);
    ok("then in due order", out[1] == 0 && out[2] == 1);
    const size_t m = q.takeDue(200u, 32u, out, 8);
    ok("the rest follow", m == 3u && out[0] == 2 && out[1] == 3 && out[2] == 4);
}

// Not due yet stays; and the tick comparison survives the GetTickCount() wrap
// (49.7 days of uptime, which a long session does reach).
static void testNotDueAndWrap() {
    RhyDrainQueue<int> q;
    q.add(1000u, 1);
    q.add(2000u, 2);
    int out[4];
    ok("only the due item is taken", q.takeDue(1500u, 32u, out, 4) == 1u && out[0] == 1);
    ok("the not-yet-due item is still queued", q.size() == 1u);

    RhyDrainQueue<int> w;
    const unsigned nearWrap = 0xFFFFFFFFu - 500u;
    w.add(nearWrap, 1);
    w.add(nearWrap + 900u, 2);  // wraps to a small tick
    // now is past both, and both comparisons must survive the wrap
    const unsigned now = nearWrap + 1200u;
    ok("wrap: both are due", w.dueCount(now) == 2u);
    const size_t n = w.takeDue(now, 32u, out, 4);
    ok("wrap: taken in due order across the wrap", n == 2u && out[0] == 1 && out[1] == 2);
    // and one that has NOT wrapped yet is not due, even though its raw tick is huge
    RhyDrainQueue<int> p;
    p.add(nearWrap + 900u, 2);
    ok("wrap: an item whose due tick is still ahead is not due",
       p.dueCount(nearWrap + 600u) == 0u);

    // A tick 2^31 in the future is NOT due (the wrap-safe comparison must not
    // read a far-future deadline as a past one).
    RhyDrainQueue<int> f;
    f.add(now + 0x7FFFFFF0u, 7);
    ok("a deadline far in the future is not due", f.dueCount(now) == 0u);
}

// rhyFlushDelayedTag() is the correctness path (same hash: the destroy has to
// land before the replacement create), so the queue needs a targeted removal.
static void testRemoveIf() {
    RhyDrainQueue<int> q;
    for (int i = 0; i < 10; ++i) {
        q.add(10u, i);
    }
    const size_t removed = q.removeIf([](int v) { return v % 3 == 0; });
    ok("removeIf reports what it removed", removed == 4u);  // 0,3,6,9
    ok("the rest are untouched", q.size() == 6u);
    int out[8];
    const size_t n = q.takeDue(11u, 32u, out, 8);
    bool order = n == 6u;
    const int want[6] = {1, 2, 4, 5, 7, 8};
    for (size_t i = 0; i < n && i < 6; ++i) {
        order = order && out[i] == want[i];
    }
    ok("removeIf keeps the remaining order", order);
}

// ------------------------------------------------------------- the schedule

// The regression guard first: phase 0 must be exactly the old `frame % 30 == 1`,
// so the street layer keeps the frame it has always re-selected on (including
// the first frame, which is when a layer first appears).
static void testPhaseZeroIsTheOldRule() {
    bool same = true;
    for (unsigned f = 0; f < 10000u; ++f) {
        const bool oldRule = (f % 30u) == 1u;
        const bool now = rhyResyncDue(f, 30u, 0u) != 0u;
        if (oldRule != now) {
            same = false;
            break;
        }
    }
    ok("phase 0 reproduces `frame % 30 == 1` exactly", same);
    // Both modules pre-increment their frame counter, so the layer's first frame
    // is g_frame == 1, which is exactly the slot phase 0 fires on.
    ok("the layer's first frame is a due frame", rhyResyncDue(1u, 30u, 0u) == 1u);
}

// The point of the phase: two modules on the same period never share a frame.
static void testTwoModulesNeverShareAFrame() {
    unsigned both = 0, street = 0, signal = 0;
    for (unsigned f = 0; f < 100000u; ++f) {
        const bool a = rhyResyncDue(f, 30u, 0u) != 0u;
        const bool b = rhyResyncDue(f, 30u, 15u) != 0u;
        both += (a && b) ? 1u : 0u;
        street += a ? 1u : 0u;
        signal += b ? 1u : 0u;
    }
    std::printf("      100000 frames, period 30: street %u scans, signal %u scans, "
                "frames carrying both %u\n", street, signal, both);
    ok("the two scans never land on the same frame", both == 0u);
    ok("neither module scans less often than before",
       street + 1u >= 100000u / 30u && street <= 100000u / 30u + 1u &&
       signal + 1u >= 100000u / 30u && signal <= 100000u / 30u + 1u);
}

// A period of 1 or 0 means "every frame" and must stay that way whatever the
// phase (an ini of resync_frames=1 is legal and means "scan every frame").
static void testDegeneratePeriods() {
    ok("period 1 is every frame", rhyResyncDue(7u, 1u, 0u) == 1u);
    ok("period 0 is every frame", rhyResyncDue(7u, 0u, 3u) == 1u);
    ok("a phase larger than the period is still one frame in N",
       rhyResyncDue(10u, 5u, 5u) == rhyResyncDue(10u, 5u, 0u));
}

// ------------------------------------------------- the whole thing, together

// The 00:48 run's worst case, replayed: a whole street layer (320 handles) is
// retired at once, destroy_delay_ms=500 later they all become due, and the frame
// they become due on is already a 16.7 ms frame. With the bound, the worst frame
// holds 32 destroys; without it, 320.
static void testRetirementReplay() {
    const unsigned kFrames = 1200u;         // 20 s at 60 fps
    const unsigned kFrameMs = 16u;          // one frame of a 60 fps run
    const unsigned delay = 500u;            // destroy_delay_ms
    unsigned worstBounded = 0, worstUnbounded = 0;
    RhyDrainQueue<int> a, b;
    for (unsigned f = 0; f < kFrames; ++f) {
        const unsigned now = f * kFrameMs;
        if (f == 100u) {              // the layer retires here
            for (int i = 0; i < 320; ++i) {
                a.add(now + delay, i);
                b.add(now + delay, i);
            }
        }
        int out[512];
        const size_t n = a.takeDue(now, 32u, out, 512);
        if (n > worstBounded) {
            worstBounded = static_cast<unsigned>(n);
        }
        const size_t m = b.takeDue(now, 0u, out, 512);  // the old flush
        if (m > worstUnbounded) {
            worstUnbounded = static_cast<unsigned>(m);
        }
    }
    std::printf("      320 handles retired at once, 500 ms delay: worst frame "
                "bounded %u destroys, unbounded %u\n", worstBounded, worstUnbounded);
    ok("the bounded drain never exceeds the per-frame bound", worstBounded <= 32u);
    ok("the old flush really did put them all in one frame", worstUnbounded == 320u);
    ok("and the bounded queue is empty afterwards", a.empty() && b.empty());
}

int main() {
    std::printf("n08 frame-pacing checks (smooth.cpp drain queue + resync schedule)\n");
    testBoundPerFrame();
    testUnboundedEscapeHatch();
    testOldestDueFirst();
    testNotDueAndWrap();
    testRemoveIf();
    testPhaseZeroIsTheOldRule();
    testTwoModulesNeverShareAFrame();
    testDegeneratePeriods();
    testRetirementReplay();
    std::printf("\n%d checks, %d failures\n", g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}
