// Lens correction (Phase 2).
//
// The engine renders each view on the render thread through FDrawSceneCommand /
// FDrawSceneCommandPP (vtables at 0x016FA564 / 0x016FA570; slot 1 is Execute). When Execute
// returns, the 3D scene incl. post-processing is in the back buffer and the HUD (canvas) has
// not been drawn yet: that is where the warp goes.
//
// The FOV hook asks LensRenderFov() which rectilinear FOV the engine must render so the chosen
// lens is fully covered; the warp then remaps that render into the lens projection.
//
// The hook only sees each camera mode's target FOV: the engine blends the FOV between modes
// (aiming, roadie run) and adds its own FOV effects after it. So the warp reads the FOV each
// frame was actually rendered with from the scene's projection matrix and picks the lens FOV
// that goes with it, rather than trusting the hook's value.
#include <windows.h>
#include <d3d10.h>
#include <math.h>
#include "gowfov.h"
#include "lens_vs.h"
#include "lens_ps.h"

static const float kPi = 3.14159265f;
static const float kMaxRenderFov = 160.f;  // single-view limit; centre sharpness collapses beyond this

// ---- lens math (mirrors lens.hlsl) ---------------------------------------------------------

struct V3 { float x, y, z; };

static V3 Norm(V3 v)
{
    float l = sqrtf(v.x * v.x + v.y * v.y + v.z * v.z);
    return {v.x / l, v.y / l, v.z / l};
}

static V3 RayRectilinear(float sx, float sy, float half)
{
    float t = tanf(half < 1.5690f ? half : 1.5690f);
    return Norm({sx * t, sy * t, 1});
}

static V3 RayPanini(float sx, float sy, float half)
{
    float edge = 2 * sinf(half) / (1 + cosf(half));
    float px = sx * edge, py = sy * edge;
    float k = px * px / 4;
    float clon = (1 - k) / (1 + k);
    float S = 1 + k;
    float lon = atan2f(px, S * clon);
    float lat = atan2f(py, S);
    return {cosf(lat) * sinf(lon), sinf(lat), cosf(lat) * cosf(lon)};
}

static V3 RayStereographic(float sx, float sy, float half)
{
    float t = tanf(half * 0.5f);
    float px = sx * t, py = sy * t;
    float r = sqrtf(px * px + py * py);
    if (r < 1e-6f) return {0, 0, 1};
    float th = 2 * atanf(r);
    return {px / r * sinf(th), py / r * sinf(th), cosf(th)};
}

// Fisheye (equidistant, r = theta): the angle from the centre grows evenly with the distance on
// screen. Up to 160 degrees across (one rendered view's limit); the corners then look past what
// was rendered and stay black like a fisheye lens' vignette.
static V3 RayFisheye(float sx, float sy, float half)
{
    float px = sx * half, py = sy * half;
    float r = sqrtf(px * px + py * py);
    if (r < 1e-6f) return {0, 0, 1};
    return {px / r * sinf(r), py / r * sinf(r), cosf(r)};
}

// Flex-fov style ramp: rectilinear up to 90 degrees, full Panini from 160.
static float AutoBlend(float fovDeg)
{
    float t = (fovDeg - 90.f) / 70.f;
    if (t <= 0) return 0;
    if (t >= 1) return 1;
    return 1 - (t - 1) * (t - 1);
}

static V3 Ray(int mode, float sx, float sy, float half, float blend)
{
    if (mode == kLensStereographic) return RayStereographic(sx, sy, half);
    if (mode == kLensFisheye) return RayFisheye(sx, sy, half);
    if (mode == kLensPanini) return RayPanini(sx, sy, half);
    V3 a = RayRectilinear(sx, sy, half), b = RayPanini(sx, sy, half);
    return Norm({a.x + (b.x - a.x) * blend, a.y + (b.y - a.y) * blend, a.z + (b.z - a.z) * blend});
}

// Smallest tan(render half-FOV) whose rectilinear image contains every ray the lens needs.
static float CoverageTan(int mode, float fovDeg, float aspect)
{
    float half = fovDeg * kPi / 360.f, blend = AutoBlend(fovDeg);
    float need = 0;
    const int n = 48;
    for (int i = 0; i <= n; ++i) {
        float t = -1 + 2.f * i / n;
        float pts[4][2] = {{t, 1 / aspect}, {t, -1 / aspect}, {1, t / aspect}, {-1, t / aspect}};
        for (auto& p : pts) {
            V3 r = Ray(mode, p[0], p[1], half, blend);
            if (r.z <= 1e-4f) return 1e9f;
            float x = fabsf(r.x / r.z), y = fabsf(r.y / r.z) * aspect;
            if (x > need) need = x;
            if (y > need) need = y;
        }
    }
    return need;
}

