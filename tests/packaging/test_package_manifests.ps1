#requires -Version 7
# Tests tools\make-package-manifests.ps1 and the three package templates under packaging\.
# Runs without building or installing anything, contacts nothing, and touches only its own scratch folder.
#
#   pwsh -NoProfile -File tests\packaging\test_package_manifests.ps1
#
# -GeneratorPath / -TemplateRoot let tests\packaging\breakit_package_manifests.ps1 run this same suite against a MUTATED
# copy of the generator or of the templates (the break-it check). Prints "N checks, M failed"; exit code 1 if any failed.

param(
    [string] $GeneratorPath = '',
    [string] $TemplateRoot = '',
    [string] $ScratchRoot = ''
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
if ($GeneratorPath -eq '') { $GeneratorPath = Join-Path $repo 'tools\make-package-manifests.ps1' }
if ($TemplateRoot -eq '') { $TemplateRoot = Join-Path $repo 'packaging' }
if ($ScratchRoot -eq '') { $ScratchRoot = Join-Path ([IO.Path]::GetTempPath()) ('packaging-test-' + $PID) }
$schemaPath = Join-Path $PSScriptRoot 'scoop-manifest.schema.json'

$script:checks = 0
$script:failed = 0
function Check([string] $Name, [bool] $Ok, [string] $Detail = '') {
    $script:checks++
    if (-not $Ok) {
        $script:failed++
        Write-Host ('FAIL  ' + $Name + $(if ($Detail -ne '') { '  ' + $Detail } else { '' }))
    }
}

# The pinned hash of the fake setup file (bytes 1..200), computed independently with Python's hashlib on 2026-11-11.
$fakeBytes = [byte[]](1..200)
$fakeSha = 'a48742653620d8aeee06f9aef53889980ead41a44aeb11c5b0996dfc4fbc127b'

New-Item -ItemType Directory -Force -Path $ScratchRoot | Out-Null
$setup = Join-Path $ScratchRoot 'foxsdr-setup-9.8.7.exe'
[IO.File]::WriteAllBytes($setup, $fakeBytes)

function Invoke-Generator([string[]] $Arguments, [string] $Shell = 'pwsh') {
    $exe = if ($Shell -eq 'pwsh') { (Get-Command pwsh).Source } else { Join-Path $env:WINDIR 'System32\WindowsPowerShell\v1.0\powershell.exe' }
    $all = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $GeneratorPath) + $Arguments
    $stdout = Join-Path $ScratchRoot ('gen-out-' + [guid]::NewGuid().ToString('N') + '.txt')
    $stderr = $stdout + '.err'
    $p = Start-Process -FilePath $exe -ArgumentList $all -NoNewWindow -Wait -PassThru -RedirectStandardOutput $stdout -RedirectStandardError $stderr
    $r = [pscustomobject]@{ Exit = $p.ExitCode; Out = (Get-Content -Raw -LiteralPath $stdout); Err = (Get-Content -Raw -LiteralPath $stderr) }
    if ($null -eq $r.Out) { $r.Out = '' }
    if ($null -eq $r.Err) { $r.Err = '' }
    return $r
}

# ---------------------------------------------------------------------------------------------------------------------
# 1. The happy path, through BOTH shells; the two outputs must be identical.
# ---------------------------------------------------------------------------------------------------------------------
$outPwsh = Join-Path $ScratchRoot 'out-pwsh'
$outWin = Join-Path $ScratchRoot 'out-winps'
$extra = @('-TemplateRoot', $TemplateRoot)
$r1 = Invoke-Generator (@('-SetupFile', $setup, '-OutDir', $outPwsh) + $extra) 'pwsh'
Check 'generator (pwsh 7) exits 0' ($r1.Exit -eq 0) ($r1.Err + $r1.Out)
$r2 = Invoke-Generator (@('-SetupFile', $setup, '-OutDir', $outWin) + $extra) 'winps'
Check 'generator (Windows PowerShell 5.1) exits 0' ($r2.Exit -eq 0) ($r2.Err + $r2.Out)

$expectedFiles = @(
    'winget\StevenFox.FoxSDR.yaml', 'winget\StevenFox.FoxSDR.locale.en-GB.yaml', 'winget\StevenFox.FoxSDR.installer.yaml',
    'scoop\foxsdr.json',
    'chocolatey\foxsdr\foxsdr.nuspec', 'chocolatey\foxsdr\tools\chocolateyInstall.ps1', 'chocolatey\foxsdr\tools\chocolateyUninstall.ps1'
)
foreach ($f in $expectedFiles) {
    Check ('written (pwsh): ' + $f) (Test-Path -LiteralPath (Join-Path $outPwsh $f))
    Check ('written (5.1): ' + $f) (Test-Path -LiteralPath (Join-Path $outWin $f))
    if ((Test-Path -LiteralPath (Join-Path $outPwsh $f)) -and (Test-Path -LiteralPath (Join-Path $outWin $f))) {
        $a = (Get-FileHash -LiteralPath (Join-Path $outPwsh $f) -Algorithm SHA256).Hash
        $b = (Get-FileHash -LiteralPath (Join-Path $outWin $f) -Algorithm SHA256).Hash
        Check ('identical from both shells: ' + $f) ($a -eq $b)
    }
}
$allOut = @(Get-ChildItem -LiteralPath $outPwsh -Recurse -File)
Check 'exactly the seven expected files, nothing else' ($allOut.Count -eq 7) ('found ' + $allOut.Count)

