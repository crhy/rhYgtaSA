// n12: the pure half of the vehicle lights - classification, pairing, geometry
// and radiometry. No windows.h, no Remix, no game memory: see vehiclelights.h
// for the game layout this models and tests/vehicle_lights_check.cpp for the
// assertions. vehiclecorona.cpp is the side that touches the game and the bridge.

#include "vehiclelights.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "lamps.h"

// ---------------------------------------------------------------------------
// the real lamps
// ---------------------------------------------------------------------------

void rhyVehicleHeadFixture(RhyFixture* out) {
    if (!out) {
        return;
    }
    out->radius = 0.09f;        // reflector aperture, not the filament
    out->coneDeg = 25.0f;       // HALF angle: a 50-degree low-beam spread
    out->coneSoftness = 0.35f;  // the beam's shoulder, not a hard cutoff
    out->what = "vehicle headlamp";
}

void rhyVehicleTailFixture(RhyFixture* out) {
    if (!out) {
        return;
    }
    out->radius = 0.05f;        // the red lens
    out->coneDeg = 0.0f;        // rear position lamps are deliberately diffuse
    out->coneSoftness = 0.0f;
    out->what = "vehicle tail lamp";
}

// ---------------------------------------------------------------------------
// flux <-> radiance, and the cone concentration gain
// ---------------------------------------------------------------------------

float rhyFluxToRadiance(float flux, float radius) {
    if (!(radius > 0.0f)) {
        return 0.0f;
    }
    return flux / (4.0f * 3.14159265358979324f * radius * radius);
}

float rhyRadianceToFlux(float radiance, float radius) {
    if (!(radius > 0.0f)) {
        return 0.0f;
    }
    return radiance * 4.0f * 3.14159265358979324f * radius * radius;
}

float rhyBeamConcentrationGain(float coneHalfDeg) {
    if (!(coneHalfDeg > 0.0f) || coneHalfDeg >= 180.0f) {
        return 1.0f;
    }
    const float c = std::cos(coneHalfDeg * 3.14159265358979324f / 180.0f);
    const float solid = 2.0f * 3.14159265358979324f * (1.0f - c);
    if (!(solid > 1e-6f)) {
        return 1.0f;  // a zero-degree cone is not a light
    }
    return (4.0f * 3.14159265358979324f) / solid;
}

// ---------------------------------------------------------------------------
// classification
// ---------------------------------------------------------------------------

bool rhyCoronaPlausible(const RhyCorona& c) {
    for (int i = 0; i < 3; i++) {
        if (!std::isfinite(c.pos[i]) || std::fabs(c.pos[i]) > 1.0e7f) {
            return false;
        }
    }
    // The game's own radii are 15.0 for every corona this module cares about;
    // anything wilder than 1000 is a torn record, and a negative radius cannot
    // come from the game at all.
    if (!std::isfinite(c.radius) || c.radius < 0.0f || c.radius > 1000.0f) {
        return false;
    }
    return true;
}

int rhyVehicleClassifyCorona(const RhyCorona& c, uint32_t headTexture) {
    if (headTexture && c.texture == headTexture) {
        return kRhyVLampHead;
    }
    const int r = c.rgba[0], g = c.rgba[1], b = c.rgba[2];
    // Red-dominant by a wide margin. The 2dFx rear lamps the game colours near
    // (200,20,20); a headlamp is (160,160,140) or (150,195,195) and an amber
    // indicator is (255,150,40), which the g <= r/3 test rejects - an indicator
    // is not a lamp that should light the road, and giving one a white-red
    // sphere is how "the car has three tail lights" happens.
    if (r >= 120 && g * 3 <= r && b * 3 <= r) {
        return kRhyVLampTail;
    }
    return kRhyVLampNone;
}

bool rhyVehicleIsBrake(const RhyCorona& c, int redMin) {
    if (redMin <= 0) {
        return false;
    }
    return c.rgba[0] >= redMin;
}

// ---------------------------------------------------------------------------
// pairing
// ---------------------------------------------------------------------------

