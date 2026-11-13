// tests/smooth_check.cpp - validates plugin/src/smooth.cpp (s28).
//
// Build:
//   g++ -std=c++17 -Wall -Wextra -o build/tmp/smoothc tests/smooth_check.cpp
//       plugin/src/smooth.cpp
//   ./build/tmp/smoothc && build/tmp/smoothc
//
// rhY's #1 finding (2026-11-12): "the whole-world lighting changes INSTANTLY
// while running or driving". The log from that run says exactly how instantly:
// the street layer went 0 -> 320 live handles, or back, 20 times in 25 minutes;
// rtx.skyBrightness stepped 0.02 -> 1.00 nine times; the sun's computed elevation
// stepped up to 104.5 degrees between two samples.
//
// So the thing under test is not a value, it is a BOUND: no lighting value this
// plugin publishes may move by more than a small amount in one frame, whatever
// the game does underneath. The tests feed the worst sequences the log actually
// recorded - a clock that jumps 22 hours, a player who appears and disappears, a
// car driving at 90 km/h through a lamp field, a whole street switching on - and
// assert the per-frame bounds hold.

#include <chrono>
#include <cmath>
#include <cstdio>
#include <vector>

#include "../plugin/src/game.h"   // the player-position grace window
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

// ---------------------------------------------------------------- helpers

// The config values the shipped rhyremix.ini uses, so the test measures the
// behaviour a player actually gets rather than a convenient fiction.
struct Tuning {
    float step;              // ini fade step
    unsigned budget;         // ini creates_per_frame
    float up, down;          // ini fade in/out seconds
    float rIn, rOut;         // max_distance / exit_distance
    unsigned cap, sticky;    // max_lights / sticky_extra
};

// ------------------------------------------------------------------ tests

static void testRateLimiter() {
    std::printf("\n== RhyRate: the ambient may not step ==\n");

    // The measured worst case: rtx.skyBrightness 0.02 (night) -> 1.00 (day) in
    // one sample, because the game clock jumped 15 hours.
    RhyRate r;
    rhyRateInit(r, 0.02f, 3.0f, 0.25f, 0.002f);
    const float dt = 1.0f / 60.0f;
    float maxStep = 0.0f;
    int framesToNinetyFive = -1;
    float prev = r.cur;
    for (int f = 0; f < 600; ++f) {
        rhyRateStep(r, 1.0f, dt);
        maxStep = std::max(maxStep, std::fabs(r.cur - prev));
        if (framesToNinetyFive < 0 && r.cur >= 0.95f) {
            framesToNinetyFive = f;
        }
        prev = r.cur;
    }
    std::printf("      0.02 -> 1.00: biggest single-frame move %.4f, reached 0.95 "
                "after %.2f s\n", maxStep, framesToNinetyFive / 60.0);
    ok("a 50x ambient step never moves more than 5% of full scale in one frame",
       maxStep <= 0.05f);
    ok("the ambient takes at least half a second to cross day/night",
       framesToNinetyFive >= 30);
    ok("and it does arrive at the target (a rate limit is not a cap)",
       std::fabs(r.cur - 1.0f) < 1e-3f);

    // The other direction, and the floor: the plugin must still be able to reach
    // the 0.02 night value, and 0.001 must be reachable from 1.0.
    maxStep = 0.0f;
    prev = r.cur;
    for (int f = 0; f < 600; ++f) {
        rhyRateStep(r, 0.02f, dt);
        maxStep = std::max(maxStep, std::fabs(r.cur - prev));
        prev = r.cur;
    }
    std::printf("      1.00 -> 0.02: biggest single-frame move %.4f, ended at "
                "%.4f\n", maxStep, r.cur);
    ok("day -> night is bounded too", maxStep <= 0.05f);
    ok("day -> night arrives", std::fabs(r.cur - 0.02f) < 1e-3f);

    // A follower must also survive a target that oscillates every frame, which is
    // what the game clock did this run (21:00, 21:05, 21:02, 21:01, 21:04...).
    RhyRate j;
    rhyRateInit(j, 0.5f, 3.0f, 0.25f, 0.002f);
    maxStep = 0.0f;
    prev = j.cur;
    for (int f = 0; f < 600; ++f) {
        rhyRateStep(j, (f % 2) ? 0.02f : 1.0f, dt);
        maxStep = std::max(maxStep, std::fabs(j.cur - prev));
        prev = j.cur;
    }
    std::printf("      alternating 0.02/1.00 target: published stays at %.3f "
                "(biggest frame %.4f)\n", j.cur, maxStep);
    ok("a target that flips every frame does not make the output flicker",
       maxStep <= 0.05f && j.cur > 0.1f && j.cur < 0.9f);
}

