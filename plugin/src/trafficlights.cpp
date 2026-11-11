#include "trafficlights.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "remix.h"
#include "config.h"
#include "delayed.h"
#include "game.h"
#include "log.h"

// One traffic light from rhyremix_traffic.txt: "x y z headz".
struct TrafficLight {
    float x;
    float y;
    float z;
    float headz;  // metres above the instance origin where the lamp head sits
};

static std::vector<TrafficLight> g_lights;
static std::map<int, remixapi_LightHandle> g_active;  // list index -> handle
static std::map<int, int> g_phase;                     // list index -> phase baked into the handle
static bool g_inited = false;
static int g_frame = 0;
static bool g_night = false;
static bool g_apiBroken = false;  // a BRIDGE call failed: stop touching the API
static DWORD g_brokenTick = 0;    // when it broke, for the post-mortem log line
static unsigned g_budgetStop = 0;  // creates our OWN cap refused (not a failure)
static unsigned g_phaseSwap = 0;   // recreations caused by a phase change
static unsigned g_cfgRev = 0;     // ini revision we are following

// ---------------------------------------------------------------- pure logic

// Cycle layout inside one period: 45% green, 10% amber, 45% red. The index
// offsets the phase so neighbouring intersections are not in sync.
int rhyTrafficPhase(int index, double nowSeconds, int mode, int periodS) {
    if (mode == 2) {  // fixed green
        return 0;
    }
    if (periodS < 1) {
        periodS = 1;
    }
    double period = static_cast<double>(periodS);
    double t = nowSeconds + static_cast<double>(index) * (period / 7.0);
    t = std::fmod(t, period);
    if (t < 0.0) {
        t += period;
    }
    if (t < 0.45 * period) {
        return 0;  // green
    }
    if (t < 0.55 * period) {
        return 1;  // amber
    }
    return 2;  // red
}

bool rhyTrafficParseLine(const char* line, float* x, float* y, float* z,
                         float* headz) {
    while (*line == ' ' || *line == '\t') {
        ++line;
    }
    if (*line == '#' || *line == '\r' || *line == '\n' || *line == '\0') {
        return false;
    }
    float a = 0.0f, b = 0.0f, c = 0.0f, d = 0.0f;
    int matched = std::sscanf(line, "%f %f %f %f", &a, &b, &c, &d);
    if (matched != 4) {
        return false;
    }
    *x = a;
    *y = b;
    *z = c;
    *headz = d;
    return true;
}

// ---------------------------------------------------------------- loading

static void loadList() {
    const RhyConfig* cfg = rhyConfigGet();
    FILE* f = std::fopen(cfg->trafficPath.c_str(), "rb");
    if (!f) {
        rhyLog("trafficlights: cannot open %s", cfg->trafficPath.c_str());
        return;
    }
    char line[256];
    while (std::fgets(line, sizeof line, f)) {
        float x = 0.0f, y = 0.0f, z = 0.0f, headz = 0.0f;
        if (!rhyTrafficParseLine(line, &x, &y, &z, &headz)) {
            continue;
        }
        TrafficLight light = {x, y, z, headz};
        g_lights.push_back(light);
    }
    std::fclose(f);
    rhyLog("trafficlights: loaded %u lights from %s",
           static_cast<unsigned>(g_lights.size()), cfg->trafficPath.c_str());
    for (size_t i = 0; i < g_lights.size() && i < 3; ++i) {
        rhyLog("trafficlights: #%d at %.1f,%.1f,%.1f head z=%.1f",
               static_cast<int>(i), g_lights[i].x, g_lights[i].y,
               g_lights[i].z, g_lights[i].z + g_lights[i].headz);
    }
}

void rhyTrafficInitOnce() {
    if (g_inited) {
        return;
    }
    g_inited = true;
    loadList();
}

// ---------------------------------------------------------------- selection

// Player position and the game clock come from game.cpp, so the traffic lights
// switch on exactly when the street lamps do.

