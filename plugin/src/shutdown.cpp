#include "shutdown.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

// Which slot of firstSeen[] a signal bit owns. Straight out of the enum, so a new signal cannot
// silently share a slot with an old one.
static unsigned slotOf(unsigned bit) {
    switch (bit) {
        case kRhyShutPresent: return 0;
        case kRhyShutDevice:  return 1;
        case kRhyShutWindow:  return 2;
        case kRhyShutCalls:   return 3;
        case kRhyShutDetach:  return 4;
        default:              return kRhyShutCount;  // not a signal we track
    }
}

void rhyShutdownTrackInit(RhyShutdownTrack& t) {
    t.started = 0;
    for (unsigned i = 0; i < kRhyShutCount; ++i) {
        t.firstSeen[i] = 0;
    }
    t.on = 0;
    t.seen = 0;
    t.deadTick = 0;
    t.deadReasons = 0;
    t.frames = 0;
    t.suspectFrames = 0;
    t.framesAfterDead = 0;
}

// Wraparound-safe difference of two GetTickCount() values, with no sentinel: GetTickCount() really
// is 0 for the first millisecond after the machine boots, so "0 means never" belongs to the log
// fields (deadTick, firstSeen's own bookkeeping) and must never leak into a measurement.
static unsigned tickDelta(unsigned nowMs, unsigned thenMs) {
    // GetTickCount() wraps at ~49.7 days; a signed difference of the unsigned values is the
    // standard wraparound-safe comparison and stays correct across it.
    const int32_t d = static_cast<int32_t>(nowMs - thenMs);
    return d > 0 ? static_cast<unsigned>(d) : 0u;
}

unsigned rhyShutdownElapsed(unsigned nowMs, unsigned thenMs) {
    return thenMs ? tickDelta(nowMs, thenMs) : 0u;
}

// The earliest of the started signals, compared wrap-safely rather than by raw minimum tick: a
// session that straddles the GetTickCount() wrap has signals whose firstSeen values sit at opposite
// ends of the 32-bit range.
unsigned rhyShutdownFirstSignal(const RhyShutdownTrack& t) {
    unsigned earliest = 0;
    for (unsigned i = 0; i < kRhyShutCount; ++i) {
        const unsigned bit = 1u << i;
        if (!(t.started & bit) || !t.firstSeen[i]) {
            continue;
        }
        if (!earliest || tickDelta(earliest, t.firstSeen[i]) > 0u) {
            earliest = t.firstSeen[i];
        }
    }
    return earliest;
}

// Start holding a signal, or stop. Stopping clears the timer rather than pausing it: a signal that
// blinks on and off is a game that is still alive, and accumulating its bad frames across the gaps
// is how a session ends up dark because of one alt-tab.
static void setSignal(RhyShutdownTrack& t, unsigned bit, bool on, unsigned nowMs) {
    const unsigned slot = slotOf(bit);
    if (slot >= kRhyShutCount) {
        return;
    }
    if (on) {
        if (!(t.started & bit)) {
            t.started |= bit;
            t.firstSeen[slot] = nowMs;
        }
        t.on |= bit;
        t.seen |= bit;
    } else {
        t.started &= ~bit;
        t.firstSeen[slot] = 0;
        t.on &= ~bit;
    }
}

// How long this signal has been holding, 0 when it is not holding. The `started` bit is the whole
// authority here - firstSeen may legitimately hold the tick value 0.
static unsigned heldMs(const RhyShutdownTrack& t, unsigned bit, unsigned nowMs) {
    const unsigned slot = slotOf(bit);
    if (slot >= kRhyShutCount || !(t.started & bit)) {
        return 0u;
    }
    return tickDelta(nowMs, t.firstSeen[slot]);
}

bool rhyShutdownUpdate(RhyShutdownTrack& t, const RhyShutdownSignals& s,
                       const RhyShutdownLimits& lim, unsigned nowMs) {
    ++t.frames;
    if (t.deadTick) {
        // Latched. Keep counting frames so the log can say how long the process has been running
        // with nothing of ours calling the bridge, but never re-declare anything.
        ++t.framesAfterDead;
        return false;
    }

    setSignal(t, kRhyShutPresent, s.presentFailed, nowMs);
    setSignal(t, kRhyShutDevice,  s.deviceLost,    nowMs);
    setSignal(t, kRhyShutWindow,  s.windowGone,    nowMs);
    setSignal(t, kRhyShutCalls,   s.callsFailing,  nowMs);
    if (t.on) {
        ++t.suspectFrames;
    }

    unsigned why = 0;
    if ((t.started & kRhyShutPresent) && heldMs(t, kRhyShutPresent, nowMs) >= lim.presentMs) {
        why |= kRhyShutPresent;
    }
    if ((t.started & kRhyShutDevice) && heldMs(t, kRhyShutDevice, nowMs) >= lim.deviceMs) {
        why |= kRhyShutDevice;
    }
    if ((t.started & kRhyShutWindow) && heldMs(t, kRhyShutWindow, nowMs) >= lim.windowMs) {
        why |= kRhyShutWindow;
    }
    // Our own failing calls, on their own, are a bad light, not a dead bridge (see the header).
    const bool company = (t.on & (kRhyShutPresent | kRhyShutDevice | kRhyShutWindow)) != 0u;
    if (company && (t.started & kRhyShutCalls) &&
        heldMs(t, kRhyShutCalls, nowMs) >= lim.callsMs) {
        why |= kRhyShutCalls;
    }

    if (!why) {
        return false;
    }
    // deadTick doubles as the "is it dead" flag, so it must never be stored as 0 (the tick really is
    // 0 for the first millisecond after boot); 1 ms of error there is nothing.
    t.deadTick = nowMs ? nowMs : 1u;
    t.deadReasons = why;
    return true;
}

bool rhyShutdownDetach(RhyShutdownTrack& t, unsigned nowMs) {
    if (t.deadTick) {
        return false;  // already declared; the detach line still gets printed by the caller
    }
    t.deadTick = nowMs ? nowMs : 1u;
    t.deadReasons = kRhyShutDetach | t.on;
    return true;
}

bool rhyShutdownDead(const RhyShutdownTrack& t) { return t.deadTick != 0u; }

bool rhyShutdownSuspect(const RhyShutdownTrack& t) {
    return t.deadTick == 0u && t.on != 0u;
}

const char* rhyShutdownReasonText(unsigned mask, char* buf, unsigned cap) {
    static const struct {
        unsigned bit;
        const char* name;
    } kNames[] = {
        {kRhyShutPresent, "present"},
        {kRhyShutDevice,  "device"},
        {kRhyShutWindow,  "window"},
        {kRhyShutCalls,   "calls"},
        {kRhyShutDetach,  "detach"},
    };
    if (buf && cap) {
        buf[0] = '\0';
        size_t len = 0;
        for (size_t i = 0; i < sizeof kNames / sizeof kNames[0]; ++i) {
            if (!(mask & kNames[i].bit)) {
                continue;
            }
            const size_t need = std::strlen(kNames[i].name) + (len ? 1u : 0u);
            if (len + need + 1u > cap) {
                break;  // truncated, but always a valid NUL-terminated string
            }
            if (len) {
                buf[len++] = '+';
            }
            for (size_t c = 0; kNames[i].name[c]; ++c) {
                buf[len++] = kNames[i].name[c];
            }
            buf[len] = '\0';
        }
    }
    return buf;
}