// n22: the exposure anchor. rtx.tonemap.exposureBias is ONE number for the whole
// frame (Global tonemapping, rtx_tone_mapping.cpp:234), so an unthrottled change
// to it is the whole screen brightening - the pop this file exists to prevent. The
// shipped bound is [tonemap] max_ev_per_sec = 0.05 EV/s, i.e. the full day/night
// swing (n16's -0.5 anchor to n22's +0.5 night anchor, 1.0 EV) takes >= 20 s.
//
// Why a second primitive and not RhyRate: an EV is signed and its unit is
// absolute. RhyRate's bound is multiplicative and it clamps cur and target to
// >= 0, so it cannot carry a value that crosses zero - the last check below
// measures exactly that, so the reason cannot be lost.
static void testSignedRate() {
    std::printf("\n== RhySignedRate: the exposure anchor may not step (n22) ==\n");

    const float dt = 1.0f / 60.0f;
    const float evPerSec = 0.05f;   // the shipped [tonemap] max_ev_per_sec

    // The measured worst case for this value: the 2026-11-12 log recorded 104.5
    // degrees of sun elevation between two samples, and the anchor is a function
    // of that elevation - so the TARGET can step the whole 1.0 EV in one frame.
    RhySignedRate r;
    rhySignedRateInit(r, -0.5f, evPerSec, 0.005f);
    float maxStep = 0.0f;
    float prev = r.cur;
    int framesToTarget = -1;
    bool monotone = true, overshot = false;
    for (int f = 0; f < 60 * 60; ++f) {
        rhySignedRateStep(r, 0.5f, dt);   // noon -> night, in one frame
        const float d = r.cur - prev;
        maxStep = std::max(maxStep, std::fabs(d));
        if (d < -1e-6f) {
            monotone = false;
        }
        if (r.cur > 0.5f + 1e-6f) {
            overshot = true;
        }
        if (framesToTarget < 0 && std::fabs(r.cur - 0.5f) <= 0.005f) {
            framesToTarget = f;
        }
        prev = r.cur;
    }
    std::printf("      -0.5 -> +0.5 EV: biggest single-frame move %.5f EV, "
                "on target after %.2f s\n", maxStep, framesToTarget / 60.0);
    // The bound is the slew plus the deadband snap: inside eps the follower is
    // allowed to jump to the target (0.005 EV = 1/200 of a stop, and it is the last
    // move of a ramp, not a step in the middle of one).
    ok("a 1 EV exposure step never moves more than 0.05 EV/s in one frame",
       maxStep <= evPerSec * dt + 0.005f + 1e-6f);
    ok("the full day/night swing takes at least 19 s (0.05 EV/s is a floor, not a cap)",
       framesToTarget >= 60 * 19);
    ok("so its average rate never beat the bound either",
       1.0f / (framesToTarget / 60.0f) <= evPerSec + 1e-3f);
    ok("and it does arrive at the target (a rate limit is not a clamp)",
       std::fabs(r.cur - 0.5f) < 1e-4f);
    ok("the anchor is monotone on the way down (no overshoot, no breathing)",
       monotone && !overshot);

    // The other direction, and the crossing of zero: this is the case RhyRate
    // cannot express. Half an EV up, half an EV down, through 0.
    maxStep = 0.0f;
    prev = r.cur;
    for (int f = 0; f < 60 * 40; ++f) {
        rhySignedRateStep(r, -0.5f, dt);
        maxStep = std::max(maxStep, std::fabs(r.cur - prev));
        prev = r.cur;
    }
    std::printf("      +0.5 -> -0.5 EV: biggest frame %.5f EV, now at %+.4f\n",
                maxStep, r.cur);
    ok("going back down is bounded the same way",
       maxStep <= evPerSec * dt + 0.005f + 1e-6f);
    ok("and it crosses zero and lands (-0.5)", std::fabs(r.cur + 0.5f) < 1e-4f);

    // Why not RhyRate: it clamps to >= 0, so a signed target below zero is
    // unreachable and a value at 0 has no multiplicative room at all.
    RhyRate wrong;
    rhyRateInit(wrong, 0.0f, 3.0f, 0.25f, 0.002f);
    for (int f = 0; f < 600; ++f) {
        rhyRateStep(wrong, -0.5f, dt);
    }
    std::printf("      RhyRate toward -0.5 EV: published %+.4f (it cannot go below 0)\n",
                wrong.cur);
    ok("RhyRate really is unusable for a signed EV (documented, not folklore)",
       wrong.cur == 0.0f);

    // A bound of 0 means frozen, and frozen must mean frozen: the caller cannot
    // get a step out of it by asking for a big dt.
    RhySignedRate frz;
    rhySignedRateInit(frz, -0.5f, 0.0f, 0.005f);
    bool moved = false;
    for (int f = 0; f < 10; ++f) {
        moved = rhySignedRateStep(frz, 4.0f, 1.0f) || moved;
    }
    ok("maxPerSec = 0 freezes the value instead of ignoring the bound",
       !moved && frz.cur == -0.5f);

    // The deadband: inside eps the follower snaps, so it does not crawl toward
    // its target forever, and the snap itself is bounded by eps.
    RhySignedRate d;
    rhySignedRateInit(d, 0.499f, evPerSec, 0.005f);
    const float before = d.cur;
    rhySignedRateStep(d, 0.5f, dt);
    ok("inside the deadband it snaps to the target, by no more than eps",
       d.cur == 0.5f && std::fabs(d.cur - before) <= 0.005f);

    // A target that flips every frame (a clock oscillating across the twilight
    // band) must leave the published value still.
    RhySignedRate alt;
    rhySignedRateInit(alt, 0.0f, evPerSec, 0.005f);
    maxStep = 0.0f;
    prev = alt.cur;
    for (int f = 0; f < 600; ++f) {
        rhySignedRateStep(alt, (f % 2) ? -0.5f : 0.5f, dt);
        maxStep = std::max(maxStep, std::fabs(alt.cur - prev));
        prev = alt.cur;
    }
    std::printf("      alternating -0.5/+0.5 target: published stays at %+.4f "
                "(biggest frame %.5f)\n", alt.cur, maxStep);
    ok("a target that flips every frame does not make the exposure flicker",
       maxStep <= evPerSec * dt + 1e-4f && alt.cur > -0.5f && alt.cur < 0.5f);
}

