#include "streetlights.h"

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

#include "lamps.h"

#include "remix.h"
#include "config.h"
#include "delayed.h"
#include "game.h"
#include "log.h"
#include "smooth.h"  // s28: fades and rate limits - lighting must never step
#include "solar.h"  // colorTemperatureToRgb for the lamp spectra
#include "sun.h"    // rhyLampsOn: one clock decides both the sun and the lamps

// One lamp light from rhyremix_lights.txt: "type x y z".
struct LampLight {
    int type;
    float x;
    float y;
    float z;
};

static std::vector<LampLight> g_lamps;
// The same positions as a flat [x,y,x,y,...] array: rhySelectLights wants that
// layout, and building it once at load keeps the per-selection cost down (the
// list is 7002 lamps).
static std::vector<float> g_xy;

// One lamp we hold a handle for, plus the envelope that says what its radiance
// is allowed to be this frame. s28: the handle's radiance is base * fade.published,
// and published only ever moves one config step at a time, so a lamp that
// streams in fades up and a lamp that streams out fades down. There is no code
// path left that changes a live lamp's brightness in one frame.
struct LampState {
    remixapi_LightHandle handle;
    RhyFade fade;
};
static std::map<int, LampState> g_active;  // list index -> live light
static std::map<int, float> g_want;        // list index -> target gain (0..1)
static std::vector<int> g_wantOrder;       // nearest first: who gets the next slot

static bool g_inited = false;
static int g_frame = 0;
static bool g_night = false;
static bool g_apiBroken = false;  // a BRIDGE call failed: stop touching the API
static DWORD g_brokenTick = 0;    // when it broke, for the post-mortem log line
static unsigned g_budgetStop = 0;  // creates our OWN cap refused (not a failure)
static unsigned g_kept = 0;        // lights the hysteresis kept this pass
static unsigned g_new = 0;         // lights this pass asked for a new handle for
static unsigned g_cfgRev = 0;     // ini revision we are following
static unsigned g_fadeSteps = 0;  // re-creates done to move a gain one step
static unsigned g_retired = 0;    // lights destroyed after fading to zero
static unsigned g_playerGone = 0; // frames the player ped pointer was unreadable

// A gain at or below this counts as "dark": the handle is retired there rather
// than kept around at zero radiance.
static const float kGainFloor = 0.02f;

// ---------------------------------------------------------------- loading

static void loadList() {
    const RhyConfig* cfg = rhyConfigGet();
    FILE* f = std::fopen(cfg->listPath.c_str(), "rb");
    if (!f) {
        rhyLog("streetlights: cannot open %s", cfg->listPath.c_str());
        return;
    }
    char line[256];
    while (std::fgets(line, sizeof line, f)) {
        char* p = line;
        while (*p == ' ' || *p == '\t') {
            ++p;
        }
        if (*p == '#' || *p == '\r' || *p == '\n' || *p == '\0') {
            continue;
        }
        int type = 0;
        float x = 0.0f, y = 0.0f, z = 0.0f;
        if (std::sscanf(p, "%d %f %f %f", &type, &x, &y, &z) != 4) {
            continue;
        }
        if (type < 1 || type > 12) {
            continue;
        }
        LampLight lamp = {type, x, y, z};
        g_lamps.push_back(lamp);
        g_xy.push_back(x);
        g_xy.push_back(y);
    }
    std::fclose(f);
    rhyLog("streetlights: loaded %u lights from %s",
           static_cast<unsigned>(g_lamps.size()), cfg->listPath.c_str());
}

void rhyStreetlightsInitOnce() {
    if (g_inited) {
        return;
    }
    g_inited = true;
    loadList();
}

// ---------------------------------------------------------------- selection

// Player position and the game clock come from game.cpp, so the lamps, the
// traffic lights and the probe cannot disagree about where the player is or
// what hour it is.

// Night window. Two ways to answer it:
//
//   when=twilight (default) - the sun's elevation. A real street lamp is on a
//     photocell and switches around civil twilight, about -6 degrees. The old
//     on_hour/off_hour window is wrong in both directions across a year: at 19:00
//     in July the sun is still up over Los Santos, and at 6:00 in December it is
//     still dark. It also never matched the moment the shadows actually changed.
//   when=hours - the original behaviour, kept for A/B and for people who want a
//     fixed schedule.
//
// rhyLampsOn() comes from the sky module so the lamps and the sun cannot
// disagree about what time of day it is.
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

