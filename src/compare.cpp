// Comparison split screen (rowe.ini compare=1): the game is drawn twice in the same frame, as a
// 16:9 picture in each half of the screen (black above and below): on the left as the game shows
// it, on the right with ROWE's picture settings.
//
// The engine supports split-screen: each frame UGameViewportClient::Draw asks every local player
// for a view (ULocalPlayer::CalcSceneView, 0x0048ABC0) and renders all views of the frame in one
// pass. The view's rectangle comes from the player's Origin and Size (fractions of the viewport,
// player +0x68..+0x74) and its projection from the controller's FOV and the rectangle's aspect
// ratio. The draw loop draws each player's HUD right after making its view, in that view.
//
// The comparison runs the loop twice for the one player: after the first pass (the right view,
// ROWE's, with its HUD) the engine's player list (GEngine->GamePlayers, +0x288) briefly holds the
// player twice, so the loop comes round again; the second CalcSceneView puts the list back at
// once and makes the left view. The engine keeps the views by player index (the left one is index
// 1), and draws the HUDs in a second loop over the players after the 3D scene (0x00491177), which
// looks the view up by that index: with the player listed twice during that loop too, it draws
// the HUD in the left view as well. Only while the gameplay camera is active: menus, the pause
// screen and cutscenes keep the single full view.
//
// The left view's FOV is the game's own: the controller's FOV (GetFOVAngle, called by
// CalcSceneView at 0x0048AD62) with ROWE's FOV and lens changes undone (FovGameFov).
//
// With supersampling, both views render at the large size (the engine cannot render views of
// different sizes in one frame), and the left half is resolved with one sample per screen pixel
// instead of the average (supersample.cpp asks CompareLeftRect): its edges look as without
// supersampling (textures can be a little sharper, as their detail is chosen for the large size).
//
// The left view has a view state of
// its own (occlusion and temporal history), made by the engine's AllocateViewState (0x007EF850,
// which ULocalPlayer uses for its own, player +0x88); the renderer needs one for every view.
#include <windows.h>
#include <string.h>
#include "rowe.h"

static volatile bool g_on;
static volatile bool g_secondView;  // CalcSceneView is building the left (game) view

// The player list while the loop runs a second time (game thread).
struct PlayerList { void** data; int num, max; };
static const DWORD kGEngine = 0x01B9F5D4;
static PlayerList g_savedPlayers;
static void* g_twice[2];
static bool g_listSwapped;  // GamePlayers holds the player twice now
static bool g_rightMade;    // this frame's right view exists: the next pass makes the left one
static bool g_leftMade;     // this frame's left view exists (index 1): the HUD loop runs twice
static volatile bool g_leftValid;  // g_left holds this frame's left view rectangle (supersampled frame pixels)
static float g_left[4];

bool CompareLeftRect(float* rect)
{
    if (!g_on || !g_leftValid) return false;
    for (int i = 0; i < 4; ++i) rect[i] = g_left[i];
    return true;
}

static PlayerList* Players()
{
    BYTE* engine = *(BYTE**)kGEngine;
    return engine ? (PlayerList*)(engine + 0x288) : nullptr;
}

// GamePlayers holds its one player twice.
static void DoublePlayers()
{
    PlayerList* list = Players();
    if (!list || list->num != 1 || !list->data || !list->data[0]) return;
    g_savedPlayers = *list;
    g_twice[0] = g_twice[1] = list->data[0];
    list->data = g_twice;
    list->num = list->max = 2;
    g_listSwapped = true;
}

static void RestorePlayers()
{
    if (!g_listSwapped) return;
    if (PlayerList* list = Players()) *list = g_savedPlayers;
    g_listSwapped = false;
}

typedef void* (__thiscall* PFN_CalcSceneView)(void* player, void* family, void* location, void* rotation, void* viewport);
static PFN_CalcSceneView g_origCalcSceneView;
typedef float (__thiscall* PFN_GetFOVAngle)(void* controller);
static PFN_GetFOVAngle g_origGetFOVAngle;
static volatile float g_gameFov;  // the left view's FOV, last frame (for its label)

