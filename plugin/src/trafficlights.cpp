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
#include "sun.h"  // rhyLampsOn: signals and street lamps switch on the same evening
#include "lamps.h"
#include "signalheads.h"
#include "signalhook.h"

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

// --- source=game: one Remix light per signal HEAD the game itself lights ---
//
// Keyed by rhyHeadId(entity, 2dfx index) instead of a list index, because the
// heads come and go with what the renderer draws rather than from a file.
struct HeadLight {
    remixapi_LightHandle handle;
    int phase;      // the phase the handle was built with
    DWORD seen;     // last tick this head registered a corona
};
static std::map<uint64_t, HeadLight> g_heads;
static RhySignalHead g_rec[kRhySignalHeadCap];
static unsigned g_recN = 0;          // records in g_rec this frame
static unsigned g_headSwaps = 0;     // recreations caused by a colour change
static unsigned g_headExpired = 0;   // lights retired by the 2 s corona timeout
static bool g_hookTried = false;     // install attempted at least once

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

// Install the corona hook, and say plainly what happened. If it cannot be
// installed the module keeps working from the list file: a signal lit at the wrong
// place in an approximate colour beats no signal at all, and the log says which
// one the user is looking at.
//
// s19: this is only half of the kill switch. rhySignalHookInstall() itself
// refuses unless [trafficlights] source=game AND the bytes at 0x49DD4C, at
// 0x6FC580 and at the caller's `add esp,0x54` all match what the stub assumes,
// so the hook cannot be installed by accident from any call path. What this
// function adds is the other half: the fallback to source=list, logged.
static void tryHook() {
    if (g_hookTried && rhySignalHookInstalled()) {
        return;
    }
    int why = kSignalHookOk;
    if (rhySignalHookInstall(&why)) {
        g_hookTried = true;
        rhyLog("trafficlights: source=game, hooking the corona the game registers "
               "for each signal head (position and colour come from the game)");
    } else {
        g_hookTried = true;
        rhyLog("trafficlights: source=game requested but the hook was not "
               "installed: %s. Falling back to source=list; editing rhyremix.ini "
               "retries the install", rhySignalHookReasonText(why));
    }
}

void rhyTrafficInitOnce() {
    if (g_inited) {
        return;
    }
    g_inited = true;
    const RhyConfig* cfg = rhyConfigGet();
    if (cfg->traffic_source == 1) {
        tryHook();
    } else {
        // The kill switch rhY uses to bisect a crash: with source=list nothing
        // is ever written to the game's code, and the byte sanity check is not
        // even run (rhySignalPatchCheck returns kRhyPatchSource before it looks
        // at the exe). This is the one line that decides whether the hook can
        // exist for this session.
        g_hookTried = false;
        rhyLog("trafficlights: source=list, no code hook: one light per pole at "
               "the list position plus headz, colour from the local cycle");
    }
    // The list is loaded either way: it is the fallback, and reading 1500 lines
    // once at startup costs nothing worth arguing about.
    loadList();
}

// ---------------------------------------------------------------- selection

// Player position and the game clock come from game.cpp, so the traffic lights
// switch on exactly when the street lamps do.

