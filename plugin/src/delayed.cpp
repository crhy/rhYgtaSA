#include "delayed.h"

#include <windows.h>

#include <algorithm>

#include "config.h"
#include "log.h"
#include "remix.h"
#include "smooth.h"

// (due tick, tag, handle).  Due tick uses the wraparound-safe GetTickCount() idiom;
// tag is the list index the handle belongs to (see delayed.h).
//
// n08: the storage is smooth.h's RhyDrainQueue, not a bare vector, because the
// queue is what makes the per-frame bound possible. Before n08 a frame destroyed
// every due handle at once; the 2026-11-12 runs measured that at 320 handles in
// one frame (00:48 run, `pending_destroy=320`) and 205 (10:22 run). A destroy is
// a bridge call like a create is, and s28 bounded only the creates.
struct PendingDestroy {
    int tag;
    remixapi_LightHandle handle;
};

static RhyDrainQueue<PendingDestroy> g_pending;
static unsigned g_deferred = 0;   // due handles the last frame refused
static DWORD g_lastDeferLog = 0;

void rhyDelayDestroy(remixapi_LightHandle handle, int tag) {
    if (!handle) {
        return;
    }
    const RhyConfig* cfg = rhyConfigGet();
    const DWORD due = GetTickCount() + static_cast<DWORD>(cfg->destroy_delay_ms);
    g_pending.add(due, PendingDestroy{tag, handle});
    if (g_pending.size() > 4096) {
        // Something is leaking handles (usually a broken bridge): destroy now
        // rather than growing without bound. removeIf() runs the predicate once
        // per item, so this destroys every handle, due or not, and empties the
        // queue in one pass.
        rhyLog("delayed: %u handles pending, destroying immediately",
               static_cast<unsigned>(g_pending.size()));
        g_pending.removeIf([](const PendingDestroy& p) {
            rhyDestroyLight(p.handle);
            return true;
        });
        g_deferred = 0;
    }
}

// Destroy the pending handles of one tag now.  Used right before re-creating a
// list index whose old handle is still waiting: same hash, so the destroy has
// to go first or it takes the new light with it.
//
// Not bounded by destroys_per_frame, on purpose: this one is a correctness rule
// (the replacement's hash is the destroyed handle's hash), it covers ONE index,
// and deferring it is what makes a lamp blink out and stay out.
void rhyFlushDelayedTag(int tag) {
    if (g_pending.empty()) {
        return;
    }
    // removeIf() calls the predicate exactly once per item, so destroying inside
    // it is well defined: the handle is destroyed and the item is dropped.
    g_pending.removeIf([tag](const PendingDestroy& p) {
        if (p.tag != tag) {
            return false;
        }
        rhyDestroyLight(p.handle);
        return true;
    });
}

void rhyFlushDelayed(unsigned perFrame) {
    if (g_pending.empty()) {
        g_deferred = 0;
        return;
    }
    const DWORD now = GetTickCount();
    unsigned budget = perFrame ? perFrame : 0xFFFFFFFFu;  // 0 = no bound
    unsigned destroyed = 0;
    PendingDestroy buf[32];
    while (budget) {
        const size_t want = budget < sizeof buf / sizeof buf[0]
                                ? static_cast<size_t>(budget)
                                : sizeof buf / sizeof buf[0];
        const size_t n = g_pending.takeDue(now, want, buf, want);
        for (size_t i = 0; i < n; ++i) {
            rhyDestroyLight(buf[i].handle);  // keeps the live-handle count honest
        }
        destroyed += static_cast<unsigned>(n);
        budget -= static_cast<unsigned>(n);
        if (n < want) {
            break;  // nothing else is due
        }
    }
    g_deferred = static_cast<unsigned>(g_pending.dueCount(now));
    if (g_deferred && now - g_lastDeferLog >= 5000u) {
        g_lastDeferLog = now;
        rhyLog("delayed: %u handles due, %u destroyed this frame, %u waiting for the "
               "next one ([fade] destroys_per_frame=%d; 0 puts the old one-frame flush "
               "back)", g_deferred + destroyed, destroyed, g_deferred,
               static_cast<int>(perFrame));
    }
}

unsigned rhyDelayedPending() {
    return static_cast<unsigned>(g_pending.size());
}

unsigned rhyDelayedDue() {
    return static_cast<unsigned>(g_pending.dueCount(GetTickCount()));
}

unsigned rhyDelayedDeferred() {
    return g_deferred;
}