// Radiance scale by lamp size: types 1-2/7-8 big, 3-4/9-10 middle, 5-6/11-12 small.
// The band is also what selects the physical fixture size, so it is factored out.
static int sizeBand(int type) {
    return (type >= 1 && type <= 12) ? ((type - 1) % 6) / 2 : 1;
}

static float radianceScale(int type, const RhyConfig& c) {
    int band = sizeBand(type);
    if (band == 0) {
        return static_cast<float>(c.radiance_big);
    }
    if (band == 2) {
        return static_cast<float>(c.radiance_small);
    }
    return static_cast<float>(c.radiance_middle);
}

// Colour of a lamp.
//
// A lamp family name wins over a colour temperature, because these are gas
// discharge lamps and a gas discharge does not have a colour temperature in any
// sense that produces the right picture. High-pressure sodium has a *correlated*
// temperature near 2000 K, but its flux is essentially the sodium D doublet at
// 589 nm: yellow-orange, with almost nothing in the deep red where a 2000 K
// Planck curve puts a third of its energy. Setting kelt_warm=2000 therefore
// makes sodium look like a dim incandescent bulb. See lamps.h.
//
// With no family configured, kelvin wins over the hand-written cold/warm triples,
// because a kelvin number is something a user can look up and get right while an
// arbitrary RGB triple is something to guess at.
static const float* lampColor(int type, const RhyConfig& c) {
    const bool cool = (type & 1) != 0;
    const int fam = cool ? c.lamp_cold_family : c.lamp_warm_family;

    static float famRgb[2][3] = {{1, 1, 1}, {1, 1, 1}};
    static int famSet[2] = {-1, -1};
    if (fam >= 0) {
        const int slot = cool ? 0 : 1;
        if (famSet[slot] != fam) {
            famSet[slot] = fam;
            rhyLampFamilyRgb(fam, famRgb[slot]);
        }
        return famRgb[slot];
    }

    const float kelvin = cool ? c.kelt_cold : c.kelt_warm;
    if (kelvin > 0.0f) {
        // One static per parity: the value only depends on the parity of the
        // type, and recomputing the Planck fit per lamp per create is pointless.
        static float coldRgb[3] = {0.85f, 0.90f, 1.00f};
        static float warmRgb[3] = {1.00f, 0.80f, 0.55f};
        static float coldK = -1.0f, warmK = -1.0f;
        if (cool) {
            if (coldK != kelvin) {
                coldK = kelvin;
                colorTemperatureToRgb(kelvin, coldRgb);
            }
            return coldRgb;
        }
        if (warmK != kelvin) {
            warmK = kelvin;
            colorTemperatureToRgb(kelvin, warmRgb);
        }
        return warmRgb;
    }
    return cool ? c.cold : c.warm;
}

// A lamp's light points straight down: street lamps light the road, not the
// sky. sphere_shaping=0 falls back to an unshaped (all-directions) sphere.
static void applyDownCone(remixapi_LightInfoSphereEXT* sphere,
                          const RhyConfig& c) {
    sphere->shaping_hasvalue = 1;
    sphere->shaping_value.direction.x = 0.0f;
    sphere->shaping_value.direction.y = 0.0f;
    sphere->shaping_value.direction.z = -1.0f;
    sphere->shaping_value.coneAngleDegrees = c.cone_angle;
    sphere->shaping_value.coneSoftness = c.cone_softness;
    sphere->shaping_value.focusExponent = c.focus_exponent;
}

