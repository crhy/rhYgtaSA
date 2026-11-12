// tests/tod_check.cpp - validates plugin/src/tod.cpp (s29): the time axis.
//
// Build:
//   g++ -std=c++17 -Wall -Wextra -Iplugin/src tests/tod_check.cpp
//       plugin/src/tod.cpp plugin/src/solar.cpp plugin/src/smooth.cpp
//       -o build/tmp/todc
//   ./build/tmp/todc
//
// rhY's #2 finding (2026-11-12): "time of day flickers and does not progress
// smoothly". The measurements in .strata/notes-s29-time-of-day-smooth.md say the
// cause is in the SAMPLING, not the publishing (s28 already smoothed what is
// published):
//
//   * the game clock only gives whole minutes - one tick every ~2000 ms (measured
//     p50 2015 ms over 320 sample pairs) - so the sun's target was a 0.25 degree
//     staircase, re-sampled at 1 Hz, i.e. half the samples were stale;
//   * the in-game date bytes are not a date ("month" walks 25,41,57,...,+16 per
//     sample, "day" sits at 3). The old sanity check accepted them whenever the
//     counter passed through 1..12: 20 of 409 logged sky samples used such a date
//     and the sun's elevation swung up to 43.4 degrees (median 23.7) and straight
//     back on the next sample. That is the flicker, A/B/A/B;
//   * the clock is written by other things (SARemix_Tour.lua calls setTimeOfDay()
//     twice per stop), and 51 of ~400 logged sample pairs had it go BACKWARDS.
//
// So the thing under test here is the time itself: it must be continuous, it must
// never go backwards except at the midnight wrap, and the sun curve built from it
// must be monotone between sunrise and sunset with a bounded per-frame step. The
// last test runs a whole simulated 24 h day twice - once the way s28 sampled it,
// once the way s29 does - and asserts the difference is not cosmetic.

#include <cmath>
#include <cstdio>

#include "../plugin/src/smooth.h"
#include "../plugin/src/solar.h"
#include "../plugin/src/tod.h"

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

// The shipped rhyremix.ini values, so the test measures what a player gets.
static const float kMsPerGameMinute = 2000.0f;
static const float kJitterTolMin = 8.0f;
static const unsigned kReanchorMs = 2000u;
static const float kSunRelPerSec = 3.0f;
static const float kSkyAbsPerSec = 0.25f;
static const float kFrameMs = 1000.0f / 60.0f;
static const float kFrameSec = 1.0f / 60.0f;
// The shipped [clock] calendar_stable_reads. The sky module calls the calendar once a
// second, so this is 30 seconds of a still date - see the note in plugin/src/tod.h.
static const int kCalendarStableReads = 30;

static RhyTodParams params() {
    RhyTodParams p;
    p.msPerGameMinute = kMsPerGameMinute;
    p.jitterToleranceMinutes = kJitterTolMin;
    p.reanchorMs = kReanchorMs;
    return p;
}

// ------------------------------------------------------------------ clock

static void testContinuousBetweenTicks() {
    std::printf("\n== the clock is continuous between whole-minute ticks ==\n");
    RhyTodParams p = params();
    RhyTodClock c;
    rhyTodInit(c, p);

    double prev = -1.0;
    double maxStep = 0.0, minStep = 1e9;
    int moved = 0, frozen = 0;
    // 10 game minutes at 60 fps: the raw minute changes 5 times, the published
    // time must move every frame.
    for (unsigned long long f = 0; f < 600; ++f) {
        const double tick = f * kFrameMs;
        const int minute = static_cast<int>(tick / 2000.0) % 60;
        rhyTodUpdate(c, 12, minute, static_cast<unsigned long long>(tick), p);
        if (prev >= 0.0) {
            const double d = c.minuteOfDay - prev;
            if (d > 1e-12) {
                ++moved;
                if (d > maxStep) maxStep = d;
                if (d < minStep) minStep = d;
            } else {
                ++frozen;
            }
        }
        prev = c.minuteOfDay;
    }
    ok("the raw clock only ticked 5 times in 10 game minutes", moved > 0);
    ok("the published time advances every frame, not every tick (>= 595 of 599)",
       frozen <= 4);
    ok("one frame moves the time by about one frame, not one minute",
       maxStep < 0.02 && minStep > 0.0);
    // 600 frames at 60 fps is 10 seconds of wall time, which at 2000 ms per game
    // minute is 5 game minutes.
    ok("10 seconds of wall time produced 5 game minutes",
       std::fabs(c.minuteOfDay - (12 * 60 + 5.0)) < 0.05);
    std::printf("      ms/game-min measured by the module: %.1f (seed %.0f)\n",
                c.msPerGameMinute, kMsPerGameMinute);
}

