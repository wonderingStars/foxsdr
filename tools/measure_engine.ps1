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
    build staged, or reported a different commit; or OTHER WORK was running
    at the start or during the run: another cascade.exe, ctest.exe, a
    compiler or build driver (cl, link, MSBuild, ninja, cmake, ...), or a
    compiler inside a running WSL distribution (cc1plus, ld, ...); or the
    BACKGROUND LOAD of any other process, by the kernel's own per-process
    CPU totals around the run, exceeded -MaxBackgroundCores (0.25) in all or
    -MaxBackgroundProcessCores (0.05) in any one.
    AND every figure a gate consumes must be present and plausible (round 2
    of the review): the window within 10 % of the one asked; CPU above zero
    and within the machine; a working set between 16 MB and 16 GB; audio
    samples within 5 % of 48 kHz x the window; a frame log whose header
    count matches its lines, with no malformed line and at least 1000
    intervals; a tone detector with a block, a tone in 200-3500 Hz and a
    steady level, every latency above one block and under 1.5 s, the
    off-tone level below 0.05 of steady; every rung of the rate ladder in
    order, held for its window with audio flowing, ending at a drop or the
    ladder's top; a readable result.json. A gate needs ALL of
    both builds' runs of its measure valid; otherwise its verdict is INVALID.
    A session that never finished, whose end environment differs from its
    start, whose format is older, or in which other work or background load
    was seen is REFUSED as a whole.

    Runs are INTERLEAVED - baseline, candidate, baseline, candidate - measure
    by measure. A build's value is the median of its runs. The gate rules are
    the document's:
      * a gate FAILS when the candidate median is worse by more than the
        allowance AND every candidate run is worse than every baseline run -
        tested FIRST, so a clearly worse candidate fails however noisy the
        baseline;
      * variance: otherwise, if the BASELINE's runs spread by more than the
        gate (p99: max-min > 1 ms; the 5 % gates and the rate: max/min >
        1.05; latency: > 16.7 ms; the stall count: more than one) the gate
        is NOT JUDGED (NOISY);
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
         changed or differs, the session never finished, other work or
         background load during the measurements, a session or summary of
         an older format, a summary marked contaminated, a staged build that
         is not the commit it was named as, or a session that could not get
         a quiet machine within its wait budget)
      4  UNDECIDED: some gate is NOISY (not judged) or RE-MEASURE, nothing
         worse, or no gate was judged at all. -AcceptNoisy lets a NOISY gate
         count as not failing (the caller's explicit choice, printed), but
         never turns a table with no judged gate into 0; RE-MEASURE is never
         accepted. The number of gates judged is printed.
      5  TOOL ERROR: this script failed (an unreadable session file, a bug);
         not a verdict.
    Precedence: 3, then 1, then 2, then 4. The same session gives the same
    code from -Summarize, at the end of a measurement, and (as a summary)
    from -CompareFiles.

    The environment (OS build, GPU and driver, power plan, battery, CPU,
    screen, window size) is recorded before and after the session and with
    every summary; a comparison between records that differ is refused.

    SAFETY. Every launch gets APPDATA and LOCALAPPDATA pointed at a fresh
    scratch tree under -Out, every network endpoint the app knows
    (telemetry, crash, update, problem/feature reports) pointed at an
    unroutable black hole, every other FOXSDR_*/CASCADE_* variable of this
    shell removed for the child, and USERPROFILE left alone. Only the PID this
    script started is ever addressed; a run that overstays is sent WM_CLOSE
    and only killed if that fails. A launch waits while any of that other
    work, or background load over the limits, is present, on ONE budget for
    the whole session (-WaitForOthersMinutes); a blocker present when the
    session starts is announced before the wait, and a blocker that outlasts
    the budget stops the session with exit 3, naming it. Windows is polled
    every 5 s during a run, WSL every 30 s and only if a distribution is
    already running (asking a stopped WSL would boot its VM).

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
    # The WHOLE SESSION's budget for waiting on other work, shared by every
    # launch in it (not per launch): exhausted, the session stops with exit 3
    # naming what blocked it.
    [Parameter(ParameterSetName = 'Measure')] [int]$WaitForOthersMinutes = 180,
    # BACKGROUND LOAD, whatever its name: CPU used by every process except the
    # measured cascade.exe, this script, and the two the app itself drives
    # (dwm, audiodg), from the kernel's own per-process totals at the start
    # and end of each run. Measured on this desktop 2026-09-26 with only the
    # owner's resident apps: the largest single process was the Claude app at
    # 0.027 cores, Chrome 0.005, the VPN 0.001; the Radar Sweep B200 capture
    # that contaminated the 6342655 baseline added rsw_logger at 0.059 cores
    # (the review measured 5-14 % of a core) and a 0.27-core python beside it.
    # So: more than 0.05 cores in any one foreign process (twice the largest
    # resident) or more than 0.25 cores in all of them together (a quarter of
    # a core - on the i9-14900K the boost clock falls as more cores wake) and
    # the run is refused as "background-load".
    [Parameter(ParameterSetName = 'Measure')] [double]$MaxBackgroundCores = 0.25,
    [Parameter(ParameterSetName = 'Measure')] [double]$MaxBackgroundProcessCores = 0.05,
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

# A FAILURE OF THIS SCRIPT IS NOT A VERDICT. Anything thrown and not handled
# (an unreadable session file, a bug here) ends the script with exit 5 and the
# error, never with PowerShell's default 1, which a caller would read as "a
# gate FAILED".
trap {
    Write-Host "TOOL ERROR (exit 5): $($_.Exception.Message)"
    Write-Host "    at $($_.InvocationInfo.PositionMessage)"
    if ($true) { exit 5 }  # CHECK:tool-error
}

# The limits a run is judged against, recorded in the session so a later
# -Summarize judges it by the same ones.
$script:Limits = @{ maxBackgroundCores = 0.25; maxBackgroundProcessCores = 0.05 }
if ($PSCmdlet.ParameterSetName -eq 'Measure') {
    $script:Limits.maxBackgroundCores = $MaxBackgroundCores
    $script:Limits.maxBackgroundProcessCores = $MaxBackgroundProcessCores
}
# Tune-to-audio: the off-tone level just before a retune back must be below
# this share of steady. Every one of the 500 retunes of the 2026-09-25/26
# A/B read exactly 0.0; a tone still present at 5 % of its power (-13 dB)
# after the VFO moved 300 kHz away is a leak, and the first round's 0.25
# let a candidate whose tone never left (0.24) through with a 5.3 ms figure.
$MaxAwayRatio = 0.05
# The fewest frame intervals a frame-time figure is taken over: section 3.1
# counts on "thousands of samples, so the 99th is a real tail".
$MinIntervals = 1000
# The audio rate every result's sample counts are against (Pipeline::kAudioRateHz).
$AudioRateHz = 48000.0

# ---------------------------------------------------------------------------
# The arithmetic, compiled: 100k-line frame logs are too slow to parse in 5.1
# script, and one implementation is what the self-test pins.
# ---------------------------------------------------------------------------
if (-not ('FoxMeasure3' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;

public static class FoxMeasure3 {
    // Frame intervals (ms) between consecutive frame starts whose start lies
    // at least warmupS after the FIRST frame's start; the work times (ms) of
    // the same frames beside them.
    // headerFrames: the frames= the writer put in the header (-1 if none);
    // malformed: lines that are not "start work" pairs of integers (a cut
    // file's last line). Neither throws: the caller refuses the run.
    public static double[] FrameIntervalsMs(string path, double warmupS, out double[] workMs,
                                            out long frames, out long overflow,
                                            out long headerFrames, out long malformed) {
        List<long> starts = new List<long>();
        List<long> works = new List<long>();
        overflow = 0; headerFrames = -1; malformed = 0;
        foreach (string line in File.ReadLines(path)) {
            if (line.Length == 0) continue;
            if (line[0] == '#') {
                foreach (string tok in line.Split(' ')) {
                    long v;
                    if (tok.StartsWith("overflow=") && long.TryParse(tok.Substring(9), NumberStyles.Integer, CultureInfo.InvariantCulture, out v)) overflow = v;
                    if (tok.StartsWith("frames=") && long.TryParse(tok.Substring(7), NumberStyles.Integer, CultureInfo.InvariantCulture, out v)) headerFrames = v;
                }
                continue;
            }
            int sp = line.IndexOf(' ');
            long s0, w0;
            if (sp <= 0 || !long.TryParse(line.Substring(0, sp), NumberStyles.Integer, CultureInfo.InvariantCulture, out s0)
                || !long.TryParse(line.Substring(sp + 1), NumberStyles.Integer, CultureInfo.InvariantCulture, out w0)) {
                ++malformed;
                continue;
            }
            starts.Add(s0);
            works.Add(w0);
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

    // --- every process's CPU so far, from the kernel, in one call ---
    // NtQuerySystemInformation(SystemProcessInformation): no handle is opened
    // on any process (Get-Process's TotalProcessorTime opens one each, and
    // took 3.6 s over 614 processes on this desktop), so the snapshot is
    // cheap enough to take around every run without being load itself.
    [DllImport("ntdll.dll")] static extern int NtQuerySystemInformation(int cls, IntPtr buf, int len, out int ret);
    public static void ProcSnapshot(out long[] ids, out string[] names, out double[] cpuS, out long[] created,
                                    out long[] parents) {
        int len = 1 << 20;
        IntPtr buf = IntPtr.Zero;
        List<long> i1 = new List<long>(); List<string> n1 = new List<string>();
        List<double> c1 = new List<double>(); List<long> t1 = new List<long>(); List<long> p1 = new List<long>();
        try {
            while (true) {
                buf = Marshal.AllocHGlobal(len);
                int ret;
                int st = NtQuerySystemInformation(5, buf, len, out ret);
                if (st == unchecked((int)0xC0000004)) { Marshal.FreeHGlobal(buf); buf = IntPtr.Zero; len = Math.Max(len * 2, ret + 65536); continue; }
                if (st != 0) throw new InvalidOperationException("NtQuerySystemInformation 0x" + st.ToString("X8"));
                break;
            }
            long off = 0;
            while (true) {
                IntPtr p = new IntPtr(buf.ToInt64() + off);
                int next = Marshal.ReadInt32(p, 0);
                long create = Marshal.ReadInt64(p, 32);
                long user = Marshal.ReadInt64(p, 40);
                long kern = Marshal.ReadInt64(p, 48);
                int nlen = Marshal.ReadInt16(p, 56) & 0xFFFF;
                IntPtr nbuf = Marshal.ReadIntPtr(p, 64);
                long pid = Marshal.ReadIntPtr(p, 80).ToInt64();
                long parent = Marshal.ReadIntPtr(p, 88).ToInt64();
                string nm = (nbuf == IntPtr.Zero) ? "Idle" : Marshal.PtrToStringUni(nbuf, nlen / 2);
                i1.Add(pid); n1.Add(nm); c1.Add((user + kern) / 1e7); t1.Add(create); p1.Add(parent);
                if (next == 0) break;
                off += next;
            }
        } finally {
            if (buf != IntPtr.Zero) Marshal.FreeHGlobal(buf);
        }
        ids = i1.ToArray(); names = n1.ToArray(); cpuS = c1.ToArray(); created = t1.ToArray(); parents = p1.ToArray();
    }

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

function Get-Median([double[]]$v) { return [FoxMeasure3]::Median($v) }

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
    $g.Add('background-short-lived: background load is the kernel''s per-process CPU between a snapshot before and after each run; a process born AND gone between the two is not counted (compilers are caught by name every 5 s instead), and interrupt/DPC time belongs to no process.')
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

# WHAT ELSE IS USING THE MACHINE. Another cascade.exe (another session's
# app), a test suite (ctest launches the app), and - since the first
# re-taken baseline came out NOISY with a stage-1 builder compiling in
# another worktree - any compiler or build driver, on Windows or inside WSL
# (a WSL build runs in the VM, where Get-Process cannot see it). Any of them
# shares the CPU with the run being measured, so a launch waits for them
# and a run that saw one is refused, exactly as for a second cascade.exe.
# NOT vctip or mspdbsrv: both outlive a build and sit idle (a VCTIP.EXE on
# this desktop had been alive four hours with 2 s of CPU), so counting them
# would hold every launch to the wait limit for nothing. MSBuild's reused
# worker nodes do count, and exit on their own ~15 minutes after the build.
$WinBusyNames = @('cl', 'link', 'msbuild', 'ninja', 'cmake', 'cc1plus', 'cc1', 'ld')
$WslBusyNames = @('cc1plus', 'cc1', 'ld', 'ld.bfd', 'ld.gold', 'collect2', 'as', 'ninja', 'cmake', 'ctest', 'make', 'cascade')

# The pure half, which the self-test pins: of these Windows processes (Name,
# Id) and these WSL command names, which count, other than $exceptId.
function Select-OtherNames($procs, [int]$exceptId, [string[]]$wslComms) {
    $hit = New-Object System.Collections.Generic.List[string]
    foreach ($pr in @($procs)) {
        if ([int]$pr.Id -eq $exceptId) { continue }
        $nm = ([string]$pr.Name).ToLowerInvariant()
        if ($nm -eq 'cascade' -or $nm -eq 'ctest') { $hit.Add($nm) }  # CHECK:others-app
        if ($WinBusyNames -contains $nm) { $hit.Add($nm) }  # CHECK:others-buildtools
    }
    foreach ($cm in @($wslComms)) { if ($WslBusyNames -contains ([string]$cm).Trim()) { $hit.Add('wsl:' + ([string]$cm).Trim()) } }  # CHECK:others-wsl
    return , $hit.ToArray()
}

# WSL's processes, ONLY when a distribution is already running: asking a
# stopped WSL anything boots its VM (3.6 s and a VM's worth of load on this
# desktop), which is the disturbance being guarded against.
function Get-WslComms {
    $names = @()
    try {
        $running = @(& wsl.exe -l --running -q 2>$null | ForEach-Object { ([string]$_) -replace "`0", '' } | ForEach-Object { $_.Trim() } | Where-Object { $_ -ne '' })
        foreach ($dist in $running) {
            $names += @(& wsl.exe -d $dist -e ps -eo comm= 2>$null | ForEach-Object { ([string]$_).Trim() })
        }
    } catch {
        $names = @()
    }
    return , $names
}

function Get-OtherNames([int]$exceptId, [bool]$askWsl = $true) {
    $wslNames = @()
    if ($askWsl) { $wslNames = Get-WslComms }
    $procs = @(Get-Process -ErrorAction SilentlyContinue | Select-Object Name, Id)
    return , (Select-OtherNames $procs $exceptId $wslNames)
}

# --- BACKGROUND LOAD, by what the kernel says each process used -------------
# A snapshot: pid -> name, CPU seconds so far, creation time; and when it was
# taken.
function Get-ProcSnapshot {
    $ids = $null; $names = $null; $cpuSec = $null; $made = $null; $parents = $null
    [FoxMeasure3]::ProcSnapshot([ref]$ids, [ref]$names, [ref]$cpuSec, [ref]$made, [ref]$parents)
    $map = @{}
    for ($q = 0; $q -lt $ids.Length; ++$q) { $map[[long]$ids[$q]] = @($names[$q], $cpuSec[$q], $made[$q], $parents[$q]) }
    return @{ at = [DateTime]::UtcNow.ToFileTimeUtc(); procs = $map }
}

# The pure half, which the self-test pins: from two snapshots and the wall
# time between them, the CPU (in cores) of every process except $exceptIds,
# the Idle process and the two the measured app drives (dwm presents its
# frames, audiodg plays its audio). A process alive at both ends counts its
# difference; one born in between counts everything it used. Processes that
# were born and died in between are not seen - compilers are, by name.
$AppDrivenNames = @('idle', 'dwm.exe', 'audiodg.exe')
function Measure-Background($snapA, $snapB, [double]$wallS, [long[]]$exceptIds) {
    $per = @{}
    foreach ($id in $snapB.procs.Keys) {
        if ($exceptIds -contains [long]$id -or [long]$id -eq 0) { continue }  # CHECK:bg-except
        $b = $snapB.procs[$id]
        # A child of the measured app or of this script (the app's device-scan
        # helper; this script's wsl.exe) is their work, not the background.
        if ($exceptIds -contains [long]$b[3]) { continue }  # CHECK:bg-children
        if ($AppDrivenNames -contains ([string]$b[0]).ToLowerInvariant()) { continue }  # CHECK:bg-app-driven
        $used = 0.0
        if ($snapA.procs.ContainsKey($id) -and $snapA.procs[$id][2] -eq $b[2]) { $used = [double]$b[1] - [double]$snapA.procs[$id][1] }
        elseif ([long]$b[2] -ge [long]$snapA.at) { $used = [double]$b[1] }  # CHECK:bg-newborn
        if ($used -gt 0) { $per["$($b[0]) $id"] = $used / $wallS }
    }
    $total = 0.0; $max = 0.0
    foreach ($v in $per.Values) { $total += $v; if ($v -gt $max) { $max = $v } }
    $top = @($per.GetEnumerator() | Sort-Object Value -Descending | Select-Object -First 5 | ForEach-Object { '{0} {1:F3}' -f $_.Key, $_.Value })
    return [ordered]@{ cores = $total; maxProcessCores = $max; top = $top }
}

function Test-BackgroundOver($bg, $lim) {
    return ([double]$bg.cores -gt [double]$lim.maxBackgroundCores -or [double]$bg.maxProcessCores -gt [double]$lim.maxBackgroundProcessCores)
}

# What stands in the way of a quiet run, or '' when nothing does: other work
# by name, then a 3-second sample of the background load.
function Get-Blocker([int]$exceptId) {
    $names = Get-OtherNames $exceptId
    if ($names.Count -gt 0) { return 'other work: ' + (($names | Sort-Object -Unique) -join ', ') }
    $a = Get-ProcSnapshot
    Start-Sleep -Seconds 3
    $b = Get-ProcSnapshot
    $bg = Measure-Background $a $b (([long]$b.at - [long]$a.at) / 1e7) @([long]$PID, [long]$exceptId)
    if (Test-BackgroundOver $bg $script:Limits) {
        return ('background load {0:F2} cores (limit {1}; one process at most {2}): {3}' -f $bg.cores, $script:Limits.maxBackgroundCores, $script:Limits.maxBackgroundProcessCores, ($bg.top -join ', '))
    }
    return ''
}

# THE WAIT, against ONE budget for the whole session ($script:SessionDeadline,
# set once when the session starts): a blocker that outlasts it stops the
# session instead of every launch waiting its own three hours.
$script:SessionDeadline = [DateTime]::MaxValue
function Wait-ForQuiet([scriptblock]$probe, [int]$stepSeconds) {
    $waited = 0
    $blocker = [string](& $probe)
    while ($blocker -ne '') {
        if ((Get-Date) -ge $script:SessionDeadline) { break }  # CHECK:session-budget
        if ($waited -eq 0) { Write-Host "    waiting: $blocker" }
        Start-Sleep -Seconds $stepSeconds
        $waited += $stepSeconds
        $blocker = [string](& $probe)
    }
    return [ordered]@{ quiet = ($blocker -eq ''); blocker = $blocker; waitedS = $waited }
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

    # Wait out anything else that would share the machine with this run, on
    # the session's one budget.
    $quiet = Wait-ForQuiet { Get-Blocker -1 } 10
    $atStart = Get-OtherNames -1
    $others = $atStart.Count
    $exeHash = (Get-FileHash -Algorithm SHA256 $Exe).Hash.ToLower()
    $info = [ordered]@{ measure = $Measure; exe = $Exe; exeSha256 = $exeHash; waitedS = $quiet.waitedS
                        otherCascadeAtStart = $others; otherProcessesDuring = 0
                        otherNamesAtStart = @($atStart | Sort-Object -Unique); otherNamesDuring = @()
                        backgroundCores = $null; backgroundMaxProcessCores = $null; backgroundTop = @() }
    if (-not $quiet.quiet) {
        $info.ended = 'not started (blocked: ' + $quiet.blocker + ')'
        $info.exitCode = $null
        Write-Json $info (Join-Path $Dir 'launch.json')
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
    $snapStart = Get-ProcSnapshot
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
    $duringNames = New-Object System.Collections.Generic.HashSet[string]
    $until = (Get-Date).AddSeconds($timeout)
    $done = $false
    $polls = 0
    while (-not $done -and (Get-Date) -lt $until) {
        $done = $proc.WaitForExit(5000)
        if (-not $done) {
            # Windows every 5 s; WSL every 30 s (each ask is a wsl.exe spawn).
            $seen = Get-OtherNames $myId (($polls % 6) -eq 0)
            ++$polls
            if ($seen.Count -gt $during) { $during = $seen.Count }
            foreach ($seenName in $seen) { [void]$duringNames.Add($seenName) }
        }
    }
    $info.otherNamesDuring = @($duringNames | Sort-Object)
    if (-not $done) {
        $how = 'wm_close'
        [void][FoxMeasure3]::CloseWindowsOf($myId)
        if (-not $proc.WaitForExit(30000)) {
            $how = 'killed'
            Stop-Process -Id $myId -Force
            $proc.WaitForExit(10000) | Out-Null
        }
    }
    $snapEnd = Get-ProcSnapshot
    $bg = Measure-Background $snapStart $snapEnd (([long]$snapEnd.at - [long]$snapStart.at) / 1e7) @([long]$PID, [long]$myId)
    $info.backgroundCores = $bg.cores
    $info.backgroundMaxProcessCores = $bg.maxProcessCores
    $info.backgroundTop = $bg.top
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
    $rec = [ordered]@{ dir = $Dir; valid = $false; reasons = $reasons; figures = $fig; commit = ''; others = 0; otherNames = @(); background = $false }
    $launchPath = Join-Path $Dir 'launch.json'
    $launch = $null
    if (Test-Path $launchPath) { try { $launch = Read-Json $launchPath } catch { $launch = $null } }
    if ($null -eq $launch) { $reasons.Add('launch: no readable launch.json - the run was never made') }  # CHECK:launch
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
        $rec.otherNames = @(@(Get-P $launch 'otherNamesAtStart' @()) + @(Get-P $launch 'otherNamesDuring' @()) | Where-Object { $_ } | Sort-Object -Unique)
        if ($null -eq $oa -or $null -eq $od -or [int]$oa -ne 0 -or [int]$od -ne 0) { $reasons.Add("others: other work ran on the machine (at start '$oa', during '$od': $($rec.otherNames -join ', '))") }  # CHECK:others
        # Background load of any name, from the kernel's per-process totals
        # around the run; not recorded is not quiet.
        $bgc = Get-P $launch 'backgroundCores' $null
        $bgm = Get-P $launch 'backgroundMaxProcessCores' $null
        $bgTop = @(Get-P $launch 'backgroundTop' @())
        if ($null -eq $bgc -or $null -eq $bgm) { $reasons.Add('background: the run recorded no background load') }  # CHECK:background-missing
        elseif (Test-BackgroundOver @{ cores = $bgc; maxProcessCores = $bgm } $knobs.limits) {  # CHECK:background-load
            $rec.background = $true
            $reasons.Add(('background-load: {0:F3} cores beside the run, one process at most {1:F3} (limits {2} / {3}): {4}' -f [double]$bgc, [double]$bgm, $knobs.limits.maxBackgroundCores, $knobs.limits.maxBackgroundProcessCores, ($bgTop -join ', ')))
        }
    }
    $resPath = Join-Path $Dir 'result.json'
    if (-not (Test-Path $resPath)) {
        $reasons.Add('no-result: no result.json - the run crashed or never finished')  # CHECK:no-result
        return $rec
    }
    $j = $null
    try { $j = Read-Json $resPath } catch { $j = $null }
    if ($null -eq $j) {
        $reasons.Add('result-unreadable: result.json is not valid JSON (a run cut short while writing it)')  # CHECK:result-unreadable
        return $rec
    }
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
            # The window the figures cover, against the one asked for.
            $expWin = [double]$knobs.runWindowS
            if ($Measure -eq 'soak') { $expWin = [double]$knobs.soakSeconds }
            $win = [double](Get-P $j 'windowS' 0)
            if ($win -le 0 -or [Math]::Abs($win - $expWin) -gt 0.1 * $expWin) { $reasons.Add("window: the measured window was $win s, $expWin s asked") }  # CHECK:window
            # CPU: present, above zero (a failed read used to be 0.0), and no
            # more than the machine has.
            $cpuSec = Get-P $j 'cpuS' $null
            $pct = -1.0
            if ($null -ne $cpuSec -and $win -gt 0) { $pct = 100.0 * [double]$cpuSec / $win }
            if ($pct -le 0 -or $pct -gt 100.0 * [double]$knobs.logicalCpus) { $reasons.Add("cpu: '$cpuSec' CPU seconds over $win s is not a reading") }  # CHECK:cpu-implausible
            if ($pct -gt 0) { $fig.cpuPct = $pct }
            # Working set: present and between 16 MB and 16 GB.
            $wsb = Get-P $j 'workingSetBytes' $null
            if ($null -eq $wsb -or [double]$wsb -lt 16MB -or [double]$wsb -gt 16GB) { $reasons.Add("working-set: '$wsb' bytes is not a reading") }  # CHECK:working-set
            if ($null -ne $wsb) { $fig.workingSetMB = [double]$wsb / 1MB }
            # Audio flowed at the audio rate for the whole window.
            $aud = Get-P $j 'audioSamples' $null
            if ($null -eq $aud -or $win -le 0 -or [Math]::Abs([double]$aud - $AudioRateHz * $win) -gt 0.05 * $AudioRateHz * $win) { $reasons.Add("audio: '$aud' audio samples in $win s, $($AudioRateHz * $win) expected") }  # CHECK:audio
            $fig.ringDropped = [double](Get-P $j 'ringDropped' 0)
            $fig.decodersActive = [double]$active
            if ($Measure -eq 'frames' -or $Measure -eq 'soak') {
                $logPath = Join-Path $Dir 'frames.log'
                if (-not (Test-Path $logPath)) { $reasons.Add('frame-log: no frames.log') }  # CHECK:frame-log
                if (Test-Path $logPath) {
                    $w = $null; $n = 0L; $ov = 0L; $hdr = 0L; $bad = 0L
                    $iv = [FoxMeasure3]::FrameIntervalsMs($logPath, [double]$knobs.warmupSeconds, [ref]$w, [ref]$n, [ref]$ov, [ref]$hdr, [ref]$bad)
                    if ($ov -gt 0 -or $iv.Length -eq 0) { $reasons.Add("frame-log: overflowed by $ov frames or empty ($($iv.Length) intervals)") }  # CHECK:frame-log-overflow
                    if ($hdr -ne $n) { $reasons.Add("frame-log-count: the header says $hdr frames, $n were read - the file was cut") }  # CHECK:frame-log-count
                    if ($bad -gt 0) { $reasons.Add("frame-log-malformed: $bad lines are not frame records") }  # CHECK:frame-log-malformed
                    if ($iv.Length -lt $MinIntervals) { $reasons.Add("frame-log-short: $($iv.Length) intervals after the warm-up, $MinIntervals needed for a p99") }  # CHECK:frame-log-short
                    if ($iv.Length -gt 0) {
                        $fig.frameMeanMs = [FoxMeasure3]::Mean($iv)
                        $fig.frameP99Ms = [FoxMeasure3]::NearestRank($iv, 99)
                        $fig.frameMaxMs = [FoxMeasure3]::Max($iv)
                        $fig.workMeanMs = [FoxMeasure3]::Mean($w)
                        $fig.workP99Ms = [FoxMeasure3]::NearestRank($w, 99)
                        $fig.intervals = [double]$iv.Length
                        $fig.stallsOver100Ms = [double][FoxMeasure3]::CountOver($iv, 100.0)
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
                            $gap = 0.0
                            if ([double]::TryParse(($line -replace '^cascade: worst frame gap\s+', '' -replace '\s*ms\s*$', ''), [Globalization.NumberStyles]::Float, [Globalization.CultureInfo]::InvariantCulture, [ref]$gap)) { $fig.worstGapMs = $gap }
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
            # The detector's own terms: a block, a tone in the audio band, a
            # steady level.
            $blk = [double](Get-P $j 'blockSamples' 0)
            $tone = [double](Get-P $j 'toneAudioHz' 0)
            $steady = [double](Get-P $j 'steadyPower' 0)
            if ($blk -le 0 -or $tone -lt 200 -or $tone -gt 3500 -or $steady -le 0) { $reasons.Add("latency-fields: block $blk samples, tone $tone Hz, steady $steady - the detector did not run as designed") }  # CHECK:latency-fields
            $blockMs = 1000.0 * $blk / $AudioRateHz
            $all = @(Get-P $j 'retunes' @())
            $ok = @($all | Where-Object { [double](Get-P $_ 'latencyMs' -1) -ge 0 -and -not (Get-P $_ 'missed' $true) } | ForEach-Object { [double]$_.latencyMs })
            if ($all.Count -ne [int]$knobs.retunes) { $reasons.Add("retunes-total: $($all.Count) retunes attempted, $($knobs.retunes) asked") }  # CHECK:retunes-total
            if ($ok.Count -lt [int]$knobs.minValidRetunes) { $reasons.Add("retunes: $($ok.Count) of $($all.Count) retunes produced a figure, $($knobs.minValidRetunes) needed") }  # CHECK:retunes-valid
            # A crossing in the very first block after the command, or none
            # within 1.5 s, is not a latency: the tone was already there (it
            # never left) or the clock is wrong. Every one of the 500 retunes
            # measured 2026-09-25/26 read 16 ms, three blocks.
            $odd = @($ok | Where-Object { $_ -le $blockMs -or $_ -gt 1500.0 })
            if ($odd.Count -gt 0) { $reasons.Add("latency-implausible: $($odd.Count) retunes read one block ($blockMs ms) or less, or over 1.5 s (e.g. $($odd[0]) ms)") }  # CHECK:latency-implausible
            $awayMax = 1.0
            if ($all.Count -gt 0) { $awayMax = [FoxMeasure3]::Max([double[]]@($all | ForEach-Object { [double](Get-P $_ 'awayRatio' 1.0) })) }
            if ($awayMax -ge $MaxAwayRatio) { $reasons.Add("away: the tone was still at $awayMax of steady after the VFO moved off it (limit $MaxAwayRatio) - the retune did not take it away") }  # CHECK:away
            if ($ok.Count -gt 0) {
                $fig.latencyMedianMs = [FoxMeasure3]::Median([double[]]$ok)
                $fig.latencyP90Ms = [FoxMeasure3]::NearestRank([double[]]$ok, 90)
            }
            $fig.retunesValid = [double]$ok.Count
            $fig.awayRatioMax = $awayMax
            $fig.ringDropped = [double](Get-P $j 'ringDropped' 0)
        }
        'rates' {
            $steps = @(Get-P $j 'steps' @())
            if ($steps.Count -eq 0) { $reasons.Add('rates-steps: no rate was measured') }  # CHECK:rates-steps
            foreach ($s in $steps) {
                if ([double](Get-P $s 'inputRateHz' -1) -ne [double](Get-P $s 'requestedHz' 0)) { $reasons.Add("rate-mismatch: rung $($s.requestedHz) Hz ran at $($s.inputRateHz) Hz") }  # CHECK:rate-mismatch-rates
            }
            # Each rung held for the window asked, with audio flowing all
            # through it: half a second with no audio "sustains" anything.
            $rw = [double]$knobs.rateWindow
            foreach ($s in $steps) {
                $sec = [double](Get-P $s 'seconds' 0)
                if ([Math]::Abs($sec - $rw) -gt 0.1 * $rw) { $reasons.Add("rate-window: rung $($s.requestedHz) Hz ran $sec s, $rw s asked") }  # CHECK:rate-window
                $aud = [double](Get-P $s 'audioSamples' -1)
                if ($sec -le 0 -or [Math]::Abs($aud - $AudioRateHz * $sec) -gt 0.05 * $AudioRateHz * $sec) { $reasons.Add("rate-audio: rung $($s.requestedHz) Hz produced $aud audio samples in $sec s") }  # CHECK:rate-audio
            }
            # The rungs are the ladder asked for, in order, and the run ends
            # at a drop or at the top of the ladder.
            $ladder = @($knobs.ladder)
            $asked = @($steps | ForEach-Object { [double](Get-P $_ 'requestedHz' 0) })
            $ladderOk = ($asked.Count -gt 0 -and $asked.Count -le $ladder.Count)
            for ($q = 0; $ladderOk -and $q -lt $asked.Count; ++$q) { if ($asked[$q] -ne $ladder[$q]) { $ladderOk = $false } }
            if ($ladderOk -and $asked.Count -lt $ladder.Count -and [double](Get-P $steps[$asked.Count - 1] 'dropped' 0) -eq 0) { $ladderOk = $false }
            if (-not $ladderOk) { $reasons.Add("rate-ladder: rungs [$($asked -join ',')] are not the ladder [$($ladder -join ',')] up to its first drop") }  # CHECK:rate-ladder
            $best = 0.0; $failedAt = 0.0
            foreach ($s in $steps) {
                if ([double](Get-P $s 'dropped' 1) -eq 0) { $best = [double]$s.requestedHz } else { $failedAt = [double]$s.requestedHz; break }
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
    $bMin = [FoxMeasure3]::Min($base); $bMax = [FoxMeasure3]::Max($base)
    $cMin = [FoxMeasure3]::Min($cand); $cMax = [FoxMeasure3]::Max($cand)
    # THE FAIL TEST COMES FIRST, noise or no noise. A candidate worse than the
    # baseline by more than the allowance AND worse in every run than every
    # baseline run is a regression however wide the baseline spread: the
    # spread cannot explain a gap that no pair of runs closes. (Round 2 of the
    # review: a candidate 20 % worse than every baseline run was NOISY, and
    # -AcceptNoisy turned that into exit 0.)
    switch ($gate.kind) {
        'abs' {
            if (($cMed - $bMed) -gt $gate.allowance -and $cMin -gt $bMax) { return 'FAIL' }  # CHECK:fail-first-abs
            if (($bMax - $bMin) -gt $gate.allowance) { return 'NOISY (baseline spread exceeds the gate) - not judged' }
            if (($cMed - $bMed) -le $gate.allowance) { return 'PASS' }
            return 'RE-MEASURE (10 a side)'
        }
        'rel' {
            if ($cMed -gt $bMed * (1.0 + $gate.allowance) -and $cMin -gt $bMax) { return 'FAIL' }  # CHECK:fail-first-rel
            if ($bMin -gt 0 -and ($bMax / $bMin) -gt (1.0 + $gate.allowance)) { return 'NOISY (baseline spread exceeds the gate) - not judged' }
            if ($cMed -le $bMed * (1.0 + $gate.allowance)) { return 'PASS' }
            return 'RE-MEASURE (10 a side)'
        }
        'nolower' {
            if ($cMed -lt $bMed -and $cMax -lt $bMin) { return 'FAIL' }  # CHECK:fail-first-nolower
            # The same 5 % spread rule as the other relative gates: the rate
            # ladder's rungs are ~10 % apart, so one rung of spread is noise.
            if ($bMin -gt 0 -and ($bMax / $bMin) -gt 1.05) { return 'NOISY (baseline spread exceeds 5 %) - not judged' }  # CHECK:noisy-nolower
            if ($cMed -ge $bMed) { return 'PASS' }
            return 'RE-MEASURE (10 a side)'
        }
        'nohigher' {
            if ($cMin -gt $bMax) { return 'FAIL' }  # CHECK:fail-first-nohigher
            # A count: more than one of spread between baseline runs is noise.
            if (($bMax - $bMin) -gt 1) { return 'NOISY (baseline spread exceeds one) - not judged' }  # CHECK:noisy-nohigher
            if ($cMax -le $bMax) { return 'PASS' }
            return 'RE-MEASURE (10 a side)'
        }
        'none' {
            if ([FoxMeasure3]::Sum($cand) -le [FoxMeasure3]::Sum($base)) { return 'PASS' }
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
    if ($b.Count -gt 0) { $row.baselineMedian = Get-Median ([double[]]$b); $row.baselineRange = @([FoxMeasure3]::Min([double[]]$b), [FoxMeasure3]::Max([double[]]$b)); $row.baselineRuns = $b }
    if ($c.Count -gt 0) { $row.candidateMedian = Get-Median ([double[]]$c); $row.candidateRange = @([FoxMeasure3]::Min([double[]]$c), [FoxMeasure3]::Max([double[]]$c)); $row.candidateRuns = $c }
    # EVERY run of both builds valid, or no verdict: a median over the
    # survivors of a crashing build is a figure about luck.
    if ($bn -le 0 -or $cn -le 0 -or $bk -lt $bn -or $ck -lt $cn) {  # CHECK:gate-all-runs
        $row.verdict = "INVALID (baseline $bk of $bn runs valid, candidate $ck of $cn)"
        return $row
    }
    $row.verdict = Get-Verdict $gate ([double[]]$b) ([double[]]$c)
    return $row
}

# How many gates were actually JUDGED (a PASS or a FAIL) out of those that
# are gates at all (INFO rows are not).
function Get-JudgedText($rows) {
    $gatesOnly = @($rows | Where-Object { [string](Get-P $_ 'verdict' '') -notlike 'INFO*' })
    $judged = @($gatesOnly | Where-Object { [string](Get-P $_ 'verdict' '') -in @('PASS', 'FAIL') })
    return ('judged {0} of {1} gates (PASS or FAIL); the rest were not decided' -f $judged.Count, $gatesOnly.Count)
}

function Get-ExitCode($rows, [bool]$noisyOk) {
    $verdicts = @($rows | ForEach-Object { [string](Get-P $_ 'verdict' '') })
    if (@($verdicts | Where-Object { $_ -like 'REFUSED*' }).Count -gt 0) { return 3 }  # CHECK:exit-refused
    if (@($verdicts | Where-Object { $_ -eq 'FAIL' }).Count -gt 0) { return 1 }  # CHECK:exit-fail
    if (@($verdicts | Where-Object { $_ -like 'INVALID*' -or $_ -eq 'NO DATA' }).Count -gt 0) { return 2 }  # CHECK:exit-invalid
    if (@($verdicts | Where-Object { $_ -like 'RE-MEASURE*' }).Count -gt 0) { return 4 }  # CHECK:exit-remeasure
    if (-not $noisyOk -and @($verdicts | Where-Object { $_ -like 'NOISY*' }).Count -gt 0) { return 4 }  # CHECK:exit-noisy
    # Never "passed" with nothing judged: -AcceptNoisy over a table of NOISY
    # rows is still undecided.
    if (@($verdicts | Where-Object { $_ -eq 'PASS' }).Count -eq 0) { return 4 }  # CHECK:exit-none-judged
    return 0
}

function Get-Summary([string]$Root, [bool]$noisyOk) {
    $meta = Read-Json (Join-Path $Root 'session.json')
    $measureList = @(Get-P $meta 'measures' @())
    $envRec = Get-P $meta 'environment' $null
    $logical = 256
    if ([string](Get-P $envRec 'cpu' '') -match '(\d+) logical') { $logical = [int]$Matches[1] }
    $ladder = @()
    foreach ($tok in ([string](Get-P $meta 'rateLadder' '')).Split(',')) {
        $hz = 0.0
        if ([double]::TryParse($tok.Trim(), [Globalization.NumberStyles]::Float, [Globalization.CultureInfo]::InvariantCulture, [ref]$hz)) { $ladder += $hz }
    }
    $limRec = Get-P $meta 'limits' $null
    $knobs = @{
        expectedDecoders = [int](Get-P $meta 'expectedDecoders' 0)
        retunes          = [int](Get-P $meta 'retunes' 50)
        minValidRetunes  = [int](Get-P $meta 'minValidRetunes' 45)
        warmupSeconds    = [double](Get-P $meta 'warmupSeconds' 5)
        runWindowS       = [double](Get-P $meta 'seconds' 65) - [double](Get-P $meta 'warmupSeconds' 5)
        soakSeconds      = [double](Get-P $meta 'soakSeconds' 600)
        rateWindow       = [double](Get-P $meta 'rateWindow' 60)
        ladder           = $ladder
        logicalCpus      = $logical
        limits           = @{ maxBackgroundCores = [double](Get-P $limRec 'maxBackgroundCores' 0.25)
                              maxBackgroundProcessCores = [double](Get-P $limRec 'maxBackgroundProcessCores' 0.05) }
    }
    $buildBlocks = [ordered]@{}
    $othersSeen = 0
    $backgroundRuns = 0
    $otherNameSet = New-Object System.Collections.Generic.HashSet[string]
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
                if ($rec.background) { ++$backgroundRuns }
                foreach ($on in $rec.otherNames) { [void]$otherNameSet.Add([string]$on) }
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
    # WHY A SESSION CANNOT BE COMPARED AT ALL - each one REFUSED (exit 3), the
    # same in -Summarize, at the end of a measurement and in -CompareFiles.
    $refusals = New-Object System.Collections.Generic.List[string]
    $envStable = [bool](Get-P $meta 'envStable' $false)
    if ([string](Get-P $meta 'format' '') -ne 'foxsdr-measure-session/2') { $refusals.Add("the session is format '$(Get-P $meta 'format' '')', not foxsdr-measure-session/2") }  # CHECK:session-format
    if (-not $envStable) { $refusals.Add('the environment changed during the session') }  # CHECK:env-stable
    if ($null -eq (Get-P $meta 'finished' $null)) { $refusals.Add('the session never finished: its end-of-session environment check did not run') }  # CHECK:session-unfinished
    $envEnd = Get-P $meta 'environmentAtEnd' $null
    if ($null -ne $envEnd -and (Test-SameEnv $envRec $envEnd).Count -gt 0) { $refusals.Add('the environment at the end differs from the start: ' + ((Test-SameEnv $envRec $envEnd) -join '; ')) }  # CHECK:env-at-end
    if ($othersSeen -gt 0) { $refusals.Add("other work ran on the machine during the measurements ($($otherNameSet -join ', '))") }  # CHECK:session-others
    if ($backgroundRuns -gt 0) { $refusals.Add("$backgroundRuns run(s) had background load over the limit") }  # CHECK:session-background
    $gateRows = @()
    if ($buildBlocks.Contains('baseline') -and $buildBlocks.Contains('candidate')) {
        foreach ($g in $GateList) {
            $baseBlk = $null; $candBlk = $null
            if ($buildBlocks.baseline.measures.Contains($g.measure)) { $baseBlk = $buildBlocks.baseline.measures[$g.measure] }
            if ($buildBlocks.candidate.measures.Contains($g.measure)) { $candBlk = $buildBlocks.candidate.measures[$g.measure] }
            $row = Get-GateRow $g $baseBlk $candBlk
            if ($refusals.Count -gt 0) { $row.verdict = 'REFUSED (' + ($refusals -join '; ') + ')' }
            $gateRows += $row
        }
    }
    if ($gateRows.Count -gt 0) {
        $code = Get-ExitCode $gateRows $noisyOk
    } else {
        # One build: nothing to judge; the code says whether every run held.
        $code = 0
        foreach ($blk in $buildBlocks.Values) { foreach ($mb in $blk.measures.Values) { if ($mb.runsValid -lt $mb.runsExpected) { $code = 2 } } }
        if ($refusals.Count -gt 0) { $code = 3 }
    }
    return [ordered]@{
        format        = 'foxsdr-measure-summary/2'
        environment   = $envRec
        envStable     = ($envStable -and $null -ne (Get-P $meta 'finished' $null) -and -not ($null -ne $envEnd -and (Test-SameEnv $envRec $envEnd).Count -gt 0))
        refusals      = @($refusals)
        otherProcessesSeen = $othersSeen + $backgroundRuns
        otherProcessNames = @($otherNameSet | Sort-Object)
        backgroundRuns = $backgroundRuns
        otherAtSessionStart = @(Get-P $meta 'otherAtSessionStart' @())
        limits        = $knobs.limits
        runs          = Get-P $meta 'runs' 5
        soakRuns      = Get-P $meta 'soakRuns' 2
        warmupSeconds = $knobs.warmupSeconds
        soakSeconds   = $knobs.soakSeconds
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
        judged        = (Get-JudgedText $gateRows)
        acceptNoisy   = $noisyOk
        exitCode      = $code
    }
}

$ExitMeaning = @{ 0 = 'every judged gate PASSED (see any NOTE on NOISY gates above)'; 1 = 'a gate FAILED'; 2 = 'a gate is INVALID or has NO DATA'; 3 = 'REFUSED - not comparable'; 4 = 'UNDECIDED - a gate is NOISY or needs RE-MEASURE, or none was judged'; 5 = 'TOOL ERROR' }

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
    foreach ($rf in @($summary.refusals)) { Write-Host "REFUSED: $rf" }
    if ($summary.acceptNoisy) { Write-Host 'NOTE: -AcceptNoisy was given: NOISY gates do not hold the exit code.' }
    Write-Host $summary.judged
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
    Check ([FoxMeasure3]::NearestRank([double[]](1..100), 99) -eq 99) 'p99 of 1..100'
    Check ([FoxMeasure3]::NearestRank([double[]](1..10), 99) -eq 10) 'p99 of 1..10'
    Check ([FoxMeasure3]::NearestRank([double[]]@(7), 99) -eq 7) 'p99 of one value'
    Check ([FoxMeasure3]::NearestRank([double[]]@(5, 1, 4, 2, 3), 50) -eq 3) 'p50 unsorted'
    Check ([FoxMeasure3]::Median([double[]]@(3, 1, 2)) -eq 2) 'median odd'
    Check ([FoxMeasure3]::Median([double[]]@(4, 1, 2, 3)) -eq 2.5) 'median even'
    Check ([FoxMeasure3]::CountOver([double[]]@(99, 100, 100.5, 250), 100) -eq 2) 'frames over 100 ms'
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
    $w = $null; $n = 0L; $ov = 0L; $hdr = 0L; $bad = 0L
    $iv = [FoxMeasure3]::FrameIntervalsMs($tmpLog, 5.0, [ref]$w, [ref]$n, [ref]$ov, [ref]$hdr, [ref]$bad)
    Remove-Item $tmpLog
    Check ($n -eq 5 -and $hdr -eq 5 -and $bad -eq 0) 'frames counted, header read, nothing malformed'
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
    Check ((Get-Verdict $gRate @(16e6, 16e6, 16e6, 16e6, 16e6) @(14e6, 14e6, 16e6, 14e6, 14e6)) -eq 'RE-MEASURE (10 a side)') 'rate lower but overlapping re-measures'
    Check ((Get-Verdict $gHang @(0, 0) @(0, 0)) -eq 'PASS') 'no hang reports passes'
    Check ((Get-Verdict $gHang @(0, 0) @(0, 1)) -eq 'FAIL') 'a new hang report fails'
    Check ((Get-Verdict $gStall @(1, 0) @(0, 1)) -eq 'PASS') 'stalls no higher passes'
    Check ((Get-Verdict $gStall @(0, 0) @(2, 3)) -eq 'FAIL') 'stalls higher in every run fails'
    # What counts as other work on the machine: a second app, a test suite,
    # a compiler or build driver on Windows, a compiler inside WSL - never
    # the run's own pid and never an unrelated program.
    $fakeProcs = @([pscustomobject]@{ Name = 'cl'; Id = 5 }, [pscustomobject]@{ Name = 'notepad'; Id = 6 },
                   [pscustomobject]@{ Name = 'cascade'; Id = 7 }, [pscustomobject]@{ Name = 'ctest'; Id = 8 },
                   [pscustomobject]@{ Name = 'MSBuild'; Id = 9 })
    $busy = Select-OtherNames $fakeProcs 7 @('bash', 'cc1plus', 'systemd')
    Check ($busy -contains 'cl') "a Windows compiler counts as other work ($($busy -join ','))"
    Check ($busy -contains 'msbuild') "MSBuild counts as other work ($($busy -join ','))"
    Check ($busy -contains 'ctest') "ctest counts as other work ($($busy -join ','))"
    Check ($busy -contains 'wsl:cc1plus') "a compiler inside WSL counts as other work ($($busy -join ','))"
    Check ($busy.Count -eq 4) "only those four count - not notepad, bash, systemd or the run's own cascade ($($busy -join ','))"
    Check ((Select-OtherNames $fakeProcs 1 @()) -contains 'cascade') 'another cascade.exe counts as other work'
    $e1 = [pscustomobject]@{ os = 'a'; gpu = 'g'; powerPlan = 'p'; battery = 'none'; cpu = 'c'; windowSize = '1600x1000'; machine = 'm' }
    $e2 = [pscustomobject]@{ os = 'a'; gpu = 'g2'; powerPlan = 'p'; battery = 'none'; cpu = 'c'; windowSize = '1600x1000'; machine = 'm' }
    Check ((Test-SameEnv $e1 $e1).Count -eq 0) 'same environment accepted'
    Check ((Test-SameEnv $e1 $e2).Count -eq 1) 'different GPU refused'

    # --- background load: the pure arithmetic over two crafted snapshots ---
    # wall 10 s; python used 3 s (0.3 cores); a process born in between used
    # 0.5 s (0.05); dwm (app-driven), the measured app (99), its child (12)
    # and this script are not background.
    $snapA = @{ at = 1000; procs = @{
            [long]10 = @('python.exe', 1.0, 5, 1); [long]11 = @('dwm.exe', 1.0, 5, 1); [long]12 = @('helper.exe', 0.0, 900, 99)
            [long]99 = @('cascade.exe', 0.0, 900, 1); [long]0 = @('Idle', 100.0, 0, 0) } }
    $snapB = @{ at = 101000; procs = @{
            [long]10 = @('python.exe', 4.0, 5, 1); [long]11 = @('dwm.exe', 9.0, 5, 1); [long]12 = @('helper.exe', 2.0, 900, 99)
            [long]13 = @('newborn.exe', 0.5, 2000, 1); [long]14 = @('reused.exe', 7.0, 500, 1)
            [long]99 = @('cascade.exe', 8.0, 900, 1); [long]0 = @('Idle', 400.0, 0, 0) } }
    # pid 14 was not in A and was created BEFORE A (a pid reused, or a
    # snapshot race): its lifetime CPU is not this run's and is not counted.
    $bgT = Measure-Background $snapA $snapB 10.0 @([long]99)
    Check ([Math]::Abs($bgT.cores - 0.35) -lt 1e-9) "background: python 0.3 + newborn 0.05 = 0.35 cores ($($bgT.cores))"
    Check ([Math]::Abs($bgT.maxProcessCores - 0.3) -lt 1e-9) "background: the largest one process is python's 0.3 ($($bgT.maxProcessCores))"
    Check (($bgT.top -join ',') -like 'python.exe 10 0.300*') "background: the top consumer is named ($($bgT.top -join ','))"
    Check (Test-BackgroundOver $bgT @{ maxBackgroundCores = 0.25; maxBackgroundProcessCores = 0.05 }) 'background: 0.35 cores is over the limit'
    Check (-not (Test-BackgroundOver @{ cores = 0.1; maxProcessCores = 0.03 } @{ maxBackgroundCores = 0.25; maxBackgroundProcessCores = 0.05 })) 'background: 0.1 cores, 0.03 at most, is quiet'
    Check (Test-BackgroundOver @{ cores = 0.1; maxProcessCores = 0.06 } @{ maxBackgroundCores = 0.25; maxBackgroundProcessCores = 0.05 }) 'background: one process at 0.06 cores is over the one-process limit'
    # The live snapshot works on this machine and sees this very process.
    $live = Get-ProcSnapshot
    Check ($live.procs.Count -gt 20 -and $live.procs.ContainsKey([long]$PID)) "live process snapshot ($($live.procs.Count) processes, this one included)"

    # --- the session's ONE wait budget ---
    $script:probeCalls = 0
    $blockerProbe = { $script:probeCalls++; if ($script:probeCalls -le 3) { 'fake blocker' } else { '' } }
    $script:SessionDeadline = (Get-Date).AddSeconds(-1)
    $wq = Wait-ForQuiet $blockerProbe 1
    Check (-not $wq.quiet -and $wq.blocker -eq 'fake blocker' -and $wq.waitedS -eq 0) "a spent session budget returns at once, naming the blocker (quiet $($wq.quiet), waited $($wq.waitedS))"
    $script:probeCalls = 0
    $script:SessionDeadline = (Get-Date).AddSeconds(30)
    $wq2 = Wait-ForQuiet $blockerProbe 1
    Check ($wq2.quiet -and $wq2.waitedS -eq 3) "a blocker that clears inside the budget is waited out ($($wq2.waitedS) s)"
    $script:SessionDeadline = [DateTime]::MaxValue

    # --- fabricated sessions: the review's broken candidates, one defect each
    #     (make_fake_sessions.py of the 5bdcdab review and make_v2_sessions.py
    #     of the 70f88db re-check, ported; every other field valid, so a
    #     verdict is attributable to exactly one check) ---
    $fakeRoot = Join-Path ([IO.Path]::GetTempPath()) ("measure-selftest-" + [guid]::NewGuid().ToString('N'))
    $shaBase = '1' * 64; $shaCand = '2' * 64
    $fakeLadder = '20480000,22528000,24576000,28672000'
    function New-FakeSession([string]$name, [string[]]$ms, [bool]$stable = $true, [int]$nRuns = 5) {
        $sd = Join-Path $fakeRoot $name
        New-Item -ItemType Directory -Force -Path (Join-Path $sd 'runs') | Out-Null
        $sess = [ordered]@{
            format = 'foxsdr-measure-session/2'; environment = $e1; envStable = $stable; runs = $nRuns; soakRuns = 2
            seconds = 65; warmupSeconds = 5; soakSeconds = 600; measures = $ms; rateLadder = $fakeLadder; rateWindow = 60
            frameCapHz = 60; retunes = 50; minValidRetunes = 45
            plugins = @('a.dll', 'b.dll', 'c.dll'); expectedDecoders = 3; gaps = @('fabricated')
            limits = @{ maxBackgroundCores = 0.25; maxBackgroundProcessCores = 0.05 }
            finished = '2026-09-26T00:00:00'
            builds = [ordered]@{ baseline = [ordered]@{ exe = 'a'; sha256 = $shaBase; commit = 'aaaaaaaaaaaa' }
                                 candidate = [ordered]@{ exe = 'b'; sha256 = $shaCand; commit = 'bbbbbbbbbbbb' } }
        }
        Write-Json $sess (Join-Path $sd 'session.json')
        return $sd
    }
    # frames: a clean log of 1 ms frames after the warm-up; 'cut' keeps a
    # header naming more frames than follow; 'partial' appends a line cut
    # mid-write; 'short' has too few intervals for a p99.
    function New-FrameLogText([string]$kind) {
        $count = 1100
        if ($kind -eq 'short') { $count = 500 }
        $fl = New-Object System.Text.StringBuilder
        $hdrCount = $count + 1
        if ($kind -eq 'cut') { $hdrCount = $count + 1 + 2000 }
        $ovf = 0; if ($kind -eq 'overflow') { $ovf = 7 }
        [void]$fl.AppendLine("# foxsdr-frame-log/1 frames=$hdrCount overflow=$ovf")
        [void]$fl.AppendLine('# start_ns work_ns')
        [void]$fl.AppendLine('1000000000 500000')
        for ($q = 0; $q -lt $count; ++$q) { [void]$fl.AppendLine(('{0} 500000' -f (6000000000 + $q * 1000000))) }
        if ($kind -eq 'partial') { [void]$fl.Append('7123456789') }
        return $fl.ToString()
    }
    function New-FakeRun([string]$sd, [string]$label, [string]$m, [int]$r, $result, $launchOver = @{}, [string]$frameLog = 'ok') {
        $rd = Join-Path $sd "runs\$label-$m-r$r"
        New-Item -ItemType Directory -Force -Path $rd | Out-Null
        $sha = $shaBase; if ($label -eq 'candidate') { $sha = $shaCand }
        $la = [ordered]@{ measure = $m; exe = 'x'; exeSha256 = $sha; otherCascadeAtStart = 0; otherProcessesDuring = 0; ended = 'exited'; exitCode = 0
                          backgroundCores = 0.02; backgroundMaxProcessCores = 0.01; backgroundTop = @('claude.exe 1 0.010') }
        foreach ($k in $launchOver.Keys) { if ($null -eq $launchOver[$k]) { $la.Remove($k) } else { $la[$k] = $launchOver[$k] } }
        Write-Json $la (Join-Path $rd 'launch.json')
        if ($result -is [string]) {
            [IO.File]::WriteAllText((Join-Path $rd 'result.json'), $result)
        } elseif ($null -ne $result) {
            if (-not $result.Contains('commit')) {
                $result.commit = 'aaaaaaaaaaaa'; if ($label -eq 'candidate') { $result.commit = 'bbbbbbbbbbbb' }
            }
            Write-Json $result (Join-Path $rd 'result.json')
        }
        if ($m -eq 'frames' -or $m -eq 'soak') {
            if ($frameLog -ne 'none') { [IO.File]::WriteAllText((Join-Path $rd 'frames.log'), (New-FrameLogText $frameLog)) }
            if ($m -eq 'soak') { New-Item -ItemType Directory -Force -Path (Join-Path $rd 'diag\crashes') | Out-Null }
        }
    }
    # A value of $null in $over REMOVES the field.
    function Get-RunResult([double]$cpuSeconds, $over = @{}, [double]$win = 60) {
        $o = [ordered]@{ format = 'foxsdr-measure/2'; error = ''; faulted = $false; rateHz = 2048000; inputRateHz = 2048000
                         cpuS = $cpuSeconds; windowS = $win; workingSetBytes = 115 * 1048576; ringDropped = 0; audioSamples = [long](48000 * $win)
                         decodersActive = 3; decoderAudioFramesFed = [long](48000 * $win); decoderIqFramesFed = 0; ticks = 3600; vfoChanges = 3600 }
        foreach ($k in $over.Keys) { if ($null -eq $over[$k]) { $o.Remove($k) } else { $o[$k] = $over[$k] } }
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
    # steps: @(requested, actual, dropped[, seconds[, audio]])
    function Get-RatesResult($stepList) {
        $st = @()
        foreach ($s in $stepList) {
            $sec = 60.0; if ($s.Count -gt 3) { $sec = [double]$s[3] }
            $aud = [long](48000 * $sec); if ($s.Count -gt 4) { $aud = [long]$s[4] }
            $st += [ordered]@{ requestedHz = $s[0]; inputRateHz = $s[1]; dropped = $s[2]; audioSamples = $aud; seconds = $sec }
        }
        return [ordered]@{ format = 'foxsdr-measure/2'; error = ''; faulted = $false; steps = $st }
    }
    $gateMeasure = @{}
    foreach ($gg in $GateList) { $gateMeasure[$gg.name] = $gg.measure }
    $goodSteps = @(@(20480000, 20480000, 0), @(22528000, 22528000, 0), @(24576000, 24576000, 5))
    $cpus = @(3.0, 3.1, 3.05, 3.02, 3.08)
    # One scenario: a good baseline, a candidate made by $candFn, and what
    # must come out.
    function Test-Scenario([string]$name, [string]$m, [scriptblock]$candFn, [string]$wantTag, [string]$wantVerdict, [int]$wantCode, [bool]$stable = $true, [scriptblock]$sessionFn = $null) {
        $sd = New-FakeSession $name @($m) $stable
        if ($null -ne $sessionFn) { & $sessionFn $sd }
        $nr = 5; if ($m -eq 'soak') { $nr = 2 }
        for ($r = 1; $r -le $nr; ++$r) {
            switch ($m) {
                'latency' { New-FakeRun $sd 'baseline' $m $r (Get-LatencyResult 16.0 0.0) }
                'rates' { New-FakeRun $sd 'baseline' $m $r (Get-RatesResult $goodSteps) }
                'soak' { New-FakeRun $sd 'baseline' $m $r (Get-RunResult $cpus[$r - 1] @{} 600) }
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
        # Controls first: the fixtures themselves must be valid, or every
        # "INVALID" below proves nothing.
        [void](Test-Scenario 'c0_equal' 'cpu' { param($sd, $m, $r) New-FakeRun $sd 'candidate' $m $r (Get-RunResult $cpus[$r - 1]) } '' 'PASS' 0)
        [void](Test-Scenario 'c1_frames_equal' 'frames' { param($sd, $m, $r) New-FakeRun $sd 'candidate' $m $r (Get-RunResult $cpus[$r - 1]) } '' 'PASS' 0)
        [void](Test-Scenario 'c2_latency_equal' 'latency' { param($sd, $m, $r) New-FakeRun $sd 'candidate' $m $r (Get-LatencyResult 16.0 0.0) } '' 'PASS' 0)
        [void](Test-Scenario 'c3_rates_equal' 'rates' { param($sd, $m, $r) New-FakeRun $sd 'candidate' $m $r (Get-RatesResult $goodSteps) } '' 'PASS' 0)
        [void](Test-Scenario 'c4_soak_equal' 'soak' { param($sd, $m, $r) New-FakeRun $sd 'candidate' $m $r (Get-RunResult $cpus[$r - 1] @{} 600) } '' 'PASS' 0)
        # s1: the retune never took the tone away (awayRatio 1.0) - read faster.
        [void](Test-Scenario 's1_tone_never_left' 'latency' { param($sd, $m, $r) New-FakeRun $sd 'candidate' $m $r (Get-LatencyResult 16.0 1.0) } 'away' 'INVALID*' 2)
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
        # The first round's refusal rules, one each.
        [void](Test-Scenario 'v_error' 'cpu' { param($sd, $m, $r) New-FakeRun $sd 'candidate' $m $r (Get-RunResult 3.0 @{ error = 'boom' }) } 'error' 'INVALID*' 2)
        [void](Test-Scenario 'v_ended' 'cpu' { param($sd, $m, $r) New-FakeRun $sd 'candidate' $m $r (Get-RunResult 3.0) @{ ended = 'wm_close' } } 'ended' 'INVALID*' 2)
        [void](Test-Scenario 'v_exit_code' 'cpu' { param($sd, $m, $r) New-FakeRun $sd 'candidate' $m $r (Get-RunResult 3.0) @{ exitCode = 3 } } 'exit-code' 'INVALID*' 2)
        [void](Test-Scenario 'v_exe_hash' 'cpu' { param($sd, $m, $r) New-FakeRun $sd 'candidate' $m $r (Get-RunResult 3.0) @{ exeSha256 = ('3' * 64) } } 'exe-hash' 'INVALID*' 2)
        [void](Test-Scenario 'v_commit' 'cpu' { param($sd, $m, $r) New-FakeRun $sd 'candidate' $m $r (Get-RunResult 3.0 @{ commit = 'cccccccccccc' }) } 'commit' 'INVALID*' 2)
        # Other work during a run: the run is refused AND the session is
        # REFUSED (exit 3) - the same answer -Summarize, the end of a
        # measurement and -CompareFiles give (review round 2, n8).
        [void](Test-Scenario 'v_others' 'cpu' { param($sd, $m, $r) New-FakeRun $sd 'candidate' $m $r (Get-RunResult 3.0) @{ otherProcessesDuring = 1 } } 'others' 'REFUSED*' 3)
        $sumBuild = Test-Scenario 'v_others_buildtool' 'cpu' { param($sd, $m, $r) New-FakeRun $sd 'candidate' $m $r (Get-RunResult 3.0) @{ otherProcessesDuring = 1; otherNamesDuring = @('cl') } } 'others' 'REFUSED*' 3
        Check ($sumBuild.otherProcessNames -contains 'cl') "the summary names the build tool that ran ($($sumBuild.otherProcessNames -join ','))"
        Check ($sumBuild.otherProcessesSeen -gt 0) 'a build tool during a run marks the session (CompareFiles then refuses it)'
        [void](Test-Scenario 'v_format' 'cpu' { param($sd, $m, $r) New-FakeRun $sd 'candidate' $m $r (Get-RunResult 3.0 @{ format = 'foxsdr-measure/1' }) } 'format' 'INVALID*' 2)
        [void](Test-Scenario 'v_no_launch' 'cpu' { param($sd, $m, $r) New-FakeRun $sd 'candidate' $m $r (Get-RunResult 3.0); Remove-Item (Join-Path $sd "runs\candidate-$m-r$r\launch.json") } 'launch' 'INVALID*' 2)
        [void](Test-Scenario 'v_cpu_rate' 'cpu' { param($sd, $m, $r) New-FakeRun $sd 'candidate' $m $r (Get-RunResult 3.0 @{ inputRateHz = 1024000 }) } 'rate-mismatch' 'INVALID*' 2)
        [void](Test-Scenario 'v_window' 'cpu' { param($sd, $m, $r) New-FakeRun $sd 'candidate' $m $r (Get-RunResult 3.0 @{ windowS = 30; audioSamples = 1440000; decoderAudioFramesFed = 1440000 }) } 'window' 'INVALID*' 2)
        [void](Test-Scenario 'v_decoders' 'cpu' { param($sd, $m, $r) New-FakeRun $sd 'candidate' $m $r (Get-RunResult 3.0 @{ decodersActive = 0 }) } 'decoders' 'INVALID*' 2)
        [void](Test-Scenario 'v_decoders_fed' 'frames' { param($sd, $m, $r) New-FakeRun $sd 'candidate' $m $r (Get-RunResult 3.0 @{ decoderAudioFramesFed = 0 }) } 'decoders-fed' 'INVALID*' 2)
        [void](Test-Scenario 'v_frame_log' 'frames' { param($sd, $m, $r) New-FakeRun $sd 'candidate' $m $r (Get-RunResult 3.0) @{} 'none' } 'frame-log' 'INVALID*' 2)
        [void](Test-Scenario 'v_frame_log_overflow' 'frames' { param($sd, $m, $r) New-FakeRun $sd 'candidate' $m $r (Get-RunResult 3.0) @{} 'overflow' } 'frame-log' 'INVALID*' 2)
        [void](Test-Scenario 'v_drag' 'cpubusy' { param($sd, $m, $r) New-FakeRun $sd 'candidate' $m $r (Get-RunResult 3.0 @{ vfoChanges = 0 }) } 'drag' 'INVALID*' 2)
        [void](Test-Scenario 'v_hang_watch' 'soak' { param($sd, $m, $r) New-FakeRun $sd 'candidate' $m $r (Get-RunResult 3.0 @{} 600); Remove-Item -Recurse (Join-Path $sd "runs\candidate-$m-r$r\diag") } 'hang-watch' 'INVALID*' 2)
        [void](Test-Scenario 'v_latency_rate' 'latency' { param($sd, $m, $r) New-FakeRun $sd 'candidate' $m $r (Get-LatencyResult 16.0 0.0 50 50 @{ inputRateHz = 1024000 }) } 'rate-mismatch' 'INVALID*' 2)
        [void](Test-Scenario 'v_latency_dropped' 'latency' { param($sd, $m, $r) New-FakeRun $sd 'candidate' $m $r (Get-LatencyResult 16.0 0.0 50 50 @{ ringDropped = 4096 }) } 'dropped' 'INVALID*' 2)
        [void](Test-Scenario 'v_retunes_total' 'latency' { param($sd, $m, $r) New-FakeRun $sd 'candidate' $m $r (Get-LatencyResult 16.0 0.0 46 46) } 'retunes-total' 'INVALID*' 2)
        [void](Test-Scenario 'v_rates_steps' 'rates' { param($sd, $m, $r) New-FakeRun $sd 'candidate' $m $r (Get-RatesResult @()) } 'rates-steps' 'INVALID*' 2)
        # The floor itself: 45 of 50 is enough, 44 is not.
        [void](Test-Scenario 'v_retunes_45_ok' 'latency' { param($sd, $m, $r) New-FakeRun $sd 'candidate' $m $r (Get-LatencyResult 16.0 0.0 45) } '' 'PASS' 0)
        [void](Test-Scenario 'v_retunes_44' 'latency' { param($sd, $m, $r) New-FakeRun $sd 'candidate' $m $r (Get-LatencyResult 16.0 0.0 44) } 'retunes' 'INVALID*' 2)
        # A soak with a new hang report FAILS.
        [void](Test-Scenario 'v_soak_new_hang' 'soak' { param($sd, $m, $r) New-FakeRun $sd 'candidate' $m $r (Get-RunResult 3.0 @{} 600); if ($r -eq 1) { Set-Content -Path (Join-Path $sd "runs\candidate-$m-r$r\diag\crashes\hang-1-1.txt") -Value 'x' } } '' 'FAIL' 1)
        # The environment changed during the session: REFUSED, exit 3.
        [void](Test-Scenario 'v_env_unstable' 'cpu' { param($sd, $m, $r) New-FakeRun $sd 'candidate' $m $r (Get-RunResult $cpus[$r - 1]) } '' 'REFUSED*' 3 $false)

        # --- review round 2 (make_v2_sessions.py): implausible figures ---
        # n1: the frame log cut at a line boundary - the header names 2000
        # more frames than follow.
        [void](Test-Scenario 'n1_frames_log_truncated' 'frames' { param($sd, $m, $r) New-FakeRun $sd 'candidate' $m $r (Get-RunResult $cpus[$r - 1]) @{} 'cut' } 'frame-log-count' 'INVALID*' 2)
        # n2: the frame log's last line cut mid-write.
        [void](Test-Scenario 'n2_frames_log_partial_line' 'frames' { param($sd, $m, $r) New-FakeRun $sd 'candidate' $m $r (Get-RunResult $cpus[$r - 1]) @{} 'partial' } 'frame-log-malformed' 'INVALID*' 2)
        [void](Test-Scenario 'v_frames_log_short' 'frames' { param($sd, $m, $r) New-FakeRun $sd 'candidate' $m $r (Get-RunResult $cpus[$r - 1]) @{} 'short' } 'frame-log-short' 'INVALID*' 2)
        # n3: every retune leaves the tone at 0.24 of steady (the first
        # round's limit was 0.25).
        [void](Test-Scenario 'n3_latency_away_024' 'latency' { param($sd, $m, $r) New-FakeRun $sd 'candidate' $m $r (Get-LatencyResult 16.0 0.24) } 'away' 'INVALID*' 2)
        # n12: latency 0.0 ms on every retune (the detector fired on the
        # command block); and one block exactly.
        [void](Test-Scenario 'n12_latency_zero' 'latency' { param($sd, $m, $r) New-FakeRun $sd 'candidate' $m $r (Get-LatencyResult 0.0 0.0) } 'latency-implausible' 'INVALID*' 2)
        [void](Test-Scenario 'v_latency_one_block' 'latency' { param($sd, $m, $r) New-FakeRun $sd 'candidate' $m $r (Get-LatencyResult 5.333 0.0) } 'latency-implausible' 'INVALID*' 2)
        [void](Test-Scenario 'v_latency_fields' 'latency' { param($sd, $m, $r) New-FakeRun $sd 'candidate' $m $r (Get-LatencyResult 16.0 0.0 50 50 @{ toneAudioHz = 0 }) } 'latency-fields' 'INVALID*' 2)
        # n4: the CPU reading came back zero.
        [void](Test-Scenario 'n4_cpu_zero' 'cpu' { param($sd, $m, $r) New-FakeRun $sd 'candidate' $m $r (Get-RunResult 0.0) } 'cpu' 'INVALID*' 2)
        # n5: the working-set field missing.
        [void](Test-Scenario 'n5_workingset_missing' 'cpu' { param($sd, $m, $r) New-FakeRun $sd 'candidate' $m $r (Get-RunResult $cpus[$r - 1] @{ workingSetBytes = $null }) } 'working-set' 'INVALID*' 2)
        [void](Test-Scenario 'v_audio_none' 'cpu' { param($sd, $m, $r) New-FakeRun $sd 'candidate' $m $r (Get-RunResult $cpus[$r - 1] @{ audioSamples = 0 }) } 'audio' 'INVALID*' 2)
        # n6: every rung "sustained" for half a second, and one with no audio.
        [void](Test-Scenario 'n6_rates_short_windows' 'rates' { param($sd, $m, $r) New-FakeRun $sd 'candidate' $m $r (Get-RatesResult @(@(20480000, 20480000, 0, 0.5), @(22528000, 22528000, 0, 0.5), @(24576000, 24576000, 0, 0.5), @(28672000, 28672000, 0, 0.5))) } 'rate-window' 'INVALID*' 2)
        [void](Test-Scenario 'v_rates_no_audio' 'rates' { param($sd, $m, $r) New-FakeRun $sd 'candidate' $m $r (Get-RatesResult @(@(20480000, 20480000, 0, 60, 0), @(22528000, 22528000, 0, 60, 0), @(24576000, 24576000, 5, 60, 0))) } 'rate-audio' 'INVALID*' 2)
        [void](Test-Scenario 'v_rates_skip_rung' 'rates' { param($sd, $m, $r) New-FakeRun $sd 'candidate' $m $r (Get-RatesResult @(@(20480000, 20480000, 0), @(24576000, 24576000, 0), @(28672000, 28672000, 5))) } 'rate-ladder' 'INVALID*' 2)
        [void](Test-Scenario 'v_rates_stopped_early' 'rates' { param($sd, $m, $r) New-FakeRun $sd 'candidate' $m $r (Get-RatesResult @(@(20480000, 20480000, 0), @(22528000, 22528000, 0))) } 'rate-ladder' 'INVALID*' 2)
        # n9: a killed run left a half-written result.json.
        [void](Test-Scenario 'n9_killed_partial_result' 'cpu' { param($sd, $m, $r) New-FakeRun $sd 'candidate' $m $r ("{`n  `"format`": `"foxsdr-measure/2`",`n  `"vers") } 'result-unreadable' 'INVALID*' 2)
        # n7: the session never finished (no 'finished'); and one whose end
        # environment differs while envStable still says true.
        [void](Test-Scenario 'n7_interrupted_session' 'cpu' { param($sd, $m, $r) New-FakeRun $sd 'candidate' $m $r (Get-RunResult $cpus[$r - 1]) } '' 'REFUSED*' 3 $true { param($sd) $mt = Read-Json (Join-Path $sd 'session.json'); $mt.PSObject.Properties.Remove('finished'); Write-Json $mt (Join-Path $sd 'session.json') })
        [void](Test-Scenario 'v_env_at_end' 'cpu' { param($sd, $m, $r) New-FakeRun $sd 'candidate' $m $r (Get-RunResult $cpus[$r - 1]) } '' 'REFUSED*' 3 $true { param($sd) $mt = Read-Json (Join-Path $sd 'session.json'); $mt | Add-Member -NotePropertyName environmentAtEnd -NotePropertyValue $e2; Write-Json $mt (Join-Path $sd 'session.json') })
        [void](Test-Scenario 'v_session_format' 'cpu' { param($sd, $m, $r) New-FakeRun $sd 'candidate' $m $r (Get-RunResult $cpus[$r - 1]) } '' 'REFUSED*' 3 $true { param($sd) $mt = Read-Json (Join-Path $sd 'session.json'); $mt.format = 'foxsdr-measure-session/1'; Write-Json $mt (Join-Path $sd 'session.json') })
        # Background load: over the limit in a run - the run refused and the
        # session REFUSED; not recorded at all - the run refused.
        $sumBg = Test-Scenario 'v_background_load' 'cpu' { param($sd, $m, $r) $lo = @{}; if ($r -eq 2) { $lo = @{ backgroundCores = 0.4; backgroundMaxProcessCores = 0.3; backgroundTop = @('python.exe 106668 0.300') } }; New-FakeRun $sd 'candidate' $m $r (Get-RunResult $cpus[$r - 1]) $lo } 'background-load' 'REFUSED*' 3
        Check (($sumBg.refusals -join ' ') -like '*background load*') "the session names the background load ($($sumBg.refusals -join ' | '))"
        [void](Test-Scenario 'v_background_one_process' 'cpu' { param($sd, $m, $r) New-FakeRun $sd 'candidate' $m $r (Get-RunResult $cpus[$r - 1]) @{ backgroundCores = 0.1; backgroundMaxProcessCores = 0.07; backgroundTop = @('rsw_logger_20260926.exe 88552 0.070') } } 'background-load' 'REFUSED*' 3)
        [void](Test-Scenario 'v_background_missing' 'cpu' { param($sd, $m, $r) New-FakeRun $sd 'candidate' $m $r (Get-RunResult $cpus[$r - 1]) @{ backgroundCores = $null } } 'background' 'INVALID*' 2)

        # --- verdicts: FAIL before NOISY, and noise rules for every kind ---
        # a1 (review round 2): every gate NOISY and the candidate worse than
        # every baseline run - FAIL, exit 1, with or without -AcceptNoisy.
        $sdA = New-FakeSession 'a1_all_noisy' @('cpu')
        $baseCpu = @(5.0, 6.0, 5.2, 5.1, 5.3); $baseWs = @(100e6, 120e6, 101e6, 102e6, 103e6)
        for ($r = 1; $r -le 5; ++$r) {
            New-FakeRun $sdA 'baseline' 'cpu' $r (Get-RunResult $baseCpu[$r - 1] @{ workingSetBytes = $baseWs[$r - 1] })
            New-FakeRun $sdA 'candidate' 'cpu' $r (Get-RunResult 6.3 @{ workingSetBytes = 125e6 })
        }
        $sumA1 = Get-Summary $sdA $true
        Check ($sumA1.exitCode -eq 1) "a1: a candidate worse than every noisy baseline run FAILS even with -AcceptNoisy (exit $($sumA1.exitCode))"
        # All NOISY, candidate equal: with -AcceptNoisy nothing was judged -
        # exit 4, not 0.
        $sdB = New-FakeSession 'a2_all_noisy_equal' @('cpu')
        for ($r = 1; $r -le 5; ++$r) {
            New-FakeRun $sdB 'baseline' 'cpu' $r (Get-RunResult $baseCpu[$r - 1] @{ workingSetBytes = $baseWs[$r - 1] })
            New-FakeRun $sdB 'candidate' 'cpu' $r (Get-RunResult $baseCpu[$r - 1] @{ workingSetBytes = $baseWs[$r - 1] })
        }
        $sumA2 = Get-Summary $sdB $true
        Check ($sumA2.exitCode -eq 4) "a2: -AcceptNoisy with no gate judged is exit 4, not 0 (exit $($sumA2.exitCode))"
        Check ($sumA2.judged -like 'judged 0 of*') "a2: the summary says how many gates were judged ($($sumA2.judged))"
        $gAbs = $GateList[0]; $gRate = $GateList[6]; $gStall = $GateList[8]
        Check ((Get-Verdict $gAbs @(5, 6.5, 5.1, 5.3, 5.0) @(9, 9.1, 9.2, 9.3, 9.4)) -eq 'FAIL') 'abs: a noisy baseline does not hide a candidate worse than every run'
        Check ((Get-Verdict $gRate @(16e6, 20e6, 16e6, 16e6, 16e6) @(12e6, 12e6, 12e6, 12e6, 12e6)) -eq 'FAIL') 'nolower: a noisy baseline does not hide a candidate below every run'
        Check ((Get-Verdict $gStall @(0, 3, 0, 0, 0) @(5, 6, 5, 5, 7)) -eq 'FAIL') 'nohigher: a noisy baseline does not hide a candidate above every run'
        Check ((Get-Verdict $gRate @(14.336e6, 22.528e6, 18.432e6, 22.528e6, 22.528e6) @(22.528e6, 22.528e6, 22.528e6, 22.528e6, 22.528e6)) -like 'NOISY*') 'nolower: a rate baseline spread 14.3-22.5 MS/s is NOISY (L3)'
        Check ((Get-Verdict $gStall @(0, 3, 0, 1, 0) @(0, 0, 0, 0, 0)) -like 'NOISY*') 'nohigher: a stall-count baseline spread 0-3 is NOISY (L3)'

        # A noisy baseline: exit 4, and 0 only when the caller accepts noise
        # AND some gate was judged.
        $sdN = New-FakeSession 'v_noisy' @('cpu')
        $noisy = @(3.0, 4.5, 3.05, 3.02, 3.08)
        for ($r = 1; $r -le 5; ++$r) { New-FakeRun $sdN 'baseline' 'cpu' $r (Get-RunResult $noisy[$r - 1]); New-FakeRun $sdN 'candidate' 'cpu' $r (Get-RunResult $cpus[$r - 1]) }
        $sN = Get-Summary $sdN $false
        Check ($sN.exitCode -eq 4) "v_noisy: exit $($sN.exitCode), want 4 (NOISY is not a pass)"
        $sN2 = Get-Summary $sdN $true
        Check ($sN2.exitCode -eq 0) "v_noisy -AcceptNoisy: exit $($sN2.exitCode), want 0 (the working-set gate was judged)"
        Check ((Get-ExitCode @([ordered]@{ verdict = 'RE-MEASURE (10 a side)' }) $true) -eq 4) 'RE-MEASURE is never accepted'
        Check ((Get-ExitCode @([ordered]@{ verdict = 'PASS' }, [ordered]@{ verdict = 'RE-MEASURE (10 a side)' }) $true) -eq 4) 'RE-MEASURE beside a PASS is still undecided, even with -AcceptNoisy'
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
        $sCont = Get-Summary (Join-Path $fakeRoot 'c0_equal') $false
        $sCont.contaminated = $true
        $sCont.contamination = 'measured beside a B200 capture'
        Write-Json $sCont (Join-Path $cmpDir 'contaminated.json')
        $sRef = Get-Summary (Join-Path $fakeRoot 'c0_equal') $false
        $sRef.refusals = @('the session never finished')
        Write-Json $sRef (Join-Path $cmpDir 'refused.json')
        # Every child run of this script: its own exit code and text, its
        # stderr kept out of this process's error stream (in 5.1 a native
        # command's stderr under Stop is a terminating error HERE, which would
        # crash the self-test instead of naming the failing case), and a hard
        # time limit: a child that hangs (a wait with its budget check gone)
        # is killed with its whole tree and reported as exit 124.
        function Invoke-Self([string[]]$argList, [int]$timeoutS = 300) {
            $psi = New-Object System.Diagnostics.ProcessStartInfo
            $psi.FileName = 'powershell'
            $all = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $PSCommandPath) + $argList
            $psi.Arguments = (@($all | ForEach-Object { '"' + $_ + '"' }) -join ' ')
            $psi.UseShellExecute = $false
            $psi.RedirectStandardOutput = $true
            $psi.RedirectStandardError = $true
            $psi.CreateNoWindow = $true
            $kid = [System.Diagnostics.Process]::Start($psi)
            $outTask = $kid.StandardOutput.ReadToEndAsync()
            $errTask = $kid.StandardError.ReadToEndAsync()
            if (-not $kid.WaitForExit($timeoutS * 1000)) {
                & taskkill.exe /T /F /PID $kid.Id | Out-Null
                [void]$kid.WaitForExit(10000)
                return @{ code = 124; text = "TIMED OUT after $timeoutS s" }
            }
            $kid.WaitForExit()
            return @{ code = $kid.ExitCode; text = ($outTask.Result + "`n" + $errTask.Result) }
        }
        $cases = @(
            @('good.json', 'good.json', 0, 'CompareFiles equal: exit 0'),
            @('good.json', 'unstable.json', 3, 'CompareFiles candidate envStable false: REFUSED'),
            @('unstable.json', 'good.json', 3, 'CompareFiles baseline envStable false: REFUSED'),
            @('good.json', 'others.json', 3, 'CompareFiles other processes during the candidate: REFUSED'),
            @('others.json', 'good.json', 3, 'CompareFiles other processes during the baseline: REFUSED'),
            @('good.json', 'old.json', 3, 'CompareFiles summary format /1: REFUSED'),
            @('good.json', 'env.json', 3, 'CompareFiles different environment: REFUSED'),
            @('good.json', 'invalid.json', 2, 'CompareFiles candidate runs invalid: exit 2'),
            @('contaminated.json', 'good.json', 3, 'CompareFiles contaminated baseline: REFUSED'),
            @('good.json', 'refused.json', 3, 'CompareFiles a summary that was itself refused: REFUSED')
        )
        foreach ($cs in $cases) {
            $got = (Invoke-Self @('-CompareBaseline', (Join-Path $cmpDir $cs[0]), '-CompareCandidate', (Join-Path $cmpDir $cs[1]))).code
            Check ($got -eq $cs[2]) "$($cs[3]) (exit $got, want $($cs[2]))"
        }

        # --- the same session, the same answer, in every mode (L1) ---
        $child = Invoke-Self @('-Summarize', (Join-Path $fakeRoot 'v_others_buildtool'))
        Check ($child.code -eq 3) "-Summarize of a session with a compiler during a run: exit $($child.code), want 3"
        $child = Invoke-Self @('-Summarize', (Join-Path $fakeRoot 'v_session_format'))
        Check ($child.code -eq 3) "-Summarize of an old-format session: exit $($child.code), want 3"

        # --- a failure of the tool is exit 5, never 1 (L2) ---
        $brokenSd = Join-Path $fakeRoot 'broken_session'
        New-Item -ItemType Directory -Force -Path $brokenSd | Out-Null
        [IO.File]::WriteAllText((Join-Path $brokenSd 'session.json'), '{ "format": "foxsdr-measure-session/2", "runs"')
        $child = Invoke-Self @('-Summarize', $brokenSd)
        Check ($child.code -eq 5) "an unreadable session.json is a TOOL ERROR, exit 5 (exit $($child.code))"
        Check ($child.text -like '*TOOL ERROR*') 'a tool error says so'

        # --- a session that cannot get a quiet machine stops at once with
        #     exit 3, saying so plainly (M3): a busy loop this test starts is
        #     the blocker; the budget is 0 minutes ---
        $stubDir = Join-Path $fakeRoot 'stub-exe'
        New-Item -ItemType Directory -Force -Path $stubDir | Out-Null
        [IO.File]::WriteAllText((Join-Path $stubDir 'cascade.exe'), 'not a program')
        $spin = Start-Process -FilePath 'powershell' -ArgumentList '-NoProfile', '-Command', '$e = (Get-Date).AddSeconds(40); while ((Get-Date) -lt $e) { }' -PassThru -WindowStyle Hidden
        try {
            Start-Sleep -Seconds 2
            $blk = Invoke-Self @('-Baseline', (Join-Path $stubDir 'cascade.exe'), '-Out', (Join-Path $fakeRoot 'blocked-session'), '-Measures', 'cpu', '-Runs', '1', '-WaitForOthersMinutes', '0') 90
            $blkCode = $blk.code
        } finally {
            if (-not $spin.HasExited) { Stop-Process -Id $spin.Id -Force }
        }
        $blkText = $blk.text
        Check ($blkCode -eq 3) "a blocked session exits 3 (exit $blkCode)"
        Check ($blkText -like '*BLOCKED at session start*') 'a blocked session says so at the start'
        Check ($blkText -like "*powershell.exe $($spin.Id)*" -or $blkText -like '*other work*') "a blocked session names its blocker: $(($blkText -split "`n") -like '*BLOCKED*' | Select-Object -First 1)"
        Check (-not (Test-Path (Join-Path $fakeRoot 'blocked-session\identify\baseline\stdout.txt'))) 'a blocked session launched nothing'

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
        if ($null -eq $oth -or [int]$oth -ne 0) { $why += "$($pair[0]): other work (cascade/ctest/build tools) ran during that session ('$oth')" }  # CHECK:cmp-others
        # A summary known to have been measured beside other work, marked so
        # by hand after the fact (the 6342655 baseline): never a reference.
        if ((Get-P $pair[1] 'contaminated' $false) -ne $false) { $why += "$($pair[0]) is marked contaminated: $(Get-P $pair[1] 'contamination' '')" }  # CHECK:cmp-contaminated
        $rfs = @(Get-P $pair[1] 'refusals' @())
        if ($rfs.Count -gt 0) { $why += "$($pair[0]) was refused when summarised: $($rfs -join '; ')" }  # CHECK:cmp-refusals
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
    $noisyRows = @($rows | Where-Object { $_.verdict -like 'NOISY*' }).Count
    if ($AcceptNoisy -and $noisyRows -gt 0) { Write-Host "NOTE: -AcceptNoisy was given: $noisyRows NOISY gate(s) were NOT judged and do not hold the exit code." }
    Write-Host (Get-JudgedText $rows)
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
# ONE wait budget for the whole session, starting now.
$script:SessionDeadline = (Get-Date).AddMinutes($WaitForOthersMinutes)
$startBlocker = Get-Blocker -1
if ($startBlocker -ne '') {
    Write-Host "BLOCKED at session start: $startBlocker"
    Write-Host "    Waiting for it to clear, up to $WaitForOthersMinutes min for the WHOLE session (-WaitForOthersMinutes); if it does not, the session stops with exit 3."
}
$stagedExe = [ordered]@{ baseline = (Copy-Stage (Resolve-Path $Baseline).Path (Join-Path $Out 'stage\baseline')) }
if ($Candidate) { $stagedExe.candidate = (Copy-Stage (Resolve-Path $Candidate).Path (Join-Path $Out 'stage\candidate')) }
$meta = [ordered]@{
    format        = 'foxsdr-measure-session/2'
    started       = (Get-Date).ToString('o')
    environment   = Get-EnvRecord $WindowSize
    # What else was running when the session began (cascade, ctest, build
    # tools, WSL builds). Informational: every run records its own, and a
    # run that saw any is refused.
    otherAtSessionStart = @(Get-OtherNames -1 | Sort-Object -Unique)
    blockedAtStart = $startBlocker
    limits        = $script:Limits
    rateWindow    = $RateWindow
    waitForOthersMinutes = $WaitForOthersMinutes
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
    if ([string]$idInfo.ended -like 'not started*') {
        Write-Host "REFUSED: the session stopped before measuring anything - the machine never became quiet within the $WaitForOthersMinutes min budget. $($idInfo.ended)"
        exit 3
    }
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
            $li = Invoke-Launch -Exe $stagedExe[$label] -Dir $runDir -Measure $m -Setup $knobSet
            if ([string]$li.ended -like 'not started*') {
                # The session's wait budget is spent: stop here, say why, and
                # leave the session marked unfinished (a -Summarize of it is
                # REFUSED for that).
                $meta.aborted = "$label $m run $r $($li.ended)"
                Write-Json $meta (Join-Path $Out 'session.json')
                Write-Host "REFUSED: the session stopped at $label $m run $r - the machine did not become quiet within the $WaitForOthersMinutes min session budget. $($li.ended)"
                exit 3
            }
        }
    }
}

$envEnd = Get-EnvRecord $WindowSize
$meta.environmentAtEnd = $envEnd
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
