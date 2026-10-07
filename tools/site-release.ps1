# The website half of a FoxSDR release, one NAMED STEP at a time: bump the site for the
# new application version, test it, build the Pi binary, stage everything on the Pi,
# swap, verify through Cloudflare.
#
#   powershell -ExecutionPolicy Bypass -File tools\site-release.ps1 -SiteDir <site worktree> `
#       -AppNotes <app>\docs\release-notes-0.99.69.md -NewSiteVersion 2.67.0 -DryRun
#   ... -Steps bump,test,build            (everything that stays on this machine)
#   ... -Steps stage                      (uploads to the Pi; the running site is not touched)
#   ... -Steps swap,verify                (the outage, about a minute, and its proof)
#
# WHERE THIS LIVES, AND WHY. It is in the application repository's tools\ folder beside
# release.ps1, not in the site's repository, because the two are one release: the files
# this script ships are the ones release.ps1 put in <release folder>\dist, it reads the
# application's release notes, and the person (or agent) running a release has this
# checkout open and not necessarily a site one. The site's own pieces it drives stay where
# they are: site_bump.py, fill-release.py, gh_notes.py and swap-site.sh live in
# C:\Users\steve\foxsdr-build\deploy (-DeployDir), because they are the site's release
# tools and swap-site.sh runs ON the Pi. The host is never written into this repository:
# see Get-ReleaseConfig in release-common.ps1.
#
# The steps, in order, and what each one CHECKS:
#
#   bump    site_bump.py (all OLD values read from the site's version.go; it refuses to run
#           twice), then fill-release.py from the five real files in <release>\dist, then
#           independent reads: the five hash constants equal the files' sha256, no PENDING
#           token is left, SiteVersion and AppVersion are the new ones. gh_notes.py only if
#           the application's notes still carry TBD hashes (release.ps1's ci step fills them).
#   test    go vet and go test -count=1 ./... (about four minutes; TestNoReleasePlaceholderRemains
#           fails by design until fill-release.py has run, which is why bump comes first).
#   build   CGO_ENABLED=0 GOOS=linux GOARCH=arm64 go build -trimpath -o foxsdr-site . and then
#           the result's own build settings (go version -m) and ELF header (aarch64).
#   stage   everything the new site needs, on the Pi, BEFORE the swap: the binary and
#           swap-site.sh in /opt/foxsdr-site/.deploy-<site>/, the five downloads into
#           downloads/, the symbol maps into the symbol directory; each by `cat >` and a
#           sha256 read back on the Pi. The site reads its downloads and its maps once, at
#           start-up, so they are in place before the new binary asks.
#   swap    swap-site.sh ON the Pi: keeps the running binary as foxsdr-site.prev-<old>,
#           restarts, waits for /healthz to answer as the new versions and rolls back if it
#           does not. THE SITE TAKES ABOUT 55 SECONDS TO START; a 40 second wait once rolled
#           back a healthy binary, so the wait is never below the script's own 180.
#   verify  through Cloudflare: /healthz, /api/update?v=<previous>, and all five /download/<name>
#           by sha256. It refuses to make a single request to a /download URL until the files
#           are known to be in place on the Pi (Cloudflare caches a 404).
#
# Every step is IDEMPOTENT and says SKIP when its outputs are already there. State is in
# <ReleaseRoot>\release-<app>\site-state.json; logs in <release>\log\site-<step>.log.
# -DryRun prints every command and remote action instead of running it; the only things
# that still run are read-only (git, file reads, ssh <host> true, and site_bump.py --check).
#
# EXIT CODES. 0 every selected step passed. 1 a check FAILED (the run stops there).
#
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

