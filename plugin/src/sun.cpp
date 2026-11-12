#include "sun.h"

#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "config.h"
#include "delayed.h"
#include "log.h"
#include "remix.h"
#include "rtxconf.h"
#include "smooth.h"
#include "solar.h"
#include "tod.h"

static const float kDeg = 0.017453292519943295f;  // pi / 180

// One live distant light: the direction and radiance it was created with, so
// the next frame can tell whether the handle still matches what we want.
// The Remix runtime fixes direction and radiance at CreateLight time, so
// "moving the sun" means retiring the handle and creating a new one.
//
// s28: that also means the sun can only be dimmed, brightened or turned by
// re-creating it, and the game clock this run stepped by up to 1372 game minutes
// between two samples (measured: sun elevation 104.5 deg in one 10 s sample,
// daylight 0.00 -> 1.00 nine times). So the handle follows a SLEW-LIMITED
// direction and three rate-limited radiance channels rather than the raw
// geometry: the target may jump, what gets published cannot.
struct DistantLight {
    remixapi_LightHandle handle;
    float dir[3];         // direction baked into the live handle
    float radiance[3];    // radiance baked into the live handle
    float lum;            // luminance of that radiance, for the "did anything at
                          // all change" test (n01: no longer a rebuild trigger)
    int rebuilds;
    bool apiBroken;
    int tag;  // delayed.h tag namespace: see kTagSun / kTagMoon
    // s28: what the geometry asked for, and what the rate limiters have published
    float wantDir[3];
    float wantRgb[3];
    float pubDir[3];
    RhyRate rate[3];
    bool seeded;  // first target is published as-is, so connecting to the bridge
                  // does not fade the world up from black
    // n01 (s29 note T7): the rebuild schedule, and what the limiters published
    // last frame - which is the only honest input to a cadence.
    float pubDirPrev[3];
    float pubLumPrev;
    RhyCadence cad;
    float maxStepDegSeen;  // largest direction step one rebuild actually carried
};

static DistantLight g_sun = {nullptr, {0.0f, 0.0f, -1.0f}, {0, 0, 0}, -1.0f, 0, false,
                             kTagSun, {0, 0, -1}, {0, 0, 0}, {0, 0, -1},
                             {{0, 3.0f, 0.25f, 0.001f}, {0, 3.0f, 0.25f, 0.001f},
                              {0, 3.0f, 0.25f, 0.001f}}, false,
                             {0, 0, -1}, 0.0f,
                             {0.5f, 0.05f, 150, 2000, 0, 2000, 0.0f, 0.0f, 0.0f, 0.0f,
                              0, 0},
                             0.0f};
static DistantLight g_moon = {nullptr, {0.0f, 0.0f, -1.0f}, {0, 0, 0}, -1.0f, 0, false,
                              kTagMoon, {0, 0, -1}, {0, 0, 0}, {0, 0, -1},
                              {{0, 3.0f, 0.25f, 0.001f}, {0, 3.0f, 0.25f, 0.001f},
                               {0, 3.0f, 0.25f, 0.001f}}, false,
                              {0, 0, -1}, 0.0f,
                              {0.5f, 0.05f, 150, 2000, 0, 2000, 0.0f, 0.0f, 0.0f,
                               0.0f, 0, 0},
                              0.0f};

// The last sky state, for the periodic log line and for the lamp modules, which
// need "is it night?" and must not disagree with what the sun is doing.
static float g_sunElev = -90.0f;
static float g_moonElev = -90.0f;
static float g_daylight = 0.0f;
static bool g_lampsOn = true;

// The time axis, live. s29: the clock bytes change once every ~2000 ms and the
// calendar bytes next to them are not a calendar, so neither is used directly any
// more. Both go through plugin/src/tod.cpp, which turns them into a continuous game
// minute that never runs backwards except at the midnight wrap, and a weather state
// that turns an ini edit into a ramp instead of a colour change.
static RhyTodClock g_tod;
static RhyCalendar g_cal;
static RhyWeather g_weather;
static bool g_todInit = false;
static float g_todSeed = 0.0f;

// ------------------------------------------------------------------ clock

