// Saves the swap chain's back buffer as a BMP in the mod folder. Triggered by F9, or by
// creating gowfov_shot.req next to the DLL (lets tools request a capture without a key press).
#include <windows.h>
#include <d3d10.h>
#include <stdio.h>
#include "gowfov.h"

static bool IsBGRA(DXGI_FORMAT f)
{
    return f == DXGI_FORMAT_B8G8R8A8_UNORM || f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
           f == DXGI_FORMAT_B8G8R8X8_UNORM || f == DXGI_FORMAT_B8G8R8A8_TYPELESS;
}

static bool IsRGBA(DXGI_FORMAT f)
{
    return f == DXGI_FORMAT_R8G8B8A8_UNORM || f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
           f == DXGI_FORMAT_R8G8B8A8_TYPELESS;
}

static bool IsRGB10A2(DXGI_FORMAT f)
{
    return f == DXGI_FORMAT_R10G10B10A2_UNORM || f == DXGI_FORMAT_R10G10B10A2_TYPELESS;
}

// Writes `bb` as a 24-bit BMP: resolves MSAA if needed, copies to a CPU-readable staging
// texture and converts each pixel from the buffer's format.
static bool Save(ID3D10Device* dev, ID3D10Texture2D* bb, const char* path)
{
    D3D10_TEXTURE2D_DESC desc;
    bb->GetDesc(&desc);
    if (!IsBGRA(desc.Format) && !IsRGBA(desc.Format) && !IsRGB10A2(desc.Format)) {
        Log("screenshot: unsupported back buffer format %d", desc.Format);
        return false;
    }

    ID3D10Texture2D* src = bb;
    ID3D10Texture2D* resolved = nullptr;
    if (desc.SampleDesc.Count > 1) {
        D3D10_TEXTURE2D_DESC rd = desc;
        rd.SampleDesc.Count = 1;
        rd.SampleDesc.Quality = 0;
        rd.BindFlags = 0;
        rd.MiscFlags = 0;
        if (FAILED(dev->CreateTexture2D(&rd, nullptr, &resolved))) return false;
        dev->ResolveSubresource(resolved, 0, bb, 0, desc.Format);
        src = resolved;
    }

    D3D10_TEXTURE2D_DESC sd = desc;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Usage = D3D10_USAGE_STAGING;
    sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D10_CPU_ACCESS_READ;
    sd.MiscFlags = 0;
    ID3D10Texture2D* staging = nullptr;
    bool ok = false;
    if (SUCCEEDED(dev->CreateTexture2D(&sd, nullptr, &staging))) {
        dev->CopyResource(staging, src);
        D3D10_MAPPED_TEXTURE2D m;
        if (SUCCEEDED(staging->Map(0, D3D10_MAP_READ, 0, &m))) {
            FILE* f = fopen(path, "wb");
            if (f) {
                int w = desc.Width, h = desc.Height, rowBytes = w * 3, pad = (4 - rowBytes % 4) % 4;
                BITMAPFILEHEADER fh = {};
                BITMAPINFOHEADER ih = {};
                fh.bfType = 0x4D42;
                fh.bfOffBits = sizeof(fh) + sizeof(ih);
                fh.bfSize = fh.bfOffBits + (rowBytes + pad) * h;
                ih.biSize = sizeof(ih);
                ih.biWidth = w;
                ih.biHeight = h;  // bottom-up
                ih.biPlanes = 1;
                ih.biBitCount = 24;
                fwrite(&fh, sizeof(fh), 1, f);
                fwrite(&ih, sizeof(ih), 1, f);
                BYTE* row = (BYTE*)malloc(rowBytes + pad);
                memset(row, 0, rowBytes + pad);
                for (int y = h - 1; y >= 0; --y) {
                    const DWORD* px = (const DWORD*)((const BYTE*)m.pData + y * m.RowPitch);
                    for (int x = 0; x < w; ++x) {
                        DWORD p = px[x];
                        BYTE r, g, b;
                        if (IsRGB10A2(desc.Format)) {
                            r = (BYTE)((p & 0x3FF) >> 2);
                            g = (BYTE)(((p >> 10) & 0x3FF) >> 2);
                            b = (BYTE)(((p >> 20) & 0x3FF) >> 2);
                        } else if (IsRGBA(desc.Format)) {
                            r = (BYTE)p; g = (BYTE)(p >> 8); b = (BYTE)(p >> 16);
                        } else {
                            b = (BYTE)p; g = (BYTE)(p >> 8); r = (BYTE)(p >> 16);
                        }
                        row[x * 3] = b; row[x * 3 + 1] = g; row[x * 3 + 2] = r;
                    }
                    fwrite(row, rowBytes + pad, 1, f);
                }
                free(row);
                fclose(f);
                ok = true;
            }
            staging->Unmap(0);
        }
        staging->Release();
    }
    if (resolved) resolved->Release();
    return ok;
}

// Every 15th call: true (and the file deleted) if the request file `name` exists.
static bool Requested(const char* name, DWORD& counter)
{
    if (++counter % 15 != 0) return false;
    char req[MAX_PATH];
    wsprintfA(req, "%s%s", ModDir(), name);
    if (GetFileAttributesA(req) == INVALID_FILE_ATTRIBUTES) return false;
    DeleteFileA(req);
    return true;
}

// realBuffer: capture what is presented (native size) instead of what the engine renders into
// (the larger virtual buffer when supersampling).
static void Capture(IDXGISwapChain* swap, const char* prefix, bool realBuffer)
{
    ID3D10Device* dev = nullptr;
    ID3D10Texture2D* bb = nullptr;
    if (FAILED(swap->GetDevice(__uuidof(ID3D10Device), (void**)&dev))) return;
    HRESULT hr = realBuffer ? SwapGetRealBuffer(swap, &bb)
                            : swap->GetBuffer(0, __uuidof(ID3D10Texture2D), (void**)&bb);
    if (SUCCEEDED(hr)) {
        SYSTEMTIME t;
        GetLocalTime(&t);
        char path[MAX_PATH];
        wsprintfA(path, "%s%s_%04d%02d%02d_%02d%02d%02d.bmp", ModDir(), prefix,
            t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
        bool ok = Save(dev, bb, path);
        Log("screenshot %s: %s", ok ? "saved" : "FAILED", path);
        bb->Release();
    }
    dev->Release();
}

void ScreenshotFrame(IDXGISwapChain* swap, bool keyPressed)
{
    static DWORD counter;
    if (Requested("gowfov_shot.req", counter) || keyPressed) Capture(swap, "gowfov", true);
}

void ScreenshotScene(IDXGISwapChain* swap)
{
    static DWORD counter;
    if (Requested("gowfov_scene.req", counter)) Capture(swap, "gowfov_scene", false);
}
