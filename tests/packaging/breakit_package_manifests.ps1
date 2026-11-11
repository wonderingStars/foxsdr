#requires -Version 7
# Break-it check for tests\packaging\test_package_manifests.ps1: each mutant removes ONE behaviour from a COPY of the
# generator or of the templates (the originals are never touched), the suite is run against it, and the suite must go RED.
# Then it is run against the unmodified files and must be GREEN. Prints a table; exit code 1 if any mutant survived.
#
#   pwsh -NoProfile -File tests\packaging\breakit_package_manifests.ps1

param([string] $Only = '')   # run only the mutants whose name contains this text

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$gen = Join-Path $repo 'tools\make-package-manifests.ps1'
$tpl = Join-Path $repo 'packaging'
$suite = Join-Path $PSScriptRoot 'test_package_manifests.ps1'
$work = Join-Path ([IO.Path]::GetTempPath()) ('packaging-breakit-' + $PID)
New-Item -ItemType Directory -Force -Path $work | Out-Null
$pwsh = (Get-Command pwsh).Source

function Mutate([string] $Text, [string] $Old, [string] $New) {
    if (-not $Text.Contains($Old)) { throw ('mutant anchor not found: ' + $Old) }
    return $Text.Replace($Old, $New)
}
function Run-Suite([string] $GeneratorPath, [string] $TemplateRoot) {
    $out = Join-Path $work ('suite-' + [guid]::NewGuid().ToString('N') + '.txt')
    $p = Start-Process -FilePath $pwsh -ArgumentList @('-NoProfile', '-File', $suite, '-GeneratorPath', $GeneratorPath, '-TemplateRoot', $TemplateRoot, '-ScratchRoot', (Join-Path $work ('s' + [guid]::NewGuid().ToString('N').Substring(0, 8)))) `
        -NoNewWindow -Wait -PassThru -RedirectStandardOutput $out -RedirectStandardError ($out + '.err')
    $lines = @(Get-Content -LiteralPath $out)
    return [pscustomobject]@{ Exit = $p.ExitCode; Fails = @($lines | Where-Object { $_ -like 'FAIL*' }); Summary = ($lines | Select-Object -Last 1) }
}

$genText = [IO.File]::ReadAllText($gen)
$utf8 = New-Object System.Text.UTF8Encoding($false)

# each mutant: name, kind (gen|tpl), edit
$mutants = @(
    @{ Name = 'generator hashes the file NAME, not the file'; Kind = 'gen'; Old = '$hashBytes = $sha.ComputeHash($stream)'; New = '$hashBytes = $sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($setupItem.Name))' },
    @{ Name = 'generator builds the URL with latest instead of the version tag'; Kind = 'gen'; Old = "'/releases/download/' + `$tag + '/'"; New = "'/releases/latest/download/'" },
    @{ Name = 'generator does not upper-case the winget hash'; Kind = 'gen'; Old = "'Sha256Upper'     = `$sha256Lower.ToUpperInvariant()"; New = "'Sha256Upper'     = `$sha256Lower" },
    @{ Name = 'generator does not check that -Version matches the file name'; Kind = 'gen'; Old = 'if ($Version -ne $nameVersion) {'; New = 'if ($false) {' },
    @{ Name = 'generator accepts any file name'; Kind = 'gen'; Old = 'if (-not $nameMatch.Success) {'; New = 'if ($false) {' },
    @{ Name = 'generator accepts an empty setup file'; Kind = 'gen'; Old = 'if ($setupItem.Length -le 0) {'; New = 'if ($false) {' },
    @{ Name = 'generator does not refuse a non-empty output folder'; Kind = 'gen'; Old = '-and -not $Force) {'; New = '-and $false) {' },
    @{ Name = 'generator does not refuse leftover placeholders'; Kind = 'gen'; Old = "if (`$text.Contains('{{') -or `$text.Contains('}}')) {"; New = 'if ($false) {' },
    @{ Name = 'generator writes each file as soon as it is rendered (no all-or-nothing)'; Kind = 'gen';
       Old = "    `$rendered += , @{ Out = `$item.Out; Text = `$text }";
       New = "    New-Item -ItemType Directory -Force -Path (Split-Path -Parent (Join-Path `$OutDir `$item.Out)) | Out-Null; [IO.File]::WriteAllText((Join-Path `$OutDir `$item.Out), `$text); `$rendered += , @{ Out = `$item.Out; Text = `$text }" },
    @{ Name = 'generator contacts a service (a web call is added)'; Kind = 'gen'; Old = "`$tag = 'v' + `$Version"; New = "`$tag = 'v' + `$Version; Invoke-WebRequest -Uri 'http://127.0.0.1:9/' -ErrorAction SilentlyContinue | Out-Null" },
    @{ Name = 'generator runs winget install'; Kind = 'gen'; Old = '& $winget.Source validate --manifest $wingetDir --disable-interactivity'; New = '& $winget.Source install --manifest $wingetDir' },
    @{ Name = 'winget installer template: wrong ManifestVersion'; Kind = 'tpl'; File = 'winget\templates\StevenFox.FoxSDR.installer.yaml.template'; Old = "ManifestType: installer`nManifestVersion: 1.12.0"; New = "ManifestType: installer`nManifestVersion: 9.9.9" },
    @{ Name = 'winget installer template: InstallerSha256 dropped'; Kind = 'tpl'; File = 'winget\templates\StevenFox.FoxSDR.installer.yaml.template'; Old = "  InstallerSha256: {{Sha256Upper}}`n"; New = '' },
    @{ Name = 'winget locale template: unknown InstallerType value'; Kind = 'tpl'; File = 'winget\templates\StevenFox.FoxSDR.installer.yaml.template'; Old = 'InstallerType: inno'; New = 'InstallerType: innosetup' },
    @{ Name = 'scoop template: homepage dropped'; Kind = 'tpl'; File = 'scoop\foxsdr.json.template'; Old = "    `"homepage`": `"https://foxsdr.com`",`n"; New = '' },
    @{ Name = 'scoop template: hash key misspelt'; Kind = 'tpl'; File = 'scoop\foxsdr.json.template'; Old = '"hash": "{{Sha256Lower}}"'; New = '"hasch": "{{Sha256Lower}}"' },
    @{ Name = 'scoop template: hash replaced by a constant'; Kind = 'tpl'; File = 'scoop\foxsdr.json.template'; Old = '"hash": "{{Sha256Lower}}"'; New = ('"hash": "' + ('0' * 64) + '"') },
    @{ Name = 'nuspec template: <id> dropped'; Kind = 'tpl'; File = 'chocolatey\foxsdr.nuspec.template'; Old = "    <id>foxsdr</id>`n"; New = '' },
    @{ Name = 'nuspec template: unclosed element (not well-formed)'; Kind = 'tpl'; File = 'chocolatey\foxsdr.nuspec.template'; Old = '<title>FoxSDR</title>'; New = '<title>FoxSDR' },
    @{ Name = 'nuspec template: wrong namespace'; Kind = 'tpl'; File = 'chocolatey\foxsdr.nuspec.template'; Old = 'packaging/2010/07/nuspec.xsd'; New = 'packaging/1999/01/nuspec.xsd' },
    @{ Name = 'chocolateyInstall template: checksum dropped'; Kind = 'tpl'; File = 'chocolatey\tools\chocolateyInstall.ps1.template'; Old = "  checksum64     = '{{Sha256Lower}}'`n"; New = '' },
    @{ Name = 'chocolateyInstall template: parameter name misspelt (url64bits instead of url64bit)'; Kind = 'tpl'; File = 'chocolatey\tools\chocolateyInstall.ps1.template'; Old = 'url64bit       =' ; New = 'url64bits      =' },
    @{ Name = 'chocolateyInstall template: silent switch /S that release.ps1 never uses'; Kind = 'tpl'; File = 'chocolatey\tools\chocolateyInstall.ps1.template'; Old = '/VERYSILENT /SUPPRESSMSGBOXES /NORESTART'; New = '/S' },
    @{ Name = 'chocolateyUninstall template: UninstallString left quoted'; Kind = 'tpl'; File = 'chocolatey\tools\chocolateyUninstall.ps1.template'; Old = '.UninstallString.Trim(''"'')'; New = '.UninstallString' },
    @{ Name = 'chocolateyUninstall template: uninstalls when two matches are found'; Kind = 'tpl'; File = 'chocolatey\tools\chocolateyUninstall.ps1.template'; Old = 'if ($key.Count -eq 1) {'; New = 'if ($key.Count -ge 1) {' },
    @{ Name = 'chocolateyUninstall template: asks for /CLEANDATA'; Kind = 'tpl'; File = 'chocolatey\tools\chocolateyUninstall.ps1.template'; Old = "'/VERYSILENT /SUPPRESSMSGBOXES /NORESTART'"; New = "'/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /CLEANDATA'" }
)

$survived = 0
$rows = @()
foreach ($m in ($mutants | Where-Object { $Only -eq '' -or $_.Name -like ('*' + $Only + '*') })) {
    $dir = Join-Path $work ('m' + [guid]::NewGuid().ToString('N').Substring(0, 8))
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    $g = $gen
    $t = $tpl
    if ($m.Kind -eq 'gen') {
        $g = Join-Path $dir 'make-package-manifests.ps1'
        [IO.File]::WriteAllText($g, (Mutate $genText $m.Old $m.New), $utf8)
    } else {
        $t = Join-Path $dir 'packaging'
        Copy-Item -Recurse -LiteralPath $tpl -Destination $t
        $f = Join-Path $t $m.File
        $orig = [IO.File]::ReadAllText($f).Replace("`r`n", "`n")
        [IO.File]::WriteAllText($f, (Mutate $orig $m.Old.Replace("`r`n", "`n") $m.New), $utf8)
    }
    $r = Run-Suite $g $t
    $red = ($r.Exit -ne 0)
    if (-not $red) { $survived++ }
    $first = if ($r.Fails.Count -gt 0) { ($r.Fails[0] -replace '^FAIL\s+', '') } else { '(none)' }
    $rows += [pscustomobject]@{ Mutant = $m.Name; Result = $(if ($red) { 'RED' } else { 'SURVIVED' }); 'First failing check' = $first }
}
$rows | Format-Table -AutoSize -Wrap | Out-String -Width 220 | Write-Host

$clean = Run-Suite $gen $tpl
Write-Host ('unmodified generator and templates: ' + $clean.Summary + ' (exit ' + $clean.Exit + ')')
Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
Write-Host (@($rows).Count.ToString() + ' mutants, ' + $survived.ToString() + ' survived')
if ($survived -gt 0 -or $clean.Exit -ne 0) { exit 1 }
exit 0
