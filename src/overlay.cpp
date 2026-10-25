// The F8 settings menu, drawn with D3D10 in the style of the game's own menus (and the
// launcher's): a darkened screen, a spaced heading, dark entry bars with a lit selection and red
// arrows around the value, and the selected entry's name large at the bottom right. The layout
// is in a 1280x720 design space, scaled to the back buffer. Text is Share Tech Mono
// (third_party/fonts, SIL OFL) rasterized with stb_truetype (third_party/imgui) at the sizes the
// menu uses, for the current scale.
//
// Tools: rowe_cmd.txt "menu open|close|up|down|left|right|left10|right10|home" drives the menu
// without the keyboard (OverlayCommand), and rowe_ui.req captures a frame with the menu drawn.
#include <windows.h>
#include <d3d10.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "rowe.h"
#include "overlay_vs.h"
#include "overlay_ps.h"

#define STBRP_STATIC
#define STB_RECT_PACK_IMPLEMENTATION
#include "imstb_rectpack.h"
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include "imstb_truetype.h"
#include "sharetechmono_ttf.h"

static const int kMinFov = 60;
static const int kMaxFov = (int)kFovMax;
static const int kToggleKey = VK_F8;

static bool g_open;
static bool g_testMode;  // ROWE_TEST=1: always open, no keyboard (used by the test harness)
static bool g_keyPrev[256];

// ---- font -----------------------------------------------------------------------------------

static const float kFontSizes[] = {12, 13, 14, 16, 17, 26, 40};  // design pixels
static const int kFontCount = sizeof(kFontSizes) / sizeof(kFontSizes[0]);
static const int kFirstChar = 32, kCharCount = 95;  // printable ASCII
static const int kAtlasSize = 2048;
static stbtt_packedchar g_chars[kFontCount][kCharCount];
static float g_ascent[kFontCount];  // top of the text to the baseline, atlas pixels
static unsigned char* g_atlas;      // kAtlasSize x kAtlasSize coverage
static float g_atlasScale;          // back buffer pixels per design pixel the atlas was made for
static float g_glyphScale = 1;      // drawn size / atlas size (1 unless the atlas had to shrink)
static bool g_atlasChanged;         // the texture must be made again

// Rasterizes every size for `scale`, smaller if it does not fit the atlas.
static bool BuildAtlas(float scale)
{
    if (!g_atlas) g_atlas = (unsigned char*)malloc(kAtlasSize * kAtlasSize);
    stbtt_fontinfo info;
    if (!g_atlas || !stbtt_InitFont(&info, g_shareTechMono, 0)) return false;
    int ascent, descent, gap;
    stbtt_GetFontVMetrics(&info, &ascent, &descent, &gap);
    for (float s = scale; s > 0.2f; s *= 0.8f) {
        stbtt_pack_context pc;
        if (!stbtt_PackBegin(&pc, g_atlas, kAtlasSize, kAtlasSize, 0, 1, nullptr)) return false;
        bool ok = true;
        for (int i = 0; i < kFontCount && ok; ++i) {
            float px = kFontSizes[i] * s;
            ok = stbtt_PackFontRange(&pc, g_shareTechMono, 0, px, kFirstChar, kCharCount, g_chars[i]) != 0;
            g_ascent[i] = ascent * stbtt_ScaleForPixelHeight(&info, px);
        }
        stbtt_PackEnd(&pc);
        if (ok) {
            g_atlasScale = scale;
            g_glyphScale = scale / s;
            g_atlasChanged = true;
            Log("overlay: font atlas for scale %.2f%s", scale, s < scale ? " (reduced)" : "");
            return true;
        }
    }
    return false;
}

static int FontIndex(float size)
{
    int best = 0;
    for (int i = 1; i < kFontCount; ++i)
        if (fabsf(kFontSizes[i] - size) < fabsf(kFontSizes[best] - size)) best = i;
    return best;
}

// ---- UI geometry ----------------------------------------------------------------------------

