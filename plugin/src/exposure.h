#ifndef RHY_EXPOSURE_H_
#define RHY_EXPOSURE_H_

// n22: the renderer's exposure anchor as a function of the SUN, not of the clock.
//
// WHY THIS FILE EXISTS
// --------------------
// n16 measured the tonemapper and shipped it: `rtx.tonemappingMode = 0` (Global)
// plus `rtx.tonemap.exposureBias = -0.5` in config/rtx.conf, .perf and .nopops.
// In Global mode the exposure of the whole frame is one number,
//     exposureFactor = 2^(rtx.tonemap.exposureBias + userEVBias)
// (rtx_tone_mapping.cpp:234; userEVBias = (userBrightness-50)/100 *
// userBrightnessEVRange, rtx_options.h:1514-1517). That is exactly what stops the
// daytime frame re-exposing per region - and exactly what makes it wrong at
// night, because rtx.conf is a file and the game clock is not.
//
// Measured, from build/tmp/n16/good-before.json (the shots the 2026-11-12 report
// called good, same tour as the daytime shots, same live config):
//
//   noon, our four daytime shots      p50 123-170,  0.28-1.79 % black, headroom 1.4-1.8
//   02 grove pools       21:00         p50  17.4,    34.5 % black,     headroom 8.6
//   03 sodium lamp       21:00         p50   7.0,    52.2 % black,     headroom 28.2
//   09 grove dawn        05:30         p50  27.8,     8.0 % black,     headroom 4.0
//
// A -0.5 EV anchor sized to pull p50 147 -> ~110 at noon takes a quarter of the
// light off a frame that already sits at p50 7-17 with half its pixels black. One
// static number cannot serve both ends of the day. So the anchor moves with the
// sun, and it moves through the same discipline everything else in this plugin
// moves through: bounded rate, floors that cannot be talked out of, and a test.
//
// WHY SUN ELEVATION AND NOT THE CLOCK
// -----------------------------------
// The task's own test of the design: 05:30 and 19:30 must get the same exposure.
// A clock rule cannot do that (05:30 is dawn, 19:30 is dusk, and the two are 14
// hours apart on the dial but a few degrees apart in the sky). `skyDaylightFactor`
// (plugin/src/solar.cpp:301) is a stateless smoothstep over two ELEVATIONS, so
// symmetry is a property of the function, not a thing to remember.
//
// WHY THE NOON SIDE IS A NO-OP
// ----------------------------
// `day_ev` defaults to -0.5, the number the three profiles already carry. At full
// daylight the plugin therefore publishes exactly what rtx.conf says, so this
// change cannot move the daytime grade at all: the numbers the next live run
// measures for n16 stay the numbers it measures for n16, and the only thing that
// moves is the night end. tests/exposure_check.cpp asserts it.
//
// WHAT IS DELIBERATELY NOT HERE
// -----------------------------
// * rtx.userBrightness / rtx.userBrightnessEVRange are never written. The real
//   option names are the doubled ones (rtx.userBrightness.userBrightness), both
//   carry RtxOptionFlags::UserSetting, they belong to the Remix UI's Brightness
//   slider, and user.conf sits ABOVE rtx.conf (rtx_option_layer.cpp:386-397): a
//   profile or a plugin that writes them fights the player's slider. The EV is
//   the same either way - 2^(exposureBias + userEVBias) - so the mod layer is
//   where a mod's anchor belongs. Pinned by tests/test_exposure_anchor.py.
// * rtx.tonemappingMode is not touched at runtime. It is a real option, so a
//   day/night MODE switch is possible, but switching mode re-arms the whole local
//   pyramid (three exposures + mip fit) and changes the look discontinuously at
//   the switch point. That is a different, bigger experiment.
// * rtx.postfx.exposureFraction is not an exposure knob at all: it is the shutter
//   fraction of the post-FX motion blur (rtx_postFx.cpp:127,294). n16's audit
//   found it on the candidate list; it stays off it.
//
// Everything here is plain arithmetic: no windows.h, no Remix, no game pointers,
// so tests/exposure_check.cpp can drive a whole simulated day and assert the
// bounds directly. The pushing itself is sun.cpp's job.

#include "smooth.h"

struct RhyExposureParams {
    float dayEv;        // the anchor at full daylight (daylight factor 1)
    float nightEv;      // ... at night (daylight factor 0)
    float duskDeg;      // sun elevation where the ramp starts, going down
    float dawnDeg;      // ... and where daylight is back, going up
    float maxEvPerSec;  // the slew bound: |dEV| <= maxEvPerSec per second
    float minEv;        // hard clamp, both anchors and the published value
    float maxEv;        // ... (the Remix UI's own span is -4..+4)
};

// The shipped defaults, in one place so the test and the plugin cannot disagree:
// -0.5 at noon (identical to config/rtx.conf's static anchor), +0.5 at night, the
// same twilight band [sky] uses (-10..+4 degrees), 0.05 EV/s, and the UI's -4..+4.
void rhyExposureDefaults(RhyExposureParams& p);

// Clamp a caller-supplied set of params. These are FLOORS, not preferences, in
// the same sense as rhyFadeClampRampSeconds: an ini typo must not be able to
// bring back the instant change.
//   * maxEvPerSec: a non-positive or absurd bound falls back to the shipped
//     0.05; anything above 0.25 EV/s is refused, because the full day/night
//     swing would then cross in 4 s - that is a pop wearing a rate limit.
//   * minEv/maxEv: clamped inside the runtime's own -8..+8 (the UI shows -4..+4),
//     and min < max is enforced or the clamp is a swap, i.e. nonsense.
//   * dayEv/nightEv: clamped into [minEv, maxEv]. A 10 EV anchor is not a look,
//     it is a black frame or a white one.
//   * duskDeg/dawnDeg: dusk < dawn, else the shipped band (same rule as [sky]).
void rhyExposureClamp(RhyExposureParams& p);

// The target EV for a sun elevation: the daylight factor (solar.cpp) blends
// nightEv -> dayEv, then the clamp. Pure function of the elevation, so the same
// elevation always gives the same EV whatever the clock said to get there.
float rhyExposureTargetEv(const RhyExposureParams& p, float sunElevationDeg);

// The follower sun.cpp owns: the target above, put through RhySignedRate.
struct RhyExposureAnchor {
    RhySignedRate follow;
    bool seeded;          // first frame publishes its target instead of ramping to it
    float maxStepSeen;    // biggest single-frame move since seeding, for the log
    float targetSeen;     // the last target, for the log
};

// Seed at `sunElevationDeg`'s own target. Same rule as the sky ambient: a plugin
// that has just connected to the bridge must not fade the world up from black -
// and the renderer's own global curve mixes the previous frame's at exp(-0.07)
// (~14 frames, tonemapping_tone_curve.comp.slang), so even this one step is
// smoothed by the runtime over a quarter of a second.
void rhyExposureAnchorInit(RhyExposureAnchor& a, const RhyExposureParams& p,
                           float sunElevationDeg);

// One frame. Returns the EV to publish (the caller decides whether it moved
// enough to be worth a SetConfigVariable; rhyRtxSetFloat's minDelta does that).
// Re-reading the ini cannot change the published value faster than the bound:
// the params are re-clamped every frame and the follower keeps its value.
float rhyExposureAnchorStep(RhyExposureAnchor& a, const RhyExposureParams& p,
                            float sunElevationDeg, float dt);

// The value currently published, for the once-a-second log line.
float rhyExposureAnchorEv(const RhyExposureAnchor& a);

#endif  // RHY_EXPOSURE_H_
