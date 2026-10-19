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