// Back buffer pixels; colors 0xAARRGGBB at the top-left, top-right, bottom-left and bottom-right
// corners; u0 < 0 for a flat quad, otherwise a glyph's place in the atlas.
struct Quad { float x, y, w, h; float u0, v0, u1, v1; DWORD c[4]; };
static Quad g_quads[8192];
static int g_quadCount;

static float g_u = 1;          // back buffer pixels per design pixel
static float g_ox, g_oy;       // the design area's top-left corner in the back buffer
static float g_line = 1;       // line thickness, back buffer pixels

static DWORD Argb(float r, float g, float b, float a)
{
    auto c = [](float v) { return (DWORD)(v <= 0 ? 0 : v >= 1 ? 255 : v * 255 + 0.5f); };
    return c(a) << 24 | c(r) << 16 | c(g) << 8 | c(b);
}

static void AddQuad(float x, float y, float w, float h, DWORD tl, DWORD tr, DWORD bl, DWORD br)
{
    if (g_quadCount >= (int)(sizeof(g_quads) / sizeof(g_quads[0]))) return;
    g_quads[g_quadCount++] = {x, y, w, h, -1, 0, 0, 0, {tl, tr, bl, br}};
}

// A filled rectangle in design units, snapped to whole pixels.
static void Rect(float x, float y, float w, float h, DWORD c)
{
    float x0 = roundf(g_ox + x * g_u), y0 = roundf(g_oy + y * g_u);
    float x1 = roundf(g_ox + (x + w) * g_u), y1 = roundf(g_oy + (y + h) * g_u);
    AddQuad(x0, y0, x1 - x0, y1 - y0, c, c, c, c);
}

static void HLine(float x0, float x1, float y, DWORD c)
{
    float px = roundf(g_ox + x0 * g_u), py = roundf(g_oy + y * g_u);
    AddQuad(px, py, roundf(g_ox + x1 * g_u) - px, g_line, c, c, c, c);
}

static void VLine(float x, float y0, float y1, DWORD c)
{
    float px = roundf(g_ox + x * g_u), py = roundf(g_oy + y0 * g_u);
    AddQuad(px, py, g_line, roundf(g_oy + y1 * g_u) - py, c, c, c, c);
}

static void Frame(float x, float y, float w, float h, DWORD c)
{
    HLine(x, x + w, y, c);
    HLine(x, x + w, y + h, c);
    VLine(x, y, y + h, c);
    VLine(x + w, y, y + h + g_line / g_u, c);
}

// Width of `s` in design units at `size` with `spacing` between letters.
static float TextWidth(const char* s, float size, float spacing)
{
    const stbtt_packedchar* chars = g_chars[FontIndex(size)];
    float w = 0;
    int n = 0;
    for (; *s; ++s, ++n) {
        int c = (unsigned char)*s - kFirstChar;
        if (c >= 0 && c < kCharCount) w += chars[c].xadvance * g_glyphScale;
    }
    return n ? w / g_u + spacing * (n - 1) : 0;
}

enum { kLeft, kRight };

// Draws `s` with its top-left (or top-right) corner at design position (x, y); shadow adds a
// dark copy underneath, for text drawn straight over the game.
static void Text(float x, float y, const char* s, float size, DWORD color, float spacing = 0, int align = kLeft,
                 bool shadow = false)
{
    if (shadow) Text(x + 1.5f, y + 1.5f, s, size, (color >> 25) << 24, spacing, align);  // black, half as opaque
    int f = FontIndex(size);
    const stbtt_packedchar* chars = g_chars[f];
    if (align == kRight) x -= TextWidth(s, size, spacing);
    float pen = g_ox + x * g_u;
    float base = roundf(g_oy + y * g_u + g_ascent[f] * g_glyphScale);
    const float k = g_glyphScale, inv = 1.f / kAtlasSize;
    for (; *s; ++s) {
        int c = (unsigned char)*s - kFirstChar;
        if (c < 0 || c >= kCharCount) continue;
        const stbtt_packedchar& g = chars[c];
        if (g.x1 > g.x0 && g_quadCount < (int)(sizeof(g_quads) / sizeof(g_quads[0]))) {
            g_quads[g_quadCount++] = {roundf(pen) + g.xoff * k, base + g.yoff * k, (g.xoff2 - g.xoff) * k,
                                      (g.yoff2 - g.yoff) * k, g.x0 * inv, g.y0 * inv, g.x1 * inv, g.y1 * inv,
                                      {color, color, color, color}};
        }
        pen += g.xadvance * k + spacing * g_u;
    }
}

