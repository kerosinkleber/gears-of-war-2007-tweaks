// The settings menu (F8, rowe.ini menukey), drawn with D3D10 in the style of the game's own menus
// (and the launcher's): a darkened screen, a spaced heading, dark entry bars with a lit selection
// and red arrows around the value, and the selected entry's name large at the bottom right. The
// layout is in a 1280x720 design space, scaled to the back buffer. Text is Share Tech Mono
// (third_party/fonts, SIL OFL) rasterized with stb_truetype (third_party/imgui) at the sizes the
// menu uses, for the current scale.
//
// Keyboard: arrows, Shift, Home, the menu key. Mouse: hover selects, a click on the value's left
// or right half changes it (elsewhere on the entry: the next value), the wheel changes it, the key
// hints can be clicked. The mouse comes from the game window's messages where Windows has a
// cursor (menus) and from the game's DirectInput mouse in gameplay (dinput.cpp); while the menu is
// open the game gets neither, nor the keys the menu uses, and the menu draws its own cursor.
//
// The stats window (rowe.ini stats=1) shows the lens, supersampling and frame rate in a corner,
// also with the menu closed, and is in the screenshot key's shots. Drag it while the menu is open.
//
// Tools: rowe_cmd.txt "menu open|close|up|down|left|right|left10|right10|home" drives the menu
// without the keyboard (OverlayCommand), and rowe_ui.req captures a frame with the menu drawn.
#include <windows.h>
#include <d3d10.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rowe.h"
#include "keynames.h"
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

static volatile bool g_open;
static bool g_testMode;  // ROWE_TEST=1: always open, no keyboard (used by the test harness)
static bool g_keyPrev[256];
static UINT g_menuKey = VK_F8;
static char g_menuKeyName[32] = "F8";

static void IniPath(char* out) { wsprintfA(out, "%srowe.ini", ModDir()); }

// ---- font -----------------------------------------------------------------------------------

static const float kFontSizes[] = {12, 13, 14, 16, 17, 26, 40};  // design pixels
static const int kFontCount = sizeof(kFontSizes) / sizeof(kFontSizes[0]);
// Printable ASCII, then Latin-1 0xA0-0xFF (for the degree and multiplication signs).
static const int kAsciiCount = 95, kLatinFirst = 0xA0, kLatinCount = 96, kGlyphCount = kAsciiCount + kLatinCount;
static const int kAtlasSize = 2048;
static stbtt_packedchar g_chars[kFontCount][kGlyphCount];
static float g_ascent[kFontCount];  // top of the text to the baseline, atlas pixels
static unsigned char* g_atlas;      // kAtlasSize x kAtlasSize coverage
static float g_atlasScale;          // back buffer pixels per design pixel the atlas was made for
static float g_bakedScale = 1;      // ... and the scale it was rasterized at (smaller if it did not fit)
static bool g_atlasChanged;         // the texture must be made again

static int GlyphIndex(unsigned char c)
{
    if (c >= 32 && c < 32 + kAsciiCount) return c - 32;
    if (c >= kLatinFirst) return kAsciiCount + c - kLatinFirst;
    return -1;
}

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
            ok = stbtt_PackFontRange(&pc, g_shareTechMono, 0, px, 32, kAsciiCount, g_chars[i]) != 0 &&
                 stbtt_PackFontRange(&pc, g_shareTechMono, 0, px, kLatinFirst, kLatinCount, g_chars[i] + kAsciiCount) != 0;
            g_ascent[i] = ascent * stbtt_ScaleForPixelHeight(&info, px);
        }
        stbtt_PackEnd(&pc);
        if (ok) {
            g_atlasScale = scale;
            g_bakedScale = s;
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
static float g_ox, g_oy;       // the design origin in the back buffer
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
        int c = GlyphIndex((unsigned char)*s);
        if (c >= 0) w += chars[c].xadvance / g_bakedScale;
    }
    return n ? w + spacing * (n - 1) : 0;
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
    const float k = g_u / g_bakedScale, inv = 1.f / kAtlasSize;  // drawn pixels per atlas pixel
    float pen = g_ox + x * g_u;
    float base = roundf(g_oy + y * g_u + g_ascent[f] * k);
    for (; *s; ++s) {
        int c = GlyphIndex((unsigned char)*s);
        if (c < 0) continue;
        const stbtt_packedchar& g = chars[c];
        if (g.x1 > g.x0 && g_quadCount < (int)(sizeof(g_quads) / sizeof(g_quads[0]))) {
            g_quads[g_quadCount++] = {roundf(pen) + g.xoff * k, base + g.yoff * k, (g.xoff2 - g.xoff) * k,
                                      (g.yoff2 - g.yoff) * k, g.x0 * inv, g.y0 * inv, g.x1 * inv, g.y1 * inv,
                                      {color, color, color, color}};
        }
        pen += g.xadvance * k + spacing * g_u;
    }
}

// The menu's layout: the 1280x720 design area, scaled to fit and centred.
static void LayoutScreen(float bw, float bh)
{
    g_u = fminf(bw / 1280.f, bh / 720.f);
    g_ox = roundf((bw - 1280 * g_u) / 2);
    g_oy = roundf((bh - 720 * g_u) / 2);
    g_line = fmaxf(1.f, floorf(g_u + 0.25f));
}

