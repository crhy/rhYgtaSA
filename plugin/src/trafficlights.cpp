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

// One traffic light from rhyremix_traffic.txt: exactly the parsed line, so the
// v1/v2 handling lives in one place (rhyTrafficParseLine, signalheads.h).
// v1 lines are "x y z headz"; v2 lines add "heading lensx lensy lensz" (the
// instance yaw in degrees and the head offset in the model's own frame), which
// is what moves the light from the mast onto the lens. See tools/traffic_list.py
// for how the columns are made.
using TrafficLight = RhyTrafficLine;

static std::vector<TrafficLight> g_lights;
static std::vector<float> g_xy;  // x,y pairs of g_lights, for rhySelectLights

// s28: a signal light is (list index, phase), and its brightness is a fade
// envelope, not a constant. Keying the live set by BOTH index and phase is what
// makes a colour change a cross-fade instead of a jump: the old colour's handle
// ramps down while the new colour's ramps up, and because the hash carries the
// phase the two can exist at the same time.
struct SignalState {
    remixapi_LightHandle handle;  // the bulb at the lens (s30/n02)
    remixapi_LightHandle glow;    // the soft glow around the head (n02b)
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
// n04: cumulative count of signals inside max_distance that a re-selection left
// without a light. Same field proof as the street layer's in_drop: it should
// stay 0 for a whole run.
static unsigned g_droppedInside = 0;
static unsigned g_playerGone = 0;  // frames the player ped pointer was unreadable

// --- source=game: one Remix light per signal HEAD the game itself lights ---
//
// Keyed by rhyHeadId(entity, 2dfx index) instead of a list index, because the
// heads come and go with what the renderer draws rather than from a file.
//
// n04: this is no longer just "what registered a corona this frame". The game
// registers a corona only for what its renderer draws, so the record set follows
// the camera frustum; keeping the memory (position + last colour + last tick)
// lets a head we turned our back on survive exactly like a lamp the player
// walked away from - by distance, never by view direction. See rhyHeadKeepAlive.
struct HeadSeen {
    DWORD seen;        // last tick this head registered a corona
    uint32_t entity;   // the CEntity* the id came from, kept to rebuild a record
    uint32_t index;    // the 2DFX head index, same reason
    int phase;         // the phase the game last showed for it (-1 = unknown)
    float pos[3];      // the lens position of the last record
    uint8_t rgb[3];    // the colour of the last record (phase is derived from it)
};
static std::map<uint64_t, HeadSeen> g_heads;
// The candidate array handed to rhyChooseHeads: the remembered heads, which
// already includes everything the game registered this frame (step 1 of
// reselectHeads folds the records into g_heads first).
static std::vector<RhySignalHead> g_headCand;

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
    remixapi_LightHandle handle;  // the bulb at the lens (s30/n02)
    remixapi_LightHandle glow;    // the soft glow around the head (n02b)
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
static DWORD g_hookTick = 0;         // when the hook went live
// n02: the last head-source decision we logged (rhyHeadsHealthy's enum, -1 = not
// decided yet). The decision is logged when it changes, once, with the distance
// that made it - not every frame, and not as a bare "records=0".
static int g_hookDecision = -1;

// n02: how long the hook may live without producing a single head before the
// module asks whether there was anything to produce. See rhyHeadsHealthy(); the
// value is the ini's head_grace_ms, this is only the default it falls back to.
static const DWORD kHeadSourceGraceMs = 20000;

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

// ---------------------------------------------------------------- loading

static void loadList() {
    const RhyConfig* cfg = rhyConfigGet();
    FILE* f = std::fopen(cfg->trafficPath.c_str(), "rb");
    if (!f) {
        rhyLog("trafficlights: cannot open %s", cfg->trafficPath.c_str());
        return;
    }
    char line[256];
    unsigned withLens = 0;
    while (std::fgets(line, sizeof line, f)) {
        RhyTrafficLine ln;
        if (!rhyTrafficParseLine(line, &ln)) {
            continue;
        }
        if (ln.hasLens) {
            ++withLens;
        }
        g_lights.push_back(ln);
        g_xy.push_back(ln.x);
        g_xy.push_back(ln.y);
    }
    std::fclose(f);
    rhyLog("trafficlights: loaded %u lights from %s (%u carry the v2 heading/lens "
           "columns, %u are v1 'x y z headz' and stay on the mast)",
           static_cast<unsigned>(g_lights.size()), cfg->trafficPath.c_str(),
           withLens, static_cast<unsigned>(g_lights.size()) - withLens);
    for (size_t i = 0; i < g_lights.size() && i < 3; ++i) {
        float p[3];
        rhyTrafficLightPos(g_lights[i], 1, p);
        rhyLog("trafficlights: #%d at %.1f,%.1f,%.1f head z=%.1f heading=%.0f "
               "lens=(%+.2f,%+.2f,%.2f) -> light %.1f,%.1f,%.1f",
               static_cast<int>(i), g_lights[i].x, g_lights[i].y,
               g_lights[i].z, g_lights[i].z + g_lights[i].headz,
               g_lights[i].heading, g_lights[i].lens[0], g_lights[i].lens[1],
               g_lights[i].lens[2], p[0], p[1], p[2]);
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
        g_hookTick = GetTickCount();
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
        static_cast<unsigned>(want.size()),
        // n04: the cap may only trim the band beyond traffic_max_distance. The
        // shipped 60+90 against up to 85 signals within 250 units (the worst case
        // measured over the whole 1522-entry list) was evicting signals that were
        // right in front of the player.
        static_cast<float>(c.traffic_max_distance));

    // n04: the field proof - a signal inside max_distance that this pass left
    // without a light. Counted over the live handles, not over the list.
    {
        std::vector<int> chosen;
        chosen.reserve(n);
        for (unsigned i = 0; i < n; ++i) {
            if (want[i].gain > 0.0f) {
                chosen.push_back(want[i].id);
            }
        }
        std::sort(chosen.begin(), chosen.end());
        const float r2 = static_cast<float>(c.traffic_max_distance) *
                         static_cast<float>(c.traffic_max_distance);
        for (std::map<int, SignalState>::iterator it = g_active.begin();
             it != g_active.end(); ++it) {
            const int index = it->first >> 2;
            if (index < 0 ||
                static_cast<size_t>(index) >= g_lights.size()) {
                continue;
            }
            if (std::binary_search(chosen.begin(), chosen.end(), index)) {
                continue;  // wanted in some phase: a colour change is not a drop
            }
            const float dx = g_lights[index].x - pp[0];
            const float dy = g_lights[index].y - pp[1];
            if (dx * dx + dy * dy < r2) {
                ++g_droppedInside;
            }
        }
    }

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

// s30: the bulb, in one place so the list path and the head path cannot drift.
// With fixture_size=1 the radius is the real lens (lamps.h: 0.10 m for a 200 mm
// SA signal) unless bulb_radius overrides it, and the radiance is the ini's
// bulb_radiance in nits AT that radius. With fixture_size=0 the old
// flux-preserving blob is kept, because there the user is driving a blob on
// purpose. See rhySignalBulb() in signalheads.h for why the (ref/r)^2 rule is
// right for a street luminaire and wrong for a signal lens.
static void bulbFor(int phase, const RhyConfig& c, RhySignalBulb* out) {
    float lens = 0.0f;
    if (c.fixture_size) {
        lens = c.traffic_bulb_radius > 0.0f ? c.traffic_bulb_radius
                                            : rhySignalRadius(c.fixture_scale);
    } else {
        lens = c.traffic_radius;
    }
    rhySignalBulb(phase, c.fixture_size, lens, c.traffic_bulb_radiance,
                  c.traffic_radiance, c.traffic_radius_ref, c.radiance_scale,
                  out);
}

// n02b: the glow, the other half of "both like real life" (rhY 2026-11-12
// 08:50). Same head, same colour, same phase, same gain envelope as the bulb -
// a bigger sphere at a much lower radiance, so the signal colours the pole, the
// wall, the road and the peds under it instead of being a bright dot that lights
// nothing. The flux default (304) is what the 2026-11-12 00:48 glow measured, and
// rhySignalGlow clamps it to one street luminaire's (3473). See signalheads.h.
static void glowFor(int phase, const RhyConfig& c, RhySignalGlow* out) {
    rhySignalGlow(phase, c.traffic_glow_radius, c.traffic_glow_flux,
                  kRhySignalGlowFluxCap, c.radiance_scale, out);
}

// n02b: glow_flux=0 means "bulb only": no second handle exists at all, so the
// A/B costs nothing and no dark light is carried around. The radiance-0 path
// inside rhySignalGlow is what a fade uses; this is the off switch.
static bool glowWanted(const RhyConfig& c) { return c.traffic_glow_flux > 0.0f; }

// n02b: a signal is two handles, so the second create needs its own credit:
// this frame's CreateLight budget (creates_per_frame) and our own light cap.
static bool canCreateAnother(unsigned attempts, unsigned frameBudget) {
    return (frameBudget == 0 || attempts < frameBudget) &&
           rhyCreateBudgetLeft() && rhyCanCreateLight();
}

// The two halves of one signal, resolved in one place so the pair cannot drift
// apart on colour, phase or gain: kind selects WHICH one, and nothing else.
enum SignalLightKind { kSignalBulb = 0, kSignalGlow = 1 };

struct SignalLightSpec {
    float radius;
    float radiance[3];
    float flux;
    const char* what;   // tail of the CreateLight log line
};

static void specFor(int phase, const RhyConfig& c, int kind,
                    SignalLightSpec* out) {
    if (kind == kSignalGlow) {
        RhySignalGlow g;
        glowFor(phase, c, &g);
        out->radius = g.radius;
        for (int i = 0; i < 3; ++i) {
            out->radiance[i] = g.radiance[i];
        }
        out->flux = g.flux;
        out->what = g.clamped ? " (glow: flux capped at one street luminaire)"
                              : (g.flux > 0.0f ? " (glow: soft spill at the head)"
                                               : " (glow: off, glow_flux=0)");
        return;
    }
    RhySignalBulb b;
    bulbFor(phase, c, &b);
    out->radius = b.radius;
    for (int i = 0; i < 3; ++i) {
        out->radiance[i] = b.radiance[i];
    }
    out->flux = b.flux;
    out->what = b.legacy ? " (bulb: legacy blob, flux-preserving)"
                         : " (bulb: lens nits)";
}

// Radiance is baked at CreateLight time, so a phase change means a second handle
// with a different hash - and s28 makes the swap a cross-fade between the two.
// The colour of each handle comes from rhySignalBulb()/rhySignalLensRgb(): the
// passbands of real signal lenses, not three hand-picked triples (see lamps.h).

static remixapi_LightHandle createLight(int index, int phase, const RhyConfig& c,
                                        float gain, int kind) {
    const remixapi_Interface* api = rhyRemixApi();
    if (!api || !api->CreateLight) {
        return nullptr;
    }
    const TrafficLight& L = g_lights[index];

    static remixapi_LightInfo info;
    static remixapi_LightInfoSphereEXT sphere;

    // A signal module lens is 200 mm across, so the emitting sphere is 0.1 game
    // units in radius; the shipped value of 6 was a six metre ball, wider than
    // the intersection it is supposed to be a point on. s30: the radiance is the
    // lens luminance at that radius (bulb_radiance nits), NOT the six-metre
    // blob's output squeezed into it - that x3600 is what made sixty signals
    // light a whole street green instead of showing sixty small bright heads.
    // n02b: `kind` says which half of the signal this handle is. The bulb and
    // the glow are created by this one function, at this one position, with this
    // one gain, and differ only in radius and radiance (specFor).
    SignalLightSpec spec;
    specFor(phase, c, kind, &spec);
    const float radius = spec.radius;
    if (gain < 0.0f) {
        gain = 0.0f;
    }
    if (gain > 1.0f) {
        gain = 1.0f;
    }

    std::memset(&info, 0, sizeof info);
    info.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
    // Distinct hash per (index, phase): the Remix runtime keys lights by hash.
    // With light_hash_base=0 the lights are anonymous instead. n02b: the glow is
    // a second light on the same signal, so it carries a fixed bias above both
    // the traffic range (index<<2 < 6092) and the street range (index < 7003).
    info.hash = c.light_hash_base +
                (static_cast<uint64_t>(index) << 2) +
                static_cast<uint64_t>(phase & 3) +
                (kind == kSignalGlow ? kRhySignalGlowHashBias : 0ull);
    info.radiance.x = spec.radiance[0] * gain;
    info.radiance.y = spec.radiance[1] * gain;
    info.radiance.z = spec.radiance[2] * gain;

    std::memset(&sphere, 0, sizeof sphere);
    sphere.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_SPHERE_EXT;
    sphere.pNext = nullptr;
    // n02: the lens, not the mast. A v2 line carries the instance heading and the
    // model-local head offset (cj_traffic_light4: -2.10,+0.10,+7.60 - the head
    // hangs 2.10 m sideways of the pole), and rotating that offset by the heading
    // is what the game itself does with a head offset at 0x59C890. A v1 line, or
    // lens_offset=0, gives the old pole x,y,z+headz byte for byte.
    float pos[3];
    rhyTrafficLightPos(L, c.traffic_lens_offset, pos);
    sphere.position.x = pos[0];
    sphere.position.y = pos[1];
    sphere.position.z = pos[2];
    sphere.radius = radius;
    // Deliberately unshaped. A real signal module is strongly directional - the
    // visor and refractor throw a narrow beam down the approach - and a v2 list
    // line now does carry the instance heading, but the heading is the model's,
    // not the visor's: nothing measured says which way inside the model the beam
    // faces (the game's own 2DFX entry carries a position and no direction
    // either - 0x59C890 rotates three floats and adds the translation). A wrong
    // cone would put a dark face on every signal seen from the cross street, so
    // the heading ships as a number we do not yet aim with. docs/FINDINGS.md has
    // the measurements; the light itself has moved onto the lens (just above).
    sphere.shaping_hasvalue = 0;
    sphere.volumetricRadianceScale = 0.0f;
    info.pNext = &sphere;

    int reason = 0;  // remix.h: 1 = our cap, 2 = the bridge, 3 = frame budget
    const remixapi_LightHandle handle = rhyCreateLight(&info, &reason);
    if (!handle) {
        if (reason == 2) {
            g_apiBroken = true;
            g_brokenTick = GetTickCount();
            rhyLog("trafficlights: CreateLight refused by the bridge #%d %s phase %d "
                   "(bridge create failures so far: %u), traffic lights off until "
                   "the next rhyremix.ini edit",
                   index, kind == kSignalGlow ? "glow" : "bulb", phase,
                   rhyCreateFailures());
            return nullptr;
        }
        if (reason == 3) {
            return nullptr;  // frame budget spent: this fade step runs next frame
        }
        if (++g_budgetStop <= 3) {
            rhyLog("trafficlights: no slot for #%d %s phase %d, max_total_lights reached "
                   "(live=%u, refusals so far: %u)", index,
                   kind == kSignalGlow ? "glow" : "bulb", phase, rhyLiveLights(),
                   g_budgetStop);
        }
        return nullptr;
    }
    // Log the exact numbers of the first few lights once, to compare against
    // probe_radius / probe_radiance when tuning. Per kind, so the log always
    // shows a bulb line AND a glow line for the same signal.
    static int createdLogs[2] = {0, 0};
    if (createdLogs[kind == kSignalGlow ? 1 : 0] < 3) {
        ++createdLogs[kind == kSignalGlow ? 1 : 0];
        rhyLog("trafficlights: CreateLight ok #%d %s phase %d hash=%llu pos=%.1f,%.1f,%.1f "
               "radiance=%.2f,%.2f,%.2f gain=%.2f radius=%.2f flux=%.0f%s",
               index, kind == kSignalGlow ? "glow" : "bulb", phase, info.hash,
               sphere.position.x, sphere.position.y,
               sphere.position.z, info.radiance.x, info.radiance.y,
               info.radiance.z, gain, sphere.radius, spec.flux, spec.what);
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
                                            const RhyConfig& c, float gain,
                                            int kind) {
    const remixapi_Interface* api = rhyRemixApi();
    if (!api || !api->CreateLight) {
        return nullptr;
    }
    // Same bulb as the list path: the lens radius, and lens luminance rather
    // than blob flux squeezed into it. n02b: and the same glow - a head the game
    // registered gets the soft second light too, at the same position, in the
    // same colour the game chose for that head. specFor is shared by both paths
    // so a head light and a list light for the same phase are the same pair.
    SignalLightSpec spec;
    specFor(phase, c, kind, &spec);
    const float radius = spec.radius;
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
    info.hash = rhyHeadLightHash(id, phase,
                                 c.light_hash_base +
                                 (kind == kSignalGlow ? kRhySignalGlowHashBias
                                                      : 0ull));
    info.radiance.x = spec.radiance[0] * gain;
    info.radiance.y = spec.radiance[1] * gain;
    info.radiance.z = spec.radiance[2] * gain;

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
                   "%016llx %s phase %d (bridge create failures so far: %u), traffic "
                   "lights off until the next rhyremix.ini edit",
                   static_cast<unsigned long long>(id),
                   kind == kSignalGlow ? "glow" : "bulb", phase,
                   rhyCreateFailures());
            return nullptr;
        }
        if (reason == 3) {
            return nullptr;
        }
        if (++g_budgetStop <= 3) {
            rhyLog("trafficlights: no slot for head %016llx %s phase %d, "
                   "max_total_lights reached (live=%u, refusals so far: %u)",
                   static_cast<unsigned long long>(id),
                   kind == kSignalGlow ? "glow" : "bulb", phase, rhyLiveLights(),
                   g_budgetStop);
        }
        return nullptr;
    }
    static int headLogs[2] = {0, 0};
    if (headLogs[kind == kSignalGlow ? 1 : 0] < 3) {
        ++headLogs[kind == kSignalGlow ? 1 : 0];
        rhyLog("trafficlights: head light ok %s id=%016llx phase=%d hash=%llu "
               "pos=%.2f,%.2f,%.2f radiance=%.2f,%.2f,%.2f "
               "gain=%.2f radius=%.2f flux=%.0f%s",
               kind == kSignalGlow ? "glow" : "bulb",
               static_cast<unsigned long long>(id), phase, info.hash, pos[0], pos[1],
               pos[2], info.radiance.x, info.radiance.y, info.radiance.z, gain,
               sphere.radius, spec.flux, spec.what);
    }
    return handle;
}

