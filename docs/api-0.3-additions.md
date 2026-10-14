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
is the whole 0.3 table (the minimum rule, "during 0.x: the current layout"),
and `FOXAPI_MIN_RECEIVER_STATE` the whole 0.3 `FoxReceiverState`, which grew
at its end (sections 8.1, 8.2 and 8.4). `FoxScopeInfo` is new, with its own
`FOXAPI_MIN_SCOPE_INFO`. No other struct changed.

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
| `fact.converter.name` | the converter in front of the receiver, as the frequency readout names it (translated when the block is built; a language switch rebuilds it) | |
| `fact.converter.radioKey` | the radio its setting is remembered for | LOCAL |
| `fact.converter.live` | the converter IN FORCE, the one the receiver applies: `<mode> <LO Hz> <inverted>` - mode 0 off, 1 up, 2 down; the LO written to the digit it reads back by (`%.17g`); inverted 0 or 1. `0 0 0` with none. Not the stored setting (`CONVERTERS`' ACTIVE entry): a sound card in I/Q mode has one stored and none in force (section 8.3) | |
| `fact.converter.alias` | the note the Source section may add when the radio was opened through SoapySDR because its native driver refused it: `""` (nothing to say), `notCarried` (the native radio's converter was not carried over, and none is set here yet) or `carried` (it still applies, and one is on) | |
| `fact.converter.modes` | the `FOXAPP_OP_SET_CONVERTER` modes the open source takes, comma-separated: `0,1,2`; `0,2` for a sound card in real mode (no up-converter); `""` for a sound card in I/Q mode, which takes no converter, so an interface draws no converter controls | |
| `fact.converter.upIgnored` | `1` when a stored up-converter for the open sound card is not used (a real-mode sound card takes a down-converter only), else `0` | |
| `fact.biasTee.reachable` | `1` when the open radio's bias tee can be switched from here: a radio is open, still answering, and has one - or the TEST STAND-IN is in front of the key (section 8.3) | |
| `fact.biasTee.mayRemember` | `1` when the switch would be remembered for this radio | |
| `fact.biasTee.radioKey` | the radio it would be remembered for | LOCAL |
| `fact.biasTee.radioName` | the radio's name as the confirmation dialog says it: the driver's own name, or the stand-in's words while the stand-in is in front of the key; `""` with none | |
| `fact.bandPlan.name` | the name of the band plan LOADED, as the Display section captions it: the plans of its chain joined with `" + "` ("ITU Region 1 + United Kingdom"), or every installed plan joined when none is selected. Not the `ACTIVE` `BAND_PLANS` entry's name, which is one plan's. An interface translates each part where it draws it; `""` with no plan (section 8.4) | |
| `fact.bookmarks.version` | the bookmark store's version counter, as a decimal: bumped by every add, edit, star, removal and import, and by nothing else. `BOOKMARKS` is copied again when it moves and not when `listSeq` does, since `listSeq` also moves when a decoder prints a line (section 8.5) | |
| `fact.input.rateHz` | the DSP chain's input rate in Hz, written to the digit it reads back by (`%.17g`): the rate the spectrum's axis, the VFO's reach and a click on the spectrum are measured against. It follows the source's rate (`sampleRateHz`) except after a change of the source's rate that the chain refused (section 8.5) | |

## 5. Lists (appended to 7.4)

Every list keeps the 0.2 rules: `read_list` returns the count and fills the
entry when `index < count`; `listSeq` (and `FOXAPI_EVENT_STATE`'s list bit)
says when to read again.

### 5.1 New lists

| List | Entry |
|---|---|
| `STATUS_LINES` (22) | the status lines that have text now. `name`: the line's key; `detail`: its text (whole: `fact.status.<key>`); `value[0]`: level 1 information, 2 warning, 3 error. The keys: `sourceError`, `gpsRefusal`, `catalogError`, `bandPlanError`, `tuneMismatchNote`, `transmitError`, `soundCardMissing`, `sdrPlayApiDetail`, `sdrPlayAdvice`, `recordNotice`, `recordError`, `presetNote`, `pluginEnforceError`, `restoreKeepLabel`, `audioHealth` (8.4: the audio watchdog's note, level 2) |
| `CONVERTERS` (23) | every radio's remembered up/down-converter. `detail`: the radio key (LOCAL sessions only, else empty); `value[0]`: 0 off, 1 up, 2 down; `value[1]`: LO Hz; `value[2]`: 1 inverted; `value[3]`: on the entry that is `ACTIVE` only, **the radio's own centre readback in Hz, read live** (the frequency the radio was told and kept - not the air frequency `centreHz` speaks; a retune moves it without moving `listSeq`; 0 on every other entry; section 8.3); `ACTIVE`: the open radio's |
| `GAIN_MODES` (24) | a radio with gain MODES (an Airspy: Linearity, Sensitivity, Free). `name`; `value[0]`: the mode code `FOXAPP_OP_AIRSPY_GAIN_MODE` takes; `ACTIVE`. Empty for every other radio |
| `DECIMATIONS` (25) | the open radio's decimation factors. `value[0]`: factor; `value[1]`: the rate it gives, Hz; `ACTIVE` |
| `PATCH_NODES` (26) | the patch graph's nodes. `id`: node; `name`; `detail`: kind (`radio`, `channel`, `demod`, `decoder`, `display`, `sink`, `map`); `value[0..1]`: x, y; `value[2]`: Hz; `value[3]`: rate Hz; `ACTIVE`: switched on; `REFUSED`: the engine refused to run it; `FAULT`: its radio faulted or would not open |
| `PATCH_WIRES` (27) | `value[0]`: from node, `[1]`: its output port, `[2]`: to node, `[3]`: its input port |
| `PATCH_PROBLEMS` (28) | what the plan found wrong. `id`: node; `name`: the problem's code; `detail`: what the page says |
| `PATCH_PARTS` (29) | the decoder parts a graph may use. `name`; `detail`: key; `value[0]`: the port type it is fed (0 I/Q, 1 audio, 2 text, 3 control, 4 track); `value[1]`: the rate it needs, Hz |
| `RECORDINGS` (30) | the recorders' takes (current or last), then the patch's files. `name`: file; `detail`: path (LOCAL sessions only); `value[0]`: 1 I/Q, 2 audio, 3 patch; `value[1]`: bytes, **read live** (a growing file moves this without moving `listSeq`); `value[2]`: rate Hz; `value[3]`: the take's length in **seconds**, **read live** like `value[1]` (a take that is running moves it without moving `listSeq`; section 8.2); `ACTIVE` recording; `FULL` at its size limit; `STALLED` writes failing (FULL wins when both) |
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

`GAINS` (0.2: `name`; `value[0..3]`: min, max, step, current) gains one word
(0.3): `detail` is the stage's **unit token**, so an interface can letter a
stage in the unit the radio speaks. Two tokens, one per
`cascade::source::GainUnit`, the same words `/api/status` calls `unit`:

| `detail` | `GainUnit` | The values are | Lettered |
|---|---|---|---|
| `dB` | `Decibels` | a gain in decibels (or the HF+'s negative attenuation) | "TUNER 30 dB" |
| `step` | `Steps` | the hardware's own register steps or table positions (an Airspy's LNA, MIXER, VGA...), whose range and step are in index units | "LNA 7", **no unit** |

Every stage of the radios shipped carries the same token, and an interface that
letters one gain for the whole radio reads stage 0's. An empty `detail` (an
engine that predates the token) is read as `dB`, which is what a gain was
before the Airspy.

A stage the radio's own automatic gain control is driving carries
`FOXAPI_ITEM_AGC` (0.3, section 8.3): an Airspy's LNA or MIXER with its AGC
switched on (`FOXAPP_OP_AIRSPY_AGC`). An interface greys such a slider rather
than hiding it - the number is what the stage goes back to when the AGC is
switched off. Switching an AGC moves `listSeq`.

### 5.3 New item flags

`FOXAPI_ITEM_REFUSED`, `FAULT` (patch), `STALLED`, `FULL` (recordings),
`FED` (decoders), `INSTALLED`, `COMPATIBLE` (catalogue), `AGC` (a gain stage its
own AGC is driving; `GAINS`, section 5.2).

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
| `FOXAPI_RX_DEVICE_OPENING` | device | a radio is being opened: the open runs on a worker and has not finished (section 8.3) |
| `FOXAPI_RX_AUTO_NOTCH_ENGAGED` | mode | the auto-notch has found a tone and is notching it; `FOXAPI_RX_AUTO_NOTCH` stays "enabled" (section 8.4) |
| `FOXAPI_RX_PILOT_LOCKED` | (seq only) | WFM: the stereo decoder has locked the 19 kHz pilot, stereo enabled or not; `FOXAPI_RX_STEREO_ACTIVE` is enabled AND locked (section 8.4) |

### 8.1 The transmitter as a page draws it (appended to FoxReceiverState)

0.2's state said whether a transmitter was open and keyed, its mode,
frequency and power; a page that SETS split, input, tone and monitor could
not read them back. Appended (all in the tx group - `txSeq` - except
`txInputPeak`, a measurement):

| Member | |
|---|---|
| `txFlags` | `FOXAPI_TX_SPLIT` (not following the receiver), `FOXAPI_TX_MONITOR` (the receiver heard while transmitting), `FOXAPI_TX_FAULTED` (the open board faulted), `FOXAPI_TX_INPUT_OPENING` (the microphone chosen is still opening: `TX_SET_INPUT` is BUSY meanwhile), `FOXAPI_TX_POWER_RANGE` (the board published a power range) |
| `txInput` | `TX_SET_INPUT`'s value: 0 microphone, 1 tone |
| `txSplitHz` | where a SPLIT transmitter goes |
| `txToneHz` | the test tone, as the transmitter took it |
| `txPowerMinDb`, `txPowerMaxDb` | the board's range for `TX_SET_POWER` (with `FOXAPI_TX_POWER_RANGE`; otherwise -89.75 and 0, the Pluto's) |
| `txInputPeak` | the input's peak since the engine's last publish, 0..1 - measured |
| `txDeviceName` | the open transmitter; "" with none |
| `txError` | the transmitter's last error; "" with none |

