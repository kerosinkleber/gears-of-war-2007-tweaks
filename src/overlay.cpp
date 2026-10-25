// Minimal D3D10 overlay: the F8 settings menu, drawn with flat quads and a built-in bitmap font.
#include <windows.h>
#include <d3d10.h>
#include <math.h>
#include <string.h>
#include "gowfov.h"
#include "overlay_vs.h"
#include "overlay_ps.h"

static const int kMinFov = 60;
static const int kMaxFov = (int)kFovMax;
static const int kToggleKey = VK_F8;

static bool g_open;
static bool g_testMode;  // GOWFOV_TEST=1: always open, no focus check (used by the test harness)
static bool g_keyPrev[256];

// ---- UI geometry (pixel space) -------------------------------------------------------------

struct Quad { float x, y, w, h; DWORD color; };  // color is 0xAARRGGBB
static Quad g_quads[8192];
static int g_quadCount;

// 5x7 glyphs, one 5-bit row per byte.
struct Glyph { char c; unsigned char rows[7]; };
static const Glyph kFont[] = {
    {'0',{14,17,19,21,25,17,14}}, {'1',{4,12,4,4,4,4,14}},   {'2',{14,17,1,2,4,8,31}},
    {'3',{30,1,1,14,1,1,30}},     {'4',{2,6,10,18,31,2,2}},  {'5',{31,16,30,1,1,17,14}},
    {'6',{6,8,16,30,17,17,14}},   {'7',{31,1,2,4,8,8,8}},    {'8',{14,17,17,14,17,17,14}},
    {'9',{14,17,17,15,1,2,12}},   {'A',{14,17,17,31,17,17,17}}, {'D',{30,17,17,17,17,17,30}},
    {'E',{31,16,16,30,16,16,31}}, {'F',{31,16,16,30,16,16,16}}, {'G',{14,17,16,23,17,17,15}},
    {'H',{17,17,17,31,17,17,17}}, {'I',{14,4,4,4,4,4,14}},   {'L',{16,16,16,16,16,16,31}},
    {'O',{14,17,17,17,17,17,14}}, {'R',{30,17,17,30,20,18,17}}, {'S',{15,16,16,14,1,1,30}},
    {'T',{31,4,4,4,4,4,4}},       {'U',{17,17,17,17,17,17,14}}, {'V',{17,17,17,17,17,10,4}},
    {'W',{17,17,17,21,21,27,17}}, {'X',{17,17,10,4,10,17,17}}, {'Y',{17,17,10,4,4,4,4}},
    {'N',{17,25,21,19,17,17,17}}, {'C',{14,17,16,16,16,17,14}}, {'M',{17,27,21,21,17,17,17}},
    {'P',{30,17,17,30,16,16,16}}, {'B',{30,17,17,30,17,17,30}}, {'K',{17,18,20,24,20,18,17}},
    {'Z',{31,1,2,4,8,16,31}},     {'J',{7,2,2,2,2,18,12}},
    {'.',{0,0,0,0,0,12,12}},   {':',{0,12,12,0,12,12,0}},
    {'-',{0,0,0,31,0,0,0}},       {'/',{1,1,2,4,8,16,16}},   {'<',{2,4,8,16,8,4,2}},
    {'>',{8,4,2,1,2,4,8}},
};

// Glyph rows for a character (letters are upper-cased), or null if the font lacks it.
static const unsigned char* FindGlyph(char c)
{
    if (c >= 'a' && c <= 'z') c -= 32;
    for (const Glyph& g : kFont)
        if (g.c == c) return g.rows;
    return nullptr;
}

// Adds one quad in back buffer pixels.
static void Rect(float x, float y, float w, float h, DWORD c)
{
    if (g_quadCount >= (int)(sizeof(g_quads) / sizeof(g_quads[0]))) return;
    g_quads[g_quadCount++] = {x, y, w, h, c};
}

// Draws a string as one quad per lit font pixel; px is the size of a font pixel.
static void Text(float x, float y, const char* s, float px, DWORD c)
{
    for (; *s; ++s, x += 6 * px) {
        const unsigned char* g = FindGlyph(*s);
        if (!g) continue;
        for (int r = 0; r < 7; ++r)
            for (int col = 0; col < 5; ++col)
                if (g[r] & (16 >> col)) Rect(x + col * px, y + r * px, px, px, c);
    }
}

// ---- settings menu -------------------------------------------------------------------------

enum { kRowFov, kRowAimFov, kRowLens, kRowSupersample, kRowShoulder, kRowMouseTrap, kRowShotFormat, kRowShotSize,
       kRowIntros, kRowCount };
