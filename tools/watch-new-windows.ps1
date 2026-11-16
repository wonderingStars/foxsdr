# watch-new-windows.ps1 - runs a command on a HIDDEN desktop and reports every top-level window that appears on the hidden desktop OR on the
# visible desktop while it runs (engine lane E1, owner decision 17: "no test may raise a Windows prompt").
#
# A Windows Security prompt (a smart-card or a credential dialog) is a top-level window. This script takes a snapshot of the top-level windows of both desktops before the
# command starts, samples both every 50 ms until it ends and once more after, and prints each window that was not in the first snapshot: its desktop, class, title and
# process id. Exit code: the command's own code, or 90 when a new window appeared on the VISIBLE desktop (the case the owner must never see). Windows on the hidden
# desktop are listed but do not change the code (a test window of the program under test is expected there); read the list for a prompt's class (#32770, CredentialUIBroker).
#
# Usage: powershell -NoProfile -ExecutionPolicy Bypass -File tools/watch-new-windows.ps1 -CommandLine "<program and arguments>" [-Desktop FoxSDRE1Tests] [-Log <file>]
#
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
param(
    [Parameter(Mandatory = $true)][string]$CommandLine,
    [string]$Desktop = 'FoxSDRE1Tests',
    [string]$Log = ''
)

Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
public static class Watch {
    delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)] static extern IntPtr CreateDesktop(string n, IntPtr dev, IntPtr dm, int fl, uint acc, IntPtr sa);
    [DllImport("user32.dll", SetLastError = true)] static extern bool EnumDesktopWindows(IntPtr desk, EnumProc p, IntPtr l);
    [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc p, IntPtr l);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetClassName(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr h);
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    public struct STARTUPINFO {
        public int cb; public string lpReserved; public string lpDesktop; public string lpTitle;
        public int dwX, dwY, dwXSize, dwYSize, dwXCountChars, dwYCountChars, dwFillAttribute, dwFlags;
        public short wShowWindow, cbReserved2; public IntPtr lpReserved2, hStdInput, hStdOutput, hStdError;
    }
    [StructLayout(LayoutKind.Sequential)] public struct PROCESS_INFORMATION { public IntPtr hProcess, hThread; public int dwProcessId, dwThreadId; }
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern bool CreateProcess(string app, string cmd, IntPtr pa, IntPtr ta, bool inh, uint fl, IntPtr env, string dir, ref STARTUPINFO si, out PROCESS_INFORMATION pi);
    [DllImport("kernel32.dll")] static extern uint WaitForSingleObject(IntPtr h, uint ms);
    [DllImport("kernel32.dll")] static extern bool GetExitCodeProcess(IntPtr h, out uint code);

    public static IntPtr Open(string name) { return CreateDesktop(name, IntPtr.Zero, IntPtr.Zero, 0, 0x000F01FF, IntPtr.Zero); }

    public static List<string> Snapshot(IntPtr desk, string label) {
        List<string> list = new List<string>();
        EnumProc p = delegate (IntPtr h, IntPtr l) {
            StringBuilder t = new StringBuilder(256), c = new StringBuilder(256);
            GetWindowText(h, t, 256); GetClassName(h, c, 256);
            uint pid; GetWindowThreadProcessId(h, out pid);
            list.Add(label + "|" + h.ToInt64().ToString("x") + "|" + c + "|" + t + "|" + pid + "|" + (IsWindowVisible(h) ? "visible" : "hidden"));
            return true;
        };
        if (desk == IntPtr.Zero) { EnumWindows(p, IntPtr.Zero); } else { EnumDesktopWindows(desk, p, IntPtr.Zero); }
        return list;
    }

    public static IntPtr Start(string cmdline, string desktop, out int pid) {
        STARTUPINFO si = new STARTUPINFO();
        si.cb = Marshal.SizeOf(typeof(STARTUPINFO));
        si.lpDesktop = desktop;
        PROCESS_INFORMATION pi;
        if (!CreateProcess(null, "cmd.exe /c " + cmdline, IntPtr.Zero, IntPtr.Zero, false, 0x08000000, IntPtr.Zero, null, ref si, out pi)) { throw new Exception("CreateProcess failed: " + Marshal.GetLastWin32Error()); }
        pid = pi.dwProcessId;
        return pi.hProcess;
    }
    public static bool Done(IntPtr h, out uint code) { code = 0; if (WaitForSingleObject(h, 0) == 0) { GetExitCodeProcess(h, out code); return true; } return false; }
}
'@