// Night window, shared with the street lamps. A real traffic signal is lit
// around the clock, so night_only=0 is the physically correct setting and costs
// handles; night_only=1 is the performance setting, and it should switch on the
// same twilight the street lamps use rather than on a wall-clock hour, or the
// signals go dark while the street is still lit.
static bool isNight(int hour, const RhyConfig& c) {
    if (c.lamp_mode == 1) {
        return rhyLampsOn();
    }
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
//
// The colours are the passbands of real signal lenses, integrated across the
// visible spectrum (see lamps.h), not three hand-picked triples. That matters
// more than it sounds: sampling a single "red" wavelength like 620 nm gives
// orange, because the eye's response at 620 nm still carries a lot of green,
// whereas a real red lens has cut the green out entirely. Integrating the band
// is what produces the saturated red and the distinctly blue-green traffic green.
static const float* phaseColor(int phase) {
    static float rgb[3][3] = {{1, 1, 1}, {1, 1, 1}, {1, 1, 1}};
    static bool done = false;
    if (!done) {
        done = true;
        for (int p = 0; p < 3; ++p) {
            rhySignalLensRgb(p, rgb[p]);
        }
    }
    return rgb[phase >= 0 && phase <= 2 ? phase : 0];
}

static remixapi_LightHandle createLight(int index, int phase,
                                        const RhyConfig& c) {
    const remixapi_Interface* api = rhyRemixApi();
    if (!api || !api->CreateLight) {
        return nullptr;
    }
    const TrafficLight& L = g_lights[index];
    const float* col = phaseColor(phase);

    static remixapi_LightInfo info;
    static remixapi_LightInfoSphereEXT sphere;

    // A signal module lens is 200 mm across, so the emitting sphere is 0.1 game
    // units in radius; the shipped value of 6 was a six metre ball, wider than
    // the intersection it is supposed to be a point on. Radiance is scaled by
    // (radius_ref/radius)^2 so the signal keeps its brightness at the new size.
    RhyFixture fix;
    rhySignalFixture(&fix);
    const float radius = c.fixture_size ? fix.radius * c.fixture_scale
                                        : c.traffic_radius;
    const float rad = rhyRadianceForFlux(c.traffic_radiance, radius,
                                         c.traffic_radius_ref) * c.radiance_scale;

    std::memset(&info, 0, sizeof info);
    info.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
    // Distinct hash per (index, phase): the Remix runtime keys lights by hash.
    // With light_hash_base=0 the lights are anonymous instead.
    info.hash = c.light_hash_base +
                (static_cast<uint64_t>(index) << 2) +
                static_cast<uint64_t>(phase & 3);
    info.radiance.x = col[0] * rad;
    info.radiance.y = col[1] * rad;
    info.radiance.z = col[2] * rad;

    std::memset(&sphere, 0, sizeof sphere);
    sphere.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_SPHERE_EXT;
    sphere.pNext = nullptr;
    sphere.position.x = L.x;
    sphere.position.y = L.y;
    sphere.position.z = L.z + L.headz;
    sphere.radius = radius;
    // Deliberately unshaped. A real signal module is strongly directional - the
    // visor and refractor throw a narrow beam down the approach - but the lamp
    // list carries only x, y, z and a head offset, with no heading, so there is
    // nothing to aim a cone with. A wrong cone is worse than none: it would put
    // a dark side on every signal seen from the cross street. Getting this right
    // means adding a heading column to rhyremix_traffic.txt from the IPL's
    // rotation, which is the honest fix and is noted in docs/FINDINGS.md.
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

// ---------------------------------------------------------------- source=game

// Tag for the delayed-destroy queue. Two different heads can fold onto the same
// tag; the only consequence is that a flush retires the other head's handle a few
// frames earlier than it would have been, and that handle was already off the
// draw list (see delayed.h).
static int headTag(uint64_t id) {
    return kTagSignalHead + static_cast<int>(id % 400000ull);
}

// One signal head: the lens position the game used for its corona, the colour the
// game chose mapped onto the measured lens spectrum, and the real lens radius.
//
// This is the whole point of s01. The list mode put one light at the pole origin
// plus a headz guess, so on a mast-arm signal the light hung several metres from
// every head, and it coloured the light from a local cycle that had nothing to do
// with the intersection. Here the position comes from the game's own 2DFX entry
// rotated through the entity matrix, and the colour from the game's own aspect
// state, so a mast-arm head is lit where it is and in the colour it shows.
static remixapi_LightHandle createHeadLight(uint64_t id, int phase,
                                            const float* pos,
                                            const RhyConfig& c) {
    const remixapi_Interface* api = rhyRemixApi();
    if (!api || !api->CreateLight) {
        return nullptr;
    }
    float col[3];
    rhySignalLensRgb(phase, col);

    RhyFixture fix;
    rhySignalFixture(&fix);
    const float radius = c.fixture_size ? fix.radius * c.fixture_scale
                                        : c.traffic_radius;
    const float rad = rhyRadianceForFlux(c.traffic_radiance, radius,
                                         c.traffic_radius_ref) * c.radiance_scale;

    static remixapi_LightInfo info;
    static remixapi_LightInfoSphereEXT sphere;

    std::memset(&info, 0, sizeof info);
    info.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
    info.hash = rhyHeadLightHash(id, phase, c.light_hash_base);
    info.radiance.x = col[0] * rad;
    info.radiance.y = col[1] * rad;
    info.radiance.z = col[2] * rad;

    std::memset(&sphere, 0, sizeof sphere);
    sphere.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_SPHERE_EXT;
    sphere.pNext = nullptr;
    sphere.position.x = pos[0];
    sphere.position.y = pos[1];
    sphere.position.z = pos[2];
    sphere.radius = radius;
    // Unshaped, as in list mode, and for the same reason plus one more: the 2DFX
    // entry that gave us this position carries no direction at all (0x59C890
    // rotates a position offset and nothing else), so there is no normal here to
    // aim a cone with either.
    sphere.shaping_hasvalue = 0;
    sphere.volumetricRadianceScale = 0.0f;
    info.pNext = &sphere;

    int reason = 0;  // see remix.h: 1 = our max_total_lights cap, 2 = the bridge
    const remixapi_LightHandle handle = rhyCreateLight(&info, &reason);
    if (!handle) {
        if (reason == 2) {
            g_apiBroken = true;
            g_brokenTick = GetTickCount();
            rhyLog("trafficlights: CreateLight refused by the bridge for head "
                   "%016llx phase %d (bridge create failures so far: %u), traffic "
                   "lights off until the next rhyremix.ini edit",
                   static_cast<unsigned long long>(id), phase, rhyCreateFailures());
            return nullptr;
        }
        if (++g_budgetStop <= 3) {
            rhyLog("trafficlights: no slot for head %016llx phase %d, "
                   "max_total_lights reached (live=%u, refusals so far: %u)",
                   static_cast<unsigned long long>(id), phase, rhyLiveLights(),
                   g_budgetStop);
        }
        return nullptr;
    }
    static int headLogs = 0;
    if (headLogs < 3) {
        ++headLogs;
        rhyLog("trafficlights: head light ok id=%016llx phase=%d hash=%llu "
               "pos=%.2f,%.2f,%.2f rgb_game=%d,%d,%d radiance=%.2f,%.2f,%.2f "
               "radius=%.2f",
               static_cast<unsigned long long>(id), phase, info.hash, pos[0], pos[1],
               pos[2], col[0] > 0.5f ? 1 : 0, col[1] > 0.5f ? 1 : 0,
               col[2] > 0.5f ? 1 : 0, info.radiance.x, info.radiance.y,
               info.radiance.z, sphere.radius);
    }
    return handle;
}

static void destroyAllHeads() {
    for (std::map<uint64_t, HeadLight>::iterator it = g_heads.begin();
         it != g_heads.end(); ++it) {
        rhyDelayDestroy(it->second.handle, headTag(it->first));
    }
    g_heads.clear();
}

// Re-diff the live head lights against what the game registered this frame.
// Same shape as resync() above, with two differences that come from the source:
// a head the game stopped registering is not "out of range", it is "this aspect
// is off / this signal left the renderer", and it expires on a timeout; and a
// colour change is the game's own, so it is not limited to phase_near - the
// game only registers the lit head in the first place, so the heads that change
// colour are the ones the player is looking at.
static void resyncHeads(const RhyConfig& c, const float* pp) {
    const DWORD now = GetTickCount();

    // 1. every head the game registered this frame is still alive.
    for (unsigned i = 0; i < g_recN; ++i) {
        const uint64_t id = rhyHeadId(g_rec[i].entity, g_rec[i].index);
        std::map<uint64_t, HeadLight>::iterator it = g_heads.find(id);
        if (it != g_heads.end()) {
            it->second.seen = g_rec[i].tick;
        }
    }

    // 2. expire the ones that stopped registering.
    for (std::map<uint64_t, HeadLight>::iterator it = g_heads.begin();
         it != g_heads.end();) {
        if (rhyHeadStale(it->second.seen, now,
                         static_cast<uint32_t>(c.traffic_head_ttl_ms))) {
            rhyDelayDestroy(it->second.handle, headTag(it->first));
            g_heads.erase(it++);
            ++g_headExpired;
        } else {
            ++it;
        }
    }

    // 3. ask the pure selector which heads should have a light right now.
    if (g_recN == 0) {
        // The game registered no signal corona on this particular frame (the
        // renderer skipped, or every signal is behind the camera). That is not
        // "every signal went out": re-diffing against an empty set would
        // destroy every head light and rebuild it 0.5 s later, which is a signal
        // that blinks. The expiry above still runs, so a signal that really is
        // gone loses its light after head_ttl_ms.
        return;
    }
    std::vector<uint64_t> active;
    active.reserve(g_heads.size());
    for (std::map<uint64_t, HeadLight>::iterator it = g_heads.begin();
         it != g_heads.end(); ++it) {
        active.push_back(it->first);
    }
    static RhyHeadWant want[kRhySignalHeadCap];
    const unsigned n = rhyChooseHeads(
        g_rec, g_recN, pp, static_cast<float>(c.traffic_max_distance),
        static_cast<float>(c.traffic_exit_distance),
        static_cast<unsigned>(c.traffic_max_lights),
        static_cast<unsigned>(c.sticky_extra),
        active.empty() ? nullptr : &active[0],
        static_cast<unsigned>(active.size()), want, kRhySignalHeadCap);

    std::map<uint64_t, int> wantPhase;
    for (unsigned i = 0; i < n; ++i) {
        wantPhase[want[i].id] = want[i].phase;  // -1 = "keep the colour it has"
    }

    // 4. drop the heads that fell out of the set or changed aspect.
    for (std::map<uint64_t, HeadLight>::iterator it = g_heads.begin();
         it != g_heads.end();) {
        std::map<uint64_t, int>::iterator wp = wantPhase.find(it->first);
        const bool stale = (wp == wantPhase.end()) ||
                           (wp->second >= 0 && wp->second != it->second.phase);
        if (stale) {
            if (wp != wantPhase.end() && wp->second >= 0) {
                ++g_headSwaps;
            }
            rhyDelayDestroy(it->second.handle, headTag(it->first));
            g_heads.erase(it++);
        } else {
            ++it;
        }
    }

    // 5. create the rest. Destroy-before-create for the same head: the hash is
    // derived from the id, so a pending destroy of the old handle would take the
    // new light with it (see delayed.h).
    for (unsigned i = 0; i < n; ++i) {
        const uint64_t id = want[i].id;
        if (g_heads.find(id) != g_heads.end()) {
            continue;
        }
        const int phase = want[i].phase;
        if (phase < 0) {
            continue;  // the game's corona colour was not recognisable; do not
                       // invent one, wait for a frame where it is
        }
        float pos[3];
        rhyHeadLightPos(&g_rec[want[i].src],
                        c.traffic_head_normal ? g_rec[want[i].src].normal : nullptr,
                        c.traffic_head_forward, pos);
        rhyFlushDelayedTag(headTag(id));
        const remixapi_LightHandle h = createHeadLight(id, phase, pos, c);
        if (h) {
            HeadLight hl;
            hl.handle = h;
            hl.phase = phase;
            hl.seen = g_rec[want[i].src].tick;
            g_heads[id] = hl;
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

    // One ini revision, two retries: a hook install that failed (so a user who
    // sets source=game by hand does not need a restart) and a module that gave up
    // on the Remix API. Doing this on the revision rather than per frame is what
    // keeps the log from repeating the same failure 60 times a second.
    const unsigned rev = rhyConfigRevision();
    if (rev != g_cfgRev) {
        g_cfgRev = rev;
        if (cfg->traffic_source == 1 && !rhySignalHookInstalled()) {
            tryHook();
        }
        if (g_apiBroken) {
            g_apiBroken = false;
            g_brokenTick = GetTickCount();
            rhyLog("trafficlights: rhyremix.ini edited, retrying the Remix API");
        }
    }
    const bool gameMode = (cfg->traffic_source == 1) && rhySignalHookInstalled();

    // Drain the hook every frame, even when this module is off or the user has
    // switched to source=list: a full array would otherwise count "dropped"
    // forever and the log line would be unreadable.
    g_recN = rhySignalHookInstalled()
                 ? rhySignalHeadsTake(g_rec, kRhySignalHeadCap)
                 : 0;
    if (!gameMode) {
        g_recN = 0;
    }

    if (!cfg->traffic_enabled || cfg->traffic_mode == 0 ||
        (!gameMode && g_lights.empty())) {
        destroyAll();
        destroyAllHeads();
        return;
    }

    float pp[3];
    if (!rhyPlayerPos(pp)) {
        destroyAll();
        destroyAllHeads();
        return;
    }

    ++g_frame;

    if ((g_frame % cfg->traffic_resync_frames) == 1) {
        g_night = !cfg->traffic_night_only || isNight(rhyGameHour(), *cfg);
        if (!g_night) {
            destroyAll();
            destroyAllHeads();
        } else if (gameMode) {
            resyncHeads(*cfg, pp);
        } else {
            // source=list (asked for, or fallen back to): any head lights from a
            // previous game-mode run have to go, or they would keep being drawn
            // with nobody maintaining them.
            destroyAllHeads();
            resync(*cfg, pp, static_cast<double>(GetTickCount()) / 1000.0);
        }
    }

    // A rejected call switches this module off for the session; the revision
    // check at the top of this function is what turns it back on.
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
    unsigned hd_drawn = 0;
    for (std::map<uint64_t, HeadLight>::iterator it = g_heads.begin();
         it != g_heads.end(); ++it) {
        if (!rhyDrawLight(it->second.handle)) {
            g_apiBroken = true;
            g_brokenTick = GetTickCount();
            rhyLog("trafficlights: DrawLightInstance rejected for head %016llx "
                   "(draw failures so far: %u, live handles: %u), traffic lights "
                   "off until the next rhyremix.ini edit",
                   static_cast<unsigned long long>(it->first), rhyDrawFailures(),
                   rhyLiveLights());
            break;
        }
        ++hd_drawn;
    }

    DWORD now = GetTickCount();
    static DWORD g_lastCountTick = 0;
    if (now - g_lastCountTick >= 10000) {  // active count once per 10 s
        g_lastCountTick = now;
        if (gameMode) {
            int green = 0, amber = 0, red = 0;
            for (std::map<uint64_t, HeadLight>::iterator it = g_heads.begin();
                 it != g_heads.end(); ++it) {
                if (it->second.phase == 0) {
                    ++green;
                } else if (it->second.phase == 1) {
                    ++amber;
                } else {
                    ++red;
                }
            }
            rhyLog("trafficlights[source=game]: %u heads lit, %u drawn this frame "
                   "(green=%d amber=%d red=%d, %u coronas registered this frame, "
                   "colour_swaps=%u expired=%u) | remix: live=%u create_fail=%u "
                   "draw_fail=%u budget_stop=%u | hook: calls=%u records=%u "
                   "dropped=%u bad=%u reentrant=%u",
                   static_cast<unsigned>(g_heads.size()), hd_drawn, green, amber,
                   red, g_recN, g_headSwaps, g_headExpired, rhyLiveLights(),
                   rhyCreateFailures(), rhyDrawFailures(), g_budgetStop,
                   rhySignalHookCalls(), rhySignalHookRecords(),
                   rhySignalHookDropped(), rhySignalHookBad(),
                   rhySignalHookReentrant());
        } else {
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
            rhyLog("trafficlights[%s]: %u active, %u drawn this frame (green=%d "
                   "amber=%d red=%d, %u awaiting destroy, %u in list, hour=%d, "
                   "phase_swaps=%u) | remix: live=%u create_fail=%u draw_fail=%u "
                   "budget_stop=%u",
                   gameMode ? "list" : "list, source=game requested but the hook "
                                   "is not installed",
                   static_cast<unsigned>(g_active.size()), tl_drawn, green, amber,
                   red, rhyDelayedPending(), static_cast<unsigned>(g_lights.size()),
                   rhyGameHour(), g_phaseSwap, rhyLiveLights(), rhyCreateFailures(),
                   rhyDrawFailures(), g_budgetStop);
        }
    }
}