static const float kSupersampleSteps[] = {1.f, 2.f, 2.25f, 3.f, 4.f, 5.f, 6.f, 8.f, 9.f, 10.f};
static const int kSupersampleStepCount = sizeof(kSupersampleSteps) / sizeof(kSupersampleSteps[0]);
static const DWORD kSupersampleApplyDelay = 600;  // ms after the last change, so cycling is cheap

static int g_row;
static float g_ssPending = -1.f;  // chosen but not yet applied
static DWORD g_ssChangeTime;

// "OFF" or e.g. "2.25X".
static void FormatFactor(char* out, float f)
{
    if (f <= 1.01f) lstrcpyA(out, "OFF");
    else wsprintfA(out, "%d.%02dX", (int)f, (int)(f * 100 + 0.5f) % 100);
}

// Lays out the menu as quads; bh is the back buffer height (picks the glyph size). Each row shows
// a label, the current value and an optional note.
static void BuildUI(int bh)
{
    g_quadCount = 0;
    float s = bh >= 1000 ? 3.f : 2.f;   // glyph pixel size
    float rowH = 12 * s;
    float pw = 300 * s, px = 20, py = 20;
    float ph = 14 * s + kRowCount * rowH + 14 * s;
    Rect(px, py, pw, ph, 0xC0101018);
    Rect(px, py, pw, 2, 0xFFE0A030);
    Text(px + 4 * s, py + 4 * s, "GOWFOV SETTINGS", s * 0.75f, 0xFFE0A030);

    static const char* kLensNames[kLensModeCount] = {"OFF", "AUTO", "PANINI", "STEREO", "FISHEYE"};
    char value[64], note[64];
    float fov = FovGet();
    for (int row = 0; row < kRowCount; ++row) {
        float y = py + 14 * s + row * rowH;
        if (row == g_row) Rect(px + 2, y - 2 * s, pw - 4, rowH, 0x60E0A030);
        const char* label = "";
        note[0] = 0;
        switch (row) {
        case kRowFov:
            label = "FOV";
            if (fov > 0) wsprintfA(value, "%d", (int)(fov + 0.5f));
            else lstrcpyA(value, "DEFAULT");
            break;
        case kRowAimFov:
            label = "AIM FOV";
            if (FovGetAim() > 0) wsprintfA(value, "%d", (int)(FovGetAim() + 0.5f));
            else lstrcpyA(value, "DEFAULT");
            lstrcpyA(note, "GAME 40");
            break;
        case kRowLens: {
            label = "LENS";
            lstrcpyA(value, kLensNames[LensGetMode()]);
            int lensFov = (int)(LensEffectiveFov() + 0.5f);
            if (lensFov > 0 && fov > 0 && lensFov + 1 < (int)(fov + 0.5f)) wsprintfA(note, "MAX %d", lensFov);
            break;
        }
        case kRowSupersample: {
            label = "SUPERSAMPLING";
            FormatFactor(value, g_ssPending > 0 ? g_ssPending : SupersampleSetting());
            UINT nw, nh, vw, vh;
            if (g_ssPending > 0) lstrcpyA(note, "APPLYING");
            else if (SupersampleGetSizes(&nw, &nh, &vw, &vh)) wsprintfA(note, "%uX%u", vw, vh);
            break;
        }
        case kRowShoulder:
            label = "START SHOULDER";
            lstrcpyA(value, ShoulderStartLeft() ? "LEFT" : "RIGHT");
            lstrcpyA(note, "TAB SWAPS");
            break;
        case kRowMouseTrap: {
            static const char* kTrapNames[kMouseTrapModeCount] = {"OFF", "GAMEPLAY", "ALWAYS"};
            static const char* kTrapNotes[kMouseTrapModeCount] = {"", "NOT IN MENUS", "MENUS TOO"};
            label = "MOUSE TRAP";
            lstrcpyA(value, kTrapNames[MouseTrapGetMode()]);
            lstrcpyA(note, kTrapNotes[MouseTrapGetMode()]);
            break;
        }
        case kRowShotFormat:
            label = "SCREENSHOT FORMAT";
            lstrcpyA(value, ScreenshotJpg() ? "JPG" : "PNG");
            wsprintfA(note, "KEY %s", ScreenshotKeyName());
            break;
        case kRowShotSize: {
            label = "SCREENSHOT SIZE";
            lstrcpyA(value, ScreenshotFull() ? "FULL" : "SCREEN");
            UINT nw, nh, vw, vh;
            if (ScreenshotFull() && SupersampleGetSizes(&nw, &nh, &vw, &vh)) wsprintfA(note, "%uX%u", vw, vh);
            break;
        }
        case kRowIntros:
            label = "INTRO VIDEOS";
            lstrcpyA(value, IntrosSkipped() ? "SKIP" : "PLAY");
            lstrcpyA(note, "NEXT START");
            break;
        }
        DWORD color = row == g_row ? 0xFFFFFFFF : 0xFFC8C8C8;
        Text(px + 4 * s, y, label, s, color);
        Text(px + 150 * s, y, value, s, color);
        if (note[0]) Text(px + 210 * s, y + 1.5f * s, note, s * 0.6f, 0xFF909090);
        if (row == kRowFov) {  // slider under the value
            float tx = px + 150 * s, tw = 54 * s, ty = y + 8 * s;
            float t = ((fov > 0 ? fov : 80) - kMinFov) / (float)(kMaxFov - kMinFov);
            if (t < 0) t = 0;
            if (t > 1) t = 1;
            Rect(tx, ty, tw, s * 0.7f, 0xFF505060);
            Rect(tx, ty, tw * t, s * 0.7f, 0xFFE0A030);
        }
    }
    Text(px + 4 * s, py + ph - 9 * s, "UP/DOWN SELECT  LEFT/RIGHT CHANGE  SHIFT X10  HOME DEFAULT  F8 CLOSE",
        s * 0.5f, 0xFF909090);
}

