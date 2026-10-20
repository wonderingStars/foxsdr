# Engine extraction, stage 4: the engine behind the C table - design, and what it needs first

Stage 4 of the extraction plan: the Engine exports `FoxEngineApi`, and
`gui::AppWindow` reads and writes only through it - `read_state` once a frame,
`read_spectrum` once, one `submit` a frame - enforced by building the interface
as its own CMake target that cannot see the engine headers.

**Status (2026-09-29): not started in code. Stopped here on purpose**, because
implementing the table faithfully changes who may key the transmitter and when
a control takes effect, against a specification that is only partly in this
tree. This document is the design and the list of what is missing.

## 1. What the tree already has

- **The table itself**: `third_party/foxsdr_api/foxsdr_api.h`, API 0.2 (draft),
  vendored unmodified from foxsdr-api commit 8fb6522 (third_party/foxsdr_api/
  README.md). `FoxEngineApi` has create/destroy, open_session/close_session,
  heartbeat, read_state, read_spectrum, submit, poll_results,
  subscribe/poll_events, read_audio, read_iq, read_list, get_setting,
  read_image, login/logout.
- **The state**: the engine already publishes a `FoxReceiverState` every
  frame (`core::PublishedState::rx`, stage 2) through the receiver snapshot,
  lock-free for readers; engine-stage2.md section 2 maps every field and
  lists the unfilled ones (OPEN 1-9 there).
- **The vocabulary**: every change to the receiver is already a `FoxCommand`
  (`FOXAPI_OP_*`, plus `FOXAPP_OP_*` app extensions 0x8000-0x8FFF,
  src/core/app_commands.hpp), applied by `Engine::applyCommand`.
- **The threading**: a thread-safe command queue and the engine's own
  control thread (engine-stage3.md section 10), so an in-process `submit` can
  be a queue push and the engine's pump never waits on an interface.

## 2. What is missing, exactly

1. **docs/API.md** (foxsdr-api). The header defers to it for: the command
   reference (which slots each op reads, its grant, its result's `applied[]`);
   the meaning of `value[]` per list ("Lists"); the patch document format
   (`FOXAPI_OP_PATCH_LOAD`); "Streams" (the spectrum's history, audio
   cursors); the "Transmitter" section behind THE KEY; the coverage table
   (which app feature maps to which op/list/state field, and which are
   gaps). Without it a second implementation guesses.
2. **The mock engine and its tests** (foxsdr-api `src/mock_engine`, named in
   `FoxEngineParams`). They are the reference behaviour for tickets, BUSY,
   merging of the five safer kinds, results in ticket order, "a result is
   never readable before the snapshot that shows it", and the latch
   protocol. Porting its conformance tests to run against the real engine is
   how stage 4 would prove it implements the same table.
3. **docs/TRANSPORTS.md** (for stage 5/6: the remote client library, local
   token file, keep-alive over a socket).

## 3. Decisions the owner has to make first

1. **The transmit key model.** The table's rules differ from the app's today:

   | | app today (core/transmitter.hpp) | API 0.2 (THE KEY) |
   |---|---|---|
   | local PTT | a level the window sets every frame; released by a frame without it, or a front end silent for 1 s | `TX_PTT 1` is a 2 s HOLD the interface re-asserts; `TX_PTT 0` releases now |
   | latch | one latch; the LATCH key toggles it; 60 s failsafe | owned by a PRINCIPAL; any end other than the owner's `LATCH 0` MARKS the principal until that session sends `LATCH 0`; 1 s re-arm after any end; edge-triggered key protocol (`LATCH 0` + `LATCH 1` in one batch) |
   | remote PTT | allowed while the Transmit page is open and a transmitter is open (implicit consent) | needs `TX_REMOTE_ARM 1` from a LOCAL session; lapses when it disarms, closes or goes silent |
   | keep-alive | one front-end stamp, one control stamp | per session: 1 s local, 2 s remote; only `heartbeat()` counts |
   | who holds the key | nobody in particular | per session (`TX_KEY_MINE`) |

   Implementing the table means adopting the right-hand column, which
   changes who may key and when - each rule needs a test shown red and then
   green, and core/transmitter.hpp's header rewritten to match. The owner
   should confirm the API's model is the one to ship (or that the table is
   implemented over the app's model with the differences reported as
   gaps).
2. **Frame semantics.** `read_state` once a frame and one `submit` a frame
   means every control lands one control pass later than today (a key press
   and its effect in the same frame is the app's behaviour now; stage 2
   section 6 declined to change it piecemeal and deferred it here). Accept
   the one-pass lag, or specify how the window should wait for its own
   batch.
3. **What the table has no read for.** The window draws things the table
   does not carry (from the ratchet's scan of its engine reads):
   - the plugin windows (`pluginUi_`, 40 reads): plugin UI code the window
     runs on its thread, through the plugin ABI's host API - these belong to
     the host process, not to an interface, and need a home;
   - the Patch page: the graph structure, per-node spectra
     (`patchSpectra_`), decoded pictures, the device choices (`patchPlan_`,
     `patchRadios_`, `patchCatalogue_`: ~60 reads) - `FOXAPI_OP_PATCH_LOAD`
     writes a document, nothing reads one back;
   - the store's entry details (legal notices, install-blocked reasons,
     planned updates), converters, the bias-tee key, the Airspy gain modes,
     the scope taps (`FOXAPI_CAP_SCOPE` "reduced"), the basemap and
     track-info plugin APIs, the status lines (`EngineStatusText`), the
     recorder paths and start times, the config snapshot.
   Each needs a list, a setting key, an event or an API addition - an
   upstream change to the draft (it is 0.x, and the owner may reshape it),
   not something to invent in this tree.

## 4. The plan once those are settled

1. **The engine exports the table** (`engine/fox_engine_api.cpp`,
   `foxsdr_engine_query`): create/destroy over an Engine with its control
   thread; sessions with grants and flags; `heartbeat`; `read_state` from
   the published `FoxReceiverState` (per-session fields filled per read);
   `read_spectrum` from the published spectrum; `submit`/`poll_results`
   with tickets, BUSY and the safer-command merge; capability bits only for
   what is implemented. Tested by the mock engine's conformance suite run
   against it.
2. **The transmit key model** per decision 1, behind red/green tests.
3. **Lists** (`read_list`) for the collections the window draws.
4. **The window's port**, group by group, onto one `read_state`, one
   `read_spectrum` and one `submit` a frame - with stage 3's remainder
   (engine-stage3.md section 10, item 4): the window's pump moves to the
   control thread in the same step, because the table is what makes its
   reads safe.
5. **The fence**: the interface built as its own CMake target that sees only
   `foxsdr_api.h` and app_commands.hpp, so a direct engine read no longer
   compiles; tests/test_command_path_guard.cpp's ratchet reaches zero and is
   retired.

Stage 5 (web and CAT as sessions) needs 1 and 2; stage 7 (the headless
foxsdr-engine) needs 1 and stage 5's transports to take commands from.
Stage 6 is skipped, as asked.

## 5. What the measurement run should watch, when this lands

Frame time and tune-to-audio latency (a control lands one pass later; the
pass period sets the added latency), GUI-thread stalls (the window no longer
runs engine waits in its frame - should improve), CPU at a fixed rate (one
control thread's passes added, the window's engine work removed), memory
(per-session rings and result queues). Nothing here touches the sample path.

## 6. Record: what was built, in order

The owner's decisions (2026-09-29): 1 ADOPT API 0.2's key model; 2 ACCEPT
the one-pass lag, keep the pass short and say what it is; 3 API.md, the
reference engine and its tests are the specification, and its conformance
suite must pass against this engine; 4 EXTEND the draft for what the table
has no read for, in a new vendored header version (section 3, item 3).

### 6.1 Closed first (patch 1)

- A page close ends every LATCH press made before it (the Opus review of
  47e991e: a press, a close and a reopen inside one pump latched on the
  reopen) - tests/test_transmit_key_race.cpp F, red 3/3 then green. Since
  step 2 the slot it was in is gone; F is held again on the session path.
- hang_watchdog (Linux): one signalled capture at a time, each tagged, so
  two watchdogs capturing at once never read each other's stacks -
  tests/test_hang_capture_concurrent.cpp, red 12,319 of 12,349 wrong.
- Engine::stopControlThread waits at most kControlJoinWait (3 s) for the
  control thread's last pass before its join, and says so in the diagnostics
  when it had to.

### 6.2 Step 1: the engine exports the table

engine/fox_engine_api.{hpp,cpp} (the host: sessions, grants, tickets, BUSY,
the safer-command merge, results after the publish in ticket order, events,
logins), engine/engine_api_lists.cpp (the list block, rebuilt only when its
fingerprint moves). `beginPass()` runs in Engine::pumpFrameBegin after the
local drain; `endPass()` in pumpPublish after the snapshot. The C table is
`foxsdr_engine_query` / `engineApi()`; the host's own Engine hands out
`apiHandle()` for the window's session.

The reference's suites, ported (tests/test_engine_api_conformance.cpp,
test_engine_api_transmit.cpp, test_engine_api_merge.cpp; the reference's
comments kept). Every difference is marked PORT in the file; they are:

| where | reference | this engine, and why |
|---|---|---|
| test hooks | `mock.*` settings | `test.*`, only with `testHooks=1` in the options - never set by the product |
| DECODER_START of one not installed | OK | NOT_FOUND: the real engine hosts decoders; nothing changed, so a refusal |
| SCAN_DEVICES, SELECT_SOURCE | synchronous | on a worker (an open has taken 57 s in the field): OK means started; `listSeq` / DEVICE_OPEN say when it landed |
| SET_SAMPLE_RATE off the list | OUT_OF_RANGE | OK + CLAMPED with the rate that landed: the radio coerces, as real drivers do |
| audio stream, settings store, decoders | served | capability bits clear, calls UNSUPPORTED - not served through the table yet |
| the merge sweep | a fresh engine per case | one engine per worker, reset between cases (`test.resetKey`); 5450 cases by default (4450 exhaustive: 3 starting states, every case of 1-3 commands from sessions A/B/C over LATCH0, LATCH1, RUN0, TX_CLOSE and PTT0, up to renaming; plus 1000 random of 5-8 commands, fixed seed); `FOXSDR_MERGE_FULL=1` runs 70450 (64450 exhaustive of 1-4 commands, plus 6000 random of 5-8) - run once: 0 differ, 584 s; re-run on the tree with the per-worker engine fix: 0 differ, 83 s on 24 threads (the default sweep: 6 s) |

Red first: 72 conformance failures before the host existed; 14 session
mutants each red.

One regression of this step, found by the stage boundary's full run and
fixed on its own (4e0679a): SET_GAIN's new NOT_FOUND for an unlisted stage
refused an Airspy's stage of ANOTHER gain mode, which is how the web remote
switches it from Linearity to Free (tests/test_airspy_app, 10 checks red).

### 6.3 Step 2: THE KEY (decision 1)

- core/tx_key_rules.{hpp,cpp}: the rules on their own, clock injected
  (tests/test_tx_key_rules.cpp, 120 checks; a stub red 65/120; 20 per-rule
  mutants, each red).
- The host applies TX_PTT / TX_LATCH / TX_REMOTE_ARM through the rules and
  drives core::Transmitter from them once a pass (ApiHostImpl::txPass): the
  latch and a local PTT's hold as levels, a remote hold bought when applied.
  The Transmitter's TX thread stays the backstop underneath, with its own two
  stamps: the FRONT END is now the key holder's heartbeat (not a frame's
  request), the CONTROL side is the pass.
- The window is a LOCAL session: its heartbeat opens each frame, the
  Transmit page is drawn from its read_state, and gui::txKeyCommands turns
  the page into commands (PTT re-asserted while held, PTT 0 once; the latch
  edge-triggered, LATCH 0 + LATCH 1 in one batch; a page that goes away lets
  go of its PTT, its latch or a press not yet landed, and its consent).
- The engine's page-key slot (engine/tx_page_key.hpp, submitTransmitPageKey,
  pumpTransmitter) is deleted.
- The web remote is an engine-internal REMOTE session (principal "web"): its
  keep-alive is the page's status poll (2 s), its PTT a 2 s hold, and it keys
  only with a LOCAL session's consent. A server that is not running ends its
  key in the same step.
- core/transmitter.hpp's header rewritten to the rules.

What the operator sees change:

- **"Allow remote transmit"**, a new switch on the Transmit page (all 33
  catalogues). The web remote's PTT does nothing without it; unticking it,
  closing the page or the window stopping releases a remote key at once.
  The web page's "not available" text says so.
- The Transmit page's open flag no longer gates any key by itself.
- After ANY latch ends, no latch closes again for 1 s (a press inside that
  second is refused and the key shows why).
- A key press lands one engine pass after the frame that made it (decision
  2): while the engine is pumped from the window's frame (until step 4) that
  is the next frame, ~16.7 ms at 60 Hz; on the control thread (16 ms period,
  woken at once by a submit) it is at most one pass.

Tests held again on the new path: test_transmit_dead_man (8 scenarios, end
to end through a real Engine and a ledger radio), test_transmit_key_race
(A-F), test_remote_key_release (consent, server stopped, page closed),
test_transmit_page (the command wiring), test_engine_control_thread,
test_engine_shutdown, test_apply_command's TX coverage, and the reference's
safety suite (test_engine_api_transmit, 658 checks, about 95 s at the real
timers). The integration was proven by mutants of txPass, the heartbeat
stamp, the engine's key ends (RUN 0, TX_OPEN, TX_CLOSE, fault), the web
server's release and txKeyCommands' close path - each red. Two survived the
first round and each got the scenario it lacked: the heartbeat's stamp on
the TX thread (dead-man 8: a latch held 2.5 s with everything alive must not
drop), and the rules being told when the web server stops (remote-key B now
reads the rules in the same step, not after a pass). The two ported suites
run alone (RUN_SERIAL): on a 4-core runner beside each other a probe's
heartbeat starved past its 1 s keep-alive and the rules rightly ended the
key.

What could move a measured number: one pass of `txPass` (the rules' tick, a
few stores; it allocates only when a TX_UNKEYED event is raised), and the
window's `read_state` (one sequence-locked copy of FoxReceiverState a
frame). Nothing on the sample path changed.

### 6.4 Step 3: the lists, and API 0.3 (decision 4)

The vendored header is now API 0.3 (third_party/foxsdr_api/README.md says
how: the 0.2 copy, extended here, everything appended), and every addition is
specified in `docs/api-0.3-additions.md` in API.md's style, to be merged
upstream. In short: the 0.2 lists step 1 left out (DECODERS, CATALOGUE,
TX_DEVICES, MARKERS, BAND_PLAN, PRESETS, USER_PRESETS, DEVICE_OPTIONS,
SERIAL_PORTS) and fourteen new ones (status lines, converters, gain modes,
decimations, the patch graph's nodes, wires, problems, parts and radios, the
recordings, the decoder-output backlog, the store's refusals and updates, the
band plans); read-only FACTS through `get_setting("fact.…")` for whole texts
and single values (a status line, the patch graph as a document, the
recorder's folder and files, a store entry's legal notice and blocked
reason, the transmitter's address, the converter's name, the bias tee's key);
`read_scope` (the demod scope's three taps, "reduced" for REMOTE),
`read_node_spectrum` (a patch radio's own spectrum), `submit_text` (one
command with a text over 255 bytes: a patch document); three state flags
(bias tee present/on, patch running); FOXAPI_EVENT_PATCH;
FOXAPI_OP_SCAN_SERIAL_PORTS. TRACKS stays a GAP.

HOW IT IS SERVED, and why it costs the pass nothing new: everything but the
live figures is in the list block (step 1's), rebuilt only when its
fingerprint moves - the fingerprint now also hashes the store, the status
lines, the graph (from its fields: no document is written for it), the
recorders' state, an Airspy's choices; nothing in it allocates, and the
plugins' keys are cached (they were being rebuilt every pass before - that
per-pass allocation is gone). Which decoders are FED comes from the keys the
publish already takes from the runner in ONE acquisition of its lock (the
lock its DSP thread takes per block): step 3 takes that lock no more often. A
recording's byte count and its STALLED/FULL state are read LIVE from the
recorder's atomics at read time, so a growing file never rebuilds the block.
The decoder-output backlog goes into a fixed ring in the host (500 lines,
fixed-size text), so a busy decoder never allocates per line on the pumping
thread. The scope taps are the pipeline's lock-free rings, snapshot from the
reader's thread. Each patch radio's spectrum gets a ring like the main one,
made when the radio first publishes.

UNTIL STEP 4 some of what the patch lists show is decided by the WINDOW: it
still compiles the patch plan (`patchPlan_`), marks refusals and drives
`patchReconcile` from its frame (the survey behind step 4 found it writes
`patchRunning_`, `patchPlan_` and `patchRefused_` itself). The lists read
those members as they are; step 4 moves that work into the engine's pass,
and the lists do not change.

Tests: tests/test_engine_api_03.cpp (166 checks; red first against stubs,
111 of 157 failing), one group per addition, through the C table only. Twelve
mutants each red (the long text dropped, LOCAL-only facts and paths served
to a REMOTE session, the byte count taken from the block, the scope not
reduced for REMOTE, the patch event never raised, the serial scan allowed
remotely, the decoder lines misnumbered, the empty status lines listed, the
graph or the status lines left out of the fingerprint, the transmitter's
seed not served) - four of them only after the test was tightened, each
noted in the file. test_engine_api_conformance serves 0.3 now (PORT: the
version gate one minor on); test_receiver_snapshot walks the three new
flags; test_apply_command covers SCAN_SERIAL_PORTS.

THE PLUGIN WINDOWS stay in the host process, and get no API. A plugin's
panel, instrument, track list and the host-drawn plates around them are
plugin code run through the plugin ABI (core::PluginUi, host API level 1),
and a plugin that draws is an extension of the HOST, not an interface a
user swaps. They are already fenced from the engine's state on one side:
their reads of the receiver are the ONE published snapshot
(core::PluginApiCore, stage 2), and their changes are commands
(pluginControlToCommand, stage 1). The other side - the window reaching into
`engine_.pluginUi_` to draw them (40 reads: tracks, panels, instruments,
the tune/settings grants) - is fenced in step 5: the interface target will
not see core/plugin_ui.hpp at all; the host process owns the plugin windows
and hands the interface one opaque hook ("draw the plugin windows now", on
the GUI thread inside the interface's frame, taking no engine type and
returning only what was drawn), so no engine member can be read through it.
The engine's pass never waits on it.

What could move a measured number: the fingerprint is longer (a few hundred
bytes hashed a pass more - the store's ids, fourteen status lines, the
graph's nodes); a block rebuild now copies more (the band plan's entries,
the store) - but only when something in it changed. Nothing on the sample
path changed; the DSP thread's locks are taken no more often.

### 6.5 Step 4: the window's port, group by group

THE PATTERN, set by the first group (the Transmit page and its rail key):
the member draws from the window's session only - `uiState_` (read once a
frame, after the publish), lists, and FACTS through a cache that is dropped
when `listSeq` moves (`AppWindow::uiFact`) - and changes things only by
`AppWindow::uiSubmit` (one command on the session, its long text through
`submit_text`; the answers drained once a frame with the key's). What the
table had no read for is added to API 0.3 under decision 4, documented in
docs/api-0.3-additions.md, served by the engine and held by
tests/test_engine_api_03.cpp; engine work the window used to call from its
frame moves into the engine's pass. tests/test_command_path_guard.cpp lists
every ported member (`kPortedMembers`): one that names `engine_.` at all
fails, and the OPEN 3 ratchet is lowered to the new count.

GROUP 1, the Transmit page (drawTransmitPage, drawTransmitSection): API 0.3
section 8.1 (the transmitter as a page draws it: split, input, tone,
monitor, the board's power range, the microphone opening and its peak, the
device, a fault, the last error; `txFrequencyHz` is where it would transmit,
`txPowerDb` the setting when no board is open). The frequency-follow the
page called every frame is the engine's pass's now (Engine::pumpPublish).
The page's open flag is the window's own (it is the window's page), told to
the engine by FOXAPP_OP_SET_TRANSMIT_PAGE_OPEN for the web status and the
save. Ratchet: 163 fields / 788 reads -> 152 / 761. What the operator sees:
nothing new; a setting lands one pass after the click (decision 2), as the
key already does.

A flake the full run found on the way, in the port of the reference's
safety suite, not the product: "three RUN 0 merged into one pass" read the
state 400 ms after the last result - enough for the mock's instant stop, not
always for a real pipeline's (a DSP-thread join after the rest of a 200 ms
stall). The port now waits for the merged ticket's own result (PORT, noted
in the file). A second, older flake, not stage 4's: test_main_view clicks
the real application at fixed frame numbers and misses under several
software-rendered GUI tests at once - patch 1's tree fails the same way under
the same mix (2 of 3 runs; this tree 1 of 3), and it passes alone and under
plain CPU load (5 of 5 each).

A Windows finding on the ported merge sweep, not the product: on this
machine test_engine_api_merge never reached its summary line, because each
worker kept its engine in a `thread_local`. MSVC destroys thread-local
objects inside LdrShutdownThread with the process loader lock held; the
engine's destructor joins the control thread, whose last act joins the
transmitter's TX thread, and no thread can finish exiting without that
lock - so 23 workers queued behind the one that held it (two full dumps,
the lock's owner waiting in the join in Engine::stopControlThread). Linux
has no loader lock around TLS destructors, which is why the sweep passed
there in about 45 s. Each worker now owns its engine on its own stack and
destroys it before the thread function returns; the product never destroys
an engine from a thread-local destructor. THE RULE FOR TESTS: never keep
anything whose destructor joins a thread in a `thread_local` - on Windows
that deadlocks at thread exit.

GROUP 2, the STATUS column (drawStatusColumn, and refreshStatusLists, which
keeps what it takes from the lists): API 0.3 section 8.2 (the status column as
a page draws it). What moved into the table: the sink's four states
(`audioFlags`: OPENING, EVER_OPENED, STREAM_ALIVE, PLUGIN_PLAYING), the output
ring's fill and size and the audio rate, the plugin audio path's gap counters
and who holds the speakers (`audioPlayingPlugin`), who mutes the audio
(`audioMutedBy`: the decoder plugins on their presets - the user's own mute is
`FOXAPI_RX_MUTED`), the decoder output total (`decoderLinesTotal`, which the
window differences over its own 2 s window), the decoders "ready" with the
receiver stopped (`decodersMatched`: not in the plan, and necessary -
`decodersRunning` is 0 then, and the card letters "N ready"), the gain's unit
token (GAINS `detail`: `dB` or `step`) and each take's elapsed seconds
(RECORDINGS `value[3]`, from two steady-clock stamps in `core::Recorder`, read
live like the bytes). The column reads `uiState_` once a frame and three
small things from the lists when `listSeq` moves (the first gain stage, the
antenna in use, where the I/Q and audio takes sit in RECORDINGS); the takes'
live figures are two `read_list` calls a frame and none before a take has
started. Ratchet: 152 fields / 761 reads -> 152 / 723 (the column's 38 reads;
every field it read is read elsewhere, so the field count stays).

What the operator sees: nothing new; a figure lands one engine pass after it
changed (decision 2). Checked against a control of the unchanged column:
card by card the same, in the stopped and the running states under isolation
(not reached there: a radio's antenna and gain lines, a take under way, a
mute, a playing plugin, a running decoder's rate - those are held by the
tests of the table and by the column's unchanged rules).

What could move a measured number: 312 bytes more copied per publish (the new
members of FoxReceiverState, and per `read_state`); the recorder's two
stamps (two atomic stores a take, one of them on the DSP thread when a take
ends at the size limit); nothing on the sample path. The runner's lock, the
one the DSP thread takes per block, is taken twice a pass whatever the
receiver is doing (`feedSnapshot`, and `audioPathSnapshot`, which reads both
gap counters and copies the playing name in one acquisition) - it was two
stopped (`audioGaps`, `audioGapFrames`) and three running - so the pass takes
it no more often; it also takes `feedSnapshot` while stopped (the "ready"
count), when no DSP thread is taking that lock. The window's frame no longer
asks the runner at all (it called `playingPlugin`, the two gap counters and
`isFeeding` once per decoder module a frame). Nothing in the publish
allocates (tests/test_engine_publish_alloc case C: a mute subject and a
playing plugin, 0 allocations in 40 publishes); the decoder modules' keys are
the cached ones.

GROUP 3, the panel notes (drawRadarSection, drawCatSection, drawWebSection,
drawUpdatesSection, drawKeyBindingsSection, drawDiagnosticsSection,
drawDiagnosticsOffer): seven members whose only reach into the engine was one
call a frame while the page was open, `engine_.telemetryNotePanel("<name>")`,
which lists the panel in the anonymous usage report (de-duplicated, capped at
16). New op `FOXAPP_OP_NOTE_PANEL` (0x841F; `text` is the name; grant VIEW,
which every session holds; LOCAL sessions only, as every app op is: a remote
session's pages are not this installation's usage; an empty name is
`BAD_ARGUMENT`; a repeat or a 17th name is an answered no-op). It is served by
the engine's existing `telemetryNotePanel`, so the report's list is built as it
was, and it is documented in docs/api-0.3-additions.md section 9. It has no
merge class: the merge rules (API.md 7.2) are for the five commands that can
only make the transmitter safer, with a fixed slot apiece, and a repeated name
is already free. THE TRAP was the frame: the pages call it every frame, and a
submit a frame would take a ticket a frame from the session's 256 and build a
command a frame. `AppWindow::uiNotePanel` therefore keeps a once-per-name set
(`uiNotedPanels_`, a sorted vector of at most 21 short strings, the window's
21 page names): a name already sent is one `std::lower_bound` against the C
string, with no temporary string, so no allocation and no session traffic
(tests/test_panel_note_once.cpp measures 1000 repeats of a 17-character name:
0 allocations on the calling thread; the first sighting: 3). All 21 call sites
in src/gui now call it - the seven above and the fourteen in members not ported
yet (audio filters, decoders, scope, plugin store, fitted modules, map, image,
decoded, recorder, bookmarks, scanner, usage reporting, privacy notice, serial
ports) - so the whole window notes its pages one way, and "telemetryNotePanel"
left the guard's `kControlMayCall`: a direct call from any control is a
violation again. The seven members and `uiNotePanel` join `kPortedMembers`.
Ratchet UNCHANGED, 152 fields / 723 reads: these were calls, not reads of an
engine field, so they were never in the count. What the operator sees: nothing;
the note reaches the engine on its next pass instead of inside the frame, and
the report's `panels` is the same list (tests/test_panel_note_once.cpp scenario
E reads it back from the usage record the engine writes: `"panels":["radar","cat"]`).

GROUP 4, the radio-specific panels (drawAirspyControls; biasKeyPanel,
biasKeyPressed, biasKeyAnswered, drawBiasKeyConfirm; converterAliasNote,
converterStatusLine, converterModesOffered, converterUnusableNote,
drawConverterControls - ten members in app_window_airspy.cpp,
app_window_bias_key.cpp and app_window_converter.cpp - and the two readers that
keep what they take of the lists, airspyLists and converterLists): API 0.3
section 8.3. What moved into the table:
- the Airspy's gain modes, decimation factors and stages were in the table
  already; what was missing is `FOXAPI_ITEM_AGC` on a GAINS stage its own AGC is
  driving (the LNA's and the mixer's, which the panel greys and whose two
  checkboxes it lettered from the published state);
- the Converter rows: the converter in force (`fact.converter.live`), the note
  about a SoapySDR fallback (`fact.converter.alias`: which of the two sentences,
  or none; the sentences stay in the window), the modes the source offers
  (`fact.converter.modes`), whether a stored Up goes unused on a sound card
  (`fact.converter.upIgnored`), the radio's own centre for "via ... - radio at
  ..." (CONVERTERS `value[3]` on the ACTIVE entry, read LIVE from an atomic the
  pass writes - the one place `rawSource()` is read, in engine_converter.cpp,
  where tests/test_converter_call_sites.cpp allows it), and
  `FOXAPI_RX_DEVICE_OPENING` for the rows' greyed state;
- the bias tee key: `fact.biasTee.reachable` includes the stand-in, the lamp is
  `FOXAPI_RX_BIAS_TEE_ON` whatever it is drawn over, the dialog's radio line is
  `fact.biasTee.radioName`, and the stand-in (FOXSDR_FORCE_BIAS_KEY) is the
  ENGINE's: the window knows none (run()'s one exempt line sets it, as before).
The pattern held, with one addition to it: the window keeps what it reads of
the lists in a small cache per panel, re-read when `listSeq` moves
(`airspyCache_`, `converterCache_`; `uiFacts_` now compares by C string, so a
fact read every frame builds no temporary), and `biasKeyPanel` hands back a
member rather than a copy (a `BiasTeePanel` holds a map, one heap node even
empty on this standard library, and the deck draws the key every frame).
tests/test_radio_panel_reads.cpp measures it: 4000 reads of an unchanged state,
0 allocations.

THE HAZARD THIS GROUP CLOSES (N9). The window decides from what it last READ;
the engine's pass, on its own thread once the pump moves, may have swapped the
radio before the command lands - and `submit` stamps the command with the
generation of the radio open AT SUBMIT TIME, so the host's "the radio this was
sent for has closed" drop does not see it. A yes for radio A would then have
switched radio B on, with nobody asked about B. `FOXAPI_OP_SET_BIAS_TEE` now
carries, in `text`, the radio the person was asked about (the deck's key puts
`fact.biasTee.radioKey` in every ON, the confirmed press as well as the yes) and
the engine refuses it, NOT_FOUND "the radio changed", when another is the open
one. tests/test_bias_key_app.cpp [14] makes the swap between the window's last
read and its submit, with the window's view deliberately stale, against the
shipping HackRF driver on a fake wire: no request reaches the wire; with the
engine's check removed, it does.