// ---- settings menu --------------------------------------------------------------------------

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
    else if ((int)(f * 100 + 0.5f) % 100) wsprintfA(out, "%d.%02dX", (int)f, (int)(f * 100 + 0.5f) % 100);
    else wsprintfA(out, "%dX", (int)(f + 0.5f));
}

// One entry: the label and value in the list, the title and description shown large while it
// is selected (the same texts as the launcher's).
struct Row { const char* label; char value[32]; const char* title; char desc[160]; };

static void FillRow(int row, Row& r)
{
    static const char* kLensNames[kLensModeCount] = {"OFF", "AUTO", "PANINI", "STEREOGRAPHIC", "FISHEYE"};
    float fov = FovGet();
    r.desc[0] = 0;
    switch (row) {
    case kRowFov:
        r.label = "FIELD OF VIEW";
        r.title = "FOV";
        if (fov > 0) wsprintfA(r.value, "%d", (int)(fov + 0.5f));
        else lstrcpyA(r.value, "DEFAULT");
        lstrcpyA(r.desc, "The normal camera's FOV, 60 to 159 (the game uses 80). Shift: steps of 10. Home: the game's.");
        break;
    case kRowAimFov:
        r.label = "AIM FOV";
        r.title = "AIM FOV";
        if (FovGetAim() > 0) wsprintfA(r.value, "%d", (int)(FovGetAim() + 0.5f));
        else lstrcpyA(r.value, "DEFAULT");
        lstrcpyA(r.desc, "How far aiming zooms in, 30 to 120 (the game uses 40). Sniper scopes keep their zoom.");
        break;
    case kRowLens: {
        r.label = "LENS";
        r.title = "LENS";
        lstrcpyA(r.value, kLensNames[LensGetMode()]);
        int lensFov = (int)(LensEffectiveFov() + 0.5f);
        if (lensFov > 0 && fov > 0 && lensFov + 1 < (int)(fov + 0.5f))
            wsprintfA(r.desc, "Wide FOVs without stretched edges. This lens reaches %d degrees.", lensFov);
        else lstrcpyA(r.desc, "Wide FOVs without stretched edges. Auto: straight up to 90, Panini by 160.");
        break;
    }
    case kRowSupersample: {
        r.label = "SUPERSAMPLING";
        r.title = "SUPERSAMPLING";
        float f = g_ssPending > 0 ? g_ssPending : SupersampleSetting();
        FormatFactor(r.value, f);
        UINT nw, nh, vw, vh;
        if (g_ssPending > 0) lstrcpyA(r.desc, "Applying...");
        else if (SupersampleGetSizes(&nw, &nh, &vw, &vh))
            wsprintfA(r.desc, "Renders %u x %u and averages it down to %u x %u.", vw, vh, nw, nh);
        else if (f > 1.01f) lstrcpyA(r.desc, "Fullscreen only: takes effect when the game runs fullscreen.");
        else lstrcpyA(r.desc, "Renders more pixels and averages them down. Fullscreen only.");
        break;
    }
    case kRowShoulder:
        r.label = "START SHOULDER";
        r.title = "SHOULDER";
        lstrcpyA(r.value, ShoulderStartLeft() ? "LEFT" : "RIGHT");
        lstrcpyA(r.desc, "The shoulder the camera starts on. Tab swaps it in game.");
        break;
    case kRowMouseTrap: {
        static const char* kTrapNames[kMouseTrapModeCount] = {"OFF", "GAMEPLAY", "ALWAYS"};
        r.label = "MOUSE TRAP";
        r.title = "MOUSE TRAP";
        lstrcpyA(r.value, kTrapNames[MouseTrapGetMode()]);
        lstrcpyA(r.desc, "Keeps the cursor on the game's monitor: in gameplay, or in menus too.");
        break;
    }
    case kRowShotFormat:
        r.label = "SCREENSHOT FORMAT";
        r.title = "SCREENSHOTS";
        lstrcpyA(r.value, ScreenshotJpg() ? "JPEG" : "PNG");
        wsprintfA(r.desc, "JPEG is small and quick to write, PNG is lossless. Screenshot key: %s.", ScreenshotKeyName());
        break;
    case kRowShotSize: {
        r.label = "SCREENSHOT SIZE";
        r.title = "SCREENSHOTS";
        lstrcpyA(r.value, ScreenshotFull() ? "FULL" : "SCREEN");
        UINT nw, nh, vw, vh;
        if (SupersampleGetSizes(&nw, &nh, &vw, &vh))
            wsprintfA(r.desc, "Full: the supersampled frame (%u x %u). Screen: your screen's size.", vw, vh);
        else lstrcpyA(r.desc, "Full: the supersampled size the game renders at. Screen: your screen's.");
        break;
    }
    case kRowIntros:
        r.label = "INTRO VIDEOS";
        r.title = "INTRO VIDEOS";
        lstrcpyA(r.value, IntrosSkipped() ? "SKIP" : "PLAY");
        lstrcpyA(r.desc, "The logo videos at startup. Takes effect at the next start.");
        break;
    }
}