Two 0.2 members are made exact: `txFrequencyHz` is WHERE IT WOULD TRANSMIT -
`txSplitHz` with SPLIT on, the receiver's `tunedHz` with it off, whether a
board is open or not (the engine keeps an open board there once a pass) - and
`txPowerDb` is the setting, which is the board's readback once one is open,
never the 0 dB a closed board would read as.

### 8.2 The status column as a page draws it (appended to FoxReceiverState)

The STATUS column (cards AUDIO, DECODER OUTPUT, DECODERS, SINK, RECORDER,
RECEIVER) letters what the receiver is doing in one place. 0.2's state carried
the running flag, the device and sink names, the underrun count and the
decoder counts; it had no read for the sink's four states, the output ring,
the plugin audio path, the mute's subject, the decoder output total, the
recorders' elapsed time or the gain's unit. Appended after `txError`. The
group says which counter a change moves (`seq` moves for every group but a
measurement, as always); a **measurement** moves none:

| Member | Group | |
|---|---|---|
| `audioFlags` | `audioSeq` | `FOXAPI_AUDIO_OPENING` (a worker is inside the output device's open: nothing else about the sink may be asked), `FOXAPI_AUDIO_EVER_OPENED` (an open of the device has succeeded since the engine started; clear on a machine with no output device), `FOXAPI_AUDIO_STREAM_ALIVE` (the audio watchdog's view: the stream's callback is still being served; **clear while OPENING**, where the watchdog's question is not asked), `FOXAPI_AUDIO_PLUGIN_PLAYING` (a plugin holding `CASCADE_CAP_AUDIO_OUT` replaces the demodulated audio; `audioPlayingPlugin` names it) |
| `audioRateHz` | `audioSeq` | the finished audio's rate (48 kHz): what the ring's frames are counted at, so `1000 * audioRingFrames / audioRateHz` is the ring's level in milliseconds |
| `audioRingFrames`, `audioRingCapacityFrames` | measurement | the output ring's fill and size, in frames |
| `audioPluginGaps`, `audioPluginGapFrames` | measurement | blocks in which the playing plugin handed over less than a full block, and the frames of silence that cost; since the plugins were last rebuilt. The plugin path's sibling of `audioUnderruns` |
| `decoderLinesTotal` | measurement | every decoder output line since the engine started. `DECODER_LINES` ids are numbered from the same counter, but that list is a ring of 500 and its ids are not a total an interface can difference over a window once it wraps; this one is. It climbs with every line and must not rebuild anything |
| `decodersMatched` | `seq` | decoder modules (of `decodersFitted`) the runner has built and matched to the receiver's rate, **running or not**: the "N ready" of a stopped receiver. `decodersRunning` is this while `FOXAPI_RX_RUNNING` and 0 while stopped |
| `audioMutedBy` | `audioSeq` | the decoder plugins muting the audio because the receiver sits on one of their presets, "A", "A and B", "A, B and C"; "" when none is (the user's own mute is `FOXAPI_RX_MUTED`, not this). Cut on a character at 127 bytes |
| `audioPlayingPlugin` | `audioSeq` | the descriptor name of the plugin holding the speakers; "" when the receiver's own audio plays. Cut on a character at 127 bytes |

`sinkName` (0.2) is the device the last successful open landed on. Not live:
while an open holds the sink's lock the last good name stands (it is the name
of a sink that is still there), and nothing counter-wise moves.

