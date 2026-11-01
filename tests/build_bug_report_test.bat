@echo off
setlocal
rem Phase 89b-d unit test of src\bug_report.h (no GPU): CRC-32, stored zip read back, privacy scrubber, capture plan, rewind length, keep-N pruning, double press.
rem Output: build\tests\bug_report_test.exe   Run: build\tests\bug_report_test.exe   (exit code 0 = all checks passed)
rem vswhere finds any Visual Studio edition (Community here, Enterprise on the GitHub runner)
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`call "%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0.."
if not exist build\tests\obj_bugreport mkdir build\tests\obj_bugreport
cl /nologo /std:c++17 /EHsc /O2 /W4 /DNOMINMAX /I src tests\bug_report_test.cpp /Fe:build\tests\bug_report_test.exe /Fo:build\tests\obj_bugreport\ /link advapi32.lib > build\tests\build_bug_report_test.txt 2>&1
echo exit %ERRORLEVEL% >> build\tests\build_bug_report_test.txt
type build\tests\build_bug_report_test.txt
