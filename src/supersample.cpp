// Supersampling: the engine renders at a "virtual" resolution larger than the display and the
// mod averages it down to the native back buffer at Present.
//
// How the engine is made to render bigger (fullscreen only):
//  - FWindowsViewport::Resize (0x00E6F080) gets the native size and is handed the virtual one.
//  - The D3D10 RHI's GetSupportedResolution (0x0113C540) would snap that to a real display
//    mode; it is skipped for the virtual size.
//  - The engine's CreateDXGIFactory (delay-load IAT slot 0x01B9DF34) is redirected so that
//    IDXGIFactory::CreateSwapChain can create the real swap chain at native size.
//  - The swap chain hooks in dx10hook.cpp hand the engine a virtual-size texture from
//    GetBuffer, report the virtual size from GetDesc, and map ResizeBuffers/ResizeTarget.
//  - The game window's client size is reported as virtual through its USER32 imports.
//  - All 2D (HUD canvases and UI scenes) lays out at native size and is drawn scaled up.
#include <windows.h>
#include <d3d10.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "rowe.h"
#include "downscale_vs.h"
#include "downscale_ps.h"

static const UINT kMaxTextureSize = 8192;  // D3D10 limit per axis
// Highest pixel-count multiplier. The render size is also capped at kMaxTextureSize per axis
// (ComputeVirtual), which at 2560x1440 is reached at about 10.2.
static const float kMaxFactor = 10.f;

static float g_factor = 1.f;          // pixel-count multiplier from rowe.ini
static volatile bool g_active;        // engine viewport is currently virtual
static UINT g_nativeW, g_nativeH, g_virtW, g_virtH;
static HWND g_window;                 // game window (swap chain output window)
static IDXGISwapChain* g_swap;        // the virtualized swap chain (weak)

// ---- settings ------------------------------------------------------------------------------

// Reads supersample= (pixel-count multiplier, clamped to 1..9) from rowe.ini.
void SupersampleInit()
{
    char path[MAX_PATH], val[32];
    wsprintfA(path, "%srowe.ini", ModDir());
    GetPrivateProfileStringA("rowe", "supersample", "1", val, sizeof(val), path);
    g_factor = (float)atof(val);
    if (g_factor < 1.f) g_factor = 1.f;
    if (g_factor > kMaxFactor) g_factor = kMaxFactor;
    Log("supersample: factor %.2f", g_factor);
}

bool SupersampleGetSizes(UINT* nw, UINT* nh, UINT* vw, UINT* vh)
{
    if (!g_active) return false;
    *nw = g_nativeW; *nh = g_nativeH; *vw = g_virtW; *vh = g_virtH;
    return true;
}

// The desktop resolution, the upper bound for a real (native) viewport size.
static void DesktopSize(UINT* w, UINT* h)
{
    DEVMODEA dm = {};
    dm.dmSize = sizeof(dm);
    if (EnumDisplaySettingsA(nullptr, ENUM_REGISTRY_SETTINGS, &dm)) {
        *w = dm.dmPelsWidth;
        *h = dm.dmPelsHeight;
    } else {
        *w = GetSystemMetrics(SM_CXSCREEN);
        *h = GetSystemMetrics(SM_CYSCREEN);
    }
}

// Virtual size = native size * sqrt(factor) per axis (factor counts pixels), capped at the D3D10
// texture limit and rounded down to even numbers.
static void ComputeVirtual()
{
    float s = sqrtf(g_factor);
    float maxS = (float)kMaxTextureSize / (float)(g_nativeW > g_nativeH ? g_nativeW : g_nativeH);
    if (s > maxS) s = maxS;
    g_virtW = ((UINT)(g_nativeW * s + 0.5f)) & ~1u;
    g_virtH = ((UINT)(g_nativeH * s + 0.5f)) & ~1u;
}

// ---- engine hooks ---------------------------------------------------------------------------

typedef void (__thiscall* PFN_Resize)(void* self, UINT x, UINT y, UINT fullscreen, INT posX, INT posY);
typedef void (__thiscall* PFN_GetSupportedResolution)(void* self, UINT* w, UINT* h);
static PFN_Resize g_origResize;
static PFN_GetSupportedResolution g_origGSR;

// GConfig->SetInt (vtable slot 0x40): the engine saves its viewport size as
// [WinDrv.WindowsClient] StartupResolutionX/Y; keep the native size in the ini so the game
// (with or without the mod) does not start at the virtual resolution next time.
typedef void (__thiscall* PFN_ConfigSetInt)(void* config, const wchar_t* section, const wchar_t* key, INT value, const wchar_t* file);
static PFN_ConfigSetInt g_origSetInt;

static void __fastcall HookConfigSetInt(void* config, void*, const wchar_t* section, const wchar_t* key, INT value, const wchar_t* file)
{
    if (g_active && section && key && lstrcmpiW(section, L"WinDrv.WindowsClient") == 0) {
        if (lstrcmpiW(key, L"StartupResolutionX") == 0 && (UINT)value == g_virtW) value = g_nativeW;
        if (lstrcmpiW(key, L"StartupResolutionY") == 0 && (UINT)value == g_virtH) value = g_nativeH;
    }
    g_origSetInt(config, section, key, value, file);
}

