# SPDX-License-Identifier: MPL-2.0
<#
.SYNOPSIS
    Builds Antikolt on Windows (MSVC + Qt 6 + optional CEF).

.DESCRIPTION
    This script exists because the toolchain is *not* discoverable by hand on a plain
    "VS Build Tools" machine:

      * `vcvars64.bat` is a forwarding stub and the classic `call vcvars64.bat` does not always
        produce a working compiler environment here, so the script sets PATH/INCLUDE/LIB explicitly
        from the MSVC and Windows SDK directories it finds on disk;
      * the CMake "Visual Studio 17 2022" generator needs a registered VS *instance*, which a
        Build Tools-only install may not provide, so the script uses Ninja (which ships with the
        Build Tools) and passes absolute paths for cmake and ninja;
      * Windows paths with backslashes break CMake cache values, so every path handed to CMake uses
        forward slashes.

    Everything is discovered (vswhere, disk scan for the newest MSVC and SDK), nothing is hardcoded
    to one machine's version numbers.

.PARAMETER CheckOnly
    Verify the toolchain and print the plan, but do not configure or build.

.PARAMETER Clean
    Delete the build directory first.

.EXAMPLE
    pwsh -File scripts/build.ps1
    pwsh -File scripts/build.ps1 -Clean -Config Debug
    pwsh -File scripts/build.ps1 -CheckOnly
#>
[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release', 'RelWithDebInfo', 'MinSizeRel')]
    [string]$Config = 'RelWithDebInfo',
    [string]$QtRoot = 'D:\Tools\Qt',
    [string]$CefSearchRoot = 'D:\Tools\cef',
    [string]$CefMarker = 'D:\Tools\cef\CEF-ROOT.txt',
    [string]$BuildDir,
    [switch]$Clean,
    [switch]$CheckOnly
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
if (-not $BuildDir) { $BuildDir = Join-Path $repoRoot 'build' }

function Fail($message) {
    Write-Host "ERROR: $message" -ForegroundColor Red
    exit 1
}

function Find-VisualStudio {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path $vswhere) {
        $path = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2>$null
        if ($path) { return $path.Trim() }
        $path = & $vswhere -latest -products * -property installationPath 2>$null
        if ($path) { return $path.Trim() }
    }
    foreach ($candidate in @(
            "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\BuildTools",
            "${env:ProgramFiles}\Microsoft Visual Studio\2022\BuildTools",
            "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\Community")) {
        if (Test-Path $candidate) { return $candidate }
    }
    return $null
}

function Find-NewestDirectory([string]$path, [string]$filter = '*') {
    if (-not (Test-Path $path)) { return $null }
    Get-ChildItem $path -Directory -Filter $filter -ErrorAction SilentlyContinue |
        Sort-Object { try { [version]($_.Name -replace '[^0-9.]', '').Trim('.') } catch { [version]'0.0' } } |
        Select-Object -Last 1
}

# ---- MSVC ---------------------------------------------------------------------------------
$vsPath = Find-VisualStudio
if (-not $vsPath) { Fail 'no Visual Studio / Build Tools installation found (install "Desktop development with C++")' }

$msvc = Find-NewestDirectory (Join-Path $vsPath 'VC\Tools\MSVC')
if (-not $msvc) { Fail "no MSVC toolset under $vsPath\VC\Tools\MSVC" }
$cl = Join-Path $msvc.FullName 'bin\Hostx64\x64\cl.exe'
if (-not (Test-Path $cl)) { Fail "cl.exe not found at $cl" }

$cmake = Join-Path $vsPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
$ninja = Join-Path $vsPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe'
if (-not (Test-Path $cmake)) { Fail "cmake.exe not found at $cmake (is the CMake component installed?)" }
if (-not (Test-Path $ninja)) { Fail "ninja.exe not found at $ninja" }

# ---- Windows SDK --------------------------------------------------------------------------
$sdkRoot = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10'
if (-not (Test-Path $sdkRoot)) { Fail "Windows SDK not found at $sdkRoot" }
$sdk = Find-NewestDirectory (Join-Path $sdkRoot 'Lib')
if (-not $sdk) { Fail "no Windows SDK version under $sdkRoot\Lib" }
$sdkVersion = $sdk.Name
$rc = Join-Path $sdkRoot "bin\$sdkVersion\x64\rc.exe"
if (-not (Test-Path $rc)) { Fail "rc.exe not found at $rc" }

# ---- Qt 6 ---------------------------------------------------------------------------------
$qtDirs = Get-ChildItem $QtRoot -Directory -ErrorAction SilentlyContinue |
    Sort-Object { try { [version]($_.Name -split '-')[0] } catch { [version]'0.0' } }
$qtKit = $null
foreach ($dir in $qtDirs) {
    $kit = Join-Path $dir.FullName 'msvc2022_64'
    if (Test-Path (Join-Path $kit 'lib\cmake\Qt6\Qt6Config.cmake')) { $qtKit = $kit }
}
if (-not $qtKit) { Fail "no Qt 6 msvc2022_64 kit under $QtRoot (run: python -m aqt install-qt windows desktop 6.8.3 win64_msvc2022_64 -O $QtRoot --archives qtbase qtdeclarative qtsvg)" }

