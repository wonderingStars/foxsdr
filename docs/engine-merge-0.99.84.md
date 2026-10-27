# Merging master (0.99.84) into the engine extraction (engine/stage4)

`origin/master` f28d40bc2b600b4b088bf93875b9b743ff765d6c ("0.99.84: the reload-phase release - version bump and notes"),
merged into `engine/stage4` at b4584aa (stage 4 complete: the window draws only through the engine API behind the compile fence;
suite 354/354, TSan 47/47). The merge base was 81691f2 ("0.99.42"), the commit docs/engine-merge-0.99.42.md already merged;
what is merged here is master's 237 non-merge commits since: releases 0.99.43 to 0.99.84, 909 files, +610,442 / -211,582 lines.
Read docs/engine-merge-0.99.42.md first for the method (isolate master's real delta of a function the branch moved, apply it to the
Engine method that now holds the machinery) and docs/engine-stage4.md for the vocabulary (EngineShell, the table, the pass, the fence).

This document is the record the campaign asked for: the shape of the conflict, the rules used, the CHECKLIST of every release
note item with its state (carried / re-expressed / stubbed / not applicable) and where it lives, the API additions, the traps, and what
is not verified. It is written as the merge goes and is complete only at its last section ("Status at the end of this pass").

## 1. The shape of the conflict

