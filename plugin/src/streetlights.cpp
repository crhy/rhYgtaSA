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

// One lamp light from rhyremix_lights.txt.
//   v1 line: "type x y z"                     - the third-party lamp table
//   v2 line: "type x y z r g b family range"  - the game's own 2dFx record
// v2 is the default since n11 and is what tools/lamp-coverage.py measures: the
// v1 table covers 2617 of the 3536 street lights of Los Santos (74.0%), the 2dFx
// list covers all of them.
struct LampLight {
    int type;
    float x;
    float y;
    float z;
    // v2 only. rgb is the colour the model's own 2dFx LIGHT record carries for
    // this bulb; family is the lamp family the fitting belongs to (-1 = the line
    // is a v1 line and the ini's cold/warm rule decides the colour); range is the
    // record's own range float in game units, kept for the log and the coverage
    // comparison - the fixture size comes from the type column, not from it.
    int rgb[3];
    int family;
    float range;
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
//
// n11 adds the second handle of the same lamp: the LUMINAIRE (the housing, a
// downward cone, the pool of light on the road) and the HEAD (the bulb, small,
// unshaped, nits-based, the thing you can see). rhY's 2026-11-12c review named
// both halves missing at once - "no lit bulb, no pool of light under them" - and
// a Remix light is not visible geometry, so one handle cannot be both. Each has
// its own envelope because each has its own distance band.
struct LampState {
    remixapi_LightHandle handle;  // the luminaire
    remixapi_LightHandle head;    // the bulb; 0 = not wanted or not created yet
    RhyFade fade;
    RhyFade hfade;
};
static std::map<int, LampState> g_active;  // list index -> live light
static std::map<int, float> g_want;        // list index -> target gain (0..1)
// n11: which bulbs get a head light this pass, and at what gain. Its own band
// (head_distance..head_exit_distance) and its own cap (head_max_lights).
static std::map<int, float> g_headWant;
static std::vector<int> g_wantOrder;       // nearest first: who gets the next slot

static bool g_inited = false;
static int g_frame = 0;
static bool g_night = false;
static bool g_apiBroken = false;  // a BRIDGE call failed: stop touching the API
static DWORD g_brokenTick = 0;    // when it broke, for the post-mortem log line
static unsigned g_budgetStop = 0;  // creates our OWN cap refused (not a failure)
static unsigned g_kept = 0;        // lights the hysteresis kept this pass
// n04: cumulative count of lamps that had a live handle, were inside
// max_distance, and were NOT wanted by the last re-selection - a street light
// switched off while it is still in front of the player, which is finding A.
// It should stay 0 for a whole run; it is printed in the 10 s status line.
static unsigned g_droppedInside = 0;
static unsigned g_new = 0;         // lights this pass asked for a new handle for
static unsigned g_cfgRev = 0;     // ini revision we are following
static unsigned g_fadeSteps = 0;  // re-creates done to move a gain one step
static unsigned g_retired = 0;    // lights destroyed after fading to zero
static unsigned g_playerGone = 0; // frames the player ped pointer was unreadable
// n11: the head (bulb) layer's own counters, so the log can say how many bulbs
// are lit and not just how many poles are.
static unsigned g_headCreated = 0;
static unsigned g_headRetired = 0;
static unsigned g_headSteps = 0;
static unsigned g_headPeak = 0;
static unsigned g_v2Lines = 0;
static unsigned g_badFamily = 0;   // v2 lines whose family name we do not know

// The head light of lamp #i is a separate Remix light, so it needs its own hash
// (the hash keys the light in the Remix runtime) and its own delayed-destroy tag
// (delayed.h matches on the tag alone). The list is under 10000 entries, so an
// offset of a million cannot collide with a luminaire hash.
static const unsigned long long kHeadHashOffset = 1000000ull;

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
        LampLight lamp = {type, x, y, z, {0, 0, 0}, -1, 0.0f};
        // A v2 line carries the bulb's own colour, its lamp family and the 2dFx
        // record's range. The family is a string, so the tail is scanned rather
        // than assumed: a line that does not match is a v1 line and keeps the
        // old colour rule. A family name the plugin does not know is logged once
        // rather than dropped - the lamp still gets a light.
        char fam[32];
        fam[0] = '\0';
        int rr = 0, gg = 0, bb = 0;
        float rng = 0.0f;
        if (std::sscanf(p, "%*d %*f %*f %*f %d %d %d %31s %f",
                        &rr, &gg, &bb, fam, &rng) == 5) {
            lamp.rgb[0] = rr;
            lamp.rgb[1] = gg;
            lamp.rgb[2] = bb;
            lamp.family = rhyLampFamilyFromName(fam);
            lamp.range = rng;
            ++g_v2Lines;
            if (lamp.family < 0) {
                ++g_badFamily;
            }
        }
        g_lamps.push_back(lamp);
        g_xy.push_back(x);
        g_xy.push_back(y);
    }
    std::fclose(f);
    rhyLog("streetlights: loaded %u lights from %s (%u v2 lines with a 2dFx colour, "
           "%u v1 lines, %u unknown family names)",
           static_cast<unsigned>(g_lamps.size()), cfg->listPath.c_str(),
           g_v2Lines, static_cast<unsigned>(g_lamps.size()) - g_v2Lines, g_badFamily);
    if (!g_lamps.empty()) {
        const LampLight& L = g_lamps[0];
        rhyLog("streetlights: first lamp type=%d pos=%.2f,%.2f,%.2f rgb=%d,%d,%d "
               "family=%s range=%.1f", L.type, L.x, L.y, L.z, L.rgb[0], L.rgb[1],
               L.rgb[2], L.family >= 0 ? rhyLampFamilyName(L.family) : "ini",
               L.range);
    }

    // n04: does the cap cover the radius? Measured once here, over every 8th lamp
    // as a stand-in for "everywhere the player can stand": the worst number of
    // lamps within max_distance of a point. If it is above max_lights, the cap is
    // evicting lamps that are still inside the radius, and the fix is max_lights
    // (or a smaller max_distance) - not a smaller cap. This is the number the ini
    // comment quotes, so it is worth printing rather than asserting offline.
    if (!g_lamps.empty() && cfg->max_distance > 0) {
        const float r2 = static_cast<float>(cfg->max_distance) *
                         static_cast<float>(cfg->max_distance);
        const float hr2 = static_cast<float>(cfg->head_distance) *
                          static_cast<float>(cfg->head_distance);
        unsigned worst = 0;
        unsigned worstHead = 0;
        for (size_t i = 0; i < g_lamps.size(); i += 8) {
            unsigned within = 0;
            unsigned withinHead = 0;
            for (size_t j = 0; j < g_lamps.size(); ++j) {
                const float dx = g_lamps[j].x - g_lamps[i].x;
                const float dy = g_lamps[j].y - g_lamps[i].y;
                const float d2 = dx * dx + dy * dy;
                if (d2 <= r2) {
                    ++within;
                }
                if (d2 <= hr2) {
                    ++withinHead;
                }
            }
            if (within > worst) {
                worst = within;
            }
            if (withinHead > worstHead) {
                worstHead = withinHead;
            }
        }
        rhyLog("streetlights: worst case %u lamps within max_distance=%d of a "
               "point (cap max_lights+sticky_extra=%d): %s",
               worst, cfg->max_distance, cfg->max_lights + cfg->sticky_extra,
               worst > static_cast<unsigned>(cfg->max_lights + cfg->sticky_extra)
                   ? "the cap will trim lamps in the far band only, and the "
                     "nearest lamps are never evicted"
                   : "the cap covers the radius");
        rhyLog("streetlights: worst case %u bulbs within head_distance=%d of a "
               "point (cap head_max_lights=%d)", worstHead, cfg->head_distance,
               cfg->head_max_lights);
    }
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

