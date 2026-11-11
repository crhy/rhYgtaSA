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
#include "solar.h"

static const float kDeg = 0.017453292519430f;  // pi / 180

// One live distant light: the direction and radiance it was created with, so
// the next frame can tell whether the handle still matches what we want.
// The Remix runtime fixes direction and radiance at CreateLight time, so
// "moving the sun" means retiring the handle and creating a new one.
struct DistantLight {
    remixapi_LightHandle handle;
    float dir[3];
    float radiance;
    int rebuilds;
    bool apiBroken;
    int tag;  // delayed.h tag namespace: see kTagSun / kTagMoon
};

static DistantLight g_sun = {nullptr, {0.0f, 0.0f, -1.0f}, -1.0f, 0, false, kTagSun};
static DistantLight g_moon = {nullptr, {0.0f, 0.0f, -1.0f}, -1.0f, 0, false, kTagMoon};

// The last sky state, for the periodic log line and for the lamp modules, which
// need "is it night?" and must not disagree with what the sun is doing.
static float g_sunElev = -90.0f;
static float g_moonElev = -90.0f;
static float g_daylight = 0.0f;
static bool g_lampsOn = true;

// ------------------------------------------------------------------ clock

// Game clock: hour at 0xB70153, minute at 0xB70152 (the same source the lamp
// and traffic-light managers use).
static void gameClock(int* hour, int* minute) {
    uint8_t* hr = reinterpret_cast<uint8_t*>(0xB70153);
    uint8_t* mn = reinterpret_cast<uint8_t*>(0xB70152);
    const RhyConfig* c = rhyConfigGet();
    if (c && c->sun_force_hour >= 0) {
        // Pinning the clock is the only practical way to compare noon against
        // sunset in one sitting: SA runs an in-game minute every 1.6 real
        // seconds, so 12:00 to 20:00 is twenty minutes of waiting. Written once
        // per change rather than every frame, so the clock still ticks.
        if (*hr != static_cast<uint8_t>(c->sun_force_hour)) {
            *hr = static_cast<uint8_t>(c->sun_force_hour);
        }
        if (c->sun_force_minute >= 0 &&
            *mn != static_cast<uint8_t>(c->sun_force_minute)) {
            *mn = static_cast<uint8_t>(c->sun_force_minute);
        }
    }
    *hour = static_cast<int>(*hr);
    *minute = static_cast<int>(*mn);
}

// GTA SA's calendar. The hour and minute bytes are trusted above; the day and
// month were guessed at +1 and +5 and measured 0/0 in a live run, so those
// offsets are wrong and day_of_year from the ini is what actually drives the
// season. The raw bytes are dumped once so the next pass can find the real
// offsets instead of guessing again.
static int gameDayOfYear(int* monthOut, int* dayOut) {
    static bool s_dumped = false;
    if (!s_dumped) {
        s_dumped = true;
        const uint8_t* p = reinterpret_cast<const uint8_t*>(0xB70150);
        rhyLog("clock: raw bytes 0xB70150..0xB7015F = %d %d %d %d %d %d %d %d | "
               "| %d %d %d %d %d %d %d %d  (hour@53=%d minute@52=%d; the guessed "
               "day@54/month@58 read 0, so day_of_year from the ini is used)",
               p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7],
               p[8], p[9], p[10], p[11], p[12], p[13], p[14], p[15], p[3], p[2]);
    }
    const int day = static_cast<int>(*reinterpret_cast<const uint8_t*>(0xB70154));
    const int month = static_cast<int>(*reinterpret_cast<const uint8_t*>(0xB70158));
    if (monthOut) {
        *monthOut = month;
    }
    if (dayOut) {
        *dayOut = day;
    }
    if (month < 1 || month > 12 || day < 1 || day > 31) {
        return 0;  // not a calendar: caller falls back to the ini
    }
    static const int kCum[] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
    return kCum[month - 1] + day;
}

// ------------------------------------------------------------------ legacy orbit

// The original model: 360 degrees per 1440 game minutes on a fixed plane. Kept
// because it is the only way to A/B "is the astronomical model the reason this
// looks different?", and because a user may want a sun that ignores the season.
static void legacySunDirection(const RhyConfig& c, float* out) {
    int hour = 0, minute = 0;
    gameClock(&hour, &minute);
    const float ang = (static_cast<float>(hour) * 60.0f +
                       static_cast<float>(minute)) * 0.25f;
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
        gameClock(&hh, &mm);
        rhyLog("%s: CreateLight ok dir=%.4f,%.4f,%.4f radiance=%.4f,%.4f,%.4f "
               "angular=%.2f volumetric=%.2f hash=%llu (hour=%d:%02d)",
               name, d[0], d[1], d[2], rgb[0], rgb[1], rgb[2],
               ext.angularDiameterDegrees, ext.volumetricRadianceScale,
               info.hash, hh, mm);
    }
    return handle;
}

