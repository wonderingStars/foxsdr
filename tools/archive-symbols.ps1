# Archives the PDB for a binary we have just built, keyed by the build id a
# crash report will quote.
#
# WHY THIS IS THE FIRST THING THE WHOLE DIAGNOSTICS FEATURE NEEDED. A captured
# stack is module+offset. Turning an offset back into a function and a line
# needs the PDB produced by THAT LINK - not a rebuild of the same source, not
# the same version number built on another machine. PDBs are not shipped to
# users, so if the one matching a shipped binary is not kept at build time it
# does not exist anywhere afterwards, and every report ever filed against that
# build is unreadable hex forever. Nothing else in the feature matters if this
# step is missing.
#
# WHY IT IS KEYED BY THE BUILD ID AND NOT THE VERSION OR THE FILE NAME. The
# linker stamps a CodeView "RSDS" record into the PE: a GUID plus an age
# counter, regenerated on every link and recorded identically in the binary and
# in its PDB. That is the only key that survives a rebuild of the same version,
# a nightly and its release, or two builds with different CRT linkage. The
# layout below is the standard symbol-server one -
#
#     <archive>\<pdb name>\<GUID><AGE>\<pdb name>
#     <archive>\<exe name>\<TIMESTAMP><SIZEOFIMAGE>\<exe name>
#
# - so the archive can be handed to WinDbg or dotnet-symbol as a symbol path
# directly, with no conversion step and nothing to remember.
#
# WHY IT IS A POST-BUILD STEP AND NOT PART OF THE RELEASE SCRIPT. A step that
# only runs when someone remembers to run it is a step that will be missed, and
# the first month of crash data would be worthless before anyone noticed.
# CMake invokes this on every build of the application, so the archive is
# populated by the same action that produced the binary - including from
# tools/build-nightly.ps1, which builds through CMake like everything else.
#
# ---------------------------------------------------------------------------
# WHERE THE ARCHIVE ACTUALLY LIVES, and this was a decision, not a default
# ---------------------------------------------------------------------------
#
# The archive is NOT in the git repository. `symbols/` is gitignored: a PDB is
# tens of megabytes of binary per link, and committing one per build would make
# the repository unusable inside a fortnight.
#
# It is also NOT local-only. "Local only" means one disk failure destroys the
# ability to read every crash report ever filed against every build already in
# users' hands - permanently, because a PDB cannot be regenerated from source
# (a rebuild produces a different GUID and different offsets). That is not a
# risk worth taking for a product that is sold.
#
# So there are two copies, with different jobs:
#
#   symbols\ in the working tree   Written by THIS script, on every build, from
#                                  a CMake POST_BUILD step. Offline, instant,
#                                  and impossible to forget because it is part
#                                  of the build itself.
#   nas:/volume1/foxsdr-symbols    The durable copy, on the Synology that
#                                  already holds this project's git mirror,
#                                  over the same SSH alias and key. Pushed by
#                                  tools/build-nightly.ps1 in the SAME step
#                                  that compiles the installer, because a
#                                  build that produces something shippable is
#                                  exactly the build whose symbols must
#                                  outlive this machine.
#
# The split is deliberate: a network copy on every incremental build would be
# unbearable, and a manual "remember to upload the symbols" step would be
# skipped. Only builds that can reach a user are mirrored, and they are
# mirrored automatically. See docs/DIAGNOSTICS.md.
#
# ---------------------------------------------------------------------------
# PLUGINS - THE MODULE CLASS MOST LIKELY TO BE THE FAULTING ONE
# ---------------------------------------------------------------------------
#
# Nothing here is specific to cascade.exe: -Binary takes any PE, the build id
# is read out of its own CodeView record, and the layout is the standard
# symbol-server one. But the POST_BUILD step in CMakeLists.txt runs for the
# `cascade` target only, and no plugin is built in this tree - they live in the
# separate plugin repository - so symbols\ holds cascade.exe and cascade.pdb
# and nothing else.
#
# That matters more than it sounds. A report's --- modules --- block carries a
# build id for every loaded plugin DLL, plugins are third-party code running
# in-process, and "which plugin was loaded" has already been the answer to real
# faults in this product. Left alone, the modules most likely to be the
# faulting one are the ones that can never be symbolised.
#
# So the plugin repository's own build calls THIS script, with the same archive
# root, from its own POST_BUILD step:
#
#   add_custom_command(TARGET <plugin> POST_BUILD
#       COMMAND powershell -NoProfile -ExecutionPolicy Bypass -File
#               "<path to cascade>/tools/archive-symbols.ps1"
#               -Binary "$<TARGET_FILE:<plugin>>"
#               -ArchiveRoot "<path to cascade>/symbols"
#               -Version "<plugin version>"
#               -Commit "<plugin repo's short SHA, -dirty if modified>"
#       VERBATIM)
#
# -Commit rather than -CommitHeader: the plugin repository has its own HEAD,
# and stamping it with the application's would be the same "sends the reader to
# a tree that exists and is wrong" failure the dirty marker exists to prevent.
# The index row names the DLL, so two repositories sharing one archive stay
# distinguishable. tests/test_diagnostics.cpp archives a module that is not
# cascade.exe into a scratch root to prove this path works.
#
# It never fails the build. A missing PDB prints a warning and exits 0; the
# assertion that the archive actually contains this build lives in
# tests/test_diagnostics.cpp, where a silent regression goes red instead of
# stopping a developer's incremental build.
#
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Binary,
    [Parameter(Mandatory = $true)][string]$ArchiveRoot,
    [string]$Version = "",
    # The commit is read from the header cmake/git-commit.cmake regenerates on
    # EVERY build, not passed as a CMake variable resolved at configure time.
    # The index used to carry the configure-time SHA, which is why it shows
    # nine different build ids all stamped 5ba13f6d0c86: the archive was
    # correct (it is keyed by the build id) while the commit column beside it
    # was the commit of whenever the build tree was last configured. Reading
    # the same file the binary's own gitCommit() comes from makes the two agree
    # by construction.
    [string]$CommitHeader = "",
    [string]$Commit = "",
    # Writes the archived paths to the OUTPUT stream (one per line) so a caller
    # can mirror exactly this build and nothing else. Without it the script is
    # silent on the pipeline and only prints for a human. Used by
    # tools/build-nightly.ps1: the durable mirror must carry the symbols for
    # the build it just produced, not for every incremental relink that
    # happened to land in the same archive.
    [switch]$EmitPaths
)