// Night window on_hour..off_hour, wrapping midnight (19..6 by default).
static bool isNight(int hour, const RhyConfig& c) {
    if (c.on_hour == c.off_hour) {
        return true;
    }
    if (c.on_hour < c.off_hour) {
        return hour >= c.on_hour && hour <= c.off_hour;
    }
    return hour >= c.on_hour || hour <= c.off_hour;
}

// Nearest-first list of list indices that should have a light.  Same two-radius
// hysteresis as the street lamps (see streetlights.cpp): a NEW light must be
// inside traffic_max_distance, an ALREADY ACTIVE one survives to
// traffic_exit_distance, and the set may grow to traffic_max_lights + sticky_extra
// (the streetlights knob is reused here on purpose: one ini value, two modules)
// before the farthest ones are dropped. Without it, a light on the boundary is
// created and destroyed on alternate passes: a traffic light that blinks.
static std::vector<int> chooseLights(const RhyConfig& c, const float* pp) {
    struct Cand {
        float d2;
        int i;
        bool sticky;
    };
    std::vector<Cand> cand;
    const float maxD = static_cast<float>(c.traffic_max_distance);
    const float exitD = static_cast<float>(c.traffic_exit_distance);
    const size_t cap = static_cast<size_t>(c.traffic_max_lights);
    const size_t grow = cap + static_cast<size_t>(c.sticky_extra);

    for (size_t i = 0; i < g_lights.size(); ++i) {
        const TrafficLight& L = g_lights[i];
        const float dx = L.x - pp[0];
        const float dy = L.y - pp[1];
        const float d2 = dx * dx + dy * dy;
        if (maxD > 0.0f && d2 > exitD * exitD) {  // the WIDER bound: see above
            continue;
        }
        const Cand e = {d2, static_cast<int>(i),
                        g_active.find(static_cast<int>(i)) != g_active.end()};
        cand.push_back(e);
    }
    std::sort(cand.begin(), cand.end(),
              [](const Cand& a, const Cand& b) { return a.d2 < b.d2; });

    std::vector<int> chosen;
    chosen.reserve(grow);
    for (size_t k = 0; k < cand.size(); ++k) {
        if (cand[k].sticky && chosen.size() < grow) {
            chosen.push_back(cand[k].i);
        }
    }
    for (size_t k = 0; k < cand.size(); ++k) {
        if (!cand[k].sticky && chosen.size() < cap) {
            chosen.push_back(cand[k].i);
        }
    }
    return chosen;
}

// ---------------------------------------------------------------- create / destroy

// Radiance is baked at CreateLight time, so a phase change means destroying
// the light and creating a new one with a different hash.
static const float kPhaseColor[3][3] = {
    {0.15f, 1.00f, 0.25f},  // green
    {1.00f, 0.55f, 0.10f},  // amber
    {1.00f, 0.12f, 0.10f},  // red
};

