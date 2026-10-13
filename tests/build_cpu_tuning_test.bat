@echo off
setlocal
rem Phase 54 unit test of src\cpu_tuning_pick.h (CPU tuning: P-core pick, narrowed-affinity rule, menu row state).
rem No D3D, no game, no build tree needed. Output: build\tests\cpu_tuning_test.exe
rem Run: build\tests\cpu_tuning_test.exe   (exit code 0 = all checks passed)
rem vswhere finds any Visual Studio edition (Community here, Enterprise on the GitHub runner)
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`call "%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0.."
if not exist build\tests\obj_cputune mkdir build\tests\obj_cputune
cl /nologo /std:c++17 /EHsc /O2 /W4 /I src tests\cpu_tuning_test.cpp /Fe:build\tests\cpu_tuning_test.exe /Fo:build\tests\obj_cputune\ > build\tests\build_cpu_tuning_test.txt 2>&1
echo exit %ERRORLEVEL% >> build\tests\build_cpu_tuning_test.txt
type build\tests\build_cpu_tuning_test.txt
