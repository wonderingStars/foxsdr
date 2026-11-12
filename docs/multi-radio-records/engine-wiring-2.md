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
| 10 | 16913b89 | core-formats 7, macros | `APP_MACRO_RUN / STOP / RELOAD` (0x8614-0x8616): `macros.json` in the per-user folder, played on the control pass through `applyCommand`, the model's deny list plus `APP_MACRO_`; facts `macro.*` | `test_engine_api_macros` M1-M6 (64 checks) | refusal check removed: M3 fails twice; `APP_MACRO_` off the deny list: M4 fails; pump removed: M2 fails 7 times |
| 11 | 8d67f50d | item 13, host event feed (AD3) | `post_event`, `read_events`, `events_seq` appended to `CascadeHostApi`; ring of 1,024 in memory; the host posts every drained decoder line as kind `text`; private events are the plugin's own; `PluginApiCore::hostEvents` for the host | `test_plugin_events` E1-E7 (1,187 checks), `test_engine_api_private` P7; **43 of 43 published plugin binaries load against the grown table** (`test_plugin_abi3_compat` on the newest win-x64 copy of each) | private filter removed: E5 fails 5x; ring bound removed: E4 fails; kind check removed: E3 fails 8x |
| 12 | acdcc9d7 | slint-gaps item 3, engine half of the frequency lock (C3) | `APP_RADIO_LOCK` (0x8617): `Receiver::tuneLocked_`, every tuning command DENIED, `retuneSourceHz` no-op; `fact.radio.n.locked` | `test_engine_api_lock` L1-L6 | op-level refusal and choke point disabled: L2 fails for 7 commands, L4 3 checks |
| 13 | 969f7137 | recording-models 3, file-name template (naming) | `APP_SET_RECORD_NAME` (0x8618): template in `record-name.txt`, validated on SET, expanded at each take's start; `fact.record.nameTemplate` | `test_engine_api_recname` N1-N5: a real take on the generator is `100.1/WFM_2026.wav` | validation off: N1 fails 9x; take path never set: N3 fails |
| 14 | ff1d753e | W1 "not reached" F, marker cap | `CASCADE_MAX_MARKERS_PER_PLUGIN` 64 -> 256, `fact.plugin.markerLimit` | `test_engine_api_markerlimit` K1-K3 | fact removed: K1 fails twice |
| 15 | 2adfa987 | array-maths D3, OIRT stereo | `Pipeline::setStereoSystem`, an `OirtStereo` beside `StereoFm`, `APP_SET_STEREO_SYSTEM` (0x8619; 0 automatic by band, 1 pilot, 2 OIRT), `pollStereoSystem` each pass per radio, `fact.radio.n.stereo.system/decoder` | `test_pipeline_oirt` (generated FM OIRT through a real `Pipeline`): **56.6 dB separation, lamp on, carrier level 1.00**; pilot-tone decoder on the same signal: mono; `test_engine_api_oirt` S1-S6 | pipeline never choosing OIRT: 6 checks fail; band test removed: 7 checks fail |

Notes on the proofs
* Everything above that touches the Linux-only lines (`confirmLinuxUpdate`) or the Windows installer (W12) is proven through its pure functions only: the Linux lines were not compiled here, and nothing was installed or opened.
* The E4000 and benchmark rows were exercised end to end (stand-in server; the real engine executable), not only by unit tests.
* A slip, found and fixed in the commit that caused it (bf330b36): a plain `boot()` in `test_process_boot_kind` starts the crash sentinel, which is a child of the test's own executable and re-runs the test; six chained copies were stopped by hand (my own processes). A second slip: `test_crash_upload` is a gui test and was run once on the visible desktop; it failed there, passed on the hidden desktop; every gui test since runs on the hidden desktop only.

## 3b. Brief items 14 and 15, what the reading found (no code)

* **Item 15, `mbe_voice` integration:** the voice lane's record (`feat/plugins-voice`, `docs/feature-records/voice-expired.md` section 9) names NO host change: decoded voice reaches the host through the existing
  `CASCADE_CAP_AUDIO_OUT` table (`pull(handle, float* interleaved, size_t frames)` and `active(handle)`), at 8,000 Hz and mono. **Verified in this tree** (`core/plugin_host.cpp` lines 760-790): the validator accepts
  `sampleRateHz` from `CASCADE_AUDIO_RATE_MIN_HZ` (1,000) up and `channels` 1 or 2, and the host resamples. The integration is therefore a plugin-side one (the P25 receiver in `dmr-monitor` or a P25 plugin, the M17 plugin);
  the hardware-vocoder packet layer stays off unless the user attaches a device. Nothing was wired in the host.
