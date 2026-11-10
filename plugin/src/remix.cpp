// The Remix API refuses to compile on 32-bit unless REMIX_ALLOW_X86 is set: our
// plugin is 32-bit (the game is), while the Remix bridge it talks to is 64-bit
// only in the sense of the API's data layout, which is fixed-size and identical.
#define REMIX_ALLOW_X86
#define REMIX_WINAPI_NO_LIBRARY_LOADER

#include <windows.h>

#include <cstring>
#include <cstdint>

#include "remix_c.h"
#include "log.h"

static bool g_tried = false;              // never retry after a failed init
static bool g_ready = false;
static remixapi_Interface g_api;
static remixapi_LightHandle g_light = nullptr;
static int g_drawCalls = 0;
static remixapi_LightInfo g_lightInfo;
static remixapi_LightInfoSphereEXT g_sphere;
static DWORD g_lastErrTick = 0;

static const UINT64 kLightHash = 0x52485952454D4958ull;  // "RH\XRHEMIX"-ish tag

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

    PFN_remixapi_InitializeLibrary initLib =
        reinterpret_cast<PFN_remixapi_InitializeLibrary>(func);

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

    // One test light: sphere, warm radiance, fixed world position.
    remixapi_LightInfo& lightInfo = g_lightInfo;
    std::memset(&lightInfo, 0, sizeof lightInfo);
    lightInfo.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
    lightInfo.hash = kLightHash;
    lightInfo.radiance.x = 5000.0f;
    lightInfo.radiance.y = 4500.0f;
    lightInfo.radiance.z = 3500.0f;

    remixapi_LightInfoSphereEXT& sphere = g_sphere;
    std::memset(&sphere, 0, sizeof sphere);
    sphere.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_SPHERE_EXT;
    sphere.pNext = nullptr;
    sphere.position.x = 2495.0f;
    sphere.position.y = -1680.0f;
    sphere.position.z = 18.0f;
    sphere.radius = 1.0f;  // rtx.sceneScale = 0.01: game units are treated as cm
    sphere.shaping_hasvalue = 0;  // shaping disabled
    sphere.volumetricRadianceScale = 0.0f;
    lightInfo.pNext = &sphere;

    if (!g_api.CreateLight) {
        rhyLog("remix: interface has no CreateLight, giving up");
        return;
    }
    remixapi_ErrorCode ce = g_api.CreateLight(&lightInfo, &g_light);
    rhyLog("remix: CreateLight -> %u handle=%p", static_cast<unsigned>(ce),
           static_cast<void*>(g_light));
    if (ce == REMIXAPI_ERROR_CODE_SUCCESS && g_light) {
        g_ready = true;
    }
}

void rhyRemixFrame() {
    if (!g_ready || !g_light || !g_api.DrawLightInstance) {
        return;
    }
    ++g_drawCalls;
    // Test light follows CJ, 2 m above his head (CPed* at 0xB7CD98).
    if (g_api.DestroyLight && g_api.CreateLight) {
        uintptr_t ped = *reinterpret_cast<uintptr_t*>(0xB7CD98);
        if (ped) {
            uintptr_t m = *reinterpret_cast<uintptr_t*>(ped + 0x14);
            const float* pos = m ? reinterpret_cast<const float*>(m + 0x30) : reinterpret_cast<const float*>(ped + 0x4);
            g_sphere.position.x = pos[0]; g_sphere.position.y = pos[1]; g_sphere.position.z = pos[2] + 2.0f;
            g_api.DestroyLight(g_light);
            g_light = nullptr;
            if (g_api.CreateLight(&g_lightInfo, &g_light) != REMIXAPI_ERROR_CODE_SUCCESS || !g_light) { g_ready = false; return; }
        }
    }
    remixapi_ErrorCode r = g_api.DrawLightInstance(g_light);

    if (g_drawCalls <= 3) {
        rhyLog("remix: DrawLightInstance #%d -> %u", g_drawCalls,
               static_cast<unsigned>(r));
    } else if (r != REMIXAPI_ERROR_CODE_SUCCESS) {
        DWORD now = GetTickCount();
        if (now - g_lastErrTick >= 10000) {  // one line per 10 s
            g_lastErrTick = now;
            rhyLog("remix: DrawLightInstance #%d -> %u (non-zero)", g_drawCalls,
                   static_cast<unsigned>(r));
        }
    }
}