static void testFades() {
    std::printf("\n== RhyFade: a lamp ramps, it does not switch ==\n");

    RhyFade f;
    rhyFadeInit(f, 0.7f, 0.9f);
    ok("a fresh envelope has no handle and no gain", f.published < 0.0f &&
       f.gain == 0.0f);
    ok("the ini cannot ask for a sub-half-second ramp",
       []() { float up = 0.1f, down = 0.2f; rhyFadeClampRampSeconds(up, down);
              return up >= 0.5f && down >= 0.5f; }());

    const float step = 0.25f;
    const float dt = 1.0f / 60.0f;
    f.target = 1.0f;
    float prevPub = 0.0f;
    float maxDelta = 0.0f;
    int framesToFull = -1;
    int creates = 0;
    for (int i = 0; i < 600; ++i) {
        rhyFadeAdvance(f, dt);
        if (!rhyFadeDue(f, step, 0.02f)) {
            continue;
        }
        const RhyFadeOp op = rhyFadeApply(f, step, 0.02f);
        if (op == kRhyFadeCreate || op == kRhyFadeRecreate) {
            ++creates;
            maxDelta = std::max(maxDelta, std::fabs(f.published - prevPub));
            prevPub = f.published;
        }
        if (framesToFull < 0 && f.published >= 0.999f) {
            framesToFull = i;
        }
    }
    std::printf("      0 -> 1: %d handles, biggest published jump %.3f, full "
                "after %.2f s\n", creates, maxDelta, framesToFull / 60.0);
    ok("one handle never carries more than one step of change",
       maxDelta <= step + 1e-4f);
    ok("coming up takes at least half a second", framesToFull >= 30);
    ok("it does get to full brightness", f.published >= 0.999f);

    // And down: the handle is only retired once the light is actually dark, which
    // is what "no instant destroy" means in code.
    f.target = 0.0f;
    prevPub = f.published;
    maxDelta = 0.0f;
    int framesToDark = -1;
    bool retiredDark = false;
    for (int i = 0; i < 600; ++i) {
        rhyFadeAdvance(f, dt);
        if (!rhyFadeDue(f, step, 0.02f)) {
            continue;
        }
        const RhyFadeOp op = rhyFadeApply(f, step, 0.02f);
        if (op == kRhyFadeRetire) {
            // The apply that retires also lands the last step, so the value the
            // handle carried going out must be within one step of nothing.
            retiredDark = (prevPub <= step + 0.02f);
            framesToDark = i;
            break;
        }
        maxDelta = std::max(maxDelta, std::fabs(f.published - prevPub));
        prevPub = f.published;
    }
    std::printf("      1 -> 0: retired after %.2f s, last published %.3f\n",
                framesToDark / 60.0, prevPub);
    ok("going down takes at least half a second", framesToDark >= 30);
    ok("a handle is retired only from within one step of dark", retiredDark);

    // A cross-fade (signal colour) may be quicker, but not instant.
    RhyFade g;
    rhyFadeInit(g, 0.7f, 0.9f);
    rhyFadeSetRamp(g, 0.25f, 0.25f, 0.05f);
    g.target = 1.0f;
    int frames = 0;
    for (int i = 0; i < 600 && g.published < 0.999f; ++i) {
        rhyFadeAdvance(g, dt);
        if (rhyFadeDue(g, step, 0.02f)) {
            rhyFadeApply(g, step, 0.02f);
        }
        frames = i;
    }
    std::printf("      cross-fade ramp 0.25 s: full after %.2f s\n", frames / 60.0);
    ok("a cross-fade honours its own (shorter) ramp", frames >= 12 && frames <= 30);
    rhyFadeSetRamp(g, 0.001f, 0.001f, 0.05f);
    ok("a cross-fade cannot be asked to be instant",
       g.secondsUp >= 0.05f && g.secondsDown >= 0.05f);
}