// n11: the colour of one lamp, v2 or v1.
//
// A v2 line carries the colour the model's own 2dFx record stores for this exact
// bulb, plus the family of the fitting. That is the whole point of the 2dFx list:
// the sodium post, the white globe post on the boardwalk and the red crossing
// lamp are three different colours, and one cold/warm ini rule for the whole
// world cannot say so. rhyLampTintedColour linearises the bytes (they are display
// values, Remix wants linear) and falls back to the family's spectrum when the
// bytes are plain white, which is where the fitting has to decide.
//
// Cross-check, measured: the game's own sodium lamps agree with the published
// HPS chromaticity once linearised - lamppost3 records (255,148,52) -> linear
// (1.000, 0.297, 0.034) against rhyLampFamilyRgb(HPS) = (1.000, 0.254, 0.003).
static const float* lampColorFor(const LampLight& L, const RhyConfig& c) {
    if (L.family < 0) {
        return lampColor(L.type, c);
    }
    static float tint[3] = {1.0f, 1.0f, 1.0f};
    rhyLampTintedColour(L.family, L.rgb, tint);
    return tint;
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
        static_cast<unsigned>(want.size()),
        // n04: the cap is a budget for the band beyond max_distance, not a licence
        // to drop a lamp because 408 others are nearer. Measured on the real
        // 7002-lamp list: the worst case within 300 units of a point is 408 lamps
        // and within 380 units is 552, against a shipped cap of 320+90 - so in a
        // dense district the cap was evicting lamps that were still 200 m away,
        // and the log shows the set jumping by 193 and by 357 handles.
        static_cast<float>(c.max_distance));

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

    // n04: the field proof of the guarantee. A lamp that holds a handle, is
    // inside max_distance, and is not wanted is a light being taken away from a
    // lamp that is still in front of the player. The hardRadius rule is supposed
    // to make this impossible; the counter says whether it did, in a real run,
    // without anyone having to watch a lamp.
    const float r2 =
        static_cast<float>(c.max_distance) * static_cast<float>(c.max_distance);
    for (std::map<int, LampState>::iterator it = g_active.begin();
         it != g_active.end(); ++it) {
        if (g_want.find(it->first) != g_want.end()) {
            continue;
        }
        const float dx = g_lamps[it->first].x - pp[0];
        const float dy = g_lamps[it->first].y - pp[1];
        if (dx * dx + dy * dy < r2) {
            ++g_droppedInside;
        }
    }

    // n11: the bulb layer, same rule, smaller band, separate cap. A 0.1 m globe
    // is a couple of pixels at 120 m and nothing at 300, so the head light runs
    // on head_distance..head_exit_distance; the luminaire set above is untouched.
    // The `active` array is the lamps that already hold a HEAD handle, which is
    // what the hysteresis is about for this layer.
    g_headWant.clear();
    if (!c.head_glow || c.head_radiance <= 0.0f || c.head_distance <= 0) {
        return;
    }
    static std::vector<int> headActive;
    headActive.clear();
    for (std::map<int, LampState>::iterator it = g_active.begin();
         it != g_active.end(); ++it) {
        if (it->second.head) {
            headActive.push_back(it->first);
        }
    }
    const unsigned hn = rhySelectLights(
        g_xy.empty() ? nullptr : &g_xy[0], static_cast<unsigned>(g_lamps.size()),
        headActive.empty() ? nullptr : &headActive[0],
        static_cast<unsigned>(headActive.size()), pp[0], pp[1],
        static_cast<float>(c.head_distance),
        static_cast<float>(c.head_exit_distance),
        static_cast<unsigned>(c.head_max_lights), 40u, &want[0],
        static_cast<unsigned>(want.size()), static_cast<float>(c.head_distance));
    for (unsigned i = 0; i < hn; ++i) {
        if (want[i].gain > 0.0f) {
            g_headWant[want[i].id] = want[i].gain;
        }
    }
}

