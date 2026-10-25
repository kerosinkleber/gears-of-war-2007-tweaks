// rowe_launcher.exe: set the mod's and the game's settings before the game starts, keep save
// slots, and start the game straight into "continue" or a new campaign at a chosen difficulty.
// It lives in the game's Binaries folder (next to Startup.exe and the mod's d3d9.dll) and is
// drawn with Dear ImGui (third_party/imgui, MIT) on Direct3D 9 in the style of the game's own menus.
//
// Save slots: Gears of War keeps its one campaign checkpoint in the player profile (with the
// XLiveEmu GFWL replacement: Binaries\XLiveEmu\<profile>\Offline\Title1-3.dat, encrypted for the
// Windows user), and a new campaign overwrites it. A slot is a copy of those files in
// Binaries\rowe_saves\<id>\ with a slot.ini (name, date). Restoring copies them back, so it also
// brings back the profile's other contents from that time (options, unlocked chapters). Before a
// new campaign or a restore the current files are kept as a slot unless a slot already has them.
//
// Starting: the launcher writes rowe_launch.txt for the mod (launch.cpp), which walks the
// game's menus once the title screen is up, then starts Startup.exe and closes.
#include <windows.h>
#include <d3d9.h>
#include <dwmapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <tlhelp32.h>
#include <math.h>
#include <stdio.h>
#include <stdarg.h>
#include <algorithm>
#include <string>
#include <vector>
#include <wincodec.h>
#include <functional>
#include "imgui.h"
#include "imgui_internal.h"
#include "backends/imgui_impl_dx9.h"
#include "backends/imgui_impl_win32.h"
#include "sharetechmono_ttf.h"
#include "../migrate.h"
#include "../keynames.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

static std::string g_bin;       // the game's Binaries folder (this exe's), with trailing backslash
static std::string g_modIni;    // rowe.ini
static std::string g_gameIni;   // WarEngineUserSettings.ini
static std::string g_status;
static bool g_statusError;

static void Status(bool error, const char* fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
    va_end(ap);
    buf[sizeof(buf) - 1] = 0;
    g_status = buf;
    g_statusError = error;
}

