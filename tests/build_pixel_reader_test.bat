@echo off
setlocal
rem Phase 109a unit test of src\pixel_reader.h (the forward pixel readers' twin: DXBC checksum, the index-chain analysis on fxc
rem look-alikes, WARP bit-exactness of the twin vs the game shader + the overscan fix; the real game bytecode under
rem E:\ETS2-DLAA-Injector\captures\ when present).
rem Output: build\tests\pixel_reader_test.exe   Run: build\tests\pixel_reader_test.exe   (exit code 0 = all checks passed)
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`call "%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0.."
if not exist build\tests\obj_pixelreader mkdir build\tests\obj_pixelreader
cl /nologo /std:c++17 /EHsc /O2 /W4 /DNOMINMAX /I src tests\pixel_reader_test.cpp /Fe:build\tests\pixel_reader_test.exe /Fo:build\tests\obj_pixelreader\ /link d3d11.lib d3dcompiler.lib > build\tests\build_pixel_reader_test.txt 2>&1
echo exit %ERRORLEVEL% >> build\tests\build_pixel_reader_test.txt
type build\tests\build_pixel_reader_test.txt
