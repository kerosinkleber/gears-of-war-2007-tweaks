// Minimal x86 detour: copies the first `len` bytes (whole, position-independent instructions)
// of `target` into a trampoline and replaces them with a jump to `hook`.
#include <windows.h>
#include <string.h>
#include "rowe.h"

// Replaces one function in the exe's import table; returns the previous target or null.
void* PatchExeImport(const char* dll, const char* name, void* hook)
{
    BYTE* base = (BYTE*)GetModuleHandleA(nullptr);
    void* target = (void*)GetProcAddress(GetModuleHandleA(dll), name);
    if (!target) return nullptr;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    for (IMAGE_IMPORT_DESCRIPTOR* d = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); d->Name; ++d) {
        if (lstrcmpiA((char*)(base + d->Name), dll) != 0) continue;
        for (void** slot = (void**)(base + d->FirstThunk); *slot; ++slot) {
            if (*slot != target) continue;
            DWORD old;
            VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old);
            *slot = hook;
            VirtualProtect(slot, sizeof(void*), old, &old);
            return target;
        }
    }
    return nullptr;
}

// Detours the exe function at targetVA. `expected` holds its first `len` bytes; nothing is
// patched unless they match, so a different exe build fails safe. The trampoline runs the copied
// bytes and jumps back to targetVA + len, i.e. calling it runs the original function.
void* InstallDetour(DWORD targetVA, const BYTE* expected, int len, void* hook, const char* name)
{
    if ((DWORD)GetModuleHandleA(nullptr) != 0x00400000) {
        Log("%s: skipped, unexpected image base %p", name, GetModuleHandleA(nullptr));
        return nullptr;
    }
    BYTE* target = (BYTE*)targetVA;
    if (len < 5 || IsBadReadPtr(target, len) || memcmp(target, expected, len) != 0) {
        Log("%s: skipped, bytes at %08lx do not match (different exe build?)", name, targetVA);
        return nullptr;
    }
    // Trampoline: the original first instructions, then `jmp target + len`.
    BYTE* tramp = (BYTE*)VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tramp) return nullptr;
    memcpy(tramp, target, len);
    tramp[len] = 0xE9;
    *(int*)(tramp + len + 1) = (int)((target + len) - (tramp + len + 5));

    // Target: `jmp hook`, the rest of the copied bytes padded with NOPs.
    DWORD old;
    VirtualProtect(target, len, PAGE_EXECUTE_READWRITE, &old);
    target[0] = 0xE9;
    *(int*)(target + 1) = (int)((BYTE*)hook - (target + 5));
    for (int i = 5; i < len; ++i) target[i] = 0x90;
    VirtualProtect(target, len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), target, len);
    Log("%s: hook installed at %08lx", name, targetVA);
    return tramp;
}

// Redirects one `call rel32` instruction; returns the original call target or null.
void* PatchCall(DWORD site, DWORD expectedTarget, void* hook, const char* name)
{
    BYTE* p = (BYTE*)site;
    if (IsBadReadPtr(p, 5) || p[0] != 0xE8 || site + 5 + *(int*)(p + 1) != expectedTarget) {
        Log("%s: call site %08lx mismatch, not hooked", name, site);
        return nullptr;
    }
    DWORD old;
    VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old);
    *(int*)(p + 1) = (int)((BYTE*)hook - (p + 5));
    VirtualProtect(p, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 5);
    return (void*)expectedTarget;
}
