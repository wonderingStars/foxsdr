# Collects the built companion programs and their legal texts into the ONE folder build-msix.ps1 takes as -CompanionsDir.
#
#   powershell -ExecutionPolicy Bypass -File installer\msix\collect-companions.ps1 `
#       -SourceDir <public copy of foxsdr-companions> -BuildDir <cmake build folder of it> -OutDir <empty folder to fill>
#
# WHAT IT DOES. For every folder of the public source copy that has a <name>.helper.json (the program's own manifest: it names the executable and states the licence),
# it takes the BUILT program from the build folder (Release output of that copy's README commands) and copies, beside it, the legal TEXT files of that folder: every file whose
# NAME says it is a licence, a copying notice, a notice, a third-party list, a trademark note, an additional-terms file or the upstream statement (UPSTREAM.md: the project it
# is built from, the commit, the licence and the changes made). It selects files by NAME and EXTENSION only. It opens no source file and copies none: a source file extension
# (c, cc, cpp, h, hpp, py, cmake, inc, sh, bat, json, ...) is never copied, whatever its name. (COPYING.LIB is the LGPL text of hfdl's libfec, not a library.)
#
# hfserial (AGPL, held out by the owner) and dvframes (held out of the public copy) are never collected, by name, whether or not a folder of that name exists.
#
# OUTPUT (what build-msix.ps1 -CompanionsDir reads):
#   <OutDir>\companions.manifest.json   {"version":1,"source":"<repository address>","commit":"<short commit of the source copy>","companions":[{folder,id,file,licence,upstream,decodes}]}
#   <OutDir>\<folder>\<program>.exe     the built program
#   <OutDir>\<folder>\legal\...         the legal text files, in the same relative places they have in the source copy
#
# NO Get-Content / Set-Content ON TEXT THAT IS COPIED: files are copied as bytes. The manifest is written through [System.IO.File] as UTF-8 without a byte-order mark.
#
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)] [string] $SourceDir,
    [Parameter(Mandatory = $true)] [string] $BuildDir,
    [Parameter(Mandatory = $true)] [string] $OutDir,
    [string] $SourceAddress = "https://github.com/wonderingStars/foxsdr-companions"
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

function Fail([string] $text) { throw "collect-companions: $text" }