static void HookConfigOnce()
{
    void** config = *(void***)0x01BC3F44;  // GConfig
    if (g_origSetInt || IsBadReadPtr(config, 4) || !config) return;
    void** vtbl = *(void***)config;
    DWORD old;
    VirtualProtect(&vtbl[16], sizeof(void*), PAGE_READWRITE, &old);
    g_origSetInt = (PFN_ConfigSetInt)vtbl[16];
    vtbl[16] = (void*)HookConfigSetInt;
    VirtualProtect(&vtbl[16], sizeof(void*), old, &old);
    Log("supersample: config hook installed");
}

static void* g_viewport;       // the FWindowsViewport, for resizing at runtime
static UINT g_viewportFullscreen;

// FWindowsViewport::Resize: every viewport size change comes through here (startup, the game's
// resolution option, our runtime switch). Records the native size and passes the virtual size on
// while supersampling is on (fullscreen only).
static void __fastcall HookResize(void* self, void*, UINT x, UINT y, UINT fullscreen, INT posX, INT posY)
{
    HookConfigOnce();
    g_viewport = self;
    g_viewportFullscreen = fullscreen;
    if (!(g_active && x == g_virtW && y == g_virtH)) {
        // A real (native) size request; remember it so supersampling can be switched later.
        UINT dw, dh;
        DesktopSize(&dw, &dh);
        // A size above the desktop is a virtual size saved by an earlier session.
        if (x > dw || y > dh) { x = dw; y = dh; }
        g_nativeW = x;
        g_nativeH = y;
    }
    if (g_factor > 1.01f && fullscreen) {
        ComputeVirtual();
        g_active = true;
        Log("supersample: viewport %ux%u -> %ux%u", g_nativeW, g_nativeH, g_virtW, g_virtH);
        x = g_virtW;
        y = g_virtH;
    } else {
        if (g_active) Log("supersample: off (%s %ux%u)", fullscreen ? "fullscreen" : "windowed", g_nativeW, g_nativeH);
        g_active = false;
        x = g_nativeW;
        y = g_nativeH;
    }
    g_origResize(self, x, y, fullscreen, posX, posY);
}

// ---- changing the factor at runtime -----------------------------------------------------------
// The new factor is applied by resizing the engine viewport, the same path the game's own
// resolution option uses. That must happen on the game thread outside of drawing, so it is
// requested with a window message and done in the game window's procedure.

static const UINT kApplyMessage = WM_APP + 0x47;  // arbitrary private message
static WNDPROC g_origWndProc;
static float g_pendingFactor;

static LRESULT CALLBACK SupersampleWndProc(HWND h, UINT msg, WPARAM w, LPARAM l)
{
    if (msg == kApplyMessage) {
        if (g_viewport && g_nativeW && g_nativeH && g_pendingFactor >= 1.f) {
            g_factor = g_pendingFactor;
            Log("supersample: switching to %.2fx", g_factor);
            HookResize(g_viewport, nullptr, g_nativeW, g_nativeH, g_viewportFullscreen, -1, -1);
        }
        return 0;
    }
    return CallWindowProcW(g_origWndProc, h, msg, w, l);
}

float SupersampleSetting() { return g_factor; }

// Saves the factor and applies it right away (fullscreen only; windowed stays native).
void SupersampleSetFactor(float factor)
{
    if (factor < 1.f) factor = 1.f;
    if (factor > kMaxFactor) factor = kMaxFactor;
    char path[MAX_PATH], val[16];
    wsprintfA(path, "%srowe.ini", ModDir());
    sprintf_s(val, "%.2f", factor);
    WritePrivateProfileStringA("rowe", "supersample", val, path);
    g_pendingFactor = factor;
    if (g_window && g_origWndProc) PostMessageW(g_window, kApplyMessage, 0, 0);
    else g_factor = factor;  // before the window exists: used at startup
}

// Called from Present: remembers the game's swap chain and window (also when the swap chain was
// created without supersampling, so it can be switched on later).
void SupersampleOnPresent(IDXGISwapChain* swap, HWND window)
{
    if (!g_swap) g_swap = swap;
    if (window && !g_origWndProc) {
        g_window = window;
        g_origWndProc = (WNDPROC)SetWindowLongPtrW(window, GWLP_WNDPROC, (LONG_PTR)SupersampleWndProc);
    }
}

// The RHI would snap the virtual size to the nearest real display mode; leave it alone.
static void __fastcall HookGSR(void* self, void*, UINT* w, UINT* h)
{
    if (g_active && w && h && *w == g_virtW && *h == g_virtH) return;
    g_origGSR(self, w, h);
}

// ---- DXGI factory ---------------------------------------------------------------------------

typedef HRESULT (WINAPI* PFN_CreateDXGIFactory)(REFIID, void**);
typedef HRESULT (STDMETHODCALLTYPE* PFN_CreateSwapChain)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
static PFN_CreateSwapChain g_origCreateSwapChain;
static void** g_patchedFactoryVtbl;

