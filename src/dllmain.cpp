// ROWE for Gears of War (2007): d3d9.dll proxy for Gears of War (PC, 2007).
// The game imports d3d9.dll statically (it probes D3D9 even when rendering with D3D10),
// which makes this a reliable place to get loaded. Rendering hooks live in dx10hook.cpp.
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <stdarg.h>
#include "rowe.h"
#include "migrate.h"

typedef IDirect3D9* (WINAPI* PFN_Direct3DCreate9)(UINT);

static HMODULE g_realD3D9;
static char g_dir[MAX_PATH];

// The file is opened for every line, so everything logged before a crash is on disk.
void Log(const char* fmt, ...)
{
    char path[MAX_PATH];
    wsprintfA(path, "%srowe.log", g_dir);
    FILE* f = fopen(path, "a");
    if (!f) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(f, "%02d:%02d:%02d.%03d ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

// rowe.ini, the log and the tool request/state files all live next to the DLL.
const char* ModDir() { return g_dir; }

// Loads the system d3d9.dll that this proxy stands in for.
static void LoadReal()
{
    if (g_realD3D9) return;
    char sys[MAX_PATH];
    GetSystemDirectoryA(sys, MAX_PATH);  // SysWOW64 for a 32-bit process
    lstrcatA(sys, "\\d3d9.dll");
    g_realD3D9 = LoadLibraryA(sys);
}

// Other modules (e.g. d3dx9) import these from d3d9.dll, so they must exist here and
// forward to the real DLL. Each stub resolves its target on first call, then jumps to it.
#define FORWARD_LIST(X) \
    X(D3DPERF_BeginEvent) X(D3DPERF_EndEvent) X(D3DPERF_GetStatus) X(D3DPERF_QueryRepeatFrame) \
    X(D3DPERF_SetMarker) X(D3DPERF_SetOptions) X(D3DPERF_SetRegion) X(Direct3DCreate9Ex) \
    X(DebugSetMute) X(Direct3DShaderValidatorCreate9) X(PSGPError) X(PSGPSampleTexture) \
    X(Direct3D9EnableMaximizedWindowedModeShim)

#define DECLARE_PTR(n) static void* p_##n;
FORWARD_LIST(DECLARE_PTR)

static void ResolveForwards()
{
    LoadReal();
#define RESOLVE(n) p_##n = (void*)GetProcAddress(g_realD3D9, #n);
    FORWARD_LIST(RESOLVE)
}

#define DEFINE_STUB(n) \
    __pragma(comment(linker, "/EXPORT:" #n "=_Stub_" #n)) \
    extern "C" __declspec(naked) void Stub_##n() { \
        __asm { cmp p_##n, 0 } \
        __asm { jne ready } \
        __asm { pushad } \
        __asm { call ResolveForwards } \
        __asm { popad } \
        __asm { ready: jmp p_##n } \
    }
FORWARD_LIST(DEFINE_STUB)

// The export the game itself calls (once at startup, to probe D3D9). It is forwarded to the real
// D3D9; the first call also installs the hooks that cannot go in from DllMain: Dx10HookInstall
// creates a D3D10 device, which must not happen under the loader lock.
extern "C" IDirect3D9* WINAPI Direct3DCreate9(UINT sdk)
{
    static bool installed;
    LoadReal();
    PFN_Direct3DCreate9 real = (PFN_Direct3DCreate9)GetProcAddress(g_realD3D9, "Direct3DCreate9");
    IDirect3D9* d3d = real(sdk);
    Log("Direct3DCreate9(%u) -> %p", sdk, d3d);
    if (!installed) {
        installed = true;
        FovInstall();
        ShoulderInstall();
        LensInstall();
        Dx10HookInstall();
    }
    return d3d;
}

// Runs when the exe loads its imports, before any engine code. Reads the settings and installs
// the hooks that must be in place before the engine starts (window and input imports, the intro
// movie queue, the viewport and DXGI factory for supersampling).
BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        GetModuleFileNameA(inst, g_dir, MAX_PATH);
        char* slash = strrchr(g_dir, '\\');
        if (slash) slash[1] = 0;
        Log("---- ROWE loaded ----");
        if (MigrateGowfovFiles(g_dir)) Log("moved the files of the earlier version (gowfov) to the new names");
        CrashLogInstall(inst);
        FovInit();
        LensInit();
        SupersampleInit();
        MouseTrapInit();
        BorderlessInit();
        CompareInit();
        ScreenshotInit();
        LaunchInit();
        BackgroundInit();
        BackgroundInstall();
        TextCaptureInstall();
        IntrosInstall();
        MouseTrapInstall();
        // The viewport and DXGI factory are set up before the engine's first Direct3DCreate9
        // call, so these hooks go in now (the exe's imports are already bound here).
        SupersampleEarlyInstall();
        SupersampleInstall();
        BorderlessInstall();
        DInputInstall();
        CompareInstall();
    }
    return TRUE;
}
