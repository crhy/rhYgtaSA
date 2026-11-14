// n12: read the game's corona store, turn the car's own light dummies into Remix
// lights. Pure decisions live in vehiclelights.cpp; this file is the part that
// touches the game's memory and the Remix bridge.

#include "vehiclecorona.h"

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <vector>

#include "config.h"
#include "delayed.h"
#include "game.h"
#include "log.h"
#include "remix.h"
#include "signalheads.h"  // rhyRangePlausible + the page cache
#include "smooth.h"
#include "vehiclelights.h"

// ---------------------------------------------------------------------------
// the store, measured out of the exe (see vehiclelights.h for the call sites)
// ---------------------------------------------------------------------------
enum : uint32_t {
    kRhyCoronaCount = 0xC3E038,   // dword, 0..64 (0x6FC35D/0x6FC36F inc/store)
    kRhyCoronaTexTable = 0xC3E000,  // 11 sprite pointers (init loop 0x6FAA8B)
    kRhyCoronaArray = 0xC3E058,   // entry 0 (0x6FC39F lea 0xc3e058(%ecx),%eax)
    kRhyCoronaHeadTexSlot = 0xC3E010  // 0xC3E000 + 4*4: "coronaheadlightline"
};
enum : uint32_t {
    kRhyCoronaStride = 0x3C,      // the init loop's `add 0x3c` (0x6FAAE6)
    kRhyCoronaMax = 64,           // the init loop's count (0x6FAAD3: mov 0x40,%ecx)
    kRhyCoronaHeadSprite = 4      // index into the sprite table
};
enum : uint32_t {
    kRhyCoronaOffKey = 0x0C,
    kRhyCoronaOffTexture = 0x10,
    kRhyCoronaOffStrength = 0x14,
    kRhyCoronaOffRange = 0x1C,
    kRhyCoronaOffRadius = 0x28,
    kRhyCoronaOffRgba = 0x2C
};

// CPlaceable::m_matrix / CMatrix::at come from signalheads.h (kRhyEntityMatrix,
// kRhyMatrixAt) - the same offsets the traffic module and game.cpp use: 0x54F200
// `mov 0x14(%ecx),%eax; add 0x30,%eax`.

static RhyPageCache g_pages;
static bool g_pagesInit = false;

static bool rhyVehPageReadable(uintptr_t page) {
    int hit = rhyPageCacheFind(&g_pages, static_cast<uint32_t>(page));
    if (hit >= 0) {
        return hit != 0;
    }
    MEMORY_BASIC_INFORMATION mbi;
    bool ok = false;
    if (VirtualQuery(reinterpret_cast<LPCVOID>(page), &mbi, sizeof mbi) ==
            sizeof mbi &&
        mbi.State == MEM_COMMIT && (mbi.Protect & PAGE_GUARD) == 0) {
        switch (mbi.Protect & 0xFFu) {
            case PAGE_READONLY: case PAGE_READWRITE: case PAGE_WRITECOPY:
            case PAGE_EXECUTE_READ: case PAGE_EXECUTE_READWRITE:
            case PAGE_EXECUTE_WRITECOPY:
                ok = true;
                break;
            default:
                break;
        }
    }
    rhyPageCachePut(&g_pages, static_cast<uint32_t>(page), ok ? 1 : 0);
    return ok;
}

static bool rhyVehReadable(uint32_t addr, unsigned len) {
    if (!rhyRangePlausible(addr, len)) {
        return false;
    }
    const uint32_t first = addr >> 12;
    const uint32_t last = (addr + len - 1) >> 12;
    return rhyVehPageReadable(first) && (first == last || rhyVehPageReadable(last));
}

// ---------------------------------------------------------------------------
// reading
// ---------------------------------------------------------------------------

uint32_t rhyVehicleHeadTexture() {
    if (!rhyVehReadable(kRhyCoronaHeadTexSlot, 4)) {
        return 0;
    }
    return *reinterpret_cast<const uint32_t*>(kRhyCoronaHeadTexSlot);
}

