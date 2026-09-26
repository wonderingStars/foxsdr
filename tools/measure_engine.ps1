<#
.SYNOPSIS
    The engine-extraction performance gate: the section-3 measures of
    docs/ENGINE-EXTRACTION.md (foxsdr-api repository), taken by script so the
    same numbers are produced before and after every stage.

.DESCRIPTION
    Every figure comes from the application itself, run as a bounded
    (--frames) session against the built-in SIGNAL GENERATOR - no radio is
    opened - with its own measurement switches:

      frames   FOXSDR_FRAME_LOG + FOXSDR_VSYNC_OFF + FOXSDR_MEASURE=run.
               Frame time = interval between consecutive frame STARTS (taken
               before event polling), first WarmupSeconds discarded; per run
               the mean and the nearest-rank 99th percentile.
      cpu      FOXSDR_MEASURE=run with vsync ON (as a user runs it): process
               CPU over the window after the warm-up, as % of one core, and
               the working set at the end. NOTE: on the development desktop
               the swap interval of 1 is not honoured (measured ~1300 frames
               a second either way), so this figure is dominated by the GUI
               thread rendering flat out, and it falls sharply whenever the
               window is covered or minimised. Leave the window alone.
      latency  FOXSDR_MEASURE=latency: tone at +100 kHz, the VFO moved off it
               and back on through the click-to-tune path, the audio sample
               clock from the command to the first 256-sample block in
               audioTap() whose tone power crosses half its steady value.
               Retunes per run (50); per run the median.
      rates    FOXSDR_MEASURE=rates: a ladder of signal-generator rates, one
               window each (60 s after a 5 s warm-up), stopping at the first
               with a ring drop (Pipeline::ringDroppedSamples). Per run the
               highest rate with zero drops.

    Runs are INTERLEAVED - baseline, candidate, baseline, candidate - measure
    by measure, Runs per build (5). A build's value is the median of its runs.
    The gate rules are the document's, exactly:
      * variance: if the BASELINE's runs spread by more than the gate (p99:
        max-min > 1 ms; the 5 % gates: max/min > 1.05; latency: > 16.7 ms)
        the gate is NOT JUDGED (NOISY);
      * a gate FAILS only when the candidate median is worse by more than the
        allowance AND every candidate run is worse than every baseline run;
      * worse beyond the allowance with overlapping runs is RE-MEASURE (10 a
        side); inside the allowance is PASS.

    The environment (OS build, GPU and driver, power plan, battery, CPU,
    screen, window size, other cascade.exe instances) is recorded before and
    after the session and with every summary; a comparison between records
    that differ is refused.

    SAFETY. Every launch gets APPDATA and LOCALAPPDATA pointed at a fresh
    scratch tree under -Out, every network endpoint the app knows
    (telemetry, crash, update, problem/feature reports) pointed at an
    unroutable black hole, and USERPROFILE left alone. Only the PID this
    script started is ever addressed; a run that overstays is sent WM_CLOSE
    and only killed if that fails.

.EXAMPLE
    # A/B, interleaved, 5 runs each, all measures:
    tools\measure_engine.ps1 -Baseline base\cascade.exe -Candidate cand\cascade.exe -Out scratch\ab

.EXAMPLE
    # Recompute the summary and verdicts of a finished session:
    tools\measure_engine.ps1 -Summarize scratch\ab

.EXAMPLE
    # Hold a new session's candidate against a saved baseline summary (the
    # environment records must match):
    tools\measure_engine.ps1 -CompareBaseline tools\measure_baseline-0.99.39.json -CompareCandidate scratch\new\summary.json

.EXAMPLE
    tools\measure_engine.ps1 -SelfTest