// CoverageTan for lens FOVs 0..179 in half-degree steps, built per (mode, aspect). The game
// thread maps lens -> render through it, the render thread maps render -> lens; both
// interpolate the same table, so the round trip is exact. Use with g_tableLock held.
static const float kTableStep = 0.5f;
static const int kTableSize = 359;
static float g_table[kTableSize];
static int g_tableMode = -1;
static float g_tableAspect;
static SRWLOCK g_tableLock = SRWLOCK_INIT;

static void EnsureTable(int mode, float aspect)
{
    if (mode == g_tableMode && aspect == g_tableAspect) return;
    float prev = 0;
    for (int i = 0; i < kTableSize; ++i) {
        float t = CoverageTan(mode, i * kTableStep, aspect);
        if (t < prev) t = prev;  // keep it monotonic so it can be inverted
        g_table[i] = prev = t;
    }
    if (mode == kLensFisheye) {
        // The fisheye's full coverage grows without bound before 160 (its corners look past 90
        // degrees). From where it reaches 90% of the render limit it is replaced by a straight
        // ramp to exactly the limit at 160: the table stays invertible (the warp maps each
        // rendered FOV back to a lens FOV, also mid-blend), the screen's edges stay covered, and
        // only the corners beyond the rendered view are left black.
        float maxTan = tanf(kMaxRenderFov * kPi / 360.f);
        int end = (int)(kMaxRenderFov / kTableStep);
        int i0 = 0;
        while (i0 + 1 < end && g_table[i0 + 1] <= 0.9f * maxTan) ++i0;
        for (int i = i0 + 1; i <= end; ++i) g_table[i] = g_table[i0] + (maxTan - g_table[i0]) * (i - i0) / (float)(end - i0);
        for (int i = end + 1; i < kTableSize; ++i) g_table[i] = 1e9f;
    }
    g_tableMode = mode;
    g_tableAspect = aspect;
}

// tan(render half-FOV) that covers lensFov.
static float TableCoverage(float lensFov)
{
    float f = lensFov / kTableStep;
    if (f <= 0) return g_table[0];
    int i = (int)f;
    if (i >= kTableSize - 1) return g_table[kTableSize - 1];
    float a = g_table[i], b = g_table[i + 1];
    if (b >= 1e9f) return b;
    return a + (b - a) * (f - i);
}

// Largest lens FOV that tan(render half-FOV) covers.
static float TableLens(float tanR)
{
    int lo = 0, hi = kTableSize - 1;
    if (g_table[hi] <= tanR) return hi * kTableStep;
    while (hi - lo > 1) {
        int mid = (lo + hi) / 2;
        if (g_table[mid] <= tanR) lo = mid;
        else hi = mid;
    }
    float a = g_table[lo], b = g_table[hi];
    float t = b > a && b < 1e9f ? (tanR - a) / (b - a) : 0.f;
    return (lo + t) * kTableStep;
}

// ---- shared state (written by the game thread, read by the render thread) ------------------

static volatile int g_mode = kLensAuto;
static volatile float g_aspect = 16.f / 9.f;
static volatile float g_lensFov;        // target lens FOV of the latest camera update (0 = no warp)
static volatile float g_renderTan;      // tan(render half-FOV) that went with it
static volatile LONG g_presentCount;
static volatile LONG g_fovFrame = -100; // present count when the FOV hook last ran

float LensRenderFov(float lensFov)
{
    int mode = g_mode;
    g_fovFrame = g_presentCount;
    if (mode == kLensOff || lensFov <= 90.f && mode == kLensAuto) {
        g_lensFov = 0;
        return lensFov;
    }
    float maxTan = tanf(kMaxRenderFov * kPi / 360.f);
    AcquireSRWLockExclusive(&g_tableLock);
    EnsureTable(mode, g_aspect);
    float tanR = TableCoverage(lensFov);
    if (tanR > maxTan) {
        // Shrink the lens FOV to what the engine can cover within the render limit.
        lensFov = TableLens(maxTan);
        tanR = maxTan;
    }
    ReleaseSRWLockExclusive(&g_tableLock);
    g_lensFov = lensFov;
    g_renderTan = tanR;
    return atanf(tanR) * 360.f / kPi;
}


