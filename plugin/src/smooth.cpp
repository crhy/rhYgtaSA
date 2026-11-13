#include "smooth.h"

#include <algorithm>
#include <cmath>
#include <vector>

// s28. See smooth.h for why this exists. Pure arithmetic on purpose: the same
// code runs in the plugin and in tests/smooth_check.cpp on the host.

// ---------------------------------------------------------------- rate limit

void rhyRateInit(RhyRate& r, float value, float relPerSec, float absPerSec,
                 float eps) {
    r.cur = value > 0.0f ? value : 0.0f;
    r.relPerSec = relPerSec >= 1.0f ? relPerSec : 1.0f;
    r.absPerSec = absPerSec >= 0.0f ? absPerSec : 0.0f;
    r.eps = eps >= 0.0f ? eps : 0.0f;
}

bool rhyRateStep(RhyRate& r, float target, float dt) {
    if (!(dt > 0.0f)) {
        dt = 0.0f;
    }
    if (target < 0.0f) {
        target = 0.0f;
    }
    if (r.cur < 0.0f) {
        r.cur = 0.0f;
    }
    if (r.relPerSec < 1.0f) {
        r.relPerSec = 1.0f;
    }
    if (r.absPerSec < 0.0f) {
        r.absPerSec = 0.0f;
    }
    // Already inside the deadband: publishing the target is not a step worth
    // talking about, and snapping stops a follower crawling toward its target
    // forever.
    if (r.eps > 0.0f && std::fabs(target - r.cur) <= r.eps) {
        if (r.cur == target) {
            return false;
        }
        r.cur = target;
        return true;
    }
    const float k = dt > 0.0f ? std::pow(r.relPerSec, dt) : 1.0f;
    float lo = r.cur / k - r.absPerSec * dt;
    float hi = r.cur * k + r.absPerSec * dt;
    if (lo < 0.0f) {
        lo = 0.0f;
    }
    float next = target;
    if (next > hi) {
        next = hi;
    }
    if (next < lo) {
        next = lo;
    }
    if (next == r.cur) {
        return false;
    }
    r.cur = next;
    return true;
}

// ---------------------------------------------------- signed rate limit (n22)

void rhySignedRateInit(RhySignedRate& r, float value, float maxPerSec, float eps) {
    r.cur = value;
    r.maxPerSec = maxPerSec > 0.0f ? maxPerSec : 0.0f;
    r.eps = eps >= 0.0f ? eps : 0.0f;
}

bool rhySignedRateStep(RhySignedRate& r, float target, float dt) {
    if (!(dt > 0.0f)) {
        dt = 0.0f;
    }
    if (!(r.maxPerSec > 0.0f)) {
        return false;  // a bound of 0 means "frozen", and frozen means frozen
    }
    if (r.eps > 0.0f && std::fabs(target - r.cur) <= r.eps) {
        if (r.cur == target) {
            return false;
        }
        r.cur = target;
        return true;
    }
    const float room = r.maxPerSec * dt;
    float next = target;
    if (next > r.cur + room) {
        next = r.cur + room;
    }
    if (next < r.cur - room) {
        next = r.cur - room;
    }
    if (next == r.cur) {
        return false;
    }
    r.cur = next;
    return true;
}

// ------------------------------------------------------------------ fades

void rhyFadeClampRampSeconds(float& up, float& down) {
    // The task's floor: a lamp must take at least half a second to come up or
    // go down. Anything shorter in the ini is a typo, and the instant switch is
    // exactly the bug being fixed here.
    const float kMin = 0.5f;
    if (!(up >= kMin)) {
        up = kMin;
    }
    if (up > 30.0f) {
        up = 30.0f;
    }
    if (!(down >= kMin)) {
        down = kMin;
    }
    if (down > 30.0f) {
        down = 30.0f;
    }
}

void rhyFadeInit(RhyFade& f, float secondsUp, float secondsDown) {
    rhyFadeClampRampSeconds(secondsUp, secondsDown);
    f.gain = 0.0f;
    f.target = 0.0f;
    f.published = -1.0f;  // no handle yet
    f.secondsUp = secondsUp;
    f.secondsDown = secondsDown;
}

