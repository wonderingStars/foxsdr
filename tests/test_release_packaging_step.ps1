# Fake-only suite for the `package-manifests` step of tools/release.ps1 (wire-c, docs/feature-records/packaging.md).
# The step's definition is AST-extracted (as tests/test_release_symbols.ps1 does), never the release script's main body; the generator
# is replaced by a recording fake, so nothing is generated, contacted or published. Nothing outside build\release-packaging-test is written.
#
#   pwsh -NoProfile -File tests\test_release_packaging_step.ps1
#   ... -SourcePath <a copy of release.ps1>     (the break-it check: run it against a mutated script)
#
# Cases: step-listed (the name is in the step order, after github, and has a dispatch line), ok (setup in dist, sha256.txt agrees, the
# generator prints the hash: PASS and the three folders are checked), dry-run (no generator call at all), no-setup, no-sha-file,
# hash-mismatch (sha256.txt names another hash: refused BEFORE the generator runs), generator-fails (exit 1), generator-wrong-hash
# (it printed another hash), no-folder (a generator that wrote nothing).
param(
    [string]$SourcePath = (Join-Path $PSScriptRoot '..\tools\release.ps1')
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0
$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$runRoot = Join-Path $repoRoot ('build\release-packaging-test\fake-' + $PID + '-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $runRoot -Force | Out-Null
$script:checks = 0; $script:failed = 0
function Check([bool]$Ok, [string]$Name) {
    $script:checks++
    if (-not $Ok) { $script:failed++; Write-Host ('FAIL  ' + $Name) } else { Write-Host ('ok    ' + $Name) }
}

$srcText = [IO.File]::ReadAllText($SourcePath)
$m = [regex]::Match($srcText, "\`$script:StepOrder = @\(([^)]*)\)")
$order = @()
if ($m.Success) { $order = @([regex]::Matches($m.Groups[1].Value, "'([^']+)'") | ForEach-Object { $_.Groups[1].Value }) }
Check ($order -contains 'package-manifests') 'step-listed: the name is in the step order'
Check (($order -contains 'package-manifests') -and ([array]::IndexOf($order, 'package-manifests') -gt [array]::IndexOf($order, 'github'))) 'step-listed: it comes after github'
Check ($srcText -match "'package-manifests'\s*\{\s*\`$outcome = Step-PackageManifests\s*\}") 'step-listed: the dispatch line calls Step-PackageManifests'

foreach ($source in @((Join-Path $repoRoot 'tools\release-common.ps1'), $SourcePath)) {
    $tokens = $null; $errors = $null
    $ast = [Management.Automation.Language.Parser]::ParseFile($source, [ref]$tokens, [ref]$errors)
    if ($errors.Count) { throw ($errors | Out-String) }
    foreach ($def in $ast.FindAll({param($n) $n -is [Management.Automation.Language.FunctionDefinitionAst]}, $false)) {
        . ([scriptblock]::Create($def.Extent.Text))
    }
}
if (-not (Get-Command Step-PackageManifests -ErrorAction SilentlyContinue)) {
    Check $false 'Step-PackageManifests exists in the script'
    Write-Host ('test_release_packaging_step: ' + $script:checks + ' checks, ' + $script:failed + ' failed'); exit 1
}

# ---- seams -----------------------------------------------------------------------------------------------------------
$script:Messages = New-Object System.Collections.ArrayList
$script:GenCalls = New-Object System.Collections.ArrayList
$script:GenMode = 'ok'
function Write-Result { param($Level, $Name, $Value = '') [void]$script:Messages.Add(($Level + ' ' + $Name + ' : ' + $Value)) }
function Write-Detail { param($Text) [void]$script:Messages.Add([string]$Text) }
function Write-Plan { param($What, $Detail = '') [void]$script:Messages.Add('DRY ' + $What) }
function Format-CommandLine { param($File, $Arguments) return ($File + ' ' + ($Arguments -join ' ')) }
function Invoke-Exe {
    param($File, $Arguments, $TimeoutSec, $Env, $WorkDir, $InputFile, $Label, $HeartbeatSec, [switch]$NoLog)
    [void]$script:GenCalls.Add($Arguments)
    $out = $Arguments[[array]::IndexOf($Arguments, '-OutDir') + 1]
    $setup = $Arguments[[array]::IndexOf($Arguments, '-SetupFile') + 1]
    $hash = (Get-FileHash -LiteralPath $setup -Algorithm SHA256).Hash.ToLowerInvariant()
    switch ($script:GenMode) {
        'fails'      { return [pscustomobject]@{ ExitCode = 1; StdOut = ''; StdErr = 'refused'; Output = 'refused'; TimedOut = $false } }
        'wrong-hash' { foreach ($f in 'winget', 'scoop', 'chocolatey') { New-Item -ItemType Directory -Path (Join-Path $out $f) -Force | Out-Null }
                       return [pscustomobject]@{ ExitCode = 0; StdOut = ('sha256 ' + ('0' * 64)); StdErr = ''; Output = ''; TimedOut = $false } }
        'no-folder'  { return [pscustomobject]@{ ExitCode = 0; StdOut = ('sha256 ' + $hash); StdErr = ''; Output = ''; TimedOut = $false } }
        default      { foreach ($f in 'winget', 'scoop', 'chocolatey') { New-Item -ItemType Directory -Path (Join-Path $out $f) -Force | Out-Null }
                       return [pscustomobject]@{ ExitCode = 0; StdOut = ('sha256 ' + $hash); StdErr = ''; Output = ''; TimedOut = $false } }
    }
}
function Assert-Check {
    param([bool]$Ok, [string]$Name, [string]$Value = '', [string]$Why = '')
    if ($Ok) { Write-Result 'PASS' $Name $Value; return }
    Write-Result 'FAIL' $Name $Value
    throw ('RELEASE-FAIL: ' + $Name)
}

function New-Case([string]$Name, [bool]$Setup, [string]$ShaMode) {
    $root = Join-Path $runRoot $Name
    $dist = Join-Path $root 'dist'
    New-Item -ItemType Directory -Path $dist -Force | Out-Null
    $inst = 'foxsdr-setup-9.9.9.exe'
    if ($Setup) { [IO.File]::WriteAllBytes((Join-Path $dist $inst), [byte[]](1..200)) }
    if ($ShaMode -ne 'none') {
        $real = if ($Setup) { (Get-FileHash -LiteralPath (Join-Path $dist $inst) -Algorithm SHA256).Hash.ToLowerInvariant() } else { ('a' * 64) }
        if ($ShaMode -eq 'wrong') { $real = ('b' * 64) }
        [IO.File]::WriteAllText((Join-Path $dist 'sha256.txt'), ($real + '  ' + $inst + "`n"))
    }
    $script:RC = @{ Dist = $dist; InstallerName = $inst; ReleaseDir = $root; Repo = $repoRoot }
    $script:Ctx = @{ DryRun = $false }
}
function Run-Step { try { $script:Outcome = Step-PackageManifests; return $true } catch { $script:Err = $_.Exception.Message; Write-Host ("   (error: " + $script:Err + ")"); return $false } }

New-Case 'ok' $true 'good'; $script:GenMode = 'ok'; $script:GenCalls.Clear()
$ok = Run-Step
Check ($ok -and $script:Outcome -eq 'done') 'ok: the step passes'
Check ($script:GenCalls.Count -eq 1) 'ok: the generator ran once'
Check (($script:GenCalls[0] -contains '-RunWingetValidate') -and ($script:GenCalls[0] -contains '-Force')) 'ok: the generator got -RunWingetValidate and -Force'
Check ((@($script:Messages | Where-Object { $_ -like 'INFO not published*' })).Count -eq 1) 'ok: it says nothing was published'

New-Case 'dry' $true 'good'; $script:Ctx = @{ DryRun = $true }; $script:GenCalls.Clear()
$ok = Run-Step
Check ($ok -and $script:GenCalls.Count -eq 0) 'dry-run: the generator is NOT called'

New-Case 'nosetup' $false 'good'; $script:GenCalls.Clear()
$ok = Run-Step
Check ((-not $ok) -and $script:GenCalls.Count -eq 0) 'no-setup: refused, generator not called'

New-Case 'noshafile' $true 'none'; $script:GenCalls.Clear()
$ok = Run-Step
Check ((-not $ok) -and $script:GenCalls.Count -eq 0) 'no-sha-file: refused, generator not called'

New-Case 'mismatch' $true 'wrong'; $script:GenCalls.Clear()
$ok = Run-Step
Check ((-not $ok) -and $script:GenCalls.Count -eq 0 -and $script:Err -like '*equals dist*') 'hash-mismatch: refused BEFORE the generator runs'

New-Case 'fails' $true 'good'; $script:GenMode = 'fails'; $script:GenCalls.Clear()
$ok = Run-Step
Check ((-not $ok) -and $script:Err -like '*exits 0*') 'generator-fails: refused'

New-Case 'wronghash' $true 'good'; $script:GenMode = 'wrong-hash'; $script:GenCalls.Clear()
$ok = Run-Step
Check ((-not $ok) -and $script:Err -like '*printed the setup file hash*') 'generator-wrong-hash: refused'

New-Case 'nofolder' $true 'good'; $script:GenMode = 'no-folder'; $script:GenCalls.Clear()
$ok = Run-Step
Check ((-not $ok) -and $script:Err -like '*package folder*') 'no-folder: refused'

Remove-Item -LiteralPath $runRoot -Recurse -Force -ErrorAction SilentlyContinue
Write-Host ('test_release_packaging_step: ' + $script:checks + ' checks, ' + $script:failed + ' failed')
if ($script:failed -gt 0) { exit 1 }
exit 0