// ---------------------------------------------------------------- head selection

// Re-diff the live head lights against the heads the game has shown us.
//
// n04 rewrote the middle of this. The two differences from reselect() above used
// to be (a) a head the game stopped registering is "this aspect is off / this
// signal left the renderer", expiring on a timeout, and (b) a colour change is
// the game's own. (b) stands. (a) does not: the game registers a signal corona
// from its renderer, so the record set IS the camera frustum, and expiring on it
// made every bulb and glow on a junction fade out 2 s after the player turned his
// back on it and get created again when he looked back - the only light in this
// plugin that came and went with camera DIRECTION. Measured in the 2026-11-12
// 10:22 run: expired=790, retired=817, steps=5669 in 970 s, for at most 19 heads.
//
// The head set is now keyed by the PLAYER's distance like everything else: the
// records are folded into a memory (position + last colour + last tick), a head
// is kept while it is within the selection radius, and it is dropped only when it
// leaves the wider one (rhyHeadKeepAlive). A head we cannot see keeps the colour
// it last showed - invisible by definition - and the frame the player looks back
// the game re-registers it and the cross-fade takes over.
//
// s28's half still holds: expiring a head never destroys its light in one frame.
// The head leaves the want list, the envelope's target goes to 0, and the handle
// is retired when the ramp reaches dark.
static void reselectHeads(const RhyConfig& c, const float* pp) {
    const DWORD now = GetTickCount();

    // 1. every head the game registered this frame updates the memory.
    for (unsigned i = 0; i < g_recN; ++i) {
        const uint64_t id = rhyHeadId(g_rec[i].entity, g_rec[i].index);
        const int phase = rhySignalPhaseFromGameRgb(g_rec[i].rgb[0], g_rec[i].rgb[1],
                                                    g_rec[i].rgb[2]);
        HeadSeen& hs = g_heads[id];
        hs.seen = g_rec[i].tick;
        hs.entity = g_rec[i].entity;
        hs.index = g_rec[i].index;
        hs.pos[0] = g_rec[i].pos[0];
        hs.pos[1] = g_rec[i].pos[1];
        hs.pos[2] = g_rec[i].pos[2];
        hs.rgb[0] = g_rec[i].rgb[0];
        hs.rgb[1] = g_rec[i].rgb[1];
        hs.rgb[2] = g_rec[i].rgb[2];
        if (phase >= 0) {
            hs.phase = phase;
        }
    }

    // 2. drop a head only when it is FAR, never when it is merely off screen.
    for (std::map<uint64_t, HeadSeen>::iterator it = g_heads.begin();
         it != g_heads.end();) {
        const float dx = it->second.pos[0] - pp[0];
        const float dy = it->second.pos[1] - pp[1];
        const float d = std::sqrt(dx * dx + dy * dy);
        if (!rhyHeadKeepAlive(it->second.seen, now,
                              static_cast<uint32_t>(c.traffic_head_ttl_ms), d,
                              static_cast<float>(c.traffic_max_distance),
                              static_cast<float>(c.traffic_exit_distance))) {
            g_heads.erase(it++);
            ++g_headExpired;
        } else {
            ++it;
        }
    }

    // 3. the candidates are the memory, not this frame's records: a frame in
    // which the game registered nothing (the renderer skipped, or every signal is
    // behind the camera) must not empty the want set.
    g_headCand.clear();
    for (std::map<uint64_t, HeadSeen>::const_iterator it = g_heads.begin();
         it != g_heads.end() && g_headCand.size() < kRhySignalHeadCap; ++it) {
        RhySignalHead h;
        std::memset(&h, 0, sizeof h);
        h.entity = it->second.entity;
        h.index = it->second.index;
        h.pos[0] = it->second.pos[0];
        h.pos[1] = it->second.pos[1];
        h.pos[2] = it->second.pos[2];
        h.normal[2] = 1.0f;
        h.rgb[0] = it->second.rgb[0];
        h.rgb[1] = it->second.rgb[1];
        h.rgb[2] = it->second.rgb[2];
        h.alpha = 255;
        h.strength = 1.0f;
        h.tick = it->second.seen;
        g_headCand.push_back(h);
    }
    if (g_headCand.empty()) {
        // Nothing has ever registered: nothing to want. (This used to be the
        // g_recN == 0 early-out, which was wrong for the same reason the expiry
        // was: one empty frame is not "every signal went out".)
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
        &g_headCand[0], static_cast<unsigned>(g_headCand.size()), pp,
        static_cast<float>(c.traffic_max_distance),
        static_cast<float>(c.traffic_exit_distance),
        static_cast<unsigned>(c.traffic_max_lights),
        static_cast<unsigned>(c.sticky_extra),
        active.empty() ? nullptr : &active[0],
        static_cast<unsigned>(active.size()), want, kRhySignalHeadCap,
        static_cast<float>(c.traffic_max_distance));

    // n04: the same field proof for heads: a head inside max_distance that this
    // pass got no light for. With the memory + distance rule it should stay 0 for
    // a whole run, and the counter is in the 10 s line so a run can show it.
    {
        std::vector<uint64_t> chosen;
        chosen.reserve(n);
        for (unsigned i = 0; i < n; ++i) {
            chosen.push_back(want[i].id);
        }
        std::sort(chosen.begin(), chosen.end());
        const float r2 = static_cast<float>(c.traffic_max_distance) *
                         static_cast<float>(c.traffic_max_distance);
        for (std::map<uint64_t, HeadSeen>::const_iterator it = g_heads.begin();
             it != g_heads.end(); ++it) {
            const float dx = it->second.pos[0] - pp[0];
            const float dy = it->second.pos[1] - pp[1];
            if (it->second.phase < 0) {
                continue;  // never showed a recognisable colour: it is not lit to lose
            }
            if (dx * dx + dy * dy < r2 &&
                !std::binary_search(chosen.begin(), chosen.end(), it->first)) {
                ++g_droppedInside;
            }
        }
    }

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
        } else if (rhySignalHookInstalled()) {
            // s30: corona_radius is a live knob. The code patch is one imm32 and
            // rhySignalCoronaApply() is idempotent, so editing the ini moves the
            // game's own flare back and forth without restarting - which is how
            // the A/B is meant to be run (0 = the game's 30 m flare, 0.45 = a
            // bulb, 1.0 = a bigger halo).
            rhySignalCoronaApply(cfg->traffic_corona_radius);
        }
        if (g_apiBroken) {
            g_apiBroken = false;
            g_brokenTick = GetTickCount();
            rhyLog("trafficlights: rhyremix.ini edited, retrying the Remix API");
        }
    }
    const bool hookLive = rhySignalHookInstalled();

    // The player position first: the head-source decision below needs the
    // distance to the nearest signal, and that needs a position.
    float pp[3];
    const RhyPlayerState ps = rhyPlayerPosGrace(
        pp, static_cast<unsigned>(cfg->player_grace_ms), now);
    if (ps != kRhyPlayerOk) {
        ++g_playerGone;
    }

    // n02: how far the nearest entry of the loaded list is, refreshed twice a
    // second (1522 entries is cheap, but not 60 times a second). FLT_MAX when
    // the list is empty, which reads as "nothing to light".
    static DWORD s_nearestTick = 0;
    static float s_nearest = 3.402823466e+38f;
    if (ps == kRhyPlayerOk && (now - s_nearestTick) >= 500u) {
        s_nearestTick = now;
        s_nearest = rhyNearestSignal2D(g_xy.empty() ? nullptr : &g_xy[0],
                                       static_cast<unsigned>(g_xy.size()) / 2u, pp);
    }

    // s30: installed is not the same as working, and n02: "working but silent" is
    // not the same as broken either. The 2026-11-11 13:03 run read records=0 for
    // 12 s at the Grove Street spawn, and that spawn is 79.1 m from the nearest
    // of the 1522 poles while the game's own corona range is 50.0 m (push
    // $0x42480000 at 0x49DCF3, tested at 0x6FC22E inside the callee, after our
    // patch point): the game draws no signal-head corona there either, so the
    // sample proves nothing. Falling back to the list on such a sample would put
    // the light back on the mast for the whole run. So the fallback needs a
    // signal inside the game's own range, the grace over, and still no record -
    // and every answer is logged once with the distance that produced it.
    int decision = kRhyHeadSourceList;
    bool hookProduces = false;
    if (hookLive) {
        const unsigned grace = cfg->traffic_head_grace_ms >= 0
                                   ? static_cast<unsigned>(cfg->traffic_head_grace_ms)
                                   : kHeadSourceGraceMs;
        decision = rhyHeadsHealthy(rhySignalHookRecords(), g_hookTick, now, grace,
                                   s_nearest,
                                   static_cast<float>(kRhySignalCoronaRangeM));
        hookProduces = rhyHeadKeepsHook(decision) != 0;
        if (decision != g_hookDecision) {
            g_hookDecision = decision;
            rhyLog("trafficlights: head source = %s (nearest signal %.1f m, the "
                   "game's own corona range %d m, grace %d ms, %lu ms since the "
                   "hook went live) | hook: calls=%u records=%u dropped=%u bad=%u "
                   "reentrant=%u%s",
                   rhyHeadSourceText(decision), s_nearest,
                   kRhySignalCoronaRangeM, cfg->traffic_head_grace_ms,
                   static_cast<unsigned long>(now - g_hookTick),
                   rhySignalHookCalls(), rhySignalHookRecords(),
                   rhySignalHookDropped(), rhySignalHookBad(),
                   rhySignalHookReentrant(),
                   decision == kRhyHeadSourceList
                       ? " - lighting the list instead. If the game's own flare now "
                         "looks too small, set [trafficlights] corona_radius=0"
                       : "");
        }
    }
    const bool gameMode = (cfg->traffic_source == 1) && hookLive && hookProduces;

    // Drain the hook every frame, even when this module is off or the user has
    // switched to source=list: a full array would otherwise count "dropped"
    // forever and the log line would be unreadable.
    g_recN = rhySignalHookInstalled()
                 ? rhySignalHeadsTake(g_rec, kRhySignalHeadCap)
                 : 0;
    if (!gameMode) {
        g_recN = 0;
    }

    ++g_frame;
    bool layerOk = cfg->traffic_enabled && cfg->traffic_mode != 0 &&
                   !g_apiBroken && ps != kRhyPlayerGone &&
                   (gameMode || !g_lights.empty());
    // n08: half a period of phase against the street layer, so the two list scans
    // cannot share a frame (see streetlights.cpp and smooth.h).
    if (rhyResyncDue(g_frame, static_cast<unsigned>(cfg->traffic_resync_frames),
                     static_cast<unsigned>(cfg->traffic_resync_frames) / 2u)) {
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
    unsigned tl_glowDrawn = 0;
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
                // n02b: both halves of the signal go together - one envelope,
                // one tag, one fade. A signal that kept its glow after its bulb
                // was retired would be the "ambient glow, no bulb" of the
                // 2026-11-12 report back again.
                if (it->second.handle) {
                    rhyDelayDestroy(it->second.handle, kTagTraffic + it->first);
                }
                if (it->second.glow) {
                    rhyDelayDestroy(it->second.glow, kTagTraffic + it->first);
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
                if (it->second.glow) {
                    rhyDelayDestroy(it->second.glow, kTagTraffic + it->first);
                }
                rhyFlushDelayedTag(kTagTraffic + it->first);  // same hash: delayed.h
                remixapi_LightHandle h = createLight(index, phase, *cfg,
                                                     it->second.fade.published,
                                                     kSignalBulb);
                remixapi_LightHandle g = nullptr;
                if (h && glowWanted(*cfg) && canCreateAnother(attempts, frameBudget)) {
                    ++attempts;
                    g = createLight(index, phase, *cfg, it->second.fade.published,
                                    kSignalGlow);
                }
                it->second.handle = h;
                it->second.glow = g;
                if (h) {
                    ++g_fadeSteps;
                } else if (g_apiBroken) {
                    // The old handles are already destroyed by here, so they must
                    // not be drawn again; null makes the next frame ask for new
                    // ones.
                    ++it;
                    break;
                }
            }
        }

        // n02b: a bulb whose glow is still missing (a create our cap or this
        // frame's budget refused, or an ini edit that turned the glow back on)
        // gets the glow on its own. The bulb is NOT destroyed to attach it:
        // destroying a working light to add a second one is the pop s28 removed.
        if (it->second.handle && !it->second.glow && glowWanted(*cfg) &&
            canCreateAnother(attempts, frameBudget)) {
            ++attempts;
            it->second.glow = createLight(index, phase, *cfg,
                                          it->second.fade.published, kSignalGlow);
        }

        bool drawOk = true;
        if (it->second.handle && !rhyDrawLight(it->second.handle)) {
            drawOk = false;
        }
        if (drawOk && it->second.glow && !rhyDrawLight(it->second.glow)) {
            drawOk = false;
        }
        if (!drawOk) {
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
        if (it->second.glow) {
            ++tl_glowDrawn;
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
        st.glow = nullptr;
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
        st.handle = createLight(index, phase, *cfg, st.fade.published, kSignalBulb);
        // n02b: the glow rides with the bulb - same key, same envelope, same
        // phase - so a signal appears as a bulb AND a glow, never as one half.
        if (st.handle && glowWanted(*cfg) && canCreateAnother(attempts, frameBudget)) {
            ++attempts;
            st.glow = createLight(index, phase, *cfg, st.fade.published, kSignalGlow);
        }
        if (st.handle) {
            g_active[key] = st;
        } else if (g_apiBroken) {
            break;
        }
    }

    // --- head lights (source=game) -------------------------------------------
    unsigned hd_drawn = 0;
    unsigned hd_glowDrawn = 0;
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
                if (it->second.glow) {
                    rhyDelayDestroy(it->second.glow,
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
                if (it->second.glow) {
                    rhyDelayDestroy(it->second.glow,
                                    headTag(it->first.id, it->first.phase));
                }
                rhyFlushDelayedTag(headTag(it->first.id, it->first.phase));
                remixapi_LightHandle h = createHeadLight(
                    it->first.id, it->first.phase, pos, *cfg,
                    it->second.fade.published, kSignalBulb);
                remixapi_LightHandle g = nullptr;
                if (h && glowWanted(*cfg) && canCreateAnother(attempts, frameBudget)) {
                    ++attempts;
                    g = createHeadLight(it->first.id, it->first.phase, pos, *cfg,
                                        it->second.fade.published, kSignalGlow);
                }
                it->second.handle = h;
                it->second.glow = g;
                if (h) {
                    ++g_fadeSteps;
                } else if (g_apiBroken) {
                    // both handles were destroyed above: do not draw them again
                    ++it;
                    break;
                }
            }
        }

        // n02b: same glow-only repair as the list path: a head whose second
        // handle is missing gets it without the bulb being destroyed.
        if (it->second.handle && !it->second.glow && glowWanted(*cfg) &&
            canCreateAnother(attempts, frameBudget)) {
            ++attempts;
            it->second.glow = createHeadLight(it->first.id, it->first.phase,
                                              it->second.pos, *cfg,
                                              it->second.fade.published,
                                              kSignalGlow);
        }

        bool headDrawOk = true;
        if (it->second.handle && !rhyDrawLight(it->second.handle)) {
            headDrawOk = false;
        }
        if (headDrawOk && it->second.glow && !rhyDrawLight(it->second.glow)) {
            headDrawOk = false;
        }
        if (!headDrawOk) {
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
        if (it->second.glow) {
            ++hd_glowDrawn;
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
        st.glow = nullptr;
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
                                    st.fade.published, kSignalBulb);
        // n02b: the head gets both halves, same position, same colour the game
        // chose, same envelope: the bulb at the lens and the glow around it.
        if (st.handle && glowWanted(*cfg) && canCreateAnother(attempts, frameBudget)) {
            ++attempts;
            st.glow = createHeadLight(key.id, key.phase, st.pos, *cfg,
                                      st.fade.published, kSignalGlow);
        }
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
            unsigned glows = 0;
            for (std::map<HeadKey, HeadState>::iterator it = g_headLights.begin();
                 it != g_headLights.end(); ++it) {
                if (it->first.phase == 0) {
                    ++green;
                } else if (it->first.phase == 1) {
                    ++amber;
                } else {
                    ++red;
                }
                if (it->second.glow) {
                    ++glows;
                }
            }
            // n02b: bulbs and glows are counted separately, because "every lit
            // head has both" is the thing the 2026-11-12 report was about. A
            // head count with fewer glows than heads names a create that failed.
            rhyLog("trafficlights[source=game]: %u heads lit (%u with a glow), "
                   "%u bulbs + %u glows drawn this frame "
                   "(green=%d amber=%d red=%d, %u coronas registered this frame, "
                   "colour_swaps=%u expired=%u steps=%u retired=%u in_drop=%u "
                   "player_gone_frames=%u) | remix: live=%u create_fail=%u "
                   "draw_fail=%u budget_stop=%u deferred=%u | hook: calls=%u "
                   "records=%u dropped=%u bad=%u reentrant=%u",
                   static_cast<unsigned>(g_headLights.size()), glows, hd_drawn,
                   hd_glowDrawn, green, amber,
                   red, g_recN, g_headSwaps, g_headExpired, g_fadeSteps, g_retired,
                   g_droppedInside, g_playerGone, rhyLiveLights(), rhyCreateFailures(),
                   rhyDrawFailures(), g_budgetStop, rhyCreateDeferred(),
                   rhySignalHookCalls(), rhySignalHookRecords(),
                   rhySignalHookDropped(), rhySignalHookBad(),
                   rhySignalHookReentrant());
        } else {
            int green = 0, amber = 0, red = 0;
            unsigned glows = 0;
            for (std::map<int, SignalState>::iterator it = g_active.begin();
                 it != g_active.end(); ++it) {
                if ((it->first & 3) == 0) {
                    ++green;
                } else if ((it->first & 3) == 1) {
                    ++amber;
                } else {
                    ++red;
                }
                if (it->second.glow) {
                    ++glows;
                }
            }
            rhyLog("trafficlights[%s]: %u active (%u with a glow), "
                   "%u bulbs + %u glows drawn this frame (green=%d "
                   "amber=%d red=%d, %u awaiting destroy, %u in list, hour=%d, "
                   "phase_swaps=%u steps=%u retired=%u in_drop=%u "
                   "player_gone_frames=%u) | "
                   "remix: live=%u create_fail=%u draw_fail=%u budget_stop=%u "
                   "deferred=%u",
                   gameMode ? "list" : (hookLive
                       ? "list, hook live but no head recorded (a signal is inside "
                         "the game's own corona range)"
                       : "list, source=game requested but the hook is not "
                         "installed"),
                   static_cast<unsigned>(g_active.size()), glows, tl_drawn,
                   tl_glowDrawn, green, amber,
                   red, rhyDelayedPending(), static_cast<unsigned>(g_lights.size()),
                   rhyGameHour(), g_phaseSwap, g_fadeSteps, g_retired,
                   g_droppedInside, g_playerGone,
                   rhyLiveLights(), rhyCreateFailures(), rhyDrawFailures(),
                   g_budgetStop, rhyCreateDeferred());
        }
    }
}

