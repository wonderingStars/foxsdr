# How many people are using FoxSDR.
#
# The Worker has been counting since it was deployed; this is the other half -
# reading the answer back. Analytics Engine has no dashboard of its own, so
# without something like this the data is collected and never seen, which is
# indistinguishable from not collecting it.
#
# WHAT YOU NEED, once:
#
#   1. Cloudflare dashboard -> My Profile -> API Tokens -> Create Token
#      -> Custom token, with exactly one permission:
#           Account | Account Analytics | Read
#      Scope it to your account and nothing else. It needs no zone access, no
#      write anywhere, and it cannot post to the Worker.
#   2. $env:CLOUDFLARE_API_TOKEN = "<the token>"
#
# The account id is read from wrangler's cache if it is there, so normally you
# only supply the token. Neither value is stored by this script.
#
# Run it with Windows PowerShell (powershell), NOT pwsh: PowerShell 7 is not
# installed on the machine this project is developed on, and "pwsh is not
# recognized" is a confusing first thing to meet. Nothing here needs 7.
#
#   powershell -File telemetry-worker/usage.ps1              # last 30 days
#   powershell -File telemetry-worker/usage.ps1 -Days 1      # yesterday
#   powershell -File telemetry-worker/usage.ps1 -Raw "SELECT ..."   # any query
#
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

[CmdletBinding()]
param(
    [int]$Days = 30,
    [string]$AccountId = $env:CLOUDFLARE_ACCOUNT_ID,
    [string]$Token = $env:CLOUDFLARE_API_TOKEN,
    [string]$Raw,
    # INSTALL IDS THAT ARE NOT USERS. Every number this script prints is meant
    # to answer "how many people use this", and the development machine is not
    # one of them - it launches the app dozens of times a day, most of them for
    # thirty seconds to check a build.
    #
    # This is not hypothetical tidiness. On 2026-08-31 a testing session ran a
    # dev build interactively with the owner's own config, and put thirteen
    # reports into the dataset under this id - six of them tagged 0.66.0 and one
    # 0.65.2, versions that were never released to anyone - along with a crash
    # count inflated from 1 to 6, because every kill of a test instance is an
    # unclean exit and an unclean exit IS how this product counts crashes.
    # Analytics Engine is append-only, so those rows cannot be deleted; the only
    # way to make the numbers honest again is to leave this install out when
    # reading them, which is what this does.
    #
    # Filtering by VERSION would not have worked: four of the thirteen are
    # tagged 0.64.0 and are indistinguishable from real use of the shipped
    # build by anything except the id.
    #
    # Pass -ExcludeInstalls @() to see the unfiltered dataset.
    [string[]]$ExcludeInstalls = @('397c600669cd9fa2dfb4b7d911edb70c')
)

$ErrorActionPreference = "Stop"

if (-not $Token) {
    Write-Error "No API token. Set CLOUDFLARE_API_TOKEN - see the header of this script for the one permission it needs (Account Analytics: Read)."
}

if (-not $AccountId) {
    # wrangler leaves the account id in its cache after a deploy. It is not a
    # secret, but it is not hard-coded here either: this file is public and the
    # repository has already had one Cloudflare account file scrubbed out of
    # its history.
    $cache = Join-Path $PSScriptRoot ".wrangler\cache\wrangler-account.json"
    if (Test-Path $cache) {
        $AccountId = (Get-Content $cache -Raw | ConvertFrom-Json).account.id
    }
}
if (-not $AccountId) {
    Write-Error "No account id. Set CLOUDFLARE_ACCOUNT_ID, or run 'npx wrangler deploy' once so the id is cached."
}

$uri = "https://api.cloudflare.com/client/v4/accounts/$AccountId/analytics_engine/sql"

