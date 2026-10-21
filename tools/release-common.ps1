# Shared helpers for tools\release.ps1 and tools\site-release.ps1. Dot-sourced by
# both; never run on its own.
#
# WHY THESE TWO SCRIPTS SHARE A FILE. Both drive a release one NAMED STEP at a time,
# both print one line per check (PASS / FAIL and the value that decided it), both
# stop at the first FAIL, both have a -DryRun that prints every command instead of
# running it, and both talk to the same two hosts through the same `cat >` pipe. A
# second copy of any of that would be the copy that drifts: the ssh quoting, the
# argument quoting and the "did it really arrive" hash comparison are the parts that
# cost a release an hour the last time they were done by hand.
#
# WHAT RUNS UNDER -DryRun. Nothing that changes a file outside the release folder,
# a remote host or an account. Read-only probes still run (the tree status, the
# tool versions, `ssh <host> true`, `gh auth status`), because a dry run that cannot
# see the machine can only show a plan, not whether the plan would work.
#
# EXECUTING PROGRAMS. Every external program goes through Invoke-Exe, which starts
# it with System.Diagnostics.Process and redirected handles. That is deliberate and
# it is not the same as `& program`:
#   * cascade.exe is becoming a WINDOWS-subsystem program (no console window). From
#     PowerShell `& cascade.exe --version` then returns at once, captures nothing,
#     and leaves $LASTEXITCODE meaningless (measured on a copy of the exe with its
#     subsystem field patched: $LASTEXITCODE was not even set). A process whose
#     handles are redirected is waited for, and its real exit code and output are
#     read, for BOTH subsystems.
#   * Windows PowerShell 5.1 wraps a native program's stderr in ErrorRecords as soon
#     as it is redirected with 2>&1, and under $ErrorActionPreference = "Stop" that
#     THROWS. Reading files has no such trap.
#   * the file piped to `ssh host 'cat > path'` goes in as the raw handle. `Get-Content |`
#     would be text and would corrupt a binary, and so (see Invoke-Exe) would the
#     StreamWriter that is the StandardInput of a Process.
#
# PowerShell 5.1 compatible: no &&, no ternary, no ?. and no ??. ASCII only, because
# 5.1 reads a BOM-less script as the ANSI code page.
#
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

Set-StrictMode -Version 2.0

$script:Ctx = @{
    DryRun  = $false
    LogPath = ''
    Secrets = New-Object System.Collections.ArrayList
}

# ---------------------------------------------------------------------------
# Output: one line per check, mirrored to the step's log
# ---------------------------------------------------------------------------

function Get-Utf8NoBom { return (New-Object System.Text.UTF8Encoding($false)) }

# Anything registered here, and any SAS signature in a URL, is replaced before a
# line reaches the console or a log. The Store credentials never print; the upload
# URL the Store hands back carries a one-time signature that must not either.
function Register-Secret([string]$Value) {
    if (-not [string]::IsNullOrEmpty($Value)) { [void]$script:Ctx.Secrets.Add($Value) }
}

function Hide-Secrets([string]$Text) {
    if ($null -eq $Text) { return '' }
    foreach ($s in $script:Ctx.Secrets) { $Text = $Text.Replace([string]$s, '***') }
    $Text = [regex]::Replace($Text, '(?i)([?&]sig=)[^&\s"]+', '$1REDACTED')
    return $Text
}

function Write-LogLine([string]$Text) {
    if ($script:Ctx.LogPath -eq '') { return }
    $stamp = (Get-Date).ToString('yyyy-MM-dd HH:mm:ss')
    [System.IO.File]::AppendAllText($script:Ctx.LogPath, ($stamp + ' ' + (Hide-Secrets $Text) + "`r`n"), (Get-Utf8NoBom))
}

function Write-Result {
    param([string]$Level, [string]$Name, [string]$Value = '')
    $text = ('{0,-5} {1}' -f $Level, $Name)
    if ($Value -ne '') { $text += ' : ' + $Value }
    $text = Hide-Secrets $text
    $color = 'Gray'
    switch ($Level) {
        'PASS'   { $color = 'Green' }
        'FAIL'   { $color = 'Red' }
        'WARN'   { $color = 'Yellow' }
        'DRY'    { $color = 'Cyan' }
        'SKIP'   { $color = 'DarkGray' }
        'STOP'   { $color = 'Magenta' }
        'ACTION' { $color = 'Magenta' }
    }
    Write-Host $text -ForegroundColor $color
    Write-LogLine $text
}

