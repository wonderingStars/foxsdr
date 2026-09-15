# FoxSDR MSIX package builder.
#
#   powershell -ExecutionPolicy Bypass -File installer\msix\build-msix.ps1 `
#       [-BuildDir <dir>] [-VcCrtDir <dir>] `
#       [-IdentityName <name>] [-IdentityPublisher <X.500>] `
#       [-PublisherDisplayName <name>] [-TestSign] [-Force]
#
# Output: installer\msix\Output\FoxSDR-<version>-x64.msix
#
# WHAT IT DOES, in order:
#   1. reads the version from cascade.exe --version (the binary's own claim)
#   2. checks the payload contract - the same one installer\cascade.iss
#      enforces at compile time, so the two cannot come to disagree about what
#      "the runtime payload" is
#   3. stages that payload into a layout directory
#   4. renders every Store logo asset from the app's own icon geometry
#   5. fills the manifest template
#   6. runs makepri to index the assets, then makeappx to pack
#   7. WITH -TestSign ONLY: makes a self-signed certificate and signs the
#      package, FOR LOCAL TESTING ON THIS MACHINE AND NOTHING ELSE
#
# THE STORE BUILD IS SUBMITTED UNSIGNED. That is not a shortcut, it is the
# documented route:
#
#   "Your MSIX and AppX packages don't have to be signed with a certificate
#    rooted in a trusted certificate authority when submitting to the Microsoft
#    Store. The Microsoft Store will automatically re-sign your MSIX/AppX
#    packages with a Microsoft certificate during the publishing process after
#    your app passes certification... You don't need to purchase a CA-trusted
#    code signing certificate for MSIX/AppX Store submissions"
#   - learn.microsoft.com/windows/apps/publish/publish-your-app/msix/app-package-requirements
#     (ms.date 2022-10-30, updated_at 2026-08-24)
#
# A self-signed certificate is REJECTED for any Store route
# (".../publish/code-signing-options": "self-signed certificates are not
# accepted"). The one it makes here exists so the package can be installed on
# this machine and driven; it is written to the CurrentUser store, it is
# reported by name so it can be removed, and nothing signed with it may leave
# this machine.
#
# NO Get-Content / Set-Content ON SOURCE FILES ANYWHERE IN THIS SCRIPT.
# Round-tripping a UTF-8 file through Get-Content -Raw / .Replace() /
# Set-Content mangles every non-ASCII character and prepends a BOM; it has cost
# this project three separate repairs. Every read and write below goes through
# [System.IO.File] with an explicit encoding.
#
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

[CmdletBinding()]
param(
    # Where the Release build put cascade.exe and SoapySDR.dll.
    [string] $BuildDir = "",
    # The VS redist folder the app-local CRT comes from. Default matches
    # installer\cascade.iss, deliberately: two different CRTs in two different
    # packages of the same version would be a support nightmare.
    [string] $VcCrtDir = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Redist\MSVC\14.44.35112\x64\Microsoft.VC143.CRT",
    # Partner Center's Package/Identity/Name. Empty means "use the local test
    # identity", which the script says loudly.
    [string] $IdentityName = "",
    # Partner Center's Package/Identity/Publisher (an X.500 string).
    [string] $IdentityPublisher = "",
    [string] $PublisherDisplayName = "Steven Fairclough",
    # The MaxVersionTested to declare. Defaults to the SDK this machine has.
    [string] $MaxVersionTested = "10.0.26100.0",
    # Make a self-signed certificate and sign the package with it. LOCAL
    # TESTING ONLY - see the header.
    [switch] $TestSign,
    # Overwrite an existing output package.
    [switch] $Force
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

function Write-Step([string] $text) { Write-Host "==> $text" -ForegroundColor Cyan }
function Write-Note([string] $text) { Write-Host "    $text" }
function Fail([string] $text) { throw "build-msix: $text" }

# --- Where everything is ----------------------------------------------------

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot  = (Resolve-Path (Join-Path $scriptDir "..\..")).Path
if ([string]::IsNullOrEmpty($BuildDir)) {
    $BuildDir = Join-Path $repoRoot "build-win\Release"
}
if (-not (Test-Path -LiteralPath $BuildDir)) {
    Fail "build directory not found: $BuildDir - build the Release configuration first"
}
$BuildDir = (Resolve-Path -LiteralPath $BuildDir).Path

$layoutDir = Join-Path $scriptDir "Layout"
$outputDir = Join-Path $scriptDir "Output"
$assetsDir = Join-Path $layoutDir "Assets"

# The SDK tools. Newest build number wins, so this keeps working across SDK
# upgrades instead of pinning a number that will rot.
$sdkRoot = "C:\Program Files (x86)\Windows Kits\10\bin"
if (-not (Test-Path -LiteralPath $sdkRoot)) { Fail "Windows SDK not found at $sdkRoot" }
$sdkBin = Get-ChildItem -LiteralPath $sdkRoot -Directory |
          Where-Object { $_.Name -match '^10\.' -and (Test-Path (Join-Path $_.FullName "x64\makeappx.exe")) } |
          Sort-Object Name |
          Select-Object -Last 1
if ($null -eq $sdkBin) { Fail "no Windows SDK bin directory with x64\makeappx.exe under $sdkRoot" }
$makeappx = Join-Path $sdkBin.FullName "x64\makeappx.exe"
$makepri  = Join-Path $sdkBin.FullName "x64\makepri.exe"
$signtool = Join-Path $sdkBin.FullName "x64\signtool.exe"
foreach ($toolPath in @($makeappx, $makepri, $signtool)) {
    if (-not (Test-Path -LiteralPath $toolPath)) { Fail "SDK tool missing: $toolPath" }
}
Write-Step "Windows SDK tools: $($sdkBin.Name)"

# --- 1. The version, from the binary ----------------------------------------

$exePath = Join-Path $BuildDir "cascade.exe"
if (-not (Test-Path -LiteralPath $exePath)) { Fail "cascade.exe not found in $BuildDir" }

$versionLine = (& $exePath --version) | Select-Object -First 1
if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($versionLine)) {
    Fail "cascade.exe --version did not answer"
}
# "FoxSDR 0.96.0"
$displayVersion = ($versionLine -split '\s+')[1]
if ([string]::IsNullOrWhiteSpace($displayVersion)) { Fail "could not parse: $versionLine" }

