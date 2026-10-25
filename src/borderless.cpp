// Borderless window (rowe.ini borderless=1): while the game runs in a window, the window has no
// frame or title bar and is centred on its monitor. If it is smaller than the monitor, a black
// backdrop covers the rest (not in background mode, where it would cover the user's desktop).
// Fullscreen is unchanged. Carried over from ROWE for Fallout New Vegas.
//
// The engine sets its window's style, size and position in FWindowsViewport::Resize (which
// supersample.cpp hooks): AdjustWindowRect turns the client size into the window size,
// SetWindowLong sets the style (only if it differs from the one it wants) and SetWindowPos places
// the window. While a windowed Resize runs, those calls are changed: no frame (in
// supersample.cpp's AdjustWindowRect hook, which holds that import), no frame styles, centred. Switching it at runtime runs Resize again.
#include <windows.h>
#include "rowe.h"

static volatile bool g_on;
static bool g_wasOn;          // the window was last placed borderless (centre it again when off)
static HWND g_window, g_backdrop;
static WNDPROC g_origWndProc;

typedef LONG (WINAPI* PFN_SetWindowLong)(HWND, int, LONG);
typedef BOOL (WINAPI* PFN_SetWindowPos)(HWND, HWND, int, int, int, int, UINT);
static PFN_SetWindowLong g_origSetWindowLongW, g_origSetWindowLongA;
static PFN_SetWindowPos g_origSetWindowPos;

static const LONG kFrameStyles = WS_CAPTION | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_BORDER | WS_DLGFRAME;

// The engine's window calls to change: inside a windowed Resize, and on the game window itself
// (once its startup splash is over the engine restores the window's saved framed style and
// placement, outside Resize).
static bool Active(HWND h) { return g_on && (SupersampleInWindowedResize() || (h && h == g_window)) && BorderlessWindowed(); }

static RECT MonitorOf(HWND h)
{
    MONITORINFO mi = {sizeof(mi)};
    HMONITOR m = h && IsWindow(h) ? MonitorFromWindow(h, MONITOR_DEFAULTTOPRIMARY) : MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    GetMonitorInfoA(m, &mi);
    return mi.rcMonitor;
}

static LRESULT CALLBACK BackdropProc(HWND h, UINT msg, WPARAM w, LPARAM l)
{
    switch (msg) {
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;  // a click on the black area leaves the game active
    case WM_SETCURSOR: SetCursor(nullptr); return TRUE;
    }
    return DefWindowProcA(h, msg, w, l);
}

// The black backdrop behind a window smaller than its monitor; the game window is owned by it, so
// it stays in front of it. Removed again when the window covers the monitor or borderless is off.
static void UpdateBackdrop(HWND game, const RECT& monitor, int cx, int cy)
{
    bool want = g_on && !BackgroundEnabled() && (cx < monitor.right - monitor.left || cy < monitor.bottom - monitor.top);
    if (!want) {
        if (g_backdrop) {
            SetWindowLongPtrA(game, GWLP_HWNDPARENT, 0);
            DestroyWindow(g_backdrop);
            g_backdrop = nullptr;
        }
        return;
    }
    if (!g_backdrop) {
        WNDCLASSA wc = {};
        wc.lpfnWndProc = BackdropProc;
        wc.hInstance = GetModuleHandleA(nullptr);
        wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
        wc.lpszClassName = "ROWE backdrop";
        RegisterClassA(&wc);
        g_backdrop = CreateWindowExA(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, wc.lpszClassName, "", WS_POPUP, 0, 0, 0, 0, nullptr,
                                     nullptr, wc.hInstance, nullptr);
        if (!g_backdrop) return;
        SetWindowLongPtrA(game, GWLP_HWNDPARENT, (LONG_PTR)g_backdrop);
        Log("borderless: backdrop %p", g_backdrop);
    }
    SetWindowPos(g_backdrop, nullptr, monitor.left, monitor.top, monitor.right - monitor.left, monitor.bottom - monitor.top,
                       SWP_NOACTIVATE | SWP_NOZORDER | SWP_SHOWWINDOW);
}

// The engine's style for its window: without frame. Not WS_POPUP: the engine takes a popup
// window for fullscreen and switches to it.
static LONG BorderlessStyle(int index, LONG value)
{
    if (index == GWL_STYLE) return value & ~(kFrameStyles | WS_POPUP);
    if (index == GWL_EXSTYLE) return (value & ~(WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_DLGMODALFRAME)) | WS_EX_APPWINDOW;
    return value;
}

static LONG WINAPI HookSetWindowLongW(HWND h, int index, LONG value)
{
    return g_origSetWindowLongW(h, index, Active(h) ? BorderlessStyle(index, value) : value);
}

static LONG WINAPI HookSetWindowLongA(HWND h, int index, LONG value)
{
    return g_origSetWindowLongA(h, index, Active(h) ? BorderlessStyle(index, value) : value);
}