function Write-Detail([string]$Text) {
    $t = Hide-Secrets $Text
    Write-Host ('      ' + $t)
    Write-LogLine ('      ' + $t)
}

function Stop-Release([string]$Message) {
    Write-Result 'FAIL' $Message
    throw ('RELEASE-FAIL: ' + $Message)
}

# One check: PASS prints the deciding value, FAIL prints it and stops the run.
function Assert-Check {
    param([bool]$Ok, [string]$Name, [string]$Value = '', [string]$Why = '')
    if ($Ok) { Write-Result 'PASS' $Name $Value; return }
    Write-Result 'FAIL' $Name $Value
    if ($Why -ne '') { Write-Detail $Why }
    throw ('RELEASE-FAIL: ' + $Name)
}

function Start-StepLog([string]$LogDir, [string]$Step) {
    if (-not (Test-Path -LiteralPath $LogDir)) { New-Item -ItemType Directory -Path $LogDir -Force | Out-Null }
    $script:Ctx.LogPath = Join-Path $LogDir ($Step + '.log')
    $mode = 'run'
    if ($script:Ctx.DryRun) { $mode = 'DRY RUN' }
    Write-LogLine ('===== step ' + $Step + ' (' + $mode + ')')
}

# ---------------------------------------------------------------------------
# External programs
# ---------------------------------------------------------------------------

# Quotes one argument the way CommandLineToArgvW will take it apart again:
# backslashes only matter in front of a double quote, where each one is doubled.
function ConvertTo-QuotedArg([string]$Arg) {
    if ($Arg.Length -eq 0) { return '""' }
    if ($Arg -notmatch '[\s"]') { return $Arg }
    $sb = New-Object System.Text.StringBuilder
    [void]$sb.Append([char]34)
    $backslashes = 0
    foreach ($ch in $Arg.ToCharArray()) {
        if ($ch -eq [char]92) { $backslashes++; continue }
        if ($ch -eq [char]34) {
            [void]$sb.Append([char]92, ($backslashes * 2 + 1))
            [void]$sb.Append([char]34)
            $backslashes = 0
            continue
        }
        if ($backslashes -gt 0) { [void]$sb.Append([char]92, $backslashes); $backslashes = 0 }
        [void]$sb.Append($ch)
    }
    if ($backslashes -gt 0) { [void]$sb.Append([char]92, ($backslashes * 2)) }
    [void]$sb.Append([char]34)
    return $sb.ToString()
}

function Format-CommandLine([string]$File, [string[]]$Arguments) {
    $parts = @((ConvertTo-QuotedArg $File))
    foreach ($a in $Arguments) { $parts += (ConvertTo-QuotedArg $a) }
    return ($parts -join ' ')
}