// Game clock: hour at 0xB70153, minute at 0xB70152 (the same source the lamp
// and traffic-light managers use). These are the ONLY two bytes of the game's
// clock that are trustworthy; the calendar next door is handled separately, and
// the value returned here is deliberately not what drives the sky any more -
// plugin/src/tod.cpp turns it into a continuous time first.
static void gameClock(const RhyConfig* c, int* hour, int* minute) {
    uint8_t* hr = reinterpret_cast<uint8_t*>(0xB70153);
    uint8_t* mn = reinterpret_cast<uint8_t*>(0xB70152);
    if (c && c->sun_force_hour >= 0) {
        // Pinning the clock is the only practical way to compare noon against
        // sunset in one sitting: SA runs an in-game minute every ~2 real
        // seconds, so 12:00 to 20:00 is twenty minutes of waiting. Written once
        // per change rather than every frame, so the clock still ticks.
        //
        // s29: and the value we then USE is the one we asked for, not a re-read
        // of the byte. Reading it back made the plugin (once a second) and the
        // game's own clock update (every frame) alternate writers of the same
        // byte, which is a self-inflicted A/B/A/B flicker: the sun sat at the
        // pinned time and at whatever the game had advanced it to, on alternate
        // samples. Writing is a request; the forced value is the answer.
        if (*hr != static_cast<uint8_t>(c->sun_force_hour)) {
            *hr = static_cast<uint8_t>(c->sun_force_hour);
        }
        if (c->sun_force_minute >= 0 &&
            *mn != static_cast<uint8_t>(c->sun_force_minute)) {
            *mn = static_cast<uint8_t>(c->sun_force_minute);
        }
        *hour = c->sun_force_hour;
        *minute = c->sun_force_minute >= 0 ? c->sun_force_minute
                                           : static_cast<int>(*mn);
        return;
    }
    *hour = static_cast<int>(*hr);
    *minute = static_cast<int>(*mn);
}

// GTA SA's calendar. The hour and minute bytes are trusted above; the day and
// month were guessed at +1 and +5 and measured 0/0 in a live run, so those
// offsets are wrong. Worse, they are not a calendar at all in the runs we have:
// the "month" byte walks 25,41,57,...,249,9,25 (+16 per sample, some other
// variable entirely) while "day" sits at 3, and every time that counter passes
// through 1..12 the old sanity check accepted it. 20 of 409 logged sky samples
// used such a date, and the sun's elevation swung by up to 43.4 degrees and
// back on the next sample - the flicker of finding 2, in its purest form.
//
// So the bytes now go through rhyCalendarUpdate(), which requires the same
// (month, day) pair over several consecutive samples before it is believed, and
// refuses a date that steps backwards or jumps more than a day. The raw bytes
// are still dumped once so the next pass can find the real offsets.
static void gameCalendarBytes(int* monthOut, int* dayOut) {
    static bool s_dumped = false;
    if (!s_dumped) {
        s_dumped = true;
        const uint8_t* p = reinterpret_cast<const uint8_t*>(0xB70150);
        rhyLog("clock: raw bytes 0xB70150..0xB7015F = %d %d %d %d %d %d %d %d | "
               "| %d %d %d %d %d %d %d %d  (hour@53=%d minute@52=%d; day@54 and "
               "month@58 are not a calendar: they walk by +16 per sample, so "
               "day_of_year from the ini is used unless the pair holds still)",
               p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7],
               p[8], p[9], p[10], p[11], p[12], p[13], p[14], p[15], p[3], p[2]);
    }
    if (monthOut) {
        *monthOut = static_cast<int>(*reinterpret_cast<const uint8_t*>(0xB70158));
    }
    if (dayOut) {
        *dayOut = static_cast<int>(*reinterpret_cast<const uint8_t*>(0xB70154));
    }
}

// ------------------------------------------------------------------ legacy orbit

// The original model: 360 degrees per 1440 game minutes on a fixed plane. Kept
// because it is the only way to A/B "is the astronomical model the reason this
// looks different?", and because a user may want a sun that ignores the season.
static void legacySunDirection(const RhyConfig& c, float minuteOfDay, float* out) {
    const float ang = minuteOfDay * 0.25f;
    const float phi = (ang + 180.0f + c.sun_spin) * kDeg;
    const float tilt = c.sun_tilt * kDeg;
    const float roll = c.sun_roll * kDeg;

    const float s = sinf(phi);
    const float co = cosf(phi);
    float x = s * cosf(tilt);
    float y = s * sinf(tilt);
    float z = -co;

    const float y2 = y * cosf(roll) - z * sinf(roll);
    const float z2 = y * sinf(roll) + z * cosf(roll);
    y = y2;
    z = z2;

    const float n = sqrtf(x * x + y * y + z * z);
    if (n > 1e-6f) {
        out[0] = x / n;
        out[1] = y / n;
        out[2] = z / n;
    }
}

// ------------------------------------------------------------------ radiance

