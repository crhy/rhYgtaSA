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

// ---------------------------------------------------------------------------
// s19: the hook's decisions (see the block comment in signalheads.h)
// ---------------------------------------------------------------------------

const uint8_t kRhyPatchCalleeBytes[5] = {0x0F, 0xB6, 0x54, 0x24, 0x28};

int rhySignalPatchCheck(int sourceIsGame, const uint8_t* callSite,
                        const uint8_t* callee, const uint8_t* cleanup,
                        uint32_t* calleeTarget) {
    // The switch first: with source=list the user has asked for no code hook at
    // all, and the byte check must not even read the exe. That is what makes
    // "set source=list and the hook is gone" a fact rather than an intention.
    if (!sourceIsGame) {
        return kRhyPatchSource;
    }
    if (!callSite || !callee || !cleanup) {
        return kRhyPatchCallSite;
    }
    // 0x49DD4C must be a `call rel32` whose target is the corona thunk. The
    // rel32 is read little-endian, exactly as the CPU decodes it.
    if (callSite[0] != 0xE8) {
        return kRhyPatchCallSite;
    }
    const uint32_t rel = static_cast<uint32_t>(
        static_cast<uint32_t>(callSite[1]) |
        (static_cast<uint32_t>(callSite[2]) << 8) |
        (static_cast<uint32_t>(callSite[3]) << 16) |
        (static_cast<uint32_t>(callSite[4]) << 24));
    // Wrapping 32-bit add: that is how the CPU computes a rel32 target.
    const uint32_t target = kRhySignalCallSite + 5u + rel;
    if (calleeTarget) {
        *calleeTarget = target;
    }
    if (target != kRhySignalCoronaFn) {
        return kRhyPatchCallSite;
    }
    // The stub hands 0x6FC580 a stack it built by hand, so that function has to
    // be the thunk the argument layout was read from.
    for (unsigned i = 0; i < sizeof kRhyPatchCalleeBytes; ++i) {
        if (callee[i] != kRhyPatchCalleeBytes[i]) {
            return kRhyPatchCallee;
        }
    }
    // And the caller must still clean 21 arguments: `add esp,0x54` three bytes
    // after the resume point. The stub pushes nothing across the tail jump, so
    // if that cleanup changed, the game's frame would come back skewed - and
    // DisplayActualLight writes its locals at [esp+0x2c..0x34] right there.
    if (!(cleanup[0] == 0x83 && cleanup[1] == 0xC4 && cleanup[2] == 0x54)) {
        return kRhyPatchCleanup;
    }
    return kRhyPatchOk;
}

// ---------------------------------------------------------------- collector

int rhyCollectHead(RhySignalCollector* c, uint32_t argsAddr, uint32_t entity,
                   uint32_t index, uint32_t tick, RhyGameReadFn read, void* ctx) {
    // Counted before anything is dereferenced: "calls=0" means the patched call
    // site is never reached (wrong build, or the game never draws a signal),
    // "calls>0 records=0" means it is reached and we reject what it hands us.
    // Those two need very different fixes.
    ++c->calls;
    if (c->busy) {
        // The recorder is not re-entrant: it writes a shared array and a shared
        // page cache without a lock. A second entry while the first is running
        // (a hook that fires from inside a read, a second thread) is a bug, and
        // the safe answer to a bug is to drop the record, not to corrupt one.
        ++c->reentrant;
        return kRhyCollectReentrant;
    }
    c->busy = 1;

    int result = kRhyCollectOk;
    if (c->n >= kRhySignalHeadCap) {
        ++c->dropped;
        result = kRhyCollectFull;
    } else {
        uint32_t args[kRhySignalArgCount];
        float pos[3];
        if (!read(ctx, argsAddr, args, sizeof args)) {
            ++c->bad;
            result = kRhyCollectBadArgs;
        } else if (!read(ctx, args[kRhyArgPos], pos, sizeof pos)) {
            ++c->bad;
            result = kRhyCollectBadPos;
        } else {
            RhySignalHead& h = c->heads[c->n];
            h.entity = entity;
            h.index = index;
            h.pos[0] = pos[0];
            h.pos[1] = pos[1];
            h.pos[2] = pos[2];
            // The entity matrix is guaranteed valid here in principle -
            // DisplayActualLight has just resolved it and used it to place this
            // very corona - but a modded or half-built entity would otherwise
            // fault on the render thread, so a matrix we cannot read just means
            // "no direction", never "no record".
            h.normal[0] = h.normal[1] = h.normal[2] = 0.0f;
            uint32_t mBits = 0;
            if (read(ctx, entity + kRhyEntityMatrix, &mBits, sizeof mBits) &&
                mBits) {
                float at[3];
                if (read(ctx, mBits + kRhyMatrixAt, at, sizeof at)) {
                    h.normal[0] = at[0];
                    h.normal[1] = at[1];
                    h.normal[2] = at[2];
                }
            }
            // The game passes the colour channels as ints from its byte tables
            // (scaled, then clamped through 0x821B40), and the callee stores
            // only their low bytes; masking matches what the game draws.
            h.rgb[0] = static_cast<uint8_t>(args[kRhyArgR] & 0xFFu);
            h.rgb[1] = static_cast<uint8_t>(args[kRhyArgG] & 0xFFu);
            h.rgb[2] = static_cast<uint8_t>(args[kRhyArgB] & 0xFFu);
            h.alpha = static_cast<uint8_t>(args[kRhyArgAlpha] & 0xFFu);
            const uint32_t sBits = args[kRhyArgStrength];
            std::memcpy(&h.strength, &sBits, sizeof h.strength);
            h.tick = tick;
            ++c->n;
            ++c->records;
        }
    }
    c->busy = 0;
    return result;
}

unsigned rhyCollectorTake(RhySignalCollector* c, RhySignalHead* out,
                          unsigned outCap) {
    // Same thread as the hook (the game's render thread), so no lock: the hook
    // has already run for this frame by the time hkPresent gets here, and it
    // cannot run again until this frame's rendering restarts.
    const unsigned n = (c->n < outCap) ? c->n : outCap;
    if (n > 0) {
        std::memcpy(out, c->heads, n * sizeof(RhySignalHead));
    }
    c->n = 0;
    return n;
}

// ------------------------------------------------------------- page cache

void rhyPageCacheInit(RhyPageCache* c) {
    for (unsigned i = 0; i < kRhyPageCacheSlots; ++i) {
        c->key[i] = 0xFFFFFFFFu;  // no page lives at the last page
        c->ok[i] = 0;
    }
    c->slot = 0;
}

void rhyPageCacheClear(RhyPageCache* c) {
    rhyPageCacheInit(c);
}

int rhyPageCacheFind(const RhyPageCache* c, uint32_t page) {
    for (unsigned i = 0; i < kRhyPageCacheSlots; ++i) {
        if (c->key[i] == page) {
            return c->ok[i];
        }
    }
    return -1;
}

void rhyPageCachePut(RhyPageCache* c, uint32_t page, int ok) {
    c->key[c->slot] = page;
    c->ok[c->slot] = ok ? 1 : 0;
    c->slot = (c->slot + 1u) % kRhyPageCacheSlots;
}