# ---- CEF (optional) -----------------------------------------------------------------------
# Discovered by scanning, not by a marker file: an extracted CEF tree is self-describing (it has
# cmake/cef_variables.cmake), and requiring a marker would mean writing outside the project.
function Find-CefRoot([string]$searchRoot) {
    if (-not (Test-Path $searchRoot)) { return $null }
    Get-ChildItem $searchRoot -Directory -Filter 'cef_binary_*' -ErrorAction SilentlyContinue |
        Where-Object { Test-Path (Join-Path $_.FullName 'cmake\cef_variables.cmake') } |
        Sort-Object Name | Select-Object -Last 1
}

$cefRoot = ''
if (Test-Path $CefMarker) {
    foreach ($line in Get-Content $CefMarker) {
        if ($line -like 'CEF_ROOT=*') { $cefRoot = $line.Substring('CEF_ROOT='.Length) }
    }
}
if (-not ($cefRoot -and (Test-Path (Join-Path $cefRoot 'cmake\cef_variables.cmake')))) {
    $found = Find-CefRoot $CefSearchRoot
    $cefRoot = if ($found) { $found.FullName } else { '' }
}
$cefReady = [bool]$cefRoot

# ---- Environment --------------------------------------------------------------------------
$env:PATH = "$(Join-Path $msvc.FullName 'bin\Hostx64\x64');$qtKit\bin;$sdkRoot\bin\$sdkVersion\x64;$env:PATH"
$env:INCLUDE = @(
    (Join-Path $msvc.FullName 'include'),
    (Join-Path $sdkRoot "Include\$sdkVersion\ucrt"),
    (Join-Path $sdkRoot "Include\$sdkVersion\um"),
    (Join-Path $sdkRoot "Include\$sdkVersion\shared"),
    (Join-Path $sdkRoot "Include\$sdkVersion\winrt"),
    (Join-Path $sdkRoot "Include\$sdkVersion\cppwinrt")
) -join ';'
$env:LIB = @(
    (Join-Path $msvc.FullName 'lib\x64'),
    (Join-Path $sdkRoot "Lib\$sdkVersion\ucrt\x64"),
    (Join-Path $sdkRoot "Lib\$sdkVersion\um\x64")
) -join ';'
if ($cefReady) { $env:CEF_ROOT = $cefRoot }

Write-Host '=== Antikolt toolchain ===' -ForegroundColor Cyan
Write-Host ("  Visual Studio : {0}" -f $vsPath)
Write-Host ("  MSVC          : {0}" -f $msvc.Name)
Write-Host ("  Windows SDK   : {0}" -f $sdkVersion)
Write-Host ("  CMake         : {0}" -f (& $cmake --version | Select-Object -First 1))
Write-Host ("  Ninja         : {0}" -f $ninja)
Write-Host ("  Qt            : {0}" -f $qtKit)
Write-Host ("  CEF           : {0}" -f ($(if ($cefReady) { $cefRoot } else { 'not installed (shell only)' })))
Write-Host ("  Build dir     : {0}" -f $BuildDir)

if ($CheckOnly) { exit 0 }

# ---- Configure and build ------------------------------------------------------------------
if ($Clean -and (Test-Path $BuildDir)) {
    Write-Host '=== Cleaning build directory ===' -ForegroundColor Cyan
    Remove-Item $BuildDir -Recurse -Force
}

$qtKitFwd = $qtKit.Replace('\', '/')
$clFwd = $cl.Replace('\', '/')
$ninjaFwd = $ninja.Replace('\', '/')

$configureArgs = @(
    '-S', $repoRoot,
    '-B', $BuildDir,
    '-G', 'Ninja',
    "-DCMAKE_BUILD_TYPE=$Config",
    "-DCMAKE_PREFIX_PATH=$qtKitFwd",
    "-DCMAKE_MAKE_PROGRAM=$ninjaFwd",
    "-DCMAKE_CXX_COMPILER=$clFwd"
)
if ($cefReady) { $configureArgs += "-DANTIKOLT_CEF_ROOT=$($cefRoot.Replace('\','/'))" }

Write-Host '=== Configure ===' -ForegroundColor Cyan
& $cmake @configureArgs
if ($LASTEXITCODE -ne 0) { Fail "configure failed ($LASTEXITCODE)" }

Write-Host '=== Build ===' -ForegroundColor Cyan
& $cmake --build $BuildDir
if ($LASTEXITCODE -ne 0) { Fail "build failed ($LASTEXITCODE)" }

$exe = Get-ChildItem $BuildDir -Recurse -Filter 'antikolt-shell.exe' -ErrorAction SilentlyContinue |
    Select-Object -First 1
if ($exe) {
    Write-Host '=== Self test (shell) ===' -ForegroundColor Cyan
    & $exe.FullName --selftest -platform offscreen
    if ($LASTEXITCODE -ne 0) { Fail "shell self test failed ($LASTEXITCODE)" }
    Write-Host ("Built: {0}" -f $exe.FullName) -ForegroundColor Green
} else {
    Fail 'the build produced no antikolt-shell.exe'
}

$engine = Get-ChildItem $BuildDir -Recurse -Filter 'antikolt-engine-host.exe' -ErrorAction SilentlyContinue |
    Select-Object -First 1
if ($engine) {
    Write-Host '=== Self test (engine host / CEF) ===' -ForegroundColor Cyan
    & $engine.FullName --selftest
    if ($LASTEXITCODE -ne 0) { Fail "engine host self test failed ($LASTEXITCODE)" }
    Write-Host ("Built: {0}" -f $engine.FullName) -ForegroundColor Green
} else {
    Write-Host 'The engine host was not built (no CEF installed).' -ForegroundColor Yellow
}
