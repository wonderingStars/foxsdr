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
#   pwsh tools/build-nightly.ps1                 # build, print the constants
#   pwsh tools/build-nightly.ps1 -Publish        # ...and copy to the website
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
    [switch]$Publish
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
