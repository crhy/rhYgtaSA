// tests/exposure_check.cpp - validates plugin/src/exposure.cpp (n22).
//
// Build:
//   g++ -std=c++17 -Wall -Wextra -Iplugin/src tests/exposure_check.cpp
//       plugin/src/exposure.cpp plugin/src/solar.cpp plugin/src/smooth.cpp
//       -o build/tmp/expc
//   ./build/tmp/expc
//
// n22: n16 shipped `rtx.tonemappingMode = 0` + `rtx.tonemap.exposureBias = -0.5`
// in all three profiles, and in Global mode that anchor IS the exposure of the
// whole frame: exposureFactor = 2^(exposureBias + userEVBias)
// (rtx_tone_mapping.cpp:234). rtx.conf is a file; the game clock is not. Measured
// on the shots the 2026-11-12 report called good (build/tmp/n16/good-before.json):
// 02 grove pools 21:00 p50 17.4 / 34.5 % black, 03 sodium lamp 21:00 p50 7.0 /
// 52.2 % black, against noon p50 123-170. One static number cannot serve both.
//
// So this file tests the SHAPE of the fix, not its effect (the effect needs a
// rendered frame, and rule 1 of the task forbids running the game here):
//   1. the anchor is a function of SUN ELEVATION, so equal elevation gives equal
//      EV whatever the clock said to get there - 05:30 and 19:30, the task's own
//      test of the design;
//   2. at full daylight it equals the profiles' static -0.5, i.e. the change is a
//      no-op at noon and the daytime numbers the next live run takes are n16's;
//   3. the floors cannot be talked out of (rate bound, EV clamp, band direction);
//   4. through the follower, a whole simulated day - and the tour's clock jumps -
//      never move the published value faster than max_ev_per_sec.

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "../plugin/src/exposure.h"
#include "../plugin/src/smooth.h"
#include "../plugin/src/solar.h"

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

// The shipped defaults, in one place, so the test measures what a player gets.
static RhyExposureParams shipped() {
    RhyExposureParams p;
    rhyExposureDefaults(p);
    return p;
}

// The observer the plugin uses with the shipped [sun] keys: Los Angeles, the
// mid-July default season (day_of_year=186), and daylight saving on (186 falls in
// the Mar-Nov window, so the timezone is -7). Same construction as sun.cpp.
static SkyObserver la() {
    SkyObserver ob;
    ob.latitudeDeg = 34.05f;
    ob.longitudeDeg = -118.24f;
    ob.dayOfYear = 186;
    ob.timezoneHours = -8.0f + 1.0f;   // [sun] dst=1 in July
    ob.turbidity = 0.12f;
    return ob;
}

static float elevAt(const SkyObserver& ob, float minuteOfDay) {
    SkyBody b;
    solarPositionF(ob, minuteOfDay, &b);
    return b.elevationDeg;
}

// --------------------------------------------------------------- the curve