# Runs a program to completion and returns ExitCode, Output (stdout then stderr),
# StdOut, StdErr, TimedOut. Never throws for a non-zero exit: the caller decides.
# On a timeout the CHILD this function started is killed, nothing else.
#
# HOW THE PIPES ARE DONE, and why it is Start-Process with FILES rather than
# System.Diagnostics.Process with redirected streams: the stdin of a Process is a
# StreamWriter, and in Windows PowerShell 5.1's .NET Framework it writes its encoding's
# preamble as soon as it is created, so with a UTF-8 console code page every byte stream
# sent to a host begins with a UTF-8 BOM. Found by the rehearsal's fake-ssh test: each
# file streamed to a host arrived three bytes longer and the sha256 read back on the far
# side did not match. A file given to Start-Process -RedirectStandardInput is handed to
# the child as the handle itself, untouched; stdout and stderr go to files the same way
# (Start-Process cannot send both to one file, so the order between the two is lost).
# Environment overrides are set on this process around the call and put back after.
function Invoke-Exe {
    param(
        [Parameter(Mandatory = $true)][string]$File,
        [string[]]$Arguments = @(),
        [hashtable]$Env = @{},
        [string]$WorkDir = '',
        [int]$TimeoutSec = 600,
        [string]$InputFile = '',
        [string]$Label = '',
        [int]$HeartbeatSec = 60,
        [switch]$NoLog
    )
    $cmdline = Format-CommandLine $File $Arguments
    Write-LogLine ('$ ' + $cmdline)
    $argString = ((@($Arguments | ForEach-Object { ConvertTo-QuotedArg $_ })) -join ' ')
    $tmp = [System.IO.Path]::GetTempPath()
    $stamp = [System.Guid]::NewGuid().ToString('N')
    $soPath = Join-Path $tmp ('foxsdr-rel-' + $stamp + '.out')
    $sePath = Join-Path $tmp ('foxsdr-rel-' + $stamp + '.err')
    $inPath = $InputFile
    $ownIn = $false
    if ($inPath -eq '') {
        # An empty file, so the child sees end-of-input at once (what `ssh -n` is for).
        $inPath = Join-Path $tmp ('foxsdr-rel-' + $stamp + '.in')
        [System.IO.File]::WriteAllBytes($inPath, (New-Object byte[] 0))
        $ownIn = $true
    }
    $saved = @{}
    foreach ($k in @($Env.Keys)) {
        $saved[$k] = [System.Environment]::GetEnvironmentVariable($k, 'Process')
        if ($null -eq $Env[$k]) { [System.Environment]::SetEnvironmentVariable($k, $null, 'Process') }
        else { [System.Environment]::SetEnvironmentVariable($k, [string]$Env[$k], 'Process') }
    }
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $p = $null
    $timedOut = $false
    $so = ''
    $se = ''
    $code = -1
    try {
        $spArgs = @{
            FilePath = $File
            RedirectStandardInput = $inPath
            RedirectStandardOutput = $soPath
            RedirectStandardError = $sePath
            NoNewWindow = $true
            PassThru = $true
        }
        if ($argString -ne '') { $spArgs['ArgumentList'] = $argString }
        if ($WorkDir -ne '') { $spArgs['WorkingDirectory'] = $WorkDir }
        try {
            $p = Start-Process @spArgs
        } catch {
            $msg = 'could not start ' + $File + ': ' + $_.Exception.Message
            Write-LogLine $msg
            return [pscustomobject]@{ ExitCode = -1; Output = $msg; StdOut = ''; StdErr = $msg; TimedOut = $false; Seconds = 0.0; Started = $false }
        }
        # Touching Handle keeps the exit code readable after the process has gone (a
        # Windows PowerShell 5.1 quirk of Start-Process -PassThru).
        [void]$p.Handle
        $slice = [Math]::Max(1, $HeartbeatSec) * 1000
        while (-not $p.WaitForExit($slice)) {
            if ($sw.Elapsed.TotalSeconds -ge $TimeoutSec) {
                try { $p.Kill() } catch { }
                $timedOut = $true
                [void]$p.WaitForExit(15000)
                break
            }
            if ($Label -ne '') { Write-Result 'INFO' ($Label + ' still running') ('{0:N0} s' -f $sw.Elapsed.TotalSeconds) }
        }
        try { $p.WaitForExit() } catch { }
        if ($p.HasExited) { $code = $p.ExitCode }
        $p.Dispose()
    } finally {
        foreach ($k in @($saved.Keys)) { [System.Environment]::SetEnvironmentVariable($k, $saved[$k], 'Process') }
    }
    if (Test-Path -LiteralPath $soPath) { $so = [System.IO.File]::ReadAllText($soPath, [System.Text.Encoding]::UTF8); Remove-Item -LiteralPath $soPath -Force -ErrorAction SilentlyContinue }
    if (Test-Path -LiteralPath $sePath) { $se = [System.IO.File]::ReadAllText($sePath, [System.Text.Encoding]::UTF8); Remove-Item -LiteralPath $sePath -Force -ErrorAction SilentlyContinue }
    if ($ownIn) { Remove-Item -LiteralPath $inPath -Force -ErrorAction SilentlyContinue }
    $all = $so
    if ($se -ne '') { if ($all -ne '') { $all += "`n" }; $all += $se }
    if (-not $NoLog) {
        $shown = $all
        if ($shown.Length -gt 200000) { $shown = $shown.Substring(0, 200000) + "`n[output truncated in the log]" }
        if ($shown.Trim() -ne '') { Write-LogLine ("output:`r`n" + $shown) }
    }
    Write-LogLine ('exit ' + $code + ' after ' + ('{0:N1}' -f $sw.Elapsed.TotalSeconds) + ' s')
    return [pscustomobject]@{ ExitCode = $code; Output = $all; StdOut = $so; StdErr = $se; TimedOut = $timedOut; Seconds = $sw.Elapsed.TotalSeconds; Started = $true }
}