unsigned rhyVehicleCoronaRead(RhyCorona* out, unsigned outCap, unsigned* outTotal) {
    if (outTotal) {
        *outTotal = 0;
    }
    if (!out || outCap == 0) {
        return 0;
    }
    if (!g_pagesInit) {
        rhyPageCacheInit(&g_pages);
        g_pagesInit = true;
    }
    // The store is one page-and-a-half of committed .bss; validate it once per
    // frame rather than per entry, and bail out if it ever is not there (a
    // different game build, or a loader that moved the segment).
    if (!rhyVehReadable(kRhyCoronaCount, 4) ||
        !rhyVehReadable(kRhyCoronaArray, kRhyCoronaStride)) {
        return 0;
    }
    unsigned count = *reinterpret_cast<const uint32_t*>(kRhyCoronaCount);
    if (count > kRhyCoronaMax) {
        return 0;  // not the store we think this is
    }
    if (outTotal) {
        *outTotal = count;
    }
    unsigned n = 0;
    for (unsigned i = 0; i < count && n < outCap; i++) {
        const uint32_t e = kRhyCoronaArray + i * kRhyCoronaStride;
        if (!rhyVehReadable(e, kRhyCoronaStride)) {
            continue;
        }
        RhyCorona& c = out[n];
        std::memset(&c, 0, sizeof c);
        const float* pos = reinterpret_cast<const float*>(e);
        for (int k = 0; k < 3; k++) {
            c.pos[k] = pos[k];
        }
        c.key = *reinterpret_cast<const uint32_t*>(e + kRhyCoronaOffKey);
        c.texture = *reinterpret_cast<const uint32_t*>(e + kRhyCoronaOffTexture);
        c.strength = *reinterpret_cast<const float*>(e + kRhyCoronaOffStrength);
        c.range = *reinterpret_cast<const float*>(e + kRhyCoronaOffRange);
        c.radius = *reinterpret_cast<const float*>(e + kRhyCoronaOffRadius);
        const uint8_t* rgba = reinterpret_cast<const uint8_t*>(e + kRhyCoronaOffRgba);
        for (int k = 0; k < 4; k++) {
            c.rgba[k] = rgba[k];
        }
        // The entity's own axis, when the key points at something readable. The
        // key is entity+head_index, so try the key rounded down to a 4-byte
        // object start; a wrong guess fails the page check or gives a null
        // matrix and simply leaves hasNormal false.
        const uint32_t ent = c.key & ~3u;
        if (ent) {
            uint32_t matrix = 0;
            if (rhyVehReadable(ent + kRhyEntityMatrix, 4)) {
                matrix = *reinterpret_cast<const uint32_t*>(ent + kRhyEntityMatrix);
            }
            if (matrix && rhyVehReadable(matrix + kRhyMatrixAt, 12)) {
                const float* at = reinterpret_cast<const float*>(matrix + kRhyMatrixAt);
                bool ok = true;
                for (int k = 0; k < 3; k++) {
                    if (!std::isfinite(at[k])) {
                        ok = false;
                        break;
                    }
                    c.normal[k] = at[k];
                }
                c.hasNormal = ok;
            }
        }
        n++;
    }
    return n;
}

// ---------------------------------------------------------------------------
// the lights
// ---------------------------------------------------------------------------

struct Live {
    remixapi_LightHandle handle;
    RhyFade fade;
    float pos[3];
    float dir[3];
    float radiance[3];
    float radius;
    float coneDeg;
    float coneSoftness;
    DWORD created;
    bool seen;
};

// The gain below which a fade is over. Same floor the other modules use: a
// handle at 0.1% radiance is a light that is off but still in the light pool.
static const float kGainFloor = 0.004f;

// One tag per lamp, from the lamp's own hash (see delayed.h: tags are a shared
// namespace, so vehicle lamps live in their own band).
static int lampTag(uint64_t hash) {
    return kTagVehicle + static_cast<int>(hash % 100000ull);
}

static std::map<uint64_t, Live> g_live;
static unsigned g_coronas = 0;
static unsigned g_lamps = 0;
static unsigned g_vehicles = 0;
static unsigned g_creates = 0;
static unsigned g_skips = 0;
static unsigned g_createFails = 0;
static bool g_apiBroken = false;
static DWORD g_brokenTick = 0;
static DWORD g_lastTick = 0;
static DWORD g_lastLog = 0;

unsigned rhyVehicleCoronaCount() { return g_coronas; }
unsigned rhyVehicleLampCount() { return g_lamps; }
unsigned rhyVehicleLiveCount() { return static_cast<unsigned>(g_live.size()); }
unsigned rhyVehicleVehicles() { return g_vehicles; }
unsigned rhyVehicleCreates() { return g_creates; }
unsigned rhyVehicleSkips() { return g_skips; }

static float vecDist2(const float* a, const float* b) {
    float s = 0.0f;
    for (int i = 0; i < 3; i++) {
        const float d = a[i] - b[i];
        s += d * d;
    }
    return s;
}

