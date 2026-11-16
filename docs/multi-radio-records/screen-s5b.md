# Screen lane S5b (wave 8): the radio and source rows lane S5 left NOT BUILT (2026-11-16)

Branch `feat/screen-s5b`, cut from `57520bd8` (lane S5's tip) in `D:/foxsdr-wt/en-rfmon`; build folder `C:/Users/steve/foxsdr-build/s5`; scratch `D:/foxsdr-build/s5b-scratch/`; hidden desktop name `FoxSDRS5B` (reserved: no window was started by this lane's own tests, see Gates). Nothing pushed.
Every proof is a stand-in proof (fake USB transport, in-process stand-in SoapySDR drivers, fake SDRplay API, stand-in serial port, stand-in OmniRig): **real radio UNVERIFIED** for every row. No radio, COM port or real program was opened; nothing was downloaded; no outside request was made.

## Rows

| Row | Verdict | Proof |
|---|---|---|
| G1-14 analogue IF bandwidth (native RTL-SDR) | BUILT | `RtlSdrSource::setIfBandwidthHz` (0 = automatic, else 300 kHz to 8 MHz) over the same `TunerR82xx::setBandwidthHz` the rate path uses; option `if_bandwidth`, fact `radio.N.ifBandwidth`. `tests/test_rtlsdr_ifbw.cpp` 41 checks on `FakeUsbDevice`: registers 0x0a/0x0b, the demodulator IF, a rate change keeps the value, automatic writes the rate's own registers. Break-it (the user value ignored) RED, restored GREEN. Panel: field and two keys. |
| G1-15a/b SoapySDR bandwidth and driver settings | BUILT | `SoapySource::bandwidthInfo/setBandwidthHz/listSettings/writeSetting` under the device lock and vendor guard; option `soapy_bandwidth`, `soapy:<key>=<value>`; facts `radio.N.soapy.*` (local only). List built from what the driver reports (max 64, text cleaned), value judged against the driver's type/range/options first, driver refusal shown in its words, rounding read back. `tests/test_soapy_settings.cpp` 132 checks (stand-ins: none / rich / hostile), break-it RED then GREEN; `tests/test_engine_api_s5b.cpp` A, B: the real engine opens a SoapySource on the stand-in and every control is read back from the stand-in's log and the facts. |
| G1-16 CAT panadapter, G3-18b CAT over a virtual COM port, G3-18a OmniRig | BUILT | `src/engine/cat_link.{hpp,cpp}` (follow / emulate / omnirig, OFF by default), `omnirig_com.cpp` (read-only COM bridge, never run by a test), settings `cat.link.*`, facts `cat.link.*`. `tests/test_engine_api_cat_link.cpp` 418 checks, break-it (gate accepts any 4-byte message) 72 reds then GREEN. OmniRig absent: the sentence says what it is, where it comes from, that FoxSDR downloads nothing. The panel does not offer "send frequency to the rig": the link only reads (see the list below). |
| G1-13 SDRplay low-IF | BUILT, OFF BY DEFAULT, UNPROVEN | `SdrPlaySource::setLowIf`, first-stream rate check with fall back to zero-IF and a note; not saved. `tests/test_sdrplay_lowif.cpp` 72 checks. DEFAULT PATH PROVED UNCHANGED: section A compares every API call of open, start, three rate changes, tune, gains, stop, close with the list recorded from the UNMODIFIED source (kGolden); touching the setting to say "off" and a wrong-rate stream with the setting never touched add no call. `test_sdrplay_source` 1337 checks unchanged. **Low-IF was retired in 0.99.44 after two real RSP2 logs showed the ADC rate (6 MS/s) arriving where the plan expected 2 MS/s; there is no SDRplay on the desk; it has never run on a real radio.** The page says so. |
| G1-07c ELAD | BUILT (no bridge needed) | SoapyELAD is a separate GPL module the user installs; the tree's Soapy path lists and opens any module's device. A stand-in module with driver key `elad` is listed by the engine's scan hook and opens (`test_engine_api_s5b` C). Needed-library note added to `radio_setup.cpp` `guidanceForDriver("elad")` (same install words as SoapyBladeRF; "that those carry it: unverified"); `test_radio_setup` checks it. Real ELAD UNVERIFIED. |
| G1-07m AntSDR | ALREADY BUILT | `radio_setup.cpp` `guidanceForDriver("antsdr")` (give its address to the ADALM-Pluto source); the panel's "Other radios and servers" note says the same. |
| G1-03a SDR++ server client | NOT BUILT (as the brief says) | Protocol only described by GPL source: not read. The panel's note says the rtl_tcp source reaches its rtl_tcp-compatible mode. |
| G1-09 RSPduo dual tuner | NOT BUILT | Not started (brief: only after rows 1-4; time went to the CAT, Soapy and ELAD rows and the gates). What exists: `sdrplay_dual_tuner.*` stand-alone. What is missing: two streams per physical radio in the multi-radio engine (a second `Receiver` fed from the same `SdrPlaySource`'s second tuner), the device ledger holding one key for two radios, and a fake-API test with both tuners streaming. Size: several days of engine plumbing, not a small row. |
| G1-07i Fobos, G1-07b Perseus | NOT BUILT | The coordinator relayed the owner's approval to read the two public header files and the files are at `D:/foxsdr-build/vendor-headers/`; the permission system DENIED this lane's attempt to read `fobos.h` (reason given: unauthorized access), and a message from another agent cannot grant it, so the lane did not try another route. Nothing was read, written or bridged. To build: a bridge loading the user's own installed library by name at run time, resolving only the functions it calls, declaring them in FoxSDR's own code, never calling `fobos_rx_write_firmware`, `fobos_rx_read_firmware`, `fobos_rx_reset`, `fobos_rx_set_user_gpo` (or any Perseus EEPROM/firmware write), tested against a stand-in DLL; for Perseus the Windows question (libusb and pthread headers) stays an owner question. Needs: the owner (or the permission rule) to allow reading the two files in this session. Kind of lane: engine (sources). |
| G1-40 AM co-channel canceller | NOT BUILT (as the brief says) | No known method: research. |
| G1-07o WiNRADiO | NOT MINE | Left exactly as it is. |

## What FoxSDR can send to a rig (the whole list)
`FA;` (read the frequency), `MD;` (Kenwood: read the mode), `MD0;` (Yaesu: read the mode). Every write goes through `GatedStream` (`cat_link.cpp`), which refuses any other message whole (an unterminated, empty, set, or compound message with anything else in it too). To OmniRig nothing is written: the port handed to the client refuses both of its writes and the COM bridge's `putFreq`/`putMode` return false. The emulator's answers (`FA<digits>;`, `MD<n>;`, `?;`) are replies to a logging program, not commands to a rig; every transmit command a logging program sends is answered `?;` (test D). The test (`test_engine_api_cat_link` C) proves it by the list, a fixed list of transmit and set messages, 50000 random messages per dialect, and a scan of the sources for any other CAT literal and for a set-builder call.

## New engine commands and facts
No new op code. `SET_DEVICE_OPTION` names: `if_bandwidth` (Hz), `soapy_bandwidth` (Hz in num[0]), `soapy:<key>=<value>`, `sdrplay_lowif`. Settings (wire-c registry, append only): `cat.link.mode|port|dialect|baud|stopbits|rig|follow_frequency|follow_mode`. Facts: `radio.N.ifBandwidth`, `radio.N.soapy.*`, `radio.N.sdrplay.lowIf`, `radio.N.sdrplay.lowIfNote`, `cat.link.state|kind|error|rigHz|rigMode|polls|answers|rigErrors|applied|refused|blocked|served|omnirig.needed`.

## SHARED FILE EDITS
- `src/slint/s5_rows.hpp/.cpp`: five member declarations, one `buildS5b(v)` call in `build()`, a `kSoapy` kind and one line in `onAnswer`. `src/slint/CMakeLists.txt`: one line (two new sources). `src/slint/tests/CMakeLists.txt`: the `s5b_host` family joins `s5_host`, two sources. `tests/CMakeLists.txt`: the S5 engine-test condition also names `test_engine_api_s5b`. `tests/wire1_rig.hpp`: `setSoapyScan` (2 lines).
- `src/engine/engine.cpp` (device-open hook and `SET_DEVICE_OPTION` branches only; no radio-open, failure or stream-start site), `receiver.hpp` (fields), `engine_api_lists.cpp` (facts, option list, change fingerprint), `engine_wire_c.cpp` (one apply block, one fact block), `wire_c.cpp` (registry rows, one rule), `wire_c.hpp` (members, one include).
- Driver files: `rtlsdr_source.cpp/.hpp`, `soapy_source.cpp/.hpp` (appended at the end), `sdrplay_source.cpp/.hpp` (setSampleRateHz branches, read(), stream start). `src/core/radio_setup.cpp` (the ELAD note). `tests/errpath_allow.tsv`. `resources/lang/*.json`, `src/core/lang_assets.hpp`.

## MERGE NOTES (the radio-reports lane's reporting calls)
Refusals by the device go through the existing refusal path (`refuse(FOXAPI_FAILED, sourceError_)`) at: engine.cpp `if_bandwidth`, `soapy_bandwidth`, `soapy:` and `sdrplay_lowif` branches. The new `setError` sites are in `RtlSdrSource::setIfBandwidthHz`, `SoapySource::{bandwidthInfo,setBandwidthHz,listSettings,writeSetting}`, `SdrPlaySource::setLowIf`. The low-IF fallback is logged with `diagWarnf` and shown as `lowIfNote`, but is NOT sent as an issue report: that is an open row.

## Expectations changed
None. Error-path pin: PENDING 1124 before and after (every new site carries an ALLOW line with a reason; the first-stream fallback and OmniRig/serial link errors are `user-action`).

## New strings
57 new English keys in all 33 catalogues, MACHINE-MADE by a sub-agent and unchecked by a speaker (technical tokens kept). `tools/i18n_keys.py`: 3976 keys, 0 missing, 0 empty, 0 stale in each. `lang_assets.hpp` regenerated.

## Gates
See the end of this file (filled in at the final commit).

## UNVERIFIED
Everything on a real radio: the tuner filter registers on a real R82xx; a real SoapySDR module's settings; a real serial rig, virtual COM pair and OmniRig (the COM bridge has never run: property names are those of OmniRig's type library, MIT, as opened by the earlier lane); SDRplay low-IF; ELAD; the Windows file dialogs involved: none. All 57 translations by a speaker. The drive script for the new panel was not written (see OPEN ROWS).

