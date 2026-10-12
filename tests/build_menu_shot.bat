@echo off
rem phase 46: menu_shot -- offline renders of the REAL tuning menu (src\menu.cpp, linked as it is) and the fps box to PNG files for the
rem README (WARP D3D11, no game). Usage: build\tests\menu_shot.exe docs\images   (exit code 0 = every image written).
rem Output: build\tests\menu_shot.exe.
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`call "%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0.."
if not exist build\tests\obj_ms mkdir build\tests\obj_ms
cl /nologo /std:c++17 /EHsc /O2 /W3 /DWITH_DLAA=1 /DNOMINMAX /I src tests\menu_shot.cpp src\menu.cpp src\shader_cache.cpp /Fe:build\tests\menu_shot.exe /Fo:build\tests\obj_ms\ /link d3d11.lib d3dcompiler.lib dxgi.lib windowscodecs.lib ole32.lib gdi32.lib user32.lib uuid.lib > build\tests\build_menu_shot.txt 2>&1
echo exit %ERRORLEVEL% >> build\tests\build_menu_shot.txt
type build\tests\build_menu_shot.txt
