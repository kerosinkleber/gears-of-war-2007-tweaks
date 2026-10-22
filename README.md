# gowfov

A mod for **Gears of War for Windows** (PC, 2007) that adds:

- **FOV slider** (60–180°). Aiming keeps the game's own zoom.
- **Lens correction** in the style of [flex-fov](https://github.com/shaunlebron/flex-fov): very wide FOVs without stretched edges (Panini or stereographic projection).
- **Shoulder swap** on Tab, with a setting for the shoulder the camera starts on.
- **Supersampling** up to 8× (renders at a higher resolution and averages it down). The HUD, menus and subtitles keep their normal size, and it can be changed while playing.
- **Intro skip**: no ESRB, Epic and Microsoft Game Studios videos at startup. Loading screens still play.
- An in-game **settings menu** (F8) and **screenshots** (F9).

It is a single `d3d9.dll` that goes next to the game's executable. No game files are modified.

## Requirements

- Gears of War for Windows with `WarGame-G4WLive.exe` version **1.0.3340.131** (right-click → Properties → Details). The mod patches the game's code at fixed addresses. Each code patch checks the bytes it replaces first, so on another build that feature switches itself off instead of crashing.
- The game in **DirectX 10 mode**. In `Documents\My Games\Gears of War for Windows\WarGame\Config\WarEngineUserSettings.ini`:
  ```ini
  ALLOWD3D10=True
  ```
  In DirectX 9 mode only the FOV setting, the start shoulder and the intro skip work. The settings menu, lens correction, supersampling, F9 screenshots and the Tab shoulder swap need DirectX 10.
- No other mod that uses a `d3d9.dll` in the same folder.

## Installing

1. Download `d3d9.dll` from the releases, or build it yourself (see below).
2. Copy it into the game's `Binaries` folder, next to `WarGame-G4WLive.exe`.
3. Start the game as usual through `Startup.exe`.

To uninstall, delete `d3d9.dll`, plus `gowfov.ini` and `gowfov.log` if they exist.

## Using it

| Key | In game |
|---|---|
| F8 | Open or close the settings menu |
| Up / Down | Select a setting (menu open) |
| Left / Right | Change it; hold for the FOV, Shift steps the FOV by 10 |
| Home | FOV back to the game default |
| Tab | Swap the camera to the other shoulder |
| F9 | Screenshot (BMP in the `Binaries` folder) |

Settings are saved to `gowfov.ini` next to the DLL the first time one is changed:

| Key | Values | Default |
|---|---|---|
| `fov` | `0` = game default, otherwise 60–180. This is the FOV of the normal camera (the game uses 80); wider camera modes get the same increase, and aiming (≤ 50°) is left alone. | `0` |
| `lens` | `0` off, `1` auto (rectilinear up to 90°, blending into Panini by 160°), `2` Panini, `3` stereographic | `1` |
| `supersample` | Pixel-count multiplier, 1–9 (`4` = twice the width and height). Fullscreen only. | `1` |
| `shoulder` | `right` or `left`: the shoulder the camera starts on | `right` |
| `skipintros` | `1` skip the intro videos, `0` play them (applies at the next start) | `1` |
| `background` | Developer tool, see below. Leave at `0`. | `0` |

Notes:

- The game renders one view, so the lens is limited to about 160° (Panini) or 147° (stereographic). A higher FOV setting is capped there, and camera modes that are wider than normal (such as the roadie run) can then not widen any further.
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
| `src/supersample.cpp`, `src/downscale.hlsl` | Supersampling |
| `src/overlay.cpp`, `src/overlay.hlsl` | F8 settings menu |
| `src/intros.cpp` | Intro skip |
| `src/screenshot.cpp` | F9 screenshots |
| `src/crashlog.cpp` | Writes the crash address and stack to `gowfov.log` |
| `src/background.cpp`, `src/gamecmd.cpp`, `src/textcapture.cpp`, `src/fovtrace.cpp` | Developer tooling (see below) |
| `test/harness.cpp` | Loads the DLL outside the game and renders the settings menu |

## Building

Needs Visual Studio 2017 or newer (the free Build Tools are enough) with the C++ x86/x64 tools and a Windows SDK. Run:

```bat
build.bat
```

The output is `out\d3d9.dll`, a 32-bit DLL with the C runtime linked in statically, so no Visual C++ redistributable is needed. The script also builds `out\harness.exe`. That program loads the DLL, renders a few frames with the settings menu forced open, and saves `out\harness.bmp`.

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
  | `gowfov_shot.req` | Screenshot |
  | `gowfov_scene.req` | Screenshot of the 3D scene before the HUD |
  | `gowfov_reload.req` | Re-read `gowfov.ini` |
  | `gowfov_fovtrace.req` | Record 8 seconds of per-frame camera, FOV and lens data to `gowfov_fovtrace.csv` |

## License

MIT, see [LICENSE](LICENSE).

## Credits

- Lens projections follow Shaun LeBron's [flex-fov](https://github.com/shaunlebron/flex-fov).
- Gears of War is a trademark of Microsoft. This project is not affiliated with Microsoft or Epic Games and contains no game files.
