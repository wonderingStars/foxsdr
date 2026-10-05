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
    [string[]]$ExcludeInstalls = @('397c600669cd9fa2dfb4b7d911edb70c', '8a7e3dd06082265df1e21fc7e2d47ed3'),  # the owner's old and current desktop ids
    # WHERE THE SQL API IS. Only the tests change this: telemetry-worker/
    # worker.test.mjs points it at a stand-in on loopback to read back the
    # queries this script sends and to feed it canned rows, so the reader is
    # exercised without a token, an account or a network. The token goes to
    # whatever this names, so never set it from anything you did not write.
    [string]$ApiBase = "https://api.cloudflare.com/client/v4"
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

$uri = "$ApiBase/accounts/$AccountId/analytics_engine/sql"

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

# DISPLAY STALLS, per version - how often the window freezes because the display
# driver was waiting, which the crash store cannot say: those reports are kept
# on the user's machine on purpose, so this one number is all that reaches us.
#
# Three queries, the install counts being count(DISTINCT index1) and never
# uniq(), and the two that count stalls asking only about rows with blob11 = '1'
# - the rows whose client SENT a stall count. That condition is the whole point
# of the section: an old build, and every row written before the field existed,
# has no count, and counting them as zero would print a confident zero for a
# number nobody has measured. Those versions are listed apart, as unmeasured -
# worked out here by subtracting the versions that report from every version
# seen, so nothing depends on what an unwritten column reads back as (which
# Cloudflare does not document). (Without credentials the script never reaches here: the token
# check at the top stops it before anything is printed, so there is no table of
# zeroes for data that could not be read.)
Write-Host ""
Write-Host "Display stalls, by version" -ForegroundColor Cyan
$reporting = Invoke-Sql "SELECT blob1 AS version, count(DISTINCT index1) AS installs FROM foxsdr_usage WHERE $window AND blob11 = '1' GROUP BY blob1 ORDER BY installs DESC"
if (-not $reporting) {
    Write-Host "  No build in this window reports a stall count yet."
    Write-Host "  That is not zero stalls - it is unmeasured."
} else {
    $stalled = Invoke-Sql "SELECT blob1 AS version, count(DISTINCT index1) AS installs, sum(double5) AS stalls FROM foxsdr_usage WHERE $window AND blob11 = '1' AND double5 > 0 GROUP BY blob1"
    $byVersion = @{}
    foreach ($s in @($stalled)) { $byVersion[[string]$s.version] = $s }
    Write-Host ("  {0,-34} {1,8} {2,12} {3,8}" -f "version", "installs", "with stalls", "stalls")
    foreach ($r in $reporting) {
        $hit = $byVersion[[string]$r.version]
        $with = 0
        $total = 0
        if ($hit) { $with = [int64]$hit.installs; $total = [int64]$hit.stalls }
        $label = [string]$r.version
        if (-not $label) { $label = "(not reported)" }
        Write-Host ("  {0,-34} {1,8} {2,12} {3,8}" -f $label, $r.installs, $with, $total)
    }
    Write-Host "  installs = distinct installs on that version that reported a count; stalls = the sum of the counts."
    $seen = Invoke-Sql "SELECT blob1 AS version FROM foxsdr_usage WHERE $window GROUP BY blob1 ORDER BY blob1"
    $reportsCount = @{}
    foreach ($r in $reporting) { $reportsCount[[string]$r.version] = $true }
    $names = @()
    foreach ($v in @($seen)) {
        if (-not $reportsCount.ContainsKey([string]$v.version)) {
            if ($v.version) { $names += [string]$v.version } else { $names += "(not reported)" }
        }
    }
    if ($names.Count -gt 0) {
        Write-Host ("  Not measured (these builds send no count): {0}" -f ($names -join ", "))
    }
}