// ---------------------------------------------------------------- create / destroy

static remixapi_LightHandle createLight(int index, const RhyConfig& c, float gain,
                                        int head) {
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
    //
    // The HEAD is not flux-preserved, deliberately. rhyRadianceForFlux exists so
    // a shrunken street luminaire keeps lighting the street; applied to a bulb it
    // turns the bulb into a flood lamp, which is the exact mistake s30 was
    // diagnosed with for signal heads ("a 0.10 m / 1200-nit / flux-67 sphere is a
    // lens you can look at and it colours nothing"). The head carries nits at its
    // own radius; the luminaire carries the flux.
    float radius = 0.0f;
    float scale = 0.0f;
    if (head) {
        RhyFixture f;
        rhyLampHeadFixture(band, &f);
        radius = f.radius * (c.head_scale > 0.0f ? c.head_scale : 1.0f);
        scale = rhyLampHeadRadiance(c.head_radiance, radius, c.radiance_scale);
    } else {
        radius = c.fixture_size ? rhyStreetRadius(band, c.fixture_scale)
                                : c.radius;
        scale = rhyRadianceForFlux(radianceScale(L.type, c), radius,
                                   c.radius_ref) * c.radiance_scale;
    }
    const float* col = lampColorFor(L, c);  // 2dFx colour + family, or the ini rule

    std::memset(&info, 0, sizeof info);
    info.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
    // LightInfo::hash keys the light in the Remix runtime. 0 = anonymous light;
    // a non-zero base makes each lamp addressable from a Remix mod. The head of
    // lamp #i is a different light from the housing of lamp #i, so it gets its
    // own hash (see kHeadHashOffset).
    info.hash = c.light_hash_base +
                (head ? kHeadHashOffset : 0ull) + static_cast<uint64_t>(index);
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
    if (head) {
        // No shaping at all. A bulb shines every way, and the 125 degree cone
        // the luminaire needs at the road is the opposite of what makes a head
        // read as lit when you look at it from the side.
        sphere.shaping_hasvalue = 0;
    } else if (c.sphere_shaping || L.type <= 6) {  // cone shaping, straight down
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
        rhyLog("streetlights: CreateLight ok #%d %s hash=%llu pos=%.1f,%.1f,%.1f "
               "radiance=%.2f,%.2f,%.2f gain=%.2f radius=%.2f cone=%.1f/%.2f focus=%.2f",
               index, head ? "HEAD" : "luminaire", info.hash, sphere.position.x,
               sphere.position.y, sphere.position.z, info.radiance.x, info.radiance.y,
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
    g_headWant.clear();   // n11: the bulbs fade out with their lamps
}

// Retire one lamp's handle now that its envelope has reached dark.
static void retireLight(std::map<int, LampState>::iterator it) {
    if (it->second.handle) {
        rhyDelayDestroy(it->second.handle, kTagStreet + it->first);
    }
    // n11: the bulb goes with its lamp. Its own envelope may still be above zero
    // when the luminaire's reaches dark (the bands differ), but a head light with
    // no lamp under it is a floating glow, and the head band is inside the
    // luminaire band, so this only ever fires when the whole lamp is going off.
    if (it->second.head) {
        rhyDelayDestroy(it->second.head, kTagStreetHead + it->first);
        ++g_headRetired;
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
    return createLight(index, c, gain, 0);  // nullptr only on a bridge/cap refusal
}

// n11: the same for the bulb. Its hash namespace is kHeadHashOffset away from
// the luminaire's, so the destroy-before-create rule needs its own tag.
static remixapi_LightHandle recreateHeadLight(int index, const RhyConfig& c,
                                              remixapi_LightHandle old, float gain) {
    if (!rhyCanCreateLight()) {
        return old;
    }
    if (old) {
        rhyDelayDestroy(old, kTagStreetHead + index);
    }
    rhyFlushDelayedTag(kTagStreetHead + index);
    return createLight(index, c, gain, 1);
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
    // n08: phase 0. The street layer keeps the frame the old `frame % 30 == 1`
    // used, and the signal layer moves half a period away so the two full list
    // scans (11.7 us over the 7002-lamp street list, 8.0 us over the 1522-entry
    // signal list, measured) never land on the same frame again.
    if (rhyResyncDue(g_frame, static_cast<unsigned>(cfg->resync_frames), 0u)) {
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

        // n11: the same lamp's BULB gets its own envelope, advanced every frame
        // like the luminaire's. The gate below is deliberately narrow: a lamp
        // outside the head band with no head handle must not spend one of the
        // frame's create attempts on a no-op, which would starve the luminaire
        // layer (creates_per_frame is the binding constraint in a dense street).
        const bool headLive = it->second.head != nullptr;
        std::map<int, float>::iterator hw = g_headWant.find(it->first);
        const bool headWanted = hw != g_headWant.end();
        it->second.hfade.target = headWanted ? hw->second : 0.0f;
        rhyFadeAdvance(it->second.hfade, dt);
        if ((headLive || headWanted) &&
            (headLive || rhyFadeDue(it->second.hfade, cfg->fade_step, kGainFloor)) &&
            (frameBudget == 0 || attempts < frameBudget) && rhyCreateBudgetLeft()) {
            ++attempts;
            const RhyFadeOp hop = rhyFadeApply(it->second.hfade, cfg->fade_step,
                                               kGainFloor);
            if (hop == kRhyFadeRetire) {
                rhySpendCreateSlot();  // a destroy is budgeted like a create (n08)
                if (headLive) {
                    rhyDelayDestroy(it->second.head, kTagStreetHead + it->first);
                    it->second.head = nullptr;
                    ++g_headRetired;
                }
            } else if (hop == kRhyFadeCreate || hop == kRhyFadeRecreate) {
                remixapi_LightHandle hh = recreateHeadLight(
                    it->first, *cfg, it->second.head, it->second.hfade.published);
                if (hh) {
                    if (!it->second.head) {
                        ++g_headCreated;
                    }
                    it->second.head = hh;
                    ++g_headSteps;
                } else {
                    // Same rule as the luminaire path: recreateHeadLight has
                    // already destroyed the old handle when it returns null, so
                    // keeping it here would be a use-after-free on the next draw.
                    it->second.head = nullptr;
                    if (g_apiBroken) {
                        ++it;
                        break;
                    }
                }
            }
        }

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
        // n11: the bulb of the same lamp is drawn with it. A head light that the
        // renderer refuses is the same class of failure as the housing's, so it
        // takes the module off in the same way rather than being retried.
        if (it->second.head) {
            if (!rhyDrawLight(it->second.head)) {
                g_apiBroken = true;
                g_brokenTick = GetTickCount();
                rhyLog("streetlights: DrawLightInstance rejected for head #%d "
                       "(draw failures so far: %u, live handles: %u), streetlights "
                       "off until the next rhyremix.ini edit",
                       it->first, rhyDrawFailures(), rhyLiveLights());
                break;
            }
            ++drawn;
        }
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
        st.head = nullptr;
        rhyFadeInit(st.fade, static_cast<float>(cfg->fade_in_ms) / 1000.0f,
                    static_cast<float>(cfg->fade_out_ms) / 1000.0f);
        // n11: the bulb's envelope starts dark and is fed by g_headWant; it is
        // created here so both envelopes of a lamp are always timed the same way.
        rhyFadeInit(st.hfade, static_cast<float>(cfg->fade_in_ms) / 1000.0f,
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
        st.handle = createLight(index, *cfg, st.fade.published, 0);
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
        unsigned headLive = 0;
        for (std::map<int, LampState>::iterator it = g_active.begin();
             it != g_active.end(); ++it) {
            if (it->second.head) {
                ++headLive;
            }
        }
        if (headLive > g_headPeak) {
            g_headPeak = headLive;
        }
        // drawn vs live is the diagnostic that matters: "active" only counts
        // handles we hold, live counts what the bridge gave us and never gave
        // back, and the draw failure counter is the only evidence of a light the
        // renderer actually refused. `steps` is the fade traffic: how many handles
        // were re-created this pass to move a gain one step.
        rhyLog("streetlights: %u active (%u with a lit bulb), %u drawn this frame, "
               "%u awaiting destroy (hour=%d, %u in list, kept=%u new=%u steps=%u "
               "created=%u retired=%u in_drop=%u player_gone_frames=%u | head: want=%u "
               "created=%u retired=%u steps=%u peak=%u) | remix: live=%u "
               "create_fail=%u draw_fail=%u budget_stop=%u deferred=%u",
               static_cast<unsigned>(g_active.size()), headLive, drawn,
               rhyDelayedPending(), rhyGameHour(),
               static_cast<unsigned>(g_lamps.size()), g_kept, g_new, stepped,
               created, g_retired, g_droppedInside, g_playerGone,
               static_cast<unsigned>(g_headWant.size()), g_headCreated,
               g_headRetired, g_headSteps, g_headPeak, rhyLiveLights(),
               rhyCreateFailures(), rhyDrawFailures(), g_budgetStop,
               rhyCreateDeferred());
    }
}
