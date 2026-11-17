@echo off
setlocal
rem Phase 101 test (issue #34): the NIS sharpen path of kRcasShader on WARP (tests\sharpen_nis_test.cpp). No GPU needed.
rem Output: build\tests\sharpen_nis_test.exe   Run from the repo root: build\tests\sharpen_nis_test.exe (exit 0 = all passed)
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`call "%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0.."
if not exist build\tests mkdir build\tests
if not exist build\tests\obj_sharpennis mkdir build\tests\obj_sharpennis
cl /nologo /std:c++17 /EHsc /O2 /W4 /DNOMINMAX tests\sharpen_nis_test.cpp /Fe:build\tests\sharpen_nis_test.exe /Fo:build\tests\obj_sharpennis\ /link d3d11.lib d3dcompiler.lib > build\tests\build_sharpen_nis_test.txt 2>&1
echo exit %ERRORLEVEL% >> build\tests\build_sharpen_nis_test.txt
type build\tests\build_sharpen_nis_test.txt