static bool Exists(const std::string& path) { return GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

// ---- ini helpers -----------------------------------------------------------------------------

static std::string IniGet(const std::string& file, const char* section, const char* key, const char* def)
{
    char buf[MAX_PATH];
    GetPrivateProfileStringA(section, key, def, buf, sizeof(buf), file.c_str());
    return buf;
}

static int IniInt(const std::string& file, const char* section, const char* key, int def)
{
    return GetPrivateProfileIntA(section, key, def, file.c_str());
}

static bool IniBool(const std::string& file, const char* section, const char* key, bool def)
{
    std::string v = IniGet(file, section, key, def ? "True" : "False");
    return !lstrcmpiA(v.c_str(), "true") || v == "1";
}

static void IniSet(const std::string& file, const char* section, const char* key, const std::string& value)
{
    WritePrivateProfileStringA(section, key, value.c_str(), file.c_str());
}

static void IniSetInt(const std::string& file, const char* section, const char* key, int value)
{
    IniSet(file, section, key, std::to_string(value));
}

// ---- the mod's settings (rowe.ini; same keys and ranges as the F8 menu) --------------------

struct ModSettings {
    int fov, aimFov, lens, mouseTrap, quality;
    float supersample;
    bool shoulderLeft, skipIntros, jpg, full, borderless, stats;
    char key[16], menuKey[16], dir[MAX_PATH];
} g_mod;

static const float kSupersampleSteps[] = {1.f, 2.f, 2.25f, 3.f, 4.f, 5.f, 6.f, 8.f, 9.f, 10.f};
static const int kSupersampleStepCount = sizeof(kSupersampleSteps) / sizeof(kSupersampleSteps[0]);

static void LoadMod()
{
    const char* s = "rowe";
    g_mod.fov = IniInt(g_modIni, s, "fov", 0);
    g_mod.aimFov = IniInt(g_modIni, s, "aimfov", 0);
    g_mod.lens = IniInt(g_modIni, s, "lens", 1);
    g_mod.mouseTrap = IniInt(g_modIni, s, "mousetrap", 0);
    g_mod.supersample = (float)atof(IniGet(g_modIni, s, "supersample", "1").c_str());
    g_mod.shoulderLeft = !lstrcmpiA(IniGet(g_modIni, s, "shoulder", "right").c_str(), "left");
    g_mod.skipIntros = IniInt(g_modIni, s, "skipintros", 1) != 0;
    g_mod.jpg = lstrcmpiA(IniGet(g_modIni, s, "screenshotformat", "jpg").c_str(), "png") != 0;
    std::string size = IniGet(g_modIni, s, "screenshotsize", "full");
    g_mod.full = lstrcmpiA(size.c_str(), "screen") && lstrcmpiA(size.c_str(), "window");
    g_mod.quality = IniInt(g_modIni, s, "screenshotquality", 95);
    lstrcpynA(g_mod.key, IniGet(g_modIni, s, "screenshotkey", "F9").c_str(), sizeof(g_mod.key));
    lstrcpynA(g_mod.menuKey, IniGet(g_modIni, s, "menukey", "F8").c_str(), sizeof(g_mod.menuKey));
    g_mod.borderless = IniInt(g_modIni, s, "borderless", 0) != 0;
    g_mod.stats = IniInt(g_modIni, s, "stats", 0) != 0;
    lstrcpynA(g_mod.dir, IniGet(g_modIni, s, "screenshotdir", "").c_str(), sizeof(g_mod.dir));
}

static void SaveMod()
{
    const char* s = "rowe";
    char ss[16];
    sprintf_s(ss, "%.2f", g_mod.supersample);
    IniSetInt(g_modIni, s, "fov", g_mod.fov);
    IniSetInt(g_modIni, s, "aimfov", g_mod.aimFov);
    IniSetInt(g_modIni, s, "lens", g_mod.lens);
    IniSetInt(g_modIni, s, "mousetrap", g_mod.mouseTrap);
    IniSet(g_modIni, s, "supersample", ss);
    IniSet(g_modIni, s, "shoulder", g_mod.shoulderLeft ? "left" : "right");
    IniSetInt(g_modIni, s, "skipintros", g_mod.skipIntros ? 1 : 0);
    IniSet(g_modIni, s, "screenshotformat", g_mod.jpg ? "jpg" : "png");
    IniSet(g_modIni, s, "screenshotsize", g_mod.full ? "full" : "screen");
    IniSetInt(g_modIni, s, "screenshotquality", g_mod.quality);
    IniSet(g_modIni, s, "screenshotkey", g_mod.key);
    IniSet(g_modIni, s, "menukey", g_mod.menuKey);
    IniSetInt(g_modIni, s, "borderless", g_mod.borderless ? 1 : 0);
    IniSetInt(g_modIni, s, "stats", g_mod.stats ? 1 : 0);
    IniSet(g_modIni, s, "screenshotdir", g_mod.dir);
}

// ---- the game's settings (WarEngineUserSettings.ini) -----------------------------------------

struct GameSettings {
    int resX, resY, msaa, aniso, detail;
    bool fullscreen, d3d10, vsync, bloom, motionBlur, dof, lensFlares, shadows;
} g_game;

struct Mode { int w, h; };
static std::vector<Mode> g_modes;

static void LoadGame()
{
    const char* sys = "SystemSettings";
    const char* win = "WinDrv.WindowsClient";
    g_game.resX = IniInt(g_gameIni, win, "StartupResolutionX", 1280);
    g_game.resY = IniInt(g_gameIni, win, "StartupResolutionY", 720);
    g_game.fullscreen = IniBool(g_gameIni, win, "StartupFullscreen", true);
    g_game.d3d10 = IniBool(g_gameIni, sys, "ALLOWD3D10", false);
    g_game.vsync = IniBool(g_gameIni, sys, "USEVSYNC", false);
    g_game.msaa = IniInt(g_gameIni, sys, "MAXMULTISAMPLES", 1);
    g_game.aniso = IniInt(g_gameIni, sys, "MaxAnisotropy", 4);
    g_game.detail = IniInt(g_gameIni, sys, "DETAILMODE", 2);
    g_game.bloom = IniBool(g_gameIni, sys, "Bloom", true);
    g_game.motionBlur = IniBool(g_gameIni, sys, "MotionBlur", true);
    g_game.dof = IniBool(g_gameIni, sys, "DepthOfField", true);
    g_game.lensFlares = IniBool(g_gameIni, sys, "LensFlares", true);
    g_game.shadows = IniBool(g_gameIni, sys, "DynamicShadows", true);
}

static void SaveGame()
{
    const char* sys = "SystemSettings";
    const char* win = "WinDrv.WindowsClient";
    auto b = [](bool v) { return std::string(v ? "True" : "False"); };
    IniSetInt(g_gameIni, win, "StartupResolutionX", g_game.resX);
    IniSetInt(g_gameIni, win, "StartupResolutionY", g_game.resY);
    IniSet(g_gameIni, win, "StartupFullscreen", b(g_game.fullscreen));
    IniSet(g_gameIni, sys, "ALLOWD3D10", b(g_game.d3d10));
    IniSet(g_gameIni, sys, "USEVSYNC", b(g_game.vsync));
    IniSetInt(g_gameIni, sys, "MAXMULTISAMPLES", g_game.msaa);
    IniSetInt(g_gameIni, sys, "MaxAnisotropy", g_game.aniso);
    IniSetInt(g_gameIni, sys, "DETAILMODE", g_game.detail);
    IniSet(g_gameIni, sys, "Bloom", b(g_game.bloom));
    IniSet(g_gameIni, sys, "MotionBlur", b(g_game.motionBlur));
    IniSet(g_gameIni, sys, "DepthOfField", b(g_game.dof));
    IniSet(g_gameIni, sys, "LensFlares", b(g_game.lensFlares));
    IniSet(g_gameIni, sys, "DynamicShadows", b(g_game.shadows));
}

// The primary display's modes (width x height, largest first).
static void EnumerateModes()
{
    DEVMODEA dm = {};
    dm.dmSize = sizeof(dm);
    for (DWORD i = 0; EnumDisplaySettingsA(nullptr, i, &dm); ++i) {
        if (dm.dmPelsWidth < 800 || dm.dmBitsPerPel < 32) continue;
        bool known = false;
        for (auto& m : g_modes) known |= m.w == (int)dm.dmPelsWidth && m.h == (int)dm.dmPelsHeight;
        if (!known) g_modes.push_back({(int)dm.dmPelsWidth, (int)dm.dmPelsHeight});
    }
    std::sort(g_modes.begin(), g_modes.end(), [](const Mode& a, const Mode& b) { return a.w * a.h > b.w * b.h; });
}

// ---- profiles and save slots -----------------------------------------------------------------

static const char* kProfileFiles[] = {"Title1.dat", "Title2.dat", "Title3.dat"};

static std::vector<std::string> g_profiles;  // XLiveEmu profile names with an Offline folder
static int g_profile;

static std::string ProfileDir() { return g_profiles.empty() ? "" : g_bin + "XLiveEmu\\" + g_profiles[g_profile] + "\\Offline\\"; }

static void FindProfiles()
{
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((g_bin + "XLiveEmu\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    FILETIME newest = {};
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == '.') continue;
        std::string title1 = g_bin + "XLiveEmu\\" + fd.cFileName + "\\Offline\\Title1.dat";
        WIN32_FILE_ATTRIBUTE_DATA a;
        if (!GetFileAttributesExA(title1.c_str(), GetFileExInfoStandard, &a)) continue;
        if (CompareFileTime(&a.ftLastWriteTime, &newest) > 0) {  // the most recently used one first
            newest = a.ftLastWriteTime;
            g_profile = (int)g_profiles.size();
        }
        g_profiles.push_back(fd.cFileName);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

struct Slot { std::string dir, name, saved; };
static std::vector<Slot> g_slots;  // newest first
static int g_slotSel = -1;

static std::string SlotsRoot() { return g_bin + "rowe_saves\\"; }

static void LoadSlots()
{
    g_slots.clear();
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((SlotsRoot() + "*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == '.') continue;
            std::string dir = SlotsRoot() + fd.cFileName + "\\";
            std::string ini = dir + "slot.ini";
            if (!Exists(ini)) continue;
            g_slots.push_back({dir, IniGet(ini, "slot", "name", fd.cFileName), IniGet(ini, "slot", "saved", "")});
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    std::sort(g_slots.begin(), g_slots.end(), [](const Slot& a, const Slot& b) { return a.dir > b.dir; });  // ids are timestamps
    if (g_slotSel >= (int)g_slots.size()) g_slotSel = -1;
}

static bool ReadFileBytes(const std::string& path, std::string* out)
{
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    out->clear();
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out->append(buf, n);
    fclose(f);
    return true;
}

// Whether two folders hold the same profile files.
static bool SameFiles(const std::string& a, const std::string& b)
{
    for (const char* name : kProfileFiles) {
        std::string x, y;
        bool hx = ReadFileBytes(a + name, &x), hy = ReadFileBytes(b + name, &y);
        if (hx != hy || x != y) return false;
    }
    return true;
}

static bool CopyProfileFiles(const std::string& from, const std::string& to)
{
    for (const char* name : kProfileFiles) {
        if (!Exists(from + name)) continue;
        if (!CopyFileA((from + name).c_str(), (to + name).c_str(), FALSE)) return false;
    }
    return true;
}

static std::string Now(bool forId)
{
    SYSTEMTIME t;
    GetLocalTime(&t);
    char buf[32];
    if (forId) sprintf_s(buf, "%04d%02d%02d_%02d%02d%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    else sprintf_s(buf, "%04d-%02d-%02d %02d:%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute);
    return buf;
}

// Copies the profile's current files into a new slot. With onlyIfNew, nothing is done when a slot
// already holds exactly these files. Returns false on an error (the status says which).
static bool SaveSlot(const std::string& name, bool onlyIfNew)
{
    std::string profile = ProfileDir();
    if (profile.empty() || !Exists(profile + "Title1.dat")) {
        Status(true, "No profile save found (XLiveEmu\\<profile>\\Offline\\Title1.dat)");
        return false;
    }
    if (onlyIfNew)
        for (auto& s : g_slots)
            if (SameFiles(profile, s.dir)) return true;
    CreateDirectoryA(SlotsRoot().c_str(), nullptr);
    std::string dir = SlotsRoot() + Now(true);
    while (Exists(dir)) dir += "_";
    dir += "\\";
    if (!CreateDirectoryA(dir.c_str(), nullptr) || !CopyProfileFiles(profile, dir)) {
        Status(true, "Could not write the save slot %s", dir.c_str());
        return false;
    }
    IniSet(dir + "slot.ini", "slot", "name", name);
    IniSet(dir + "slot.ini", "slot", "saved", Now(false));
    IniSet(dir + "slot.ini", "slot", "profile", g_profiles[g_profile]);
    LoadSlots();
    return true;
}

static void DeleteSlot(const Slot& s)
{
    for (const char* name : kProfileFiles) DeleteFileA((s.dir + name).c_str());
    DeleteFileA((s.dir + "slot.ini").c_str());
    RemoveDirectoryA(s.dir.c_str());
    LoadSlots();
}

static bool RestoreSlot(const Slot& s)
{
    if (!SaveSlot("Kept before restoring \"" + s.name + "\"", true)) return false;
    if (!CopyProfileFiles(s.dir, ProfileDir())) {
        Status(true, "Could not copy the slot into the profile");
        return false;
    }
    Status(false, "Restored \"%s\": Continue campaign now loads it", s.name.c_str());
    return true;
}

// ---- starting the game -----------------------------------------------------------------------

static bool GameRunning()
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32 pe = {sizeof(pe)};
    bool running = false;
    for (BOOL ok = Process32First(snap, &pe); ok; ok = Process32Next(snap, &pe))
        running |= !lstrcmpiA(pe.szExeFile, "WarGame-G4WLive.exe") || !lstrcmpiA(pe.szExeFile, "Startup.exe");
    CloseHandle(snap);
    return running;
}

static const char* kDifficulties[] = {"Casual", "Hardcore", "Insane"};

// Writes the request for the mod and starts the game; true if it started.
static bool Launch(bool newCampaign, int difficulty)
{
    SaveMod();
    SaveGame();
    if (newCampaign && !SaveSlot("Kept before a new campaign", true)) return false;
    std::string req = g_bin + "rowe_launch.txt";
    IniSet(req, "launch", "action", newCampaign ? "new" : "continue");
    IniSet(req, "launch", "difficulty", newCampaign ? kDifficulties[difficulty] : "");
    STARTUPINFOA si = {sizeof(si)};
    PROCESS_INFORMATION pi = {};
    std::string exe = g_bin + "Startup.exe";
    if (!CreateProcessA(exe.c_str(), nullptr, nullptr, nullptr, FALSE, 0, nullptr, g_bin.c_str(), &si, &pi)) {
        DeleteFileA(req.c_str());
        Status(true, "Could not start %s (error %lu)", exe.c_str(), GetLastError());
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

// ---- look: Gears of War's own menus ----------------------------------------------------------
// Drawn like the game's Main Menu: the title screen's artwork behind everything (saved by the mod
// as rowe_menubg.jpg on the first start; a dark red gradient until then), a spaced capital
// heading, the entries as dark bars in a thin bracket frame with the highlighted one lit, a large
// title and a line of text for the highlighted entry at the bottom right, and key hints at the
// bottom left. Laid out in a 1280x720 design space scaled to the window.

static float g_u = 1;   // window pixels per design pixel
static ImVec2 g_org;    // the design area's top-left corner in the window
static ImFont* g_font;
static IDirect3DDevice9* g_dev;
static IDirect3DTexture9* g_bg;
static int g_bgW, g_bgH;

static ImVec2 P(float x, float y) { return ImVec2(g_org.x + x * g_u, g_org.y + y * g_u); }
static ImU32 Rgba(float r, float g, float b, float a) { return ImGui::ColorConvertFloat4ToU32(ImVec4(r, g, b, a)); }

// Width of `s` (UTF-8) at `size` design pixels with `spacing` between letters.
static float TextWidth(const char* s, float size, float spacing)
{
    float w = 0;
    const char* end = s + strlen(s);
    for (const char* p = s; p < end;) {
        unsigned int c;
        int n = ImTextCharFromUtf8(&c, p, end);
        if (n <= 0) break;
        w += g_font->CalcTextSizeA(size * g_u, FLT_MAX, 0, p, p + n).x + spacing * g_u;
        p += n;
    }
    return w > 0 ? (w - spacing * g_u) / g_u : 0;
}

enum { kLeft, kRight, kCenter };

// Draws `s` at design position (x, y), letter-spaced, aligned at x.
static void Text(ImDrawList* dl, float x, float y, const char* s, float size, ImU32 col, float spacing = 0, int align = kLeft)
{
    float w = TextWidth(s, size, spacing);
    float px = align == kRight ? x - w : align == kCenter ? x - w / 2 : x;
    ImVec2 at = P(px, y);
    const char* end = s + strlen(s);
    for (const char* p = s; p < end;) {
        unsigned int c;
        int n = ImTextCharFromUtf8(&c, p, end);
        if (n <= 0) break;
        dl->AddText(g_font, size * g_u, at, col, p, p + n);
        at.x += g_font->CalcTextSizeA(size * g_u, FLT_MAX, 0, p, p + n).x + spacing * g_u;
        p += n;
    }
}

static std::string Upper(std::string s)
{
    for (char& c : s)
        if (c >= 'a' && c <= 'z') c -= 32;
    return s;
}

// The title screen's artwork, if the mod has saved it (WIC decode into a managed texture).
static void LoadBackground()
{
    std::string path = g_bin + "rowe_menubg.jpg";
    if (!Exists(path)) return;
    HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    IWICImagingFactory* f = nullptr;
    IWICBitmapDecoder* dec = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* conv = nullptr;
    wchar_t wpath[MAX_PATH];
    MultiByteToWideChar(CP_ACP, 0, path.c_str(), -1, wpath, MAX_PATH);
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f));
    if (SUCCEEDED(hr)) hr = f->CreateDecoderFromFilename(wpath, nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec);
    if (SUCCEEDED(hr)) hr = dec->GetFrame(0, &frame);
    if (SUCCEEDED(hr)) hr = f->CreateFormatConverter(&conv);
    if (SUCCEEDED(hr)) hr = conv->Initialize(frame, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom);
    UINT w = 0, h = 0;
    if (SUCCEEDED(hr)) hr = conv->GetSize(&w, &h);
    if (SUCCEEDED(hr)) hr = g_dev->CreateTexture(w, h, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &g_bg, nullptr);
    D3DLOCKED_RECT lr;
    if (SUCCEEDED(hr) && SUCCEEDED(g_bg->LockRect(0, &lr, nullptr, 0))) {
        hr = conv->CopyPixels(nullptr, lr.Pitch, lr.Pitch * h, (BYTE*)lr.pBits);
        g_bg->UnlockRect(0);
        g_bgW = w;
        g_bgH = h;
    }
    if (FAILED(hr) && g_bg) { g_bg->Release(); g_bg = nullptr; }
    if (conv) conv->Release();
    if (frame) frame->Release();
    if (dec) dec->Release();
    if (f) f->Release();
    if (SUCCEEDED(co)) CoUninitialize();
}

static void DrawBackground(ImDrawList* dl, ImVec2 size)
{
    if (g_bg) {  // cover the window, cropped to its aspect
        float sa = (float)g_bgW / g_bgH, wa = size.x / size.y;
        ImVec2 uv0(0, 0), uv1(1, 1);
        if (sa > wa) { float k = wa / sa; uv0.x = (1 - k) / 2; uv1.x = 1 - uv0.x; }
        else { float k = sa / wa; uv0.y = (1 - k) / 2; uv1.y = 1 - uv0.y; }
        dl->AddImage((ImTextureID)(intptr_t)g_bg, ImVec2(0, 0), size, uv0, uv1);
    } else {
        dl->AddRectFilledMultiColor(ImVec2(0, 0), size, Rgba(0.30f, 0.03f, 0.03f, 1), Rgba(0.10f, 0.01f, 0.01f, 1),
                                    Rgba(0.02f, 0, 0, 1), Rgba(0.12f, 0.02f, 0.02f, 1));
    }
    // Darker towards the left, where the entries are, as the game's menus are.
    dl->AddRectFilledMultiColor(ImVec2(0, 0), size, Rgba(0, 0, 0, 0.55f), Rgba(0, 0, 0, 0.15f), Rgba(0, 0, 0, 0.25f), Rgba(0, 0, 0, 0.65f));
}

// ---- menu model ------------------------------------------------------------------------------

struct Row {
    std::string label, value;              // value empty: an entry that opens or starts something
    std::string title, desc;               // shown large / small while highlighted
    std::function<void(int)> change;       // Left/Right, wheel, the arrows (settings)
    std::function<void()> activate;        // Enter / click
    bool enabled = true;
};

enum Page { kPageMain, kPageDifficulty, kPageSlots, kPageMod, kPageGame, kPageCount };
static const char* kPageNames[kPageCount] = {"MAIN MENU", "SELECT DIFFICULTY", "SAVE SLOTS", "MOD SETTINGS", "GAME SETTINGS"};
static Page g_page = kPageMain;
static float g_px;  // font pixel size for the window's DPI
static int g_sel[kPageCount], g_top[kPageCount];

// A box in the style of the game's "Checkpoint Overwrite" dialog: a title, text, an optional
// text field, and buttons (Left/Right picks one, Enter presses it, Esc closes).
struct Button { std::string label; std::function<void()> press; };
struct Dialog {
    bool open = false, input = false, focus = false;
    int openedFrame = -1;
    std::string title, text;
    char buf[MAX_PATH] = "";
    std::vector<Button> buttons;
    int sel = 0;
} g_dialog;

static void OpenDialog(const std::string& title, const std::string& text, std::vector<Button> buttons)
{
    g_dialog = Dialog();
    g_dialog.open = true;
    g_dialog.title = title;
    g_dialog.text = text;
    g_dialog.buttons = std::move(buttons);
    g_dialog.openedFrame = ImGui::GetFrameCount();
}

static void OpenInput(const std::string& title, const std::string& text, const char* value, std::function<void(const char*)> accept)
{
    OpenDialog(title, text, {{"ACCEPT", [accept]() { accept(g_dialog.buf); }}, {"CANCEL", nullptr}});
    g_dialog.input = g_dialog.focus = true;
    lstrcpynA(g_dialog.buf, value, sizeof(g_dialog.buf));
}

// Key capture: the next key pressed (WndProc) becomes the setting; Esc cancels.
static std::function<void(UINT)> g_capture;

static void CaptureKey(const char* title, char* target, int size, const char* other, const char* otherName)
{
    std::string t = title;
    OpenDialog(t, "Press the new key. Esc cancels.", {{"CANCEL", []() { g_capture = nullptr; }}});
    g_capture = [target, size, other, otherName, t](UINT vk) {
        char name[16];
        KeyToName(vk, name);
        if (!lstrcmpiA(name, other)) {
            Status(true, "%s is already the %s.", name, otherName);
        } else {
            lstrcpynA(target, name, size);
            SaveMod();
            Status(false, "%s: %s", t.c_str(), name);
        }
        g_capture = nullptr;
        g_dialog.open = false;
    };
}

// The screenshot folder as the mod resolves it (empty: the Binaries folder; relative: from there).
static std::string ShotFolder()
{
    if (!g_mod.dir[0]) return g_bin;
    char raw[MAX_PATH], full[MAX_PATH];
    ExpandEnvironmentStringsA(g_mod.dir, raw, MAX_PATH);
    bool absolute = (raw[0] && raw[1] == ':') || (raw[0] == '\\' && raw[1] == '\\');
    std::string path = absolute ? std::string(raw) : g_bin + raw;
    GetFullPathNameA(path.c_str(), MAX_PATH, full, nullptr);
    return full;
}

// Windows' folder picker, starting at the current folder; true with the choice in `out`.
static bool BrowseFolder(HWND owner, std::string* out)
{
    HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    IFileOpenDialog* dlg = nullptr;
    bool ok = false;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) {
        DWORD opts = 0;
        dlg->GetOptions(&opts);
        dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
        dlg->SetTitle(L"Screenshot folder");
        wchar_t start[MAX_PATH];
        MultiByteToWideChar(CP_ACP, 0, ShotFolder().c_str(), -1, start, MAX_PATH);
        IShellItem* item = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(start, nullptr, IID_PPV_ARGS(&item)))) {
            dlg->SetFolder(item);
            item->Release();
        }
        if (SUCCEEDED(dlg->Show(owner)) && SUCCEEDED(dlg->GetResult(&item))) {
            wchar_t* path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                char a[MAX_PATH];
                WideCharToMultiByte(CP_ACP, 0, path, -1, a, MAX_PATH, nullptr, nullptr);
                *out = a;
                ok = true;
                CoTaskMemFree(path);
            }
            item->Release();
        }
        dlg->Release();
    }
    if (SUCCEEDED(co)) CoUninitialize();
    return ok;
}

