# Engine wiring, second session (lane W2) - branch feat/engine-wire-2

Cut from `feat/engine-wire` tip 2b2ede72 on 2026-11-12 02:12. Worktree `C:\Users\steve\foxsdr-wt\mr-engine1`, build `D:/foxsdr-build/mr-engine1`.
This record is written in the order the work was done; the last section is the final state.

## 0. First job: the `test_engine_api_merge` access violation (fixed, commit 1b0d69a0)

* **Measured, not guessed.** The test binary was run three times at once under a small debugger launcher (scratch tool, outside the repo) that prints a
  symbolised stack at the first access violation. Three of three runs faulted: `Pa_Terminate+0x64` (`third_party/portaudio/src/common/pa_front.c:396`,
  the `CloseOpenStreams` call) twice and `Pa_CloseStream+0x48` (`pa_front.c:1379`, `RemoveOpenStream`) once, reading `0xFFFFFFFFFFFFFFFF` and `0x17`,
  called from `cascade::sink::paTerminateShared` (`src/sink/pa_init.cpp:59`). The two Windows application-log entries for the W1 binary (offset 0x3d8d94)
  and for the other lane's build (`en-packaging`, offset 0x3a69a4) resolve, through the PDB, to the same `Pa_Terminate+0x64`.
* **Cause.** PortAudio keeps its open streams in one linked list with no lock. `PaStreamListGuard` is meant to serialise the two writers
  (`Pa_OpenStream` adds as its last act, `Pa_CloseStream` removes as its first), but `AudioOut` and `AudioIn` take it with `NoWait`, and a `NoWait` that
  finds it busy goes ahead without it. The test runs 24 engines at once, each opening and closing the default output on a worker, so two writers changed
  the list together, lost nodes and left one pointing at a freed stream, which the last `Pa_Terminate` then closed.
* **Classification.** Not W1's change: `src/sink`, `third_party/portaudio` and `src/engine/pipeline.cpp` are identical from `engine/multi-radio` d3630d78 to
  W1's tip, and `feat/packaging` (d3630d78 plus six commits that touch none of them) crashed at the same offset. UNVERIFIED on a d3630d78 binary itself: no
  build of that exact commit exists (the `mr-dsp` binary of 11:02 on 2026-11-11 predates six commits and passed three runs of three at once, so the trigger
  is timing-dependent; the defect is the unguarded list).
