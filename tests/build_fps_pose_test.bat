@echo off
setlocal
rem Phase 44 unit test of src\fps_pose.h (fps box colours, VR turn quaternion, drawn-in turned corners, distance shift).
rem No D3D, no game, no build tree needed. Output: build\tests\fps_pose_test.exe
rem Run: build\tests\fps_pose_test.exe   (exit code 0 = all checks passed)
rem vswhere finds any Visual Studio edition (Community here, Enterprise on the GitHub runner)
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`call "%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0.."
if not exist build\tests\obj_fpspose mkdir build\tests\obj_fpspose
cl /nologo /std:c++17 /EHsc /O2 /W4 /I src tests\fps_pose_test.cpp /Fe:build\tests\fps_pose_test.exe /Fo:build\tests\obj_fpspose\ > build\tests\build_fps_pose_test.txt 2>&1
echo exit %ERRORLEVEL% >> build\tests\build_fps_pose_test.txt
type build\tests\build_fps_pose_test.txt
