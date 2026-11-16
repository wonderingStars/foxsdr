# Second merge: screen lane S3, lane X, the S5b/S5c radios follow-on (branch integ/second-merge)

Cut from `engine/multi-radio` 66072154 (merge 7's merged tip). Three `git merge --no-ff`, in this order, tips checked before each: `feat/screen-s3` f613c8f1, `feat/lane-x` 13839a08, `feat/screen-s5c` 1f53e073 (contains S5b c3efc203; S5 was already in the base). Never pushed. Worktree D:/foxsdr-wt/en-screen-j, build folder D:/foxsdr-build/en-screen-j.

## Commits
| Step | Commit |
|---|---|
| Merge S3 | baf87c0f |
| Merge lane X | ce47595e |
| Merge S5c (S5b, S5c) | d3957aa8 |
| MERGE NOTES applied, re-seed, report tests | the commit after d3957aa8 (this record's own commit) |

## Conflicts and how each was settled (both sides kept)
- `resources/lang/*.json` (all three merges): union of keys through a script (a key both sides changed keeps the base-side text; 3 keys per language differed: "a second", "16-bit integer", "Bandwidth (kHz)"), then `tools/i18n_keys.py --merge <lang>` dropped keys the old branch point carried that merge 7 had already removed (4 after S3: "About", "Cross-origin sign-in stays off: it needs HTTPS...", "No record of the last start...", "that is not a number (use a dot for decimals)"), then `python tools/embed-lang.py`. `tools/i18n_keys.py`: 0 missing, 0 stale, exit 0, after each merge. `lang_assets.hpp` is regenerated, never merged.
- `src/slint/main.cpp`: S3 (9 hunks): git interleaved S3's publisher block with S1's/S5's; S3's block is kept as its own block after S5's. Lane X (5 hunks): same, lane X's block kept whole after S3's with a closing brace supplied; tick, answer, `s3.`/`lx.` state hooks kept beside the S1 to S5 ones. S5c: none.
- `src/slint/ui/main.slint`: LaneS3 import/export and five mounts; KeyFocus, LaneX exports and the help sheet.
- `src/slint/CMakeLists.txt`, `src/slint/tests/CMakeLists.txt`, `tests/CMakeLists.txt`: lanes3, lanex, knob, tester families added as whole blocks; S5c's `s5_host OR s5b_host` and the two S5b sources.
- `src/engine/engine.cpp`, `engine_api_lists.cpp`, `wire_c.cpp`, `wire_c.hpp`: both sides' additions (S3's rec.* rows beside E1/S4's rows; S3's recplay facts beside S1's passband facts; S5b's CatLink members).
- `tests/errpath_allow.tsv`: union, then re-seeded with `tools/errpath_seed.py --write-pin` (see the pin below).

## SHARED FILE EDITS: every lane's list, grepped after the merges (all survive)
- Mounts and exports in `main.slint`: LaneS3/S3Overview 7 lines, LaneX/KeyFocus/XHelpSheet 8 lines, LaneS1/S2/S4/S5 14 lines.
- `main.cpp`: laneS3p 7, lastPlayState 3, laneXp 9, knobHost 3, helpLastRow 3, `s3.` 3, `lx.` 2, `pane.` 3, testerDuty 4, laneS1p 7, laneS2p 8, s4p 7, s5p 7; menu item 610 (Help topics) and its `case 610`.
- Lane X's `signal_logic.cpp` (PF-1, +26 lines), `key_dispatch.cpp` (Ctrl+F1), `host_logic.cpp` (`fmtNumber`), `radio_pane.slint` (P1) are in the merged tree (diffed against the base).
- S3's engine edits: `rec.roll_mb` and `rec.replay_s` rows in `wire_c.cpp`, `recplayRadioFacts` in `engine_api_lists.cpp`, `raw_file_id.hpp` include in `engine.cpp`. E1/S4's rows (`rds.log`, `out.mqtt.enabled`) are still there. S5b's `catLink` members are in `wire_c.hpp`.

## The dangling-reference search (handlers capturing a local that is gone)
The new blocks (S3's publisher, lane X's publisher and knob host, S5's) capture by reference only names declared at the top of `main()` (`stepTune`, `tuneStepHz`, `setVolume`, `setSquelch`, `zoomView`, `setMode`, `selectRadio`, `workspace`, `duties`, `openSections`, `bankIndex`, `lastPlayState`, `helpLastRow`), each checked by grep to be a 4-space-indent declaration in main's own scope. The one block-local (`ke`, the knob host's Env) is moved into its host, not captured. None found; nothing changed. (A reading, not a run on a second platform.)

## MERGE NOTES applied (the radio-reports calls; `src/core/radio_events.hpp`)
S5b (the refusal sites, each wrapped once with `noteControlRefusedText`, which hands the sentence back to `setError`):
- `RtlSdrSource::setIfBandwidthHz`: the range refusal (`Bandwidth`, userRange) and the tuner's refusal (`Bandwidth`). The busy, no-radio and faulted sentences stay unwrapped, as the sibling `setSampleRateHz`/`setCenterFrequencyHz` leave theirs.
- `SoapySource::setBandwidthHz`: not positive (userRange), the driver threw, no bandwidth setting, outside the driver's range (userRange): `Bandwidth`. `SoapySource::writeSetting`: a throw, a refusal (`Other`), an unknown key (`Other`, userRange). `bandwidthInfo` and `listSettings` throws: `noteVendorCallFault(this, "soapy", "vendorcall", ...)` (absorbed).
- `SdrPlaySource::setLowIf`: needs no call of its own: it goes through `setSampleRateHz`, which already reports. The first-stream low-IF fallback stays NOT an issue report (an open row, as the S5b record says).
S5c (`src/source/fobos_source.cpp`): the Impl now knows its owner (the FobosSource the engine registers; `engine_patch_radios.cpp:358` registers every source generically, so no `registerSource` call is needed in the driver). `markDead` raises `noteDeviceFault`; `Impl::call` raises `noteVendorCallFault("vendorcall", gave-up)` for TimedOut and Faulted; the guarded stream call's fault is reported at both places it is read (start, read); `setSampleRateHz`, `setCenterFrequencyHz`, `setGainDb` refusals are `noteControlRefusedText` (`Rate`, `Frequency`, `Gain`); the silence watchdog and the bad-callback count raise `noteStreamTrouble`.
Tests that see each report (new shared helper `tests/radio_report_rig.hpp`: a stand-in server and a temporary queue folder):
- `test_fobos_source`: three blocks (refused rate, tune, gain -> three `radio_settings`; a crash on a control call -> one `radio_recovery` gave-up plus one `radio_close`; impossible-length callbacks -> `radio_stream` with `usbErrors`). 444 checks, 0 failed. BREAK-IT: the rate wrapper removed -> RED (`msgs.size() == 3` failed, 439 checks, 1 failed); restored -> 444/0.
- `test_rtlsdr_ifbw`: out-of-range bandwidth -> `out of range: bandwidth`, the value not in the text; a tuner that stopped answering -> `refused: bandwidth`. 50 checks, 0 failed.
- `test_soapy_settings`: out-of-range bandwidth, a driver that refuses a setting, an unknown key. 143 checks, 0 failed.
Lessons, for the next test writer: the reporter folds a repeat of one control on one radio for 5 s (`admitCall`, steady clock) and applies a daily first-and-last bound, so a test uses a second radio identity for a second case of the same control.
NOT reported (still open): the S5b/S5c sites not in the notes; the open-failure paths (those are `radio_fail`, the engine's).

## Pinned numbers (both values)
| Pin | Before | After | Arithmetic |
|---|---|---|---|
| `kPendingPin` (PENDING lines in `tests/errpath_allow.tsv`) | 459 (base) | 459 | none of the three lanes added a PENDING line (their new sites are ALLOW with a reason); the re-seed after the MERGE NOTES dropped the allow lines of sites that now report (75 lines out, 58 in, as keys moved) and the PENDING group counts are unchanged. 459 + 0 = 459. |
By group (the re-seed's listing, top of the PENDING groups): S1-startup 3, S4-first-run-check 2, G1-device-dead 2, PL2-companion 1, G7-patch-radio 1, then the rest of the base's groups unchanged.
Not re-measured: `test_engine_publish_alloc` bound (passes at the merged bound, 25 s; E1/S4's 2200 is the base's; no lane here raised it). The section-overlap drive scripts were not run (GUI; see NOT RUN), so the 2058 figure is untouched.

## Expectations changed
None in existing tests. New assertions only (the three report blocks above). S5c's own two changes (needed-library rows 9 to 10; source kinds 12 to 13, `openRadioId(12,"x")` -> 13) arrived with the lane and pass on the merged tree.

## Tests run (all Release, `ctest -j2`, foreground)
S3: `test_iq_file_s3`, `test_overview_s3`, `test_record_rollover_s3`, `test_slint_lanes3_host`. Lane X: `test_slint_knob_host`, `test_slint_lanex_host`, `test_slint_tester_host`, `test_input_event`, `test_input_hid_parse`, `test_input_mapping`, `test_input_midi`, `test_input_win`. S5b/S5c: `test_slint_s5_host`, `test_slint_s5b_host`, `test_engine_api_cat_link`, `test_engine_api_s5b`, `test_fobos_source`, `test_rtlsdr_ifbw`, `test_soapy_settings`, `test_slint_lanel_host`, `test_wire_b_saved_kinds`, `test_engine_api_wireb_neededlibs`. Named suites: `test_error_paths`, `errpath_gate_selftest`, `test_i18n` (Korean word-spacing check included), `test_radio_events`, `test_engine_publish_alloc`. All passed. Reds: `test_sdrplay_lowif` failed once under load in the first batch and passed 72/0 when re-run alone (twice): load flake, listed in the standard rules. Two of my own new report tests were red at first (a throttle I had not known about, above) and were fixed in the tests, not the code.
Builds: `foxsdr-slint` (the merged main.cpp and main.slint) built, rc 0.

## NOT RUN
The full headless and GUI gates (the brief forbids them tonight), every drive script (S3, lane X, S5b), `test_web_touch` (a browser test: built, not run), the stamp tests, `test_radio_reports_engine` (not built), the section-overlap scripts, the full-tree build (not requested; one `--target` build for the lanes' tests).

## Config hash
`%APPDATA%/foxsdr/config.json` sha256 c21ce98dc30e50327f7666f7ee29047c0983d130ca3f21716d0c0e20e04603c3 (unchanged; hashed, never opened for writing).

## UNVERIFIED
Everything on real hardware (Fobos, RTL-SDR filter, Soapy modules). The merged window on screen (no drive script run). Whether the re-seeded allow list is minimal (the tool keeps lines whose site exists). The translations (machine-made, three lanes' keys).