void rhyFadeSetRamp(RhyFade& f, float up, float down, float minSeconds) {
    if (!(minSeconds >= 0.05f)) {
        minSeconds = 0.05f;
    }
    if (!(up >= minSeconds)) {
        up = minSeconds;
    }
    if (!(down >= minSeconds)) {
        down = minSeconds;
    }
    if (up > 30.0f) {
        up = 30.0f;
    }
    if (down > 30.0f) {
        down = 30.0f;
    }
    f.secondsUp = up;
    f.secondsDown = down;
}

void rhyFadeAdvance(RhyFade& f, float dt) {
    if (!(dt > 0.0f)) {
        return;
    }
    if (f.target < 0.0f) {
        f.target = 0.0f;
    }
    if (f.target > 1.0f) {
        f.target = 1.0f;
    }
    // The floors here are arithmetic safety only; the policy floors (0.5 s for a
    // light appearing or disappearing) are applied where the envelope is created
    // or re-timed, by rhyFadeInit / rhyFadeClampRampSeconds / rhyFadeSetRamp.
    float up = f.secondsUp >= 0.05f ? f.secondsUp : 0.05f;
    float down = f.secondsDown >= 0.05f ? f.secondsDown : 0.05f;
    if (f.gain < f.target) {
        f.gain += dt / up;
        if (f.gain > f.target) {
            f.gain = f.target;
        }
    } else if (f.gain > f.target) {
        f.gain -= dt / down;
        if (f.gain < f.target) {
            f.gain = f.target;
        }
    }
    if (f.gain < 0.0f) {
        f.gain = 0.0f;
    }
    if (f.gain > 1.0f) {
        f.gain = 1.0f;
    }
}

// True when the envelope wants a handle change. Shared by rhyFadeDue and
// rhyFadeApply: if the two disagree, a light can end up settled at a gain its
// handle does not carry - the first version of this got a lamp stuck at 0.774 of
// its target because the last 0.226 of the ramp was smaller than one step and so
// was never published (found by tests/smooth_check.cpp).
static bool rhyFadeNeeds(const RhyFade& f, float step, float eps) {
    if (f.published < 0.0f) {
        return f.gain > 0.0f;  // needs its first handle
    }
    if (f.gain <= eps && f.target <= 0.0f && f.published <= eps) {
        return true;  // faded to nothing: retire it
    }
    if (std::fabs(f.gain - f.published) >= step) {
        return true;  // a full step is due
    }
    // The follower has stopped moving but the handle is behind it: one final
    // create lands the last fraction. It is the only create allowed to be
    // smaller than a step, and it cannot make a visible jump because the gap is
    // smaller than a step by definition.
    return std::fabs(f.gain - f.published) > 1e-4f &&
           std::fabs(f.gain - f.target) <= 1e-4f;
}

bool rhyFadeDue(const RhyFade& f, float step, float eps) {
    if (step < 0.01f) {
        step = 0.01f;
    }
    if (eps < 0.0f) {
        eps = 0.0f;
    }
    return rhyFadeNeeds(f, step, eps);
}

RhyFadeOp rhyFadeApply(RhyFade& f, float step, float eps) {
    if (step < 0.01f) {
        step = 0.01f;
    }
    if (eps < 0.0f) {
        eps = 0.0f;
    }
    if (f.published < 0.0f) {
        // First handle. Start it at the bottom of the ramp rather than at the
        // current gain: a light that has been waiting for a create slot must not
        // arrive at full radiance.
        f.published = f.gain < step ? f.gain : step;
        if (f.published <= 0.0f) {
            f.published = step;
        }
        return kRhyFadeCreate;
    }
    float delta = f.gain - f.published;
    if (delta > step) {
        delta = step;
    }
    if (delta < -step) {
        delta = -step;
    }
    if (std::fabs(f.gain - f.published) < step &&
        std::fabs(f.gain - f.target) <= 1e-4f) {
        delta = f.gain - f.published;  // the settle step, see rhyFadeNeeds
    }
    f.published += delta;
    if (f.published < 0.0f) {
        f.published = 0.0f;
    }
    if (f.published > 1.0f) {
        f.published = 1.0f;
    }
    if (f.gain <= eps && f.target <= 0.0f && f.published <= eps) {
        // Dark. Retire instead of creating a light with zero radiance: the ramp
        // is finished, so removing the handle is not a step. The test is `<= eps`
        // rather than `== 0` because the follower lands on a tiny positive value
        // (0.0000004) on its way to zero, and a handle carrying that is already
        // invisible.
        f.published = -1.0f;
        return kRhyFadeRetire;
    }
    return kRhyFadeRecreate;
}

