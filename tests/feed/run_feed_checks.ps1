<#
.SYNOPSIS
  Re-runs the headless plugin feed checks (lane feedplan) on a fresh build, for example after plugins merge round 3.

.DESCRIPTION
  1. builds test_engine_api_feed_check and test_feed_fixtures in -BuildDir (cmake --build --parallel 3, never /m) unless -SkipBuild;
  2. runs the generator's self-test, writes the generated fixtures to <OutDir>\gen and compares their SHA256SUMS with tests\feed\SHA256SUMS;
  3. makes the 23 slint drive-set fixtures with the repository's own make_*.py (python -I, into <OutDir>\drive-fixtures);
  4. copies the NEWEST module of every plugin id from -PluginSrc (a built plugins repo: its out\Release folder) into <OutDir>\plugins;
  5. runs feed-cases-new.tsv, feed-cases-drive.tsv and, when present, feed-cases-real.tsv (LOCAL ONLY: it names real recordings; never committed) through the real engine;
  6. writes <OutDir>\RESULTS.md (one PASS / FAIL / SKIP row per case with the real output fragment) and exits 1 when any generated or drive-set row failed.

  Everything is headless: no window, no radio, no network, a hermetic profile (the real %APPDATA%\foxsdr\config.json is never read or written; its sha256 is printed before and after).
  Nothing here opens a certificate store, a file dialog or any other Windows prompt.

.PARAMETER BuildDir     The CMake build folder of this tree (configured with -DCASCADE_BUILD_SLINT=OFF -DCASCADE_SOFTGL=OFF; the Slint build works too).
.PARAMETER PluginSrc    Folder holding the built plugin modules (*-abi3-win-x64.dll), e.g. D:\foxsdr-build\plug-merge\out\Release.
.PARAMETER OutDir       Working and results folder, outside any repository.
.PARAMETER SamplesDir   The local real recordings (never copied into a repository).
.PARAMETER Only         Case id prefixes to run; default: every case.
#>
param(
    [string]$BuildDir = 'D:\foxsdr-build\en-feedplan',
    [string]$PluginSrc = 'D:\foxsdr-build\plug-merge\out\Release',
    [string]$OutDir = 'D:\foxsdr-build\prep\feed',
    [string]$SamplesDir = 'D:\foxsdr-samples',
    [string[]]$Only = @(),
    [switch]$SkipBuild
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$feedDir = Join-Path $repo 'tests\feed'
$realCfg = Join-Path $env:APPDATA 'foxsdr\config.json'
$shaBefore = if (Test-Path $realCfg) { (Get-FileHash $realCfg -Algorithm SHA256).Hash.ToLower() } else { 'absent' }
Write-Host "real config.json sha256 before: $shaBefore"

$free = (Get-PSDrive -Name ($OutDir.Substring(0, 1))).Free / 1GB
if ($free -lt 15) { throw "less than 15 GB free on the output drive ($([math]::Round($free,1)) GB): stopping" }
New-Item -ItemType Directory -Force -Path $OutDir, "$OutDir\gen", "$OutDir\drive-fixtures", "$OutDir\plugins" | Out-Null

if (-not $SkipBuild) {
    & cmake --build $BuildDir --config Release --target test_engine_api_feed_check test_feed_fixtures --parallel 3
    if ($LASTEXITCODE -ne 0) { throw 'build failed' }
}
$feedExe = Join-Path $BuildDir 'tests\Release\test_engine_api_feed_check.exe'
$genExe = Join-Path $BuildDir 'tests\Release\test_feed_fixtures.exe'
foreach ($e in @($feedExe, $genExe)) { if (-not (Test-Path $e)) { throw "missing $e (build it, or drop -SkipBuild)" } }

# 2. the generator: self-test, write, checksum comparison
& $genExe; if ($LASTEXITCODE -ne 0) { throw 'test_feed_fixtures failed' }
& $feedExe --selftest | Select-Object -Last 1
& $genExe --write "$OutDir\gen" | Out-Null; if ($LASTEXITCODE -ne 0) { throw 'fixture write failed' }
$want = Get-Content (Join-Path $feedDir 'SHA256SUMS')
$got = Get-Content "$OutDir\gen\SHA256SUMS"
$diff = Compare-Object $want $got
if ($diff) { Write-Host 'WARNING: generated fixtures differ from tests\feed\SHA256SUMS (a builder changed: update the list in the same commit)'; $diff | Format-Table | Out-String | Write-Host }
else { Write-Host 'generated fixtures match tests\feed\SHA256SUMS' }

# 3. the drive-set fixtures: the repository's own generators, copied so that the repository stays clean
$driveSrc = Join-Path $repo 'src\slint\tests\drive\fixtures'
Get-ChildItem $driveSrc -Filter 'make_*.py' | ForEach-Object { Copy-Item $_.FullName "$OutDir\drive-fixtures" -Force }
Push-Location "$OutDir\drive-fixtures"
try {
    Get-ChildItem -Filter 'make_*.py' | ForEach-Object {
        & python -I $_.Name | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "$($_.Name) failed" }
    }
} finally { Pop-Location }