# every placeholder filled, no BOM, the version/hash/url in the right places
$url = 'https://github.com/wonderingStars/foxsdr/releases/download/v9.8.7/foxsdr-setup-9.8.7.exe'
foreach ($f in $allOut) {
    $text = [IO.File]::ReadAllText($f.FullName)
    Check ('no placeholder left: ' + $f.Name) (-not $text.Contains('{{') -and -not $text.Contains('}}'))
    $bytes = [IO.File]::ReadAllBytes($f.FullName)
    Check ('no byte-order mark: ' + $f.Name) (-not ($bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF))
}
$wInst = Get-Content -Raw (Join-Path $outPwsh 'winget\StevenFox.FoxSDR.installer.yaml')
$wVer = Get-Content -Raw (Join-Path $outPwsh 'winget\StevenFox.FoxSDR.yaml')
$wLoc = Get-Content -Raw (Join-Path $outPwsh 'winget\StevenFox.FoxSDR.locale.en-GB.yaml')
Check 'winget installer: URL by the release naming rule' ($wInst -match ('(?m)^  InstallerUrl: ' + [regex]::Escape($url) + '\r?$'))
Check 'winget installer: SHA-256 is the file''s (upper case, matched case-sensitively)' ($wInst -cmatch ('(?m)^  InstallerSha256: ' + $fakeSha.ToUpperInvariant() + '\r?$'))
Check 'winget installer: InstallerType inno' ($wInst -match '(?m)^InstallerType: inno\r?$')
foreach ($pair in @(@('version', $wVer), @('locale', $wLoc), @('installer', $wInst))) {
    Check ('winget ' + $pair[0] + ' file: PackageVersion 9.8.7') ($pair[1] -match '(?m)^PackageVersion: 9\.8\.7\r?$')
    Check ('winget ' + $pair[0] + ' file: PackageIdentifier StevenFox.FoxSDR') ($pair[1] -match '(?m)^PackageIdentifier: StevenFox\.FoxSDR\r?$')
    Check ('winget ' + $pair[0] + ' file: ManifestVersion 1.12.0') ($pair[1] -match '(?m)^ManifestVersion: 1\.12\.0\r?$')
}
Check 'winget version file: ManifestType version' ($wVer -match '(?m)^ManifestType: version\r?$')
Check 'winget locale file: ManifestType defaultLocale' ($wLoc -match '(?m)^ManifestType: defaultLocale\r?$')
Check 'winget installer file: ManifestType installer' ($wInst -match '(?m)^ManifestType: installer\r?$')
Check 'winget locale: ShortDescription within 3..256 characters' (($wLoc -match '(?m)^ShortDescription: (.+?)\r?$') -and $Matches[1].Length -ge 3 -and $Matches[1].Length -le 256)
Check 'winget locale: at most 16 tags' ((([regex]::Matches($wLoc, '(?m)^- [a-z0-9-]+\r?$')).Count) -le 16)

# ---------------------------------------------------------------------------------------------------------------------
# 2. winget validate (read-only, installs nothing), when winget is on this PC
# ---------------------------------------------------------------------------------------------------------------------
$winget = Get-Command winget -ErrorAction SilentlyContinue
if ($null -ne $winget) {
    $v = Start-Process -FilePath $winget.Source -ArgumentList @('validate', '--manifest', (Join-Path $outPwsh 'winget'), '--disable-interactivity') -NoNewWindow -Wait -PassThru `
        -RedirectStandardOutput (Join-Path $ScratchRoot 'wv.out') -RedirectStandardError (Join-Path $ScratchRoot 'wv.err')
    $vt = (Get-Content -Raw (Join-Path $ScratchRoot 'wv.out')) + (Get-Content -Raw (Join-Path $ScratchRoot 'wv.err'))
    Check 'winget validate --manifest accepts the generated manifests' ($v.ExitCode -eq 0 -and $vt -match 'Manifest validation succeeded') $vt
    # winget itself must refuse a manifest with one required field removed (proves the validation really ran)
    $bad = Join-Path $ScratchRoot 'wingetbad'
    Copy-Item -Recurse -LiteralPath (Join-Path $outPwsh 'winget') -Destination $bad
    $badInst = Join-Path $bad 'StevenFox.FoxSDR.installer.yaml'
    (Get-Content -Raw $badInst) -replace '(?m)^  InstallerSha256: .*\r?\n', '' | Set-Content -NoNewline -Encoding utf8NoBOM $badInst
    $vb = Start-Process -FilePath $winget.Source -ArgumentList @('validate', '--manifest', $bad, '--disable-interactivity') -NoNewWindow -Wait -PassThru `
        -RedirectStandardOutput (Join-Path $ScratchRoot 'wvb.out') -RedirectStandardError (Join-Path $ScratchRoot 'wvb.err')
    Check 'winget validate refuses a manifest without InstallerSha256' ($vb.ExitCode -ne 0)
} else {
    Write-Host 'SKIPPED  winget is not on this PC: winget validate was not run'
}

