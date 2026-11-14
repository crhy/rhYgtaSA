#ifndef RHY_SMOOTH_H_
#define RHY_SMOOTH_H_

#include <cstddef>  // size_t (the n08 drain queue)
#include <cstdint>  // int32_t (wraparound-safe tick comparison)
#include <vector>   // the n08 drain queue's storage

// s28: the time behaviour of every lighting value this plugin publishes.
//
// WHY THIS FILE EXISTS
// --------------------
// rhY's #1 complaint (2026-11-12 test, finding 1): "the whole-world lighting
// changes INSTANTLY while running or driving". The measurements in
// .strata/notes-s28-lighting-no-pops.md say why: the street layer went from 0
// to 320 live handles, or back, in ONE frame 20 times in a 25 minute run;
// rtx.skyBrightness stepped 0.02 -> 1.00 (a 50x ambient step) 9 times; a signal
// was destroyed and re-created in another colour 766 times.
//
// The Remix C API bakes a light's radiance at CreateLight time
// (third_party/remix/remix_c.h: LightInfo{hash, radiance} + DrawLightInstance,
// there is no UpdateLight), so the ONLY way to fade a light is to re-create it
// at intermediate radiance. Everything here exists to make that cheap and,
// above all, bounded: no published lighting value may move by more than a small
// amount in one frame, whatever the game does underneath.
//
// Two primitives cover the whole problem:
//
//   RhyRate  - a follower for a continuous value (rtx.skyBrightness, the sun's
//              radiance). The move per step is bounded multiplicatively AND
//              absolutely, so a target that jumps 50x still arrives as a ramp.
//   RhyFade  - a 0..1 envelope for one light. `gain` is the animated value,
//              `published` the gain actually baked into the live handle, and the
//              gap between them is closed one `step` at a time, each step
//              costing one CreateLight. A light is only ever destroyed after its
//              published gain has ramped to zero.
//
// Everything here is pure arithmetic: no windows.h, no Remix, no game pointers,
// so tests/smooth_check.cpp can feed it a synthetic sequence and assert the
// per-frame bounds directly.

// ---------------------------------------------------------------- rate limit

struct RhyRate {
    float cur;         // the value we have actually published
    float relPerSec;   // max multiplicative rate: 3.0 = at most 3x up or 3x down per second
    float absPerSec;   // extra absolute allowance per second, so values near 0 can move
    float eps;         // deadband: within this of the target, snap to the target
};

void rhyRateInit(RhyRate& r, float value, float relPerSec, float absPerSec,
                 float eps);

// Advance cur toward target over dt seconds. The move is bounded by
//     |move| <= cur * (relPerSec^dt - 1) + absPerSec * dt
// so no matter how far the target jumps, one frame cannot show a step bigger
// than that. Returns true when cur changed (the caller then decides whether the
// change is big enough to be worth pushing to the renderer).
bool rhyRateStep(RhyRate& r, float target, float dt);

// ---------------------------------------------------- signed rate limit (n22)

// A follower for a value whose meaningful unit is ABSOLUTE and whose range
// CROSSES ZERO: an exposure bias in EV. RhyRate cannot carry one. Its bound is
// multiplicative, and 2^EV is never negative, so the EV itself is not the thing
// you can multiply: at cur = 0 the relative term vanishes and the follower is
// left with absPerSec alone, while at cur = -3 the same "3x per second" would
// allow a 6 EV jump. It also clamps cur and target to >= 0, which is right for
// an ambient level and wrong for a signed trim. So the exposure gets the shape
// its unit actually has: a straight slew limit.
//
// n22: rtx.tonemap.exposureBias is one number for the whole frame (Global
// tonemapping, rtx_tone_mapping.cpp:234), so a change to it is the whole image
// brightening - the pop every other limiter in this file exists to prevent. The
// game clock can jump 104.5 degrees of sun elevation between two samples
// (measured, 2026-11-12), and the anchor is a function of that elevation, so
// without a bound here a clock jump would be a one-frame exposure step.
struct RhySignedRate {
    float cur;        // the value we have actually published
    float maxPerSec;  // |move| <= maxPerSec * dt, always
    float eps;        // deadband: within this of the target, snap to the target
};

