@echo off
setlocal
rem v0.12.1 phase 50 unit test (issue #16): src\hotkey_scan.h (the Hotkeys page's fresh-press detector) + src\dinput_wrap.cpp's
rem per-input hiding (KeySwallow::SetMenuHide, the keyboard / controller filters, ControllerState's known slots).
rem No D3D, no game, no build tree needed. Output: build\tests\hotkey_scan_test.exe
rem Run: build\tests\hotkey_scan_test.exe   (exit code 0 = all checks passed)
rem vswhere finds any Visual Studio edition (Community here, Enterprise on the GitHub runner)
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`call "%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0.."
if not exist build\tests\obj_hkscan mkdir build\tests\obj_hkscan
cl /nologo /std:c++17 /EHsc /O2 /W3 /DNOMINMAX /I src tests\hotkey_scan_test.cpp /Fe:build\tests\hotkey_scan_test.exe /Fo:build\tests\obj_hkscan\ /link dinput8.lib dxguid.lib user32.lib > build\tests\build_hotkey_scan_test.txt 2>&1
echo exit %ERRORLEVEL% >> build\tests\build_hotkey_scan_test.txt
type build\tests\build_hotkey_scan_test.txt
