// Hooks IDXGISwapChain. The swap chain vtable is shared by every swap chain of the same class,
// so it is patched through a throwaway device + swap chain (and again on the engine's own swap
// chain when supersampling creates it, in case that happens first).
#include <windows.h>
#include <d3d10.h>
#include "gowfov.h"

typedef HRESULT (STDMETHODCALLTYPE* PFN_Present)(IDXGISwapChain*, UINT, UINT);
typedef HRESULT (STDMETHODCALLTYPE* PFN_GetBuffer)(IDXGISwapChain*, UINT, REFIID, void**);
typedef HRESULT (STDMETHODCALLTYPE* PFN_GetDesc)(IDXGISwapChain*, DXGI_SWAP_CHAIN_DESC*);
typedef HRESULT (STDMETHODCALLTYPE* PFN_ResizeBuffers)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
typedef HRESULT (STDMETHODCALLTYPE* PFN_ResizeTarget)(IDXGISwapChain*, const DXGI_MODE_DESC*);

static PFN_Present g_origPresent;
static PFN_GetBuffer g_origGetBuffer;
static PFN_GetDesc g_origGetDesc;
static PFN_ResizeBuffers g_origResize;
static PFN_ResizeTarget g_origResizeTarget;

static void PatchSlot(void** vtbl, int index, void* fn, void** orig)
{
    if (vtbl[index] == fn) return;
    DWORD old;
    VirtualProtect(&vtbl[index], sizeof(void*), PAGE_READWRITE, &old);
    *orig = vtbl[index];
    vtbl[index] = fn;
    VirtualProtect(&vtbl[index], sizeof(void*), old, &old);
}

// The swap chain's actual back buffer, even when GetBuffer hands the engine a virtual one.
HRESULT SwapGetRealBuffer(IDXGISwapChain* swap, ID3D10Texture2D** out)
{
    return g_origGetBuffer(swap, 0, __uuidof(ID3D10Texture2D), (void**)out);
}

// Once per frame on the render thread, after the engine has drawn everything (scene, lens warp,
// HUD, menus). Order matters: the supersampling resolve fills the real back buffer, then the
// overlay draws on top of it.
static HRESULT STDMETHODCALLTYPE HookPresent(IDXGISwapChain* swap, UINT sync, UINT flags)
{
    static int count;
    if (count++ == 0) Log("first Present swap=%p", swap);
    if (!(flags & DXGI_PRESENT_TEST)) {
        static HWND window;
        if (!window) {
            DXGI_SWAP_CHAIN_DESC sd;
            if (SUCCEEDED(g_origGetDesc(swap, &sd))) {
                window = sd.OutputWindow;
                GameCmdSetWindow(window);
            }
        }
        SupersampleOnPresent(swap, window);  // remembers the swap chain, subclasses the window
        if (BackgroundEnabled()) BackgroundOnPresent(window);
        LensOnPresent(swap);                 // per-frame lens bookkeeping, settings reload
        FovTracePresent();                   // diagnostics trace (only when requested)
        ID3D10Texture2D* real = nullptr;
        if (SUCCEEDED(SwapGetRealBuffer(swap, &real)) && real) {
            SupersamplePresent(swap, real);
            real->Release();
        }
        OverlayFrame(swap);
    }
    return g_origPresent(swap, sync, flags);
}

// While supersampling, the engine gets the virtual-size texture instead of the real buffer.
static HRESULT STDMETHODCALLTYPE HookGetBuffer(IDXGISwapChain* swap, UINT index, REFIID riid, void** out)
{
    if (index == 0 && SupersampleOwns(swap)) return SupersampleGetBuffer(riid, out);
    return g_origGetBuffer(swap, index, riid, out);
}

// Reports the virtual size while supersampling, matching what GetBuffer hands out.
static HRESULT STDMETHODCALLTYPE HookGetDesc(IDXGISwapChain* swap, DXGI_SWAP_CHAIN_DESC* desc)
{
    HRESULT hr = g_origGetDesc(swap, desc);
    if (SUCCEEDED(hr)) SupersampleFixDesc(swap, desc);
    return hr;
}

// Views of the old buffers are dropped first (a live reference makes ResizeBuffers fail); a
// virtual size is turned into the native size of the real swap chain.
static HRESULT STDMETHODCALLTYPE HookResize(IDXGISwapChain* swap, UINT n, UINT w, UINT h, DXGI_FORMAT fmt, UINT flags)
{
    OverlayLost();
    LensOnResize();
    bool virt = SupersampleBeforeResize(swap, &w, &h);
    HRESULT hr = g_origResize(swap, n, w, h, fmt, flags);
    if (virt && SUCCEEDED(hr)) SupersampleAfterResize(swap, fmt);
    return hr;
}

// Display mode changes are made at the native size, never the virtual one.
static HRESULT STDMETHODCALLTYPE HookResizeTarget(IDXGISwapChain* swap, const DXGI_MODE_DESC* mode)
{
    if (mode) {
        DXGI_MODE_DESC m = *mode;
        SupersampleFixMode(swap, &m);
        return g_origResizeTarget(swap, &m);
    }
    return g_origResizeTarget(swap, mode);
}

void Dx10HookPatchSwapChain(IDXGISwapChain* swap)
{
    void** vtbl = *(void***)swap;
    PatchSlot(vtbl, 8, (void*)HookPresent, (void**)&g_origPresent);
    PatchSlot(vtbl, 9, (void*)HookGetBuffer, (void**)&g_origGetBuffer);
    PatchSlot(vtbl, 12, (void*)HookGetDesc, (void**)&g_origGetDesc);
    PatchSlot(vtbl, 13, (void*)HookResize, (void**)&g_origResize);
    PatchSlot(vtbl, 14, (void*)HookResizeTarget, (void**)&g_origResizeTarget);
}

// Creates a throwaway device and swap chain on a hidden window only to reach the IDXGISwapChain
// vtable, which the engine's swap chain will share.
void Dx10HookInstall()
{
    WNDCLASSA wc = {};
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "gowfov_dummy";
    RegisterClassA(&wc);
    HWND hw = CreateWindowA("gowfov_dummy", "", WS_OVERLAPPED, 0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);

    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 1;
    sd.BufferDesc.Width = 64;
    sd.BufferDesc.Height = 64;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hw;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    IDXGISwapChain* swap = nullptr;
    ID3D10Device* dev = nullptr;
    HRESULT hr = D3D10CreateDeviceAndSwapChain(nullptr, D3D10_DRIVER_TYPE_HARDWARE, nullptr, 0,
        D3D10_SDK_VERSION, &sd, &swap, &dev);
    Log("dummy D3D10 device hr=0x%08lx", hr);
    if (SUCCEEDED(hr)) {
        Dx10HookPatchSwapChain(swap);
        swap->Release();
        dev->Release();
    }
    if (hw) DestroyWindow(hw);
    UnregisterClassA("gowfov_dummy", wc.hInstance);
}
