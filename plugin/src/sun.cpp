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

// Game clock: hour at 0xB70153, minute at 0xB70152 (the same source the lamp
// and traffic-light managers use).
static void gameClock(int* hour, int* minute) {
    *hour = static_cast<int>(*reinterpret_cast<const uint8_t*>(0xB70153));
    *minute = static_cast<int>(*reinterpret_cast<const uint8_t*>(0xB70152));
}

// SARemix_Real_Sun.lua turns the sun 360 degrees per 1440 game minutes
// (getTargetRotationFromGameTime) and hangs that off a game object.  The same
// orbit is computed here directly, with no object, no model id and no USD
// asset: phi = 0 -> light points straight down (sun overhead), phi = 180 ->
// light points straight up (sun below the horizon, i.e. midnight).
//
// Game axes are used unchanged, because the lamp lights already land on the
// lamps with unmodified game coordinates: x = east, y = north, z = up.
// sun_spin is the Lua mod's "Sun Y position" slider (basePosition), sun_tilt
// and sun_roll are its "Sun X Rotaion" / "Sun Z Rotaion" sliders.
static void sunDirection(const RhyConfig& c, float* out) {
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

    // Roll around the east-west axis: lean the orbit north/south.
    const float y2 = y * cosf(roll) - z * sinf(roll);
    const float z2 = y * sinf(roll) + z * cosf(roll);
    y = y2;
    z = z2;

    const float n = sqrtf(x * x + y * y + z * z);
    if (n > 1e-6f) {  // the API requires a normalised direction
        out[0] = x / n;
        out[1] = y / n;
        out[2] = z / n;
    }
}

// -dir.z is the sine of the body's altitude above the horizon.  Radiance fades
// with it, which produces dawn and dusk without an hour table: the Lua mod
// needs three hard hour windows (5-20 sun, 12-16 bright sun, 17-7 moon)
// precisely because it can only switch whole objects on and off.
static float bodyRadiance(float elev, float base, float fadePow,
                         float minElevation) {
    if (elev <= minElevation) {
        return 0.0f;
    }
    float k = elev > 1.0f ? 1.0f : elev;
    if (fadePow == 1.0f) {
        return base * k;
    }
    return base * powf(k, fadePow);
}

static remixapi_LightHandle createDistant(DistantLight& which,
                                          const RhyConfig& c,
                                          const float* d, float radiance,
                                          float angular,
                                          const float* color,
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
    info.radiance.x = color[0] * radiance;
    info.radiance.y = color[1] * radiance;
    info.radiance.z = color[2] * radiance;

    ext.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DISTANT_EXT;
    ext.pNext = nullptr;
    ext.direction.x = d[0];
    ext.direction.y = d[1];
    ext.direction.z = d[2];
    // Disc size: small = hard shadows, large = soft penumbrae.  This is the
    // first-class version of the mod's "Use Disc Light as Sun" checkbox, which
    // swaps model id 15889 (distant) for 15890 (disc).
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
        rhyLog("%s: CreateLight ok dir=%.4f,%.4f,%.4f radiance=%.3f,%.3f,%.3f "
               "angular=%.2f volumetric=%.2f hash=%llu (hour=%d:%02d)",
               name, d[0], d[1], d[2], info.radiance.x, info.radiance.y,
               info.radiance.z, ext.angularDiameterDegrees,
               ext.volumetricRadianceScale, info.hash,
               *reinterpret_cast<const uint8_t*>(0xB70153),
               *reinterpret_cast<const uint8_t*>(0xB70152));
    }
    return handle;
}

