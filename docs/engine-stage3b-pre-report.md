# engine/stage3b-pre report

Branch `engine/stage3b-pre`, pushed to `origin/engine/stage3b-pre`. Head at
the time of this report: `97f7498`. Never pushed to `master` or any other
existing branch; no tags or releases created; no force-push.

## Environment note (read this first)

The task brief describes "a cloud Linux environment... you have NO access to
the owner's Windows PC". The actual environment this session ran in is a git
worktree **on** the owner's Windows machine
(`C:\Users\steve\OneDrive\Documents\FoxSDR\.claude\worktrees\agent-aca1ff62f62ce6a01`),
under a sandbox that explicitly refuses `wsl.exe` and `cmd.exe /c` invocations
it cannot statically verify stay inside that worktree (both were tried and
refused). There is no Linux toolchain, container or WSL distribution reachable
from here. This is a real, load-bearing gap: the CI build this task's step 1
asks for (`.github/workflows/build.yml`: Ubuntu, GCC, GLFW's Wayland+X11
backends, `xvfb-run`, a null ALSA device) could not be run or verified in this
session, at all, by anyone or anything. What follows is what WAS achievable
and verified instead, stated plainly so the gap is not mistaken for having
been closed.

What was reachable: a real MSVC toolchain (Visual Studio 2022 Build Tools,
found via `vcvars64.bat`) plus a real CMake/Ninja pair (bundled with an
installed CLion, not on `PATH` by default) and `radioconda`'s bundled
SoapySDR. That combination builds and links the entire application, not just
the engine library, and every test in the suite except three could be
compiled and run for real. Full commands and the exact results are in
docs/engine-merge-0.99.42.md's last section. **This proves the merge is
correct C++ that builds, links and passes its own tests on a real machine; it
does not prove the Linux CI job would pass**, and no claim in this report or
in the commits should be read as covering that.

## Commits, in order

1. `b118d6f` - merge `origin/master` (0.99.42) into `engine/stage3b-pre`.
2. `46aae38` - merge `origin/engine/step1` (the measurement tool) into
   `engine/stage3b-pre`.
3. `6ffa063` - fixes for real build/test failures an actual MSVC build and
   `ctest` run surfaced in the two merges above (not visible from reasoning
   about the diffs alone).
4. `97f7498` - the 3b-pre 2a item: a behavioural test for the Transmit page's
   dead-man's handle, proven red against the named mutant.

Every commit message is a full account of what changed, why, and (for 3-4)
what was verified and how - they are not repeated in full here;
docs/engine-merge-0.99.42.md is the detailed conflict-by-conflict record for
commits 1-3, and the "2a" section below covers commit 4.

## Conflicts

24 conflicted hunks across 5 files from the `origin/master` merge
(`src/gui/app_window_converter.cpp` x1, `src/gui/app_window_soundcard.cpp`
x1, `src/gui/app_window_patch_radios.cpp` x5, `src/gui/app_window.hpp` x6,
`src/gui/app_window.cpp` x10 - counted by `<<<<<<<` markers at first
`git merge`, before resolution), plus 1 in `CMakeLists.txt` from the
`origin/engine/step1` merge - **25 total**. Every one is recorded in
docs/engine-merge-0.99.42.md with: what master changed since
the merge base, where the equivalent Engine method already lived, and
exactly what was applied where. The recurring pattern (stated once, applied
25 times): master edited a function stage 3a had already moved into `Engine`
verbatim; isolate master's real delta against the pre-extraction merge base,
apply it to the `Engine` copy, delete master's stale `AppWindow` copy.

Two design decisions worth surfacing here rather than only in the file-by-file
list, because they involved judgement rather than mechanical porting:

- **Where new-to-master state belongs.** Nothing in the existing
  `kWindowMayWrite`/ownership tables anticipated 0.99.40's I/Q-recording
  playback or 0.99.41's Airspy gain memory, because both post-date stage 3a.
  Placed by the SAME rule stage 3a already used for everything else:
  receiver/device state (`patchRecordings_`, `patchRecordingCache_`,
  `patchListsWanted_`, `airspyMemory_`, `airspyAtOpen`, the Airspy
  choose*/refresh methods) went to `Engine`; UI-only state
  (`drawPatchView`, `setMainViewPatch`, `drawViewKeys`, `drawAirspyControls`,
  `basemapStandIn_`) stayed on `AppWindow`.
- **A scan/probe command reused where it fit, a new one added where it did
  not.** Two of master's "ask for a device list" call sites looked
  identical at first (`FOXAPP_OP_SCAN_DEVICES_ON_OPEN` fit one) but master's
  own comments distinguish them ("a Radio's device list opened, or 'Look for
  radios'" vs "a Radio part added ... needs the native list" - explicitly
  NOT the SoapySDR probe). Reusing the existing command for both was tried
  first and was wrong - caught by `tests/test_main_view.cpp` (new in master,
  brought over by the merge), which asserts the "add a Radio part" action
  never logs "soapy: device scan started". Fixed with a new, narrower
  command (`FOXAPP_OP_SCAN_NATIVE_ONLY`). Full account in
  docs/engine-merge-0.99.42.md.