* **Item 14, row B9 (does the NFM path pass about 7 kHz of discriminator audio unfiltered for 9600 baud packet?):** READ FROM THE CODE, NOT MEASURED. The audio a decoder plugin receives is taken in `Pipeline::process`
  ("DECODER TAP, BEFORE the AGC and the squelch"): the demodulator's output (NFM: the quadrature discriminator followed by the user's one-pole de-emphasis, which is flat when the control says Off) after the channel filter,
  resampled to the decoder rate by `resamplerD_`. No audio low-pass, high-pass, equaliser, notch or noise reduction sits in front of it (those come after, "notch, auto-notch and noise reduction ... every one of them damages data").
  So 9600 baud needs: the **De-emph control Off** (50 or 75 us costs about 10 dB at 7 kHz) and a **channel bandwidth of 20 kHz or more** (the 12.5 kHz default filter clips the sidebands). Whether the TNC plugin's settings
  make that so was not checked, and no 9600 baud signal was generated: UNVERIFIED.

## 4. Gates

* **First-third headless gate** (`ctest -L "headless|slint" -I <slice>`, 100 test numbers a slice, `-j4 --timeout 300`, 883 tests, at the tip before the macros commit): 8 red. Five are the
  known false failures (`test_airband_data`, `test_band_plan`, `test_converter_call_sites`, `test_page_resize_id`, `test_png_decode`), `test_diagnostics_free` is the stale-stamp one (uncommitted
  tree at the time), `test_engine_main_password_not_in_argv` passed alone (7.21 s: load), and **`test_crash_upload_free` fails alone, twice in a row, at `tests/test_crash_upload.cpp:1782`
  (`srv.connections() == 1`)**: the same line fails, with `FOXSDR_PASS=free`, in the test binaries of two OTHER lanes' builds on the base (`mr-dsp` = engine/multi-radio and `en-sysmodels`), so it
  is not this lane's change. Its gui sibling `test_crash_upload` passes on the hidden desktop (22.7 s).
* **First-third GUI gate:** started on the hidden desktop (`FOXSDR_FRAME_CAP_HZ=60`, `-L gui -j3`), 278 tests; 21 had run (all passed) after 10 minutes, a rate that makes the gate about two
  hours, and a build cannot be made while it runs, so it was stopped by hand (my own `ctest`). The GUI gate is therefore run ONCE, at the end, on the final tip (section 8).
* Second-third and final gates: section 8.

## 5. Telemetry, for the Worker (NOT deployed; the standing go-ahead for the Worker is the orchestrator's to use)

Nothing was added to the application's usage report or health vocabulary by this lane. What the Worker must learn, in the order "Worker first, then the application":
* radio kind word **`kiwisdr`** (W1): `health::vocabulary()` driver list, `core/event_trail.cpp kDrivers`, `core/telemetry.cpp nativeFamilyModel`, and the Worker's own copies. Until then the app reports `other` and the equality tests stay green.
* **`radios_max`** (the number of open radios allowed, eight): a usage-report field the app does not send yet; add it to the Worker's schema before the app is changed to send it.
* radio kind words for the sources merged from `feat/radio-sources` and `feat/unblocked-desk` when they are registered (not registered here): `soapyremote`, `sdrconnect`, `extio`; and `hpsdr`, `rspduo` when their blocks lift.
* **The log address scrubber** now hides `kiwisdr=`, `soapyremote=` and `sdrconnect=` like `rtltcp=` (commit 923b8ccd, with a test).

## 6. What was NOT done, and why (an ordered list, so a follow-up message can continue it)

Order = suggested order of the next session. "Needs" says what is missing.
1. **`test_engine_api_merge` residual:** one run of three concurrent copies ended `1 differ` (`nothing yet | A:RUN0 B:LATCH1 A:RUN0`, merged 293 ms against one-a-pass 274 ms: both inside the 900 ms window, so not
   a stall). Not investigated. Needs: ten quiet-machine runs alone, then the merge-sweep case run in a loop.
2. **SoapyRemote and SDRconnect registration** (radio-sources R3/R4): the model's fake servers live inside their test files; the engine test needs them extracted into headers first (about 300 lines each), then the
   KiwiSDR source pattern (`kXDriverKey`, `makeDeviceSource`, `openXAt`, SELECT_SOURCE dispatch and exclusion list, second-radio key, REMOTE-deny list at `fox_engine_api.cpp:2010`) in about ten files; the telemetry word first.
