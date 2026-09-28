# Merging master (0.99.42) and engine/step1 into the engine extraction

Branch `engine/stage3b-pre`, created from `engine/stage3` (a68bd0b, "engine
stage 3a (9): docs/engine-stage3.md"), then `git merge origin/master`
(b118d6f) then `git merge origin/engine/step1` (46aae38), then two follow-up
commits (6ffa063, 97f7498) fixing what an actual build found. Read
docs/engine-stage1.md, docs/engine-stage2.md and docs/engine-stage3.md first
if you have not - this document assumes their vocabulary (Engine, EngineHost,
`kWindowMayWrite`, `kControlMayCall`, the command path).

Master had moved on three releases since the merge base (1de7221, 0.99.39,
which is also `git merge-base engine/stage3 master`): 0.99.40 made the patch
view the application's main face and let a patch Radio play an I/Q recording,
0.99.41 added Airspy R2/Mini gain modes/decimation/a staged channel filter,
0.99.42 fixed map/radar-scope tile credits. None of that master work knew the
engine extraction existed, so every real conflict has the same shape:
**stage 3a had already moved a function's machinery from `gui::AppWindow`
into `cascade::engine::Engine` verbatim, and master, unaware, had gone on
editing the `AppWindow` copy.**

## Method

For every conflicted file, `git diff <merge-base> origin/master -- <file>`
isolates the REAL delta master made (as opposed to the noise of a whole
function reappearing as "theirs" because stage 3a deleted it from
`AppWindow`). That delta is then applied to the Engine method that now holds
the machinery, and master's stale `AppWindow` copy is deleted. This is
mechanical and repeatable, and it is how every one of the conflicts below was
resolved - restating it per file would be repetitive, so the file-by-file
list below focuses on the outcome and anything genuinely new.

Two things this method does NOT catch, and both bit:

1. **A file master added outright** (no conflict at all, since `AppWindow`
   never had the file) can still reference fields that moved to `Engine` -
   `git`'s line-based merge has nothing to flag, because there is no
   competing hunk. `src/gui/app_window_airspy.cpp` (new in master 0.99.41)
   referenced `device_`, `deviceGainNames_`, `sourceError_` etc. bare, all of
   which are `Engine::` fields since stage 3a. Caught only by an actual
   build (see "What an actual build found" below), not by reading the diff.
2. **A hunk master touched that stage 3a's own diff happened to leave
   unconflicted** - because the two edits, by chance, did not overlap enough
   lines for git's merge algorithm to flag a conflict, even though the
   RESULT is wrong (bare `pipeline_`/`dbMin_`/`dbMax_` reads inside the new
   `drawUi()` patch-view branch, a bare `scanNative()` call added for "a
   Radio part added"). Also caught only by a build.

## Conflicts, file by file

### src/gui/app_window_converter.cpp (1 conflict)

Master's real delta: `converterForKey` gained an I/Q-recording indirection
(`core::patch::converterKeyForDevice`) so a patch Radio playing a recording
uses the receiver's own converter for I/Q files. Applied to
`Engine::converterForKey` (src/engine/engine_converter.cpp); the
`#include "core/patch_devices.hpp"` that function needs came with it.
Master's copy of the whole helper block (`resolveConverterKey`,
`converterRawKeyNow`, `converterRadioKeyNow`, `converterForKey`,
`noteConverterFallback` as `AppWindow::` methods) was deleted - already
`Engine::` methods since stage 3a, called through `engine_.` from
`converterAliasNote()`, which was untouched.

### src/gui/app_window_soundcard.cpp (1 conflict)

Master's real delta: `pollSoundCard()` no longer auto-scans sound cards
merely because the patch page is open - since 0.99.40 the patch IS the main
view and is open at every launch, so that would scan every launch. Gated
instead on a new `patchListsWanted_` flag, set only when the user opens a
Radio's device list or presses "Look for radios". Applied to
`Engine::pollSoundCard` (src/engine/engine_soundcard.cpp); `patchListsWanted_`
placed as a new `Engine` field (device/receiver state, matching where
`soapyScanDeferredLogged_` and friends already live), added to the guard's
`kWindowMayWrite` (the window sets it in place from two UI sites - see
below). The rest of the file (`soundCardBackendFactory`, `launchSoundCardOpen`,
`pollSoundCard`'s body, `reapSoundCardWorkers`, `soundCardPatchArgs`) was
already `Engine::`; master's stale copy deleted.

### src/gui/app_window_patch_radios.cpp (5 conflicts)

The big one substantively: master's real delta (isolated the same way) is
the whole 0.99.40 I/Q-recording-playback feature - `openedAs` rewritten to
use `pc::radioOpenIdentity` (a recording's rate is the file's, so it is left
out of the identity string), two new helpers (`recordingRateText`,
`recordingFileName`), `patchDeviceChoices` listing recordings alongside the
generator, a new `patchListRecordings()`, `patchDeviceLabel` naming a
recording by its file, `patchDefaultDeviceKey` excluding recordings from "a
new Radio's default device", and - inside `patchReconcile`'s per-node
open loop - a whole new branch opening `core::patch::openIqRecording` when a
node's device is an I/Q file key, plus a rewritten comment on why the page no
longer scans on open (0.99.40, same reasoning as pollSoundCard above), and a
logging addition in `patchStopAll`.

All of it placed by the same rule as everything stage 3a already moved: the
receiver/patch-runtime state and logic went to `Engine`
(src/engine/engine_patch_radios.cpp - `patchDeviceChoices`, `patchDeviceLabel`,
new `patchListRecordings`, `patchReconcile`'s new branch, `patchStopAll`'s
logging), plus two new Engine fields (`patchRecordings_`,
`patchRecordingCache_`, declared in engine.hpp beside `patchDeviceChoices`).
The two small pure helpers `recordingRateText`/`openedAs`'s `radioRate`
dependency needed to be reachable from BOTH the Engine (which builds the
recordings list) and the window (`drawPatchRadioInspector`, which prints a
recording's rate) - `recordingRateText` was initially duplicated into
`engine_patch_radios.cpp`'s own anonymous namespace, then (once a build
proved that broke the window's use of it - see below) moved to
`engine/receiver_tables.hpp`, which exists for exactly this and already held
`radioRate` the same way. `recordingFileName` stayed engine-local since only
the Engine's `patchDeviceLabel` needs it.

The one true UI-only addition (`patchDefaultDeviceKey`'s recording exclusion)
stayed in the window, already calling `engine_.patchDeviceChoices()`.

Two call sites needed a real design decision, not just relocation, because
the window is not allowed to call `scanNative()`/`scanSoapy()` directly
(the command-path guard) and master's code, written before that rule
existed, did:

- **A Radio's device list opened, or "Look for radios" pressed**: master
  called `scanNative()` (always) and `scanSoapy()` (conditionally or
  unconditionally). The engine already had a command for exactly the
  Source-panel-list-opening case, `FOXAPP_OP_SCAN_DEVICES_ON_OPEN`
  (`scanNative()` always; `scanSoapy()` only if never/partially scanned) -
  reused here rather than inventing a second near-identical command.
  `patchListsWanted_ = true` is set directly by the window (the same
  `kWindowMayWrite` pattern as the other form flags) rather than needing its
  own command, since it is pure UI-visible state the Engine only reads.
- **A Radio PART added to the canvas** ("it starts on a free radio, so it
  needs the native list"): master's comment is explicit that this needs
  ONLY the native walk, never the SoapySDR probe. Reusing
  `FOXAPP_OP_SCAN_DEVICES_ON_OPEN` here was **wrong** - on a never-scanned
  session it also runs `scanSoapy()`, exactly the "probe on every
  interaction" regression 0.99.40 was written to prevent, just moved to a
  different trigger. Caught by `tests/test_main_view.cpp` (new in master),
  which asserts `!logHas(add, "soapy: device scan started")` for this exact
  action. Fixed by adding a new op, `FOXAPP_OP_SCAN_NATIVE_ONLY` (0x8406,
  `core/app_commands.hpp`; handler in `engine.cpp` calls `scanNative()`
  only) and pointing "add a Radio part" at it instead.

### src/gui/app_window.hpp (6 conflicts)

Almost entirely deletions of content master added that was already `Engine`
state (the whole `DeviceOpenResult`/`SoundCardOpenResult` struct bodies,
`soapyDevices_`/`soapyScanned_`/etc., `device_`/`soapyView_`/gain-mirror
fields, `biasTeePanel_`) - confirmed each was already declared in
`engine.hpp` before restoring nothing. Three genuine additions:

- `airspyAtOpen` (an `optional<AirspySetting>`, carried on `DeviceOpenResult`
  so the worker applies a remembered Airspy state before asking for a rate) -
  placed in `Engine::DeviceOpenResult` beside its sibling `preTuneRadioHz`.
- `airspyMemory_` (per-radio remembered Airspy settings) and the whole
  Airspy-controls declaration block - placed in `engine.hpp`, drawing
  declaration (`drawAirspyControls()`) kept in `AppWindow` (see
  app_window_airspy.cpp below).
- `patchOpen_`'s default flipped `false` -> `true` and its comment rewritten
  (0.99.40: the patch view is what a fresh launch opens on) - `patchOpen_`
  itself stayed exactly where it already was, an `AppWindow` field (the
  `EngineHost::patchPageOpen()` hook reads it). `patchListsWanted_` and
  `patchGraph_`, which master's copy of this same hunk also declared, were
  dropped - both already `Engine` fields (`patchGraph_` always was;
  `patchListsWanted_` per the soundcard.cpp entry above).
- `basemapStandIn_` (a bounded-run stand-in basemap flag, FOXSDR_FORCE_BASEMAP)
  - pure test-seam UI state, stayed on `AppWindow` beside its sibling
    `biasStandIn_` (which is `Engine`'s).
- `drawPatchView()`, `setMainViewPatch()`, `drawViewKeys()` declarations
  (0.99.40's patch-as-main-view) and `patchRecordings_`/`patchRecordingCache_`/
  `patchListRecordings()` (covered above) auto-merged with NO conflict at
  all - already consistent because they were pure additions with nothing on
  either side to collide with.

### src/gui/app_window.cpp (10 conflicts, the largest)

Same pattern at scale. The single largest conflict (originally ~1,500 lines)
was almost entirely `scanSoapy`/`launchDeviceOpen`/`scanNative`/
`adoptDeviceMirrors`/`openDeviceSync`/`soapyScanGated`/`soapyScanPlan` -
confirmed every one already `Engine::`-owned before deleting master's copy
and porting six real deltas into the `Engine` originals:

- `scanSoapy`/`scanNative`: a `diagLogf` line each, so a log (and
  `test_main_view`) can tell when a scan actually ran.
- `soapyScanPlan`: a patch-radio I/Q recording is excluded from the "radios
  on the bus" list the SoapySDR scan protects against probing (neither a
  recording nor the generator is on any bus).
- `launchDeviceOpen`: carries `airspyAtOpen` from `airspyMemory_` into the
  open request, and applies it to a newly-opened `AirspySource` on the
  worker thread, before the rate is asked for (decimation changes which
  rates exist).
- `adoptDeviceMirrors`: an Airspy's Rate combo shows delivered (decimated)
  rates via `airspyRateLabel`, and its two AGC switches are read back rather
  than force-disabled at open (they belong to whichever gain mode was just
  restored).
- `openDeviceSync`: the synchronous (config-restore) open path applies
  `airspyMemory_` the same way `launchDeviceOpen`'s worker does, and in the
  same order (remembered state before the rate).

`drawSourceSection`'s Auto-gain/gain-sliders block: merged so
`drawAirspyControls()` (new, draws its own panel and returns true for an open
Airspy) suppresses the generic Auto-gain checkbox and gain sliders, keeping
`FOXAPI_OP_SET_DEVICE_AGC`'s already-command-based application from HEAD's
side rather than master's direct `device_->setAutoGain()` call.

`applyControlRequest` (the web/CAT control-request handler): master's entire
conflicted block - Source/antenna/rate/gain/AGC handling written directly
against `device_`, `deviceGainNames_` etc. - was master's PRE-command-path
version of logic that `Engine::applyControlRequest` had ALREADY been
rewritten to handle via `cascade::net::controlRequestToCommands()` (an
unconflicted, unchanged file) converting the same `ControlRequest` fields
into the SAME `FoxCommand`s the desktop panel submits, applied through
`applyCommand`. Deleted master's block entirely; the two Airspy-refresh
additions it wanted (re-reading the gain list/AGC state and remembering the
open state after a remote gain-by-name or AGC change) were added instead to
`FOXAPI_OP_SET_GAIN`'s and `FOXAPI_OP_SET_DEVICE_AGC`'s command handlers in
`engine.cpp`, which is the ONE place both the desktop and the web/CAT path
now go through - a better fix than reintroducing the duplicate logic.

`drawUi()`'s patch-view branch (`if (patchOpen_) { ... }`, 0.99.40): the
whole branch auto-merged with NO conflict, but its body read bare
`pipeline_`/`dbMin_`/`dbMax_` (all `Engine::` since stage 3a) - a build
error, not a merge conflict, fixed to `engine_.pipeline_`/`engine_.dbMin_`/
`engine_.dbMax_`. Two more of the same class, also invisible to `git merge`
because they landed in already-clean hunks: a bare
`deviceOpenPending_`/`soundCardOpenPending_` check gating `seedPatchIfNeeded()`,
and a bare `scanNative()` call for "a Radio part added" (superseded by
`FOXAPP_OP_SCAN_NATIVE_ONLY`, see app_window_patch_radios.cpp above).

`refreshPluginRunner`/`pumpAddAll`/`removeInstalledPlugin`: already `Engine::`;
master's copy deleted. The one real addition - `refreshPluginRunner`
substituting a bounded-run basemap stand-in when no plugin supplies one -
**cannot go in `Engine::refreshPluginRunner`**: `gui/basemap_stand_in.cpp`
(the stand-in's actual definition, not header-only) compiles into
`cascade_gui`, not `cascade_engine`, so `Engine` calling it would link-fail
`cascade_engine`-only binaries (`test_engine_headless`, in particular) and
violate the "the engine must not depend on imgui, GLFW or the window"
boundary the CMake scan enforces (see the `engine/step1` section below - this
is exactly the class of thing that scan exists to catch, and very nearly
shipped as a straight `#include "gui/basemap_stand_in.hpp"` from `engine.cpp`
before the scan flagged it). Fixed by folding the substitution into
`AppWindow::attachBasemap` (the `EngineHost` hook the window already
implements to receive `Engine`'s plugin-supplied basemap): when the Engine
hands it null and `basemapStandIn_` is set, the window substitutes the
stand-in itself before forwarding to `basemap_.attach()`.

`AppWindow::currentConfig()`/`applyConfig()`: master's whole conflicted
blocks were the pre-extraction, monolithic versions of what `Engine::fillConfig`/
`applyConfig` already do (confirmed field-by-field: `sourceKind`, `soapyArgs`,
`converters`, `plutoUri`, `soundCard`, `centerHz`, `mode`, `bandwidthHz`,
`squelchDb`, `volume`, `dbMin`/`dbMax`, `biasTee` - all present, unchanged, in
the `Engine` versions). Two real new fields needed adding, one to each side
of the config split: `cfg.airspy = airspyMemory_` (receiver state) added to
`Engine::fillConfig`/`applyConfig`; `cfg.mainView = patchOpen_ ? "patch" :
"receiver"` (window state - `patchOpen_` is `AppWindow`'s) added to
`AppWindow::currentConfig`. The restore side of `mainView`
(`patchOpen_ = cfg.mainView != "receiver"`) had already auto-merged with no
conflict.

### CMakeLists.txt (1 conflict, from the engine/step1 merge)

`engine/step1` rewrote the command-path guard's file-scanning glob
(`_cascade_engine_files`) and introduced the actual regex it scans with
(`_cascade_hdr_re` - case-insensitive, handles a backslash-continued
`#include`, catches a `#define`-then-`#include` indirection) but dropped
`src/engine/*` from its own file list, because `step1`'s line of history
predates `src/engine/` existing at all. `stage3b-pre`'s side already had
`src/engine/*.cpp`/`*.hpp`/`*.h` in a per-extension glob (needed once the
engine's own sources existed to police). Kept both: the per-extension list
including `src/engine/*`, with `step1`'s `_cascade_hdr_re` definition beside
it.

**This scan is what caught a real problem the master merge had introduced**:
`engine.cpp`/`engine_airspy.cpp` had `#include "gui/airspy_panel.hpp"`
(master's new 0.99.41 header, pure inline logic, no ImGui) - flagged
correctly as an engine file naming a window header. Fixed the way every
earlier pure-logic helper was (stage 3a: `bias_tee.hpp`, `soundcard_panel.hpp`,
`tune_control.hpp`, `device_scan_plan.hpp`): moved to
`src/engine/airspy_panel.hpp`, namespace kept as `cascade::gui` (the code
inside is unchanged), every include site updated
(`engine.cpp`, `engine_airspy.cpp`, `app_window_airspy.cpp`,
`tests/test_airspy_panel.cpp`, `tests/test_airspy_app.cpp`).

### resources/lang/\*.json, src/core/lang_assets.hpp

Auto-merged with no conflict. Regenerated with `tools/embed-lang.py` anyway,
per the standing rule for generated files (never trust a textual auto-merge
of generated bytes) - `git diff --numstat` showed **zero changed lines**
after regeneration (only a checkout-time LF/CRLF difference, which
`core.autocrlf` normalises on `git add` regardless - verified with
`git show :src/core/lang_assets.hpp` returning pure LF). The auto-merge was
already byte-correct.

### src/core/engine_measure.cpp/.hpp, tools/measure_engine.ps1 (engine/step1)

Auto-merged with no conflict - `engine/step1`'s newest measurement-tool
commits (cycle-rate measurement refused on a busy machine, retakes per slot,
drop-aware rates, pinned builds) landed cleanly.

## What an actual build found (not visible from reasoning about diffs alone)

This environment turned out to have a genuine, working MSVC toolchain after
all (Visual Studio 2022 Build Tools + Ninja, both already installed; SoapySDR
via `radioconda`'s bundled copy, `CMAKE_PREFIX_PATH=C:\Users\steve\radioconda\Library`) -
found only after `cmake -G "MinGW Makefiles"` failed on MSVC-only flags
(`/MP`, `/wd4251`) the project's `CMakeLists.txt` passes unconditionally on
Windows. Building for real, rather than reasoning about the merge from the
diffs, surfaced defects that the diff-by-diff method above could not have
found on its own (documented in the two follow-up commits, 6ffa063 and
97f7498, in full): three bare-field reads and one bare mutating call in
`app_window.cpp` that landed inside unconflicted hunks (see above), two
missing `#include`s, one helper duplicated with the wrong linkage
(`recordingRateText`), a real command-path-guard violation this merge itself
introduced (the Airspy gain sliders writing `deviceGainsDb_` directly rather
than through the command path - fixed to the same pattern the generic
sliders already use, not exempted), and the `FOXAPP_OP_SCAN_NATIVE_ONLY` /
`FOXAPP_OP_SCAN_DEVICES_ON_OPEN` behavioural bug `test_main_view.cpp` caught.

**Build commands** (from the repo root, Git Bash `cd` into it first;
`vcvars64.bat`'s environment does not survive across separate tool calls, so
configure/build must run in the SAME `cmd.exe /c '...'` invocation as the
`call vcvars64.bat`):

```
cmd.exe /c 'call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" ^
  && cmake -S . -B <build-dir> -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="C:\Users\steve\radioconda\Library" ^
  && ninja -C <build-dir>'
ctest --test-dir <build-dir> -E "^(test_rtlsdr_live|test_web_server)$" -j4 --output-on-failure
```

(`cmake`/`ninja`/`ctest` used here from
`C:\Program Files\JetBrains\CLion 2026.1.3\bin\cmake\win\x64\bin` and
`...\bin\ninja\win\x64`, since neither is on `PATH` by default in this
environment - any CMake 3.20+/Ninja pair works.)

**Verified, this session, on this machine (Windows 11, MSVC 19.44)**:

- `cascade_engine`, `cascade_gui` and `cascade` (the full application) build
  with zero errors, only pre-existing warnings unrelated to this merge
  (`crash_handler.cpp`, `diag_log.cpp`, `sdrplay_source.cpp` - none of them
  touched by either merge commit).
- `cascade --selftest` passes (`peak_bin=666 audio_hz=703.1`) - the real DSP
  chain, not just a link check.
- The full `ctest` suite, `-E "^(test_rtlsdr_live|test_web_server)$" -j4`:
  **214/217 (built from an out-of-tree scratch directory) / 214/216 (the
  same run after the 3b-pre commit added `test_transmit_dead_man` and
  removed the need to exclude the two now-passing Airspy/patch-view tests)**.
  The remaining failures, confirmed NOT caused by either merge commit:
  - `test_converter_call_sites` fails only because the build directory used
    for this verification is `%TEMP%\...\scratchpad\build-msvc`, outside
    the repository entirely, and the test finds its source root by walking
    UP FROM THE CURRENT WORKING DIRECTORY looking for
    `src/core/freq_converter.hpp` (a normal, in-tree `build/` subdirectory
    finds it in one step; this out-of-tree one never finds it in the 8
    levels the test tries). Run directly with the repo root as `argv[1]`:
    **5/5 checks pass, 0 bypass the converter**.
  - `test_soapy_enum_proc`/`test_soapy_source` fail spawning their REAL
    child helper process against this desktop's actual USB bus (a webcam, a
    mouse, real SDRplay/UHD network discovery attempts visible in the log -
    `outcome=spawn-failed`). Neither test file has a single line changed by
    either merge commit (`git diff <both merge commits> -- <those two
    files>` is empty) - this is a real-hardware/real-desktop artifact of
    running here, the same class of thing the "verified state" lessons
    describe repeatedly, not a regression.
    **CORRECTION (engine/stage3b-pre repair round, 2026-09-28): this claim
    does not hold on a re-run.** Both tests PASS on this same desktop, same
    USB bus, both the branch build and a master control build (see the
    repair round's own ctest output for the exact counts and command). The
    original `spawn-failed` was real for that run, but "this desktop's
    actual USB bus" was the wrong diagnosis for its CAUSE - a transient
    condition (most likely a helper process or port momentarily busy from
    the SAME session's earlier device-open tests, not a structural fact
    about the hardware attached here) rather than something the tests
    cannot pass around. Left in place above rather than rewritten, per the
    standing rule against silently rewriting a claim once it has been
    pushed; this note is the correction.
  - `test_diagnostics` failed once, transiently, purely because the binary
    under test was built before the LAST commit and so reported a stale
    (but real) prior commit hash as `-dirty` against a since-advanced git
    HEAD - an artifact of iterate-then-commit ordering in an interactive
    session, not a code defect (a CI build, which builds once per commit,
    cannot exhibit this).

**What this does NOT verify**: this is a Windows MSVC build, not the Linux
build `.github/workflows/build.yml` actually runs (Ubuntu, GCC, GLFW's
Wayland+X11 backends, `xvfb-run`, a null ALSA device). No Linux toolchain,
container, or WSL was reachable from this environment (WSL invocations are
refused by the sandbox this session runs under - see
docs/engine-stage3b-pre-report.md). Everything above is real, but it answers
"does this compile and run correctly under MSVC on a real desktop", not
"does the Linux CI job pass" - those are different questions, and only the
second is what `.github/workflows/build.yml` actually gates. The Linux build
needs its own verification by whoever next has a Linux/WSL/container
environment available; nothing in this merge is Windows-specific by design
(no `#ifdef _WIN32` was added or removed by either merge commit), so there
is no known reason to expect it to fail there, but "no known reason" is not
the same as "verified".
