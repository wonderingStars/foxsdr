<#
.SYNOPSIS
    DORMANT code-signing step for the FoxSDR setup executable. It does nothing until the owner has bought a code-signing certificate and named it below.

.DESCRIPTION
    NOT WIRED INTO ANYTHING. installer/cascade.iss has no SignTool= line and tools/release.ps1 does not call this script; the hand-off in
    docs/feature-records/sysmodels.md says exactly which lines to add once a certificate exists. IT WAS WRITTEN AND PARSE-CHECKED ONLY: IT HAS NEVER BEEN RUN, because
    running signtool against a certificate store can raise a Windows prompt (smart-card / key-provider dialog), and no lane may do that.

    The certificate is named by ENVIRONMENT VARIABLES, never by anything in this file:
      FOXSDR_SIGN_CERT_SHA1     SHA-1 thumbprint (40 hex) of the certificate in the signing machine's CURRENT USER or LOCAL MACHINE store, or
      FOXSDR_SIGN_PFX           path to a .pfx file (then FOXSDR_SIGN_PFX_PASSWORD_FILE names a file holding its password; the password is never an argument)
      FOXSDR_SIGN_TIMESTAMP_URL RFC 3161 timestamp server URL (https), chosen by the owner
    With none of the certificate variables set, or no timestamp URL, the script REFUSES (exit 2) before it looks for any tool. Nothing is generated, installed or trusted.

    Owner decisions this waits for: which certificate authority and certificate type (an OV certificate, an EV certificate, or Azure Trusted Signing), the cost,
    and which timestamp service. None of that is decided here.

.PARAMETER SetupPath
    The setup executable to sign (an existing .exe).

.PARAMETER Check
    Validate the environment and the path and print what WOULD be run; call nothing. The only mode that is safe to try without a certificate.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$SetupPath,
    [switch]$Check
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

function Stop-Refuse([string]$Why) {
    [Console]::Error.WriteLine("sign-setup: refusing: $Why")
    exit 2
}

$thumb = $env:FOXSDR_SIGN_CERT_SHA1
$pfx = $env:FOXSDR_SIGN_PFX
$pfxPasswordFile = $env:FOXSDR_SIGN_PFX_PASSWORD_FILE
$timestamp = $env:FOXSDR_SIGN_TIMESTAMP_URL

if ([string]::IsNullOrWhiteSpace($thumb) -and [string]::IsNullOrWhiteSpace($pfx)) {
    Stop-Refuse 'no certificate is named (set FOXSDR_SIGN_CERT_SHA1 or FOXSDR_SIGN_PFX); this script is dormant until the owner supplies one'
}
if (-not [string]::IsNullOrWhiteSpace($thumb) -and -not [string]::IsNullOrWhiteSpace($pfx)) {
    Stop-Refuse 'both FOXSDR_SIGN_CERT_SHA1 and FOXSDR_SIGN_PFX are set; name exactly one certificate'
}
if (-not [string]::IsNullOrWhiteSpace($thumb) -and $thumb -notmatch '^[0-9A-Fa-f]{40}$') {
    Stop-Refuse 'FOXSDR_SIGN_CERT_SHA1 is not a 40-hex-digit thumbprint'
}
if (-not [string]::IsNullOrWhiteSpace($pfx)) {
    if (-not (Test-Path -LiteralPath $pfx -PathType Leaf)) { Stop-Refuse 'FOXSDR_SIGN_PFX does not name an existing file' }
    if ([string]::IsNullOrWhiteSpace($pfxPasswordFile) -or -not (Test-Path -LiteralPath $pfxPasswordFile -PathType Leaf)) {
        Stop-Refuse 'a .pfx needs FOXSDR_SIGN_PFX_PASSWORD_FILE naming a file that holds its password (never on a command line)'
    }
}
if ([string]::IsNullOrWhiteSpace($timestamp) -or $timestamp -notmatch '^https://[^\s]+$') {
    Stop-Refuse 'FOXSDR_SIGN_TIMESTAMP_URL must be an https URL; an unsigned-in-time signature expires with the certificate'
}
if (-not (Test-Path -LiteralPath $SetupPath -PathType Leaf) -or ([IO.Path]::GetExtension($SetupPath) -ne '.exe')) {
    Stop-Refuse 'SetupPath is not an existing .exe'
}

# The tool is looked for only now, after every check above has passed, and only as a command already on PATH or in the Windows SDK folder of this machine.
$signtool = (Get-Command signtool.exe -ErrorAction SilentlyContinue)
if ($null -eq $signtool) { Stop-Refuse 'signtool.exe was not found on PATH (install the Windows SDK signing tools on the signing machine)' }

$arguments = @('sign', '/fd', 'sha256', '/td', 'sha256', '/tr', $timestamp)
if (-not [string]::IsNullOrWhiteSpace($thumb)) {
    $arguments += @('/sha1', $thumb)
} else {
    $arguments += @('/f', $pfx, '/p', (Get-Content -LiteralPath $pfxPasswordFile -Raw).Trim())
}
$arguments += $SetupPath

if ($Check) {
    Write-Output "sign-setup: check only. Would run: $($signtool.Source) sign /fd sha256 /td sha256 /tr <timestamp> <certificate by environment> $SetupPath"
    exit 0
}

& $signtool.Source @arguments
if ($LASTEXITCODE -ne 0) { Stop-Refuse "signtool failed with exit code $LASTEXITCODE" }
& $signtool.Source verify /pa /v $SetupPath
if ($LASTEXITCODE -ne 0) { Stop-Refuse "the signature on $SetupPath did not verify (exit code $LASTEXITCODE)" }
Write-Output "sign-setup: signed and verified $SetupPath"
