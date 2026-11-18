# SPDX-License-Identifier: MPL-2.0
<#
.SYNOPSIS
    Runs the same repository checks the CI runs, locally.

.DESCRIPTION
    Written after a real miss: the first commit shipped four files without the SPDX header and the
    gate noticed only in CI. A local check that differs from CI is not a check, so this script walks
    the same files with the same rules.

    Checks:
      * SPDX licence header on every source file (Rust, C++, JS, CSS, HTML, SQL, PowerShell, CMake, YAML);
      * every relative markdown link resolves to a real file;
      * no build output or secret-looking file is tracked;
      * Rust: format, clippy (warnings are errors), tests — skipped when cargo is unavailable;
      * the home page script parses.

.PARAMETER SkipRust
    Skip the Rust checks (useful on a machine without a toolchain).

.EXAMPLE
    pwsh -File scripts/check-repo.ps1
#>
[CmdletBinding()]
param([switch]$SkipRust)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
Push-Location $repoRoot
$failures = 0

function Report([string]$name, [bool]$ok, [string]$detail = '') {
    if ($ok) {
        Write-Host ("  ok    {0}" -f $name) -ForegroundColor Green
    } else {
        Write-Host ("  FAIL  {0} {1}" -f $name, $detail) -ForegroundColor Red
        $script:failures++
    }
}

Write-Host '=== SPDX headers ===' -ForegroundColor Cyan
$patterns = @('*.rs', '*.cpp', '*.h', '*.hpp', '*.js', '*.css', '*.html', '*.sql', '*.ps1', '*.cmake', '*.yml', '*.yaml')
$missing = @()
$checked = 0
foreach ($pattern in $patterns) {
    Get-ChildItem -Recurse -File -Filter $pattern |
        Where-Object { $_.FullName -notmatch '\\(target|build|node_modules)\\' } |
        ForEach-Object {
            $checked++
            $head = (Get-Content -TotalCount 3 $_.FullName) -join "`n"
            if ($head -notmatch 'SPDX-License-Identifier: MPL-2.0') {
                $missing += $_.FullName.Replace($repoRoot, '')
            }
        }
}
Get-ChildItem -Recurse -File -Filter 'CMakeLists.txt' |
    Where-Object { $_.FullName -notmatch '\\(target|build|node_modules)\\' } |
    ForEach-Object {
        $checked++
        $head = (Get-Content -TotalCount 3 $_.FullName) -join "`n"
        if ($head -notmatch 'SPDX-License-Identifier: MPL-2.0') {
            $missing += $_.FullName.Replace($repoRoot, '')
        }
    }
Report ("{0} source files carry the SPDX header" -f $checked) ($missing.Count -eq 0) ($missing -join ', ')

Write-Host '=== Markdown links ===' -ForegroundColor Cyan
$broken = @()
foreach ($file in Get-ChildItem -Recurse -File -Filter '*.md' |
        Where-Object { $_.FullName -notmatch '[\\/](dist|build|target|node_modules)[\\/]' }) {
    $text = Get-Content -Raw -Encoding utf8 $file.FullName
    foreach ($match in [regex]::Matches($text, '\]\(([^)]+)\)')) {
        $target = $match.Groups[1].Value
        if ($target -match '^(https?:|mailto:|#)') { continue }
        $clean = ($target -split '#')[0]
        if ([string]::IsNullOrWhiteSpace($clean)) { continue }
        $resolved = [System.IO.Path]::GetFullPath((Join-Path $file.DirectoryName $clean))
        if (-not (Test-Path $resolved)) { $broken += ("{0} -> {1}" -f $file.Name, $target) }
    }
}
Report 'every relative link resolves' ($broken.Count -eq 0) ($broken -join '; ')

Write-Host '=== Repository hygiene ===' -ForegroundColor Cyan
$trackedBad = @()
if (Test-Path (Join-Path $repoRoot '.git')) {
    $tracked = & git -C $repoRoot ls-files
    $trackedBad = @($tracked | Where-Object { $_ -match '(^|/)(target|build|node_modules)/' -or $_ -match '(^|/)\.env$|\.pfx$|\.pem$|\.key$' })
}
Report 'no build output or secrets tracked' ($trackedBad.Count -eq 0) ($trackedBad -join ', ')

Write-Host '=== Home page ===' -ForegroundColor Cyan
$node = Get-Command node -ErrorAction SilentlyContinue
if ($node) {
    & node --check (Join-Path $repoRoot 'home/app.js')
    Report 'home/app.js parses' ($LASTEXITCODE -eq 0)
} else {
    Write-Host '  skip  node is not installed' -ForegroundColor Yellow
}

if (-not $SkipRust) {
    Write-Host '=== Rust ===' -ForegroundColor Cyan
    $cargo = Get-Command cargo -ErrorAction SilentlyContinue
    if ($cargo) {
        & cargo fmt --all -- --check
        Report 'cargo fmt' ($LASTEXITCODE -eq 0)
        & cargo clippy --all-targets --offline -- -D warnings
        Report 'cargo clippy -D warnings' ($LASTEXITCODE -eq 0)
        & cargo test --offline -q
        Report 'cargo test' ($LASTEXITCODE -eq 0)
    } else {
        Write-Host '  skip  cargo is not installed' -ForegroundColor Yellow
    }
}

Pop-Location
Write-Host ''
if ($failures -eq 0) {
    Write-Host 'ALL CHECKS PASSED' -ForegroundColor Green
    exit 0
}
Write-Host ("{0} CHECK(S) FAILED" -f $failures) -ForegroundColor Red
exit 1
