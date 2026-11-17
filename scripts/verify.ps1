# SPDX-License-Identifier: MPL-2.0
<#
.SYNOPSIS
    Builds Antikolt and runs every self test in one pass, with one summary.

.DESCRIPTION
    This is the command a working session runs before it claims anything works. It exists because the
    checks were six separately-remembered invocations, and a session that forgets one of them reports
    a green state it did not measure:

      * scripts/build.ps1        configure + build + the two --selftest paths
      * --gridtest               the grid shows the core's cards, and a click opens one
      * --lifecycletest          an idle card hibernates itself and the grid comes back
      * --opentest               the bridge opens a card as a new surface
      * --embedtest              the engine drew into the Qt card, with focus both ways
      * --capturetest            the engine can capture its own pixels through DevTools
      * --coretest               the shell talks to the Rust core service
      * scripts/check-repo.ps1   SPDX, links, hygiene, node --check, cargo fmt/clippy/test

    The engine-backed checks need the Qt DLLs on PATH and three executables on disk; a missing one is
    reported as SKIP-less FAIL, never silently ignored.

.PARAMETER SkipBuild
    Run the checks against the binaries that are already built.

.PARAMETER SkipRepo
    Skip scripts/check-repo.ps1 (useful while iterating on the C++ side only).

.EXAMPLE
    pwsh -File scripts/verify.ps1
    pwsh -File scripts/verify.ps1 -SkipBuild