// ---- input --------------------------------------------------------------------------------

// True once per key press (edge-triggered), polled every frame.
static bool Pressed(int vk)
{
    bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
    bool edge = down && !g_keyPrev[vk];
    g_keyPrev[vk] = down;
    return edge;
}

// Keys are only read while the game window is in front.
static bool GameHasFocus()
{
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

// Steps the pending factor through kSupersampleSteps (applied later by Update).
static void ChangeSupersample(int dir, DWORD now)
{
    float cur = g_ssPending > 0 ? g_ssPending : SupersampleSetting();
    int idx = 0;
    for (int i = 0; i < kSupersampleStepCount; ++i)
        if (fabsf(kSupersampleSteps[i] - cur) < fabsf(kSupersampleSteps[idx] - cur)) idx = i;
    idx += dir;
    if (idx < 0) idx = 0;
    if (idx >= kSupersampleStepCount) idx = kSupersampleStepCount - 1;
    g_ssPending = kSupersampleSteps[idx];
    g_ssChangeTime = now;
}

// Menu input and settings changes, every frame.
static void Update()
{
    DWORD now = GetTickCount();
    // A supersampling change is applied once the selection has settled (even if closed).
    if (g_ssPending > 0 && now - g_ssChangeTime > kSupersampleApplyDelay) {
        if (fabsf(g_ssPending - SupersampleSetting()) > 0.01f) SupersampleSetFactor(g_ssPending);
        g_ssPending = -1.f;
    }
    if (g_testMode) { g_open = true; return; }
    if (!GameHasFocus()) return;
    if (Pressed(kToggleKey)) g_open = !g_open;
    if (!g_open) return;

    if (Pressed(VK_UP)) g_row = (g_row + kRowCount - 1) % kRowCount;
    if (Pressed(VK_DOWN)) g_row = (g_row + 1) % kRowCount;
    if (Pressed(VK_HOME)) {
        if (g_row == kRowAimFov) FovSetAim(0);
        else FovSet(0);
    }

    bool leftEdge = Pressed(VK_LEFT), rightEdge = Pressed(VK_RIGHT);
    int dir = rightEdge ? 1 : leftEdge ? -1 : 0;
    switch (g_row) {
    case kRowFov:
    case kRowAimFov: {
        // Held arrows repeat for the FOVs.
        static DWORD lastStep;
        bool left = (GetAsyncKeyState(VK_LEFT) & 0x8000) != 0;
        bool right = (GetAsyncKeyState(VK_RIGHT) & 0x8000) != 0;
        if ((left || right) && (dir != 0 || now - lastStep > 60)) {
            lastStep = now;
            bool aim = g_row == kRowAimFov;
            float cur = aim ? FovGetAim() : FovGet();
            float lo = aim ? kAimFovMin : (float)kMinFov, hi = aim ? kAimFovMax : (float)kMaxFov;
            float step = (GetAsyncKeyState(VK_SHIFT) & 0x8000) ? 10.f : 1.f;
            if (cur <= 0) cur = aim ? 40.f : 80.f;
            cur += right ? step : -step;
            if (cur < lo) cur = lo;
            if (cur > hi) cur = hi;
            if (aim) FovSetAim(cur);
            else FovSet(cur);
        }
        break;
    }
    case kRowLens:
        if (dir) LensSetMode((LensGetMode() + kLensModeCount + dir) % kLensModeCount);
        break;
    case kRowSupersample:
        if (dir) ChangeSupersample(dir, now);
        break;
    case kRowShoulder:
        if (dir) ShoulderSetStartLeft(!ShoulderStartLeft());
        break;
    case kRowMouseTrap:
        if (dir) MouseTrapSetMode((MouseTrapGetMode() + kMouseTrapModeCount + dir) % kMouseTrapModeCount);
        break;
    case kRowShotFormat:
        if (dir) ScreenshotSetJpg(!ScreenshotJpg());
        break;
    case kRowShotSize:
        if (dir) ScreenshotSetFull(!ScreenshotFull());
        break;
    case kRowIntros:
        if (dir) IntrosSetSkipped(!IntrosSkipped());
        break;
    }
}

// ---- D3D10 rendering ----------------------------------------------------------------------

struct Vtx { float x, y; DWORD rgba; };  // rgba in R8G8B8A8_UNORM byte order
static const int kMaxVerts = 8192 * 6;

static ID3D10Device* g_dev;
static ID3D10VertexShader* g_vs;
static ID3D10PixelShader* g_ps;
static ID3D10InputLayout* g_layout;
static ID3D10Buffer* g_vb;
static ID3D10BlendState* g_blend;
static ID3D10RasterizerState* g_raster;
static ID3D10DepthStencilState* g_depth;
static ID3D10StateBlock* g_stateBlock;
static ID3D10RenderTargetView* g_rtv;

template <class T> static void SafeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }

