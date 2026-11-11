# Break-it check for the SDK template's test: each mutant changes ONE behaviour in a COPY of sdk\ (the real files are
# never touched), the copy is configured, built and run under CTest, and CTest must go RED. Then the unmodified copy must
# be GREEN. Scratch lives in -WorkDir (default: a folder in the temp directory, removed at the end).
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File sdk\tests\breakit_sdk.ps1 [-Generator "Visual Studio 17 2022"]
param(
    [string] $Generator = 'Visual Studio 17 2022',
    [string] $Platform = 'x64',
    [string] $Configuration = 'Release',
    [string] $WorkDir = ''
)
# Native tools write progress to stderr and ctest exits non-zero on purpose here: do not turn either into a terminating error.
$ErrorActionPreference = 'Continue'
$sdk = Split-Path -Parent $PSScriptRoot
$repo = Split-Path -Parent $sdk
if ($WorkDir -eq '') { $WorkDir = Join-Path ([IO.Path]::GetTempPath()) ('sdk-breakit-' + $PID) }
# Copy-Item into an existing folder nests the copy inside it, so start from nothing (this folder is the script's own scratch).
if (Test-Path -LiteralPath $WorkDir) { Remove-Item -LiteralPath $WorkDir -Recurse -Force }
New-Item -ItemType Directory -Force -Path $WorkDir | Out-Null
$utf8 = New-Object System.Text.UTF8Encoding($false)

$mutants = @(
    @{ N = 'a second is one sample late (counted <= rate)'; Old = 'if (s->counted < s->rate) {'; New = 'if (s->counted <= s->rate) {' },
    @{ N = 'no back-pressure: a new line overwrites the one not yet collected'; Old = 'if (s->pendingRead == s->pendingLen) {'; New = 'if (true) {' },
    @{ N = 'poll_text writes more than the room it was given'; Old = "    if (avail > cap) {`n        avail = cap;`n    }`n"; New = '' },
    @{ N = 'the capability entry lies about its table size'; Old = '{CASCADE_CAP_DECODER, static_cast<uint32_t>(sizeof(CascadeDecoderApi)), &kDecoder}'; New = '{CASCADE_CAP_DECODER, 0u, &kDecoder}' },
    @{ N = 'the entry point answers a host with another ABI'; Old = 'if (hostAbiVersion != CASCADE_PLUGIN_ABI_VERSION) {'; New = 'if (false) {' },
    @{ N = 'poll_text does not guard a NULL handle'; Old = 'if (s == nullptr || buf == nullptr || cap == 0u) {'; New = 'if (buf == nullptr || cap == 0u) {' },
    @{ N = 'the descriptor version no longer matches CMakeLists.txt'; Old = '"1.0.0",              // keep equal'; New = '"1.0.1",              // keep equal' },
    @{ N = 'the plugin prints to stderr (the sink scan must refuse it)'; Old = '    (void)samples;'; New = '    (void)samples; std::fprintf(stderr, "x");' },
    @{ N = 'the line number is off by one'; Old = 'std::snprintf(s->pending, sizeof(s->pending), "heartbeat %u\n", s->seconds);'; New = 'std::snprintf(s->pending, sizeof(s->pending), "heartbeat %u\n", s->seconds + 1u);' }
)

function Build-And-Test([string] $Dir, [string] $Label) {
    $b = Join-Path $Dir 'build'
    $cfg = & cmake -S $Dir -B $b -G $Generator -A $Platform "-DFOXSDR_PLUGIN_ABI_DIR=$(Join-Path $repo 'src\core')" 2>&1
    if ($LASTEXITCODE -ne 0) { return [pscustomobject]@{ Red = $true; Why = 'configure failed'; Tail = ($cfg | Select-Object -Last 3) -join ' | ' } }
    $bld = & cmake --build $b --config $Configuration --parallel 3 2>&1
    if ($LASTEXITCODE -ne 0) { return [pscustomobject]@{ Red = $true; Why = 'build failed'; Tail = ($bld | Where-Object { $_ -match 'error' } | Select-Object -First 1) } }
    $t = & ctest --test-dir $b -C $Configuration --timeout 90 2>&1
    $failed = @($t | Where-Object { $_ -match '\*\*\*Failed|\*\*\*Exception|\*\*\*Timeout|\*\*\*Not Run' })
    return [pscustomobject]@{ Red = ($LASTEXITCODE -ne 0); Why = $(if ($failed.Count -gt 0) { ($failed[0] -replace '^\s*\d+/\d+\s+Test\s+#\d+:\s*', '').Trim() } else { 'ctest exit ' + $LASTEXITCODE }); Tail = ($t | Select-Object -Last 3) -join ' | ' }
}

$survived = 0
$i = 0
foreach ($m in $mutants) {
    $i++
    $d = Join-Path $WorkDir ('m' + $i)
    Copy-Item -Recurse -LiteralPath $sdk -Destination $d -Exclude build
    $src = Join-Path $d 'src\heartbeat_decoder.cpp'
    $text = [IO.File]::ReadAllText($src).Replace("`r`n", "`n")
    $old = $m.Old.Replace("`r`n", "`n")
    if (-not $text.Contains($old)) { throw ('mutant anchor not found: ' + $m.N) }
    [IO.File]::WriteAllText($src, $text.Replace($old, $m.New), $utf8)
    $r = Build-And-Test $d $m.N
    if (-not $r.Red) { $survived++ }
    Write-Host ('{0,-72} {1,-9} {2}' -f $m.N, $(if ($r.Red) { 'RED' } else { 'SURVIVED' }), $r.Why)
}
$clean = Join-Path $WorkDir 'clean'
Copy-Item -Recurse -LiteralPath $sdk -Destination $clean -Exclude build
$c = Build-And-Test $clean 'unmodified'
Write-Host ('unmodified copy: ' + $(if ($c.Red) { 'RED (' + $c.Why + ')' } else { 'GREEN' }))
Remove-Item -LiteralPath $WorkDir -Recurse -Force -ErrorAction SilentlyContinue
Write-Host ($mutants.Count.ToString() + ' mutants, ' + $survived.ToString() + ' survived')
if ($survived -gt 0 -or $c.Red) { exit 1 }
exit 0
