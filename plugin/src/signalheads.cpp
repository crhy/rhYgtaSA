#include "signalheads.h"

#include <algorithm>
#include <cmath>
#include <cstring>

// ---------------------------------------------------------------- identity

uint64_t rhyHeadId(uint32_t entity, uint32_t index) {
    // The entity pointer is the stable part (one signal object, one id for the
    // whole session) and the 2DFX index picks the head out of that object. A
    // 64-bit key means the two never have to be packed into a small int, which
    // is what made the list mode's "index << 2" hash fragile.
    return (static_cast<uint64_t>(entity) << 8) |
           static_cast<uint64_t>(index & 0xFFu);
}

uint64_t rhyHeadLightHash(uint64_t id, int phase, uint64_t base) {
    // Murmur3 finalizer: entity pointers are 16-byte aligned and clustered, so
    // a plain sum leaves the low bits identical between neighbouring signals.
    uint64_t x = id * 0x9E3779B97F4A7C15ull +
                 static_cast<uint64_t>((phase & 3) + 1) * 0xC2B2AE3D27D4EB4Full;
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdull;
    x ^= x >> 33;
    return base + x;
}

// ---------------------------------------------------------------- colour

// The three colours the game itself hands to RegisterCorona for a signal head,
// read out of the exe's byte tables (0x8A6214 red = {0,255,255}, 0x8A6218 green
// = {255,128,0}, 0xA9AD94 blue = {0,0,0}, all indexed by the head's colour
// index). They are matched rather than table-indexed on purpose: the game
// multiplies them by a global factor ([0xB7C4E4]*[0x858CA8]) that a mod or a
// different settings profile can move, and a nearest-colour match survives that.
static const int kGameSignalRgb[3][3] = {
    {0, 255, 0},     // 0 green
    {255, 128, 0},   // 1 amber
    {255, 0, 0},     // 2 red
};

int rhySignalPhaseFromGameRgb(int r, int g, int b) {
    if (r < 0) r = 0;
    if (g < 0) g = 0;
    if (b < 0) b = 0;
    if (r > 255) r = 255;
    if (g > 255) g = 255;
    if (b > 255) b = 255;
    const int m = (r > g ? r : g) > b ? (r > g ? r : g) : b;
    if (m <= 0) {
        return -1;  // a black corona is not a signal aspect
    }
    // Scale the reference triple to the observed brightest channel, then pick
    // the closest one. Comparing normalised colours instead of raw bytes is
    // what keeps the answer the same when the game's global factor dims all
    // three channels together.
    int best = -1;
    long bestErr = 0x7FFFFFFF;
    for (int p = 0; p < 3; ++p) {
        const long rr = static_cast<long>(kGameSignalRgb[p][0]) * m / 255;
        const long gg = static_cast<long>(kGameSignalRgb[p][1]) * m / 255;
        const long bb = static_cast<long>(kGameSignalRgb[p][2]) * m / 255;
        const long dr = rr - r, dg = gg - g, db = bb - b;
        const long err = dr * dr + dg * dg + db * db;
        if (err < bestErr) {
            bestErr = err;
            best = p;
        }
    }
    // A corona that is nowhere near any aspect (a white one, say) is reported
    // as unknown rather than forced into the nearest of three: an unknown head
    // keeps the colour it already had instead of flicking to red.
    if (bestErr > 3L * 90L * 90L) {
        return -1;
    }
    return best;
}

// ---------------------------------------------------------------- lifetime

int rhyHeadStale(uint32_t lastSeenTick, uint32_t nowTick, uint32_t ttlMs) {
    // Unsigned subtraction is wraparound-safe for GetTickCount(), which rolls
    // over after ~49.7 days of uptime.
    const uint32_t age = nowTick - lastSeenTick;
    return age > ttlMs ? 1 : 0;
}

// ---------------------------------------------------------------- geometry