int LensGetMode() { return g_mode; }

void LensSetMode(int mode)
{
    g_mode = mode;
    char path[MAX_PATH], val[8];
    wsprintfA(path, "%sgowfov.ini", ModDir());
    wsprintfA(val, "%d", mode);
    WritePrivateProfileStringA("gowfov", "lens", val, path);
}

float LensEffectiveFov() { return g_lensFov; }

bool LensGameplayActive() { return g_presentCount - g_fovFrame <= 5; }

// ---- warp rendering -----------------------------------------------------------------------

struct LensCB { float halfFov, tanHalfRender, aspect, blend; int mode; float pad[3]; };

static ID3D10Device* g_dev;
static ID3D10VertexShader* g_vs;
static ID3D10PixelShader* g_ps;
static ID3D10Buffer* g_cb;
static ID3D10SamplerState* g_sampler;
static ID3D10RasterizerState* g_raster;
static ID3D10DepthStencilState* g_depth;
static ID3D10BlendState* g_blend;
static ID3D10StateBlock* g_stateBlock;
static ID3D10Texture2D* g_copy;
static ID3D10ShaderResourceView* g_copySRV;
static ID3D10RenderTargetView* g_rtv;
static ID3D10Texture2D* g_rtvTarget;     // back buffer the RTV was made for (weak)
static D3D10_TEXTURE2D_DESC g_copyDesc;
static bool g_failed;

template <class T> static void SafeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }

static void ReleaseTargets()
{
    SafeRelease(g_rtv);
    SafeRelease(g_copySRV);
    SafeRelease(g_copy);
    g_rtvTarget = nullptr;
}

static void ReleaseAll()
{
    ReleaseTargets();
    SafeRelease(g_stateBlock);
    SafeRelease(g_blend);
    SafeRelease(g_depth);
    SafeRelease(g_raster);
    SafeRelease(g_sampler);
    SafeRelease(g_cb);
    SafeRelease(g_ps);
    SafeRelease(g_vs);
    SafeRelease(g_dev);
}

// Shaders, constant buffer and fixed states for the warp pass (once per device).
static bool CreateResources(ID3D10Device* dev)
{
    g_dev = dev;
    dev->AddRef();
    if (FAILED(dev->CreateVertexShader(g_lensVS, sizeof(g_lensVS), &g_vs))) return false;
    if (FAILED(dev->CreatePixelShader(g_lensPS, sizeof(g_lensPS), &g_ps))) return false;

    D3D10_BUFFER_DESC bd = {};
    bd.ByteWidth = sizeof(LensCB);
    bd.Usage = D3D10_USAGE_DYNAMIC;
    bd.BindFlags = D3D10_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D10_CPU_ACCESS_WRITE;
    if (FAILED(dev->CreateBuffer(&bd, nullptr, &g_cb))) return false;

    D3D10_SAMPLER_DESC sd = {};
    sd.Filter = D3D10_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D10_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D10_FLOAT32_MAX;
    if (FAILED(dev->CreateSamplerState(&sd, &g_sampler))) return false;

    D3D10_RASTERIZER_DESC rs = {};
    rs.FillMode = D3D10_FILL_SOLID;
    rs.CullMode = D3D10_CULL_NONE;
    rs.DepthClipEnable = TRUE;
    if (FAILED(dev->CreateRasterizerState(&rs, &g_raster))) return false;

    D3D10_DEPTH_STENCIL_DESC ds = {};
    if (FAILED(dev->CreateDepthStencilState(&ds, &g_depth))) return false;

    D3D10_BLEND_DESC bl = {};
    bl.SrcBlend = bl.SrcBlendAlpha = D3D10_BLEND_ONE;
    bl.DestBlend = bl.DestBlendAlpha = D3D10_BLEND_ZERO;
    bl.BlendOp = bl.BlendOpAlpha = D3D10_BLEND_OP_ADD;
    bl.RenderTargetWriteMask[0] = D3D10_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(dev->CreateBlendState(&bl, &g_blend))) return false;

    D3D10_STATE_BLOCK_MASK mask;
    D3D10StateBlockMaskEnableAll(&mask);
    return SUCCEEDED(D3D10CreateStateBlock(dev, &mask, &g_stateBlock));
}

