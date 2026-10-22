// Shoulder swap: Tab mirrors the third-person camera to the other shoulder. The shoulder the
// camera starts on is a setting (gowfov.ini: shoulder=left|right).
//
// UWarCameraMode::GetViewOffset (exe VA 0x00EFAFD0) returns the camera offset relative to the
// pawn in camera space (X forward, Y right, Z up). Negating Y moves the camera to the left
// shoulder; the engine's own camera collision still runs on the result afterwards.
#include <windows.h>
#include "gowfov.h"

struct FVector { float X, Y, Z; };

typedef FVector* (__thiscall* PFN_GetViewOffset)(void* self, FVector* ret, void* pawn, float dt, void* viewRot);

static PFN_GetViewOffset g_orig;
static const BYTE kExpected[8] = {0x83, 0xEC, 0x30, 0x53, 0x8B, 0x5C, 0x24, 0x3C};  // sub esp,30h; push ebx; mov ebx,[esp+3Ch]
static const int kToggleKey = VK_TAB;
static const float kSwapSeconds = 0.25f;

static volatile bool g_left;   // target shoulder
static bool g_startLeft;       // setting: shoulder at game start
static float g_side = 1.f;     // current blend: 1 = right shoulder, -1 = left
static LARGE_INTEGER g_lastTime, g_freq;

static FVector* __fastcall HookGetViewOffset(void* self, void*, FVector* ret, void* pawn, float dt, void* viewRot)
{
    FVector* r = g_orig(self, ret, pawn, dt, viewRot);
    FovTraceAdd('m', r->X, r->Y, r->Z, dt, self);  // self = the active camera mode object

    // Advance the slide with wall-clock time; the hook can run more than once per frame.
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    float elapsed = g_lastTime.QuadPart ? (float)(now.QuadPart - g_lastTime.QuadPart) / (float)g_freq.QuadPart : 0.f;
    g_lastTime = now;
    if (elapsed > 0.1f) elapsed = 0.1f;
    float target = g_left ? -1.f : 1.f;
    float step = 2.f * elapsed / kSwapSeconds;
    if (g_side < target) g_side = g_side + step > target ? target : g_side + step;
    else if (g_side > target) g_side = g_side - step < target ? target : g_side - step;

    // Smoothstep the blend so the camera eases in and out of the move.
    float t = (g_side + 1.f) * 0.5f;
    float eased = t * t * (3.f - 2.f * t) * 2.f - 1.f;
    r->Y *= eased;
    return r;
}

// Reads the start shoulder from gowfov.ini and detours GetViewOffset.
void ShoulderInstall()
{
    char path[MAX_PATH], val[16];
    wsprintfA(path, "%sgowfov.ini", ModDir());
    GetPrivateProfileStringA("gowfov", "shoulder", "right", val, sizeof(val), path);
    g_startLeft = lstrcmpiA(val, "left") == 0;
    g_left = g_startLeft;
    g_side = g_left ? -1.f : 1.f;
    QueryPerformanceFrequency(&g_freq);
    g_orig = (PFN_GetViewOffset)InstallDetour(0x00EFAFD0, kExpected, sizeof(kExpected),
        (void*)HookGetViewOffset, "shoulder");
}

bool ShoulderStartLeft() { return g_startLeft; }

void ShoulderSetStartLeft(bool left)
{
    g_startLeft = left;
    g_left = left;
    char path[MAX_PATH];
    wsprintfA(path, "%sgowfov.ini", ModDir());
    WritePrivateProfileStringA("gowfov", "shoulder", left ? "left" : "right", path);
}

// Polled from Present; `focused` is true when the game window has keyboard focus.
void ShoulderUpdate(bool focused)
{
    static bool prev;
    bool down = focused && (GetAsyncKeyState(kToggleKey) & 0x8000) != 0;
    if (down && !prev) {
        g_left = !g_left;
        Log("shoulder: %s", g_left ? "left" : "right");
    }
    prev = down;
}