#>
[CmdletBinding(DefaultParameterSetName = 'Measure')]
param(
    [Parameter(ParameterSetName = 'Measure', Mandatory = $true)] [string]$Baseline,
    [Parameter(ParameterSetName = 'Measure')] [string]$Candidate,
    [Parameter(ParameterSetName = 'Measure', Mandatory = $true)] [string]$Out,
    [Parameter(ParameterSetName = 'Measure')] [int]$Runs = 5,
    [Parameter(ParameterSetName = 'Measure')] [string[]]$Measures = @('frames', 'cpu', 'latency', 'rates'),
    [Parameter(ParameterSetName = 'Measure')] [string]$WindowSize = '1600x1000',
    [Parameter(ParameterSetName = 'Measure')] [double]$Seconds = 65,
    [Parameter(ParameterSetName = 'Measure')] [double]$WarmupSeconds = 5,
    # Every rung must give the chain an exact integer channel (Pipeline::
    # setInputRateHz refuses anything else and the result says so): these are
    # all 204.8 kHz x an integer. On the development desktop (i9-14900K) the
    # ceiling measured between 16.4 and 24.6 MS/s, so the ladder is fine there
    # and starts one rung below.
    [Parameter(ParameterSetName = 'Measure')] [string]$RateLadder = '14336000,16384000,18432000,20480000,22528000,24576000,28672000',
    [Parameter(ParameterSetName = 'Measure')] [double]$RateWindow = 60,
    [Parameter(ParameterSetName = 'Measure')] [int]$Retunes = 50,
    # Decoder plugin DLLs to load in every launch (the "three decoders open"
    # of the frame-time measure). Copied into each staged build's plugins\.
    [Parameter(ParameterSetName = 'Measure')] [string[]]$Plugins = @(),
    [Parameter(ParameterSetName = 'Summarize', Mandatory = $true)] [string]$Summarize,
    [Parameter(ParameterSetName = 'CompareFiles', Mandatory = $true)] [string]$CompareBaseline,
    [Parameter(ParameterSetName = 'CompareFiles', Mandatory = $true)] [string]$CompareCandidate,
    [Parameter(ParameterSetName = 'SelfTest', Mandatory = $true)] [switch]$SelfTest
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2

# ---------------------------------------------------------------------------
# The arithmetic, compiled: 100k-line frame logs are too slow to parse in 5.1
# script, and one implementation is what the self-test pins.
# ---------------------------------------------------------------------------
if (-not ('FoxMeasure' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;

public static class FoxMeasure {
    // Frame intervals (ms) between consecutive frame starts whose start lies
    // at least warmupS after the FIRST frame's start; the work times (ms) of
    // the same frames beside them.
    public static double[] FrameIntervalsMs(string path, double warmupS, out double[] workMs,
                                            out long frames, out long overflow) {
        List<long> starts = new List<long>();
        List<long> works = new List<long>();
        overflow = 0;
        foreach (string line in File.ReadLines(path)) {
            if (line.Length == 0) continue;
            if (line[0] == '#') {
                int at = line.IndexOf("overflow=");
                if (at >= 0) overflow = long.Parse(line.Substring(at + 9).Trim(), CultureInfo.InvariantCulture);
                continue;
            }
            int sp = line.IndexOf(' ');
            starts.Add(long.Parse(line.Substring(0, sp), CultureInfo.InvariantCulture));
            works.Add(long.Parse(line.Substring(sp + 1), CultureInfo.InvariantCulture));
        }
        frames = starts.Count;
        List<double> iv = new List<double>();
        List<double> wk = new List<double>();
        if (starts.Count == 0) { workMs = new double[0]; return new double[0]; }
        long cut = starts[0] + (long)(warmupS * 1e9);
        for (int i = 1; i < starts.Count; ++i) {
            if (starts[i - 1] < cut) continue;
            iv.Add((starts[i] - starts[i - 1]) / 1e6);
            wk.Add(works[i - 1] / 1e6);
        }
        workMs = wk.ToArray();
        return iv.ToArray();
    }

    // Nearest-rank percentile: the smallest value with at least p% of the
    // sample at or below it. rank = ceil(p/100 * N), 1-based.
    public static double NearestRank(double[] v, double p) {
        if (v.Length == 0) return double.NaN;
        double[] s = (double[])v.Clone();
        Array.Sort(s);
        int rank = (int)Math.Ceiling(p / 100.0 * s.Length);
        if (rank < 1) rank = 1;
        if (rank > s.Length) rank = s.Length;
        return s[rank - 1];
    }

    public static double Median(double[] v) {
        if (v.Length == 0) return double.NaN;
        double[] s = (double[])v.Clone();
        Array.Sort(s);
        int n = s.Length;
        return (n % 2 == 1) ? s[n / 2] : 0.5 * (s[n / 2 - 1] + s[n / 2]);
    }

    public static double Mean(double[] v) {
        if (v.Length == 0) return double.NaN;
        double sum = 0; foreach (double x in v) sum += x;
        return sum / v.Length;
    }

    public static double Max(double[] v) { double m = double.NegativeInfinity; foreach (double x in v) if (x > m) m = x; return m; }
    public static double Min(double[] v) { double m = double.PositiveInfinity; foreach (double x in v) if (x < m) m = x; return m; }

    // --- polite close of a process WE started, by its pid only ---
    delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc cb, IntPtr l);
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] static extern bool PostMessage(IntPtr h, uint msg, IntPtr w, IntPtr l);
    public static int CloseWindowsOf(int pid) {
        int sent = 0;
        EnumWindows((h, l) => {
            uint owner; GetWindowThreadProcessId(h, out owner);
            if (owner == (uint)pid && IsWindowVisible(h)) { PostMessage(h, 0x0010, IntPtr.Zero, IntPtr.Zero); ++sent; }
            return true;
        }, IntPtr.Zero);
        return sent;
    }
}
'@
}