# WHAT FAILS, by version (0.99.64) - the failures that are NOT crashes: a radio that
# would not open, no sound output, an update or a plugin install that failed. The
# crash store cannot see these (the application did not crash, it did not work),
# and a release in which something quietly stops working is exactly what this
# section exists to show. Five releases once shipped that detected no radio at
# all; 46 of the 49 people who took one never came back.
#
# THE SAME DISCIPLINE AS THE STALLS ABOVE. Every question asks only about rows
# with blob12 = '1' - the rows whose client SENT the failure counts - so an old
# build, and every row written before the field existed, is never counted as
# "nothing failed". Those versions are listed apart as not measured, worked out
# by subtracting the versions that report from every version seen. Install counts
# are count(DISTINCT index1), never uniq(). (Without credentials the script never
# reaches here: the token check at the top stops it before anything is printed.)
#
# THE COLUMNS (README.md): blob12 is the marker, blob13 the `token=count` text
# (vocabulary only - the Worker drops anything else), double6..double9 the sums of
# radio failures, radio opens, radios whose samples arrived and sessions with
# sound. The sums come from the validated tokens, never from a number the client
# sent beside them. One row is one record; a record carries every count the
# client had not yet had accepted, so a count is "times", a row is "a session
# (or more, after a failed send)".
Write-Host ""
Write-Host "What fails, by version" -ForegroundColor Cyan
$healthReporting = Invoke-Sql "SELECT blob1 AS version, count(DISTINCT index1) AS installs, count() AS sessions, sum(double6) AS radioFail, sum(double7) AS radioOpen, sum(double8) AS radioData, sum(double9) AS sound FROM foxsdr_usage WHERE $window AND blob12 = '1' GROUP BY blob1 ORDER BY installs DESC"
if (-not $healthReporting) {
    Write-Host "  No build in this window reports failure counts yet."
    Write-Host "  That is not zero failures - it is unmeasured."
} else {
    Write-Host "  Per version (only builds that send the counts):"
    Write-Host ("  {0,-34} {1,8} {2,9} {3,11} {4,11} {5,9} {6,10}" -f "version", "installs", "sessions", "radio fail", "radio open", "samples", "w/ sound")
    foreach ($r in $healthReporting) {
        $label = [string]$r.version
        if (-not $label) { $label = "(not reported)" }
        Write-Host ("  {0,-34} {1,8} {2,9} {3,11} {4,11} {5,9} {6,10}" -f $label, $r.installs, $r.sessions, $r.radioFail, $r.radioOpen, $r.radioData, $r.sound)
    }
    Write-Host "  radio fail = radio opens that failed; radio open = opens that succeeded; samples = opened radios whose samples reached the display;"
    Write-Host "  w/ sound = sessions in which the speakers played. A reporting version with a zero is a real zero."

    # Every failure token seen, per version. The tokens are read out of the
    # distinct count strings (one row per distinct string and version), so the
    # sessions and the total are exact; the installs are asked for per token with
    # count(DISTINCT index1), which cannot be added up from the strings.
    $stringRows = Invoke-Sql "SELECT blob1 AS version, blob13 AS health, count() AS sessions FROM foxsdr_usage WHERE $window AND blob12 = '1' AND blob13 != '' GROUP BY blob1, blob13 ORDER BY sessions DESC LIMIT 1000"
    $stat = @{}
    $tokens = @{}
    foreach ($row in @($stringRows)) {
        $ver = [string]$row.version
        $n = [int64]$row.sessions
        foreach ($pair in ([string]$row.health).Split(',')) {
            $eq = $pair.IndexOf('=')
            if ($eq -lt 1) { continue }
            $tok = $pair.Substring(0, $eq)
            # Pasted into SQL below: only what the vocabulary is made of.
            if ($tok -notmatch '^[a-z0-9_.]+$') { continue }
            # The same text also carries the slow frames and the recoveries, which
            # have their own sections below; their tokens are not failures.
            if ($tok -match '^(slow|recovered)\.') { continue }
            $cnt = [int64]0
            if (-not [int64]::TryParse($pair.Substring($eq + 1), [ref]$cnt)) { continue }
            $key = "$ver|$tok"
            if (-not $stat.ContainsKey($key)) {
                $stat[$key] = @{ version = $ver; token = $tok; installs = [int64]0; sessions = [int64]0; total = [int64]0 }
            }
            $stat[$key].sessions += $n
            $stat[$key].total += $cnt * $n
            $tokens[$tok] = $true
        }
    }
    foreach ($tok in @($tokens.Keys | Sort-Object)) {
        $per = Invoke-Sql "SELECT blob1 AS version, count(DISTINCT index1) AS installs FROM foxsdr_usage WHERE $window AND blob12 = '1' AND (startsWith(blob13, '$tok=') OR position(blob13, ',$tok=') > 0) GROUP BY blob1"
        foreach ($p in @($per)) {
            $k = "$([string]$p.version)|$tok"
            if ($stat.ContainsKey($k)) { $stat[$k].installs = [int64]$p.installs }
        }
    }
    Write-Host ""
    if ($stat.Count -eq 0) {
        Write-Host "  No failure was reported by any build in this window."
    } else {
        Write-Host "  Failures by event (events are written driver.reason, api.reason or class - see PRIVACY.md):"
        Write-Host ("  {0,-34} {1,-32} {2,8} {3,9} {4,7}" -f "version", "event", "installs", "sessions", "count")
        $ordered = $stat.Values | Sort-Object -Property @{ Expression = { $_.version } }, @{ Expression = { $_.sessions }; Descending = $true }, @{ Expression = { $_.token } }
        foreach ($s in $ordered) {
            $label = [string]$s.version
            if (-not $label) { $label = "(not reported)" }
            Write-Host ("  {0,-34} {1,-32} {2,8} {3,9} {4,7}" -f $label, $s.token, $s.installs, $s.sessions, $s.total)
        }
        Write-Host "  installs = distinct installs with the event; sessions = records carrying it; count = times it happened (sound_ok and scan_none count once a session)."
        if (@($stringRows).Count -ge 1000) {
            Write-Host "  (the 1000 most common count strings were read; the rarest are not shown)"
        }
    }
    $seenHealth = Invoke-Sql "SELECT blob1 AS version FROM foxsdr_usage WHERE $window GROUP BY blob1 ORDER BY blob1"
    $healthReports = @{}
    foreach ($r in $healthReporting) { $healthReports[[string]$r.version] = $true }
    $healthNames = @()
    foreach ($v in @($seenHealth)) {
        if (-not $healthReports.ContainsKey([string]$v.version)) {
            if ($v.version) { $healthNames += [string]$v.version } else { $healthNames += "(not reported)" }
        }
    }
    if ($healthNames.Count -gt 0) {
        Write-Host ("  Not measured (these builds send no failure counts): {0}" -f ($healthNames -join ", "))
    }
}

