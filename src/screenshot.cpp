// Screenshots.
//
// The screenshot key (rowe.ini screenshotkey, default F9) saves the finished frame (HUD
// included, before the settings menu is drawn) to screenshotdir, at the supersampled size
// (screenshotsize=full, the frame the engine rendered before it is averaged down) or the
// screen's, as JPEG or PNG (screenshotformat, screenshotquality), encoded by Windows' imaging
// codecs (WIC). The game is not held up while the file is written: the frame is copied on the GPU
// in the frame it is taken, read back over the next frames in strips (a staging copy polled
// without waiting) and encoded on a worker thread.
//
// With showcase=1 the key takes the showcase series instead (see "showcase series" below).
//
// Tools: rowe_shot.req saves the frame at the screen's size, rowe_ui.req the same with the
// settings menu drawn and rowe_scene.req the 3D scene before the HUD (from lens.cpp), all as
// rowe_<kind>_<date>_<time>.png next to the DLL.
#include <windows.h>
#include <d3d10.h>
#include <wincodec.h>
#include <stdio.h>
#include <stdlib.h>
#include "rowe.h"
#include "keynames.h"

// ---- settings ------------------------------------------------------------------------------

static UINT g_key = VK_F9;
static char g_keyName[32] = "F9";
static char g_dir[MAX_PATH];  // as written in the ini (may hold %VARIABLES%); empty = next to the DLL
static bool g_jpg = true;
static int g_quality = 95;
static bool g_full = true;
static bool g_showcase;

static void IniPath(char* out) { wsprintfA(out, "%srowe.ini", ModDir()); }

// Reads the screenshot settings from rowe.ini (also used to reload them at runtime).
void ScreenshotInit()
{
    char path[MAX_PATH], val[64];
    IniPath(path);
    GetPrivateProfileStringA("rowe", "screenshotkey", "F9", val, sizeof(val), path);
    g_key = KeyFromName(val);
    if (!g_key) {
        Log("screenshot: unknown key '%s', using F9", val);
        g_key = VK_F9;
        lstrcpyA(val, "F9");
    }
    lstrcpynA(g_keyName, val, sizeof(g_keyName));
    CharUpperA(g_keyName);
    GetPrivateProfileStringA("rowe", "screenshotdir", "", g_dir, sizeof(g_dir), path);
    GetPrivateProfileStringA("rowe", "screenshotformat", "jpg", val, sizeof(val), path);
    g_jpg = lstrcmpiA(val, "png") != 0;
    g_quality = GetPrivateProfileIntA("rowe", "screenshotquality", 95, path);
    if (g_quality < 1) g_quality = 1;
    if (g_quality > 100) g_quality = 100;
    GetPrivateProfileStringA("rowe", "screenshotsize", "full", val, sizeof(val), path);
    g_full = lstrcmpiA(val, "screen") != 0 && lstrcmpiA(val, "window") != 0;
    g_showcase = GetPrivateProfileIntA("rowe", "showcase", 0, path) != 0;
}

bool ScreenshotJpg() { return g_jpg; }
const char* ScreenshotKeyName() { return g_keyName; }
bool ScreenshotFull() { return g_full; }

void ScreenshotSetJpg(bool jpg)
{
    g_jpg = jpg;
    char path[MAX_PATH];
    IniPath(path);
    WritePrivateProfileStringA("rowe", "screenshotformat", jpg ? "jpg" : "png", path);
}

bool ScreenshotShowcase() { return g_showcase; }

void ScreenshotSetShowcase(bool on)
{
    g_showcase = on;
    char path[MAX_PATH];
    IniPath(path);
    WritePrivateProfileStringA("rowe", "showcase", on ? "1" : "0", path);
}

void ScreenshotSetFull(bool full)
{
    g_full = full;
    char path[MAX_PATH];
    IniPath(path);
    WritePrivateProfileStringA("rowe", "screenshotsize", full ? "full" : "screen", path);
}