How a page letters the SINK card from these, in the order it asks (the first
that holds wins; the plugin line is added under every one): `audioMutedBy`
non-empty is MUTED, "by `audioMutedBy`"; `FOXAPI_AUDIO_OPENING` is OPENING;
no `FOXAPI_AUDIO_EVER_OPENED` is NO DEVICE; no `FOXAPI_AUDIO_STREAM_ALIVE` is
STOPPED; otherwise OPEN, with `sinkName`.

**Two lists change.** `GAINS` entries carry the unit token in `detail` (section
5.2), and `RECORDINGS` entries carry the take's elapsed seconds in
`value[3]` (section 5.1):

- `value[3]` is read **live** from the recorder at read time, like `value[1]`,
  from a **steady-clock** stamp the recorder takes when the take starts: not
  any interface's frame clock, and readable from any thread. For a take that
  is `ACTIVE` it is the time since the stamp; for one that is not (stopped, or
  ended at its size limit) it is the length the take had when it ended, which
  the recorder stamps too. 0 when unknown (a patch recording, or no take has
  ever started). Nothing about a running take moves `listSeq`.
- A page reads the recorders' state from the state's flags
  (`FOXAPI_RX_RECORDING_IQ`, `FOXAPI_RX_RECORDING_AUDIO`) and the entries'
  flags: `STALLED` on an `ACTIVE` entry is a disk that stopped taking the
  file; `FULL` on an entry that is not `ACTIVE` is a take that ended itself at
  4 GB. The entry with `value[0]` 1 is the I/Q take, 2 the audio take; both
  exist once a take has started and stay (the last take) until the next.