function Invoke-Sql([string]$sql) {
    try {
        $res = Invoke-RestMethod -Uri $uri -Method POST -Body $sql `
            -Headers @{ Authorization = "Bearer $Token" } -ContentType "text/plain"
    } catch {
        $code = $null
        if ($_.Exception.Response) { $code = $_.Exception.Response.StatusCode.value__ }
        if ($code -eq 401 -or $code -eq 403) {
            Write-Error "Cloudflare refused the token ($code). It needs Account Analytics: Read on account $AccountId."
        }
        throw
    }
    # The SQL API answers with {"meta":[...],"data":[...],"rows":N}. An empty
    # data array is a real answer - nobody has reported in this window - and is
    # NOT an error, which matters on a young dataset where it is the normal case.
    return $res.data
}

# The dataset only exists once something has been written to it. Querying an
# absent table is an error, not an empty result, so say which case this is
# rather than letting a raw API error stand.
# Sanitised to hex before it reaches a query: these ids arrive from a command
# line and are pasted straight into SQL.
$exclude = ""
if ($ExcludeInstalls) {
    $clean = @($ExcludeInstalls |
        ForEach-Object { ($_ -replace '[^0-9a-fA-F]', '').ToLower() } |
        Where-Object { $_.Length -gt 0 })
    if ($clean.Count -gt 0) {
        $exclude = " AND index1 NOT IN (" + (($clean | ForEach-Object { "'$_'" }) -join ", ") + ")"
    }
}

$window = "timestamp > NOW() - INTERVAL '$Days' DAY$exclude"

if ($Raw) {
    Invoke-Sql $Raw | ConvertTo-Json -Depth 6
    return
}

Write-Host ""
Write-Host "FoxSDR usage - last $Days days" -ForegroundColor Cyan
Write-Host ("=" * 40)
# SAID OUT LOUD, EVERY RUN. A number that quietly leaves rows out is worse than
# one that does not, because the reader cannot tell which they are looking at -
# so the exclusion is printed whenever it is in force, and its absence is
# printed too.
if ($exclude) {
    Write-Host ("Excluding $($clean.Count) development install(s) - these are not users." ) -ForegroundColor DarkGray
} else {
    Write-Host "No installs excluded: development machines are counted as users." -ForegroundColor Yellow
}

# THE headline number. count(DISTINCT index1), never uniq(): uniq() is
# approximate and reads LOW on small samples, which is exactly the sample size
# this project has, so it would under-report the thing it exists to report.
$installs = Invoke-Sql "SELECT count(DISTINCT index1) AS installs, count() AS reports FROM foxsdr_usage WHERE $window"
if (-not $installs) {
    Write-Host "No reports in this window." -ForegroundColor Yellow
    Write-Host "That is a real answer, not an error - either nobody has run a build with"
    Write-Host "reporting left on, or the window is too short."
    return
}
$i = $installs[0]
Write-Host ("installs (unique) : {0}" -f $i.installs)
Write-Host ("reports (launches): {0}" -f $i.reports)

# The two figures people actually mean by "how many users".
foreach ($d in 1, 7) {
    $r = Invoke-Sql "SELECT count(DISTINCT index1) AS installs FROM foxsdr_usage WHERE timestamp > NOW() - INTERVAL '$d' DAY$exclude"
    if ($r) {
        $label = if ($d -eq 1) { "active today" } else { "active in 7 days" }
        Write-Host ("{0,-18}: {1}" -f $label, $r[0].installs)
    }
}

function Show-Breakdown([string]$title, [string]$sql, [string]$key) {
    $rows = Invoke-Sql $sql
    if (-not $rows) { return }
    Write-Host ""
    Write-Host $title -ForegroundColor Cyan
    foreach ($r in $rows) {
        $label = $r.$key
        if (-not $label) { $label = "(not reported)" }
        Write-Host ("  {0,-34} {1}" -f $label, $r.installs)
    }
}

# DAILY ACTIVE INSTALLS, which is the number most worth watching over time.
#
# One row per day, counting DISTINCT install ids that reported that day. A
# report is written once per LAUNCH, so "active" here means "started FoxSDR at
# least once that day" - not "had it open", which this data cannot answer and
# which no honest label should imply.
#
# The daily figure is always lower than the 30-day one and that is not an
# error: most people do not use a receiver every day, so a healthy 30-day
# population of N produces a daily figure well below N.
$daily = Invoke-Sql @"
SELECT toDate(timestamp) AS day,
       count(DISTINCT index1) AS installs,
       count() AS launches
FROM foxsdr_usage
WHERE $window
GROUP BY day
ORDER BY day DESC
"@
if ($daily) {
    Write-Host ""
    Write-Host "Daily active installs" -ForegroundColor Cyan
    # Right alignment in a .NET format string is a POSITIVE width; ">" is not a
    # thing and throws "Input string was not in a correct format". The data rows
    # below always had it right, so only the HEADER threw - the one line no test
    # would have looked at, and the first thing a reader sees.
    Write-Host ("  {0,-12} {1,8} {2,10}" -f "day", "installs", "launches")
    foreach ($r in $daily) {
        Write-Host ("  {0,-12} {1,8} {2,10}" -f $r.day, $r.installs, $r.launches)
    }
    # A crude trend, because a single day means little on its own: the average
    # over the window, so today can be read against it.
    $avg = ($daily | Measure-Object -Property installs -Average).Average
    Write-Host ("  {0,-12} {1,8:N1}" -f "average", $avg)
}

Show-Breakdown "By version" `
    "SELECT blob1 AS version, count(DISTINCT index1) AS installs FROM foxsdr_usage WHERE $window GROUP BY blob1 ORDER BY installs DESC" `
    "version"

Show-Breakdown "By operating system" `
    "SELECT blob2 AS os, count(DISTINCT index1) AS installs FROM foxsdr_usage WHERE $window GROUP BY blob2 ORDER BY installs DESC" `
    "os"

Show-Breakdown "By radio" `
    "SELECT blob4 AS sdr, count(DISTINCT index1) AS installs FROM foxsdr_usage WHERE $window AND blob4 != '' GROUP BY blob4 ORDER BY installs DESC" `
    "sdr"

Show-Breakdown "Most-used demodulator" `
    "SELECT blob5 AS mode, count(DISTINCT index1) AS installs FROM foxsdr_usage WHERE $window AND blob5 != '' GROUP BY blob5 ORDER BY installs DESC" `
    "mode"

# Stability, per install rather than per report: one person launching a
# hundred times must not look like a hundred stable users.
$crash = Invoke-Sql "SELECT sum(double2) AS crashes, count(DISTINCT index1) AS installs FROM foxsdr_usage WHERE $window"
if ($crash -and $crash[0].installs -gt 0) {
    Write-Host ""
    Write-Host "Stability" -ForegroundColor Cyan
    Write-Host ("  unclean exits reported          {0}" -f $crash[0].crashes)
    Write-Host ("  per install                     {0:N2}" -f ($crash[0].crashes / $crash[0].installs))
}
Write-Host ""
