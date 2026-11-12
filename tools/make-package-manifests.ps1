# make-package-manifests.ps1: fills the winget, Scoop and Chocolatey templates under packaging\ from ONE release
# setup file, and writes the three packages' files into -OutDir.
#
# What it does:  reads the setup file, computes its SHA-256 LOCALLY, takes the version from the file name
#                (foxsdr-setup-<x.y.z>.exe, the name installer\cascade.iss gives it and tools\release.ps1 expects),
#                builds the download URL from the release naming rule
#                (<RepoUrl>/releases/download/v<version>/foxsdr-setup-<version>.exe), replaces every {{Placeholder}}
#                in the templates, and refuses to write anything if one is left over or a template names a
#                placeholder this script does not know.
# What it NEVER does: contact a service, upload, publish, sign or install anything. It uses no web cmdlet, no
#                git, no gh and no ssh; tests\packaging\test_package_manifests.ps1 scans this file for each of them.
#                Submitting the manifests to winget-pkgs, a Scoop bucket or the Chocolatey community repository is the
#                owner's step (docs\feature-records\packaging.md lists the commands).
#
# Windows PowerShell 5.1 and PowerShell 7 both run it.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\make-package-manifests.ps1 `
#       -SetupFile C:\path\foxsdr-setup-0.99.108.exe -OutDir C:\path\package-out
#
# -RunWingetValidate additionally runs `winget validate --manifest <winget folder> --disable-interactivity` (read-only; it
# installs nothing) when winget is on this PC.
#
# Exit codes: 0 done, 1 refused (message says why).

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)] [string] $SetupFile,
    [Parameter(Mandatory = $true)] [string] $OutDir,
    [string] $Version = '',
    [string] $RepoUrl = 'https://github.com/wonderingStars/foxsdr',
    [string] $AppExe = 'foxsdr-slint.exe',
    [string] $TemplateRoot = '',
    [switch] $Force,
    [switch] $RunWingetValidate
)

$ErrorActionPreference = 'Stop'

function Stop-Refused([string] $Message) {
    [Console]::Error.WriteLine('make-package-manifests: ' + $Message)
    exit 1
}

if ($TemplateRoot -eq '') {
    $TemplateRoot = Join-Path (Split-Path -Parent $PSScriptRoot) 'packaging'
}

# --- the setup file and its name ---------------------------------------------------------------------------------------
if (-not (Test-Path -LiteralPath $SetupFile -PathType Leaf)) { Stop-Refused ('the setup file does not exist: ' + $SetupFile) }
$setupItem = Get-Item -LiteralPath $SetupFile
if ($setupItem.Length -le 0) { Stop-Refused ('the setup file is empty: ' + $SetupFile) }

$nameMatch = [regex]::Match($setupItem.Name, '^foxsdr-setup-(\d+\.\d+\.\d+)\.exe$')
if (-not $nameMatch.Success) {
    Stop-Refused ('the setup file name must be foxsdr-setup-<x.y.z>.exe (the release naming rule), got: ' + $setupItem.Name)
}
$nameVersion = $nameMatch.Groups[1].Value
if ($Version -eq '') { $Version = $nameVersion }
if ($Version -notmatch '^\d+\.\d+\.\d+$') { Stop-Refused ('the version must be x.y.z, got: ' + $Version) }
if ($Version -ne $nameVersion) {
    Stop-Refused ('-Version ' + $Version + ' does not match the version in the file name (' + $nameVersion + ')')
}
if ($RepoUrl -notmatch '^https://[A-Za-z0-9.-]+/[A-Za-z0-9._-]+/[A-Za-z0-9._-]+$') { Stop-Refused ('-RepoUrl must be https://host/owner/repo, got: ' + $RepoUrl) }
if ($AppExe -notmatch '^[A-Za-z0-9._-]+\.exe$') { Stop-Refused ('-AppExe must be a file name ending in .exe, got: ' + $AppExe) }

# --- the hash, computed here ------------------------------------------------------------------------------------------
$sha = [System.Security.Cryptography.SHA256]::Create()
$stream = [System.IO.File]::OpenRead($setupItem.FullName)
try { $hashBytes = $sha.ComputeHash($stream) } finally { $stream.Dispose(); $sha.Dispose() }
$sha256Lower = (($hashBytes | ForEach-Object { $_.ToString('x2') }) -join '')

