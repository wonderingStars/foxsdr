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
