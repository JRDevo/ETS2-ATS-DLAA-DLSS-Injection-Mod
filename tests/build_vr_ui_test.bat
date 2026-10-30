@echo off
setlocal
rem Phase 83 unit test of src\vr_ui.h (the game's VR UI placement typed into its console): config.cfg parsing,
rem the menu steps, the command text, the debounce / queue and the keystroke plan.
rem Pure logic, no GPU. Output: build\tests\vr_ui_test.exe   Run: build\tests\vr_ui_test.exe   (exit code 0 = all checks passed)
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`call "%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0.."
if not exist build\tests\obj_vrui mkdir build\tests\obj_vrui
cl /nologo /std:c++17 /EHsc /O2 /W4 /DNOMINMAX /I src tests\vr_ui_test.cpp /Fe:build\tests\vr_ui_test.exe /Fo:build\tests\obj_vrui\ > build\tests\build_vr_ui_test.txt 2>&1
echo exit %ERRORLEVEL% >> build\tests\build_vr_ui_test.txt
type build\tests\build_vr_ui_test.txt