$tag = 'v' + $Version
$values = @{
    'Version'         = $Version
    'InstallerUrl'    = $RepoUrl + '/releases/download/' + $tag + '/' + $setupItem.Name
    'Sha256Lower'     = $sha256Lower
    'Sha256Upper'     = $sha256Lower.ToUpperInvariant()
    'RepoUrl'         = $RepoUrl
    'ReleaseNotesUrl' = $RepoUrl + '/releases/tag/' + $tag
    'AppExe'          = $AppExe
}

# --- what is written where ------------------------------------------------------------------------------------------------
$plan = @(
    @{ Template = 'winget\templates\StevenFox.FoxSDR.yaml.template';               Out = 'winget\StevenFox.FoxSDR.yaml' },
    @{ Template = 'winget\templates\StevenFox.FoxSDR.locale.en-GB.yaml.template';  Out = 'winget\StevenFox.FoxSDR.locale.en-GB.yaml' },
    @{ Template = 'winget\templates\StevenFox.FoxSDR.installer.yaml.template';     Out = 'winget\StevenFox.FoxSDR.installer.yaml' },
    @{ Template = 'scoop\foxsdr.json.template';                                    Out = 'scoop\foxsdr.json' },
    @{ Template = 'chocolatey\foxsdr.nuspec.template';                             Out = 'chocolatey\foxsdr\foxsdr.nuspec' },
    @{ Template = 'chocolatey\tools\chocolateyInstall.ps1.template';               Out = 'chocolatey\foxsdr\tools\chocolateyInstall.ps1' },
    @{ Template = 'chocolatey\tools\chocolateyUninstall.ps1.template';             Out = 'chocolatey\foxsdr\tools\chocolateyUninstall.ps1' }
)

# --- render everything in memory first: nothing is written unless every file is complete -----------------------------------
$rendered = @()
foreach ($item in $plan) {
    $templatePath = Join-Path $TemplateRoot $item.Template
    if (-not (Test-Path -LiteralPath $templatePath -PathType Leaf)) { Stop-Refused ('template missing: ' + $templatePath) }
    $text = [System.IO.File]::ReadAllText($templatePath, (New-Object System.Text.UTF8Encoding($false)))
    foreach ($m in [regex]::Matches($text, '\{\{([A-Za-z0-9_]+)\}\}')) {
        if (-not $values.ContainsKey($m.Groups[1].Value)) {
            Stop-Refused ('template ' + $item.Template + ' names an unknown placeholder: {{' + $m.Groups[1].Value + '}}')
        }
    }
    foreach ($k in $values.Keys) { $text = $text.Replace('{{' + $k + '}}', $values[$k]) }
    if ($text.Contains('{{') -or $text.Contains('}}')) {
        Stop-Refused ('a placeholder is left unfilled (or a stray brace pair is in) ' + $item.Template)
    }
    $rendered += , @{ Out = $item.Out; Text = $text }
}

# --- write -------------------------------------------------------------------------------------------------------------------
if ((Test-Path -LiteralPath $OutDir) -and ((Get-ChildItem -LiteralPath $OutDir -Force | Measure-Object).Count -gt 0) -and -not $Force) {
    Stop-Refused ('the output folder is not empty (use -Force to write into it): ' + $OutDir)
}
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)
foreach ($r in $rendered) {
    $dest = Join-Path $OutDir $r.Out
    $parent = Split-Path -Parent $dest
    if (-not (Test-Path -LiteralPath $parent)) { New-Item -ItemType Directory -Force -Path $parent | Out-Null }
    [System.IO.File]::WriteAllText($dest, $r.Text, $utf8NoBom)
}

Write-Host ('version      ' + $Version)
Write-Host ('setup file   ' + $setupItem.Name + ' (' + $setupItem.Length + ' bytes)')
Write-Host ('sha256       ' + $sha256Lower)
Write-Host ('url          ' + $values['InstallerUrl'])
Write-Host ('written to   ' + $OutDir)
foreach ($r in $rendered) { Write-Host ('  ' + $r.Out) }

if ($RunWingetValidate) {
    $winget = Get-Command winget -ErrorAction SilentlyContinue
    if ($null -eq $winget) { Stop-Refused 'winget is not on this PC, so -RunWingetValidate cannot run' }
    $wingetDir = Join-Path $OutDir 'winget'
    & $winget.Source validate --manifest $wingetDir --disable-interactivity
    if ($LASTEXITCODE -ne 0) { Stop-Refused ('winget validate failed with exit code ' + $LASTEXITCODE) }
}
exit 0