static HWND g_hwnd;

static bool g_running;
static bool g_startNew, g_startContinue;
static int g_difficulty;

static void Cycle(int& v, int dir, int count) { v = (v + count + dir) % count; }

// Steps a FOV-like value: below `lo` it becomes 0 ("game default"), from 0 it starts at `base`.
static void StepFov(int& v, int dir, int lo, int hi, int base)
{
    int step = ImGui::GetIO().KeyShift ? 10 : 1;
    if (v == 0) v = base;
    v += dir * step;
    if (v < lo) v = dir < 0 ? 0 : lo;
    if (v > hi) v = hi;
}

static std::string OnOff(bool v) { return v ? "ON" : "OFF"; }

// The render size supersampling gives at the game's resolution (mirrors the mod's ComputeVirtual).
static void VirtualSize(float factor, int* w, int* h)
{
    float s = sqrtf(factor);
    float maxS = 8192.f / (float)(g_game.resX > g_game.resY ? g_game.resX : g_game.resY);
    if (s > maxS) s = maxS;
    *w = ((int)(g_game.resX * s + 0.5f)) & ~1;
    *h = ((int)(g_game.resY * s + 0.5f)) & ~1;
}

static void Go(Page p)
{
    g_page = p;
    g_status.clear();
}

static std::vector<Row> BuildRows()
{
    std::vector<Row> rows;
    bool idle = !g_running;
    const char* busy = "Close the game first.";
    switch (g_page) {
    case kPageMain:
        rows.push_back({"CONTINUE CAMPAIGN", "", "CONTINUE", idle ? "Start the game and load your checkpoint." : busy, nullptr,
                        []() { g_startContinue = true; }, idle});
        rows.push_back({"NEW CAMPAIGN", "", "NEW CAMPAIGN", idle ? "Start the story from the beginning on a difficulty you choose." : busy,
                        nullptr, []() { Go(kPageDifficulty); }, idle});
        rows.push_back({"SAVE SLOTS", "", "SAVE SLOTS", "Keep copies of your checkpoint and bring them back.", nullptr, []() { Go(kPageSlots); }});
        rows.push_back({"MOD SETTINGS", "", "MOD SETTINGS", "FOV, lens, supersampling, mouse trap, screenshots.", nullptr, []() { Go(kPageMod); }});
        rows.push_back({"GAME SETTINGS", "", "GAME SETTINGS", "Resolution, DirectX 10 and the graphics options.", nullptr, []() { Go(kPageGame); }});
        rows.push_back({"QUIT", "", "QUIT", "Close the launcher.", nullptr, []() { PostQuitMessage(0); }});
        break;
    case kPageDifficulty: {
        static const char* kDesc[3] = {"For players new to shooters.", "For players who know how to take cover and aim.",
                                       "The hardest. The game unlocks it once you finish the campaign."};
        for (int i = 0; i < 3; ++i) {
            rows.push_back({Upper(kDifficulties[i]), "", Upper(kDifficulties[i]), idle ? kDesc[i] : busy, nullptr, [i]() {
                g_difficulty = i;
                OpenDialog("NEW CAMPAIGN", std::string("Start a new campaign on ") + kDifficulties[i] +
                               "? The game overwrites its checkpoint. Your current campaign is kept as a save slot first.",
                           {{"ACCEPT", []() { g_startNew = true; }}, {"CANCEL", nullptr}});
            }, idle});
        }
        break;
    }
    case kPageSlots: {
        if (g_profiles.empty()) {
            rows.push_back({"NO PROFILE FOUND", "", "NO PROFILE", "Save slots need the XLiveEmu profile in Binaries\\XLiveEmu.", nullptr, nullptr, false});
            break;
        }
        const char* restoreDesc = "Enter: restore or delete. Restoring also brings back that time's options and unlocks.";
        rows.push_back({"SAVE CURRENT CAMPAIGN", "", "SAVE", idle ? "Store the checkpoint the profile holds now as a new slot." : busy, nullptr, []() {
            OpenInput("SAVE CURRENT CAMPAIGN", "Name for this slot:", ("Campaign " + Now(false)).c_str(), [](const char* name) {
                if (SaveSlot(name[0] ? name : "Campaign", false)) Status(false, "Saved \"%s\"", name);
            });
        }, idle});
        if (g_profiles.size() > 1)
            rows.push_back({"PROFILE", Upper(g_profiles[g_profile]), "PROFILE", "Which XLiveEmu profile the slots are for.",
                            [](int d) { Cycle(g_profile, d, (int)g_profiles.size()); }, nullptr});
        for (int i = 0; i < (int)g_slots.size(); ++i) {
            const Slot& s = g_slots[i];
            bool current = SameFiles(ProfileDir(), s.dir);
            rows.push_back({s.saved + "   " + s.name, current ? "CURRENT" : "", "SLOT", idle ? restoreDesc : busy, nullptr, [i]() {
                const Slot& s = g_slots[i];
                OpenDialog("SAVE SLOT", s.name + " (" + s.saved + ")",
                           {{"RESTORE", [i]() { RestoreSlot(g_slots[i]); }},
                            {"DELETE", [i]() {
                                 std::string name = g_slots[i].name;
                                 OpenDialog("DELETE SLOT", "Delete \"" + name + "\"? This cannot be undone.",
                                            {{"DELETE", [i]() { DeleteSlot(g_slots[i]); }}, {"CANCEL", nullptr}});
                             }},
                            {"CANCEL", nullptr}});
            }, idle});
        }
        if (g_slots.empty()) rows.push_back({"NO SLOTS YET", "", "SAVE SLOTS", "Save the current campaign to make the first one.", nullptr, nullptr, false});
        break;
    }
    case kPageMod: {
        auto ch = [](std::function<void(int)> f) { return [f](int d) { f(d); SaveMod(); }; };
        rows.push_back({"FIELD OF VIEW", g_mod.fov ? std::to_string(g_mod.fov) : "DEFAULT", "FOV",
                        "The normal camera's FOV, 60 to 159 (the game uses 80). Shift: steps of 10.",
                        ch([](int d) { StepFov(g_mod.fov, d, 60, 159, 80); }), nullptr});
        rows.push_back({"AIM FOV", g_mod.aimFov ? std::to_string(g_mod.aimFov) : "DEFAULT", "AIM FOV",
                        "How far aiming zooms in, 30 to 120 (the game uses 40). Sniper scopes keep their zoom.",
                        ch([](int d) { StepFov(g_mod.aimFov, d, 30, 120, 40); }), nullptr});
        static const char* kLens[] = {"OFF", "AUTO", "PANINI", "STEREOGRAPHIC", "FISHEYE", "FISHEYE CIRCLE"};
        rows.push_back({"LENS", kLens[g_mod.lens % 6], "LENS",
                        g_mod.lens == 5 ? "The fisheye zoomed out: at FOV 159 all that is rendered shows, as a circle."
                                        : "Wide FOVs without stretched edges. Auto: straight up to 90, Panini by 160.",
                        ch([](int d) { Cycle(g_mod.lens, d, 6); }), nullptr});
        int ss = 0;
        for (int i = 0; i < kSupersampleStepCount; ++i)
            if (fabsf(kSupersampleSteps[i] - g_mod.supersample) < fabsf(kSupersampleSteps[ss] - g_mod.supersample)) ss = i;
        char ssv[16], ssd[128];
        if (ss == 0) lstrcpyA(ssv, "OFF");
        else sprintf_s(ssv, "%gX", kSupersampleSteps[ss]);
        int vw, vh;
        VirtualSize(kSupersampleSteps[ss], &vw, &vh);
        if (ss == 0) lstrcpyA(ssd, "Renders more pixels and averages them down: smoother edges, sharper detail.");
        else sprintf_s(ssd, "Renders %d x %d and averages it down to %d x %d.", vw, vh, g_game.resX, g_game.resY);
        rows.push_back({"SUPERSAMPLING", ssv, "SUPERSAMPLING", ssd, ch([](int d) {
            int i = 0;
            for (int k = 0; k < kSupersampleStepCount; ++k)
                if (fabsf(kSupersampleSteps[k] - g_mod.supersample) < fabsf(kSupersampleSteps[i] - g_mod.supersample)) i = k;
            i += d;
            if (i < 0) i = 0;
            if (i >= kSupersampleStepCount) i = kSupersampleStepCount - 1;
            g_mod.supersample = kSupersampleSteps[i];
        }), nullptr});
        rows.push_back({"START SHOULDER", g_mod.shoulderLeft ? "LEFT" : "RIGHT", "SHOULDER", "The shoulder the camera starts on. Tab swaps it in game.",
                        ch([](int) { g_mod.shoulderLeft = !g_mod.shoulderLeft; }), nullptr});
        static const char* kTrap[] = {"OFF", "GAMEPLAY", "ALWAYS"};
        rows.push_back({"MOUSE TRAP", kTrap[g_mod.mouseTrap % 3], "MOUSE TRAP", "Keeps the cursor on the game's monitor: in gameplay, or in menus too.",
                        ch([](int d) { Cycle(g_mod.mouseTrap, d, 3); }), nullptr});
        rows.push_back({"SCREENSHOT FORMAT", g_mod.jpg ? "JPEG" : "PNG", "SCREENSHOTS", "JPEG is small and quick to write, PNG is lossless.",
                        ch([](int) { g_mod.jpg = !g_mod.jpg; }), nullptr});
        rows.push_back({"SCREENSHOT SIZE", g_mod.full ? "FULL" : "SCREEN", "SCREENSHOTS", "Full: the supersampled size the game renders at. Screen: your screen's.",
                        ch([](int) { g_mod.full = !g_mod.full; }), nullptr});
        rows.push_back({"SCREENSHOT KEY", Upper(g_mod.key), "SCREENSHOTS", "Enter, then press the key. Pick one the game does not use.", nullptr, []() {
            CaptureKey("SCREENSHOT KEY", g_mod.key, sizeof(g_mod.key), g_mod.menuKey, "settings menu key");
        }});
        rows.push_back({"JPEG QUALITY", std::to_string(g_mod.quality), "SCREENSHOTS", "1 to 100. 95 is visually lossless.", ch([](int d) {
            g_mod.quality += d * (ImGui::GetIO().KeyShift ? 1 : 5);
            if (g_mod.quality < 1) g_mod.quality = 1;
            if (g_mod.quality > 100) g_mod.quality = 100;
        }), nullptr});
        std::string dir = g_mod.dir[0] ? g_mod.dir : "BINARIES FOLDER";
        if (dir.size() > 26) dir = "..." + dir.substr(dir.size() - 23);
        rows.push_back({"SCREENSHOT FOLDER", dir, "SCREENSHOTS", "Enter: browse, type or open it. Empty: the Binaries folder.", nullptr, []() {
            OpenDialog("SCREENSHOT FOLDER", ShotFolder(), {
                {"BROWSE", []() {
                    std::string chosen;
                    if (BrowseFolder(g_hwnd, &chosen)) {
                        lstrcpynA(g_mod.dir, chosen.c_str(), sizeof(g_mod.dir));
                        SaveMod();
                        Status(false, "Screenshots go to %s", chosen.c_str());
                    }
                }},
                {"TYPE", []() {
                    OpenInput("SCREENSHOT FOLDER", "Folder (created if missing; empty = the Binaries folder; %USERPROFILE% and the like work):",
                              g_mod.dir, [](const char* v) {
                                  lstrcpynA(g_mod.dir, v, sizeof(g_mod.dir));
                                  SaveMod();
                              });
                }},
                {"OPEN", []() {
                    std::string f = ShotFolder();
                    SHCreateDirectoryExA(nullptr, f.c_str(), nullptr);
                    ShellExecuteA(g_hwnd, "open", f.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                }},
                {"CANCEL", nullptr}});
        }});
        rows.push_back({"STATS WINDOW", OnOff(g_mod.stats), "STATS", "Lens, supersampling and frame rate in a corner of the game. Drag it in the F8 menu.",
                        ch([](int) { g_mod.stats = !g_mod.stats; }), nullptr});
        rows.push_back({"SETTINGS MENU KEY", Upper(g_mod.menuKey), "MENU KEY", "Enter, then press the key that opens the settings menu in game.", nullptr, []() {
            CaptureKey("SETTINGS MENU KEY", g_mod.menuKey, sizeof(g_mod.menuKey), g_mod.key, "screenshot key");
        }});
        rows.push_back({"INTRO VIDEOS", g_mod.skipIntros ? "SKIP" : "PLAY", "INTRO VIDEOS", "The ESRB, Epic and Microsoft videos at startup. Loading screens still play.",
                        ch([](int) { g_mod.skipIntros = !g_mod.skipIntros; }), nullptr});
        break;
    }
    case kPageGame: {
        auto ch = [](std::function<void(int)> f) { return [f](int d) { f(d); SaveGame(); }; };
        char res[32];
        sprintf_s(res, "%d X %d", g_game.resX, g_game.resY);
        rows.push_back({"RESOLUTION", res, "RESOLUTION", "The display mode the game starts in.", ch([](int d) {
            if (g_modes.empty()) return;
            int i = 0;
            for (int k = 0; k < (int)g_modes.size(); ++k)
                if (g_modes[k].w == g_game.resX && g_modes[k].h == g_game.resY) i = k;
            i = (i - d + (int)g_modes.size()) % (int)g_modes.size();  // the list runs largest first
            g_game.resX = g_modes[i].w;
            g_game.resY = g_modes[i].h;
        }), nullptr});
        auto flag = [&](const char* label, bool& v, const char* title, const char* desc) {
            bool* p = &v;
            rows.push_back({label, OnOff(v), title, desc, ch([p](int) { *p = !*p; }), nullptr});
        };
        static const char* kDisplay[] = {"FULLSCREEN", "WINDOW", "BORDERLESS WINDOW"};
        static const char* kDisplayDesc[] = {"The game takes over the display at the resolution above.",
                                             "A window with a frame and title bar.",
                                             "A window without frame, centred on the monitor (black around it if smaller)."};
        int display = g_game.fullscreen ? 0 : g_mod.borderless ? 2 : 1;
        rows.push_back({"DISPLAY MODE", kDisplay[display], "DISPLAY MODE", kDisplayDesc[display], [](int d) {
            int k = g_game.fullscreen ? 0 : g_mod.borderless ? 2 : 1;
            Cycle(k, d, 3);
            g_game.fullscreen = k == 0;
            if (k) g_mod.borderless = k == 2;
            SaveGame();
            SaveMod();
        }, nullptr});
        flag("VSYNC", g_game.vsync, "VSYNC", "Waits for the display's refresh: no tearing.");
        flag("DIRECTX 10", g_game.d3d10, "DIRECTX 10", "The mod's menu, lens, supersampling and screenshots need DirectX 10.");
        static const int kMsaa[] = {1, 2, 4, 8};
        static const char* kMsaaNames[] = {"OFF", "2X", "4X", "8X"};
        int m = 0;
        for (int i = 0; i < 4; ++i)
            if (kMsaa[i] <= g_game.msaa) m = i;
        rows.push_back({"ANTIALIASING", kMsaaNames[m], "ANTIALIASING", "The game's MSAA. With supersampling it matters less.", ch([](int d) {
            int k = 0;
            for (int i = 0; i < 4; ++i)
                if (kMsaa[i] <= g_game.msaa) k = i;
            Cycle(k, d, 4);
            g_game.msaa = kMsaa[k];
        }), nullptr});
        static const int kAniso[] = {1, 2, 4, 8, 16};
        static const char* kAnisoNames[] = {"OFF", "2X", "4X", "8X", "16X"};
        int a = 0;
        for (int i = 0; i < 5; ++i)
            if (kAniso[i] <= g_game.aniso) a = i;
        rows.push_back({"ANISOTROPIC FILTERING", kAnisoNames[a], "TEXTURES", "Sharper textures at an angle.", ch([](int d) {
            int k = 0;
            for (int i = 0; i < 5; ++i)
                if (kAniso[i] <= g_game.aniso) k = i;
            Cycle(k, d, 5);
            g_game.aniso = kAniso[k];
        }), nullptr});
        static const char* kDetail[] = {"LOW", "MEDIUM", "HIGH"};
        rows.push_back({"DETAIL", kDetail[g_game.detail % 3], "DETAIL", "The game's detail level.", ch([](int d) { Cycle(g_game.detail, d, 3); }), nullptr});
        flag("DYNAMIC SHADOWS", g_game.shadows, "SHADOWS", "Shadows from moving lights and characters.");
        flag("BLOOM", g_game.bloom, "BLOOM", "Glow around bright light.");
        flag("MOTION BLUR", g_game.motionBlur, "MOTION BLUR", "Blur when the camera turns quickly.");
        flag("DEPTH OF FIELD", g_game.dof, "DEPTH OF FIELD", "Blurs what is far away.");
        flag("LENS FLARES", g_game.lensFlares, "LENS FLARES", "Flares from bright lights.");
        break;
    }
    default:
        break;
    }
    return rows;
}

// ---- drawing and input -----------------------------------------------------------------------

static void KeyHint(ImDrawList* dl, float& x, float y, const char* key, const char* what, Page back = kPageCount)
{
    float kw = TextWidth(key, 13, 0) + 10;
    dl->AddRect(P(x, y - 1), P(x + kw, y + 17), Rgba(1, 1, 1, 0.7f), 2 * g_u, ImDrawFlags_None, 1.f);
    Text(dl, x + 5, y, key, 13, Rgba(1, 1, 1, 0.9f));
    Text(dl, x + kw + 6, y + 1, what, 14, Rgba(1, 1, 1, 0.75f), 2.5f);
    float w = kw + 6 + TextWidth(what, 14, 2.5f);
    if (back != kPageCount && ImGui::IsMouseHoveringRect(P(x, y - 2), P(x + w, y + 18)) && ImGui::IsMouseClicked(0)) Go(back);
    x += w + 22;
}

static void DrawDialog(ImDrawList* dl)
{
    Dialog& d = g_dialog;
    float x0 = 300, x1 = 980, y0 = 250;
    float textY = y0 + 52;
    // Wrap the text at the box's width.
    std::vector<std::string> lines;
    std::string line, word;
    std::string text = d.text + " ";
    for (char c : text) {
        if (c != ' ') { word += c; continue; }
        if (TextWidth((line + " " + word).c_str(), 15, 0) > x1 - x0 - 60 && !line.empty()) { lines.push_back(line); line.clear(); }
        line += (line.empty() ? "" : " ") + word;
        word.clear();
    }
    if (!line.empty()) lines.push_back(line);
    float y1 = textY + lines.size() * 22 + (d.input ? 44 : 0) + 56;
    dl->AddRectFilled(ImVec2(0, 0), ImGui::GetIO().DisplaySize, Rgba(0, 0, 0, 0.45f));
    dl->AddRectFilled(P(x0, y0), P(x1, y1), Rgba(0.05f, 0.05f, 0.05f, 0.92f));
    dl->AddRect(P(x0, y0), P(x1, y1), Rgba(1, 1, 1, 0.25f));
    dl->AddLine(P(x0 + 20, y0 + 40), P(x1 - 20, y0 + 40), Rgba(0.6f, 0.08f, 0.06f, 0.9f), 2 * g_u);
    Text(dl, x0 + 24, y0 + 12, d.title.c_str(), 20, Rgba(1, 1, 1, 1), 3);
    for (size_t i = 0; i < lines.size(); ++i) Text(dl, x0 + 30, textY + i * 22, lines[i].c_str(), 15, Rgba(1, 1, 1, 0.9f));
    float by = y1 - 40;
    bool pressed = false;
    bool fresh = ImGui::GetFrameCount() == d.openedFrame;  // the key or click that opened it
    if (d.input) {
        ImGui::SetCursorScreenPos(P(x0 + 30, by - 46));
        ImGui::SetNextItemWidth((x1 - x0 - 60) * g_u);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.15f, 0.15f, 0.15f, 1));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
        if (d.focus) { ImGui::SetKeyboardFocusHere(); d.focus = false; }
        if (ImGui::InputText("##edit", d.buf, sizeof(d.buf), ImGuiInputTextFlags_EnterReturnsTrue)) { d.sel = 0; pressed = true; }
        ImGui::PopStyleColor(2);
    }
    // Buttons, right-aligned like the game's.
    float bx = x1 - 30;
    for (int i = (int)d.buttons.size() - 1; i >= 0; --i) {
        float w = TextWidth(d.buttons[i].label.c_str(), 15, 3) + 28;
        bx -= w;
        bool hot = i == d.sel;
        ImVec2 a = P(bx, by), b = P(bx + w, by + 26);
        if (ImGui::IsMouseHoveringRect(a, b)) {
            if (ImGui::GetIO().MouseDelta.x || ImGui::GetIO().MouseDelta.y) d.sel = i;
            if (ImGui::IsMouseClicked(0) && !fresh) { d.sel = i; pressed = true; }
        }
        dl->AddRectFilled(a, b, hot ? Rgba(0.85f, 0.85f, 0.85f, 0.35f) : Rgba(0, 0, 0, 0.6f));
        Text(dl, bx + 14, by + 5, d.buttons[i].label.c_str(), 15, Rgba(1, 1, 1, hot ? 1.f : 0.7f), 3);
        bx -= 12;
    }
    if (!fresh && (!d.input || !ImGui::GetIO().WantTextInput)) {
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) d.sel = (d.sel + (int)d.buttons.size() - 1) % (int)d.buttons.size();
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) d.sel = (d.sel + 1) % (int)d.buttons.size();
        if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)) pressed = true;
    }
    if (!fresh && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) { d.open = false; return; }
    if (pressed) {
        std::function<void()> f = d.buttons[d.sel].press;
        d.open = false;
        if (f) f();  // may open the next dialog
    }
}