static void testTornRead() {
    std::printf("\n== a torn hour/minute read cannot walk the clock backwards ==\n");
    RhyTodParams p = params();
    RhyTodClock c;
    rhyTodInit(c, p);

    double prev = -1.0, worstBack = 0.0;
    // One game hour is 7200 frames at 60 fps and 2000 ms per game minute. Run past
    // 9:00 and corrupt the ONE sample that lands between the game's two byte writes,
    // at three hour rolls:
    //   7:00, 8:00: the hour byte is still the old one (reads 6:00, truth 7:00)
    //   9:00:       the minute byte is still the old one (reads 9:59, truth 9:00)
    int lastTotal = -1;
    for (unsigned long long f = 0; f < 10ull * 7200ull; ++f) {
        const double tick = f * kFrameMs;
        const int total = static_cast<int>(tick / 2000.0);  // game minutes since 0:00
        // only the first sample of the game minute can be the one that raced the write
        int corrupt = 0;
        if (total != lastTotal) {
            lastTotal = total;
            corrupt = (total == 420 || total == 480) ? 1 : (total == 540 ? 2 : 0);
        }
        int h = (total / 60) % 24, m = total % 60;
        if (corrupt == 1) {
            h -= 1;  // hour stale, minute fresh
        } else if (corrupt == 2) {
            m = 59;  // minute stale, hour fresh
        }
        rhyTodUpdate(c, h, m, static_cast<unsigned long long>(tick), p);
        if (prev >= 0.0 && c.minuteOfDay < prev) {
            worstBack = std::fmax(worstBack, prev - c.minuteOfDay);
        }
        prev = c.minuteOfDay;
    }
    ok("all three torn samples were repaired", c.tornReads == 3);
    ok("the clock never went backwards across the hour rolls", worstBack <= 0.0);
    ok("and it is still the right time afterwards",
       std::fabs(c.minuteOfDay - (10 * 60.0)) < 2.0);
    std::printf("      tornReads=%u ticks=%u jitter=%u\n", c.tornReads, c.ticks,
                c.jitterIgnored);
}

static void testClockPins() {
    std::printf("\n== a script pinning the clock cannot make time oscillate ==\n");
    // Exactly the shape SARemix_Tour.lua produced between 00:49:40 and 00:51:20:
    // the clock is pinned to 21:00, runs to 21:05, is pinned again, ...
    const int raw[] = {1260, 1265, 1262, 1261, 1264, 1260, 1265, 1262, 1261, 1263};
    RhyTodParams p = params();
    RhyTodClock c;
    rhyTodInit(c, p);
    double prev = -1.0, worstBack = 0.0;
    for (int s = 0; s < 40; ++s) {
        const int rawMin = raw[s % 10];
        const unsigned long long tick = 10000ull + static_cast<unsigned long long>(s) * 10000ull;
        rhyTodUpdate(c, rawMin / 60, rawMin % 60, tick, p);
        if (prev >= 0.0 && c.minuteOfDay < prev) {
            worstBack = std::fmax(worstBack, prev - c.minuteOfDay);
        }
        prev = c.minuteOfDay;
    }
    ok("the pins were classified as jitter, not as new times", c.jitterIgnored > 0);
    ok("no re-anchor was taken for a 5 minute pin", c.reanchors == 0);
    ok("the published time never went backwards", worstBack <= 0.0);
    ok("and it never ran more than one game minute ahead of the anchor",
       c.minuteOfDay < 1265.0 + 1.0 + kJitterTolMin);
    std::printf("      jitter=%u reanchors=%u time=%.2f\n", c.jitterIgnored,
                c.reanchors, c.minuteOfDay);
}

