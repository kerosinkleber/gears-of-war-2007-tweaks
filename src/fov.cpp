// FOV state, persistence, and the engine hook.
//
// AWarPlayerCamera::AdjustFOVForViewport (exe VA 0x00EFB860, __stdcall(float fov, AActor* pawn),
// result in st0) is the last step before the camera FOV reaches the view setup; GetWarCamFOV
// funnels through it. We detour it and rewrite the returned horizontal FOV.
#include <windows.h>
#include <string.h>
#include <math.h>
#include "rowe.h"

static volatile float g_fov;     // slider value; 0 = no override
static volatile float g_aimFov;  // aiming FOV; 0 = the game's (kAimFov)

static const DWORD kTargetVA = 0x00EFB860;
static const BYTE kExpected[7] = {0x83, 0xEC, 0x10, 0x8B, 0x44, 0x24, 0x18};  // sub esp,10h; mov eax,[esp+18h]
static const float kBaseFov = 80.f;   // game default for the normal camera
static const float kAimFov = 40.f;    // game default while aiming
static const float kScopeMax = 25.f;  // at or below: scope zoom, never changed
static const float kPi = 3.14159265f;

typedef float (__stdcall* AdjustFn)(float fov, void* pawn);
static AdjustFn g_orig;
static volatile float g_lastCappedFov;  // the last camera mode FOV that Remap took to its cap

static float SmoothStep(float t)
{
    if (t <= 0) return 0;
    if (t >= 1) return 1;
    return t * t * (3 - 2 * t);
}

static float LogTan(float deg) { return logf(tanf(deg * kPi / 360.f)); }
static float FromLogTan(float v) { return atanf(expf(v)) * 360.f / kPi; }

// Wide camera modes (>=80) take the full FOV offset, with a smooth blend for modes between 50 and
// 80; zoomed aiming (<=50) is left alone. With an aim FOV set, everything between the scope range
// and the normal camera instead goes through three anchors, interpolated in log tan (even zoom
// ratios, monotonic): kScopeMax stays, kAimFov becomes the aim FOV, kBaseFov becomes the normal
// FOV. The engine blends from one mode's result to the next after this hook (lens.cpp follows
// the blended FOV).
static float Remap(float fov)
{
    float s = g_fov, aim = g_aimFov;
    float out;
    if (aim > 0 && fov > kScopeMax && fov < kBaseFov) {
        float normal = s > 0 ? s : kBaseFov;
        if (aim > normal) aim = normal;  // aiming never zooms out past the normal camera
        bool low = fov <= kAimFov;
        float a0 = low ? kScopeMax : kAimFov, a1 = low ? kAimFov : kBaseFov;
        float b0 = low ? kScopeMax : aim, b1 = low ? aim : normal;
        float t = (LogTan(fov) - LogTan(a0)) / (LogTan(a1) - LogTan(a0));
        out = FromLogTan(LogTan(b0) + t * (LogTan(b1) - LogTan(b0)));
    } else {
        if (s <= 0) return fov;
        out = fov + SmoothStep((fov - 50.f) / 30.f) * (s - kBaseFov);
    }
    if (out > kFovMax) out = kFovMax;
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
    if (lensFov >= kFovMax - 0.01f) g_lastCappedFov = r;
    float out = LensRenderFov(lensFov);
    FovTraceAdd('c', fov, r, lensFov, out, pawn);
    if (logged < 12 && out != r) {
        ++logged;
        Log("AdjustFOV in=%.2f orig=%.2f out=%.2f slider=%.0f", fov, r, out, g_fov);
    }
    return out;
}

// The comparison's left view (compare.cpp): the FOV the game itself would use in this frame, from
// the FOV the engine renders with now (after this hook and the engine's blend between camera
// modes): first the lens' wider render FOV back to the lens FOV, then Remap undone. Remap never
// decreases, so a bisection finds the game's FOV. At Remap's cap (kFovMax) every wider camera
// mode looks the same, so there the camera mode's own FOV the hook saw last is used.
float FovGameFov(float renderFov)
{
    float target = LensFovForRender(renderFov);
    if (g_fov <= 0 && g_aimFov <= 0) return target;
    if (target >= kFovMax - 0.05f) {
        if (g_lastCappedFov > 0) return g_lastCappedFov;
        target = kFovMax;
    }
    float lo = 1.f, hi = 170.f;
    for (int i = 0; i < 40; ++i) {
        float mid = (lo + hi) / 2;
        if (Remap(mid) < target) lo = mid;
        else hi = mid;
    }
    return (lo + hi) / 2;
}

static void IniPath(char* out)
{
    wsprintfA(out, "%srowe.ini", ModDir());
}

// Reads fov= and aimfov= from rowe.ini (also used to reload them at runtime).
void FovInit()
{
    char path[MAX_PATH];
    IniPath(path);
    g_fov = (float)GetPrivateProfileIntA("rowe", "fov", 0, path);
    if (g_fov > kFovMax) g_fov = kFovMax;
    float aim = (float)GetPrivateProfileIntA("rowe", "aimfov", 0, path);
    g_aimFov = aim > 0 ? (aim < kAimFovMin ? kAimFovMin : aim > kAimFovMax ? kAimFovMax : aim) : 0.f;
    Log("FovInit fov=%d aimfov=%d", (int)g_fov, (int)g_aimFov);
}

void FovInstall()
{
    g_orig = (AdjustFn)InstallDetour(kTargetVA, kExpected, sizeof(kExpected), (void*)HookAdjust, "fov");
}

float FovGet() { return g_fov; }

void FovSet(float degrees, bool save)
{
    if (degrees > kFovMax) degrees = kFovMax;
    g_fov = degrees;
    if (!save) return;
    char path[MAX_PATH], val[16];
    IniPath(path);
    wsprintfA(val, "%d", (int)(degrees + 0.5f));
    WritePrivateProfileStringA("rowe", "fov", val, path);
}

float FovGetAim() { return g_aimFov; }

void FovSetAim(float degrees)
{
    if (degrees > 0 && degrees < kAimFovMin) degrees = kAimFovMin;
    if (degrees > kAimFovMax) degrees = kAimFovMax;
    g_aimFov = degrees;
    char path[MAX_PATH], val[16];
    IniPath(path);
    wsprintfA(val, "%d", (int)(degrees + 0.5f));
    WritePrivateProfileStringA("rowe", "aimfov", val, path);
}
