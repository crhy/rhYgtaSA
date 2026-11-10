#include <windows.h>
#include <d3d9.h>

#include "log.h"
#include "remix.h"

// The game's D3D9 device (0 until rendering starts); in the RTX build it belongs
// to the Remix bridge client d3d9.dll in the game folder.
static const void* const kDeviceSlot = reinterpret_cast<const void*>(0xC97C28);

// IDirect3DDevice9 vtable: 0..2 = IUnknown, Present is entry 17.
static const int kPresentEntry = 17;

typedef HRESULT(__stdcall* rhyPresentFn)(IDirect3DDevice9*, const RECT*,
                                        const RECT*, HWND, const RGNDATA*);

static rhyPresentFn g_origPresent = nullptr;

static HRESULT __stdcall hkPresent(IDirect3DDevice9* dev, const RECT* srcRect,
                                   const RECT* dstRect, HWND focusWindow,
                                   const RGNDATA* regions) {
    rhyRemixInitOnce();  // first frame, on the render thread
    rhyRemixFrame();     // draw the test light before presenting

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
