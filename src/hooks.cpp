#include "hooks.h"

#include "addresses.h"
#include "patch.h"
#include "samp.h"
#include "surfaces.h"

#include <windows.h>
#include <intrin.h>

namespace {

Detour g_beginUpdateDetour = {kRwCameraBeginUpdate, {}, false};
Detour g_endUpdateDetour = {kRwCameraEndUpdate, {}, false};
Detour g_cameraClearDetour = {kRwCameraClear, {}, false};
Detour g_rasterCreateDetour = {kRwRasterCreate, {}, false};

RasterCreateFn g_rasterCreateTrampoline = nullptr;
int g_rasterScale = 1;

IDirect3DDevice9* GetDevice() {
    return static_cast<IDirect3DDevice9*>(reinterpret_cast<GetD3DDeviceFn>(kRwD3D9GetCurrentDevice)());
}

CameraUpdateFn CameraDispatch(void* camera, size_t offset) {
    return *reinterpret_cast<CameraUpdateFn*>(reinterpret_cast<uint8_t*>(camera) + offset);
}

void* __cdecl RwCameraBeginUpdateHook(void* camera) {
    const void* returnAddress = _ReturnAddress();

    void* result = CameraDispatch(camera, kCameraBeginUpdateOffset)(camera);
    if (!result || !IsMultiplayerCaller(returnAddress) || camera == *reinterpret_cast<void**>(kSceneCamera)) {
        return result;
    }

    IDirect3DDevice9* device = GetDevice();
    if (!device) {
        return result;
    }

    // No surface is created unless it can be released before a device reset,
    // otherwise the next alt-tab would hang the game.
    if (!EnsureDeviceResetHook(device)) {
        return result;
    }

    BeginSupersampledPass(device, camera);
    return result;
}

void* __cdecl RwCameraEndUpdateHook(void* camera) {
    if (IsPassActive(camera)) {
        EndSupersampledPass(GetDevice());
    }
    return CameraDispatch(camera, kCameraEndUpdateOffset)(camera);
}

void* __cdecl RwCameraClearHook(void* camera, const RwRGBA* color, int flags) {
    if (color && IsMultiplayerCaller(_ReturnAddress())) {
        RecordCameraClear(camera, *color, flags);
    }

    void* engine = *reinterpret_cast<void**>(kRwEngineInstance);
    auto clear = *reinterpret_cast<DeviceCameraClearFn*>(reinterpret_cast<uint8_t*>(engine) + kEngineCameraClearOffset);
    return clear(camera, color, flags) ? camera : nullptr;
}

// The client hardcodes a 256x256 preview raster, which is the real limit when
// a preview textdraw is displayed larger than that. Enlarging the raster keeps
// the camera, the view window and the projection untouched; only the pixel
// grid the model is rasterised on gets denser.
void* __cdecl RwRasterCreateHook(int width, int height, int depth, int flags) {
    if (g_rasterScale > 1 && width == kPreviewRasterSize && height == kPreviewRasterSize &&
        width * g_rasterScale <= kMaxPreviewRasterSize) {
        const int type = flags & kRasterTypeMask;
        if ((type == kRasterTypeCameraTexture || type == kRasterTypeZBuffer) && IsMultiplayerCaller(_ReturnAddress())) {
            width *= g_rasterScale;
            height *= g_rasterScale;
        }
    }

    return g_rasterCreateTrampoline(width, height, depth, flags);
}

// RwRasterCreate does not start with a tail jump, so its first instruction is
// relocated into a stub that continues into the untouched remainder.
bool InstallRasterCreateDetour() {
    void* stub = CreateTrampoline(kRwRasterCreate, kRasterCreateBytes, sizeof(kRasterCreateBytes));
    if (!stub) {
        return false;
    }

    if (!InstallDetour(g_rasterCreateDetour, reinterpret_cast<const void*>(&RwRasterCreateHook))) {
        VirtualFree(stub, 0, MEM_RELEASE);
        return false;
    }

    g_rasterCreateTrampoline = reinterpret_cast<RasterCreateFn>(stub);
    return true;
}

} // namespace

bool InstallHooks(int previewScale) {
    if (reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)) != kImageBase ||
        !IsExecutableAddress(kRwCameraBeginUpdate) || !IsExecutableAddress(kRwCameraEndUpdate) ||
        !IsExecutableAddress(kRwCameraClear) || !IsExecutableAddress(kRwRasterCreate) ||
        !IsExecutableAddress(kRwD3D9GetCurrentDevice)) {
        return false;
    }

    if (!MatchesBytes(kRwCameraBeginUpdate, kBeginUpdateBytes, sizeof(kBeginUpdateBytes)) ||
        !MatchesBytes(kRwCameraEndUpdate, kEndUpdateBytes, sizeof(kEndUpdateBytes)) ||
        !MatchesBytes(kRwCameraClear, kCameraClearBytes, sizeof(kCameraClearBytes))) {
        return false;
    }

    // The clear and end hooks are inert without the begin hook, so the begin
    // hook goes in last and a failure between them leaves the game as it was.
    if (!InstallDetour(g_cameraClearDetour, reinterpret_cast<const void*>(&RwCameraClearHook)) ||
        !InstallDetour(g_endUpdateDetour, reinterpret_cast<const void*>(&RwCameraEndUpdateHook)) ||
        !InstallDetour(g_beginUpdateDetour, reinterpret_cast<const void*>(&RwCameraBeginUpdateHook))) {
        RemoveDetour(g_beginUpdateDetour);
        RemoveDetour(g_endUpdateDetour);
        RemoveDetour(g_cameraClearDetour);
        return false;
    }

    g_rasterScale = previewScale;
    if (g_rasterScale > 1) {
        if (!MatchesBytes(kRwRasterCreate, kRasterCreateBytes, sizeof(kRasterCreateBytes)) ||
            !InstallRasterCreateDetour()) {
            g_rasterScale = 1;
        }
    }
    return true;
}

void RemoveHooks() {
    RemoveDetour(g_beginUpdateDetour);
    RemoveDetour(g_endUpdateDetour);
    RemoveDetour(g_cameraClearDetour);
    RemoveDetour(g_rasterCreateDetour);
    RemoveDeviceResetHook();
}