# The MSIX Version attribute is four numeric parts and the fourth is reserved:
# "the last (fourth) section of the version number is reserved for Store use
# and must be left as 0 when you build your package". A pre-release suffix
# (0.96.0-nightly.20260915.abc1234) has no place in it, so only the numeric
# core is carried and the full string stays in the binary and in the listing.
$numericCore = ($displayVersion -split '-')[0]
$parts = $numericCore -split '\.'
while ($parts.Count -lt 3) { $parts += "0" }
# The Store refuses a first section of 0 ("except for the first section, which
# cannot be 0" - .../msix/app-package-requirements), so the package major is
# the product major PLUS ONE: product 0.96.4 -> package 1.96.4.0, and product
# 1.0.0 -> package 2.0.0.0. The mapping is monotonic across the 0.x -> 1.0
# boundary (the Store serves the highest package version, so a later product
# must always pack to a higher number) and it is mechanical, so a package
# version always reads back to exactly one product version. Decided 2026-09-15
# for the first submission; see installer\msix\README.md section 5.2.
$packageMajor = [int]$parts[0] + 1
$packageVersion = "{0}.{1}.{2}.0" -f $packageMajor, $parts[1], $parts[2]
Write-Step "version: $displayVersion  ->  package Version=$packageVersion (product major + 1, README 5.2)"

# --- 2. The payload contract ------------------------------------------------
#
# installer\cascade.iss aborts its compile if any DLL other than SoapySDR.dll
# appears in the build output. The same check lives here, for the same reason:
# a new runtime DLL must be added to BOTH packages deliberately, or neither.

Write-Step "payload contract"
$foundDlls = @(Get-ChildItem -LiteralPath $BuildDir -Filter *.dll -File | ForEach-Object { $_.Name })
$unexpected = @($foundDlls | Where-Object { $_.ToLowerInvariant() -ne "soapysdr.dll" })
if ($unexpected.Count -gt 0) {
    Fail ("unexpected runtime DLL(s) in {0}: {1} - the payload contract is cascade.exe + SoapySDR.dll only. Update installer\msix\build-msix.ps1 AND installer\cascade.iss deliberately." -f $BuildDir, ($unexpected -join ", "))
}
if ($foundDlls.Count -eq 0) { Fail "SoapySDR.dll not found in $BuildDir - the build is incomplete" }
Write-Note "cascade.exe + SoapySDR.dll, as installer\cascade.iss requires"

# --- 3. Stage the layout ----------------------------------------------------

Write-Step "staging layout: $layoutDir"
if (Test-Path -LiteralPath $layoutDir) { Remove-Item -LiteralPath $layoutDir -Recurse -Force }
New-Item -ItemType Directory -Path $layoutDir | Out-Null
New-Item -ItemType Directory -Path $assetsDir | Out-Null