// The screenshot folder (created if missing), with a trailing backslash. A relative path starts
// at the DLL's folder.
static void Folder(char* out)
{
    char raw[MAX_PATH], full[MAX_PATH], dir[MAX_PATH];
    if (!g_dir[0]) {
        lstrcpynA(out, ModDir(), MAX_PATH);
        return;
    }
    ExpandEnvironmentStringsA(g_dir, raw, MAX_PATH);
    bool absolute = (raw[0] && raw[1] == ':') || (raw[0] == '\\' && raw[1] == '\\');
    if (absolute) lstrcpynA(full, raw, MAX_PATH);
    else _snprintf(full, MAX_PATH - 1, "%s%s", ModDir(), raw), full[MAX_PATH - 1] = 0;
    GetFullPathNameA(full, MAX_PATH, dir, nullptr);
    int n = lstrlenA(dir);
    if (n && dir[n - 1] != '\\' && dir[n - 1] != '/' && n < MAX_PATH - 1) lstrcatA(dir, "\\");
    for (char* p = dir + 3; *p; ++p) {  // create each level: "C:\a\b\" -> "C:\a", "C:\a\b"
        if (*p != '\\' && *p != '/') continue;
        char c = *p;
        *p = 0;
        CreateDirectoryA(dir, nullptr);
        *p = c;
    }
    lstrcpynA(out, dir, MAX_PATH);
}

