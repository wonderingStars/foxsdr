# Merging master (0.99.100) into the engine extraction (engine/stage4)

`origin/master` c32047ab ("Record the five verified 0.99.100 release hashes"), merged into `engine/stage4` at 29d8b8a2 (the hostile-plugin suite, plugin-fault batch P2, docs/engine-stage4.md 6.29).
The merge base was aaf6e9c3 ("0.99.97: nothing drawn while the screen is off ... version bump and notes"), the commit docs/engine-merge-0.99.97.md merged; what is merged here is
master's 12 commits since: releases 0.99.98, 0.99.99 and 0.99.100, 60 files, +2,463 / -99 lines (55 files, +2,335 / -91, outside the release notes and PRIVACY.md). House discipline as in the 0.99.97 ledger
(sections 1 and 2 there): every commit classified before the merge ran, the code that moved re-expressed where it lives now with the same semantics, no silent stub.
**Stubbed: none.** The brief's "stop and hand back" conditions (a conflict that cannot be resolved without changing the plugin ABI or the C API table's semantics; an issue-report design
that fights the branch's reporting architecture) did not arise: the one addition to the application's command set is one new extension op (section 3), the table's semantics did not move,
and master's issue reports file from the very functions the branch's counters already feed.

**WHAT IS NOT MERGED, ON PURPOSE.** `origin/master` has moved on since the brief was written: it is at f78387ff, version 0.99.101, five commits past the 0.99.100 release (a quarter-second ring
between the radio and the DSP, an issue report sent in the session that wrote it). The remote has the tag v0.99.100 and **no v0.99.101 tag**, and the brief names 0.99.100 as the version the branch
must print, so the merge stops at c32047ab (the last commit of the 0.99.100 release). The five 0.99.101 commits are the next merge's; the ring change is a rewrite of src/core/pipeline.cpp
(+120 lines) and src/core/pipeline.hpp, which the branch has changed too (the per-thread watchdog's beats), and it is not something to take half-announced.

CLASSES: **carried** = merged (or applied by hand to the same function) with no change of place; **re-expressed** = its code moved on this branch and master's behaviour was put where the
machinery lives now (the destination is named); **n/a** = release notes, release hashes.

## 1. The ledger: one row per commit

| # | Commit | What | Class | Where it lives now |
|---|--------|------|-------|--------------------|
| 1 | ae506e8b | the five 0.99.97 release hashes recorded in the notes | n/a | docs/release-notes-0.99.97.md |
| 2 | c61e2195 | release.ps1: find the CI build run, not the newest run of any workflow | carried | tools/release.ps1 |
| 3 | c8ce8150 | 0.99.98: the fax chart drawn inside the Radiofax recorder, beside the keys that correct it (a HOST change; WEFAX 1.1.0 needs none) | carried + re-expressed half | Version, README, release notes, tests/fixtures/fax_picture_plugin.cpp, tests/test_fax_picture_app.cpp and their CMake registration carried. THE WINDOW HALF is `AppWindow::drawPluginWindows` (app_window.cpp: the same function), re-expressed over the branch's shape: the shared block `drawFittedPicture` and the recorder's three-part layout as master's; the click is no longer the window's own call into a plugin (`pluginRunner_.imagePress`) - the window holds no runner - but `FOXAPP_OP_IMAGE_PRESS` (section 3), delivered by the engine through the plugin guard. SEE SECTION 4: master's change stands on three earlier master features the branch had lost |
| 4 | acc12fce | the five 0.99.98 release hashes | n/a | docs/release-notes-0.99.98.md |
| 5 | 193295f7 | A detached monitor is one display change, not one a frame - and the GLFW refusal line says why | carried (applied by hand to the same two functions) | gui/display_query_episode.hpp (new, merged clean), third_party/glfw/src/wgl_context.c (the Win32 code in the 65543 refusal), tests/test_display_query_episode.cpp and test_display_query_app.cpp carried. `glfwErrorCallback` and `AppWindow::run` (app_window.cpp): the callback counts the display-settings error and writes nothing; the frame loop turns the count into one episode (one grace, one warn line, one settled line) and the `FOXSDR_TEST_DISPLAY_QUERY_FAIL` seam. SEE SECTION 5 for WHICH watchdog |
| 6 | 28da7383 | 0.99.99 version bump and notes | carried | CMakeLists.txt, README.md, docs/release-notes-0.99.99.md |
| 7 | 01c88cb8 | the five 0.99.99 release hashes | n/a | docs/release-notes-0.99.99.md |
| 8 | bcceceb8 | The driver's refusal of a second window is counted as a health event - `recovered.viewport` | carried | core/health_events.* (the word), the count in `AppWindow::run` where the refusal is read (same function), PRIVACY.md, docs/DIAGNOSTICS.md, the Worker's word, tests/test_health_events.cpp, test_health_paths.cpp, test_viewport_fallback_app.cpp |
| 9 | cd5b5702 | Every fault the program carries on from is reported, not just counted - the issue report (kind `issue`) | carried + re-expressed half | core/issue_report.{hpp,cpp} (new), core/crash_handler.hpp (`kIssueReasonPrefix`), crash_upload.*, diag_history.cpp, diag_report.*, exit_cause.cpp, health_events.cpp (`noteRecovered` and the four polled words file), PRIVACY.md "An issue report", tests/test_issue_report.cpp, test_crash_upload.cpp carried. Re-expressed: the branch's FIVE polled words (section 2), and the start-up flush of words met before capture is armed (row 10) |
| 10 | fdfcd524 | Eighteen recovered-from words for the handled faults the survey found - each now an issue report; the vocabulary is thirty-three words, so the bitmasks are 64 bits | carried + re-expressed half | THE CORE and twelve of the eighteen words carried verbatim: `readerleak` (airspy, airspyhf, aor, hackrf, mirisdr, rx888, pluto, rtl_tcp, rtlsdr, pluto_tx, patch_radio), `closeleak` (soundcard_source; pa_init in the branch's two-branch shape, section 2), `linkstranded`, `streamrestart`, `tunerretry`, `latecall`, `scanlimit`, `usbstuck`, `ratesdefault`, `catstop`, `sentinelgone`, `displaywatch`. RE-EXPRESSED, because the code moved into the engine: `gainrefused` -> `Engine::adoptDeviceMirrors` (engine.cpp), `ppmrefused` -> engine_ppm.cpp (both sites), `iqfilegone` -> `Engine::applyConfig` (engine.cpp), `cataloguebad` -> engine_store.cpp (`loadCachedCatalogue`, `loadCachedSites`), `filekept` -> engine.cpp (bookmarks, markers) and engine_service.cpp (settings) - master's one shared `setAsideDamagedFile` is three places here, `takeheader` -> engine.cpp `collectFinish`. The pending flush master put in `main()` is `bootProcess`'s `armDiagnostics` step (process_boot.cpp), so the headless engine files them too; the Settings page's two calls (app_window.cpp) carried |
| 11 | b9f510c9 | 0.99.100 version bump and notes | carried | CMakeLists.txt (`project(cascade VERSION 0.99.100)`), README.md, docs/release-notes-0.99.100.md; both binaries print "FoxSDR 0.99.100" |
| 12 | c32047ab | the five 0.99.100 release hashes | n/a | docs/release-notes-0.99.100.md |

Totals: **carried 5** (2, 5, 6, 8, 11), **carried + a re-expressed half 3** (3, 9, 10), **n/a 4** (1, 4, 7, 12) = 12. Of the nineteen recovered-from words master added (`viewport` and the eighteen of the survey):
**thirteen carried, six re-expressed** (the six above). **Stubbed: 0.**

## 2. What the merge itself resolved, and the hot spots

`git merge --no-commit` reported 14 conflicted files (`ort`):

  * **src/core/health_events.{hpp,cpp}, telemetry-worker/worker.js, tests/test_health_events.cpp, PRIVACY.md, docs/DIAGNOSTICS.md: THE VOCABULARY.** Master's thirty-three words and the branch's five
    (`pluginfault`, `pluginquarantine`, `pluginwedge`, `pluginoveruse` from the plugin containment; `threadwedge` from the per-thread watchdog) are one closed family, so the answer is the union:
    **thirty-eight words, master's thirty-three in master's order and then the branch's five**. Master's words keep their places so that the next merge from master (0.99.101 and on) stays a merge;
    every list that holds the vocabulary - the enum, the table with its once-a-session flag, the Worker's `HEALTH_VOCABULARY`, PRIVACY.md's "Recovered from" table, DIAGNOSTICS.md's survey of
    counting sites, the issue sentences, the tests' word lists, the 220 + 54 + 38 token count - was moved together, as tests/test_health_events.cpp requires in both directions. The bitmasks
    (`g_raisedRecovered`, `g_filed`, `g_pending`) are master's 64 bits, which holds 38. The Worker's own suite gained one test (below).
  * **THE ISSUE REPORT AND THE BRANCH'S COUNTERS (the design question the brief asked).** Master's rule is "every handled fault is reported": an issue report is filed once a session from
    `health_events.cpp` wherever a word is counted - `noteRecovered`, and the four polled words in `RecoveryWatch::poll` - and from nowhere else. The branch's five words are counted by the same
    `RecoveryWatch::poll` (the engine's pass reads the plugin guard's totals and the per-thread watchdog's wedge count), so the master's intent and the branch's architecture are one change:
    **each of the five now files its issue report where it is counted**, with a fixed sentence in core/issue_report.cpp (static_asserted to the count, a sentence that names no plugin and no thread;
    which plugin or thread it was stays in the crash and freeze reports' `blamed-plugin` and `thread` fields, PRIVACY.md says so). The poll runs on the engine's pass, a healthy thread -
    issue_report.hpp's contract ("never from a fault handler: every caller is a healthy thread, it allocates and takes the log ring's lock") holds. Nothing in master's design fought the branch's.
    Held by tests/test_issue_report.cpp (new block; break-it M1 below).
  * **src/core/patch_radio.cpp** (include of thread_beats.hpp against health_events.hpp): both kept. **src/sink/pa_init.cpp**: the branch's `PaStreamListGuard` tells a holder that stopped from a queue that
    starved (two messages); master's `closeleak` counted the one message "held for more than ...". The word is raised only in the `holderStopped` branch - the fault master named; a queue that
    merely starved is not a close that did not return.
  * **src/gui/app_window.cpp, app_window_disk_work.cpp, app_window_ppm.cpp, src/main.cpp**: the branch's taken whole (`checkout --ours`), master's hunks re-applied by hand from
    `git diff aaf6e9c3 c32047ab -- src/gui/app_window.cpp` (15 hunks of 19 did not apply by `git apply --reject`; every hunk is a row of section 1 and nothing else). In disk_work and ppm and main the
    master hunks are all word counts whose code is the engine's here (row 10); main.cpp's flush is process_boot.cpp's.
  * **tests/CMakeLists.txt**: both sides' registrations kept (test_display_query_app, test_fax_picture_app with the branch's `if(TEST ...)` timeouts). **tests/test_health_paths.cpp**: the table of
    counting sites moved with the sites (six words now name engine files, `FileKept` three sites in two files), master's "no file counts a word the table does not list for it" check kept beside
    the branch's per-word file count, and the watched set (nine words with no direct use) kept.

Not conflicts, and the traps of this merge (the 0.99.84 and 0.99.97 ledgers' lesson, again): **three features the branch had LOST at the 0.99.84 merge sat underneath master's changes**
(section 4), found only because master's new test, test_fax_picture_app, failed on the first run of the merged tree (the picture was absent from the recorder). **Line endings**: the working
copy is CRLF on this machine (`core.autocrlf=true`, `i/lf w/crlf`); every file touched by a script was written LF and the commit normalises; a Python patch script written through a bash heredoc
loses a doubled backslash (`'\\0'` became a NUL byte, `"\\n"` a raw newline in a C++ string) - found by an assertion in the script and by a failed read of the output, and the scripts that
carry backslashes were written with the editor tools instead. **Incremental links**: MSBuild did not relink a test executable after a static library it uses was rebuilt in two mutant runs
(the mutant "passed"), found because the executable's timestamp was older than the library's; every mutant run below deletes the executable first, and the gate builds follow a deletion of every executable.

## 3. API: one op

Documented in docs/api-0.3-additions.md 9.18 and held by tests/test_apply_command.cpp (every refusal and the no-change answers) and tests/test_fax_picture_app.cpp (the whole route, below).

  * `FOXAPP_OP_IMAGE_PRESS` (0x870F, SETTINGS, LOCAL sessions only, not on either internal profile): `text` the plugin's name as its picture carries it, `ival[0]`, `ival[1]` the click in the
    picture's own pixels. The engine hands it to the plugin's image-input capability (`CascadeImageInputApi::press`) through `PluginRunner::imagePress` -> `calls::press`, **the guard**: a
    faulting or never-returning `press` is absorbed like any other call into a plugin (tests/test_plugin_runner.cpp, test_plugin_hostile for the entry). `BAD_ARGUMENT` for empty text,
    `OUT_OF_RANGE` for a negative or 32-bit-overflowing coordinate, `NO_CHANGE` when nothing was delivered, `OK` when the plugin was handed the click. A remote session is refused by the general
    rule for application ops and again where the op is applied. The plugin ABI did not change; no field of the C API table changed; `get_local_extension`'s plugin-UI port gained no method (its
    virtual budget is untouched: test_engine_shell_guard passes at its numbers).
  * Not an op: the recorder's picture is drawn by the window from the images the port's `pollImages` already hands it (`pluginImages_`), so the plugin-UI port needed nothing for the DRAWING.

## 4. Master's fax change stands on three features the branch had lost

The brief expected the recorder change to be a layout change over the plugin-UI port. It is, but master's c8ce8150 is the third step of a series and the first two were not on the branch.
`git grep` of the branch at the 0.99.84 merge (2c5d14f9) finds none of them, and the master tip of that merge's base (04cba53a) has all three. The 0.99.84 ledger
classed their releases on other rows: 0.99.83's "the Radiofax recorder prints the chart on its paper" as **carried (gui/instrument_fax.cpp)** - the face's half was, the window's half that hands
the face its picture was not; 0.99.78's "a start-up death at the window says how it ended" as re-expressed with its window: lines, and the callback's log line was not among them; 0.99.81's
"IBOC dashboard programme rows respond to clicks" as the panel rows (B9) - the picture's click was not among them. All three are **restored here**,
because the feature cannot work without them; each is held by a test and a break-it:

  1. **A click on a plugin's picture reaches the plugin (0.99.81, `CASCADE_CAP_IMAGE_INPUT`, WEFAX's SET LEFT EDGE).** The image window called `pluginRunner_.imagePress`; the branch's window has
     no runner and nothing called `PluginRunner::imagePress` but its own unit test, so on this branch a click on a weather chart did nothing at all. Restored as `FOXAPP_OP_IMAGE_PRESS`
     (section 3), in the picture window and, new with 0.99.98, in the recorder. **A finding for the owner**: this was missing from the 0.99.90 and 0.99.97 builds of the branch.
  2. **The chart on the recorder's paper strip (0.99.83, field report 932444a354dee32f).** The window handed the face no `cue.chartTex`; `instrument_fax.cpp` was ready for it. Restored, with
     `AppWindow::ensureImageTexture` and `faxInstrumentWantsChart` (the upload serves the picture window or a shown recorder, and a decoder nobody watches still costs no texture).
  3. **The error callback's line in the Diagnostics log (0.99.78, field report A0E0967441A8E6B4).** `glfwErrorCallback` wrote stderr only; the block that writes the first fifty GLFW errors to
     the log (`windowplatform::glfwErrorLine`, the cap line) was gone, `glfwErrorLine` had a unit test and no caller. Master's 0.99.99 and 0.99.100 both lean on it: the flood fix says it must not
     spend those fifty lines, and the viewport issue report is only worth filing if the log it carries has the 65543 line with the driver's Win32 code. Restored, and the test that holds it is new
     (section 7): FOXSDR_TEST_GLFW_ERROR_AT.

## 5. The display-query flood and the two watchdogs

Master's fix lands in the **window's** hang watchdog and nowhere else, and that is where the fault was. The episode belongs to `AppWindow::run`'s frame loop: the loop's `PresentGrace`
pauses `watchdog_` (core/hang_watchdog.hpp, `kDisplayGraceMs`) for a present that stalls, and the flood restarted that grace every frame. The branch's per-thread watchdog
(core/thread_beats.*, docs/engine-stage4.md 6.27) watches the ENGINE's workers - dsp, source, keeper, the servers, the disk jobs - which a monitor going away cannot stop, and has no display grace
(`git grep kDisplayGrace\|displayChange src/core/thread_beats.*` finds nothing). The GUI thread's own beat is the hang watchdog's. So there is one place to change and it is changed; the comment
at the episode's declaration says why the other watchdog is not touched. Held by master's tests, which pass unchanged (test_display_query_episode; test_display_query_app: one warn
line for 41 failures, one settled line, no raw 65544 line, the cap not reached) and by the new case (section 7). Break-it M8: the early return removed from the callback -> test_display_query_app RED
(the flood written as 65544 lines).

## 6. i18n

`tools/i18n_keys.py`, each tree extracted with `git archive`: base aaf6e9c3 **2,032** keys; master c32047ab **2,032** (master added no `tr()` string in this range); branch 29d8b8a2 **2,039**
(the 2,035 of the 0.99.97 merge plus the four the plugin containment added); merged **2,039**. `comm` of the branch's list and the merged list: identical, both ways. The per-catalogue
report of the merged tree is byte-identical to the branch's: **2,039 translated, 0 empty, 0 missing, 0 stale in every one of the 33 catalogues.** src/core/lang_assets.hpp did not conflict and
did not change; no `tr()` string was added by the re-expressed code (every sentence of it is a log line or an issue sentence, neither translated).

## 7. Tests: new, converted, and the break-it list

Master's own tests ran unchanged where they run the real binary (test_fax_picture_app, test_display_query_app, test_display_query_episode, test_viewport_fallback_app with its issue-report
assertions, test_crash_upload, test_issue_report) and passed. Converted or added, never weakened:

  * **test_health_events** / **test_health_paths**: the word lists and the table of counting sites for 38 words (section 2). **test_health_paths**'s six re-located words name the engine files; its
    per-word file count and master's "no file counts a word the table does not list for it" both hold.
  * **test_issue_report** (new block): the five branch words, driven through the real `RecoveryWatch::poll` with the real crash directory: a baseline poll files nothing, a poll with nothing moved
    files nothing, a counter that moves files ONE report with the fixed reason and signature (two events in one poll are one report), a second move files nothing more.
  * **test_health_app**: `iqfilegone` driven through `Engine::applyConfig` with a missing saved I/Q file (the engine counts it; a second restore does not count it again).
    **test_plugin_catalogue_app**: a kept catalogue that is not a catalogue counts `recovered.cataloguebad` once (the engine's `loadCachedCatalogue`).
  * **test_apply_command**: `covering(FOXAPP_OP_IMAGE_PRESS)`: empty text, negative and over-32-bit coordinates, an unknown plugin and a plugin with no picture.
  * **test_fax_picture_app**: master's scenarios unchanged (the picture inside the recorder at four to one, the key row beside it, a click on the picture is `press 200 49`, a run that ends before
    the release hands nothing, a click on the key is `key 3`, the separate window never opened) and a **new scenario for the separate picture window** (its own rail row, "<plugin> image"; the
    recorder shut; a click in the middle reaches the plugin once as pixel (200, 49)) - the picture window's click had no test anywhere.
  * **test_display_query_app**: **two new runs**, a control and a flood, each with the new seam FOXSDR_TEST_GLFW_ERROR_AT=120 (bounded runs only, beside master's
    FOXSDR_TEST_DISPLAY_QUERY_FAIL): the callback is handed the 65543 refusal with its Win32 code at frame 120 and the log must carry that line once, after a flood too.
  * **test_process_boot_order**: the pending flush follows `setCrashCaptureEnabled` and precedes the log's `configure`, in the one function, once.
  * **worker.test.mjs** (60 tests, 59 + 1): the merged vocabulary - all thirty-three of master's words and the five of the branch's are each kept alone through the real Worker, and near misses
    of both are dropped.
  * Break-it list (mutants applied to the source, never to the test; the test executable deleted so it must relink; every file restored from saved bytes and its sha256 compared equal):
    M1 `ThreadWedge` not reported by the poll -> test_issue_report RED (4 of 5 files; "no issue report for threadwedge"); M2 the engine not counting `IqFileGone` -> test_health_app RED
    (`:1224`, `:1227`) and test_health_paths RED ("counted nowhere"); M3 the engine not counting `CatalogueBad` -> test_plugin_catalogue_app RED (`:471`); M4 the engine not delivering the
    press -> test_fax_picture_app RED (the recorder's press and the picture window's); M5 the callback's log block disabled -> test_display_query_app RED (the 65543 line absent, control and flood);
    M6 the chart not drawn beside the keys -> test_fax_picture_app RED (picture absent from the recorder); M7 no upload for a shown recorder (`chartWanted` false) -> RED, the same; M8 the
    display-settings error logged per frame (the callback's early return removed) -> test_display_query_app RED (the flood written line by line); W the Worker without `viewport` -> the merged
    vocabulary test RED. Two earlier "passes" of M1 and M2 were a stale executable (section 2), not a surviving mutant.

## 8. The gate

*Build:* `cmake -S . -B build` exit 0, then every executable under build/ deleted and `cmake --build build --config Release -j 16` exit 0 (m4-build-final.log; 512 test executables relinked; the warnings
are the ones already there - crash_handler.cpp, diag_log.cpp, updater.cpp, fox_engine_api.cpp, three C4456 lines of app_window.cpp - none in a line this merge wrote). *Headless*,
`ctest -C Release -L headless -j 8 --timeout 900`, scratch APPDATA / LOCALAPPDATA / USERPROFILE / HOME / CASCADE_CONFIG_TEST (m4-headless.bat): **647 tests, 647 passed (761 s)**. The first headless run of
the merged tree (before this batch's own new tests were added) was 646 of 647, the one red the recorded flake test_look_order (`:335 acted >= 1`); solo afterwards it failed **5 of 14** runs at that
same line (6 runs: 2 red; 8 runs: 3 red) and passed in the final full run - the desk's flake, not A/B'd against the pre-merge tree. *The gui label* on the hidden desktop
(`~/.claude/scripts/run_on_hidden_desktop.ps1`, CreateDesktop "FoxSDRTests", m4-gui.bat, `ctest -C Release -L gui -j 1 --timeout 900`, scratch profile): **68 tests, 68 passed (1,357 s)** - the branch's own and master's two new real-binary tests (test_display_query_app, test_fax_picture_app); test_shutdown_budget
and test_radio_setup_app, the recorded reds of earlier gates, passed. *Worker*: `node --test worker.test.mjs` in telemetry-worker, **60 tests, 60 passed** (59 + the merged-vocabulary test).
*Versions:* `build/Release/foxsdr-engine.exe --version` and `build/Release/cascade.exe --version` both print "FoxSDR 0.99.100", exit 0 (CMakeLists.txt `project(cascade VERSION 0.99.100)`, README.md,
docs/release-notes-0.99.100.md, merged clean). *i18n:* section 6 (2,039 keys, 0 empty / 0 missing / 0 stale in each of 33 catalogues; byte-identical to the branch's report). *The real settings file:*
%APPDATA%\foxsdr\config.json at the start and the end of the whole job - 7,892 bytes, sha256 73bb5198b6b23f410684b91dc396f07bcfcefe63c4e4026ec71dd1b9322728fc, identical; every run set its own APPDATA /
LOCALAPPDATA / USERPROFILE / HOME / CASCADE_CONFIG_TEST, and every window of the gui label was on the hidden desktop. *The door:* test_engine_shell_guard and the 45 interface_fence probes are in the headless
run and passed: no engine header reached src/gui (the new op is sent with `uiSubmit(cmd::makeText(...))`, as every other), no shell method, no port method.

**UNVERIFIED / NOT DONE**: (a) the five 0.99.101 commits are not merged (above); (b) Linux and macOS were not built or run - the vendored GLFW change is Win32 only, and `process_vm_readv` and the
rest of the plugin guard's Linux path are as 6.29 left them; (c) no ThreadSanitizer run of this merge: the new counts run on the engine's pass thread (`noteRecovered` takes the ledger's lock, as
the pass's existing `AudioLead` count does) and the press is applied on the pass thread; (d) of the six re-located counting sites only `iqfilegone` and `cataloguebad` have a scenario that drives
the real code; `gainrefused`, `ppmrefused`, `filekept` and `takeheader` are held by test_health_paths (which file counts the word, how many times) and by master's own style of test for them, which was
that table alone; (e) the press was driven through the test fixture's image input, not through a real WEFAX module, and a HOSTILE press through the new op was not run (test_plugin_hostile covers the `press`
entry through the runner; the op adds the engine's pass as the caller, where the window was before - a press that never returns now costs the pass its guard's budget instead of the window's);
(f) nobody looked at the recorder: the layout is checked from the UI census's rectangles (inside the window, four to one, the keys beside it), not from a screenshot; (g) the Worker is not deployed
(no wrangler run) and its deploy-before rule applies to the whole thirty-eight words; (h) nothing is pushed.