# EXACTLY what installer\cascade.iss's [Files] section ships, item for item.
# Any divergence between the two packages is a different product wearing the
# same version number.
$payload = @(
    @{ From = (Join-Path $BuildDir "cascade.exe");        To = "cascade.exe" },
    @{ From = (Join-Path $BuildDir "SoapySDR.dll");       To = "SoapySDR.dll" },
    @{ From = (Join-Path $VcCrtDir "msvcp140.dll");       To = "msvcp140.dll" },
    @{ From = (Join-Path $VcCrtDir "vcruntime140.dll");   To = "vcruntime140.dll" },
    @{ From = (Join-Path $VcCrtDir "vcruntime140_1.dll"); To = "vcruntime140_1.dll" },
    @{ From = (Join-Path $repoRoot "LICENSE");                       To = "LICENSE.txt" },
    @{ From = (Join-Path $scriptDir "..\THIRD-PARTY-LICENSES.txt");  To = "THIRD-PARTY-LICENSES.txt" },
    @{ From = (Join-Path $scriptDir "..\POSTINSTALL.txt");           To = "POSTINSTALL.txt" }
)
foreach ($item in $payload) {
    if (-not (Test-Path -LiteralPath $item.From)) { Fail "payload file missing: $($item.From)" }
    Copy-Item -LiteralPath $item.From -Destination (Join-Path $layoutDir $item.To) -Force
    Write-Note "$($item.To)"
}

# Band plans. BandPlan::defaultDir() looks for resources\bandplans BESIDE
# cascade.exe, so inside the package that is <package root>\resources\bandplans
# - readable, though the package is read-only, which is all the overlay needs.
# The feature shipped inert up to 0.80.0 because no released build carried a
# single plan file; it is checked here rather than assumed.
$bandSrc = Join-Path $repoRoot "resources\bandplans"
$bandDst = Join-Path $layoutDir "resources\bandplans"
New-Item -ItemType Directory -Path $bandDst -Force | Out-Null
$plans = @(Get-ChildItem -LiteralPath $bandSrc -Filter *.json -File)
if ($plans.Count -eq 0) { Fail "no band plans found in $bandSrc" }
foreach ($plan in $plans) { Copy-Item -LiteralPath $plan.FullName -Destination $bandDst -Force }
Write-Note "resources\bandplans: $($plans.Count) plan(s)"

# NO PLUGINS ARE STAGED, and that is correct rather than an omission.
# installer\cascade.iss ships none either: decoder modules come from the
# catalogue, on a press, and are installed into a per-user directory. Inside a
# package that directory is %LOCALAPPDATA%\foxsdr\plugins
# (PluginHost::defaultPluginDir with the packaged branch), which is writable
# and which uninstalling the package cleans up.
Write-Note "plugins: none staged - same as the Inno installer; the catalogue delivers them"

# --- 4. Logo assets ---------------------------------------------------------

Write-Step "rendering logo assets"
$pythonExe = "py"
& $pythonExe -3.14 (Join-Path $scriptDir "make-logos.py") $assetsDir
if ($LASTEXITCODE -ne 0) { Fail "make-logos.py failed" }

# --- 5. The manifest --------------------------------------------------------

Write-Step "manifest"
$usingTestIdentity = $false
if ([string]::IsNullOrWhiteSpace($IdentityName)) {
    $IdentityName = "FoxSDR.Test.Unsubmittable"
    $usingTestIdentity = $true
}
if ([string]::IsNullOrWhiteSpace($IdentityPublisher)) {
    $IdentityPublisher = "CN=FoxSDR MSIX Test Signing (LOCAL ONLY), O=FoxSDR, C=GB"
    $usingTestIdentity = $true
}
if ($usingTestIdentity) {
    Write-Host "    ****************************************************************" -ForegroundColor Yellow
    Write-Host "    LOCAL TEST IDENTITY IN USE. This package CANNOT be submitted."   -ForegroundColor Yellow
    Write-Host "    Pass -IdentityName and -IdentityPublisher from Partner Center's" -ForegroundColor Yellow
    Write-Host "    Product management > Product identity page for a real build."    -ForegroundColor Yellow
    Write-Host "    ****************************************************************" -ForegroundColor Yellow
}
Write-Note "Identity Name      = $IdentityName"
Write-Note "Identity Publisher = $IdentityPublisher"