// The engine asks for a swap chain at the virtual size: it is created at native size, and the
// engine renders into a virtual-size texture instead (handed out by the GetBuffer hook).
static HRESULT STDMETHODCALLTYPE HookCreateSwapChain(IDXGIFactory* f, IUnknown* dev, DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** out)
{
    if (g_active && desc && desc->BufferDesc.Width == g_virtW && desc->BufferDesc.Height == g_virtH) {
        DXGI_SWAP_CHAIN_DESC d = *desc;
        d.BufferDesc.Width = g_nativeW;
        d.BufferDesc.Height = g_nativeH;
        HRESULT hr = g_origCreateSwapChain(f, dev, &d, out);
        Log("supersample: CreateSwapChain %ux%u (engine sees %ux%u) windowed=%d hr=0x%08lx",
            g_nativeW, g_nativeH, g_virtW, g_virtH, d.Windowed, hr);
        if (SUCCEEDED(hr) && out && *out) {
            Dx10HookPatchSwapChain(*out);  // GetBuffer must be hooked before the engine calls it
            g_swap = *out;
            g_window = desc->OutputWindow;
            ID3D10Device* d3d = nullptr;
            if (SUCCEEDED(dev->QueryInterface(__uuidof(ID3D10Device), (void**)&d3d))) {
                SupersampleCreateVirtual(d3d, desc->BufferDesc.Format);
                d3d->Release();
            }
        }
        return hr;
    }
    return g_origCreateSwapChain(f, dev, desc, out);
}

// Stands in for the engine's CreateDXGIFactory; patches CreateSwapChain (vtable slot 10).
static HRESULT WINAPI HookCreateDXGIFactory(REFIID riid, void** out)
{
    static PFN_CreateDXGIFactory real;
    if (!real) real = (PFN_CreateDXGIFactory)GetProcAddress(LoadLibraryA("dxgi.dll"), "CreateDXGIFactory");
    if (!real) return E_FAIL;
    HRESULT hr = real(riid, out);
    if (SUCCEEDED(hr) && out && *out) {
        void** vtbl = *(void***)*out;
        if (vtbl != g_patchedFactoryVtbl) {
            DWORD old;
            VirtualProtect(&vtbl[10], sizeof(void*), PAGE_READWRITE, &old);
            if (vtbl[10] != (void*)HookCreateSwapChain) {
                g_origCreateSwapChain = (PFN_CreateSwapChain)vtbl[10];
                vtbl[10] = (void*)HookCreateSwapChain;
            }
            VirtualProtect(&vtbl[10], sizeof(void*), old, &old);
            g_patchedFactoryVtbl = vtbl;
        }
        Log("supersample: engine DXGI factory hooked");
    }
    return hr;
}

// Called from DllMain: the engine resolves CreateDXGIFactory lazily through this slot.
void SupersampleEarlyInstall()
{
    if ((DWORD)GetModuleHandleA(nullptr) != 0x00400000) return;
    void** slot = (void**)0x01B9DF34;
    if (IsBadReadPtr(slot, 4) || *slot != (void*)0x012CEB7E) {
        Log("supersample: delay-load slot mismatch, not hooked");
        return;
    }
    DWORD old;
    VirtualProtect(slot, 4, PAGE_READWRITE, &old);
    *slot = (void*)HookCreateDXGIFactory;
    VirtualProtect(slot, 4, old, &old);
}

// ---- window size / cursor mapping ------------------------------------------------------------
// The engine sees a client area of the virtual size (GetClientRect) and maps client points back
// to the screen (ClientToScreen, e.g. to centre the cursor). Cursor positions read through
// ScreenToClient stay native: the UI lays out at native size and hit-tests the mouse in native
// pixels (see HookUiViewportCallback and HookPixelToScreen).

typedef BOOL (WINAPI* PFN_PtFn)(HWND, LPPOINT);
typedef BOOL (WINAPI* PFN_GetClientRect)(HWND, LPRECT);
static PFN_PtFn g_origClientToScreen;
static PFN_GetClientRect g_origGetClientRect;

// True for the game window while the engine works at a virtual size.
static bool Scaling(HWND h) { return g_active && g_swap && h == g_window && g_nativeW && g_nativeH; }

static BOOL WINAPI HookClientToScreen(HWND h, LPPOINT p)
{
    if (p && Scaling(h)) {
        p->x = MulDiv(p->x, g_nativeW, g_virtW);
        p->y = MulDiv(p->y, g_nativeH, g_virtH);
    }
    return g_origClientToScreen(h, p);
}

static BOOL WINAPI HookGetClientRect(HWND h, LPRECT rc)
{
    BOOL r = g_origGetClientRect(h, rc);
    if (r && rc && Scaling(h)) {
        rc->right = rc->left + MulDiv(rc->right - rc->left, g_virtW, g_nativeW);
        rc->bottom = rc->top + MulDiv(rc->bottom - rc->top, g_virtH, g_nativeH);
    }
    return r;
}

// ---- scaled passes ----------------------------------------------------------------------------------
// 2D drawn at native layout and scaled up by our transform ("scaled passes"). Text drawn outside
// them (subtitles) needs its own correction, see AdjustSubtitleScale.

static int g_scaledDepth;

static void BeginScaledPass()
{
    if (g_active) ++g_scaledDepth;
}

static void EndScaledPass()
{
    if (g_scaledDepth > 0) --g_scaledDepth;
}

// Replaces the FCanvas::PopTransform call that closes a scaled pass.
typedef void (__thiscall* PFN_Void)(void* self);
static const PFN_Void kPopTransform = (PFN_Void)0x0055D980;

static void __fastcall HookPassPop(void* fcanvas, void*)
{
    kPopTransform(fcanvas);
    EndScaledPass();
}