static remixapi_LightHandle createLight(int index, int phase,
                                        const RhyConfig& c) {
    const remixapi_Interface* api = rhyRemixApi();
    if (!api || !api->CreateLight) {
        return nullptr;
    }
    const TrafficLight& L = g_lights[index];
    const float* col = kPhaseColor[phase >= 0 && phase <= 2 ? phase : 0];

    static remixapi_LightInfo info;
    static remixapi_LightInfoSphereEXT sphere;

    std::memset(&info, 0, sizeof info);
    info.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
    // Distinct hash per (index, phase): the Remix runtime keys lights by hash.
    // With light_hash_base=0 the lights are anonymous instead.
    info.hash = c.light_hash_base +
                (static_cast<uint64_t>(index) << 2) +
                static_cast<uint64_t>(phase & 3);
    info.radiance.x = col[0] * c.traffic_radiance * c.radiance_scale;
    info.radiance.y = col[1] * c.traffic_radiance * c.radiance_scale;
    info.radiance.z = col[2] * c.traffic_radiance * c.radiance_scale;

    std::memset(&sphere, 0, sizeof sphere);
    sphere.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_SPHERE_EXT;
    sphere.pNext = nullptr;
    sphere.position.x = L.x;
    sphere.position.y = L.y;
    sphere.position.z = L.z + L.headz;
    sphere.radius = c.traffic_radius;
    sphere.shaping_hasvalue = 0;
    sphere.volumetricRadianceScale = 0.0f;
    info.pNext = &sphere;

    int reason = 0;  // see remix.h: 1 = our max_total_lights cap, 2 = the bridge
    const remixapi_LightHandle handle = rhyCreateLight(&info, &reason);
    if (!handle) {
        if (reason == 2) {
            g_apiBroken = true;
            g_brokenTick = GetTickCount();
            rhyLog("trafficlights: CreateLight refused by the bridge #%d phase %d "
                   "(bridge create failures so far: %u), traffic lights off until "
                   "the next rhyremix.ini edit",
                   index, phase, rhyCreateFailures());
            return nullptr;
        }
        if (++g_budgetStop <= 3) {
            rhyLog("trafficlights: no slot for #%d phase %d, max_total_lights reached "
                   "(live=%u, refusals so far: %u)", index, phase, rhyLiveLights(),
                   g_budgetStop);
        }
        return nullptr;
    }
    // Log the exact numbers of the first few lights once, to compare against
    // probe_radius / probe_radiance when tuning.
    static int createdLogs = 0;
    if (createdLogs < 3) {
        ++createdLogs;
        rhyLog("trafficlights: CreateLight ok #%d phase %d hash=%llu pos=%.1f,%.1f,%.1f "
               "radiance=%.2f,%.2f,%.2f radius=%.2f",
               index, phase, info.hash, sphere.position.x, sphere.position.y,
               sphere.position.z, info.radiance.x, info.radiance.y,
               info.radiance.z, sphere.radius);
    }
    return handle;
}

static void destroyAll() {
    for (std::map<int, remixapi_LightHandle>::iterator it = g_active.begin();
         it != g_active.end(); ++it) {
        rhyDelayDestroy(it->second, kTagTraffic + it->first);
    }
    g_active.clear();
    g_phase.clear();
}

// Re-diff the active set against the chosen set, recreating lights whose phase
// changed - but only the ones close enough for the colour to be readable.
// Radiance is baked at CreateLight time, so a phase change costs a destroy plus
// a create; doing that for every light in a 250 m radius is a traffic light that
// visibly blinks across the whole district, for a colour change nobody can see
// at that distance. phase_near is where the two stop being worth trading.
static void resync(const RhyConfig& c, const float* pp, double nowSeconds) {
    std::vector<int> chosen = chooseLights(c, pp);

    const float nearD = static_cast<float>(c.traffic_phase_near);
    const float near2 = nearD * nearD;

    std::map<int, bool> keep;
    std::map<int, int> wantPhase;  // -1 = "any phase will do": keep what we have
    for (size_t i = 0; i < chosen.size(); ++i) {
        keep[chosen[i]] = true;
        const TrafficLight& L = g_lights[chosen[i]];
        const float dx = L.x - pp[0];
        const float dy = L.y - pp[1];
        wantPhase[chosen[i]] =
            (dx * dx + dy * dy <= near2)
                ? rhyTrafficPhase(chosen[i], nowSeconds, c.traffic_mode,
                                  c.traffic_period)
                : -1;
    }

    // Drop lights that are no longer chosen, or whose phase changed.
    for (std::map<int, remixapi_LightHandle>::iterator it = g_active.begin();
         it != g_active.end();) {
        const int index = it->first;
        std::map<int, bool>::iterator k = keep.find(index);
        std::map<int, int>::iterator wp = wantPhase.find(index);
        std::map<int, int>::iterator cp = g_phase.find(index);
        const bool stale = (k == keep.end()) ||
                           (wp == wantPhase.end()) ||
                           (cp == g_phase.end()) ||
                           (wp->second >= 0 && cp->second != wp->second);
        if (stale) {
            // Count the phase-driven recreations HERE: g_phase is erased below,
            // so the create loop can no longer tell "this index used to be lit
            // in another colour" from "this index was never lit".
            if (cp != g_phase.end() && wp != wantPhase.end() &&
                wp->second >= 0 && cp->second != wp->second) {
                ++g_phaseSwap;
            }
            rhyDelayDestroy(it->second, kTagTraffic + index);
            g_active.erase(it++);
            g_phase.erase(index);
        } else {
            ++it;
        }
    }

    // Create newly chosen lights (and recreated ones after a phase change).
    for (size_t i = 0; i < chosen.size(); ++i) {
        const int index = chosen[i];
        if (g_active.find(index) != g_active.end()) {
            continue;
        }
        int phase = wantPhase[index];
        if (phase < 0) {
            // Too far away to care about the colour, but it has no light yet,
            // so give it the phase it would have had anyway.
            phase = rhyTrafficPhase(index, nowSeconds, c.traffic_mode,
                                    c.traffic_period);
        }
        rhyFlushDelayedTag(kTagTraffic + index);  // destroy-before-create: same hash, see delayed.h
        remixapi_LightHandle h = createLight(index, phase, c);
        if (h) {
            g_active[index] = h;
            g_phase[index] = phase;
        }
    }
}

