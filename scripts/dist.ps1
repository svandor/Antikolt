# SPDX-License-Identifier: MPL-2.0
<#
.SYNOPSIS
    Assembles a self-contained, double-clickable Antikolt folder and puts a shortcut on the desktop.

.DESCRIPTION
    The browser is three processes (shell, engine host, core service) plus a page and two runtimes (Qt and CEF).
    A build directory is fine for a developer and useless for a shortcut, so this script makes one directory
    that holds everything the shell looks for next to its own executable:

        antikolt-shell.exe            the window
        antikolt-engine-host.exe      the CEF host (found by the shell next to itself)
        antikolt-core-service.exe     the Rust core (also found next to the shell)
        home\                         the grid page (appDir/home/index.html)
        Qt DLLs + platforms\ ...      deployed by windeployqt
        libcef.dll, *.pak, ...        copied from the CEF Release directory

    The shortcut is created by the same script, because "final version, startable from the desktop" is one
    action: assemble, then point the desktop at it. Nothing is installed and nothing is registered — deleting
    the folder and the shortcut removes it completely.

.PARAMETER Destination
    Where the portable folder goes. Defaults to dist\Antikolt.

.PARAMETER Shortcut
    Create (or replace) the desktop shortcut. Defaults to on.

.PARAMETER NoBuild
    Assemble from the current build\ output instead of building first.
#>
[CmdletBinding()]
param(
    [string]$Destination = (Join-Path $PSScriptRoot '..\dist\Antikolt'),
    [switch]$Shortcut = $true,
    [switch]$NoBuild,
    [switch]$Release
)

$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$Destination = [System.IO.Path]::GetFullPath($Destination)
if ($Release) {
    # A Release configuration: no debug info, optimised C++.  It is a separate build directory so the
    # developer's incremental Debug build (and every self test) keeps working untouched.
    $build = Join-Path $repo 'build-release'
    $coreExeExpected = 'release'
} else {
    $build = Join-Path $repo 'build'
    $coreExeExpected = $null
}
$cefRoot = 'D:\Tools\cef\cef_binary_158.0.3+g8c29be2+chromium-158.0.8127.18_windows64_beta'
if ($env:CEF_ROOT -and (Test-Path $env:CEF_ROOT)) { $cefRoot = $env:CEF_ROOT }

Write-Host '=== Antikolt portable build ===' -ForegroundColor Cyan
Write-Host "  from : $build"
Write-Host "  to   : $Destination"
Write-Host "  cef  : $cefRoot"

if (-not $NoBuild) {
    if ($Release) {
        # build.ps1 owns the toolchain (vcvars, cmake, ninja, Qt, CEF discovery) — duplicating it here is how
        # the first attempt failed with "cmake is not recognized": this shell has no MSVC environment.
        & (Join-Path $PSScriptRoot 'build.ps1') -Config 'Release' -BuildDir $build
        if ($LASTEXITCODE -ne 0) { throw 'the Release build failed' }
        & cargo build --release -p antikolt-core-service
        if ($LASTEXITCODE -ne 0) { throw 'the Release core build failed' }
    } else {
        & (Join-Path $PSScriptRoot 'build.ps1')
        if ($LASTEXITCODE -ne 0) { throw 'the build failed; nothing was assembled' }
    }
}

# --- the three processes --------------------------------------------------------------------------------
$shellExe = Join-Path $build 'apps\desktop-shell\antikolt-shell.exe'
$engineExe = Join-Path $build 'apps\engine-host\antikolt-engine-host.exe'
$coreExe = Join-Path $repo 'target\release\antikolt-core-service.exe'
if (-not (Test-Path $coreExe)) { $coreExe = Join-Path $repo 'target\debug\antikolt-core-service.exe' }
foreach ($required in @($shellExe, $engineExe, $coreExe)) {
    if (-not (Test-Path $required)) { throw "missing $required — run this without -NoBuild" }
}

if (Test-Path $Destination) { Remove-Item $Destination -Recurse -Force }
New-Item -ItemType Directory -Path $Destination | Out-Null
Copy-Item $shellExe $Destination
Copy-Item $engineExe $Destination
Copy-Item $coreExe $Destination

# --- the page -------------------------------------------------------------------------------------------
Copy-Item (Join-Path $repo 'home') (Join-Path $Destination 'home') -Recurse
Copy-Item (Join-Path $repo 'assets\antikolt.ico') $Destination