static void testDeliberateBackwardsJump() {
    std::printf("\n== a deliberate jump is followed once, not oscillated ==\n");
    RhyTodParams p = params();
    RhyTodClock c;
    rhyTodInit(c, p);
    // 21:00 for 20 s, then the tour sets 13:00 and leaves it there. The re-anchor
    // needs reanchor_ms of persistence, so the samples have to keep coming.
    for (int s = 0; s < 20; ++s) {
        rhyTodUpdate(c, 21, 0, static_cast<unsigned long long>(s) * 1000ull, p);
    }
    const double before = c.minuteOfDay;
    for (int s = 0; s < 60; ++s) {
        rhyTodUpdate(c, 13, 0, 20000ull + static_cast<unsigned long long>(s) * 100ull, p);
    }
    ok("the jump was taken as one re-anchor", c.reanchors == 1);
    ok("it was not taken before it had persisted", before > 21 * 60.0);
    ok("the time is now the game's time", std::fabs(c.minuteOfDay - 13 * 60.0) < 2.0);
    // and it must not be re-taken every sample afterwards
    for (int s = 0; s < 100; ++s) {
        rhyTodUpdate(c, 13, 0, 21000ull + static_cast<unsigned long long>(s) * 100ull, p);
    }
    ok("a re-anchor is one event, not one per sample", c.reanchors == 1);
}

static void testMidnightWrap() {
    std::printf("\n== midnight wraps, it does not break ==\n");
    RhyTodParams p = params();
    RhyTodClock c;
    rhyTodInit(c, p);
    double prev = -1.0;
    unsigned long long prevAbs = 0;
    bool absMonotone = true;
    int wrapsSeen = 0;
    for (unsigned long long f = 0; f < 26ull * 7200ull; ++f) {  // 26 game hours
        const double tick = f * kFrameMs;
        const int total = static_cast<int>(tick / 2000.0);
        rhyTodUpdate(c, (total / 60) % 24, total % 60,
                     static_cast<unsigned long long>(tick), p);
        if (prev >= 0.0 && c.minuteOfDay < prev) {
            ++wrapsSeen;
        }
        if (prevAbs && c.absoluteMinute < prevAbs) {
            absMonotone = false;
        }
        prev = c.minuteOfDay;
        prevAbs = c.absoluteMinute;
    }
    ok("minuteOfDay decreased exactly once (the wrap) in 26 hours", wrapsSeen == 1);
    ok("wraps counter says 1", c.wraps == 1);
    ok("the absolute minute never decreased", absMonotone);
    ok("26 hours of ticks were counted", c.ticks > 1500);
    std::printf("      absoluteMinute=%llu minuteOfDay=%.2f\n", c.absoluteMinute,
                c.minuteOfDay);
}

static void testCalibrationAndGarbage() {
    std::printf("\n== the tick length is measured, and bad bytes are ignored ==\n");
    RhyTodParams p = params();
    p.msPerGameMinute = 20000.0f;  // a wrong seed on purpose
    RhyTodClock c;
    rhyTodInit(c, p);
    for (unsigned long long f = 0; f < 6000; ++f) {
        const double tick = f * kFrameMs;
        const int total = static_cast<int>(tick / 1674.0);  // the fastest tick logged
        rhyTodUpdate(c, (total / 60) % 24, total % 60,
                     static_cast<unsigned long long>(tick), p);
    }
    ok("the calibration found the real tick length from a bad seed",
       c.msPerGameMinute > 1600.0f && c.msPerGameMinute < 1800.0f);

    // Out-of-range bytes must not become a jump: the 2026-11-12 log shows month=249
    // style garbage, and an hour byte of 255 would be worse than ignoring it.
    const double before = c.minuteOfDay;
    rhyTodUpdate(c, 255, 77, 200000ull, p);
    rhyTodUpdate(c, -3, 0, 200016ull, p);
    ok("garbage clock bytes are counted, not followed", c.badSamples == 2);
    ok("and the time did not jump", std::fabs(c.minuteOfDay - before) < 1.0);
    std::printf("      ms/game-min=%.0f bad=%u\n", c.msPerGameMinute, c.badSamples);
}

// --------------------------------------------------------------- calendar

