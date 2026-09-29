# FoxSDR Engine API — additions in 0.3 (draft)

Status: **draft 0.3, 2026-09-29**, authored in this tree by the owner's
decision 4 of that day (engine-stage4.md section 3, item 3: "extend the draft
for what the table has no read for"). It is written to be merged into the
upstream `docs/API.md` section by section. Until then this file is the
contract for every member, list, flag, event, op and fact below, and
`third_party/foxsdr_api/foxsdr_api.h` (0.3) must match it. Everything in the
0.2 specification (`third_party/foxsdr_api/reference/docs/API.md`) still
holds except where a section below says otherwise.

Implemented by the application's engine (`src/engine/fox_engine_api.cpp`,
`src/engine/engine_api_lists.cpp`) and held by
`tests/test_engine_api_03.cpp`. The reference mock engine is 0.2 and
implements none of it.

---

## 1. Versioning

`FOXAPI_VERSION_MINOR` is **3**. Everything is appended (rule 4): no member,
list, flag, op or event of 0.2 moved or changed meaning. During 0.x an engine
serves only its own minor, so a 0.3 engine answers `foxsdr_engine_query(0, 2)`
with NULL, exactly as a 0.2 engine answered `(0, 1)`. `FOXAPI_MIN_ENGINE_API`
is the whole 0.3 table (the minimum rule, "during 0.x: the current layout").
No other struct changed; `FoxScopeInfo` is new, with its own
`FOXAPI_MIN_SCOPE_INFO`.

## 2. Capabilities

| Bit | What it says |
|---|---|
| `FOXAPI_CAP_FACTS` (26) | `get_setting` answers the read-only `fact.*` keys (section 4) |
| `FOXAPI_CAP_LONG_TEXT` (27) | `submit_text` (section 3) |
| `FOXAPI_CAP_NODE_SPECTRUM` (28) | `read_node_spectrum` (section 7) |
| `FOXAPI_CAP_SCOPE` (23, 0.2) | now served: `read_scope` (section 6) |

## 3. The long-text channel — `submit_text` (closes 0.2 gap 10)

`submit_text(session, command, text, len, result)` submits ONE command whose
text is `len` bytes of UTF-8 at `text` (no NUL needed) instead of its
`text[]` slot: a patch document, a bookmark file, a report, a path. It is
`submit` with a count of 1 in every other respect — the same answers
(`FoxSubmitResult`, tickets, BUSY, merging), the same result, the same
grants and refusals. The engine validates the command on the text's first
255 bytes (what the slot would have held) and applies it with the whole
text. `len` over `FOXAPI_MAX_LONG_TEXT` (1 MiB) is `FOXAPI_LIMIT` for the
call; a NULL command, a command shorter than `FOXAPI_MIN_COMMAND`, or a NULL
text with `len > 0` is `FOXAPI_BAD_ARGUMENT`.

## 4. Facts — `get_setting("fact.…")`

A **fact** is a read-only value the engine publishes: a whole text a list
item would cut (`detail[]` holds 159 bytes), or a single value no list
carries. Facts are read with `get_setting`, which already returns the full
length and copies what fits; no `SETTING_SET` ever takes a `fact.` key (it is
refused, and the value does not move). A fact the engine does not have is
`NOT_FOUND`; a known fact with nothing in it is `""` (length 0). Facts marked
**LOCAL** name paths or radios on the engine's machine and answer
`FOXAPI_DENIED` to a REMOTE session. Facts move with `listSeq`: they are
published in the same block as the lists, in the same pass.

| Key | Value | |
|---|---|---|
| `fact.status.<line>` | the whole text of a status line (section 5.1), `""` when it has none | |
| `fact.patch.graph` | the patch graph as a document — exactly what `FOXAPP_OP_PATCH_SET_GRAPH` (and a patch file) take | LOCAL |
| `fact.recorder.dir` | the folder recordings go to | LOCAL |
| `fact.recorder.iq.path`, `fact.recorder.audio.path` | the current or last take's file | LOCAL |
| `fact.store.<id>.legal` | a catalogue entry's legal notice, whole | |
| `fact.store.<id>.summary`, `.description`, `.homepage`, `.version` | the entry's texts | |
| `fact.store.<id>.blocked` | why installing it would be refused now (the notice taken as acknowledged); `""` when it would not | |
| `fact.transmit.args` | the address `TX_OPEN` is given: the one typed, else the seed the Transmit page uses | LOCAL |
| `fact.converter.name` | the converter in front of the receiver, as the frequency readout names it | |
| `fact.converter.radioKey` | the radio its setting is remembered for | LOCAL |
| `fact.biasTee.reachable` | `1` when the open radio's bias tee can be switched from here | |
| `fact.biasTee.mayRemember` | `1` when the switch would be remembered for this radio | |
| `fact.biasTee.radioKey` | the radio it would be remembered for | LOCAL |