namespace {

struct Group {
    uint32_t key;        // the first key seen for this vehicle
    uint32_t entity;     // key & ~3: a stable id for the handle hash
    int head[8];
    int nHead;
    int tail[8];
    int nTail;
    float headC[3];
    float tailC[3];
    float centroid[3];
    float spread;
    float d2;
    bool normalOk;
    float normal[3];
};

float dist2(const float* a, const float* b) {
    float s = 0.0f;
    for (int i = 0; i < 3; i++) {
        const float d = a[i] - b[i];
        s += d * d;
    }
    return s;
}

void addVec(float* dst, const float* src) {
    for (int i = 0; i < 3; i++) {
        dst[i] += src[i];
    }
}

void scaleVec(float* v, float s) {
    for (int i = 0; i < 3; i++) {
        v[i] *= s;
    }
}

float normVec(float* v) {
    const float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (l < 1e-6f) {
        return 0.0f;
    }
    scaleVec(v, 1.0f / l);
    return l;
}

}  // namespace

uint64_t rhyVehicleLampHash(uint32_t entity, int kind, int slot) {
    // FNV-1a over the identity, then a final mix so neighbouring entities do not
    // land on neighbouring ids in the bridge's hash table.
    uint64_t h = 1469598103934665603ull;
    const uint32_t parts[3] = {entity, static_cast<uint32_t>(kind),
                               static_cast<uint32_t>(slot)};
    for (uint32_t p : parts) {
        for (int i = 0; i < 4; i++) {
            h ^= static_cast<uint64_t>((p >> (i * 8)) & 0xffu);
            h *= 1099511628211ull;
        }
    }
    h ^= h >> 29;
    h *= 0xbf58476d1ce4e5b9ull;
    h ^= h >> 32;
    if (!h) {
        h = 1;
    }
    return h;
}

