# Engine extraction, stage 3a: the Engine owns the receiver

Stage 3 of docs/ENGINE-EXTRACTION.md (foxsdr-api repository) gives the
receiver its own control thread. It is split in two. **3a (this document)
moves OWNERSHIP**: everything that is the receiver - its objects, its state,
the machinery that changes it and the per-frame steps that keep it running -
leaves `gui::AppWindow` for a new class, `cascade::engine::Engine`
(src/engine), and the window holds one and asks it. **Nothing about threads
changes**: every Engine method still runs on the GUI thread, called from the
same places in the frame loop, in the same order. **3b** then moves the
Engine's calls onto a control thread; section 8 is its checklist and section
9 what it has to settle first.

No user-visible behaviour changes. Code moved verbatim: every commit message
names what moved and every line that did not move verbatim, with the reason.
The scratchpad tool `verify_m.py` reversed every listed substitution and found
all 153 definitions moved in commits 4-6 byte-identical to their AppWindow
originals, comments included.

## 1. Commits (branch engine/stage3, from f598bc0)

| # | commit | what |
|---|--------|------|
| 1 | 1bb430c | the pure helpers the machinery needs move to src/engine (tune_control, rate_follow_status, source_fallback, device_scan_plan, soundcard_panel, bias_tee, audio_open, running_view, tx_frequency, plugin_store_reasons); include paths only |
| 2 | 6822eaa | the snapshot's hand-over becomes a lock-free slot (review L-a/L-b/L-c and the nit); test_snapshot_stress adopted (section 6) |
| 3 | c5ed08a | the Engine owns the receiver's state (241 fields, types, constants); AppWindow holds `engine_` and reaches it as a friend; EngineHost introduced |
| 4 | 73f0f09 | 115 leaf methods move (call depth 0-2), verbatim |
| 5 | fe6c405 | 28 methods: tuning, radios, the plugin rebuild, the publish (split: the engine publishes and returns the centre, the window copies the browser's spectrum frame) |
| 6 | ceca8e1 | 13 methods: the command path (applyCommand, submitCommand, drainLocalCommands, applyControlRequest, applyPluginApi), the patch runtime |
| 7 | 5a09612 | start-up, teardown, the config's receiver half and the per-frame pump become Engine methods |
| 8 | 3b2e131 | test_engine_headless: an Engine with no window, driven by commands |
| 9 | (this) | this document |

Size: src/gui/app_window.cpp 25222 -> 18968 lines, app_window.hpp 4249 ->
2410; src/engine 14437 lines. The Engine has 174 methods (38 const) and 242
data members (the guard's count, section 7).

## 2. Ownership

"Was" is gui::AppWindow for every row; "now" is where the object lives after
3a. The window holds `std::unique_ptr<Engine> engineHolder_` (declared FIRST,
so it is destroyed LAST, after every window member that could still call it)
and `Engine& engine_`, and implements `engine::EngineHost` (section 3).

| object | now | notes |
|--------|-----|-------|
| `Pipeline pipeline_` (the DSP chain, the sources it swaps, the audio sink, the scopes) | Engine | constructed in `Engine(EngineHost&)` with the window's old rate/FFT/alpha (engine/receiver_tables.hpp) |
| the open device: `device_`, `soapyView_`, `deviceArgs_`, `deviceModel_`, gains/antenna/AGC/rate mirrors, `sourceKind_`, `restoreKeep_` | Engine | |
| device-open and scan workers: `deviceOpenFuture_`, `soapyScanFuture_`, `soapyScanAbsent_`, recovery state, `retuneCoalescer_` | Engine | |
| sound card source: settings, `soundCardScanFuture_`, `soundCardOpenFuture_`, live/remembered cards | Engine | `soundCard_` is still edited in place by the panel (OPEN 1) |
| recorders `iqRecorder_`, `audioRecorder_`, `recordDir_`, notices | Engine | `defaultRecordDir()` moved with them |
| transmitter `transmitter_`, `micOpen_`, the transmit settings | Engine | the key is applied by `Engine::pumpTransmitter` |
| audio output gate `audioOpen_`, `devices_`, `deviceIndex_` | Engine | |
| plugin host `pluginHost_`, runner `pluginRunner_`, `pluginUi_` (the host API), `pluginRepo_`, catalogue/install futures, inventory, presets, mutes, stops, grants, settings | Engine | the plugin WINDOWS, the basemap and track-info attachments stay in the window (host hooks) |
| patch runtime: `patchGraph_`, `patchPlan_`, `patchRadios_`, `patchCatalogue_`, `patchRunning_`, the main-radio loan | Engine | the canvas UI, its view and the document text stay in the window; the page edits a draft of the graph and sends it as `FOXAPP_OP_PATCH_SET_GRAPH` (OPEN 6) |
| scanner `scanner_` and its form | Engine | the form is edited in place (OPEN 1) |
| bookmarks `freqMgr_`, `bookmarkPath_`, save debounce | Engine | the list VIEW is the window's (`onBookmarksChanged`) |
| band plan `bandPlan_`, selection | Engine | drawing it stays in the window |
| GPS `gpsReader_`, receiver position `rxLat_/rxLon_/rxSet_` | Engine | the typed fields and map homes are the window's (`onReceiverPositionApplied`) |
| converters `converters_` | Engine | |
| the receiver's half of the config | Engine | `applyConfig(cfg)` / `fillConfig(cfg)` (section 3) |
| usage telemetry: `telemetryReporter_`, `telemetryHeartbeat_`, accruals, install id, enabled flag, `telemetryStartup`/`telemetryJournal` | Engine | the crash-upload sweep, the crash offer, the crash-loop limiter, the update check and the diagnostics switches stay in the window (`diagnosticsStartup`/`diagnosticsJournal`) |
| crash handler context `refreshDiagContext()` | Engine | the crash handler itself is installed by `run()` (window) |
| the one receiver snapshot `receiverSnapshot_` and its publish (PublishedState + the web lists block) | Engine | `snapshot()` is the public reader; the browser's spectrum frame (a stream) is the window's `publishWebSpectrum` |
| the command queue `localCommands_`, `applyCommand`, `submitCommand`, `drainLocalCommands`, `applyControlRequest`, `applyPluginApi` | Engine | public: applyCommand, submitCommand, drainLocalCommands |
| web server, CAT server, their providers | **window** | until stage 5; they read the Engine's snapshot and their requests go to `engine_.applyControlRequest` |
| config FILE, `configWriter_`, save debounce, `savedCfg_` | **window** | the file holds both halves; the window reads it and hands the engine its half |
| hang watchdog `watchdog_` | **window** | the engine pauses it through the host around its bounded waits |
| spectrum/waterfall views, map pages, scope, basemap, track info, plugin windows, patch canvas, every panel | **window** | |

### The config line

`AppConfig` is one file with two halves; every field is written by exactly one.

- **Engine** (`Engine::applyConfig`, `Engine::fillConfig`): the device scan's
  USRP rule and saved Soapy args, volume, display range (applied to the
  window's spectrum through `onDisplayRange`), squelch, bias tee memory, Pluto
  address, de-emphasis/stereo/NR/notch/auto-notch, band plan selection, the
  transmitter's settings (never its key), receiver position, catalogue URL
  and last check, plugin grants/settings/stops/mutes/user presets, mode,
  converters, sound card, the source (kind, args, file, rate, antenna), centre,
  bandwidth and VFO offset, and the usage report's fields.
- **Window** (`AppWindow::applyConfig`, `AppWindow::currentConfig`): split
  ratio, band plan overlay/size/palette, the patch DOCUMENT (text and view -
  it writes the parsed graph into `engine_.patchGraph_`), language and country,
  tuner style, theme, counter and readings scale, map trails and icon sizes,
  radar and demod scope, rail bank, key bindings, map page rectangles, the
  typed receiver position and GPS port/baud, the store and fitted-modules
  windows, web and CAT settings, the diagnostics/crash/update settings, and
  the I/Q path box (filled from the file whenever the saved source is a file,
  which is when both branches of the old file restore filled it).

`AppWindow::applyConfig` is now: `startupState(saved)`, then
`engine_.applyConfig(cfg)`, then the window's statements in their original
order. Nothing in the engine's half reads the window's state (the host hooks it
reaches read none of the fields the window half restores; the patch graph it
writes is read by no engine code the restore runs), so the only reorder visible
from outside is that **the web and CAT listeners now start after the VFO
restore instead of just before it** - both serve the snapshot, which nothing
publishes until the first frame either way.

## 3. The host: what the engine asks the window

`engine::EngineHost` (src/engine/engine_host.hpp) is the engine's only view of
a front end. The defaults in engine_host.cpp are a headless front end's
answers (clocks from std::chrono, no views, ADD ALL refused); AppWindow
overrides each. Every hook is called SYNCHRONOUSLY on the calling thread - in
3a the GUI thread. The last column is what 3b has to do with it.

| hook | window's answer | 3b |
|------|-----------------|----|
| `frameClockRunning`, `frameTimeS` | ImGui's clock (`ImGui::GetTime`) | NOT callable off the GUI thread (reads ImGui's context): the engine needs its own clock |
| `wallTimeS` | `glfwGetTime` | thread-safe after init, but belongs to the engine |
| `pauseWatchdog`, `resumeWatchdog` | the GUI hang watchdog | the control thread's bounded waits must not pause the GUI's watchdog; it needs its own (section 9) |
| `onDisplayRange` | the spectrum's range | event |
| `onBookmarksChanged` | rebuilds the bookmark list view | event |
| `openPluginWindowsFor` | opens a plugin's windows (preset applied) | event |
| `onReceiverPositionApplied` | typed fields, every map page's home, scope, coverage reset | event |
| `onConverterChanged` | re-seeds the LO field | event |
| `onPatchGraphChanged` | marks the canvas dirty | event |
| `onPatchPicture` | hands a decoded picture to the canvas | event (with the image) |
| `onGpsFixApplied` | reveals the GPS row | event |
| `onCatalogueFetchStarting`, `onCatalogueResult`, `onAddAllFinished` | store deck consent reset, the bounded-run report, the ADD ALL acknowledgement | events |
| `planAddAll` | builds the store model and ADD ALL plan from the window's store state | the plan must be computed from engine state or carried by the command (OPEN 5) |
| `beforePluginRescan` | folds map page geometry into the saved list | must stay synchronous (before the plugins are unloaded) - a handshake |
| `onPluginsUnloading` | detaches the basemap and track-info plugin APIs | MUST complete before the modules are unmapped: a handshake, not an event |
| `attachBasemap`, `attachTrackInfo` | the window keeps the plugin API pointers | the window calling plugin code from the GUI thread while the engine unloads it is the race `onPluginsUnloading` guards; needs the handshake |
| `showDemonstrationInstrument` | shows an instrument window | event |
| `drainTrackInfoText` | the engine pulls track-info text from the window | invert: the window pushes |
| `patchPageOpen` | the Patch page is on screen | page state the engine reads (OPEN 6) |
| `webListening` | the web server's running flag | stage 5 moves the server |
| `tunerDisplayStyle`, `basemapFacts`, `enrichWebTrack`, `fillWebImages` | the window's facts the publish copies into the web block | the publish must not call into the window from the control thread: the window must hand these over (or the web block's view-derived parts move to the window) |

## 4. The frame, before and after

Before 3a (6822eaa, all in AppWindow): `run()` per frame - display-change
bookkeeping, `applyPendingTheme`, `applyPendingLanguage`, ImGui NewFrame,
`pollGpsReader`, feature-request and problem-report polls, the test seams,
`drawUi()`, `pollConfigWriter`, `maybeSaveConfig`, render, and every 60 frames
`refreshDiagContext`. Inside `drawUi()`:

1. readings scale
2. `receiverSnapshot_->retryInstall()`
3. `drainLocalCommands()`
4. `dispatchKeyBindings()`, `drainLocalCommands()`
5. clear the transmit key flags (`transmitPttHeld_`, `transmitLatchPressed_`, `transmitPageLive_`)
6. `endTakesOnFault()`, `pumpDecoderOutput()`
7. `applyWebControls()`
8. `applyPluginApi()`, `scannerFrame()`
9. a dropped file's import (`applyCommand`)
10. `flushBookmarkSave(false)`, `publishReceiverState()` (with the web spectrum copy)
11. `publishWebAudio()`, `publishWebImages()`, `pumpWebTiles()`
12. `updateAudioMute()`
13. `drawPluginWindows()`
14. `txPageKey` -> `transmitter_.setLatched`, `setPttHeld`; `transmitter_.tick()`
15. the root window and every panel drawn; basemap `endFrame`
16. `pollSourceAsync()`, `pollSoundCard()`, `pollSoapyRecovery()`, `pollPendingRetune()`, `pollPluginAsync()`, `pumpAddAll()`
17. `pollUpdateAsync()`
18. `pollAudioOpen()`, `pollAudioHealth()`, `pollMicOpen()`, `telemetryHeartbeat_.poll(ImGui::GetTime())`

After 3a: `run()` is unchanged (it calls `engine_.pollGpsReader()` and
`engine_.refreshDiagContext()` where it did). `drawUi()` - **E** is an Engine
phase, the rest the window's own:

1. readings scale
2. **E** `pumpFrameBegin()` = retryInstall, drainLocalCommands
3. `dispatchKeyBindings()`, **E** `drainLocalCommands()`
4. clear the transmit key flags
5. **E** `pumpInputs()` = endTakesOnFault, pumpDecoderOutput
6. `applyWebControls()` (-> `engine_.applyControlRequest` per request)
7. **E** `pumpPlugins()` = applyPluginApi, scannerFrame
8. a dropped file's import (`engine_.applyCommand`)
9. **E** `pumpPublish()` = flushBookmarkSave(false), publishReceiverState -> the centre; `publishWebSpectrum(centre)`
10. `publishWebAudio()`, `publishWebImages()`, `pumpWebTiles()`
11. **E** `pumpAudioMute()` = updateAudioMute
12. `drawPluginWindows()`
13. **E** `pumpTransmitter(transmitPageLive_, transmitLatchPressed_, transmitPttHeld_)` = txPageKey, setLatched, setPttHeld, tick
14. the root window and every panel drawn; basemap `endFrame`
15. **E** `pumpWorkers()` = pollSourceAsync, pollSoundCard, pollSoapyRecovery, pollPendingRetune, pollPluginAsync, pumpAddAll
16. `pollUpdateAsync()`
17. **E** `pumpAudio()` = pollAudioOpen, pollAudioHealth, pollMicOpen, the heartbeat on `host_.frameTimeS()` (= `ImGui::GetTime()` in the window)

Every ordering comment in drawUi stayed where it was. `Engine::pump()` runs
`pollGpsReader` and then the phases in the same order for a front end with no
frame of its own (one drain - the second is the keyboard's;
`pumpTransmitter(false, false, false)` - no page, so the key is released).

**Construction** (`AppWindow::AppWindow`): configPath/announce;
`engine_.initialise()` = record folder, demo tones, display range (host),
VFO park and P7 mirrors, band plan, plugin scan, catalogue URL default, audio
and microphone gates (their watchdog hooks through the host), output device
list; then the store URL box, the web/CAT providers, the config writer's bind,
the config restore (`applyConfig` above, then `telemetryStartup` and the
window's `diagnosticsStartup`), and behind the config-path gate
`engine_.loadBookmarks()`. The providers and the config writer's bind moved
after the audio gates' bind: none of them starts anything.

**Teardown** (`AppWindow::~AppWindow`): web stop, CAT stop,
`engine_.stopTransfers()` (plugin transfer cancel), the window's update
cancel and check reap, `engine_.teardown()` = device open, sound card, audio
and microphone gates reaped, GPS stopped, Soapy scan reaped, patch radios
stopped, plugins detached and unloaded, recordings stopped - the old order
exactly. `~Engine` runs both if the front end did not (`tornDown_`), which is
what the headless test's first engine exercises. `run()`'s own teardown
sequence (transmitter stop, GPS, recordings, bookmark save, pipeline stop,
patch, plugins, clean-exit mark) is unchanged and still spelled out in
`run()` between the window's own steps (OPEN 9).

## 5. What the window may still do to the engine

The guard (tests/test_command_path_guard.cpp, section 7) holds the line
"only the Engine changes the receiver; the window submits commands", with
these reviewed exceptions:

- **window machinery** (`kWindowMachinery`): the constructor, destructor,
  `applyConfig`, `currentConfig`, `applyWebControls`, and the EngineHost
  overrides. May call the Engine; no ImGui input.
- **line-allowed members** (`kLineAllowed`): drawUi's phase calls (section
  4); `run()`'s start-up, test seams and teardown lines; drawPatchPage's
  patch runtime steps; drawTransmitPage's `followTransmitFrequency`.
- **non-const Engine methods a control may call** (`kControlMayCall`):
  applyCommand, submitCommand, telemetryNotePanel, currentConfig,
  currentAbsoluteHz, carriedAirCentre, muteNameList, refreshDiagContext,
  scanSoundCards.
- **engine fields a control still edits in place** (`kWindowMayWrite`, OPEN 1):
  25 fields at 3a; NONE since engine/stage3b-pre's OPEN 6 graph round -
  `kWindowMayWriteScoped` and `kWindowMayWriteUnscoped` are both empty.

## 6. The snapshot's hand-over (review L-a, L-b, L-c, the nit)

- **L-a**: `ReceiverSnapshot`'s hand-over is a lock-free raw-pointer slot
  (`std::atomic<std::shared_ptr<const Full>*> handoff_`). The writer
  try-locks `fullMutex_`; if a reader holds it, it EXCHANGES its block into
  the slot and deletes whatever came back (the older undelivered block). A
  reader takes the slot's block under `fullMutex_` (exchange out), installs it
  if its generation is newer, and frees it. `retryInstall()` installs a
  waiting block when the lock is free. The destructor frees a block left in
  the slot. The held-back mutex and `holdHandoffLockForTest` are gone. The
  generation check stays.
- **L-b**: the comments that said the GUI thread never waits are corrected:
  `applyControlRequest` (web/CAT row resolution) calls `readFull()`, which
  takes `fullMutex_` for a pointer copy - a short blocking lock. In 3a the GUI
  thread takes it; after 3b the control thread does (docs/engine-stage2.md
  section 5 amended).
- **L-c**: the rows comment in receiver_snapshot.hpp says rows resolve against
  the block current when the request is APPLIED.
- **nit**: `deferred_` counts failed `retryInstall` try-locks too (commented).
- `installPending()` is writer-side only (tests); never exposed to the window.
- **3b must call `retryInstall()` every pass** of the control thread's loop:
  `pumpFrameBegin()` and `pump()` do.

tests/test_snapshot_stress.cpp (adopted from the review's snap_stress.cpp,
10 s bound in ctest): one writer publishing as fast as it can, readers on
`read`, `readFull` and `retryInstall`, checking no block is lost or torn and
nothing leaks. On this machine: publish max 0.139 ms, retryInstall max 0.028
ms, a writer-side readFull max 0.287 ms. Four mutants of receiver_snapshot.cpp
were each red in both test_receiver_snapshot and the stress test: the writer
dropping its block (lost), the reader never taking the slot (lost), the
writer keeping the returned block (payload leak), readFull without the lock
(test_receiver_snapshot crashed 0xC0000005, stress saw torn ids); the source
restored, both green.

## 7. Tests

**Changed mechanically** (each follows code to where it now lives; no
assertion loosened): test_apply_command, test_bias_key_app,
test_converter_app_paths, test_snapshot_app, test_soundcard_app_paths,
test_state_snapshot_golden (`a.engine_.x`, `Engine::testHooks_`);
test_converter_call_sites, test_stop_ends_recordings, test_shutdown_budget,
test_soundcard_source, test_diagnostics (the source scans read src/engine and
`Engine::` heads); test_transmit_page section 8 (the key's application is
read in `Engine::pumpTransmitter`, the window must have none of it and call the
phase once after drawPluginWindows - seen red by moving the call before
drawPluginWindows); test_audio_open (the "no microphone opened on the calling
thread" scan reads src/engine too); include paths in the helper tests
(test_bench_text_fits, test_bias_key, test_bias_tee_rx888, test_config,
test_device_scan_plan, test_mute_banner, test_preset_bar, test_rate_coercion,
test_rate_follow, test_rtl_bias_tee, test_running_view, test_source_fallback,
test_theme, test_tune_control, test_user_presets); test_receiver_snapshot [2]
rewritten for the slot (section 6).

**test_command_path_guard** reads the Engine's surface from src/engine (every
`Engine::x` definition, which are `const` in the header, every data member),
treats `engine_.` as absent, and judges every window definition: a control
may not call a state-changing helper, a window-machinery member or a non-const
Engine method (except `kControlMayCall`), may not call a mutating method on an
engine object, may not write any engine field outside `kWindowMayWrite`, and
src/engine has no ImGui at all. At HEAD: 174 engine methods (38 const), 242
fields, 0 ImGui uses, 212 controls judged, 0 violations. Seen red at HEAD,
each probe inserted at the top of `drawToolbar` and the source restored after:
`engine_.volume_ = 0.3f;` (field write volume_), `engine_.pipeline_.stop();`
(engine call pipeline_.stop), `engine_.pumpWorkers();`,
`engine_.applyConfig(...)`, `engine_.teardown();` (helper ...) - each 1 failed.

**tests/test_engine_headless.cpp** (new): constructs `Engine` with no window,
links cascade_engine ALONE, drives it with `applyCommand`/`submitCommand`,
`pump()`, `applyConfig`/`fillConfig`, and reads the published snapshot. Checks:
[1] the first pump publishes, not running; [2] volume 0, SELECT_SOURCE siggen,
RUN 1, snapshot running; [3] SET_FREQUENCY 100.3 MHz, tunedHz; [4] a submitted
SET_MODE AM changes nothing until one pump; [5] RECORD_AUDIO 1/0: the flag,
then one WAV in the scratch folder (142094 bytes after 1.5 s); [6] fillConfig
writes AM, volume 0, siggen; [7] a fresh engine restores USB, 145.5 MHz, 0.25
from an AppConfig; [8] explicit stopTransfers/teardown (the first engine is
destroyed without them). 30 checks, 0 failed, 2.7 s. Red evidence (each mutant
of engine.cpp built after deleting the exe - hash changed each time - run,
source restored byte-exact, rebuilt, green):

| mutant | failed |
|--------|--------|
| pumpPublish returns without publishing | 8: [1] published, [2] running, [3] tuned, [4] mode, [5] recording flag, [7] mode/centre/volume |
| pumpFrameBegin without its drain | 2: [4] mode, [6] mode |
| initialise's record folder elsewhere | 2: [5] no take, no bytes |
| fillConfig without the mode line | 1: [6] |
| applyConfig without the generator's centre | 1: [7] centre |

(MSVC stamps the PE header, so the restored exe never hashes like the first
green one; the restore is proven by comparing the source with its backup and
by the green run.)

**Runs.**
- Windows (MSVC Release, build-win in the worktree): after commit 7,
  `ctest -E "^(test_rtlsdr_live|test_web_server)$" -j 8`: **212/212 passed**
  (test_engine_headless included); `test_web_server` alone: passed (3.9 s).
  Suites between the big steps: 210/210 before commit 1 and after it;
  211/211 after commit 2; after commit 3 210/211 (the guard, before its
  stage-3a rules) then the guard alone 152/0; 211/211 after commit 4; after
  commits 5 and 6 the source-reading tests still pointing at the moved code
  failed (209/211, 210/211) and passed alone after their mechanical update
  (each commit message records which).
- Linux (WSL Ubuntu, gcc, Ninja Release, native clone of the branch;
  `WAYLAND_DISPLAY=nope LIBGL_ALWAYS_SOFTWARE=1 xvfb-run -a ctest -j 8`):
  build clean of new warnings (the gcc warnings in app_window.cpp -
  format-security, format-truncation, an unused `n` in the input debug line -
  are in code 3a did not write). HEAD: 7 full runs; the first was **213/214,
  `test_bias_key_run` failed**, the other six **214/214**; `test_bias_key_run`
  alone 16/16. The failing run's output was not kept (the log was written
  without --output-on-failure and the rerun overwrote LastTest.log), so the
  cause is NOT known. The base f598bc0: 6 full runs, 212/212 each. The test
  starts the real application under xvfb with software GL and clicks through
  an input script by frame number; a load-timing flake is the likely reading,
  a 3a regression is not ruled out (section 9, OPEN 16).