The RECEIVER card's other readings are already in the table: `deviceName`,
`sampleRateHz`, `FOXAPI_RX_RUNNING`, `FOXAPI_RX_FAULTED`,
`FOXAPI_RX_DEVICE_OPEN`, `FOXAPI_RX_DEVICE_AGC`, `gainCount`, the `GAINS` list
and the `ANTENNAS` entry that is `ACTIVE`.

`FOXAPI_MIN_RECEIVER_STATE` is the whole 0.3 layout.

### 8.3 The radio panels as their pages draw them

The Source section's Airspy controls, the deck's bias tee key (and its
confirmation) and the Converter rows read the engine's table only. 0.3 had most
of what they need (`GAIN_MODES`, `DECIMATIONS`, `GAINS`, `CONVERTERS`,
`fact.converter.name`, `fact.converter.radioKey`, `fact.biasTee.*`,
`tunedHz`); this adds the rest. Nothing is appended to `FoxReceiverState`: one
flag, one item flag, five facts and one list value.

- **`FOXAPI_RX_DEVICE_OPENING`** (device group): a radio is being opened - the
  open runs on a worker and has not finished. It is clear at rest, set from the
  pass that starts an open, and clear again in the pass that installs the radio
  (`FOXAPI_RX_DEVICE_OPEN` rises in that pass) or reports that it would not
  open. An interface greys the controls that would land on "whichever radio
  happened to be installed at that instant" (the converter's) while it is set;
  the engine refuses `FOXAPP_OP_SET_CONVERTER` meanwhile (`BUSY`). It moves
  `deviceSeq` and `seq`, and not the plugin ABI's device counters.
