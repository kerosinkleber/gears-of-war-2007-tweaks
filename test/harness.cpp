// Test harness: loads out\d3d9.dll like the game does, renders a few D3D10 frames through a
// hooked swap chain with the overlay forced open, and saves a capture to out\harness.bmp.
#include <windows.h>
#include <d3d10.h>
#include <stdio.h>

typedef void* (WINAPI* PFN_Direct3DCreate9)(UINT);

static void SaveBmp(HWND hwnd, const char* path)
{
    RECT rc;
    GetClientRect(hwnd, &rc);
    int w = rc.right, h = rc.bottom;
    HDC wdc = GetDC(hwnd);
    HDC mdc = CreateCompatibleDC(wdc);
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(wdc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(mdc, bmp);
    PrintWindow(hwnd, mdc, PW_CLIENTONLY | 2 /* PW_RENDERFULLCONTENT */);
    GdiFlush();

    BITMAPFILEHEADER fh = {};
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(fh) + sizeof(bi.bmiHeader);
    fh.bfSize = fh.bfOffBits + w * h * 4;
    FILE* f = fopen(path, "wb");
    if (f) {
        fwrite(&fh, sizeof(fh), 1, f);
        fwrite(&bi.bmiHeader, sizeof(bi.bmiHeader), 1, f);
        fwrite(bits, w * h * 4, 1, f);
        fclose(f);
    }
    SelectObject(mdc, old);
    DeleteObject(bmp);
    DeleteDC(mdc);
    ReleaseDC(hwnd, wdc);
}

int main()
{
    char dir[MAX_PATH];
    GetModuleFileNameA(nullptr, dir, MAX_PATH);
    *strrchr(dir, '\\') = 0;
    char dll[MAX_PATH], out[MAX_PATH];
    sprintf(dll, "%s\\d3d9.dll", dir);
    sprintf(out, "%s\\harness.bmp", dir);

    SetEnvironmentVariableA("GOWFOV_TEST", "1");
    HMODULE mod = LoadLibraryA(dll);
    if (!mod) { printf("LoadLibrary failed %lu\n", GetLastError()); return 1; }
    PFN_Direct3DCreate9 create = (PFN_Direct3DCreate9)GetProcAddress(mod, "Direct3DCreate9");
    printf("Direct3DCreate9 -> %p\n", create ? create(32) : nullptr);

    WNDCLASSA wc = {};
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "gowfov_harness";
    RegisterClassA(&wc);
    RECT r = {0, 0, 1280, 720};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowA("gowfov_harness", "gowfov harness", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        50, 50, r.right - r.left, r.bottom - r.top, nullptr, nullptr, wc.hInstance, nullptr);

    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 1;
    sd.BufferDesc.Width = 1280;
    sd.BufferDesc.Height = 720;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    IDXGISwapChain* swap = nullptr;
    ID3D10Device* dev = nullptr;
    HRESULT hr = D3D10CreateDeviceAndSwapChain(nullptr, D3D10_DRIVER_TYPE_HARDWARE, nullptr, 0,
        D3D10_SDK_VERSION, &sd, &swap, &dev);
    if (FAILED(hr)) { printf("device failed 0x%08lx\n", hr); return 1; }

    ID3D10Texture2D* bb = nullptr;
    swap->GetBuffer(0, __uuidof(ID3D10Texture2D), (void**)&bb);
    ID3D10RenderTargetView* rtv = nullptr;
    dev->CreateRenderTargetView(bb, nullptr, &rtv);
    bb->Release();

    for (int frame = 0; frame < 90; ++frame) {
        MSG msg;
        while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageA(&msg);
        float clear[4] = {0.25f, 0.35f, 0.45f, 1};
        dev->OMSetRenderTargets(1, &rtv, nullptr);
        dev->ClearRenderTargetView(rtv, clear);
        swap->Present(1, 0);
        if (frame == 80) SaveBmp(hwnd, out);
    }
    printf("saved %s\n", out);
    rtv->Release();
    swap->Release();
    dev->Release();
    DestroyWindow(hwnd);
    return 0;
}