# One command that changes something (or takes long enough to be worth showing).
# Under -DryRun it prints the exact command line and returns $null, so a caller
# written as `$r = Invoke-Planned ...; if ($null -eq $r) { skip its checks }` has
# one code path for the plan and for the run.
function Invoke-Planned {
    param(
        [Parameter(Mandatory = $true)][string]$What,
        [Parameter(Mandatory = $true)][string]$File,
        [string[]]$Arguments = @(),
        [hashtable]$Env = @{},
        [string]$WorkDir = '',
        [int]$TimeoutSec = 600,
        [string]$InputFile = '',
        [switch]$ReadOnly,
        [int]$HeartbeatSec = 60
    )
    if ($script:Ctx.DryRun -and -not $ReadOnly) {
        Write-Result 'DRY' $What (Format-CommandLine $File $Arguments)
        if ($InputFile -ne '') { Write-Detail ('(standard input: ' + $InputFile + ')') }
        return $null
    }
    return (Invoke-Exe -File $File -Arguments $Arguments -Env $Env -WorkDir $WorkDir -TimeoutSec $TimeoutSec -InputFile $InputFile -Label $What -HeartbeatSec $HeartbeatSec)
}

# A planned action with no external program (a file copy, a state write).
function Write-Plan([string]$What, [string]$Detail = '') {
    Write-Result 'DRY' $What $Detail
}

# ---------------------------------------------------------------------------
# Files and hashes
# ---------------------------------------------------------------------------

function Get-Sha256([string]$Path) {
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Get-FileInfoLine([string]$Path) {
    $i = Get-Item -LiteralPath $Path
    return ('{0:N0} bytes, sha256 {1}' -f $i.Length, (Get-Sha256 $Path))
}

# The Subsystem field of the PE optional header: 2 is Windows (no console), 3 is
# Console. It sits at the same offset (+68) in PE32 and PE32+, so one read serves.
function Get-PeSubsystem([string]$Path) {
    $fs = [System.IO.File]::OpenRead($Path)
    try {
        $br = New-Object System.IO.BinaryReader($fs)
        $fs.Seek(0x3C, [System.IO.SeekOrigin]::Begin) | Out-Null
        $pe = $br.ReadInt32()
        $fs.Seek($pe + 4 + 20 + 68, [System.IO.SeekOrigin]::Begin) | Out-Null
        $v = $br.ReadUInt16()
        if ($v -eq 2) { return 'windows' }
        if ($v -eq 3) { return 'console' }
        return ('other(' + $v + ')')
    } finally {
        $fs.Dispose()
    }
}

# A JSON file read into nested hashtables (ConvertFrom-Json gives PSCustomObjects,
# which cannot be extended in place, and the state file is).
function ConvertTo-Hashtable($Object) {
    if ($null -eq $Object) { return $null }
    if ($Object -is [System.Management.Automation.PSCustomObject]) {
        $h = @{}
        foreach ($prop in $Object.PSObject.Properties) { $h[$prop.Name] = (ConvertTo-Hashtable $prop.Value) }
        return $h
    }
    if ($Object -is [System.Array]) {
        $list = @()
        foreach ($item in $Object) { $list += , (ConvertTo-Hashtable $item) }
        return , $list
    }
    return $Object
}

function Read-JsonFile([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path)) { return $null }
    $text = [System.IO.File]::ReadAllText($Path, [System.Text.Encoding]::UTF8)
    if ($text.Trim() -eq '') { return $null }
    return (ConvertTo-Hashtable ($text | ConvertFrom-Json))
}

function Write-JsonFile([string]$Path, $Object) {
    $json = $Object | ConvertTo-Json -Depth 8
    [System.IO.File]::WriteAllText($Path, $json + "`r`n", (Get-Utf8NoBom))
}

# ---------------------------------------------------------------------------
# Tools on this desktop
# ---------------------------------------------------------------------------

function Find-Tool([string]$Name, [string[]]$Candidates) {
    $cmd = Get-Command $Name -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($null -ne $cmd) { return $cmd.Source }
    foreach ($c in $Candidates) {
        $hit = @(Get-ChildItem -Path $c -ErrorAction SilentlyContinue | Select-Object -First 1)
        if ($hit.Count -gt 0) { return $hit[0].FullName }
    }
    return ''
}

function Find-Gh {
    return (Find-Tool 'gh' @((Join-Path $env:LOCALAPPDATA 'Microsoft\WinGet\Packages\GitHub.cli_*\bin\gh.exe'), 'C:\Program Files\GitHub CLI\gh.exe'))
}