static void testDistanceGain() {
    std::printf("\n== rhyDistanceGain: the cut-off becomes a fade band ==\n");
    ok("full gain inside max_distance", rhyDistanceGain(100.0f, 300.0f, 380.0f) == 1.0f);
    ok("zero gain at exit_distance", rhyDistanceGain(380.0f, 300.0f, 380.0f) == 0.0f);
    ok("zero gain beyond exit_distance", rhyDistanceGain(500.0f, 300.0f, 380.0f) == 0.0f);
    const float mid = rhyDistanceGain(340.0f, 300.0f, 380.0f);
    std::printf("      gain at 340 m of a 300..380 band: %.3f\n", mid);
    ok("halfway through the band is half brightness", std::fabs(mid - 0.5f) < 1e-3f);

    // A car at 90 km/h covers 0.42 m per 60 fps frame: the gain a lamp is allowed
    // to lose per frame is therefore tiny.
    float maxStep = 0.0f;
    float prev = rhyDistanceGain(290.0f, 300.0f, 380.0f);
    for (float d = 290.0f; d <= 400.0f; d += 25.0f / 60.0f) {
        const float g = rhyDistanceGain(d, 300.0f, 380.0f);
        maxStep = std::max(maxStep, std::fabs(g - prev));
        prev = g;
    }
    std::printf("      driving out at 90 km/h: biggest per-frame gain change "
                "%.5f\n", maxStep);
    ok("driving past a lamp never steps its gain by more than 1% per frame",
       maxStep <= 0.01f);
    ok("no fade band configured means full gain up to the cut-off (not zero)",
       rhyDistanceGain(100.0f, 300.0f, 300.0f) == 1.0f);
}

