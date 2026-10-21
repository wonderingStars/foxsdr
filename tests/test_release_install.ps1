# Fake-only regression suite: definitions are AST-extracted, never release main.
param(
    [string]$SourcePath = (Join-Path $PSScriptRoot '..\tools\release.ps1'),
    [string[]]$Cases = @()
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0
$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$runRoot = Join-Path $repoRoot ('build\release-install-test-fix\fake-' + $PID + '-' + [guid]::NewGuid().ToString('N'))
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
function Get-StepInfo { return @{exeSha256 = 'fake-hash'; dllSha256 = 'fake-hash'} }
function Get-Sha256 { return 'fake-hash' }
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
function Invoke-Planned { param($What, $File, $Arguments, $TimeoutSec)
    if ($What -ne 'install') { throw 'unexpected planned operation' }
    $script:InstallCalls++
    $prior = Join-Path $script:InstallDir 'prior.txt'
    if (Test-Path -LiteralPath $prior) { Remove-Item -LiteralPath $prior }
    $script:Payload = @()
    foreach ($rel in @('cascade.exe', 'SoapySDR.dll', 'msvcp140.dll', 'vcruntime140.dll', 'vcruntime140_1.dll', 'LICENSE.txt', 'THIRD-PARTY-LICENSES.txt', 'POSTINSTALL.txt', 'unins000.exe', 'unins000.dat', 'resources\bandplans\fake.json', 'plugins\shipped\payload.dll')) {
        $script:Payload += Join-Path $script:InstallDir $rel
    }
    foreach ($rel in @('StartMenu\FoxSDR.lnk', 'StartMenu\Hardware setup notes.lnk', 'StartMenu\Uninstall FoxSDR.lnk', 'OneDrive Desktop\FoxSDR.lnk')) {
        $script:Payload += Join-Path $script:CaseRoot $rel
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
                ($script:CaseName -eq 'leftover-shortcut' -and $p -like '*OneDrive Desktop\FoxSDR.lnk')
        if (-not $keep) { Remove-Item -LiteralPath $p -Force }
    }
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
$allCases = @('plugins-data', 'async-clean', 'async-registry', 'absent-root', 'empty-root', 'leftover-exe', 'logged-plugin', 'leftover-dat', 'leftover-uninstaller', 'leftover-shortcut', 'unknown-file', 'hidden-file', 'unexpected-dir', 'plugins-file', 'plugins-junction', 'retained-hkcu', 'retained-hklm', 'retained-wow', 'retained-handler', 'retained-machine-handler', 'missing-log', 'empty-log', 'no-destinations', 'no-success', 'malformed-log', 'relative-log', 'replace-both', 'replace-directory', 'replace-registry', 'refuse-directory', 'refuse-registry', 'uninstall-error', 'dry-run')
if ($Cases.Count) {
    $selected = @($Cases | ForEach-Object { $_ -split ',' })
    $allCases = @($allCases | Where-Object { $selected -contains $_ })
}
if (-not $allCases.Count) { throw 'no selected cases' }
$originalLocal = $env:LOCALAPPDATA
$failed = 0
try {
    foreach ($name in $allCases) {
        $script:CaseName = $name
        $script:CaseRoot = Join-Path $runRoot $name
        $env:LOCALAPPDATA = Join-Path $script:CaseRoot 'Local'
        $script:InstallDir = Join-Path $env:LOCALAPPDATA 'Programs\FoxSDR'
        $script:RC = @{ Version = '0.99.79'; Head = 'fake-head'; Repo = (Join-Path $script:CaseRoot 'repo'); Dist = (Join-Path $script:CaseRoot 'dist'); InstallerName = 'fake-setup.exe'; Scratch = (Join-Path $script:CaseRoot 'scratch'); Verify = (Join-Path $script:CaseRoot 'verify') }
        New-Item -ItemType Directory -Path $script:RC.Verify -Force | Out-Null
        Put-FakeFile (Join-Path $script:RC.Repo 'resources\bandplans\fake.json')
        $script:Registry = @{}
        $script:UninstallKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\{B3D4A7E2-6C51-4F8B-9A0D-2E7F5C8B1A64}_is1'
        $script:Ctx = @{ DryRun = ($name -eq 'dry-run'); LogPath = ''; Secrets = (New-Object Collections.ArrayList) }
        $script:Messages = New-Object Collections.ArrayList
        $script:InstallCalls = 0; $script:UninstallCalls = 0; $script:Polls = 0
        $script:Clock = [datetime]'2026-10-14T00:00:00'; $script:Pending = $false
        $script:OldRootWait = $false; $script:Done = $false
        $Force = $true; $KeepInstalled = $false
        $AllowReplaceInstalled = $name -like 'replace-*'
        if ($name -in @('replace-both', 'replace-directory', 'refuse-directory')) { Put-FakeFile (Join-Path $script:InstallDir 'prior.txt') }
        if ($name -in @('replace-both', 'replace-registry', 'refuse-registry')) { $script:Registry[$script:UninstallKey] = $true }
        $errorText = ''
        try { $result = Step-InstallTest } catch { $errorText = $_.Exception.Message }
        $wantPass = $name -in @('plugins-data', 'async-clean', 'async-registry', 'absent-root', 'empty-root', 'replace-both', 'replace-directory', 'replace-registry')
        $problem = ''
        if ($wantPass -and ($errorText -ne '' -or -not $script:Done)) { $problem = 'expected done, got ' + $errorText }
        if (-not $wantPass -and ($errorText -notlike 'RELEASE-FAIL:*' -or $script:Done)) { $problem = 'expected release refusal, got ' + $errorText }
        if ($name -eq 'dry-run') {
            if ($errorText -or $result -ne 'done' -or $script:InstallCalls -ne 0 -or $script:UninstallCalls -ne 0 -or $script:Done -or $script:Registry.Count -ne 0 -or (Test-Path -LiteralPath $env:LOCALAPPDATA) -or (Test-Path -LiteralPath $script:RC.Scratch) -or @(Get-ChildItem -LiteralPath $script:RC.Verify -Force).Count -ne 0) { $problem = 'dry-run changed scratch user paths/state or invoked an installer' } else { $problem = '' }
        } elseif ($name -like 'refuse-*') {
            if ($script:InstallCalls -ne 0) { $problem = 'refusal ran installer' }
        } elseif ($script:InstallCalls -ne 1 -or $script:UninstallCalls -ne 1) { $problem = 'install/uninstall fake was not executed exactly once' }
        $expectedRejection = switch ($name) {
            { $_ -in @('missing-log', 'empty-log', 'no-destinations', 'no-success', 'malformed-log', 'relative-log') } { 'install log valid' }
            { $_ -in @('leftover-exe', 'logged-plugin', 'leftover-dat', 'leftover-uninstaller', 'leftover-shortcut') } { 'installed files gone after uninstall' }
            { $_ -in @('unknown-file', 'hidden-file', 'unexpected-dir', 'plugins-file', 'plugins-junction') } { 'only user plugins remain under install directory' }
            { $_ -in @('retained-hkcu', 'retained-hklm', 'retained-wow') } { 'uninstall registry keys gone' }
            { $_ -in @('retained-handler', 'retained-machine-handler') } { 'foxsdr: protocol keys gone' }
            { $_ -like 'refuse-*' } { 'no FoxSDR installed before the test' }
            'uninstall-error' { 'uninstaller exit code' }
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
        if ($problem) {
            $failed++; Write-Host ('FAIL ' + $name + ': ' + $problem)
            Write-Host ($script:Messages -join "`n")
        } else { Write-Host ('PASS ' + $name + $(if ($errorText) { ': ' + $errorText } else { '' })) }
    }
} finally { $env:LOCALAPPDATA = $originalLocal }
Write-Host ('release install fakes: ' + $allCases.Count + ' cases, ' + $failed + ' failed; PowerShell ' + $PSVersionTable.PSVersion + '; scratch ' + $runRoot)
if ($failed) { exit 1 }
exit 0