# SLOW FRAMES, by version (0.99.64, the second pass) - how often the window took a
# quarter of a second or more to draw a frame, and which part of the frame the time
# went in. The hang watchdog fires at five seconds and the crash store sees only
# what ends in a report; a stutter that never becomes a freeze left no trace at all.
#
# THE SAME DISCIPLINE: only rows with blob12 = '1' (the client SENT its counts; 0.99.64
# is the first build to send either family, so such a row measured its frames), the
# rest listed apart by the "What fails" section above. Installs are count(DISTINCT
# index1), never uniq(). The sums are double10 (every slow frame) and double11
# (those of a second or more), computed by the Worker from the validated tokens; the
# scopes are read out of the count strings (blob13) exactly as the failures are, and
# each scope's installs asked for with count(DISTINCT index1). `user-wait` is never a
# scope: the application does not count it, and the Worker drops it.
Write-Host ""
Write-Host "Slow frames, by version" -ForegroundColor Cyan
$slowVersions = Invoke-Sql "SELECT blob1 AS version, count(DISTINCT index1) AS installs, count() AS sessions, sum(double10) AS slow, sum(double11) AS slowLong FROM foxsdr_usage WHERE $window AND blob12 = '1' GROUP BY blob1 ORDER BY installs DESC"
if (-not $slowVersions) {
    Write-Host "  No build in this window reports slow frames yet."
    Write-Host "  That is not zero slow frames - it is unmeasured."
} else {
    $withSlow = Invoke-Sql "SELECT blob1 AS version, count(DISTINCT index1) AS installs FROM foxsdr_usage WHERE $window AND blob12 = '1' AND double10 > 0 GROUP BY blob1"
    $withSlowBy = @{}
    foreach ($s in @($withSlow)) { $withSlowBy[[string]$s.version] = [int64]$s.installs }
    Write-Host ("  {0,-34} {1,8} {2,9} {3,10} {4,11} {5,11}" -f "version", "installs", "sessions", "w/ slow", "per 1000", "1 s+ /1000")
    foreach ($r in $slowVersions) {
        $label = [string]$r.version
        if (-not $label) { $label = "(not reported)" }
        $with = 0
        if ($withSlowBy.ContainsKey([string]$r.version)) { $with = $withSlowBy[[string]$r.version] }
        $sessions = [double]$r.sessions
        $per = 0.0
        $perLong = 0.0
        if ($sessions -gt 0) { $per = 1000.0 * [double]$r.slow / $sessions; $perLong = 1000.0 * [double]$r.slowLong / $sessions }
        Write-Host ("  {0,-34} {1,8} {2,9} {3,10} {4,11:F1} {5,11:F1}" -f $label, $r.installs, $r.sessions, $with, $per, $perLong)
    }
    Write-Host "  installs = distinct installs that reported; w/ slow = those with at least one slow frame (250 ms or more); per 1000 = slow frames"
    Write-Host "  per 1,000 records (a record is a session); 1 s+ = the slow frames of a second or more. A reporting version with a zero is a real zero."

    # The scopes: one row per distinct count string and version, so sessions and
    # frames are exact; installs per scope come from the database, per version.
    $slowStrings = Invoke-Sql "SELECT blob1 AS version, blob13 AS slowHealth, count() AS sessions FROM foxsdr_usage WHERE $window AND blob12 = '1' AND position(blob13, 'slow.') > 0 GROUP BY blob1, blob13 ORDER BY sessions DESC LIMIT 1000"
    $scopeStat = @{}
    $scopes = @{}
    foreach ($row in @($slowStrings)) {
        $ver = [string]$row.version
        $n = [int64]$row.sessions
        $seenScope = @{}
        foreach ($pair in ([string]$row.slowHealth).Split(',')) {
            $eq = $pair.IndexOf('=')
            if ($eq -lt 1) { continue }
            # Pasted into SQL below: only a scope and a tier of the vocabulary.
            if ($pair.Substring(0, $eq) -notmatch '^slow\.([a-z]+(-[a-z]+)*)\.(250ms|1s|5s)$') { continue }
            $scope = $Matches[1]
            $tier = $Matches[3]
            # Never a word of the vocabulary (the application does not count it and the
            # Worker drops it); refused here as well so it can never reach a query.
            if ($scope -eq 'user-wait') { continue }
            $cnt = [int64]0
            if (-not [int64]::TryParse($pair.Substring($eq + 1), [ref]$cnt)) { continue }
            $key = "$ver|$scope"
            if (-not $scopeStat.ContainsKey($key)) {
                $scopeStat[$key] = @{ version = $ver; scope = $scope; installs = [int64]0; sessions = [int64]0; frames = [int64]0; long = [int64]0 }
            }
            if (-not $seenScope.ContainsKey($scope)) { $scopeStat[$key].sessions += $n; $seenScope[$scope] = $true }
            $scopeStat[$key].frames += $cnt * $n
            if ($tier -ne '250ms') { $scopeStat[$key].long += $cnt * $n }
            $scopes[$scope] = $true
        }
    }
    foreach ($scope in @($scopes.Keys | Sort-Object)) {
        $per = Invoke-Sql "SELECT blob1 AS version, count(DISTINCT index1) AS installs FROM foxsdr_usage WHERE $window AND blob12 = '1' AND (startsWith(blob13, 'slow.$scope.') OR position(blob13, ',slow.$scope.') > 0) GROUP BY blob1"
        foreach ($p in @($per)) {
            $k = "$([string]$p.version)|$scope"
            if ($scopeStat.ContainsKey($k)) { $scopeStat[$k].installs = [int64]$p.installs }
        }
    }
    Write-Host ""
    if ($scopeStat.Count -eq 0) {
        Write-Host "  No slow frame was reported by any build in this window."
    } else {
        Write-Host "  Top scopes (the part of the frame that took most of the slow frame; at most 5 a version):"
        Write-Host ("  {0,-34} {1,-16} {2,8} {3,9} {4,8} {5,8}" -f "version", "scope", "installs", "sessions", "frames", "1 s+")
        $byVersion = $scopeStat.Values | Group-Object -Property { $_.version } | Sort-Object -Property Name
        foreach ($g in $byVersion) {
            $top = $g.Group | Sort-Object -Property @{ Expression = { $_.frames }; Descending = $true }, @{ Expression = { $_.scope } } | Select-Object -First 5
            foreach ($s in $top) {
                $label = [string]$s.version
                if (-not $label) { $label = "(not reported)" }
                Write-Host ("  {0,-34} {1,-16} {2,8} {3,9} {4,8} {5,8}" -f $label, $s.scope, $s.installs, $s.sessions, $s.frames, $s.long)
            }
        }
        Write-Host "  installs = distinct installs with a slow frame in the scope; sessions = records carrying it; frames = slow frames in it; 1 s+ = of a second or more."
        if (@($slowStrings).Count -ge 1000) {
            Write-Host "  (the 1000 most common count strings were read; the rarest are not shown)"
        }
    }
}

