# Fake-only regression suite: definitions are AST-extracted, never release main.
param(
    [string]$SourcePath = (Join-Path $PSScriptRoot '..\tools\release.ps1'),
    [string[]]$Cases = @(),
    # Where the fake trees are made (default: the ignored build\release-install-test-fix).
    [string]$ScratchRoot = (Join-Path $PSScriptRoot '..\build\release-install-test-fix')
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0
$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$runRoot = Join-Path ([IO.Path]::GetFullPath($ScratchRoot)) ('fake-' + $PID + '-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $runRoot -Force | Out-Null
foreach ($source in @((Join-Path $repoRoot 'tools\release-common.ps1'), $SourcePath)) {
    $tokens = $null; $errors = $null
    $ast = [Management.Automation.Language.Parser]::ParseFile($source, [ref]$tokens, [ref]$errors)
    if ($errors.Count) { throw ($errors | Out-String) }
    foreach ($def in $ast.FindAll({param($n) $n -is [Management.Automation.Language.FunctionDefinitionAst]}, $false)) {
        . ([scriptblock]::Create($def.Extent.Text))
    }
}
# All side effects of Step-InstallTest stop at these seams. No real registry,
# installer, executable, network or release state is used.
function Write-Result { param($Level, $Name, $Value = '')
    [void]$script:Messages.Add(($Level + ' ' + $Name + ' : ' + $Value))
}
function Write-Detail { param($Text) [void]$script:Messages.Add([string]$Text) }
function Test-StepDone { return $false }
function Assert-StepDone {}
function Set-StepDone { $script:Done = $true }
function Get-StepInfo {
    $info = @{exeSha256 = 'fake-hash'; dllSha256 = 'fake-hash'; softglOpengl32Sha256 = 'fake-hash'; softglGalliumSha256 = 'fake-hash'; slintExeSha256 = 'fake-hash'; slintDllSha256 = 'fake-hash'}
    # A build state written before the Slint hashes were recorded.
    if ($script:CaseName -eq 'slint-no-build-sha') { $info.Remove('slintDllSha256') }
    return $info
}
function Get-Sha256 { param([string]$Path)
    $leaf = [IO.Path]::GetFileName($Path)
    if ($script:CaseName -eq 'slint-sha-exe' -and $leaf -eq 'foxsdr-slint.exe') { return 'other-hash' }
    if ($script:CaseName -eq 'slint-sha-dll' -and $leaf -eq 'slint_cpp.dll') { return 'other-hash' }
    return 'fake-hash'
}
function Find-Iscc { return 'C:\fake\ISCC.exe' }
function Get-PeSubsystem { return 'Windows (fake)' }
function Test-Path { param([string]$LiteralPath, [string]$Path, $PathType, $ErrorAction)
    $p = $LiteralPath; if (-not $p) { $p = $Path }
    if ($p -match '^HK(CU|LM):') { return $script:Registry.ContainsKey($p) }
    if ($PathType) { return Microsoft.PowerShell.Management\Test-Path -LiteralPath $p -PathType $PathType }
    return Microsoft.PowerShell.Management\Test-Path -LiteralPath $p
}
function Get-ItemProperty { param($LiteralPath) return [pscustomobject]@{DisplayVersion = '0.99.79'} }
function Get-Date {
    # Accelerates the real polling deadline, without replacing the cleanup policy.
    $script:Clock = $script:Clock.AddSeconds(31)
    return $script:Clock
}
function Start-Sleep { param($Milliseconds)
    $script:Polls++
    if ($script:Pending -and $script:Polls -eq 2) {
        if ($script:CaseName -eq 'async-registry') { $script:Registry.Clear(); $script:Pending = $false } else { Complete-FakeUninstall }
    }
}
function Wait-ForGone { param($Path, $TimeoutSec)
    # Baseline seam only: the old step waits for the root, even with valid data.
    $script:OldRootWait = $true
    if ($script:Pending) { Complete-FakeUninstall }
    if (Test-Path -LiteralPath $Path) { return -1.0 }
    return 0.0
}
function Put-FakeFile([string]$Path) {
    New-Item -ItemType Directory -Path ([IO.Path]::GetDirectoryName($Path)) -Force | Out-Null
    [IO.File]::WriteAllText($Path, 'fake', (Get-Utf8NoBom))
}
function Invoke-Planned { param($What, $File, $Arguments, $TimeoutSec, $WorkDir)
    if ($What -eq 'iscc') {
        # Step-Installer is only ever driven under -DryRun here: record its switches, run nothing.
        $script:IsccArgs = @($Arguments)
        return $null
    }
    if ($What -ne 'install') { throw 'unexpected planned operation' }
    $script:InstallCalls++
    $prior = Join-Path $script:InstallDir 'prior.txt'
    if (Test-Path -LiteralPath $prior) { Remove-Item -LiteralPath $prior }
    $script:Payload = @()
    foreach ($rel in @('cascade.exe', 'SoapySDR.dll', 'softgl\opengl32.dll', 'softgl\libgallium_wgl.dll', 'msvcp140.dll', 'vcruntime140.dll', 'vcruntime140_1.dll', 'LICENSE.txt', 'THIRD-PARTY-LICENSES.txt', 'POSTINSTALL.txt', 'unins000.exe', 'unins000.dat', 'resources\bandplans\fake.json', 'plugins\shipped\payload.dll')) {
        $script:Payload += Join-Path $script:InstallDir $rel
    }
    foreach ($rel in @('StartMenu\FoxSDR.lnk', 'StartMenu\Hardware setup notes.lnk', 'StartMenu\Uninstall FoxSDR.lnk', 'OneDrive Desktop\FoxSDR.lnk')) {
        $script:Payload += Join-Path $script:CaseRoot $rel
    }
    # The Slint window as the application (installer\cascade.iss, SlintIsApp): two more files in {app} and the classic window's own
    # Start-menu entry. The classic installer has none of them; the classic-slint-* cases put them there anyway.
    $slintFiles = @((Join-Path $script:InstallDir 'foxsdr-slint.exe'), (Join-Path $script:InstallDir 'slint_cpp.dll'))
    $classicLnk = Join-Path $script:CaseRoot 'StartMenu\FoxSDR (classic window).lnk'
    $script:Unlogged = @()
    if (-not $ClassicInstaller) {
        if ($script:CaseName -ne 'slint-no-exe') { $script:Payload += $slintFiles[0] }
        if ($script:CaseName -ne 'slint-no-dll') { $script:Payload += $slintFiles[1] }
        if ($script:CaseName -ne 'slint-no-classic-shortcut') { $script:Payload += $classicLnk }
    } elseif ($script:CaseName -eq 'classic-slint-logged') {
        $script:Payload += $slintFiles
        $script:Payload += $classicLnk
    } elseif ($script:CaseName -eq 'classic-slint-on-disk') {
        $script:Unlogged = $slintFiles
        foreach ($p in $script:Unlogged) { Put-FakeFile $p }
    }
    foreach ($p in $script:Payload) { Put-FakeFile $p }
    $script:Registry[$script:UninstallKey] = $true
    $script:Registry['HKCU:\Software\Classes\foxsdr'] = $true
    $log = @('2026-10-14 05:41:40.100   Creating new uninstall log: ' + (Join-Path $script:InstallDir 'unins000.dat'))
    foreach ($p in $script:Payload | Where-Object { $_ -notlike '*unins000.dat' }) { $log += '2026-10-14 05:41:40.101   Dest filename: ' + $p }
    # Duplicate destinations, timestamps and spaces match the real Inno log.
    $log += '2026-10-14 05:41:40.102   Dest filename: ' + $script:Payload[0]
    $log += '2026-10-14 05:41:41.000   Installation process succeeded.'
    switch ($script:CaseName) {
        'empty-log' { $log = @() }
        'no-destinations' { $log = @('2026-10-14 05:41:41.000   Installation process succeeded.') }
        'no-success' { $log = @($log | Where-Object { $_ -notlike '*Installation process succeeded.*' }) }
        'malformed-log' { $log += '2026-10-14 05:41:40.103   Dest filename: ' }
        'relative-log' { $log += '2026-10-14 05:41:40.103   Dest filename: relative\file.dll' }
    }
    if ($script:CaseName -ne 'missing-log') {
        [IO.File]::WriteAllText((Join-Path $script:RC.Verify 'install.log'), ($log -join "`r`n"), (Get-Utf8NoBom))
    }
    return @{ExitCode = 0}
}
function Invoke-Exe { param($File, $Arguments, $TimeoutSec, $Env, $WorkDir)
    [void]$script:ExeCalls.Add([pscustomobject]@{File = $File; Args = @($Arguments); Env = $Env; WorkDir = $WorkDir})
    if ($File -like '*unins000.exe') {
        $script:UninstallCalls++
        $script:Pending = $true
        if ($script:CaseName -ne 'async-clean') { Complete-FakeUninstall }
        if ($script:CaseName -eq 'async-registry') {
            $script:Registry[$script:UninstallKey] = $true
            $script:Registry['HKCU:\Software\Classes\foxsdr'] = $true
            $script:Pending = $true
        }
        return @{ExitCode = $(if ($script:CaseName -eq 'uninstall-error') { 1 } else { 0 })}
    }
    if ($File -like '*foxsdr-slint.exe') {
        # The Slint window: --version as cascade.exe; --frames N writes its evidence to stderr (the helper joins it into Output).
        switch ($Arguments[0]) {
            '--version' { $out = $(if ($script:CaseName -eq 'slint-version-bad') { 'FoxSDR 0.99.78' } else { 'FoxSDR 0.99.79' }) }
            '--frames' {
                $lang = 'en (english)'
                if ($Env -and $Env.ContainsKey('FOXSDR_LANGUAGE') -and $Env['FOXSDR_LANGUAGE'] -eq 'fr') { $lang = 'fr (bundled)' }
                if ($script:CaseName -eq 'slint-language-unbundled') { $lang = 'fr (not bundled)' }
                $ticks = '60 of 60'
                if ($script:CaseName -eq 'slint-frames-ticks') { $ticks = '12 of 60' }
                $out = 'foxsdr-slint: language ' + $lang + "`n" + 'foxsdr-slint: bounded run: ' + $ticks + ' ticks, exit 0'
                if ($script:CaseName -eq 'slint-language-missing') { $out = 'foxsdr-slint: bounded run: 60 of 60 ticks, exit 0' }
                return @{ExitCode = $(if ($script:CaseName -eq 'slint-frames-exit') { 3 } else { 0 }); StdOut = ''; Output = $out; TimedOut = $false}
            }
            default { throw 'unexpected foxsdr-slint call' }
        }
        return @{ExitCode = 0; StdOut = $out; Output = $out; TimedOut = $false}
    }
    switch ($Arguments[0]) {
        '--version' { $out = 'FoxSDR 0.99.79' }
        '--selftest' { $out = 'selftest PASS' }
        '--frames' { $out = 'rendered 60 frames' }
        default { throw 'unexpected executable call' }
    }
    return @{ExitCode = 0; StdOut = $out; Output = $out; TimedOut = $false}
}
function Complete-FakeUninstall {
    $script:Pending = $false
    foreach ($p in $script:Payload) {
        $keep = ($script:CaseName -eq 'leftover-exe' -and $p -eq (Join-Path $script:InstallDir 'cascade.exe')) -or
                ($script:CaseName -eq 'logged-plugin' -and $p -like '*plugins\shipped\payload.dll') -or
                ($script:CaseName -eq 'leftover-dat' -and $p -like '*unins000.dat') -or
                ($script:CaseName -eq 'leftover-uninstaller' -and $p -like '*unins000.exe') -or
                ($script:CaseName -eq 'leftover-shortcut' -and $p -like '*OneDrive Desktop\FoxSDR.lnk') -or
                ($script:CaseName -eq 'slint-leftover-exe' -and $p -like '*foxsdr-slint.exe') -or
                ($script:CaseName -eq 'slint-leftover-dll' -and $p -like '*slint_cpp.dll') -or
                ($script:CaseName -eq 'slint-leftover-classic-shortcut' -and $p -like '*(classic window).lnk')
        if (-not $keep) { Remove-Item -LiteralPath $p -Force }
    }
    foreach ($p in $script:Unlogged) { if (Test-Path -LiteralPath $p) { Remove-Item -LiteralPath $p -Force } }
    # Remove empty payload directories; all paths came from this unique scratch tree.
    Get-ChildItem -LiteralPath $script:InstallDir -Directory -Recurse | Sort-Object { $_.FullName.Length } -Descending | ForEach-Object {
        if (@(Get-ChildItem -LiteralPath $_.FullName -Force).Count -eq 0) { Remove-Item -LiteralPath $_.FullName }
    }
    $script:Registry.Clear()
    switch ($script:CaseName) {
        'retained-hkcu' { $script:Registry[$script:UninstallKey] = $true }
        'retained-hklm' { $script:Registry[$script:UninstallKey.Replace('HKCU:', 'HKLM:')] = $true }
        'retained-wow' { $script:Registry[$script:UninstallKey.Replace('Software\Microsoft', 'Software\WOW6432Node\Microsoft')] = $true }
        'retained-handler' { $script:Registry['HKCU:\Software\Classes\foxsdr'] = $true }
        'retained-machine-handler' { $script:Registry['HKLM:\Software\Classes\foxsdr'] = $true }
        'unknown-file' { Put-FakeFile (Join-Path $script:InstallDir 'unknown.txt') }
        'hidden-file' { $p = Join-Path $script:InstallDir 'hidden.txt'; Put-FakeFile $p; [IO.File]::SetAttributes($p, [IO.FileAttributes]::Hidden) }
        'unexpected-dir' { New-Item -ItemType Directory -Path (Join-Path $script:InstallDir 'cache') | Out-Null }
        'plugins-file' { Put-FakeFile (Join-Path $script:InstallDir 'plugins') }
        'plugins-junction' {
            $target = Join-Path $script:CaseRoot 'junction-target'
            New-Item -ItemType Directory -Path $target | Out-Null
            New-Item -ItemType Junction -Path (Join-Path $script:InstallDir 'plugins') -Target $target | Out-Null
        }
    }
    if ($script:CaseName -in @('plugins-data', 'async-clean', 'replace-both', 'replace-directory', 'replace-registry')) {
        Put-FakeFile (Join-Path $script:InstallDir 'plugins\user\manifest.json')
    }
    if (@(Get-ChildItem -LiteralPath $script:InstallDir -Force).Count -eq 0 -and $script:CaseName -ne 'empty-root') { Remove-Item -LiteralPath $script:InstallDir }
}
$allCases = @('plugins-data', 'async-clean', 'async-registry', 'absent-root', 'empty-root', 'leftover-exe', 'logged-plugin', 'leftover-dat', 'leftover-uninstaller', 'leftover-shortcut', 'unknown-file', 'hidden-file', 'unexpected-dir', 'plugins-file', 'plugins-junction', 'retained-hkcu', 'retained-hklm', 'retained-wow', 'retained-handler', 'retained-machine-handler', 'missing-log', 'empty-log', 'no-destinations', 'no-success', 'malformed-log', 'relative-log', 'replace-both', 'replace-directory', 'replace-registry', 'refuse-directory', 'refuse-registry', 'uninstall-error', 'dry-run',
    # the Slint window as the application (the default), and the classic installer asserting the Slint files are absent
    'slint-ok', 'slint-no-exe', 'slint-no-dll', 'slint-no-classic-shortcut', 'slint-sha-exe', 'slint-sha-dll', 'slint-no-build-sha', 'slint-version-bad', 'slint-frames-exit', 'slint-frames-ticks',
    'slint-language-unbundled', 'slint-language-missing', 'slint-leftover-exe', 'slint-leftover-dll', 'slint-leftover-classic-shortcut',
    'classic-slint-logged', 'classic-slint-on-disk', 'dry-run-slint', 'dry-run-classic', 'installer-slint', 'installer-classic', 'build-state')
if ($Cases.Count) {
    $selected = @($Cases | ForEach-Object { $_ -split ',' })
    $allCases = @($allCases | Where-Object { $selected -contains $_ })
}
if (-not $allCases.Count) { throw 'no selected cases' }
$originalLocal = $env:LOCALAPPDATA
$VcCrtDir = 'C:\fake\crt'   # a parameter of release.ps1 that Step-Installer reads
$failed = 0
try {
    foreach ($name in $allCases) {
        $script:CaseName = $name
        $script:CaseRoot = Join-Path $runRoot $name
        $env:LOCALAPPDATA = Join-Path $script:CaseRoot 'Local'
        $script:InstallDir = Join-Path $env:LOCALAPPDATA 'Programs\FoxSDR'
        $script:RC = @{ BuildDir = (Join-Path $script:CaseRoot 'build'); Version = '0.99.79'; Head = 'fake-head'; Repo = (Join-Path $script:CaseRoot 'repo'); Dist = (Join-Path $script:CaseRoot 'dist'); InstallerName = 'fake-setup.exe'; Scratch = (Join-Path $script:CaseRoot 'scratch'); Verify = (Join-Path $script:CaseRoot 'verify') }
        New-Item -ItemType Directory -Path $script:RC.Verify -Force | Out-Null
        Put-FakeFile (Join-Path $script:RC.Repo 'resources\bandplans\fake.json')
        $script:Registry = @{}
        $script:UninstallKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\{B3D4A7E2-6C51-4F8B-9A0D-2E7F5C8B1A64}_is1'
        $script:Ctx = @{ DryRun = ($name -like 'dry-run*' -or $name -like 'installer-*'); LogPath = ''; Secrets = (New-Object Collections.ArrayList) }
        $script:Messages = New-Object Collections.ArrayList
        $script:InstallCalls = 0; $script:UninstallCalls = 0; $script:Polls = 0
        $script:Clock = [datetime]'2026-10-14T00:00:00'; $script:Pending = $false
        $script:OldRootWait = $false; $script:Done = $false
        $script:ExeCalls = New-Object Collections.ArrayList; $script:IsccArgs = @(); $script:Unlogged = @()
        $Force = $true; $KeepInstalled = $false
        # The classic installer is what every case before the Slint window became the application exercised; the slint-* cases
        # (and the dry runs / installer plans named for it) run the default, the Slint application.
        $isSpecial = $name -in @('installer-slint', 'installer-classic', 'build-state')
        $ClassicInstaller = -not ($name -like 'slint-*' -or $name -in @('dry-run-slint', 'installer-slint'))
        $isDry = $name -like 'dry-run*'
        $AllowReplaceInstalled = $name -like 'replace-*'
        if ($name -in @('replace-both', 'replace-directory', 'refuse-directory')) { Put-FakeFile (Join-Path $script:InstallDir 'prior.txt') }
        if ($name -in @('replace-both', 'replace-registry', 'refuse-registry')) { $script:Registry[$script:UninstallKey] = $true }
        $errorText = ''
        try {
            if ($name -like 'installer-*') { $result = Step-Installer } elseif ($name -eq 'build-state') { $result = 'done' } else { $result = Step-InstallTest }
        } catch { $errorText = $_.Exception.Message }
        $wantPass = $name -in @('plugins-data', 'async-clean', 'async-registry', 'absent-root', 'empty-root', 'replace-both', 'replace-directory', 'replace-registry', 'slint-ok')
        $problem = ''
        if ($wantPass -and ($errorText -ne '' -or -not $script:Done)) { $problem = 'expected done, got ' + $errorText }
        if (-not $wantPass -and ($errorText -notlike 'RELEASE-FAIL:*' -or $script:Done)) { $problem = 'expected release refusal, got ' + $errorText }
        if ($isSpecial) {
            $problem = ''
            if ($errorText) { $problem = 'unexpected error: ' + $errorText }
            elseif ($name -eq 'build-state') {
                # Step-Build cannot be run against fakes; its state record is held from its text: the Slint dll's hash is recorded beside the exe's.
                $buildText = (Get-Command Step-Build).Definition
                if ($buildText -notmatch 'slintDllSha256\s*=') { $problem = 'Step-Build does not record slintDllSha256 in the build state' }
                elseif ($buildText -notmatch 'slintExeSha256\s*=') { $problem = 'Step-Build does not record slintExeSha256 in the build state' }
            } else {
                $ia = @($script:IsccArgs)
                if ($ia.Count -eq 0) { $problem = 'ISCC was not planned' }
                elseif ($name -eq 'installer-slint') {
                    if ($ia -notcontains '/DSlintIsApp=1') { $problem = 'the default installer is not built with /DSlintIsApp=1: ' + ($ia -join ' ') }
                    elseif (@($ia | Where-Object { $_ -like '/DSlintDir=*build*src*slint*Release' }).Count -ne 1) { $problem = 'the default installer is not given /DSlintDir: ' + ($ia -join ' ') }
                } else {
                    if ($ia -notcontains '/DSlintIsApp=0') { $problem = '-ClassicInstaller does not build with /DSlintIsApp=0: ' + ($ia -join ' ') }
                    elseif ($ia -contains '/DSlintIsApp=1') { $problem = '-ClassicInstaller still passes /DSlintIsApp=1: ' + ($ia -join ' ') }
                }
            }
        } elseif ($isDry) {
            $problem = ''
            $dryText = $script:Messages -join "`n"
            $slintPlans = @('installed foxsdr-slint\.exe and slint_cpp\.dll have the sha256 of the built files', 'installed foxsdr-slint\.exe --version prints "FoxSDR <version>"', 'foxsdr-slint\.exe --frames 60 .*FOXSDR_LANGUAGE=fr.*"60 of 60 ticks".*"language fr \(bundled\)"', '"FoxSDR \(classic window\)"')
            if ($errorText -or $result -ne 'done' -or $script:InstallCalls -ne 0 -or $script:UninstallCalls -ne 0 -or $script:Done -or $script:Registry.Count -ne 0 -or (Test-Path -LiteralPath $env:LOCALAPPDATA) -or (Test-Path -LiteralPath $script:RC.Scratch) -or @(Get-ChildItem -LiteralPath $script:RC.Verify -Force).Count -ne 0) { $problem = 'dry-run changed scratch user paths/state or invoked an installer' }
            elseif ($name -eq 'dry-run-slint') {
                foreach ($pl in $slintPlans) { if ($dryText -notmatch ('(?m)^DRY check : .*' + $pl)) { $problem = 'the plan does not say: ' + $pl } }
                if ($dryText -match 'classic installer') { $problem = 'the Slint plan mentions the classic installer' }
            } elseif ($name -eq 'dry-run-classic') {
                if ($dryText -notmatch '(?m)^DRY check : classic installer \(-ClassicInstaller\): .*no Slint files') { $problem = 'the classic plan does not say no Slint files are installed' }
                foreach ($pl in $slintPlans) { if ($dryText -match $pl) { $problem = 'the classic plan still plans the Slint checks: ' + $pl } }
            }
        } elseif ($name -like 'refuse-*') {
            if ($script:InstallCalls -ne 0) { $problem = 'refusal ran installer' }
        } elseif ($script:InstallCalls -ne 1 -or $script:UninstallCalls -ne 1) { $problem = 'install/uninstall fake was not executed exactly once' }
        $expectedRejection = switch ($name) {
            { $_ -in @('missing-log', 'empty-log', 'no-destinations', 'no-success', 'malformed-log', 'relative-log', 'slint-no-exe', 'slint-no-dll', 'slint-no-classic-shortcut') } { 'install log valid' }
            { $_ -in @('leftover-exe', 'logged-plugin', 'leftover-dat', 'leftover-uninstaller', 'leftover-shortcut', 'slint-leftover-exe', 'slint-leftover-dll', 'slint-leftover-classic-shortcut') } { 'installed files gone after uninstall' }
            { $_ -in @('unknown-file', 'hidden-file', 'unexpected-dir', 'plugins-file', 'plugins-junction') } { 'only user plugins remain under install directory' }
            { $_ -in @('retained-hkcu', 'retained-hklm', 'retained-wow') } { 'uninstall registry keys gone' }
            { $_ -in @('retained-handler', 'retained-machine-handler') } { 'foxsdr: protocol keys gone' }
            { $_ -like 'refuse-*' } { 'no FoxSDR installed before the test' }
            'uninstall-error' { 'uninstaller exit code' }
            'slint-sha-exe' { 'installed foxsdr-slint.exe = built foxsdr-slint.exe' }
            'slint-sha-dll' { 'installed slint_cpp.dll = built slint_cpp.dll' }
            'slint-no-build-sha' { 'build state records the Slint hashes' }
            'slint-version-bad' { 'installed foxsdr-slint.exe --version' }
            'slint-frames-exit' { 'installed foxsdr-slint.exe --frames 60 exit code' }
            'slint-frames-ticks' { 'foxsdr-slint --frames 60 ticks' }
            { $_ -in @('slint-language-unbundled', 'slint-language-missing') } { 'foxsdr-slint language fr bundled' }
            { $_ -in @('classic-slint-logged', 'classic-slint-on-disk') } { 'classic installer installs no Slint files' }
        }
        if ($expectedRejection -and $errorText -ne ('RELEASE-FAIL: ' + $expectedRejection)) { $problem = 'wrong rejection: ' + $errorText + '; expected ' + $expectedRejection }
        if ($name -like 'replace-*' -and (($script:Messages -join "`n") -match 'PASS nothing installed before the test|absent, no uninstall key')) { $problem = 'preexisting install falsely reported absent' }
        if ($name -like 'replace-*' -and (($script:Messages -join "`n") -notmatch 'WARN replacing existing install \(-AllowReplaceInstalled\)')) { $problem = 'replacement authorization not reported' }
        if ($name -eq 'absent-root' -and (($script:Messages -join "`n") -notmatch 'PASS nothing installed before the test')) { $problem = 'absent install not reported absent' }
        if ($name -in @('async-clean', 'async-registry') -and ($script:Polls -ne 2 -or $script:OldRootWait)) { $problem = 'did not poll actual asynchronous payload and registry cleanup' }
        if ($name -eq 'plugins-data' -and ($script:Polls -ne 0 -or $script:OldRootWait)) { $problem = 'waited for valid retained plugins root' }
        if ($name -in @('plugins-data', 'async-clean', 'replace-both', 'replace-directory', 'replace-registry')) {
            $pluginPath = Join-Path $script:InstallDir 'plugins'
            if (($script:Messages -join "`n") -notlike ('*' + $pluginPath + ': user-installed plugins preserved without /CLEANDATA*')) { $problem = 'retained plugin path/reason not reported' }
            $manifest = Join-Path $pluginPath 'user\manifest.json'
            if (-not (Test-Path -LiteralPath $manifest) -or [Convert]::ToBase64String([IO.File]::ReadAllBytes($manifest)) -ne 'ZmFrZQ==') { $problem = 'retained manifest bytes changed' }
        }
        if ($name -in @('empty-root', 'absent-root')) {
            $state = $(if ($name -eq 'empty-root') { 'empty' } else { 'absent' })
            if (($script:Messages -join "`n") -notlike ('*INFO install directory after uninstall : ' + $script:InstallDir + ' ' + $state + '*')) { $problem = 'root state not reported truthfully' }
        }
        if (-not $problem -and $wantPass -and $ClassicInstaller) {
            # The classic installer: cascade.exe is the application, the Slint files are asserted absent and the Slint window is never run.
            if (@($script:ExeCalls | Where-Object { $_.File -like '*foxsdr-slint.exe' }).Count -ne 0) { $problem = 'the classic installer test ran foxsdr-slint.exe' }
            elseif (($script:Messages -join "`n") -notlike '*PASS classic installer installs no Slint files*') { $problem = 'the classic installer test did not assert the Slint files absent' }
        }
        if (-not $problem -and $name -eq 'slint-ok') {
            $msgs = $script:Messages -join "`n"
            foreach ($want in @('PASS installed foxsdr-slint.exe : ', 'PASS installed slint_cpp.dll : ', 'PASS installed foxsdr-slint.exe = built foxsdr-slint.exe', 'PASS installed slint_cpp.dll = built slint_cpp.dll',
                                'PASS installed foxsdr-slint.exe --version', 'PASS installed foxsdr-slint.exe --frames 60 exit code', 'PASS foxsdr-slint --frames 60 ticks', 'PASS foxsdr-slint language fr bundled',
                                'PASS installed cascade.exe = built cascade.exe', 'PASS installed cascade.exe --version', 'PASS installed cascade.exe --selftest')) {
                if ($msgs -notlike ('*' + $want + '*')) { $problem = 'not reported: ' + $want }
            }
            $slintCalls = @($script:ExeCalls | Where-Object { $_.File -like '*foxsdr-slint.exe' })
            $verCalls = @($slintCalls | Where-Object { $_.Args[0] -eq '--version' })
            $frCalls = @($slintCalls | Where-Object { $_.Args[0] -eq '--frames' })
            if ($verCalls.Count -ne 1) { $problem = 'foxsdr-slint.exe --version was run ' + $verCalls.Count + ' times' }
            elseif ($frCalls.Count -ne 1 -or $frCalls[0].Args[1] -ne '60') { $problem = 'foxsdr-slint.exe --frames 60 was not run exactly once' }
            elseif (@($slintCalls | Where-Object { $_.Args[0] -eq '--selftest' }).Count -ne 0) { $problem = 'foxsdr-slint.exe --selftest asserted without being known to exist' }
            else {
                $fe = $frCalls[0].Env
                foreach ($k in @('APPDATA', 'LOCALAPPDATA', 'USERPROFILE', 'HOME', 'CASCADE_CONFIG_TEST')) {
                    if (-not $fe.ContainsKey($k) -or -not ([string]$fe[$k]).StartsWith($script:RC.Scratch)) { $problem = 'the bounded Slint run is not given a scratch ' + $k }
                }
                if (-not $fe.ContainsKey('FOXSDR_LANGUAGE') -or $fe['FOXSDR_LANGUAGE'] -ne 'fr') { $problem = 'the bounded Slint run is not given FOXSDR_LANGUAGE=fr' }
                if (-not $fe.ContainsKey('FOXSDR_TELEMETRY_URL') -or $fe['FOXSDR_TELEMETRY_URL'] -ne 'http://127.0.0.1:9/') { $problem = 'the bounded Slint run can reach the telemetry host' }
            }
            if (@($script:ExeCalls | Where-Object { $_.File -like '*\cascade.exe' }).Count -ne 3) { $problem = 'the cascade.exe checks (--version, --selftest, --frames) did not all stay' }
        }
        if ($problem) {
            $failed++; Write-Host ('FAIL ' + $name + ': ' + $problem)
            Write-Host ($script:Messages -join "`n")
        } else { Write-Host ('PASS ' + $name + $(if ($errorText) { ': ' + $errorText } else { '' })) }
    }
} finally { $env:LOCALAPPDATA = $originalLocal }
Write-Host ('release install fakes: ' + $allCases.Count + ' cases, ' + $failed + ' failed; PowerShell ' + $PSVersionTable.PSVersion + '; scratch ' + $runRoot)
if ($failed) { exit 1 }
exit 0
