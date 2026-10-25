// Background mode (rowe.ini: background=1): the game keeps running and accepts posted
// keyboard messages while its window is not in focus, so tools can drive it without stealing
// the user's focus. Off by default; normal alt-tab pausing is unchanged unless enabled.
//
// The engine pauses when it sees its window deactivate and checks GetFocus/GetActiveWindow
// (and, in a window, GetForegroundWindow), so in background mode those imports answer with the
// game window and the deactivate messages are not passed on to the engine's window procedure.
//
// When the game window first comes up it takes the focus; in background mode the mod hands it
// straight back to the window that was in front when the game started and puts the game
// window behind the others, so a tool can start the game without interrupting the user.
//
// The viewport also polls GetKeyState every frame and releases any key the OS says is up,
// which turns a posted key-down into a tap. Keys the tools hold (BackgroundSetKeyHeld) are
// reported as down to the engine's GetKeyState/GetAsyncKeyState.
#include <windows.h>
#include "rowe.h"

static bool g_enabled;
static HWND g_window;
static HWND g_userWindow;   // in front when the game started (not the game's)
static DWORD g_firstPresent;
static WNDPROC g_origWndProc;
static volatile bool g_held[256];

typedef HWND (WINAPI* PFN_GetHwnd)();
static PFN_GetHwnd g_origGetFocus, g_origGetActiveWindow, g_origGetForegroundWindow;
typedef SHORT (WINAPI* PFN_GetKeyState)(int vk);
static PFN_GetKeyState g_origGetKeyState, g_origGetAsyncKeyState;

static SHORT WINAPI HookGetKeyState(int vk)
{
    if (g_enabled && vk >= 0 && vk < 256 && g_held[vk]) return (SHORT)0x8000;
    return g_origGetKeyState(vk);
}

static SHORT WINAPI HookGetAsyncKeyState(int vk)
{
    if (g_enabled && vk >= 0 && vk < 256 && g_held[vk]) return (SHORT)0x8000;
    return g_origGetAsyncKeyState(vk);
}

void BackgroundSetKeyHeld(UINT vk, bool held)
{
    if (vk < 256) g_held[vk] = held;
}

// Tool cursor: once a tool has placed the cursor, the engine's GetCursorPos/SetCursorPos use
// it instead of the real one (screen coordinates), so the user's mouse is left alone.
typedef BOOL (WINAPI* PFN_GetCursorPos)(LPPOINT);
typedef BOOL (WINAPI* PFN_SetCursorPos)(int, int);
static PFN_GetCursorPos g_origGetCursorPos;
static PFN_SetCursorPos g_origSetCursorPos;
static volatile bool g_toolCursor;
static volatile LONG g_toolX, g_toolY;

static BOOL WINAPI HookGetCursorPos(LPPOINT p)
{
    if (g_enabled && g_toolCursor && p) {
        p->x = g_toolX;
        p->y = g_toolY;
        return TRUE;
    }
    return g_origGetCursorPos(p);
}

static BOOL WINAPI HookSetCursorPos(int x, int y)
{
    if (g_enabled && g_toolCursor) {
        g_toolX = x;
        g_toolY = y;
        return TRUE;
    }
    return g_origSetCursorPos(x, y);
}

void BackgroundSetCursor(int x, int y)
{
    POINT p = {x, y};
    if (g_window) ClientToScreen(g_window, &p);
    g_toolX = p.x;
    g_toolY = p.y;
    g_toolCursor = true;
}

static HWND WINAPI HookGetFocus()
{
    return (g_enabled && g_window) ? g_window : g_origGetFocus();
}

static HWND WINAPI HookGetActiveWindow()
{
    return (g_enabled && g_window) ? g_window : g_origGetActiveWindow();
}

static HWND WINAPI HookGetForegroundWindow()
{
    return (g_enabled && g_window) ? g_window : g_origGetForegroundWindow();
}

// Swallows the deactivation messages that would pause the game.
static LRESULT CALLBACK BackgroundWndProc(HWND h, UINT msg, WPARAM w, LPARAM l)
{
    if (g_enabled) {
        if (msg == WM_ACTIVATEAPP && !w) return 0;
        if (msg == WM_ACTIVATE && LOWORD(w) == WA_INACTIVE) return 0;
        if (msg == WM_KILLFOCUS) return 0;
    }
    return CallWindowProcW(g_origWndProc, h, msg, w, l);
}

// Reads background= from rowe.ini (only at startup).
void BackgroundInit()
{
    char path[MAX_PATH];
    wsprintfA(path, "%srowe.ini", ModDir());
    g_enabled = GetPrivateProfileIntA("rowe", "background", 0, path) != 0;
    HWND front = GetForegroundWindow();
    DWORD pid = 0;
    GetWindowThreadProcessId(front, &pid);
    if (g_enabled && front && pid != GetCurrentProcessId()) g_userWindow = front;
}

// The first seconds after the game window appears: whenever it takes the focus, it goes back.
static void ReturnFocus(HWND window)
{
    if (!g_userWindow || GetTickCount() - g_firstPresent > 20000) return;
    if (GetForegroundWindow() != window) return;
    if (!IsWindow(g_userWindow)) { g_userWindow = nullptr; return; }
    SetWindowPos(window, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    SetForegroundWindow(g_userWindow);
    Log("background mode: focus returned to %p", g_userWindow);
}

bool BackgroundEnabled() { return g_enabled; }

// Patches the exe's USER32 imports; the hooks only change anything while background=1.
void BackgroundInstall()
{
    g_origGetFocus = (PFN_GetHwnd)PatchExeImport("user32.dll", "GetFocus", (void*)HookGetFocus);
    g_origGetActiveWindow = (PFN_GetHwnd)PatchExeImport("user32.dll", "GetActiveWindow", (void*)HookGetActiveWindow);
    g_origGetForegroundWindow = (PFN_GetHwnd)PatchExeImport("user32.dll", "GetForegroundWindow", (void*)HookGetForegroundWindow);
    g_origGetKeyState = (PFN_GetKeyState)PatchExeImport("user32.dll", "GetKeyState", (void*)HookGetKeyState);
    g_origGetAsyncKeyState = (PFN_GetKeyState)PatchExeImport("user32.dll", "GetAsyncKeyState", (void*)HookGetAsyncKeyState);
    g_origGetCursorPos = (PFN_GetCursorPos)PatchExeImport("user32.dll", "GetCursorPos", (void*)HookGetCursorPos);
    g_origSetCursorPos = (PFN_SetCursorPos)PatchExeImport("user32.dll", "SetCursorPos", (void*)HookSetCursorPos);
}

// Called from Present with the swap chain's output window.
void BackgroundOnPresent(HWND window)
{
    if (!g_enabled || !window) return;
    if (window == g_window) {
        ReturnFocus(window);
        return;
    }
    g_window = window;
    g_firstPresent = GetTickCount();
    g_origWndProc = (WNDPROC)SetWindowLongPtrW(window, GWLP_WNDPROC, (LONG_PTR)BackgroundWndProc);
    // The window may never have been activated if the game started behind other windows.
    PostMessageW(window, WM_ACTIVATEAPP, TRUE, 0);
    PostMessageW(window, WM_ACTIVATE, WA_ACTIVE, 0);
    Log("background mode: window %p", window);
}
