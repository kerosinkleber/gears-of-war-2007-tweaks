# Gears of War (2007) Tweaks

A mod for **Gears of War for Windows** (PC, 2007) that adds:

- **FOV slider** (60–159°), and an **aim FOV** for how far aiming zooms in. Sniper scopes keep their zoom.
- **Lens correction** in the style of [flex-fov](https://github.com/shaunlebron/flex-fov): very wide FOVs without stretched edges (Panini, stereographic or fisheye projection).
- **Shoulder swap** on Tab, with a setting for the shoulder the camera starts on.
- **Supersampling** up to 8× (renders at a higher resolution and averages it down). The HUD, menus and subtitles keep their normal size, and it can be changed while playing.
- **Mouse trap**: keeps the cursor on the game's monitor, during gameplay only or in menus too, so it can't drift onto another screen.
- **Intro skip**: no ESRB, Epic and Microsoft Game Studios videos at startup. Loading screens still play.
- An in-game **settings menu** (F8).
- **Screenshots** (F9 by default) as JPEG or PNG, at the full supersampled resolution or the screen's, to a folder you choose. They are written in the background, so the game doesn't stall.
- A **launcher** (`gowfov_launcher.exe`): all settings before the game starts, **save slots** for the campaign, and a direct start into **Continue** or a **New campaign** at a chosen difficulty.

It is a single `d3d9.dll` that goes next to the game's executable. No game files are modified. Its own files keep the mod's original name, gowfov (`gowfov.ini`, `gowfov.log`).

## Requirements

- Gears of War for Windows with `WarGame-G4WLive.exe` version **1.0.3340.131** (right-click → Properties → Details). The mod patches the game's code at fixed addresses. Each code patch checks the bytes it replaces first, so on another build that feature switches itself off instead of crashing.
- The game in **DirectX 10 mode**. In `Documents\My Games\Gears of War for Windows\WarGame\Config\WarEngineUserSettings.ini`:
  ```ini
  ALLOWD3D10=True
  ```
  In DirectX 9 mode only the FOV setting, the start shoulder and the intro skip work. The settings menu, lens correction, supersampling, screenshots and the Tab shoulder swap need DirectX 10.
- No other mod that uses a `d3d9.dll` in the same folder.

## Installing

1. Download `d3d9.dll` from the [releases](https://github.com/kerosinkleber/gears-of-war-2007-tweaks/releases), or build it yourself (see below).
2. Copy it into the game's `Binaries` folder, next to `WarGame-G4WLive.exe`.
3. Start the game as usual through `Startup.exe`, or through the launcher (below).

To uninstall, delete `d3d9.dll`, plus `gowfov.ini` and `gowfov.log` if they exist (and `gowfov_launcher.exe` and the `gowfov_saves` folder if you used the launcher).

## Launcher

`gowfov_launcher.exe` goes into the same `Binaries` folder. It has three tabs:

- **Play**:
  - **Continue campaign** starts the game and loads the checkpoint, without clicking through the menus.
  - **New campaign** on Casual, Hardcore or Insane starts the game and goes through its own menus (Campaign, Single Player, New Campaign, the difficulty, and the checkpoint overwrite question) by itself. Insane is only offered by the game after you finish the campaign; if it is locked, the launcher's request stops at the difficulty screen.
  - **Save slots**: the game has only one checkpoint, and a new campaign overwrites it. The launcher keeps copies of it as named slots. **Save current campaign** stores the current checkpoint; **Restore selected** puts a slot back, so the next Continue loads it. Before a new campaign or a restore, the current checkpoint is stored as a slot by itself (unless a slot already holds exactly it).
- **Mod settings**: everything in `gowfov.ini`.
- **Game settings**: resolution, fullscreen, DirectX 10, VSync, antialiasing, anisotropic filtering, detail and effects, in the game's `WarEngineUserSettings.ini`.

Settings are saved as you change them. Desktop shortcuts can start the game at once: `gowfov_launcher.exe --continue`, or `--new casual` / `--new hardcore` / `--new insane`.

Notes:

- The checkpoint lives in the player profile. Save slots work with the XLiveEmu GFWL replacement, which keeps the profile in `Binaries\XLiveEmu\<profile>\Offline\Title1–3.dat`. Those files are encrypted for your Windows account, so slots cannot be moved to another PC or user.
- A slot holds the whole profile file, so restoring one also brings back that time's options and unlocked chapters.
- Starting into Continue or a new campaign reads the game's English menu texts. With another game language the launcher's request stops at the menu and you continue by hand.
- Slots are folders in `Binaries\gowfov_saves`; each has a `slot.ini` with its name and date.

## Using it

| Key | In game |
|---|---|
| F8 | Open or close the settings menu |
| Up / Down | Select a setting (menu open) |
| Left / Right | Change it; hold for the FOV, Shift steps the FOV by 10 |
| Home | FOV back to the game default |
| Tab | Swap the camera to the other shoulder |
| F9 | Screenshot (key, folder, format and size: see the `screenshot...` settings) |

Settings are saved to `gowfov.ini` next to the DLL the first time one is changed:

| Key | Values | Default |
|---|---|---|
| `fov` | `0` = game default, otherwise 60–159. This is the FOV of the normal camera (the game uses 80); wider camera modes get the same increase up to 159°, and aiming is set by `aimfov`. | `0` |
| `aimfov` | `0` = game default (40), otherwise 30–120 (at most the `fov` value): the FOV while aiming. Camera modes zoomed in further than aiming (sniper scopes, 25° and below) keep their FOV. | `0` |
| `lens` | `0` off, `1` auto (rectilinear up to 90°, blending into Panini by 160°), `2` Panini, `3` stereographic, `4` fisheye (equidistant: angles spread evenly from the centre; above about 140° the corners beyond the rendered view stay black, like a fisheye lens' vignette) | `1` |
| `supersample` | Pixel-count multiplier, 1–9 (`4` = twice the width and height). Fullscreen only. | `1` |
| `shoulder` | `right` or `left`: the shoulder the camera starts on | `right` |
| `mousetrap` | `0` off (the game's own behaviour), `1` keep the cursor on the game's monitor during gameplay and free it in menus, pause, loading screens and cutscenes, `2` keep it there in menus too. Only while the game is in front; switching to another program frees the cursor. | `0` |
| `skipintros` | `1` skip the intro videos, `0` play them (applies at the next start) | `1` |
| `screenshotkey` | Screenshot key: `F1`–`F24`, `A`–`Z`, `0`–`9`, `Num0`–`Num9`, `Home`, `End`, `Insert`, `Delete`, `PageUp`, `PageDown`, `Pause`, `ScrollLock`, `PrintScreen`, or a virtual key code such as `0x78`. Avoid F8 (settings menu) and keys the game uses. | `F9` |
| `screenshotdir` | Folder for screenshots, created if missing. `%VARIABLES%` are expanded (e.g. `%USERPROFILE%\Pictures\Gears`); a relative path starts at the `Binaries` folder. Empty: the `Binaries` folder. | empty |
| `screenshotformat` | `jpg` (small and quick to write) or `png` (lossless). | `jpg` |
| `screenshotquality` | JPEG quality, 1–100. | `95` |
| `screenshotsize` | `full`: the size the game renders at (with supersampling, e.g. 7240×4072 at 8×); `screen`: the screen's size. | `full` |
| `background` | Developer tool, see below. Leave at `0`. | `0` |

Notes:

- The FOV is capped at 159°: the game renders one view, so the lens can't go much wider anyway (about 160° with Panini, 147° with stereographic, where a higher setting is capped at 147°; the fisheye goes to 160° with black corners). Near the cap, camera modes that are normally wider (such as the roadie run) can't widen any further.
- Supersampling is demanding: 8× renders eight times as many pixels. Lower it if the frame rate drops.

## How it works

The game imports `d3d9.dll` even when it renders with DirectX 10, so a proxy DLL with that name is loaded at startup. It forwards the D3D9 exports to the system's `d3d9.dll` and patches the game in memory.

| Feature | How |
|---|---|
| FOV | Detours `AdjustFOVForViewport`, the last step of the camera's FOV. The camera mode's FOV is remapped there to the chosen FOV and then to the wider FOV that the lens needs rendered. |
| Lens | After each 3D scene render command, before the HUD is drawn, a full-screen pass resamples the wide rectilinear render into the lens projection (`lens.hlsl`). The engine blends the FOV between camera modes after the FOV hook, so the warp reads the FOV each frame was actually rendered with from the scene's projection matrix. |
| Shoulder | Detours the camera mode's `GetViewOffset` and mirrors the sideways offset, eased over a quarter second. |
| Supersampling | The engine's viewport is resized to a larger "virtual" size. The real swap chain stays at the display size; the engine renders into a virtual-size texture, which is averaged down in linear light at Present (`downscale.hlsl`). The HUD, menus and subtitles lay out at the display size and are drawn under a scale transform. Menu mouse hit tests are kept in display pixels. |
| Intro skip | The startup movie queue is filtered right after it is built. |
| Settings menu | Drawn with its own D3D10 quads and a built-in bitmap font after the frame is finished. |

| File | Contents |
|---|---|
| `src/dllmain.cpp` | Proxy exports, logging, startup order |
| `src/detour.cpp` | Function detours and import table patches |
| `src/dx10hook.cpp` | Swap chain hooks (Present, GetBuffer, ResizeBuffers, ...) |
| `src/fov.cpp` | FOV hook and setting |
| `src/lens.cpp`, `src/lens.hlsl` | Lens projection math, warp pass, scene command hooks |
| `src/shoulder.cpp` | Shoulder swap |
| `src/mousetrap.cpp` | Mouse trap |
| `src/supersample.cpp`, `src/downscale.hlsl` | Supersampling |
| `src/overlay.cpp`, `src/overlay.hlsl` | F8 settings menu |
| `src/intros.cpp` | Intro skip |
| `src/screenshot.cpp` | Screenshots (GPU copy, read back over a few frames, JPEG/PNG encoding on a worker thread) |
| `src/crashlog.cpp` | Writes the crash address and stack to `gowfov.log` |
| `src/launch.cpp` | Carries out the launcher's start request (continue, or a new campaign through the menus) |
| `src/launcher/launcher.cpp` | The launcher |
| `third_party/` | Dear ImGui (MIT) and Share Tech Mono (SIL OFL), each with its licence, for the launcher |
| `src/background.cpp`, `src/gamecmd.cpp`, `src/textcapture.cpp`, `src/fovtrace.cpp` | Developer tooling (see below) |
| `test/harness.cpp` | Loads the DLL outside the game and renders the settings menu |

## Building

Needs Visual Studio 2017 or newer (the free Build Tools are enough) with the C++ x86/x64 tools and a Windows SDK. Run:

```bat
build.bat
```

The output is `out\d3d9.dll`, a 32-bit DLL with the C runtime linked in statically, so no Visual C++ redistributable is needed, and `out\gowfov_launcher.exe`. The script also builds `out\harness.exe`. That program loads the DLL, renders a few frames with the settings menu forced open, and saves `out\harness.bmp`.

## Developer tooling

These exist to diagnose and test the mod without a person at the keyboard. Normal play doesn't use any of them.

- `gowfov.log` records what was hooked, settings changes and, on a crash, the faulting address with the return addresses found on the stack.
- `background=1` keeps the game running and accepting input while its window is in the background.
  Keys and mouse buttons held by the tools count as held down. A tool cursor replaces the real one inside the game.
- `gowfov_cmd.txt`: commands, one per line, run on the game thread and then deleted:

  | Command | Effect |
  |---|---|
  | `continue` | Load the campaign checkpoint |
  | `travel <url>` | Travel to a map |
  | `key`, `keydown`, `keyup` `<name>` | Press, hold or release a key |
  | `rmb down` / `rmb up` | Right mouse button (aim) |
  | `mouse <x> <y>` | Move the cursor (client pixels) |
  | `lmb down` / `lmb up` | Left mouse button |
  | `ss <factor>` | Set supersampling |
- `gowfov_state.txt` is rewritten twice a second. It holds the frame count, whether the gameplay camera is active, the lens and supersampling state, and every string drawn on screen with its position and color. The highlighted menu entry is the one with alpha 0.99.
- Request files: create the file next to the DLL and the mod picks it up within a few frames:

  | File | Effect |
  |---|---|
  | `gowfov_shot.req` | Screenshot at the screen's size (`gowfov_shot_<date>_<time>.png`) |
  | `gowfov_scene.req` | Screenshot of the 3D scene before the HUD (`gowfov_scene_<date>_<time>.png`) |
  | `gowfov_reload.req` | Re-read `gowfov.ini` |
  | `gowfov_fovtrace.req` | Record 8 seconds of per-frame camera, FOV and lens data to `gowfov_fovtrace.csv` |

## License

MIT, see [LICENSE](LICENSE).

## Credits

- Lens projections follow Shaun LeBron's [flex-fov](https://github.com/shaunlebron/flex-fov).
- The launcher uses [Dear ImGui](https://github.com/ocornut/imgui) (MIT) and the Share Tech Mono font (SIL Open Font License).
- Gears of War is a trademark of Microsoft. This project is not affiliated with Microsoft or Epic Games and contains no game files.
