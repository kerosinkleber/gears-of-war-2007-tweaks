// Command + state channel for driving the game from tools without screenshots or focus.
//
//  gowfov_cmd.txt   one command per line, picked up on the game thread and then deleted:
//                     continue          load the campaign checkpoint (what "Continue Campaign" does)
//                     travel <url>      GEngine->SetClientTravel(url)
//                     key <name>        post a key press to the game window (Enter, Esc, Up, ...)
//                     keydown / keyup <name>, rmb down|up   hold keys / the right mouse button
//                     mouse <x> <y>, lmb down|up   move the (tool) cursor in client pixels, left button
//                     ss <factor>       set supersampling (saved, applied right away)
//  gowfov_state.txt rewritten twice a second: frame count, whether the gameplay camera is
//                   active, lens and supersampling state, and the text drawn last frame.
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gowfov.h"

static const DWORD kGEngine = 0x01B9F5D4;  // UEngine* GEngine
static const int kTravelSlot = 0x114 / 4;  // SetClientTravel(const TCHAR* URL, ETravelType)

typedef void (__thiscall* PFN_Travel)(void* engine, const wchar_t* url, int travelType);

static HWND g_window;
static DWORD g_frames;

void GameCmdSetWindow(HWND window) { g_window = window; }

// Same call the menus make to load a map or the saved game.
static void Travel(const char* url)
{
    void* engine = *(void**)kGEngine;
    if (!engine) { Log("cmd: no engine"); return; }
    wchar_t wurl[512];
    MultiByteToWideChar(CP_ACP, 0, url, -1, wurl, 512);
    PFN_Travel travel = (PFN_Travel)(*(void***)engine)[kTravelSlot];
    travel(engine, wurl, 0);
    Log("cmd: travel %s", url);
}

// phase: 0 = press and release, 1 = down only, 2 = up only
static void PostKey(const char* name, int phase = 0)
{
    static const struct { const char* name; UINT vk; bool ext; } kKeys[] = {
        {"enter", VK_RETURN, false}, {"esc", VK_ESCAPE, false}, {"up", VK_UP, true}, {"down", VK_DOWN, true},
        {"left", VK_LEFT, true}, {"right", VK_RIGHT, true}, {"space", VK_SPACE, false}, {"tab", VK_TAB, false},
        {"r", 'R', false}, {"1", '1', false}, {"2", '2', false}, {"3", '3', false}, {"4", '4', false},
        {"w", 'W', false}, {"a", 'A', false}, {"s", 'S', false}, {"d", 'D', false},
    };
    for (auto& k : kKeys) {
        if (lstrcmpiA(k.name, name) != 0) continue;
        if (!g_window) { Log("cmd: no game window yet"); return; }
        UINT scan = MapVirtualKeyA(k.vk, MAPVK_VK_TO_VSC);
        LPARAM down = 1 | (scan << 16) | (k.ext ? (1 << 24) : 0);
        LPARAM up = down | (1u << 30) | (1u << 31);
        if (phase == 1) BackgroundSetKeyHeld(k.vk, true);
        if (phase == 2) BackgroundSetKeyHeld(k.vk, false);
        if (phase != 2) PostMessageA(g_window, WM_KEYDOWN, k.vk, down);
        if (phase != 1) PostMessageA(g_window, WM_KEYUP, k.vk, up);
        Log("cmd: key %s (%d)", name, phase);
        return;
    }
    Log("cmd: unknown key %s", name);
}

// Right button at the client centre (aiming).
static void PostRightMouse(bool down)
{
    if (!g_window) return;
    RECT rc;
    GetClientRect(g_window, &rc);
    LPARAM pos = MAKELPARAM((rc.right - rc.left) / 2, (rc.bottom - rc.top) / 2);
    BackgroundSetKeyHeld(VK_RBUTTON, down);
    PostMessageA(g_window, down ? WM_RBUTTONDOWN : WM_RBUTTONUP, down ? MK_RBUTTON : 0, pos);
    Log("cmd: right mouse %s", down ? "down" : "up");
}

static int g_mouseX, g_mouseY;  // tool cursor, client coordinates