// ------------------------------------------------------------- distance gain

float rhyDistanceGain(float dist, float fullRadius, float zeroRadius) {
    if (dist < 0.0f) {
        dist = 0.0f;
    }
    if (!(zeroRadius > fullRadius)) {
        return 1.0f;  // no fade band configured: full gain up to the cut-off
    }
    if (dist <= fullRadius) {
        return 1.0f;
    }
    if (dist >= zeroRadius) {
        return 0.0f;
    }
    return (zeroRadius - dist) / (zeroRadius - fullRadius);
}

// ------------------------------------------------------------- direction

float rhyAngleDeg(const float* a, const float* b) {
    const float cx = a[1] * b[2] - a[2] * b[1];
    const float cy = a[2] * b[0] - a[0] * b[2];
    const float cz = a[0] * b[1] - a[1] * b[0];
    const float cross = std::sqrt(cx * cx + cy * cy + cz * cz);
    const float dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    return std::atan2(cross, dot) * 57.29577951308232f;
}

void rhyDirLimit(float* cur, const float* target, float maxDeg) {
    if (maxDeg <= 0.0f) {
        return;
    }
    float c[3] = {cur[0], cur[1], cur[2]};
    float t[3] = {target[0], target[1], target[2]};
    const float nc = std::sqrt(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]);
    const float nt = std::sqrt(t[0] * t[0] + t[1] * t[1] + t[2] * t[2]);
    if (nc < 1e-6f) {
        cur[0] = nt > 1e-6f ? t[0] / nt : 0.0f;
        cur[1] = nt > 1e-6f ? t[1] / nt : 0.0f;
        cur[2] = nt > 1e-6f ? t[2] / nt : -1.0f;
        return;
    }
    c[0] /= nc;
    c[1] /= nc;
    c[2] /= nc;
    if (nt < 1e-6f) {
        return;
    }
    t[0] /= nt;
    t[1] /= nt;
    t[2] /= nt;

    float dot = c[0] * t[0] + c[1] * t[1] + c[2] * t[2];
    if (dot > 1.0f) {
        dot = 1.0f;
    }
    if (dot < -1.0f) {
        dot = -1.0f;
    }
    const float theta = std::acos(dot) * 57.29577951308232f;  // rad -> deg
    if (theta <= maxDeg || theta < 1e-4f) {
        cur[0] = t[0];
        cur[1] = t[1];
        cur[2] = t[2];
        return;
    }
    const float w = maxDeg / theta;  // slerp weight for this step
    const float s = std::sin(theta * 0.017453292519943295f);
    if (s < 1e-6f) {
        cur[0] = t[0];
        cur[1] = t[1];
        cur[2] = t[2];
        return;
    }
    const float a = std::sin((1.0f - w) * theta * 0.017453292519943295f) / s;
    const float b = std::sin(w * theta * 0.017453292519943295f) / s;
    float x = a * c[0] + b * t[0];
    float y = a * c[1] + b * t[1];
    float z = a * c[2] + b * t[2];
    const float nn = std::sqrt(x * x + y * y + z * z);
    if (nn < 1e-6f) {
        return;
    }
    cur[0] = x / nn;
    cur[1] = y / nn;
    cur[2] = z / nn;
}

// ------------------------------------------------------- handle cadence (n01)

