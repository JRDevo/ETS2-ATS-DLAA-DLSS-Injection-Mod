@echo off
setlocal
rem v0.12.0 issue #12 unit test of src\flat_views.h (flat multi-view / triple screen: layout, view index, confirm / drop, shared jitter phase).
rem No D3D, no game, no build tree needed. Output: build\tests\flat_views_test.exe
rem Run: build\tests\flat_views_test.exe   (exit code 0 = all checks passed)
rem vswhere finds any Visual Studio edition (Community here, Enterprise on the GitHub runner)
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`call "%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0.."
if not exist build\tests\obj_flatviews mkdir build\tests\obj_flatviews
cl /nologo /std:c++17 /EHsc /O2 /W4 /I src tests\flat_views_test.cpp /Fe:build\tests\flat_views_test.exe /Fo:build\tests\obj_flatviews\ > build\tests\build_flat_views_test.txt 2>&1
echo exit %ERRORLEVEL% >> build\tests\build_flat_views_test.txt
type build\tests\build_flat_views_test.txt
