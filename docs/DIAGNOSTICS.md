# Diagnostics — crashes, freezes, and reading a report six months later

This describes phase 1, which is complete: FoxSDR captures faults **locally**
and uploads **nothing**. Phase 2 (a Cloudflare Worker that receives a minimal
report) is deliberately not built yet — publishing noise from a capture nobody
has trusted yet would only make the numbers harder to believe later.

## Why a crash handler was not enough on its own

Every fault this product has actually shipped was a **hang**, not a crash:

- a 120-second freeze while a CAT client was shut down,
- a map window that could not be closed,
- an audio stream that had died while every number on screen still read
  healthy.

A crash handler would have caught none of them. The process was alive and, from
its own point of view, fine. So there are two capture paths, and the second one
is the one that pays: `src/core/crash_handler.cpp` for a process that dies, and
`src/core/hang_watchdog.cpp` for one that stops moving.

## What is captured

| | Crash | Hang |
|---|---|---|
| Trigger | SEH filter, `std::terminate`, `SIGABRT`, purecall, CRT invalid parameter | GUI thread has not pumped for 5 s |
| Stack | the faulting thread, unwound from the exception context | **every** thread in the process |
| Written | `%LOCALAPPDATA%\FoxSDR\crashes\crash-*.txt` | `…\crashes\hang-*.txt` |
| Afterwards | the process dies as it always did (Windows Error Reporting still runs) | the application **carries on**, and the watchdog re-arms |

Both carry: exact version **and** git commit, the stack as `module+offset`, the
loaded module list with each module's **build id**, the last 256 log lines from
the in-memory ring, the application context (mode, source, sample rate, radio
model with the serial stripped, loaded plugins with versions), and a **stable
signature** for grouping.

A hang report captures every thread because *a deadlock is only legible as a
pair.* "The GUI thread is blocked" names no bug; "the GUI thread waits on the
CAT server's mutex while the CAT thread waits inside a socket close" is the bug,
and it is only visible if both stacks are in the same file.

## The 5-second threshold, and why it is not a guess

The GUI paces itself off vsync, so the normal interval between heartbeats is one
display frame — 16.7 ms at 60 Hz. Five seconds is 300 consecutive missed frames.
The real hangs above were 120 s and permanent.

That number is **measured, not asserted**. `cascade --frames N` prints the worst
heartbeat gap it observed, and `tests/test_diag_hang.cpp` runs exactly that and
requires the measured worst gap to be under **half** the threshold. If a future
change makes a frame legitimately slow, that test goes red before a user ever
gets a false report.

Three separate things would otherwise cry wolf, and each is suppressed for its
own reason:

1. **A debugger at a breakpoint.** `IsDebuggerPresent()` — a break is not a hang.
2. **A nested Windows modal loop.** Dragging or resizing the window, or holding a
   system menu open, stops the application's own loop turning over at all, and
   can legitimately last minutes. `GetGUIThreadInfo` reports exactly this
   (`GUI_INMOVESIZE` / `GUI_INMENUMODE` / `GUI_POPUPMENUMODE`).
2b. **Blocking work the application enters knowingly** — `WatchdogPause`. One
   path takes it today, and it is named rather than left as a general
   principle, because a mitigation with no call sites protects nothing:
   `AppWindow::rescanPlugins()`, which unloads and re-`LoadLibrary`s every
   installed plugin on the GUI thread. Anything added later that blocks that
   thread must take one too — a synchronous device open or a native modal
   dialog would, and neither exists yet (the Soapy open and scan are async,
   and there is no native file dialog). `cascade --frames N` prints how many
   pauses the run took and `tests/test_diag_hang.cpp` requires at least one,
   so a rescan that stops pausing goes red instead of silently arming a false
   report.
3. **The whole machine stopping.** Sleep, hibernate, or a paused VM freezes the
   watchdog thread too. If its own poll overshot by more than the threshold, it
   cannot tell the two apart, so it re-arms and says nothing.

The watchdog is stopped **last**, after the GL teardown — so a shutdown that
wedges is reported like any other hang. That is not an oversight: the worst
freeze this product ever shipped was inside a shutdown path.