# 4. the newest module of every plugin id
Get-ChildItem "$OutDir\plugins" -Filter '*.dll' | Remove-Item -Force
$best = @{}
Get-ChildItem $PluginSrc -Filter '*-abi3-win-x64.dll' | ForEach-Object {
    if ($_.Name -match '^(.+)-(\d+)\.(\d+)\.(\d+)-abi3-win-x64\.dll$') {
        $id = $Matches[1]; $v = [int]$Matches[2] * 1000000 + [int]$Matches[3] * 1000 + [int]$Matches[4]
        if (-not $best.ContainsKey($id) -or $best[$id].v -lt $v) { $best[$id] = @{ v = $v; path = $_.FullName } }
    }
}
$best.Values | ForEach-Object { Copy-Item $_.path "$OutDir\plugins" -Force }
Write-Host "$($best.Count) plugin modules copied from $PluginSrc"

# 5. the cases
$env:DRIVE_DIR = "$OutDir\drive-fixtures"; $env:FEED_DIR = "$OutDir\gen"; $env:SAMPLES_DIR = $SamplesDir
$sets = @(@{ name = 'new'; file = Join-Path $feedDir 'feed-cases-new.tsv'; gate = $true },
          @{ name = 'drive'; file = Join-Path $feedDir 'feed-cases-drive.tsv'; gate = $true })
$realCases = Join-Path $OutDir 'feed-cases-real.tsv'
if (Test-Path $realCases) { $sets += @{ name = 'real'; file = $realCases; gate = $false } }
$failedGate = 0
$md = New-Object System.Collections.Generic.List[string]
$md.Add("# Feed check results"); $md.Add(""); $md.Add("Run $(Get-Date -Format 'yyyy-MM-dd HH:mm') on branch $(git -C $repo rev-parse --abbrev-ref HEAD) at $(git -C $repo rev-parse --short HEAD); modules from $PluginSrc."); $md.Add("")
foreach ($set in $sets) {
    $resFile = "$OutDir\results-$($set.name).tsv"
    & $feedExe $set.file "$OutDir\plugins" $resFile @Only | Tee-Object -FilePath "$OutDir\run-$($set.name).log" | Select-String '^(PASS|FAIL|SKIP|feed check)'
    $rows = Import-Csv $resFile -Delimiter "`t"
    $md.Add("## $($set.name) cases"); $md.Add(""); $md.Add('| case | verdict | seconds | tuning read back | output fragment / note |'); $md.Add('|---|---|---|---|---|')
    foreach ($r in $rows) {
        $frag = if ($set.name -eq 'real') { ($r.fragment -replace '[A-Z0-9]{1,2}[0-9][A-Z]{1,3}(-[0-9]+)?', '##') } else { $r.fragment }   # real recordings: callsign-shaped words masked
        $md.Add("| $($r.id) | $($r.verdict) | $($r.seconds) | $($r.tuning) | $(($r.note + ' ' + $frag).Replace('|','/')) |")
        if ($set.gate -and $r.verdict -eq 'FAIL') { $failedGate++ }
    }
    $md.Add("")
}
Set-Content -Path "$OutDir\RESULTS.md" -Value $md -Encoding utf8
$shaAfter = if (Test-Path $realCfg) { (Get-FileHash $realCfg -Algorithm SHA256).Hash.ToLower() } else { 'absent' }
Write-Host "real config.json sha256 after:  $shaAfter  ($(if ($shaAfter -eq $shaBefore) { 'unchanged' } else { 'CHANGED - investigate' }))"
Write-Host "results: $OutDir\RESULTS.md ; generated/drive rows failed: $failedGate"
if ($shaAfter -ne $shaBefore -or $failedGate -gt 0) { exit 1 }