static void TimeStamp(char* out)
{
    SYSTEMTIME t;
    GetLocalTime(&t);
    wsprintfA(out, "%04d%02d%02d_%02d%02d%02d_%03d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
}

// ---- encoder thread ------------------------------------------------------------------------
// The render thread hands over the frame in strips of raw rows (4 bytes per pixel, no padding);
// the worker converts each to 24-bit BGR and passes it to the WIC encoder.

enum PixelKind { kRGBA8, kBGRA8, kRGB10A2 };

struct Job {
    char path[MAX_PATH];
    UINT w, h;
    PixelKind kind;
    bool jpg;
    int quality;
    DWORD start;
};

struct Strip { BYTE* data; UINT rows; };

static const int kQueueSize = 8;
static const UINT kStripRows = 256;
static const int kStripsPerFrame = 2;

static Job s_job;
static Strip s_queue[kQueueSize];
static int s_qHead, s_qCount;
static CRITICAL_SECTION s_qLock;
static HANDLE s_qEvent;
static volatile LONG s_busy;           // a screenshot is being taken or written
static volatile bool s_allQueued, s_abort;

static bool Pop(Strip* out)
{
    EnterCriticalSection(&s_qLock);
    bool got = s_qCount > 0;
    if (got) {
        *out = s_queue[s_qHead];
        s_qHead = (s_qHead + 1) % kQueueSize;
        --s_qCount;
    }
    LeaveCriticalSection(&s_qLock);
    return got;
}

static bool Push(BYTE* data, UINT rows)
{
    EnterCriticalSection(&s_qLock);
    bool room = s_qCount < kQueueSize;
    if (room) {
        s_queue[(s_qHead + s_qCount) % kQueueSize] = {data, rows};
        ++s_qCount;
    }
    LeaveCriticalSection(&s_qLock);
    if (room) SetEvent(s_qEvent);
    return room;
}

static bool QueueFull()
{
    EnterCriticalSection(&s_qLock);
    bool full = s_qCount >= kQueueSize;
    LeaveCriticalSection(&s_qLock);
    return full;
}

// In place: 4 bytes per pixel -> B, G, R (the output never overtakes the input).
static void ToBgr(BYTE* p, size_t pixels, PixelKind kind)
{
    const BYTE* s = p;
    BYTE* o = p;
    for (size_t i = 0; i < pixels; ++i, s += 4, o += 3) {
        BYTE r, g, b;
        if (kind == kRGB10A2) {
            DWORD v = *(const DWORD*)s;
            r = (BYTE)((v & 0x3FF) >> 2);
            g = (BYTE)(((v >> 10) & 0x3FF) >> 2);
            b = (BYTE)(((v >> 20) & 0x3FF) >> 2);
        } else if (kind == kRGBA8) {
            r = s[0]; g = s[1]; b = s[2];
        } else {
            b = s[0]; g = s[1]; r = s[2];
        }
        o[0] = b; o[1] = g; o[2] = r;
    }
}

static DWORD WINAPI EncodeThread(void*)
{
    HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IWICImagingFactory* factory = nullptr;
    IWICStream* stream = nullptr;
    IWICBitmapEncoder* encoder = nullptr;
    IWICBitmapFrameEncode* frame = nullptr;
    IPropertyBag2* props = nullptr;
    wchar_t wpath[MAX_PATH];
    MultiByteToWideChar(CP_ACP, 0, s_job.path, -1, wpath, MAX_PATH);
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (SUCCEEDED(hr)) hr = factory->CreateStream(&stream);
    if (SUCCEEDED(hr)) hr = stream->InitializeFromFilename(wpath, GENERIC_WRITE);
    if (SUCCEEDED(hr)) hr = factory->CreateEncoder(s_job.jpg ? GUID_ContainerFormatJpeg : GUID_ContainerFormatPng, nullptr, &encoder);
    if (SUCCEEDED(hr)) hr = encoder->Initialize(stream, WICBitmapEncoderNoCache);
    if (SUCCEEDED(hr)) hr = encoder->CreateNewFrame(&frame, &props);
    if (SUCCEEDED(hr) && props && s_job.jpg) {
        PROPBAG2 opt = {};
        opt.pstrName = (LPOLESTR)L"ImageQuality";
        VARIANT v;
        VariantInit(&v);
        v.vt = VT_R4;
        v.fltVal = s_job.quality / 100.f;
        props->Write(1, &opt, &v);
    }
    if (SUCCEEDED(hr)) hr = frame->Initialize(props);
    if (SUCCEEDED(hr)) hr = frame->SetSize(s_job.w, s_job.h);
    WICPixelFormatGUID fmt = GUID_WICPixelFormat24bppBGR;
    if (SUCCEEDED(hr)) hr = frame->SetPixelFormat(&fmt);
    if (SUCCEEDED(hr) && fmt != GUID_WICPixelFormat24bppBGR) hr = E_FAIL;  // both encoders take 24-bit BGR
    if (FAILED(hr)) Log("screenshot: encoder setup failed (%08lx) for %s", hr, s_job.path);
    UINT written = 0;
    for (;;) {
        Strip st;
        if (Pop(&st)) {
            if (SUCCEEDED(hr) && !s_abort) {
                ToBgr(st.data, (size_t)s_job.w * st.rows, s_job.kind);
                hr = frame->WritePixels(st.rows, s_job.w * 3, s_job.w * 3 * st.rows, st.data);
            }
            written += st.rows;
            free(st.data);
            continue;
        }
        if (s_abort || s_allQueued) {
            Strip rest;
            while (Pop(&rest)) free(rest.data);
            break;
        }
        WaitForSingleObject(s_qEvent, 200);
    }
    bool ok = SUCCEEDED(hr) && !s_abort && written == s_job.h;
    if (ok) hr = frame->Commit();
    if (ok && SUCCEEDED(hr)) hr = encoder->Commit();
    ok = ok && SUCCEEDED(hr);
    if (props) props->Release();
    if (frame) frame->Release();
    if (encoder) encoder->Release();
    if (stream) stream->Release();
    if (factory) factory->Release();
    if (SUCCEEDED(co)) CoUninitialize();
    if (!ok) DeleteFileA(s_job.path);
    Log("screenshot: %s (%ux%u, %s) %s in %lu ms", s_job.path, s_job.w, s_job.h, s_job.jpg ? "JPEG" : "PNG",
        ok ? "saved" : (s_abort ? "abandoned" : "FAILED"), GetTickCount() - s_job.start);
    InterlockedExchange(&s_busy, 0);
    return 0;
}

// ---- read-back (render thread) -------------------------------------------------------------

static ID3D10Device* s_dev;
static ID3D10Texture2D* s_staging;  // CPU-readable copy of the frame being taken
static bool s_mapped;
static D3D10_MAPPED_TEXTURE2D s_map;
static UINT s_nextRow;

static void ReleaseStaging()
{
    if (s_staging) {
        if (s_mapped) s_staging->Unmap(0);
        s_staging->Release();
    }
    s_staging = nullptr;
    s_mapped = false;
    if (s_dev) s_dev->Release();
    s_dev = nullptr;
}

static bool KindOf(DXGI_FORMAT f, PixelKind* kind)
{
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        *kind = kRGBA8; return true;
    case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8X8_UNORM:
        *kind = kBGRA8; return true;
    case DXGI_FORMAT_R10G10B10A2_UNORM: case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        *kind = kRGB10A2; return true;
    default:
        return false;
    }
}

