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
    # The engine's own container isolation (ADR-071): run against the engine binary, not the shell, because the claim
    # is about request contexts, the proxy preference and the permission policy — no window is needed to prove it.
    @{ Name = 'partitiontest'; Args = @('--partitiontest'); Direct = $true },
    # F0 (ADR-027/023): the WebAuthn spike — can the engine create a CDP virtual authenticator?
    @{ Name = 'webauthntest'; Args = @('--webauthntest'); Direct = $true },
    # ADR-053: the isolated-world spike — can the engine run JS invisibly to the page's main world?
    @{ Name = 'isolatedworldtest'; Args = @('--isolatedworldtest'); Direct = $true },
    # ADR-053: the autofill spike — fill a login form in the isolated world without leaking the secret.
    @{ Name = 'autofilltest'; Args = @('--autofilltest'); Direct = $true },
    # ADR-023: the passkey flow — `navigator.credentials.create` against the CDP virtual authenticator,
    # served from a 127.0.0.1 page (the WebAuthn "localhost" exception; a custom scheme is rejected).
    @{ Name = 'passkeyflowtest'; Args = @('--passkeyflowtest') },
    # ADR-093: the passkey export side — the shell reads the virtual authenticator's credentials.
    @{ Name = 'passkeyexporttest'; Args = @('--passkeyexporttest') },
    # ADR-095: the Document-PiP probe — `documentPictureInPicture` is exposed on the secure `antikolt://`
    # scheme (CEF 158 is Chrome-runtime-only; no views switch is needed).
    @{ Name = 'pipchecktest'; Args = @('--pipchecktest'); Direct = $true },
    # ADR-097: the views switch de-risking — the views framework loads a page (CHROME style), and rejects
    # a custom cache path (the partition mechanism).
    @{ Name = 'viewstest'; Args = @('--viewstest'); Direct = $true },
    @{ Name = 'gridtest'; Args = @('--gridtest') },
    @{ Name = 'gridlangtest'; Args = @('--gridlangtest') },
    @{ Name = 'autoprofiltest'; Args = @('--autoprofiltest') },
    @{ Name = 'bookmarkimporttest'; Args = @('--bookmarkimporttest') },
    @{ Name = 'lifecycletest'; Args = @('--lifecycletest') },
    @{ Name = 'opentest'; Args = @('--opentest') },
    @{ Name = 'embedtest'; Args = @('--embedtest') },
    @{ Name = 'fillcredentialtest'; Args = @('--fillcredentialtest') },
    @{ Name = 'capturetest'; Args = @('--capturetest') },
    @{ Name = 'wipetest'; Args = @('--wipetest') },
    @{ Name = 'cleanuptest'; Args = @('--cleanuptest') },
    @{ Name = 'bridgetest'; Args = @('--bridgetest') },
    @{ Name = 'pintest'; Args = @('--pintest') },
    @{ Name = 'notetest'; Args = @('--notetest') },
    @{ Name = 'timeouttest'; Args = @('--timeouttest') },
    @{ Name = 'previewtest'; Args = @('--previewtest') },
    @{ Name = 'previewgentest'; Args = @('--previewgentest') },
    @{ Name = 'previewworkertest'; Args = @('--previewworkertest') },
    @{ Name = 'metricstest'; Args = @('--metricstest') },
    @{ Name = 'prefstest'; Args = @('--prefstest') },
    @{ Name = 'memorytest'; Args = @('--memorytest') },
    @{ Name = 'customhometest'; Args = @('--customhometest') },
    @{ Name = 'resethometest'; Args = @('--resethometest') },
    @{ Name = 'homeedittest'; Args = @('--homeedittest') },
    @{ Name = 'sandboxtest'; Args = @('--sandboxtest') },
    @{ Name = 'containerpolicytest'; Args = @('--containerpolicytest') },
    @{ Name = 'memorypressuretest'; Args = @('--memorypressuretest') },
    @{ Name = 'memorypressuree2etest'; Args = @('--memorypressuree2etest') },
    @{ Name = 'memoryhibernatetest'; Args = @('--memoryhibernatetest') },
    @{ Name = 'memorywarnpausetest'; Args = @('--memorywarnpausetest') },
    @{ Name = 'videocaptest'; Args = @('--videocaptest') },
    @{ Name = 'printtest'; Args = @('--printtest') },
    @{ Name = 'findtest'; Args = @('--findtest') },
    @{ Name = 'downloadtest'; Args = @('--downloadtest') },
    @{ Name = 'crashtest'; Args = @('--crashtest') },
    @{ Name = 'pdftest'; Args = @('--pdftest') },
    @{ Name = 'accessibilitytest'; Args = @('--accessibilitytest') },
    @{ Name = 'enginerestarttest'; Args = @('--enginerestarttest') },
    @{ Name = 'rastertest'; Args = @('--rastertest') },
    @{ Name = 'rasterhometest'; Args = @('--rasterhometest') },
    @{ Name = 'rasterautotest'; Args = @('--rasterautotest') },
    @{ Name = 'rasterclicktest'; Args = @('--rasterclicktest') },
    @{ Name = 'memoryreport'; Args = @('--memoryreport') },
    @{ Name = 'mediatest'; Args = @('--mediatest') },
    @{ Name = 'themetest'; Args = @('--themetest') },
    @{ Name = 'searchtest'; Args = @('--searchtest') },
    @{ Name = 'mediacleantest'; Args = @('--mediacleantest') },
    @{ Name = 'gridscaletest'; Args = @('--gridscaletest') },
    @{ Name = 'containertest'; Args = @('--containertest') },
    @{ Name = 'workspacetest'; Args = @('--workspacetest') },
    @{ Name = 'workspacedragtest'; Args = @('--workspacedragtest') },
    @{ Name = 'profiletest'; Args = @('--profiletest') },
    @{ Name = 'addcardtest'; Args = @('--addcardtest') },
    @{ Name = 'tagtest'; Args = @('--tagtest') },
    @{ Name = 'scrollzoomtest'; Args = @('--scrollzoomtest') },
    @{ Name = 'scrollrestore'; Args = @('--scrollrestore') }
)

