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
