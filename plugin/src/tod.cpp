// s29: the time axis. See tod.h for the three measured defects this file exists to
// fix (quantisation, two alternating calendar sources, a torn clock read) and for the
// rule it enforces: continuous, and never backwards except at the midnight wrap.

#include "tod.h"

#include <cmath>

namespace {

// SA's own tick length. The 2026-11-12 log says p50 2015 ms per game minute over 320
// sample pairs, and the shipped ini default is 2000; both are inside this window.
// The window exists because the calibration is fed by observed gaps: one bad gap
// (a pause, a load, a script pin) must not be able to make a game minute a
// millisecond long or an hour long.
const double kMinMsPerGameMinute = 200.0;
const double kMaxMsPerGameMinute = 60000.0;
const double kDefaultMsPerGameMinute = 2000.0;

double clampd(double v, double lo, double hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

// Milliseconds into the current game minute, 0..1. Capped at 1 on purpose: if the
// clock has not ticked for longer than one game minute (a cutscene, a paused clock,
// a script re-pinning it every frame) the interpolation must stop at the end of the
// minute instead of running away from the game. That cap is what makes "hold the
// anchor when the clock is being pinned" safe: the published time can never get more
// than one game minute ahead of the anchor.
double minuteFraction(const RhyTodClock& c, unsigned long long tickMs) {
    if (tickMs <= c.anchorTick) {
        return 0.0;
    }
    const double ms = static_cast<double>(tickMs - c.anchorTick);
    const double mpm = c.msPerGameMinute > 0.0 ? static_cast<double>(c.msPerGameMinute)
                                              : kDefaultMsPerGameMinute;
    return clampd(ms / mpm, 0.0, 1.0);
}

// Recompute the published values from the anchor. Called after every sample, whether
// or not the sample changed anything: the point of s29 is that time advances between
// the whole-minute ticks.
void publish(RhyTodClock& c, unsigned long long tickMs) {
    double unwrapped = c.anchorMinute + minuteFraction(c, tickMs);
    if (unwrapped < 0.0) {
        unwrapped = 0.0;  // the day arithmetic below assumes a non-negative time
    }
    const int day = static_cast<int>(std::floor(unwrapped / 1440.0));
    if (day > c.day) {
        ++c.wraps;  // midnight: the only decrease minuteOfDay is allowed to make
    }
    c.day = day;
    c.dayBase = static_cast<double>(day) * 1440.0;
    double m = unwrapped - c.dayBase;
    if (m < 0.0) {
        m = 0.0;
    }
    if (m >= 1440.0) {
        m -= 1440.0;
    }
    c.minuteOfDay = m;
    c.absoluteMinute = static_cast<unsigned long long>(static_cast<long long>(day) * 1440LL +
                                                       static_cast<long long>(std::floor(m)));
}

double seedMsPerGameMinute(const RhyTodParams& p) {
    const double v = static_cast<double>(p.msPerGameMinute);
    if (!(v >= kMinMsPerGameMinute)) {  // also catches NaN
        return kDefaultMsPerGameMinute;
    }
    return v > kMaxMsPerGameMinute ? kMaxMsPerGameMinute : v;
}

}  // namespace

void rhyTodInit(RhyTodClock& c, const RhyTodParams& p) {
    c.seeded = false;
    c.minuteOfDay = 0.0;
    c.absoluteMinute = 0;
    c.msPerGameMinute = static_cast<float>(seedMsPerGameMinute(p));
    c.ticks = c.tornReads = c.jitterIgnored = c.reanchors = c.wraps = c.badSamples = 0;
    c.day = 0;
    c.anchorMinute = 0.0;
    c.anchorTick = 0;
    c.dayBase = 0.0;
    c.backSince = 0;
    c.backMinute = 0.0;
    c.lastMinuteField = -1;
}

void rhyTodReseed(RhyTodClock& c, const RhyTodParams& p) {
    // Only the calibration moves. The anchor, the day base and every counter stay
    // where they are, so editing ms_per_game_minute in rhyremix.ini cannot teleport
    // the sun the way re-running rhyTodInit() would.
    c.msPerGameMinute = static_cast<float>(seedMsPerGameMinute(p));
}

void rhyTodUpdate(RhyTodClock& c, int rawHour, int rawMinute, unsigned long long tickMs,
                  const RhyTodParams& p) {
    if (!c.seeded) {
        c.seeded = true;
        c.day = 0;
        c.dayBase = 0.0;
        c.anchorMinute = static_cast<double>(rawHour * 60 + rawMinute);
        c.anchorTick = tickMs;
        c.lastMinuteField = rawMinute;
        publish(c, tickMs);
        return;
    }

    // Out-of-range bytes are not a time. Ignore the sample (the interpolation still
    // advances) rather than clamping it: clamping a garbage hour to 0 would itself be
    // a jump, which is the thing this file is supposed to prevent.
    if (rawHour < 0 || rawHour > 23 || rawMinute < 0 || rawMinute > 59) {
        ++c.badSamples;
        publish(c, tickMs);
        return;
    }

    const double raw = static_cast<double>(rawHour * 60 + rawMinute);

    // Where the clock should be now if it has run at the calibrated rate since the
    // anchor. Every candidate below is judged against this, not against the anchor:
    // that is what makes the interpolation and the jump detection agree.
    const double expected = c.anchorMinute + minuteFraction(c, tickMs);

    // Put the raw pair in the same (unwrapped) day as the reference. More than half a
    // day away means it belongs to the neighbouring day: the midnight wrap forward, or
    // a crossing back over midnight. Either way it has to be unwrapped before it can
    // be compared with anything.
    double cand = raw + c.dayBase;
    if (cand - expected < -720.0) {
        cand += 1440.0;
    } else if (cand - expected > 720.0) {
        cand -= 1440.0;
    }

    // T3, the torn read. SA writes the minute and the hour in two stores, so a sample
    // taken between them is off by an hour in one of exactly two shapes: 6:59 -> 7:00
    // read as (6,0) is 60 minutes behind, read as (7,59) it is 59 minutes ahead. Both
    // shapes require the minute field to have been at 59 in the previous sample, which
    // is what a roll looks like from inside. Without that condition a deliberate
    // setTimeOfDay() that happened to land on the hour would be "repaired" an hour off
    // its target, which is worse than the problem.
    if (rawMinute == 0 && c.lastMinuteField == 59 && cand < expected - 30.0) {
        cand += 60.0;
        ++c.tornReads;
    } else if (rawMinute == 59 && c.lastMinuteField == 59 && cand > expected + 30.0) {
        cand -= 59.0;
        ++c.tornReads;
    }

    if (cand > c.anchorMinute) {
        // The clock moved forward. Measure how long its tick was and fold that into
        // the calibration, then re-anchor. A gap of more than two hours is a pause or
        // a scripted jump: the time follows it, but it says nothing about how long a
        // game minute is, so it must not move the calibration.
        const double ticked = cand - c.anchorMinute;
        if (ticked >= 1.0 && ticked <= 120.0 && tickMs > c.anchorTick) {
            const double obs = static_cast<double>(tickMs - c.anchorTick) / ticked;
            if (obs >= kMinMsPerGameMinute && obs <= kMaxMsPerGameMinute) {
                c.msPerGameMinute += static_cast<float>(0.25 * (obs - c.msPerGameMinute));
            }
        }
        c.anchorMinute = cand;
        c.anchorTick = tickMs;
        c.backSince = 0;
        ++c.ticks;
    } else if (cand < c.anchorMinute) {
        const double tol = p.jitterToleranceMinutes >= 0.0 ? static_cast<double>(p.jitterToleranceMinutes)
                                                         : 0.0;
        const double deficit = expected - cand;
        if (deficit <= tol) {
            // Noise: a script re-pinning the clock (the tour does it twice per stop),
            // a jittery clock update. Holding is monotone and costs at most the
            // tolerance; following it is the A/B/A/B flicker this task is about.
            ++c.jitterIgnored;
            c.backSince = 0;
        } else if (c.backSince == 0) {
            c.backSince = tickMs;  // first sighting of a real backwards set
            c.backMinute = cand;
        } else {
            c.backMinute = cand;
            if (tickMs - c.backSince >= p.reanchorMs) {
                // The game really did move the clock backwards and has stayed there.
                // Follow it - once, deliberately, and logged by the caller - instead of
                // oscillating. The published lighting still ramps to the new time,
                // because sun.cpp rate limits what it publishes (s28).
                c.anchorMinute = c.backMinute;
                c.anchorTick = tickMs;
                c.backSince = 0;
                ++c.reanchors;
            }
        }
    } else {
        c.backSince = 0;  // back on the anchor line: any pending re-anchor is void
    }

    c.lastMinuteField = rawMinute;
    publish(c, tickMs);
}

// ------------------------------------------------------------------ calendar

void rhyCalendarInit(RhyCalendar& c) {
    c.month = c.day = 0;
    c.stable = 0;
    c.doy = 0;
    c.accepted = c.rejected = 0;
}

int rhyCalendarUpdate(RhyCalendar& c, int month, int day, int neededStable) {
    if (month != c.month || day != c.day) {
        c.month = month;
        c.day = day;
        c.stable = 1;
    } else if (c.stable < 1000000) {
        ++c.stable;
    }
    const int need = neededStable < 1 ? 1 : neededStable;
    if (c.stable < need || month < 1 || month > 12 || day < 1 || day > 31) {
        ++c.rejected;
        return 0;
    }
    static const int kCum[] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
    const int doy = kCum[month - 1] + day;
    // A date does not step backwards, and does not move more than a day between two
    // frames. The measured counter (25, 41, 57, ... +16 per sample) passes the
    // "looks like a month" test every sixteen samples; this test and the stability
    // requirement are what stop the season alternating between two values.
    if (c.doy > 0 && (doy < c.doy - 1 || doy > c.doy + 1)) {
        ++c.rejected;
        return 0;
    }
    c.doy = doy;
    ++c.accepted;
    return doy;
}

// ------------------------------------------------------------------- weather

void rhyWeatherInit(RhyWeather& w, float turbidity, float cloud) {
    w.turbidity = turbidity >= 0.0f ? turbidity : 0.0f;
    w.cloud = clampf(cloud, 0.0f, 1.0f);
    w.seeded = true;
}

namespace {
// Move cur toward target by at most maxDelta. Landing exactly on the target matters:
// a follower that stops 0.001 short of it keeps "changing" forever, and the caller
// republishes a value that never settles.
void ramp(float& cur, float target, float maxDelta) {
    if (maxDelta < 0.0f) {
        maxDelta = 0.0f;
    }
    if (cur < target) {
        cur = cur + maxDelta >= target ? target : cur + maxDelta;
    } else if (cur > target) {
        cur = cur - maxDelta <= target ? target : cur - maxDelta;
    }
}
}  // namespace

void rhyWeatherUpdate(RhyWeather& w, float targetTurbidity, float targetCloud, float dtSec,
                      float maxTurbidityPerSec, float maxCloudPerSec) {
    if (!(targetTurbidity >= 0.0f)) {  // also catches NaN
        targetTurbidity = 0.0f;
    }
    targetCloud = clampf(targetCloud, 0.0f, 1.0f);
    if (!w.seeded) {
        rhyWeatherInit(w, targetTurbidity, targetCloud);
        return;
    }
    if (dtSec < 0.0f) {
        dtSec = 0.0f;
    }
    ramp(w.turbidity, targetTurbidity, maxTurbidityPerSec * dtSec);
    ramp(w.cloud, targetCloud, maxCloudPerSec * dtSec);
}

float rhyCloudTransmission(float cloud) {
    // An overcast sky has no sun disc to cast a shadow with: what is left of the
    // direct beam is a bright patch, not a beam. 0.1 is roughly what a thick
    // overcast leaves. The diffuse half of the change belongs to the ambient term,
    // which is what rhyCloudAmbientGain() is for.
    const float c = clampf(cloud, 0.0f, 1.0f);
    const float t = 1.0f - 0.9f * c;
    return t < 0.1f ? 0.1f : t;
}

float rhyCloudAmbientGain(float cloud) {
    // The light an overcast takes off the beam does not disappear, it is scattered
    // into the whole sky, so the ambient rises while the direct light falls. 1.6x at
    // full overcast keeps a grey afternoon from rendering as a black one. With
    // cloud = 0 (the shipped default) this is exactly 1.0 and nothing changes.
    return 1.0f + 0.6f * clampf(cloud, 0.0f, 1.0f);
}
