# AGC choice and noise blanker: the wiring (multi-radio lane C, 2026-11-10/11)

Branch `mr/agc-nb-wiring` (cut from `mr/screen-s1-s2` merge f2213607), worktree `C:\Users\steve\foxsdr-wt\mr-screen1`, build folder `D:/foxsdr-build/mr-screen1`.
Design: `docs/agc-noise-blanker-design.md` of the engine-stage4 worktree (sections 2.2, 3.2, 4, 5 lane C); the blocks are lanes A and B (`docs/multi-radio-records/agc-nb-blocks.md`).
Nothing here is pushed or merged. Nobody has listened to either feature and no real radio was used: every figure below is from generated signals, the fake radio and the
signal generator.

## Commits

| Commit | Subject |
|---|---|
| 880a5f3a | Engine: AGC choice and noise blanker through the API, the state and the saved settings |
| c032232e | Mock engine and API.md: the audio AGC choice and the noise blanker |
| 647d8847 | Slint window: the AGC selector and the noise blanker in the SIGNAL bank, the "GAIN AUTO" note, 14 catalogue strings |
| (this file) | docs: record |

## What was built

1. **Ops** `FOXAPI_OP_SET_AGC` 0x0111 (`ival[0]` mode 0..4 = Auto, Fast, Medium, Slow, Off, `FOXAPI_AGC_*`; `ival[1]` = 1: `num[0]` is the fixed gain, dB) and
   `FOXAPI_OP_SET_NB` 0x0112 (`ival[0]` on/off; `ival[1]` = 1: `num[0]` is the threshold, 6..30 dB). Names in `app_commands.cpp`; the API's range checks in
   `fox_engine_api.cpp validate` (mode 0..4; fixed gain -20..+40 and threshold 6..30 only when carried); `Engine::applyCommand` refuses a mode past Off and clamps (and flags
   CLAMPED) a number outside the range. The AGC handler sets the mode FIRST and the fixed gain after it.
2. **Receiver-scoped.** Both are in `radio_scope.hpp` (47 -> 49 ops), so `FoxCommand.target` addresses them; the handlers take the slot with
   `receivers_[pickRadio(...).radio - 1]` and keep their state in the `Receiver` struct (`agcMode_`, `agcFixedGainDb_`, `nbEnabled_`, `nbThresholdDb_`). No Engine-level
   alias reference was added.
3. **Fixed gain range -20..+40 dB** (owner decision) in `Agc::kFixedGainMinDb`, `setFixedGainDb` (a NaN lands on 0 dB, not on the new negative floor), `Agc::setMode` (entering
   Off clamps the held gain into the range and moves gain and fixed gain together, so a held gain below -20 dB is frozen AT -20 with no second step), the API and engine range
   checks, the slider (-20..+40 over 0..1, whole dB), the config clamp, the mock and API.md.
4. **State.** `FoxReceiverState` appended after `rxLonDeg`: `agcMode`, `agcFixedGainDb`, `agcGainDb`, `nbEnabled`, `nbThresholdDb`, `nbBlankedPct`, and a SEVENTH, `nbStandingDown`
   (see deviations). The four settings move `modeSeq` and `seq`; the three measurements move nothing (`receiver_snapshot.cpp`). `FOXAPI_MIN_RECEIVER_STATE` still ends at
   `rxLonDeg`. In Off the published and saved fixed gain is the pipeline's own (the gain frozen at the switch); otherwise the value last set or loaded
   (`agcFixedGainShown`, engine.cpp).
5. **Saved settings** `agcMode`, `agcFixedGainDb`, `nbEnabled`, `nbThresholdDb` beside `nrEnabled` (Engine half: `AppConfig`, `config_halves.hpp`, the reader with clamps [0,4] /
   [-20,40] / [6,30], the writer, `applyConfig`, `fillConfig`, the start-up push - mode before fixed gain in both pushes). Defaults Auto, 0 dB, off, 15 dB; a file without the
   keys loads as before. No telemetry or usage-report field.