- **`FOXAPI_ITEM_AGC`** on `GAINS` entries (section 5.2): a stage the radio's
  own AGC is driving. The Airspy's LNA and MIXER stages while their AGC is on.
  An item flag, because the state's flag word is scarce and a list entry is
  where a stage's other facts already are; and not an entry in
  `DEVICE_OPTIONS`, which would make the AGC a second thing to write.
- **`CONVERTERS` `value[3]`** (section 5.1) on the `ACTIVE` entry: the radio's
  own centre readback, read live. The Converter rows letter "radio at ..." from
  it beside the air frequency the counter shows. It moves with every retune, so
  it is NOT in the list fingerprint (it must not rebuild the block): the engine
  writes it into an atomic once a pass and the list reads the atomic when it is
  read, as a recording's byte count is read.
- **`fact.converter.live`, `.alias`, `.modes`, `.upIgnored`** (section 4): the
  converter in force, which note the Source section adds for a SoapySDR
  fallback, which modes the open source offers, and whether a stored
  up-converter goes unused on the open sound card. The sentences themselves stay
  in the interface (it translates them); the engine says only which, or none.
- **`fact.biasTee.radioName`** (section 4): the radio's name for the
  confirmation dialog.

**The bias tee stand-in is the engine's.** With `FOXSDR_FORCE_BIAS_KEY=accept`
(or `refuse`) in a bounded (`--frames`) run - or, in an engine created with
`testHooks=1`, `SETTING_SET test.biasStandIn=accept|refuse|none` - a stand-in
bias tee stands in front of the key while no real radio with one is reachable:
it takes (`accept`) or refuses (`refuse`, the refusal is a `sourceError` status
line) every `FOXAPI_OP_SET_BIAS_TEE` and starts off. While it is in front,
`fact.biasTee.reachable` is `1`, `FOXAPI_RX_BIAS_TEE_ON` is the stand-in's state,
`fact.biasTee.radioKey` / `.mayRemember` / `.radioName` are its own, and
`FOXAPI_RX_BIAS_TEE_PRESENT` is not set (no real radio has one). An interface
draws the key from the fact and the flag and knows nothing of the stand-in. The
product never has one: an interactive launch ignores the variable.