// The warp reads a copy of the back buffer and writes the back buffer itself: (re)creates the
// copy and the render target view whenever the back buffer or its size changes.
static bool PrepareTargets(ID3D10Texture2D* bb)
{
    D3D10_TEXTURE2D_DESC d;
    bb->GetDesc(&d);
    if (g_rtv && g_rtvTarget == bb && g_copy && d.Width == g_copyDesc.Width &&
        d.Height == g_copyDesc.Height && d.Format == g_copyDesc.Format)
        return true;
    ReleaseTargets();

    D3D10_TEXTURE2D_DESC cd = d;
    cd.SampleDesc.Count = 1;
    cd.SampleDesc.Quality = 0;
    cd.Usage = D3D10_USAGE_DEFAULT;
    cd.BindFlags = D3D10_BIND_SHADER_RESOURCE;
    cd.CPUAccessFlags = 0;
    cd.MiscFlags = 0;
    cd.MipLevels = 1;
    cd.ArraySize = 1;
    if (FAILED(g_dev->CreateTexture2D(&cd, nullptr, &g_copy))) return false;
    if (FAILED(g_dev->CreateShaderResourceView(g_copy, nullptr, &g_copySRV))) return false;
    if (FAILED(g_dev->CreateRenderTargetView(bb, nullptr, &g_rtv))) return false;
    g_rtvTarget = bb;
    g_copyDesc = d;
    Log("lens: targets %ux%u fmt%d ms%u", d.Width, d.Height, d.Format, d.SampleDesc.Count);
    return true;
}

// Finds the projection matrix of the first view in an FSceneRenderer: an array (data, num, max)
// whose element contains a perspective matrix (m[0] > 0, m[5]/m[0] == aspect, m[11] == 1,
// m[15] == 0). The offsets are found once and cached. Returns tan(half horizontal FOV) or 0.
static int g_viewsOffset = -1, g_matrixOffset = -1;

static bool LooksLikeProjection(const float* m, float aspect)
{
    return m[0] > 0.01f && m[0] < 100.f && m[5] > 0.01f && fabsf(m[5] / m[0] - aspect) < 0.02f &&
           fabsf(m[11] - 1.f) < 1e-4f && fabsf(m[15]) < 1e-4f && fabsf(m[1]) < 1e-4f && fabsf(m[4]) < 1e-4f;
}

static float ReadMiss(int reason, const float* m)
{
    static int misses;
    if (misses++ < 10)
        Log("lens: projection read failed (%d) m0=%.4f m5=%.4f m11=%.4f m15=%.4f m1=%.4f m4=%.4f", reason,
            m ? m[0] : 0.f, m ? m[5] : 0.f, m ? m[11] : 0.f, m ? m[15] : 0.f, m ? m[1] : 0.f, m ? m[4] : 0.f);
    return 0;
}

static float SceneRenderTan(BYTE* renderer, float aspect)
{
    if (!renderer) return 0;
    if (g_viewsOffset < 0) {
        if (IsBadReadPtr(renderer, 0x400)) return 0;
        for (int off = 0; off < 0x400 && g_viewsOffset < 0; off += 4) {
            BYTE* data = *(BYTE**)(renderer + off);
            int num = *(int*)(renderer + off + 4);
            if (num < 1 || num > 4 || IsBadReadPtr(data, 0x800)) continue;
            for (int m = 0; m < 0x800 - 64; m += 4) {
                if (LooksLikeProjection((const float*)(data + m), aspect)) {
                    g_viewsOffset = off;
                    g_matrixOffset = m;
                    Log("lens: view projection found at renderer+0x%x -> view+0x%x", off, m);
                    break;
                }
            }
        }
        if (g_viewsOffset < 0) return 0;
    }
    if (IsBadReadPtr(renderer + g_viewsOffset, 4)) return ReadMiss(1, nullptr);
    BYTE* data = *(BYTE**)(renderer + g_viewsOffset);
    if (!data || IsBadReadPtr(data + g_matrixOffset, 64)) return ReadMiss(2, nullptr);
    const float* m = (const float*)(data + g_matrixOffset);
    return LooksLikeProjection(m, aspect) ? 1.f / m[0] : ReadMiss(3, m);
}