$ErrorActionPreference = "Stop"

function Warn($msg) { Write-Host "archive-symbols: $msg" -ForegroundColor Yellow }

# An explicit -Commit still wins, so a caller with its own idea (a tarball
# build, a re-archive of an old binary) is not overridden.
if ($Commit -eq "" -and $CommitHeader -ne "" -and (Test-Path -LiteralPath $CommitHeader)) {
    $line = Select-String -LiteralPath $CommitHeader -Pattern 'CASCADE_GIT_COMMIT\s+"([^"]*)"'
    if ($null -ne $line) { $Commit = $line.Matches[0].Groups[1].Value }
}

if (-not (Test-Path -LiteralPath $Binary)) {
    Warn "no binary at $Binary"
    exit 0
}

$bytes = [System.IO.File]::ReadAllBytes($Binary)
if ($bytes.Length -lt 0x40 -or $bytes[0] -ne 0x4D -or $bytes[1] -ne 0x5A) {
    Warn "$Binary is not a PE image"
    exit 0
}

$peOff = [BitConverter]::ToInt32($bytes, 0x3C)
if ($peOff -le 0 -or $peOff + 24 -ge $bytes.Length) { Warn "bad PE header offset"; exit 0 }
if ([BitConverter]::ToUInt32($bytes, $peOff) -ne 0x00004550) { Warn "no PE signature"; exit 0 }

$coff = $peOff + 4
$numSections = [BitConverter]::ToUInt16($bytes, $coff + 2)
$timeDateStamp = [BitConverter]::ToUInt32($bytes, $coff + 4)
$sizeOfOptional = [BitConverter]::ToUInt16($bytes, $coff + 16)
$optOff = $coff + 20
$magic = [BitConverter]::ToUInt16($bytes, $optOff)