$templatePath = Join-Path $scriptDir "AppxManifest.xml"
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)
$manifestText = [System.IO.File]::ReadAllText($templatePath, [System.Text.Encoding]::UTF8)
$manifestText = $manifestText.Replace("@IDENTITY_NAME@", $IdentityName)
$manifestText = $manifestText.Replace("@IDENTITY_PUBLISHER@", $IdentityPublisher)
$manifestText = $manifestText.Replace("@PUBLISHER_DISPLAY_NAME@", $PublisherDisplayName)
$manifestText = $manifestText.Replace("@VERSION@", $packageVersion)
$manifestText = $manifestText.Replace("@MAX_VERSION_TESTED@", $MaxVersionTested)
if ($manifestText -match '@[A-Z_]+@') {
    Fail "the manifest template still has an unsubstituted token: $($Matches[0])"
}
$manifestPath = Join-Path $layoutDir "AppxManifest.xml"
[System.IO.File]::WriteAllText($manifestPath, $manifestText, $utf8NoBom)

# PARSE IT HERE, rather than letting makepri find out.
#
# makepri's answer to a malformed manifest is "PRI191: Appx manifest not found
# or is invalid", buried in a screenful of its own usage text, with the real
# reason ("Incorrect syntax was used in a comment") printed separately and
# UTF-16 encoded. This manifest carries long explanatory comments, and an XML
# comment may not contain a double hyphen - so writing a command-line switch
# into one silently breaks the build two tools downstream. XmlDocument names
# the line and the reason in one sentence.
try {
    $manifestXml = New-Object System.Xml.XmlDocument
    $manifestXml.PreserveWhitespace = $true
    $manifestXml.LoadXml($manifestText)
} catch {
    Fail ("the generated manifest is not well-formed XML: {0}" -f $_.Exception.Message)
}
Write-Note "wrote $manifestPath (well-formed)"

# --- 6. makepri, then makeappx ---------------------------------------------
#
# makepri is NOT optional. The manifest names Assets\Square44x44Logo.png; the
# eighty-odd scale- and targetsize-qualified variants beside it are only ever
# selected through resources.pri. Without it Windows uses the one unqualified
# file at every size, which is exactly the "undesirable backplate" and
# blurred-taskbar-icon outcome the asset table exists to avoid.

