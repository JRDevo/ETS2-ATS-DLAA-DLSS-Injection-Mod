<#
    package.ps1 -- build Release and make the distributable release zip.

    Produces dist\ETS2-ATS-DLAA-Injector-v<version>.zip containing exactly:
        dinput8.dll, dlaa.ini (clean defaults), KEYS.md, README.md, LICENSE, THIRD_PARTY.md

    The version comes from project(... VERSION x.y.z) in CMakeLists.txt (single source).
    The script FAILS if any nvngx*.dll or sl.*.dll would end up in the zip.

    Windows PowerShell 5.1 compatible (no '&&', no ternary, no null-coalescing).
    Run from anywhere:  powershell -ExecutionPolicy Bypass -File scripts\package.ps1
#>

$ErrorActionPreference = 'Stop'

function Fail($msg) {
    Write-Host "ERROR: $msg" -ForegroundColor Red
    exit 1
}

# --- paths -------------------------------------------------------------------
$root = Split-Path -Parent $PSScriptRoot
$buildDir = Join-Path $root 'build'
$distDir  = Join-Path $root 'dist'

# --- version from CMakeLists.txt ---------------------------------------------
$cmlPath = Join-Path $root 'CMakeLists.txt'
if (-not (Test-Path $cmlPath)) { Fail "CMakeLists.txt not found at $cmlPath" }
$cml = Get-Content -Raw $cmlPath
$verMatch = [regex]::Match($cml, 'project\s*\([^)]*VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)')
if (-not $verMatch.Success) { Fail "could not read project(... VERSION x.y.z) from CMakeLists.txt" }
$version = $verMatch.Groups[1].Value
Write-Host "Packaging version v$version" -ForegroundColor Cyan

# --- locate cmake ------------------------------------------------------------
$cmake = $null
$onPath = Get-Command cmake -ErrorAction SilentlyContinue
if ($onPath) {
    $cmake = $onPath.Source
} else {
    $editions = @('Community', 'Professional', 'Enterprise')
    $bases = @(
        "$env:ProgramFiles\Microsoft Visual Studio\2022",
        "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022"
    )
    foreach ($base in $bases) {
        foreach ($ed in $editions) {
            $cand = Join-Path $base "$ed\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
            if (Test-Path $cand) { $cmake = $cand; break }
        }
        if ($cmake) { break }
    }
}
if (-not $cmake) { Fail "cmake not found on PATH or under Visual Studio 2022 (Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe)" }
Write-Host "Using cmake: $cmake"

# --- configure + build Release ----------------------------------------------
# Re-configure so a version bump in CMakeLists.txt is picked up.
& $cmake -B $buildDir -S $root -G "Visual Studio 17 2022" -A x64
if ($LASTEXITCODE -ne 0) { Fail "cmake configure failed (exit $LASTEXITCODE)" }
& $cmake --build $buildDir --config Release
if ($LASTEXITCODE -ne 0) { Fail "cmake build failed (exit $LASTEXITCODE)" }

$dll = Join-Path $buildDir 'Release\dinput8.dll'
if (-not (Test-Path $dll)) { Fail "build produced no dinput8.dll at $dll" }

# --- stage exactly the release files -----------------------------------------
$stageName = "ETS2-ATS-DLAA-Injector-v$version"
$stage = Join-Path $distDir $stageName
$zip   = Join-Path $distDir "$stageName.zip"

if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
if (Test-Path $zip)   { Remove-Item -Force $zip }
New-Item -ItemType Directory -Force -Path $stage | Out-Null

# source -> name-in-zip
$files = @(
    @{ src = $dll;                                    name = 'dinput8.dll' },
    @{ src = (Join-Path $root 'docs\dlaa.ini.sample'); name = 'dlaa.ini' },
    @{ src = (Join-Path $root 'docs\KEYS.md');         name = 'KEYS.md' },
    @{ src = (Join-Path $root 'README.md');            name = 'README.md' },
    @{ src = (Join-Path $root 'LICENSE');              name = 'LICENSE' },
    @{ src = (Join-Path $root 'THIRD_PARTY.md');       name = 'THIRD_PARTY.md' }
)
foreach ($f in $files) {
    if (-not (Test-Path $f.src)) { Fail "missing source file: $($f.src)" }
    Copy-Item -LiteralPath $f.src -Destination (Join-Path $stage $f.name) -Force
}

# --- safety net: no NVIDIA redistributables in the zip -----------------------
$forbidden = Get-ChildItem -Path $stage -Recurse -File | Where-Object {
    $_.Name -like 'nvngx*.dll' -or $_.Name -like 'sl.*.dll'
}
if ($forbidden) {
    $names = ($forbidden | ForEach-Object { $_.Name }) -join ', '
    Fail "refusing to package NVIDIA redistributable(s): $names (the user must supply nvngx_dlss.dll themselves)"
}

# --- zip ---------------------------------------------------------------------
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip -Force
Write-Host ""
Write-Host "Created $zip" -ForegroundColor Green
Write-Host "Contents:"
Add-Type -AssemblyName System.IO.Compression.FileSystem
$archive = [System.IO.Compression.ZipFile]::OpenRead($zip)
try {
    foreach ($entry in $archive.Entries) {
        "  {0,10}  {1}" -f $entry.Length, $entry.FullName
    }
} finally {
    $archive.Dispose()
}