function Get-Median([double[]]$v) { return [FoxMeasure]::Median($v) }

# ---------------------------------------------------------------------------
# The environment record
# ---------------------------------------------------------------------------
function Get-EnvRecord([string]$windowSize) {
    $cv = Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion'
    $gpus = @(Get-CimInstance Win32_VideoController | ForEach-Object {
            "$($_.Name) driver $($_.DriverVersion) $($_.CurrentHorizontalResolution)x$($_.CurrentVerticalResolution)@$($_.CurrentRefreshRate)"
        }) -join '; '
    $scheme = (powercfg /getactivescheme) -join ' '
    $scheme = ($scheme -replace '^Power Scheme GUID:\s*', '').Trim()
    $bat = @(Get-CimInstance Win32_Battery -ErrorAction SilentlyContinue)
    $battery = 'none'
    if ($bat.Count -gt 0) { $battery = ($bat | ForEach-Object { "status $($_.BatteryStatus)" }) -join '; ' }
    $cpu = (Get-CimInstance Win32_Processor | Select-Object -First 1).Name.Trim()
    return [ordered]@{
        os          = "Windows $($cv.DisplayVersion) build $($cv.CurrentBuild).$($cv.UBR)"
        gpu         = $gpus
        powerPlan   = $scheme
        battery     = $battery
        cpu         = "$cpu, $([Environment]::ProcessorCount) logical"
        windowSize  = $windowSize
        machine     = $env:COMPUTERNAME
    }
}

function Test-SameEnv($a, $b) {
    $diffs = @()
    foreach ($k in @('os', 'gpu', 'powerPlan', 'battery', 'cpu', 'windowSize', 'machine')) {
        $va = $null; $vb = $null
        if ($a.PSObject.Properties[$k]) { $va = $a.$k } elseif ($a -is [System.Collections.IDictionary]) { $va = $a[$k] }
        if ($b.PSObject.Properties[$k]) { $vb = $b.$k } elseif ($b -is [System.Collections.IDictionary]) { $vb = $b[$k] }
        if ("$va" -ne "$vb") { $diffs += "$k : '$va' vs '$vb'" }
    }
    return , $diffs
}

# ---------------------------------------------------------------------------
# One launch
# ---------------------------------------------------------------------------
$BlackHole = 'http://127.0.0.1:9/'

function Invoke-Launch {
    param([string]$Exe, [string]$Dir, [string]$Measure)
    New-Item -ItemType Directory -Force -Path $Dir | Out-Null
    $scratch = Join-Path $Dir 'profile'
    New-Item -ItemType Directory -Force -Path (Join-Path $scratch 'appdata'), (Join-Path $scratch 'localappdata') | Out-Null

    $vars = [ordered]@{
        APPDATA                    = (Join-Path $scratch 'appdata')
        LOCALAPPDATA               = (Join-Path $scratch 'localappdata')
        FOXSDR_TELEMETRY_URL       = $BlackHole
        FOXSDR_CRASH_URL           = $BlackHole
        FOXSDR_UPDATE_URL          = $BlackHole
        FOXSDR_PROBLEM_URL         = $BlackHole
        FOXSDR_FEATURE_URL         = $BlackHole
        FOXSDR_REPORTS_URL         = $BlackHole
        FOXSDR_WINDOW_SIZE         = $WindowSize
        FOXSDR_OPEN_DEMOD_SCOPE    = '1'
        FOXSDR_MEASURE_OUT         = (Join-Path $Dir 'result.json')
        FOXSDR_MEASURE_WARMUP      = "$WarmupSeconds"
        FOXSDR_MEASURE_SECONDS     = "$Seconds"
        FOXSDR_MEASURE_RATE        = '2048000'
        FOXSDR_MEASURE_RATES       = $RateLadder
        FOXSDR_MEASURE_WINDOW      = "$RateWindow"
        FOXSDR_MEASURE_RETUNES     = "$Retunes"
        FOXSDR_MEASURE             = $null
        FOXSDR_FRAME_LOG           = $null
        FOXSDR_VSYNC_OFF           = $null
    }
    switch ($Measure) {
        'frames' { $vars.FOXSDR_MEASURE = 'run'; $vars.FOXSDR_FRAME_LOG = (Join-Path $Dir 'frames.log'); $vars.FOXSDR_VSYNC_OFF = '1' }
        'cpu' { $vars.FOXSDR_MEASURE = 'run' }
        'latency' { $vars.FOXSDR_MEASURE = 'latency' }
        'rates' { $vars.FOXSDR_MEASURE = 'rates' }
        default { throw "unknown measure $Measure" }
    }
    $timeout = 120 + $Seconds
    if ($Measure -eq 'latency') { $timeout = 120 + 2 * $Retunes }
    if ($Measure -eq 'rates') { $timeout = 120 + ($RateLadder.Split(',').Count) * ($RateWindow + $WarmupSeconds + 5) }

    $others = @(Get-Process -Name cascade -ErrorAction SilentlyContinue).Count
    $saved = @{}
    foreach ($k in $vars.Keys) { $saved[$k] = [Environment]::GetEnvironmentVariable($k, 'Process') }
    try {
        foreach ($k in $vars.Keys) { [Environment]::SetEnvironmentVariable($k, $vars[$k], 'Process') }
        $p = Start-Process -FilePath $Exe -ArgumentList '--frames', '1000000' -PassThru `
            -WorkingDirectory (Split-Path -Parent $Exe) `
            -RedirectStandardOutput (Join-Path $Dir 'stdout.txt') -RedirectStandardError (Join-Path $Dir 'stderr.txt')
    } finally {
        foreach ($k in $saved.Keys) { [Environment]::SetEnvironmentVariable($k, $saved[$k], 'Process') }
    }
    $myPid = $p.Id
    $null = $p.Handle  # held now, so ExitCode is readable after the exit
    $how = 'exited'
    if (-not $p.WaitForExit([int]($timeout * 1000))) {
        $how = 'wm_close'
        [void][FoxMeasure]::CloseWindowsOf($myPid)
        if (-not $p.WaitForExit(30000)) {
            $how = 'killed'
            Stop-Process -Id $myPid -Force
            $p.WaitForExit(10000) | Out-Null
        }
    }
    $info = [ordered]@{ measure = $Measure; exe = $Exe; pid = $myPid; ended = $how; exitCode = $p.ExitCode; otherCascadeAtStart = $others }
    $info | ConvertTo-Json | Set-Content -Encoding ascii (Join-Path $Dir 'launch.json')
    if ($how -ne 'exited') { Write-Warning "$Measure run in $Dir did not finish on its own ($how)" }
    return $info
}

