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
(See the next sections: filled as the passes land.)