static void testSelection() {
    std::printf("\n== rhySelectLights: caps kept, hysteresis kept, gain assigned ==\n");

    // 60 lamps in a line every 20 m, player at the origin.
    std::vector<float> xy;
    for (int i = 0; i < 60; ++i) {
        xy.push_back(static_cast<float>(i) * 20.0f);
        xy.push_back(0.0f);
    }
    std::vector<RhyWant> out(60);
    const unsigned n = rhySelectLights(&xy[0], 60, nullptr, 0, 0.0f, 0.0f,
                                       300.0f, 380.0f, 320, 90, &out[0], 60);
    std::printf("      %u of 60 lamps chosen (rIn 300, rOut 380)\n", n);
    ok("everything inside the wider radius is a candidate (0 m .. 380 m)",
       n == 20);
    ok("nearest first", out[0].dist < out[1].dist && out[1].dist < out[2].dist);
    ok("a lamp at the far edge is chosen DIM, not at full radiance",
       out[n - 2].gain > 0.0f && out[n - 2].gain < 1.0f);
    ok("the one exactly on exit_distance is already at zero gain",
       out[n - 1].gain == 0.0f && out[n - 1].dist <= 380.0f);

    // The cap still binds: 60 candidates, cap 10, sticky 5.
    const unsigned n2 = rhySelectLights(&xy[0], 60, nullptr, 0, 0.0f, 0.0f,
                                        300.0f, 380.0f, 10, 5, &out[0], 60);
    ok("max_lights is still the cap", n2 == 10);
    ok("and it keeps the NEAREST ones", out[9].dist <= 200.0f);

    // Hysteresis: an active light at 360 m survives; an inactive one at 400 m does
    // not exist as a candidate.
    const int active[1] = {18};  // 360 m
    const unsigned n3 = rhySelectLights(&xy[0], 60, active, 1, 0.0f, 0.0f,
                                        300.0f, 380.0f, 2, 0, &out[0], 60);
    bool kept = false;
    for (unsigned i = 0; i < n3; ++i) {
        if (out[i].id == 18) {
            kept = true;
        }
    }
    ok("an already-lit lamp past max_distance is kept (hysteresis)", kept);
    ok("and it is kept dim, which is what makes keeping it free of a pop",
       rhyDistanceGain(360.0f, 300.0f, 380.0f) < 0.3f);

    // Cost guard. This runs on the render thread over the real 7002-lamp list,
    // every resync pass. The first version scanned the active list once per lamp
    // (7002 x 320 comparisons a pass); the sorted + binary-search version has to
    // stay far under a frame, or the fade layer costs more than the pop it fixes.
    std::vector<float> big;
    big.reserve(14004);
    for (int i = 0; i < 7002; ++i) {
        big.push_back(static_cast<float>(i % 100) * 20.0f);
        big.push_back(static_cast<float>(i / 100) * 20.0f);
    }
    std::vector<int> act;
    for (int i = 0; i < 320; ++i) {
        act.push_back(i * 7);  // sorted, as the callers guarantee
    }
    std::vector<RhyWant> bout(7002);
    const auto t0 = std::chrono::steady_clock::now();
    const unsigned bn = rhySelectLights(&big[0], 7002, &act[0], 320, 1000.0f,
                                        1000.0f, 300.0f, 380.0f, 320, 90,
                                        &bout[0], 7002);
    const long us = std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - t0).count();
    std::printf("      7002-lamp selection pass: %u chosen in %ld us\n", bn, us);
    ok("a 7002-lamp selection pass stays under 5 ms (render thread)", us < 5000);
    ok("and it kept the sticky ones it was given", bn > 0 && bn <= 410);
}

static void testDirection() {
    std::printf("\n== rhyDirLimit: the sun cannot teleport ==\n");
    // The measured worst case from the run: the sun's direction moved 104.5 deg
    // between two samples because the game clock jumped.
    float cur[3] = {0.0f, 0.0f, -1.0f};
    const float tgt[3] = {0.94f, 0.0f, 0.34f};  // ~110 deg away
    const float dt = 1.0f / 60.0f;
    const float degPerSec = 12.0f;
    float maxStepDeg = 0.0f;
    int frames = 0;
    for (int i = 0; i < 60 * 30; ++i) {
        float before[3] = {cur[0], cur[1], cur[2]};
        rhyDirLimit(cur, tgt, degPerSec * dt);
        float dot = before[0] * cur[0] + before[1] * cur[1] + before[2] * cur[2];
        dot = dot > 1.0f ? 1.0f : (dot < -1.0f ? -1.0f : dot);
        maxStepDeg = std::max(maxStepDeg, std::acos(dot) * 57.29578f);
        ++frames;
        float n = std::sqrt(cur[0] * cur[0] + cur[1] * cur[1] + cur[2] * cur[2]);
        if (std::fabs(n - 1.0f) > 1e-4f) {
            ok("the slewed direction stays a unit vector", false);
            return;
        }
        if (dot > 0.999999f) {
            break;
        }
    }
    std::printf("      110 deg clock jump: biggest per-frame turn %.3f deg, "
                "took %.2f s\n", maxStepDeg, frames / 60.0);
    ok("the published sun direction never turns more than 0.25 deg in a frame",
       maxStepDeg <= 0.25f);
    ok("and it still gets there (110 deg at 12 deg/s is ~9 s)",
       frames >= 60 && frames <= 60 * 15);
    // Degenerate inputs must not produce NaN.
    float z[3] = {0.0f, 0.0f, 0.0f};
    rhyDirLimit(z, tgt, 5.0f);
    ok("a zero current direction is repaired, not left at zero length",
       std::fabs(std::sqrt(z[0] * z[0] + z[1] * z[1] + z[2] * z[2]) - 1.0f) < 1e-3f);
    // A direction already at the target must not drift. The target here is a
    // proper unit vector: rhyDirLimit normalises what it writes, which is the
    // point (a distant light's direction has to be unit length).
    const float unit[3] = {0.0f, 0.6f, -0.8f};
    float same[3] = {unit[0], unit[1], unit[2]};
    for (int i = 0; i < 1000; ++i) {
        rhyDirLimit(same, unit, 0.0001f);
    }
    ok("already at the target stays at the target (no acos() drift)",
       std::fabs(same[0] - unit[0]) < 1e-6f &&
       std::fabs(same[1] - unit[1]) < 1e-6f &&
       std::fabs(same[2] - unit[2]) < 1e-6f);
}