// Copies `src` on the GPU and starts the worker that encodes it to `path`.
static void Start(ID3D10Device* dev, ID3D10Texture2D* src, const char* path, bool jpg)
{
    if (s_busy) {
        Log("screenshot: still writing the previous one, skipped");
        return;
    }
    D3D10_TEXTURE2D_DESC d;
    src->GetDesc(&d);
    PixelKind kind;
    if (!KindOf(d.Format, &kind)) {
        Log("screenshot: unsupported frame format %d", d.Format);
        return;
    }
    D3D10_TEXTURE2D_DESC sd = d;
    sd.MipLevels = 1;
    sd.ArraySize = 1;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Usage = D3D10_USAGE_STAGING;
    sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D10_CPU_ACCESS_READ;
    sd.MiscFlags = 0;
    if (FAILED(dev->CreateTexture2D(&sd, nullptr, &s_staging))) {
        Log("screenshot: no memory for a %ux%u copy", d.Width, d.Height);
        s_staging = nullptr;
        return;
    }
    if (d.SampleDesc.Count > 1) {  // MSAA: resolve into a plain texture first
        D3D10_TEXTURE2D_DESC rd = sd;
        rd.Usage = D3D10_USAGE_DEFAULT;
        rd.CPUAccessFlags = 0;
        ID3D10Texture2D* resolved = nullptr;
        if (FAILED(dev->CreateTexture2D(&rd, nullptr, &resolved))) {
            ReleaseStaging();
            return;
        }
        dev->ResolveSubresource(resolved, 0, src, 0, d.Format);
        dev->CopyResource(s_staging, resolved);
        resolved->Release();
    } else {
        dev->CopyResource(s_staging, src);
    }
    s_dev = dev;
    s_dev->AddRef();
    s_mapped = false;
    s_nextRow = 0;

    lstrcpynA(s_job.path, path, MAX_PATH);
    s_job.w = d.Width;
    s_job.h = d.Height;
    s_job.kind = kind;
    s_job.jpg = jpg;
    s_job.quality = g_quality;
    s_job.start = GetTickCount();
    s_allQueued = s_abort = false;
    if (!s_qEvent) {
        InitializeCriticalSection(&s_qLock);
        s_qEvent = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    }
    InterlockedExchange(&s_busy, 1);
    HANDLE t = CreateThread(nullptr, 0, EncodeThread, nullptr, 0, nullptr);
    if (!t) {
        Log("screenshot: worker thread failed");
        ReleaseStaging();
        InterlockedExchange(&s_busy, 0);
        return;
    }
    CloseHandle(t);
    Log("screenshot: taking %ux%u -> %s", d.Width, d.Height, path);
}

// Once the GPU has finished the copy (polled, never waited for), queues up to kStripsPerFrame
// strips for the worker.
static void ReadStrips()
{
    if (!s_staging) return;
    if (!s_mapped) {
        HRESULT hr = s_staging->Map(0, D3D10_MAP_READ, D3D10_MAP_FLAG_DO_NOT_WAIT, &s_map);
        if (hr == DXGI_ERROR_WAS_STILL_DRAWING) return;
        if (FAILED(hr)) {
            Log("screenshot: read back failed (%08lx)", hr);
            s_abort = true;
            SetEvent(s_qEvent);
            ReleaseStaging();
            return;
        }
        s_mapped = true;
    }
    for (int i = 0; i < kStripsPerFrame && s_nextRow < s_job.h && !QueueFull(); ++i) {
        UINT rows = s_job.h - s_nextRow < kStripRows ? s_job.h - s_nextRow : kStripRows;
        size_t rowBytes = (size_t)s_job.w * 4;
        BYTE* data = (BYTE*)malloc(rowBytes * rows);
        if (!data) {
            Log("screenshot: out of memory at row %u", s_nextRow);
            s_abort = true;
            SetEvent(s_qEvent);
            ReleaseStaging();
            return;
        }
        for (UINT y = 0; y < rows; ++y)
            memcpy(data + y * rowBytes, (const BYTE*)s_map.pData + (size_t)(s_nextRow + y) * s_map.RowPitch, rowBytes);
        Push(data, rows);
        s_nextRow += rows;
    }
    if (s_nextRow >= s_job.h) {
        s_allQueued = true;
        SetEvent(s_qEvent);
        ReleaseStaging();
    }
}

// ---- triggers ------------------------------------------------------------------------------

static bool Requested(const char* name, DWORD& counter)
{
    if (++counter % 15 != 0) return false;
    char req[MAX_PATH];
    wsprintfA(req, "%s%s", ModDir(), name);
    if (GetFileAttributesA(req) == INVALID_FILE_ATTRIBUTES) return false;
    DeleteFileA(req);
    return true;
}