void rhySignedRateInit(RhySignedRate& r, float value, float maxPerSec, float eps);

// Advance cur toward target over dt seconds, moving by at most maxPerSec * dt.
// Returns true when cur changed. Unlike rhyRateStep it never clamps the value
// to >= 0: the caller owns the range (see rhyExposureClamp in exposure.h).
bool rhySignedRateStep(RhySignedRate& r, float target, float dt);

// ------------------------------------------------------------------ fades

// What the caller must do to the Remix handle after this frame's tick.
enum RhyFadeOp {
    kRhyFadeNone = 0,      // nothing: keep drawing the handle we have
    kRhyFadeCreate,        // first handle: build it with gain == published
    kRhyFadeRecreate,      // replace the handle: same id, radiance * published
    kRhyFadeRetire         // published reached 0: destroy the handle, drop the light
};

struct RhyFade {
    float gain;          // the animated value, 0..1
    float target;        // what we want (distance falloff * layer on/off)
    float published;     // the gain baked into the live handle; < 0 = no handle
    float secondsUp;     // seconds for a full 0 -> 1 ramp (never < 0.5)
    float secondsDown;   // seconds for a full 1 -> 0 ramp (never < 0.5)
};

// Clamp the ramp times to the task's floor (>= 0.5 s) and return the pair the
// config gave, so a typo in rhyremix.ini cannot bring back the instant switch.
void rhyFadeClampRampSeconds(float& up, float& down);

// Re-time an existing envelope. `minSeconds` is the floor THIS ramp must obey:
// a light appearing or disappearing passes 0.5 (the task's floor), while a
// traffic signal's colour cross-fade passes something shorter - there the total
// output is held roughly constant (one handle ramps down as the other ramps up),
// and half a second of red-and-green overlap on a signal head is its own kind
// of wrong. Values below 0.05 s are refused: that is one frame at 20 fps, which
// is an instant change wearing a disguise.
void rhyFadeSetRamp(RhyFade& f, float up, float down, float minSeconds);

void rhyFadeInit(RhyFade& f, float secondsUp, float secondsDown);

// Move gain toward target at 1/secondsUp or 1/secondsDown per second. Call this
// once per frame for every light, whether or not a create is affordable.
void rhyFadeAdvance(RhyFade& f, float dt);

// True when the handle no longer matches the envelope closely enough (or when a
// light that has faded to nothing must be retired). Checked before the frame's
// CreateLight budget is spent, so a light that cannot be re-created this frame
// simply keeps its current handle - it never jumps.
bool rhyFadeDue(const RhyFade& f, float step, float eps);

// Close the gap by at most one `step` and report the handle operation. Only call
// this when a CreateLight is affordable. A kRhyFadeRetire also has to be paid for:
// the modules call rhySpendCreateSlot() for it, because a fade that finished and
// then destroyed every remaining light in one frame is the same pop by another
// route (tests/smooth_check.cpp measures it).
RhyFadeOp rhyFadeApply(RhyFade& f, float step, float eps);

// ------------------------------------------------------------- distance gain

// The replacement for a hard radius cut-off: 1 inside fullRadius, ramping
// linearly to 0 at zeroRadius, 0 beyond. The lamp modules use their existing
// hysteresis band (max_distance -> exit_distance) as this fade band, so the
// hard cut-off now happens where the light is already at zero gain.
float rhyDistanceGain(float dist, float fullRadius, float zeroRadius);

// ------------------------------------------------------------- direction

// Angle between two directions, in degrees, measured as atan2(|a x b|, a.b).
// NOT acos(a.b): the sun at rest moves 0.002 deg per frame, and a float dot
// product that close to 1 makes acos return noise (it reported 0.02 deg for a
// real 0.002 deg step in tests/handle_check.cpp, 10x over). A cadence or a slew
// driven by that noise rebuilds every body at its churn floor forever.
float rhyAngleDeg(const float* a, const float* b);

