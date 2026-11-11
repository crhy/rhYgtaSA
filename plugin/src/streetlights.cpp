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
static std::map<int, remixapi_LightHandle> g_active;  // list index -> handle
static bool g_inited = false;
static int g_frame = 0;
static bool g_night = false;
static bool g_apiBroken = false;  // a BRIDGE call failed: stop touching the API
static DWORD g_brokenTick = 0;    // when it broke, for the post-mortem log line
static unsigned g_budgetStop = 0;  // creates our OWN cap refused (not a failure)
static unsigned g_kept = 0;        // lights the hysteresis kept this pass
static unsigned g_new = 0;         // lights this pass asked for a new handle for
static unsigned g_cfgRev = 0;     // ini revision we are following

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

// Which lamps should have a light.  Two radii and one overshoot, and the gap
// between them is the whole point:
//
//   max_distance   a lamp the player never had a light for gets one inside this
//   exit_distance  a lamp that ALREADY has a light keeps it out to this
//
// With one radius, a lamp sitting on the boundary is created at one re-selection
// and destroyed at the next: a street light that blinks while you walk past it.
// The same hysteresis applies to the COUNT - the active set grows to
// max_lights + sticky_extra before the farthest lamps are dropped, instead of
// churning at exactly max_lights.  g_active is read here (not passed) because
// "already active" is the whole question.
static std::vector<int> chooseLights(const RhyConfig& c, const float* pp) {
    struct Cand {
        float d2;
        int i;
        bool sticky;
    };
    std::vector<Cand> cand;
    const float minD = static_cast<float>(c.min_distance);
    const float maxD = static_cast<float>(c.max_distance);
    const float exitD = static_cast<float>(c.exit_distance);
    const size_t cap = static_cast<size_t>(c.max_lights);
    const size_t grow = cap + static_cast<size_t>(c.sticky_extra);

    for (size_t i = 0; i < g_lamps.size(); ++i) {
        const LampLight& L = g_lamps[i];
        const float dx = L.x - pp[0];
        const float dy = L.y - pp[1];
        const float d2 = dx * dx + dy * dy;
        if (minD > 0.0f && d2 < minD * minD) {
            continue;
        }
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
    for (size_t k = 0; k < cand.size(); ++k) {  // keep what is already lit
        if (cand[k].sticky && chosen.size() < grow) {
            chosen.push_back(cand[k].i);
        }
    }
    for (size_t k = 0; k < cand.size(); ++k) {  // then fill up to cap with new lamps
        if (!cand[k].sticky && chosen.size() < cap) {
            chosen.push_back(cand[k].i);
        }
    }
    return chosen;
}

// ---------------------------------------------------------------- create / destroy

static remixapi_LightHandle createLight(int index, const RhyConfig& c) {
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
    info.radiance.x = col[0] * scale;
    info.radiance.y = col[1] * scale;
    info.radiance.z = col[2] * scale;

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

    int reason = 0;  // see remix.h: 1 = our max_total_lights cap, 2 = the bridge
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
               "radiance=%.2f,%.2f,%.2f radius=%.2f cone=%.1f/%.2f focus=%.2f",
               index, info.hash, sphere.position.x, sphere.position.y,
               sphere.position.z, info.radiance.x, info.radiance.y,
               info.radiance.z, sphere.radius,
               sphere.shaping_value.coneAngleDegrees,
               sphere.shaping_value.coneSoftness,
               sphere.shaping_value.focusExponent);
    }
    return handle;
}

static void destroyAll() {
    for (std::map<int, remixapi_LightHandle>::iterator it = g_active.begin();
         it != g_active.end(); ++it) {
        rhyDelayDestroy(it->second, kTagStreet + it->first);
    }
    g_active.clear();
}