// Which lamps should have a light, and how bright each one is allowed to be.
//
// Two radii and one overshoot, and the gap between them is the whole point:
//
//   max_distance   a lamp the player never had a light for gets one inside this
//   exit_distance  a lamp that ALREADY has a light keeps it out to this
//
// With one radius, a lamp sitting on the boundary is created at one re-selection
// and destroyed at the next: a street light that blinks while you walk past it.
// The same hysteresis applies to the COUNT - the active set grows to
// max_lights + sticky_extra before the farthest lamps are dropped, instead of
// churning at exactly max_lights.
//
// s28 adds the third thing the gap was always meant to do: it is now also the
// FADE band. rhySelectLights returns a target gain of 1 inside max_distance and
// a linear ramp to 0 at exit_distance, so a lamp no longer appears at full
// radiance at 300 m or vanishes at full radiance at 380 m - it brightens as you
// approach and dims as you leave, and the hard cut-off happens where the light
// is already dark. The caps are unchanged.
static void reselect(const RhyConfig& c, const float* pp) {
    g_want.clear();
    g_wantOrder.clear();

    std::vector<int> active;
    active.reserve(g_active.size());
    for (std::map<int, LampState>::iterator it = g_active.begin();
         it != g_active.end(); ++it) {
        active.push_back(it->first);
    }

    static std::vector<RhyWant> want;
    want.resize(g_lamps.size());
    const unsigned n = rhySelectLights(
        g_xy.empty() ? nullptr : &g_xy[0], static_cast<unsigned>(g_lamps.size()),
        active.empty() ? nullptr : &active[0], static_cast<unsigned>(active.size()),
        pp[0], pp[1], static_cast<float>(c.max_distance),
        static_cast<float>(c.exit_distance), static_cast<unsigned>(c.max_lights),
        static_cast<unsigned>(c.sticky_extra), &want[0],
        static_cast<unsigned>(want.size()));

    const float minD = static_cast<float>(c.min_distance);
    for (unsigned i = 0; i < n; ++i) {
        // min_distance is a "do not light the pavement right under CJ" knob. It
        // used to exclude the lamp from the set, which made it vanish in one
        // frame; now it just pins the target gain to 0, so it fades out.
        const float gain = (minD > 0.0f && want[i].dist < minD)
                               ? 0.0f
                               : want[i].gain;
        if (gain <= 0.0f) {
            continue;  // not wanted: the live envelope's target stays 0 and fades
        }
        g_want[want[i].id] = gain;
        g_wantOrder.push_back(want[i].id);
    }
    g_kept = 0;
    g_new = 0;
    for (size_t i = 0; i < g_wantOrder.size(); ++i) {
        if (g_active.find(g_wantOrder[i]) != g_active.end()) {
            ++g_kept;
        } else {
            ++g_new;
        }
    }
}

// ---------------------------------------------------------------- create / destroy