// ---------------------------------------------------------------- scenario

// The whole loop the lamp modules run, driven by the sequences this run actually
// produced: a player driving at 90 km/h through a lamp field, a street that
// switches on and off, and a player ped pointer that goes to 0 for two seconds
// (finding 3, "the whole world went dark on a cab roof").
//
// It asserts the aggregate - the number that corresponds to "how lit is the world
// I am looking at" - never steps, and that the per-light and per-frame create
// bounds hold.
static void testScenario() {
    std::printf("\n== scenario: drive + street on/off + player lost ==\n");

    const Tuning t = {0.25f, 24, 0.7f, 0.9f, 300.0f, 380.0f, 320, 90};
    const float dt = 1.0f / 60.0f;

    // A lamp field: 900 lamps on a 30 m grid around the road the player drives.
    std::vector<float> xy;
    for (int i = 0; i < 60; ++i) {
        for (int j = 0; j < 15; ++j) {
            xy.push_back(static_cast<float>(i) * 30.0f);
            xy.push_back(static_cast<float>(j) * 30.0f - 210.0f);
        }
    }
    const unsigned nLamps = static_cast<unsigned>(xy.size() / 2);

    struct Live {
        int id;
        RhyFade fade;
    };
    std::vector<Live> live;
    std::vector<RhyWant> want(nLamps);

    float prevAggregate = 0.0f;
    float maxAggregateStep = 0.0f;
    float maxStepAt = -1.0f, maxStepFrom = 0.0f, maxStepTo = 0.0f;
    float maxLightStep = 0.0f;
    unsigned maxCreatesInFrame = 0;
    unsigned maxLive = 0;
    unsigned maxWant = 0;
    bool retiredBright = false;
    // Phase measurements. The layer is on 0-20 s, off 20-25 s (twilight flip), on
    // 25-40 s, off 40-42 s (the player ped pointer going to 0), on 42-60 s.
    float aggOn = -1.0f;      // steady-state output just before the first flip
    float offFallS = -1.0f;   // 20 s flip: seconds to dark
    float onRiseS = -1.0f;    // 25 s flip: seconds back to steady
    float goneFallS = -1.0f;  // 40 s "no player": seconds to dark
    float endAggregate = 0.0f;
    float aggRef = -1.0f;

    for (int f = 0; f < 60 * 60; ++f) {
        const float time = f * dt;
        // 54 km/h along +x. Slow enough that the player stays in the middle of
        // the lamp field for the whole run, so a change in the aggregate means a
        // lighting change and not "the player drove off the street".
        const float px = 20.0f + time * 15.0f;
        const float py = 0.0f;

        // The layer: on, off from 20 s to 25 s (twilight flip), on again, and
        // "no player ped" from 40 s to 42 s (finding 3).
        bool layerOn = !(time >= 20.0f && time < 25.0f) &&
                       !(time >= 40.0f && time < 42.0f);

        unsigned n = 0;
        if (layerOn) {
            std::vector<int> active;
            active.reserve(live.size());
            for (size_t i = 0; i < live.size(); ++i) {
                active.push_back(live[i].id);
            }
            n = rhySelectLights(&xy[0], nLamps, active.empty() ? nullptr : &active[0],
                                static_cast<unsigned>(active.size()), px, py,
                                t.rIn, t.rOut, t.cap, t.sticky, &want[0], nLamps);
        }

        unsigned creates = 0;
        // 1. advance every live light; pay for a step when the budget allows
        for (size_t i = 0; i < live.size();) {
            float target = 0.0f;
            for (unsigned k = 0; k < n; ++k) {
                if (want[k].id == live[i].id) {
                    target = want[k].gain;
                    break;
                }
            }
            live[i].fade.target = target;
            rhyFadeAdvance(live[i].fade, dt);
            if (rhyFadeDue(live[i].fade, t.step, 0.02f) && creates < t.budget) {
                const float before = live[i].fade.published;
                const RhyFadeOp op = rhyFadeApply(live[i].fade, t.step, 0.02f);
                if (op == kRhyFadeRetire) {
                    // A destroy counts against the same per-frame budget a create
                    // does: otherwise a finished fade removes every light in one
                    // frame, which is the pop again.
                    ++creates;
                    // The last value the handle carried must be within one step
                    // (plus the dark epsilon) of nothing: that is what makes
                    // removing it invisible.
                    if (before > t.step + 0.02f) {
                        retiredBright = true;
                    }
                    live[i] = live.back();
                    live.pop_back();
                    continue;
                }
                ++creates;
                maxLightStep = std::max(maxLightStep,
                                        std::fabs(live[i].fade.published - before));
            }
            ++i;
        }
        // 2. lights the selection wants that have no handle yet, nearest first
        for (unsigned k = 0; k < n && creates < t.budget; ++k) {
            bool have = false;
            for (size_t i = 0; i < live.size(); ++i) {
                if (live[i].id == want[k].id) {
                    have = true;
                    break;
                }
            }
            if (have || want[k].gain <= 0.0f) {
                continue;
            }
            Live L;
            L.id = want[k].id;
            rhyFadeInit(L.fade, t.up, t.down);
            L.fade.target = want[k].gain;
            rhyFadeAdvance(L.fade, dt);
            if (!rhyFadeDue(L.fade, t.step, 0.02f)) {
                continue;
            }
            if (rhyFadeApply(L.fade, t.step, 0.02f) != kRhyFadeCreate) {
                continue;
            }
            ++creates;
            live.push_back(L);
        }

        // 3. measure what the renderer would have seen this frame
        float aggregate = 0.0f;
        for (size_t i = 0; i < live.size(); ++i) {
            aggregate += live[i].fade.published > 0.0f ? live[i].fade.published : 0.0f;
        }
        if (f > 0 && std::fabs(aggregate - prevAggregate) > maxAggregateStep) {
            maxAggregateStep = std::fabs(aggregate - prevAggregate);
            maxStepAt = time;
            maxStepFrom = prevAggregate;
            maxStepTo = aggregate;
        }
        prevAggregate = aggregate;
        maxCreatesInFrame = std::max(maxCreatesInFrame, creates);
        maxLive = std::max(maxLive, static_cast<unsigned>(live.size()));
        maxWant = std::max(maxWant, n);

        if (time >= 20.0f && aggOn < 0.0f) {
            aggOn = aggregate;  // the last frame of the first "on" phase
        }
        if (aggOn > 0.0f && offFallS < 0.0f && time >= 20.0f && aggregate <= 0.02f) {
            offFallS = time - 20.0f;
        }
        if (aggOn > 0.0f && onRiseS < 0.0f && time >= 25.0f &&
            aggregate >= 0.9f * aggOn) {
            onRiseS = time - 25.0f;
        }
        if (goneFallS < 0.0f && time >= 40.0f && aggregate <= 0.02f) {
            goneFallS = time - 40.0f;
        }
        if (time >= 55.0f && aggRef < 0.0f) {
            aggRef = aggregate;  // steady state of the last "on" phase
        }
        if (time >= 59.5f) {
            endAggregate = aggregate;
        }
    }

    std::printf("      lamps in the field %u, peak wanted %u, peak live %u, "
                "steady output %.1f gain-units, peak creates/frame %u\n", nLamps,
                maxWant, maxLive, aggOn, maxCreatesInFrame);
    std::printf("      biggest one-frame change in total lit output: %.3f "
                "(%.2f%% of steady)\n", maxAggregateStep,
                100.0f * maxAggregateStep / (aggOn > 0.0f ? aggOn : 1.0f));
    std::printf("      ...that step happened at t=%.2f s: %.2f -> %.2f\n", maxStepAt,
                maxStepFrom, maxStepTo);
    std::printf("      last on-phase: %.2f at 55 s, %.2f at 59.5 s\n", aggRef,
                endAggregate);
    std::printf("      biggest one-frame change in one lamp: %.3f\n", maxLightStep);
    std::printf("      street off -> dark %.2f s | back on -> steady %.2f s | "
                "player lost -> dark %.2f s\n", offFallS, onRiseS, goneFallS);

    ok("the per-frame create budget is honoured", maxCreatesInFrame <= t.budget);
    ok("the light cap (max_lights + sticky_extra) is honoured", maxWant <= t.cap + t.sticky);
    ok("handles still in their fade-out stay bounded (no leak)",
       maxLive <= 2u * (t.cap + t.sticky));
    ok("no individual lamp steps by more than one configured step",
       maxLightStep <= t.step + 1e-3f);
    ok("the world's total lit output never steps by more than 5% of itself",
       maxAggregateStep <= 0.05f * aggOn);
    ok("a handle is never retired from more than one step of brightness",
       !retiredBright);
    ok("the street takes at least half a second to go dark", offFallS >= 0.5f);
    ok("the street takes at least half a second to come back on", onRiseS >= 0.5f);
    ok("losing the player ped fades the street out, it does not cut it",
       goneFallS >= 0.5f);
    ok("and the lighting is back afterwards (a fade is not a shutdown)",
       aggRef > 0.0f && endAggregate >= 0.9f * aggRef);
}