// Re-diff the active set against the chosen set.
static void resync(const RhyConfig& c, const float* pp) {
    g_kept = 0;
    g_new = 0;
    std::vector<int> chosen = chooseLights(c, pp);

    std::map<int, bool> keep;
    for (size_t i = 0; i < chosen.size(); ++i) {
        keep[chosen[i]] = true;
    }

    // Drop lights that are no longer chosen (destroyed later, not now).  The tag
    // is the list index, so the create loop can force this index's destroy to
    // land first: LightInfo::hash is derived from the index, and a destroy that
    // lands after the replacement's create takes the new light with it - a lamp
    // that blinks out and stays out until the next re-selection puts it back.
    for (std::map<int, remixapi_LightHandle>::iterator it = g_active.begin();
         it != g_active.end();) {
        if (keep.find(it->first) == keep.end()) {
            rhyDelayDestroy(it->second, kTagStreet + it->first);
            g_active.erase(it++);
        } else {
            ++it;
        }
    }

    // Create newly chosen lights.
    for (size_t i = 0; i < chosen.size(); ++i) {
        const int index = chosen[i];
        if (g_active.find(index) != g_active.end()) {
            ++g_kept;
            continue;
        }
        ++g_new;
        rhyFlushDelayedTag(kTagStreet + index);  // destroy-before-create: same hash, see delayed.h
        remixapi_LightHandle h = createLight(index, c);
        if (h) {
            g_active[index] = h;
        }
    }
}

// ---------------------------------------------------------------- per frame

void rhyStreetlightsFrame() {
    const remixapi_Interface* api = rhyRemixApi();
    if (!api || !api->DrawLightInstance) {
        return;
    }
    const RhyConfig* cfg = rhyConfigGet();
    if (!cfg->enabled || g_lamps.empty()) {
        destroyAll();
        return;
    }

    float pp[3];
    if (!rhyPlayerPos(pp)) {
        destroyAll();
        return;
    }

    ++g_frame;
    if ((g_frame % cfg->resync_frames) == 1) {  // every resync_frames, first too
        g_night = isNight(rhyGameHour(), *cfg);
        if (!g_night) {
            destroyAll();
        } else {
            resync(*cfg, pp);
        }
    }

    // A rejected CreateLight or DrawLightInstance switches this module off for
    // the session; a fresh rhyremix.ini revision is the signal to try again, so
    // tuning the ini live can recover from a bad experiment.
    if (g_apiBroken && g_cfgRev != rhyConfigRevision()) {
        g_apiBroken = false;
        g_cfgRev = rhyConfigRevision();
        rhyLog("streetlights: rhyremix.ini edited, retrying the Remix API");
    }

    if (!g_night || g_apiBroken) {
        return;
    }
    unsigned drawn = 0;
    for (std::map<int, remixapi_LightHandle>::iterator it = g_active.begin();
         it != g_active.end(); ++it) {
        if (!rhyDrawLight(it->second)) {
            // The bridge hands back a handle for a light the Remix server never
            // took, so the draw is where a rejected create finally shows up.
            // One bad light usually means the whole set is bad; do not keep
            // hammering it every frame.
            g_apiBroken = true;
            g_brokenTick = GetTickCount();
            rhyLog("streetlights: DrawLightInstance rejected for #%d "
                   "(draw failures so far: %u, live handles: %u), streetlights "
                   "off until the next rhyremix.ini edit",
                   it->first, rhyDrawFailures(), rhyLiveLights());
            break;
        }
        ++drawn;
    }

    DWORD now = GetTickCount();
    static DWORD g_lastCountTick = 0;
    if (now - g_lastCountTick >= 10000) {  // active count once per 10 s
        g_lastCountTick = now;
        // drawn vs live is the diagnostic that matters: "active" only counts
        // handles we hold, live counts what the bridge gave us and never gave
        // back, and the draw failure counter is the only evidence of a light the
        // renderer actually refused.
        rhyLog("streetlights: %u active, %u drawn this frame, %u awaiting "
               "destroy (hour=%d, %u in list, kept=%u new=%u) | remix: live=%u "
               "create_fail=%u draw_fail=%u budget_stop=%u",
               static_cast<unsigned>(g_active.size()), drawn,
               rhyDelayedPending(), rhyGameHour(),
               static_cast<unsigned>(g_lamps.size()), g_kept, g_new, rhyLiveLights(),
               rhyCreateFailures(), rhyDrawFailures(), g_budgetStop);
    }
}