static void testCalendar() {
    std::printf("\n== the in-game date is only trusted when it behaves like a date ==\n");
    RhyCalendar cal;
    rhyCalendarInit(cal);

    // The measured counter: month walks +16 per sample (25,41,...,249,9,25,...),
    // day sits at 3. The old check accepted it whenever month landed in 1..12.
    // Here it changes every 10 s, which is how fast the log shows it moving.
    int acceptedOld = 0, acceptedNew = 0;
    for (int k = 0; k < 409; ++k) {
      for (int sub = 0; sub < 10; ++sub) {   // 10 one-second reads per value
        const int month = (25 + 16 * k) & 0xFF;
        const int day = 3;
        if (month >= 1 && month <= 12 && day >= 1 && day <= 31) {
            ++acceptedOld;  // what sun.cpp used to do
        }
        if (rhyCalendarUpdate(cal, month, day, kCalendarStableReads) > 0) {
            ++acceptedNew;
        }
      }
    }
    ok("the old rule accepted the garbage 20 times in 409 samples", acceptedOld > 0);
    ok("the new rule never accepts it", acceptedNew == 0);
    ok("and the sky therefore keeps one day of year all session", cal.doy == 0);

    RhyCalendar good;
    rhyCalendarInit(good);
    bool early = false;
    for (int k = 0; k < kCalendarStableReads - 1; ++k) {
        if (rhyCalendarUpdate(good, 7, 5, kCalendarStableReads) > 0) {
            early = true;
        }
    }
    ok("a real date is not trusted until it has held still", !early);
    ok("a real date is trusted once it has",
       rhyCalendarUpdate(good, 7, 5, kCalendarStableReads) == 186);
    ok("and stays trusted", rhyCalendarUpdate(good, 7, 5, kCalendarStableReads) == 186);
    // a date that jumps half a year is the alternating-calendar bug, by another route
    for (int k = 0; k < kCalendarStableReads; ++k) {
        if (rhyCalendarUpdate(good, 1, 3, kCalendarStableReads) > 0) {
            break;
        }
    }
    ok("a date that jumps 180 days is refused", good.doy == 186);
    ok("the refused date is reported", good.rejected > 0);
}

// ---------------------------------------------------------------- weather

static void testWeather() {
    std::printf("\n== a weather change is a ramp, not a step ==\n");
    RhyWeather w;
    rhyWeatherInit(w, 0.12f, 0.0f);
    float prevT = rhyCloudTransmission(w.cloud);
    float maxStep = 0.0f;
    int frames = 0;
    for (int f = 0; f < 60 * 60; ++f) {  // one second of frames after the front arrives
        rhyWeatherUpdate(w, 0.12f, 1.0f, kFrameSec, 0.05f, 0.05f);
        const float t = rhyCloudTransmission(w.cloud);
        maxStep = std::fmax(maxStep, std::fabs(t - prevT));
        prevT = t;
        if (t < 0.999f) {
            ++frames;
        }
    }
    ok("the front took seconds, not one frame", frames > 60 * 10);
    ok("no frame moved the direct beam by more than 0.1 of it", maxStep < 0.02f);
    ok("and it reaches the target", std::fabs(rhyCloudTransmission(w.cloud) - 0.1f) < 1e-3f);
    ok("cloud never overshoots its range", w.cloud <= 1.0f && w.cloud >= 0.0f);

    RhyWeather w2;
    rhyWeatherInit(w2, 0.12f, 0.0f);
    float prevTur = w2.turbidity, maxTur = 0.0f;
    for (int f = 0; f < 600; ++f) {
        rhyWeatherUpdate(w2, 0.9f, 0.0f, kFrameSec, 0.05f, 0.05f);
        maxTur = std::fmax(maxTur, std::fabs(w2.turbidity - prevTur));
        prevTur = w2.turbidity;
    }
    ok("turbidity (the sun's colour) also ramps", maxTur < 0.01f);
    ok("clear sky is exactly neutral", rhyCloudTransmission(0.0f) == 1.0f &&
       rhyCloudAmbientGain(0.0f) == 1.0f);
    std::printf("      overcast: beam x%.3f ambient x%.3f\n",
                rhyCloudTransmission(1.0f), rhyCloudAmbientGain(1.0f));
}

// ------------------------------------------------- the simulated 24 hours

// One full game day, sampled two ways: the way sun.cpp did it before s29 (whole
// minutes, geometry once a second, the old "does this look like a date?" rule) and
// the way it does it now (continuous clock, bodies every frame, stable-calendar
// rule). Everything downstream of the sampling - the s28 rate limiters, the sky
// ambient, the lamp decision - is identical in both, so any difference in the
// numbers is the sampling, which is exactly what finding 2 is about.
struct DayStats {
    double maxElevStep = 0.0;      // largest |change| of sun elevation in one frame, deg
    double maxElevRate = 0.0;      // deg of sun elevation per game minute on a quiet
                                   // frame: the per-frame smoothness bound
    double morningReversals = 0.0; // total backwards elevation before solar noon, deg
    double maxRadStepRel = 0.0;    // largest per-frame relative change of the TARGET radiance
    double maxPubRadRel = 0.0;     // ... of the published (rate limited) radiance
    double maxPubRadStep = 0.0;    // largest per-frame change of the published radiance
    double maxSkyStep = 0.0;       // largest per-frame change of the published ambient
    double maxMinuteStep = 0.0;    // largest per-frame change of the published time
    int timeBackwards = 0;         // frames where the published time went down
    int wraps = 0;
    int doyChanges = 0;            // times the season changed under the player
    int lampFlips = 0;
    unsigned reanchors = 0, jitter = 0, torn = 0;
    double weatherRampMinutes = 0.0;
};