// ---- settings menu --------------------------------------------------------------------------

enum { kRowFov, kRowAimFov, kRowLens, kRowSupersample, kRowBorderless, kRowCompare, kRowShoulder, kRowMouseTrap, kRowShotFormat,
       kRowShotSize, kRowShowcase, kRowStats, kRowIntros, kRowCount };
static const float kSupersampleSteps[] = {1.f, 2.f, 2.25f, 3.f, 4.f, 5.f, 6.f, 8.f, 9.f, 10.f};
static const int kSupersampleStepCount = sizeof(kSupersampleSteps) / sizeof(kSupersampleSteps[0]);
static const DWORD kSupersampleApplyDelay = 600;  // ms after the last change, so cycling is cheap

// Entry list geometry (design units).
static const float kListX0 = 105, kListX1 = 745, kListY0 = 140, kRowH = 27, kRowGap = 3;

static int g_row;
static float g_ssPending = -1.f;  // chosen but not yet applied
static DWORD g_ssChangeTime;
static float g_valueLeft[kRowCount];  // design x where each entry's "<" starts (for mouse clicks)

static bool g_stats;
static volatile bool g_statsForced;  // shown for the showcase series, whatever g_stats says
static float g_statsX = -1, g_statsY = -1;  // fractions of the free space; -1: top right

// "OFF" or e.g. "2.25X".
static void FormatFactor(char* out, float f)
{
    if (f <= 1.01f) lstrcpyA(out, "OFF");
    else if ((int)(f * 100 + 0.5f) % 100) wsprintfA(out, "%d.%02dX", (int)f, (int)(f * 100 + 0.5f) % 100);
    else wsprintfA(out, "%dX", (int)(f + 0.5f));
}

static const char* kLensNames[kLensModeCount] = {"OFF", "AUTO", "PANINI", "STEREOGRAPHIC", "FISHEYE", "FISHEYE CIRCLE"};

// One entry: the label and value in the list, the title and description shown large while it
// is selected (the same texts as the launcher's).
struct Row { const char* label; char value[32]; const char* title; char desc[160]; };

static void FillRow(int row, Row& r)
{
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
        if (LensGetMode() == kLensFisheyeCircle)
            lstrcpyA(r.desc, "The fisheye zoomed out: at FOV 159 all that is rendered shows, as a circle. Aiming fills the screen.");
        else if (lensFov > 0 && fov > 0 && lensFov + 1 < (int)(fov + 0.5f))
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
        else lstrcpyA(r.desc, "Renders more pixels and averages them down: smoother edges, sharper detail.");
        break;
    }
    case kRowBorderless:
        r.label = "BORDERLESS WINDOW";
        r.title = "BORDERLESS";
        lstrcpyA(r.value, BorderlessGet() ? "ON" : "OFF");
        lstrcpyA(r.desc, BorderlessWindowed() ? "In a window: no frame or title bar, centred on the monitor (black around it if smaller)."
                                              : "Applies when the game runs in a window (it runs fullscreen now).");
        break;
    case kRowCompare:
        r.label = "COMPARE";
        r.title = "COMPARE";
        lstrcpyA(r.value, CompareActive() ? "SPLIT" : "OFF");
        lstrcpyA(r.desc, "In gameplay: the game as it is on the left, with ROWE's FOV, lens and supersampling on the right.");
        break;
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
    case kRowShowcase:
        r.label = "SCREENSHOT SERIES";
        r.title = "SHOWCASE";
        lstrcpyA(r.value, ScreenshotShowcase() ? "ON" : "OFF");
        wsprintfA(r.desc, "%s takes 15 shots: 5 lenses at FOV 75, 90 and 159, with the stats window. Again: stop.", ScreenshotKeyName());
        break;
    case kRowStats:
        r.label = "STATS WINDOW";
        r.title = "STATS";
        lstrcpyA(r.value, g_stats ? "ON" : "OFF");
        lstrcpyA(r.desc, "Lens, supersampling and frame rate in a corner, also with the menu closed. Drag it with the mouse.");
        break;
    case kRowIntros:
        r.label = "INTRO VIDEOS";
        r.title = "INTRO VIDEOS";
        lstrcpyA(r.value, IntrosSkipped() ? "SKIP" : "PLAY");
        lstrcpyA(r.desc, "The logo videos at startup. Takes effect at the next start.");
        break;
    }
}

// Key hints, with their place on screen (back buffer pixels) for mouse clicks.
enum { kHintNone, kHintClose, kHintHome };
struct Hint { float x0, y0, x1, y1; int action; };
static Hint g_hints[8];
static int g_hintCount;