function Find-Iscc {
    return (Find-Tool 'ISCC' @((Join-Path $env:LOCALAPPDATA 'Programs\Inno Setup 6\ISCC.exe'), 'C:\Program Files (x86)\Inno Setup 6\ISCC.exe'))
}

function Find-Go {
    return (Find-Tool 'go' @('C:\Program Files\Go\bin\go.exe'))
}

function Find-Ssh {
    return (Find-Tool 'ssh' @('C:\Windows\System32\OpenSSH\ssh.exe'))
}

# Hosts are kept OUT of the repository: this tree is public, and .gitignore already
# says why session notes with hosts and network layout never ship in it. They come
# from -PiHost / -NasHost, then the FOXSDR_PI_HOST environment variable, then a
# machine-local JSON file beside the release folders. Only the NAS has a default,
# and that is the ssh alias tools\build-nightly.ps1 already names.
function Get-ReleaseConfig([string]$Path) {
    $cfg = @{
        piHost = ''
        nasHost = 'nas'
        nasSymbolRoot = '/volume1/foxsdr-symbols'
        piSymbolDir = '/var/lib/foxsdr-site/symbols'
        piSiteRoot = '/opt/foxsdr-site'
        repo = 'wonderingStars/foxsdr'
    }
    $file = Read-JsonFile $Path
    if ($null -ne $file) {
        foreach ($k in @($file.Keys)) { $cfg[$k] = $file[$k] }
    }
    if ($cfg['piHost'] -eq '' -and $env:FOXSDR_PI_HOST) { $cfg['piHost'] = $env:FOXSDR_PI_HOST }
    return $cfg
}

# ---------------------------------------------------------------------------
# Remote hosts: ssh only (the Pi has no scp or sftp, the NAS needs scp -O)
# ---------------------------------------------------------------------------

function ConvertTo-RemoteQuoted([string]$Text) {
    return ("'" + $Text.Replace("'", "'\''") + "'")
}

# stdin is closed at once unless a file is being streamed (what `ssh -n` is for:
# an ssh that is not itself reading a file must not eat somebody else's input).
function Invoke-Ssh {
    param(
        [Parameter(Mandatory = $true)][string]$HostName,
        [Parameter(Mandatory = $true)][string]$Command,
        [int]$TimeoutSec = 120,
        [string]$InputFile = '',
        [string]$Label = ''
    )
    $ssh = Find-Ssh
    if ($ssh -eq '') { Stop-Release 'ssh is not installed' }
    return (Invoke-Exe -File $ssh -Arguments @('-o', 'BatchMode=yes', '-o', 'ConnectTimeout=15', $HostName, $Command) -TimeoutSec $TimeoutSec -InputFile $InputFile -Label $Label)
}

function Get-RemoteSha256 {
    param([string]$HostName, [string]$Path)
    $q = ConvertTo-RemoteQuoted $Path
    $r = Invoke-Ssh -HostName $HostName -Command ("sha256sum " + $q + " 2>/dev/null || shasum -a 256 " + $q + " 2>/dev/null || openssl dgst -sha256 " + $q + " 2>/dev/null")
    if ($r.ExitCode -ne 0) { return '' }
    $m = [regex]::Match($r.StdOut, '[0-9a-f]{64}')
    if ($m.Success) { return $m.Value }
    return ''
}

function Test-RemoteFile {
    param([string]$HostName, [string]$Path)
    $r = Invoke-Ssh -HostName $HostName -Command ('test -f ' + (ConvertTo-RemoteQuoted $Path) + ' && echo present || echo absent')
    return ($r.ExitCode -eq 0 -and $r.StdOut.Trim() -eq 'present')
}

