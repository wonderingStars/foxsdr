# Loads the SDK's built plugin with FoxSDR's OWN plugin host, using the existing test test_plugin_abi3_compat
# (tests/test_plugin_abi3_compat.cpp: PluginHost scans and validates, PluginRunner creates and feeds every decoder).
# Nothing is installed: only the module is copied into a scratch folder and that folder is named in
# FOXSDR_PUBLISHED_PLUGINS_DIR. The test points APPDATA/LOCALAPPDATA/HOME at a scratch profile itself.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File sdk\tests\host_load_check.ps1 `
#       -SdkBuildDir build-sdk -FoxsdrBuildDir <a FoxSDR build folder with test_plugin_abi3_compat built>
#
# Exit 0 only if the module was LOADED, its decoder ran, and the test reported no failed check.
param(
    [Parameter(Mandatory = $true)] [string] $SdkBuildDir,
    [Parameter(Mandatory = $true)] [string] $FoxsdrBuildDir,
    [string] $Configuration = 'Release',
    [int] $TimeoutSeconds = 240
)
$ErrorActionPreference = 'Stop'

$module = @(Get-ChildItem -LiteralPath (Join-Path $SdkBuildDir $Configuration) -Filter 'heartbeat-decoder-*-abi3-*' |
    Where-Object { $_.Extension -in '.dll', '.so', '.dylib' })
if ($module.Count -ne 1) { Write-Error ('expected exactly one built module in ' + $SdkBuildDir + '\' + $Configuration + ', found ' + $module.Count); exit 1 }
$exe = Join-Path $FoxsdrBuildDir ('tests\' + $Configuration + '\test_plugin_abi3_compat.exe')
if (-not (Test-Path -LiteralPath $exe)) { Write-Error ('build test_plugin_abi3_compat first: ' + $exe + ' is missing'); exit 1 }

$scratch = Join-Path ([IO.Path]::GetTempPath()) ('sdk-host-load-' + $PID)
New-Item -ItemType Directory -Force -Path $scratch | Out-Null
Copy-Item -LiteralPath $module[0].FullName -Destination $scratch
$log = Join-Path $scratch 'out.txt'
$env:FOXSDR_PUBLISHED_PLUGINS_DIR = $scratch
$p = Start-Process -FilePath $exe -WindowStyle Hidden -PassThru -RedirectStandardOutput $log -RedirectStandardError ($log + '.err')
if (-not $p.WaitForExit($TimeoutSeconds * 1000)) { Write-Error 'the host test did not finish in time (left running, not killed)'; exit 1 }
$text = Get-Content -Raw -LiteralPath $log
Write-Host $text
$ok = ($p.ExitCode -eq 0) -and ($text -match 'LOADED\s+Heartbeat Decoder') -and ($text -match '1 of 1 published binaries loaded') -and ($text -match '1 running') -and ($text -match '(\d+) text lines' -and [int]$Matches[1] -gt 0)
Remove-Item -LiteralPath $scratch -Recurse -Force -ErrorAction SilentlyContinue
if (-not $ok) { Write-Error 'the plugin was not loaded and run by the host (see the output above)'; exit 1 }
Write-Host 'host load check: PASS'
exit 0