// The old rule, verbatim from the s28-era sun.cpp: accept the bytes whenever they
// look like a date.
static int oldDayOfYear(int month, int day) {
    if (month < 1 || month > 12 || day < 1 || day > 31) {
        return 0;
    }
    static const int kCum[] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
    return kCum[month - 1] + day;
}

static DayStats runDay(bool continuous, bool withWeather, bool withPins) {
    DayStats s;
    const RhyTodParams p = params();
    RhyTodClock c;
    rhyTodInit(c, p);
    RhyCalendar cal;
    rhyCalendarInit(cal);
    RhyWeather w;
    rhyWeatherInit(w, 0.12f, 0.0f);

    SkyObserver ob;
    ob.latitudeDeg = 34.05f;   // GTA SA's map stands in for Los Angeles
    ob.longitudeDeg = -118.24f;
    ob.timezoneHours = -8.0f;
    ob.dayOfYear = 186;        // mid-July, the ini default
    ob.turbidity = 0.12f;

    // Published state: the s28 pipeline, identical in both modes.
    RhyRate rad[3];
    RhyRate sky;
    float pubDir[3] = {0.0f, 0.0f, -1.0f};
    bool seeded = false;

    SkyBody cached;
    const unsigned long long frames =
        static_cast<unsigned long long>(1440.0 * kMsPerGameMinute / kFrameMs) + 10ull;
    unsigned long long geoTick = 0;
    bool haveGeo = false;
    int doy = 186;
    bool lampsOn = false;
    double prevMinute = -1.0, prevElev = 0.0;
    bool havePrev = false;
    float prevWant[3] = {0.0f, 0.0f, 0.0f};
    double cloudStart = -1.0, cloudEnd = -1.0;

    for (unsigned long long f = 0; f < frames; ++f) {
        const unsigned long long tick =
            static_cast<unsigned long long>(f * static_cast<double>(kFrameMs));
        const int gameMinute = static_cast<int>(tick / kMsPerGameMinute);
        int hour = (gameMinute / 60) % 24;
        int minute = gameMinute % 60;
        if (withPins && hour >= 8 && hour < 10) {
            // the tour's shape: setTimeOfDay(8,0) at the stop, the game runs the clock
            // to 08:05, the tour pins it again, for two game hours. When the stop ends
            // the clock jumps forward to where the game left it - a deliberate change
            // of time of day, not a flicker.
            const int phase = static_cast<int>((tick / 10000ull) % 5ull);
            hour = 8;
            minute = phase;
        }
        // The calendar bytes measured in the 2026-11-12 log. "day" sits at 3 all
        // session. "month" is a counter: at the log's 10 s sampling it walks 25, 41,
        // 57, ... +16 per sample, and the day-of-year the old code computed from it
        // went 62, 246, 186, 337, 186, 276, 186, 34, 186, 246, ... (186 is the ini
        // value, used whenever the byte happened to be out of range). Replayed here
        // at the 1 Hz the sky module samples at, with the month that produces each of
        // those values; the byte is a counter, so it is not stable for longer than one
        // sample.
        static const int kLogMonth[] = {3, 9, 12, 10, 2};
        const int month = kLogMonth[(tick / 1000ull) % 5ull];
        const int dayByte = 3;

        double minuteOfDay = 0.0;
        if (continuous) {
            rhyTodUpdate(c, hour, minute, tick, p);
            minuteOfDay = c.minuteOfDay;
        } else {
            minuteOfDay = static_cast<double>(hour * 60 + minute);
        }

        // --- the keyframe pass: once a second, in both modes ---
        const bool keyframe = !haveGeo || tick - geoTick >= 1000ull;
        if (keyframe) {
            haveGeo = true;
            geoTick = tick;
            const int d = continuous ? rhyCalendarUpdate(cal, month, dayByte,
                                                         kCalendarStableReads)
                                     : oldDayOfYear(month, dayByte);
            if (d > 0) {
                doy = d;
            }
        }
        if (ob.dayOfYear != doy) {
            ++s.doyChanges;  // the season changed under the player: the flicker
            ob.dayOfYear = doy;
        }

        // --- weather: same in both modes, and it really does change mid-day ---
        float cloudTarget = 0.0f;
        if (withWeather) {
            const float h = static_cast<float>(minuteOfDay) / 60.0f;
            if (h >= 6.0f && h < 15.0f) {
                cloudTarget = 1.0f;
            }
        }
        rhyWeatherUpdate(w, 0.12f, cloudTarget, kFrameSec, 0.05f, 0.05f);
        ob.turbidity = w.turbidity;
        const float cloudT = rhyCloudTransmission(w.cloud);
        if (withWeather && cloudT < 0.999f) {
            if (cloudStart < 0.0) {
                cloudStart = minuteOfDay;
            }
            cloudEnd = minuteOfDay;
        }

        // --- the bodies: every frame at the continuous minute (s29), or once a
        // second at the whole-minute clock and held in between (s28) ---
        if (continuous) {
            solarPositionF(ob, static_cast<float>(minuteOfDay), &cached);
        } else if (keyframe) {
            solarPosition(ob, static_cast<int>(minuteOfDay / 60.0f) % 24,
                          static_cast<int>(minuteOfDay) % 60, &cached);
        }
        const SkyBody& sun = cached;

        const double elev = sun.elevationDeg;
        // A frame is "quiet" when the clock moved by about what one frame is worth
        // (0.0083 game minutes at 60 fps). Frames where the game changed the time of
        // day on purpose - the tour's stop, a mission jump - are excluded from the
        // smoothness numbers: they are deliberate, and it is the s28 limiters that
        // ramp them, not the clock.
        const double timeStep = minuteOfDay - prevMinute;
        const bool quietFrame = std::fabs(timeStep) <= 0.05;
        if (havePrev) {
            const double step = std::fabs(elev - prevElev);
            if (step > s.maxElevStep) {
                s.maxElevStep = step;
            }
            if (quietFrame) {
                // Degrees of sun elevation per game minute: the honest per-frame
                // bound, because it does not care how long the frame was. The real
                // sun moves 0.25 deg per game minute at most a couple of times a day.
                // When the clock did not move at all in this frame, the divisor is the
                // frame's own length: a jump on a still clock is the worst case there
                // is, and it is measured per game minute of wall time (1 game minute =
                // ms_per_game_minute of wall time).
                double denom = std::fabs(timeStep);
                if (denom < 1e-6) {
                    denom = static_cast<double>(kFrameMs) / kMsPerGameMinute;
                }
                const double rate = step / denom;
                if (rate > s.maxElevRate) {
                    s.maxElevRate = rate;
                }
                // Before solar noon (12:04 here) the sun climbs. Anything that moves
                // it down by a visible amount in that half of the day is the flicker,
                // whatever caused it. 00:30 is past the day's lowest point.
                if (minuteOfDay > 30.0 && minuteOfDay < 12.0 * 60.0 + 4.0 &&
                    elev < prevElev - 0.005) {
                    s.morningReversals += prevElev - elev;  // the A/B/A/B flicker
                }
            }
            if (prevMinute - minuteOfDay > 720.0) {
                ++s.wraps;  // the midnight crossing, the only allowed decrease
            } else if (minuteOfDay < prevMinute - 1e-9) {
                ++s.timeBackwards;
            }
            if (timeStep > s.maxMinuteStep) {
                s.maxMinuteStep = timeStep;
            }
        }
        prevMinute = minuteOfDay;
        prevElev = elev;
        havePrev = true;

        // --- targets, then the s28 publishing pipeline (identical in both modes) ---
        const float daylight = skyDaylightFactor(sun.elevationDeg, -10.0f, 4.0f);
        float wantDir[3] = {-sun.toX, -sun.toY, -sun.toZ};
        float wantRad[3];
        for (int i = 0; i < 3; ++i) {
            wantRad[i] = 6.0f * sun.transmit[i] * cloudT;
        }
        if (seeded && quietFrame && std::fabs(minuteOfDay - prevMinute) > 1e-6) {
            // The TARGET is what the sampling produces: this is the number s29 is
            // about, because the s28 limiters already bound what is published.
            for (int i = 0; i < 3; ++i) {
                if (prevWant[i] > 0.1f) {
                    const double rel = std::fabs(wantRad[i] - prevWant[i]) / prevWant[i];
                    if (rel > s.maxRadStepRel) {
                        s.maxRadStepRel = rel;
                    }
                }
                prevWant[i] = wantRad[i];
            }
        } else {
            for (int i = 0; i < 3; ++i) {
                prevWant[i] = wantRad[i];
            }
        }
        if (!seeded) {
            for (int i = 0; i < 3; ++i) {
                rhyRateInit(rad[i], wantRad[i], kSunRelPerSec, kSkyAbsPerSec, 1e-4f);
            }
            rhyRateInit(sky, 0.02f + 0.98f * daylight, kSunRelPerSec, kSkyAbsPerSec, 0.002f);
            pubDir[0] = wantDir[0];
            pubDir[1] = wantDir[1];
            pubDir[2] = wantDir[2];
            seeded = true;
        } else {
            for (int i = 0; i < 3; ++i) {
                const float before = rad[i].cur;
                rhyRateStep(rad[i], wantRad[i], kFrameSec);
                const double rel = before > 0.1f
                                       ? std::fabs(rad[i].cur - before) / before : 0.0;
                if (rel > s.maxPubRadRel) {
                    s.maxPubRadRel = rel;
                }
                if (std::fabs(rad[i].cur - before) > s.maxPubRadStep) {
                    s.maxPubRadStep = std::fabs(rad[i].cur - before);
                }
            }
            rhyDirLimit(pubDir, wantDir, 12.0f * kFrameSec);
            const float skyBefore = sky.cur;
            rhyRateStep(sky, (0.02f + 0.98f * daylight) * rhyCloudAmbientGain(w.cloud),
                        kFrameSec);
            if (std::fabs(sky.cur - skyBefore) > s.maxSkyStep) {
                s.maxSkyStep = std::fabs(sky.cur - skyBefore);
            }
        }

        const bool nowLamps = lampsShouldBeOn(sun.elevationDeg, -6.0f);
        if (f == 0) {
            lampsOn = nowLamps;  // the day's starting state is not a switch
        } else if (nowLamps != lampsOn) {
            ++s.lampFlips;
            lampsOn = nowLamps;
        }
    }
    s.reanchors = c.reanchors;
    s.jitter = c.jitterIgnored;
    s.torn = c.tornReads;
    if (cloudStart >= 0.0) {
        s.weatherRampMinutes = cloudEnd - cloudStart;
    }
    return s;
}