// A key in a box, then what it does (as the hints at the bottom of the game's menus).
static void KeyHint(float& x, float y, const char* key, const char* what)
{
    float kw = TextWidth(key, 13, 0) + 10;
    Rect(x, y - 1, kw, 18, Argb(0, 0, 0, 0.35f));
    Frame(x, y - 1, kw, 18, Argb(1, 1, 1, 0.7f));
    Text(x + 5, y, key, 13, Argb(1, 1, 1, 0.9f));
    Text(x + kw + 6, y + 1, what, 14, Argb(1, 1, 1, 0.8f), 2.5f, kLeft, true);
    x += kw + 6 + TextWidth(what, 14, 2.5f) + 22;
}

// Lays out the menu as quads for a bw x bh back buffer.
static void BuildUI(float bw, float bh)
{
    g_quadCount = 0;
    g_u = fminf(bw / 1280.f, bh / 720.f);
    g_ox = roundf((bw - 1280 * g_u) / 2);
    g_oy = roundf((bh - 720 * g_u) / 2);
    g_line = fmaxf(1.f, floorf(g_u + 0.25f));

    // The game stays visible on the right, darker towards the left where the entries are.
    AddQuad(0, 0, bw, bh, Argb(0, 0, 0, 0.78f), Argb(0, 0, 0, 0.2f), Argb(0, 0, 0, 0.85f), Argb(0, 0, 0, 0.45f));

    Text(105, 60, "ROWE SETTINGS", 26, Argb(1, 1, 1, 0.95f), 6, kLeft, true);
    Text(107, 96, "ROWE FOR GEARS OF WAR (2007)", 12, Argb(0.85f, 0.25f, 0.2f, 0.95f), 4);

    const float x0 = 105, x1 = 745, y0 = 140, rowH = 30, gap = 4;
    // The bracket frame around the entries.
    float fy0 = y0 - 14, fy1 = y0 + kRowCount * (rowH + gap) + 10;
    DWORD frame = Argb(1, 1, 1, 0.3f);
    HLine(x0 - 18, x1 + 18, fy0, frame);
    HLine(x0 - 18, x1 + 18, fy1, frame);
    VLine(x0 - 18, fy0, fy0 + 24, frame);
    VLine(x1 + 18, fy1 - 24, fy1, frame);
    VLine(x1 + 18, fy0, fy0 + 8, frame);
    VLine(x0 - 18, fy1 - 8, fy1, frame);

    Row sel = {};
    for (int i = 0; i < kRowCount; ++i) {
        Row r = {};
        FillRow(i, r);
        bool hot = i == g_row;
        if (hot) sel = r;
        float y = y0 + i * (rowH + gap);
        // Darker than the launcher's: here the bars lie over the game, not a dimmed picture.
        Rect(x0, y, x1 - x0, rowH, Argb(0, 0, 0, hot ? 0.6f : 0.7f));
        if (hot) Rect(x0, y, x1 - x0, rowH, Argb(0.85f, 0.85f, 0.85f, 0.32f));
        HLine(x0, x1, y, Argb(1, 1, 1, hot ? 0.25f : 0.06f));
        DWORD text = Argb(1, 1, 1, hot ? 1.f : 0.72f);
        Text(x0 + 14, y + 6, r.label, 16, text, 3);
        Text(x1 - (hot ? 34 : 14), y + 6, r.value, 16, text, 2.5f, kRight);
        if (hot) {
            float vw = TextWidth(r.value, 16, 2.5f);
            DWORD red = Argb(0.9f, 0.3f, 0.25f, 1);
            Text(x1 - 34 - vw - 22, y + 5, "<", 17, red);
            Text(x1 - 22, y + 5, ">", 17, red);
        }
    }

    // The selected entry, large at the bottom right, as the game shows its menu entries.
    Text(1180, 548, sel.title, 40, Argb(1, 1, 1, 0.95f), 8, kRight, true);
    Text(1180, 600, sel.desc, 14, Argb(1, 1, 1, 0.85f), 1, kRight, true);

    float hx = 105;
    KeyHint(hx, 670, "^ v", "SELECT");
    KeyHint(hx, 670, "< >", "CHANGE");
    if (g_row == kRowFov || g_row == kRowAimFov) {
        KeyHint(hx, 670, "Shift", "BY 10");
        KeyHint(hx, 670, "Home", "DEFAULT");
    }
    KeyHint(hx, 670, "F8", "CLOSE");
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

// Tool commands, queued by the game thread (OverlayCommand) and run by Update, one per frame.
enum { kCmdNone, kCmdOpen, kCmdClose, kCmdUp, kCmdDown, kCmdLeft, kCmdRight, kCmdLeft10, kCmdRight10, kCmdHome };
static const char* kCmdNames[] = {"", "open", "close", "up", "down", "left", "right", "left10", "right10", "home"};
static SRWLOCK g_cmdLock = SRWLOCK_INIT;
static int g_cmdQueue[32], g_cmdCount;

bool OverlayCommand(const char* name)
{
    for (int i = 1; i < (int)(sizeof(kCmdNames) / sizeof(kCmdNames[0])); ++i) {
        if (lstrcmpiA(name, kCmdNames[i]) != 0) continue;
        AcquireSRWLockExclusive(&g_cmdLock);
        if (g_cmdCount < 32) g_cmdQueue[g_cmdCount++] = i;
        ReleaseSRWLockExclusive(&g_cmdLock);
        return true;
    }
    return false;
}

static int NextCommand()
{
    AcquireSRWLockExclusive(&g_cmdLock);
    int cmd = kCmdNone;
    if (g_cmdCount) {
        cmd = g_cmdQueue[0];
        memmove(g_cmdQueue, g_cmdQueue + 1, --g_cmdCount * sizeof(int));
    }
    ReleaseSRWLockExclusive(&g_cmdLock);
    return cmd;
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
    int cmd = NextCommand();
    if (cmd == kCmdOpen) g_open = true;
    if (cmd == kCmdClose) g_open = false;
    bool keys = !g_testMode && GameHasFocus();
    if (g_testMode) g_open = true;
    if (keys && Pressed(kToggleKey)) g_open = !g_open;
    if (!g_open) return;

    if ((keys && Pressed(VK_UP)) || cmd == kCmdUp) g_row = (g_row + kRowCount - 1) % kRowCount;
    if ((keys && Pressed(VK_DOWN)) || cmd == kCmdDown) g_row = (g_row + 1) % kRowCount;
    if ((keys && Pressed(VK_HOME)) || cmd == kCmdHome) {
        if (g_row == kRowAimFov) FovSetAim(0);
        else if (g_row == kRowFov) FovSet(0);
    }

    bool leftEdge = keys && Pressed(VK_LEFT), rightEdge = keys && Pressed(VK_RIGHT);
    bool big = cmd == kCmdLeft10 || cmd == kCmdRight10 || (keys && (GetAsyncKeyState(VK_SHIFT) & 0x8000));
    int cmdDir = cmd == kCmdLeft || cmd == kCmdLeft10 ? -1 : cmd == kCmdRight || cmd == kCmdRight10 ? 1 : 0;
    int dir = rightEdge ? 1 : leftEdge ? -1 : cmdDir;
    switch (g_row) {
    case kRowFov:
    case kRowAimFov: {
        // Held arrows repeat for the FOVs.
        static DWORD lastStep;
        bool left = keys && (GetAsyncKeyState(VK_LEFT) & 0x8000) != 0;
        bool right = keys && (GetAsyncKeyState(VK_RIGHT) & 0x8000) != 0;
        int step = cmdDir;
        if ((left || right) && (leftEdge || rightEdge || now - lastStep > 60)) step = right ? 1 : -1;
        if (step) {
            lastStep = now;
            bool aim = g_row == kRowAimFov;
            float cur = aim ? FovGetAim() : FovGet();
            float lo = aim ? kAimFovMin : (float)kMinFov, hi = aim ? kAimFovMax : (float)kMaxFov;
            if (cur <= 0) cur = aim ? 40.f : 80.f;
            cur += step * (big ? 10.f : 1.f);
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

struct Vtx { float x, y, u, v; DWORD rgba; };  // rgba in R8G8B8A8_UNORM byte order
static const int kMaxVerts = 8192 * 6;

static ID3D10Device* g_dev;
static ID3D10VertexShader* g_vs;
static ID3D10PixelShader* g_ps;
static ID3D10InputLayout* g_layout;
static ID3D10Buffer* g_vb;
static ID3D10BlendState* g_blend;
static ID3D10RasterizerState* g_raster;
static ID3D10DepthStencilState* g_depth;
static ID3D10SamplerState* g_sampler;
static ID3D10StateBlock* g_stateBlock;
static ID3D10RenderTargetView* g_rtv;
static ID3D10ShaderResourceView* g_fontView;

template <class T> static void SafeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }

static void ReleaseAll()
{
    SafeRelease(g_fontView);
    SafeRelease(g_rtv);
    SafeRelease(g_stateBlock);
    SafeRelease(g_sampler);
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
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D10_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 16, D3D10_INPUT_PER_VERTEX_DATA, 0},
    };
    if (FAILED(dev->CreateInputLayout(layout, 3, g_vsMain, sizeof(g_vsMain), &g_layout))) return false;

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

    D3D10_SAMPLER_DESC sd = {};
    sd.Filter = D3D10_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D10_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D10_FLOAT32_MAX;
    if (FAILED(dev->CreateSamplerState(&sd, &g_sampler))) return false;

    D3D10_STATE_BLOCK_MASK mask;
    D3D10StateBlockMaskEnableAll(&mask);
    if (FAILED(D3D10CreateStateBlock(dev, &mask, &g_stateBlock))) return false;
    g_atlasChanged = true;  // the font texture is made for this device
    return true;
}

// The atlas as a texture (after BuildAtlas, or for a new device).
static bool CreateFontTexture()
{
    SafeRelease(g_fontView);
    D3D10_TEXTURE2D_DESC td = {};
    td.Width = td.Height = kAtlasSize;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D10_USAGE_IMMUTABLE;
    td.BindFlags = D3D10_BIND_SHADER_RESOURCE;
    D3D10_SUBRESOURCE_DATA init = {g_atlas, kAtlasSize, 0};
    ID3D10Texture2D* tex = nullptr;
    if (FAILED(g_dev->CreateTexture2D(&td, &init, &tex))) return false;
    HRESULT hr = g_dev->CreateShaderResourceView(tex, nullptr, &g_fontView);
    tex->Release();
    g_atlasChanged = false;
    return SUCCEEDED(hr);
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

// Draws the open menu into the real (native) back buffer. False if it cannot be drawn at all.
static bool DrawMenu(IDXGISwapChain* swap)
{
    ID3D10Device* dev = nullptr;
    if (FAILED(swap->GetDevice(__uuidof(ID3D10Device), (void**)&dev)) || !dev) return true;
    if (dev != g_dev) {
        ReleaseAll();
        bool ok = CreateResources(dev);
        dev->Release();  // g_dev holds its own reference
        if (!ok) {
            Log("overlay: resource creation failed");
            ReleaseAll();
            return false;
        }
        Log("overlay: resources created");
    } else {
        dev->Release();
    }

    static UINT rtvW, rtvH;
    if (!g_rtv) {
        ID3D10Texture2D* bb = nullptr;
        if (FAILED(SwapGetRealBuffer(swap, &bb))) return true;
        D3D10_TEXTURE2D_DESC bd;
        bb->GetDesc(&bd);
        rtvW = bd.Width;
        rtvH = bd.Height;
        HRESULT hr = g_dev->CreateRenderTargetView(bb, nullptr, &g_rtv);
        bb->Release();
        if (FAILED(hr)) return true;
    }

    float bw = (float)rtvW, bh = (float)rtvH;
    float scale = fminf(bw / 1280.f, bh / 720.f);
    if (fabsf(scale - g_atlasScale) > 0.001f && !BuildAtlas(scale)) {
        Log("overlay: font atlas failed");
        return false;
    }
    if (g_atlasChanged && !CreateFontTexture()) {
        Log("overlay: font texture failed");
        return false;
    }
    BuildUI(bw, bh);

    Vtx* v = nullptr;
    if (FAILED(g_vb->Map(D3D10_MAP_WRITE_DISCARD, 0, (void**)&v))) return true;
    int n = 0;
    for (int i = 0; i < g_quadCount && n + 6 <= kMaxVerts; ++i) {
        const Quad& q = g_quads[i];
        float x0 = q.x / bw * 2 - 1, x1 = (q.x + q.w) / bw * 2 - 1;
        float y0 = 1 - q.y / bh * 2, y1 = 1 - (q.y + q.h) / bh * 2;
        Vtx tl = {x0, y0, q.u0, q.v0, ToRGBA(q.c[0])}, tr = {x1, y0, q.u1, q.v0, ToRGBA(q.c[1])};
        Vtx bl = {x0, y1, q.u0, q.v1, ToRGBA(q.c[2])}, br = {x1, y1, q.u1, q.v1, ToRGBA(q.c[3])};
        v[n++] = tl; v[n++] = tr; v[n++] = bl;
        v[n++] = tr; v[n++] = br; v[n++] = bl;
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
    g_dev->PSSetShaderResources(0, 1, &g_fontView);
    g_dev->PSSetSamplers(0, 1, &g_sampler);
    g_dev->Draw(n, 0);

    g_stateBlock->Apply();
    g_stateBlock->ReleaseAllDeviceObjects();  // don't keep the engine's targets referenced
    return true;
}

// Every frame from Present: input for the menu, Tab and F9, then the menu itself when open.
void OverlayFrame(IDXGISwapChain* swap)
{
    static bool init, failed;
    if (!init) {
        init = true;
        char v[8] = {};
        g_testMode = GetEnvironmentVariableA("ROWE_TEST", v, sizeof(v)) && v[0] == '1';
        if (g_testMode && GetEnvironmentVariableA("ROWE_TEST_ROW", v, sizeof(v))) g_row = atoi(v) % kRowCount;
    }
    Update();
    bool focused = !g_testMode && GameHasFocus();
    ShoulderUpdate(focused);
    ScreenshotFrame(swap, focused);
    if (g_open && !failed) failed = !DrawMenu(swap);
    ScreenshotWithMenu(swap);
}
