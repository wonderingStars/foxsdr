# FoxSDR Engine API — specification 0.2 (draft)

Status: **draft 0.2, 2026-09-25** (0.2 changed what `submit` answers, section
7.2, and fixed the minimum-size rule). The in-process form is implemented in
`include/foxsdr_api.h` (MIT), by the mock engine in `src/mock_engine` and
used by the standard GUI plugin in `src/gui_plugin`. Anything marked
**spec** below is defined in the header but not yet implemented by any
engine; anything marked **GAP** has no API yet. Until 1.0 the owner may still
reshape it.

This document is the contract. The header must match it; where they
disagree, that is a bug in one of them.

---

## 1. What the API is for

FoxSDR is split in two:

- The **engine**: radio sources, the DSP chain, decoders and their plugins,
  the recorder, the patch page, the transmitter, bookmarks, the scanner,
  settings, the plugin store, crash and problem reporting, telemetry. It runs
  without a window.
- **Interfaces**: anything a person looks at. An interface is a plugin that
  talks **only** to this API. The standard FoxSDR window is the first one;
  others can be downloaded from the same checksummed catalogue as decoder
  plugins.

An interface plugin brings its own drawing stack (the standard one statically
links its own Dear ImGui and GLFW), so the engine has no ImGui at all and no
two interfaces can conflict over a version.

The same interface binary runs two ways:

| | in-process | out-of-process |
|---|---|---|
| how | the engine module's C table (`FoxEngineApi`) | a client library that implements the same table over a socket (docs/TRANSPORTS.md) |
| for | the standard desktop app | crash isolation; remote use (engine on a Pi, interface on a laptop) |
| cost | a few microseconds per frame (measured below) | spectra and reduced-rate audio/I/Q over the network, never full-rate samples |

## 2. The four rules

1. **Reads are snapshots and never wait.** `read_state` and `read_spectrum`
   copy the newest consistent value the engine published, through a sequence
   lock, with no mutex. A read that keeps overlapping a publish returns
   `FOXAPI_BUSY` instead of waiting.
2. **Changes are commands in a queue, and every answer is a result.**
   `submit` never waits for the engine's judgement: `FOXAPI_OK` for a
   command means only that the transport has it (queued, with a ticket).
   Each ticketed command produces exactly one `FoxCommandResult` through
   `poll_results`, in ticket order: either what actually landed (status
   `OK`, with the clamped value), or - with `FOXAPI_RESULT_REFUSED` set
   EXACTLY when the status is not `OK`, and nothing changed - a refusal
   decided at submit (grant, range, unknown op, no transmitter) or when
   applied (another key holder, a latch mark, the re-arm time, consent
   withdrawn, the session closed or detached meanwhile), a failure, or
   `NO_CHANGE` (7.2). The engine's control thread
   applies commands in order through the one control path every client
   shares, and hands out a pass's results only after it has published the
   state they describe, so a result is never readable before the snapshot
   that shows it. So an interface on the far end of a network submits its frame's
   batch and draws on - no round trip per click - and a per-frame PTT
   re-assertion costs nothing.
3. **The DSP thread never waits on an interface.** Queues are bounded (full =
   `FOXAPI_BUSY`), snapshots are published without waiting for readers, and
   no engine thread ever calls interface code. There are no callbacks;
   interfaces poll.
4. **Everything grows at the end.** Every struct starts with `structSize`.
   Each side touches only `min(own sizeof, structSize)` bytes and reports how
   much it filled. Members are only ever appended. `FOXAPI_HAS(api, fn)` is
   the test for a function. (These are the rules `CascadeHostApi` already
   follows in the plugin ABI; the answer codes and demod values are the same
   numbers.)

## 3. Versioning and compatibility

- **Version numbers.** `FOXAPI_VERSION_MAJOR.MINOR` (0.2). MAJOR changes only
  if an existing member changes meaning. MINOR advances when members,
  commands, lists, events or capability bits are appended.
- **The handshake.** A host loads an engine module and calls
  `foxsdr_engine_query(major, minor)` with the version the INTERFACE was
  built for; the engine returns its table or NULL. During 0.x an engine
  serves only its OWN minor (the draft may change what a member means - 0.2
  changed `submit`'s answers - so a 0.1 interface must not meet a 0.2
  engine); from 1.0 on it serves any minor and the interface relies on
  `FOXAPI_HAS` and capability bits. The interface module's
  `foxsdr_interface_query(major, minor)` applies the same rule to its host. The host also checks
  `FoxInterfaceDesc::requiredCaps` against `FoxEngineApi::capabilities`
  before calling `run`.
- **Growth.** Table members and struct members are appended only.
  **One minimum rule:** every struct (and the table) has a `FOXAPI_MIN_*`,
  which is its WHOLE layout in the oldest version an engine accepts (during
  0.x: the current 0.2 layout). A smaller `structSize` cannot come from any
  build allowed to call and is `FOXAPI_BAD_ARGUMENT`, with nothing written.
  Nothing compares a caller's size with its own `sizeof`, which grows.
  `src/common/c_check.c` pins the offset of every member and every size at
  compile time (32- and 64-bit). Arrays are walked at the caller's stride (the
  first element's `structSize`), so an older interface's shorter commands
  and a newer one's longer commands both work. Proven by
  `tests/test_abi_compat.cpp` with guard bytes after every buffer.
- **Unknown things degrade.** An op the engine does not know is a
  `FOXAPI_UNSUPPORTED` refusal (a result), never a crash; an unknown list is
  `FOXAPI_UNSUPPORTED`; capability bits the interface does not know are
  ignored.
- **Capability discovery.** `FoxEngineApi::capabilities` says what an engine
  implements (`FOXAPI_CAP_*`, section 6). Whether something is available right
  now (a transmitter open, a radio present) is state, in
  `FoxReceiverState::flags`. The standard interface greys out what the
  engine lacks.
- **Decoder plugins are not affected.** Decoder plugins keep
  `plugin_abi.h` (ABI 3, host API level 1) unchanged; the engine hosts them.
  Interface plugins are a different kind of module with a different entry
  point.

## 4. Threading and performance

- Every function may be called from any thread. One `FoxSession` must not be
  used by two threads at once; sessions are cheap, so a thread that needs one
  opens one (up to the engine's session limit: `open_session` answers
  `FOXAPI_LIMIT` past it; the mock allows 64 in all, of which at most 16
  ATTACHED REMOTE, as docs/TRANSPORTS.md 2.5 sets for a transport, and the
  last 4 of the 64 go only to LOCAL sessions, so remote logins can never
  lock the local window out. A session detached by logout or expiry is
  closed by the engine itself on its next control pass and holds no place
  from then on; its handle answers `DETACHED` until `close_session`).
  Because those 4 places are LOCAL-only, an engine configured with
  `maxSessions` of 4 or fewer serves LOCAL sessions only: every remote
  `open_session` answers `FOXAPI_LIMIT` (the mock's `maxSessions` option;
  `test_mock_engine` pins 4 = none and 5 = one). A reaped handle keeps only
  the session object itself (small and of fixed size; its results and
  events are freed when it is reaped) until `close_session`, because the
  client may still call through it: an in-process client that never closes
  its handles keeps one such object per handle, as with any handle it leaks.
- **Pure C at the boundary.** No exception crosses a table function or an
  interface's `run`: implementations catch everything at every entry and
  answer `FOXAPI_FAILED`. Every function pointer is `FOXAPI_CALL` (cdecl on
  32-bit Windows, where a `/Gz` build would otherwise disagree; empty
  elsewhere).
- The engine has (at least) a DSP thread, a control thread, and workers for
  blocking driver calls (device open, audio open). The control thread applies
  commands, runs the transmitter's watchdogs and publishes the snapshot. The
  DSP thread publishes spectrum frames and audio into rings. Neither ever
  waits for an interface.
- **The interface's frame**, which is the in-process performance contract:
  `heartbeat` → `read_state` once → `read_spectrum` once (returns
  `FOXAPI_NO_CHANGE` unless there is a newer frame) → draw, collecting
  commands → `submit` one batch → `poll_results`, `poll_events`. Sliders
  coalesce: one command per control per frame, carrying the value where the
  drag ended.
- **Measured** (mock engine, standard interface, 1440×880, hidden, Windows,
  Release): 90 frames, 90 state reads, 39 spectrum frames, 5 submits for 5
  clicks, **5.2 µs per frame inside API calls** (0.03% of a 16.7 ms frame),
  1.0 ms mean CPU+GPU work per frame. The heavy work never moves: the DSP
  chain stays where it is and the interface only copies a ~700-byte snapshot
  and up to 1024 spectrum bins per frame.