74 files conflicted in the merge itself (git merge, `ort`):

  * 33 resources/lang/*.json: both sides added keys to the catalogues (the branch three, master dozens). Resolved by data, not by text:
    master's catalogue plus the branch's three keys ("Allow remote transmit", the web PTT tooltip, "Type the board's address first."),
    re-sorted and re-written byte-exactly (the files are LF and sorted; the writer round-trips master's files with zero diff), and
    src/core/lang_assets.hpp regenerated with tools/embed-lang.py (it is generated: 451,867 lines of conflict markers were not read).
  * Build and CI (7): .gitattributes (both sides' rules kept), CMakeLists.txt (3 hunks), README.md, cmake/sanitize.cmake,
    .github/workflows/sanitize.yml, tests/tsan.cmake, tools/sanitize/tsan.supp, docs/SANITIZERS-AND-FUZZING.md, tests/CMakeLists.txt (9).
    The branch had PORTED master's sanitizer plumbing (pump move step 0) when master was at ~0.99.69; master kept editing the originals,
    so these were add/add. Resolution: master's file wins (workflow, page, cmake/sanitize.cmake) with the branch's one note on what it
    attaches to (cascade_engine, not cascade_lib), the branch's suppressions kept (a strict superset of master's), and tests/tsan.cmake
    merged (section 5).
  * Engine-side core (14): config.cpp/.hpp, diag_report.cpp/.hpp, freq_manager.cpp/.hpp, plugin_api.cpp/.hpp, plugin_runner.cpp/.hpp,
    plugin_ui.hpp, recorder.cpp, scanner.hpp, transmitter.hpp. Each was both sides editing one place; read both, merged (section 3).
  * src/net/web_server.cpp (2), src/sink/audio_out.cpp (1): both sides' additions.
  * The window and its tests (21): app_window.cpp (149 hunks, +10,542/-1,589 from master against the branch's +4,713/-8,280),
    app_window.hpp (32), app_window_converter/patch_radios/soundcard.cpp, module_census.cpp, plugin_store_view.cpp/.hpp, plugins_view.hpp,
    rail_banks.hpp, spectrum_view.hpp, transmit_page.hpp; tests/test_recorder.cpp, test_soundcard_app_paths.cpp,
    test_stop_ends_recordings.cpp.

Not conflicts, and the traps of the merge (the 0.99.42 merge's lesson, again): git merged cleanly, and WRONGLY, wherever master's change
sat in a file the branch had not touched but the code it calls had moved:

  * Master's new window files (app_window_airband.cpp 1,329 lines, app_window_disk_work.cpp 933, app_window_patch_presets.cpp,
    app_window_ppm.cpp, app_window_radio_setup.cpp, app_window_import_dialog.cpp, plugin_settings_form.cpp) are AppWindow members that
    read `pipeline_`, `config_`, `bookmarks_` ... directly. No conflict; every one of them fails the fence.
  * Master's tests (about 100 new) reach the window's engine fields through AppWindowTestAccess (`testHooks_`, `iqRecorder_` ...).
  * Master's `main.cpp` (+266 lines) calls AppWindow members that exist only in master's window.
  * app_window.cpp's functions that the branch moved to Engine (`GONE-in-branch` in the survey, section 6): git does not follow a function
    across files, so master's edits to them are in NO conflict and NO build error - they are silently absent. Found by the function-level
    delta survey (tools: a script over `git diff -U0 81691f2 origin/master -- src/gui/app_window*.cpp`, grouped by enclosing function and
    looked up in the branch), not by the compiler. Section 6 is that survey's output and is the worklist.

## 2. The rules used

  1. Non-window files: master's content wins where the branch did not restructure it; where both changed, both read and merged.
  2. src/gui and the window: THE BRANCH'S PORTED STRUCTURE IS THE BASE (app_window.cpp/.hpp, the converter/patch/sound-card parts are the
     branch's, taken whole: `git checkout --ours`), and master's features are re-expressed onto it one cluster at a time. The pure view files
     master changed (map_view, scope_view, spectrum_view, waterfall_view, patch_view, plugins_view.cpp, fonts, instruments, ui_scale, ...)
     merged by git and compile behind the fence; what each of them needs from the window is the cluster's.
  3. A window feature that cannot be ported in this pass is STUBBED, not dropped: its source file moves to src/gui_pending/ (not globbed by
     any target, not scanned by the fence guard), its tests are kept untouched in tests/ and replaced by a RED placeholder
     `merge_pending_<test>` (tests/merge_pending.cmake: `ctest -L merge-pending` lists what is owed, and each says which ledger id it belongs
     to), and every code stub carries `FIXME(2026-10-21, 0.99.84 merge, ledger <ID>)`.
  4. Master's tests are converted, never weakened: where one fails for a reason that is the merge's (a semantic master added) the fix is in
     the code or in the test's expectation WITH the reason written beside it.
  5. Version strings and release notes: master's.
  6. third_party/foxsdr_api/foxsdr_api.h: the branch's 0.3 header. Master has not changed it (0.2 only); nothing to understand.

## 3. What the merge itself resolved (engine side)

  * core/config.cpp/.hpp: master's includes (tester_usage, write_fault, sink/audio_out) beside the branch's view_settings include;
    the rail bank comment follows the VIEW -> TOOLS rename (the index did not move); `core::RailBank::View` became `Tools`
    (view_settings.hpp), and the window's two uses followed.
  * core/diag_report.hpp/.cpp: the branch's split (renderDiagContext, the one caller being the engine) with master's sound-path lines and
    master's "unchanged means untouched" rule in setDiagContext (now under the branch's context mutex). DiagBundleInput keeps the branch's
    `contextBlock`; the bundle's plugin inventory for the log scrub is read back from the block's `plugin:` lines.
  * core/freq_manager: master's `scan`/`heardSeconds` on Bookmark beside the branch's `id`; master's "a frequency that did not move keeps its
    row's place" in updateAt, with the branch's rule that the entry keeps its id (the caller's copy's id is never trusted).
  * core/plugin_api: the branch's snapshot-backed core, with master's per-plugin TRANSMIT grant, tx state, settings-UI sequence, row-key
    helpers and `setLive`; master's `ReceiverFacts` (now position-aware) is gone with the branch's receiver snapshot, and the receiver
    position master added to `publish()` moved to where the branch derives the level-1 counters (core/receiver_snapshot.cpp: a position
    change moves `abiSeq`, as master's did) and to `getState()` (`p.app.rxPositionSet/rxLatDeg/rxLonDeg`).
  * core/plugin_runner: both sides' hunks (the audio-path snapshot and the TRANSMIT path). core/plugin_ui.hpp: the branch's data split with
    master's `dormant_`; master's trackHasPosition / unplaced / estimated-position rules were ported into core/plugin_ui_data.hpp, where the
    branch moved HostTrack and the visibility rule.
  * core/recorder: master's asynchronous finish (FinishRequest / finishFile) with the branch's take clock (startedNs_/endedNs_).
  * core/scanner.hpp + scanner_params.hpp: master's list mode (`list`) is in ScannerParams, the data header the window holds drafts of.
  * core/transmitter.hpp/.cpp: master's plugin key (the cap, the fault latch, one source) merged into the branch's key model; the TX thread's
    `anyKey` now includes the plugin key (it did not: the plugin's audio was never pulled; found by test_tx_audio). [Until stub batch B13 NOTHING in
    the engine called setPluginKey (ledger TX-PLUGIN): a plugin could not key a radio, which was the safe state. Since B13 the engine calls it from ONE
    place, src/engine/engine_tx_plugin.cpp (txPluginStep), behind core::pluginMayKey, and every rule that made the absence of a caller safe is a test:
    docs/engine-stage4.md 6.22, B13 - the safety record.] The engine API's TX_SET_INPUT range is
    the hand inputs only (kTxHandInputCount): the Plugin input is never selectable.
  * net/web_server.cpp: `audioLeadMs` in the status JSON (statusJson), the usage callback; sink/audio_out.cpp: both new members.
  * engine/engine.cpp: master's native radio rows - HydraSDR and AOR enumerations and their unbound ids, the `Engine::makeDeviceSource`
    kinds "hydrasdr" and "aor", the Windows RSP row rule (`withoutDuplicateRsps` with the API-installed switch, `anyRspOrphanedByHiding`,
    `sdrPlaySourceAdvice`) and `Engine::dropRefusedMiricsSoapyRows`. The rtl_tcp row and its address box are the window's with the opener (NEW-RADIOS).
  * engine/engine_soundcard.cpp: master's privacy fix (no log line names a sound card: `loggableSoundCardDescription`) and the sound-card
    open failure count (`health::noteRadioFail`/`noteRadioOpen`).
  * Engine::publishConfig starts from the config as LOADED (`loadedCfg_`), so a field no half of this tree writes yet goes back to the file as
    it was read and not as a default (PPM values, patch presets, plugin sites, the per-radio gain memory ... would otherwise be lost at the
    first save by a build that cannot show them). Every field a half writes overwrites it.
  * core/patch_audio.hpp: master's slow-disk seams gave it the recorder, which the fence denies; kOutRateHz (the one thing the window draws)
    moved to core/patch_audio_rate.hpp. engine/record_start.hpp (master's gui/record_start.hpp: the recording's blocking open on a worker)
    lives in src/engine/ - it is engine machinery that master put in gui/ because the window owned the recorders (namespace still cascade::gui).

## 4. The build, the fence, the tests

The results are in section 9 ("Status at the end of this pass"); the API additions are section 7, the traps section 8.

## 5. tests/tsan.cmake

Master's selection (about 70 tests) plus the branch's (the engine API, the control thread, the plugin UI, the pump move's tests and every
`<name>_free`). The branch had excluded 46 of master's thread-bearing tests as "not run yet: master found its first races in code this
branch does not have the fixes for"; the fixes are here now, so that exclusion is gone and they are IN. Tests that are pending
(section 6) are skipped by the typo check, not selected.

## 6. The ledger

Every bold item of master's release notes 0.99.43 to 0.99.84 (docs/release-notes-0.99.NN.md), with its state:

  * **carried**: the change lives in a file the branch did not restructure (core/, dsp/, source/, sink/, net/, a pure gui/ view, a script, a
    catalogue) and git merged it; it compiles behind the fence. "Carried" is the merge's claim, not a verification: where a test of the item
    exists and runs, the test is named; where none does, the item says (unverified).
  * **re-expressed**: master's AppWindow code became an Engine member plus facts/ops for the window (or stayed the window's where it is
    pure interface), with the commit that carries it and the test that holds it. C0 = 2c5d14f (the merge), C1 = 96093f8, C2 = 168ff14,
    C3 = 00e35d2, C4 = 26203b7, C5 = the commit that follows C4 (async disk work).
  * **stubbed**: NOT done. Each has an id; its master tests are out of the build in tests/merge_pending.cmake and replaced by a red
    `merge_pending_<test>` (`ctest -L merge-pending`); its master source is in src/gui_pending/ where it is a whole file, and in
    docs/engine-merge-0.99.84-lostlines.txt (every code line master added to a window function that is absent from this tree, by function)
    where it is part of a function. A stub is a feature missing from the window, never a feature that is wrong.
  * **n/a**: not part of the application (a catalogue, a site, a packaging or documentation note, a plugin's own change).

### 6.1 Release by release

0.99.43  (the merge base's successor; plugin settings forms)
  * a plugin's settings UI, CASCADE_CAP_SETTINGS_UI .......................................... re-expressed, stub batch B9 (src/gui/plugin_settings_form.cpp over a
    gui::SettingsEditSink - the fence denies plugin_api.hpp to src/gui; the window's sink reads PLUGIN_SETTINGS (list 52) and fact.plugin.setting.<id> and sends
    FOXAPP_OP_PLUGIN_SETTING_EDIT, which the ENGINE judges and writes through settingsUiSet - src/engine/engine_plugin_settings.cpp; test_plugin_settings_ui
    with an adapter over PluginApiCore and its [S] section, tests/test_engine_api_settings.cpp)

0.99.44
  * interface size, for 4K screens ........................................................... re-expressed, stub batch B1 (docs/engine-stage4.md 6.22): the
    window's geometry goes through gui/ui_scale.* (app_window_chrome.cpp, run()'s DPI block, ~25 sites), the config carries interfaceScale; held by
    test_ui_scale_app (the census at 100% and 150%: every rail rectangle 1.5x, nothing drawn over anything new; written RED first), test_window_chrome_config
  * SDRplay asks for zero-IF ................................................................. carried (source/sdrplay_source.cpp) (unverified: no radio)
  * audio no longer crackles when the channel filter widens ................................... carried (dsp/) (test_bandwidth_app also exercises the filter, C3)
  * a per-frame system call removed .......................................................... carried (unverified)
  * Airspy ADS-B with decimation raised ...................................................... carried (source/)
  * beta tester usage reporting .............................................................. re-expressed, stub batch B15 (src/gui/app_window_tester.cpp: the WINDOW's
    recorder, fed from the table's facts and the window's own events; SYSTEM > Beta tester; the journal and the retry queue; the headless engine cannot send a tester
    report: test_tester_host_only. The section's own test, written red first: test_tester_usage_app)
  * the crash-report cleaner and a plugin's name ............................................. carried (core/) (test_crash_scrub runs)

0.99.45
  * link FoxSDR from the tester portal ....................................................... re-expressed, stub batch B15 (app_window_tester.cpp: the link-request file
    polled on a worker, confirm-by-name, the "Link this FoxSDR?" prompt, the portal-token exchange; test_tester_link_app, test_link_request_poll)
  * the usage report is sent once, not once per launch ....................................... carried (core/telemetry) with the engine's journal (C0, C1)

0.99.46
  * Linux: closing while sending a report ..................................................... carried (net/, core/) (unverified: Linux)
  * slow computers file no start-up freeze ................................................... carried (core/hang_watchdog) (test_diag_hang green)

0.99.47  (no bold item of its own in the notes)

0.99.48
  * bench meter styles ....................................................................... re-expressed, C3 (test_meter_style_app green; visual check in 8)

0.99.49
  * whole-patch presets ...................................................................... stubbed PATCH-PRESETS (test_patch_presets_app; core/patch_presets
    and the file src/gui_pending/app_window_patch_presets.cpp)
  * AOR AR5700D / IQ5001 ..................................................................... re-expressed, C0 (engine enumeration, the "aor" source kind and its
    unbound-id row; no window-specific part)

0.99.50
  * the plugin store cleans up after its own updates ......................................... re-expressed, stub batches B3 and B4 (B3, the ENGINE's half:
    engine_store.cpp: an update removes the copy it replaced, on a worker, that plugin's only; CLEAN UP and REMOVE FILE are FOXAPP_OP_STORE_CLEAN_OLD;
    a refused delete is queued for the next start; test_plugin_cleanup_app, test_plugin_update_cleanup_app, tests/test_engine_api_store.cpp S5. B4, the
    window's: the CLEAN UP OLD VERSIONS key at the foot of the UPDATES tab and its one confirmation send FOXAPP_OP_STORE_CLEAN_OLD 0 and the foot lists
    fact.store.oldCopies; tests/test_plugin_store_window.cpp A and D2c, test_plugin_store_view)
  * FUNCTION SELECT folds to a strip ......................................................... re-expressed, stub batch B1 (test_rail_fold, test_rail_fold_app,
    test_app_rail, test_window_chrome_config)
  * the patch page's information pane at the top right ....................................... stubbed PATCH-VIEW (lostlines: drawPatchView, drawPatchFaces)
  * the patch canvas pans by dragging empty canvas, the wheel pans ........................... stubbed PATCH-VIEW
  * two fixes for radios reported wrongly (the Source row's words) ........................... re-expressed in part: the one-chip-one-route rule runs in
    the engine and is held by test_one_chip_app (C4); the Source row's chip/preview/"No radio hardware found" is stubbed SOURCE-ROWS (test_source_row_app)

0.99.51
  * SDRplay: no more "restart the SDR API" on a slow setting ................................. carried (source/)
  * Raspberry Pi 64-bit downloads ............................................................ n/a

0.99.52
  * Windows testers can link from the tester page again ...................................... re-expressed, stub batch B15 (the foxsdr:// activation and the instance
    claim are the process boot's, STEP 5, Host kind only - live since stage 7a; the window's poll and prompt are B15's; test_tester_link, its token-never-logged scan
    now reading engine/process_boot.cpp)

0.99.53
  * the Store version tells you when the Store has an update ................................. re-expressed, stub batch B14 (src/gui/app_window_updates.cpp: a Store copy
    asks the Store, the banner and its install key, the Updates section's Store states; test_store_update_app)

0.99.54
  * close bookmark names stack instead of hiding ............................................. re-expressed, stub batch B10 (drawBookmarkMarkers' rows, the Bookmarks
    section's "Stack close names" key, and the window half of the config - applyConfig/currentConfig had dropped both bookmarkMarkers_ and
    bookmarkStackNames_ - test_bookmark_stacking_app)

0.99.55
  * the SDRplay API Service stopped or stuck is named and can be restarted ................... re-expressed in part: its two recovery troubles are counted by the
    engine (C2, test_health_app); the card, the restart key and the note are stubbed SDRPLAY-SERVICE

0.99.56
  * PPM frequency correction ................................................................. re-expressed, C1 (engine_ppm.cpp; test_ppm_app, inline and stepped, break-it done)
  * RTL-SDR steadier when the computer is busy ............................................... carried (source/)
  * audio no longer turns choppy ............................................................. carried (sink/)

0.99.57
  * markers on the waterfall ................................................................. re-expressed, stub batch B10 (the marker LIST and its persistence were
    the engine's since C5; now it is worked through FOXAPP_OP_MARKER_DROP/_REMOVE/_CLEAR/_NOTE 0x8606-0x8609 and read as FREQ_MARKERS (53), engine_markers.cpp;
    the window's half is app_window_markers.cpp: the lines, the tabs, the right-click menu on the receiver's waterfall and on a patch Display part, the Markers
    window; test_freq_markers_app, test_markers_window_app, test_engine_api_markers)
  * peak hold and average on the spectrum ................................................... re-expressed, stub batch B11 (app_window_display.cpp: the trace-mode menu, the
    held trace over the receiver's spectrum and a patch Spectrum part's, the label, the config; window state, no table surface; test_spectrum_trace_app)
  * the receiver's waterfall fits its window ................................................ stubbed MARKERS-SPECTRUM (spectrum/waterfall views are in the tree)
  * holding the left button on the waterfall no longer tunes ................................. re-expressed, stub batch B10 (drawCenterPanels' release branch asks
    gui::pressIsClick again, wfPressT_; held by test_freq_markers_app's long press)

0.99.58
  * the map's coverage line counts only what a receiver could hear ........................... stubbed MAP-WINDOWS (lostlines: drawPluginWindows, recordCoverage)
  * plugin updates in the catalogue .......................................................... n/a

0.99.59
  * Red Pitaya request ....................................................................... n/a
  * picking a radio that does not answer no longer freezes the window ........................ carried (the engine's open worker: C0)
  * a faulty ASIO driver no longer makes the scan fail ....................................... carried (source/)
  * SDRplay through SoapySDR no longer freezes after the service restarts .................... carried (source/)
  * privacy: reports carry no addresses, computer names, ports or sound-card names ........... carried (core/) and re-expressed (C1: no log line names a card)

0.99.60
  * Airband: type an airport, hear its control frequencies ................................... re-expressed, stub batch B7 (src/gui/app_window_airband.cpp: the lookup sends the
    airport's rows as FOXAPP_OP_BOOKMARK_ADD_ROWS, a list; the engine's half is batch B6's; test_airband_app, lookup)
  * LISTEN, ticked frequencies ............................................................... re-expressed in part, stub batch B6: the ENGINE runs the monitor
    (src/engine/engine_airband.cpp: the blocks, the squelch, the scan between them, the hand-back from the patch view, the heard time) and the scanner's list mode
    (FOXAPP_OP_AIRBAND, _BOOKMARK_TICK, _SCANNER_LIST, AIRBAND_CHANNELS; tests/test_engine_api_airband.cpp), and the window draws the Bookmarks tick column and the
    Scanner's "Ticked frequencies" key; the Airband SECTION (its LISTEN key, rows, lamps) is re-expressed, stub batch B7 (test_airband_app, listen / stop / hand-back)

0.99.61
  * the Bandwidth control narrows what you hear .............................................. re-expressed, C3 (the filter is core/dsp; test_bandwidth_app)
  * SDRplay live controls name the tuner ..................................................... carried (source/)
  * SoapySDR is not a second way into a released SDRplay radio ............................... carried (source/) with the engine's row rule (C0)
  * Linux (Wayland) crash when closing ....................................................... carried (gui/) (unverified: Linux)
  * the window does not freeze on a slow settings folder ..................................... re-expressed, C5 (the settings writer was async already; C5
    adds the back-off and the kept-aside damaged file)
  * a freeze cannot go unreported; a taskbar-ended freeze is a crash ......................... carried (core/sentinel) and re-expressed, C1 (the phases)
  * scan crash reports name the driver ....................................................... carried (core/, source/)
  * "No audio" reports answer themselves ..................................................... re-expressed, C1/C2 (the diagnostics context's sound-path lines;
    test_diag_audio_app, test_diag_context_app)

0.99.62
  * freezes grouped by FoxSDR's own code ..................................................... carried (core/)
  * one scan failure is one report ........................................................... carried (core/, source/)
  * display stalls counted in the usage record ............................................... re-expressed, C1 (core::globalStallLedger, the watchdog's sink)
  * diagnostics describe the radio in use; a bug report carries the previous session's log ... re-expressed, C1/C2 (Engine::refreshDiagContext on every event
    - Engine::commitModeIndex, DiagContextOnExit - and AppWindow::currentDiagnosticsBundle with the history; test_diag_context_app, test_diag_history)
  * Linux line-level debug information kept ................................................. n/a (the symbol archive: tools/ and CMake, merged)

0.99.63
  * Record does not freeze while the disk creates the file; a waiting start can be cancelled . re-expressed, C5 (Engine::startAudioRecording/startIqRecording
    prepare, RecordStart opens, pollRecordStarts arms; test_record_start)
  * a catalogue refresh reloads nothing when nothing changed; every reload logs its steps ..... re-expressed, C4 (Engine::rescanPluginsIfChanged; test_plugin_rescan_log;
    test_plugin_rescan_skip converted by stub batch B2: re-expressed)
  * the Fitted modules window reads no plugin file's size per frame ......................... re-expressed, stub batch B5 (the size is PLUGINS value[1], measured by the
    engine when the table's block is made; the window's draw function and the cache it reads - src/gui/app_window_fitted.cpp - ask the file system nothing:
    test_fitted_modules_no_disk, whose scan now reads that file and both functions, edited with its reason beside it)
  * freeze reports name a late-loaded module ................................................ re-expressed, C1 (ForeignModuleWatch; test_foreign_modules_app)

0.99.64
  * FoxSDR notices when it ends without saying why ........................................... re-expressed, C1 (breadcrumb phases/activity; test_sentinel_app)
  * a slow frame is measured and named ....................................................... re-expressed in part, C1 (the frame timer and its scopes through run()
    and drawUi; the recorder and reload scopes are not carried - those run on the engine's pass, not the frame; test_slow_frames_app stays pending)
  * the usage record counts what quietly did not work ........................................ re-expressed, C1/C2 (test_health_app, test_health_paths)
  * eight more file operations off the drawing thread ........................................ re-expressed in part: the recorders (C5), the bookmark list's save
    (C5), a patch speaker's file (C5); the picture save, the bookmark export, the I/Q open on a worker, the screenshot flush are stubbed DISK-WORK
    (test_failure_files, test_gui_file_jobs, test_iq_open_async, test_gui_disk_audit)
  * five faults found with a memory checker .................................................. carried (core/)

0.99.65
  * Stop then Record does not overwrite ...................................................... carried (core/recorder naming) and re-expressed, C5 (the queued Record)
  * a damaged settings, bookmark or marker file is kept ...................................... re-expressed, C5 (AppWindow::setAsideDamagedFile for the settings,
    Engine::loadBookmarks for the two lists); test_failure_files (not converted) holds the three
  * Linux: a folder in a list's place no longer ends the program ............................. carried (core/) (unverified: Linux)
  * stopping a recording does not freeze the window .......................................... re-expressed, C5 (finishTake / RecordFinisher; test_record_finish_async)
  * a freeze report keeps its log on a slow disk ............................................. carried (core/diag_log; test_diag_log_lock)
  * the usage record counts slow frames and recoveries ....................................... re-expressed, C1/C2
  * an unwritable settings folder is not retried every two seconds .......................... re-expressed, C5 (noteConfigWrite / configRetryHeld)

0.99.66
  * the Airband monitor takes a typed list and plays NFM ..................................... re-expressed in part, stub batch B6 (the engine: a typed row, an NFM channel and one
    channel per frequency, test_engine_api_airband A2 / B3 / F1) and B7 (the add row, the mode list: test_airband_app, add row / nfm / one channel per frequency)
  * presets: a named list filled by hand, from a file, exported ............................. re-expressed, stub batch B7 (the preset field, Add, Remove preset - the engine stops
    a monitor that plays the group, B6; IMPORT of a typed path and EXPORT CSV are window-side disk jobs in app_window_airband.cpp, see docs/engine-stage4.md 6.22 B7:
    the existing BOOKMARK_IMPORT_FILE / BOOKMARK_EXPORT carry neither a group nor a CSV; the empty-box Import DIALOG is B8's - openImportDialog is the empty hook)
  * the spectrum shows what the monitor is hearing ........................................... re-expressed, stub batch B7 (marks fed from AIRBAND_CHANNELS read every frame;
    the VFO band, its waterfall line and its peak figure are not drawn while it listens; test_airband_app, spectrum)
  * a native HydraSDR RFOne driver ........................................................... re-expressed, C0/C4 (engine enumeration and kind; the remembered
    gain mode under its own key, found by test_hydrasdr_app)
  * a sentinel report says where a fast-fail was ............................................. carried (core/)

0.99.67
  * closing the console window no longer ends FoxSDR ......................................... re-expressed, C1 (the frame loop's condition; test_engine_shell_guard)

0.99.68
  * a stop does not wait on idle recorders; the log says who asked ........................... re-expressed, C2/C5 (Engine::stopReceiver(who), FOXAPP_OP_TEST_HOLD_AUDIO_LOCK;
    test_stop_ends_recordings green on the hidden desktop)
  * the browser remote no longer makes the window wait for the signal thread each frame ....... carried by the architecture (the pass publishes the audio; unverified)

0.99.69
  * no console window beside FoxSDR's own ................................................... carried (main.cpp, merged) (unverified)
  * the RADIO SETUP page ..................................................................... re-expressed, stub batch B8 (src/gui/app_window_radio_setup.cpp; the INSTALL DRIVER key of 0.99.88 and the
    UHD images finding of 0.99.89 with it; test_radio_setup_app, test_radio_setup_inventory)
  * NFM de-emphasis in the Airband monitor ................................................... re-expressed, stub batch B6 (the engine builds an NFM channel with the 50 us network;
    test_engine_api_airband F1 reads it off the set as built); the section's note is B7's (the monitor's sentences are fact.airband.note, shown in the section)
  * an airport lookup while listening changes nothing played ................................ re-expressed in part, stub batch B6 (the engine's rule: the monitor's preset is the LISTEN's,
    a row added to another preset restarts nothing; test_engine_api_airband C2); the window's lookup leaves the group on show - and so the monitor - alone while it listens, B7
    (test_airband_app, lookup while listening; the in-process mate checkAnAirportLookupKeepsAListeningMonitorsChoice of test_gui_file_jobs is B19's)
  * a pasted report is recognised; `upload local-only` says what it means ..................... re-expressed, stub batch B14 (src/gui/app_window_report.cpp: the recognised-report
    line, the attach-the-diagnostics rows and the preview, `upload local-only` said in words, the no-email nudge on both pages; tests/test_problem_report_app.cpp, written RED first -
    this item had no pending placeholder). The bundle the page attaches also carries the signature ids of the reports on the machine and the recovered words (owner-approved
    2026-10-27: core/diag_evidence.hpp)
  * Fitted modules names the files not installed from the store ............................ re-expressed, stub batches B3 and B5 (the engine's half - `fact.plugin.origin.<file>`:
    orphan / site / removed-site - is B3's; the window's words, the row's note and the orphan's REMOVE FILE key (FOXAPP_OP_STORE_CLEAN_OLD ival[0] 1) are
    B5's: test_fitted_modules_window A, E, test_plugin_dup_app)
  * the log and every report name the other programs' DLLs ................................... re-expressed, C1 (test_foreign_modules_app, test_diag_context_app)
  * the usage report says how an unclean session ended ....................................... re-expressed, C1 (exit causes)
  * the ADS-B scope's compass labels ......................................................... carried (gui/scope_view.cpp)
  * machine translations reviewed ............................................................ carried (resources/lang); test_i18n is RED until the stubbed window
    features' strings are back (see 6.3)
  * `--rtlsdr-check` notices a dongle that stopped receiving ................................ carried (main.cpp, source/)

0.99.70
  * listening to an rtl_tcp server ........................................................... re-expressed, C1 (the kind, the open, the address box and its op;
    the remote API refuses an address-bearing open from a remote session)

0.99.71
  * the Airband monitor says what it is hearing where you look ............................... re-expressed, stub batch B7 (fact.airband.block and the open channels' names, in the rail
    above the row list and as a caption on the spectrum; test_airband_app, caption)

0.99.72
  * the plugin store laid out like a store; Fitted modules like an operating panel ........... re-expressed in part: THE STORE since stub batch B4 (the window
    around gui/plugin_store_view.cpp, master's own, in the new src/gui/app_window_store.cpp: the BROWSE and UPDATES tabs, the cards - the whole card is the
    page's key - the page with its pictures and sections in their fixed order, GET greyed until the notice's tick, GET EVERYTHING, UPDATE ALL, the window's
    rectangle kept in the config; the model is built from the table once for each listSeq move; tests/test_plugin_store_window.cpp REWRITTEN against
    master's 0.99.72 model, test_plugin_store_view, test_plugin_store_app). FITTED MODULES like an operating panel is re-expressed since stub batch B5
    (src/gui/app_window_fitted.cpp: the window around gui/plugins_view.cpp, master's own - the list, the module's page, the chips, IDLE and KEEP RUNNING, the
    command plate, CLEAN UP, RESET WINDOW SIZES - over the table's PLUGINS (value[3] the run state), its per-file facts and the two new ones of API 0.3
    9.5; tests/test_fitted_modules_window.cpp REWRITTEN against master's 0.99.72 model, restated from MASTER's drawFittedModulesWindow); the FITTED MODULES
    scenarios of test_plugin_store_app run again (the part store_app_fitted is gone)

0.99.73
  * the radio and its sound stay responsive with many modules fitted ......................... re-expressed: the audio buffer that deepens itself
    (C1, test_audio_lead_app); a fitted plugin that runs only while it is used is the ENGINE's since stub batch B2 (engine_plugin_run.cpp,
    FOXAPP_OP_PLUGIN_RUN_MODE / _USE; test_plugin_rescan_skip, the part audio_lead_plugin_wake restored, test_engine_api_03 group N). The Fitted
    modules window's half - the IDLE chip, START and the KEEP RUNNING tick, the window rows and map pages of a plugin that is asleep, listed from what the
    module declares (fact.plugin.<file>.windows) - is stub batch B5's: test_plugin_inuse_app, nine scenarios in real processes, is green
  * the DECODER OUTPUT and plugin picture windows open inside the main window; RESET WINDOW SIZES places them there ........ stubbed WINDOW-PLACEMENT
    (placeInsideMainWindow; test_decoder_window_app). Found by the gui-labelled tests, not by the release-note pass: the ledger had not listed it.

0.99.74
  * GAIN and SQUELCH dials with AUTO keys on the top bar ..................................... re-expressed, stub batch B12 (the dials and the keys are the window's,
    src/gui/app_window_deck.cpp; AUTOMATIC squelch is the ENGINE's, src/engine/engine_squelch_auto.cpp, behind FOXAPP_OP_SET_SQUELCH_AUTO and
    fact.squelch.auto / .floorDb; test_deck_knobs_app, test_theme_census, test_engine_api_squelch)
  * the STATUS column folds ................................................................. re-expressed, stub batch B1 (test_status_fold, test_status_fold_app,
    test_window_chrome_config)
  * the VIEW bank is TOOLS; Display moves to SYSTEM and starts folded ........................ re-expressed, stub batch B1 (the bank rename is in rail_banks.hpp, C0; the
    rail's heading and the Display section's place and remembered open state are the window's: test_display_fold_app, test_window_chrome_config). One name
    of test_display_fold_app's list is carved out as the red part display_fold_airband_section: the TOOLS bank's Airband section is AIRBAND (B7)
  * each decoder's settings in a folded "Plugin settings" line ............................... re-expressed, stub batch B9 (AppWindow::drawPluginSettingsRailNode;
    the same line folds the form on a plugin's own windows and the patch inspector; test_decoder_settings_fold_app)
  * the ADS-B map lists only the aircraft you can see; no-altitude is grey ................... stubbed MAP-WINDOWS
  * third-party plugin sites in the store ................................................... re-expressed in part: the ENGINE's half since stub batch B3
    (engine_store.cpp: the sites list, each site read on the catalogue's worker, merged under the official list, kept as its own copy and loaded at start-up,
    ADD with the consent and REMOVE as FOXAPP_OP_STORE_SITE, a site's plugin fitted only with the notice acknowledged and NEVER by a REMOTE session - the
    web page's rows and the install op carry the refusal sentence; test_plugin_catalogue_app, tests/test_engine_api_store.cpp S1, S2, S6) and the WINDOW's
    since stub batch B4 (the SITES panel, the consent tick that ADD waits for - the op asks for it again - the THIRD PARTY badge, the page's "From a site you
    added" line and notice; test_plugin_store_app sites, tests/test_plugin_store_window.cpp A, D2b)
  * the plugin store keeps the catalogue it read, so a failed read empties nothing (0.99.72) ........ re-expressed, stub batch B3 (the kept copy, the regional
    list's outcome, the pictures one transfer at a time; test_plugin_catalogue_app, tests/test_engine_api_store.cpp S3 and S4; the old contract that a failed
    refetch EMPTIED the catalogue, pinned by tests/test_engine_api_03.cpp I1 and I5, is replaced by master's)
  * BROWSE FILES beside a plugin setting that names a file ................................... re-expressed, stub batch B9 (the form's key over AppWindow::pickFile, B8's;
    the chosen path is an edit like a typed one; test_plugin_pathpick_app)
  * switching auto gain off gives the radio its manual gains back ........................... re-expressed, C1 (Engine::changeAutoGain; test_autogain_app)
  * a plugin may send transmit audio through FoxSDR, granted and armed ...................... re-expressed, stub batch B13 (SAFETY-CRITICAL; the engine's:
    src/engine/engine_tx_plugin.cpp; ops FOXAPP_OP_PLUGIN_TX_GRANT 0x870D, FOXAPP_OP_TX_PLUGIN_ARM 0x842B and FOXAPP_OP_TX_DATA_FLAT 0x842C, all LOCAL-only and
    none on the web profile; API 0.3 9.10; test_engine_api_txplugin, test_tx_audio_app, test_tx_audio_page_app, test_tx_audio_plate_app, test_transmit_page
    section 10). No real transmitter was keyed in the making of it: every test keys a ledger fake.
  * POCSAG settings and messages together; automatic configuration ........................... n/a (the plugin) / PLUGIN-SETTINGS-UI
  * Language & country does not freeze while it looks for CJK fonts ........................... carried (gui/fonts, merged) (unverified)

0.99.75
  * unplaced aircraft listed apart from the placed ones ...................................... stubbed MAP-WINDOWS
  * a radio's manual gain remembered and put back at its next open .......................... re-expressed, C1 (Engine::rememberManualGain / restoreManualGain;
    test_autogain_app)
  * B200 antenna/gain advice ................................................................. n/a

0.99.76
  * the spectrum's and waterfall's range set by hand ......................................... re-expressed, stub batch B11 (app_window_display.cpp: the Display block that ends
    the spectrum's menu and the waterfall's, the 4-second tip, displayRange_ - the person's choice - over the engine's window in force, sent as SET_DISPLAY_RANGE;
    the Display section's two sliders choose Manual; test_display_menu_app)
  * no more death when a window is dropped into / pulled out of a dock node .................. re-expressed, C1 (page_resize_id.hpp; test_page_resize_id)
  * a Linux scan helper with no reader is not a crash ........................................ carried (source/)

0.99.77
  * the Fitted modules / DECODE lists grouped under the store's headings ..................... re-expressed, stub batch B5 (the category is the install record's catalogue entry's:
    fact.plugin.<file>.record; the bank's rows are scoped by the plugin's run id, so two files of one plugin are one section; a double-click on a Fitted
    modules row opens that plugin's window: openPluginOwnWindow; test_plugin_inuse_app groups and dblclick, test_fitted_modules_window)
  * a row in a plugin's panel is a key ....................................................... re-expressed, stub batch B9 (panelRowKeyId / panelRowPressable hoisted into
    core/plugin_ui_data.hpp; the registered set is PLUGIN_COMMANDS and a press is FOXAPI_OP_PLUGIN_COMMAND: AppWindow::drawPluginRows; test_row_keys_app, and
    test_programme_audio_app, converted to the engine through rawEngine and the pass seam)
  * an old copy of a renamed plugin no longer loads beside the new one; ImGui's duplicate-ID box is off in the shipped build
    .......................................................................................... carried (core/plugin_host, gui/imgui_debug_policy.hpp) and held in part by
    test_plugin_host_origin (C4); the window's half - ConfigDebugHighlightIdConflicts set from the policy, its log line and the `imgui:idconflict` census
    note - was lost by the merge and is back since stub batch B5 (test_plugin_dup_app)
  * Linux builds of 41 of 43 plugins ......................................................... n/a

0.99.78
  * an aircraft at the edge of range stays on the map (estimated positions) .................. stubbed MAP-WINDOWS
  * a start-up death at the window says how it ended and which window system it used ......... re-expressed, C1 (window: lines, the failure message,
    FOXSDR_WINDOW_PLATFORM, the breadcrumb flag; test_window_failed_app)

0.99.79
  * a message from the FoxSDR team is shown whether or not there is an update ................ re-expressed, stub batch B14 (drawTeamMessages, teamMessageSeen and the
    pruning of the Seen list in pollUpdateAsync, src/gui/app_window_updates.cpp; test_team_message_app)

0.99.80
  * bookmark Import: drop zone and Browse button; by-name import; translated errors;
    Unicode filenames on Windows; Browse opens in the recordings folder ....................... re-expressed, stub batch B8 (src/gui/app_window_import_dialog.cpp, AppWindow::pickFile;
    test_import_dialog_app; the engine's answer to BOOKMARK_IMPORT_FILE opens and closes the window and the engine words the reason)

0.99.81
  * IBOC dashboard programme rows respond to clicks ......................................... re-expressed, stub batch B9 (the same rows: test_row_keys_app)
  * DAB decoder muting preserves decoded sound ............................................... carried in the core mute logic; the engine's updateAudioMute was not
    diffed against master's (unverified)

0.99.82
  * changing frequency or applying a preset does not stall the window ........................ carried (core/pipeline) (unverified under load)
  * a radio scan cannot freeze the window ................................................... re-expressed, C0 (Engine::scanNative's bounded try)
  * Wayland: a hidden window no longer blocks in the buffer swap ............................. carried (third_party/glfw: the two upstream commits, wl_window.c /
    egl_context.c / context.c; third_party/glfw/FOXSDR-PATCHES.md) (unverified: Linux/Wayland); the window-state: / window-system: report lines
    are re-expressed, C1 (AppWindow::run's frame scopes, window-system log)
  * LSB and USB reject the opposite sideband ................................................. carried (dsp/)
  * a machine with no working OpenGL driver is told so ....................................... re-expressed, C1 (window failure message)
  * a Linux audio device reporting zero periods no longer crashes ........................... carried (sink/) (unverified: Linux)
  * Bookmark Import accepts the path shapes people use ....................................... re-expressed, stub batch B8 (cleanImportPath in the window BEFORE the op; a path with no root is
    looked for on a worker; test_gui_file_jobs' section stays B19's)
  * slide the tuned band with Shift + wheel .................................................. re-expressed, stub batch B11 (slidePassband over the state, SET_VFO_OFFSET and,
    past the end of the span, SET_FREQUENCY; the picture follows for 1.5 s; the second press of a double-click on the waterfall is not a click; test_passband_slide_app)

0.99.83
  * the audio buffer comes back down by itself and says where it stands ...................... re-expressed, C1 (test_audio_lead_app)
  * the mode follows a typed band change ..................................................... re-expressed, stub batch B11 (commitTypedFrequency asks core::modeFollowsTypedBandChange
    over the rail's BAND_PLAN rows and sends SET_MODE after the tune; NEW test_typed_band_mode_app, which master never had)
  * the Radiofax recorder prints the chart on its paper .................................... carried (gui/instrument_fax.cpp)

0.99.84
  * a freeze inside a plugin reload names the reload's step ................................. re-expressed, C4 (the phases are published by the engine's rescan and
    by every caller of detachAndUnloadPlugins; core/hang_watchdog reports `reload-phase:`)

### 6.2 Totals

134 checklist items (docs/release-notes-0.99.43 to 0.99.84, one line each; an item that is split between states is counted under the
state that decides whether the user has it):

  * carried      42   (git merged them in files the branch did not restructure: core/, dsp/, source/, sink/, net/, third_party/, a pure gui/ view,
                       a catalogue). "Carried" is the merge's CLAIM. The gui-labelled tests showed it is not safe to read it as verified: nine
                       changes of master's that sat in functions the branch restructured were silently absent (section 9, commit C7), and
                       nothing but a running test found them. Where a test of the item runs, the line names it; the others are (unverified).
  * re-expressed 35   (an Engine member plus facts/ops, or a window part kept as it was: C1-C7; every line names its commit and its test)
  * stubbed      49   (NOT done: section 6.3; each has a ledger id, its master tests out of the build and replaced by a red placeholder)
  * n/a           7

49 stubbed of 134 is more than the brief's scope line (about 25 features): the merge commit and the receiver-side re-expressions (PPM, rtl_tcp,
gain memory, usage record, audio buffer, diagnostics, sentinel, async recordings and lists, plugin rescan, bandwidth log, meter faces) were
done to the house bar, and the window-side features that need the window ported (the rail chrome, the store and Fitted modules windows, the
tester pages, patch presets and view keys, markers and spectrum traces, the display range, the map lists, the radio setup, the import dialog,
the plugin settings forms, plugin run modes, window placement) were left stubbed rather than rushed. The second pass starts from 6.3.

### 6.3 The tests that are red, and why

`ctest -L merge-pending`: 54 red placeholders (51 tests, 3 parts of converted tests). Each says its ledger id. In ids (tests):

  RAIL-CHROME (7)         test_app_rail test_rail_fold test_rail_fold_app test_status_fold test_status_fold_app test_deck_knobs_app test_theme_census
  STORE-VIEW (6)          test_plugin_store_window test_fitted_modules_window test_plugin_store_view test_fitted_modules_no_disk
                          test_plugin_dup_app test_plugin_store_app   [B3 removed test_plugin_catalogue_app, test_plugin_cleanup_app and
                          test_plugin_update_cleanup_app; B4 removed test_plugin_store_window, test_plugin_store_view and test_plugin_store_app,
                          leaving test_fitted_modules_window, test_fitted_modules_no_disk and test_plugin_dup_app, and added the part store_app_fitted;
                          B5 removed the rest and the part: STORE-VIEW is whole]
  (PLUGIN-SETTINGS-UI and PLUGIN-ROW-KEYS - test_plugin_settings_ui, test_decoder_settings_fold_app, test_plugin_pathpick_app, test_programme_audio_app and
  test_row_keys_app - were removed by B9: both ids are whole)
  (TX-PLUGIN - test_tx_audio_app, test_tx_audio_page_app, test_tx_audio_plate_app and the part transmit_page_plugin_key - was removed by B13: whole)
  (TESTER-LINK - test_tester_link_app, test_link_request_poll, test_tester_link - was removed by B15: whole)
  (MARKERS-SPECTRUM and DISPLAY-RANGE - test_bookmark_stacking_app and test_freq_markers_app removed by B10; test_spectrum_trace_app, test_passband_slide_app and
  test_display_menu_app removed by B11; test_display_fold_app by B1 - are whole)
  PATCH-VIEW (2)          test_patch_canvas_nav test_patch_info_pane
  DISK-WORK (4)           test_gui_file_jobs test_failure_files test_iq_open_async test_gui_disk_audit
  one each                PATCH-PRESETS test_patch_presets_app; HEALTH-DIAG test_slow_frames_app;
                          SOURCE-ROWS test_source_row_app;
                          MAP-WINDOWS test_unplaced_list_app; WINDOW-PLACEMENT test_decoder_window_app
                          [UPDATES (test_store_update_app) and the placeholder TESTER-USAGE held for the team message (test_team_message_app) were removed by B14: UPDATES is
                          whole; TESTER-USAGE's own section had no placeholder of its own - its census test, tests/test_tester_usage_app.cpp, was written red first at B15, and
                          TESTER-USAGE is whole]
  parts (3)               transmit_page_plugin_key (TX-PLUGIN; removed by B13), excuse_cap_pickfile (IMPORT-DIALOG; removed by B8), audio_lead_plugin_wake (PLUGIN-RUN)
                          [B2 removed audio_lead_plugin_wake and added nothing; B1 had added display_fold_airband_section (AIRBAND); B4 added
                          store_app_fitted (STORE-VIEW: the FITTED MODULES scenarios of test_plugin_store_app); B5 removed store_app_fitted; B1's display_fold_airband_section (AIRBAND) and the test test_airband_app were removed by B7: AIRBAND is whole]

UPDATE 2026-10-26, stub batch B1 (docs/engine-stage4.md 6.22): test_app_rail, test_rail_fold, test_rail_fold_app, test_status_fold, test_status_fold_app
and test_display_fold_app are converted and green, and the placeholders of those six are gone (RAIL-CHROME is now test_deck_knobs_app and test_theme_census,
DISPLAY-RANGE is test_display_menu_app). A part is added: display_fold_airband_section (AIRBAND), the one name of test_display_fold_app's TOOLS-bank list
that names a feature not yet on this window. test_i18n: 155 stale, 1 missing (the `VIEW` heading is gone from the source).

UPDATE 2026-10-26, stub batch B2 (docs/engine-stage4.md 6.22): test_plugin_rescan_skip is converted (it now also runs as _stepped) and the part
audio_lead_plugin_wake is restored in test_audio_lead_app: both placeholders are gone, 47 remain (test_plugin_inuse_app, PLUGIN-RUN's window half, stays for B5).
The engine decides which plugins have instances; a window-less host runs every plugin (owner decision). test_i18n: 153 stale, 1 missing.

UPDATE 2026-10-26, stub batch B3 (docs/engine-stage4.md 6.22): test_plugin_catalogue_app, test_plugin_cleanup_app and test_plugin_update_cleanup_app are
converted to Engine members (through rawEngine and the pass seam; each now also runs as _stepped) and green, and their placeholders are gone: 44 remain.
The engine keeps the catalogue it read (and the sites' copies), lists the sites a user added, fetches a plugin's pictures one transfer at a time and removes
the old copies an update leaves, none of it on the pass; a REMOTE session cannot fit a plugin from a site. test_i18n: 152 stale, 1 missing (the Catalogue-source
tooltip, B4's); the one key that returned is the refusal sentence.

UPDATE 2026-10-26, stub batch B4 (docs/engine-stage4.md 6.22): test_plugin_store_window (REWRITTEN against master's 0.99.72 model, restated from MASTER's
buildPluginStoreModel and fillStoreModule; it keeps its _stepped and _free registrations), test_plugin_store_view (un-pended unchanged: 387 checks) and
test_plugin_store_app (the store and sites scenarios) are green and their placeholders are gone. The one red part added is store_app_fitted: the FITTED
MODULES scenarios of test_plugin_store_app read the 0.99.72 Fitted modules window, which is B5's (master's text is untouched behind
CASCADE_STORE_APP_FITTED_PART). The red placeholders are therefore the 44 of B3, less three, plus that one part: 42 (38 tests and 4 parts). The engine grew: facts for what a card draws (category, whatsNew, published, sha256,
capabilities, install state, the installed copy's file), the version-aware "installed" rule of master's 0.99.72, and UPDATE ALL (FOXAPP_OP_STORE_ADD_ALL
ival[1] 1). test_i18n: 149 stale, 0 missing (the three keys that returned: "From a site you added: %s", "Offered by foxsdr.com in some countries only." and
the Catalogue-source tooltip's last two lines).

UPDATE 2026-10-26, stub batch B5 (docs/engine-stage4.md 6.22): test_fitted_modules_window (REWRITTEN against master's 0.99.72 model, restated from MASTER's
drawFittedModulesWindow; it keeps its _stepped and _free registrations), test_plugin_dup_app, test_plugin_inuse_app and test_fitted_modules_no_disk (an
expectation edit, with its reason beside it) are green and their placeholders are gone, and so is the part store_app_fitted (master's text of the FITTED
MODULES scenarios of test_plugin_store_app is back as it was): STORE-VIEW and PLUGIN-RUN are whole. The red placeholders are the 42 of B4, less four tests and
one part: 37 (34 tests and 3 parts). The engine grew by two facts a module (API 0.3 9.5: the install record and the module's own window titles), the preset
event's value[0] (the preset's flags), and three lines of applyPluginPreset / maybeAutoPreset that the merge had lost (CASCADE_PRESET_FLAT_AUDIO,
the explicit rate request, CASCADE_PRESET_KEEP_TUNED_ON_START). The window got back what the merge had lost around it: the `switchrow:` census rect, the
`image:window:` / `panel:window:` / `instrument:window:` / `decoder:window:` census rects, the ImGui ID-conflict setup. test_i18n: 147 stale, 0 missing (the two keys
that returned: "Installed from %s, a site you removed. It gets no updates." and "Installed from %s, not from the FoxSDR catalogue.").

UPDATE 2026-10-26, stub batch B6 (docs/engine-stage4.md 6.22): no placeholder is removed - test_airband_app is B7's - so the red set is the 37 of B5 (34 tests and 3
parts) and its names do not change. The monitor, the scanner's list mode and the frequency list's tick are the ENGINE's (src/engine/engine_airband.cpp; API 0.3 9.6:
FOXAPP_OP_AIRBAND 0x8429, _BOOKMARK_TICK 0x8604, _BOOKMARK_ADD_ROWS 0x8605, _SCANNER_LIST 0x860A, the live list AIRBAND_CHANNELS 51, fact.airband.* and
fact.scanner.listMode / .ticked), held by the new tests/test_engine_api_airband.cpp (386 checks, each group shown red against an engine whose ops answered UNSUPPORTED,
then against 14 mutants) and by four new cases of test_apply_command. The window drew back two keys: the Bookmarks tick column and the Scanner's "Ticked frequencies"
(held by the new real-binary tests/test_bookmark_tick_app.cpp, gui label, 37 checks, against 3 mutants; two of this branch's own port tests, test_scanner_draft_input and
test_recorder_bookmarks_scanner, had their expectations edited for the two new widgets). test_i18n:
144 stale, 0 missing (the three keys that returned: "Ticked: scanned by the Scanner's \"Ticked frequencies\"..." (the tick's tooltip), "Ticked frequencies (%zu)" and its
tooltip).

UPDATE 2026-10-26, stub batch B7 (docs/engine-stage4.md 6.22): the AIRBAND section is back in the window (src/gui/app_window_airband.cpp, src/gui/airband_window_state.hpp; the parked
file is gone). test_airband_app is converted (the real binary, 330 checks: the 316 of the parked test and a new scenario, an airport looked up while the monitor plays a preset) and the
part display_fold_airband_section is restored (test_display_fold_app's TOOLS-bank list is master's again, "section:Airband###airband" included): both placeholders are gone, 35 remain (33
tests and 2 parts). Not engine work: no op, list or fact was added. The preset's IMPORT of a typed path and EXPORT CSV are window-side DiskJobs (the existing BOOKMARK_IMPORT_FILE carries no group
and BOOKMARK_EXPORT writes SDR# XML), which B19 should fold into the disk-work file; the import-dialog hook (openImportDialog) is empty until B8; the tester recorder's feature notes
(NOTEFEATURE call sites) are B15's and the squelch AUTO follow-up is B12's. test_i18n: 144 stale, 0 missing (unchanged: the parked file's keys were already counted).

UPDATE 2026-10-26, stub batch B8 (docs/engine-stage4.md 6.22): the IMPORT-DIALOG and RADIO-SETUP pages are back in the window (src/gui/app_window_import_dialog.cpp, src/gui/app_window_radio_setup.cpp, git mv from src/gui_pending; AppWindow::pickFile in app_window.cpp, FOXSDR_FILE_PICK and FOXSDR_FAKE_RADIO_SETUP read again by run()). test_import_dialog_app and test_radio_setup_app are converted (the real binary: 114 and 156 checks) and the part excuse_cap_pickfile is gone (tests/test_excuse_cap.cpp counts master's 3 native-wait call sites): both placeholders and the part are gone, 32 remain (31 tests and 1 part). The ledger lines of 0.99.69 (the page), 0.99.80 (the dialog) and 0.99.82 (the path shapes) read re-expressed above; R1 (the INSTALL DRIVER key, 0.99.88) and R2 (the UHD images finding, 0.99.89) of docs/engine-merge-0.99.90.md section 3 are done with the page. test_radio_setup_app had NO scenario for the INSTALL DRIVER key (the plan could not tell): one was written RED first and now holds it, with a new bounded-run seam FOXSDR_FAKE_DRIVER_INSTALL; a new pure test, tests/test_radio_setup_inventory.cpp, holds the page's reading of the application's lists. ONE ENGINE CHANGE the plan did not list: FOXAPP_OP_BOOKMARK_IMPORT_FILE is answered FAILED (it was OK with only the note saying so) when it read nothing, and the note words the reason in the user's language - the window opens and closes its dialog on that answer (docs/api-0.3-additions.md section 9). Three existing tests were edited with their reasons beside the edit (test_recorder_bookmarks_scanner H, test_source_section_window's sweep helper with a new scenario A0 for the RADIO SETUP key, test_apply_command's added checks). The page raises itself again for a person with no radio: master counted the always-present rtl_tcp row as a radio found (decision 3 of the B8 note). Not engine work otherwise: no op, list or fact was added. test_gui_file_jobs' import sections stay B19's (it calls `consumePendingDrop`, which is AppWindow::submitPendingDrop here). test_i18n: 144 stale, 0 missing (unchanged).

UPDATE 2026-10-26, stub batch B9 (docs/engine-stage4.md 6.22): the PLUGIN-SETTINGS-UI and PLUGIN-ROW-KEYS items are back (src/gui/plugin_settings_form.cpp, git mv from src/gui_pending, over a gui::SettingsEditSink; src/gui/app_window_plugin_settings.cpp; src/engine/engine_plugin_settings.cpp; the pressable-row rule hoisted into core/plugin_ui_data.hpp). The table grew by the list PLUGIN_SETTINGS (52), two facts a field and FOXAPP_OP_PLUGIN_SETTING_EDIT 0x870C (API 0.3 9.7; LOCAL only; the door did not move). test_plugin_settings_ui (edited: the adapter over PluginApiCore, its reason beside it, and the three publish calls; a section [S] added), test_decoder_settings_fold_app, test_plugin_pathpick_app (four scenarios of mine added), test_programme_audio_app (converted to the engine through rawEngine and the pass seam; it now also runs as _stepped) and test_row_keys_app are green and their placeholders are gone; PLUGIN-SETTINGS-UI and PLUGIN-ROW-KEYS are whole. New: tests/test_engine_api_settings.cpp and tests/test_plugin_settings_window.cpp (the real window against the real engine). The red placeholders are the 32 of B8 (31 tests and 1 part) less five tests: 27 (26 tests and 1 part). test_i18n: 143 stale, 0 missing (the one key that returned: "Plugin settings"). The preset lines B2 flagged are in the engine (verified line by line before test_programme_audio_app was converted).

UPDATE 2026-10-26, stub batch B10 (docs/engine-stage4.md 6.22): the waterfall's markers (0.99.57), the close bookmark names that stack (0.99.54) and the press that is held (0.99.57) are back (src/gui/app_window_markers.cpp, src/engine/engine_markers.cpp; drawBookmarkMarkers and the Bookmarks section's "Stack close names" key in app_window.cpp; the window half of the config, which had dropped bookmarkMarkers_ and bookmarkStackNames_). The table grew by the ops FOXAPP_OP_MARKER_DROP / _REMOVE / _CLEAR / _NOTE 0x8606 - 0x8609 (LOCAL only, not on the web profile), the list FREQ_MARKERS (53, LOCAL only) and the facts fact.markers.version / .note.<id> / .error (API 0.3 9.8; the door did not move). test_freq_markers_app and test_bookmark_stacking_app are green, master's, unedited, and their placeholders are gone. New: tests/test_engine_api_markers.cpp (349 checks) and tests/test_markers_window_app.cpp (37 checks; the Markers window's note, Tune, Bookmark, x and Clear against the real binary); test_apply_command gained the four ops (133 implemented, 133 tested). The red placeholders are the 27 of B9 (26 tests and 1 part) less two tests: 25 (24 tests and 1 part). test_i18n: 122 stale, 0 missing (the 21 keys that returned are itemised in 6.22). Not here: the Display block of master's 0.99.76 that ends the marker menu, and the display tip - B11's.

UPDATE 2026-10-26, stub batch B11 (docs/engine-stage4.md 6.22): the spectrum's peak hold and average (0.99.57), the display range menu and its 4-second tip (0.99.76), Shift + wheel to slide the tuned band (0.99.82) and the mode that follows a typed band change (0.99.83) are back (src/gui/app_window_display.cpp, new; call sites in app_window.cpp and the Display block in app_window_markers.cpp). WINDOW STATE OVER EXISTING WORDS: no op, list or fact was added, the door did not move; the dB window in force goes to the engine as FOXAPI_OP_SET_DISPLAY_RANGE and the person's choice of Automatic or Manual and the pair Manual returns to are the window's, saved by currentConfig as waterfallRangeMode, dbMin and dbMax (the engine's own dbMin/dbMax are the window in force, which Automatic makes the standard one). test_spectrum_trace_app, test_passband_slide_app and test_display_menu_app are green, master's, unedited, and their placeholders are gone. New: tests/test_typed_band_mode_app.cpp (master never had one: "none own - NEW assertion", plan section 2 S49). The red placeholders are the 25 of B10 (24 tests and 1 part) less three tests: 22 (21 tests and 1 part). test_i18n: 106 stale, 0 missing (the 16 keys that returned are itemised in 6.22).

UPDATE 2026-10-26, stub batch B12 (docs/engine-stage4.md 6.22): the GAIN and SQUELCH dials with their AUTO keys on the top bar (0.99.74) are back (src/gui/app_window_deck.cpp and gui/deck_parts.hpp, new; drawToolbar calls drawDeckKnobs, the bar's floor scale is 0.57 again as master has it, the meters scale with the deck, the bias key never falls under 12 px and the mute banner measures the middle against the cluster as drawn). AUTOMATIC squelch is the ENGINE's (src/engine/engine_squelch_auto.cpp): the table grew by the op FOXAPP_OP_SET_SQUELCH_AUTO 0x842A (LOCAL only, not on the web profile) and the facts fact.squelch.auto / .floorDb (API 0.3 9.9; no list, no header change, the door did not move); AppConfig::squelchAuto is the engine's half of the config now, and an accepted SET_SQUELCH, the monitor's squelch or a pressed above-the-noise turn it off. test_deck_knobs_app (converted: the fake radio through the engine's hooks, the run's end state through rawEngine) and test_theme_census (master's, unedited) are green and their placeholders are gone. New: tests/test_engine_api_squelch.cpp (the rule, in the engine, with no window); test_apply_command gained the op (134 implemented, 134 tested); test_engine_main_config_roundtrip knows squelchAuto is now written by the engine (its reason is beside the edit). The red placeholders are the 22 of B11 (21 tests and 1 part) less two tests: 20 (19 tests and 1 part). test_i18n: 98 stale, 0 missing (the 8 keys that returned are itemised in 6.22).

One more red test is a consequence and is not in the list: **test_i18n** (2 keys missing, 162 stale in every catalogue). The stale keys are the
strings of stubbed features (they return with them); the two missing are the Catalogue-source tooltip's last two lines (they send the reader to
SITES in the store, which is STORE-VIEW) and the "VIEW" bank heading (RAIL-CHROME renamed it TOOLS; the branch's rail still says VIEW). It is
left red on purpose: loosening it would hide the strings the second pass owes.

## 7. API additions (docs/api-0.3-additions.md, section 9.1)

No change to `third_party/foxsdr_api/foxsdr_api.h` (0.3's header stands) and no new fingerprint field: five APP ops and the facts they move.

  * ops (core/app_commands.hpp, all LOCAL sessions, listed in kOps): 0x8424 FOXAPP_OP_SET_RTLTCP_ADDR, 0x8425 FOXAPP_OP_SET_AUDIO_BUFFER,
    0x8426 FOXAPP_OP_SET_PPM, 0x8427 FOXAPP_OP_TEST_HOLD_AUDIO_LOCK (bounded runs only), 0x8428 FOXAPP_OP_BANDWIDTH_DRAG_END.
  * facts (get_setting): `source.rtlTcpAddr`, `audio.bufferMs`, `audio.leadMs`, `audio.leadNote`, `ppm.on` `ppm.method` `ppm.value` `ppm.whole`
    `ppm.error` `ppm.card` `ppm.model` `ppm.serial` `ppm.key`. Hashed into the publish fingerprint from the raw members they are built from, with
    no string built (test_engine_publish_alloc holds it).
  * the door (not the table): EngineShell::shutdownRelease() lost its argument and EngineShell::shutdownDrain(deadline) is new - the plugins come
    down BEFORE the clean-exit marker (master 0.99.60), the drain of the lists' and takes' last writes after it. test_engine_shell_guard: 40
    methods, 72 call sites (was 39 / 71).
  * moved, not added: ConfigWriter and BackgroundSaver now live in src/core (the engine's saves need them and the CMake guard forbids the engine
    including src/gui).

## 8. Traps found on the way (for the second pass and for the next merge)

  1. THE SURVEY IS NOT THE CHECK. docs/engine-merge-0.99.84-lostlines.txt is a function-level diff; it missed whole statements in functions the
     branch restructured (the clean-exit marker's place in the teardown, the waterfall's height, a log line, the input script's deaf mode and
     verbs, the Add-all rule, a watchdog seam). Every one was found by RUNNING a test: the gui label found six, test_i18n one. Run
     `ctest -L gui` on the hidden desktop before calling a window merge carried.
  2. The gui-labelled tests are not in the `headless` label. A headless-only run of this merge was 41 red of 495, all explained - and blind to
     28 failures of the real-binary tests.
  3. `hidrun.cmd` / run_on_hidden_desktop.ps1: a ctest regex with `^` or `|` is eaten by cmd.exe; drive ctest from a .cmd file with the names in a
     list file.
  4. Heredocs through the tool layer collapse `\\n` and `\\0` to a real newline and a real NUL byte: a Python edit of app_window.cpp wrote a NUL into
     a char literal twice (grep said "Binary file matches" - that is the tell). Build the escape with chr(92).
  5. tests/tsan.cmake is stored with CR CR LF line ends on the branch (it was at b4584aa); editing it with an LF-writing tool rewrites every line.
     Edit it as bytes.
  6. GCC refuses a default member initializer in Engine::TestHooks (an aggregate used as `inline static TestHooks testHooks_{}` inside the class):
     the first Linux error of the merge. A TSan build of master here also needs `called_from_lib:libpulse.so.0` in tools/sanitize/tsan.supp.
  7. This WSL's TSan leg: `test_plugin_audio`, `test_sdrplay_source` and `test_transmitter` fail on a TSan build of origin/master itself with the
     branch's suppressions (races in the tests' own fake-plugin and fake-device fields); they are not the merge's.
  8. `$t` inside `wsl bash -lc '...'` called from Git Bash expands to empty before WSL sees it: put loops in a script file and run the file.

## 9. Status at the end of this pass

Commits on engine/stage4 (none pushed): C0 2c5d14f (the merge of origin/master f28d40b), C1 96093f8, C2 168ff14, C3 00e35d2, C4 26203b7, C5 f7cfb85,
C6 0631eb6, C7 c4a7517 (the gui label's first run), and the commit that carries this section.

Verification (this tree, after C7):

  * Build: a clean rebuild (`cmake --build build --config Release --clean-first --parallel 24`), 0 errors; incremental builds after C7 clean.
  * Headless label, `ctest -L headless -j 8`: 522 tests; the red ones are the 54 placeholders and test_i18n (6.3). Two other reds appeared once and
    are not real: test_file_pick (a -j 8 flake; 3 of 3 solo) and test_diagnostics_free (compares the binary's embedded commit with HEAD: stale until
    the rebuild after C7; 3 of 3 after).
  * Gui label on the hidden kernel desktop (`-j 1`): 23 of 23 pass (46 before the stubbed ones left it). No straggler cascade/ctest/test_ process
    afterwards (the two cascade.exe in C:\Program Files\FoxSDR are the installed product, not these runs).
  * Fence probes: interface_fence_allows, 12 denies / 12 controls - all pass. test_engine_shell_guard passes (40 / 72).
  * TSan (WSL Ubuntu, GCC 15, `-DCASCADE_SANITIZE=thread`, `ctest -L tsan -j2`, 108 tests): 103 pass, 5 fail.
      - test_plugin_audio, test_sdrplay_source, test_transmitter: the same race at the same test line on a TSan build of origin/master here (section 8.7).
      - test_engine_api_transmit: fails its throughput assertion (`issued.size() > 1000`: 533 to 780 new tickets); the same binary built from the
        branch BEFORE the merge (b4584aa) run three times directly reads 625, 744, 810 - below the line as well - and passed once under ctest, so the
        assertion straddles its threshold on this machine regardless of the merge. Not attributed to the merge.
      - test_engine_api_merge: EXPLAINED AND FIXED IN SECTION 10 (real-time threads from master's 0.99.73 starving the control threads). Before the merge: 106 s on 24 threads, 0 differ. After: about 465 s, 783-851 differ
        (every difference a keep-alive that lapsed - "the interface holding the key stopped answering" - so the engine's passes are slower under
        TSan than the test's timers allow). Not caused by pumpDiagContext / healthPoll / pollSdrPlayTrouble / exitCausePoll (removing the four per-pass
        calls: 471 s, 851 differ). UNVERIFIED which cluster: a TSan build of C0 (the raw merge, before the re-expressions) does not get that far - the same test stops on an
        unsymbolised data race there - so the bisect is C1..C7 and is the second pass's first task (build one commit at a time into a scratch
        tree with the typo-check and exclusion fixes of C7 applied; ~12 min a build, ~8 min a run).
  * Visual (hidden desktop, scratch config, SOAPY_SDR_PLUGIN_PATH cleared, master built into a scratch dir; frames 150 / 100 settled; the real
    %APPDATA% config was never read or written - every run set APPDATA to a scratch tree): four features compared against master's own build:
    the meter faces (LED ladder on VOLUME, needle on SAMPLE RATE) - identical; the right-click meter-style menu with the checked entry - identical;
    the PPM frequency-correction section - identical; the receiver view (spectrum, splitter, waterfall to the view's bottom) - identical after the
    waterfall fix. The only visible differences are the known stubs: the GAIN and SQUELCH dials, the rail and status fold keys, the RADIO SETUP
    button, the patch page's PRESETS button and information pane.

Not verified: every item of the ledger marked carried without a named test; Linux and Wayland behaviour of the carried glfw patch; the stubbed
features by definition; rtl_tcp and the audio-buffer combo were not compared visually (their tests, test_audio_lead_app and test_apply_command,
run and pass).

## 10. The test_engine_api_merge slowdown: real-time threads starving the engines' own control threads

Found by the push-gate pass (the second pass's first task), 2026-10-21. Method: scratch TSan builds of b4584aa (the pre-merge control), C0 (2c5d14f)
and the tip (2399ae2) in WSL (Ubuntu, GCC 15, `-DCASCADE_SANITIZE=thread`, 24 engines on 32 processors), run through ctest with the leg's own
TSAN_OPTIONS (symbolize=1: see "traps" below).

Measured, `test_engine_api_merge`, wall time / user CPU / differences:

  b4584aa (control)        106 s   3,160 s CPU    0 differ   (12 runs: 11 x 0, 1 x 1 - the flake below)
  tip 2399ae2              459 s  14,120 s CPU  793 differ,  19 with a missing result
  C0 2c5d14f (the merge)   470 s  14,420 s CPU   (halted by a PortAudio race at the end: below)
  tip, CAP_SYS_NICE dropped (`setpriv --bounding-set=-sys_nice`)   107 s   3,169 s CPU   0 differ
  tip + the fix            106-108 s in 24 runs: 21 x 0 differ, 3 x 1 differ (the flake below; 11 of the 24 ran a test copy that also logged a
                           setUp latch that did not take - it never fired, and none of those 11 differed)
  C0 + the fix             104 s and 106 s, 0 differ

THE MECHANISM. src/core/thread_priority.cpp (master 0.99.73, "protect audio on slower machines"; the file is NOT in b4584aa; it arrived with
the merge, C0, through src/core/pipeline.cpp's dspThreadMain and sourceThreadMain, which git merged without a conflict). On Linux the DSP thread
and the source thread of every Pipeline call pthread_setschedparam(SCHED_RR, priority 1). An ordinary user is refused (EPERM) and the thread
is left alone - the case on GitHub's runners and on a Linux user's machine - but a privileged process is granted it, and the WSL TSan leg runs as
root. `ps -L` on the running test: 41 threads in class RR, priority 1, each at about 100 % CPU (under ThreadSanitizer the DSP chain runs flat out);
none at b4584aa. 24 engines put 48 real-time threads on 32 processors, and every ordinary thread - each engine's control thread, the transmitter's
thread, the test's workers - gets what the real-time throttle leaves (95 % of each second for real-time work). A control pass that does not run for
over a second is a lapsed keep-alive (src/core/tx_key_rules.cpp:251, "the interface holding the key stopped answering", the 1 s local limit), which
is why every one of the 783-851 differences was one; the sweep takes 4.3x the wall time and 4.5x the CPU because everything outside the real-time
threads is waiting for them. Nothing in the key's rules, the session heartbeat path, steadyMs or the control pass changed (src/core/tx_key_rules.cpp
and fox_engine_api.cpp's pass are identical to b4584aa's apart from one validate() line), and the four per-pass polls the first pass suspected
were innocent: they cost nothing next to this.

IT IS MASTER'S PRODUCT BEHAVIOUR AND IT IS KEPT: a real-time DSP thread is what 0.99.73 is for, one engine on a machine with a free processor does
not starve its control thread, and the keep-alive lapsing is the safe direction (a lapse drops the key). What was wrong was the test rig: it asks the
operating system for real-time scheduling 48 times over in one process, which no user's machine does.

THE FIX (3 files and a test). `ThreadPriorityScope::setDisabledForTest(bool)` (core/thread_priority.hpp/.cpp): on every platform the scope asks for
nothing and says "none: switched off for a test"; `engine_api_test::hermetic()` (tests/engine_api_test_env.hpp), which every engine-API test calls
first, switches it on. RED first: tests/test_engine_api_hermetic.cpp (new; in the TSan selection) runs `hermetic()`, starts a Pipeline, and requires
its two thread outcomes to be "none: ..." and, on Linux, that no thread of the process is SCHED_FIFO/SCHED_RR (/proc/self/task/*/stat field 41):
RED on the unfixed tip as root ("sched-rr", "sched-rr", 2 real-time threads), GREEN after. tests/test_dsp_priority.cpp gained section 2b: the
seam asks for nothing and is off again afterwards. The priority request itself is still tested, unchanged, by test_dsp_priority's other sections.

