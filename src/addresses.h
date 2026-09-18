#pragma once

#include <windows.h>
#include <d3d9.h>

#include <cstddef>
#include <cstdint>

// GTA San Andreas 1.0 US, default image base.
constexpr uintptr_t kImageBase = 0x00400000;

// RwCameraEndUpdate and RwCameraBeginUpdate thunks: `mov eax, [esp+4]` then a
// jump through the camera's dispatch pointer.
constexpr uintptr_t kRwCameraEndUpdate = 0x007EE180;
constexpr uintptr_t kRwCameraBeginUpdate = 0x007EE190;
// RwCameraClear: reads the engine instance and calls its device clear entry.
constexpr uintptr_t kRwCameraClear = 0x007EE340;
// RwD3D9GetCurrentD3DDevice.
constexpr uintptr_t kRwD3D9GetCurrentDevice = 0x007F9D50;
// RwRasterCreate.
constexpr uintptr_t kRwRasterCreate = 0x007FB230;
// RwEngineInstance pointer.
constexpr uintptr_t kRwEngineInstance = 0x00C97B24;
// Scene camera pointer; the SA-MP preview cameras are never this one.
constexpr uintptr_t kSceneCamera = 0x00C17038;

// RwCamera dispatch pointers used by the two thunks above.
constexpr size_t kCameraBeginUpdateOffset = 0x18;
constexpr size_t kCameraEndUpdateOffset = 0x1C;
// Device camera-clear entry inside the RenderWare engine instance.
constexpr size_t kEngineCameraClearOffset = 0x9C;
// IDirect3DDevice9::Reset is the seventeenth entry of the device vtable.
constexpr size_t kDeviceResetSlot = 16;

// The client asks for these rasters when it builds its model preview camera.
constexpr int kPreviewRasterSize = 256;
constexpr int kRasterTypeMask = 0x07;
constexpr int kRasterTypeZBuffer = 0x01;
constexpr int kRasterTypeCameraTexture = 0x05;
constexpr int kMaxPreviewRasterSize = 2048;

// RwCameraClear flags.
constexpr int kClearImage = 0x01;
constexpr int kClearDepth = 0x02;
constexpr int kClearStencil = 0x04;

// mov eax, [esp+4]; mov [esp+4], eax; jmp [eax+18h]
constexpr uint8_t kBeginUpdateBytes[] = {0x8B, 0x44, 0x24, 0x04, 0x89, 0x44, 0x24, 0x04, 0xFF, 0x60, 0x18};
// mov eax, [esp+4]; mov [esp+4], eax; jmp [eax+1Ch]
constexpr uint8_t kEndUpdateBytes[] = {0x8B, 0x44, 0x24, 0x04, 0x89, 0x44, 0x24, 0x04, 0xFF, 0x60, 0x1C};
// mov ecx, [esp+0Ch]; mov eax, [RwEngineInstance]; mov edx, [esp+8]; push esi; mov esi, [esp+8]
constexpr uint8_t kCameraClearBytes[] = {0x8B, 0x4C, 0x24, 0x0C, 0xA1, 0x24, 0x7B, 0xC9, 0x00,
                                         0x8B, 0x54, 0x24, 0x08, 0x56, 0x8B, 0x74, 0x24, 0x08};
// mov eax, [RwEngineInstance]. Exactly five position-independent bytes, so
// the instruction can be relocated into a trampoline verbatim.
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
using DeviceResetFn = HRESULT(__stdcall*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