// Subtitles (UnSubtitleManager, 0x005633D0..) are drawn at full resolution outside our scaled
// passes, with scale = clamp(min(StartupResX/1280, StartupResY/720) * 0.9, 1, 1.5) computed from
// the client's (native) startup resolution, so at a virtual resolution they would shrink. Their
// centred-text calls (0x00562CD0: Canvas, X, Y, Text, Font, Color, ScaleX, ScaleY) get the scale
// multiplied by the supersampling factor before the text is measured and drawn.
static void* g_centeredTextTramp;

static void __cdecl AdjustSubtitleScale(DWORD* args, DWORD returnAddress)
{
    if (!g_active || g_scaledDepth || returnAddress < 0x005633D0 || returnAddress >= 0x00563700) return;
    float s = (float)g_virtH / (float)g_nativeH;
    *(float*)&args[6] *= s;
    *(float*)&args[7] *= s;
}

__declspec(naked) static void CenteredTextThunk()
{
    __asm {
        mov eax, [esp]        // return address
        push eax
        lea eax, [esp + 8]    // first argument
        push eax
        call AdjustSubtitleScale
        add esp, 8
        jmp g_centeredTextTramp
    }
}

// ---- HUD at native size ------------------------------------------------------------------------
// UGameViewportClient::Draw sets each player's HUD canvas size from the view (0x00490537),
// calls UCanvas::Update (call at 0x00490559) which copies it to ClipX/ClipY, and pushes a
// translation for the view origin (FCanvas::PushRelativeTransform, call at 0x0049060E) before
// the HUD's PostRender. The HUD art has fixed pixel sizes, so at a virtual resolution it would
// shrink; instead the canvas gets the native size and the transform scales it back up.

typedef void (__thiscall* PFN_CanvasUpdate)(void* canvas);
typedef void (__thiscall* PFN_PushTransform)(void* canvas, float* matrix);
static PFN_CanvasUpdate g_origCanvasUpdate;
static PFN_PushTransform g_origPushTransform;

static void __fastcall HookHudCanvasUpdate(void* canvas, void*)
{
    if (g_active && g_nativeW && g_virtW) {
        INT* size = (INT*)((BYTE*)canvas + 0x6C);  // SizeX, SizeY
        size[0] = MulDiv(size[0], g_nativeW, g_virtW);
        size[1] = MulDiv(size[1], g_nativeH, g_virtH);
    }
    g_origCanvasUpdate(canvas);
}

static void __fastcall HookHudPushTransform(void* canvas, void*, float* m)
{
    if (g_active && g_nativeW && g_virtW) {
        m[0] *= (float)g_virtW / (float)g_nativeW;
        m[5] *= (float)g_virtH / (float)g_nativeH;
        BeginScaledPass();  // ended by HookPassPop at the pass's PopTransform
    }
    g_origPushTransform(canvas, m);
}

// Third pass: a full-viewport canvas (Update call at 0x00491555) for the viewport client's
// PostRender (subtitles and similar) and the UI scenes, which are rendered on the same FCanvas
// (call 0x0061D990 at 0x0049156C). It has no transform of its own, so a scale is pushed after
// Update and popped when the pass's stat scope closes (call 0x00405520 at 0x004915B6). The UI
// lays out at native size (HookUiViewportSize), so it renders under this scale like the HUD.
typedef void (__thiscall* PFN_UiRender)(void* uiController, void* fcanvas);
static PFN_UiRender g_origUiRender;
static PFN_Void g_origScopeEnd;
static void* g_viewportPassCanvas;  // FCanvas with our scale pushed, or null

// Pushes a native -> virtual scale onto the canvas transform stack.
static void PushScale(void* fcanvas)
{
    __declspec(align(16)) float m[16] = {};  // the engine's matrix code uses aligned SSE loads
    m[0] = (float)g_virtW / (float)g_nativeW;
    m[5] = (float)g_virtH / (float)g_nativeH;
    m[10] = m[15] = 1.f;
    g_origPushTransform(fcanvas, m);
    BeginScaledPass();
}

static void PopScale(void* fcanvas)
{
    kPopTransform(fcanvas);
    EndScaledPass();
}

// Replaces the pass's UCanvas::Update: native canvas size, then the scale.
static void __fastcall HookViewportPassUpdate(BYTE* canvas, void*)
{
    GameCmdTick();  // this pass runs once per frame on the game thread, menus included
    HookHudCanvasUpdate(canvas, nullptr);
    void* fcanvas = *(void**)(canvas + 0x74);
    if (g_active && g_nativeW && g_virtW && fcanvas && !g_viewportPassCanvas) {
        PushScale(fcanvas);
        g_viewportPassCanvas = fcanvas;
    }
}

// UI scenes: UUISceneClient::GetViewportSize (vtable slot 0x130) is answered with the native
// size, so every scene (menus, dialogs, tips — including the ones laid out in fixed pixels)
// lays out exactly as at native resolution; the UI is then rendered inside the scaled pass.
typedef UINT (__thiscall* PFN_GetViewportSize)(void* sceneClient, void* scene, float* outSize);
static PFN_GetViewportSize g_origUiViewportSize;
static bool g_inUiRender;
static bool g_inUiResize;  // the viewport already reports native size (HookUiViewportCallback)

static UINT __fastcall HookUiViewportSize(void* sceneClient, void*, void* scene, float* out)
{
    UINT r = g_origUiViewportSize(sceneClient, scene, out);
    if (r && out && g_active && g_virtW && g_virtH && !g_inUiResize) {
        out[0] *= (float)g_nativeW / (float)g_virtW;
        out[1] *= (float)g_nativeH / (float)g_virtH;
    }
    return r;
}

