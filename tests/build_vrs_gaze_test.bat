@echo off
setlocal
rem Phase 59 unit test of the eye-tracked foveation maths (tests\vrs_gaze_test.cpp): vrs.cpp FillMaskRuns = FillMask byte for
rem byte (+ its speed at an ATS eye size), gaze_math.h GazeUv against an OpenXR fov projection, GazeToViewTangents signs, the
rem smoother and the fake gaze. vrs.cpp is compiled WITHOUT WITH_VRS (no NVAPI) and with VRS_NO_LOG; no D3D device, no game.
rem Output: build\tests\vrs_gaze_test.exe   (exit code 0 = all checks passed)
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`call "%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0.."
if not exist build\tests\obj_vgz mkdir build\tests\obj_vgz
cl /nologo /std:c++17 /EHsc /O2 /W4 /DVRS_NO_LOG /DNOMINMAX /I src tests\vrs_gaze_test.cpp src\vrs.cpp /Fe:build\tests\vrs_gaze_test.exe /Fo:build\tests\obj_vgz\ > build\tests\build_vrs_gaze_test.txt 2>&1
echo exit %ERRORLEVEL% >> build\tests\build_vrs_gaze_test.txt
type build\tests\build_vrs_gaze_test.txt