#>
[CmdletBinding()]
param(
    [switch]$SkipBuild,
    [switch]$SkipRepo,
    [string]$QtRoot = 'D:\Tools\Qt'
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot

function Find-FirstFile([string[]]$candidates) {
    foreach ($candidate in $candidates) {
        if ($candidate -and (Test-Path $candidate)) { return (Get-Item $candidate).FullName }
    }
    return $null
}

$shell = Find-FirstFile @(Get-ChildItem (Join-Path $repoRoot 'build') -Recurse -Filter 'antikolt-shell.exe' -ErrorAction SilentlyContinue | ForEach-Object { $_.FullName })
$engine = Find-FirstFile @(Get-ChildItem (Join-Path $repoRoot 'build') -Recurse -Filter 'antikolt-engine-host.exe' -ErrorAction SilentlyContinue | ForEach-Object { $_.FullName })
$core = Find-FirstFile @(
    (Join-Path $repoRoot 'target\debug\antikolt-core-service.exe'),
    (Join-Path $repoRoot 'target\release\antikolt-core-service.exe')
)

# The shell links Qt dynamically; the engine brings its own CEF runtime next to the exe.
$qtKit = Get-ChildItem $QtRoot -Directory -ErrorAction SilentlyContinue |
    Sort-Object { try { [version]($_.Name -split '-')[0] } catch { [version]'0.0' } } |
    Select-Object -Last 1
if ($qtKit) {
    $qtBin = Join-Path $qtKit.FullName 'msvc2022_64\bin'
    if (Test-Path $qtBin) { $env:PATH = "$qtBin;$env:PATH" }
}

$results = New-Object System.Collections.Generic.List[object]

function Add-Result([string]$name, [bool]$ok, [string]$detail = '') {
    $results.Add([pscustomobject]@{ Check = $name; Result = $(if ($ok) { 'ok' } else { 'FAILED' }); Detail = $detail })
}

if (-not $SkipBuild) {
    Write-Host '=== Build ===' -ForegroundColor Cyan
    & (Join-Path $PSScriptRoot 'build.ps1')
    Add-Result 'build + selftest' ($LASTEXITCODE -eq 0)
    # Re-discover: a clean build may have produced them just now.
    $shell = Find-FirstFile @(Get-ChildItem (Join-Path $repoRoot 'build') -Recurse -Filter 'antikolt-shell.exe' | ForEach-Object { $_.FullName })
    $engine = Find-FirstFile @(Get-ChildItem (Join-Path $repoRoot 'build') -Recurse -Filter 'antikolt-engine-host.exe' | ForEach-Object { $_.FullName })
}

# The core service is Cargo's, not CMake's: without this step the shell would talk to whatever binary was
# last built by hand, and a field the shell writes (but the stale core does not know) disappears silently.
Write-Host '=== Core service ===' -ForegroundColor Cyan
& cargo build -p antikolt-core-service --manifest-path (Join-Path $repoRoot 'Cargo.toml')
Add-Result 'core service' ($LASTEXITCODE -eq 0)
$core = Find-FirstFile @(
    (Join-Path $repoRoot 'target\debug\antikolt-core-service.exe'),
    (Join-Path $repoRoot 'target\release\antikolt-core-service.exe')
)

$engineChecks = @(
    @{ Name = 'gridtest'; Args = @('--gridtest') },
    @{ Name = 'lifecycletest'; Args = @('--lifecycletest') },
    @{ Name = 'opentest'; Args = @('--opentest') },
    @{ Name = 'embedtest'; Args = @('--embedtest') },
    @{ Name = 'capturetest'; Args = @('--capturetest') },
    @{ Name = 'wipetest'; Args = @('--wipetest') },
    @{ Name = 'cleanuptest'; Args = @('--cleanuptest') },
    @{ Name = 'bridgetest'; Args = @('--bridgetest') },
    @{ Name = 'pintest'; Args = @('--pintest') },
    @{ Name = 'notetest'; Args = @('--notetest') },
    @{ Name = 'timeouttest'; Args = @('--timeouttest') },
    @{ Name = 'previewtest'; Args = @('--previewtest') },
    @{ Name = 'previewgentest'; Args = @('--previewgentest') },
    @{ Name = 'metricstest'; Args = @('--metricstest') },
    @{ Name = 'mediatest'; Args = @('--mediatest') },
    @{ Name = 'scrollzoomtest'; Args = @('--scrollzoomtest') }
)

foreach ($check in $engineChecks) {
    Write-Host ("=== {0} ===" -f $check.Name) -ForegroundColor Cyan
    if (-not $shell -or -not $engine) {
        Add-Result $check.Name $false 'missing antikolt-shell.exe or antikolt-engine-host.exe'
        continue
    }
    & $shell @($check.Args) $engine
    Add-Result $check.Name ($LASTEXITCODE -eq 0)
}

Write-Host '=== coretest ===' -ForegroundColor Cyan
if (-not $shell -or -not $core) {
    Add-Result 'coretest' $false 'missing antikolt-shell.exe or antikolt-core-service.exe'
} else {
    & $shell --coretest $core
    Add-Result 'coretest' ($LASTEXITCODE -eq 0)
}

Write-Host '=== sessionpanel ===' -ForegroundColor Cyan
if (-not $shell -or -not $engine -or -not $core) {
    Add-Result 'sessionpanel' $false 'missing shell, engine or core service'
} else {
    & $shell --sessionpanel $engine
    Add-Result 'sessionpanel' ($LASTEXITCODE -eq 0)
}

Write-Host '=== previewclean ===' -ForegroundColor Cyan
if (-not $shell) {
    Add-Result 'previewclean' $false 'missing antikolt-shell.exe'
} else {
    & $shell --previewclean
    Add-Result 'previewclean' ($LASTEXITCODE -eq 0)
}
Write-Host '=== rulestest ===' -ForegroundColor Cyan
if (-not $shell -or -not $core) {
    Add-Result 'rulestest' $false 'missing antikolt-shell.exe or antikolt-core-service.exe'
} else {
    & $shell --rulestest $core
    Add-Result 'rulestest' ($LASTEXITCODE -eq 0)
}

if (-not $SkipRepo) {
    Write-Host '=== Repository checks ===' -ForegroundColor Cyan
    & (Join-Path $PSScriptRoot 'check-repo.ps1')
    Add-Result 'check-repo' ($LASTEXITCODE -eq 0)
}

Write-Host ''
$results | Format-Table -AutoSize | Out-String | Write-Host
$failed = @($results | Where-Object { $_.Result -ne 'ok' })
if ($failed.Count -gt 0) {
    Write-Host ("{0} CHECK(S) FAILED" -f $failed.Count) -ForegroundColor Red
    exit 1
}
Write-Host 'ALL SELF TESTS PASSED' -ForegroundColor Green
exit 0
