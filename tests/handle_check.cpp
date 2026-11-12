// tests/handle_check.cpp - the distant-light handle cadence (n01, closes s29 T7).
//
// Build:
//   g++ -std=c++17 -Wall -Wextra -Iplugin/src -o build/tmp/handlec
//       tests/handle_check.cpp plugin/src/smooth.cpp
//   ./build/tmp/handlec
//
// What is under test is WHEN a Remix distant light's handle is replaced. The
// runtime bakes direction and radiance at CreateLight time, so a moving sun is a
// sequence of destroy+create pairs and each pair is a hard step in shadow
// direction. s28/s29 chose that moment from the value: rebuild once the handle is
// more than [sun] step_deg (1.0 deg) out of date, or 5% out of date in luminance.
// s29's notes left it open as T7 because tightening the threshold costs one
// create per body per frame. n01 replaces the trigger with a clock:
// rhyCadence* in plugin/src/smooth.cpp.
//
// The same simulated sky runs twice - once through the old threshold rule, once
// through the cadence - and the two are compared on what a player can see: how
// many bridge creates per second, how large a step each rebuild carries, and how
// evenly spaced the rebuilds are. Everything downstream of the decision
// (rhyDirLimit, RhyRate, the fade model) is identical in both runs, so the
// difference is the decision rule alone.

#include <algorithm>
#include <cmath>
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

static const float kDeg = 0.017453292519943295f;

// The same helper the plugin uses (atan2-based, not acos: see smooth.h).
static float angleDeg(const float* a, const float* b) { return rhyAngleDeg(a, b); }

// ---------------------------------------------------------------- the knobs

// The shipped values, so the test measures what a player gets rather than a
// convenient fiction. stepDeg is [sun] step_deg from config/rhyremix-production.ini:
// the OLD trigger and, with handle_step_deg=0, the NEW direction budget.
struct Tuning {
    float degPerSec;     // [fade] sun_deg_per_sec: the s28 direction slew limit
    float relPerSec;     // [fade] sun_rel_per_sec: the s28 radiance rate limit
    float absPerSec;     // [fade] sky_abs_per_sec
    float stepDeg;       // [sun] step_deg
    int   cadenceMs;     // [fade] handle_cadence_ms
    int   cadenceMinMs;  // [fade] handle_cadence_min_ms
    float budgetDeg;     // [fade] handle_step_deg
    float budgetRel;     // [fade] handle_rel_lum
};

static Tuning shipped() {
    Tuning t;
    t.degPerSec = 12.0f;
    t.relPerSec = 3.0f;
    t.absPerSec = 0.25f;
    t.stepDeg = 1.0f;
    t.cadenceMs = 2000;
    t.cadenceMinMs = 150;
    t.budgetDeg = 0.5f;   // the sun's disc is 0.53 deg
    t.budgetRel = 0.05f;
    return t;
}

// ------------------------------------------------------------------ the sky

enum SkyKind {
    kSkySteady = 0,   // a normal morning: 0.25 deg of sun per game minute
    kSkyJump,         // the measured clock jump: 104.5 deg of elevation in one sample
    kSkyJumpNow,      // ... and the same jump at t=0, so the window IS the slew
    kSkyPin,          // SARemix_Tour.lua's setTimeOfDay() sawtooth
    kSkyDusk          // the sun's radiance ramping 6.0 -> 0.02 across twilight
};

// Per-frame targets for one body: a direction that rotates about the world Z at
// 0.125 deg/s (0.25 deg per 2000 ms game minute, the rate s29 measured) and a
// luminance. The published state is whatever the limiters make of it.
struct Sky {
    std::vector<float> dir;   // 3 per frame
    std::vector<float> lum;
    double seconds;
    float dt;
};

