#include "config.h"

#include "resource.h"

#include <cwchar>

namespace {

bool BuildIniPath(HMODULE module, wchar_t (&path)[MAX_PATH]) {
    const DWORD length = GetModuleFileNameW(module, path, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        return false;
    }

    wchar_t* slash = wcsrchr(path, L'\\');
    if (!slash) {
        return false;
    }

    const size_t room = static_cast<size_t>(MAX_PATH - (slash + 1 - path));
    return wcscpy_s(slash + 1, room, L"TextDrawAntiAliasingFix.ini") == 0;
}

// Writes the RCDATA copy of Config\TextDrawAntiAliasingFix.ini byte for byte.
void CreateDefaultIni(HMODULE module, const wchar_t* path) {
    if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES) {
        return;
    }

    const HRSRC resource = FindResourceW(module, MAKEINTRESOURCEW(IDR_DEFAULT_INI), RT_RCDATA);
    if (!resource) {
        return;
    }
    const HGLOBAL handle = LoadResource(module, resource);
    const DWORD size = SizeofResource(module, resource);
    const void* data = handle ? LockResource(handle) : nullptr;
    if (!data || size == 0) {
        return;
    }

    const HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }
    DWORD written = 0;
    WriteFile(file, data, size, &written, nullptr);
    CloseHandle(file);
}

int ClampFactor(int value) {
    if (value >= 8) {
        return 8;
    }
    if (value >= 4) {
        return 4;
    }
    if (value >= 2) {
        return 2;
    }
    return 1;
}

} // namespace

Settings LoadSettings(HMODULE module) {
    Settings settings;
    wchar_t path[MAX_PATH] = {};
    if (!BuildIniPath(module, path)) {
        return settings;
    }
    CreateDefaultIni(module, path);

    settings.previewScale =
        ClampFactor(GetPrivateProfileIntW(L"antiAliasing", L"previewScale", settings.previewScale, path));
    settings.supersample =
        ClampFactor(GetPrivateProfileIntW(L"antiAliasing", L"supersample", settings.supersample, path));
    return settings;
}
