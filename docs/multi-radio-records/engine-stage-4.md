# Multi-radio engine, stage 4: slots live (record)

Branch `mr/engine-stage4`, cut from f2213607 (the merge of engine stages 1 to 3), built 2026-11-10 to 11 in the worktree `C:\Users\steve\foxsdr-wt\mr-engine1`, build folder
`D:/foxsdr-build/mr-engine1`. Design: `docs/multi-radio-engine-design.md` of the engine-stage4 worktree (untracked there, not copied; its last section supersedes every "four":
`kMaxReceivers` = 8, at most eight open radios in total). What stages 1 to 3 built and left for this stage: `docs/multi-radio-records/engine-stages-1-3.md`.

## Commits (oldest first)

| Commit | Subject |
|---|---|
| 9abab014 | AudioOut: a route (both, left, right, none) and a solo silence, ramped (part 1) |
| 4168d75b | Engine: `rx()` - every engine function acts on the receiver slot its thread selected (part 2: the mechanical conversion; `tools/multi_radio_slot_rewrite.py`) |
| c10bc6bd | Engine: slots live - RADIO_ADD / RADIO_REMOVE, test radios, per-radio passes, reads, audio route and solo, recorders |
| 950f27a1 | Engine: an added radio keeps the audio stream its pipeline opened; the load tool shows the sinks |
| (this file) | docs: multi-radio record, stage 4 |

## What was built

The engine now runs up to eight radios at once. `RADIO_ADD` builds a `Receiver` (its own `Pipeline` with source and DSP threads and its own `AudioOut`, device, open worker, fault and retry
state, health counters, recorders and published snapshot) into a free slot from 2 to 8; `RADIO_REMOVE` stops it (RUN 0 through `applyCommand`: the takes end first, the threads join), reaps
its open worker, lets go of its device and frees the slot. Radio 1 is unchanged: with no second radio every function does what it did.

| Piece | Where |
|---|---|
| `Engine::rx()`, `SlotScope`, `RadioWork`, `forEachReceiver` (the slot an engine function acts on) | `engine.hpp` |
| The mechanical conversion of 1389 alias uses to `rx().<name>` (idempotent, re-runnable after a merge) | `tools/multi_radio_slot_rewrite.py` |
| `RADIO_ADD`, `RADIO_REMOVE`, `RADIO_AUDIO`, `RADIO_SOLO`; the added radios' pass and publish; take-file prefix | `engine_radios.cpp` |
| Route and solo silence in the sink, ramped (10 ms) | `sink/audio_out.{hpp,cpp}` |
| Per-radio spectrum rings and scratch frames, audio rings and feeds, scope taps and snapshot reads in the API host | `fox_engine_api.cpp` |
| Ops 0x1202 to 0x1205, `fact.radio.<n>.audio` / `.refusal`, docs 9.20 | `foxsdr_api.h`, `app_commands.cpp`, `engine_api_lists.cpp`, `docs/api-0.3-additions.md` |
| The eight-radio load through the real engine | `tools/multi_radio_load/engine_main.cpp` (`multi_radio_load_engine`, built by name) |

### The mechanism: a selected slot, not a parameter (deviation from "convert to a named slot")