foreach ($check in $engineChecks) {
    Write-Host ("=== {0} ===" -f $check.Name) -ForegroundColor Cyan
    if (-not $engine) {
        Add-Result $check.Name $false 'missing antikolt-engine-host.exe'
        continue
    }
    if ($check.Direct) {
        # Run by the engine itself: the claim is CEF's request-context behaviour, and a shell in the way would only
        # add a window and a pipe to the thing being measured.
        & $engine @($check.Args)
        Add-Result $check.Name ($LASTEXITCODE -eq 0)
        continue
    }
    if (-not $shell) {
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

Write-Host '=== vaulttest ===' -ForegroundColor Cyan
if (-not $shell -or -not $core) {
    Add-Result 'vaulttest' $false 'missing antikolt-shell.exe or antikolt-core-service.exe'
} else {
    & $shell --vaulttest $core
    Add-Result 'vaulttest' ($LASTEXITCODE -eq 0)
}

Write-Host '=== vaultpaneltest ===' -ForegroundColor Cyan
if (-not $shell -or -not $core) {
    Add-Result 'vaultpaneltest' $false 'missing antikolt-shell.exe or antikolt-core-service.exe'
} else {
    & $shell --vaultpaneltest $core
    Add-Result 'vaultpaneltest' ($LASTEXITCODE -eq 0)
}

Write-Host '=== passkeystoretest ===' -ForegroundColor Cyan
if (-not $shell -or -not $core) {
    Add-Result 'passkeystoretest' $false 'missing antikolt-shell.exe or antikolt-core-service.exe'
} else {
    & $shell --passkeystoretest $core
    Add-Result 'passkeystoretest' ($LASTEXITCODE -eq 0)
}

Write-Host '=== sessionpanel ===' -ForegroundColor Cyan
if (-not $shell -or -not $engine -or -not $core) {
    Add-Result 'sessionpanel' $false 'missing shell, engine or core service'
} else {
    & $shell --sessionpanel $engine
    Add-Result 'sessionpanel' ($LASTEXITCODE -eq 0)
}

Write-Host '=== scrollbook ===' -ForegroundColor Cyan
if (-not $shell) {
    Add-Result 'scrollbook' $false 'missing antikolt-shell.exe'
} else {
    & $shell --scrollbook
    Add-Result 'scrollbook' ($LASTEXITCODE -eq 0)
}
Write-Host '=== traytest ===' -ForegroundColor Cyan
if (-not $shell) {
    Add-Result 'traytest' $false 'missing antikolt-shell.exe'
} else {
    & $shell --traytest
    Add-Result 'traytest' ($LASTEXITCODE -eq 0)
}
Write-Host '=== logtest ===' -ForegroundColor Cyan
if (-not $shell) {
    Add-Result 'logtest' $false 'missing antikolt-shell.exe'
} else {
    & $shell --logtest
    Add-Result 'logtest' ($LASTEXITCODE -eq 0)
}
Write-Host '=== previewbudget ===' -ForegroundColor Cyan
if (-not $shell) {
    Add-Result 'previewbudget' $false 'missing antikolt-shell.exe'
} else {
    & $shell --previewbudget
    Add-Result 'previewbudget' ($LASTEXITCODE -eq 0)

Write-Host '=== previewqueue ===' -ForegroundColor Cyan
if (-not (Test-Path $shell)) {
    Add-Result 'previewqueue' $false 'missing antikolt-shell.exe'
} else {
    & $shell --previewqueue
    Add-Result 'previewqueue' ($LASTEXITCODE -eq 0)
}
}
Write-Host '=== previewrefresh ===' -ForegroundColor Cyan
if (-not $shell) {
    Add-Result 'previewrefresh' $false 'missing antikolt-shell.exe'
} else {
    & $shell --previewrefresh
    Add-Result 'previewrefresh' ($LASTEXITCODE -eq 0)
}
Write-Host '=== previewclean ===' -ForegroundColor Cyan
if (-not $shell) {
    Add-Result 'previewclean' $false 'missing antikolt-shell.exe'
} else {
    & $shell --previewclean
    Add-Result 'previewclean' ($LASTEXITCODE -eq 0)
}
Write-Host '=== previewhashtest ===' -ForegroundColor Cyan
if (-not $shell) {
    Add-Result 'previewhashtest' $false 'missing antikolt-shell.exe'
} else {
    & $shell --previewhashtest
    Add-Result 'previewhashtest' ($LASTEXITCODE -eq 0)
}
Write-Host '=== languagetest ===' -ForegroundColor Cyan
if (-not $shell) {
    Add-Result 'languagetest' $false 'missing antikolt-shell.exe'
} else {
    & $shell --languagetest
    Add-Result 'languagetest' ($LASTEXITCODE -eq 0)
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