static remixapi_LightHandle createLight(int index, const RhyConfig& c, float gain) {
    const remixapi_Interface* api = rhyRemixApi();
    if (!api || !api->CreateLight) {
        return nullptr;
    }
    const LampLight& L = g_lamps[index];

    static remixapi_LightInfo info;
    static remixapi_LightInfoSphereEXT sphere;

    const int band = sizeBand(L.type);
    // Physical fixture size, with radiance rescaled to hold total flux constant.
    // Without the rescale, replacing the shipped 15 game unit sphere (a 30 metre
    // ball) with a real 0.2 m luminaire head would cut the light by ~2500x and
    // the street would go black - which is why the radius had never been fixed.
    const float radius = c.fixture_size ? rhyStreetRadius(band, c.fixture_scale)
                                        : c.radius;
    const float scale = rhyRadianceForFlux(radianceScale(L.type, c), radius,
                                           c.radius_ref) * c.radiance_scale;
    const float* col = lampColor(L.type, c);  // family, kelvin, or cold/warm

    std::memset(&info, 0, sizeof info);
    info.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
    // LightInfo::hash keys the light in the Remix runtime. 0 = anonymous light;
    // a non-zero base makes each lamp addressable from a Remix mod.
    info.hash = c.light_hash_base + static_cast<uint64_t>(index);
    // s28: the radiance a handle carries is the fixture's radiance TIMES the
    // envelope's published gain. Fading a lamp in or out is a sequence of these
    // calls, each one a step closer to the target gain - never one jump.
    if (!(gain > 0.0f)) {
        gain = 0.0f;
    }
    if (gain > 1.0f) {
        gain = 1.0f;
    }
    info.radiance.x = col[0] * scale * gain;
    info.radiance.y = col[1] * scale * gain;
    info.radiance.z = col[2] * scale * gain;

    std::memset(&sphere, 0, sizeof sphere);
    sphere.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_SPHERE_EXT;
    sphere.pNext = nullptr;
    sphere.position.x = L.x;
    sphere.position.y = L.y;
    sphere.position.z = L.z;
    sphere.radius = radius;
    if (c.sphere_shaping || L.type <= 6) {  // cone shaping, straight down
        applyDownCone(&sphere, c);
        if (c.fixture_size) {
            // The fixture's own distribution, not a single global cone. A
            // roadway luminaire floods the pavement at ~125 degrees; a
            // post-top pedestrian luminaire is wider and softer still.
            RhyFixture f;
            rhyStreetFixture(band, &f);
            sphere.shaping_value.coneAngleDegrees = f.coneDeg;
            sphere.shaping_value.coneSoftness = f.coneSoftness;
        }
    } else {
        sphere.shaping_hasvalue = 0;
    }
    sphere.volumetricRadianceScale = 0.0f;
    info.pNext = &sphere;

    int reason = 0;  // remix.h: 1 = our cap, 2 = the bridge, 3 = frame budget
    const remixapi_LightHandle handle = rhyCreateLight(&info, &reason);
    if (!handle) {
        if (reason == 2) {
            g_apiBroken = true;
            g_brokenTick = GetTickCount();
            rhyLog("streetlights: CreateLight refused by the bridge #%d (bridge "
                   "create failures so far: %u), streetlights off until the next "
                   "rhyremix.ini edit", index, rhyCreateFailures());
            return nullptr;
        }
        if (reason == 3) {
            return nullptr;  // frame budget spent: this fade step runs next frame
        }
        // The cap is a config value, not a broken light: the module stays on and
        // the ini gets the blame in the log. This is what stopped one refused
        // create from switching the whole street off for the rest of the session.
        if (++g_budgetStop <= 3) {
            rhyLog("streetlights: no slot for lamp #%d, max_total_lights reached "
                   "(live=%u, refusals so far: %u): raise max_total_lights or "
                   "lower max_lights in rhyremix.ini", index, rhyLiveLights(),
                   g_budgetStop);
        }
        return nullptr;
    }
    // Log the exact numbers of the first few lights once: this is what to
    // compare against probe_radius / probe_radiance when tuning brightness.
    static int createdLogs = 0;
    if (createdLogs < 3) {
        ++createdLogs;
        rhyLog("streetlights: CreateLight ok #%d hash=%llu pos=%.1f,%.1f,%.1f "
               "radiance=%.2f,%.2f,%.2f gain=%.2f radius=%.2f cone=%.1f/%.2f focus=%.2f",
               index, info.hash, sphere.position.x, sphere.position.y,
               sphere.position.z, info.radiance.x, info.radiance.y,
               info.radiance.z, gain, sphere.radius,
               sphere.shaping_value.coneAngleDegrees,
               sphere.shaping_value.coneSoftness,
               sphere.shaping_value.focusExponent);
    }
    return handle;
}

// s28: there is no destroyAll() any more. Day, the player pointer going to 0, or
// the module being switched off all set every target gain to 0; the envelope then
// ramps each lamp down over fade_out_ms and retires the handle when it reaches
// dark. That is the difference between "the whole world went dark" and a street
// going off.
static void fadeAllOut(const char* why) {
    if (!g_want.empty() || !g_wantOrder.empty()) {
        rhyLog("streetlights: fading %u lights out (%s)",
               static_cast<unsigned>(g_active.size()), why);
    }
    g_want.clear();
    g_wantOrder.clear();
}

// Retire one lamp's handle now that its envelope has reached dark.
static void retireLight(std::map<int, LampState>::iterator it) {
    if (it->second.handle) {
        rhyDelayDestroy(it->second.handle, kTagStreet + it->first);
    }
    g_active.erase(it);
    ++g_retired;
}

// Replace a live handle with one carrying the envelope's new published gain.
// Same hash, so the old handle's destroy has to land first (delayed.h). The
// create is checked for room BEFORE the old handle is destroyed: a re-create that
// destroys first and then gets refused by the cap or the frame budget leaves a
// light that is simply gone, which is the pop this whole task is about.
static remixapi_LightHandle recreateLight(int index, const RhyConfig& c,
                                          remixapi_LightHandle old, float gain) {
    if (!rhyCanCreateLight()) {
        return old;  // keep the handle we have; the step runs when there is room
    }
    if (old) {
        rhyDelayDestroy(old, kTagStreet + index);
    }
    rhyFlushDelayedTag(kTagStreet + index);  // destroy-before-create: same hash
    return createLight(index, c, gain);  // nullptr only on a bridge/cap refusal
}

