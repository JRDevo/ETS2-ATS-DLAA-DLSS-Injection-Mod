@echo off
setlocal
rem Phase 103 unit test of src\shadow_lock.h (own-vehicle shadow grid lock): block signature found / not found / false hits,
rem phase -> d, constant camera phase over drifting frames, y flip, forward-reader rows, other cascade sizes, pairing modes,
rem scan spans, raster emulation + a WARP check of the fractional-viewport sign.
rem Output: build\tests\shadow_lock_test.exe   Run: build\tests\shadow_lock_test.exe   (exit code 0 = all checks passed)
rem vswhere finds any Visual Studio edition (Community here, Enterprise on the GitHub runner)
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`call "%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0.."
if not exist build\tests\obj_shadowlock mkdir build\tests\obj_shadowlock
cl /nologo /std:c++17 /EHsc /O2 /W4 /DNOMINMAX /I src tests\shadow_lock_test.cpp /Fe:build\tests\shadow_lock_test.exe /Fo:build\tests\obj_shadowlock\ /link d3d11.lib d3dcompiler.lib > build\tests\build_shadow_lock_test.txt 2>&1
echo exit %ERRORLEVEL% >> build\tests\build_shadow_lock_test.txt
type build\tests\build_shadow_lock_test.txt