# ---------------------------------------------------------------------------
# One run's figures, read back from its directory
# ---------------------------------------------------------------------------
function Read-RunFigures([string]$Dir, [string]$Measure) {
    $res = Join-Path $Dir 'result.json'
    if (-not (Test-Path $res)) { return $null }
    $j = Get-Content -Raw $res | ConvertFrom-Json
    if ($j.error) { Write-Warning "$Dir : $($j.error)"; return $null }
    switch ($Measure) {
        'frames' {
            $w = $null; $n = 0L; $ov = 0L
            $iv = [FoxMeasure]::FrameIntervalsMs((Join-Path $Dir 'frames.log'), $WarmupSecondsForRead, [ref]$w, [ref]$n, [ref]$ov)
            if ($ov -gt 0) { Write-Warning "$Dir : frame log overflowed ($ov frames not kept)" }
            return [ordered]@{
                frameMeanMs  = [FoxMeasure]::Mean($iv)
                frameP99Ms   = [FoxMeasure]::NearestRank($iv, 99)
                frameMaxMs   = [FoxMeasure]::Max($iv)
                workMeanMs   = [FoxMeasure]::Mean($w)
                workP99Ms    = [FoxMeasure]::NearestRank($w, 99)
                intervals    = $iv.Length
                ringDropped  = [double]$j.ringDropped
            }
        }
        'cpu' {
            return [ordered]@{
                cpuPct       = 100.0 * [double]$j.cpuS / [double]$j.windowS
                workingSetMB = [double]$j.workingSetBytes / 1MB
                ringDropped  = [double]$j.ringDropped
            }
        }
        'latency' {
            $ok = @($j.retunes | Where-Object { $_.latencyMs -ge 0 -and -not $_.missed } | ForEach-Object { [double]$_.latencyMs })
            $away = @($j.retunes | ForEach-Object { [double]$_.awayRatio })
            $awayMax = 0.0
            if ($away.Count -gt 0) { $awayMax = [FoxMeasure]::Max([double[]]$away) }
            if ($awayMax -ge 0.25) { Write-Warning "$Dir : off-tone level reached $awayMax of steady - the detector is not clean" }
            return [ordered]@{
                latencyMedianMs = [FoxMeasure]::Median([double[]]$ok)
                latencyP90Ms    = [FoxMeasure]::NearestRank([double[]]$ok, 90)
                retunesValid    = $ok.Count
                retunesTotal    = @($j.retunes).Count
                awayRatioMax    = $awayMax
                toneAudioHz     = [double]$j.toneAudioHz
            }
        }
        'rates' {
            $best = 0.0; $failedAt = $null
            foreach ($s in $j.steps) {
                if ([double]$s.dropped -eq 0) { $best = [double]$s.requestedHz } else { $failedAt = [double]$s.requestedHz; break }
            }
            return [ordered]@{
                maxRateHz      = $best
                firstDropHz    = $failedAt
                ceilingReached = ($null -ne $failedAt)
            }
        }
    }
}

