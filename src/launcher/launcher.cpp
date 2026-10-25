// gowfov_launcher.exe: set the mod's and the game's settings before the game starts, keep save
// slots, and start the game straight into "continue" or a new campaign at a chosen difficulty.
// It lives in the game's Binaries folder (next to Startup.exe and the mod's d3d9.dll) and is
// drawn with Dear ImGui (third_party/imgui, MIT) on Direct3D 9, in the F8 menu's colours.
//
// Save slots: Gears of War keeps its one campaign checkpoint in the player profile (with the
// XLiveEmu GFWL replacement: Binaries\XLiveEmu\<profile>\Offline\Title1-3.dat, encrypted for the
// Windows user), and a new campaign overwrites it. A slot is a copy of those files in
// Binaries\gowfov_saves\<id>\ with a slot.ini (name, date). Restoring copies them back, so it also
// brings back the profile's other contents from that time (options, unlocked chapters). Before a
// new campaign or a restore the current files are kept as a slot unless a slot already has them.
//
// Starting: the launcher writes gowfov_launch.txt for the mod (launch.cpp), which walks the
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
#include "imgui.h"
#include "backends/imgui_impl_dx9.h"
#include "backends/imgui_impl_win32.h"
#include "sharetechmono_ttf.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

static std::string g_bin;       // the game's Binaries folder (this exe's), with trailing backslash
static std::string g_modIni;    // gowfov.ini
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

// ---- the mod's settings (gowfov.ini; same keys and ranges as the F8 menu) --------------------

struct ModSettings {
    int fov, aimFov, lens, mouseTrap, quality;
    float supersample;
    bool shoulderLeft, skipIntros, jpg, full;
    char key[16], dir[MAX_PATH];
} g_mod;

static const float kSupersampleSteps[] = {1.f, 2.f, 2.25f, 3.f, 4.f, 5.f, 6.f, 8.f};

static void LoadMod()
{
    const char* s = "gowfov";
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
    lstrcpynA(g_mod.dir, IniGet(g_modIni, s, "screenshotdir", "").c_str(), sizeof(g_mod.dir));
}

static void SaveMod()
{
    const char* s = "gowfov";
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

static std::string SlotsRoot() { return g_bin + "gowfov_saves\\"; }

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
    std::string req = g_bin + "gowfov_launch.txt";
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

// ---- look ------------------------------------------------------------------------------------

static const ImVec4 kAccent = ImVec4(0.88f, 0.63f, 0.19f, 1.f);  // the F8 menu's amber

static void ApplyStyle(float px)
{
    ImGuiStyle& st = ImGui::GetStyle();
    st.WindowPadding = ImVec2(px, px * 0.8f);
    st.FramePadding = ImVec2(px * 0.4f, px * 0.25f);
    st.ItemSpacing = ImVec2(px * 0.5f, px * 0.35f);
    st.WindowRounding = st.FrameRounding = st.GrabRounding = st.TabRounding = 0;
    st.WindowBorderSize = 0;
    ImVec4* c = st.Colors;
    ImVec4 bg(0.06f, 0.06f, 0.08f, 1.f), panel(0.11f, 0.11f, 0.14f, 1.f);
    ImVec4 dim(kAccent.x * 0.45f, kAccent.y * 0.45f, kAccent.z * 0.45f, 1.f), mid(kAccent.x * 0.7f, kAccent.y * 0.7f, kAccent.z * 0.7f, 1.f);
    c[ImGuiCol_WindowBg] = bg;
    c[ImGuiCol_ChildBg] = panel;
    c[ImGuiCol_PopupBg] = panel;
    c[ImGuiCol_Text] = ImVec4(0.85f, 0.85f, 0.85f, 1.f);
    c[ImGuiCol_TextDisabled] = ImVec4(0.5f, 0.5f, 0.5f, 1.f);
    c[ImGuiCol_FrameBg] = ImVec4(0.16f, 0.16f, 0.2f, 1.f);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.22f, 0.2f, 0.18f, 1.f);
    c[ImGuiCol_FrameBgActive] = dim;
    c[ImGuiCol_Button] = ImVec4(0.18f, 0.16f, 0.14f, 1.f);
    c[ImGuiCol_ButtonHovered] = dim;
    c[ImGuiCol_ButtonActive] = mid;
    c[ImGuiCol_Header] = dim;
    c[ImGuiCol_HeaderHovered] = mid;
    c[ImGuiCol_HeaderActive] = kAccent;
    c[ImGuiCol_Tab] = ImVec4(0.14f, 0.13f, 0.12f, 1.f);
    c[ImGuiCol_TabHovered] = mid;
    c[ImGuiCol_TabSelected] = dim;
    c[ImGuiCol_SliderGrab] = kAccent;
    c[ImGuiCol_SliderGrabActive] = kAccent;
    c[ImGuiCol_CheckMark] = kAccent;
    c[ImGuiCol_CheckboxSelectedBg] = c[ImGuiCol_FrameBg];
    c[ImGuiCol_Separator] = dim;
    c[ImGuiCol_Border] = dim;
}