// A key in a box, then what it does (as the hints at the bottom of the game's menus).
static void KeyHint(float& x, float y, const char* key, const char* what, int action = kHintNone)
{
    float kw = TextWidth(key, 13, 0) + 10;
    Rect(x, y - 1, kw, 18, Argb(0, 0, 0, 0.35f));
    Frame(x, y - 1, kw, 18, Argb(1, 1, 1, 0.7f));
    Text(x + 5, y, key, 13, Argb(1, 1, 1, 0.9f));
    Text(x + kw + 6, y + 1, what, 14, Argb(1, 1, 1, 0.8f), 2.5f, kLeft, true);
    float w = kw + 6 + TextWidth(what, 14, 2.5f);
    if (g_hintCount < 8) g_hints[g_hintCount++] = {g_ox + x * g_u, g_oy + (y - 3) * g_u, g_ox + (x + w) * g_u, g_oy + (y + 19) * g_u, action};
    x += w + 22;
}

// Lays out the menu as quads for a bw x bh back buffer.
static void BuildUI(float bw, float bh)
{
    LayoutScreen(bw, bh);

    // The game stays visible on the right, darker towards the left where the entries are.
    AddQuad(0, 0, bw, bh, Argb(0, 0, 0, 0.78f), Argb(0, 0, 0, 0.2f), Argb(0, 0, 0, 0.85f), Argb(0, 0, 0, 0.45f));

    Text(105, 60, "ROWE SETTINGS", 26, Argb(1, 1, 1, 0.95f), 6, kLeft, true);
    Text(107, 96, "ROWE FOR GEARS OF WAR (2007)", 12, Argb(0.85f, 0.25f, 0.2f, 0.95f), 4);

    const float x0 = kListX0, x1 = kListX1, y0 = kListY0;
    // The bracket frame around the entries.
    float fy0 = y0 - 14, fy1 = y0 + kRowCount * (kRowH + kRowGap) + 10;
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
        float y = y0 + i * (kRowH + kRowGap);
        // Darker than the launcher's: here the bars lie over the game, not a dimmed picture.
        Rect(x0, y, x1 - x0, kRowH, Argb(0, 0, 0, hot ? 0.6f : 0.7f));
        if (hot) Rect(x0, y, x1 - x0, kRowH, Argb(0.85f, 0.85f, 0.85f, 0.32f));
        HLine(x0, x1, y, Argb(1, 1, 1, hot ? 0.25f : 0.06f));
        DWORD text = Argb(1, 1, 1, hot ? 1.f : 0.72f);
        Text(x0 + 14, y + 5, r.label, 16, text, 3);
        Text(x1 - (hot ? 34 : 14), y + 5, r.value, 16, text, 2.5f, kRight);
        float vw = TextWidth(r.value, 16, 2.5f);
        g_valueLeft[i] = x1 - 34 - vw - 22;
        if (hot) {
            DWORD red = Argb(0.9f, 0.3f, 0.25f, 1);
            Text(x1 - 34 - vw - 22, y + 4, "<", 17, red);
            Text(x1 - 22, y + 4, ">", 17, red);
        }
    }

    // The selected entry, large at the bottom right, as the game shows its menu entries.
    Text(1180, 548, sel.title, 40, Argb(1, 1, 1, 0.95f), 8, kRight, true);
    Text(1180, 600, sel.desc, 14, Argb(1, 1, 1, 0.85f), 1, kRight, true);

    float hx = 105;
    g_hintCount = 0;
    KeyHint(hx, 670, "^ v", "SELECT");
    KeyHint(hx, 670, "< >", "CHANGE");
    if (g_row == kRowFov || g_row == kRowAimFov) {
        KeyHint(hx, 670, "Shift", "BY 10");
        KeyHint(hx, 670, "Home", "DEFAULT", kHintHome);
    }
    KeyHint(hx, 670, g_menuKeyName, "CLOSE", kHintClose);
}

// ---- stats window -----------------------------------------------------------------------------

static float g_fps;
static float g_statsRect[4];  // last drawn place on screen (back buffer pixels): x0, y0, x1, y1

static const float kStatsW = 300, kStatsLine = 19;  // design units
static const int kStatsLines = 5;

// The box's top-left corner in a bw x bh buffer drawn at scale u.
static void StatsPlace(float bw, float bh, float u, float* x, float* y)
{
    float w = kStatsW * u, h = (kStatsLines * kStatsLine + 30) * u, m = 16 * u;
    *x = g_statsX < 0 ? bw - w - m : g_statsX * (bw - w);
    *y = g_statsY < 0 ? m : g_statsY * (bh - h);
}

