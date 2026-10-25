@echo off
rem Builds the 32-bit d3d9.dll proxy and the test harness into out\.
rem Needs Visual Studio 2017 or newer (or its Build Tools) with the C++ x86/x64 tools and a
rem Windows SDK (for fxc and the D3D10 headers). Run it from any folder.
setlocal

rem Use an existing x86 developer environment, otherwise find Visual Studio with vswhere.
if /i "%VSCMD_ARG_TGT_ARCH%"=="x86" goto build
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (echo vswhere.exe not found: install Visual Studio or the VS Build Tools & exit /b 1)
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (echo No Visual Studio installation with the C++ tools found & exit /b 1)
rem (vcvars32 prints harmless "not found" messages on some installs, hence 2>nul.)
call "%VSDIR%\VC\Auxiliary\Build\vcvars32.bat" >nul 2>nul
where cl >nul 2>nul || (echo vcvars32.bat did not set up the compiler & exit /b 1)

:build
set "SRC=%~dp0src"
set "OUT=%~dp0out"
if not exist "%OUT%" mkdir "%OUT%"

rem Shaders are compiled to headers (byte arrays) and built into the DLL.
fxc /nologo /T vs_4_0 /E VSMain /Vn g_vsMain /Fh "%OUT%\overlay_vs.h" "%SRC%\overlay.hlsl" >nul || exit /b 1
fxc /nologo /T ps_4_0 /E PSMain /Vn g_psMain /Fh "%OUT%\overlay_ps.h" "%SRC%\overlay.hlsl" >nul || exit /b 1
fxc /nologo /T vs_4_0 /E VSMain /Vn g_lensVS /Fh "%OUT%\lens_vs.h" "%SRC%\lens.hlsl" >nul || exit /b 1
fxc /nologo /T ps_4_0 /E PSMain /Vn g_lensPS /Fh "%OUT%\lens_ps.h" "%SRC%\lens.hlsl" >nul || exit /b 1
fxc /nologo /T vs_4_0 /E VSMain /Vn g_downscaleVS /Fh "%OUT%\downscale_vs.h" "%SRC%\downscale.hlsl" >nul || exit /b 1
fxc /nologo /T ps_4_0 /E PSMain /Vn g_downscalePS /Fh "%OUT%\downscale_ps.h" "%SRC%\downscale.hlsl" >nul || exit /b 1

rem /MT links the C runtime statically, so the DLL needs no Visual C++ redistributable.
cl /nologo /O2 /MT /EHsc /W3 /D_CRT_SECURE_NO_WARNINGS /LD /I"%OUT%" /Fo"%OUT%\\" /Fe"%OUT%\d3d9.dll" ^
  "%SRC%\dllmain.cpp" "%SRC%\detour.cpp" "%SRC%\crashlog.cpp" "%SRC%\dx10hook.cpp" "%SRC%\fov.cpp" ^
  "%SRC%\lens.cpp" "%SRC%\shoulder.cpp" "%SRC%\supersample.cpp" "%SRC%\overlay.cpp" "%SRC%\intros.cpp" ^
  "%SRC%\screenshot.cpp" "%SRC%\mousetrap.cpp" "%SRC%\launch.cpp" "%SRC%\background.cpp" "%SRC%\gamecmd.cpp" "%SRC%\textcapture.cpp" "%SRC%\fovtrace.cpp" ^
  /link /EXPORT:Direct3DCreate9 user32.lib kernel32.lib d3d10.lib ole32.lib oleaut32.lib windowscodecs.lib || exit /b 1

cl /nologo /O2 /MT /EHsc /W3 /D_CRT_SECURE_NO_WARNINGS /Fo"%OUT%\\" /Fe"%OUT%\harness.exe" "%~dp0test\harness.cpp" ^
  /link user32.lib gdi32.lib d3d10.lib || exit /b 1
echo Built %OUT%\d3d9.dll

rem The launcher (settings, save slots, continue / new campaign), Dear ImGui on Direct3D 9.
set "IMGUI=%~dp0third_party\imgui"
if not exist "%OUT%\launcher" mkdir "%OUT%\launcher"
cl /nologo /O2 /MT /EHsc /W3 /std:c++17 /D_CRT_SECURE_NO_WARNINGS /DIMGUI_IMPL_WIN32_DISABLE_GAMEPAD /I"%IMGUI%" /I"%~dp0third_party\fonts" ^
  /Fo"%OUT%\launcher\\" /Fe"%OUT%\gowfov_launcher.exe" "%SRC%\launcher\launcher.cpp" ^
  "%IMGUI%\imgui.cpp" "%IMGUI%\imgui_draw.cpp" "%IMGUI%\imgui_tables.cpp" "%IMGUI%\imgui_widgets.cpp" ^
  "%IMGUI%\backends\imgui_impl_dx9.cpp" "%IMGUI%\backends\imgui_impl_win32.cpp" ^
  /link /SUBSYSTEM:WINDOWS user32.lib gdi32.lib kernel32.lib shell32.lib shlwapi.lib ole32.lib windowscodecs.lib d3d9.lib dwmapi.lib || exit /b 1
echo Built %OUT%\gowfov_launcher.exe
