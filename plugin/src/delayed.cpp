#include "delayed.h"

#include <windows.h>

#include <utility>
#include <vector>

#include "config.h"
#include "log.h"
#include "remix.h"

// (due tick, tag, handle).  Due tick uses the wraparound-safe GetTickCount() idiom;
// tag is the list index the handle belongs to (see delayed.h).
struct PendingDestroy {
    DWORD due;
    int tag;
    remixapi_LightHandle handle;
};

static std::vector<PendingDestroy> g_pending;

void rhyDelayDestroy(remixapi_LightHandle handle, int tag) {
    if (!handle) {
        return;
    }
    const RhyConfig* cfg = rhyConfigGet();
    const DWORD due = GetTickCount() + static_cast<DWORD>(cfg->destroy_delay_ms);
    PendingDestroy e = {due, tag, handle};
    g_pending.push_back(e);
    if (g_pending.size() > 4096) {
        // Something is leaking handles (usually a broken bridge): destroy now
        // rather than growing without bound.
        rhyLog("delayed: %u handles pending, destroying immediately",
               static_cast<unsigned>(g_pending.size()));
        for (size_t i = 0; i < g_pending.size(); ++i) {
            rhyDestroyLight(g_pending[i].handle);
        }
        g_pending.clear();
    }
}

// Destroy the pending handles of one tag now.  Used right before re-creating a
// list index whose old handle is still waiting: same hash, so the destroy has
// to go first or it takes the new light with it.
void rhyFlushDelayedTag(int tag) {
    if (g_pending.empty()) {
        return;
    }
    size_t kept = 0;
    for (size_t i = 0; i < g_pending.size(); ++i) {
        if (g_pending[i].tag == tag) {
            rhyDestroyLight(g_pending[i].handle);
            continue;
        }
        if (kept != i) {
            g_pending[kept] = g_pending[i];
        }
        ++kept;
    }
    g_pending.resize(kept);
}

void rhyFlushDelayed() {
    if (g_pending.empty()) {
        return;
    }
    const DWORD now = GetTickCount();
    size_t kept = 0;
    for (size_t i = 0; i < g_pending.size(); ++i) {
        if (static_cast<LONG>(now - g_pending[i].due) < 0) {  // not due yet
            if (kept != i) {
                g_pending[kept] = g_pending[i];
            }
            ++kept;
            continue;
        }
        rhyDestroyLight(g_pending[i].handle);  // keeps the live-handle count honest
    }
    g_pending.resize(kept);
}

unsigned rhyDelayedPending() {
    return static_cast<unsigned>(g_pending.size());
}