## What a fault handler is allowed to do

A handler that allocates, takes a lock, or calls back into normal logging can
deadlock or double-fault a process that has already failed once. So the crash
path:

- **allocates nothing** — no `new`, no `malloc`, no `std::string`, no iostream;
- **takes none of this application's locks** — in particular the log ring's
  crash-path reader (`DiagLog::copyRingRaw`) deliberately does *not* take the
  log mutex, because a crash can happen while another thread holds it and a
  handler that blocked there would turn a crash into a hang with no report at
  all. The bounded cost is one possibly torn line, and it is the newest one;
- **calls no CRT formatting** — `snprintf` can take a locale lock, and a locale
  lock is a lock. Integers are rendered by hand;
- **writes with `CreateFileA`/`WriteFile`**, not buffered CRT streams that would
  need a flush the process may not live long enough to perform;
- keeps its large buffers (the 48 KiB ring copy, the `CONTEXT` it unwinds)
  **static, not on the stack**, because the fault it most needs to survive is a
  stack overflow, where roughly one page of stack is left.

Everything the fault path needs is prepared while the process is healthy: the
directory is created at install time, the module table is snapshotted in
advance, the context block is pre-rendered.

**The one honest caveat.** Unwinding on x64 means asking ntdll where a
function's unwind data is, and that reads loader data under a lock another
thread could hold. There is no unwind without it. So the report is written
**incrementally**, most valuable first: the fault kind, the faulting address as
`module+offset`, and the application context all reach the disk *before* the
walk is attempted. If the walk ever did deadlock, the file already on disk still
names the bug.

**The hang watchdog obeys the same caveat, and one more.** It runs in a process
that has not faulted, so it may allocate — but it walks *other* threads, and the
lock above is the one `LoadLibrary` holds exclusively while inserting a module.
This application calls `LoadLibrary` from the GUI thread (a plugin rescan) and
from a worker (the Soapy enumerate), so unwinding a thread *while it is
suspended* could block forever with that thread never resumed: the diagnostic
becomes a permanent hang. It therefore captures in two phases — the suspend
window contains `GetThreadContext` and nothing else, no user-mode lock and no
allocation, and every thread is running again before a single frame is unwound.
The price is that a thread which really is running can move under its own walk
and produce a garbled tail; frame 0 comes from the register and is exact, and
the threads that matter in a hang are not going anywhere. The identifying half
of the report is written and flushed before phase 2 starts, and
`tests/test_diag_hang.cpp` stops a capture there and reads the file back.

`__fastfail` (the `/GS` stack-cookie failure, `0xC0000409`) transfers straight
to the kernel and no user-mode handler runs. Those appear as an unclean exit
with no report — which `telemetryCleanExit` still counts, so they are visible as
a number even when they are invisible as a report.

### A finding worth keeping

MSVC's `set_terminate` installs a **per-thread** handler. The one installed on
the main thread does not apply to a thread this application never created —
which is precisely the case the terminate registration exists for, a vendor SDR
driver throwing out of its own stream-read thread. Measured, not assumed: with
only `set_terminate` installed, an exception escaping a `std::thread` killed the
process with `0xC0000409` and left **no report at all**. `abort()` is where all
of those paths converge on whatever thread they happen on, and the UCRT raises
`SIGABRT` before it fast-fails — so a `SIGABRT` handler is the net underneath,
and it is load-bearing rather than belt-and-braces.

## Symbols — where the archive lives, and why that was a decision

A captured stack is `module+offset`. Turning an offset back into a function and
a line needs the PDB produced by **that link** — not a rebuild of the same
source, not the same version number built on another machine. PDBs are not
shipped to users, so if the one matching a shipped binary is not kept at build
time it does not exist anywhere afterwards, and every report ever filed against
that build is unreadable hex forever. Nothing else in this feature matters if
that step is missing.

**The key is the PE build id, never the file name or the version.** The linker
stamps a CodeView `RSDS` record into every PE: a GUID plus an age counter,
regenerated on every link and recorded identically in the binary and in its PDB.
"0.61.0" is the release, the nightly heading towards it, and every rebuild in
between; only the build id tells them apart.