function Tree([int]$root) {
    $all = Get-CimInstance Win32_Process | Select-Object ProcessId, ParentProcessId
    $set = @{}
    $set[$root] = $true
    $grew = $true
    while ($grew) {
        $grew = $false
        foreach ($p in $all) { if ($set.ContainsKey([int]$p.ParentProcessId) -and -not $set.ContainsKey([int]$p.ProcessId)) { $set[[int]$p.ProcessId] = $true; $grew = $true } }
    }
    return $set
}
$hDesk = [Watch]::Open($Desktop)
if ($hDesk -eq [IntPtr]::Zero) { throw "CreateDesktop failed" }
# A window is identified by class, title and process (handles are reused); the first snapshot is what is "already there".
function Key([string]$s) { $p = $s.Split('|'); return "$($p[0])|$($p[2])|$($p[3])|$($p[4])" }
$before = @{}
foreach ($s in ([Watch]::Snapshot([IntPtr]::Zero, 'visible-desktop') + [Watch]::Snapshot($hDesk, 'hidden-desktop'))) { $before[(Key $s)] = $true }
$seen = @{}
$ours = @{}
$childPid = 0
$h = [Watch]::Start($CommandLine, $Desktop, [ref]$childPid)
$code = [uint32]0
$samples = 0
while ($true) {
    foreach ($s in ([Watch]::Snapshot([IntPtr]::Zero, 'visible-desktop') + [Watch]::Snapshot($hDesk, 'hidden-desktop'))) {
        $k = Key $s
        if (-not $before.ContainsKey($k) -and -not $seen.ContainsKey($k)) { $seen[$k] = $s }
    }
    $samples++
    if (($samples % 4) -eq 1) { foreach ($k in (Tree $childPid).Keys) { $ours[$k] = $true } }
    if ([Watch]::Done($h, [ref]$code)) { break }
    Start-Sleep -Milliseconds 50
}
foreach ($s in ([Watch]::Snapshot([IntPtr]::Zero, 'visible-desktop') + [Watch]::Snapshot($hDesk, 'hidden-desktop'))) {
    $k = Key $s
    if (-not $before.ContainsKey($k) -and -not $seen.ContainsKey($k)) { $seen[$k] = $s }
}
$visibleAll = @($seen.Values | Where-Object { $_.StartsWith('visible-desktop') })
$visible = @($visibleAll | Where-Object { $ours.ContainsKey([int]($_.Split('|')[4])) })
$visibleOthers = @($visibleAll | Where-Object { -not $ours.ContainsKey([int]($_.Split('|')[4])) })
$hidden = @($seen.Values | Where-Object { $_.StartsWith('hidden-desktop') })
$baseVisible = @($before.Keys | Where-Object { $_.StartsWith('visible-desktop') }).Count
$baseHidden = @($before.Keys | Where-Object { $_.StartsWith('hidden-desktop') }).Count
$lines = @("watch-new-windows: before the command the visible desktop had $baseVisible windows and the hidden one $baseHidden (the enumeration works)",
           "watch-new-windows: command ended with exit code $code after $samples samples of both desktops",
           "new windows on the VISIBLE desktop made by the command or anything it started: $($visible.Count) (others on the visible desktop, made by other programs meanwhile: $($visibleOthers.Count))",
           "new windows on the hidden desktop '$Desktop': $($hidden.Count)")
foreach ($s in $visible) { $lines += "  VISIBLE  $s" }
foreach ($s in $visibleOthers) { $lines += "  other    $s" }
foreach ($s in $hidden) { $lines += "  hidden   $s" }
$lines | ForEach-Object { Write-Output $_ }
if ($Log -ne '') { $lines | Set-Content -Path $Log -Encoding UTF8 }
if ($visible.Count -gt 0) { exit 90 }
if ($hidden.Count -gt 0 -and $code -eq 0) { exit 0 }
exit [int]$code
