// The game's DirectInput mouse, for the settings menu. In gameplay the engine reads the mouse
// through DirectInput 8 and holds it so that Windows sends the window no mouse messages; the menu
// gets its movement, wheel and left button from these reads instead (OverlayMouseDI), and while
// the menu is open the game reads an idle mouse, so the camera stays still and nothing fires.
//
// DirectInput8Create is patched in the exe's imports; the interface's CreateDevice (vtable slot 3)
// then marks the system mouse, and the devices' GetDeviceState / GetDeviceData (slots 9 and 10)
// are patched in their vtable (shared by the game's devices; only the mouse is changed).
#include <windows.h>
#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>
#include "rowe.h"

static const GUID kSysMouse = {0x6F1D2B60, 0xD5A0, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};

typedef HRESULT (WINAPI* PFN_DirectInput8Create)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
typedef HRESULT (STDMETHODCALLTYPE* PFN_CreateDevice)(void* di, REFGUID guid, void** device, void* outer);
typedef HRESULT (STDMETHODCALLTYPE* PFN_GetDeviceState)(void* device, DWORD size, void* data);
typedef HRESULT (STDMETHODCALLTYPE* PFN_GetDeviceData)(void* device, DWORD size, DIDEVICEOBJECTDATA* data, DWORD* count, DWORD flags);
static PFN_DirectInput8Create g_origCreate;
static PFN_CreateDevice g_origCreateDevice;
static PFN_GetDeviceState g_origGetDeviceState;
static PFN_GetDeviceData g_origGetDeviceData;
static void* volatile g_mice[4];  // the game's mouse devices (it makes more than one)
static bool g_button;           // left button as last read

static void* PatchSlot(void* object, int slot, void* hook)
{
    void** vtbl = *(void***)object;
    if (vtbl[slot] == hook) return nullptr;
    DWORD old;
    VirtualProtect(&vtbl[slot], sizeof(void*), PAGE_READWRITE, &old);
    void* orig = vtbl[slot];
    vtbl[slot] = hook;
    VirtualProtect(&vtbl[slot], sizeof(void*), old, &old);
    return orig;
}

static bool IsMouse(void* device)
{
    for (void* m : g_mice)
        if (m && m == device) return true;
    return false;
}

static void Button(bool down)
{
    if (down != g_button) OverlayMouseDI(0, 0, 0, down ? 1 : -1);
    g_button = down;
}

// Immediate reads (DIMOUSESTATE / DIMOUSESTATE2).
static HRESULT STDMETHODCALLTYPE HookGetDeviceState(void* device, DWORD size, void* data)
{
    HRESULT hr = g_origGetDeviceState(device, size, data);
    if (!IsMouse(device) || FAILED(hr) || !data || size < sizeof(DIMOUSESTATE)) return hr;
    static bool logged;
    if (!logged) { logged = true; Log("dinput: the game reads the mouse with GetDeviceState (%lu bytes)", size); }
    DIMOUSESTATE* m = (DIMOUSESTATE*)data;
    OverlayMouseDI(m->lX, m->lY, m->lZ, 0);
    Button((m->rgbButtons[0] & 0x80) != 0);
    if (OverlayBlocksInput()) memset(data, 0, size);
    return hr;
}

// Buffered reads: the events up to now.
static HRESULT STDMETHODCALLTYPE HookGetDeviceData(void* device, DWORD size, DIDEVICEOBJECTDATA* data, DWORD* count, DWORD flags)
{
    HRESULT hr = g_origGetDeviceData(device, size, data, count, flags);
    if (!IsMouse(device) || FAILED(hr) || !data || !count || size < sizeof(DIDEVICEOBJECTDATA)) return hr;
    static bool logged;
    if (!logged) { logged = true; Log("dinput: the game reads the mouse with GetDeviceData"); }
    if (!(flags & DIGDD_PEEK)) {
        int dx = 0, dy = 0, dz = 0;
        for (DWORD i = 0; i < *count; ++i) {
            const DIDEVICEOBJECTDATA* e = (const DIDEVICEOBJECTDATA*)((const BYTE*)data + i * size);
            if (e->dwOfs == DIMOFS_X) dx += (int)e->dwData;
            else if (e->dwOfs == DIMOFS_Y) dy += (int)e->dwData;
            else if (e->dwOfs == DIMOFS_Z) dz += (int)e->dwData;
            else if (e->dwOfs == DIMOFS_BUTTON0) Button((e->dwData & 0x80) != 0);
        }
        OverlayMouseDI(dx, dy, dz, 0);
    }
    if (OverlayBlocksInput()) *count = 0;
    return hr;
}

static HRESULT STDMETHODCALLTYPE HookCreateDevice(void* di, REFGUID guid, void** device, void* outer)
{
    HRESULT hr = g_origCreateDevice(di, guid, device, outer);
    if (SUCCEEDED(hr) && device && *device && IsEqualGUID(guid, kSysMouse)) {
        for (void* volatile& m : g_mice)
            if (!m) { m = *device; break; }
        void* p = PatchSlot(*device, 9, (void*)HookGetDeviceState);
        if (p) g_origGetDeviceState = (PFN_GetDeviceState)p;
        p = PatchSlot(*device, 10, (void*)HookGetDeviceData);
        if (p) g_origGetDeviceData = (PFN_GetDeviceData)p;
        Log("dinput: mouse device %p", *device);
    }
    return hr;
}

static HRESULT WINAPI HookDirectInput8Create(HINSTANCE inst, DWORD version, REFIID riid, LPVOID* out, LPUNKNOWN outer)
{
    HRESULT hr = g_origCreate(inst, version, riid, out, outer);
    if (SUCCEEDED(hr) && out && *out) {
        void* p = PatchSlot(*out, 3, (void*)HookCreateDevice);
        if (p) g_origCreateDevice = (PFN_CreateDevice)p;
    }
    return hr;
}

void DInputInstall()
{
    g_origCreate = (PFN_DirectInput8Create)PatchExeImport("dinput8.dll", "DirectInput8Create", (void*)HookDirectInput8Create);
    Log("dinput: create hook %d", !!g_origCreate);
}
