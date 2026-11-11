#include "probe.h"

#include <windows.h>

#include <algorithm>
#include <cstring>

#include "remix.h"
#include "config.h"
#include "delayed.h"
#include "game.h"
#include "log.h"

// One light, drawn every frame, whose only purpose is to answer the question
// the street lamps cannot answer themselves: which radius / radiance /
// coordinate space actually produces light in the Remix world.  Everything
// about it is a knob, and probe_sweep walks one knob through powers of ten so a
// single run brackets the answer instead of guessing.
static remixapi_LightHandle g_probe = nullptr;
static DWORD g_bornTick = 0;    // when the current handle was created
static int g_step = 0;          // current sweep step: value x 10^g_step
static DWORD g_stepTick = 0;
static unsigned long long g_created = 0;  // how many probe lights we made
static int g_drawErrs = 0;      // DrawLightInstance failures, logged a few times

// The sweep multiplier for the current step (10^g_step, g_step < 9 by config).
static float sweepMul() {
    float m = 1.0f;
    for (int i = 0; i < g_step; ++i) {
        m *= 10.0f;
    }
    return m;
}

static remixapi_LightHandle makeProbe(const RhyConfig& c, float x, float y,
                                      float z, float radius, float radiance) {
    const remixapi_Interface* api = rhyRemixApi();
    if (!api || !api->CreateLight) {
        return nullptr;
    }

    static remixapi_LightInfo info;
    static remixapi_LightInfoSphereEXT sphere;
    static remixapi_LightInfoDiskEXT disk;
    static remixapi_LightInfoCylinderEXT cyl;

    std::memset(&info, 0, sizeof info);
    info.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
    // LightInfo::hash is an opaque uint64_t with no documented constraint, but
    // it is never zero in any light that has worked here, and the probe was the
    // only light we ever created with hash=0 -- and the only one that never
    // showed up.  Treat non-zero as a hypothesis to test, not a documented rule:
    // probe_hash is configurable so a run can change it and nothing else.
    //
    // The hash is bumped per creation as well: recreating the probe while the
    // previous handle is still awaiting its delayed destroy would otherwise
    // re-use a hash the server may still be holding.
    info.hash = c.probe_hash + g_created;
    info.radiance.x = radiance;
    info.radiance.y = radiance;
    info.radiance.z = radiance;

    // Which light type the bridge and the Remix server actually accept is not
    // known: the distant light (sun/moon) is the only type proven to work here.
    // probe_shape picks the chain, one run per value, nothing else changed:
    //   0 bare LightInfo, no pNext at all
    //   1 sphere (what the lamp lights use)
    //   2 disk, facing down, like a lamp head
    //   3 cylinder, vertical, like a lamp post
    // Whichever one shows up names the type to use for every lamp light.
    std::memset(&sphere, 0, sizeof sphere);
    sphere.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_SPHERE_EXT;
    sphere.pNext = nullptr;
    sphere.position.x = x;
    sphere.position.y = y;
    sphere.position.z = z;
    sphere.radius = radius;
    // probe_cone=0 leaves the sphere unshaped on purpose: a street lamp's cone
    // is a second reason it could be invisible, and the probe exists to remove
    // every reason but one.
    if (c.probe_cone) {
        sphere.shaping_hasvalue = 1;
        sphere.shaping_value.direction.x = 0.0f;
        sphere.shaping_value.direction.y = 0.0f;
        sphere.shaping_value.direction.z = -1.0f;
        sphere.shaping_value.coneAngleDegrees = c.cone_angle;
        sphere.shaping_value.coneSoftness = c.cone_softness;
        sphere.shaping_value.focusExponent = c.focus_exponent;
    } else {
        sphere.shaping_hasvalue = 0;
    }
    sphere.volumetricRadianceScale = 0.0f;

    std::memset(&disk, 0, sizeof disk);
    disk.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DISK_EXT;
    disk.pNext = nullptr;
    disk.position.x = x;
    disk.position.y = y;
    disk.position.z = z;
    disk.xAxis.x = 1.0f; disk.xAxis.y = 0.0f; disk.xAxis.z = 0.0f;
    disk.xRadius = radius;
    disk.yAxis.x = 0.0f; disk.yAxis.y = 1.0f; disk.yAxis.z = 0.0f;
    disk.yRadius = radius;
    disk.direction.x = 0.0f; disk.direction.y = 0.0f; disk.direction.z = -1.0f;
    disk.shaping_hasvalue = 0;
    disk.volumetricRadianceScale = 0.0f;

    std::memset(&cyl, 0, sizeof cyl);
    cyl.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_CYLINDER_EXT;
    cyl.pNext = nullptr;
    cyl.position.x = x;
    cyl.position.y = y;
    cyl.position.z = z;
    cyl.radius = radius;
    cyl.axis.x = 0.0f; cyl.axis.y = 0.0f; cyl.axis.z = 1.0f;
    cyl.axisLength = radius;
    cyl.volumetricRadianceScale = 0.0f;

    void* chain = nullptr;
    if (c.probe_shape == 1) {
        chain = &sphere;
    } else if (c.probe_shape == 2) {
        chain = &disk;
    } else if (c.probe_shape == 3) {
        chain = &cyl;
    }
    info.pNext = chain;

    int reason = 0;  // see remix.h: 1 = our max_total_lights cap, 2 = the bridge
    const remixapi_LightHandle handle = rhyCreateLight(&info, &reason);
    ++g_created;
    if (!handle) {
        if (reason == 2) {
            rhyLog("probe: CreateLight refused by the bridge (bridge create failures "
                   "so far: %u), no probe light - read rtx-remix/logs/bridge64.log",
                   rhyCreateFailures());
        } else {
            rhyLog("probe: no slot, max_total_lights reached (live=%u): the probe "
                   "needs a free slot, raise max_total_lights in rhyremix.ini",
                   rhyLiveLights());
        }
        return nullptr;
    }
    rhyLog("probe: created #%llu hash=%llu shape=%d pos=%.2f,%.2f,%.2f radius=%.3f "
           "radiance=%.3f scale=%.4f shaping=%d (check bridge64.log for a "
           "matching 'Remix API call failed')",
           g_created, info.hash, c.probe_shape, x, y, z, radius, radiance, c.probe_scale,
           c.probe_shape == 1 ? sphere.shaping_hasvalue : 0);
    return handle;
}

