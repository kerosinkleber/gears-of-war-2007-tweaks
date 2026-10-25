// gowfov: declarations shared between the modules, grouped by source file.
//
// Threads: the "game thread" is the engine's main thread (camera, UI, input, window messages);
// the "render thread" is the engine's rendering thread (scene commands, Present). Most hooks run
// on exactly one of them; values handed from one to the other are plain volatile variables.
//
// All engine addresses in this project are for WarGame-G4WLive.exe 1.0.3340.131 (image base
// 0x00400000). Code patches check the bytes they replace first and skip themselves on a mismatch.
#pragma once
#include <windows.h>
#include <d3d10.h>
#include <stdio.h>

// dllmain.cpp
void Log(const char* fmt, ...);  // appends a timestamped line to gowfov.log
const char* ModDir();            // folder of the DLL, with trailing backslash

// detour.cpp: returns the trampoline (call it to run the original), or null if not installed.
void* InstallDetour(DWORD targetVA, const BYTE* expected, int len, void* hook, const char* name);
void* PatchExeImport(const char* dll, const char* name, void* hook);  // returns the original function

void CrashLogInstall(HMODULE self);  // crashlog.cpp: logs access violations to gowfov.log
void IntrosInstall();                // intros.cpp: skips the startup intro videos
bool IntrosSkipped();                // setting (takes effect at the next launch)
void IntrosSetSkipped(bool skip);

// gamecmd.cpp: gowfov_cmd.txt commands and gowfov_state.txt status (see the file header).
void GameCmdTick();                   // once per frame, game thread
void GameCmdSetWindow(HWND window);
bool GameCmdKey(const char* name);    // posts a key press to the game window (false: no window yet)
void GameCmdTravel(const char* url);  // GEngine->SetClientTravel

// fovtrace.cpp: per-frame FOV pipeline trace (request: gowfov_fovtrace.req).
void FovTraceAdd(char kind, float a, float b, float c, float d, void* obj);
void FovTracePresent();

// textcapture.cpp: strings the engine draws each frame, written into gowfov_state.txt.
void TextCaptureInstall();
void TextCaptureFrame();
void TextCaptureWrite(FILE* f);
bool TextFind(const wchar_t* text, float* y, float* alpha);  // a string drawn last frame
bool TextFocused(wchar_t* out, int size, float* y);          // the highlighted menu entry

// launch.cpp: carries out gowfov_launch.txt (written by the launcher): continue the campaign or
// start a new one at a difficulty, through the game's own menus.
void LaunchInit();   // DllMain: reads and deletes the request (game process only)
void LaunchTick();   // once per frame, game thread

// background.cpp: gowfov.ini background=1 keeps the game active and accepting posted input
// while unfocused (for driving it from tools).
void BackgroundInit();
void BackgroundInstall();
bool BackgroundEnabled();
void BackgroundOnPresent(HWND window);
void BackgroundSetKeyHeld(UINT vk, bool held);  // tools' held keys read as down by the engine
void BackgroundSetCursor(int x, int y);         // tool cursor, game window client coordinates

// shoulder.cpp: Tab swaps the camera to the other shoulder; the start side is a setting.
void ShoulderInstall();
void ShoulderUpdate(bool focused);
bool ShoulderStartLeft();
void ShoulderSetStartLeft(bool left);  // saves; also switches the camera now

// mousetrap.cpp: keeps the cursor on the game's monitor while the game is in front.
enum { kMouseTrapOff = 0, kMouseTrapGameplay = 1, kMouseTrapAlways = 2, kMouseTrapModeCount = 3 };
void MouseTrapInit();               // reads gowfov.ini
void MouseTrapInstall();            // ClipCursor import
void MouseTrapFrame(HWND window);   // from Present
int MouseTrapGetMode();
void MouseTrapSetMode(int mode);    // saves to gowfov.ini

// dx10hook.cpp: hooks IDXGISwapChain (the game renders with D3D10).
void Dx10HookInstall();
void Dx10HookPatchSwapChain(IDXGISwapChain* swap);  // patches the vtable shared by this swap chain class
HRESULT SwapGetRealBuffer(IDXGISwapChain* swap, ID3D10Texture2D** out);  // bypasses supersampling

