# SPDX-License-Identifier: MPL-2.0
<#
.SYNOPSIS
    Measures what a freeze actually returns: memory with a live page, then with the card on disk.

.DESCRIPTION
    This is the measurement that decides whether Antikolt's central promise is real. It runs the
    probe in freeze mode, which loads a page, captures a snapshot, destroys the surface and pauses
    between phases. While it runs, this script samples the engine process tree's private memory and
    attributes every sample to a phase using the `phase=` markers the probe prints.

    The number that matters is the difference between `loaded` and `frozen`: that is the RAM the
    browser gives back when a card melts back into the home page.

.PARAMETER Url
    Page to load.

.EXAMPLE
    pwsh -File perf/measure-freeze.ps1
    pwsh -File perf/measure-freeze.ps1 -Url https://en.wikipedia.org/wiki/Web_browser
#>
[CmdletBinding()]
param(
    [string]$Url = 'https://example.com',
    [string]$Probe,
    [string]$EngineExe,
    [int]$SettleSeconds = 3,
    [string]$OutputDir
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
if (-not $OutputDir) { $OutputDir = Join-Path $repoRoot 'perf/reports' }
New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null

function Find-Artifact([string]$name) {
    $found = Get-ChildItem (Join-Path $repoRoot 'build') -Recurse -Filter $name -ErrorAction SilentlyContinue |
        Select-Object -First 1
    if ($found) { return $found.FullName }
    return $null
}

if (-not $EngineExe) { $EngineExe = Find-Artifact 'antikolt-engine-host.exe' }
if (-not $Probe) {
    $candidate = Get-ChildItem (Join-Path $repoRoot 'target') -Recurse -Filter 'antikolt-probe.exe' -ErrorAction SilentlyContinue |
        Select-Object -First 1
    if ($candidate) { $Probe = $candidate.FullName }
}
if (-not $EngineExe) { throw 'antikolt-engine-host.exe not found. Build first: pwsh -File scripts/build.ps1' }
if (-not $Probe) { throw 'antikolt-probe.exe not found. Build first: cargo build -p antikolt-probe' }

# The GPU process aborts on some machines (virtualised GPUs); software compositing keeps the
# measurement deterministic. Recorded in the report so nobody mistakes it for a normal run.
$env:ANTIKOLT_ENGINE_DISABLE_GPU = '1'
$processName = [System.IO.Path]::GetFileNameWithoutExtension($EngineExe)
$counter = '\Process({0}*)\Working Set - Private' -f $processName

$stdout = Join-Path $env:TEMP ('freeze-probe-{0}.txt' -f ([guid]::NewGuid().ToString('N')))
$stderr = Join-Path $env:TEMP ('freeze-probe-{0}.err' -f ([guid]::NewGuid().ToString('N')))

Write-Host ("==> {0} (freeze)" -f $Url) -ForegroundColor Cyan
$process = Start-Process -FilePath $Probe -ArgumentList @($EngineExe, $Url, '--freeze') -PassThru `
    -RedirectStandardOutput $stdout -RedirectStandardError $stderr

$samples = @()
while (-not $process.HasExited) {
    $bytes = $null
    try {
        $counterSamples = (Get-Counter -Counter $counter -ErrorAction Stop).CounterSamples |
            Where-Object { $_.Status -eq 0 }
        $bytes = ($counterSamples | Measure-Object CookedValue -Sum).Sum
    } catch {
        $tree = Get-Process -Name "$processName*" -ErrorAction SilentlyContinue
        if ($tree) { $bytes = ($tree | Measure-Object PrivateMemorySize64 -Sum).Sum }
    }
    if ($bytes) {
        $samples += [pscustomobject]@{ Ts = [DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds(); Bytes = [double]$bytes }
    }
    Start-Sleep -Milliseconds 400
}
$process.WaitForExit()
Start-Sleep -Milliseconds 500

# ---- attribute samples to phases ---------------------------------------------------------------
$lines = @()
if (Test-Path $stdout) { $lines = Get-Content $stdout }
$phases = @()
foreach ($line in $lines) {
    if ($line -match '^phase=(?<name>\w+) ts=(?<ts>\d+)(?<rest>.*)$') {
        $phases += [pscustomobject]@{
            Name = $Matches.name
            Ts   = [double]$Matches.ts
            Rest = $Matches.rest
        }
    }
}

$results = @()
for ($i = 0; $i -lt $phases.Count - 1; $i++) {
    $start = $phases[$i].Ts
    $end = $phases[$i + 1].Ts
    $window = @($samples | Where-Object { $_.Ts -ge $start -and $_.Ts -lt $end })
    if ($window.Count -eq 0) { continue }
    $sorted = $window | Sort-Object Bytes
    $median = $sorted[[int][math]::Floor($sorted.Count / 2)]
    $results += [pscustomobject]@{
        Phase   = $phases[$i].Name
        MedianMB = [math]::Round($median.Bytes / 1MB, 1)
        PeakMB   = [math]::Round(($sorted | Select-Object -Last 1).Bytes / 1MB, 1)
        Samples  = $window.Count
    }
}

$loaded = $results | Where-Object Phase -eq 'loaded'
$frozen = $results | Where-Object Phase -eq 'frozen'
$snapshotLine = $lines | Where-Object { $_ -like 'snapshot *' } | Select-Object -First 1
$profileLine = $lines | Where-Object { $_ -like 'profile=*' } | Select-Object -First 1

$stamp = Get-Date -Format 'yyyy-MM-dd-HHmm'
$reportPath = Join-Path $OutputDir "freeze-$stamp.md"
$linesOut = @(
    '# Freeze measurement — ' + $stamp
    ''
    "- Page: $Url"
    "- Method: private working set of the engine process tree, 400 ms sampling, median per phase"
    "- GPU: disabled for this run (`ANTIKOLT_ENGINE_DISABLE_GPU=1`)"
    ''
    '| Phase | Median MB | Peak MB | Samples |'
    '|---|---|---|---|'
)
foreach ($r in $results) {
    $linesOut += ("| {0} | {1} | {2} | {3} |" -f $r.Phase, $r.MedianMB, $r.PeakMB, $r.Samples)
}
if ($loaded -and $frozen) {
    $linesOut += ''
    $linesOut += ('**RAM returned by the freeze: {0} MB** ({1} MB loaded → {2} MB frozen)' -f
        [math]::Round($loaded.MedianMB - $frozen.MedianMB, 1), $loaded.MedianMB, $frozen.MedianMB)
}
if ($snapshotLine) { $linesOut += ''; $linesOut += 'Snapshot: `' + $snapshotLine + '`' }
if ($profileLine) { $linesOut += ''; $linesOut += 'Profile (kept for inspection): `' + $profileLine + '`' }
$linesOut | Set-Content -LiteralPath $reportPath -Encoding utf8

$results | Format-Table Phase, MedianMB, PeakMB, Samples -AutoSize
if ($loaded -and $frozen) {
    Write-Host ("RAM returned by the freeze: {0} MB" -f ([math]::Round($loaded.MedianMB - $frozen.MedianMB, 1))) -ForegroundColor Green
}
if ($snapshotLine) { Write-Host $snapshotLine }
Write-Host ("Report: {0}" -f $reportPath)

Remove-Item Env:\ANTIKOLT_ENGINE_DISABLE_GPU -ErrorAction SilentlyContinue
foreach ($name in "$processName*", 'antikolt-probe*') {
    Get-Process -Name $name -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
}
