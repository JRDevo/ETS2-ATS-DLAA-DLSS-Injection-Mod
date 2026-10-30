@echo off
setlocal
rem Phase 78 unit test of src\light_fs.h + the lighting pass's full-screen draw with the jitter shift (WARP, no GPU needed):
rem every pixel written once, its own G-buffer texel read, uv matches the shifted texel; edge re-draw for negative shifts.
rem Output: build\tests\light_fs_test.exe   Run: build\tests\light_fs_test.exe   (exit code 0 = all checks passed)
rem vswhere finds any Visual Studio edition (Community here, Enterprise on the GitHub runner)
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`call "%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0.."
if not exist build\tests\obj_lightfs mkdir build\tests\obj_lightfs
cl /nologo /std:c++17 /EHsc /O2 /W4 /DNOMINMAX /I src tests\light_fs_test.cpp /Fe:build\tests\light_fs_test.exe /Fo:build\tests\obj_lightfs\ /link d3d11.lib d3dcompiler.lib > build\tests\build_light_fs_test.txt 2>&1
echo exit %ERRORLEVEL% >> build\tests\build_light_fs_test.txt
type build\tests\build_light_fs_test.txt
