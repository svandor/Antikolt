# SPDX-License-Identifier: MPL-2.0
<#
.SYNOPSIS
    Signs the packaged Antikolt binaries and verifies the signature.

.DESCRIPTION
    There are only three honest ways to get a signature on this exe, and this script supports all of them:

      1. SignPath Foundation (free for open-source projects, key on their HSM, certificate issued to
         SignPath) — the signing happens in their pipeline, not here. See docs/HU/20-ALASAS.md.
      2. A commercial certificate (Certum, SSL.com, Sectigo...) on a hardware token or in the cloud.
         Export it to a PFX, or point this script at the certificate already installed in the store.
      3. A self-signed certificate — free, and it only helps *your own* machines: SmartScreen still
         warns everyone else. `-SelfSigned` exists so the signing and verification flow can be exercised
         offline, never so it can pretend to be trusted.

    Nothing is faked: without a certificate this script stops and says so.

.PARAMETER Pfx
    Path to a PFX with the code-signing certificate.

.PARAMETER Password
    Password for the PFX. Prefer the ANTIkOLT_PFX_PASSWORD environment variable over the command line.

.PARAMETER Thumbprint
    Use a certificate already installed in the current user's store (for example from a hardware token).

.PARAMETER SelfSigned
    Create (or reuse) a self-signed test certificate in the current user's store and sign with it. The
    resulting signature verifies on this machine only; it does not remove SmartScreen warnings anywhere else.

.PARAMETER Files
    What to sign. Defaults to the executables in dist\Antikolt.
#>
[CmdletBinding(DefaultParameterSetName = 'Pfx')]
param(
    [Parameter(ParameterSetName = 'Pfx')][string]$Pfx,
    [Parameter(ParameterSetName = 'Pfx')][string]$Password = $env:ANTIKOLT_PFX_PASSWORD,
    [Parameter(ParameterSetName = 'Store')][string]$Thumbprint,
    [Parameter(ParameterSetName = 'Self')][switch]$SelfSigned,
    [Parameter(ParameterSetName = 'Verify')][switch]$VerifyOnly,
    [string[]]$Files,
    [string]$Destination = (Join-Path $PSScriptRoot '..\dist\Antikolt')
)

$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$Destination = [System.IO.Path]::GetFullPath($Destination)

# --- signtool: it ships with the Windows SDK, and there are usually several of them ----------------------
$signtool = Get-ChildItem 'C:\Program Files (x86)\Windows Kits\10\bin' -Recurse -Filter 'signtool.exe' -ErrorAction SilentlyContinue |
    Where-Object { $_.FullName -match '\\x64\\' } |
    Sort-Object FullName -Descending |
    Select-Object -First 1 -ExpandProperty FullName
if (-not $signtool) { throw 'signtool.exe not found — install the Windows SDK (see docs/HU/17-ESZKOZLANC.md)' }

if (-not $Files -or $Files.Count -eq 0) {
    if (-not (Test-Path $Destination)) { throw "nothing to sign: $Destination does not exist (run scripts/dist.ps1 first)" }
    $Files = Get-ChildItem $Destination -Filter '*.exe' | Select-Object -ExpandProperty FullName
}
if ($Files.Count -eq 0) { throw 'nothing to sign' }

Write-Host '=== Antikolt signing ===' -ForegroundColor Cyan
Write-Host "  signtool : $signtool"
foreach ($file in $Files) { Write-Host "  file     : $file" }

$signArgs = @('sign', '/fd', 'SHA256', '/td', 'SHA256', '/tr', 'http://timestamp.digicert.com', '/v')
switch ($PSCmdlet.ParameterSetName) {
    'Pfx' {
        if (-not $Pfx) {
            throw 'no certificate: pass -Pfx <file.pfx>, -Thumbprint <thumb>, -SelfSigned, or sign through SignPath in CI (docs/HU/20-ALASAS.md)'
        }
        $signArgs += @('/f', (Resolve-Path $Pfx).Path)
        if ($Password) { $signArgs += @('/p', $Password) }
    }
    'Store' {
        $signArgs += @('/sha1', $Thumbprint)
    }
    'Self' {
        $subject = 'CN=Antikolt test (NOT TRUSTED)'
        $cert = Get-ChildItem Cert:\CurrentUser\My | Where-Object { $_.Subject -eq $subject } | Select-Object -First 1
        if (-not $cert) {
            Write-Host '  creating a self-signed test certificate (valid one year)' -ForegroundColor Yellow
            $cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject $subject `
                -KeyUsage DigitalSignature -KeySpec Signature `
                -KeyExportPolicy Exportable -NotAfter (Get-Date).AddYears(1)
        }
        $signArgs += @('/sha1', $cert.Thumbprint)
        Write-Host '  WARNING: a self-signed signature is trusted only on machines that installed this certificate.' -ForegroundColor Yellow
    }
}

if (-not $VerifyOnly) {
    foreach ($file in $Files) {
    & $signtool @signArgs $file
    if ($LASTEXITCODE -ne 0) { throw "signing failed for $file" }
    }
}

Write-Host '=== Verification ===' -ForegroundColor Cyan
$failed = 0
foreach ($file in $Files) {
    $output = & $signtool 'verify' '/pa' '/v' $file 2>&1
    $ok = $LASTEXITCODE -eq 0
    if (-not $ok) { $failed++ }
    $subject = ($output | Select-String -Pattern 'Issued to:' | Select-Object -First 1).Line
    $issuer = ($output | Select-String -Pattern 'Issued by:' | Select-Object -First 1).Line
    Write-Host ("  {0} : {1}" -f (Split-Path $file -Leaf), $(if ($ok) { 'signed' } else { 'NOT SIGNED' }))
    if ($subject) { Write-Host "      $($subject.Trim())" }
    if ($issuer) { Write-Host "      $($issuer.Trim())" }
}
if ($failed -gt 0) { throw "$failed file(s) failed verification" }
Write-Host 'DONE — remember that SmartScreen reputation is earned over time, not granted by a signature alone.' -ForegroundColor Green