void rhyCadenceInit(RhyCadence& c, float maxStepDeg, float maxRelLum,
                    int minMs, int maxMs) {
    // A budget of 0 would mean "rebuild every frame": the churn floor is what
    // protects the bridge, so an out-of-range budget is corrected rather than
    // honoured. Same reasoning as the [fade] floors in config.cpp.
    if (!(maxStepDeg >= 0.05f)) {
        maxStepDeg = 0.05f;
    }
    if (maxStepDeg > 30.0f) {
        maxStepDeg = 30.0f;
    }
    if (!(maxRelLum >= 0.005f)) {
        maxRelLum = 0.005f;
    }
    if (maxRelLum > 1.0f) {
        maxRelLum = 1.0f;
    }
    if (minMs < 20) {
        minMs = 20;  // 50 rebuilds per second per body is not a cadence
    }
    if (minMs > 10000) {
        minMs = 10000;
    }
    if (maxMs < minMs) {
        maxMs = minMs;
    }
    if (maxMs > 120000) {
        maxMs = 120000;  // 2 min: a body never rebuilt is a bug, not a cadence
    }
    c.maxStepDeg = maxStepDeg;
    c.maxRelLum = maxRelLum;
    c.minMs = minMs;
    c.maxMs = maxMs;
    c.elapsedMs = 0;
    c.intervalMs = maxMs;
    c.gapDeg = 0.0f;
    c.gapRel = 0.0f;
    c.degPerSec = 0.0f;
    c.relPerSec = 0.0f;
    c.rebuilds = 0;
    c.skips = 0;
}

void rhyCadenceObserve(RhyCadence& c, float degMoved, float relLumMoved,
                       float dtSec) {
    if (!(dtSec > 0.0f)) {
        return;
    }
    if (degMoved < 0.0f) {
        degMoved = 0.0f;
    }
    if (relLumMoved < 0.0f) {
        relLumMoved = 0.0f;
    }
    // EMA with a ~0.25 s time constant. One frame of a clock jump (the limiter
    // can publish 0.2 deg of direction in a frame) must not set the cadence for
    // the next minute, and a body that stopped moving must drift back to the
    // fixed period rather than stay pinned at the fast one.
    const float a = dtSec / (dtSec + 0.25f);
    c.degPerSec += a * (degMoved / dtSec - c.degPerSec);
    c.relPerSec += a * (relLumMoved / dtSec - c.relPerSec);
}

int rhyCadenceIntervalMs(const RhyCadence& c) {
    // How many ms of motion fit in each budget. A rate of zero means "no
    // constraint": the fixed cadence applies.
    float byDeg = c.degPerSec > 1e-4f ? c.maxStepDeg / c.degPerSec * 1000.0f
                                      : static_cast<float>(c.maxMs);
    float byLum = c.relPerSec > 1e-4f ? c.maxRelLum / c.relPerSec * 1000.0f
                                      : static_cast<float>(c.maxMs);
    float ms = byDeg < byLum ? byDeg : byLum;
    if (!(ms > 0.0f)) {
        ms = static_cast<float>(c.minMs);
    }
    int iv = static_cast<int>(ms);  // floor: never round up past what the budget allows
    if (iv > c.maxMs) {
        iv = c.maxMs;
    }
    if (iv < c.minMs) {
        iv = c.minMs;
    }
    return iv;
}

void rhyCadenceAdvance(RhyCadence& c, float dtSec) {
    if (!(dtSec > 0.0f)) {
        dtSec = 0.0f;
    }
    c.elapsedMs += static_cast<int>(dtSec * 1000.0f + 0.5f);
    c.intervalMs = rhyCadenceIntervalMs(c);
}

void rhyCadenceSetGap(RhyCadence& c, float degGap, float relGap) {
    c.gapDeg = degGap < 0.0f ? 0.0f : degGap;
    c.gapRel = relGap < 0.0f ? 0.0f : relGap;
}

