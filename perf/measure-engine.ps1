# SPDX-License-Identifier: MPL-2.0
<#
.SYNOPSIS
    Measures the private working set of the Antikolt engine process tree for one or more pages.

.DESCRIPTION
    This is the F0 measurement (docs/HU/03-MEMORIA.md §5). It answers one question with a number
    instead of an opinion: *what does one live page cost?*

    Notes on method, because the numbers are worthless without it:
      * it measures the **private working set** of every process in the tree (browser, GPU, network,
        renderer, utility). Summing RSS would double-count shared pages;
      * CEF launches its helper processes from the same executable, so the tree is "all processes
        named antikolt-engine-host";
      * it samples once per second after a settle period, and reports the **median** sample (plus the
        peak), because memory is noisy;
      * it always kills the tree it started, even on failure.

.PARAMETER Url
    One or more pages to measure, in order. Each is measured in a fresh process tree.

.PARAMETER SettleSeconds
    How long to wait after launch before sampling (the page must finish loading and settle).

.PARAMETER Samples
    How many one-second samples to take.

.EXAMPLE
    pwsh -File perf/measure-engine.ps1
    pwsh -File perf/measure-engine.ps1 -Url 'https://example.com' -SettleSeconds 15
#>
[CmdletBinding()]
param(
    [string[]]$Url = @('about:blank'),
    [string]$Exe,
    [int]$SettleSeconds = 10,
    [int]$Samples = 5,
    [switch]$Visible,
    [string]$OutputDir
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
if (-not $OutputDir) { $OutputDir = Join-Path $repoRoot 'perf/reports' }
New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null

if (-not $Exe) {
    $candidate = Get-ChildItem (Join-Path $repoRoot 'build') -Recurse -Filter 'antikolt-engine-host.exe' -ErrorAction SilentlyContinue |
        Select-Object -First 1
    if (-not $candidate) {
        throw 'antikolt-engine-host.exe not found. Build first: pwsh -File scripts/build.ps1'
    }
    $Exe = $candidate.FullName
}

$processName = [System.IO.Path]::GetFileNameWithoutExtension($Exe)
$counter = '\Process({0}*)\Working Set - Private' -f $processName

function Get-TreePrivateBytes {
    <# Total private working set of the engine tree, plus the process count. #>
    $samples = $null
    try {
        $samples = (Get-Counter -Counter $counter -ErrorAction Stop).CounterSamples |
            Where-Object { $_.Status -eq 0 }
    } catch {
        # Fall back to private bytes from the process objects (commit rather than working set).
        $processes = Get-Process -Name "$processName*" -ErrorAction SilentlyContinue
        if (-not $processes) { return $null }
        return [pscustomobject]@{
            Bytes    = ($processes | Measure-Object PrivateMemorySize64 -Sum).Sum
            Processes = @($processes).Count
            Source   = 'Process.PrivateMemorySize64'
        }
    }
    if (-not $samples) { return $null }
    return [pscustomobject]@{
        Bytes     = ($samples | Measure-Object CookedValue -Sum).Sum
        Processes = @($samples).Count
        Source    = 'Working Set - Private'
    }
}

function Measure-Page([string]$pageUrl) {
    $arguments = @('--url', $pageUrl, '--user-data-dir', (Join-Path $env:TEMP "antikolt-measure-$([guid]::NewGuid().ToString('N'))"))
    if (-not $Visible) { $arguments += '--hidden' }

    Write-Host ("==> {0}" -f $pageUrl) -ForegroundColor Cyan
    $process = Start-Process -FilePath $Exe -ArgumentList $arguments -PassThru
    try {
        Start-Sleep -Seconds $SettleSeconds
        $readings = @()
        for ($i = 0; $i -lt $Samples; $i++) {
            $reading = Get-TreePrivateBytes
            if ($reading) { $readings += $reading }
            Start-Sleep -Seconds 1
        }
        if ($readings.Count -eq 0) {
            throw "no measurement for $pageUrl (did the engine exit?)"
        }
        $sorted = $readings | Sort-Object Bytes
        $median = $sorted[[int][math]::Floor($sorted.Count / 2)]
        $peak = $readings | Sort-Object Bytes | Select-Object -Last 1
        return [pscustomobject]@{
            Url            = $pageUrl
            MedianMB       = [math]::Round($median.Bytes / 1MB, 1)
            PeakMB         = [math]::Round($peak.Bytes / 1MB, 1)
            Processes      = $median.Processes
            MaxProcesses   = ($readings | Measure-Object Processes -Maximum).Maximum
            Source         = $median.Source
            Samples        = $readings.Count
        }
    } finally {
        Get-Process -Name "$processName*" -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
        if ($process -and -not $process.HasExited) {
            $process.WaitForExit(5000) | Out-Null
        }
        Start-Sleep -Milliseconds 500
    }
}

$results = @()
foreach ($page in $Url) {
    $results += Measure-Page $page
}

# ---- report ------------------------------------------------------------------------------------
$stamp = Get-Date -Format 'yyyy-MM-dd-HHmm'
$machine = [pscustomobject]@{
    Host       = $env:COMPUTERNAME
    OS         = (Get-CimInstance Win32_OperatingSystem -ErrorAction SilentlyContinue).Caption
    Timestamp  = (Get-Date -Format o)
    EngineExe  = $Exe
    SettleSecs = $SettleSeconds
    Samples    = $Samples
    Visible    = [bool]$Visible
}

$jsonPath = Join-Path $OutputDir "engine-$stamp.json"
[pscustomobject]@{ Machine = $machine; Results = $results } |
    ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $jsonPath -Encoding utf8

Write-Host ''
$results | Format-Table Url, MedianMB, PeakMB, Processes, MaxProcesses, Source -AutoSize

$reportPath = Join-Path $OutputDir "engine-$stamp.md"
$lines = @(
    "# Engine memory measurement — $stamp"
    ''
    "- Host: $($env:COMPUTERNAME)"
    "- Engine: ``$Exe``"
    "- Method: private working set of the process tree, median of $Samples one-second samples after $SettleSeconds s settle"
    "- Window visible during measurement: $([bool]$Visible)"
    ''
    '| Page | Median MB | Peak MB | Processes | Max processes |'
    '|---|---|---|---|---|'
)
foreach ($r in $results) {
    $lines += ("| {0} | {1} | {2} | {3} | {4} |" -f $r.Url, $r.MedianMB, $r.PeakMB, $r.Processes, $r.MaxProcesses)
}
$lines += ''
$lines += 'Compare with the F0 gate in `docs/HU/03-MEMORIA.md` section 5.4: a static article must stay'
$lines += 'under 620 MB, and one heavy page under 1000 MB, for the project to keep its promise.'
$lines | Set-Content -LiteralPath $reportPath -Encoding utf8

Write-Host ("JSON  : {0}" -f $jsonPath)
Write-Host ("Report: {0}" -f $reportPath)
