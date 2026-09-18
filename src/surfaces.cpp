#include "surfaces.h"

#include "addresses.h"
#include "patch.h"

namespace {

constexpr int kMaxDownsampleStages = 3;

IDirect3DDevice9* g_surfaceDevice = nullptr;
IDirect3DSurface9* g_largeColor = nullptr;
IDirect3DSurface9* g_largeDepth = nullptr;
IDirect3DSurface9* g_downsampleChain[kMaxDownsampleStages] = {};
int g_downsampleCount = 0;
IDirect3DSurface9* g_clientColor = nullptr;
IDirect3DSurface9* g_clientDepth = nullptr;
D3DSURFACE_DESC g_colorDesc = {};
D3DFORMAT g_depthFormat = D3DFMT_UNKNOWN;
int g_requestedFactor = 4;
int g_activeFactor = 0;
bool g_passActive = false;
const void* g_passCamera = nullptr;

const void* g_clearedCamera = nullptr;
RwRGBA g_clearColor = {};
int g_clearFlags = 0;

void** g_deviceVtable = nullptr;
DeviceResetFn g_originalReset = nullptr;

bool HasStencil(D3DFORMAT format) {
    return format == D3DFMT_D15S1 || format == D3DFMT_D24S8 || format == D3DFMT_D24X4S4;
}

void ReleaseDownsampleChain() {
    for (int i = 0; i < kMaxDownsampleStages; ++i) {
        if (g_downsampleChain[i]) {
            g_downsampleChain[i]->Release();
            g_downsampleChain[i] = nullptr;
        }
    }
    g_downsampleCount = 0;
}

void ReleaseLargeTargets() {
    ReleaseDownsampleChain();
    if (g_largeDepth) {
        g_largeDepth->Release();
        g_largeDepth = nullptr;
    }
    if (g_largeColor) {
        g_largeColor->Release();
        g_largeColor = nullptr;
    }
    g_surfaceDevice = nullptr;
    g_colorDesc = {};
    g_depthFormat = D3DFMT_UNKNOWN;
    g_activeFactor = 0;
}

void ReleaseClientSurfaces() {
    if (g_clientDepth) {
        g_clientDepth->Release();
        g_clientDepth = nullptr;
    }
    if (g_clientColor) {
        g_clientColor->Release();
        g_clientColor = nullptr;
    }
    g_passActive = false;
    g_passCamera = nullptr;
}

// Plain single-sample surfaces at a multiple of the preview size. Rendering
// the client's preview into a multisampled target loses the depth comparison
// and the body shell of the model is rejected at every sample count, while
// the same pass into a single-sample target reproduces the client's own
// image exactly. Supersampling therefore does the anti-aliasing, and the box
// filter in StretchRect resolves it.
bool CreateLargeTargets(IDirect3DDevice9* device, const D3DSURFACE_DESC& color,
                        const D3DSURFACE_DESC& depth) {
    ReleaseLargeTargets();

    int factor = g_requestedFactor;
    while (factor >= 1) {
        const UINT width = color.Width * static_cast<UINT>(factor);
        const UINT height = color.Height * static_cast<UINT>(factor);
        IDirect3DSurface9* colorSurface = nullptr;
        IDirect3DSurface9* depthSurface = nullptr;

        HRESULT colorResult = device->CreateRenderTarget(width, height, color.Format, D3DMULTISAMPLE_NONE,
                                                         0, FALSE, &colorSurface, nullptr);
        HRESULT depthResult = E_FAIL;
        if (SUCCEEDED(colorResult)) {
            depthResult = device->CreateDepthStencilSurface(width, height, depth.Format, D3DMULTISAMPLE_NONE,
                                                            0, FALSE, &depthSurface, nullptr);
        }

        if (SUCCEEDED(colorResult) && SUCCEEDED(depthResult)) {
            g_surfaceDevice = device;
            g_largeColor = colorSurface;
            g_largeDepth = depthSurface;
            g_colorDesc = color;
            g_depthFormat = depth.Format;
            g_activeFactor = factor;

            // StretchRect filters bilinearly and reads only a 2x2 neighbourhood,
            // so a single 4x or 8x reduction would discard most of the image.
            // Halving repeatedly keeps every step at 2:1, where the bilinear tap
            // averages all four texels of each block.
            for (int step = factor / 2; step >= 2; step /= 2) {
                IDirect3DSurface9* stage = nullptr;
                if (FAILED(device->CreateRenderTarget(color.Width * static_cast<UINT>(step),
                                                      color.Height * static_cast<UINT>(step), color.Format,
                                                      D3DMULTISAMPLE_NONE, 0, FALSE, &stage, nullptr))) {
                    ReleaseDownsampleChain();
                    break;
                }
                g_downsampleChain[g_downsampleCount++] = stage;
            }
            return true;
        }

        if (depthSurface) {
            depthSurface->Release();
        }
        if (colorSurface) {
            colorSurface->Release();
        }
        factor /= 2;
    }

    return false;
}

bool EnsureLargeTargets(IDirect3DDevice9* device, const D3DSURFACE_DESC& color,
                        const D3DSURFACE_DESC& depth) {
    if (g_largeColor && g_largeDepth && g_surfaceDevice == device && g_activeFactor == g_requestedFactor &&
        g_colorDesc.Width == color.Width && g_colorDesc.Height == color.Height &&
        g_colorDesc.Format == color.Format && g_depthFormat == depth.Format) {
        return true;
    }
    return CreateLargeTargets(device, color, depth);
}

// The colour clear reproduces what the client did to its raster, so the
// reduced image keeps the original background colour and alpha. Depth and
// stencil are always cleared: these surfaces belong to the plugin, survive
// between previews, and the client never clears the stencil it asked for.
void ReplayCameraClear(IDirect3DDevice9* device, const void* camera) {
    DWORD flags = D3DCLEAR_ZBUFFER;
    D3DCOLOR color = D3DCOLOR_ARGB(0, 0, 0, 0);

    if (camera == g_clearedCamera && g_clearFlags != 0) {
        if (g_clearFlags & kClearImage) {
            flags |= D3DCLEAR_TARGET;
        }
        color = D3DCOLOR_ARGB(g_clearColor.alpha, g_clearColor.red, g_clearColor.green, g_clearColor.blue);
    } else {
        flags |= D3DCLEAR_TARGET;
    }

    if (HasStencil(g_depthFormat)) {
        flags |= D3DCLEAR_STENCIL;
    }

    device->Clear(0, nullptr, flags, color, 1.0f, 0);
}

bool ActivateLargeTargets(IDirect3DDevice9* device, const void* camera) {
    HRESULT result = device->SetDepthStencilSurface(nullptr);
    if (SUCCEEDED(result)) {
        result = device->SetRenderTarget(0, g_largeColor);
    }
    if (SUCCEEDED(result)) {
        result = device->SetDepthStencilSurface(g_largeDepth);
    }
    if (FAILED(result)) {
        device->SetDepthStencilSurface(nullptr);
        device->SetRenderTarget(0, g_clientColor);
        device->SetDepthStencilSurface(g_clientDepth);
        ReleaseLargeTargets();
        return false;
    }

    ReplayCameraClear(device, camera);

    // The client renders the preview with depth testing disabled and that is
    // left alone: its camera uses a 0.01 near plane against a 300 far plane,
    // so at the model's distance there is no usable depth precision, and
    // forcing the test on produces z-fighting that looks like holes.
    return true;
}

// Every surface this plugin creates lives in D3DPOOL_DEFAULT, and
// IDirect3DDevice9::Reset refuses to run while any such surface is alive. The
// game resets the device after it was lost, which is what alt-tab does in
// exclusive fullscreen, and RenderWare retries a failed reset every frame
// without drawing, so a preview rendered before the switch left the game on
// a black screen. Dropping the surfaces here lets the reset through.
HRESULT __stdcall DeviceResetHook(IDirect3DDevice9* device, D3DPRESENT_PARAMETERS* parameters) {
    ReleaseClientSurfaces();
    ReleaseLargeTargets();
    return g_originalReset(device, parameters);
}

} // namespace