// Build the LightInfo for one lamp at a given envelope gain and (for a headlamp)
// with its beam direction. Everything here is a fresh struct: the bridge copies
// it at CreateLight time.
static bool createLamp(const RhyVehicleLamp& lamp, float gain,
                       remixapi_LightHandle* out) {
    remixapi_LightInfo info;
    remixapi_LightInfoSphereEXT sphere;
    std::memset(&info, 0, sizeof info);
    std::memset(&sphere, 0, sizeof sphere);
    info.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
    info.hash = lamp.hash;
    sphere.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_SPHERE_EXT;
    sphere.position.x = lamp.pos[0];
    sphere.position.y = lamp.pos[1];
    sphere.position.z = lamp.pos[2];
    sphere.radius = lamp.radius;
    if (lamp.kind == kRhyVLampHead && lamp.coneDeg > 0.0f) {
        sphere.shaping_hasvalue = 1;
        sphere.shaping_value.direction.x = lamp.dir[0];
        sphere.shaping_value.direction.y = lamp.dir[1];
        sphere.shaping_value.direction.z = lamp.dir[2];
        sphere.shaping_value.coneAngleDegrees = lamp.coneDeg;
        sphere.shaping_value.coneSoftness = lamp.coneSoftness;
        sphere.shaping_value.focusExponent = 0.0f;
    }
    sphere.volumetricRadianceScale = 0.0f;
    info.pNext = &sphere;
    info.radiance.x = lamp.radiance[0] * gain;
    info.radiance.y = lamp.radiance[1] * gain;
    info.radiance.z = lamp.radiance[2] * gain;

    int reason = 0;
    const remixapi_LightHandle h = rhyCreateLight(&info, &reason);
    if (!h) {
        if (reason == 2) {
            g_apiBroken = true;
            g_brokenTick = GetTickCount();
            rhyLog("vehiclelights: CreateLight refused by the bridge #%u - the "
                   "module stops touching the bridge (reason 2)", g_createFails);
        }
        return false;
    }
    *out = h;
    g_creates++;
    return true;
}

// One look at the store, logged once, so a session where the module silently did
// nothing is a session with a line in the log saying why. The store can hold zero
// coronas at this moment (a loading screen, an interior), so what is checked is
// the count word and the sprite table, not the entries.
void rhyVehicleLightsInitOnce() {
    static bool done = false;
    if (done) {
        return;
    }
    done = true;
    if (!g_pagesInit) {
        rhyPageCacheInit(&g_pages);
        g_pagesInit = true;
    }
    const bool countOk = rhyVehReadable(kRhyCoronaCount, 4);
    const bool arrayOk = rhyVehReadable(kRhyCoronaArray, kRhyCoronaStride);
    const unsigned count = (countOk && arrayOk)
                               ? *reinterpret_cast<const uint32_t*>(kRhyCoronaCount)
                               : 0xFFFFFFFFu;
    rhyLog("vehiclelights: corona store entries@%08X stride=%u max=%u count@%08X "
           "readable=%d/%d count=%u head_tex=%08X (sprite slot %u)",
           kRhyCoronaArray, kRhyCoronaStride, kRhyCoronaMax, kRhyCoronaCount,
           countOk ? 1 : 0, arrayOk ? 1 : 0, count, rhyVehicleHeadTexture(),
           kRhyCoronaHeadSprite);
}

