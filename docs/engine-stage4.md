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
| the merge sweep | a fresh engine per case | one engine per worker, reset between cases (`test.resetKey`); 5450 cases by default (all of length <= 3, 1000 random of 4), `FOXSDR_MERGE_FULL=1` runs all 70450 - run once: 0 differ, 584 s |

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