// Rotate the unit vector cur toward the unit vector target by at most maxDeg
// degrees. Used for the sun and moon: the game clock can jump 100 degrees
// between two samples (measured: 104.5 deg), and a distant light's direction is
// baked at create time, so the handle follows a slew-limited direction instead.
void rhyDirLimit(float* cur, const float* target, float maxDeg);

// ------------------------------------------------------- handle cadence (n01)

// A Remix distant light's direction and radiance are baked at CreateLight time,
// so "move the sun" means destroy the handle and create a new one. s28/s29 chose
// that moment from the VALUE: rebuild when the body has moved more than
// [sun] step_deg (1.0 deg) or brightened by 5%; s29 left that as its open note
// T7. It is bounded and monotone, so it is a staircase and not a flicker, but it
// has two properties a clock does not: the step that triggers the rebuild is as
// big as the whole threshold (the handle is replaced exactly when the difference
// has grown to 1.0 deg, twice the sun's 0.53 deg disc), and the interval between
// rebuilds follows the body's rate, so it is ~8 s while the clock runs normally
// and every ~80 ms while the direction limiter is slewing at its 12 deg/s cap.
//
// n01 takes the decision from the clock instead. The period is fixed
// (`maxMs`), and the s28 limiters only ever *shorten* it: observe how fast the
// published direction and luminance are actually moving and rebuild often enough
// that no rebuild carries more than `maxStepDeg` of direction or `maxRelLum` of
// luminance. `minMs` is the floor that keeps bridge churn bounded, and the
// per-frame create budget in remix.cpp bounds it again on top of that.
struct RhyCadence {
    float maxStepDeg;   // direction budget for one rebuild, degrees
    float maxRelLum;    // luminance budget for one rebuild, fraction of itself
    int   minMs;        // churn floor: never rebuild a body faster than this
    int   maxMs;        // the fixed cadence: a still body is rebuilt this often
    int   elapsedMs;    // ms since the last rebuild
    int   intervalMs;   // period in effect right now
    float gapDeg;       // how far the live handle is from what we would publish
    float gapRel;       // ... in luminance, as a fraction of itself
    float degPerSec;    // smoothed rate of the published direction, deg/s
    float relPerSec;    // ... and of the published luminance, fraction/s
    int   rebuilds;     // rebuilds decided, for the log line
    int   skips;        // periods that needed no bridge call at all
};

// Clamp the budgets and the bounds. maxMs < minMs is a typo and would make the
// floor unreachable, so the floor wins.
void rhyCadenceInit(RhyCadence& c, float maxStepDeg, float maxRelLum,
                    int minMs, int maxMs);

// Feed one frame of what the limiters actually published: `degMoved` degrees of
// direction and `relLumMoved` of fractional luminance, over `dtSec` seconds.
// Smoothed over ~0.25 s, because one frame of a clock jump must not set the
// cadence for the next minute - and a rate that stopped moving must clear it.
void rhyCadenceObserve(RhyCadence& c, float degMoved, float relLumMoved,
                       float dtSec);

// One frame elapsed. Advances the clock and re-derives the period.
void rhyCadenceAdvance(RhyCadence& c, float dtSec);

// Report how far the LIVE handle is from what would be published now (degrees of
// direction, and luminance as a fraction of itself). The period is derived from a
// rate, and a rate that collapses - a radiance ramp reaching its target, a slew
// that lands - would otherwise leave a handle that is already off by more than the
// budget sitting there for a whole period. Measured in tests/handle_check.cpp:
// without this, the last swap of a 6.0 -> 0.02 twilight ramp carried 0.84 of its
// own luminance.
void rhyCadenceSetGap(RhyCadence& c, float degGap, float relGap);