// The stats box at design scale u (the screen's, or the supersampled frame's for a screenshot).
static void BuildStats(float bw, float bh, float u, bool screen)
{
    g_u = u;
    g_line = fmaxf(1.f, floorf(u + 0.25f));
    StatsPlace(bw, bh, u, &g_ox, &g_oy);
    g_ox = roundf(g_ox);
    g_oy = roundf(g_oy);
    float h = kStatsLines * kStatsLine + 30;
    if (screen) {  // where the mouse can grab it
        g_statsRect[0] = g_ox;
        g_statsRect[1] = g_oy;
        g_statsRect[2] = g_ox + kStatsW * u;
        g_statsRect[3] = g_oy + h * u;
    }
    Rect(0, 0, kStatsW, h, Argb(0, 0, 0, 0.62f));
    Frame(0, 0, kStatsW, h, Argb(1, 1, 1, 0.25f));
    Rect(0, 0, kStatsW, 2, Argb(0.6f, 0.08f, 0.06f, 0.9f));
    Text(12, 8, "ROWE", 12, Argb(0.85f, 0.25f, 0.2f, 0.95f), 4);

    char lines[kStatsLines][2][64] = {};
    int mode = LensGetMode();
    float shown, rendered;
    lstrcpyA(lines[0][0], "LENS");
    lstrcpyA(lines[0][1], kLensNames[mode]);
    lstrcpyA(lines[1][0], "FOV");
    if (FovGet() <= 0) lstrcpyA(lines[1][1], "GAME DEFAULT");
    else if (mode == kLensOff) sprintf(lines[1][1], "%.0f\xB0", FovGet());
    else if (!LensStats(&shown, &rendered)) lstrcpyA(lines[1][1], "NOT IN GAMEPLAY");
    else if (shown <= 0) sprintf(lines[1][1], "%.0f\xB0 STRAIGHT", rendered);
    else sprintf(lines[1][1], "%.0f\xB0, RENDERED %.0f\xB0", shown, rendered);
    lstrcpyA(lines[2][0], "SUPERSAMPLING");
    UINT nw, nh, vw, vh;
    char factor[16];
    FormatFactor(factor, SupersampleSetting());
    if (SupersampleGetSizes(&nw, &nh, &vw, &vh)) sprintf(lines[2][1], "%s, %u \xD7 %u", factor, vw, vh);
    else lstrcpyA(lines[2][1], "OFF");
    lstrcpyA(lines[3][0], "SCREEN");
    if (SupersampleGetSizes(&nw, &nh, &vw, &vh)) sprintf(lines[3][1], "%u \xD7 %u", nw, nh);
    else sprintf(lines[3][1], "%.0f \xD7 %.0f", bw, bh);
    lstrcpyA(lines[4][0], "FRAME RATE");
    sprintf(lines[4][1], "%.0f FPS", g_fps);
    for (int i = 0; i < kStatsLines; ++i) {
        float y = 26 + i * kStatsLine;
        Text(12, y, lines[i][0], 13, Argb(1, 1, 1, 0.65f), 2);
        Text(kStatsW - 12, y, lines[i][1], 13, Argb(1, 1, 1, 0.95f), 1, kRight);
    }
}

// ---- compare labels -----------------------------------------------------------------------------

// Compare mode: what each half shows, in the bars above the pictures (or over them when there is no
// bar), and a dark line between them. u: back buffer pixels per design pixel.
static void BuildCompareLabels(float bw, float bh, float u)
{
    float lx, rx, y, w, h;
    CompareLayout(bw, bh, &lx, &rx, &y, &w, &h);
    g_u = u;
    g_ox = g_oy = 0;
    g_line = fmaxf(1.f, floorf(u + 0.25f));
    float top = y * bh / u, size = 16, line = 2;  // design units
    // Divider: the pictures meet in the middle.
    Rect((lx + w) * bw / u - line / 2, top, line, h * bh / u, Argb(0, 0, 0, 1));
    char left[64], right[96], factor[16];
    sprintf(left, "FOV %.0f", CompareGameFov());
    float shown, rendered;
    int mode = LensGetMode();
    float fov = LensStats(&shown, &rendered) && shown > 0 ? shown : FovGet() > 0 ? FovGet() : CompareGameFov();
    sprintf(right, "FOV %.0f", fov);
    if (mode != kLensOff) sprintf(right + strlen(right), " \xB7 %s", kLensNames[mode]);
    UINT nw, nh, vw, vh;
    if (SupersampleGetSizes(&nw, &nh, &vw, &vh)) {
        FormatFactor(factor, SupersampleSetting());
        sprintf(right + strlen(right), " \xB7 SUPERSAMPLING %s", factor);
    }
    float ly = top >= size + 16 ? top - size - 10 : top + 10;  // in the bar, else over the picture
    struct { float x; const char* name; const char* what; } labels[2] = {{lx * bw / u + 12, "GAME", left},
                                                                       {rx * bw / u + 12, "ROWE", right}};
    for (auto& l : labels) {
        Text(l.x, ly, l.name, size, Argb(0.85f, 0.25f, 0.2f, 1), 4, kLeft, true);
        Text(l.x + TextWidth(l.name, size, 4) + 14, ly, l.what, size, Argb(1, 1, 1, 0.9f), 2, kLeft, true);
    }
}

// ---- mouse cursor -------------------------------------------------------------------------------

// The arrow, as Windows draws it: X outline, . fill.
static const char* kArrow[] = {
    "X", "XX", "X.X", "X..X", "X...X", "X....X", "X.....X", "X......X", "X.......X", "X........X", "X.....XXXXX",
    "X..X..X", "X.X X..X", "XX  X..X", "X    X..X", "     X..X", "      XX",
};

static float g_cursorX, g_cursorY;  // back buffer pixels
static bool g_cursorShown;          // the mouse has moved since the menu opened

