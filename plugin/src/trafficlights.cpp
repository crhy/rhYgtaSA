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
#include "smooth.h"  // s28: fades and rate limits - lighting must never step
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
static std::vector<float> g_xy;  // x,y pairs of g_lights, for rhySelectLights

// s28: a signal light is (list index, phase), and its brightness is a fade
// envelope, not a constant. Keying the live set by BOTH index and phase is what
// makes a colour change a cross-fade instead of a jump: the old colour's handle
// ramps down while the new colour's ramps up, and because the hash carries the
// phase the two can exist at the same time.
struct SignalState {
    remixapi_LightHandle handle;
    RhyFade fade;
};
static int signalKey(int index, int phase) { return index * 4 + (phase & 3); }

static std::map<int, SignalState> g_active;  // signalKey -> live light
static std::map<int, float> g_want;          // signalKey -> target gain (0..1)
static std::vector<int> g_wantOrder;         // nearest first

static bool g_inited = false;
static int g_frame = 0;
static bool g_night = false;
static bool g_apiBroken = false;  // a BRIDGE call failed: stop touching the API
static DWORD g_brokenTick = 0;    // when it broke, for the post-mortem log line
static unsigned g_budgetStop = 0;  // creates our OWN cap refused (not a failure)
static unsigned g_phaseSwap = 0;   // recreations caused by a phase change
static unsigned g_cfgRev = 0;     // ini revision we are following
static unsigned g_fadeSteps = 0;  // re-creates that moved a gain one step
static unsigned g_retired = 0;    // lights destroyed after fading to dark
static unsigned g_playerGone = 0;  // frames the player ped pointer was unreadable

// --- source=game: one Remix light per signal HEAD the game itself lights ---
//
// Keyed by rhyHeadId(entity, 2dfx index) instead of a list index, because the
// heads come and go with what the renderer draws rather than from a file.
struct HeadSeen {
    DWORD seen;   // last tick this head registered a corona
    int phase;    // the phase the game last showed for it
};
static std::map<uint64_t, HeadSeen> g_heads;

// The handles themselves are keyed by (head, phase) for the same reason the list
// lights are keyed by (index, phase): a colour change needs both handles alive
// for the length of the cross-fade.
struct HeadKey {
    uint64_t id;
    int phase;
    bool operator<(const HeadKey& o) const {
        if (id != o.id) {
            return id < o.id;
        }
        return phase < o.phase;
    }
};
struct HeadState {
    remixapi_LightHandle handle;
    RhyFade fade;
    float pos[3];  // where the lens was when this light was created: a re-create
                   // keeps it, so a fade never moves the light
};
static std::map<HeadKey, HeadState> g_headLights;
static std::map<HeadKey, float> g_wantHead;
static std::vector<HeadKey> g_wantHeadOrder;

static RhySignalHead g_rec[kRhySignalHeadCap];
static unsigned g_recN = 0;          // records in g_rec this frame
static unsigned g_headSwaps = 0;     // recreations caused by a colour change
static unsigned g_headExpired = 0;   // heads that stopped registering coronas
static bool g_hookTried = false;     // install attempted at least once