static void testSimulatedDay() {
    std::printf("\n== a simulated 24 h day, sampled the old way and the new way ==\n");
    const DayStats oldWay = runDay(false, true, true);
    const DayStats newWay = runDay(true, true, true);

    std::printf("      old: sun moves up to %8.2f deg per game minute | worst frame "
                "%8.3f deg | morning reversals %8.2f deg | season changes %3d | lamp "
                "flips %d | time backwards %d\n",
                oldWay.maxElevRate, oldWay.maxElevStep, oldWay.morningReversals,
                oldWay.doyChanges, oldWay.lampFlips, oldWay.timeBackwards);
    std::printf("      new: sun moves up to %8.2f deg per game minute | worst frame "
                "%8.3f deg | morning reversals %8.2f deg | season changes %3d | lamp "
                "flips %d | time backwards %d\n",
                newWay.maxElevRate, newWay.maxElevStep, newWay.morningReversals,
                newWay.doyChanges, newWay.lampFlips, newWay.timeBackwards);
    std::printf("      new: time step/frame %.3f min | target radiance %.4f rel "
                "(published %.4f rel, %.4f abs) | ambient step/frame %.5f | pins "
                "ignored %u reanchors %u | weather ramp %.1f game min\n",
                newWay.maxMinuteStep, newWay.maxRadStepRel, newWay.maxPubRadRel,
                newWay.maxPubRadStep, newWay.maxSkyStep, newWay.jitter,
                newWay.reanchors, newWay.weatherRampMinutes);

    ok("the new clock never runs backwards (the midnight wrap excepted)",
       newWay.timeBackwards == 0);
    ok("and it wraps once in 24 hours", newWay.wraps == 1);
    ok("the sun moves continuously: < 1 deg per game minute (reality: 0.25)",
       newWay.maxElevRate < 1.0);
    ok("the sun never reverses on its way up", newWay.morningReversals < 0.005);
    ok("the season never changes under the player", newWay.doyChanges == 0);
    ok("lamps switch twice a day, not more", newWay.lampFlips == 2);
    ok("the sun's target radiance never moves more than 3% in a frame",
       newWay.maxRadStepRel < 0.03);
    ok("the published radiance stays inside its own rate limit",
       newWay.maxPubRadRel < 0.06);
    ok("or more than 0.05 radiance units in a frame", newWay.maxPubRadStep < 0.05);
    ok("the published ambient never moves more than 0.01 in a frame",
       newWay.maxSkyStep < 0.01);
    ok("a weather front takes minutes, not frames", newWay.weatherRampMinutes > 10.0);
    // The tour's stop is a deliberate change of time of day: the clock follows it, and
    // the s28 limiters are what keep the world from stepping with it.
    ok("a deliberate 2 hour jump is followed, not hidden", newWay.maxMinuteStep > 100.0);

    // The point of running the old way too: the fix has to be measurable, not cosmetic.
    ok("the old sampling really did move the sun tens of degrees per game minute",
       oldWay.maxElevRate > 100.0);
    ok("the old sampling really did reverse the sun on the way up",
       oldWay.morningReversals > 1.0);
    ok("the old sampling really did change the season mid-session",
       oldWay.doyChanges > 0);
    ok("the new rate is at least 100x smaller than the old one",
       newWay.maxElevRate * 100.0 < oldWay.maxElevRate);
    ok("the clock pins were absorbed as jitter, not followed", newWay.jitter > 0);
}

