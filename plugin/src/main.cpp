#include <windows.h>
#include <d3d9.h>

#include "config.h"
#include "delayed.h"
#include "game.h"
#include "log.h"
#include "probe.h"
#include "remix.h"
#include "rtxconf.h"
#include "streetlights.h"
#include "sun.h"
#include "trafficlights.h"

// The game's D3D9 device (0 until rendering starts); in the RTX build it belongs
// to the Remix bridge client d3d9.dll in the game folder.
static const void* const kDeviceSlot = reinterpret_cast<const void*>(0xC97C28);

// IDirect3DDevice9 vtable: 0..2 = IUnknown, Present is entry 17.
static const int kPresentEntry = 17;

// IDirect3DDevice9::SetGammaRamp is entry 21 (checked against the MinGW-w64
// d3d9 header: the base classes take entries 0..17, Present is 17, and SetGammaRamp
// is the entry directly after it).
typedef HRESULT(__stdcall* rhySetGammaRampFn)(IDirect3DDevice9*, UINT,
                                             D3DGAMMARAMP*, BOOL);
static const int kSetGammaRampEntry = 21;
static rhySetGammaRampFn g_origSetGammaRamp = nullptr;
static int g_gammaCalls = 0;

// The last Remix bridge command queued before the server dies with crash id
// 0xe06d7363 when the user presses Ctrl+PrintScreen, and the only call in that
// screenshot path the ASI itself makes.  The game's gamma ramp is cosmetic under
// Remix (rtx.tonemap.finalizeWithACES does the tonemapping), so swallow it
// instead of forwarding it.  Returning S_OK keeps the game from treating it as
// a failure and retrying in a loop.
static HRESULT __stdcall hkSetGammaRamp(IDirect3DDevice9* dev, UINT which,
                                       D3DGAMMARAMP* ramp, BOOL scaleGamma) {
    (void)dev;
    (void)which;
    (void)ramp;
    (void)scaleGamma;
    ++g_gammaCalls;
    if ((g_gammaCalls % 20) == 1) {
        rhyLog("SetGammaRamp swallowed (%d calls so far)", g_gammaCalls);
    }
    return S_OK;
}

typedef HRESULT(__stdcall* rhyPresentFn)(IDirect3DDevice9*, const RECT*,
                                        const RECT*, HWND, const RGNDATA*);

static rhyPresentFn g_origPresent = nullptr;

static HRESULT __stdcall hkPresent(IDirect3DDevice9* dev, const RECT* srcRect,
                                   const RECT* dstRect, HWND focusWindow,
                                   const RGNDATA* regions) {
    rhyRemixInitOnce();        // first frame, on the render thread
    rhyStreetlightsInitOnce(); // loads rhyremix.ini and the lamp list once
    rhyTrafficInitOnce();      // loads rhyremix_traffic.txt once
    rhyFlushDelayed();         // destroy handles retired on earlier frames
    rhyConfigReloadIfChanged(); // re-read rhyremix.ini when it changed on disk
    // s28: a fade is a sequence of CreateLight calls, so the frame gets an
    // explicit create budget before any module runs. Without it, the street
    // layer's 320 lights arriving (or leaving) is one burst that changes the
    // whole world's lighting between two frames - which is exactly what the
    // 2026-11-12 test measured 20 times in 25 minutes.
    if (const RhyConfig* cfg0 = rhyConfigGet()) {
        rhyBeginFrame(static_cast<unsigned>(cfg0->creates_per_frame));
    }
    if (const RhyConfig* cfg = rhyConfigGet()) {
        rhySetLightBudget(cfg->max_total_lights);
        // Push the extra rtx.* settings once the bridge is up. This is the hook
        // that lets the mod own the renderer's ambient and exposure instead of
        // leaving them as constants in rtx.conf; rhySunFrame() then drives the
        // time-varying part of them every second.
        static bool s_rtxApplied = false;
        if (!s_rtxApplied && rhyRemixApi()) {
            s_rtxApplied = true;
            if (cfg->sky_probe) {
                rhyRtxProbeOnce();
            }
            if (!cfg->sky_settings.empty()) {
                rhyRtxApplyFile(cfg->sky_settings.c_str());
            }
        }
    }
    rhyProbeFrame();           // one-light calibration probe, if enabled
    rhySunFrame();             // sun as a Remix distant light (replaces the Lua sun)
    rhyStreetlightsFrame();    // draw the street lamp lights before presenting
    rhyTrafficFrame();         // draw the traffic light lights too

    // One line a second, independent of which module is on.  This is the number
    // to watch while tuning: "live" is how many handles the bridge still holds
    // for us, and a draw_fail that climbs while live stays flat means the bridge
    // handed us handles for lights the Remix server never accepted.
    {
        const DWORD now = GetTickCount();
        static DWORD s_tick = 0;
        if (now - s_tick >= 1000) {
            s_tick = now;
            rhyLog("remix: live=%u create_fail=%u draw_fail=%u budget_refused=%u "
                   "| pending_destroy=%u creates_this_frame=%u deferred=%u "
                   "| player: lost_events=%u gone_ms=%u",
                   rhyLiveLights(), rhyCreateFailures(), rhyDrawFailures(),
                   rhyBudgetRefusals(), rhyDelayedPending(), rhyFrameCreates(),
                   rhyCreateDeferred(), rhyPlayerLostEvents(),
                   rhyPlayerLostMs(now));
        }
    }

    if (g_origPresent) {
        return g_origPresent(dev, srcRect, dstRect, focusWindow, regions);
    }
    return E_FAIL;
}

