#ifndef RHY_SMOOTH_H_
#define RHY_SMOOTH_H_

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

// Rotate the unit vector cur toward the unit vector target by at most maxDeg
// degrees. Used for the sun and moon: the game clock can jump 100 degrees
// between two samples (measured: 104.5 deg), and a distant light's direction is
// baked at create time, so the handle follows a slew-limited direction instead.
void rhyDirLimit(float* cur, const float* target, float maxDeg);

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
// `active` lists the ids that currently have a handle and MUST be sorted
// ascending (both callers build it from a std::map, and the traffic one sorts):
// membership is looked up with a binary search, because the lamp list is 7002
// entries and this runs on the render thread - a linear scan per lamp would be
// 7002 x 320 comparisons every resync pass.
// Returns the number of entries written to out, nearest first, never more than
// cap + stickyExtra and never more than outCap.
unsigned rhySelectLights(const float* xy, unsigned n, const int* active,
                         unsigned activeN, float px, float py, float rIn,
                         float rOut, unsigned cap, unsigned stickyExtra,
                         RhyWant* out, unsigned outCap);

#endif  // RHY_SMOOTH_H_
