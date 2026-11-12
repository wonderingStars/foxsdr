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

All five new ops are receiver-scoped, saved per radio (`config.json` keys appended, `config_halves.hpp` rows appended), in no web / CAT / TCI profile, and documented in `docs/api-0.3-additions.md` 9.38 to 9.42. The tests that count ops and the config round trip (`test_apply_command`, `test_radio_scope`, `test_receiver_snapshot`, `test_config`, `test_engine_main_config_roundtrip`, the fuzz seed) were extended in each commit. The reference mock engine (`third_party/foxsdr_api/reference/mock_engine`) was NOT extended for these ops (see UNVERIFIED).