## 5. Sessions, grants and security

A **session** is one client's connection: its grants, its command queue, its
results, its event subscription, its keep-alive. The host opens the
interface's session (so the host, not the interface, decides the grants) and
closes it afterwards.

| Flag | Meaning |
|---|---|
| `FOXAPI_SESSION_LOCAL` | in-process, on this machine. May latch the transmit key. |
| `FOXAPI_SESSION_REMOTE` | arrived over a transport. Needs a login token. Never latches, never holds ADMIN, never names a file path, and of the transmitter may only press and release the PTT, with the local operator's consent (6.10). |

| Grant | Allows |
|---|---|
| `VIEW` | reads: state, spectrum, lists, settings; problem reports and feature requests. Always granted. |
| `TUNE` | frequency, VFO offset, step tuning, bookmark tune, scanner. (The Doppler-follower grant.) |
| `SETTINGS` | run/stop, mode, bandwidth, squelch, volume, mute, audio DSP, display, source, rate, gains, recorder, bookmarks edit, decoders, patch, audio device, position |
| `TRANSMIT` | the transmitter and its key. For a REMOTE session: the PTT only, and only while a LOCAL session has armed remote transmit (`TX_REMOTE_ARM`) |
| `ADMIN` | plugin store, settings store, telemetry, web/CAT listeners, update checks — LOCAL only |
| `AUDIO` / `IQ` | the audio / I/Q streams |

