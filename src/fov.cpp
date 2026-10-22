// FOV state, persistence, and the engine hook.
//
// AWarPlayerCamera::AdjustFOVForViewport (exe VA 0x00EFB860, __stdcall(float fov, AActor* pawn),
// result in st0) is the last step before the camera FOV reaches the view setup; GetWarCamFOV
// funnels through it. We detour it and rewrite the returned horizontal FOV.
#include <windows.h>
#include <string.h>
#include "gowfov.h"

static volatile float g_fov;  // slider value; 0 = no override

static const DWORD kTargetVA = 0x00EFB860;
static const BYTE kExpected[7] = {0x83, 0xEC, 0x10, 0x8B, 0x44, 0x24, 0x18};  // sub esp,10h; mov eax,[esp+18h]
static const float kBaseFov = 80.f;   // game default for the normal camera
static const float kMaxResult = 179.f;

typedef float (__stdcall* AdjustFn)(float fov, void* pawn);
static AdjustFn g_orig;

static float SmoothStep(float t)
{
    if (t <= 0) return 0;
    if (t >= 1) return 1;
    return t * t * (3 - 2 * t);
}

// Wide camera modes (>=80) take the full offset; zoomed aiming (<=50) is left alone,
// with a smooth blend for modes in between. The engine blends from one mode's result to the
// next after this hook (lens.cpp follows the blended FOV).
static float Remap(float fov)
{
    float s = g_fov;
    if (s <= 0) return fov;
    float out = fov + SmoothStep((fov - 50.f) / 30.f) * (s - kBaseFov);
    if (out > kMaxResult) out = kMaxResult;
    if (out < 10.f) out = 10.f;
    return out;
}

// The camera mode's FOV -> our FOV (Remap) -> the wider FOV the engine must render so the lens
// warp has every pixel it needs (LensRenderFov). The engine blends between camera modes after
// this returns, so only targets pass through here.
static float __stdcall HookAdjust(float fov, void* pawn)
{
    static int logged;
    float r = g_orig(fov, pawn);  // non-null: the hook only exists if the detour installed
    float lensFov = Remap(r);
    float out = LensRenderFov(lensFov);
    FovTraceAdd('c', fov, r, lensFov, out, pawn);
    if (logged < 12 && out != r) {
        ++logged;
        Log("AdjustFOV in=%.2f orig=%.2f out=%.2f slider=%.0f", fov, r, out, g_fov);
    }
    return out;
}

static void IniPath(char* out)
{
    wsprintfA(out, "%sgowfov.ini", ModDir());
}

// Reads fov= from gowfov.ini (also used to reload it at runtime).
void FovInit()
{
    char path[MAX_PATH];
    IniPath(path);
    g_fov = (float)GetPrivateProfileIntA("gowfov", "fov", 0, path);
    Log("FovInit fov=%d", (int)g_fov);
}

void FovInstall()
{
    g_orig = (AdjustFn)InstallDetour(kTargetVA, kExpected, sizeof(kExpected), (void*)HookAdjust, "fov");
}

float FovGet() { return g_fov; }

void FovSet(float degrees)
{
    g_fov = degrees;
    char path[MAX_PATH], val[16];
    IniPath(path);
    wsprintfA(val, "%d", (int)(degrees + 0.5f));
    WritePrivateProfileStringA("gowfov", "fov", val, path);
}
