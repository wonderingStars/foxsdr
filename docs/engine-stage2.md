# Engine extraction, stage 2: one snapshot for everything

Status: **implemented on branch `engine/stage2`** (from `engine/stage1`
bca426f, with the stage-1 follow-up round 24f0e81 merged). The plan is
foxsdr-api `docs/ENGINE-EXTRACTION.md` section 4, "Stage 2"; the vocabulary is
the engine API, `third_party/foxsdr_api/foxsdr_api.h` (API 0.2, vendored
unmodified).

## 1. What changed

- **One receiver snapshot, published once a frame** (`core::ReceiverSnapshot`,
  src/core/receiver_snapshot.hpp). The GUI thread fills a `PublishedState` -
  the API's own `FoxReceiverState` plus `AppStateExt`, the app's extension for
  what API 0.2 has no field for (section 3) - and the /api/status text and
  lists (a `net::RadioStatus` holding only strings and vectors), and publishes
  both from ONE call, `AppWindow::publishReceiverState`.
- **Every reader answers from it:**
  - the plugin host API - `get_state`, `get_gain`, `get_sample_rates`,
    `get_stream_info`'s output figures, and the device checks inside its
    requests (`PluginApiCore`);
  - CAT - every `executeCatLine` query, and the figures its sets are planned
    against (`AppWindow::catStatusNow`);
  - the web server - `GET /api/status` and the `/api/control` key gate, which
    reads `transmitAvailable` from the same status (`AppWindow::webStatusNow`).
