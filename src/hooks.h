#pragma once

// Detours RwCameraBeginUpdate, RwCameraEndUpdate and RwCameraClear so a
// preview pass entered from samp.dll renders supersampled, and RwRasterCreate
// so the client's 256x256 preview raster is created `previewScale` times
// larger. Returns false when the executable does not match; a failed raster
// detour only costs preview resolution and does not undo the rest.
bool InstallHooks(int previewScale);
void RemoveHooks();