// Test captures: PNG next to the DLL.
static void ToolPath(char* out, const char* kind)
{
    char stamp[32];
    TimeStamp(stamp);
    _snprintf(out, MAX_PATH - 1, "%srowe_%s_%s.png", ModDir(), kind, stamp);
    out[MAX_PATH - 1] = 0;
}

// Every frame from Present, after the supersampling resolve and before the settings menu is
// drawn. focused: the game window is in front (the key is only read then).
static volatile bool s_userRequest;
static char s_pathRequest[MAX_PATH];  // ScreenshotRequestTo: screen-size JPEG to this path
static volatile bool s_pathPending;

// The next frame at the screen's size as a JPEG to `path` (game thread; used for the launcher's
// background).
void ScreenshotRequestTo(const char* path)
{
    lstrcpynA(s_pathRequest, path, MAX_PATH);
    s_pathPending = true;
}

// Tools (`usershot` command): takes a screenshot as the key does.
void ScreenshotRequestUser() { s_userRequest = true; }

// ---- showcase series ---------------------------------------------------------------------------
// With showcase=1 the screenshot key takes a series instead of one shot: no lens, Panini,
// stereographic, fisheye and fisheye full circle at FOV 75, 90 and 159 (15 shots), at the
// screen's size with the stats window in them, named ROWE_showcase_<time>_fov<fov>_<lens>.
// Each setting gets kShowcaseSettle frames to take effect, and a shot waits until the previous
// one is written. The next shot's settings are applied on the frame after a shot (as in ROWE for
// Fallout New Vegas, where the stats window is drawn into the copy later in the frame; here it is
// drawn before, which this keeps safe either way). The FOV and lens are set without saving, and
// the user's come back afterwards; the key again stops the series. From the FNV port.

static const float kShowcaseFov[] = {75.f, 90.f, 159.f};
static const int kShowcaseLens[] = {kLensOff, kLensPanini, kLensStereographic, kLensFisheye, kLensFisheyeCircle};
static const char* kShowcaseLensName[] = {"nolens", "panini", "stereographic", "fisheye", "fisheyecircle"};
static const int kShowcaseLenses = 5, kShowcaseShots = 3 * kShowcaseLenses, kShowcaseSettle = 20;
static int s_showcaseShot = -1;  // the next shot of the running series (kShowcaseShots: all taken), -1 = none
static int s_showcaseWait;       // frames left before the next shot
static bool s_showcaseNext;      // a shot was taken last frame: apply the next one's settings now
static char s_showcaseStamp[32];
static float s_userFov;
static int s_userLens;

static void ShowcaseApply(int shot)
{
    FovSet(kShowcaseFov[shot / kShowcaseLenses], false);
    LensSetMode(kShowcaseLens[shot % kShowcaseLenses], false);
    s_showcaseWait = kShowcaseSettle;
}

static void ShowcaseEnd(const char* how)
{
    FovSet(s_userFov, false);
    LensSetMode(s_userLens, false);
    OverlayForceStats(false);
    Log("showcase: %s after %d of %d shots", how, s_showcaseShot, kShowcaseShots);
    s_showcaseShot = -1;
}

static void ShowcaseStart()
{
    s_userFov = FovGet();
    s_userLens = LensGetMode();
    OverlayForceStats(true);
    TimeStamp(s_showcaseStamp);
    s_showcaseShot = 0;
    s_showcaseNext = false;
    ShowcaseApply(0);
    Log("showcase: a series of %d shots", kShowcaseShots);
}

static void ShowcaseFrame(IDXGISwapChain* swap)
{
    if (s_showcaseShot < 0) return;
    if (s_showcaseShot >= kShowcaseShots) {  // the last one was taken last frame
        ShowcaseEnd("done");
        return;
    }
    if (s_showcaseNext) {
        s_showcaseNext = false;
        ShowcaseApply(s_showcaseShot);
        return;
    }
    if (s_showcaseWait > 0) { --s_showcaseWait; return; }
    if (s_busy) return;  // the previous shot is still being written
    ID3D10Device* dev = nullptr;
    ID3D10Texture2D* src = nullptr;
    if (FAILED(swap->GetDevice(__uuidof(ID3D10Device), (void**)&dev)) || !dev) return;
    if (SUCCEEDED(SwapGetRealBuffer(swap, &src)) && src) {
        char dir[MAX_PATH], path[MAX_PATH];
        Folder(dir);
        _snprintf(path, MAX_PATH - 1, "%sROWE_showcase_%s_fov%03d_%s.%s", dir, s_showcaseStamp,
                  (int)kShowcaseFov[s_showcaseShot / kShowcaseLenses], kShowcaseLensName[s_showcaseShot % kShowcaseLenses],
                  g_jpg ? "jpg" : "png");
        path[MAX_PATH - 1] = 0;
        Start(dev, src, path, g_jpg);
        src->Release();
        if (++s_showcaseShot < kShowcaseShots) s_showcaseNext = true;
    }
    dev->Release();
}

