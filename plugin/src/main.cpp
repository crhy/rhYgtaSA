#include <windows.h>
#include <d3d9.h>

#include <cstdint>
#include <cstring>

#include "config.h"
#include "delayed.h"
#include "game.h"
#include "log.h"
#include "probe.h"
#include "remix.h"
#include "rtxconf.h"
#include "shutdown.h"
#include "signalhook.h"
#include "streetlights.h"
#include "sun.h"
#include "trafficlights.h"
#include "vehiclecorona.h"

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

// --- n09: the two device calls the shutdown tracker needs -------------------------------
//
// TestCooperativeLevel is vtable entry 3 and GetCreationParameters entry 9, counted against the
// MinGW-w64 d3d9 header exactly the way entry 17 (Present) and entry 21 (SetGammaRamp) were.
//
// Entry 9 is how the plugin gets the game's HWND at all: the hFocusWindow argument GTA hands to
// Present is NULL in the ordinary path, and hardcoding a window class name or a game address is
// exactly the kind of assumption this plugin has been burned by. The creation parameters are the
// device's own answer, and IsWindow() on that handle is the earliest quit signal available from
// inside the process: Quit destroys the window long before the device stops existing.
//
// TestCooperativeLevel is a call into the very client we are asking the question about, so it is
// polled at most once a second and only once something else already looks wrong.
typedef HRESULT(__stdcall* rhyTestCoopFn)(IDirect3DDevice9*);
typedef HRESULT(__stdcall* rhyGetCreationParamsFn)(IDirect3DDevice9*,
                                                   D3DDEVICE_CREATION_PARAMETERS*);
static const int kTestCooperativeLevelEntry = 3;
static const int kGetCreationParametersEntry = 9;
static rhyTestCoopFn g_origTestCoop = nullptr;
static HWND g_gameWindow = NULL;   // the window the device was created with (0 = unknown)
static DWORD g_startTick = 0;      // DllMain's GetTickCount(), for the shutdown timings
static RhyShutdownTrack g_shut;    // written by the render thread (hkPresent) and by DllMain only
static bool g_suspectLogged = false;

// The [shutdown] persistence windows.
static RhyShutdownLimits shutdownLimits() {
    const RhyConfig* cfg = rhyConfigGet();
    RhyShutdownLimits lim;
    lim.presentMs = cfg ? static_cast<unsigned>(cfg->shutdown_present_ms) : 1500u;
    lim.deviceMs = cfg ? static_cast<unsigned>(cfg->shutdown_device_ms) : 3000u;
    lim.windowMs = cfg ? static_cast<unsigned>(cfg->shutdown_window_ms) : 750u;
    lim.callsMs = cfg ? static_cast<unsigned>(cfg->shutdown_calls_ms) : 4000u;
    return lim;
}

// The moment the tracker decided the bridge is not coming back. Everything the 2026-11-12 run
// failed to say, in one line: which signal ended it, which ones were on, when the first of them
// started, and how long the plugin had been running.
static void rhyShutdownLatched(DWORD now) {
    char why[64];
    char signals[64];
    rhyShutdownReasonText(g_shut.deadReasons, why, sizeof why);
    rhyShutdownReasonText(g_shut.on, signals, sizeof signals);
    const unsigned first = rhyShutdownFirstSignal(g_shut);
    rhyLog("shutdown: bridge gone (reasons=%s signals_now=%s first_signal=+%u ms after start "
           "held=%u ms uptime=%u ms suspect_frames=%u)",
           why, signals, rhyShutdownElapsed(first, g_startTick),
           rhyShutdownElapsed(now, first), rhyShutdownElapsed(now, g_startTick),
           g_shut.suspectFrames);
    // One latch covers every Remix call in the plugin: rhyRemixApi() goes null, and each module's
    // existing `if (!api || !api->DrawLightInstance) return;` does the rest. The handles are not
    // destroyed - the process is ending and DestroyLight is exactly the call that must not happen
    // into a bridge client whose server has exited.
    rhyLog("shutdown: no more Remix calls: %u handles stay as they are, %u pending destroys are "
           "dropped, SetConfigVariable and every Create/Draw/DestroyLight are skipped from here",
           rhyLiveLights(), rhyDelayedPending());
    rhyRemixShutdown();
    rhySignalHookUninstall();  // logs its own line; the game's teardown runs the game's own code
}

