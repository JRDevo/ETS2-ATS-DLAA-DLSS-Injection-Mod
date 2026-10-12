@echo off
rem phase 40 WARP test for the VR hidden area mask (tests\hidden_mask_test.cpp): the view-tangent -> NDC mapping + its sanity
rem checks, and the depth-only draw on a D32S8 depth (nearest value inside, untouched outside, stencil untouched, every D3D11
rem state restored). Output: build\tests\hidden_mask_test.exe (exit code 0 = all checks passed).
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`call "%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0.."
if not exist build\tests\obj_hm mkdir build\tests\obj_hm
cl /nologo /std:c++17 /EHsc /O2 /W3 /DWITH_DLAA=1 /DNOMINMAX /I src tests\hidden_mask_test.cpp src\hidden_mask.cpp src\preview_blit.cpp src\shader_cache.cpp /Fe:build\tests\hidden_mask_test.exe /Fo:build\tests\obj_hm\ /link d3d11.lib d3dcompiler.lib dxgi.lib > build\tests\build_hidden_mask_test.txt 2>&1
echo exit %ERRORLEVEL% >> build\tests\build_hidden_mask_test.txt
type build\tests\build_hidden_mask_test.txt