// The scene client exists once the UI is up; its vtable is patched on the first UI render.
static void HookSceneClientOnce(BYTE* uiController)
{
    if (g_origUiViewportSize || !uiController) return;
    void* sceneClient = *(void**)(uiController + 0x78);
    if (!sceneClient) return;
    void** vtbl = *(void***)sceneClient;
    DWORD old;
    VirtualProtect(&vtbl[0x130 / 4], sizeof(void*), PAGE_READWRITE, &old);
    g_origUiViewportSize = (PFN_GetViewportSize)vtbl[0x130 / 4];
    vtbl[0x130 / 4] = (void*)HookUiViewportSize;
    VirtualProtect(&vtbl[0x130 / 4], sizeof(void*), old, &old);
    Log("supersample: UI viewport size hook installed (vtbl %p)", vtbl);
}

// When the viewport is resized at runtime, the UI scene client's callback (0x00611EE0, event
// 0x29 = viewport resized) reads the new size straight from the viewport (FViewport fields
// +0x44/+0x48), stores it in every scene and sends NotifyResolutionChanged. While it runs the
// viewport reports the native size, so the UI keeps its native layout across a factor change.
// The scene client also rebuilds its canvas-to-screen matrices (mouse hit tests) in there through
// GetViewportSize, which must then not scale a second time.
typedef void (__thiscall* PFN_UiViewportCallback)(void* self, int type, BYTE* viewport, UINT message);
static PFN_UiViewportCallback g_origUiViewportCallback;

static void __fastcall HookUiViewportCallback(void* self, void*, int type, BYTE* viewport, UINT message)
{
    if (type != 0x29 || !viewport || !g_active || IsBadWritePtr(viewport + 0x44, 8)) {
        g_origUiViewportCallback(self, type, viewport, message);
        return;
    }
    UINT* size = (UINT*)(viewport + 0x44);
    UINT saved[2] = {size[0], size[1]};
    size[0] = g_nativeW;
    size[1] = g_nativeH;
    g_inUiResize = true;
    g_origUiViewportCallback(self, type, viewport, message);
    g_inUiResize = false;
    size[0] = saved[0];
    size[1] = saved[1];
}

// Mouse hit tests: a widget converts the mouse position (native client pixels) to normalized
// screen space in UUIScreenObject::PixelToScreen (0x005A3F70), dividing by the size of the
// real viewport (virtual). Everything else in the UI is native, so the point is scaled up to
// virtual pixels first, which gives the normalized position of the native layout.
typedef void (__thiscall* PFN_PixelToScreen)(void* self, float* out, const float* pixel);
static PFN_PixelToScreen g_origPixelToScreen;

static void __fastcall HookPixelToScreen(void* self, void*, float* out, const float* pixel)
{
    if (g_active && g_nativeW && g_nativeH && g_virtW && g_virtH && pixel) {
        float p[2] = {pixel[0] * g_virtW / g_nativeW, pixel[1] * g_virtH / g_nativeH};
        g_origPixelToScreen(self, out, p);
        return;
    }
    g_origPixelToScreen(self, out, pixel);
}

// Replaces the call that renders the UI scenes inside the viewport pass.
static void __fastcall HookViewportPassUiRender(void* ui, void*, void* fcanvas)
{
    HookSceneClientOnce((BYTE*)ui);
    // The UI lays out at native size, so it renders under the pass's scale like the HUD.
    g_inUiRender = true;
    g_origUiRender(ui, fcanvas);
    g_inUiRender = false;
}

// Replaces the stat scope end that closes the viewport pass: pops our scale first.
static void __fastcall HookViewportPassEnd(void* scope, void*)
{
    if (g_viewportPassCanvas) {
        PopScale(g_viewportPassCanvas);
        g_viewportPassCanvas = nullptr;
    }
    g_origScopeEnd(scope);
}

// The in-game HUD is a UI scene rendered by 0x00EC53C0: it sizes the UCanvas (esi) from the
// scene, then pushes the scene's widget transform (call 0x0055CC00 at 0x00EC5503) and renders.
// Same treatment: native canvas size, transform scaled up. A thunk passes esi (the UCanvas).
static PFN_PushTransform g_origUiPushTransform;

static void __stdcall UiPushTransformImpl(void* fcanvas, float* m, BYTE* ucanvas)
{
    // Inside the UI render the scene already has native layout under the pass's scale.
    if (g_active && g_nativeW && g_virtW && !g_inUiRender) {
        float sx = (float)g_virtW / (float)g_nativeW, sy = (float)g_virtH / (float)g_nativeH;
        *(INT*)(ucanvas + 0x6C) = MulDiv(*(INT*)(ucanvas + 0x6C), g_nativeW, g_virtW);  // SizeX
        *(INT*)(ucanvas + 0x70) = MulDiv(*(INT*)(ucanvas + 0x70), g_nativeH, g_virtH);  // SizeY
        *(float*)(ucanvas + 0x48) /= sx;  // OrgX
        *(float*)(ucanvas + 0x4C) /= sy;  // OrgY
        *(float*)(ucanvas + 0x50) /= sx;  // ClipX
        *(float*)(ucanvas + 0x54) /= sy;  // ClipY
        // Scale the transform's output: x column (0,4,8,12) and y column (1,5,9,13).
        for (int r = 0; r < 4; ++r) {
            m[r * 4 + 0] *= sx;
            m[r * 4 + 1] *= sy;
        }
        BeginScaledPass();  // ended by HookPassPop at 0x00EC5512
    }
    g_origUiPushTransform(fcanvas, m);
}

