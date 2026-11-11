@echo off
setlocal
rem Phase 94 unit test of srcother_mods.h (game.log.txt mod / plugin lines, once-per-run filter).
rem Output: build\tests\other_mods_test.exe   Run: build\tests\other_mods_test.exe   (exit code 0 = all checks passed)
rem vswhere finds any Visual Studio edition (Community here, Enterprise on the GitHub runner)
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`call "%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0.."
if not exist build\tests\obj_othermods mkdir build\tests\obj_othermods
cl /nologo /std:c++17 /EHsc /O2 /W4 /DNOMINMAX /I src tests\other_mods_test.cpp /Fe:build\tests\other_mods_test.exe /Fo:build\tests\obj_othermods\ > build\tests\build_other_mods_test.txt 2>&1
echo exit %ERRORLEVEL% >> build\tests\build_other_mods_test.txt
type build\tests\build_other_mods_test.txt
