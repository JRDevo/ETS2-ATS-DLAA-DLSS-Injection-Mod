@echo off
rem v0.10.0 WARP harness for the per-draw motion vectors (tests\drawid_harness.cpp). Output: build\tests\drawid_harness.exe
rem Run: build\tests\drawid_harness.exe [scene 1..9, -1 = all] [snap px] [debug | nocons | nofwd | noattach | notwin |
rem noinherit | noparent | parentconj | nodepth]   (exit code 0 = all checks passed; S5 = the v0.10.0 phase 2 mirror view +
rem main view; S6 = phase 3 consensus camera R + forward-pass ids; S7 = phase 4 plates take their body's motion (attach) +
rem phase 5 matrix twins; S8 = phase 6 rigid parents; nocons / nofwd = S6 mutations, noattach / notwin / noinherit = S7
rem mutations, noparent / parentconj / nodepth = S8 mutations: they must FAIL; S8 also covers phase 6b: an instanced plate on
rem the trailer takes its motion, an instanced roadside grass clump stays static; phase 6c: scene 8 also runs a second time
rem with a 6 px march reach, "S8 short reach"; S9 = phase 7 pre / post-tonemap stage: DlaaStage hysteresis + the real
rem SceneDlaa with two colour sets, NGX replaced by tests\fake_dlaa.cpp)
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0.."
if not exist build\tests\obj mkdir build\tests\obj
cl /nologo /std:c++17 /EHsc /O2 /W3 /DWITH_DLAA=1 /DNOMINMAX /I src tests\drawid_harness.cpp src\motion_vectors.cpp src\draw_ids.cpp src\shader_cache.cpp src\scene_dlaa.cpp src\gpu_perf.cpp tests\fake_dlaa.cpp /Fe:build\tests\drawid_harness.exe /Fo:build\tests\obj\ /link d3d11.lib d3dcompiler.lib dxgi.lib > build\tests\build_drawid_harness.txt 2>&1
echo exit %ERRORLEVEL% >> build\tests\build_drawid_harness.txt
type build\tests\build_drawid_harness.txt