static void DrawLauncher()
{
    ImGuiIO& io = ImGui::GetIO();
    ImVec2 size = io.DisplaySize;
    g_u = fminf(size.x / 1280.f, size.y / 720.f);
    g_org = ImVec2((size.x - 1280 * g_u) / 2, (size.y - 720 * g_u) / 2);
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(size);
    ImGui::Begin("launcher", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoMove |
                                          ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoScrollWithMouse);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    DrawBackground(dl, size);

    // Heading, as "MAIN MENU" in the game.
    Text(dl, 105, 60, kPageNames[g_page], 26, Rgba(1, 1, 1, 0.95f), 6);
    Text(dl, 107, 96, "ROWE FOR GEARS OF WAR (2007)", 12, Rgba(0.85f, 0.25f, 0.2f, 0.9f), 4);

    std::vector<Row> rows = BuildRows();
    int& sel = g_sel[g_page];
    int& top = g_top[g_page];
    if (sel >= (int)rows.size()) sel = (int)rows.size() - 1;
    if (sel < 0) sel = 0;
    bool settings = g_page == kPageMod || g_page == kPageGame || g_page == kPageSlots;
    float x0 = settings ? 105 : 360, x1 = settings ? 745 : 720, y0 = settings ? 140 : 190;
    const float rowH = 30, gap = 4;
    const int maxRows = settings ? 13 : 10;
    if (sel < top) top = sel;
    if (sel >= top + maxRows) top = sel - maxRows + 1;
    int shown = (int)rows.size() - top < maxRows ? (int)rows.size() - top : maxRows;

    // The bracket frame around the entries.
    float fy0 = y0 - 14, fy1 = y0 + shown * (rowH + gap) + 10;
    ImU32 frame = Rgba(1, 1, 1, 0.3f);
    dl->AddLine(P(x0 - 18, fy0), P(x1 + 18, fy0), frame);
    dl->AddLine(P(x0 - 18, fy1), P(x1 + 18, fy1), frame);
    dl->AddLine(P(x0 - 18, fy0), P(x0 - 18, fy0 + 24), frame);
    dl->AddLine(P(x1 + 18, fy1), P(x1 + 18, fy1 - 24), frame);
    dl->AddLine(P(x1 + 18, fy0), P(x1 + 18, fy0 + 8), frame);
    dl->AddLine(P(x0 - 18, fy1), P(x0 - 18, fy1 - 8), frame);

    bool dialog = g_dialog.open;
    bool moved = io.MouseDelta.x != 0 || io.MouseDelta.y != 0;
    for (int k = 0; k < shown; ++k) {
        int i = top + k;
        const Row& r = rows[i];
        float y = y0 + k * (rowH + gap);
        ImVec2 a = P(x0, y), b = P(x1, y + rowH);
        bool hot = i == sel;
        bool hover = !dialog && ImGui::IsMouseHoveringRect(a, b);
        if (hover && moved) sel = i, hot = true;
        dl->AddRectFilled(a, b, hot ? Rgba(0.85f, 0.85f, 0.85f, 0.32f) : Rgba(0, 0, 0, 0.55f));
        dl->AddLine(a, ImVec2(b.x, a.y), Rgba(1, 1, 1, hot ? 0.25f : 0.06f));
        float alpha = !r.enabled ? 0.35f : hot ? 1.f : 0.72f;
        if (settings) {
            Text(dl, x0 + 14, y + 6, r.label.c_str(), 16, Rgba(1, 1, 1, alpha), 3);
            if (!r.value.empty()) {
                bool arrows = hot && r.change;
                Text(dl, x1 - (arrows ? 34 : 14), y + 6, r.value.c_str(), 16, Rgba(1, 1, 1, alpha), 2.5f, kRight);
                if (arrows) {
                    float vw = TextWidth(r.value.c_str(), 16, 2.5f);
                    Text(dl, x1 - 34 - vw - 22, y + 5, "<", 17, Rgba(0.9f, 0.3f, 0.25f, 1));
                    Text(dl, x1 - 22, y + 5, ">", 17, Rgba(0.9f, 0.3f, 0.25f, 1));
                    if (hover && ImGui::IsMouseClicked(0)) {
                        float mx = (io.MousePos.x - g_org.x) / g_u;
                        if (mx < x1 - 34 - vw - 4) r.change(-1);
                        else r.change(+1);
                        continue;
                    }
                }
            }
        } else {
            Text(dl, x1 - 16, y + 6, r.label.c_str(), 17, Rgba(1, 1, 1, alpha), 3.5f, kRight);  // right-aligned, as the game's
        }
        if (hover && r.enabled && ImGui::IsMouseClicked(0)) {
            if (r.activate) r.activate();
            else if (r.change) r.change(+1);
        }
        if (hover && r.enabled && r.change && io.MouseWheel != 0) r.change(io.MouseWheel > 0 ? 1 : -1);
    }
    if (top > 0) Text(dl, x1 + 4, fy0 - 18, "^", 14, Rgba(1, 1, 1, 0.6f));
    if (top + shown < (int)rows.size()) Text(dl, x1 + 4, fy1 + 2, "v", 14, Rgba(1, 1, 1, 0.6f));

    // Keyboard, as in the game's menus.
    if (!dialog && !rows.empty()) {
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) sel = (sel + (int)rows.size() - 1) % (int)rows.size();
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) sel = (sel + 1) % (int)rows.size();
        const Row& r = rows[sel];
        if (r.enabled && r.change) {
            if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) r.change(-1);
            if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) r.change(+1);
        }
        if (r.enabled && (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false))) {
            if (r.activate) r.activate();
            else if (r.change) r.change(+1);
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_Backspace, false)) {
            if (g_page != kPageMain) Go(kPageMain);
        }
    }

    // The highlighted entry, large at the bottom right, as "HOST OR JOIN" in the game.
    if (!rows.empty()) {
        const Row& r = rows[sel];
        Text(dl, 1180, 548, r.title.c_str(), 40, Rgba(1, 1, 1, 0.92f), 8, kRight);
        Text(dl, 1180, 600, r.desc.c_str(), 14, Rgba(1, 1, 1, 0.8f), 1, kRight);
    }

    // Status and key hints, bottom left.
    if (!g_status.empty())
        Text(dl, 105, 622, g_status.c_str(), 14, g_statusError ? Rgba(1, 0.45f, 0.35f, 1) : Rgba(0.75f, 0.9f, 0.75f, 1));
    else if (g_running)
        Text(dl, 105, 622, "The game is running.", 14, Rgba(1, 0.6f, 0.4f, 1));
    std::string profile = g_profiles.empty() ? "-" : Upper(g_profiles[g_profile]);
    Text(dl, 105, 646, ("GAMER PROFILE: " + profile).c_str(), 13, Rgba(1, 1, 1, 0.6f), 3);
    float hx = 105;
    KeyHint(dl, hx, 670, "Enter", "ACCEPT");
    if (g_page != kPageMain) KeyHint(dl, hx, 670, "Esc", "BACK", kPageMain);
    if (settings) KeyHint(dl, hx, 670, "< >", "CHANGE");

    if (dialog) DrawDialog(ImGui::GetForegroundDrawList());
    ImGui::End();
}

