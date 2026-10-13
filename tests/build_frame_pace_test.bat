@echo off
setlocal
rem Phase 52 unit test of src\frame_pace.h (the flat limiter's deadline rule, the FPS cap steps, the VR FPS cap's wait target
rem against a model runtime). No D3D, no NVAPI, no game, no build tree needed. Output: build\tests\frame_pace_test.exe
rem Run: build\tests\frame_pace_test.exe   (exit code 0 = all checks passed)
rem vswhere finds any Visual Studio edition (Community here, Enterprise on the GitHub runner)
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`call "%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0.."
if not exist build\tests\obj_pace mkdir build\tests\obj_pace
cl /nologo /std:c++17 /EHsc /O2 /W4 /I src tests\frame_pace_test.cpp /Fe:build\tests\frame_pace_test.exe /Fo:build\tests\obj_pace\ > build\tests\build_frame_pace_test.txt 2>&1
echo exit %ERRORLEVEL% >> build\tests\build_frame_pace_test.txt
type build\tests\build_frame_pace_test.txt
