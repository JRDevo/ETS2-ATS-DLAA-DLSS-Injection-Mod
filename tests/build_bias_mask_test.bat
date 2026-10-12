@echo off
setlocal
rem phase 49 WARP test for the glow bias mask (tests\bias_mask_test.cpp): the additive glow re-draw through DrawIdRecord::ReplayDepth
rem against a read-only scene depth, the share pass (DrawIdRecord::BiasShare: values, clip, state restore) and the crop step shader
rem (kBiasPrepShader). Output: build\tests\bias_mask_test.exe (exit code 0 = all checks passed).
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`call "%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0.."
if not exist build\tests\obj_bm mkdir build\tests\obj_bm
cl /nologo /std:c++17 /EHsc /O2 /W3 /DWITH_DLAA=1 /DNOMINMAX /I src tests\bias_mask_test.cpp src\motion_vectors.cpp src\draw_ids.cpp src\shader_cache.cpp src\scene_dlaa.cpp src\gpu_perf.cpp tests\fake_dlaa.cpp /Fe:build\tests\bias_mask_test.exe /Fo:build\tests\obj_bm\ /link d3d11.lib d3dcompiler.lib dxgi.lib > build\tests\build_bias_mask_test.txt 2>&1
echo exit %ERRORLEVEL% >> build\tests\build_bias_mask_test.txt
type build\tests\build_bias_mask_test.txt