Write-Step "makepri"
# The config lives OUTSIDE the layout, so it is not itself indexed and does not
# end up inside the package.
$priWork = Join-Path ([System.IO.Path]::GetTempPath()) ("foxsdr-msix-pri-" + [System.Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $priWork | Out-Null
try {
    $priConfig = Join-Path $priWork "priconfig.xml"
    & $makepri createconfig /cf $priConfig /dq en-GB /o | Out-Null
    if ($LASTEXITCODE -ne 0) { Fail "makepri createconfig failed" }
    & $makepri new /pr $layoutDir /cf $priConfig /of (Join-Path $layoutDir "resources.pri") /mn $manifestPath /o
    if ($LASTEXITCODE -ne 0) { Fail "makepri new failed" }
    if (-not (Test-Path -LiteralPath (Join-Path $layoutDir "resources.pri"))) {
        Fail "makepri reported success but wrote no resources.pri"
    }
} finally {
    Remove-Item -LiteralPath $priWork -Recurse -Force -ErrorAction SilentlyContinue
}
Write-Note "resources.pri written"

Write-Step "makeappx pack"
if (-not (Test-Path -LiteralPath $outputDir)) { New-Item -ItemType Directory -Path $outputDir | Out-Null }
$msixPath = Join-Path $outputDir ("FoxSDR-{0}-x64.msix" -f $displayVersion)
if (Test-Path -LiteralPath $msixPath) {
    if (-not $Force) { Fail "$msixPath already exists - pass -Force to overwrite" }
    Remove-Item -LiteralPath $msixPath -Force
}
& $makeappx pack /d $layoutDir /p $msixPath /o
if ($LASTEXITCODE -ne 0) { Fail "makeappx pack failed" }
$msixSize = (Get-Item -LiteralPath $msixPath).Length
Write-Note ("{0} ({1:N0} bytes)" -f $msixPath, $msixSize)

# --- 7. Test signing, and ONLY test signing ---------------------------------

if ($TestSign) {
    Write-Step "TEST SIGNING - local only, never submitted"
    # The certificate's Subject must be BYTE-IDENTICAL to the manifest's
    # Identity/Publisher or the signature is refused: "the publisher name that
    # you choose matches the name on the certificate you use to sign your app"
    # (.../desktop-to-uwp-manual-conversion).
    $existing = @(Get-ChildItem Cert:\CurrentUser\My |
                  Where-Object { $_.Subject -eq $IdentityPublisher })
    if ($existing.Count -gt 0) {
        $cert = $existing[0]
        Write-Note "reusing certificate $($cert.Thumbprint)"
    } else {
        $cert = New-SelfSignedCertificate `
            -Type Custom `
            -Subject $IdentityPublisher `
            -KeyUsage DigitalSignature `
            -FriendlyName "FoxSDR MSIX LOCAL TEST - safe to delete" `
            -CertStoreLocation "Cert:\CurrentUser\My" `
            -NotAfter (Get-Date).AddDays(30) `
            -TextExtension @("2.5.29.37={text}1.3.6.1.5.5.7.3.3", "2.5.29.19={text}Subject Type:End Entity")
        Write-Note "created certificate $($cert.Thumbprint) (expires in 30 days)"
    }

    & $signtool sign /fd SHA256 /sha1 $cert.Thumbprint /t http://timestamp.digicert.com $msixPath
    if ($LASTEXITCODE -ne 0) {
        # A timestamp needs the network; without one the signature is still
        # valid until the certificate expires, which for a 30-day test cert is
        # all it has to be.
        Write-Note "timestamped signing failed; retrying without a timestamp (test cert only)"
        & $signtool sign /fd SHA256 /sha1 $cert.Thumbprint $msixPath
        if ($LASTEXITCODE -ne 0) { Fail "signtool failed" }
    }
    # `signtool verify` is run for its DIAGNOSIS, not as a gate, and it is
    # EXPECTED TO FAIL here with "terminated in a root certificate which is not
    # trusted by the trust provider" until the certificate is imported into the
    # machine's Trusted People store (README.md section 7). Any OTHER failure
    # means the signature itself is wrong, which is why it is run at all.
    # `signtool verify` is run for its DIAGNOSIS, not as a gate.
    #
    # NO `2>&1` HERE, deliberately. In Windows PowerShell 5.1, redirecting a
    # native command's stderr inside the shell wraps every line in an
    # ErrorRecord (NativeCommandError); with $ErrorActionPreference = "Stop"
    # that is TERMINATING, so the redirect alone made this script abort after a
    # completely successful build. stderr goes to the console on its own.
    Write-Note "verifying (an untrusted-root complaint here is EXPECTED and correct"
    Write-Note "until the certificate is trusted - see README.md section 7):"
    & $signtool verify /pa $msixPath
    $verifyCode = $LASTEXITCODE
    if ($verifyCode -eq 0) {
        Write-Note "signtool verify: trusted on this machine already"
    } else {
        Write-Note "signtool verify: exit $verifyCode - read the message above. Anything other"
        Write-Note "than an untrusted root means the SIGNATURE is wrong, not the trust."
    }
    # Discarded on purpose: the point of the call is that it is allowed to
    # complain, and a stale $LASTEXITCODE would make this script exit non-zero.
    $global:LASTEXITCODE = 0

    $cerPath = Join-Path $outputDir "FoxSDR-LOCAL-TEST.cer"
    [System.IO.File]::WriteAllBytes($cerPath, $cert.Export([System.Security.Cryptography.X509Certificates.X509ContentType]::Cert))
    Write-Note "exported the public certificate to $cerPath"
    Write-Host ""
    Write-Host "    TO INSTALL FOR TESTING (and see README.md, which says how to undo it):" -ForegroundColor Yellow
    Write-Host "      Import-Certificate -FilePath '$cerPath' -CertStoreLocation Cert:\CurrentUser\TrustedPeople" -ForegroundColor Yellow
    Write-Host "      Add-AppxPackage -Path '$msixPath'" -ForegroundColor Yellow
    Write-Host "    TO REMOVE EVERYTHING AFTERWARDS:" -ForegroundColor Yellow
    Write-Host "      Get-AppxPackage '$IdentityName' | Remove-AppxPackage" -ForegroundColor Yellow
    Write-Host "      Get-ChildItem Cert:\CurrentUser\TrustedPeople,Cert:\CurrentUser\My | Where-Object Thumbprint -eq '$($cert.Thumbprint)' | Remove-Item" -ForegroundColor Yellow
} else {
    Write-Step "NOT signed - this is the shape a Store submission takes"
    Write-Note "Microsoft re-signs MSIX packages after certification; pass -TestSign"
    Write-Note "only to install it on this machine."
}

Write-Host ""
Write-Step "done: $msixPath"
exit 0
