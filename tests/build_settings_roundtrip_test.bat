@echo off
setlocal
rem Issue #26 test: every tuning menu row survives a game restart; Reset resets (tests\settings_roundtrip_test.cpp: inject.cpp
rem WITH_DLAA in one TU, the fake NGX of tests\fake_dlaa.cpp, no GPU needed). Needs a configured build tree
rem (build\_deps\minhook-src, build\Release\minhook_static.lib) and external\DLSS\include.
rem Output: build\tests\settings_rt\settings_roundtrip_test.exe (in its own folder: the test writes dlaa.ini next to it)
rem Run: build\tests\settings_rt\settings_roundtrip_test.exe   (exit code 0 = all checks passed)
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`call "%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0.."
if not exist build\tests\settings_rt mkdir build\tests\settings_rt
if not exist build\tests\obj_srt mkdir build\tests\obj_srt
cl /nologo /std:c++17 /EHsc /O2 /MT /W3 /utf-8 /permissive- /Zc:__cplusplus /DWITH_DLAA=1 /DNOMINMAX /DDLAA_INJECTOR_VERSION=\"0.0.0-test\" /I src /I build\_deps\minhook-src\include /I external\DLSS\include tests\settings_roundtrip_test.cpp src\ofxr.cpp src\dinput_wrap.cpp src\xr_hook.cpp src\vrs.cpp src\frame_pace.cpp src\motion_vectors.cpp src\scene_dlaa.cpp src\preview_blit.cpp src\shader_cache.cpp src\draw_ids.cpp src\gpu_perf.cpp src\menu.cpp src\hidden_mask.cpp src\fps_cab.cpp src\wire_widen.cpp src\lang.cpp tests\fake_dlaa.cpp /Fe:build\tests\settings_rt\settings_roundtrip_test.exe /Fo:build\tests\obj_srt\ /link build\Release\minhook_static.lib d3d11.lib dxgi.lib d3dcompiler.lib user32.lib version.lib dxguid.lib windowscodecs.lib ole32.lib uuid.lib gdi32.lib shell32.lib advapi32.lib winmm.lib >build\tests\build_settings_roundtrip_test.txt 2>&1
echo exit %ERRORLEVEL% >> build\tests\build_settings_roundtrip_test.txt
type build\tests\build_settings_roundtrip_test.txt