// Radiance of the direct beam, normalised so that `base` is the value at the
// zenith. Dividing by the zenith transmittance is what makes the ini number
// mean something: without it, "radiance=6" would really be 6 x 0.83 = 5.0 and
// the number would drift with turbidity for reasons the user cannot see.
static void beamRadiance(const SkyBody& b, float base, const float* tint,
                         float turbidity, float* out) {
    // exp(-tau * 1) per channel at one air mass. Same constants as solar.cpp;
    // recomputed rather than shared because it is three multiplies and it keeps
    // solar.cpp free of any knowledge about radiance at all.
    static const float kTau[3] = {0.0681f + 0.020f, 0.1054f + 0.030f,
                                  0.2162f + 0.010f};
    const float beta = turbidity < 0.0f ? 0.0f : turbidity;
    static const float kAer[3] = {0.7576f, 1.0f, 1.2687f};

    for (int c = 0; c < 3; ++c) {
        const float zenithT = expf(-(kTau[c] + beta * kAer[c]));
        const float t = zenithT > 1e-4f ? b.transmit[c] / zenithT : 0.0f;
        out[c] = base * tint[c] * t;
    }
}

static remixapi_LightHandle createDistant(DistantLight& which,
                                          const float* d, const float* rgb,
                                          float angular,
                                          unsigned long long hash,
                                          float volumetric, const char* name) {
    const remixapi_Interface* api = rhyRemixApi();
    if (!api || !api->CreateLight) {
        return nullptr;
    }

    remixapi_LightInfo info;
    remixapi_LightInfoDistantEXT ext;
    std::memset(&info, 0, sizeof info);
    std::memset(&ext, 0, sizeof ext);

    info.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
    info.hash = hash;
    info.radiance.x = rgb[0];
    info.radiance.y = rgb[1];
    info.radiance.z = rgb[2];

    ext.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DISTANT_EXT;
    ext.pNext = nullptr;
    ext.direction.x = d[0];
    ext.direction.y = d[1];
    ext.direction.z = d[2];
    // The real sun subtends 0.53 degrees. Remix uses this for penumbra softness,
    // so a larger value is a softer, less real shadow - the honest default is
    // near the real number and the ini lets a user trade it away.
    ext.angularDiameterDegrees = angular;
    ext.volumetricRadianceScale = volumetric;
    info.pNext = &ext;

    int reason = 0;  // see remix.h: 1 = our max_total_lights cap, 2 = the bridge
    const remixapi_LightHandle handle = rhyCreateLight(&info, &reason);
    if (!handle) {
        if (reason == 3) {
            return nullptr;  // frame budget spent: retried next frame, not a failure
        }
        if (reason == 2) {
            // Only the bridge saying no is worth switching the sky off for the
            // session; the sun and moon are one handle each, so a cap refusal
            // means the lamp lists asked for every slot there is.
            which.apiBroken = true;
            rhyLog("%s: CreateLight refused by the bridge (bridge create failures "
                   "so far: %u), %s disabled until the next rhyremix.ini edit", name,
                   rhyCreateFailures(), name);
        } else if (which.rebuilds < 3) {
            rhyLog("%s: no slot, max_total_lights reached (live=%u): the sky light "
                   "is skipped this step, raise max_total_lights in rhyremix.ini",
                   name, rhyLiveLights());
        }
        return nullptr;
    }
    if (which.rebuilds < 3) {  // the exact numbers to compare when tuning
        int hh = 0, mm = 0;
        gameClock(nullptr, &hh, &mm);
        rhyLog("%s: CreateLight ok dir=%.4f,%.4f,%.4f radiance=%.4f,%.4f,%.4f "
               "angular=%.2f volumetric=%.2f hash=%llu (hour=%d:%02d)",
               name, d[0], d[1], d[2], rgb[0], rgb[1], rgb[2],
               ext.angularDiameterDegrees, ext.volumetricRadianceScale,
               info.hash, hh, mm);
    }
    return handle;
}

