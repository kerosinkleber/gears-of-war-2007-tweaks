// Start actions from the launcher. gowfov_launch.txt next to the DLL (written by
// gowfov_launcher.exe, read and deleted at startup by the game process):
//   action=continue            load the campaign checkpoint
//   action=new                 start a new campaign
//   difficulty=casual|hardcore|insane   (new campaign)
//
// Continue: past the title screen (which signs the profile in), then the same travel the
// menus use (WarGame_P?LoadSaveGame). New campaign: through the game's own menus, as a player
// would: Main Menu > Campaign > Single Player > New Campaign > Select Difficulty > accept the
// checkpoint overwrite. The menus set the difficulty (AWarPC::SetDifficultyLevel*_Worker) and
// write the new chapter-start checkpoint themselves. Every step waits for its screen, read from
// the text the game draws (the English menu texts), and the highlighted entry; anything
// unexpected or a timeout stops the sequence and leaves the game at that menu.
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "gowfov.h"

enum Action { kNone, kContinue, kNew };
enum Step { kTitle, kMainMenu, kCampaign, kSinglePlayer, kDifficulty, kOverwrite, kDone };

static Action g_action;
static const wchar_t* g_difficulty = L"Casual";
static Step g_step;
static DWORD g_stepStart, g_lastKey;
static int g_moves;  // focus moves on the current screen

void LaunchInit()
{
    char exe[MAX_PATH];
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    const char* name = strrchr(exe, '\\');
    if (lstrcmpiA(name ? name + 1 : exe, "WarGame-G4WLive.exe") != 0) return;  // not Startup.exe
    char path[MAX_PATH], val[32];
    wsprintfA(path, "%sgowfov_launch.txt", ModDir());
    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) return;
    GetPrivateProfileStringA("launch", "action", "", val, sizeof(val), path);
    if (lstrcmpiA(val, "continue") == 0) g_action = kContinue;
    else if (lstrcmpiA(val, "new") == 0) g_action = kNew;
    GetPrivateProfileStringA("launch", "difficulty", "casual", val, sizeof(val), path);
    if (lstrcmpiA(val, "hardcore") == 0) g_difficulty = L"Hardcore";
    else if (lstrcmpiA(val, "insane") == 0) g_difficulty = L"Insane";
    DeleteFileA(path);
    g_step = kTitle;
    g_stepStart = GetTickCount();
    if (g_action == kNew) Log("launch: new campaign on %ls", g_difficulty);
    else Log("launch: %s", g_action == kContinue ? "continue the campaign" : "unknown action, ignored");
}

static void Go(Step step)
{
    g_step = step;
    g_stepStart = GetTickCount();
    g_moves = 0;
}

static void Stop(const char* why)
{
    Log("launch: stopped (%s); the game stays at this menu", why);
    g_action = kNone;
}

static bool Shown(const wchar_t* text) { return TextFind(text, nullptr, nullptr); }

static bool Focused(const wchar_t* text)
{
    wchar_t f[80];
    return TextFocused(f, 80, nullptr) && lstrcmpW(f, text) == 0;
}

// Highlights `entry` on the current screen (Up/Down towards it); true once it is highlighted.
static bool FocusOn(const wchar_t* entry, DWORD now)
{
    if (Focused(entry)) return true;
    float want, at;
    wchar_t f[80];
    if (!TextFind(entry, &want, nullptr) || !TextFocused(f, 80, &at)) return false;
    if (now - g_lastKey < 400) return false;
    if (++g_moves > 8) {
        Stop("could not highlight the menu entry");
        return false;
    }
    if (GameCmdKey(want > at ? "down" : "up")) g_lastKey = now;
    return false;
}

// Presses Enter once the screen has settled.
static void Accept(Step next, DWORD now)
{
    if (now - g_stepStart < 500 || now - g_lastKey < 600) return;
    if (!GameCmdKey("enter")) return;  // the game window is not known yet: try again next frame
    g_lastKey = now;
    Go(next);
}

// The launcher's background: the title screen, saved once as gowfov_menubg.jpg next to the DLL
// in a frame where "Press Start" is blinked off, so only the artwork shows. It is taken on this
// PC from the user's own game, so the launcher ships no game art. The capture goes to a
// temporary file first and is kept only if the text comes back (it was the blink, not the
// player leaving the title screen).
static void MenuBackgroundTick(DWORD now)
{
    static bool done;
    static DWORD titleSince, gone, captured;
    if (done) return;
    char path[MAX_PATH], tmp[MAX_PATH];
    wsprintfA(path, "%sgowfov_menubg.jpg", ModDir());
    wsprintfA(tmp, "%sgowfov_menubg.tmp.jpg", ModDir());
    if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) {
        done = true;
        return;
    }
    bool text = Shown(L"Press ") || Shown(L" to Start");
    if (captured) {
        if (text && now - captured > 300) {  // the blink: keep it
            MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING);
            Log("launch: saved the launcher background %s", path);
            done = true;
        } else if (now - captured > 2000) {  // left the title screen: try again next time
            DeleteFileA(tmp);
            captured = 0;
            titleSince = 0;
        }
        return;
    }
    if (text) {
        if (!titleSince) titleSince = now;
        gone = 0;
        return;
    }
    if (!titleSince || now - titleSince < 3000) return;  // let the title screen fade in
    if (!gone) gone = now;
    if (now - gone < 100) return;  // a few frames without the text
    ScreenshotRequestTo(tmp);
    captured = now;
}

// Once per frame (game thread), after the text of the last frame is known.
void LaunchTick()
{
    MenuBackgroundTick(GetTickCount());
    if (g_action == kNone) return;
    DWORD now = GetTickCount();
    DWORD timeout = g_step == kTitle ? 120000 : 20000;
    if (now - g_stepStart > timeout) {
        Stop("timed out waiting for the next screen");
        return;
    }
    switch (g_step) {
    case kTitle:
        if (Shown(L"Main Menu")) Go(kMainMenu);
        else if (Shown(L"Press ") || Shown(L" to Start")) Accept(kMainMenu, now);
        break;
    case kMainMenu:
        if (!Shown(L"Main Menu")) break;
        if (g_action == kContinue) {
            if (now - g_stepStart < 500) break;
            Log("launch: loading the campaign checkpoint");
            GameCmdTravel("WarGame_P?LoadSaveGame");
            Go(kDone);
            break;
        }
        if (FocusOn(L"Campaign", now)) Accept(kCampaign, now);
        break;
    case kCampaign:
        if (Shown(L"Single Player") && FocusOn(L"Single Player", now)) Accept(kSinglePlayer, now);
        break;
    case kSinglePlayer:
        if (Shown(L"New Campaign") && FocusOn(L"New Campaign", now)) Accept(kDifficulty, now);
        break;
    case kDifficulty: {
        if (!Shown(L"Select Difficulty")) break;
        float alpha = 0;
        if (!TextFind(g_difficulty, nullptr, &alpha)) break;
        if (alpha < 0.5f) {
            Stop("that difficulty is locked (Insane unlocks after finishing the campaign)");
            return;
        }
        if (FocusOn(g_difficulty, now)) Accept(kOverwrite, now);
        break;
    }
    case kOverwrite:
        // With a campaign in progress the game asks before it overwrites the checkpoint.
        if (Shown(L"Checkpoint Overwrite")) {
            Accept(kDone, now);
        } else if (now - g_stepStart > 4000 && !Shown(L"Select Difficulty")) {
            Go(kDone);  // no checkpoint to overwrite: the campaign is already loading
        }
        break;
    case kDone:
        Log("launch: done");
        g_action = kNone;
        break;
    }
}
