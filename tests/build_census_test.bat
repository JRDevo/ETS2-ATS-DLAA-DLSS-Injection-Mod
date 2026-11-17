@echo off
setlocal
rem Phase 108 unit test of src\census.h (the shader census: DXBC classifier on fxc look-alikes + the real game bytecode under
rem E:\ETS2-DLAA-Injector\captures\ when present, the per-pair verdict with wire_widen's known sets, thin rule, texts, DDS header).
rem Output: build\tests\census_test.exe   Run: build\tests\census_test.exe   (exit code 0 = all checks passed)
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`call "%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0.."
if not exist build\tests\obj_census mkdir build\tests\obj_census
cl /nologo /std:c++17 /EHsc /O2 /W4 /DNOMINMAX /DWITH_DLAA=1 /DWW_NO_D3D /I src tests\census_test.cpp src\wire_widen.cpp /Fe:build\tests\census_test.exe /Fo:build\tests\obj_census\ /link d3d11.lib d3dcompiler.lib > build\tests\build_census_test.txt 2>&1
echo exit %ERRORLEVEL% >> build\tests\build_census_test.txt
type build\tests\build_census_test.txt
