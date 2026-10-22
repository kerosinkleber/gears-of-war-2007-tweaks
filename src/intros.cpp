// Skips the startup intro videos (ESRB notice, Epic and Microsoft Game Studios logos).
//
// The FFullScreenMovieBink constructor (called at 0x0042240F) builds the startup playback queue
// at object +0x70: an array of 28-byte entries that begin with the movie name (FString).
// GameThreadInitiateStartupSequence plays entry 0 and the player advances through the rest.
// Editing the ini is not reliable for this game, so the mod empties that queue right after
// construction. "Startup" is removed from the queue too: it is the preloaded loading-screen movie,
// and normally the title screen interrupts the sequence before it plays. If it plays at boot, the
// first loading screen's request for it fails ("Failed to open/prepare movie") and loads show no
// loading screen. It stays registered as an always-loaded movie, so loading screens still use it.
// gowfov.ini: skipintros=0 turns this off.
#include <windows.h>
#include <string.h>
#include <intrin.h>
#include "gowfov.h"

struct QueueArray { BYTE* data; int num; int max; };
static const int kEntrySize = 28;  // FString name (data, num, max) + 4 more fields

typedef void* (__thiscall* PFN_MovieCtor)(void* self);
static const PFN_MovieCtor kMovieCtor = (PFN_MovieCtor)0x00421C70;
static bool g_enabled = true;

static bool IsIntro(const wchar_t* name)
{
    if (!name) return false;
    return _wcsnicmp(name, L"ESRB", 4) == 0 || _wcsicmp(name, L"EpicLogo") == 0 ||
           _wcsicmp(name, L"MGSLogo") == 0 || _wcsicmp(name, L"Startup") == 0;
}

static void* __fastcall HookMovieCtor(void* self, void*)
{
    void* result = kMovieCtor(self);
    QueueArray* queue = (QueueArray*)((BYTE*)self + 0x70);
    if (!g_enabled || !queue->data || queue->num <= 0 || queue->num > queue->max) return result;
    int kept = 0;
    for (int i = 0; i < queue->num; ++i) {
        BYTE* entry = queue->data + i * kEntrySize;
        const wchar_t* name = *(const wchar_t**)entry;
        if (IsIntro(name)) {
            Log("intros: skipping %ls", name);
            continue;  // the entry's own allocations are left alone (a few bytes)
        }
        if (kept != i) memmove(queue->data + kept * kEntrySize, entry, kEntrySize);
        ++kept;
    }
    queue->num = kept;
    return result;
}

// Debug log of movie requests: FFullScreenMovieBink vtable slot 6 =
// GameThreadPlayMovie(Mode, Filename, StartFrame).
typedef void (__thiscall* PFN_PlayMovie)(void* self, int mode, const wchar_t* file, int startFrame);
static PFN_PlayMovie g_origPlayMovie;

static void __fastcall HookPlayMovie(void* self, void*, int mode, const wchar_t* file, int startFrame)
{
    Log("movie: play mode=%x file=%ls from %08lx", mode, file ? file : L"(null)", (DWORD)_ReturnAddress());
    g_origPlayMovie(self, mode, file, startFrame);
}

bool IntrosSkipped() { return g_enabled; }

// The startup queue is built once at launch, so a change takes effect at the next start.
void IntrosSetSkipped(bool skip)
{
    g_enabled = skip;
    char path[MAX_PATH];
    wsprintfA(path, "%sgowfov.ini", ModDir());
    WritePrivateProfileStringA("gowfov", "skipintros", skip ? "1" : "0", path);
}

// Patches the call to the movie player constructor (and the PlayMovie slot, for the log).
void IntrosInstall()
{
    if ((DWORD)GetModuleHandleA(nullptr) == 0x00400000) {
        void** slot = (void**)(0x015996C8 + 6 * 4);
        if (!IsBadReadPtr(slot, 4) && *slot == (void*)0x00423590) {
            DWORD old;
            VirtualProtect(slot, 4, PAGE_READWRITE, &old);
            g_origPlayMovie = (PFN_PlayMovie)*slot;
            *slot = (void*)HookPlayMovie;
            VirtualProtect(slot, 4, old, &old);
        }
    }
    char path[MAX_PATH];
    wsprintfA(path, "%sgowfov.ini", ModDir());
    g_enabled = GetPrivateProfileIntA("gowfov", "skipintros", 1, path) != 0;

    if ((DWORD)GetModuleHandleA(nullptr) != 0x00400000) return;
    BYTE* site = (BYTE*)0x0042240F;
    if (IsBadReadPtr(site, 5) || site[0] != 0xE8 || 0x0042240F + 5 + *(int*)(site + 1) != 0x00421C70) {
        Log("intros: call site mismatch, not hooked");
        return;
    }
    DWORD old;
    VirtualProtect(site, 5, PAGE_EXECUTE_READWRITE, &old);
    *(int*)(site + 1) = (int)((BYTE*)HookMovieCtor - (site + 5));
    VirtualProtect(site, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, 5);
}