# ---------------------------------------------------------------------------------------------------------------------
# 3. Scoop: JSON well-formed, schema, values; negative cases
# ---------------------------------------------------------------------------------------------------------------------
$scoopPath = Join-Path $outPwsh 'scoop\foxsdr.json'
$scoopText = Get-Content -Raw $scoopPath
$schemaText = Get-Content -Raw $schemaPath
Check 'scoop manifest is JSON' (Test-Json -Json $scoopText -ErrorAction SilentlyContinue)
Check 'scoop manifest passes the schema' (Test-Json -Json $scoopText -Schema $schemaText -ErrorAction SilentlyContinue)
$sc = $scoopText | ConvertFrom-Json
Check 'scoop: version' ($sc.version -eq '9.8.7')
Check 'scoop: url by the release naming rule' ($sc.architecture.'64bit'.url -eq $url)
Check 'scoop: hash is the file''s (lower case)' ($sc.architecture.'64bit'.hash -eq $fakeSha)
Check 'scoop: innosetup true' ($sc.innosetup -eq $true)
Check 'scoop: autoupdate url carries $version and the release naming rule' ($sc.autoupdate.architecture.'64bit'.url -eq 'https://github.com/wonderingStars/foxsdr/releases/download/v$version/foxsdr-setup-$version.exe')
Check 'scoop: shortcut names the application exe' ($sc.shortcuts[0][0] -eq 'foxsdr-slint.exe')
$neg = @(
    @{ N = 'without version'; Edit = { param($o) $o.PSObject.Properties.Remove('version') } },
    @{ N = 'without homepage'; Edit = { param($o) $o.PSObject.Properties.Remove('homepage') } },
    @{ N = 'without license'; Edit = { param($o) $o.PSObject.Properties.Remove('license') } },
    @{ N = 'with an unknown key'; Edit = { param($o) $o | Add-Member -NotePropertyName 'hasch' -NotePropertyValue 'x' } },
    @{ N = 'with a 63-digit hash'; Edit = { param($o) $o.architecture.'64bit'.hash = $o.architecture.'64bit'.hash.Substring(1) } },
    @{ N = 'with a hash that is not hex'; Edit = { param($o) $o.architecture.'64bit'.hash = ('z' * 64) } },
    @{ N = 'with a url that is not http(s)'; Edit = { param($o) $o.architecture.'64bit'.url = 'ftp://example/x.exe' } },
    @{ N = 'with an unknown architecture'; Edit = { param($o) $o.architecture | Add-Member -NotePropertyName 'ia64' -NotePropertyValue ([pscustomobject]@{}) } },
    @{ N = 'with innosetup as a string'; Edit = { param($o) $o.innosetup = 'true' } },
    @{ N = 'with a license object without identifier'; Edit = { param($o) $o.license = [pscustomobject]@{ url = 'https://x.example/l' } } }
)
foreach ($n in $neg) {
    $o = $scoopText | ConvertFrom-Json
    & $n.Edit $o
    $j = $o | ConvertTo-Json -Depth 20
    Check ('scoop schema refuses a manifest ' + $n.N) (-not (Test-Json -Json $j -Schema $schemaText -ErrorAction SilentlyContinue))
}

