#include "patch.h"

#include <windows.h>

#include <cstring>

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
    if (!address ||
        VirtualQuery(reinterpret_cast<const void*>(address), &info, sizeof(info)) != sizeof(info)) {
        return false;
    }

    constexpr DWORD executablePages =
        PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    return info.State == MEM_COMMIT && (info.Protect & executablePages) != 0;
}

bool WriteMemory(void* destination, const void* source, size_t size) {
    DWORD oldProtect = 0;
    if (!VirtualProtect(destination, size, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        return false;
    }

    memcpy(destination, source, size);
    FlushInstructionCache(GetCurrentProcess(), destination, size);

    DWORD unused = 0;
    VirtualProtect(destination, size, oldProtect, &unused);
    return true;
}

bool MatchesBytes(uintptr_t address, const uint8_t* expected, size_t size) {
    uint8_t actual[32] = {};
    if (size > sizeof(actual) || !SafeCopy(address, actual, size)) {
        return false;
    }
    return memcmp(actual, expected, size) == 0;
}

bool InstallDetour(Detour& detour, const void* target) {
    if (!SafeCopy(detour.address, detour.original, sizeof(detour.original))) {
        return false;
    }

    const intptr_t distance =
        reinterpret_cast<uintptr_t>(target) - (detour.address + sizeof(detour.original));
    if (distance < INT32_MIN || distance > INT32_MAX) {
        return false;
    }

    uint8_t jump[5] = {0xE9};
    const auto displacement = static_cast<int32_t>(distance);
    memcpy(&jump[1], &displacement, sizeof(displacement));
    if (!WriteMemory(reinterpret_cast<void*>(detour.address), jump, sizeof(jump))) {
        return false;
    }

    detour.installed = true;
    return true;
}

void RemoveDetour(Detour& detour) {
    if (!detour.installed) {
        return;
    }
    WriteMemory(reinterpret_cast<void*>(detour.address), detour.original, sizeof(detour.original));
    detour.installed = false;
}

void* CreateTrampoline(uintptr_t address, const uint8_t* bytes, size_t size) {
    constexpr size_t kStubSize = 16;
    if (size + 5 > kStubSize) {
        return nullptr;
    }

    auto* stub = static_cast<uint8_t*>(
        VirtualAlloc(nullptr, kStubSize, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!stub) {
        return nullptr;
    }

    memcpy(stub, bytes, size);
    const uintptr_t resume = address + size;
    const intptr_t distance = resume - (reinterpret_cast<uintptr_t>(stub) + size + 5);
    if (distance < INT32_MIN || distance > INT32_MAX) {
        VirtualFree(stub, 0, MEM_RELEASE);
        return nullptr;
    }

    stub[size] = 0xE9;
    const auto displacement = static_cast<int32_t>(distance);
    memcpy(stub + size + 1, &displacement, sizeof(displacement));
    FlushInstructionCache(GetCurrentProcess(), stub, kStubSize);
    return stub;
}