static Sky makeSky(SkyKind kind, double seconds, float dt) {
    Sky s;
    s.seconds = seconds;
    s.dt = dt;
    const int frames = static_cast<int>(seconds / dt);
    s.dir.resize(3 * frames);
    s.lum.resize(frames);
    for (int f = 0; f < frames; ++f) {
        const double t = f * dt;
        double ang = 0.125 * t;  // deg: the real sky's rate at this latitude
        double lum = 6.0;
        if (kind == kSkyJump && t >= 20.0) {
            ang += 104.5;        // the 2026-11-12 log's worst sample step
        }
        if (kind == kSkyJumpNow && t >= 0.02) {
            ang += 104.5;  // the jump lands on frame 1: the window IS the slew
        }
        if (kind == kSkyPin && t >= 10.0) {
            // The tour pins the clock twice per stop; the log's sawtooth was
            // 1260,1265,1262,1261,1264,1260,... i.e. +/- ~1.5 deg of sun.
            const int step = static_cast<int>((t - 10.0) / 5.0);
            ang += (step % 2) ? -1.5 : 1.5;
        }
        if (kind == kSkyDusk && t >= 20.0) {
            lum = 0.02;          // a 300x radiance step, twilight in one sample
        }
        const double a = ang * kDeg;
        s.dir[3 * f + 0] = static_cast<float>(std::cos(a));
        s.dir[3 * f + 1] = static_cast<float>(std::sin(a));
        s.dir[3 * f + 2] = 0.0f;
        s.lum[f] = static_cast<float>(lum);
    }
    return s;
}

// One body: the published (rate-limited) state, and the handle built from it.
struct Body {
    float want[3];
    float pub[3];
    float wantLum, pubLum;
    RhyRate rate;
    bool have;
    float hdir[3];
    float hlum;
};

static void bodyInit(Body& b, const Sky& sky, const Tuning& t) {
    for (int i = 0; i < 3; ++i) {
        b.want[i] = b.pub[i] = b.hdir[i] = sky.dir[i];
    }
    b.wantLum = b.pubLum = sky.lum[0];
    rhyRateInit(b.rate, sky.lum[0], t.relPerSec, t.absPerSec, 1e-4f);
    b.have = true;
    b.hlum = sky.lum[0];
}

// The followers, identical for both rules: this is the s28 layer, unchanged.
static void advance(Body& b, const Sky& sky, int f, const Tuning& t,
                    float* maxFrameDeg) {
    b.want[0] = sky.dir[3 * f + 0];
    b.want[1] = sky.dir[3 * f + 1];
    b.want[2] = sky.dir[3 * f + 2];
    b.wantLum = sky.lum[f];
    const float dt = sky.dt;
    const float before[3] = {b.pub[0], b.pub[1], b.pub[2]};
    rhyRateStep(b.rate, b.wantLum, dt);
    b.pubLum = b.rate.cur;  // what the next handle would be created with
    rhyDirLimit(b.pub, b.want, t.degPerSec * dt);
    *maxFrameDeg = std::max(*maxFrameDeg, angleDeg(before, b.pub));
}

struct Stats {
    double creates;
    double skips;
    double seconds;
    float maxStepDeg;   // largest direction step one rebuild carried
    float maxRelLum;    // largest fractional luminance change one rebuild carried
    int minGapMs, maxGapMs;
    float maxFrameDeg;  // the s28 bound, same for both rules
    float maxRelRate;   // steepest observed luminance rate, fraction/s
};

static void statsInit(Stats& s) {
    s.creates = 0;
    s.skips = 0;
    s.seconds = 0.0;
    s.maxStepDeg = 0.0f;
    s.maxRelLum = 0.0f;
    s.minGapMs = 1 << 30;
    s.maxGapMs = 0;
    s.maxFrameDeg = 0.0f;
    s.maxRelRate = 0.0f;
}

static void takeHandle(Body& b) {
    b.have = true;
    b.hdir[0] = b.pub[0];
    b.hdir[1] = b.pub[1];
    b.hdir[2] = b.pub[2];
    b.hlum = b.pubLum;
}