# ---------------------------------------------------------------------------------------------------------------------
# 4. Chocolatey: nuspec well-formed and structurally complete; scripts parse and behave (stubbed helpers)
# ---------------------------------------------------------------------------------------------------------------------
$nuspecPath = Join-Path $outPwsh 'chocolatey\foxsdr\foxsdr.nuspec'
$nuspec = $null
try { $nuspec = [xml](Get-Content -Raw -LiteralPath $nuspecPath); Check 'nuspec is well-formed XML' $true } catch { Check 'nuspec is well-formed XML' $false $_.Exception.Message }
if ($null -ne $nuspec) {
    $ns = New-Object System.Xml.XmlNamespaceManager($nuspec.NameTable)
    $ns.AddNamespace('n', 'http://schemas.microsoft.com/packaging/2010/07/nuspec.xsd')
    $md = $nuspec.SelectSingleNode('/n:package/n:metadata', $ns)
    Check 'nuspec: root is package in the nuspec namespace with a metadata element' ($null -ne $md)
    if ($null -ne $md) {
        foreach ($req in @('id', 'version', 'description', 'authors')) {
            $el = $md.SelectSingleNode('n:' + $req, $ns)
            Check ('nuspec: required element <' + $req + '> present and not empty') ($null -ne $el -and $el.InnerText.Trim().Length -gt 0)
        }
        Check 'nuspec: id is foxsdr (lower case, no dots)' ($md.SelectSingleNode('n:id', $ns).InnerText -ceq 'foxsdr')
        Check 'nuspec: version 9.8.7' ($md.SelectSingleNode('n:version', $ns).InnerText -eq '9.8.7')
        $d = $md.SelectSingleNode('n:description', $ns)
        Check 'nuspec: description 30..4000 characters' ($null -ne $d -and $d.InnerText.Length -ge 30 -and $d.InnerText.Length -le 4000)
        $tags = $md.SelectSingleNode('n:tags', $ns)
        Check 'nuspec: tags present, space separated, no comma, none is chocolatey' ($null -ne $tags -and $tags.InnerText -notmatch ',' -and (($tags.InnerText -split '\s+') -notcontains 'chocolatey'))
        Check 'nuspec: title differs from id' ($md.SelectSingleNode('n:title', $ns).InnerText -cne $md.SelectSingleNode('n:id', $ns).InnerText)
        Check 'nuspec: licenseUrl differs from projectUrl' ($md.SelectSingleNode('n:licenseUrl', $ns).InnerText -ne $md.SelectSingleNode('n:projectUrl', $ns).InnerText)
        Check 'nuspec: licence acceptance not required' ($md.SelectSingleNode('n:requireLicenseAcceptance', $ns).InnerText -eq 'false')
        foreach ($u in @('projectUrl', 'licenseUrl', 'packageSourceUrl', 'releaseNotes')) {
            $t = $md.SelectSingleNode('n:' + $u, $ns).InnerText
            Check ('nuspec: ' + $u + ' is an https URL') ($t -match '^https://')
        }
        $fileEl = $nuspec.SelectSingleNode('/n:package/n:files/n:file', $ns)
        Check 'nuspec: files element maps tools\** to tools' ($null -ne $fileEl -and $fileEl.GetAttribute('src') -eq 'tools\**' -and $fileEl.GetAttribute('target') -eq 'tools')
    }
}
$nuspecBytes = [IO.File]::ReadAllBytes($nuspecPath)
Check 'nuspec declares UTF-8' ([Text.Encoding]::UTF8.GetString($nuspecBytes, 0, 60) -match 'encoding="utf-8"')

function Read-Ast([string] $Path) {
    $tokens = $null; $errors = $null
    $ast = [System.Management.Automation.Language.Parser]::ParseFile($Path, [ref]$tokens, [ref]$errors)
    return [pscustomobject]@{ Ast = $ast; Errors = @($errors) }
}
$installPath = Join-Path $outPwsh 'chocolatey\foxsdr\tools\chocolateyInstall.ps1'
$uninstallPath = Join-Path $outPwsh 'chocolatey\foxsdr\tools\chocolateyUninstall.ps1'
$ia = Read-Ast $installPath
$ua = Read-Ast $uninstallPath
Check 'chocolateyInstall.ps1 parses with no errors' ($ia.Errors.Count -eq 0)
Check 'chocolateyUninstall.ps1 parses with no errors' ($ua.Errors.Count -eq 0)
$cmdNames = @($ia.Ast.FindAll({ param($n) $n -is [System.Management.Automation.Language.CommandAst] }, $true) | ForEach-Object { $_.GetCommandName() })
Check 'install script calls Install-ChocolateyPackage' ($cmdNames -contains 'Install-ChocolateyPackage')
$ucmds = @($ua.Ast.FindAll({ param($n) $n -is [System.Management.Automation.Language.CommandAst] }, $true) | ForEach-Object { $_.GetCommandName() })
Check 'uninstall script calls Get-UninstallRegistryKey and Uninstall-ChocolateyPackage' (($ucmds -contains 'Get-UninstallRegistryKey') -and ($ucmds -contains 'Uninstall-ChocolateyPackage'))
Check 'install script never calls Start-Process (CPMR0046)' ($cmdNames -notcontains 'Start-Process' -and $ucmds -notcontains 'Start-Process')

