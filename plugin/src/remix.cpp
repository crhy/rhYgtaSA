#include "remix.h"

#include <cstring>
#include <cstdint>

#include "log.h"

static bool g_tried = false;  // never retry after a failed init
static bool g_ready = false;
static remixapi_Interface g_api;

void rhyRemixInitOnce() {
    if (g_tried) {
        return;
    }
    g_tried = true;

    HMODULE bridge = GetModuleHandleA("d3d9.dll");
    if (!bridge) {
        rhyLog("remix: d3d9.dll (Remix bridge) not loaded, giving up");
        return;
    }

    FARPROC func = GetProcAddress(bridge, "remixapi_InitializeLibrary");
    if (!func) {
        rhyLog("remix: d3d9.dll does not export remixapi_InitializeLibrary, giving up");
        return;
    }

    // GetProcAddress can only ever hand back a FARPROC - "int (__stdcall*)()",
    // which is not the real prototype of anything - so casting it straight to the
    // Remix prototype trips -Wcast-function-type. Nothing is being papered over:
    // PFN_remixapi_InitializeLibrary is the exact prototype remix_c.h declares for
    // that export, and on this
    // ABI an entry point is just one 32-bit code address, so convert through the
    // address value and assert that the conversion cannot lose anything.
    static_assert(sizeof(FARPROC) == sizeof(PFN_remixapi_InitializeLibrary),
                  "FARPROC and the Remix entry point must be the same size");
    const uintptr_t initLibAddr = reinterpret_cast<uintptr_t>(func);
    PFN_remixapi_InitializeLibrary initLib =
        reinterpret_cast<PFN_remixapi_InitializeLibrary>(initLibAddr);

    remixapi_InitializeLibraryInfo info;
    std::memset(&info, 0, sizeof info);
    info.sType = REMIXAPI_STRUCT_TYPE_INITIALIZE_LIBRARY_INFO;
    info.version = REMIXAPI_VERSION_MAKE(REMIXAPI_VERSION_MAJOR,
                                         REMIXAPI_VERSION_MINOR,
                                         REMIXAPI_VERSION_PATCH);

    std::memset(&g_api, 0, sizeof g_api);
    remixapi_ErrorCode err = initLib(&info, &g_api);
    rhyLog("remix: remixapi_InitializeLibrary v%u.%u.%u -> %u (0 = success)",
           static_cast<unsigned>(REMIXAPI_VERSION_MAJOR),
           static_cast<unsigned>(REMIXAPI_VERSION_MINOR),
           static_cast<unsigned>(REMIXAPI_VERSION_PATCH),
           static_cast<unsigned>(err));
    if (err != REMIXAPI_ERROR_CODE_SUCCESS) {
        return;  // game keeps running; never try again
    }
    g_ready = true;
}

const remixapi_Interface* rhyRemixApi() {
    return g_ready ? &g_api : nullptr;
}

// ---------------------------------------------------------------- light calls

static unsigned g_live = 0;
static unsigned g_createFail = 0;
static unsigned g_drawFail = 0;
static unsigned g_budget = 0;  // 0 = no cap; the modules cap themselves
static unsigned g_budgetRefuse = 0;

// s28: creates per presented frame. A fade is a sequence of CreateLight calls
// (radiance is baked at create time), so without this a fade of the whole street
// layer would be one 320-create burst - which is the very thing being fixed.
static unsigned g_frameBudget = 0;   // 0 = no per-frame limit
static unsigned g_frameCreates = 0;
static unsigned g_deferred = 0;

void rhySetLightBudget(unsigned cap) { g_budget = cap; }
unsigned rhyBudgetRefusals() { return g_budgetRefuse; }

void rhyBeginFrame(unsigned perFrameCreates) {
    g_frameBudget = perFrameCreates;
    g_frameCreates = 0;
}
unsigned rhyFrameCreates() { return g_frameCreates; }
unsigned rhyCreateDeferred() { return g_deferred; }

bool rhyCreateBudgetLeft() {
    return g_frameBudget == 0 || g_frameCreates < g_frameBudget;
}

void rhySpendCreateSlot() {
    if (g_frameBudget != 0 && g_frameCreates < g_frameBudget) {
        ++g_frameCreates;
    }
}

bool rhyCanCreateLight() {
    if (g_frameBudget != 0 && g_frameCreates >= g_frameBudget) {
        return false;
    }
    return g_budget == 0 || g_live < g_budget;
}

// Returns the handle the bridge gave us, or null when the create failed.  Note
// what "failed" means here: the bridge client can return SUCCESS plus a handle
// for a light the Remix server then refuses to draw.  That is why rhyDrawLight
// counts separately - the draw counter is the one that actually tracks whether
// the lights exist in the renderer.  reason separates "we said no to ourselves"
// (1) from "the bridge said no" (2); see remix.h.
remixapi_LightHandle rhyCreateLight(const remixapi_LightInfo* info, int* reason) {
    const remixapi_Interface* api = rhyRemixApi();
    if (!api || !api->CreateLight) {
        if (reason) {
            *reason = 2;
        }
        return nullptr;
    }
    if (g_frameBudget != 0 && g_frameCreates >= g_frameBudget) {
        // A fade step that did not fit in this frame. The caller keeps the
        // handle it has and tries again next frame, so the light's published
        // radiance does not jump - it just arrives one frame later.
        if (g_deferred++ < 3 || (g_deferred % 500) == 0) {
            rhyLog("remix: frame create budget spent (%u creates this frame, "
                   "%u deferred so far): the fade continues next frame",
                   g_frameCreates, g_deferred);
        }
        if (reason) {
            *reason = 3;
        }
        return nullptr;
    }
    if (g_budget != 0 && g_live >= g_budget) {
        // Logged a few times, not every frame: the cap is a config value, so the
        // fix is max_total_lights in rhyremix.ini, or fewer lights in a module.
        if (g_budgetRefuse++ < 3) {
            rhyLog("remix: refusing a light, %u handles already live "
                   "(max_total_lights=%u)", g_live, g_budget);
        }
        if (reason) {
            *reason = 1;  // our cap, not the bridge: the module must stay on
        }
        return nullptr;
    }
    remixapi_LightHandle handle = nullptr;
    const remixapi_ErrorCode err = api->CreateLight(info, &handle);
    if (err != REMIXAPI_ERROR_CODE_SUCCESS || !handle) {
        ++g_createFail;
        if (reason) {
            *reason = 2;
        }
        return nullptr;
    }
    if (reason) {
        *reason = 0;
    }
    ++g_live;
    ++g_frameCreates;
    return handle;
}

void rhyDestroyLight(remixapi_LightHandle handle) {
    const remixapi_Interface* api = rhyRemixApi();
    if (!api || !api->DestroyLight || !handle) {
        return;
    }
    if (api->DestroyLight(handle) == REMIXAPI_ERROR_CODE_SUCCESS && g_live > 0) {
        --g_live;
    }
}

bool rhyDrawLight(remixapi_LightHandle handle) {
    const remixapi_Interface* api = rhyRemixApi();
    if (!api || !api->DrawLightInstance || !handle) {
        return false;
    }
    if (api->DrawLightInstance(handle) != REMIXAPI_ERROR_CODE_SUCCESS) {
        ++g_drawFail;
        return false;
    }
    return true;
}

unsigned rhyLiveLights() { return g_live; }
unsigned rhyCreateFailures() { return g_createFail; }
unsigned rhyDrawFailures() { return g_drawFail; }