The brief asked for the receiver-scoped code paths to be converted from the slot-0 aliases to a named slot. Passing `Receiver&` to 110 functions would have changed every signature, every caller,
every test and every merge. Instead every Engine function names its radio through `rx()`: the slot the calling thread selected with a `SlotScope`, or slot 0 when none is. The selection is per thread
and per engine, restored when the scope ends, and never carried across a thread (a worker a slot starts hands its answer back in its own result and the pass applies it under that slot's scope).
Choosing WHICH radio happens in few places, each explicit:

* `applyCommand`: the radio is the sender's focus (`commandFocus_`, set by the API host for the length of the apply) or the `target`; a target of ALL radios (RUN alone) applies the command to each
  radio in order. The handler then runs under a `RadioWork` for that slot.
* `pumpExtraReceivers` / `publishExtraReceivers`: each added radio's own housekeeping and publish, in its own scope; the automatic squelch frame per radio; `scannerFrame` on the radio the one scanner is
  bound to; the shutdown steps (`forEachReceiver`); `rebuildDeviceLedger` (always the first radio's books); `radioStateWord` and the facts.

`RadioWork` also swaps the engine-wide mirrors that are one radio's (`Receiver::*_wide_`): the Source list's selected row, the I/Q file's path, the two saved-config device spellings, the bias tee
panel, the gains kept before auto gain, the crystal correction last applied. A command for radio 3 therefore writes radio 3's, and the first radio's are put back when it ends.

## Decisions and deviations from the design

1. **Selected slot, not a named parameter** (above). The 106 reference declarations stay in Engine, bound to slot 0, for the friends and tests that name them.
2. **RADIO_AUDIO and RADIO_SOLO are not target-scoped ops.** They carry the radio in `ival[0]` (like RADIO_FOCUS, RADIO_REMOVE), so `radio_scope.hpp`'s list of 47 receiver-scoped ops and its test are unchanged.
   RADIO_ADD takes `ival[0]` = number (0 = lowest free), `ival[1]` = start running, `text` = device id or "generator", `num[0]` = a test radio's tone.
3. **Test radios are an ordinary RADIO_ADD** (`text` "" or "generator", any session with the SETTINGS grant), not a test-hook-gated source: the generator is already a Source row. Each radio has its own
   generator inside its own pipeline, so two radios can never hear each other's tone. The default tone is 25 kHz times the radio's number.
4. **An added radio starts stopped** unless `ival[1]` = 1; RUN with the target ALL starts or stops every radio.
5. **Audio.** Each radio keeps its own `AudioOut` (a Pipeline opens the system default output when it is built); an added radio is opened on the first radio's device only if it is not already playing on it.
   Route and solo live in the sink as ramped gains (kRouteRampFrames = 480, 10 ms): the route is the user's choice per radio, the solo is a silence held on every OTHER radio that never touches a route or a mute, so
   un-soloing restores exactly. Removing the solo radio clears the solo. A mono sink plays left and right as both. No in-process mixer (design section 6).
6. **Per-radio spectrum, audio and scope reads** are in the API host (a ring and a scratch frame per radio, created by the pass, never freed before the host; retired when the slot empties so a read shows nothing of a
   removed radio and a re-added one continues the sequence upward). `read_state`, `read_spectrum`, `read_audio` and `read_scope` follow the session's focus.
7. **The seq groups of a radio other than the first carry its number in the high bits** (`(n - 1) << 48`). Each radio's snapshot counts its own seq groups, so a focus change from radio 1 to radio 2 could otherwise
   land on a number the client had already read for radio 1 and not redraw (the first run of the focus-bump test caught it on `listSeq`).
8. **A latent assumption of stage 3 fixed here.** `Pipeline::getLatestFrame(out)` answers "a frame newer than the one I hold", and each pipeline counts its frames from 1: one scratch frame shared by all radios (the first
   draft) starved every radio whose counter was behind another's, and a radio re-added into a slot never published. The pass keeps a scratch frame per slot and clears it when the slot empties.
9. **The `fact.radio.<n>.plugins`, decoder plugins and the plugin runner stay on radio 1** (stage 6): an added radio's state shows no decoders; `pluginRunner_.retune` and `audioOutputTunePending_` are the first radio's only.
10. **The single-radio LISTS (GAINS, ANTENNAS, SAMPLE_RATES, DECODERS) still describe radio 1**: they are one block built by the pass; per-radio blocks are the screen's to ask for. The brief's per-radio reads
    (`read_radio_state`, `read_radio_spectrum`, `fact.radio.*`) are complete.
11. **A patch that names a device held by radio 2 to 8 is refused naming it**, not lent. The design's lending of a radio from slot 1 to 3 to the patch ("lent to the patch") is not built; only radio 1's loan exists, as before.
12. **Thread names "dsp.N" / "source.N" were not changed**: the beat registry names a thread by kind, not by instance, and every radio registers its own. A wedge report from radio 3's DSP thread says "dsp". The
    minute line of the ring says `[radio N]` once there are several radios; the `ringdrop` issue word carries no radio number; the bundle's ring counters and figures are the first radio's.
13. **The transmitter key ends only on the first radio's stop**; which radio transmits remains the Quadrant's open decision. The scanner and the airband monitor: one scanner bound to the radio it was started on
    (SCANNER_RUN on another radio moves it), the monitor is the first radio's, and an added radio's automatic squelch waits while the monitor listens.
14. **Tests edited** (the brief says no existing test source edited for radio 1; these changed for the reasons given, nothing weakened): `tests/test_apply_command.cpp` gained `radioOps` (its own rule: every implemented op is
    covered); `tests/test_audio_lead_app.cpp` (four search strings), `tests/test_dsp_drop_minute_app.cpp` (one) and `tests/test_health_paths.cpp` (three) read the engine's SOURCE TEXT and look for exact spellings
    such as `pipeline_.ringDroppedSamples()`; the mechanical conversion made those `rx().pipeline_...`, so the expected strings follow the rename. `tests/engine_api_fake_radio.hpp` gained `fakeRadioThrows()` (a switch,
    off in every other test).

## The per-op re-read (the 47 receiver-scoped ops of `radio_scope.hpp`)

Each handler's case in `Engine::applyCommand` and every function it calls was read for what it touches that is NOT `rx()`. "Only rx()" means nothing else was found; the engine-wide members found are listed with what was done.

| Ops | Acts on the targeted radio through | Engine-wide things found, and what was done |
|---|---|---|
| RUN (start, stop, ALL) | `startReceiver` / `stopReceiver`: its pipeline, takes, fault latch | the transmitter key (`api_->endKeyFromEngine`) now ends only on the first radio's stop (guard); `testFault_` and the audio-lock test seam are the engine's test seams; ALL = each radio in order |
| SET_CENTRE, SET_FREQUENCY, SET_VFO_OFFSET, STEP_TUNE, VFO_TO_ABSOLUTE, SET_VFO_OFFSET_FREE | pipeline, device, retune coalescer, mirrors | `scanner_` user-tune baseline and the "tune immediately" rule now apply only to the scanner's own radio; `audioOutputTunePending_` and `pluginRunner_.retune` first radio only; the band plan (shared by design) |
| SET_MODE, SET_BANDWIDTH, BANDWIDTH_STEP / DRAG / DRAG_END, SET_SQUELCH, SET_SQUELCH_AUTO, SET_VOLUME, SET_MUTED, SET_DEEMPHASIS, SET_STEREO, SET_NR, NR_STRENGTH, SET_NOTCH, NOTCH_FREQUENCY / Q, SET_AUTO_NOTCH, SET_DISPLAY_RANGE / MIN / MAX | only rx() | none; the automatic squelch frame now runs for each radio |
| SELECT_SOURCE | `selectSourceById` and the open worker | the Source row, I/Q path and config spellings are swapped per radio; the DEVICE LEDGER is asked first (a device another radio holds is refused by name); the sound card ids are refused on a radio other than the first |
| SET_SAMPLE_RATE, SET_GAIN, GAIN_NO_READBACK, SET_DEVICE_AGC, SET_ANTENNA, SET_DEVICE_OPTION | its device and mirrors | the keyed per-device memories (`autoGainMemory_`, `manualGainMemory_`, `airspyMemory_`) are keyed by device, so another radio never reads them; `manualGainsBeforeAgc_` swapped per radio |
| SET_BIAS_TEE, SET_PPM, AIRSPY_DECIMATION / GAIN_MODE / AGC, SOUNDCARD_IQ_CENTRE | its device | the bias tee panel, the crystal correction last applied and the Airspy list are swapped per radio; the sound card is the first radio's (refused elsewhere: a radio that is not on a card is refused already) |
| RECORD_IQ, RECORD_AUDIO | its two recorders | `recordDir_` is one folder for every take; the file prefix names the radio once there are several |
| BOOKMARK_TUNE | `tuneToBookmark` on its pipeline | the bookmark list is shared by design |
| SCANNER_RUN, SCANNER_SKIP, SCANNER_CONFIG, SCANNER_RANGE | the scanner is bound to the radio it was started on (`scannerSlot_`) | one scanner: SCANNER_RUN on another radio stops and moves it; other radios' states show it idle |

New ops: RADIO_ADD, RADIO_REMOVE, RADIO_AUDIO, RADIO_SOLO act on the slot table, the ledger and the sinks (not on one radio's handler), under the engine's own locks; their cases are covered in `test_apply_command`.

### Aliases left, and what stayed in Engine though it is one radio's

* The 106 same-named references in Engine remain (bound to slot 0) for the friends and the tests; inside Engine functions every use is `rx().<name>`.
* One radio's but left in Engine, with how it is treated: `sourceSel_`, `iqOpenPath_`, `cfgSoapyArgs_`, `cfgNativeArgs_`, `biasTeePanel_`, `manualGainsBeforeAgc_`, `ppmRadioApplied_` (swapped per radio by
  `RadioWork`); `converterHeldAir_` (keyed by device); `lastRefusedRequestHz_` (a log de-duplication); `soundCard_*`, `plutoUri_`, `rtlTcpAddr_`, `deviceIndex_`/`devices_` (the Source and Sinks panels' shared lists: the first
  radio's, and the output list every radio's sink opens against); the airband monitor and `pluginRunner_` (radio 1 until stage 6); `testFault_` and the other test seams (radio 1).

## Measured

`multi_radio_load_engine 8 300` (eight test radios on paced generators at 2 MS/s, WFM, volume zero, through the real engine: control thread, API host, per-radio publish and housekeeping, a real audio stream per
radio on the default output): **zero ring drops on every radio, zero audio underruns on every radio, 603 380 000 samples read by each (2.000 MS/s), no fault; process CPU 0.37 core on average = 1.16% of this
machine (32 logical processors; peak ten-second sample 0.81 core), working set 92 MiB, 30 to 37 threads.** The stage-0 bare-pipeline figure for eight radios was 0.25 core and 117 MiB; the engine adds its control
thread, API host and per-radio publish and housekeeping. (The same run, before commit 950f27a1, showed 368 underruns on each of radios 2 to 8: see that commit.) This machine, one fast desktop; no slow machine was measured.

## Proofs

* Build: `cmake --build D:/foxsdr-build/mr-engine1 --config Release --parallel` exit 0 (log line `BUILD_DONE_0`).
* `ctest --test-dir D:/foxsdr-build/mr-engine1 -C Release -L "headless|slint" -j4 --timeout 300` on the stage's tip (950f27a1, plus this record): `99% tests passed, 7 tests failed out of 748` (1316 s). All seven are known or
  explained: the five that find the source tree from the working directory (`test_airband_data`, `test_band_plan`, `test_converter_call_sites`, `test_page_resize_id`, `test_png_decode`) pass from the worktree root (115, 5174, 5, 22,
  49 checks, 0 failed: `envtests.sh`); `test_engine_api_merge` failed at 35 s in the gate and passes alone (19.7 s); `test_engine_api_transmit` is a randomised ticket test whose own threshold (`issued.size() > 1000`)
  is borderline on this machine: it printed 1105, 963, 1047 and 537 new tickets in four runs of this tree (pass, fail, pass, fail) and 1049, 785 and 1040 in three runs of the stage-3 binary built in the screen lane's build folder
  (pass, fail, pass), so it fails about one run in two with or without this stage. The FIRST gate run (before the source-text expectations were updated, see deviation 14) failed 15: those five, the transmit test,
  `test_apply_command` (x3: the four new ops uncovered), `test_audio_lead_app` (x2), `test_dsp_drop_minute_app` (x2), `test_health_paths` (the source-text tests) and `test_stop_ends_recordings_free` - which was a real
  finding: the test holds every caller of `stopReceiver` and every `pipeline_.stop()` to a list, and the first draft of RADIO_REMOVE and of `teardown` added two. RADIO_REMOVE now stops through `applyCommand` (RUN 0) and
  the teardown loop was dropped; the test is unchanged and passes.
* `ctest -L gui` on the hidden desktop (`FOXSDR_FRAME_CAP_HZ=60`, desktop `FoxSDRTests-mr1`, serial, 4376 s): `99% tests passed, 1 tests failed out of 225`; the one failure is `test_health_app` (failed at 43 s on checks
  1623, 1635 and 1641 about the health ledger's counts after failed radio opens); run alone on the hidden desktop straight after it passes (31.8 s). Cause unproven: other lanes' gates were running on this machine at
  the same time and several window tests use fixed folders under the temp directory; the radio-open failure path itself (`finishDeviceOpen`) was not changed by this stage, only its caller's polling was split into `pollDeviceOpen`.
  The Slint drive scripts (149 `drive` tests) passed unchanged.
* New tests: `test_engine_api_multi_radio` (329 checks), `test_audio_route` (2944 checks); each run 20 times in a loop together with `test_engine_api_radios` and `test_device_ledger`: 20 of 20 green, no failure line.
* Break-it checks (each mutation applied, the test built and run, red seen, the mutation reverted with `git checkout`):

| Mutation | What turned red |
|---|---|
| the command's radio ignored (every handler runs on slot 0) | section C: radio 2's volume, mode, squelch, tune and seq groups never moved and radio 1's did (24 checks), plus the recorder, ALL-radios and stop sections that aim at a radio |
| ledger: the claim is always granted (RADIO_ADD) and SELECT_SOURCE skips the holder check | the by-name refusals (both sides) and the limit (ninth radio and patch radios): 20 checks in sections D |
| patch Radio nodes not claimed on the ledger | "patch radios count in the limit": 5 checks |
| no tone set on a test radio | each radio's tone at its own bin (B) and the re-added radio's tone (E) |
| extra radios' frames published into radio 1's spectrum ring | radio 2 and 3 read no spectrum of their own (B), the test timed out |
| the extras' publish removed from the pass | radio 2's state never appears (B, C, ...) |
| the extras' housekeeping removed from the pass | the recording flag of radio 2, the device open of radio 3 and the fault test (J) |
| the solo silence never set on a sink | the three solo checks (F) |
| the solo not cleared when its radio is removed | `soloRadio` stays 3 (F) |
| the take-file prefix removed | the three per-radio file counts (G) |
| RUN with ALL radios applies to the first only | start-together and the thread count (H) |
| the spectrum ring not retired on removal | the re-added radio shows nothing new / the old tone (E) |
| the seq tag of a radio's number removed | `listSeq` of the focus-bump check (C) |
| the route ramp replaced by a jump (`AudioOut`) | the ramp checks of `test_audio_route` (start level, midpoint) |

## What is unverified

* No real radio was used (stage 7): every radio but the first in the tests is a generator or the fake radio. Two real radios at once, the B200 and the RTL-SDR, are untested; several RSPs on one SDRplay session stay unproven.
* The eight-radio figure is this machine's, from generators; the audio streams are to the default output at volume zero. WASAPI shared mode was not exercised (the sink's default host API is used).
* Sound was never listened to: route and solo are proven in the sink's callback arithmetic and in the engine's state, not through speakers.
* Rapid add / remove storms are proven by the 20-run loop and a storm test, not by ThreadSanitizer (no sanitizer build was made here).
* The window (classic and Slint) was not changed and was not driven with a second radio; the drive scripts ran unchanged.
* The lists of the engine API (GAINS and the others) still follow radio 1.

## What the later stages must know

* **Stage 5 (saved settings).** Per-radio data now lives in `Receiver` (including `audioRoute_`, `claimedKey_`, `testToneOffsetHz_`) and in the `*_wide_` swap fields; the engine-wide config mirrors (`cfgSoapyArgs_`,
  `iqOpenPath_`, the bias tee panel's `remembered` map, `manualGainMemory_` ...) are written for radio 1 only. Anything written inside a `RadioWork` for a radio other than the first has to be read out of that radio's
  `Receiver` before the scope ends. `RADIO_ADD` is the restore path: `text` + `ival` + tone, then the ordinary ops aimed at the new radio.
* **Stage 6 (plugins per radio).** The places that are the first radio's by `scopedSlot() == 0u` or by name: `fillPublishedState` (decoder counts and the DEC lamp), `applyRetuneNow`'s `pluginRunner_.retune`,
  `audioOutputTunePending_`, `pipeline_.setPluginRunner` (at plugin load, radio 1), `updateAudioMute` and the mute-by-preset state, `squelchAutoFrame` while the airband monitor listens.
* **The screen.** Read each pane with `read_radio_state(n)` / `read_radio_spectrum(n)` / `fact.radio.<n>.*`; the seq groups of radio n carry `(n - 1) << 48`. `RADIO_AUDIO` / `RADIO_SOLO` are the speaker controls; a
  radio that failed to open stays on the generator with `fact.radio.<n>.refusal` set. After a merge with the AGC and noise-blanker lane, run `tools/multi_radio_slot_rewrite.py` over `engine.cpp`, `engine_*.cpp` and
  `engine.hpp`, build, and add the new Receiver fields to nothing else: their handlers then act on the targeted radio.