// The period in effect: what the two budgets allow, clamped to [minMs, maxMs].
int rhyCadenceIntervalMs(const RhyCadence& c);

// True when the handle is due. Two triggers, one clock and one safety net: the
// period, or a gap already past the budget once the churn floor has passed (the
// floor still wins, so the step bound is max(budget, rate x minMs) either way).
// Neither consumes the due state: a rebuild the frame's create budget refuses has
// to be retried next frame, and a rule that forgot a refused rebuild would be a
// threshold rule by another name.
bool rhyCadenceDue(const RhyCadence& c);

// The caller rebuilt (or established that the handle already matches): start the
// next period.
void rhyCadenceRebuilt(RhyCadence& c);

// ------------------------------------------------------------- selection

// One selected light: which list entry, how far it is, and the gain it should
// end up at.
struct RhyWant {
    int id;
    float dist;
    float gain;
};

// Shared light selection with the two-radius hysteresis AND the distance fade.
//   * a light that has never had a handle must be inside rIn to be chosen;
//   * a light that already has one survives out to rOut (the wider bound);
//   * the set may grow to cap + stickyExtra before the farthest are dropped;
//   * every chosen light gets a target gain of rhyDistanceGain(dist, rIn, rOut),
//     so nothing appears or disappears at full radiance.
//
// n04: `hardRadius` is what makes the cap stop evicting lights that are close.
// A light strictly inside hardRadius is ALWAYS selected: the cap may only drop
// lights in the band between hardRadius and rOut. Without it the cap is a
// distance rule wearing a count: measured on the real 7002-lamp list, the
// worst-case number of lamps within 300 m of a point is 408 and within 380 m is
// 552, against a shipped cap of 320+90 - so in a dense district the farthest
// lamps were being dropped while they were still 200 m away, and picking them up
// again on the next pass is a street light blinking as you walk. Pass 0 to keep
// the old behaviour (the cap binds everywhere); both lamp modules pass rIn, so
// the cap only ever bites beyond the radius, where a lamp's contribution is
// already invisible and its gain is already ramping to 0.
// `active` lists the ids that currently have a handle and MUST be sorted
// ascending (both callers build it from a std::map, and the traffic one sorts):
// membership is looked up with a binary search, because the lamp list is 7002
// entries and this runs on the render thread - a linear scan per lamp would be
// 7002 x 320 comparisons every resync pass.
// Returns the number of entries written to out, nearest first, never more than
// outCap - and never more than cap + stickyExtra unless the excess is lamps
// inside hardRadius, which the cap is not allowed to drop.
unsigned rhySelectLights(const float* xy, unsigned n, const int* active,
                         unsigned activeN, float px, float py, float rIn,
                         float rOut, unsigned cap, unsigned stickyExtra,
                         RhyWant* out, unsigned outCap, float hardRadius = 0.0f);

// ------------------------------------------------------- frame pacing (n08)
//
// n08 (finding G, "FPS a little less steady than previous builds") is about the
// TAIL, not the average: the 2026-11-12 10:22 run has a slightly BETTER mean
// frametime than the 00:48 run (17.50 vs 17.72 ms) and 1.8x the frames over
// 25 ms (137 vs 77 per minute). A tail is made by whatever lands on top of a
// frame that already had no headroom - the median frame of both runs is
// 16.68 ms, exactly the 60 Hz budget - so the plugin's job is to make sure the
// work it owns is never a burst, whatever else is happening.
//
// What the two runs' own logs say we own, measured with tools/perf-report.py and
// the numbers in docs/FINDINGS.md:
//
//   * `pending_destroy` reached 320 in the 00:48 run and 205 in the 10:22 run.
//     Every one of those handles was destroyed inside ONE frame: rhyFlushDelayed()
//     had no bound. A create is already bounded (creates_per_frame, s28); a
//     destroy never was, and a destroy is a bridge call like any other.
//   * the street and the signal layer re-scanned their lists on the SAME frame
//     (both used `frame % period == 1`), so one frame every 30 carried both
//     scans. Measured on the real lists: 11.7 us for the 7002-lamp street list
//     plus 8.0 us for the 1522-entry signal list. Small - but it is a burst that
//     always lands on the same frame, and "always the same frame" is how a
//     periodic hitch becomes visible.
//
// So: a drain queue that can only be emptied `perFrame` items at a time, and a
// resync schedule that can put two modules' periods out of phase. Both are pure
// arithmetic (no windows.h, no Remix): tests/drain_spread_check.cpp asserts the
// per-frame bound directly, the way tests/smooth_check.cpp asserts the fade ones.

