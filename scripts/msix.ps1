# SPDX-License-Identifier: MPL-2.0
<#
.SYNOPSIS
    Packs the portable folder into an MSIX (the Store channel of ADR-040) and optionally signs it for local
    testing.

.DESCRIPTION
    The Store route exists because a Store MSIX needs no certificate of ours: Microsoft re-signs it after
    certification, and a Store install never shows a SmartScreen warning. This script produces that package
    from exactly what `scripts/dist.ps1` already builds, so there is one payload and two doors.

    Local installation for testing needs the package to be signed by a certificate the machine trusts. The
    Identity Publisher in packaging\msix\AppxManifest.xml must match the signing certificate's subject -
    `-SelfSigned` creates a certificate with that subject and signs the package, but installing it still
    requires importing the certificate as trusted on that machine.

.PARAMETER Source
    The portable folder to pack. Defaults to dist\Antikolt.

.PARAMETER Output
    Where the .msix goes. Defaults to dist\Antikolt.msix.

.PARAMETER Pfx
    Sign the package with this certificate (a Store submission does not need this; local tests do).

.PARAMETER SelfSigned
    Create (or reuse) the test certificate named in the manifest's Publisher and sign with it.
#>
[CmdletBinding()]
param(
    [string]$Source = (Join-Path $PSScriptRoot '..\dist\Antikolt'),
    [string]$Output = (Join-Path $PSScriptRoot '..\dist\Antikolt.msix'),
    [string]$Pfx,
    [string]$Password = $env:ANTIKOLT_PFX_PASSWORD,
    [switch]$SelfSigned
)

$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$Source = [System.IO.Path]::GetFullPath($Source)
$Output = [System.IO.Path]::GetFullPath($Output)
$manifestDir = Join-Path $repo 'packaging\msix'
$manifest = Join-Path $manifestDir 'AppxManifest.xml'
$staging = Join-Path $repo 'build\msix-staging'

$sdkRoot = 'C:\Program Files (x86)\Windows Kits\10\bin'
$makeappx = Get-ChildItem $sdkRoot -Recurse -Filter 'makeappx.exe' -ErrorAction SilentlyContinue |
    Where-Object { $_.FullName -match '\\x64\\' } |
    Sort-Object FullName -Descending | Select-Object -First 1 -ExpandProperty FullName
if (-not $makeappx) { throw "makeappx.exe not found under $sdkRoot (install the Windows SDK)" }
$signtool = Get-ChildItem $sdkRoot -Recurse -Filter 'signtool.exe' -ErrorAction SilentlyContinue |
    Where-Object { $_.FullName -match '\\x64\\' } |
    Sort-Object FullName -Descending | Select-Object -First 1 -ExpandProperty FullName

if (-not (Test-Path $Source)) { throw "nothing to pack: $Source does not exist (run scripts/dist.ps1 first)" }
if (-not (Test-Path $manifest)) { throw "no manifest at $manifest" }

Write-Host '=== Antikolt MSIX ===' -ForegroundColor Cyan
Write-Host "  source   : $Source"
Write-Host "  manifest : $manifest"
Write-Host "  output   : $Output"

# --- staging: the payload exactly as the portable folder has it, plus the manifest and the assets ---------
if (Test-Path $staging) { Remove-Item $staging -Recurse -Force }
New-Item -ItemType Directory -Path $staging | Out-Null
Copy-Item (Join-Path $Source '*') $staging -Recurse
Copy-Item $manifest $staging
Copy-Item (Join-Path $manifestDir 'assets') (Join-Path $staging 'assets') -Recurse

# `makeappx` refuses a directory with a stale package or with the .msix inside it.
Get-ChildItem $staging -Filter '*.msix' | Remove-Item -Force

if (Test-Path $Output) { Remove-Item $Output -Force }
& $makeappx pack /o /d $staging /p $Output
if ($LASTEXITCODE -ne 0) { throw "makeappx pack failed ($LASTEXITCODE)" }

$size = [math]::Round((Get-Item $Output).Length / 1MB, 1)
Write-Host "  packed   : $size MB" -ForegroundColor Green

# --- signing: optional for local tests, unnecessary for a Store submission --------------------------------
if ($Pfx -or $SelfSigned) {
    if (-not $signtool) { throw 'signtool.exe not found (install the Windows SDK)' }
    $signArgs = @('sign', '/fd', 'SHA256', '/debug', '/v')
    if ($Pfx) {
        $signArgs += @('/f', (Resolve-Path $Pfx).Path)
        if ($Password) { $signArgs += @('/p', $Password) }
    } else {
        # The subject must equal Identity/@Publisher, or Add-AppxPackage rejects the package.
        [xml]$doc = Get-Content $manifest
        $subject = $doc.Package.Identity.Publisher
        $cert = Get-ChildItem Cert:\CurrentUser\My | Where-Object { $_.Subject -eq $subject } | Select-Object -First 1
        if (-not $cert) {
            Write-Host "  creating a self-signed certificate for '$subject'" -ForegroundColor Yellow
            $cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject $subject `
                -KeyUsage DigitalSignature -KeySpec Signature -KeyExportPolicy Exportable `
                -NotAfter (Get-Date).AddYears(1)
        }
        $signArgs += @('/sha1', $cert.Thumbprint)
    }
    & $signtool @signArgs $Output
    if ($LASTEXITCODE -ne 0) { throw 'signing the package failed' }
}

# --- verification: prove the package is readable and holds the three processes ---------------------------
$unpacked = Join-Path $repo 'build\msix-unpacked'
if (Test-Path $unpacked) { Remove-Item $unpacked -Recurse -Force }
& $makeappx unpack /o /p $Output /d $unpacked | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'the packed package could not be unpacked again' }
$expected = @('antikolt-shell.exe', 'antikolt-engine-host.exe', 'antikolt-core-service.exe',
    'AppxManifest.xml', 'home\index.html', 'libcef.dll', 'icudtl.dat')
$missing = @()
foreach ($name in $expected) {
    if (-not (Test-Path (Join-Path $unpacked $name))) { $missing += $name }
}
Write-Host ("  payload  : {0}" -f ($(if ($missing.Count -eq 0) { 'complete' } else { 'MISSING ' + ($missing -join ', ') })))
if ($missing.Count -gt 0) { throw 'the package is incomplete' }
$size = [math]::Round((Get-Item $Output).Length / 1MB, 1)
Write-Host "msix=$Output size_mb=$size payload=ok"
Write-Host 'DONE — a Store submission uses this package as it is (Microsoft signs it); local installs need the certificate trusted.'