// APlayerController::GetFOVAngle inside CalcSceneView: for the left view, the game's FOV.
static float __fastcall HookGetFOVAngle(void* controller, void*)
{
    float fov = g_origGetFOVAngle(controller);
    if (!g_secondView) return fov;
    g_gameFov = FovGameFov(fov);
    return g_gameFov;
}

float CompareGameFov() { return g_gameFov; }

// The left view's state, made once (it lives as long as the game).
static void* LeftViewState()
{
    static void* state;
    static bool tried;
    if (state || tried) return state;
    tried = true;
    static const BYTE kStart[7] = {0x6A, 0xFF, 0x68, 0x8B, 0x16, 0x44, 0x01};  // push -1; push 0144168Bh
    if (IsBadReadPtr((void*)0x007EF850, 7) || memcmp((void*)0x007EF850, kStart, 7) != 0) {
        Log("compare: AllocateViewState mismatch");
        return nullptr;
    }
    typedef void* (__cdecl* PFN_AllocateViewState)();
    state = ((PFN_AllocateViewState)0x007EF850)();
    Log("compare: left view state %p", state);
    return state;
}

bool CompareActive() { return g_on; }
bool CompareSplitNow() { return g_on && g_leftValid; }

void CompareSet(bool on)
{
    g_on = on;
    if (!on) g_leftValid = false;
    char path[MAX_PATH];
    wsprintfA(path, "%srowe.ini", ModDir());
    WritePrivateProfileStringA("rowe", "compare", on ? "1" : "0", path);
}
bool CompareSecondView() { return g_secondView; }

// FViewport::GetSizeX / GetSizeY (vtable slots 2 and 3).
static UINT ViewportSize(void* viewport, int slot)
{
    typedef UINT (__thiscall* PFN_Size)(void*);
    return ((PFN_Size)(*(void***)viewport)[slot])(viewport);
}

// A 16:9 rectangle in each half of a vw x vh screen, centred (fractions of the screen): left
// and right Origin X, and the shared Origin Y, Size X, Size Y.
void CompareLayout(float vw, float vh, float* leftX, float* rightX, float* y, float* w, float* h)
{
    *w = 0.5f;
    *h = vh > 0 ? 0.5f * vw / vh * 9.f / 16.f : 1.f;
    if (*h > 1.f) {  // a narrow screen: full height, narrower halves
        *h = 1.f;
        *w = vw > 0 ? vh * 16.f / 9.f / vw : 0.5f;
    }
    *leftX = (0.5f - *w) / 2;
    *rightX = 0.5f + *leftX;
    *y = (1.f - *h) / 2;
}

// Replaces the draw loop's CalcSceneView call (0x0048FE3A). First pass: the right view. Second
// pass (the player list holds the player twice): the list is put back, and the left view is made.
static void* __fastcall HookCalcSceneView(BYTE* player, void*, void* family, void* location, void* rotation, void* viewport)
{
    bool second = g_listSwapped;
    RestorePlayers();
    void* leftState = g_on && viewport && LensGameplayActive() ? LeftViewState() : nullptr;
    if (!leftState) {
        g_rightMade = g_leftMade = false;
        g_leftValid = false;
        return g_origCalcSceneView(player, family, location, rotation, viewport);
    }
    float* rect = (float*)(player + 0x68);  // Origin X, Origin Y, Size X, Size Y
    void** viewState = (void**)(player + 0x88);
    float saved[4] = {rect[0], rect[1], rect[2], rect[3]};
    void* savedState = *viewState;

    float leftX, rightX, y, w, h;
    CompareLayout((float)ViewportSize(viewport, 2), (float)ViewportSize(viewport, 3), &leftX, &rightX, &y, &w, &h);
    rect[0] = second ? leftX : rightX;
    rect[1] = y;
    rect[2] = w;
    rect[3] = h;
    if (second) {
        *viewState = leftState;
        g_secondView = true;
    }
    void* view = g_origCalcSceneView(player, family, location, rotation, viewport);
    g_secondView = false;
    *viewState = savedState;
    for (int i = 0; i < 4; ++i) rect[i] = saved[i];
    g_rightMade = !second && view;
    g_leftMade = second && view;
    if (second) {
        g_leftValid = false;
        if (view) {
            memcpy(g_left, (BYTE*)view + 0x1C, 16);  // FSceneView X, Y, SizeX, SizeY
            g_leftValid = true;
        }
    }
    return view;
}