**What moves `listSeq`, beyond what 8.1 and 8.2 listed.** The lists and facts
are rebuilt only when the fingerprint of everything they carry moves, so each
input of the facts above is in it: whether the bias tee is reachable (a radio
that stops answering changes nothing else), the size of the bias tee memory
(`mayRemember`), the stand-in, the open radio's name, the converter in force,
the SoapySDR alias and not-carried keys, the sound card's device, host API and
format (two sound cards leave the radio args empty alike), each gain stage's
AGC flag, and the language (the converter's name is a translated sentence).
The radio's centre is not.

### 8.4 The receiver rail and the display as their pages draw them

The Radio section (mode, VFO, bandwidth, squelch, de-emphasis, the WFM stereo
and RDS controls, the S-meter), the audio filters, the Sinks section, the
Display section's band plan, the band plan overlay on the spectrum, the rail's
footer and the Language section's country picker read the engine's table only.
0.3 had nearly all of it (`demodMode`, `bandwidthHz`, `squelchDb`,
`deemphasis`, `vfoOffsetHz`, `tunedHz`, `signalDb`, `dbMin`, `dbMax`, the
flags `RUNNING`, `STEREO_ENABLED`, `STEREO_ACTIVE`, `NR`, `NOTCH`, `AUTO_NOTCH`,
`nrStrength`, `notchHz`, `notchQ`, `pilotLevel`, `audioUnderruns`, the sink's
`audioFlags`, `audioMutedBy`, and the lists `BAND_PLAN`, `BAND_PLANS`,
`AUDIO_DEVICES`, `STATUS_LINES`); this adds the rest. Appended after
`audioPlayingPlugin`; **`FOXAPI_MIN_RECEIVER_STATE` moves to the end of
`rdsRadioText`**.

| Member | Group | |
|---|---|---|
| `rdsFlags` | `rdsSeq` | `FOXAPI_RDS_SYNCED` (the block decoder has found the 1187.5 bit/s stream: listening, no field complete yet), `FOXAPI_RDS_PI_VALID`, `FOXAPI_RDS_PS_VALID` (all four fragments of the programme service name are in), `FOXAPI_RDS_TP` (the station carries traffic information at all), `FOXAPI_RDS_TA` (an announcement is on the air now), `FOXAPI_RDS_RT_VALID` (`rdsRadioText` holds some text) |
| `rdsPi`, `rdsPty` | `rdsSeq` | the programme identification, with `PI_VALID`; the programme type as the raw 5-bit code (RDS and RBDS name it differently, so the code is what is published) |
| `rdsPs[9]`, `rdsRadioText[65]` | `rdsSeq` | the programme service name: 8 characters as transmitted, space padded, `""` until `PS_VALID`; RadioText, up to 64 characters, `""` until some has arrived. Bytes as the decoder recovered them, each NUL-terminated |
| `rdsSeq` | | advances when any of the five above changed; `seq` moves with it. The five change a few times a minute, not on every pass |
| `rdsGroups`, `rdsErrors` | measurement | complete groups decoded, and blocks that failed their checkword while synchronised, since the decoder was last reset (it is on every retune). They climb with the stream and move **no** counter |
| `autoNotchHz` | measurement | where the auto-notch sits; `0` when it is not engaged. Moves no counter |

- **`FOXAPI_RX_AUTO_NOTCH_ENGAGED`** (mode group): the auto-notch has found a
  tone and is notching it. `FOXAPI_RX_AUTO_NOTCH` stays "enabled": the rail
  letters "on 1000 Hz" when both are set and "searching" when only the second
  is. It moves `modeSeq` and `seq`, and not the plugin ABI's mode counters.
- **`FOXAPI_RX_PILOT_LOCKED`** (`seq` only): the stereo decoder has locked the
  19 kHz pilot, stereo enabled or not. `FOXAPI_RX_STEREO_ACTIVE` is enabled AND
  locked, so the rail's third state - "ST (forced mono)", a station with a pilot
  while the user has forced mono - and its "pilot 0.93" figure need the lock
  itself; `pilotLevel` is a measurement and the decoder's lock has hysteresis,
  so it cannot stand in.
- **`audioHealth`** in `STATUS_LINES` (level 2, warning) and
  `fact.status.audioHealth` (section 5.1): the audio watchdog's note under the
  Sinks section's device combo ("output device stopped and was restarted (1
  time)", "audio device busy - still opening"). It moves `listSeq` with every
  other status line.