// A gain at or below this counts as dark: the handle is retired there.
static const float kGainFloor = 0.02f;

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
        g_xy.push_back(x);
        g_xy.push_back(y);
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
//
// s28: the band between the two radii is now also the fade band, so a signal
// that streams in fades up instead of arriving at full radiance, and one that
// streams out fades to nothing before its handle goes away.
static void reselect(const RhyConfig& c, const float* pp, double nowSeconds) {
    g_want.clear();
    g_wantOrder.clear();

    // Which list indices already have a handle (in any phase).
    std::vector<int> active;
    active.reserve(g_active.size());
    for (std::map<int, SignalState>::iterator it = g_active.begin();
         it != g_active.end(); ++it) {
        const int index = it->first / 4;
        if (active.empty() || active.back() != index) {
            active.push_back(index);
        }
    }
    std::sort(active.begin(), active.end());

    static std::vector<RhyWant> want;
    want.resize(g_lights.size());
    const unsigned n = rhySelectLights(
        g_xy.empty() ? nullptr : &g_xy[0], static_cast<unsigned>(g_lights.size()),
        active.empty() ? nullptr : &active[0], static_cast<unsigned>(active.size()),
        pp[0], pp[1], static_cast<float>(c.traffic_max_distance),
        static_cast<float>(c.traffic_exit_distance),
        static_cast<unsigned>(c.traffic_max_lights),
        static_cast<unsigned>(c.sticky_extra), &want[0],
        static_cast<unsigned>(want.size()));

    const float nearD = static_cast<float>(c.traffic_phase_near);
    const float near2 = nearD * nearD;
    for (unsigned i = 0; i < n; ++i) {
        const int index = want[i].id;
        // Only inside phase_near does a phase change recreate the light: beyond
        // it, keep whatever colour the light already has. Radiance is baked at
        // CreateLight, so recreating every light on every phase change is a
        // traffic light that visibly blinks across the whole district, for a
        // colour change nobody can see at that distance.
        const float dx = g_lights[index].x - pp[0];
        const float dy = g_lights[index].y - pp[1];
        int phase = (dx * dx + dy * dy <= near2)
                        ? rhyTrafficPhase(index, nowSeconds, c.traffic_mode,
                                          c.traffic_period)
                        : -1;
        if (phase < 0) {
            // Keep the colour it has. If it has no light yet, give it the phase
            // it would have had anyway.
            phase = -1;
            for (int p = 0; p < 3 && phase < 0; ++p) {
                if (g_active.find(signalKey(index, p)) != g_active.end()) {
                    phase = p;
                }
            }
            if (phase < 0) {
                phase = rhyTrafficPhase(index, nowSeconds, c.traffic_mode,
                                        c.traffic_period);
            }
        }
        if (want[i].gain <= 0.0f) {
            continue;
        }
        const int key = signalKey(index, phase);
        g_want[key] = want[i].gain;
        g_wantOrder.push_back(key);
        std::map<int, SignalState>::iterator cur =
            g_active.find(signalKey(index, phase));
        if (cur == g_active.end()) {
            ++g_phaseSwap;  // a handle in this colour does not exist yet
        }
    }
}

// ---------------------------------------------------------------- create / destroy

// Radiance is baked at CreateLight time, so a phase change means a second handle
// with a different hash - and s28 makes the swap a cross-fade between the two.
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

static remixapi_LightHandle createLight(int index, int phase, const RhyConfig& c,
                                        float gain) {
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
    if (gain < 0.0f) {
        gain = 0.0f;
    }
    if (gain > 1.0f) {
        gain = 1.0f;
    }

    std::memset(&info, 0, sizeof info);
    info.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
    // Distinct hash per (index, phase): the Remix runtime keys lights by hash.
    // With light_hash_base=0 the lights are anonymous instead.
    info.hash = c.light_hash_base +
                (static_cast<uint64_t>(index) << 2) +
                static_cast<uint64_t>(phase & 3);
    info.radiance.x = col[0] * rad * gain;
    info.radiance.y = col[1] * rad * gain;
    info.radiance.z = col[2] * rad * gain;

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

    int reason = 0;  // remix.h: 1 = our cap, 2 = the bridge, 3 = frame budget
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
        if (reason == 3) {
            return nullptr;  // frame budget spent: this fade step runs next frame
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
               "radiance=%.2f,%.2f,%.2f gain=%.2f radius=%.2f",
               index, phase, info.hash, sphere.position.x, sphere.position.y,
               sphere.position.z, info.radiance.x, info.radiance.y,
               info.radiance.z, gain, sphere.radius);
    }
    return handle;
}

// s28: no destroyAll(). Day, a missing player ped, or the module being switched
// off set every target gain to 0; the envelopes then ramp each signal down and
// retire the handle when it reaches dark.
static void fadeAllOut(const char* why) {
    if (!g_want.empty() || !g_wantHead.empty()) {
        rhyLog("trafficlights: fading %u signal lights out (%s)",
               static_cast<unsigned>(g_active.size() + g_headLights.size()), why);
    }
    g_want.clear();
    g_wantOrder.clear();
    g_wantHead.clear();
    g_wantHeadOrder.clear();
}