// s28/s29 (the open note T7): rebuild when the handle is more than step_deg out
// of date, or the luminance moved by more than 5%.
static Stats runOldRule(const Sky& sky, const Tuning& t) {
    Stats s;
    statsInit(s);
    s.seconds = sky.seconds;
    Body b;
    bodyInit(b, sky, t);
    int gapMs = 0;
    const int frames = static_cast<int>(sky.seconds / sky.dt);
    for (int f = 0; f < frames; ++f) {
        advance(b, sky, f, t, &s.maxFrameDeg);
        gapMs += static_cast<int>(sky.dt * 1000.0f + 0.5f);
        const float moved = angleDeg(b.hdir, b.pub);
        const float mean = 0.5f * (b.hlum + b.pubLum);
        if (mean > 1e-6f) {
            s.maxRelLum = std::max(s.maxRelLum, std::fabs(b.pubLum - b.hlum) / mean);
        }
        const bool dimChanged =
            std::fabs(b.pubLum - b.hlum) > 0.05f * (b.pubLum + b.hlum);
        if (!b.have || moved > t.stepDeg || dimChanged) {
            s.maxStepDeg = std::max(s.maxStepDeg, moved);
            ++s.creates;
            s.minGapMs = std::min(s.minGapMs, gapMs);
            s.maxGapMs = std::max(s.maxGapMs, gapMs);
            gapMs = 0;
            takeHandle(b);
        }
    }
    return s;
}

// n01: rebuild on the cadence. The "handle already matches" branch is the one
// plugin/src/sun.cpp takes for a pinned clock: the period still passes, but no
// bridge call is made for a change that does not exist.
static Stats runCadence(const Sky& sky, const Tuning& t) {
    Stats s;
    statsInit(s);
    s.seconds = sky.seconds;
    Body b;
    bodyInit(b, sky, t);
    RhyCadence cad;
    rhyCadenceInit(cad, t.budgetDeg, t.budgetRel, t.cadenceMinMs, t.cadenceMs);
    float prevDir[3] = {b.pub[0], b.pub[1], b.pub[2]};
    float prevLum = b.pubLum;
    const int frames = static_cast<int>(sky.seconds / sky.dt);
    for (int f = 0; f < frames; ++f) {
        advance(b, sky, f, t, &s.maxFrameDeg);

        const float degMoved = angleDeg(prevDir, b.pub);
        const float mean = 0.5f * (b.pubLum + prevLum);
        const float relMoved = mean > 1e-6f
                                   ? std::fabs(b.pubLum - prevLum) / mean
                                   : 0.0f;
        prevDir[0] = b.pub[0];
        prevDir[1] = b.pub[1];
        prevDir[2] = b.pub[2];
        prevLum = b.pubLum;

        s.maxRelRate = std::max(s.maxRelRate, relMoved / sky.dt);
        rhyCadenceObserve(cad, degMoved, relMoved, sky.dt);
        rhyCadenceAdvance(cad, sky.dt);
        // what the live handle is off by, which is also the safety-net trigger
        const float hmean = 0.5f * (b.hlum + b.pubLum);
        rhyCadenceSetGap(cad, angleDeg(b.hdir, b.pub),
                         hmean > 1e-6f ? std::fabs(b.pubLum - b.hlum) / hmean : 0.0f);
        if (rhyCadenceDue(cad)) {
            const float moved = angleDeg(b.hdir, b.pub);
            const bool sameLum =
                std::fabs(b.pubLum - b.hlum) <=
                1e-6f * (b.pubLum + b.hlum + 1.0f);
            if (moved < 1e-4f && sameLum) {
                ++s.skips;
            } else {
                s.maxStepDeg = std::max(s.maxStepDeg, moved);
                const float hmean = 0.5f * (b.hlum + b.pubLum);
                if (hmean > 1e-6f) {
                    s.maxRelLum = std::max(s.maxRelLum,
                                           std::fabs(b.pubLum - b.hlum) / hmean);
                }
                ++s.creates;
                s.minGapMs = std::min(s.minGapMs, cad.elapsedMs);
                s.maxGapMs = std::max(s.maxGapMs, cad.elapsedMs);
                takeHandle(b);
            }
            rhyCadenceRebuilt(cad);
        }
    }
    return s;
}