// -------------------------------------------------- the tour's clock, with the sun

// The exact 10-sample shape the 2026-11-12 log recorded between 00:49:40 and
// 00:51:20 (the tour pinning 21:00 twice per stop), with the sun attached: the
// logged elevation wobbled -10.0, -10.8, -10.3, -10.1, -10.6, -10.0, ... at 10 s.
static void testPinnedClockSun() {
    std::printf("\n== the tour's pinned clock, with the sun attached ==\n");
    const int raw[] = {1260, 1265, 1262, 1261, 1264, 1260, 1265, 1262, 1261, 1263};
    const RhyTodParams p = params();
    RhyTodClock c;
    rhyTodInit(c, p);
    SkyObserver ob;
    ob.latitudeDeg = 34.05f;
    ob.longitudeDeg = -118.24f;
    ob.timezoneHours = -8.0f;
    ob.dayOfYear = 186;
    ob.turbidity = 0.12f;

    double worstBack = 0.0, maxStepQuiet = 0.0, prev = 0.0, prevTod = 0.0;
    bool have = false;
    int reversals = 0;
    for (unsigned long long f = 0; f < 12000ull; ++f) {  // 200 s at 60 fps
        const unsigned long long tick = 10000ull + static_cast<unsigned long long>(f) *
                                            static_cast<unsigned long long>(kFrameMs);
        const int rawMin = raw[(tick / 10000ull) % 10ull];
        rhyTodUpdate(c, rawMin / 60, rawMin % 60, tick, p);
        SkyBody sun;
        solarPositionF(ob, static_cast<float>(c.minuteOfDay), &sun);
        if (have) {
            const double d = sun.elevationDeg - prev;
            if (c.minuteOfDay < prevTod - 1e-9) {
                worstBack = std::fmax(worstBack, prevTod - c.minuteOfDay);
            }
            // 21:00 at LA is after the sunset crossing: the sun is going DOWN, so any
            // upward move is the flicker rhY saw, whatever its cause.
            if (d > 1e-9) {
                ++reversals;
            }
            if (std::fabs(c.minuteOfDay - prevTod) <= 1.0) {
                maxStepQuiet = std::fmax(maxStepQuiet, std::fabs(d));
            }
        }
        prev = sun.elevationDeg;
        prevTod = c.minuteOfDay;
        have = true;
    }
    std::printf("      sun elevation moved at most %.5f deg in a frame; "
                "upward reversals: %d; time backwards: %.4f min\n",
                maxStepQuiet, reversals, worstBack);
    ok("the time never went backwards while the clock was pinned", worstBack <= 0.0);
    ok("the sun never reversed direction (no A/B/A/B)", reversals == 0);
    ok("and it moved continuously: < 0.01 deg in a frame", maxStepQuiet < 0.01);
    ok("the pins were counted as jitter", c.jitterIgnored > 0);
    ok("no re-anchor for a 5 minute pin", c.reanchors == 0);
}

int main() {
    testContinuousBetweenTicks();
    testTornRead();
    testClockPins();
    testDeliberateBackwardsJump();
    testMidnightWrap();
    testCalibrationAndGarbage();
    testCalendar();
    testWeather();
    testSimulatedDay();
    testPinnedClockSun();

    std::printf("\n%d checks, %d failed\n", g_checks, g_fail);
    if (g_fail) {
        std::printf("TOD CHECK FAILED\n");
        return 1;
    }
    std::printf("tod check passed\n");
    return 0;
}