// Tag for the delayed-destroy queue of one head light. Two different heads can
// fold onto the same tag; the only consequence is that a flush retires the other
// head's handle a few frames earlier than it would have been, and that handle was
// already fading out (see delayed.h).
static int headTag(uint64_t id, int phase) {
    return kTagSignalHead +
           static_cast<int>((id * 4u + static_cast<uint64_t>(phase & 3)) % 400000ull);
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
                                            const RhyConfig& c, float gain) {
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
    if (gain < 0.0f) {
        gain = 0.0f;
    }
    if (gain > 1.0f) {
        gain = 1.0f;
    }

    static remixapi_LightInfo info;
    static remixapi_LightInfoSphereEXT sphere;

    std::memset(&info, 0, sizeof info);
    info.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
    info.hash = rhyHeadLightHash(id, phase, c.light_hash_base);
    info.radiance.x = col[0] * rad * gain;
    info.radiance.y = col[1] * rad * gain;
    info.radiance.z = col[2] * rad * gain;

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

    int reason = 0;  // remix.h: 1 = our cap, 2 = the bridge, 3 = frame budget
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
        if (reason == 3) {
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
               "gain=%.2f radius=%.2f",
               static_cast<unsigned long long>(id), phase, info.hash, pos[0], pos[1],
               pos[2], col[0] > 0.5f ? 1 : 0, col[1] > 0.5f ? 1 : 0,
               col[2] > 0.5f ? 1 : 0, info.radiance.x, info.radiance.y,
               info.radiance.z, gain, sphere.radius);
    }
    return handle;
}

// ---------------------------------------------------------------- head selection