- **`fact.bandPlan.name`** (section 4): the name of the band plan LOADED. The
  Display section captions it with the count ("ITU Region 1 + United Kingdom
  (84 bands)"); `BAND_PLANS` names single plans, so the `ACTIVE` entry's name
  is "United Kingdom" and not the caption. Its input, `bandPlan_.name()`, was
  already in the list fingerprint.

How the rail reads the band plan, the one list that can hold hundreds of
entries: `BAND_PLAN` is in the order the model keeps (start ascending, widest
first on a shared start), so walking it back to front draws a band under the
ones nested inside it; the band under a frequency is the NARROWEST entry
containing it (both edges inclusive, the first on a tie), and the entries a
view shows are those with `start < high && stop > low` (strict at both edges).
An interface that draws it every frame keeps its own copy of the rows and reads
them again only when the plan changed - `listSeq` also moves when a decoder
prints a line, so it is not the plan's own signal: the number of entries, the
`ACTIVE` `BAND_PLANS` entry and `fact.bandPlan.name` are.

**Names are cut at the list item's size** (`name[96]`): an audio output device
whose name is longer than 95 bytes is listed cut on a character boundary. The
sink's own `sinkName` (64) and the `deviceName` the rail's footer letters (64)
are cut the same way, at their member's size.

**Test hooks** (an engine created with `testHooks=1` only, as
`SETTING_SET test.…=…`): a hermetic engine on the signal generator hears no RDS,
has no pilot to lock and no tone for the auto-notch, and has no band plans beside
its executable, so `test.rds=flags;pi;pty;groups;errors;ps;radioText` (flags a
word of `S`ynced, `P`I valid, service-name valid `N`, `T`P, `A`nnouncement),
`test.autoNotch=<Hz>` (engaged there), `searching` or `searching:<Hz>` (not
engaged, the pipeline's frequency still reading that), `test.pilot=1|0` and
`test.bandPlanDir=<folder>` stand in for what the DSP thread measures and where
the plans are, and `test.statusLine=audioHealth|<text>` for the watchdog. An
empty value hands the member back to the real source. Everything after the hook
- the flag, the counter, the bytes, the cut - is the engine's own;
`tests/test_engine_api_rds_real.cpp` holds the other half against a real decode.

**What moves `listSeq`, beyond 8.1 to 8.3.** The audio watchdog's note
(`audioHealth`). The plan's name, the plans there are, the selection and the
number of entries were already in the fingerprint.

`FOXAPI_MIN_RECEIVER_STATE` is the whole 0.3 layout, ending at `rdsRadioText`.

### 8.5 The tuning surface as its pages draw it

The top bar (the transport, the master lamps, the volume dial and its two
meters, the frequency readout), the keyboard and the centre panels (the
spectrum, the waterfall, the band markers and the VFO overlay) read the
engine's table only. 0.3 had nearly all of it: the state (`tunedHz`,
`centreHz`, `vfoOffsetHz`, `bandwidthHz`, `demodMode`, `volume`, `squelchDb`,
`dbMin`, `dbMax`, `sampleRateHz`, `channelRateHz`, and the flags `RUNNING`,
`FAULTED`, `MUTED`, `DECODER_ACTIVE`, `RECORDING_AUDIO`, `TX_KEYED`, with
`audioMutedBy` and the transmitter's `FOXAPI_TX_MONITOR`), the lists
`BOOKMARKS` and `DECODERS`, `read_scope(FOXAPI_SCOPE_AUDIO)` for the volume
meter, `read_spectrum` for the trace and the waterfall, and
`fact.converter.live` / `.name` for the readout's "via ..." line. This adds two
facts (section 4). Nothing is appended to `FoxReceiverState`, and no op is new:
every command the surface sends is one it always sent.

- **`fact.bookmarks.version`**: the bookmark store's own version counter
  (`FreqManager::version`), which the list fingerprint already hashed. The
  markers on the spectrum come from `BOOKMARKS` (sorted by frequency, so the
  ones in view are two binary searches; `value[0]` the Hz, `FAVOURITE`, `name`,
  cut at the list item's 95 bytes), which can hold tens of thousands of
  entries. `listSeq` moves for far more than the store - every line a decoder
  prints moves it - so an interface that copies the rows when `listSeq` moves
  copies them all the time. It copies them when this fact moves, and reads
  nothing else of the list between.
- **`fact.input.rateHz`**: the DSP chain's input rate. `sampleRateHz` is the
  SOURCE's. They are the same number until the source's rate is changed to one
  the chain refuses (no decimation gives an exact channel, or the rate is out
  of the range it accepts): the radio then streams at the new rate while the
  chain, and so the VFO's reach (`vfoOffsetInsideSpan`) and every decoder, stay
  on the old one, and the display is deliberately drawn against the old one
  too (`Engine::followInputRate`). An interface that draws the spectrum, keeps
  the VFO band inside it or turns a click into a frequency uses this and not
  `sampleRateHz`. It moves `listSeq`, in the same publish as the rate.

**The surface's own rules, over what the table serves.**

- The MUTE lamp is lit when `FOXAPI_RX_MUTED` is set or `audioMutedBy` is not
  empty (the user's mute, or a plugin's). The volume meter's hard mute is that
  AND a keyed transmitter that is not monitored (`FOXAPI_RX_TX_KEYED` without
  `FOXAPI_TX_MONITOR`): the pipeline's own `audioMuted`.
- The DEC lamp is `FOXAPI_RX_DECODER_ACTIVE`; the waterfall's caption counts the
  `DECODERS` entries with `FOXAPI_ITEM_FED` and names the first.
- The spectrum is drawn from ONE `read_spectrum` a frame, into a buffer the
  interface keeps, at the engine's own bin count (a `cap` that is the bin count
  of the last frame, so nothing is reduced); `FOXAPI_NO_CHANGE` keeps the frame
  and adds nothing to the waterfall. The waterfall is the history of this
  stream (API.md 6.5): one row for each NEW `seq` the interface reads and none
  for a `NO_CHANGE`, so a row is never drawn twice. `seq` is the ring's own
  counter, one for each pass that had a frame; an interface that reads once a
  frame sees the newest of the frames published since, as `getLatestFrame` did.
  The centre of the axis is the state's `centreHz`, not the frame's, so the axis
  follows a retune while the receiver is stopped and no frame arrives.
- A command the interface needs the RESULT of in the same frame (a drag that
  moves the VFO band and draws it moved) is not served by the table, whose
  commands land at the engine's next pass: the interface applies the engine's
  own limits itself for the frame (`vfoOffsetInsideSpan(offset, fact.input.
  rateHz, bandwidthHz)`, and `bandwidth` into [3 kHz, 0.9 x `channelRateHz`])
  and sends the same numbers; the next read carries the result.
- `FOXAPI_OP_SET_FREQUENCY` is refused outside (0, 6 GHz] (`OUT_OF_RANGE`),
  where the window's typed entry used to hand the driver any figure; the window
  checks the range itself and says so, and shows any other refusal of its
  ticket from `poll_results` a pass later.

**What moves `listSeq`, beyond 8.1 to 8.4.** The chain's input rate. The
store's version (and its size) were already in the fingerprint.

**Test seam.** `ApiHost::publishSpectrumForTest(bins, count, centreHz, spanHz)`
puts one frame into the ring as `endPass` does (engine-side C++, not the table;
nothing in the product calls it), so a hermetic engine that runs no pipeline -
a window test must not open a sound device - can hand an interface a spectrum
whose every bin is known.

## 9. Events and ops

`FOXAPI_EVENT_PATCH` (10): the patch graph, its epoch (a document load) or
whether it runs moved in the pass; `value[0]`: the epoch, `value[1]`: 1
running.

`FOXAPI_OP_SCAN_SERIAL_PORTS` (0x0F03, SETTINGS, **LOCAL sessions only** —
they are this machine's ports): the engine lists them on its own thread; the
result arrives as `SERIAL_PORTS` changing.

`FOXAPI_OP_SET_BIAS_TEE` (0x0307, SETTINGS) takes, in `text`, **optionally** the
radio the sender was ASKED about (`fact.biasTee.radioKey`). A person is asked
once per radio before power goes up the antenna cable; the answer is "yes, for
that radio". When the text is not empty and the radio open when the command
runs is not that one - it changed between the question and the answer, or
between the interface last looking and its submit - the engine refuses it
(`FOXAPI_NOT_FOUND`, result text "the radio changed") and switches nothing.
No text, no check: every other sender (the Source panel's checkbox, a plugin,
the web remote) is as it was. The standard window sends the key on every "on"
the deck's key makes; the radio can change between the window's last look and
its submit, and a yes for A must not land on B once the engine's passes run on
their own thread.

`FOXAPP_OP_NOTE_PANEL` (0x841F, VIEW, **LOCAL sessions only** — an app op,
like every `FOXAPP_OP_*`, and a remote session's pages are not this
installation's usage): `text` is the name of a page the standard window has
open ("radar", "map"). The engine adds it to the list the anonymous usage
report sends as `panels` — **once**: a name already listed is a no-op, and the
list stops at **16** names, so a seventeenth is a no-op too (both answer OK;
nothing is refused or merged). An empty name is `BAD_ARGUMENT`; a REMOTE
session is `DENIED` and adds nothing. The window sends each name once a run
(`AppWindow::uiNotePanel`), where it used to call the engine from the page's
frame, every frame the page was open. Not a "safer" command (API.md 7.2): it
is never merged, because a repeat is already free.

## 10. What stays a GAP after 0.3

From 0.2's list (API.md 10.7), 0.3 serves the RDS snapshot (1) and the
auto-notch readout (2) (section 8.4). What stays: plugin panels and instruments
as data and a plugin's command-key list
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