// __thiscall entry with the UCanvas still in esi; forwards (FCanvas, matrix, UCanvas).
__declspec(naked) static void UiPushTransformThunk()
{
    __asm {
        push esi             // UCanvas
        push dword ptr [esp + 8]  // matrix (the caller's stack argument)
        push ecx             // FCanvas
        call UiPushTransformImpl
        ret 4
    }
}

// Redirects one `call rel32` instruction; returns the original call target or null.
static void* PatchCall(DWORD site, DWORD expectedTarget, void* hook, const char* name)
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

// ---- install ---------------------------------------------------------------------------------

// Installs the engine-side hooks (from DllMain). Each group is all-or-nothing where a partial
// patch would leave a transform pushed without its pop or a canvas at the wrong size.
void SupersampleInstall()
{
    static const BYTE kResize[7] = {0x6A, 0xFF, 0x68, 0xB2, 0xB5, 0x4D, 0x01};  // push -1; push 014DB5B2h
    static const BYTE kGSR[7] = {0x6A, 0xFF, 0x68, 0x26, 0x53, 0x52, 0x01};     // push -1; push 01525326h
    g_origResize = (PFN_Resize)InstallDetour(0x00E6F080, kResize, sizeof(kResize), (void*)HookResize, "ss-resize");
    g_origGSR = (PFN_GetSupportedResolution)InstallDetour(0x0113C540, kGSR, sizeof(kGSR), (void*)HookGSR, "ss-modes");
    if (!g_origResize || !g_origGSR) {
        g_factor = 1.f;  // without both hooks the engine cannot be told the virtual size
        return;
    }
    // Two per-player canvas passes in UGameViewportClient::Draw use this pattern (Update call,
    // PushRelativeTransform call, PopTransform call after the HUD); patch each set together or
    // not at all, since a scaled transform without the native canvas size would misplace the pass.
    static const DWORD kPasses[][3] = {{0x00490559, 0x0049060E, 0x00490655}, {0x0049126D, 0x00491326, 0x004913C4}};
    int patched = 0;
    for (auto& pass : kPasses) {
        BYTE* u = (BYTE*)pass[0];
        BYTE* t = (BYTE*)pass[1];
        BYTE* p = (BYTE*)pass[2];
        bool ok = !IsBadReadPtr(u, 5) && !IsBadReadPtr(t, 5) && !IsBadReadPtr(p, 5) &&
                  u[0] == 0xE8 && t[0] == 0xE8 && p[0] == 0xE8 &&
                  pass[0] + 5 + *(int*)(u + 1) == 0x0055BF30 && pass[1] + 5 + *(int*)(t + 1) == 0x0055CC50 &&
                  pass[2] + 5 + *(int*)(p + 1) == 0x0055D980;
        if (!ok) {
            Log("ss-hud: pass %08lx mismatch, not hooked", pass[0]);
            continue;
        }
        PatchCall(pass[0], 0x0055BF30, (void*)HookHudCanvasUpdate, "ss-hud-size");
        PatchCall(pass[1], 0x0055CC50, (void*)HookHudPushTransform, "ss-hud-scale");
        PatchCall(pass[2], 0x0055D980, (void*)HookPassPop, "ss-hud-pop");
        ++patched;
    }
    if (patched) {
        g_origCanvasUpdate = (PFN_CanvasUpdate)0x0055BF30;
        g_origPushTransform = (PFN_PushTransform)0x0055CC50;
        // Viewport pass: all three call sites or none (a pushed scale must always be popped).
        BYTE* a = (BYTE*)0x00491555;
        BYTE* b = (BYTE*)0x0049156C;
        BYTE* c = (BYTE*)0x004915B6;
        bool ok = a[0] == 0xE8 && b[0] == 0xE8 && c[0] == 0xE8 &&
                  0x00491555 + 5 + *(int*)(a + 1) == 0x0055BF30 &&
                  0x0049156C + 5 + *(int*)(b + 1) == 0x0061D990 &&
                  0x004915B6 + 5 + *(int*)(c + 1) == 0x00405520;
        if (ok) {
            g_origUiRender = (PFN_UiRender)0x0061D990;
            g_origScopeEnd = (PFN_Void)0x00405520;
            PatchCall(0x004915B6, 0x00405520, (void*)HookViewportPassEnd, "ss-vp-end");
            PatchCall(0x0049156C, 0x0061D990, (void*)HookViewportPassUiRender, "ss-vp-ui");
            PatchCall(0x00491555, 0x0055BF30, (void*)HookViewportPassUpdate, "ss-vp-size");
        } else {
            Log("ss-vp: viewport pass mismatch, not hooked");
        }
    }
    static const BYTE kUiCallback[6] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8};  // push ebp; mov ebp,esp; and esp,-8
    g_origUiViewportCallback = (PFN_UiViewportCallback)InstallDetour(0x00611EE0, kUiCallback, sizeof(kUiCallback),
        (void*)HookUiViewportCallback, "ss-ui-resize");
    static const BYTE kPixelToScreen[6] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0};  // push ebp; mov ebp,esp; and esp,-16
    g_origPixelToScreen = (PFN_PixelToScreen)InstallDetour(0x005A3F70, kPixelToScreen, sizeof(kPixelToScreen),
        (void*)HookPixelToScreen, "ss-ui-mouse");
    static const BYTE kCentered[8] = {0x83, 0xEC, 0x08, 0x56, 0x8B, 0x74, 0x24, 0x1C};  // sub esp,8; push esi; mov esi,[esp+1Ch]
    g_centeredTextTramp = InstallDetour(0x00562CD0, kCentered, sizeof(kCentered), (void*)CenteredTextThunk, "ss-subtitles");
    // HUD scene: the push and its matching pop together.
    BYTE* sp = (BYTE*)0x00EC5512;
    if (!IsBadReadPtr(sp, 5) && sp[0] == 0xE8 && 0x00EC5512 + 5 + *(int*)(sp + 1) == 0x0055D980) {
        g_origUiPushTransform = (PFN_PushTransform)PatchCall(0x00EC5503, 0x0055CC00, (void*)UiPushTransformThunk, "ss-hud-scene");
        if (g_origUiPushTransform) PatchCall(0x00EC5512, 0x0055D980, (void*)HookPassPop, "ss-hud-scene-pop");
    }
    Log("supersample: hud hooks %d %d %d", !!g_origCanvasUpdate, !!g_origPushTransform, !!g_origUiPushTransform);
    g_origClientToScreen = (PFN_PtFn)PatchExeImport("user32.dll", "ClientToScreen", (void*)HookClientToScreen);
    g_origGetClientRect = (PFN_GetClientRect)PatchExeImport("user32.dll", "GetClientRect", (void*)HookGetClientRect);
    Log("supersample: window hooks %d %d", !!g_origClientToScreen, !!g_origGetClientRect);
}

