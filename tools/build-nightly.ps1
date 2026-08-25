# Builds a nightly FoxSDR installer from the current working tree.
#
# A nightly is the same product as a release, built from master instead of from
# a tag, and stamped so it can never be confused with one. It carries a
# pre-release version - <next>-nightly.<date>.<sha> - which sorts BEFORE the
# release it is heading towards under semver, and which the binary itself
# reports: the file name, the About line, the usage report and the bug-report
# form all say the same string because CMake and Inno Setup are given the same
# string.
#
# That last part is the point. A nightly whose binary called itself "0.56.0"
# would produce bug reports naming a release that does not exist, and no way to
# tell which build the reporter ran.
#
# The build directory is SEPARATE from the normal one on purpose. The version
# is a compile definition, so sharing a build tree with a release build would
# mean every switch between them rebuilds - and worse, a stale object could
# leave a release binary reporting a nightly version.
#
# Usage:
#   powershell -File tools/build-nightly.ps1                 # build, print the constants
#   powershell -File tools/build-nightly.ps1 -Publish        # ...and copy to the website
#
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

[CmdletBinding()]
param(
    # Where the website keeps the files it serves. Only used with -Publish.
    [string]$SiteDir = "C:\Users\steve\OneDrive\Documents\fox sdr",
    # SoapySDR and OpenSSL are the two dependencies that are not vendored, and
    # they come from vcpkg. A fresh build tree has none of the release tree's
    # cached configuration, so the toolchain has to be named explicitly or the
    # configure fails on "Could not find SoapySDR".
    [string]$Toolchain = "C:/vcpkg/scripts/buildsystems/vcpkg.cmake",
    [string]$Triplet = "x64-windows",
    [switch]$Publish,
    # Where the PDBs for every shippable build are kept so they outlive this
    # machine. Same host and key as the git mirror (see ~/.ssh/config "nas").
    [string]$SymbolMirror = "nas:/volume1/foxsdr-symbols",
    # Deliberate escape hatch for building with the NAS unreachable. It is a
    # switch and not a silent fallback on purpose: skipping this leaves the
    # only copy of these symbols on one disk, and if that disk dies every crash
    # report ever filed against this build becomes unreadable hex forever.
    [switch]$SkipSymbolMirror
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
Set-Location $repo

# --- Identify the build -----------------------------------------------------

# The version this tree is heading towards, read from CMakeLists rather than
# repeated here: two places to bump is one place to forget.
$projectLine = Select-String -Path (Join-Path $repo "CMakeLists.txt") -Pattern 'project\(cascade VERSION ([0-9]+\.[0-9]+\.[0-9]+)' | Select-Object -First 1
if (-not $projectLine) { throw "could not read the project version from CMakeLists.txt" }
$next = $projectLine.Matches[0].Groups[1].Value

$sha = (git rev-parse --short=7 HEAD | Out-String).Trim()
if (-not $sha) { throw "could not read the commit; a nightly must name what it was built from" }

# A dirty tree produces a build nobody can reproduce from a commit. Say so in
# the version rather than pretending otherwise.
#
# Piped through Out-String because `git status --porcelain` on a CLEAN tree
# returns $null, not an empty string, and calling .Trim() on it throws - so the
# first version of this script failed on exactly the case it is meant to
# handle, and would only have worked on a dirty tree.
$dirty = ""
if ((git status --porcelain | Out-String).Trim()) {
    $dirty = ".dirty"
    Write-Warning "working tree has uncommitted changes - marking the build .dirty"
}

$date = Get-Date -Format "yyyyMMdd"
$version = "$next-nightly.$date.$sha$dirty"
Write-Host "Building nightly $version" -ForegroundColor Cyan

# --- Build ------------------------------------------------------------------

$buildDir = Join-Path $repo "build-nightly"
# CMAKE_PREFIX_PATH as well as the toolchain: on this machine vcpkg is in
# classic (non-manifest) mode, so the toolchain alone does not put the installed
# tree on the search path and find_package(SoapySDR) fails.
$vcpkgRoot = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $Toolchain))
$installed = Join-Path $vcpkgRoot "installed/$Triplet"
cmake -S $repo -B $buildDir -DCASCADE_VERSION_STRING="$version" `
    -DCMAKE_TOOLCHAIN_FILE="$Toolchain" -DVCPKG_TARGET_TRIPLET="$Triplet" `
    -DCMAKE_PREFIX_PATH="$installed" | Out-Null
if ($LASTEXITCODE -ne 0) { throw "cmake configure failed" }

