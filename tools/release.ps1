# The Windows side of a FoxSDR release, one NAMED STEP at a time, from a clean
# worktree at the release commit.
#
#   powershell -ExecutionPolicy Bypass -File tools\release.ps1 -DryRun
#   powershell -ExecutionPolicy Bypass -File tools\release.ps1 -Steps preflight,build,installer,msix,install-test
#   powershell -ExecutionPolicy Bypass -File tools\release.ps1 -From ci
#   powershell -ExecutionPolicy Bypass -File tools\release.ps1 -Steps store -SubmitStore
#
# The steps, in order, and what each one CHECKS (docs\RELEASING.md has the long form):
#
#   preflight     the tree is clean, HEAD is tagged or taggable, CMakeLists.txt, README.md
#                 and docs\release-notes-<v>.md agree on the version, the notes carry
#                 five TBD hash lines or five real ones, every tool is present and the
#                 two hosts answer, and a changed telemetry Worker has been deployed.
#   build         configure and build Release twice, hash cascade.exe, run --version,
#                 archive the symbols for exactly that binary.
#   installer     ISCC, then the installer's name, size and hash.
#   msix          build-msix.ps1 with the Store identity, then the package's own
#                 manifest (name, publisher, version) and the exe inside it.
#   install-test  silent per-user install, the installed exe against the built one,
#                 --version, --selftest, a bounded --frames run, silent uninstall, and
#                 the install directory gone afterwards.
#   ci            find the GitHub Actions run for HEAD, wait for it, download its
#                 artifacts, check every file, write the five hashes into the notes.
#   symbols       mirror the Windows and Linux symbols to the NAS, append the index
#                 rows, put the symbol maps on the Pi, verify every hash remotely.
#   github        tag, draft release, compare GitHub's digests, publish as latest.
#   store         Microsoft Store submission through the Partner Center submission API.
#   store-status  which version the public Store catalogue is serving, and whether the
#                 site's StoreAppVersion is stale.
#
# Every step is IDEMPOTENT: it checks its own outputs (the files it wrote, by hash)
# and says SKIP when they are all there for this commit. -Force runs it again. The
# state lives in <ReleaseRoot>\release-<v>\state.json beside dist\, msix\, ci\,
# verify\ and log\<step>.log.
#
# -DryRun prints every command and remote action instead of running it. The only things
# that still run are read-only: git and file reads, tool versions, `ssh <host> true`,
# `gh auth status`, and the public Store catalogue query in store-status.
#
# EXIT CODES. 0 every selected step passed. 1 a check FAILED (the run stops there).
# 2 something only the owner can do is next (the Store credentials, or -SubmitStore).
#
# NO Get-Content / Set-Content ON ANY SOURCE FILE HERE. The one file this script edits
# in the tree (the five hash lines of the release notes) is read and written through
# [System.IO.File] with an explicit encoding and its line endings checked afterwards.
#
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

