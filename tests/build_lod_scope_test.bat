@echo off
rem v0.10.0 phase 11 unit test of the texture LOD bias scope (tests\lod_scope_test.cpp: inject.cpp WITHOUT WITH_DLAA in one TU,
rem WARP device, the real LodFrameUpdate / LodOnBind / hkOMSetBlendState / hkPSSetSamplers / hkDrawIndexed / hkDraw;
rem phase 12: + hkCreatePixelShader / hkPSSetShader / DxbcPsClass = the cut-out rule; "--scan <dir>" classifies *.dxbc).
rem Needs a configured build tree (build\_deps\minhook-src, build\Release\minhook_static.lib). Output: build\tests\lod_scope_test.exe
rem Run: build\tests\lod_scope_test.exe   (exit code 0 = all checks passed)
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0.."
if not exist build\tests\obj_lod mkdir build\tests\obj_lod
cl /nologo /std:c++17 /EHsc /O2 /MD /W3 /permissive- /Zc:__cplusplus /DNOMINMAX /DDLAA_INJECTOR_VERSION=\"0.10.0\" /I src /I build\_deps\minhook-src\include tests\lod_scope_test.cpp /Fe:build\tests\lod_scope_test.exe /Fo:build\tests\obj_lod\ /link build\Release\minhook_static.lib d3d11.lib dxgi.lib d3dcompiler.lib user32.lib version.lib > build\tests\build_lod_scope_test.txt 2>&1
echo exit %ERRORLEVEL% >> build\tests\build_lod_scope_test.txt
type build\tests\build_lod_scope_test.txt