# PE32+ (0x20b) puts the data directories at +112; PE32 (0x10b) at +96. Both
# are handled so this does not quietly stop working on a 32-bit build.
if ($magic -eq 0x20b) {
    $sizeOfImage = [BitConverter]::ToUInt32($bytes, $optOff + 56)
    $dirOff = $optOff + 112
} elseif ($magic -eq 0x10b) {
    $sizeOfImage = [BitConverter]::ToUInt32($bytes, $optOff + 56)
    $dirOff = $optOff + 96
} else {
    Warn ("unknown optional header magic 0x{0:X}" -f $magic)
    exit 0
}

# Data directory 6 is IMAGE_DIRECTORY_ENTRY_DEBUG.
$debugRva = [BitConverter]::ToUInt32($bytes, $dirOff + 6 * 8)
$debugSize = [BitConverter]::ToUInt32($bytes, $dirOff + 6 * 8 + 4)
if ($debugRva -eq 0 -or $debugSize -eq 0) {
    Warn "$Binary carries no debug directory - the build produced no PDB, and any crash report against it will be unreadable"
    exit 0
}

# RVA -> file offset through the section table.
$secOff = $optOff + $sizeOfOptional
$fileOff = 0
for ($i = 0; $i -lt $numSections; $i++) {
    $s = $secOff + $i * 40
    # IMAGE_SECTION_HEADER: Name[8], VirtualSize +8, VirtualAddress +12,
    # SizeOfRawData +16, PointerToRawData +20. The last two are easy to swap
    # and the swap is silent - it produces a plausible file offset pointing at
    # nothing, which reads as "this binary has no CodeView record" rather than
    # as a parsing bug.
    $vsize = [BitConverter]::ToUInt32($bytes, $s + 8)
    $va = [BitConverter]::ToUInt32($bytes, $s + 12)
    $rawSize = [BitConverter]::ToUInt32($bytes, $s + 16)
    $raw = [BitConverter]::ToUInt32($bytes, $s + 20)
    $span = [Math]::Max($vsize, $rawSize)
    if ($debugRva -ge $va -and $debugRva -lt ($va + $span)) {
        $fileOff = $raw + ($debugRva - $va)
        break
    }
}
if ($fileOff -eq 0) { Warn "could not map the debug directory RVA"; exit 0 }

$buildId = $null
$pdbRecorded = $null
$entries = [int]($debugSize / 28)
for ($i = 0; $i -lt $entries; $i++) {
    $e = $fileOff + $i * 28
    $type = [BitConverter]::ToUInt32($bytes, $e + 12)
    if ($type -ne 2) { continue }   # IMAGE_DEBUG_TYPE_CODEVIEW
    $cvSize = [BitConverter]::ToUInt32($bytes, $e + 16)
    $cvOff = [BitConverter]::ToUInt32($bytes, $e + 24)
    if ($cvOff -eq 0 -or $cvSize -lt 25) { continue }
    if ([System.Text.Encoding]::ASCII.GetString($bytes, $cvOff, 4) -ne "RSDS") { continue }

    # The GUID is stored in the native little-endian layout; the symbol-server
    # key is its canonical big-endian rendering with the hyphens removed,
    # followed by the age in hex. Composed field by field rather than via
    # [Guid]::ToString("N") so the byte order is explicit and reviewable.
    $d1 = [BitConverter]::ToUInt32($bytes, $cvOff + 4)
    $d2 = [BitConverter]::ToUInt16($bytes, $cvOff + 8)
    $d3 = [BitConverter]::ToUInt16($bytes, $cvOff + 10)
    $tail = ""
    for ($k = 0; $k -lt 8; $k++) { $tail += "{0:X2}" -f $bytes[$cvOff + 12 + $k] }
    $age = [BitConverter]::ToUInt32($bytes, $cvOff + 20)
    $buildId = ("{0:X8}{1:X4}{2:X4}{3}{4:X}" -f $d1, $d2, $d3, $tail, $age)

    $end = $cvOff + $cvSize
    $p = $cvOff + 24
    $sb = New-Object System.Text.StringBuilder
    while ($p -lt $end -and $p -lt $bytes.Length -and $bytes[$p] -ne 0) {
        [void]$sb.Append([char]$bytes[$p]); $p++
    }
    $pdbRecorded = $sb.ToString()
    break
}

if (-not $buildId) { Warn "no CodeView record in $Binary"; exit 0 }

$pdbName = if ($pdbRecorded) { Split-Path -Leaf $pdbRecorded } else { [System.IO.Path]::GetFileNameWithoutExtension($Binary) + ".pdb" }
$exeName = Split-Path -Leaf $Binary