# ---------------------------------------------------------------------------
# The gates
# ---------------------------------------------------------------------------
# kind: 'abs' (allowance in the figure's own unit), 'rel' (fractional),
# 'nolower' (allowance 0, higher is better).
$Gates = @(
    @{ name = 'frame p99 (ms)'; measure = 'frames'; field = 'frameP99Ms'; kind = 'abs'; allowance = 1.0 },
    @{ name = 'frame mean (ms)'; measure = 'frames'; field = 'frameMeanMs'; kind = 'rel'; allowance = 0.05 },
    @{ name = 'CPU (% of a core)'; measure = 'cpu'; field = 'cpuPct'; kind = 'rel'; allowance = 0.05 },
    @{ name = 'working set (MB)'; measure = 'cpu'; field = 'workingSetMB'; kind = 'rel'; allowance = 0.05 },
    @{ name = 'tune-to-audio median (ms)'; measure = 'latency'; field = 'latencyMedianMs'; kind = 'abs'; allowance = 16.7 },
    @{ name = 'max sustained rate (Hz)'; measure = 'rates'; field = 'maxRateHz'; kind = 'nolower'; allowance = 0.0 }
)

function Get-Verdict($gate, [double[]]$base, [double[]]$cand) {
    if ($base.Count -eq 0 -or $cand.Count -eq 0) { return 'NO DATA' }
    $bMed = Get-Median $base; $cMed = Get-Median $cand
    $bMin = [FoxMeasure]::Min($base); $bMax = [FoxMeasure]::Max($base)
    $cMin = [FoxMeasure]::Min($cand); $cMax = [FoxMeasure]::Max($cand)
    switch ($gate.kind) {
        'abs' {
            if (($bMax - $bMin) -gt $gate.allowance) { return 'NOISY (baseline spread exceeds the gate) - not judged' }
            $worse = $cMed - $bMed
            if ($worse -le $gate.allowance) { return 'PASS' }
            if ($cMin -gt $bMax) { return 'FAIL' }
            return 'RE-MEASURE (10 a side)'
        }
        'rel' {
            if ($bMin -gt 0 -and ($bMax / $bMin) -gt (1.0 + $gate.allowance)) { return 'NOISY (baseline spread exceeds the gate) - not judged' }
            if ($cMed -le $bMed * (1.0 + $gate.allowance)) { return 'PASS' }
            if ($cMin -gt $bMax) { return 'FAIL' }
            return 'RE-MEASURE (10 a side)'
        }
        'nolower' {
            if ($cMed -ge $bMed) { return 'PASS' }
            if ($cMax -lt $bMin) { return 'FAIL' }
            return 'RE-MEASURE (10 a side)'
        }
    }
}

function Get-Summary([string]$Root) {
    $meta = Get-Content -Raw (Join-Path $Root 'session.json') | ConvertFrom-Json
    $script:WarmupSecondsForRead = [double]$meta.warmupSeconds
    $builds = [ordered]@{}
    foreach ($label in @($meta.builds.PSObject.Properties.Name)) {
        $vals = [ordered]@{}
        foreach ($m in $meta.measures) {
            for ($r = 1; $r -le [int]$meta.runs; ++$r) {
                $dir = Join-Path $Root "runs\$label-$m-r$r"
                if (-not (Test-Path $dir)) { continue }
                $fig = Read-RunFigures $dir $m
                if ($null -eq $fig) { continue }
                foreach ($k in $fig.Keys) {
                    if (-not $vals.Contains($k)) { $vals[$k] = New-Object System.Collections.Generic.List[object] }
                    $vals[$k].Add($fig[$k])
                }
            }
        }
        $med = [ordered]@{}
        foreach ($k in $vals.Keys) {
            $nums = @($vals[$k] | Where-Object { $_ -is [double] -or $_ -is [int] -or $_ -is [long] } | ForEach-Object { [double]$_ })
            if ($nums.Count -gt 0) { $med[$k] = Get-Median ([double[]]$nums) }
        }
        $builds[$label] = [ordered]@{ exe = $meta.builds.$label.exe; sha256 = $meta.builds.$label.sha256; runs = $vals; median = $med }
    }
    $gates = @()
    if ($builds.Contains('baseline') -and $builds.Contains('candidate')) {
        foreach ($g in $Gates) {
            $b = @(); $c = @()
            if ($builds.baseline.runs.Contains($g.field)) { $b = @($builds.baseline.runs[$g.field] | ForEach-Object { [double]$_ }) }
            if ($builds.candidate.runs.Contains($g.field)) { $c = @($builds.candidate.runs[$g.field] | ForEach-Object { [double]$_ }) }
            $v = Get-Verdict $g ([double[]]$b) ([double[]]$c)
            if (-not $meta.envStable) { $v = 'REFUSED (environment changed during the session)' }
            $row = [ordered]@{ gate = $g.name; verdict = $v }
            if ($b.Count -gt 0) { $row.baselineMedian = Get-Median ([double[]]$b); $row.baselineRange = @([FoxMeasure]::Min([double[]]$b), [FoxMeasure]::Max([double[]]$b)); $row.baselineRuns = $b }
            if ($c.Count -gt 0) { $row.candidateMedian = Get-Median ([double[]]$c); $row.candidateRange = @([FoxMeasure]::Min([double[]]$c), [FoxMeasure]::Max([double[]]$c)); $row.candidateRuns = $c }
            $gates += $row
        }
    }
    return [ordered]@{
        format      = 'foxsdr-measure-summary/1'
        environment = $meta.environment
        envStable   = $meta.envStable
        runs        = $meta.runs
        warmupSeconds = $meta.warmupSeconds
        measures    = $meta.measures
        rateLadder  = $meta.rateLadder
        builds      = $builds
        gates       = $gates
    }
}