## 8. The GUI-THREAD-ONLY checklist for 3b

Every comment line in src/ that states or leans on "the GUI thread" (274
lines, generated by scratchpad engine4/gen_checklist.py - a regex, so the
contract/mention split is a heuristic: a **contract** line says only / must /
may / never, or opens with "GUI thread." as a caller rule). In 3b each engine
line becomes CONTROL-THREAD-ONLY (or is re-examined, where the point was "not
the audio/DSP thread"); each src/gui line either stays true (views, which stay
on the GUI thread) or names engine state the window now reads across threads.
No thread comment was rewritten for the control thread in 3a (the L-b
corrections in section 6 are the only thread comments changed).

#### src/engine (37 lines, 7 stated as a contract)

- [ ] `src/engine/audio_open.hpp:6` (mention) an output device in the Sinks panel; the GUI thread went
- [ ] `src/engine/audio_open.hpp:16` (mention) own log records the GUI thread coming back 57 seconds later. Nothing had
- [ ] `src/engine/audio_open.hpp:33` (mention) (AppWindow::pollSourceAsync). The GUI thread therefore cannot be inside
- [ ] `src/engine/engine.cpp:188` (mention) destructor run somewhere that is not the GUI thread.
- [ ] `src/engine/engine.cpp:838` (mention) so quitting while a device open was in flight parked the GUI thread
- [ ] `src/engine/engine.cpp:882` (mention) lazy scan was in flight parked the GUI thread inside ~AppWindow for the
- [ ] `src/engine/engine.cpp:907` (mention) so the wait happens off the GUI thread. If the process exits first the
- [ ] `src/engine/engine.cpp:1135` (mention) it is converted here, on the GUI thread that owns the converter table,
- [ ] `src/engine/engine.cpp:1193` (mention) return std::move(r);  // r.dev stays null: the GUI thread reports it
- [ ] `src/engine/engine.cpp:1340` (mention) that is not there spends its connect bound off the GUI thread and the
- [ ] `src/engine/engine.cpp:1591` (mention) GUI thread, inline, whenever the list might be stale, including while a
- [ ] `src/engine/engine.cpp:2142` (mention) this application actually has on its GUI thread: unloading and then
- [ ] `src/engine/engine.cpp:3226` (mention) Everything read here is read on the GUI thread, which is the contract
- [ ] `src/engine/engine.cpp:4098` (mention) on the GUI thread for a Stop.
- [ ] `src/engine/engine.cpp:4623` (**contract**) GUI thread.
- [ ] `src/engine/engine.cpp:4661` (mention) thread). Everything read here is read on the GUI thread, which is the
- [ ] `src/engine/engine.cpp:5662` (**contract**) were (net/control_ops.cpp), by applyCommand. GUI thread only.
- [ ] `src/engine/engine.cpp:5670` (mention) LOCK taken by the publishing thread itself (the GUI thread in stage 3a;
- [ ] `src/engine/engine.cpp:6479` (mention) frame of its own (tests/test_engine_headless). GUI thread in stage 3a; the
- [ ] `src/engine/engine.hpp:20` (mention) GUI thread, exactly where the window called it before (the per-frame order
- [ ] `src/engine/engine.hpp:22` (**contract**) control thread; every GUI-THREAD-ONLY note in the code below is a checklist
- [ ] `src/engine/engine.hpp:161` (mention) The GUI-thread half of an open, wherever it completed: republishes the
- [ ] `src/engine/engine.hpp:270` (**contract**) their marks. GUI thread - the only thread that may touch the receiver,
- [ ] `src/engine/engine.hpp:292` (**contract**) any other way. GUI thread only - the same rule every setter it calls
- [ ] `src/engine/engine.hpp:325` (mention) GUI thread, like applyCommand.
- [ ] `src/engine/engine.hpp:445` (mention) Opens or closes the transmit radio. Bounded, and on the GUI thread -
- [ ] `src/engine/engine.hpp:788` (mention) itself is only ever touched by the GUI thread once the future resolves,
- [ ] `src/engine/engine.hpp:826` (mention) converter on the GUI thread by launchDeviceOpen - the worker has no
- [ ] `src/engine/engine.hpp:889` (mention) Drains a pending device open OFF the GUI thread at shutdown. See the
- [ ] `src/engine/engine.hpp:999` (mention) Applies a resolved open on the GUI thread (panel mirrors, gain priming,
- [ ] `src/engine/engine.hpp:1192` (mention) enough to run on the GUI thread.
- [ ] `src/engine/engine.hpp:1480` (**contract**) GUI thread: rebuilds the two lists above from pluginHost_.
- [ ] `src/engine/engine.hpp:1810` (mention) protected. It is done off the GUI thread because it re-hashes every
- [ ] `src/engine/engine.hpp:1961` (**contract**) Its two halves. Every read is a GUI-thread read, which is the contract
- [ ] `src/engine/engine.hpp:1994` (mention) ~Engine does both for a front end that did not. GUI thread in 3a.
- [ ] `src/engine/engine_patch_radios.cpp:415` (mention) Converted HERE, on the GUI thread that owns the remembered settings;
- [ ] `src/engine/engine_patch_radios.cpp:636` (mention) seconds and this is the GUI thread. Its future moves to a thread of its

#### src/core (117 lines, 20 stated as a contract)

- [ ] `src/core/config.hpp:1019` (mention) (0.96.3): the GUI thread's periodic debounced save (AppWindow::
- [ ] `src/core/config.hpp:1025` (mention) GUI thread (engine/audio_open.hpp): the write moves to a worker, and the
- [ ] `src/core/config.hpp:1027` (mention) steps so the CHEAP one (building the JSON) can stay on the GUI thread -
- [ ] `src/core/config.hpp:1045` (mention) atomicity guarantee, on a thread that is not the GUI thread's.
- [ ] `src/core/feature_request.hpp:3` (mention) reaches foxsdr.com without the GUI thread ever touching a socket.
- [ ] `src/core/feature_request.hpp:38` (**contract**) poll() is called once a frame from the GUI thread and only ever reads that
- [ ] `src/core/gps_reader.hpp:217` (mention) it)" - the open cost and its failure both live off the GUI thread that
- [ ] `src/core/hang_watchdog.cpp:418` (mention) Rule 2c's one fact: the module a stalled GUI thread is executing in when it
- [ ] `src/core/hang_watchdog.cpp:657` (mention) to accuse the GUI thread of losing, it cannot tell the two apart -
- [ ] `src/core/hang_watchdog.cpp:684` (mention) flag is set the GUI thread is inside stop() joining this thread. A
- [ ] `src/core/hang_watchdog.cpp:748` (mention) module. This application calls LoadLibrary from the GUI thread (a plugin
- [ ] `src/core/hang_watchdog.hpp:12` (mention) pair. "GUI thread is blocked in WaitForSingleObject" says nothing; "GUI
- [ ] `src/core/hang_watchdog.hpp:91` (mention) 1. A DEBUGGER at a breakpoint stops the GUI thread for as long as the
- [ ] `src/core/hang_watchdog.hpp:102` (mention) enough for Windows 11 to offer its snap layouts, parks the GUI thread
- [ ] `src/core/hang_watchdog.hpp:111` (mention) PeekMessage, and the only way its GUI thread can sit inside win32u's
- [ ] `src/core/hang_watchdog.hpp:119` (mention) LoadLibrary-s every installed plugin on the GUI thread. Twelve modules
- [ ] `src/core/hang_watchdog.hpp:121` (mention) later that blocks the GUI thread - a synchronous device open, a native
- [ ] `src/core/hang_watchdog.hpp:159` (mention) for, the GUI thread is inside it joining this thread; it is not
- [ ] `src/core/hang_watchdog.hpp:207` (mention) lock, waited for by SoapySource::stop() on the GUI thread;
- [ ] `src/core/hang_watchdog.hpp:257` (mention) debounced save used to write the file synchronously ON the GUI thread,
- [ ] `src/core/hang_watchdog.hpp:263` (mention) off the GUI thread and gives the LAST one a single bounded, charged
- [ ] `src/core/hang_watchdog.hpp:301` (mention) filed against its own clean shutdown. Called once, from the GUI thread,
- [ ] `src/core/hang_watchdog.hpp:344` (mention) leaves no file. Called from the GUI thread while the watchdog thread is
- [ ] `src/core/hang_watchdog.hpp:349` (mention) Called once per rendered frame from the GUI thread. One relaxed store;
- [ ] `src/core/hang_watchdog.hpp:383` (mention) TEST HOOK for rule 2c. Where a stalled GUI thread's instruction pointer
- [ ] `src/core/hang_watchdog.hpp:451` (mention) Rule 2c: whether the stalled GUI thread is parked in the window
- [ ] `src/core/hang_watchdog.hpp:453` (mention) one is set, otherwise from one register read of the GUI thread.
- [ ] `src/core/hang_watchdog.hpp:479` (mention) ATOMIC because beginShutdown() raises it from the GUI thread while the
- [ ] `src/core/hang_watchdog.hpp:501` (mention) ATOMIC because heartbeat() (the GUI thread) writes it and the watchdog
- [ ] `src/core/patch_audio.hpp:22` (mention) (the sets own it by shared_ptr and die on the GUI thread; see
- [ ] `src/core/patch_plan.hpp:172` (mention) megabytes of filter per decoder, designed on the GUI thread.
- [ ] `src/core/patch_radio.hpp:13` (mention) this thread adopts them, and retired sets die on the GUI thread through
- [ ] `src/core/patch_radio.hpp:74` (mention) The device's own readback. GUI thread.
- [ ] `src/core/patch_radio.hpp:80` (mention) converted one. GUI thread; sources accept a retune while their reader
- [ ] `src/core/patch_radio.hpp:92` (**contract**) GUI thread; the reader thread is its DSP thread.
- [ ] `src/core/patch_runner.hpp:6` (mention) patch on the GUI thread while the DSP thread is halfway through a block, and
- [ ] `src/core/patch_runner.hpp:12` (mention) THE GUI THREAD BUILDS. THE DSP THREAD ADOPTS AND OWNS.
- [ ] `src/core/patch_runner.hpp:14` (mention) publish() is called from the GUI thread with a set that is already complete -
- [ ] `src/core/patch_runner.hpp:18` (mention) the only thread that touches it. The GUI thread never reads or writes a set
- [ ] `src/core/patch_runner.hpp:22` (mention) WHY NOT A MUTEX. A lock held on the audio thread is a lock the GUI thread can
- [ ] `src/core/patch_runner.hpp:23` (mention) make it wait on, and a GUI thread that is swapping a patch is doing
- [ ] `src/core/patch_runner.hpp:25` (**contract**) stack was the GUI thread blocked in a write; the audio thread must never be
- [ ] `src/core/patch_runner.hpp:33` (mention) EVERY SET DIES ON THE GUI THREAD. A set the DSP thread stops running -
- [ ] `src/core/patch_runner.hpp:36` (mention) called by the GUI thread every frame, destroys it. A set that was published
- [ ] `src/core/patch_runner.hpp:38` (**contract**) clear(), which are GUI-thread calls anyway. So no set's destructor ever runs
- [ ] `src/core/patch_runner.hpp:44` (mention) it is called from the GUI thread when the patch page closes - so closing the
- [ ] `src/core/patch_runner.hpp:125` (mention) StripSet only ever dies on the GUI thread, after the DSP thread has stopped
- [ ] `src/core/patch_runner.hpp:203` (mention) rounding. Built here, on the GUI thread, with everything else.
- [ ] `src/core/patch_runner.hpp:234` (**contract**) Builds the set a plan describes. GUI THREAD ONLY - it allocates.
- [ ] `src/core/patch_runner.hpp:239` (**contract**) Builds the decoder instances a plan describes into 'set'. GUI THREAD ONLY:
- [ ] `src/core/patch_runner.hpp:478` (**contract**) speaker on it that has an output. GUI THREAD ONLY. Nothing belonging to
- [ ] `src/core/patch_runner.hpp:545` (**contract**) GUI THREAD. Hands a complete set over. Returns immediately; the DSP
- [ ] `src/core/patch_runner.hpp:567` (**contract**) never waits on the GUI thread except in the instant a patch actually
- [ ] `src/core/patch_runner.hpp:568` (mention) changed, where the GUI thread holds the mutex for one pointer move.
- [ ] `src/core/patch_runner.hpp:615` (mention) GUI THREAD, and SYNCHRONOUS. Stops the patch and destroys every set
- [ ] `src/core/patch_runner.hpp:663` (**contract**) GUI THREAD. A channel's squelch threshold (kSquelchOffDb for off), taken
- [ ] `src/core/patch_runner.hpp:760` (mention) THE OLD SET GOES BACK TO THE GUI THREAD, not to the
- [ ] `src/core/patch_runner.hpp:764` (mention) allocate on the audio thread in practice; if the GUI thread
- [ ] `src/core/patch_runner.hpp:1050` (**contract**) GUI THREAD. Every line the patch's decoders have finished since the last
- [ ] `src/core/patch_runner.hpp:1059` (**contract**) GUI THREAD. The newest picture from each picture decoder since the last
- [ ] `src/core/patch_runner.hpp:1140` (**contract**) GUI THREAD. Asks the DSP thread to stop running the patch at its next
- [ ] `src/core/patch_runner.hpp:1158` (mention) GUI THREAD, every frame, whether or not the patch page is open - the
- [ ] `src/core/patch_runner.hpp:1182` (mention) Written by the GUI thread under the mutex, read by the DSP thread.
- [ ] `src/core/patch_runner.hpp:1188` (mention) the DSP thread and swapped out by the GUI thread, both under the mutex.
- [ ] `src/core/pipeline.cpp:1617` (mention) set the GUI thread has published, so a rewire takes effect on a block
- [ ] `src/core/pipeline.hpp:75` (mention) audio callback 110 times a minute) is exactly that: the GUI thread stalled
- [ ] `src/core/pipeline.hpp:195` (mention) thread called stop() — the GUI thread, straight out of the toolbar's
- [ ] `src/core/pipeline.hpp:421` (mention) Atomic because it is written from the GUI thread and read by the DSP
- [ ] `src/core/pipeline.hpp:430` (mention) THE PATCH. The GUI thread builds a set of strips and publishes it;
- [ ] `src/core/pipeline.hpp:433` (**contract**) thread must never be able to queue behind a GUI thread that is
- [ ] `src/core/pipeline.hpp:449` (mention) GUI thread for 57 seconds. Call it directly only where blocking is
- [ ] `src/core/pipeline.hpp:468` (mention) from the GUI thread when an asynchronous open completes; 'ok' false
- [ ] `src/core/pipeline.hpp:917` (mention) whole block, and a GUI thread that had to take it to set one bool would
- [ ] `src/core/plugin_abi.h:765` (mention) on permanent failure. Called from the GUI thread at frame rate, so it
- [ ] `src/core/plugin_abi.h:830` (mention) failure. GUI thread, at frame rate: cheap, non-blocking, no I/O. */
- [ ] `src/core/plugin_abi.h:972` (mention) negative on permanent failure. GUI thread, at frame rate: cheap,
- [ ] `src/core/plugin_abi.h:1253` (mention) EVERY level-1 function may be called from ANY thread - the GUI thread, the
- [ ] `src/core/plugin_abi.h:1260` (mention) the change itself is queued and applied by the host's GUI thread at the
- [ ] `src/core/plugin_abi.h:1267` (mention) get_stream_info read a snapshot the GUI thread publishes each frame
- [ ] `src/core/plugin_abi.h:1274` (mention) so no call can make the DSP or GUI thread wait on plugin work. The
- [ ] `src/core/plugin_abi.h:1552` (mention) ---- Receiver control: REQUESTS, applied on the GUI thread's next
- [ ] `src/core/plugin_abi.h:1738` (mention) the GUI thread, never in real time, and not tied to any instance - a
- [ ] `src/core/plugin_abi.h:1774` (mention) rendered frame, on the GUI thread. A plugin that fetches over a network must
- [ ] `src/core/plugin_abi.h:1859` (mention) get_info MUST NOT BLOCK. It is called on the GUI thread, per frame, for
- [ ] `src/core/plugin_api.hpp:11` (mention) place that publishes the snapshot and drains the queues, on its GUI thread.
- [ ] `src/core/plugin_api.hpp:14` (mention) these from any thread - its own worker, the GUI thread, the real-time DSP
- [ ] `src/core/plugin_api.hpp:25` (mention) waiting for the GUI thread.
- [ ] `src/core/plugin_api.hpp:172` (mention) A message a plugin logged, drained by the GUI thread.
- [ ] `src/core/plugin_api.hpp:219` (mention) Find-or-create the client for a module. GUI thread.
- [ ] `src/core/plugin_api.hpp:256` (mention) Drains. GUI thread. Each empties its queue into 'out' (appended).
- [ ] `src/core/plugin_repo.cpp:617` (mention) the GUI thread that called it.
- [ ] `src/core/plugin_runner.cpp:613` (mention) audio thread and emptied by the GUI thread; a GUI that has stopped
- [ ] `src/core/plugin_runner.hpp:15` (mention) drainText()         GUI thread, once per frame.
- [ ] `src/core/plugin_runner.hpp:16` (mention) pollImages()        GUI thread, once per frame.
- [ ] `src/core/plugin_runner.hpp:24` (**contract**) requires. That is enforced by never touching instances_ from the GUI thread
- [ ] `src/core/plugin_runner.hpp:270` (**contract**) GUI thread. Writes the diagnostics the audio path recorded: one line per
- [ ] `src/core/plugin_runner.hpp:281` (**contract**) GUI thread. Moves out whatever has been decoded since the last call.
- [ ] `src/core/plugin_runner.hpp:284` (**contract**) GUI thread. Refreshes 'out' from the image decoders: one entry per image
- [ ] `src/core/plugin_runner.hpp:300` (**contract**) GUI thread. What each loaded decoder is doing, or why it is not.
- [ ] `src/core/plugin_runner.hpp:422` (mention) the GUI thread. FIXED STORAGE and a fixed-size array, because the thread
- [ ] `src/core/plugin_runner.hpp:537` (**contract**) reports the minute rather than the session. GUI thread only.
- [ ] `src/core/plugin_ui.cpp:1417` (mention) a rescan replacing the services on the GUI thread can neither tear the
- [ ] `src/core/plugin_ui.hpp:5` (mention) lock; everything here is created, polled and destroyed on the GUI thread and
- [ ] `src/core/plugin_ui.hpp:836` (mention) GUARDED, because a plugin does not only call these from the GUI thread.
- [ ] `src/core/plugin_ui.hpp:837` (mention) The ABI documents the host table for the GUI thread, but Satellites
- [ ] `src/core/plugin_ui.hpp:838` (mention) 1.0.1 read the clock from its own background worker, and the GUI thread
- [ ] `src/core/receiver_snapshot.hpp:18` (mention) GUI thread in stage 2, the engine's control thread from stage 3). Readers
- [ ] `src/core/receiver_snapshot.hpp:40` (mention) since engine stage 3; the GUI thread in 3a, the engine's control thread
- [ ] `src/core/recorder.hpp:179` (mention) Atomic because the DSP thread latches it and the GUI thread reads it
- [ ] `src/core/retune_coalescer.hpp:16` (mention) the GUI thread, where all tuning already lives.
- [ ] `src/core/transmitter.cpp:588` (mention) The GUI thread stamps lastTickMs_ every frame. If it has stopped,
- [ ] `src/core/transmitter.hpp:21` (mention) - A FROZEN WINDOW CANNOT LEAVE IT KEYED. The GUI thread calls tick()
- [ ] `src/core/transmitter.hpp:192` (mention) THE DEAD-MAN'S HANDLE. If the GUI thread has not called tick() within
- [ ] `src/core/transmitter.hpp:230` (mention) i.e. from the GUI thread, between frames. For the panel's readouts.
- [ ] `src/core/transmitter.hpp:328` (mention) ONCE A FRAME, FROM THE GUI THREAD. Applies the key request, enforces
- [ ] `src/core/transmitter.hpp:353` (mention) the GUI thread, so it is snapshotted rather than handed out by pointer.
- [ ] `src/core/updater.hpp:136` (mention) The once-per-launch update check, run off the GUI thread, AND ABANDONABLE.

#### src/source (54 lines, 4 stated as a contract)

(`src/source/airspy_source.cpp`/`.hpp`'s four line references regenerated
2026-09-28, engine/stage3b-pre: 0.99.41's Airspy R2/Mini gain modes,
decimation and channel filter shifted the file by ~130 lines; the same four
comments, the same mention/contract split, only the numbers moved.)

- [ ] `src/source/airspy_source.cpp:833` (mention) back from - and waiting for it on the GUI thread would be the hang
- [ ] `src/source/airspy_source.cpp:904` (**contract**) helpers want devMutex_, which the GUI thread may be holding while it
- [ ] `src/source/airspy_source.hpp:22` (mention) rather than waited for, because a hang on the GUI thread is worse than a
- [ ] `src/source/airspy_source.hpp:597` (**contract**) on the reader thread and WITHOUT devMutex_ (the GUI thread may hold it
- [ ] `src/source/airspyhf_source.cpp:883` (mention) back from - and waiting for it on the GUI thread would be the hang
- [ ] `src/source/airspyhf_source.hpp:25` (mention) GUI thread is worse than a leak. An abandoned reader must still have
- [ ] `src/source/airspyhf_source.hpp:41` (mention) WHICH IS WHY THE RETUNE PATH LOOKS ODD. The GUI thread computes a tune and
- [ ] `src/source/device_source.hpp:14` (mention) Every method here is safe to call from the GUI thread while the reader
- [ ] `src/source/hackrf_source.cpp:621` (mention) back from - and waiting for it on the GUI thread would be the hang
- [ ] `src/source/hackrf_source.hpp:21` (mention) GUI thread is worse than a leak. An abandoned reader must still have
- [ ] `src/source/iiod_client.cpp:276` (mention) park the GUI thread for the operating system's own retry schedule -
- [ ] `src/source/iq_file_source.cpp:89` (mention) or block on demand, and open() runs on the GUI thread where the blocking
- [ ] `src/source/iq_file_source.cpp:121` (mention) BOUNDED, because this loop runs on the GUI thread and its trip count was
- [ ] `src/source/iq_file_source.cpp:173` (mention) the trailing-metadata reads of a broadcast WAV off the GUI thread.
- [ ] `src/source/iq_file_source.hpp:107` (mention) source thread sets it and the GUI thread reads it.
- [ ] `src/source/iq_file_source.hpp:111` (mention) open() runs on the GUI thread and the walk's trip count was set by the
- [ ] `src/source/mirisdr_source.cpp:502` (mention) from - and waiting for it on the GUI thread would be the hang the
- [ ] `src/source/mirisdr_source.hpp:29` (mention) a hang on the GUI thread is worse than a leak. An abandoned reader must
- [ ] `src/source/msi2500.hpp:138` (mention) control transfer is spent on the GUI thread during a teardown - the
- [ ] `src/source/pluto_source.cpp:746` (mention) back from - and waiting for it on the GUI thread would be the hang
- [ ] `src/source/pluto_source.hpp:41` (mention) a wedged reader, because a hang on the GUI thread is worse than a leak. An
- [ ] `src/source/pluto_tx.cpp:784` (mention) waiting for it on the GUI thread would be the hang the bounds exist
- [ ] `src/source/pluto_tx.hpp:80` (mention) the GUI thread is worse than a leak. So everything the writer touches - the
- [ ] `src/source/rx888_source.cpp:759` (mention) back from - and waiting for it on the GUI thread would be the hang
- [ ] `src/source/rx888_source.hpp:38` (mention) because a hang on the GUI thread is worse than a leak. Everything the
- [ ] `src/source/sdrplay_source.cpp:741` (mention) kEnumerateWait of GUI thread and abandon another worker inside it.
- [ ] `src/source/sdrplay_source.cpp:1608` (mention) stop() - on the GUI thread, in the same frame, straight into the Uninit
- [ ] `src/source/sdrplay_source.cpp:1661` (mention) and the GUI thread never came back from that ReleaseDevice.
- [ ] `src/source/sdrplay_source.cpp:1843` (mention) five seconds to answer ServiceNotResponding on the GUI thread, which is
- [ ] `src/source/sdrplay_source.cpp:2640` (mention) devMutex_: a GUI-thread control can hold it for up to kControlWait +
- [ ] `src/source/sdrplay_source.hpp:71` (mention) report is a GUI thread inside stopStreamingLocked's sdrplay_api_Uninit,
- [ ] `src/source/sdrplay_source.hpp:198` (mention) the GUI thread, which is where scanNative() runs - went into sessionAcquire
- [ ] `src/source/sdrplay_source.hpp:215` (mention) another three seconds of GUI thread and leak another abandoned worker into a
- [ ] `src/source/sdrplay_source.hpp:365` (mention) hang watchdog's whole frame threshold, spent on the GUI thread because
- [ ] `src/source/sdrplay_source.hpp:382` (mention) control can now cost the GUI thread is kControlWait + kUpdateWait =
- [ ] `src/source/sdrplay_source.hpp:848` (**contract**) must not queue behind a GUI-thread control in flight.
- [ ] `src/source/soapy_log_bridge.cpp:92` (mention) Called by SoapySDR from whichever thread logged - the GUI thread during an
- [ ] `src/source/soapy_source.cpp:65` (mention) How long a GUI-thread control call will wait for the driver lock before it
- [ ] `src/source/soapy_source.cpp:212` (mention) deactivateStream on the GUI thread. For an RTL-SDR that is
- [ ] `src/source/soapy_source.cpp:1211` (mention) covers the second. It cannot cover the third, and a hang on the GUI thread is
- [ ] `src/source/soapy_source.cpp:1240` (mention) closing the radio - the GUI thread, out of the Source panel -
- [ ] `src/source/soapy_source.cpp:1428` (mention) which is the GUI thread out of the toolbar, and it is the exact call
- [ ] `src/source/soapy_source.cpp:1467` (mention) is the GUI thread: Pipeline::stop() calls it straight out of the
- [ ] `src/source/soapy_source.cpp:1571` (mention) the GUI thread, as it was, there is nothing left to give up: the
- [ ] `src/source/soapy_source.cpp:2036` (mention) Lock-free, like centerFrequencyHz() — this is read on the GUI thread
- [ ] `src/source/soapy_source.hpp:13` (mention) takes devMutex_ for the duration of the driver call, so a GUI-thread retune
- [ ] `src/source/soapy_source.hpp:44` (**contract**) never came back, on the GUI thread, out of stopLocked(). So the VENDOR CALL
- [ ] `src/source/soapy_source.hpp:688` (mention) GUI thread on every frame it draws the Source section, and errorMutex_
- [ ] `src/source/soundcard_source.cpp:860` (mention) ever, on a card that has gone - and this runs on the GUI thread (a
- [ ] `src/source/soundcard_source.hpp:75` (mention) on the GUI thread (a source switch, a patch radio switched off, the exit).
- [ ] `src/source/soundcard_source.hpp:269` (mention) it off the GUI thread.
- [ ] `src/source/soundcard_source.hpp:292` (mention) GUI thread costs nothing and waits on nothing). The list it enumerates is
- [ ] `src/source/soundcard_source.hpp:432` (mention) stream at the matching card rate (blocking - off the GUI thread). A
- [ ] `src/source/soundcard_source.hpp:447` (mention) run on the GUI thread (Pipeline::start/stop). The stream is closed by

#### src/sink (11 lines, 1 stated as a contract)

- [ ] `src/sink/audio_in.hpp:118` (mention) Read from the GUI thread; written by the realtime callback, which is
- [ ] `src/sink/audio_in.hpp:132` (mention) key-down, and the benefit is that a GUI-thread hiccup does not punch a
- [ ] `src/sink/audio_out.cpp:171` (mention) open: ~AudioOut runs on the GUI thread at exit.
- [ ] `src/sink/audio_out.hpp:176` (mention) the callback, so the GUI thread can move a slider while audio runs
- [ ] `src/sink/audio_out.hpp:223` (mention) samples). Deep enough to ride out GUI-thread hiccups on the producer
- [ ] `src/sink/pa_init.hpp:8` (mention) GUI thread and from workers alike; two of those at once is a race on that
- [ ] `src/sink/pa_init.hpp:15` (**contract**) could be holding is a lock the GUI thread must never wait on (the fault
- [ ] `src/sink/pa_init.hpp:44` (mention) THE GUI THREAD NEVER WAITS FOR IT. The audio output's open is not always on
- [ ] `src/sink/pa_init.hpp:50` (mention) same way, NoWait (~AudioOut can run on the GUI thread at exit): a close
- [ ] `src/sink/pa_init.hpp:52` (mention) held goes ahead rather than make the GUI thread wait.
- [ ] `src/sink/pa_init.hpp:82` (mention) NoWait,  // only if it is free right now (anything that may be the GUI thread)

#### src/net (15 lines, 4 stated as a contract)

- [ ] `src/net/cat_protocol.hpp:24` (mention) request for the GUI thread. cat_server.cpp is the thin transport around it.
- [ ] `src/net/cat_protocol.hpp:53` (mention) thread and the pipeline belongs to the GUI thread. Same discipline as
- [ ] `src/net/cat_server.cpp:105` (mention) from the GUI thread, so "for ever" would be a frozen application.
- [ ] `src/net/cat_server.cpp:375` (mention) the GUI thread. This timeout is what bounds stop() to one interval.
- [ ] `src/net/cat_server.cpp:453` (**contract**) OLDEST request is dropped, because a stalled GUI thread must
- [ ] `src/net/cat_server.hpp:14` (mention) requests are QUEUED for the GUI thread exactly as the browser's are: this
- [ ] `src/net/cat_server.hpp:47` (mention) Bound on the queue the GUI thread drains, and deliberately the same number
- [ ] `src/net/cat_server.hpp:49` (**contract**) application that stops draining — a stalled GUI thread — must not let a
- [ ] `src/net/control_ops.hpp:32` (mention) (applyControlRequest translates and applies in one call on the GUI thread).
- [ ] `src/net/web_audio.hpp:25` (mention) THREADING. One writer (the application's GUI thread) and any number of
- [ ] `src/net/web_control.hpp:5` (mention) several of the things a browser wants to change are documented GUI-THREAD-
- [ ] `src/net/web_control.hpp:8` (mention) GUI thread drains the queue and applies it on the next frame. That split is
- [ ] `src/net/web_server.hpp:500` (**contract**) browser can ask for are GUI-thread-only. The application drains this
- [ ] `src/net/web_server.hpp:535` (**contract**) may only be called on the GUI thread (the ABI promises nothing about any
- [ ] `src/net/web_server.hpp:550` (mention) oldest first; empties the queue. GUI thread, once per frame.

#### src/gui (40 lines, 12 stated as a contract)

- [ ] `src/gui/app_window.cpp:691` (mention) the snapshot the GUI thread publishes each frame — see the note in
- [ ] `src/gui/app_window.cpp:880` (mention) vsync: the GUI thread paces itself off the display. FOXSDR_VSYNC_OFF is
- [ ] `src/gui/app_window.cpp:1251` (mention) the GUI thread, through the one door. Before the UI is drawn so the
- [ ] `src/gui/app_window.cpp:2290` (mention) rail has always been drawn in; nothing here can nest, and the GUI thread is
- [ ] `src/gui/app_window.cpp:2394` (**contract**) one key, and the GUI thread is the only thread that draws.
- [ ] `src/gui/app_window.cpp:6556` (mention) answered - so this line legitimately blocks the GUI thread for as long as
- [ ] `src/gui/app_window.cpp:10216` (**contract**) on the GUI thread, which is the only place a plugin handle inside it
- [ ] `src/gui/app_window.cpp:12067` (mention) the plugin's runs here, on the GUI thread's frame.
- [ ] `src/gui/app_window.cpp:14984` (mention) room nobody asked it to), and NOT ON THE GUI THREAD's own
- [ ] `src/gui/app_window.hpp:696` (mention) the GUI thread for as long as the shell takes, which for an elevation or
- [ ] `src/gui/app_window.hpp:1148` (mention) to write the file directly, on the GUI thread, inside whichever of
- [ ] `src/gui/app_window.hpp:1348` (mention) put the GUI thread inside the file write itself. See
- [ ] `src/gui/app_window.hpp:2274` (**contract**) two of the things a browser wants are documented GUI-THREAD-ONLY:
- [ ] `src/gui/app_window.hpp:2282` (mention) lock the GUI thread ever waits for.
- [ ] `src/gui/app_window.hpp:2304` (**contract**) keeps the plugin on the GUI thread, the only thread the ABI lets call
- [ ] `src/gui/app_window.hpp:2319` (mention) Called once per frame from drawUi, on the GUI thread, because that is
- [ ] `src/gui/basemap_cache.cpp:185` (mention) the GUI thread, once per frame, and the loop used to run until the
- [ ] `src/gui/basemap_cache.hpp:65` (mention) called from the GUI thread), and the same start-the-fetch semantics:
- [ ] `src/gui/basemap_cache.hpp:87` (mention) from owning the GUI thread for ever.
- [ ] `src/gui/config_writer.hpp:1` (mention) config_writer.hpp - saving the config file without blocking the GUI thread.
- [ ] `src/gui/config_writer.hpp:5` (mention) tail shows a mode change, two "gui thread recovered after a stall"
- [ ] `src/gui/config_writer.hpp:25` (mention) (std::async), so the GUI thread is never inside the write. Unlike
- [ ] `src/gui/frame_log.hpp:120` (mention) "with vsync on" measured the GUI thread rendering flat out, and moved with
- [ ] `src/gui/instrument_teleprinter.cpp:62` (**contract**) The elapsed time comes from a tiny GUI-thread-only table keyed on the
- [ ] `src/gui/instrument_teleprinter.cpp:110` (**contract**) that takes half a second needs the difference. GUI THREAD ONLY - every
- [ ] `src/gui/instrument_teleprinter_math.hpp:15` (mention) std::string, no heap. It runs once per frame on the GUI thread.
- [ ] `src/gui/plugins_view.hpp:68` (**contract**) GUI THREAD ONLY, like everything else in this directory.
- [ ] `src/gui/present_grace.hpp:6` (mention) Win32: Failed to query display settings" and then the GUI thread sits in
- [ ] `src/gui/scope_view.hpp:31` (**contract**) GUI THREAD ONLY, like everything else in this directory.
- [ ] `src/gui/shell_open.hpp:1` (mention) shell_open.hpp - handing something to the Windows shell from the GUI thread,
- [ ] `src/gui/shell_open.hpp:18` (mention) anything added later that blocks the GUI thread - "a synchronous device open,
- [ ] `src/gui/shell_open.hpp:24` (mention) detached thread so the GUI thread never blocks - would keep the frame loop
- [ ] `src/gui/track_detail_view.hpp:17` (**contract**) GUI THREAD ONLY, like everything else in this directory.
- [ ] `src/gui/track_info_cache.cpp:111` (mention) BOUNDED PER CALL. This runs on the GUI thread, once per frame, and the
- [ ] `src/gui/track_info_cache.hpp:11` (**contract**) GUI THREAD ONLY, like every other plugin-facing surface in this directory.
- [ ] `src/gui/track_info_cache.hpp:71` (mention) from owning the GUI thread for ever.
- [ ] `src/gui/waterfall_view.cpp:176` (**contract**) after theme::generation() moves (a setTheme). GUI THREAD ONLY: every caller
- [ ] `src/gui/waterfall_view.hpp:38` (**contract**) rebuilt when theme::generation() moves; GUI thread only.
- [ ] `src/gui/waterfall_view.hpp:378` (mention) long as it stayed paused. Called from addLine() and draw() (GUI thread).
- [ ] `src/gui/win_frame.cpp:30` (mention) Written by the window procedure (the GUI thread) and read by the frame loop

## 9. OPEN

Things 3a could not move without changing threading or behaviour, left where
they were with the facts; and what 3b has to settle first.

1. **Engine fields the window still edits in place** (`kWindowMayWrite`, now
   NONE - `kWindowMayWriteScoped` and `kWindowMayWriteUnscoped` are both
   EMPTY; the last entry, `patchGraph_`, closed with OPEN 6's graph half,
   below. See the CLOSED/SCOPED notes below). Each was a form or a status
   line the engine reads when it acts; in 3b each becomes a command (or a form
   a command carries), or the status line moves to the window.
   **CLOSED, engine/stage3b-pre fields-to-commands round 2 (2026-09-28):**
   the 7 scanner-form fields, `soundCard_`, `plutoUri_`, `transmitArgs_`,
   `pluginCatalogueUrl_`, `telemetryEnabled_`/`telemetryInstallId_`,
   `patchListsWanted_`, `patchSinkLines_` and `muteKeptRunning_`/`mutePopup_` -
   every remaining `kWindowMayWrite` entry except `patchGraph_` (OPEN 6/item
   3, see below). Design per field:
   - **The scanner form** (7 fields): a window-side draft (7 doubles, seeded
     once in `applyConfig`), committed on deactivate-after-edit through TWO
     EXISTING ops rather than a new one - `FOXAPI_OP_SCANNER_CONFIG`
     (dwell/hold/resume/listen, which already reconfigures a running scan on
     its own) then `FOXAPP_OP_SCANNER_RANGE` (start/stop/step, bit 8 added
     when the scanner is active). A from-scratch `FOXAPP_OP_SCANNER_TIMING`
     was written first and found, only once built, to duplicate
     `FOXAPI_OP_SCANNER_CONFIG` byte for byte - removed before commit; the
     lesson (check for an existing op before adding one) is worth a line
     because it very nearly shipped as a real op.
   - **`soundCard_`**: one new op, `FOXAPP_OP_SOUND_CARD_FORM` - the whole
     struct in one command (`ival[0]` a bitmask: format/channel/swapIq/
     pickedFromList; `num[0..1]` cardRateHz/iqCentreHz; `text`
     `"device<0x1F>hostApi"`), applied AT ONCE from every widget in
     `drawSoundCardControls` so nothing drawn later in the same frame (the
     rate list, the "Receives..." preview - both still reading
     `engine_.soundCard_` directly) ever sees a stale value.
   - **`plutoUri_`/`transmitArgs_`**: NO new op for either. Each box now
     edits a window-local draft (`plutoUriDraft_`/`transmitArgsDraft_`,
     never the engine field), and "Open" sends the draft's text through the
     command each already used
     (`FOXAPI_OP_SELECT_SOURCE "open-pluto:uri=..."` /
     `FOXAPI_OP_TX_OPEN`) - both of whose handlers already persist the
     field from that text (`TX_OPEN`'s did already; `openPlutoAt` gained one
     line to do the same). `transmitArgs_`'s "seed a default if empty" logic
     moved into `FOXAPP_OP_SET_TRANSMIT_PAGE_OPEN`'s handler (closed 2b),
     so the draft is never seeded blank.
   - **`pluginCatalogueUrl_`**: one new op, `FOXAPP_OP_SET_CATALOGUE_URL`
     (text, empty clears) - the window already had a draft buffer
     (`pluginUrlBuf_`) and committed on deactivate-after-edit; only the
     commit's direct write needed converting.
   - **`telemetryEnabled_`/`telemetryInstallId_`**: NO new op - the real API
     already had a slot for this, `FOXAPI_OP_TELEMETRY_ENABLE`
     (`third_party/foxsdr_api/foxsdr_api.h`, ival[0] 0/1), simply never
     implemented (`app_commands.cpp` had it marked `false`). Implemented
     now: on mints an id, off forgets it (so a later opt-in cannot be tied
     to the old one) and reconfigures `telemetryHeartbeat_` in the same
     step, moved verbatim from the Checkbox handler.
   - **`patchListsWanted_`**: one new op, `FOXAPP_OP_PATCH_LOOK_FOR_RADIOS` -
     unconditional (unlike the existing `FOXAPP_OP_PATCH_RADIO_LIST_OPENED`'s
     one-time wish), matching what "Look for radios" always did.
   - **`patchSinkLines_`**: NO command at all - a new reviewed direct call,
     `Engine::prunePatchSinkLines()` (the same pattern as `scanSoundCards`/
     `patchListRecordings`, added to `kControlMayCall`), since the window's
     one write site was housekeeping (pruning a gone node's cached lines)
     inside the SAME per-frame pass that already prunes the window's own
     `patchScopes_`/`patchScopeSeq_` maps for the identical reason - moving
     the mutation into the Engine needed no new vocabulary, just a home.
   - **`muteKeptRunning_`/`mutePopup_`**: one new op,
     `FOXAPP_OP_MUTE_KEEP_RUNNING` (no args) - both fields move together, as
     the ONE user decision "Keep it running" always was, never two
     independent clears. ("Stop and resume sound" needed no change: it
     already went through `FOXAPP_OP_DECODER_STOP_LIST`, and the mute lifts
     on its own once the engine's own `advanceMutePopup` state machine sees
     the decoder actually stop.)

   Every write site converted to `engine_.applyCommand(...)`/
   `engine_.submitCommand(...)` (matching whichever the site already used),
   applied at the same point in the frame as the direct write - no
   user-visible behaviour change. Tests: `tests/test_apply_command.cpp`
   gained or extended coverage for every op above (`FOXAPP_OP_SOUND_CARD_FORM`
   with both a fully-set and an all-clear command; `FOXAPI_OP_TELEMETRY_ENABLE`
   proving a second ON mints a DIFFERENT id than the first; `FOXAPP_OP_MUTE_KEEP_RUNNING`
   checking both fields move together; the existing `FOXAPI_OP_SCANNER_CONFIG`
   coverage extended from 2 to all 4 fields since it is now also the panel's
   own commit path; `FOXAPP_OP_PATCH_LOOK_FOR_RADIOS` unconditional versus
   `FOXAPP_OP_PATCH_RADIO_LIST_OPENED`'s one-time wish). Proven red against a
   combined mutant (drop `soundCard_.channel`'s bit, skip clearing
   `telemetryInstallId_` on OFF, skip resetting `mutePopup_` in
   `MUTE_KEEP_RUNNING`): 3/811 failed exactly at the three affected
   assertions; source restored byte-identical and reverified green.
   `test_command_path_guard`: 112/112, 0 violations - `kWindowMayWriteScoped`
   is now empty (a `std::vector`, not a C array, so the empty list still
   compiles: an array's bound cannot be deduced from an empty initialiser).
   **ROUND 3 FIX (branch claude/engine-round3-fix, 2026-09-28)**, for the
   review of 1497f79/41e3853 (`docs/review-harness/engine-round3-review.md`,
   REJECT). Four of the round-2 designs above changed:
   - **The scanner form** is no longer "seeded once in `applyConfig`". Each
     draft (`scanDraft_[7]`, defaulting to `Scanner::Params{}`, never 0) is
     re-seeded from its engine field on every frame that field is neither
     active nor holding an uncommitted edit, so a web-remote range shows at
     once and a later desk edit no longer writes the old range back.
     Scanner IDLE: each edit is committed as typed (the stored fields only
     say what Start will scan), so a collapsed section or a switched bank
     cannot lose it. Scanner RUNNING: an edit waits for
     deactivate-after-edit - a running scan never reconfigures from a
     half-typed value - or, if the section stops being drawn first, for
     `flushScannerDraft()`, called once a frame from `drawUi`. Only the
     edited fields are committed (the range's bits for the range fields; the
     engine's own values for timings nobody touched).
   - **`plutoUri_`/`transmitArgs_`** each gained an op after all -
     `FOXAPP_OP_SET_PLUTO_URI`/`FOXAPP_OP_SET_TRANSMIT_ARGS`, committed as
     typed - so an address typed and never opened is again what
     `config.json` keeps, as it was before the drafts. Both boxes re-seed
     from the engine when not being typed in. `SET_TRANSMIT_ARGS` is
     refused while a board is open (the box is disabled then anyway).
   - **`FOXAPI_OP_TX_OPEN` refuses an empty or blank address**
     (`FOXAPI_BAD_ARGUMENT`) instead of opening whatever `transmitArgs_`
     held - a board the cleared box no longer showed. The page greys Open
     with "Type the board's address first." while the box is empty.
   - **Usage reporting**: the desktop's checkbox now sends
     `FOXAPP_OP_TELEMETRY_CONSENT`, an extension op no transport carries,
     and `FOXAPI_OP_TELEMETRY_ENABLE` is implemented only as a refusal
     (`FOXAPI_DENIED`) from every origin, so no plugin, browser or future
     API session can switch reporting on or off. ON while already on is
     `FOXAPI_NO_CHANGE` and keeps the install id.

   The sound card form's encode and decode moved together into
   `engine/soundcard_panel.hpp` (`soundCardFormCommand`/
   `soundCardFormFromCommand`). Tests: `tests/test_scanner_draft_input.cpp`
   drives the Scanner section through ImGui's input queue (the reviewer's
   five harness scenarios plus three for a running scan). The harness itself
   passes 16/16, unmodified. `test_apply_command` gained a 16-case sound-card
   round trip, the empty-`TX_OPEN` refusal, both new address ops, the
   telemetry refusal and keep-the-id checks, and a `prunePatchSinkLines`
   check. `test_airspy_app` reads back the published decimation choices.
   Each of the review's four surviving mutants, and eight more against the
   new code (no re-seed, no flush, idle edits not committed, running edits
   committed per keystroke, `TELEMETRY_ENABLE` accepted, ON re-minting the
   id, empty `TX_OPEN` opening the old board, choice count zero), now fails
   at least one test.
   **CLOSED, engine/stage3b-pre 2c (2026-09-28): `sourceError_`,
   `soapyScanDeferredLogged_`, `gpsRefusal_`, `decoderLog_`,
   `bookmarkImportNote_`, `soundCardMissing_`, `mutePopupQueued_`** (7 of
   the 8 remaining status-line entries after 2b; `patchSinkLines_`,
   `muteKeptRunning_` and `mutePopup_` were NOT attempted - see below). Three
   new extension ops (`core/app_commands.hpp`): `FOXAPP_OP_SET_SOURCE_ERROR`
   (text, empty clears - the window's OWN validation of a typed frequency it
   could not parse; every radio-side error still reaches `sourceError_` from
   inside the Engine, unchanged), `FOXAPP_OP_SET_BOOKMARK_NOTE` (text - the
   Bookmarks panel's export result; an import's note is still set by the
   Engine itself from `FOXAPI_OP_BOOKMARK_IMPORT`), and `FOXAPP_OP_CLEAR_STATUS`
   (`ival[0]`, a `cascade::core::cmd::FoxAppStatus` selector) covering the
   other five - each purely a "forget this, back to default" reset the window
   used to do in place once a status line's purpose was served (a port
   changed, a card was picked, a Clear button pressed, a gate reopened, a
   popup was shown), never anything a radio/GPS/scan/decoder might still need
   to report a NEW instance of a moment later - which is why one op, selected
   by an enum, safely covers all five rather than needing five. Every write
   site converted to `engine_.applyCommand(...)`, applied AT ONCE (same-frame,
   no behaviour change): the Source frequency editor (2 sites: clear on a
   good parse, set the parse-failure message on a bad one), three GPS-form
   sites (port typed, port picked from the list, baud picked - each pairs
   with `gpsReader_.clearResult()`, which stays a direct call, already
   `kReadOnly`), the sound-card device picker, the Decoder output window's
   Clear button, the Source section's per-frame "the gate reopened" line
   (`drawSourceSection`, still checked every frame - now submits the same
   idempotent command every frame that condition holds, harmless, matching
   what the direct write already did at the same frequency), the mute
   popup's "shown" consumption, and the Bookmarks panel's export note.
   `tests/test_apply_command.cpp` gained `statusLineOps` covering all three
   ops (including the out-of-range `CLEAR_STATUS` selector refusing, not
   silently no-op-ing). Proven red against two mutants in the same run (drop
   `gpsRefusal_.clear()` from its case; drop the `default:` refusal): 2/758
   failed, source restored byte-identical and reverified green.
   `test_command_path_guard`: 128/128 (7 fewer than the post-2b 135), 0
   violations.
   **NOT ATTEMPTED this round: `patchSinkLines_`** - its one write site
   (`app_window.cpp`, pruning a gone patch node's cached lines out of the map)
   sits inside the same per-frame patch reconciliation loop OPEN item 6
   describes, not a standalone status line; converting it without touching
   that loop's own open design question was judged not worth doing in
   isolation. **`muteKeptRunning_`/`mutePopup_`**: genuinely bidirectional
   dialog state (the Engine opens the question, the window's answer sets
   `muteKeptRunning_` and resets `mutePopup_` together, as one user decision,
   not a status line either side merely clears) - a real design worth doing
   for 3b, not attempted here for the same reason the previous round gave for
   not attempting 2b/2c at all: not without the same build/test verification
   the rest of this round got.
   **CLOSED, engine/stage3b-pre 2b (2026-09-28): `transmitOpen_`** (stage 1
   OPEN 2), the one entry on this list flagged SAFETY. The toolbar switch and
   the page's own close (ImGui's close box and its custom chrome's close
   press) both wrote `engine_.transmitOpen_` directly, including through
   `&engine_.transmitOpen_` handed to `beginPage`. Now: a new extension op,
   `FOXAPP_OP_SET_TRANSMIT_PAGE_OPEN` (`core/app_commands.hpp`, ival[0] 0/1),
   applied AT ONCE (`applyCommand`, not `submitCommand`, so the page still
   opens/closes on the same frame the direct write did); `drawTransmitPage`
   passes `beginPage` a local mirror bool, never the engine field's address,
   and submits the command when the mirror goes false on either exit path.
   The handler sets `transmitOpen_` and, closing, calls
   `transmitter_.releaseRemote(...)` IN THE SAME STEP - closing OPEN item
   10's "page closed" half (the "web server stopped/disabled" half stays in
   the window's `applyWebControls`, unchanged, since the web server itself is
   the window's until stage 5). The window's own per-frame poll in
   `applyWebControls` (`if (!engine_.transmitOpen_) { ...releaseRemote... }`)
   is INTENTIONALLY left in place as a second line of defence for anything
   that still sets `transmitOpen_` directly (today: test-only friend
   accessors) - both firing is harmless (`releaseRemote` on an unheld key is
   a no-op, exactly as the "a server stopped between two frames releases
   twice" comment already documented for the other condition). Tests:
   `tests/test_apply_command.cpp` covers the op directly (open, close
   releasing a held key, closing-when-already-closed is not a refusal);
   `tests/test_remote_key_release.cpp` gained scenario D, proving the release
   happens through the command alone, with `applyWebControls` never called -
   distinguishing it from scenario A's window-poll mechanism. Proven red
   against the named mutant (drop the `releaseRemote` call from the new
   command's handler): `test_apply_command` 1/734 failed
   (`A::remoteHoldMs(a) == 0`), `test_remote_key_release` 1/20 failed
   (scenario D's own assertion); source restored byte-identical and
   reverified green. `test_command_path_guard`: 135/135 (one fewer than the
   pre-existing 136, `transmitOpen_` removed from `kWindowMayWrite`; still
   0 violations). `test_transmit_page` (a source-text scan) needed no change
   - `drawTransmitPage`'s edit kept the literal `if (!beginPage("Transmit###
   transmitwindow"` text the scan matches, adding the mirror-bool check
   around both of that `if`'s exits rather than restructuring it.
   **engine/stage3b-pre (2026-09-28) added one more of the same shape:**
   `patchListsWanted_` (0.99.40's patch-page device-list wish: set in place
   by `drawPatchRadioInspector`'s "Look for radios" button, and until B2's
   fix also by the combo-open site, now folded into
   `FOXAPP_OP_PATCH_RADIO_LIST_OPENED`'s handler - one of the two write
   sites is a command already; the other, "Look for radios", was not
   converted, for the same reason 2b below gives - not attempted without the
   same build/test verification the rest of this merge got).
   **SCOPED, engine/stage3b-pre 2d (2026-09-28):** `kWindowMayWrite` was a
   FLAT list - any control could write any listed field, which is not what
   "the window still edits it in place" meant (each was written from exactly
   one place). It is now `kWindowMayWriteScoped` (`tests/test_command_path_guard.cpp`),
   pairing every remaining field with the ONE `AppWindow` member allowed to
   write it (`drawScannerSection` for the 7 scanner fields,
   `drawSoundCardControls` for `soundCard_`, `drawSourceSection` for
   `plutoUri_`, `drawTransmitPage` for `transmitArgs_`, `drawPatchFaces` for
   `patchSinkLines_`, `drawMutePopup` for `muteKeptRunning_`/`mutePopup_`,
   `drawPluginStoreSection` for `pluginCatalogueUrl_`,
   `drawUsageReportingSection` for `telemetryEnabled_`/`telemetryInstallId_`,
   `drawPatchRadioInspector` for `patchListsWanted_`) - a write from any OTHER
   member is now rule 3's violation, exactly as if the field were not listed
   at all. `patchGraph_` stays in a separate, still-flat
   `kWindowMayWriteUnscoped` (written from a dozen-odd patch canvas/inspector
   members, all tied to OPEN item 6's still-open design; scoping it to "any
   of a dozen" would be the flat list with extra steps). The existing
   sub-field write detection (`fieldPatterns()`'s regexes) only matched a
   write to a field's WHOLE value or a mutating container call on it - a
   struct-typed field's sub-field assignment (`soundCard_.cardRateHz = ...`)
   passed silently, which is exactly the shape the brief's own probe named
   ("a probe writing transmitOpen_ or soundCard_ from drawToolbar must be
   flagged"). A new `scopedFieldPatterns()` adds a sub-field assignment regex
   (and a sub-field mutating-container-call regex) used only for the scoped
   list, since every `kFields` entry (the always-checked receiver fields) is
   a scalar and does not need it today. **Proven red**: a probe inserted at
   the top of `drawToolbar` (`engine_.soundCard_.cardRateHz = 1.0;` -
   `soundCard_` is `drawSoundCardControls`'s alone;
   `engine_.transmitOpen_ = true;` - no longer listed at all since 2b closed
   it) - both flagged (`writes soundCard_, which only
   AppWindow::drawSoundCardControls may write`; the pre-existing general
   `field write transmitOpen_`), 2 violations; probe removed (confirmed by
   `git status`/`git diff` showing no residual change to `app_window.cpp`)
   and reverified 0 violations, 145/145 checks.
2. **Non-command calls a control still makes** (`kControlMayCall`):
   `telemetryNotePanel` (a usage counter), `currentConfig` (the save's
   snapshot - now `engine_.fillConfig` inside it), `currentAbsoluteHz` and
   `carriedAirCentre` (queries not declared const because
   `Pipeline::activeSource()` is not), `muteNameList` (static),
   `refreshDiagContext` (the diagnostics bundle), `scanSoundCards` (the sound
   card panel lists the cards). In 3b each is either a read of the snapshot or
   a command.
   **engine/stage3b-pre (2026-09-28) added five more, all 0.99.41's Airspy
   controls, all direct calls a control makes on `engine_` that mutate
   state outside the command path:** `chooseAirspyDecimation`,
   `chooseAirspyGainMode`, `chooseAirspyAgc` (the mode buttons, the AGC
   switches and the decimation combo in `drawAirspyControls` call these
   directly - each changes what the radio streams), `airspyRememberOpen`
   (writes `airspyMemory_`, called after each of the three above and after a
   web/CAT gain-by-name or AGC change) and `patchListRecordings` (the patch
   device combo's own list). None was converted to a command in this
   session - it would mean carrying the Airspy panel's whole mode/AGC/
   decimation surface through `FoxCommand`, which is exactly the class of
   change 2b below declines for the same reason: not attempted without the
   same level of verification the rest of the merge got.
   **Scoping `kControlMayCall` (engine/stage3b-pre 2d) was considered and
   NOT applied**, unlike `kWindowMayWrite`'s fields (which each had exactly
   one real owner - see item 1's 2d note). Checked every entry's call sites:
   `telemetryNotePanel` is called from ~20 different panel-open controls by
   design (every panel notes itself), `currentAbsoluteHz` from several
   unrelated controls, `refreshDiagContext` from both a control and `run()`
   (machinery) - none of these has a single owner to scope to, so a
   `kWindowMayWrite`-shaped fix does not fit. Only the Airspy group
   (`asAirspyDevice`, `chooseAirspyDecimation`, `chooseAirspyGainMode`,
   `chooseAirspyAgc`, `airspyRememberOpen` - all `app_window_airspy.cpp`'s
   `drawAirspyControls` alone), `scanSoundCards`
   (`app_window_soundcard.cpp`'s sound-card panel alone) and
   `patchListRecordings` (`app_window_patch_radios.cpp`'s patch radio
   inspector alone) are genuinely single-owner and would scope cleanly the
   same way - left undone this round for the same reason the calls
   themselves were not converted to commands (not attempted without the same
   verification), not because it is a different kind of problem.
   **CLOSED, engine/stage3b-pre Airspy round (2026-09-28):**
   `chooseAirspyDecimation`/`chooseAirspyGainMode`/`chooseAirspyAgc` and
   `asAirspyDevice` (item 3's own entry, closed together since the fix is one
   change). Three new commands (`FOXAPP_OP_AIRSPY_DECIMATION`/`GAIN_MODE`/
   `AGC`, `core/app_commands.hpp`) - queued (`submitCommand`), like every
   other gain control in the Source section, each a thin dispatch onto the
   SAME Engine methods (unchanged, still called internally, still what
   `tests/test_airspy_app.cpp` drives directly to prove the underlying
   logic) - out-of-range gain modes/AGC stages refuse
   (`FOXAPI_OUT_OF_RANGE`), a refused decimation/mode/AGC change refuses
   (`FOXAPI_FAILED`, `sourceError_` set, exactly as the direct call already
   did). `airspyRememberOpen` and `patchListRecordings` were NOT converted
   this round (see the 2d note above - genuinely single-owner, but not
   reached). `drawAirspyControls` no longer calls `asAirspyDevice()` at all -
   removed from `kControlMayCall`.
   **CLOSED, stage-3b-pre-end round, item 2 (2026-09-29): the two calls the
   brief named.**
   - **`scanSoundCards`**: the sound card row now sends
     `FOXAPP_OP_SOUND_CARDS_WANTED` (0x841D, applied at once as the call
     was); the engine lists the cards on its worker unless they are listed,
     a list is being taken, or a card is opening - the test the panel used
     to make itself. Removed from `kControlMayCall`. Test:
     test_soundcard_app_paths `testSoundCardsWantedIsACommand` (fake
     backend, counting lists: ask, ask while scanning, ask once listed ->
     exactly one list). Red before (op unknown, 6 checks); mutants "scans
     every time" (2) and "lists nothing" (3) red.
   - **`currentConfig`/`fillConfig`**: the window no longer runs
     `Engine::fillConfig`. The engine fills the receiver half into a copy
     behind a mutex (`Engine::publishConfig`) as the last step of its
     frame's `pumpAudio` (so `Engine::pump` too), and at the two points a
     save must be current - the window's first snapshot (constructor) and
     `saveConfigNow` (shutdown, a language switch); `currentConfig()` reads
     only `Engine::configSnapshot()`. `publishConfig` joins
     `kControlMayCall` (called from `saveConfigNow`: it refreshes the copy
     and changes no receiver state; on a control thread it becomes a
     request that thread answers). The one visible consequence: a change a
     widget applies AFTER the frame's `pumpAudio` reaches the debounced
     save one frame later - absorbed by the debounce; the shutdown save
     publishes first. Test: tests/test_config_snapshot_app.cpp (A: a change
     not yet published is not in `currentConfig()`, then is; B: one
     `Engine::pump` publishes; C: `saveConfigNow` saves a change made after
     the last pump; D: with volume 0.3 in the file, the constructor's
     remembered config says 0.3). Red before (A, 2 checks). Mutants: the
     live fill back (2), no publish in the frame (1), none in
     `saveConfigNow` (1), none in the constructor (1). test_airspy_app,
     test_bias_key_app and test_soundcard_app_paths now publish before
     reading `currentConfig()`, as a frame does.
   **Still open under this item**: `telemetryNotePanel`,
   `currentAbsoluteHz`, `carriedAirCentre`, `muteNameList` (static),
   `refreshDiagContext`, `airspyRememberOpen`, `patchListRecordings`,
   `prunePatchSinkLines`. Each is a query or a local reviewed call rather
   than a receiver change; converting the queries means reading them from
   the published snapshot, which is item 3's work, and the rest is
   bookkeeping with one owner each (see the 2d note above).
3. **The window reads engine members directly** as a friend (every panel
   draws from `engine_.x_`, `engine_.pipeline_.y()`): allowed by the guard as
   reads, and safe only because both run on one thread. 3b needs every such
   read to come from the snapshot (or a window-side copy the engine hands
   over), which is stage 4's read API arriving early for the window.
   **engine/stage3b-pre (2026-09-28) found a sharper instance of the same
   problem:** `Engine::asAirspyDevice()` (0.99.41) hands `drawAirspyControls`
   a raw `cascade::source::AirspySource*` into the open device, read (and, in
   the mode-button/AGC/decimation handlers above, driven) EVERY FRAME the
   panel is open - not a copy, not a snapshot field, the live pointer the
   Engine's own worker thread will one day be reopening behind in 3b. Every
   other query this stage added (`currentAbsoluteHz`, `carriedAirCentre`)
   returns a value; this one is the one exception that returns a pointer
   into engine-owned, mutable object state. 3b's read API (item 3's own
   fix) has to cover this case explicitly, or the window keeps a dangling
   pointer across the frame in which a control thread reopens the radio.
   **CLOSED, engine/stage3b-pre Airspy round (2026-09-28):** a new block in
   `PublishedState::app` (`core/receiver_snapshot.hpp`'s `AppStateExt`:
   `airspyOpen`, `airspyDecimation`, `airspyHardwareSampleRateHz`,
   `airspyGainMode`, `airspyLnaAgc`, `airspyMixerAgc`, and a fixed
   `airspyDecimationChoices`/`airspyDecimationChoiceCount` pair - a real
   `std::vector` has no place in a struct a real-time plugin thread may read
   lock-free, matching `gains`/`rates`' own fixed-array shape above), filled
   in `fillPublishedState` from `asAirspyDevice()` - the ONE place that call
   still happens, now on the Engine's own thread, never crossing to a
   control. `drawAirspyControls` reads `engine_.receiverSnapshot_->read(...)`
   once per call (the same lock-free path `catStatusNow()` already used) and
   answers every question it used to ask the raw pointer from that block
   instead - `deviceGainNames_`/`deviceGainsDb_`/`deviceGainRanges_` (the
   generic gain mirrors every OTHER Source-panel slider already reads the
   same way) are unchanged, since they were never the pointer this item
   named. One consequence, accepted rather than worked around: a change now
   reads back on the frame AFTER the click (the publish that already ran
   this frame is stale by definition) - the same one-frame bound every gain
   slider in this section already has, not a new one. Proven red (a mutant
   swapping `FOXAPP_OP_AIRSPY_AGC`'s LNA/Mixer selection):
   `tests/test_airspy_app.cpp` failed 2/75 at the exact two assertions
   checking which stage moved; source restored byte-identical and
   reverified green, 75/75.
   **PARTLY CLOSED, stage-3b-pre-end round, item 3 (2026-09-29): strings
   first, and a ratchet on the rest.** The window read 177 engine fields
   directly, 829 times (counted by the guard below, at 41c67ab). Converting
   all of them in one round, verified to the same standard, is not
   possible; this round moves the first group and stops the rest growing.
   - **The status lines are a handed copy**: `Engine::StatusText` - the
     fourteen sentences the engine writes for a panel to letter
     (`sourceError_`, `gpsRefusal_`, `catalogError_`, `bandPlanError_`,
     `tuneMismatchNote_`, `transmitError_`, `soundCardMissing_`,
     `sdrPlayApiDetail_`, `sdrPlayAdvice_`, `recordNotice_`, `recordError_`,
     `presetNote_`, `pluginEnforceError_`, `restoreKeepLabel_`) - published
     behind a mutex by `Engine::publishStatusText` with the frame's
     snapshot (`pumpPublish`, before anything is drawn) and at the end of
     the frame (`pumpAudio`, after the workers' answers), and in the
     constructor. The window keeps one copy a frame (`statusLines_`,
     refreshed right after `pumpPublish`) and every panel letters from it;
     the report functions run after the last frame read a fresh copy. The
     one `run()` seam that WROTE one of them (`gpsRefusal_.clear()` before
     a hook-port GPS read) is now `FOXAPP_OP_CLEAR_STATUS`. Consequence,
     accepted as the Airspy readback's was: a line set by an at-once command
     DURING drawing shows on the next frame (lines set by workers already
     did).
   - **The guard (tests/test_command_path_guard.cpp)**: `kPublishedOnly` -
     any `engine_.<one of the fourteen>` in AppWindow code, machinery
     included, is a violation; and the RATCHET - the window's remaining
     direct reads of engine fields are counted (`codeLines(..., true)`, the
     text with `engine_.` kept) and may not exceed
     `kWindowFieldsReadBudget`/`kWindowFieldReadsBudget` (163 fields, 793
     reads now); each round that moves a group lowers them.
   *Tests.* A probe reading `engine_.sourceError_` and
   `engine_.catalogError_` in drawToolbar passed the old guard (0
   violations); the same guard over 41c67ab's window reports the 36 live
   reads this round removed. tests/test_config_snapshot_app.cpp E: all
   fourteen reach `statusText()` only on a publish, at both points.
   Mutants: no publish with the snapshot (2 checks), one line left out (3),
   no end-of-frame publish (1).
   **Still open**: 163 fields (containers first next: the device and gain
   lists - `deviceGainNames_`/`deviceGainsDb_`/`deviceGainRanges_`,
   `nativeDevices_`, `soapyDevices_`, `soundCardDevices_`, `devices_` - the
   plugin catalogue `catalog_`, `patchCatalogue_`, `decoderLog_`, and the
   patch runtime's maps), then the scalars.
4. **The EngineHost hooks are synchronous calls into the window** (section 3).
   Most become events the window drains. Three are not events:
   `onPluginsUnloading`/`beforePluginRescan` must finish before modules are
   unmapped, and `attachBasemap`/`attachTrackInfo` hand plugin API pointers to
   the window - the window calling into plugin code on the GUI thread while
   the control thread unloads it is a race that needs a handshake. The
   publish-time hooks (`tunerDisplayStyle`, `basemapFacts`, `enrichWebTrack`,
   `fillWebImages`, `webListening`) put window state into the web block; the
   control thread cannot call them. `frameTimeS` reads ImGui's context.
   **PARTLY CLOSED, stage-3b-pre-end round, item 4 (2026-09-29).**
   - **`frameTimeS`/`frameClockRunning` no longer read ImGui from another
     thread.** On the GUI thread they read ImGui as before and leave the
     answer in an atomic; from any other thread they return that answer
     (`AppWindow::frameTimeS`, `guiThread_`), and `drawUi` reads them once a
     frame so it is at most a frame old. Nothing changes in 3a (the engine
     calls them on the GUI thread); a control thread gets a safe clock.
     Not replaced by a handed-in time: tests drive the engine's clock
     through ImGui's DeltaTime without drawing a frame (the scanner draft
     tests), and a handed-in value would have frozen it.
   - **`webListening`, `tunerDisplayStyle`, `basemapFacts` are handed
     over**, not hooks: the window builds `FrontEndFacts` (`handFrontEndFacts`,
     window machinery) in the constructor and in `drawUi` just before
     `pumpPublish`, and the publish reads `Engine::frontEndFacts()`. The
     three virtuals left `EngineHost`; the headless default face stays
     "nixie". test_state_snapshot_golden's publish now hands the facts over
     first, as the frame does - the golden record itself is unchanged
     (0/878 lines differ).
   *Tests.* tests/test_engine_host_threads.cpp (new): A - the GUI thread
   reads 1.0 s, ImGui moves to 3.0 s unread, another thread gets 1.0 s (red
   before: 3.0 s); B - with the context gone another thread still gets the
   GUI thread's last answer (red before); C - handed-over facts (listening,
   basemap 3..11) are what is published (red before: 0, 0..19). Mutants:
   either off-thread branch removed (1 each), the publish ignoring the
   handed basemap (2) or listener (1); drawUi's hand-over removed fails the
   guard's kLineAllowed entry.
   **Still open, and why - each needs the control thread to exist to be
   designed properly, not only reworded:** `beforePluginRescan`/
   `onPluginsUnloading` (the window must drop every plugin pointer BEFORE a
   module is unmapped - on two threads that is a handshake: the control
   thread asks, the GUI thread answers at a frame boundary, the unload
   waits for the answer, bounded); `attachBasemap`/`attachTrackInfo` (they
   hand plugin API pointers across; same handshake, in reverse);
   `enrichWebTrack`/`fillWebImages` (the window fills parts of the web
   block from plugin state it owns - they become the window's own post-pass
   over the published block, or data it hands over, once the publish runs
   elsewhere); and the event-shaped hooks (`onBookmarksChanged` and the
   rest) become a queue the window drains. None of these is a race in 3a.
5. **ADD ALL's plan is built by the window** (`planAddAll`: the store model
   and its plan come from the window's store deck); a headless engine refuses
   ADD ALL. 3b/4: compute the plan from engine state (catalogue + inventory)
   or carry it in the command.
   **CLOSED, stage-3b-pre-end round, item 5 (2026-09-29): the plan is the
   engine's.** The rule (what ADD ALL picks, the words on the key, why it
   may not be pressed) moved verbatim from gui/plugin_store_view.cpp into
   engine/add_all_plan.hpp (`planAddAllRows`, over plain `AddAllRow`s -
   name, fitted, update planned, notice, the install gate's two answers -
   and the catalogue's state). The store window letters its key from it
   (`gui::planAddAll` is now a wrapper over the model's rows) and
   `Engine::startAddAll` runs from it over the engine's own catalogue and
   inventory (`catalogEntryInstalled`, `plannedPluginUpdates`,
   `pluginInstallBlockedReason`) - so the key's words and the run are one
   answer, and a headless engine runs ADD ALL instead of refusing it. The
   one thing only the window knew - which module's OWN notice tick is on
   (the store's selected row) - travels in the window's new
   `FOXAPP_OP_STORE_ADD_ALL` (0x841E: ival[0] the ADD ALL tick, text that
   module's id); `FOXAPI_OP_STORE_UPDATE_ALL` is the same with none.
   `EngineHost::planAddAll`/`AddAllChoice` are gone. Test:
   tests/test_add_all_plan.cpp (new, an Engine and no window; a catalogue
   of a plain module, a noticed one and one with no build here): A
   UPDATE_ALL unacknowledged runs {plain}; B acknowledged {plain,
   noticed}; C STORE_ADD_ALL with noticed's own tick {plain, noticed},
   with another module's tick {plain}; D no catalogue refused with the
   window's own sentence. Red before (5/6: "ADD ALL is planned by the
   store window, and there is none"). Mutants: the per-module tick
   ignored (1 + 1 in test_apply_command), the ADD ALL tick ignored (1 + 5
   in test_plugin_store_view), the old refusal back (5). The guard's OPEN 3
   ratchet caught the first draft of the window's key reading
   `engine_.catalog_` for the selected id; it reads the model it just
   built instead.
6. **The patch runtime is still tied to the Patch page.** drawPatchPage
   compiles the plan, reconciles the radios, publishes each radio's set and
   stops the patch when the page closes, every frame, through reviewed engine
   lines (stage 1 OPEN 2/10); the engine asks `patchPageOpen()`. The document
   (text, pan, zoom) is restored and saved by the window. 3b: the compile and
   reconcile belong on the control thread, driven by a command carrying the
   graph; the "page closed" rule becomes an explicit command.
   **DESIGN ONLY, engine/stage3b-pre "next round" item 3 (2026-09-28) - NOT
   implemented.** Surveyed first, because the brief's own bar for
   implementing ("add node, wire, delete, device change, save/load through
   commands, with no user-visible change") turned out not to be reachable
   this round without either leaving it half done or risking a real,
   unverified behaviour change - both worse than stopping here and saying so.

   **What is actually there.** `engine_.patchGraph_` is edited from the
   window at 27 call sites across `app_window.cpp` and
   `app_window_patch_radios.cpp` - `addNode` (the parts bin drop, the Radio
   inspector's own add), `mutableNode` (dragged position, resize, every
   inspector field: device, frequency, label, squelch...), `removeNode`
   (Delete key / context menu), and the wire endpoints the canvas drags
   between ports. None of these goes through a command; all are direct
   mutations of a live, friend-accessible `core::patch::Graph`, which is
   exactly stage 1/3a's "the window still edits it in place" (`patchGraph_`
   is `kWindowMayWriteUnscoped`'s one remaining, deliberately-unscoped entry -
   see item 1). `patchOpen_` (which face is showing) is a WINDOW field, not
   an engine one - the engine only ever asks for it through the
   `patchPageOpen()` host hook, which is already the correct stage-3a shape
   and is NOT itself a guard violation; the actual problem is what it gates.

   **Design A - the graph as one command (recommended).** The document is
   small (a session's whole patch, typically under a dozen nodes) and the
   application ALREADY HAS a complete, tested serialise/parse pair for it
   (`core::patch::serialise`/`parse`, used for config save/load right now -
   `AppWindow::applyConfig`/`currentConfig`, `patchUi_.dirty`'s own
   reserialise at the bottom of `drawPatchView`). The cheapest correct design
   reuses this AS-IS: a new op, `FOXAPP_OP_PATCH_SET_GRAPH` (`text`/`longText`
   carrying `core::patch::serialise`'s own string - the document format
   already round-trips pan/zoom too, though those belong to the window's
   view, not the engine's graph, and would need to be split at the parse
   boundary rather than carried through the command). The canvas keeps
   mutating a LOCAL DRAFT `core::patch::Graph` (a plain copy, drawn from and
   written to exactly as `engine_.patchGraph_` is today) for the DURATION of
   an interactive edit - a drag in progress, a text field mid-edit - and
   submits the whole draft as one `FOXAPP_OP_PATCH_SET_GRAPH` at the same
   points the codebase already treats as "the edit is committed": mouse-up
   after a drag (already tracked - `patchUi_.dirty`), Delete pressed,
   deactivate-after-edit on an inspector field, a device chosen from a combo.
   The Engine parses the incoming text with the SAME `core::patch::parse`
   validation `applyConfig` already trusts (`pr.ok`) and refuses the command
   if it fails, so a malformed graph can never reach `patchGraph_` - a
   STRONGER guarantee than today's direct mutation has. An incremental
   edit-op model (one op per node/wire operation) was considered and set
   aside: it would need roughly one op per the 27 call sites above, each
   with its own validation, for a graph small enough that shipping the
   whole thing is not a bandwidth concern - more vocabulary for no real
   benefit.

   **Design B - the runtime driven from pump, not from drawing - A REAL
   SUBTLETY FOUND, not just a relocation.** The instinct is: move
   `patchReconcile()`/`patchPublishSets()` out of `drawPatchView` into a new
   `Engine::pumpPatchRuntime()` phase, called unconditionally from `pump()`
   like every other phase, gated internally on `host_->patchPageOpen()`
   exactly as today. For `patchReconcile()` (which explicitly runs "before
   anything is compiled or drawn this frame" per its own call-site comment)
   this is a mechanical, safe move - a pump phase already runs before the
   window's own drawing, so "before this frame's canvas" is preserved either
   way. **`patchPublishSets()` is NOT equally free to move**: its call site
   comment states it runs deliberately AFTER the canvas has drawn, so a
   drag or resize that happened THIS frame is captured in THIS frame's
   signature check and does not restart a decoder needlessly - moving it
   into a pre-drawing pump phase would make it publish using LAST frame's
   node positions/dirty flag instead, a one-frame lag in exactly the
   "dragging must not restart every decoder" property that call site exists
   to protect. Sixteen milliseconds is very likely imperceptible, but
   "very likely imperceptible" is a claim about user perception, not a
   verified fact about the code, and this document's own core doctrine is
   that unverified claims do not get stated as settled - so this stays open
   rather than being implemented on a guess. The "page closed stops
   everything" half (`drawPatchPage`'s `!patchOpen_` branch) has NO such
   ordering constraint - it does not run alongside canvas drawing at all -
   and could move into the same pump phase safely today
   (`if (!host_->patchPageOpen() && patchWasOpen_) { ...the existing stop
   sequence... }`, idempotent exactly as the window's version already is);
   it was not done in isolation this round because half-closing item 6 while
   leaving Design A entirely undone would not itself close anything the
   guard or a test can observe, so it was left with the rest for one
   dedicated round to do together, verified together.

   **Estimated shape of that round**: convert the 27 call sites to
   draft-plus-`FOXAPP_OP_PATCH_SET_GRAPH` (Design A), add
   `Engine::pumpPatchRuntime()` and move both calls into it (Design B, with
   an explicit, measured answer to the one-frame-lag question above before
   trusting it), and the red-first tests the brief itself named: add a node,
   wire two ports, delete a node, change a Radio's device, save and reload -
   each proving the SAME graph and the same running decoders survive the
   command round-trip that the direct mutation produces today. Not attempted
   this round for the same reason 2b/2c/2d's own report gave for declining a
   shallow attempt: converting 27 call sites without the same build/test
   verification the rest of this round got would not meet the bar this
   document holds everything else to.
   **GRAPH HALF CLOSED, engine/stage3b-pre patch-graph round (branch
   claude/engine-patchgraph, 2026-09-28): Design A, as above, with three
   additions the design did not foresee.** Design B (the runtime driven
   from pump) is NOT done and stays open - see the end of this entry.

   *What was done.*
   - **The op.** `FOXAPP_OP_PATCH_SET_GRAPH` (`core/app_commands.hpp`,
     0x841B): the whole graph as long text; the Engine replaces
     `patchGraph_` with it or refuses it (`FOXAPI_BAD_ARGUMENT`, a sentence
     in the result's message, a diag warning) and keeps the graph it had.
     Refused WHOLE if the text is not a patch or ANY line of it cannot be
     honoured (`LoadResult::dropped != 0`) - the config load repairs a
     damaged document, the command does not repair a draft. The text is
     `core::patch::serialise`'s own document read by the same
     `core::patch::parse` (no new format), written and read by
     `core::patch::graphCommandText`/`graphFromCommandText`
     (`core/patch_draft.hpp`, new, header-only).
   - **The draft.** `AppWindow::patchDraft_`: the canvas
     (`drawPatchCanvas`), the faces, the inspector, the parts bin,
     `patchAddRadioPart` and `seedPatchIfNeeded` all edit it; nothing in
     `src/gui` writes `engine_.patchGraph_` any more. `commitPatchDraft()`
     sends it when it differs from the graph it was copied from - after the
     parts bin (before the compile, as the part always was), after the
     canvas and the faces, after the inspector (before
     `patchPublishSets`), and once a frame from `drawUi` so an edit never
     waits on a view that stopped drawing. Each is a no-op when nothing
     changed. `syncPatchDraft()` - at the top of `drawPatchView` and again
     right after `patchReconcile` - copies the engine's graph whenever it
     differs from the one the draft was last in step with (compared with
     `core::patch::graphsEqual` - every field the command's text carries,
     nothing written out - so no engine mutation site has to remember to
     bump anything): a config load, START/ALL OFF's switches, a device or
     centre the patch runtime set. Never mid-drag, resize or wire
     (`patchInteracting()`, true only while the left button is held). A new
     document (another epoch) drops the page's drag, resize, wire and
     selection, however it arrived. The draft is never a default: until it has
     been copied from the engine (`patchDraftInStep_`) nothing is ever
     committed from it.
   - **The config's patch and `FOXSDR_PATCH_FILE`** go to the engine
     through the same op flagged `FOXAPP_PATCH_GRAPH_DOCUMENT`
     (`AppWindow::loadPatchDocument`), and the draft starts again from what
     the engine then holds. The document text (`patchText_`) and the view
     (pan/zoom) stay the window's; the op always carries "view 0 0 1".
   - **The timing is kept.** `patchReconcile()` still runs before the
     canvas and `patchPublishSets()` after it, in the same frame, and the
     canvas's edit is committed between the two - so a node dragged THIS
     frame is in `patchGraph_` when the sets are built.
     `test_patch_graph_draft_input` B proves it through a test hook
     (`Engine::testHooks_.patchPublishing`, called at the top of
     `patchPublishSets`). The one-frame lag Design B's naive move would
     cause is now MEASURED, not argued: with `patchPublishSets()` moved
     above the canvas, every frame of the drag published the previous
     frame's position (40,40 while the node was at 70,52; 70,52 at
     100,64; ...).

   *The three additions, each because the round trip would otherwise
   change something.*
   - **Ids are kept** (`parse(text, Ids::Keep)`, `Graph::addNodeAs`). A
     document's ids are renumbered 1..N on load, which is right for a file
     and wrong here: the running radios (`patchRadios_`), the plan, the
     canvas's selection and every per-node map on either side are keyed by
     id, so deleting a middle node would have handed its id to the next
     one. Same format - only which id the parser gives a node changes. A
     duplicate or zero id is a dropped line, so refused.
   - **Two lines the document parser already skips** ("a line from the
     future", `patch_io.hpp`): `next-id <n>`, so an id is never handed out
     twice (the Engine also keeps the larger of its own and the draft's),
     and `centre-chosen <id>`, `Node::centreChosen` - session-only and
     never SAVED, but the command is not a save, and without it a Radio's
     typed 0 Hz centre (through a converter) would be forgotten by the
     next unrelated edit. Neither is written to config.json.
   - **The engine moves on while a node is dragged** (the take-over names
     a device and centre, a running radio reports its centre, a recording
     its rate, START/ALL OFF throw the switches). Sending the draft as it
     stood would put the page's stale copy of those back - the round-3
     review's finding 1 in another place. So the commit puts the page's
     edit ON TOP of the engine's graph as it is now
     (`core::patch::rebaseDraft`: what differs between the draft and the
     graph it was copied from wins, field group by field group - position,
     size, centre with `centreChosen`, and so on - everything else is the
     engine's; deletes, adds and wires likewise). With nothing raced it is
     the draft, byte for byte. A NEW DOCUMENT (`patchGraphEpoch_`, bumped
     by the flag) is never rebased onto: an edit of the old patch is
     dropped with the drag, and the page shows the new one.

   *One small behaviour change, deliberate.* A node can no longer be
   resized past `kMaxLoadedNodeSize` (4000 world units) on the canvas
   (`gui::patch::resizeNodeTo`). The loader has always clamped a saved
   size there, so such a node already came back smaller after a restart;
   now that every commit goes through that loader, a larger live size
   would have been one size on the canvas and another in the engine.
   Likewise a frequency that is not a finite number (reachable, if at all,
   only by typing one - not checked whether ImGui's number box lets one
   through) is now refused - the edit is dropped and the page shows the
   engine's graph - where the direct write would have stored it.

   *The guard.* `patchGraph_` left `kWindowMayWriteUnscoped`, which is
   now empty (a `std::vector`, as `kWindowMayWriteScoped` already was).
   Rule 3's mutating-call pattern gained the graph's own mutators
   (`addNode`, `addNodeAs`, `removeNode`, `mutableNode`, `connect`,
   `disconnect`, `reserveIds`) - none is a container call, so without
   this a direct `engine_.patchGraph_.addNode(...)` would still have
   passed. Proven with a probe in `drawToolbar`
   (`(void)engine_.patchGraph_.mutableNode(1);` and
   `engine_.patchGraph_ = Graph{};`): 2 violations; with the new pattern
   removed, 1 (only the assignment); probe removed, 0 violations, 107/107.
   Review round 2 closed the rest (see below): `patchGraph_` may be bound
   only to a const reference and passed only to a whitelisted helper that
   takes `const Graph&`.

   *Tests.* `tests/test_patch_graph_draft_input.cpp` (new) drives the
   real `drawPatchView` through ImGui's input queue and reads the ENGINE's
   graph: the parts bin, a drag, a wire made and cut, a node deleted (the
   middle one - the others keep their ids), ALL OFF and a new document
   followed by the page, a drag during two engine changes, a draft the
   engine refuses, and a never-copied draft that must not be sent.
   `tests/test_patch_graph_command.cpp` (new): the document round trip
   exact (144 800 000.25 Hz and awkward floats, the first node included),
   the command's round trip (ids, next id, chosen centre), the rebase's
   cases, `addNodeAs`. `test_apply_command` covers the op (long text,
   ids, epoch, seven refused texts, graph unchanged after each);
   `test_patch_view_math` the resize ceiling. Every one was broken and
   seen red before being restored; mutants run: no command applied, no
   cut on Delete, publish before the canvas, no in-view commits, no
   rebase, re-seed instead of rebase, no re-seed, dropped lines accepted,
   float-precision frequencies, six-digit positions, ids remapped, rebase
   ignoring the engine, duplicate ids, no next-id, no chosen centre, no
   resize ceiling. Two did NOT fail on their own in that round; the review
   (round 2, below) made one a real test and explained the other.

   *Still open.*
   - **Design B, unchanged**: `patchReconcile`/`patchPublishSets`, the
     compile (`engine_.patchPlan_ = compile(...)`), `rebuildPatchCatalogue`
     and `patchApplyRunning` are still called from `drawPatchView`, and the
     "page closed stops everything" sequence is still `drawPatchPage`'s -
     all `kLineAllowed`. The patch still runs only while the page draws.
     Moving `patchPublishSets` into a pump phase needs its own answer to
     the lag measured above (a publish after the pump's own graph change,
     or accepting the frame).
   - **The window still READS `engine_.patchGraph_`** as a friend (the
     rail chip, the compile, the config save, the draft's own sync) - OPEN
     item 3's general "reads engine members directly", not a write.
   - **The guard's whole-object check is for `patchGraph_` only**
     (`kWholeObjectFields`). Any other struct-typed engine field could still
     be handed to a non-const helper unseen; none is written by the window
     today, and widening it to every field would flag every by-value use.
   - **No other sender exists yet.** Nothing but the window sends
     `FOXAPP_OP_PATCH_SET_GRAPH` (no transport carries app ops). A future
     API sender replacing the graph mid-drag without the document flag
     would be rebased onto field by field, id for id - correct for an edit
     of the same graph, not for an unrelated one; such a sender should set
     `FOXAPP_PATCH_GRAPH_DOCUMENT`.
   - **Windows**: the reviewer ran the round-1 commits under MSVC (225/225
     with a temporary fix for `near`); the round-2 commits have been run on
     Linux only. The canvas has still not been driven by a person on a
     desktop: the input test and the full suites are the evidence.

   **REVIEW ROUND 2 (branch claude/engine-patchgraph, on top of e29c1a7),
   for the Windows/MSVC review that rejected e19909a/e29c1a7 on one
   blocking finding.** Each item test-first, then broken on purpose:
   1. *(blocking)* `test_patch_graph_draft_input` declared `near()`, which
      windows.h defines as a macro (MSVC C2062/C2143). Renamed `nearly()`.
      Checked on Linux with the macros emulated (`-Dnear= -Dfar=
      -Dsmall=char -Dinterface=struct`): red before at that line, 0 errors
      after.
   2. *An abandoned wire drag stopped the page following the engine.* The
      canvas ends a wire only on the release edge, which it sees only while
      drawn. `patchInteracting()` is now true only while the button is
      held, and clears drag, resize and wire when the button is neither
      held nor being released this frame (not on the release frame itself:
      the sync runs before the canvas, which must still connect the wire).
      Test K: red (engine on=0, page on=1), green; clearing on the release
      frame breaks C, and no clearing leaves a phantom wire for K's
      second half to catch.
   3. *A new document mid-drag.* J0 (a DOCUMENT op) and J1
      (`loadPatchDocument`, which adopted the new graph without touching
      `patchUi_`) put a Map under the dragged node's id: J1 dragged it to
      200,80. `adoptPatchGraph()` now drops drag, resize, wire and
      selection whenever it adopts a new epoch. M4b on the reviewed code
      fails J0; with this fix in, M4b alone survives again - the reset
      covers it too - and the gate stays as a second line of defence.
   4. Test I's comment corrected (the rebase also stood between an empty
      draft and the graph); I2 (the engine's node store not replaced by
      the first flush) failed under M5. Since item 7, M5 is caught by I's
      "the draft starts from the engine's graph" instead: an empty draft
      now equals an empty base, so I2 can no longer tell the two apart.
   5. *The op repaired values.* `graphFromCommandText` now requires the
      text to be exactly what `graphCommandText` writes for the graph it
      reads as, so every repair the loader makes (squelch outside -120..0
      dB, rate outside 0..1e10 Hz, a size above 4000 or of 0 and below,
      and any line it would skip) is a refusal naming the node. The squelch
      slider is `ImGuiSliderFlags_AlwaysClamp`, so -200 typed after
      Ctrl+click is -120 held, as it was before this work (test S, through
      the input queue).
   6. *The guard.* `kWholeObjectFields` (`patchGraph_`): a control may
      bind it only to a const reference and pass it only to
      `kConstGraphHelpers` (each checked to take `const Graph&` in
      src/core). The reviewer's four probes - a non-const reference, then
      `seedDefaultPatch`, `std::swap` and `drawPatchCanvas` - were 0
      violations and are now 4; a call split over two lines is seen; the
      probes were removed and the tree is back to 0 violations.
   7. *Cost.* `core::patch::graphsEqual` replaces the text comparisons:
      an idle frame, page open or not drawn, serialised 5 and 1 times and
      now serialises none; a drag frame writes 3 (the command, the
      engine's canonical check, the page's document text), down from 7.
      Test P counts them (`core::patch::serialiseCount()`).
7. **The TX dead-man's handle is tied to the frame loop** (core/transmitter.hpp:
   "A FROZEN WINDOW CANNOT LEAVE IT KEYED" - the TX thread unkeys if
   `tick()` stops for `kKeyAliveWait` = 1000 ms). In 3a `tick()` runs in
   `Engine::pumpTransmitter`, called from drawUi, so a frozen window still
   stops the ticks. **If 3b moves the pump to the control thread, a frozen
   WINDOW no longer stops `tick()`** and the rule silently weakens to "a
   frozen control thread". The page's key request (pageLive, latch press,
   PTT held) must carry the window's own liveness - e.g. a GUI-frame stamp the
   control thread checks before honouring a held key - and this must be
   settled before the pump moves (ENGINE-EXTRACTION.md stage 3's TX item).
   **3b-pre (2026-09-28, docs/engine-merge-0.99.42.md):** the DESIGN problem
   above is unchanged and still 3b's to settle - nothing here moves the pump
   or adds a liveness stamp. What is new is a red/green BEHAVIOURAL check for
   the property 3b's redesign must preserve:
   tests/test_transmit_dead_man.cpp drives a headless `Engine` (a fake
   `IqSink` behind `transmitter_`, no window) through `pumpTransmitter` and
   asserts the key is released WITHIN ONE PUMP of `pageLive` going false -
   for a latch and for a held PTT - rather than only eventually, at
   `kKeyAliveWait`. The gap this closes: the existing net,
   test_transmit_page.cpp, proves WHERE `transmitter_.tick()` is called by
   scanning source text for the three lines, so `if (pageLive)
   transmitter_.tick();` still passes it (the text is still there, just
   wrongly gated) while silently changing "the page closed" from an
   immediate release to "wait for the frozen-window bound" - exactly the
   kind of regression a 3b rewrite of this function could introduce without
   any existing test noticing. Proven red against that exact mutant (5/15
   checks failed) while test_transmit_page.cpp stayed green against it.
   **CLOSED, engine/stage3b-pre stage-3b-pre-end round, item 1 (branch
   claude/engine-patchgraph, 2026-09-29): the design problem above is
   settled BEFORE the pump moves.** (The note just above - "nothing here
   adds a liveness stamp" - described the previous round.)
   - **(a) Two liveness stamps, checked by the TX thread itself**
     (core/transmitter.cpp threadBody). `controlAliveMs_` (was
     `lastTickMs_`) is stamped by `tick()`, the control side;
     `frontEndAliveMs_` by `Transmitter::frontEndAlive()`, one atomic store
     a frame from the window. The TX thread opens the key if the control
     stamp is more than `kKeyAliveWait` (1000 ms) old, OR if a LOCAL key
     (PTT, LATCH) is asserted and the front-end stamp is more than 1000 ms
     old. A remote key does not depend on the window and is not dropped by
     it. The remote hold (`kRemotePttHoldMs`) is enforced by the TX thread
     too; `tick()` no longer does it. And whenever NO key is asserted any
     more (PTT let go, LATCH opened, page closed, remote hold out), the TX
     thread lowers the key itself, with its tail (the ramp plays, then the
     sink's `finish()`), without waiting for `tick()`.
   - **(b) The page's key request is a latest-value slot**
     (`gui::TxPageRequest`, engine/tx_page_key.hpp: pageLive, pttHeld,
     latchPressCount, frameSeq). The LATCH press is a COUNT: the control
     side acts on the presses since the count it last saw - an odd number
     toggles, an even number does not - so no press is lost or doubled
     however frames and pumps interleave. Two halves:
     `Engine::submitTransmitPageKey` (the front end: the liveness stamp,
     KEY-UP applied at once - page not live clears PTT and LATCH, PTT let go
     clears the PTT, direct atomic stores - and the request into the slot)
     and `Engine::pumpTransmitter()` (the control side: key-DOWN from the
     newest request via txPageKey, then the tick). drawUi calls both, in
     that order, where it called the one pump before; in 3b the second
     moves to the control thread and the first stays.
     `pumpTransmitter(pageLive, latchPressed, pttHeld)` remains as both
     halves in one call, for Engine::pump and tests. A LATCH press that
     OPENS the latch is still applied by the control side: whether a press
     means on or off is only known against the transmitter's own latch,
     which the TX side releases on its own (failsafe, fault); deciding it on
     the front end would race those. The releases that matter for safety -
     the page going away and the PTT let go - do not wait.
   - **(c) The remote key's standing conditions are the engine's.** Closing
     the page through `FOXAPP_OP_SET_TRANSMIT_PAGE_OPEN` already released it
     in the same step (2b). The window's per-frame `transmitOpen_` poll in
     applyWebControls is gone: `Engine::pumpTransmitter()` checks it. The
     web server (the window's until stage 5) now says when it is not running
     with a new op, `FOXAPP_OP_WEB_CONTROL_STOPPED` (0x841C), sent in the
     same step `applyWebSettings` stops it and once a frame while it is not
     running; the window no longer touches `transmitter_` for either.
   - **(d) Shutdown order**: there is no control thread in 3a, so
     "transmitter stop before the control thread's join" holds trivially
     today; the rule is written into `Engine::shutdown()` with OPEN 9
     (see item 9).
   *Tests, each red first or against a mutant.* The reviewed mutant
   (`if (pageLive) transmitter_.tick();`) was ALREADY caught by
   tests/test_transmit_dead_man.cpp (3b-pre; re-verified 5/15 red before
   this round, while test_transmit_page stayed green against it); its new
   form (`if (r.pageLive) transmitter_.tick();`) fails 8/40 now.
   tests/test_transmit_liveness.cpp (new): control ticking with the front
   end stopped - PTT (A) and LATCH (B) released at ~1008 ms (bound 1000 +
   one block), red before (never released); remote keyed with the front end
   silent (C) - still keyed at 1.5 s, released at the 2 s hold; control
   stopped (D) - ~1014 ms; both alive (E) - no false release. Mutants: no
   front-end check (8 liveness + 2 dead-man checks fail); no remote-hold
   check on the TX thread (4 + 8 in test_transmitter). test_transmit_dead_man
   4-7 (new): key-up with NO pump quiet in ~2 ms with its tail (red before:
   1011 ms, cut); two presses one pump / one press two pumps; front end
   silent while the control pumps (~1014 ms); the engine pump releasing a
   remote key with the page shut. Mutants: no play-down (4 fail), a press
   read as a flag (4), presses not remembered (2), no front-end key-up (4),
   no standing condition (2 + 1 in test_remote_key_release), the web op a
   no-op (2 in test_remote_key_release, 1 in test_apply_command).
   test_remote_key_release gained E (the settings panel switching the
   server off releases in the same step) and its A now pumps the engine.
   test_transmitter's and test_transmit_page's simulated frames stamp both
   sides (a 3a frame is both); test_transmit_page's source scan now pins
   the two halves (key-down written once, in pumpTransmitter, before the
   tick; every other engine write a key-UP `false`).
8. **The hang watchdog is the GUI frame's.** The engine pauses it (through the
   host) around its bounded waits (audio/mic open, plugin rescan, device
   open). On a control thread those waits no longer block the frame; they
   need a watchdog of the control thread's own, and the GUI watchdog must stop
   being paused by them.
9. **`run()`'s teardown is still spelled out in the window**: transmitter
   stop, GPS stop and last poll, recordings stopped, bookmark flush, pipeline
   stop, the clean-exit mark, patch stop and plugin unload, between the
   window's own steps (config save, crash upload, update reap). Reviewed
   lines in the guard. 3b: one `Engine::shutdown()` the window calls, ordered
   against the control thread's join.
10. **`applyWebControls` stays in the window** (the web and CAT servers are
    the window's until stage 5): it drains them and calls
    `engine_.applyControlRequest`. **HALF CLOSED, engine/stage3b-pre 2b
    (2026-09-28):** the "Transmit page closed" release now happens inside the
    engine itself, in the same step as `FOXAPP_OP_SET_TRANSMIT_PAGE_OPEN`'s
    own handler (section 1's CLOSED note above) - `applyWebControls`'s
    matching check is kept as a harmless second line of defence, not removed.
    The "web server stopped/disabled" release is UNCHANGED and still lives
    here, calling `engine_.transmitter_.releaseRemote` directly: it cannot
    move into the engine until the web server itself does (stage 5), since
    the engine has no way to observe "the server was just disabled in the
    settings panel" on its own.
11. **Moved helpers keep namespace `cascade::gui`** (the src/engine headers
    and plugin_store_reasons, receiver_tables' shared locals), so the moved
    code reads exactly as it did; engine .cpp files say `using namespace
    cascade::gui`. Renaming to `cascade::engine` is a mechanical follow-up.
12. **Friend access until stage 4**: `Engine` befriends `gui::AppWindow` and
    `gui::AppWindowTestAccess`; its public surface is applyCommand,
    submitCommand, drainLocalCommands, publishReceiverState, the life cycle
    (initialise, loadBookmarks, stopTransfers, teardown), applyConfig,
    fillConfig, the pump phases, pump and snapshot.
13. **CatServer::stop gives up after 5 s and leaves threads that captured
    `this`** (pre-existing, src/net/cat_server.cpp). stop() shuts down every
    client socket and waits for `clients_` to reach 0 with a deadline of
    `kClientDrainTimeoutMs` = 5000 ms; past it, it logs "N connection
    thread(s) still running after 5000 ms; giving up waiting" and returns.
    The client threads are `std::thread([this, c]{ serveClient(c); ...
    clients_.fetch_sub(1); }).detach()` (acceptLoop), so one still running
    after that return touches the CatServer - and `catStatusNow` through its
    provider, which captures the window - after ~AppWindow may have destroyed
    both. Each thread ends within one poll interval once `stopping_` is set,
    so reaching the deadline needs a thread wedged in a send/recv the socket
    timeouts do not bound. Not changed in 3a. Fix: the client threads hold a
    `shared_ptr` to a state block that outlives the server (or stop() joins
    them with no deadline and the send/recv timeouts become the bound).
14. **Reorders made in 3a** (all verified to change nothing observable in
    the suite; recorded because they are reorders): the web and CAT listeners
    start after the VFO restore instead of before it; the store URL box, the
    web/CAT providers and the config writer's bind are set after the audio
    and microphone gates are bound; the window's view state in applyConfig is
    restored after the whole engine half instead of interleaved.
15. **~Engine's own teardown is not independently observable** in the
    headless test: a Recorder finalizes itself in its destructor, so a take
    left running is finished either way. It is exercised (the first engine
    in the test is destroyed without teardown) but no check can go red on it.
16. **Linux `test_bias_key_run` failed once in 7 full-suite runs on HEAD**
    (-j 8, xvfb, software GL), passed in the other 6 and 16 times alone; 6
    full runs of the base were clean. Its output was lost (section 7). Before
    3b, run it with --output-on-failure under -j 8 until it fails again and
    read what it says; treat it as unexplained until then.
