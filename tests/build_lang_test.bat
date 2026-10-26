@echo off
setlocal
rem phase 74 unit test: src\lang.cpp (the menu translations: .po parsing, the placeholder guard, the font pick).
rem No D3D, no game, no build tree needed. Output: build\tests\lang_test.exe
rem Run: build\tests\lang_test.exe   (exit code 0 = all checks passed);  build\tests\lang_test.exe fit lang  (width report)
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`call "%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0.."
if not exist build\tests\obj_lang mkdir build\tests\obj_lang
cl /nologo /std:c++17 /EHsc /O2 /W3 /DNOMINMAX /utf-8 /I src tests\lang_test.cpp src\lang.cpp /Fe:build\tests\lang_test.exe /Fo:build\tests\obj_lang\ /link gdi32.lib user32.lib shell32.lib ole32.lib advapi32.lib > build\tests\build_lang_test.txt 2>&1
echo exit %ERRORLEVEL% >> build\tests\build_lang_test.txt
type build\tests\build_lang_test.txt
