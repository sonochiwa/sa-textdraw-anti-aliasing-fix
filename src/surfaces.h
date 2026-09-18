#pragma once

#include <d3d9.h>

// The plain surfaces a preview pass renders into, at `factor` times the size
// of the client's preview raster, and the 2:1 chain that reduces them back.
void SetSupersampleFactor(int factor);

// Captures the surfaces the client bound, switches the device to the large
// pair and clears it the way the client cleared its own raster. Returns false
// and leaves the device untouched when anything fails.
bool BeginSupersampledPass(IDirect3DDevice9* device, const void* camera);

// Restores the client's surfaces and reduces the large image into them.
void EndSupersampledPass(IDirect3DDevice9* device);

bool IsPassActive(const void* camera);

// Remembers the clear the client asked for on `camera`, replayed by
// BeginSupersampledPass.
struct RwRGBA;
void RecordCameraClear(const void* camera, const RwRGBA& color, int flags);

// Replaces IDirect3DDevice9::Reset in the device's vtable so the plugin's
// D3DPOOL_DEFAULT surfaces are released before a reset. Returns false for a
// device of another class.
bool EnsureDeviceResetHook(IDirect3DDevice9* device);
void RemoveDeviceResetHook();