# The far side's copy of a file compared with ours. sha256 when the host has a tool for it
# (sha256sum, shasum or openssl); when it has none, the byte count, and the run says so once
# per host - a size match is weaker than a hash match and is not presented as one. The Pi
# has sha256sum; the NAS's DSM has not been checked for one.
$script:SizeOnlyWarned = @{}
function Compare-RemoteFile {
    param([string]$HostName, [string]$Path, [string]$Sha256, [int64]$Size)
    $h = Get-RemoteSha256 -HostName $HostName -Path $Path
    if ($h -ne '') { return @{ Exists = $true; Match = ($h -eq $Sha256); Kind = 'sha256'; Remote = $h } }
    $r = Invoke-Ssh -HostName $HostName -Command ('stat -c %s ' + (ConvertTo-RemoteQuoted $Path) + ' 2>/dev/null')
    $n = [int64]0
    if ($r.ExitCode -eq 0 -and [int64]::TryParse($r.StdOut.Trim(), [ref]$n)) {
        if (-not $script:SizeOnlyWarned.ContainsKey($HostName)) {
            $script:SizeOnlyWarned[$HostName] = $true
            Write-Result 'WARN' ('no sha256 tool on ' + $HostName) 'files there are verified by byte count only (sha256sum, shasum and openssl all failed)'
        }
        return @{ Exists = $true; Match = ($n -eq $Size); Kind = 'size'; Remote = [string]$n }
    }
    return @{ Exists = $false; Match = $false; Kind = ''; Remote = '' }
}

# Streams a file to <RemotePath> through `cat >` into a .part name, compares what the far
# side holds with the local file (Compare-RemoteFile), and only then renames it into place.
# A file that is already there and matches is left alone; a file there that does NOT match
# stops the run (a symbol, a download or an index row is never silently replaced).
function Send-RemoteFile {
    param(
        [Parameter(Mandatory = $true)][string]$HostName,
        [Parameter(Mandatory = $true)][string]$LocalPath,
        [Parameter(Mandatory = $true)][string]$RemotePath,
        [string]$Mode = '',
        [string]$Owner = '',
        [switch]$MakeDirectory,
        [switch]$ReplaceExisting
    )
    $local = Get-Sha256 $LocalPath
    $size = (Get-Item -LiteralPath $LocalPath).Length
    # The last two path parts, so two files called symmap.json.gz can be told apart.
    $pieces = @($RemotePath.Split('/') | Where-Object { $_ -ne '' })
    $name = $pieces[$pieces.Count - 1]
    if ($pieces.Count -ge 2) { $name = $pieces[$pieces.Count - 2] + '/' + $name }
    $dir = $RemotePath.Substring(0, $RemotePath.LastIndexOf('/'))
    if ($script:Ctx.DryRun) {
        Write-Result 'DRY' ('send ' + $name + ' to ' + $HostName) ('ssh ' + $HostName + " 'cat > " + $RemotePath + ".part' < " + $LocalPath + ' ; sha256sum ; mv')
        return $local
    }
    $there = Compare-RemoteFile -HostName $HostName -Path $RemotePath -Sha256 $local -Size $size
    if ($there.Exists) {
        if ($there.Match) {
            Write-Result 'SKIP' ($name + ' on ' + $HostName) ('already there with the same ' + $there.Kind)
            return $local
        }
        if (-not $ReplaceExisting) {
            Stop-Release ($RemotePath + ' on ' + $HostName + ' already exists with a different ' + $there.Kind + ' (' + $there.Remote + ', local ' + $local + ' / ' + $size + ' bytes); it is never replaced silently')
        }
    }
    if ($MakeDirectory) {
        $mk = Invoke-Ssh -HostName $HostName -Command ('mkdir -p ' + (ConvertTo-RemoteQuoted $dir))
        if ($mk.ExitCode -ne 0) { Stop-Release ('mkdir -p ' + $dir + ' on ' + $HostName + ' failed: ' + $mk.Output.Trim()) }
    }
    $part = $RemotePath + '.part'
    $up = Invoke-Ssh -HostName $HostName -Command ('cat > ' + (ConvertTo-RemoteQuoted $part)) -InputFile $LocalPath -TimeoutSec 1800 -Label ('upload ' + $name)
    if ($up.ExitCode -ne 0) { Stop-Release ('upload of ' + $name + ' to ' + $HostName + ' failed (exit ' + $up.ExitCode + '): ' + $up.Output.Trim()) }
    $got = Compare-RemoteFile -HostName $HostName -Path $part -Sha256 $local -Size $size
    if (-not $got.Match) {
        [void](Invoke-Ssh -HostName $HostName -Command ('rm -f ' + (ConvertTo-RemoteQuoted $part)))
        Stop-Release ($got.Kind + ' of ' + $name + ' on ' + $HostName + ' is "' + $got.Remote + '", local is ' + $local + ' / ' + $size + ' bytes (the partial file was removed)')
    }
    $finish = ''
    if ($Owner -ne '') { $finish += 'chown ' + $Owner + ' ' + (ConvertTo-RemoteQuoted $part) + ' && ' }
    if ($Mode -ne '') { $finish += 'chmod ' + $Mode + ' ' + (ConvertTo-RemoteQuoted $part) + ' && ' }
    $finish += 'mv -f ' + (ConvertTo-RemoteQuoted $part) + ' ' + (ConvertTo-RemoteQuoted $RemotePath)
    $fin = Invoke-Ssh -HostName $HostName -Command $finish
    if ($fin.ExitCode -ne 0) { Stop-Release ('could not put ' + $name + ' in place on ' + $HostName + ': ' + $fin.Output.Trim()) }
    $final = Compare-RemoteFile -HostName $HostName -Path $RemotePath -Sha256 $local -Size $size
    Assert-Check $final.Match ($name + ' on ' + $HostName) ('{0:N0} bytes, {1} {2}' -f $size, $final.Kind, $final.Remote) ('after the rename the far side holds "' + $final.Remote + '" (' + $final.Kind + '), local is ' + $local + ' / ' + $size + ' bytes')
    return $local
}