# Twice: a file added since the last configure is registered by the
# CONFIGURE_DEPENDS glob but not compiled in the same pass under the Visual
# Studio generator, and ctest then reports the new test as "Not Run".
cmake --build $buildDir --config Release | Out-Null
cmake --build $buildDir --config Release | Out-Null
if ($LASTEXITCODE -ne 0) { throw "build failed" }

# A nightly that fails its own tests is not a nightly, it is a broken upload.
Push-Location $buildDir
ctest -C Release --output-on-failure | Out-Null
$testsOk = ($LASTEXITCODE -eq 0)
Pop-Location
if (-not $testsOk) { throw "tests failed - refusing to produce a nightly" }

# The binary must agree with the name it is about to be given.
$exe = Join-Path $buildDir "Release\cascade.exe"
if (-not (Test-Path $exe)) { throw "no cascade.exe at $exe" }
$reported = (& $exe --version 2>&1 | Out-String).Trim()
if ($reported -notmatch [regex]::Escape($version)) {
    throw "binary reports '$reported' but the nightly is '$version' - the version injection is broken"
}
Write-Host "binary reports: $reported" -ForegroundColor Green

# --- Package ----------------------------------------------------------------

$iscc = Join-Path $env:LOCALAPPDATA "Programs\Inno Setup 6\ISCC.exe"
if (-not (Test-Path $iscc)) { $iscc = "C:\Program Files (x86)\Inno Setup 6\ISCC.exe" }
if (-not (Test-Path $iscc)) { throw "Inno Setup not found" }

# /DBuildDir points the installer at the nightly build tree rather than the
# release one, so the two never pick up each other's binaries.
# AppVersionNumeric as well: Windows' VERSIONINFO resource takes x.y.z and
# rejects a pre-release suffix, so the file-properties version is the plain
# number while everything a human reads keeps the full nightly string.
& $iscc /DAppVersion="$version" /DAppVersionNumeric="$next" /DBuildDir="$buildDir\Release" `
    (Join-Path $repo "installer\cascade.iss") | Out-Null
if ($LASTEXITCODE -ne 0) { throw "installer compile failed" }

$setup = Join-Path $repo "installer\Output\foxsdr-setup-$version.exe"
if (-not (Test-Path $setup)) { throw "no installer produced at $setup" }
$hash = (Get-FileHash $setup -Algorithm SHA256).Hash.ToLower()
$size = (Get-Item $setup).Length

# --- Durable symbols --------------------------------------------------------
#
# THIS IS IN THE SAME STEP AS THE INSTALLER, AND THAT IS THE POINT. The moment
# an installer exists, a binary exists that can reach a user, and every crash
# report that binary will ever produce is unreadable without the PDB from this
# exact link. A PDB cannot be regenerated: rebuilding the same source produces
# a different CodeView GUID and different offsets. So the only safe moment to
# copy it somewhere that outlives this machine is now, automatically, as part
# of the action that made the risk - not as a line in a checklist.
#
# The CMake POST_BUILD step has already put this build into $repo\symbols\,
# keyed by build id (tools/archive-symbols.ps1). This pushes that archive to
# the NAS, which is the same box and the same SSH key the git mirror uses.
#
# A failure here is FATAL rather than a warning. A warning at the end of a long
# build is a warning nobody reads, and the thing being risked is unrecoverable.
$symbolsDir = Join-Path $repo "symbols"

# Re-run the archiver against THIS binary to learn which entries belong to it.
# It is idempotent (it copies only when the file differs), so this is a no-op
# on top of the POST_BUILD run and costs nothing; what it buys is the exact
# list of archive paths for this build, so the mirror carries this nightly and
# not every incremental relink that happens to share the archive root.
$archived = @(& (Join-Path $repo "tools\archive-symbols.ps1") -Binary $exe `
    -ArchiveRoot $symbolsDir -Version $version -Commit $sha -EmitPaths)
if ($archived.Count -lt 3) {
    throw "the symbol archiver produced nothing for $exe - this build has no symbols anywhere, and a crash report against it could never be read"
}