// ---------------------------------------------------------------- per frame

void rhyTrafficFrame() {
    const remixapi_Interface* api = rhyRemixApi();
    if (!api || !api->DrawLightInstance) {
        return;
    }
    const RhyConfig* cfg = rhyConfigGet();
    if (!cfg->traffic_enabled || cfg->traffic_mode == 0 || g_lights.empty()) {
        destroyAll();
        return;
    }

    float pp[3];
    if (!rhyPlayerPos(pp)) {
        destroyAll();
        return;
    }

    ++g_frame;
    if ((g_frame % cfg->traffic_resync_frames) == 1) {
        g_night = !cfg->traffic_night_only || isNight(rhyGameHour(), *cfg);
        if (!g_night) {
            destroyAll();
        } else {
            resync(*cfg, pp, static_cast<double>(GetTickCount()) / 1000.0);
        }
    }

    // A rejected call switches this module off for the session; a fresh ini
    // revision is the signal to try again.
    if (g_apiBroken && g_cfgRev != rhyConfigRevision()) {
        g_apiBroken = false;
        g_cfgRev = rhyConfigRevision();
        rhyLog("trafficlights: rhyremix.ini edited, retrying the Remix API");
    }

    if (!g_night || g_apiBroken) {
        return;
    }

    unsigned tl_drawn = 0;
    for (std::map<int, remixapi_LightHandle>::iterator it = g_active.begin();
         it != g_active.end(); ++it) {
        if (!rhyDrawLight(it->second)) {
            g_apiBroken = true;
            g_brokenTick = GetTickCount();
            rhyLog("trafficlights: DrawLightInstance rejected for #%d (draw "
                   "failures so far: %u, live handles: %u), traffic lights off "
                   "until the next rhyremix.ini edit",
                   it->first, rhyDrawFailures(), rhyLiveLights());
            break;
        }
        ++tl_drawn;
    }

    DWORD now = GetTickCount();
    static DWORD g_lastCountTick = 0;
    if (now - g_lastCountTick >= 10000) {  // active count once per 10 s
        g_lastCountTick = now;
        int green = 0, amber = 0, red = 0;
        for (std::map<int, int>::iterator it = g_phase.begin();
             it != g_phase.end(); ++it) {
            if (it->second == 0) {
                ++green;
            } else if (it->second == 1) {
                ++amber;
            } else {
                ++red;
            }
        }
        rhyLog("trafficlights: %u active, %u drawn this frame (green=%d amber=%d "
               "red=%d, %u awaiting destroy, %u in list, hour=%d, phase_swaps=%u) "
               "| remix: live=%u create_fail=%u draw_fail=%u budget_stop=%u",
               static_cast<unsigned>(g_active.size()), tl_drawn, green, amber, red,
               rhyDelayedPending(), static_cast<unsigned>(g_lights.size()),
               rhyGameHour(), g_phaseSwap, rhyLiveLights(), rhyCreateFailures(),
               rhyDrawFailures(), g_budgetStop);
    }
}
