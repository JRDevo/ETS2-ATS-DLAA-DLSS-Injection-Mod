@echo off
setlocal
rem Phase 97 unit test of src\water_ps.h (the new SCS water's checkerboard shaders, DXBC classifier on fxc look-alikes) + the
rem checker maths on WARP: W x H shifted = cell is the pixel (+ edge re-draw), (W+1) x (H+1) = the issue #30 wrong-parity blocks.
rem Output: build\tests\water_ps_test.exe   Run: build\tests\water_ps_test.exe   (exit code 0 = all checks passed)
rem vswhere finds any Visual Studio edition (Community here, Enterprise on the GitHub runner)
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`call "%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0.."
if not exist build\tests\obj_waterps mkdir build\tests\obj_waterps
cl /nologo /std:c++17 /EHsc /O2 /W4 /DNOMINMAX /I src tests\water_ps_test.cpp /Fe:build\tests\water_ps_test.exe /Fo:build\tests\obj_waterps\ /link d3d11.lib d3dcompiler.lib > build\tests\build_water_ps_test.txt 2>&1
echo exit %ERRORLEVEL% >> build\tests\build_water_ps_test.txt
type build\tests\build_water_ps_test.txt