// The frame path once the bridge is gone. No init-once, no config reload, no frame budget, no
// module, no flush of pending destroys: on 2026-11-12 all of that ran every frame for the last
// second of the session, drawing 302 street lights and 4 signal lights into nothing.
static HRESULT rhyPresentAfterShutdown(IDirect3DDevice9* dev, const RECT* srcRect,
                                       const RECT* dstRect, HWND focusWindow,
                                       const RGNDATA* regions, DWORD now) {
    // Keep the counters honest (frames seen after the latch) without re-declaring anything: an
    // already latched tracker only counts.
    const RhyShutdownSignals idle = {};
    rhyShutdownUpdate(g_shut, idle, shutdownLimits(), now);

    const RhyConfig* cfg = rhyConfigGet();
    const unsigned logMs = cfg ? static_cast<unsigned>(cfg->shutdown_log_ms) : 5000u;
    static DWORD s_lastLog = 0;
    if (now - s_lastLog >= logMs) {
        s_lastLog = now;
        rhyLog("shutdown: still running %u ms after the bridge went away (frames since=%u, "
               "remix calls skipped=%u, handles held=%u, pending destroys=%u, window=%s)",
               rhyShutdownElapsed(now, rhyRemixDeadTick()), g_shut.framesAfterDead,
               rhyRemixSkippedCalls(), rhyLiveLights(), rhyDelayedPending(),
               (g_gameWindow && IsWindow(g_gameWindow)) ? "alive" : "gone");
    }

    // The A/B switch for the spin itself. The 2026-11-12 evidence puts the spin in the game's
    // device-reset path (the last renderer line of the run is DXVK failing to recreate the swap
    // chain, and our own Present-driven log stops 340 ms before the game's exit path begins), so
    // this is off by default: it is how the next run finds out whether the bridge client's Present
    // is where the CPU went, which is the one part of that loop an ASI can reach.
    const unsigned stopAfter =
        cfg ? static_cast<unsigned>(cfg->stop_present_after_ms) : 0u;
    if (stopAfter && rhyShutdownElapsed(now, rhyRemixDeadTick()) >= stopAfter) {
        static bool s_said = false;
        if (!s_said) {
            s_said = true;
            rhyLog("shutdown: stop_present_after_ms=%u reached - Present is no longer forwarded to "
                   "the bridge client and returns D3D_OK. If the game exits now, the spin was "
                   "inside that call; if it still hangs, the spin is in the game's own reset loop "
                   "and no ASI sitting behind Present can end it.", stopAfter);
        }
        return D3D_OK;
    }
    if (!g_origPresent) {
        return E_FAIL;
    }
    return g_origPresent(dev, srcRect, dstRect, focusWindow, regions);
}