[CmdletBinding()]
param(
    # Named steps to run (comma separated), or -From to run one step and everything after.
    [string[]] $Steps = @(),
    [string] $From = '',
    [switch] $DryRun,
    # Run the selected steps again even though their outputs are already there.
    [switch] $Force,
    # Rehearsal only: a modified tree stamps "-dirty" into the binary, so a real release
    # never passes this.
    [switch] $AllowDirty,
    # Said by the owner after deploying telemetry-worker\ (see preflight).
    [switch] $WorkerDeployed,
    # Without this the store step prepares everything and stops before the first write.
    [switch] $SubmitStore,
    # The Store keeps one in-progress submission per app. By default an existing one
    # stops the step (it may be the owner's draft); this deletes it first.
    [switch] $ReplacePendingStoreSubmission,
    # The installer silently removes a FoxSDR installed in the other scope. By default
    # the install test refuses to run over an existing install.
    [switch] $AllowReplaceInstalled,
    [switch] $KeepInstalled,

    [string] $ReleaseRoot = 'C:\Users\steve\foxsdr-build',
    [string] $BuildDir = '',
    [string] $CiRunId = '',
    [int] $CiTimeoutMinutes = 120,
    # The windows job in build.yml is unverified until it has run once, so the run is
    # judged on the Linux jobs alone. Add 'windows' here once it has been seen green.
    [string[]] $RequiredCiJobs = @('linux', 'arm64'),
    [string] $Repo = '',
    [string] $PiHost = '',
    [string] $NasHost = '',
    # Machine-local hosts (see Get-ReleaseConfig in release-common.ps1); not in the repository.
    [string] $ConfigFile = 'C:\Users\steve\foxsdr-build\deploy\release-config.json',
    [string] $StoreCredentialsFile = 'C:\Users\steve\foxsdr-build\store-credentials.json',
    [string] $StoreProductId = '9NBT95VMRVL2',
    [string] $StoreNotesFile = '',
    [string] $StoreMandatoryEffective = '',
    [string] $SiteDir = '',
    [string] $VcCrtDir = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Redist\MSVC\14.44.35112\x64\Microsoft.VC143.CRT',
    [string] $VcpkgRoot = 'C:\vcpkg',
    [string] $GitUserName = 'Steven Fox',
    [string] $GitUserEmail = 'wonderingStars@users.noreply.github.com'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0
. (Join-Path $PSScriptRoot 'release-common.ps1')

$script:StepOrder = @('preflight', 'build', 'installer', 'msix', 'install-test', 'ci', 'symbols', 'github', 'store', 'store-status')
$script:Results = New-Object System.Collections.ArrayList
$script:StoreSeamAnnounced = $false
$script:Ctx.DryRun = [bool]$DryRun

# The identity the Store package is built with (Partner Center > Product identity).
# Both values are inside every published package, so they are not secrets.
$script:StoreIdentityName = 'hedgerowlabs.FoxSDR'
$script:StoreIdentityPublisher = 'CN=A483DBDF-9D74-4013-9DFA-B1BE0334C31F'
$script:StorePublisherDisplayName = 'hedgerowlabs'

# ---------------------------------------------------------------------------
# What this release is
# ---------------------------------------------------------------------------

function Invoke-Git {
    param([string[]]$GitArgs, [string[]]$Identity = @())
    $git = Find-Tool 'git' @('C:\Program Files\Git\cmd\git.exe')
    if ($git -eq '') { Stop-Release 'git is not installed' }
    return (Invoke-Exe -File $git -Arguments (@('-C', $script:RC.Repo) + $Identity + $GitArgs) -NoLog)
}

function Get-ReleaseContext {
    $repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
    $cmakeText = [System.IO.File]::ReadAllText((Join-Path $repoRoot 'CMakeLists.txt'))
    $m = [regex]::Match($cmakeText, '(?m)^project\(cascade VERSION (\d+\.\d+\.\d+)')
    if (-not $m.Success) { throw 'RELEASE-FAIL: no "project(cascade VERSION x.y.z" line in CMakeLists.txt' }
    $version = $m.Groups[1].Value
    $git = Find-Tool 'git' @('C:\Program Files\Git\cmd\git.exe')
    $headRes = Invoke-Exe -File $git -Arguments @('-C', $repoRoot, 'rev-parse', 'HEAD') -NoLog
    if ($headRes.ExitCode -ne 0) { throw 'RELEASE-FAIL: git rev-parse HEAD failed (not a git checkout?)' }
    $head = $headRes.StdOut.Trim()
    $relDir = Join-Path $ReleaseRoot ('release-' + $version)
    $build = $BuildDir
    if ($build -eq '') { $build = Join-Path $repoRoot 'build' }
    $files = @(
        ('foxsdr-setup-' + $version + '.exe'),
        ('FoxSDR-' + $version + '-x86_64.AppImage'),
        ('foxsdr-' + $version + '-linux-x64.tar.gz'),
        ('FoxSDR-' + $version + '-aarch64.AppImage'),
        ('foxsdr-' + $version + '-linux-arm64.tar.gz')
    )
    return @{
        Repo = $repoRoot
        Git = $git
        Version = $version
        Head = $head
        HeadShort = $head.Substring(0, 12)
        Tag = ('v' + $version)
        ReleaseDir = $relDir
        Dist = (Join-Path $relDir 'dist')
        Msix = (Join-Path $relDir 'msix')
        MsixIn = (Join-Path $relDir 'msix-in')
        Ci = (Join-Path $relDir 'ci')
        Verify = (Join-Path $relDir 'verify')
        LogDir = (Join-Path $relDir 'log')
        Scratch = (Join-Path $relDir 'scratch')
        StoreDir = (Join-Path $relDir 'store')
        StatePath = (Join-Path $relDir 'state.json')
        BuildDir = $build
        NotesRel = ('docs/release-notes-' + $version + '.md')
        NotesPath = (Join-Path $repoRoot ('docs\release-notes-' + $version + '.md'))
        Files = $files
        InstallerName = $files[0]
        MsixName = ('FoxSDR-' + $version + '-x64.msix')
    }
}

# ---------------------------------------------------------------------------
# State: what each step finished, for which commit, with which output hashes
# ---------------------------------------------------------------------------

function Get-State {
    $s = Read-JsonFile $script:RC.StatePath
    if ($null -eq $s) { $s = @{ version = $script:RC.Version; head = $script:RC.Head; steps = @{} } }
    if (-not $s.ContainsKey('steps')) { $s['steps'] = @{} }
    return $s
}

function Save-State($State) {
    if ($script:Ctx.DryRun) { return }
    if (-not (Test-Path -LiteralPath $script:RC.ReleaseDir)) { New-Item -ItemType Directory -Path $script:RC.ReleaseDir -Force | Out-Null }
    Write-JsonFile $script:RC.StatePath $State
}

# $Outputs maps a path RELATIVE to the release folder to the sha256 recorded for it.
function Set-StepDone([string]$Step, [hashtable]$Outputs, [hashtable]$Info) {
    if ($script:Ctx.DryRun) { return }
    $s = Get-State
    $s['version'] = $script:RC.Version
    $s['head'] = $script:RC.Head
    $s['steps'][$Step] = @{ done = (Get-Date).ToUniversalTime().ToString('o'); head = $script:RC.Head; outputs = $Outputs; info = $Info }
    Save-State $s
}

function Get-StepInfo([string]$Step) {
    $s = Get-State
    if (-not $s['steps'].ContainsKey($Step)) { return $null }
    $rec = $s['steps'][$Step]
    if ($rec['head'] -ne $script:RC.Head) { return $null }
    return $rec['info']
}

# Done means: recorded for THIS commit, and every recorded output is still there
# with the recorded hash.
function Test-StepDone([string]$Step) {
    $s = Get-State
    if (-not $s['steps'].ContainsKey($Step)) { return $false }
    $rec = $s['steps'][$Step]
    if ($rec['head'] -ne $script:RC.Head) { return $false }
    foreach ($rel in @($rec['outputs'].Keys)) {
        $p = Join-Path $script:RC.ReleaseDir $rel
        if (-not (Test-Path -LiteralPath $p)) { return $false }
        if ((Get-Sha256 $p) -ne $rec['outputs'][$rel]) { return $false }
    }
    return $true
}

function Assert-StepDone([string]$Needed, [string]$ForStep) {
    if (Test-StepDone $Needed) { return }
    if ($script:Ctx.DryRun) {
        Write-Result 'INFO' ($ForStep + ' needs step ' + $Needed) 'not done yet for this commit (a real run stops here)'
        return
    }
    Stop-Release ('step ' + $ForStep + ' needs step ' + $Needed + ' to have run for commit ' + $script:RC.HeadShort + ' (run: tools\release.ps1 -Steps ' + $Needed + ')')
}

# ---------------------------------------------------------------------------
# The release notes: five "SHA-256 of `name`:" blocks
# ---------------------------------------------------------------------------

function Get-NotesEntries([string]$Path) {
    $text = [System.IO.File]::ReadAllText($Path)
    $ms = [regex]::Matches($text, '\*\*SHA-256 of `([^`]+)`:\*\*\r?\n`([^`]*)`')
    $list = @()
    foreach ($m in $ms) { $list += [pscustomobject]@{ Name = $m.Groups[1].Value; Value = $m.Groups[2].Value } }
    return $list
}

function Set-NotesHashes([string]$Path, [hashtable]$Hashes) {
    $bytes = [System.IO.File]::ReadAllBytes($Path)
    if ($bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF) {
        Stop-Release ($Path + ' starts with a UTF-8 BOM; it is not edited by a script')
    }
    $crBefore = @($bytes | Where-Object { $_ -eq 13 }).Count
    $text = [System.Text.Encoding]::UTF8.GetString($bytes)
    $pattern = '(\*\*SHA-256 of `([^`]+)`:\*\*\r?\n`)(TBD|[0-9a-f]{64})(`)'
    $ms = [regex]::Matches($text, $pattern)
    if ($ms.Count -ne 5) { Stop-Release ('expected five hash blocks in the notes, found ' + $ms.Count) }
    for ($i = $ms.Count - 1; $i -ge 0; $i--) {
        $name = $ms[$i].Groups[2].Value
        if (-not $Hashes.ContainsKey($name)) { Stop-Release ('the notes name ' + $name + ', which is not one of the five release files') }
        $g = $ms[$i].Groups[3]
        $text = $text.Substring(0, $g.Index) + $Hashes[$name] + $text.Substring($g.Index + $g.Length)
    }
    [System.IO.File]::WriteAllText($Path, $text, (Get-Utf8NoBom))
    $after = [System.IO.File]::ReadAllBytes($Path)
    $crAfter = @($after | Where-Object { $_ -eq 13 }).Count
    Assert-Check ($crBefore -eq $crAfter) 'notes line endings kept' ($crAfter.ToString() + ' CRs before and after') 'the edit changed the line endings of the notes file'
}

# The paragraphs above the "---" line, as plain text (what the site's notes.go entry
# and the Store's "What's new" both want).
function Get-ReleaseNotesPlain([string]$Path) {
    $md = ([System.IO.File]::ReadAllText($Path)) -replace "`r`n", "`n"
    $body = ($md -split "`n---`n")[0]
    $paras = @()
    foreach ($p in ($body -split "`n`n")) {
        $t = $p.Trim()
        if ($t -eq '') { continue }
        $t = $t.Replace('**', '').Replace('`', '')
        $t = [regex]::Replace($t, '\*([^*]+)\*', '$1')
        $paras += $t
    }
    return $paras
}

# ---------------------------------------------------------------------------
# preflight
# ---------------------------------------------------------------------------

# The commit a tag names, or '' when there is no such tag. `git rev-list -n 1 <tag>`
# rather than `rev-parse <tag>^{commit}`: it gives the commit for an annotated or a
# lightweight tag alike, and has no ^ in it for a shell in between to eat.
function Get-TagCommit([string]$Tag) {
    $r = Invoke-Git @('rev-list', '-n', '1', ('refs/tags/' + $Tag))
    if ($r.ExitCode -ne 0) { return '' }
    return $r.StdOut.Trim()
}

function Get-PreviousTag([string]$Version) {
    $r = Invoke-Git @('tag', '--list', 'v*', '--sort=-version:refname')
    $cur = [version]$Version
    foreach ($line in ($r.StdOut -split "`r?`n")) {
        $t = $line.Trim()
        if ($t -notmatch '^v(\d+\.\d+\.\d+)$') { continue }
        if ([version]$Matches[1] -lt $cur) { return $t }
    }
    return ''
}

function Get-SdkBin {
    $root = 'C:\Program Files (x86)\Windows Kits\10\bin'
    if (-not (Test-Path -LiteralPath $root)) { return '' }
    $d = Get-ChildItem -LiteralPath $root -Directory | Where-Object { $_.Name -match '^10\.' -and (Test-Path (Join-Path $_.FullName 'x64\makeappx.exe')) } | Sort-Object Name | Select-Object -Last 1
    if ($null -eq $d) { return '' }
    return $d.FullName
}

function Step-Preflight {
    $RC = $script:RC
    $v = $RC.Version
    Write-Result 'INFO' 'release' ($v + ' at commit ' + $RC.HeadShort + ' (' + $RC.Repo + ')')

    # --- the tree -----------------------------------------------------------
    $st = Invoke-Git @('status', '--porcelain=v1', '--untracked-files=all')
    $lines = @($st.StdOut -split "`r?`n" | Where-Object { $_ -ne '' })
    $notesRel = $RC.NotesRel
    $state = Get-State
    if ($lines.Count -eq 1 -and $lines[0] -match ('^ M ' + [regex]::Escape($notesRel) + '$') -and $state.ContainsKey('notesFilledSha256') -and (Get-Sha256 $RC.NotesPath) -eq $state['notesFilledSha256']) {
        Write-Result 'PASS' 'tree clean' 'apart from the five hash lines the ci step wrote into the release notes'
    } elseif ($lines.Count -eq 0) {
        Write-Result 'PASS' 'tree clean' 'git status --porcelain is empty'
    } elseif ($AllowDirty) {
        Write-Result 'WARN' 'tree NOT clean (allowed by -AllowDirty)' ($lines.Count.ToString() + ' path(s); the binary will carry -dirty')
        foreach ($l in ($lines | Select-Object -First 6)) { Write-Detail $l }
    } else {
        foreach ($l in ($lines | Select-Object -First 8)) { Write-Detail $l }
        Assert-Check $false 'tree clean' ($lines.Count.ToString() + ' path(s) modified or untracked') 'A release is built from a clean worktree at the release commit: a modified tracked file stamps "-dirty" into the commit the binary reports (cmake/git-commit.cmake). Commit or discard, or pass -AllowDirty for a rehearsal.'
    }

    # --- the tag ------------------------------------------------------------
    $tagCommit = Get-TagCommit $RC.Tag
    if ($tagCommit -eq '') {
        Write-Result 'PASS' 'tag taggable' ($RC.Tag + ' does not exist yet; the github step will tag ' + $RC.HeadShort)
    } elseif ($tagCommit -eq $RC.Head) {
        $kind = 'lightweight'
        if ((Invoke-Git @('cat-file', '-t', ('refs/tags/' + $RC.Tag))).StdOut.Trim() -eq 'tag') { $kind = 'annotated' }
        Write-Result 'INFO' 'already tagged' ($RC.Tag + ' is an ' + $kind + ' tag on HEAD (informational: this release exists)')
    } else {
        Assert-Check $false 'tag taggable' ($RC.Tag + ' exists on ' + $tagCommit.Substring(0, 12) + ', HEAD is ' + $RC.HeadShort) 'The tag names a different commit. Do not move a published tag; bump the version instead.'
    }

    # --- the version, three places ------------------------------------------
    $readmeText = [System.IO.File]::ReadAllText((Join-Path $RC.Repo 'README.md'))
    $rm = [regex]::Match($readmeText, 'The current release is \*\*(\d+\.\d+\.\d+)\*\*')
    $readmeVer = ''
    if ($rm.Success) { $readmeVer = $rm.Groups[1].Value }
    Assert-Check ($readmeVer -eq $v) 'version: CMakeLists.txt = README.md' ('CMakeLists ' + $v + ', README "' + $readmeVer + '"') 'README.md line "The current release is **x.y.z**" must carry the CMake project version.'
    Assert-Check (Test-Path -LiteralPath $RC.NotesPath) 'version: release notes file' $RC.NotesRel ('docs\release-notes-' + $v + '.md does not exist')

    # --- the notes' five hash lines -----------------------------------------
    $entries = @(Get-NotesEntries $RC.NotesPath)
    $gotNames = @($entries | ForEach-Object { $_.Name } | Sort-Object)
    $wantNames = @($RC.Files | Sort-Object)
    Assert-Check (($gotNames -join '|') -eq ($wantNames -join '|')) 'notes name the five release files' ($entries.Count.ToString() + ' blocks') ('expected exactly: ' + ($RC.Files -join ', '))
    $tbd = @($entries | Where-Object { $_.Value -eq 'TBD' }).Count
    $real = @($entries | Where-Object { $_.Value -match '^[0-9a-f]{64}$' }).Count
    Assert-Check ($tbd -eq 5 -or $real -eq 5) 'notes hash lines' ($tbd.ToString() + ' x TBD, ' + $real.ToString() + ' x sha256') 'The notes must carry five TBD lines (before the ci step) or five real hashes (after it), never a mixture.'

    # --- the tools ------------------------------------------------------------
    $cm = Find-Tool 'cmake' @('C:\Program Files\CMake\bin\cmake.exe')
    Assert-Check ($cm -ne '') 'tool: cmake' $cm 'cmake is not installed'
    $script:Cmake = $cm
    $cv = Invoke-Exe -File $cm -Arguments @('--version') -NoLog
    Write-Result 'PASS' 'tool: cmake version' (($cv.StdOut -split "`r?`n")[0])
    $gen = Invoke-Exe -File $cm -Arguments @('--help') -NoLog
    Assert-Check ($gen.StdOut -match 'Visual Studio 17 2022') 'tool: generator "Visual Studio 17 2022"' 'listed by cmake --help' 'install Visual Studio 2022 (Build Tools) with the C++ workload'
    $tc = Join-Path $VcpkgRoot 'scripts\buildsystems\vcpkg.cmake'
    Assert-Check (Test-Path -LiteralPath $tc) 'tool: vcpkg toolchain' $tc 'vcpkg is expected at C:\vcpkg (-VcpkgRoot)'
    $soapy = Join-Path $VcpkgRoot 'installed\x64-windows\share\soapysdr\SoapySDRConfig.cmake'
    Assert-Check (Test-Path -LiteralPath $soapy) 'tool: vcpkg soapysdr:x64-windows' $soapy 'vcpkg install soapysdr:x64-windows'
    $crtMissing = @('msvcp140.dll', 'vcruntime140.dll', 'vcruntime140_1.dll') | Where-Object { -not (Test-Path -LiteralPath (Join-Path $VcCrtDir $_)) }
    Assert-Check (@($crtMissing).Count -eq 0) 'tool: app-local VC CRT' $VcCrtDir ('missing there: ' + (@($crtMissing) -join ', ') + ' (-VcCrtDir)')
    $iscc = Find-Iscc
    Assert-Check ($iscc -ne '') 'tool: ISCC' $iscc 'Inno Setup 6 is expected under %LOCALAPPDATA%\Programs\Inno Setup 6'
    $py = Invoke-Exe -File 'py' -Arguments @('-3.14', '-c', 'import PIL, sys; print(sys.version.split()[0], "pillow", PIL.__version__)') -NoLog
    Assert-Check ($py.ExitCode -eq 0) 'tool: py -3.14 with pillow' $py.StdOut.Trim() 'py -3.14 -m pip install pillow (the symbol map export and the MSIX logos need it)'
    $sdk = Get-SdkBin
    Assert-Check ($sdk -ne '') 'tool: Windows SDK makeappx/makepri/signtool' $sdk 'install the Windows 10/11 SDK'
    $gh = Find-Gh
    Assert-Check ($gh -ne '') 'tool: gh' $gh 'winget install GitHub.cli'
    $script:Gh = $gh
    $auth = Invoke-Exe -File $gh -Arguments @('auth', 'status') -NoLog
    $acct = [regex]::Match($auth.Output, 'account (\S+)')
    $acctName = 'logged in'
    if ($acct.Success) { $acctName = 'account ' + $acct.Groups[1].Value }
    Assert-Check ($auth.ExitCode -eq 0) 'tool: gh auth status' $acctName 'gh auth login (feed a token from Git Bash redirect, not from PowerShell 5.1)'

    $cfg = $script:Cfg
    $nasRes = Invoke-Ssh -HostName $cfg['nasHost'] -Command 'true'
    Assert-Check ($nasRes.ExitCode -eq 0) ('host: ssh ' + $cfg['nasHost'] + ' true') ('exit ' + $nasRes.ExitCode) ('the NAS did not answer a key-authenticated ssh: ' + $nasRes.Output.Trim())
    Assert-Check ($cfg['piHost'] -ne '') 'host: Pi configured' 'piHost' 'No Pi host. Give -PiHost, set FOXSDR_PI_HOST, or put {"piHost": "<user>@<address>"} in release-config.json beside the release folders. It is kept out of the (public) repository on purpose.'
    $piRes = Invoke-Ssh -HostName $cfg['piHost'] -Command 'true'
    Assert-Check ($piRes.ExitCode -eq 0) ('host: ssh ' + $cfg['piHost'] + ' true') ('exit ' + $piRes.ExitCode) ('the Pi did not answer a key-authenticated ssh: ' + $piRes.Output.Trim())

    # --- the telemetry Worker ---------------------------------------------------
    # A Worker that is older than the application drops the fields the application
    # sends (0.99.64 -> 0.99.65 lost a morning of slow-frame counts that way), so a
    # change to worker.js since the previous release is a hard stop until the owner
    # says it has been deployed.
    $prev = Get-PreviousTag $v
    if ($prev -eq '') {
        Assert-Check ($WorkerDeployed.IsPresent) 'telemetry Worker' ('no earlier release tag to compare with; ' + $(if ($WorkerDeployed.IsPresent) { '-WorkerDeployed given' } else { '-WorkerDeployed not given' })) 'Cannot tell whether worker.js changed. Deploy the Worker (cd telemetry-worker; npx --yes wrangler deploy), then pass -WorkerDeployed.'
    } else {
        $wd = Invoke-Git @('diff', '--name-only', $prev, 'HEAD', '--', 'telemetry-worker/worker.js')
        if ($wd.StdOut.Trim() -eq '') {
            Write-Result 'PASS' 'telemetry Worker vocabulary' ('telemetry-worker/worker.js unchanged since ' + $prev)
        } else {
            $why = (Invoke-Git @('log', '--oneline', ($prev + '..HEAD'), '--', 'telemetry-worker/worker.js')).StdOut.Trim()
            Write-Result 'WARN' 'telemetry-worker/worker.js changed' ('since ' + $prev + ':')
            foreach ($l in ($why -split "`r?`n")) { Write-Detail $l }
            Assert-Check ($WorkerDeployed.IsPresent) 'telemetry Worker deployed' ($(if ($WorkerDeployed.IsPresent) { '-WorkerDeployed given' } else { '-WorkerDeployed not given' })) 'Deploy it BEFORE releasing an app that sends new fields: cd telemetry-worker; npx --yes wrangler deploy (the account token is the owner''s); probe: POST "not json" to https://telemetry.foxsdr.com/ answers 400; then pass -WorkerDeployed.'
            if (-not $script:Ctx.DryRun) {
                $probe = Invoke-Exe -File 'curl.exe' -Arguments @('-s', '-m', '20', '-o', 'NUL', '-w', '%{http_code}', '-X', 'POST', '-d', 'not json', 'https://telemetry.foxsdr.com/') -NoLog
                Assert-Check ($probe.StdOut.Trim() -eq '400') 'telemetry Worker probe' ('POST "not json" answered ' + $probe.StdOut.Trim()) 'the Worker should answer 400 to a body that is not JSON'
            }
        }
    }

    if (-not $script:Ctx.DryRun) {
        foreach ($d in @($RC.ReleaseDir, $RC.Dist, $RC.Msix, $RC.Ci, $RC.Verify, $RC.LogDir)) {
            if (-not (Test-Path -LiteralPath $d)) { New-Item -ItemType Directory -Path $d -Force | Out-Null }
        }
        $s = Get-State
        $s['version'] = $v
        $s['head'] = $RC.Head
        Save-State $s
    }
    if ($script:Ctx.DryRun) { Write-Result 'INFO' 'release folder' ($RC.ReleaseDir + ' (created by a real run)') } else { Write-Result 'PASS' 'release folder' $RC.ReleaseDir }
    return 'done'
}

# ---------------------------------------------------------------------------
# build
# ---------------------------------------------------------------------------

function Assert-TreeCleanForBuild {
    $st = Invoke-Git @('status', '--porcelain=v1', '--untracked-files=no')
    $n = @($st.StdOut -split "`r?`n" | Where-Object { $_ -ne '' }).Count
    if ($n -eq 0) { Write-Result 'PASS' 'no tracked file modified' 'the binary will report a clean commit'; return }
    if ($AllowDirty) { Write-Result 'WARN' 'tracked files modified (allowed by -AllowDirty)' ($n.ToString() + ' file(s); the binary reports a -dirty commit'); return }
    Assert-Check $false 'no tracked file modified' ($n.ToString() + ' file(s)') 'A modified tracked file stamps "-dirty" into the binary that ships. Build from a clean worktree.'
}

function Get-VersionOutput([string]$Exe) {
    $r = Invoke-Exe -File $Exe -Arguments @('--version') -TimeoutSec 60 -Env @{ FOXSDR_TELEMETRY_URL = 'http://127.0.0.1:9/'; FOXSDR_UPDATE_URL = 'http://127.0.0.1:9/' }
    return $r
}

function Step-Build {
    $RC = $script:RC
    $v = $RC.Version
    if (-not $script:Ctx.DryRun) { foreach ($d in @($RC.MsixIn, $RC.Verify)) { if (-not (Test-Path -LiteralPath $d)) { New-Item -ItemType Directory -Path $d -Force | Out-Null } } }
    if (-not $Force -and -not $script:Ctx.DryRun -and (Test-StepDone 'build')) {
        $info = Get-StepInfo 'build'
        Write-Result 'SKIP' 'build' ('already done for this commit: cascade.exe sha256 ' + $info['exeSha256'])
        return 'skipped'
    }
    if ($script:Ctx.DryRun -and (Test-StepDone 'build')) { Write-Result 'INFO' 'build' 'already done for this commit (a real run would skip it)' }
    Assert-TreeCleanForBuild

    $toolchain = (Join-Path $VcpkgRoot 'scripts\buildsystems\vcpkg.cmake').Replace('\', '/')
    $cmake = Find-Tool 'cmake' @('C:\Program Files\CMake\bin\cmake.exe')
    $cfgArgs = @('-S', $RC.Repo, '-B', $RC.BuildDir, '-G', 'Visual Studio 17 2022', '-A', 'x64',
        ('-DCMAKE_TOOLCHAIN_FILE=' + $toolchain), '-DVCPKG_TARGET_TRIPLET=x64-windows', '-DCMAKE_BUILD_TYPE=Release')
    $r = Invoke-Planned -What 'configure' -File $cmake -Arguments $cfgArgs -TimeoutSec 900 -WorkDir $RC.Repo
    if ($null -ne $r) {
        Assert-Check ($r.ExitCode -eq 0) 'configure exit code' ([string]$r.ExitCode) ('see ' + $script:Ctx.LogPath)
        $vs = [regex]::Match($r.Output, 'cascade version string: (\S+)')
        Assert-Check ($vs.Success -and $vs.Groups[1].Value -eq $v) 'configure: version string' ($vs.Groups[1].Value + ' (CMakeLists ' + $v + ')') 'CASCADE_VERSION_STRING must be the project version in a release build; a stale cache entry or a -D override would make the binary report something else'
    }

    # Built TWICE. The first pass registers anything a CONFIGURE_DEPENDS glob picked
    # up late and runs the symbol archive; the second must be a no-op, and says so by
    # leaving cascade.exe byte-identical.
    $exe = Join-Path $RC.BuildDir 'Release\cascade.exe'
    $hashes = @()
    foreach ($pass in 1, 2) {
        $b = Invoke-Planned -What ('build pass ' + $pass) -File $cmake -Arguments @('--build', $RC.BuildDir, '--config', 'Release', '--target', 'cascade', '--', '-m:4') -TimeoutSec 3600 -WorkDir $RC.Repo
        if ($null -eq $b) { continue }
        Assert-Check ($b.ExitCode -eq 0) ('build pass ' + $pass + ' exit code') ([string]$b.ExitCode + ' in ' + ('{0:N0}' -f $b.Seconds) + ' s') ('see ' + $script:Ctx.LogPath)
        Assert-Check (Test-Path -LiteralPath $exe) ('build pass ' + $pass + ': cascade.exe') $exe 'the build reported success but wrote no cascade.exe'
        $hashes += (Get-Sha256 $exe)
    }
    if ($script:Ctx.DryRun) {
        Write-Plan 'check' 'cascade.exe --version prints "FoxSDR <version>"'
        Write-Plan 'check' 'build\Release holds no runtime DLL but SoapySDR.dll'
        Write-Plan 'archive symbols' ('tools\archive-symbols.ps1 -Binary ' + $exe + ' -EmitPaths')
        Write-Plan 'copy' ('cascade.exe and SoapySDR.dll to ' + $RC.MsixIn)
        return 'done'
    }
    if ($hashes[0] -eq $hashes[1]) {
        Write-Result 'PASS' 'build pass 2 left cascade.exe unchanged' $hashes[1]
    } else {
        Write-Result 'WARN' 'build pass 2 relinked cascade.exe' ('pass 1 ' + $hashes[0] + ', pass 2 ' + $hashes[1] + '; the final one is what ships and what the symbols below are archived for')
    }

    $ver = Get-VersionOutput $exe
    $line = ($ver.StdOut -split "`r?`n" | Where-Object { $_.Trim() -ne '' } | Select-Object -First 1)
    if ($null -eq $line) { $line = '' }
    Assert-Check ($ver.ExitCode -eq 0 -and $line.Trim() -eq ('FoxSDR ' + $v)) 'cascade.exe --version' ('"' + $line.Trim() + '" exit ' + $ver.ExitCode) ('expected "FoxSDR ' + $v + '"; for a windows-subsystem build the output must still arrive on the redirected pipe')
    $sub = Get-PeSubsystem $exe
    Write-Result 'INFO' 'cascade.exe subsystem' $sub

    $relDir = Join-Path $RC.BuildDir 'Release'
    $dlls = @(Get-ChildItem -LiteralPath $relDir -Filter *.dll -File | ForEach-Object { $_.Name })
    $extra = @($dlls | Where-Object { $_.ToLowerInvariant() -ne 'soapysdr.dll' })
    Assert-Check ($dlls.Count -ge 1 -and $extra.Count -eq 0) 'payload contract: only SoapySDR.dll' ($dlls -join ', ') 'installer\cascade.iss and build-msix.ps1 both refuse any other runtime DLL; add it to both on purpose or remove it'

    # The symbols for exactly this binary. The CMake POST_BUILD step already did this;
    # running the archiver again is idempotent and returns the paths that belong to
    # THIS build, which is what must be mirrored (the archive also holds every
    # incremental relink that ever shared it).
    $symRoot = Join-Path $RC.Repo 'symbols'
    $archived = @(& (Join-Path $RC.Repo 'tools\archive-symbols.ps1') -Binary $exe -ArchiveRoot $symRoot -Version $v -CommitHeader (Join-Path $RC.BuildDir 'generated\cascade_git_commit.h') -EmitPaths)
    Assert-Check ($archived.Count -ge 4) 'symbols archived' ($archived.Count.ToString() + ' paths') 'tools\archive-symbols.ps1 returned fewer than the pdb, the exe, the map and index.txt: a build without symbols makes every crash report against it unreadable for ever'
    $pdbRel = [string]$archived[0]
    $buildId = $pdbRel.Split('\')[1]
    $symFiles = @{}
    foreach ($rel in $archived) {
        if ([string]$rel -eq 'index.txt') { continue }
        $full = Join-Path $symRoot ([string]$rel)
        Assert-Check ((Test-Path -LiteralPath $full) -and ((Get-Item -LiteralPath $full).Length -gt 0)) ('symbol file ' + $rel) '' ($full + ' is missing or empty')
        $symFiles[[string]$rel] = (Get-Sha256 $full)
    }
    Assert-Check ($symFiles.Keys -contains ('cascade.pdb\' + $buildId + '\symmap.json.gz')) 'symbol map for this build id' $buildId 'symmap.json.gz is what the crash dashboard groups faults by; archive-symbols.ps1 needs py -3.14 to make it'
    $idxPath = Join-Path $symRoot 'index.txt'
    $row = ''
    foreach ($l in [System.IO.File]::ReadAllLines($idxPath)) { if ($l.Contains("`t" + $buildId + "`t")) { $row = $l } }
    Assert-Check ($row -ne '') 'symbol index row for this build id' $buildId ($idxPath + ' has no row for it')
    Write-JsonFile (Join-Path $RC.Verify 'symbols.json') @{ buildId = $buildId; indexRow = $row; files = $symFiles }
    Write-Result 'PASS' 'symbols for this build' ('build id ' + $buildId + ', ' + $symFiles.Count + ' files')

    Copy-Item -LiteralPath $exe -Destination (Join-Path $RC.MsixIn 'cascade.exe') -Force
    Copy-Item -LiteralPath (Join-Path $relDir 'SoapySDR.dll') -Destination (Join-Path $RC.MsixIn 'SoapySDR.dll') -Force
    $exeSha = Get-Sha256 (Join-Path $RC.MsixIn 'cascade.exe')
    $dllSha = Get-Sha256 (Join-Path $RC.MsixIn 'SoapySDR.dll')
    Assert-Check ($exeSha -eq $hashes[1]) 'payload copy matches the build' $exeSha 'the copy in msix-in differs from build\Release\cascade.exe'
    $size = (Get-Item -LiteralPath $exe).Length
    Write-Result 'PASS' 'cascade.exe' ('{0:N0} bytes, sha256 {1}' -f $size, $exeSha)
    Set-StepDone 'build' @{ 'msix-in\cascade.exe' = $exeSha; 'msix-in\SoapySDR.dll' = $dllSha } @{ exeSha256 = $exeSha; dllSha256 = $dllSha; size = $size; buildId = $buildId; subsystem = $sub; versionLine = $line.Trim() }
    return 'done'
}

# ---------------------------------------------------------------------------
# installer
# ---------------------------------------------------------------------------

function Step-Installer {
    $RC = $script:RC
    $v = $RC.Version
    $setup = Join-Path $RC.Dist $RC.InstallerName
    if (-not $Force -and -not $script:Ctx.DryRun -and (Test-StepDone 'installer')) {
        Write-Result 'SKIP' 'installer' ('already done: ' + (Get-FileInfoLine $setup))
        return 'skipped'
    }
    Assert-StepDone 'build' 'installer'
    $iscc = Find-Iscc
    Assert-Check ($iscc -ne '') 'ISCC found' $iscc 'Inno Setup 6 is expected under %LOCALAPPDATA%\Programs\Inno Setup 6'
    $genIss = Join-Path $RC.Repo 'installer\generated-version.iss'
    if (Test-Path -LiteralPath $genIss) {
        $gv = [System.IO.File]::ReadAllText($genIss)
        Assert-Check ($gv -match ('#define AppVersion "' + [regex]::Escape($v) + '"')) 'installer version include' ('generated-version.iss says ' + $v) 'installer\generated-version.iss is written at configure time; reconfigure'
    }
    if (-not $script:Ctx.DryRun -and -not (Test-Path -LiteralPath $RC.Dist)) { New-Item -ItemType Directory -Path $RC.Dist -Force | Out-Null }
    # Started from here, not from Git Bash: ISCC's /D and /O switches are rewritten
    # into paths by MSYS. The switches are the ones `ISCC /?` prints.
    $isccArgs = @(('/O' + $RC.Dist), ('/DBuildDir=' + (Join-Path $RC.BuildDir 'Release')), ('/DVcCrtDir=' + $VcCrtDir), (Join-Path $RC.Repo 'installer\cascade.iss'))
    $r = Invoke-Planned -What 'iscc' -File $iscc -Arguments $isccArgs -TimeoutSec 900 -WorkDir $RC.Repo
    if ($null -eq $r) { Write-Plan 'check' ('the installer ' + $RC.InstallerName + ' exists, with its size and sha256'); return 'done' }
    Assert-Check ($r.ExitCode -eq 0 -and $r.Output -match 'Successful compile') 'ISCC exit code' ([string]$r.ExitCode) ('see ' + $script:Ctx.LogPath)
    Assert-Check (Test-Path -LiteralPath $setup) ('installer ' + $RC.InstallerName) $setup 'ISCC reported success but the file is not in dist'
    $size = (Get-Item -LiteralPath $setup).Length
    Assert-Check ($size -gt 1000000) 'installer size' ('{0:N0} bytes' -f $size) 'an installer under 1 MB cannot hold the application'
    $sha = Get-Sha256 $setup
    $sig = (Get-AuthenticodeSignature -LiteralPath $setup).Status
    Write-Result 'INFO' 'installer signature' ($sig.ToString() + ' (the installer is not code-signed; SmartScreen will say so)')
    Write-Result 'PASS' 'installer' ('{0:N0} bytes, sha256 {1}' -f $size, $sha)
    Set-StepDone 'installer' @{ ('dist\' + $RC.InstallerName) = $sha } @{ size = $size; sha256 = $sha }
    return 'done'
}

# ---------------------------------------------------------------------------
# msix
# ---------------------------------------------------------------------------

function Read-ZipEntryText($Zip, [string]$Name) {
    $e = $Zip.GetEntry($Name)
    if ($null -eq $e) { return $null }
    $sr = New-Object System.IO.StreamReader($e.Open(), [System.Text.Encoding]::UTF8)
    try { return $sr.ReadToEnd() } finally { $sr.Dispose() }
}

function Get-ZipEntrySha256($Zip, [string]$Name) {
    $e = $Zip.GetEntry($Name)
    if ($null -eq $e) { return '' }
    $st = $e.Open()
    try {
        $sha = [System.Security.Cryptography.SHA256]::Create()
        $h = $sha.ComputeHash($st)
        return (([System.BitConverter]::ToString($h)).Replace('-', '').ToLowerInvariant())
    } finally { $st.Dispose() }
}

function Step-Msix {
    $RC = $script:RC
    $v = $RC.Version
    $msix = Join-Path $RC.Msix $RC.MsixName
    if (-not $Force -and -not $script:Ctx.DryRun -and (Test-StepDone 'msix')) {
        Write-Result 'SKIP' 'msix' ('already done: ' + (Get-FileInfoLine $msix))
        return 'skipped'
    }
    Assert-StepDone 'build' 'msix'
    # The SAME exe as the installer carries, from the directory that holds only
    # cascade.exe and SoapySDR.dll (build-msix.ps1 refuses any other DLL).
    if (-not $script:Ctx.DryRun) {
        $buildInfo = Get-StepInfo 'build'
        $have = Get-Sha256 (Join-Path $RC.MsixIn 'cascade.exe')
        Assert-Check ($have -eq $buildInfo['exeSha256']) 'msix payload is the built exe' $have 'msix-in\cascade.exe differs from the one the build step recorded'
        $names = @(Get-ChildItem -LiteralPath $RC.MsixIn -File | ForEach-Object { $_.Name } | Sort-Object)
        Assert-Check (($names -join '|') -eq 'cascade.exe|SoapySDR.dll') 'msix-in holds only cascade.exe and SoapySDR.dll' ($names -join ', ') 'anything else in that folder would be packaged'
    }
    $psExe = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
    $msixArgs = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $RC.Repo 'installer\msix\build-msix.ps1'),
        '-BuildDir', $RC.MsixIn, '-VcCrtDir', $VcCrtDir,
        '-IdentityName', $script:StoreIdentityName, '-IdentityPublisher', $script:StoreIdentityPublisher,
        '-PublisherDisplayName', $script:StorePublisherDisplayName, '-Force')
    $r = Invoke-Planned -What 'build-msix' -File $psExe -Arguments $msixArgs -TimeoutSec 900 -WorkDir $RC.Repo
    if ($null -eq $r) {
        Write-Plan 'check' ('package manifest: Identity Name ' + $script:StoreIdentityName + ', Publisher ' + $script:StoreIdentityPublisher + ', Version ' + (Get-MsixVersion $v))
        Write-Plan 'check' 'cascade.exe inside the package has the sha256 of the built exe'
        Write-Plan 'copy' ('installer\msix\Output\' + $RC.MsixName + ' to ' + $RC.Msix)
        return 'done'
    }
    Assert-Check ($r.ExitCode -eq 0) 'build-msix.ps1 exit code' ([string]$r.ExitCode) ('see ' + $script:Ctx.LogPath)
    $built = Join-Path $RC.Repo ('installer\msix\Output\' + $RC.MsixName)
    Assert-Check (Test-Path -LiteralPath $built) ('package ' + $RC.MsixName) $built 'build-msix.ps1 reported success but wrote no package'
    if (-not (Test-Path -LiteralPath $RC.Msix)) { New-Item -ItemType Directory -Path $RC.Msix -Force | Out-Null }
    Copy-Item -LiteralPath $built -Destination $msix -Force

    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $zip = [System.IO.Compression.ZipFile]::OpenRead($msix)
    try {
        $manifestText = Read-ZipEntryText $zip 'AppxManifest.xml'
        Assert-Check ($null -ne $manifestText) 'package has AppxManifest.xml' '' 'the package is not a valid MSIX'
        $xml = New-Object System.Xml.XmlDocument
        $xml.LoadXml($manifestText)
        $id = $xml.DocumentElement.SelectSingleNode("*[local-name()='Identity']")
        $wantVer = Get-MsixVersion $v
        Assert-Check ($id.GetAttribute('Name') -eq $script:StoreIdentityName) 'package Identity Name' $id.GetAttribute('Name') ('expected ' + $script:StoreIdentityName)
        Assert-Check ($id.GetAttribute('Publisher') -eq $script:StoreIdentityPublisher) 'package Identity Publisher' $id.GetAttribute('Publisher') ('expected ' + $script:StoreIdentityPublisher)
        Assert-Check ($id.GetAttribute('Version') -eq $wantVer) 'package Identity Version' ($id.GetAttribute('Version') + ' (product ' + $v + ', major + 1)') ('expected ' + $wantVer)
        $inner = Get-ZipEntrySha256 $zip 'cascade.exe'
        $builtSha = (Get-StepInfo 'build')['exeSha256']
        Assert-Check ($inner -eq $builtSha) 'cascade.exe inside the package' $inner ('the built exe is ' + $builtSha)
    } finally { $zip.Dispose() }
    $sha = Get-Sha256 $msix
    Write-Result 'PASS' 'msix' ('{0:N0} bytes, sha256 {1} (unsigned: the Store signs it)' -f (Get-Item -LiteralPath $msix).Length, $sha)
    Set-StepDone 'msix' @{ ('msix\' + $RC.MsixName) = $sha } @{ sha256 = $sha; packageVersion = (Get-MsixVersion $v) }
    return 'done'
}

# Product 0.99.68 packs to 1.99.68.0: a Store package version may not start with 0,
# so the major is the product major plus one (installer\msix\README.md section 5.2).
function Get-MsixVersion([string]$Version) {
    $p = $Version.Split('.')
    return ('{0}.{1}.{2}.0' -f ([int]$p[0] + 1), $p[1], $p[2])
}

# ---------------------------------------------------------------------------
# install-test
# ---------------------------------------------------------------------------

function Get-InstalledState {
    $guid = 'B3D4A7E2-6C51-4F8B-9A0D-2E7F5C8B1A64'
    $key = 'Software\Microsoft\Windows\CurrentVersion\Uninstall\{' + $guid + '}_is1'
    $found = @()
    foreach ($hive in @(@('HKCU', 'HKCU:\'), @('HKLM', 'HKLM:\'))) {
        foreach ($sub in @($key, ('Software\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\{' + $guid + '}_is1'))) {
            $p = $hive[1] + $sub
            if (Test-Path -LiteralPath $p) { $found += $hive[0] + ':' + $sub }
        }
    }
    return $found
}

function Wait-ForGone([string]$Path, [int]$TimeoutSec) {
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    while (Test-Path -LiteralPath $Path) {
        if ($sw.Elapsed.TotalSeconds -ge $TimeoutSec) { return -1.0 }
        Start-Sleep -Milliseconds 500
    }
    return $sw.Elapsed.TotalSeconds
}

function Step-InstallTest {
    $RC = $script:RC
    $v = $RC.Version
    $setup = Join-Path $RC.Dist $RC.InstallerName
    $installDir = Join-Path $env:LOCALAPPDATA 'Programs\FoxSDR'
    $guid = 'B3D4A7E2-6C51-4F8B-9A0D-2E7F5C8B1A64'
    if (-not $Force -and -not $script:Ctx.DryRun -and (Test-StepDone 'install-test')) {
        Write-Result 'SKIP' 'install-test' 'already passed for this commit'
        return 'skipped'
    }
    Assert-StepDone 'installer' 'install-test'
    Assert-StepDone 'build' 'install-test'
    $scratch = Join-Path $RC.Scratch 'install-test'
    $env1 = @{
        APPDATA = (Join-Path $scratch 'appdata')
        LOCALAPPDATA = (Join-Path $scratch 'localappdata')
        CASCADE_CONFIG_TEST = (Join-Path $scratch 'config.json')
        # Nothing leaves the machine from a test run: usage and update traffic go to the
        # discard port, and the scratch config has both switched off as well.
        FOXSDR_TELEMETRY_URL = 'http://127.0.0.1:9/'
        FOXSDR_UPDATE_URL = 'http://127.0.0.1:9/'
    }
    $instArgs = @('/CURRENTUSER', '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', ('/LOG=' + (Join-Path $RC.Verify 'install.log')))
    if ($script:Ctx.DryRun) {
        Write-Plan 'check' ('no FoxSDR installed in either scope, and ' + $installDir + ' absent')
        Write-Result 'DRY' 'install (per user)' (Format-CommandLine $setup $instArgs)
        Write-Plan 'check' 'installed cascade.exe and SoapySDR.dll have the sha256 of the built files; the other payload files are present'
        Write-Plan 'check' 'installed cascade.exe --version prints "FoxSDR <version>" (read from redirected pipes, so a windows-subsystem exe works)'
        Write-Plan 'check' 'installed cascade.exe --selftest prints "selftest PASS", and --frames 60 exits 0 having rendered 60 frames, with scratch APPDATA/LOCALAPPDATA/CASCADE_CONFIG_TEST'
        Write-Result 'DRY' 'uninstall' (Format-CommandLine (Join-Path $installDir 'unins000.exe') @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', ('/LOG=' + (Join-Path $RC.Verify 'uninstall.log'))))
        Write-Plan 'check' ($installDir + ' is gone afterwards (polled: the uninstaller returns before it has finished), and so are its registry keys')
        return 'done'
    }

    $already = @(Get-InstalledState)
    $dirThere = Test-Path -LiteralPath $installDir
    if (($already.Count -gt 0 -or $dirThere) -and -not $AllowReplaceInstalled) {
        Assert-Check $false 'no FoxSDR installed before the test' (($already -join ', ') + ' ' + $installDir) 'The installer silently uninstalls a FoxSDR it finds in the OTHER scope (installer\cascade.iss PrepareToInstall), and this test would remove an install it did not make. Uninstall it yourself, or pass -AllowReplaceInstalled.'
    }
    Write-Result 'PASS' 'nothing installed before the test' ($installDir + ' absent, no uninstall key in HKCU/HKLM')
    foreach ($d in @($scratch, $env1['APPDATA'], $env1['LOCALAPPDATA'])) { if (-not (Test-Path -LiteralPath $d)) { New-Item -ItemType Directory -Path $d -Force | Out-Null } }
    [System.IO.File]::WriteAllText($env1['CASCADE_CONFIG_TEST'], '{"telemetryEnabled": false, "updateCheckEnabled": false}' + "`r`n", (Get-Utf8NoBom))

    $buildInfo = Get-StepInfo 'build'
    $installed = $false
    $ur = $null
    try {
        $r = Invoke-Planned -What 'install' -File $setup -Arguments $instArgs -TimeoutSec 600
        # Setup exit codes (jrsoftware.org/ishelp/topic_setupexitcodes.htm): 0 is success.
        Assert-Check ($r.ExitCode -eq 0) 'installer exit code' ([string]$r.ExitCode) 'Setup returned non-zero (1 failed to initialise, 3 fatal while preparing, 4 fatal during install, 5 cancelled, 7/8 cannot proceed); read verify\install.log'
        $installed = $true
        $exe = Join-Path $installDir 'cascade.exe'
        Assert-Check (Test-Path -LiteralPath $exe) 'installed cascade.exe' $exe 'the installer reported success but the file is not there'
        $instSha = Get-Sha256 $exe
        Assert-Check ($instSha -eq $buildInfo['exeSha256']) 'installed cascade.exe = built cascade.exe' ('installed ' + $instSha + ' / built ' + $buildInfo['exeSha256']) 'the installer carries a different binary from the one that was built and hashed'
        $instDll = Get-Sha256 (Join-Path $installDir 'SoapySDR.dll')
        Assert-Check ($instDll -eq $buildInfo['dllSha256']) 'installed SoapySDR.dll = built SoapySDR.dll' $instDll 'the installer carries a different SoapySDR.dll'
        $missing = @('msvcp140.dll', 'vcruntime140.dll', 'vcruntime140_1.dll', 'LICENSE.txt', 'THIRD-PARTY-LICENSES.txt', 'POSTINSTALL.txt', 'unins000.exe') | Where-Object { -not (Test-Path -LiteralPath (Join-Path $installDir $_)) }
        Assert-Check (@($missing).Count -eq 0) 'installed payload files' 'CRT, licences, notes, uninstaller' ('missing: ' + (@($missing) -join ', '))
        $plansSrc = @(Get-ChildItem -LiteralPath (Join-Path $RC.Repo 'resources\bandplans') -Filter *.json -File).Count
        $plansDst = @(Get-ChildItem -LiteralPath (Join-Path $installDir 'resources\bandplans') -Filter *.json -File -ErrorAction SilentlyContinue).Count
        Assert-Check ($plansSrc -gt 0 -and $plansDst -eq $plansSrc) 'installed band plans' ($plansDst.ToString() + ' of ' + $plansSrc) 'the overlay is inert without its plans'
        $uk = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\{' + $guid + '}_is1'
        Assert-Check (Test-Path -LiteralPath $uk) 'uninstall registry key (per user)' $uk 'Add/Remove Programs would not list it'
        $dv = (Get-ItemProperty -LiteralPath $uk).DisplayVersion
        Assert-Check ($dv -eq $v) 'uninstall entry DisplayVersion' $dv ('expected ' + $v)
        $sub = Get-PeSubsystem $exe
        Write-Result 'INFO' 'installed exe subsystem' ($sub + ' (the checks below read redirected pipes, which works for either)')

        $vr = Invoke-Exe -File $exe -Arguments @('--version') -TimeoutSec 60 -Env $env1
        $vl = ($vr.StdOut -split "`r?`n" | Where-Object { $_.Trim() -ne '' } | Select-Object -First 1)
        if ($null -eq $vl) { $vl = '' }
        Assert-Check ($vr.ExitCode -eq 0 -and $vl.Trim() -eq ('FoxSDR ' + $v)) 'installed cascade.exe --version' ('"' + $vl.Trim() + '" exit ' + $vr.ExitCode) ('expected "FoxSDR ' + $v + '"')
        $sr = Invoke-Exe -File $exe -Arguments @('--selftest') -TimeoutSec 120 -Env $env1
        $sline = ($sr.Output -split "`r?`n" | Where-Object { $_ -match '^selftest ' } | Select-Object -Last 1)
        if ($null -eq $sline) { $sline = '' }
        Assert-Check ($sr.ExitCode -eq 0 -and $sline -match '^selftest PASS') 'installed cascade.exe --selftest' ($sline + ' exit ' + $sr.ExitCode) 'the headless DSP chain did not produce the expected spectrum peak and sidetone'
        $fr = Invoke-Exe -File $exe -Arguments @('--frames', '60') -TimeoutSec 180 -Env $env1 -WorkDir $installDir
        Assert-Check ($fr.ExitCode -eq 0 -and -not $fr.TimedOut) 'installed cascade.exe --frames 60 exit code' ([string]$fr.ExitCode) ('timed out: ' + $fr.TimedOut)
        $rendered = [regex]::Match($fr.Output, 'rendered (\d+) frames')
        Assert-Check ($rendered.Success -and $rendered.Groups[1].Value -eq '60') '--frames 60 rendered' ($rendered.Value) 'the bounded run must say it rendered exactly 60 frames'
        $cfgAfter = [System.IO.File]::ReadAllText($env1['CASCADE_CONFIG_TEST'])
        Assert-Check ($cfgAfter -match '"telemetryEnabled"\s*:\s*false') 'test run left usage reporting off' 'scratch config still says telemetryEnabled false' 'the run would have sent a usage report'
        Assert-Check (-not (@(Get-ChildItem -LiteralPath $env1['LOCALAPPDATA'] -Filter 'telemetry-sent-*' -Recurse -ErrorAction SilentlyContinue).Count -gt 0)) 'no usage-sent marker from the test run' '' 'a telemetry-sent marker appeared under the scratch LOCALAPPDATA'
    } finally {
        if ($installed -and -not $KeepInstalled) {
            $unins = Join-Path $installDir 'unins000.exe'
            if (Test-Path -LiteralPath $unins) {
                $ur = Invoke-Exe -File $unins -Arguments @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', ('/LOG=' + (Join-Path $RC.Verify 'uninstall.log'))) -TimeoutSec 300
                # The uninstaller copies itself to TEMP and returns before the copy has
                # finished (jrsoftware.org/ishelp/topic_uninstexitcodes.htm), so the
                # directory is POLLED for rather than looked at once.
                Write-Result 'INFO' 'uninstaller exit code' ([string]$ur.ExitCode)
                $gone = Wait-ForGone $installDir 120
                Write-Result 'INFO' 'uninstall wait' ('{0:N1} s' -f $gone)
            }
        }
    }
    if ($KeepInstalled) {
        Write-Result 'WARN' 'installed copy kept (-KeepInstalled)' $installDir
        Set-StepDone 'install-test' @{} @{ kept = $true }
        return 'done'
    }
    Assert-Check ($null -ne $ur -and $ur.ExitCode -eq 0) 'uninstaller exit code' ($(if ($null -eq $ur) { 'unins000.exe was not found' } else { [string]$ur.ExitCode })) 'any non-zero exit means the uninstaller did not run to completion'
    $left = @(Get-InstalledState)
    $dirGone = -not (Test-Path -LiteralPath $installDir)
    if (-not $dirGone) {
        Write-Detail (((Get-ChildItem -LiteralPath $installDir -Recurse -ErrorAction SilentlyContinue | ForEach-Object { $_.FullName }) | Select-Object -First 10) -join "`n      ")
    }
    Assert-Check $dirGone 'install directory gone after uninstall' $installDir 'the uninstaller left files behind (a running cascade.exe holds them: close it and uninstall by hand)'
    Assert-Check ($left.Count -eq 0) 'uninstall registry keys gone' ('none in HKCU/HKLM') ('still present: ' + ($left -join ', '))
    Assert-Check (-not (Test-Path -LiteralPath 'HKCU:\Software\Classes\foxsdr')) 'foxsdr: protocol key gone' 'HKCU\Software\Classes\foxsdr removed' 'the uninstaller left the URL handler registered'
    Write-Result 'PASS' 'install-test' ('installed, ran, uninstalled; exe sha256 ' + $buildInfo['exeSha256'])
    Write-JsonFile (Join-Path $RC.Verify 'install-test.json') @{ head = $RC.Head; exeSha256 = $buildInfo['exeSha256']; installerSha256 = (Get-Sha256 $setup); when = (Get-Date).ToUniversalTime().ToString('o') }
    Set-StepDone 'install-test' @{ 'verify\install-test.json' = (Get-Sha256 (Join-Path $RC.Verify 'install-test.json')) } @{ exeSha256 = $buildInfo['exeSha256'] }
    return 'done'
}

# ---------------------------------------------------------------------------
# ci
# ---------------------------------------------------------------------------

function Get-CiRun {
    $gh = $script:Gh
    $fields = 'databaseId,status,conclusion,workflowName,headSha,event,url,createdAt,attempt'
    $r = Invoke-Exe -File $gh -Arguments @('run', 'list', '--commit', $script:RC.Head, '-R', $script:Cfg['repo'], '--json', $fields, '--limit', '30') -NoLog
    if ($r.ExitCode -ne 0) { Stop-Release ('gh run list failed: ' + $r.Output.Trim()) }
    $runs = @($r.StdOut | ConvertFrom-Json) | Where-Object { $_.workflowName -eq 'build' -and $_.headSha -eq $script:RC.Head }
    $runs = @($runs | Sort-Object createdAt -Descending)
    if ($runs.Count -eq 0) { return $null }
    return $runs[0]
}

function Get-CiJobs([string]$RunId) {
    $r = Invoke-Exe -File $script:Gh -Arguments @('run', 'view', $RunId, '-R', $script:Cfg['repo'], '--json', 'jobs,status,conclusion,url') -NoLog
    if ($r.ExitCode -ne 0) { Stop-Release ('gh run view failed: ' + $r.Output.Trim()) }
    return ($r.StdOut | ConvertFrom-Json)
}

function Step-Ci {
    $RC = $script:RC
    $v = $RC.Version
    $cfg = $script:Cfg
    if (-not $script:Ctx.DryRun -and -not $Force -and (Test-StepDone 'ci')) {
        Write-Result 'SKIP' 'ci' 'five release files in dist and the hashes in the notes are already done for this commit'
        return 'skipped'
    }
    $artifacts = @('foxsdr-linux-x64', 'foxsdr-linux-x64-appimage', 'foxsdr-linux-x64-symbols', 'foxsdr-linux-arm64', 'foxsdr-linux-arm64-appimage', 'foxsdr-linux-arm64-symbols')
    if ($script:Ctx.DryRun) {
        Write-Result 'DRY' 'find the run for HEAD' (Format-CommandLine 'gh' @('run', 'list', '--commit', $RC.Head, '-R', $cfg['repo'], '--json', 'databaseId,status,conclusion,workflowName,headSha,event,url,createdAt,attempt'))
        Write-Result 'DRY' 'wait for jobs' ('poll every 60 s: gh run view <id> -R ' + $cfg['repo'] + ' --json jobs,status,conclusion,url ; required: ' + ($RequiredCiJobs -join ', ') + ' ; give up after ' + $CiTimeoutMinutes + ' min')
        foreach ($a in $artifacts) { Write-Result 'DRY' ('download ' + $a) (Format-CommandLine 'gh' @('run', 'download', '<run-id>', '-R', $cfg['repo'], '-n', $a, '-D', (Join-Path $RC.Ci $a))) }
        Write-Plan 'check' ('every tarball and AppImage exists (' + (($RC.Files | Select-Object -Skip 1) -join ', ') + ') with a gzip or ELF header')
        Write-Plan 'copy' ('the four Linux files beside ' + $RC.InstallerName + ' in ' + $RC.Dist + ', then write dist\sha256.txt')
        Write-Plan 'edit' ('replace the five TBD lines in ' + $RC.NotesRel + ' with the five sha256 values, print git diff of that file')
        return 'done'
    }
    Assert-StepDone 'installer' 'ci'
    $run = $null
    if ($CiRunId -ne '') {
        $run = [pscustomobject]@{ databaseId = [int64]$CiRunId; url = ('https://github.com/' + $cfg['repo'] + '/actions/runs/' + $CiRunId) }
    } else {
        $run = Get-CiRun
        Assert-Check ($null -ne $run) ('CI run for ' + $RC.HeadShort) '' ('No GitHub Actions run named "build" exists for commit ' + $RC.Head + '. Push master first (a tag push triggers nothing), or pass -CiRunId.')
    }
    $runId = [string]$run.databaseId
    Write-Result 'PASS' 'CI run found' ($runId + ' ' + $run.url)

    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    while ($true) {
        $info = Get-CiJobs $runId
        $jobs = @($info.jobs)
        $summary = @($jobs | ForEach-Object { $_.name + '=' + $(if ($_.status -eq 'completed') { $_.conclusion } else { $_.status }) }) -join ' '
        $pending = @($RequiredCiJobs | Where-Object { $n = $_; $j = @($jobs | Where-Object { $_.name -eq $n }); $j.Count -eq 0 -or $j[0].status -ne 'completed' })
        Write-Result 'INFO' 'CI jobs' ($summary + ' (elapsed ' + $sw.Elapsed.ToString('hh\:mm\:ss') + ')')
        if ($pending.Count -eq 0) { break }
        if ($sw.Elapsed.TotalMinutes -ge $CiTimeoutMinutes) { Stop-Release ('CI did not finish its required jobs (' + ($pending -join ', ') + ') within ' + $CiTimeoutMinutes + ' min') }
        Start-Sleep -Seconds 60
    }
    $bad = @($RequiredCiJobs | Where-Object { $n = $_; $j = @($jobs | Where-Object { $_.name -eq $n }); $j[0].conclusion -ne 'success' })
    if ($bad.Count -gt 0) {
        Write-Detail ('To rerun only the failed jobs (hosted runners fail now and then with nothing wrong in the code): gh run rerun ' + $runId + ' --failed -R ' + $cfg['repo'])
        Assert-Check $false 'CI required jobs succeeded' ($bad -join ', ') ('see ' + $run.url)
    }
    Write-Result 'PASS' 'CI required jobs succeeded' (($RequiredCiJobs -join ', ') + ' on run ' + $runId)
    $others = @($jobs | Where-Object { $RequiredCiJobs -notcontains $_.name })
    foreach ($o in $others) { Write-Result 'INFO' ('CI job ' + $o.name + ' (not required)') ($(if ($o.status -eq 'completed') { $o.conclusion } else { $o.status })) }

    # One artifact at a time, each into its own folder: a single -n download is
    # extracted into -D WITHOUT the artifact-name folder, a multi-artifact download
    # WITH it, and the two layouts have been mistaken for each other. The 76 MB arm64
    # symbols artifact has timed out once and worked on a plain retry, so three tries.
    foreach ($a in $artifacts) {
        $dir = Join-Path $RC.Ci $a
        if ((Test-Path -LiteralPath $dir) -and (@(Get-ChildItem -LiteralPath $dir -Recurse -File).Count -gt 0)) {
            Write-Result 'SKIP' ('download ' + $a) 'already in ci\'
            continue
        }
        $ok = $false
        for ($try = 1; $try -le 3 -and -not $ok; $try++) {
            $d = Invoke-Exe -File $script:Gh -Arguments @('run', 'download', $runId, '-R', $cfg['repo'], '-n', $a, '-D', $dir) -TimeoutSec 1200 -Label ('download ' + $a)
            if ($d.ExitCode -eq 0 -and @(Get-ChildItem -LiteralPath $dir -Recurse -File -ErrorAction SilentlyContinue).Count -gt 0) { $ok = $true } else {
                Write-Result 'WARN' ('download ' + $a + ' attempt ' + $try + ' failed') ($d.Output.Trim() -replace '\s+', ' ')
                if (Test-Path -LiteralPath $dir) { Remove-Item -LiteralPath $dir -Recurse -Force }
                Start-Sleep -Seconds 20
            }
        }
        Assert-Check $ok ('download ' + $a) $dir 'three attempts failed'
        Write-Result 'PASS' ('download ' + $a) (@(Get-ChildItem -LiteralPath $dir -Recurse -File).Count.ToString() + ' file(s)')
    }

    # The four Linux release files, found where each artifact put them.
    $where = @{
        ('foxsdr-' + $v + '-linux-x64.tar.gz') = 'foxsdr-linux-x64'
        ('FoxSDR-' + $v + '-x86_64.AppImage') = 'foxsdr-linux-x64-appimage'
        ('foxsdr-' + $v + '-linux-arm64.tar.gz') = 'foxsdr-linux-arm64'
        ('FoxSDR-' + $v + '-aarch64.AppImage') = 'foxsdr-linux-arm64-appimage'
    }
    $hashes = @{}
    $setup = Join-Path $RC.Dist $RC.InstallerName
    $hashes[$RC.InstallerName] = Get-Sha256 $setup
    foreach ($name in $where.Keys) {
        $src = Join-Path (Join-Path $RC.Ci $where[$name]) $name
        Assert-Check (Test-Path -LiteralPath $src) ('CI file ' + $name) $src 'the artifact does not contain the expected file (is the CI run for this version?)'
        $head = New-Object byte[] 4
        $fs = [System.IO.File]::OpenRead($src)
        try { [void]$fs.Read($head, 0, 4) } finally { $fs.Dispose() }
        if ($name -like '*.tar.gz') {
            Assert-Check ($head[0] -eq 0x1F -and $head[1] -eq 0x8B) ('gzip header ' + $name) '1f 8b' 'not a gzip file'
        } else {
            Assert-Check ($head[0] -eq 0x7F -and $head[1] -eq 0x45 -and $head[2] -eq 0x4C -and $head[3] -eq 0x46) ('ELF header ' + $name) '7f 45 4c 46' 'not an ELF file'
        }
        $size = (Get-Item -LiteralPath $src).Length
        Assert-Check ($size -gt 1000000) ('size ' + $name) ('{0:N0} bytes' -f $size) 'under 1 MB cannot hold the application'
        $dst = Join-Path $RC.Dist $name
        if (-not (Test-Path -LiteralPath $dst) -or (Get-Sha256 $dst) -ne (Get-Sha256 $src)) { Copy-Item -LiteralPath $src -Destination $dst -Force }
        $hashes[$name] = Get-Sha256 $dst
    }
    $lines = @()
    foreach ($n in $RC.Files) { $lines += ($hashes[$n] + ' *' + $n) }
    [System.IO.File]::WriteAllText((Join-Path $RC.Dist 'sha256.txt'), (($lines -join "`n") + "`n"), (Get-Utf8NoBom))
    foreach ($n in $RC.Files) { Write-Result 'PASS' ('release file ' + $n) ('{0:N0} bytes, sha256 {1}' -f (Get-Item -LiteralPath (Join-Path $RC.Dist $n)).Length, $hashes[$n]) }

    # The one edit this script makes to the tree.
    $entries = @(Get-NotesEntries $RC.NotesPath)
    $realNow = @($entries | Where-Object { $_.Value -match '^[0-9a-f]{64}$' })
    if ($realNow.Count -eq 5) {
        foreach ($e in $entries) { Assert-Check ($e.Value -eq $hashes[$e.Name]) ('notes hash ' + $e.Name) $e.Value ('the notes already carry a DIFFERENT hash than the file in dist (' + $hashes[$e.Name] + ')') }
        Write-Result 'SKIP' 'notes hashes' 'already filled, and equal to the files'
    } else {
        Set-NotesHashes $RC.NotesPath $hashes
        $d = Invoke-Git @('diff', '--no-color', '--unified=0', '--', $RC.NotesRel)
        Write-Result 'PASS' ('notes written: ' + $RC.NotesRel) 'five hashes (the only edit this script makes to the tree)'
        foreach ($l in ($d.StdOut -split "`r?`n")) { if ($l -match '^[+-]' -and $l -notmatch '^(\+\+\+|---)') { Write-Detail $l } }
    }
    $s = Get-State
    $s['notesFilledSha256'] = (Get-Sha256 $RC.NotesPath)
    Save-State $s
    $outs = @{ 'dist\sha256.txt' = (Get-Sha256 (Join-Path $RC.Dist 'sha256.txt')) }
    foreach ($n in $RC.Files) { $outs['dist\' + $n] = $hashes[$n] }
    Set-StepDone 'ci' $outs @{ runId = $runId; runUrl = [string]$run.url }
    return 'done'
}

# ---------------------------------------------------------------------------
# symbols
# ---------------------------------------------------------------------------

function Add-NasIndexRow {
    param([string]$NasHost, [string]$Root, [string]$BuildId, [string]$Row, [string]$TempDir)
    $idx = $Root + '/index.txt'
    $q = ConvertTo-RemoteQuoted $idx
    if ($script:Ctx.DryRun) {
        Write-Result 'DRY' ('append index row for ' + $BuildId) ("ssh " + $NasHost + " 'cat >> " + $idx + "' < one row; first check grep -c -F " + $BuildId + ', last byte 0a, first byte not ef')
        return
    }
    $has = Invoke-Ssh -HostName $NasHost -Command ('grep -c -F -- ' + (ConvertTo-RemoteQuoted $BuildId) + ' ' + $q)
    if ($has.ExitCode -gt 1) { Stop-Release ('could not read ' + $idx + ' on ' + $NasHost + ': ' + $has.Output.Trim()) }
    $count = 0
    [void][int]::TryParse($has.StdOut.Trim(), [ref]$count)
    if ($count -ge 1) { Write-Result 'SKIP' ('NAS index row ' + $BuildId) ('already present (' + $count + ')'); return }
    # Never overwritten, only appended to, and only when the file ends in a newline: an
    # appended row on an unterminated last line would join the two.
    $last = Invoke-Ssh -HostName $NasHost -Command ('tail -c 1 ' + $q + ' | od -An -tx1')
    Assert-Check ($last.StdOut.Trim() -eq '0a') 'NAS index.txt ends with a newline' $last.StdOut.Trim() ($idx + ' on the NAS does not end with a newline; fix it by hand before appending')
    $first = Invoke-Ssh -HostName $NasHost -Command ('head -c 1 ' + $q + ' | od -An -tx1')
    Assert-Check ($first.StdOut.Trim() -ne 'ef') 'NAS index.txt has no BOM' $first.StdOut.Trim() ($idx + ' begins with a UTF-8 BOM, which makes an anchored parse drop its oldest row')
    $before = [int]((Invoke-Ssh -HostName $NasHost -Command ('wc -l < ' + $q)).StdOut.Trim())
    $tmp = Join-Path $TempDir ('index-row-' + $BuildId + '.txt')
    [System.IO.File]::WriteAllText($tmp, ($Row.TrimEnd("`r", "`n") + "`r`n"), (Get-Utf8NoBom))
    $app = Invoke-Ssh -HostName $NasHost -Command ('cat >> ' + $q) -InputFile $tmp
    Assert-Check ($app.ExitCode -eq 0) ('append index row ' + $BuildId) ('exit ' + $app.ExitCode) $app.Output.Trim()
    $after = [int]((Invoke-Ssh -HostName $NasHost -Command ('wc -l < ' + $q)).StdOut.Trim())
    $now = [int]((Invoke-Ssh -HostName $NasHost -Command ('grep -c -F -- ' + (ConvertTo-RemoteQuoted $BuildId) + ' ' + $q)).StdOut.Trim())
    Assert-Check ($after -eq $before + 1 -and $now -eq 1) ('NAS index row ' + $BuildId) ('rows ' + $before + ' -> ' + $after + ', build id appears ' + $now + ' time') 'the index did not grow by exactly one row'
    Remove-Item -LiteralPath $tmp -Force
}

function Step-Symbols {
    $RC = $script:RC
    $cfg = $script:Cfg
    if (-not $script:Ctx.DryRun -and -not $Force -and (Test-StepDone 'symbols')) {
        Write-Result 'SKIP' 'symbols' 'already mirrored for this commit (-Force re-verifies every file)'
        return 'skipped'
    }
    $nas = $cfg['nasHost']
    $root = $cfg['nasSymbolRoot']
    $pi = $cfg['piHost']
    $piDir = $cfg['piSymbolDir']
    $temp = Join-Path $RC.Verify 'tmp'
    if (-not $script:Ctx.DryRun) {
        Assert-StepDone 'build' 'symbols'
        Assert-StepDone 'ci' 'symbols'
        if (-not (Test-Path -LiteralPath $temp)) { New-Item -ItemType Directory -Path $temp -Force | Out-Null }
    }
    # --- what there is to send ----------------------------------------------------
    $items = @()      # @{ Local; Rel }  every file that goes to the NAS
    $maps = @()       # @{ Local; PiName }
    $rows = @()       # @{ BuildId; Row }
    $symJson = $null
    if (Test-Path -LiteralPath (Join-Path $RC.Verify 'symbols.json')) { $symJson = Read-JsonFile (Join-Path $RC.Verify 'symbols.json') }
    if ($null -ne $symJson) {
        $symRoot = Join-Path $RC.Repo 'symbols'
        foreach ($rel in @($symJson['files'].Keys)) {
            $items += @{ Local = (Join-Path $symRoot $rel); Rel = $rel.Replace('\', '/') }
            if ($rel -like '*symmap.json.gz') { $maps += @{ Local = (Join-Path $symRoot $rel); PiName = ('cascade-' + $symJson['buildId'] + '.json.gz') } }
        }
        $rows += @{ BuildId = $symJson['buildId']; Row = $symJson['indexRow'] }
    } elseif ($script:Ctx.DryRun) {
        Write-Plan 'Windows symbols' 'verify\symbols.json is written by the build step; its files (cascade.pdb, cascade.exe, symmap.json.gz) and index row go here'
    } else {
        Stop-Release 'verify\symbols.json is missing: run the build step'
    }
    foreach ($art in @('foxsdr-linux-x64-symbols', 'foxsdr-linux-arm64-symbols')) {
        $dir = Join-Path $RC.Ci $art
        if (-not (Test-Path -LiteralPath $dir)) {
            if ($script:Ctx.DryRun) { Write-Plan ('Linux symbols ' + $art) 'downloaded by the ci step: cascade.debug\<id>\{cascade.debug,symmap.json.gz}, cascade\<id>\cascade, one index row'; continue }
            Stop-Release ($dir + ' is missing: run the ci step')
        }
        $files = @(Get-ChildItem -LiteralPath $dir -Recurse -File)
        $idFile = $files | Where-Object { $_.Name -eq 'index.txt' } | Select-Object -First 1
        Assert-Check ($null -ne $idFile) ('index.txt in ' + $art) '' 'the symbols artifact carries its one-row index.txt'
        $row = ([System.IO.File]::ReadAllLines($idFile.FullName) | Where-Object { $_.Trim() -ne '' } | Select-Object -First 1)
        $cols = $row.Split("`t")
        Assert-Check ($cols.Count -ge 5) ('index row of ' + $art) $row 'expected the tab-separated row tools/archive-symbols-linux.sh writes'
        $id = $cols[4]
        $rows += @{ BuildId = $id; Row = $row }
        foreach ($f in $files) {
            if ($f.Name -eq 'index.txt') { continue }
            $rel = $f.FullName.Substring($dir.Length + 1).Replace('\', '/')
            $items += @{ Local = $f.FullName; Rel = $rel }
            if ($f.Name -eq 'symmap.json.gz') { $maps += @{ Local = $f.FullName; PiName = ('cascade-' + $id + '.linux.json.gz') } }
        }
    }
    # --- the NAS ----------------------------------------------------------------------
    if ($items.Count -gt 0 -or -not $script:Ctx.DryRun) {
        Write-Result 'INFO' 'symbols to mirror' ($items.Count.ToString() + ' files, ' + $rows.Count + ' index rows, ' + $maps.Count + ' maps for the Pi')
    }
    foreach ($it in $items) {
        if ($script:Ctx.DryRun -and -not (Test-Path -LiteralPath $it.Local)) { Write-Result 'DRY' ('send ' + $it.Rel + ' to ' + $nas) ("ssh " + $nas + " 'cat > " + $root + '/' + $it.Rel + ".part' < " + $it.Local); continue }
        [void](Send-RemoteFile -HostName $nas -LocalPath $it.Local -RemotePath ($root + '/' + $it.Rel) -MakeDirectory)
    }
    foreach ($r in $rows) { Add-NasIndexRow -NasHost $nas -Root $root -BuildId $r.BuildId -Row $r.Row -TempDir $temp }
    # --- the Pi: read once at the site's start-up, so putting them there before the swap is safe
    if ($script:Ctx.DryRun) { Write-Result 'DRY' 'check on the Pi' ('test -d ' + $piDir) }
    else {
        $td = Invoke-Ssh -HostName $pi -Command ('test -d ' + (ConvertTo-RemoteQuoted $piDir) + ' && echo ok')
        Assert-Check ($td.StdOut.Trim() -eq 'ok') ('Pi symbol directory ' + $piDir) $td.StdOut.Trim() 'the site reads its symbol maps from there'
    }
    foreach ($m in $maps) {
        if ($script:Ctx.DryRun -and -not (Test-Path -LiteralPath $m.Local)) { Write-Result 'DRY' ('send ' + $m.PiName + ' to the Pi') ("ssh <pi> 'cat > " + $piDir + '/' + $m.PiName + ".part' < " + $m.Local + ' ; chown foxsdr:foxsdr ; chmod 640 ; sha256sum ; mv'); continue }
        [void](Send-RemoteFile -HostName $pi -LocalPath $m.Local -RemotePath ($piDir + '/' + $m.PiName) -Owner 'foxsdr:foxsdr' -Mode '640')
    }
    if ($script:Ctx.DryRun) { return 'done' }
    Write-Result 'PASS' 'symbols' ($items.Count.ToString() + ' files on ' + $nas + ', ' + $rows.Count + ' index rows, ' + $maps.Count + ' maps on the Pi')
    Set-StepDone 'symbols' @{} @{ files = $items.Count; maps = $maps.Count }
    return 'done'
}

# ---------------------------------------------------------------------------
# github
# ---------------------------------------------------------------------------

function Get-ReleaseAssets {
    $r = Invoke-Exe -File $script:Gh -Arguments @('release', 'view', $script:RC.Tag, '-R', $script:Cfg['repo'], '--json', 'assets,isDraft,url,name') -NoLog
    if ($r.ExitCode -ne 0) { return $null }
    return ($r.StdOut | ConvertFrom-Json)
}

function Step-Github {
    $RC = $script:RC
    $v = $RC.Version
    $cfg = $script:Cfg
    $repo = $cfg['repo']
    $tag = $RC.Tag
    $notesOut = Join-Path $RC.Dist ('gh-notes-' + $v + '.md')
    $paths = @($RC.Files | ForEach-Object { Join-Path $RC.Dist $_ })
    $ident = @('-c', ('user.name=' + $GitUserName), '-c', ('user.email=' + $GitUserEmail))
    if ($script:Ctx.DryRun) {
        Write-Plan 'check' ('HEAD is contained in origin/master (the CI run and the tag are for a pushed commit)')
        Write-Result 'DRY' 'tag (if absent)' (Format-CommandLine 'git' (@('-C', $RC.Repo) + $ident + @('tag', '-a', $tag, '-m', ('FoxSDR ' + $v), $RC.Head)))
        Write-Result 'DRY' 'push the tag' (Format-CommandLine 'git' @('-C', $RC.Repo, 'push', 'origin', ('refs/tags/' + $tag)))
        Write-Plan 'write' ($notesOut + ' (the notes with the five real hashes, LF line endings)')
        Write-Result 'DRY' 'draft release' (Format-CommandLine 'gh' (@('release', 'create', $tag, '--verify-tag', '--draft', '--title', ('FoxSDR ' + $v), '--notes-file', $notesOut, '-R', $repo) + $paths))
        Write-Result 'DRY' 'compare digests' ((Format-CommandLine 'gh' @('release', 'view', $tag, '-R', $repo, '--json', 'assets,isDraft,url,name')) + '  (each asset: name, size, digest "sha256:<hex>" against dist)')
        Write-Result 'DRY' 'publish' (Format-CommandLine 'gh' @('release', 'edit', $tag, '-R', $repo, '--draft=false', '--latest'))
        return 'done'
    }
    if (-not $Force -and (Test-StepDone 'github')) {
        Write-Result 'SKIP' 'github' 'release already published for this commit'
        return 'skipped'
    }
    Assert-StepDone 'ci' 'github'
    # --- the commit must be one GitHub has ---------------------------------------------
    $cont = Invoke-Git @('branch', '-r', '--contains', $RC.Head)
    Assert-Check ($cont.StdOut -match 'origin/master') 'HEAD is on origin/master' ($cont.StdOut.Trim() -replace '\s+', ' ') 'push master first: CI builds the pushed commit and gh --verify-tag needs the tag on the remote'
    # --- the hashes in dist are the hashes in the notes ----------------------------------
    $entries = @(Get-NotesEntries $RC.NotesPath)
    $hashes = @{}
    foreach ($n in $RC.Files) { $hashes[$n] = Get-Sha256 (Join-Path $RC.Dist $n) }
    foreach ($e in $entries) { Assert-Check ($e.Value -eq $hashes[$e.Name]) ('notes hash ' + $e.Name) $e.Value ('differs from the file in dist (' + $hashes[$e.Name] + '); run the ci step') }
    $md = ([System.IO.File]::ReadAllText($RC.NotesPath)) -replace "`r`n", "`n"
    [System.IO.File]::WriteAllText($notesOut, $md, (Get-Utf8NoBom))
    Write-Result 'PASS' 'release text' ($notesOut + ' (' + $entries.Count + ' hashes, LF)')

    # --- the tag ------------------------------------------------------------------------
    $tagCommit = Get-TagCommit $tag
    if ($tagCommit -eq '') {
        $t = Invoke-Git -GitArgs @('tag', '-a', $tag, '-m', ('FoxSDR ' + $v), $RC.Head) -Identity $ident
        Assert-Check ($t.ExitCode -eq 0) ('tag ' + $tag) ('annotated on ' + $RC.HeadShort) $t.Output.Trim()
    } else {
        Assert-Check ($tagCommit -eq $RC.Head) ('tag ' + $tag) 'already on HEAD' ('the tag names ' + $tagCommit + ', not HEAD')
    }
    $remoteTag = Invoke-Git @('ls-remote', '--tags', 'origin', ('refs/tags/' + $tag))
    if ($remoteTag.StdOut.Trim() -eq '') {
        $pt = Invoke-Git @('push', 'origin', ('refs/tags/' + $tag))
        Assert-Check ($pt.ExitCode -eq 0) ('push tag ' + $tag) 'origin' $pt.Output.Trim()
    } else {
        Write-Result 'SKIP' ('push tag ' + $tag) 'origin already has it'
    }

    # --- the release ----------------------------------------------------------------------
    $existing = Get-ReleaseAssets
    if ($null -eq $existing) {
        $c = Invoke-Exe -File $script:Gh -Arguments (@('release', 'create', $tag, '--verify-tag', '--draft', '--title', ('FoxSDR ' + $v), '--notes-file', $notesOut, '-R', $repo) + $paths) -TimeoutSec 3600 -Label 'gh release create'
        Assert-Check ($c.ExitCode -eq 0) 'draft release created' ($c.StdOut.Trim()) $c.Output.Trim()
        $existing = Get-ReleaseAssets
        Assert-Check ($null -ne $existing) 'draft release readable' $tag 'gh release view found nothing after create'
    } else {
        Write-Result 'SKIP' 'create release' ('release ' + $tag + ' exists (draft: ' + $existing.isDraft + ')')
    }
    $byName = @{}
    foreach ($a in @($existing.assets)) { $byName[$a.name] = $a }
    foreach ($n in $RC.Files) {
        Assert-Check ($byName.ContainsKey($n)) ('GitHub asset ' + $n) '' 'the release has no asset of that name'
        $a = $byName[$n]
        $dg = ''
        if ($a.PSObject.Properties.Name -contains 'digest' -and $null -ne $a.digest) { $dg = [string]$a.digest }
        Assert-Check ($dg -ne '') ('GitHub digest of ' + $n) '' 'gh release view returned no digest for the asset, so the upload cannot be compared'
        $hex = $dg -replace '^sha256:', ''
        $size = (Get-Item -LiteralPath (Join-Path $RC.Dist $n)).Length
        Assert-Check ($hex -eq $hashes[$n] -and [int64]$a.size -eq $size) ('GitHub asset ' + $n) ('digest ' + $hex + ', ' + $a.size + ' bytes') ('local sha256 ' + $hashes[$n] + ', ' + $size + ' bytes')
    }
    if ($existing.isDraft) {
        $p = Invoke-Exe -File $script:Gh -Arguments @('release', 'edit', $tag, '-R', $repo, '--draft=false', '--latest')
        Assert-Check ($p.ExitCode -eq 0) 'release published as latest' ($p.StdOut.Trim()) $p.Output.Trim()
        $after = Get-ReleaseAssets
        Assert-Check (-not $after.isDraft) 'release is no longer a draft' $after.url ''
    } else {
        Write-Result 'SKIP' 'publish' 'already published'
    }
    Set-StepDone 'github' @{} @{ tag = $tag; url = [string]$existing.url }
    return 'done'
}

# ---------------------------------------------------------------------------
# store: the Microsoft Store submission API
# ---------------------------------------------------------------------------
#
# WHICH MECHANISM, AND WHY. Two routes reach the same service:
#   * the Microsoft Store Developer CLI (msstore, winget "Microsoft Store Developer CLI"):
#     learn.microsoft.com/windows/apps/publish/msstore-dev-cli/commands - `msstore
#     publish <path> -i <msix> -id <productId> -nc` replaces the package and leaves a
#     draft; `msstore submission get` then `submission update <id> <json>` is how the
#     What's new text and the mandatory flag are changed, with the WHOLE submission
#     JSON as one command-line argument; `submission publish` and `submission poll`
#     finish it. It needs the .NET 9 desktop runtime and keeps its credentials in its
#     own configuration (`msstore reconfigure --tenantId --sellerId --clientId
#     --clientSecret`).
#   * the Partner Center submission REST API, which the CLI calls:
#     learn.microsoft.com/windows/uwp/monetize/create-and-manage-submissions-using-windows-store-services
#     .../manage-app-submissions, .../create-an-app-submission, .../update-an-app-submission,
#     .../commit-an-app-submission, .../get-status-for-an-app-submission, .../get-app-data.
# This script calls the REST API directly. It does exactly what the five requirements
# need and nothing else, with no extra runtime and no credential store of its own:
#   (a) create a submission from the last published one  POST   /v1.0/my/applications/{id}/submissions
#   (b) replace the package       PUT same + zip uploaded to the returned fileUploadUrl (Azure Blob SAS)
#   (c) set the en-GB What's new  PUT body: listings["en-gb"].baseListing.releaseNotes
#   (d) set the mandatory flag    PUT body: packageDeliveryOptions.isMandatoryUpdate (+ mandatoryUpdateEffectiveDate)
#   (e) submit for certification  POST  .../submissions/{sid}/commit
#   (f) report certification      GET   .../submissions/{sid}/status   (status + statusDetails)
# Token: POST https://login.microsoftonline.com/<tenant>/oauth2/token with grant_type=
# client_credentials, client_id, client_secret, resource=https://manage.devcenter.microsoft.com.
#
# UNVERIFIED, AND THE ONE THING THAT MAY STOP THE WHOLE ROUTE: the page above says "This
# API cannot be used with apps or add-ons that use mandatory app updates ... the API will
# return a 409 error code ... you must use Partner Center". Every FoxSDR submission ticks
# "Make this update mandatory", and the same page's own data model documents
# packageDeliveryOptions.isMandatoryUpdate, so the two statements disagree and only a
# live call can say which is true for this app. A 409 from the first call is therefore
# handled, not treated as a defect: the step writes the Partner Center bundle (package,
# What's new text, the click-by-click list) and reports it as owner action. msstore
# calls the same API and would meet the same 409.

function Get-StoreCredentials {
    if (-not (Test-Path -LiteralPath $StoreCredentialsFile)) { return $null }
    $j = Read-JsonFile $StoreCredentialsFile
    if ($null -eq $j) { return $null }
    foreach ($k in 'tenantId', 'clientId', 'clientSecret', 'applicationId') {
        if (-not $j.ContainsKey($k) -or [string]::IsNullOrWhiteSpace([string]$j[$k])) { Stop-Release ($StoreCredentialsFile + ' has no "' + $k + '" (values are never printed)') }
        Register-Secret ([string]$j[$k])
    }
    return $j
}

function Show-StoreSetup {
    Write-Result 'ACTION' 'store credentials needed' ($StoreCredentialsFile + ' does not exist')
    $lines = @(
        'One-time setup in Partner Center (https://partner.microsoft.com/dashboard), about ten minutes:',
        '  1. The Partner Center account must be associated with a Microsoft Entra ID directory and you must be',
        '     a Global administrator of it (learn.microsoft.com/windows/apps/publish/partner-center/associate-azure-ad-with-partner-center).',
        '  2. Account settings > User management > the "Microsoft Entra applications" tab >',
        '     "Add Microsoft Entra application" > "Create Microsoft Entra application" (or add an existing one).',
        '     Give it the MANAGER role ("Roles applicable to developer programs").',
        '     (learn.microsoft.com/windows/apps/publish/partner-center/manage-azure-ad-applications-in-partner-center)',
        '  3. Click the application''s name: copy its Tenant ID and Client ID.',
        '  4. "Add new key": copy the Key NOW; it is shown once.',
        ('  5. Create ' + $StoreCredentialsFile + ' (it is outside the repository; nothing here prints it):'),
        ('       { "tenantId": "<guid>", "clientId": "<guid>", "clientSecret": "<key>", "applicationId": "' + $StoreProductId + '" }'),
        '     applicationId is the product''s Store ID (Partner Center > FoxSDR > Product identity).',
        '  6. Run:  tools\release.ps1 -Steps store            (checks everything, writes nothing)',
        '           tools\release.ps1 -Steps store -SubmitStore   (creates, uploads, commits)',
        'Documented limit: the API answers 409 for an app that uses mandatory app updates (see the comment',
        'above Get-StoreCredentials in this script). If it does, the step prepares the Partner Center bundle instead.'
    )
    foreach ($l in $lines) { Write-Detail $l }
}

function Write-StoreBundle([string]$WhatsNew, [string]$Why) {
    $RC = $script:RC
    if ($script:Ctx.DryRun) {
        Write-Plan 'write' ($RC.StoreDir + '\whats-new-' + $RC.Version + '.txt and MANUAL-' + $RC.Version + '.txt (the Partner Center fallback)')
        return
    }
    if (-not (Test-Path -LiteralPath $RC.StoreDir)) { New-Item -ItemType Directory -Path $RC.StoreDir -Force | Out-Null }
    $wn = Join-Path $RC.StoreDir ('whats-new-' + $RC.Version + '.txt')
    [System.IO.File]::WriteAllText($wn, $WhatsNew + "`r`n", (Get-Utf8NoBom))
    $msix = Join-Path $RC.Msix $RC.MsixName
    $man = @(
        ('Microsoft Store submission for FoxSDR ' + $RC.Version + ' by hand in Partner Center'),
        ('Reason: ' + $Why),
        '',
        ('Package: ' + $msix),
        ('  (unsigned; Identity ' + $script:StoreIdentityName + ', version ' + (Get-MsixVersion $RC.Version) + ')'),
        ('What''s new text: ' + $wn),
        '',
        ('In Partner Center, product FoxSDR (Store ID ' + $StoreProductId + '):'),
        '  1. Overview > Start update. This makes a draft from the last published submission.',
        '  2. Packages: drop the new .msix on the upload box; the old package goes away on Save.',
        '     Tick "Make this update mandatory" (owner standing instruction, 2026-10-06), choose the date and time.',
        '  3. Store listings > English (United Kingdom) > "What''s new in this version": paste the text file above.',
        '  4. Save, then "Submit for certification". Certification took a few hours both times on 2026-10-05.',
        '  5. When it is live: tools\release.ps1 -Steps store-status, then set StoreAppVersion with',
        '     site_bump.py --store <version> in the next site release.'
    )
    [System.IO.File]::WriteAllText((Join-Path $RC.StoreDir ('MANUAL-' + $RC.Version + '.txt')), (($man -join "`r`n") + "`r`n"), (Get-Utf8NoBom))
    Write-Result 'INFO' 'Partner Center bundle written' $RC.StoreDir
}

# The two Microsoft endpoints, and ONE test seam: FOXSDR_STORE_TEST_BASE=http://127.0.0.1:<port>
# points both at a fake server on this machine, which is how the rehearsal exercises the whole
# submission flow without a Partner Center account. Only a loopback address is accepted: an
# environment variable that could name any host would be a way to send the client secret
# somewhere the owner never chose.
function Get-StoreBases {
    $t = $env:FOXSDR_STORE_TEST_BASE
    if ($t) {
        if ($t -notmatch '^http://127\.0\.0\.1:\d+$') { Stop-Release 'FOXSDR_STORE_TEST_BASE may only be http://127.0.0.1:<port> (a test seam for a fake server on this machine)' }
        if (-not $script:StoreSeamAnnounced) {
            $script:StoreSeamAnnounced = $true
            Write-Result 'WARN' 'Store TEST SEAM in use' ($t + ' instead of Microsoft''s hosts')
        }
        return @{ Login = ($t + '/login/'); Api = ($t + '/api/') }
    }
    return @{ Login = 'https://login.microsoftonline.com/'; Api = 'https://manage.devcenter.microsoft.com/v1.0/my/' }
}

function Get-StoreToken($Creds) {
    $bases = Get-StoreBases
    $body = 'grant_type=client_credentials&client_id=' + [uri]::EscapeDataString([string]$Creds['clientId']) + '&client_secret=' + [uri]::EscapeDataString([string]$Creds['clientSecret']) + '&resource=' + [uri]::EscapeDataString('https://manage.devcenter.microsoft.com')
    $r = Invoke-Http -Method 'POST' -Url ($bases.Login + $Creds['tenantId'] + '/oauth2/token') -Body ([System.Text.Encoding]::UTF8.GetBytes($body)) -ContentType 'application/x-www-form-urlencoded; charset=utf-8'
    if ($r.Status -ne 200) {
        $short = $r.Text
        if ($short.Length -gt 400) { $short = $short.Substring(0, 400) }
        Stop-Release ('the Azure AD token request answered ' + $r.Status + ': ' + $short)
    }
    $tok = ($r.Text | ConvertFrom-Json).access_token
    Register-Secret $tok
    return $tok
}

function Invoke-StoreApi {
    param([string]$Method, [string]$Path, [string]$Token, [string]$JsonBody = '')
    $h = @{ Authorization = ('Bearer ' + $Token); 'User-Agent' = 'foxsdr-release' }
    $bytes = $null
    $ct = ''
    if ($JsonBody -ne '') { $bytes = [System.Text.Encoding]::UTF8.GetBytes($JsonBody); $ct = 'application/json; charset=utf-8' }
    $bases = Get-StoreBases
    $r = Invoke-Http -Method $Method -Url ($bases.Api + $Path) -Headers $h -Body $bytes -ContentType $ct -TimeoutSec 300
    Write-LogLine ($Method + ' ' + $Path + ' -> ' + $r.Status + ' (MS-CorrelationId ' + $r.Correlation + ')')
    return $r
}

# A copy of a dictionary/array graph made of plain .NET objects only. PowerShell wraps
# anything New-Object makes (an ArrayList, a Dictionary) in a PSObject, and a
# JavaScriptSerializer that meets one reflects over the wrapper's own members and stops
# with "A circular reference was detected ... PSParameterizedProperty" - found by the
# rehearsal's fake Store server, on the first PUT of the submission.
function ConvertTo-PlainGraph($Obj) {
    if ($null -eq $Obj) { return $null }
    $Obj = $Obj.psobject.BaseObject
    if ($Obj -is [string] -or $Obj -is [System.ValueType]) { return $Obj }
    # ::new(), not New-Object: the cmdlet's result is the wrapped kind.
    if ($Obj -is [System.Collections.IDictionary]) {
        $d = [System.Collections.Generic.Dictionary[string, object]]::new()
        foreach ($k in @($Obj.Keys)) {
            $v = ConvertTo-PlainGraph $Obj[$k]
            if ($null -ne $v) { $v = $v.psobject.BaseObject }
            $d.Add([string]$k, $v)
        }
        return , $d
    }
    if ($Obj -is [System.Collections.IEnumerable]) {
        $l = [System.Collections.Generic.List[object]]::new()
        foreach ($e in $Obj) {
            $v = ConvertTo-PlainGraph $e
            if ($null -ne $v) { $v = $v.psobject.BaseObject }
            $l.Add($v)
        }
        return , $l
    }
    return $Obj
}

function Step-Store {
    $RC = $script:RC
    $v = $RC.Version
    $msix = Join-Path $RC.Msix $RC.MsixName
    $whatsNewParas = @()
    $notesSrc = $RC.NotesPath
    if ($StoreNotesFile -ne '') { $notesSrc = $StoreNotesFile }
    if (Test-Path -LiteralPath $notesSrc) { $whatsNewParas = @(Get-ReleaseNotesPlain $notesSrc) }
    $whatsNew = ($whatsNewParas -join "`r`n`r`n")
    $pkgVer = Get-MsixVersion $v
    $creds = $null
    $credsFilePresent = Test-Path -LiteralPath $StoreCredentialsFile

    if ($script:Ctx.DryRun) {
        Write-Result 'INFO' 'store credentials file' ($(if ($credsFilePresent) { 'present (contents are never printed)' } else { 'ABSENT: a real run would stop here and print the Partner Center setup' }))
        if (-not $credsFilePresent) { Show-StoreSetup }
        Write-Result 'INFO' 'What''s new text' ($whatsNew.Length.ToString() + ' characters from ' + $notesSrc)
        Write-Result 'DRY' 'token' 'POST https://login.microsoftonline.com/<tenantId>/oauth2/token (client_credentials, resource https://manage.devcenter.microsoft.com; the secret is not logged)'
        Write-Result 'DRY' 'read the app' 'GET https://manage.devcenter.microsoft.com/v1.0/my/applications/<applicationId> (a pending submission stops the step unless -ReplacePendingStoreSubmission)'
        Write-Result 'DRY' 'read what the Store has' ('GET .../applications/<id>/submissions/<lastPublished>  (its package version must be older than ' + $pkgVer + ')')
        Write-Result 'DRY' 'create' 'POST .../applications/<id>/submissions   (a copy of the last published submission; 409 = the documented unsupported case)'
        Write-Result 'DRY' 'update' ('PUT .../submissions/<sid>: old package fileStatus PendingDelete, new ' + [System.IO.Path]::GetFileName($msix) + ' PendingUpload; listings[en-gb].baseListing.releaseNotes = the notes; packageDeliveryOptions.isMandatoryUpdate = true')
        Write-Result 'DRY' 'upload' ('zip ' + [System.IO.Path]::GetFileName($msix) + ' and PUT it to fileUploadUrl with x-ms-blob-type: BlockBlob (learn.microsoft.com/rest/api/storageservices/put-blob)')
        Write-Result 'DRY' 'commit' 'POST .../submissions/<sid>/commit, then GET .../status every 60 s while CommitStarted'
        Write-StoreBundle $whatsNew 'dry run'
        return 'done'
    }

    if (-not $credsFilePresent) {
        Show-StoreSetup
        Write-StoreBundle $whatsNew 'no Store API credentials file'
        return 'action'
    }
    Assert-StepDone 'msix' 'store'
    $creds = Get-StoreCredentials
    try {
        $state = Get-State
        $prior = $null
        if ($state['steps'].ContainsKey('store') -and $state['steps']['store']['head'] -eq $RC.Head) { $prior = $state['steps']['store']['info'] }
        $token = Get-StoreToken $creds
        Write-Result 'PASS' 'Azure AD token' 'obtained (60 minutes; not printed)'
        $appId = [string]$creds['applicationId']
        $app = Invoke-StoreApi 'GET' ('applications/' + $appId) $token
        Assert-Check ($app.Status -eq 200) 'read the app' ('HTTP ' + $app.Status) ($app.Text.Substring(0, [Math]::Min(400, $app.Text.Length)))
        $appObj = $app.Text | ConvertFrom-Json
        $pending = $null
        if ($appObj.PSObject.Properties.Name -contains 'pendingApplicationSubmission') { $pending = $appObj.pendingApplicationSubmission }
        $lastPub = $null
        if ($appObj.PSObject.Properties.Name -contains 'lastPublishedApplicationSubmission') { $lastPub = $appObj.lastPublishedApplicationSubmission }
        Write-Result 'PASS' 'app' ($appObj.primaryName + ' (' + $appObj.packageIdentityName + ')')

        # Resume: this release already has a committed submission, so only report it.
        if ($null -ne $prior -and $prior.ContainsKey('submissionId')) {
            $sid = [string]$prior['submissionId']
            $s = Invoke-StoreApi 'GET' ('applications/' + $appId + '/submissions/' + $sid + '/status') $token
            Assert-Check ($s.Status -eq 200) 'status of the committed submission' ('HTTP ' + $s.Status) $s.Text
            $so = $s.Text | ConvertFrom-Json
            Write-Result 'PASS' 'store submission status' ($so.status + ' (submission ' + $sid + ')')
            foreach ($e in @($so.statusDetails.errors)) { Write-Detail ('error ' + $e.code + ': ' + $e.details) }
            foreach ($e in @($so.statusDetails.warnings)) { Write-Detail ('warning ' + $e.code + ': ' + $e.details) }
            foreach ($rep in @($so.statusDetails.certificationReports)) { Write-Detail ('certification report ' + $rep.date + ': ' + $rep.reportUrl) }
            Assert-Check (@('CommitFailed', 'PreProcessingFailed', 'CertificationFailed', 'ReleaseFailed', 'PublishFailed', 'Canceled') -notcontains $so.status) 'submission has not failed' $so.status 'read the statusDetails above and the certification report'
            return 'done'
        }

        $lastVersion = ''
        if ($null -ne $lastPub) {
            $ls = Invoke-StoreApi 'GET' ('applications/' + $appId + '/submissions/' + $lastPub.id) $token
            if ($ls.Status -eq 200) {
                $lo = $ls.Text | ConvertFrom-Json
                $vers = @($lo.applicationPackages | ForEach-Object { $_.version } | Where-Object { $_ })
                if ($vers.Count -gt 0) { $lastVersion = [string]$vers[0] }
            }
        }
        Write-Result 'INFO' 'Store has' ($(if ($lastVersion -ne '') { 'package ' + $lastVersion } else { 'no readable published package version' }))
        if ($lastVersion -ne '') {
            Assert-Check ([version]$pkgVer -gt [version]$lastVersion) 'new package is newer than the published one' ($pkgVer + ' > ' + $lastVersion) 'the Store serves the highest package version; a not-newer package would be rejected or would go nowhere'
        }
        if ($null -ne $pending -and $null -ne $pending.id) {
            if (-not $ReplacePendingStoreSubmission) {
                Assert-Check $false 'no pending Store submission' ('submission ' + $pending.id + ' is in progress') 'The Store keeps ONE in-progress submission. It may be your own draft from Partner Center; the API cannot safely edit it ("if you use Partner Center to change a submission that you originally created by using the API, you can no longer change or commit it by using the API", and the reverse). Submit or delete it in Partner Center, or pass -ReplacePendingStoreSubmission to delete it.'
            }
            $del = Invoke-StoreApi 'DELETE' ('applications/' + $appId + '/submissions/' + $pending.id) $token
            Assert-Check ($del.Status -ge 200 -and $del.Status -lt 300) 'pending submission deleted' ('HTTP ' + $del.Status) $del.Text
        }
        if (-not $SubmitStore) {
            Write-Result 'ACTION' 'ready to submit' ('package ' + $pkgVer + ', What''s new ' + $whatsNew.Length + ' characters, mandatory update on; nothing was written. Pass -SubmitStore to create, upload and commit.')
            return 'action'
        }

        # --- (a) create ---------------------------------------------------------------
        $create = Invoke-StoreApi 'POST' ('applications/' + $appId + '/submissions') $token
        if ($create.Status -eq 409) {
            Write-Result 'WARN' 'create submission refused' 'HTTP 409'
            Write-Detail 'The documented answer for "the current state of the app, or the app uses a Partner Center feature that is currently not supported by the Microsoft Store submission API" - which includes apps that use mandatory app updates (create-and-manage-submissions-using-windows-store-services#not_supported).'
            Write-StoreBundle $whatsNew 'the submission API answered 409 to the create call'
            return 'action'
        }
        Assert-Check ($create.Status -ge 200 -and $create.Status -lt 300) 'create submission' ('HTTP ' + $create.Status) ($create.Text.Substring(0, [Math]::Min(400, $create.Text.Length)))
        Add-Type -AssemblyName System.Web.Extensions
        $ser = New-Object System.Web.Script.Serialization.JavaScriptSerializer
        $ser.MaxJsonLength = [int]::MaxValue
        $sub = $ser.DeserializeObject($create.Text)
        $sid = [string]$sub['id']
        $uploadUrl = [string]$sub['fileUploadUrl']
        Register-Secret $uploadUrl
        Write-Result 'PASS' 'create submission' ('submission ' + $sid + ' (a copy of the last published one)')
        $s0 = Get-State
        $s0['steps']['store'] = @{ head = $RC.Head; done = (Get-Date).ToUniversalTime().ToString('o'); outputs = @{}; info = @{ createdSubmissionId = $sid } }
        Save-State $s0

        # --- (b)(c)(d) the update body --------------------------------------------------
        $allowed = @('applicationCategory', 'pricing', 'visibility', 'targetPublishMode', 'targetPublishDate', 'listings', 'hardwarePreferences', 'automaticBackupEnabled', 'canInstallOnRemovableMedia', 'isGameDvrEnabled', 'gamingOptions', 'hasExternalInAppProducts', 'meetAccessibilityGuidelines', 'notesForCertification', 'applicationPackages', 'packageDeliveryOptions', 'enterpriseLicensing', 'allowMicrosoftDecideAppAvailabilityToFutureDeviceFamilies', 'allowTargetFutureDeviceFamilies', 'trailers')
        $body = New-Object 'System.Collections.Generic.Dictionary[string,object]'
        foreach ($k in $allowed) { if ($sub.ContainsKey($k)) { $body[$k] = $sub[$k] } }
        # The package being replaced is marked PendingDelete and the new one PendingUpload;
        # the other fields of the old package are left exactly as the Store returned them.
        $pkgs = New-Object System.Collections.ArrayList
        if ($body.ContainsKey('applicationPackages') -and $null -ne $body['applicationPackages']) {
            foreach ($p in @($body['applicationPackages'])) { $p['fileStatus'] = 'PendingDelete'; [void]$pkgs.Add($p) }
        }
        $newPkg = New-Object 'System.Collections.Generic.Dictionary[string,object]'
        $newPkg['fileName'] = $RC.MsixName
        $newPkg['fileStatus'] = 'PendingUpload'
        $newPkg['minimumDirectXVersion'] = 'None'
        $newPkg['minimumSystemRam'] = 'None'
        [void]$pkgs.Add($newPkg)
        $body['applicationPackages'] = $pkgs
        $gbKey = ''
        if ($body.ContainsKey('listings') -and $null -ne $body['listings']) {
            foreach ($k in @($body['listings'].Keys)) { if ($k.ToLowerInvariant() -eq 'en-gb') { $gbKey = $k } }
        }
        $haveKeys = ''
        if ($body.ContainsKey('listings') -and $null -ne $body['listings']) { $haveKeys = (@($body['listings'].Keys) -join ', ') }
        Assert-Check ($gbKey -ne '') 'en-GB listing present' $haveKeys 'the submission has no en-GB listing to put the What''s new text in; it is not invented here'
        $body['listings'][$gbKey]['baseListing']['releaseNotes'] = $whatsNew
        if (-not $body.ContainsKey('packageDeliveryOptions') -or $null -eq $body['packageDeliveryOptions']) {
            $body['packageDeliveryOptions'] = New-Object 'System.Collections.Generic.Dictionary[string,object]'
        }
        $body['packageDeliveryOptions']['isMandatoryUpdate'] = $true
        $eff = $StoreMandatoryEffective
        if ($eff -eq '') { $eff = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ss', [System.Globalization.CultureInfo]::InvariantCulture) + '.0000000Z' }
        $body['packageDeliveryOptions']['mandatoryUpdateEffectiveDate'] = $eff
        $json = $ser.Serialize((ConvertTo-PlainGraph $body))
        $upd = Invoke-StoreApi 'PUT' ('applications/' + $appId + '/submissions/' + $sid) $token $json
        Assert-Check ($upd.Status -ge 200 -and $upd.Status -lt 300) 'update submission' ('HTTP ' + $upd.Status) ($upd.Text.Substring(0, [Math]::Min(600, $upd.Text.Length)))
        Write-Result 'PASS' 'update submission' ('package ' + $RC.MsixName + ' PendingUpload, What''s new ' + $whatsNew.Length + ' chars on ' + $gbKey + ', mandatory update from ' + $eff)

        # --- upload the zip -----------------------------------------------------------------
        # ZipArchiveMode is in System.IO.Compression.dll, ZipFile in ...FileSystem.dll.
        Add-Type -AssemblyName System.IO.Compression
        Add-Type -AssemblyName System.IO.Compression.FileSystem
        $zipPath = Join-Path $RC.Msix ('FoxSDR-' + $v + '-store-upload.zip')
        if (Test-Path -LiteralPath $zipPath) { Remove-Item -LiteralPath $zipPath -Force }
        $z = [System.IO.Compression.ZipFile]::Open($zipPath, [System.IO.Compression.ZipArchiveMode]::Create)
        try { [void][System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($z, $msix, $RC.MsixName) } finally { $z.Dispose() }
        $put = Invoke-Http -Method 'PUT' -Url $uploadUrl -Headers @{ 'x-ms-blob-type' = 'BlockBlob'; 'x-ms-version' = '2019-12-12' } -InFile $zipPath -ContentType 'application/octet-stream' -TimeoutSec 1800
        Assert-Check ($put.Status -eq 201) 'upload package zip' ('HTTP ' + $put.Status + ' (Put Blob answers 201 Created)') ($put.Text.Substring(0, [Math]::Min(400, $put.Text.Length)))

        # --- (e) commit, then watch the commit finish ------------------------------------------
        $commit = Invoke-StoreApi 'POST' ('applications/' + $appId + '/submissions/' + $sid + '/commit') $token
        Assert-Check ($commit.Status -ge 200 -and $commit.Status -lt 300) 'commit submission' ('HTTP ' + $commit.Status) ($commit.Text.Substring(0, [Math]::Min(400, $commit.Text.Length)))
        $statusNow = ''
        for ($i = 0; $i -lt 90; $i++) {
            $s = Invoke-StoreApi 'GET' ('applications/' + $appId + '/submissions/' + $sid + '/status') $token
            if ($s.Status -ne 200) { Stop-Release ('status call answered ' + $s.Status + ': ' + $s.Text) }
            $so = $s.Text | ConvertFrom-Json
            $statusNow = [string]$so.status
            Write-Result 'INFO' 'store submission status' $statusNow
            if ($statusNow -ne 'CommitStarted') { break }
            Start-Sleep -Seconds 60
        }
        foreach ($e in @($so.statusDetails.errors)) { Write-Detail ('error ' + $e.code + ': ' + $e.details) }
        Assert-Check (@('PreProcessing', 'Certification', 'Release', 'Published', 'PendingPublication', 'Publishing') -contains $statusNow) 'commit accepted' $statusNow 'CommitFailed or an error status: read the statusDetails above'
        Write-Result 'PASS' 'store submission committed' ('submission ' + $sid + ', status ' + $statusNow + '. Certification took a few hours in 2026-10-05; check again with: tools\release.ps1 -Steps store')
        Set-StepDone 'store' @{} @{ submissionId = $sid; status = $statusNow; committed = (Get-Date).ToUniversalTime().ToString('o') }
        return 'done'
    } finally {
        $script:Ctx.Secrets.Clear()
    }
}

# ---------------------------------------------------------------------------
# store-status: what the public Store catalogue says
# ---------------------------------------------------------------------------

# The package Version field is four 16-bit numbers packed into one: 281900178669568 is
# 1.99.3.0 (the comment above StoreAppVersion in the site's version.go). The package major
# is the product major plus one, so 1.99.3.0 is FoxSDR 0.99.3.
function ConvertFrom-StorePackedVersion([string]$Packed) {
    $n = [uint64]$Packed
    $a = [int](($n -shr 48) -band 0xFFFF)
    $b = [int](($n -shr 32) -band 0xFFFF)
    $c = [int](($n -shr 16) -band 0xFFFF)
    $d = [int]($n -band 0xFFFF)
    return [pscustomobject]@{ Package = ('{0}.{1}.{2}.{3}' -f $a, $b, $c, $d); Product = ('{0}.{1}.{2}' -f ($a - 1), $b, $c) }
}

function Step-StoreStatus {
    $RC = $script:RC
    $url = 'https://displaycatalog.mp.microsoft.com/v7.0/products?bigIds=' + $StoreProductId + '&market=GB&languages=en-gb&MS-CV=chk'
    $site = $SiteDir
    if ($site -eq '') { $site = 'C:\Users\steve\foxsdr-wt\site-' + $RC.Version.Split('.')[2] }
    $versionGo = Join-Path $site 'version.go'
    Write-Result 'INFO' 'catalogue query (read-only, public, no credentials)' $url
    $r = Invoke-Http -Method 'GET' -Url $url -Headers @{ 'User-Agent' = 'foxsdr-release' } -TimeoutSec 60
    Assert-Check ($r.Status -eq 200) 'Store catalogue answered' ('HTTP ' + $r.Status) 'displaycatalog.mp.microsoft.com did not answer 200'
    $cat = $r.Text | ConvertFrom-Json
    $packed = @()
    # Products[].DisplaySkuAvailabilities[].Sku.Properties.Packages[].Version, found by
    # reading the live answer on 2026-10-06 (the field is a string of digits).
    foreach ($p in @($cat.Products)) {
        foreach ($sku in @($p.DisplaySkuAvailabilities)) {
            foreach ($pk in @($sku.Sku.Properties.Packages)) {
                if ($pk.PSObject.Properties.Name -contains 'Version' -and $null -ne $pk.Version) { $packed += [string]$pk.Version }
            }
        }
    }
    Assert-Check ($packed.Count -gt 0) 'catalogue lists a package' ($packed.Count.ToString() + ' package version(s)') 'no Products[].DisplaySkuAvailabilities[].Sku.Properties.Packages[].Version in the answer'
    $best = $null
    foreach ($pv in $packed) {
        $d = ConvertFrom-StorePackedVersion $pv
        if ($null -eq $best -or [version]$d.Package -gt [version]$best.Package) { $best = $d }
    }
    Write-Result 'PASS' 'Store is serving' ('package ' + $best.Package + ' = FoxSDR ' + $best.Product)
    $mod = ''
    foreach ($p in @($cat.Products)) { $mod = [string]$p.LastModifiedDate }
    Write-Result 'INFO' 'catalogue LastModifiedDate' $mod
    Write-Result 'INFO' 'this release' ($RC.Version + ' (package ' + (Get-MsixVersion $RC.Version) + ')')
    if (-not (Test-Path -LiteralPath $versionGo)) {
        Write-Result 'WARN' 'site StoreAppVersion not compared' ($versionGo + ' not found; pass -SiteDir <site worktree>')
        return 'done'
    }
    $vg = [System.IO.File]::ReadAllText($versionGo)
    $sm = [regex]::Match($vg, 'StoreAppVersion\s*=\s*"([^"]+)"')
    Assert-Check $sm.Success 'site version.go has StoreAppVersion' $versionGo ''
    if ($vg -notmatch [regex]::Escape($StoreProductId)) { Write-Result 'WARN' 'site comment' 'the catalogue command in version.go no longer names this Store ID' }
    if ($sm.Groups[1].Value -eq $best.Product) {
        Write-Result 'PASS' 'site StoreAppVersion is current' ('site says ' + $sm.Groups[1].Value + ', the Store serves ' + $best.Product)
    } else {
        Write-Result 'WARN' 'site StoreAppVersion is STALE' ('site says ' + $sm.Groups[1].Value + ', the Store serves ' + $best.Product + '; run the site bump with --store ' + $best.Product + ' (site-release.ps1 -StoreVersion ' + $best.Product + ')')
    }
    return 'done'
}

# ---------------------------------------------------------------------------
# The run
# ---------------------------------------------------------------------------

function Resolve-SelectedSteps {
    $requested = @()
    foreach ($s in $Steps) { foreach ($t in ($s -split ',')) { if ($t.Trim() -ne '') { $requested += $t.Trim().ToLowerInvariant() } } }
    if ($requested.Count -gt 0 -and $From -ne '') { throw 'RELEASE-FAIL: give -Steps or -From, not both' }
    if ($From -ne '') {
        $f = $From.Trim().ToLowerInvariant()
        if ($script:StepOrder -notcontains $f) { throw ('RELEASE-FAIL: unknown step "' + $f + '"; steps are ' + ($script:StepOrder -join ', ')) }
        $i = [array]::IndexOf($script:StepOrder, $f)
        return @($script:StepOrder[$i..($script:StepOrder.Count - 1)])
    }
    if ($requested.Count -eq 0) { return @($script:StepOrder) }
    foreach ($t in $requested) { if ($script:StepOrder -notcontains $t) { throw ('RELEASE-FAIL: unknown step "' + $t + '"; steps are ' + ($script:StepOrder -join ', ')) } }
    return @($script:StepOrder | Where-Object { $requested -contains $_ })
}

$exitCode = 0
try {
    $script:RC = Get-ReleaseContext
    $script:Gh = Find-Gh
    $script:Cmake = Find-Tool 'cmake' @('C:\Program Files\CMake\bin\cmake.exe')
    $script:Cfg = Get-ReleaseConfig $ConfigFile
    if ($Repo -ne '') { $script:Cfg['repo'] = $Repo }
    if ($PiHost -ne '') { $script:Cfg['piHost'] = $PiHost }
    if ($NasHost -ne '') { $script:Cfg['nasHost'] = $NasHost }
    $selected = Resolve-SelectedSteps
    $mode = 'RUN'
    if ($DryRun) { $mode = 'DRY RUN (nothing is changed outside the log folder)' }
    Write-Host ('FoxSDR release ' + $script:RC.Version + ' at ' + $script:RC.HeadShort + ' : ' + $mode) -ForegroundColor White
    Write-Host ('steps: ' + ($selected -join ', ') + '   folder: ' + $script:RC.ReleaseDir)

    # A release folder belongs to ONE commit: mixing the outputs of two would ship a
    # binary the symbols, the hashes and the tag do not describe.
    $st0 = Read-JsonFile $script:RC.StatePath
    if ($null -ne $st0 -and $st0.ContainsKey('head') -and $st0['head'] -ne $script:RC.Head) {
        $msg = ('the release folder belongs to commit ' + ([string]$st0['head']).Substring(0, 12) + ' but HEAD is ' + $script:RC.HeadShort + '; use a new -ReleaseRoot, or -Force to start over')
        if (-not $Force) { throw ('RELEASE-FAIL: ' + $msg) }
        Write-Host ('WARN  ' + $msg) -ForegroundColor Yellow
    }

    foreach ($step in $selected) {
        Write-Host ''
        Write-Host ('==== ' + $step) -ForegroundColor White
        Start-StepLog $script:RC.LogDir $step
        $outcome = ''
        switch ($step) {
            'preflight'    { $outcome = Step-Preflight }
            'build'        { $outcome = Step-Build }
            'installer'    { $outcome = Step-Installer }
            'msix'         { $outcome = Step-Msix }
            'install-test' { $outcome = Step-InstallTest }
            'ci'           { $outcome = Step-Ci }
            'symbols'      { $outcome = Step-Symbols }
            'github'       { $outcome = Step-Github }
            'store'        { $outcome = Step-Store }
            'store-status' { $outcome = Step-StoreStatus }
        }
        if ($DryRun -and $outcome -eq 'done') { $outcome = 'planned (dry run)' }
        [void]$script:Results.Add(@{ Step = $step; Outcome = $outcome })
        if ($outcome -eq 'action') { $exitCode = 2; break }
    }
} catch {
    $m = $_.Exception.Message
    if ($m -like 'RELEASE-FAIL:*') {
        if ($script:Ctx.LogPath -eq '') { Write-Host ('FAIL  ' + $m.Substring(14).Trim()) -ForegroundColor Red }
    } else {
        Write-Result 'FAIL' 'unexpected error' $m
        Write-Detail ($_.ScriptStackTrace -replace "`r?`n", ' | ')
    }
    $exitCode = 1
}

Write-Host ''
foreach ($r in $script:Results) { Write-Host ('{0,-14} {1}' -f $r.Step, $r.Outcome) }
if ($exitCode -eq 0) { Write-Host 'release.ps1: every selected step passed' -ForegroundColor Green }
elseif ($exitCode -eq 2) { Write-Host 'release.ps1: stopped for something only the owner can do (see the ACTION line above)' -ForegroundColor Magenta }
else { Write-Host 'release.ps1: STOPPED at the first FAIL (see the log under <release folder>\log)' -ForegroundColor Red }
exit $exitCode
