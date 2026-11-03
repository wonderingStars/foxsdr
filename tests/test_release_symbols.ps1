# Fake-only regression suite for what engine extraction stage 7g changed in tools/release.ps1:
# the `symbols` step with TWO modules in each Linux symbols artifact, and the read-back of the
# Linux tarballs in the `ci` step. Definitions are AST-extracted (as tests/test_release_install.ps1
# does), never the release script's main body; every network and remote seam is a recording fake,
# and the tarballs are REAL gzip tars made here and listed by the real tar.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File tests\test_release_symbols.ps1
#   ... -SourcePath <a copy of release.ps1>     (the mutation check: run it against the OLD script)
#
# What it holds:
#   symbols-two-modules   each Linux artifact has cascade AND foxsdr-engine, an index.txt with two rows and two
#                         symmap files. Every build id reaches the NAS index (five rows: Windows, and two per
#                         artifact), every module file reaches the NAS, and the Pi gets ONLY cascade's maps, each
#                         named for its own build id (never the engine's id on a cascade name, never the engine's map).
#   symbols-no-engine-row the artifact's index has no foxsdr-engine row (the engine was not archived): refused.
#   symbols-no-cascade-row, symbols-bad-row, symbols-map-mismatch   the same for the other broken shapes.
#   symbols-slint-windows the Windows record (verify\symbols.json) carries the Slint window beside cascade: its own build id,
#                         index row and files (pdb, exe, symmap) under `modules`; cascade keeps the top-level keys. Both ids
#                         reach the NAS index, every file reaches the NAS, and the Pi gets cascade-<win id>.json.gz AND
#                         foxsdr-slint-<slint id>.json.gz (never the Slint map under cascade's name or id).
#   symbols-slint-from-builder   the same, with the record made by New-WindowsSymbolsRecord (what the build step writes).
#   symbols-slint-map-mismatch   the record names one build id for foxsdr-slint but the map's own path is another: refused.
#   build-slint-record    New-WindowsSymbolsRecord through Write-JsonFile / Read-JsonFile: the top-level buildId / indexRow / files
#                         are exactly cascade's (tools\site-release.ps1 names every map in `files` cascade-<id>), the Slint
#                         window's id, row and files are under `modules`.
#   build-slint-ok / build-slint-wrong-pdb / build-slint-same-id / build-slint-no-index-row / build-slint-no-map
#                         the build step's archive of foxsdr-slint.exe (Invoke-ModuleSymbolArchive, driven by a fake
#                         archive-symbols.ps1): the good run is accepted and recorded with its files, hashes and index row; a
#                         PDB archived under another name, a build id equal to cascade's, a missing index row and a missing
#                         symbol map are each refused for that thing.
#   tarball-ok / tarball-no-engine / tarball-no-cascade / tarball-no-plan / tarball-no-notice   Assert-LinuxTarballContents
#                         against real archives: the good one is accepted, each lacking one thing is refused for that thing.
param(
    [string]$SourcePath = (Join-Path $PSScriptRoot '..\tools\release.ps1'),
    [string[]]$Cases = @()
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0
$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$runRoot = Join-Path $repoRoot ('build\release-symbols-test\fake-' + $PID + '-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $runRoot -Force | Out-Null
foreach ($source in @((Join-Path $repoRoot 'tools\release-common.ps1'), $SourcePath)) {
    $tokens = $null; $errors = $null
    $ast = [Management.Automation.Language.Parser]::ParseFile($source, [ref]$tokens, [ref]$errors)
    if ($errors.Count) { throw ($errors | Out-String) }
    foreach ($def in $ast.FindAll({param($n) $n -is [Management.Automation.Language.FunctionDefinitionAst]}, $false)) {
        . ([scriptblock]::Create($def.Extent.Text))
    }
}
$hasTarballCheck = [bool](Get-Command Assert-LinuxTarballContents -ErrorAction SilentlyContinue)
$hasModuleArchive = [bool](Get-Command Invoke-ModuleSymbolArchive -ErrorAction SilentlyContinue)
$hasRecordBuilder = [bool](Get-Command New-WindowsSymbolsRecord -ErrorAction SilentlyContinue)

# ---- the seams: nothing real is reached --------------------------------------------------------------------------
function Write-Result { param($Level, $Name, $Value = '') [void]$script:Messages.Add(($Level + ' ' + $Name + ' : ' + $Value)) }
function Write-Detail { param($Text) [void]$script:Messages.Add([string]$Text) }
function Test-StepDone { return $false }
function Assert-StepDone {}
function Set-StepDone { $script:Done = $true }
function Send-RemoteFile { param($HostName, $LocalPath, $RemotePath, $Mode, $Owner, [switch]$MakeDirectory, [switch]$ReplaceExisting)
    [void]$script:Sent.Add(@{ Host = $HostName; Remote = $RemotePath })
}
function Add-NasIndexRow { param($NasHost, $Root, $BuildId, $Row, $TempDir) [void]$script:Rows.Add($BuildId) }
function Invoke-Ssh { param($HostName, $Command, $InputFile) return @{ ExitCode = 0; StdOut = 'ok'; Output = 'ok' } }
# the one runner the tarball check needs: really runs the (real) tar
function Invoke-Exe { param($File, $Arguments, $TimeoutSec, $Env, $WorkDir, $InputFile, $Label, $HeartbeatSec, [switch]$NoLog)
    $psi = New-Object Diagnostics.ProcessStartInfo
    $psi.FileName = $File
    $psi.Arguments = (($Arguments | ForEach-Object { '"' + $_ + '"' }) -join ' ')
    $psi.RedirectStandardOutput = $true; $psi.RedirectStandardError = $true; $psi.UseShellExecute = $false
    $p = [Diagnostics.Process]::Start($psi)
    $out = $p.StandardOutput.ReadToEnd(); $err = $p.StandardError.ReadToEnd(); $p.WaitForExit()
    return @{ ExitCode = $p.ExitCode; StdOut = $out; Output = ($out + $err); TimedOut = $false }
}
function Put-Text([string]$Path, [string]$Text) {
    New-Item -ItemType Directory -Path ([IO.Path]::GetDirectoryName($Path)) -Force | Out-Null
    [IO.File]::WriteAllText($Path, $Text, (New-Object Text.UTF8Encoding($false)))
}

# ---- a fake CI symbols artifact -----------------------------------------------------------------------------------
function New-FakeSymbolsArtifact([string]$Dir, [string]$Arch, [bool]$Cascade, [bool]$Engine, [bool]$BadRow = $false, [bool]$MapMismatch = $false) {
    $ids = @{ cascade = ('c' + $Arch).PadRight(40, 'a'); 'foxsdr-engine' = ('e' + $Arch).PadRight(40, 'b') }
    $rows = @()
    foreach ($m in @('cascade', 'foxsdr-engine')) {
        if ($m -eq 'cascade' -and -not $Cascade) { continue }
        if ($m -eq 'foxsdr-engine' -and -not $Engine) { continue }
        $id = $ids[$m]
        Put-Text (Join-Path $Dir ($m + '.debug\' + $id + '\' + $m + '.debug')) 'debug'
        Put-Text (Join-Path $Dir ($m + '\' + $id + '\' + $m)) 'module'
        $mapId = $id
        if ($MapMismatch -and $m -eq 'cascade') { $mapId = ('z' + $Arch).PadRight(40, 'z') }
        Put-Text (Join-Path $Dir ($m + '.debug\' + $mapId + '\symmap.json.gz')) 'map'
        $rows += ("2026-10-22 12:00:00`t" + $m + "`t0.99.84`tabc1234`t" + $id + "`t" + $m + '.debug')
    }
    if ($BadRow) { $rows = @("2026-10-22 12:00:00`tcascade`t0.99.84") }
    Put-Text (Join-Path $Dir 'index.txt') (($rows -join "`r`n") + "`r`n")
    return $ids
}

# ---- a fake tools\archive-symbols.ps1 -----------------------------------------------------------------------------
# Writes what the real one writes for a binary (pdb, exe, optionally the map, the index) into -ArchiveRoot and emits the
# same relative paths in the same order. $Mode picks the one thing that is wrong, if any.
function New-FakeArchiveTool([string]$Path, [string]$Mode, [string]$Module, [string]$Id) {
    $pdb = $Module + '.pdb'
    if ($Mode -eq 'wrong-pdb') { $pdb = 'cascade.pdb' }
    $map = if ($Mode -eq 'no-map') { '0' } else { '1' }
    $row = if ($Mode -eq 'no-index-row') { '0' } else { '1' }
    $body = @'
param([string]$Binary, [string]$ArchiveRoot, [string]$Version = '', [string]$CommitHeader = '', [switch]$EmitPaths)
function Put([string]$Rel, [string]$Text) { $p = Join-Path $ArchiveRoot $Rel; New-Item -ItemType Directory -Path (Split-Path -Parent $p) -Force | Out-Null; [IO.File]::WriteAllText($p, $Text) }
$pdb = '@PDB@'; $id = '@ID@'; $mod = '@MOD@.exe'
Put ($pdb + '\' + $id + '\' + $pdb) 'pdb'
Put ($mod + '\6AE970901433000\' + $mod) 'exe'
$paths = @(($pdb + '\' + $id + '\' + $pdb), ($mod + '\6AE970901433000\' + $mod))
if ('@MAP@' -eq '1') { Put ($pdb + '\' + $id + '\symmap.json.gz') 'map'; $paths += ($pdb + '\' + $id + '\symmap.json.gz') }
$idx = Join-Path $ArchiveRoot 'index.txt'
$text = if (Test-Path -LiteralPath $idx) { [IO.File]::ReadAllText($idx) } else { "2026-11-03 09:00:00`tcascade.exe`t0.99.104`tabc1234`tCASCADEID`tcascade.pdb`r`n" }
if ('@ROW@' -eq '1') { $text += "2026-11-03 09:01:00`t" + $mod + "`t" + $Version + "`tabc1234`t" + $id + "`t" + $pdb + "`r`n" }
[IO.File]::WriteAllText($idx, $text)
$paths += 'index.txt'
if ($EmitPaths) { $paths }
'@
    $body = $body.Replace('@PDB@', $pdb).Replace('@ID@', $Id).Replace('@MOD@', $Module).Replace('@MAP@', $map).Replace('@ROW@', $row)
    Put-Text $Path $body
}

# ---- a real tarball ---------------------------------------------------------------------------------------------
function New-FakeTarball([string]$Root, [string]$Top, [string[]]$Omit) {
    $stage = Join-Path $Root 'stage'
    if (Test-Path -LiteralPath $stage) { Remove-Item -Recurse -Force $stage }
    foreach ($f in @('cascade', 'foxsdr-engine', 'README.md', 'LICENSE', 'PRIVACY.md', 'resources\bandplans\uk.json', 'resources\firmware\aor\NOTICE')) {
        if ($Omit -contains $f) { continue }
        Put-Text (Join-Path $stage ($Top + '\' + $f)) 'x'
    }
    $tarball = Join-Path $Root ($Top + '.tar.gz')
    if (Test-Path -LiteralPath $tarball) { Remove-Item -Force $tarball }
    $tar = (Get-Command tar -ErrorAction Stop).Source
    & $tar -czf $tarball -C $stage $Top
    if ($LASTEXITCODE -ne 0) { throw 'tar could not make the fake tarball' }
    return $tarball
}

$allCases = @('symbols-two-modules', 'symbols-no-engine-row', 'symbols-no-cascade-row', 'symbols-bad-row', 'symbols-map-mismatch',
              'symbols-slint-windows', 'symbols-slint-from-builder', 'symbols-slint-map-mismatch',
              'build-slint-record', 'build-slint-ok', 'build-slint-wrong-pdb', 'build-slint-same-id', 'build-slint-no-index-row', 'build-slint-no-map',
              'tarball-ok', 'tarball-no-engine', 'tarball-no-cascade', 'tarball-no-plan', 'tarball-no-notice')
if ($Cases.Count) {
    $selected = @($Cases | ForEach-Object { $_ -split ',' })
    $allCases = @($allCases | Where-Object { $selected -contains $_ })
}
if (-not $allCases.Count) { throw 'no selected cases' }
$failed = 0
foreach ($name in $allCases) {
    $caseRoot = Join-Path $runRoot $name
    $script:Messages = New-Object Collections.ArrayList
    $script:Sent = New-Object Collections.ArrayList
    $script:Rows = New-Object Collections.ArrayList
    $script:Done = $false
    $problem = ''
    $errorText = ''
    if ($name -like 'symbols-*') {
        $ci = Join-Path $caseRoot 'ci'
        $verify = Join-Path $caseRoot 'verify'
        New-Item -ItemType Directory -Path $verify -Force | Out-Null
        $cascade = $name -ne 'symbols-no-cascade-row'
        $engine = $name -ne 'symbols-no-engine-row'
        $idsX64 = New-FakeSymbolsArtifact (Join-Path $ci 'foxsdr-linux-x64-symbols') 'x64' $cascade $engine ($name -eq 'symbols-bad-row') ($name -eq 'symbols-map-mismatch')
        $idsArm = New-FakeSymbolsArtifact (Join-Path $ci 'foxsdr-linux-arm64-symbols') 'arm64' $true $true
        # what the build step wrote for Windows: one build id, its pdb, its map
        $winId = 'W' * 33
        Put-Text (Join-Path $caseRoot ('repo\symbols\cascade.pdb\' + $winId + '\symmap.json.gz')) 'map'
        Put-Text (Join-Path $caseRoot ('repo\symbols\cascade.pdb\' + $winId + '\cascade.pdb')) 'pdb'
        $files = @{}
        $files['cascade.pdb\' + $winId + '\cascade.pdb'] = 'h1'
        $files['cascade.pdb\' + $winId + '\symmap.json.gz'] = 'h2'
        $record = @{ buildId = $winId; indexRow = ("2026-10-22 12:00:00`tcascade.exe`t0.99.84`tabc`t" + $winId + "`tcascade.pdb"); files = $files }
        # the Slint window beside cascade: its own build id, pdb, exe and map, and the per-module map of ids
        $slintId = 'S' * 33
        if ($name -like 'symbols-slint-*') {
            Put-Text (Join-Path $caseRoot ('repo\symbols\foxsdr-slint.pdb\' + $slintId + '\foxsdr-slint.pdb')) 'pdb'
            Put-Text (Join-Path $caseRoot ('repo\symbols\foxsdr-slint.pdb\' + $slintId + '\symmap.json.gz')) 'map'
            Put-Text (Join-Path $caseRoot 'repo\symbols\foxsdr-slint.exe\6AE970901433000\foxsdr-slint.exe') 'exe'
            # top-level `files` stays cascade's (tools\site-release.ps1 names every map in it cascade-<buildId>); the Slint
            # window's files are under its own entry in `modules`
            $slintFiles = @{}
            $slintFiles['foxsdr-slint.pdb\' + $slintId + '\foxsdr-slint.pdb'] = 'h3'
            $slintFiles['foxsdr-slint.exe\6AE970901433000\foxsdr-slint.exe'] = 'h4'
            $slintFiles['foxsdr-slint.pdb\' + $slintId + '\symmap.json.gz'] = 'h5'
            $slintRecordedId = if ($name -eq 'symbols-slint-map-mismatch') { 'M' * 33 } else { $slintId }
            $record['modules'] = @{
                'cascade'      = @{ buildId = $winId; indexRow = $record['indexRow'] }
                'foxsdr-slint' = @{ buildId = $slintRecordedId; indexRow = ("2026-10-22 12:00:01`tfoxsdr-slint.exe`t0.99.84`tabc`t" + $slintRecordedId + "`tfoxsdr-slint.pdb"); files = $slintFiles }
            }
        }
        $recordProblem = ''
        if ($name -eq 'symbols-slint-from-builder') {
            # the record exactly as the build step writes it (New-WindowsSymbolsRecord -> Write-JsonFile), read by the symbols step
            if (-not $hasRecordBuilder) { $recordProblem = 'this release.ps1 has no New-WindowsSymbolsRecord' }
            else {
                $slintFiles = @{}
                $slintFiles['foxsdr-slint.pdb\' + $slintId + '\foxsdr-slint.pdb'] = 'h3'
                $slintFiles['foxsdr-slint.exe\6AE970901433000\foxsdr-slint.exe'] = 'h4'
                $slintFiles['foxsdr-slint.pdb\' + $slintId + '\symmap.json.gz'] = 'h5'
                Put-Text (Join-Path $caseRoot ('repo\symbols\foxsdr-slint.pdb\' + $slintId + '\symmap.json.gz')) 'map'
                $record = New-WindowsSymbolsRecord -BuildId $winId -IndexRow $record['indexRow'] -Files $files -Modules @{ 'foxsdr-slint' = @{ BuildId = $slintId; IndexRow = ("2026-10-22 12:00:01`tfoxsdr-slint.exe`t0.99.84`tabc`t" + $slintId + "`tfoxsdr-slint.pdb"); Files = $slintFiles } }
            }
        }
        [IO.File]::WriteAllText((Join-Path $verify 'symbols.json'), ($record | ConvertTo-Json -Depth 6), (New-Object Text.UTF8Encoding($false)))
        $script:RC = @{ Version = '0.99.84'; Ci = $ci; Verify = $verify; Repo = (Join-Path $caseRoot 'repo') }
        $script:Cfg = @{ nasHost = 'fake-nas'; nasSymbolRoot = '/fake/symbols'; piHost = 'fake-pi'; piSymbolDir = '/fake/pi-symbols' }
        $script:Ctx = @{ DryRun = $false; LogPath = ''; Secrets = (New-Object Collections.ArrayList) }
        $Force = $true
        try { $null = Step-Symbols } catch { $errorText = $_.Exception.Message }
        $pi = @($script:Sent | Where-Object { $_.Host -eq 'fake-pi' } | ForEach-Object { $_.Remote })
        $nas = @($script:Sent | Where-Object { $_.Host -eq 'fake-nas' } | ForEach-Object { $_.Remote })
        switch ($name) {
            'symbols-two-modules' {
                if ($errorText) { $problem = 'refused a good pair of artifacts: ' + $errorText }
                $want = @($winId, $idsX64['cascade'], $idsX64['foxsdr-engine'], $idsArm['cascade'], $idsArm['foxsdr-engine'])
                if (-not $problem -and (@($script:Rows | Sort-Object) -join ',') -ne (($want | Sort-Object) -join ',')) { $problem = 'NAS index rows were ' + ($script:Rows -join ', ') + ', wanted every build id: ' + ($want -join ', ') }
                $wantPi = @(('/fake/pi-symbols/cascade-' + $winId + '.json.gz'), ('/fake/pi-symbols/cascade-' + $idsX64['cascade'] + '.linux.json.gz'), ('/fake/pi-symbols/cascade-' + $idsArm['cascade'] + '.linux.json.gz'))
                if (-not $problem -and ($pi | Sort-Object) -join ',' -ne (($wantPi | Sort-Object) -join ',')) { $problem = 'Pi maps were ' + ($pi -join ', ') + ', wanted ' + ($wantPi -join ', ') }
                if (-not $problem -and @($nas | Where-Object { $_ -like '*foxsdr-engine.debug/*' }).Count -ne 4) { $problem = 'the NAS did not get both artifacts engine debug + map (' + (@($nas | Where-Object { $_ -like '*foxsdr-engine*' }) -join ', ') + ')' }
                if (-not $problem -and @($nas | Where-Object { $_ -like '*foxsdr-engine/*' -and $_ -notlike '*.debug/*' }).Count -ne 2) { $problem = 'the NAS did not get both engine modules' }
            }
            { $_ -in @('symbols-slint-windows', 'symbols-slint-from-builder') } {
                if ($recordProblem) { $problem = $recordProblem }
                if (-not $problem -and $errorText) { $problem = 'refused a Windows record carrying the Slint window: ' + $errorText }
                $want = @($winId, $slintId, $idsX64['cascade'], $idsX64['foxsdr-engine'], $idsArm['cascade'], $idsArm['foxsdr-engine'])
                if (-not $problem -and (@($script:Rows | Sort-Object) -join ',') -ne (($want | Sort-Object) -join ',')) { $problem = 'NAS index rows were ' + ($script:Rows -join ', ') + ', wanted every build id: ' + ($want -join ', ') }
                $wantPi = @(('/fake/pi-symbols/cascade-' + $winId + '.json.gz'), ('/fake/pi-symbols/foxsdr-slint-' + $slintId + '.json.gz'), ('/fake/pi-symbols/cascade-' + $idsX64['cascade'] + '.linux.json.gz'), ('/fake/pi-symbols/cascade-' + $idsArm['cascade'] + '.linux.json.gz'))
                if (-not $problem -and ($pi | Sort-Object) -join ',' -ne (($wantPi | Sort-Object) -join ',')) { $problem = 'Pi maps were ' + ($pi -join ', ') + ', wanted ' + ($wantPi -join ', ') }
                $wantNas = @(('/fake/symbols/foxsdr-slint.pdb/' + $slintId + '/foxsdr-slint.pdb'), '/fake/symbols/foxsdr-slint.exe/6AE970901433000/foxsdr-slint.exe', ('/fake/symbols/foxsdr-slint.pdb/' + $slintId + '/symmap.json.gz'))
                foreach ($w in $wantNas) { if (-not $problem -and $nas -notcontains $w) { $problem = 'the NAS did not get ' + $w } }
            }
            'symbols-slint-map-mismatch' { if ($errorText -ne 'RELEASE-FAIL: foxsdr-slint map matches its record in symbols.json') { $problem = 'expected a refusal of a Slint map for another build than the record names, got: ' + $errorText } }
            'symbols-no-engine-row' { if ($errorText -ne 'RELEASE-FAIL: index row for foxsdr-engine in foxsdr-linux-x64-symbols') { $problem = 'expected a refusal naming the missing engine row, got: ' + $errorText } }
            'symbols-no-cascade-row' { if ($errorText -ne 'RELEASE-FAIL: index row for cascade in foxsdr-linux-x64-symbols') { $problem = 'expected a refusal naming the missing cascade row, got: ' + $errorText } }
            'symbols-bad-row' { if ($errorText -notlike 'RELEASE-FAIL: index row of foxsdr-linux-x64-symbols*') { $problem = 'expected a refusal of the malformed row, got: ' + $errorText } }
            'symbols-map-mismatch' { if ($errorText -ne 'RELEASE-FAIL: cascade map matches its index row in foxsdr-linux-x64-symbols') { $problem = 'expected a refusal of a map for another build, got: ' + $errorText } }
        }
    } elseif ($name -eq 'build-slint-record') {
        # New-WindowsSymbolsRecord -> Write-JsonFile -> Read-JsonFile: cascade keeps the top-level keys alone (site-release.ps1
        # names every map in `files` cascade-<buildId>), the Slint window has its own entry under `modules`.
        New-Item -ItemType Directory -Path $caseRoot -Force | Out-Null
        if (-not $hasRecordBuilder) {
            $problem = 'this release.ps1 has no New-WindowsSymbolsRecord (the old build step writes cascade''s files only)'
        } else {
            $cId = 'C' * 33; $sId = 'S' * 33
            $cFiles = @{ ('cascade.pdb\' + $cId + '\cascade.pdb') = 'a'; ('cascade.pdb\' + $cId + '\symmap.json.gz') = 'b'; 'cascade.exe\6AE970901433000\cascade.exe' = 'c' }
            $sFiles = @{ ('foxsdr-slint.pdb\' + $sId + '\foxsdr-slint.pdb') = 'd'; ('foxsdr-slint.pdb\' + $sId + '\symmap.json.gz') = 'e'; 'foxsdr-slint.exe\6AE970901433000\foxsdr-slint.exe' = 'f' }
            $built = New-WindowsSymbolsRecord -BuildId $cId -IndexRow 'CROW' -Files $cFiles -Modules @{ 'foxsdr-slint' = @{ BuildId = $sId; IndexRow = 'SROW'; Files = $sFiles } }
            $jsonPath = Join-Path $caseRoot 'symbols.json'
            Write-JsonFile $jsonPath $built
            $back = Read-JsonFile $jsonPath
            if ($back['buildId'] -ne $cId -or $back['indexRow'] -ne 'CROW') { $problem = 'the top-level buildId / indexRow are not cascade''s: ' + $back['buildId'] + ' / ' + $back['indexRow'] }
            elseif ((@($back['files'].Keys | Sort-Object) -join '|') -ne (@($cFiles.Keys | Sort-Object) -join '|')) { $problem = 'the top-level files are not exactly cascade''s: ' + (@($back['files'].Keys) -join ', ') }
            elseif ($back['modules']['cascade']['buildId'] -ne $cId -or $back['modules']['foxsdr-slint']['buildId'] -ne $sId) { $problem = 'modules do not hold both build ids' }
            elseif ($back['modules']['foxsdr-slint']['indexRow'] -ne 'SROW') { $problem = 'the Slint index row is not in its entry' }
            elseif ((@($back['modules']['foxsdr-slint']['files'].Keys | Sort-Object) -join '|') -ne (@($sFiles.Keys | Sort-Object) -join '|')) { $problem = 'the Slint entry does not hold exactly the Slint files: ' + (@($back['modules']['foxsdr-slint']['files'].Keys) -join ', ') }
        }
    } elseif ($name -like 'build-slint-*') {
        New-Item -ItemType Directory -Path $caseRoot -Force | Out-Null
        $script:Ctx = @{ DryRun = $false; LogPath = ''; Secrets = (New-Object Collections.ArrayList) }
        if (-not $hasModuleArchive) {
            $problem = 'this release.ps1 has no Invoke-ModuleSymbolArchive (the old build step archives cascade.exe and foxsdr-engine.exe only)'
        } else {
            $cascadeId = 'C' * 33
            $slintId = 'S' * 33
            $mode = switch ($name) { 'build-slint-wrong-pdb' { 'wrong-pdb' } 'build-slint-same-id' { 'same-id' } 'build-slint-no-index-row' { 'no-index-row' } 'build-slint-no-map' { 'no-map' } default { 'ok' } }
            $archiveId = if ($mode -eq 'same-id') { $cascadeId } else { $slintId }
            $tool = Join-Path $caseRoot 'tools\archive-symbols.ps1'
            New-FakeArchiveTool $tool $mode 'foxsdr-slint' $archiveId
            $symRoot = Join-Path $caseRoot 'symbols'
            New-Item -ItemType Directory -Path $symRoot -Force | Out-Null
            Put-Text (Join-Path $symRoot 'index.txt') ("2026-11-03 09:00:00`tcascade.exe`t0.99.104`tabc1234`t" + $cascadeId + "`tcascade.pdb`r`n")
            $rec = $null
            try { $rec = Invoke-ModuleSymbolArchive -Module 'foxsdr-slint' -Binary (Join-Path $caseRoot 'foxsdr-slint.exe') -ArchiveTool $tool -SymRoot $symRoot -Version '0.99.104' -CommitHeader '' -DistinctFrom @{ cascade = $cascadeId; 'foxsdr-engine' = ('E' * 33) } } catch { $errorText = $_.Exception.Message }
            $wantErr = switch ($name) {
                'build-slint-wrong-pdb' { 'RELEASE-FAIL: foxsdr-slint PDB archived as foxsdr-slint.pdb' }
                'build-slint-same-id' { 'RELEASE-FAIL: foxsdr-slint build id is its own' }
                'build-slint-no-index-row' { 'RELEASE-FAIL: foxsdr-slint symbol index row' }
                'build-slint-no-map' { 'RELEASE-FAIL: foxsdr-slint symbols archived' }
                default { '' }
            }
            if ($errorText -ne $wantErr) { $problem = 'expected "' + $wantErr + '", got "' + $errorText + '"' }
            elseif ($name -eq 'build-slint-ok') {
                $wantFiles = @(('foxsdr-slint.pdb\' + $slintId + '\foxsdr-slint.pdb'), 'foxsdr-slint.exe\6AE970901433000\foxsdr-slint.exe', ('foxsdr-slint.pdb\' + $slintId + '\symmap.json.gz'))
                if ($null -eq $rec -or $rec['buildId'] -ne $slintId) { $problem = 'record build id was "' + $(if ($null -eq $rec) { '' } else { $rec['buildId'] }) + '", wanted ' + $slintId }
                elseif ((@($rec['files'].Keys | Sort-Object) -join '|') -ne (($wantFiles | Sort-Object) -join '|')) { $problem = 'record files were ' + (@($rec['files'].Keys) -join ', ') + ', wanted ' + ($wantFiles -join ', ') }
                elseif ($rec['files'][$wantFiles[0]] -ne (Get-Sha256 (Join-Path $symRoot $wantFiles[0]))) { $problem = 'the record holds a hash that is not the pdb''s sha256' }
                elseif (-not $rec['indexRow'].Contains("`t" + $slintId + "`t") -or $rec['indexRow'].Split("`t")[1] -ne 'foxsdr-slint.exe') { $problem = 'record index row was "' + $rec['indexRow'] + '"' }
            }
        }
    } else {
        New-Item -ItemType Directory -Path $caseRoot -Force | Out-Null
        $script:Ctx = @{ DryRun = $false; LogPath = ''; Secrets = (New-Object Collections.ArrayList) }
        if (-not $hasTarballCheck) {
            $problem = 'this release.ps1 has no Assert-LinuxTarballContents (the old script reads no tarball)'
        } else {
            $top = 'foxsdr-0.99.84-linux-x64'
            $omit = switch ($name) { 'tarball-no-engine' { @('foxsdr-engine') } 'tarball-no-cascade' { @('cascade') } 'tarball-no-plan' { @('resources\bandplans\uk.json') } 'tarball-no-notice' { @('resources\firmware\aor\NOTICE') } default { @() } }
            $tarball = New-FakeTarball $caseRoot $top $omit
            try { Assert-LinuxTarballContents $tarball ($top + '.tar.gz') } catch { $errorText = $_.Exception.Message }
            $wantErr = switch ($name) {
                'tarball-no-engine' { 'RELEASE-FAIL: tarball member ' + $top + '/foxsdr-engine' }
                'tarball-no-cascade' { 'RELEASE-FAIL: tarball member ' + $top + '/cascade' }
                'tarball-no-plan' { 'RELEASE-FAIL: tarball band plans in ' + $top + '.tar.gz' }
                'tarball-no-notice' { 'RELEASE-FAIL: tarball member ' + $top + '/resources/firmware/aor/NOTICE' }
                default { '' }
            }
            if ($errorText -ne $wantErr) { $problem = 'expected "' + $wantErr + '", got "' + $errorText + '"' }
        }
    }
    if ($problem) { $failed++; Write-Host ('FAIL ' + $name + ': ' + $problem); Write-Host (($script:Messages | Select-Object -Last 12) -join "`n") }
    else { Write-Host ('PASS ' + $name + $(if ($errorText) { ': refused as expected (' + $errorText + ')' } else { '' })) }
}
Write-Host ('release symbols and tarball fakes: ' + $allCases.Count + ' cases, ' + $failed + ' failed; PowerShell ' + $PSVersionTable.PSVersion + '; scratch ' + $runRoot)
if ($failed) { exit 1 }
exit 0