// Retire the handle if it no longer matches what we want, create it if it is
// missing, then draw it. dir/radiance are the values the live handle was
// built with, so a rebuild only happens when the body actually moved or
// brightened by more than the configured step.
static void updateDistant(DistantLight& which, const float* d,
                          const float* rgb, float angular,
                          unsigned long long hash, float volumetric,
                          float stepDeg, const char* name) {
    const remixapi_Interface* api = rhyRemixApi();
    if (!api || !api->DrawLightInstance) {
        return;
    }
    if (which.apiBroken) {
        return;
    }

    const float lum = 0.2126f * rgb[0] + 0.7152f * rgb[1] + 0.0722f * rgb[2];
    if (!(lum > 0.0f)) {  // body below the horizon, or fully extinguished
        if (which.handle) {
            rhyDelayDestroy(which.handle, which.tag);
            which.handle = nullptr;
            which.radiance = -1.0f;
        }
        return;
    }

    const float dot = which.dir[0] * d[0] + which.dir[1] * d[1] +
                      which.dir[2] * d[2];
    const float clampd = dot < -1.0f ? -1.0f : (dot > 1.0f ? 1.0f : dot);
    const float movedDeg = acosf(clampd) / kDeg;
    const bool dimChanged =
        fabsf(lum - which.radiance) > 0.05f * (lum + which.radiance);

    if (!which.handle || movedDeg > stepDeg || dimChanged) {
        if (which.handle) {
            rhyDelayDestroy(which.handle, which.tag);
        }
        // The hash is fixed per body, so the retired handle must be destroyed
        // before the replacement with the same hash is created.
        rhyFlushDelayedTag(which.tag);
        remixapi_LightHandle h = createDistant(which, d, rgb, angular, hash,
                                               volumetric, name);
        if (!h) {
            which.handle = nullptr;
            return;  // createDistant already flagged apiBroken
        }
        which.handle = h;
        which.dir[0] = d[0];
        which.dir[1] = d[1];
        which.dir[2] = d[2];
        which.radiance = lum;
        ++which.rebuilds;
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

// Push the renderer's own ambient/exposure to match the sky. This is the part
// that makes night actually dark: rtx.skyBrightness is a constant in rtx.conf,
// so a fixed 3 lights the street at midnight exactly like noon, and no amount of
// lamp tuning fixes a scene whose floor is already at 3.
static void applySkySettings(const RhyConfig& c) {
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
    // 0.001 rather than 0: with a zero sky ambient, anything not in direct view
    // of a light renders pure black and the denoiser has nothing to work with.
    rhyRtxSetFloat("rtx.skyBrightness", sky < 0.001f ? 0.001f : sky, 0.01f);

    // Emissives are the asset pack's lamp housings, signs and tail lights. They
    // read far stronger at night than by day, and that is true physically as
    // well as perceptually, so the gain follows the same ramp.
    if (c.sky_emissive_night > 0.0f) {
        const float e = c.sky_emissive_day +
                        (c.sky_emissive_night - c.sky_emissive_day) *
                            (1.0f - g_daylight);
        rhyRtxSetFloat("rtx.emissiveIntensity", e, 0.05f);
    }
}

// ------------------------------------------------------------------ per frame

void rhySunFrame() {
    const remixapi_Interface* api = rhyRemixApi();
    if (!api || !api->DrawLightInstance) {
        return;
    }
    const RhyConfig* cfg = rhyConfigGet();

    // Recomputing the sky every frame is pointless: the game clock advances a
    // minute every 1.6 real seconds at normal speed, and a distant light can only
    // move in steps of step_deg anyway. Once a second is plenty and keeps the
    // SetConfigVariable traffic off the frame budget.
    static DWORD s_lastTick = 0;
    const DWORD now = GetTickCount();
    static bool s_first = true;
    if (!s_first && now - s_lastTick < 1000) {
        if (g_sun.handle) {
            rhyDrawLight(g_sun.handle);
        }
        if (g_moon.handle) {
            rhyDrawLight(g_moon.handle);
        }
        return;
    }
    s_first = false;
    s_lastTick = now;

    // --- geometry ---
    SkyObserver ob;
    ob.latitudeDeg = cfg->sky_latitude;
    ob.longitudeDeg = cfg->sky_longitude;
    ob.timezoneHours = cfg->sky_timezone;
    ob.turbidity = cfg->sky_turbidity;
    ob.dayOfYear = cfg->sky_day_of_year;
    if (cfg->sky_track_date) {
        int month = 0, day = 0;
        const int doy = gameDayOfYear(&month, &day);
        if (doy > 0) {
            ob.dayOfYear = doy;
        }
    }
    if (ob.dayOfYear < 1 || ob.dayOfYear > 366) {
        ob.dayOfYear = 186;  // mid-July, the season GTA SA's story opens in
    }

    int hour = 0, minute = 0;
    gameClock(&hour, &minute);

    SkyBody sun, moon;
    solarPosition(ob, hour, minute, &sun);
    lunarPosition(ob, hour, minute, cfg->moon_phase, &moon);
    g_sunElev = sun.elevationDeg;
    g_moonElev = moon.elevationDeg;

    g_daylight = skyDaylightFactor(g_sunElev, cfg->sky_dusk_deg, cfg->sky_dawn_deg);
    g_lampsOn = lampsShouldBeOn(g_sunElev, cfg->lamp_dusk_deg, cfg->lamp_dawn_deg);

    // --- sun ---
    if (cfg->sun_enabled && !g_sun.apiBroken) {
        float d[3];
        if (cfg->sun_model) {
            // A light points where its photons travel: away from the body.
            d[0] = -sun.toX;
            d[1] = -sun.toY;
            d[2] = -sun.toZ;
        } else {
            legacySunDirection(*cfg, d);
        }
        float rgb[3];
        beamRadiance(sun, cfg->sun_radiance, cfg->sun_color, ob.turbidity, rgb);
        updateDistant(g_sun, d, rgb, cfg->sun_angular, cfg->sun_hash,
                      cfg->sun_volumetric, cfg->sun_step_deg, "sun");
    } else if (g_sun.handle) {
        rhyDelayDestroy(g_sun.handle, kTagSun);
        g_sun.handle = nullptr;
    }

    // --- moon ---
    if (cfg->moon_enabled && !g_moon.apiBroken) {
        const float d[3] = {-moon.toX, -moon.toY, -moon.toZ};
        // Phase is why a crescent does not light the street like a full moon.
        // The old code had no phase at all: it was permanently full.
        const float phase = lunarPhaseFactor(cfg->moon_phase);
        float rgb[3];
        beamRadiance(moon, cfg->moon_radiance * phase, cfg->moon_color,
                     ob.turbidity, rgb);
        updateDistant(g_moon, d, rgb, cfg->moon_angular, cfg->moon_hash, 0.0f,
                      cfg->moon_step_deg, "moon");
    } else if (g_moon.handle) {
        rhyDelayDestroy(g_moon.handle, kTagMoon);
        g_moon.handle = nullptr;
    }

    // --- the renderer's own ambient, exposure and emissive gain ---
    applySkySettings(*cfg);

    static DWORD s_logTick = 0;
    if (now - s_logTick >= 10000) {
        s_logTick = now;
        int month = 0, day = 0;
        gameDayOfYear(&month, &day);
        rhyLog("sky: sun elev=%+.1f az=%.1f airMass=%.2f T=%.3f/%.3f/%.3f "
               "daylight=%.2f | moon elev=%+.1f az=%.1f phase=%.2f | lamps=%s "
               "(hour=%d:%02d doy=%d month=%d day=%d) | rtx: set=%u unknown=%u "
               "badval=%u",
               g_sunElev, sun.azimuthDeg, sun.airMass, sun.transmit[0],
               sun.transmit[1], sun.transmit[2], g_daylight, g_moonElev,
               moon.azimuthDeg, lunarPhaseFactor(cfg->moon_phase),
               g_lampsOn ? "on" : "off", hour, minute, ob.dayOfYear, month, day,
               rhyRtxSets(), rhyRtxUnknown(), rhyRtxBadValue());
    }
}

// ------------------------------------------------------------------ queries

float rhySunElevationDeg() { return g_sunElev; }
float rhyDaylightFactor() { return g_daylight; }
bool rhyLampsOn() { return g_lampsOn; }