Security rules (the existing web remote's rules, kept):

- A network transport binds to loopback by default. A non-loopback bind
  without a usable password is refused (the app's `web_policy` rule 1), and
  remote use requires TLS (docs/TRANSPORTS.md).
- A remote session presents a login token; wrong or missing =
  `FOXAPI_UNAUTHENTICATED`. The token comes from the table's `login(engine,
  user, password, token, cap)` (over a transport: its login request, which
  the client library turns into this call): a 256-bit random token that
  lasts at most 12 hours; `logout(engine, token)` revokes it, and every
  session opened with it is DETACHED at once (any key it held opens, and of
  what it had queued only the commands that make the transmitter safer are
  applied). `logout` answers `OK` for a live token, `NOT_FOUND` for one that
  is unknown, already revoked or expired, and `DENIED` for the engine's
  CONFIGURED token (an engine option, not a login: `logout` cannot revoke
  it and detaches nothing; changing the configuration does); an
  expired token's sessions are detached however the expiry is noticed (the
  control thread's pass, a `submit`, an `open_session` or a `logout` that
  presents it). Five
  failed logins lock login out for a while (`FOXAPI_LIMIT`, right password
  or wrong). An engine with no credential configured refuses every login
  and every remote session.
- No remote session can name a path on the engine's machine: `SELECT_SOURCE`
  refuses `file:` ids from REMOTE sessions, the recorder directory is not
  settable, `BOOKMARK_IMPORT` takes file *content*.
- Device ids are matched against the scanned list, never passed to a driver
  verbatim (the web API's `soapyArgs` rule).
- Interface plugins are code. They come from the same checksummed catalogue
  as decoder plugins (SHA-256 per file, legal-notice acknowledgement where
  the catalogue says so), and the host loads only an installed, verified
  file. An out-of-process interface runs with its user's privileges on its
  own machine, which is the point of the crash-isolation mode.

## 6. The object model

Every object below is reached through the same five mechanisms: fields in
the **state snapshot**, **commands** (`FOXAPI_OP_*`), **lists**
(`FOXAPI_LIST_*`), **events** (`FOXAPI_EVENT_*`) and **streams**. The
"App today" column names where the thing lives in the FoxSDR app now.

### 6.1 Engine
Lifecycle (`create`, `destroy`), capabilities, version, fault state
(`FOXAPI_RX_FAULTED`, `faultMessage`, `FOXAPI_EVENT_FAULT`). A fault latches
until the next `RUN 1`, stops the receiver and opens the transmit key. App
today: `Pipeline::faulted()`, `AppWindow`.

### 6.2 Sources and devices — `FOXAPI_CAP_SOURCES`, `FOXAPI_CAP_GAINS`
State: `deviceName`, `sampleRateHz`, `channelRateHz`, `FOXAPI_RX_DEVICE_OPEN`,
`gainCount`, `DEVICE_AGC`, `AGC_SUPPORTED`. Commands: `SCAN_DEVICES`,
`SELECT_SOURCE` (id from the list; `file:`/`pluto:` LOCAL only),
`SET_SAMPLE_RATE`, `SET_GAIN`, `SET_DEVICE_AGC`, `SET_ANTENNA`,
`SET_BIAS_TEE`, `SET_DEVICE_OPTION`. Lists: `DEVICES`, `SAMPLE_RATES`,
`GAINS` (min, max, step, current), `ANTENNAS`, `DEVICE_OPTIONS`. App today:
`AppWindow::selectSource`, `scanNative`/`scanSoapy`, `device_` setters.

### 6.3 Receiver and VFO — `FOXAPI_CAP_RECEIVER`
State: `centreHz`, `vfoOffsetHz`, `tunedHz`, `bandwidthHz`, `squelchDb`,
`signalDb`, `sMeter`, `RUNNING`, `SQUELCH_OPEN`, `tuneSeq`, `modeSeq`.
Commands: `RUN`, `SET_CENTRE` (the radio moves), `SET_FREQUENCY` (the tuned
channel moves; VFO offset kept, centre follows — the counter's rule),
`SET_VFO_OFFSET` (clamped into the band), `STEP_TUNE` (±n × step, the
counter's switches and wheel, TUNE UP/DN, the key bindings),
`SET_BANDWIDTH` (clamped to 0.9 × channel rate), `SET_SQUELCH`.
One VFO in 0.2; `FoxCommand::target` is reserved for VFO n. App today:
`retuneSourceHz`, `tuneAbsoluteHz`, `setVfoToAbsoluteHz`, `Pipeline`.

### 6.4 Demodulator and audio DSP — `FOXAPI_CAP_AUDIO_DSP`, `FOXAPI_CAP_RDS`
State: `demodMode`, `deemphasis`, `STEREO_ENABLED`, `STEREO_ACTIVE`,
`pilotLevel`, `NR` + `nrStrength`, `NOTCH` + `notchHz` + `notchQ`,
`AUTO_NOTCH`. Commands: `SET_MODE` (bandwidth moves to the mode's default,
as the mode keys do), `SET_DEEMPHASIS`, `SET_STEREO`, `SET_NR`, `SET_NOTCH`,
`SET_AUTO_NOTCH`. List: `MODES` (mode, default bandwidth), `BANDWIDTHS`.
**GAP**: RDS fields (PI, PS, PTY, RadioText, group and error counts), and
the auto-notch readout (engaged, frequency).

### 6.5 Spectrum and waterfall — `FOXAPI_CAP_SPECTRUM`
Stream: `read_spectrum(since, info, bins, cap)`. Frames carry a strictly
increasing `seq`, `centreHz`, `spanHz`, `unixMs`, `binCount`. If `cap` is
smaller than the engine's bin count the engine reduces by taking the
**maximum** of each group, so a narrow carrier never vanishes. **The
waterfall is not a separate stream**: it is the history of this one, kept by
the interface (the standard interface keeps 256 rows in a texture). State:
`dbMin`, `dbMax` (the display range the spectrum axis and waterfall palette
share), command `SET_DISPLAY_RANGE` (10 dB minimum span, never inverted).
**GAP**: a sub-range / higher-resolution request for zoomed views (matters
remotely), and a waterfall backlog for an interface that connects late.

### 6.6 Audio — `FOXAPI_CAP_AUDIO_LEVELS`, `FOXAPI_CAP_AUDIO_STREAM`
Audio is **played by the engine**; an interface normally only shows levels.
State: `volume`, `MUTED`, `audioLevelDb` (the VOLUME meter), `audioUnderruns`,
`SINK_OPEN`, `sinkName`. Commands: `SET_VOLUME`, `SET_MUTED`,
`AUDIO_DEVICE`; list `AUDIO_DEVICES`. Stream: `read_audio(cursor, ...)`
(grant `AUDIO`): the finished audio, by absolute frame cursor, 0 = join live,
with a `dropped` count when the reader fell behind — the reader's problem,
never the DSP thread's. Remote clients get it at a reduced rate.
**GAP**: the sink's OPENING state; a flag for a plugin-imposed mute as
against the user's.

### 6.7 Decoders and plugins — `FOXAPI_CAP_DECODERS`, `FOXAPI_CAP_IMAGES`, `FOXAPI_CAP_TRACKS`
State: `decodersRunning`, `decodersFitted`, `DECODER_ACTIVE` (the DEC lamp).
Lists: `DECODERS`, `PLUGINS`, `PRESETS`, `USER_PRESETS`, `MARKERS`,
`TRACKS`. Commands: `DECODER_START`, `DECODER_STOP`, `DECODER_STOP_ALL`,
`PLUGIN_PRESET`, `PLUGIN_GRANT` (receiver control / radio settings),
`PLUGIN_COMMAND` (a key on the plugin's plate), `PLUGIN_MUTE`,
`PLUGIN_RESCAN`, `USER_PRESET_SAVE`, `USER_PRESET_FORGET`. Events:
`DECODER_TEXT` (one output line), `DECODER_IMAGE` (then `read_image`).
**GAP**: plugin panels (rows of cells) and instruments (state + inputs such
as the nav-bearing OBS knob) as data, the list of a plugin's command keys,
basemap tiles and track info for the map, decoder-output backlog. These
are all things the app's host draws from plugin data today; the interface
must be able to draw them from API data instead.

### 6.8 Patch page — `FOXAPI_CAP_PATCH`
Commands: `PATCH_LOAD` (a whole patch document), `PATCH_RUN`,
`PATCH_ALL_OFF`. **GAP**: reading the graph back, per-node edits (add part,
wire, inspector fields), per-radio state, per-node spectrum/scope streams,
file sinks (WAV/MP3, LOCAL only). The patch page is the largest gap.

### 6.9 Recorder — `FOXAPI_CAP_RECORDER`
State: `RECORDING_IQ`, `RECORDING_AUDIO`. Commands: `RECORD_IQ`,
`RECORD_AUDIO`. The directory is not settable remotely. `RUN 0` must stop
recordings first (the desktop STOP does, the web API's `running=false` does
not today — the extraction fixes that divergence). **GAP**: file name,
size, STALLED/FULL state, recordings list.

### 6.10 Transmitter — `FOXAPI_CAP_TRANSMIT` (grant `TRANSMIT`)
State: `TX_AVAILABLE`, `TX_KEYED`, `TX_LATCHED`, `TX_KEY_MINE`,
`TX_REMOTE_ARMED`, `txMode`, `txFrequencyHz`, `txPowerDb`,
`txHoldRemainingMs`, `txLatchRemainingMs`, `txUnkeyReason`. Commands:
`TX_OPEN`, `TX_CLOSE`, `TX_PTT`, `TX_LATCH`, `TX_REMOTE_ARM`, `TX_SET_MODE`,
`TX_SET_FREQUENCY`, `TX_SET_POWER`, `TX_SET_INPUT`, `TX_SET_SPLIT`,
`TX_SET_TONE`, `TX_SET_MONITOR`. List `TX_DEVICES`. Event `TX_UNKEYED` (with
the reason).

**Remote transmit is the app's web remote, no wider.** The app's web remote
can send exactly one transmit field, `transmitPtt` (`web_control.cpp`), and
the key it closes exists only while the local operator has the Transmit page
open (`app_window.cpp` releases it otherwise). So a REMOTE session, whatever
grants it holds, may of the transmitter only press and release the PTT; it
cannot open, close or configure a transmitter, latch, or arm remote transmit.
Pressing needs the LOCAL operator's standing consent: `TX_REMOTE_ARM 1` from a
local session with the TRANSMIT grant (the Transmit page being open, in the
standard interface). The consent belongs to that local session and lapses
when it sends `TX_REMOTE_ARM 0`, closes, or stops answering its keep-alive;
any key a remote session holds opens at once when it lapses.

**The key-safety rules are enforced by the ENGINE, not by any interface:**

| Rule | Value | Enforced by the mock | Test |
|---|---|---|---|
| PTT is a HOLD, not a switch: `TX_PTT 1` keys or extends for `FOXAPI_PTT_HOLD_MS`; the interface re-asserts while the key is held; silence opens it | 2000 ms | yes | `testPttIsAHold` |
| The latch is refused to any non-LOCAL session | — | yes (DENIED) | `testLatchRules` |
| The latch releases itself, counted from when it CLOSED | `FOXAPI_LATCH_TIMEOUT_MS` 60 s | yes | `testLatchRules` |
| A latch cannot be extended: `LATCH 1` while latched changes nothing (`FOXAPI_NO_CHANGE`) and the ORIGINAL deadline stands | — | yes (the app's `setLatched(true)` is a no-op while latched) | `testRelatchDoesNotExtend` |
| **One latch, not a chain.** A latch belongs to a PRINCIPAL: `local` for EVERY LOCAL session (in-process, or through the local token file: the operator at the machine), the login token (or the configured token) for a remote one. When a latch ends by ANY means other than its OWNING session's `LATCH 0` - the timeout, a stop (`RUN 0` from any session, local or remote), a fault, a transmitter change or close, a lost keep-alive, its session closing or being detached, a `LATCH 0` from any session that did not hold it (the operator's other window included: pressing RELEASE there because the window holding it is stuck) - the owner's principal is MARKED. While marked, `LATCH 1` from ANY session of that principal - including sessions opened later - is refused (`DENIED`, the message naming how the latch ended: "the latch ended (the receiver was stopped): this interface must send LATCH 0 first"; within the re-arm time the re-arm refusal comes first) until THAT SESSION ITSELF sends `LATCH 0`. **A `LATCH 0` exempts only the session that sent it**, whether or not a latch was active: the owner's own `LATCH 0` on its active latch ends it unmarked and exempts the owner; a `LATCH 0` from any other session ends an active latch, marks the owner's principal and exempts the sender; with nothing latched it exempts the sender. Every new marking end re-marks and withdraws every exemption. The mark itself is never cleared, and closing sessions clears nothing and exempts nobody: a closed or detached session's queued `LATCH 0` still releases an active latch (it makes the transmitter safer, 7.2) but exempts no one - not even itself, which can never press again. When in doubt the transmitter stays unkeyed. So an interface that re-sends `LATCH 1` every frame gets ONE latch - it never sends `LATCH 0`, so nothing frees it: however often it is closed and reconnects (round-4 review: 5 latches in 12 s), however many local sessions do the same, and however often, and whenever, the operator presses RELEASE in another window (round-5 review: a second RELEASE press, or one press after the latch had timed out, found nothing latched, cleared the mark and re-keyed the stuck window 1 s or 30 ms later). The operator's deliberate release-then-press works in the window where it is pressed, while the stuck one goes on sending `LATCH 1`. And `LATCH 0` is idempotent - sent once or six times, merged or not, it leaves the same state - so merging it can never change the outcome (round-5 review, r3: B `LATCH 0`, A `LATCH 1`, C `RUN 1`, B `LATCH 0` in one stalled pass let A latch again, because the merged copy's last application found nothing latched and cleared the mark). (A remote session cannot send `LATCH` at all, so only the `local` principal is ever marked in the mock, and keying a remote session's marks by its token is not yet observable: a mutant that gives remote sessions the `local` principal survives every test, as it must.) | — | yes | `testEveryLatchEndMarksItsOwner` (one case per end, each with `LATCH 1` every frame for 2.5 s after it), `testStallsDoNotChainLatches`, `testReconnectingDoesNotEscapeTheMark`, `testMarkBelongsToTheOperator`, `testReleaseInAnotherWindowKeepsTheKeyOpen` (RELEASE in another window once, twice, after a timeout and every frame, each watched 5 s, then a deliberate latch in that window while the stuck one goes on), `testLatchReleaseIsIdempotent`, `testGoneWindowsReleaseStillReleases`, `testRelatchDoesNotExtend`, `testTwoSessionsCannotFreeEachOther` |
| **The latch key protocol, and the flag that shows it.** A latch key is EDGE-triggered: an interface sends only on a press - on a key shown released, `TX_LATCH 0` and `TX_LATCH 1` in ONE `submit` batch (release, then press); on a key shown latched, `TX_LATCH 0`. So a deliberate press always latches (past the re-arm time) whatever ended the last latch, while a stuck window that only re-sends `LATCH 1` stays refused. `FOXAPI_RX_TX_LATCH_RELEASE_FIRST`, per session in `read_state`, says this session's `LATCH 1` will be refused until it sends `LATCH 0` (its principal is marked and it has not released since); an interface shows it on its key. (Round-6 review, L1: the app's own key is a toggle, and ported as "a press sends `LATCH` of what is not shown" it sent `LATCH 1` after every timeout and was refused for ever, restarts included, with nothing in the state to say why.) A LEVEL-triggered latch key - sending the key's state every frame - is NOT supported: two such windows fight (round-6 review, L2: the second window's per-frame `LATCH 0`, read from a snapshot a pass old, ended the latch just pressed in the first, 6 of 12 presses), and a stuck one gets one latch and then only refusals. | — | yes (the flag) | `testLatchKeyProtocol` (the app's toggle key ported to the protocol beside a window stuck on `LATCH 1`: refused bare, taken by the protocol after a timeout, after its own release and in a restarted interface; the stuck window latches once) |
| The key is off for a while after every latch: after ANY latch ends (released, or opened in any way above) no session can close the latch again for `FOXAPI_LATCH_REARM_MS` (`DENIED`, `applied[0]` = milliseconds left, the message giving the time and how the last latch ended), so `LATCH 0` + `LATCH 1` in one batch every frame cannot hold it | 1 s | yes | `testToggleCannotHoldTheLatch`, `testStopUnkeys` |
| Dead-man keep-alive: a session whose `heartbeat` is older than its limit loses the key. Only `heartbeat` counts (a re-asserted PTT is not one; over a transport, the interface's own `beat` message, never a WebSocket ping) | `FOXAPI_KEEPALIVE_MS` 1 s local; `FOXAPI_REMOTE_KEEPALIVE_MS` 2 s remote (the app's `kRemotePttHoldMs`) | yes | `testKeepAliveLoss`, `testRemoteKeepAlive` |
| Release on interface loss: closing a session (or its transport dropping) opens the key at once | — | yes (< 100 ms measured) | `testSessionCloseReleases` |
| A key request with no transmitter is refused, never queued | — | yes (NO_DEVICE) | `testNoTransmitterNoKey` |
| One key holder; anyone with the grant may OPEN the key | — | yes | `testOneKeyholder` |
| A REMOTE session may only press and release the PTT: `TX_OPEN`, `TX_CLOSE`, `TX_LATCH`, `TX_REMOTE_ARM`, `TX_SET_*` are DENIED to it whatever its grants | — | yes | `testRemoteIsPttOnlyWithLocalConsent` |
| A remote PTT needs LOCAL consent (`TX_REMOTE_ARM 1` from a local session); withdrawing it, or the consenting session closing or going quiet, opens a remote key at once | the consenting session's keep-alive | yes | `testRemoteIsPttOnlyWithLocalConsent` |
| A stop (`RUN 0`), a fault, a transmitter change or close opens the key | — | yes | `testStopUnkeys`, `testFaultAndCloseUnkey` |
| **Decision: stopping is a safety action.** A REMOTE session with the SETTINGS grant may send `RUN 0`, and a stop opens ANY key - including a LOCAL session's latch, whose owner is then marked as above. This is kept deliberately: anyone allowed to stop the receiver may stop the transmitter with it, and the local operator must release and re-press the latch key afterwards | — | yes | `testEveryLatchEndMarksItsOwner` (the remote-stop case) |
| Commands that can only make the transmitter safer (`TX_LATCH 0`, `TX_PTT 0`, `RUN 0`, `TX_CLOSE`, `TX_REMOTE_ARM 0`) are NEVER refused `BUSY` and never pile up: at most one ticket of each kind is outstanding per session, and a newer one of the kind is merged with it (7.2). Merging never moves a command: every send takes effect in its own place, and only a repeat that cannot change anything is dropped, so sent once or N times, merged or not, the state is the same (checked exhaustively). Never merged into a ticket whose result has been read; results in ticket order, merged ones included. One REFUSED at submit (malformed, no grant) is not a safer command: never merged, its own ticket and refusal, counted in the 256 and so possibly `BUSY` (round-5 review: a NaN `LATCH 0` replaced the valid one and the latch stayed closed) | one per kind (`FOXAPI_SAFETY_RESERVE` 5) | yes | `testSaferCommandsAreNeverBusy`, `testSaferCommandsDoNotPileUpInTheQueue`, `testMergingKeepsSafetyInOrder`, `testMergedSendsKeepTheirPlace`, `test_merge_exhaustive`, `testRefusedSaferCommandsAreNotMerged`, `testRefusedSaferCommandCanBeBusy`, `testMergedResultsKeepTicketOrder`, `testTicketBooksUnderMerging` |
| A session that has CLOSED or been DETACHED (login revoked or expired) before its queued commands are applied has only the ones that make the transmitter safer applied; everything else it left queued is refused (7.2). A key or consent it held is released at the START of the next pass, before any command is applied. **Decision (round-6 review, L8):** so a window that releases its own latch and closes in the same pass has its latch ended by the CLOSE, a marking end - stricter than its own release, and it withdraws every other window's exemption. Kept: the close pass stays simple and fail-safe, and an interface following the key protocol (the row above) presses through it without noticing | — | yes (`DETACHED`) | `testDetachedSessionsQueuedCommandsAreRefused`, `testGoneSessionsApplyOnlySaferCommands`, `testGoneWindowsReleaseStillReleases`, `testDetachedKeyHolderIsReleasedFirst` |
| Configuration never restores a key or a consent | — | yes (no persisted key state) | by construction |
| Test options may only SHORTEN these timers | — | yes | `testTimersCannotBeLengthened`, `testRemoteKeepAliveCannotBeLengthened` |

The real engine must add the app's second, independent dead-man: the TX
thread itself checks how stale the control thread's last tick is and unkeys
when it is older than 1 s (the app's `Transmitter::threadBody` checks the GUI
thread's tick the same way), so a wedged control thread cannot leave a key
closed (docs/ENGINE-EXTRACTION.md, stage 3). The mock has no TX thread.

**GAP**: microphone device list and peak meter, blocks sent/short counters.

### 6.11 Bookmarks — `FOXAPI_CAP_BOOKMARKS`
Modelled on the app's `core/freq_manager`: one list, sorted by frequency
(stable), where each bookmark has a name, frequency, mode, **bandwidth**, a
**group** ("" = ungrouped) and a **favourite** flag. An imported frequency
list (SDR# XML, CSV) is not a separate object: it is bookmarks with a group,
exactly as the app stores them since 0.99.19 - so 0.2 retired the
`FREQ_LISTS` capability, list and `FREQ_LIST_TUNE` op of 0.1. List
`BOOKMARKS` (id; name; detail = group; value[0] Hz, [1] mode, [2] bandwidth;
`FOXAPI_ITEM_FAVOURITE`), `BOOKMARK_GROUPS` (name; value[0] entries).
Commands: `BOOKMARK_ADD` (current frequency, mode and bandwidth, ungrouped; a
name already in use gets " (2)", as `FreqManager::add`), `BOOKMARK_TUNE`
(frequency, mode and bandwidth), `BOOKMARK_REMOVE`, `BOOKMARK_FAVOURITE`,
`BOOKMARK_REMOVE_GROUP`, `BOOKMARK_IMPORT` (content, not a path; spec). The
app's list can hold tens of thousands (imports): interfaces page through
`read_list` without a cap. **GAP**: export (SDR# XML), rename, edit.

### 6.12 Scanner — `FOXAPI_CAP_SCANNER`
State `SCANNER_ACTIVE`. Commands `SCANNER_RUN` (start, stop, step),
`SCANNER_CONFIG` (dwell, hold, resume, listen ms), `SCANNER_SKIP`.
**GAP**: current scan frequency and progress.

### 6.13 Settings — `FOXAPI_CAP_SETTINGS`
`get_setting(key)` and `SETTING_SET "key=value"` (ADMIN). Keys are the
engine's config keys (the app's `core/config.hpp`). **GAP**: a list of keys
with types and ranges, so an interface can build a settings page without
hard-coding them.

### 6.14 Plugin store — `FOXAPI_CAP_PLUGIN_STORE` (ADMIN)
List `CATALOGUE`; commands `STORE_FETCH`, `STORE_CANCEL`, `STORE_INSTALL`
(id + legal-notice acknowledgement — the same gate as the desktop),
`STORE_UPDATE_ALL`, `STORE_REMOVE`. Interface plugins install through the
same store.

### 6.15 Reports and telemetry — `FOXAPI_CAP_REPORTS`, `FOXAPI_CAP_TELEMETRY`
Commands `PROBLEM_REPORT` (bug / dislike), `FEATURE_REQUEST` (grant VIEW),
`TELEMETRY_ENABLE` (ADMIN). Crash capture stays in the engine process; an
out-of-process interface's own crashes are reported by the interface's
own handler. **GAP**: report text is limited to 255 bytes by
`FoxCommand::text` in 0.2 (a blob channel is needed), contact field, the
"did not close normally" offer (last-run-crashed flag, report text, reports
folder).

### 6.16 Position, GPS, band plan
Commands `SET_POSITION`, `GPS`, `SET_BAND_PLAN`; lists `SERIAL_PORTS`,
`BAND_PLAN`. Country (it selects the band plan region) is a setting.

### 6.17 Updates and listeners
`UPDATE_CHECK` (ADMIN), `SERVER_CONFIG` (web and CAT listeners, JSON, LOCAL
+ ADMIN only). **GAP**: update result and install (arguably the host
application's job, not the engine's).

## 7. Mechanics

### 7.1 Reads (snapshots)
`read_state` fills a `FoxReceiverState`. It is one consistent copy — no field
can come from a different instant from another. `seq` and the group counters
(`tuneSeq`, `modeSeq`, `deviceSeq`, `audioSeq`, `displaySeq`, `txSeq`,
`listSeq`) advance when a non-measurement field changes; an interface
re-reads lists when `listSeq` moves. The engine patches in the per-session
fields (`grants`, `TX_KEY_MINE`, `TX_LATCH_RELEASE_FIRST`) on read;
`TX_LATCH_RELEASE_FIRST` is kept per session by the control thread and may
trail the rest of the snapshot by one control pass.

### 7.2 Commands and results
`FoxCommand { op, target, flags, num[4], ival[2], text[256] }`; each op
documents its slots (the header lists them next to the op).

`submit(cmds, n, results)` hands the batch over and returns at once. Its
answer for the WHOLE call: the number taken, or `BAD_ARGUMENT` (NULL, a
stride below `FOXAPI_MIN_COMMAND` or `FOXAPI_MIN_SUBMIT_RESULT`), `LIMIT`
(n > `FOXAPI_MAX_BATCH`, 1024 - checked before anything is read or sized by
n), `DETACHED`; otherwise the number of NEW tickets given. Per command
(`FoxSubmitResult`): `OK` with a new ticket; `NO_CHANGE` with an existing
ticket (a safer command merged, below); or `BUSY` when the session already
has `FOXAPI_MAX_PENDING` (256) ordinary commands outstanding - queued, being
applied, or answered but not yet polled (not taken: no ticket, no result).
Nothing else: every judgement of a command is a result.

**Safer commands are never BUSY and never pile up.** A command that can
only make the transmitter safer - `TX_PTT 0`, `TX_LATCH 0`, `RUN 0`,
`TX_CLOSE`, `TX_REMOTE_ARM 0`, five KINDS - is not held to 256. Instead a
session has at most ONE ticket of each kind outstanding (queued, or answered
but not yet read). A safer command of a kind that already has one is
MERGED with it: `submit` answers `NO_CHANGE` with THAT ticket (it gets no
ticket of its own and is not counted in `submit`'s return), the command is
still applied, and the ticket's one result reports the latest application
(or the one before it, when the result was read while a merged application
was still queued; the kind then takes a new ticket next time).

**Merging touches tickets and results, never where a command takes
effect.** Every send is applied in its own place in the queue, exactly as if
it had not been merged, with one exception that keeps the queue bounded: a
merged send is DROPPED when the same session's same kind is already queued
and nothing but (valid) safer commands, from any session, has been queued
since. Such a send cannot change anything - safer commands never close the
key, latch, start, open a transmitter or give consent, so after the first
application nothing between can give the second one anything to do: the key
it would open is open, the latch it would end has ended, no latch has closed
since (so there is no new mark), and the sender is already exempt. So a
command sent once or N times, merged or not, leaves the same state - and
this is checked, not argued: `test_merge_exhaustive` runs every sequence of
1 to 4 commands from three local sessions over `LATCH 0`, `LATCH 1`, `RUN 0`,
`TX_CLOSE` and `PTT 0` from three starting states (64,450 cases up to
renaming sessions, plus 6,000 random ones of 5 to 8 commands), once all in
one control pass (repeats merge) and once one command a pass with each
result read first (nothing merges), and requires the key, latch, hold,
running, transmitter, consent, end reasons and every mark with its exempt
sessions to be identical. (Round-6 review: a merged copy used to MOVE to its
last send's place, and B `LATCH 0`, A's own `LATCH 0`, B `LATCH 0` then ended
A's latch as A's own release, unmarked, where the same commands one pass
each ended it as B's, marked - the same with `RUN 0` or `TX_CLOSE` in B's
place. Round 5's patch for moved copies, applying them "early" before any
command that could undo them, was stricter than the unmerged order and is
gone: a merged `TX_CLOSE` around another window's `TX_OPEN` and a PTT now
does exactly what the four commands do one pass each - the PTT keys and the
second close opens it.) So one session's safer commands occupy at most one
queue place per kind between two commands that are not safer, even while
the control thread is held. So an interface that has stopped reading its results can always
open the key, one that sends `PTT 0` every frame uses one place, not one a
frame (round-4 review: 16 of them used the old shared 16-place reserve and
then `LATCH 0` and `RUN 0` were `BUSY` with the latch held), and a session
never holds more than 256 + 5 unread results.

Two more rules (round-5 review):
- **Never into a read ticket.** A send of a kind whose ticket's result
  `poll_results` has already returned takes a NEW ticket, even while a merged
  copy of the old one is still queued: a merge into an answered ticket would
  be answered to nobody.
- **Only valid commands merge.** A safer command REFUSED at submit
  (malformed - NaN, text with no NUL -, no grant, a remote session's
  `LATCH 0`) changes nothing, so it is not a safer command: it is never
  merged and never replaces a valid one of its kind in the queue; it takes
  its own ticket and its own refusal as an ordinary command, and so counts
  toward the 256 and may be answered `BUSY`.

All five safer commands are idempotent in effect: applying one twice in a
row leaves the key, the transmitter, the receiver, the consent and the latch
marks as applying it once (only the change counters move again); `LATCH 0`
became so in round 5, when it stopped clearing marks (6.10). The dropped
repeats above rely on a stronger fact, which the exhaustive test checks.

New tickets are per session, start at 1 and only increase. Only the ENGINE
assigns tickets and decides what is merged - its answer to `submit` is
where it says so (`OK` and a new ticket, or `NO_CHANGE` and the ticket the
command was merged into) - and nothing on the far side of a transport
predicts either: a remote client library maps its own request ids to the
engine's tickets from the answers it receives (docs/TRANSPORTS.md 2.2).
(Round-5 review: 0.2.2 told a remote client to number batches and answer
merges itself, which could disagree with the engine - it would merge where
the engine queued afresh or refused - and could merge into a ticket whose
result it had already handed to the interface.)

Every ticketed command yields exactly one `FoxCommandResult {status, ticket,
op, flags, applied[2], message}`, in ticket order - merged ones included:
a merged command is applied in the place of its LAST send, which can be
behind later tickets of the session, and its result still comes before
theirs (round-5 review: `RUN 0` t1, `SET_VOLUME` t2, `RUN 0` merged into t1
came out 2 then 1):
- `FOXAPI_RESULT_REFUSED` is set on a result EXACTLY when its status is not
  `OK`, and then nothing changed; `OK` never carries it.
- **refused** at submit: `BAD_ARGUMENT` (NaN,
  unterminated or empty text), `OUT_OF_RANGE`, `DENIED` (grant; a remote
  session asking for more than the PTT), `UNSUPPORTED` (unknown or
  unimplemented op), `NO_DEVICE` (a key request with no transmitter - judged
  against the state at submit, so it never waits in the queue for a radio);
- **refused or failed when applied** (the state had moved since submit):
  `NOT_FOUND`, `BUSY` (another session holds the key), `DENIED` (a latch
  mark not yet released with `LATCH 0`, the latch re-arm time, consent
  withdrawn meanwhile), `NO_DEVICE`, `LIMIT` (a full list), `DETACHED` (the
  session was closed, or detached - its login revoked or expired - before
  the command was applied: of what it queued only the commands that make the
  transmitter safer are applied, so a closed window's queued
  `TX_REMOTE_ARM 1` can never let a remote PTT through), or `NO_CHANGE`
  (already so: a re-latch while latched);
- **applied**: `OK`, with `FOXAPI_RESULT_CLAMPED` when the value in force is
  not the one asked for (bandwidth, VFO offset, gain, display range).

No result is ever dropped. A command counts as outstanding until its result
has been READ by `poll_results`, so a session never has more than 256 + 5
unread results (one per safer kind included); an interface that stops
polling is answered `BUSY` for new commands
instead of silently losing answers to old ones. (That is why there is no
overflow marker for results as there is for events: with this bound, results
cannot overflow. An interface should drain `poll_results` every frame until
it returns fewer than it asked for.)

A session opened with a login token lives only as long as the token: when
the token expires (12 hours at most) or is revoked (`logout`), the session is
DETACHED - checked on the engine's control thread every pass and again on
every `submit`, `open_session` and `logout` that presents the token; a token
is only ever dropped together with its sessions - and any key or consent it
held is released before anything it had queued would have been applied. The
engine then closes the detached session itself on the same control pass
(after the safer commands it left queued), so it holds no place; its handle
answers `DETACHED` until `close_session`.

### 7.3 Events
`subscribe(mask)` then `poll_events`. Kinds: `STATE` (which groups moved),
`FAULT`, `TX_UNKEYED`, `NOTICE`, `DECODER_TEXT`, `DECODER_IMAGE`,
`LIST_CHANGED`, `SESSION`, `OVERFLOW`. Each session has a ring of 256; when
it overflows the oldest are dropped and the next poll starts with an
`OVERFLOW` carrying the count, so a slow interface knows.

### 7.4 Lists
`read_list(list, index, out)` returns the entry count and fills `out` when
`index < count`. Entry `FoxListItem { list, index, flags, id, value[4],
name[96], detail[160] }`; `FOXAPI_ITEM_ACTIVE` marks the selected entry. The
meaning of `value[]` is in the header next to each list.

### 7.5 Streams
Spectrum by sequence number; audio and I/Q by absolute frame cursor with a
drop count. In-process, audio is the engine's 48 kHz finished audio;
remotely a reduced rate (docs/TRANSPORTS.md). I/Q is never delivered at the
full device rate to a remote session.

## 8. Command reference

Implemented by the mock engine (and exercised by `tests/`):

| Op | Grant | Slots | Clamp / refusal |
|---|---|---|---|
| `RUN` | SETTINGS | ival0 0/1 | start clears a fault |
| `SET_CENTRE` | TUNE | num0 Hz | 0 < f ≤ 6 GHz |
| `SET_FREQUENCY` | TUNE | num0 tuned Hz | VFO kept, centre follows |
| `SET_VFO_OFFSET` | TUNE | num0 Hz | \|off\| < 1e8; clamped to ±(rate − bw)/2 |
| `STEP_TUNE` | TUNE | ival0 steps (≠0), num0 step Hz | |
| `SET_MODE` | SETTINGS | ival0 demod | bandwidth → mode default |
| `SET_BANDWIDTH` | SETTINGS | num0 Hz | 100 Hz – 10 MHz; clamped to 0.9 × channel |
| `SET_SQUELCH` | SETTINGS | num0 dB | −200 – +20 |
| `SET_VOLUME` | SETTINGS | num0 | 0 – 1 |
| `SET_MUTED`, `SET_STEREO`, `SET_AUTO_NOTCH` | SETTINGS | ival0 0/1 | |
| `SET_DEEMPHASIS` | SETTINGS | ival0 0/1/2 | |
| `SET_NR` | SETTINGS | ival0 on, ival1 set-strength, num0 0–1 | |
| `SET_NOTCH` | SETTINGS | ival0 on, ival1 set, num0 10–20000 Hz, num1 Q 0.1–1000 | |
| `SET_DISPLAY_RANGE` | SETTINGS | num0 min, num1 max dB | −200 – +40; 10 dB minimum span |
| `SCAN_DEVICES`, `SELECT_SOURCE`, `SET_SAMPLE_RATE`, `SET_GAIN`, `SET_DEVICE_AGC` | SETTINGS | as header | id/rate/stage must be listed |
| `BOOKMARK_ADD`, `BOOKMARK_TUNE`, `BOOKMARK_REMOVE`, `BOOKMARK_FAVOURITE`, `BOOKMARK_REMOVE_GROUP` | SETTINGS / TUNE (tune) | text name / ival0 id (ival1 0/1) / text group | 1000 bookmarks in the mock; sorted by frequency; names made unique |
| `TX_OPEN`, `TX_CLOSE`, `TX_PTT`, `TX_LATCH`, `TX_SET_MODE`, `TX_SET_FREQUENCY`, `TX_SET_POWER` | TRANSMIT | as header | the key rules (6.10) |
| `TX_REMOTE_ARM` | TRANSMIT, LOCAL only | ival0 0/1 | the local operator's consent to remote PTT (6.10) |
| `SETTING_SET` | ADMIN | text key=value | mock test hooks: `mock.fault=<why>`, `mock.bookmark=name\|hz\|mode\|bw\|group`, `mock.stall=<ms>` (the control thread is busy that long, at most 1 s, inside the pass), `mock.stallAfterPass=<ms>` (the same, but after the pass's keep-alive and token checks, before the next pass takes the queue), `mock.openDelay=<ms>` (`open_session` waits that long between checking a token and listing the session, at most 1 s), `mock.gather=<n>` (the next control pass waits, at most 2 s, until n more commands have been submitted - merged ones included - and takes them all at once); `get_setting("mock.queueDepth")` reads how many commands wait for the control thread, `get_setting("mock.keyState")` the key, latch, hold, running, transmitter, consent, end reasons and every mark with its exempt sessions as the last pass left them |

Every other op in the header is **spec**: the mock refuses it with
`FOXAPI_UNSUPPORTED` (a result) and does not set the capability bit.

## 9. Errors

| Code | Value | Meaning |
|---|---|---|
| `FOXAPI_OK` | 0 | done (read, result) / taken by the transport (submit) |
| `FOXAPI_NO_CHANGE` | 1 | a read: nothing newer than the cursor; a result: it was already so; `submit`: a safer command merged with the one of its kind already outstanding (7.2) |
| `FOXAPI_DENIED` | −1 | the session lacks the grant (or a remote session asked for more than the PTT, or pressed it without local consent; or, for `LATCH 1`, the session's principal is marked and this session has not sent `LATCH 0` since, or the re-arm time is running - 6.10); `logout` of the engine's configured token |
| `FOXAPI_OUT_OF_RANGE` | −2 | a value outside what the engine accepts |
| `FOXAPI_NO_DEVICE` | −3 | needs an open radio or transmitter |
| `FOXAPI_FAILED` | −4 | the engine could not do it |
| `FOXAPI_BAD_ARGUMENT` | −5 | NULL, NaN, a short struct, unterminated text |
| `FOXAPI_UNSUPPORTED` | −6 | this engine or radio has no such thing |
| `FOXAPI_BUSY` | −7 | a bounded queue is full, or a read kept overlapping |
| `FOXAPI_DETACHED` | −8 | the session or engine is closed; as a result, the session was closed or detached before the command was applied (any command but a safer one) |
| `FOXAPI_NOT_FOUND` | −9 | no such id, key or entry |
| `FOXAPI_LIMIT` | −10 | a quota is full |
| `FOXAPI_WRONG_THREAD` | −11 | not allowed on this thread |
| `FOXAPI_UNAUTHENTICATED` | −12 | remote: no valid token |
| `FOXAPI_VERSION_MISMATCH` | −13 | the two sides cannot talk |

−1 … −11 are the same numbers as `CASCADE_API_*`.

## 10. Coverage: every current control → the API

Built from `src/gui/app_window.hpp/.cpp`, `rail_banks.hpp` and the other
views of the FoxSDR app at 0.99.35. **Finding that shapes the extraction:
today no desktop control goes through `AppWindow::applyControlRequest`** —
only the web remote, CAT and plugins do; every widget changes state
directly. So "on the shared path" below means an operation a browser, CAT
or plugin can already reach, not that the desktop uses it.

Status: **M** = in the API and implemented by the mock; **G** = also drawn by
the standard GUI plugin's first slice; **S** = in the API (spec), no engine
implements it yet; **I** = interface-local, needs no engine API; **GAP** =
no API yet.

### 10.1 Deck (`drawToolbar`)

| Control | App today | API | Status |
|---|---|---|---|
| STOP/RUN dome | `pipeline_.start/stop` (+ stops recordings) | `RUN`; `RUNNING` | M G |
| MASTER lamps RUN / DEC / MUTE / FAIL | read-only | `RUNNING`, `DECODER_ACTIVE`, `MUTED`, `FAULTED` | M G (DEC always off in the mock) |
| Counter: wheel over a figure | `tuneAbsoluteHz(±place)` | `STEP_TUNE` (step = 10^place) | M G |
| Counter: UP/DN switch per figure (hold repeats) | `tuneAbsoluteHz` | `STEP_TUNE` | M G (click; hold-repeat not yet) |
| Counter: click → typed entry, Enter/Esc | `parseFrequencyHz` → tune | `SET_FREQUENCY` | M (GUI: not yet) |
| TUNE UP / TUNE DN (key bindings Ctrl+Up/Down) | 10 kHz steps | `STEP_TUNE` | M G |
| RCVR lamp | read-only | `RUNNING` | M G |
| VOLUME dial | `audio().setVolume` | `SET_VOLUME`; `volume` | M G |
| SAMPLE RATE meter | read-only | `sampleRateHz` | M G |
| VOLUME meter | read-only | `audioLevelDb` | M G |
| Title rail: minimise / maximise / close / drag | GLFW | — | I |

### 10.2 Centre panels (`drawCenterPanels`)

| Control | App today | API | Status |
|---|---|---|---|
| Spectrum trace, grid, axis | `SpectrumFrame` | `read_spectrum`, `dbMin/dbMax` | M G |
| Click to tune (Shift = no snap) | `setVfoToAbsoluteHz` | `SET_VFO_OFFSET` | M G (no snap yet) |
| Drag the VFO band | offset | `SET_VFO_OFFSET` | M (GUI: not yet) |
| Drag a band edge | bandwidth | `SET_BANDWIDTH` | M (GUI: not yet) |
| Wheel zoom, double-click unzoom | `scale_` | — (display) | I; remote wants a sub-range read: GAP |
| Waterfall + click/drag | texture | history of `read_spectrum` | M G |
| Band plan overlay | `band_plan` | `LIST_BAND_PLAN` | S |
| Bookmark markers | bookmarks | `LIST_BOOKMARKS` | M (GUI: not drawn yet) |
| Plugin markers | plugin API marks | `LIST_MARKERS` | S |
| File drop → bookmark import | `freq_import` | `BOOKMARK_IMPORT` (content) | S |

### 10.3 FUNCTION SELECT rail

Bank keys F1–F5 and the rail footer text: **I** (the footer reads
`deviceName`, `sampleRateHz`, `channelRateHz`, `audioUnderruns` — M G).

**SIGNAL bank**

| Control | App today | API | Status |
|---|---|---|---|
| Patch switch row (opens PATCH) | `patchOpen_` | page toggle | I; patch state GAP |
| Source combo | `selectSource` | `LIST_DEVICES`, `SELECT_SOURCE` | M G |
| Refresh | `scanNative/scanSoapy` | `SCAN_DEVICES` | M G |
| "Where FoxSDR looked" tree | scan diagnostics | — | GAP |
| IQ file path + Open | `pipeline_.setSource(file)` | `SELECT_SOURCE "file:…"` LOCAL only | S |
| Pluto URI + Open | Pluto | `SELECT_SOURCE "pluto:…"` LOCAL only | S |
| Rate combo | `applySourceRate` | `LIST_SAMPLE_RATES`, `SET_SAMPLE_RATE` | M G |
| Antenna combo | `setAntenna` | `LIST_ANTENNAS`, `SET_ANTENNA` | S |
| Auto gain | `setAutoGain` | `SET_DEVICE_AGC` | M G |
| Gain slider per stage | `setGainDb` | `LIST_GAINS`, `SET_GAIN` | M G |
| Bias tee | `biasTeeTicked` | `SET_BIAS_TEE` | S |
| SDRplay FM notch, DAB notch, HDR, ADC dither, randomiser | `device_` setters | `LIST_DEVICE_OPTIONS`, `SET_DEVICE_OPTION` | S |
| Mode keys NFM…RAW | `setModeIndex` | `LIST_MODES`, `SET_MODE` | M G |
| VFO slider | `setVfoOffsetHz` | `SET_VFO_OFFSET` | M G |
| Bandwidth combo | `setVfoBandwidthHz` | `LIST_BANDWIDTHS`, `SET_BANDWIDTH` | M G |
| Squelch slider | `setSquelchDb` | `SET_SQUELCH` | M G |
| De-emph combo | `setDeemphasisUs` | `SET_DEEMPHASIS` | M G |
| Stereo checkbox, ST lamp, pilot | `setStereoEnabled` | `SET_STEREO`; `STEREO_ACTIVE`, `pilotLevel` | M G |
| RDS readouts (PS, RadioText, groups/errors) | `rdsSnapshot` | — | GAP |
| S-meter | `signalPowerDb` | `sMeter`, `signalDb` | M G |
| Noise reduction + strength | pipeline | `SET_NR` | M (GUI: not yet) |
| Notch + freq + Q | pipeline | `SET_NOTCH` | M (GUI: not yet) |
| Auto notch (+ engaged readout) | pipeline | `SET_AUTO_NOTCH`; readout GAP | M / GAP |
| Sinks: output device combo | `AudioOpen` | `LIST_AUDIO_DEVICES`, `AUDIO_DEVICE` | S (mock lists one) |
| Mute (key M; the GUI slice has a checkbox) | `userMuted_` | `SET_MUTED` | M G |
| Transmit switch row (opens TRANSMIT) | `transmitOpen_` | page toggle | I |
| Record IQ / Stop IQ, Record audio / Stop audio | recorder | `RECORD_IQ`, `RECORD_AUDIO` | S |

**DECODE bank**

| Control | App today | API | Status |
|---|---|---|---|
| Plugin store / Plugins (fitted modules) switch rows | window toggles | — | I |
| Blocked plugin rows: Update, Remove → Confirm | store | `STORE_INSTALL`, `STORE_REMOVE`; `LIST_PLUGINS` flags | S |
| Decoder status rows: show/hide output | page toggle | `LIST_DECODERS` | I / S |
| STOP ALL n RUNNING | `setPluginStopped` | `DECODER_STOP_ALL` | S |
| Per-decoder STOP/START | `setPluginStopped` | `DECODER_START/STOP` | S |
| Mute audio while running | `setPluginMutes` | `PLUGIN_MUTE` | S |
| Plugin preset buttons | `applyPluginPreset` | `LIST_PRESETS`, `PLUGIN_PRESET` | S |
| User preset, x forget, "+ Save … as a preset" | user presets | `LIST_USER_PRESETS`, `USER_PRESET_SAVE/FORGET` | S |
| Receiver control checkbox per plugin | `setPluginTuneAllowed` | `PLUGIN_GRANT` | S |
| Target details: Go to on map, Follow | map view | `LIST_TRACKS` | I / S |
| Map page rows, plugin window rows (pictures, panels, instruments) | window toggles | pictures `read_image` S; panels, instruments GAP | I / S / GAP |

**VIEW bank**

| Control | App today | API | Status |
|---|---|---|---|
| Min dB / Max dB | `spectrum_->setRange` | `SET_DISPLAY_RANGE` | M G |
| Frequency display style | `tunerDisplayStyle` | foxsdr-ui/1 `counter.face` | I |
| Aircraft icons px, trail width | map view | — | I |
| Band plan checkbox, size, colour | overlay style | — | I |
| Band plan region | `bandPlanSelection` | `SET_BAND_PLAN`, `LIST_BAND_PLAN` | S |
| Demod scope switch row | page toggle | — | I; scope data GAP |
| Receiver position: lat/lon, Set RX here, offers | `rxLatDeg/rxLonDeg` | `SET_POSITION` | S |
| GPS control | `gps_reader` | `GPS`, `LIST_SERIAL_PORTS` | S |
| Radar scope / Leave radar scope | `scopeMode_` | — | I |
| Bookmarks: name + Add current | `freq_manager` | `BOOKMARK_ADD` | M G |
| Bookmarks: click to tune, x delete | | `BOOKMARK_TUNE`, `BOOKMARK_REMOVE` | M G |
| Bookmarks: star/favourite | | `BOOKMARK_FAVOURITE` | M |
| Bookmarks: import path + Import | `freq_import` | `BOOKMARK_IMPORT` (content) | S |
| Bookmarks: group combo, favourites only, on the spectrum | filters | `LIST_BOOKMARK_GROUPS` (M); filters I | M / I |
| Bookmarks: Remove this group | `removeGroup` | `BOOKMARK_REMOVE_GROUP` | M |
| Bookmarks: Export for SDR# | | — | GAP |
| Scanner: start / stop / step, Start scan, Stop scan | `scanner_` | `SCANNER_RUN` | S |
| Scanner: dwell, hold, resume, listen | | `SCANNER_CONFIG` | S |
| Scanner: Skip | | `SCANNER_SKIP` | S |
| Scanner progress (current frequency) | | — | GAP |

**EXTEND bank**

| Control | App today | API | Status |
|---|---|---|---|
| Web access: serve, reachable from, port, user, password set/clear, apply | `applyWebSettings` | `SERVER_CONFIG` (LOCAL + ADMIN) | S |
| CAT: accept, port, reachable from other machines | CAT server | `SERVER_CONFIG` | S |

**SYSTEM bank**

| Control | App today | API | Status |
|---|---|---|---|
| Language | `i18n` | — | I |
| Country (selects band plan region) | config | `SETTING_SET country=…` | S |
| Suggest a better translation | opens a URL | — | I |
| Updates: check at startup, Show it again | updater | `UPDATE_CHECK` | S |
| Update banner: install, download, open site, not now | updater | — | GAP (host-level) |
| Serial ports: Refresh + table | `serial_port` | `LIST_SERIAL_PORTS` | S |
| Key bindings: capture, reset | `key_bindings` | — | I |
| Diagnostics: record crashes, full memory dump | config | `SETTING_SET` | S |
| Diagnostics: pointer ledger | debug overlay | — | I |
| Diagnostics: copy diagnostics, open reports folder | `diag_report` | — | GAP |
| Usage reporting switch | telemetry | `TELEMETRY_ENABLE` | S |
| What exactly is sent / privacy policy | links | — | I |

### 10.4 STATUS column (`drawStatusColumn`)

| Item | API | Status |
|---|---|---|
| AUDIO · UNDERRUNS | `audioUnderruns` | M G |
| DECODER OUTPUT (lines/s) | count `DECODER_TEXT` events | I (from S) |
| DECODERS n running of m | `decodersRunning`, `decodersFitted` | M G |
| SINK OPEN / MUTED / NO DEVICE / STOPPED | `SINK_OPEN`, `MUTED`, `RUNNING` | M G |
| SINK OPENING | — | GAP |
| RECORDER IQ / AUDIO / off | `RECORDING_*` | M G (flags) |
| RECORDER STALLED / FULL | — | GAP |
| WEB ACCESS port / REFUSED / off | `WEB_LISTENING`; port and reason GAP | S / GAP |
| RECEIVER device, antenna, gain, MS/s, FAULT/RUNNING/STOPPED | `deviceName`, `sampleRateHz`, `FAULTED`, `RUNNING`; antenna via list ACTIVE | M G |
| REQUEST A FEATURE | `FEATURE_REQUEST` | S (G: key drawn, disabled when the engine lacks `CAP_REPORTS`) |
| REPORT A BUG / DISLIKE | `PROBLEM_REPORT` | S (same) |

### 10.5 Pages and windows

| Page | Controls → API | Status |
|---|---|---|
| PLUGIN STORE | search, SHOW rockers, SORT: I · CHECK NOW `STORE_FETCH` · CANCEL `STORE_CANCEL` · FIT/UPDATE `STORE_INSTALL` · acknowledgement `ival0` · ADD ALL / UPDATE n `STORE_UPDATE_ALL` · rows `LIST_CATALOGUE` | S |
| FITTED MODULES | SCAN AGAIN `PLUGIN_RESCAN` · filters, RESET WINDOW SIZES: I · START/STOP `DECODER_START/STOP` · REMOVE → CONFIRM `STORE_REMOVE` · GRANT/REVOKE RECEIVER CONTROL / RADIO SETTINGS `PLUGIN_GRANT` · the module's command keys `PLUGIN_COMMAND` (the key list: GAP) | S / GAP |
| DECODER OUTPUT | Follow, Clear: I (history from `DECODER_TEXT`); backlog for a late interface: GAP | I / GAP |
| TRANSMIT | device + Open/Close `TX_OPEN/CLOSE` · MHz `TX_SET_FREQUENCY` · SPLIT `TX_SET_SPLIT` · CW/AM/NFM/USB/LSB `TX_SET_MODE` · power `TX_SET_POWER` · Microphone/Tone `TX_SET_INPUT` · Tone Hz `TX_SET_TONE` · PTT (Space) `TX_PTT` re-asserted · LATCH `TX_LATCH` · Listen `TX_SET_MONITOR` · microphone device, mic peak, blocks sent: GAP | M (open/close/PTT/latch/mode/freq/power) · S · GAP; GUI: status card only |
| PATCH | STOP/RUN `PATCH_RUN` · ALL OFF `PATCH_ALL_OFF` · add parts, canvas, wires, inspector, radio ON/OFF, device, centre, rate, squelch, map fit, sink output (WAV/MP3/speakers), face editors: GAP | S / GAP |
| DEMOD SCOPE | AUDIO/SPEC/I/Q/VECTOR/MPX, timebase, gain, AUTO, NORM/AVG/PERSIST: I drawing over scope taps — the taps: GAP (`CAP_SCOPE` declared, no read function in 0.2) | GAP |
| Radar scope mode | EXIT, RANGE ±, target select: I · GAIN knob `SET_GAIN` · POWER `RUN` | I / M |
| Map pages | fit, trails, coverage, sort, follow: I over `LIST_TRACKS` (S) · RX position `SET_POSITION` · GPS `GPS` · basemap tiles, track info: GAP | I / S / GAP |
| GPS control | port, baud, read/stop: `GPS`, `LIST_SERIAL_PORTS` | S |
| TARGET DETAILS | I over `LIST_TRACKS`; registry info GAP | I / GAP |
| Plugin picture windows | preset bar `PLUGIN_PRESET` · Save as BMP: I over `read_image` | S |
| Plugin panels and instruments | preset bar `PLUGIN_PRESET` · the rows/faces, nav-bearing OBS knob: GAP | GAP |
| FEATURE REQUEST | text, contact, SEND `FEATURE_REQUEST` (255 bytes, no contact field: GAP) | S / GAP |
| REPORT A BUG / DISLIKE | bug/dislike `ival0`, text, contact, SEND `PROBLEM_REPORT` (same limits) | S / GAP |
| "Sound is muted" modal, mute banner | Stop plugin `DECODER_STOP`; Keep running: I; plugin-mute flag GAP | S / GAP |
| "Did not close normally" offer | copy diagnostics, open reports folder, not now: GAP | GAP |

### 10.6 Key bindings (`key_bindings.hpp`)

All interface-local mappings onto the same commands: StartStop → `RUN`;
Mute → `SET_MUTED`; Volume± → `SET_VOLUME`; Squelch± → `SET_SQUELCH`; the
eight mode keys → `SET_MODE`; TuneStep± → `STEP_TUNE` 10 kHz; TuneSpan± →
`STEP_TUNE` by the span; Record → `RECORD_AUDIO`; TransmitPtt (Space) →
`TX_PTT` re-asserted while held; QuickTune, Zoom, Screenshot, Fullscreen,
banks, OpenSettings → I.

### 10.7 The gaps, in one list

1. RDS snapshot (PI, PS, PTY, RadioText, counters) — `CAP_RDS` is declared
   with nothing behind it yet.
2. Auto-notch readout (engaged, frequency).
3. Recorder detail: file, size, STALLED/FULL; recordings list.
4. Decoder output backlog and rate; plugin panels (rows) and instruments
   (state and inputs); a plugin's command-key list; plugin-imposed mute flag.
5. Demod-scope taps (audio, channel I/Q, MPX) at reduced size.
6. Map: basemap tiles, track registry info.
7. Patch page: graph read-back, per-node editing, per-radio state, per-node
   streams, file sinks.
8. Transmitter: microphone device list, mic peak, blocks sent/short.
9. Device-scan diagnostics; antenna in the state; sink OPENING; web
   listener port and refusal reason.
10. A long-text channel (reports, bookmark import, patch documents exceed
    `text[256]`).
11. Bookmarks export; scanner progress.
12. Diagnostics: last-run-crashed, report text, reports folder.
13. Update result and install (probably the host's, not the engine's).
14. Spectrum sub-range / higher resolution for zoom; waterfall backlog.
15. Multi-VFO (`target` reserved).
16. A settings catalogue (keys, types, ranges).