static void BuildCursor()
{
    float s = fmaxf(1.f, roundf(g_u));
    for (int r = 0; r < (int)(sizeof(kArrow) / sizeof(kArrow[0])); ++r)
        for (int c = 0; kArrow[r][c]; ++c) {
            if (kArrow[r][c] == ' ') continue;
            DWORD col = kArrow[r][c] == 'X' ? 0xFF000000 : 0xFFFFFFFF;
            AddQuad(roundf(g_cursorX) + c * s, roundf(g_cursorY) + r * s, s, s, col, col, col, col);
        }
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

// Mouse input, gathered on the game thread (window messages, DirectInput) and used by Update.
// Window messages carry the position; DirectInput only movement, used when no messages come
// (Windows sends none while the game holds the mouse exclusively in gameplay).
static SRWLOCK g_mouseLock = SRWLOCK_INIT;
static struct {
    bool hasAbs;
    int absX, absY;
    DWORD msgTime;     // last window mouse message
    int dx, dy, wheel; // DirectInput movement, wheel (both sources) in WHEEL_DELTA units
    int downs, ups;
    bool moved;
} g_mouse;

static void MouseMessage(int x, int y)
{
    AcquireSRWLockExclusive(&g_mouseLock);
    g_mouse.hasAbs = true;
    g_mouse.absX = x;
    g_mouse.absY = y;
    g_mouse.moved = true;
    g_mouse.msgTime = GetTickCount();
    ReleaseSRWLockExclusive(&g_mouseLock);
}

static void MouseButton(bool down, bool fromMessage)
{
    AcquireSRWLockExclusive(&g_mouseLock);
    if (fromMessage) g_mouse.msgTime = GetTickCount();
    if (fromMessage || GetTickCount() - g_mouse.msgTime > 250) {
        if (down) ++g_mouse.downs;
        else ++g_mouse.ups;
    }
    ReleaseSRWLockExclusive(&g_mouseLock);
}

static void MouseWheel(int delta, bool fromMessage)
{
    AcquireSRWLockExclusive(&g_mouseLock);
    if (fromMessage) g_mouse.msgTime = GetTickCount();
    if (fromMessage || GetTickCount() - g_mouse.msgTime > 250) g_mouse.wheel += delta;
    ReleaseSRWLockExclusive(&g_mouseLock);
}

bool OverlayBlocksInput() { return g_open && !g_testMode; }

// From dinput.cpp: the game's mouse as it reads it (game thread).
void OverlayMouseDI(int dx, int dy, int wheel, int buttonEdge)
{
    if (!g_open) return;
    AcquireSRWLockExclusive(&g_mouseLock);
    bool messages = GetTickCount() - g_mouse.msgTime <= 250;
    if (!messages && (dx || dy)) {
        g_mouse.dx += dx;
        g_mouse.dy += dy;
        g_mouse.moved = true;
    }
    ReleaseSRWLockExclusive(&g_mouseLock);
    if (wheel) MouseWheel(wheel, false);
    if (buttonEdge) MouseButton(buttonEdge > 0, false);
}

// The game window's messages: the mouse and the menu's keys go to the menu while it is open.
static WNDPROC g_origWndProc;

static LRESULT CALLBACK OverlayWndProc(HWND h, UINT msg, WPARAM w, LPARAM l)
{
    if ((msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) && w == g_menuKey) return 0;  // the game never sees the menu key
    if (g_open && !g_testMode) {
        switch (msg) {
        case WM_MOUSEMOVE: MouseMessage((short)LOWORD(l), (short)HIWORD(l)); return 0;
        case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: MouseMessage((short)LOWORD(l), (short)HIWORD(l)); MouseButton(true, true); return 0;
        case WM_LBUTTONUP: MouseButton(false, true); return 0;
        case WM_MOUSEWHEEL: MouseWheel(GET_WHEEL_DELTA_WPARAM(w), true); return 0;
        case WM_RBUTTONDOWN: case WM_RBUTTONDBLCLK: case WM_MBUTTONDOWN: case WM_MBUTTONDBLCLK: case WM_XBUTTONDOWN:
        case WM_XBUTTONDBLCLK: case WM_MOUSEHWHEEL:
            return 0;
        case WM_KEYDOWN:
            if (w == VK_UP || w == VK_DOWN || w == VK_LEFT || w == VK_RIGHT || w == VK_HOME) return 0;
            break;
        }
        // Button releases still reach the game, so nothing it saw pressed stays down.
    }
    return CallWindowProcW(g_origWndProc, h, msg, w, l);
}

void OverlaySetWindow(HWND window)
{
    if (window && !g_origWndProc) g_origWndProc = (WNDPROC)SetWindowLongPtrW(window, GWLP_WNDPROC, (LONG_PTR)OverlayWndProc);
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

static void SaveStats()
{
    char path[MAX_PATH], val[32];
    IniPath(path);
    WritePrivateProfileStringA("rowe", "stats", g_stats ? "1" : "0", path);
    sprintf(val, "%.4f", g_statsX);
    WritePrivateProfileStringA("rowe", "statsx", val, path);
    sprintf(val, "%.4f", g_statsY);
    WritePrivateProfileStringA("rowe", "statsy", val, path);
}

// One step of the selected entry's value (dir -1 or +1; big: the FOVs by 10).
static void ChangeRow(int row, int dir, bool big, DWORD now)
{
    switch (row) {
    case kRowFov:
    case kRowAimFov: {
        bool aim = row == kRowAimFov;
        float cur = aim ? FovGetAim() : FovGet();
        float lo = aim ? kAimFovMin : (float)kMinFov, hi = aim ? kAimFovMax : (float)kMaxFov;
        if (cur <= 0) cur = aim ? 40.f : 80.f;
        cur += dir * (big ? 10.f : 1.f);
        if (cur < lo) cur = lo;
        if (cur > hi) cur = hi;
        if (aim) FovSetAim(cur);
        else FovSet(cur);
        break;
    }
    case kRowLens: LensSetMode((LensGetMode() + kLensModeCount + dir) % kLensModeCount); break;
    case kRowSupersample: ChangeSupersample(dir, now); break;
    case kRowBorderless: BorderlessSet(!BorderlessGet()); break;
    case kRowCompare: CompareSet(!CompareActive()); break;
    case kRowShoulder: ShoulderSetStartLeft(!ShoulderStartLeft()); break;
    case kRowMouseTrap: MouseTrapSetMode((MouseTrapGetMode() + kMouseTrapModeCount + dir) % kMouseTrapModeCount); break;
    case kRowShotFormat: ScreenshotSetJpg(!ScreenshotJpg()); break;
    case kRowShotSize: ScreenshotSetFull(!ScreenshotFull()); break;
    case kRowShowcase: ScreenshotSetShowcase(!ScreenshotShowcase()); break;
    case kRowStats: g_stats = !g_stats; SaveStats(); break;
    case kRowIntros: IntrosSetSkipped(!IntrosSkipped()); break;
    }
}

static void Home()
{
    if (g_row == kRowAimFov) FovSetAim(0);
    else if (g_row == kRowFov) FovSet(0);
}

static float g_bufW, g_bufH;  // the real back buffer's size (for the cursor and the stats box)
static bool g_dragging;
static float g_dragDX, g_dragDY;

// The entry under a back buffer point, or -1.
static int RowAt(float px, float py, float* designX)
{
    float x = (px - g_ox) / g_u, y = (py - g_oy) / g_u;
    *designX = x;
    if (x < kListX0 || x > kListX1 || y < kListY0) return -1;
    int i = (int)((y - kListY0) / (kRowH + kRowGap));
    if (i >= kRowCount || y - kListY0 - i * (kRowH + kRowGap) > kRowH) return -1;
    return i;
}

// The mouse, once per frame while the menu is open (hit tests against last frame's menu).
static void UpdateMouse(DWORD now, bool big)
{
    AcquireSRWLockExclusive(&g_mouseLock);
    auto m = g_mouse;
    g_mouse.dx = g_mouse.dy = g_mouse.wheel = g_mouse.downs = g_mouse.ups = 0;
    g_mouse.hasAbs = g_mouse.moved = false;
    ReleaseSRWLockExclusive(&g_mouseLock);
    if (g_bufW <= 0) return;
    LayoutScreen(g_bufW, g_bufH);  // the menu's layout, for RowAt
    if (m.hasAbs) {
        g_cursorX = (float)m.absX;
        g_cursorY = (float)m.absY;
    }
    float speed = fmaxf(1.f, g_bufH / 1080.f);
    g_cursorX += m.dx * speed;
    g_cursorY += m.dy * speed;
    g_cursorX = fminf(fmaxf(g_cursorX, 0), g_bufW - 1);
    g_cursorY = fminf(fmaxf(g_cursorY, 0), g_bufH - 1);
    if (m.moved) g_cursorShown = true;
    if (!g_cursorShown) return;

    float dx;
    int row = RowAt(g_cursorX, g_cursorY, &dx);
    if (g_dragging) {
        float w = g_statsRect[2] - g_statsRect[0], h = g_statsRect[3] - g_statsRect[1];
        float x = g_cursorX - g_dragDX, y = g_cursorY - g_dragDY;
        g_statsX = g_bufW > w ? fminf(fmaxf(x / (g_bufW - w), 0), 1) : 0;
        g_statsY = g_bufH > h ? fminf(fmaxf(y / (g_bufH - h), 0), 1) : 0;
        if (m.ups) {
            g_dragging = false;
            SaveStats();
        }
        return;
    }
    if (m.moved && row >= 0) g_row = row;
    if (m.wheel && row >= 0) ChangeRow(row, m.wheel > 0 ? 1 : -1, big, now);
    if (!m.downs) return;
    if (g_stats && g_cursorX >= g_statsRect[0] && g_cursorX < g_statsRect[2] && g_cursorY >= g_statsRect[1] && g_cursorY < g_statsRect[3]) {
        g_dragging = true;
        g_dragDX = g_cursorX - g_statsRect[0];
        g_dragDY = g_cursorY - g_statsRect[1];
        return;
    }
    for (int i = 0; i < g_hintCount; ++i) {
        const Hint& hnt = g_hints[i];
        if (g_cursorX < hnt.x0 || g_cursorX >= hnt.x1 || g_cursorY < hnt.y0 || g_cursorY >= hnt.y1) continue;
        if (hnt.action == kHintClose) g_open = false;
        if (hnt.action == kHintHome) Home();
        return;
    }
    if (row < 0) return;
    g_row = row;
    // The value's left half steps back, its right half (and the label) forward.
    float mid = (g_valueLeft[row] + kListX1) / 2;
    ChangeRow(row, dx >= g_valueLeft[row] - 8 && dx < mid ? -1 : 1, big, now);
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
    bool wasOpen = g_open;
    if (cmd == kCmdOpen) g_open = true;
    if (cmd == kCmdClose) g_open = false;
    bool keys = !g_testMode && GameHasFocus();
    if (g_testMode) g_open = true;
    if (keys && Pressed(g_menuKey)) g_open = !g_open;
    if (g_open && !wasOpen) {
        g_cursorShown = false;
        g_dragging = false;
        g_cursorX = g_bufW / 2;
        g_cursorY = g_bufH / 2;
        AcquireSRWLockExclusive(&g_mouseLock);
        g_mouse = {};
        ReleaseSRWLockExclusive(&g_mouseLock);
    }
    if (!g_open) return;

    bool big = cmd == kCmdLeft10 || cmd == kCmdRight10 || (keys && (GetAsyncKeyState(VK_SHIFT) & 0x8000));
    UpdateMouse(now, big);
    if (!g_open) return;  // closed by a click

    if ((keys && Pressed(VK_UP)) || cmd == kCmdUp) g_row = (g_row + kRowCount - 1) % kRowCount;
    if ((keys && Pressed(VK_DOWN)) || cmd == kCmdDown) g_row = (g_row + 1) % kRowCount;
    if ((keys && Pressed(VK_HOME)) || cmd == kCmdHome) Home();

    bool leftEdge = keys && Pressed(VK_LEFT), rightEdge = keys && Pressed(VK_RIGHT);
    int cmdDir = cmd == kCmdLeft || cmd == kCmdLeft10 ? -1 : cmd == kCmdRight || cmd == kCmdRight10 ? 1 : 0;
    if (g_row == kRowFov || g_row == kRowAimFov) {
        // Held arrows repeat for the FOVs.
        static DWORD lastStep;
        bool left = keys && (GetAsyncKeyState(VK_LEFT) & 0x8000) != 0;
        bool right = keys && (GetAsyncKeyState(VK_RIGHT) & 0x8000) != 0;
        int step = cmdDir;
        if ((left || right) && (leftEdge || rightEdge || now - lastStep > 60)) step = right ? 1 : -1;
        if (step) {
            lastStep = now;
            ChangeRow(g_row, step, big, now);
        }
    } else {
        int dir = rightEdge ? 1 : leftEdge ? -1 : cmdDir;
        if (dir) ChangeRow(g_row, dir, big, now);
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

// Device resources, the real back buffer's render target and the font for its size. False if the
// overlay cannot be drawn at all (it then stays off).
static bool g_failed;

static bool Prepare(IDXGISwapChain* swap)
{
    ID3D10Device* dev = nullptr;
    if (FAILED(swap->GetDevice(__uuidof(ID3D10Device), (void**)&dev)) || !dev) return false;
    if (dev != g_dev) {
        ReleaseAll();
        bool ok = CreateResources(dev);
        dev->Release();  // g_dev holds its own reference
        if (!ok) {
            Log("overlay: resource creation failed");
            ReleaseAll();
            g_failed = true;
            return false;
        }
        Log("overlay: resources created");
    } else {
        dev->Release();
    }
    if (!g_rtv) {
        ID3D10Texture2D* bb = nullptr;
        if (FAILED(SwapGetRealBuffer(swap, &bb))) return false;
        D3D10_TEXTURE2D_DESC bd;
        bb->GetDesc(&bd);
        g_bufW = (float)bd.Width;
        g_bufH = (float)bd.Height;
        HRESULT hr = g_dev->CreateRenderTargetView(bb, nullptr, &g_rtv);
        bb->Release();
        if (FAILED(hr)) return false;
    }
    float scale = fminf(g_bufW / 1280.f, g_bufH / 720.f);
    if (fabsf(scale - g_atlasScale) > 0.001f && !BuildAtlas(scale)) {
        Log("overlay: font atlas failed");
        g_failed = true;
        return false;
    }
    if (g_atlasChanged && !CreateFontTexture()) {
        Log("overlay: font texture failed");
        g_failed = true;
        return false;
    }
    return true;
}

// Draws the quads into a w x h render target.
static void DrawQuads(ID3D10RenderTargetView* rtv, float w, float h)
{
    Vtx* v = nullptr;
    if (!g_quadCount || FAILED(g_vb->Map(D3D10_MAP_WRITE_DISCARD, 0, (void**)&v))) return;
    int n = 0;
    for (int i = 0; i < g_quadCount && n + 6 <= kMaxVerts; ++i) {
        const Quad& q = g_quads[i];
        float x0 = q.x / w * 2 - 1, x1 = (q.x + q.w) / w * 2 - 1;
        float y0 = 1 - q.y / h * 2, y1 = 1 - (q.y + q.h) / h * 2;
        Vtx tl = {x0, y0, q.u0, q.v0, ToRGBA(q.c[0])}, tr = {x1, y0, q.u1, q.v0, ToRGBA(q.c[1])};
        Vtx bl = {x0, y1, q.u0, q.v1, ToRGBA(q.c[2])}, br = {x1, y1, q.u1, q.v1, ToRGBA(q.c[3])};
        v[n++] = tl; v[n++] = tr; v[n++] = bl;
        v[n++] = tr; v[n++] = br; v[n++] = bl;
    }
    g_vb->Unmap();

    g_stateBlock->Capture();  // the engine's pipeline state, restored after our draw

    D3D10_VIEWPORT vp = {0, 0, (UINT)w, (UINT)h, 0.f, 1.f};
    float blendFactor[4] = {0, 0, 0, 0};
    UINT stride = sizeof(Vtx), offset = 0;
    g_dev->OMSetRenderTargets(1, &rtv, nullptr);
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
}

// From screenshot.cpp, before a full-size shot of the supersampled frame: the stats window goes
// into that frame too (scaled up with it).
void OverlayForceStats(bool on) { g_statsForced = on; }

void OverlayStatsInto(ID3D10Texture2D* tex)
{
    if (!(g_stats || g_statsForced || CompareSplitNow()) || !g_dev || g_failed || g_bufH <= 0) return;
    D3D10_TEXTURE2D_DESC td;
    tex->GetDesc(&td);
    ID3D10RenderTargetView* rtv = nullptr;
    if (FAILED(g_dev->CreateRenderTargetView(tex, nullptr, &rtv))) return;
    float scale = fminf(g_bufW / 1280.f, g_bufH / 720.f) * td.Height / g_bufH;
    g_quadCount = 0;
    if (g_stats || g_statsForced) BuildStats((float)td.Width, (float)td.Height, scale, false);
    if (CompareSplitNow()) BuildCompareLabels((float)td.Width, (float)td.Height, scale);
    DrawQuads(rtv, (float)td.Width, (float)td.Height);
    rtv->Release();
}

static void MeasureFps()
{
    static LARGE_INTEGER freq, last;
    static int frames;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (!freq.QuadPart) {
        QueryPerformanceFrequency(&freq);
        last = now;
    }
    ++frames;
    double dt = (double)(now.QuadPart - last.QuadPart) / freq.QuadPart;
    if (dt >= 0.5) {
        g_fps = (float)(frames / dt);
        frames = 0;
        last = now;
    }
}

// Reads menukey and the stats window's settings from rowe.ini.
static void LoadSettings()
{
    char path[MAX_PATH], val[32];
    IniPath(path);
    GetPrivateProfileStringA("rowe", "menukey", "F8", val, sizeof(val), path);
    g_menuKey = KeyFromName(val);
    if (!g_menuKey) {
        Log("overlay: unknown menu key '%s', using F8", val);
        g_menuKey = VK_F8;
        lstrcpyA(val, "F8");
    }
    lstrcpynA(g_menuKeyName, val, sizeof(g_menuKeyName));
    CharUpperA(g_menuKeyName);
    g_stats = GetPrivateProfileIntA("rowe", "stats", 0, path) != 0;
    GetPrivateProfileStringA("rowe", "statsx", "-1", val, sizeof(val), path);
    g_statsX = (float)atof(val);
    GetPrivateProfileStringA("rowe", "statsy", "-1", val, sizeof(val), path);
    g_statsY = (float)atof(val);
    if (g_statsX > 1) g_statsX = 1;
    if (g_statsY > 1) g_statsY = 1;
}

// Every frame from Present: input for the menu, Tab and the screenshot key, the stats window,
// then the menu itself when open.
void OverlayFrame(IDXGISwapChain* swap)
{
    static bool init;
    if (!init) {
        init = true;
        LoadSettings();
        char v[8] = {};
        g_testMode = GetEnvironmentVariableA("ROWE_TEST", v, sizeof(v)) && v[0] == '1';
        if (g_testMode && GetEnvironmentVariableA("ROWE_TEST_ROW", v, sizeof(v))) g_row = atoi(v) % kRowCount;
        if (g_testMode && GetEnvironmentVariableA("ROWE_TEST_STATS", v, sizeof(v))) g_stats = v[0] == '1';
    }
    MeasureFps();
    Update();
    bool focused = !g_testMode && GameHasFocus();
    ShoulderUpdate(focused);
    bool stats = g_stats || g_statsForced, labels = CompareSplitNow();
    bool ready = !g_failed && (g_open || stats || labels) && Prepare(swap);
    if (ready && (stats || labels)) {  // before the screenshot key's shot, so they are in it
        float scale = fminf(g_bufW / 1280.f, g_bufH / 720.f);
        g_quadCount = 0;
        if (labels) BuildCompareLabels(g_bufW, g_bufH, scale);
        if (stats) BuildStats(g_bufW, g_bufH, scale, true);
        DrawQuads(g_rtv, g_bufW, g_bufH);
    }
    ScreenshotFrame(swap, focused);
    if (ready && g_open) {
        g_quadCount = 0;
        BuildUI(g_bufW, g_bufH);
        if (stats) BuildStats(g_bufW, g_bufH, g_u, true);  // again on top, to be dragged
        if (g_cursorShown) BuildCursor();
        DrawQuads(g_rtv, g_bufW, g_bufH);
    }
    ScreenshotWithMenu(swap);
}