Ratchet: 152 fields / 723 reads -> 149 / 694 (the 29 reads of the ten members;
biasStandInOn_, converterKeyAlias_ and converterNotCarried_ are no longer read by
the window at all). The brief's "149 / 732" was computed from group 2's
starting 761; the tree was already at 723. `kControlMayCall` loses nothing to
this port: `currentAbsoluteHz` is still called by two unported members, and the
two entries with no caller in src/gui, `airspyRememberOpen` and `muteNameList`,
had none before it (left in place; their removal is not this group's).

What the operator sees: nothing new. A setting lands one engine pass after the
click (decision 2), the bias lamp and the converter rows likewise. Checked
against the unchanged window by the tests that drove the old members: the
bias key (its thirteen existing scenarios, [5]'s dangerous case among them), the converter rows'
alias note, the sound card's offered modes and unusable-Up note, the Airspy's
commands, and the real application with the stand-in key (tests/test_bias_key_run).

What could move a measured number: the list fingerprint, computed every pass,
gained the bias tee's reachability (a dynamic_cast chain and `deviceDead()`),
the memory's size, the stand-in, the radio's name (a strlen), the converter in
force (ONE more acquisition of the pipeline's lock; `converterAliasState` takes
two more, but only when the block is rebuilt), the alias set (empty in
practice), the sound card's device, host API and format (two strings and an
enum) and the language code - all hashed in place, no allocation (tests/
test_engine_publish_alloc is unchanged and passes); and the pass makes one
`rawSource().centerFrequencyHz()` and one relaxed atomic store, the same
readback it already makes through the air view for `centreHz`.

THE TEST-ACCESSOR TRAP, for the next group: a test that calls a ported member
without a frame reads the SESSION, not the engine. Its harness must run what a
frame runs - `engine_.publishReceiverState(); refreshUiState();` before the
member, and `engine_.pumpFrameBegin(); engine_.pumpPublish();` after it (the
old `drainLocalCommands()` applies only the window's local queue). The helpers
are `look` and `pass` in tests/test_bias_key_app.cpp and
tests/test_radio_panel_reads.cpp; the assertions of the tests that were
re-plumbed are untouched.

ONE TIMEOUT THE FULL RUN FOUND, not explained. The run after the port
(build\ctest-g4b.log) ended with test_engine_api_transmit at its 600 s timeout,
once; every other test took within 0.3 s of the run before, so the machine was
not loaded. The test is engine-only (it links no window). Re-run alone 11 times
it passed each time, about 101.5 s by ctest's clock, as in the logs of groups 2
and 3 (102.00 and 101.75 s). If it recurs: take a dump of the process while it
is still alive (`rundll32 comsvcs.dll, MiniDump <pid> <file> full` does not end
it) before ctest's timeout does.

GROUP 5, the receiver rail and the display (drawMenuColumn's footer,
drawRadioSection, drawStereoRdsControls, drawAudioFilterSection,
drawSinksSection, drawDisplaySection, drawBandPlanOverlay and
drawLanguageSection's country picker - eight members in app_window.cpp and
app_window_language.cpp - and the two readers they share, railLists and
readFactText): API 0.3 section 8.4, FoxReceiverState 1216 -> 1336 bytes. What
moved into the table:
- the RDS decoder as the Radio section draws it: `rdsFlags` (synced, PI, PS and
  RadioText valid, TP, TA), `rdsPi`, `rdsPty`, `rdsPs[9]`, `rdsRadioText[65]`
  and a counter of their own, `rdsSeq`, which moves `seq`; the texts are
  compared and copied as the C strings they are, raw bytes included. `rdsGroups`
  and `rdsErrors` are measurements and move no counter. All of it comes from the
  RDS snapshot the pass already takes: no extra lock;
- `FOXAPI_RX_AUTO_NOTCH_ENGAGED` (the mode group) and `autoNotchHz` (a
  measurement, 0 unless engaged: the pipeline keeps the frequency of the last
  tone it held);
- the audio watchdog's note under the Sinks section's device combo: the
  STATUS_LINES key and `fact.status.audioHealth` (level 2);
- two reads the brief did not list, found by the survey: `FOXAPI_RX_PILOT_LOCKED`
  (the stereo indicator draws three states, and the table had only the one that
  needs stereo enabled AND locked, so "ST (forced mono)" and the pilot figure
  could not be told from MONO), and `fact.bandPlan.name` (the Display section
  captions the plan LOADED, a chain joined with " + ", where the ACTIVE entry of
  BAND_PLANS names one plan, which would have changed the words);
- NOT made: the brief's R6 (the set of muting plugins). The one place the
  members read it is an emptiness test, and the set is empty exactly when the
  joined text (`audioMutedBy`) is; MUTE_CAUSES stays with group 8.
The pattern held, with one addition to it: the window keeps what it reads of the
lists in ONE cache for the rail and the display (`railCache_`, `railLists()`),
re-read when `listSeq` moves - the plans and the one in force, the audio outputs
and the one in use, the loaded plan's name and the two status facts - and the
loaded plan's ROWS (hundreds, walked every frame by the overlay and the footer)
only when their number, the plan in force or its name changes, because `listSeq`
also moves every time a decoder prints a line. The rules the members used to get
by calling the engine are restated in src/gui/rail_reads.hpp (the narrowest band
under a frequency, the bands a view shows, the mode key, the Language section's
note about the band plan) and held against the model they restate, on every
shipped plan and the merge of them all, by tests/test_rail_reads.cpp.

Ratchet: 149 fields / 694 reads -> 132 / 631 (the 63 reads of the eight
members; 17 fields leave: audioHealthNote_, audioOpen_, autoNotch_,
bandPlanChoices_, bandPlanSelection_, bandPlan_, bandwidthIndex_, deemphIndex_,
deviceIndex_, mutedBy_, notchEnabled_, notchFreqHz_, notchQ_, nrEnabled_,
nrStrength_, stereoEnabled_, vfoOffsetKhz_), the budgets exactly the printed
counts. `audioHealthNote_` joins kPublishedOnly, since the engine publishes it.
`kControlMayCall` and `kLineAllowed` lose nothing: `currentAbsoluteHz` and
`muteSubjectText` still have callers in unported members, and no entry names one
of the eight. The guard goes red on a single `engine_.` read put back into
drawRadioSection (mutant B1) and on the whole pre-port window (78 lines).

What the operator sees: nothing. Drawn by the group 4 build and by this one, at
a tuning with no band under it and at one inside a band-rich stretch of the
world plan, the rail (SIGNAL bank, receiver view) and the band plan ribbon on
the spectrum differ in 0 pixels at a tolerance of 24, area by area; two runs of
the same build differ by 76 to 217 pixels at tolerance 0, the noise floor. A
setting lands one engine pass after the click (decision 2), as before: the
session's queue is drained at the top of the next frame, before the publish and
before the window's read, right beside `drainLocalCommands` (read from the frame
order in drawUi; the click itself is not measured - scripted clicks do not reach
ImGui in this harness, so the other banks and the controls were not captured,
and tests/test_rail_reads.cpp F and G hold them). The seventeen commands keep
their op and argument text (16 identical, the 17th the same plan id from the
cache instead of bandPlanChoices_). The only site that depends on WHEN a command
lands is the Language section's note, "Display has since been set to a
different band plan"; the window now remembers what the pick asked for
(`bandPlanAsk_`) and the rule is one tested function. Two limits come with the
table's field sizes: an audio output's name is cut at the list item's 95 bytes
and `audioMutedBy` at 127.

What could move a measured number: FoxReceiverState is 120 bytes larger, copied
once per publish and once per window read; the publish compares the RDS texts
(74 bytes); the list fingerprint, computed every pass, hashes one more string
(`audioHealthNote_`) - all in place, no allocation (tests/
test_engine_publish_alloc case D: 40 publishes with RDS text, an engaged
auto-notch and a locked pilot, 0 heap allocations); the pass takes no extra lock
and reads the two auto-notch atomics and the pilot lock once. In the window, a
refresh costs the engine's own `get_setting` allocations, one for each of three
facts (tests/test_rail_reads D: 100 refreshes, 300 allocations; 1000 reads of an
unchanged state, 0), and it recurs on every pass while decoder lines flow,
because that is when `listSeq` moves; the rows are not part of it (25 refreshes
across 25 listSeq moves, rows copied 0 times).

THE TEST-ACCESSOR TRAP held: tests/test_rail_reads.cpp runs `look`
(`publishReceiverState(); refreshUiState()`) before a member and `pass` or
`passStatuses` (`pumpFrameBegin(); pumpPublish()` and the session's answers)
after. No older test needed re-plumbing: none of them called the eight members.
Test seams for hermetic runs, testHooks=1 only: `test.rds`, `test.autoNotch`,
`test.pilot`, `test.bandPlanDir` and `audioHealth` in `test.statusLine`
(tests/test_engine_api_03.cpp group R); the real peer for the RDS fields is
tests/test_engine_api_rds_real.cpp, the pipeline decoding a modulated carrier.

NOT UNDERSTOOD, recorded: while test_rail_reads was being written, an early
version of its drawing test (E: the members drawn in real frames) ended in a
SIGSEGV in its first drawing frames. The product code was not edited
afterwards; the test was rewritten (one frame per member, the audio filters and
Language sections opened by the capture hooks FOXSDR_OPEN_SECTIONS and
FOXSDR_OPEN_LANGUAGE) and has passed since, 30 of 30 runs of one pinned binary.
Three kinds of replay against the final harness do not crash (build\g5-segv-
exp*.log, five runs, the experiments not kept): the members drawn with the audio filters
before the stereo controls, the filters row open and closed; the early body
itself (Radio, Stereo, Sinks, Display, Language, Overlay in one frame, then four
frames with the filters before the stereo controls) with every row closed; and
that again with no fonts loaded. The cause is not established. The one
mechanism in the code that could do it is the drawer child `benchSection`
leaves open for the next rail primitive to end (`g_railPending`; drawMenuColumn
ends with `benchRailFlush()`, a test frame does not): a guess, not a
measurement. If a drawing test of the rail ever dies like this, start there.

GROUP 6, the tuning surface (drawToolbar, drawFrequencyReadout, applyKeyAction and
drawCenterPanels - four members in app_window.cpp - and what they share: the bookmark
markers' reader drawBookmarkMarkers, and the window's own tuningLists, readSpectrum,
dragVfoCentre, dragVfoEdge and commitTypedFrequency): API 0.3 section 8.5. The top bar's
transport, master lamps, volume dial and meters, the counter, the keyboard, and the spectrum
and waterfall are the highest-traffic surface in the window. What moved into the table:
- `fact.bookmarks.version`, the bookmark store's own version counter (the one the list
  fingerprint already hashed), so the markers' rows are copied when the STORE changes and
  not on every listSeq move;
- `fact.input.rateHz`, the DSP chain's input rate to the digit (`%.17g`), added to the
  fingerprint. NOT in the brief's list: the spectrum's axis (`scale_.setSpan`), the VFO's
  reach (`vfoOffsetInsideSpan`) and the click-to-tune are measured against it, and it is not
  `sampleRateHz`, the SOURCE's - they differ after a change of the source's rate that the
  chain refused (`Engine::followInputRate`: "the display span then reflects the OLD rate").
- Nothing else: no op is new (the survey found every one of the 27 commands to be an op
  already sent: 23 `submitCommand`, 4 `applyCommand`), FoxReceiverState did not grow, and the
  three queries (`currentAbsoluteHz`, `converterName`, `muteSubjectText`) are answered by
  state and facts the table already had (`tunedHz`; `fact.converter.live` and `.name`; the
  MUTED flag and `audioMutedBy`).
The pattern held, with these additions to it:
- ONE `read_spectrum` a frame (decision 1), in `refreshUiState` after the state, into
  `lastFrame_` (the vector is sized to the engine's bin count once; a different count is
  the only thing that resizes it): the three places that polled the pipeline for a frame
  (drawUi in scope mode and in the patch view, drawCenterPanels) read it, and the waterfall
  is fed where it is read - one row for each NEW sequence, none for NO_CHANGE. The ring's
  sequence is its own counter, one for each engine pass that had a frame, so the window sees
  the newest of the frames published since its last read, as `getLatestFrame` handed over.
  tests/test_tuning_surface.cpp B counts rows against published sequences: 40 frames read
  one a look give 40 rows for 40 sequences, three frames between two reads give one row (the
  newest), NO_CHANGE feeds nothing, the buffer never moves, and a refresh allocates 0.
  What a frame costs, measured (this desktop, Release): the copy out of the stream, 1024 bins
  = 4096 bytes, 0.2 to 0.3 us; the whole of the window's read including the waterfall's row
  (work the old poll sites did too), about 2 us; a refresh that finds nothing new, about
  0.12 us; `read_scope` for the volume meter, about 0.1 us a call before any sample is copied.
- THE MARKERS (decision 2): `tuningLists()` copies BOOKMARKS' rows only when
  `fact.bookmarks.version` moved. Twenty-five decoder lines move listSeq twenty-five times
  and copy the rows 0 times; an add, a star (the list keeps its size) and a remove copy them
  once each; a 33 000-entry import copies once, 33 199 rows in 2.8 ms. The draw's search over
  the copy is `core::FreqManager::range`'s rule (41 103 views held equal, edges, NaN and
  duplicates included). The names are cut at the list item's 95 bytes.
- SAME-FRAME SITES (decision 3), three looked at: (1) THE VFO AND BANDWIDTH DRAGS are real
  ones. The passband the panel measures PEAK IN PASSBAND over and paints is the one the
  drag just set, so the window keeps what the step sent for the rest of the frame
  (`tuneLocal_`), worked out by the engine's own functions (`vfoOffsetInsideSpan` over the
  chain's input rate and the bandwidth; [kVfoBwMinHz, kVfoBwMaxChanFrac x channel rate]),
  and `refreshUiState` drops it - one pass. Test D holds the copy equal to the engine's
  result over 552 steps (four chain rates, every bandwidth step, the limits at both edges);
  the centre drag sends the CLAMPED offset (the table refuses 1e8 or more, which a pointer
  dragged far past the panel on a wide source reaches, where the engine clamps), the edge
  drag sends the width as asked. (2) THE TYPED ENTRY'S RED LINE IS NOT ONE, though the old
  code's comment said so: the line is drawn from the status copy drawUi takes at the top of
  the frame, before the readout, so the old direct `applyCommand` showed on the NEXT frame;
  a session command made during a frame lands at the top of the next, before that copy -
  the same frame (test E shows both). What the port must keep is the ORDER: the old code
  applied the clear at once and queued the tune, so the clear came first; the port submits
  the clear before the tune, so a tune error the radio posts survives it. (3) THE KEYS read
  the value they change. `dispatchKeyBindings` stood at the top of the frame, before the
  publish, so a key repeating on consecutive frames (a held volume key at 20 frames a
  second or less) started from a state a pass older than the last command and lost a step.
  It now stands right after `refreshUiState` in drawUi (nothing between draws or touches
  WantTextInput or the popup stack): eight volume repeats, one a frame, land exactly 0.7.
- THE ENTRY (decision 3): `commitTypedFrequency` checks the figure as the table will
  (`typedFrequencyTunable`: more than 0 Hz and at most 6 GHz, held against what the table
  answers at 17 figures, NaN and infinities included) and says so on the red line with the
  existing translated sentence ("could not read frequency"): a new sentence would be a new
  `tr()` key in 33 catalogues. A refusal only the table judges is read from `poll_results`
  by `pumpTransmitKey` into `freqEntryRefusal_`, which the readout puts on the red line in
  the engine's own words (the transmit page's key note does the same), so it shows one
  pass after the answer: the accepted lag. Where the old path asked the driver and the
  generator took any figure, a typed 0 Hz or more than 6 GHz is now refused.
- `gui/tuning_reads.hpp` (new, no engine, no ImGui): the mute lamp and the volume meter's
  hard mute (`masterMuteLamp`, `audioHardMuted`: the pipeline's own `audioMuted` is the
  lamp's two causes and a keyed transmitter that is not monitored - the brief's two-term
  expression would have left the third out), the markers' row and search, the decoders the
  waterfall's caption counts (`FedDecoders`: per plugin, where the old loop counted status
  lines; nothing in the tree gives a plugin two), and the typed range.
- `AppWindow::uiSubmitTap_` (a test seam, null in the product, read at one place): every
  command `uiSubmit` takes, in order, so the 27 commands are held against the old expressions
  without a pass applying them (a key that starts the receiver or a take).

Ratchet: 132 fields / 631 reads -> 126 / 587 (the 36 reads of the four members, the two
reads of `drawBookmarkMarkers` and the six of the polls in drawUi; the fields that left:
dbMax_, dbMin_, squelchDb_, userMuted_, vfoBandwidthHz_, volume_), the budgets exactly the
printed counts. The four members, `drawBookmarkMarkers`, `tuningLists`, `readSpectrum`,
`dragVfoCentre`, `dragVfoEdge` and `commitTypedFrequency` join `kPortedMembers`.
`currentAbsoluteHz` leaves `kControlMayCall`: its last caller was the readout. The brief
said the readout's `applyCommand` line was among `kLineAllowed`; it was not (those two calls
passed because `applyCommand` is in `kControlMayCall`, where it stays: drawUi, the mute popup
and the sections not ported yet call it), so nothing left that list. The guard goes red on a
single `engine_.` read put back into drawToolbar (mutant G1: "AppWindow::drawToolbar is
PORTED ... reaches the engine directly").

What the operator sees: nothing new. Drawn by the build before this one and by this one -
the application's own framebuffer capture in bounded runs, twice for each of five states
(the default tuning; 2.5 MHz, a band-rich stretch of the world plan; a bookmarks file with
a starred bookmark in view; a receiver left stopped; the patch view open for the first 200
frames) - the top bar, the counter, the volume dial and meters, the spectrum, its marker
strip, the VFO band, the waterfall and its caption differ in 0 pixels at a tolerance of 24
between the two builds in every state, except the waterfall: up to 77 of 336 200 pixels
between builds, up to 55 between two runs of the same build (the rows land on different
frames from run to run). At tolerance 0 no area differs by more than 346 pixels (the
spectrum's trace and grid), against 326 between two runs of the same build
(build\g6-vis-summary.txt is the table, build\g6-vis-<state>.txt each pairing). Gestures
through the application's own input queue (FOXSDR_INPUT_SCRIPT: a VFO centre drag, an edge
drag, a click on a tube with typed text, a click on the waterfall, a wheel on a digit), six
runs of each build: the readout and the rail's VFO and bandwidth fields reach the same three
outcomes at each of four shots in both builds, pixel for pixel (the same hashes). Which
outcome a run reaches varies from run to run in the control as well (a bounded run's window
has no OS focus, and a scripted gesture lands or does not), and the typed entry and the wheel
reached none in either build - those two are held by tests/test_tuning_surface.cpp E and F,
not by this capture. A command from a key lands one pass after the press, as a widget's
does (the second drain that applied it at once is no longer how a key is applied); the
entry's refusal shows one pass after the answer.

What could move a measured number: the window copies the spectrum from the ring (0.2 to
0.3 us a frame) where it copied it from the pipeline's frame slot under a mutex - the pass's
own copy into the ring was already there since stage 4; the toolbar's meter reads
`read_scope`, which also takes a snapshot of the published state to fill its info (about 0.1
us a call); a refresh on a listSeq move now reads two more facts (`fact.input.rateHz`,
`fact.bookmarks.version`) and the DECODERS list, and the longer key costs ONE allocation
inside the table's `get_setting` (a temporary string for a key longer than the string's own
buffer; test H: 100 refreshes, 100 allocations, none of them the window's), on every pass
while decoder lines flow; the list fingerprint hashes one more double.

THE TEST-ACCESSOR TRAP held, and did not bite: no test drove the four members, so none was
re-plumbed; test_tuning_surface runs `look` (`publishReceiverState(); refreshUiState()`) before
a member and a frame in drawUi's order (`pumpFrameBegin`, `pumpPublish`, the status copy,
`refreshUiState`, a key, `pumpTransmitKey`) for the rest. Two things a test of this group must
mind: a key that starts the receiver or a take stays QUEUED on the session until a pass, so
the test drops the queue (closes and reopens the session) before it runs a pass; and
`drawCenterPanels` cannot be drawn without a GL context (the waterfall uploads its texture), so
its logic is split into members that can (`dragVfoCentre`, `dragVfoEdge`, `readSpectrum`,
`tuningLists`) and the drawn whole is held by the bounded-run comparison above.

Still reading the engine directly in the toolbar's neighbourhood, for group 8: `muteBannerSubject`
(`muteKeptRunning_`, `muteSubjectText`), called from drawToolbar.

GROUP 7, the recorder, the bookmarks and the scanner (drawRecorderSection, drawBookmarksSection,
rebuildBookmarkView, drawScannerSection and commitScannerDraft - five members in app_window.cpp -
with the commit flushScannerDraft makes and `readTake`, the one reader of a recorder's take, which
the status column's RECORDER card now shares): API 0.3 section 8.6, FoxReceiverState 1336 -> 1400
bytes. These were the last sections that read the recorders, the bookmark store and the scanner
directly. The brief's survey counted 71 uses for six members (50 field reads, 21 calls, 16
commands); the window's own, written before the first edit (build\g7-survey.md), found 66 for the
five (48 reads, 18 calls: the same 16 commands and two queries), drawBookmarkMarkers having gone
with group 6. What moved into the table:
- the scanner as its section draws it (S1): `scanSeq`, `scanCurrentHz`, `scanStartHz`,
  `scanStopHz`, `scanStepHz`, `scannerState` (FOXAPI_SCANNER_IDLE, SCANNING, PAUSED, HOLDING) and
  the four timings. The seven settings are a group of their own - they move `scanSeq` and `seq` -
  and the state word and the frequency are live state that moves `seq` alone, as the scanner's
  flag does (a scan steps at every dwell, and the receiver's tuning moves `tuneSeq` with it). The
  range is published as STORED, in Hz, the engine's own product of its megahertz and kilohertz
  fields, so a value that is not exact in megahertz reads back as that product; the timings are
  whole milliseconds, rounded to the nearest (a half goes up), 0 for a negative (the scanner
  floors it too) and saturating at 4294967295. Plain numbers: the publish still allocates nothing
  (tests/test_engine_publish_alloc case E: a scan running, stepping, paused, holding and stopped);
- the Bookmarks section's note and red line as STATUS_LINES keys `bookmarkNote` (level 1) and
  `bookmarkError` (level 3), with `fact.status.*`, the fingerprint, the engine's handed-over copy
  and the guard's kPublishedOnly (S2);
- one op (S3), FOXAPP_OP_BOOKMARK_EXPORT: the ids of the rows the section shows go by the long-text
  channel and the ENGINE writes those bookmarks, whole and in the store's order, as SDR#'s
  frequencies.xml under the record folder, and sets the note. It is not the window writing the
  file from its copy because the table's BOOKMARKS item cuts a name at 95 bytes and a group at 159
  and carries the mode as a code (a mode no code names reads 0): a file made from the copy would
  shorten and rewrite what the user shares. One pass over the store, not a search per id (the
  mutant that searched per id ran 40 000 ids in 831 ms against 37 ms for the pass, which the first
  cost test let live; it now exports 100 000 under a ceiling of 2.5 s, the mutant taking 5.7 s and
  the pass 139 ms). Every other command the three sections send was an op already: the survey found
  no Engine method they call that is not one;
- S4, BOOKMARK_GROUPS (21): NO engine change. The window derives its group names from its one copy
  of BOOKMARKS - sorted, distinct, as the old code derived them from the store - and does not read
  the list, so the names can never disagree with the rows they filter, and the section needs no
  second read. The list is as it was (built per call from the published block, O(bookmarks) per
  entry read): a client that wants the groups of a long list should not read it, which section 8.6
  says. The test holds the window's names equal to the list's, on 10 000 entries;
- nothing for the recorder: RECORDINGS' `value[1]` (bytes) and `value[3]` (seconds) are live since
  group 2 and the paths are LOCAL facts.
The pattern held, with these additions to it:
- THE BOOKMARK COPY, decision 2's rule widened. The markers' rows (group 6) became the window's ONE
  copy of BOOKMARKS: `BookmarkRow` (gui/tuning_reads.hpp; `MarkerRow` stays as its name for the
  markers) carries the id the commands name, the group, the mode code and the bandwidth beside the
  frequency, the star and the name, and `tuningLists()` copies the rows only when
  `fact.bookmarks.version` moves - never on a `listSeq` move, never per frame. The Bookmarks
  section, its chip's count, its view and its group names read that copy, so the list is walked
  once per change of the store and held once. Measured, this desktop, Release (test_recorder_
  bookmarks_scanner B and C): 33 299 rows (a 33 000-entry import and the rest) are copied once in
  4.1 ms and hold 3 463 864 bytes (104 a row: sizeof(BookmarkRow), the test list's names and
  groups all fitting the strings' own 15-byte buffers; a longer name or group adds its own heap, up
  to 96 and 160 bytes); group 6's three-field row copied 33 199 rows in 2.7 ms, and 3.2 ms with the wider row
  on that same list (no groups in it); the group names and the view are built in 1.1 ms; an
  unchanged view costs four comparisons a frame (1000 rebuilds over 42 290 entries: 0.01 ms in
  all, 0 allocations - the old code built a key string a call). The groups are built by sorting
  pointers and copying the distinct names, so a list of 33 000 makes a dozen strings, not 33 000.
  A chosen group stays chosen by NAME when the list changes and is dropped when its group goes.
- THE SAME-FRAME SITES (decision 3), three looked at; none needs a copy beyond what the window
  already kept. (1) THE SCANNER'S DRAFT (`scanDraft_`, its pending bits and ids) is the window's
  form and stays: an idle scanner commits each edit as it is typed, a running one on deactivate,
  and a field is re-seeded from the engine only while it is neither active nor holding a pending
  edit. What is new is the lag: a command lands one pass later, so a field is re-seeded the frame
  AFTER it stops being active, by which time the pass has run (the queue is drained, published and
  read, in that order, at the top of the frame); before the first state has been read the form
  keeps its defaults, which are Scanner::Params. (2) THE FLUSH. flushScannerDraft commits a running
  scan's edit left pending by a section that stopped being drawn, and the CONFIG command it builds
  carries the engine's own values for the fields nobody edited - the state's. It stood at the top of
  drawUi, before the publish and the read, where the state is a pass old and a timing set by the
  frame before would be set back; it now stands right after refreshUiState, beside the keyboard
  (group 6's reasoning), and a test reads drawUi's source for it. (3) START carries the range the
  engine stores (the state's), as the old code carried scannerParams(); a Start in the frame of
  the last keystroke would carry the range one pass old, and a click and a key in one 16 ms frame
  is not a gesture a hand makes - the draft path (type, then Start with the field still active)
  has frames between, and test_scanner_draft_input C holds it. There is no bookmark rename in the
  section (the name field is the NEXT bookmark's name, the window's buffer).
- The view's state (`bookmarkViewCopies_`, the group chosen, favourites-only and the filter it was
  built from) replaces `bookmarkViewVersion_` and a key string; the engine's host hook
  `onBookmarksChanged`, called inside the engine's own pass, is empty: the view follows the copy's
  version the frame after, and calling the window's session from the pass is not where it reads.
- `gui/section_reads.hpp` (new, no engine, no ImGui): the take's samples (an I/Q frame is 8 bytes,
  an audio frame 2: Recorder::flushStage counts them so), the group names, the view's filter, the
  form's seven values from the state and the three commands the scanner sends (the timings, the
  range with its edit mask, Start), as pure functions held against the models they restate.
- THE RECORDER: its seconds are the recorder's own steady clock (RECORDINGS `value[3]`), where the
  window subtracted two frame-clock readings; the note under an audio take is `audioHardMuted` (held
  equal to Pipeline::audioMuted in group 6) and `audioMutedBy`, which the engine fills from
  muteSubjectText; the two takes are found by refreshStatusLists and read, live, by `readTake`.

Ratchet: 126 fields / 587 reads -> 111 / 539 (the 48 reads of the five members; the fields that
left: audioRecordStartS_, audioRecorder_, bookmarkError_, bookmarkImportNote_, freqMgr_,
iqRecordStartS_, iqRecorder_, scanDwellMs_, scanHoldMs_, scanListenMs_, scanResumeMs_,
scanStartMhz_, scanStepKhz_, scanStopMhz_ and scanner_ - 15; `pipeline_` and `recordDir_` are read
elsewhere and stay), the budgets exactly the printed counts (the brief expected about 113 / 545;
the predictor said 111 / 539 before the edit and the guard prints it). The five members,
flushScannerDraft and readTake join `kPortedMembers`; `bookmarkImportNote_` and `bookmarkError_`
join `kPublishedOnly`. Twelve `kReadOnly` entries lost their last caller and are retired: the two
recorders' recording / bytesWritten / samplesWritten, freqMgr_.list / .list().size / .version and
scanner_.active / .currentHz / .state. Fifty-two older entries of that list had no caller before
this group either; they are left, as they are not this group's to retire. The guard goes red on a
single `engine_.` read put back into drawScannerSection (mutant G1: "AppWindow::drawScannerSection
is PORTED ... reaches the engine directly", and 112 fields / 540 reads against the budget).

What the operator sees: nothing new. Drawn by the build before this group and by this one - the
application's own framebuffer capture in bounded runs, four runs of each build in each of five
states, 1500x1380, the signal generator, FOXSDR_SINGLE_VIEWPORT=1 so that the input script's
coordinates are the window's own (build\g7-vis-final.txt is the table): the Recorder idle; with an
audio and an I/Q take running (pressed through FOXSDR_INPUT_SCRIPT); the VIEW bank scrolled to the end
over a bookmarks file of 400 entries in view (a favourite, six groups, their markers on the
spectrum); after Start scan (Paused at 88.0000 MHz: the generator's signal opens the squelch); and
with the group combo opened. At tolerance 24, area by area: 0 pixels differ in the recorder's header,
folder line and both buttons, in the scanner's header, seven fields, buttons and state line, and in
the marker strip and its labels; at most 3 pixels in the bookmarks' twelve rows, which is also the
most between two runs of one build. What differs as it does between two runs of one build is what
runs with the clock: the take's counter line (941 pixels at tolerance 24 in both pairings, "IQ 0.9 s
| 1799168 samples | 14.4 MB" against "IQ 0.9 s | 1739776 samples | 13.9 MB") and the status column's
RECORDER card (159 pixels in both). A setting lands one engine pass after the click (decision 2),
as every ported member's does; the Record, Start and Stop buttons swap their words a frame later,
and a scanner edit typed on an idle scanner shows in the engine a pass after the keystroke.
Two things the capture taught. With the window's own viewports on (the default) the input
script's pointer lands where the OS put the window, which differs from run to run - the first batch
here pressed Start scan on the Dwell field - so a bounded run for a gesture sets
FOXSDR_SINGLE_VIEWPORT=1. And the batch's runs are sorted by what they landed (visall7.py in the
session's scratch) and compared only within an outcome; with single viewport all eight runs of
every scenario landed the same.

What could move a measured number: FoxReceiverState is 64 bytes larger, copied once per publish and
once per window read; the window's bookmark copy is wider (above: 2.7 -> 3.2 ms for group 6's own
33 199-row list, 4.1 ms for a list whose rows mostly have a group) and costs once per change of the
store; the engine's pass copies two more status strings (the 16th and 17th line of
EngineStatusText) and hashes them in the list fingerprint; `seq` moves at every step of a running
scan (one counter increment a step, so a reader of `seq` - the web page, a plugin through the
snapshot - sees movement the scanner's flag alone did not give it; the plugin ABI's own counters
are unchanged); a take is read with a `read_list` a frame (two at most, as the status column's
card already did); an export now runs on the engine's pass (100 000 bookmarks, 139 ms) where the
window wrote the file in its frame.

THE TEST-ACCESSOR TRAP bit once, in tests/test_scanner_draft_input.cpp, which drives
drawScannerSection and flushScannerDraft through ImGui's own input queue and read the engine's
fields back: its frame now looks first (the engine publishes, the window reads its state) and lets
the engine's pass take the session's commands after the draw, as drawUi does; its 44 assertions
are untouched. Three things a test of this group must mind: benchSection leaves a drawer child
open for the next rail primitive to end when its section is folding, so a test that CLICKS a
section's header folds it and draws a frame the rail's own flush never ends (a sweep of the window
for widgets skips the header band; the title bar is part of it); FOXSDR_OPEN_SECTIONS is read once
a process, so it is set before the first AppWindow; and a click that sends a command leaves it
QUEUED on the session, so the sweep drops the queue (closes and reopens the session) before the
next pass applies it.