// ---- Direct3D 9 window -----------------------------------------------------------------------

static IDirect3D9* g_d3d;
static D3DPRESENT_PARAMETERS g_pp;
static UINT g_resizeW, g_resizeH;

static void ResetDevice()
{
    ImGui_ImplDX9_InvalidateDeviceObjects();
    if (g_dev->Reset(&g_pp) == D3DERR_INVALIDCALL) return;
    ImGui_ImplDX9_CreateDeviceObjects();
}

static LRESULT WINAPI WndProc(HWND h, UINT msg, WPARAM w, LPARAM l)
{
    // A key being captured (CaptureKey): its first press, not Enter's repeat or a held key.
    if (g_capture && (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) && !(l & 0x40000000)) {
        UINT vk = (UINT)w;
        if (vk == VK_ESCAPE) {
            g_capture = nullptr;
            g_dialog.open = false;
            return 0;
        }
        if (vk != VK_RETURN && vk != VK_SHIFT && vk != VK_CONTROL && vk != VK_MENU && vk != VK_LWIN && vk != VK_RWIN) {
            std::function<void(UINT)> f = g_capture;
            f(vk);
            return 0;
        }
    }
    if (ImGui_ImplWin32_WndProcHandler(h, msg, w, l)) return 1;
    if (msg == WM_SIZE && w != SIZE_MINIMIZED) {
        g_resizeW = LOWORD(l);
        g_resizeH = HIWORD(l);
        return 0;
    }
    if (msg == WM_SYSCOMMAND && (w & 0xFFF0) == SC_KEYMENU) return 0;
    if (msg == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(h, msg, w, l);
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE, LPSTR cmdLine, int)
{
    // --tab mod|game opens on that page; --continue and --new casual|hardcore|insane start the game
    // at once, as the buttons do (for desktop shortcuts).
    if (strstr(cmdLine, "--tab mod")) g_page = kPageMod;
    else if (strstr(cmdLine, "--tab game")) g_page = kPageGame;
    bool quickContinue = strstr(cmdLine, "--continue") != nullptr, quickNew = false;
    if (const char* n = strstr(cmdLine, "--new")) {
        quickNew = true;
        for (int i = 0; i < 3; ++i)
            if (StrStrIA(n, kDifficulties[i])) g_difficulty = i;
    }
    char exe[MAX_PATH];
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    *(strrchr(exe, '\\') + 1) = 0;
    g_bin = exe;
    if (!Exists(g_bin + "Startup.exe")) {
        MessageBoxA(nullptr, "Put rowe_launcher.exe into the game's Binaries folder (next to Startup.exe).",
                    "ROWE for Gears of War (2007)", MB_ICONERROR);
        return 1;
    }
    MigrateGowfovFiles(g_bin.c_str());
    g_modIni = g_bin + "rowe.ini";
    char docs[MAX_PATH] = "";
    SHGetFolderPathA(nullptr, CSIDL_PERSONAL, nullptr, 0, docs);
    g_gameIni = std::string(docs) + "\\My Games\\Gears of War for Windows\\WarGame\\Config\\WarEngineUserSettings.ini";
    LoadMod();
    LoadGame();
    EnumerateModes();
    FindProfiles();
    LoadSlots();
    if ((quickContinue || quickNew) && !GameRunning()) {
        if (Launch(quickNew, g_difficulty)) return 0;
        MessageBoxA(nullptr, g_status.c_str(), "ROWE for Gears of War (2007)", MB_ICONERROR);
    }
    if (!Exists(g_gameIni)) Status(true, "The game's settings file is missing: start the game once first");
    else if (!Exists(g_bin + "d3d9.dll")) Status(true, "The mod (d3d9.dll) is not in this folder");

    WNDCLASSEXA wc = {sizeof(wc), CS_CLASSDC, WndProc, 0, 0, inst, nullptr, LoadCursor(nullptr, IDC_ARROW), nullptr, nullptr, "rowe_launcher", nullptr};
    HICON iconBig = nullptr, iconSmall = nullptr;
    ExtractIconExA((g_bin + "WarGame-G4WLive.exe").c_str(), 0, &iconBig, &iconSmall, 1);
    wc.hIcon = iconBig;
    wc.hIconSm = iconSmall;
    RegisterClassExA(&wc);
    HDC screen = GetDC(nullptr);
    float scale = GetDeviceCaps(screen, LOGPIXELSY) / 96.f;
    ReleaseDC(nullptr, screen);
    g_px = floorf(20.f * scale + 0.5f);
    RECT work;
    SystemParametersInfoA(SPI_GETWORKAREA, 0, &work, 0);
    // A 1280x720 client area (the menu's design size), smaller if the screen needs it.
    float fit = fminf(1.f, fminf((work.right - work.left) * 0.9f / 1280.f, (work.bottom - work.top) * 0.9f / 720.f));
    RECT wr = {0, 0, (LONG)(1280 * fit), (LONG)(720 * fit)};
    AdjustWindowRect(&wr, WS_OVERLAPPEDWINDOW, FALSE);
    int ww = wr.right - wr.left, wh = wr.bottom - wr.top;
    HWND hwnd = CreateWindowExA(0, wc.lpszClassName, "ROWE for Gears of War (2007) - launcher", WS_OVERLAPPEDWINDOW,
                                work.left + (work.right - work.left - ww) / 2, work.top + (work.bottom - work.top - wh) / 2, ww, wh,
                                nullptr, nullptr, inst, nullptr);
    g_hwnd = hwnd;
    BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, 20, &dark, sizeof(dark));  // DWMWA_USE_IMMERSIVE_DARK_MODE

    g_d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!g_d3d) { MessageBoxA(hwnd, "Direct3D 9 is not available.", "launcher", MB_ICONERROR); return 1; }
    g_pp.Windowed = TRUE;
    g_pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    g_pp.BackBufferFormat = D3DFMT_UNKNOWN;
    g_pp.PresentationInterval = D3DPRESENT_INTERVAL_ONE;
    if (FAILED(g_d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd, D3DCREATE_HARDWARE_VERTEXPROCESSING, &g_pp, &g_dev))) {
        MessageBoxA(hwnd, "Could not create a Direct3D 9 device.", "launcher", MB_ICONERROR);
        return 1;
    }
    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    ImFontConfig cfg;
    cfg.FontDataOwnedByAtlas = false;
    g_font = io.Fonts->AddFontFromMemoryTTF((void*)g_shareTechMono, (int)g_shareTechMonoSize, g_px, &cfg);
    LoadBackground();
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX9_Init(g_dev);

    DWORD lastCheck = 0;
    bool done = false;
    while (!done) {
        MSG m;
        while (PeekMessageA(&m, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&m);
            DispatchMessageA(&m);
            if (m.message == WM_QUIT) done = true;
        }
        if (done) break;
        if (GetTickCount() - lastCheck > 1000) {
            lastCheck = GetTickCount();
            g_running = GameRunning();
        }
        if (g_resizeW && g_resizeH) {
            g_pp.BackBufferWidth = g_resizeW;
            g_pp.BackBufferHeight = g_resizeH;
            g_resizeW = g_resizeH = 0;
            ResetDevice();
        }
        ImGui_ImplDX9_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        DrawLauncher();
        ImGui::EndFrame();
        g_dev->SetRenderState(D3DRS_ZENABLE, FALSE);
        g_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        g_dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
        g_dev->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(0, 0, 0), 1.f, 0);
        if (SUCCEEDED(g_dev->BeginScene())) {
            ImGui::Render();
            ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
            g_dev->EndScene();
        }
        if (g_dev->Present(nullptr, nullptr, nullptr, nullptr) == D3DERR_DEVICELOST && g_dev->TestCooperativeLevel() == D3DERR_DEVICENOTRESET)
            ResetDevice();
        if ((g_startContinue || g_startNew) && !g_running) {
            if (Launch(g_startNew, g_difficulty)) done = true;
        }
        g_startContinue = g_startNew = false;
    }
    ImGui_ImplDX9_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    if (g_bg) g_bg->Release();
    g_dev->Release();
    g_d3d->Release();
    DestroyWindow(hwnd);
    return 0;
}