// actualTan: tan(half FOV) the scene was really rendered with (0 if unknown: then the FOV hook's
// values are used, which are only right once the engine's FOV blend has settled).
static void Warp(IDXGISwapChain* swap, float actualTan)
{
    int mode = g_mode;
    if (mode == kLensOff || g_failed) return;
    // Cutscenes and menus may render without going through the FOV hook; leave those alone.
    if (g_presentCount - g_fovFrame > 2) return;

    // A failed read reuses the previous frame's value: mid-blend that is far closer than the
    // hook's target.
    static float lastTan;
    static LONG lastFrame = -100;
    if (actualTan > 0) {
        lastTan = actualTan;
        lastFrame = g_presentCount;
    } else if (g_presentCount - lastFrame <= 2) {
        actualTan = lastTan;
    }

    float lensFov = g_lensFov, renderTan = g_renderTan;
    if (actualTan > 0) {
        AcquireSRWLockExclusive(&g_tableLock);
        EnsureTable(mode, g_aspect);
        lensFov = TableLens(actualTan);
        ReleaseSRWLockExclusive(&g_tableLock);
        renderTan = actualTan;
        if (mode == kLensAuto && lensFov <= 90.f) lensFov = 0;  // Auto is rectilinear up to 90
    }
    FovTraceAdd('w', lensFov, atanf(renderTan) * 360.f / kPi, actualTan > 0 ? atanf(actualTan) * 360.f / kPi : 0.f,
        (float)g_presentCount, nullptr);
    if (lensFov <= 0) return;

    ID3D10Device* dev = nullptr;
    if (FAILED(swap->GetDevice(__uuidof(ID3D10Device), (void**)&dev)) || !dev) return;
    if (dev != g_dev) {
        ReleaseAll();
        if (!CreateResources(dev)) {
            Log("lens: resource creation failed, warp disabled");
            ReleaseAll();
            g_failed = true;
            dev->Release();
            return;
        }
    }
    dev->Release();

    ID3D10Texture2D* bb = nullptr;
    if (FAILED(swap->GetBuffer(0, __uuidof(ID3D10Texture2D), (void**)&bb)) || !bb) return;
    if (!PrepareTargets(bb)) {
        Log("lens: target creation failed, warp disabled");
        g_failed = true;
        bb->Release();
        return;
    }

    // Copy the finished scene, then draw one fullscreen triangle that resamples it into the lens.
    if (g_copyDesc.SampleDesc.Count > 1) g_dev->ResolveSubresource(g_copy, 0, bb, 0, g_copyDesc.Format);
    else g_dev->CopyResource(g_copy, bb);
    bb->Release();

    LensCB* cb = nullptr;
    if (FAILED(g_cb->Map(D3D10_MAP_WRITE_DISCARD, 0, (void**)&cb))) return;
    cb->halfFov = lensFov * kPi / 360.f;
    cb->tanHalfRender = renderTan;
    cb->aspect = (float)g_copyDesc.Width / (float)g_copyDesc.Height;
    cb->blend = AutoBlend(lensFov);
    cb->mode = mode;
    g_cb->Unmap();

    g_stateBlock->Capture();  // the engine's pipeline state, restored after our draw
    D3D10_VIEWPORT vp = {0, 0, g_copyDesc.Width, g_copyDesc.Height, 0.f, 1.f};
    float blendFactor[4] = {0, 0, 0, 0};
    g_dev->OMSetRenderTargets(1, &g_rtv, nullptr);
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
    g_dev->PSSetShaderResources(0, 1, &g_copySRV);
    g_dev->PSSetSamplers(0, 1, &g_sampler);
    g_dev->Draw(3, 0);
    ID3D10ShaderResourceView* nullSRV = nullptr;
    g_dev->PSSetShaderResources(0, 1, &nullSRV);
    g_stateBlock->Apply();
    // The captured state holds references to the engine's bound targets (the back buffer view);
    // left alive they make the next ResizeBuffers fail.
    g_stateBlock->ReleaseAllDeviceObjects();
}

// ---- engine hooks -------------------------------------------------------------------------

typedef UINT (__thiscall* PFN_Execute)(void* self);

struct CommandHook {
    DWORD vtblSlot;  // address of the Execute slot
    DWORD expected;  // original Execute address, to verify the exe build
    PFN_Execute orig;
    const char* name;
};