# --- CEF: the host needs its own runtime beside it ------------------------------------------------------
# The CEF distribution keeps the *binaries* in Release\ and the *resources* (icudtl.dat, the .pak files and
# locales\) in Resources\ — copying only the first half produced "Invalid file descriptor to ICU data
# received" in the first packaged build.
$cefRelease = Join-Path $cefRoot 'Release'
$cefResources = Join-Path $cefRoot 'Resources'
if (-not (Test-Path $cefRelease)) { throw "no CEF Release directory at $cefRelease" }
if (-not (Test-Path $cefResources)) { throw "no CEF Resources directory at $cefResources" }
Get-ChildItem $cefRelease -File | Where-Object { $_.Extension -in @('.dll', '.bin', '.json') } |
    ForEach-Object { Copy-Item $_.FullName $Destination }
Get-ChildItem $cefResources -File | ForEach-Object { Copy-Item $_.FullName $Destination }
foreach ($directory in @('locales')) {
    $from = Join-Path $cefResources $directory
    if (Test-Path $from) { Copy-Item $from (Join-Path $Destination $directory) -Recurse }
}
foreach ($directory in @('swiftshader')) {
    $from = Join-Path $cefRelease $directory
    if (Test-Path $from) { Copy-Item $from (Join-Path $Destination $directory) -Recurse }
}

# --- Qt: windeployqt puts the DLLs and plugins where the loader looks for them ---------------------------
$windeploy = 'D:\Tools\Qt\6.8.3\msvc2022_64\bin\windeployqt.exe'
if (-not (Test-Path $windeploy)) { throw "windeployqt not found at $windeploy" }
& $windeploy --release --no-translations --no-system-d3d-compiler --no-opengl-sw `
    (Join-Path $Destination 'antikolt-shell.exe') | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'windeployqt failed' }

# --- a note for whoever opens the folder ----------------------------------------------------------------
@'
Antikolt — hordozható build
===========================

Indítás: antikolt-shell.exe (vagy a desktop parancsikon).
Az ablakban nyomd meg a „Motor indítása" gombot; utána megjelenik a kártyarács.

A mappa mindent tartalmaz, amire futáshoz szükség van (Qt, CEF, Rust mag, a lap).
Semmit nem telepít és nem ír a rendszerleíró adatbázisba: a mappa és a parancsikon
törlésével nyom nélkül eltávolítható.

A profil (kártyák, munkamenet-napló, előnézetek) a felhasználói AppData alatt van:
  %LOCALAPPDATA%\Antikolt\Antikolt\
'@ | Set-Content -Path (Join-Path $Destination 'README.txt') -Encoding UTF8

# --- the desktop shortcut -------------------------------------------------------------------------------
$shellPath = Join-Path $Destination 'antikolt-shell.exe'
if ($Shortcut) {
    $desktop = [Environment]::GetFolderPath('Desktop')
    $linkPath = Join-Path $desktop 'Antikolt.lnk'
    $wsh = New-Object -ComObject WScript.Shell
    $link = $wsh.CreateShortcut($linkPath)
    $link.TargetPath = $shellPath
    $link.WorkingDirectory = $Destination
    $link.IconLocation = (Join-Path $Destination 'antikolt.ico')
    $link.Description = 'Antikolt — memóriatakarékos kártyás böngésző'
    $link.Save()
    Write-Host "  shortcut: $linkPath"

    # Start menu too: a portable build still deserves a place where people look for programs. It is the same
    # .lnk, so deleting the folder and both links is still the whole uninstall.
    $programs = [Environment]::GetFolderPath('Programs')
    $startLink = Join-Path $programs 'Antikolt.lnk'
    $start = $wsh.CreateShortcut($startLink)
    $start.TargetPath = $shellPath
    $start.WorkingDirectory = $Destination
    $start.IconLocation = (Join-Path $Destination 'antikolt.ico')
    $start.Description = 'Antikolt — memóriatakarékos kártyás böngésző'
    $start.Save()
    Write-Host "  start menu: $startLink"
}

$size = [math]::Round((Get-ChildItem $Destination -Recurse -File | Measure-Object Length -Sum).Sum / 1MB, 1)
Write-Host "  size    : $size MB" -ForegroundColor Green
Write-Host "  exe     : $shellPath" -ForegroundColor Green
Write-Host 'DONE (no installer, no registry: delete the folder and the shortcut to remove it)'