# ---------------------------------------------------------------------------
# HTTP (Windows PowerShell 5.1 has no -SkipHttpErrorCheck and decodes a body with no
# charset as Latin-1, so System.Net.Http is used directly: a non-2xx answer is returned
# for the caller to read, and the body is decoded as UTF-8 from its bytes)
# ---------------------------------------------------------------------------

function Initialize-Http {
    Add-Type -AssemblyName System.Net.Http
    [System.Net.ServicePointManager]::SecurityProtocol = [System.Net.SecurityProtocolType]::Tls12
}

# One HTTP call. Does NOT throw for a non-2xx answer: the Store's 409 and 400 are
# answers the caller has to read. -OutFile streams the body to a file instead of
# holding it as text (the downloads are tens of megabytes).
function Invoke-Http {
    param(
        [string]$Method, [string]$Url, [hashtable]$Headers = @{}, [byte[]]$Body = $null,
        [string]$ContentType = '', [string]$InFile = '', [string]$OutFile = '', [int]$TimeoutSec = 120
    )
    Initialize-Http
    $client = New-Object System.Net.Http.HttpClient
    $client.Timeout = [TimeSpan]::FromSeconds($TimeoutSec)
    $req = New-Object System.Net.Http.HttpRequestMessage((New-Object System.Net.Http.HttpMethod($Method)), $Url)
    foreach ($k in $Headers.Keys) { [void]$req.Headers.TryAddWithoutValidation($k, [string]$Headers[$k]) }
    $fs = $null
    $out = $null
    try {
        if ($InFile -ne '') {
            $fs = [System.IO.File]::OpenRead($InFile)
            $req.Content = New-Object System.Net.Http.StreamContent($fs)
            if ($ContentType -ne '') { [void]$req.Content.Headers.TryAddWithoutValidation('Content-Type', $ContentType) }
        } elseif ($null -ne $Body) {
            $req.Content = New-Object System.Net.Http.ByteArrayContent(, $Body)
            if ($ContentType -ne '') { [void]$req.Content.Headers.TryAddWithoutValidation('Content-Type', $ContentType) }
        }
        $resp = $client.SendAsync($req, [System.Net.Http.HttpCompletionOption]::ResponseHeadersRead).Result
        $text = ''
        $length = 0
        if ($OutFile -ne '' -and [int]$resp.StatusCode -ge 200 -and [int]$resp.StatusCode -lt 300) {
            $out = [System.IO.File]::Create($OutFile)
            $resp.Content.CopyToAsync($out).Wait()
            $length = $out.Length
        } else {
            $bytes = $resp.Content.ReadAsByteArrayAsync().Result
            $length = $bytes.Length
            $text = [System.Text.Encoding]::UTF8.GetString($bytes)
        }
        $corr = ''
        $vals = $null
        if ($resp.Headers.TryGetValues('MS-CorrelationId', [ref]$vals)) { $corr = ($vals -join ',') }
        $cache = ''
        $vals2 = $null
        if ($resp.Headers.TryGetValues('CF-Cache-Status', [ref]$vals2)) { $cache = ($vals2 -join ',') }
        return [pscustomobject]@{ Status = [int]$resp.StatusCode; Text = $text; Length = $length; Correlation = $corr; CacheStatus = $cache }
    } finally {
        if ($null -ne $out) { $out.Dispose() }
        if ($null -ne $fs) { $fs.Dispose() }
        $client.Dispose()
    }
}
