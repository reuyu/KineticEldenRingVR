<#
.SYNOPSIS
  Configure + build ERVR (Release, x64) into .\build\dist.
.EXAMPLE
  .\build.ps1
  .\build.ps1 -Config Debug
#>
param([string]$Config = "Release")

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path

# Locate cmake: PATH first, then the one bundled with Visual Studio.
$cmake = (Get-Command cmake -ErrorAction SilentlyContinue).Source
if (-not $cmake) {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        $vs = & $vswhere -latest -products * -property installationPath
        $candidate = Join-Path $vs "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
        if (Test-Path $candidate) { $cmake = $candidate }
    }
}
if (-not $cmake) { throw "cmake not found. Install CMake or Visual Studio 2022+ with the C++ Desktop workload." }

Write-Host "Using $cmake"

# Kinetic: PhysX (the B&S-style sword) comes from vcpkg. Use VCPKG_ROOT, else the vcpkg that ships with
# Visual Studio 2022 (17.6+, "vcpkg package manager" component). The first build compiles PhysX (10-20 min).
if (-not $env:VCPKG_ROOT) {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        $vs = & $vswhere -latest -products * -property installationPath
        $bundled = Join-Path $vs "VC\vcpkg"
        if (Test-Path (Join-Path $bundled "scripts\buildsystems\vcpkg.cmake")) { $env:VCPKG_ROOT = $bundled }
    }
}
if ($env:VCPKG_ROOT) { Write-Host "vcpkg: $env:VCPKG_ROOT" }
else { Write-Warning "vcpkg not found: building without PhysX (no B&S-style sword). Install the 'vcpkg package manager' component of Visual Studio, or set VCPKG_ROOT." }
& $cmake -S $root -B "$root\build" -A x64
if ($LASTEXITCODE -ne 0) { throw "configure failed" }
& $cmake --build "$root\build" --config $Config
if ($LASTEXITCODE -ne 0) { throw "build failed" }
Write-Host "`nOutput: $root\build\dist" -ForegroundColor Green
Get-ChildItem -Recurse "$root\build\dist" | Select-Object FullName, Length