## 5. Lists (appended to 7.4)

Every list keeps the 0.2 rules: `read_list` returns the count and fills the
entry when `index < count`; `listSeq` (and `FOXAPI_EVENT_STATE`'s list bit)
says when to read again.

### 5.1 New lists

| List | Entry |
|---|---|
| `STATUS_LINES` (22) | the status lines that have text now. `name`: the line's key; `detail`: its text (whole: `fact.status.<key>`); `value[0]`: level 1 information, 2 warning, 3 error. The keys: `sourceError`, `gpsRefusal`, `catalogError`, `bandPlanError`, `tuneMismatchNote`, `transmitError`, `soundCardMissing`, `sdrPlayApiDetail`, `sdrPlayAdvice`, `recordNotice`, `recordError`, `presetNote`, `pluginEnforceError`, `restoreKeepLabel` |
| `CONVERTERS` (23) | every radio's remembered up/down-converter. `detail`: the radio key (LOCAL sessions only, else empty); `value[0]`: 0 off, 1 up, 2 down; `value[1]`: LO Hz; `value[2]`: 1 inverted; `ACTIVE`: the open radio's |
| `GAIN_MODES` (24) | a radio with gain MODES (an Airspy: Linearity, Sensitivity, Free). `name`; `value[0]`: the mode code `FOXAPP_OP_AIRSPY_GAIN_MODE` takes; `ACTIVE`. Empty for every other radio |
| `DECIMATIONS` (25) | the open radio's decimation factors. `value[0]`: factor; `value[1]`: the rate it gives, Hz; `ACTIVE` |
| `PATCH_NODES` (26) | the patch graph's nodes. `id`: node; `name`; `detail`: kind (`radio`, `channel`, `demod`, `decoder`, `display`, `sink`, `map`); `value[0..1]`: x, y; `value[2]`: Hz; `value[3]`: rate Hz; `ACTIVE`: switched on; `REFUSED`: the engine refused to run it; `FAULT`: its radio faulted or would not open |
| `PATCH_WIRES` (27) | `value[0]`: from node, `[1]`: its output port, `[2]`: to node, `[3]`: its input port |
| `PATCH_PROBLEMS` (28) | what the plan found wrong. `id`: node; `name`: the problem's code; `detail`: what the page says |
| `PATCH_PARTS` (29) | the decoder parts a graph may use. `name`; `detail`: key; `value[0]`: the port type it is fed (0 I/Q, 1 audio, 2 text, 3 control, 4 track); `value[1]`: the rate it needs, Hz |
| `RECORDINGS` (30) | the recorders' takes (current or last), then the patch's files. `name`: file; `detail`: path (LOCAL sessions only); `value[0]`: 1 I/Q, 2 audio, 3 patch; `value[1]`: bytes, **read live** (a growing file moves this without moving `listSeq`); `value[2]`: rate Hz; `ACTIVE` recording; `FULL` at its size limit; `STALLED` writes failing (FULL wins when both) |
| `DECODER_LINES` (31) | the decoder-output backlog, oldest first, at most 500. `id`: line number (1 = the first line since the engine started); `name`: plugin; `detail`: the line (cut to 159 bytes) |
| `STORE_BLOCKED` (32) | installed plugins the store's policy refuses to load. `name`: file; `detail`: why |
| `STORE_UPDATES` (33) | the updates the store would install. `name`: catalogue id; `detail`: "from -> to" |
| `BAND_PLANS` (34) | the plans `SET_BAND_PLAN` takes. `name`; `detail`: id; `ACTIVE` |
| `PATCH_RADIOS` (35) | the patch's radios. `id`: node; `name`: device; `detail`: why it faulted or did not open; `value[0]`: rate Hz; `ACTIVE` open; `FAULT` |

### 5.2 0.2 lists now served

`DECODERS` (`name`; `detail`: why idle, "" running; `ACTIVE` running;
`FED` fed signal now), `CATALOGUE` (`name`; `detail`: id; `INSTALLED`,
`COMPATIBLE`), `TX_DEVICES` (one entry: `detail` is the address `TX_OPEN`
will be given; `ACTIVE` open), `MARKERS` (`id`; `name`: label; `detail`:
plugin; `value[0..2]`: Hz, width, colour), `BAND_PLAN` (`name`; `detail`:
service; `value[0..2]`: start, stop, colour), `PRESETS` and `USER_PRESETS`
(`name`: label; `detail`: plugin; `value[0..2]`: Hz, mode, bandwidth),
`DEVICE_OPTIONS` (`name`: the `SET_DEVICE_OPTION` name; `ACTIVE` on; only the
ones the open radio has), `SERIAL_PORTS` (`name`; filled by
`SCAN_SERIAL_PORTS`). `TRACKS` stays **GAP** (`UNSUPPORTED`): map tracks come
through the map page's plugin APIs, which are not served yet.

### 5.3 New item flags

`FOXAPI_ITEM_REFUSED`, `FAULT` (patch), `STALLED`, `FULL` (recordings),
`FED` (decoders), `INSTALLED`, `COMPATIBLE` (catalogue).

## 6. Scope taps — `read_scope` (closes 0.2 gap 5)

`read_scope(session, tap, info, out, capFrames)` copies the NEWEST
`capFrames` frames of a demod-scope tap, oldest first, and returns how many it
copied. It is a SNAPSHOT, not a stream: an interface draws the last N and
asks again next frame. Taps: `FOXAPI_SCOPE_AUDIO` (the finished audio, mono,
48 kHz), `FOXAPI_SCOPE_IQ` (the channel's I/Q, two floats a frame, at the
channel rate), `FOXAPI_SCOPE_MPX` (the WFM multiplex, only while WFM runs).
`info` (may be NULL; shorter than `FOXAPI_MIN_SCOPE_INFO` is BAD_ARGUMENT)
carries `channels`, `rateHz` and `written` — frames the tap has taken in
total, so a reader tells a silent tap from a stalled one. A REMOTE session is
given at most `FOXAPI_SCOPE_REMOTE_FRAMES` (4096) frames: "reduced", as
`FOXAPI_CAP_SCOPE` always said. An unknown tap is `UNSUPPORTED`. The read
never waits: the tap is a lock-free single-producer ring the DSP thread
pushes and any thread may snapshot.

## 7. A patch radio's spectrum — `read_node_spectrum`

`read_node_spectrum(session, nodeId, sinceSeq, info, bins, cap)` is
`read_spectrum` for one patch radio: the same `FoxSpectrumInfo`, the same
`NO_CHANGE`, the same reduction by maximum when `cap` is smaller than the
bins. `nodeId` is a `PATCH_RADIOS` entry's `id`; a node with no radio running
is `NOT_FOUND`. The engine publishes each radio's newest spectrum once a
control pass into a ring of its own (made when the radio first publishes,
dropped when it goes).

## 8. State flags (appended to FoxReceiverState::flags)

| Flag | Group | Meaning |
|---|---|---|
| `FOXAPI_RX_BIAS_TEE_PRESENT` | device | the open radio has a bias tee this engine can switch |
| `FOXAPI_RX_BIAS_TEE_ON` | device | ...and it is on (the radio's readback) |
| `FOXAPI_RX_PATCH_RUNNING` | (seq only) | the patch page's graph is running |

## 9. Events and ops

`FOXAPI_EVENT_PATCH` (10): the patch graph, its epoch (a document load) or
whether it runs moved in the pass; `value[0]`: the epoch, `value[1]`: 1
running.

`FOXAPI_OP_SCAN_SERIAL_PORTS` (0x0F03, SETTINGS, **LOCAL sessions only** —
they are this machine's ports): the engine lists them on its own thread; the
result arrives as `SERIAL_PORTS` changing.

## 10. What stays a GAP after 0.3

From 0.2's list (API.md 10.7): the RDS snapshot (1), the auto-notch readout
(2), plugin panels and instruments as data and a plugin's command-key list
(4), map tiles and track info (6), per-node patch editing as ops (7: 0.3
reads the graph back and edits it whole through
`FOXAPP_OP_PATCH_SET_GRAPH` with `submit_text`), the transmitter's microphone
list and meters (8), device-scan diagnostics (9), bookmarks export and
scanner progress (11), diagnostics (12), updates (13), zoomed spectrum (14),
multi-VFO (15), a settings catalogue (16). The AUDIO stream, the settings
store and the decoders' text and image EVENTS are still not served by this
engine (their capability bits are clear).

**The plugin windows** (a plugin's own panel, drawn by the plugin's code
through the plugin ABI) are not an interface's to draw and get no API: they
stay in the HOST PROCESS (engine-stage4.md 6.4).