6. **Mock engine and API.md** (6.19, the command table, 6.4) and `docs/api-0.3-additions.md` 9.20.
7. **Slint** (Audio filters section of the SIGNAL bank, below the Auto notch row so the older rows keep their places): AGC dropdown + gain in use, Fixed gain slider, Noise
   blanker tick, Threshold slider, "Blanking n %" / amber "Standing down: signal too strong". Files: `signal_bank.slint`, `signal_logic.*`, `signal_publish.cpp`, `main.cpp`
   (drive states `agcMode`, `agcFixedGainDb`, `nbEnabled`, `nbThresholdDb`, plus `agcGainDb`, `nbBlankedPct`), the specimen, `COMPONENTS.md`. No classic-window or web control.
8. **"GAIN AUTO - AGC ON" -> "GAIN AUTO"** at `host_logic.cpp` (the Slint status card). Grep of every user: the classic window (`src/gui/app_window.cpp:4619`) shows the old text, so the
   old catalogue key was NOT removed from the 33 files; no test, drive script, web page or other catalogue use. The classic window still says "AGC ON" for the hardware's
   automatic gain; making it match is a one-line change in `app_window.cpp` that this lane was not asked to make.
9. **Strings.** 14 of the 77 prepared strings were added to each of the 33 catalogues exactly as prepared (462 entries): AGC, Fast, Slow, Fixed gain, "AGC gain now: %d dB",
   Noise blanker, Threshold, "Blanking %d %%", "Standing down: signal too strong", GAIN AUTO and the four tooltips (AGC selector, Fixed gain, Noise blanker, Threshold).
   Auto, Medium and Off were existing keys. `test_i18n` fails on a catalogue key no source uses ("has a key the source no longer uses"); with all 77 added it listed exactly the
   63 multi-radio-screen strings, so they were taken out again and wait for the lanes that use them (`D:/foxsdr-build/prep/new-strings-34-languages.json`, still complete). The
   two strings with a number are worded in C++ (`tr("Blanking %d %%")`, `tr("AGC gain now: %d dB")`): a Slint `@tr` key is looked up with `{}` and the catalogue form is `%d`.
   `src/core/lang_assets.hpp` was regenerated with `tools/embed-lang.py` (a generated file: on merge, regenerate it rather than resolve it by hand). No string English-only.

## Decisions

* The web profile was NOT extended with the two ops (the brief said "the remote profile where noise reduction has them"): `test_engine_api_05a` P1 requires the web profile to be exactly the
  closure of what `controlRequestToCommands` can send, and the web page gets no control in this work, so adding them would either break that test or need an op the page cannot send.
  A REMOTE session with the SETTINGS grant may still send them (they are not application ops). Adding them later is one line in `kWebProfile`, one in `controlRequestToCommands` and
  the page.
* `nbStandingDown` is a seventh state field, appended after the six. The brief asked the readout to say when the blanker stands down; the six fields cannot carry it, and a
  sentinel in `nbBlankedPct` (-1) would hide a state in a measurement. No flag bit is spent. The other lane was told six fields after `rxLonDeg`; the seventh is after them, so
  their six keep their offsets.
* The state fill reads `receivers_[0]` (the only live receiver until stage 4); the lane merging stage 4 changes that index where it builds the per-radio states.
* The Fixed gain slider sends the mode in force with `ival[1] = 1` (not a gain alone), because the engine's handler sets the mode first; the slider is live only in Off, so that
  mode is Off.
