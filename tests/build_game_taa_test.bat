@echo off
setlocal
rem Phase 82 unit test of src\game_taa.h (the game's own TAA with the mod: dlaa.ini game_taa / game_taa_mix): DXBC checksum, live
rem window + jitter hold, the jitter from the TAA resolve's constants, the per-pass decision, the menu steps, the composite mix.
rem Pure logic, no GPU. Output: build\tests\game_taa_test.exe   Run: build\tests\game_taa_test.exe   (exit code 0 = all checks passed)
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`call "%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0.."
if not exist build\tests\obj_gametaa mkdir build\tests\obj_gametaa
cl /nologo /std:c++17 /EHsc /O2 /W4 /DNOMINMAX /I src tests\game_taa_test.cpp /Fe:build\tests\game_taa_test.exe /Fo:build\tests\obj_gametaa\ > build\tests\build_game_taa_test.txt 2>&1
echo exit %ERRORLEVEL% >> build\tests\build_game_taa_test.txt
type build\tests\build_game_taa_test.txt
