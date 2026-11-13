@echo off
setlocal
rem Phase 100 unit test of src\motion_blur.h + its HLSL on WARP (no GPU needed): still picture, smear, speed weight,
rem DLSS upscale scale, MaxLen clamp, NaN vectors, 50 %% strength.
rem Output: build\tests\motion_blur_test.exe   Run: build\tests\motion_blur_test.exe   (exit code 0 = all checks passed)
rem vswhere finds any Visual Studio edition (Community here, Enterprise on the GitHub runner)
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`call "%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0.."
if not exist build\tests\obj_motionblur mkdir build\tests\obj_motionblur
cl /nologo /std:c++17 /EHsc /O2 /W4 /DNOMINMAX /I src tests\motion_blur_test.cpp /Fe:build\tests\motion_blur_test.exe /Fo:build\tests\obj_motionblur\ /link d3d11.lib d3dcompiler.lib > build\tests\build_motion_blur_test.txt 2>&1
echo exit %ERRORLEVEL% >> build\tests\build_motion_blur_test.txt
type build\tests\build_motion_blur_test.txt