static void report(const char* what, const Stats& o, const Stats& n) {
    std::printf("      %-12s old: %6.2f creates/s  step<=%6.2f deg  gap %5d..%5d ms\n",
                what, o.creates / o.seconds, o.maxStepDeg, o.minGapMs, o.maxGapMs);
    std::printf("      %-12s new: %6.2f creates/s  step<=%6.2f deg  gap %5d..%5d ms"
                "  (%.0f skipped)\n",
                "", n.creates / n.seconds, n.maxStepDeg, n.minGapMs, n.maxGapMs,
                n.skips);
}

// ------------------------------------------------------------------- the sky

static void testSteadyDay() {
    std::printf("\n== a normal morning: what does each rule cost? ==\n");
    const Tuning t = shipped();
    const Sky sky = makeSky(kSkySteady, 120.0, 1.0f / 60.0f);
    const Stats o = runOldRule(sky, t);
    const Stats n = runCadence(sky, t);
    report("steady", o, n);

    // The old rule's step IS its threshold: it rebuilds when the error has grown
    // to step_deg, so every rebuild moves the shadow direction by ~1 deg - twice
    // the sun's 0.53 deg disc - and does it every ~8 s (1 deg / 0.125 deg per s).
    ok("the old rule's rebuild carries the whole 1.0 deg threshold",
       o.maxStepDeg > 0.99f);
    ok("the old rule rebuilds about every 8 s at this rate",
       o.minGapMs > 7000 && o.maxGapMs < 9000);
    ok("the old rule costs about 0.13 creates/s per body",
       o.creates / o.seconds > 0.10 && o.creates / o.seconds < 0.20);

    // The cadence: a fixed 2 s period, each rebuild carrying a quarter of a
    // degree (0.125 deg/s x 2 s), i.e. 4x finer than the threshold rule.
    ok("the cadence is the fixed period it claims to be",
       n.minGapMs >= t.cadenceMs && n.maxGapMs <= t.cadenceMs + 20);
    ok("one rebuild carries at most the 0.5 deg direction budget",
       n.maxStepDeg <= t.budgetDeg + 1e-3f);
    ok("and in practice a quarter of a degree, 4x finer than the old rule",
       n.maxStepDeg < 0.30f && n.maxStepDeg < o.maxStepDeg / 3.0f);
    ok("churn stays under one create per second per body",
       n.creates / n.seconds <= 1.0 / (t.cadenceMs / 1000.0) + 0.01);
    ok("the s28 per-frame direction bound is untouched by the new rule",
       n.maxFrameDeg <= t.degPerSec / 60.0f + 1e-3f &&
       std::fabs(n.maxFrameDeg - o.maxFrameDeg) < 1e-3f);
}