static void ReleaseAll()
{
    SafeRelease(g_rtv);
    SafeRelease(g_stateBlock);
    SafeRelease(g_depth);
    SafeRelease(g_raster);
    SafeRelease(g_blend);
    SafeRelease(g_vb);
    SafeRelease(g_layout);
    SafeRelease(g_ps);
    SafeRelease(g_vs);
    SafeRelease(g_dev);
}

// Shaders, vertex buffer and states for drawing the quads (once per device).
static bool CreateResources(ID3D10Device* dev)
{
    g_dev = dev;
    g_dev->AddRef();
    if (FAILED(dev->CreateVertexShader(g_vsMain, sizeof(g_vsMain), &g_vs))) return false;
    if (FAILED(dev->CreatePixelShader(g_psMain, sizeof(g_psMain), &g_ps))) return false;

    D3D10_INPUT_ELEMENT_DESC layout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D10_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 8, D3D10_INPUT_PER_VERTEX_DATA, 0},
    };
    if (FAILED(dev->CreateInputLayout(layout, 2, g_vsMain, sizeof(g_vsMain), &g_layout))) return false;

    D3D10_BUFFER_DESC bd = {};
    bd.ByteWidth = kMaxVerts * sizeof(Vtx);
    bd.Usage = D3D10_USAGE_DYNAMIC;
    bd.BindFlags = D3D10_BIND_VERTEX_BUFFER;
    bd.CPUAccessFlags = D3D10_CPU_ACCESS_WRITE;
    if (FAILED(dev->CreateBuffer(&bd, nullptr, &g_vb))) return false;

    D3D10_BLEND_DESC blend = {};
    blend.BlendEnable[0] = TRUE;
    blend.SrcBlend = D3D10_BLEND_SRC_ALPHA;
    blend.DestBlend = D3D10_BLEND_INV_SRC_ALPHA;
    blend.BlendOp = D3D10_BLEND_OP_ADD;
    blend.SrcBlendAlpha = D3D10_BLEND_ONE;
    blend.DestBlendAlpha = D3D10_BLEND_INV_SRC_ALPHA;
    blend.BlendOpAlpha = D3D10_BLEND_OP_ADD;
    blend.RenderTargetWriteMask[0] = D3D10_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(dev->CreateBlendState(&blend, &g_blend))) return false;

    D3D10_RASTERIZER_DESC rs = {};
    rs.FillMode = D3D10_FILL_SOLID;
    rs.CullMode = D3D10_CULL_NONE;
    rs.DepthClipEnable = TRUE;
    if (FAILED(dev->CreateRasterizerState(&rs, &g_raster))) return false;

    D3D10_DEPTH_STENCIL_DESC ds = {};
    ds.DepthEnable = FALSE;
    ds.StencilEnable = FALSE;
    if (FAILED(dev->CreateDepthStencilState(&ds, &g_depth))) return false;

    D3D10_STATE_BLOCK_MASK mask;
    D3D10StateBlockMaskEnableAll(&mask);
    if (FAILED(D3D10CreateStateBlock(dev, &mask, &g_stateBlock))) return false;
    return true;
}

