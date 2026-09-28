# Engine pre-3b round 3 - review findings to fix

Commits reviewed: 1497f79 and 41e3853 (on top of 2e3bbe0). Verdict: REJECT.
The rest of the round (sound card, telemetry, mute, Airspy, catalogue URL,
patch "Look for radios", prune) matched 2e3bbe0 in review. All findings are
in the scanner draft path, the Transmit box and test coverage.

`test_zz_review_scanner_draft.cpp` in this folder is the reviewer's
throwaway harness. It feeds real mouse and keyboard events through ImGui's
input queue into `AppWindow::drawScannerSection` and reads only the
Engine's scan fields. It passed 16/16 against 2e3bbe0 and failed 7/16
against 41e3853. It is NOT built by CMake from here; turn its scenarios into
committed tests.

## 1. HIGH - a desk edit to any scanner field reverts a range the web remote set

- The seven scanner drafts (app_window.cpp ~17449-17474) are seeded only in
  `AppWindow::applyConfig` (~18748-18754) and never re-seeded when the
  engine's fields change. The web remote changes them through
  `FOXAPP_OP_SCANNER_RANGE` / `SCANNER_RUN` (src/net/control_ops.cpp ~124,
  ~151). Any later desk edit commits all seven drafts at once.
- Scenario: the remote sets 144-146 MHz with a 12.5 kHz step, then the user
  changes only Dwell to 75 at the desk. Engine afterwards: 88 / 108 / 100
  (41e3853) against 144 / 146 / 12.5 (2e3bbe0).
- Until that edit, the panel shows 88-108 while the engine holds 144-146,
  and "Start scan" reads `engine_.scannerParams()` (~17478), so the scan
  runs a range the panel is not showing.
- Fix: re-seed each draft from the engine whenever that field is not the
  active item, or whenever the engine value changes. Not only in
  applyConfig.

## 2. MEDIUM - config load failure leaves the drafts at zero, and one edit writes zeros

- app_window.hpp ~1293-1299 initialises the drafts to 0.0. `applyConfig`
  runs only when `ConfigStore::load` succeeds (app_window.cpp ~735-747).
- With a corrupt config, or a hermetic run with an empty path, the panel
  shows 0.0000 in all seven fields. Editing Dwell alone leaves the engine
  with start/stop/step/hold all 0 (base: 88 / 108 / 100 / 2000).
- Fix: default the drafts to `Scanner::Params{}`; the re-seed in finding 1
  also covers this.

## 3. MEDIUM-LOW - an edit is lost if the section stops drawing before the field deactivates

- `IsItemDeactivatedAfterEdit` is only seen if the widget is drawn again.
- Scenario: type Start 150, then switch page or collapse the section. The
  engine keeps 88; when the section comes back the box shows 150, but Start
  scans from 88. Base: the engine had 150 and the scan used 150.
- Typing and pressing Start straight away works.
- Fix: commit any pending draft edit when the section is not drawn, or
  commit on every edit (not just on deactivate). Keep the "no half-applied
  settings for a running scan" property.

## 4. LOW - Transmit can open a different board from the one its box shows

- The `TX_OPEN` handler (engine.cpp ~5708) ignores empty text
  (`if (!text.empty())`) and keeps the previous `transmitArgs_`, so
  clearing the box and pressing Open opens the previous board
  (app_window.cpp ~15093).
- Fix: an empty box must not silently open the previous board. Refuse with
  a clear status, or open with empty args, whichever the pre-round
  behaviour implied.
- Also note (decide and document): a Pluto or Transmit address typed but
  not opened is no longer kept in config.json across a restart; it is now
  saved only on Open. Before this round it was saved as typed. Restore that
  unless there is a reason not to.

## 5. LOW-MEDIUM - untested new paths

The reviewer's mutants each got a full rebuild and a whole-suite run:

| Mutant | Result |
|---|---|
| panel never sends the scanner timing command | survived |
| `submitSoundCardForm` drops the Swap I/Q bit | survived |
| Airspy decimation choices never published | survived |
| `prunePatchSinkLines` prunes nothing | survived |
| telemetry OFF keeps the install id | caught |
| "Keep it running" leaves the popup subject open | caught |

Every new test calls `Engine::applyCommand` with hand-built commands. None
drives a window control or the deactivate commit. Add tests that fail for
each surviving mutant:

- the harness scenarios, as committed tests;
- an encode-then-decode round trip of `submitSoundCardForm`;
- a read-back of the published Airspy decimation choices;
- a check that `prunePatchSinkLines` actually prunes.

## 6. LOW - stale comments

- app_window.cpp ~18744 names `FOXAPP_OP_SET_PLUTO_URI`, which does not
  exist.
- app_window.hpp ~1290 and tests/test_command_path_guard.cpp ~271 name
  `FOXAPP_OP_SCANNER_TIMING`, which was removed.
- app_commands.hpp ~53 cites `cmd::encodeSoundCardForm`, which does not
  exist.

## 7. Telemetry op - gate it now (privacy)

- Today nothing outside the desktop can send `FOXAPI_OP_TELEMETRY_ENABLE`.
- It is now marked implemented with no permission check. Once an API
  session forwards FOXAPI ops, a client could switch reporting on silently.
- The handler also mints a NEW install id on every "on", even when
  reporting is already on. The old checkbox used `on && !enabled`.
- Fix: accept this op only from the desktop's own control path. Refuse it
  from any API/plugin/remote origin, with a test. Make "on while already
  on" a no-op that keeps the id, with a test.

## Environment notes

- On Linux under WSLg, `test_viewports` fails on EGL/ZINK "failed to
  choose pdev". That is this machine's environment, not the code; a cloud
  VM may differ.
- On Windows, `test_bias_key_run` flaked once (10 of 11 solo runs passed).
  It is suspected harness flakiness, unproven.

## Resolution (branch claude/engine-round3-fix)

1. Fixed. Each scanner draft re-seeds from its engine field whenever that
   field is not active and has no pending edit. The desk commits only the
   fields it edited.
2. Fixed. The drafts default to `Scanner::Params{}`, and the re-seed covers
   a config that never loaded.
3. Fixed. With the scanner idle, an edit is committed as typed. With a scan
   running, it is committed on deactivate, or by `flushScannerDraft()` from
   `drawUi` once the field is no longer active. A running scan never takes a
   half-typed value.
4. Fixed. `TX_OPEN` refuses an empty or blank address, and the page greys
   Open with a reason. Both address boxes commit as typed again
   (`FOXAPP_OP_SET_PLUTO_URI` / `FOXAPP_OP_SET_TRANSMIT_ARGS`), so a typed
   but unopened address is kept in config.json as it was before this round.
5. Covered by `tests/test_scanner_draft_input.cpp` (this harness's
   scenarios plus three for a running scan), a sound-card encode/decode
   round trip, an Airspy decimation-choices read-back and a
   `prunePatchSinkLines` check. Each surviving mutant in the table above now
   fails at least one test. The harness in this folder passes 16/16
   unmodified.
6. Fixed. All four comments were corrected.
7. Fixed. `FOXAPI_OP_TELEMETRY_ENABLE` is refused (`FOXAPI_DENIED`) from
   every origin. The desktop uses `FOXAPP_OP_TELEMETRY_CONSENT`, which no
   transport carries. "On while on" returns `FOXAPI_NO_CHANGE` and keeps the
   id. Both behaviours are tested.

Linux (Ubuntu 24.04 container, Xvfb): 222 of 225 ctest entries pass. The
three failures (`test_apply_command`'s audio-device check, `test_audio_out`
and `test_report_reader`) fail identically on the unmodified e6d6d00 in the
same container, which has no sound card. Not run on Windows.
