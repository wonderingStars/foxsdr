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
      cpu      FOXSDR_MEASURE=run at a display's frame rate, interface idle:
               process CPU over the window after the warm-up, as % of one
               core, and the working set at the end. The rate is imposed by
               the clock (FOXSDR_FRAME_CAP_HZ, -FrameCapHz 60) because on the
               development desktop the swap interval of 1 is not honoured
               (~1300 frames a second either way). latency, rates, cpubusy
               and soak run at the same cap.
      cpubusy  the cpu run with the interface BUSY: the VFO slider is dragged
               back and forth for the whole run through the app's own
               scripted pointer (FOXSDR_INPUT_SCRIPT, gui/input_script.hpp) -
               events fed into ImGui's queue inside the process, never the
               operating system's cursor. Run single-viewport
               (FOXSDR_SINGLE_VIEWPORT=1) so the script's coordinates are the
               window's own. A run whose drag did not move the VFO on at least
               90 % of its frames is refused.
      latency  FOXSDR_MEASURE=latency: tone at +100 kHz, the VFO moved off it
               and back on through the click-to-tune path, the audio sample
               clock from the command to the first 256-sample block in
               audioTap() whose tone power crosses half its steady value.
               Retunes per run (50); per run the median.
      rates    FOXSDR_MEASURE=rates: a ladder of signal-generator rates, one
               window each (60 s after a 5 s warm-up), stopping at the first
               with a ring drop (Pipeline::ringDroppedSamples). Per run the
               highest rate with zero drops.
      soak     the 10-minute soak, SoakRuns (2) a build: a cpu-style run of
               SoakSeconds (600) after the warm-up, with the frame log and the
               hang watchdog writing into the run's own directory
               (FOXSDR_DIAG_DIR). Longest frame, frames over 100 ms, hang
               reports written, and the working set after 10 minutes.

    EVERY RUN IS VALIDATED before a figure of it is used, from what the
    application itself reports. A run is INVALID if: it did not exit on its
    own or exited non-zero; it wrote no result; its result says error or
    faulted; its input rate is not the rate asked for (any rung, for rates);
    the ring dropped samples where the measure's premise is zero drops
    (frames, cpu, cpubusy, soak, latency); fewer decoder instances were fed
    than decoder plugins were staged, or they were handed no samples
    (frames, cpu, cpubusy, soak); a retune's off-tone level reached 0.25 of
    steady, or fewer than MinValidRetunes of the Retunes produced a figure,
    or fewer retunes were attempted; the frame log is missing or overflowed;
    the busy run's drag did not land; the soak's watchdog had nowhere to
    write; the executable launched was not byte-identical (sha256) to the
    build staged, or reported a different commit; or another cascade.exe or
    ctest.exe was running at the start or during the run. A gate needs ALL of
    both builds' runs of its measure valid; otherwise its verdict is INVALID.

    Runs are INTERLEAVED - baseline, candidate, baseline, candidate - measure
    by measure. A build's value is the median of its runs. The gate rules are
    the document's:
      * variance: if the BASELINE's runs spread by more than the gate (p99:
        max-min > 1 ms; the 5 % gates: max/min > 1.05; latency: > 16.7 ms)
        the gate is NOT JUDGED (NOISY);
      * a gate FAILS only when the candidate median is worse by more than the
        allowance AND every candidate run is worse than every baseline run;
      * worse beyond the allowance with overlapping runs is RE-MEASURE (10 a
        side); inside the allowance is PASS;
      * hang reports: none new (any candidate report beyond the baseline's
        count FAILS); frames over 100 ms in the soak: no higher.

    EXIT CODES (every mode but -SelfTest, which is 0 pass / 1 fail):
      0  every gate PASSED (a session with no candidate: every run valid)
      1  at least one gate FAILED
      2  at least one gate INVALID or NO DATA, none FAILED (runs refused, or
         a measure missing)
      3  REFUSED: the comparison cannot be made at all (the environment
         changed, the environments differ, another cascade.exe/ctest.exe ran
         during the measurements, a summary of an older format, or a staged
         build that is not the commit it was named as)
      4  UNDECIDED: some gate is NOISY (not judged) or RE-MEASURE, nothing
         worse. -AcceptNoisy lets a NOISY gate count as not failing (the
         caller's explicit choice, printed); RE-MEASURE is never accepted.
    Precedence: 3, then 1, then 2, then 4.

    The environment (OS build, GPU and driver, power plan, battery, CPU,
    screen, window size) is recorded before and after the session and with
    every summary; a comparison between records that differ is refused.

    SAFETY. Every launch gets APPDATA and LOCALAPPDATA pointed at a fresh
    scratch tree under -Out, every network endpoint the app knows
    (telemetry, crash, update, problem/feature reports) pointed at an
    unroutable black hole, every other FOXSDR_*/CASCADE_* variable of this
    shell removed for the child, and USERPROFILE left alone. Only the PID this
    script started is ever addressed; a run that overstays is sent WM_CLOSE
    and only killed if that fails. A launch waits while any other cascade.exe
    or ctest.exe is running (up to -WaitForOthersMinutes).

    GAPS. What section 3 asks for that this script does not measure is
    listed in every session and summary ("gaps") and printed with the table.

.EXAMPLE
    # A/B, interleaved, 5 runs each (soak 2), all measures:
    tools\measure_engine.ps1 -Baseline base\cascade.exe -BaselineCommit 54d5745 `
        -Candidate cand\cascade.exe -Out scratch\ab -Plugins p\cw.dll,p\eas.dll,p\aprs.dll

.EXAMPLE
    # Recompute the summary and verdicts of a finished session:
    tools\measure_engine.ps1 -Summarize scratch\ab

.EXAMPLE
    # Hold a new session's candidate against a saved baseline summary:
    tools\measure_engine.ps1 -CompareBaseline tools\measure_baseline-0.99.39.json -CompareCandidate scratch\new\summary.json

.EXAMPLE
    tools\measure_engine.ps1 -SelfTest
#>
[CmdletBinding(DefaultParameterSetName = 'Measure')]
param(
    [Parameter(ParameterSetName = 'Measure', Mandatory = $true)] [string]$Baseline,
    [Parameter(ParameterSetName = 'Measure')] [string]$Candidate,
    [Parameter(ParameterSetName = 'Measure', Mandatory = $true)] [string]$Out,
    # The commit each build must report (a prefix is enough). Checked against
    # what the staged executable itself reports before anything is measured.
    [Parameter(ParameterSetName = 'Measure')] [string]$BaselineCommit = '',
    [Parameter(ParameterSetName = 'Measure')] [string]$CandidateCommit = '',
    [Parameter(ParameterSetName = 'Measure')] [int]$Runs = 5,
    [Parameter(ParameterSetName = 'Measure')] [string[]]$Measures = @('frames', 'cpu', 'cpubusy', 'latency', 'rates', 'soak'),
    [Parameter(ParameterSetName = 'Measure')] [string]$WindowSize = '1600x1000',
    [Parameter(ParameterSetName = 'Measure')] [double]$Seconds = 65,
    [Parameter(ParameterSetName = 'Measure')] [double]$WarmupSeconds = 5,
    # Every rung must give the chain an exact integer channel (Pipeline::
    # setInputRateHz refuses anything else and the result says so): these are
    # all 204.8 kHz x an integer.
    [Parameter(ParameterSetName = 'Measure')] [string]$RateLadder = '14336000,16384000,18432000,20480000,22528000,24576000,28672000',
    [Parameter(ParameterSetName = 'Measure')] [double]$RateWindow = 60,
    [Parameter(ParameterSetName = 'Measure')] [int]$Retunes = 50,
    # A run needs this many of its Retunes to produce a figure. 45 of 50:
    # every run of the 0.99.39 baseline produced 50 of 50, and a retune only
    # fails to produce one when the reader fell more than the 85 ms audio tap
    # behind (a frame that long) or the tone never came back within 1.5 s -
    # a broken tune path fails nearly all of them, as the review's
    # fabricated "1 of 50" did. 10 % absorbs a stray long frame without
    # letting a median be taken over a handful of survivors.
    [Parameter(ParameterSetName = 'Measure')] [int]$MinValidRetunes = 45,
    [Parameter(ParameterSetName = 'Measure')] [int]$FrameCapHz = 60,
    [Parameter(ParameterSetName = 'Measure')] [int]$SoakRuns = 2,
    [Parameter(ParameterSetName = 'Measure')] [double]$SoakSeconds = 600,
    # Decoder plugin DLLs to load in every launch (the "three decoders open"
    # of the frame-time measure). Copied into each staged build's plugins\.
    [Parameter(ParameterSetName = 'Measure')] [string[]]$Plugins = @(),
    # The VFO slider the busy run drags, in the window's own coordinates at
    # the default 1600x1000 window: the grab, the row, and the two ends of
    # the stroke (found from a self-capture of 0.99.39, FOXSDR_SHOT_AT_FRAME).
    [Parameter(ParameterSetName = 'Measure')] [double]$BusyGrabX = 219,
    [Parameter(ParameterSetName = 'Measure')] [double]$BusyY = 590,
    [Parameter(ParameterSetName = 'Measure')] [double]$BusyMinX = 45,
    [Parameter(ParameterSetName = 'Measure')] [double]$BusyMaxX = 255,
    [Parameter(ParameterSetName = 'Measure')] [int]$WaitForOthersMinutes = 180,
    [Parameter(ParameterSetName = 'Summarize', Mandatory = $true)] [string]$Summarize,
    [Parameter(ParameterSetName = 'CompareFiles', Mandatory = $true)] [string]$CompareBaseline,
    [Parameter(ParameterSetName = 'CompareFiles', Mandatory = $true)] [string]$CompareCandidate,
    [Parameter(ParameterSetName = 'Measure')]
    [Parameter(ParameterSetName = 'Summarize')]
    [Parameter(ParameterSetName = 'CompareFiles')] [switch]$AcceptNoisy,
    [Parameter(ParameterSetName = 'SelfTest', Mandatory = $true)] [switch]$SelfTest
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2

# ---------------------------------------------------------------------------
# The arithmetic, compiled: 100k-line frame logs are too slow to parse in 5.1
# script, and one implementation is what the self-test pins.
# ---------------------------------------------------------------------------
if (-not ('FoxMeasure2' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;

public static class FoxMeasure2 {
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
    public static double Sum(double[] v) { double s = 0; foreach (double x in v) s += x; return s; }
    public static int CountOver(double[] v, double thr) { int n = 0; foreach (double x in v) if (x > thr) ++n; return n; }

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

function Get-Median([double[]]$v) { return [FoxMeasure2]::Median($v) }

# A property of a parsed JSON object or a dictionary, or a default when it is
# absent (StrictMode makes a missing property an exception, and a missing
# field is exactly what the validation has to be able to say).
function Get-P($obj, [string]$name, $default = $null) {
    if ($null -eq $obj) { return $default }
    if ($obj -is [System.Collections.IDictionary]) {
        if ($obj.Contains($name)) { return $obj[$name] }
        return $default
    }
    $prop = $obj.PSObject.Properties[$name]
    if ($null -eq $prop) { return $default }
    return $prop.Value
}

function Write-Json($obj, [string]$path) {
    $text = $obj | ConvertTo-Json -Depth 12
    [IO.File]::WriteAllText($path, $text, (New-Object System.Text.UTF8Encoding($false)))
}

function Read-Json([string]$path) {
    return ([IO.File]::ReadAllText($path) | ConvertFrom-Json)
}

# ---------------------------------------------------------------------------
# What section 3 asks for that this script does not measure. Printed with
# every table and kept in every session and summary, so a PASS is never read
# as covering more than it does.
# ---------------------------------------------------------------------------
function Get-Gaps([string[]]$measureList, [int]$pluginCount) {
    $g = New-Object System.Collections.Generic.List[string]
    $g.Add('raspberry-pi: section 3 records every measure on the Raspberry Pi as well; this script runs only on the Windows desktop and no Pi figures were taken.')
    $g.Add('real-radio: every figure is taken off the built-in signal generator, not the B200 or RTL-SDR section 3 names; the USB transport and the vendor drivers'' threads are outside every figure.')
    $g.Add('release: section 3 names 0.99.35 as the release to record; the baseline measured is 0.99.39 (master 1de7221 plus the measurement switches).')
    $g.Add('decoders-open: "three decoders open" is measured as decoder instances created and fed, from the app''s own report; their output window is not opened.')
    $g.Add('busy-viewport: CPU with the interface busy runs single-viewport (FOXSDR_SINGLE_VIEWPORT=1, the DEMOD SCOPE drawn inside the main window) so the scripted drag''s coordinates are the window''s own; it is comparable baseline-to-candidate, not to the idle CPU run.')
    $g.Add('rate-resolution: the maximum sustained rate is the highest rung of a fixed ladder with zero drops, not a continuous search between rungs.')
    $g.Add('longest-frame: the soak''s longest frame is reported, not gated - one frame is one sample and swings by tens of ms between runs of one binary; "no new stalls" is gated as the count of frames over 100 ms.')
    if ($pluginCount -lt 3) { $g.Add("decoders-count: section 3 asks for three decoders open; this session staged $pluginCount decoder plugin(s).") }
    foreach ($m in @('frames', 'cpu', 'cpubusy', 'latency', 'rates', 'soak')) {
        if ($measureList -notcontains $m) { $g.Add("not-measured: the '$m' measure was not taken in this session.") }
    }
    return , $g.ToArray()
}

# ---------------------------------------------------------------------------
# The environment record
# ---------------------------------------------------------------------------
function Get-EnvRecord([string]$sizeText) {
    $cv = Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion'
    $gpus = @(Get-CimInstance Win32_VideoController | ForEach-Object {
            "$($_.Name) driver $($_.DriverVersion) $($_.CurrentHorizontalResolution)x$($_.CurrentVerticalResolution)@$($_.CurrentRefreshRate)"
        }) -join '; '
    $scheme = (powercfg /getactivescheme) -join ' '
    $scheme = ($scheme -replace '^Power Scheme GUID:\s*', '').Trim()
    $bat = @(Get-CimInstance Win32_Battery -ErrorAction SilentlyContinue)
    $battery = 'none'
    if ($bat.Count -gt 0) { $battery = ($bat | ForEach-Object { "status $($_.BatteryStatus)" }) -join '; ' }
    $cpuName = (Get-CimInstance Win32_Processor | Select-Object -First 1).Name.Trim()
    return [ordered]@{
        os          = "Windows $($cv.DisplayVersion) build $($cv.CurrentBuild).$($cv.UBR)"
        gpu         = $gpus
        powerPlan   = $scheme
        battery     = $battery
        cpu         = "$cpuName, $([Environment]::ProcessorCount) logical"
        windowSize  = $sizeText
        machine     = $env:COMPUTERNAME
    }
}

function Test-SameEnv($a, $b) {
    $diffs = @()
    foreach ($k in @('os', 'gpu', 'powerPlan', 'battery', 'cpu', 'windowSize', 'machine')) {
        $va = Get-P $a $k $null
        $vb = Get-P $b $k $null
        if ("$va" -ne "$vb") { $diffs += "$k : '$va' vs '$vb'" }
    }
    return , $diffs
}

# ---------------------------------------------------------------------------
# One launch
# ---------------------------------------------------------------------------
$BlackHole = 'http://127.0.0.1:9/'

# cascade.exe and ctest.exe processes other than the one this script is
# waiting on: another session's app, or a test suite that launches the app,
# would share the machine with the run being measured.
function Get-OtherCount([int]$exceptId) {
    $n = 0
    foreach ($pr in @(Get-Process -Name cascade, ctest -ErrorAction SilentlyContinue)) {
        if ($pr.Id -ne $exceptId) { ++$n }
    }
    return $n
}

function Invoke-Launch {
    param([string]$Exe, [string]$Dir, [string]$Measure, [hashtable]$Setup)
    New-Item -ItemType Directory -Force -Path $Dir | Out-Null
    $scratch = Join-Path $Dir 'profile'
    New-Item -ItemType Directory -Force -Path (Join-Path $scratch 'appdata'), (Join-Path $scratch 'localappdata') | Out-Null

    $runSeconds = $Setup.Seconds
    if ($Measure -eq 'soak') { $runSeconds = $Setup.SoakSeconds + $Setup.WarmupSeconds }
    $vars = [ordered]@{
        APPDATA                    = (Join-Path $scratch 'appdata')
        LOCALAPPDATA               = (Join-Path $scratch 'localappdata')
        FOXSDR_TELEMETRY_URL       = $BlackHole
        FOXSDR_CRASH_URL           = $BlackHole
        FOXSDR_UPDATE_URL          = $BlackHole
        FOXSDR_PROBLEM_URL         = $BlackHole
        FOXSDR_FEATURE_URL         = $BlackHole
        FOXSDR_REPORTS_URL         = $BlackHole
        FOXSDR_WINDOW_SIZE         = $Setup.WindowSize
        FOXSDR_OPEN_DEMOD_SCOPE    = '1'
        FOXSDR_MEASURE_OUT         = (Join-Path $Dir 'result.json')
        FOXSDR_MEASURE_WARMUP      = "$($Setup.WarmupSeconds)"
        FOXSDR_MEASURE_SECONDS     = "$runSeconds"
        FOXSDR_MEASURE_RATE        = '2048000'
        FOXSDR_MEASURE_RATES       = $Setup.RateLadder
        FOXSDR_MEASURE_WINDOW      = "$($Setup.RateWindow)"
        FOXSDR_MEASURE_RETUNES     = "$($Setup.Retunes)"
    }
    $capText = "$($Setup.FrameCapHz)"
    switch ($Measure) {
        'identify' { $vars.FOXSDR_MEASURE = 'run'; $vars.FOXSDR_MEASURE_SECONDS = '2'; $vars.FOXSDR_MEASURE_WARMUP = '0.5' }
        'frames' { $vars.FOXSDR_MEASURE = 'run'; $vars.FOXSDR_FRAME_LOG = (Join-Path $Dir 'frames.log'); $vars.FOXSDR_VSYNC_OFF = '1' }
        'cpu' { $vars.FOXSDR_MEASURE = 'run'; $vars.FOXSDR_FRAME_CAP_HZ = $capText }
        'cpubusy' {
            $vars.FOXSDR_MEASURE = 'run'; $vars.FOXSDR_FRAME_CAP_HZ = $capText
            $vars.FOXSDR_SINGLE_VIEWPORT = '1'; $vars.FOXSDR_INPUT_SCRIPT = $Setup.BusyScript
        }
        'latency' { $vars.FOXSDR_MEASURE = 'latency'; $vars.FOXSDR_FRAME_CAP_HZ = $capText }
        'rates' { $vars.FOXSDR_MEASURE = 'rates'; $vars.FOXSDR_FRAME_CAP_HZ = $capText }
        'soak' {
            $vars.FOXSDR_MEASURE = 'run'; $vars.FOXSDR_FRAME_CAP_HZ = $capText
            $vars.FOXSDR_FRAME_LOG = (Join-Path $Dir 'frames.log')
            # The hang watchdog's reports land here, and only here (main.cpp
            # diagnosticsOnDisk: FOXSDR_DIAG_DIR redirects the whole tree and
            # turns capture on in a bounded run).
            $vars.FOXSDR_DIAG_DIR = (Join-Path $Dir 'diag')
        }
        default { throw "unknown measure $Measure" }
    }
    $timeout = 120 + $runSeconds
    if ($Measure -eq 'latency') { $timeout = 120 + 2 * $Setup.Retunes }
    if ($Measure -eq 'rates') { $timeout = 120 + ($Setup.RateLadder.Split(',').Count) * ($Setup.RateWindow + $Setup.WarmupSeconds + 5) }

    # Wait out anything else that would share the machine with this run.
    $waited = 0
    $deadline = (Get-Date).AddMinutes($Setup.WaitForOthersMinutes)
    while ((Get-OtherCount -1) -gt 0 -and (Get-Date) -lt $deadline) {
        if ($waited -eq 0) { Write-Host "    waiting: another cascade.exe or ctest.exe is running" }
        Start-Sleep -Seconds 10
        $waited += 10
    }
    $others = Get-OtherCount -1
    $exeHash = (Get-FileHash -Algorithm SHA256 $Exe).Hash.ToLower()
    $info = [ordered]@{ measure = $Measure; exe = $Exe; exeSha256 = $exeHash; waitedS = $waited
                        otherCascadeAtStart = $others; otherProcessesDuring = 0 }
    if ($others -gt 0) {
        $info.ended = 'not started (other cascade.exe/ctest.exe still running)'
        $info.exitCode = $null
        Write-Json $info (Join-Path $Dir 'launch.json')
        Write-Warning "$Measure run in $Dir not started: other processes still running after $($Setup.WaitForOthersMinutes) min"
        return $info
    }

    # The child's environment: ours, minus every FOXSDR_/CASCADE_ switch this
    # shell happens to carry, plus the run's own.
    $saved = @{}
    foreach ($e in @([Environment]::GetEnvironmentVariables('Process').Keys)) {
        $ks = [string]$e
        if ($ks -like 'FOXSDR_*' -or $ks -like 'CASCADE_*') { $saved[$ks] = [Environment]::GetEnvironmentVariable($ks, 'Process') }
    }
    foreach ($k in $vars.Keys) { if (-not $saved.ContainsKey($k)) { $saved[$k] = [Environment]::GetEnvironmentVariable($k, 'Process') } }
    try {
        foreach ($k in @($saved.Keys)) { if ($k -like 'FOXSDR_*' -or $k -like 'CASCADE_*') { [Environment]::SetEnvironmentVariable($k, $null, 'Process') } }
        foreach ($k in $vars.Keys) { [Environment]::SetEnvironmentVariable($k, $vars[$k], 'Process') }
        $proc = Start-Process -FilePath $Exe -ArgumentList '--frames', '1000000' -PassThru `
            -WorkingDirectory (Split-Path -Parent $Exe) `
            -RedirectStandardOutput (Join-Path $Dir 'stdout.txt') -RedirectStandardError (Join-Path $Dir 'stderr.txt')
    } finally {
        foreach ($k in $saved.Keys) { [Environment]::SetEnvironmentVariable($k, $saved[$k], 'Process') }
    }
    $myId = $proc.Id
    $null = $proc.Handle  # held now, so ExitCode is readable after the exit
    $how = 'exited'
    $during = 0
    $until = (Get-Date).AddSeconds($timeout)
    $done = $false
    while (-not $done -and (Get-Date) -lt $until) {
        $done = $proc.WaitForExit(5000)
        if (-not $done) {
            $o = Get-OtherCount $myId
            if ($o -gt $during) { $during = $o }
        }
    }
    if (-not $done) {
        $how = 'wm_close'
        [void][FoxMeasure2]::CloseWindowsOf($myId)
        if (-not $proc.WaitForExit(30000)) {
            $how = 'killed'
            Stop-Process -Id $myId -Force
            $proc.WaitForExit(10000) | Out-Null
        }
    }
    $info.pid = $myId
    $info.ended = $how
    $info.exitCode = $proc.ExitCode
    $info.otherProcessesDuring = $during
    Write-Json $info (Join-Path $Dir 'launch.json')
    if ($how -ne 'exited') { Write-Warning "$Measure run in $Dir did not finish on its own ($how)" }
    return $info
}

# ---------------------------------------------------------------------------
# One run, validated, and its figures
# ---------------------------------------------------------------------------
# $want: sha256 and commit the build was staged as; $knobs: expectedDecoders,
# retunes, minValidRetunes, warmupSeconds. Every check below adds a reason
# whose first word is its tag; the self-test asserts the tag, and deleting a
# line marked CHECK:<tag> must turn the self-test red.
function Test-Run([string]$Dir, [string]$Measure, $want, $knobs) {
    $reasons = New-Object System.Collections.Generic.List[string]
    $fig = [ordered]@{}
    $rec = [ordered]@{ dir = $Dir; valid = $false; reasons = $reasons; figures = $fig; commit = ''; others = 0 }
    $launchPath = Join-Path $Dir 'launch.json'
    $launch = $null
    if (Test-Path $launchPath) { $launch = Read-Json $launchPath }
    if ($null -eq $launch) { $reasons.Add('launch: no launch.json - the run was never made') }  # CHECK:launch
    else {
        $ended = [string](Get-P $launch 'ended' '')
        if ($ended -ne 'exited') { $reasons.Add("ended: the app did not exit on its own ($ended)") }  # CHECK:ended
        $ec = Get-P $launch 'exitCode' $null
        if ($null -eq $ec -or [int64]$ec -ne 0) { $reasons.Add("exit-code: the app exited with '$ec'") }  # CHECK:exit-code
        $hash = [string](Get-P $launch 'exeSha256' '')
        if ($hash -ne [string]$want.sha256) { $reasons.Add("exe-hash: launched $hash, staged $($want.sha256)") }  # CHECK:exe-hash
        $oa = Get-P $launch 'otherCascadeAtStart' $null
        $od = Get-P $launch 'otherProcessesDuring' $null
        if ($null -ne $oa -and $null -ne $od) { $rec.others = [int]$oa + [int]$od }
        if ($null -eq $oa -or $null -eq $od -or [int]$oa -ne 0 -or [int]$od -ne 0) { $reasons.Add("others: another cascade.exe/ctest.exe ran (at start '$oa', during '$od')") }  # CHECK:others
    }
    $resPath = Join-Path $Dir 'result.json'
    if (-not (Test-Path $resPath)) {
        $reasons.Add('no-result: no result.json - the run crashed or never finished')  # CHECK:no-result
        return $rec
    }
    $j = Read-Json $resPath
    if ([string](Get-P $j 'format' '') -ne 'foxsdr-measure/2') { $reasons.Add("format: result format '$(Get-P $j 'format' '')' is not foxsdr-measure/2") }  # CHECK:format
    $err = [string](Get-P $j 'error' '')
    if ($err -ne '') { $reasons.Add("error: $err") }  # CHECK:error
    if ((Get-P $j 'faulted' $true) -ne $false) { $reasons.Add('faulted: the pipeline reported a fault (or did not say)') }  # CHECK:faulted
    $rec.commit = [string](Get-P $j 'commit' '')
    if ([string]$want.commit -ne '' -and $rec.commit -ne [string]$want.commit) { $reasons.Add("commit: the app reports '$($rec.commit)', staged as '$($want.commit)'") }  # CHECK:commit

    switch ($Measure) {
        { $_ -in @('frames', 'cpu', 'cpubusy', 'soak') } {
            $rate = [double](Get-P $j 'rateHz' 0)
            $inRate = [double](Get-P $j 'inputRateHz' -1)
            if ($inRate -ne $rate -or $rate -le 0) { $reasons.Add("rate-mismatch: asked $rate Hz, the chain ran at $inRate Hz") }  # CHECK:rate-mismatch-run
            $drop = Get-P $j 'ringDropped' $null
            if ($null -eq $drop -or [double]$drop -ne 0) { $reasons.Add("dropped: the ring dropped '$drop' samples") }  # CHECK:dropped-run
            $active = [int](Get-P $j 'decodersActive' -1)
            if ($active -lt [int]$knobs.expectedDecoders) { $reasons.Add("decoders: $active decoder instances fed, $($knobs.expectedDecoders) staged") }  # CHECK:decoders
            $fed = [double](Get-P $j 'decoderAudioFramesFed' 0) + [double](Get-P $j 'decoderIqFramesFed' 0)
            if ([int]$knobs.expectedDecoders -gt 0 -and $fed -le 0) { $reasons.Add('decoders-fed: the decoders were handed no samples in the window') }  # CHECK:decoders-fed
            $win = [double](Get-P $j 'windowS' 0)
            if ($win -le 0) { $reasons.Add("window: the measured window was $win s") }  # CHECK:window
            if ($win -gt 0) { $fig.cpuPct = 100.0 * [double](Get-P $j 'cpuS' 0) / $win }
            $fig.workingSetMB = [double](Get-P $j 'workingSetBytes' 0) / 1MB
            $fig.ringDropped = [double](Get-P $j 'ringDropped' 0)
            $fig.decodersActive = [double]$active
            if ($Measure -eq 'frames' -or $Measure -eq 'soak') {
                $logPath = Join-Path $Dir 'frames.log'
                if (-not (Test-Path $logPath)) { $reasons.Add('frame-log: no frames.log') }  # CHECK:frame-log
                if (Test-Path $logPath) {
                    $w = $null; $n = 0L; $ov = 0L
                    $iv = [FoxMeasure2]::FrameIntervalsMs($logPath, [double]$knobs.warmupSeconds, [ref]$w, [ref]$n, [ref]$ov)
                    if ($ov -gt 0 -or $iv.Length -eq 0) { $reasons.Add("frame-log: overflowed by $ov frames or empty ($($iv.Length) intervals)") }  # CHECK:frame-log-overflow
                    if ($iv.Length -gt 0) {
                        $fig.frameMeanMs = [FoxMeasure2]::Mean($iv)
                        $fig.frameP99Ms = [FoxMeasure2]::NearestRank($iv, 99)
                        $fig.frameMaxMs = [FoxMeasure2]::Max($iv)
                        $fig.workMeanMs = [FoxMeasure2]::Mean($w)
                        $fig.workP99Ms = [FoxMeasure2]::NearestRank($w, 99)
                        $fig.intervals = [double]$iv.Length
                        $fig.stallsOver100Ms = [double][FoxMeasure2]::CountOver($iv, 100.0)
                    }
                }
            }
            if ($Measure -eq 'cpubusy') {
                $ticks = [double](Get-P $j 'ticks' 0)
                $moves = [double](Get-P $j 'vfoChanges' 0)
                if ($ticks -le 0 -or $moves -lt 0.9 * $ticks) { $reasons.Add("drag: the VFO moved on $moves of $ticks frames - the scripted drag did not land") }  # CHECK:drag
                $fig.vfoChangeFraction = 0.0
                if ($ticks -gt 0) { $fig.vfoChangeFraction = $moves / $ticks }
            }
            if ($Measure -eq 'soak') {
                $crashDir = Join-Path $Dir 'diag\crashes'
                if (-not (Test-Path $crashDir)) { $reasons.Add('hang-watch: diag\crashes does not exist - the hang watchdog had nowhere to write') }  # CHECK:hang-watch
                $fig.hangReports = [double]@(Get-ChildItem -Path (Join-Path $Dir 'diag') -Recurse -Filter 'hang-*.txt' -ErrorAction SilentlyContinue).Count
                $so = Join-Path $Dir 'stdout.txt'
                if (Test-Path $so) {
                    foreach ($line in [IO.File]::ReadAllLines($so)) {
                        if ($line -like 'cascade: worst frame gap *') {
                            $fig.worstGapMs = [double]::Parse(($line -replace '^cascade: worst frame gap\s+', '' -replace '\s*ms\s*$', ''), [Globalization.CultureInfo]::InvariantCulture)
                        }
                    }
                }
            }
        }
        'latency' {
            $rate = [double](Get-P $j 'rateHz' 0)
            $inRate = [double](Get-P $j 'inputRateHz' -1)
            if ($inRate -ne $rate -or $rate -le 0) { $reasons.Add("rate-mismatch: asked $rate Hz, the chain ran at $inRate Hz") }  # CHECK:rate-mismatch-latency
            $drop = Get-P $j 'ringDropped' $null
            if ($null -eq $drop -or [double]$drop -ne 0) { $reasons.Add("dropped: the ring dropped '$drop' samples during the retunes") }  # CHECK:dropped-latency
            $all = @(Get-P $j 'retunes' @())
            $ok = @($all | Where-Object { [double]$_.latencyMs -ge 0 -and -not $_.missed } | ForEach-Object { [double]$_.latencyMs })
            if ($all.Count -ne [int]$knobs.retunes) { $reasons.Add("retunes-total: $($all.Count) retunes attempted, $($knobs.retunes) asked") }  # CHECK:retunes-total
            if ($ok.Count -lt [int]$knobs.minValidRetunes) { $reasons.Add("retunes: $($ok.Count) of $($all.Count) retunes produced a figure, $($knobs.minValidRetunes) needed") }  # CHECK:retunes-valid
            $awayMax = 0.0
            if ($all.Count -gt 0) { $awayMax = [FoxMeasure2]::Max([double[]]@($all | ForEach-Object { [double]$_.awayRatio })) }
            if ($awayMax -ge 0.25) { $reasons.Add("away: the tone was still at $awayMax of steady after the VFO moved off it - the retune did not take it away") }  # CHECK:away
            if ($ok.Count -gt 0) {
                $fig.latencyMedianMs = [FoxMeasure2]::Median([double[]]$ok)
                $fig.latencyP90Ms = [FoxMeasure2]::NearestRank([double[]]$ok, 90)
            }
            $fig.retunesValid = [double]$ok.Count
            $fig.awayRatioMax = $awayMax
            $fig.ringDropped = [double](Get-P $j 'ringDropped' 0)
        }
        'rates' {
            $steps = @(Get-P $j 'steps' @())
            if ($steps.Count -eq 0) { $reasons.Add('rates-steps: no rate was measured') }  # CHECK:rates-steps
            $best = 0.0; $failedAt = 0.0
            foreach ($s in $steps) {
                if ([double]$s.inputRateHz -ne [double]$s.requestedHz) { $reasons.Add("rate-mismatch: rung $($s.requestedHz) Hz ran at $($s.inputRateHz) Hz") }  # CHECK:rate-mismatch-rates
            }
            foreach ($s in $steps) {
                if ([double]$s.dropped -eq 0) { $best = [double]$s.requestedHz } else { $failedAt = [double]$s.requestedHz; break }
            }
            $fig.maxRateHz = $best
            $fig.firstDropHz = $failedAt
        }
    }
    $rec.valid = ($reasons.Count -eq 0)
    return $rec
}

# ---------------------------------------------------------------------------
# The gates
# ---------------------------------------------------------------------------
# kind: 'abs' (allowance in the figure's own unit), 'rel' (fractional),
# 'nolower' (higher is better, allowance 0), 'nohigher' (lower is better,
# allowance 0, a count), 'none' (none new: the candidate's total may not
# exceed the baseline's), 'info' (reported, never judged).
$GateList = @(
    @{ name = 'frame p99 (ms)'; measure = 'frames'; field = 'frameP99Ms'; kind = 'abs'; allowance = 1.0 },
    @{ name = 'frame mean (ms)'; measure = 'frames'; field = 'frameMeanMs'; kind = 'rel'; allowance = 0.05 },
    @{ name = 'CPU idle (% of a core)'; measure = 'cpu'; field = 'cpuPct'; kind = 'rel'; allowance = 0.05 },
    @{ name = 'CPU busy (% of a core)'; measure = 'cpubusy'; field = 'cpuPct'; kind = 'rel'; allowance = 0.05 },
    @{ name = 'working set 60 s (MB)'; measure = 'cpu'; field = 'workingSetMB'; kind = 'rel'; allowance = 0.05 },
    @{ name = 'tune-to-audio median (ms)'; measure = 'latency'; field = 'latencyMedianMs'; kind = 'abs'; allowance = 16.7 },
    @{ name = 'max sustained rate (Hz)'; measure = 'rates'; field = 'maxRateHz'; kind = 'nolower'; allowance = 0.0 },
    @{ name = 'soak hang reports'; measure = 'soak'; field = 'hangReports'; kind = 'none'; allowance = 0.0 },
    @{ name = 'soak frames > 100 ms'; measure = 'soak'; field = 'stallsOver100Ms'; kind = 'nohigher'; allowance = 0.0 },
    @{ name = 'soak working set 10 min (MB)'; measure = 'soak'; field = 'workingSetMB'; kind = 'rel'; allowance = 0.05 },
    @{ name = 'soak longest frame (ms)'; measure = 'soak'; field = 'frameMaxMs'; kind = 'info'; allowance = 0.0 }
)

function Get-Verdict($gate, [double[]]$base, [double[]]$cand) {
    if ($base.Count -eq 0 -or $cand.Count -eq 0) { return 'NO DATA' }
    $bMed = Get-Median $base; $cMed = Get-Median $cand
    $bMin = [FoxMeasure2]::Min($base); $bMax = [FoxMeasure2]::Max($base)
    $cMin = [FoxMeasure2]::Min($cand); $cMax = [FoxMeasure2]::Max($cand)
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
        'nohigher' {
            if ($cMax -le $bMax) { return 'PASS' }
            if ($cMin -gt $bMax) { return 'FAIL' }
            return 'RE-MEASURE (10 a side)'
        }
        'none' {
            if ([FoxMeasure2]::Sum($cand) -le [FoxMeasure2]::Sum($base)) { return 'PASS' }
            return 'FAIL'
        }
        'info' { return 'INFO (reported, not a gate)' }
    }
    throw "unknown gate kind $($gate.kind)"
}

# One gate's row, from the two builds' blocks for its measure (a live
# summary's dictionaries or a saved summary's parsed JSON alike).
function Get-GateRow($gate, $bBlock, $cBlock) {
    $row = [ordered]@{ gate = $gate.name; verdict = '' }
    # Not taken by either side: listed among the gaps, judged by neither.
    # Taken by one side only: there is nothing to hold it against.
    if ($null -eq $bBlock -and $null -eq $cBlock) { $row.verdict = 'NOT MEASURED'; return $row }
    if ($null -eq $bBlock -or $null -eq $cBlock) { $row.verdict = 'NO DATA'; return $row }
    $bn = [int](Get-P $bBlock 'runsExpected' 0); $bk = [int](Get-P $bBlock 'runsValid' 0)
    $cn = [int](Get-P $cBlock 'runsExpected' 0); $ck = [int](Get-P $cBlock 'runsValid' 0)
    $b = @(@(Get-P (Get-P $bBlock 'values' $null) $gate.field @()) | ForEach-Object { [double]$_ })
    $c = @(@(Get-P (Get-P $cBlock 'values' $null) $gate.field @()) | ForEach-Object { [double]$_ })
    if ($b.Count -gt 0) { $row.baselineMedian = Get-Median ([double[]]$b); $row.baselineRange = @([FoxMeasure2]::Min([double[]]$b), [FoxMeasure2]::Max([double[]]$b)); $row.baselineRuns = $b }
    if ($c.Count -gt 0) { $row.candidateMedian = Get-Median ([double[]]$c); $row.candidateRange = @([FoxMeasure2]::Min([double[]]$c), [FoxMeasure2]::Max([double[]]$c)); $row.candidateRuns = $c }
    # EVERY run of both builds valid, or no verdict: a median over the
    # survivors of a crashing build is a figure about luck.
    if ($bn -le 0 -or $cn -le 0 -or $bk -lt $bn -or $ck -lt $cn) {  # CHECK:gate-all-runs
        $row.verdict = "INVALID (baseline $bk of $bn runs valid, candidate $ck of $cn)"
        return $row
    }
    $row.verdict = Get-Verdict $gate ([double[]]$b) ([double[]]$c)
    return $row
}

function Get-ExitCode($rows, [bool]$noisyOk) {
    $verdicts = @($rows | ForEach-Object { [string](Get-P $_ 'verdict' '') })
    if (@($verdicts | Where-Object { $_ -like 'REFUSED*' }).Count -gt 0) { return 3 }  # CHECK:exit-refused
    if (@($verdicts | Where-Object { $_ -eq 'FAIL' }).Count -gt 0) { return 1 }  # CHECK:exit-fail
    if (@($verdicts | Where-Object { $_ -like 'INVALID*' -or $_ -eq 'NO DATA' }).Count -gt 0) { return 2 }  # CHECK:exit-invalid
    if (@($verdicts | Where-Object { $_ -like 'RE-MEASURE*' }).Count -gt 0) { return 4 }  # CHECK:exit-remeasure
    if (-not $noisyOk -and @($verdicts | Where-Object { $_ -like 'NOISY*' }).Count -gt 0) { return 4 }  # CHECK:exit-noisy
    return 0
}

function Get-Summary([string]$Root, [bool]$noisyOk) {
    $meta = Read-Json (Join-Path $Root 'session.json')
    $measureList = @(Get-P $meta 'measures' @())
    $knobs = @{
        expectedDecoders = [int](Get-P $meta 'expectedDecoders' 0)
        retunes          = [int](Get-P $meta 'retunes' 50)
        minValidRetunes  = [int](Get-P $meta 'minValidRetunes' 45)
        warmupSeconds    = [double](Get-P $meta 'warmupSeconds' 5)
    }
    $buildBlocks = [ordered]@{}
    $othersSeen = 0
    $buildsMeta = Get-P $meta 'builds' $null
    foreach ($label in @($buildsMeta.PSObject.Properties.Name)) {
        $bm = Get-P $buildsMeta $label $null
        $want = @{ sha256 = [string](Get-P $bm 'sha256' ''); commit = [string](Get-P $bm 'commit' '') }
        $perMeasure = [ordered]@{}
        foreach ($m in $measureList) {
            $n = [int](Get-P $meta 'runs' 5)
            if ($m -eq 'soak') { $n = [int](Get-P $meta 'soakRuns' 2) }
            $recs = New-Object System.Collections.Generic.List[object]
            $vals = [ordered]@{}
            $k = 0
            for ($r = 1; $r -le $n; ++$r) {
                $rec = Test-Run (Join-Path $Root "runs\$label-$m-r$r") $m $want $knobs
                if ($rec.others -gt $othersSeen) { $othersSeen = $rec.others }
                $recs.Add([ordered]@{ run = $r; valid = $rec.valid; reasons = @($rec.reasons); commit = $rec.commit; figures = $rec.figures })
                if ($rec.valid) {
                    ++$k
                    foreach ($fk in $rec.figures.Keys) {
                        $fv = [double]$rec.figures[$fk]
                        if ([double]::IsNaN($fv) -or [double]::IsInfinity($fv)) { continue }
                        if (-not $vals.Contains($fk)) { $vals[$fk] = New-Object System.Collections.Generic.List[double] }
                        $vals[$fk].Add($fv)
                    }
                }
            }
            $med = [ordered]@{}
            foreach ($fk in $vals.Keys) { $med[$fk] = Get-Median ([double[]]$vals[$fk].ToArray()) }
            $perMeasure[$m] = [ordered]@{ runsExpected = $n; runsValid = $k; values = $vals; median = $med; runs = $recs }
        }
        $buildBlocks[$label] = [ordered]@{ exe = Get-P $bm 'exe' ''; sha256 = $want.sha256; commit = $want.commit; measures = $perMeasure }
    }
    $envStable = [bool](Get-P $meta 'envStable' $false)
    $gateRows = @()
    if ($buildBlocks.Contains('baseline') -and $buildBlocks.Contains('candidate')) {
        foreach ($g in $GateList) {
            $baseBlk = $null; $candBlk = $null
            if ($buildBlocks.baseline.measures.Contains($g.measure)) { $baseBlk = $buildBlocks.baseline.measures[$g.measure] }
            if ($buildBlocks.candidate.measures.Contains($g.measure)) { $candBlk = $buildBlocks.candidate.measures[$g.measure] }
            $row = Get-GateRow $g $baseBlk $candBlk
            if (-not $envStable) { $row.verdict = 'REFUSED (environment changed during the session)' }  # CHECK:env-stable
            $gateRows += $row
        }
    }
    if ($gateRows.Count -gt 0) {
        $code = Get-ExitCode $gateRows $noisyOk
    } else {
        # One build: nothing to judge; the code says whether every run held.
        $code = 0
        foreach ($blk in $buildBlocks.Values) { foreach ($mb in $blk.measures.Values) { if ($mb.runsValid -lt $mb.runsExpected) { $code = 2 } } }
        if (-not $envStable) { $code = 3 }
    }
    return [ordered]@{
        format        = 'foxsdr-measure-summary/2'
        environment   = Get-P $meta 'environment' $null
        envStable     = $envStable
        otherProcessesSeen = $othersSeen
        runs          = Get-P $meta 'runs' 5
        soakRuns      = Get-P $meta 'soakRuns' 2
        warmupSeconds = $knobs.warmupSeconds
        soakSeconds   = Get-P $meta 'soakSeconds' 0
        measures      = $measureList
        rateLadder    = Get-P $meta 'rateLadder' ''
        frameCapHz    = Get-P $meta 'frameCapHz' 0
        retunes       = $knobs.retunes
        minValidRetunes = $knobs.minValidRetunes
        plugins       = @(Get-P $meta 'plugins' @())
        expectedDecoders = $knobs.expectedDecoders
        gaps          = @(Get-P $meta 'gaps' @())
        builds        = $buildBlocks
        gates         = $gateRows
        acceptNoisy   = $noisyOk
        exitCode      = $code
    }
}

$ExitMeaning = @{ 0 = 'every gate PASSED'; 1 = 'a gate FAILED'; 2 = 'a gate is INVALID or has NO DATA'; 3 = 'REFUSED - not comparable'; 4 = 'UNDECIDED - a gate is NOISY or needs RE-MEASURE' }

function Write-GateTable($summary) {
    foreach ($label in $summary.builds.Keys) {
        $blk = $summary.builds[$label]
        Write-Host ("{0}: {1}  commit {2}" -f $label, $blk.exe, $blk.commit)
        foreach ($m in $blk.measures.Keys) {
            $mb = $blk.measures[$m]
            Write-Host ("  {0}: {1} of {2} runs valid" -f $m, $mb.runsValid, $mb.runsExpected)
            foreach ($rr in $mb.runs) {
                if (-not $rr.valid) { Write-Host ("      r{0} INVALID: {1}" -f $rr.run, ($rr.reasons -join ' | ')) }
            }
            foreach ($fk in $mb.median.Keys) {
                $runList = ($mb.values[$fk] | ForEach-Object { '{0:G5}' -f $_ }) -join ', '
                Write-Host ("    {0,-18} median {1,12:G6}   runs [{2}]" -f $fk, $mb.median[$fk], $runList)
            }
        }
    }
    if (@($summary.gates).Count -gt 0) {
        Write-Host ''
        Write-Host ('{0,-30} {1,12} {2,-24} {3,12} {4,-24} {5}' -f 'gate', 'base med', 'base range', 'cand med', 'cand range', 'verdict')
        foreach ($g in $summary.gates) {
            $br = ''; $cr = ''; $bmText = ''; $cmText = ''
            if ($g.Contains('baselineMedian')) { $bmText = '{0:G6}' -f $g.baselineMedian; $br = '{0:G6} .. {1:G6}' -f $g.baselineRange[0], $g.baselineRange[1] }
            if ($g.Contains('candidateMedian')) { $cmText = '{0:G6}' -f $g.candidateMedian; $cr = '{0:G6} .. {1:G6}' -f $g.candidateRange[0], $g.candidateRange[1] }
            Write-Host ('{0,-30} {1,12} {2,-24} {3,12} {4,-24} {5}' -f $g.gate, $bmText, $br, $cmText, $cr, $g.verdict)
        }
    }
    Write-Host ''
    Write-Host 'Not measured (gaps):'
    foreach ($gp in $summary.gaps) { Write-Host "  - $gp" }
    if (-not $summary.envStable) { Write-Host 'REFUSED: the environment changed during the session.' }
    if ($summary.otherProcessesSeen -gt 0) { Write-Host "REFUSED: another cascade.exe/ctest.exe ran during the session ($($summary.otherProcessesSeen))." }
    if ($summary.acceptNoisy) { Write-Host 'NOTE: -AcceptNoisy was given: NOISY gates do not hold the exit code.' }
    Write-Host ("exit {0}: {1}" -f $summary.exitCode, $ExitMeaning[[int]$summary.exitCode])
}

# The busy run's script: the pointer to the VFO slider's grab, the button
# down, then the pointer swept between the slider's ends a couple of pixels a
# frame for the whole run, the button up at the end.
function New-BusyScript([string]$path, [int]$lastFrame, [double]$grabX, [double]$rowY, [double]$minX, [double]$maxX) {
    $sb = New-Object System.Text.StringBuilder
    [void]$sb.AppendLine('# measure_engine.ps1 cpubusy: drag the VFO slider for the whole run')
    [void]$sb.AppendLine(('60 screen {0} {1}' -f $grabX.ToString([Globalization.CultureInfo]::InvariantCulture), $rowY.ToString([Globalization.CultureInfo]::InvariantCulture)))
    [void]$sb.AppendLine('62 down')
    $span = $maxX - $minX
    $period = 200
    for ($fr = 64; $fr -lt $lastFrame; ++$fr) {
        $ph = ($fr - 64) % $period
        $u = $ph
        if ($ph -ge $period / 2) { $u = $period - $ph }
        $x = $minX + $span * $u / ($period / 2)
        [void]$sb.AppendLine(('{0} screen {1} {2}' -f $fr, $x.ToString('F1', [Globalization.CultureInfo]::InvariantCulture), $rowY.ToString([Globalization.CultureInfo]::InvariantCulture)))
    }
    [void]$sb.AppendLine("$lastFrame up")
    [IO.File]::WriteAllText($path, $sb.ToString(), (New-Object System.Text.UTF8Encoding($false)))
}

# ---------------------------------------------------------------------------
# Modes
# ---------------------------------------------------------------------------
if ($SelfTest) {
    $script:fail = 0
    function Check($cond, $what) { if (-not $cond) { Write-Host "FAIL $what"; $script:fail++ } }
    # Nearest rank: of 1..100, the 99th percentile is 99; of 1..10 it is 10
    # (ceil(9.9) = 10); of a single value it is that value.
    Check ([FoxMeasure2]::NearestRank([double[]](1..100), 99) -eq 99) 'p99 of 1..100'
    Check ([FoxMeasure2]::NearestRank([double[]](1..10), 99) -eq 10) 'p99 of 1..10'
    Check ([FoxMeasure2]::NearestRank([double[]]@(7), 99) -eq 7) 'p99 of one value'
    Check ([FoxMeasure2]::NearestRank([double[]]@(5, 1, 4, 2, 3), 50) -eq 3) 'p50 unsorted'
    Check ([FoxMeasure2]::Median([double[]]@(3, 1, 2)) -eq 2) 'median odd'
    Check ([FoxMeasure2]::Median([double[]]@(4, 1, 2, 3)) -eq 2.5) 'median even'
    Check ([FoxMeasure2]::CountOver([double[]]@(99, 100, 100.5, 250), 100) -eq 2) 'frames over 100 ms'
    # The frame log: warm-up cut against the FIRST start, intervals between
    # consecutive starts, work beside them.
    $tmpLog = [IO.Path]::GetTempFileName()
    $lines = @('# foxsdr-frame-log/1 frames=5 overflow=0', '# start_ns work_ns')
    $lines += '1000000000 1000000'      # t=1.0 s
    $lines += '3000000000 1000000'      # t=3.0
    $lines += '6000000000 2000000'      # t=6.0  (>= 1.0 + 5 s warm-up)
    $lines += '6010000000 3000000'      # +10 ms
    $lines += '6030000000 4000000'      # +20 ms
    [IO.File]::WriteAllLines($tmpLog, $lines)
    $w = $null; $n = 0L; $ov = 0L
    $iv = [FoxMeasure2]::FrameIntervalsMs($tmpLog, 5.0, [ref]$w, [ref]$n, [ref]$ov)
    Remove-Item $tmpLog
    Check ($n -eq 5) 'frames counted'
    Check ($iv.Length -eq 2) "intervals after warm-up ($($iv.Length))"
    Check ($iv[0] -eq 10 -and $iv[1] -eq 20) 'interval values'
    Check ($w[0] -eq 2 -and $w[1] -eq 3) 'work values'
    # The verdict rules.
    $gP99 = $GateList[0]; $gMean = $GateList[1]; $gRate = $GateList[6]; $gHang = $GateList[7]; $gStall = $GateList[8]
    Check ((Get-Verdict $gP99 @(5, 5.2, 5.1, 5.3, 5.0) @(5.5, 5.6, 5.4, 5.9, 5.7)) -eq 'PASS') 'p99 inside 1 ms passes'
    Check ((Get-Verdict $gP99 @(5, 5.2, 5.1, 5.3, 5.0) @(7, 7.1, 7.2, 7.3, 7.4)) -eq 'FAIL') 'p99 2 ms worse, no overlap fails'
    Check ((Get-Verdict $gP99 @(5, 5.2, 5.1, 5.3, 5.0) @(7, 7.1, 7.2, 7.3, 5.2)) -eq 'RE-MEASURE (10 a side)') 'p99 worse but overlapping re-measures'
    Check ((Get-Verdict $gP99 @(5, 6.5, 5.1, 5.3, 5.0) @(5, 5, 5, 5, 5)) -like 'NOISY*') 'p99 noisy baseline not judged'
    Check ((Get-Verdict $gMean @(10, 10.1, 10.2, 10.1, 10) @(10.4, 10.5, 10.3, 10.6, 10.2)) -eq 'PASS') 'mean inside 5 % passes'
    Check ((Get-Verdict $gMean @(10, 10.1, 10.2, 10.1, 10) @(11, 11.1, 11.2, 11.3, 11.4)) -eq 'FAIL') 'mean 10 % worse, no overlap fails'
    Check ((Get-Verdict $gRate @(16e6, 16e6, 16e6, 16e6, 16e6) @(16e6, 16e6, 16e6, 16e6, 16e6)) -eq 'PASS') 'rate equal passes'
    Check ((Get-Verdict $gRate @(16e6, 16e6, 16e6, 16e6, 16e6) @(14e6, 14e6, 14e6, 14e6, 14e6)) -eq 'FAIL') 'rate lower everywhere fails'
    Check ((Get-Verdict $gRate @(16e6, 16e6, 14e6, 16e6, 16e6) @(14e6, 14e6, 16e6, 14e6, 14e6)) -eq 'RE-MEASURE (10 a side)') 'rate lower but overlapping re-measures'
    Check ((Get-Verdict $gHang @(0, 0) @(0, 0)) -eq 'PASS') 'no hang reports passes'
    Check ((Get-Verdict $gHang @(0, 0) @(0, 1)) -eq 'FAIL') 'a new hang report fails'
    Check ((Get-Verdict $gStall @(1, 0) @(0, 1)) -eq 'PASS') 'stalls no higher passes'
    Check ((Get-Verdict $gStall @(0, 0) @(2, 3)) -eq 'FAIL') 'stalls higher in every run fails'
    $e1 = [pscustomobject]@{ os = 'a'; gpu = 'g'; powerPlan = 'p'; battery = 'none'; cpu = 'c'; windowSize = '1600x1000'; machine = 'm' }
    $e2 = [pscustomobject]@{ os = 'a'; gpu = 'g2'; powerPlan = 'p'; battery = 'none'; cpu = 'c'; windowSize = '1600x1000'; machine = 'm' }
    Check ((Test-SameEnv $e1 $e1).Count -eq 0) 'same environment accepted'
    Check ((Test-SameEnv $e1 $e2).Count -eq 1) 'different GPU refused'

    # --- fabricated sessions: the review's broken candidates, one defect each
    #     (make_fake_sessions.py of the 5bdcdab review, ported; every other
    #     field valid, so a verdict is attributable to exactly one check) ---
    $fakeRoot = Join-Path ([IO.Path]::GetTempPath()) ("measure-selftest-" + [guid]::NewGuid().ToString('N'))
    $shaBase = '1' * 64; $shaCand = '2' * 64
    function New-FakeSession([string]$name, [string[]]$ms, [bool]$stable = $true, [int]$nRuns = 5) {
        $sd = Join-Path $fakeRoot $name
        New-Item -ItemType Directory -Force -Path (Join-Path $sd 'runs') | Out-Null
        $sess = [ordered]@{
            format = 'foxsdr-measure-session/2'; environment = $e1; envStable = $stable; runs = $nRuns; soakRuns = 2
            warmupSeconds = 5; measures = $ms; rateLadder = 'x'; frameCapHz = 60; retunes = 50; minValidRetunes = 45
            plugins = @('a.dll', 'b.dll', 'c.dll'); expectedDecoders = 3; gaps = @('fabricated')
            builds = [ordered]@{ baseline = [ordered]@{ exe = 'a'; sha256 = $shaBase; commit = 'aaaaaaaaaaaa' }
                                 candidate = [ordered]@{ exe = 'b'; sha256 = $shaCand; commit = 'bbbbbbbbbbbb' } }
        }
        Write-Json $sess (Join-Path $sd 'session.json')
        return $sd
    }
    function New-FakeRun([string]$sd, [string]$label, [string]$m, [int]$r, $result, $launchOver = @{}, [string]$frameLog = 'ok') {
        $rd = Join-Path $sd "runs\$label-$m-r$r"
        New-Item -ItemType Directory -Force -Path $rd | Out-Null
        $sha = $shaBase; if ($label -eq 'candidate') { $sha = $shaCand }
        $la = [ordered]@{ measure = $m; exe = 'x'; exeSha256 = $sha; otherCascadeAtStart = 0; otherProcessesDuring = 0; ended = 'exited'; exitCode = 0 }
        foreach ($k in $launchOver.Keys) { $la[$k] = $launchOver[$k] }
        Write-Json $la (Join-Path $rd 'launch.json')
        if ($null -ne $result) {
            if (-not $result.Contains('commit')) {
                $result.commit = 'aaaaaaaaaaaa'; if ($label -eq 'candidate') { $result.commit = 'bbbbbbbbbbbb' }
            }
            Write-Json $result (Join-Path $rd 'result.json')
        }
        if ($m -eq 'frames' -or $m -eq 'soak') {
            if ($frameLog -ne 'none') {
                $ovf = 0; if ($frameLog -eq 'overflow') { $ovf = 7 }
                $fl = New-Object System.Text.StringBuilder
                [void]$fl.AppendLine("# foxsdr-frame-log/1 frames=40 overflow=$ovf")
                for ($q = 0; $q -lt 40; ++$q) { [void]$fl.AppendLine(('{0} 500000' -f (1000000000 + $q * 1000000000 / 4))) }
                [IO.File]::WriteAllText((Join-Path $rd 'frames.log'), $fl.ToString())
            }
            if ($m -eq 'soak') { New-Item -ItemType Directory -Force -Path (Join-Path $rd 'diag\crashes') | Out-Null }
        }
    }
    function Get-RunResult([double]$cpuSeconds, $over = @{}) {
        $o = [ordered]@{ format = 'foxsdr-measure/2'; error = ''; faulted = $false; rateHz = 2048000; inputRateHz = 2048000
                         cpuS = $cpuSeconds; windowS = 60; workingSetBytes = 115 * 1048576; ringDropped = 0
                         decodersActive = 3; decoderAudioFramesFed = 2880000; decoderIqFramesFed = 0; ticks = 3600; vfoChanges = 3600 }
        foreach ($k in $over.Keys) { $o[$k] = $over[$k] }
        return $o
    }
    function Get-LatencyResult([double]$lat, [double]$away, [int]$valid = 50, [int]$total = 50, $over = @{}) {
        $rt = @()
        for ($q = 0; $q -lt $total; ++$q) {
            $good = $q -lt $valid
            $lm = -1.0; if ($good) { $lm = $lat }
            $rt += [ordered]@{ latencyMs = $lm; wallMs = 20.0; awayRatio = $away; missed = $false }
        }
        $o = [ordered]@{ format = 'foxsdr-measure/2'; error = ''; faulted = $false; rateHz = 2048000; inputRateHz = 2048000
                         ringDropped = 0; toneAudioHz = 1000; steadyPower = 0.01; blockSamples = 256; retunes = $rt }
        foreach ($k in $over.Keys) { $o[$k] = $over[$k] }
        return $o
    }
    function Get-RatesResult($stepList) {
        $st = @()
        foreach ($s in $stepList) { $st += [ordered]@{ requestedHz = $s[0]; inputRateHz = $s[1]; dropped = $s[2]; audioSamples = 1; seconds = 60 } }
        return [ordered]@{ format = 'foxsdr-measure/2'; error = ''; faulted = $false; steps = $st }
    }
    $gateMeasure = @{}
    foreach ($gg in $GateList) { $gateMeasure[$gg.name] = $gg.measure }
    $goodSteps = @(@(20480000, 20480000, 0), @(22528000, 22528000, 0), @(24576000, 24576000, 5))
    $cpus = @(3.0, 3.1, 3.05, 3.02, 3.08)
    # One scenario: a good baseline, a candidate made by $candFn, and what
    # must come out.
    function Test-Scenario([string]$name, [string]$m, [scriptblock]$candFn, [string]$wantTag, [string]$wantVerdict, [int]$wantCode, [bool]$stable = $true) {
        $sd = New-FakeSession $name @($m) $stable
        $nr = 5; if ($m -eq 'soak') { $nr = 2 }
        for ($r = 1; $r -le $nr; ++$r) {
            switch ($m) {
                'latency' { New-FakeRun $sd 'baseline' $m $r (Get-LatencyResult 16.0 0.0) }
                'rates' { New-FakeRun $sd 'baseline' $m $r (Get-RatesResult $goodSteps) }
                default { New-FakeRun $sd 'baseline' $m $r (Get-RunResult $cpus[$r - 1]) }
            }
            & $candFn $sd $m $r
        }
        $sum = Get-Summary $sd $false
        $rows = @($sum.gates | Where-Object { $gateMeasure[$_.gate] -eq $m -and $_.verdict -notlike 'INFO*' })
        $vs = @($rows | ForEach-Object { $_.verdict })
        Check ($rows.Count -gt 0) "${name}: has a gate row"
        if ($wantVerdict -eq 'FAIL') {
            # A regression fails the gate it is about; the others may pass.
            Check (@($vs | Where-Object { $_ -eq 'FAIL' }).Count -gt 0) "${name}: no gate FAILED ($($vs -join '; '))"
        } else {
            foreach ($v in $vs) { Check ($v -like $wantVerdict) "${name}: verdict '$v' is not '$wantVerdict'" }
        }
        Check ($sum.exitCode -eq $wantCode) "${name}: exit code $($sum.exitCode), want $wantCode"
        if ($wantTag -ne '') {
            $cand = $sum.builds.candidate.measures[$m]
            $tags = @($cand.runs | ForEach-Object { $_.reasons } | ForEach-Object { ($_ -split ':')[0] })
            Check ($tags -contains $wantTag) "${name}: candidate reasons [$($tags -join ',')] lack '$wantTag'"
            $btags = @($sum.builds.baseline.measures[$m].runs | ForEach-Object { $_.reasons })
            Check ($btags.Count -eq 0) "${name}: the good baseline was refused: $($btags -join ' | ')"
        }
        return $sum
    }
    try {
        # s1: the retune never took the tone away (awayRatio 1.0) - read faster.
        [void](Test-Scenario 's1_tone_never_left' 'latency' { param($sd, $m, $r) New-FakeRun $sd 'candidate' $m $r (Get-LatencyResult 5.333 1.0) } 'away' 'INVALID*' 2)
        # s2: the candidate crashed in 4 of 5 runs (no result.json).
        [void](Test-Scenario 's2_crashes_4_of_5' 'cpu' { param($sd, $m, $r) $res = $null; if ($r -eq 3) { $res = Get-RunResult 3.0 }; New-FakeRun $sd 'candidate' $m $r $res } 'no-result' 'INVALID*' 2)
        # s3: the pipeline faulted - it did less work.
        [void](Test-Scenario 's3_pipeline_faulted' 'cpu' { param($sd, $m, $r) New-FakeRun $sd 'candidate' $m $r (Get-RunResult 1.0 @{ faulted = $true }) } 'faulted' 'INVALID*' 2)
        # s4: the chain refused every high rate.
        [void](Test-Scenario 's4_rate_refused' 'rates' { param($sd, $m, $r) New-FakeRun $sd 'candidate' $m $r (Get-RatesResult @(@(20480000, 2048000, 0), @(22528000, 2048000, 0), @(24576000, 2048000, 5))) } 'rate-mismatch' 'INVALID*' 2)
        # s5: 1 retune of 50 produced audio.
        [void](Test-Scenario 's5_one_retune_of_50' 'latency' { param($sd, $m, $r) New-FakeRun $sd 'candidate' $m $r (Get-LatencyResult 16.0 0.0 1) } 'retunes' 'INVALID*' 2)
        # s6: the cpu runs dropped samples by the million.
        [void](Test-Scenario 's6_cpu_run_dropping' 'cpu' { param($sd, $m, $r) New-FakeRun $sd 'candidate' $m $r (Get-RunResult $cpus[$r - 1] @{ ringDropped = 50000000 }) } 'dropped' 'INVALID*' 2)
        # s7 (control): genuinely twice the CPU must FAIL, exit 1.
        [void](Test-Scenario 's7_control_cpu_doubled' 'cpu' { param($sd, $m, $r) New-FakeRun $sd 'candidate' $m $r (Get-RunResult (2 * $cpus[$r - 1])) } '' 'FAIL' 1)
        # Control: an equal candidate PASSES, exit 0.
        [void](Test-Scenario 'c0_equal' 'cpu' { param($sd, $m, $r) New-FakeRun $sd 'candidate' $m $r (Get-RunResult $cpus[$r - 1]) } '' 'PASS' 0)
        # The rest of the refusal rules, one each.
        [void](Test-Scenario 'v_error' 'cpu' { param($sd, $m, $r) New-FakeRun $sd 'candidate' $m $r (Get-RunResult 3.0 @{ error = 'boom' }) } 'error' 'INVALID*' 2)
        [void](Test-Scenario 'v_ended' 'cpu' { param($sd, $m, $r) New-FakeRun $sd 'candidate' $m $r (Get-RunResult 3.0) @{ ended = 'wm_close' } } 'ended' 'INVALID*' 2)
        [void](Test-Scenario 'v_exit_code' 'cpu' { param($sd, $m, $r) New-FakeRun $sd 'candidate' $m $r (Get-RunResult 3.0) @{ exitCode = 3 } } 'exit-code' 'INVALID*' 2)
        [void](Test-Scenario 'v_exe_hash' 'cpu' { param($sd, $m, $r) New-FakeRun $sd 'candidate' $m $r (Get-RunResult 3.0) @{ exeSha256 = ('3' * 64) } } 'exe-hash' 'INVALID*' 2)
        [void](Test-Scenario 'v_commit' 'cpu' { param($sd, $m, $r) New-FakeRun $sd 'candidate' $m $r (Get-RunResult 3.0 @{ commit = 'cccccccccccc' }) } 'commit' 'INVALID*' 2)
        [void](Test-Scenario 'v_others' 'cpu' { param($sd, $m, $r) New-FakeRun $sd 'candidate' $m $r (Get-RunResult 3.0) @{ otherProcessesDuring = 1 } } 'others' 'INVALID*' 2)
        [void](Test-Scenario 'v_format' 'cpu' { param($sd, $m, $r) New-FakeRun $sd 'candidate' $m $r (Get-RunResult 3.0 @{ format = 'foxsdr-measure/1' }) } 'format' 'INVALID*' 2)
        [void](Test-Scenario 'v_no_launch' 'cpu' { param($sd, $m, $r) New-FakeRun $sd 'candidate' $m $r (Get-RunResult 3.0); Remove-Item (Join-Path $sd "runs\candidate-$m-r$r\launch.json") } 'launch' 'INVALID*' 2)
        [void](Test-Scenario 'v_cpu_rate' 'cpu' { param($sd, $m, $r) New-FakeRun $sd 'candidate' $m $r (Get-RunResult 3.0 @{ inputRateHz = 1024000 }) } 'rate-mismatch' 'INVALID*' 2)
        [void](Test-Scenario 'v_window' 'cpu' { param($sd, $m, $r) New-FakeRun $sd 'candidate' $m $r (Get-RunResult 3.0 @{ windowS = 0 }) } 'window' 'INVALID*' 2)
        [void](Test-Scenario 'v_decoders' 'cpu'{ param($sd, $m, $r) New-FakeRun $sd 'candidate' $m $r (Get-RunResult 3.0 @{ decodersActive = 0 }) } 'decoders' 'INVALID*' 2)
        [void](Test-Scenario 'v_decoders_fed' 'frames' { param($sd, $m, $r) New-FakeRun $sd 'candidate' $m $r (Get-RunResult 3.0 @{ decoderAudioFramesFed = 0 }) } 'decoders-fed' 'INVALID*' 2)
        [void](Test-Scenario 'v_frame_log' 'frames' { param($sd, $m, $r) New-FakeRun $sd 'candidate' $m $r (Get-RunResult 3.0) @{} 'none' } 'frame-log' 'INVALID*' 2)
        [void](Test-Scenario 'v_frame_log_overflow' 'frames' { param($sd, $m, $r) New-FakeRun $sd 'candidate' $m $r (Get-RunResult 3.0) @{} 'overflow' } 'frame-log' 'INVALID*' 2)
        [void](Test-Scenario 'v_drag' 'cpubusy' { param($sd, $m, $r) New-FakeRun $sd 'candidate' $m $r (Get-RunResult 3.0 @{ vfoChanges = 0 }) } 'drag' 'INVALID*' 2)
        [void](Test-Scenario 'v_hang_watch' 'soak' { param($sd, $m, $r) New-FakeRun $sd 'candidate' $m $r (Get-RunResult 3.0); Remove-Item -Recurse (Join-Path $sd "runs\candidate-$m-r$r\diag") } 'hang-watch' 'INVALID*' 2)
        [void](Test-Scenario 'v_latency_rate' 'latency' { param($sd, $m, $r) New-FakeRun $sd 'candidate' $m $r (Get-LatencyResult 16.0 0.0 50 50 @{ inputRateHz = 1024000 }) } 'rate-mismatch' 'INVALID*' 2)
        [void](Test-Scenario 'v_latency_dropped' 'latency' { param($sd, $m, $r) New-FakeRun $sd 'candidate' $m $r (Get-LatencyResult 16.0 0.0 50 50 @{ ringDropped = 4096 }) } 'dropped' 'INVALID*' 2)
        [void](Test-Scenario 'v_retunes_total' 'latency' { param($sd, $m, $r) New-FakeRun $sd 'candidate' $m $r (Get-LatencyResult 16.0 0.0 46 46) } 'retunes-total' 'INVALID*' 2)
        [void](Test-Scenario 'v_rates_steps' 'rates' { param($sd, $m, $r) New-FakeRun $sd 'candidate' $m $r (Get-RatesResult @()) } 'rates-steps' 'INVALID*' 2)
        # The floor itself: 45 of 50 is enough, 44 is not.
        [void](Test-Scenario 'v_retunes_45_ok' 'latency' { param($sd, $m, $r) New-FakeRun $sd 'candidate' $m $r (Get-LatencyResult 16.0 0.0 45) } '' 'PASS' 0)
        [void](Test-Scenario 'v_retunes_44' 'latency' { param($sd, $m, $r) New-FakeRun $sd 'candidate' $m $r (Get-LatencyResult 16.0 0.0 44) } 'retunes' 'INVALID*' 2)
        # A soak with a new hang report FAILS; an equal one passes.
        [void](Test-Scenario 'v_soak_new_hang' 'soak' { param($sd, $m, $r) New-FakeRun $sd 'candidate' $m $r (Get-RunResult 3.0); if ($r -eq 1) { Set-Content -Path (Join-Path $sd "runs\candidate-$m-r$r\diag\crashes\hang-1-1.txt") -Value 'x' } } '' 'FAIL' 1)
        # The environment changed during the session: REFUSED, exit 3.
        [void](Test-Scenario 'v_env_unstable' 'cpu' { param($sd, $m, $r) New-FakeRun $sd 'candidate' $m $r (Get-RunResult $cpus[$r - 1]) } '' 'REFUSED*' 3 $false)
        # A noisy baseline: exit 4, and 0 only when the caller accepts noise.
        $sdN = New-FakeSession 'v_noisy' @('cpu')
        $noisy = @(3.0, 4.5, 3.05, 3.02, 3.08)
        for ($r = 1; $r -le 5; ++$r) { New-FakeRun $sdN 'baseline' 'cpu' $r (Get-RunResult $noisy[$r - 1]); New-FakeRun $sdN 'candidate' 'cpu' $r (Get-RunResult $cpus[$r - 1]) }
        $sN = Get-Summary $sdN $false
        Check ($sN.exitCode -eq 4) "v_noisy: exit $($sN.exitCode), want 4 (NOISY is not a pass)"
        $sN2 = Get-Summary $sdN $true
        Check ($sN2.exitCode -eq 0) "v_noisy -AcceptNoisy: exit $($sN2.exitCode), want 0"
        Check ((Get-ExitCode @([ordered]@{ verdict = 'RE-MEASURE (10 a side)' }) $true) -eq 4) 'RE-MEASURE is never accepted'
        Check ((Get-ExitCode @([ordered]@{ verdict = 'PASS' }, [ordered]@{ verdict = 'FAIL' }, [ordered]@{ verdict = 'INVALID (x)' }) $false) -eq 1) 'FAIL outranks INVALID'
        Check ((Get-ExitCode @([ordered]@{ verdict = 'FAIL' }, [ordered]@{ verdict = 'REFUSED (x)' }) $false) -eq 3) 'REFUSED outranks FAIL'
        Check ((Get-ExitCode @([ordered]@{ verdict = 'NO DATA' }) $false) -eq 2) 'NO DATA is exit 2'
        # Every gate gets a row (a local $gates once shadowed the script's
        # list - PowerShell names are case-insensitive - and the real
        # session's gate table came out empty with nothing to say so).
        Check (@($sN.gates).Count -eq $GateList.Count) "summary has a row per gate ($(@($sN.gates).Count))"

        # --- CompareFiles: the saved-summary comparison's refusals ---
        $cmpDir = Join-Path $fakeRoot 'compare'
        New-Item -ItemType Directory -Force -Path $cmpDir | Out-Null
        $sEq = Get-Summary (Join-Path $fakeRoot 'c0_equal') $false
        Write-Json $sEq (Join-Path $cmpDir 'good.json')
        $sBad = Get-Summary (Join-Path $fakeRoot 'c0_equal') $false
        $sBad.envStable = $false
        Write-Json $sBad (Join-Path $cmpDir 'unstable.json')
        $sOth = Get-Summary (Join-Path $fakeRoot 'c0_equal') $false
        $sOth.otherProcessesSeen = 1
        Write-Json $sOth (Join-Path $cmpDir 'others.json')
        $sOld = Get-Summary (Join-Path $fakeRoot 'c0_equal') $false
        $sOld.format = 'foxsdr-measure-summary/1'
        Write-Json $sOld (Join-Path $cmpDir 'old.json')
        $sEnv = Get-Summary (Join-Path $fakeRoot 'c0_equal') $false
        $sEnv.environment = $e2
        Write-Json $sEnv (Join-Path $cmpDir 'env.json')
        $sInv = Get-Summary (Join-Path $fakeRoot 's6_cpu_run_dropping') $false
        Write-Json $sInv (Join-Path $cmpDir 'invalid.json')
        $me = $PSCommandPath
        $cases = @(
            @('good.json', 'good.json', 0, 'CompareFiles equal: exit 0'),
            @('good.json', 'unstable.json', 3, 'CompareFiles candidate envStable false: REFUSED'),
            @('unstable.json', 'good.json', 3, 'CompareFiles baseline envStable false: REFUSED'),
            @('good.json', 'others.json', 3, 'CompareFiles other processes during the candidate: REFUSED'),
            @('others.json', 'good.json', 3, 'CompareFiles other processes during the baseline: REFUSED'),
            @('good.json', 'old.json', 3, 'CompareFiles summary format /1: REFUSED'),
            @('good.json', 'env.json', 3, 'CompareFiles different environment: REFUSED'),
            @('good.json', 'invalid.json', 2, 'CompareFiles candidate runs invalid: exit 2')
        )
        foreach ($cs in $cases) {
            $null = & powershell -NoProfile -ExecutionPolicy Bypass -File $me -CompareBaseline (Join-Path $cmpDir $cs[0]) -CompareCandidate (Join-Path $cmpDir $cs[1]) 2>&1
            $got = $LASTEXITCODE
            Check ($got -eq $cs[2]) "$($cs[3]) (exit $got, want $($cs[2]))"
        }

        # --- the script's own names: PowerShell variables are
        #     case-insensitive, and an assignment to an automatic variable
        #     ($pid, $input, $matches ...) either throws or does nothing ---
        $ast = [System.Management.Automation.Language.Parser]::ParseFile($PSCommandPath, [ref]$null, [ref]$null)
        $spell = @{}
        foreach ($va in $ast.FindAll({ param($a) $a -is [System.Management.Automation.Language.VariableExpressionAst] }, $true)) {
            $nm = $va.VariablePath.UserPath
            $low = $nm.ToLowerInvariant()
            if (-not $spell.ContainsKey($low)) { $spell[$low] = New-Object System.Collections.Generic.HashSet[string] }
            [void]$spell[$low].Add($nm)
        }
        $multi = @($spell.Keys | Where-Object { $spell[$_].Count -gt 1 } | ForEach-Object { ($spell[$_] -join '/') })
        Check ($multi.Count -eq 0) "variable names with more than one spelling: $($multi -join ', ')"
        $reserved = @('pid', 'host', 'pwd', 'input', 'matches', 'error', 'args', 'profile', 'home', 'true', 'false', 'this', 'psitem',
                      'psscriptroot', 'pscommandpath', 'executioncontext', 'lastexitcode', 'foreach', 'switch', 'psboundparameters',
                      'myinvocation', 'psversiontable', 'event', 'eventargs', 'sender', 'stacktrace', 'shellid', 'nestedpromptlevel')
        $bad = @()
        foreach ($asg in $ast.FindAll({ param($a) $a -is [System.Management.Automation.Language.AssignmentStatementAst] }, $true)) {
            if ($asg.Left -is [System.Management.Automation.Language.VariableExpressionAst]) {
                $lname = $asg.Left.VariablePath.UserPath.ToLowerInvariant()
                if ($reserved -contains $lname) { $bad += $lname }
            }
        }
        foreach ($fe in $ast.FindAll({ param($a) $a -is [System.Management.Automation.Language.ForEachStatementAst] }, $true)) {
            $lname = $fe.Variable.VariablePath.UserPath.ToLowerInvariant()
            if ($reserved -contains $lname) { $bad += $lname }
        }
        foreach ($pa in $ast.FindAll({ param($a) $a -is [System.Management.Automation.Language.ParameterAst] }, $true)) {
            $lname = $pa.Name.VariablePath.UserPath.ToLowerInvariant()
            if ($reserved -contains $lname) { $bad += $lname }
        }
        Check ($bad.Count -eq 0) "assignments to automatic variables: $($bad -join ', ')"
    } finally {
        Remove-Item -Recurse -Force $fakeRoot -ErrorAction SilentlyContinue
    }
    if ($script:fail -eq 0) { Write-Host 'measure_engine self-test: PASS'; exit 0 } else { Write-Host "measure_engine self-test: $($script:fail) failed"; exit 1 }
}

if ($PSCmdlet.ParameterSetName -eq 'Summarize') {
    $s = Get-Summary $Summarize ([bool]$AcceptNoisy)
    Write-Json $s (Join-Path $Summarize 'summary.json')
    Write-GateTable $s
    exit ([int]$s.exitCode)
}

if ($PSCmdlet.ParameterSetName -eq 'CompareFiles') {
    $sa = Read-Json $CompareBaseline
    $sb2 = Read-Json $CompareCandidate
    $why = @()
    foreach ($pair in @(@($CompareBaseline, $sa), @($CompareCandidate, $sb2))) {
        $fmt = [string](Get-P $pair[1] 'format' '')
        if ($fmt -ne 'foxsdr-measure-summary/2') { $why += "$($pair[0]) is format '$fmt', not foxsdr-measure-summary/2 (its runs were never validated)" }  # CHECK:cmp-format
        if ((Get-P $pair[1] 'envStable' $false) -ne $true) { $why += "$($pair[0]): the environment changed during that session" }  # CHECK:cmp-env-stable
        $oth = Get-P $pair[1] 'otherProcessesSeen' $null
        if ($null -eq $oth -or [int]$oth -ne 0) { $why += "$($pair[0]): another cascade.exe/ctest.exe ran during that session ('$oth')" }  # CHECK:cmp-others
    }
    $diffs = Test-SameEnv (Get-P $sa 'environment' $null) (Get-P $sb2 'environment' $null)
    if ($diffs.Count -gt 0) { $why += 'the two sessions were measured in different environments: ' + ($diffs -join '; ') }  # CHECK:cmp-env-same
    if ($why.Count -gt 0) {
        Write-Host 'REFUSED:'
        $why | ForEach-Object { Write-Host "    $_" }
        exit 3
    }
    $baSum = Get-P (Get-P $sa 'builds' $null) 'baseline' $null
    $cbSum = Get-P (Get-P $sb2 'builds' $null) 'candidate' $null
    if ($null -eq $cbSum) { $cbSum = Get-P (Get-P $sb2 'builds' $null) 'baseline' $null }
    Write-Host 'NOTE: runs from two sessions are not interleaved; section 3.1 prefers one interleaved session.'
    $rows = @()
    foreach ($g in $GateList) {
        $row = Get-GateRow $g (Get-P (Get-P $baSum 'measures' $null) $g.measure $null) (Get-P (Get-P $cbSum 'measures' $null) $g.measure $null)
        Write-Host ('{0,-30} {1}' -f $g.name, $row.verdict)
        $rows += $row
    }
    Write-Host 'Not measured (gaps):'
    foreach ($gp in @(Get-P $sb2 'gaps' @())) { Write-Host "  - $gp" }
    $code = Get-ExitCode $rows ([bool]$AcceptNoisy)
    Write-Host ("exit {0}: {1}" -f $code, $ExitMeaning[$code])
    exit $code
}

# --- Measure ---------------------------------------------------------------
foreach ($m in $Measures) {
    if (@('frames', 'cpu', 'cpubusy', 'latency', 'rates', 'soak') -notcontains $m) { throw "unknown measure '$m'" }
}
# Each build is STAGED first: its executable, SoapySDR.dll and resources are
# copied under -Out, so a rebuild during the session cannot change what is
# being measured, and the plugin directory the app chooses (beside the exe) is
# the staged one rather than the build tree's.
function Copy-Stage([string]$exePath, [string]$dest) {
    New-Item -ItemType Directory -Force -Path $dest | Out-Null
    $src = Split-Path -Parent $exePath
    Copy-Item -Force $exePath (Join-Path $dest 'cascade.exe')
    foreach ($dll in @(Get-ChildItem -Path $src -Filter *.dll)) { Copy-Item -Force $dll.FullName $dest }
    if (Test-Path (Join-Path $src 'resources')) { Copy-Item -Recurse -Force (Join-Path $src 'resources') $dest }
    $pd = Join-Path $dest 'plugins'
    New-Item -ItemType Directory -Force -Path $pd | Out-Null
    foreach ($pl in $Plugins) { Copy-Item -Force $pl $pd }
    return (Join-Path $dest 'cascade.exe')
}
New-Item -ItemType Directory -Force -Path $Out | Out-Null
$knobSet = @{
    Seconds = $Seconds; WarmupSeconds = $WarmupSeconds; SoakSeconds = $SoakSeconds; RateLadder = $RateLadder
    RateWindow = $RateWindow; Retunes = $Retunes; FrameCapHz = $FrameCapHz; WindowSize = $WindowSize
    WaitForOthersMinutes = $WaitForOthersMinutes; BusyScript = (Join-Path $Out 'busy-drag.txt')
}
$stagedExe = [ordered]@{ baseline = (Copy-Stage (Resolve-Path $Baseline).Path (Join-Path $Out 'stage\baseline')) }
if ($Candidate) { $stagedExe.candidate = (Copy-Stage (Resolve-Path $Candidate).Path (Join-Path $Out 'stage\candidate')) }
$meta = [ordered]@{
    format        = 'foxsdr-measure-session/2'
    started       = (Get-Date).ToString('o')
    environment   = Get-EnvRecord $WindowSize
    envStable     = $true
    runs          = $Runs
    soakRuns      = $SoakRuns
    soakSeconds   = $SoakSeconds
    seconds       = $Seconds
    warmupSeconds = $WarmupSeconds
    measures      = $Measures
    rateLadder    = $RateLadder
    frameCapHz    = $FrameCapHz
    retunes       = $Retunes
    minValidRetunes = $MinValidRetunes
    plugins       = @($Plugins | ForEach-Object { Split-Path -Leaf $_ })
    expectedDecoders = @($Plugins).Count
    busy          = [ordered]@{ grabX = $BusyGrabX; y = $BusyY; minX = $BusyMinX; maxX = $BusyMaxX; script = $knobSet.BusyScript }
    gaps          = Get-Gaps $Measures @($Plugins).Count
    builds        = [ordered]@{}
}
# Which commit each staged build IS: asked of the staged executable itself
# (a two-second run whose result names the commit it was built from), and
# held against -BaselineCommit/-CandidateCommit when given. Every later run
# must report the same commit and hash to the same sha256.
foreach ($k in $stagedExe.Keys) {
    $srcExe = $Baseline; $expect = $BaselineCommit
    if ($k -eq 'candidate') { $srcExe = $Candidate; $expect = $CandidateCommit }
    $idDir = Join-Path $Out "identify\$k"
    $idInfo = Invoke-Launch -Exe $stagedExe[$k] -Dir $idDir -Measure 'identify' -Setup $knobSet
    $idRes = Join-Path $idDir 'result.json'
    $reported = ''
    if (Test-Path $idRes) { $reported = [string](Get-P (Read-Json $idRes) 'commit' '') }
    if ($reported -eq '' -or $idInfo.ended -ne 'exited' -or [int64](Get-P $idInfo 'exitCode' 1) -ne 0) {
        Write-Host "REFUSED: the staged $k build did not identify itself (see $idDir)"
        exit 3
    }
    if ($expect -ne '' -and -not $reported.StartsWith($expect)) {
        Write-Host "REFUSED: the staged $k build reports commit '$reported', not '$expect'"
        exit 3
    }
    if ($reported -like '*-dirty') { Write-Warning "$k was built from a modified tree ($reported): its commit does not name its source" }
    $meta.builds[$k] = [ordered]@{ exe = $stagedExe[$k]; source = (Resolve-Path $srcExe).Path; sha256 = (Get-FileHash -Algorithm SHA256 $stagedExe[$k]).Hash.ToLower(); commit = $reported }
    Write-Host ("{0}: commit {1}, sha256 {2}" -f $k, $reported, $meta.builds[$k].sha256)
}
Write-Json $meta (Join-Path $Out 'session.json')
if ($Measures -contains 'cpubusy') {
    New-BusyScript $knobSet.BusyScript ([int](($Seconds + 30) * [Math]::Max($FrameCapHz, 60))) $BusyGrabX $BusyY $BusyMinX $BusyMaxX
}

foreach ($m in $Measures) {
    $nr = $Runs
    if ($m -eq 'soak') { $nr = $SoakRuns }
    for ($r = 1; $r -le $nr; ++$r) {
        foreach ($label in $stagedExe.Keys) {
            $runDir = Join-Path $Out "runs\$label-$m-r$r"
            Write-Host ("[{0}] {1} {2} run {3}/{4}" -f (Get-Date).ToString('HH:mm:ss'), $label, $m, $r, $nr)
            [void](Invoke-Launch -Exe $stagedExe[$label] -Dir $runDir -Measure $m -Setup $knobSet)
        }
    }
}

$envEnd = Get-EnvRecord $WindowSize
$d = Test-SameEnv ([pscustomobject]$meta.environment) ([pscustomobject]$envEnd)
if ($d.Count -gt 0) {
    $meta.envStable = $false
    $meta.environmentAtEnd = $envEnd
    Write-Warning ('environment changed during the session: ' + ($d -join '; '))
}
$meta.finished = (Get-Date).ToString('o')
Write-Json $meta (Join-Path $Out 'session.json')

$s = Get-Summary $Out ([bool]$AcceptNoisy)
Write-Json $s (Join-Path $Out 'summary.json')
Write-GateTable $s
exit ([int]$s.exitCode)
