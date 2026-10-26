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
at its end (sections 8.1, 8.2, 8.4, 8.6, 8.10 and 8.11). `FoxScopeInfo` is new, with its own
`FOXAPI_MIN_SCOPE_INFO` (its one reserved word became `capacityFrames` in section 8.11). `FoxImageInfo` grew at its end
(`complete`, a reserved word and `sequence`, section 8.12.7; `FOXAPI_MIN_IMAGE_INFO` is unchanged). No other struct changed.

## 2. Capabilities

| Bit | What it says |
|---|---|
| `FOXAPI_CAP_FACTS` (26) | `get_setting` answers the read-only `fact.*` keys (section 4) |
| `FOXAPI_CAP_LONG_TEXT` (27) | `submit_text` (section 3) |
| `FOXAPI_CAP_NODE_SPECTRUM` (28) | `read_node_spectrum` (section 7) |
| `FOXAPI_CAP_SCOPE` (23, 0.2) | now served: `read_scope` (section 6) |
| `FOXAPI_CAP_IMAGES` (25, 0.2) | now served: `read_image` and `FOXAPI_EVENT_DECODER_IMAGE`, for the patch decoders' pictures (section 8.12.7) |
| `FOXAPI_CAP_AUDIO_STREAM` (3, 0.2) | now served: `read_audio` (section 8.16.1) |
| `SOURCES` (5), `GAINS` (6), `RDS` (8), `PATCH` (10), `RECORDER` (11), `BOOKMARKS` (13), `SCANNER` (15), `PLUGIN_STORE` (17), `BAND_PLAN` (21), `POSITION` (24) (0.2) | served since the steps that brought them, and declared from stage 5a; section 8.16.2 states the reason for every bit of the word, set and clear |

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
| `fact.patch.version` | the graph's version, a decimal: a counter the engine bumps on every accepted change to the graph, its own or a session's (starts at 1). In the same block as `fact.patch.graph` and `.epoch`; `FOXAPP_OP_PATCH_SET_GRAPH` names it in `ival[1]` (section 8.12) | |
| `fact.patch.epoch` | how many patch documents have been loaded, a decimal (section 8.12) | |
| `fact.patch.runnable` | `1` when the plan can run, else `0` (a refused plugin does not clear it; a radio that would not open does; section 8.12) | |
| `fact.patch.device.<id>` | the whole device key of `PATCH_DEVICES` entry `<id>` (section 8.12, part 2) | LOCAL |
| `fact.patch.receiverRadio` | the key of the receiver's radio as a Radio node names it: the one lent to the patch, else the one open, else `""` (8.12) | LOCAL |
| `fact.patch.receiverCentre` | the AIR centre in Hz the receiver's radio carries, or `""` when it was never tuned; its existence moves `listSeq`, its value does not (8.12) | |
| `fact.patch.conv.<node>` | the converter in force for a Radio node's device, `"<mode> <LO Hz> <inverted>"` as `fact.converter.live` (8.12) | |
| `fact.recorder.dir` | the folder recordings go to | LOCAL |
| `fact.recorder.iq.path`, `fact.recorder.audio.path` | the current or last take's file | LOCAL |
| `fact.telemetry.enabled` | `1` when anonymous usage reporting is on, else `0` (section 8.13) | LOCAL |
| `fact.telemetry.installId` | the random id the engine minted at consent, whole; `""` while reporting is off - the id does not exist then (section 8.13) | LOCAL |
| `fact.diag.launches`, `fact.diag.crashes` | this installation's launch count and the runs that never wrote their clean-exit marker, decimals (section 8.13) | LOCAL |
| `fact.store.<id>.legal` | a catalogue entry's legal notice, whole | |
| `fact.store.<id>.summary`, `.description`, `.homepage`, `.version` | the entry's texts | |
| `fact.store.<id>.blocked` | why installing it would be refused now (the notice taken as acknowledged); `""` when it would not | |
| `fact.store.<id>.author`, `.licence`, `.platforms`, `.floor`, `.file`, `.updateReason` | the entry's maker and licence, the builds it publishes (`windows/x64, linux/x64`), its retirement floor, the file name this machine's build installs under (the join to `PLUGINS`' `detail`), and the update plan's reason; `""` for none of them (section 8.9) | |
| `fact.store.url` | the catalogue index the store reads: a URL, or a path on this machine (section 8.9) | LOCAL |
| `fact.blocked.<file>.id`, `.message` | for a retired plugin (`STORE_BLOCKED`'s `name` is `<file>`): the catalogue id the update is matched on, and the whole sentence composed in the language in force (section 8.9) | |
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
| `fact.bookmarks.version` | the bookmark store's version counter, as a decimal: bumped by every add, edit, star, removal and import, and by nothing else. `BOOKMARKS` is copied again when it moves and not when `listSeq` does: `listSeq` moves for every other list change (section 8.5), and an interface that keeps a copy of a long list should not take it again for those | |
| `fact.input.rateHz` | the DSP chain's input rate in Hz, written to the digit it reads back by (`%.17g`): the rate the spectrum's axis, the VFO's reach and a click on the spectrum are measured against. It follows the source's rate (`sampleRateHz`) except after a change of the source's rate that the chain refused (section 8.5) | |
| `fact.mute.subject` | the plugins muting the audio, "A", "A and B", "A, B and C", **whole** (`audioMutedBy` is the same text cut at 127 bytes); `""` when none is (section 8.7) | |
| `fact.mute.keptRunning` | `1` while the mute is held after a tune-away and the operator has not answered the question, else `0` (section 8.7) | |
| `fact.plugin.lastDenied` | the module file name of the last plugin whose request to tune the receiver was refused; `""` none, and again after a rescan (section 8.7) | |
| `fact.plugin.dir` | the folder the engine loads plugins from (section 8.8) | LOCAL |
| `fact.plugin.<file>.path` | the absolute path a module file was loaded (or refused) from; `<file>` is the module file name, as `PLUGINS` names it (section 8.8) | LOCAL |
| `fact.plugin.<file>.version`, `.author`, `.licence` | the descriptor's strings, `""` for a file refused before a descriptor was read (section 8.8) | |
| `fact.plugin.<file>.error` | why the host refused the file, whole; `""` when it loaded (section 8.8) | |
| `fact.plugin.<file>.idle` | the runner's own sentence for why a module is not being fed, whole; `""` when it is (section 8.8) | |
| `fact.plugin.<file>.notice` | the module's newest WARN or ERROR, whole (up to 512 bytes; its level is `PLUGINS` `value[2]`) (section 8.8) | |
| `fact.plugin.<file>.integrity` | the host's sentence when the file is not the one installed (`FOXAPI_ITEM_DIGEST_MISMATCH`); else `""` (section 8.8) | |
| `fact.source.row` | the Source combo's selected row as a decimal, `-1` for none: 0 the generator, 1 the IQ file, 2 the sound card, 3 and on the radios (the index of the `DEVICES` entry; section 8.10) | |
| `fact.source.keep` | the kind of the source the config is still naming while the generator stands in for it: `""`, `file`, `soundcard`, `soapy` or a native driver's key (section 8.10) | |
| `fact.source.opening` | the name of the radio being opened, `""` when none is (section 8.10) | |
| `fact.source.scanGate`, `fact.source.scanSkip` | with `FOXAPI_SRC_SCAN_GATED`: the name the deferral sentences quote, and the drivers a scan beside the open radios leaves out (`""`: the whole SoapySDR scan is deferred) (section 8.10) | |
| `fact.source.faultedDrivers` | the drivers whose scan child died this session, joined `", "`; `""` none (section 8.10) | |
| `fact.source.plutoUri` | the address in the Pluto box (section 8.10) | LOCAL |
| `fact.source.id.<n>` | the whole `SELECT_SOURCE` id of the `DEVICES` entry whose `id` is `n`: `detail` cuts it at 159 bytes, this does not (section 8.10) | |
| `fact.soundcard.device`, `fact.soundcard.hostApi` | the sound card form's card, whole (section 8.10) | LOCAL |
| `fact.soundcard.span` | `<lo Hz> <hi Hz>` (`%.17g`): what the form's card would receive on the air, through the converter kept for it (section 8.10) | |

## 5. Lists (appended to 7.4)

Every list keeps the 0.2 rules: `read_list` returns the count and fills the
entry when `index < count`; `listSeq` (and `FOXAPI_EVENT_STATE`'s list bit)
says when to read again.

### 5.1 New lists

| List | Entry |
|---|---|
| `STATUS_LINES` (22) | the status lines that have text now. `name`: the line's key; `detail`: its text (whole: `fact.status.<key>`); `value[0]`: level 1 information, 2 warning, 3 error. The keys: `sourceError`, `gpsRefusal`, `catalogError`, `bandPlanError`, `tuneMismatchNote`, `transmitError`, `soundCardMissing`, `sdrPlayApiDetail`, `sdrPlayAdvice`, `recordNotice`, `recordError`, `presetNote`, `pluginEnforceError`, `restoreKeepLabel`, `audioHealth` (8.4: the audio watchdog's note, level 2), `bookmarkNote` (8.6: what the last bookmark import, export or group removal did, level 1), `bookmarkError` (8.6: the bookmark file could not be read or saved, level 3), `installReport` (8.8: what the last plugin install or removal did, level 1), `installError` (8.8: why the last one failed, level 3), `catalogStatus` (8.9: "N plugins in the catalogue", set by every fetch that landed, level 1), `addAllSummary` (8.9: what the last ADD ALL run did, level 1, or 3 when any module in it failed), `deviceFault` (8.10: the radio's fault sentence WHOLE, level 3: the state's `faultMessage` is the same text cut at 127 bytes) |
| `CONVERTERS` (23) | every radio's remembered up/down-converter. `detail`: the radio key (LOCAL sessions only, else empty); `value[0]`: 0 off, 1 up, 2 down; `value[1]`: LO Hz; `value[2]`: 1 inverted; `value[3]`: on the entry that is `ACTIVE` only, **the radio's own centre readback in Hz, read live** (the frequency the radio was told and kept - not the air frequency `centreHz` speaks; a retune moves it without moving `listSeq`; 0 on every other entry; section 8.3); `ACTIVE`: the open radio's |
| `GAIN_MODES` (24) | a radio with gain MODES (an Airspy: Linearity, Sensitivity, Free). `name`; `value[0]`: the mode code `FOXAPP_OP_AIRSPY_GAIN_MODE` takes; `ACTIVE`. Empty for every other radio |
| `DECIMATIONS` (25) | the open radio's decimation factors. `value[0]`: factor; `value[1]`: the rate it gives, Hz; `ACTIVE` |
| `PATCH_NODES` (26) | the patch graph's nodes. `id`: node; `name`; `detail`: kind (`radio`, `channel`, `demod`, `decoder`, `display`, `sink`, `map`); `value[0..1]`: x, y; `value[2]`: Hz; `value[3]`: rate Hz; `ACTIVE`: switched on; `REFUSED`: the engine refused to run it; `FAULT`: its radio faulted or would not open |
| `PATCH_WIRES` (27) | `value[0]`: from node, `[1]`: its output port, `[2]`: to node, `[3]`: its input port |
| `PATCH_PROBLEMS` (28) | what the plan found wrong. `id`: node; `name`: the problem's code, a decimal (the table is in section 8.12; 5 and 14 are advisory); `detail`: what the page says |
| `PATCH_PARTS` (29) | the decoder parts a graph may use. `name`; `detail`: key; `value[0]`: the port type it is fed (0 I/Q, 1 audio, 2 text, 3 control, 4 track); `value[1]`: the rate it needs, Hz; `value[2]`: 1 when the module also has a track source (8.12) |
| `RECORDINGS` (30) | the recorders' takes (current or last), then the patch's files. `name`: file; `detail`: path (LOCAL sessions only); `value[0]`: 1 I/Q, 2 audio, 3 patch; `value[1]`: bytes, **read live** (a growing file moves this without moving `listSeq`); `value[2]`: rate Hz; `value[3]`: the take's length in **seconds**, **read live** like `value[1]` (a take that is running moves it without moving `listSeq`; section 8.2); `ACTIVE` recording; `FULL` at its size limit; `STALLED` writes failing (FULL wins when both) |
| `DECODER_LINES` (31) | the decoder-output backlog, oldest first, at most 500. `id`: line number (1 = the first line since the engine started); `name`: plugin; `detail`: the line (cut to 159 bytes); `value[0]`: the patch decoder node that said it, 0 for the receiver's own decoders (8.12). **A line does not move `listSeq`** (8.7): watch `decoderLinesTotal` (the state's measurement) and read the lines above the last `id` you hold |
| `STORE_BLOCKED` (32) | installed plugins the store's policy refuses to load. `name`: file; `detail`: why |
| `STORE_UPDATES` (33) | the updates the store would install. `name`: catalogue id; `detail`: "from -> to" |
| `BAND_PLANS` (34) | the plans `SET_BAND_PLAN` takes. `name`; `detail`: id; `ACTIVE` |
| `PATCH_RADIOS` (35) | the patch's radios. `id`: node; `name`: device; `detail`: why it faulted or did not open, or for a running radio the note its face prints (section 8.12); `value[0]`: rate Hz; `value[1]`: an open radio's AIR centre Hz (8.12); `value[2]`: 1 while its open is on its worker (8.12); `ACTIVE` open; `FAULT` |
| `MUTE_CAUSES` (36) | the plugins muting the receiver's audio now (8.7). `name`: the plugin; `detail`: its module file name; `value[0]`: `1` when the mute is held after a tune-away; `ACTIVE` on every entry. Empty when nothing mutes |
| `STALE_GRANTS` (37) | the tune grants no loaded module holds (8.7). `name`: the module file name; `REFUSED` when the host holds a record of that file that did not load. In the order the grants were given |
| `PLUGIN_COMMANDS` (38) | the command keys the loaded modules have added (8.8). `id`: the command id (`FOXAPI_OP_PLUGIN_COMMAND`'s `ival[0]`); `name`: the key's label (at most 32 characters, the module's own text); `detail`: the module file name. Module by module in the order of `PLUGINS`, each module's keys in the order it added them (at most 8 a module) |
| `STORE_ACTIVITY` (39) | what the plugin store is doing now (8.9): at most two entries, told apart by `id`, none when nothing is going on. `id` 1: a catalogue fetch or a download is in flight (`name`: the module's name, `""` for the catalogue; `value[0]`: the progress 0..1, **read live**; `value[1]`: 1 fetching the catalogue, 2 downloading); `id` 2: an ADD ALL run is active (`name`: the module moving now; `value[0]`: its position, `value[1]`: how many in the run). `ACTIVE` on both |
| `UNBOUND_RADIOS` (40) | USB radios that are plugged in and that nothing can open (no WinUSB driver bound; 8.10). `name`: the bus description, `""` when none; `detail`: the family, `rtl`, `rx888` or `""`; `value[0]`, `value[1]`: vendor id, product id |
| `SCAN_EVIDENCE` (41) | **LOCAL sessions only (`DENIED` to a REMOTE one)**: where the SoapySDR runtime looked, taken by the engine when a whole scan lands having found nothing (8.10). `value[0]`: 1 a vendor install (`name`: its vendor, `detail`: its root), 2 a module search path, 3 a module loaded (`detail`) |
| `SOUND_CARDS` (42) | the sound card inputs as the last listing found them (8.10). `id`: index + 1; `name`: `device (host API)`; `detail`: the host API; `ACTIVE`: the entry the form's card is |
| `SOUND_CARD_RATES` (43) | the rates the form's card offers in the form's format (8.10). `value[0]`: Hz of the card; `EXCLUSIVE`; `ACTIVE`: the form's rate |
| `SOUND_CARD_FORM` (44) | ONE entry, the sound card form (8.10). `name`: the device, `detail`: its host API (both `""` to a REMOTE session); `value[0]`: rate Hz, `[1]`: I/Q centre Hz, `[2]`: channel (0 left, 1 right); `IQ_STEREO`, `SWAP_IQ`, `PICKED` |
| `PATCH_PLAN` (45) | the plan the patch page draws (8.12): one entry per planned channel, then per planned decoder. `id`: node; `detail`: `channel`, or `decoder:radio` / `decoder:channel` / `decoder:audio`; `name`: a decoder's plugin key; `value[0]`: a channel's offset Hz from its radio's centre, a decoder's rate Hz; `[1]`: a channel's output rate Hz, a decoder's input rate Hz; `[2]`: a channel's decimation, an I/Q decoder's RF centre Hz at DC; `[3]`: the radio node |
| `PATCH_DEVICES` (46) | the devices a patch Radio can be set to, in the order its Device combo lists them (8.12, part 2). `id`: index + 1; `name`: the label; `detail`: the device key (**LOCAL sessions only**, else empty; cut at 159 bytes: `fact.patch.device.<id>` is the whole key); `value[0]`: a recording's own rate Hz, else 0 |
| `PATCH_LIVE` (47) | what the patch's running nodes are doing NOW (8.12, part 2): one entry per node that has something, copied once a pass into a block of its own. **It never moves `listSeq`: read it each time you draw.** `id`: node; `value[2]`: the kind - 1 a demodulator's squelch gate (`value[0]`: the level it judges, dB; `ACTIVE`: open), 2 a speaker's output (`name`: what it plays to; `value[0]`: peak 0..1; `value[1]`: samples written; `detail`: its error), 3 a channel's level (`value[0]`: dB), 4 a decoder's face (`value[0]`: lines said; `detail`: the newest), 5 a speaker whose output could not be made (`detail`: why), 6 a picture decoder's newest picture (`value[0]`: the image id `read_image` takes; `value[1]`: its bytes; `name`: the node's name; `ACTIVE`: finished) - 8.12.7 |
| `GPS` (48) | ONE entry, the GPS reader as the position row draws it (8.13). **Read at the call and never part of the lists' block: it never moves `listSeq`, so read it each time you draw.** `name`: the port given to the last start, `""` when the reader is idle (**LOCAL sessions only**); `detail`: the status line the engine words (`""` when idle; **LOCAL sessions only** - the sentence names the port; cut at 159 bytes); `value[0]`: the state (0 idle, 1 listening, 2 fixed, 3 failed, 4 timed out); `value[1]`: seconds since the start (live while it listens, frozen when it ends); `value[2]`: satellites in view, -1 until a sentence says; `value[3]`: the baud; `ACTIVE`: listening; `FAULT`: failed or timed out. Never a coordinate |
| `SERVERS` (49) | **LOCAL sessions only (`DENIED` to a REMOTE one).** The web server and the CAT listener the engine runs for `FOXAPI_OP_SERVER_CONFIG` (8.16.5): always two entries, web first. `id` 1 / 2; `name`: `web` / `cat`; `detail`: the sentence the last start was refused with, `""` when none; `value[0]`: the port (the one listened on, else the configured one); `value[1]`: browser sessions / CAT clients, `value[2]`: browsers listening to the audio, `value[3]`: commands received - all three **read at the call**; `ACTIVE`: listening; `FAULT`: enabled and refused to start. The configuration is `fact.web.*` and `fact.cat.*` |
| `STORE_PICTURES` (50) | the plugin store's pictures, every one it was asked for (`FOXAPP_OP_STORE_PICTURES_WANT`) and still knows, in sha256 order (9.4). `id`: the first 64 bits of the sha256; `name`: the sha256 (64 hex digits - the cache key; the entry names its pictures in `fact.store.<key>.pictures`); `detail`: a Ready one's cached file path (**LOCAL sessions only**; whole in `fact.store.picture.<sha256>.path`), a Failed one's reason cut at 159 bytes (whole in `.reason`); `value[0]`: 1 pending (queued or in flight), 2 ready, 3 failed; `ACTIVE`: the one in flight. Moves `listSeq` with every change of a picture's state |
| `AIRBAND_CHANNELS` (51) | the channels the Airband monitor plays (9.6): one entry per channel of the plan - not only the block on the air - in frequency order, and none while it is not listening. **Read at the call and never part of the lists' block: it never moves `listSeq`, so read it each time you draw** (`PATCH_LIVE`'s rule: a channel's level and heard time change every pass). `id`: the channel's number in the plan, from 1; `name`: the row that names it (the first ticked row on that frequency); `value[0]`: the frequency, Hz; `value[1]`: the channel width, Hz; `value[2]`: the level its squelch judges, dB (-200 until it has reported one); `value[3]`: the seconds heard since the monitor last wrote them to the list (add them to `BOOKMARKS`' `value[3]` for the figure to show); `ACTIVE`: its squelch is open now; `ON_AIR`: it is in the block on the air (none is, before the first block has been tuned - with several blocks that waits for the scan's first tick) |
| `PLUGIN_SETTINGS` (52) | **LOCAL sessions only (`DENIED` to a REMOTE one).** The fields of the settings forms the loaded modules declare (`CASCADE_CAP_SETTINGS_UI`, 9.7), one entry per field the host can store, a module's fields together and in its own order. `id`: the entry's number in this list, from 1 (what `fact.plugin.setting.<id>` is read by; it moves when the modules do); `name`: the field's label (the plugin's own text); `detail`: the module file name (`PLUGINS`' detail); `value[0]`: the kind (0 TEXT, 1 NUMBER, 2 BOOL, 3 PATH), `value[1]`: the longest value in UTF-8 bytes. The field's key and placeholder are the fact `fact.plugin.setting.<id>.spec`, its value the fact `fact.plugin.setting.<id>`. The list moves `listSeq` when a field's value changes, whoever changed it |

### 5.2 0.2 lists now served

`DECODERS` (`name`; `detail`: why idle, "" running; `ACTIVE` running;
`FED` fed signal now), `CATALOGUE` (`name`; `detail`: id; `INSTALLED`,
`COMPATIBLE`; 0.3 (8.9): `value[0]` the ABI version the entry's build declares,
`value[1]` the size of the build for this machine, `BUILD_HERE`), `TX_DEVICES` (one entry: `detail` is the address `TX_OPEN`
will be given; `ACTIVE` open), `MARKERS` (`id`; `name`: label; `detail`:
plugin; `value[0..2]`: Hz, width, colour), `BAND_PLAN` (`name`; `detail`:
service; `value[0..2]`: start, stop, colour), `PRESETS` and `USER_PRESETS`
(`name`: label; `detail`: plugin; `value[0..2]`: Hz, mode, bandwidth),
`DEVICE_OPTIONS` (`name`: the `SET_DEVICE_OPTION` name; `ACTIVE` on; only the
ones the open radio has), `SERIAL_PORTS` (`name`; filled by
`SCAN_SERIAL_PORTS`). `TRACKS` stays **GAP** (`UNSUPPORTED`): map tracks come
through the map page's plugin APIs, which are not served yet.

`BOOKMARKS` (0.2: `id`, `name`, `detail` the group, `value[0..2]` Hz, mode, bandwidth, `FAVOURITE`) gains in 0.3 (9.6) `value[3]`, the seconds the Airband monitor has
heard the row talking (kept with the list, "which ones are busy"), and `FOXAPI_ITEM_TICKED`, the row's tick.

`DEVICES` (0.2: the generator and the scanned radios; `name`: label, `detail`: the id `SELECT_SOURCE` takes, `ACTIVE`: the source in use) is **completed** in 0.3 (8.10): the IQ file and the sound card are entries 2 and 3, so an entry's index is its row in the Source combo and every radio's entry moved down two places; the whole id is `fact.source.id.<n>` where `detail` has cut it. `SAMPLE_RATES` (0.2: `value[0]`, Hz) carries the rate's caption in `detail` (8.10).

In 0.3 (8.7) `PLUGINS` carries `value[0]`, the plugin's capability word, and `STOPPED`, `TUNE_ALLOWED`
and `SETTINGS_ALLOWED`, and (8.8) `value[1]`, the module file's size in bytes, `value[2]`, its notice level, and
`MATCHED`, `ASKED_SETTINGS` and `DIGEST_MISMATCH`, and (9.3) `value[3]`, how the plugin runs; `DECODERS` carries `MUTES`; `PRESETS` carries `DEVICE_CENTRE`; `MARKERS` carries
`value[2]`, the colour, and `SPAN`, `DASHED` and `SPECTRUM_ONLY`.

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

Section 8.7 adds eight, each on the list named: `FOXAPI_ITEM_MUTES` 0x400 (`DECODERS`:
"mute audio while running" is on), `TUNE_ALLOWED` 0x800 and `SETTINGS_ALLOWED` 0x1000
(`PLUGINS`: the module is granted control of the tuning / the settings), `STOPPED` 0x2000
(`PLUGINS`: a loaded plugin the user stopped), `DEVICE_CENTRE` 0x4000 (`PRESETS`: moves the
radio's centre, not the VFO), `SPAN` 0x8000, `DASHED` 0x10000 and `SPECTRUM_ONLY` 0x20000
(`MARKERS`: a shaded band, drawn dashed, kept off the waterfall).

Section 8.8 adds three, all on `PLUGINS`: `FOXAPI_ITEM_MATCHED` 0x40000 (the runner feeds the module, or
would with the receiver running; any kind of plugin), `ASKED_SETTINGS` 0x80000 (the module has asked for
something only the settings grant allows) and `DIGEST_MISMATCH` 0x100000 (the file is not the one that was
installed).

Section 8.9 adds one, on `CATALOGUE`: `FOXAPI_ITEM_BUILD_HERE` 0x200000 (the catalogue publishes a build for
this machine's os and architecture). This is the next free bit of the **item** flags; the `0x00200000` of
`FOXAPI_RX_WEB_LISTENING` is a bit of the state's flag word, a different namespace.

Section 8.10 adds four: `FOXAPI_ITEM_EXCLUSIVE` 0x400000 (`SOUND_CARD_RATES`: a rate only WASAPI exclusive mode reaches), and on `SOUND_CARD_FORM` `FOXAPI_ITEM_IQ_STEREO` 0x800000, `SWAP_IQ` 0x1000000 and `PICKED` 0x2000000.

Section 9.6 adds two: `FOXAPI_ITEM_TICKED` 0x4000000 (`BOOKMARKS`: the row is ticked - the scanner's list mode scans it and, when its mode is exactly AM or NFM, the
Airband monitor plays it) and `FOXAPI_ITEM_ON_AIR` 0x8000000 (`AIRBAND_CHANNELS`: the channel is in the block on the air). Four item-flag bits are left (0x10000000 to 0x80000000).

## 6. Scope taps — `read_scope` (closes 0.2 gap 5)

`read_scope(session, tap, info, out, capFrames)` copies the NEWEST
`capFrames` frames of a demod-scope tap, oldest first, and returns how many it
copied. It is a SNAPSHOT, not a stream: an interface draws the last N and
asks again next frame. Taps: `FOXAPI_SCOPE_AUDIO` (the finished audio, mono,
48 kHz), `FOXAPI_SCOPE_IQ` (the channel's I/Q, two floats a frame, at the
channel rate), `FOXAPI_SCOPE_MPX` (the WFM multiplex, only while WFM runs).
`info` (may be NULL; shorter than `FOXAPI_MIN_SCOPE_INFO` is BAD_ARGUMENT)
carries `channels`, `rateHz`, `written` — frames the tap has taken in
total, so a reader tells a silent tap from a stalled one - and `capacityFrames`
(section 8.11): the most frames a call on THIS session can return for the tap.
A LOCAL session is given whatever the ring holds (65536 frames of audio,
262144 of I/Q, 65536 of multiplex; the call is bounded by the ring and the
caller's `capFrames`, nothing else). A REMOTE session is given at most
`FOXAPI_SCOPE_REMOTE_FRAMES` (4096) frames: "reduced", as `FOXAPI_CAP_SCOPE`
always said (and `capacityFrames` says so: `min(ring, 4096)`). The return value
is an `int32_t` that is NEGATIVE on error (`DETACHED`, `BAD_ARGUMENT`,
`UNSUPPORTED`): a client must test it before it uses it as a count. `info`'s
`structSize` must be set, or the whole call is `BAD_ARGUMENT`. An unknown tap
is `UNSUPPORTED`. The read never waits: the tap is a lock-free
single-producer ring the DSP thread pushes and any thread may snapshot; the
frames and `written` are read AT THE CALL, not taken from the published state,
so a waveform is never a publish old.

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
| `FOXAPI_RX_POSITION_SET` 0x40000000 | (seq only) | the receiver's position is known: `rxLatDeg` and `rxLonDeg` are valid (section 8.11). The next free bit; `0x80000000` is the one left |

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
them again only when the plan changed - `listSeq` also moves for every other
list change, so it is not the plan's own signal: the number of entries, the
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
  entries. `listSeq` moves for far more than the store - a gain, a status line, a
  preset note - so an interface that copies the rows when `listSeq` moves
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

### 8.6 The recorder, the bookmarks and the scanner as their sections draw them

The Recorder, Bookmarks and Scanner sections read the engine's table only. 0.3 had
most of it: the state's flags (`FOXAPI_RX_RECORDING_IQ`, `FOXAPI_RX_RECORDING_AUDIO`,
`FOXAPI_RX_SCANNER_ACTIVE`, `FOXAPI_RX_RUNNING`), `audioMutedBy`, the `RECORDINGS` list
and `fact.recorder.dir`, the `BOOKMARKS` list with `fact.bookmarks.version`, and every
command the sections send. This adds the scanner's readout, two status lines and one op.
Appended after `rdsRadioText`; **`FOXAPI_MIN_RECEIVER_STATE` moves to the end of
`scanListenMs`** (`FoxReceiverState` is 64 bytes larger).

**The scanner** (S1):

| Member | Group | |
|---|---|---|
| `scanSeq` | | advances when any of the seven settings below changed; `seq` moves with it |
| `scanStartHz`, `scanStopHz`, `scanStepHz` | `scanSeq` | the range a Start would scan, **as stored** and in Hz: what `FOXAPP_OP_SCANNER_RANGE` and `FOXAPI_OP_SCANNER_RUN` wrote, times the same factor the engine reads them back with (a value that is not exact in megahertz reads back as the engine's product). A reversed range is published reversed; `Scanner::configure` swaps it when the scan runs |
| `scanDwellMs`, `scanHoldMs`, `scanResumeMs`, `scanListenMs` | `scanSeq` | the four timings of `FOXAPI_OP_SCANNER_CONFIG`, in **whole milliseconds**: rounded to the nearest (a half goes up), 0 for a negative (the scanner floors a negative at 0 as well), saturating at 4294967295. The engine keeps what it was sent; this is how it is published |
| `scannerState` | `seq` only | `FOXAPI_SCANNER_IDLE` 0 (not scanning), `FOXAPI_SCANNER_SCANNING` 1 (stepping one point each dwell), `FOXAPI_SCANNER_PAUSED` 2 (a signal was found: held on it, listening), `FOXAPI_SCANNER_HOLDING` 3 (the signal went quiet: waiting `scanHoldMs` for it to come back). The same words `/api/status` names (`idle`, `scanning`, `paused`, `holding`). `FOXAPI_RX_SCANNER_ACTIVE` is set exactly when this is not `IDLE` |
| `scanCurrentHz` | `seq` only | where the scanner is: the lattice point it last emitted. After a stop it keeps reporting where the scan stopped |

Which counter each moves: the **settings** are a group of their own, since a form that
shows them re-reads them when they change and at no other time; they move `scanSeq` and
`seq`. `scannerState` and `scanCurrentHz` are live state: a scan steps at every dwell, so
they move `seq` and no group counter, as the scanner's flag does; the receiver's tuning
moves `tuneSeq` (and `txSeq`, where the transmitter follows the dial) with the scan, as
for any retune. A scan running does not move `scanSeq`. Nothing allocates: plain numbers
in the state.

A command through the table lands at the engine's next pass, and the state shows it after
that pass's publish. A form that edits the range or a timing and then reads the state back
sees its own edit one pass later.

**The Bookmarks section's two lines** (S2): `bookmarkNote` (level 1, information) and
`bookmarkError` (level 3, error), in `STATUS_LINES` (section 5.1), as `fact.status.bookmarkNote`
and `fact.status.bookmarkError` (whole), and in the engine's handed-over copy of the lines.
`bookmarkNote` is what the last import, export or group removal did ("Exported 12 to
C:/.../foxsdr-frequencies-20261014-091500.xml", "Removed 3 from \"Airband\"", "Could not
import: ..."); `bookmarkError` is the bookmark file's own problem (it could not be read, or
could not be saved). Both move `listSeq` with every other status line.

**The bookmark copy and its groups** (S4). `BOOKMARKS` is copied when `fact.bookmarks.version`
moves and not when `listSeq` does (section 8.5); the rows carry what the section needs (`id`;
`name`; `detail`, the group, cut at 159 bytes; `value[0]` Hz; `value[1]` the mode code;
`value[2]` the bandwidth; `FAVOURITE`). The group names the section offers are **derived from
that copy** - the sorted, distinct non-empty `detail` strings - so they cannot disagree with the
rows they filter. `BOOKMARK_GROUPS` (21) is served as it was (sorted by name, with counts), built
from the published block on each call, which is O(bookmarks) per entry read; an interface that
wants the groups of a long list does not read it.

**`FOXAPP_OP_BOOKMARK_EXPORT`** (S3, section 9): writes bookmarks as SDR# frequencies.xml.

**The recorder.** The section reads the two takes from `RECORDINGS` (kind 1 and 2): the
bytes (`value[1]`) and the seconds (`value[3]`) are read live; the samples it letters are the
bytes over the bytes a frame takes (an I/Q frame is eight bytes: two 32-bit floats; an audio
frame is two: one 16-bit sample), which is what the recorder counts a frame as. "Muted by ..."
under an audio take is `audioMutedBy` while the audio is hard-muted (the user's mute or a
plugin's, or a keyed transmitter that is not monitored: section 8.5).

### 8.7 The decoders, the presets, the marks, the plugin-mute cluster and the tune grants as their sections draw them

The Decoders rail section, the Decoder output window, the preset bars, the marks on the
spectrum, the "sound is muted" banner and dialog, and the Receiver control rows read the
engine's table only. 0.3 had most of it (`DECODERS`, `PLUGINS`, `PRESETS`, `USER_PRESETS`,
`MARKERS`, `DECODER_LINES`, `decodersRunning`, `decodersFitted`, `decodersMatched`,
`decoderLinesTotal`, `audioMutedBy`, and every command the surfaces send); this adds what it
lacked. **No member of `FoxReceiverState` is added, no op, and no struct changes.**

**The plugin-mute cluster** (D1). While the receiver sits on a muting plugin's preset the audio
is muted for that plugin; tuned away from it with the plugin still running, the mute is HELD
(the engine keeps the audio down until the operator answers) and the engine asks. The cluster
is the list, two facts and an event:

| | |
|---|---|
| `MUTE_CAUSES` (36) | one entry for each plugin muting the receiver's audio now, in the order `audioMutedBy` names them. `name`: the plugin; `detail`: its **module file name** (what `FOXAPP_OP_DECODER_STOP_LIST` takes); `value[0]`: `1` when the mute is **held after a tune-away** (the plugin keeps decoding while the audio stays down), `0` while the receiver is on its preset; `ACTIVE` on every entry (an entry is listed because it is muting). Empty when nothing mutes. The user's own mute is `FOXAPI_RX_MUTED` and is not here |
| `fact.mute.subject` | the same sentence as `audioMutedBy` ("A", "A and B", "A, B and C"), **whole**: the state's text is cut at 127 bytes |
| `fact.mute.keptRunning` | `1` while the mute is held after a tune-away and the operator has not stopped the plugins, `0` otherwise. The banner an interface keeps up is exactly this |
| `FOXAPI_EVENT_MUTE_TUNE_AWAY` (11) | the receiver tuned off the presets of plugins that are still running and muting, and the engine is asking whether to stop them. `value[0]`: how many plugins (`MUTE_CAUSES` names them and carries `value[0] = 1`). **Once for each tune-away** - arriving on a preset re-arms it - and never raised for a plugin that stopped first or for the user stopping one. It is raised by the **publish that shows the held mute**, which is the pass after the one that decided it (the engine decides the mute after a pass's publish), so when an interface receives it the list already names the plugins and `fact.mute.keptRunning` already reads `1` |

An interface that asks the question captures the names and keys from `MUTE_CAUSES` when the
event arrives and keeps them: the dialog must not re-aim itself at plugins that muted later. It
is **withdrawn** when `fact.mute.keptRunning` reads `0` again (the engine lets the hold go
exactly when it withdraws its own question: the receiver is back on a preset, or the plugins
named were stopped). "Keep it running" (`FOXAPP_OP_MUTE_KEEP_RUNNING`) leaves the hold in
place and the question closed; "Stop ... and resume sound" is `FOXAPP_OP_DECODER_STOP_LIST`
with `ival[0] = 1` and the captured keys. `mutePopupQueued_`, the flag a window used to
consume, stays in the engine and nothing in the table reads it.

**The decoders and plugins** (D2). `DECODERS` (section 5.2) gains `FOXAPI_ITEM_MUTES`: the
plugin's effective "mute audio while running" setting (the default its capabilities imply -
on for an I/Q decoder, off for an audio decoder - or the operator's override of it).
`PLUGINS` gains, for **every** record the host holds (loaded or not):

| | |
|---|---|
| `value[0]` | the plugin's **capability word** as it declared it, `CASCADE_CAP_*` of `src/core/plugin_abi.h` (`DECODER` 0x1, `IQ_DECODER` 0x2, `IMAGE_DECODER` 0x4, `TRACK_SOURCE` 0x8, `PANEL` 0x10, `HOST_CLIENT` 0x20, `PRESET` 0x40, `BASEMAP` 0x80, `TRACK_INFO` 0x100, `INSTRUMENT` 0x200, `AUDIO_OUT` 0x400, `AUDIO_PROCESSOR` 0x800). 0 for a file refused before a descriptor was accepted; for a duplicate the host turned off, the word it declared. For a LOADED plugin a declared bit has its table behind it (the host proves it), so the word says what the plugin offers |
| `FOXAPI_ITEM_STOPPED` | a loaded plugin the user stopped (any plugin; a decoder's stop also shows as `DECODERS`' `ACTIVE` clearing) |
| `FOXAPI_ITEM_TUNE_ALLOWED`, `FOXAPI_ITEM_SETTINGS_ALLOWED` | the plugin's module file is granted control of the receiver's tuning / the radio's settings (`FOXAPI_OP_PLUGIN_GRANT`, `ival[0]` 1 / 2, `ival[1]` on / off, `text` the module file name). The grants the config holds, which the plugin host mirrors |

`STALE_GRANTS` (37) is the half a flag on `PLUGINS` cannot carry: a grant is held for a module
file, and the file may be gone. One entry for each **tune** grant no loaded module holds, in
the order the grants were given: `name`: the module file name; `FOXAPI_ITEM_REFUSED` when the
host holds a record of that file and it did not load (the file is on the disk and was refused),
clear when no file of that name is fitted at all. An interface shows them so a permission
nobody can see is one somebody can take back - toggling is the same `FOXAPI_OP_PLUGIN_GRANT`.
`fact.plugin.lastDenied` is the module file name of the last plugin whose request to tune the
receiver was refused ("" none; a rescan clears it): the key the grant is held under, and so the
key to look up in `PLUGINS`.

**The presets and the marks** (from the survey). `PRESETS` gains `FOXAPI_ITEM_DEVICE_CENTRE`:
the preset moves the radio's centre rather than the VFO, and is "already tuned to" by that
comparison (|centre - frequency| within half the radio's rate) and not by the tuned frequency's
(within the larger of half the bandwidth and 5 kHz). `id` of a `PRESETS` entry is the
plugin's own RAW index (what `FOXAPI_OP_PLUGIN_PRESET` takes); the host drops a preset whose
frequency is not a frequency, so the ids may skip. `detail` is the plugin's display name; an
empty `name` is a preset with no label of its own, which an interface letters with the plugin's
name. `USER_PRESETS`' `detail` is the version-stripped module id the operator's own presets are
filed under (`core::userPresetKey` of the module file name), and the position among one
plugin's entries is the ordinal `FOXAPP_OP_USER_PRESET_APPLY` and `_FORGET_AT` take.
`MARKERS` gains `FOXAPI_ITEM_SPAN` (a shaded band from `value[0]` to `value[0] + value[1]`,
not a line), `FOXAPI_ITEM_DASHED` and `FOXAPI_ITEM_SPECTRUM_ONLY` (not drawn on the waterfall);
`value[2]` is the colour, 0xRRGGBBAA, 0 meaning the host's own.

**The decoder lines** (D3). `DECODER_LINES` NO LONGER MOVES `listSeq`. A decoder that prints
tens of lines a second (ADS-B) used to move it once a line, and every interface that keeps a
cache on `listSeq` read every list it keeps again, once a line. A line is a measurement:
`decoderLinesTotal` climbs and nothing else moves. An interface that shows the backlog watches
`decoderLinesTotal` and, when it has moved, reads `DECODER_LINES` from the end back to the last
`id` it holds (the ids are the line numbers, so only the new lines are read; the list is a
ring of 500, and an interface that falls more than 500 behind has lost the older ones). A
line is cut to the item's 159 bytes. The lines the track-info plugin writes ("Aircraft info":
why it cannot reach its registry) are counted like every other; they were not before, and so
never reached the backlog. (Nor were a plugin's own `log()` lines, found beside the fitted-modules
window: 8.8.)

**Timing.** A command through the table lands at the engine's next pass and the lists show it
after that pass's publish: the mute setting, a stop or start, a grant, a preset's effect on
where the receiver sits. An interface whose control reads such a thing back sees its own change
one pass later; one that toggles a local copy at the click and re-reads the list the frame after
draws the engine's value from then on. `PLUGINS`, `DECODERS`, `PRESETS`, `USER_PRESETS`,
`MARKERS`, `MUTE_CAUSES` and `STALE_GRANTS` move `listSeq` when they change. Nothing here
allocates in the pass: the module keys are the cached ones, the mute cluster's names are hashed
in place, and the block is rebuilt only when the fingerprint moves.

### 8.8 The fitted-modules window as it draws it

The Fitted modules window (the rail's FITTED MODULES key; one plate for each plugin file the host
holds, with its state, its keys and everything the module and the host have to say about it) reads
the engine's table only. 0.3 had most of it: the state's `FOXAPI_RX_RUNNING`, `PLUGINS` (the display
name, the module file name in `detail`, the capability word in `value[0]`, `ACTIVE` loaded, and the flags
`STOPPED`, `TUNE_ALLOWED`, `SETTINGS_ALLOWED`: 8.7), and every command the window sends
(`FOXAPI_OP_DECODER_START` / `_STOP`, `FOXAPI_OP_PLUGIN_RESCAN`, `FOXAPI_OP_STORE_REMOVE` (ADMIN, like the
rest of the store's), `FOXAPI_OP_PLUGIN_GRANT` with `ival[0]` 1 and 2, `FOXAPI_OP_PLUGIN_COMMAND`: all
named by the module file name). This adds what it lacked. **No member of `FoxReceiverState` is added, no
op, and no struct changes.**

**Why most of it is a fact and not a column.** The window quotes the host and the modules, it does not
paraphrase them: a module's own words (its author, its licence, its warning of up to 512 bytes), the
host's refusal of a file, the runner's sentence about why a module is not being fed, the install
record's finding about a file. A list item's `detail` cuts at 159 bytes, so each is a **fact** in the
sense of section 4: the whole text, `""` when there is nothing to say, and the module file name in the
key. What is a number or a state is a column or a flag on `PLUGINS`, where the window already reads
every module's row.

**The per-file facts.** `<file>` is the module file name exactly as `PLUGINS` names it in `detail`
(the key the grants, the stop set and the command keys are filed under). Every key below is present
for every record the host holds, loaded or not, so "known and empty" (`""`, length 0) is told apart from
a file nobody holds (`NOT_FOUND`).

| Key | Value | |
|---|---|---|
| `fact.plugin.dir` | the folder the engine loads plugins from | LOCAL |
| `fact.plugin.<file>.path` | the absolute path the module was loaded (or refused) from | LOCAL |
| `fact.plugin.<file>.version`, `.author`, `.licence` | the descriptor's own strings, copied only once the host accepted a descriptor: `""` for a file refused before that (the window must not draw a maker, a version and "no licence declared" for a module nobody opened). A duplicate the host turned off after reading it keeps them | |
| `fact.plugin.<file>.error` | the host's reason for refusing the file, verbatim; `""` when it loaded. For a duplicate it names the file that was kept instead | |
| `fact.plugin.<file>.idle` | the runner's own sentence for the FIRST of the module's status rows that is not running ("... failed to start its audio processor ...", the rate it needs against the rate it is given, "... is not being fed"), whole and unedited; `""` for a module that is fed, stopped (a stopped plugin has no rows) or has none. Any kind of plugin: a decoder, an I/Q or image decoder, an audio processor | |
| `fact.plugin.<file>.notice` | the newest WARN or ERROR the module logged (at most 512 bytes: `CASCADE_LOG_TEXT_BYTES`), whole; its level is `PLUGINS` `value[2]`. Kept for the session | |
| `fact.plugin.<file>.integrity` | when `FOXAPI_ITEM_DIGEST_MISMATCH` is set, the host's sentence ("... is not the file that was installed for ...: its bytes no longer match the sha256 recorded when it was installed ..."), about 220 bytes of English; else `""`. The HOST's finding, drawn apart from `.notice` so a module cannot overwrite it by logging | |

A module file name can have dots in it (`group.nine.v2.dll`), and no leaf does, so **an interface that
is handed a key parses the leaf from the RIGHT**: the leaf is what follows the last dot, the file is
everything between `fact.plugin.` and it. (An interface that builds the key from a `PLUGINS` entry does
not parse at all, which is what the window does, once for each facts refresh.) The keys do not collide
with `fact.plugin.lastDenied` and `fact.plugin.dir`, which have no leaf and are not files: `NOT_FOUND`
is the answer for `fact.plugin.dir.path`, for a key with no leaf, and for a leaf this list does not
have. A REMOTE session reads every one of these but the two LOCAL paths (`DENIED`); a file nobody holds
is `NOT_FOUND` to it too, not `DENIED`.

**`PLUGINS` gains**, for every record (as 8.7's did):

| | |
|---|---|
| `value[1]` | the module file's **size in bytes**, measured by the **engine** when the list block is built (the path is the engine machine's, so the interface never stats it); `0` when it could not be measured. It is a stat each time the block is rebuilt (a rescan, and anything else that moves `listSeq`) and never a stat a pass: a file that grows behind the engine's back says so at the next rebuild. The plate draws `0` as "not measured", never as an empty file |
| `value[2]` | the level of the module's newest WARN or ERROR: `0` none, `2` warning (`CASCADE_LOG_WARN`), `3` error (`CASCADE_LOG_ERROR`); the text is `fact.plugin.<file>.notice`. An INFO line is a decoder-output line and not a notice |
| `FOXAPI_ITEM_MATCHED` 0x40000 | the runner holds an instance for the module **and it is matched to the rate the chain delivers**: `PluginRunner::isFeeding`, the engine's answer to "is it being fed, or would it be with the receiver running". **Whether or not the receiver runs, and for ANY kind of plugin** - an in-chain audio processor has no `DECODERS` entry to say so. `DECODERS`' `FED` is this and the receiver running, for a decoder; an interface letters FED from `MATCHED` and `FOXAPI_RX_RUNNING`, and "matched, start the receiver" from `MATCHED` alone. A stopped module is not matched (it has no instance); one that could not start or wants another rate is not, and `.idle` says why |
| `FOXAPI_ITEM_ASKED_SETTINGS` 0x80000 | the module has asked, at least once, for something only the settings grant allows (a mode, a bandwidth, the squelch, the rate, a gain, the AGC, running or stopping the receiver, the volume, the mute): the window offers its REVOKE / GRANT RADIO SETTINGS key only to a module that has. Recorded even when the request was refused, and **sticky for as long as the engine lives** - a stop, a grant and its revocation and a rescan do not clear it. A request that needs the TUNE grant is not a request for a setting, and a stopped module is refused before it is recorded as asking |
| `FOXAPI_ITEM_DIGEST_MISMATCH` 0x100000 | the engine's install record for the file says its bytes are no longer the ones verified at install (`InstalledPlugin::digestMismatch`, set when a rescan re-hashes the folder). The record is matched by file name, ignoring case; a module fitted by hand has no record and is never flagged. The sentence is `.integrity` |

**`PLUGIN_COMMANDS` (38)** is the command keys the loaded modules have added (`CascadeHostApi::add_command`;
at most 8 a module, labels of at most 32 characters). One entry for each key, module by module in the
order of `PLUGINS` and each module's keys in the order it added them. `id`: the command id, which is what
`FOXAPI_OP_PLUGIN_COMMAND` takes in `ival[0]`; `name`: the key's label, the module's own text, drawn as
given; `detail`: the module file name, the `text` of the same op (and what an interface groups the keys by).
A press is a command that lands at the next pass and is queued for the module's own `poll_command`; nothing of
the module runs in the engine's pass, and an id the module no longer lists is `NOT_FOUND` ("no such command
key"). A module's keys go when it is stopped (at once, not at the next rebuild) or unloaded, and a module that
is started or reloaded adds them again as it attaches. Not LOCAL-only: a key is not a path on this machine.

**The install outcome.** `installReport` (level 1, information) and `installError` (level 3, error) join
`STATUS_LINES` (section 5.1), as `fact.status.installReport` and `fact.status.installError` (whole) and in
the engine's handed-over copy of the lines, as group 7's `bookmarkNote` did. `installReport` is what the
last install or removal of a plugin did ("Removed zz_refused.dll"); `installError` is why the last one
failed, verbatim ("... is not an installed plugin file"). Neither clears the other except as the engine
does: an install or a remove clears both before it starts, and a failed one sets only the error. The store
window reads the same pair, so the fitted-modules window draws them only in a frame in which the store did
not.

**How the window reads it** (what step B of the port holds the engine to):

| The window draws | From |
|---|---|
| the folder | `fact.plugin.dir` |
| the receiver's state | `FOXAPI_RX_RUNNING` |
| a module's file, display name, capability word, loaded | `PLUGINS` `detail`, `name`, `value[0]`, `ACTIVE`. A file refused before a descriptor was read has `value[0]` 0 and `name` equal to `detail`: no name read |
| a module's host-client key (`tuneCapable`) | `value[0]` has `CASCADE_CAP_HOST_CLIENT`, for a loaded module (the host refuses a module that declares the bit with no usable table) |
| stopped, tune grant, settings grant | `STOPPED`, `TUNE_ALLOWED`, `SETTINGS_ALLOWED` |
| fed, and "matched, start the receiver" | `MATCHED` with `FOXAPI_RX_RUNNING` |
| why it is not fed | `.idle` |
| whether to offer the settings grant | `ASKED_SETTINGS` |
| its command keys | the `PLUGIN_COMMANDS` entries whose `detail` is the file |
| its warning | `value[2]` and `.notice` |
| its integrity finding | `DIGEST_MISMATCH` and `.integrity` |
| its size | `value[1]` |
| its path, version, author, licence, refusal | the five facts |
| the green and red lines under the strip | `fact.status.installReport` / `installError` (or the lines list) |

**A correction beside this window (F7).** A plugin's own `log()` lines (INFO, WARN and ERROR) are decoder-output
lines, one for each line of the text (a text with several lines is several; an empty part is none). They went
to the backlog without being counted in `decoderLinesTotal`, so an interface that reads the backlog when the
total moves (8.7, D3) never saw them, and the ring - which takes "the lines the count says are new" - numbered
and sliced the lines around them wrongly. They are counted now, one for each line pushed.

**Timing.** A command through the table lands at the engine's next pass and the lists show it after that
pass's publish: a stop or start (the row's state, `MATCHED`, `value[2]`'s plate and the keys of the module),
a rescan (the whole list is replaced: an interface keys what it remembers about a module, such as a Remove
that is armed, on the file name and not on a position), a grant, a removal. A removal is the one that gives
two things at once: the record leaves `PLUGINS` and `installReport` or `installError` is set **in the same
publish**, so an interface that reads the lines from the engine's handed-over copy shows the sentence the
frame the row goes, and not one frame later. A press of a command key has no visible effect of its own.

**What moves `listSeq`, beyond 8.1 to 8.7.** Everything new is in the list fingerprint: a module's
size, descriptor strings and refusal (they change only across a rescan, which is hashed itself, and are
hashed as well, so a refused file replaced by another refused file keeps no stale reason), the plugins
folder, a module's newest notice **by level and text** (the same warning said again moves nothing, so a
module that repeats one every pass does not make an interface re-read every list: group 8's D3 would
otherwise return here; a different text, or the same text at another level, does), `MATCHED` for every
kind of plugin (the runner's matched keys are now hashed whether or not the receiver runs, where 8.7 hashed
them only while it did), the generation of the runner's status rows (so a module's idle sentence that
changes with the same matched set is no longer stale, and nor is `DECODERS`' `detail`), the first time a
module asks for a setting, the install records' verdicts (a doctored record counts), a change of any module's
command keys (one added, relabelled or removed; the same key said again is no change), and the two status
lines. A decoder line is still not one. **Nothing here allocates in a pass:** the module keys are the
cached ones, the notices are hashed by level and text in place, and the plugin API and the runner each
keep one counter for the two answers that would otherwise build a vector or copy every row per call
(`PluginApiCore::settingsRequestersSeq`, `commandsSeq`, `PluginRunner::statusGeneration`); the block, its
facts and the files' sizes are built only when the fingerprint moves.

### 8.9 The plugin store as its pages draw it

The Plugin store window (the rail's STORE key, its chip and its row of retired modules; one plate for each
catalogue entry, its install key, ADD ALL, UPDATE, and the line that says what it is doing) reads the engine's
table only. 0.3 had most of it: `CATALOGUE` (the display name in `name`, the catalogue id in `detail`, `INSTALLED`,
`COMPATIBLE`), `fact.store.<id>.{legal, summary, description, homepage, version, blocked}`, `STORE_BLOCKED` and
`STORE_UPDATES`, `PLUGINS` and the facts of 8.8 for the host's side of a module that is fitted,
`STATUS_LINES` `catalogError`, `pluginEnforceError`, `installReport` and `installError`, and every command the
window sends (`FOXAPI_OP_STORE_FETCH`, `_INSTALL`, `_CANCEL`, `FOXAPP_OP_SET_CATALOGUE_URL`, `_STORE_UPDATE`,
`_STORE_REMOVE_BLOCKED`, `_STORE_ADD_ALL`: all ADMIN, the four app ops LOCAL only). This adds what it lacked.
**No member of `FoxReceiverState` is added, no op, and no struct changes.**

**Downloading and the queue are the engine's.** The catalogue fetch, each download and ADD ALL's queue run in the
engine, on workers it starts and polls (a result lands at the pass after the one that started it, see Timing);
the table only reports them. Nothing in this section moves to the interface, and the interface has nothing to
poll but the lists.

**A catalogue entry's columns.** `CATALOGUE` gains, for every entry:

| | |
|---|---|
| `value[0]` | the ABI version the entry's build declares (`abiVersion` of the index; 0 when the index says none). `COMPATIBLE` stays "equals this engine's"; the number is for a plate that letters "built for 3, this is 4". The engine's own number is not a member of any table: an interface takes it from `plugin_abi.h` (`CASCADE_PLUGIN_ABI_VERSION`) in the build it is compiled with, as the window does |
| `value[1]` | the size in bytes of the build for THIS machine's os and architecture, as the index states it (advisory: the download is capped and hashed whatever it says); **`0`: the index did not state one, or there is no build for this machine.** A plate draws `0` as "not told", never as an empty file |
| `FOXAPI_ITEM_BUILD_HERE` 0x200000 | the index publishes a build for this machine's os and architecture. An entry is installable here when it is `COMPATIBLE` **and** `BUILD_HERE`; `fact.store.<id>.blocked` names whichever gate is shut, and also the ones these two flags do not say (a transfer in flight, no licence declared, already installed) |

`name` is cut at the item's 95 bytes, where the old window drew a display name whole (whether any real entry's name reaches the
cut was not checked; the code's own example is 37 characters). The id in `detail` is a short machine key and is not cut in practice.

**The per-entry facts.** One set for each `CATALOGUE` entry, keyed by the id exactly as `detail` has it, every key
present for every entry (`""` when there is nothing: "known and empty" is told from `NOT_FOUND` for an id nobody
listed), in the sense of section 4 - the texts a list item would cut, or a state that is not a number:

| Key | Value |
|---|---|
| `fact.store.<id>.author`, `.licence` | the entry's texts, whole: free text the plate prints as given. `.licence` is `""` for an entry that declares none, which is also why `.blocked` reads "the catalogue entry declares no licence" |
| `fact.store.<id>.platforms` | the builds the index publishes, in the index's order, joined as the plate letters them: `windows/x64, linux/x64` (a finished string, nothing to parse); `""` for an entry announced before its first build |
| `fact.store.<id>.floor` | the retirement floor (`minSupportedVersion`): an installed copy older than this is not loaded. `""` is the normal case, **no floor**, and must never be read as "retire everything" |
| `fact.store.<id>.file` | the file name the build for this machine installs under: the catalogue's own spelling put through the loader's name rule (`PluginRepo::sanitiseFileName`), exactly what "is it installed" compares. **The join to `PLUGINS`:** compare it with a `PLUGINS` entry's `detail` ignoring ASCII case (the engine's own rule). A match is the host's record of the module (its `ACTIVE`, `STOPPED`, `TUNE_ALLOWED`, `value[0]`'s `CASCADE_CAP_HOST_CLIENT`, and `fact.plugin.<file>.error`); **no match on an entry that is `INSTALLED` is a retired module, which the host holds no record of** (it was renamed out of the scan), and is not an error. `""` with no build for this machine or a name the rule refuses. (Since stub campaign B4 the join for an installed plugin is `.installedFile`, then this.) |
| `fact.store.<id>.updateReason` | the update plan's reason, one English sentence (the window draws it as given); `""` when no update is planned. The entry has a plan exactly when its id is a `STORE_UPDATES` `name`, and that entry's `detail` is "from -> to" |

`fact.store.url` is the catalogue index the store reads: an `https://` URL, or a path to a local `index.json` (an
address with no `://` is read from disk, which is how the store is exercised offline). It is **LOCAL**: a path
names the engine's machine, and a REMOTE session's read is `DENIED`. It reads the default `https://` address until a config or
`FOXAPP_OP_SET_CATALOGUE_URL` sets another, and `""` once it has been cleared. It has no leaf of an
id, so it cannot collide with a `fact.store.<id>.<leaf>` (an id "url" has keys `fact.store.url.legal` and the
rest). It is the value `FOXAPP_OP_SET_CATALOGUE_URL` stored, read back one pass after the command.

How a plate reads the gate. `fact.store.<id>.blocked` is `pluginInstallBlockedReason(i, true)`: the reason an
install would be refused now **with the legal notice taken as acknowledged**, `""` when it would not be. The gates
run in the order busy, ABI, platform, licence, already installed, and the acknowledgement last, so the reason for the
state of one row's tick follows from it: with the notice acknowledged it is `.blocked`; without, it is the same
unless `.blocked` is `""` and `.legal` is not, and then it is "the legal notice must be acknowledged first" (an
English sentence, translated where it is drawn, like every one of these: `trStoredReason`). The tick belongs to one
row, the window's selection, and is the window's own state; it travels as `ival[0]` of `FOXAPI_OP_STORE_INSTALL` and
`FOXAPP_OP_STORE_ADD_ALL`, and the engine re-tests the gate when the command applies.

**The retired modules.** `STORE_BLOCKED` lists the installed plugins the store's policy refuses to load (`name` is the
file). Their facts are keyed by the **file**, not under `fact.store.<id>`: a retired module may be listed with no
catalogue fetched at all.

| Key | Value |
|---|---|
| `fact.blocked.<file>.id` | the catalogue id the module was installed under: the `name` of its `STORE_UPDATES` entry when the catalogue has an update for it, and the `text` of `FOXAPP_OP_STORE_UPDATE`. `""` for a module whose install record names none |
| `fact.blocked.<file>.message` | the sentence the row draws, **whole**, in the language in force: what the module is, why it is disabled and what to do about it (update it, or ask its author, as the case is; about 200 bytes) |

Why a fact and not `STORE_BLOCKED`'s `detail`: that item cuts at 159 bytes, and the sentence is composed when the
block is built, **not** when the plugins were loaded (which is before any language is chosen, and is what `detail`
still carries, in English): the fingerprint hashes the language, so a language switch rebuilds the block and with it
every `.message`, as `fact.converter.name` is rebuilt. Both keys exist exactly for the files `STORE_BLOCKED` lists (`.id`
is `""` for a row whose install record names none); any other file is `NOT_FOUND`. A file name with dots in it is keyed
whole, so an interface that is handed a key parses the leaf from the right, as 8.8 says for `fact.plugin.<file>`.

**What the store is doing - `STORE_ACTIVITY` (39).** One list for the busy lamp, the progress bar and the ADD ALL line,
**at most two entries, told apart by `id`**, and none when nothing is happening. The first entry's `id` is 1, the
second's is 2; with only ADD ALL going, the one entry is the list's first and still has `id` 2.

| `id` | Present while | `name` | `value[0]` | `value[1]` | |
|---|---|---|---|---|---|
| 1 | a catalogue fetch or a download is in flight (the old "busy": one transfer at a time, the repo has one progress and one cancel) | the module's display name for a download; `""` for the catalogue | **the progress, 0 to 1, read live** | `1` fetching the catalogue, `2` downloading a module (an update included) | `ACTIVE` |
| 2 | an ADD ALL run is active | the module moving now (`""` before the first one starts) | the run's position, 1-based, clamped to the total: the number "installing 4 of 23" shows | the total: how many modules the run will take | `ACTIVE` |

The windows' sentences stay the interface's: "fetching the catalogue" (kind 1), "downloading <name>" (kind 2) and
"installing <position> of <total>: <name>" (entry 2; the window letters "starting" for an empty name). The two entries are
independent: entry 2 is "the run is going" and entry 1 is "a transfer is moving". Entry 2 stands alone in the publish of the
pass that applied ADD ALL, before the first module has started (name `""`, position 1); from then until the run ends entry 1
is present in every publish, because the next module is started in the same pump that finds the last finished, so no block
shows the gap between two modules.

**The position is the module in flight, 1-based.** The engine's queue steps its index past a module as it starts it, and the
position is that index (clamped to 1 .. the total): the first module is "1 of 23", the last "23 of 23", each number once. The
old window lettered `min(next + 1, total)` of that index, which reads one too many - the first module as "2 of 23" and the last
two both as "23 of 23" - and the table does not carry the mistake (found by reading the position after every pass of a real run,
which printed `Bad One@2 Bad Two@3 Bad Three@3` before it was corrected). A window ported to this list draws the true number.

**The progress is the one thing the list reads live.** `value[0]` of entry 1 is the transfer's own progress figure, a
relaxed atomic the download worker writes every chunk, read when the list is read, as `RECORDINGS`' `value[1]` is. It
moves with no list change and moves **neither the block nor `listSeq`**. `0` while the server has not sent a length
(a figure that lies is worse than one that waits); a catalogue fetch moves it too, and a fetch of a local `index.json`
does not touch it, so it keeps what the last transfer left (`1` after one that finished): an interface that draws
it for a kind-1 entry draws what the old window drew. It is read without allocating, from any thread.

**Two status lines** join `STATUS_LINES` (5.1), as `fact.status.catalogStatus` and `fact.status.addAllSummary` (whole) and
in the engine's handed-over copy of the lines (`catalogStatus` and `addAllSummary`, and `addAllFailed`, the source of the second
one's level: the copy is what the standard window draws its lines from): `catalogStatus` (level 1) is "N plugins in
the catalogue" ("1 plugin in the catalogue"), set by every fetch that landed **including one that listed nothing**
("0 plugins in the catalogue") and cleared when a fetch starts; it is what separates "asked, and the index lists nothing"
from "nobody has asked" (an empty `CATALOGUE` with neither this line, a `catalogError` nor entry 1 is the second).
`addAllSummary` (level 1; **level 3 when any module in the run failed**) is what the last ADD ALL run did: "23 installed, 0
failed", or that and then each failure's name and the reason it gave; it stays until the next run starts and is empty
before the first.

**How the window reads it** (what step B of the port holds the engine to):

| The window draws | From |
|---|---|
| the rail's chip: IDLE, N UPD, BUSY | `CATALOGUE`'s count (IDLE when `CATALOGUE` has no rows, the rule the window has always kept: a status line or a fetch does not change it), `STORE_UPDATES`' count, `STORE_ACTIVITY`'s entry 1 (BUSY, which outranks the other two) |
| the "CATALOGUE SOURCE" line | `fact.store.url`, `catalogStatus`, the `catalogError` line |
| a row and its plate: name, version, maker, licence, the blurb, summary, homepage, legal notice, the floor | `CATALOGUE` `name`; `fact.store.<id>.version`, `.author`, `.licence`, `.description` (else `.summary`), `.summary`, `.homepage`, `.legal`, `.floor` |
| the ABI line, the builds, the size | `CATALOGUE` `value[0]` and `COMPATIBLE`; `.platforms`; `value[1]` (`0`: not told) and `BUILD_HERE` |
| FIT enabled, and why not | `COMPATIBLE` and `BUILD_HERE` (installable here); `.blocked` and `.legal`, and the row's tick (see above) |
| fitted, loaded, running, refused, the grants | `INSTALLED`; the `PLUGINS` entry whose `detail` is `.file` (`ACTIVE`, `STOPPED`, `TUNE_ALLOWED`, `value[0]`) and `fact.plugin.<file>.error` |
| UPDATE, and its reason | the id in `STORE_UPDATES`; `.updateReason`, `.version` (the version it goes to) |
| the retired rows | `STORE_BLOCKED` (the file); `fact.blocked.<file>.message`; the Update key from the `STORE_UPDATES` entry whose `name` is `fact.blocked.<file>.id`; Remove is `FOXAPP_OP_STORE_REMOVE_BLOCKED` with the file |
| busy lamp, the bar, "downloading <name>" / "fetching the catalogue" | `STORE_ACTIVITY` entry 1 |
| "installing 4 of 23: <name>" | `STORE_ACTIVITY` entry 2 |
| the green and red result lines | `installReport` / `installError` (8.8) |
| the ADD ALL summary, red or not | `addAllSummary` and its level |

**Timing.** A command through the table lands at the engine's next pass and the lists show it after that pass's publish.
A transfer is applied by the pass that polls it, which is **after** that pass's publish: `STORE_FETCH` is applied at the
top of a pass, so entry 1 (kind 1) appears, `CATALOGUE` empties and `catalogStatus`, `catalogError`, `installReport` and
`installError` clear, **all in that pass's publish**; the fetch's result is applied by the pass that finds it ready, and
**the pass after it** publishes the rows, `catalogStatus` (or `catalogError`) and entry 1 gone, in one block. A download
and an ADD ALL step land the same way. The engine's handed-over copy of the lines is published at the end of the pass that
applied the result, so a window that reads its lines from the copy and its rows from the table can show a result line one
pass before the row it belongs to: the table's own lines (`STATUS_LINES`, `fact.status.*`) are in the same block as the
rows and cannot differ from them. ADD ALL moves from one download to the next **inside one pump** (the pass that finds a
download finished starts the next in the same call), so no publish shows a gap between two modules: entry 1 stays
present, its `name` changes, entry 2's `value[0]` advances. An interface that must notice "a catalogue was replaced" (the window clears the legal notice ticks it holds when one is: a
tick is consent for a notice that was listed, not for another) finds entry 1 with `value[1]` 1 in some block, **or**, when it did not
read the block in between (a fetch of a local file can start and land within two of its reads), finds the catalogue itself
different from the one it holds. **`CATALOGUE` is NOT emptied when a fetch starts, and a fetch that fails does not empty it (0.99.72, restored by
stub campaign B3; 9.4):** the list on screen stays until a fetch succeeds and replaces it, and a failure leaves it (or, with nothing on screen, brings back the
kept copy) beside `catalogError`; `fact.store.refreshFailed` says which. What stays offered is only ever a list this client accepted once, and `install()`
re-checks every entry it is handed whatever list it came from.
The index (`fact.store.url`) is one pass behind its command.

**What moves `listSeq`, beyond 8.1 to 8.8.** Everything new but the progress is in the list fingerprint: the catalogue
(one counter, bumped wherever the engine replaces, clears or adds to it, so a refetch that returns the same ids, versions
and notice lengths with other texts moves it, and the entries' texts are never hashed one by one; on every real path a
fetch's own busy flag and the rescan a landing runs move the fingerprint as well, and the counter is what makes it exact), the index, the busy name
(**the name, not only that something is busy**: the flag does not change between two modules of an ADD ALL run), the run
(`active`, position, total, current name, and whether it failed), the retired rows' ids and sentences, and the two status
lines. **The progress moves nothing**, the transfer's start, end and each module's turn do. A retired module's
sentence is rebuilt on a language switch (the language was already hashed). **Nothing here allocates in a pass:** the
fingerprint is hashed in place from members the engine keeps, and the block, the facts, the composed sentences and the
update plans are built only when it moves; the live progress is an atomic read.

### 8.10 The Source section and the sound card as their pages draw them

The Source section (its combo of radios, the Refresh key and "Look for network USRPs", the IQ file, sound card and
Pluto panels, the notes about dongles that cannot be opened and radios that cannot be found, and the open radio's
rate, antenna, gain and option controls) and the sound card panel under it read the engine's table only. 0.3 had the
radio half already (`SAMPLE_RATES`, `GAINS`, `ANTENNAS`, `DEVICE_OPTIONS`, `CONVERTERS`, `GAIN_MODES`,
`DECIMATIONS`, the flags of 8.3, `STATUS_LINES` `sourceError`, `restoreKeepLabel`, `soundCardMissing`,
`sdrPlayAdvice`, `sdrPlayApiDetail`, `tuneMismatchNote`) and every command the section sends. It had almost nothing
of the half this section is mostly about: the combo's selection and two of its three fixed rows, whole row ids, every
state of the scan and the sentences' inputs, the dongles that cannot be opened, the SDRplay and UHD notes, where the
runtime looked, and the whole of the sound card panel. This adds that. **One member is appended to
`FoxReceiverState`** (`sourceFlags`, below; `FOXAPI_MIN_RECEIVER_STATE` moves to its end), five lists, eleven facts,
one status line, one column, one op and one field of an op.

**The word - `sourceFlags`** (a `uint32_t` after `scanListenMs`; the device group, so it moves `deviceSeq` and `seq`,
and not the plugin ABI's device counters). Each bit is one condition, read from the member that owns it:

| Bit | Set while |
|---|---|
| `FOXAPI_SRC_SCAN_PENDING` 0x1 | a SoapySDR device scan is running on its worker (the combo and Refresh are greyed; the scan is a child process the engine starts and collects) |
| `FOXAPI_SRC_SCANNED` 0x2 | a scan has been started this session. A scan that was **deferred** (a radio is open, see `SCAN_GATED`) does not set it, so the section knows the first scan is still owed |
| `FOXAPI_SRC_SCAN_PARTIAL` 0x4 | the last scan was one beside an open radio, which left that radio's driver out; a whole one is still owed (the combo's next opening, `FOXAPP_OP_SCAN_DEVICES_ON_OPEN`, runs it once nothing gates it) |
| `FOXAPI_SRC_SCAN_GATED` 0x8 | a radio is open, opening, held by the patch, or one this session could not release: a SoapySDR scan is deferred, or leaves that radio's driver out. `fact.source.scanGate` and `.scanSkip` say which, and for whom |
| `FOXAPI_SRC_NETWORK_USRPS` 0x10 | "Look for network USRPs" is on (`FOXAPP_OP_SET_NETWORK_USRP_SCAN`, which rescans when it goes on) |
| `FOXAPI_SRC_UHD_LEFT_OUT` 0x20 | the last scan that landed left UHD out because nothing could be a USRP (none on the USB bus, none named by a saved setting, and the box above off): the section says "Network USRPs are not searched" |
| `FOXAPI_SRC_SDRPLAY_ROWS` 0x40 | the last native walk found an SDRplay API row. The note about the SDRplay API (`STATUS_LINES` `sdrPlayAdvice`, with where it looked in `sdrPlayApiDetail`) is for the case where this is clear and `sdrPlayAdvice` has text |
| `FOXAPI_SRC_CARD_LISTED` 0x80 | the sound card inputs have been listed; `SOUND_CARDS` is that list, and may be empty ("No sound card inputs found") |
| `FOXAPI_SRC_CARD_SCANNING` 0x100 | the inputs are being listed on a worker |
| `FOXAPI_SRC_CARD_OPENING` 0x200 | a sound card is being opened on a worker. With `FOXAPI_RX_DEVICE_OPENING` (a radio being opened) it greys the card's controls; it is also what keeps the "saved radio not open" lamp dark while a restored card opens |

**`DEVICES`, completed.** The entries are now, in the Source combo's order: the signal generator (`detail` `siggen`),
**the IQ file** (name `IQ file`, `detail` `row:iqfile`; `ACTIVE` when the source in use is a recording), **the sound
card** (name `Sound card`, `detail` `row:soundcard`; `ACTIVE` when it is the source in use), then the native rows, then
the SoapySDR rows. **An entry's index is its row in the combo** (the numbering `fact.source.row` speaks: 0 the
generator, 1 the IQ file, 2 the sound card, 3 and on the radios), and its `id` is that plus one. 0.2 listed only the
generator and the scanned radios: the two fixed rows are new, and every radio's entry moved down two places (a
0.2 interface found its radio by the `detail`, which did not change). A native row's name is the driver's label; the
combo draws it with " (native)" after it, in its own words, **except the Pluto's** (an address, not a radio that was
found; the kind of a row is the part of its id before the first `:`). A row is a SoapySDR row when its id begins
`soapy:`.

**A row's id is read from a fact, whole.** `detail` holds 159 bytes and the id a row sends is not that short: a SoapySDR
row's is `soapy:` and the whole kwargs string a vendor module reported (a network USRP's can run long), the Pluto's is `pluto:uri=` and an address of up to 191 bytes. A cut id matches no
scanned device, and `FOXAPI_OP_SELECT_SOURCE` answers `NOT_FOUND` ("no scanned device matches that id"); **`SELECT_SOURCE`'s "an id from
`DEVICES`, verbatim" therefore means from `fact.source.id.<n>`, never from a `detail` that may have been cut.**
`fact.source.id.<n>` is the whole id of the entry whose `id` is `n` (1 to the entry count; `NOT_FOUND` past it). It is
the id the combo itself sends, from one spelling in the engine (`Engine::sourceIdForRow`). An interface reads it when
a row is clicked and does not keep a copy. The Pluto's id is built when the native walk ran, from the address the
box held then (`pluto:uri=<address>`), so its `ACTIVE` compares an old address with the open one and is false after an
Open at a changed address: **an interface draws the Pluto's selection from `fact.source.row`, which follows the
family alone**, and does not use that entry's `ACTIVE`.

**The facts.**

| Key | Value | |
|---|---|---|
| `fact.source.row` | the Source combo's own selected row as a decimal: `-1` none (a rescan dropped the radio's row, or a restore failed: the combo then shows the live source's name), `0` the generator, `1` the IQ file, `2` the sound card, `3` and on the radios in `DEVICES` order. Not `DEVICES`' `ACTIVE`, which is the source **in use**: they differ whenever the combo sits on the IQ file, the sound card or the Pluto before Open. Which panel shows under the combo is decided by this row | |
| `fact.source.keep` | the kind of the radio, sound card or file the config is still naming while the generator stands in for it (a restore that could not open it, a card released for a re-Open, a card handed back by the patch): `""` when none, else `file`, `soundcard`, `soapy` or the native driver's key. The lamp on the section's header is lit when it is not `""` and not `file`, no radio is open, the source in use is the generator and `FOXAPI_SRC_CARD_OPENING` is clear. (`STATUS_LINES` `restoreKeepLabel` is the combo's caption for it, and is empty for a card kept at a startup restore) | |
| `fact.source.opening` | the name of the radio being opened, as the "Opening ..." line says it (the row's label, "... (native)" for a SoapySDR row opened through its native driver, "ADALM-Pluto at <address>"); `""` when no radio is opening (`FOXAPI_RX_DEVICE_OPENING`) | |
| `fact.source.scanGate` | with `FOXAPI_SRC_SCAN_GATED`, the name the two deferral sentences quote: the open radio's model (never its serial), the label of the one opening, `the patch radio '<name>'`, `a patch radio`, or `a radio this session could not release`; `""` otherwise. English, quoted whole in the interface's own sentence | |
| `fact.source.scanSkip` | with the scan gated, the drivers a scan beside the open radios leaves out, joined `", "`; **`""` means the whole SoapySDR scan is deferred** and a non-empty text means Refresh still runs and leaves exactly these out. `""` with nothing gating it | |
| `fact.source.faultedDrivers` | the drivers whose scan child died earlier this session (they are not asked again), lower case, joined `", "`; `""` when none. Taken when a scan lands | |
| `fact.source.uhdNoImages`, `fact.source.usrpOnUsb` | `"1"`/`"0"` (0.99.90 merge): UHD itself said, during the last scan that landed, that it has no firmware images; and that scan's USB listing held a USRP. The Source section says its one amber line (`uhdImagesMissingHint`) when BOTH are `"1"`. Taken when a scan lands | |
| `fact.source.plutoUri` | the address in the Pluto box, as the engine holds it (`FOXAPP_OP_SET_PLUTO_URI` stores each edit; 191 bytes at most) | LOCAL |
| `fact.source.id.<n>` | the whole `SELECT_SOURCE` id of `DEVICES` entry `n` (above) | |

**The radio's fault, whole - `STATUS_LINES` key `deviceFault`** (level 3; and `fact.status.deviceFault`). The state's
`faultMessage` is the same sentence cut at 127 bytes, and the one the section must not draw cut is 134 bytes: the
zombie-source sentence ("source thread: the radio's driver is holding an abandoned thread and this source cannot
restart; pick another source or restart FoxSDR"). The section adds "Reconnect it and pick the source again, or switch
to the signal generator" under every fault **except** one whose sentence contains `restart FoxSDR`, and in the cut copy
that ending is gone, so the line would be added under a sentence that says the opposite. **An interface draws the fault
from this line and the flag `FOXAPI_RX_FAULTED`, never from `faultMessage`** (which stays the quick, cut copy for
those that want only a word). The line is the pipeline's sentence while it is faulted, or the test stand-in's.

**`SAMPLE_RATES` `detail`** is the rate's caption as the Rate combo letters it ("2.400 MS/s"; an Airspy's rates are
captioned by its decimation). The row that is current is the one **nearest** `sampleRateHz` of the state (a radio that
coerces a rate to one it supports is on that row, and `SAMPLE_RATES`' own `ACTIVE`, an exact match, is then on none).
`value[0]` is the Hz `FOXAPI_OP_SET_SAMPLE_RATE` takes.

**`UNBOUND_RADIOS` (40)** - the USB radios that are plugged in and that nothing can open, because no WinUSB driver is
bound (the DVB-T television driver, or none). One entry each: `name` the bus description (`""` when it reports none:
the section then says "A USB radio"), `detail` the family that decides the instruction, `rtl` (an RTL2832U, a
composite device whose SDR half is interface 0), `rx888` (two identities, each to be bound) or `""` (any other), and
`value[0]`, `value[1]` the vendor and product ids. The three sentences stay in the interface; with the family the
interface carries no table of ids. Rebuilt by every native walk.

**`SCAN_EVIDENCE` (41)** - where the SoapySDR runtime looked, for the "No radio hardware found" tree: `value[0]` 1 a
vendor install (`name` its vendor, `detail` its root), 2 a module search path (`detail`), 3 a module loaded (`detail`),
in that order. **LOCAL sessions only: a REMOTE session's read is `DENIED`** (the entries are paths on the engine's
machine). `detail` cuts at 159 bytes; it is evidence for a person. **The engine takes it once, when a whole scan lands
having found nothing** (not by the interface every frame: each of the three asks is a call into the SoapySDR library,
the first loads it and edits the process's environment and DLL search order), and it is empty at every other
time, including after a scan that found a radio and after one that left a driver out. The count of entries with
`value[0]` 3 is "N vendor modules are loaded". A hermetic engine (one whose scan is stood in for) reads none of it.

**The sound card.**

| List | Entry |
|---|---|
| `SOUND_CARDS` (42) | the inputs as the last listing found them, in the backend's order. `id`: index + 1 (what `FOXAPP_OP_SOUND_CARD_PICK` takes); `name`: `device (host API)` as the combo letters it; `detail`: the host API; `ACTIVE`: the entry the form's card is, matched the way the open matches it (an entry chosen from the list names exactly that entry even beside an identical card; a name read from a saved setting is matched by the card's identity, which on Linux ignores the ALSA card number). A form with no device named means the system's default input, which is the entry the backend flags as the default. None is `ACTIVE` when the form's card is not listed (a saved card that is unplugged: the combo keeps its name in the preview, and `soundCardMissing` says why). Empty until `FOXAPI_SRC_CARD_LISTED` |
| `SOUND_CARD_RATES` (43) | the rates that entry offers in the form's format (real needs an even rate of at least 16 kHz; I/Q needs two channels), ascending, in Hz of the card: `value[0]`; `FOXAPI_ITEM_EXCLUSIVE` 0x400000 for a rate only WASAPI exclusive mode reaches ("(exclusive)" and the tooltip); `ACTIVE`: the form's rate. Empty when no entry is `ACTIVE`; the Rate combo then shows the form's own figure |
| `SOUND_CARD_FORM` (44) | ONE entry (`id` 1), the form the Open key opens: `name` the device and `detail` its host API (**`""` to a REMOTE session**, whole in the two LOCAL facts below), `value[0]` the card's rate Hz, `value[1]` the I/Q centre Hz, `value[2]` the channel (0 left, 1 right), and the flags `FOXAPI_ITEM_IQ_STEREO` 0x800000 (I/Q, else real), `FOXAPI_ITEM_SWAP_IQ` 0x1000000 and `FOXAPI_ITEM_PICKED` 0x2000000 (chosen from `SOUND_CARDS`) |

| Key | Value | |
|---|---|---|
| `fact.soundcard.device`, `fact.soundcard.hostApi` | the form's card, whole | LOCAL |
| `fact.soundcard.span` | `<lo Hz> <hi Hz>`, each written to the digit it reads back by (`%.17g`): what the form's card would receive on the air through the converter kept for it (none for I/Q; a down-converter for real), the two figures of "Receives ... to ...". Never negative; an inverting converter swaps the edges | |

**`FOXAPP_OP_SOUND_CARD_PICK` (0x8420, SETTINGS, LOCAL sessions only; `ival[0]`: a `SOUND_CARDS` entry's `id`)** is
what a click on a row of the Device combo does. The engine puts that card's name and host API into the form, marks it as
chosen from the list, clears the `soundCardMissing` line, and **keeps the form's rate when the new card offers it in the
form's format, else the card's own default, else the highest it offers** - the rule the window held in its draw. `NOT_FOUND`
(and nothing changes) for an id with no entry and before the cards are listed. The pick of the card already `ACTIVE` is
applied all the same (it marks the card as chosen from the list); an interface that wants a click on the current row to do
nothing does not send it.

**`FOXAPP_OP_SOUND_CARD_FORM` gains a field mask in `ival[1]`.** 0 (and 127) is the whole form, as before. Otherwise a set
bit names a field the command carries, and every other field keeps the engine's own value: 1 the device and host API (the
text), 2 the format (`ival[0]` bit 1), 4 the channel (bit 2), 8 swap I/Q (bit 4), 16 chosen from the list (bit 8), 32
the rate (`num[0]`), 64 the centre (`num[1]`). A control that changed one thing sends one field, so it cannot write
back a stale copy of another that a pass changed since the interface last read it (a worker's open result replaces the whole
form). A bit past 64, or a negative mask, is `OUT_OF_RANGE` and changes nothing. The form is the engine's: the Open key
(`SELECT_SOURCE` `soundcard:open`) opens the card the engine's form describes, not a copy an interface holds.

**What the engine now does on its own** (each used to be a command an interface sent every frame, and none is any
longer needed - the commands stay valid):
- *The deferral's one log line.* "Device scan deferred while X is open" is written once for each radio; the latch that
  keeps it from being written sixty times a second is let go **by the engine's pass the moment the gate opens**
  (`FOXAPP_OP_CLEAR_STATUS` with `FOXAPP_STATUS_SOAPY_SCAN_DEFERRED_LOGGED` did it from the window's frame whenever the
  section was drawn).
- *Listing the sound cards.* While the combo's row is the sound card (`fact.source.row` `2`) and the cards are not listed,
  not being listed and no card is opening, **the pass starts the listing** (`FOXAPP_OP_SOUND_CARDS_WANTED` did it from the
  window every frame until the list arrived). Unlike then it happens whether or not the section is on screen.
- *The sound card's centre.* `FOXAPP_OP_SOUNDCARD_IQ_CENTRE` answers `NO_CHANGE` ("no I/Q sound card is running") when
  the source in use is not a sound card in I/Q mode, so an interface sends it after every centre it commits and does not
  consult the live card first.
- *The scan the combo's first open asks for* is still `FOXAPP_OP_SCAN_DEVICES_ON_OPEN` (the native walk, always, runs inline
  in the pass that applies it; the SoapySDR scan if it has never run, or a partial one is owed).

**What moves `listSeq`, beyond 8.1 to 8.9.** Everything new is in the list fingerprint, and the flag word moves `seq` and
`deviceSeq` itself: the combo's row, the kept source's kind, the radio being opened and its label, the open radio's model, the scan's
four states and **the gate as the answer it is** (the patch's radios still opening, and the process-wide count of open
SoapySDR radios, move it with nothing else changing), the network box and the UHD note, the SDRplay row flag, the Pluto's
address, the unbound dongles (id, product and description), the drivers that faulted, the evidence (as one counter, bumped when a
landed scan changed it; its rows are paths and are not hashed one by one), the whole fault sentence, the rates' captions, the
sound card's listing state, its form and every input it listed (name, host API, default rate, each rate and whether it is exclusive).
The block, the facts, the families of the unbound dongles, the matching of the form's card and the converter lookup behind
`fact.soundcard.span` are built only when it moves. **Nothing allocates in a settled pass.**

**How the window reads it** (what step B of the port holds the engine to):

| The window draws | From |
|---|---|
| the header's chip, lamp and lit state | `deviceName`; `FOXAPI_RX_FAULTED`, `FOXAPI_RX_RUNNING`; the lamp: `fact.source.keep`, `FOXAPI_RX_DEVICE_OPEN`, the `ACTIVE` entry of `DEVICES` being the generator, `FOXAPI_SRC_CARD_OPENING` |
| "Device stopped: ..." and its advice | `FOXAPI_RX_FAULTED` and `STATUS_LINES` `deviceFault` (whole) |
| "Opening ..." / "Scanning for devices..." and the greyed combo | `FOXAPI_RX_DEVICE_OPENING` with `fact.source.opening`; `FOXAPI_SRC_SCAN_PENDING` |
| the combo's preview and rows | `fact.source.row`; `DEVICES` entry by entry (the row's index is its entry's); `restoreKeepLabel` with `fact.source.keep` for the saved-not-open preview; `FOXAPI_RX_PATCH_RUNNING` greys the rows |
| a click on a row | `FOXAPI_OP_SELECT_SOURCE` with `fact.source.id.<entry id>` |
| the scan deferral sentences and the Refresh tooltip | `FOXAPI_SRC_SCAN_GATED`, `fact.source.scanGate`, `fact.source.scanSkip` |
| "Look for network USRPs" and the line under it | `FOXAPI_SRC_NETWORK_USRPS`, `FOXAPI_SRC_UHD_LEFT_OUT` |
| the note of drivers not scanned again | `fact.source.faultedDrivers` |
| one sentence for each dongle that cannot be opened | `UNBOUND_RADIOS` |
| the SDRplay note | `STATUS_LINES` `sdrPlayAdvice` and `sdrPlayApiDetail` while `FOXAPI_SRC_SDRPLAY_ROWS` is clear |
| "No radio hardware found" and the tree | `FOXAPI_SRC_SCANNED`, not `FOXAPI_SRC_SCAN_PARTIAL`, not `FOXAPI_SRC_SCAN_PENDING`, and no entry in `DEVICES` that is a SoapySDR row (id begins `soapy:`); `SCAN_EVIDENCE` |
| which panel shows under the combo (IQ file / sound card / Pluto) | `fact.source.row`; the Pluto's row is the entry whose id begins `pluto:` |
| the Pluto box | `fact.source.plutoUri` for the draft; `FOXAPP_OP_SET_PLUTO_URI` per edit; Open: `SELECT_SOURCE` `open-pluto:uri=<draft>` |
| the radio panel's Rate, Antenna, Auto gain, gain sliders, bias tee, options | `SAMPLE_RATES` (`detail`, nearest to `sampleRateHz`), `ANTENNAS`, `FOXAPI_RX_DEVICE_AGC` and `FOXAPI_RX_AGC_SUPPORTED`, `GAINS`, `FOXAPI_RX_BIAS_TEE_*`, `DEVICE_OPTIONS` (8.3, unchanged) |
| the sound card panel | `FOXAPI_SRC_CARD_*`; `SOUND_CARDS`, `SOUND_CARD_RATES`, `SOUND_CARD_FORM`; `fact.soundcard.*`; `soundCardMissing`; the commands above |

**Timing.** Every command lands at the engine's next pass, and the lists, facts and flag word show it after that pass's
publish; a worker's result is collected by the pass after the one that started it. So an interface that sends a form change and
draws in the same frame draws the old form for one pass (the window's old "apply at once" is no longer something an
interface can ask for), and the Pluto box deactivating while its last edit is still queued reads the previous address back
for one pass: an interface keeps its own draft for the box and sends it with Open, as the window does for the scanner's
fields.

**What a test may stand in for.** In an engine of the application's own build (`Engine::testHooks_`, not the table):
`nativeScanDetail` stands in for the whole native walk - the radios, the unbound dongles, the SDRplay advice and detail
and the API-row flag, so those sentences are reachable with no USB walk and no SDRplay install - and `scanDiagnostics`
for the three SoapySDR diagnostics and the faulted-driver list. With the scan stood in for (`soapyScan`) and no
diagnostics hook, the engine reads none of the four: a hermetic engine never calls into the runtime.

### 8.11 The radar scope and the demod scope as they draw

The radar scope (the aircraft plan-position indicator that replaces the window, with its range stepper, its gauges, its
GAIN knob and its POWER key) and the demod scope page (the audio, spectrum, vector and multiplex traces) read the table
only. 0.3 had almost all of it: `FOXAPI_RX_RUNNING`, `FOXAPI_RX_DEVICE_OPEN`, `FOXAPI_RX_DEVICE_AGC`, `signalDb`,
`channelRateHz`, `demodMode` and `audioPlayingPlugin` in the state, the first gain stage in `GAINS`, the module census in
`PLUGINS`, the samples through `read_scope`, and every command (`FOXAPP_OP_SET_GAIN_NO_READBACK`, `FOXAPI_OP_RUN`). It
lacked two small things. **No op, list, fact or status line is added.**

**8.11.1 The receiver's position.** (Window port group 14, part a, retired the hook the engine used to call when a position was
applied, `EngineHost::onReceiverPositionApplied`: the typed fields, every map page's and Patch Map part's home, the scope and the
coverage map follow the state, which the window reads once a frame - a change of the pair moves `seq` - on its own thread. No API
changed. One difference: a re-apply of the pair already held, from a session that is not the window's own press, is not seen.) `FoxReceiverState` gains at its END two `double`s, `rxLatDeg` and `rxLonDeg`
(degrees north and east), and the state flag `FOXAPI_RX_POSITION_SET` 0x40000000 (the next free bit of `flags`). The
coordinates are valid when the flag is set and are 0 otherwise (a position that was never set, or was cleared, is never
published as 0 N 0 E with the flag up: the flag is the truth, and the numbers are a pair that means nothing without it).
They are what `FOXAPI_OP_SET_POSITION` writes (refused when out of range, unchanged) and what the saved configuration
restores: a position restored at start-up is in the FIRST published state, so the first frame of an interface that
draws from it already sees it. They move `seq` and no other counter (the flag word moves `seq` as every flag outside a
group does; a change of either coordinate moves `seq`; the plugin ABI's own five counters do not move). The coordinates
are NOT in the lists' fingerprint: nothing a list serves depends on them, and a position change must not rebuild the
block. `FOXAPI_MIN_RECEIVER_STATE` moves to the end of `rxLonDeg` (this is a 0.3 append, as 8.10's `sourceFlags` was).
The state's tail gains 16 bytes (4 of padding before the first `double`) and a publish copies them.

**8.11.2 The scope tap's capacity.** `FoxScopeInfo::reserved0` becomes `uint32_t capacityFrames` (same offset, same
size: `FOXAPI_MIN_SCOPE_INFO` is unchanged, so every client's `info` already covers it, and an engine that predates it
leaves 0 there, which a client reads as "unknown, ask for what you need and take what comes"). It is the MOST FRAMES A
`read_scope` ON THIS SESSION CAN RETURN for this tap: for a LOCAL session the ring's size, 65536 for `FOXAPI_SCOPE_AUDIO`,
262144 for `FOXAPI_SCOPE_IQ` and 65536 for `FOXAPI_SCOPE_MPX`; for a REMOTE session `min(ring, FOXAPI_SCOPE_REMOTE_FRAMES)`,
that is 4096 for all three, so a client's clamp works on both. It is constant for the life of the engine, read from the
ring (`ScopeTap::capacity()`), and is filled in whether or not `capFrames` is 0 (an info-only call per tap is enough).
What it is for: a scope's sweep is ten divisions of its time base (`FoxScopeInfo::rateHz` frames a second), and the
longest base, 50 ms a division, asks for more frames than the I/Q ring holds whenever the channel rate is above about
524 kHz (a trigger that hunts for an edge asks for twice a sweep, from about 262 kHz). A client that does not know the
ring sees fewer frames than it asked for and cannot tell "the ring is the limit" from "the tap has not got that many yet";
`capacityFrames` is the clamp: `want = min(wanted, capacityFrames)`.

**What the radar scope and the demod scope read, from where:**

| Datum | From |
|---|---|
| the receiver has a position (the honest empty state "the scope needs the receiver's position"), the range and bearing of an aircraft from it | `FOXAPI_RX_POSITION_SET`, `rxLatDeg`, `rxLonDeg` |
| the SIG gauge and the POWER lamp | `signalDb`, `FOXAPI_RX_RUNNING` |
| the GAIN knob: whether a radio is open and lists a stage, its travel, the lit tick, the figure and its unit, the stage name sent, the "AUTO" note | `FOXAPI_RX_DEVICE_OPEN`, `gainCount`, `FOXAPI_RX_DEVICE_AGC`, `GAINS` entry 0 (`value[0]`, `value[1]`, `value[3]`, `name`, `detail`) |
| the POWER key | `FOXAPI_OP_RUN` |
| the knob | `FOXAPP_OP_SET_GAIN_NO_READBACK` (text: the stage name, `num[0]`: dB), SETTINGS |
| "the scope will be empty" (no loaded module is a track source) | `PLUGINS`: `value[0]` capability word, `ACTIVE` loaded, `STOPPED` |
| the aircraft, their count, the selected altitude and range | the HOST PROCESS (the plugin windows' track poll, called from the interface's frame at frame rate by the plugin ABI): not served, as `TRACKS` always was not (section 10) |
| the demod scope's waveform, spectrum and vector, and the liveness of each trace | `read_scope` AT THE CALL: the samples and `written` |
| the time axis (the I/Q rate), the WFM key, whose sound the trace is | `FoxScopeInfo::rateHz` or `channelRateHz`, `demodMode == FOXAPI_DEMOD_WFM`, `audioPlayingPlugin` |

**Timing.** The tap's frames and `written` are read at the call and are never a publish old. Everything else is the
published state: one engine pass old at most, so a rate or a mode change is labelled a pass late (the liveness rule of an
interface is far slower than a pass), and a gain command's new figure arrives after the pass that applied it. An
interface whose control computes its next value from the readback (a knob that adds a step) must hold its own copy of the
value until the readback catches up, or two steps sent before the pass publishes would both be added to the old figure
and one would be lost.

### 8.12 The Patch page, part 1: the runtime in the pass, the graph's version and the plan

The Patch page (the canvas, its parts bin, its faces and its two inspectors) is the standard window's default face. Before
this section it was also where the patch RAN: the window's frame rebuilt the parts catalogue, applied START and STOP,
opened and closed the patch's radios, compiled the plan and published each radio's set, and ran the rule that leaving the
view stops the patch. So the patch lists (`PATCH_RADIOS`, `PATCH_PROBLEMS`, `PATCH_PARTS`), `read_node_spectrum` and the
`running` bit of `FOXAPI_EVENT_PATCH` only moved while a window was drawing the view. And the page's edit of the graph was
the one place an interface and the engine shared a document without a protocol: the window merged its draft over the
engine's graph "as it is now" and the engine then replaced its graph with the result, which is correct only while the merge
and the apply are one synchronous step. This part moves the runtime into the engine's pass, gives the graph a version
that is published with it, makes the engine do the merge, and adds the plan the page draws. The page's other reads (the
device choices, the live readouts, the decoders' lines) are part 2, further down this section; the pictures are part 3.
**One op, one list, three facts, one field of an op, two fields of a list entry.**

**8.12.1 The runtime is the pass's: `FOXAPP_OP_SET_PATCH_PAGE_OPEN` (0x8421, VIEW, LOCAL sessions only).** `ival[0]` 0 or 1:
the standard window's patch view is showing. The engine's pass has a patch step between the plugins' step and the publish
(`Engine::pumpPatch`), which while the flag is set runs exactly what the view ran, in the order it ran it: the parts
catalogue is rebuilt from the loaded modules, START and STOP are applied (a stop closes every radio, finalises every file
and gives the receiver its radio back), the radios are reconciled with the graph (the receiver's radio taken when the patch
runs, devices opened on their workers, a centre or rate the radio reported written back into the graph), the plan is
compiled, and each radio's set is built and published to its reader thread; and when the flag goes from set to clear, the
close rule: the patch stops (`FOXAPI_RX_PATCH_RUNNING` clears), every patch radio closes, the receiver gets its radio back
and its decoders return. Nothing starts a patch by itself: opening the view runs the step, START runs the patch, as before.

- **The default is "showing"**, and it is the engine's, not an interface's: the view is the window's first face, so an
  engine nobody has told behaves as the first frame of a window that is showing it. An interface that has no patch view
  sends 0 once (the standard window sends it when it shows the receiver face and once at start-up when its saved
  configuration says so). The op is idempotent and never refused for its value; a REMOTE session is `DENIED` as every
  `FOXAPP_OP_*`.
- **What an interface can now rely on.** The patch lists, the plan, the node spectra and the `running` bit move with the
  pass whether or not any window draws; a graph edit is applied at the head of the pass and the radios' sets are built
  from it in the same pass, so a `SET_GRAPH` and the sets it caused are one pass apart at most. With the flag clear the
  step does nothing but the close rule: the plan, the catalogue and the lists stand where the last pass left them.
- **What a pass costs.** The step allocates by design while the view is showing (the catalogue is rebuilt from the module
  list, the plan is compiled into vectors, a set is built when its signature moves); only the PUBLISH side is held to
  "a settled pass allocates nothing" (`tests/test_engine_publish_alloc.cpp`, F7). The step is a bounded number of
  small allocations, once a pass, never per sample.
- **Retired with it:** the receiver pipeline's own patch runner (`Pipeline::patchRunner()`), which nothing in the engine
  publishes a set to since each patch radio owns its runner (0.99.17), was reaped and cleared by the window's frame; both
  calls are gone (nothing moved: a runner no one publishes to has nothing to retire). `EngineHost::patchPageOpen` and
  `EngineHost::onPatchGraphChanged` are gone: the engine reads its own flag, and a change to the graph is a version.

**8.12.2 The graph's identity and a merge-safe `FOXAPP_OP_PATCH_SET_GRAPH`.** Three facts, published in the SAME block as
`fact.patch.graph` (so they move together, with `listSeq`, and an interface that wants one consistent look reads them in
this order and checks the first again):

| Fact | Value |
|---|---|
| `fact.patch.version` | the graph's version, a decimal: a counter the engine bumps on EVERY accepted change to the graph - an edit from any session, a document, and the engine's own (START's and ALL OFF's switches, a centre or rate the runtime wrote back, a device named at the take-over). It starts at 1 for the empty graph, so 0 means "no version". Not a hash: it never repeats, and it never goes back |
| `fact.patch.epoch` | the number of documents loaded, a decimal (the epoch the event already carried): it moves when a NEW patch replaces the graph (`FOXAPP_PATCH_GRAPH_DOCUMENT`) |
| `fact.patch.runnable` | `1` or `0`: whether the plan can run, as the page lettered it (8.12.3) |

`fact.patch.graph` stays LOCAL; the three above name nothing on the machine and are not.

**The op.** `ival[0]` bit 0 is the document flag as before. **`ival[1]` is the version the sender's graph was edited from**
(0, the default, is "unconditional" and behaves exactly as the op always did: the graph sent replaces the engine's). With a
version the engine MERGES:

1. the version is the current one: the graph sent replaces the engine's, as above;
2. the version is an earlier one the engine still holds (it keeps the graphs of its last 8 versions, one per version it
   noticed; a document clears them): the engine applies `core::patch::rebaseDraft(that version's graph, the graph sent,
   the graph now)`. What the sender changed - the nodes it added and removed, a position, a size, a name, a centre (the
   value and its `centreChosen`), a mode, a plugin, a device, a rate, a squelch, a switch, the wires it made and cut -
   wins; everything else is the engine's. A node the engine removed meanwhile stays removed. The merged graph is what the
   engine holds afterwards. The result carries `FOXAPI_RESULT_CLAMPED` and `applied[1] = 1`;
3. the version is one the engine no longer holds (older than 8 changes, or from before a document, or one it never had):
   `FOXAPI_FAILED`, "the patch graph moved on since this edit was started; it was not applied". Nothing changes. The sender
   reads the graph again and decides;
4. a negative version is `FOXAPI_BAD_ARGUMENT`; a document ignores the version.

**`applied[0]` is the version of the graph after the command**, so a sender learns its new base from the answer - and the
answer, as every answer, is delivered only after the publish that shows the state it describes, so the next read of
`fact.patch.graph` is at that version or later. A text the engine cannot honour is refused whole as before
(`FOXAPI_BAD_ARGUMENT`), the version unmoved.

**Why the engine merges.** A command reaches the engine a pass after it was sent, and the graph it replaces is the engine's
as of THAT pass, not the one the sender last read. Anything the pass did in between is lost by a replacement: START
throws the radios' switches on and then marks the patch running; a stale document applied after it puts the switches back
off - the patch is "running" with every radio off and nothing opens until the next START. A merge on the sender's side
cannot close this, because "now" is a publish old; moved to where "now" is exact, it does (`tests/test_engine_api_03.cpp`,
G13-2, red with the version left out and green with it, for START and for a second session).

**How an interface uses it.** Read `fact.patch.version`, `fact.patch.epoch`, `fact.patch.graph` and `fact.patch.version`
again; if the two versions differ a pass was published between the reads, ask again. Edit a copy. Send the whole copy with
`ival[1]` = the version read. **Do not adopt the engine's graph until the answer names the new version**: the published
graph is the old one until the pass has applied the command, and a copy that is overwritten with it snaps a node that is
being dragged back where it was. When the answer arrives (OK, `applied[0]` = V) and a publish at version V or later has been
read, take that graph as the new base and put the edits made since the send on top of it with the same
`rebaseDraft` (the edit and the base are both in the window's hands). One command in flight at a time is the simplest
discipline and the one the standard window uses; more than one is correct too (the engine merges each against the version
named), only wasteful. A `FOXAPI_FAILED` or `FOXAPI_BAD_ARGUMENT` answer means "show the engine's graph".

`FOXAPI_EVENT_PATCH` (10) is raised, as before, in the pass in which the epoch, whether the patch runs or the graph moved;
the graph moving is now the version moving, so the engine's own changes to it raise the event, and `value[0]`, `value[1]`
are unchanged. An interface that reads the facts needs no event.

**8.12.3 The plan.** `Plan::runnable` and the plan's channels and decoders are what the page's marks, its inspector and its
"fed N Hz" lines draw from; none could be derived from `PATCH_PROBLEMS` (which an interface could read, and which
cannot say whether the patch has anything to do). They are served now:

- **`PATCH_PLAN` (45)**, one entry per planned channel, then one per planned decoder, in the plan's order. `id`: the node.
  A channel: `detail` `channel`; `value[0]` its offset from its radio's centre in Hz (signed), `[1]` the rate it outputs,
  `[2]` the decimation from the radio's rate, `[3]` the radio node it is cut from. A decoder: `name` the plugin's key (a
  picture decoder's ends `#image`, as in `PATCH_PARTS`); `detail` what feeds it - `decoder:radio` (an I/Q decoder wired
  to the radio: the whole capture, the radio's centre at DC), `decoder:channel` (an I/Q decoder on a channel: the
  channel's slice, the channel's frequency at DC) or `decoder:audio` (an audio decoder behind a demodulator);
  `value[0]` the rate the plugin is created at, `[1]` the rate of the source, `[2]` the RF centre in Hz at DC (I/Q
  decoders; 0 for audio), `[3]` the radio node it runs on. The plan is compiled against the radios as they are (the
  open device's own rate and centre, or the node's settings before it opens), once a pass.
- **`fact.patch.runnable`**: `1` when nothing blocking was found and there is something to run. The two marks only an
  attempt can find are added after the compile, and they differ, and the difference is kept: **a radio that would not
  open is a problem and clears `runnable`; a plugin that refused to start is a problem (`PluginRefused` in
  `PATCH_PROBLEMS`) and does not** - the page has always drawn "This patch can run." beside a refused plugin, and its
  words say so. (Whether that is right is the owner's; an interface that wants the other reading ORs the two itself.)
- **`PATCH_PROBLEMS` `name`** is the problem's code as a decimal: 0 `NothingFeedsIt`, 1 `NotFedByARadio`, 2
  `NoFrequency`, 3 `OutOfBand`, 4 `RateUnreachable`, **5 `NothingListens` (advisory)**, 6 `NoPlugin`, 7 `PluginMissing`, 8
  `PluginWrongFeed`, 9 `DecoderTooFast`, 10 `PluginRefused`, 11 `NoDevice`, 12 `DeviceTwice`, 13 `RadioFailed`, **14
  `NoTracks` (advisory)**. Advisory problems do not stop a patch running. `detail` is the sentence, in the language in
  force. The codes are append-only, pinned by a test.
- **`PATCH_RADIOS`** gains `value[1]`, an open radio's AIR centre in Hz (its own readback, which may be below 0 Hz through
  an up-converter; 0 for a radio that is not open); `value[2]`, 1 for a radio whose device is still being opened on its
  worker (it was not listed at all until it opened: such an entry has no `ACTIVE`, no `FAULT` and a device name from the
  node's setting); and a `detail` that is the line the face prints under the radio, which for a RUNNING radio is not
  only a fault: the open that refused the rate asked for and runs at its own, or a device that would not take a retune,
  leaves a note (`FAULT` stays the fault's alone - a radio with a note and no fault is `ACTIVE` and not `FAULT`).
- **`PATCH_PARTS`** is hashed by content now, not by count: a module replaced by another keeps the count and used to leave the
  list stale.

**Timing.** The plan, the marks and the radios' figures trail an edit by the pass that applies it and the pass that compiles
it (they are one pass: the edit is a command, applied at the head, and the compile is in the same pass); an interface that
sends an edit and draws in the same frame draws the old plan for a frame. A node an interface adds appears unmarked for a
frame. The spectrum is read at the call (section 7); the squelch gate, the speakers' levels and the decoders' faces are
part 2 (`PATCH_LIVE`).

**What moves `listSeq`, beyond 8.1 to 8.11.** The version (so every change to the graph, whoever made it, moves it; the
hash of the fields, which it replaces as the event's trigger, stays in the fingerprint), the plan (every figure of every
channel and decoder, and `runnable`), the parts bin (every entry's key, name, input, rate, picture and track bits), an
open radio's centre. The block, the facts and the lists are built only when it moves. **Nothing in the publish allocates
in a settled pass**; noticing a change to the graph is one comparison with the newest retained version, and allocates
only when something changed.

**How the window reads it** (what part 1 of the port holds the engine to):

| The window draws or does | From |
|---|---|
| the draft: the graph it edits, the base it was edited from, the version and the epoch | `fact.patch.version`, `fact.patch.epoch`, `fact.patch.graph` of ONE look (the version read again); a draft is never made "in step" with a graph that has not been published |
| a commit | `FOXAPP_OP_PATCH_SET_GRAPH` through `submit_text`, `ival[1]` the draft's base version, the ticket kept; one in flight; `FOXAPI_BUSY` keeps the edit in the draft and sends it next frame |
| the commit's answer | `poll_results`: OK with `applied[0]` the new version, or the refusal that shows the engine's graph |
| a new document (the config's patch, a patch file) | the same op flagged `FOXAPP_PATCH_GRAPH_DOCUMENT` |
| the rail's chip | `FOXAPI_RX_PATCH_RUNNING` and the node count of the look |
| the view is showing, and the receiver face | `FOXAPP_OP_SET_PATCH_PAGE_OPEN` |
| a Radio part's starting device | `FOXAPP_OP_SCAN_NATIVE_ONLY`, then the list it fills, a pass later |

#### 8.12, part 2: the devices, the live readings and the decoders' lines

Part 1 moved the runtime into the pass and served the plan. What the page still read out of the engine's own objects, every
frame, was the rest of what it draws: the parts bin's last bit, the devices a Radio can be set to and the receiver's radio,
the converter in force for a Radio's device, what the running nodes are doing at this moment (a squelch gate, a speaker's
level, a channel's level, a decoder's newest line), which Text out a decoder's line went to, and the recordings a Radio can
play. This part serves them. **Two lists, an extra figure on each of two lists, two facts and two families of facts, one
op.** Decoded pictures stay for part 3.

**8.12.4 The devices a Radio can be set to.**

- **`PATCH_DEVICES` (46)** is the Radio's Device combo as the engine composes it (`Engine::patchDeviceChoices`, the one rule,
  which the engine also names a node's device by): the signal generator, then the I/Q recordings it has listed, the native
  radios (not a Pluto: its address is typed in the Source section), the SoapySDR radios, the sound cards, and the radio the
  receiver has lent to the patch when no list shows it. `id`: index + 1; `name`: the label as the combo letters it, in the
  language in force; `detail`: the device KEY a Radio node names it by (`core/patch_devices.hpp`; **LOCAL sessions only**, a
  REMOTE session gets `""`, because a key names hardware, and a recording's a path, on this machine; cut at 159 bytes);
  `value[0]`: a recording's own rate in Hz (the rate its header says: a Radio on a recording runs at it), else 0. The key
  whole is `fact.patch.device.<id>` (LOCAL), for the id of the entry. A node's label for a key the list does not hold (a
  SoapySDR radio is only listed after a scan; a recording is named by its file whether or not it was listed) is a function
  of the key and the list, `core::patch::deviceLabelFor`, which the engine and the standard window share.
- **`fact.patch.receiverRadio`** (LOCAL): the key of the receiver's radio as a Radio node would name it - the radio lent to
  the patch while the patch runs, else the one the receiver has open, `""` for the generator, a file or a sound card the
  receiver has not opened as a radio. What a new Radio starts on first. The interface asks `FOXAPI_RX_DEVICE_OPEN` for
  "the receiver has it open now" (the list marks that row "lent to the patch when it starts"); the lent one is not open.
- **`fact.patch.receiverCentre`**: the AIR centre in Hz the receiver's radio carries (`%.17g`, so it reads back to the digit),
  `""` when it has never been tuned. **Whether it exists** moves `listSeq`; **its value does not**, because a retune would
  rebuild the block on every tune: the value is as old as the last time something else moved it. The live figure is the
  state's `centreHz`, which is the same number (it is `activeSource().centerFrequencyHz()`); the standard window starts a
  Radio on the state's figure when the fact is not empty.
- **`fact.patch.conv.<node>`**: for every Radio node in the graph that has a device, the converter IN FORCE for that device,
  `"<mode> <LO Hz> <inverted>"` spelled as `fact.converter.live` spells it (mode 0 off, 1 up, 2 down; the LO to the digit it
  reads back by; 1 inverted) - the setting `core::radioCentreTakeable` judges a centre typed on the node by (a sound card's is
  the one its format allows, a recording's the receiver's I/Q-file converter). Absent for a Radio with no device and for any
  node that is not a Radio. It speaks for the device the ENGINE has the node on: a device an interface has chosen and not yet
  sent has none yet, and an interface that judges a typed centre in that window judges it as a radio with no converter, and
  lets the engine be the judge when the edit lands. The setting, the node and the device are inputs of the block's
  fingerprint.
- **`FOXAPP_OP_PATCH_LIST_RECORDINGS`** (0x8422, SETTINGS, **LOCAL sessions only**), no arguments: the engine lists the
  I/Q recordings of the record folder (`fact.recorder.dir`) and of `FOXSDR_PATCH_SAMPLES`, on its own pass - at most 200
  listed and at most 64 files' headers opened by one listing, a file already read is not opened again until it changes - and
  the recordings rows of `PATCH_DEVICES` (and `RECORDINGS` kind 3) change as the answer. Nothing lists them by itself. The
  Radio's device list sends it when it opens, and "Look for radios" sends it with the scan. The listing is now in the
  fingerprint **by what it names** (each file, its key and its rate), not by its number: a relisting that finds other files
  moves the block.
- **`PATCH_PARTS` `value[2]`**: 1 when the module also has a track source (it can put targets on a Map part), else 0. A picture
  decoder is still the key's `#image` suffix (`FOXAPI_LIST_PATCH_PARTS` says so in `kImageKeySuffix`; there is no bit for it).

**8.12.5 What the running nodes are doing now: `PATCH_LIVE` (47).** The page draws four figures at the frame rate that other
threads own: a demodulator's squelch gate, a speaker's level and count, a channel's level, a decoder's newest line. They are
copied by the pass into **a block of their own** (once a pass, on the pumping thread), because they change every pass a patch
runs: carried in the block of lists they would rebuild it - its facts, its plugin sizes - every pass, and make every interface
re-read every list every pass; read at the call from the objects they live in they would be read through a pointer to a radio
that the pass after may have closed. So: **`PATCH_LIVE` never moves `listSeq`, and an interface reads it each time it draws.**
The block has a fingerprint of its own (hashed from the readings, with the texts that can move without a number moving - a
speaker's error): **a pass that finds the same readings builds nothing and allocates nothing**. One entry for each node that
has something; `id` is the node and `value[2]` the kind:

| `value[2]` | Node | `value[0]` | `value[1]` | `name` | `detail` | flags |
|---|---|---|---|---|---|---|
| 1 | a demodulator with a squelch gate running (its channel's radio is open and the channel has run a block) | the level the gate is judging, dB | | | | `ACTIVE`: the gate is open |
| 2 | an audio sink with an output | its newest block's peak, 0..1 | samples written since the output was made | what it plays to (`WAV  patch-4-Name_20260923_101500.wav`, `Playing on ...`; cut at 95) | the output's error, `""` while it takes sound | |
| 3 | a planned channel whose radio runs | the channel's level, dB, measured off its radio's spectrum | | | | |
| 4 | a planned decoder that is running (not one that refused to start) | lines it has said since it was built - 0 for a decoder that has said none, which is itself the answer to "is it running" | | | its newest line (cut at 159) | |
| 5 | an audio sink whose output could not be made | | | | why (the folder cannot be made, ...) | |
| 6 | a picture decoder that has made a picture (8.12.7), whether or not the patch runs | the newest picture's id (`read_image`) | its bytes | the node's name | | `ACTIVE`: the decoder has finished the picture |

A node can have rows of more than one kind (a speaker with an output has kind 2 and, when it also has an error line of its
own, kind 5). The nodes' **spectra** are not here: they are read with `read_node_spectrum`, at the call, as before. A row is
at most one pass old; the bar and the figures cannot show that.

**8.12.6 Which decoder said a line: `DECODER_LINES` `value[0]`.** The originating patch decoder's node id; 0 for the
receiver's own decoders. A patch decoder's line is in the backlog when its Text output reaches a Text out (the rule the page
always had), and now says which decoder it came from, so an interface that keeps a face for each Text out routes a line to
every Text out that decoder is wired to - by its own copy of the graph - and keeps a line from the receiver's decoders off
them all. (A decoder wired to no Text out says nothing here; its face is its row in `PATCH_LIVE`.)

**8.12.7 The decoded pictures: `read_image`, `FOXAPI_EVENT_DECODER_IMAGE` and `PATCH_LIVE` kind 6.** A patch decoder that makes
pictures (a weather-satellite or slow-scan decoder: a module with the plugin ABI's image-decoder capability) has always had
its newest picture on its face on the canvas. The picture reached the face through a hook the engine's pump called into the
window (`EngineHost::onPatchPicture`), which wrote the window's own picture map from whichever thread pumps - and
`read_image`, which 0.2's table has for exactly this, answered `UNSUPPORTED` and `FOXAPI_CAP_IMAGES` was clear. This part serves
the picture through the table and deletes the hook. **`read_image` and `FOXAPI_CAP_IMAGES` are served; one event has its
patch meaning; one `PATCH_LIVE` kind and two appended fields of `FoxImageInfo` are new.** The receiver's own plugins' pictures
(the plugin windows) stay the host process's, with the windows they are drawn in (section 10).

- **`read_image(session, imageId, info, pixels, cap)` is 0.2's contract, kept, and now exact.** It copies up to `cap` bytes of
  the picture the id names into `pixels` and **returns the picture's full byte count**, whatever `cap` copied: a caller that
  gets a count above its `cap` knows it was short and asks again with room. `info` is optional, and so is `pixels` (with a
  `cap` of 0): both absent is the size alone, and `info` alone is the size and the shape. The pixels are tightly packed rows,
  no padding, `width * height * 1` (`format` 1, `GRAY8`) or `* 3` (`format` 2, `RGB24`, R then G then B) bytes -
  `CASCADE_IMAGE_*`'s two formats, which are all a decoder may make; there is no palette. `info->width`, `height`, `format`,
  `bytes` and `caption` (the node's name, cut at 63 bytes) are 0.2's fields. A picture is at most
  `CASCADE_IMAGE_MAX_DIM` (16384) on a side, 805 306 368 bytes: the signed 32-bit return holds every picture the host
  accepts (a larger one is `FAILED`, and none exists). Results: `BAD_ARGUMENT` for a `cap` with no `pixels`, an `info` shorter
  than `FOXAPI_MIN_IMAGE_INFO`, or no session; `NOT_FOUND` for an id that names no picture now - one that was never given, 0,
  one a newer picture has replaced, or a node that has gone; otherwise the byte count.
- **An id names one picture, for good, and the picture is whole or the read is not made.** The engine gives each picture the next
  number of a counter that starts at 1 and is never reused (the id is the picture's identity: a new picture is a new id, so "the
  id moved" is "the pixels changed"), and keeps **the newest picture of each picture-decoder node** - not a history: once a
  newer picture has replaced it, the older id is `NOT_FOUND` and a reader that fell behind skips to the newest, as the taps do.
  An interface that must not miss a picture reads on the event; one that draws reads the row (below).
- **How a reader can never see half a picture (the store's threading design).** A picture is produced on whichever thread the
  decoder's radio reads on, handed to the engine's pass by the runner (the newest per node, as ever), and **moved in whole**
  behind a `const` pointer in an immutable set that the pass builds when something moved and swaps into the host in one step.
  A reader takes the set's pointer under the host's short lock and copies the pixels outside it, on its own thread: the picture
  it holds is never written again by anyone, so what it copies is whole, and a picture the pass replaces meanwhile is still
  whole in its hand. The pass waits for no reader and no reader for the pass; a settled pass builds nothing and allocates
  nothing, and a new picture costs a bounded handful of allocations that does not grow with its size (the pixels are moved, not
  copied: a 1.4 MB picture costs the publish under a kilobyte).
- **`FOXAPI_EVENT_DECODER_IMAGE` (6, 0.2's event) is raised once for a picture that arrived, in the pass that took it,** after
  the set that holds it is in place, so an interface that is told can read at once. For a PATCH decoder: `target`: the patch
  node; `value[0]`: the image id; `value[1]`: 1 (a patch node - the receiver's own plugins' pictures are not served, section 10);
  `text`: the node's name. An interface subscribes with `FOXAPI_EVENT_MASK(FOXAPI_EVENT_DECODER_IMAGE)` like any other event,
  and a full ring raises `FOXAPI_EVENT_OVERFLOW` as for any other, which is why the row below exists. `FOXAPI_CAP_DECODERS` stays
  clear: it also promises the text events and a decoder list, which this engine serves as `DECODER_LINES` and not as events.
- **`PATCH_LIVE` kind 6 is the state to the event's edge** (section 8.12.5; it moves no `listSeq`, an interface reads it each time it
  draws): one row for each node that holds a picture. `id`: the node; `value[0]`: the newest picture's id; `value[1]`: its byte
  count (a reader's buffer, in one call); `name`: the node's name; `ACTIVE`: the decoder has finished the picture (`complete`).
  A session that opened after the picture arrived has no event for it and reads the row; so does one whose event ring
  overflowed. **The row, and the picture, stay while the node is in the graph - whether or not the patch runs**: STOP does not
  take the picture off the face (as it never did), and nothing more arrives. A node the graph no longer holds takes its
  picture with it (a pass later than the removal at the most); **a patch document takes every picture** - the nodes of a document
  are new, and may carry the old ones' ids, so a picture is not shown on a node that is not the one that made it (an edit with
  `SET_GRAPH` that is not a document keeps them).
- **`FoxImageInfo` grew at its end (rule 4):** `complete` (0 while the decoder is still receiving the picture - the plugin
  ABI's `complete` - 1 when it is finished), a reserved word and `sequence` (the decoder's own count of the picture's frames). A
  caller that gives `FOXAPI_MIN_IMAGE_INFO` bytes gets 0.2's fields and none of these; the returned `structSize` says how much was
  written. `FOXAPI_MIN_IMAGE_INFO` is unchanged.
- **REMOTE sessions.** A picture is decoded traffic, like the lines of `DECODER_LINES`, which a REMOTE session already reads (the
  decoded text is not a path or a device on this machine, so nothing is withheld from a REMOTE session and nothing is
  `LOCAL`-only); the web remote's page already shows the receiver's plugin pictures. So `read_image`, the event and the row are the
  same to a REMOTE session as to a LOCAL one, and `FOXAPI_GRANT_VIEW` (every session has it) is the only grant needed. There is
  no reduced picture: a transport that must limit a link slices the bytes it forwards (TRANSPORTS.md's image message is on
  request after the event). The node's name in `caption`, the event's `text` and the row's `name` is the name the graph already
  shows a REMOTE session in `PATCH_NODES`.
- **What the standard window does with it.** It no longer keeps the picture. A Decoder face asks `PATCH_LIVE` for its node's picture
  row; when the id differs from the texture it holds, it reads the picture with `read_image` (the row's byte count sizes the
  buffer), checks that the shape describes the bytes it got, uploads them on its own thread - where the GL context is current,
  as before - and lets the pixels go: only the texture and the picture's figures are kept. A `NOT_FOUND` (the picture moved on
  between the row and the read), or an answer that is not a picture, leaves the texture on the face as it is and the next
  frame asks again of the row.

**What moves `listSeq`, beyond 8.12.3.** The track bit (it was already hashed, and is now served), the listing of the
recordings by content, the receiver's lent radio (whether it is lent, its kind, its arguments and its label), whether the
receiver's radio has ever been tuned, and the devices the lists above already hashed. Not the readings of `PATCH_LIVE` (a picture is one of them), not
the value of `fact.patch.receiverCentre`.

**How the window reads it** (what part 2 of the port holds the engine to):

| The window draws or does | From |
|---|---|
| the parts bin (the keys, their twins, the labels and tips), the Decoder inspector's plugin combo, the Map part's sources | `PATCH_PARTS` (input in `value[0]`, rate in `value[1]`, track bit in `value[2]`, picture in the key's suffix), as the core's `DecoderInfo` list |
| the canvas's marks, "This patch can run", "N channel(s), decimate by D", "fed N Hz", the node's problems, a Display's slice and markers | `PATCH_PLAN`, `PATCH_PROBLEMS` and `fact.patch.runnable`, as the core's `Plan` |
| a Radio's face and inspector: its rate, its air centre, "opening...", the line under it, the START lamp's "n of m radios open", a decoder that refused to start | `PATCH_RADIOS`, `PATCH_NODES`' `REFUSED`, `FOXAPI_RX_PATCH_RUNNING` |
| a Radio's device label, the Device combo, a recording's rate, "lent to the patch when it starts", the starter Radio's device and centre | `PATCH_DEVICES`, `fact.patch.receiverRadio`, `fact.patch.receiverCentre` with the state's `centreHz` and `FOXAPI_RX_DEVICE_OPEN` |
| a typed centre judged (and the sentence that refuses it) | `fact.patch.conv.<node>` |
| the gate, the channels' numbers, the speakers' bars and seconds, a Spectrum part's trace and waterfall, a decoder's count and line, a speaker with no output | `PATCH_LIVE` once a frame, `read_node_spectrum` for the radio's spectrum (the cursor is the newest sequence held) |
| a Text out's lines | `DECODER_LINES` (the window's copy of it) and `value[0]`, routed by the draft's wires |
| a picture decoder's picture | `PATCH_LIVE` kind 6 (the id, the byte count), then `read_image` when the id has moved - uploaded once for each picture |
| the Sound goes to combo, "Files go in ..." | `AUDIO_DEVICES`, `fact.recorder.dir` |
| START, STOP, ALL OFF, "Look for radios", the device list opening | `FOXAPI_OP_PATCH_RUN`, `FOXAPI_OP_PATCH_ALL_OFF`, `FOXAPP_OP_PATCH_LOOK_FOR_RADIOS`, `FOXAPI_OP_SCAN_DEVICES`, `FOXAPP_OP_PATCH_RADIO_LIST_OPENED` and `FOXAPP_OP_PATCH_LIST_RECORDINGS`, through the session: applied by the next pass |

The Map part's tracks and paths are the host process's (the plugin ABI polls a track source from the interface's thread at the
frame rate); the Map's chart, the waterfall's memory and the pictures' textures are the window's own (the pictures themselves are
the engine's, 8.12.7).

### 8.13 The GPS row, the usage switch and the diagnostics bundle as they draw

The position row (a port field, a baud, a key and a status line), the Usage reporting row and the Copy diagnostics key read the
table only. 0.3 had the position (8.11.1), `FOXAPI_OP_GPS`, `FOXAPI_OP_SCAN_SERIAL_PORTS` and the `SERIAL_PORTS` list, the
status line `gpsRefusal`, `FOXAPP_OP_CLEAR_STATUS` and `FOXAPP_OP_TELEMETRY_CONSENT`. It lacked the GPS reader's own account of
itself, a read of the usage switch, and two counters. **One list, one status code and four facts are added; no op.**

**8.13.1 The GPS reader: `GPS` (48) and `FOXAPP_STATUS_GPS_RESULT` (6).** ONE entry, always (an idle reader is an entry that says
nothing), read AT THE CALL from the engine's reader - an engine-lifetime member behind its own mutex, so a live read can neither
dangle nor wait on the pass - and never part of the lists' block: the seconds and the sentence of a listening read change every
second and must not rebuild the block or move `listSeq`. The state, the flags and the figures come from ONE copy of the reader's
status, so they cannot disagree. `name` (the port given to the last start) and `detail` (the sentence, worded by the core's
`gpsStatusLine` so its wording is tested once) name a device on this machine and are **blank to a REMOTE session**, which
still gets the state, the figures and the flags. No coordinate is anywhere in it: the position a fix gives is the state's
(`FOXAPI_RX_POSITION_SET`, `rxLatDeg`, `rxLonDeg`, 8.11.1), and the log and the sentence never carry one (PRIVACY.md).
`FOXAPP_OP_CLEAR_STATUS` code **6** forgets a finished read (the reader's own `clearResult`: a no-op while it listens); a port or a
baud edit sends it with code 1 (the refusal), in that order, and both land in one pass. Started and stopped with the existing
`FOXAPI_OP_GPS` (start: `ival[0]` 1, `text` the port, `num[0]` the baud; stop: `ival[0]` 0). The serial ports the drop-down and the Serial ports
section list are the engine's, asked for with `FOXAPI_OP_SCAN_SERIAL_PORTS` and read from `SERIAL_PORTS` (the window used to walk the
registry itself in four places; the engine's comment already said it did not).

**The hook that died with it.** `EngineHost::onGpsFixApplied` (the engine told the window, from its own pass, to open the rail's
position fold) is deleted: the interface sees the entry go to Fixed. The same cue also starts its coverage map again, because the
old hook cleared it for every fix, including a fix equal to the position already held - a re-apply the state cannot show
(`seq` does not move for the same pair).

**8.13.2 The usage switch: `fact.telemetry.enabled` and `fact.telemetry.installId`, both LOCAL.** `1`/`0`, and the id the
engine minted at the moment of consent (`""` while reporting is off - the id does not exist then; OFF deletes it, so a later
opt-in cannot be tied to the old one). Consent is given at this machine: both facts answer `FOXAPI_DENIED` to a REMOTE session,
`FOXAPI_OP_TELEMETRY_ENABLE` stays refused from everyone, and `FOXAPP_OP_TELEMETRY_CONSENT` (the desktop's own op, a REMOTE
session is refused) is unchanged - ON while already on answers `NO_CHANGE` and changes nothing. All four facts of this section
are in the lists' fingerprint (a consent changes the block once).

**8.13.3 The diagnostics bundle: `fact.diag.launches` and `fact.diag.crashes` (LOCAL), and the context.** Decimals; they move
`listSeq` when they change (at start-up, from the configuration). The bundle's context lines are no longer re-derived by the
interface: they are **the engine's block** (`core::diagContextBlock()`, the very text a crash or a hang report prints), which
the engine's own pass renders - at the end of `applyConfig` and then at most once a second, and only when something it describes
changed (the mode, the source, the rate, the radio, the loaded plugins), so a settled pass allocates nothing. Staleness: the block
can be up to a second old, where the press used to render it. `core::buildDiagnosticsBundle` no longer calls `setDiagContext`:
**a bundle is a read, and it used to replace the crash handler's copy of the context** with whatever the caller had built (the
window's re-derivation of the engine's - the same code today, so nothing visible went wrong; proved red in
tests/test_diagnostics.cpp before the change). `renderDiagContext` is the pure rendering `setDiagContext` stores.

The interface-side differences, by the code and the tests (tests/test_gps_window.cpp, tests/test_usage_diag_window.cpp): the
status line of a finished read clears a pass or two after a port or baud edit; the key turns to Stop a pass or two after the press
(it did one frame late already); a click on the usage checkbox shows at once (the interface keeps the value asked for until the
fact says it, half a second at most) and the chip, the lamp and the install id follow a pass later; the serial-port list
appears a pass after the arrow is pressed (nothing is said, and not "none found", until the scan's answer is in).

### 8.14 A preset applied, and the track-info plugin's lines (the pump move, step P2)

Two hooks the engine used to call into the interface from its own pass became the interface's own to answer, on its own thread.

**8.14.1 `FOXAPI_EVENT_PRESET_APPLIED` (12).** A plugin's preset was applied: the engine tuned, set the mode, bandwidth and device
rate the preset asks for, rebuilt the decoders against the receiver they are now pointed at - and an interface is told, once for
each application, so it can open what that plugin contributes (a map page, the picture window, a panel, an instrument, or the shared
Decoder output window for a text decoder with no window of its own). `text`: the plugin's **module file name** - `PLUGINS`' `detail`,
the key every plugin op and grant takes (a display name is the plugin's own choice and is not unique). `value[0]` (stub campaign B5): the
applied preset's `CASCADE_PRESET_*` flags word as a number (0 for a preset that sets none) - the interface needs it to know whether the preset asks
for the shared Decoder output window beside the module's own (`CASCADE_PRESET_SHOW_TEXT_OUTPUT`). No other field is set.
- **When.** Raised by the `endPass` of the pass that applied the preset, after the publish that carries what it changed - the new
  frequency, the decoder rows (`DECODERS`, `PLUGINS`) - so an interface that is told finds them already in the lists. A preset applied
  by any route raises it: the `FOXAPI_OP_PLUGIN_PRESET` command (a preset bar's key, the browser's `pluginPresetName`), a user preset
  (`FOXAPP_OP_USER_PRESET_APPLY`), and the automatic preset when a plugin is started or its window opened (`FOXAPP_OP_PLUGIN_AUTO_PRESET`).
- **Who gets it.** A session that subscribed to it (`FOXAPI_EVENT_MASK(FOXAPI_EVENT_PRESET_APPLIED)`), any grant and any kind of
  session: the event names a module the session can already read in `PLUGINS`. The standard window subscribes, finds the module's row
  by its file name and opens its windows; a session that does not care ignores it.
- **What it replaces.** `EngineHost::openPluginWindowsFor`, a call the engine made from inside `applyPluginPreset` into the window,
  which wrote the window's map pages and window set from whichever thread the pass ran on. The windows now open when the interface
  looks - one frame after the press, where the hook opened them in the frame of the drain.

**8.14.2 `FOXAPP_OP_TRACK_INFO_LINE` (0x8423, VIEW, LOCAL sessions only).** `text` (or long text): one status line from the
track-info plugin ("cannot reach the registry"). The engine appends it to the Decoder output backlog as "Aircraft info", counted like
every other line (`decoderLinesTotal` moves, `DECODER_LINES` serves it, `listSeq` does not). An empty text is `FOXAPI_BAD_ARGUMENT`. The
interface that holds the plugin's cache (the standard window's `TrackInfoCache` is GUI-thread state) polls the plugin's text in its own
frame and sends each line; the engine used to ask for them through `EngineHost::drainTrackInfoText` on every pass.

### 8.15 The plugin UI is the interface's thread (the pump move, step P3) - what a plugin author and an interface may rely on

No new op, list, fact or event: the engine API is unchanged. What moved is WHICH THREAD touches a plugin's GUI capabilities, and with it
three promises of the **plugin ABI** (`src/core/plugin_abi.h`, `docs/PLUGIN-API.md`, appended - nothing above them changed, and
`CASCADE_HOST_API_LEVEL` stays 1: the table's shape and every function in it are as they were). Decided by the owner (the pump move's
survey, decisions 1 and 2) and adopted with the recommendation, each reversible:

**8.15.1 `request_tune` is queued; `CASCADE_TUNE_OK` means ACCEPTED (decision 1).** The legacy host call used to retune the receiver on
its caller's thread - the GUI thread inside a track source's `poll_tracks`, or a plugin's own worker - which is a write into the
receiver from outside the engine's pass. It is now checked and queued like the level-1 `set_frequency`: the refusals are answered in the
call, as always (`DENIED` for a stopped plugin or no grant - and the refusal is recorded for the Fitted modules plate; `OUT_OF_RANGE`;
`NO_DEVICE` when no source has a rate; `FAILED` for a detached host or a full queue), and everything else is `OK`, meaning the retune is
queued for the pass. The pass applies it at its next plugin phase - **within one pass, 16 ms** - after checking the grant and the stop set
again (a grant withdrawn between the call and the pass stops the retune silently) and that a source is still open, then retunes the
source's centre exactly as the call used to. **One request is held per plugin**: a second call before the pass applies the first
replaces it (the newest frequency is the one that matters; a corrector that asks once a second is unaffected, one that asks every frame
cannot fill anything). The ABI text now says all of this next to `CASCADE_TUNE_*`, and the plugin author's guide's "Threads" section. A
plugin that must know the receiver has moved reads `centre_hz()` or the level-1 state's `centreHz`/`tuneSeq`; the centre `centre_hz()`
answers is the published one (at most one pass old), not a read of the source from outside the pass.

**8.15.2 The GUI capabilities run on one thread (what a host now guarantees).** A track source, panel, instrument, basemap or track-info
instance, and the host-client `attach`, are created, polled and destroyed on the interface's (GUI) thread - `create`/`destroy` between
frames, never from inside or beside one of that instance's own `poll_*`/`get_*` calls - where they used to be created and destroyed from
the host's control thread. A host with no interface (a headless engine) still creates and destroys them from its control thread and never
polls them. Decoders are unchanged.

**8.15.3 A module is never unmapped while one of them is alive (the unload handshake; decision 2).** Before a rescan, a removal or the
shutdown unmaps the modules, the engine posts a RELEASE directive and waits for the interface to say it has destroyed every instance and
let go of the basemap and the track-info client - **bounded at 250 ms**, with the control thread's watchdog paused across the wait, and
ending at once if the control thread is told to stop. **If the wait runs out the modules are NOT unmapped**: the pass writes one
warning, goes on running at its own period, and retries when the interface frames again (it does not wait again while the interface is
silent). The shutdown does not wait on an interface that is no longer framing: it joins the control thread first and then releases the
plugin UI on the interface's own thread, as it always did.

**What an interface does.** The standard window applies the engine's newest plugin directive at the head of each frame and at the head of
its plugin poll (`AppWindow::applyPluginDirective`), which is also the acknowledgement; it owns its `PluginUi` (built over the engine's
API core), the basemap's tiles (GL textures - every GL delete they cause is on its thread) and the track-info cache. An interface that
holds none of this has nothing to do: the engine drives its own plugin UI, inline, when no interface has installed an applier
(`Engine::setPluginUiApplier`).

### 8.16 Web and CAT become engine sessions - the table's side (engine extraction stage 5a)

The web server and the CAT listener are going to be part of the engine (docs/TRANSPORTS.md 2.6), each holding sessions of its own on this table in place of the private queues and pull they use today. Stage 5a is the table's side of that and nothing else: **no server has moved and no product behaviour changes** (the browser page, `/api/control`, CAT and the window's settings panels are exactly as they were, running over the old chain). What 5a adds is everything the servers will need to stand on, each held by tests/test_engine_api_05a.cpp, tests/test_internal_session_order.cpp and F12 of tests/test_engine_publish_alloc.cpp. The owner's decisions D1-D10 of the stage-5 survey were adopted as recommended; the ones this section carries out are named.

**8.16.1 `read_audio` is served (`FOXAPI_CAP_AUDIO_STREAM`).** Mono, 48 kHz, float (`FoxStreamInfo`: `channels` 1, `rateHz` 48000), for any session that holds `FOXAPI_GRANT_AUDIO` - a LOCAL or a REMOTE one alike, because the audio is decoded traffic like `DECODER_LINES` (the 0.2 text's "remote clients get it at a reduced rate" is not done: a REMOTE reader gets the same 48 kHz, which is what the browser's `/api/audio` has always carried). The contract is the 0.2 header's, call for call:
- **No `AUDIO` grant: `FOXAPI_DENIED`.** A null cursor, or a null buffer with a count, or an `info` shorter than `FOXAPI_MIN_STREAM_INFO` is `FOXAPI_BAD_ARGUMENT`, and the short `info` is refused **before the cursor moves**, so no audio is lost to the refusal. A count of 0 with no buffer is a legal call.
- **Cursor 0 joins LIVE:** the call returns 0 frames and leaves the cursor at the stream's end. Any other cursor reads the frames after it and advances it by exactly what it returned. The stream's first frame is a frame of silence the engine writes when the ring is made, so a joined cursor is **never 0** (a client that joined an empty stream and kept 0 would join again at its next call and lose whatever came between); `written` counts it.
- **A reader that falls behind** the ring (96 000 frames, two seconds) is resynced to the oldest frame it still holds, and `dropped` says how many frames it missed (`oldest - cursor`, exactly). Nobody else is ever made to wait for a slow reader, the pass included. `written` is the frames the ring has been given in all.
- **Where the frames come from.** The ring is `net::AudioRing`, the browser's own (net/web_audio.hpp), filled from the pipeline's 4096-frame audio tap by its monotonic produced counter exactly as `AppWindow::publishWebAudio` did it (the same cap on one copy, the same "re-mark when the counter goes backwards") - by whoever looks first: every `read_audio` tops the ring up itself on the reader's thread (read-time, as `read_scope` reads its tap), and the pass tops it up once a pass, in `endPass`, so a reader that polls more slowly than the tap's 85 ms still has everything. The counter and the tap are two reads, so a block that lands between them shifts the copied span by that block (a few frames repeated, the same few missed - as it was); the COUNT of frames the ring is given is exact, and with the receiver stopped the bytes a reader gets are exactly the tap's newest frames (the test compares them with the old ring's).
- **The pass pays nothing for an engine nobody listens to.** The pass feeds only for `audioDemandMs` (5 s; a test may shorten it with the option of that name) after the last `read_audio`; then it stops, and the first read after a quiet spell marks live again and adds nothing for the gap. A reader that stopped reading with frames unread finds them still there when it returns (up to the ring's two seconds), as a slow reader would. The ring (384 KB) is made by the first read, not with the engine. The feed allocates nothing (F12: 1000 reads and 60 passes with a listener, 0 heap allocations).
- The one lock the pass takes is the pipeline's tap lock for a microsecond copy - the lock the window took once a frame before stage 4 and the DSP thread holds only while it writes a block's frames into the tap. A pass never waits for a reader: it tries the feed's mutex and skips a pass when a reader is feeding at that moment (the reader fed the same frames).

**8.16.2 The capability word, audited.** `FoxEngineApi::capabilities` was set once, at plan step 1, with what the table then did; most of the 0.2 vocabulary was served by later steps and never declared, and an interface that greys out what a bit does not promise (the header's own rule) would have greyed out most of the engine. A bit is now **set exactly when every feature its header comment names is served**, and clear otherwise; the test (K in test_engine_api_05a.cpp) calls the feature each bit promises and fails on a bit with no row.

| Bit | | Why |
|---|---|---|
| `RECEIVER` (0), `SPECTRUM` (1), `AUDIO_LEVELS` (2), `AUDIO_DSP` (7), `EVENTS` (20), `TRANSMIT` (12) | set (as before) | |
| `AUDIO_STREAM` (3) | **set (8.16.1)** | `read_audio` |
| `SOURCES` (5) | **set** | `DEVICES`, `SAMPLE_RATES`, `ANTENNAS`; `SELECT_SOURCE`, `SET_SAMPLE_RATE`, `SET_ANTENNA` |
| `GAINS` (6) | **set** | `GAINS`; `SET_GAIN`, `SET_DEVICE_AGC` |
| `RDS` (8) | **set** | the RDS fields of the state (8.4) |
| `PATCH` (10) | **set** | the graph read back (8.12), `PATCH_RUN`, `PATCH_ALL_OFF`, the whole-document edit `FOXAPP_OP_PATCH_SET_GRAPH` (`PATCH_LOAD` itself is not an op of this engine) |
| `RECORDER` (11) | **set** | `RECORD_IQ`, `RECORD_AUDIO`, `RECORDINGS` |
| `BOOKMARKS` (13) | **set** | `BOOKMARKS`, the add/tune/remove/favourite/group ops (8.6) |
| `SCANNER` (15) | **set** | `SCANNER_RUN`, `SCANNER_SKIP`, `SCANNER_CONFIG` and the scanner's state (8.6) |
| `PLUGIN_STORE` (17) | **set** | `CATALOGUE`, `STORE_FETCH`, `STORE_INSTALL`, `STORE_REMOVE` (8.9) |
| `BAND_PLAN` (21) | **set** | `BAND_PLAN`, `BAND_PLANS`, `SET_BAND_PLAN` |
| `POSITION` (24) | **set** | `SET_POSITION`, `GPS`, the `GPS` list (8.13) |
| `SCOPE` (23), `IMAGES` (25), `FACTS` (26), `LONG_TEXT` (27), `NODE_SPECTRUM` (28) | set (as before) | |
| `IQ_STREAM` (4) | **clear** | `read_iq` answers `UNSUPPORTED` |
| `DECODERS` (9) | **clear** | the header's "text + image events": `FOXAPI_EVENT_DECODER_TEXT` is not emitted anywhere (the decoders' lines are the `DECODER_LINES` list); only the patch decoders' image event is. Set when the text event is |
| `SETTINGS` (16) | **clear** | `SETTING_SET` is the test hooks only; the settings store is not served through the table |
| `REPORTS` (18) | **clear** | `PROBLEM_REPORT` and `FEATURE_REQUEST` are not implemented ops |
| `TELEMETRY` (19) | **clear** | `TELEMETRY_ENABLE` is refused from every origin (the desktop's consent is the one door) |
| `TRACKS` (22) | **clear** | `LIST_TRACKS` is a documented gap (section 10) |

There is no bit for the listeners: `FOXAPI_OP_SERVER_CONFIG` is an ADMIN op, and its list (`SERVERS`, 8.16.5) is a LOCAL session's.

**8.16.3 Engine-internal sessions and their closed op profiles (D1, D2, D5).** The web server and each CAT connection hold a session that no outside client opened: `ApiHost::openInternalSession(InternalKind::Web | Cat, label)` (engine/fox_engine_api.hpp), returning the table's own `FoxSession*` - every other call (`submit`, `poll_results`, `read_state`, `read_spectrum`, `read_audio`, `heartbeat`, `close_session`) is the table's, on it. **It is not in the C table and not in the vendored header: only code inside the engine can open one**, so TRANSPORTS 2.6's rule that a REMOTE session needs a login token still holds for every session a client opens. An internal session:
- has the `FOXAPI_SESSION_REMOTE` flag (it can never latch, name a path or be armed for transmit - every REMOTE rule applies) and **no token**; its principal is `"web"` or `"cat:<id>"`;
- counts against the session limits like any other (the 64 in all and the 16 REMOTE places; `FOXAPI_LIMIT` beyond);
- is bound at open to a **closed op profile**, an allow-list it can never widen, **enforced at submit**: an op that is not on it is answered `FOXAPI_DENIED` as a result (the submit itself succeeds, `FOXAPI_RESULT_REFUSED` set, the message "this session's profile does not include that command") and changes nothing. An op on the profile is not refused by the profile; it is then judged by the table's ordinary rules (the PTT still needs the local operator's consent and a transmitter, a value out of range is `OUT_OF_RANGE`);
- holds the profile's grants: **WEB** `VIEW | TUNE | SETTINGS | TRANSMIT | AUDIO`, **CAT** `VIEW | TUNE | SETTINGS` - never `ADMIN`, and CAT never `TRANSMIT`.

*The web profile* is exactly the closure of `net::controlRequestToCommands` - every op any `ControlRequest` can turn into, and no other - 38 ops (the test derives the closure by setting every field of a request, and compares both ways): `RUN`, `SET_CENTRE`, `SET_MODE`, `SET_BANDWIDTH`, `SET_VFO_OFFSET`, `SET_SQUELCH`, `SET_VOLUME`, `SET_DISPLAY_RANGE`, `SET_DEEMPHASIS`, `SET_STEREO`, `SET_NR`, `SET_NOTCH`, `SET_AUTO_NOTCH`, `SCAN_DEVICES`, `SELECT_SOURCE`, `SET_ANTENNA`, `SET_SAMPLE_RATE`, `SET_GAIN`, `SET_DEVICE_AGC`, `RECORD_IQ`, `RECORD_AUDIO`, `BOOKMARK_ADD`, `BOOKMARK_TUNE`, `BOOKMARK_REMOVE`, `PLUGIN_GRANT`, `PLUGIN_PRESET`, `SCANNER_RUN`, `SCANNER_SKIP`, `TX_PTT`; **three ADMIN ops** (D1) - `STORE_FETCH`, `STORE_INSTALL`, `STORE_REMOVE`, the page's "Get plugins", Install and Remove buttons, which the profile (not a grant) allows; and **six of the application's own ops** (D2) - `FOXAPP_OP_SET_DISPLAY_MIN`, `SET_DISPLAY_MAX`, `SET_NR_STRENGTH`, `SET_NOTCH_FREQUENCY`, `SET_NOTCH_Q`, `SCANNER_RANGE`, the sliders' own, which every other REMOTE session is refused. *The CAT profile* is the four ops rigctld's `F` and `M` send: `SET_CENTRE`, `SET_VFO_OFFSET`, `SET_MODE`, `SET_BANDWIDTH` (D7: CAT keeps its `planTune`, centre and offset in one batch, and does **not** use `SET_FREQUENCY`, which retunes the hardware on every change). Everything else - the other transmitter ops, `TX_LATCH`, `TX_REMOTE_ARM`, `SETTING_SET`, `SERVER_CONFIG`, every other store, application or file-naming op - is `DENIED` to both.

**The public rules are unchanged.** A session a client opened (`open_session`) is judged exactly as before: `ADMIN` is stripped from a REMOTE one, the application's ops are refused it, the three store ops and `SERVER_CONFIG` are `DENIED`, `file:` / `pluto:` / `open-rtltcp:` sources are refused it. TRANSPORTS 2.1's "REMOTE sessions never hold ADMIN" stays true of the new WebSocket transport; the browser page's compat session is the one session that may send the three store ops, and only because its profile lists them (TRANSPORTS 2.6 should say so). D10: the Pluto / rtl_tcp rows the browser lists are refused to its session as to any REMOTE one (`SELECT_SOURCE` with an address-bearing id); the release notes say so when the page moves.

**8.16.4 The internal tier (D3, D9): the P1 ordering guarantee, structural.** Submits from internal sessions go to **a queue of their own**, and `beginPass` does not drain it. `ApiHost::applyTail()` does, at the place the web remote's and CAT's pull has always had in the pass (`Engine::pumpRemoteControls`, which since stage 5d (8.16.8) is nothing BUT this call, after the local queue, the API queue and the inputs, before the plugins' requests and the scanner): the web session's commands first, then the CAT connections', each session's in the order it submitted them. Each is applied through the table's one `apply` - the same range checks, key rules and `Engine::applyCommand` as every session's - so what the tier adds is *when*, not what. **Then the key drive runs again** (`txPass`): the pass's drive sits at the end of `beginPass`, before the tier, and the old pull released a PTT 0 itself, while the table's way is to decide in the rules and let the drive release the Transmitter - without the second drive a browser's PTT 0 would keep the radio keyed until the next pass. The drive is idempotent and runs only when the tier had something.

The guarantee this makes true is the one the old comment promised and nothing pinned: **a browser that arrives first in wall time still meets its key** - the desk's press lands in the same pass and wins it (rule 6) - because the tier is applied after the local and API queues whenever it arrived (`test_internal_session_order` A1, and B with the control thread parked so both submits are certain to be in ONE pass: red against a tier drained in arrival order). The table's own queue contract is unchanged for every other session (the merge sweep of test_engine_api_merge). The overflow rule is the table's: the **newest** command past `FOXAPI_MAX_PENDING` (256 unread) is `BUSY`, the oldest 256 stand, and the five kinds that only make the transmitter safer are never `BUSY` (one outstanding of a kind, merged); the servers' old "drop the oldest at 64" is gone with their queues. A server must `poll_results` on every request or command and on an idle tick (10 000 commands with the results drained are never `BUSY`: C of test_internal_session_order).

*Liveness.* The internal session's key liveness is its `heartbeat`, as for any session: the remote keep-alive (2 s) is judged from it, and a PTT re-assertion is not one (E1 of test_internal_session_order: a browser that re-asserts without polling loses the key at 2 s; one that polls keeps it). The web server's call is `heartbeat` on an authorised `GET /api/status` and nowhere else.

**8.16.5 `FOXAPI_OP_SERVER_CONFIG`, the list `SERVERS` (49) and the facts (D8).** `SERVER_CONFIG` (0x1001, grant `ADMIN`, **LOCAL sessions only**: a REMOTE session and every internal session are `DENIED`) is implemented. Its text is a JSON object (by `submit_text` when over 255 bytes), every member optional and meaning "leave alone" when absent:

```
{"web": {"enabled": true, "bindAddress": "127.0.0.1", "port": 8073, "username": "admin",
         "password": "...", "passwordRecord": "pbkdf2-sha256$...", "clearPassword": true},
 "cat": {"enabled": true, "bindAll": false, "port": 4532}}
```

The parse is strict (a control request is a live instruction, not a file): an unknown key at either level, a wrong type, a port outside 1024-65535, a `bindAddress` the policy does not accept (`net::isAcceptableBindAddress`), a password under eight characters, a record that does not parse, more than one of `password` / `passwordRecord` / `clearPassword`, or an object that says nothing is `BAD_ARGUMENT` with the reason in the result's message and **nothing changed**. An empty `bindAddress` or `username` resets to the default (the config's own rule: a lost value must never widen the binding). The same configuration said again is `NO_CHANGE`. Whether the configuration may start is the policy's, not the op's: the configuration is stored as given, and `fact.web.policy` carries the sentence the policy refuses it with (0.0.0.0 with no password, say).
- **The password is hashed on a worker.** `"password"` is accepted at once (`OK`, "the password is being hashed and its record will follow"), PBKDF2-HMAC-SHA256 at 600 000 iterations runs on a worker the pass polls (`Engine::pumpWorkers`) - never in the pass: a pass stalled past a second unkeys the transmitter's thread, and the click that used to run the hash ran it on the GUI thread - and `fact.web.hasPassword` turns on when the record exists (`fact.web.passwordPending` is `1` meanwhile). One hash runs at a time; of several passwords the newest wins. **The plaintext is never stored or published and the record is not published**; only whether a record exists is. A record handed in whole (`passwordRecord`, a restore) is stored as it is; `clearPassword` removes it.
- **The configuration is the engine's** (`Engine::servers()`, engine/server_host.hpp): restored by `applyConfig`, written by `fillConfig` into the AppConfig fields `web*` and `cat*` (the file's keys and the password-record format are unchanged). Since 5b the engine is the only writer: the window keeps no copy of them (its panels read the facts below, and its `currentConfig` takes the engine's half whole).
- **The list `SERVERS` (49), LOCAL sessions only (`DENIED` to REMOTE).** Always two entries, web then CAT. `id` 1 / 2; `name` `"web"` / `"cat"`; `detail`: the sentence the last start was refused with (`""` when none; it never holds a password); `value[0]`: the port (the one it listens on, else the configured one); `value[1]`: browser sessions (web) or connected clients (CAT); `value[2]`: browsers listening to the audio (web; 0 for CAT); `value[3]`: commands received; `ACTIVE`: listening; `FAULT`: enabled and refused to start. `value[1..3]` are read at the call and move nothing; the rest is part of the lists' block (a listener's report moves `listSeq`, traffic does not).
- **The facts, all LOCAL:** `fact.web.enabled`, `.bindAddress`, `.port`, `.username`, `.hasPassword`, `.passwordPending` (`0`/`1`), `.reachable` and `.authRequired` (what the policy makes of the configuration, `1` only when it would be allowed to start; "reachable" is off this machine), `.policy` (the policy's refusal sentence, `""` when allowed), `.addresses` (the machine's interface addresses the web server answers on, one per line, as the listener reported them); `fact.cat.enabled`, `.bindAll`, `.port`.
- **What reports into it, and who starts them (5b).** The engine OWNS the listeners (`ServerHost` holds the `net::WebServer` and the `net::CatServer`). A **worker thread of the host's own** - never the pass - makes them match the configuration: a `SERVER_CONFIG` (even one that changes nothing: the operator asking again is how a refused port is retried), the restore of the config file (`Engine::applyConfig`) and the landing of a password's record each wake it, it takes the configuration as it stands at that moment, and it starts, restarts (a changed port, binding, account or password record; a running server with the configuration already in force is left alone, so no browser session is lost) or stops what differs. `WebServer::stop()` can block for the read timeout (10 s) when a browser holds a request open: that wait is the worker's. A restart waits for a password's hash in flight (so the server comes up once, with the record the operator just set); switching OFF is never held back. A start the policy or the socket refuses is **a report, not a result**: the listener says so through `ServerHost::setWeb` / `setCat` (running, bound port, the refusal sentence, the interface addresses of an off-machine binding), the `SERVERS` entry is `FAULT` with the sentence in `detail`, and nothing tries that configuration again until the operator says it again. `value[1]` / `value[2]` / `value[3]` are read off a running server **at the call** (the host's sampler, `Counters::sample`); the receiver state's `FOXAPI_RX_WEB_LISTENING` bit is the web server's `running()`. The refusal sentence in `detail` is cut at 159 bytes (a list item's size): `fact.web.policy` carries the policy's sentence whole, and an interface that draws the refusal takes it from there when `detail` is its beginning. **An engine created with `testHooks=1` hosts no listener** unless it also asks for `servers=1`: a hermetic engine opens no socket, whatever it is configured with.

**8.16.6 What stays, and what the next group needs.** Unchanged and held by the suites that held them: every test of test_remote_control_pull and test_remote_key_release (at 5a the old pull was still there, and still first in its phase; 5d deleted it, 8.16.8, and these suites were re-pointed at the sessions with their assertions kept), the dead-man suites, the merge sweep. Not done by 5a, and what 5b did and did not do (docs/engine-stage4.md 6.10): the servers moved into the engine (`ServerHost`) and are configured, started and stopped through this section; **control still flowed by the old private chain** - `Engine::applyControlRequest`, the pull (armed over the engine's own servers by the composition root, `Engine::installServerPull`), `webRemote*` and `FOXAPP_OP_WEB_CONTROL_STOPPED` were all still in use (5c replaced CAT's share with sessions, 8.16.7; 5d replaced the browser's and deleted them, 8.16.8); the web's status JSON is still composed by the engine's Full-block composer (D4). 5b needs the engine to host the listeners (and report through `servers()`), 5c a `CatRadio` fill from `read_state` on the connection's session (`net::catRadioFromRx(FoxReceiverState)`, held equal to the window's snapshot read by test_engine_api_05a; `executeCatLine` and `catDumpState` take the six-field `CatRadio` now and no longer include the web server), and 5d the translator on the HTTP thread and the heartbeat on the status route.

**8.16.7 CAT is on the tier (stage 5c; docs/engine-stage4.md 6.11).** Each accepted CAT connection holds an internal `Cat` session (8.16.3): opened by the acceptor thread right after the four-client check (an engine that answers `FOXAPI_LIMIT` refuses the connection as the cap does), closed by the connection's own thread before the client count drops - so when `CatServer::stop()` returns, no session of the server exists. The connection calls only the table, on that session: `read_state` for the six figures of every line (one retry on `FOXAPI_BUSY`, then the last good figures; before the first publish, `seq` 0, the default figures a reader of the snapshot always got), `submit` for a set's commands, `poll_results` after every request and on every idle tick of the connection (200 ms) - a session that does not collect its results is answered `BUSY` past 256 outstanding commands - and `close_session` last. No heartbeat: a CAT session never holds a key, and only a key holder's liveness is judged. `F` keeps its tune policy and is **never `SET_FREQUENCY`**: inside the sampled band only the VFO moves (`SET_VFO_OFFSET`), outside it the centre moves and the offset returns to 0 (`SET_CENTRE` then `SET_VFO_OFFSET` in one batch); `M` is `SET_MODE` then `SET_BANDWIDTH`; `T 1` stays `RPRT -11` and the profile's four ops can never transmit, latch, arm, open a transmitter or name a path. **A connection that closes waits for its commands**: a closed session has its queued commands refused (`DETACHED`, 8.16.4), and a one-shot client (`rigctl F ...`: send, read `RPRT 0`, hang up) would otherwise never retune the radio, so the connection waits - at most `kCatSessionDrainMs` (1 s), and not at all when the server is stopping - for its outstanding results before it closes. The CAT queue (64 deep, the OLDEST dropped) is gone; the overflow rule is the table's (D9): the NEWEST command past 256 outstanding is not taken, and the wire still answers what it always answered (`RPRT 0`).

**8.16.8 The browser is on the tier (stage 5d; docs/engine-stage4.md 6.12).** The web server holds **one** internal `Web` session while it runs, shared by every browser (D5): opened by the thread that starts the server once the bind has succeeded (an engine that answers `FOXAPI_LIMIT` refuses the start, with a sentence, as it refuses a port it cannot bind) and before a handler can run, closed by the thread that stops it after the last thread of the server has ended - so a stopped server holds none, a cookie login or logout touches none (17 browsers are one engine session), and **closing it is what releases a key the browser held**: the table opens the key a closed session held at the start of its next pass. The private chain is gone: `Engine::applyControlRequest`, the pull (`RemoteControls`, `setRemoteControlSource`, `ServerHost::pull`), `ApiHost::webRemotePtt` / `webRemoteStopped` / `webRemoteBeat` with their fake session id, `FOXAPP_OP_WEB_CONTROL_STOPPED` (0x841C, retired, not reused), the server's own queue (`takePendingControls`, `kMaxQueuedControls`: 64 deep, the oldest dropped) and the stop-time queued release. The `TX_PTT` arm of `Engine::applyCommand` **refuses** (`DENIED`, "the transmit key is pressed through a session, not applied directly"): a key is judged for a session.

- *`POST /api/control`.* The HTTP thread authenticates, checks the content type, parses **strictly** (an unknown key is 400, `{}` is refused, the latch is refused as a decision), keeps the **409 gate** as the UX check against the status the page is shown (its sentence is unchanged and pinned byte for byte), then **translates** (`net::controlRequestToCommands`, unchanged: its output type - the public ops and the profile's application ops - is why its tests survive) and submits the request's commands to the session in **one batch** (a table submit is atomic: another browser's batch cannot come between them; a command whose text is over the slot goes by `submit_text`, one at a time). The answer is **202 `{"accepted":true}`** whatever the engine then decides: a refusal - the profile, a range, the key rules, `BUSY` - is a *result* on the session, collected and **not surfaced** to the browser, as it never was. A server with no session answers 503.
- *The context is taken at the post.* The bookmark rows a request names are resolved to ids from the Full block the status is being served from, **when the request is POSTED** (the pass resolved them when it applied the request, a pass later - the same block's rows); a scan that is started carries the range the last publish stored (`read_state`'s `scanStartHz` / `scanStopHz` / `scanStepHz`) with the request's own range fields merged in by the translation. A read that keeps overlapping a publish (four tries) is a 503 and nothing is handed over.
- *The heartbeat is the status poll and nothing else.* An **authorised** `GET /api/status` calls the session's `heartbeat`; a control request - a PTT re-assertion above all -, a login, a refused poll and every other route do not (API 0.2). The remote keep-alive (2 s) is judged from it, so a page that keeps pressing and has stopped polling loses the key at 2 s counted from its last poll, hold renewed or not (test_transmit_dead_man 7e; test_remote_key_release F against the real server).
- *Results are collected* around every request that touches the session (once before the hand-over, once after) and on every status poll: a browser that only ever posts cannot fill the session. And on an **idle tick**: while the server runs a thread of its own collects them every 200 ms whether or not a browser asks (it is joined before the session closes), so a session is never left holding results nobody reads.
- *Overflow is the table's* (D9): the newest command past 256 outstanding is answered `BUSY` and not taken, the five safer kinds (`TX_PTT 0`, `RUN 0`, ...) never are; the server's old "64 deep, drop the oldest" is gone.
- *Order.* The session is on the tier, so the web's commands are applied after the local and API queues of the pass they land in whenever they arrived, **web before CAT** (test_remote_control_pull A4: the browser's PTT first in wall time, the local press second - the local press wins, three rounds), and the key drive runs again after the tier so a browser's `PTT 0` lets the Transmitter go in the **same pass** (A5).

Behaviour deltas, all deliberate: refusals arrive as results, not to the client (as before); the newest command is `BUSY`-refused where the oldest was dropped; bookmark rows are resolved at the post, a pass earlier; every range is the table's (`centerHz` 0 - which the strict parse lets through - is refused `OUT_OF_RANGE`; a radio that has closed since the post is `NO_DEVICE`, not applied to its successor); a scan started in the same breath as a change to the stored range reads the range the *last publish* stored (the request's own range fields are merged, a previous request's may not have been published yet).


## 9. Events and ops

`FOXAPI_EVENT_PATCH` (10): the patch graph, its epoch (a document load) or
whether it runs moved in the pass; `value[0]`: the epoch, `value[1]`: 1
running.

`FOXAPI_EVENT_DECODER_IMAGE` (6, 0.2's event, served for the patch decoders' pictures): a picture arrived for a patch decoder in
this pass. `target`: the patch node; `value[0]`: the image id for `read_image`; `value[1]`: 1; `text`: the node's name. Raised after
the picture can be read (section 8.12.7). Subscribe with `FOXAPI_EVENT_MASK(FOXAPI_EVENT_DECODER_IMAGE)`.

`FOXAPI_EVENT_MUTE_TUNE_AWAY` (11): the receiver tuned off the presets of plugins that
are still running and muting the audio, and the engine holds the mute and asks whether to
stop them. `value[0]`: how many plugins; `MUTE_CAUSES` names them (`value[0] = 1` on each).
Once for each tune-away, raised by the publish that shows the held mute (section 8.7).
Subscribe with `FOXAPI_EVENT_MASK(FOXAPI_EVENT_MUTE_TUNE_AWAY)`.

`FOXAPI_EVENT_PRESET_APPLIED` (12): a plugin's preset was applied (section 8.14.1). `text`: the plugin's module file name; `value[0]`: the preset's
`CASCADE_PRESET_*` flags word.
Once for each application, raised by the `endPass` of the pass that applied it, after the publish that shows what it changed.
Subscribe with `FOXAPI_EVENT_MASK(FOXAPI_EVENT_PRESET_APPLIED)`.

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

`FOXAPP_OP_BOOKMARK_EXPORT` (0x8603, SETTINGS, **LOCAL sessions only** - it writes a file on
this machine): the long text (`submit_text`; the ids of a long list do not fit `text[]`) is the
ids of the bookmarks to export, as the `id` of their `BOOKMARKS` entries, in decimal, with
anything that is not a digit between them. The engine writes those bookmarks - **whole**, from
its store, in the store's own order (by frequency, the order they are listed in), not in the
order the ids were sent and once each however often an id is repeated - as SDR#'s
frequencies.xml to `foxsdr-frequencies-<YYYYMMDD>-<HHMMSS>.xml` in the record folder
(`fact.recorder.dir`), creating the folder, and sets the `bookmarkNote` line to "Exported <n> to
<folder>/<file>". The sender names no path and no filter: the ids are the selection (an
interface that lists a filtered view sends the ids of what it shows). Why the engine writes it
and the interface does not: a `BOOKMARKS` entry cuts a name at 95 bytes and a group at 159 and
carries the mode as a code, so a file made from the list would shorten names and groups and
rewrite a mode no code names. No digits in the text is `BAD_ARGUMENT`; ids none of which is a
bookmark (an id of 0 or one too large for 64 bits names nothing) is `NOT_FOUND`; a file that
could not be written is `FAILED` (the note says "Could not write <path>"). Nothing is written
in the first two. A REMOTE session is `DENIED`. The text is at most `FOXAPI_MAX_LONG_TEXT` (1
MiB): about 130 000 ids of seven digits.

`FOXAPP_OP_BOOKMARK_IMPORT_FILE` (0x8601, SETTINGS, **LOCAL sessions only** - it reads a file on this machine): `text` (or long
text) is a PATH. The engine reads SDR#'s frequencies.xml or a CSV and adds every entry as the file wrote it (an identical row is
skipped), then sets the `bookmarkNote` line ("<format>: <n> entries read, <m> added") and saves. The interface sends a path:
what a person typed, pasted or dropped is cleaned first (`core::cleanImportPath`: the quotes of "Copy as path" or a terminal's
drag-and-drop, the Enter or the space a paste leaves, a `file://` address with its `%XX` escapes), and a path with no root is
looked for in the working directory and then the record folder by the window, on a worker, which sends the place it found (the
engine reads the one path it is given, and never looks in two places). **The answer says whether it read anything** (stub
campaign B8; it was `OK` either way, with the outcome only in the note): `OK` when it read entries, even entries that were all
here already; `FAILED` - the list unchanged - when it read none, with the note's sentence as the message (cut at the message's
127 bytes) and in the note whole: "Could not import: the file "<path>" could not be opened" (or "could not be read", or "no line of
the file had a usable frequency"), in the user's language. The log names the failure's kind and never the file ("bookmarks: an
import (.csv) failed: cannot open "(the file)""). The window opens its "Import a frequency list" window on `FAILED` and closes it
on `OK`. An empty text is `BAD_ARGUMENT`; a REMOTE session is `DENIED`.

`FOXAPP_OP_SET_PATCH_PAGE_OPEN` (0x8421, VIEW, **LOCAL sessions only**): `ival[0]` 0 or 1, whether the standard window's patch
view is showing. The engine's pass runs the patch runtime while it is (the default) and its close rule when it goes (section 8.12).
`FOXAPP_OP_PATCH_SET_GRAPH`'s `ival[1]` is the graph version the sender edited from; the engine merges a stale one, and the
answer's `applied[0]` is the new version (section 8.12).

`FOXAPP_OP_PATCH_LIST_RECORDINGS` (0x8422, SETTINGS, **LOCAL sessions only**): no arguments. The engine lists the I/Q
recordings of the record folder and of `FOXSDR_PATCH_SAMPLES` on its own pass (a bounded number of headers opened; a file
already read is not opened again until it changes), and the recordings of `PATCH_DEVICES` and `RECORDINGS` kind 3 change as the
answer. A VIEW-only session is `DENIED`, as is a REMOTE one (section 8.12, part 2).

`FOXAPP_OP_SOUND_CARD_PICK` (0x8420, SETTINGS, **LOCAL sessions only**): `ival[0]` is a `SOUND_CARDS` entry's `id`; the
engine puts that card in the sound card form (and keeps, or re-chooses, the rate), see 8.10. `FOXAPP_OP_SOUND_CARD_FORM`
takes a field mask in `ival[1]` (8.10); 0 is the whole form, as it always was.

### 9.1 The 0.99.84 merge's additions (docs/engine-merge-0.99.84.md)

Master's window kept five things in engine fields it wrote in place; the table had no op for them. All five ops are APP ops
(`core/app_commands.hpp`, the 0x8000-0x8FFF band), **LOCAL sessions only**, under the rules every app op follows (the header's comment),
each listed in `kOps` of `core/app_commands.cpp` (test_apply_command holds the list). 0.3's header (`foxsdr_api.h`) is unchanged:
no table entry, no struct, no fingerprint field was added - the facts below are `get_setting` keys and the ops are the app band.

  * `FOXAPP_OP_SET_RTLTCP_ADDR` (0x8424): `text`, the rtl_tcp address box as typed (the Pluto box's twin). Fact `source.rtlTcpAddr` (local-only).
  * `FOXAPP_OP_SET_AUDIO_BUFFER` (0x8425): `ival[0]` 0 AUTOMATIC | 120 | 240 | 480 | 960 ms (`sink::validAudioBufferSetting`; anything else is
    `FOXAPI_OUT_OF_RANGE`, "no such buffer"). Facts `audio.bufferMs` (the setting), `audio.leadMs` (the lead in force: the AUTOMATIC
    preview), `audio.leadNote` (what the automatic mode last did to the buffer, one sentence, empty when it has done nothing). All local-only.
  * `FOXAPP_OP_SET_PPM` (0x8426): `ival[0]` the crystal correction's switch (0/1), `num[0]` the open radio's value in ppm
    (`core::sanitisePpm`; 0 = none). Facts `ppm.on`, `ppm.method` ("na" nothing to correct | "radio" | "retune"), `ppm.value`, `ppm.whole`
    (the whole-ppm value for a radio that takes integers only, else empty), `ppm.error`, `ppm.card`, plus the radio the value is for -
    `ppm.model`, `ppm.serial`, `ppm.key` (local-only).
  * `FOXAPP_OP_TEST_HOLD_AUDIO_LOCK` (0x8427): bounded runs only (`FOXSDR_HOLD_AUDIO_LOCK`, 0.99.68's seam): `ival[0]` ms. The first stop a
    control request makes of a running receiver first has a thread take the audio mutex for that long; 0 = no seam. Sent through the window
    session (`uiSubmit`), not through a door method (test_engine_shell_guard's call-site count is unchanged).
  * `FOXAPP_OP_BANDWIDTH_DRAG_END` (0x8428): the drag on the spectrum's band edge has settled; the engine writes one log line for the width it
    ended on (`logBandwidthChange("dragged on the spectrum")`, only if the drag moved it). The drag itself sends `FOXAPP_OP_SET_BANDWIDTH_DRAG`
(0x8202) as before, which marks the width as dragged and logs nothing per step.

`listSeq` and the publish fingerprint: the PPM and audio facts above are hashed from the raw members they are built from, with no
string built (the fingerprint runs on every pass and test_engine_publish_alloc counts its allocations). Nothing else about the lists
changed. What was NOT added, and is stubbed in the window instead, is in the ledger of docs/engine-merge-0.99.84.md (section 6): where a
stubbed feature will need an op (a marker list and its persistence, the plugin key's grant, the plugin settings' forms) the ledger says so.

### 9.2 The 0.99.90 merge's additions (docs/engine-merge-0.99.90.md)

Two facts, no op, no header change: `fact.source.uhdNoImages` and `fact.source.usrpOnUsb` (section 8.10's table), the text "1" or "0", public like
`fact.source.faultedDrivers`. They are hashed into the publish fingerprint from the two engine members they are built from. Everything else master
added to a window function in 0.99.85 to 0.99.90 either stayed the window's or moved to the engine without a new table entry (the pipeline ring's
once-a-minute log line; the model scrub).

### 9.3 A fitted plugin runs only while it is used (stub campaign B2; docs/engine-stage4.md 6.22)

Master's 0.99.73 decided, in the window, which plugins have instances. It is the ENGINE's now - once a pass, from the signals the engine knows and the ones a
window reports - so a headless engine makes the same decision. Two ops, one column, no list, no fact, no header struct; both ops are APP ops
(`core/app_commands.hpp`), **LOCAL sessions only** (`DENIED` to a REMOTE one, and not in the web profile's closure of 38), each in `kOps` and
covered by test_apply_command.

  * `FOXAPP_OP_PLUGIN_RUN_MODE` (0x8707, grant SETTINGS): `text`: the plugin's module file name (or its id); `ival[0]`: 0 AUTO | 1 ALWAYS | 2 STOPPED.
    AUTO runs while something uses the plugin and is dormant (no instance in the runner or the plugin UI, nothing polled) 30 seconds after the last use
    ends; ALWAYS runs without a use ("keep running": a START press and a preset press set it); STOPPED runs nowhere. The state is kept by plugin id
    (version-stripped: an update keeps it) and saved as `pluginRun` (AUTO is never written); the old `pluginsStopped` file-name list is migrated into it
    once, by id, and saved empty. One plugin's instances are started or stopped; nothing else is rebuilt. `FOXAPI_OP_DECODER_START` / `_STOP` and the
    preset ops now set ALWAYS / STOPPED / ALWAYS through the same call. A name no installed plugin carries and no run state records is `NOT_FOUND`, an
    `ival[0]` outside 0..2 is `OUT_OF_RANGE`, no name is `BAD_ARGUMENT`.
  * `FOXAPP_OP_PLUGIN_USE` (0x8708, grant VIEW): the window's WHOLE table of what it shows, replacing the last: one line `<file>\t<bits>` per plugin with
    at least one signal, in `text` (or the long text), the bits `FOXAPP_PLUGIN_USE_WINDOW` 1 (one of its image / panel / instrument windows is shown),
    `_MAP_PAGE` 2, `_RADAR_SCOPE` 4, `_DECODER_OUTPUT` 8. Empty text clears the table; a line that does not parse is skipped. Sent on CHANGE only. The engine
    holds each bit to what the module can show (a window bit for a decoder with no window of its own wakes nothing) and adds the signals it knows: a running
    patch node naming the plugin (and its wiring to a Text sink), the plugin holding the speakers or a manual tune offering an audio-output decoder its first
    chance, a web session for a track or image source, and a STANDING DUTY - an in-chain audio processor, or a module that only attaches to the host - which is
    in use for as long as it is fitted.
  * `PLUGINS` `value[3]`: bits 0-1 how the plugin runs (0 AUTO, 1 ALWAYS, 2 STOPPED), bit 2 (value 4) ASLEEP - AUTO and nothing is using it, so it has no instance.
    Hashed into the publish fingerprint from raw members by the plugin's cached id (nothing allocates; tests/test_engine_publish_alloc holds it).

**The signal is one pass late by design (the window reads what a pass published and its command is applied by the next one); the wake is not later still.**
A command lands at the head of a pass (`pumpFrameBegin`) and the lifecycle runs in the same pass's plugin phase, so the plugin is created in the pass that applies
the report and the first block the DSP thread hands the runner afterwards is the plugin's first (tests/test_engine_api_03.cpp group N2: the probe decoder names
each block by its first sample).

**A host with no window runs every plugin (owner decision, 2026-10-26).** Nothing could ever report a window to a headless engine, so AUTO is treated as
pinned there - today's behaviour, and what keeps the decoders of a Pi serving the web page. "No window" is "no front end has installed its plugin UI applier"
(`Engine::setPluginUiApplier`); a window that goes away makes the engine window-less again, and what was asleep wakes.

**One directive, no second applier.** The plugin UI's half rides the directive the engine already posts (core/plugin_directive.hpp): a rebuild carries the
dormant set beside the stop set, and a wake or sleep of ONE plugin is the same struct with `lifeOnly` set, applied by the same `PluginUi::apply`. A life-only
directive carries STATE (plugins, stop set, dormant set), not an event, so it is merged into a waiting directive (a rebuild keeps its services, a life-only
takes the newest sets) and is dropped beside a waiting RELEASE, whose epoch the engine unmaps the modules on (test_plugin_directive H).

**What the log says** (unchanged from master): `plugin: <name> woke - <signal>` and `plugin: <name> dormant after 30 s without a use`, one per transition of an AUTO
plugin. `FOXSDR_DORMANT_AFTER_MS` (1 to 3 600 000) replaces the 30 seconds for the engine's first lifecycle step; the line still says "30 s".

### 9.4 The plugin store's kept copy, sites, pictures and clean-up (stub campaign B3; docs/engine-stage4.md 6.22; the card's facts and UPDATE ALL, B4)

**`INSTALLED` is "installed in any version" (stub campaign B4; master 0.99.72).** `CATALOGUE`'s `INSTALLED` flag, the install gate's "already installed" and
ADD ALL's `fitted` all ask `Engine::catalogEntryInstalled`, which decides by **plugin id, source and version** from the install records and the scan - an
installed 1.8.0 makes the 1.8.1 entry an update, where the file-name test that stood here called it not installed (the 1.8.1 file has never been on disk) and
offered a second install - and only then by the file name: a file with the very name `install()` would write is already in the folder (a side-loaded copy no
record names, or a record whose file is there). The window draws a card from `fact.store.<key>.installState` and the host's record from `.installedFile`.

Master's 0.99.49 - 0.99.74 ran all of this in the window. It is the ENGINE's now (`src/engine/engine_store.cpp`), so a headless engine keeps the store's catalogue
offline, lists the sites a user added, fetches pictures and removes the old copies an update leaves. **No pass waits on the network or the disk**: a read, a
picture and a clean-up run on a worker each and the pass collects what has finished; start-up (the kept copies, the queue the last session left) is the one place a
disk is read synchronously, before the first pass. **One transfer at a time**: a catalogue read, a download, a picture and a clean-up never overlap (the gate says "a
transfer is already in progress"); the picture queue waits behind a read, a download, an ADD ALL run and a clean-up.

**The entry's key.** `CATALOGUE`'s `detail` and every `fact.store.<…>` use the entry's CATALOGUE KEY: the plain id for an official plugin (so every name that existed
before sites is unchanged) and `<id>@<site label>` for a plugin from a site the user added (the label is the site's host, or a local file's name). `STORE_UPDATES`
names the same key. `FOXAPI_OP_STORE_INSTALL` and `FOXAPP_OP_STORE_UPDATE` take it; a bare id of a site's plugin names nothing to a LOCAL session.

**Ops** (all `APP` ops, **LOCAL sessions only**, grant ADMIN, none on the web profile's closure of 38, each in `kOps` and covered by test_apply_command):

  * `FOXAPP_OP_STORE_SITE` (0x8709): `text`: the site's address as typed (it is normalised and re-checked here by the one validator,
    `PluginRepo::normaliseSiteUrl`: an `https://` address with a lockable host and no credentials, or a path on this machine; at most 8 sites); `ival[0]`: 0 ADD,
    1 REMOVE; `ival[1]` (ADD): 1 = the consent - the box that says what a site is was ticked. Without it ADD is `DENIED` and nothing is read or written; an address
    the validator refuses, one already listed, the official one and a full list are `BAD_ARGUMENT`; any other `ival[0]` is `OUT_OF_RANGE`; while a transfer is in
    flight it is `BUSY`. The refusal's English sentence is the result's message and `fact.store.siteError`. ADD lists the site and starts a catalogue read;
    REMOVE of an address that is not listed is `NOT_FOUND`, and otherwise takes the site's entries out of `CATALOGUE`, deletes its kept copy (on a worker) and
    **leaves what was installed from it installed** (its record keeps its source; nothing lists it any more, so it gets no updates).
  * `FOXAPP_OP_STORE_PICTURES_WANT` (0x870A): `text`: a catalogue key; `ival[0]`: 1 = ask again for a picture that failed. Queues the entry's pictures that are not
    already Ready, pending or in flight; `NOT_FOUND` for a key the catalogue does not hold. A site's picture whose address is not on the site's own host is never
    queued (`mergeSite` held every address to it when the list was made; this is the second look, at the request, because the request is what contacts an address).
  * `FOXAPP_OP_STORE_ADD_ALL` (0x841E, 8.9) gains **`ival[1]`: 1 = UPDATE ALL** (stub campaign B4; 0, what every earlier client sends, is ADD ALL as it
    was): the same queue with nothing to fetch in it - the plan's `install` and `skipped` lists are emptied, so only the updates the planner has a plan for
    run, and what is passed over for want of a build or a notice is neither run nor logged. With nothing to update it starts nothing and `installError` says
    "every module in the catalogue is already fitted, and none has a newer build". The tick and the one module named in `text` are ADD ALL's, and an
    UPDATE ALL carries neither (a plugin that is fitted has had its notice shown).
  * `FOXAPP_OP_STORE_CLEAN_OLD` (0x870B): `ival[0]` 0: remove EVERY old copy the last scan found superseded (the store's CLEAN UP key, once the window has confirmed
    it); `ival[0]` 1: REMOVE FILE on the orphaned module named in `text`, judged again against the engine's own scan (a row gone stale removes nothing and
    `installError` says so; a name that tries to leave the folder removes nothing). The clean-up's one rule is `core::supersededPlugins`: only a copy the scan
    turned off as another copy of a loaded, strictly newer, catalogue-recorded plugin, never one the manifest records; an update's own clean-up is restricted
    again to the copies THAT file superseded. A delete the system refuses (a mapped module) is queued for the next start, which deletes it only if its own scan
    still finds it superseded. `BUSY` under a transfer; `OUT_OF_RANGE` for any other `ival[0]`.

**The list `STORE_PICTURES` (50)**: see 5.1. A picture is tracked by its sha256, so one two plugins share is one request; Ready means the bytes in
`<plugins dir>/store-cache/<sha256>.png` were hashed to the catalogue's digest.

**Facts** (get_setting, hashed into the publish fingerprint from raw members; the ones marked LOCAL are `DENIED` to a REMOTE session):

| Fact | What it says | |
|---|---|---|
| `fact.store.fromCache` | `1` while `CATALOGUE` is a kept copy (`0` once a read succeeded this session, and while it is empty) | |
| `fact.store.readThisSession` | `1` when `CATALOGUE` holds a list read this session: not empty and not from the cache (the rail's IDLE and the store's one automatic read ask this) | |
| `fact.store.refreshFailed` | `1` when the last read failed: `catalogError` is then a refresh that did not happen and the rows are a kept copy or an earlier read | |
| `fact.store.cacheTime` | seconds since the epoch of the read behind `CATALOGUE` (the read's own time, or the one the kept copy recorded); `0` = none or not recorded | |
| `fact.store.busyKey`, `fact.store.resultKey` | the catalogue key of the transfer in flight (the store draws FITTING... on that key) and of the plugin the last install report or error is about | |
| `fact.store.sites` | one line per site, in order: `<address>\t<state>\t<listed>\t<hidden-same-id>\t<hidden-off-host>\t<error>`, state 0 not read, 1 the kept copy, 2 read, 3 failed | LOCAL |
| `fact.store.siteError` | why the last `FOXAPP_OP_STORE_SITE` was refused, English; `""` after one that was not | LOCAL |
| `fact.store.<key>.site` | the entry's site address, `""` for an official entry | LOCAL |
| `fact.store.<key>.regional` | `1` when the regional list offered it (shown by the store as "Offered by foxsdr.com in some countries only") | |
| `fact.store.<key>.pictures` | the pictures the store may ask for, one per line `<sha256>\t<caption>` (a site's picture on another host is not listed) | |
| `fact.store.picture.<sha256>.path`, `.reason` | a Ready picture's whole cached path (LOCAL), a Failed one's whole reason | `.path` LOCAL |
| `fact.store.regional.state`, `.added`, `.dropped` | the regional list's outcome (`not-asked`, `ok`, `failed`), for the CASCADE_PLUGIN_TEST report and nothing else: the outcome itself would say which country answered, so it is never shown to a reader and never logged | LOCAL |
| `fact.store.oldCopies` | one line per old copy the last scan found superseded: `<file>\t<name>\t<version>\t<kept file>\t<kept version>` - what CLEAN UP lists | LOCAL |
| `fact.store.cleanupReport` | what the last clean-up did, in the language in force (a sentence; `""` when it did nothing) | |
| `fact.store.<key>.category`, `.whatsNew`, `.published`, `.sha256` | (stub campaign B4) the shop-window fields of the entry, whole: the category's id (`aircraft`, `marine`, ... or `""`), the paragraph about THIS version, `YYYY-MM-DD`, and the digest of **this machine's** build (`""` with none). Present for every entry, `""` when the index said nothing, so "told nothing" is told from `NOT_FOUND` | |
| `fact.store.<key>.experimental` | `1` when the index marks the plugin as never having decoded a real signal, else `0` | |
| `fact.store.<key>.capabilities` | what the catalogue says the binary declares, as the decimal OR of the `CASCADE_CAP_*` bits of the names this build knows; **`2147483648` (0x80000000) when it names only capabilities this build has not heard of** - "declares one this build does not know", which the page letters differently from "declares nothing"; `""` when it names none (a fitted module's own descriptor wins once it is loaded: `PLUGINS` `value[0]`) | |
| `fact.store.<key>.installState` | how the entry stands against what is installed, **by plugin id, source and version** (`PluginRepo::installStateFor`; the scan wins where it and an install record both know a module): `none`, `installed`, `update` (the catalogue's is newer than the newest installed) or `newer` (an installed copy is newer than the catalogue's) | |
| `fact.store.<key>.installedVersion`, `.installedFile` | the newest installed version (`""` with none) and the module file of that copy, the join to `PLUGINS` when it is a scanned module - **use it before `.file`**: an update's own file has never been on disk, so `.file` finds nothing for it (`""` for an installed copy no scan describes, e.g. a retired module) | |
| `fact.plugin.origin.<file>` | for every module file the host holds a record of: `<word>\t<address>`; the word is `orphan` (the plugin index does not know it and nothing runs it: REMOVE FILE is offered), `site` (installed from a site that is still listed), `removed-site` (installed from one the user has since removed: it gets no updates) or `""`; the address is the install record's source | LOCAL |

**A plugin from a site is fitted from the desktop store only.** Its page, its notice and the tick under it are what stand between a stranger's native code and this
machine, and a request from a browser cannot have read any of them. A site's plugin ALWAYS carries a notice (the install gate asks for the acknowledgement of
every such plugin: the same sentence, the same tick, the same ADD ALL path a legal notice always had). **A REMOTE session - the web page's own session included -
is refused it** by `FOXAPI_OP_STORE_INSTALL`, whether it names the entry by key or by the bare id the page lists, and whatever acknowledgement it sends: `DENIED`
with `installError` and the result's message `plugins from sites you added can only be fitted from the plugin store on this computer`, nothing started, nothing
fetched. The web page's catalogue rows carry the same sentence as `blockedReason` for a site's entry. (The engine knows a command's origin from the session
that sent it: `ApiHostImpl::apply` sets `Engine::commandFromRemote_` around its one call of `applyCommand`.)

**What moves `listSeq`.** The kept copy's flags and time, the sites and what each said, `siteError`, the regional outcome, the pictures (one counter, bumped at every
change of a picture's state or of the queue), whether a picture or a clean-up is in flight, the old copies and the clean-up's report, and the busy and result keys - all
hashed in place from members; nothing allocates in a pass (tests/test_engine_publish_alloc).

### 9.5 What the Fitted modules window and the DECODE bank need of each module (stub campaign B5; docs/engine-stage4.md 6.22)

Two per-file facts, keyed like the other `fact.plugin.<file>.*` (the module file name, the leaf parsed from the right), present for every file the host
holds a record of - loaded or refused - so "none" is told from "no such file". Built when the lists are rebuilt (a rescan, a change of the install
records, the catalogue or the sites moves the fingerprint; the install record's ABI and time are hashed now, which the first of them was not).

| Fact | What it says | |
|---|---|---|
| `fact.plugin.<file>.record` | the install record that names the file: `<catalogue key> TAB <abi version> TAB <installed at, unix seconds> TAB <category>`. The key is the id for an official plugin and `<id>@<site label>` for one installed from a site, `""` when the record names no id; the category is the catalogue entry's (`aircraft`, `marine`, ... or `""` when the catalogue does not hold it or was never read). `""` when no record names the file (a hand-fitted module) | LOCAL |
| `fact.plugin.<file>.windows` | one line for each window the module's OWN tables declare beyond a picture window (which its capability word says): `panel TAB <title>` and `instrument TAB <title>`, the title the module gave (a window's identity carries it). `""` for a module with none and for a file the host refused | |

Why the table must say it: a plugin set to AUTO that nothing is using is ASLEEP (9.3) and has no instance, but its window rows on the rail and the
double-click on its Fitted modules row must name its windows all the same - opening one is how it is woken. A track source needs nothing more (its map page
is named by its display name), a picture decoder neither; a panel and an instrument carry a title only the module's table knows.

**`FOXAPI_EVENT_PRESET_APPLIED` carries the preset's flags** in `value[0]` (8.14.1). `FOXAPP_OP_PLUGIN_RUN_MODE` is sent by the Fitted modules page's KEEP RUNNING
tick (`ival[0]` 1 / 0), `FOXAPI_OP_DECODER_START` / `_STOP` by the row's key, `FOXAPP_OP_STORE_CLEAN_OLD` (`ival[0]` 1) by an orphan's REMOVE FILE, and
`FOXAPP_OP_PLUGIN_USE` by the window for every window shown - no new op.

### 9.6 The Airband monitor, the frequency list's ticks and the scanner's list mode (stub campaign B6; docs/engine-stage4.md 6.22)

Master's 0.99.60 - 0.99.71 let a tester tick frequencies in the frequency list and LISTEN: several AM and NFM channels off the receiver's one radio, mixed, each behind its
own squelch, and the scanner could scan the ticked rows. The DSP, the radio and the list were the engine's even in master; the window only drove them, from its frame. They
are the ENGINE's now (`src/engine/engine_airband.cpp`), so a headless engine listens too. Four APP ops (`core/app_commands.hpp`), **LOCAL sessions only** (`DENIED` to a
REMOTE one and not in the web profile's closure of 38), each in `kOps` and covered by test_apply_command; one list (`AIRBAND_CHANNELS`, 5.1); two item flags and one value
word on `BOOKMARKS` (5.2, 5.3); the facts below. The patch runner and the pass's bookmark save are the ones the engine already had: no thread was added.

| Op | Grant | What it does |
|---|---|---|
| `FOXAPP_OP_BOOKMARK_TICK` (0x8604) | SETTINGS | `ival[0]`: the bookmark's id; `ival[1]`: 0 untick / 1 tick. `NOT_FOUND` for an id the list does not hold. A row already as asked changes nothing and writes nothing (the list's version does not move). The row stays where it is among its peers on one frequency (the monitor plays the first). |
| `FOXAPP_OP_BOOKMARK_ADD_ROWS` (0x8605) | SETTINGS | `long text` (or `text`), one row a line: `<Hz> TAB <mode> TAB <bandwidth Hz> TAB <tick 0/1> TAB <group> TAB <name>` (the last two may be empty; a width that is no number takes the mode's default). `ival[0]` 0: rows TYPED IN - added as the list's own add does (a name in use gets " (2)"; a nameless row is named by its frequency, "121.5000 MHz") and judged by the rule a tune the radio refuses is: a frequency outside what the open radio covers (a converter's LO taken out) is `OUT_OF_RANGE` with the sentence that says what it does cover, and nothing is added; a radio that publishes no range (the generator, an I/Q file) refuses nothing. `ival[0]` 1: a LIST (an airport's rows) - added as an import adds (a row identical in name, frequency and group to one already there is skipped) and not judged. `applied[0]`: the rows added. `BAD_ARGUMENT` for text with no row in it, `OUT_OF_RANGE` for any other `ival[0]`. Why an op of its own: `FOXAPI_OP_BOOKMARK_IMPORT` is not served and `BOOKMARK_ADD` adds the tuned station only. |
| `FOXAPP_OP_SCANNER_LIST` (0x860A) | TUNE | `ival[0]` 0: range mode (the scanner scans the range again); 1: list mode (it scans the ticked rows, in frequency order, each tuned with its own mode and bandwidth) - either stops a running scan, as the checkbox did; 2: START a list scan (list mode, whatever it was; the Airband monitor stands down). A start with nothing ticked is `NO_CHANGE` ("no frequency is ticked"). List mode is session-only (never in the config). While a list scan runs it follows the ticks: a row ticked or unticked restarts it from the first row, nothing ticked stops it; a heard time written or a bookmark added elsewhere leaves it where it is. `FOXAPI_OP_SCANNER_RUN` (a range scan, from the desk or a browser) leaves list mode - the range is what the command carried. |
| `FOXAPP_OP_AIRBAND` (0x8429) | TUNE | `ival[0]` the action. **0 STOP** (`NO_CHANGE` when nothing listens or waits). **1 LISTEN**, `text`: the preset (a bookmark group) to play, "" for every ticked AM or NFM row: nothing ticked there is `NO_CHANGE` with the note "Tick at least one AM or NFM frequency to listen to."; a radio that cannot be divided to an audio rate, or a block the radio will not tune to, is `FAILED` with the note; the same preset while listening is already so, another is a restart on it. **2 NEXT BLOCK** (`NO_CHANGE` with one block). **3 ABOVE THE NOISE**: the squelch to 8 dB over the median channel level the strips have reported (`NO_CHANGE` with no level yet; `applied[1]`: the dB) - the one place the rule is written; the top bar's AUTO squelch (batch B12) calls it. **4 SQUELCH** `num[0]` dB, clamped to -110..0 (`CLAMPED`; `applied[1]`); a listening monitor rebuilds its block with it. **5 HOLD** `num[0]` seconds, clamped to 0..60: how long a block stays after its last word; a listening multi-block monitor starts again. **6 TUNE ONE** `ival[1]`: a bookmark's id - the monitor stops ("Tuned to <name> alone.") and the receiver tunes to the row, as a click on it would. `applied[0]` is the state after: 0 idle, 1 listening, 2 waiting for the patch to hand the radio back. |

**One radio, one runner - the hand-back.** The monitor plays on the pipeline's patch runner, which the patch view otherwise has. They are never on it at once. LISTEN with the patch
view showing (`FOXAPP_OP_SET_PATCH_PAGE_OPEN`'s flag, true until a window says otherwise) switches to the receiver view: the engine closes the patch view itself (so a headless engine,
whose flag nobody reports, listens too), says it is waiting (`applied[0]` 2, `fact.airband.pending` "1") and starts on the first pass after the patch has let go of the radio - the close
rule has run (a running patch stopped, its radios closed) and no radio is being opened. A window that follows `fact.airband.pending` switches its own view and reports it. The patch view
being shown again - the window's report - stops a listening monitor ("Stopped: the patch view took the radio.") and cancels a waiting LISTEN: the user's hand wins over the wish.
The other rules that stop it, each with its note in `fact.airband.note`: the receiver stopped, the receiver retuned by hand (a centre the monitor did not tune to), a scan started (and
LISTEN stops a scan), the preset removed (`FOXAPI_OP_BOOKMARK_REMOVE_GROUP` of the group it plays: "Stopped: the preset was removed."), the radio unable to tune where the ticked
frequencies are. A different radio or sample rate, or a change of the rows it plays, cuts the blocks again without a note; a row added to ANOTHER preset changes nothing.

**One channel per frequency.** Two ticked rows on one frequency (an AM and an NFM row, CTAF and UNICOM) are one channel: the FIRST ticked row names it, gives it its mode and width,
and is the row its heard time is credited to. A row whose mode is not exactly AM or NFM (WFM, SSB, CW...) is never played; ticked, it is a row of the scanner's list mode and nothing else.
An NFM channel runs the 50 us de-emphasis the receiver's own NFM runs and is mixed at the level that matches an AM one.

**The heard time has one writer.** Every second a channel's squelch is open while the monitor plays it is added to its row's `heardSeconds`: by the engine's pass, flushed to the list
every 20 s, at STOP and at exit (the engine's shutdown flushes before its final save), and saved by the bookmark worker from a snapshot taken on the pass. Nothing else writes it. A flush
moves the list's version (and `listSeq`); `AIRBAND_CHANNELS`' `value[3]` carries what has not been flushed yet.

| Fact | What it says |
|---|---|
| `fact.airband.listening` | "1" while the monitor listens, else "0" |
| `fact.airband.pending` | "1" while a LISTEN waits for the patch to hand the radio back |
| `fact.airband.preset` | the preset on the air ("" = every ticked AM or NFM row) |
| `fact.airband.note` | what the monitor last said: the reason it stopped, or why a LISTEN was refused (a sentence, in the language in force when it was said) |
| `fact.airband.block` | "Block 2 of 6: 10 channels, 118.100-119.950 MHz", or "All 7 channels at once, ..." when one block holds every channel; "" when not listening |
| `fact.airband.blocks` | how many blocks the plan has while listening, else "0" |
| `fact.airband.squelchDb`, `fact.airband.holdS` | the monitor's squelch (dB) and hold (s), to one decimal |
| `fact.scanner.listMode` | "1" while the scanner is in list mode |
| `fact.scanner.ticked` | how many rows are ticked |

All are hashed into the publish fingerprint in place, from the members they are built from (the monitor's state is a few scalars and two strings): nothing allocates in a settled
pass. `AIRBAND_CHANNELS` is built once a pass when its own fingerprint moved - a level to half a decibel, the heard time to a tenth of a second - and an idle engine builds one empty
block and no more. The ticks are `BOOKMARKS`' `TICKED`, hashed with the list's version.

Held by tests/test_engine_api_airband.cpp (each group written red first, against an engine whose ops answered `UNSUPPORTED`, then against mutants), test_apply_command (the four ops'
cases) and the engine's own `fox_engine_api.hpp` block `ApiAirbandBlock`.

### 9.7 A plugin's settings form (stub campaign B9; docs/engine-stage4.md 6.22)

A plugin that declares `CASCADE_CAP_SETTINGS_UI` (0.99.43) gets a form drawn for it by the host: a text box, a number, a checkbox, and (0.99.74) a path with a BROWSE key. The rules - what a
field shows, what an edit may store - are `core/plugin_settings_ui.hpp`'s and pure; the store is the plugin API's, whose `settingsUiSet` is the ONE path that advances the plugin's
`settings_seq`. In master the window held the form AND called that path. The window may not include `plugin_api.hpp` (the interface fence), and a headless engine has no window, so the
form is the table's: one list, two facts per field and one op. The engine judges and stores an edit with the two core calls master's window made (`commitSettingEdit`: `prepareSettingEdit`,
then `settingsUiSet`) - `src/engine/engine_plugin_settings.cpp`.

* **`PLUGIN_SETTINGS` (list 52, 5.1)**, LOCAL sessions only: what each loaded module declares, from its static table (memory, never a call into its code). Built with the lists' block, whose
  fingerprint hashes the settings store's generation, so a value changed by the form, by the plugin's own `settings_set` or by a config load moves `listSeq`.
* **`fact.plugin.setting.<id>`** (LOCAL): the whole value the field shows - the stored text, else the plugin's default (the rule the plugin's own `settings_get` has) - and
  **`fact.plugin.setting.<id>.spec`** (LOCAL): `<key>`, the unit separator (byte 0x1F), `<placeholder>`. `<id>` is the list entry's `id`.
* **`FOXAPP_OP_PLUGIN_SETTING_EDIT` (0x870C, grant SETTINGS, LOCAL sessions only, not on the web profile)**: `text` (or long text): `<module file>` 0x1F `<key>` 0x1F `<value>` - the file name
  `PLUGINS` lists, the key from the spec fact, and what the person typed or chose, exactly as typed (a value may be empty: a cleared box is a value, not a deletion; everything after the
  second separator is the value). The command is the answer to *leaving* a box (TEXT, NUMBER, PATH) or clicking a checkbox - never to a keystroke. The result: `OK` for an accepted edit
  (`NO_CHANGE` is not used: an edit that leaves the stored value as it was is accepted, and moves nothing); otherwise a refusal whose `applied[0]` is the verdict
  (`core::SettingEditResult`: 1 TooLong, 2 NotANumber, 3 BadText, 4 StoreFull, 5 StoreError, 6 Refused) and whose status is `OUT_OF_RANGE`, `BAD_ARGUMENT`, `BAD_ARGUMENT`, `LIMIT`, `FAILED`,
  `NOT_FOUND` in that order, with the English sentence as the message. `applied[0]` is 0 for an accepted edit. `NOT_FOUND` also answers a module that is not loaded or declares no such
  key, and `BAD_ARGUMENT` an edit that names no module and field. **The plugin sees `settings_seq` move once for each accepted edit that changes the value, and never for a refused one.** Each such
  edit also writes one line to the diagnostic log - `plugin settings: <plugin name> - <key> saved (<n> bytes)` - naming the field and the length and never the text (a callsign, a path on this machine).
* **The window's half** (`app_window_plugin_settings.cpp`): the form's state - the text being typed, the edits sent and not yet answered - is the window's (no feed method: the door did not
  move). A box that is left sends its edit and keeps the typed text until the ticketed answer comes back; a refusal's verdict makes it snap back to the stored value from the next frame, an
  acceptance keeps the box on the text it was accepted as until the table reads it back (a bounded number of frames). A surface that stops drawing a box that was being typed in flushes
  it through the same road, once.

Held by tests/test_engine_api_settings.cpp (the list and the facts, LOCAL-only, accepted and refused edits, `settings_seq`, against the real fixture module), test_engine_api_03 (its "every
list is served" loops run to list 52), test_apply_command (the op's cases), tests/test_plugin_settings_ui.cpp (the form's contract with a sink whose answer comes later),
tests/test_plugin_settings_window.cpp (the real window against the real engine, frame by frame), and the real-binary tests test_decoder_settings_fold_app and test_plugin_pathpick_app.

## 10. What stays a GAP after 0.3

From 0.2's list (API.md 10.7), 0.3 serves the RDS snapshot (1) and the
auto-notch readout (2) (section 8.4), bookmarks export and scanner progress (11) (section
8.6), and a plugin's command-key list (4, in part) (section 8.8). What stays: plugin panels and
instruments as data (4), map tiles and track info (6), per-node patch editing as ops (7: 0.3
reads the graph back and edits it whole through
`FOXAPP_OP_PATCH_SET_GRAPH` with `submit_text`), the transmitter's microphone
list and meters (8), device-scan diagnostics (9: served in 0.3 for the SoapySDR runtime's own account, `SCAN_EVIDENCE`, section 8.10), diagnostics (12), updates (13), zoomed spectrum (14),
multi-VFO (15), a settings catalogue (16). The settings
store and the decoders' TEXT events are still not served by this engine (their capability
bits are clear; the AUDIO stream is served since stage 5a, section 8.16.1); the decoders' image event and `read_image` are served for the patch decoders'
pictures (8.12.7), and the receiver's own plugins' pictures are not (they are the plugin
windows' and stay with them, below).

**The plugin windows** (a plugin's own panel, drawn by the plugin's code
through the plugin ABI) are not an interface's to draw and get no API: they
stay in the HOST PROCESS (engine-stage4.md 6.4).