$Held = @("hfserial", "dvframes")
$LegalName = '(?i)(licen[cs]e|copying|notice|third[_-]?party|trademark|additional-terms|upstream|copyright)'
$SourceExt = @(".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".inc", ".in", ".py", ".cmake", ".sh", ".bat", ".cmd", ".ps1", ".json", ".exe", ".dll", ".obj", ".o", ".a", ".s", ".asm", ".rs", ".go", ".js", ".pl", ".txt.in")
$SkipDirs = @("test", "tests", "build", ".git", "patches")

if (-not (Test-Path -LiteralPath $SourceDir)) { Fail "source copy not found: $SourceDir" }
if (-not (Test-Path -LiteralPath $BuildDir)) { Fail "build folder not found: $BuildDir" }
$SourceDir = (Resolve-Path -LiteralPath $SourceDir).Path
$BuildDir = (Resolve-Path -LiteralPath $BuildDir).Path
if (Test-Path -LiteralPath $OutDir) {
    if (@(Get-ChildItem -LiteralPath $OutDir -Force).Count -gt 0) { Fail "the output folder is not empty: $OutDir" }
} else {
    New-Item -ItemType Directory -Path $OutDir | Out-Null
}
$OutDir = (Resolve-Path -LiteralPath $OutDir).Path

# the commit of the source copy (read-only; "unknown" when it is not a git folder)
$commit = "unknown"
try {
    $c = & git -C $SourceDir rev-parse --short HEAD 2>$null
    if ($LASTEXITCODE -eq 0 -and -not [string]::IsNullOrWhiteSpace($c)) { $commit = $c.Trim() }
} catch { }

# what each program decodes, from the README's table (a row: | `folder/` | decodes | upstream | licence | proof |)
$decodes = @{}
$readme = Join-Path $SourceDir "README.md"
if (Test-Path -LiteralPath $readme) {
    $text = [System.IO.File]::ReadAllText($readme, [System.Text.Encoding]::UTF8)
    foreach ($m in [regex]::Matches($text, '(?m)^\|\s*`([a-z0-9]+)/`\s*\|\s*([^|]+?)\s*\|')) {
        $decodes[$m.Groups[1].Value] = $m.Groups[2].Value
    }
}

$entries = @()
foreach ($dir in (Get-ChildItem -LiteralPath $SourceDir -Directory | Sort-Object Name)) {
    $folder = $dir.Name
    $manifestFile = Join-Path $dir.FullName ($folder + ".helper.json")
    if (-not (Test-Path -LiteralPath $manifestFile)) { continue }
    if ($Held -contains $folder.ToLowerInvariant()) { Write-Host "    skipped (held out): $folder"; continue }
    $meta = [System.IO.File]::ReadAllText($manifestFile, [System.Text.Encoding]::UTF8) | ConvertFrom-Json
    $exe = $(if ($meta.PSObject.Properties['executable']) { [string]$meta.executable } else { "" })
    if ([string]::IsNullOrWhiteSpace($exe) -or $exe -notmatch '^[A-Za-z0-9._-]+\.exe$') { Fail "$folder : its manifest names no usable executable ($exe)" }
    $built = @(Get-ChildItem -LiteralPath $BuildDir -Recurse -File -Filter $exe -ErrorAction SilentlyContinue | Where-Object { $_.FullName -match '(?i)[\\/]Release[\\/]' })
    if ($built.Count -eq 0) { Fail "$folder : the built program $exe was not found under $BuildDir (a Release build of the source copy)" }
    $builtFile = $built | Sort-Object LastWriteTime | Select-Object -Last 1
    $outFolder = Join-Path $OutDir $folder
    New-Item -ItemType Directory -Path $outFolder | Out-Null
    Copy-Item -LiteralPath $builtFile.FullName -Destination (Join-Path $outFolder $exe)

    # the legal text files, by name and extension only
    $legalCount = 0
    $root = $dir.FullName
    foreach ($f in (Get-ChildItem -LiteralPath $root -Recurse -File)) {
        $rel = $f.FullName.Substring($root.Length).TrimStart('\', '/')
        $parts = $rel -split '[\\/]'
        if ($parts.Count -gt 1) {
            $skip = $false
            foreach ($p in $parts[0..($parts.Count - 2)]) { if ($SkipDirs -contains $p.ToLowerInvariant()) { $skip = $true } }
            if ($skip) { continue }
        }
        if ($f.Name -notmatch $LegalName) { continue }
        $ext = $f.Extension.ToLowerInvariant()
        if ($SourceExt -contains $ext) { continue }
        if ($f.Name -like "*.helper.json") { continue }
        if ($f.Length -gt 2MB) { continue }
        $dest = Join-Path (Join-Path $outFolder "legal") $rel
        $destDir = Split-Path -Parent $dest
        if (-not (Test-Path -LiteralPath $destDir)) { New-Item -ItemType Directory -Path $destDir | Out-Null }
        Copy-Item -LiteralPath $f.FullName -Destination $dest
        $legalCount++
    }
    if ($legalCount -eq 0) { Fail "$folder : no licence text was found" }
    $entries += [ordered]@{
        folder   = $folder
        id       = "companion-" + $folder
        file     = $exe
        licence  = $(if ($meta.PSObject.Properties['licence']) { [string]$meta.licence } else { "" })
        upstream = $(if ($meta.PSObject.Properties['upstream']) { [string]$meta.upstream } else { "" })
        decodes  = $(if ($decodes.ContainsKey($folder)) { $decodes[$folder] } else { "" })
    }
    Write-Host ("    {0,-12} {1,-22} {2,3} legal file(s)" -f $folder, $exe, $legalCount)
}
if ($entries.Count -eq 0) { Fail "no companion folder with a manifest was found in $SourceDir" }

$doc = [ordered]@{ version = 1; source = $SourceAddress; commit = $commit; companions = $entries }
$json = $doc | ConvertTo-Json -Depth 5
[System.IO.File]::WriteAllText((Join-Path $OutDir "companions.manifest.json"), $json, (New-Object System.Text.UTF8Encoding($false)))
Write-Host ("==> collected {0} companion program(s) from {1} (commit {2}) into {3}" -f $entries.Count, $SourceDir, $commit, $OutDir)
exit 0