# WHAT THE PROGRAM RECOVERED FROM, by version (0.99.64, the second pass) - the places
# it met a fault and carried on without telling anybody: the sound output restarted,
# a radio's driver reopened, a settings file that could not be written (PRIVACY.md,
# "Recovered from"). The same discipline: rows with blob12 = '1' only, installs
# from count(DISTINCT index1), the sum double12 computed by the Worker from the
# validated tokens, the tokens read out of the count strings (blob13) and each
# token's installs asked for from the database.
Write-Host ""
Write-Host "Recovered from, by version" -ForegroundColor Cyan
$recVersions = Invoke-Sql "SELECT blob1 AS version, count(DISTINCT index1) AS installs, count() AS sessions, sum(double12) AS recoveries FROM foxsdr_usage WHERE $window AND blob12 = '1' GROUP BY blob1 ORDER BY installs DESC"
if (-not $recVersions) {
    Write-Host "  No build in this window reports recoveries yet."
    Write-Host "  That is not zero recoveries - it is unmeasured."
} else {
    $withRec = Invoke-Sql "SELECT blob1 AS version, count(DISTINCT index1) AS installs FROM foxsdr_usage WHERE $window AND blob12 = '1' AND double12 > 0 GROUP BY blob1"
    $withRecBy = @{}
    foreach ($s in @($withRec)) { $withRecBy[[string]$s.version] = [int64]$s.installs }
    Write-Host ("  {0,-34} {1,8} {2,9} {3,12} {4,11}" -f "version", "installs", "sessions", "w/ recovery", "recoveries")
    foreach ($r in $recVersions) {
        $label = [string]$r.version
        if (-not $label) { $label = "(not reported)" }
        $with = 0
        if ($withRecBy.ContainsKey([string]$r.version)) { $with = $withRecBy[[string]$r.version] }
        Write-Host ("  {0,-34} {1,8} {2,9} {3,12} {4,11}" -f $label, $r.installs, $r.sessions, $with, $r.recoveries)
    }
    Write-Host "  installs = distinct installs that reported; w/ recovery = those with at least one; recoveries = the sum of the counts (a word that"
    Write-Host "  can repeat by itself counts once a session). A reporting version with a zero is a real zero."

    $recStrings = Invoke-Sql "SELECT blob1 AS version, blob13 AS recoveredHealth, count() AS sessions FROM foxsdr_usage WHERE $window AND blob12 = '1' AND position(blob13, 'recovered.') > 0 GROUP BY blob1, blob13 ORDER BY sessions DESC LIMIT 1000"
    $recStat = @{}
    $recTokens = @{}
    foreach ($row in @($recStrings)) {
        $ver = [string]$row.version
        $n = [int64]$row.sessions
        foreach ($pair in ([string]$row.recoveredHealth).Split(',')) {
            $eq = $pair.IndexOf('=')
            if ($eq -lt 1) { continue }
            # Pasted into SQL below: only a word of the vocabulary.
            if ($pair.Substring(0, $eq) -notmatch '^recovered\.[a-z]+$') { continue }
            $tok = $pair.Substring(0, $eq)
            $cnt = [int64]0
            if (-not [int64]::TryParse($pair.Substring($eq + 1), [ref]$cnt)) { continue }
            $key = "$ver|$tok"
            if (-not $recStat.ContainsKey($key)) {
                $recStat[$key] = @{ version = $ver; token = $tok; installs = [int64]0; sessions = [int64]0; total = [int64]0 }
            }
            $recStat[$key].sessions += $n
            $recStat[$key].total += $cnt * $n
            $recTokens[$tok] = $true
        }
    }
    foreach ($tok in @($recTokens.Keys | Sort-Object)) {
        $per = Invoke-Sql "SELECT blob1 AS version, count(DISTINCT index1) AS installs FROM foxsdr_usage WHERE $window AND blob12 = '1' AND (startsWith(blob13, '$tok=') OR position(blob13, ',$tok=') > 0) GROUP BY blob1"
        foreach ($p in @($per)) {
            $k = "$([string]$p.version)|$tok"
            if ($recStat.ContainsKey($k)) { $recStat[$k].installs = [int64]$p.installs }
        }
    }
    Write-Host ""
    if ($recStat.Count -eq 0) {
        Write-Host "  No recovery was reported by any build in this window."
    } else {
        Write-Host "  Top recoveries (at most 5 a version; the word is the place - see PRIVACY.md):"
        Write-Host ("  {0,-34} {1,-24} {2,8} {3,9} {4,7}" -f "version", "recovered", "installs", "sessions", "count")
        $recByVersion = $recStat.Values | Group-Object -Property { $_.version } | Sort-Object -Property Name
        foreach ($g in $recByVersion) {
            $top = $g.Group | Sort-Object -Property @{ Expression = { $_.sessions }; Descending = $true }, @{ Expression = { $_.token } } | Select-Object -First 5
            foreach ($s in $top) {
                $label = [string]$s.version
                if (-not $label) { $label = "(not reported)" }
                Write-Host ("  {0,-34} {1,-24} {2,8} {3,9} {4,7}" -f $label, $s.token, $s.installs, $s.sessions, $s.total)
            }
        }
        Write-Host "  installs = distinct installs with the word; sessions = records carrying it; count = times it happened."
        if (@($recStrings).Count -ge 1000) {
            Write-Host "  (the 1000 most common count strings were read; the rarest are not shown)"
        }
    }
}
Write-Host ""
