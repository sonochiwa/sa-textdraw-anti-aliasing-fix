#include "samp.h"

#include <windows.h>

#include <cstdint>

namespace {

HMODULE g_sampModule = nullptr;
uintptr_t g_sampStart = 0;
uintptr_t g_sampEnd = 0;

} // namespace

bool IsMultiplayerCaller(const void* returnAddress) {
    if (!g_sampModule) {
        g_sampModule = GetModuleHandleW(L"samp.dll");
        if (!g_sampModule) {
            return false;
        }

        const auto base = reinterpret_cast<uintptr_t>(g_sampModule);
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(g_sampModule);
        const auto* nt =
            reinterpret_cast<const IMAGE_NT_HEADERS*>(base + static_cast<uintptr_t>(dos->e_lfanew));
        g_sampStart = base;
        g_sampEnd = base + nt->OptionalHeader.SizeOfImage;
    }

    const auto address = reinterpret_cast<uintptr_t>(returnAddress);
    return address >= g_sampStart && address < g_sampEnd;
}