if ($SkipSymbolMirror) {
    Write-Warning "symbol mirror SKIPPED - the only copy of this build's PDBs is $symbolsDir. If this disk is lost, every crash report against $version becomes unreadable, permanently."
} else {
    $mirrorHost, $mirrorPath = $SymbolMirror -split ":", 2
    if (-not $mirrorPath) { throw "SymbolMirror must be <sshhost>:<path>, got '$SymbolMirror'" }
    Write-Host "Mirroring symbols to $SymbolMirror" -ForegroundColor Cyan
    foreach ($rel in $archived) {
        $src = Join-Path $symbolsDir $rel
        if (-not (Test-Path -LiteralPath $src)) { throw "archiver named $rel but it is not on disk" }
        # POSIX separators on the far side; the archive layout is the symbol
        # server one and a debugger looks it up by that exact shape.
        $relPosix = $rel -replace '\\', '/'
        $destDir = "$mirrorPath/" + (Split-Path -Parent $relPosix -ErrorAction SilentlyContinue)
        if (-not (Split-Path -Parent $relPosix)) { $destDir = $mirrorPath }
        $destDir = $destDir -replace '\\', '/'
        & ssh -o BatchMode=yes $mirrorHost "mkdir -p '$destDir'"
        if ($LASTEXITCODE -ne 0) { throw "could not reach the symbol mirror at $SymbolMirror (use -SkipSymbolMirror only if you accept losing these symbols)" }
        # -O forces the LEGACY SCP protocol. Modern OpenSSH scp speaks SFTP by
        # default and this DSM box exposes no SFTP subsystem for this account,
        # so without -O every transfer dies with a bare "Connection closed"
        # that reads like a network fault and is not one. Measured, 2026-08-25.
        & scp -O -q -B "$src" "${mirrorHost}:$destDir/"
        if ($LASTEXITCODE -ne 0) { throw "symbol mirror copy of $rel failed - refusing to call this nightly finished" }
    }
    # PROOF IT ARRIVED, not merely that scp exited 0. A negative result needs
    # evidence the operation actually ran, so the PDB is read back by size.
    $pdbRel = ($archived[0] -replace '\\', '/')
    $remoteSize = (& ssh -o BatchMode=yes $mirrorHost "stat -c %s '$mirrorPath/$pdbRel' 2>/dev/null || echo 0" | Out-String).Trim()
    $localSize = (Get-Item -LiteralPath (Join-Path $symbolsDir $archived[0])).Length
    if ("$remoteSize" -ne "$localSize") {
        throw "symbol mirror verification failed: $pdbRel is $remoteSize bytes on the mirror, $localSize here"
    }
    Write-Host "  verified $pdbRel ($localSize bytes) on $SymbolMirror" -ForegroundColor Green

    # AND THE INDEX IS READABLE ON THE COPY THAT HAS TO OUTLIVE THIS MACHINE.
    # index.txt is mirrored wholesale in the loop above, so the local file
    # simply overwrites the remote one - but that only helps while the local
    # file is good, and the failure this guards against is silent by
    # construction. The mirror sat for a day with a UTF-8 BOM on row 1 (written
    # by the `Add-Content -Encoding utf8` this script's archiver used to use)
    # after the local copy had been repaired: `grep -c '^2026'` returned 9 of
    # 10 rows there and 10 of 10 here, and nothing anywhere noticed. A BOM is
    # invisible in every editor, so the only thing that can catch it is a byte
    # read, and the only place worth reading is the remote file itself.
    #
    # od rather than `head -c1`, because a raw 0xEF byte through the SSH
    # pipeline is not something to trust to text handling on either side.
    $indexFirst = (& ssh -o BatchMode=yes $mirrorHost "head -c 1 '$mirrorPath/index.txt' 2>/dev/null | od -An -tx1 | tr -d ' \n'" | Out-String).Trim()
    if ($indexFirst -eq "") {
        throw "symbol mirror verification failed: $mirrorPath/index.txt is missing or empty on $mirrorHost - the build id of this nightly cannot be mapped to a release on the durable copy"
    }
    if ($indexFirst -eq "ef") {
        throw "symbol mirror verification failed: $mirrorPath/index.txt on $mirrorHost begins 0xEF (a UTF-8 BOM), so an anchored parse silently drops its oldest row - re-mirror the repaired index before shipping this nightly"
    }
    Write-Host "  verified index.txt on $SymbolMirror starts 0x$indexFirst (no BOM)" -ForegroundColor Green
}

Write-Host ""
Write-Host "Nightly built:" -ForegroundColor Green
Write-Host "  file   : $setup"
Write-Host "  size   : $size"
Write-Host "  sha256 : $hash"
Write-Host ""
Write-Host "Paste into the website's version.go:" -ForegroundColor Cyan
Write-Host "    NightlyVersion  = `"$version`""
Write-Host "    NightlySHA256   = `"$hash`""
Write-Host "    NightlyCommit   = `"$sha`""
Write-Host "    NightlyBuilt    = `"$(Get-Date -Format 'yyyy-MM-dd')`""

if ($Publish) {
    $dest = Join-Path $SiteDir "downloads\foxsdr-setup-$version.exe"
    Copy-Item $setup $dest -Force
    Write-Host ""
    Write-Host "Copied to $dest" -ForegroundColor Green
    Write-Host "Now update version.go, run go test ./..., and deploy."
}
