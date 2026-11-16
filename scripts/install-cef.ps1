# SPDX-License-Identifier: MPL-2.0
<#
.SYNOPSIS
    Downloads and extracts the CEF standard distribution for Windows x64 into D:\Tools\cef.

.DESCRIPTION
    Antikolt's engine host is built on the Chromium Embedded Framework (ADR-002). This script
    installs the binary distribution only; it is a *tool*, not project source, so it lives under
    D:\Tools (the machine's tool directory) and is excluded from version control.

    The newest build is chosen by *comparing version numbers*, not by trusting the order of CEF's
    public index — the index is not sorted the way you would hope (an earlier version of this script
    picked a 2016 Chromium because it took the last entry).

.EXAMPLE
    pwsh -File scripts/install-cef.ps1
#>
[CmdletBinding()]
param(
    [string]$Destination = 'D:\Tools\cef',
    [switch]$Force
)

$ErrorActionPreference = 'Stop'
$indexUrl = 'https://cef-builds.spotifycdn.com/index.json'
$buildsUrl = 'https://cef-builds.spotifycdn.com'

function Write-Step($message) { Write-Host "==> $message" -ForegroundColor Cyan }

Write-Step 'Fetching the CEF build index'
$indexPath = Join-Path $env:TEMP 'cef-index.json'
& curl.exe -sS --fail -m 300 -o $indexPath $indexUrl
if ($LASTEXITCODE -ne 0) { throw "could not download $indexUrl" }

$index = Get-Content -Raw -LiteralPath $indexPath | ConvertFrom-Json
$versions = $index.windows64.versions
if (-not $versions -or $versions.Count -eq 0) { throw 'the CEF index contains no windows64 builds' }

# Collect every entry that ships a standard archive, with a numeric key for sorting.
$candidates = foreach ($entry in $versions) {
    $standard = $entry.files | Where-Object { $_.type -eq 'standard' } | Select-Object -First 1
    if (-not $standard) { continue }
    $numbers = @(($entry.chromium_version -split '\.') | ForEach-Object {
            $digits = ($_ -replace '\D', '')
            if ($digits) { [int]$digits } else { 0 }
        })
    while ($numbers.Count -lt 4) { $numbers += 0 }
    [pscustomobject]@{
        CefVersion      = $entry.cef_version
        ChromiumVersion = $entry.chromium_version
        File            = $standard.name
        Major           = $numbers[0]
        Minor           = $numbers[1]
        Build           = $numbers[2]
        Patch           = $numbers[3]
    }
}

if (-not $candidates) { throw 'no standard CEF build found in the index' }
$chosen = $candidates | Sort-Object Major, Minor, Build, Patch | Select-Object -Last 1

Write-Step ("Selected CEF {0} (Chromium {1}) -> {2}" -f $chosen.CefVersion, $chosen.ChromiumVersion, $chosen.File)

$archive = Join-Path $env:TEMP $chosen.File
if ($Force -or -not (Test-Path $archive) -or (Get-Item $archive).Length -lt 10MB) {
    Write-Step 'Downloading the archive (this is the slow part)'
    & curl.exe -sS --fail -L -m 3600 -o $archive "$buildsUrl/$($chosen.File)"
    if ($LASTEXITCODE -ne 0) { throw "download failed: $buildsUrl/$($chosen.File)" }
} else {
    Write-Step "Reusing the already downloaded archive $archive"
}

New-Item -ItemType Directory -Force -Path $Destination | Out-Null

# Drop any earlier, mismatched extraction so two CEF trees never sit side by side.
$existing = Get-ChildItem $Destination -Directory -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -like 'cef_binary_*' }
foreach ($dir in $existing) {
    if ($dir.Name -notlike "*$($chosen.CefVersion.Replace('+', '*'))*") {
        Write-Step "Removing the older extraction $($dir.Name)"
        Remove-Item $dir.FullName -Recurse -Force
    }
}

Write-Step "Extracting into $Destination (bsdtar handles .tar.bz2)"
& tar.exe -xjf $archive -C $Destination
if ($LASTEXITCODE -ne 0) { throw 'extraction failed' }

$extracted = Get-ChildItem $Destination -Directory |
    Where-Object { $_.Name -like 'cef_binary_*' } | Sort-Object Name | Select-Object -Last 1
if (-not $extracted) { throw 'the archive did not produce a cef_binary_* directory' }

# Verify the pieces the build needs, so a half-extracted tree fails here and not inside CMake.
foreach ($relative in @('include\cef_app.h', 'cmake\cef_variables.cmake', 'CMakeLists.txt', 'libcef_dll\libcef_dll.cc')) {
    if (-not (Test-Path (Join-Path $extracted.FullName $relative))) {
        throw "the extracted CEF tree is missing $relative"
    }
}
$dll = Get-ChildItem $extracted.FullName -Recurse -Filter 'libcef.dll' -ErrorAction SilentlyContinue |
    Select-Object -First 1
if (-not $dll) { throw 'the extracted CEF tree contains no libcef.dll' }

$marker = Join-Path $Destination 'CEF-ROOT.txt'
@(
    "CEF_ROOT=$($extracted.FullName)"
    "CEF_VERSION=$($chosen.CefVersion)"
    "CHROMIUM_VERSION=$($chosen.ChromiumVersion)"
    "ARCHIVE=$archive"
    "LIBCEF_DLL=$($dll.FullName)"
    "LIBCEF_SIZE=$($dll.Length)"
    "INSTALLED_AT=$(Get-Date -Format o)"
) | Set-Content -LiteralPath $marker -Encoding utf8

Write-Step "CEF is ready. CEF_ROOT=$($extracted.FullName)"
Get-Content $marker