3. **Network I/Q out, SBS out, metrics, webhooks, MCP server:** each needs a `ServerHost` listener key (and its `AppConfig` fields) - the metrics page and the phone-web pieces (N26) need `web_server.cpp`, owned by
   `feat/web-secure` (not merged here), so they are listed, not touched. Webhooks and the MCP server also need the thread rows (present as exempt rows) and a credential decision.
4. ~~OIRT stereo~~ DONE (row 15 of section 3). What it still needs: the screen's "Stereo system" choice (section 7), a real OIRT recording, and the standard's maximum-deviation table to confirm the 50 kHz.
5. **FLAC, IF recording, rollover, RF64/SigMF writers, recordings browser, seek and A-B loop, scanner priority and lockouts (`scan_aids`), operating aids (VFO A/B, band stacking), CLI options (`cli_options`), UTC clock:**
   see the Step 0 table (section 2). The writers need a second sink beside `Recorder` and a stop path in `stopReceiver` and `shutdownQuiesce`; scan_aids is superseded by screen-c's scanner extras (the op number
   `0x860C` it proposes collides with this tree: use `0x860A`/`0x860B`, free here, at the screen merge); the CLI parser needs a branch in `process_boot.cpp parseFlags`.
6. **Decode database and alert rules** (core-formats 5/6): decoded text on disk is a privacy decision; with the private flag and the event ring now in, the database can read `PluginApiCore::hostEvents` and skip
   private events - needs the owner's word on default and retention.
7. **Items that need a screen:** every row marked `screen` in section 2; the W1 screen hand-off controls; the ones this lane added are listed in section 7.
8. **Host rows of the depth lanes (brief item 14):** not built: alert sound call (B1), post-squelch capture (B2), airline lookup (B6), waterfall click (B7), RTTY_SCOPE (B8), `start_capture` (B12), marker kind with a dB level (B13), choice-list setting kind (B14), Winlink forms window (B16). Each is an appended
   call or capability bit with a host-side plumbing and a screen half; see `D:/foxsdr-build/prep/depth-digest.md`. The two the digest lists as "already done in the host" were verified so (B15 `STATE_IMAGE_INPUT`,
   B18 estimated tracks). **B9 (NFM 7 kHz discriminator audio for 9600 baud) was not measured.**
9. **`mbe_voice` integration (item 15)** and the INDEX.md serial rows (item 16): not reached; the serial rows R5 R10 R11 R18 R19 D4-D7 F6-F16 F20 S1-S3 S12 T4 T6 T7 P4 P7 P15 are described in the Step 0 survey
   (`D:/foxsdr-build/w2-scratch/step0-D.md`, section D); the web-page rows are listed because `feat/web-secure` has not merged.
10. **knob-input:** left as a model (item 9): the record says it is a host layer on the Slint side, `plugin-or-host.md` row U11a places it as the plugin `controllers`.
11. **The PASSWORD-at-rest question** is not decided by the owner and no credential storage was built: settings of kind PASSWORD travel the same configuration file as every other setting (`settings_set` writes each
    value verbatim to the file with dated backup copies; whether the file is protected at rest is UNVERIFIED); an owner decision (see section 9).

## 7. Hand-offs

**To the screen lanes (English strings; none translated):**
* Frequency lock: the lock lamp sends `FOXAPP_OP_RADIO_LOCK` (target = the radio, `ival[0]` 1/0) beside its own mask; `fact.radio.<n>.locked` is the read-back.
* Macros: a list reading `fact.macro.names` with "Macros", "Run", "Stop", "Reload macros"; `fact.macro.running`, `fact.macro.last`, `fact.macro.error` for the status line.
* Recording file name: a "File name" field with the hint "{freq_mhz}/{mode}_{datetime}", `FOXAPP_OP_SET_RECORD_NAME`, `fact.record.nameTemplate`; the model's refusal words come back as the op's message.
* E4000 offset tuning: a row "Offset tuning" (tooltip "Moves the E4000 tuner's oscillator off the wanted frequency to keep its centre spike out of the picture."): the Signal bank's option table (`signal_logic.cpp`, near line 703) ignores an unknown option name.
* OIRT stereo: a "Stereo system" choice in the WFM controls (English: "Stereo system", "Automatic", "Pilot tone", "OIRT (65.8-74 MHz)"), `FOXAPP_OP_SET_STEREO_SYSTEM` (target = the radio, `ival[0]` 0/1/2), `fact.radio.<n>.stereo.system` and `.stereo.decoder` as its read-back; the lamp already reads `pilotLocked` / `stereoActive`.
* `--benchmark` is a command-line mode (kept out of the usage text); a Settings > About button is optional.