// Items become due at a tick (GetTickCount()-style, so the comparison is
// wraparound-safe: an item is due when int32(now - due) >= 0). A frame takes at
// most `perFrame` of the due items, earliest due first, so a queue that fills up
// with 320 items drains over several frames instead of spending them all inside
// one Present.
template <class T>
class RhyDrainQueue {
public:
    void add(unsigned dueTick, const T& value) {
        items_.push_back(Item{dueTick, value});
    }

    size_t size() const { return items_.size(); }

    bool empty() const { return items_.empty(); }

    // How many items are due right now (telemetry: this is the number a frame
    // would have had to process, bounded or not).
    size_t dueCount(unsigned now) const {
        size_t n = 0;
        for (size_t i = 0; i < items_.size(); ++i) {
            if (static_cast<int32_t>(now - items_[i].due) >= 0) {
                ++n;
            }
        }
        return n;
    }

    // Copy up to `perFrame` due items into out (earliest due first) and remove
    // them. perFrame = 0 means no bound (the pre-n08 behaviour, kept as the
    // documented escape hatch). Returns how many were taken.
    size_t takeDue(unsigned now, size_t perFrame, T* out, size_t outCap) {
        size_t taken = 0;
        while (!items_.empty()) {
            if (perFrame != 0 && taken >= perFrame) {
                break;
            }
            if (outCap != 0 && taken >= outCap) {
                break;
            }
            size_t best = items_.size();
            for (size_t i = 0; i < items_.size(); ++i) {
                if (static_cast<int32_t>(now - items_[i].due) < 0) {
                    continue;  // not due yet
                }
                if (best == items_.size() ||
                    static_cast<int32_t>(items_[i].due - items_[best].due) < 0) {
                    best = i;
                }
            }
            if (best == items_.size()) {
                break;  // nothing due
            }
            if (out) {
                out[taken] = items_[best].value;
            }
            items_.erase(items_.begin() + best);
            ++taken;
        }
        return taken;
    }

    // Drop every item the predicate accepts (n08: the pending-destroy queue uses
    // this to remove one list index's handles right before re-creating them).
    // Returns how many were removed.
    template <class Pred>
    size_t removeIf(Pred p) {
        const size_t before = items_.size();
        size_t kept = 0;
        for (size_t i = 0; i < items_.size(); ++i) {
            if (p(items_[i].value)) {
                continue;
            }
            if (kept != i) {
                items_[kept] = items_[i];
            }
            ++kept;
        }
        items_.resize(kept);
        return before - kept;
    }

    // Drop everything (n08: the overflow path of the pending-destroy queue).
    void clear() { items_.clear(); }

    // Read-only walk for telemetry/tests.
    unsigned dueAt(size_t i) const { return items_[i].due; }
    const T& valueAt(size_t i) const { return items_[i].value; }

private:
    struct Item {
        unsigned due;
        T value;
    };
    std::vector<Item> items_;
};

// Is this the frame on which a periodic job runs? `period` frames apart, and
// `phase` (0 <= phase < period) slides the whole schedule along the frame axis.
// Two modules with the same period and different phases never run together.
// Returns 1 on a due frame, 0 otherwise; period 0 or 1 means "every frame".
unsigned rhyResyncDue(unsigned frame, unsigned period, unsigned phase);

#endif  // RHY_SMOOTH_H_