void rhyHeadLightPos(const RhySignalHead* h, const float* normal,
                     float forward, float* out) {
    out[0] = h->pos[0];
    out[1] = h->pos[1];
    out[2] = h->pos[2];
    if (!normal || !(forward != 0.0f)) {
        return;  // no direction known: the corona position is the lens position
    }
    const float len2 = normal[0] * normal[0] + normal[1] * normal[1] +
                       normal[2] * normal[2];
    if (!(len2 > 1e-6f)) {
        return;
    }
    const float k = forward / std::sqrt(len2);
    out[0] += normal[0] * k;
    out[1] += normal[1] * k;
    out[2] += normal[2] * k;
}

// ---------------------------------------------------------------- selection

bool rhyRangePlausible(uint32_t addr, unsigned len) {
    // 32-bit Windows user space: 0x10000 .. 0x7FFFFFFF (and in practice well
    // below that). The value that killed the 13:04:44 run, 0xC48F9A0F, is the
    // float -575.2 and is rejected here without touching the page table.
    if (addr < 0x10000u || addr >= 0x80000000u) {
        return false;
    }
    if (len > 0x10000u) {
        return false;  // a corona argument block is 84 bytes, not 64 KB
    }
    return addr + len <= 0x80000000u;
}

unsigned rhyChooseHeads(const RhySignalHead* heads, unsigned nHeads,
                        const float* playerPos, float maxDist, float exitDist,
                        unsigned cap, unsigned stickyExtra,
                        const uint64_t* active, unsigned nActive,
                        RhyHeadWant* out, unsigned outCap) {
    struct Cand {
        float d2;
        int src;
        int phase;
        uint64_t id;
        bool sticky;
    };
    Cand cand[kRhySignalHeadCap];
    unsigned n = 0;

    const float exit2 = (exitDist > 0.0f) ? exitDist * exitDist : 0.0f;
    const float max2 = (maxDist > 0.0f) ? maxDist * maxDist : 0.0f;

    for (unsigned i = 0; i < nHeads && n < kRhySignalHeadCap; ++i) {
        const RhySignalHead& h = heads[i];
        if (!(std::isfinite(h.pos[0]) && std::isfinite(h.pos[1]) &&
              std::isfinite(h.pos[2]))) {
            continue;  // a NaN position would poison every distance test below
        }
        const float dx = h.pos[0] - playerPos[0];
        const float dy = h.pos[1] - playerPos[1];
        const float d2 = dx * dx + dy * dy;
        if (exit2 > 0.0f && d2 > exit2) {
            continue;  // the WIDER bound first: see the hysteresis note in .h
        }
        uint64_t id = rhyHeadId(h.entity, h.index);
        bool sticky = false;
        for (unsigned a = 0; a < nActive; ++a) {
            if (active[a] == id) {
                sticky = true;
                break;
            }
        }
        // A head that has no light yet only gets one inside maxDist.
        if (!sticky && max2 > 0.0f && d2 > max2) {
            continue;
        }
        cand[n].d2 = d2;
        cand[n].src = static_cast<int>(i);
        cand[n].phase = rhySignalPhaseFromGameRgb(h.rgb[0], h.rgb[1], h.rgb[2]);
        cand[n].id = id;
        cand[n].sticky = sticky;
        ++n;
    }

    std::sort(cand, cand + n, [](const Cand& a, const Cand& b) {
        return a.d2 < b.d2;
    });

    unsigned written = 0;
    const unsigned grow = cap + stickyExtra;
    // Active heads first, so the ones that already have a handle are never the
    // ones the cap drops; then the new ones, nearest first.
    for (unsigned k = 0; k < n && written < outCap; ++k) {
        if (cand[k].sticky && written < grow) {
            out[written].id = cand[k].id;
            out[written].src = cand[k].src;
            out[written].phase = cand[k].phase;
            out[written].d2 = cand[k].d2;
            ++written;
        }
    }
    for (unsigned k = 0; k < n && written < outCap; ++k) {
        if (!cand[k].sticky && written < cap) {
            out[written].id = cand[k].id;
            out[written].src = cand[k].src;
            out[written].phase = cand[k].phase;
            out[written].d2 = cand[k].d2;
            ++written;
        }
    }
    return written;
}