static DWORD WINAPI rhyInitThread(void* /*unused*/) {
    rhyLogInit();
    rhyLog("rhyremix start");

    IDirect3DDevice9* dev = nullptr;
    for (int i = 0; i < 1200; ++i) {  // 100 ms polls, max 120 s
        dev = *reinterpret_cast<IDirect3DDevice9**>(const_cast<void*>(kDeviceSlot));
        if (dev) {
            break;
        }
        Sleep(100);
    }
    if (!dev) {
        rhyLog("no D3D9 device at 0xC97C28 after 120 s, giving up");
        return 0;
    }
    rhyLog("device=%p", static_cast<void*>(dev));

    void** vtable = *reinterpret_cast<void***>(dev);
    DWORD prevPerms = 0;
    if (!VirtualProtect(&vtable[kPresentEntry], sizeof(void*),
                       PAGE_EXECUTE_READWRITE, &prevPerms)) {
        rhyLog("VirtualProtect on vtable[%d] failed (err=%lu)", kPresentEntry,
               static_cast<unsigned long>(GetLastError()));
        return 0;
    }

    g_origPresent = reinterpret_cast<rhyPresentFn>(vtable[kPresentEntry]);
    rhyLog("Present old=%p", reinterpret_cast<void*>(g_origPresent));
    vtable[kPresentEntry] = reinterpret_cast<void*>(&hkPresent);
    rhyLog("vtable[%d] patched to hkPresent=%p", kPresentEntry,
           reinterpret_cast<void*>(&hkPresent));

    // --- SetGammaRamp guard ---
    const RhyConfig* cfg = rhyConfigGet();
    if (!cfg->block_gamma) {
        rhyLog("block_gamma=0: SetGammaRamp forwarded to the bridge unchanged "
               "(Ctrl+PrintScreen may still crash the Remix server)");
        return 0;
    }
    if (FAILED(VirtualProtect(&vtable[kSetGammaRampEntry], sizeof(void*),
                              PAGE_EXECUTE_READWRITE, &prevPerms))) {
        rhyLog("VirtualProtect on vtable[%d] failed (err=%lu), gamma guard not "
               "installed", kSetGammaRampEntry,
               static_cast<unsigned long>(GetLastError()));
    } else {
        g_origSetGammaRamp =
            reinterpret_cast<rhySetGammaRampFn>(vtable[kSetGammaRampEntry]);
        vtable[kSetGammaRampEntry] = reinterpret_cast<void*>(&hkSetGammaRamp);
        rhyLog("vtable[%d] SetGammaRamp old=%p -> swallowed: the game's gamma "
               "ramp never reaches the bridge, so Ctrl+PrintScreen no longer "
               "queues the command that precedes crash 0xe06d7363", kSetGammaRampEntry,
               reinterpret_cast<void*>(g_origSetGammaRamp));
    }
    return 0;
}

BOOL WINAPI DllMain(HMODULE self, DWORD type, void* /*unused*/) {
    if (type == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(self);
        if (!CreateThread(NULL, 0, rhyInitThread, NULL, 0, NULL)) {
            rhyLog("CreateThread for init failed (err=%lu)",
                   static_cast<unsigned long>(GetLastError()));
        }
    }
    return TRUE;
}