static void Note(const char* text)
{
    ImGui::SameLine();
    ImGui::TextDisabled("%s", text);
}

// ---- the window ------------------------------------------------------------------------------

static bool g_running;
static bool g_startNew, g_startContinue;
static int g_difficulty;
static char g_slotName[64];
static float g_px;
static int g_startTab = -1;  // --tab mod|game: open on that tab (once)

static void PlayTab()
{
    ImGui::TextColored(kAccent, "START");
    ImGui::BeginDisabled(g_running);
    if (ImGui::Button("  Continue campaign  ")) g_startContinue = true;
    Note("loads the checkpoint the profile holds now");
    ImGui::Spacing();
    if (ImGui::Button("  New campaign  ")) ImGui::OpenPopup("new");
    ImGui::SameLine();
    for (int i = 0; i < 3; ++i) {
        ImGui::SameLine();
        ImGui::RadioButton(kDifficulties[i], &g_difficulty, i);
    }
    if (g_difficulty == 2) Note("unlocked after finishing the campaign");
    if (ImGui::BeginPopupModal("new", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
        ImGui::Text("Start a new campaign on %s?", kDifficulties[g_difficulty]);
        ImGui::TextDisabled("The game overwrites its checkpoint. Your current campaign is kept\nas a save slot first, so you can go back to it.");
        if (ImGui::Button("  Start  ")) { g_startNew = true; ImGui::CloseCurrentPopup(); }
        ImGui::SameLine();
        if (ImGui::Button("  Cancel  ")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::EndDisabled();
    if (g_running) ImGui::TextColored(ImVec4(1, 0.5f, 0.4f, 1), "The game is running. Close it to start it from here or to change saves.");

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextColored(kAccent, "SAVE SLOTS");
    if (g_profiles.empty()) {
        ImGui::TextDisabled("No XLiveEmu profile found in %sXLiveEmu: save slots need it.", g_bin.c_str());
        return;
    }
    if (g_profiles.size() > 1) {
        ImGui::SetNextItemWidth(g_px * 12);
        if (ImGui::BeginCombo("profile", g_profiles[g_profile].c_str())) {
            for (int i = 0; i < (int)g_profiles.size(); ++i)
                if (ImGui::Selectable(g_profiles[i].c_str(), i == g_profile)) g_profile = i;
            ImGui::EndCombo();
        }
    }
    ImGui::BeginDisabled(g_running);
    ImGui::SetNextItemWidth(g_px * 16);
    ImGui::InputTextWithHint("##name", "name for the current campaign", g_slotName, sizeof(g_slotName));
    ImGui::SameLine();
    if (ImGui::Button("Save current campaign")) {
        std::string name = g_slotName[0] ? g_slotName : "Campaign " + Now(false);
        if (SaveSlot(name, false)) {
            Status(false, "Saved \"%s\"", name.c_str());
            g_slotName[0] = 0;
        }
    }
    ImGui::BeginChild("slots", ImVec2(0, -g_px * 1.8f), true);
    for (int i = 0; i < (int)g_slots.size(); ++i) {
        const Slot& s = g_slots[i];
        bool current = SameFiles(ProfileDir(), s.dir);
        char label[200];
        sprintf_s(label, "%-16s  %s%s##%d", s.saved.c_str(), s.name.c_str(), current ? "   (current)" : "", i);
        if (ImGui::Selectable(label, g_slotSel == i)) g_slotSel = i;
    }
    if (g_slots.empty()) ImGui::TextDisabled("No save slots yet.");
    ImGui::EndChild();
    bool sel = g_slotSel >= 0 && g_slotSel < (int)g_slots.size();
    ImGui::BeginDisabled(!sel);
    if (ImGui::Button("Restore selected")) ImGui::OpenPopup("restore");
    ImGui::SameLine();
    if (ImGui::Button("Delete selected")) ImGui::OpenPopup("delete");
    ImGui::EndDisabled();
    Note("(also restores that time's options and unlocks)");
    if (ImGui::BeginPopupModal("restore", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
        ImGui::Text("Restore \"%s\"?", sel ? g_slots[g_slotSel].name.c_str() : "");
        ImGui::TextDisabled("The current campaign is kept as a slot first.");
        if (ImGui::Button("  Restore  ") && sel) { RestoreSlot(g_slots[g_slotSel]); ImGui::CloseCurrentPopup(); }
        ImGui::SameLine();
        if (ImGui::Button("  Cancel  ")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopupModal("delete", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
        ImGui::Text("Delete \"%s\"? This cannot be undone.", sel ? g_slots[g_slotSel].name.c_str() : "");
        if (ImGui::Button("  Delete  ") && sel) { DeleteSlot(g_slots[g_slotSel]); g_slotSel = -1; ImGui::CloseCurrentPopup(); }
        ImGui::SameLine();
        if (ImGui::Button("  Cancel  ")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::EndDisabled();
}

static void ModTab()
{
    bool ch = false;
    float w = g_px * 14;
    ImGui::TextColored(kAccent, "VIEW");
    ImGui::SetNextItemWidth(w);
    ch |= ImGui::SliderInt("FOV", &g_mod.fov, 0, 159, g_mod.fov ? "%d" : "game default (80)");
    if (g_mod.fov > 0 && g_mod.fov < 60) g_mod.fov = 60;
    ImGui::SetNextItemWidth(w);
    ch |= ImGui::SliderInt("Aim FOV", &g_mod.aimFov, 0, 120, g_mod.aimFov ? "%d" : "game default (40)");
    if (g_mod.aimFov > 0 && g_mod.aimFov < 30) g_mod.aimFov = 30;
    static const char* kLens[] = {"off", "auto", "Panini", "stereographic", "fisheye"};
    ImGui::SetNextItemWidth(w);
    ch |= ImGui::Combo("Lens", &g_mod.lens, kLens, 5);
    ImGui::TextDisabled("  sniper scopes keep their zoom; the lens shows at most about 160 degrees");
    ImGui::Spacing();
    ImGui::TextColored(kAccent, "IMAGE");
    int ss = 0;
    for (int i = 0; i < 8; ++i)
        if (fabsf(kSupersampleSteps[i] - g_mod.supersample) < fabsf(kSupersampleSteps[ss] - g_mod.supersample)) ss = i;
    char ssLabel[32];
    if (ss == 0) lstrcpyA(ssLabel, "off");
    else sprintf_s(ssLabel, "%gx (%.2f per side)", kSupersampleSteps[ss], sqrtf(kSupersampleSteps[ss]));
    ImGui::SetNextItemWidth(w);
    if (ImGui::SliderInt("Supersampling", &ss, 0, 7, ssLabel)) { g_mod.supersample = kSupersampleSteps[ss]; ch = true; }
    ImGui::TextDisabled("  fullscreen only; renders this many times the pixels and averages them down");
    ImGui::Spacing();
    ImGui::TextColored(kAccent, "CAMERA AND MOUSE");
    int shoulder = g_mod.shoulderLeft ? 1 : 0;
    ch |= ImGui::RadioButton("start on the right shoulder", &shoulder, 0);
    ImGui::SameLine();
    ch |= ImGui::RadioButton("left shoulder", &shoulder, 1);
    g_mod.shoulderLeft = shoulder == 1;
    Note("(Tab swaps in game)");
    static const char* kTrap[] = {"off (the game's own)", "gameplay only", "gameplay and menus"};
    ImGui::SetNextItemWidth(w);
    ch |= ImGui::Combo("Mouse trap", &g_mod.mouseTrap, kTrap, 3);
    ImGui::Spacing();
    ImGui::TextColored(kAccent, "SCREENSHOTS");
    int fmt = g_mod.jpg ? 0 : 1, size = g_mod.full ? 0 : 1;
    ch |= ImGui::RadioButton("JPEG", &fmt, 0);
    ImGui::SameLine();
    ch |= ImGui::RadioButton("PNG", &fmt, 1);
    ImGui::SameLine(0, g_px * 2);
    ch |= ImGui::RadioButton("full supersampled size", &size, 0);
    ImGui::SameLine();
    ch |= ImGui::RadioButton("screen size", &size, 1);
    g_mod.jpg = fmt == 0;
    g_mod.full = size == 0;
    ImGui::SetNextItemWidth(g_px * 5);
    ch |= ImGui::InputText("key", g_mod.key, sizeof(g_mod.key));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(g_px * 6);
    ch |= ImGui::SliderInt("JPEG quality", &g_mod.quality, 1, 100);
    ImGui::SetNextItemWidth(-g_px * 4);
    ch |= ImGui::InputTextWithHint("folder", "empty: the Binaries folder; %USERPROFILE% etc. work", g_mod.dir, sizeof(g_mod.dir));
    ImGui::Spacing();
    ImGui::TextColored(kAccent, "STARTUP");
    ch |= ImGui::Checkbox("Skip the intro videos", &g_mod.skipIntros);
    if (ch) SaveMod();
}

static void GameTab()
{
    bool ch = false;
    float w = g_px * 14;
    ImGui::TextColored(kAccent, "DISPLAY");
    char cur[32];
    sprintf_s(cur, "%d x %d", g_game.resX, g_game.resY);
    ImGui::SetNextItemWidth(w);
    if (ImGui::BeginCombo("Resolution", cur)) {
        for (auto& m : g_modes) {
            char l[32];
            sprintf_s(l, "%d x %d", m.w, m.h);
            if (ImGui::Selectable(l, m.w == g_game.resX && m.h == g_game.resY)) { g_game.resX = m.w; g_game.resY = m.h; ch = true; }
        }
        ImGui::EndCombo();
    }
    ch |= ImGui::Checkbox("Fullscreen", &g_game.fullscreen);
    ImGui::SameLine(0, g_px * 2);
    ch |= ImGui::Checkbox("VSync", &g_game.vsync);
    ch |= ImGui::Checkbox("DirectX 10", &g_game.d3d10);
    if (!g_game.d3d10) Note("needed for the F8 menu, lens, supersampling and screenshots");
    ImGui::Spacing();
    ImGui::TextColored(kAccent, "QUALITY");
    static const int kMsaa[] = {1, 2, 4, 8};
    static const char* kMsaaNames[] = {"off", "2x", "4x", "8x"};
    int m = 0;
    for (int i = 0; i < 4; ++i)
        if (kMsaa[i] <= g_game.msaa) m = i;
    ImGui::SetNextItemWidth(w);
    if (ImGui::Combo("Antialiasing (MSAA)", &m, kMsaaNames, 4)) { g_game.msaa = kMsaa[m]; ch = true; }
    static const int kAniso[] = {1, 2, 4, 8, 16};
    static const char* kAnisoNames[] = {"off", "2x", "4x", "8x", "16x"};
    int a = 0;
    for (int i = 0; i < 5; ++i)
        if (kAniso[i] <= g_game.aniso) a = i;
    ImGui::SetNextItemWidth(w);
    if (ImGui::Combo("Anisotropic filtering", &a, kAnisoNames, 5)) { g_game.aniso = kAniso[a]; ch = true; }
    static const char* kDetail[] = {"low", "medium", "high"};
    ImGui::SetNextItemWidth(w);
    ch |= ImGui::Combo("Detail", &g_game.detail, kDetail, 3);
    ch |= ImGui::Checkbox("Dynamic shadows", &g_game.shadows);
    ImGui::SameLine(0, g_px * 2);
    ch |= ImGui::Checkbox("Bloom", &g_game.bloom);
    ImGui::SameLine(0, g_px * 2);
    ch |= ImGui::Checkbox("Motion blur", &g_game.motionBlur);
    ch |= ImGui::Checkbox("Depth of field", &g_game.dof);
    ImGui::SameLine(0, g_px * 2);
    ch |= ImGui::Checkbox("Lens flares", &g_game.lensFlares);
    ImGui::Spacing();
    ImGui::TextWrapped("Saved to %s", g_gameIni.c_str());
    if (ch) SaveGame();
}

static void DrawLauncher()
{
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    ImGui::Begin("launcher", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    ImGui::TextColored(kAccent, "GEARS OF WAR (2007) TWEAKS");
    ImGui::SameLine();
    ImGui::TextDisabled("launcher");
    ImGui::Separator();
    if (ImGui::BeginTabBar("tabs")) {
        // Each tab scrolls above the status line.
        auto tab = [](const char* name, int index, void (*draw)()) {
            if (!ImGui::BeginTabItem(name, nullptr, g_startTab == index ? ImGuiTabItemFlags_SetSelected : 0)) return;
            ImGui::BeginChild("tab", ImVec2(0, -g_px * 1.8f), false);
            draw();
            ImGui::EndChild();
            ImGui::EndTabItem();
        };
        tab("  Play  ", 0, PlayTab);
        tab("  Mod settings  ", 1, ModTab);
        tab("  Game settings  ", 2, GameTab);
        g_startTab = -1;
        ImGui::EndTabBar();
    }
    ImGui::SetCursorPosY(ImGui::GetWindowHeight() - g_px * 1.6f);
    ImGui::Separator();
    if (!g_status.empty()) ImGui::TextColored(g_statusError ? ImVec4(1, 0.45f, 0.35f, 1) : ImVec4(0.7f, 0.8f, 0.7f, 1), "%s", g_status.c_str());
    else ImGui::TextDisabled("Settings are saved as you change them.");
    ImGui::End();
}

// ---- Direct3D 9 window -----------------------------------------------------------------------

static IDirect3D9* g_d3d;
static IDirect3DDevice9* g_dev;
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
    // --tab mod|game opens on that tab; --continue and --new casual|hardcore|insane start the game
    // at once, as the buttons do (for desktop shortcuts).
    if (strstr(cmdLine, "--tab mod")) g_startTab = 1;
    else if (strstr(cmdLine, "--tab game")) g_startTab = 2;
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
        MessageBoxA(nullptr, "Put gowfov_launcher.exe into the game's Binaries folder (next to Startup.exe).",
                    "Gears of War (2007) Tweaks", MB_ICONERROR);
        return 1;
    }
    g_modIni = g_bin + "gowfov.ini";
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
        MessageBoxA(nullptr, g_status.c_str(), "Gears of War (2007) Tweaks", MB_ICONERROR);
    }
    if (!Exists(g_gameIni)) Status(true, "The game's settings file is missing: start the game once first");
    else if (!Exists(g_bin + "d3d9.dll")) Status(true, "The mod (d3d9.dll) is not in this folder");

    WNDCLASSEXA wc = {sizeof(wc), CS_CLASSDC, WndProc, 0, 0, inst, nullptr, LoadCursor(nullptr, IDC_ARROW), nullptr, nullptr, "gowfov_launcher", nullptr};
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
    int ww = (int)fminf(g_px * 46, (work.right - work.left) * 0.92f), wh = (int)fminf(g_px * 38, (work.bottom - work.top) * 0.92f);
    HWND hwnd = CreateWindowExA(0, wc.lpszClassName, "Gears of War (2007) Tweaks - launcher", WS_OVERLAPPEDWINDOW,
                                work.left + (work.right - work.left - ww) / 2, work.top + (work.bottom - work.top - wh) / 2, ww, wh,
                                nullptr, nullptr, inst, nullptr);
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
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImFontConfig cfg;
    cfg.FontDataOwnedByAtlas = false;
    io.Fonts->AddFontFromMemoryTTF((void*)g_shareTechMono, (int)g_shareTechMonoSize, g_px, &cfg);
    ApplyStyle(g_px);
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
        g_dev->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(15, 15, 20), 1.f, 0);
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
    g_dev->Release();
    g_d3d->Release();
    DestroyWindow(hwnd);
    return 0;
}