// finding 3: "the whole world went dark on a cab roof, then came back". The ped
// pointer at 0xB7CD98 reads 0 in a dozen ordinary states; this run it read 0 for
// 13 seconds from 01:09:20, and every module took its no-player branch, which used
// to destroy every light in the world in one frame.
static void testPlayerGrace() {
    std::printf("\n== rhyPlayerTrackUpdate: losing the ped is not a lighting event ==\n");
    RhyPlayerTrack t;
    rhyPlayerTrackInit(t);
    float held[3] = {0.0f, 0.0f, 0.0f};
    const unsigned grace = 8000;

    ok("no player yet: Gone, because there is nothing to hold on to",
       rhyPlayerTrackUpdate(t, false, nullptr, grace, 1000, held) == kRhyPlayerGone);

    const float p[3] = {10.0f, 20.0f, 30.0f};
    ok("ped present: Ok",
       rhyPlayerTrackUpdate(t, true, p, grace, 2000, held) == kRhyPlayerOk);
    ok("and the position is the fresh one", held[0] == 10.0f && held[2] == 30.0f);

    bool allGrace = true;
    for (unsigned f = 1; f <= 240; ++f) {  // 4 s of pointer = 0, at 60 fps
        if (rhyPlayerTrackUpdate(t, false, nullptr, grace, 2000 + f * 16, held) !=
            kRhyPlayerGrace) {
            allGrace = false;
        }
    }
    ok("4 s of ped pointer = 0 stays inside the grace window", allGrace);
    ok("and keeps reporting the last known position (selection keeps working)",
       held[0] == 10.0f && held[1] == 20.0f && held[2] == 30.0f);
    ok("one disappearance counts as ONE event, not one per frame",
       t.lostEvents == 1);

    ok("past the grace window: Gone",
       rhyPlayerTrackUpdate(t, false, nullptr, grace, 2000 + 9000, held) ==
       kRhyPlayerGone);
    ok("Gone still hands back a position: the caller fades the layers out, it does "
       "not blank them", held[0] == 10.0f);

    const float q[3] = {40.0f, 50.0f, 60.0f};
    ok("the ped coming back is Ok",
       rhyPlayerTrackUpdate(t, true, q, grace, 12000, held) == kRhyPlayerOk);
    ok("and the position follows again", held[1] == 50.0f);

    // GetTickCount() wraps at ~49.7 days; the window has to be measured with
    // wraparound-safe arithmetic or the grace would end early (or never).
    RhyPlayerTrack w;
    rhyPlayerTrackInit(w);
    const unsigned near = 0xFFFFFF00u;  // 256 ms before the wrap
    rhyPlayerTrackUpdate(w, true, p, grace, near, held);
    bool acrossWrap = true;
    for (unsigned i = 1; i <= 60; ++i) {
        if (rhyPlayerTrackUpdate(w, false, nullptr, grace, near + i * 16, held) !=
            kRhyPlayerGrace) {
            acrossWrap = false;
        }
    }
    ok("the grace window survives the GetTickCount() wrap", acrossWrap);
    ok("and a window measured across the wrap still expires",
       rhyPlayerTrackUpdate(w, false, nullptr, grace, near + 60000u, held) ==
       kRhyPlayerGone);
}

int main() {
    testRateLimiter();
    testSignedRate();
    testFades();
    testDistanceGain();
    testSelection();
    testDirection();
    testPlayerGrace();
    testScenario();
    std::printf("\n%d checks, %d failures\n", g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}