# The PDB the linker recorded, or the one sitting beside the binary. The
# recorded path is absolute and correct on the build machine, which is where
# this runs.
$pdbPath = $null
if ($pdbRecorded -and (Test-Path -LiteralPath $pdbRecorded)) { $pdbPath = $pdbRecorded }
if (-not $pdbPath) {
    $beside = Join-Path (Split-Path -Parent $Binary) $pdbName
    if (Test-Path -LiteralPath $beside) { $pdbPath = $beside }
}
if (-not $pdbPath) {
    Warn "no PDB found for $exeName (looked for $pdbRecorded) - reports against this build cannot be symbolised"
    exit 0
}

$pdbDest = Join-Path (Join-Path (Join-Path $ArchiveRoot $pdbName) $buildId) $pdbName
$binKey = ("{0:X8}{1:x}" -f $timeDateStamp, $sizeOfImage)
$binDest = Join-Path (Join-Path (Join-Path $ArchiveRoot $exeName) $binKey) $exeName

foreach ($pair in @(@($pdbPath, $pdbDest), @($Binary, $binDest))) {
    $dir = Split-Path -Parent $pair[1]
    if (-not (Test-Path -LiteralPath $dir)) { New-Item -ItemType Directory -Path $dir -Force | Out-Null }
    # Copy only when it differs: a rebuild that did not relink keeps the same
    # build id, and rewriting a 40 MB PDB on every incremental build would be
    # noticed.
    $need = $true
    if (Test-Path -LiteralPath $pair[1]) {
        $need = (Get-Item -LiteralPath $pair[0]).Length -ne (Get-Item -LiteralPath $pair[1]).Length
    }
    if ($need) { Copy-Item -LiteralPath $pair[0] -Destination $pair[1] -Force }
}

# The human index. The archive is keyed for machines; this is the file a
# future session greps when it has a version and wants a build id, or a build
# id and wants to know which release it belongs to.
#
# WRITTEN WITHOUT A BOM, AND THAT IS NOT A DETAIL. This was
# `Add-Content -Encoding utf8`, which on Windows PowerShell 5.1 writes a UTF-8
# BOM when it CREATES the file - so row 1 began with EF BB BF instead of a
# date, and every anchored parse of the index (`grep "^2026"`, a ^\d{4} regex,
# a split on the first tab) silently skipped the oldest entry. It is the only
# row that can never be re-derived from a later build, which makes it exactly
# the wrong one to lose. The archive itself was unaffected throughout: it is
# keyed by build id, and only this human index was wrong.
#
# AppendAllText with an explicit BOM-less UTF8Encoding, rather than
# Out-File/Set-Content, because every PowerShell text cmdlet's default
# encoding differs between 5.1 and 7 and this file is read by tools that are
# not PowerShell.
if (-not (Test-Path -LiteralPath $ArchiveRoot)) {
    New-Item -ItemType Directory -Path $ArchiveRoot -Force | Out-Null
}
$indexPath = Join-Path ((Resolve-Path -LiteralPath $ArchiveRoot).Path) "index.txt"
$line = "{0}`t{1}`t{2}`t{3}`t{4}`t{5}" -f (Get-Date -Format "yyyy-MM-dd HH:mm:ss"), $exeName, $Version, $Commit, $buildId, $pdbName
$existing = @()
if (Test-Path -LiteralPath $indexPath) { $existing = @(Get-Content -LiteralPath $indexPath) }
if (-not ($existing | Where-Object { $_ -like "*`t$buildId`t*" })) {
    $utf8NoBom = New-Object System.Text.UTF8Encoding($false)
    [System.IO.File]::AppendAllText($indexPath, $line + "`r`n", $utf8NoBom)
}

Write-Host "archive-symbols: $exeName $Version build $buildId -> $pdbDest"
if ($EmitPaths) {
    # Relative to $ArchiveRoot, because the caller mirrors into a root of its
    # own and an absolute Windows path would be meaningless there.
    Write-Output (Join-Path (Join-Path $pdbName $buildId) $pdbName)
    Write-Output (Join-Path (Join-Path $exeName $binKey) $exeName)
    Write-Output "index.txt"
}
exit 0