GROUP 8, the decoders, the presets, the marks, the plugin-mute cluster and the tune grants
(drawDecodersSection, drawDecoderStatusRows, drawDecoderWindow, drawPluginPresets, drawPresetKeys,
drawDecoderPresetBars, drawPluginTuneControls, drawPluginMarkers, drawMutePopup, muteBannerSubject,
drawMuteBanner and muteStateForDisplayName - twelve members in app_window.cpp - with
drawPluginPresetBar, which calls the lookup, and five helpers the window gained: pluginLists,
decoderLines, clearDecoderLines, pumpUiEvents and askAboutMutedPlugins): API 0.3 section 8.7,
FoxReceiverState unchanged at 1400 bytes. The brief's survey counted 68 uses (40 field reads, 28
calls); the window's own, written before the first edit (build\g8-survey.md), found 66 (the same 40
reads, 26 calls: 17 commands and 9 queries), the brief having counted two calls the code does not
make. Every one of the 17 commands was an op the window already sent: the brief's D2 expected a
FOXAPP_OP_PLUGIN_GRANT and the stale-grant checkbox sends FOXAPI_OP_PLUGIN_GRANT already (the
settings grant is toggled only from the fitted-modules window, group 9), so no op is added. What
moved into the table (commit fdcfa91, step A):
- D1, the plugin-mute cluster: list MUTE_CAUSES (36), one entry for each plugin muting the audio now,
  in the order audioMutedBy names them (name, the module file name in `detail`, `value[0]` = 1 while
  the mute is HELD after a tune-away); facts `mute.subject` (the whole sentence, where the state's
  audioMutedBy is cut at 127 bytes) and `mute.keptRunning`; event FOXAPI_EVENT_MUTE_TUNE_AWAY (11,
  `value[0]` = how many plugins); item flag MUTES on DECODERS (the plugin's effective "mute audio
  while running" setting, Engine::pluginMutesFor, the allocation-free form of pluginMutes). THE
  EVENT IS RAISED BY THE NEXT PASS'S PUBLISH, not at the decision: Engine::updateAudioMute runs in
  pumpAudioMute AFTER pumpPublish, so what a pass decides the pass after is the first to publish;
  ApiHost::muteTuneAway arms it and the next endPass broadcasts it, after the lists that show the
  mute, so a client that is told finds MUTE_CAUSES already naming the plugins and
  fact.mute.keptRunning already 1 (the mutant that broadcasts at the decision fails the test that
  reads them the moment the event arrives). mutePopupQueued_ stays in the engine, read by nothing
  in the table;
- D2, the grants: item flags TUNE_ALLOWED and SETTINGS_ALLOWED on PLUGINS (from the durable lists
  the config saves, which pluginUi_ mirrors), PLUGINS `value[0]` = the capability word as declared
  (CASCADE_CAP_*), fact `plugin.lastDenied` = the module file name (the key a grant is held under)
  of the last refused request to tune. Two additions the brief did not list, found by the survey:
  item flag STOPPED on PLUGINS (the runner's status list is empty exactly when no loaded plugin is
  un-stopped, and only decoders had a stop state in the table), and list STALE_GRANTS (37): the tune
  grants no LOADED module holds, in the config's order, REFUSED when the host holds a record of that
  file that did not load. A grant held for a module the host has no record of at all cannot be a
  flag on a PLUGINS entry, and drawPluginTuneControls exists to show exactly those;
- D3, DECODER_LINES no longer moves listSeq: publishApiLists returned the fingerprint XOR the line
  count, so a busy decoder moved listSeq once a line and every cache the window keeps on it read its
  lists once a line. The ring still takes the lines; the count is `decoderLinesTotal`, a state
  measurement, and the window's line cache is keyed on that. Three sentences of the draft that said
  otherwise are corrected (section 4's bookmarks row, 8.4, 8.5), and test_engine_api_03 Y and Q1,
  which held that a line DOES move listSeq, hold that it does not. The helper aLineAppears of
  test_rail_reads, test_recorder_bookmarks_scanner and test_tuning_surface, which moved listSeq with
  `++decoderLinesTotal_` to stand in for "a change the cache must not re-read the rows for", now
  changes the preset note (a status line, which moves listSeq all the same); their assertions are
  untouched;
- from the survey, not in the brief: item flag DEVICE_CENTRE on PRESETS (the "already tuned to" test
  compares the DEVICE centre for such a preset, and the list had no way to say so) and item flags
  SPAN, DASHED and SPECTRUM_ONLY on MARKERS (the geometry needs the kind of a mark and its two
  drawing flags). And a bug found on the way: the decoder lines the track-info provider writes
  ("Aircraft info", the only place a user sees why it is quiet) had never been counted in
  decoderLinesTotal_, so the ring - which takes the lines the count says are new - never saw them,
  and a window that draws the Decoder output from the ring would have lost them. They are counted.
The pattern held, with these additions to it (commit 24cdc86, step B):
- THE PLUGIN CACHE, `pluginLists()` (gui/decoder_reads.hpp, PluginRow): PLUGINS merged with its
  DECODERS, PRESETS and USER_PRESETS entries into one row a plugin, the MARKERS rebuilt as the
  plugin ABI's CascadeMarker rows so pluginMarkerGeometry is unchanged, MUTE_CAUSES, STALE_GRANTS and
  the three facts, read again ONLY when listSeq moves (and, since D3, a decoder line does not move
  it). A plugin is matched to its entries by display name (the host runs one loaded plugin a name,
  resolveDuplicatePlugins) and its saved presets by userPresetKey(file); the position among a
  plugin's own saved presets IS the ordinal the saved-preset ops take, a preset's raw index the
  plugin-preset op's. muteStateForDisplayName answers with a row of this cache, its presets built as
  rebuildMuteStates built them (the plugin's own first, then the user's at 1000 + ordinal; a
  bandwidth kept only when 0 < bw < 1e12; the label falling back to the plugin's name). The engine's
  muteStates_ stays (the audio mute and its tests use it) and the window stops reading it. Held by
  test A (the same two plugins' presets equal to the engine's own snapshot) and A2 (the decision
  rules over every combination of a row's states: 8232 of them, against the model they restate).
  A settled frame allocates nothing (1000 reads of an unchanged state: 0); a refresh of the cache
  costs four allocations, none of them from get_setting;
- THE DECODER LINES, `decoderLines()`: the window's one copy of DECODER_LINES, read only when
  `decoderLinesTotal` has moved, and then only the entries above the highest id it holds, from the
  newest back to that id, bounded at kDecoderLogMax with the newest kept. Clear empties the copy at
  once and the ids at or below the highest it held are never taken again, so a line that arrives
  after is copied and one the ring still holds is not. The Decoder output window, the status rows
  and the rail's line count read this copy and nothing else reads list 31. 1000 lines in 100
  frames are 1000 copied in 100 ring reads, the window holds 500, the newest id is 1000;
- THE EVENT, drained once a frame: the session subscribes to MUTE_TUNE_AWAY, pumpTransmitKey drains
  the events (pumpUiEvents) and askAboutMutedPlugins captures the names and the module keys from
  MUTE_CAUSES when one arrives - the dialog is about THOSE plugins however the lists move on - and
  asks drawMutePopup to open. The dialog is withdrawn when fact.mute.keptRunning reads 0 (the
  engine clears the latch exactly when it withdraws its own popup: an arrival on a preset, or the
  named plugins stopped) or the operator answers; Keep it running closes it at once and sends the
  op. The banner is drawn while the latch reads 1 and names the fact's whole sentence;
- THE CENSUS RULE, once: gui/module_census gains the overload that counts a module from a row (same
  rule: loaded and providing, stopped, refused by capability word, unread when the word is empty),
  so the status rows' census follows the rule the module-list census always had; test A2 holds
  the overload against that rule written out, over every combination of a row's states.

THE SAME-FRAME SITES (decision 3), five looked at (the survey's list); none needs a copy beyond what
the window already kept. (1) The tune-away dialog opens in the frame AFTER the pass that decided it
(above); the window keeps the names and the keys it captured, as the engine's own popup subject did.
(2) "Mute audio while running": a click toggles the checkbox's local bool at once, the command lands
at the top of the next frame and is published and read before anything draws, so the next frame
draws the engine's value. (3) A preset key's LIT look and a STOP/START label follow the state one pass
behind a click, as every ported control's does. (4) Clear in the decoder window: the engine clears
its log one pass later and the window empties its copy now. (5) The grant checkbox exists only on a
stale row, which goes the frame after the toggle. Three commands that were applied at once
(CLEAR_STATUS in the decoder window, CLEAR_STATUS and MUTE_KEEP_RUNNING in the dialog) now land at
the engine's next pass; the dialog's CLEAR_STATUS goes away with the event.

Ratchet: 111 fields / 539 reads -> 105 / 499 (the 40 reads of the twelve members; the survey
predicted it before the first edit and the guard prints it), the budgets exactly the printed counts.
The six fields that left: decoderLinesTotal_, muteKeptRunning_, mutePopupQueued_, mutePopup_,
mutedByKeys_ and pluginMarkers_; seven the twelve read stay because unported members read them
(decoderLog_, muteStates_, pipeline_, pluginHost_, pluginRunner_, pluginTuneAllowed_, pluginUi_).
The twelve members, drawPluginPresetBar and the five helpers join `kPortedMembers`. Two kReadOnly
entries lost their last caller and are retired: pipeline_.vfoOffsetHz and
pluginUi_.lastDeniedPlugin; no kControlMayCall or kLineAllowed entry became unused. The guard goes
red on a single `engine_.` read put back into drawDecodersSection (mutant G1: "AppWindow::
drawDecodersSection is PORTED ... reaches the engine directly", and 105 fields / 500 reads against
the budget).

D3, MEASURED (tests/test_decoders_mute_grants.cpp C, 1000 decoder lines one a pass, the window's
three caches read after every pass): with the count folded back into the fingerprint (the mutant,
which is the engine as it was before step A) listSeq goes 1 -> 1001 and the rail, tuning and plugin
caches each re-read their lists 1000 times; now listSeq stays at 1 and each re-reads 0 times. Ten
lines a frame for 100 frames: 1 -> 101 and 100 re-reads each before, 0 now. The band plan's ROW COPY
the brief asked about was 0 in both runs: the rail's row cache (group 3) compares the plan's count,
id and name before it copies, so a refresh already re-read the plan and device lists and the three
facts and not the rows. The test asserts 0 and says so; what it holds D3 by is listSeq and the three
refresh counts, which do move with the bug.

What the operator sees: the tune-away dialog opens one frame later than it did (the old code opened
it in the frame of the decision); a decoded line in the Decoder output window is cut at the table's
159 bytes (it showed the whole line); a plugin that is not a decoder and whose every preset fails
validation no longer gets a row of its own with only a name on it (it had nothing to press).
Everything else is drawn as before. Drawn by the build before this group and by this one - the
application's own framebuffer capture in bounded runs, four runs of each build in each of four
states, 1500x1380, the signal generator at volume 0, FOXSDR_SINGLE_VIEWPORT=1, the two fixture
modules fitted (build\g8-vis-final.txt is the table, vis8.py and campaign8.py in the session's
scratch): the Decoders section with one decoder stopped and one fed (13 areas: the header and chip,
the output key and line count, each plugin's rows, the pointer line, the status column's two cards,
the marks on the spectrum and the waterfall, the master lamps, the banner's place); both fed with
STOP ALL offered (7 areas); the Decoder output page open with 40 lines and the text decoder's preset
bar (9 areas: the page, its header row, the preset bar, the log, the rail's output key and section,
the two cards); and the tune-away (Group Eight's channel preset pressed, then another plugin's:
the dialog; then Keep it running: the banner; 7 areas at each of two shots). At tolerance 24, 0
pixels differ in every area of every state, between the two builds AND between two runs of one
build (6 control-against-control pairs and 16 control-against-port pairs each). At tolerance 0 the
most is 8 pixels (the dialog's place at the first mute shot), 107 against 101 in the dialog's place
at the second (the waterfall behind it, where the dialog was) and 6 in the status column's output
card; the runs of one build differ by the same amounts. The gesture's outcome was the same in all
eight runs of the mute scenario (the dialog up at the first shot, down with the banner at the
second), so no run was set aside.

What could move a measured number: the engine's pass hashes more per plugin (the capability word,
the settings grant and the mute setting from the cached module keys, the tune grants as one list,
the last refusal and the mute cluster), all in place and held at 0 allocations by
tests/test_engine_publish_alloc cases F and F2 (40 fingerprints and 40 passes with unchanged
lists, with two muting plugins and a held mute, and with two real plugins loaded and a grant, a
mute override and a stop each moving the fingerprint); the table serves two more lists
(MUTE_CAUSES, STALE_GRANTS); the window's plugin cache costs one refresh per change of the lists
(four allocations), and no longer one per decoder line, which is the saving D3 buys; the decoder
ring is read once a frame in which the total moved, copying only the new lines; the "Aircraft info"
lines now count, so a reader of decoderLinesTotal sees them (and the status column's decoder-output
rate, which is built from that count, includes them).

THE TEST-ACCESSOR TRAP did not bite: no existing test drives these members through the engine's
fields and then reads them back. The tests that drive them (the new one) look first, as drawUi
does, and let the engine's pass take the session's commands after the draw. Three things a test of
this group must mind, besides group 7's: pumpTransmitKey drains the session's answers as well as its
events, so a replay of a command's answer reads through the window's own look and not the session;
and a sweep for widgets skips the title bar (a spot with y under 26), where the window's collapse
arrow swallows the click and folds it.

GROUP 9, the fitted-modules window (drawFittedModulesWindow, with `fittedLists`, the helper the window
gained - one member in app_window.cpp): API 0.3 section 8.8, FoxReceiverState unchanged at 1400
bytes. This is the window the rail's FITTED MODULES key opens: one plate for each plugin file the host
holds, with its state, its keys, and everything the module and the host have to say about it. The brief
placed the member's end at about line 12626; it ends at 12551 (the lines after it are the plugin
panels' table drawer and a legend helper, which read no engine field). The window's own survey,
written before the first edit, found 14 counted field reads of 9 distinct fields, one method call
(`pluginIsStopped`, which the guard does not count) and seven `submitCommand` calls, plus one read the
guard cannot see: a `std::filesystem::file_size` of every module file, every frame the window was open.
Every one of the seven commands was an op the window already sent (the plugin rescan, the decoder
start and stop, the store's remove, the plugin grant with `ival[0]` 1 and 2, and the plugin command),
so no op is added. Step A put the table's side in place, step B the window's.

What moved into the table (step A; docs/api-0.3-additions.md section 8.8, and nothing in
FoxReceiverState, no op and no struct changed):
- the per-file FACTS: `fact.plugin.dir` and `fact.plugin.<file>.path` (both LOCAL: a remote session
  is DENIED the two paths and nothing else), and `.version`, `.author`, `.licence`, `.error`, `.idle`,
  `.notice` and `.integrity`. They are facts and not columns because the window quotes the module and
  the host and does not paraphrase them, and a list item's `detail` cuts at 159 bytes where a module's
  warning is 512 and the install record's finding about 220. Every key is present for every record
  the host holds, so a known file with nothing to say answers `""` (length 0) and a file nobody holds
  answers NOT_FOUND; the engine has no key-parsing code (they are plain map entries,
  `plugin.<file>.<leaf>`), and "parse the leaf from the right", for a file name with dots in it, is
  advice in 8.8 to an interface that is handed a key;
- PLUGINS `value[1]` (the file's size in bytes, measured by the ENGINE when the block is built, 0 when
  the stat failed) and `value[2]` (the level of the module's newest WARN or ERROR: 0, 2 or 3), and
  three item flags: MATCHED 0x40000 (the runner's `isFeeding` answer, for any kind of plugin and
  whether or not the receiver runs: DECODERS' FED is MATCHED plus running), ASKED_SETTINGS 0x80000
  (the module has asked, at least once, for something only the settings grant allows) and
  DIGEST_MISMATCH 0x100000 (the install record says the file's bytes are no longer the ones verified
  at install). The vendored header gains FOXAPI_LIST_PLUGIN_COMMANDS 38 and the flags, and its
  SHA-256 in third_party/foxsdr_api/README.md and THIRD_PARTY.md is updated;
- PLUGIN_COMMANDS (38), the command keys the loaded modules have added: id = the command id, name =
  the label, detail = the module file, in PLUGINS order and each module's keys in the order added;
  not LOCAL-only;
- the install outcome as two STATUS_LINES keys, `installReport` (level 1) and `installError` (level
  3), with `fact.status.*`, the fingerprint and the engine's handed-over copy of the lines (the test
  hook's `test.statusLine` takes both keys; test_config_snapshot_app's hand-over now covers 19 lines,
  nothing weakened). A removal puts the new sentence and the removed row into the same publish.
THE FINGERPRINT, and what it costs. Everything new is in the list fingerprint, hashed in place from
the cached module keys: a module's notice by level and text (found by `pluginNotices_.find(key)`),
size, descriptor strings and refusal, the plugins folder, MATCHED for every kind of plugin, the
install records (file, id, digest verdict), a scan count (`Engine::pluginScans_`, one a rescan), and
three counters: `PluginRunner::statusGeneration()`, bumped wherever the runner's status rows are
written, and `PluginApiCore::commandsSeq()` and `settingsRequestersSeq()` (commands added, relabelled
or removed, not the same key said again; a module's first ask for a setting). Two atomics and no
accessor that builds a vector or copies every row per call. The block, the facts and the one `file_size` stat a record are built only when
the fingerprint moves. Measured (tests/test_engine_publish_alloc case F3, three real modules and a
refused file, a notice, a settings ask and command keys): 40 fingerprints, 40 unchanged `publishLists`
passes and 40 full publishes each allocate 0; a change rebuilds the block once at 307 to 316
allocations (147 after a rescan) and 40 settled passes after it allocate 0; 25 repeated identical
WARNs leave the fingerprint equal. `pluginNotices_` is now written only when the level or the text
differs from what is stored, which changes no result (the fingerprint hashes both) but spares the
churn.

A BUG FOUND ON THE WAY, the second of the kind (F7): a plugin's own `log()` lines were not counted in
`decoderLinesTotal`. Group 8 found the same of the track-info provider's lines and counted those; this
is a second writer of the same log, in `Engine::applyPluginApi`. The DECODER_LINES ring takes "the
lines the count says are new", and the window reads it only when the total moves, so a plugin's INFO,
WARN or ERROR line never reached the Decoder output window that draws from the ring, and the ring
numbered and sliced the lines around it wrongly. RED, test H0 in tests/test_engine_api_03.cpp, before
the fix: `test_engine_api_03: 1409 checks passed, 20 failed`, the first `FAIL ...test_engine_api_03.cpp:3131:
s.state().decoderLinesTotal == base + 1u  (0 vs 1)`, with DECODER_LINES showing 0 entries where 1 was
expected. Fix: `++decoderLinesTotal_` for each line actually pushed (a log with several lines counts
several, an empty part counts none). GREEN: 1429 checks, 0 failed.

THE TESTS and the break-it checks of step A. tests/fixture_group9_plugin.cpp is new (registered for
test_engine_api_03 and test_engine_publish_alloc; an audio-processor-only module with two command keys
and a version, an author and a licence of its own; FOXSDR_FIXTURE_G9_NO_CREATE makes its `create()`
fail so the runner has an idle sentence to say), and group H of test_engine_api_03 (H0 to H5) holds the
facts, the flags, the commands, the two lines and the timing; the two "every list is served" loops now
run to PLUGIN_COMMANDS and PluginWorld gained optional parameters for the third fixture and for a copy
under a dotted name. Thirteen mutants, each red: the notice text, the notice level, the runner's
generation, the commands generation, the askers generation, the install records and the scan count
each left out of the fingerprint; matched keys hashed only while the receiver runs; the path fact not
LOCAL; MATCHED gated by the receiver running; the size not stat-ed; a relabel and a first ask not
bumping their counters. One of them - the scan count left out - was NOT killed at first, because a
rescan always rewrites the runner's rows and something else moves the fingerprint with it; a test now
bumps `pluginScans_` alone and fails that mutant. Not isolated by a mutant: the descriptor strings and
the error, which are hashed beside the scan count and change only across a rescan, so they are
redundant with it by construction. The suite ran 251/251 on the first and on the last run of step A;
test_engine_api_transmit failed once in a parallel run on its timing assertions (`plain < 150`,
`longest < 100`, `keyed(a)`) and passed three of three solo, at the 100 s its baseline takes, and in
the rerun of the whole suite.

Where step A DIVERGED from the survey, and the section says so: (1) ASKED_SETTINGS never clears. The
survey wrote "sticky until the next rescan"; the code (`PluginApiClient::askedSettings`) only ever
stores true and the clients live as long as the core, so the flag survives a stop, a grant and its
revocation, and a rescan too. Documented in 8.8, held by a test. (2) The idle fact takes the FIRST
non-Running row of the module, as the window does; DECODERS' detail still takes the LAST, as it did.
(3) Matched keys are hashed whether or not the receiver runs (before, only while it did). There is no
second runner lock for it: the pass already takes `feedSnapshot` while stopped. (4) More inputs went
into the fingerprint than the survey listed: the runner's status generation and the scan count stand
where it had counted on allocation-free accessors over the runner's rows, and the folder, the
descriptor strings and the refusal, and the install records' file, id and verdict are hashed as well. (5) A side benefit: DECODERS' detail can no longer go stale when
an idle sentence changes while the matched set stays the same.

The pattern held, with these additions to it (step B):
- THE FITTED CACHE, `fittedLists()` (gui/plugins_view.hpp: `FittedRow`, `fillFittedModule`,
  `makeFittedModuleFromRow`, `FittedListCache`): PLUGINS, eight facts for each module (read with
  `readFactText` inside the refresh, as `pluginLists` does, and not through `uiFact`) and
  PLUGIN_COMMANDS, read again ONLY when listSeq moves, into the model the panel draws. The receiver's
  run state (`FOXAPI_RX_RUNNING` from `uiState_`) and the install sentence are set on every call, not
  on the refresh, because neither is a list's. The old `makeFittedModule` is KEPT and
  tests/test_plugins_view.cpp is untouched and passes: test A of the new test builds the model both
  ways, from the table and the way the window built it from the engine, in eight states over four
  modules (32 comparisons), and requires them equal field for field. The window's open flag, its
  rectangle and its deck stay the window's own;
- the green and red lines come from the `statusLines_` copy, drawn only in a frame in which the store
  window drew nothing (the rule stays at the top of drawPluginWindows: store first, then this one);
- all seven commands go through `uiSubmit`, so the Remove's two presses arm on the file name (an armed
  Remove survives a Rescan) and the grant, the Stop and Start keys and a plugin's own keys land at the
  engine's next pass, like every ported control's.

Ratchet: 105 fields / 499 reads -> 104 / 485 (the 14 counted reads of the member; the survey predicted
it before the first edit and the guard prints `the window reads 104 engine fields directly, 485 times
(budget 104 fields, 485 reads)`), the budgets exactly the printed counts. One field left: `pluginNotices_`.
Eight of the nine the member read stay because unported members read them (`pipeline_`, `pluginRunner_`,
`pluginHost_`, `pluginUi_`, `pluginDir_`, `pluginInventory_`, `installReport_`, `installError_`);
`installReport_` and `installError_` also stay in the count because the plugin store's model
(`buildPluginStoreModel`, group 10) still reads them, and the entry in `kPublishedOnly` says so.
drawFittedModulesWindow and fittedLists join `kPortedMembers`. The floor `submitsInDraw >= 40`, which
the survey flagged because a ported window sends its commands through `uiSubmit` and the guard counts
`submitCommand` calls, is now `>= 38`, the printed `38 submitCommand calls in draw members`. Four
`kReadOnly` entries lost their last caller (grep found none left in src/gui) and are retired:
`pluginRunner_.isFeeding`, `pluginUi_.api().commands`, `pluginUi_.api().settingsRequesters` and
`pluginUi_.settingsAllowed`; `pluginRunner_.status`, `pluginHost_.plugins` and `pluginUi_.tuneAllowed`
still have callers and stay. The guard goes red on a single `engine_.` read put back into the ported
member (mutant G1, `const std::string mutantDir = engine_.pluginDir_;` in a scratch copy of src:
"AppWindow::drawFittedModulesWindow is PORTED (draws from its engine API session only) but reaches the
engine directly at app_window.cpp:12416", and 104 fields / 486 reads against the budget of 485); the
real tree runs clean.

tests/test_fitted_modules_window.cpp (new; loads the stage-1, group 8 and group 9 fixtures; 915
checks, 0 failed, about 15 s, driving the real AppWindow): A, the model equal to the old window's in
eight states; B, the modules, the size, the descriptor, the command keys, MATCHED with the receiver
stopped (the plate says "starting the receiver is all this needs"), a refused file, and the install
sentence suppressed when the store drew it; C, the cache and the allocation measurements (below); D,
every widget swept and clicked for a loaded module's plate and a refused file's, with exactly Rescan,
the Stop keys, the module's grants and its command keys sent and Remove arming once; E, Stop, Start,
both grants, a command key, Rescan and Remove (two presses) driven through clicks, each showing nothing
in the click's frame and the engine's value the frame after the pass, and Remove removing the row and
showing "Removed zz_second_refused.dll" in the same frame. Two mutants on the cache: refreshing on every
call fails 5 checks in C, and dropping the install report fails the Remove check in E (the real file
restored and rebuilt after each). The full suite: `ctest -C Release -j 8`, 252/252 in 202 s (251 and
the new one), no flake that time.

Measured, this desktop, Release (C of the new test): 1000 reads of an unchanged state allocate 0, also
with an install sentence showing; the old window's model, built the old way every frame, took 64
allocations a frame and a stat of each file. A rebuild (four files, eight facts each, and the keys list)
takes 42 allocations the first time and 40 on average over 100 refreshes, once for each listSeq move
however often it is asked; how many of them are inside the table's `get_setting` was not separated.
1000 decoder lines, one a pass, leave listSeq and the cache's refresh count unchanged (group 8's D3 holds
here). A chatty plugin saying the same warning 1000 passes running moves listSeq by 1 and rebuilds once;
one that says a DIFFERENT warning every pass rebuilds 1000 times, at about 0.078 ms a pass including the
engine's pass and publish (step A's design allows it, and says so). With the window shut, 50 list moves
cause 0 rebuilds, and opening it causes 1.

What the operator sees. From step B: everything drawn as before, with the one-pass lag every ported
control has; a file's size is measured by the engine when the list rebuilds and no longer stat-ed every
frame, so a file that grows behind the engine's back shows the new size at the next listSeq move; a
module that repeats the same warning moves nothing and a different text does; the display name now
arrives through the list item, which cuts at 95 bytes where the old window drew it whole (whether any
real descriptor name is that long was not checked); `stopped` now means "loaded and stopped", where the
old value could be true for a refused file, and the window reads it only after `loaded`, so nothing
visible differs. From step A, by the code and not by the capture (the fixtures log nothing): a plugin's
own log lines reach the Decoder output window again, which draws from the ring since group 8. From the
capture, below: nothing else, in the four states and the frames looked at. Drawn by the build before
this group and by this one - the application's own framebuffer capture in bounded runs, four runs of
each build in each of four states, the window 1502x1405 as the capture measures it (1500x1380
requested), FOXSDR_SINGLE_VIEWPORT=1, the two fixture modules of stages 1 and 8 fitted under
`aa_stage_one.dll` and `bb_group_eight.dll` in a plugins folder beside a scratch copy of each exe (the
same folder for both builds, because the window prints the path), the receiver stopped, no radio
visible to the process (SOAPY_SDR_PLUGIN_PATH removed from its environment), each run on its own copy
of the config (CASCADE_CONFIG_TEST) and bounded by `--frames` (build\g9-vis-final.txt is the table,
runone.py, campaign9.py and compare9.py in the session's scratch, under g9-vis). The states: the window
open with both modules fitted and the first plate selected (the receiver-stopped notice, two rows, the
stage-one module's plate); the same with the stage-one module in `pluginsStopped` ("STOPPED BY YOU",
a START key where there was a STOP key, "fitted, stopped"); a refused file (an invalid
`zz_refused.dll` beside the two) with its row pressed through FOXSDR_INPUT_SCRIPT, so the plate reads
"nothing was read out of this file", why it is not running and the REMOVE MODULE key; and the install
sentence (the same, with Remove pressed twice: the file goes, the row goes, the green line "Removed
zz_refused.dll" shows under the folder and everything below it moves down 30 pixels). The window's open flag is
cleared at start-up by `startupState` (nothing in a config opens it, as the member's own comment says),
so the capture opens it with the member's own seam, FOXSDR_OPEN_FITTED_MODULES, which both builds have,
and seeds only the rectangle (fittedModulesX, Y, Width, Height = 20, 40, 1060, 1300). Areas: 16 for
the open and the stopped state, 20 for the refused one and 17 for the install sentence, then the whole
fitted window (1 378 000 pixels) and the main window outside it (732 310): the title bar, the
FITTED MODULES plate, the header with its two keys and the five counts, the receiver-stopped notice,
the folder line, the sentence, the five filter keys, each module's row, the empty well under them, the
detail column's cards (identity, what it reaches, what it is doing and, for the refused file, why it
is not running and the Remove key with its hint), the scroll strip, and the cabinet's three rails.
At tolerance 24, 0 pixels differ in every area of every state, between the two builds (16
control-against-port pairs a state) AND between two runs of one build (6 control-against-control and 6
port-against-port pairs). At tolerance 0 the result is the same: 0 pixels in every area, every pair, and
0 in the whole window beyond the named areas, so there is no noise to bound at these frames. The
gesture's outcome was the same in all eight runs of each of the two states that press anything (the
refused plate up; the file removed, with the sentence showing), so no run was set aside. Because that
is a table of zeros, three checks that it is not a comparator that cannot see: it reports 5 862 pixels
at tolerance 24 (20 176 at tolerance 0) in "what it is doing" for the open state against the stopped
state of one build, and counts a +30 and a +10 poke on single pixels as 1 pixel at tolerance 24 and 2
at tolerance 0; the framebuffer differs from frame to frame inside a build (frame 77 of the install
state, where the row goes, differs from frame 76 by 176 628 pixels at tolerance 24); and, in
two more runs of each build with a shot at each of 16 frames - the window's first frames (1, 2, 3, 4, 5,
6, 8 and 12) and the ones around the second press of Remove (75, 76, 77, 78, 79, 80, 82 and 86) - every
frame is pixel for pixel the same in the two builds at both tolerances (four control-against-port
pairings, and one pairing of each build against itself, for each frame; the table is appended to
build\g9-vis-final.txt). So the one-pass lag is
not visible at the granularity of a frame in these two sequences: the removed row and the green
sentence land together, at frame 77, in both builds. What the capture does NOT draw: a FED row or a
"takes no signal" one (the receiver is stopped, which is the state the member's text most needs to be
right about), a module's own command keys, its warning or its integrity line, and the two grants'
keys toggled (the fixtures declare no commands and log nothing); the model for those is held by
test_fitted_modules_window A and D and E, not by a picture.

What could move a measured number: the engine's pass hashes more per module (above: the notice, the
runner's generation, the scan count, the commands and settings counters, the install records, the
folder and the descriptors), all in place and held at 0 allocations by test_engine_publish_alloc F3;
the block is rebuilt, with a stat of each file, whenever that fingerprint moves, 307 to 316 allocations
and 147 after a rescan; matched keys are now hashed while the receiver is stopped, where they were hashed
only while it ran; the table serves one more list (PLUGIN_COMMANDS) and two more status strings (the
18th and 19th line of the handed-over copy); the window's cache costs about 40
allocations a refresh and none a frame, where the old model cost 64 a frame and a stat of each file;
a module that says a different warning every pass makes the window rebuild every pass (0.078 ms); the
plugins' log lines now count in `decoderLinesTotal`, so a reader of it sees them, as group 8 recorded
for the aircraft-info lines; and `pluginScans_` makes every rescan rebuild the block, as it already
rewrote the runner's rows.

THE TEST-ACCESSOR TRAP was minded, as in group 8: every frame of the new test looks first
(`pumpFrameBegin`, `pumpInputs`, `pumpPlugins`, `pumpPublish`, the status text copy, `refreshUiState`),
draws, and lets the next look apply the session's commands, and `pumpTransmitKey` is not run so the
answers stay readable on the session. Six more things a test of this group must mind. The page chrome's
RESET WINDOW SIZES key sits inside the face and moves the window 25 pixels when a sweep clicks it, so the
sweep restores the rectangle before each click and after the last. ImGui needs two frames after the
pointer moves before a click lands (the capture's script places the pointer at least four frames before
it presses, for the same reason). At 1060x720 the plate falls below the visible area, so the test opens the window at
1560x1900 on a 1600x2000 display, and the capture at 1060x1300 in a 1502x1405 window. A pressed key
keeps focus, which changes the picture, so picture checks compare only pictures drawn after the same
clicks, with the state moved by commands no click sends. The window draws with `AddText`, so ImGui's log
buffer cannot capture its text: the tests check the model and the pure sentence functions, and real
draws and clicks. And a capture that opens the window through the config finds it shut: see above.

GROUP 10, the plugin store (drawPluginStoreSection, drawPluginsSection, drawBlockedPluginRows and
drawPluginStoreWindow, with `storeLists`, the cache the window gained; `buildPluginStoreModel` and
`addAllProgressLine` are deleted): API 0.3 section 8.9, no op added. This is the rail's store key with its
chip and catalogue URL field, the retired ("Disabled") rows hung under it, the rail's PLUGINS key, and the
store window the key opens: the catalogue, each module's data plate, the updates, the show and sort wells,
ADD ALL and the install outcome. The window's own survey, written before the first edit, found 44 counted
field reads of 10 distinct fields, 12 method calls the guard does not count, 8 `submitCommand` calls and one
`applyCommand`, in SIX members: the four the brief named and the two helpers that belong to them alone
(`drawBlockedPluginRows`, 11 reads between the two helpers, and `addAllProgressLine`; leaving them behind
would have kept a quarter of the reads). Where the brief and the code disagreed: (1) `reportPluginStatus` is
not a frame member: it runs once, from `run()`'s teardown, only in a bounded `--frames` run, and
`reportPluginTestResult` is a host hook the ENGINE calls from inside `pollPluginAsync` and reads the live
`catalog_` by design; both are test machinery, stay on `engine_.` and out of `kPortedMembers`, and that is
the whole reason `catalog_` and `pluginBlocked_` stay in the count (porting them would have been 93 / 438 and
86 / 405, measured by renaming the reads in a scratch copy of src); (2) `drawPluginsSection` is not a store
member, it is the fitted window's rail row, and it needed no new API. All nine commands the window sends
(the catalogue URL commit, Update and Remove on a retired row, FETCH from the first open and from CHECK NOW, CANCEL, ADD ALL, FIT
and Update on a row) were ops already and go through `uiSubmit`. One same-frame site existed, the URL commit,
which was applied at once with `applyCommand` and is a submit now (difference 3 below). Step A put the
table's side in place, step B the window's.

What moved into the table (step A; docs/api-0.3-additions.md section 8.9, with a per-datum table of what the
window draws and from where):
- CATALOGUE gains `value[0]` (the entry's ABI version) and `value[1]` (the size of the build for THIS host,
  0 for "not stated" or "no build"), and the item flag BUILD_HERE 0x00200000 (the next free ITEM bit; the
  0x00200000 at foxsdr_api.h:266 is FOXAPI_RX_WEB_LISTENING, another namespace). "Installable here" is
  COMPATIBLE and BUILD_HERE;
- six facts an entry, `fact.store.<id>.{author, licence, platforms, floor, file, updateReason}`, beside the
  six that were there, so twelve in all. Every key exists for every entry and is "" when empty; an unknown id
  or leaf is NOT_FOUND. `platforms` is "windows/x64, plan9/mips" in the index's order, `file` the sanitised
  name the engine itself joins to PLUGINS on (ASCII case ignored; an INSTALLED entry with no PLUGINS row is a
  retired module, which is normal), `updateReason` the plan's own English sentence ("version 9.2.0 replaces
  the installed 9.0.0"). `fact.store.url` is LOCAL (a REMOTE session is DENIED it), and its default is the
  real raw.githubusercontent.com address, not "";
- the retired rows' facts, `fact.blocked.<file>.id` and `.message`, keyed by FILE and present exactly for
  STORE_BLOCKED's files. The sentence is composed by `PluginRepo::pluginBlockMessage` when the block is
  built, so a language switch rebuilds it (proved: listSeq moved, the sentence changed, the item's `detail`
  stayed English) and it is whole, where a list item's `detail` cuts at 159 bytes (the test uses an
  over-limit message and checks `detail` is a prefix of the whole sentence);
- STORE_ACTIVITY (39), at most two entries, ACTIVE on both. Id 1 is the transfer: `name` the module, "" for
  the catalogue fetch, `value[0]` the progress read LIVE from the repo's atomic (no allocation), `value[1]`
  1 fetching the catalogue, 2 downloading (an update counts as 2). Id 2 is ADD ALL: `name` the module moving
  now, `value[0]` its 1-based position, `value[1]` the total. With only the run going entry 2 is first but
  keeps its id; it stands alone only in the publish of the pass that applied ADD ALL (name "", position 1),
  and from then entry 1 is there in every publish until the run ends;
- two more STATUS_LINES keys, `catalogStatus` (level 1) and `addAllSummary` (level 1, 3 when the run had
  failures), each also as `fact.status.*`. The vendored header gains FOXAPI_LIST_STORE_ACTIVITY 39 and
  FOXAPI_ITEM_BUILD_HERE, and its SHA-256, now 9cccb600aa5b7e5f505a7ab0b95f9cf909b2d7c39f47938a7e49390592669c3d,
  is updated in third_party/foxsdr_api/README.md and THIRD_PARTY.md.
TIMING, which the section documents: a fetch's start (entry 1 of kind 1 appears, CATALOGUE empties,
`catalogStatus`, `catalogError` and the install lines clear) is published together in the pass that applied
it; the result is published one pass after the pass that polled it; the handed-over lines copy is published
at the end of the pass that applied the result. So a window that mixes the copy with table rows can show a
result line one pass before its rows (difference 1), and a fetch of a local index.json can start AND land
between two reads, so the window must not rely on ever seeing kind 1.

THE FINGERPRINT, and why `catalogSeq_` is in it. The block, the facts, the composed sentences and the update
plans (one `plannedPluginUpdates()` a block) are built only when the list fingerprint moves, and the
fingerprint hashes no entry text: a new counter, `Engine::catalogSeq_`, is bumped at the four places
`catalog_` is written (a fetch's start, its landing, its failure and the test hook), and the fingerprint
hashes that, plus `pluginCatalogueUrl_`, `installBusyName_` (the NAME, not only the flag: ADD ALL moves to
the next download inside one `pumpWorkers` call, so the busy flag never drops between two modules and the
label would otherwise show the last one's name for as long as the next download lasts), `addAllRun_`'s
`active`, `next`, `total` and `currentName`, `addAllFailed_`, each retired row's id and stored sentence, and
the two new lines. The progress is the one live-read value and is not hashed. WHY IT IS THE GUARANTEE EVEN
THOUGH IT IS REDUNDANT IN PRODUCTION: on every real path the busy flag and the landing's rescan move the
fingerprint as well, so a catalogue could not change without it moving; `catalogSeq_` is what makes that
exact and not a coincidence of other inputs, and the test world changes the catalogue through the counter
alone, which is what kills its mutants. Measured (tests/test_engine_publish_alloc case F4: 24 entries with
descriptions of 528 bytes to 3 KB, a retired row, a transfer and an ADD ALL run): 40 fingerprints, 40
unchanged `publishLists` passes and 40 full publishes each allocate 0; 1000 iterations that read both
STORE_ACTIVITY entries while the progress moves allocate 0 and leave the fingerprint unchanged; a change
rebuilds once at 1193 to 1203 allocations and the 40 settled passes after it allocate 0; fifteen
single-input changes each move the fingerprint ALONE, and the same state said again 25 times leaves it
equal.

A BUG FOUND ON THE WAY, in the survey's own formula (red, then green): the survey had the table serve
`position = min(next + 1, total)`, which is the OLD window's formula in `addAllProgressLine`, and it is off
by one. `pumpAddAll` steps `next` past a module as it STARTS it, so while module k downloads `next == k`;
the old line drew the first module as "2 of N" and the last two both as "N of N". RED, test I3 in
tests/test_engine_api_03.cpp with the survey's formula: `FAIL test_engine_api_03.cpp:4311: run.value[0]
(2 vs 1)`, then `(3 vs 2)`, trace `@1 Bad One@2 Bad Two@3 Bad Three@3`. The fix is in the TABLE: it serves
`clamp(next, 1, total)`. GREEN: trace `@1 Bad One@1 Bad Two@2 Bad Three@3`. The window letters the true
number, so one visible line differs from the old one (difference 2); the test restates the old formula to
say so.

THE TESTS and the break-it checks of step A. Group I of tests/test_engine_api_03.cpp (I1 to I5) does a REAL
fetch of a local index.json with a retired module, an updatable fitted one and five entries for the columns
(no build, another ABI, no licence, a legal notice, a plain one), drives the transfer's workers through a
promise, runs a REAL ADD ALL over three entries whose file names `install()` refuses before the network (so a
run is a handful of passes), and holds every NOT_FOUND, "" and DENIED edge; F4 is above; the two "every list
is served" loops now run to STORE_ACTIVITY; tests/test_config_snapshot_app.cpp's hand-over is 21 lines (it
was 19), nothing weakened. Twenty-eight mutants were each applied alone and each is red, restored after:
twelve un-hash an input (`catalogSeq_`, `pluginCatalogueUrl_`, `installBusyName_`, `addAllRun_.active`,
`.next`, `.total` and `.currentName`, `addAllFailed_`, a retired row's id, a retired row's sentence,
`catalogStatus_` and `addAllSummary_` out of the lines loop); ten do the wrong thing (progress hashed,
`fact.store.url` not LOCAL, BUILD_HERE never set, the old position formula, `addAllSummary` always level 1,
the blocked message left as stored English, `.file` not sanitised, `updateReason` always empty, the transfer
kinds swapped, the live repo pointer dropped); six break a counter or a copy (the `catalogSeq_` bump removed
at each of its four sites, killed by I5; `catalogStatus` not handed over; `addAllFailed` not handed over).
The one that needed care is `catalogSeq_`'s own: on real paths something else moves the fingerprint with it,
so only a test that changes the catalogue through the counter alone (F4 and I5) kills it. Full rebuild: zero
errors (only the old C4244 warnings at fox_engine_api.cpp 1787 and 1790). Full suite 252/252 in 206.47 s;
test_engine_api_03 2094 checks and 0 failed, five solo runs in a row; test_engine_publish_alloc 223 checks;
test_config_snapshot_app 14.

Where step A DIVERGED from the survey: (1) EngineStatusText gains THREE members, not two: `catalogStatus`,
`addAllSummary` AND `addAllFailed` (a bool). The window reads its lines from that copy, and the red-versus-ink
level of the ADD ALL summary needs a source there; the table carries the level in STORE_LINES as well.
(2) The position is `clamp(next, 1, total)` and not `min(next + 1, total)` (above). (3) No
`fact.store.generation` was added: the survey would have added it only if step B measured a refresh cost, and
step B did not need it (see what could move a number). (4) `hostAbiVersion` stays the window's own
CASCADE_PLUGIN_ABI_VERSION and is not served; section 8.9 says so. (5) The legal-acknowledgement sentence was
still a literal at engine.cpp:2485, which step B hoisted (below).

The pattern held, with these additions to it (step B):
- THE STORE CACHE, `storeLists(bool full)` (gui/plugin_store_view.hpp: `StoreRow`, `StoreBlockedRow`,
  `StoreListCache`, and the pure helpers `fillStoreModule`, `setStoreBlockedReason`, `storeBusyLabel`,
  `storeAddAllLine`, `foldStoreRow`). TWO TIERS, both read only when listSeq moves. The RAIL TIER (every
  call, window open or shut, because the rail's chip, the retired rows and the PLUGINS key draw whenever the
  DECODE bank is up) reads CATALOGUE's count, STORE_UPDATES' ids, STORE_ACTIVITY's two entries, PLUGINS'
  records (and how many the host did not load) and STORE_BLOCKED with the two facts of each retired row. The
  FULL TIER (only from the window's draw, only while it is drawn) reads each CATALOGUE entry, its twelve
  facts and the PLUGINS record whose `detail` is the entry's `.file`, with that module's
  `fact.plugin.<file>.error`, into the `PluginStoreModel` the view draws. Three things are not the lists' and
  are set on every call: the status lines the engine handed over (the catalogue's line and error, the install
  report and error, the ADD ALL summary and its level), each row's `blockedReason` (it depends on the deck's
  selection and tick, which move between refreshes, so every row other than the selected one is asked as
  unacknowledged), and the transfer's progress, read live from STORE_ACTIVITY's first entry and only while
  one is present. The old `buildPluginStoreModel` is NOT kept: test A of the new test restates it and holds
  the table's model equal to it field for field, in eleven states over 63 module comparisons;
- THE ORDER OF THE REFRESH AND THE FIRST-OPEN TEST: `drawPluginStoreWindow` reads the RAIL tier every frame
  before the open test, then the FULL tier after it, and only then asks `storeShouldCheckOnOpen`. The
  predicate settles on its first call, so asked of a cache that has never been read ("no catalogue") it would
  send a FETCH that the engine executes by CLEARING a catalogue another path (the web remote's, a bounded
  run's hook) had loaded. Test D1: a catalogue loaded by another path sends no FETCH on opening, a
  never-asked first open sends exactly one, and removing the early rail read turns D1 red;
- `blockedRemoveConfirmIdx_` is gone for `blockedRemoveConfirmFile_`: the retired row's Remove is armed for a
  FILE, as the fitted window's is, and is cleared when its file leaves the list (F2, with two retired
  modules: a rescan that moves the rows does not move the armed one);
- the hoisted sentence: src/engine/plugin_store_reasons.hpp (`kLegalNoticeFirstReason`, the FOX_TR_NOOP
  literal now used by both engine.cpp and the view; the translation key is unchanged), so the engine's gate and
  the window's per-row reason say the same sentence, and a ticked-or-not row gets the same one.

THE THREADING FIX, which the survey recorded and step B made. The engine called two host hooks from ITS thread,
`onCatalogueFetchStarting` (inside the STORE_FETCH apply) and `onAddAllFinished` (inside `pumpAddAll`), and
they cleared the deck's consent ticks (`forgetCatalogueConsent`, `addAllAck = false`) while the window reads
and writes `selected`, `legalAck` and `addAllAck` in its frame: one thread today, a data race on a flag that
matters (a tick that survives a catalogue change is consent nobody gave) once the pump moves to the control
thread. The window no longer defines either hook and nothing from the engine's thread touches the deck:
`storeLists` clears the ticks on the window's own thread from three signals. (1) STORE_ACTIVITY's first
entry with `value[1]` 1 (a catalogue fetch began) clears all of them; (2) entry 2 present and then gone (the
run ended), or a new `addAllSummary` line, clears the ADD ALL tick; (3) the full tier, finding that the
catalogue it reads is not the one it read last (a hash over the rows it read), clears all of them, which covers a
fetch that started and landed between two reads. Both hooks are caller-less in src/gui now (the engine still
calls them and the default EngineHost methods are no-ops), and `kHostHooks` lost both names. Mutants on the
consent paths: deleting the kind-1 clear, the content-change clear or the run-end clear each turns exactly its
own check red (F). The plugin windows' own hazard (the host process draws `pluginUi_` while an
install's rescan unloads and reloads every module) is group 9's F6 and is NOT touched here.

Ratchet: 104 fields / 485 reads -> 94 / 441, the 44 reads the survey counted and the guard prints as `the
window reads 94 engine fields directly, 441 times (budget 94 fields, 441 reads)`; the survey predicted the
numbers by renaming the six members' reads in a scratch copy of src before the first edit, and they match
exactly. `kPortedMembers` gains `drawPluginStoreSection`, `drawPluginsSection`, `drawBlockedPluginRows`,
`drawPluginStoreWindow` and `storeLists`. TEN FIELDS left the count because nothing in src/gui reads them any
more: `addAllFailed_`, `addAllRun_`, `addAllSummary_`, `catalogPending_`, `catalogStatus_`, `installBusyName_`,
`installError_`, `installPending_`, `installReport_` and `pluginRepo_`. `installReport_`, `installError_`,
`catalogStatus_`, `addAllSummary_` and `addAllFailed_` join `kPublishedOnly` (the engine hands the copy).
`catalog_` and `pluginBlocked_` stay in the count, because the two diagnostics above read them. The floor
`submitsInDraw >= 38` is `>= 30`, the printed `30 submitCommand calls in draw members` (the eight the store
sent are `uiSubmit` calls now; the floor is the scan seeing the real files, not a ratchet). `kReadOnly`
retired `pluginRepo_.progress` and `pluginUi_.tuneAllowed`, their last callers gone (grep-confirmed);
`kHostHooks` lost `onCatalogueFetchStarting` and `onAddAllFinished`. The guard goes red on a single
`engine_.` read put back: with `engine_.catalog_` read back into `drawPluginStoreSection` the printout is
`FAIL: AppWindow::drawPluginStoreSection is PORTED ... reaches the engine directly at app_window.cpp:8214`,
with 94 fields / 442 reads against the budget and 186 checks, 2 failed; the real tree runs clean, 186
checks, 0 failed.

tests/test_plugin_store_window.cpp (new, registered with the fixture loops; 3517 checks, 0 failed, about
15 s, driving the real AppWindow over a REAL fetch of a local index.json laid over a plugin folder of its
own): A, the model equal to the old window's (above); B, entry by entry, the ABI, the size, the builds, the
gate's reason, the plan, the host's record, the retired row's id and whole sentence, and the rail's counts;
C, the cache and its allocations (below); D, every widget swept and clicked in six states, with exact
expected command sets (a ticked notice sends STORE_INSTALL with `ival[0]` 1; with a transfer in flight
STORE_CANCEL is the only live key; D1 above); E, a click landing at the engine's NEXT pass and showing
nothing in the click's frame and the engine's value the frame after, for CHECK NOW, FIT, CANCEL, the rail's
Update, Remove and Confirm, and a typed URL; F, the consents (above), a REAL ADD ALL ending in a red summary
with `addAllFailed`, the status lines and their ink flags, a retired Remove staying armed for its file, and
the PLUGINS key's counts equal to the engine's. A mutant on the cache, refreshing on every call, fails 8
checks, including 105 000 allocations for 1000 full reads. The full suite, `ctest -C Release -j 8`: the first
run had two failures, test_main_view (the known flake, 3 of 3 solo reruns pass) and the new test, which
failed in the parallel mix because its D3 swept keys while the first-open fetch was still in flight (the test
now waits for it; 3 solo runs, 8 concurrent runs and the second full run pass); the second full run is
253/253.

Measured, this desktop, Release (C of the new test): 1000 settled FULL reads allocate 0, 1000 rail-tier reads
0, 1000 reads with a consent tick flipping 0, and 1000 live progress moves (listSeq unmoved) 0. One rebuild
(7 entries, twelve facts each, the host's records, a retired row) allocates 105, about 15 an entry; the old
window's model, built every frame it was open, allocated 227 and ran `plannedPluginUpdates()` and
`catalogEntryInstalled` over the host's records a row, and the closed window's rail still paid one
`plannedPluginUpdates()` a frame with a catalogue loaded.

THE ELEVEN THINGS THE OPERATOR CAN SEE DIFFER, by the code and the tests (the survey's, step B's, and one
resolved here); the capture below holds none of them in frame except where it says:
1. One pass of lag on every command, like every ported control's; a result line from the handed-over copy
   can show one pass before its table rows (documented in 8.9's timing note).
2. The ADD ALL position line is the true number: the new trace is `1 of 3: starting | 1 of 3: Bad One | 2 of 3:
   Bad Two | 3 of 3: Bad Three` where the old one was `starting | 2 of 3: Bad One | 3 of 3: Bad Two | 3 of 3:
   Bad Three` (a KNOWN visible difference, from step A's bug).
3. The URL commit is a submit: it lands one pass later, and `fact.store.url` a pass behind that. The one
   hazard, a URL commit and CHECK NOW in one frame (the store is drawn before the rail's field, so FETCH
   would be submitted first and fetch the old URL), is closed by ImGui's input trickle: a click's down and up
   are never the same frame, and the field deactivates on the down. Reasoned from third_party/imgui, not
   reproduced.
4. The catalogue entry's name arrives through a list item and is cut at 95 bytes, where the old window drew
   it whole (a KNOWN visible difference, tested and not captured). The longest live name the code cites is
   51 characters; the live catalogue was not read.
5. A refetch of the SAME catalogue that no frame saw does not clear the consent ticks (the old window cleared
   them at every fetch). Test F(c) pins it. A fetch the window sees clears them as before.
6. A retired row's Remove confirmation is keyed on the file, not on an index, and is cleared if its file
   leaves the list.
7. The model is rebuilt when listSeq moves, not every frame: a file added to the folder by hand shows at the
   next rescan, and `running` follows STOPPED through the pass.
8. A retired row's sentence is the engine's composed fact, in the language in force, rebuilt on a language
   switch; the old window composed it at draw.
9. The progress figure is read only while a transfer is present (the bar draws only while busy, unchanged).
10. THE RAIL CHIP'S IDLE RULE did not change, and the DOCUMENT said otherwise: section 8.9's chip row read "IDLE
    with neither a catalogue, a status line nor a fetch", where the window has always said IDLE when there
    are no catalogue rows (a status line or a fetch in flight does not change it; BUSY outranks the rest).
    The window's rule is the kept one and the DOCUMENT's row was corrected, in this group, to say it.
11. `haveTuneGrant` is derived from a loaded record's CASCADE_CAP_HOST_CLIENT bit instead of the `hostClient`
    pointer the old model read; held equal on the fixtures.

What the operator sees, in a picture. Drawn by the build before this group (the tree at 18482a7: groups 1 to
9, no group 10, in its own worktree) and by this one - the application's own framebuffer capture in bounded
runs, four runs of each build in each of FIVE states, the window 1502x1405 as the capture measures it
(1500x1380 requested), FOXSDR_SINGLE_VIEWPORT=1, a plugins folder beside one scratch copy of each exe (the
same folder for both builds), no radio visible to the process (SOAPY_SDR_PLUGIN_PATH removed from its
environment), each run on its own copy of the config (CASCADE_CONFIG_TEST) and bounded by `--frames`
(build\g10-vis-final.txt is the table; runone10.py, campaign10.py, compare10.py, mkindex.py and the five
input scripts s_<state>.txt are in the session's scratch, under g10-vis). The world: the two fixture modules
of stages 1 and 8 fitted under `aa_stage_one.dll` and `bb_group_eight.dll`, with an `installed.json` written
for the run (the stage-one module installed at 1.0.0 with a cached policy floor of 2.0.0, so the host
retires it and renames its file to `.disabled` at the first scan; the group-8 module installed at 8.0.0), and
a LOCAL catalogue, seven entries in a scratch index.json (the two fixtures at 2.0.0 and 8.1.0, so two
updates are on offer, and five entries for the columns: a plain one with a long description and
two builds, one with a legal notice and a floor, one with no build, one for another ABI, one with no
licence), the longest name 19 bytes so the 95-byte cut is out of frame. THE CATALOGUE URL IS IN THE SEEDED
CONFIG (`pluginCatalogueUrl`, the index's absolute path, an address with no "://"): the first open of the
store reads it from disk and nothing touches the network (a first open fetches, and with the default URL it
would have gone to raw.githubusercontent.com). The store window opens with the capture
seam FOXSDR_OPEN_PLUGIN_STORE and the rail goes to DECODE with FOXSDR_OPEN_DECODERS, both builds having
both. The states: STORE, the window open and the catalogue read, the face scrolled down by a wheel step, the
Beta Decoder row pressed (its data plate: ABI "3, matches this build", builds for windows/x64, "Cannot fit:
the legal notice must be acknowledged first", the updates panel with its two keys, the show wells, the
catalogue source line); IDLE, the rail with the store never opened (chip IDLE, "Plugins (1 disabled)", the
retired module's sentence, "Open the plugin store with the key above to fetch the update." and the Remove
key); CHIP, the store opened once and shut again by its own close key (chip "2 UPD" with its amber lamp, the
retired row's Update and Remove keys); ARMED, the same with Remove pressed once ("Delete aa_stage_one.dll?"
with Confirm delete and Cancel); and REMOVED, Remove pressed twice, the store reopened from the rail's key
(the catalogue is loaded, so nothing is fetched and the report stays), the face and then the data plate
scrolled to bring it into frame: the green "Removed aa_stage_one.dll", the updates list one entry shorter,
and in all eight runs the folder left holding `bb_group_eight.dll` and `installed.json`. Areas: 17 named
areas in each of the two store states (the main window above, beside and below the store, its title bar, the
plate heading, the updates panel, the search, show and sort panels, the modules heading, two rows and the
cut-off rows, the data plate's heading and body, the face's scroll strip and the window's foot), 10 in each
rail state (the top deck, the rail's head, the store key and chip, the URL field, the Disabled block, the
PLUGINS key, the rest of the rail, its foot, the patch page and the frame's ring; these tile the capture
exactly), then the whole capture (2 110 310 pixels) and, in the store states, the 81 214 pixels left in no
named area. At tolerance 24, 0 pixels differ in every area of every state, between the two builds (16
control-against-port pairs a state) AND between two runs of one build (6 control-against-control and 6
port-against-port pairs). At tolerance 0 the result is the same: 0 in every area, every pair, the whole
capture and the unnamed pixels, so there is no noise to bound. It is stronger than that: the eight
captures of each state (four of each build) have ONE MD5 between them, so they are the same file; the
stderr and the plugins left behind are identical in the two builds in every state, and the stdout differs only
in the teardown time and the worst frame gap. The gesture's outcome was the same in all eight runs of each
state, so no run was set aside. Because that is a table of zeros, three checks that it is not a comparator
that cannot see: control IDLE against control CHIP reports 476 pixels at tolerance 24 (818 at tolerance 0)
in the store key's row, 6 202 (7 604) in the Disabled block, 8 531 (10 592) in the PLUGINS key's row and 58
077 (81 679) in the rest of the rail, and 0 and 6 in the patch page that did not change; control STORE
against control REMOVED reports 84 734 (122 621) in the updates panel and 51 117 (94 163) in the data plate;
and a +30 and a +10 poke on single pixels are counted as 1 pixel at tolerance 24 and 2 at tolerance 0.
What the capture does NOT draw, and so does not hold: a BUSY chip, a transfer and its bar, an ADD ALL run and
its summary (the two known differences, which a deliberate capture would show), a catalogue error or status
line, a failed install's red line, a notice ticked, an Update or FIT pressed to completion (they download;
the tests inject a transfer so the gate refuses), a language switch, and a sequence of frames: group 9
shot sixteen frames round its gesture to look for the one-pass lag, and this campaign did not, so that lag is
held by test E and by the code and NOT by a picture. The real config
(%APPDATA%\foxsdr\config.json, 5 076 bytes, written at 14:52:27 UTC by another session, sha256 BA532E6C...A353C) was
identical before the first launch and after the last (size, modification time and hash), and none of the
scratch values (the index's path) is in it. The model for those is held by test_plugin_store_window
A, D, E and F and by group I of test_engine_api_03.

What could move a measured number: a REBUILD of the window's model costs about 15 allocations an entry (105
for seven; 24 entries about 400), once for each listSeq move however often it is asked, where the old
model cost 227 every frame; the engine's block rebuild (step A) costs 1193 to 1203 allocations for 24
entries with 528-byte to 3 KB descriptions, whenever the fingerprint moves; a decoder's FED toggling moves
listSeq, so while the store is open each toggle rebuilds the window's model once (step A added no
`fact.store.generation` on purpose, and measuring that rebuild is the test to run if it ever shows in a
trace); `plannedPluginUpdates()` is now once a block and no longer once a frame while the rail is up (twice a
frame with a retired module); the engine's pass hashes `catalogSeq_`, the URL, the busy name and the run's
fields in place at 0 allocations (F4); the table serves one more list (STORE_ACTIVITY) and two more
status strings (the 20th and 21st line of the handed-over copy) and its progress is one live read of an
atomic, not hashed; and a settled read of the cache, whatever the window draws from it, is 0 allocations.

THE TEST-ACCESSOR TRAP was minded, as in groups 8 and 9, and more of it bit this time. A test frame is LOOK
(`pumpFrameBegin`, `pumpInputs`, `pumpPlugins`, `pumpPublish`, the `statusLines_ = engine_.statusText()`
copy, `refreshUiState`), DRAW, then `pumpWorkers` and `pumpAudio`: without the last two a fetch never lands,
and a test that calls a ported member must copy `statusLines_` first because nothing else sets it. Seven more
things for the next group. `SETTING_SET test.statusLine` is refused on an engine without the test hooks, so a
test sets the line fields directly. A FIT or Update sent for real would DOWNLOAD: inject a transfer so the gate
refuses, or use entries whose file names `install()` refuses before the network. The store window must be
1580x1980 in a test to expose the plate's keys (in the capture it opens inside the main window at 1452x980
and the face scrolls: a wheel step with the pointer over an empty spot of the plate, then a second one over
the data plate's own pane for what sits under its key). The first open of the store FETCHES, so a test or a
capture that opens it with a catalogue never asked must point the URL at a local file first (in a capture, in
the SEEDED config), and a test that sweeps keys must wait for that fetch (D3's first failure). The survey
must drop `uiSubmitTap_` on exit: a dangling tap pointer is undefined behaviour. A retired module needs an
`installed.json` with a cached floor in the plugins folder, and the host renames its file to `.disabled` at
the first scan, so the file a capture's Remove names is the module's own (`aa_stage_one.dll`) and not the
renamed one. An input script's path is read from the run's own folder, so it must be absolute. And, from
step A, Git Bash `sed -i` strips the CR from the CRLF files in this tree: every edited file was restored to
CRLF (the vendored header is LF, as in the repo).

GROUP 11, the Source section and the sound card panel (drawSourceSection, radioNotOpenLit,
drawSoundCardControls and soundCardReceivesText, with `sourceLists`, the cache the window gained; the window's own
`sourceIdForRow` is deleted): API 0.3 section 8.10, one member appended to the state, five lists, eleven facts, one
status line, one column, one op and one field of an op. This is the rail's SOURCE row with its chip and lamp, the
combo of radios (the generator, the IQ file, the sound card, the native rows, the SoapySDR rows) with its Refresh
key and "Look for network USRPs", the scan's sentences (scanning, deferred beside an open radio, drivers not scanned
again, the dongles nothing can open, the SDRplay API, "No radio hardware found" and its tree of evidence), the IQ
file, Pluto and sound card panels, the fault card and the saved-source preview, and the open radio's rate, antenna,
gain and option controls. The window's own survey, written before the first edit, found 128 counted field reads in
FIVE members (79 in `drawSourceSection`, 4 in `radioNotOpenLit`, 4 in `sourceIdForRow`, 37 in `drawSoundCardControls`
and 4 in `soundCardReceivesText`), 46 distinct fields of which 34 have no other reader, 15 `submitCommand` lines and
11 same-frame `applyCommand` sites (the most of any group; groups 9 and 10 had none and one), and where the brief and
the code disagreed: (1) the subject is bigger than the three names, because `drawSourceSection` calls
`drawSoundCardControls` (its only caller) which calls `soundCardReceivesText`, and together they read 41 times, more
than half of the section's own 79; (2) the brief's "about 94" was a crude count that also took method calls, the
guard's rule (a field the Engine declares, `engine_.` at a non-identifier boundary) gives the numbers above;
(3) `sourceIdForRow` is not exclusive to the section: `AppWindow::run`'s `FOXSDR_SOURCE_DEVICE` bounded-run seam
calls it, next to three direct `engine_.nativeDevices_` reads and a line-allowed `scanNative()`; (4) the serial ports
are not in this section (`drawSerialPortsSection` is in SYSTEM) and the device options are; (5) there is no deck or
view type to fill (the only window-side state is `iqPath_`, `plutoUriDraft_`, `sourceComboWasOpen_` and
`soundCardCentreMhz_`, which stay); (6) "much of this already has table coverage" is true of the radio half and
false of the half the section is mostly about; (7) the scanning IS the engine's work already (`scanNative` runs
inline on whichever thread applies the command, the SoapySDR scan is a `std::async` worker whose body is the child
process of the 0.63.0 enumeration, collected by `pollSourceAsync` in `pumpWorkers`), the window only triggers it and
reads the rows; (8) the section reached the engine's machine through calls that are not `engine_.` reads and so sit
in no ratchet (`source::sessionFaultedDrivers()` every frame, `SoapySource::loadedModules()`, `vendorInstalls()` and
`moduleSearchPaths()` every frame while the "No radio hardware found" block showed, the two USB id tables, and
`submitSoundCardForm(Engine&, ...)`, a free function in a window file that took the Engine by reference); (9) two
commands were sent EVERY FRAME, not on an edge (group 3's trap): `applyCommand(CLEAR_STATUS,
SOAPY_SCAN_DEFERRED_LOGGED)` whenever the scan gate was open and `applyCommand(SOUND_CARDS_WANTED)` until the cards
were listed; a port by `uiSubmit` would have taken a ticket a frame. Step A put the table's side in place, step B the
window's.

What moved into the table (step A; docs/api-0.3-additions.md section 8.10, with a per-datum table of what the window
draws and from where):
- THE STATE gains `sourceFlags`, a `uint32_t` appended after `scanListenMs` and in the device group (it moves
  `deviceSeq` and `seq`); `FOXAPI_MIN_RECEIVER_STATE` ends at it. Ten bits, in order: `FOXAPI_SRC_SCAN_PENDING`,
  `SCANNED`, `SCAN_PARTIAL`, `SCAN_GATED`, `NETWORK_USRPS`, `UHD_LEFT_OUT`, `SDRPLAY_ROWS`, `CARD_LISTED`,
  `CARD_SCANNING` and `CARD_OPENING`. (The state's existing flag word is nearly full: a new word was cheaper than
  moving it. Cost: four bytes a publish; the bits are reads of fields the pass already holds plus
  `soapyScanGated()`, which reads `anyDeviceOpen()`, an atomic, and the patch radios.) Wiring: the snapshot's
  device group left it out at first, red in test_receiver_snapshot and then green (the layout asserts now end at it,
  and a new `sourceFlagsGroup` case holds it in the group);
- `DEVICES` is COMPLETED, which the survey had said it would not be: the entries are now the generator, the IQ file
  (`row:iqfile`), the sound card (`row:soundcard`), the native rows and the SoapySDR rows, so an entry's index is its
  row in the combo and its `id` is that plus one, and every radio's entry moved down two places. The alternative, a
  strict append at the end, was rejected to keep index equal to combo row (`fact.source.row` speaks the combo's
  numbering on purpose: a port that indexed the combo into `DEVICES` by position would have been wrong by two). The
  0.2 conformance counts moved (1 to 3 and 2 to 4, the radio at index 3; api_03's 2 to 4 and 4 to 6) and 8.10
  says so. The window composes "(native)" itself from the id's kind, in its own words (same msgid), and not for the
  Pluto's row, which is an address;
- THE WHOLE-ID FACTS, closing a cut the API had had since 0.2: `FOXAPI_OP_SELECT_SOURCE` says "an id from DEVICES,
  verbatim", and `detail` cuts at 159 bytes, where a SoapySDR row's id is `soapy:` and the whole kwargs string a
  vendor module reported and the Pluto's is `pluto:uri=` and an address of up to 191 bytes. A cut id matches no
  scanned device, so the radio could not be opened (`NOT_FOUND`, "no scanned device matches that id"), for the
  window and for every other client, the web page included. `fact.source.id.<n>` is the whole id of the entry whose
  `id` is `n` (it exists for every entry; `NOT_FOUND` past the last), from one spelling in the engine
  (`Engine::sourceIdForRow(int) const`, new, which the bounded-run seam now calls; the window's copy is deleted).
  Proved with a 199-byte Pluto id and a 280-byte Soapy id: the cut detail is refused by SELECT_SOURCE and the fact's
  id opens the radio. The Pluto row's selection is drawn from `fact.source.row`, which follows the family alone,
  and not from the entry's `ACTIVE` (its id is built at scan time from the address the box held then);
- THE FACTS: `fact.source.row`, `.keep`, `.opening`, `.scanGate`, `.scanSkip`, `.faultedDrivers` and `.plutoUri`
  (LOCAL), `fact.source.id.<n>`, and `fact.soundcard.device`, `.hostApi` (LOCAL) and `.span` (`"<lo> <hi>"`, `%.17g`,
  what the form's card would receive through the converter kept for it). Eleven in all;
- THE FAULT, WHOLE: the `deviceFault` STATUS_LINES key (level 3, the whole sentence; also `fact.status.deviceFault`
  and `EngineStatusText::deviceFault`). It closes a cut and a hazard the survey found by arithmetic and step A proved:
  the state's `faultMessage` holds 127 bytes of the 134-byte zombie-source sentence ("source thread: the radio's
  driver is holding an abandoned thread and this source cannot restart; pick another source or restart FoxSDR"),
  and the window's `reconnectAdviceApplies` looks for "restart FoxSDR" at its END, so on the cut copy the generic
  "Reconnect it and pick the source again" advice would have been wrongly ADDED under a sentence that says the
  opposite. RED/GREEN as a fact about the cut copy: G11-4 asserts it is true on the cut copy and false on the whole
  line, the whole fact and the whole copy, and that a 400-byte fault is cut in `detail` and whole in the fact. The
  old window read the whole `faultMessage` directly, so this is a HAZARD the port would have introduced and the
  line avoids, not a fix of old behaviour (difference 9);
- THE LISTS: `UNBOUND_RADIOS` (40: `name` the bus description, `detail` the family `rtl`, `rx888` or `""`,
  `value[0..1]` vid and pid, rebuilt by every native walk, so the window's two USB id tables are deleted);
  `SCAN_EVIDENCE` (41, LOCAL and DENIED to a REMOTE session: `value[0]` 1 a vendor install, 2 a search path, 3 a
  module loaded); `SOUND_CARDS` (42: `id` index + 1, `ACTIVE` the entry the form's card matches the way the open
  matches it); `SOUND_CARD_RATES` (43: `value[0]` Hz, `FOXAPI_ITEM_EXCLUSIVE` 0x400000); `SOUND_CARD_FORM` (44, ONE
  entry: `value[0..2]` rate, centre, channel; the flags `FOXAPI_ITEM_IQ_STEREO` 0x800000, `SWAP_IQ` 0x1000000 and
  `PICKED` 0x2000000; `name` and `detail` empty to a REMOTE session); and the column `SAMPLE_RATES.detail` = the
  rate's caption (an Airspy's own label where it applies). `SCAN_EVIDENCE` is taken ONCE, when a whole scan lands
  having found nothing, and kept as a member hashed by a counter: each of the three asks is a call into the
  SoapySDR library, the first of which loads it and edits the process's environment and DLL search order, so it must
  not run on every block rebuild. It clears at a find or a partial scan, and an identical landing moves nothing. The
  faulted-driver list is refreshed at every landing;
- THE OP and THE MASK: `FOXAPP_OP_SOUND_CARD_PICK` 0x8420 (SETTINGS, LOCAL; `ival[0]` a SOUND_CARDS id): the engine puts
  the card in the form, marks it picked, clears `soundCardMissing`, and keeps the rate if the card offers it, else its
  default, else its highest (the rule the window held in its draw); `NOT_FOUND` for no entry and before the cards are
  listed. `FOXAPP_OP_SOUND_CARD_FORM` takes a field mask in `ival[1]`: 1 device, 2 format, 4 channel, 8 swap, 16
  picked, 32 rate, 64 centre; 0 and 127 are the whole form; above 127 or negative is `OUT_OF_RANGE`;
  `soundCardFormCommand(s, fieldMask)` builds it. The mask is the N9 shape from group 4: a control that changed one
  thing sends one field, so it cannot write back a stale copy of a field a pass changed since (a worker's open
  result replaces the whole form). `FOXAPP_OP_SOUNDCARD_IQ_CENTRE` already answered `NO_CHANGE` with no I/Q card
  running (now tested and documented), so the window sends it after every committed centre and its gate and its
  three reads are deleted. `FOXAPP_OP_SCAN_NATIVE_ONLY` is in no `kOps` entry and answers UNSUPPORTED through the
  table (pre-existing);
- THE ENGINE'S OWN PASS, `pumpSourceHousekeeping()` at the head of `pumpWorkers`: it clears
  `soapyScanDeferredLogged_` when the gate opens and lists the cards while `sourceSel_` is the sound card row and
  nothing is listed, scanning or opening. Both used to be commands the window sent every frame; the commands stay
  valid. Difference from today: the cards are listed with the section folded too (difference 6);
- TEST HOOKS: `testHooks_.nativeScanDetail(NativeScanResult&)` (the rows, the unbound dongles, the SDRplay advice, the
  detail and the row flag: the old `nativeScan` hook returned early and never reached the unbound list and the
  SDRplay assignments, so those sentences were unreachable in a hermetic test) and `scanDiagnostics()` (the three
  SoapySDR diagnostics and the faulted drivers). With `soapyScan` hooked and no `scanDiagnostics`, the engine reads
  none of the four real calls; the hermetic guard mutant for that went red only after a test fix (scan an empty list,
  so the diagnostics are actually asked).
The vendored header gained the state member, ten bits, five list ids, the four item flags and the op, and its
SHA-256, now 88db25170c1dc5f18fe94b3fb0ba71736621bfa9497b0caaad093f8a0a549512, is updated in
third_party/foxsdr_api/README.md and THIRD_PARTY.md.

Where step A DIVERGED from the survey: (1) `DEVICES` rows 1 and 2 are added at the FRONT (above); (2)
`fact.source.id.<n>` is keyed by the entry's id (index + 1), not by "row n"; (3) `SCAN_EVIDENCE` is taken once on an
empty landing and not on every block rebuild; (4) the faulted-driver list is refreshed at every landing; (5)
`SOUND_CARDS` names are visible to a REMOTE session (a card's name is a label, as a radio's is), and only the form's
name and detail and the two facts are LOCAL; an empty form device means the system's default input, and that entry
is `ACTIVE`; (6) `FOXAPP_OP_SCAN_NATIVE_ONLY` stays out of `kOps`; (7) the cards are listed by the pass with the
section folded.

THE FINGERPRINT, and the allocation numbers. The block, the facts, the composed sentences and the families of the
unbound dongles are built only when the list fingerprint moves (`buildApiListsSource`), and the fingerprint now
hashes: the combo's row, the kept source's kind, the open-pending flag, the busy label and the device model, the scan's
pending, scanned and partial states, `SoapySource::openDeviceCount()` and the patch radios still opening (the gate as
the answer it is: those two move it with nothing else changing), the network-USRP flag and the absent-driver names,
the SDRplay row flag, the Pluto buffer, the unbound dongles (vid, pid, description), the faulted drivers, the evidence
counter, the whole fault text, the rate captions, and the sound card's listing state, every form field and every
listed input (name, host API, default rate, each rate and its exclusive flag). The gate's ANSWER is deliberately not
hashed (a function of hashed inputs). Measured (tests/test_engine_publish_alloc case F5, a world with the section's
whole data): 40 fingerprints allocate 0, 40 unchanged `publishLists` passes 0, 40 full publishes 0, 1000 rounds of
six list reads 0; a change rebuilds once at about 240 allocations and the 40 settled passes after it allocate 0; all
19 probed inputs move the fingerprint ALONE; the same value said 25 times moves nothing. The facts allocate on the
READER's thread, by the table's existing design.

THE MUTANTS of step A, each applied alone, each restored after: 39 un-hash mutations (each input above taken out of
the fingerprint) and 38 that do the wrong thing (the latch clear; the cards-wanted line; the in-flight guard; each of
the four PICK rules and the picked flag; the fault line pointing at the cut field; the fault level; the
`EngineStatusText::deviceFault` copy; `apiFaultText_` cut at 127; the IQ-file DEVICES row and its id; the sound card's
`ACTIVE` in both lists; the whole-id facts cut at 159; `plutoUri` and `soundcard.device` not LOCAL; SCAN_EVIDENCE not
DENIED; the form's name shown to a REMOTE session; evidence taken on a non-empty landing and on a partial one; the
faulted-driver refresh; each of four flag bits; the `scanSkip` and `scanGate` facts; the unbound family token; the
rate captions; the span converter; the mask ignored, its rate bit dropped and its range check; `sourceFlags` out of
the device group; the hermetic guard). All 38 behaviour mutants are red (counting each PICK rule and each flag bit). Of the
39 un-hash mutants, 38 are red in the G11-10 probes, and the 19 inputs F5 probes are red there as well; ONE
SURVIVES AND IS DOCUMENTED: un-hashing `SoapySource::openDeviceCount()`, a
private static atomic with no seam, which cannot be moved from a hermetic test. The wiring in the fingerprint reads
correctly and the other half of the gate's input (the patch's radios) is killed; the process-wide count is not.
Tests: tests/test_engine_api_03.cpp G11-1 to G11-10 (2714 checks, 0 failed; it was 2094), test_engine_publish_alloc
301 (it was 223), test_apply_command 1175 (the PICK and the field mask; its "one case per known op" rule required the
new op's), test_receiver_snapshot 724, test_engine_api_conformance 377, test_soundcard_app_paths 276.

The pattern held, with these additions to it (step B):
- THE SOURCE CACHE, `sourceLists()` (gui/app_window.hpp: `SourceListCache`), read only when `listSeq` moves, as
  `railLists`, `fittedLists` and `storeLists` are: the combo's rows (the captions with "(native)" composed here, in
  the language in force: the fingerprint hashes the language, so a switch rebuilds the cache), `fact.source.row`,
  `keep`, `opening`, `scanGate`, `scanSkip`, `faultedDrivers`, the unbound dongles, the evidence, the radio panel's
  rates (with their captions), antennas, gains and options, and the sound card's lists, form and facts. The cache is
  read FIRST in `drawSourceSection` and in `radioNotOpenLit` and `soundCardReceivesText`, before anything below
  decides from it (group 10's rule). Cheap per-frame state (`sourceFlags`, the flags, `deviceName`, `sampleRateHz`) is
  read straight from `uiState_`. A click's whole id is read once, from `fact.source.id.<n>`, and not cached;
- THE DRAFT-PATTERN BOXES (group 7's scanner draft, `commitScannerDraft` and `flushScannerDraft`): the Pluto box
  holds `plutoUriPending_` and `plutoUriEditedAt_` and is re-seeded from `fact.source.plutoUri` only when no edit is
  in flight (each keystroke still sends `SET_PLUTO_URI`; the draft is released when the fact equals it or after 2 s
  without an edit; Open sends the draft, so it is never stale); the sound card's centre box holds
  `soundCardCentrePending_`, `soundCardCentreSentHz_` and `soundCardCentreEditedAt_`, keeps what was typed until the
  engine's centre equals it or 2 s pass, and commits on Return (`EnterReturnsTrue`), NOT on deactivation: the step B
  brief said deactivation, which would be a behaviour change;
- THE DELETE LIST, executed: the per-frame `applyCommand(CLEAR_STATUS, SOAPY_SCAN_DEFERRED_LOGGED)` and its local
  `scanGated`; the per-frame `applyCommand(SOUND_CARDS_WANTED)`; `soundCardCentreAppliesLive` and its three reads; the
  pick handler's rate rule, its `CLEAR_STATUS` and `soundCardRatesFor`; `submitSoundCardForm` and the free function
  that took the Engine; the window's `sourceIdForRow`; the three per-frame SoapySDR crossings and
  `sessionFaultedDrivers()`; the `rtlSdrUsbIds()` and `rx888UsbIds()` tables in the draw. Stays the window's:
  `iqPath_`, `plutoUriDraft_`, `sourceComboWasOpen_`, `soundCardCentreMhz_` (as the display value) and the "(native)"
  composition. The combo's scan on open is a `uiSubmit` on the open EDGE, sent once. `radioNotOpenLit` keeps its
  name (a test calls it) and is a function of the cache through the new pure
  `radioNotOpenLampFromSession(keepKind, hasDevice, generatorInUse, cardOpening)` in engine/source_fallback.hpp (the
  old overload stays for test_source_fallback);
- THE ACTIVE-CARD-ROW GATE, kept. Today's code gated the sound card combo's current row
  (`ImGui::Selectable(..., sel) && !sel`), so a click on the card already in use did nothing, and the engine
  applies SOUND_CARD_PICK for the active row all the same (it marks the card picked). The window keeps the gate: no
  PICK is sent for the active row, and a mutant dropping the gate goes red (3 failures).

Ratchet: 94 fields / 441 reads -> 60 / 313, the 34 fields and 128 reads the survey counted, printed by the guard as
`the window reads 60 engine fields directly, 313 times (budget 60 fields, 313 reads)`; the survey predicted it by
renaming the five members' reads in a scratch copy of src before the first edit (M3: 60 / 313, floor 13) and step B
printed exactly that. `kPortedMembers` gains `drawSourceSection`, `radioNotOpenLit`, `drawSoundCardControls`,
`soundCardReceivesText` and `sourceLists` (`sourceIdForRow` was never listed: it is a deletion). The 34 fields that
left the count are `biasTeePanel_`, `converters_`, `deviceAdcSwitchesPresent_`, `deviceAgcSupported_`,
`deviceAntenna_`, `deviceAntennas_`, `deviceBusyLabel_`, `deviceDabNotchPresent_`, `deviceDabNotch_`,
`deviceDither_`, `deviceHdrPresent_`, `deviceHdr_`, `deviceRandomiser_`, `deviceRateIndex_`, `deviceRateLabels_`,
`deviceRatesHz_`, `deviceRfNotchPresent_`, `deviceRfNotch_`, `lookForNetworkUsrps_`, `nativeRowLabels_`,
`nativeUnbound_`, `plutoUri_`, `restoreKeep_`, `sdrPlayRowsFound_`, `soapyAbsentDrivers_`, `soapyDevices_`,
`soapyScanPartial_`, `soapyScanned_`, `soundCardDevices_`, `soundCardListed_`, `soundCardLive_`,
`soundCardScanPending_`, `soundCard_` and `sourceSel_`. What STAYS in the count, by name and reader: `nativeDevices_`
(three reads in `run`'s bounded-run seam, which is why the figure is 60 / 313 and not M4's 59 / 310: the
`sourceIdForRow` call there became a const Engine method call, not a counted read, but the three direct reads
remain), `deviceOpenPending_`, `soapyScanPending_` and `soundCardOpenPending_` (the patch page's inspector and
`drawPatchView`), `deviceAgc_`, `deviceGainNames_`, `deviceGainRanges_` and `deviceGainsDb_` (`drawScopeMode`),
`sourceKind_`, `device_`, `patchRunning_` and `pipeline_` (the patch page, the scope, `reportPluginStatus`,
`copyDiagnosticsBundle`); those are later groups'. The floor `submitsInDraw >= 30` is `>= 13`, the printed `13
submitCommand calls in draw members` (30 until group 11 ported the Source section's seventeen; the floor is the scan
seeing the real files, not a ratchet). `kReadOnly` retired `device_.sampleRateHz`, `pipeline_.activeSource().name`,
`pipeline_.faultMessage` and `pipeline_.faulted`, their last callers gone (grep-confirmed; `pipeline_.activeSource()
.faulted` was already caller-less and unlisted). `kControlMayCall`, `kEngineObjects`, `kLineAllowed` and `kHostHooks`
did not move. The guard goes red on a single `engine_.` read put back: with `engine_.restoreKeep_.valid()` read back
into `drawSourceSection` on a scratch copy the printout is `FAIL: AppWindow::drawSourceSection is PORTED ... reaches
the engine directly at app_window.cpp:6854`, with 61 fields / 314 reads against the budget and 191 checks, 3 failed;
the real tree runs clean: `254 controls judged, 13 submitCommand calls in draw members, ... 0 violations`, 191
checks, 0 failed.

tests/test_source_section_window.cpp (new, globbed: the configure picks it up; 569 checks, 0 failed, about 2 s, four
runs): the real AppWindow over hooked engines, with the text of a frame captured through `ImGui::LogToBuffer()`.
A, the rows, the "(native)" captions, the scan sent once on the open edge only, and a selection by whole id; B, the
scan states; C, the evidence, the unbound dongles and the SDRplay note; D, the fault card, the lamp and the preview;
E, the radio panel; F, the IQ file and Pluto panels; G, the sound card; H, a sweep of every control in each state,
each sending exactly its expected command set (generator, Pluto, radio open, sound card real and I/Q, fault, IQ file,
a radio with every switch, a card opening, no card); I, a click landing at the engine's NEXT pass (nothing in the
click's frame, the value the frame after); J, the cache. The whole-id test sends a 296-byte Soapy id and a 199-byte
Pluto id whole and a cut id answers NOT_FOUND. Zero crossings: the diagnostics hook counts 1 over 400 frames after the
scan and 2 after a second one, an unhooked engine reads none, and the draw names no `SoapySource` and no
`sessionFaultedDrivers`. The sound card: the rate rule keep, default, highest and NOT_FOUND; the form mask (a stale
window copy cannot overwrite the engine's rate); the centre draft sends mask 64 and then `SOUNDCARD_IQ_CENTRE`
(`NO_CHANGE`); a card opening greys the controls and keeps the lamp dark. D1: the state copy is cut at 127 of the 134
bytes (and `reconnectAdviceApplies` is true on it), the drawn sentence comes whole from `statusLines_.deviceFault`,
and the advice is absent. Seventeen behaviour mutants, each applied alone and each red (the step B report said 16 and
listed 17; the results file holds 17): the cache refreshed on every call (9 failures), the active-row gate removed
(3), the field mask dropped (23), the advice read from the cut copy (2), the id cut at 159 (8), the Pluto draft not
held (5), the centre draft not held (6), the evidence module count dropped (3), `SOUND_CARDS_WANTED` every frame
(53), the scan on every open frame (36), the UHD hint ignoring its flag (1), the patch greying fixed rows (3), the gain
unit ignored (2), the rate row always first (1), the any-SoapySDR-row flag dropped (74), "(native)" dropped (7) and
the lamp ignoring a card opening (1). tests/test_soundcard_app_paths.cpp gained its LOOK (the frame's look before each
call into the window) and no assertion was touched: 276 checks, as before. Full suite, `ctest -C Release -j 8`:
254/254 (the first complete run, 220 s). test_theme_census failed once under load in step A's first run and once in
step B's last run, the second time as a crash inside `NahimicOSD.dll` (an audio-overlay DLL injected into the test
process by this desktop's audio software, 0xC0000005) while its other layouts passed; it passed solo three times each
time (46 to 49 s) and in the reruns. test_main_view and test_engine_api_transmit, the known flakes, did not flake.

Measured, this desktop, Release: 1000 settled `sourceLists` reads allocate 0 and refresh 0 times; the first, cold
read allocates 16; a rebuild allocates at most 5 and refreshes exactly once for each `listSeq` move; 20 DRAWN section
frames allocate 100, 5 a frame, the draw's own strings (a combo caption, the chip). The engine's side is step A's
F5 above. The old window did, every frame the section was open, a mutex-and-vector copy for `sessionFaultedDrivers()`
and a `soapyScanPlan()` vector while gated, and with the sound card row selected three strings and a converter
lookup, a `matchSoundCard` candidates vector and a `soundCardRatesFor` vector; while the "No radio hardware found"
block showed, three guarded calls into SoapySDR (`loadedModules`, `vendorInstalls`, `moduleSearchPaths`). None of it
was measured before the port, so there is no before-number, only the figure that the new draw does none of it.

THE THIRTEEN THINGS THE OPERATOR CAN SEE DIFFER, by the code and the tests (the survey's, step B's and one resolved
here); the capture below holds only the ones it says it holds:
1. One pass of lag on every command (rate, antenna, Auto gain, gain slider, bias tee, the RSP and RX888 switches,
   the network box, Refresh, a combo row, a card pick and each form field): the command is applied at the top of the
   next frame and drawn in it, where the old `applyCommand` was applied at once.
2. THE COMBO'S OPENING: `SCAN_DEVICES_ON_OPEN` is a `uiSubmit` on the open edge (sent once; the control's was an
   `applyCommand` on the same edge, behind the same `sourceComboWasOpen_` latch, that ran the native walk inline, so
   the rows drawn two statements later were the fresh walk). The first frame of an open combo draws the last walk's rows, then the fresh ones: visible only when a
   dongle was plugged or pulled since the last open. A click in that frame is matched by content, so a vanished row
   answers NOT_FOUND ("no scanned device matches") and opens nothing else.
3. A FORMAT CLICK on the sound card shows the dependent controls (the Channel combo where Swap and Centre should
   appear, the old rate list, the "Receives" line and the Open key's `canOpen`) as the old ones for one pass; the
   old file's own comment said every widget applied the whole form at once so nothing drawn later in the frame saw a
   stale value. Pinned by test G4 and HELD BY A PICTURE (below).
4. THE SOUND CARD CENTRE BOX keeps what was typed until the engine's centre equals it or 2 s pass (it was re-seeded
   from the engine every frame, with a one-frame flicker after Return). The commit trigger is Return, not
   deactivation.
5. THE PLUTO BOX: each keystroke still sends `SET_PLUTO_URI`; the draft is held while an edit is in flight and
   released when the fact equals the draft or after 2 s of inactivity; Open sends the draft.
6. THE CARDS ARE LISTED BY THE ENGINE'S PASS: zero `SOUND_CARDS_WANTED` over 120 frames, the backend asked once; they
   list with the section folded too (the old window listed them only while it drew the panel).
7. The per-frame `CLEAR_STATUS(SOAPY_SCAN_DEFERRED_LOGGED)` is gone; the engine's pass clears the latch.
8. "WHERE FOXSDR LOOKED" is the engine's evidence, taken once when a whole scan lands empty: a vendor module that
   appears while the block is up shows after the NEXT scan (the old window asked three times a frame), and the
   faulted-drivers note is `fact.source.faultedDrivers`, refreshed at each landing.
9. THE FAULT CARD: the text and the advice are as before (the whole sentence, and no reconnect advice under "restart
   FoxSDR"). It is not a difference of behaviour but is listed because the cut copy would have made it one (above).
10. A SoapySDR row's label is cut at 95 bytes (the list's `name`); the old combo drew it whole. Live labels were not
    read.
11. THE ANTENNA combo's preview and the one-port line come from the ANTENNAS entry that is `ACTIVE`; a readback
    antenna that is not in the list (none `ACTIVE`) used to be drawn and now is not (the boundary the status column
    already accepted).
12. "actual X MS/s" reads the state's `sampleRateHz` instead of `device_->sampleRateHz()`: the same object's rate,
    UNVERIFIED under an Airspy decimation or a converter.
13. THE ROW CAPTIONS: "(native)" is composed in the window at the cache refresh, in the language in force (the same
    msgid; "(nativ)" in German, test J4); the Pluto's caption is `tr()` of its label as before.

What the operator sees, in a picture. Drawn by the build before this group (the tree at da630fd: groups 1 to 10, no
group 11, in its own worktree, g11-control) and by this one: the application's own framebuffer capture in bounded
runs, four runs of each build in each of TEN states, the window 1502x1405 as the capture measures it (1500x1380
requested), FOXSDR_SINGLE_VIEWPORT=1, the SOURCE section opened by the seam FOXSDR_OPEN_SECTIONS, one scratch copy of
each exe with the same resources beside it, no radio visible to the process (SOAPY_SDR_PLUGIN_PATH removed from its
environment), each run on its own seeded copy of the config (CASCADE_CONFIG_TEST) and bounded by `--frames`
(build\g11-vis-final.txt is the table; runone11.py, campaign11.py, compare11.py, mkfix.py and the input scripts
s_<state>.txt are in the session's scratch, under g11-vis). THE STATES ARE ALL CHOSEN NOT TO SCAN, and the source
combo is never opened in a captured frame: the engine's start-up restore puts the source in the state, and
the input script (inside the process; no OS-level input) does the rest. GEN, the startup default: the generator in
the combo, the Refresh key and the network box; FILE, an I/Q file restored from the seeded config (a generated 2 s,
48 kHz stereo WAV): the IQ file row with its path box and Open key; KEEP, the config naming a recording that is not
there: the combo's "(saved, not open)" preview, the red "cannot open file" line with the whole path, the amber
sentence that the generator is standing in, the lamp dark (the rule for a file); CARD, a sound card restore of a card that does not exist
(an invented name, so no audio device is ever opened): the lamp lit, the Device combo, the real-format form (Format, Channel,
Sample rate), the Open key, "Receives 0 kHz to 24 kHz.", the "is not connected" sentence and the saved-card sentence;
CARDIQ, the same with an I/Q form, Swap ticked and a 7.1 MHz centre: the Swap box, the Centre box and "Receives
7.0760 MHz to 7.1240 MHz."; and five gestures: PICK (the card form's Device combo opened, not the source combo, and
the one listed input, a headset microphone, picked: the form takes it, the not-connected sentence is
cleared, the rate stays at 48 kHz); FMT (the Format combo and "I/Q (stereo)" clicked: Swap and Centre appear);
SWAP (the Swap box unticked); CARDOPEN (the Open key pressed for the missing card, its open failing on the worker)
and CENTRE (the Centre box selected, 3.5 typed, Return: the span becomes "Receives 3.4760 MHz to 3.5240 MHz."), the
last taken 50 frames after the commit. Areas: the capture is tiled exactly (162 areas over the ten states, the named
areas covering every one of its 2 110 310 pixels, so there is no unnamed remainder): the top deck, the rail's head,
the rail cut into horizontal bands named for what the Source section draws there in that state (the header row, the
combo, Refresh and the network box, the path row, the Device combo, Format, Channel or Swap, Rate or Centre, the Open
key, the Receives line, the sentences, the Converter block, and "the Radio section and the rest of the rail"), the
rail's foot, the patch page and the frame's ring; 10 areas in GEN, 11 in FILE, 12 in KEEP, 17 to 19 in the
sound card states. At tolerance 24, 0 pixels differ in every area of every state, between the two builds (16
control-against-port pairs a state) AND between two runs of one build (6 control-against-control and 6
port-against-port pairs). At tolerance 0 the result is the same: 0 in every area, every pair and the whole capture,
so there is no noise to bound. It is stronger than that: the eight captures of each state (four of each build) have
ONE MD5 between them, so they are the same file, ten states, ten MD5s; the stderr is identical in the two builds in
every run and the stdout differs only in the teardown time and the worst frame gap; and the scratch config each run
saved on exit is identical between the builds in all 40 pairs, the telemetry install id apart (a random id each
launch, in the scratch copy only). The sound card list this desk offers did not vary: the PICK state lists the one
input in all eight runs. Because that is a table of zeros, four checks that it is not a comparator that cannot see:
control CARD against control CARDIQ reports 545 pixels (757 at tolerance 0) in the Format row, 2 120 (4 358) in
Channel, 1 623 (1 954) in Rate, 2 461 (4 526) in Open, 10 710 (13 984) in the sentence, 11 649 (20 124) in the
saved-card sentence, and 0 above the Format row; control CARD against control PICK reports 3 019 (3 909) in the
Device combo and 10 126 (17 950) in the not-connected sentence's band; control GEN against control FILE reports 614
(836) in the combo and 4 315 (10 864) in the path row; and a +30 and a +10 poke on single pixels are counted as 1
pixel at tolerance 24 and 2 at tolerance 0. THE ONE DIFFERENCE THE PICTURE CAN HOLD, held: difference 3, the Format
click, shot frame by frame (control and port, two runs each, frames 50 to 58, the click down at frame 48 and up at
52). Frames 50 and 51 are identical in the two builds. At frame 52 the control has already drawn the I/Q layout
under the open popup and the port has not: 134 176 pixels differ, rows 551 to 1312 (the Centre box's place, and
everything under it shifted by one row of controls). At frame 53 the port has the I/Q layout and the rail's scroll
thumb one frame behind: 249 pixels differ at x 392 to 400, rows 1220 to 1248, and in that strip port frame 53 equals
control frame 52 and port frame 54 equals control frame 53 with 0 differing pixels, so it is the same one-pass delay
and not a third difference (the thumb follows the content height a frame later in ImGui). From frame 54 on the two
builds are the same file again (MD5 8915a3ce...). The same bracket on PICK and on SWAP shows no frame that differs
at all (the combo's and the checkbox's own state is drawn in the click's frame from a local copy in both builds).
THE THIRD-DIFFERENCE RULE therefore found nothing to investigate: every pixel that differs is difference 3.
What the capture does NOT draw, and so does not hold: any radio row (no radio is visible to the process): the radio
panel's rate, antenna, Auto gain, gain, bias tee and option controls (differences 1, 11, 12), the combo opened
(difference 2), the "(native)" captions (13), a Soapy row's 95-byte cut (10), the Pluto row and its box (5), the fault
card (9), the busy "Opening ..." and "Scanning for devices..." lines, the scan-deferral sentences, and every state
that follows a scan: the unbound-dongle sentences, the SDRplay note, "No radio hardware found" and the evidence
(8). THE SCAN STATES WERE TRIED AND SET ASIDE, under the house rule: one run of the port build with the Refresh key
pressed (frames 310 and then 1310 of a bounded run, no control run) showed that the scan never landed in 12.7 s (the
section still read "Scanning for devices..." at frame 1300), the run's stderr carried libusb's enumeration of this
desk's USB bus and a UHD X300 network-discovery error (the in-process SoapySDR fallback is probing the machine and
the network), the capture changed between frames 600, 800, 1000 and 1300 (three different files in five), and the
section was drawing the desk's own real, unbound RTL dongle and the missing SDRplay API. That is timing- and
hardware-dependent, so it was not campaigned and nothing in it is claimed; those sentences, the evidence tree and
difference 8 are held by test_source_section_window B, C and the F5 and G11 cases only. Difference 6 is held by a
counter, not a picture. The real config (%APPDATA%\foxsdr\config.json, 5 076 bytes, written at 14:52:27 UTC by
another session, sha256 BA532E6C...A353C) was identical before the first launch and after the last (size,
modification time and hash), and none of the scratch values (the card's invented name, the recording's path) is in it.

What could move a measured number: the engine's block rebuild (step A's F5, about 240 allocations) whenever the
fingerprint moves, and the fingerprint now follows the sound card's whole list and every rate, so a card plugged or
pulled rebuilds it once; the evidence's three SoapySDR calls run once per empty landing, on the control pass, and
the first of them loads the library and edits the process's environment and DLL search order (not measured: no
SoapySDR runtime of this desk was driven by a test); `scanNative` (a SetupAPI walk, an SDRplay service query and an
unbound-device walk) still runs inline in whichever pass applies the scan, and when the pump moves to the engine's
control thread it sits inside a pass (not timed; the survey said the same); `pumpSourceHousekeeping` runs at the head
of every `pumpWorkers` and lists the cards even with the section folded, so a desk with a slow PortAudio backend now
pays the listing once at the row's selection and not on first sight; `sourceFlags` adds four bytes to every publish
and `soapyScanGated()` is evaluated each pass; and a window rebuild of the cache is at most 5 allocations once for
each `listSeq` move, which moves with every input above and with a decoder's FED toggle.

THE TRAPS, for the next group (step B's own list is long and every item cost time; step A's are with it).
1. A test that sets `sourceSel_` to 2 directly now STARTS A REAL CARD LISTING (the engine's pass lists the cards while
   the row is the sound card): it must install `testHooks_.soundCardBackend`, or it enumerates this desk's PortAudio.
   `sourceSel_` is hashed now, so setting it directly also moves `listSeq`.
2. A test frame is LOOK, DRAW, then `pumpWorkers` and `pumpAudio` (group 10's trap), and a call into a ported member
   that follows an engine mutation needs the LOOK first (`pumpFrameBegin`, `pumpInputs`, `pumpPlugins`, `pumpPublish`,
   `statusLines_ = engine_.statusText()`, `refreshUiState`) because the member now reads the window's cache: the nine
   lamp calls and five "Receives" checks in test_soundcard_app_paths gained one.
3. The rail's section header is a `benchSection` row: a click on it folds the section into a drawer child that only
   the rail's flush ends ("Must call EndChild()" errors in a test window). A sweep starts below it (y >= 38 here).
4. `ImGui::LogToBuffer()` after `Begin` captures every widget's text, popups included (checkboxes as `[ ]` and `[x]`,
   combos as `{ ... }`); its default depth auto-opens tree nodes, which is how "Where FoxSDR looked" is asserted.
   Disabled widgets still show as hovered ids in a sweep, so grey is tested by what a click sends.
5. `nativeScanDetail` adds no Pluto row (the test lists it); hooked row labels are bare; the Pluto id comes from the
   hook's arguments. `Engine::testHooks_` is private: a test sets it through its friend accessor.
6. A `makeDevice` hook that waits on a gate holds the open on its worker: a real DEVICE_OPENING state whose scan
   gate is the deferral sentence.
7. `%.4g` is "actual 2.4 MS/s", not 2.400. A frame's look applies the PREVIOUS click's command, so "nothing in the
   click's frame, the value the frame after" is the right assertion. The 2 s draft timeouts are tested with ImGui's
   frame time (1/60 s a frame), not wall time (UNVERIFIED against a real 2 s).
8. The engine's fingerprint hashes the language, so a language switch rebuilds the cache and re-composes "(native)".
9. The shell tool collapses `\\` in heredocs: write C++ containing `\n` through the file tools. Git Bash `sed -i`
   strips the CR from the CRLF files in this tree (group 10's trap, again): the window files and this one are CRLF.
10. A capture that wants to be deterministic must not scan, and the source can be put in a state without opening the
    combo: the config's `sourceKind` and `iqFilePath` or `soundCard` restore it at start-up (a card that does not exist
    fails on its worker without opening any device, and the rest of the panel is the same). The sound card form's
    combos are not the source combo: opening them scans nothing. Any state past a Refresh press is hardware- and
    timing-dependent on this desk (above).
11. UNVERIFIED, carried forward: no real SoapySDR, USB or sound hardware was driven (every test is hooked); the real,
    non-hooked `takeScanDiagnostics` path into SoapySDR beyond one mutant run; the "actual" rate under an Airspy
    decimation or a converter; the 95-byte label cut against live labels; and the one surviving mutant
    (`SoapySource::openDeviceCount()`, no hermetic seam).

GROUP 12, the two scopes (drawScopeMode and drawScopeModeControl, the radar; gatherDemodScope and pipelineIsWfm, the demod
page's feed; with drawDemodScopePage, drawDemodScopeSection and scopeSignalNow held at zero reads; the window gained
`scopeGainLocal_`, `scopeKnob_`, `hostTracks()` and gui/scope_gain.hpp): API 0.3 section 8.11, one pair of members appended
to the state with one flag, one field of the scope info, no op, no list, no fact, no status line. The window's own survey,
written before the first edit, found 45 counted field reads in FOUR members (30 in `drawScopeMode`, 11 in `gatherDemodScope`,
3 in `drawScopeModeControl`, 1 in `pipelineIsWfm`), 2 `submitCommand` lines and no `applyCommand` and no same-frame site at
all, and where the brief and the code disagreed: (1) THERE ARE TWO SCOPES, not one. `drawScopeMode` and
`drawScopeModeControl` are the RADAR scope, the aircraft plan-position indicator that replaces the whole window (range
stepper, SIG and ALT gauges, GAIN knob, POWER key, no waveform); the demod page's oscilloscope is `gatherDemodScope`,
`drawDemodScopePage` (0 reads) and `drawDemodScopeSection` (the rail key, 0 reads), and `pipelineIsWfm` carried one read the
brief had not listed. The halves share a name and nothing else (the radar is 33 reads and the two commands, the demod page
12 and none); (2) by the guard's rule the counts are 30, 11, 3 and 1, where the brief had about 28, 10 and 4 (the fourth of
`drawScopeModeControl` is a method call, `pluginIsStopped`); (3) "FOXAPI_CAP_SCOPE is reduced, a gap" was stale, not a gap:
`read_scope` has been served since step 2 and "reduced" is a REMOTE-session limit only (4096 frames), the window's LOCAL
session gets whatever the ring holds; the header comment still said "(reduced)"; (4) `read_scope` did not need "channels,
trigger state, the control's settings": channels are served, the trigger is window arithmetic over the samples, the
control's settings are the window's own persisted view state, and the DSP thread only PUSHES to the rings, so the window is
already a lock-free READER of them; the one real gap is the ring's CAPACITY; (5) the four gain fields group 11 had left in
the count (`deviceAgc_`, `deviceGainNames_`, `deviceGainRanges_`, `deviceGainsDb_`, 16 reads) are read by nothing but the
radar's knob, so they leave with this group; (6) the radar half cannot become session-only, and the reason is not the
engine's: its 6 track reads are HOST-PROCESS (below), and its 6 position reads were a genuine small gap (the position was in
the published block for `/api/status` but not in the table); (7) three members the two halves share with other groups are NOT this
group's and were left alone, so nobody ports them by accident: `drawReceiverPositionOffers`, `drawRxPositionEntry` and
`drawGpsPositionControl` (also drawn on the map pages; the last has three same-frame `applyCommand` sites). Step A put the
table's side in place, step B the window's.

What moved into the table (step A; docs/api-0.3-additions.md section 8.11, with a per-datum table of what each scope draws
and from where):
- 8.11.1, THE RECEIVER'S POSITION. `FoxReceiverState` gains at its END two doubles, `rxLatDeg` and `rxLonDeg`, and the flag
  `FOXAPI_RX_POSITION_SET` 0x40000000 (the next free bit of `flags`, verified free; 0x80000000 is still free). The pair is
  published as 0.0 unless the flag is set, even with leftover coordinates in the configuration (a position that was never
  set or was cleared is never published as 0 N 0 E with the flag up: the flag is the truth). The position restored from the
  saved configuration is in the FIRST published state (a real-app test, test_patch_map_credit, opens the radar scope with a
  config position and requires the `scope:area` census item, so the first frame must already see it), and
  `FOXAPI_OP_SET_POSITION` is unchanged (refused out of range). `FOXAPI_MIN_RECEIVER_STATE` moves to the end of `rxLonDeg`;
  the struct grows 16 bytes (4 of them padding before the first double). They move `seq` and no other counter: the flag
  word through `RxFlagSources::positionSet`, the coordinates through the snapshot's "other" comparison;
- 8.11.2, THE SCOPE TAP'S CAPACITY. `FoxScopeInfo::reserved0` becomes `uint32_t capacityFrames` (same offset and size,
  `FOXAPI_MIN_SCOPE_INFO` unchanged, an engine that predates it leaves 0, which a client reads as "ask for what you need and
  take what comes"): the most frames a `read_scope` on THIS session can return for the tap, 65536 audio, 262144 I/Q and 65536
  multiplex on a LOCAL session and `min(ring, 4096)` on a REMOTE one, filled even when `capFrames` is 0 so one info-only call
  per tap is enough. What it is for: the sweep is ten divisions of the time base and the longest base, 50 ms, asks for more
  frames than the I/Q ring holds above about 524 kHz of channel rate (a trigger that hunts asks for twice a sweep, from about
  262 kHz); without it a window sees `got < want` and cannot tell "the ring is the limit" from "not that many yet". The same
  could be inferred from `written`; the survey recommended against leaning on a counter read before the snapshot;
- THE SECTION 6 CORRECTIONS, made in the same change: a NEGATIVE return of `read_scope` is an error (`DETACHED`,
  `BAD_ARGUMENT`, `UNSUPPORTED`) and a client must test it before it uses it as a count; `info.structSize` is MANDATORY (a
  short or zero one fails the whole call as `BAD_ARGUMENT`, a dead scope with no message); and "(reduced)" is retired from
  `FOXAPI_CAP_SCOPE`'s comment (it now says what the limit is: a REMOTE session is held to `FOXAPI_SCOPE_REMOTE_FRAMES` a
  read). The vendored header's SHA-256, now 4a6faf8a4ac3d84b33fa41bc15d9ffa6620fb20752a8077a2fe5397b9cff4c3c, is updated in
  third_party/foxsdr_api/README.md and THIRD_PARTY.md;
- THE SNAPSHOT-GROUP DECISION: the position is in the state's CHANGE DETECTION (a change moves `seq`) and NOT in the lists'
  fingerprint. Nothing a list serves depends on it and a position change must not rebuild the block; the test holds both
  halves (F6 below: a position does not move the fingerprint). The decision was confirmed at review.

Where step A kept to the survey and where it did not: the flag went in `flags` and not in a new word beside `sourceFlags` (the
survey had offered either; group 11 spent a word because that one was nearly full, and a bit remained here); nothing else
diverged. TIMING, which the section documents: the tap's frames and `written` are read AT THE CALL and are never a publish
old; everything else is one pass old at most, and "an interface whose control computes its next value from the readback (a
knob that adds a step) must hold its own copy of the value until the readback catches up" (the lost detent, below).

THE TESTS and the break-it checks of step A: group G12 of tests/test_engine_api_03.cpp (G12-1: the position state, the
refused ranges, repeats, a config-restored first publish, a stale config pair; G12-2: the capacity per tap and session, the
cut at the ring, an unknown tap, a short and a zero `structSize`, a null buffer), 2809 checks, 0 failed (it was 2714);
tests/test_receiver_snapshot.cpp (a `positionGroup` case, the layout asserts, the walking-one flag test), 768 checks;
tests/test_engine_publish_alloc.cpp case F6: 40 publishes with no position and 40 with one allocate 0, 1000 rounds of
`read_state` and three info-only `read_scope` calls allocate 0, and a position does not move the list fingerprint; 315
checks (it was 301). Nine mutants, each applied alone and each red, restored after: the flag never set, the flag at the wrong
bit, latitude and longitude swapped, the coordinates published without the flag gate, latitude not compared, longitude not
compared, the REMOTE capacity not reduced, the I/Q capacity taken from the audio ring, the multiplex capacity 0. Full suite
after step A: 254/254.

The pattern held, with these additions to it (step B):
- THE SPLIT OF THE TWO SCOPES. The radar reads the position and the flags from `uiState_` (RUNNING, DEVICE_OPEN, DEVICE_AGC,
  `signalDb`), the knob from the FIRST `GAINS` entry through the existing `sourceLists()` (no new cache reader), the
  "scope will be empty" census from `pluginLists().plugins` (`censusOf(..., CASCADE_CAP_TRACK_SOURCE)`, group 8's way), and
  sends POWER and the detents through `uiSubmit`; `FOXAPI_OP_RUN` stays TEXTUALLY inside `drawScopeMode`, because
  tests/test_stop_ends_recordings.cpp:1095 greps for it there and a helper would turn it red. The demod page's feed reads
  `read_scope` at the call: THREE info-only calls a frame (cap 0) for the three taps' liveness, the rates and the
  capacity, then the data of the selected signal; the liveness records are updated every frame whichever signal is shown,
  as before. `pipelineIsWfm` is `uiState_.demodMode == FOXAPI_DEMOD_WFM`; whose sound the trace is comes from the state's
  `audioPlayingPlugin`; the audio rate is the tap's, the I/Q rate the I/Q tap's with the state's `channelRateHz` as the
  fallback when the seqlock read gave up (an unguarded 0 would blank a frame). The sweep is clamped to `capacityFrames`
  exactly as it was clamped to the ring. A failing read draws nothing and a negative count is never a length (the mutant
  that read the count as a length died by 0xC0000005 in the test);
- `hostTracks()`, THE ONE REMAINING ENGINE NAME. One accessor wrapping `engine_.pluginUi_.tracks()`; it is NOT in
  `kPortedMembers` (it must name `engine_`), and the guard counts it as one read. TRACKS stays HOST-PROCESS, as the plan
  says (the plugin windows "belong to the host process, not to an interface", section 3, lines 76-78 of this file, and the
  step 5 plan, lines 302-309; and section 10 of the additions keeps `TRACKS` a GAP): the plugin ABI has a track
  source polled "from the GUI thread at frame
  rate" (core/plugin_abi.h:763-766), the poll is `drawPluginWindows`' `pluginUi_.poll()` which runs BEFORE the scope in the same
  frame, and `ScopeView::draw` takes `core::HostTrack`, `basemap_` and `trackInfo_`, all host-side types. A `TRACKS` list
  could not carry what the view reads (id, label, position, altitude, course, speed, age, kind, flags) in a `FoxListItem`
  without a second list or a new read. What stays cross-thread, not made worse, for step 5: the engine's own status build
  reads `pluginUi_.tracks()` (engine.cpp:5289) on whichever thread publishes, and once the pump moves that is group 9's F6
  class; the accessor concentrates the window's side of it in one place;
- THE LOST-DETENT FIX, `scopeGainLocal_` (gui/scope_gain.hpp: stage name, the dB sent, when). A detent sends the stage's
  readback plus two units, clamped; the old knob read the readback live, and a command applied at the top of the next
  frame, so the next frame already held the new figure; from the session the readback arrives a pass after the pass that
  applied it, and two detents in two frames before then would both add to the old figure, so one is LOST. The next detent
  computes from the kept figure, which is released when the readback equals the figure sent, when the stage changes (a
  different first stage is another radio), or after 0.5 s. IT DELIBERATELY DIFFERS FROM `tuneLocal_`'s RULE, which is dropped
  when the next state is read: that is right while the window's frame IS the pass and loses a detent the day the pass runs
  on its own thread, and this one must survive the pump move. A single detent is identical to today. The radio's own AGC
  reads "AUTO", lights no tick and sends nothing. The knob's rules are pure functions (travel, the five ticks, the lit tick,
  the pointer's fraction, the detent arithmetic, the local copy's rules) so they are tested without GL;
- THE CABINET'S BADGE: the cabinet's badge texture is uploaded lazily (a GL call and no context in a test), so a hermetic
  frame reaches the knob only by handing the window a texture id it never uploads; the bare scope draws below 900 pixels
  wide and has no knob;
- THE PER-FRAME COST THAT LEFT: the demod page used to lock the plugin runner's mutex every frame (the lock the DSP thread
  takes per block) and build a `std::string` for `playingPlugin()`; the draw no longer touches `pluginRunner_` (by the
  guard, by a source check in the new test and by the state-fed name; the old cost was NOT measured, code reading only).

Ratchet: 60 fields / 313 reads -> 56 / 269, printed by the guard as `the window reads 56 engine fields directly, 269 times
(budget 56 fields, 269 reads)`; the survey predicted exactly that by renaming the four members' reads in a scratch copy of
src (44 reads leave, the one in `hostTracks()` stays) and step B printed it. The four fields that left the count are
`deviceAgc_`, `deviceGainNames_`, `deviceGainRanges_` and `deviceGainsDb_`. `kPortedMembers` gains `drawScopeMode`,
`drawScopeModeControl`, `gatherDemodScope` and `pipelineIsWfm`, and the three with no engine reads so they can never grow
one: `scopeSignalNow`, `drawDemodScopePage` and `drawDemodScopeSection`. The floor `submitsInDraw >= 13` is `>= 11` (the two
radar commands are `uiSubmit` calls now; the floor is the scan seeing the real files, not a ratchet). `kReadOnly` retired
fourteen entries whose last caller is gone (grep-confirmed): `pipeline_.signalPowerDb`, `pipeline_.channelRateHz`,
`pipeline_.demodMode`, the nine `pipeline_.scopeAudio()`, `scopeIq()` and `scopeMpx()` `.capacity`, `.snapshot` and
`.written` entries, `pluginRunner_.playingPlugin` and `pluginUi_.trackPluginNames().empty`; `pipeline_.running`,
`pluginUi_.tracks`, `pluginUi_.trackPluginNames` and `pluginHost_.plugins` stay (other readers). `kControlMayCall`,
`kEngineObjects`, `kLineAllowed`, `kHostHooks` and `kPublishedOnly` did not move. The guard goes red on a single `engine_.`
read put back: with one read put back into `drawScopeMode` on a scratch copy the printout is `FAIL: AppWindow::drawScopeMode
is PORTED ... reaches the engine directly at app_window.cpp:11879`, with 270 reads against the budget of 269 and 2 checks
failed; the real tree runs clean:

    255 controls judged, 11 submitCommand calls in draw members, 656 window-machinery lines, 46 allowed engine lines in drawing members, 0 violations
    the window reads 56 engine fields directly, 269 times (budget 56 fields, 269 reads)
    test_command_path_guard: 198 checks, 0 failed

(it was 191; the 255 controls judged, one more than before, include `hostTracks()`, which passes as a judged control, as
the survey's mutant predicted).

tests/test_scope_gain.cpp (new, globbed; 54 checks, 0 failed): the knob's travel and fallback, the five ticks, the lit tick,
the pointer's fraction, the detent arithmetic and both clamps, and the local copy's rules. tests/test_scope_window.cpp (new,
378 checks, 0 failed, about 10 s, three stable runs): A, the demod feed (the old gather restated over frozen taps and held
equal, 15 signal and time-base pairs bitwise identical; liveness from all three taps, a frozen ring stale after 0.25 s; the
WFM gate with its one pass of lag and the saved setting never rewritten; whose sound from the state; the clamp to capacity
through a wrapped table (a 700 kHz channel cut to 262144, because the real channel rate is capped below 300 kHz and the
clamp is unreachable in this tree otherwise); zero capacity and zero rate fallbacks; failing reads draw nothing; live real
frames); B, the radar's empty state and position (the one pass of lag, latitude and longitude told right, the bare scope at
700x600, the rail's block); C, the knob and POWER against the fake radio (30 dB on a TUNER stage: lit tick 2, fraction
0.5988, travel 0 to 49.6; the wheel's detents clamped at both ends; THE LOST DETENT: 29.7 to 31.7 and 33.7 then 35.7 and the
radio reaches the sum; a readback that never matches releases after the hold; the radio's AGC reads AUTO, lights nothing and
sends nothing; POWER sends RUN 1 and 0; a source swap drops the copy); D, the source check (no `engine_.`, `pipeline_.` or
`pluginRunner_.` in the members, `FOXAPI_OP_RUN` in `drawScopeMode`). Nineteen window mutants, each applied alone and each
red (the one that read a count as a length is red by a crash, 0xC0000005), restored after. Full suite, `ctest -C
Release -j 8`: the step B run was 256/256 (the 254 plus the two new tests); the last full run, the
one in the worktree's ctest-after-g12.log, is 255/256 with test_main_view the lone failure in the parallel mix and 6 of 6
solo passes, the documented flake (test_command_path_guard, test_scope_gain, test_scope_window, test_stop_ends_recordings and
test_patch_map_credit passed in it, and test_patch_map_credit is the real-app regression for "the first frame sees the
position").

Measured, this desktop, Release: a settled demod frame allocates 0 (60 frames for each of the five signals, the trace live
in 70 of 70, a 68-byte playing-plugin name); a radar cabinet frame with a radio and a position allocates 0 (60 settled
frames); 1000 rounds of `read_state` and three info-only `read_scope` calls allocate 0 (F6). `read_scope` on an empty tap
costs about 89 ns a call (the survey's measure through test_tuning_surface, the call overhead: a seqlock read and the guard
wrapper), so the three info-only calls are about 0.27 us a frame by arithmetic, which was NOT measured as a whole in situ.

THE SIX THINGS THE OPERATOR CAN SEE DIFFER, by the code and the tests (the survey's and step B's); the capture below holds
the ones it says it holds:
1. The position, the POWER lamp, ON LINE and STANDBY, the signal gauge, and the demod page's rate, mode and WFM key trail the
   command by one engine pass (about 16 ms once the pass has its own thread); the radar's empty state can show one frame
   longer after a position is set; the waveform itself has no lag (it is read at the call). Today the pass still runs at the
   head of the window's frame, so no picture shows it: the POWER bracket below is identical frame by frame.
2. After a detent the knob shows the figure it SENT for up to 0.5 s if the radio never reads back exactly that figure (today:
   the readback the next frame); identical with the fake radio, which reads back what it was sent.
3. A double POWER click inside one pass can send two RUN commands (the engine's safer-merge coalesces a stop; a start is not
   merged).
4. The playing-plugin label is cut at 127 bytes and the gain stage name sent at 95 (no real name is that long).
5. "The scope will be empty" now depends on the PLUGINS census rather than the plugin UI's track-source list: the two differ
   for a module whose `create()` returns nothing (PluginUi then has no instance) and for one pass after a rescan.
6. The time-axis rate comes from the info-only read made just before the samples, not from the same call.

What the operator sees, in a picture. Drawn by the build before this group (the tree at 2c8f96e: groups 1 to 11, no group
12, in its own worktree, g12-control) and by this one: the application's own framebuffer capture in bounded runs, four runs
of each build in each of TWELVE states (96 runs), the window 1502x1405 as the capture measures it (1500x1380 requested),
FOXSDR_SINGLE_VIEWPORT=1, one scratch copy of each exe with the same resources beside it, the built-in signal generator as
the source, no radio visible to the process (SOAPY_SDR_PLUGIN_PATH removed from its environment), each run on its own
seeded copy of the config (CASCADE_CONFIG_TEST; telemetry and the update check off, volume 0, the receiver position 51.47 N
0.45 W in the radar states) and bounded by `--frames` (build\g12-vis-final.txt is the table; runone12.py, campaign12.py,
compare12.py, bracket12.py and the input scripts s_<state>.txt are in the session's scratch, under g12-vis). No state scans.
The receiver starts STOPPED, so the running states press the master START (demod) or the radar's POWER key (radar) once from
the input script (inside the process; no OS-level input) at frames 38 to 42 (the `_NS` states press POWER first and then two
more clicks, the last released at frame 78), and the capture is at frame 120, or 130 for the `_NS` states, at least 52
frames after the last click, so no capture holds a click's frame. The radar scope is opened by
FOXSDR_OPEN_RADAR_SCOPE (bounded runs only) and the
demod page by FOXSDR_OPEN_DEMOD_SCOPE. The states: RADAR_OFF (the receiver stopped: STANDBY, the SIG gauge empty, "0 PLOTTED",
the TRACKS drum 000, the knob dead at "N/A no device", the position in the corner "51.5N 000.5W"); RADAR_ON (POWER pressed:
ON LINE, the lamp lit, the SIG gauge lit to -30); RADAR_NOPOS (no position: "The scope needs the receiver's position." and
the three offers); RADAR_OFF_NS and RADAR_ON_NS (the same two with the info panel's SYS tab open, which prints the position
readout "ANTENNA 51.470 -0.450", and the sweep switched OFF by one click on its row: see below for why); and seven demod
pages on the generator in WFM unless named, DEMOD_AUDIO, DEMOD_SPEC, DEMOD_IQ, DEMOD_VEC, DEMOD_MPX (the fifth key), DEMOD_AM
(the gate closed: four keys) and DEMOD_STOP (the receiver stopped: "NO SAMPLES - RECEIVER STOPPED"), the mode and signal from
the seeded config. Areas, tiling the capture exactly (2 110 310 pixels; the named areas are asserted disjoint and each
state's last row is the remainder, the whole capture minus the named areas): in the radar states 11 named areas (the
frame's ring, the top deck, the scope bar, the SIG column, the PPI disc, the info panel, the ALT column, the FOX plate,
the GAIN plate in two pieces, the dial and its readout, and the POWER plate with its lamp and label) and a remainder
of 233 844 pixels (the cabinet's frame and the gaps between
the plates); in RADAR_NOPOS 5 and a remainder of 1 458 608 (the empty page); in the demod states 15 (the ring, the top
deck, the rail, the rail's foot, the patch page round the scope window, the scope window's title bar, signal keys, TIME,
GAIN and DISPLAY rows, the tube's trace area, and the tube's label strip in four pieces: the title at the left, the centre
time-axis label, the right gain label and the gaps) and a remainder of 62 208 (the window's frame and bezel).

THE RESULT, in two parts, because two of the areas are alive. EVERYTHING THAT IS NOT LIVE: at tolerance 24, 0 pixels differ
in every named area and in the remainder of every state, between the two builds (16 control-against-port pairs a state)
AND between two runs of one build (6 control-against-control and 6 port-against-port pairs). At tolerance 0 the result is the
same: 0 in every such area, every pair, and 0 in the whole capture minus the live areas. That includes the radar's whole
cabinet (the SIG gauge, the POWER lamp and its ON LINE or STANDBY word, the knob's dial and its readout, the plate and its
FOX badge, which is a REAL GL upload in this run, the TRACKS and SET RANGE drums, the info panel with its position
readout), the empty state, and the demod page's chrome: the signal keys (five under WFM, four under AM, so the gate is
held), the TIME and GAIN rows with their time-base and auto-gain text, the DISPLAY row, the tube's title, its centre
time-axis label ("10 ms/DIV", "0 - 20.0 kHz", "0 - 100.0 kHz" by signal), its gain label and the gaps, all at 0 in every
pair of every state, so none of the axis labels jitters between the eight runs of one build and nothing was set aside. THE
LIVE AREAS, measured and NOT part of the 0-pixel claim, because their content is not a function of the build: (a) the demod
tube's trace area, 227 976 pixels (the graticule with the live trace over it; the graticule alone is static, and the area
cannot be split from the trace), where the pictures depend on the live samples: DEMOD_AUDIO cvp24 max 1 029, cvp0 0 to
1 484, control-against-control 1 385, port-against-port 1 484, noise at tolerance 24 (934, 1 029); DEMOD_IQ cvp24 29, cvp0
0 to 29, cvc0 21, pvp0 28, noise24 (21, 28); DEMOD_VEC cvp24 0, cvp0 0 to 7, cvc0 7, pvp0 7, noise24 (0, 0); DEMOD_AM cvp24
4 001, cvp0 0 to 4 884, cvc0 4 884, pvp0 4 884, noise24 (4 001, 4 001); and DEMOD_SPEC, DEMOD_MPX and DEMOD_STOP 0 in every
pair, one MD5 across all eight captures of each. The control-against-port counts are inside the noise bound of one build
(DEMOD_IQ's 29 is one pixel over the largest within-build count, 28, from 16 pairs against 6), and the evidence that the
port draws the same waveforms the control does is stronger than a count: the pictures REPEAT, byte for byte, ACROSS the
builds (the captures of DEMOD_AUDIO are 3 distinct files in 8 and 2 of the 3 are drawn by both builds, DEMOD_IQ 5 files and
2 shared, DEMOD_VEC 4 and 3 shared, DEMOD_AM 3 and 2 shared). The generator is quiet: in the spectrum and multiplex tubes
no trace is drawn in either build (only the graticule, the labels and the multiplex legends Mono L+R, Pilot, Stereo L-R,
RDS and SCA), so those two paths through the samples are NOT held by a picture; the feed is held by test_scope_window A,
which restates the old gather over frozen taps and holds every signal bitwise equal. (b) The radar's PPI disc, 678 976
pixels, IN THE TWO STATES WITH THE SWEEP ON: the sweep wedge is driven by the wall clock (scope_view.cpp:2481, `ImGui::GetTime()
/ 4.2`), so its position in the capture is where the clock happens to be: RADAR_ON cvp24 886, cvp0 1 739 to 42 264, cvc0 36
244, pvp0 33 005, noise24 (748, 674); RADAR_OFF cvp24 644, cvp0 1 265 to 31 701, cvc0 24 983, pvp0 10 985, noise24 (504,
213); every one of the 8 captures of each state is a different file. A limit stated and not a hole: the two `_NS`
states exist for it, the same gestures with the sweep off, and there the PPI disc is 0 in every pair and the eight
captures of each state are ONE file (ca79eb0f... for ON, 40316aa7... for OFF), so the disc's rings, ticks, corner labels
(including the position "51.5N 000.5W"), the receiver mark and the scan lines are held at 0 against the control. The stderr
of the two builds is identical in all 48 control-and-port pairs, the stdout identical once the teardown time and the worst
frame gap are masked, and the config each run saved on exit identical in all 48 (the telemetry install id, a per-run random
id, apart). Because that is a table of zeros, the comparator's own checks that it can see: control RADAR_ON against control
RADAR_OFF reports 4 960 pixels (5 750 at tolerance 0) in the top deck (START and STOP, the volume meter), 38 901 (40 938) in
the SIG column, 5 634 (10 333) in the POWER plate and 569 (28 180) in the PPI disc (the sweep), and 0 in the info panel
and the other plates; control DEMOD_AUDIO
against control DEMOD_SPEC reports 3 772 (4 200) in the keys row, 1 920 (2 250) in TIME, 1 642 (1 996) in GAIN and 1 296
(1 530), 481 (569) and 355 (441) in the three label pieces, and 0 in the DISPLAY row; control WFM against control AM reports
72 199 (94 124) in the rail and 2 100 (2 100) in the keys row (the fifth key gone); and a +30 and a +10 poke on single pixels
are counted as 1 pixel at tolerance 24 and 2 at tolerance 0. THE THIRD-DIFFERENCE RULE found nothing to investigate: no
pixel outside the live areas differs in any pair. THE ONE LAG THE PICTURE COULD HOLD, tried: the POWER key shot frame by
frame (the sweep off, the pointer on the key from frame 70, the button down at 74 and up at 78, frames 72 to 86; control
and port, two runs each). The picture changes at frame 75 (the key pressed) and settles from frame 80, and in every frame
the control's run and the port's run of the same number are the SAME FILE: 0 pixels differ in all 13 frames. Run 1 and run 2
differ from each other in frames 76 to 79 (38 256 pixels at 76, 105 at 77 and 78, 3 253 at 79, in the top deck's meters and
the SIG column settling after the start; run 1 is the same file in both builds and so is run 2), so this is a lag the
pass does not show today
(it still runs at the head of the frame), and difference 1 is held by test_scope_window B and C and by the code, not by a
picture. What the capture does NOT draw, and so does not hold: a radio's knob (the generator lists no gain stage, so the
dial is the dead "N/A, no device" figure at its fallback travel 0 to 60 and the live knob, a detent, the lit tick, "AUTO"
and the stage's own unit are held by test_scope_window C only: no radio is visible to the process), any aircraft (the ALT
gauge, the TRACKS drums above 000, the plotted count, the selected altitude and the range drum: no track-source module is
fitted, `hostTracks()` is held by code reading), the rail's "scope will be empty" block (the rail's RADAR section is not in
frame), a position being set (difference 1's empty-state frame), a WFM-gate click, a detent mid-hold, the capacity clamp
(unreachable in this tree), the playing-plugin label (no plugin is playing), and any time base other than the 10 ms/DIV
default. The real config (%APPDATA%\foxsdr\config.json, 5 076 bytes, written at 14:52:27 UTC by another session, sha256
BA532E6C...A353C) was identical before the first launch and after the last (size, modification time and hash), and none of the
scratch values (the seeded position) is in it.

What could move a measured number: the three info-only `read_scope` calls a frame (about 89 ns each, arithmetic, not
measured in situ); the demod page no longer takes the runner's lock or builds a string (unmeasured before the port, code
reading only); the I/Q copy of up to 262144 complex frames (2 MB) a frame at 50 ms/DIV on a wide channel is a cost that
exists today and is unchanged; `capacityFrames` is one 32-bit store in each `read_scope` and the state publish copies 16
bytes more and compares two doubles, so a position that CHANGES moves `seq` and re-publishes the state (a GPS receiver
feeding fixes does so at the fix rate; by design and not measured), and does not rebuild any list; the knob's local copy is
a string compare per frame while it stands, at most 0.5 s.

THE TRAPS, for the next group.
1. The generator starts STOPPED in a bounded run: a capture of a live scope presses the master START (100,112 in a 1500x1380
   window) or the radar's POWER key (1225,1236) from the input script, and captures at least 50 frames later.
2. The radar's sweep is wall-clock driven: a capture of the PPI with the sweep on is never deterministic. Switch it off through
   the SYS tab (the tab at 1290,297, the SWEEP row at 1170,400) before the capture, the scope's own options and no seam.
3. A frozen demod trace cannot be captured by stopping the receiver: `written` stops, the liveness rule (0.25 s) calls the
   tube stale and it says NO SAMPLES. The generator's spectrum and multiplex are blank at the default settings; a test that
   wants a trace drives a tone.
4. Both scopes' open flags are cleared at start-up (`startupState`): nothing in a config opens them. FOXSDR_OPEN_RADAR_SCOPE is
   bounded runs only; FOXSDR_OPEN_DEMOD_SCOPE is not.
5. `read_scope` returns an `int32_t` that can be negative: `size_t got = read_scope(...)` turns -1 into an overrun in
   `got - sweep` (the mutant that did exactly that is the one that died by crash). `info.structSize` must be set or the whole
   call fails. The I/Q `out` is a `float*` with `cap` in FRAMES and no out-size argument: size `scopeIqBuf_` for `want` frames
   first.
6. tests/test_stop_ends_recordings.cpp:1095 is a SOURCE-TEXT test that requires `drawScopeMode` itself to name
   `FOXAPI_OP_RUN`: the POWER press cannot move into a helper member.
7. The radar's cabinet uploads its badge on first use (a GL call and no context in a test): hand the window a texture id it
   never uploads and take it back before the window goes; under 900 pixels wide the scope draws bare and the knob and POWER
   are not there.
8. No hermetic frame can put a track in `pluginUi_` (it is filled only by `poll()` from a loaded track-source module, and
   neither fixture plugin is one): the aircraft count, the selected altitude and the range drum need a third fixture
   module to be covered.
9. Not touched, pre-existing: `scopeLive_` is set only while the demod page is drawn and read by the rail key's lamp, so it
   keeps its last value with the page closed; `roomForFace`'s first condition (`scale >= 0.42f`) is always true after the
   clamp two lines above it; when the I/Q sweep is longer than the ring the sweep is shortened to the ring while the
   time-base readout still names the selected ms/DIV.
10. A test frame is LOOK, DRAW, then `pumpWorkers` and `pumpAudio` (group 10's trap), and a ported member reads the window's
    cache and session, so a call into it after an engine mutation needs the LOOK first. The window files and this one are
    CRLF and Git Bash `sed -i` strips the CR (groups 10 and 11's trap, again).
11. UNVERIFIED, carried forward: no real radio, SoapySDR or sound hardware was driven; the live aircraft path (count,
    altitude, range drum) is held by code; the capacity clamp's real effect is unreachable in this tree and proven only
    through a wrapped table; the lag of difference 1 as a picture (it does not exist until the pump moves); the cost of the
    deleted runner lock.

GROUP 13, the Patch page (the standard window's default face: drawPatchSection, drawPatchView, drawPatchFaces and drawPatchPage; the
draft flow, adoptPatchGraph, syncPatchDraft, commitPatchDraft, loadPatchDocument, seedPatchIfNeeded, settlePatchSend and the new
`patchLook`; and src/gui/app_window_patch_radios.cpp, the transport, the Radio and speaker inspectors, the squelch, the Map inspector
and the device rule; the window gained `patchLists()`, `hostPaths()`, the stop-and-wait committer and the two-step Radio part, and
lost `EngineHost::patchPageOpen`, `onPatchGraphChanged` and `onPatchPicture`): API 0.3 section 8.12 in THREE parts (8.12.1 to 8.12.3,
8.12.4 to 8.12.6, 8.12.7), the biggest group of step 4 by a distance: one op for the page's open flag, one op for the recordings, one
list for the plan, one for the devices, one for the live readings, three facts for the graph's identity, a family of facts for the
devices and converters, a merge-safe SET_GRAPH, `read_image` with its event and one more kind of the live list, and the engine's whole
patch runtime moved from the window's frame into the pass. Nothing committed; three parts, one api_03 test repair and this record, in
the worktree. The survey, the three reports and the repair were written one after another and are folded here.

THE SURVEY AND THE SCOPE JUDGEMENT. The window's own survey, written before the first edit (and run, as groups 9 to 12 were, on a
scratch copy of src with the real guard binary), found where the brief and the code disagreed, in this order of consequence: (1) THE
CLUSTER IS IN TWO FILES. app_window.cpp carries 90 counted reads (drawPatchView 45, drawPatchFaces 22, drawPatchPage 9, syncPatchDraft 4,
adoptPatchGraph 4, commitPatchDraft 3, seedPatchIfNeeded 1, drawPatchSection 2, loadPatchDocument 0); the page's other half,
app_window_patch_radios.cpp (590 lines), carries 36 more in eight members and ALL THREE `submitCommand` lines that count towards the
guard's floor. The page is 126 reads on 33 fields, not the brief's ~94, and 22 of those fields lose their last reader. (2) MOST OF THE
GROUP IS NOT READS. The page is where the window still DROVE the engine's patch runtime from its frame: drawPatchView called
`rebuildPatchCatalogue()`, `patchApplyRunning()`, `patchReconcile()` and `patchPublishSets()` and wrote `engine_.patchPlan_` and
`engine_.patchWasOpen_`, and drawPatchPage ran the close-stops-the-patch rule: 16 `kLineAllowed` entries and about a dozen uncounted
calls, work that section 6.5's pattern says moves into the pass and that had not. The consequence nobody had written down: PATCH_RADIOS,
PATCH_PROBLEMS, PATCH_PARTS, `read_node_spectrum` and the `running` bit of the patch event only changed while a window was DRAWING the
patch view. That prerequisite is the group's real size. (3) "NOTHING READS A DOCUMENT BACK" was stale on both halves: the graph was
readable (`fact.patch.graph`, PATCH_NODES, PATCH_WIRES, FOXAPI_EVENT_PATCH), and FOXAPP_OP_PATCH_SET_GRAPH, an APP op, is the writer,
so a REMOTE session can read the graph and cannot edit it; `FOXAPI_OP_PATCH_LOAD` is in the op table and not implemented by this engine
at all (it falls to UNSUPPORTED). (4) THE WINDOW ALREADY EDITED THROUGH SET_GRAPH, BUT NOT THROUGH ITS SESSION: commitPatchDraft and
loadPatchDocument called `engine_.applyCommand` directly, synchronous, same frame, result read at once. The vocabulary was ready; the
TIMING was the whole problem (a lost update: a window-side merge over the engine's graph "as it is now", then a replace, is correct only
while merge and apply are one step). (5) "DECODED PICTURES" WERE ZERO GUARD READS AND WERE NOT SERVED AT ALL: `read_image` was a stub
returning UNSUPPORTED, FOXAPI_CAP_IMAGES was clear, nothing raised FOXAPI_EVENT_DECODER_IMAGE, and the window's `patchPictures_` was
filled by an EngineHost HOOK, a cross-thread write the day the pump moves. (6) `pipeline_.patchRunner()` was vestigial (each patch
radio has owned its runner since 0.99.17; the page called `.reap()` and `.clear()` on one nothing published to). (7) the ~60-read
"device choices" were mostly uncounted method calls. (8) Four lists were weaker than their descriptions (PATCH_RADIOS lacked the
coerced-rate note, PATCH_PARTS the track bit, PATCH_PROBLEMS a published code table, and no list carried the Plan). (9)
`Plan::runnable` was not derivable from the problems: the window pushed `PluginRefused` problems WITHOUT clearing `runnable` and
`RadioFailed` WITH clearing it, so the green "This patch can run." stayed up with a refused plugin. THE JUDGEMENT: counting the
work and not the reads, the group is closer to groups 10 and 11 together than to group 12, so it was SPLIT three ways on the
dependency order and on where the risk sits: 13a, THE STRUCTURAL AND THE HARD HALF (the runtime into the pass, the graph's version
and a merging SET_GRAPH, the plan, and the draft flow with its stop-and-wait committer: the part that changes WHEN things happen and
therefore the part that can lose an edit); 13b, THE READINGS (the devices, the live figures, the line routing, the recordings, and the
view, the faces and the inspectors that draw them: wide, mechanical once 13a stands, and the part that holds most of the guard's
movement); 13c, THE PICTURES (a delivery gap and not a read gap, with its own protocol, its own fixture and its own threading
question, separable because nothing in 13a or 13b waits on it and the hook it removes was the last of the page's three cross-thread
hooks).
The survey MEASURED the ratchet on a scratch copy before the first edit: the whole page, both files, the Map's tracks and paths
through two accessors, was predicted at 34 fields / 144 reads, floor 8, 22 fields leaving, 16 allowed lines deleted (46 to 30), and
the split alternatives (the draft flow alone 55/255; app_window.cpp's members 45/179 with the floor staying 11). The three parts
printed 49/230 (13a), 33/141 (13b) and 33/141 (13c): one field and three reads under the prediction, the `patchGraph_` reads in `run`
and `applyConfig` that 13a moved too and the survey had not counted; 23 fields left, not 22.

13a, THE RUNTIME IN THE PASS, THE GRAPH'S VERSION, THE PLAN AND THE DRAFT FLOW (docs/api-0.3-additions.md 8.12.1 to 8.12.3). Server side:
- 8.12.1 THE RUNTIME IS THE PASS'S. `FOXAPP_OP_SET_PATCH_PAGE_OPEN` (0x8421, VIEW, LOCAL, `ival[0]` 0 or 1, idempotent; the engine's
  default is OPEN, matching the default face, and the window tells it only on a change). `Engine::pumpPatch()` sits between
  `pumpPlugins` and `pumpPublish` in `controlPass`, `pump` and `drawUi`; while the flag is set it rebuilds the catalogue, applies START
  and STOP, reconciles the radios, compiles the plan (two marks) and publishes the sets, in the order the view ran them; on a set-to-clear
  edge it runs the close rule once. `EngineHost::patchPageOpen` and `onPatchGraphChanged` are deleted from the host and from the guard's
  `kHostHooks`; `pipeline_.patchRunner().reap()` and `.clear()` were confirmed vestigial and deleted with their `kLineAllowed` entries;
- 8.12.2 THE VERSION / EPOCH / MERGE PROTOCOL, exactly. `patchGraphVersion_` is a counter from 1 (0 means none) that bumps on EVERY
  change of the graph by anyone: a session's edit, a document, and the runtime's own (START and ALL OFF's switches, a learnt centre or
  rate, a take-over of the receiver's device); the change is noticed by `syncPatchGraphVersion`, run at the start and the end of
  `pumpPatch`, before every publish and at a SET_GRAPH's apply, and a settled pass allocates nothing. The engine RETAINS THE LAST EIGHT
  versions' graphs (a document clears them). `fact.patch.version`, `fact.patch.epoch` and `fact.patch.graph` are built in ONE block, so
  they move together with `listSeq`, and the version is in the fingerprint. `FOXAPP_OP_PATCH_SET_GRAPH` takes the BASE VERSION in
  `ival[1]`: 0 is an unconditional replace; the CURRENT version is applied as sent; a RETAINED older one is MERGED, by
  `rebaseDraft(retained, draft, current)`, and answers `FOXAPI_RESULT_CLAMPED` with `applied[1] = 1`; a version the engine does not
  hold, or one from the future, is `FOXAPI_FAILED` ("the patch graph moved on since this edit was started; it was not applied"); a
  negative one is BAD_ARGUMENT; a DOCUMENT ignores the base, bumps the epoch and the version and clears the history. `applied[0]` is the
  version after the command. FOXAPI_EVENT_PATCH now triggers on the version moving. THE LOST-UPDATE PROOF (G13-2): with the base named,
  a START survives a stale rename of the same pass; sent unconditionally, the START is LOST (the stale text puts the radios' switches back
  OFF and the patch is "RUNNING" with every radio off until the next START); two racing sessions keep BOTH edits when each names its base.
  The lost update is real only for an ASYNCHRONOUS window, which is why it had never shown;
- 8.12.3 THE PLAN. `PATCH_PLAN` (45): channel entries (id = node, detail "channel", `value[0]` the offset in Hz, `value[1]` the output
  rate, `value[2]` the decimation, `value[3]` the radio) then decoder entries (name = the plugin's key, detail "decoder:" and then
  "radio", "channel" or "audio", `value[0]` the rate it was created at, `value[1]` the source rate, `value[2]` the RF centre at DC,
  `value[3]` the radio). `fact.patch.runnable` copies the window's quirk EXACTLY (a failed radio clears it, a refused plugin does not;
  whether to change that is the owner's call and is recorded in the section). `PATCH_RADIOS` gains `value[1]`, the open radio's AIR
  centre, `value[2]`, 1 while it is opening on its worker, and `detail`, the running radio's line (the coerced-rate or retune note);
  `PATCH_PROBLEMS` codes 0 to 14 are pinned (advisory: 5 and 14); the parts bin is hashed by content;
- THE BUG FOUND ON THE WAY, fixed red first: `FOXAPP_OP_SCAN_NATIVE_ONLY` was never in the op table, so a SESSION submit of it was
  refused as unsupported and only a direct `applyCommand` worked; test_main_view caught it once the port sent the Radio part's scan
  through a session, and the `test_apply_command` case that holds it (the op is in the table) was written to fail before the
  one-line table entry went in. It is a PRE-EXISTING bug that nothing exercised because nothing but a direct call used the op.
Window side (the part that changes the draft flow, and the reason for the split):
- `patchLook()` reads the version, the epoch and the graph from ONE publish (the version re-read after the graph, to detect a
  straddle), re-parses only on movement and is invalid before the first publish; `syncPatchDraft` never adopts until a look is valid;
- THE ADOPT FIX. Today's trailing `adoptPatchGraph()` after a commit copied the engine's graph over the draft; from a PUBLISHED graph,
  still the old one, that is a snap-back (three commits a frame while a node is dragged would draw the node at its old place and then
  its new one). `commitPatchDraft` now sends the WHOLE draft by `submit_text` with the base in `ival[1]`, keeps the ticket, and is
  STOP-AND-WAIT: one in flight, a BUSY or a missing session leaves the edit and retries next frame, nothing is adopted while one is in
  flight, the answer is drained in `pumpTransmitKey`, and on OK, once the publish at the new version is read, `settlePatchSend` sets the
  draft to `rebaseDraft(sent, draft-now, engine graph)` and the base to the engine's graph (a node being dragged never snaps back); a
  REFUSAL adopts the engine's graph. The three commits a frame collapse to at most one SET_GRAPH in flight;
- THE FRAME-1 SEED. The very first `commitPatchDraft` is at the TOP of frame 1, before the first publish, so a graph read there is
  empty and adopting it would make the draft "in step" with an empty graph while the config's document is queued, and the seed's commit
  would overwrite the loaded patch (test I guarded the synchronous version only). The seed now waits for a valid look and for any document
  in flight, and `applyConfig` sets `patchSeeded_` from the document it sent;
- `loadPatchDocument` is asynchronous: it drops a drag, the selection and the waiting Radio parts at once, shows the document
  immediately and adopts when settled. The page-open flag goes through `syncPatchPageOpen` (difference-only, initially open).
  `patchAddRadioPart` is TWO steps: the node is added at once with NO device (so the press shows), the scan goes through the session,
  and the device is chosen in `finishPatchRadioParts` in PRESS ORDER, `taken()` running over the draft so a second Radio press does not
  take the first's device. `drawPatchPage` is deleted; `patchText_` is serialised from the draft; "dirty" is marked on an
  engine-originated version movement except when the page adopts its own document.
Ratchet: 56 fields / 269 reads to 49 / 230, printed `261 controls judged, 11 submitCommand calls in draw members, 658 window-machinery
lines, 31 allowed engine lines in drawing members, 0 violations` and `the window reads 49 engine fields directly, 230 times (budget 49
fields, 230 reads)`, 196 checks 0 failed. Per member: adoptPatchGraph 4 to 0, commitPatchDraft 3 to 0, syncPatchDraft 4 to 0,
drawPatchSection 2 to 0, drawPatchPage 9 to 0, drawPatchView 45 to 31, applyConfig 7 to 6, run 18 to 17. Seven fields lost their last
reader (`patchDspSig_`, `patchGraphEpoch_`, `patchGraph_` - `run`'s end-of-script printout now serialises `patchDraft_` -,
`patchRadioFailedAs_`, `patchWasOpen_`, `patchWasRunning_`, `soundCardOpenPending_`). THE 16 `kLineAllowed` ENTRIES THE SURVEY LISTED
WERE ALL DELETED (the guard fails on every stale one) and ONE ADDED, `drawUi`'s `pumpPatch();`, so 46 allowed lines became 31 (the
survey predicted 30 without the addition). `kHostHooks` lost two; `kPortedMembers` gained 15 (drawPatchSection, setMainViewPatch,
syncPatchPageOpen, uiSubmitTicket, patchLook, syncPatchDraft, commitPatchDraft, loadPatchDocument, settlePatchSend, onPatchGraphResult,
adoptPatchLook, forgetPatchInteraction, patchInteracting, finishPatchRadioParts, patchAddRadioPart). Two members were NOT listed and
say why in the guard: `seedPatchIfNeeded` (it still read `device_` and `carriedAirCentre()`) and `patchDefaultDeviceKey` (13b's).
The floor `submitsInDraw >= 11` did not move (the transport's two submits and the Radio inspector's one were 13b's).

13b, THE READINGS (8.12.4 to 8.12.6). Server side: `PATCH_PARTS` `value[2]` is the track bit; NEW `PATCH_DEVICES` (46), the Radio's Device
combo as the engine composes it (`Engine::patchDeviceChoices`, the one rule); NEW `PATCH_LIVE` (47), the figures a running patch changes
every pass (a squelch gate, a speaker's peak and count and error, a channel's level, a decoder's face), IN A BLOCK OF ITS OWN with its
own fingerprint that NEVER MOVES `listSeq` - the group-9/D3 lesson applied: carried in the block of lists the figures would rebuild the
block, its facts and its plugin sizes every pass and make every interface re-read every list every pass; a settled pass builds nothing
and allocates nothing, and the window reads it once a frame. Where 13b kept to the brief and where it did not: PATCH_LIVE is NOT in the
main fingerprint (above); the converter fact is the SPACE form `"<mode> <LO Hz> <inverted>"` that `fact.converter.live` spells (the
survey had commas); `fact.patch.receiverCentre`'s existence moves the block and its value does not (below); and a decoder's newest line
is in `detail` (159 bytes) and not in `name` (95). `DECODER_LINES` `value[0]` is the ORIGINATING NODE (0 for the receiver's own decoders), so a window that
keeps a face for each Text out routes a line to every Text out its decoder is wired to by its own copy of the graph. New facts:
`fact.patch.device.<id>` (the key whole; LOCAL), `fact.patch.receiverRadio`, `fact.patch.receiverCentre` (its EXISTENCE moves the block,
its value does not, because a retune would rebuild the block on every tune; the window uses the state's `centreHz` once the fact is
non-empty) and `fact.patch.conv.<node>`, the converter in force for a Radio node's device. THE PER-NODE FACTS ARE PER NODE ON PURPOSE:
a typed centre is judged by `core::radioCentreTakeable` against the converter of the device the ENGINE has the node on. NEW
`FOXAPP_OP_PATCH_LIST_RECORDINGS` (0x8422, SETTINGS, LOCAL; a VIEW-only and a REMOTE session are DENIED): the engine lists the I/Q
recordings on its own pass (at most 200 listed, at most 64 headers opened by one listing), and the recordings are hashed BY CONTENT
(each file, its key and its rate) and not by count. The fingerprint also gained the lent radio's four fields and whether the receiver's
radio was ever tuned (not its value). THE SHARED LABEL RULE: `core/patch_device_label.hpp` holds `deviceLabelFor` and `decoderIsShown`
moved into patch_plan.hpp, so the engine and the window name a node's device and decide a decoder's face by one function. The engine
pruned its own Text-out lines each pass in 13b (dead state, retired in 13c).
Window side: drawPatchView, drawPatchFaces, seedPatchIfNeeded, patchDefaultDeviceKey and the whole radios file are ported; no `engine_`
read remains in the page except `hostTracks()` and the new `hostPaths()` (the Map's tracks and paths stay HOST-PROCESS, as group 12
decided for the radar, and the two accessors concentrate that in two places that are NOT in `kPortedMembers`). The transport's START
and ALL OFF, the Radio inspector's two former `applyCommand` calls and "Look for radios" go through `uiSubmit`. A new cache reader,
`patchLists()`, builds once per `listSeq` the parts, the plan, the radios, the refused nodes, the devices, the receiver's radio and each
Radio node's converter; PATCH_LIVE is read once a frame and `read_node_spectrum` at most once per radio per frame, with a negative
return never a length (group 12's trap 5). Four unused `Engine::` type aliases left the window header.
Ratchet: 49 / 230 to 33 / 141, printed `269 controls judged, 8 submitCommand calls in draw members, 658 window-machinery lines, 31
allowed engine lines in drawing members, 0 violations` and `the window reads 33 engine fields directly, 141 times (budget 33 fields,
141 reads)`, 212 checks 0 failed; the floor `submitsInDraw >= 11` is `>= 8` (the eight that remain are group 14's and the position
helpers'); `kControlMayCall` retired `carriedAirCentre`, `patchListRecordings` and `prunePatchSinkLines`; `kPortedMembers` gained 19.
The guard went red on a single read put back: `(void)engine_.patchPlan_.runnable;` in drawPatchFaces printed `FAIL: AppWindow::
drawPatchFaces is PORTED ... reaches the engine directly at app_window.cpp:10053`, 34 / 142 against the budget, 3 failed; restored,
33 / 141.

13c, THE PICTURES (8.12.7). `read_image(session, imageId, info, pixels, cap)` copies up to `cap` bytes and returns the full byte count,
`info` and `pixels` each optional; a `cap` with no pixels, an `info` shorter than FOXAPI_MIN_IMAGE_INFO or a null session is
BAD_ARGUMENT, and id 0, an unknown or replaced id or a removed node is NOT_FOUND; the pixels are tightly packed GRAY8 or RGB24 with no
palette. FOXAPI_CAP_IMAGES is set (FOXAPI_CAP_DECODERS stays clear: text events are unserved). `FoxImageInfo` gained `complete`, a
reserved word and `sequence` at its END (append-only: a caller built to the 0.2 minimum gets the 0.2 fields only). THE ROW OVER THE
EVENT: `FOXAPI_EVENT_DECODER_IMAGE` (6, the 0.2 id; target the node, `value[0]` the image id, `value[1]` 1, text the node's name) is
raised once per arrival, AFTER the picture is readable, but THE WINDOW POLLS `PATCH_LIVE` kind 6 (id the node, `value[0]` the newest
image id, `value[1]` the byte count, name the node's name, ACTIVE = complete), not the event, because a row is state: a session that
attaches late, or whose event ring overflowed, still sees every picture, and the window's correctness does not depend on a channel it
could miss (the same decision as the patch event, where version, epoch and graph from one block replaced the event for the window).
The row and the picture live while the node is in the graph, so a STOPPED patch keeps its picture. REMOTE is the same as LOCAL with the
VIEW grant only (decoded traffic like DECODER_LINES; it names no path or device). THE STORE THREADING: the pass moves each picture whole
into a `const HostImage` behind a `shared_ptr` and swaps an immutable SET into the host; a reader takes the set pointer under the short
lock and copies pixels on its own thread, so a replaced picture is still whole in the reader's hand and the window never touches a
buffer a decoder writes. THE HOOK IS DELETED: `EngineHost::onPatchPicture`, the window's override and its `kHostHooks` entry; with it
the third and last of the three EngineHost hooks the survey found touching plain window state across threads (13a took the other
two). `patchSinkLines_`, with its writer loop in `pumpDecoderOutput` and
`prunePatchSinkLines`, is RETIRED (write-only: the Text outs' lines come from DECODER_LINES and the window's copy; the old prune test
became the picture-store equivalent); `patchDecoderFaces_` stays (PATCH_LIVE kind 4 reads it, and it is in that block's fingerprint).
The window's `patchPictures_` holds only a texture and its figures, no pixels; a Decoder face reads kind 6 and, when the id moved,
`takePatchPicture` reads and uploads ON THE WINDOW'S THREAD (NOT_FOUND or a malformed reply leaves the texture and retries next frame; one
upload per picture, as before); a document load now clears every picture. Guard: `272 controls judged, 8 submitCommand calls in draw
members, 654 window-machinery lines, 31 allowed engine lines in drawing members, 0 violations`, 33 / 141 unchanged (`patchPictures_` was
never an engine field and the hook write was never counted; the gain is a hook leaving `kHostHooks`), `takePatchPicture` added to
`kPortedMembers` and red on a read put back (`FAIL: ... takePatchPicture is PORTED ... at app_window.cpp:10012`).
THE MUTATION-HARNESS MISHAP, in the record because it was close: the 13c mutation harness was launched twice, the two instances briefly
overwrote each other's restore copies and left the E1 and E7 edits in engine_api_lists.cpp and fox_engine_api.cpp. It was caught by
reading the diff and repaired by hand; every mutant's original string was then verified to occur EXACTLY ONCE in its file, the tree was
rebuilt and the serial suite was green after the repair. (13b recorded the same trap in its own list: a mutant batch is never started
twice, and a restore from a snapshot is how it was undone there.) The mutant harness is not a place to be clever with a second terminal.

THE API_03 LOAD RACE (the repair that closes the part; only tests/test_engine_api_03.cpp changed, the engine did not). After 13c the
suite was green serially and test_engine_api_03 FAILED under `ctest -j 8`: group Y's `playing.audioSeq == quiet.audioSeq + 1u (7 vs
6)`, and other groups as "seq audio rds" against "seq rds". THE DIAGNOSIS, proved for the transitions and unproven for the trigger: the
extra transition is the audio OUTPUT STREAM DYING and being REOPENED, not a late open. `AudioOut::streamAlive()` sees
`Pa_IsStreamActive == 0` on the real Realtek USB device, STREAM_ALIVE (bit 4) falls (one `audioSeq` step), and `Engine::pollAudioHealth()`
(a 1 Hz watchdog) reopens it - in traced failing runs the first reopen failed once and succeeded about a second later - and STREAM_ALIVE
returns (a second step). The old `settled()` waited only for the seq to be still for 300 ms, and the watchdog's next probe can be a
second away, so baselines straddled the recovery (14 flags against 10, 7 against 6 and 9 against 8 seqs). THE ENGINE IS NOT AT FAULT: it
reports a dead stream truthfully and recovers it. WHY the stream dies is UNPROVEN (nothing in-process stops it; no device event in the
event log; the rate varied over the session; external contention on the device is the likely reading). THE FIX: `audioSteady()` (no
open in flight, a sink that has ever opened is alive, STREAM_ALIVE agrees with RX_SINK_OPEN), `settled()` additionally requires
`audioSteady()` and unchanged audio flags for the whole 300 ms window (bounded at about 40 s, which fixes every `settled()` caller, groups Y
and R and others), and `steadyState()` is a free wait for baselines taken straight off the session's state, applied to the two RDS-loop
baselines; no `== base + 1` or `countersMoved` check was weakened. THE DETERMINISTIC BREAK-IT: a new test, `testBaselineWaitsForTheSink`,
kills the stream with `test.audioClose`, asks for the baseline while it is down and checks the one-command pair reads exactly +1; with
the OLD `settled()` restored it FAILS 3 of 3 with `on.audioFlags == off.audioFlags (6 vs 2)` and `on.audioSeq == off.audioSeq + 1u (4 vs
3)`, the exact shape of the load failure on demand, and passes with the fix. The 13c hammer test does NOT run alongside the audio checks
(sequential in one process), so it was not moved; the direct-engine tests were left alone (their own passes; the baseline-to-probe gap is
milliseconds). After the repair: api_03 solo 3 of 3 green (3582 checks, about 72 s); the full suite at `-j 8` run 1: 256/256; run 2:
255/256, test_theme_census failed in the first seconds of the run and passed solo 3 of 3; run 3: 255/256, test_startup_state, solo 3 of
3; and the GUI subset (api_03 and the four big window tests, `-j 5`) green twice. THE RESIDUAL, recorded as ENVIRONMENTAL and not
diagnosed: four tests fail under load and pass alone - test_theme_census (group 11 recorded one crash of it inside `NahimicOSD.dll`,
an injected audio-overlay DLL, 0xC0000005; the cause of this run's failure was NOT examined), test_startup_state, and, once or twice each
during the investigation, test_crash_upload and test_pa_stream_list_guard (neither re-run after, neither investigated). Full-suite runs are
weak evidence of a flake fix on their own (the pre-fix rate swung from 0 of 9 to most runs); the deterministic red-then-green is the
proof, and a stream death landing in the few-millisecond window between a baseline and its probe can still fail one check (widening the
wait further would weaken the contract). Logs: ctest-fix-before.log and ctest-fix-after1.log to ctest-fix-after3.log in the worktree.

THE TESTS AND THE BREAK-IT CHECKS, per part (every case went into an EXISTING executable, so the suite stayed at 256 tests; the new
files are tests/fixture_group13_plugin.cpp, a real picture decoder module, and its CMake line). 13a: test_engine_api_03 G13-1 (the
pass runs the patch with no window: the plan, START, the centre, a node's spectrum, the page edges, the grants, the default), G13-2
(version, epoch, the merge, the refusals, the eight-version edge, one-block consistency, the event), G13-3 (the plan against the core's
compile with two real modules; the radios' note and pending; the two marks), G13-4 (30 settled passes move nothing); alloc F7;
test_apply_command (SET_PATCH_PAGE_OPEN, SCAN_NATIVE_ONLY, the version cases); the window scenarios L (a drag across a stalled pass), M
(the first-frame seed, three ways), N (a stale commit and a START), O and O2 (the page-open edges) and B2 (BUSY), with LOOKs added to
test_patch_graph_draft_input, test_patch_add_radio_part and test_converter_app_paths' `seedPatch` accessor. Mutants, each applied alone
and each RED: the engine's (page default closed; the close rule; the op ignored; no merge; history 7; a document keeps the history; the
engine's own changes not versioned; epoch equal to version; the refused-plugin and failed-radio runnable rules; the decoder source
mapping swapped; the channel's offset and out-rate swapped; the radios' centre equal to the rate; the note dropped; pending dropped;
the event ignoring the version; sync-before-block removed; every fingerprint input alone in F7) and the window's (W2 base not named, W3
an unpublished graph adopted, W5 and W6 the flag never sent and sent every frame, W7 a device before the scan's answer, W8 the answer
adopted whole, W9 a refusal unanswered, W10 a document keeping the interaction, W11 a second commit in flight, W13 applyConfig silent,
W14 BUSY dropping the edit). SURVIVORS: W1 (adopt while in flight) and W4 (the seed ignoring a document in flight) are equivalent by
construction; W12 (no second version read) matters only with a concurrent publish, UNVERIFIED and kept as defence. One fingerprint input
is not mutant-tested alone: an open radio's centre (no hermetic way to move it without the graph moving), tested by value. 13b:
api_03 G13b-1 to G13b-4 (the devices and the recordings op; the receiver's radio and the converter; PATCH_LIVE against a running
generator patch; the decoder face with DECODER_LINES' node ids against the real group-8 fixture), alloc F8, apply_command's new op,
the window scenarios Q, R, S, T, V and X of test_patch_graph_draft_input (text read through ImGui's log), add_radio_part's device
order and the lent radio, and converter_app_paths' two seed cases and the published-node `look()`; 35 mutants, all red but two
equivalents (W4: a node-0 guard no wire can match; E7 and E14 went red once the missing alloc cases were added; evidence in the
session's scratch under w13). Not covered: the Map's track bit at the window (no track-source fixture) and the engine's then-dead
`patchSinkLines_` prune (retired by 13c). 13c: api_03 G13c-1 to G13c-3 against the REAL picture-decoder fixture in the pass with no
window (the event readable the moment it is taken; ids rising; the node's name in the event; the buffer protocol, a stale id, a removed
node; VIEW-only, REMOTE and a late LOCAL session; a stopped patch keeping its picture; an edit against a document load; `listSeq`
unchanged; two reader threads against the pass), alloc F9, draft_input scenario W (a GL stand-in: one upload per picture shown,
NOT_FOUND and a bad shape leave the texture, a document load and a node's removal delete it, the hook gone by a source check),
apply_command and the guard; 25 mutants, 24 red first or corrected, and the survivor (PATCH_LIVE kind 6's ACTIVE always true) was
closed by a new per-pass check and is red. Among the engine reds: no event, ids reused, no prune on removal, no clear on a document
load, the set handed over a pass late, info-only returning 0, a short buffer returning the copied count, REMOTE denied, the
capability clear, the row's byte count 0, the caption empty, the event's `value[1]` wrong, STOP clearing pictures, the cap-without-buffer
check dropped, the info-size check dropped, the appended fields unguarded, the pixels copied and not moved, and an IN-PLACE OVERWRITE
(5 torn reads); the window's: upload every frame, NOT_FOUND marked shown, the shape check dropped, the row prune dropped, the hook
re-added. THE FINAL NUMBERS, solo, on this tree after the api_03 repair and before this record: test_engine_api_03 3 582 checks, 0
failed (72.55 s); test_engine_publish_alloc 466/0; test_patch_graph_draft_input 383/0; test_apply_command 1 231/0;
test_converter_app_paths 336/0; test_patch_add_radio_part 25/0; test_command_path_guard 212/0 (below). The last serial `ctest -C
Release`, 256/256, was after 13c (1 078 s, ctest-after-g13c-serial.log); the `-j 8` runs after the repair are above.

THE ALLOCATION NUMBERS, per part. 13a: a SETTLED pass allocates nothing (`syncPatchGraphVersion` against the same graph; 30 settled
passes move nothing, G13-4), and every input of the fingerprint moves it alone (alloc case F7, 404 checks); the patch step's cost per
pass was by design and was NOT measured. 13b: 200 unchanged `patchLists()` refreshes allocate 0, a settled pass with PATCH_LIVE builds
nothing and allocates 0 (F8, 448 checks), and a RUNNING page's whole frame averages 43 heap allocations (a speaker and a Spectrum
part); the old draw was not measured, by code reading it built the ids vector, `patchReadings_`, a `compile()` plan and
`patchDeviceChoices()` strings for every Radio face and copied two graphs. 13c: a settled pass and 1000 reads of the row, the info and the
pixels allocate 0 (F9, now 466 checks), a NEW picture costs 5 to 10 allocations and under 1 KB in the publish for a 1.4 MB picture
(the pixels move, they are not copied), and two reader threads of about 2.9 million reads each ran against the pass with 0 torn.

THE OPERATOR-VISIBLE DIFFERENCES, by the code and the tests; the capture below holds the ones it says it holds. FROM 13a: (1) the plan's
marks, "This patch can run.", "fed N Hz" and a radio's rate and centre trail an edit by a pass or two, and a node added from the bin is
unmarked for a frame; (2) a new Radio part shows "Choose a device on the right." for about two frames before its device is chosen; (3)
the engine is one frame behind the draft (test P's allowance is 5 per drag frame, it was 3; idle frames 0); (4) the scripted run's
end-of-script printout and the config's patch text come from the page's graph; (5) START pressed while the view is closed is still
honoured when it reopens; (6) a rare open race: an unsent Radio part with no device could be given the lent radio the take-over names
(the later device edit wins the merge). FROM 13b (six: the report's five, its first split in two): (1) the gate, level, speaker and decoder-face readouts are one pass later;
(2) a Radio's rate and centre and START's effects are one pass later; (3) Text-out faces are fed from the 500-line DECODER_LINES ring, cut
at 159 bytes, and are NOT cleared by the Decoder output window's Clear (THE TEXT-OUT FEED CHANGE: the old feed was the window's own
`patchSinkLines_`; a line longer than 159 bytes now differs, so a long line was kept out of the capture); (4) "Sound goes to" device
names are cut at 95 bytes by AUDIO_DEVICES (a longer name gets a different key); (5) a typed Radio centre is judged by the converter
published for the node's device, and a device chosen in the last couple of frames is judged as having no converter (the engine is the
judge when the edit lands); (6) the recordings listing appears one frame after the combo opens. FROM 13c: (1) a picture can trail its
decoder by one pass; (2) A DOCUMENT LOAD CLEARS EVERY PICTURE (before, an old picture could show on a new node that reused an id). AND
ONE THE CAMPAIGN FOUND AND NO REPORT HAD: (3, new) AT THE FRAME OF A START OR A STOP the port shows its consequences ONE FRAME EARLIER
than the control, not later: the first frame after the click, the Speaker's face (the file name, "Waiting for its radio." before) and the
rail's SoapySDR-deferral note appear in the port's frame N and in the control's frame N+1 (178 891 pixels differ at the frame, 6 at
every frame after, below). The reading by code: the control's runtime ran inside `drawPatchView`, AFTER the rail and the faces of the
frame it applied the command in; the port's runs in `pumpPatch` at the head of the pass, before the draw; and it holds only while
the pass runs at the head of the window's frame (once the pass has its own thread the 13a/13b statements, one pass later, are the right
ones). It is recorded as a difference, not a defect: nothing is lost or shown wrong at any frame.

THE PICTURES, as far as a picture goes. The application's own framebuffer capture in bounded runs, the window 1502x1405 as the capture
measures it (1500x1380 requested, 2 110 310 pixels), FOXSDR_SINGLE_VIEWPORT=1, one scratch copy of each exe with the same resources
beside it (and, for the Text-out states, the group-8 fixture module in the scratch exe's own `plugins` directory, never the real one),
the built-in signal generator as the source, no radio visible to the process (SOAPY_SDR_PLUGIN_PATH removed), each run on its own seeded
copy of the config (CASCADE_CONFIG_TEST; telemetry and the update check off, volume 0) with every folder the process derives from the
environment (USERPROFILE, HOME, APPDATA, LOCALAPPDATA) pointing into the run's own scratch directory and every URL at a dead local
port, and bounded by `--frames`. THE CONTROL is g13-control, the tree at 69f7d6a (groups 1 to 12, no group 13) in its own worktree, the
PORT is 69f7d6a plus parts a, b and c plus the api_03 repair; build\g13-vis-final.txt is the table (the scripts runone13.py,
campaign13.py, compare13.py, bracket13.py, shift13.py, where13.py, pts13.py and the input scripts are in the session's scratch, under
g13-vis). THE PATCH PAGE IS THE DEFAULT FACE, so it is in every capture and the campaign is TWELVE states, four runs of each build in each
(96 runs): DEFAULT (a fresh config on the generator: the app seeds a STARTER Radio once the receiver's open has answered, so the default
page is not empty); EMPTYDOC (no node at all: FOXSDR_PATCH_FILE with a document of no nodes, because a config document with no nodes does
not count - `patchSeeded_` is set only for a document that has one); SEEDED (a generator Radio wired to a Channel, a Demod and a
Speaker, a document in the config, NOT running: the faces, the plan marks, "This patch can run."); RUNNING (the same, the bounded-run
FOXSDR_PATCH_START seam: the transport, the readouts); RUNNING_SPEC (the same with a Spectrum part on the radio, a live trace);
RUNNING_CLICK (the same started by a click on the START dome from the input script, frames 38 to 42); ADDRADIO (the Radio key pressed
on the default page: a second Radio on top of the starter); ADDRADIO_EMPTY (the Radio key pressed on the empty page) and ADDRADIO_DOC
(the same radio as a document: the control for the history point below); TEXTOUT_OFF, TEXTOUT and TEXTOUT_CLICK (the group-8 fixture
decoder, which says "G8 decoded line 001..003" once a process with FOXSDR_FIXTURE_G8_LINES=3, wired to a Text out; idle, started
from the seam, started by a click; the lines are short, so the 159-byte cut does not enter and all three lines are the same in both
builds). THREE BRACKETS, frame by frame, three runs of each build: STARTCLICK and STOPCLICK (the dome pressed, a capture a frame from 34
to 61) and ADDBRACKET (the Radio key on the empty page, 36 to 59). The captures are 576 files. Areas, tiling the capture exactly (the
named areas are asserted disjoint, each node's bands lie inside the canvas, and each state's last row is the REMAINDER, the whole
capture minus the named areas, 2 892 pixels in every state): the frame's ring, the deck in five pieces, the rail in three, the page's
title strip, the transport in four (the dome, ALL OFF, the status sentence, the rest of the row), the parts bin, the decoders row,
the inspector in two, the CANVAS (computed as the canvas rectangle minus the node bands: the grid and the wires), and each node in
bands (the title bar, the rows its face draws, the readout row): 19 areas in EMPTYDOC up to 39 in RUNNING_SPEC. THE LIVE AREAS, measured
and printed and NOT in the 0-pixel claim, because their content is not a function of the build: the Channel's level readout, the
Demod's gate readout, the Speaker's file name and level bar (a second-resolution time stamp in the WAV's name: 8 distinct crops in 8
captures of every running state) and the Spectrum's trace.
THE RESULT. In SEVEN states (DEFAULT, EMPTYDOC, SEEDED, ADDRADIO, ADDRADIO_DOC, TEXTOUT_OFF, TEXTOUT_CLICK), 0 pixels differ at tolerance 24 AND at tolerance 0 in every area, in every one of the 16 control-against-port
pairs and in the 6 + 6 inside one build, and all 8 captures of each state are ONE FILE (9daeed2f, 1f3139fc, 21bcb389, fa928f6c, f40a48de,
c53e36df, 4618ca28). That covers the empty page's whole chrome and parts bin, the starter Radio's face, the seeded chain with its marks,
verdict and channel count, the idle Text-out chain, a Text out's three lines ("IQ decoder: G8 decoded line 00", cut by the face), the
decoder face ("3 lines", the newest line), the rail, the deck and the cabinet. In the five states with a running patch or a click
history, at tolerance 24 the figure is 0 everywhere outside the live areas but ONE pixel (below), and at tolerance 0: RUNNING and
RUNNING_SPEC 6 pixels in the decoders row, RUNNING_CLICK 6 in the transport's status sentence, ADDRADIO_EMPTY 2 in the Radio's centre
field and TEXTOUT 2 in the decoder's newest line, every one the SAME in all 16 control-against-port pairs and 0 inside either build, so
deterministic and not noise. THE THIRD-DIFFERENCE RULE: what are they? Each is one column at the edge of a SINGLE GLYPH of static
text (the decoders row's sentence; the status sentence; the serif tips, top and bottom, of the "M" of "MHz" in a centre field, seen at
12 times; a glyph of a decoder's newest line), the largest channel delta 27 (the one pixel over tolerance 24), 20, 11 and 8. They are not a
layout or a state difference, and the evidence is that the same kind of difference exists between two runs of ONE build whose only
difference is HISTORY: control START-by-seam against control START-by-click, same final state, 1 pixel in the decoders row; port
START-by-seam against port START-by-click, 5; control SEEDED against control RUNNING, 1; while the control's click run and the
port's click run are the SAME in that row (0), the port's RADIO-AS-DOCUMENT and the control's are the same (0), and the two builds'
TEXTOUT_CLICK are one file. The reading, UNVERIFIED as a mechanism: ImGui 1.92.8 (third_party/imgui) rasterises glyphs into a DYNAMIC
atlas in the order they are first drawn, and the port draws some text in a different order and frame from the control (a new Radio
shows "Choose a device on the right." for a frame that the control never draws; START's consequences come a frame sooner; the seam
starts the patch at frame 1 in both builds but the port's readouts arrive on other frames), which can change the sampling of an edge
column; the atlas itself was not dumped. NOTHING OUTSIDE THESE EIGHT FIGURES differs in any pair of any state. THE LIVE AREAS, with the
bound of the largest count two runs of one build show: RUNNING, Channel level (4 600 px) cvp24 163, cvp0 0 to 226, cvc0 183, pvp0 226,
noise24 (80, 163); Demod gate (7 590) cvp24 375, cvp0 0 to 545, cvc0 545, pvp0 0, noise24 (375, 0); Speaker (13 800) cvp24 299, cvp0
233 to 422, cvc0 405, pvp0 426, noise24 (277, 296); the whole capture cvp24 668, cvp0 239 to 1 096, cvc0 1 122, pvp0 591. RUNNING_SPEC,
the same bands within 10 pixels of the same bounds (the Speaker's cvp0 reaches 432 against a within-build 422, 16 pairs against 6)
and the Spectrum's trace (38 808 px) cvp24 561, cvp0 0 to 1 801, cvc0 1 806, pvp0 1 795, noise24 (558, 522); the whole capture cvp24
1 255, cvp0 215 to 2 799. RUNNING_CLICK, Channel cvp0 0 to 183 against 183, Demod gate 0 in every pair (one crop in 8 captures), Speaker
191 to 321 against 328, the whole capture cvp0 229 to 510 against a within-build 487 (6 of it the glyph pixels above). The control-
against-port counts are inside the noise of one build, and the shared-file evidence is better than a count: the Channel's level crop
is 3 distinct crops in 8 captures of RUNNING and 2 of the 3 are drawn by both builds, the Demod gate 2 and 1 shared, RUNNING_CLICK's
Demod gate ONE crop in all 8, RUNNING_SPEC's trace 6 distinct and 1 shared; the Speaker's name is a clock and shares nothing (8 in 8).
The stderr of the two builds is identical in all 48 control-and-port pairs (12 of them after masking the run's own directory in the
"patch loaded from" line), the stdout identical with its digits masked, and the config each run saved on exit identical in all 48.
THE COMPARATOR'S OWN CHECKS: control SEEDED against control RUNNING reports 127 282 pixels (187 985 at tolerance 0) with 121 577 in the
rail (the "Patch RUNNING" lamp, the SoapySDR note), 1 152 in the START dome, 1 381 in the status sentence, 329 in the Radio's device
row and 737 in its rate line, and 0 elsewhere; control DEFAULT against control EMPTYDOC reports 10 935 (16 325), 9 819 of them in the starter
Radio's five bands, 786 in the status sentence and 330 in the rail; control TEXTOUT_OFF against control TEXTOUT 128 679, 97 in the decoder's
line count, 434 in its newest line and 2 970 in the Text out's lines; and a +30 and a +10 poke on single pixels are counted as 1 pixel
at tolerance 24 and 2 at tolerance 0.
THE BRACKETS. STARTCLICK, 34 to 61 (button down at 38, up at 42): every frame from 34 to 42 is ONE FILE in all six runs (2f22dc.. for
34 to 37, f6faec.. for 38 to 41, 40cee1.. for 42): 0 pixels differ between the builds. AT FRAME 43, THE FIRST AFTER THE RELEASE, 178 891
pixels differ outside the live bands, in the rail (the SoapySDR note and the section list shift) and the Speaker's face; from 44 to 61
6 pixels (the glyph edge). The SHIFT TEST holds the one-frame reading: the port's frame 43 against the control's frame 44, outside the
live bands and the START dome's hover tooltip (which fades in by frame count, so is a frame younger there: 1 732 pixels), is 6 pixels,
and 44 against 45 the same. STOPCLICK (running from the seam, the dome pressed at the same frames) is the same shape: 178 890 at
frame 43, 6 before (the seam's history) and after, shift test 6. ADDBRACKET (the Radio key on the empty page): frames 36 to 41 ONE FILE
(0 differ); FRAME 42, the port shows the node as "No device chosen" with "Choose a device on the right." (the control: "Signal
generator", "Ready - press START.", 2 189 pixels); FRAME 43 the port's node already has its device and carries an orange outline round it,
which by the survey's and 13a's account is the plan's mark, a pass old, on a node that had no device (not separately identified;
2 115 pixels); from 44 to 59, 2 pixels (the "M"). That is 13a's difference 2, exactly
one frame of the sentence and one of the mark, and the plan marks trailing an edit by a pass; what the page shows from frame 44 is the
control's page. NO RUN WAS SET ASIDE. What the capture does NOT draw, and so does not hold: a real radio's device list, a SoapySDR
scan, a real sound card, a lent radio taken over by the patch, a Map with tracks (no hermetic track source, `hostTracks()` and
`hostPaths()` are held by code reading, and the Map's track bit at the window is not covered by any test), a decoded picture (the
fixture picture decoder makes a new picture per poll, so a capture of one is never deterministic, and the upload and delete of the
texture are held by the GL stand-in of scenario W), a document load frame, a drag mid-commit, the recordings combo just opened, a
Text out fed a LONG line (the 159-byte cut), and the squelch gate when it opens (the generator's level never opens it). The real
config (C:\Users\steve\AppData\Roaming\foxsdr\config.json, 5 076 bytes, written 2026-10-19 14:52:27 UTC by another session, sha256
BA532E6C...A353C) was identical before the first launch, in the middle of the campaign and after the last (size, modification time and
hash), and none of the scratch values (the seeded documents, the fixture's module name) is in it; nothing under
%LOCALAPPDATA%\foxsdr or %APPDATA%\foxsdr was written. ONE SIDE EFFECT OUTSIDE THE RULE: the first two RUNNING trial runs, before the
environment was redirected, were running patches whose Speaker records, and wrote two WAV files (11 564 and 12 524 bytes) into
%USERPROFILE%\Documents\SDR-recordings, a folder those runs created (its parent's modification time is the first run's, 06:10:31
on 2026-10-20); every later run wrote under its own scratch directory. The two
files are left in place for the owner to delete (nothing in the campaign deletes).

WHAT COULD MOVE A MEASURED NUMBER. 13a: the patch step runs every pass while the page is open (the catalogue, START and STOP, the
reconcile and the compile are by design a per-pass cost; with a settled graph the version sync and the publish allocate nothing, and
a graph that CHANGES, a drag, parses the SET_GRAPH text in the engine and `fact.patch.graph` in the window on every published change,
which the old frame never did, and is the allocation the whole frame's 43 below is part of); 8 retained graphs (a few KB each) and one
`rebaseDraft` per racing edit. 13b: PATCH_LIVE copies the readings into its block once a pass while a patch runs (its fingerprint
makes a pass that finds the same readings build nothing), `read_node_spectrum` copies 8 KB per radio per new frame, and a recordings
relisting is bounded (200 listed, 64 headers per listing). 13c: a NEW picture costs a move and one set swap (5 to 10 allocations, under
1 KB), a reader copies its pixels on its own thread and a 1.4 MB picture is a 1.4 MB copy per read (once per picture, in the window);
one `read_image` call is a short lock and a copy. The new window-side state is one cache refresh per `listSeq` and a stop-and-wait
ticket. None of these was measured in situ as a whole frame; the running frame's 43 allocations are the only whole-frame figure.

THE TRAPS, for the next group, by part.
13a. (1) `CHECK_EQ` binds REFERENCES: a temporary graph in a `CHECK_EQ` dangles; use by-value helpers. (2) Bash heredocs collapse a
backslash and an `n`; any file that holds a literal `\n` (this campaign's own scripts, again) is written with the file tools. (3) A test
frame is LOOK (`pumpFrameBegin`, `pumpPatch`, `pumpPublish`, `refreshUiState`, `pumpTransmitKey`), then DRAW; a draw that follows an engine
mutation without the LOOK reads the previous publish. (4) NEVER LEAVE AN ENGINE-HELD PROMISE UNFULFILLED: teardown waits for it. (5) A
`FOXAPP_OP_*` that is not in the op table cannot go through a session, whatever `applyCommand` says: the SCAN_NATIVE_ONLY bug was found by
a test that went through a session. (6) The adopt is the dangerous line: nothing may be adopted while a send is in flight, and a draft
that was never copied from a PUBLISHED graph is never sent. (7) The graph's version moves on ANYONE's change, including the runtime's
own; a test that expects it to stay still while a patch runs expects the wrong thing.
13b. (1) Bash heredocs mangle certain quote and backslash sequences: file tools. (2) `look()` is needed before touching an
unpublished node in a window test. (3) NEVER START A MUTANT BATCH TWICE: the instances contend for the files and one restore is from a
snapshot. (4) PATCH_LIVE is NOT in the fingerprint and must not be put there: it would rebuild the whole block every pass of a running
patch. (5) The receiver centre's VALUE is not in the fingerprint either; read the state's `centreHz`. (6) A device chosen in the last
couple of frames has no converter yet; a typed centre in that window is judged as a radio's with none.
13c. (1) Never overwrite a mutant harness's restore copy; verify every original string occurs exactly once before the rebuild. (2) A
picture row outlives a STOP and ends with its node: the window polls the row, never the event. (3) `FoxImageInfo` is append-only, and
a caller built to FOXAPI_MIN_IMAGE_INFO must be handed only the fields it was built to. (4) The picture fixture (tests/fixture_group13_plugin.cpp)
offers a new picture each time samples have arrived, so no capture of it is deterministic; a test that wants one waits for an id.
The api_03 repair. (1) A full-suite run proves nothing about an intermittent fix: break it deterministically. (2) A baseline in a test
that counts transitions must wait for the SINK to be steady, not only the counter to be still: a dead stream is a legitimate state
that recovers on the watchdog's schedule.
The campaign. (1) The default face is not empty (the starter Radio); a page with no node needs FOXSDR_PATCH_FILE. (2) A RUNNING PATCH
RECORDS: the Speaker writes a WAV under %USERPROFILE%\Documents\SDR-recordings, so redirect USERPROFILE, HOME, APPDATA and LOCALAPPDATA
into the run's scratch before the FIRST running run. (3) The Speaker's file name is a clock: its face is live in every running state.
(4) A node's capture rectangle is the canvas origin (427, 350 at 1500x1380) plus the document's (x, y) and size; a node wider than the
canvas (about 800 px) hides behind the inspector. (5) The decoder fixture goes in the SCRATCH exe's `plugins` directory (the plugin
directory is the one beside the exe when it is writable), never the real one. (6) Equal drawing HISTORY is part of equal pictures:
the seam, a click and a document reach the same state by different histories, and static text's edge pixels can differ between them;
compare like with like, and when a few pixels differ, compare the build against ITSELF reached by the other path before calling it a
port difference. (7) Python in a Bash heredoc loses its `\n` escapes (trap 2 of 13a): write scripts with the file tools.
(8) The tooltip of a pressed key fades in by frame count: a shift test excludes its rectangle.

UNVERIFIED, carried forward. No real radio, SoapySDR, sound card or hardware of any kind was driven for any part; no threaded pass was
run against the window (the pass runs at the head of the window's frame, so difference 3 of the capture, a frame early, is a property
of that order); the cross-thread safety of the picture store is proven by the two-reader test and not by a standard-window run; the
real GL upload and `glDeleteTextures` were held by a stand-in; a stale picture from a closed radio's runner could in principle show
once on a same-id node right after a document load (not reproduced); the patch step's per-pass cost; window mutant W12 (no second
version read: it matters only with a concurrent publish) and one fingerprint input (an open radio's centre, which no hermetic
test can move without moving the graph) are tested by VALUE and not alone; the unproven cause of the dying audio stream and the
undiagnosed four load flakes above; and the MECHANISM of the glyph-edge pixels (the atlas reading is a hypothesis).

OPEN FOR THE PUMP MOVE (the independent review of 47e991e..c47def4, 2026-10-13;
F3 is recorded, not fixed; F5 was checked and needs nothing).

F3, the key rules are touched from more than the pass. `TxKeyRules` is
single-threaded by contract (core/tx_key_rules.hpp, THREADING: one thread,
the one that runs the control pass; nothing in it locks), and
`ApiHostImpl::rules_` is reached from `Engine::applyCommand` as well as from
`txPass`: RUN 0, TX_OPEN and TX_CLOSE call `api_->endKeyFromEngine`, the web
remote's TX_PTT calls `api_->webRemotePtt`, and
FOXAPP_OP_WEB_CONTROL_STOPPED calls `api_->webRemoteStopped`. The window calls
`engine_.applyCommand` and `engine_.applyControlRequest` from its frame
(`AppWindow::applyWebControls` among them). Today that is safe, because the
window's frame IS the pumping thread. When this step's end moves the pump to
the engine's control thread, `txPass` -> `rules_.tick` runs there while the
GUI thread applies a browser PTT, a RUN 0 or a TX_CLOSE: both mutate
`keyOwner_`, `holdUntilMs_` and the `marks_` map, and a torn update can leave
`rules_.keyed()` true with no owner the tick can find, so `txPass` keeps
driving `setLatched` and `setPttHeld` from it and only the Transmitter's own
deadlines (`kLatchTimeout`, `kKeyAliveWait`) would end the key, not the 2 s
hold. THE FIX THE REVIEW PROPOSES, and to make with the move: send the web
PTT and every engine-ended key through the API queue (or `submitCommand`), so
`rules_` is only ever touched on the pumping thread. Until then the window must
remain the one thread that pumps: the only caller of `startControlThread()`
outside the engine itself is the table's `create()` (an engine the host
process does not own, which has no window beside it).

F5, the TX thread's front-end stamp is NOT written in two clock bases. The
stamp is written by `Transmitter::frontEndAlive()` (the key holder's
heartbeat) with the Transmitter's `nowMs()`, and by a key grant through
`frontEndAliveAt(lastBeatMs)` with the session's `lastBeatMs`, which the host
takes from its own `steadyMs()`. The two functions are the same expression:

    // src/engine/fox_engine_api.cpp
    std::int64_t steadyMs() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(SteadyClock::now().time_since_epoch()).count();
    }
    // src/core/transmitter.cpp
    std::int64_t nowMs() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }

(`SteadyClock` is `std::chrono::steady_clock`.) One clock has one epoch in a
process, so the stamps share an origin and the TX thread's
`now - frontEndAliveMs_ > kKeyAliveWait` compares like with like; a stamp can
be neither far in the past nor in the future. Nothing to fix. The two
functions are duplicates, though: if either is ever changed to another clock,
change both (or give them one definition), because no test can tell them apart
while they read the same clock.
