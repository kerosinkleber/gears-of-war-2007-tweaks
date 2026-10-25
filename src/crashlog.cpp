// Crash logger: on an access violation, writes the faulting address and the likely return
// addresses found on the stack (values pointing into the exe's or this DLL's code) to
// rowe.log. It does not handle the exception; the game crashes as it would anyway.
#include <windows.h>
#include "rowe.h"

static BYTE* g_selfBase;
static DWORD g_selfSize;

// The exe's code section (fixed image base 0x00400000) and this DLL's whole image.
static bool InExeCode(DWORD v) { return v >= 0x00401000 && v < 0x01582000; }
static bool InSelf(DWORD v) { return v >= (DWORD)g_selfBase && v < (DWORD)g_selfBase + g_selfSize; }

static LONG CALLBACK CrashHandler(EXCEPTION_POINTERS* ep)
{
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
    // Only faults in the game's or our own code; IsBadReadPtr probes fault inside kernel32.
    DWORD ip = ep->ContextRecord->Eip;
    if (!InExeCode(ip) && !InSelf(ip)) return EXCEPTION_CONTINUE_SEARCH;
    static LONG once;
    if (InterlockedExchange(&once, 1)) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT* c = ep->ContextRecord;
    Log("CRASH: access violation at %08lx (%s address %08lx) eax=%08lx ecx=%08lx edx=%08lx esi=%08lx edi=%08lx esp=%08lx ebp=%08lx",
        c->Eip, ep->ExceptionRecord->ExceptionInformation[0] ? "writing" : "reading",
        (DWORD)ep->ExceptionRecord->ExceptionInformation[1], c->Eax, c->Ecx, c->Edx, c->Esi, c->Edi, c->Esp, c->Ebp);
    DWORD* sp = (DWORD*)c->Esp;
    int found = 0;
    for (int i = 0; i < 2048 && found < 40; ++i) {
        if (IsBadReadPtr(sp + i, 4)) break;
        DWORD v = sp[i];
        if (InExeCode(v)) { Log("  stack+%04x: %08lx (exe)", i * 4, v); ++found; }
        else if (InSelf(v)) { Log("  stack+%04x: %08lx (rowe+%lx)", i * 4, v, v - (DWORD)g_selfBase); ++found; }
    }
    ClipCursor(nullptr);  // never leave the cursor confined to a game that is going down
    return EXCEPTION_CONTINUE_SEARCH;
}

void CrashLogInstall(HMODULE self)
{
    g_selfBase = (BYTE*)self;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(g_selfBase + ((IMAGE_DOS_HEADER*)g_selfBase)->e_lfanew);
    g_selfSize = nt->OptionalHeader.SizeOfImage;
    AddVectoredExceptionHandler(1, CrashHandler);
}
