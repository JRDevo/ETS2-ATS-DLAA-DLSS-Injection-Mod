@echo off
setlocal
rem Phase 77 unit test of src\cutout_mips.h (coverage-preserving binary alpha mips for cut-out BC3 textures).
rem No D3D, no game, no build tree needed. Output: build\tests\cutout_mips_test.exe
rem Run: build\tests\cutout_mips_test.exe   (exit code 0 = all checks passed; the captured ATS fence is used when present)
rem vswhere finds any Visual Studio edition (Community here, Enterprise on the GitHub runner)
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`call "%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0.."
if not exist build\tests\obj_cutoutmips mkdir build\tests\obj_cutoutmips
cl /nologo /std:c++17 /EHsc /O2 /W4 /I src tests\cutout_mips_test.cpp /Fe:build\tests\cutout_mips_test.exe /Fo:build\tests\obj_cutoutmips\ /link ole32.lib windowscodecs.lib > build\tests\build_cutout_mips_test.txt 2>&1
echo exit %ERRORLEVEL% >> build\tests\build_cutout_mips_test.txt
type build\tests\build_cutout_mips_test.txt
