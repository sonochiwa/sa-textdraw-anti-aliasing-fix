#define NOMINMAX
#include <windows.h>
#include <d3d9.h>
#include <intrin.h>

#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>

namespace {

constexpr uintptr_t kImageBase = 0x00400000;
constexpr uintptr_t kRwCameraEndUpdate = 0x007EE180;
constexpr uintptr_t kRwCameraBeginUpdate = 0x007EE190;
constexpr uintptr_t kRwCameraClear = 0x007EE340;
constexpr uintptr_t kRwD3D9GetCurrentDevice = 0x007F9D50;
constexpr uintptr_t kRwRasterCreate = 0x007FB230;
constexpr uintptr_t kRwEngineInstance = 0x00C97B24;
constexpr uintptr_t kSceneCamera = 0x00C17038;

// The client asks for these rasters when it builds its model preview camera.
constexpr int kPreviewRasterSize = 256;
constexpr int kRasterTypeMask = 0x07;
constexpr int kRasterTypeZBuffer = 0x01;
constexpr int kRasterTypeCameraTexture = 0x05;
constexpr int kMaxPreviewRasterSize = 2048;
constexpr int kMaxDownsampleStages = 3;

// RwCamera dispatch pointers, as used by the RwCameraBeginUpdate and
// RwCameraEndUpdate thunks in the retail executable.
constexpr size_t kCameraBeginUpdateOffset = 0x18;
constexpr size_t kCameraEndUpdateOffset = 0x1C;
// Device camera-clear entry inside the RenderWare engine instance.
constexpr size_t kEngineCameraClearOffset = 0x9C;

constexpr int kClearImage = 0x01;
constexpr int kClearDepth = 0x02;
constexpr int kClearStencil = 0x04;

constexpr uint8_t kBeginUpdateBytes[] = {0x8B, 0x44, 0x24, 0x04, 0x89, 0x44,
                                         0x24, 0x04, 0xFF, 0x60, 0x18};
constexpr uint8_t kEndUpdateBytes[] = {0x8B, 0x44, 0x24, 0x04, 0x89, 0x44,
                                       0x24, 0x04, 0xFF, 0x60, 0x1C};
constexpr uint8_t kCameraClearBytes[] = {0x8B, 0x4C, 0x24, 0x0C, 0xA1, 0x24,
                                         0x7B, 0xC9, 0x00, 0x8B, 0x54, 0x24,
                                         0x08, 0x56, 0x8B, 0x74, 0x24, 0x08};
// The first instruction of RwRasterCreate is exactly five bytes and position
// independent, so it can be relocated into a trampoline verbatim.
constexpr uint8_t kRasterCreateBytes[] = {0xA1, 0x24, 0x7B, 0xC9, 0x00};

struct RwRGBA {
    uint8_t red;
    uint8_t green;
    uint8_t blue;
    uint8_t alpha;
};

using CameraUpdateFn = void*(__cdecl*)(void*);
using DeviceCameraClearFn = int(__cdecl*)(void*, const RwRGBA*, int);
using GetD3DDeviceFn = void*(__cdecl*)();
using RasterCreateFn = void*(__cdecl*)(int, int, int, int);

struct Detour {
    uintptr_t address;
    uint8_t original[5];
    bool installed;
};

Detour g_beginUpdateDetour = {kRwCameraBeginUpdate, {}, false};
Detour g_endUpdateDetour = {kRwCameraEndUpdate, {}, false};
Detour g_cameraClearDetour = {kRwCameraClear, {}, false};
Detour g_rasterCreateDetour = {kRwRasterCreate, {}, false};

RasterCreateFn g_rasterCreateTrampoline = nullptr;
int g_rasterScale = 1;
bool g_logging = false;
bool g_reportedTarget = false;
bool g_reportedResolve = false;
bool g_reportedClear = false;
bool g_reportedStates = false;
bool g_reportedEndStates = false;
volatile BOOL g_dumpPending = FALSE;
bool g_dumpEnabled = false;
int g_dumpIndex = 0;
wchar_t g_dumpBasePath[MAX_PATH] = {};
wchar_t g_logPath[MAX_PATH] = {};

HMODULE g_sampModule = nullptr;
uintptr_t g_sampStart = 0;
uintptr_t g_sampEnd = 0;

IDirect3DDevice9* g_surfaceDevice = nullptr;
IDirect3DSurface9* g_msaaColor = nullptr;
IDirect3DSurface9* g_msaaDepth = nullptr;
IDirect3DSurface9* g_downsampleChain[kMaxDownsampleStages] = {};
int g_downsampleCount = 0;
IDirect3DSurface9* g_resolveColor = nullptr;
IDirect3DSurface9* g_resolveDepth = nullptr;
D3DSURFACE_DESC g_colorDesc = {};
D3DFORMAT g_depthFormat = D3DFMT_UNKNOWN;
volatile int g_requestedFactor = 4;
int g_activeFactor = 0;
volatile BOOL g_reloadPending = FALSE;
volatile BOOL g_enabled = TRUE;
volatile BOOL g_shuttingDown = FALSE;
bool g_hotkeyEnabled = true;
int g_hotkeyModifier = VK_MENU;
int g_hotkeyKey = 'T';
wchar_t g_iniPath[MAX_PATH] = {};
bool g_resolveActive = false;
void* g_resolveCamera = nullptr;
void* g_previewCamera = nullptr;
bool g_installed = false;

void* g_clearedCamera = nullptr;
RwRGBA g_clearColor = {};
int g_clearFlags = 0;

void Log(const char* format, ...) {
    if (!g_logging || g_logPath[0] == L'\0')
        return;

    FILE* file = nullptr;
    if (_wfopen_s(&file, g_logPath, L"a") != 0 || !file)
        return;

    SYSTEMTIME now = {};
    GetLocalTime(&now);
    fprintf(file, "[%02u:%02u:%02u.%03u] ", now.wHour, now.wMinute, now.wSecond,
            now.wMilliseconds);

    va_list args;
    va_start(args, format);
    vfprintf(file, format, args);
    va_end(args);

    fputc('\n', file);
    fclose(file);
}

bool SafeCopy(uintptr_t address, void* result, size_t size) {
    __try {
        memcpy(result, reinterpret_cast<const void*>(address), size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool IsExecutableAddress(uintptr_t address) {
    MEMORY_BASIC_INFORMATION info = {};
    if (!address || VirtualQuery(reinterpret_cast<const void*>(address), &info,
                                 sizeof(info)) != sizeof(info))
        return false;

    constexpr DWORD executablePages = PAGE_EXECUTE | PAGE_EXECUTE_READ |
                                      PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    return info.State == MEM_COMMIT && (info.Protect & executablePages) != 0;
}

bool WriteMemory(void* destination, const void* source, size_t size) {
    DWORD oldProtect = 0;
    if (!VirtualProtect(destination, size, PAGE_EXECUTE_READWRITE, &oldProtect))
        return false;

    memcpy(destination, source, size);
    FlushInstructionCache(GetCurrentProcess(), destination, size);

    DWORD unused = 0;
    VirtualProtect(destination, size, oldProtect, &unused);
    return true;
}

bool MatchesBytes(uintptr_t address, const uint8_t* expected, size_t size) {
    uint8_t actual[32] = {};
    if (size > sizeof(actual) || !SafeCopy(address, actual, size))
        return false;
    return memcmp(actual, expected, size) == 0;
}

bool ResolvePaths(HMODULE module) {
    wchar_t basePath[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameW(module, basePath, MAX_PATH);
    if (length == 0 || length >= MAX_PATH)
        return false;

    wchar_t* slash = wcsrchr(basePath, L'\\');
    if (!slash)
        return false;

    const size_t room = static_cast<size_t>(MAX_PATH - (slash + 1 - basePath));
    wcscpy_s(slash + 1, room, L"TextDrawAntiAliasingFix.ini");
    wcscpy_s(g_iniPath, basePath);
    wcscpy_s(slash + 1, room, L"TextDrawAntiAliasingFix.log");
    wcscpy_s(g_logPath, basePath);
    wcscpy_s(slash + 1, room, L"TextDrawAntiAliasingFix");
    wcscpy_s(g_dumpBasePath, basePath);
    return true;
}

void LoadSupersampleFactor() {
    const int configured = GetPrivateProfileIntW(
        L"antiAliasing", L"supersample", 4, g_iniPath);
    int factor;
    if (configured >= 8)
        factor = 8;
    else if (configured >= 4)
        factor = 4;
    else if (configured >= 2)
        factor = 2;
    else
        factor = 1;

    if (factor != g_requestedFactor) {
        g_requestedFactor = factor;
        g_reloadPending = TRUE;
    }
}

void LoadConfiguration(HMODULE module) {
    if (!ResolvePaths(module))
        return;

    g_enabled = GetPrivateProfileIntW(L"general", L"isEnabled", 1, g_iniPath) != 0;
    g_hotkeyEnabled =
        GetPrivateProfileIntW(L"general", L"hotkeyEnabled", 1, g_iniPath) != 0;
    g_hotkeyModifier =
        GetPrivateProfileIntW(L"general", L"hotkeyModifier", VK_MENU, g_iniPath);
    g_hotkeyKey = GetPrivateProfileIntW(L"general", L"hotkeyKey", 'T', g_iniPath);
    g_logging = GetPrivateProfileIntW(L"general", L"logging", 0, g_iniPath) != 0;
    g_dumpEnabled =
        GetPrivateProfileIntW(L"general", L"dumpPreviews", 0, g_iniPath) != 0;
    g_dumpPending = g_dumpEnabled ? TRUE : FALSE;

    const int scale =
        GetPrivateProfileIntW(L"antiAliasing", L"previewScale", 2, g_iniPath);
    if (scale >= 8)
        g_rasterScale = 8;
    else if (scale >= 4)
        g_rasterScale = 4;
    else if (scale >= 2)
        g_rasterScale = 2;
    else
        g_rasterScale = 1;

    LoadSupersampleFactor();
    g_reloadPending = FALSE;

    if (g_logging)
        DeleteFileW(g_logPath);
    Log("TextDraw Anti-Aliasing Fix loaded: isEnabled=%d supersample=%d "
        "previewScale=%d hotkey=%d+%d",
        g_enabled ? 1 : 0, g_requestedFactor, g_rasterScale,
        g_hotkeyEnabled ? g_hotkeyModifier : 0, g_hotkeyEnabled ? g_hotkeyKey : 0);
}

void StoreEnabledState() {
    WritePrivateProfileStringW(L"general", L"isEnabled", g_enabled ? L"1" : L"0",
                               g_iniPath);
}

bool IsGameForeground() {
    DWORD processId = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &processId);
    return processId == GetCurrentProcessId();
}

bool IsHotkeyDown() {
    if (g_hotkeyModifier != 0 && (GetAsyncKeyState(g_hotkeyModifier) & 0x8000) == 0)
        return false;
    return (GetAsyncKeyState(g_hotkeyKey) & 0x8000) != 0;
}

// The hotkey re-reads the INI when it turns the fix back on, so a different
// supersample can be compared without restarting the game.
DWORD WINAPI HotkeyThread(void*) {
    bool wasDown = false;
    while (!g_shuttingDown) {
        const bool isDown = IsGameForeground() && IsHotkeyDown();
        if (isDown && !wasDown) {
            g_enabled = !g_enabled;
            if (g_enabled) {
                if (g_dumpEnabled) {
                    // Diagnostic mode steps through the multisample modes so one
                    // run produces every configuration, including no
                    // multisampling at all, which isolates the surface swap
                    // from the sampling.
                    int next = g_requestedFactor / 2;
                    if (next < 1)
                        next = 8;
                    g_requestedFactor = next;
                    g_reloadPending = TRUE;
                } else {
                    LoadSupersampleFactor();
                }
            }
            StoreEnabledState();
            // Each toggle asks for one more sample, so the two states can be
            // compared as files rather than by eye.
            if (g_dumpEnabled)
                g_dumpPending = TRUE;
            Log("hotkey: isEnabled=%d supersample=%d", g_enabled ? 1 : 0,
                g_requestedFactor);
        }
        wasDown = isDown;
        Sleep(30);
    }
    return 0;
}

IDirect3DDevice9* GetDevice() {
    return static_cast<IDirect3DDevice9*>(
        reinterpret_cast<GetD3DDeviceFn>(kRwD3D9GetCurrentDevice)());
}

// The preview cameras are owned by the multiplayer client, so the plugin only
// reacts to RenderWare camera updates that are entered from samp.dll.
bool IsMultiplayerCaller(const void* returnAddress) {
    if (!g_sampModule) {
        g_sampModule = GetModuleHandleW(L"samp.dll");
        if (!g_sampModule)
            return false;

        const auto base = reinterpret_cast<uintptr_t>(g_sampModule);
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(g_sampModule);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
            base + static_cast<uintptr_t>(dos->e_lfanew));
        g_sampStart = base;
        g_sampEnd = base + nt->OptionalHeader.SizeOfImage;
    }

    const auto address = reinterpret_cast<uintptr_t>(returnAddress);
    return address >= g_sampStart && address < g_sampEnd;
}

bool HasStencil(D3DFORMAT format) {
    return format == D3DFMT_D15S1 || format == D3DFMT_D24S8 ||
           format == D3DFMT_D24X4S4;
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

void ReleaseResolveTargets() {
    ReleaseDownsampleChain();
    if (g_msaaDepth) {
        g_msaaDepth->Release();
        g_msaaDepth = nullptr;
    }
    if (g_msaaColor) {
        g_msaaColor->Release();
        g_msaaColor = nullptr;
    }
    g_surfaceDevice = nullptr;
    g_colorDesc = {};
    g_depthFormat = D3DFMT_UNKNOWN;
    g_activeFactor = 0;
}

void ReleaseFrameSurfaces() {
    if (g_resolveDepth) {
        g_resolveDepth->Release();
        g_resolveDepth = nullptr;
    }
    if (g_resolveColor) {
        g_resolveColor->Release();
        g_resolveColor = nullptr;
    }
    g_resolveActive = false;
    g_resolveCamera = nullptr;
}

bool CreateResolveTargets(IDirect3DDevice9* device,
                          const D3DSURFACE_DESC& color,
                          const D3DSURFACE_DESC& depth) {
    ReleaseResolveTargets();

    // Plain, single sample surfaces at a multiple of the preview size. Measured
    // on nineteen dumped previews: rendering the client's preview into a
    // multisampled target loses the depth comparison and the body shell of the
    // model is rejected, at every sample count from 2x up, while the same swap
    // into a single sample target reproduces the client's own image exactly.
    // Supersampling therefore does the anti-aliasing, and the box filter in
    // StretchRect resolves it.
    int factor = g_requestedFactor;
    while (factor >= 1) {
        const UINT width = color.Width * static_cast<UINT>(factor);
        const UINT height = color.Height * static_cast<UINT>(factor);
        IDirect3DSurface9* colorSurface = nullptr;
        IDirect3DSurface9* depthSurface = nullptr;

        HRESULT colorResult = device->CreateRenderTarget(
            width, height, color.Format, D3DMULTISAMPLE_NONE, 0, FALSE,
            &colorSurface, nullptr);
        HRESULT depthResult = E_FAIL;
        if (SUCCEEDED(colorResult)) {
            depthResult = device->CreateDepthStencilSurface(
                width, height, depth.Format, D3DMULTISAMPLE_NONE, 0, FALSE,
                &depthSurface, nullptr);
        }

        if (SUCCEEDED(colorResult) && SUCCEEDED(depthResult)) {
            g_surfaceDevice = device;
            g_msaaColor = colorSurface;
            g_msaaDepth = depthSurface;
            g_colorDesc = color;
            g_depthFormat = depth.Format;
            g_activeFactor = factor;

            // StretchRect filters bilinearly, so it only ever reads a 2x2
            // neighbourhood. Reducing 4x or 8x in one step would discard most
            // of the supersampled image instead of averaging it. Halving
            // repeatedly keeps every step at exactly 2:1, where the bilinear
            // tap lands in the centre of each 2x2 block and averages all four
            // texels, which is the box filter this needs.
            for (int step = factor / 2; step >= 2; step /= 2) {
                IDirect3DSurface9* stage = nullptr;
                if (FAILED(device->CreateRenderTarget(
                        color.Width * static_cast<UINT>(step),
                        color.Height * static_cast<UINT>(step), color.Format,
                        D3DMULTISAMPLE_NONE, 0, FALSE, &stage, nullptr))) {
                    Log("downsample stage %dx could not be created", step);
                    ReleaseDownsampleChain();
                    break;
                }
                g_downsampleChain[g_downsampleCount++] = stage;
            }
            Log("downsample chain: %d intermediate stage(s)", g_downsampleCount);
            Log("created %ux%u supersampled pair for a %ux%u preview (%dx), "
                "color format %u, depth format %u",
                width, height, color.Width, color.Height, factor,
                static_cast<unsigned>(color.Format),
                static_cast<unsigned>(depth.Format));
            return true;
        }

        Log("%dx supersampling rejected: color 0x%08X, depth 0x%08X", factor,
            static_cast<unsigned>(colorResult), static_cast<unsigned>(depthResult));
        if (depthSurface)
            depthSurface->Release();
        if (colorSurface)
            colorSurface->Release();
        factor /= 2;
    }

    Log("no supersampling factor accepted for %ux%u", color.Width, color.Height);
    return false;
}

bool EnsureResolveTargets(IDirect3DDevice9* device,
                          const D3DSURFACE_DESC& color,
                          const D3DSURFACE_DESC& depth) {
    if (g_msaaColor && g_msaaDepth && g_surfaceDevice == device &&
        g_activeFactor == g_requestedFactor &&
        g_colorDesc.Width == color.Width && g_colorDesc.Height == color.Height &&
        g_colorDesc.Format == color.Format && g_depthFormat == depth.Format)
        return true;
    return CreateResolveTargets(device, color, depth);
}

// The color clear reproduces what the client did to the single-sample raster, so
// the resolved image keeps the original background color and alpha. Depth and
// stencil are always cleared: these surfaces belong to the plugin, they survive
// between previews, and the client never clears the stencil channel it asked
// for. Leaving stale stencil content there makes the scene's leftover stencil
// test reject parts of the model, which reads as a see-through body.
void ReplayCameraClear(IDirect3DDevice9* device, const void* camera) {
    DWORD flags = D3DCLEAR_ZBUFFER;
    D3DCOLOR color = D3DCOLOR_ARGB(0, 0, 0, 0);

    if (camera == g_clearedCamera && g_clearFlags != 0) {
        if (g_clearFlags & kClearImage)
            flags |= D3DCLEAR_TARGET;
        color = D3DCOLOR_ARGB(g_clearColor.alpha, g_clearColor.red,
                              g_clearColor.green, g_clearColor.blue);
    } else {
        flags |= D3DCLEAR_TARGET;
    }

    if (HasStencil(g_depthFormat))
        flags |= D3DCLEAR_STENCIL;

    const HRESULT result = device->Clear(0, nullptr, flags, color, 1.0f, 0);
    if (!g_reportedClear) {
        g_reportedClear = true;
        Log("first clear: flags 0x%02X color 0x%08X result 0x%08X",
            static_cast<unsigned>(flags), static_cast<unsigned>(color),
            static_cast<unsigned>(result));
    }
}

// Writes the surface currently bound as render target 0 to an uncompressed
// 32-bit TGA next to the plugin. This is the only way to see what the preview
// texture actually holds in colour and in alpha, instead of inferring it from
// what the finished frame looks like on screen.
void DumpRenderTarget(IDirect3DDevice9* device, const wchar_t* tag) {
    IDirect3DSurface9* source = nullptr;
    if (FAILED(device->GetRenderTarget(0, &source)) || !source)
        return;

    D3DSURFACE_DESC desc = {};
    IDirect3DSurface9* readable = nullptr;
    D3DLOCKED_RECT locked = {};
    bool locked_ok = false;

    if (SUCCEEDED(source->GetDesc(&desc)) &&
        SUCCEEDED(device->CreateOffscreenPlainSurface(
            desc.Width, desc.Height, desc.Format, D3DPOOL_SYSTEMMEM, &readable,
            nullptr)) &&
        SUCCEEDED(device->GetRenderTargetData(source, readable)) &&
        SUCCEEDED(readable->LockRect(&locked, nullptr, D3DLOCK_READONLY)))
        locked_ok = true;

    if (locked_ok) {
        wchar_t path[MAX_PATH] = {};
        swprintf_s(path, L"%s-preview-%02d-%s.tga", g_dumpBasePath, g_dumpIndex,
                   tag);

        FILE* file = nullptr;
        if (_wfopen_s(&file, path, L"wb") == 0 && file) {
            const auto width = static_cast<uint16_t>(desc.Width);
            const auto height = static_cast<uint16_t>(desc.Height);
            uint8_t header[18] = {};
            header[2] = 2;                       // uncompressed true colour
            memcpy(&header[12], &width, 2);
            memcpy(&header[14], &height, 2);
            header[16] = 32;                     // bits per pixel
            header[17] = 0x28;                   // 8 alpha bits, top-left origin
            fwrite(header, 1, sizeof(header), file);

            const auto* rows = static_cast<const uint8_t*>(locked.pBits);
            for (UINT y = 0; y < desc.Height; ++y)
                fwrite(rows + static_cast<size_t>(y) * locked.Pitch, 4,
                       desc.Width, file);
            fclose(file);

            Log("dumped preview %02d (%s): %ux%u format %u", g_dumpIndex, "tga",
                desc.Width, desc.Height, static_cast<unsigned>(desc.Format));
        }
        readable->UnlockRect();
        ++g_dumpIndex;
    }

    if (readable)
        readable->Release();
    source->Release();
}

bool ActivateResolveTargets(IDirect3DDevice9* device, const void* camera) {
    HRESULT result = device->SetDepthStencilSurface(nullptr);
    if (SUCCEEDED(result))
        result = device->SetRenderTarget(0, g_msaaColor);
    if (SUCCEEDED(result))
        result = device->SetDepthStencilSurface(g_msaaDepth);
    if (FAILED(result)) {
        device->SetDepthStencilSurface(nullptr);
        device->SetRenderTarget(0, g_resolveColor);
        device->SetDepthStencilSurface(g_resolveDepth);
        ReleaseResolveTargets();
        return false;
    }

    ReplayCameraClear(device, camera);

    if (!g_reportedStates) {
        g_reportedStates = true;
        DWORD zEnable = 0, zWrite = 0, zFunc = 0, stencil = 0, alphaBlend = 0,
              alphaTest = 0, cull = 0, colorWrite = 0;
        device->GetRenderState(D3DRS_ZENABLE, &zEnable);
        device->GetRenderState(D3DRS_ZWRITEENABLE, &zWrite);
        device->GetRenderState(D3DRS_ZFUNC, &zFunc);
        device->GetRenderState(D3DRS_STENCILENABLE, &stencil);
        device->GetRenderState(D3DRS_ALPHABLENDENABLE, &alphaBlend);
        device->GetRenderState(D3DRS_ALPHATESTENABLE, &alphaTest);
        device->GetRenderState(D3DRS_CULLMODE, &cull);
        device->GetRenderState(D3DRS_COLORWRITEENABLE, &colorWrite);
        Log("states at preview start: zEnable=%lu zWrite=%lu zFunc=%lu "
            "stencil=%lu alphaBlend=%lu alphaTest=%lu cull=%lu colorWrite=0x%lX",
            zEnable, zWrite, zFunc, stencil, alphaBlend, alphaTest, cull,
            colorWrite);
    }

    // Nothing else about the pass is touched. The client renders the preview
    // with depth testing disabled, which is deliberate: its camera uses a 0.01
    // near plane against a 300 far plane, so at the distance the model sits at
    // there is no usable depth precision left. Forcing the test on produces
    // z-fighting that looks like holes in the model, which measurement on
    // thirteen dumped previews confirmed.
    return true;
}

void* __cdecl RwCameraBeginUpdateHook(void* camera) {
    const void* returnAddress = _ReturnAddress();

    void* result = reinterpret_cast<CameraUpdateFn>(
        *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(camera) +
                                  kCameraBeginUpdateOffset))(camera);
    if (!result || !IsMultiplayerCaller(returnAddress) ||
        camera == *reinterpret_cast<void**>(kSceneCamera))
        return result;

    IDirect3DDevice9* device = GetDevice();
    if (!device) {
        Log("preview camera %p seen but RwD3D9GetCurrentD3DDevice returned null",
            camera);
        return result;
    }

    // Remembered even while the fix is off, so the end hook can dump what the
    // client rendered on its own for comparison.
    g_previewCamera = camera;

    // The hotkey thread only raises flags; the surfaces are always touched from
    // the rendering thread.
    if (g_reloadPending) {
        g_reloadPending = FALSE;
        ReleaseResolveTargets();
    }
    if (!g_enabled)
        return result;

    ReleaseFrameSurfaces();
    if (FAILED(device->GetRenderTarget(0, &g_resolveColor)) || !g_resolveColor ||
        FAILED(device->GetDepthStencilSurface(&g_resolveDepth)) || !g_resolveDepth) {
        ReleaseFrameSurfaces();
        return result;
    }

    D3DSURFACE_DESC color = {};
    D3DSURFACE_DESC depth = {};
    if (FAILED(g_resolveColor->GetDesc(&color)) ||
        FAILED(g_resolveDepth->GetDesc(&depth))) {
        ReleaseFrameSurfaces();
        return result;
    }

    if (!g_reportedTarget) {
        g_reportedTarget = true;
        Log("preview target: %ux%u color format %u multisample %u, depth %ux%u "
            "format %u multisample %u",
            color.Width, color.Height, static_cast<unsigned>(color.Format),
            static_cast<unsigned>(color.MultiSampleType), depth.Width, depth.Height,
            static_cast<unsigned>(depth.Format),
            static_cast<unsigned>(depth.MultiSampleType));
    }

    if (color.MultiSampleType != D3DMULTISAMPLE_NONE) {
        ReleaseFrameSurfaces();
        return result;
    }

    if (!EnsureResolveTargets(device, color, depth) ||
        !ActivateResolveTargets(device, camera)) {
        ReleaseFrameSurfaces();
        return result;
    }

    g_resolveActive = true;
    g_resolveCamera = camera;
    return result;
}

void* __cdecl RwCameraEndUpdateHook(void* camera) {
    // The fix being off is a valid state to sample: the render target then still
    // holds exactly what the client drew by itself.
    if (!g_resolveActive && g_dumpPending && camera == g_previewCamera) {
        g_dumpPending = FALSE;
        if (IDirect3DDevice9* device = GetDevice())
            DumpRenderTarget(device, L"off");
    }

    if (g_resolveActive && camera == g_resolveCamera) {
        IDirect3DDevice9* device = GetDevice();
        if (device && g_resolveColor && g_msaaColor) {
            device->SetDepthStencilSurface(nullptr);
            HRESULT targetResult = device->SetRenderTarget(0, g_resolveColor);
            HRESULT depthResult = device->SetDepthStencilSurface(g_resolveDepth);
            if (SUCCEEDED(targetResult) && SUCCEEDED(depthResult)) {
                const D3DTEXTUREFILTERTYPE filter =
                    g_activeFactor > 1 ? D3DTEXF_LINEAR : D3DTEXF_NONE;
                IDirect3DSurface9* stageSource = g_msaaColor;
                HRESULT resolveResult = S_OK;
                for (int i = 0; i < g_downsampleCount && SUCCEEDED(resolveResult);
                     ++i) {
                    resolveResult = device->StretchRect(
                        stageSource, nullptr, g_downsampleChain[i], nullptr,
                        D3DTEXF_LINEAR);
                    stageSource = g_downsampleChain[i];
                }
                if (SUCCEEDED(resolveResult)) {
                    resolveResult = device->StretchRect(
                        stageSource, nullptr, g_resolveColor, nullptr, filter);
                }
                if (g_dumpPending) {
                    g_dumpPending = FALSE;
                    wchar_t tag[32] = {};
                    swprintf_s(tag, L"on-%dx", g_activeFactor);
                    DumpRenderTarget(device, tag);
                }
                if (!g_reportedResolve) {
                    g_reportedResolve = true;
                    Log("first resolve: StretchRect 0x%08X",
                        static_cast<unsigned>(resolveResult));
                }
            } else {
                Log("could not rebind preview surfaces: target 0x%08X depth 0x%08X",
                    static_cast<unsigned>(targetResult),
                    static_cast<unsigned>(depthResult));
            }
            if (!g_reportedEndStates) {
                g_reportedEndStates = true;
                DWORD zEnable = 0, zFunc = 0, zWrite = 0, alphaBlend = 0,
                      srcBlend = 0, destBlend = 0, cull = 0;
                device->GetRenderState(D3DRS_ZENABLE, &zEnable);
                device->GetRenderState(D3DRS_ZFUNC, &zFunc);
                device->GetRenderState(D3DRS_ZWRITEENABLE, &zWrite);
                device->GetRenderState(D3DRS_ALPHABLENDENABLE, &alphaBlend);
                device->GetRenderState(D3DRS_SRCBLEND, &srcBlend);
                device->GetRenderState(D3DRS_DESTBLEND, &destBlend);
                device->GetRenderState(D3DRS_CULLMODE, &cull);
                Log("states after preview: zEnable=%lu zFunc=%lu zWrite=%lu "
                    "alphaBlend=%lu srcBlend=%lu destBlend=%lu cull=%lu",
                    zEnable, zFunc, zWrite, alphaBlend, srcBlend, destBlend, cull);
            }
        }
        ReleaseFrameSurfaces();
    }

    return reinterpret_cast<CameraUpdateFn>(
        *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(camera) +
                                  kCameraEndUpdateOffset))(camera);
}

// The client hardcodes a 256x256 preview raster, which is the real limit when a
// preview textdraw is displayed larger than that. Enlarging the raster keeps the
// camera, the view window and the projection untouched; only the pixel grid the
// model is rasterized on gets denser.
void* __cdecl RwRasterCreateHook(int width, int height, int depth, int flags) {
    if (g_rasterScale > 1 && width == kPreviewRasterSize &&
        height == kPreviewRasterSize &&
        width * g_rasterScale <= kMaxPreviewRasterSize) {
        const int type = flags & kRasterTypeMask;
        if ((type == kRasterTypeCameraTexture || type == kRasterTypeZBuffer) &&
            IsMultiplayerCaller(_ReturnAddress())) {
            width *= g_rasterScale;
            height *= g_rasterScale;
            Log("preview raster type %d scaled to %dx%d", type, width, height);
        }
    }

    return g_rasterCreateTrampoline(width, height, depth, flags);
}

void* __cdecl RwCameraClearHook(void* camera, const RwRGBA* color, int flags) {
    if (color && IsMultiplayerCaller(_ReturnAddress())) {
        g_clearedCamera = camera;
        g_clearColor = *color;
        g_clearFlags = flags;
    }

    void* engine = *reinterpret_cast<void**>(kRwEngineInstance);
    auto clear = *reinterpret_cast<DeviceCameraClearFn*>(
        reinterpret_cast<uint8_t*>(engine) + kEngineCameraClearOffset);
    return clear(camera, color, flags) ? camera : nullptr;
}

bool InstallDetour(Detour& detour, const void* target) {
    if (!SafeCopy(detour.address, detour.original, sizeof(detour.original)))
        return false;

    const intptr_t distance = reinterpret_cast<uintptr_t>(target) -
                              (detour.address + sizeof(detour.original));
    if (distance < INT32_MIN || distance > INT32_MAX)
        return false;

    uint8_t jump[5] = {0xE9};
    const auto displacement = static_cast<int32_t>(distance);
    memcpy(&jump[1], &displacement, sizeof(displacement));
    if (!WriteMemory(reinterpret_cast<void*>(detour.address), jump, sizeof(jump)))
        return false;

    detour.installed = true;
    return true;
}

void RemoveDetour(Detour& detour) {
    if (!detour.installed)
        return;
    WriteMemory(reinterpret_cast<void*>(detour.address), detour.original,
                sizeof(detour.original));
    detour.installed = false;
}

// RwRasterCreate does not start with a tail jump, so its first instruction is
// relocated into a small stub that continues into the untouched remainder.
bool InstallRasterCreateDetour() {
    auto* stub = static_cast<uint8_t*>(VirtualAlloc(
        nullptr, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!stub)
        return false;

    memcpy(stub, kRasterCreateBytes, sizeof(kRasterCreateBytes));
    const uintptr_t resume = kRwRasterCreate + sizeof(kRasterCreateBytes);
    const intptr_t distance =
        resume - (reinterpret_cast<uintptr_t>(stub) + sizeof(kRasterCreateBytes) + 5);
    if (distance < INT32_MIN || distance > INT32_MAX) {
        VirtualFree(stub, 0, MEM_RELEASE);
        return false;
    }

    stub[sizeof(kRasterCreateBytes)] = 0xE9;
    const auto displacement = static_cast<int32_t>(distance);
    memcpy(stub + sizeof(kRasterCreateBytes) + 1, &displacement,
           sizeof(displacement));
    FlushInstructionCache(GetCurrentProcess(), stub, 16);

    if (!InstallDetour(g_rasterCreateDetour,
                       reinterpret_cast<const void*>(&RwRasterCreateHook))) {
        VirtualFree(stub, 0, MEM_RELEASE);
        return false;
    }

    g_rasterCreateTrampoline = reinterpret_cast<RasterCreateFn>(stub);
    return true;
}

bool InstallHooks() {
    if (reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)) != kImageBase ||
        !IsExecutableAddress(kRwCameraBeginUpdate) ||
        !IsExecutableAddress(kRwCameraEndUpdate) ||
        !IsExecutableAddress(kRwCameraClear) ||
        !IsExecutableAddress(kRwRasterCreate) ||
        !IsExecutableAddress(kRwD3D9GetCurrentDevice)) {
        Log("install aborted: unexpected executable layout");
        return false;
    }

    if (!MatchesBytes(kRwCameraBeginUpdate, kBeginUpdateBytes,
                      sizeof(kBeginUpdateBytes)) ||
        !MatchesBytes(kRwCameraEndUpdate, kEndUpdateBytes,
                      sizeof(kEndUpdateBytes)) ||
        !MatchesBytes(kRwCameraClear, kCameraClearBytes,
                      sizeof(kCameraClearBytes))) {
        Log("install aborted: RenderWare camera entry points do not match the "
            "expected US 1.0 bytes, another modification may have hooked them");
        return false;
    }

    // The clear and end hooks are inert without the begin hook, so the begin
    // hook is installed last.
    if (!InstallDetour(g_cameraClearDetour,
                       reinterpret_cast<const void*>(&RwCameraClearHook)) ||
        !InstallDetour(g_endUpdateDetour,
                       reinterpret_cast<const void*>(&RwCameraEndUpdateHook)) ||
        !InstallDetour(g_beginUpdateDetour,
                       reinterpret_cast<const void*>(&RwCameraBeginUpdateHook))) {
        Log("install aborted: could not write the camera detours");
        RemoveDetour(g_beginUpdateDetour);
        RemoveDetour(g_endUpdateDetour);
        RemoveDetour(g_cameraClearDetour);
        return false;
    }

    // A failed raster detour only costs preview resolution, so it does not undo
    // the multisampling hooks.
    if (g_rasterScale > 1) {
        if (!MatchesBytes(kRwRasterCreate, kRasterCreateBytes,
                          sizeof(kRasterCreateBytes)) ||
            !InstallRasterCreateDetour()) {
            g_rasterScale = 1;
            Log("previewScale disabled: RwRasterCreate could not be hooked");
        }
    }

    Log("hooks installed");

    g_installed = true;
    return true;
}

void RemoveHooks() {
    RemoveDetour(g_beginUpdateDetour);
    RemoveDetour(g_endUpdateDetour);
    RemoveDetour(g_cameraClearDetour);
    RemoveDetour(g_rasterCreateDetour);
    g_installed = false;
}

DWORD WINAPI Initialize(void* parameter) {
    LoadConfiguration(static_cast<HMODULE>(parameter));
    Sleep(1000);
    if (!InstallHooks())
        return 0;

    if (g_hotkeyEnabled && g_hotkeyKey != 0)
        HotkeyThread(nullptr);
    return 0;
}

} // namespace

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void*) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
        if (HANDLE thread = CreateThread(nullptr, 0, Initialize, instance, 0, nullptr))
            CloseHandle(thread);
    } else if (reason == DLL_PROCESS_DETACH) {
        g_shuttingDown = TRUE;
        if (g_installed)
            RemoveHooks();
    }
    return TRUE;
}
