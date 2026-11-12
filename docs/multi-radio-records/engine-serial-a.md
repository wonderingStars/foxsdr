# Engine serial lane A (`serial-a`) - branch feat/engine-serial-a

Cut from `feat/engine-wire-2` tip 5a45640f on 2026-11-12 07:56 (worktree `C:\Users\steve\foxsdr-wt\mr-engine1`, build `D:/foxsdr-build/mr-engine1`, Slint ON).
Time limit: no new row after 15:56 (8 hours of wall clock). Written in the order the work was done; the last sections are the final state.

## 1. Step 0 table (reading only; "block" = a tested `src/dsp` or `src/core` header that nothing in `src/` includes)

Sources opened: `docs/multi-radio-records/engine-wiring-1.md` and `engine-wiring-2.md` (sections 2.8, 3, 6), the survey `D:/foxsdr-build/w2-scratch/step0-D.md` section D, the row text of `FEATURE-GAPS.md` and
`feature-gaps-survey.md` section 1, `queued-briefs/remaining/INDEX.md`, and the code (grep of `src tests` for each header; `git log -S` for the names).

| Row | State | Where / evidence | Plan |
|---|---|---|---|
| R5 raw I/Q over UDP/TCP as a source | new (shared body exists) | `source/net_sample_source.hpp` is the reader-thread/ring body of SoapyRemote, SDRconnect, HPSDR (merged in feat/radio-sources, not yet registered); no source for plain I/Q streams | new `source/raw_iq_net_source.*` on that body; framing documented in the file |
| R10 hot-plug detection | new | no `WM_DEVICECHANGE`/`RegisterDeviceNotification` anywhere in `src` (grep); `engine/device_ledger.hpp` exists; `feat/sysmodels` has a standalone watcher (read its record first) | poll/notice -> ledger diff -> fact; injected events only, no unplugging |
| R11 direct-sampling control (RTL) | new | `source/rtlsdr_source.cpp:94` `kDirectSamplingBelowHz = 24e6`, automatic switch at :916 | manual override (auto / force on / force off), device option, fact |
| R18 automatic ppm calibration | partly built | `core/ppm_correction.hpp` + `engine/engine_ppm.cpp` apply a typed value; no measurement loop | pilot-based measurement model + engine loop |
| R19 tuner pooling | new (L) | multi-radio engine (`engine_radios.cpp`) has no claim-any-free-radio logic | pure model + engine claim op, only if time allows |
| D4 MPX output for RDS Spy | new | StereoFm sees the multiplex inside the pipeline; no 192 kHz output | file/sink of the multiplex |
| D5 pseudo-stereo | plugin place | `plugin-or-host.md`: own plugin "audio-effects" via AUDIO_PROCESSOR; not host work | DECISION: not built in the host; recorded |
| D6 adjustable CW pitch | already built | W1 C3 `b4e3bafc` (`SET_CW_PITCH 0x0118`, 300..1200 Hz, `Demodulator::setCwPitchHz`); the filter half is F6/F8 | nothing; filter half in F8 |
| D7 medium and super-wide FM widths | new | `core/receiver_tables.hpp:79` `kBwHz[6] = {200000, 150000, 12500, 10000, 6000, 3000}` | add widths to the table, append-only |
| F6 filter shape, window, order | block exists, wire it | `dsp/cw_filter.hpp` has a test (`test_cw_filter`) and no includer | wire with F8 |
| F8 CW peak filter / APF | block exists, wire it | same header | wire |
| F9 several manual notches | already built | W1 C2 `9f475cfa` (`SET_NOTCH_SLOT 0x0117`, ten slots per radio) | nothing |
| F10 AM co-channel canceller | new (method open) | no dsp file; method not documented | report unless an opened source gives a method |
| F11 audio equaliser, F12 audio HP/LP | already built | W1 E1 (`0x011A`, `0x011B`; `dsp/audio_eq.hpp` included by `core/pipeline.hpp`) | nothing |
| F13 AFC for AM/FM; CW auto-tune | block exists, wire it | `dsp/afc.hpp` has a test (`test_afc`) and no includer | wire |
| F15 FM noise squelch | already built | W1 C4 `f89a6019` (`0x0119`) | nothing |
| F16 generic I/Q balance, DC removal, swap I/Q | block exists, wire it | `dsp/iq_correct.hpp` (test only) | wire |
| F20 per-channel volume, pan | block exists, wire it | `dsp/stereo_place.hpp` (test only); per-radio volume exists | wire pan |
| S1 FFT size, S2 FFT window | block exists, wire it (L) | `dsp/spectrum_block.hpp` (test only); `kFftSize = 1024` at `core/receiver_tables.hpp:40`, 18 uses in `src` | wire with a variable bin count |
| S3 frame-rate setting | wire with S1 | same | wire |
| S12 zoom FFT of the IF passband | block exists, wire it | `dsp/zoom_fft.hpp` (test only) | wire if time allows |
| T4 centre tuning (CTUN) | new | engine tuning semantics | after the DSP rows |
| T6 VFO A/B, swap, split | block exists, wire it | `core/operating_aids.hpp` `VfoPair` (TuneSource threading needed) | receive-only A/B |
| T7 several VFOs in the main window | new (L) | one channel per radio in the receive path | report unless time allows |
| P4 IF recording | block exists, wire it | `core/if_recorder.hpp` reached only via `rec_browser.hpp` | tap after `vfo_.process` |
| P7 sample-format choice for recordings | new | `core/recorder.cpp`: baseband = float32 WAV, audio = PCM16 WAV | choice setting |
| P15 seek bar, A-B loop, real start time | block exists, wire it (L) | `source/iq_playback.hpp` (reached only via `rec_browser.hpp`); `IqFileSource` has no seek | adapter through `Engine::installOpenedIqFile` |