void rhyVehicleCoronaFrame() {
    const remixapi_Interface* api = rhyRemixApi();
    if (!api || !api->DrawLightInstance) {
        return;
    }
    const RhyConfig* cfg = rhyConfigGet();
    const DWORD now = GetTickCount();
    float dt = (g_lastTick && now > g_lastTick)
                   ? static_cast<float>(now - g_lastTick) / 1000.0f
                   : 1.0f / 60.0f;
    if (dt > 0.5f) {
        dt = 0.5f;  // a paused or alt-tabbed stretch must not become one huge step
    }
    g_lastTick = now;

    if (g_apiBroken) {
        // The bridge said no once. Do not spam it; let the fades finish on the
        // handles we already have and stop creating.
        for (std::map<uint64_t, Live>::iterator it = g_live.begin();
             it != g_live.end();) {
            if (!it->second.seen) {
                rhyDestroyLight(it->second.handle);
                g_live.erase(it++);
                continue;
            }
            it->second.seen = false;
            rhyDrawLight(it->second.handle);
            ++it;
        }
        return;
    }

    if (!cfg->veh_enabled) {
        for (std::map<uint64_t, Live>::iterator it = g_live.begin();
             it != g_live.end(); ++it) {
            rhyDestroyLight(it->second.handle);
        }
        if (!g_live.empty()) {
            rhyLog("vehiclelights: disabled, %d handle(s) destroyed",
                   static_cast<int>(g_live.size()));
        }
        g_live.clear();
        g_lamps = 0;
        g_coronas = 0;
        return;
    }

    // --- read the game ----------------------------------------------------
    rhyPageCacheClear(&g_pages);  // a page freed between frames must not stay "ok"
    RhyCorona cor[kRhyCoronaMax];
    unsigned total = 0;
    g_coronas = rhyVehicleCoronaRead(cor, kRhyCoronaMax, &total);

    float player[3] = {0, 0, 0};
    const bool havePlayer = rhyPlayerPos(player);

    RhyVehicleTuning t;
    std::memset(&t, 0, sizeof t);
    t.headTexture = rhyVehicleHeadTexture();
    t.maxDistance = static_cast<float>(cfg->veh_max_distance);
    t.headFlux = cfg->veh_head_flux;
    t.tailFlux = cfg->veh_tail_flux;
    t.brakeFlux = cfg->veh_brake_flux;
    t.headRadius = cfg->veh_head_radius;
    t.tailRadius = cfg->veh_tail_radius;
    t.headConeDeg = cfg->veh_head_cone_deg;
    t.headConeSoftness = cfg->veh_head_cone_softness;
    t.radianceScale = cfg->veh_radiance_scale;  // n23: NOT cfg->radiance_scale
    t.brakeRedMin = cfg->veh_brake_red_min;
    t.maxLamps = cfg->veh_max_lights;
    t.maxSpread = cfg->veh_max_spread;

    RhyVehicleLamp lamps[64];
    int vehicles = 0;
    const int n = rhyVehicleBuildLamps(cor, g_coronas,
                                       havePlayer ? player : nullptr, t, lamps,
                                       cfg->veh_max_lights, &vehicles);
    g_lamps = static_cast<unsigned>(n);
    g_vehicles = static_cast<unsigned>(vehicles);

    // --- what the game is asking for --------------------------------------
    //
    // A car moves, and a Remix light's position, direction and radiance are all
    // baked at CreateLight time (remix.h: there is no UpdateLight), so a moving
    // lamp has to be re-created. Bounded twice: only when it actually moved
    // (veh_move_step), re-aimed or changed filament, and at most once per
    // veh_cadence_ms. A car at 100 km/h is 28 m/s, so with move_step 0.8 and
    // cadence 120 ms that is at most 8 creates per second per lamp - and the
    // per-frame create budget in remix.cpp bounds it again on top of that.
    const DWORD cadenceMs = static_cast<DWORD>(cfg->veh_cadence_ms > 0
                                                   ? cfg->veh_cadence_ms : 120);
    // `seen` is per-pass. Without the reset a lamp keeps the flag it got on the
    // frame it was created, the retire branch below never sees a missing lamp, and
    // a car that drives away (or switches its lights off, or falls out of the cap)
    // keeps its lamps for the rest of the session.
    for (std::map<uint64_t, Live>::iterator it = g_live.begin();
         it != g_live.end(); ++it) {
        it->second.seen = false;
    }
    for (int i = 0; i < n; i++) {
        const RhyVehicleLamp& lamp = lamps[i];
        std::map<uint64_t, Live>::iterator it = g_live.find(lamp.hash);
        const float gain = havePlayer
            ? rhyDistanceGain(std::sqrt(vecDist2(lamp.pos, player)),
                              t.maxDistance * 0.6f, t.maxDistance)
            : 1.0f;
        if (it == g_live.end()) {
            if (!rhyCanCreateLight()) {
                g_skips++;
                continue;
            }
            Live lv;
            std::memset(&lv, 0, sizeof lv);
            rhyFadeInit(lv.fade, static_cast<float>(cfg->fade_in_ms) / 1000.0f,
                        static_cast<float>(cfg->fade_out_ms) / 1000.0f);
            lv.fade.target = gain;
            lv.fade.gain = gain;
            lv.fade.published = gain;
            for (int k = 0; k < 3; k++) {
                lv.pos[k] = lamp.pos[k];
                lv.dir[k] = lamp.dir[k];
                lv.radiance[k] = lamp.radiance[k];
            }
            lv.radius = lamp.radius;
            lv.coneDeg = lamp.coneDeg;
            lv.coneSoftness = lamp.coneSoftness;
            lv.created = now;
            lv.seen = true;
            rhyFlushDelayedTag(lampTag(lamp.hash));
            if (!createLamp(lamp, gain, &lv.handle)) {
                continue;
            }
            g_live[lamp.hash] = lv;
            continue;
        }

        Live& lv = it->second;
        lv.seen = true;
        lv.fade.target = gain;
        rhyFadeAdvance(lv.fade, dt);

        const bool moved = vecDist2(lamp.pos, lv.pos) >
                           cfg->veh_move_step * cfg->veh_move_step;
        float dot = 0.0f;
        for (int k = 0; k < 3; k++) {
            dot += lamp.dir[k] * lv.dir[k];
        }
        // 0.9848 = cos(10 deg). A car that turns has to have its beam turned with
        // it; a car going straight does not, and rhyAngleDeg's warning in
        // smooth.h applies (a dot product near 1 is noise), so the test is a
        // plain threshold on the dot rather than an angle.
        const bool reaimed = lamp.kind == kRhyVLampHead && dot < 0.9848f;
        float radDelta = 0.0f;
        for (int k = 0; k < 3; k++) {
            radDelta = std::max(radDelta, std::fabs(lamp.radiance[k] -
                                                    lv.radiance[k]));
        }
        const bool rebright = radDelta > 0.08f;  // the brake filament came on
        const bool cadenceOk = (now - lv.created) >= cadenceMs;
        if ((moved || reaimed || rebright ||
             rhyFadeDue(lv.fade, cfg->fade_step, kGainFloor)) &&
            cadenceOk && rhyCanCreateLight()) {
            const RhyFadeOp op = rhyFadeApply(lv.fade, cfg->fade_step,
                                              kGainFloor);
            if (op == kRhyFadeRetire) {
                rhySpendCreateSlot();
                if (lv.handle) {
                    rhyDelayDestroy(lv.handle, lampTag(lamp.hash));
                    lv.handle = 0;
                }
                continue;
            }
            if (op != kRhyFadeNone) {
                rhyFlushDelayedTag(lampTag(lamp.hash));
                remixapi_LightHandle h = 0;
                if (createLamp(lamp, lv.fade.published, &h)) {
                    if (lv.handle) {
                        rhyDelayDestroy(lv.handle, lampTag(lamp.hash));
                    }
                    lv.handle = h;
                    lv.created = now;
                    for (int k = 0; k < 3; k++) {
                        lv.pos[k] = lamp.pos[k];
                        lv.dir[k] = lamp.dir[k];
                        lv.radiance[k] = lamp.radiance[k];
                    }
                } else {
                    g_skips++;
                }
            }
        }
    }

    // --- retire what the game stopped registering, and draw the rest ------
    for (std::map<uint64_t, Live>::iterator it = g_live.begin();
         it != g_live.end();) {
        Live& lv = it->second;
        if (!lv.seen) {
            // The corona is gone: the car's lights went off, the car streamed
            // out, or the cap stopped covering it. Fade, never an instant cut
            // (s28 finding 1), and the handle goes only after published hits 0.
            lv.fade.target = 0.0f;
            rhyFadeAdvance(lv.fade, dt);
            const bool cadenceOk = (now - lv.created) >= cadenceMs;
            if (cadenceOk && rhyCanCreateLight()) {
                const RhyFadeOp op = rhyFadeApply(lv.fade, cfg->fade_step,
                                                  kGainFloor);
                if (op == kRhyFadeRetire) {
                    rhySpendCreateSlot();
                    if (lv.handle) {
                        rhyDelayDestroy(lv.handle, lampTag(it->first));
                    }
                    g_live.erase(it++);
                    continue;
                }
                if (op != kRhyFadeNone) {
                    RhyVehicleLamp held;
                    std::memset(&held, 0, sizeof held);
                    held.kind = (lv.coneDeg > 0.0f) ? kRhyVLampHead
                                                    : kRhyVLampTail;
                    for (int k = 0; k < 3; k++) {
                        held.pos[k] = lv.pos[k];
                        held.dir[k] = lv.dir[k];
                        held.radiance[k] = lv.radiance[k];
                    }
                    held.radius = lv.radius;
                    held.coneDeg = lv.coneDeg;
                    held.coneSoftness = lv.coneSoftness;
                    rhyFlushDelayedTag(lampTag(it->first));
                    remixapi_LightHandle h = 0;
                    if (createLamp(held, lv.fade.published, &h)) {
                        if (lv.handle) {
                            rhyDelayDestroy(lv.handle, lampTag(it->first));
                        }
                        lv.handle = h;
                        lv.created = now;
                    }
                }
            }
        }
        if (lv.handle) {
            rhyDrawLight(lv.handle);
        }
        ++it;
    }

    if (now - g_lastLog >= 1000) {
        g_lastLog = now;
        rhyLog("vehiclelights: coronas=%u/%u head_tex=%08X lamps=%d vehicles=%u "
               "live=%u creates=%u budget_skips=%u",
               g_coronas, total, rhyVehicleHeadTexture(), n, g_vehicles,
               static_cast<unsigned>(g_live.size()), g_creates, g_skips);
    }
}
