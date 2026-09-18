#pragma once

#include <cstddef>
#include <cstdint>

// Reads that fail on an unmapped page return false instead of faulting.
bool SafeCopy(uintptr_t address, void* result, size_t size);
bool IsExecutableAddress(uintptr_t address);
bool WriteMemory(void* destination, const void* source, size_t size);
bool MatchesBytes(uintptr_t address, const uint8_t* expected, size_t size);

// A five-byte JMP written over the start of a function, with the bytes it
// replaced so it can be undone.
struct Detour {
    uintptr_t address;
    uint8_t original[5];
    bool installed;
};

bool InstallDetour(Detour& detour, const void* target);
void RemoveDetour(Detour& detour);

// Copies `size` bytes of the function at `address` into an executable stub
// followed by a JMP back to the byte after them. Returns nullptr on failure.
// The copied bytes must be whole, position-independent instructions.
void* CreateTrampoline(uintptr_t address, const uint8_t* bytes, size_t size);