* **Fix.** A small lock inside the vendored PortAudio around the list operation only (`SRWLOCK` on Windows, a static `pthread_mutex_t` elsewhere; never held
  across a host API's open or close, so a hung driver cannot hold it); fenced `FOXSDR PATCH open-stream-list-lock` and recorded in
  `third_party/portaudio/FOXSDR-PATCHES.md`.
* **Test.** `tests/test_pa_open_list_race.cpp`: (L) holds the list lock by hand and requires an open and a close on other threads to wait for it; plus a
  12-thread, 1,440-open stress with no guard that requires the list to hold exactly what is open. The race itself is too narrow to catch on demand (12 threads
  x 1,440 opens in a quiet run changed nothing: the test said so in its first, red-free version), hence (L).
* **Break-it.** Lock macros emptied: `the open waited: 0, the close waited: 0`, `test_pa_open_list_race: 11 checks, 2 failed` (exit 1). Restored:
  `11 checks, 0 failed` twice.
* **Proof of the fix.** `test_engine_api_merge` three at once: no access violation in three of three (before: three of three). Alone through ctest:
  `Passed 44.59 sec`. `test_audio_in`, `test_audio_out`, `test_pa_stream_list_guard`, `test_portaudio_alsa_periods`, `test_soundcard_source`,
  `test_soundcard_app_paths` pass.
* **Residual.** One of the three concurrent runs ended with `1 differ` (`nothing yet | A:RUN0 B:LATCH1 A:RUN0`, merged 293 ms against one-a-pass 274 ms, both inside
  the 900 ms window the test judges time by, so not excused as a stall): a separate, rare difference in the merge sweep (1 in 5,450 cases, 1 run of 3
  concurrent runs, 0 of 1 run alone). NOT investigated; listed under "Not done".

## 1. Merges (each `git merge --no-ff`, in the brief's order)

| # | Branch (tip) | Merge commit | Conflicts |
|---|---|---|---|
| 1 | feat/recording-models (0754bfdc) | f841cb4a | none |
| 2 | feat/core-formats (91730327) | fd9c1bb3 | none |
| 3 | feat/net-map-models (428831c0) | bc13a0de | none |
| 4 | feat/net-outputs (09b81e60) | 9fa56572 | `tests/thread_inventory_rows.inc`: both sides appended to the src/net block; kept all four rows in file-name order |
| 5 | feat/radio-sources (5765d6b7) | 017cdde0 | `tests/test_health_paths.cpp` (the ReaderLeak table): HEAD's kiwisdr_source row and the branch's net_sample_source row, both kept |
| 6 | feat/array-maths (efd72f1b) | eb4d841b | none |
| 7 | feat/unblocked-desk (d24c3a5c) | 91cd50a1 | none |
| 8 | feat/platform (da606fea) | 99caf9d5 | none |
| 9 | feat/knob-input (c86bb870) | NOT MERGED | its record: "The input layer lives on the Slint host side, not in the engine (the engine has no key table; a headless engine has no use for a knob)"; `plugin-or-host.md` row U11a places it as the plugin `controllers`. Left as a model. |
| 10 | feat/slint-gaps (b9e22d3f) | 02a75528 | none |

After the merges: configure + full Release build, `build exit=0` (26 minutes, Slint window included). The 54 tests the merges added all pass except
`test_thread_inventory` (five new files that start a thread had no row): fixed in 857eeb1c (rows say NOT COVERED by a heartbeat; each wiring that starts one
gives it a ThreadSpec and removes the exemption). `ctest -R` of the 54 + `test_thread_inventory` + `test_health_paths`: 55 tests, 55 passed after that commit
(`test_thread_inventory` re-run alone: Passed).

Screen branches B, C and S1 are NOT merged here (the `engine-merge` lane does that); their engine hand-offs are in section 2, rows C1-C9.

## 2. Step 0 table: every item, with its state

State: **in branch** = the model is merged and tested but nothing in the engine, window or web server calls it (grep of `src/engine src/core src/slint src/gui
src/net src/main*.cpp`; every model of the ten branches came back "not referenced"); **wire now** = engine-side work this lane does; **screen** = needs a `.slint`
page or a key (a screen lane, or this lane if time allows); **decision** = owner or orchestrator; **blocked** = see the reason. Sizes: S under an hour, M a few
hours, L a day or more. Source: four read-only surveys of the merged tree (opened files, not memory), condensed here.

### 2.1 recording-models (merged: in branch)
| Part | State | What the wiring must change | Size |
|---|---|---|---|
| UTC clock (`status_clock`) | screen | status-bar text from the window's one-second timer; setting in AppConfig | S |
| CLI options (`cli_options`) | wire now | `process_boot.cpp parseFlags` refuses unknown arguments in both programs: add a cli branch (the record's pass-through list is incomplete: `--seconds --safe-plugins --selftest --version --web-* --cat-* --diag-* --update-check --record-check --rds-check`...); start-up ops run once after the engine is up | M |
| Naming templates + rollover (`rec_naming`, `civil_time`) | wire now | template layer ahead of `Recorder::prepare` (`recorder.cpp:130-148`; `Recorder::makeFilename` is pinned by tests, keep as the default path). The record's "reuses a name within the same second" is STALE: `openFile` already uses `createUnique` | M |
| Scanner priority/lockouts/log/mute (`scan_aids`) | wire now | hooks exist in `Engine::scannerFrame` (`scanner_.skip()` exists); the "user wins" check (`scannerExpectedAbsHz_`) must be re-armed on a priority retune; mute setter UNVERIFIED | M |
| Band stacking, VFO A/B + split, lock, mode memory (`operating_aids`) | wire now (receive side only) | `TuneLock` needs a `TuneSource` threaded through `Engine::tuneAbsoluteHz` callers; split = transmit-adjacent: receive A/B only (decision) | L |
| FLAC writer | wire now | second writer beside `Recorder` (it is a stdio WAV writer): `startAudioRecording` picks `flac::AsyncRecorder`; ops `SET_RECORD_FORMAT`, `SET_RECORD_BITS` + name rows + scoping list + coverage test | M-L |
| IF recording (`if_recorder`) | wire now | tap after `vfo_.process` (`pipeline.cpp:2179`), `setChannelRecorder`, `startIfRecording`; own writer (not a `Recorder` kind) | M |
| Recordings browser (`rec_browser`) | wire now + screen | replaces `core/patch_recordings.*` folder logic; ops `SCAN_RECORDINGS`, `OPEN_RECORDING`; `IqFileSource` opens only PCM16/float32 WAV, so OPEN needs Part 5 or widened IqFileSource | M-L |
| Seek / A-B loop (`iq_playback`) | wire now | `PlaybackModel` is not an `IqSource`: adapter installed through `Engine::installOpenedIqFile` | M-L |
| One file per transmission (`clip_recorder`) | blocked on `replay_ring` wiring | needs the replay ring fed from the audio path and `ClipTrigger::update` once per pass | L |

### 2.2 core-formats (merged: in branch)
| Part | State | What the wiring must change | Size |
|---|---|---|---|
| DSP benchmark | wire now | `--benchmark` tool mode beside `--selftest` (`process_boot.cpp`, `tool_modes.cpp`) | S |
| gqrx/CubicSDR import | wire now | dispatch in `core/freq_import.cpp` (`importFrequencyList`); fold the result type; existing op and worker; formats UNVERIFIED (no real files) | S |
| Macros | wire now | `pumpMacros` modelled on `pumpSchedule`, steps go through `Engine::applyCommand` under the caller's grants (UNVERIFIED how grants are enforced), denied-prefix list rechecked | M |
| RF64 / SigMF writers | wire now (after naming) | a second sink in the recorder or a queue worker; SigMF `addCapture` from the retune path | M-L |
| Decode database, alert rules | decision (privacy) | rides `Engine::pumpDecoderOutput` (lines only, no frequency or kind today); text to disk needs the plugin PRIVATE flag first (section 12 of the brief) | M each |
| KML/GeoJSON export | blocked on track store wiring | needs a track history source | M-L |

### 2.3 net-map-models (merged: in branch)
| Part | State | What the wiring must change | Size |
|---|---|---|---|
| Metrics page | wire now, in `web_server.cpp` (feat/web-secure not merged here: LISTED) | `/metrics` route with the web auth rule; per-plugin decode tally | M |
| Webhooks (HTTPS stays refused) | decision (credential-free, cleartext http by user URL) | `ServerHost` listener-pattern key `webhook`; needs an event source | M |
| MCP server (never transmits) | wire now | new `InternalKind::Mcp` session with ops exactly `{SET_CENTRE, SET_MODE}` + VIEW grant | L |
| Coverage grid, track store | wire now | one shared feed point in `Engine::fillStatusLists` (window-fed tracks: empty headless); retention setting | M each |
| RF position log | wire now | `Engine::pollGpsReader` calls `addFix`; location history on disk: owner decision on retention | M |
| Tile cache | blocked | host sees decoded tiles only; needs an ABI fetch call | L |
| Sky positions, lookup links, locators, antenna calculators | screen | pages calling the models | S each |

### 2.4 net-outputs (merged: in branch)
| Part | State | What the wiring must change | Size |
|---|---|---|---|
| Network I/Q out | wire now | `Pipeline` setter mirroring `setIqRecorder` (`pipeline.cpp:1743`); `submit` at the raw-band point; one `ServerHost` listener-pattern key; off by default, loopback | M |
| SBS out | wire now | builds from polled tracks in the engine pass | M |
| Beast out | blocked | needs frames from a plugin (ABI call) | - |
| AIS sentence out | blocked | needs raw message bits from the AIS plugin (ABI call) | - |
| APRS-IS client | decision recorded: ONE path | the plugin `aprs-decoder` 1.1.0 already has APRS-IS receive and iGate (its own client, `,qAO,` at `aprs_is.h:386`); nothing in this repo needs the core client; the core client stays an unwired model and enforces `qAO` at `src/net/aprs_is.cpp:274` (never `qAR`). Wiring both would run two iGates. | - |

### 2.5 radio-sources (merged: in branch)
| Part | State | What the wiring must change | Size |
|---|---|---|---|
| SoapyRemote, SDRconnect | wire now | source-registration pattern (`kXDriverKey`, `makeDeviceSource`, `openXAt`, SELECT_SOURCE dispatch + exclusion list, second-radio key, REMOTE-denied list at `fox_engine_api.cpp:2010`); telemetry word first | M each |
| SDRplay low-IF plans | wire now (UNPROVEN without hardware) | `SdrPlaySource` mode member; replace two `sdrPlayRatePlan` calls | M |
| SDRplay diagnostic report, service restart (SCM first) | screen | front-end only | S each |
| Kenwood/Yaesu CAT follower | wire now | worker thread + row; overlap with `net/cat_emulation` noted | L |
| HPSDR | blocked | no frame decoder (frame layout not in the allowed document); do NOT register a source that cannot receive | - |
| RSPduo | blocked | empty rate table; needs the document and the multi-radio engine | - |

### 2.6 array-maths, unblocked-desk, platform (merged: in branch); knob-input (not merged)
| Part | State | What the wiring must change | Size |
|---|---|---|---|
| E4000 offset tuning | wire now | `SET_DEVICE_OPTION` pattern (`withOffsetTuning` beside `withDabNotch`, `engine.cpp:308`), no new op | S |
| W12 driver allow-list | wire now | gate the `installTarget` in `radio_setup.cpp:445-449`; `winusb_install.cpp` already refuses through `decide` | S |
| confirmStartup | wire now (Linux only) | one call in `bootProcess` after the tool-mode step | S |
| W18 WebSDR link-out | blocked on data | needs the owner's WebSDR address list; glue is S | S |
| OIRT stereo | wire now | `unique_ptr<OirtStereo>` beside `stereo_` in `pipeline.cpp`, one setting, one op, a fact | M |
| N26 phone web | listed (`web_server.cpp`, feat/web-secure not merged): CSP needs `manifest-src 'self'`, ungated manifest + 192/512 px icon routes, icons to be made | M |
| File drop | wire now | replaces the working slint-gaps drop (first file only, by extension) with content-based multi-file drop: a behaviour change, UI only | M |
| Discord presence | wire now (off until an ID is given) | settings `discord_client_id`, `discord_show_frequency`; one object owned by the engine | M |
| CAT emulation (receive only) | wire now | new listeners beside `reconcileCat` in `server_host.cpp`; TX commands answer `E;`/`?;`; port must differ from 4532 | L |
| ExtIO helper | wire now | `DeviceSource` adapter over `ExtioSource`; deny `open-extio:` to REMOTE sessions; helper build lines in the root CMake and installer | L |
| Linux update check, Install USB permissions | wire now (Linux code stays unproven) | `LiveWorld::updateCheckStart` Linux branch; a `SetupKey` + handler | L / M |
| Combiner, DF maths | blocked | no multi-channel coherent source in the tree | L |
| knob-input | not merged | host-layer, not an engine need (see section 1) | - |

### 2.7 slint-gaps and screen-lane engine hand-offs
slint-gaps is merged (02a75528). Its engine hand-offs: an engine-side frequency lock (`RADIO_LOCK` op + `fact.radio.n.locked`), a recording "closing" state,
and a plugin-setting-edit answer that does not clear its pending state (cause not found). Screen-b/-c/-s1 hand-offs (they are not in this tree; the
merge lane merges them):

| # | Item | State | Notes |
|---|---|---|---|
| C1 | Scanner extras (locked/priority/threshold/mute params; `scanner_.tick` signature) | after the screen-c merge | the proposed op number `FOXAPP_OP_SCANNER_EXTRAS 0x860C` COLLIDES with this tree (0x860C-0x8613 are SCHEDULE_* and CAL_*); screen-c's own 0x860A/0x860B are free here: renumber at merge |
| C2 | Bookmark carries squelch, threshold, offset, tone | after the screen-c merge | `core/bookmark.hpp`, `freq_manager.cpp`, `FoxListItem.value[4..7]`, new op |
| C3 | Frequency / delta lock in the engine | wire now (S-M) | plugin, web and CAT can still change a delta-locked VFO offset |
| C5 | Apply bookmark tone through `SET_TONE_SQUELCH` (0x0113) | after C2 | S |
| C6 | Tell a plugin the symbol rate | ABI (level 3) | M |
| C7 | Separate plugin mute from the user mute flag | wire now (S) | |
| C8 | Per-radio tracked frequency in Hz, SNR tap + history ring (7cc18410, merged, unwired), core CSV power logger (b103a510), Display persistence fields | wire now (M) | |
| C9 | T13 spoken announcements, U11b controllers | plugins (AD3) | |

### 2.8 W1 leftovers (brief item 11)
| # | Item | State | Size |
|---|---|---|---|
| A1 | Synchronous AM (`dsp/sam.hpp`) | wire now (touches every mode-counting test) | L |
| A2/A3/A7 | IF shift (`passband.hpp`), CW pass band centred on the pitch (`cw_filter.hpp`), CW peak filter | wire now | L / M / M |
| A4 | Spectrum block: FFT size to 131072, window, frame rate | wire now (frame type carries a variable bin count) | L |
| A5 | AFC (`dsp/afc.hpp`) | wire now | M |
| A6 | I/Q correction (no dsp file of that name; UNVERIFIED) | wire now | M |
| A8 | Stereo placement per channel | wire now | M |
| A9 | Symbol tap, zoom FFT, SNR tap | wire now (SNR tap with C8) | M each |
| A10 | Marker and bookmark cap 256 with a `markerLimit` fact, visible span to plugins | wire now (ABI append) | M |
| A11 | EVENTS interface (AD3) | brief item 13 | M-L |
| A12 | MQTT publisher, rotator, decode webhook | decision (credentials, privacy) | M each |
| A13 | Power and SNR logging, I/Q readers behind `IqFileSource::open` | wire now | M each |
| A14 | Replay ring, clips, listening log / ADIF | blocked on the audio-chain tap | L |
| A15 | W1 SCREEN HAND-OFF controls | screen only (KiwiSDR, tone squelch, S-meter units, NR method, notches, CW pitch, noise squelch, equaliser) | L |
| A16 | Telemetry word `kiwisdr`, `kiwisdr=` in the log address scrubber | wire now (S) | S |

### 2.9 Brief items 12 to 16 (host and ABI)
| Item | State | Notes |
|---|---|---|
| 12 Private-plugin flag + host text sinks | wire now | descriptor flag; web mirror ("decoded", "tracks") and Copy diagnostics skip private data; tests per rule. PASSWORD at rest: NOT decided by the owner: no new credential storage, present behaviour stated in the final section |
| 13 Host event feed (AD3) | wire now, small | append to `plugin_abi.h` (`CascadeHostApi` ends at `set_schedule_hint`/`clear_schedule_hints`; host API level is 2 and no level-3 text exists yet); `docs/PLUGIN-API.md` section; plugin-loading tests |
| 14 Depth-lane host rows | ABI-only first | B1 alert sound, B2 post-squelch capture, B4/B5 `CascadeTrackInfo` fields, B6 airline lookup, B7 waterfall click (new capability bit 0x20000), B8 RTTY_SCOPE (kind 9), B9 NFM 7 kHz (measure first), B12 `start_capture`, B13 marker level, B14 choice-list setting kind; already done in the host: B15 (`STATE_IMAGE_INPUT`), B18 (estimated tracks), B17/B19 plugin-side; screen: B3, B10, B16, B20-B22 |
| 15 `mbe_voice` | wire if a host change is named | read the voice record first |
| 16 Serial rows of `remaining/INDEX.md` | wire now / listed | R5 R10 R11 R18 R19 D4-D7 F6-F16 F20 S1-S3 S12 T4 T6 T7 P4 P7 P15; the web-page rows are listed because `feat/web-secure` has not merged |

Plugin ABI facts for the work above: `CASCADE_PLUGIN_ABI_VERSION` 3, `CASCADE_HOST_API_LEVEL` 2, last capability bit `TRACK_PHOTO` 0x10000 (next free 0x20000),
growth rule `CASCADE_HOST_COVERS`. The plugins repo has NO byte-compare test of `plugin_abi.h` (only `test_plugin_abi3_compat`, which loads published binaries from
`FOXSDR_PUBLISHED_PLUGINS_DIR` and prints SKIP without it): the "identical copy" check is a manual diff, listed for `plugins-merge-3`.

## 3. What was wired, in the order done (one commit each)

Break-it column: the behaviour removed (or the lock/gate disabled), the result seen RED, then restored and GREEN.

| # | Commit | Item (brief) | What was wired | Proof (decisive result) | Break-it |
|---|---|---|---|---|---|
| 1 | 1b0d69a0 | `test_engine_api_merge` access violation (first job) | lock around PortAudio's open-stream list (section 0) | merge test alone `Passed 44.59 sec`; 3 at once: no access violation (before: 3 of 3); `test_pa_open_list_race` 11 checks | lock removed: `the open waited: 0, the close waited: 0`, 11 checks 2 failed |
| 2 | 857eeb1c | merges (item 1-10) | `tests/thread_inventory_rows.inc`: rows for the 5 merged headers that start a thread | `test_thread_inventory` Passed (before: 5 failed of 11673) | - (the test names the missing rows) |
| 3 | 923b8ccd | telemetry/log list, W1 gap | `kiwisdr=`, `soapyremote=`, `sdrconnect=` in the log address scrubber (`core/diag_log.cpp maskKeyedHosts`) | `test_upload_scrub` Passed; neighbours `test_crash_upload` (hidden desktop), `test_diag_log_lock`, `test_issue_report`, `fuzz_corpus_crash_report` pass | before the change 2 of the 4 new cases failed (`got == want`) |
| 4 | bf330b36 | core-formats 8, DSP benchmark | `--benchmark` tool mode (`process_boot.cpp`, `tool_modes.cpp/.hpp`), kept out of the usage string | `foxsdr-engine.exe --benchmark` -> `benchmark PASS: 21 items, 0 failed`, exit 0; `test_tool_benchmark`, `test_process_boot_kind` pass | dispatch line removed: `b.exit && b.exitCode == 0` fails twice |
| 5 | d583200c | core-formats 4, gqrx/CubicSDR import | `importFrequencyList` recognises both by content and folds the result into `ImportResult` | `test_freq_import_foreign` +31 lines; `test_freq_import`, `_formats`, `fuzz_corpus_freq_import` pass | dispatch removed: 9 of 102 checks fail |
| 6 | c7743ba3 | array-maths/desk R12, E4000 offset tuning | `SET_DEVICE_OPTION "offset_tuning"` on an rtl_tcp radio whose header names an E4000 (`withOffsetTuning`, presence at open, list item, fingerprint) | `test_engine_api_source_kinds` B2: R820T gets nothing; E4000 radio 2 hears 0/1/0; radio 1 lists it off/on/off | dispatch branch off: 6 checks fail (`-6 vs 0`, server never hears 1) |
| 7 | fecdd2d9 | array-maths W12, driver allow-list | `driverpolicy::decide` gates `radio_setup.cpp installTarget` and `winusb_install.cpp isRtlInterfaceZeroId` (the elevated helper's refusal) | `test_radio_setup`, `test_winusb_install`, `test_driver_policy`, `test_radio_setup_inventory` pass | RTL policy row -> NotAllowed: `test_radio_setup` :160 x2, :199 and `test_winusb_install` :41, :44, :47 fail |
| 8 | e2955e1e | platform `confirmStartup` | `confirmStartupFor` (new, pure) called by `bootProcess` step 7b, Linux only, after the tool modes | `test_linux_update` `testConfirmStartupFor` passes | kind gate removed: 6 checks fail (:724/:725 x the five other kinds) |
| 9 | e75e0810 | item 12, private plugins | `CASCADE_FLAG_PRIVATE_DATA` (bit 31, a flag outside `CASCADE_CAP_ALL_KNOWN`); web "decoded", "tracks", served pictures, REMOTE decoder lines, copied diagnostics all skip a private plugin | `test_engine_api_private` P1-P6 (28 checks), `test_plugin_validate`, `test_plugin_api/host`, `test_module_census`, `test_plugins_view`, `test_plugin_abi3_compat`, `test_server_host` pass; `test_engine_api_03` Passed alone 55.65 s after one failure in a parallel batch | all five sinks mutated: 7 checks fail, one group per sink |

Notes on the proofs
* Everything above that touches the Linux-only lines (`confirmLinuxUpdate`) or the Windows installer (W12) is proven through its pure functions only: the Linux lines were not compiled here, and nothing was installed or opened.
* The E4000 and benchmark rows were exercised end to end (stand-in server; the real engine executable), not only by unit tests.
* A slip, found and fixed in the commit that caused it (bf330b36): a plain `boot()` in `test_process_boot_kind` starts the crash sentinel, which is a child of the test's own executable and re-runs the test; six chained copies were stopped by hand (my own processes). A second slip: `test_crash_upload` is a gui test and was run once on the visible desktop; it failed there, passed on the hidden desktop; every gui test since runs on the hidden desktop only.

(Further sections are appended as the remaining items are finished.)