// Retire the handle if it no longer matches what we want, create it if it is
// missing, then draw it. dir/radiance are the values the live handle was built
// with, so a rebuild only happens when the body actually moved or brightened.
//
// s28: what is compared is no longer the raw geometry but the rate-limited
// published values (w.pubDir, w.rate[]). The target may jump - the game clock did,
// by up to 1372 game minutes between two samples this run - and the handle then
// walks to it: at most sun_deg_per_sec degrees of direction and at most
// sun_rel_per_sec x of radiance per second. A sun that goes below the horizon is
// ramped to zero and only retired once it is dark, so "the whole world went dark,
// then came back" cannot happen from this path any more.
//
// n01 (s29 note T7): WHEN the handle is replaced is a clock now, not a threshold.
// The old rule - rebuild once the handle is more than [sun] step_deg (1.0 deg) or
// 5% out of date - is bounded, but it fires at the worst possible moment (the
// difference has grown to the whole threshold, and the sun's disc is 0.53 deg) and
// its period follows the body's rate: ~8 s while the clock runs normally, ~80 ms
// while the direction limiter is slewing at its 12 deg/s cap. The cadence rebuilds
// every [fade] handle_cadence_ms, shortened only as far as the s28 limiters demand
// so that no rebuild carries more than handle_step_deg of direction or
// handle_rel_lum of luminance, and never below handle_cadence_min_ms.
// `stepBudgetDeg` is that direction budget (handle_step_deg, or the body's own
// step_deg when handle_step_deg=0), so [sun]/[moon] step_deg still means
// something - as a budget, not as a trigger.
static void updateDistant(DistantLight& which, float angular,
                          unsigned long long hash, float volumetric,
                          float stepBudgetDeg, const char* name, const RhyConfig& c,
                          float dt) {
    const remixapi_Interface* api = rhyRemixApi();
    if (!api || !api->DrawLightInstance) {
        return;
    }
    if (which.apiBroken) {
        return;
    }

    // --- advance the followers toward the target ---
    for (int i = 0; i < 3; ++i) {
        if (which.rate[i].relPerSec != c.sun_rel_per_sec ||
            which.rate[i].absPerSec != c.sky_abs_per_sec) {
            // Re-timed from the ini (or first use): keep the published value.
            rhyRateInit(which.rate[i], which.rate[i].cur, c.sun_rel_per_sec,
                        c.sky_abs_per_sec, 1e-4f);
        }
        rhyRateStep(which.rate[i], which.wantRgb[i], dt);
    }
    rhyDirLimit(which.pubDir, which.wantDir, c.sun_deg_per_sec * dt);

    const float pub[3] = {which.rate[0].cur, which.rate[1].cur, which.rate[2].cur};
    const float pubLum = 0.2126f * pub[0] + 0.7152f * pub[1] + 0.0722f * pub[2];
    const float wantLum = 0.2126f * which.wantRgb[0] +
                          0.7152f * which.wantRgb[1] +
                          0.0722f * which.wantRgb[2];

    if (!(pubLum > 1e-4f) && !(wantLum > 1e-4f)) {
        // Body below the horizon, or switched off: it got here by ramping down,
        // so retiring the handle now is not a step.
        if (which.handle) {
            rhyDelayDestroy(which.handle, which.tag);
            which.handle = nullptr;
            which.lum = -1.0f;
        }
        which.pubDirPrev[0] = which.pubDir[0];
        which.pubDirPrev[1] = which.pubDir[1];
        which.pubDirPrev[2] = which.pubDir[2];
        which.pubLumPrev = pubLum;
        return;
    }

    // --- what this frame's limiters actually published (n01) ---
    // The configured limits are NOT the input. At rest the sun moves 0.25 deg per
    // game minute (~0.002 deg/frame) while the limiter ALLOWS 12 deg/s: cadencing
    // off the configured limit would rebuild every body at the churn floor
    // forever, and cadencing off what was published rebuilds when the world is
    // actually moving.
    float degMoved = 0.0f;
    {
        degMoved = rhyAngleDeg(which.pubDirPrev, which.pubDir);
    }
    float relMoved = 0.0f;
    {
        const float mean = 0.5f * (pubLum + which.pubLumPrev);
        if (mean > 1e-6f) {
            relMoved = fabsf(pubLum - which.pubLumPrev) / mean;
        }
    }
    which.pubDirPrev[0] = which.pubDir[0];
    which.pubDirPrev[1] = which.pubDir[1];
    which.pubDirPrev[2] = which.pubDir[2];
    which.pubLumPrev = pubLum;

    // Re-time the cadence when an ini edit changed it under us. The counters and
    // the observed rates survive: editing rhyremix.ini must not restart the period.
    if (which.cad.maxMs != c.handle_cadence_ms ||
        which.cad.minMs != c.handle_cadence_min_ms ||
        which.cad.maxStepDeg != stepBudgetDeg ||
        which.cad.maxRelLum != c.handle_rel_lum) {
        const int rebuilds = which.cad.rebuilds;
        const int skips = which.cad.skips;
        const float degPerSec = which.cad.degPerSec;
        const float relPerSec = which.cad.relPerSec;
        const int elapsed = which.cad.elapsedMs;
        rhyCadenceInit(which.cad, stepBudgetDeg, c.handle_rel_lum,
                       c.handle_cadence_min_ms, c.handle_cadence_ms);
        which.cad.rebuilds = rebuilds;
        which.cad.skips = skips;
        which.cad.degPerSec = degPerSec;
        which.cad.relPerSec = relPerSec;
        which.cad.elapsedMs = elapsed;
    }
    rhyCadenceObserve(which.cad, degMoved, relMoved, dt);
    rhyCadenceAdvance(which.cad, dt);

    // How far the live handle is from what we would publish now. n01 uses that to
    // measure what one rebuild actually carried, and to skip a rebuild that would
    // change nothing (a pinned clock, a body that has not moved since the last
    // period): a bridge call that changes no light is churn for nothing.
    float handleDeg = 1e9f;
    float handleRel = 1e9f;
    if (which.handle) {
        handleDeg = rhyAngleDeg(which.dir, which.pubDir);
        const float hmean = 0.5f * (which.lum + pubLum);
        handleRel = hmean > 1e-6f ? fabsf(pubLum - which.lum) / hmean : 0.0f;
    }
    rhyCadenceSetGap(which.cad, handleDeg, handleRel);
    const bool sameLum =
        which.handle &&
        fabsf(pubLum - which.lum) <= 1e-6f * (pubLum + which.lum + 1.0f);

    // A missing handle is not a cadence question: the body is lit, so it needs
    // one - and the radiance it is created with has been ramping up from 0, so the
    // first create is not a step either.
    const bool due = !which.handle || rhyCadenceDue(which.cad);
    if (due) {
        if (handleDeg < 1e-4f && sameLum) {
            ++which.cad.skips;
            rhyCadenceRebuilt(which.cad);
        } else if (!rhyCanCreateLight()) {
            // No handle-change credit this frame: keep drawing the handle we have
            // and stay due. The period is NOT consumed, so the rebuild lands as
            // soon as there is credit; dropping it here would be a threshold rule
            // wearing a clock. Destroying first and being refused would leave no
            // sun at all, which is why the credit is checked before the destroy.
            if (which.handle) {
                rhyDrawLight(which.handle);
            }
            return;
        } else {
            if (which.handle) {
                rhyDelayDestroy(which.handle, which.tag);
            }
            // The hash is fixed per body, so the retired handle must be destroyed
            // before the replacement with the same hash is created.
            rhyFlushDelayedTag(which.tag);
            remixapi_LightHandle h = createDistant(which, which.pubDir, pub, angular,
                                                   hash, volumetric, name);
            if (!h) {
                which.handle = nullptr;
                return;  // createDistant already flagged apiBroken (or the cap did)
            }
            which.handle = h;
            which.dir[0] = which.pubDir[0];
            which.dir[1] = which.pubDir[1];
            which.dir[2] = which.pubDir[2];
            which.radiance[0] = pub[0];
            which.radiance[1] = pub[1];
            which.radiance[2] = pub[2];
            which.lum = pubLum;
            ++which.rebuilds;
            ++which.cad.rebuilds;
            if (handleDeg < 1e8f && handleDeg > which.maxStepDegSeen) {
                which.maxStepDegSeen = handleDeg;
            }
            rhyCadenceRebuilt(which.cad);
        }
    }

    if (!rhyDrawLight(which.handle)) {
        which.apiBroken = true;
        rhyLog("%s: DrawLightInstance rejected (draw failures so far: %u, live "
               "handles: %u), %s disabled until the next rhyremix.ini edit",
               name, rhyDrawFailures(), rhyLiveLights(), name);
        which.handle = nullptr;
    }
}

