// Key names in rowe.ini (screenshotkey, menukey), shared by the mod and the launcher: F1-F24,
// A-Z, 0-9, Num0-Num9, a few named keys, or a virtual key code such as 0x7B.
#pragma once
#include <windows.h>
#include <stdlib.h>
#include <string.h>

static const struct { const char* name; UINT vk; } kKeyNames[] = {
    {"Home", VK_HOME}, {"End", VK_END}, {"Insert", VK_INSERT}, {"Delete", VK_DELETE},
    {"PageUp", VK_PRIOR}, {"PageDown", VK_NEXT}, {"Pause", VK_PAUSE}, {"ScrollLock", VK_SCROLL},
    {"PrintScreen", VK_SNAPSHOT},
};

// The virtual key for a name, 0 if it is not one.
static UINT KeyFromName(const char* s)
{
    for (auto& k : kKeyNames)
        if (lstrcmpiA(s, k.name) == 0) return k.vk;
    if ((s[0] == 'F' || s[0] == 'f') && s[1] >= '1' && s[1] <= '9') {
        int n = atoi(s + 1);
        if (n >= 1 && n <= 24) return VK_F1 + n - 1;
    }
    if (_strnicmp(s, "Num", 3) == 0 && s[3] >= '0' && s[3] <= '9' && !s[4]) return VK_NUMPAD0 + (s[3] - '0');
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) return (UINT)strtoul(s, nullptr, 16) & 0xFF;
    if (s[0] && !s[1]) {
        char c = s[0] >= 'a' && s[0] <= 'z' ? s[0] - 32 : s[0];
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return (UINT)c;
    }
    return 0;
}

// The name for a virtual key (as KeyFromName reads it back); out holds at least 8 characters.
static void KeyToName(UINT vk, char* out)
{
    for (auto& k : kKeyNames)
        if (k.vk == vk) { lstrcpyA(out, k.name); return; }
    if (vk >= VK_F1 && vk <= VK_F24) wsprintfA(out, "F%u", vk - VK_F1 + 1);
    else if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) wsprintfA(out, "Num%u", vk - VK_NUMPAD0);
    else if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) wsprintfA(out, "%c", (char)vk);
    else wsprintfA(out, "0x%02X", vk);
}