**The archive is not in git.** `symbols/` is gitignored. A PDB is tens of
megabytes of binary per link and committing one per build would make the
repository unusable inside a fortnight.

**It is also not local-only.** Local-only means one disk failure permanently
destroys the ability to read every crash report ever filed against every build
already in users' hands. That is not a risk worth accepting for a product that
is sold. So there are two copies, with different jobs:

| Copy | Written by | When | Job |
|---|---|---|---|
| `symbols\` in the working tree | `tools/archive-symbols.ps1` from a CMake `POST_BUILD` step | **every** build | offline, instant, impossible to forget because it *is* part of the build |
| `nas:/volume1/foxsdr-symbols` | `tools/build-nightly.ps1` | in the **same step** that compiles the installer | the durable copy, on the box that already holds this project's git mirror, over the same SSH alias and key |

Only builds that can reach a user are mirrored, and they are mirrored
automatically — a network copy on every incremental relink would be unbearable,
and a manual "remember to upload the symbols" step would eventually be skipped.
A mirror failure is **fatal** to the nightly; `-SkipSymbolMirror` exists for a
deliberately offline build and prints exactly what is being risked.

**`symbols\` grows, and that is expected.** Every relink adds a PDB — around
23 MB for `cascade.exe` at the time of writing — because a rebuild produces a
new build id and therefore a new archive entry. It is gitignored, so it costs
disk and nothing else. Prune it by hand when it gets uncomfortable, but **only**
entries older than the newest build that has been mirrored: anything shippable
that has *not* reached the NAS exists in one place. `symbols\index.txt` lists
every entry with its date, version and commit, which is how to tell them apart.

**A manual `ISCC` run bypasses the mirror.** The mirror is wired into
`tools/build-nightly.ps1`, so an installer compiled by hand from
`installer/cascade.iss` produces a shippable binary whose symbols were archived
locally and nowhere else. If you build an installer by hand, run the mirror step
yourself, or build through the script.

Both copies use the standard symbol-server layout, so the archive can be handed
to WinDbg or `dotnet-symbol` as a symbol path with no conversion step:

```
<archive>\<pdb name>\<GUID><AGE>\<pdb name>
<archive>\<exe name>\<TIMESTAMP><SIZEOFIMAGE>\<exe name>
```

`tests/test_diagnostics.cpp` asserts that **this** build's PDB really is in the
archive under the build id a report would quote, and that the build id read from
the running image is byte-identical to the one read from the file on disk — a
mismatch there would index the archive by something no report ever mentions,
while every individual piece still looked correct.

### Plugin PDBs are not in this archive, and that is the module class that matters most

**Read this before hunting for a plugin's symbols.** The `POST_BUILD` step is
attached to the `cascade` target, and no plugin is built in this tree — they
live in the separate plugin repository — so `symbols\` contains
`cascade.exe\` and `cascade.pdb\` and nothing else. Every plugin build id a
report quotes will be missing from it.

That is the wrong way round, and it is worth being blunt about why: plugins are
third-party code running in-process, "which plugin was loaded" has already been
the answer to real faults in this product, and a report's `--- modules ---`
block dutifully records a build id for each one. Left as it is, the modules
**most** likely to be the faulting ones are the ones that can never be
symbolised.

`tools/archive-symbols.ps1` is not specific to the application: `-Binary` takes
any PE, the build id comes out of that file's own CodeView record, and the
layout is the same symbol-server one. So the plugin repository archives into
**this** archive root from its own `POST_BUILD` step:

```cmake
add_custom_command(TARGET <plugin> POST_BUILD
    COMMAND powershell -NoProfile -ExecutionPolicy Bypass -File
            "<path to cascade>/tools/archive-symbols.ps1"
            -Binary "$<TARGET_FILE:<plugin>>"
            -ArchiveRoot "<path to cascade>/symbols"
            -Version "<plugin version>"
            -Commit "<plugin repo's short SHA, -dirty if modified>"
    VERBATIM)
