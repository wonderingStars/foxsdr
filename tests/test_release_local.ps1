# Fake-only suite for the LOCAL-BUILD release procedure in tools/release.ps1 (owner rule 2026-11-25: build everything
# locally unless GitHub is the better place; docs: the run sheet RELEASE-RUN-LOCAL.md). Three changes are held here:
#
#   gate-hash      a step that records the SHA-256 of every binary the installer and the MSIX are made from (cascade.exe,
#                  foxsdr-engine.exe, SoapySDR.dll, softgl\*, foxsdr-slint.exe, slint_cpp.dll) in <release>\verify\gate-binaries.json,
#                  run AFTER the full gates, in the one build folder the gates ran in.
#   -RequireGate   build (after pass 2), installer (before ISCC) and msix (before makeappx) refuse unless every one of those files
#                  still has the recorded hash, the record is for this commit, and nothing was added to softgl\.
#   -NoCiWait      the ci step does not find or wait for GitHub's run: it writes dist\sha256.txt and the notes hash from the local
#                  installer. A release with Linux files (five hashes) refuses it, because those files come from CI.
#   ci-check       a step that reads the CI run ONCE and never fails: red jobs print a WARN with the rerun command.
#
# Definitions are AST-extracted (as tests/test_release_preflight.ps1 does), never the release script's main body. Only git, the
# tool lookups, the process runner and the CI lookups are recording fakes. Nothing is built, installed, downloaded or sent.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File tests\test_release_local.ps1
#   pwsh -NoProfile -File tests\test_release_local.ps1
#   ... -SourcePath <a copy of release.ps1>     (the break-it check: run it against a mutated or the OLD script)
#   ... -Cases gate-check-ok,ci-nowait-real
param(
    [string]$SourcePath = (Join-Path $PSScriptRoot '..\tools\release.ps1'),
    [string[]]$Cases = @()
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0
$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$runRoot = Join-Path $repoRoot ('build\release-local-test\fake-' + $PID + '-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $runRoot -Force | Out-Null
$srcText = [IO.File]::ReadAllText($SourcePath)
foreach ($source in @((Join-Path $repoRoot 'tools\release-common.ps1'), $SourcePath)) {
    $tokens = $null; $errors = $null
    $ast = [Management.Automation.Language.Parser]::ParseFile($source, [ref]$tokens, [ref]$errors)
    if ($errors.Count) { throw ($errors | Out-String) }
    foreach ($def in $ast.FindAll({param($n) $n -is [Management.Automation.Language.FunctionDefinitionAst]}, $false)) {
        $text = $def.Extent.Text
        if ($def.Name -eq 'Get-ReleaseContext') { $text = $text.Replace('$PSScriptRoot', '$script:FakeScriptRoot') }
        . ([scriptblock]::Create($text))
    }
}

# ---- the seams: nothing real is reached ---------------------------------------------------------------------------
function Write-Result { param($Level, $Name, $Value = '') [void]$script:Messages.Add(($Level + ' ' + $Name + ' : ' + $Value)) }
function Write-Detail { param($Text) [void]$script:Messages.Add([string]$Text) }
function Test-StepDone { return $false }
function Assert-StepDone {}
function Set-StepDone { param($Step, $Outputs, $Info) $script:Done = $Step; $script:DoneOutputs = $Outputs }
function Find-Tool { param($Name, $Candidates) if ($Name -eq 'git') { return 'fake-git' } return '' }
function Find-Iscc { return '' }
function Invoke-Git {
    param([string[]]$GitArgs, [string[]]$Identity = @())
    switch ($GitArgs[0]) {
        'diff' { return @{ ExitCode = 0; StdOut = "+changed`n"; Output = "+changed`n" } }
    }
    return @{ ExitCode = 0; StdOut = ''; Output = '' }
}
function Invoke-Exe {
    param($File, $Arguments, $Env, $WorkDir, $TimeoutSec, $InputFile, $Label, $HeartbeatSec, [switch]$NoLog)
    [void]$script:ExeCalls.Add(($File + ' ' + ($Arguments -join ' ')))
    if ($Arguments -contains 'rev-parse') { return @{ ExitCode = 0; StdOut = "0123456789abcdef0123456789abcdef01234567`n"; Output = ''; TimedOut = $false } }
    return @{ ExitCode = 1; StdOut = ''; Output = 'unexpected process'; TimedOut = $false }
}
function Get-CiRun {
    $script:CiRunLookups++
    if ($script:CiRunMode -eq 'throw') { throw 'RELEASE-FAIL: gh run list failed: fake' }
    if ($script:CiRunMode -eq 'none') { return $null }
    return [pscustomobject]@{ databaseId = 4242; url = 'https://example.invalid/runs/4242' }
}
function Get-CiJobs {
    param($RunId)
    $script:CiJobLookups++
    $windows = 'success'
    if ($script:CiRunMode -eq 'red') { $windows = 'failure' }
    return [pscustomobject]@{ jobs = @(
        [pscustomobject]@{ name = 'windows'; status = 'completed'; conclusion = $windows },
        [pscustomobject]@{ name = 'linux'; status = 'completed'; conclusion = 'success' },
        [pscustomobject]@{ name = 'arm64'; status = 'in_progress'; conclusion = '' }) }
}

$dash = [string][char]0x2014
$version = '0.99.105'
$installer = 'foxsdr-setup-' + $version + '.exe'
$fiveNames = @($installer, ('FoxSDR-' + $version + '-x86_64.AppImage'), ('foxsdr-' + $version + '-linux-x64.tar.gz'), ('FoxSDR-' + $version + '-aarch64.AppImage'), ('foxsdr-' + $version + '-linux-arm64.tar.gz'))
$titleWindows = '# FoxSDR ' + $version + ' ' + $dash + ' Windows'
$titlePlain = '# FoxSDR ' + $version

function New-Blocks([string[]]$Names, [string[]]$Values) {
    $b = @()
    for ($i = 0; $i -lt $Names.Count; $i++) { $b += , @($Names[$i], $Values[$i]) }
    return , $b
}
function New-NotesText([string]$Title, $Blocks, [string]$Eol = "`r`n") {
    $lines = @($Title, '', 'Some release text.', '', '---', '')
    foreach ($b in $Blocks) { $lines += ('**SHA-256 of `' + $b[0] + '`:**'); $lines += ('`' + $b[1] + '`'); $lines += '' }
    return (($lines -join $Eol).TrimEnd("`r", "`n") + $Eol)
}
function Put-Bytes([string]$Path, [string]$Text) {
    New-Item -ItemType Directory -Path ([IO.Path]::GetDirectoryName($Path)) -Force | Out-Null
    [IO.File]::WriteAllText($Path, $Text, (New-Object Text.UTF8Encoding($false)))
}
$gateFiles = @('Release\cascade.exe', 'Release\foxsdr-engine.exe', 'Release\SoapySDR.dll', 'Release\softgl\opengl32.dll', 'Release\softgl\libgallium_wgl.dll', 'src\slint\Release\foxsdr-slint.exe', 'src\slint\Release\slint_cpp.dll')

# A scratch repository at the release version, a build folder holding the seven gated files, and the release context.
function Initialize-Case([string]$CaseRoot, [string]$NotesText) {
    $repo = Join-Path $CaseRoot 'repo'
    Put-Bytes (Join-Path $repo 'CMakeLists.txt') ("project(cascade VERSION " + $version + " LANGUAGES CXX)`r`n")
    Put-Bytes (Join-Path $repo 'README.md') ('The current release is **' + $version + '**.' + "`r`n")
    Put-Bytes (Join-Path $repo ('docs\release-notes-' + $version + '.md')) $NotesText
    New-Item -ItemType Directory -Path (Join-Path $repo 'tools') -Force | Out-Null
    $script:FakeScriptRoot = Join-Path $repo 'tools'
    $script:ReleaseRoot = Join-Path $CaseRoot 'rel'
    $script:BuildDir = Join-Path $CaseRoot 'build'
    foreach ($f in $gateFiles) { Put-Bytes (Join-Path $script:BuildDir $f) ('bytes of ' + $f) }
    $script:Ctx = @{ DryRun = $false; LogPath = ''; Secrets = (New-Object Collections.ArrayList) }
    $script:Cfg = @{ repo = 'fake/repo' }
    $script:Gh = 'fake-gh'
    $script:RC = Get-ReleaseContext
    New-Item -ItemType Directory -Path $script:RC.Verify -Force | Out-Null
    $script:ExeCalls.Clear(); $script:Messages.Clear()
    $script:CiRunLookups = 0; $script:CiJobLookups = 0; $script:CiRunMode = 'ok'
    $script:Done = ''; $script:DoneOutputs = $null
    $script:RequireGate = $false; $script:NoCiWait = $false
    return $script:RC
}
function New-WindowsCase { return (Initialize-Case $caseRoot (New-NotesText $titleWindows (New-Blocks @($installer) @('TBD')))) }
function Has-Message([string]$Like) { return (@($script:Messages | Where-Object { $_ -like $Like }).Count -gt 0) }
function Try-Step([scriptblock]$Body) { try { $null = & $Body; return '' } catch { return $_.Exception.Message } }
function Add-GateRecord { $script:Force = $false; $e = Try-Step { Step-GateHash }; if ($e) { throw ('could not record: ' + $e) } }

$AllowDirty = $false
$Force = $false
$CiRunId = ''
$CiTimeoutMinutes = 1
$RequiredCiJobs = @('windows')
$ClassicInstaller = $false
$VcCrtDir = 'C:\fake'
$SlintSdk = 'C:\fake-slint'
$GitUserName = 'Test'
$GitUserEmail = 'test@example.invalid'
$RequireGate = $false
$NoCiWait = $false
$CiRunMode = 'ok'

$tests = [ordered]@{}

# ---- the step list ------------------------------------------------------------------------------------------------
$tests['steps-listed'] = {
    $m = [regex]::Match($srcText, "\`$script:StepOrder = @\(([^)]*)\)")
    if (-not $m.Success) { return 'no StepOrder line' }
    $order = @([regex]::Matches($m.Groups[1].Value, "'([^']+)'") | ForEach-Object { $_.Groups[1].Value })
    foreach ($n in @('gate-hash', 'ci-check')) { if ($order -notcontains $n) { return ($n + ' is not in the step order') } }
    if ([array]::IndexOf($order, 'gate-hash') -ge [array]::IndexOf($order, 'build')) { return 'gate-hash must come before build' }
    if ([array]::IndexOf($order, 'ci-check') -le [array]::IndexOf($order, 'ci')) { return 'ci-check must come after ci' }
    if ($srcText -notmatch "'gate-hash'\s*\{\s*\`$outcome = Step-GateHash\s*\}") { return 'no dispatch line for gate-hash' }
    if ($srcText -notmatch "'ci-check'\s*\{\s*\`$outcome = Step-CiCheck\s*\}") { return 'no dispatch line for ci-check' }
    return ''
}
$tests['build-step-checks-gate'] = {
    $body = [regex]::Match($srcText, '(?s)function Step-Build \{.*?\r?\n\}\r?\n').Value
    $i = $body.IndexOf('Assert-GateBinaries')
    $j = $body.IndexOf("Set-StepDone 'build'")
    if ($i -lt 0) { return 'Step-Build never calls Assert-GateBinaries' }
    if ($i -gt $j) { return 'Step-Build records itself done before it checks the gate' }
    return ''
}

# ---- gate-hash: recording ------------------------------------------------------------------------------------------
$tests['gate-record'] = {
    $rc = New-WindowsCase
    $e = Try-Step { Step-GateHash }
    if ($e) { return ('the step stopped: ' + $e) }
    $rec = Read-JsonFile (Join-Path $rc.Verify 'gate-binaries.json')
    if ($null -eq $rec) { return 'no verify\gate-binaries.json' }
    if ($rec['head'] -ne $rc.Head) { return 'the record does not name this commit' }
    if ($rec['files'].Count -ne $gateFiles.Count) { return ('the record holds ' + $rec['files'].Count + ' files, not ' + $gateFiles.Count) }
    foreach ($f in $gateFiles) {
        $want = Get-Sha256 (Join-Path $script:BuildDir $f)
        if ($rec['files'][$f] -ne $want) { return ('wrong hash for ' + $f) }
    }
    if ($script:Done -ne 'gate-hash') { return 'the step did not record itself done' }
    return ''
}
$tests['gate-record-missing-binary'] = {
    $rc = New-WindowsCase
    Remove-Item -LiteralPath (Join-Path $script:BuildDir 'src\slint\Release\slint_cpp.dll')
    $e = Try-Step { Step-GateHash }
    if ($e -notlike 'RELEASE-FAIL:*') { return ('expected a refusal, got: ' + $e) }
    if (Test-Path -LiteralPath (Join-Path $rc.Verify 'gate-binaries.json')) { return 'a record was written for an incomplete build' }
    return ''
}
$tests['gate-record-twice-differs'] = {
    $rc = New-WindowsCase
    Add-GateRecord
    Put-Bytes (Join-Path $script:BuildDir 'Release\cascade.exe') 'relinked after the gates'
    $e = Try-Step { Step-GateHash }
    if ($e -notlike 'RELEASE-FAIL:*') { return ('a second record over a changed binary was not refused: ' + $e) }
    $rec = Read-JsonFile (Join-Path $rc.Verify 'gate-binaries.json')
    if ($rec['files']['Release\cascade.exe'] -eq (Get-Sha256 (Join-Path $script:BuildDir 'Release\cascade.exe'))) { return 'the record was overwritten' }
    return ''
}
$tests['gate-record-twice-same'] = {
    $null = New-WindowsCase
    Add-GateRecord
    $script:Messages.Clear()
    $e = Try-Step { Step-GateHash }
    if ($e) { return ('re-recording unchanged files stopped: ' + $e) }
    return ''
}
$tests['gate-record-dry'] = {
    $rc = New-WindowsCase
    $script:Ctx.DryRun = $true
    $e = Try-Step { Step-GateHash }
    if ($e) { return ('the dry run stopped: ' + $e) }
    if (Test-Path -LiteralPath (Join-Path $rc.Verify 'gate-binaries.json')) { return 'a dry run wrote the record' }
    if (-not (Has-Message 'DRY*gate*')) { return 'the dry run printed no plan line' }
    return ''
}

# ---- -RequireGate: the check --------------------------------------------------------------------------------------
$tests['gate-check-ok'] = {
    $null = New-WindowsCase
    Add-GateRecord
    $script:RequireGate = $true
    $e = Try-Step { Assert-GateBinaries 'test' }
    if ($e) { return ('an unchanged build was refused: ' + $e) }
    if (-not (Has-Message 'PASS*gate*')) { return 'no PASS line' }
    return ''
}
$tests['gate-check-off'] = {
    $null = New-WindowsCase
    $script:RequireGate = $false
    $e = Try-Step { Assert-GateBinaries 'test' }
    if ($e) { return ('without -RequireGate the check stopped: ' + $e) }
    return ''
}
$tests['gate-check-changed-binary'] = {
    $null = New-WindowsCase
    Add-GateRecord
    Put-Bytes (Join-Path $script:BuildDir 'src\slint\Release\foxsdr-slint.exe') 'rebuilt after the gates'
    $script:RequireGate = $true
    $e = Try-Step { Assert-GateBinaries 'test' }
    if ($e -notlike 'RELEASE-FAIL:*') { return ('a binary changed after the gates was not refused: ' + $e) }
    if (-not (Has-Message 'FAIL*foxsdr-slint.exe*')) { return 'the FAIL line does not name the changed file' }
    return ''
}
$tests['gate-check-no-record'] = {
    $null = New-WindowsCase
    $script:RequireGate = $true
    $e = Try-Step { Assert-GateBinaries 'test' }
    if ($e -notlike 'RELEASE-FAIL:*') { return ('a missing record was not refused: ' + $e) }
    return ''
}
$tests['gate-check-other-commit'] = {
    $rc = New-WindowsCase
    Add-GateRecord
    $p = Join-Path $rc.Verify 'gate-binaries.json'
    $t = [IO.File]::ReadAllText($p).Replace($rc.Head, ('f' * 40))
    [IO.File]::WriteAllText($p, $t)
    $script:RequireGate = $true
    $e = Try-Step { Assert-GateBinaries 'test' }
    if ($e -notlike 'RELEASE-FAIL:*') { return ('a record for another commit was not refused: ' + $e) }
    return ''
}
$tests['gate-check-extra-softgl-file'] = {
    $null = New-WindowsCase
    Add-GateRecord
    Put-Bytes (Join-Path $script:BuildDir 'Release\softgl\extra.dll') 'not gated'
    $script:RequireGate = $true
    $e = Try-Step { Assert-GateBinaries 'test' }
    if ($e -notlike 'RELEASE-FAIL:*') { return ('a file added to softgl after the gates was not refused: ' + $e) }
    return ''
}
$tests['gate-check-dry'] = {
    $null = New-WindowsCase
    $script:RequireGate = $true
    $script:Ctx.DryRun = $true
    $e = Try-Step { Assert-GateBinaries 'test' }
    if ($e) { return ('the dry run stopped: ' + $e) }
    if (-not (Has-Message 'DRY*gate*')) { return 'the dry run printed no plan line' }
    return ''
}

# ---- the installer and the MSIX refuse before they build ----------------------------------------------------------------
$tests['installer-refuses-changed-binary'] = {
    $null = New-WindowsCase
    Add-GateRecord
    Put-Bytes (Join-Path $script:BuildDir 'Release\cascade.exe') 'relinked'
    $script:RequireGate = $true
    $e = Try-Step { Step-Installer }
    if ($e -notlike 'RELEASE-FAIL:*') { return ('the installer step went on over a changed binary: ' + $e) }
    if (-not (Has-Message 'FAIL*cascade.exe*')) { return 'the FAIL line does not name the changed file' }
    if ($script:ExeCalls.Count -ne 0) { return ('a process was started: ' + ($script:ExeCalls -join ' | ')) }
    return ''
}
$tests['installer-gate-ok-reaches-iscc'] = {
    $null = New-WindowsCase
    Add-GateRecord
    $script:RequireGate = $true
    $e = Try-Step { Step-Installer }
    if ($e -ne 'RELEASE-FAIL: ISCC found') { return ('expected to pass the gate and stop at ISCC (the fake finds none), got: ' + $e) }
    return ''
}
$tests['msix-refuses-changed-binary'] = {
    $null = New-WindowsCase
    Add-GateRecord
    Put-Bytes (Join-Path $script:BuildDir 'Release\SoapySDR.dll') 'swapped'
    $script:RequireGate = $true
    $e = Try-Step { Step-Msix }
    if ($e -notlike 'RELEASE-FAIL:*') { return ('the msix step went on over a changed binary: ' + $e) }
    if (-not (Has-Message 'FAIL*SoapySDR.dll*')) { return 'the FAIL line does not name the changed file' }
    if ($script:ExeCalls.Count -ne 0) { return ('a process was started: ' + ($script:ExeCalls -join ' | ')) }
    return ''
}

# ---- ci: -NoCiWait ----------------------------------------------------------------------------------------------------
$tests['ci-nowait-real'] = {
    $rc = New-WindowsCase
    Put-Bytes (Join-Path $rc.Dist $installer) 'fake installer bytes'
    $want = Get-Sha256 (Join-Path $rc.Dist $installer)
    $script:NoCiWait = $true
    $e = Try-Step { Step-Ci }
    if ($e) { return ('the ci step stopped: ' + $e) }
    if ($script:CiRunLookups -ne 0 -or $script:CiJobLookups -ne 0) { return ('GitHub was asked: ' + $script:CiRunLookups + ' run lookups, ' + $script:CiJobLookups + ' job lookups') }
    if ($script:ExeCalls.Count -ne 0) { return ('a process was started: ' + ($script:ExeCalls -join ' | ')) }
    $sum = [IO.File]::ReadAllText((Join-Path $rc.Dist 'sha256.txt'))
    if ($sum -ne ($want + ' *' + $installer + "`n")) { return ('sha256.txt is: ' + $sum) }
    $got = @(Get-NotesEntries $rc.NotesPath)
    if ($got.Count -ne 1 -or $got[0].Value -ne $want) { return 'the one hash line was not filled with the installer hash' }
    if ($script:Done -ne 'ci') { return 'the step did not record itself done' }
    if (-not (Has-Message 'INFO*CI*not*wait*')) { return 'no INFO line saying CI was not waited for' }
    return ''
}
$tests['ci-nowait-five-refused'] = {
    $rc = Initialize-Case $caseRoot (New-NotesText $titlePlain (New-Blocks $fiveNames @('TBD', 'TBD', 'TBD', 'TBD', 'TBD')))
    Put-Bytes (Join-Path $rc.Dist $installer) 'fake installer bytes'
    $script:NoCiWait = $true
    $e = Try-Step { Step-Ci }
    if ($e -notlike 'RELEASE-FAIL:*') { return ('a five-file release with -NoCiWait was not refused: ' + $e) }
    if ($script:CiJobLookups -ne 0) { return 'GitHub was asked' }
    return ''
}
$tests['ci-default-still-waits'] = {
    $rc = New-WindowsCase
    Put-Bytes (Join-Path $rc.Dist $installer) 'fake installer bytes'
    $e = Try-Step { Step-Ci }
    if ($e) { return ('the ci step stopped: ' + $e) }
    if ($script:CiJobLookups -lt 1) { return 'without -NoCiWait the ci step no longer waits for the run' }
    return ''
}
$tests['ci-nowait-dry'] = {
    $null = New-WindowsCase
    $script:NoCiWait = $true
    $script:Ctx.DryRun = $true
    $e = Try-Step { Step-Ci }
    if ($e) { return ('the dry run stopped: ' + $e) }
    if (Has-Message 'DRY*wait for jobs*') { return 'the plan still waits for jobs' }
    if (-not (Has-Message 'DRY*sha256.txt*')) { return 'the plan lost the sha256.txt line' }
    return ''
}

# ---- ci-check: read once, never fail ----------------------------------------------------------------------------------
$tests['ci-check-green'] = {
    $null = New-WindowsCase
    $e = Try-Step { Step-CiCheck }
    if ($e) { return ('ci-check stopped: ' + $e) }
    if ($script:CiJobLookups -ne 1) { return ('expected ONE read, saw ' + $script:CiJobLookups) }
    if (Has-Message 'WARN*') { return 'a green run produced a WARN' }
    if (-not (Has-Message 'PASS*windows*')) { return 'no PASS line for the windows job' }
    return ''
}
$tests['ci-check-red'] = {
    $null = New-WindowsCase
    $script:CiRunMode = 'red'
    $e = Try-Step { Step-CiCheck }
    if ($e) { return ('a red job made ci-check fail: ' + $e) }
    if (-not (Has-Message 'WARN*windows*failure*')) { return 'no WARN line naming the red job' }
    if (-not (Has-Message '*gh run rerun*')) { return 'no rerun hint' }
    return ''
}
$tests['ci-check-no-run'] = {
    $null = New-WindowsCase
    $script:CiRunMode = 'none'
    $e = Try-Step { Step-CiCheck }
    if ($e) { return ('a missing run made ci-check fail: ' + $e) }
    if (-not (Has-Message 'WARN*')) { return 'no WARN line' }
    return ''
}
$tests['ci-check-gh-fails'] = {
    $null = New-WindowsCase
    $script:CiRunMode = 'throw'
    $e = Try-Step { Step-CiCheck }
    if ($e) { return ('a gh failure made ci-check fail: ' + $e) }
    if (-not (Has-Message 'WARN*')) { return 'no WARN line' }
    return ''
}
$tests['ci-check-dry'] = {
    $null = New-WindowsCase
    $script:Ctx.DryRun = $true
    $e = Try-Step { Step-CiCheck }
    if ($e) { return ('the dry run stopped: ' + $e) }
    if ($script:CiJobLookups -ne 0) { return 'the dry run asked GitHub' }
    if (-not (Has-Message 'DRY*')) { return 'no plan line' }
    return ''
}

# ---- run ----------------------------------------------------------------------------------------------------------------
$names = @($tests.Keys)
if ($Cases.Count) {
    $selected = @($Cases | ForEach-Object { $_ -split ',' })
    $names = @($names | Where-Object { $selected -contains $_ })
}
if (-not $names.Count) { throw 'no selected cases' }
$failed = 0
foreach ($name in $names) {
    $caseRoot = Join-Path $runRoot $name
    New-Item -ItemType Directory -Path $caseRoot -Force | Out-Null
    $script:Messages = New-Object Collections.ArrayList
    $script:ExeCalls = New-Object Collections.ArrayList
    $script:Done = ''; $script:DoneOutputs = $null
    $script:CiRunLookups = 0; $script:CiJobLookups = 0
    $problem = ''
    try { $problem = [string](@(& $tests[$name]) | Select-Object -Last 1) } catch { $problem = 'the case itself threw: ' + $_.Exception.Message }
    if ($problem) { $failed++; Write-Host ('FAIL ' + $name + ': ' + $problem); Write-Host (($script:Messages | Select-Object -Last 8) -join "`n") }
    else { Write-Host ('PASS ' + $name) }
}
Write-Host ('local-release fakes: ' + $names.Count + ' cases, ' + $failed + ' failed; PowerShell ' + $PSVersionTable.PSVersion + '; scratch ' + $runRoot)
if ($failed) { exit 1 }
exit 0