- **PluginApiCore's own snapshot is gone.** It used to keep a `SeqlockBox` of
  its own, published by `publishPluginApiState`; it now reads the application's
  `ReceiverSnapshot` (handed to it through `PluginUi`'s new constructor).
  `webStatus_` (the RadioStatus the web and CAT providers copied under
  `webMutex_`) is gone too. There is one published state.
- `net::composeRadioStatus` (src/net/status_compose.*) builds the RadioStatus
  a web or CAT reader is given, **on the reader's thread**: every scalar from
  the PublishedState, every string and list from the block.
- `SeqlockBox` moved, unchanged, to src/core/seqlock_box.hpp; the plugin
  runner's `StreamClock` still uses it (section 4).
- `net::statusJson` - the /api/status body, moved verbatim out of the route so
  the golden test renders exactly what the route renders.

**Where the publish happens.** `drawUi` calls `publishReceiverState` where
`publishWebSnapshot` was: after both command drains, the web/CAT requests,
the plugin requests, the scanner's frame and a dropped file - every command
of the frame has landed - and before anything is drawn. The plugin snapshot
used to be published a few lines earlier (the end of `applyPluginApi`, before
the scanner); a control a plugin asked for is still in the snapshot it reads
that frame, and now so is the scanner's retune of that frame.

## 2. The field map

`FoxReceiverState` field -> where it is read (GUI thread,
`AppWindow::fillPublishedState`) -> which readers use it in stage 2.
"none yet" means the field is published for the API and read by nobody until
stage 4 moves the window (and stage 5 the transports) onto `read_state`.

| Field | Source | Readers |
|---|---|---|
| `structSize` | set by `publish()` | - |
| `flags` RUNNING | `pipeline_.running()` | plugin RUNNING; web `running`; CAT `\get_powerstat` |
| DEVICE_OPEN | `device_ != nullptr` | plugin DEVICE_OPEN, `get_sample_rates` (0 without), request checks (NO_DEVICE) |
| FAULTED | `pipeline_.faulted()` | web `faulted` |
| MUTED | `userMuted_` | plugin MUTED |
| SQUELCH_OPEN | `signalDb > squelchDb` (the level-1 rule; section 6, OPEN 4) | none yet (the plugin computes the same rule from the same two figures) |
| STEREO_ENABLED | `stereoEnabled_` | web `stereoEnabled` |
| STEREO_ACTIVE | `pipeline_.stereoActive()` | plugin STEREO; web `stereoActive` |
| NR / NOTCH / AUTO_NOTCH | `nrEnabled_` / `notchEnabled_` / `autoNotch_` | web `nrEnabled` / `notchEnabled` / `autoNotch` |
| DEVICE_AGC | `deviceAgc_` | plugin DEVICE_AGC; web `agc` |
| AGC_SUPPORTED | `deviceAgcSupported_` | plugin AGC_SUPPORTED, request check (UNSUPPORTED); web `agcSupported` |
| RECORDING_IQ / _AUDIO | `iqRecorder_.recording()` / `audioRecorder_.recording()` | web `iqRecording` / `audioRecording` |
| SCANNER_ACTIVE | `scanner_.active()` | web `scannerActive` |
| DECODER_ACTIVE | `running &&` the runner's fed-instance count `> 0` (the DEC lamp's own predicate; `PluginRunner::feedSnapshot`, one lock) | none yet |
| TX_AVAILABLE | `transmitter_.haveSink()` | none yet |
| TX_KEYED | `transmitter_.transmitting()` | web `transmitting` |
| TX_LATCHED | `transmitter_.latched()` | none yet |
| TX_REMOTE_ARMED | `transmitOpen_ && transmitter_.haveSink()` (section 6, OPEN 6) | web `transmitAvailable`; the `/api/control` key gate |
| SINK_OPEN | `pipeline_.audio().running()` | none yet |
| WEB_LISTENING | `webServer_.running()` | none yet |
| TX_KEY_MINE, TX_LATCH_RELEASE_FIRST | 0 - per session; there are no sessions before stage 5 | - |
| `seq`, `tuneSeq`, `modeSeq`, `deviceSeq`, `audioSeq`, `displaySeq`, `txSeq` | derived by `publish()` (section 5) | none yet (plugins read the level-1 counters, section 3) |
| `listSeq` | 1 from the first publish (OPEN 1) | none yet |
| `grants` | 0 - per session | - |
| `centreHz` | `activeSource().centerFrequencyHz()` | plugin; web `centerHz` (and the spectrum frame's centre); CAT `f`, planTune |
| `vfoOffsetHz` | `pipeline_.vfoOffsetHz()` | plugin; web; CAT `f`, planTune |
| `tunedHz` | `centreHz + vfoOffsetHz` | none directly (the plugin computes the same sum) |
| `sampleRateHz` | `activeSource().sampleRateHz()` | plugin; web; CAT planTune, `\dump_state` |
| `channelRateHz` | `pipeline_.channelRateHz()` | none yet |
| `bandwidthHz` | `vfoBandwidthHz_` | plugin; web; CAT `m` |
| `squelchDb` | `squelchDb_` (float, exact in double) | plugin; web (back to float, exact) |
| `volume` | `volume_` | plugin; web |
| `signalDb` | `pipeline_.signalPowerDb()` | plugin; web |
| `sMeter` | `clamp((signalDb + 120) / 120, 0, 1)` | none directly (the plugin computes the same) |
| `audioLevelDb` | -200, not filled (OPEN 2) | - |
| `dbMin`, `dbMax` | `dbMin_`, `dbMax_` | web |
| `nrStrength`, `notchHz`, `notchQ` | `nrStrength_`, `notchFreqHz_`, `notchQ_` | web `nrStrength`, `notchFreqHz`, `notchQ` |
| `pilotLevel` | `pipeline_.pilotLevel()` | none yet |
| `demodMode` | `gui::abiDemodForModeIndex(modeIndex_)` | plugin; web `mode` and CAT `m` by name (`net::kDemodNames`, static_asserted equal to `kModeNames`) |
| `deemphasis` | `deemphIndex_` | web `deemphasisIndex` |
| `gainCount` | `deviceGainRanges_.size()` with a radio, else 0 | none yet (plugins read `abiGainCount`, capped at 16) |
| `decodersRunning` / `decodersFitted` | the loaded decoder modules the runner feeds (0 when stopped) / the loaded decoder modules - `fedDecoderCount()` / `loadedDecoderCount()`'s answers, from one `feedSnapshot` (review L4) | none yet |
| `txMode`, `txFrequencyHz`, `txPowerDb` | `transmitter_.mode()`, `.frequencyHz()`, `.powerDb()` | none yet |
| `audioUnderruns` | `pipeline_.audio().underruns()` | web `audioUnderruns` |
| `txHoldRemainingMs` | `transmitter_.remoteHoldRemainingMs()` | web `transmitRemoteHold` |
| `txLatchRemainingMs` | 0, not filled (OPEN 3) | - |
| `deviceName` | `activeSource().name()` through `formatUtf8` (64 bytes, as the plugin ABI's) | plugin `deviceName` |
| `sinkName` | `pipeline_.audio().openedDeviceName()` (a try-lock read; its busy "" keeps the last good name, `keepLastGoodName`, review L6) | none yet |
| `faultMessage` | `pipeline_.faultMessage()` (127 bytes) | none yet (the web's `faultMessage` is the untruncated one in the lists) |
| `txUnkeyReason` | empty, not filled (OPEN 3) | - |

**The lists block** (`AppWindow::fillStatusLists`, the rest of the old
`publishWebSnapshot`) carries every `std::string` and `std::vector` member of
`RadioStatus`, untruncated: `faultMessage`, `sourceName`, `tunerDisplayStyle`,
`rdsPs`, `rdsRadioText`, `sourceKind`, `soapyArgs`, `antenna`, `antennas`,
`devices`, `gains`, `sourceError`, `audioMutedBy`, `audioSource`, `recordDir`,
`recordError`, `recordNotice`, `bookmarks` (with the ids of those rows, which
ride in the same block - `Full::bookmarkIds`, review L3), `tracks`
(whose loop still drives the track-info lookups every frame), `images`,
`basemap.attribution`, `plugins`, `catalogue`, `catalogueStatus`,
`catalogueError`, `installReport`, `installError`, and `decoded`. Readers: the
web only.

## 3. The extension, `AppStateExt` - every member, and why API 0.2 lacks it

| Member | Source | Reader | Why it is not a FoxReceiverState field |
|---|---|---|---|
| `published` | set by `publish()` | web, CAT: before the first publish they answer a default `RadioStatus`, as before | an implementation flag (the API's "0 = never read" is the counters) |
| `abiSeq`, `abiTuneSeq`, `abiModeSeq`, `abiDeviceSeq`, `abiAudioSeq` | derived by `publish()` | plugin `get_state` counters | the host API level 1's groups are not the engine API's (its modeSeq excludes the audio DSP; its seq counts the stereo lamp; it has no display/tx/list groups); a plugin's counters must not change meaning |
| `abiGainCount`, `gains[16]` (`PublishedGain`) | `deviceGainRanges_`, `deviceGainsDb_` | plugin `get_gain`, request checks | gain stages are a LIST in the API (`FOXAPI_LIST_GAINS`); plugins read them from any thread, so they travel with the state |
| `rateCount`, `rates[32]` | `deviceRatesHz_` | plugin `get_sample_rates` | a LIST in the API (`FOXAPI_LIST_SAMPLE_RATES`) |
| `outputRateHz`, `outputFrames` | `Pipeline::kAudioRateHz`, `audioSamplesProduced()` | plugin `get_stream_info` | `get_stream_info` is a GAP in the API |
| `rxPositionSet`, `rxLatDeg`, `rxLonDeg` | `rxSet_`, `rxLat_`, `rxLon_` | web | no position fields (FOXAPI_CAP_POSITION has none in the state yet) |
| `autoNotchEngaged`, `autoNotchFreqHz` | `pipeline_.autoNotchEngaged()`, `.autoNotchFrequencyHz()` | web | API GAP: the auto-notch readout |
| `pilotLocked` | `pipeline_.pilotLocked()` | web | the API has `pilotLevel` and STEREO_ACTIVE, not the lock itself |
| `rdsSynced`, `rdsPiValid`, `rdsPi`, `rdsPsValid`, `rdsPty`, `rdsTp`, `rdsTa`, `rdsGroups`, `rdsErrors` | `pipeline_.rdsSnapshot()` | web | API GAP: RDS fields |
| `sourceBusy` | `soapyScanPending_ \|\| deviceOpenPending_` | web | no "scan/open in flight" state |
| `audioPrimingCallbacks`, `audioRingMs`, `audioRingCapacityMs` | the sink | web | the API has only `audioUnderruns` |
| `audioPluginGaps`, `audioPluginGapFrames` | `pluginRunner_` | web | no plugin-audio health in the API |
| `iqBytes`, `audioBytes` | the recorders | web | the recorder's progress is not in the state |
| `scannerState`, `scanStartHz`, `scanStopHz`, `scanStepHz` | `scanner_.state()`, `scanStartMhz_`... (the same expressions as before) | web (`scannerState` by name) | the API has SCANNER_ACTIVE only |
| `catalogueBusy` | `catalogPending_ \|\| installPending_` | web | the store's state is not receiver state |
| `basemapActive`, `basemapMinZoom`, `basemapMaxZoom`, `basemapTileSize` | `basemap_` | web | the basemap is a plugin service, not in the API |

`sizeof(PublishedState)` = 2328 bytes (FoxReceiverState 664 + extension 1664),
most of it the gain and rate tables.

## 4. Freshness: what readers got before, and what they get now

**No reader read the receiver live before stage 2.** The web server and CAT
copied `webStatus_`, published once a frame; plugins read PluginApiCore's
snapshot, published once a frame. So nothing a reader is told became older:

- **Plugins**: still once a frame, now published a few lines LATER in the same
  frame (after the scanner and a dropped file) - fresher by exactly those, and
  still before anything is drawn.
- **Web and CAT**: once a frame, from the same point as before.
- **Measurements** (signalDb, stereo lamp, pilot, RDS, the sink's health, the
  recorders' bytes) were once-a-frame samples for every reader and stay so.
  Accepted: every reader of them is a display polling at 4 Hz (the page) or
  slower.
- **Kept live: the plugin stream clock** (`StreamClock`, get_stream_info's
  epoch and iq/audio frame counts). It is written by the DSP thread under the
  runner's mutex once per BLOCK, it is a clock rather than state, and a
  decoder timestamping its output needs it exact to the block; a once-a-frame
  copy would be up to 16.7 ms stale. It stays its own SeqlockBox. The API has
  no field for it (get_stream_info is a GAP).
- **The web block can trail the lock-free state only until the next
  `readFull()`** when a web reader held the swap lock at the instant of a
  publish (section 5): the block is handed over and the next reader installs
  it. Only if a reader is inside that very hand-over too does it wait for the
  writer's next pass (`retryInstall`, every frame). Measured under a reader
  calling `readFull` in a tight loop: 29% of publishes could not install
  directly (before the hand-over existed). The block is always whole.
- **The browser's bookmark rows and the ids they are resolved by are one
  block** (review L3): `applyControlRequest` resolves a web row through
  `readFull()->bookmarkIds`, the ids of the rows /api/status is serving at
  that moment. What remains is the browser's own lag - its rows are from its
  last poll (up to 250 ms), and a row that moved since names what is on it
  NOW - which is the web protocol's (rows, not ids), as before stage 2.
- **CAT**, which reads only figures, takes the lock-free path; if a read keeps
  overlapping a publish (SeqlockBox::kMaxTries) it takes the installed block
  instead - at most a frame older - rather than answer nothing.

## 5. Threading, and why nothing waits

- **One writer**: `publishReceiverState` on the GUI thread (the engine's
  control thread from stage 3; the snapshot does not care which, as long as it
  is one).
- **`read()`** is `SeqlockBox::load`: no lock, no allocation, callable from a
  real-time thread; the writer never waits for it.
- **`readFull()`** copies a `shared_ptr` to an immutable block (the state AND
  the lists of one publish) under `fullMutex_`, held for a reference-count
  increment. **The writer only try-locks it.** When a reader holds it, the
  writer HANDS THE BLOCK OVER under a second lock, `handoffMutex_`, which it
  also only try-locks, and the next `readFull()` installs it (readers take
  `fullMutex_` then `handoffMutex_`; a handed-over block older than the
  installed one, by generation, is dropped). If that lock is busy too - a
  reader is in that very install - the writer keeps the block and
  `retryInstall()` lands it: `drawUi` calls it at the top of EVERY frame,
  change or not, and in stage 3 the control thread must call it on every
  pass. Why a hand-over and not just a retry (review M2): the Windows
  move/resize loop runs no frames at all (FoxSDR draws nothing from a
  refresh callback), and stage 3's control thread may publish only on
  change, so "the next publish" may be a long way off. The GUI thread's
  worst case is two failed `try_lock`s. Replaced blocks are released after
  the locks are dropped. `std::atomic<std::shared_ptr>` was considered and
  not taken: probed on MSVC 14.44 and GCC 15.2 it is provided but
  `is_lock_free()` is false (a lock the writer would wait on), and the
  Android NDK r27's libc++ 18 does not define it
  (`__cpp_lib_atomic_shared_ptr` absent).
- **The counters are derived, not set**: `publish()` compares each group's
  fields with the previous publish. Engine API: tune = centre, offset; mode =
  demod, bandwidth, squelch, de-emphasis, NR strength, notch Hz/Q and the
  STEREO_ENABLED/NR/NOTCH/AUTO_NOTCH flags; device = running, device open,
  faulted, AGC, AGC supported, rate, channel rate, gain count, device name,
  fault message and the gain/rate tables; audio = volume, mute, sink open,
  sink name; display = dbMin, dbMax; tx = the TX flags, mode, frequency,
  power, unkey reason; seq = any of those, any other flag, or the decoder
  counts. Measurements move nothing: signalDb, sMeter, audioLevelDb,
  audioUnderruns, the two key timers, and pilotLevel (OPEN 5). The level-1
  counters use exactly the comparison PluginApiCore::publish used.
- **Before stage 2 the GUI thread did wait for readers**: the web and CAT
  providers copied the whole `RadioStatus` (every bookmark, track, plugin and
  catalogue entry) under `webMutex_`, and the GUI thread's publish took the
  same mutex. The providers now copy a pointer under the lock and compose the
  RadioStatus off it, unlocked.

**Cost** (Release, this desktop; `test_receiver_snapshot` [6] and
`test_state_snapshot_golden`):

| What | ns |
|---|---|
| `ReceiverSnapshot::publish` (counters, seqlock store, block, swap) | **166 per frame** |
| the whole `publishReceiverState` on the real window (fill both halves + publish; generator, no plugins, two bookmarks) | **2255 per frame** (0.014% of a 16.7 ms frame) |
| `read()` (plugins, CAT) | 67 |
| `readFull()` + `composeRadioStatus` with 301 bookmarks, 20 plugins, 50 tracks (web, on the HTTP thread) | 18 276 |

The lists half costs what `publishWebSnapshot` cost (it is the same code, and
built a RadioStatus every frame then as now); the old per-frame figure was not
measured separately.

## 6. The window

**It still reads its own members this stage, everywhere.** Considered and not
done where it looked cheap (the DEC lamp: `running &&
pluginRunner_.activeCount() > 0` is exactly the published DECODER_ACTIVE):
the publish point is before `pollSourceAsync` and the immediately-applied
gestures (the VFO and band-edge drags, the Source list's scan on open), all of
which change receiver state later in the same frame. A draw site switched to
the snapshot would therefore show the value from before them - one frame
behind the member it replaced, in precisely the frames where something just
changed - which is a (small) visible change this stage must not make. Stage 4
moves the window onto `read_state` as a whole, with the publish on the control
thread, and the question is settled there.

## 7. Tests

- **`test_state_snapshot_golden`** (new): the real window on the
  test_apply_command harness driven through 24 states (never published, start,
  tuned, all eight modes, channel, mute, audio DSP, display + position + the
  pass-through strings, scanned, a radio open, its gains/antenna/rate, AGC,
  bookmarks + scanner range, running, running retuned, stopped, back to the
  generator). Each state renders the /api/status body (`net::statusJson`), the
  reply and control of 22 CAT lines and every plugin read plus seven plugin
  requests, and compares with `tests/golden/state_snapshot.golden` - **written
  by the code before stage 2** (commit 79ca44b, on bca426f, twice, identical).
  Doubles are compared as bit patterns. Masked in capture and comparison alike:
  the audio sink's health (this machine's device), and after the receiver has
  run its measurements. Result: **0 of 878 lines differ.**
- **`test_receiver_snapshot`** (new, 409 checks): torn-read races (SeqlockBox
  with 64 identical words; the snapshot's `read` and `readFull`, the block's
  lists paired with its state), the writer never waiting and no block
  stranded (the hand-over lands with no further publish or retry; with both
  locks held, `retryInstall` lands it with nothing changed; a newer block
  replaces a held-back one), every counter group (27 steps),
  `composeRadioStatus` field by field (86 rows) with a scan of
  `net/web_server.hpp` that fails when `RadioStatus` gains a member without a
  row, the plugin API reading the snapshot it was given, **walking one**
  (review M1: each of the 32 flag bits alone composes exactly its own
  RadioStatus boolean or none; each extension boolean alone likewise, the 25
  booleans checked against the header; each of the 22 `RxFlagSources` alone
  sets exactly its own flag; each flag alone gives exactly its level-1
  plugin flag), a busy sink-name read moving no counter, the bookmark ids in
  the block, and the cost.
- **`test_snapshot_app`** (new, 112 checks): the real window with three
  decoder plugins (tests/fixture_stage1_plugin.cpp built under three names -
  the host runs one plugin per name): the publish cost with three fed, the
  published decoder counts against the window's own; **walking one through
  the window** - mute, stereo, NR, notch, auto notch, a radio opening, device
  AGC, run, I/Q and audio recording, stopping every decoder, the scanner, a
  transmitter opening, the Transmit page - each changing exactly its own
  flag (SQUELCH_OPEN, STEREO_ACTIVE and SINK_OPEN follow the signal or the
  audio device and are left out; FAULTED, TX_KEYED, TX_LATCHED and
  WEB_LISTENING are not reachable without hardware or a fault and are
  walked in the pure test); and the L3 case - with a newer block held back,
  the page's row 0 tunes what the served block shows on it, and after
  `retryInstall` the new row 0.
- **The golden's two retunes now land** (review L5): on a radio a retune is
  paced by the retune coalescer and landed by the frame's
  `pollPendingRetune`; the drive now does the same (`settleRetune`), so
  "Golden two" is added at 433.9 MHz and "running, retuned" is. The record
  was re-recorded by the OLD code - a throwaway worktree at 79ca44b with the
  same test patch, written twice, identical - not by stage 2.
- `test_plugin_api` and `test_plugin_abi3_compat` publish through
  `tests/receiver_facts_helper.hpp` (the old `ReceiverFacts`, mapped onto a
  PublishedState the way the window maps its members).
- `test_apply_command`'s "web bookmark rows" (stage 1's follow-up) reads the
  rows through `webStatusNow`; `test_command_path_guard` lists
  `publishReceiverState`, `fillPublishedState` and `fillStatusLists` as
  reviewed engine members (they read the receiver and write only the snapshot
  and the row map).

**Seen red** (the behaviour broken, the test exe deleted and rebuilt with a
changed hash, then restored with `git checkout`, rebuilt, green):

| Mutant | Test | Red |
|---|---|---|
| SeqlockBox::load's sequence re-check replaced by `true` | test_receiver_snapshot | 286 042 torn reads of SeqlockBox and 286 971 of the snapshot; 2 failed |
| `publish()` locks the swap lock instead of trying it | test_receiver_snapshot | "publish returned while the lock was held: NO"; 1 failed |
| the level-1 mode group made the engine API's, and compose drops `rdsTa` | test_receiver_snapshot | NR, notch and de-emphasis steps moved abiMode; the rdsTa row; 4 failed |
| compose takes `bandwidthHz` from the rate | test_state_snapshot_golden | 67 of 878 lines differ |
| `get_state`'s modeSeq from the engine API's counter | test_state_snapshot_golden | 11 of 878 lines differ |
| the window fills `scanStepHz` in the wrong unit | test_state_snapshot_golden | 23 of 878 lines differ |
| review round (logs: scratchpad `engine3-review/rg_*.log`): | | |
| compose `transmitAvailable = flag(TX_KEYED)` (the reviewer's mutant) | test_receiver_snapshot | [7a] TX_KEYED -> [transmitting transmitAvailable], TX_REMOTE_ARMED -> []; 2 failed |
| compose swaps `iqRecording` / `audioRecording` | test_receiver_snapshot | [7a] RECORDING_IQ -> [audioRecording] and back; 2 failed |
| `receiverFlags` swaps the two recording sources' bits | test_receiver_snapshot | [7c] recordingIq -> 00002000 and back; 2 failed |
| the window hands each recording condition the other recorder | test_snapshot_app | [2] record I/Q changed 00002000, record audio 00001000 (x2 each); 4 failed |
| no hand-over (a deferred block waits for the writer) | test_receiver_snapshot | [2a] the next readFull: centre 1 (published 2); 6 failed |
| nothing kept for the retry (the old "next publish" rule) | test_receiver_snapshot | [2b] after retryInstall: centre 2 (published 3); 3 failed |
| the web row map from the latest fill, not the served block | test_snapshot_app | [3] row 0 (showing A, AM) tuned USB; 1 failed |
| every fitted decoder counted as fed while running | test_snapshot_app | running, none fed: published 3, window 0; 3 failed |
| a busy sink-name read publishes "" | test_receiver_snapshot | [8] counters moved [seq audio]; 2 failed |

## 8. OPEN

(Review round: M1, M2, L3, L4, L5, L6 and the NIT are closed - sections 4,
5 and 7. The window's publish with three fed decoders measured 4398 and
4460 ns/frame before L4 and 4334 and 4465 after - the same within run
noise on an uncontended lock; what L4 changes is 4 acquisitions of the
runner lock the DSP thread takes per block down to 1.)

1. **`listSeq` is not derived** (held at 1). Deriving it needs list
   generations (bookmarks, devices, plugins, catalogue, decoded lines,
   tracks); with them, the lists block could also be skipped on frames where
   nothing changed instead of rebuilt and republished every frame (as
   `webStatus_` was). Belongs with `read_list` (stage 4).
2. **`audioLevelDb` is not filled** (-200). The deck's VOLUME meter computes
   it from a 2048-sample snapshot of the scope audio tap as it draws; doing
   that at publish time would add that copy to every frame. Decide when the
   deck moves onto `read_state` (stage 4).
3. **`txLatchRemainingMs` and `txUnkeyReason` are not filled**: the
   Transmitter publishes no latch timer and keeps no unkey sentence. Stage 3
   (the keep-alive and the API's latch protocol, stage 1 OPEN 1).
4. **SQUELCH_OPEN is the level rule** (`signalDb > squelchDb`, what the plugin
   API has always told plugins), not the DSP's gate with its 3 dB hysteresis:
   the pipeline does not publish the gate.
5. **`pilotLevel` is treated as a measurement** for `seq`; API 0.2's list of
   measurements (signalDb, sMeter, audioLevelDb, the underrun and hold
   counters) does not name it, but it follows the signal exactly as signalDb
   does. Suggest the API document it.
6. **TX_REMOTE_ARMED is the app's implicit consent**: a transmitter open AND
   the Transmit page on screen (what /api/status has always called
   `transmitAvailable`). The API models consent as an explicit
   `TX_REMOTE_ARM` from a LOCAL session; until that op exists (stage 3/5),
   the flag reports the app's rule.
7. **The spectrum frame for the browser still goes through `webMutex_`**: a
   stream, not state (the API's `read_spectrum`), so outside this stage. The
   GUI thread takes that mutex when a new frame arrived, and an
   `/api/spectrum` handler holds it while it copies the bins - the one place
   left where a web reader can delay the GUI thread (by a few-KB copy).
8. **Truncation in the API fields**: `deviceName` (64 bytes) and
   `faultMessage` (127) are fixed-size in `FoxReceiverState`; the web reads the
   untruncated strings from the lists block, so nothing changed for it, but an
   interface reading only `read_state` (stage 4) would see the truncated ones.
9. **Two gain lists**: `gainCount` and the plugin table come from
   `deviceGainRanges_`, the /api/status `gains` from `deviceGainNames_` - as
   before stage 2, when the plugin and the browser read them the same way. They
   are filled together by the device open; nothing checks they agree.