// ---- virtual back buffer + resolve -----------------------------------------------------------

static ID3D10Device* g_dev;
static ID3D10Texture2D* g_virtTex;
static ID3D10ShaderResourceView* g_virtSRV;
static ID3D10VertexShader* g_vs;
static ID3D10PixelShader* g_ps;
static ID3D10Buffer* g_cb;
static ID3D10RasterizerState* g_raster;
static ID3D10DepthStencilState* g_depth;
static ID3D10BlendState* g_blend;
static ID3D10StateBlock* g_stateBlock;
static ID3D10RenderTargetView* g_realRTV;
static ID3D10Texture2D* g_realRTVTarget;  // weak

template <class T> static void SafeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }

static void ReleaseVirtual()
{
    SafeRelease(g_virtSRV);
    SafeRelease(g_virtTex);
}

// (Re)creates the virtual-size texture the engine renders into; if that fails, supersampling
// switches itself off.
void SupersampleCreateVirtual(ID3D10Device* dev, DXGI_FORMAT fmt)
{
    ReleaseVirtual();
    if (dev != g_dev) {
        SafeRelease(g_stateBlock); SafeRelease(g_blend); SafeRelease(g_depth); SafeRelease(g_raster);
        SafeRelease(g_cb); SafeRelease(g_ps); SafeRelease(g_vs); SafeRelease(g_realRTV);
        SafeRelease(g_dev);
        g_dev = dev;
        g_dev->AddRef();
    }
    D3D10_TEXTURE2D_DESC td = {};
    td.Width = g_virtW;
    td.Height = g_virtH;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = fmt;
    td.SampleDesc.Count = 1;
    td.Usage = D3D10_USAGE_DEFAULT;
    td.BindFlags = D3D10_BIND_RENDER_TARGET | D3D10_BIND_SHADER_RESOURCE;
    HRESULT hr = dev->CreateTexture2D(&td, nullptr, &g_virtTex);
    if (SUCCEEDED(hr)) hr = dev->CreateShaderResourceView(g_virtTex, nullptr, &g_virtSRV);
    Log("supersample: virtual back buffer %ux%u fmt%d hr=0x%08lx", g_virtW, g_virtH, fmt, hr);
    if (FAILED(hr)) {
        ReleaseVirtual();
        g_active = false;
    }
}

// Shaders and states for the resolve pass, created on first use.
static bool CreateResolveResources()
{
    if (FAILED(g_dev->CreateVertexShader(g_downscaleVS, sizeof(g_downscaleVS), &g_vs))) return false;
    if (FAILED(g_dev->CreatePixelShader(g_downscalePS, sizeof(g_downscalePS), &g_ps))) return false;
    D3D10_BUFFER_DESC bd = {};
    bd.ByteWidth = 16;
    bd.Usage = D3D10_USAGE_DYNAMIC;
    bd.BindFlags = D3D10_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D10_CPU_ACCESS_WRITE;
    if (FAILED(g_dev->CreateBuffer(&bd, nullptr, &g_cb))) return false;
    D3D10_RASTERIZER_DESC rs = {};
    rs.FillMode = D3D10_FILL_SOLID;
    rs.CullMode = D3D10_CULL_NONE;
    rs.DepthClipEnable = TRUE;
    if (FAILED(g_dev->CreateRasterizerState(&rs, &g_raster))) return false;
    D3D10_DEPTH_STENCIL_DESC ds = {};
    if (FAILED(g_dev->CreateDepthStencilState(&ds, &g_depth))) return false;
    D3D10_BLEND_DESC bl = {};
    bl.SrcBlend = bl.SrcBlendAlpha = D3D10_BLEND_ONE;
    bl.DestBlend = bl.DestBlendAlpha = D3D10_BLEND_ZERO;
    bl.BlendOp = bl.BlendOpAlpha = D3D10_BLEND_OP_ADD;
    bl.RenderTargetWriteMask[0] = D3D10_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(g_dev->CreateBlendState(&bl, &g_blend))) return false;
    D3D10_STATE_BLOCK_MASK mask;
    D3D10StateBlockMaskEnableAll(&mask);
    return SUCCEEDED(D3D10CreateStateBlock(g_dev, &mask, &g_stateBlock));
}