void SetSupersampleFactor(int factor) {
    g_requestedFactor = factor;
}

bool BeginSupersampledPass(IDirect3DDevice9* device, const void* camera) {
    ReleaseClientSurfaces();
    if (FAILED(device->GetRenderTarget(0, &g_clientColor)) || !g_clientColor ||
        FAILED(device->GetDepthStencilSurface(&g_clientDepth)) || !g_clientDepth) {
        ReleaseClientSurfaces();
        return false;
    }

    D3DSURFACE_DESC color = {};
    D3DSURFACE_DESC depth = {};
    if (FAILED(g_clientColor->GetDesc(&color)) || FAILED(g_clientDepth->GetDesc(&depth)) ||
        color.MultiSampleType != D3DMULTISAMPLE_NONE) {
        ReleaseClientSurfaces();
        return false;
    }

    if (!EnsureLargeTargets(device, color, depth) || !ActivateLargeTargets(device, camera)) {
        ReleaseClientSurfaces();
        return false;
    }

    g_passActive = true;
    g_passCamera = camera;
    return true;
}

void EndSupersampledPass(IDirect3DDevice9* device) {
    if (device && g_clientColor && g_largeColor) {
        device->SetDepthStencilSurface(nullptr);
        const HRESULT targetResult = device->SetRenderTarget(0, g_clientColor);
        const HRESULT depthResult = device->SetDepthStencilSurface(g_clientDepth);
        if (SUCCEEDED(targetResult) && SUCCEEDED(depthResult)) {
            const D3DTEXTUREFILTERTYPE filter = g_activeFactor > 1 ? D3DTEXF_LINEAR : D3DTEXF_NONE;
            IDirect3DSurface9* stageSource = g_largeColor;
            HRESULT reduction = S_OK;
            for (int i = 0; i < g_downsampleCount && SUCCEEDED(reduction); ++i) {
                reduction = device->StretchRect(stageSource, nullptr, g_downsampleChain[i], nullptr,
                                                D3DTEXF_LINEAR);
                stageSource = g_downsampleChain[i];
            }
            if (SUCCEEDED(reduction)) {
                device->StretchRect(stageSource, nullptr, g_clientColor, nullptr, filter);
            }
        }
    }
    ReleaseClientSurfaces();
}

