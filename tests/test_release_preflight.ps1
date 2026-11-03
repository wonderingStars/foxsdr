# Fake-only regression suite for how tools/release.ps1 treats the hash lines of the release notes: a Windows-only
# release (the notes title ends in "<em dash> Windows") carries ONE "SHA-256 of `foxsdr-setup-<v>.exe`:" block and has the
# installer alone as its release file; any other release carries FIVE. Definitions are AST-extracted (as
# tests/test_release_install.ps1 and tests/test_release_symbols.ps1 do), never the release script's main body.
# Get-ReleaseContext, Step-Preflight, Set-NotesHashes, Step-Ci (dry and real), Step-Symbols (dry) and Step-Github (dry)
# are the REAL functions, run against a scratch repository (CMakeLists.txt, README.md, the notes, a dist folder) made
# under build\release-preflight-test\fake-<PID>-<guid>. Only git, the tool lookup, the process runner and the CI lookups
# are recording fakes. Nothing is built, installed, downloaded or sent.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File tests\test_release_preflight.ps1
#   pwsh -NoProfile -File tests\test_release_preflight.ps1
#   ... -SourcePath <a copy of release.ps1>     (the mutation check: run it against the OLD script)
#   ... -Cases preflight-windows-one-tbd,notes-set-five
#
# What it holds:
#   title-detection         the title rule alone: "<em dash> Windows" then nothing, a space and more, or a non-letter; not a
#                           hyphen, not "Windowsill", not "Linux and Windows", not an untitled notes file.
#   real-notes              the notes in this tree: 0.99.103 is a five-block release, 0.99.104 and 0.99.105 are Windows-only
#                           with exactly one block, named for their installer.
#   context-windows / context-plain   Get-ReleaseContext: WindowsOnly, Files (the installer alone, or five), InstallerName.
#   preflight-windows-one-tbd / -one-sha    a Windows-only notes file with one TBD line, with one real sha256: the notes
#                           checks pass and the INFO line says "Windows-only release: the notes carry one hash line".
#   preflight-windows-bad-value   one block whose value is neither TBD nor a sha256: refused.
#   preflight-windows-five-blocks a Windows-only title with five blocks: refused, in a sentence that says why.
#   preflight-plain-one-block     a plain title with one block: refused, in a sentence that says why.
#   preflight-five-tbd / -five-sha / -five-mixture   the plain five-block release as before (and a mixture still refused).
#   notes-set-windows-one (LF and CRLF), notes-set-five, notes-set-windows-five-refused, notes-set-plain-one-refused
#                           Set-NotesHashes writes exactly the hashes, keeps every line ending and the em dash, and
#                           refuses a block count that does not fit the release.
#   ci-dry-windows / ci-dry-five, symbols-dry-windows / symbols-dry-five, github-dry-windows
#                           the dry-run plans: the Windows-only release names no Linux download, no Linux symbols, no
#                           Linux asset (an INFO line says they are skipped); the five-file release is unchanged.
#   ci-real-windows-only    the real ci step on a Windows-only release with fakes for the CI lookup: no Linux artifact is
#                           downloaded, dist\sha256.txt carries the installer alone, the one TBD line becomes the
#                           installer's hash.
param(
    [string]$SourcePath = (Join-Path $PSScriptRoot '..\tools\release.ps1'),
    [string[]]$Cases = @()
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0
$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$runRoot = Join-Path $repoRoot ('build\release-preflight-test\fake-' + $PID + '-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $runRoot -Force | Out-Null
foreach ($source in @((Join-Path $repoRoot 'tools\release-common.ps1'), $SourcePath)) {
    $tokens = $null; $errors = $null
    $ast = [Management.Automation.Language.Parser]::ParseFile($source, [ref]$tokens, [ref]$errors)
    if ($errors.Count) { throw ($errors | Out-String) }
    foreach ($def in $ast.FindAll({param($n) $n -is [Management.Automation.Language.FunctionDefinitionAst]}, $false)) {
        $text = $def.Extent.Text
        # the release script finds its repository from its own folder; here that folder is the scratch repository's tools\
        if ($def.Name -eq 'Get-ReleaseContext') { $text = $text.Replace('$PSScriptRoot', '$script:FakeScriptRoot') }
        . ([scriptblock]::Create($text))
    }
}

# ---- the seams: nothing real is reached ---------------------------------------------------------------------------
function Write-Result { param($Level, $Name, $Value = '') [void]$script:Messages.Add(($Level + ' ' + $Name + ' : ' + $Value)) }
function Write-Detail { param($Text) [void]$script:Messages.Add([string]$Text) }
function Test-StepDone { return $false }
function Assert-StepDone {}
function Set-StepDone { $script:Done = $true }
function Find-Tool { param($Name, $Candidates) if ($Name -eq 'git') { return 'fake-git' } return '' }
function Invoke-Git {
    param([string[]]$GitArgs, [string[]]$Identity = @())
    [void]$script:GitCalls.Add(($GitArgs -join ' '))
    switch ($GitArgs[0]) {
        'status'   { return @{ ExitCode = 0; StdOut = ''; Output = '' } }
        'rev-list' { return @{ ExitCode = 128; StdOut = ''; Output = 'unknown revision' } }
        'diff'     { return @{ ExitCode = 0; StdOut = "+changed`n"; Output = "+changed`n" } }
    }
    return @{ ExitCode = 0; StdOut = ''; Output = '' }
}
function Invoke-Exe {
    param($File, $Arguments, $Env, $WorkDir, $TimeoutSec, $InputFile, $Label, $HeartbeatSec, [switch]$NoLog)
    [void]$script:ExeCalls.Add(($File + ' ' + ($Arguments -join ' ')))
    if ($Arguments -contains 'rev-parse') { return @{ ExitCode = 0; StdOut = "0123456789abcdef0123456789abcdef01234567`n"; Output = ''; TimedOut = $false } }
    return @{ ExitCode = 1; StdOut = ''; Output = 'unexpected process'; TimedOut = $false }
}
function Get-CiRun { return [pscustomobject]@{ databaseId = 4242; url = 'https://example.invalid/runs/4242' } }
function Get-CiJobs {
    param($RunId)
    return [pscustomobject]@{ jobs = @(
        [pscustomobject]@{ name = 'linux'; status = 'completed'; conclusion = 'success' },
        [pscustomobject]@{ name = 'arm64'; status = 'completed'; conclusion = 'success' }) }
}

$dash = [string][char]0x2014
$version = '0.99.105'
$installer = 'foxsdr-setup-' + $version + '.exe'
$fiveNames = @($installer, ('FoxSDR-' + $version + '-x86_64.AppImage'), ('foxsdr-' + $version + '-linux-x64.tar.gz'), ('FoxSDR-' + $version + '-aarch64.AppImage'), ('foxsdr-' + $version + '-linux-arm64.tar.gz'))
$sha1 = 'a' * 64
$sha2 = 'b' * 64
$titleWindows = '# FoxSDR ' + $version + ' ' + $dash + ' Windows (Slint beta)'
$titlePlain = '# FoxSDR ' + $version

function New-Blocks([string[]]$Names, [string[]]$Values) {
    $b = @()
    for ($i = 0; $i -lt $Names.Count; $i++) { $b += , @($Names[$i], $Values[$i]) }
    return , $b
}
function New-NotesText([string]$Title, $Blocks, [string]$Eol = "`r`n") {
    $lines = @($Title, '', 'Some release text with an em dash ' + $dash + ' in it.', '', '---', '')
    foreach ($b in $Blocks) { $lines += ('**SHA-256 of `' + $b[0] + '`:**'); $lines += ('`' + $b[1] + '`'); $lines += '' }
    return (($lines -join $Eol).TrimEnd("`r", "`n") + $Eol)
}
function Put-Bytes([string]$Path, [string]$Text) {
    New-Item -ItemType Directory -Path ([IO.Path]::GetDirectoryName($Path)) -Force | Out-Null
    [IO.File]::WriteAllText($Path, $Text, (New-Object Text.UTF8Encoding($false)))
}
# A scratch repository at the release version, the notes in it, and the release context the real script builds from it.
function Initialize-Case([string]$CaseRoot, [string]$NotesText) {
    $repo = Join-Path $CaseRoot 'repo'
    Put-Bytes (Join-Path $repo 'CMakeLists.txt') ("cmake_minimum_required(VERSION 3.20)`r`nproject(cascade VERSION " + $version + " LANGUAGES CXX)`r`n")
    Put-Bytes (Join-Path $repo 'README.md') ('The current release is **' + $version + '** of FoxSDR.' + "`r`n")
    Put-Bytes (Join-Path $repo ('docs\release-notes-' + $version + '.md')) $NotesText
    New-Item -ItemType Directory -Path (Join-Path $repo 'tools') -Force | Out-Null
    $script:FakeScriptRoot = Join-Path $repo 'tools'
    $script:ReleaseRoot = Join-Path $CaseRoot 'rel'
    $script:BuildDir = ''
    $script:Ctx = @{ DryRun = $false; LogPath = ''; Secrets = (New-Object Collections.ArrayList) }
    $script:Cfg = @{ repo = 'fake/repo'; nasHost = 'fake-nas'; nasSymbolRoot = '/fake/symbols'; piHost = 'fake-pi'; piSymbolDir = '/fake/pi-symbols' }
    $script:Gh = 'fake-gh'
    $script:RC = Get-ReleaseContext
    $script:ExeCalls.Clear()
    return $script:RC
}
function Has-Message([string]$Like) { return (@($script:Messages | Where-Object { $_ -like $Like }).Count -gt 0) }
function Get-Prop($Table, [string]$Name) { if ($Table.ContainsKey($Name)) { return $Table[$Name] } return $null }
function Invoke-Preflight {
    $script:Messages.Clear()
    $err = ''
    try { $null = Step-Preflight } catch { $err = $_.Exception.Message }
    return $err
}
# a preflight that got past the notes checks stops at the first tool check (cmake is not found by the fake)
$pastNotes = 'RELEASE-FAIL: tool: cmake'

$AllowDirty = $false
$Force = $false
$CiRunId = ''
$CiTimeoutMinutes = 1
$RequiredCiJobs = @('linux', 'arm64')
$GitUserName = 'Test'
$GitUserEmail = 'test@example.invalid'

# ---- the cases ------------------------------------------------------------------------------------------------------
$tests = [ordered]@{}

$tests['title-detection'] = {
    $table = @(
        @(('# FoxSDR 0.99.104 ' + $dash + ' Windows'), $true),
        @(('# FoxSDR 0.99.105 ' + $dash + ' Windows (Slint beta)'), $true),
        @(('# FoxSDR 0.99.105 ' + $dash + ' Windows-only'), $true),
        @(('# FoxSDR 0.99.105 ' + $dash + ' Windowsill'), $false),
        @(('# FoxSDR 0.99.105 ' + $dash + ' Linux and Windows'), $false),
        @('# FoxSDR 0.99.105 - Windows', $false),
        @('# FoxSDR 0.99.103', $false),
        @('', $false))
    foreach ($row in $table) {
        $path = Join-Path $caseRoot 'n.md'
        Put-Bytes $path ($row[0] + "`r`n`r`nbody`r`n")
        $got = Test-WindowsOnlyNotes $path
        if ($got -ne $row[1]) { return ('title "' + $row[0] + '": Windows-only was ' + $got + ', wanted ' + $row[1]) }
    }
    return ''
}

$tests['real-notes'] = {
    $want = @{ '0.99.103' = 5; '0.99.104' = 1; '0.99.105' = 1 }
    foreach ($v in @('0.99.103', '0.99.104', '0.99.105')) {
        $p = Join-Path $repoRoot ('docs\release-notes-' + $v + '.md')
        if (-not (Test-Path -LiteralPath $p)) { continue }
        $entries = @(Get-NotesEntries $p)
        $winOnly = Test-WindowsOnlyNotes $p
        if ($winOnly -ne ($want[$v] -eq 1)) { return ($v + ': Windows-only read as ' + $winOnly) }
        if ($entries.Count -ne $want[$v]) { return ($v + ': ' + $entries.Count + ' hash blocks, wanted ' + $want[$v]) }
        if ($want[$v] -eq 1 -and $entries[0].Name -ne ('foxsdr-setup-' + $v + '.exe')) { return ($v + ': the one block is for ' + $entries[0].Name) }
    }
    return ''
}

$tests['context-windows'] = {
    $rc = Initialize-Case $caseRoot (New-NotesText $titleWindows (New-Blocks @($installer) @('TBD')))
    if (-not [bool](Get-Prop $rc 'WindowsOnly')) { return 'WindowsOnly is not set for a notes title ending in "Windows (Slint beta)"' }
    if ($rc.Files.Count -ne 1 -or $rc.Files[0] -ne $installer -or $rc.InstallerName -ne $installer) { return ('Files = ' + ($rc.Files -join ', ') + ', InstallerName = ' + $rc.InstallerName) }
    return ''
}

$tests['context-plain'] = {
    $rc = Initialize-Case $caseRoot (New-NotesText $titlePlain (New-Blocks $fiveNames @('TBD', 'TBD', 'TBD', 'TBD', 'TBD')))
    if ([bool](Get-Prop $rc 'WindowsOnly')) { return 'WindowsOnly is set for a plain title' }
    if ($rc.Files.Count -ne 5 -or $rc.Files[0] -ne $installer -or $rc.InstallerName -ne $installer) { return ('Files = ' + ($rc.Files -join ', ') + ', InstallerName = ' + $rc.InstallerName) }
    return ''
}

function Test-PreflightPass([string]$Title, $Blocks, [string]$Info) {
    $null = Initialize-Case $caseRoot (New-NotesText $Title $Blocks)
    $err = Invoke-Preflight
    if ($err -ne $pastNotes) { return ('preflight stopped at "' + $err + '" instead of getting past the notes checks to the tools') }
    if (-not (Has-Message ('PASS notes hash lines*'))) { return 'no PASS line for the notes hash lines' }
    if (-not (Has-Message ('INFO ' + $Info + '*'))) { return ('no INFO line "' + $Info + '"') }
    return ''
}
function Test-PreflightRefused([string]$Title, $Blocks, [string]$Failed, [string]$Sentence) {
    $null = Initialize-Case $caseRoot (New-NotesText $Title $Blocks)
    $err = Invoke-Preflight
    if ($err -ne ('RELEASE-FAIL: ' + $Failed)) { return ('preflight ended with "' + $err + '", wanted "RELEASE-FAIL: ' + $Failed + '"') }
    if ($Sentence -ne '' -and -not (Has-Message ('*' + $Sentence + '*'))) { return ('the refusal does not say: ' + $Sentence) }
    return ''
}

$tests['preflight-windows-one-tbd'] = { return (Test-PreflightPass $titleWindows (New-Blocks @($installer) @('TBD')) 'Windows-only release: the notes carry one hash line') }
$tests['preflight-windows-one-sha'] = { return (Test-PreflightPass ('# FoxSDR ' + $version + ' ' + $dash + ' Windows') (New-Blocks @($installer) @($sha1)) 'Windows-only release: the notes carry one hash line') }
$tests['preflight-windows-bad-value'] = { return (Test-PreflightRefused $titleWindows (New-Blocks @($installer) @('soon')) 'notes hash lines' 'one TBD line') }
$tests['preflight-windows-five-blocks'] = {
    return (Test-PreflightRefused $titleWindows (New-Blocks $fiveNames @('TBD', 'TBD', 'TBD', 'TBD', 'TBD')) 'notes name the one release file' 'Windows-only release and the notes must carry exactly one hash block')
}
$tests['preflight-plain-one-block'] = {
    return (Test-PreflightRefused $titlePlain (New-Blocks @($installer) @('TBD')) 'notes name the five release files' 'is a five-file release and the notes must carry five hash blocks')
}
$tests['preflight-five-tbd'] = { return (Test-PreflightPass $titlePlain (New-Blocks $fiveNames @('TBD', 'TBD', 'TBD', 'TBD', 'TBD')) 'Five-file release: the notes carry five hash lines') }
$tests['preflight-five-sha'] = { return (Test-PreflightPass $titlePlain (New-Blocks $fiveNames @($sha1, $sha1, $sha2, $sha2, $sha1)) 'Five-file release: the notes carry five hash lines') }
$tests['preflight-five-mixture'] = { return (Test-PreflightRefused $titlePlain (New-Blocks $fiveNames @('TBD', $sha1, 'TBD', $sha2, 'TBD')) 'notes hash lines' 'never a mixture') }

function Test-NotesSet([string]$Title, $Names, [string]$Eol) {
    $values = @(); foreach ($n in $Names) { $values += 'TBD' }
    $rc = Initialize-Case $caseRoot (New-NotesText $Title (New-Blocks $Names $values) $Eol)
    $before = [IO.File]::ReadAllBytes($rc.NotesPath)
    $hashes = @{}; $i = 0
    foreach ($n in $rc.Files) { $hashes[$n] = ([string]($i + 1)) * 64; $i++ }
    $script:Messages.Clear()
    Set-NotesHashes $rc.NotesPath $hashes
    $after = [IO.File]::ReadAllBytes($rc.NotesPath)
    $crB = @($before | Where-Object { $_ -eq 13 }).Count; $crA = @($after | Where-Object { $_ -eq 13 }).Count
    if ($crB -ne $crA) { return ('CRs ' + $crB + ' before, ' + $crA + ' after') }
    if ($after.Length -ne $before.Length + 61 * $Names.Count) { return ('length ' + $before.Length + ' -> ' + $after.Length) }
    if ($after.Length -ge 3 -and $after[0] -eq 0xEF -and $after[1] -eq 0xBB -and $after[2] -eq 0xBF) { return 'the edit added a BOM' }
    $text = [Text.Encoding]::UTF8.GetString($after)
    if (-not $text.Contains($dash)) { return 'the em dash was lost' }
    if ($text.StartsWith($Title) -eq $false) { return 'the title line changed' }
    $got = @(Get-NotesEntries $rc.NotesPath)
    if ($got.Count -ne $Names.Count) { return ('entries ' + $got.Count) }
    foreach ($e in $got) { if ($e.Value -ne $hashes[$e.Name]) { return ('the block for ' + $e.Name + ' holds ' + $e.Value) } }
    # nothing but the hashes changed: put the TBDs back and compare byte for byte
    $restored = $text
    foreach ($h in $hashes.Values) { $restored = $restored.Replace($h, 'TBD') }
    if ($restored -ne [Text.Encoding]::UTF8.GetString($before)) { return 'something other than the hash values changed' }
    return ''
}
$tests['notes-set-windows-one-crlf'] = { return (Test-NotesSet $titleWindows @($installer) "`r`n") }
$tests['notes-set-windows-one-lf'] = { return (Test-NotesSet $titleWindows @($installer) "`n") }
$tests['notes-set-five'] = { return (Test-NotesSet $titlePlain $fiveNames "`r`n") }

function Test-NotesSetRefused([string]$Title, $Names, [string]$Sentence) {
    $values = @(); foreach ($n in $Names) { $values += 'TBD' }
    $rc = Initialize-Case $caseRoot (New-NotesText $Title (New-Blocks $Names $values))
    $before = [IO.File]::ReadAllBytes($rc.NotesPath)
    $hashes = @{}; foreach ($n in $rc.Files) { $hashes[$n] = 'c' * 64 }
    $err = ''
    try { Set-NotesHashes $rc.NotesPath $hashes } catch { $err = $_.Exception.Message }
    if ($err -notlike ('*' + $Sentence + '*')) { return ('Set-NotesHashes ended with "' + $err + '", wanted: ' + $Sentence) }
    if ([Convert]::ToBase64String([IO.File]::ReadAllBytes($rc.NotesPath)) -ne [Convert]::ToBase64String($before)) { return 'the notes were changed although the edit was refused' }
    return ''
}
$tests['notes-set-windows-five-refused'] = { return (Test-NotesSetRefused $titleWindows $fiveNames 'expected one hash block in the notes, found 5') }
$tests['notes-set-plain-one-refused'] = { return (Test-NotesSetRefused $titlePlain @($installer) 'expected five hash blocks in the notes, found 1') }

function New-StepCase([bool]$WindowsOnly, [bool]$DryRun) {
    if ($WindowsOnly) { $rc = Initialize-Case $caseRoot (New-NotesText $titleWindows (New-Blocks @($installer) @('TBD'))) }
    else { $rc = Initialize-Case $caseRoot (New-NotesText $titlePlain (New-Blocks $fiveNames @('TBD', 'TBD', 'TBD', 'TBD', 'TBD'))) }
    $script:Ctx.DryRun = $DryRun
    $script:Messages.Clear()
    return $rc
}
$tests['ci-dry-windows'] = {
    $null = New-StepCase $true $true
    $err = ''; try { $null = Step-Ci } catch { $err = $_.Exception.Message }
    if ($err) { return $err }
    if (Has-Message '*download foxsdr-linux*') { return 'the plan still downloads Linux artifacts' }
    if (Has-Message '*AppImage*') { return 'the plan still names an AppImage' }
    if (-not (Has-Message 'INFO*Linux*skipped*')) { return 'no INFO line saying the Linux parts are skipped' }
    if (-not (Has-Message '*replace the one TBD line*')) { return 'the edit plan does not say one TBD line' }
    return ''
}
$tests['ci-dry-five'] = {
    $null = New-StepCase $false $true
    $err = ''; try { $null = Step-Ci } catch { $err = $_.Exception.Message }
    if ($err) { return $err }
    foreach ($a in @('foxsdr-linux-x64', 'foxsdr-linux-x64-appimage', 'foxsdr-linux-x64-symbols', 'foxsdr-linux-arm64', 'foxsdr-linux-arm64-appimage', 'foxsdr-linux-arm64-symbols')) {
        if (-not (Has-Message ('DRY download ' + $a + ' *'))) { return ('the plan lost the download of ' + $a) }
    }
    if (Has-Message 'INFO*skipped*') { return 'a five-file release must not say anything is skipped' }
    if (-not (Has-Message '*replace the five TBD lines*with the five sha256 values*')) { return 'the edit plan no longer says five TBD lines' }
    return ''
}
$tests['symbols-dry-windows'] = {
    $null = New-StepCase $true $true
    $err = ''; try { $null = Step-Symbols } catch { $err = $_.Exception.Message }
    if ($err) { return $err }
    if (Has-Message 'DRY Linux symbols*') { return 'the plan still has Linux symbols' }
    if (-not (Has-Message 'INFO Linux symbols*skipped*')) { return 'no INFO line saying the Linux symbols are skipped' }
    return ''
}
$tests['symbols-dry-five'] = {
    $null = New-StepCase $false $true
    $err = ''; try { $null = Step-Symbols } catch { $err = $_.Exception.Message }
    if ($err) { return $err }
    if (-not (Has-Message 'DRY Linux symbols foxsdr-linux-x64-symbols*') -or -not (Has-Message 'DRY Linux symbols foxsdr-linux-arm64-symbols*')) { return 'the plan lost the Linux symbols' }
    if (Has-Message 'INFO Linux symbols*skipped*') { return 'a five-file release must not skip Linux symbols' }
    return ''
}
$tests['github-dry-windows'] = {
    $null = New-StepCase $true $true
    $err = ''; try { $null = Step-Github } catch { $err = $_.Exception.Message }
    if ($err) { return $err }
    $draft = @($script:Messages | Where-Object { $_ -like 'DRY draft release*' })
    if ($draft.Count -ne 1) { return 'no draft release line' }
    if ($draft[0] -notlike ('*' + $installer + '*')) { return 'the draft release does not carry the installer' }
    if ($draft[0] -like '*AppImage*' -or $draft[0] -like '*.tar.gz*') { return 'the draft release still carries Linux files' }
    if (-not (Has-Message '*the notes with the one real hash*')) { return 'the notes line does not say one real hash' }
    return ''
}
$tests['ci-real-windows-only'] = {
    $rc = New-StepCase $true $false
    Put-Bytes (Join-Path $rc.Dist $installer) 'fake installer bytes'
    $want = Get-Sha256 (Join-Path $rc.Dist $installer)
    $err = ''; try { $null = Step-Ci } catch { $err = $_.Exception.Message }
    if ($err) { return ('the ci step stopped: ' + $err) }
    if ($script:ExeCalls.Count -ne 0) { return ('the ci step ran a process: ' + ($script:ExeCalls -join ' | ')) }
    if (Has-Message '*download*foxsdr-linux*') { return 'a Linux artifact was downloaded' }
    if (-not (Has-Message 'INFO*Linux*skipped*')) { return 'no INFO line saying the Linux parts are skipped' }
    $sum = [IO.File]::ReadAllText((Join-Path $rc.Dist 'sha256.txt'))
    if ($sum -ne ($want + ' *' + $installer + "`n")) { return ('sha256.txt is: ' + $sum) }
    $got = @(Get-NotesEntries $rc.NotesPath)
    if ($got.Count -ne 1 -or $got[0].Value -ne $want) { return 'the one hash line was not filled with the installer hash' }
    if (-not (Has-Message 'PASS notes written*one hash (*')) { return 'no PASS line saying one hash was written' }
    return ''
}

# ---- run -------------------------------------------------------------------------------------------------------------
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
    $script:GitCalls = New-Object Collections.ArrayList
    $script:ExeCalls = New-Object Collections.ArrayList
    $script:Done = $false
    $problem = ''
    try { $problem = [string](@(& $tests[$name]) | Select-Object -Last 1) } catch { $problem = 'the case itself threw: ' + $_.Exception.Message }
    if ($problem) { $failed++; Write-Host ('FAIL ' + $name + ': ' + $problem); Write-Host (($script:Messages | Select-Object -Last 12) -join "`n") }
    else { Write-Host ('PASS ' + $name) }
}
Write-Host ('release notes hash-line fakes: ' + $names.Count + ' cases, ' + $failed + ' failed; PowerShell ' + $PSVersionTable.PSVersion + '; scratch ' + $runRoot)
if ($failed) { exit 1 }
exit 0