// The engine placing its window: centred on its monitor (borderless, or framed right after
// borderless was switched off, so the title bar is not left above the screen).
static BOOL WINAPI HookSetWindowPos(HWND h, HWND after, int x, int y, int cx, int cy, UINT flags)
{
    // Background mode: the engine's topmost / not-topmost pair would put the game over the
    // user's windows.
    if (BackgroundEnabled()) flags |= SWP_NOZORDER | SWP_NOACTIVATE;
    if (!SupersampleInWindowedResize() || !h || (GetWindowLongA(h, GWL_STYLE) & WS_CHILD)) return g_origSetWindowPos(h, after, x, y, cx, cy, flags);
    g_window = h;
    if (flags & SWP_NOSIZE) {
        RECT r;
        GetWindowRect(h, &r);
        cx = r.right - r.left;
        cy = r.bottom - r.top;
    }
    if (g_on) {
        // The style as well: a new window was created with the framed style, which the engine
        // then does not set again.
        SetWindowLongW(h, GWL_STYLE, BorderlessStyle(GWL_STYLE, GetWindowLongW(h, GWL_STYLE)));  // this DLL's own imports
        SetWindowLongW(h, GWL_EXSTYLE, BorderlessStyle(GWL_EXSTYLE, GetWindowLongW(h, GWL_EXSTYLE)));
        flags |= SWP_FRAMECHANGED;
    }
    if (g_on || g_wasOn) {
        RECT m = MonitorOf(h);
        x = m.left + (m.right - m.left - cx) / 2;
        y = m.top + (m.bottom - m.top - cy) / 2;
        if (y < m.top) y = m.top;
        if (x < m.left) x = m.left;
        flags &= ~SWP_NOMOVE;
        UpdateBackdrop(h, m, cx, cy);
        if (g_wasOn != g_on) Log("borderless: %s, window %dx%d at %d,%d", g_on ? "on" : "off", cx, cy, x, y);
        g_wasOn = g_on;
    }
    return g_origSetWindowPos(h, after, x, y, cx, cy, flags);
}

// Minimizing the game hides the backdrop with it.
static LRESULT CALLBACK BorderlessWndProc(HWND h, UINT msg, WPARAM w, LPARAM l)
{
    if (msg == WM_SIZE && g_backdrop) ShowWindow(g_backdrop, w == SIZE_MINIMIZED ? SW_HIDE : SW_SHOWNOACTIVATE);
    return CallWindowProcW(g_origWndProc, h, msg, w, l);
}

void BorderlessFrame(HWND window)
{
    if (window && !g_origWndProc) g_origWndProc = (WNDPROC)SetWindowLongPtrW(window, GWLP_WNDPROC, (LONG_PTR)BorderlessWndProc);
}

void BorderlessInit()
{
    char path[MAX_PATH];
    wsprintfA(path, "%srowe.ini", ModDir());
    g_on = GetPrivateProfileIntA("rowe", "borderless", 0, path) != 0;
}

bool BorderlessGet() { return g_on; }
bool BorderlessWindowed() { return SupersampleViewportWindowed(); }

void BorderlessSet(bool on)
{
    g_on = on;
    char path[MAX_PATH];
    wsprintfA(path, "%srowe.ini", ModDir());
    WritePrivateProfileStringA("rowe", "borderless", on ? "1" : "0", path);
    if (BorderlessWindowed()) SupersampleResizeViewport();
}

// The splash restore's placement: the window stays where it is.
typedef BOOL (WINAPI* PFN_SetWindowPlacement)(HWND, const WINDOWPLACEMENT*);
static PFN_SetWindowPlacement g_origSetWindowPlacement;

static BOOL WINAPI HookSetWindowPlacement(HWND h, const WINDOWPLACEMENT* wp)
{
    if (!wp || !Active(h)) return g_origSetWindowPlacement(h, wp);
    WINDOWPLACEMENT keep = *wp;
    GetWindowRect(h, &keep.rcNormalPosition);
    return g_origSetWindowPlacement(h, &keep);
}

void BorderlessInstall()
{
    g_origSetWindowPlacement = (PFN_SetWindowPlacement)PatchExeImport("user32.dll", "SetWindowPlacement", (void*)HookSetWindowPlacement);
    g_origSetWindowLongW = (PFN_SetWindowLong)PatchExeImport("user32.dll", "SetWindowLongW", (void*)HookSetWindowLongW);
    g_origSetWindowLongA = (PFN_SetWindowLong)PatchExeImport("user32.dll", "SetWindowLongA", (void*)HookSetWindowLongA);
    g_origSetWindowPos = (PFN_SetWindowPos)PatchExeImport("user32.dll", "SetWindowPos", (void*)HookSetWindowPos);
    Log("borderless: %s, hooks %d %d %d %d", g_on ? "on" : "off", !!g_origSetWindowLongW,
        !!g_origSetWindowLongA, !!g_origSetWindowPos, !!g_origSetWindowPlacement);
}