```

`-Commit` and not `-CommitHeader`: the plugin repository has its own `HEAD`, and
stamping a plugin with the application's commit is the same "sends the reader to
a tree that exists and is wrong" failure the dirty marker below exists to
prevent. The index row names the DLL, so two repositories sharing one archive
stay distinguishable. `tests/test_diagnostics.cpp` runs the script against a
module that is **not** `cascade.exe` into a scratch archive root, so this path
is exercised rather than merely described.

Until a plugin's build has that step, say so in the report triage rather than
searching: the symbols do not exist anywhere, and no amount of looking in
`symbols\` will produce them.

Release builds are compiled with `/Zi` and linked with `/DEBUG /OPT:REF
/OPT:ICF`. `/Zi` is a debug-*information* switch, not an optimisation switch:
the generated code is byte-for-byte what `/O2` produced without it.

## Reading a report

1. Take the `build=` value for the faulting module out of the `--- modules ---`
   block.
2. Find it under `symbols\<pdb>\<build id>\<pdb>`, or on the NAS mirror, or grep
   `symbols\index.txt` for it to learn which version and commit it belongs to.
   **If the faulting module is a plugin, it will not be there unless the plugin
   repository archived it — see the section above.**
3. Point a debugger at the archive as a symbol path and resolve the
   `module+offset` frames.
4. `commit:` in the report names the tree to check out — exactly, unless it ends
   in `-dirty`, which says the build was made from a tree with uncommitted
   changes and that commit is only the nearest one.

That last line is only worth having if it is **this** build's commit. It is
generated by `cmake/git-commit.cmake`, which runs on **every build** and
rewrites the header only when the SHA changes. It used to be an
`execute_process` at configure time, which is silently wrong: CMake
re-configures when `CMakeLists.txt` changes, not when `HEAD` moves, so every
build after the next commit stamped the *previous* one — and a wrong SHA is
worse than "unknown", because it sends the reader to a tree that exists and is
not the one the offsets came from. (`symbols\index.txt` still shows the
evidence: nine different build ids all stamped `5ba13f6d0c86`.) The build id in
the modules block never had this problem, so the archive stayed correct
throughout; only the "check out this tree" line was wrong.
`tests/test_diagnostics.cpp` holds `gitCommit()` to what `git rev-parse` says
`HEAD` is right now, and runs the generator twice across a commit in a scratch
repository to prove it re-reads.

**And a fresh SHA still is not the tree.** Measured on this project: the built
binary reported `5ba13f6d0c86` while `git status` listed 32 modified or
untracked entries, the whole diagnostics feature among them — so following step
4 above would have produced a tree that does **not** contain the code those
offsets came from. Same failure as a stale SHA, and worse than "unknown" for the
same reason. A build from a modified tree is therefore stamped
`<sha>-dirty`, and the marker is on the **commit**, not the version:
`tools/build-nightly.ps1` appends `.dirty` to the version string, but an
installer compiled by hand from `installer/cascade.iss` — which
`installer/README-installer.md` explicitly contemplates — never goes near that
script and was covered by nothing.

"Dirty" is `git status --porcelain`, so an **untracked** source file counts. It
is exactly as absent from the checked-out tree as a modified one, and in the
measurement above most of the feature was untracked; `.gitignore` still applies,
so build outputs and `symbols\` do not mark a tree dirty. Both arms are
asserted: `tests/test_diagnostics.cpp` derives the expected string from the
tree's real state, so the marker appearing when the tree is clean fails just as
loudly as it missing when the tree is not, and drives a scratch repository
through clean → untracked → committed-then-edited.

`symbols\index.txt` carries the same string, because the archiver reads the
commit out of the header the generator writes rather than from a CMake variable.
It is written **without a BOM**: `Add-Content -Encoding utf8` on PowerShell 5.1
stamps one when it creates the file, which put `EF BB BF` in front of the first
row's date and made every anchored parse (`grep "^2026"`, a `^\d{4}` regex, a
split on the first tab) silently skip the oldest entry — the one row that can
never be re-derived from a later build.

## Where the user finds things

- **Log:** `%LOCALAPPDATA%\FoxSDR\logs\foxsdr.log`, rotating at 1 MiB, three
  files kept — `foxsdr.log`, `foxsdr.1.log`, `foxsdr.2.log`, so at most ~3 MiB
  and never a `foxsdr.3.log`. The count is asserted after three rotations in
  `tests/test_diagnostics.cpp`; it used to keep four, which nothing noticed
  because the first two rotations look identical either way.
- **Reports:** `%LOCALAPPDATA%\FoxSDR\crashes\`.
- **Settings → Diagnostics** shows both paths, has the on/off switch and the
  minidump switch, and has **Copy diagnostics** — one click that puts the whole
  bundle on the clipboard *and* saves it as `crashes\diagnostics.txt`, because a
  clipboard does not survive the next copy and a support thread can take days.
- After a run that did not exit cleanly (detected by the same
  `telemetryCleanExit` marker the crash counter already uses), the next start
  offers the same thing in a dialog. A crash handler can write a report but it
  cannot ask anything — by the time it runs there is no user interface left.

Nothing on that list is re-derived. The version, commit, plugin list with
versions, source and device state all come from state the application already
holds.

## Turning it off

`Settings → Diagnostics`. Off means off: no directory, no log file, no report —
not an empty folder, not a zero-byte file. `tests/test_diagnostics.cpp` and
`tests/test_crash_capture.cpp` both assert the directory does not even exist.

**And off from the first instruction of `main()`, not from the first frame.**
The switch is read out of the config *before* the crash handlers are armed and
before the log is configured, because the alternative is not a cosmetic
ordering detail: arming the on-disk half first means a user who opted out gets
a log file, a `crashes\` directory, and a live crash handler across the config
load, the GL context and the `LoadLibrary` of every third-party plugin — the
most fault-prone part of start-up. That is exactly what the shipped code did
until this was found by running it: a config saying `"diagnosticsEnabled":
false` produced a 14-line log. `tests/test_diagnostics.cpp` now launches the
real binary with that config and requires the tree not to exist — the two
older "off means off" tests could not see it, because they call
`configure()`/`installCrashHandlers()` themselves and so say nothing about
*when* `main()` calls them.

The directory path is still handed to the crash handler while capture is off,
so that switching diagnostics **on** mid-session has somewhere to write; the
directory itself is created at that moment, not before.

**And off from the moment the box is unticked, not from the next launch.** The
switch governs three components — the crash handler, the log, and the hang
watchdog — and for a while it governed only the first two. `start()` is a no-op
once the watchdog is running and is called once, before the frame loop, so the
directory handed to it there was the only thing that ever decided whether a
freeze report reached the disk: a user who started with diagnostics on and
unticked the box kept a live watchdog writing `hang-<pid>-N.txt` on the next
stall, which is precisely what the paragraph above promises does not happen.
The mirror image was as bad and quieter — switching diagnostics **on** after a
freeze armed the crash handler and the log and left the watchdog disarmed, so
the next freeze, the fault the user had just turned it on for, still wrote
nothing. All three now go through one function
(`AppWindow::applyDiagnosticsEnabled`), and `tests/test_diag_hang.cpp` drives
the **real** application through it in both directions with the hidden
`--diag-toggle on|off` hook — off then stall leaves nothing, on then stall
leaves a report — because there is no way to click a checkbox from ctest and the
defect lived exactly in the gap between what the checkbox governed and what it
did not.

Diagnostics also stay off for every flagged invocation (`--frames`,
`--selftest`, `--version`, the bench checks): those are tools and tests, and
they must leave nothing on the machine that ran them. `FOXSDR_DIAG_DIR`
redirects the whole tree into a caller-owned directory *and* turns capture on,
which is how the tests exercise the real application without going near a user's
`%LOCALAPPDATA%\FoxSDR`.

## What phase 2 will send

See PRIVACY.md. In short: a minimal report automatically, richer detail only on
explicit consent, and a minidump never — it is written locally and offered for
manual sending.