function Write-GateTable($summary) {
    foreach ($label in $summary.builds.Keys) {
        Write-Host ("{0}: {1}" -f $label, $summary.builds[$label].exe)
        foreach ($k in $summary.builds[$label].median.Keys) {
            $runs = ($summary.builds[$label].runs[$k] | ForEach-Object { '{0:G5}' -f $_ }) -join ', '
            Write-Host ("    {0,-18} median {1,12:G6}   runs [{2}]" -f $k, $summary.builds[$label].median[$k], $runs)
        }
    }
    if ($summary.gates.Count -gt 0) {
        Write-Host ''
        Write-Host ('{0,-28} {1,12} {2,-24} {3,12} {4,-24} {5}' -f 'gate', 'base med', 'base range', 'cand med', 'cand range', 'verdict')
        foreach ($g in $summary.gates) {
            $br = ''; $cr = ''; $bm = ''; $cm = ''
            if ($g.Contains('baselineMedian')) { $bm = '{0:G6}' -f $g.baselineMedian; $br = '{0:G6} .. {1:G6}' -f $g.baselineRange[0], $g.baselineRange[1] }
            if ($g.Contains('candidateMedian')) { $cm = '{0:G6}' -f $g.candidateMedian; $cr = '{0:G6} .. {1:G6}' -f $g.candidateRange[0], $g.candidateRange[1] }
            Write-Host ('{0,-28} {1,12} {2,-24} {3,12} {4,-24} {5}' -f $g.gate, $bm, $br, $cm, $cr, $g.verdict)
        }
    }
}