bool IsPassActive(const void* camera) {
    return g_passActive && camera == g_passCamera;
}

void RecordCameraClear(const void* camera, const RwRGBA& color, int flags) {
    g_clearedCamera = camera;
    g_clearColor = color;
    g_clearFlags = flags;
}

// The hook replaces the vtable entry rather than the function behind it, so
// it chains with whatever the client or other plugins put there and needs no
// knowledge of the d3d9.dll build. RenderWare creates one device per process
// and resets it in place, so a single vtable is all this ever sees.
bool EnsureDeviceResetHook(IDirect3DDevice9* device) {
    void** vtable = *reinterpret_cast<void***>(device);
    if (g_deviceVtable) {
        return vtable == g_deviceVtable;
    }

    auto original = reinterpret_cast<DeviceResetFn>(vtable[kDeviceResetSlot]);
    const void* hook = reinterpret_cast<const void*>(&DeviceResetHook);
    if (!original || !WriteMemory(&vtable[kDeviceResetSlot], &hook, sizeof(hook))) {
        return false;
    }

    g_originalReset = original;
    g_deviceVtable = vtable;
    return true;
}

void RemoveDeviceResetHook() {
    if (!g_deviceVtable) {
        return;
    }

    // Another plugin may have hooked the slot after this one, in which case
    // the entry is theirs to restore.
    void* current = nullptr;
    if (SafeCopy(reinterpret_cast<uintptr_t>(&g_deviceVtable[kDeviceResetSlot]), &current, sizeof(current)) &&
        current == reinterpret_cast<void*>(&DeviceResetHook)) {
        const void* original = reinterpret_cast<const void*>(g_originalReset);
        WriteMemory(&g_deviceVtable[kDeviceResetSlot], &original, sizeof(original));
    }
    g_deviceVtable = nullptr;
    g_originalReset = nullptr;
}