# Run the generated scripts with the Chocolatey helpers STUBBED here: they record what they were called with.
$stubRun = {
    param($ScriptPath, $Stubs, $Env)
    foreach ($k in $Env.Keys) { Set-Item -Path ('env:' + $k) -Value $Env[$k] }
    $global:calls = @()
    $global:warnings = @()
    # The stubs declare exactly the parameters the Chocolatey function reference lists (docs.chocolatey.org, opened
    # 2026-11-11, no aliases), so a script that passes any other name fails here instead of on a user's machine.
    function Install-ChocolateyPackage {
        param($PackageName, $FileType, $SilentArgs, $Url, $Url64bit, $ValidExitCodes, $Checksum, $ChecksumType, $Checksum64,
              $ChecksumType64, $Options, $File, $File64, $UseOnlyPackageSilentArguments, $UseOriginalLocation, $BeforeInstall, $IgnoredArguments)
        $global:calls += , @{ Name = 'Install'; Bound = $PSBoundParameters }
    }
    function Uninstall-ChocolateyPackage {
        param($PackageName, $FileType, $SilentArgs, $File, $ValidExitCodes, $IgnoredArguments)
        $global:calls += , @{ Name = 'Uninstall'; Bound = $PSBoundParameters }
    }
    function Get-UninstallRegistryKey { param($SoftwareName) $global:softwareName = $SoftwareName; return $Stubs }
    function Write-Warning { param($Message) $global:warnings += $Message }
    . $ScriptPath
    return [pscustomobject]@{ Calls = $global:calls; Warnings = $global:warnings; SoftwareName = $global:softwareName }
}
function Invoke-Stub([string] $Path, $Stubs) {
    try { return (& $stubRun $Path $Stubs @{ ChocolateyPackageName = 'foxsdr' }) }
    catch { Check ('script runs against the stubbed Chocolatey helpers: ' + (Split-Path -Leaf $Path)) $false $_.Exception.Message; return [pscustomobject]@{ Calls = @(); Warnings = @(); SoftwareName = '' } }
}
$run = Invoke-Stub $installPath @()
$ic = @($run.Calls)
Check 'install script makes exactly one Install-ChocolateyPackage call' ($ic.Count -eq 1)
if ($ic.Count -eq 1) {
    $b = $ic[0].Bound
    Check 'install: url64bit is the release URL' ($b['url64bit'] -eq $url)
    Check 'install: checksum64 is the SHA-256 of the file' ($b['checksum64'] -eq $fakeSha)
    Check 'install: checksumType64 sha256' ($b['checksumType64'] -eq 'sha256')
    Check 'install: fileType exe' ($b['fileType'] -eq 'exe')
    Check 'install: packageName comes from the environment' ($b['packageName'] -eq 'foxsdr')
    Check 'install: valid exit codes are 0 only' (($b['validExitCodes'] -join ',') -eq '0')
    # the silent arguments are the ones tools\release.ps1 itself runs the setup with
    $rel = Get-Content -Raw (Join-Path $repo 'tools\release.ps1')
    foreach ($sw in ($b['silentArgs'] -split ' ')) {
        Check ('install: silent switch ' + $sw + ' is one tools\release.ps1 runs the setup with') ($rel.Contains("'" + $sw + "'"))
    }
    Check 'install: checksum is not the SHA-256 of nothing' ($b['checksum64'] -ne 'e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855')
}
$key1 = [pscustomobject]@{ DisplayName = 'FoxSDR 9.8.7'; UninstallString = '"C:\Program Files\FoxSDR\unins000.exe"' }
$key2 = [pscustomobject]@{ DisplayName = 'FoxSDR 9.8.6 (other scope)'; UninstallString = '"C:\Users\x\AppData\Local\Programs\FoxSDR\unins000.exe"' }
$u1 = Invoke-Stub $uninstallPath @($key1)
Check 'uninstall: looks the program up as FoxSDR*' ($u1.SoftwareName -eq 'FoxSDR*')
Check 'uninstall (one match): exactly one Uninstall-ChocolateyPackage call' (@($u1.Calls).Count -eq 1)
if (@($u1.Calls).Count -eq 1) {
    $ub = @($u1.Calls)[0].Bound
    Check 'uninstall (one match): file is the unquoted unins000.exe path' ($ub['File'] -eq 'C:\Program Files\FoxSDR\unins000.exe')
    Check 'uninstall (one match): silent args, no /CLEANDATA' ($ub['SilentArgs'] -eq '/VERYSILENT /SUPPRESSMSGBOXES /NORESTART' -and $ub['SilentArgs'] -notmatch 'CLEANDATA')
    Check 'uninstall (one match): fileType exe' ($ub['FileType'] -eq 'exe')
}
$u0 = Invoke-Stub $uninstallPath @()
Check 'uninstall (no match): nothing is uninstalled and a warning is given' (@($u0.Calls).Count -eq 0 -and @($u0.Warnings).Count -ge 1)
$u2 = Invoke-Stub $uninstallPath @($key1, $key2)
Check 'uninstall (two matches): nothing is uninstalled, both are named in warnings' (@($u2.Calls).Count -eq 0 -and (($u2.Warnings -join "`n") -match 'FoxSDR 9\.8\.7') -and (($u2.Warnings -join "`n") -match 'other scope'))