bool rhyCadenceDue(const RhyCadence& c) {
    if (c.elapsedMs >= c.intervalMs) {
        return true;
    }
    // The safety net: the handle is already off by more than the budget. The churn
    // floor still applies, so this can never rebuild faster than minMs - which is
    // also why the step bound is max(budget, rate x minMs) either way.
    if (c.elapsedMs < c.minMs) {
        return false;
    }
    return c.gapDeg > c.maxStepDeg || c.gapRel > c.maxRelLum;
}

void rhyCadenceRebuilt(RhyCadence& c) {
    c.elapsedMs = 0;
}

// ------------------------------------------------------------- selection

namespace {
struct Cand {
    float d;
    int i;
    bool sticky;
};
}  // namespace

unsigned rhySelectLights(const float* xy, unsigned n, const int* active,
                         unsigned activeN, float px, float py, float rIn,
                         float rOut, unsigned cap, unsigned stickyExtra,
                         RhyWant* out, unsigned outCap, float hardRadius) {
    if (!xy || !out || outCap == 0) {
        return 0;
    }
    if (rOut < 0.0f) {
        rOut = 0.0f;
    }
    if (rIn < 0.0f || rIn > rOut) {
        rIn = rOut;
    }
    const float rOut2 = rOut * rOut;

    struct Cand {
        float d;
        int i;
        bool sticky;
    };
    // Static scratch: this runs on the render thread over a 7002-entry list, so
    // the allocation is reused rather than made per pass.
    static std::vector<Cand> cand;
    static std::vector<float> dist;
    cand.clear();
    cand.reserve(n);
    dist.assign(n, 0.0f);

    for (unsigned i = 0; i < n; ++i) {
        const float dx = xy[2 * i + 0] - px;
        const float dy = xy[2 * i + 1] - py;
        const float d2 = dx * dx + dy * dy;
        if (rOut > 0.0f && d2 > rOut2) {  // the WIDER bound: see smooth.h
            continue;
        }
        // active is sorted (see smooth.h): binary search, not a scan. A scan here
        // is 7002 x 320 comparisons per pass, on the render thread.
        const bool sticky =
            active && activeN > 0 &&
            std::binary_search(active, active + activeN, static_cast<int>(i));
        const float d = std::sqrt(d2);
        dist[i] = d;
        const Cand e = {d, static_cast<int>(i), sticky};
        cand.push_back(e);
    }
    std::sort(cand.begin(), cand.end(),
              [](const Cand& a, const Cand& b) { return a.d < b.d; });

    // n04: the lamps strictly inside hardRadius are a prefix of the sorted list,
    // and they are taken before the cap is consulted at all. The cap is a budget
    // for the band beyond the radius, not a licence to drop a lamp that is 100 m
    // from the player because 408 others are nearer.
    if (hardRadius > rOut) {
        hardRadius = rOut;  // a typo must not disable the cap everywhere
    }
    if (hardRadius < 0.0f) {
        hardRadius = 0.0f;
    }
    size_t forced = 0;
    while (forced < cand.size() && cand[forced].d < hardRadius) {
        ++forced;
    }
    if (forced > outCap) {
        forced = outCap;
    }

    const unsigned grow = cap + stickyExtra;
    static std::vector<int> chosen;
    chosen.clear();
    chosen.reserve(grow + forced);
    for (size_t k = 0; k < forced; ++k) {
        chosen.push_back(cand[k].i);
    }
    for (size_t k = forced; k < cand.size() && chosen.size() < grow; ++k) {
        if (cand[k].sticky) {
            chosen.push_back(cand[k].i);
        }
    }
    for (size_t k = forced; k < cand.size() && chosen.size() < cap; ++k) {
        if (!cand[k].sticky) {
            chosen.push_back(cand[k].i);
        }
    }
    unsigned written = 0;
    for (size_t k = 0; k < chosen.size() && written < outCap; ++k) {
        const int id = chosen[k];
        out[written].id = id;
        out[written].dist = dist[id];
        out[written].gain = rhyDistanceGain(dist[id], rIn, rOut);
        ++written;
    }
    return written;
}