// Replaces the audio listener update that ends each pass of the draw loop (0x004906CA, a
// __thiscall with three arguments): after the right view's pass, the player is listed twice so
// the loop runs again for the left view.
typedef void (__thiscall* PFN_ListenerUpdate)(void* self, void* location, float a, float b);
static PFN_ListenerUpdate g_origListenerUpdate;

static void __fastcall HookListenerUpdate(void* self, void*, void* location, float a, float b)
{
    g_origListenerUpdate(self, location, a, b);
    if (!g_rightMade) return;
    g_rightMade = false;
    DoublePlayers();
}

// Replaces the statistics scope that opens the HUD loop (0x00491152, __thiscall, one argument):
// the player is listed twice for it when the frame has a left view.
typedef void (__thiscall* PFN_ScopeBegin)(void* scope, int stat);
static PFN_ScopeBegin g_origScopeBegin;

static void __fastcall HookHudLoopBegin(void* scope, void*, int stat)
{
    g_origScopeBegin(scope, stat);
    if (!g_leftMade) return;
    g_leftMade = false;
    DoublePlayers();
}

// Replaces UCanvas::Init right after the HUD loop (0x0049152C; it does nothing): the list is put back.
typedef void (__thiscall* PFN_CanvasInit)(void* canvas);
static PFN_CanvasInit g_origCanvasInit;

static void __fastcall HookHudLoopEnd(void* canvas, void*)
{
    RestorePlayers();
    g_origCanvasInit(canvas);
}

void CompareInit()
{
    char path[MAX_PATH];
    wsprintfA(path, "%srowe.ini", ModDir());
    g_on = GetPrivateProfileIntA("rowe", "compare", 0, path) != 0;
}

void CompareInstall()
{
    g_origCalcSceneView = (PFN_CalcSceneView)PatchCall(0x0048FE3A, 0x0048ABC0, (void*)HookCalcSceneView, "compare-view");
    if (g_origCalcSceneView) g_origGetFOVAngle = (PFN_GetFOVAngle)PatchCall(0x0048AD62, 0x00485200, (void*)HookGetFOVAngle, "compare-fov");
    if (g_origGetFOVAngle)
        g_origListenerUpdate = (PFN_ListenerUpdate)PatchCall(0x004906CA, 0x0047C0F0, (void*)HookListenerUpdate, "compare-pass");
    // The HUD loop's pair (both or neither: a doubled list must always be put back).
    BYTE* b = (BYTE*)0x00491152;
    BYTE* e = (BYTE*)0x0049152C;
    if (g_origListenerUpdate && !IsBadReadPtr(b, 5) && !IsBadReadPtr(e, 5) && b[0] == 0xE8 && e[0] == 0xE8 &&
        0x00491152 + 5 + *(int*)(b + 1) == 0x00405470 && 0x0049152C + 5 + *(int*)(e + 1) == 0x0055AD80) {
        g_origCanvasInit = (PFN_CanvasInit)PatchCall(0x0049152C, 0x0055AD80, (void*)HookHudLoopEnd, "compare-hud-end");
        g_origScopeBegin = (PFN_ScopeBegin)PatchCall(0x00491152, 0x00405470, (void*)HookHudLoopBegin, "compare-hud-begin");
    }
    Log("compare: %s, view hook %d, FOV hook %d, pass hook %d, HUD hooks %d", g_on ? "on" : "off", !!g_origCalcSceneView,
        !!g_origGetFOVAngle, !!g_origListenerUpdate, g_origScopeBegin && g_origCanvasInit);
}