static void testClockJump() {
    std::printf("\n== the measured 104.5 deg clock jump ==\n");
    const Tuning t = shipped();
    const Sky sky = makeSky(kSkyJump, 120.0, 1.0f / 60.0f);
    const Stats o = runOldRule(sky, t);
    const Stats n = runCadence(sky, t);
    report("jump", o, n);

    // The window that matters is the slew itself: the direction limiter is at its
    // 12 deg/s cap for 8.7 s, and the old rule fires every time the error passes
    // 1 deg - 12 rebuilds/s for one body. The cadence's floor (150 ms) is the
    // churn bound there, and it is also the step bound: 12 deg/s x 0.15 s.
    const Sky slew = makeSky(kSkyJumpNow, 12.0, 1.0f / 60.0f);
    const Stats os = runOldRule(slew, t);
    const Stats ns = runCadence(slew, t);
    report("slew", os, ns);
    ok("the threshold rule rebuilds ~8-12 times a second while slewing",
       os.creates / os.seconds > 6.0);
    ok("the cadence rebuilds at most at its floor while slewing",
       ns.creates / ns.seconds <= 1000.0 / t.cadenceMinMs + 1.0);
    ok("which is about half the old rule's churn in the same window",
       ns.creates < os.creates * 0.75);
    ok("a slew step never exceeds what the floor allows (12 deg/s x 150 ms)",
       ns.maxStepDeg <= t.degPerSec * t.cadenceMinMs / 1000.0f + 0.25f);
    ok("the published direction still moves at most sun_deg_per_sec per second",
       ns.maxFrameDeg <= t.degPerSec / 60.0f + 1e-3f);
    ok("and the jump does not make the whole run busier than the floor allows",
       n.creates / n.seconds <= 1000.0 / t.cadenceMinMs + 1.0);
}

static void testPinWobble() {
    std::printf("\n== the tour's clock pin (an A/B/A/B in the target) ==\n");
    const Tuning t = shipped();
    const Sky steady = makeSky(kSkySteady, 120.0, 1.0f / 60.0f);
    const Sky pin = makeSky(kSkyPin, 120.0, 1.0f / 60.0f);
    const Stats oSteady = runOldRule(steady, t);
    const Stats oPin = runOldRule(pin, t);
    const Stats nSteady = runCadence(steady, t);
    const Stats nPin = runCadence(pin, t);
    report("pin", oPin, nPin);
    std::printf("      steady spacing: old %d..%d ms, new %d..%d ms\n",
                oSteady.minGapMs, oSteady.maxGapMs, nSteady.minGapMs,
                nSteady.maxGapMs);

    // A threshold rule cannot tell a wobble from a change: every swing of the
    // published value past 1 deg buys a rebuild, so the tour's clock pin (which is
    // what the 2026-11-12 log's sawtooth is) triples the rebuild rate. A clock
    // rebuilds on a schedule; the value only decides how far it moved since the
    // last one, and the budget shortens the schedule at most to the floor.
    ok("the threshold rule buys extra rebuilds from the wobble",
       oPin.creates > oSteady.creates * 1.5);
    ok("the cadence's spacing stays inside [floor, cadence] whatever the value does",
       nPin.minGapMs >= t.cadenceMinMs && nPin.maxGapMs <= t.cadenceMs + 20);
    ok("the old rule's spacing spans two orders of magnitude",
       oPin.maxGapMs > 50 * oPin.minGapMs);
    ok("the cadence's spacing spans far less (bounded by floor and cadence)",
       nPin.maxGapMs <= 15 * nPin.minGapMs);
    ok("the old rule could churn faster than the floor the cadence enforces",
       oPin.minGapMs < nPin.minGapMs);
    ok("and a wobbling target does not push it past the floor",
       nPin.creates / nPin.seconds <= 1000.0 / t.cadenceMinMs + 1.0);
}