# ---------------------------------------------------------------------------------------------------------------------
# 5. Refusals: each exits 1, says why, and writes nothing
# ---------------------------------------------------------------------------------------------------------------------
function Refusal([string] $Name, [string[]] $Arguments, [string] $Pattern, [string] $OutDirToCheck) {
    $r = Invoke-Generator ($Arguments + $extra)
    Check ('refused: ' + $Name + ' (exit 1)') ($r.Exit -eq 1) ('exit ' + $r.Exit + ' ' + $r.Err)
    Check ('refused: ' + $Name + ' (says why)') ($r.Err -match $Pattern) $r.Err
    if ($OutDirToCheck -ne '') { Check ('refused: ' + $Name + ' (nothing written)') (-not (Test-Path -LiteralPath $OutDirToCheck)) }
}
$o = Join-Path $ScratchRoot 'out-refused'
Refusal 'missing setup file' @('-SetupFile', (Join-Path $ScratchRoot 'foxsdr-setup-1.2.3.exe'), '-OutDir', $o) 'does not exist' $o
$empty = Join-Path $ScratchRoot 'empty'; New-Item -ItemType Directory -Force $empty | Out-Null
[IO.File]::WriteAllBytes((Join-Path $empty 'foxsdr-setup-1.2.3.exe'), [byte[]]@())
Refusal 'empty setup file' @('-SetupFile', (Join-Path $empty 'foxsdr-setup-1.2.3.exe'), '-OutDir', $o) 'is empty' $o
foreach ($badName in @('setup.exe', 'foxsdr-setup-1.2.exe', 'foxsdr-setup-1.2.3.msi', 'FoxSDR-1.2.3-x64.msix', 'foxsdr-setup-1.2.3-nightly.exe', 'foxsdr-setup-v1.2.3.exe')) {
    $d = Join-Path $ScratchRoot ('badname-' + [guid]::NewGuid().ToString('N')); New-Item -ItemType Directory -Force $d | Out-Null
    [IO.File]::WriteAllBytes((Join-Path $d $badName), $fakeBytes)
    Refusal ('file name ' + $badName) @('-SetupFile', (Join-Path $d $badName), '-OutDir', $o) 'release naming rule' $o
}
Refusal 'version that disagrees with the file name' @('-SetupFile', $setup, '-OutDir', $o, '-Version', '9.8.8') 'does not match' $o
Refusal 'malformed -Version' @('-SetupFile', $setup, '-OutDir', $o, '-Version', '9.8') 'x\.y\.z|does not match' $o
Refusal 'repository URL that is not https' @('-SetupFile', $setup, '-OutDir', $o, '-RepoUrl', 'http://example.com/a/b') 'RepoUrl' $o
Refusal 'repository URL with a placeholder-looking part' @('-SetupFile', $setup, '-OutDir', $o, '-RepoUrl', 'https://example.com/{{a}}/b') 'RepoUrl' $o
Refusal 'application exe that is not a file name' @('-SetupFile', $setup, '-OutDir', $o, '-AppExe', '..\evil.exe') 'AppExe' $o
Refusal 'output folder that already has files' @('-SetupFile', $setup, '-OutDir', $outPwsh) 'not empty' ''
$beforeCount = @(Get-ChildItem -LiteralPath $outPwsh -Recurse -File).Count
Check 'refusal to overwrite left the earlier output as it was' ($beforeCount -eq 7)
$rf = Invoke-Generator (@('-SetupFile', $setup, '-OutDir', $outPwsh, '-Force') + $extra)
Check '-Force writes into a non-empty output folder' ($rf.Exit -eq 0)