// Averages the virtual frame down into the real back buffer (downscale.hlsl), at Present.
static void Resolve(ID3D10Texture2D* real)
{
    static bool failed;
    if (failed || !g_virtSRV) return;
    if (!g_vs && !CreateResolveResources()) {
        Log("supersample: resolve resources failed");
        failed = true;
        return;
    }
    if (real != g_realRTVTarget || !g_realRTV) {
        SafeRelease(g_realRTV);
        if (FAILED(g_dev->CreateRenderTargetView(real, nullptr, &g_realRTV))) return;
        g_realRTVTarget = real;
    }
    D3D10_TEXTURE2D_DESC rd;
    real->GetDesc(&rd);

    float* cb = nullptr;
    if (FAILED(g_cb->Map(D3D10_MAP_WRITE_DISCARD, 0, (void**)&cb))) return;
    cb[0] = (float)g_virtW / (float)rd.Width;
    cb[1] = (float)g_virtH / (float)rd.Height;
    cb[2] = (float)(g_virtW - 1);
    cb[3] = (float)(g_virtH - 1);
    g_cb->Unmap();

    g_stateBlock->Capture();
    D3D10_VIEWPORT vp = {0, 0, rd.Width, rd.Height, 0.f, 1.f};
    float blendFactor[4] = {0, 0, 0, 0};
    g_dev->OMSetRenderTargets(1, &g_realRTV, nullptr);
    g_dev->OMSetBlendState(g_blend, blendFactor, 0xFFFFFFFF);
    g_dev->OMSetDepthStencilState(g_depth, 0);
    g_dev->RSSetViewports(1, &vp);
    g_dev->RSSetState(g_raster);
    g_dev->IASetInputLayout(nullptr);
    g_dev->IASetPrimitiveTopology(D3D10_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_dev->VSSetShader(g_vs);
    g_dev->GSSetShader(nullptr);
    g_dev->PSSetShader(g_ps);
    g_dev->PSSetConstantBuffers(0, 1, &g_cb);
    g_dev->PSSetShaderResources(0, 1, &g_virtSRV);
    g_dev->Draw(3, 0);
    ID3D10ShaderResourceView* nullSRV = nullptr;
    g_dev->PSSetShaderResources(0, 1, &nullSRV);
    g_stateBlock->Apply();
    g_stateBlock->ReleaseAllDeviceObjects();  // don't keep the engine's targets referenced
}

// ---- swap chain glue (called from dx10hook.cpp) -----------------------------------------------

// The engine's swap chain while a virtual buffer exists.
bool SupersampleOwns(IDXGISwapChain* swap) { return swap && swap == g_swap && g_virtTex; }

// GetBuffer(0) for the engine: the virtual texture.
HRESULT SupersampleGetBuffer(REFIID riid, void** out)
{
    return g_virtTex->QueryInterface(riid, out);
}

// GetDesc: the engine sees the virtual size.
void SupersampleFixDesc(IDXGISwapChain* swap, DXGI_SWAP_CHAIN_DESC* d)
{
    if (SupersampleOwns(swap) && d) {
        d->BufferDesc.Width = g_virtW;
        d->BufferDesc.Height = g_virtH;
    }
}

// Rewrites a ResizeBuffers request; returns true if the virtual buffer must be recreated after.
bool SupersampleBeforeResize(IDXGISwapChain* swap, UINT* w, UINT* h)
{
    if (!swap || swap != g_swap) return false;
    SafeRelease(g_realRTV);
    g_realRTVTarget = nullptr;
    ReleaseVirtual();
    if (g_active && *w == g_virtW && *h == g_virtH) {
        *w = g_nativeW;
        *h = g_nativeH;
        return true;
    }
    return false;  // non-virtual size: the swap chain goes back to normal
}

// After a resize that was mapped to native size: a new virtual texture at the new size.
void SupersampleAfterResize(IDXGISwapChain* swap, DXGI_FORMAT fmt)
{
    ID3D10Device* dev = nullptr;
    if (SUCCEEDED(swap->GetDevice(__uuidof(ID3D10Device), (void**)&dev)) && dev) {
        if (fmt == DXGI_FORMAT_UNKNOWN) fmt = DXGI_FORMAT_R8G8B8A8_UNORM;
        SupersampleCreateVirtual(dev, fmt);
        dev->Release();
    }
}

// ResizeTarget: display modes stay native.
void SupersampleFixMode(IDXGISwapChain* swap, DXGI_MODE_DESC* m)
{
    if (swap == g_swap && g_active && m && m->Width == g_virtW && m->Height == g_virtH) {
        m->Width = g_nativeW;
        m->Height = g_nativeH;
    }
}

// Present: resolve the virtual frame into the real back buffer.
void SupersamplePresent(IDXGISwapChain* swap, ID3D10Texture2D* real)
{
    if (SupersampleOwns(swap) && real) Resolve(real);
}