* The Fixed gain and Threshold sliders are whole dB; the engine keeps the number it is given, the API accepts any number in range.
* The filters chip ("n ON") does not count the blanker, and the AGC is not a filter: unchanged.
* In RAW demodulation the AGC is not forced off (lane A's recommendation); the choice is the user's.

## Proofs (see the report for the gate numbers)

* New: `test_engine_api_agc_nb` (real engine through the C table: both ops land in the state and move `modeSeq`; mode first then gain, Off with -12 dB keeps -12 dB, both ends of every
  range; every range refusal at the submit changes nothing, a number not carried is not range-checked; target 1 and 0 land, 2, one past the limit and 0xFFFFFFFE are NO_DEVICE,
  ALL is BAD_ARGUMENT; the local `applyCommand` path obeys the same rule; with the radio running the live readouts never move `modeSeq`).
* Extended: `test_apply_command`, `test_radio_scope` (49), `test_receiver_snapshot` (seven steps, the layout), `test_config` (round trip, junk clamps, old file), `test_engine_main_config_roundtrip`
  (Off at -7.5 dB with the blanker at 22.5 dB survive two engine runs), `test_agc_modes`, `test_pipeline_agc`, the fuzz seed `seed-defaults.json`, the mock's `test_mock_engine`
  (25 checks), `test_slint_host_signal`, `test_slint_host_logic`, `test_slint_signal` (pixels and four pointer events), drive scripts `signal-agc` and `signal-noise-blanker`, ledger
  rows 4400-4405 (2148 rows, 0 unclassified).
* The golden snapshot (`tests/golden/state_snapshot.golden`) did NOT change and was not regenerated: it renders the web status, CAT replies and the plugin ABI state, none of which
  carries the new fields (the test passed untouched; the new ops are not in its drive list).

## Gates (Release, MSVC, D:/foxsdr-build/mr-screen1)

* `cmake --build ... --parallel`: exit 0 after each commit.
* `ctest -L "headless|slint" -j4 --timeout 300`: 747 tests, 741 passed, 6 failed: the five source-locating tests (`test_airband_data`, `test_band_plan`, `test_converter_call_sites`,
  `test_page_resize_id`, `test_png_decode`: the build folder is outside the worktree; each passes, rc 0, run with the worktree as working directory) and `test_engine_api_transmit`
  (`issued.size() > 1000` at line 1500: 936 new tickets against 1000, a throughput figure; it failed alone twice, at 104 s and 118 s, while another lane's window gate was
  running on the machine, and the pre-change binary of the stage 1-3 build folder (`D:/foxsdr-build/mr-engine1`, which has none of this code) fails the same line with 899
  tickets: a load-sensitive test, not this lane's).
* `ctest -L gui` on the hidden desktop (own desktop `FoxSDRTestsC`, `FOXSDR_FRAME_CAP_HZ=60`, `-j3` after a first serial run was stopped at 41 tests because it was taking 100 minutes
  beside another lane's gate): 227 tests, 226 passed; `test_diagnostics` failed on the stale commit stamp and passed (with `test_diagnostics_free`) after the rebuild.
* After the last rebuild, alone: the 14 headless tests of the lane, `slint_drive_signal-agc`, `slint_drive_signal-noise-blanker`, `slint_drive_signal-audio-filters`: all passed.

## Break-it checks (each behaviour removed, RED seen, restored, rebuilt, GREEN seen)

Engine and core (one build, all six removed at once; every test that guards one went red): the AGC handler setting the gain before the mode; the two ops out of the receiver-scoped list;
the four settings out of the mode group in `receiver_snapshot.cpp`; the blanker threshold not written to the file; `kFixedGainMinDb` back at 0; the threshold range check removed from
`validate`. RED: `test_engine_api_agc_nb`, `test_receiver_snapshot`, `test_engine_main_config_roundtrip`, `test_apply_command`, `test_agc_modes`, `test_radio_scope`, `test_config`,
`test_pipeline_agc` (8 of the 9 tests run; `test_engine_api_radios`, which does not use the new behaviour, stayed green).
Slint (one build, five removed at once): the blanker tick sending nothing; the Fixed gain op not saying it carries a gain; the old status note; the AGC dropdown not forwarding its pick; the Fixed
gain slider always live in the model. RED: `test_slint_host_signal`, `test_slint_host_logic`, `test_slint_signal`, `test_i18n` (the old key is then the one the source uses),
`slint_drive_signal-agc`, `slint_drive_signal-noise-blanker`. The mock: the mock's own checks were run once, green (418 passed; the two version-gate failures are not this change's).
The break-its were done in the combination above rather than one build per behaviour; the red test per behaviour is the one named in its test's header, not separately isolated.

## Mock engine

Compiled as in stage 3 against the three stand-in headers (copied to `D:/foxsdr-build/mr-screen1/mock/shim`, not committed): `test_mock_engine` 418 checks passed, 2 failed - the same two
version-gate checks that fail on the 0.3 header before this change (`engineApi(0, 2)` expected non-null, `engineApi(0, 3)` expected null).

## Unverified

* Nobody has listened to the AGC modes or the blanker through this wiring, and no real radio or real impulse noise was involved; the blanker was proven on generated clicks only (lane B).
* The Slint rows were measured by pixels on the specimen and driven on the hidden desktop; they were not looked at on a visible window.
* Window layout at a narrow width: the Fixed gain / Threshold labels are 56 px wide and elide in a language whose word is longer; the tooltip carries the full text. Not measured in the
  33 languages.
* The mock's `agcGainDb` / `nbBlankedPct` are stand-ins (no loop, no blanker).
