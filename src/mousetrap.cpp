// Mouse trap: keeps the cursor on the monitor the game runs on, so it cannot drift onto another
// screen (where a click would switch away from the game). rowe.ini mousetrap=0 off (the game's
// own behaviour), 1 during gameplay only (menus leave the cursor free, even where the game would
// confine it), 2 during gameplay and in menus. The mod only manages the cursor while the game
// window is in front and lets go as soon as it is not.
//
// Gameplay vs. menus: gameplay is when the player camera has updated within the last few frames
// (LensGameplayActive). The engine's own mouse capture is no help here: the game draws its own
// cursor in menus and keeps the mouse captured there too.
//
// The engine confines the cursor itself while it has the mouse captured (LockMouseToWindow ->
// ClipCursor with the window's client rectangle). All its ClipCursor calls go through
// HookClipCursor, which also clamps that rectangle to the real client area: while
// supersampling, GetClientRect reports the larger virtual size and the engine's rectangle would
// reach past the screen.
#include <windows.h>
#include "rowe.h"

static volatile int g_mode;              // kMouseTrapOff / Gameplay / Always
static volatile bool g_engineClips;      // the engine's last ClipCursor request was a rectangle
static RECT g_engineRect;                // ... and that rectangle
static volatile bool g_managing;         // the clip is ours (trap on, game in front)
static HWND g_window;
static WNDPROC g_origWndProc;

typedef BOOL (WINAPI* PFN_ClipCursor)(const RECT*);
static PFN_ClipCursor g_origClipCursor;

static bool GameInFront()
{
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

static bool MonitorRect(RECT* out)
{
    MONITORINFO mi = {sizeof(mi)};
    if (!g_window || !GetMonitorInfoA(MonitorFromWindow(g_window, MONITOR_DEFAULTTONEAREST), &mi)) return false;
    *out = mi.rcMonitor;
    return true;
}

// The game window's real client area in screen coordinates (this DLL's own USER32 imports,
// which supersampling does not touch).
static bool ClientRectOnScreen(RECT* out)
{
    RECT rc;
    if (!g_window || !GetClientRect(g_window, &rc)) return false;
    POINT tl = {rc.left, rc.top}, br = {rc.right, rc.bottom};
    ClientToScreen(g_window, &tl);
    ClientToScreen(g_window, &br);
    SetRect(out, tl.x, tl.y, br.x, br.y);
    return true;
}

// The engine's own clip, kept inside the window. False for "no clip".
static bool EngineClip(RECT* out)
{
    if (!g_engineClips) return false;
    RECT r = g_engineRect, client;
    if (ClientRectOnScreen(&client) && IntersectRect(&client, &client, &r)) r = client;
    *out = r;
    return true;
}

// The clip the trap wants while the game is in front. False for "no clip".
static bool TrapClip(RECT* out)
{
    bool gameplay = LensGameplayActive();
    if (g_mode == kMouseTrapGameplay && !gameplay) return false;  // menus: free
    RECT r, monitor;
    bool clip = EngineClip(&r);
    if (MonitorRect(&monitor) && (!clip || !IntersectRect(&r, &r, &monitor))) r = monitor;
    else if (!clip) return false;
    *out = r;
    return true;
}

static BOOL WINAPI HookClipCursor(const RECT* rc)
{
    g_engineClips = rc != nullptr;
    if (rc) g_engineRect = *rc;
    RECT want;
    bool clip = g_mode != kMouseTrapOff && GameInFront() ? TrapClip(&want) : EngineClip(&want);
    return g_origClipCursor(clip ? &want : nullptr);
}

// Hands the cursor back: to the engine's own clip while the game is in front, otherwise free.
static void Release()
{
    if (!g_managing) return;
    g_managing = false;
    RECT want;
    g_origClipCursor(GameInFront() && EngineClip(&want) ? &want : nullptr);
}

// Releases the trap as soon as the game is switched away from; outermost subclass of the game
// window, so it sees deactivation even where background mode hides it from the engine.
static LRESULT CALLBACK MouseTrapWndProc(HWND h, UINT msg, WPARAM w, LPARAM l)
{
    if ((msg == WM_ACTIVATEAPP && !w) || (msg == WM_ACTIVATE && LOWORD(w) == WA_INACTIVE)) Release();
    return CallWindowProcW(g_origWndProc, h, msg, w, l);
}

// Every frame from Present: while the trap is on and the game is in front, makes the actual
// clip match TrapClip (which also puts it back if something else reset it).
void MouseTrapFrame(HWND window)
{
    if (!window || !g_origClipCursor) return;
    if (!g_window) {
        g_window = window;
        g_origWndProc = (WNDPROC)SetWindowLongPtrW(window, GWLP_WNDPROC, (LONG_PTR)MouseTrapWndProc);
    }
    if (g_mode == kMouseTrapOff || !GameInFront()) {
        Release();
        return;
    }
    g_managing = true;
    RECT want, cur;
    bool clip = TrapClip(&want);
    if (!clip) {
        // Unclipped, GetClipCursor reports the whole virtual desktop.
        SetRect(&want, GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN),
            GetSystemMetrics(SM_XVIRTUALSCREEN) + GetSystemMetrics(SM_CXVIRTUALSCREEN),
            GetSystemMetrics(SM_YVIRTUALSCREEN) + GetSystemMetrics(SM_CYVIRTUALSCREEN));
    }
    if (GetClipCursor(&cur) && EqualRect(&cur, &want)) return;
    g_origClipCursor(clip ? &want : nullptr);
}

// Reads mousetrap= from rowe.ini (also used to reload it at runtime).
void MouseTrapInit()
{
    char path[MAX_PATH];
    wsprintfA(path, "%srowe.ini", ModDir());
    int mode = GetPrivateProfileIntA("rowe", "mousetrap", kMouseTrapOff, path);
    g_mode = mode >= 0 && mode < kMouseTrapModeCount ? mode : kMouseTrapOff;
}

int MouseTrapGetMode() { return g_mode; }

void MouseTrapSetMode(int mode)
{
    g_mode = mode;
    char path[MAX_PATH], val[8];
    wsprintfA(path, "%srowe.ini", ModDir());
    wsprintfA(val, "%d", mode);
    WritePrivateProfileStringA("rowe", "mousetrap", val, path);
}

// From DllMain: the exe's ClipCursor import.
void MouseTrapInstall()
{
    g_origClipCursor = (PFN_ClipCursor)PatchExeImport("user32.dll", "ClipCursor", (void*)HookClipCursor);
    Log("mousetrap: mode %d, clip hook %d", (int)g_mode, !!g_origClipCursor);
}