static HRESULT __stdcall hkPresent(IDirect3DDevice9* dev, const RECT* srcRect,
                                   const RECT* dstRect, HWND focusWindow,
                                   const RGNDATA* regions) {
    const DWORD now = GetTickCount();
    if (rhyShutdownDead(g_shut)) {
        return rhyPresentAfterShutdown(dev, srcRect, dstRect, focusWindow, regions, now);
    }
    rhyRemixInitOnce();        // first frame, on the render thread
    rhyStreetlightsInitOnce(); // loads rhyremix.ini and the lamp list once
    rhyTrafficInitOnce();      // loads rhyremix_traffic.txt once
    rhyVehicleLightsInitOnce(); // n12: the corona store's layout, checked once
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
    rhyVehicleCoronaFrame();   // n12: the car's own head and tail lamps

    // One line a second, independent of which module is on.  This is the number
    // to watch while tuning: "live" is how many handles the bridge still holds
    // for us, and a draw_fail that climbs while live stays flat means the bridge
    // handed us handles for lights the Remix server never accepted.
    {
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

    if (!g_origPresent) {
        return E_FAIL;
    }
    const HRESULT hr = g_origPresent(dev, srcRect, dstRect, focusWindow, regions);

    // --- n09: what this frame says about the device, and the decision that follows.
    // The window check is two user32 calls; TestCooperativeLevel is a call into the bridge client,
    // so it is polled at most once a second and only once something else already looks wrong.
    RhyShutdownSignals sig = {};
    sig.presentFailed = FAILED(hr);
    sig.windowGone = (g_gameWindow != NULL) && !IsWindow(g_gameWindow);
    static DWORD s_coopTick = 0;
    if (g_origTestCoop && (sig.presentFailed || sig.windowGone) &&
        now - s_coopTick >= 1000u) {
        s_coopTick = now;
        sig.deviceLost = FAILED(g_origTestCoop(dev));
    }
    // Our own failing create/draw counters. Never enough on their own - one light the Remix server
    // refuses makes them climb for a whole session, and streetlights.cpp already handles that with
    // g_apiBroken - but with another signal on, they are the bridge dying under our hands.
    static unsigned s_lastCallFails = 0;
    const unsigned callFails = rhyCreateFailures() + rhyDrawFailures();
    sig.callsFailing = (callFails != s_lastCallFails);
    s_lastCallFails = callFails;

    if (rhyShutdownSuspect(g_shut)) {
        static DWORD s_suspectLog = 0;
        if (!g_suspectLogged || now - s_suspectLog >= 5000u) {
            s_suspectLog = now;
            g_suspectLogged = true;
            char signals[64];
            rhyShutdownReasonText(g_shut.on, signals, sizeof signals);
            rhyLog("shutdown: suspect (%s): the device, its window or our own calls stopped "
                   "answering - the [shutdown] windows decide when the Remix calls stop",
                   signals);
        }
    } else {
        g_suspectLogged = false;  // a new episode gets a new line
    }
    if (rhyShutdownUpdate(g_shut, sig, shutdownLimits(), now)) {
        rhyShutdownLatched(now);
    }
    return hr;
}

static DWORD WINAPI rhyInitThread(void* /*unused*/) {
    rhyLogInit();
    rhyLog("rhyremix start");

    IDirect3DDevice9* dev = nullptr;
    const DWORD waitStart = GetTickCount();
    for (int i = 0; i < 1200; ++i) {  // 100 ms polls, max 120 s
        dev = *reinterpret_cast<IDirect3DDevice9**>(const_cast<void*>(kDeviceSlot));
        if (dev) {
            break;
        }
        // n09: bounded, and it stops early if the game is already on its way out - patching a
        // device vtable while the process tears itself down is not a thing we want to do.
        if (rhyShutdownDead(g_shut)) {
            rhyLog("init: shutdown was latched while waiting for the device (waited %u ms): "
                   "nothing is patched", rhyShutdownElapsed(GetTickCount(), waitStart));
            return 0;
        }
        Sleep(100);
    }
    if (!dev) {
        rhyLog("no D3D9 device at 0xC97C28 after 120 s, giving up");
        return 0;
    }
    rhyLog("device=%p after %u ms", static_cast<void*>(dev),
           rhyShutdownElapsed(GetTickCount(), waitStart));

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

    // --- n09: the shutdown watch -------------------------------------------------
    // Entries 3 and 9 are untouched originals (only 17 and 21 are patched), so calling through
    // them is the game's own device code, not ours.
    g_origTestCoop = reinterpret_cast<rhyTestCoopFn>(vtable[kTestCooperativeLevelEntry]);
    const rhyGetCreationParamsFn getParams =
        reinterpret_cast<rhyGetCreationParamsFn>(vtable[kGetCreationParametersEntry]);
    D3DDEVICE_CREATION_PARAMETERS params;
    std::memset(&params, 0, sizeof params);
    if (getParams && SUCCEEDED(getParams(dev, &params))) {
        g_gameWindow = params.hFocusWindow;
        rhyLog("shutdown watch: the device was created with window 0x%08X (IsWindow() on it is the "
               "earliest quit signal in this process; TestCooperativeLevel entry %d is the backup)",
               static_cast<unsigned>(reinterpret_cast<uintptr_t>(g_gameWindow)),
               kTestCooperativeLevelEntry);
    } else {
        rhyLog("shutdown watch: GetCreationParameters failed, so the window signal is off for this "
               "run - Present failures and our own failing calls still detect the exit");
    }

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
        g_startTick = GetTickCount();
        rhyShutdownTrackInit(g_shut);
        if (!CreateThread(NULL, 0, rhyInitThread, NULL, 0, NULL)) {
            rhyLog("CreateThread for init failed (err=%lu)",
                   static_cast<unsigned long>(GetLastError()));
        }
    } else if (type == DLL_PROCESS_DETACH) {
        // n09: the line the 2026-11-12 run never printed. A clean exit ends here, so this is the
        // one place that can say "the plugin saw the process end, and here is what was still live".
        // It is written without the log mutex (rhyLogDetach) because another thread can have been
        // terminated holding it, and it does no Remix call and no VirtualProtect: at detach the
        // loader lock is held and the bridge client may already be gone.
        const DWORD now = GetTickCount();
        rhyShutdownDetach(g_shut, now);
        rhyRemixShutdown();
        char why[64];
        rhyShutdownReasonText(g_shut.deadReasons, why, sizeof why);
        rhyLogDetach("shutdown: DLL_PROCESS_DETACH after %u ms (reasons=%s frames=%u "
                     "frames_after_bridge_gone=%u handles_held=%u pending_destroys=%u "
                     "remix_calls_skipped=%u)",
                     rhyShutdownElapsed(now, g_startTick), why, g_shut.frames,
                     g_shut.framesAfterDead, rhyLiveLights(), rhyDelayedPending(),
                     rhyRemixSkippedCalls());
    }
    return TRUE;
}