// Re-diff the live head lights against what the game registered this frame.
// Same shape as reselect() above, with two differences that come from the source:
// a head the game stopped registering is not "out of range", it is "this aspect
// is off / this signal left the renderer", and it expires on a timeout; and a
// colour change is the game's own, so it is not limited to phase_near - the game
// only registers the lit head in the first place, so the heads that change
// colour are the ones the player is looking at.
//
// s28: expiring a head no longer destroys its light. The head goes off the want
// list, its envelope's target goes to 0, and the handle is retired when the ramp
// reaches dark - which is also what stops a signal blinking when the renderer
// skips a corona for one frame.
static void reselectHeads(const RhyConfig& c, const float* pp) {
    const DWORD now = GetTickCount();

    // 1. every head the game registered this frame is alive.
    for (unsigned i = 0; i < g_recN; ++i) {
        const uint64_t id = rhyHeadId(g_rec[i].entity, g_rec[i].index);
        const int phase = rhySignalPhaseFromGameRgb(g_rec[i].rgb[0], g_rec[i].rgb[1],
                                                    g_rec[i].rgb[2]);
        HeadSeen& hs = g_heads[id];
        hs.seen = g_rec[i].tick;
        if (phase >= 0) {
            hs.phase = phase;
        }
    }

    // 2. expire the ones that stopped registering. Their lights stay, on their
    // way down, until the envelope reaches dark.
    for (std::map<uint64_t, HeadSeen>::iterator it = g_heads.begin();
         it != g_heads.end();) {
        if (rhyHeadStale(it->second.seen, now,
                         static_cast<uint32_t>(c.traffic_head_ttl_ms))) {
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
    for (std::map<uint64_t, HeadSeen>::iterator it = g_heads.begin();
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

    g_wantHead.clear();
    g_wantHeadOrder.clear();
    for (unsigned i = 0; i < n; ++i) {
        const uint64_t id = want[i].id;
        int phase = want[i].phase;
        if (phase < 0) {
            // The game's corona colour was not recognisable on this frame. Do not
            // invent one: keep the colour the head already shows, if any.
            std::map<uint64_t, HeadSeen>::iterator hs = g_heads.find(id);
            if (hs == g_heads.end()) {
                continue;
            }
            phase = hs->second.phase;
            if (phase < 0) {
                continue;
            }
        }
        const float dist = std::sqrt(want[i].d2);
        const float gain = rhyDistanceGain(dist,
                                           static_cast<float>(c.traffic_max_distance),
                                           static_cast<float>(c.traffic_exit_distance));
        if (gain <= 0.0f) {
            continue;
        }
        HeadKey k;
        k.id = id;
        k.phase = phase;
        g_wantHead[k] = gain;
        g_wantHeadOrder.push_back(k);
        if (g_headLights.find(k) == g_headLights.end()) {
            ++g_headSwaps;  // a handle in this colour does not exist yet
        }
    }
}

// ---------------------------------------------------------------- per frame

// Advance one envelope, pay for the step if the frame can afford it, and draw.
// The `want` map holds the target gain for this exact (light, phase) key; a miss
// means "not wanted", which is a target of 0 and therefore a fade to dark. A
// fade to dark while ANOTHER phase of the same light is wanted is a colour
// cross-fade and gets the short phase_fade_ms ramp instead of fade_out_ms.

void rhyTrafficFrame() {
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

    float pp[3];
    const RhyPlayerState ps = rhyPlayerPosGrace(
        pp, static_cast<unsigned>(cfg->player_grace_ms), now);
    if (ps != kRhyPlayerOk) {
        ++g_playerGone;
    }

    ++g_frame;
    bool layerOk = cfg->traffic_enabled && cfg->traffic_mode != 0 &&
                   !g_apiBroken && ps != kRhyPlayerGone &&
                   (gameMode || !g_lights.empty());
    if ((g_frame % cfg->traffic_resync_frames) == 1) {
        g_night = !cfg->traffic_night_only || isNight(rhyGameHour(), *cfg);
        if (!layerOk || !g_night) {
            fadeAllOut(!cfg->traffic_enabled ? "disabled"
                           : cfg->traffic_mode == 0 ? "mode=off"
                           : !g_night ? "daylight"
                           : g_apiBroken ? "bridge refused a call"
                                         : "no player ped");
        } else if (gameMode) {
            reselectHeads(*cfg, pp);
        } else {
            reselect(*cfg, pp, static_cast<double>(GetTickCount()) / 1000.0);
        }
    }

    const float cross = static_cast<float>(cfg->phase_fade_ms) / 1000.0f;

    // One frame's worth of handle changes, counting ATTEMPTS. rhyCreateBudgetLeft()
    // only counts creates the bridge accepted, so a create the light cap refuses is
    // free on that counter and a frame would otherwise make hundreds of calls that
    // all fail (see remix.h).
    unsigned attempts = 0;
    const unsigned frameBudget = cfg->creates_per_frame
                                     ? static_cast<unsigned>(cfg->creates_per_frame)
                                     : 0u;  // 0 = no per-frame limit

    // --- list-mode lights -----------------------------------------------------
    unsigned tl_drawn = 0;
    for (std::map<int, SignalState>::iterator it = g_active.begin();
         it != g_active.end();) {
        const int index = it->first / 4;
        const int phase = it->first & 3;
        std::map<int, float>::iterator w = g_want.find(it->first);
        const float target = (w != g_want.end()) ? w->second : 0.0f;
        it->second.fade.target = target;
        // A fade to dark while another phase of the same signal is wanted is a
        // colour cross-fade, not a light going out: it gets the short ramp.
        bool crossFade = false;
        if (target <= 0.0f) {
            for (int p = 0; p < 3; ++p) {
                if (p == phase) {
                    continue;
                }
                if (g_want.find(signalKey(index, p)) != g_want.end()) {
                    crossFade = true;
                    break;
                }
            }
        }
        if (crossFade) {
            rhyFadeSetRamp(it->second.fade, cross, cross, 0.05f);
        }
        rhyFadeAdvance(it->second.fade, dt);

        const bool needsHandle = !it->second.handle;  // a create that failed
        if ((needsHandle || rhyFadeDue(it->second.fade, cfg->fade_step, kGainFloor)) &&
            (frameBudget == 0 || attempts < frameBudget) && rhyCreateBudgetLeft()) {
            ++attempts;
            const RhyFadeOp op = rhyFadeApply(it->second.fade, cfg->fade_step,
                                              kGainFloor);
            if (op == kRhyFadeRetire) {
                rhySpendCreateSlot();  // a destroy counts against the frame budget
                if (it->second.handle) {
                    rhyDelayDestroy(it->second.handle, kTagTraffic + it->first);
                }
                g_active.erase(it++);
                ++g_retired;
                continue;
            }
            if (op == kRhyFadeRecreate || op == kRhyFadeCreate) {
                if (!rhyCanCreateLight()) {
                    ++it;
                    continue;  // no room this frame: keep the handle we have
                }
                if (it->second.handle) {
                    rhyDelayDestroy(it->second.handle, kTagTraffic + it->first);
                }
                rhyFlushDelayedTag(kTagTraffic + it->first);  // same hash: delayed.h
                remixapi_LightHandle h = createLight(index, phase, *cfg,
                                                     it->second.fade.published);
                if (h) {
                    it->second.handle = h;
                    ++g_fadeSteps;
                } else {
                    // The old handle is already destroyed by here, so it must not be
                    // drawn again; null makes the next frame ask for a new one.
                    it->second.handle = nullptr;
                    if (g_apiBroken) {
                        ++it;
                        break;
                    }
                }
            }
        }

        if (it->second.handle && !rhyDrawLight(it->second.handle)) {
            g_apiBroken = true;
            g_brokenTick = GetTickCount();
            rhyLog("trafficlights: DrawLightInstance rejected for #%d (draw "
                   "failures so far: %u, live handles: %u), traffic lights off "
                   "until the next rhyremix.ini edit",
                   it->first, rhyDrawFailures(), rhyLiveLights());
            break;
        }
        if (it->second.handle) {
            ++tl_drawn;
        }
        ++it;
    }

    // newly wanted list lights, nearest first
    for (size_t i = 0; i < g_wantOrder.size(); ++i) {
        const int key = g_wantOrder[i];
        if (g_active.find(key) != g_active.end()) {
            continue;
        }
        if ((frameBudget != 0 && attempts >= frameBudget) || !rhyCreateBudgetLeft()) {
            break;
        }
        std::map<int, float>::iterator w = g_want.find(key);
        if (w == g_want.end() || w->second <= 0.0f) {
            continue;
        }
        SignalState st;
        st.handle = nullptr;
        rhyFadeInit(st.fade, static_cast<float>(cfg->fade_in_ms) / 1000.0f,
                    static_cast<float>(cfg->fade_out_ms) / 1000.0f);
        st.fade.target = w->second;
        // Is this the incoming half of a colour cross-fade? Another phase of the
        // same index is already live, so use the short cross-fade ramp.
        const int index = key / 4;
        const int phase = key & 3;
        for (int p = 0; p < 3; ++p) {
            if (p == phase) {
                continue;
            }
            if (g_active.find(signalKey(index, p)) != g_active.end()) {
                rhyFadeSetRamp(st.fade, cross, cross, 0.05f);
                break;
            }
        }
        rhyFadeAdvance(st.fade, dt);
        if (!rhyFadeDue(st.fade, cfg->fade_step, kGainFloor)) {
            continue;
        }
        if (rhyFadeApply(st.fade, cfg->fade_step, kGainFloor) != kRhyFadeCreate) {
            continue;
        }
        ++attempts;
        rhyFlushDelayedTag(kTagTraffic + key);  // destroy-before-create: same hash
        st.handle = createLight(index, phase, *cfg, st.fade.published);
        if (st.handle) {
            g_active[key] = st;
        } else if (g_apiBroken) {
            break;
        }
    }

    // --- head lights (source=game) -------------------------------------------
    unsigned hd_drawn = 0;
    for (std::map<HeadKey, HeadState>::iterator it = g_headLights.begin();
         it != g_headLights.end();) {
        std::map<HeadKey, float>::iterator w = g_wantHead.find(it->first);
        const float target = (w != g_wantHead.end()) ? w->second : 0.0f;
        it->second.fade.target = target;
        if (target <= 0.0f) {
            // colour change or expiry: a colour change cross-fades (short ramp),
            // an expiry uses the normal fade-out.
            for (int p = 0; p < 3; ++p) {
                if (p == it->first.phase) {
                    continue;
                }
                HeadKey other;
                other.id = it->first.id;
                other.phase = p;
                if (g_wantHead.find(other) != g_wantHead.end()) {
                    rhyFadeSetRamp(it->second.fade, cross, cross, 0.05f);
                    break;
                }
            }
        }
        rhyFadeAdvance(it->second.fade, dt);

        const bool needsHeadHandle = !it->second.handle;  // a create that failed
        if ((needsHeadHandle ||
             rhyFadeDue(it->second.fade, cfg->fade_step, kGainFloor)) &&
            (frameBudget == 0 || attempts < frameBudget) && rhyCreateBudgetLeft()) {
            ++attempts;
            const RhyFadeOp op = rhyFadeApply(it->second.fade, cfg->fade_step,
                                              kGainFloor);
            if (op == kRhyFadeRetire) {
                rhySpendCreateSlot();  // a destroy counts against the frame budget
                if (it->second.handle) {
                    rhyDelayDestroy(it->second.handle,
                                    headTag(it->first.id, it->first.phase));
                }
                g_headLights.erase(it++);
                ++g_retired;
                continue;
            }
            if (op == kRhyFadeRecreate || op == kRhyFadeCreate) {
                if (!rhyCanCreateLight()) {
                    ++it;
                    continue;  // no room this frame: keep the handle we have
                }
                // The position is the one this light was created with: the head
                // has not moved, and re-deriving it from a fresh record would
                // make the light jump (and on a signal mounted on something that
                // moves, would be its own kind of lighting step).
                const float* pos = it->second.pos;
                if (it->second.handle) {
                    rhyDelayDestroy(it->second.handle,
                                    headTag(it->first.id, it->first.phase));
                }
                rhyFlushDelayedTag(headTag(it->first.id, it->first.phase));
                remixapi_LightHandle h = createHeadLight(
                    it->first.id, it->first.phase, pos, *cfg,
                    it->second.fade.published);
                if (h) {
                    it->second.handle = h;
                    ++g_fadeSteps;
                } else {
                    it->second.handle = nullptr;  // destroyed: see above
                    if (g_apiBroken) {
                        ++it;
                        break;
                    }
                }
            }
        }

        if (it->second.handle && !rhyDrawLight(it->second.handle)) {
            g_apiBroken = true;
            g_brokenTick = GetTickCount();
            rhyLog("trafficlights: DrawLightInstance rejected for head %016llx "
                   "(draw failures so far: %u, live handles: %u), traffic lights "
                   "off until the next rhyremix.ini edit",
                   static_cast<unsigned long long>(it->first.id), rhyDrawFailures(),
                   rhyLiveLights());
            break;
        }
        if (it->second.handle) {
            ++hd_drawn;
        }
        ++it;
    }

    // newly wanted head lights, nearest first
    for (size_t i = 0; i < g_wantHeadOrder.size(); ++i) {
        const HeadKey& key = g_wantHeadOrder[i];
        if (g_headLights.find(key) != g_headLights.end()) {
            continue;
        }
        if ((frameBudget != 0 && attempts >= frameBudget) || !rhyCreateBudgetLeft()) {
            break;
        }
        std::map<HeadKey, float>::iterator w = g_wantHead.find(key);
        if (w == g_wantHead.end() || w->second <= 0.0f) {
            continue;
        }
        // find the record this head came from, for its position
        int src = -1;
        for (unsigned r = 0; r < g_recN; ++r) {
            if (rhyHeadId(g_rec[r].entity, g_rec[r].index) == key.id) {
                src = static_cast<int>(r);
                break;
            }
        }
        if (src < 0) {
            continue;
        }
        HeadState st;
        st.handle = nullptr;
        rhyFadeInit(st.fade, static_cast<float>(cfg->fade_in_ms) / 1000.0f,
                    static_cast<float>(cfg->fade_out_ms) / 1000.0f);
        st.fade.target = w->second;
        for (int p = 0; p < 3; ++p) {
            if (p == key.phase) {
                continue;
            }
            HeadKey other;
            other.id = key.id;
            other.phase = p;
            if (g_headLights.find(other) != g_headLights.end()) {
                rhyFadeSetRamp(st.fade, cross, cross, 0.05f);
                break;
            }
        }
        rhyFadeAdvance(st.fade, dt);
        if (!rhyFadeDue(st.fade, cfg->fade_step, kGainFloor)) {
            continue;
        }
        if (rhyFadeApply(st.fade, cfg->fade_step, kGainFloor) != kRhyFadeCreate) {
            continue;
        }
        ++attempts;
        rhyHeadLightPos(&g_rec[src],
                        cfg->traffic_head_normal ? g_rec[src].normal : nullptr,
                        cfg->traffic_head_forward, st.pos);
        rhyFlushDelayedTag(headTag(key.id, key.phase));  // same hash: see delayed.h
        st.handle = createHeadLight(key.id, key.phase, st.pos, *cfg,
                                    st.fade.published);
        if (st.handle) {
            g_headLights[key] = st;
        } else if (g_apiBroken) {
            break;
        }
    }

    static DWORD g_lastCountTick = 0;
    if (now - g_lastCountTick >= 10000) {  // active count once per 10 s
        g_lastCountTick = now;
        if (gameMode) {
            int green = 0, amber = 0, red = 0;
            for (std::map<HeadKey, HeadState>::iterator it = g_headLights.begin();
                 it != g_headLights.end(); ++it) {
                if (it->first.phase == 0) {
                    ++green;
                } else if (it->first.phase == 1) {
                    ++amber;
                } else {
                    ++red;
                }
            }
            rhyLog("trafficlights[source=game]: %u heads lit, %u drawn this frame "
                   "(green=%d amber=%d red=%d, %u coronas registered this frame, "
                   "colour_swaps=%u expired=%u steps=%u retired=%u "
                   "player_gone_frames=%u) | remix: live=%u create_fail=%u "
                   "draw_fail=%u budget_stop=%u deferred=%u | hook: calls=%u "
                   "records=%u dropped=%u bad=%u reentrant=%u",
                   static_cast<unsigned>(g_headLights.size()), hd_drawn, green, amber,
                   red, g_recN, g_headSwaps, g_headExpired, g_fadeSteps, g_retired,
                   g_playerGone, rhyLiveLights(), rhyCreateFailures(),
                   rhyDrawFailures(), g_budgetStop, rhyCreateDeferred(),
                   rhySignalHookCalls(), rhySignalHookRecords(),
                   rhySignalHookDropped(), rhySignalHookBad(),
                   rhySignalHookReentrant());
        } else {
            int green = 0, amber = 0, red = 0;
            for (std::map<int, SignalState>::iterator it = g_active.begin();
                 it != g_active.end(); ++it) {
                if ((it->first & 3) == 0) {
                    ++green;
                } else if ((it->first & 3) == 1) {
                    ++amber;
                } else {
                    ++red;
                }
            }
            rhyLog("trafficlights[%s]: %u active, %u drawn this frame (green=%d "
                   "amber=%d red=%d, %u awaiting destroy, %u in list, hour=%d, "
                   "phase_swaps=%u steps=%u retired=%u player_gone_frames=%u) | "
                   "remix: live=%u create_fail=%u draw_fail=%u budget_stop=%u "
                   "deferred=%u",
                   gameMode ? "list" : "list, source=game requested but the hook "
                                   "is not installed",
                   static_cast<unsigned>(g_active.size()), tl_drawn, green, amber,
                   red, rhyDelayedPending(), static_cast<unsigned>(g_lights.size()),
                   rhyGameHour(), g_phaseSwap, g_fadeSteps, g_retired, g_playerGone,
                   rhyLiveLights(), rhyCreateFailures(), rhyDrawFailures(),
                   g_budgetStop, rhyCreateDeferred());
        }
    }
}
