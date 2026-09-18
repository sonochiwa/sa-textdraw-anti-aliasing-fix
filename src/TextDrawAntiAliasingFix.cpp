// SA-MP model preview textdraws are rendered by the client through a
// RenderWare camera of its own into a hardcoded 256x256 raster, with no
// anti-aliasing, so a preview shown larger than that is both blocky and
// jagged. Multisampling the raster does not work: the client's preview pass
// into a multisampled target loses the depth comparison and the body shell
// of the model is rejected. The plugin instead detours the RenderWare camera
// entry points so a preview pass entered from samp.dll renders into a plain
// surface several times the raster size and is halved back into it, and
// detours RwRasterCreate so the raster itself is created larger. The
// client's camera, view window and projection are untouched.

#include "config.h"
#include "hooks.h"
#include "surfaces.h"

#include <windows.h>

namespace {

bool g_installed = false;

DWORD WINAPI Initialize(void* parameter) {
    const Settings settings = LoadSettings(static_cast<HMODULE>(parameter));
    SetSupersampleFactor(settings.supersample);
    // The RenderWare device exists only after the game has finished its own
    // startup, which the loader does not wait for.
    Sleep(1000);
    g_installed = InstallHooks(settings.previewScale);
    return 0;
}

} // namespace

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void*) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
        if (HANDLE thread = CreateThread(nullptr, 0, Initialize, instance, 0, nullptr)) {
            CloseHandle(thread);
        }
    } else if (reason == DLL_PROCESS_DETACH && g_installed) {
        RemoveHooks();
        g_installed = false;
    }
    return TRUE;
}
