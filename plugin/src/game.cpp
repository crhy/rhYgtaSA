#include "game.h"

#include <cstdint>

bool rhyPlayerPos(float* out) {
    uintptr_t ped = *reinterpret_cast<uintptr_t*>(0xB7CD98);
    if (!ped) {
        return false;
    }
    uintptr_t m = *reinterpret_cast<uintptr_t*>(ped + 0x14);
    const float* p = m ? reinterpret_cast<const float*>(m + 0x30)
                       : reinterpret_cast<const float*>(ped + 0x4);
    out[0] = p[0];
    out[1] = p[1];
    out[2] = p[2];
    return true;
}

// --- s28: the grace window. See game.h for why the pointer going to 0 must not
// be an instant "destroy every light" event.
namespace {
RhyPlayerTrack g_track = {{0.0f, 0.0f, 0.0f}, false, false, 0, 0};

// Wraparound-safe difference of two GetTickCount() values.
inline int32_t tickDelta(unsigned a, unsigned b) {
    return static_cast<int32_t>(a - b);
}
}  // namespace

void rhyPlayerTrackInit(RhyPlayerTrack& t) {
    t.last[0] = t.last[1] = t.last[2] = 0.0f;
    t.haveLast = false;
    t.lost = false;
    t.lastGoodTick = 0;
    t.lostEvents = 0;
}

RhyPlayerState rhyPlayerTrackUpdate(RhyPlayerTrack& t, bool read, const float* pos,
                                    unsigned graceMs, unsigned nowMs, float* held) {
    if (read && pos) {
        t.last[0] = held[0] = pos[0];
        t.last[1] = held[1] = pos[1];
        t.last[2] = held[2] = pos[2];
        t.haveLast = true;
        t.lost = false;
        t.lastGoodTick = nowMs;
        return kRhyPlayerOk;
    }
    if (!t.lost) {
        t.lost = true;
        // A ped pointer that is 0 before the player has ever existed (loading
        // screen) is not a disappearance, and counting it would make the log line
        // say the player was lost once per session for no reason.
        if (t.haveLast) {
            ++t.lostEvents;  // one count per disappearance, not per frame
        }
    }
    if (!t.haveLast) {
        return kRhyPlayerGone;  // never had a player: nothing to hold on to
    }
    held[0] = t.last[0];
    held[1] = t.last[1];
    held[2] = t.last[2];
    if (graceMs > 0 &&
        tickDelta(nowMs, t.lastGoodTick) <= static_cast<int32_t>(graceMs)) {
        return kRhyPlayerGrace;
    }
    return kRhyPlayerGone;
}

RhyPlayerState rhyPlayerPosGrace(float* out, unsigned graceMs, unsigned nowMs) {
    float pos[3];
    const bool read = rhyPlayerPos(pos);
    return rhyPlayerTrackUpdate(g_track, read, read ? pos : nullptr, graceMs, nowMs,
                               out);
}

unsigned rhyPlayerLostEvents() { return g_track.lostEvents; }

unsigned rhyPlayerLostMs(unsigned nowMs) {
    if (!g_track.lost || !g_track.haveLast) {
        return 0;
    }
    const int32_t d = tickDelta(nowMs, g_track.lastGoodTick);
    return d > 0 ? static_cast<unsigned>(d) : 0u;
}

int rhyGameHour() {
    return static_cast<int>(*reinterpret_cast<const uint8_t*>(0xB70153));
}