# ---------------------------------------------------------------------------
# Modes
# ---------------------------------------------------------------------------
if ($SelfTest) {
    $fail = 0
    function Check($cond, $what) { if (-not $cond) { Write-Host "FAIL $what"; $script:fail++ } }
    # Nearest rank: of 1..100, the 99th percentile is 99; of 1..10 it is 10
    # (ceil(9.9) = 10); of a single value it is that value.
    Check ([FoxMeasure]::NearestRank([double[]](1..100), 99) -eq 99) 'p99 of 1..100'
    Check ([FoxMeasure]::NearestRank([double[]](1..10), 99) -eq 10) 'p99 of 1..10'
    Check ([FoxMeasure]::NearestRank([double[]]@(7), 99) -eq 7) 'p99 of one value'
    Check ([FoxMeasure]::NearestRank([double[]]@(5, 1, 4, 2, 3), 50) -eq 3) 'p50 unsorted'
    Check ([FoxMeasure]::Median([double[]]@(3, 1, 2)) -eq 2) 'median odd'
    Check ([FoxMeasure]::Median([double[]]@(4, 1, 2, 3)) -eq 2.5) 'median even'
    # The frame log: warm-up cut against the FIRST start, intervals between
    # consecutive starts, work beside them.
    $tmp = [IO.Path]::GetTempFileName()
    $lines = @('# foxsdr-frame-log/1 frames=5 overflow=0', '# start_ns work_ns')
    $lines += '1000000000 1000000'      # t=1.0 s
    $lines += '3000000000 1000000'      # t=3.0
    $lines += '6000000000 2000000'      # t=6.0  (>= 1.0 + 5 s warm-up)
    $lines += '6010000000 3000000'      # +10 ms
    $lines += '6030000000 4000000'      # +20 ms
    [IO.File]::WriteAllLines($tmp, $lines)
    $w = $null; $n = 0L; $ov = 0L
    $iv = [FoxMeasure]::FrameIntervalsMs($tmp, 5.0, [ref]$w, [ref]$n, [ref]$ov)
    Remove-Item $tmp
    Check ($n -eq 5) 'frames counted'
    Check ($iv.Length -eq 2) "intervals after warm-up ($($iv.Length))"
    Check ($iv[0] -eq 10 -and $iv[1] -eq 20) 'interval values'
    Check ($w[0] -eq 2 -and $w[1] -eq 3) 'work values'
    # The verdict rules.
    $p99 = $Gates[0]; $mean = $Gates[1]; $rate = $Gates[5]
    Check ((Get-Verdict $p99 @(5, 5.2, 5.1, 5.3, 5.0) @(5.5, 5.6, 5.4, 5.9, 5.7)) -eq 'PASS') 'p99 inside 1 ms passes'
    Check ((Get-Verdict $p99 @(5, 5.2, 5.1, 5.3, 5.0) @(7, 7.1, 7.2, 7.3, 7.4)) -eq 'FAIL') 'p99 2 ms worse, no overlap fails'
    Check ((Get-Verdict $p99 @(5, 5.2, 5.1, 5.3, 5.0) @(7, 7.1, 7.2, 7.3, 5.2)) -eq 'RE-MEASURE (10 a side)') 'p99 worse but overlapping re-measures'
    Check ((Get-Verdict $p99 @(5, 6.5, 5.1, 5.3, 5.0) @(5, 5, 5, 5, 5)) -like 'NOISY*') 'p99 noisy baseline not judged'
    Check ((Get-Verdict $mean @(10, 10.1, 10.2, 10.1, 10) @(10.4, 10.5, 10.3, 10.6, 10.2)) -eq 'PASS') 'mean inside 5 % passes'
    Check ((Get-Verdict $mean @(10, 10.1, 10.2, 10.1, 10) @(11, 11.1, 11.2, 11.3, 11.4)) -eq 'FAIL') 'mean 10 % worse, no overlap fails'
    Check ((Get-Verdict $rate @(16e6, 16e6, 16e6, 16e6, 16e6) @(16e6, 16e6, 16e6, 16e6, 16e6)) -eq 'PASS') 'rate equal passes'
    Check ((Get-Verdict $rate @(16e6, 16e6, 16e6, 16e6, 16e6) @(14e6, 14e6, 14e6, 14e6, 14e6)) -eq 'FAIL') 'rate lower everywhere fails'
    Check ((Get-Verdict $rate @(16e6, 16e6, 14e6, 16e6, 16e6) @(14e6, 14e6, 16e6, 14e6, 14e6)) -eq 'RE-MEASURE (10 a side)') 'rate lower but overlapping re-measures'
    $e1 = [pscustomobject]@{ os = 'a'; gpu = 'g'; powerPlan = 'p'; battery = 'none'; cpu = 'c'; windowSize = 'w'; machine = 'm' }
    $e2 = [pscustomobject]@{ os = 'a'; gpu = 'g2'; powerPlan = 'p'; battery = 'none'; cpu = 'c'; windowSize = 'w'; machine = 'm' }
    Check ((Test-SameEnv $e1 $e1).Count -eq 0) 'same environment accepted'
    Check ((Test-SameEnv $e1 $e2).Count -eq 1) 'different GPU refused'
    if ($fail -eq 0) { Write-Host 'measure_engine self-test: PASS' ; exit 0 } else { Write-Host "measure_engine self-test: $fail failed"; exit 1 }
}

if ($PSCmdlet.ParameterSetName -eq 'Summarize') {
    $s = Get-Summary $Summarize
    $s | ConvertTo-Json -Depth 8 | Set-Content -Encoding ascii (Join-Path $Summarize 'summary.json')
    Write-GateTable $s
    exit 0
}

