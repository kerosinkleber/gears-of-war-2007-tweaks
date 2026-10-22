// Records the text the engine draws each frame (DrawString at 0x005605A0, used by both the
// canvas printing and the UI string renderer), so tools can read what is on screen — menu
// entries, prompts, subtitles — from gowfov_state.txt instead of taking screenshots.
#include <windows.h>
#include <stdio.h>
#include "gowfov.h"

struct TextEntry { float x, y; float color[4]; float scaleX, scaleY; DWORD caller; wchar_t text[80]; };

static const int kMaxEntries = 96;
static TextEntry g_current[kMaxEntries], g_last[kMaxEntries];
static int g_currentCount, g_lastCount;
static void* g_trampoline;

// args: [0] FCanvas*, [1] X, [2] Y, [3] const TCHAR* Text, [4] UFont*, [5] const FLinearColor&,
//       [6] ScaleX, [7] ScaleY
static void __cdecl Record(DWORD* args)
{
    const wchar_t* text = (const wchar_t*)args[3];
    if (!text || !text[0] || g_currentCount >= kMaxEntries) return;
    TextEntry& e = g_current[g_currentCount++];
    e.x = *(float*)&args[1];
    e.y = *(float*)&args[2];
    const float* c = (const float*)args[5];
    for (int i = 0; i < 4; ++i) e.color[i] = c ? c[i] : 0.f;
    e.scaleX = *(float*)&args[6];
    e.scaleY = *(float*)&args[7];
    e.caller = args[-1];  // return address into the code that drew this string
    lstrcpynW(e.text, text, 80);
}

__declspec(naked) static void DrawStringThunk()
{
    __asm {
        lea eax, [esp + 4]   // first argument
        pushad
        push eax
        call Record
        add esp, 4
        popad
        jmp g_trampoline
    }
}

void TextCaptureInstall()
{
    static const BYTE kExpected[8] = {0x83, 0xEC, 0x0C, 0x56, 0x8B, 0x74, 0x24, 0x24};  // sub esp,0Ch; push esi; mov esi,[esp+24h]
    g_trampoline = InstallDetour(0x005605A0, kExpected, sizeof(kExpected), (void*)DrawStringThunk, "text-capture");
}

// Called once per frame (game thread): the strings drawn since the last call become "last frame".
void TextCaptureFrame()
{
    memcpy(g_last, g_current, sizeof(TextEntry) * g_currentCount);
    g_lastCount = g_currentCount;
    g_currentCount = 0;
}

void TextCaptureWrite(FILE* f)
{
    fprintf(f, "text_count=%d\n", g_lastCount);
    for (int i = 0; i < g_lastCount; ++i) {
        const TextEntry& e = g_last[i];
        char utf8[256];
        WideCharToMultiByte(CP_UTF8, 0, e.text, -1, utf8, sizeof(utf8), nullptr, nullptr);
        fprintf(f, "text %6.0f %6.0f rgba %.2f %.2f %.2f %.2f scale %.3f %.3f from %08lx \"%s\"\n",
            e.x, e.y, e.color[0], e.color[1], e.color[2], e.color[3], e.scaleX, e.scaleY, e.caller, utf8);
    }
}
