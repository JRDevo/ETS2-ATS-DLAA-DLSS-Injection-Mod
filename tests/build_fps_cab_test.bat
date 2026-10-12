@echo off
setlocal
rem phase 47: fps_cab_test -- the VR fps box attached to the vehicle (src\fps_cab.h maths; WARP: the real DrawIdRecord + FpsCab
rem runtime + menu.cpp VSCab, a swaying cab, zero-lag check against the same frame's truth). Output: build\tests\fps_cab_test.exe
rem (exit code 0 = all checks passed).
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`call "%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0.."
if not exist build\tests\obj_fc mkdir build\tests\obj_fc
cl /nologo /std:c++17 /EHsc /O2 /W3 /DWITH_DLAA=1 /DNOMINMAX /I src tests\fps_cab_test.cpp src\fps_cab.cpp src\draw_ids.cpp src\menu.cpp src\shader_cache.cpp src\gpu_perf.cpp /Fe:build\tests\fps_cab_test.exe /Fo:build\tests\obj_fc\ /link d3d11.lib d3dcompiler.lib dxgi.lib gdi32.lib user32.lib > build\tests\build_fps_cab_test.txt 2>&1
echo exit %ERRORLEVEL% >> build\tests\build_fps_cab_test.txt
type build\tests\build_fps_cab_test.txt