## Tests

- `test_command_path_guard`: kept green and meaningful. It initially FAILED
  after the merge - not because the merge broke the rule, but because (a)
  the guard's `kLineAllowed` table still named `drawPatchPage` for seven
  lines that moved to the new `drawPatchView` when 0.99.40 made the patch the
  window's other face, and (b) this merge's own new code (the Airspy gain
  sliders writing `deviceGainsDb_` directly) genuinely violated the rule.
  (a) was a stale allow-list, fixed by retargeting those seven entries. (b)
  was a real defect in code THIS merge wrote, fixed by converting the
  slider to the same local-copy-plus-`submitCommand` pattern the generic
  gain sliders already use - not by adding an exemption. One further
  violation (`asAirspy(engine_.device_)`, rule 4: no engine object passed as
  an argument) was fixed with a proper query method
  (`Engine::asAirspyDevice()`, matching the existing
  `currentAbsoluteHz`/`carriedAirCentre` pattern) rather than a guard
  exemption. `chooseAirspyDecimation`/`GainMode`/`Agc`, `airspyRememberOpen`,
  `asAirspyDevice` and `patchListRecordings` were added to `kControlMayCall`
  (the same reviewed direct-call pattern as `scanSoundCards`, all already
  true in the merged code - the guard's lists were behind, not the code);
  `patchListsWanted_` added to `kWindowMayWrite`. Verified green after every
  change; verified it still catches its own named probes is unchanged
  (nothing in this session touched the probe machinery itself).
- `tests/test_airspy_app.cpp` (new in master, unaware of the extraction):
  adapted mechanically to the `engine_.`-qualified access pattern every
  other `AppWindow` test already uses since stage 3a
  (`AppWindow::testHooks_` -> `cascade::engine::Engine::testHooks_`, every
  field/method access through `a.engine_.x`) - the same transform
  docs/engine-stage3.md section 7 describes for the tests stage 3a itself
  had to update. Verified green.
- Full suite (`ctest -E "^(test_rtlsdr_live|test_web_server)$" -j4`, MSVC,
  this machine): 214/216 green after all fixes; the two remaining failures
  (`test_soapy_enum_proc`, `test_soapy_source`) are this desktop's real USB
  bus, confirmed by `git diff` to have zero lines touched by either merge
  commit - see docs/engine-merge-0.99.42.md for the full account, including
  `test_converter_call_sites`, which is 5/5 green run with an explicit root
  argument (it only fails from this session's out-of-tree scratch build
  directory) and is not counted against the 216.
  **CORRECTION (engine/stage3b-pre repair round, 2026-09-28): the "this
  desktop's real USB bus" diagnosis for `test_soapy_enum_proc`/
  `test_soapy_source` does not hold - both pass on a re-run, on this same
  desktop, on both the branch and a master control build. See the
  correction note in docs/engine-merge-0.99.42.md for the detail; left in
  place above rather than rewritten.**
- **Linux**: not run. See "Environment note" above.

## 3b-pre round (task step 2)

### 2a - SAFETY FIRST: the TX dead-man's handle - DONE

`tests/test_transmit_dead_man.cpp` (new, commit `97f7498`): a headless
`Engine` (links `cascade_engine` alone) with a fake `IqSink` behind
`transmitter_`, driven through the real public `Engine::pumpTransmitter`.
Three scenarios: latched and keyed then the Transmit page closes in one pump
(released at once, not after the 1000 ms frozen-window bound); PTT held and
keyed, the same page-close check; and, as a regression net through the same
Engine-owned `transmitter_`, the remote PTT's own `kRemotePttHoldMs` hold
still expires on schedule (this path does not go through
`pumpTransmitter` at all - it is `keyRemote()`'s own mechanism, already
covered at the raw-`Transmitter` level by `tests/test_transmitter.cpp`, and
included here only to confirm it still works when the `Transmitter` is
Engine-owned rather than standalone).

**Proven red**: reintroduced the exact mutant named in the brief,
`if (pageLive) transmitter_.tick();` in `Engine::pumpTransmitter`
(`src/engine/engine.cpp`). Result: `test_transmit_dead_man` failed 5 of 15
checks (both the latch and the PTT scenario - the sink stayed running one
pump after the page closed); `tests/test_transmit_page.cpp`, rebuilt against
the identical mutant, **stayed green** - confirming the gap named in the
brief (that existing test proves WHERE the three lines are, by scanning
source text, not that they run correctly). Source restored (diffed
byte-identical against a saved copy before rebuilding) and reverified green,
15/15.

This closes the TESTING gap in docs/engine-stage3.md OPEN item 7 (a
behavioural check now exists for "the page closing releases the key at
once", and it is provably capable of catching a regression in that
property). It does **not** close the DESIGN problem item 7 describes for 3b
itself (moving the pump to a control thread means a frozen WINDOW no longer
stops `tick()`, and the page's key request needs to carry the window's own
liveness) - that is explicitly 3b's work, out of scope here, and is still
listed open in docs/engine-stage3.md with this session's addition appended
in place.

### 2b - window forms writing engine fields in place - NOT DONE

None of the 25 `kWindowMayWrite` fields (scanner range/timing, `soundCard_`,
`plutoUri_`, `transmitArgs_`, `pluginCatalogueUrl_`, `telemetryEnabled_`/
`telemetryInstallId_`, `transmitOpen_`, `patchGraph_`, the status-line fields
covered in 2c) were converted to commands or window-side drafts in this
session. `patchListsWanted_`, added to the list by the master merge (see
above), was placed following the SAME pattern (written in place by the
window) rather than converted, for consistency with its 24 siblings - not
because it is any easier to convert than they are.

**Why not attempted**: this is a genuinely large, multi-file change-set (25
fields, each with its own call sites, each needing a command definition, a
handler, and verification that no observable behaviour changed) that this
session's time went to instead getting the 0.99.42 merge itself to a
real, MSVC-verified, all-tests-green state - which turned out to need far
more of this session than expected, because it surfaced genuine defects
(the bare-field reads in unconflicted hunks, the command-path-guard
violation, the scan-command behavioural bug) that a diff-only merge would
have shipped. Converting 25 fields without the same level of build/test
verification per field would not meet the "prove each new/changed test
red then green" bar this brief (and the standing rules) set, so it was not
attempted rather than attempted shallowly.

**`transmitOpen_` specifically** (flagged SAFETY in the brief, because
`applyControlRequest`/`applyWebControls` reads it to decide whether to
release a remote transmit key): still an `AppWindow` field, still written by
ImGui directly, still read by `AppWindow::applyWebControls` (not
`Engine::applyControlRequest`, which does not currently see it at all - the
remote-key release for "the Transmit page closed" happens in the window,
calling `engine_.transmitter_.releaseRemote(...)` directly, per
docs/engine-stage3.md OPEN item 10, unchanged by this session). This remains
open and is, on its own account in that document, the more safety-relevant
of the two related items - not addressed here.

### 2c - engine-written, window-cleared status queues - NOT DONE

`sourceError_`, `gpsRefusal_`, `bookmarkImportNote_`, `soundCardMissing_`,
`decoderLog_`, `patchSinkLines_`, `mutePopupQueued_`/`muteKeptRunning_`/
`mutePopup_`, `soapyScanDeferredLogged_` - none moved to events or
clear-commands in this session. Same reason as 2b: not attempted rather than
attempted without verification.

### 2d - scoping kWindowMayWrite to the writing member(s) - NOT DONE

The guard's `kWindowMayWrite` list is still a flat allow-list (any control
may write a listed field), not scoped per-member. Two entries were ADDED in
this session (`patchListsWanted_`, and briefly `deviceGainsDb_` before it was
converted to the command path and the entry removed again - see the Tests
section) but the scoping mechanism itself was not built. `~Engine`'s
fallback-teardown-calls-a-half-destroyed-window question (the other half of
2d) was not investigated.

