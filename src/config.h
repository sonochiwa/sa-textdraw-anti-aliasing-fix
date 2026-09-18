#pragma once

#include <windows.h>

struct Settings {
    // 1, 2, 4 or 8. The client's 256x256 preview raster is created at this
    // multiple of its size.
    int previewScale = 2;
    // 1, 2, 4 or 8. Each preview pass renders at this multiple of the raster
    // size and is reduced back by halving.
    int supersample = 4;
};

// Creates TextDrawAntiAliasingFix.ini next to the plugin from the embedded
// canonical file when it is missing, then reads it.
Settings LoadSettings(HMODULE module);