# a template that names a placeholder the generator does not know, and one with a stray brace pair: nothing is written
$tr = Join-Path $ScratchRoot 'templates-bogus'
Copy-Item -Recurse -LiteralPath $TemplateRoot -Destination $tr
$bogus = Join-Path $tr 'chocolatey\tools\chocolateyUninstall.ps1.template'
Add-Content -LiteralPath $bogus -Value '# {{Bogus}}'
$ob = Join-Path $ScratchRoot 'out-bogus'
$rb = Invoke-Generator @('-SetupFile', $setup, '-OutDir', $ob, '-TemplateRoot', $tr)
Check 'unknown placeholder in a template: exit 1' ($rb.Exit -eq 1)
Check 'unknown placeholder in a template: names it' ($rb.Err -match 'Bogus')
Check 'unknown placeholder in a template: nothing written (all files rendered before any is written)' (-not (Test-Path -LiteralPath $ob))
$tr2 = Join-Path $ScratchRoot 'templates-brace'
Copy-Item -Recurse -LiteralPath $TemplateRoot -Destination $tr2
Add-Content -LiteralPath (Join-Path $tr2 'chocolatey\tools\chocolateyUninstall.ps1.template') -Value '# }} stray'
$ob2 = Join-Path $ScratchRoot 'out-brace'
$rb2 = Invoke-Generator @('-SetupFile', $setup, '-OutDir', $ob2, '-TemplateRoot', $tr2)
Check 'stray brace pair in a template: exit 1 and nothing written' ($rb2.Exit -eq 1 -and -not (Test-Path -LiteralPath $ob2))
$tr3 = Join-Path $ScratchRoot 'templates-missing'
Copy-Item -Recurse -LiteralPath $TemplateRoot -Destination $tr3
Remove-Item -LiteralPath (Join-Path $tr3 'scoop\foxsdr.json.template')
$ob3 = Join-Path $ScratchRoot 'out-missing'
$rb3 = Invoke-Generator @('-SetupFile', $setup, '-OutDir', $ob3, '-TemplateRoot', $tr3)
Check 'missing template: exit 1 and nothing written' ($rb3.Exit -eq 1 -and $rb3.Err -match 'template missing' -and -not (Test-Path -LiteralPath $ob3))

# a different version/app exe/repository: the values really are parameters
$s2dir = Join-Path $ScratchRoot 's2'; New-Item -ItemType Directory -Force $s2dir | Out-Null
$setup2 = Join-Path $s2dir 'foxsdr-setup-0.99.109.exe'
[IO.File]::WriteAllBytes($setup2, [byte[]](255..100))
$o2 = Join-Path $ScratchRoot 'out-2'
$r3 = Invoke-Generator (@('-SetupFile', $setup2, '-OutDir', $o2, '-AppExe', 'cascade.exe', '-RepoUrl', 'https://github.com/example-owner/example-repo') + $extra)
Check 'second run (other version, exe, repository) exits 0' ($r3.Exit -eq 0) $r3.Err
if ($r3.Exit -eq 0) {
    $h2 = (Get-FileHash -LiteralPath $setup2 -Algorithm SHA256).Hash
    $sc2 = Get-Content -Raw (Join-Path $o2 'scoop\foxsdr.json') | ConvertFrom-Json
    Check 'second run: version, URL and hash follow the file' ($sc2.version -eq '0.99.109' -and $sc2.architecture.'64bit'.url -eq 'https://github.com/example-owner/example-repo/releases/download/v0.99.109/foxsdr-setup-0.99.109.exe' -and $sc2.architecture.'64bit'.hash -eq $h2.ToLowerInvariant())
    Check 'second run: -AppExe reaches the shortcut' ($sc2.shortcuts[0][0] -eq 'cascade.exe')
    Check 'second run: the hash differs from the first (it is computed, not constant)' ($sc2.architecture.'64bit'.hash -ne $fakeSha)
    $wi2 = Get-Content -Raw (Join-Path $o2 'winget\StevenFox.FoxSDR.installer.yaml')
    Check 'second run: winget hash is the new one, upper case' ($wi2 -cmatch ('InstallerSha256: ' + $h2.ToUpperInvariant()))
}

# ---------------------------------------------------------------------------------------------------------------------
# 6. The generator contacts nothing: a scan of its code (comments removed) for anything that could
# ---------------------------------------------------------------------------------------------------------------------
$gen = Read-Ast $GeneratorPath
Check 'generator parses with no errors' ($gen.Errors.Count -eq 0)
$genCmds = @($gen.Ast.FindAll({ param($n) $n -is [System.Management.Automation.Language.CommandAst] }, $true) | ForEach-Object { $_.GetCommandName() } | Where-Object { $_ })
$forbidden = @('Invoke-WebRequest', 'Invoke-RestMethod', 'iwr', 'irm', 'curl', 'wget', 'Start-BitsTransfer', 'git', 'gh', 'ssh', 'scp', 'sftp',
    'signtool', 'certutil', 'certreq', 'Start-Process', 'Invoke-Expression', 'iex', 'New-SelfSignedCertificate', 'Set-AuthenticodeSignature', 'choco', 'scoop')