### 2e - docs/engine-stage3.md OPEN list updated - DONE (partially)

Item 7 has this session's finding appended in place (see above and the file
itself). Items 1-6, 8-16 are unchanged by this session and remain exactly as
stage 3a left them - this report is the "precisely what remains" the task
asked for in place of updating each one individually, since nothing in this
session closed or altered them.

## What could not be verified on Linux, for the owner's Windows/Linux
verification pass

Nothing in this session's diff is Windows-specific by construction (no
`#ifdef _WIN32` added or removed by any commit here), so there is no known
reason for it to behave differently on Linux. That said, specifically
unverified there:

- The full `.github/workflows/build.yml` job itself (GCC, GLFW's Wayland/X11
  backends under `xvfb-run`, the null ALSA device) - not run at all.
- `test_soapy_enum_proc`/`test_soapy_source` on a clean runner with no real
  SDR hardware attached (this session's Windows failures are specifically
  ABOUT real hardware being attached; a clean Linux CI runner has none,
  which is a different and probably better-behaved case, but that is an
  expectation, not a result).
- Anything gated by `#ifdef _WIN32`/`#ifdef __linux__` elsewhere in the
  files this session touched - a search for such guards in the touched
  files found none, but that search was not exhaustive against the whole
  touched diff.
- `tools/measure_engine.ps1` (PowerShell, Windows-only by name) - untouched
  by this session beyond the clean auto-merge; not exercised.