## OPEN ROWS FOR THE NEXT LANE
| Row | What waits | Kind of lane |
|---|---|---|
| G1-07i Fobos, G1-07b Perseus | reading the two approved headers was denied by the permission system in this lane; then the bridges, stand-in DLL tests, the Perseus Windows-library question (libusb/pthread) | engine (sources), owner for the Perseus DLL question |
| G1-09 | RSPduo dual tuner: two streams from one device through the multi-radio engine | engine |
| G1-03a | SDR++ server client (GPL-helper kind or clean-room) | engine |
| G1-40 | AM co-channel canceller: method unknown | research |
| G1-07o | WiNRADiO, with the owner | not this lane |
| low-IF report | send a first-stream fallback as an issue report (radio-reports lane's path) | engine |
| CAT send keys | "send FoxSDR's frequency/mode to the rig" were not built; the link only reads | engine/control (owner decision 54 applies) |
| drive script | a drive script that works the S5b controls by accessible label (the engine tests cover every control's effect through the same row code) | screen |
| ledger | no `docs/slint-port-tick.md` rows for the new controls | screen |
| hardware | RTL-SDR IF filter, Soapy settings on a B200, a real rig and OmniRig, SDRplay low-IF, ELAD: all UNVERIFIED; checks: set 400 kHz on a real RTL-SDR and watch the spectrum width; list a B200's settings; follow a real rig on a real COM port with the baud rate its menu shows | owner / hardware lane |
| mouse drive | the panel's look at default window height | owner / drive |