foreach ($f in $forbidden) { Check ('generator calls no ' + $f) ($genCmds -notcontains $f) }
$genNoComments = (($gen.Ast.Extent.Text -split "`n") | Where-Object { $_.TrimStart() -notmatch '^#' }) -join "`n"
foreach ($f in @('System.Net', 'WebClient', 'HttpClient', 'Sockets', 'Cert:', 'X509')) { Check ('generator code has no reference to ' + $f) (-not $genNoComments.Contains($f)) }
# the only external program it may run is winget, read-only (validate), and only with -RunWingetValidate
Check 'generator runs winget only as validate --manifest' ((([regex]::Matches($genNoComments, '(?i)winget')).Count -gt 0) -and $genNoComments -match 'validate --manifest')
$wingetCalls = @([regex]::Matches($genNoComments, '&\s*\$winget\.Source\s+(\S+)') | ForEach-Object { $_.Groups[1].Value })
Check 'generator runs winget in exactly one place' ($wingetCalls.Count -eq 1) ($wingetCalls -join ',')
Check 'generator never runs winget with anything but validate (no install, upgrade, source, import)' (@($wingetCalls | Where-Object { $_ -ne 'validate' }).Count -eq 0)

# ---------------------------------------------------------------------------------------------------------------------
# 7. The templates and the generator agree; the naming rule is the release script's
# ---------------------------------------------------------------------------------------------------------------------
$known = @([regex]::Matches(([regex]::Match($gen.Ast.Extent.Text, '(?s)\$values\s*=\s*@\{(.*?)\n\}')).Groups[1].Value, "'([A-Za-z0-9_]+)'\s*=") | ForEach-Object { $_.Groups[1].Value })
Check 'generator defines its placeholder table' ($known.Count -ge 5) ($known -join ',')
$used = @()
foreach ($t in (Get-ChildItem -LiteralPath $TemplateRoot -Recurse -File -Filter '*.template')) {
    foreach ($m in [regex]::Matches((Get-Content -Raw $t.FullName), '\{\{([A-Za-z0-9_]+)\}\}')) { $used += $m.Groups[1].Value }
}
$used = @($used | Sort-Object -Unique)
Check 'every placeholder in the templates is one the generator fills' (@($used | Where-Object { $known -notcontains $_ }).Count -eq 0) (($used | Where-Object { $known -notcontains $_ }) -join ',')
Check 'every placeholder the generator fills is used by some template' (@($known | Where-Object { $used -notcontains $_ }).Count -eq 0) (($known | Where-Object { $used -notcontains $_ }) -join ',')
Check 'seven templates exist' (@(Get-ChildItem -LiteralPath $TemplateRoot -Recurse -File -Filter '*.template').Count -eq 7)
$rel = Get-Content -Raw (Join-Path $repo 'tools\release.ps1')
Check 'tools\release.ps1 still names the setup foxsdr-setup-<version>.exe' ($rel.Contains("'foxsdr-setup-' + `$Version + '.exe'"))
Check 'tools\release.ps1 still tags releases v<version>' ($rel.Contains("Tag = ('v' + `$version)"))
$iss = Get-Content -Raw (Join-Path $repo 'installer\cascade.iss')
Check 'installer\cascade.iss still names its output foxsdr-setup-<AppVersion>' ($iss.Contains('OutputBaseFilename=foxsdr-setup-{#AppVersion}'))
Check 'installer\cascade.iss still uses UninstallDisplayName "<AppName> <AppVersion>" (the winget entry and the Chocolatey lookup rely on it)' ($iss.Contains('UninstallDisplayName={#AppName} {#AppVersion}'))
Check 'installer\cascade.iss default application exe is foxsdr-slint.exe (the generator''s -AppExe default)' ($iss -match '(?s)#if SlintOn\s+#define AppExe "foxsdr-slint\.exe"')
Check 'installer\cascade.iss installs cascade.exe (the classic-window shortcut)' ($iss.Contains('Source: "{#BuildDir}\cascade.exe"'))
Check 'installer\cascade.iss keeps plugins beside the exe, which is what Scoop persists' ($iss.Contains('{app}\plugins'))
$genDefaults = Get-Content -Raw $GeneratorPath
Check 'generator default -AppExe is foxsdr-slint.exe' ($genDefaults.Contains("[string] `$AppExe = 'foxsdr-slint.exe'"))
$readme = Get-Content -Raw (Join-Path $repo 'README.md')
$relDoc = Get-Content -Raw (Join-Path $repo 'docs\RELEASING.md')
Check 'generator default -RepoUrl is the repository README.md links its releases to, and docs\RELEASING.md names' ($genDefaults.Contains("'https://github.com/wonderingStars/foxsdr'") -and $readme.Contains('https://github.com/wonderingStars/foxsdr/releases/latest') -and $relDoc.Contains('"repo": "wonderingStars/foxsdr"'))

# ---------------------------------------------------------------------------------------------------------------------
Remove-Item -LiteralPath $ScratchRoot -Recurse -Force -ErrorAction SilentlyContinue
Write-Host ($script:checks.ToString() + ' checks, ' + $script:failed.ToString() + ' failed')
if ($script:failed -gt 0) { exit 1 }
exit 0