SEPARATE FINDINGS OF THE SAME PASS (neither caused by the merge):
  * THE C0 TSan RACE was not the slowdown. It is PortAudio's ALSA backend: `stream->isActive = 0` in OnExit (the callback thread's last act,
    pa_linux_alsa.c:3415) against `return stream->isActive` in IsStreamActive (:3165), reached through Pa_IsStreamActive from
    AudioOut::streamAlive() on the engine's control thread (C0: fillPublishedState, a b4584aa call; the tip: also refreshDiagContext, which the
    merge added, at teardown). A plain unsynchronised status flag, harmless on its own, and the only race of the leg that survives the fix: once in
    9 runs at the tip (it halts the process: halt_on_error=1), once in 1 at C0, none in 12 at b4584aa. tools/sanitize/tsan.supp: `race:IsStreamActive`,
    by the function that reads it (the RealStop precedent), with the report and the reasoning beside it.
  * THE ONE DIFFERENCE IN ABOUT EIGHT RUNS is old: b4584aa shows it too (1 run in 12; the fixed tip 3 in 24 - not distinguishable at these counts). It is
    always the same shape - ONE side's state is empty where the other's is marked, in either direction ("one a pass: ended=0 marks={}" at b4584aa and
    twice at the tip; "merged: marks={}" once) - which reads as that side's setUp latch not having taken (setUp checks the submit status, never the
    latch's result). Which load-dependent rule refused it is UNVERIFIED: an instrumented copy of the test that logs a latch result other than OK did not
    catch one in 11 runs. The test reports it as a difference; it is not the regression, and the push should not be read as promising 0 on every run.
  * test_engine_api_transmit's throughput line (`issued.size() > 1000`) fails about half the runs at b4584aa too (1 pass, 2 fail in 3 ctest runs here).
  * test_audio_lead_app (and its _stepped variant) failed on this Windows checkout, not on the merge: section 12 scans src/engine/engine.cpp for a
    function's end as "\n}\n" after a BINARY read, and this working copy holds CRLF (autocrlf=true; the index is LF, and the files the tool layer
    wrote were LF until a later git operation rewrote engine.cpp). It now reads the file as LF. Red before (3 checks failed, section 12), green after.