static void PostMouseMove(int x, int y)
{
    if (!g_window) return;
    g_mouseX = x;
    g_mouseY = y;
    BackgroundSetCursor(x, y);
    PostMessageA(g_window, WM_MOUSEMOVE, 0, MAKELPARAM(x, y));
    Log("cmd: mouse %d %d", x, y);
}

static void PostLeftMouse(bool down)
{
    if (!g_window) return;
    BackgroundSetKeyHeld(VK_LBUTTON, down);
    PostMessageA(g_window, down ? WM_LBUTTONDOWN : WM_LBUTTONUP, down ? MK_LBUTTON : 0, MAKELPARAM(g_mouseX, g_mouseY));
    Log("cmd: left mouse %s", down ? "down" : "up");
}

// Reads and deletes gowfov_cmd.txt, then runs its commands in order (up to 16).
static void RunCommands()
{
    char path[MAX_PATH];
    wsprintfA(path, "%sgowfov_cmd.txt", ModDir());
    FILE* f = fopen(path, "r");
    if (!f) return;
    char line[600];
    char lines[16][600];
    int n = 0;
    while (n < 16 && fgets(line, sizeof(line), f)) {
        char* e = line + strlen(line);
        while (e > line && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ')) *--e = 0;
        if (line[0]) lstrcpyA(lines[n++], line);
    }
    fclose(f);
    DeleteFileA(path);

    for (int i = 0; i < n; ++i) {
        char* cmd = lines[i];
        char* arg = strchr(cmd, ' ');
        if (arg) *arg++ = 0;
        if (lstrcmpiA(cmd, "continue") == 0) Travel("WarGame_P?LoadSaveGame");
        else if (lstrcmpiA(cmd, "travel") == 0 && arg) Travel(arg);
        else if (lstrcmpiA(cmd, "key") == 0 && arg) PostKey(arg);
        else if (lstrcmpiA(cmd, "keydown") == 0 && arg) PostKey(arg, 1);
        else if (lstrcmpiA(cmd, "keyup") == 0 && arg) PostKey(arg, 2);
        else if (lstrcmpiA(cmd, "rmb") == 0 && arg) PostRightMouse(lstrcmpiA(arg, "down") == 0);
        else if (lstrcmpiA(cmd, "lmb") == 0 && arg) PostLeftMouse(lstrcmpiA(arg, "down") == 0);
        else if (lstrcmpiA(cmd, "mouse") == 0 && arg) {
            int x, y;
            if (sscanf(arg, "%d %d", &x, &y) == 2) PostMouseMove(x, y);
        }
        else if (lstrcmpiA(cmd, "ss") == 0 && arg) { SupersampleSetFactor((float)atof(arg)); Log("cmd: ss %s", arg); }
        else Log("cmd: unknown command '%s'", cmd);
    }
}

// Writes gowfov_state.txt through a temporary file, so readers never see half a file.
static void WriteState()
{
    char path[MAX_PATH], tmp[MAX_PATH];
    wsprintfA(path, "%sgowfov_state.txt", ModDir());
    wsprintfA(tmp, "%sgowfov_state.tmp", ModDir());
    FILE* f = fopen(tmp, "w");
    if (!f) return;
    UINT nw = 0, nh = 0, vw = 0, vh = 0;
    bool ss = SupersampleGetSizes(&nw, &nh, &vw, &vh);
    fprintf(f, "frame=%lu\n", g_frames);
    fprintf(f, "gameplay_camera=%d\n", LensGameplayActive() ? 1 : 0);
    fprintf(f, "lens_mode=%d\nlens_fov=%.1f\nfov_setting=%.0f\n", LensGetMode(), LensEffectiveFov(), FovGet());
    fprintf(f, "supersample=%s %ux%u -> %ux%u\n", ss ? "on" : "off", vw, vh, nw, nh);
    TextCaptureWrite(f);
    fclose(f);
    MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING);
}

// Called once per frame on the game thread.
void GameCmdTick()
{
    ++g_frames;
    TextCaptureFrame();
    if (g_frames % 10 == 0) RunCommands();
    if (g_frames % 30 == 0) WriteState();
}