// 0xAARRGGBB -> R8G8B8A8_UNORM byte order.
static DWORD ToRGBA(DWORD argb)
{
    return (argb & 0xFF00FF00) | ((argb >> 16) & 0xFF) | ((argb & 0xFF) << 16);
}

void OverlayLost()
{
    SafeRelease(g_rtv);
}

// Every frame from Present: input for the menu, Tab and F9, then the menu itself when open.
void OverlayFrame(IDXGISwapChain* swap)
{
    static bool init, failed;
    if (!init) {
        init = true;
        char v[8] = {};
        g_testMode = GetEnvironmentVariableA("GOWFOV_TEST", v, sizeof(v)) && v[0] == '1';
    }
    Update();
    ShoulderUpdate(!g_testMode && GameHasFocus());
    ScreenshotFrame(swap, !g_testMode && GameHasFocus());
    if (!g_open || failed) return;

    ID3D10Device* dev = nullptr;
    if (FAILED(swap->GetDevice(__uuidof(ID3D10Device), (void**)&dev)) || !dev) return;
    if (dev != g_dev) {
        ReleaseAll();
        if (!CreateResources(dev)) {
            Log("overlay: resource creation failed");
            ReleaseAll();
            failed = true;
            dev->Release();
            return;
        }
        Log("overlay: resources created");
    }
    dev->Release();  // g_dev holds its own reference

    // Draw into the real (native) back buffer, after any supersampling resolve.
    static UINT rtvW, rtvH;
    if (!g_rtv) {
        ID3D10Texture2D* bb = nullptr;
        if (FAILED(SwapGetRealBuffer(swap, &bb))) return;
        D3D10_TEXTURE2D_DESC bd;
        bb->GetDesc(&bd);
        rtvW = bd.Width;
        rtvH = bd.Height;
        HRESULT hr = g_dev->CreateRenderTargetView(bb, nullptr, &g_rtv);
        bb->Release();
        if (FAILED(hr)) return;
    }

    float bw = (float)rtvW, bh = (float)rtvH;
    BuildUI((int)bh);

    Vtx* v = nullptr;
    if (FAILED(g_vb->Map(D3D10_MAP_WRITE_DISCARD, 0, (void**)&v))) return;
    int n = 0;
    for (int i = 0; i < g_quadCount && n + 6 <= kMaxVerts; ++i) {
        const Quad& q = g_quads[i];
        float x0 = q.x / bw * 2 - 1, x1 = (q.x + q.w) / bw * 2 - 1;
        float y0 = 1 - q.y / bh * 2, y1 = 1 - (q.y + q.h) / bh * 2;
        DWORD c = ToRGBA(q.color);
        v[n++] = {x0, y0, c}; v[n++] = {x1, y0, c}; v[n++] = {x0, y1, c};
        v[n++] = {x1, y0, c}; v[n++] = {x1, y1, c}; v[n++] = {x0, y1, c};
    }
    g_vb->Unmap();

    g_stateBlock->Capture();  // the engine's pipeline state, restored after our draw

    D3D10_VIEWPORT vp = {0, 0, rtvW, rtvH, 0.f, 1.f};
    float blendFactor[4] = {0, 0, 0, 0};
    UINT stride = sizeof(Vtx), offset = 0;
    g_dev->OMSetRenderTargets(1, &g_rtv, nullptr);
    g_dev->OMSetBlendState(g_blend, blendFactor, 0xFFFFFFFF);
    g_dev->OMSetDepthStencilState(g_depth, 0);
    g_dev->RSSetViewports(1, &vp);
    g_dev->RSSetState(g_raster);
    g_dev->IASetInputLayout(g_layout);
    g_dev->IASetVertexBuffers(0, 1, &g_vb, &stride, &offset);
    g_dev->IASetPrimitiveTopology(D3D10_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_dev->VSSetShader(g_vs);
    g_dev->GSSetShader(nullptr);
    g_dev->PSSetShader(g_ps);
    g_dev->Draw(n, 0);

    g_stateBlock->Apply();
    g_stateBlock->ReleaseAllDeviceObjects();  // don't keep the engine's targets referenced
}