if ($PSCmdlet.ParameterSetName -eq 'CompareFiles') {
    $a = Get-Content -Raw $CompareBaseline | ConvertFrom-Json
    $b = Get-Content -Raw $CompareCandidate | ConvertFrom-Json
    $diffs = Test-SameEnv $a.environment $b.environment
    if ($diffs.Count -gt 0) {
        Write-Host 'REFUSED: the two sessions were measured in different environments:'
        $diffs | ForEach-Object { Write-Host "    $_" }
        exit 2
    }
    $ba = $a.builds.PSObject.Properties | Where-Object { $_.Name -eq 'baseline' } | Select-Object -First 1
    $cb = $b.builds.PSObject.Properties | Where-Object { $_.Name -eq 'candidate' } | Select-Object -First 1
    if ($null -eq $cb) { $cb = $b.builds.PSObject.Properties | Where-Object { $_.Name -eq 'baseline' } | Select-Object -First 1 }
    Write-Host 'NOTE: runs from two sessions are not interleaved; section 3.1 prefers one interleaved session.'
    foreach ($g in $Gates) {
        $bv = @(); $cv = @()
        if ($ba.Value.runs.PSObject.Properties[$g.field]) { $bv = @($ba.Value.runs.($g.field) | ForEach-Object { [double]$_ }) }
        if ($cb.Value.runs.PSObject.Properties[$g.field]) { $cv = @($cb.Value.runs.($g.field) | ForEach-Object { [double]$_ }) }
        Write-Host ('{0,-28} {1}' -f $g.name, (Get-Verdict $g ([double[]]$bv) ([double[]]$cv)))
    }
    exit 0
}

# --- Measure ---------------------------------------------------------------
# Each build is STAGED first: its executable, SoapySDR.dll and resources are
# copied under -Out, so a rebuild during the session cannot change what is
# being measured, and the plugin directory the app chooses (beside the exe) is
# the staged one rather than the build tree's.
function Copy-Stage([string]$exe, [string]$dest) {
    New-Item -ItemType Directory -Force -Path $dest | Out-Null
    $src = Split-Path -Parent $exe
    Copy-Item -Force $exe (Join-Path $dest 'cascade.exe')
    foreach ($dll in @(Get-ChildItem -Path $src -Filter *.dll)) { Copy-Item -Force $dll.FullName $dest }
    if (Test-Path (Join-Path $src 'resources')) { Copy-Item -Recurse -Force (Join-Path $src 'resources') $dest }
    $pd = Join-Path $dest 'plugins'
    New-Item -ItemType Directory -Force -Path $pd | Out-Null
    foreach ($pl in $Plugins) { Copy-Item -Force $pl $pd }
    return (Join-Path $dest 'cascade.exe')
}
New-Item -ItemType Directory -Force -Path $Out | Out-Null
$builds = [ordered]@{ baseline = (Copy-Stage (Resolve-Path $Baseline).Path (Join-Path $Out 'stage\baseline')) }
if ($Candidate) { $builds.candidate = (Copy-Stage (Resolve-Path $Candidate).Path (Join-Path $Out 'stage\candidate')) }
$envStart = Get-EnvRecord $WindowSize
$meta = [ordered]@{
    format        = 'foxsdr-measure-session/1'
    started       = (Get-Date).ToString('o')
    environment   = $envStart
    envStable     = $true
    runs          = $Runs
    warmupSeconds = $WarmupSeconds
    measures      = $Measures
    rateLadder    = $RateLadder
    plugins       = @($Plugins | ForEach-Object { Split-Path -Leaf $_ })
    builds        = [ordered]@{}
}
foreach ($k in $builds.Keys) {
    $src = $Baseline
    if ($k -eq 'candidate') { $src = $Candidate }
    $meta.builds[$k] = [ordered]@{ exe = $builds[$k]; source = (Resolve-Path $src).Path; sha256 = (Get-FileHash -Algorithm SHA256 $builds[$k]).Hash.ToLower() }
}
$meta | ConvertTo-Json -Depth 6 | Set-Content -Encoding ascii (Join-Path $Out 'session.json')

foreach ($m in $Measures) {
    for ($r = 1; $r -le $Runs; ++$r) {
        foreach ($label in $builds.Keys) {
            $dir = Join-Path $Out "runs\$label-$m-r$r"
            Write-Host ("[{0}] {1} {2} run {3}/{4}" -f (Get-Date).ToString('HH:mm:ss'), $label, $m, $r, $Runs)
            [void](Invoke-Launch -Exe $builds[$label] -Dir $dir -Measure $m)
        }
    }
}

$envEnd = Get-EnvRecord $WindowSize
$d = Test-SameEnv ([pscustomobject]$envStart) ([pscustomobject]$envEnd)
if ($d.Count -gt 0) {
    $meta.envStable = $false
    $meta.environmentAtEnd = $envEnd
    Write-Warning ('environment changed during the session: ' + ($d -join '; '))
}
$meta.finished = (Get-Date).ToString('o')
$meta | ConvertTo-Json -Depth 6 | Set-Content -Encoding ascii (Join-Path $Out 'session.json')

$s = Get-Summary $Out
$s | ConvertTo-Json -Depth 8 | Set-Content -Encoding ascii (Join-Path $Out 'summary.json')
Write-GateTable $s
