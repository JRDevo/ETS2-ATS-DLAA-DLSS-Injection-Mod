@echo off
setlocal
rem Phase 81 unit test of src\outside_aa.h + its HLSL on WARP (no GPU needed): feather interior / bands, Catmull-Rom weights,
rem and the unjitter of frames rasterised with the mod's jittered viewport (in place CS + upscale PS, sign mutations).
rem Output: build\tests\outside_aa_test.exe   Run: build\tests\outside_aa_test.exe   (exit code 0 = all checks passed)
rem vswhere finds any Visual Studio edition (Community here, Enterprise on the GitHub runner)
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`call "%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0.."
if not exist build\tests\obj_outsideaa mkdir build\tests\obj_outsideaa
cl /nologo /std:c++17 /EHsc /O2 /W4 /DNOMINMAX /I src tests\outside_aa_test.cpp /Fe:build\tests\outside_aa_test.exe /Fo:build\tests\obj_outsideaa\ /link d3d11.lib d3dcompiler.lib > build\tests\build_outside_aa_test.txt 2>&1
echo exit %ERRORLEVEL% >> build\tests\build_outside_aa_test.txt
type build\tests\build_outside_aa_test.txt
