#include "exposure.h"

#include <cmath>

#include "solar.h"   // skyDaylightFactor: the elevation -> daylight smoothstep

// n22. See exposure.h for why this exists and what is deliberately not in it.
// Pure arithmetic on purpose: the same code runs in the plugin and in
// tests/exposure_check.cpp on the host, where a whole simulated day can be run
// against the bounds.

// The runtime's own span for this control: rtx_tone_mapping.cpp:134 is
//   DragFloat("Global Exposure", &rtx.tonemap.exposureBias, -4, 4)
// so -4..+4 is what NVIDIA considers the usable range and -8..+8 the range a
// typo may not escape.
static const float kUiMinEv = -4.0f;
static const float kUiMaxEv = 4.0f;
static const float kHardMinEv = -8.0f;
static const float kHardMaxEv = 8.0f;

// The slew bound: shipped, refused-below, refused-above. 0.05 EV/s means the
// whole day/night swing takes >= 20 s; 0.25 EV/s is the most anyone may ask for,
// because at that rate a 1 EV swing crosses in 4 s and the frame visibly breathes.
static const float kDefaultEvPerSec = 0.05f;
static const float kMinEvPerSec = 0.005f;
static const float kMaxEvPerSec = 0.25f;

void rhyExposureDefaults(RhyExposureParams& p) {
    p.dayEv = -0.5f;        // == config/rtx.conf's static anchor: noon is a no-op
    p.nightEv = 0.5f;       // 1.0 EV of swing, inside the UI's +/-4
    p.duskDeg = -10.0f;     // the same twilight band [sky] uses
    p.dawnDeg = 4.0f;
    p.maxEvPerSec = kDefaultEvPerSec;
    p.minEv = kUiMinEv;
    p.maxEv = kUiMaxEv;
}

void rhyExposureClamp(RhyExposureParams& p) {
    if (!(p.maxEvPerSec >= kMinEvPerSec)) {
        p.maxEvPerSec = kDefaultEvPerSec;   // 0 or negative = "as fast as possible"
    }
    if (p.maxEvPerSec > kMaxEvPerSec) {
        p.maxEvPerSec = kMaxEvPerSec;
    }
    if (p.minEv < kHardMinEv) {
        p.minEv = kHardMinEv;
    }
    if (p.maxEv > kHardMaxEv) {
        p.maxEv = kHardMaxEv;
    }
    if (!(p.minEv < p.maxEv)) {
        p.minEv = kUiMinEv;   // an inverted clamp is a typo, not a preference
        p.maxEv = kUiMaxEv;
    }
    if (p.dayEv < p.minEv) {
        p.dayEv = p.minEv;
    }
    if (p.dayEv > p.maxEv) {
        p.dayEv = p.maxEv;
    }
    if (p.nightEv < p.minEv) {
        p.nightEv = p.minEv;
    }
    if (p.nightEv > p.maxEv) {
        p.nightEv = p.maxEv;
    }
    // The band has to run upwards, and it has to have some width: a zero-width
    // band is a step in the sun, which is the thing this module is for.
    if (!(p.duskDeg < p.dawnDeg - 0.5f)) {
        p.duskDeg = -10.0f;
        p.dawnDeg = 4.0f;
    }
    if (p.duskDeg < -40.0f) {
        p.duskDeg = -40.0f;
    }
    if (p.dawnDeg > 40.0f) {
        p.dawnDeg = 40.0f;
    }
}

float rhyExposureTargetEv(const RhyExposureParams& pIn, float sunElevationDeg) {
    // Re-clamp rather than trust the caller: the params come from an ini that is
    // re-read live, and the bound is only a bound if nothing can bypass it.
    RhyExposureParams p = pIn;
    rhyExposureClamp(p);
    const float day = skyDaylightFactor(sunElevationDeg, p.duskDeg, p.dawnDeg);
    float ev = p.nightEv + (p.dayEv - p.nightEv) * day;
    if (ev < p.minEv) {
        ev = p.minEv;
    }
    if (ev > p.maxEv) {
        ev = p.maxEv;
    }
    return ev;
}

void rhyExposureAnchorInit(RhyExposureAnchor& a, const RhyExposureParams& p,
                           float sunElevationDeg) {
    RhyExposureParams c = p;
    rhyExposureClamp(c);
    rhySignedRateInit(a.follow, rhyExposureTargetEv(c, sunElevationDeg),
                      c.maxEvPerSec, 0.005f);
    a.seeded = true;
    a.maxStepSeen = 0.0f;
    a.targetSeen = a.follow.cur;
}

float rhyExposureAnchorStep(RhyExposureAnchor& a, const RhyExposureParams& p,
                            float sunElevationDeg, float dt) {
    if (!a.seeded) {
        rhyExposureAnchorInit(a, p, sunElevationDeg);
        return a.follow.cur;
    }
    RhyExposureParams c = p;
    rhyExposureClamp(c);
    // The bound itself may have been re-read from the ini; the follower follows
    // the new one from now on, it never retroactively jumps.
    a.follow.maxPerSec = c.maxEvPerSec;
    const float target = rhyExposureTargetEv(c, sunElevationDeg);
    a.targetSeen = target;
    const float before = a.follow.cur;
    rhySignedRateStep(a.follow, target, dt);
    const float step = std::fabs(a.follow.cur - before);
    if (step > a.maxStepSeen) {
        a.maxStepSeen = step;
    }
    return a.follow.cur;
}

float rhyExposureAnchorEv(const RhyExposureAnchor& a) { return a.follow.cur; }