TRAPS (add to section 8):
  9. TSAN_OPTIONS=symbolize=0 HIDES THE SUPPRESSIONS: a race:/called_from_lib: line is matched by function or library name, which an unsymbolised
     stack does not have, so every suppressed race halts the run. The merge pass's "C0 stops on an unsymbolised race before the sweep prints" was
     that, plus the one real PortAudio race above; its direct-run experiments (symbolize=0) cannot be compared with the leg's. Run through ctest.
  10. `git archive` on this machine (autocrlf=true) writes CRLF: a scratch build then fails to parse tools/sanitize/tsan.supp ("failed to parse
     suppressions", exit 66) and tools/*.sh (`set: -: invalid option`). `sed -i 's/\r$//'` on those two before building.
  11. A WSL shell started from Git Bash rewrites a leading-slash argument (`wsl bash -lc '/mnt/c/...'` becomes `C:/Program Files/Git/mnt/c/...`):
      set MSYS_NO_PATHCONV=1. And a process started with `&` inside `wsl -e` dies with the shell; use the tool's own background mode.

VERIFICATION OF THE FIX (Windows, Release, this tree after the fix): `ctest -L headless -j 8` 523 tests; red: the 54 merge_pending placeholders, test_i18n (6.3),
and test_audio_lead_app / _stepped (fixed afterwards, 2 of 2 green). test_engine_api_merge 15.6 s, test_engine_api_transmit 98 s, test_remote_control_pull,
test_transmit_key_race, test_remote_key_release (and their _stepped/_free variants), test_dsp_priority and the new test_engine_api_hermetic pass.
WSL TSan, the touched sets (test_engine_api_conformance, _03, _merge, _transmit, test_remote_control_pull, test_transmit_key_race, test_remote_key_release,
test_dsp_priority, test_engine_api_hermetic): all pass except test_engine_api_transmit (its throughput line, above) and one test_engine_api_merge run
halted by the PortAudio race above, before its suppression. test_engine_api_rds_real, test_engine_publish_alloc and test_plugin_status_report are not
in the TSan selection. NOT VERIFIED: the full TSan leg (`ctest -L tsan`, 108 tests) was not re-run end to end; the suppression was never seen
to fire (the race it names appears about one run in ten and did not recur in the 11 runs after it was added).

UPDATE 2026-10-27, stub batch B13 (docs/engine-stage4.md 6.22, the safety record): a plugin may key the transmitter again (0.99.74's CASCADE_CAP_TX_AUDIO), and the key is the ENGINE's: the grant, the
arming window and the decision are on the engine's pass (src/engine/engine_tx_plugin.cpp, called from the key drive ApiHostImpl::txPass between the hands' levels and Transmitter::tick). Three ops,
every one LOCAL-only and off the web profile (still 38 ops): FOXAPP_OP_PLUGIN_TX_GRANT 0x870D (NOT a kind of FOXAPI_OP_PLUGIN_GRANT, which a web page may send), FOXAPP_OP_TX_PLUGIN_ARM 0x842B (a
LEASE on the session that opened it) and FOXAPP_OP_TX_DATA_FLAT 0x842C; facts fact.tx.plugin.* and fact.plugin.<file>.tx (API 0.3 9.10). test_tx_audio_app (converted: the page is a stand-in for the
commands the real page sends; _stepped and _free), test_tx_audio_page_app and test_tx_audio_plate_app (the real binary; unedited) are green and their placeholders, and the part
transmit_page_plugin_key (re-pinned against the engine's drive), are gone: the red placeholders are the 20 of B12 (19 tests and 1 part) less three tests and the part: 16 (16 tests, no part). New:
tests/test_engine_api_txplugin.cpp (the engine, a real transmitting module, a ledger radio, no window). test_i18n: 88 stale, 0 missing (the 10 keys of drawTransmitPage that returned are itemised in 6.22).

UPDATE 2026-10-27, stub batch B14 (docs/engine-stage4.md 6.22): UPDATES and PROBLEM-REPORT. The update check's whole window half is back in src/gui/app_window_updates.cpp, from master's text
of 0.99.90 (unchanged to 0.99.95): a Microsoft Store copy asks the STORE (core/store_update.hpp; an unpackaged copy never does, an unticked box asks nobody), the Store banner with its install key and
the outcome of the install, the Updates section's Store states, a message from the FoxSDR team above the banner, "Seen" kept in the settings file, and the Seen list pruned to the ids the last
successful answer still carries. test_store_update_app (63 checks, with a new "Not now" section) and test_team_message_app (65) are green and their placeholders are gone: the red placeholders are the
16 of B13 less two: 14 (14 tests, no part). The page REPORT A BUG / DISLIKE and the feature page moved to src/gui/app_window_report.cpp with master's text of the rest of 0.99.69: the
recognised-report line, "Attach the diagnostics log", "Attach the SDRplay diagnostic too" (its text is B18's; nothing fills it yet, so the row is never drawn), "Show what will be sent" and the sentence for
`upload local-only`, the no-email nudge on both pages. That item had no pending placeholder, so its test is NEW and was written RED first (tests/test_problem_report_app.cpp: 95 checks, 24 failed against
the page as it was; 109 checks, 0 failed after). Owner-approved addition of 2026-10-27: the diagnostics bundle (and so the attachment) carries two more header lines, `recent-faults:` (signature id and
kind of the newest reports on the machine) and `recovered:` (what the program recovered from this session, fixed words and counts) - core/diag_evidence.*, tests/test_diag_evidence.cpp, PRIVACY.md,
docs/DIAGNOSTICS.md. THE BREADCRUMB TRAIL JOINS POST-MERGE (it lives on master's field-early-warning branch). Where the machine cannot say, the line says so: `recovered:` reads "(not counted - usage
reporting is off)" while the health ledger is not counting, which is how a run with usage reporting off sees it. test_i18n: 62 stale, 0 missing (the 26 keys of the five window functions returned).

UPDATE 2026-10-27, stub batch B15 (docs/engine-stage4.md 6.22, the B15 record): TESTER-USAGE and TESTER-LINK. The beta tester's two flows are back in the WINDOW (src/gui/app_window_tester.cpp, from master's text of
0.99.90, unchanged to 0.99.95): SYSTEM > Beta tester (paste a code, Remove code, a confirmed link by name with Unlink, "Show what is sent" with the token masked), the usage record (the recorder, the retry queue
and the sender are the window's: a headless engine cannot hold a code or send a report), the link-request file polled on a worker and the "Link this FoxSDR?" prompt with its confirm-by-name and portal-token
exchange. The recorder is fed by the window's own events (the one-line marks beside each control, the four Airband NOTEFEATURE sites included) and by facts the table already publishes (a take, the scanner,
RF going out, the radio in use, the plugin minutes); the two listeners' marks ("browser", "cat") come through ONE new door method, `EngineShell::setServerUsageHooks` (CLASS HOST, END stage 7h: test_engine_shell_guard
HOST budget 2 -> 3, call sites 65 -> 66), called once by the window's constructor. src/core/tester_usage.cpp and tester_link.cpp were split with the code unchanged (tester_usage_session.cpp, tester_link_net.cpp) so that
the built foxsdr-engine.exe does not contain the usage endpoint or the portal's paths. test_tester_link_app and test_link_request_poll are converted (the second with one expectation edit: the wiring scan reads
app_window_tester.cpp), test_tester_link is un-pended with its token-never-logged scan re-pointed at engine/process_boot.cpp (where stage 7a put the activation; the old scan of main.cpp passed vacuously), all
three placeholders are gone: the red placeholders are the 14 of B14 less three: 11 (11 tests, no part). New: tests/test_tester_usage_app.cpp (the section's census test, written RED first: 63 checks, 35 failed
against the window as it was) and tests/test_tester_host_only.cpp (a headless engine can never send a tester report: the sources, the process boot, the built images, the running engine against a stand-in site, and the
running window as the control); test_airband_app gained the two Airband marks. test_i18n: 36 stale, 0 missing (the 26 keys returned). The real settings file is untouched.