static CommandHook g_cmds[2] = {
    {0x016FA568, 0x008D8C00, nullptr, "FDrawSceneCommand"},
    {0x016FA574, 0x008D8A80, nullptr, "FDrawSceneCommandPP"},
};

static IDXGISwapChain* g_swap;  // last swap chain seen in Present (not ref-counted)
static int g_scenesThisFrame;

// After each scene render command (render thread): the 3D frame is done, the HUD is not drawn yet.
static void SceneDone(float actualTan)
{
    ++g_scenesThisFrame;
    if (!g_swap) return;
    Warp(g_swap, actualTan);
    ScreenshotScene(g_swap);
}

// The command's scene renderer (+4) is deleted by Execute, so its view is read beforehand.
static UINT __fastcall Exec0(void* self, void*)
{
    float actualTan = SceneRenderTan(*(BYTE**)((BYTE*)self + 4), g_aspect);
    UINT r = g_cmds[0].orig(self);
    SceneDone(actualTan);
    return r;
}

static UINT __fastcall Exec1(void* self, void*)
{
    float actualTan = SceneRenderTan(*(BYTE**)((BYTE*)self + 4), g_aspect);
    UINT r = g_cmds[1].orig(self);
    SceneDone(actualTan);
    return r;
}

// Reads lens= from gowfov.ini (also used to reload it at runtime).
void LensInit()
{
    char path[MAX_PATH];
    wsprintfA(path, "%sgowfov.ini", ModDir());
    int mode = GetPrivateProfileIntA("gowfov", "lens", kLensAuto, path);
    g_mode = mode >= 0 && mode < kLensModeCount ? mode : kLensAuto;
}

// Replaces the Execute slots of both scene command vtables (checked against the originals).
void LensInstall()
{
    void* hooks[2] = {(void*)Exec0, (void*)Exec1};
    for (int i = 0; i < 2; ++i) {
        DWORD* slot = (DWORD*)g_cmds[i].vtblSlot;
        if (IsBadReadPtr(slot, 4) || *slot != g_cmds[i].expected) {
            Log("lens: %s slot mismatch, not hooked", g_cmds[i].name);
            continue;
        }
        DWORD old;
        VirtualProtect(slot, 4, PAGE_READWRITE, &old);
        g_cmds[i].orig = (PFN_Execute)*slot;
        *slot = (DWORD)hooks[i];
        VirtualProtect(slot, 4, old, &old);
        Log("lens: hooked %s", g_cmds[i].name);
    }
}

// Once per frame from Present: tracks the swap chain and aspect ratio, counts frames (the warp
// uses the count to tell gameplay frames from cutscenes), logs stats, handles gowfov_reload.req.
void LensOnPresent(IDXGISwapChain* swap)
{
    g_swap = swap;
    DXGI_SWAP_CHAIN_DESC sd;
    if (SUCCEEDED(swap->GetDesc(&sd)) && sd.BufferDesc.Height)
        g_aspect = (float)sd.BufferDesc.Width / (float)sd.BufferDesc.Height;
    if (g_presentCount % 600 == 0) {
        static LARGE_INTEGER last, freq;
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
        float fps = last.QuadPart ? 600.f * freq.QuadPart / (float)(now.QuadPart - last.QuadPart) : 0.f;
        last = now;
        Log("lens: scenes/frame=%d lensFov=%.1f renderFov=%.1f mode=%d fps=%.0f", g_scenesThisFrame,
            (float)g_lensFov, atanf(g_renderTan) * 360.f / kPi, (int)g_mode, fps);
    }
    g_scenesThisFrame = 0;
    InterlockedIncrement(&g_presentCount);

    // gowfov_reload.req: re-read gowfov.ini (lets tools change settings without key presses).
    if (g_presentCount % 15 == 0) {
        char req[MAX_PATH];
        wsprintfA(req, "%sgowfov_reload.req", ModDir());
        if (GetFileAttributesA(req) != INVALID_FILE_ATTRIBUTES) {
            DeleteFileA(req);
            FovInit();
            LensInit();
            MouseTrapInit();
            ScreenshotInit();
            Log("settings reloaded: lens=%d", (int)g_mode);
        }
    }
}

// Before ResizeBuffers: the copy and the back buffer view must not outlive the old buffers.
void LensOnResize()
{
    ReleaseTargets();
}