Rows already built by W1 (nothing to do): D6, F9, F11, F12, F15. Plugin place: D5. The brief's own count of 29 is not exactly the id list (the id list is 31); this table names every id.

## 2. Rows built, in the order done (one commit each; break-it = the behaviour removed, RED seen, restored, GREEN)

| # | Commit | Rows | Op / state | Decisive result | Break-it |
|---|---|---|---|---|---|
| 1 | c0ed1cb6 | S1 S2 S3 | `SET_SPECTRUM` 0x011C: FFT 1,024..131,072, window 0..4, frame rate 0 (automatic: 30/s) or 1..120; state `spectrumFftSize/Window/FrameRateHz`, saved per radio | `test_pipeline_spectrum` 81 checks (tone + noise + 40 ppm clock error: level within 1.5 dB of the legacy reading at 4k..131k, noise per bin falls with the size, frame rate 10 -> 10.0/s, 50 -> 49.7/s); `test_engine_api_spectrum` A/B/C (ring serves 1,024 / 8,192 / 16,384 bins; 65,536 is max-reduced to the ring's 16,384); CPU: `test_spectrum_cost` 65,536 bins at 30/s = 1.2 %, 1.2 %, 1.2 % of one core at 2.4 MS/s (legacy 1.3 %, 131,072 bins 2.6 / 2.4 / 2.5 %) | block path never switched on: `test_engine_api_spectrum` C fails (3 checks) |
| 2 | a6dfabed | P15 (seek, A-B loop, position, real start time) | `FILE_SEEK` 0x861A, `FILE_LOOP` 0x861B; 7 measurements on the radio state | `test_iq_file_seek` 66 checks (loop seam without gap or repeat, a seeker thread against a reader for 2 s, 14 file-name shapes); `test_engine_api_fileseek` 44 checks (a 10 s file as radio 2: position stays in 1.010..1.400 s inside a 1.0..1.4 s loop) | seeks not applied: 5 checks of `test_iq_file_seek` and B of `test_engine_api_fileseek` fail |
| 3 | 66ad3981 | F16 | `SET_IQ_CORRECT` 0x011D (1 DC, 2 balance, 4 swap), `iqCorrect` saved | `test_pipeline_iqcorrect` 26 checks: image -41.3 -> -57.4 dB, DC spike -33.6 -> -80.7 dB, swap puts the tone where its image was | process() removed: 6 checks fail |
| 4 | 440af1b4 | F13 | `SET_AFC` 0x011E (switch, pull-in 50..20,000 Hz), `afcOn/afcPullInHz` saved | `test_pipeline_afc` 28 checks: carrier 1,500 Hz away caught at 1498.6 Hz and locked, a 100 Hz/s drift followed to 1898.7 Hz, CW peak mode 1450.7 Hz | tracker never fed: 5 checks fail |
| 5 | 9aa59ec4 | F8 (the audio half of F6/F8; the channel-shape half of F6 is NOT built) | `SET_CW_PEAK` 0x011F (switch, width 30..500 Hz, boost 0..24 dB) | `test_pipeline_cwpeak` 26 checks: tone +12.0 dB exactly, off-pitch noise within 0.8 dB, USB total power within 0.1 dB | process() removed: 2 tone checks fail |
| 6 | 80cf59f9 | F20 (pan), D5 (pseudo-stereo, binaural) | `SET_STEREO_PLACE` 0x0120 (0 off / 1 pan / 2 pseudo / 3 binaural, one value), saved per radio | `test_pipeline_stereoplace` 31 checks: off = identical samples; pan -1/+1 puts the other ear 300 dB down, centre -3.0 dB each; pseudo 1.0 correlation 0.00 and 0.5 correlation 0.60; binaural +60: right 7.8 dB louder (5.2 ILD + 2.8 dB white-noise cost of the interpolated delay), left 23 samples later (Woodworth 23.4) | placer process() removed: 6 checks fail |
| 7 | 6477c524 | T6 (receive-only A/B, swap, copy; NO split) | `VFO_SELECT` 0x861C, `VFO_SWAP` 0x861D, `VFO_STORE` 0x861E; state `vfoActive` + the other register; saved per radio; `engine_vfo.cpp` | `test_engine_api_vfoab` 84 checks (A's frequency, mode and bandwidth come back; swap twice restores; locked radio DENIED) | retune removed from the exchange: 5 checks fail |
| 8 | 8259636b | P7 | `SET_RECORD_FORMAT` 0x0121: I/Q float32 or int16; audio int16, int24 or float32; `Recorder::setSampleFormat` | `test_recorder_formats` 51 checks (int16 I/Q replay worst error 1.53e-05 against a step of 3.05e-05; int24 worst error 5.96e-08 against 1.19e-07; the 4 GiB ceiling counted in the take's own frame size); `test_engine_api_recformat` 68 checks incl. five real takes through the engine | header always the default: 4 + 3 checks fail |
| 9 | 9b7b60f8 | R5 | source kind `rawiq`: "open-rawiq:[tcp://\|udp://]host:port[?fmt&rate&freq&hdr=1&timeout]"; FXIQ framing (the pair of `net/iq_out.hpp`) or headerless | `test_raw_iq_net_source` 132 checks incl. real loopback sockets; `test_engine_api_rawiq` 32 checks (radio 2 shows the tone at +25 kHz / -30 kHz; same stream twice BUSY; REMOTE DENIED) | partial-sample carry removed: 5 tests fail |

All five new ops are receiver-scoped, saved per radio (`config.json` keys appended, `config_halves.hpp` rows appended), in no web / CAT / TCI profile, and documented in `docs/api-0.3-additions.md` 9.38 to 9.42. The tests that count ops and the config round trip (`test_apply_command`, `test_radio_scope`, `test_receiver_snapshot`, `test_config`, `test_engine_main_config_roundtrip`, the fuzz seed) were extended in each commit. The reference mock engine (`third_party/foxsdr_api/reference/mock_engine`) was NOT extended for these ops (see UNVERIFIED).

| 10 | ef015b1e | R10 | no op; `startHotplugWatch` after `initialise()` in the real service; facts `fact.hotplug.on/bursts/arrivals/removals`; `sys_device_watch.{hpp,cpp}` byte for byte from `feat/sysmodels` 21f38996 | `test_engine_api_hotplug` 57 checks (a burst of three arrivals and a removal under both classes is one action; a monitor counts nothing; an arrival resets a waiting busy retry to 0 attempts, a removal alone leaves it; no serial or vendor id in any fact; the two source files print nothing; shutdown stops it) | retry re-arm disabled: 2 checks fail |

## 3. Rows NOT built, in the order a follow-up should take them (each with what stopped it)

1. **The screen half of every built row** (S1/S2/S3, F13, F16, F8, F20/D5, T6, P7, P15, R5): each needs its Slint control, strings, ledger rows and a drive script; none was started (section 4 is the exact list). A `main.slint` compile is 14 to 40 minutes and the controls are best batched with the other screen lanes' (the merge order matters for `main.slint`, which this lane did not touch).
2. **R11 direct sampling (RTL), a latent finding first.** `source/rtlsdr_source.cpp` switches to direct sampling below `kDirectSamplingBelowHz` = 24 MHz (line 94, used at line 916) but `setCenterFrequencyHz` refuses any frequency below the tuner's range first (`rangeLocked`, line 993: `kR82xxLoHz` = 24 MHz for a non-V4 dongle, 500 kHz for a V4), so the automatic branch cannot be reached on a dongle without an upconverter. A manual override therefore has to widen the allowed range as well as pick the mode, and the register sequence for the two direct modes is only provable on the wire with `usb::FakeUsbDevice` (as `test_rtlsdr_source.cpp` does for the rest). Not started. UNVERIFIED: which branch an HF modification wires up.
3. **P4 IF recording.** The take machinery (`startIqRecording`: the async open worker `iqStart_`, `takeFinishing`, the finish queue, `shutdownQuiesce`, the naming template) is written once for the raw tap; an IF take needs the same around a second recorder fed from `chanBuf_` after `vfo_.process` (the sample rate is the channel rate, `Pipeline::channelRateHz()`). The recorder side is ready (P7's int16 I/Q halves its size); the engine side is a copy of those paths with a second `Recorder` and a second `RecordStart`. `core/if_recorder.hpp` was not used.
4. **S12 zoom FFT of the IF passband** (`dsp/zoom_fft.hpp`): needs a second spectrum ring (the engine API serves one frame per radio) and a Slint view; ABI-visible.
5. **T4 centre tuning (CTUN)**: tuning semantics of `Engine::tuneAbsoluteHz` (today the counter keeps the VFO offset and moves the centre); needs the owner's definition of the toggle before code.
6. **R18 automatic ppm calibration**: the 19 kHz stereo pilot of a broadcast station is only as exact as the station (the standard allows +-2 Hz, 105 ppm of 19 kHz), so the method needs a reference whose accuracy is looked up and opened first (a GPS-locked station list, or a measurement against a known carrier); `core/ppm_correction.hpp` applies a typed value only.
7. **F6 channel-filter shape and order**: `designChannelFilter` (Kaiser, shape factors 1.3 / 1.8 / 3.16, Gaussian above 2.6) is a tested design helper; `dsp/vfo.cpp` builds its own FIR from the bandwidth and has no seam for taps. A seam in `Vfo` plus the shape choice is the row. (The audio half of F6/F8, the CW peak filter, is built.)
8. **D7 medium and super-wide FM widths**: the engine accepts any width from 3 kHz to 0.9 of the channel rate (`Vfo::clampBandwidth`), so 30 and 50 kHz "medium" widths work today through `SET_BANDWIDTH`; the presets are the Bandwidth combo's table (`core/receiver_tables.hpp kBwHz`, six steps whose count the Slint labels and several tests pin: a screen change). "Super-wide" above about 225 kHz cannot be reached: the channel rate is chosen in [150 kHz, 300 kHz) (`kTargetChannelRateHz` = 200 kHz) and the clamp is 0.9 of it.
9. **D4 MPX output for RDS Spy**: the multiplex exists only inside `StereoFm` at the channel rate (about 200 kHz); an output at 192 kHz needs a resampler from the channel rate and a sink (a file sink or a virtual cable). Not started.
10. **R19 tuner pooling, T7 several VFOs in the main window**: the engine has one channel per radio; both extend the multi-radio engine's claim logic (L rows).
11. **F10 AM co-channel canceller**: no opened source describes a method; an own design needs the owner's go-ahead on the approach (a research row).
12. **P15 whole-file waterfall**: only seek, loop, position and real start time were built; the overview is a pass over the file with the spectrum block into a small picture (a worker, `source/iq_readers.hpp` for the formats) and a Slint view.
13. **Mock engine**: `third_party/foxsdr_api/reference/mock_engine` and its test were NOT extended for the ops and state this lane added (section 6 says whether any gate compared them with the header).

## 4. Screen hand-off (English labels; nothing translated; every control a Slint lane or this lane's follow-up must add)

| Control | Label | Op / fact | Range | Default |
|---|---|---|---|---|
| Spectrum size | "FFT size" | `SET_SPECTRUM` `ival[0]`, `spectrumFftSize` | 1024, 2048 ... 131072 (powers of two) | 1024 (the brief's recommended 8,192 is a one-line default change, see decisions) |
| Spectrum window | "Window" with "Rectangular", "Hann", "Hamming", "Blackman-Harris", "Flat top" | `SET_SPECTRUM` `ival[1]`, `spectrumWindow` | 0..4 | 3 |
| Spectrum frame rate | "Frame rate" with "Automatic" | `SET_SPECTRUM` `num[0]`, `spectrumFrameRateHz` | 0 automatic, 1..120 | 0 |
| I/Q correction | "Remove DC offset", "Balance gain and phase", "Swap I and Q" (three switches, one op: the sum 1 + 2 + 4); read-out "Image rejection" | `SET_IQ_CORRECT`, `iqCorrectMask`, `iqImageRejectionDb` (200 = not measured) | 0..7 | 0 |
| AFC | "AFC" switch, "Pull-in range" | `SET_AFC`, `afcOn`, `afcPullInHz`; read-outs `afcCorrectionHz`, `afcLocked` | 50..20,000 Hz | off, 2,500 |
| CW peak filter | "Peak filter" switch, "Width", "Boost" | `SET_CW_PEAK`, `cwPeakOn/WidthHz/BoostDb` | 30..500 Hz, 0..24 dB | off, 100, 12 |
| Stereo placement | "Placement" with "Off", "Pan", "Pseudo-stereo", "Binaural"; one slider whose caption follows the mode ("Pan", "Amount", "Angle") | `SET_STEREO_PLACE`, `stereoPlaceMode/Value` | pan -1..1, amount 0..1, angle -90..90 | off |
| VFO A/B | "A", "B" (select), "A<>B" (swap), "A=B" (copy) | `VFO_SELECT`, `VFO_SWAP`, `VFO_STORE`; `vfoActive`, `vfoOther*` | | A |
| Recording format | "I/Q file format" with "32-bit float", "16-bit integer"; "Audio file format" with "16-bit", "24-bit", "32-bit float" | `SET_RECORD_FORMAT`, `recordIqFormat/AudioFormat` | 0..1, 0..2 | 0, 0 |
| Seek bar | a bar at `filePositionSec / fileDurationSec`, "A" and "B" loop markers, a "Loop" switch, the start time as a clock | `FILE_SEEK` (`ival[0]` 0 s / 1 fraction), `FILE_LOOP`; the `file*` state | | |
| Raw I/Q stream | a row in the add-radio card: "I/Q stream (TCP or UDP)" with the address field and the hint "host:port, udp://host:port, ?rate=...&fmt=u8, s16 or f32, or ?hdr=1" | `RADIO_ADD` / `SELECT_SOURCE` "open-rawiq:" + the text | | |
| Hot-plug | none; an optional line "USB radios plugged in: n, pulled out: n" | `fact.hotplug.*` | | |

## 5. New settings keys and strings

Per radio, in `config.json` and the `radios` entries: `spectrumFftSize`, `spectrumWindow`, `spectrumFrameRateHz`, `iqCorrect`, `afcOn`, `afcPullInHz`, `cwPeakOn`, `cwPeakWidthHz`, `cwPeakBoostDb`, `stereoPlaceMode`, `stereoPlaceValue`, `vfoActive`, `vfoOtherHz`, `vfoOtherMode`, `vfoOtherBwHz`, `recordIqFormat`, `recordAudioFormat`. The source kind word `rawiq` (saved radio / `sourceKind`) is NOT yet in the telemetry vocabulary (`health::vocabulary()`, `core/event_trail.cpp kDrivers`, `core/telemetry.cpp nativeFamilyModel`, the Worker): the Worker first, then the application; until then it reports as `other`.
New strings: none in the string tables (no control was built); the English strings are those of section 4.

## 6. Gates

Machine: about twenty other lanes; Release build in `D:/foxsdr-build/mr-engine1` (Slint ON, `cmake --build . --config Release --parallel 3`, exit 0, 0 errors); headless in slices of 100 test numbers (`ctest -L "headless|slint" -I a,b -j4 --timeout 300`, 912 tests); GUI on the hidden desktop `FoxSDRTests` with `FOXSDR_FRAME_CAP_HZ=60`, `-L gui -j3`.

| Gate | Tip | Result |
|---|---|---|
| headless + slint, first run | 86450ff8 | 912 tests, 7 red: the five known false failures (`test_airband_data`, `test_band_plan`, `test_converter_call_sites`, `test_page_resize_id`, `test_png_decode`), `test_diagnostics_free`, and **`test_plugin_containment`, which was this lane's**: its scan read `specBlock_->columns()` in `pipeline.cpp` as a plugin ABI call. Fixed in 54e3067a (a reference, no behaviour change); passes alone. |
| GUI (`-L gui`, 278 tests) | 54e3067a build (fresh rebuild after the fix) | **278 of 278 passed**, 2,109 s |
| headless + slint, final | 54e3067a | 912 tests, 7 red, all explained in 6b (five known false failures, `test_diagnostics_free`, and the load flake `test_engine_main_password_not_in_argv` which passes alone) |

### 6b. Every red of the final headless gate, by name, with its result re-run alone (compare with section 8b of `engine-wiring-2.md`)

Final headless gate at 54e3067a: 912 tests, 7 red, 905 passed.

| Test | First gate | Final gate | Alone | Class |
|---|---|---|---|---|
| `test_airband_data` | red | red | **fails alone** (0.05 s) | known false failure |
| `test_band_plan` | red | red | **fails alone** (0.07 s) | known false failure |
| `test_converter_call_sites` | red | red | **fails alone** (0.01 s) | known false failure |
| `test_page_resize_id` | red | red | **fails alone** (0.01 s) | known false failure |
| `test_png_decode` | red | red | **fails alone** (0.01 s) | known false failure |
| `test_diagnostics_free` | red | red | **fails alone** (6.34 s) | stale-stamp family, as in W2 8b (its gui sibling `test_diagnostics` passes in the GUI gate) |
| `test_engine_main_password_not_in_argv` | green | red | **passes alone** (6.03 s) | load (it is in W2's list as the same load flake) |
| `test_plugin_containment` | red (mine) | green | passes alone | fixed in 54e3067a |

No crash among them: no test of either gate ended in an access violation; `test_engine_api_merge` was green in both.

The reference mock engine was not extended and no test compares it with the header (`test_mock_engine` and the conformance tests pass).

## 7. Decisions for the owner

1. **Spectrum default size.** Kept at 1,024 / Blackman-Harris / automatic, which is the legacy estimator unchanged. The brief recommends 8,192 as the default; that is a change of one number (`Receiver::spectrumFft_` and the `AppConfig` default) but it changes the frame every view receives (the ring serves at most 16,384 bins, a wider FFT is max-reduced), so it should go with the screen lane that draws them.
2. **Spectrum level.** A tone reads the same level at every size and window (the block's compensated reading is lowered by the legacy estimator's -8.9 dB so the S-meter calibration, the noise floor and thresholds do not move). The alternative, calibrated dBFS, would move all of them.
3. **Pseudo-stereo (D5)** is placed as a plugin by `plugin-or-host.md`; the host already had it as a mode of the stereo placer, so it is offered (`SET_STEREO_PLACE` mode 2) and the plugin route is not closed.
4. **VFO split** is not built (a receive-only program has nothing to transmit on B).
5. **16-bit I/Q recordings clip** beyond full scale (a float take keeps it); offered as an option, float stays the default.
6. **Raw I/Q source.** A UDP source listens where its address says (127.0.0.1 hears this computer only; 0.0.0.0 has to be spelled); a stream with a declared centre refuses a retune to another number. The telemetry word `rawiq` waits for the Worker.
7. **Hot-plug.** No off switch (a local notification, no network); a radio that died by being unplugged is not re-opened (its descriptor is not kept anywhere the retry could use).
8. **AFC** moves the real VFO by a few hundred hertz while the counter keeps showing the frequency the user chose (`vfoOffsetHz` is the user's own); the correction is `afcCorrectionHz`. Confirm that the counter should not follow it.
9. **R11:** the automatic direct-sampling branch is unreachable on a dongle without an upconverter (section 3, item 2). Decide whether the manual override should also widen the allowed range to HF.
10. **`sys_device_watch.{hpp,cpp}`** are identical to `feat/sysmodels` 21f38996 and the thread-inventory row is its text (it still says "not yet wired into the engine"): the merge is clean, the sentence is stale.

## 8. UNVERIFIED (everything not run)

* A real unplug and re-plug (no radio was touched), the OS registration of the device-change notice (the sysmodels lane's), and any real RTL-SDR, Airspy or other radio.
* AFC and CW peak on a real off-air signal; the stereo placement on a real stereo decode (only the "mono audio only" rule is exercised, with USB noise); I/Q correction on a real imbalanced dongle (generated: image -41.3 -> -57.4 dB).
* Spectrum sizes above 16,384 bins reach a reader only as the maximum of groups (tested to 65,536 through the ring); sizes above that reach only the engine's own frame. No view was checked against 131,072 bins.
* The Slint window with any of the new state (no control was built); the web, CAT and TCI surfaces do not offer the new ops.
* The wire protocol of a third-party sender other than FoxSDR's own FXIQ framing (u8/s16/f32 headerless formats follow the usual conventions and were tested against streams generated here only).
* The `usb` direct-sampling path (R11), IF recording (P4) and everything in section 3.

## 9. Configuration file

The real `%APPDATA%/foxsdr/config.json` sha256 before (start of the lane) and after (checked at 07:56, 10:05, 11:30 and at the end): `c21ce98dc30e50327f7666f7ee29047c0983d130ca3f21716d0c0e20e04603c3`, byte-identical. Every engine test runs with the profile redirected (`engine_api_test::hermetic`) or in a scratch folder. No radio was opened, no Windows dialog was raised (the GUI tests ran on the hidden desktop only), nothing was pushed, nothing was downloaded.

## 10. Rows found already built (the second request: each re-verified at this tip, one commit each)

* **D6 adjustable CW pitch** - built by W1 C3 b4e3bafc: SET_CW_PITCH 0x0118, 300 to 1200 Hz, Demodulator::setCwPitchHz replaces kCwToneHz, saved per radio (cwPitchHz). Re-run at tip 1c1a6688: test_pipeline_cwpitch Passed 8.22 s (a carrier beats at 450/1000/300/1200 Hz, kept across mode and rate changes), test_engine_api_cwpitch Passed 2.36 s. Left: the CW pass band centred on the pitch is the F6/F8 filter half; F8's audio peak filter (this lane, 9aa59ec4) follows the pitch.
* **F9 several manual notches** - built by W1 C2 9f475cfa: SET_NOTCH_SLOT 0x0117, ten slots per radio (dsp/notch_bank.hpp), saved per radio (notchBank). Re-run at tip 1c1a6688: test_pipeline_notchbank Passed 5.55 s, test_engine_api_notchbank Passed 2.44 s. Left: placing a notch by a click on the spectrum is the screen's (B7).
* **F11 audio equaliser** - built by W1 E1 (dsp/audio_eq.hpp): SET_EQ_BAND 0x011A (bass, middle, treble, five graphic bands, -12..+12 dB), saved per radio (eqDb). Re-run at tip 1c1a6688: test_pipeline_audioeq Passed 6.55 s, test_engine_api_audioeq Passed 2.84 s. Left: none beyond the control.
* **F12 audio high-pass / low-pass toggles** - built by W1 E1 (dsp/audio_eq.hpp): SET_AUDIO_FILTERS 0x011B (high-pass off/100/300 Hz, low-pass off/2400/3000/6000 Hz), saved per radio (audioHighPassHz, audioLowPassHz). Re-run at tip 1c1a6688: same tests as F11 (the filters are the equaliser block's). Left: none beyond the control.