// ------------------------------------------------------------------ sky layer

// The renderer's own ambient/exposure, pushed to match the sky. This is the part
// that makes night actually dark: rtx.skyBrightness is a constant in rtx.conf, so
// a fixed 3 lights the street at midnight exactly like noon, and no amount of lamp
// tuning fixes a scene whose floor is already at 3.
//
// s28: the value pushed is no longer the value computed. rtx.skyBrightness spans
// 0.02 (night) to 1.00 (day), and this run the computed value stepped 0.00 -> 1.00
// nine times in 25 minutes - a 50x ambient step in one frame, which is finding 1
// in its purest form. Two rate limiters turn any target into a ramp, and the push
// is throttled to 10 Hz so the bridge sees a smooth sequence of small writes
// instead of one big one.
static RhyRate g_skyRate = {1.0f, 3.0f, 0.25f, 0.002f};
static RhyRate g_emissiveRate = {1.0f, 3.0f, 0.25f, 0.01f};
static bool g_skyRateInit = false;

static void applySkySettings(const RhyConfig& c, float dt, DWORD now) {
    if (!c.sky_apply) {
        return;
    }
    // Night floor plus the day ramp. The moon adds its own term so a full moon
    // lifts the ambient and a new moon does not - which is exactly what a real
    // street looks like and what no constant in rtx.conf can express.
    float sky = c.sky_night + (c.sky_day - c.sky_night) * g_daylight;
    if (c.moon_enabled && g_moonElev > 0.0f) {
        const float phase = lunarPhaseFactor(c.moon_phase);
        const float elev = g_moonElev > 60.0f ? 1.0f : g_moonElev / 60.0f;
        sky += c.sky_moon * phase * elev;
    }
    if (sky < 0.0f) {
        sky = 0.0f;
    }
    // s29: an overcast does not only take the sun's disc away (the beam is scaled by
    // rhyCloudTransmission where the radiance is computed), it scatters that light
    // into the whole sky, so the ambient gains part of it back. With cloud = 0, the
    // shipped default, this factor is exactly 1.0 and nothing about the look changes.
    sky *= rhyCloudAmbientGain(g_weather.cloud);
    // Emissives are the asset pack's lamp housings, signs and tail lights. They
    // read far stronger at night than by day, and that is true physically as
    // well as perceptually, so the gain follows the same ramp.
    float emissive = c.sky_emissive_day +
                     (c.sky_emissive_night - c.sky_emissive_day) *
                         (1.0f - g_daylight);
    if (emissive < 0.0f) {
        emissive = 0.0f;
    }

    if (!g_skyRateInit) {
        // First run starts AT the target: the plugin must not fade the world up
        // from black every time it connects to the bridge.
        rhyRateInit(g_skyRate, sky, c.sky_rel_per_sec, c.sky_abs_per_sec, 0.002f);
        rhyRateInit(g_emissiveRate, emissive, c.sky_rel_per_sec,
                    c.sky_abs_per_sec, 0.01f);
        g_skyRateInit = true;
    } else {
        if (g_skyRate.relPerSec != c.sky_rel_per_sec ||
            g_skyRate.absPerSec != c.sky_abs_per_sec) {
            rhyRateInit(g_skyRate, g_skyRate.cur, c.sky_rel_per_sec,
                        c.sky_abs_per_sec, 0.002f);
            rhyRateInit(g_emissiveRate, g_emissiveRate.cur, c.sky_rel_per_sec,
                        c.sky_abs_per_sec, 0.01f);
        }
        rhyRateStep(g_skyRate, sky, dt);
        rhyRateStep(g_emissiveRate, emissive, dt);
    }

    static DWORD s_pushTick = 0;
    if (s_pushTick && now - s_pushTick < 100) {
        return;  // 10 Hz is plenty for an ambient level; the bridge re-reads its
                 // option layer once per frame anyway
    }
    s_pushTick = now;
    // 0.001 rather than 0: with a zero sky ambient, anything not in direct view
    // of a light renders pure black and the denoiser has nothing to work with.
    rhyRtxSetFloat("rtx.skyBrightness",
                   g_skyRate.cur < 0.001f ? 0.001f : g_skyRate.cur, 0.002f);
    if (c.sky_emissive_night > 0.0f) {
        rhyRtxSetFloat("rtx.emissiveIntensity", g_emissiveRate.cur, 0.01f);
    }
}