// ---------------------------------------------------------------- per frame

void rhyStreetlightsFrame() {
    const remixapi_Interface* api = rhyRemixApi();
    if (!api || !api->DrawLightInstance) {
        return;
    }
    const RhyConfig* cfg = rhyConfigGet();

    const DWORD now = GetTickCount();
    static DWORD s_lastTick = 0;
    float dt = s_lastTick ? static_cast<float>(now - s_lastTick) / 1000.0f : 0.016f;
    if (dt > 0.25f) {
        dt = 0.25f;  // a paused game or a hitch must not fast-forward a fade
    }
    if (dt < 0.0f) {
        dt = 0.0f;
    }
    s_lastTick = now;

    // A rejected CreateLight or DrawLightInstance switches this module off for
    // the session; a fresh rhyremix.ini revision is the signal to try again, so
    // tuning the ini live can recover from a bad experiment.
    if (g_apiBroken && g_cfgRev != rhyConfigRevision()) {
        g_apiBroken = false;
        g_cfgRev = rhyConfigRevision();
        rhyLog("streetlights: rhyremix.ini edited, retrying the Remix API");
    }

    float pp[3];
    const RhyPlayerState ps = rhyPlayerPosGrace(
        pp, static_cast<unsigned>(cfg->player_grace_ms), now);
    if (ps != kRhyPlayerOk) {
        ++g_playerGone;
    }

    ++g_frame;
    if ((g_frame % cfg->resync_frames) == 1) {  // every resync_frames, first too
        g_night = isNight(rhyGameHour(), *cfg);
        if (!cfg->enabled || g_lamps.empty() || !g_night || g_apiBroken ||
            ps == kRhyPlayerGone) {
            // Not "destroy everything": the wanted set goes empty and every live
            // light's target goes to 0, which the envelope turns into a ramp.
            fadeAllOut(!cfg->enabled ? "disabled" : g_lamps.empty()
                           ? "empty list" : !g_night ? "daylight"
                           : g_apiBroken ? "bridge refused a call"
                                         : "no player ped");
        } else {
            reselect(*cfg, pp);
        }
    }

    // --- advance every live light, then pay for the steps the frame can afford.
    // Order matters: rhyFadeAdvance runs for every light whether or not a create
    // is affordable, but rhyFadeApply (which moves the published gain) only runs
    // when one is, so a frame with no budget left keeps the handle it has rather
    // than jumping.
    //
    // `attempts` counts handle-change ATTEMPTS, not successes. rhyCreateBudgetLeft()
    // only counts the creates the bridge accepted, so on its own a create that the
    // light cap refuses is free, and a frame would then make 320 bridge calls that
    // all fail. The two limits together bound the frame.
    unsigned drawn = 0;
    unsigned stepped = 0;
    unsigned attempts = 0;
    const unsigned frameBudget = cfg->creates_per_frame
                                     ? static_cast<unsigned>(cfg->creates_per_frame)
                                     : 0u;  // 0 = no per-frame limit
    for (std::map<int, LampState>::iterator it = g_active.begin();
         it != g_active.end();) {
        std::map<int, float>::iterator w = g_want.find(it->first);
        it->second.fade.target = (w != g_want.end()) ? w->second : 0.0f;
        rhyFadeAdvance(it->second.fade, dt);

        // A light in g_active with no handle is a create that failed (cap, or a
        // bridge refusal that did not break the module). It is not a zombie: it
        // asks for a handle every frame until it gets one or fades to dark.
        const bool needsHandle = !it->second.handle;
        if ((needsHandle || rhyFadeDue(it->second.fade, cfg->fade_step, kGainFloor)) &&
            (frameBudget == 0 || attempts < frameBudget) && rhyCreateBudgetLeft()) {
            ++attempts;
            const RhyFadeOp op = rhyFadeApply(it->second.fade, cfg->fade_step,
                                              kGainFloor);
            if (op == kRhyFadeRetire || op == kRhyFadeNone) {
                if (op == kRhyFadeRetire) {
                    // A destroy counts against the frame's budget too: that is
                    // what stops a finished fade from removing 300 lights in
                    // one frame (see remix.h).
                    rhySpendCreateSlot();
                    retireLight(it++);
                    continue;
                }
            } else {
                remixapi_LightHandle h = recreateLight(
                    it->first, *cfg, it->second.handle, it->second.fade.published);
                if (h) {
                    it->second.handle = h;
                    ++g_fadeSteps;
                    ++stepped;
                } else {
                    // recreateLight only returns null when it already destroyed the
                    // old handle (a bridge or cap refusal after the pre-check). Clear
                    // it: keeping a destroyed handle here is a use-after-free on the
                    // next draw, and null makes the next frame ask for a new one.
                    it->second.handle = nullptr;
                    if (g_apiBroken) {
                        ++it;
                        break;  // the draw loop below is skipped by the check
                    }
                }
            }
        }

        if (!it->second.handle) {
            ++it;
            continue;
        }
        if (!rhyDrawLight(it->second.handle)) {
            // The bridge hands back a handle for a light the Remix server never
            // took, so the draw is where a rejected create finally shows up.
            // One bad light usually means the whole set is bad; do not keep
            // hammering it every frame. The handles still fade out from here.
            g_apiBroken = true;
            g_brokenTick = GetTickCount();
            rhyLog("streetlights: DrawLightInstance rejected for #%d "
                   "(draw failures so far: %u, live handles: %u), streetlights "
                   "off until the next rhyremix.ini edit",
                   it->first, rhyDrawFailures(), rhyLiveLights());
            break;
        }
        ++drawn;
        ++it;
    }

    // --- lights the selection wants that do not have a handle yet: nearest
    // first, so the near field fades in smoothly even when the frame budget is
    // the binding constraint.
    unsigned created = 0;
    for (size_t i = 0; i < g_wantOrder.size(); ++i) {
        const int index = g_wantOrder[i];
        if (g_active.find(index) != g_active.end()) {
            continue;
        }
        if (frameBudget != 0 && attempts >= frameBudget) {
            break;
        }
        if (!rhyCreateBudgetLeft()) {
            break;
        }
        std::map<int, float>::iterator w = g_want.find(index);
        if (w == g_want.end() || w->second <= 0.0f) {
            continue;
        }
        LampState st;
        st.handle = nullptr;
        rhyFadeInit(st.fade, static_cast<float>(cfg->fade_in_ms) / 1000.0f,
                    static_cast<float>(cfg->fade_out_ms) / 1000.0f);
        st.fade.target = w->second;
        rhyFadeAdvance(st.fade, dt);
        if (!rhyFadeDue(st.fade, cfg->fade_step, kGainFloor)) {
            continue;  // not bright enough yet to be worth a handle this frame
        }
        const RhyFadeOp op = rhyFadeApply(st.fade, cfg->fade_step, kGainFloor);
        if (op != kRhyFadeCreate) {
            continue;
        }
        ++attempts;
        rhyFlushDelayedTag(kTagStreet + index);  // destroy-before-create: same hash
        st.handle = createLight(index, *cfg, st.fade.published);
        if (st.handle) {
            g_active[index] = st;
            ++created;
        } else if (g_apiBroken) {
            break;
        }
    }

    static DWORD g_lastCountTick = 0;
    if (now - g_lastCountTick >= 10000) {  // active count once per 10 s
        g_lastCountTick = now;
        // drawn vs live is the diagnostic that matters: "active" only counts
        // handles we hold, live counts what the bridge gave us and never gave
        // back, and the draw failure counter is the only evidence of a light the
        // renderer actually refused. `steps` is the fade traffic: how many handles
        // were re-created this pass to move a gain one step.
        rhyLog("streetlights: %u active, %u drawn this frame, %u awaiting "
               "destroy (hour=%d, %u in list, kept=%u new=%u steps=%u created=%u "
               "retired=%u player_gone_frames=%u) | remix: live=%u create_fail=%u "
               "draw_fail=%u budget_stop=%u deferred=%u",
               static_cast<unsigned>(g_active.size()), drawn,
               rhyDelayedPending(), rhyGameHour(),
               static_cast<unsigned>(g_lamps.size()), g_kept, g_new, stepped,
               created, g_retired, g_playerGone, rhyLiveLights(),
               rhyCreateFailures(), rhyDrawFailures(), g_budgetStop,
               rhyCreateDeferred());
    }
}