static void testDuskRamp() {
    std::printf("\n== twilight: the sun's radiance ramps 6.0 -> 0.02 ==\n");
    const Tuning t = shipped();
    const Sky sky = makeSky(kSkyDusk, 120.0, 1.0f / 60.0f);
    const Stats o = runOldRule(sky, t);
    const Stats n = runCadence(sky, t);
    report("dusk", o, n);
    std::printf("      biggest luminance step in one rebuild: old %.3f, new %.3f\n",
                o.maxRelLum, n.maxRelLum);

    // This is the churn the old rule hid in plain sight: "more than 5% luminance
    // change" during a 3x-per-second ramp is a rebuild every ~3 frames, 20/s for
    // one body, for as long as twilight lasts - and twilight is exactly when the
    // player is watching the sky.
    ok("the old rule's luminance trigger buys ~50 rebuilds in a 5 s ramp",
       o.creates > 40.0 && o.creates < 90.0);
    ok("and fires every frame at the steepest point",
       o.minGapMs <= 20);
    ok("its luminance step is its 10%-of-mean threshold or worse",
       o.maxRelLum > 0.09f && o.maxRelLum < 0.25f);
    ok("the cadence bounds the whole run at its floor",
       n.creates / n.seconds <= 1000.0 / t.cadenceMinMs + 1.0);
    // The invariant the cadence actually claims: a rebuild carries at most the
    // budget, or at most whatever the churn floor forced - rate x floor. The sun's
    // radiance ramp is the worst case in the whole suite, because [fade]
    // sky_abs_per_sec makes small values fall faster in RELATIVE terms: the rate
    // peaks at ~4/s near the 0.02 night floor, so 150 ms of it is 0.8 of itself.
    // Absolute luminance there is 0.08 -> 0.03, i.e. a sun that has already set.
    const float allowed = std::max(t.budgetRel, n.maxRelRate * t.cadenceMinMs / 1000.0f);
    std::printf("      steepest luminance rate %.2f/s -> a 150 ms period allows a "
                "%.2f step; measured %.2f\n",
                n.maxRelRate, allowed, n.maxRelLum);
    ok("every luminance step is within what the churn floor allows",
       n.maxRelLum <= allowed + 1e-3f);
    ok("and the floor is what binds there, not the budget (documented trade)",
       allowed > t.budgetRel);
}

static void testPinnedClock() {
    std::printf("\n== a pinned clock: nothing moves, so nothing is created ==\n");
    Tuning t = shipped();
    Sky sky = makeSky(kSkySteady, 60.0, 1.0f / 60.0f);
    // force_hour=20: the target direction and radiance are constant.
    for (size_t i = 0; i < sky.dir.size(); i += 3) {
        sky.dir[i + 0] = 0.5f;
        sky.dir[i + 1] = 0.5f;
        sky.dir[i + 2] = 0.0f;
    }
    for (size_t i = 0; i < sky.lum.size(); ++i) {
        sky.lum[i] = 6.0f;
    }
    const Stats n = runCadence(sky, t);
    std::printf("      pinned     new: %6.2f creates/s  %.0f periods skipped\n",
                n.creates / n.seconds, n.skips);
    ok("a body that has not moved since its last rebuild costs no bridge call",
       n.creates == 0);
    ok("the periods still pass (the cadence is a clock, not a queue)",
       n.skips >= 60.0 * 1000.0 / t.cadenceMs - 2.0);
}

// ------------------------------------------------------------- the primitives