void ScreenshotFrame(IDXGISwapChain* swap, bool focused)
{
    static bool prev;
    static DWORD counter;
    static bool wantTool;
    bool down = focused && (GetAsyncKeyState(g_key) & 0x8000) != 0;
    bool wantUser = (down && !prev) || s_userRequest;
    s_userRequest = false;
    prev = down;
    if (Requested("rowe_shot.req", counter)) wantTool = true;

    ReadStrips();
    if (wantUser && (s_showcaseShot >= 0 || g_showcase)) {  // the showcase series instead of one shot
        wantUser = false;
        if (s_showcaseShot >= 0) ShowcaseEnd("stopped");
        else ShowcaseStart();
    }
    ShowcaseFrame(swap);
    bool wantPath = s_pathPending && !s_busy;
    if (!wantUser && !(wantTool && !s_busy) && !wantPath) return;

    ID3D10Device* dev = nullptr;
    if (FAILED(swap->GetDevice(__uuidof(ID3D10Device), (void**)&dev)) || !dev) return;
    ID3D10Texture2D* src = nullptr;
    char path[MAX_PATH];
    if (wantUser) {
        // The supersampled frame the engine rendered, or the real (screen-size) back buffer.
        if (g_full && SupersampleOwns(swap) && SUCCEEDED(SupersampleGetBuffer(__uuidof(ID3D10Texture2D), (void**)&src)))
            OverlayStatsInto(src);  // (on the screen-size frame it is drawn already)
        else
            SwapGetRealBuffer(swap, &src);
        char dir[MAX_PATH], stamp[32];
        Folder(dir);
        TimeStamp(stamp);
        _snprintf(path, MAX_PATH - 1, "%sGearsOfWar_%s.%s", dir, stamp, g_jpg ? "jpg" : "png");
        path[MAX_PATH - 1] = 0;
        if (src) Start(dev, src, path, g_jpg);
    } else if (wantPath) {
        s_pathPending = false;
        SwapGetRealBuffer(swap, &src);
        if (src) Start(dev, src, s_pathRequest, true);
    } else {
        wantTool = false;
        SwapGetRealBuffer(swap, &src);
        ToolPath(path, "shot");
        if (src) Start(dev, src, path, false);
    }
    if (src) src->Release();
    dev->Release();
}

// After the settings menu is drawn (from Present): rowe_ui.req.
void ScreenshotWithMenu(IDXGISwapChain* swap)
{
    static DWORD counter;
    static bool want;
    if (Requested("rowe_ui.req", counter)) want = true;
    if (!want || s_busy) return;
    want = false;
    ID3D10Device* dev = nullptr;
    ID3D10Texture2D* src = nullptr;
    if (FAILED(swap->GetDevice(__uuidof(ID3D10Device), (void**)&dev)) || !dev) return;
    if (SUCCEEDED(SwapGetRealBuffer(swap, &src)) && src) {
        char path[MAX_PATH];
        ToolPath(path, "ui");
        Start(dev, src, path, false);
        src->Release();
    }
    dev->Release();
}

// After the 3D scene, before the HUD (render thread, from lens.cpp): rowe_scene.req.
void ScreenshotScene(IDXGISwapChain* swap)
{
    static DWORD counter;
    static bool want;
    if (Requested("rowe_scene.req", counter)) want = true;
    if (!want || s_busy) return;
    want = false;
    ID3D10Device* dev = nullptr;
    ID3D10Texture2D* bb = nullptr;
    if (FAILED(swap->GetDevice(__uuidof(ID3D10Device), (void**)&dev)) || !dev) return;
    if (SUCCEEDED(swap->GetBuffer(0, __uuidof(ID3D10Texture2D), (void**)&bb)) && bb) {
        char path[MAX_PATH];
        ToolPath(path, "scene");
        Start(dev, bb, path, false);
        bb->Release();
    }
    dev->Release();
}