void rhyProbeFrame() {
    const RhyConfig* c = rhyConfigGet();
    const remixapi_Interface* api = rhyRemixApi();
    if (!api || !api->DrawLightInstance) {
        return;
    }

    if (!c->probe) {
        if (g_probe) {
            rhyDelayDestroy(g_probe, kTagProbe);
            g_probe = nullptr;
        }
        return;
    }

    const DWORD now = GetTickCount();

    // Sweep: walk one knob through powers of ten.  The first step at which the
    // light becomes visible is the order of magnitude to copy into the lamp
    // config; the step where it blows out is the one above it.
    if (c->probe_sweep && c->probe_sweep_steps > 1 &&
        static_cast<DWORD>(now - g_stepTick) >=
            static_cast<DWORD>(c->probe_step_ms)) {
        g_stepTick = now;
        g_step = (g_step + 1) % c->probe_sweep_steps;
        rhyLog("probe: sweep step %d (x%.0f on %s)", g_step, sweepMul(),
               c->probe_sweep_axis ? "radius" : "radiance");
    }

    // Position: probe_scale is the coordinate-space question.  1 means the API
    // wants game units like the lamp lists; 0.01 (rtx.sceneScale) means it wants
    // Remix metres.  Following the player removes the "it is somewhere else"
    // variable entirely, so the only thing left to explain is whether it shows.
    float x = c->probe_x;
    float y = c->probe_y;
    float z = c->probe_z;
    if (c->probe_follow) {
        float pp[3];
        if (!rhyPlayerPos(pp)) {
            if (g_probe) {
                rhyDelayDestroy(g_probe, kTagProbe);
                g_probe = nullptr;
            }
            return;
        }
        x = pp[0];
        y = pp[1];
        z = pp[2] + c->probe_dz;
    }
    x *= c->probe_scale;
    y *= c->probe_scale;
    z *= c->probe_scale;

    float radius = c->probe_radius;
    float radiance = c->probe_radiance;
    if (c->probe_sweep) {
        if (c->probe_sweep_axis == 0) {
            radiance *= sweepMul();
        } else {
            radius *= sweepMul();
        }
    }

    // Recreate on a timer.  The bridge client cannot tell us whether the server
    // accepted a create - it returns a handle either way - so the only safe
    // way to recover from a rejected create, or to pick up a live ini edit,
    // is to build the light again periodically.
    const bool stale = (g_probe == nullptr) ||
                       (static_cast<DWORD>(now - g_bornTick) >=
                        static_cast<DWORD>(c->probe_refresh_ms));
    if (stale) {
        if (g_probe) {
            rhyDelayDestroy(g_probe, kTagProbe);
        }
        // The probe keeps one hash across refreshes, so the retired handle has to
        // be gone before the new one with the same hash is created.
        rhyFlushDelayedTag(kTagProbe);
        g_probe = makeProbe(*c, x, y, z, radius, radiance);
        g_bornTick = now;
    }

    if (!g_probe) {
        return;
    }

    if (!rhyDrawLight(g_probe)) {
        if (g_drawErrs < 3) {
            rhyLog("probe: DrawLightInstance failed (%d so far, live handles: "
                   "%u): the Remix server does not know this handle, so the create "
                   "was rejected server-side - read rtx-remix/logs/bridge64.log, not "
                   "this log", ++g_drawErrs, rhyLiveLights());
        }
    }
}