[CmdletBinding()]
param(
    [string[]] $Steps = @(),
    [string] $From = '',
    [switch] $DryRun,
    [switch] $Force,
    # The site worktree, at the commit the new site is to be built on.
    [Parameter(Mandatory = $true)][string] $SiteDir,
    [string] $NewSiteVersion = '',
    # The application's docs\release-notes-<version>.md (the version is taken from its name).
    [string] $AppNotes = '',
    [string] $AppVersion = '',
    # What /api/update?v=<this> is asked in verify. Defaults to the AppVersion the site had
    # before the bump (recorded by the bump step).
    [string] $PreviousAppVersion = '',
    # Only when the public Store catalogue now serves a newer one (release.ps1 -Steps store-status).
    [string] $StoreVersion = '',
    [string] $NotesDate = '',
    [string] $NotesSummary = '',
    [switch] $AllowDirty,
    [string] $ReleaseRoot = 'C:\Users\steve\foxsdr-build',
    [string] $DeployDir = 'C:\Users\steve\foxsdr-build\deploy',
    [string] $PiHost = '',
    [string] $ConfigFile = 'C:\Users\steve\foxsdr-build\deploy\release-config.json',
    [string] $SiteUrl = 'https://foxsdr.com',
    # The wait swap-site.sh makes for /healthz is TRIES x 2 s. 90 is the script's own default
    # (180 s); a smaller number is refused.
    [int] $SwapTries = 90,
    [int] $VerifyWaitMinutes = 5
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0
. (Join-Path $PSScriptRoot 'release-common.ps1')

$script:StepOrder = @('bump', 'test', 'build', 'stage', 'swap', 'verify')
$script:Results = New-Object System.Collections.ArrayList
$script:Ctx.DryRun = [bool]$DryRun

# ---------------------------------------------------------------------------
# What this site release is
# ---------------------------------------------------------------------------

function Get-VersionGoValue([string]$Text, [string]$Pattern) {
    $m = [regex]::Match($Text, $Pattern)
    if ($m.Success) { return $m.Groups[1].Value }
    return ''
}

function Get-SiteContext {
    $site = (Resolve-Path -LiteralPath $SiteDir).Path
    $vgPath = Join-Path $site 'version.go'
    if (-not (Test-Path -LiteralPath $vgPath)) { throw ('RELEASE-FAIL: no version.go in ' + $site + ' (-SiteDir must be the site worktree)') }
    $vg = [System.IO.File]::ReadAllText($vgPath)
    $siteNow = Get-VersionGoValue $vg '(?m)^\tSiteVersion = "(\d+\.\d+\.\d+)"'
    $appNow = Get-VersionGoValue $vg '(?m)^\tAppVersion = "(\d+\.\d+\.\d+)"'
    $storeNow = Get-VersionGoValue $vg '(?m)^\tStoreAppVersion = "(\d+\.\d+\.\d+)"'
    if ($siteNow -eq '' -or $appNow -eq '') { throw 'RELEASE-FAIL: version.go has no SiteVersion or AppVersion constant' }
    $app = $AppVersion
    if ($app -eq '' -and $AppNotes -ne '') {
        $m = [regex]::Match((Split-Path -Leaf $AppNotes), 'release-notes-(\d+\.\d+\.\d+)\.md$')
        if ($m.Success) { $app = $m.Groups[1].Value }
    }
    if ($app -eq '') { $app = $appNow }
    $newSite = $NewSiteVersion
    if ($newSite -eq '') { $newSite = $siteNow }
    $relDir = Join-Path $ReleaseRoot ('release-' + $app)
    $files = @(
        ('foxsdr-setup-' + $app + '.exe'),
        ('FoxSDR-' + $app + '-x86_64.AppImage'),
        ('foxsdr-' + $app + '-linux-x64.tar.gz'),
        ('FoxSDR-' + $app + '-aarch64.AppImage'),
        ('foxsdr-' + $app + '-linux-arm64.tar.gz')
    )
    $notes = $AppNotes
    if ($notes -eq '') { $notes = Join-Path (Resolve-Path (Join-Path $PSScriptRoot '..')).Path ('docs\release-notes-' + $app + '.md') }
    return @{
        Site = $site
        VersionGo = $vgPath
        SiteNow = $siteNow
        AppNow = $appNow
        StoreNow = $storeNow
        App = $app
        NewSite = $newSite
        ReleaseDir = $relDir
        Dist = (Join-Path $relDir 'dist')
        Verify = (Join-Path $relDir 'verify')
        LogDir = (Join-Path $relDir 'log')
        Out = (Join-Path $relDir 'site')
        StatePath = (Join-Path $relDir 'site-state.json')
        Files = $files
        Notes = $notes
        HasGit = (Test-Path -LiteralPath (Join-Path $site '.git'))
        Py = 'py'
    }
}

function Get-State {
    $s = Read-JsonFile $script:SX.StatePath
    if ($null -eq $s) { $s = @{ app = $script:SX.App; site = $script:SX.NewSite; steps = @{} } }
    if (-not $s.ContainsKey('steps')) { $s['steps'] = @{} }
    return $s
}

function Save-State($State) {
    if ($script:Ctx.DryRun) { return }
    if (-not (Test-Path -LiteralPath $script:SX.ReleaseDir)) { New-Item -ItemType Directory -Path $script:SX.ReleaseDir -Force | Out-Null }
    Write-JsonFile $script:SX.StatePath $State
}

function Set-StepDone([string]$Step, [hashtable]$Info) {
    if ($script:Ctx.DryRun) { return }
    $s = Get-State
    $s['app'] = $script:SX.App
    $s['site'] = $script:SX.NewSite
    $s['steps'][$Step] = @{ done = (Get-Date).ToUniversalTime().ToString('o'); info = $Info }
    Save-State $s
}

function Get-StepInfo([string]$Step) {
    $s = Get-State
    if (-not $s['steps'].ContainsKey($Step)) { return $null }
    return $s['steps'][$Step]['info']
}

function Assert-StepDone([string]$Needed, [string]$ForStep) {
    if ($null -ne (Get-StepInfo $Needed)) { return }
    if ($script:Ctx.DryRun) { Write-Result 'INFO' ($ForStep + ' needs step ' + $Needed) 'not done yet (a real run stops here)'; return }
    Stop-Release ('step ' + $ForStep + ' needs step ' + $Needed + ' to have run (tools\site-release.ps1 -Steps ' + $Needed + ' ...)')
}

# A hash of every file in the site tree except .git and the built binary: what `build`
# must be building is what `test` tested.
function Get-TreeFingerprint([string]$Dir) {
    $root = (Resolve-Path -LiteralPath $Dir).Path
    $sb = New-Object System.Text.StringBuilder
    $files = Get-ChildItem -LiteralPath $root -Recurse -File -Force | Where-Object { $_.FullName -notmatch '\\\.git(\\|$)' -and $_.Name -notlike 'foxsdr-site*' } | Sort-Object FullName
    foreach ($f in $files) { [void]$sb.Append($f.FullName.Substring($root.Length) + ':' + (Get-Sha256 $f.FullName) + "`n") }
    $sha = [System.Security.Cryptography.SHA256]::Create()
    $h = $sha.ComputeHash([System.Text.Encoding]::UTF8.GetBytes($sb.ToString()))
    return (([System.BitConverter]::ToString($h)).Replace('-', '').ToLowerInvariant())
}

function Invoke-SiteGit([string[]]$GitArgs) {
    $git = Find-Tool 'git' @('C:\Program Files\Git\cmd\git.exe')
    return (Invoke-Exe -File $git -Arguments (@('-C', $script:SX.Site) + $GitArgs) -NoLog)
}

# ---------------------------------------------------------------------------
# bump
# ---------------------------------------------------------------------------

function Get-DistHashes {
    $h = @{}
    foreach ($n in $script:SX.Files) {
        $p = Join-Path $script:SX.Dist $n
        if (Test-Path -LiteralPath $p) { $h[$n] = Get-Sha256 $p }
    }
    return $h
}

function Step-Bump {
    $SX = $script:SX
    $siteBump = Join-Path $DeployDir 'site_bump.py'
    $fill = Join-Path $DeployDir 'fill-release.py'
    $ghNotes = Join-Path $DeployDir 'gh_notes.py'
    foreach ($p in @($siteBump, $fill, $ghNotes)) { Assert-Check (Test-Path -LiteralPath $p) ('tool ' + (Split-Path -Leaf $p)) $p 'the site release tools live in -DeployDir' }
    Assert-Check ($NewSiteVersion -ne '') 'new site version' $NewSiteVersion 'pass -NewSiteVersion (the next minor, e.g. 2.67.0)'
    Assert-Check (Test-Path -LiteralPath $SX.Notes) 'application release notes' $SX.Notes 'pass -AppNotes <app>\docs\release-notes-<version>.md'
    $applied = ($SX.SiteNow -eq $NewSiteVersion -and $SX.AppNow -eq $SX.App)

    if ($SX.HasGit) {
        $st = Invoke-SiteGit @('status', '--porcelain=v1')
        $dirty = @($st.StdOut -split "`r?`n" | Where-Object { $_ -ne '' })
        $head = (Invoke-SiteGit @('log', '-1', '--format=%h %s')).StdOut.Trim()
        Write-Result 'INFO' 'site worktree' ($SX.Site + ' at ' + $head)
        if ($dirty.Count -eq 0 -or $applied) {
            Write-Result 'PASS' 'site worktree state' ($(if ($applied) { 'bump already applied; edits expected' } else { 'clean before the bump' }))
        } elseif ($AllowDirty) {
            Write-Result 'WARN' 'site worktree not clean (allowed by -AllowDirty)' ($dirty.Count.ToString() + ' path(s)')
        } else {
            Assert-Check $false 'site worktree clean before the bump' ($dirty.Count.ToString() + ' path(s)') 'start from a clean checkout of the site at the commit to build on (or -AllowDirty)'
        }
    } else {
        Write-Result 'WARN' 'site directory is not a git checkout' ($SX.Site + ' (tree-clean check and git diff skipped; a rehearsal copy?)')
    }

    $dist = Get-DistHashes
    $haveAll = ($dist.Count -eq 5)
    if (-not $DryRun -and -not $haveAll) {
        Assert-Check $false 'the five release files are in dist' ($dist.Count.ToString() + ' of 5 in ' + $SX.Dist) 'run tools\release.ps1 through the ci step first'
    }
    if ($haveAll) { Write-Result 'PASS' 'the five release files are in dist' $SX.Dist } else { Write-Result 'INFO' 'dist is incomplete' ($dist.Count.ToString() + ' of 5 in ' + $SX.Dist + ' (fill-release.py below would stop)') }

    $date = $NotesDate
    if ($date -eq '') { $date = (Get-Date).ToString('yyyy-MM-dd') }
    $bumpArgs = @($siteBump, '--new-site', $NewSiteVersion, '--notes', $SX.Notes, '--date', $date, '--site-dir', $SX.Site)
    if ($StoreVersion -ne '') { $bumpArgs = @($bumpArgs + @('--store', $StoreVersion)) }
    if ($NotesSummary -ne '') { $bumpArgs = @($bumpArgs + @('--summary', $NotesSummary)) }
    $pyArgs = @('-3.14') + $bumpArgs

    if ($applied) {
        Write-Result 'SKIP' 'site_bump.py' ('version.go already says site ' + $SX.SiteNow + ' and application ' + $SX.AppNow + ' (it would refuse a second run)')
    } elseif ($DryRun) {
        # --check makes every check site_bump.py makes and writes nothing, so it is safe
        # to run even for a dry run, and it shows what the real run would change.
        $r = Invoke-Exe -File $SX.Py -Arguments ($pyArgs + @('--check')) -TimeoutSec 120
        Write-Result 'DRY' 'site_bump.py (run with --check, read-only)' (Format-CommandLine $SX.Py $pyArgs)
        foreach ($l in ($r.Output -split "`r?`n")) { if ($l.Trim() -ne '') { Write-Detail $l } }
        Assert-Check ($r.ExitCode -eq 0) 'site_bump.py --check' ('exit ' + $r.ExitCode) 'the bump would be refused; read the message above'
    } else {
        $r = Invoke-Exe -File $SX.Py -Arguments $pyArgs -TimeoutSec 300 -WorkDir $SX.Site
        foreach ($l in ($r.Output -split "`r?`n")) { if ($l.Trim() -ne '') { Write-Detail $l } }
        Assert-Check ($r.ExitCode -eq 0 -and $r.Output -match 'wrote \d+ files') 'site_bump.py' ('exit ' + $r.ExitCode) 'site_bump.py refused or failed; nothing was written (it stages every file first)'
        $info = @{ oldSite = $SX.SiteNow; oldApp = $SX.AppNow; oldStore = $SX.StoreNow; newSite = $NewSiteVersion; newApp = $SX.App }
        $s = Get-State
        $s['bumpFrom'] = $info
        Save-State $s
    }

    $vgNow = ''
    if (Test-Path -LiteralPath $SX.VersionGo) { $vgNow = [System.IO.File]::ReadAllText($SX.VersionGo) }
    $pending = ($vgNow -match 'PENDING-[0-9.]+-(SHA256|SIZE)')
    if ($DryRun) {
        Write-Result 'DRY' 'fill-release.py' (Format-CommandLine $SX.Py @('-3.14', $fill, '--dir', $SX.Dist))
    } elseif ($pending) {
        $f = Invoke-Exe -File $SX.Py -Arguments @('-3.14', $fill, '--dir', $SX.Dist) -TimeoutSec 120 -WorkDir $SX.Site
        foreach ($l in ($f.Output -split "`r?`n")) { if ($l.Trim() -ne '') { Write-Detail $l } }
        Assert-Check ($f.ExitCode -eq 0) 'fill-release.py' ('exit ' + $f.ExitCode) 'it writes nothing unless every replacement succeeded exactly once'
    } else {
        Write-Result 'SKIP' 'fill-release.py' 'no PENDING token in version.go'
    }

    # gh_notes.py writes the GitHub release text with the five hashes from the notes' TBD
    # lines. release.ps1's ci step already wrote the hashes into the notes and its github
    # step writes dist\gh-notes-<v>.md from them, so it only has a job here when the notes
    # still say TBD.
    $notesText = [System.IO.File]::ReadAllText($SX.Notes)
    $ghOut = Join-Path $SX.Dist ('gh-notes-' + $SX.App + '.md')
    if ($notesText -match '`TBD`') {
        $g = Invoke-Planned -What 'gh_notes.py' -File $SX.Py -Arguments @('-3.14', $ghNotes, $SX.Notes, $SX.Dist, $ghOut) -TimeoutSec 60
        if ($null -ne $g) { Assert-Check ($g.ExitCode -eq 0) 'gh_notes.py' ($g.Output.Trim()) 'the notes need five TBD blocks and the five files in dist' }
    } else {
        Write-Result 'SKIP' 'gh_notes.py' 'the application notes already carry real hashes (release.ps1 github writes the GitHub text)'
    }
    if ($DryRun) { Write-Plan 'check' 'the five hash constants in version.go equal the sha256 of the five files in dist; no PENDING token is left anywhere; SiteVersion and AppVersion are the new ones'; return 'done' }

    # --- independent reads: the script that edited the files is not the one that vouches for them ---
    $vg = [System.IO.File]::ReadAllText($SX.VersionGo)
    Assert-Check ($vg -match ('(?m)^\tSiteVersion = "' + [regex]::Escape($NewSiteVersion) + '"')) 'version.go SiteVersion' $NewSiteVersion ''
    Assert-Check ($vg -match ('(?m)^\tAppVersion = "' + [regex]::Escape($SX.App) + '"')) 'version.go AppVersion' $SX.App ''
    $consts = @{ 'InstallerSHA256' = $SX.Files[0]; 'LinuxAppImageSHA256' = $SX.Files[1]; 'LinuxTarballSHA256' = $SX.Files[2]; 'PiAppImageSHA256' = $SX.Files[3]; 'PiTarballSHA256' = $SX.Files[4] }
    $dist = Get-DistHashes
    foreach ($c in ($consts.Keys | Sort-Object)) {
        $m = [regex]::Match($vg, '(?m)^\t' + $c + '\s*=\s*"([^"]*)"')
        Assert-Check ($m.Success -and $m.Groups[1].Value -eq $dist[$consts[$c]]) ('version.go ' + $c) $m.Groups[1].Value ('differs from the sha256 of ' + $consts[$c] + ' in dist (' + $dist[$consts[$c]] + ')')
    }
    $index = [System.IO.File]::ReadAllText((Join-Path $SX.Site 'web\index.html'))
    foreach ($n in $SX.Files) { Assert-Check ($index.Contains($dist[$n])) ('web/index.html names the hash of ' + $n) $dist[$n] 'the page would publish a checksum that is not the file''s' }
    # The same scope and pattern as the site's own TestNoReleasePlaceholderRemains
    # (placeholder_test.go): every .go file, everything under web/ and i18n/.
    $scan = @(Get-ChildItem -LiteralPath $SX.Site -Recurse -File -Force -Filter *.go)
    foreach ($sub in 'web', 'i18n') {
        $d = Join-Path $SX.Site $sub
        if (Test-Path -LiteralPath $d) { $scan += @(Get-ChildItem -LiteralPath $d -Recurse -File -Force) }
    }
    $left = @($scan | Where-Object { $_.FullName -notmatch '\\\.git(\\|$)' } | Select-String -Pattern 'PENDING-[0-9A-Za-z.]+-(SHA256|SIZE)' -CaseSensitive -List)
    Assert-Check ($left.Count -eq 0) 'no PENDING token left' ($left.Count.ToString() + ' file(s)') (($left | ForEach-Object { $_.Path }) -join ', ')
    if ($SX.HasGit) {
        $ds = Invoke-SiteGit @('diff', '--stat')
        Write-Result 'INFO' 'git diff --stat of the site' ''
        foreach ($l in ($ds.StdOut -split "`r?`n")) { if ($l.Trim() -ne '') { Write-Detail $l } }
    }
    Write-Result 'PASS' 'bump' ('site ' + $NewSiteVersion + ', application ' + $SX.App + ' (version.go, the pages, README.md, notes.go and the hashes)')
    Set-StepDone 'bump' @{ newSite = $NewSiteVersion; newApp = $SX.App }
    return 'done'
}

# ---------------------------------------------------------------------------
# test, build
# ---------------------------------------------------------------------------

function Step-Test {
    $SX = $script:SX
    $go = Find-Go
    Assert-Check ($go -ne '') 'tool: go' $go 'Go is expected at C:\Program Files\Go\bin\go.exe'
    $gv = Invoke-Exe -File $go -Arguments @('version') -NoLog
    Write-Result 'PASS' 'tool: go version' $gv.StdOut.Trim()
    if ($DryRun) {
        Write-Result 'DRY' 'go vet' ((Format-CommandLine $go @('vet', './...')) + '  (in ' + $SX.Site + ')')
        Write-Result 'DRY' 'go test' ((Format-CommandLine $go @('test', '-count=1', './...')) + '  (about four minutes)')
        return 'done'
    }
    $info = Get-StepInfo 'test'
    if (-not $Force -and $null -ne $info -and $info['fingerprint'] -eq (Get-TreeFingerprint $SX.Site)) {
        Write-Result 'SKIP' 'test' 'already passed on exactly this tree'
        return 'skipped'
    }
    Assert-StepDone 'bump' 'test'
    $v = Invoke-Exe -File $go -Arguments @('vet', './...') -WorkDir $SX.Site -TimeoutSec 900 -Label 'go vet'
    Assert-Check ($v.ExitCode -eq 0 -and $v.Output.Trim() -eq '') 'go vet ./...' ('exit ' + $v.ExitCode + ', ' + $(if ($v.Output.Trim() -eq '') { 'no findings' } else { 'findings' })) $v.Output.Trim()
    $t = Invoke-Exe -File $go -Arguments @('test', '-count=1', './...') -WorkDir $SX.Site -TimeoutSec 2400 -Label 'go test' -HeartbeatSec 60
    $ok = @($t.Output -split "`r?`n" | Where-Object { $_ -match '^ok\s' })
    Assert-Check ($t.ExitCode -eq 0 -and $ok.Count -ge 1) 'go test -count=1 ./...' ('exit ' + $t.ExitCode + ', ' + (($ok | Select-Object -First 1) -replace '\s+', ' ')) ($t.Output.Substring([Math]::Max(0, $t.Output.Length - 1500)))
    Write-Result 'PASS' 'site tests' (($ok | Select-Object -First 1) -replace '\s+', ' ')
    Set-StepDone 'test' @{ fingerprint = (Get-TreeFingerprint $SX.Site) }
    return 'done'
}

function Step-Build {
    $SX = $script:SX
    $go = Find-Go
    $out = Join-Path $SX.Out 'foxsdr-site'
    $env2 = @{ CGO_ENABLED = '0'; GOOS = 'linux'; GOARCH = 'arm64' }
    $goArgs = @('build', '-trimpath', '-o', $out, '.')
    if ($DryRun) {
        Write-Result 'DRY' 'go build (the exact line the running binary was built with)' ('CGO_ENABLED=0 GOOS=linux GOARCH=arm64 ' + (Format-CommandLine $go $goArgs) + '  (in ' + $SX.Site + ')')
        Write-Plan 'check' 'go version -m shows CGO_ENABLED=0, GOARCH=arm64, -trimpath=true; the file starts with an aarch64 ELF header'
        return 'done'
    }
    Assert-StepDone 'test' 'build'
    $test = Get-StepInfo 'test'
    $fp = Get-TreeFingerprint $SX.Site
    Assert-Check ($test['fingerprint'] -eq $fp) 'the tree is the one that was tested' $fp.Substring(0, 16) 'the site tree changed after go test passed; run the test step again'
    $prev = Get-StepInfo 'build'
    if (-not $Force -and $null -ne $prev -and (Test-Path -LiteralPath $out) -and (Get-Sha256 $out) -eq $prev['sha256'] -and $prev['fingerprint'] -eq $fp) {
        Write-Result 'SKIP' 'build' ('already built: ' + $prev['sha256'])
        return 'skipped'
    }
    if (-not (Test-Path -LiteralPath $SX.Out)) { New-Item -ItemType Directory -Path $SX.Out -Force | Out-Null }
    $b = Invoke-Exe -File $go -Arguments $goArgs -Env $env2 -WorkDir $SX.Site -TimeoutSec 900 -Label 'go build'
    Assert-Check ($b.ExitCode -eq 0) 'go build exit code' ([string]$b.ExitCode) $b.Output.Trim()
    $size = (Get-Item -LiteralPath $out).Length
    Assert-Check ($size -gt 5000000) 'binary size' ('{0:N0} bytes' -f $size) 'a site binary under 5 MB cannot embed the pages'
    $head = New-Object byte[] 20
    $fs = [System.IO.File]::OpenRead($out)
    try { [void]$fs.Read($head, 0, 20) } finally { $fs.Dispose() }
    $machine = [int]$head[18] + 256 * [int]$head[19]
    Assert-Check ($head[0] -eq 0x7F -and $head[1] -eq 0x45 -and $head[2] -eq 0x4C -and $head[3] -eq 0x46 -and $head[4] -eq 2 -and $machine -eq 183) 'ELF header' ('64-bit ELF, machine ' + $machine + ' (183 = aarch64)') 'the Pi runs a linux/arm64 binary'
    $mi = Invoke-Exe -File $go -Arguments @('version', '-m', $out) -NoLog
    $settings = @{ 'CGO_ENABLED' = '0'; 'GOARCH' = 'arm64'; 'GOOS' = 'linux'; '-trimpath' = 'true' }
    foreach ($k in $settings.Keys) {
        $m = [regex]::Match($mi.Output, '(?m)^\s*build\s+' + [regex]::Escape($k) + '=(\S+)')
        Assert-Check ($m.Success -and $m.Groups[1].Value -eq $settings[$k]) ('build setting ' + $k) $m.Groups[1].Value ('expected ' + $settings[$k] + ' (go version -m)')
    }
    $sha = Get-Sha256 $out
    [System.IO.File]::WriteAllText($out + '.sha256', $sha + "`n", (Get-Utf8NoBom))
    Write-Result 'PASS' 'foxsdr-site' ('{0:N0} bytes, sha256 {1}' -f $size, $sha)
    Set-StepDone 'build' @{ sha256 = $sha; size = $size; fingerprint = $fp }
    return 'done'
}

# ---------------------------------------------------------------------------
# stage, swap
# ---------------------------------------------------------------------------

function Get-SymbolMaps {
    $maps = @()
    $symJson = Read-JsonFile (Join-Path $script:SX.Verify 'symbols.json')
    if ($null -ne $symJson) {
        $symRoot = Join-Path (Resolve-Path (Join-Path $PSScriptRoot '..')).Path 'symbols'
        foreach ($rel in @($symJson['files'].Keys)) {
            if ($rel -like '*symmap.json.gz') { $maps += @{ Local = (Join-Path $symRoot $rel); Name = ('cascade-' + $symJson['buildId'] + '.json.gz') } }
        }
    }
    foreach ($art in @('foxsdr-linux-x64-symbols', 'foxsdr-linux-arm64-symbols')) {
        $dir = Join-Path (Join-Path $script:SX.ReleaseDir 'ci') $art
        if (Test-Path -LiteralPath $dir) {
            foreach ($f in @(Get-ChildItem -LiteralPath $dir -Recurse -File -Filter symmap.json.gz)) {
                $maps += @{ Local = $f.FullName; Name = ('cascade-' + $f.Directory.Name + '.linux.json.gz') }
            }
        }
    }
    return $maps
}

function Step-Stage {
    $SX = $script:SX
    $cfg = $script:Cfg
    $pi = $cfg['piHost']
    $root = $cfg['piSiteRoot']
    $stage = $root + '/.deploy-' + $SX.NewSite
    $bin = Join-Path $SX.Out 'foxsdr-site'
    $swapLocal = Join-Path $DeployDir 'swap-site.sh'
    if (-not $DryRun) {
        Assert-StepDone 'build' 'stage'
        Assert-Check (Test-Path -LiteralPath $bin) 'built binary' $bin 'run the build step'
        Assert-Check ($pi -ne '') 'Pi configured' 'piHost' 'give -PiHost, set FOXSDR_PI_HOST, or fill in release-config.json'
        $dist = Get-DistHashes
        Assert-Check ($dist.Count -eq 5) 'the five release files are in dist' ($dist.Count.ToString() + ' of 5') 'run tools\release.ps1 through the ci step'
        $true1 = Invoke-Ssh -HostName $pi -Command 'true'
        Assert-Check ($true1.ExitCode -eq 0) ('ssh ' + $pi + ' true') ('exit ' + $true1.ExitCode) $true1.Output.Trim()
        # The root volume was 68% full with 4.5 GB free on 2026-10-06 and every swap leaves
        # a 27 MB foxsdr-site.prev-<old> behind; say so rather than fill it.
        $df = Invoke-Ssh -HostName $pi -Command ('df -Pk ' + (ConvertTo-RemoteQuoted $root) + ' | tail -1 | awk ''{print $4}''')
        $availKb = 0
        [void][int64]::TryParse($df.StdOut.Trim(), [ref]$availKb)
        $needKb = [int64](((Get-Item -LiteralPath $bin).Length * 2 + ($SX.Files | ForEach-Object { (Get-Item -LiteralPath (Join-Path $SX.Dist $_)).Length } | Measure-Object -Sum).Sum * 2) / 1024) + 51200
        Assert-Check ($availKb -gt $needKb) 'room on the Pi' (('{0:N0} MB free, about {1:N0} MB needed' -f ($availKb / 1024), ($needKb / 1024))) 'prune old foxsdr-site.prev-* binaries (about 27 MB each) on the Pi'
    } else {
        Write-Result 'DRY' 'check the Pi' ('ssh ' + $pi + ' true ; df -Pk ' + $root + ' (room for the binary and the downloads)')
    }
    if ($DryRun) { Write-Result 'DRY' 'directory' ('ssh ' + $pi + ' mkdir -p ' + $stage) }
    if (-not $DryRun) {
        $mk = Invoke-Ssh -HostName $pi -Command ('mkdir -p ' + (ConvertTo-RemoteQuoted $stage))
        Assert-Check ($mk.ExitCode -eq 0) ('staging directory ' + $stage) ('exit ' + $mk.ExitCode) $mk.Output.Trim()
    }
    $binLocalSha = ''
    if (Test-Path -LiteralPath $bin) { $binLocalSha = Get-Sha256 $bin }
    if ($DryRun -and -not (Test-Path -LiteralPath $bin)) {
        Write-Result 'DRY' 'send the binary' ('ssh ' + $pi + " 'cat > " + $stage + "/foxsdr-site.new.part' < " + $bin + ' ; sha256sum ; mv')
    } else {
        [void](Send-RemoteFile -HostName $pi -LocalPath $bin -RemotePath ($stage + '/foxsdr-site.new') -ReplaceExisting)
    }
    if ((Test-Path -LiteralPath $swapLocal)) {
        [void](Send-RemoteFile -HostName $pi -LocalPath $swapLocal -RemotePath ($stage + '/swap-site.sh') -Mode '755' -ReplaceExisting)
    } else {
        Assert-Check $false 'swap-site.sh' $swapLocal 'the on-Pi swap script is expected in -DeployDir'
    }
    # The five downloads: staged, then installed into downloads/ (owner foxsdr, mode 644), then
    # read back in place. One that is already there with the same hash is left alone; one there
    # with a DIFFERENT hash stops the run, because a published file is never replaced silently.
    foreach ($n in $SX.Files) {
        $local = Join-Path $SX.Dist $n
        $final = $root + '/downloads/' + $n
        if ($DryRun -and -not (Test-Path -LiteralPath $local)) {
            Write-Result 'DRY' ('stage ' + $n) ('ssh ' + $pi + " 'cat > " + $stage + '/' + $n + ".part' < " + $local + ' ; sha256sum ; mv ; install -o foxsdr -g foxsdr -m 644 ' + $stage + '/' + $n + ' ' + $final + ' ; sha256sum ' + $final)
            continue
        }
        $want = Get-Sha256 $local
        if ($DryRun) {
            Write-Result 'DRY' ('stage ' + $n) ('upload to ' + $stage + '/' + $n + ', sha256 ' + $want + ', then install -o foxsdr -g foxsdr -m 644 into ' + $root + '/downloads/')
            continue
        }
        $have = Get-RemoteSha256 -HostName $pi -Path $final
        if ($have -eq $want) { Write-Result 'SKIP' ('download ' + $n) 'already in downloads/ with the same sha256'; continue }
        if ($have -ne '') { Stop-Release ($final + ' on the Pi has a different sha256 (' + $have + ') than the file in dist (' + $want + '); a published download is not replaced silently') }
        [void](Send-RemoteFile -HostName $pi -LocalPath $local -RemotePath ($stage + '/' + $n))
        $inst = Invoke-Ssh -HostName $pi -Command ('install -o foxsdr -g foxsdr -m 644 ' + (ConvertTo-RemoteQuoted ($stage + '/' + $n)) + ' ' + (ConvertTo-RemoteQuoted $final) + ' && rm -f ' + (ConvertTo-RemoteQuoted ($stage + '/' + $n)))
        Assert-Check ($inst.ExitCode -eq 0) ('install ' + $n) ('exit ' + $inst.ExitCode) $inst.Output.Trim()
        $in2 = Get-RemoteSha256 -HostName $pi -Path $final
        Assert-Check ($in2 -eq $want) ('download in place: ' + $n) ('sha256 ' + $in2) ('expected ' + $want)
    }
    # The symbol maps: normally put there by release.ps1's symbols step; whatever is missing is sent now.
    foreach ($m in @(Get-SymbolMaps)) {
        [void](Send-RemoteFile -HostName $pi -LocalPath $m.Local -RemotePath ($cfg['piSymbolDir'] + '/' + $m.Name) -Owner 'foxsdr:foxsdr' -Mode '640')
    }
    if ($DryRun) { return 'done' }
    Write-Result 'PASS' 'stage' ('binary, swap-site.sh, five downloads and the symbol maps are on the Pi (' + $stage + ')')
    Set-StepDone 'stage' @{ stage = $stage; binarySha256 = $binLocalSha }
    return 'done'
}

function Step-Swap {
    $SX = $script:SX
    $cfg = $script:Cfg
    $pi = $cfg['piHost']
    $root = $cfg['piSiteRoot']
    $stage = $root + '/.deploy-' + $SX.NewSite
    if ($SwapTries -lt 90) { Stop-Release ('-SwapTries ' + $SwapTries + ' would wait ' + ($SwapTries * 2) + ' s for a site that takes about 55 s to start AND must be allowed three minutes; the minimum is 90') }
    $binSha = ''
    $bin = Join-Path $SX.Out 'foxsdr-site'
    if (Test-Path -LiteralPath $bin) { $binSha = Get-Sha256 $bin }
    $shaShown = $binSha
    if ($shaShown -eq '') { $shaShown = '<sha256 of the built foxsdr-site>' }
    $cmd = 'SWAP_TRIES=' + $SwapTries + ' bash ' + (ConvertTo-RemoteQuoted ($stage + '/swap-site.sh')) + ' ' + $SX.NewSite + ' ' + $SX.App + ' ' + $shaShown
    if ($DryRun) {
        Write-Result 'DRY' 'read the running site' ('ssh ' + $pi + ' curl -s http://127.0.0.1:8090/healthz')
        Write-Result 'DRY' ('swap (runs ON the Pi; keeps foxsdr-site.prev-<old>, restarts, waits up to ' + ($SwapTries * 2) + ' s for /healthz, rolls back if it does not)') ('ssh ' + $pi + ' ' + $cmd)
        Write-Plan 'check' 'exit 0 and a "SWAPPED:" or "already running" line; any "ROLLING BACK" or "STOPPED" is a FAIL with the script''s own output'
        return 'done'
    }
    Assert-StepDone 'stage' 'swap'
    $before = Invoke-Ssh -HostName $pi -Command 'curl -s -m 5 http://127.0.0.1:8090/healthz'
    Write-Result 'INFO' 'running site before the swap' ($before.StdOut.Trim())
    $r = Invoke-Ssh -HostName $pi -Command $cmd -TimeoutSec 1200 -Label 'swap-site.sh'
    foreach ($l in ($r.Output -split "`r?`n")) { if ($l.Trim() -ne '') { Write-Detail $l } }
    Assert-Check ($r.ExitCode -eq 0 -and $r.Output -match '(SWAPPED:|already running)') 'swap-site.sh' ('exit ' + $r.ExitCode) 'exit 1 is STOPPED (nothing swapped), exit 2 is ROLLING BACK (the old site is running again); the lines above are the script''s own'
    $m = [regex]::Match($r.Output, 'healthz: (.+)')
    Write-Result 'PASS' 'swap' ($(if ($m.Success) { $m.Groups[1].Value } else { 'site ' + $SX.NewSite + ' is running' }))
    Set-StepDone 'swap' @{ output = ($r.Output.Trim() -replace '\s+', ' ') }
    return 'done'
}

# ---------------------------------------------------------------------------
# verify, through Cloudflare
# ---------------------------------------------------------------------------

function Step-Verify {
    $SX = $script:SX
    $cfg = $script:Cfg
    $pi = $cfg['piHost']
    $root = $cfg['piSiteRoot']
    $ua = @{ 'User-Agent' = 'foxsdr-release-check' }
    $prev = $PreviousAppVersion
    if ($prev -eq '') {
        $s = Get-State
        if ($s.ContainsKey('bumpFrom')) { $prev = [string]$s['bumpFrom']['oldApp'] }
    }
    if ($DryRun) {
        Write-Result 'DRY' 'on the Pi first' ('ssh ' + $pi + ' sha256sum ' + $root + '/downloads/<each of the five> (no /download request is made before this passes)')
        Write-Result 'DRY' 'healthz' ('GET ' + $SiteUrl + '/healthz until it answers version ' + $SX.NewSite + ' and app ' + $SX.App + ' (up to ' + $VerifyWaitMinutes + ' min)')
        Write-Result 'DRY' 'update check' ('GET ' + $SiteUrl + '/api/update?v=' + $(if ($prev -ne '') { $prev } else { '<previous app version>' }) + ' : newer true, version ' + $SX.App + ', the installer''s sha256, size and URL')
        foreach ($n in $SX.Files) { Write-Result 'DRY' ('download ' + $n) ('GET ' + $SiteUrl + '/download/' + $n + ' -> sha256 equals the file in dist') }
        Write-Result 'DRY' 'download latest' ('GET ' + $SiteUrl + '/download/latest -> the installer')
        return 'done'
    }
    Assert-StepDone 'stage' 'verify'
    Assert-StepDone 'swap' 'verify'
    Assert-Check ($prev -ne '') 'previous application version' $prev 'pass -PreviousAppVersion (what /api/update?v= is asked as)'
    $dist = Get-DistHashes
    Assert-Check ($dist.Count -eq 5) 'the five release files are in dist' ($dist.Count.ToString() + ' of 5') ''
    # THE GATE. A /download/<name> request for a file that is not on the Pi yet is answered
    # 404 and Cloudflare caches the 404. So every file is read back on the Pi first.
    foreach ($n in $SX.Files) {
        $have = Get-RemoteSha256 -HostName $pi -Path ($root + '/downloads/' + $n)
        Assert-Check ($have -eq $dist[$n]) ('on the Pi: ' + $n) $have ('expected ' + $dist[$n] + '; no /download request is made until every file is in place')
    }
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $hz = $null
    while ($true) {
        $r = Invoke-Http -Method 'GET' -Url ($SiteUrl + '/healthz') -Headers $ua -TimeoutSec 30
        if ($r.Status -eq 200) {
            $j = $r.Text | ConvertFrom-Json
            if ($j.version -eq $SX.NewSite -and $j.app -eq $SX.App) { $hz = $j; break }
            Write-Result 'INFO' 'healthz' ('answers site ' + $j.version + ' app ' + $j.app + ' (waiting for ' + $SX.NewSite + ' / ' + $SX.App + ')')
        } else {
            Write-Result 'INFO' 'healthz' ('HTTP ' + $r.Status)
        }
        if ($sw.Elapsed.TotalMinutes -ge $VerifyWaitMinutes) { Assert-Check $false 'healthz through Cloudflare' ('no ' + $SX.NewSite + ' / ' + $SX.App + ' answer within ' + $VerifyWaitMinutes + ' min') '' }
        Start-Sleep -Seconds 10
    }
    Write-Result 'PASS' 'healthz through Cloudflare' ('site ' + $hz.version + ', app ' + $hz.app)
    $u = Invoke-Http -Method 'GET' -Url ($SiteUrl + '/api/update?v=' + $prev) -Headers $ua -TimeoutSec 30
    Assert-Check ($u.Status -eq 200) ('/api/update?v=' + $prev) ('HTTP ' + $u.Status) ''
    $uj = $u.Text | ConvertFrom-Json
    $installer = $SX.Files[0]
    $insize = (Get-Item -LiteralPath (Join-Path $SX.Dist $installer)).Length
    Assert-Check ($uj.newer -eq $true -and $uj.version -eq $SX.App) ('update answer for ' + $prev) ('newer ' + $uj.newer + ', version ' + $uj.version) ('expected newer true and version ' + $SX.App)
    Assert-Check ($uj.sha256 -eq $dist[$installer] -and [int64]$uj.size -eq $insize -and ([string]$uj.url).EndsWith('/' + $installer)) 'update answer names the installer' ($uj.url + ', ' + $uj.size + ' bytes') ('expected sha256 ' + $dist[$installer] + ' and ' + $insize + ' bytes')
    if (-not (Test-Path -LiteralPath $SX.Verify)) { New-Item -ItemType Directory -Path $SX.Verify -Force | Out-Null }
    $names = @($SX.Files + @('latest'))
    foreach ($n in $names) {
        $dest = Join-Path $SX.Verify ('site-' + $n)
        $d = Invoke-Http -Method 'GET' -Url ($SiteUrl + '/download/' + $n) -Headers $ua -OutFile $dest -TimeoutSec 900
        if ($d.Status -eq 404) { Assert-Check $false ('download ' + $n) 'HTTP 404' 'the file is on the Pi but Cloudflare answered 404: it may have cached one. Purge that URL in Cloudflare, then run verify again.' }
        Assert-Check ($d.Status -eq 200) ('download ' + $n) ('HTTP ' + $d.Status) ''
        $want = $dist[$n]
        if ($n -eq 'latest') { $want = $dist[$installer] }
        $got = Get-Sha256 $dest
        Assert-Check ($got -eq $want) ('download ' + $n) ('sha256 ' + $got + ', ' + ('{0:N0}' -f $d.Length) + ' bytes') ('expected ' + $want)
    }
    Write-Result 'PASS' 'verify' ('healthz, update answer and six downloads through ' + $SiteUrl)
    Set-StepDone 'verify' @{ site = $hz.version; app = $hz.app }
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
    $script:SX = Get-SiteContext
    $script:Cfg = Get-ReleaseConfig $ConfigFile
    if ($PiHost -ne '') { $script:Cfg['piHost'] = $PiHost }
    $selected = Resolve-SelectedSteps
    $mode = 'RUN'
    if ($DryRun) { $mode = 'DRY RUN (nothing is changed outside the log folder)' }
    Write-Host ('FoxSDR site release: site ' + $script:SX.NewSite + ' for application ' + $script:SX.App + ' : ' + $mode) -ForegroundColor White
    Write-Host ('steps: ' + ($selected -join ', ') + '   site: ' + $script:SX.Site + '   release folder: ' + $script:SX.ReleaseDir)
    foreach ($step in $selected) {
        Write-Host ''
        Write-Host ('==== ' + $step) -ForegroundColor White
        Start-StepLog $script:SX.LogDir ('site-' + $step)
        $outcome = ''
        switch ($step) {
            'bump'   { $outcome = Step-Bump }
            'test'   { $outcome = Step-Test }
            'build'  { $outcome = Step-Build }
            'stage'  { $outcome = Step-Stage }
            'swap'   { $outcome = Step-Swap }
            'verify' { $outcome = Step-Verify }
        }
        if ($DryRun -and $outcome -eq 'done') { $outcome = 'planned (dry run)' }
        [void]$script:Results.Add(@{ Step = $step; Outcome = $outcome })
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
foreach ($r in $script:Results) { Write-Host ('{0,-10} {1}' -f $r.Step, $r.Outcome) }
if ($exitCode -eq 0) { Write-Host 'site-release.ps1: every selected step passed' -ForegroundColor Green }
else { Write-Host 'site-release.ps1: STOPPED at the first FAIL (see the log under <release folder>\log)' -ForegroundColor Red }
exit $exitCode
