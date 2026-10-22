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
        [IO.File]::WriteAllText((Join-Path $verify 'symbols.json'), (@{ buildId = $winId; indexRow = ("2026-10-22 12:00:00`tcascade.exe`t0.99.84`tabc`t" + $winId + "`tcascade.pdb"); files = $files } | ConvertTo-Json -Depth 5), (New-Object Text.UTF8Encoding($false)))
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
            'symbols-no-engine-row' { if ($errorText -ne 'RELEASE-FAIL: index row for foxsdr-engine in foxsdr-linux-x64-symbols') { $problem = 'expected a refusal naming the missing engine row, got: ' + $errorText } }
            'symbols-no-cascade-row' { if ($errorText -ne 'RELEASE-FAIL: index row for cascade in foxsdr-linux-x64-symbols') { $problem = 'expected a refusal naming the missing cascade row, got: ' + $errorText } }
            'symbols-bad-row' { if ($errorText -notlike 'RELEASE-FAIL: index row of foxsdr-linux-x64-symbols*') { $problem = 'expected a refusal of the malformed row, got: ' + $errorText } }
            'symbols-map-mismatch' { if ($errorText -ne 'RELEASE-FAIL: cascade map matches its index row in foxsdr-linux-x64-symbols') { $problem = 'expected a refusal of a map for another build, got: ' + $errorText } }
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
