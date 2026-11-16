# Screen lane S5 (wave 8): radios and sources a user could not reach or could not set (2026-11-16)

Branch `feat/screen-s5`, cut from `d4f855c5` in `D:/foxsdr-wt/en-rfmon`; build folder `C:/Users/steve/foxsdr-build/s5` (Slint ON, Release, clip probe OFF); scratch `D:/foxsdr-build/s5-scratch/`; hidden desktop `FoxSDRS5`. Nothing pushed. (This file is named `screen-s5-sources.md` because `screen-s5.md` already holds the older multi-radio stage S5, Link tuning.)
The real `%APPDATA%/foxsdr/config.json` has sha256 `c21ce98dc30e50327f7666f7ee29047c0983d130ca3f21716d0c0e20e04603c3` (checked at the end, see Gates).
Every proof below is a stand-in proof (made-up facts, the test generator radio, a loopback TCP sender): **real radio UNVERIFIED** for every row.

## Rows

| Row | Verdict | Proof / reason |
|---|---|---|
| G1-05 raw I/Q over UDP and TCP | BUILT | Source section, "More source controls", "Raw I/Q stream". `test_slint_s5_host` (address text, every bad field refused with the field named, the exact command sent); `test_engine_api_s5` A: the text the fields make opens a loopback TCP sender as radio 1's source with the typed rate and centre, the 25 kHz tone lands where it belongs, and the same stream read as a different typed rate lands at 50 kHz (the typed rate is what the engine used); a UDP listener opens on 127.0.0.1; a hostile stream (lying header, 4 KiB of random bytes) leaves the radio on the generator with a sentence and the engine serving; an unresolvable host is refused with the engine's words and the fields kept. Network input is validated by the engine's own parser (`raw_iq_net_source`, with its existing fuzz and hostile tests); this lane adds none to it. |
| G1-11 RTL-SDR direct sampling | BUILT | Four-way choice (automatic, never, I branch, Q branch) over `SET_DEVICE_OPTION direct_sampling`; the current value is the new fact `radio.N.directSampling` (engine_api_lists.cpp, present only while the radio has the setting, which fixes a comment that named a fact nobody published). A radio without the setting shows no control and says why (`test_engine_api_s5` D, the host test). The engine and wire side was already proved by `test_rtlsdr_direct` (R11). Engine-level readback of the new fact on a real native RTL driver: UNVERIFIED (no stand-in can reach `withDirectSampling`'s native cast). |
| G1-17 automatic ppm | BUILT | Measure / Cancel / Use this correction over `PPM_MEASURE` and `SET_PPM`; states and progress read from `fact.radio.N.ppmauto.*`; the reference is stated in words (the station's 19 kHz stereo pilot, taken as exact); the measurement is only a suggestion and replacing a correction the person has switched on with a different number needs a second press (never fights a typed ppm). Proved: host test (command recorded, armed key, disarm by another action, same number needs no second press), engine test C (the engine's WFM-only refusal shown in its words, state "measuring" read back, Cancel, nothing applied). The "done" path (a real measurement result) is UNVERIFIED: the generator carries no pilot. |
| G1-18 tuner pooling | BUILT | Frequency (MHz), mode, bandwidth (kHz) over `CHANNEL_CLAIM`; the answer says which radio and how; a Release key per claimed radio from `fact.radio.N.claimed`. `test_engine_api_s5` B: the claimed radio is at the typed frequency and bandwidth, a second claim takes the other radio, a third gets the engine's refusal, Release clears the fact. |
| G1-13 SDRplay Low-IF | NOT BUILT | `sdrplay_if_mode.hpp` has no caller and low-IF was retired in 0.99.44 after two RSP2 logs showed the ADC rate arriving instead of the planned rate. It needs: an IF-mode setting in `SdrPlaySource::setSampleRateHz`, the rate menu from `sdrPlaySupportedRatesForMode`, and the first-stream rate check (`sdrPlayLowIfRateVerdict`) that falls back to zero-IF on a mismatch, proved on `tests/sdrplay_fake_api.hpp`. Not started: touches the live SDRplay streaming path. Kind of lane: engine (sources), then a control. |
| G1-09 RSPduo dual tuner | NOT BUILT | `sdrplay_dual_tuner.*` is stand-alone; needs two streams per radio and the multi-radio engine's second-radio plumbing. Kind: engine. |
| G1-14 analogue IF bandwidth | NOT BUILT | The native RTL driver sets the tuner bandwidth from the rate (`rtlsdr_source.cpp` open and `setSampleRateHz`); a user value needs a source setter, a device option plus fact, and a wire test on `FakeUsbDevice`, then a field. Kind: engine, then control. |
| G1-40 AM co-channel canceller | NOT BUILT | No method is known (`MISSED-ROWS`: nine code-host queries found none); nothing was searched again here. Kind: research lane. |
| G1-15a/b SoapySDR bandwidth and driver settings | NOT BUILT | `SoapySource` has no `setBandwidth`, `listSettings` or `writeSetting`; needs those under its device lock, an engine op, facts and a stand-in module. Kind: engine, then control. |
| G1-03a SDR++ server client | NOT BUILT | Needs the GPL-helper external-decoder kind (owner decision of 2026-11-12) or a clean-room protocol; the existing rtl_tcp control already reaches SDR++ Server's rtl_tcp-compatible mode. |
| G1-16 CAT panadapter, G3-18a OmniRig, G3-18b CAT over a virtual COM port | NOT BUILT | `rig_cat.hpp` and `omnirig_client.hpp` are stand-alone: need a follower thread over `serial_port_win` (and a COM late-binding bridge for OmniRig that says what to install when it is absent), a `SET_FREQUENCY`/`SET_MODE` sender, facts and a stand-in serial interface. Receive-side following only. Kind: engine/host, then control. |
| G1-07b Perseus, G1-07c ELAD, G1-07i Fobos, G1-07m AntSDR | NOT BUILT | Each needs a vendor-library bridge loading a user-installed library with a needed-library page and a stand-in library; none exists. AntSDR today: give its address to the ADALM-Pluto source. Kind: engine (sources). |
| G1-07o WiNRADiO | NOT MINE | Left exactly as it is. |

## New engine commands
None. One new fact: `radio.N.directSampling`.

## SHARED FILE EDITS
- `src/slint/ui/main.slint`: `import { LaneS5 }`, `export { LaneS5 }`, one `KPanel` mount line after lane M's in SIGNAL > Source.
- `src/slint/main.cpp`: the include, the member, the publisher block, three one-line hooks (tick, answers twice) and the `s5.` drive-state line.
- `src/slint/CMakeLists.txt` (one source line), `src/slint/tests/CMakeLists.txt` (the `s5_host` family block), `tests/CMakeLists.txt` (extra sources for `test_engine_api_s5`).
- `src/engine/engine_api_lists.cpp`: the one fact line. No radio-open, failure or stream-start site in `engine.cpp` was touched.
- `resources/lang/*.json`, `src/core/lang_assets.hpp` (regenerated by `tools/embed-lang.py`).

## Expectations changed
- `tests/drive/layout-section-overlap-1600.txt` and `-624.txt`: "Read the pitch now" bottom `<= 2028` became `<= 2058`, and "Sinks" top `>= 2028` became `>= 2058`. Reason: the Source section now carries one more 30 px row ("More source controls"), so the sections under it sit 30 px lower; the rule the scripts guard (a section starts at or below the last control above it) is unchanged and still holds.

## New strings
62 new English keys (every `tr("...")` of `s5_logic.cpp` and `s5_rows.cpp`) in all 33 catalogues, MACHINE-MADE and unchecked by a speaker (a sub-agent wrote them; technical tokens kept). `tools/i18n_keys.py`: 3919 keys, 0 empty, 0 missing, 0 stale in each.

## Gates
Final build `C:/Users/steve/foxsdr-build/s5`, Release, hidden desktop `FoxSDRS5`, `FOXSDR_FRAME_CAP_HZ=60`, a busy machine.
- Own tests: `test_slint_s5_host` 101 checks 0 failed; `test_engine_api_s5` 70 checks 0 failed; `slint_drive_s5-sources` passed (60.85 s); `test_i18n` and `test_error_paths` passed (error-path pin: PENDING 1124 before and after; five validation messages allow-listed with reason `validation`).
- Headless gate `ctest -L "headless|slint" -j3 --timeout 300` (1168 tests): `99% tests passed, 6 tests failed out of 1168`, 2444 s. Reds: `test_slint_house_rules` (MINE: the new .slint file did not import the Theme; fixed, passes); `test_failure_files`, `test_pass_control`, `test_health_app_stepped` (load flakes: all passed re-run alone); `test_slint_tools` (pixel probes of the TOOLS markers specimen at test_slint_tools.cpp:883-900, failed alone too; this lane does not touch that specimen; not run on the base, so UNVERIFIED whether it is a base red); `telemetry_worker` (a Node test of the telemetry Worker, failed alone, not touched by this lane; base not run).
- GUI gate `ctest -L gui -j3 --timeout 600` (385 tests): `99% tests passed, 3 tests failed out of 385`, 2963 s. Reds: `slint_drive_layout-section-overlap-1600` and `-624` (MINE: geometry, expectation updated above, both pass); `test_diagnostics` (the build-stamp test: stale until rebuilt at the final commit; passes after the rebuild, below).
- Stamp tests after a rebuild at the final code commit: `test_diagnostics` Passed (2.76 s, on the hidden desktop) and `test_diagnostics_free` Passed.
- Real `%APPDATA%/foxsdr/config.json` sha256 at the end: `c21ce98dc30e50327f7666f7ee29047c0983d130ca3f21716d0c0e20e04603c3` (unchanged). Nothing pushed, no outside connection, no download, `FOXSDR_ALLOW_*` never set, no real radio or COM port opened.

## UNVERIFIED
- Every row on a real radio. Hardware check for G1-11: open a real RTL-SDR with the native driver, choose I branch at 7.1 MHz and read `radio.1.directSampling` and the spectrum; for G1-05 send a real rtl_sdr stream through netcat; for G1-17 measure on a strong WFM station with a stereo pilot and compare two stations.
- The engine fact `radio.N.directSampling` on a real native RTL driver; the "done" path of automatic ppm.
- Windows system dialogs: none involved.
- All 62 translations by a speaker.

## OPEN ROWS FOR THE NEXT LANE
| Row | What waits | Kind of lane |
|---|---|---|
| G1-13 | Low-IF wiring and first-stream rate check | engine (sources) |
| G1-09 | RSPduo dual tuner | engine |
| G1-14 | user analogue IF bandwidth (RTL source setter, option, fact, field) | engine then control |
| G1-40 | method unknown | research |
| G1-15a/b | SoapySDR bandwidth and driver settings | engine then control |
| G1-03a | SDR++ server client (GPL-helper kind or clean-room) | engine |
| G1-16, G3-18a, G3-18b | CAT, OmniRig, virtual-COM follower | engine/host then control |
| G1-07b/c/i/m | Perseus, ELAD, Fobos, AntSDR bridges | engine (sources) |
| G1-07o | WiNRADiO, with the owner | not this lane |
| radio-reports | a failed raw I/Q open is reported through the path the other sources use; a new journal event for "stream lost" would belong to the radio-reports lane | engine |
| mouse drive | the panel's look at the default window height (it is folded so the Transmit row stays reachable); real hardware checks above | owner / drive |
| ledger | no `docs/slint-port-tick.md` rows were added for the new controls, so no `coverage-s5.tsv` | screen |
