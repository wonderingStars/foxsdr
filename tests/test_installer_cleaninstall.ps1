# End-to-end regression suite for the installer's "cleaninstall" task (installer\cascade.iss): a REMEMBERED clean-install choice must never wipe
# the user's data. It compiles a throw-away installer from the real script with a TEST AppId (a different product to Windows), installs it
# per-user into a scratch folder with both data roots moved under the scratch folder (/DATAROOTS=), and checks marker files after each run.
#
#   pwsh -NoProfile -File tests\test_installer_cleaninstall.ps1 -ScratchRoot <dir>
#   ... -IssPath <a copy of cascade.iss>   (the break-it / red check: run it against the old or a mutated script)
#   ... -MutationCheck                     (silent break-it: runs this suite against mutated copies of the script and prints which it catches; the
#                                           interactive reset mutant is expected to go UNdetected, see the report line it prints)
#   ... -Interactive                      (also drive the wizard windows: raises setup dialogs on the desktop, so it is opt-in; never run unattended)
#
# NOTHING real is touched: the AppId is a fresh GUID per run, every setup and uninstaller run gets /CURRENTUSER /DIR= /DATAROOTS=, and the
# payload is a read-only COPY of an installed FoxSDR (no engine build). The real %APPDATA%\foxsdr\config.json is measured before and after.
#
# Cases (a) first install that stores "cleaninstall", then a plain silent upgrade keeps the data; (b) a hand-seeded key holding exactly
# "desktopicon,cleaninstall" (the owner's state), then a silent install keeps the data; (c) /CLEANINSTALL=1 wipes both roots and nothing else;
# (d) a plain fresh install keeps the data; (f) a silent /TASKS="cleaninstall" and /MERGETASKS="cleaninstall" never wipe (decision: only
# /CLEANINSTALL=1 wipes unattended); (e) interactive (-Interactive only): the tasks page shows the box UNticked although the key holds it, and a wipe happens only
# when it is ticked in that run (driven through UI Automation; reported SKIPPED, not passed, if the wizard cannot be driven).
param(
    [string]$IssPath = (Join-Path $PSScriptRoot '..\installer\cascade.iss'),
    [string]$ScratchRoot = (Join-Path $PSScriptRoot '..\build\installer-cleaninstall-test'),
    [string]$PayloadSource = 'C:\Program Files\FoxSDR',
    [string]$Iscc = (Join-Path $env:LOCALAPPDATA 'Programs\Inno Setup 6\ISCC.exe'),
    [string]$Label = 'run',
    [switch]$Interactive,
    [switch]$MutationCheck
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0
$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$IssPath = [IO.Path]::GetFullPath($IssPath)
$runRoot = Join-Path ([IO.Path]::GetFullPath($ScratchRoot)) ($Label + '-' + [guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory -Path $runRoot -Force | Out-Null
$logPath = Join-Path $runRoot 'test.log'
$realGuid = '{B3D4A7E2-6C51-4F8B-9A0D-2E7F5C8B1A64}'
$testGuid = '{' + [guid]::NewGuid().ToString().ToUpperInvariant() + '}'
$uninstKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\' + $testGuid + '_is1'
# The test build renames the foxsdr: protocol key and the Desktop shortcut with this suffix (cascade.iss /DTestSuffix), so no run can overwrite or
# delete the real registration or the real FoxSDR.lnk.
$testSuffix = [guid]::NewGuid().ToString('N').Substring(0, 8)
$testProtoKey = 'HKCU:\Software\Classes\foxsdr-' + $testSuffix
$groupName = 'FoxSDR-InstTest-' + $testGuid.Substring(1, 8)
$script:checks = 0; $script:failed = 0; $script:skipped = 0
$script:installs = New-Object System.Collections.ArrayList
function Log([string]$Text) { Add-Content -LiteralPath $logPath -Value $Text -Encoding UTF8 }
function Check([bool]$Ok, [string]$Name) {
    $script:checks++
    if (-not $Ok) { $script:failed++ }
    $line = $(if ($Ok) { 'ok    ' } else { 'FAIL  ' }) + $Name
    Log $line; Write-Host $line
}
function Skip([string]$Name) { $script:skipped++; $line = 'SKIP  ' + $Name; Log $line; Write-Host $line }

# --- hard guards: this suite may only ever act on its own scratch folder and its own test AppId -----------------------------------------
if ($testGuid -eq $realGuid) { throw 'test AppId equals the real one' }
if (-not (Test-Path -LiteralPath $Iscc)) { throw ('ISCC not found: ' + $Iscc) }
# The script under test (it may be a mutated or older copy) must itself honour every isolation this suite relies on, or running it would act on the
# REAL product: its AppId, its two data roots, its foxsdr: protocol key and its Desktop shortcut. Checked as text BEFORE anything is compiled or run.
$issText = [IO.File]::ReadAllText($IssPath)
$isolation = @(
    @('overridable AppGuid', '(?m)^#ifndef AppGuid\s*$'),
    @('AppId from AppGuid', '(?m)^AppId=\{\{\{#AppGuid\}\}\s*$'),
    @('uninstall-key lookup from AppGuid', 'Uninstall\\\{\{#AppGuid\}\}_is1'),
    @('TestSuffix renames the protocol key', '(?m)^\s*#define ProtoKey "foxsdr-" \+ TestSuffix'),
    @('TestSuffix renames the shortcut', '(?m)^\s*#define ShortcutName AppName \+ "-" \+ TestSuffix'),
    @('Registry uses ProtoKey only', '(?m)^Root: HKA; Subkey: "Software\\Classes\\\{#ProtoKey\}'),
    @('Desktop shortcut uses ShortcutName', '(?m)^Name: "\{autodesktop\}\\\{#ShortcutName\}"')
)
foreach ($i in $isolation) { if ($issText -notmatch $i[1]) { throw ('refusing to run: the script under test lacks the test isolation "' + $i[0] + '" (' + $IssPath + ')') } }
if (([regex]::Matches($issText, [regex]::Escape('{param:dataroots|}'))).Count -lt 2) { throw ('refusing to run: the script under test does not honour /DATAROOTS= in both data roots (' + $IssPath + ')') }
if ($issText -match '(?m)^Root: HKA; Subkey: "Software\\Classes\\foxsdr') { throw 'refusing to run: the script under test registers the REAL foxsdr: protocol key' }
if ($issText -match '(?m)^Name: "\{autodesktop\}\\\{#AppName\}"') { throw 'refusing to run: the script under test creates the REAL desktop shortcut name' }
function Assert-InRun([string]$Path) {
    $full = [IO.Path]::GetFullPath($Path)
    if (-not $full.StartsWith($runRoot + '\', [StringComparison]::OrdinalIgnoreCase)) { throw ('refusing a path outside the run folder: ' + $full) }
}

# --- the real profile is only MEASURED ----------------------------------------------------------------------------------------------------
$realConfig = Join-Path $env:APPDATA 'foxsdr\config.json'
function Get-RealConfigState {
    if (Test-Path -LiteralPath $realConfig) { $i = Get-Item -LiteralPath $realConfig; return ('sha256=' + (Get-FileHash -LiteralPath $realConfig -Algorithm SHA256).Hash + ' size=' + $i.Length + ' mtimeUtc=' + $i.LastWriteTimeUtc.ToString('o')) }
    return 'absent'
}
# Everything the suite promises not to touch, as one comparable string: the real config, the machine-wide shortcut's target, the two real
# protocol keys (existence and, when present, their command), and the Desktop listing.
function Get-RealState {
    $lnk = 'C:\Users\Public\Desktop\FoxSDR.lnk'
    $target = if (Test-Path -LiteralPath $lnk) { (New-Object -ComObject WScript.Shell).CreateShortcut($lnk).TargetPath } else { 'absent' }
    $cmd = { param($k) if (Test-Path -LiteralPath $k) { (Get-ItemProperty -LiteralPath ($k + '\shell\open\command') -ErrorAction SilentlyContinue).'(default)' } else { 'absent' } }
    $desk = (Get-ChildItem -LiteralPath ([Environment]::GetFolderPath('Desktop')) -Force | ForEach-Object { $_.Name } | Sort-Object) -join '|'
    return [ordered]@{
        config = (Get-RealConfigState); publicLnkTarget = $target
        hkcuProto = ((Test-Path 'HKCU:\Software\Classes\foxsdr').ToString() + ' ' + (& $cmd 'HKCU:\Software\Classes\foxsdr'))
        hklmProto = ((Test-Path 'HKLM:\Software\Classes\foxsdr').ToString() + ' ' + (& $cmd 'HKLM:\Software\Classes\foxsdr'))
        realUninstKey = (Test-Path -LiteralPath ('HKLM:\Software\Microsoft\Windows\CurrentVersion\Uninstall\' + $realGuid + '_is1')).ToString() + ' ' + (Test-Path -LiteralPath ('HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\' + $realGuid + '_is1')).ToString()
        desktop = $desk
    }
}
$realBefore = Get-RealConfigState
$realStateBefore = Get-RealState
Log ('real state before: ' + (($realStateBefore.GetEnumerator() | ForEach-Object { $_.Key + '=' + $_.Value }) -join ' ;; '))

# --- -MutationCheck: run THIS suite (silent, no windows) against mutated copies of the script and report which mutants it catches ----------
if ($MutationCheck) {
    $mutants = @(
        @{ Name = 'mut-reset-removed'; Expect = 'GREEN (not detectable silently)'; ExpectCode = 0
           Why = 'the interactive CurPageChanged reset deleted; silent runs never show the tasks page, so this suite cannot see it'
           WantCleaned = $true   # and the strip must have had something to clean, and cleaned it (proves the strip works on a stored tick)
           Find = "    WizardSelectTasks('!cleaninstall');"; Repl = '    ;' },
        @{ Name = 'mut-reset-and-strip-removed'; Expect = 'RED'; ExpectCode = 1
           Why = 'the reset deleted AND the strip deleted: the tick is stored, so the stored-task assertions must catch it'
           Find = "    WizardSelectTasks('!cleaninstall');"; Repl = '    ;'; Find2 = "    ForgetCleanTask('ssPostInstall');"; Repl2 = "    Log('x');"; Find3 = "    ForgetCleanTask('ssDone');"; Repl3 = "    Log('y');" },
        @{ Name = 'mut-guard-only-removed'; Expect = 'GREEN (second layer; the reset also covers silent runs)'; ExpectCode = 0
           Why = 'CleanInstallWanted trusts the box again, but CurPageChanged still fires in a silent run and unticks it first: two layers, one is enough'
           Find = "  if (not Result) and (not WizardSilent()) and CleanTaskReset then"; Repl = '  if not Result then' },
        @{ Name = 'mut-reset-and-guard-removed'; Expect = 'RED'; ExpectCode = 1
           Why = 'the original bug: nothing unticks a remembered cleaninstall and the condition trusts it'
           Find = "  if (not Result) and (not WizardSilent()) and CleanTaskReset then"; Repl = '  if not Result then'; Find2 = "    WizardSelectTasks('!cleaninstall');"; Repl2 = '    ;' }
    )
    $bad = 0
    foreach ($m in $mutants) {
        $txt = $issText
        foreach ($pair in @(@($m['Find'], $m['Repl']), @($m['Find2'], $m['Repl2']), @($m['Find3'], $m['Repl3']))) { if ($pair[0]) { if (-not $txt.Contains($pair[0])) { throw ('mutation anchor not found: ' + $pair[0]) }; $txt = $txt.Replace($pair[0], $pair[1]) } }
        if ($txt -eq $issText) { throw ('mutant ' + $m.Name + ' did not change the script') }
        $mp = Join-Path $runRoot ($m.Name + '.iss'); [IO.File]::WriteAllText($mp, $txt, (New-Object Text.UTF8Encoding($false)))
        $out = Join-Path $runRoot ($m.Name + '.out.txt')
        & pwsh -NoProfile -File $PSCommandPath -IssPath $mp -ScratchRoot $ScratchRoot -PayloadSource $PayloadSource -Iscc $Iscc -Label $m.Name *> $out
        $code = $LASTEXITCODE
        $fails = @(Select-String -LiteralPath $out -Pattern '^FAIL' | ForEach-Object { $_.Line })
        $line = ('{0}: exit {1}, expected {2}; {3} FAIL lines. {4}' -f $m.Name, $code, $m.Expect, $fails.Count, $m.Why)
        Log $line; Write-Host $line; foreach ($f in $fails) { Write-Host ('    ' + $f); Log ('    ' + $f) }
        if ($code -ne $m.ExpectCode) { $bad++; Write-Host ('  UNEXPECTED for ' + $m.Name) }
        if ($m['WantCleaned']) {
            $childRoot = Get-ChildItem -LiteralPath ([IO.Path]::GetFullPath($ScratchRoot)) -Directory -Filter ($m.Name + '-*') | Sort-Object LastWriteTime | Select-Object -Last 1
            $hits = @(Get-ChildItem -LiteralPath $childRoot.FullName -Filter 'setup-*.log' | ForEach-Object { Select-String -LiteralPath $_.FullName -Pattern 'stored tasks ".*cleaninstall.*" cleaned to' } | ForEach-Object { $_.Line.Trim() })
            Log ('  strip evidence in ' + $childRoot.FullName + ': ' + $hits.Count + ' cleaned lines'); foreach ($h in $hits) { Log ('    ' + $h); Write-Host ('    ' + $h) }
            if ($hits.Count -lt 1) { $bad++; Write-Host ('  UNEXPECTED for ' + $m.Name + ': the strip never cleaned a stored tick') }
        }
    }
    $after = Get-RealState; $same = $true
    foreach ($k in $realStateBefore.Keys) { if ($realStateBefore[$k] -ne $after[$k]) { $same = $false; Write-Host ('REAL STATE CHANGED: ' + $k) } }
    Write-Host ('real state unchanged across the mutation run: ' + $same)
    if ($bad -gt 0 -or -not $same) { exit 1 } else { exit 0 }
}
$protocolKeyExisted = Test-Path 'HKCU:\Software\Classes\foxsdr'

# --- the payload: a read-only copy of an installed FoxSDR, in the layout the script's contract checks want ----------------------------------
$build = Join-Path $runRoot 'payload\build'; $slint = Join-Path $runRoot 'payload\slint'; $crt = Join-Path $runRoot 'payload\crt'
New-Item -ItemType Directory -Force -Path (Join-Path $build 'softgl'), $slint, $crt | Out-Null
foreach ($f in 'cascade.exe', 'SoapySDR.dll') { Copy-Item -LiteralPath (Join-Path $PayloadSource $f) -Destination $build }
foreach ($f in 'opengl32.dll', 'libgallium_wgl.dll') { Copy-Item -LiteralPath (Join-Path $PayloadSource ('softgl\' + $f)) -Destination (Join-Path $build 'softgl') }
foreach ($f in 'foxsdr-slint.exe', 'slint_cpp.dll') { Copy-Item -LiteralPath (Join-Path $PayloadSource $f) -Destination $slint }
foreach ($f in 'msvcp140.dll', 'vcruntime140.dll', 'vcruntime140_1.dll') { Copy-Item -LiteralPath (Join-Path $PayloadSource $f) -Destination $crt }

# --- compile the throw-away installer. The script is copied beside the real one so its relative paths (..\LICENSE ...) resolve ------------
$issCopy = Join-Path (Join-Path $repoRoot 'installer') ('_cleaninstall_test_' + $PID + '.iss')
$outDir = Join-Path $runRoot 'out'
$setup = Join-Path $outDir 'test-setup.exe'
try {
    Copy-Item -LiteralPath $IssPath -Destination $issCopy
    $isccLog = Join-Path $runRoot 'iscc.log'
    & $Iscc "/DAppGuid=$($testGuid.Trim('{}'))" "/DTestSuffix=$testSuffix" '/DAppVersion=0.0.1-test' '/DAppVersionNumeric=0.0.1' "/DBuildDir=$build" "/DSlintDir=$slint" "/DVcCrtDir=$crt" "/O$outDir" '/Ftest-setup' $issCopy *> $isccLog
    Log ('ISCC exit ' + $LASTEXITCODE + ' for ' + $IssPath)
    Check (($LASTEXITCODE -eq 0) -and (Test-Path -LiteralPath $setup)) 'compile: the test installer builds from the script under test'
} finally {
    if (Test-Path -LiteralPath $issCopy) { Remove-Item -LiteralPath $issCopy -Force }
}
if (-not (Test-Path -LiteralPath $setup)) { Write-Host ('compile failed; see ' + $isccLog); exit 2 }

# --- helpers: one install target per case, seeded data, setup / uninstaller runs ---------------------------------------------------------
function New-Case([string]$Name) {
    $c = [pscustomobject]@{ Name = $Name; Dir = (Join-Path $runRoot ('case-' + $Name + '\app')); Data = (Join-Path $runRoot ('case-' + $Name + '\data')); Installed = $false }
    Assert-InRun $c.Dir; Assert-InRun $c.Data
    New-Item -ItemType Directory -Force -Path $c.Data | Out-Null
    [void]$script:installs.Add($c)
    return $c
}
function Roaming($c) { Join-Path $c.Data 'Roaming\foxsdr' }
function Local($c) { Join-Path $c.Data 'Local\FoxSDR' }
function Seed($c) {
    New-Item -ItemType Directory -Force -Path (Roaming $c), (Join-Path (Local $c) 'plugins'), (Join-Path $c.Data 'Roaming\other'), (Join-Path $c.Data 'Local\Other'), (Join-Path $c.Data 'recordings') | Out-Null
    Set-Content -LiteralPath (Join-Path (Roaming $c) 'config.json') -Value '{"marker":1}'
    Set-Content -LiteralPath (Join-Path (Local $c) 'plugins\marker.txt') -Value 'plugin'
    Set-Content -LiteralPath (Join-Path $c.Data 'Roaming\other\keep.txt') -Value 'x'
    Set-Content -LiteralPath (Join-Path $c.Data 'Local\Other\keep.txt') -Value 'x'
    Set-Content -LiteralPath (Join-Path $c.Data 'recordings\keep.iq') -Value 'x'
    Set-Content -LiteralPath (Join-Path $c.Data 'sibling.txt') -Value 'x'
}
function Data-Kept($c) { (Test-Path -LiteralPath (Join-Path (Roaming $c) 'config.json')) -and (Test-Path -LiteralPath (Join-Path (Local $c) 'plugins\marker.txt')) }
function Data-Gone($c) { (-not (Test-Path -LiteralPath (Roaming $c))) -and (-not (Test-Path -LiteralPath (Local $c))) }
function Bystanders-Kept($c) {
    foreach ($p in 'Roaming\other\keep.txt', 'Local\Other\keep.txt', 'recordings\keep.iq', 'sibling.txt') { if (-not (Test-Path -LiteralPath (Join-Path $c.Data $p))) { return $false } }
    return $true
}
function Quote([string]$s) { '"' + $s + '"' }
function Setup-Args($c, [string[]]$Extra, [switch]$Silent) {
    $a = @('/CURRENTUSER', ('/DIR=' + (Quote $c.Dir)), ('/DATAROOTS=' + (Quote $c.Data)), ('/GROUP=' + (Quote $groupName)), ('/LOG=' + (Quote (Join-Path $runRoot ('setup-' + $c.Name + '.log')))), '/NORESTART')
    if ($Silent) { $a += @('/VERYSILENT', '/SUPPRESSMSGBOXES') }
    return ($a + $Extra) -join ' '
}
# A timeout must end the whole tree: the bootstrapper (test-setup.exe) hands the work to a test-setup.tmp child that would keep installing.
function Stop-Tree([int]$ProcessId) { & taskkill.exe /T /F /PID $ProcessId *> $null }
function Run-Setup($c, [string[]]$Extra) {
    $p = Start-Process -FilePath $setup -ArgumentList (Setup-Args $c $Extra -Silent) -PassThru -WindowStyle Hidden
    if (-not $p.WaitForExit(240000)) { Stop-Tree $p.Id; throw ('setup timed out in case ' + $c.Name) }
    Log ('  setup ' + ($Extra -join ' ') + ' -> exit ' + $p.ExitCode)
    if ($p.ExitCode -eq 0) { $c.Installed = $true }
    return $p.ExitCode
}
function Run-Uninstall($c) {
    $u = Join-Path $c.Dir 'unins000.exe'
    if (-not (Test-Path -LiteralPath $u)) { return }
    Assert-InRun $u
    $p = Start-Process -FilePath $u -ArgumentList ('/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /DATAROOTS=' + (Quote $c.Data)) -PassThru -WindowStyle Hidden
    if (-not $p.WaitForExit(120000)) { Stop-Tree $p.Id; Log ('  uninstall ' + $c.Name + ' timed out; process tree killed') }
    Log ('  uninstall ' + $c.Name + ' -> exit ' + $p.ExitCode)
    $c.Installed = $false
}
function Selected-Tasks { (Get-ItemProperty -LiteralPath $uninstKey -ErrorAction SilentlyContinue).'Inno Setup: Selected Tasks' }
function Reset-Key { if (Test-Path -LiteralPath $uninstKey) { Remove-Item -LiteralPath $uninstKey -Recurse -Force } }
# Sets only the one value: when the first install already wrote the key, the rest of it (UninstallString ...) stays, as in a real upgrade.
function Seed-Key([string]$Tasks) {
    if (-not (Test-Path -LiteralPath $uninstKey)) { New-Item -Path $uninstKey -Force | Out-Null }
    New-ItemProperty -LiteralPath $uninstKey -Name 'Inno Setup: Selected Tasks' -Value $Tasks -PropertyType String -Force | Out-Null
}
function Setup-LogLines($c, [string]$Pattern) {
    $l = Join-Path $runRoot ('setup-' + $c.Name + '.log')
    if (Test-Path -LiteralPath $l) { @(Select-String -LiteralPath $l -Pattern $Pattern | ForEach-Object { $_.Line.Trim() }) } else { @() }
}

try {
    # (a) a first install that stores cleaninstall, then a plain silent upgrade must keep the data
    $a = New-Case 'a'
    Log '== (a) first install storing cleaninstall, then plain silent upgrade'
    Check ((Run-Setup $a @('/TASKS="cleaninstall"')) -eq 0) '(a) first install with /TASKS="cleaninstall" succeeds'
    $stored = Selected-Tasks
    Log ('  Selected Tasks after first install: ' + $stored)
    # Which Inno step first sees the stored task list is not documented; the installer logs what ssPostInstall and ssDone found.
    foreach ($l in (Setup-LogLines $a 'ssPostInstall:|ssDone:')) { Log ('  setup log: ' + $l) }
    # (With the interactive/silent page reset in place the task is deselected before Inno stores it, so the strip normally finds nothing to
    # clean; the strip itself is exercised by the -MutationCheck mutant that removes the reset.)
    Check (($null -ne $stored) -and ($stored -notmatch 'cleaninstall')) '(a) the stored "Selected Tasks" no longer carries cleaninstall after the install'
    # Whatever the installer stored, the upgrade below must start from a key that holds it (the owner's state, written by an OLDER installer), so
    # write that value by hand: only the one value, the rest of the key the first install wrote stays.
    Seed-Key 'cleaninstall'
    Check ((Selected-Tasks) -match 'cleaninstall') '(a) precondition: the key remembers cleaninstall before the upgrade'
    Seed $a
    Check ((Run-Setup $a @('/MERGETASKS="!desktopicon"')) -eq 0) '(a) plain silent upgrade succeeds'
    Check (Data-Kept $a) '(a) the remembered cleaninstall did NOT wipe the data on a plain silent upgrade'
    Check (Bystanders-Kept $a) '(a) no other folder touched'
    Run-Uninstall $a; Reset-Key

    # (b) the owner's exact registry state, then a silent install
    $b = New-Case 'b'
    Log '== (b) key seeded with "desktopicon,cleaninstall"'
    Seed-Key 'desktopicon,cleaninstall'; Seed $b
    Check ((Selected-Tasks) -eq 'desktopicon,cleaninstall') '(b) precondition: the key holds exactly desktopicon,cleaninstall'
    Check ((Run-Setup $b @('/MERGETASKS="!desktopicon"')) -eq 0) '(b) silent install over the seeded key succeeds'
    Check (Data-Kept $b) '(b) the owner state (desktopicon,cleaninstall) did NOT wipe the data'
    Check (Bystanders-Kept $b) '(b) no other folder touched'
    Run-Uninstall $b; Reset-Key

    # (c) /CLEANINSTALL=1 wipes both roots and nothing else
    $c = New-Case 'c'
    Log '== (c) /CLEANINSTALL=1'
    Reset-Key; Seed $c
    Check ((Run-Setup $c @('/CLEANINSTALL=1', '/MERGETASKS="!desktopicon"')) -eq 0) '(c) install with /CLEANINSTALL=1 succeeds'
    Check (Data-Gone $c) '(c) /CLEANINSTALL=1 wiped both data roots'
    Check (Bystanders-Kept $c) '(c) no other folder touched (sibling folders, recordings)'
    Run-Uninstall $c; Reset-Key

    # (d) a plain fresh install keeps the data
    $d = New-Case 'd'
    Log '== (d) plain fresh install'
    Reset-Key; Seed $d
    Check ((Run-Setup $d @('/MERGETASKS="!desktopicon"')) -eq 0) '(d) plain fresh install succeeds'
    Check (Data-Kept $d) '(d) a plain fresh install keeps the data'
    Check (Bystanders-Kept $d) '(d) no other folder touched'
    Check (Test-Path -LiteralPath $testProtoKey) '(d) the install wrote the RENAMED protocol key (so the real foxsdr: key was not the one written)'
    Run-Uninstall $d; Reset-Key
    Check (-not (Test-Path -LiteralPath $testProtoKey)) '(d) the uninstaller removed the renamed protocol key'

    # (f) explicit task switches in a silent run never wipe
    foreach ($sw in @(@('f1', '/TASKS="cleaninstall"'), @('f2', '/MERGETASKS="cleaninstall"'))) {
        $f = New-Case $sw[0]
        Log ('== (' + $sw[0] + ') silent ' + $sw[1])
        Reset-Key; Seed $f
        Check ((Run-Setup $f @($sw[1])) -eq 0) ('(' + $sw[0] + ') silent install with ' + $sw[1] + ' succeeds')
        Check (Data-Kept $f) ('(' + $sw[0] + ') silent ' + $sw[1] + ' does not wipe (only /CLEANINSTALL=1 does)')
        $storedF = Selected-Tasks
        Check (($null -ne $storedF) -and ($storedF -notmatch 'cleaninstall')) ('(' + $sw[0] + ') the stored task list does not carry cleaninstall after the run (stored: ' + $storedF + ')')
        Run-Uninstall $f; Reset-Key
    }

    # (e) the interactive wizard, driven through plain Win32 messages (Inno's controls are Win32; UI Automation sees no patterns on them). The box
    # state is READ from the rendered check list (checked boxes are the saturated-colour ones in its left margin) and, after a full run, from the
    # "Selected Tasks" Inno writes to the key.
    if (-not $Interactive) { Skip '(e) interactive tasks page: not run (opt-in with -Interactive; it raises setup windows on the desktop)' } else {
        Add-Type -AssemblyName System.Drawing
        Add-Type @"
using System; using System.Text; using System.Runtime.InteropServices; using System.Collections.Generic;
public class W32 {
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc p, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr h, EnumProc p, IntPtr l);
  [DllImport("user32.dll")] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
  [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  public static List<IntPtr> Tops(uint pid) { var r = new List<IntPtr>(); EnumWindows((h, l) => { uint p; GetWindowThreadProcessId(h, out p); if (p == pid && IsWindowVisible(h)) r.Add(h); return true; }, IntPtr.Zero); return r; }
  public static List<IntPtr> Kids(IntPtr top) { var r = new List<IntPtr>(); EnumChildWindows(top, (h, l) => { r.Add(h); return true; }, IntPtr.Zero); return r; }
  public static string Cls(IntPtr h) { var s = new StringBuilder(256); GetClassName(h, s, 256); return s.ToString(); }
  public static string Txt(IntPtr h) { var s = new StringBuilder(512); GetWindowText(h, s, 512); return s.ToString(); }
}
"@
        # The wizard's process is the "test-setup.tmp" child that the "test-setup.exe" bootstrapper starts; find the window of either.
        function Get-SetupProcs { @(Get-Process -Name 'test-setup', 'test-setup.tmp' -ErrorAction SilentlyContinue) }
        function Get-Form {
            foreach ($pr in (Get-SetupProcs)) {
                foreach ($t in [W32]::Tops([uint32]$pr.Id)) { if ([W32]::Cls($t) -eq 'TWizardForm') { return $t } }
            }
            return [IntPtr]::Zero
        }
        function Get-Kid([IntPtr]$Form, [string]$Class, [string]$TextPattern) {
            foreach ($k in [W32]::Kids($Form)) {
                if (([W32]::Cls($k) -eq $Class) -and [W32]::IsWindowVisible($k) -and ([W32]::Txt($k) -match $TextPattern)) { return $k }
            }
            return [IntPtr]::Zero
        }
        function Click-Win([IntPtr]$H) { [void][W32]::SendMessage($H, 0xF5, [IntPtr]::Zero, [IntPtr]::Zero) }   # BM_CLICK
        # Row positions (fraction of the list height) of the CHECKED boxes in a TNewCheckListBox, read from its pixels; the picture is kept.
        function Get-CheckedRows([IntPtr]$List) {
            # Topmost, so nothing on the owner's desktop can be in front of it when the screen is copied (SetForegroundWindow alone is refused).
            $form = Get-Form
            if ($form -ne [IntPtr]::Zero) { [void][W32]::SetWindowPos($form, [IntPtr](-1), 0, 0, 0, 0, 0x0043) }
            Start-Sleep -Milliseconds 500
            $r = New-Object W32+RECT; [void][W32]::GetWindowRect($List, [ref]$r)
            $w = $r.R - $r.L; $h = $r.B - $r.T
            $bmp = New-Object System.Drawing.Bitmap $w, $h
            $g = [System.Drawing.Graphics]::FromImage($bmp); $g.CopyFromScreen($r.L, $r.T, 0, 0, $bmp.Size); $g.Dispose()
            $rows = New-Object System.Collections.ArrayList
            for ($y = 0; $y -lt $h; $y++) {
                $hit = $false
                for ($x = 0; $x -lt [Math]::Min(22, $w); $x++) {
                    $px = $bmp.GetPixel($x, $y)
                    if (([Math]::Max($px.R, [Math]::Max($px.G, $px.B)) - [Math]::Min($px.R, [Math]::Min($px.G, $px.B))) -gt 90) { $hit = $true; break }
                }
                if ($hit) { [void]$rows.Add($y) }
            }
            $bmp.Save((Join-Path $runRoot ('checklist-' + [guid]::NewGuid().ToString('N').Substring(0, 6) + '.png'))); $bmp.Dispose()
            $clusters = @(); $start = $null; $prev = $null
            foreach ($y in $rows) {
                if ($null -eq $start) { $start = $y } elseif ($y -ne $prev + 1) { $clusters += ((($start + $prev) / 2.0) / $h); $start = $y }
                $prev = $y
            }
            if ($null -ne $start) { $clusters += ((($start + $prev) / 2.0) / $h) }
            return , $clusters
        }
        function Toggle-Item([IntPtr]$List, [int]$Index) {
            [void][W32]::SendMessage($List, 0x186, [IntPtr]$Index, [IntPtr]::Zero)      # LB_SETCURSEL
            [void][W32]::SendMessage($List, 0x100, [IntPtr]0x20, [IntPtr]::Zero)        # WM_KEYDOWN VK_SPACE
            [void][W32]::SendMessage($List, 0x101, [IntPtr]0x20, [IntPtr]0xC0000001)    # WM_KEYUP
            Start-Sleep -Milliseconds 400
        }
        # Starts the wizard and clicks Next until the tasks page (a visible check list beside a Next button). Returns the process and the list.
        function Open-TasksPage($c) {
            $p = Start-Process -FilePath $setup -ArgumentList (Setup-Args $c @()) -PassThru
            $res = [pscustomobject]@{ Process = $p; List = [IntPtr]::Zero }
            $deadline = (Get-Date).AddSeconds(60)
            while ((Get-Date) -lt $deadline) {
                Start-Sleep -Milliseconds 600
                $form = Get-Form; if ($form -eq [IntPtr]::Zero) { continue }
                $list = Get-Kid $form 'TNewCheckListBox' '.*'
                if ($list -ne [IntPtr]::Zero) { $res.List = $list; return $res }
                $acc = Get-Kid $form 'TNewRadioButton' 'accept'; if ($acc -ne [IntPtr]::Zero) { Click-Win $acc }
                $next = Get-Kid $form 'TNewButton' 'Next'; if ($next -ne [IntPtr]::Zero) { Click-Win $next }
            }
            return $res
        }
        function Close-Wizard($page) {
            try {
                $form = Get-Form
                if ($form -ne [IntPtr]::Zero) {
                    $cancel = Get-Kid $form 'TNewButton' 'Cancel'; if ($cancel -ne [IntPtr]::Zero) { Click-Win $cancel; Start-Sleep -Milliseconds 800 }
                    foreach ($pr in (Get-SetupProcs)) {
                        foreach ($t in [W32]::Tops([uint32]$pr.Id)) { if ([W32]::Cls($t) -eq '#32770') { foreach ($k in [W32]::Kids($t)) { if ([W32]::Txt($k) -match '^&?Yes') { Click-Win $k } } } }
                    }
                }
            } catch { Log ('  close wizard: ' + $_) }
            Start-Sleep -Milliseconds 800
            foreach ($pr in (Get-SetupProcs)) { if (-not $pr.WaitForExit(8000)) { $pr.Kill() } }
        }

        # (e1) the key holds cleaninstall; the tasks page must show the box UNticked (the desktop icon, also remembered, is the control: ticked)
        $e1 = New-Case 'e1'
        Log '== (e1) interactive: key holds desktopicon,cleaninstall; the box must start UNticked'
        Seed-Key 'desktopicon,cleaninstall'; Seed $e1
        $page = Open-TasksPage $e1
        if ($page.List -eq [IntPtr]::Zero) { Skip '(e1) interactive: could not reach the tasks page'; Close-Wizard $page } else {
            $rows = Get-CheckedRows $page.List
            Log ('  checked-box rows (fraction of list height): ' + (($rows | ForEach-Object { '{0:N2}' -f $_ }) -join ', '))
            Check (($rows.Count -eq 1) -and ($rows[0] -lt 0.3)) '(e1) interactive: tasks page shows the desktop icon ticked and the clean-install box UNticked although the key holds cleaninstall'
            Close-Wizard $page
            Check (Data-Kept $e1) '(e1) interactive: cancelling at the tasks page leaves the data'
        }
        Reset-Key

        # (e2) leave the box alone and run to the end: no wipe; (e3) tick it in this run: wipe
        foreach ($mode in @('e2', 'e3')) {
            $ec = New-Case $mode
            Log ('== (' + $mode + ') interactive full run, ' + $(if ($mode -eq 'e3') { 'box TICKED by the user' } else { 'box left alone' }))
            Seed-Key 'desktopicon,cleaninstall'; Seed $ec
            $page = Open-TasksPage $ec
            if ($page.List -eq [IntPtr]::Zero) { Skip ('(' + $mode + ') interactive: could not reach the tasks page'); Close-Wizard $page; continue }
            if ($mode -eq 'e3') {
                Toggle-Item $page.List 3
                $rows = Get-CheckedRows $page.List
                Log ('  after ticking: rows ' + (($rows | ForEach-Object { '{0:N2}' -f $_ }) -join ', '))
                if ($rows.Count -ne 2) { Skip '(e3) interactive: could not tick the box'; Close-Wizard $page; continue }
            }
            $done = $false; $deadline = (Get-Date).AddSeconds(150)
            while ((Get-Date) -lt $deadline -and -not $page.Process.HasExited -and -not $done) {
                Start-Sleep -Milliseconds 800
                $form = Get-Form; if ($form -eq [IntPtr]::Zero) { continue }
                # The finish page offers "Launch FoxSDR now", ticked. NEVER press Finish: that would start the scratch exe against the REAL profile
                # (/DATAROOTS only moves the installer's roots). By the finish page the files, the wipe and the key are all done, so the wizard is
                # simply ended there.
                $fin = Get-Kid $form 'TNewButton' 'Finish'
                if ($fin -ne [IntPtr]::Zero) { $done = $true; continue }
                foreach ($pat in 'Next', 'Install') { $go = Get-Kid $form 'TNewButton' $pat; if ($go -ne [IntPtr]::Zero) { Click-Win $go; break } }
            }
            foreach ($pr in (Get-SetupProcs)) { $pr.Kill() }   # the finish page is never confirmed (see above); this ends the wizard
            foreach ($pr in (Get-SetupProcs)) { [void]$pr.WaitForExit(10000) }
            $ec.Installed = (Test-Path -LiteralPath (Join-Path $ec.Dir 'unins000.exe'))
            $stored = Selected-Tasks
            Log ('  Selected Tasks after the interactive run: ' + $stored + '; installed=' + $ec.Installed)
            if (-not $ec.Installed) { Skip ('(' + $mode + ') interactive: the wizard did not complete an install'); Reset-Key; continue }
            if ($mode -eq 'e3') {
                Check (Data-Gone $ec) '(e3) interactive: the box ticked in THIS run wiped both roots'
                Check (($null -ne $stored) -and ($stored -notmatch 'cleaninstall')) '(e3) interactive: the tick that wiped is NOT left in the stored task list for the next install'
            } else {
                Check (Data-Kept $ec) '(e2) interactive: the box left alone (key remembered cleaninstall) kept the data'
                Check (($null -ne $stored) -and ($stored -notmatch 'cleaninstall')) '(e2) interactive: the installed task set no longer carries cleaninstall'
            }
            Check (Bystanders-Kept $ec) ('(' + $mode + ') no other folder touched')
            Run-Uninstall $ec; Reset-Key
        }
    }
} finally {
    # cleanup: uninstall every test install (always with /DATAROOTS), drop the test key, and the protocol key only if this run created it
    foreach ($c in $script:installs) { try { Run-Uninstall $c } catch { Log ('cleanup uninstall ' + $c.Name + ': ' + $_) } }
    Reset-Key
    if (Test-Path -LiteralPath $testProtoKey) { Remove-Item -LiteralPath $testProtoKey -Recurse -Force }   # only ever the RENAMED key; the real foxsdr: key is never removed
    $sm = Join-Path $env:APPDATA ('Microsoft\Windows\Start Menu\Programs\' + $groupName)
    if (Test-Path -LiteralPath $sm) { Remove-Item -LiteralPath $sm -Recurse -Force }
}

$realAfter = Get-RealConfigState
Log ('real config after:  ' + $realAfter)
Check ($realBefore -eq $realAfter) ('the real %APPDATA%\foxsdr\config.json is unchanged (' + $realBefore + ')')
Check (-not (Test-Path -LiteralPath $uninstKey)) 'no test uninstall key left in HKCU'
Check ((Test-Path 'HKCU:\Software\Classes\foxsdr') -eq $protocolKeyExisted) 'the foxsdr: protocol key is as it was before the run'
Check (-not (Test-Path -LiteralPath $testProtoKey)) 'no renamed test protocol key left in HKCU'
$realStateAfter = Get-RealState
foreach ($k in $realStateBefore.Keys) {
    Check ($realStateBefore[$k] -eq $realStateAfter[$k]) ('real state unchanged: ' + $k)
    if ($realStateBefore[$k] -ne $realStateAfter[$k]) { Log ('  before: ' + $realStateBefore[$k]); Log ('  after:  ' + $realStateAfter[$k]) }
}
$summary = ('{0} checks, {1} failed, {2} skipped; log {3}' -f $script:checks, $script:failed, $script:skipped, $logPath)
Log $summary; Write-Host $summary
if ($script:failed -gt 0) { exit 1 }
exit 0