static void testCadenceMath() {
    std::printf("\n== rhyCadence*: the arithmetic itself ==\n");

    RhyCadence c;
    rhyCadenceInit(c, 0.5f, 0.05f, 150, 2000);
    ok("a still body keeps the fixed period", rhyCadenceIntervalMs(c) == 2000);
    rhyCadenceObserve(c, 0.0f, 0.0f, 1.0f / 60.0f);
    ok("observing nothing does not change the period",
       rhyCadenceIntervalMs(c) == 2000);

    // 0.125 deg/s (the real sky) with a 0.5 deg budget: 4 s allowed, the fixed
    // cadence is shorter, so the cadence wins.
    rhyCadenceInit(c, 0.5f, 0.05f, 150, 2000);
    for (int f = 0; f < 600; ++f) {
        rhyCadenceObserve(c, 0.125f / 60.0f, 0.0f, 1.0f / 60.0f);
    }
    ok("a slow body is not rebuilt faster than the fixed cadence",
       rhyCadenceIntervalMs(c) == 2000);

    // 12 deg/s (the limiter slewing) with the same budget: 41 ms wanted, the
    // churn floor is what applies.
    rhyCadenceInit(c, 0.5f, 0.05f, 150, 2000);
    for (int f = 0; f < 600; ++f) {
        rhyCadenceObserve(c, 12.0f / 60.0f, 0.0f, 1.0f / 60.0f);
    }
    ok("a fast body is rebuilt at the churn floor, not every frame",
       rhyCadenceIntervalMs(c) == 150);

    // A luminance rate binds it too: 0.5 of itself per second against a 0.05
    // budget is 100 ms, above the floor, so it is the binding constraint.
    rhyCadenceInit(c, 0.5f, 0.05f, 150, 2000);
    for (int f = 0; f < 600; ++f) {
        rhyCadenceObserve(c, 0.0f, 0.5f / 60.0f, 1.0f / 60.0f);
    }
    ok("the luminance budget can bind the period",
       rhyCadenceIntervalMs(c) == 150);   // 100 ms wanted, 150 ms floor
    rhyCadenceInit(c, 0.5f, 0.05f, 50, 2000);
    for (int f = 0; f < 600; ++f) {
        rhyCadenceObserve(c, 0.0f, 0.05f / 60.0f, 1.0f / 60.0f);
    }
    ok("with a lower floor the luminance budget sets the period (~1 s)",
       rhyCadenceIntervalMs(c) > 900 && rhyCadenceIntervalMs(c) < 1100);

    // A rate that stops must let the period drift back to the fixed cadence: a
    // clock jump must not pin a body at the churn floor for the rest of the day.
    rhyCadenceInit(c, 0.5f, 0.05f, 150, 2000);
    for (int f = 0; f < 600; ++f) {
        rhyCadenceObserve(c, 12.0f / 60.0f, 0.0f, 1.0f / 60.0f);
    }
    int settled = -1;
    for (int f = 0; f < 600; ++f) {
        rhyCadenceObserve(c, 0.0f, 0.0f, 1.0f / 60.0f);
        if (rhyCadenceIntervalMs(c) == 2000) {
            settled = f;
            break;
        }
    }
    ok("the period returns to the fixed cadence once the motion stops",
       settled >= 0 && settled < 120);

    // due / advance / rebuilt: the due state survives a refused create.
    rhyCadenceInit(c, 0.5f, 0.05f, 150, 200);
    bool due = false;
    int f = 0;
    for (; f < 1000 && !due; ++f) {
        rhyCadenceAdvance(c, 1.0f / 60.0f);
        due = rhyCadenceDue(c);
    }
    ok("the period elapses in wall time, not in frames", f >= 11 && f <= 14);
    ok("still due after nothing consumed it", rhyCadenceDue(c));
    ok("still due a frame later (a refused create must be retried)",
       (rhyCadenceAdvance(c, 1.0f / 60.0f), rhyCadenceDue(c)));
    rhyCadenceRebuilt(c);
    ok("rebuilding clears it", !rhyCadenceDue(c));

    // Garbage in: a floor above the period, a zero budget, a negative period.
    rhyCadenceInit(c, 0.0f, 0.0f, 5000, 1000);
    ok("a floor above the cadence is corrected to the cadence",
       c.minMs == 5000 && c.maxMs == 5000);
    ok("a zero budget is corrected, not honoured as 'rebuild every frame'",
       c.maxStepDeg >= 0.05f && c.maxRelLum >= 0.005f);
    rhyCadenceInit(c, 1000.0f, 100.0f, -5, 10000000);
    ok("an absurd budget and bounds are clamped",
       c.maxStepDeg <= 30.0f && c.maxRelLum <= 1.0f && c.minMs >= 20 &&
       c.maxMs <= 120000);
}

int main() {
    testCadenceMath();
    testSteadyDay();
    testClockJump();
    testPinWobble();
    testDuskRamp();
    testPinnedClock();
    std::printf("\n%d checks, %d failures\n", g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}