// supersample.cpp: engine renders at a virtual resolution, averaged down at Present.
void SupersampleInit();          // reads gowfov.ini
void SupersampleEarlyInstall();  // DllMain: redirects the engine's CreateDXGIFactory
void SupersampleInstall();       // engine hooks (viewport size, display modes, mouse)
float SupersampleSetting();      // configured multiplier (applies in fullscreen)
void SupersampleSetFactor(float factor);  // saves and applies at runtime
void SupersampleOnPresent(IDXGISwapChain* swap, HWND window);
// Swap chain glue for dx10hook.cpp: while supersampling, the engine's swap chain is "virtual".
bool SupersampleGetSizes(UINT* nativeW, UINT* nativeH, UINT* virtW, UINT* virtH);
void SupersampleCreateVirtual(ID3D10Device* dev, DXGI_FORMAT fmt);  // the texture the engine renders into
bool SupersampleOwns(IDXGISwapChain* swap);
HRESULT SupersampleGetBuffer(REFIID riid, void** out);
void SupersampleFixDesc(IDXGISwapChain* swap, DXGI_SWAP_CHAIN_DESC* desc);
bool SupersampleBeforeResize(IDXGISwapChain* swap, UINT* w, UINT* h);
void SupersampleAfterResize(IDXGISwapChain* swap, DXGI_FORMAT fmt);
void SupersampleFixMode(IDXGISwapChain* swap, DXGI_MODE_DESC* mode);
void SupersamplePresent(IDXGISwapChain* swap, ID3D10Texture2D* real);

// lens.cpp: hooks the engine's scene render commands (scene done, HUD not drawn yet) and
// warps the frame into the chosen lens projection.
enum { kLensOff = 0, kLensAuto = 1, kLensPanini = 2, kLensStereographic = 3, kLensFisheye = 4, kLensModeCount = 5 };
void LensInit();
void LensInstall();
void LensOnPresent(IDXGISwapChain* swap);
void LensOnResize();
float LensRenderFov(float lensFov);  // FOV the engine must render for this lens FOV
float LensEffectiveFov();            // lens FOV actually in use (0 when not warping)
bool LensGameplayActive();           // the gameplay camera updated within the last few frames
int LensGetMode();
void LensSetMode(int mode);  // saves to gowfov.ini

// overlay.cpp: the F8 settings menu (also polls the F9 screenshot and Tab shoulder keys).
void OverlayFrame(IDXGISwapChain* swap);  // from Present, after the supersampling resolve
void OverlayLost();                       // before the swap chain's buffers are resized

// screenshot.cpp: the screenshot key (JPEG/PNG, full supersampled or screen size, written in the
// background) and the tools' captures.
void ScreenshotInit();                                   // reads gowfov.ini
void ScreenshotFrame(IDXGISwapChain* swap, bool focused);  // from Present, before the overlay draws
void ScreenshotScene(IDXGISwapChain* swap);              // after the 3D scene: gowfov_scene.req
bool ScreenshotJpg();
void ScreenshotSetJpg(bool jpg);     // saves to gowfov.ini
bool ScreenshotFull();
void ScreenshotSetFull(bool full);   // saves to gowfov.ini
const char* ScreenshotKeyName();
void ScreenshotRequestUser();    // tools: a screenshot as the key takes it

// fov.cpp
// 0 means "game default" (no override); otherwise horizontal FOV in degrees, at most kFovMax.
// The single rendered view limits the lens to about 160 degrees, so wider settings add nothing.
static const float kFovMax = 159.f;  // highest FOV the mod sets or hands to the engine
void FovInit();
void FovInstall();  // detours the engine's FOV function; call once the engine is loaded
float FovGet();
void FovSet(float degrees);  // saves to gowfov.ini
// Aiming FOV: 0 = the game's (40), otherwise kAimFovMin..kAimFovMax. Scopes keep their zoom.
static const float kAimFovMin = 30.f, kAimFovMax = 120.f;  // also capped at the normal FOV
float FovGetAim();
void FovSetAim(float degrees);  // saves to gowfov.ini