// Retire the handle if it no longer matches what we want, create it if it is
// missing, then draw it.  dir/radiance are the values the live handle was
// built with, so a rebuild only happens when the body actually moved or
// brightened by more than the configured step.
static void updateDistant(DistantLight& which, const RhyConfig& c,
                          const float* d, float radiance, float angular,
                          const float* color, unsigned long long hash,
                          float volumetric, float stepDeg,
                          const char* name) {
    const remixapi_Interface* api = rhyRemixApi();
    if (!api || !api->DrawLightInstance) {
        return;
    }
    if (which.apiBroken) {
        return;
    }

    if (!(radiance > 0.0f)) {  // body below the horizon: no light at all
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
        fabsf(radiance - which.radiance) > 0.05f * (radiance + which.radiance);

    if (!which.handle || movedDeg > stepDeg || dimChanged) {
        if (which.handle) {
            rhyDelayDestroy(which.handle, which.tag);
        }
        // The hash is fixed per body, so the retired handle must be destroyed
        // before the replacement with the same hash is created.
        rhyFlushDelayedTag(which.tag);
        remixapi_LightHandle h =
            createDistant(which, c, d, radiance, angular, color, hash,
                          volumetric, name);
        if (!h) {
            which.handle = nullptr;
            return;  // createDistant already flagged apiBroken
        }
        which.handle = h;
        which.dir[0] = d[0];
        which.dir[1] = d[1];
        which.dir[2] = d[2];
        which.radiance = radiance;
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

void rhySunFrame() {
    const remixapi_Interface* api = rhyRemixApi();
    if (!api || !api->DrawLightInstance) {
        return;
    }
    const RhyConfig* cfg = rhyConfigGet();

    float d[3] = {g_sun.dir[0], g_sun.dir[1], g_sun.dir[2]};
    sunDirection(*cfg, d);

    if (cfg->sun_enabled && !g_sun.apiBroken) {
        const float radiance =
            bodyRadiance(-d[2], cfg->sun_radiance, cfg->sun_fade_pow,
                         cfg->sun_min_elevation);
        updateDistant(g_sun, *cfg, d, radiance, cfg->sun_angular, cfg->sun_color,
                      cfg->sun_hash, cfg->sun_volumetric, cfg->sun_step_deg,
                      "sun");
    } else if (g_sun.handle) {
        rhyDelayDestroy(g_sun.handle, kTagSun);
        g_sun.handle = nullptr;
    }

    // The moon is the anti-sun: the Lua mod spins a second object (model id
    // 15891) on the same orbit, on for hour >= 17 or hour < 7.  Negating the
    // sun direction puts it exactly opposite, and its altitude is -sun altitude,
    // so the same fade code turns it on at night and off by day.
    if (cfg->moon_enabled && !g_moon.apiBroken) {
        const float md[3] = {-d[0], -d[1], -d[2]};
        // md is the moon's light direction, so its altitude is -md[2], exactly
        // like the sun's is -d[2].  Passing md[2] here would light the scene
        // by day and leave the moon dark all night.
        const float radiance =
            bodyRadiance(-md[2], cfg->moon_radiance, cfg->moon_fade_pow,
                         cfg->moon_min_elevation);
        updateDistant(g_moon, *cfg, md, radiance, cfg->moon_angular,
                      cfg->moon_color, cfg->moon_hash, 0.0f, cfg->moon_step_deg,
                      "moon");
    } else if (g_moon.handle) {
        rhyDelayDestroy(g_moon.handle, kTagMoon);
        g_moon.handle = nullptr;
    }

    static DWORD g_lastTick = 0;
    const DWORD now = GetTickCount();
    if (now - g_lastTick >= 10000) {
        g_lastTick = now;
        rhyLog("sky: sun elev=%.2f rad=%.2f rebuilds=%d broken=%d | moon elev=%.2f "
               "rad=%.2f rebuilds=%d broken=%d (hour=%d:%02d)",
               -g_sun.dir[2], g_sun.radiance, g_sun.rebuilds, g_sun.apiBroken,
               -g_moon.dir[2], g_moon.radiance, g_moon.rebuilds, g_moon.apiBroken,
               *reinterpret_cast<const uint8_t*>(0xB70153),
               *reinterpret_cast<const uint8_t*>(0xB70152));
    }
}