// ------------------------------------------------------------------ per frame

void rhySunFrame() {
    const remixapi_Interface* api = rhyRemixApi();
    if (!api || !api->DrawLightInstance) {
        return;
    }
    const RhyConfig* cfg = rhyConfigGet();

    const DWORD now = GetTickCount();
    static DWORD s_lastTick = 0;
    float dt = s_lastTick ? static_cast<float>(now - s_lastTick) / 1000.0f : 0.016f;
    if (dt > 0.25f) {
        dt = 0.25f;
    }
    if (dt < 0.0f) {
        dt = 0.0f;
    }
    s_lastTick = now;

    // --- the clock: every frame, continuous (s29) ---
    //
    // s28 made the publishing per-frame and rate limited; what it left behind was a
    // target that only moved when a whole game minute ticked (0.25 degrees of sun at
    // a time, every ~2 s) and a season that alternated between the ini's day of year
    // and a garbage byte, swinging the sun by tens of degrees. Both are sampling
    // bugs, and a rate limiter cannot fix a target that is itself flickering.
    static int hour = 0, minute = 0;  // the raw bytes, for the log line
    gameClock(cfg, &hour, &minute);
    const RhyTodParams todParams = {cfg->clock_ms_per_game_minute,
                                    cfg->clock_jitter_min,
                                    static_cast<unsigned>(cfg->clock_reanchor_ms)};
    if (!g_todInit) {
        rhyTodInit(g_tod, todParams);
        rhyCalendarInit(g_cal);
        g_todInit = true;
        g_todSeed = cfg->clock_ms_per_game_minute;
    } else if (g_todSeed != cfg->clock_ms_per_game_minute) {
        // rhyremix.ini asked for a different tick length under us: keep the time,
        // restart the calibration from the new seed.
        rhyTodReseed(g_tod, todParams);
        g_todSeed = cfg->clock_ms_per_game_minute;
    }
    rhyTodUpdate(g_tod, hour, minute, static_cast<unsigned long long>(now), todParams);
    const float minuteOfDay = static_cast<float>(g_tod.minuteOfDay);

    // --- observer and calendar: once a second ---
    //
    // Latitude, longitude, season and daylight saving are a keyframe: they change on
    // the scale of a game hour at the fastest, so they are refreshed at 1 Hz. The
    // calendar bytes are re-tested every second so the "is this a date?" counter can
    // keep counting.
    static DWORD s_geoTick = 0;
    static bool s_haveGeo = false;
    static SkyObserver ob;
    static SkyBody sun, moon;
    static int month = 0, day = 0;
    if (!s_haveGeo || now - s_geoTick >= 1000) {
        s_haveGeo = true;
        s_geoTick = now;

        ob.latitudeDeg = cfg->sky_latitude;
        ob.longitudeDeg = cfg->sky_longitude;
        ob.timezoneHours = cfg->sky_timezone;
        ob.dayOfYear = cfg->sky_day_of_year;
        if (cfg->sky_track_date && cfg->clock_calendar_stable_reads > 0) {
            gameCalendarBytes(&month, &day);
            const int doy = rhyCalendarUpdate(g_cal, month, day,
                                              cfg->clock_calendar_stable_reads);
            if (doy > 0) {
                ob.dayOfYear = doy;
            }
        }
        if (ob.dayOfYear < 1 || ob.dayOfYear > 366) {
            ob.dayOfYear = 186;  // mid-July, the season GTA SA's story opens in
        }
        // US daylight saving: the second Sunday of March to the first Sunday of
        // November falls on days 67-73 and 305-311; 70..307 is the middle of both.
        // Without it a July game clock runs an hour ahead of the sun.
        if (cfg->sky_dst && ob.dayOfYear >= 70 && ob.dayOfYear <= 307) {
            ob.timezoneHours += 1.0f;
        }
    }

    // --- weather: every frame, rate limited (s29) ---
    //
    // The atmosphere is the one input that can change while the player watches
    // (rhyremix.ini is re-read live), and it feeds the sun's colour directly: an
    // unsmoothed turbidity is a sunset that changes hue between two frames. The
    // follower is also where a real CWeather read would be plugged in; there is no
    // verified offset for one, and rtx.ignoreGameDirectionalLights = True means the
    // game's own timecycle/weather blend never reaches the renderer anyway.
    if (!g_weather.seeded) {
        rhyWeatherInit(g_weather, cfg->sky_turbidity, cfg->sky_cloud);
    }
    rhyWeatherUpdate(g_weather, cfg->sky_turbidity, cfg->sky_cloud, dt,
                     cfg->weather_turbidity_per_sec, cfg->weather_cloud_per_sec);
    ob.turbidity = g_weather.turbidity;
    const float cloudT = rhyCloudTransmission(g_weather.cloud);

    // --- the bodies: every frame, at the continuous minute ---
    //
    // This is the interpolation half of s29: the sun and moon are a function of the
    // clock, so evaluating them at a continuous minute turns the 0.25 degree
    // staircase that the whole-minute clock produced into the curve it samples.
    solarPositionF(ob, minuteOfDay, &sun);
    lunarPositionF(ob, minuteOfDay, cfg->moon_phase, &moon);
    g_sunElev = sun.elevationDeg;
    g_moonElev = moon.elevationDeg;

    g_daylight = skyDaylightFactor(g_sunElev, cfg->sky_dusk_deg,
                                   cfg->sky_dawn_deg);
    g_lampsOn = lampsShouldBeOn(g_sunElev, cfg->lamp_dusk_deg);

    // --- sun target ---
    if (cfg->sun_enabled && !g_sun.apiBroken) {
        float d[3] = {0.0f, 0.0f, -1.0f};
        if (cfg->sun_model) {
            // A light points where its photons travel: away from the body.
            d[0] = -sun.toX;
            d[1] = -sun.toY;
            d[2] = -sun.toZ;
        } else {
            legacySunDirection(*cfg, minuteOfDay, d);
        }
        float rgb[3];
        beamRadiance(sun, cfg->sun_radiance * cloudT, cfg->sun_color, ob.turbidity,
                     rgb);
        g_sun.wantDir[0] = d[0];
        g_sun.wantDir[1] = d[1];
        g_sun.wantDir[2] = d[2];
        g_sun.wantRgb[0] = rgb[0];
        g_sun.wantRgb[1] = rgb[1];
        g_sun.wantRgb[2] = rgb[2];
    } else {
        g_sun.wantRgb[0] = g_sun.wantRgb[1] = g_sun.wantRgb[2] = 0.0f;
    }

    // --- moon target ---
    if (cfg->moon_enabled && !g_moon.apiBroken) {
        const float d[3] = {-moon.toX, -moon.toY, -moon.toZ};
        // Phase is why a crescent does not light the street like a full moon.
        // The old code had no phase at all: it was permanently full.
        const float phase = lunarPhaseFactor(cfg->moon_phase);
        float rgb[3];
        beamRadiance(moon, cfg->moon_radiance * phase * cloudT, cfg->moon_color,
                     ob.turbidity, rgb);
        g_moon.wantDir[0] = d[0];
        g_moon.wantDir[1] = d[1];
        g_moon.wantDir[2] = d[2];
        g_moon.wantRgb[0] = rgb[0];
        g_moon.wantRgb[1] = rgb[1];
        g_moon.wantRgb[2] = rgb[2];
    } else {
        g_moon.wantRgb[0] = g_moon.wantRgb[1] = g_moon.wantRgb[2] = 0.0f;
    }

    // First target: publish it as-is. A plugin that has just connected to the
    // bridge must not fade the world up from black.
    if (!g_sun.seeded) {
        g_sun.seeded = true;
        g_sun.pubDir[0] = g_sun.wantDir[0];
        g_sun.pubDir[1] = g_sun.wantDir[1];
        g_sun.pubDir[2] = g_sun.wantDir[2];
        for (int i = 0; i < 3; ++i) {
            rhyRateInit(g_sun.rate[i], g_sun.wantRgb[i], cfg->sun_rel_per_sec,
                        cfg->sky_abs_per_sec, 1e-4f);
        }
    }
    if (!g_moon.seeded) {
        g_moon.seeded = true;
        g_moon.pubDir[0] = g_moon.wantDir[0];
        g_moon.pubDir[1] = g_moon.wantDir[1];
        g_moon.pubDir[2] = g_moon.wantDir[2];
        for (int i = 0; i < 3; ++i) {
            rhyRateInit(g_moon.rate[i], g_moon.wantRgb[i], cfg->sun_rel_per_sec,
                        cfg->sky_abs_per_sec, 1e-4f);
        }
    }

    // --- publish: every frame, rate-limited ---
    // n01: what is passed is the DIRECTION BUDGET for one rebuild, not a rebuild
    // trigger. handle_step_deg=0 keeps the old per-body numbers as the budget, so
    // an existing rhyremix.ini loads unchanged.
    const float sunBudget = cfg->handle_step_deg > 0.0f ? cfg->handle_step_deg
                                                        : cfg->sun_step_deg;
    const float moonBudget = cfg->handle_step_deg > 0.0f ? cfg->handle_step_deg
                                                         : cfg->moon_step_deg;
    updateDistant(g_sun, cfg->sun_angular, cfg->sun_hash, cfg->sun_volumetric,
                  sunBudget, "sun", *cfg, dt);
    updateDistant(g_moon, cfg->moon_angular, cfg->moon_hash, 0.0f,
                  moonBudget, "moon", *cfg, dt);
    applySkySettings(*cfg, dt, now);

    static DWORD s_logTick = 0;
    if (now - s_logTick >= 10000) {
        s_logTick = now;
        rhyLog("sky: sun elev=%+.1f az=%.1f airMass=%.2f T=%.3f/%.3f/%.3f "
               "daylight=%.2f | moon elev=%+.1f az=%.1f phase=%.2f | lamps=%s "
               "(clock=%d:%02d tod=%.2f min ms/min=%.0f doy=%d month=%d day=%d) | "
               "tod: ticks=%u torn=%u jitter=%u reanchor=%u wraps=%u bad=%u | "
               "cal: stable=%d accepted=%u rejected=%u | cloud=%.2f | "
               "published: sun %.2f,%.2f,%.2f sky=%.3f emissive=%.3f rebuilds=%d/%d "
               "cad=%d/%dms step<=%.2f/%.2f skip=%d/%d "
               "| rtx: set=%u unknown=%u badval=%u",
               g_sunElev, sun.azimuthDeg, sun.airMass, sun.transmit[0],
               sun.transmit[1], sun.transmit[2], g_daylight, g_moonElev,
               moon.azimuthDeg, lunarPhaseFactor(cfg->moon_phase),
               g_lampsOn ? "on" : "off", hour, minute, g_tod.minuteOfDay,
               g_tod.msPerGameMinute, ob.dayOfYear, month, day,
               g_tod.ticks, g_tod.tornReads, g_tod.jitterIgnored, g_tod.reanchors,
               g_tod.wraps, g_tod.badSamples,
               g_cal.stable, g_cal.accepted, g_cal.rejected, g_weather.cloud,
               g_sun.rate[0].cur, g_sun.rate[1].cur, g_sun.rate[2].cur,
               g_skyRate.cur, g_emissiveRate.cur, g_sun.rebuilds, g_moon.rebuilds,
               g_sun.cad.intervalMs, g_moon.cad.intervalMs,
               g_sun.maxStepDegSeen, g_moon.maxStepDegSeen,
               g_sun.cad.skips, g_moon.cad.skips,
               rhyRtxSets(), rhyRtxUnknown(), rhyRtxBadValue());
    }
}

// ------------------------------------------------------------------ queries

float rhySunElevationDeg() { return g_sunElev; }
float rhyDaylightFactor() { return g_daylight; }
bool rhyLampsOn() { return g_lampsOn; }
