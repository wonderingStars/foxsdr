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
    `anyKey` now includes the plugin key (it did not: the plugin's audio was never pulled; found by test_tx_audio). NOTHING in the engine
    calls setPluginKey yet (ledger TX-PLUGIN): a plugin cannot key a radio, which is the safe state. The engine API's TX_SET_INPUT range is
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

(Filled in at the end of this pass: sections 7 and 8.)

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
  * a plugin's settings UI, CASCADE_CAP_SETTINGS_UI .......................................... stubbed PLUGIN-SETTINGS-UI (test_plugin_settings_ui; core and
    gui/plugin_settings_form.* are in the tree and their pure tests run; the window's forms and the engine's grants are not wired)

0.99.44
  * interface size, for 4K screens ........................................................... stubbed UI-SCALE (gui/ui_scale.* is in the tree and its
    pure tests pass; no window code calls it: the scale never leaves 100%)
  * SDRplay asks for zero-IF ................................................................. carried (source/sdrplay_source.cpp) (unverified: no radio)
  * audio no longer crackles when the channel filter widens ................................... carried (dsp/) (test_bandwidth_app also exercises the filter, C3)
  * a per-frame system call removed .......................................................... carried (unverified)
  * Airspy ADS-B with decimation raised ...................................................... carried (source/)
  * beta tester usage reporting .............................................................. stubbed TESTER-USAGE (core/tester_usage.* and its pure tests
    are in; the Settings section, the hooks that note a feature, the engine's journal field are not)
  * the crash-report cleaner and a plugin's name ............................................. carried (core/) (test_crash_scrub runs)

0.99.45
  * link FoxSDR from the tester portal ....................................................... stubbed TESTER-LINK (test_tester_link_app, test_link_request_poll)
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
  * the plugin store cleans up after its own updates ......................................... stubbed STORE-VIEW (the engine keeps the scan's old copies, C4;
    test_plugin_cleanup_app, test_plugin_update_cleanup_app)
  * FUNCTION SELECT folds to a strip ......................................................... stubbed RAIL-CHROME (test_rail_fold*, test_app_rail)
  * the patch page's information pane at the top right ....................................... stubbed PATCH-VIEW (lostlines: drawPatchView, drawPatchFaces)
  * the patch canvas pans by dragging empty canvas, the wheel pans ........................... stubbed PATCH-VIEW
  * two fixes for radios reported wrongly (the Source row's words) ........................... re-expressed in part: the one-chip-one-route rule runs in
    the engine and is held by test_one_chip_app (C4); the Source row's chip/preview/"No radio hardware found" is stubbed SOURCE-ROWS (test_source_row_app)

0.99.51
  * SDRplay: no more "restart the SDR API" on a slow setting ................................. carried (source/)
  * Raspberry Pi 64-bit downloads ............................................................ n/a

0.99.52
  * Windows testers can link from the tester page again ...................................... stubbed TESTER-LINK

0.99.53
  * the Store version tells you when the Store has an update ................................. stubbed UPDATES (core/store_update.* in; the window's check and
    banner are not)

0.99.54
  * close bookmark names stack instead of hiding ............................................. stubbed MARKERS-SPECTRUM (the setting is in AppConfig and kept
    by loadedCfg_; the spectrum's drawing is not)

0.99.55
  * the SDRplay API Service stopped or stuck is named and can be restarted ................... re-expressed in part: its two recovery troubles are counted by the
    engine (C2, test_health_app); the card, the restart key and the note are stubbed SDRPLAY-SERVICE

0.99.56
  * PPM frequency correction ................................................................. re-expressed, C1 (engine_ppm.cpp; test_ppm_app, inline and stepped, break-it done)
  * RTL-SDR steadier when the computer is busy ............................................... carried (source/)
  * audio no longer turns choppy ............................................................. carried (sink/)

0.99.57
  * markers on the waterfall ................................................................. stubbed MARKERS-SPECTRUM (the marker LIST and its persistence are
    the engine's since C5: Engine::freqMarkers_, markers.json saved on a worker, tests/test_bookmark_save_async.cpp holds it; the waterfall's lines,
    the right-click menu and the Markers window are not drawn)
  * peak hold and average on the spectrum ................................................... stubbed MARKERS-SPECTRUM
  * the receiver's waterfall fits its window ................................................ stubbed MARKERS-SPECTRUM (spectrum/waterfall views are in the tree)
  * holding the left button on the waterfall no longer tunes ................................. stubbed MARKERS-SPECTRUM

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
  * Airband: type an airport, hear its control frequencies ................................... stubbed AIRBAND (src/gui_pending/app_window_airband.cpp, 1,329 lines;
    core/ and the views are in the tree)
  * LISTEN, ticked frequencies ............................................................... stubbed AIRBAND / MARKERS-SPECTRUM (the scanner's list mode is
    in core/scanner_params.hpp; the window's tick column and the engine's list scan are not)

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
    test_plugin_rescan_skip waits for PLUGIN-RUN)
  * the Fitted modules window reads no plugin file's size per frame ......................... stubbed STORE-VIEW (test_fitted_modules_no_disk red)
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
  * the Airband monitor takes a typed list and plays NFM ..................................... stubbed AIRBAND
  * presets: a named list filled by hand, from a file, exported ............................. stubbed AIRBAND
  * the spectrum shows what the monitor is hearing ........................................... stubbed AIRBAND
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
  * the RADIO SETUP page ..................................................................... stubbed RADIO-SETUP (src/gui_pending/app_window_radio_setup.cpp)
  * NFM de-emphasis in the Airband monitor ................................................... stubbed AIRBAND
  * an airport lookup while listening changes nothing played ................................ stubbed AIRBAND
  * a pasted report is recognised; `upload local-only` says what it means ..................... stubbed PROBLEM-REPORT (core/pasted_report is in; the page's
    attach-diagnostics rows, the preview and the recognised-report line are not - lostlines: drawProblemReportPage)
  * Fitted modules names the files not installed from the store ............................ stubbed STORE-VIEW
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
  * the Airband monitor says what it is hearing where you look ............................... stubbed AIRBAND

0.99.72
  * the plugin store laid out like a store; Fitted modules like an operating panel ........... stubbed STORE-VIEW

0.99.73
  * the radio and its sound stay responsive with many modules fitted ......................... re-expressed in part: the audio buffer that deepens itself
    (C1, test_audio_lead_app); a fitted plugin that runs only while it is used is stubbed PLUGIN-RUN (test_plugin_rescan_skip and
    merge_pending_part_audio_lead_plugin_wake)

0.99.74
  * GAIN and SQUELCH dials with AUTO keys on the top bar ..................................... stubbed RAIL-CHROME (test_deck_knobs_app)
  * the STATUS column folds ................................................................. stubbed RAIL-CHROME (test_status_fold*)
  * the VIEW bank is TOOLS; Display moves to SYSTEM and starts folded ........................ carried in part (the bank rename is in rail_banks.hpp, C0); the
    folded-Display memory is stubbed RAIL-CHROME
  * each decoder's settings in a folded "Plugin settings" line ............................... stubbed PLUGIN-SETTINGS-UI
  * the ADS-B map lists only the aircraft you can see; no-altitude is grey ................... stubbed MAP-WINDOWS
  * third-party plugin sites in the store ................................................... stubbed STORE-VIEW
  * BROWSE FILES beside a plugin setting that names a file ................................... stubbed PLUGIN-SETTINGS-UI
  * switching auto gain off gives the radio its manual gains back ........................... re-expressed, C1 (Engine::changeAutoGain; test_autogain_app)
  * a plugin may send transmit audio through FoxSDR, granted and armed ...................... stubbed TX-PLUGIN, in the SAFE state: Transmitter's plugin key and
    cap are merged and tested (test_tx_audio), and NOTHING in the engine calls Transmitter::setPluginKey, so no plugin can key a radio
  * POCSAG settings and messages together; automatic configuration ........................... n/a (the plugin) / PLUGIN-SETTINGS-UI
  * Language & country does not freeze while it looks for CJK fonts ........................... carried (gui/fonts, merged) (unverified)

0.99.75
  * unplaced aircraft listed apart from the placed ones ...................................... stubbed MAP-WINDOWS
  * a radio's manual gain remembered and put back at its next open .......................... re-expressed, C1 (Engine::rememberManualGain / restoreManualGain;
    test_autogain_app)
  * B200 antenna/gain advice ................................................................. n/a

0.99.76
  * the spectrum's and waterfall's range set by hand ......................................... stubbed DISPLAY-RANGE (core RangeMode is in; the menu and the
    dB window are not)
  * no more death when a window is dropped into / pulled out of a dock node .................. re-expressed, C1 (page_resize_id.hpp; test_page_resize_id)
  * a Linux scan helper with no reader is not a crash ........................................ carried (source/)

0.99.77
  * the Fitted modules / DECODE lists grouped under the store's headings ..................... stubbed STORE-VIEW
  * a row in a plugin's panel is a key ....................................................... stubbed PLUGIN-ROW-KEYS (test_programme_audio_app)
  * an old copy of a renamed plugin no longer loads beside the new one; ImGui's duplicate-ID box is off in the shipped build
    .......................................................................................... carried (core/plugin_host, gui/imgui_debug_policy.hpp) and held in part by
    test_plugin_host_origin (C4)
  * Linux builds of 41 of 43 plugins ......................................................... n/a

0.99.78
  * an aircraft at the edge of range stays on the map (estimated positions) .................. stubbed MAP-WINDOWS
  * a start-up death at the window says how it ended and which window system it used ......... re-expressed, C1 (window: lines, the failure message,
    FOXSDR_WINDOW_PLATFORM, the breadcrumb flag; test_window_failed_app)

0.99.79
  * a message from the FoxSDR team is shown whether or not there is an update ................ stubbed UPDATES

0.99.80
  * bookmark Import: drop zone and Browse button; by-name import; translated errors;
    Unicode filenames on Windows; Browse opens in the recordings folder ....................... stubbed IMPORT-DIALOG (src/gui_pending/app_window_import_dialog.cpp)

0.99.81
  * IBOC dashboard programme rows respond to clicks ......................................... stubbed PLUGIN-ROW-KEYS
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
  * Bookmark Import accepts the path shapes people use ....................................... stubbed IMPORT-DIALOG
  * slide the tuned band with Shift + wheel .................................................. stubbed MARKERS-SPECTRUM (gui/passband_slide.hpp is in the tree)

0.99.83
  * the audio buffer comes back down by itself and says where it stands ...................... re-expressed, C1 (test_audio_lead_app)
  * the mode follows a typed band change ..................................................... stubbed MARKERS-SPECTRUM (lostlines: drawFrequencyReadout)
  * the Radiofax recorder prints the chart on its paper .................................... carried (gui/instrument_fax.cpp)

0.99.84
  * a freeze inside a plugin reload names the reload's step ................................. re-expressed, C4 (the phases are published by the engine's rescan and
    by every caller of detachAndUnloadPlugins; core/hang_watchdog reports `reload-phase:`)

### 6.2 Totals

(Computed from section 6.1 at the end of the pass: see "Status at the end of this pass".)

### 6.3 The tests that are red, and why

(See "Status at the end of this pass".)