**To `engine-merge`:** the screen branches B (`feat/screen-b`), C (`feat/screen-c`) and S1 are not in this tree. Op numbers: screen-c's proposed `FOXAPP_OP_SCANNER_EXTRAS 0x860C` collides with this tree (0x860C-0x8613 are SCHEDULE_* and CAL_*; this lane added 0x8614-0x8618); its `0x860A`/`0x860B` are free here. The engine hand-offs C1-C9 are in section 2.7. The `thread_inventory_rows.inc`
rows for `flac_writer`, `if_recorder`, `rec_browser`, `mcp_http`, `webhook` are exempt rows ("NOT COVERED by a heartbeat: nothing starts it yet"): the wiring that starts each gives it a ThreadSpec and removes the exemption.

**To `plugins-merge-3`:** the plugins repo copy `include/plugin_abi.h` (branch review-fixes-2026-11 holds a STALE one) must gain: `CASCADE_FLAG_PRIVATE_DATA` (bit 31) and its comment; `CascadeEvent`, `CASCADE_EVENT_*` and the three appended host members `post_event`, `read_events`, `events_seq`; `CASCADE_MAX_MARKERS_PER_PLUGIN` 256. There is NO byte-compare test of the header copy (only `test_plugin_abi3_compat`, which needs `FOXSDR_PUBLISHED_PLUGINS_DIR` and prints SKIP without it): the check is a manual diff. `docs/PLUGIN-API.md` has the new sections "Decode events" and "Private data".
The privacy plugins (Remote ID, trunking, meters, BLE, LoRa) should set `CASCADE_FLAG_PRIVATE_DATA` in their descriptors.
`test_plugin_abi3_compat` has a STALE assertion (`(p.capabilities & ~0x7FFu) == 0u`: no published plugin declares a capability above `CASCADE_CAP_AUDIO_OUT`): 13 of the 43 newest published binaries declare higher bits, so with
`FOXSDR_PUBLISHED_PLUGINS_DIR` set it reports 13 failures although all 43 load.

## 9. Decisions for the owner (this lane's)

1. **Private-plugin flag:** bit 31 in `capabilities`, no table, outside the capability census. Confirm; and tell the privacy plugin lanes to set it.
2. **PASSWORD settings at rest:** undecided; present behaviour: the value is written to the configuration file verbatim (and its dated backups). No credential storage was built here.
3. **Macros:** a step acts on the first radio; the model's deny list (no transmit, no plugin grants, no server or interface configuration, no uploads) applies; the file is the user's own `macros.json`. OK as is?
4. **Frequency lock:** not saved across a restart (the window's lamp is its own). Should it be?
5. **File-name template:** one template for every take, local time, `{bookmark}` unresolved (reads `unknown`). Rollover, FLAC and one-file-per-transmission are separate decisions.
6. **Event ring:** a plugin may post without a quota (it only displaces others' events from a ring of 1,024). Add a quota?
7. **Marker cap 256** (plugin ABI constant): confirm; a plugin built against it on an older host gets `LIMIT` at the older host's quota.
8. **Two APRS-IS paths:** the plugin's (receive and iGate, `qAO`) is the one that works; the core client (`net/aprs_is.*`, `qAO` enforced at `aprs_is.cpp:274`) stays an unwired model so no station runs two iGates.
9a. **OIRT stereo:** automatic by band (65.8 to 74 MHz, both edges in) is the default for every radio in WFM; the choice is not saved (a restart is automatic); 50 kHz is the OIRT deviation the decoder is scaled for (UNVERIFIED against the standard). Confirm "automatic" as the default.
9. **PortAudio is patched in the vendored tree** (`third_party/portaudio/src/common/pa_front.c`, fenced, recorded in `FOXSDR-PATCHES.md`): a lock around its open-stream list. Upstream's fix, if any, should replace it at the next upgrade.

## 10. Unverified (everything not run)

* `confirmLinuxUpdate` (Linux-only lines of `process_boot.cpp`) was not compiled: no Linux toolchain here; CI's Linux build is the check.
* gqrx and CubicSDR bookmark files were never seen (the model's own caveat): the import is proven against the model's hand-written fixtures.
* E4000 offset tuning against a real E4000 dongle behind a real rtl_tcp server.
* A real plugin setting `CASCADE_FLAG_PRIVATE_DATA` (none does yet): the flag-to-sink path is proven with stand-in names; the flag read from a descriptor by the validator test.
* The classification of the `test_engine_api_merge` crash on the exact base commit d3630d78 (no binary of it exists; the evidence is the identical sink code from d3630d78 to W1's tip and another lane's crash at the same offset).
* The frequency lock against a real plugin `request_tune` and a running list scan (both end in `retuneSourceHz`; reasoned, not driven).
* Every item in section 6.