int rhyVehicleBuildLamps(const RhyCorona* cor, unsigned nCoronas,
                         const float* player, const RhyVehicleTuning& t,
                         RhyVehicleLamp* out, int cap, int* outVehicles) {
    if (outVehicles) {
        *outVehicles = 0;
    }
    if (!cor || !out || cap <= 0) {
        return 0;
    }

    RhyFixture headF, tailF;
    rhyVehicleHeadFixture(&headF);
    rhyVehicleTailFixture(&tailF);
    const float headRadius = (t.headRadius > 0.0f) ? t.headRadius : headF.radius;
    const float tailRadius = (t.tailRadius > 0.0f) ? t.tailRadius : tailF.radius;
    const float headCone = (t.headConeDeg > 0.0f) ? t.headConeDeg : headF.coneDeg;
    const float headSoft = (t.headConeSoftness >= 0.0f) ? t.headConeSoftness
                                                        : headF.coneSoftness;

    // Headlamp colour: a halogen headlamp is a 3000-3200 K tungsten filament
    // behind glass, which is what lamps.h' tungsten family is. Not 6500 K: an
    // LED headlamp is, but SA's vehicle set is halogen, and the bluish-white the
    // game pushes for its own corona (0x96,0xc3,0xc3) is the game's sprite tint,
    // not the light's spectrum.
    float headRgb[3];
    rhyLampFamilyRgb(RHY_LAMP_TUNGSTEN, headRgb);
    // Rear lamp colour: the red GLASS of a rear position lamp, from the same
    // passband integration signalheads.h uses for a signal lens (610-640 nm).
    // A tail lamp is a coloured filter over a filament, exactly like a signal
    // lens, so it gets the same treatment - and it is why the lamp is red on the
    // sand rather than white-with-a-red-tint.
    float tailRgb[3];
    rhySignalLensRgb(2, tailRgb);

    // --- group the coronas by vehicle -------------------------------------
    Group groups[64];
    int nGroups = 0;
    for (unsigned i = 0; i < nCoronas; i++) {
        if (!rhyCoronaPlausible(cor[i])) {
            continue;
        }
        const int kind = rhyVehicleClassifyCorona(cor[i], t.headTexture);
        if (kind == kRhyVLampNone) {
            continue;
        }
        // The game's key is the entity plus the head index: the traffic sites
        // push entity+index and the vehicle sites push esi (0x6E0CFF) and esi+7
        // (0x6E219A) for the same vehicle. So "the same vehicle" is "keys that
        // differ by less than the largest head index the game uses", not
        // "key & ~3" - an index of 7 would split one car into two groups.
        const uint32_t key = cor[i].key;
        if (!key) {
            continue;
        }
        int g = 0;
        while (g < nGroups) {
            const long long d = static_cast<long long>(key) -
                                static_cast<long long>(groups[g].key);
            if (d >= -16 && d <= 16) {
                break;
            }
            g++;
        }
        if (g == nGroups) {
            if (nGroups == 64) {
                break;
            }
            std::memset(&groups[nGroups], 0, sizeof groups[nGroups]);
            groups[nGroups].key = key;
            groups[nGroups].entity = key & ~3u;
            nGroups++;
        }
        Group& gr = groups[g];
        if (kind == kRhyVLampHead) {
            if (gr.nHead < 8) {
                gr.head[gr.nHead++] = static_cast<int>(i);
            }
        } else {
            if (gr.nTail < 8) {
                gr.tail[gr.nTail++] = static_cast<int>(i);
            }
        }
        if (!gr.normalOk && cor[i].hasNormal) {
            gr.normalOk = true;
            for (int k = 0; k < 3; k++) {
                gr.normal[k] = cor[i].normal[k];
            }
        }
    }

    // --- per group: is it a vehicle, and which way does it point? ----------
    struct Pick {
        RhyVehicleLamp lamp;
        float d2;
    };
    Pick picks[128];
    int nPicks = 0;
    int vehicles = 0;

    for (int g = 0; g < nGroups; g++) {
        Group& gr = groups[g];
        const int n = gr.nHead + gr.nTail;
        if (!n) {
            continue;
        }
        float hc[3] = {0, 0, 0}, tc[3] = {0, 0, 0}, c[3] = {0, 0, 0};
        for (int i = 0; i < gr.nHead; i++) {
            addVec(hc, cor[gr.head[i]].pos);
        }
        for (int i = 0; i < gr.nTail; i++) {
            addVec(tc, cor[gr.tail[i]].pos);
        }
        if (gr.nHead) {
            scaleVec(hc, 1.0f / static_cast<float>(gr.nHead));
        }
        if (gr.nTail) {
            scaleVec(tc, 1.0f / static_cast<float>(gr.nTail));
        }
        addVec(c, hc);
        addVec(c, tc);
        scaleVec(c, 1.0f / static_cast<float>(n));

        // The lamps of one vehicle fit inside a few metres. This is also what
        // keeps a red shopfront 10 m from a parked car's headlamp corona from
        // becoming that car's tail lamp.
        float spread = 0.0f;
        for (int i = 0; i < gr.nHead; i++) {
            for (int j = i + 1; j < gr.nHead; j++) {
                spread = std::max(spread, dist2(cor[gr.head[i]].pos,
                                                cor[gr.head[j]].pos));
            }
        }
        for (int i = 0; i < gr.nTail; i++) {
            for (int j = i + 1; j < gr.nTail; j++) {
                spread = std::max(spread, dist2(cor[gr.tail[i]].pos,
                                                cor[gr.tail[j]].pos));
            }
        }
        if (gr.nHead && gr.nTail) {
            spread = std::max(spread, dist2(hc, tc));
        }
        spread = std::sqrt(spread);
        const float maxSpread = (t.maxSpread > 0.0f) ? t.maxSpread : 6.0f;
        if (spread > maxSpread) {
            continue;
        }

        gr.centroid[0] = c[0];
        gr.centroid[1] = c[1];
        gr.centroid[2] = c[2];
        gr.spread = spread;
        gr.d2 = player ? dist2(c, player) : 0.0f;
        if (t.maxDistance > 0.0f && gr.d2 > t.maxDistance * t.maxDistance) {
            continue;
        }
        vehicles++;

        // The beam axis: from the vehicle's own light dummies.
        float fwd[3] = {0, 0, 0};
        bool haveFwd = false;
        if (gr.nHead && gr.nTail) {
            for (int i = 0; i < 3; i++) {
                fwd[i] = hc[i] - tc[i];
            }
            haveFwd = normVec(fwd) > 0.2f;
        }
        if (!haveFwd && gr.normalOk) {
            for (int i = 0; i < 3; i++) {
                fwd[i] = gr.normal[i];
            }
            haveFwd = normVec(fwd) > 0.5f;
        }

        const float headGain = rhyBeamConcentrationGain(headCone);
        for (int i = 0; i < gr.nHead; i++) {
            if (!haveFwd) {
                break;  // an aimed light needs an axis; see the header
            }
            if (nPicks == 128) {
                break;
            }
            const RhyCorona& cc = cor[gr.head[i]];
            Pick& p = picks[nPicks++];
            p.d2 = player ? dist2(cc.pos, player) : 0.0f;
            std::memset(&p.lamp, 0, sizeof p.lamp);
            p.lamp.kind = kRhyVLampHead;
            p.lamp.entity = gr.entity;
            p.lamp.slot = i;
            for (int k = 0; k < 3; k++) {
                p.lamp.pos[k] = cc.pos[k];
                p.lamp.dir[k] = fwd[k];
            }
            p.lamp.radius = headRadius;
            p.lamp.coneDeg = headCone;
            p.lamp.coneSoftness = headSoft;
            const float flux = (t.headFlux > 0.0f) ? t.headFlux : 240.0f;
            const float l = rhyFluxToRadiance(flux, headRadius) * headGain *
                            t.radianceScale;
            for (int k = 0; k < 3; k++) {
                p.lamp.radiance[k] = l * headRgb[k];
            }
            p.lamp.flux = rhyRadianceToFlux(l, headRadius);
            p.lamp.hash = rhyVehicleLampHash(gr.entity, kRhyVLampHead, i);
        }

        for (int i = 0; i < gr.nTail; i++) {
            if (nPicks == 128) {
                break;
            }
            const RhyCorona& cc = cor[gr.tail[i]];
            const bool brake = rhyVehicleIsBrake(cc, t.brakeRedMin);
            float flux = brake ? t.brakeFlux : t.tailFlux;
            if (!(flux > 0.0f)) {
                flux = brake ? 240.0f : 60.0f;
            }
            Pick& p = picks[nPicks++];
            p.d2 = player ? dist2(cc.pos, player) : 0.0f;
            std::memset(&p.lamp, 0, sizeof p.lamp);
            p.lamp.kind = kRhyVLampTail;
            p.lamp.entity = gr.entity;
            p.lamp.slot = i;
            for (int k = 0; k < 3; k++) {
                p.lamp.pos[k] = cc.pos[k];
                p.lamp.dir[k] = 0.0f;
            }
            p.lamp.radius = tailRadius;
            p.lamp.coneDeg = 0.0f;
            p.lamp.coneSoftness = 0.0f;
            const float l = rhyFluxToRadiance(flux, tailRadius) *
                            t.radianceScale;
            for (int k = 0; k < 3; k++) {
                p.lamp.radiance[k] = l * tailRgb[k];
            }
            p.lamp.flux = rhyRadianceToFlux(l, tailRadius);
            p.lamp.hash = rhyVehicleLampHash(gr.entity, kRhyVLampTail, i);
        }
    }

    // Nearest first, then the cap. Same reason as the lamp lists: the cap has to
    // trim the far end of the set, never the lamp the player is standing next to.
    std::sort(picks, picks + nPicks, [](const Pick& a, const Pick& b) {
        if (a.d2 != b.d2) {
            return a.d2 < b.d2;
        }
        return a.lamp.hash < b.lamp.hash;
    });
    const int keep = std::min(nPicks, cap);
    for (int i = 0; i < keep; i++) {
        out[i] = picks[i].lamp;
    }
    if (outVehicles) {
        *outVehicles = vehicles;
    }
    return keep;
}