static void testCurve() {
    std::printf("\n== the anchor is a function of the sun, not of the clock ==\n");
    const RhyExposureParams p = shipped();

    // (2) the noon no-op: at full daylight the plugin publishes what config/rtx.conf
    // already carries, so n22 cannot move the daytime grade at all.
    const float noonElev = 45.0f;
    const float noon = rhyExposureTargetEv(p, noonElev);
    std::printf("      elevation %+.1f deg -> %+.3f EV (rtx.conf says %+.1f)\n",
                noonElev, noon, p.dayEv);
    ok("at full daylight the target IS the profile's static -0.5 EV anchor",
       std::fabs(noon - (-0.5f)) < 1e-6f);

    // The night end: below the band the anchor is night_ev, i.e. the light that
    // the local tonemapper used to add is put back as an EV number.
    const float night = rhyExposureTargetEv(p, -30.0f);
    std::printf("      elevation %+.1f deg -> %+.3f EV (2^%+.2f = x%.2f the light)\n",
                -30.0f, night, night - p.dayEv, std::pow(2.0f, night - p.dayEv));
    ok("at night the target is night_ev (+0.5), 1.0 EV above the noon anchor",
       std::fabs(night - 0.5f) < 1e-6f);
    ok("so a night frame gets 2x the light a noon frame gets from the anchor",
       std::fabs(std::pow(2.0f, night - noon) - 2.0f) < 1e-4f);

    // Monotone: more sun, less exposure. A curve that wiggles is a frame that
    // breathes twice a day.
    bool monotone = true;
    float prev = rhyExposureTargetEv(p, -40.0f);
    for (float e = -39.0f; e <= 60.0f; e += 1.0f) {
        const float v = rhyExposureTargetEv(p, e);
        if (v > prev + 1e-6f) {
            monotone = false;
        }
        prev = v;
    }
    ok("the curve is monotone in elevation (no wiggle, no double breath)", monotone);

    // (1) the task's own test: the same EV at 05:30 and 19:30. The two clock times
    // are NOT symmetric about solar noon, so their elevations differ - what has to
    // be equal is the EV at EQUAL elevation. Take the evening elevation, find the
    // morning minute that reaches it, and compare the EVs.
    const SkyObserver ob = la();
    const float elevEvening = elevAt(ob, 19.5f * 60.0f);
    float lo = 0.0f, hi = 12.0f * 60.0f;   // the morning side of solar noon
    for (int i = 0; i < 60; ++i) {
        const float mid = 0.5f * (lo + hi);
        if (elevAt(ob, mid) < elevEvening) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    const float minuteMorning = 0.5f * (lo + hi);
    const float elevMorning = elevAt(ob, minuteMorning);
    const float evMorning = rhyExposureTargetEv(p, elevMorning);
    const float evEvening = rhyExposureTargetEv(p, elevEvening);
    std::printf("      19:30 elev %+.2f deg -> %+.4f EV | same elevation at "
                "%02d:%02d (elev %+.2f) -> %+.4f EV\n",
                elevEvening, evEvening, static_cast<int>(minuteMorning / 60.0f),
                static_cast<int>(std::fmod(minuteMorning, 60.0f)), elevMorning, evMorning);
    ok("equal elevation on the way up and on the way down gives the same EV",
       std::fabs(evMorning - evEvening) < 1e-5f);
    // 05:30 and 19:30 are 14 hours apart on the dial but they are NOT the same sky:
    // this model, at this latitude and season, has sunrise at 05:52 and geometric
    // sunset at 20:04 (solar noon 12:57, max elevation 78.7 deg), so 05:30 is 22
    // minutes BEFORE sunrise (-3.8 deg, inside the twilight band) while 19:30 is 34
    // minutes before sunset (+6.2 deg, above it). The honest statement of the design
    // is the one above - equal elevation, equal EV - plus this: the pair that IS
    // symmetric about solar noon gets the same EV, which a clock rule cannot do.
    float noonMin = 0.0f, best = -100.0f;
    for (float m = 0.0f; m <= 1440.0f; m += 1.0f) {
        const float e = elevAt(ob, m);
        if (e > best) {
            best = e;
            noonMin = m;
        }
    }
    // Pick the offset from solar noon that puts the sun IN the twilight band (-3
    // deg), so the comparison is inside the ramp and not between two saturated
    // ends.
    float dLo = 300.0f, dHi = 600.0f;
    for (int i = 0; i < 60; ++i) {
        const float mid = 0.5f * (dLo + dHi);
        if (elevAt(ob, noonMin + mid) > -3.0f) {
            dLo = mid;
        } else {
            dHi = mid;
        }
    }
    const float dMin = 0.5f * (dLo + dHi);
    // The sky this model actually predicts, printed once so the numbers the docs
    // quote are the plugin's own and not an almanac's: solar noon, and the minutes
    // where the elevation crosses zero.
    float riseMin = noonMin, setMin = noonMin;
    for (float m = noonMin; m >= 0.0f; m -= 1.0f) {
        if (elevAt(ob, m) < 0.0f) {
            riseMin = m + 1.0f;
            break;
        }
    }
    for (float m = noonMin; m <= 1440.0f; m += 1.0f) {
        if (elevAt(ob, m) < 0.0f) {
            setMin = m;
            break;
        }
    }
    std::printf("      the model's sky: solar noon %02d:%02d (max elev %+.2f deg), "
                "sunrise %02d:%02d, geometric sunset %02d:%02d\n",
                static_cast<int>(noonMin / 60.0f),
                static_cast<int>(std::fmod(noonMin, 60.0f)), best,
                static_cast<int>(riseMin / 60.0f),
                static_cast<int>(std::fmod(riseMin, 60.0f)),
                static_cast<int>(setMin / 60.0f),
                static_cast<int>(std::fmod(setMin, 60.0f)));
    const float evAm = rhyExposureTargetEv(p, elevAt(ob, noonMin - dMin));
    const float evPm = rhyExposureTargetEv(p, elevAt(ob, noonMin + dMin));
    std::printf("      solar noon %02d:%02d, +-%.0f min: am elev %+.2f -> "
                "%+.4f EV, pm elev %+.2f -> %+.4f EV\n",
                static_cast<int>(noonMin / 60.0f),
                static_cast<int>(std::fmod(noonMin, 60.0f)), dMin,
                elevAt(ob, noonMin - dMin), evAm,
                elevAt(ob, noonMin + dMin), evPm);
    ok("the pair symmetric about solar noon gets the same EV (dawn = dusk)",
       std::fabs(evAm - evPm) <= 0.05f);
    const float ev0530 = rhyExposureTargetEv(p, elevAt(ob, 5.5f * 60.0f));
    const float ev1930 = rhyExposureTargetEv(p, elevAt(ob, 19.5f * 60.0f));
    std::printf("      05:30 elev %+.2f -> %+.3f EV | 19:30 elev %+.2f -> %+.3f EV "
                "(delta %.3f EV: different sky, so different EV - by design)\n",
                elevAt(ob, 5.5f * 60.0f), ev0530, elevAt(ob, 19.5f * 60.0f), ev1930,
                ev0530 - ev1930);
    ok("05:30 (pre-sunrise) sits between the two anchors, 19:30 (past sunrise) at the day one",
       ev0530 > p.dayEv && ev0530 < p.nightEv && std::fabs(ev1930 - p.dayEv) < 1e-3f);
}

// ------------------------------------------------------------------ the floors

static void testFloors() {
    std::printf("\n== the floors cannot be talked out of ==\n");

    // "As fast as possible" (0 or negative) is not a rate: fall back to the shipped
    // 0.05 EV/s. And 10 EV/s is refused: the whole swing in 0.1 s is a pop.
    RhyExposureParams p = shipped();
    p.maxEvPerSec = 0.0f;
    rhyExposureClamp(p);
    ok("max_ev_per_sec = 0 falls back to 0.05 instead of meaning 'instant'",
       std::fabs(p.maxEvPerSec - 0.05f) < 1e-6f);
    p = shipped();
    p.maxEvPerSec = -3.0f;
    rhyExposureClamp(p);
    ok("a negative bound does the same", std::fabs(p.maxEvPerSec - 0.05f) < 1e-6f);
    p = shipped();
    p.maxEvPerSec = 10.0f;
    rhyExposureClamp(p);
    std::printf("      asked for 10 EV/s, allowed %.3f EV/s (full swing >= %.1f s)\n",
                p.maxEvPerSec, 1.0f / p.maxEvPerSec);
    ok("the rate bound may be tuned but never past 0.25 EV/s",
       p.maxEvPerSec <= 0.25f + 1e-6f);
    p = shipped();
    p.maxEvPerSec = 0.001f;
    rhyExposureClamp(p);
    ok("and never below 0.005 EV/s either (a frozen anchor is a worse surprise)",
       p.maxEvPerSec >= 0.005f - 1e-6f);

    // The EV clamp: the Remix UI's own control is DragFloat("Global Exposure", -4, 4)
    // (rtx_tone_mapping.cpp:134). A 10 EV anchor is a black frame or a white one.
    p = shipped();
    p.dayEv = -30.0f;
    p.nightEv = 30.0f;
    rhyExposureClamp(p);
    std::printf("      asked for -30/+30 EV, clamped to %+.1f / %+.1f\n",
                p.dayEv, p.nightEv);
    ok("both anchors are clamped inside the runtime's own span",
       p.dayEv >= -8.0f && p.nightEv <= 8.0f);
    ok("and inside the published clamp too",
       p.dayEv >= p.minEv && p.dayEv <= p.maxEv &&
       p.nightEv >= p.minEv && p.nightEv <= p.maxEv);

    // An inverted clamp is a swap, i.e. nonsense: fall back to -4..+4.
    p = shipped();
    p.minEv = 5.0f;
    p.maxEv = -5.0f;
    rhyExposureClamp(p);
    ok("min_ev > max_ev is repaired, not honoured",
       p.minEv < p.maxEv && p.minEv >= -8.0f && p.maxEv <= 8.0f);

    // The band has to run upward and have width, or the curve is a step in the sun.
    p = shipped();
    p.duskDeg = 10.0f;
    p.dawnDeg = -10.0f;
    rhyExposureClamp(p);
    ok("a reversed twilight band falls back to -10..+4",
       p.duskDeg < p.dawnDeg);
    p = shipped();
    p.duskDeg = 0.0f;
    p.dawnDeg = 0.1f;
    rhyExposureClamp(p);
    ok("a near-zero-width band falls back too (a step is not a ramp)",
       p.dawnDeg - p.duskDeg >= 0.5f);

    // The published value can never leave the clamp, whatever the anchors say.
    p = shipped();
    p.dayEv = 7.0f;
    p.nightEv = -7.0f;
    rhyExposureClamp(p);
    bool inside = true;
    for (float e = -60.0f; e <= 60.0f; e += 0.5f) {
        const float v = rhyExposureTargetEv(p, e);
        if (v < p.minEv - 1e-6f || v > p.maxEv + 1e-6f) {
            inside = false;
        }
    }
    ok("the target never escapes [min_ev, max_ev] at any elevation", inside);
}

// ------------------------------------------------- the follower over a real day

// A whole simulated day at 60 fps, the sun from solar.cpp, the shipped params: the
// published anchor must never move faster than the bound, must stay inside the
// clamp, and must actually follow the sky (a limiter that stopped moving would
// pass the first test and be useless).
static void testDay() {
    std::printf("\n== a whole day through the follower ==\n");
    const RhyExposureParams p = shipped();
    const SkyObserver ob = la();
    const float dt = 1.0f / 60.0f;
    // The clock at its measured rate: one game minute per ~2 s real
    // (rhyremix.ini [clock] ms_per_game_minute=2000, measured p50 2015 ms), so a
    // whole game day is 2880 s of frames. Anything faster is the tour pinning the
    // clock, which is the next test.
    const float gameMinPerFrame = dt / 2.0f;

    RhyExposureAnchor a;
    rhyExposureAnchorInit(a, p, elevAt(ob, 13.0f * 60.0f));  // start at noon
    const float start = rhyExposureAnchorEv(a);

    float maxStep = 0.0f, minSeen = 1e9f, maxSeen = -1e9f;
    int frames = 0;
    for (float minute = 13.0f * 60.0f; minute < 13.0f * 60.0f + 1440.0f;
         minute += gameMinPerFrame) {
        const float e = elevAt(ob, std::fmod(minute, 1440.0f));
        const float before = rhyExposureAnchorEv(a);
        const float v = rhyExposureAnchorStep(a, p, e, dt);
        maxStep = std::max(maxStep, std::fabs(v - before));
        minSeen = std::min(minSeen, v);
        maxSeen = std::max(maxSeen, v);
        ++frames;
    }
    std::printf("      24 game-h at 60 fps (%d frames, %.0f s real): published range "
                "%+.3f .. %+.3f EV, biggest frame %.5f EV (bound %.5f)\n",
                frames, frames * dt, minSeen, maxSeen, maxStep, p.maxEvPerSec * dt);
    std::printf("      the sun alone moves the anchor at <= %.5f EV/s - the bound "
                "only ever bites on a clock that jumps\n",
                (maxSeen - minSeen) / (frames * dt));
    ok("the published anchor never moves faster than max_ev_per_sec allows",
       maxStep <= p.maxEvPerSec * dt + 0.005f + 1e-6f);
    ok("it stays inside the clamp",
       minSeen >= p.minEv - 1e-6f && maxSeen <= p.maxEv + 1e-6f);
    ok("and it really followed the sky (it covered the whole night-to-noon span)",
       maxSeen - minSeen >= (p.nightEv - p.dayEv) - 0.01f);
    ok("a day later it is back where it started (the curve is periodic, not a ratchet)",
       std::fabs(rhyExposureAnchorEv(a) - start) < 0.02f);

    // The tour's actual hazard: SARemix_Tour.lua pins the clock per stop, so the
    // sun jumps by tens of degrees between two frames (13:00 -> 21:00 -> 19:05 ->
    // 05:30 inside a minute). The target steps the whole 1 EV; the published value
    // may not.
    RhyExposureAnchor t;
    rhyExposureAnchorInit(t, p, elevAt(ob, 13.0f * 60.0f));
    maxStep = 0.0f;
    int framesToNight = 0;
    for (int f = 0; f < 60 * 60; ++f) {
        const float before = rhyExposureAnchorEv(t);
        const float v = rhyExposureAnchorStep(t, p, elevAt(ob, 21.0f * 60.0f), dt);
        maxStep = std::max(maxStep, std::fabs(v - before));
        if (std::fabs(v - p.nightEv) <= 0.005f) {
            framesToNight = f;
            break;
        }
    }
    std::printf("      clock pinned 13:00 -> 21:00 in one frame: target steps "
                "1.00 EV, biggest published frame %.5f EV, arrived in %.2f s\n",
                maxStep, framesToNight / 60.0);
    ok("a pinned clock does not step the exposure",
       maxStep <= p.maxEvPerSec * dt + 0.005f + 1e-6f);
    ok("and the night anchor is still reached (>= 19 s later)", framesToNight >= 60 * 19);

    // Re-reading the ini mid-frame cannot smuggle a step in: the params change, the
    // follower keeps its value and walks to the new target at the new bound.
    RhyExposureParams q = shipped();
    q.nightEv = 3.0f;
    const float before = rhyExposureAnchorEv(t);
    rhyExposureAnchorStep(t, q, elevAt(ob, 21.0f * 60.0f), dt);
    ok("an ini edit that moves the anchor by 2.5 EV moves the output by <= one frame",
       std::fabs(rhyExposureAnchorEv(t) - before) <= q.maxEvPerSec * dt + 1e-6f);
}

// ------------------------------------------------------ the table for the docs

// What the plugin would publish at the tour's pinned clock times. This is arithmetic
// on the plugin's own solar model, not a rendered frame: it says which EV each shot
// would get, and how much more (or less) light that is than the static -0.5 the
// profiles carry. The pixel numbers still belong to a live run.
static void printTable() {
    std::printf("\n== predicted anchor at the tour's pinned clock times ==\n");
    const RhyExposureParams p = shipped();
    const SkyObserver ob = la();
    static const struct { const char* stop; int hour; int minute; } kStops[] = {
        {"00-baseline-noon-grove",        13, 0},
        {"12-car-mirror-grove-1300",      13, 0},
        {"08a/08b-sunset-1905",           19, 5},
        {"09-grove-dawn-0530",             5, 30},
        {"01..07,11 night stops",         21, 0},
    };
    std::printf("      %-28s %-6s %8s %8s %8s %8s\n", "stop", "clock", "elev",
                "EV", "vs -0.5", "light");
    for (size_t i = 0; i < sizeof kStops / sizeof kStops[0]; ++i) {
        const float minute = kStops[i].hour * 60.0f + kStops[i].minute;
        const float e = elevAt(ob, minute);
        const float ev = rhyExposureTargetEv(p, e);
        std::printf("      %-28s %02d:%02d  %+8.2f %+8.3f %+8.3f x%6.2f\n",
                    kStops[i].stop, kStops[i].hour, kStops[i].minute, e, ev,
                    ev - (-0.5f), std::pow(2.0f, ev - (-0.5f)));
    }
}

int main() {
    testCurve();
    testFloors();
    testDay();
    printTable();
    std::printf("\n%d checks, %d failures\n", g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}
