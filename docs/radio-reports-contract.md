# Radio reports: the contract (schema 2)

Status: DRAFT 2, written 2026-11-16 on `feat/radio-reports`. Schema 1 (draft 1) was never deployed, so schema 2 replaces it: a
message of schema 1 is refused (`bad-schema`). Nothing in this file is deployed. The app, the Worker (`telemetry-worker/worker.js`,
`radio-reports.js`, `radio-journal-spec.js`) and the site (`foxsdr-site`, `radiofail.go`, `radiojournal.go`) are all held to THIS file
and to the fixture files in `telemetry-worker/test-fixtures/radio-reports/`, which each side's tests load. If a rule here and a
fixture disagree, the fixture is wrong; if a side and a fixture disagree, the side is wrong. The field tables of the journal kinds
(section 3) are ALSO a data file, `telemetry-worker/radio-journal-spec.js` (JSON copy `test-fixtures/radio-journal-spec.json`),
which the Worker and the site both read: a limit cannot differ between them, and a test fails when this document does not name a
field of the file.

Owner rules this implements (HANDOVER-FABLE-2026-11-16.md, OWNER-ANSWERS-2026-11-16.md): D4 ("I want to know every fail of every
radio, not an accumulation", sent whether or not the Diagnostics switch is on), D5 (the site shows RADIOS: one row per physical radio,
the serial stored and the key of the row, everything that happened to it, the FoxSDR versions it ran under; always on, kept forever;
C2 nothing lost to a crash; C8 a daily bound must never drop silently), decision 12 (the Usage reporting switch turns the journal off,
radio failure reports are still sent always), decision 13 (journals kept forever go to the NAS first), L7 (one-time users are seen
during their first session).

## 1. The messages and the shared envelope

Every message is `POST`ed as `application/json` to the usage endpoint the app already knows (`telemetryEndpoint()`,
`https://telemetry.foxsdr.com/`), one message per request. The Worker tells them from a usage record and from a beat by the `kind`
field. A usage record has no `kind`; a beat has `beat: 1`; neither changes shape (the beat gains one optional field, section 3.10).

Kinds: `radio_fail` (section 2), the journal kinds `radio_found`, `radio_open`, `radio_settings`, `radio_stream`, `radio_recovery`,
`radio_close` and `session_env` (section 3), `first_session` (section 7) and `radio_beat` (section 3.10, which the Worker builds from a
beat and the site accepts). `radio_batch` is RESERVED for a batched envelope (section 3.9) and refused as `reserved-kind`, which is
different from `unknown-kind` so that an app one release ahead is told apart from a hostile one.

The ENVELOPE (every kind except `radio_beat`):

| Field | Type | Limit | Required | Meaning |
|---|---|---|---|---|
| `schema` | integer | exactly `2` | yes | contract version |
| `kind` | string | one of the kinds above | yes | which message |
| `installId` | string | `^[0-9a-f]{32}$` | yes, except a `radio_fail` from an install with Usage reporting off (1.2) | the install id the usage report uses (`newInstallId()`). NOT called `id`: see section 11, "the old Worker" |
| `sessionId` | string | `^[0-9a-f]{16}$` | with `installId` | random per process start; not derived from anything |
| `seq` | integer | 0 to 1,000,000 | with `installId` | per session, counts every message of this family the process made, in order, starting at 0; a gap says a message was dropped or lost |
| `messageId` | string | `^[0-9a-f]{16}$` | only without `installId` (1.2), and then required | random per message |
| `t` | integer | 0 to 2,592,000 | yes | whole seconds since the session started when the event happened (never a clock time) |
| `age` | integer | 0 to 2,592,000 | no (default 0) | whole seconds the message waited in the on-disk queue before THIS send attempt; the site reads the event's time as its arrival minus `age`, so a queue flushed at the next start does not look like new activity. Recomputed at every attempt |
| `radioKey` | string | see 1.1 | yes for the radio kinds, `""` or absent for `session_env` and `first_session` | the physical radio the message is about |
| `body` | object | per kind | yes | the kind's own fields |

### 1.1 The radio key

`radioKey = <modelSlug> ":" <serialKey>`

* `modelSlug`: the body's `model`, lower-cased, every run of characters outside `a-z0-9` replaced by one `-`, leading and
  trailing `-` removed, cut to 40 characters.
* `serialKey`: the body's `serial` upper-cased, or `-` when the serial is empty.

Example: model `uhd b200`, serial `EDR04ZDB2` gives `uhd-b200:EDR04ZDB2`. A radio whose serial the app cannot read gives
`rtl-sdr:-`. The sender computes it; the Worker and the site COMPUTE IT AGAIN from `model` and `serial` and refuse a message whose
`radioKey` differs (`key-mismatch`). The key is how one physical radio seen from two installs becomes one radio on the site; it is
deliberately not derived from the driver word, because the same dongle can be opened by two drivers. A radio with no readable serial
is keyed `<model>:-`, so the site shows one row per model for those, and says on the row that its serial is unknown; two such dongles
of one model, on one or on two installs, are therefore ONE row (the site cannot tell them apart and says so; it never invents a serial).

### 1.2 A radio failure from an install with Usage reporting OFF

Decision 12: the Usage reporting switch turns the journal off, but radio failure reports are still sent, always. Such an install has
no install id (the id exists only while Usage reporting is on and is deleted when it is turned off). So a `radio_fail` is valid
WITHOUT `installId`, and then, and only then:

* `installId`, `sessionId` and `seq` are all ABSENT (a message that has `sessionId` or `seq` but no `installId` is refused,
  `bad-field`: they would link one install's reports to each other). `messageId` is present (random 16 hex characters, new for
  every message, never reused, not derived from anything).
* The message carries nothing that links two reports of the install to each other except the radio's own model and serial, which
  the owner decided are stored: no id, no session, no sequence, no clock time. `t` and `age` are seconds, `attempt` counts the
  failures of THAT radio in the process (it links two reports of one radio, which the key already does). What remains that could
  identify an install in practice is the combination of FoxSDR version, OS build, architecture and channel; nothing here uses it
  to link reports, the site does not group by it, and section 10 says so to the owner.
* `messageId` is only so that a retry whose answer was lost is stored once; the site keeps the last 100,000 of them.
* The site cannot tie such a report to an install, so "delete by install id" cannot remove it; the site offers deletion by radio key
  (section 8).
* No other kind has an anonymous form: with Usage reporting off the journal, `session_env`, `first_session` and the beat are not
  sent at all.

## 2. `radio_fail`: one radio that did not open

ONE message per failed open, every time: the start-up restore, a radio the user picked, the saved-radio retry, a patch page radio,
the automatic reopen after a driver fault, the sound-card input. Sent whether or not the Diagnostics switch is on (owner answer
2026-11-15, "Send them always") AND whether or not Usage reporting is on (decision 12, section 1.2). Repeats are NOT folded by the
sender: a busy-again retry is a new message with a higher `attempt`. The only thing that ever stops a message is the daily bound (2.3),
and that is counted.

### 2.1 Body fields

| Field | Type | Limit | Required | Example | Meaning |
|---|---|---|---|---|---|
| `driver` | string | one of the 14 driver words (`rtlsdr hackrf airspy airspyhf sdrplay mirisdr rx888 pluto aor hydrasdr rtltcp soapy soundcard other`) | yes | `airspyhf` | `health::driverWord(kind)` |
| `model` | string | 1 to 32 chars, unchanged by `scrubModel` (no token holding `=` `:` `/` `\` `@`, no dotted quad) | yes | `Airspy HF+` | the model as the usage report sends it (`sanitiseDevice` / `nativeFamilyModel`, then `scrubModel`) |
| `serial` | string | `""` or `^[A-Za-z0-9._-]{1,40}$` | yes (may be empty) | `HF0001A2` | the radio's hardware serial, AS ITS OWN FIELD: the owner decided it is stored (D5 correction 2026-11-15). Empty when the driver cannot read one (rtl_tcp, Pluto by address, sound card, a driver that fails before it can). NEVER an address or a device argument string |
| `firmware` | string | `""` or `^[A-Za-z0-9._+ -]{1,48}$` | no | `3.1.0` | radio firmware version where known |
| `library` | string | same | no | `libairspyhf 1.6.8` | driver or library version where known |
| `reason` | string | 1 to 400 chars, section 5 | yes | `the radio stopped answering while restoring the crystal trim: a control transfer failed (Windows error 31, serial <stripped>)` | the FULL reason sentence the driver gave, put through `scrubUploadLine` first (serials, user names in paths, hosts, quoted names and frequencies removed from the TEXT), and THEN every path (a drive path, a `\\host\share`, anything under `\Users\`, `/Users/` or `/home/`) replaced by `<path>`: `scrubUploadLine` keeps the rest of a path, the Worker and the site refuse any path (section 5), so a path must not reach them |
| `reasonClass` | string | `busy driver bind absent timeout vendor rate other` | yes | `other` | `health::radioReasonWord(classifyRadioOpen(kind, reason))`, computed on the unscrubbed text |
| `trigger` | string | `startup-restore user-pick saved-retry patch-radio auto-reopen other` | yes | `startup-restore` | which call site |
| `attempt` | integer | 1 to 10,000 | yes | `1` | ordinal of the failed attempts of THIS radio key in THIS session (1 for the first) |
| `openMs` | integer | 0 to 3,600,000 | no | `5230` | how long the open took before it failed |
| `sampleRateSps` | integer | 0 to 1,000,000,000 | no | `768000` | the sample rate asked for, in samples per second. A sample rate, never a tuned or centre frequency |
| `superseded` | boolean | | no | `false` | true when the user had already moved on to another source when the result arrived (the engine returns early for these; the report is still filed) |
| `dropped` | integer | 0 to 1,000,000 | no | `0` | section 3.9: failures of this radio key since the previous SENT report of this key that were not sent because of the daily bound |
| `diagOn` | boolean | | yes | `false` | whether the Diagnostics switch was on. It does not change what is sent; it tells the owner which kind of install this was |
| `logLines` | array of strings | at most 40 lines, each 1 to 240 chars, section 5 | yes (may be empty) | `["12:01:07.331 soapy: airspyhf open failed ..."]` | the radio lines of the log ring, newest last, each put through `scrubUploadLine`. A radio line is one whose first word (after an optional `HH:MM:SS.mmm ` stamp) is `source:` `soapy:` `vendor:` `radio:` `rtltcp:` `aor:` `rx888:` `pluto:` `mirisdr:` `hackrf:` `airspy:` `airspyhf:` `rtlsdr:` `sdrplay:` `hydrasdr:` `usb:` or `patch:`; any other line is refused (`bad-field`). The app lane confirms this list against the log's real prefixes |
| `v` | string | 1 to 48 chars | yes | `0.99.109` | app version |
| `ch` | string | `store installer appimage tarball android` or `""` | no | `installer` | install channel |
| `uch` | string | `stable beta` or `""` | no | `stable` | update channel setting |
| `os` | string | 1 to 40 chars | yes | `Windows 10.0.22631` | OS and build, as the usage report |
| `arch` | string | 1 to 8 chars | yes | `x64` | architecture |

A `radio_fail` message has `radioKey` in the envelope and `body.model` and `body.serial` agree with it (1.1).

### 2.2 Response

| Status | Body | Meaning | Sender does |
|---|---|---|---|
| 204 | none | accepted and stored | delete the queued message |
| 400 | `refused: <code>` | this message can never be accepted (section 6) | if the code is `path-in-text`, `address-in-text`, `serial-in-text` or `frequency-in-text`: replace `reason` with `withheld: <code>` and `logLines` with `[]` and send it ONCE more (the failure itself, driver, model, serial and class, must still reach the owner; the fixture `valid-radio-fail-withheld` is that form). Any other code, or a refusal of the degraded form: delete it, count it locally, never retry |
| 413 | `refused: too-large` | over the kind's size (16,384 bytes for `radio_fail`, 8,192 for `first_session`, 4,096 for every other kind) | as 400 |
| 429 | `retry-after` seconds | the site's per-install, per-radio or total limit | keep, retry after the delay |
| 502, 503 | text | the Worker could not reach the site, or has no site configured | keep, retry with back-off |
| network error | | | keep, retry with back-off |

A journal event refused for a text-rule code is sent once more with the offending text field removed (the body without it), then dropped
and counted; every other refusal drops it and counts it.

Back-off: 1 minute, 5 minutes, 1 hour, then every 6 hours; a queued message is dropped (and counted) after 14 days.

### 2.3 Sender rules

1. Queue first, send second (section 3.9 for the queue's shape). The message is written to disk before any network call, so a crash or a
   quit does not lose it; whatever is left is sent at the next start (C2).
2. Not gated by Diagnostics and NOT gated by Usage reporting (decision 12). Gated only by an endpoint being configured. With an install id
   the message carries it; without one it is the anonymous form of 1.2.
3. THE DAILY BOUND. At most 100 `radio_fail` messages are sent per install per UTC day (per application data folder: the sender keeps the
   count itself; nothing on the server needs an id for it). At the bound: (a) the FIRST failure of every distinct (`radioKey`,
   `reasonClass`) of the day is always sent, bound or not; (b) later failures of the same pair are not sent but are counted, and the
   NEWEST one is kept as one pending message for that pair, replaced by each later failure; (c) the pending message is sent when the day
   rolls over, at the next start, or at quit, with `dropped` set to the number of failures of that radio key that were not sent since its
   previous sent report, so the last failure of a loop is always kept and the number lost is always said. Nothing is dropped without
   being counted; the site shows "N reports dropped by the daily bound" per radio and per day from the `dropped` values (C8).
4. The bound does not use `core::crash_upload`'s 24-hour signature dedup, its 5-a-day window, its persisted `UploadPolicyState` or the
   issue report's once-per-word rule. Those stay as they are for every other report.

### 2.4 NEVER in it

A tuned or centre frequency, a bookmark, a band name (a failure has no field for one), a file or recording name, anything decoded, a
position, an IP address or host name, a user or computer name, a path, the contents of the config. The text rules (section 5) and the
forbidden-field rule (section 4) are the enforcement, on the Worker and on the site; the sender's own scrubbing is the first line, not
the only one.

## 3. The journal: what happens to a radio, and to the machine around it

The journal is what makes the site's radio page: every event in the life of every radio in every session. It is ON for every install
from the first launch (owner decision 2026-11-15, "No, always on") and is turned off only by the Usage reporting switch (decision 12).
Every journal message uses the envelope of section 1 (with `installId`: there is no anonymous journal), the radio key (1.1), the text
rules (section 5) and the forbidden-field rule (section 4). The tables below are `radio-journal-spec.js`.

### 3.1 Fields every radio kind carries in its `body`

| Field | Type | Limit | Required | Example | Meaning |
|---|---|---|---|---|---|
| `driver` | string | the 14 driver words (2.1) | yes | `airspyhf` | |
| `model` | string | 1 to 32 chars, unchanged by `scrubModel` | yes | `Airspy HF+` | as 2.1 |
| `serial` | string | `""` or `^[A-Za-z0-9._-]{1,40}$` | yes | `HF0001A2` | as 2.1 |
| `v` | string | 1 to 48 chars | yes | `0.99.109` | FoxSDR version. It is what makes the radio's VERSION HISTORY: the site records the first time each version is seen with each radio and each install |
| `firmware` | string | `""` or `^[A-Za-z0-9._+ -]{1,48}$` | no | `3.1.0` | |
| `library` | string | same | no | `libairspyhf 1.6.8` | |
| `dropped` | integer | 0 to 1,000,000 | no | `0` | events of THIS radio key that the daily bound (3.9) kept off the wire since the previous event of this key that was sent |

Where a kind below says "sent when", it names the app's event point. A kind is sent once per event; nothing is folded except by the
daily bound.

### 3.2 `radio_found`: a radio seen

Sent when a scan (the start-up enumeration, a hot-plug rescan, the first-run radio check) sees a radio, once per radio per scan, so the
site can show a radio that is present but cannot be opened (C3: a known radio with no usable driver, with the model from the USB id and
the serial when the descriptor gives it).

| Field | Type | Limit | Required | Example | Meaning |
|---|---|---|---|---|---|
| (3.1) | | | | | |
| `binding` | string | `usable wrong-driver vendor-missing not-visible no-device not-checked` | yes | `wrong-driver` | the first-run check's own vocabulary (`setup` in the health vocabulary): whether a driver FoxSDR can use is bound to it |
| `source` | string | `scan first-run-check hotplug` | yes | `scan` | what saw it |
| `usbId` | string | `^[0-9a-f]{4}:[0-9a-f]{4}$` | no | `03eb:800c` | the USB vendor and product id: a model code, never a serial or a device path |
| `usbHost` | string | 1 to 64 chars, section 5 | no | `Intel USB 3.1 eXtensible Host Controller` | the product name of the USB host controller the radio sits on |

### 3.3 `radio_open`: an open began, or succeeded

A failed open is a `radio_fail` (section 2), not this. Sent when an open begins (`phase` `start`, written to disk BEFORE the call, so an open
that hangs or crashes the process leaves a start with no result) and when it succeeds (`phase` `ok`).

| Field | Type | Limit | Required | Example | Meaning |
|---|---|---|---|---|---|
| (3.1) | | | | | |
| `phase` | string | `start ok` | yes | `ok` | |
| `trigger` | string | the triggers of 2.1 | yes | `startup-restore` | which call site |
| `attempt` | integer | 1 to 10,000 | yes | `1` | ordinal of the opens of this radio key in this session |
| `openMs` | integer | 0 to 3,600,000 | no | `910` | how long the open took (with `ok`) |
| `sampleRateSps` | integer | 0 to 1,000,000,000 | no | `768000` | the rate asked for |
| `band` | string | a NAME from `lf mf hf vhf-low fm-broadcast air-band vhf-high uhf l-band shf other` | no | `fm-broadcast` | the band the radio was tuned within. NEVER a frequency: a number is refused (`bad-field`) |

### 3.4 `radio_settings`: the settings applied

Sent after an open applies the radio's settings, and whenever the user or the automatic control changes one, and when the radio REFUSES one
(the G3 control-refused paths: the setting is named in `refused`).

| Field | Type | Limit | Required | Example | Meaning |
|---|---|---|---|---|---|
| (3.1) | | | | | |
| `why` | string | `open user auto restore` | yes | `open` | what applied it |
| `sampleRateSps` | integer | 0 to 1,000,000,000 | no | `768000` | samples per second |
| `bandwidth` | integer | 0 to 1,000,000,000 | no | `600000` | the front-end filter bandwidth in hertz, a width and not a tuned or centre frequency (the key has no unit suffix: the section 4 rule forbids names ending in `hz`) |
| `gainMode` | string | `auto manual agc other` | no | `manual` | |
| `gainDb` | number | -200 to 200 | no | `12.5` | the overall gain |
| `ppm` | number | -2000 to 2000 | no | `-1.5` | the frequency correction |
| `biasTee` | boolean | | no | `false` | |
| `antenna` | string | 1 to 24 chars, section 5 | no | `RX2` | the antenna port name |
| `directSampling` | string | `off i q` | no | `off` | |
| `decimation` | integer | 1 to 65,536 | no | `4` | |
| `band` | string | the names of 3.3 | no | `fm-broadcast` | |
| `refused` | string | `rate bandwidth gain ppm bias antenna direct decimation` | no | `ppm` | the setting the radio refused |
| `reason` | string | 1 to 200 chars, section 5 | no | `the radio refused the frequency correction` | why it was refused, scrubbed |

### 3.5 `radio_stream`: stream health, and what is attached

Sent about once a minute per open radio (and the sender coarsens it, section 3.9), and at once with `note` when the app notices output that
is wrong (C4).

| Field | Type | Limit | Required | Example | Meaning |
|---|---|---|---|---|---|
| (3.1) | | | | | |
| `windowSec` | integer | 1 to 3,600 | yes | `60` | the seconds this event covers |
| `samplesPerSec` | integer | 0 to 1,000,000,000 | yes | `767940` | the sample rate actually achieved over the window; 0 is an opened radio that gave no samples |
| `expectedSps` | integer | 0 to 1,000,000,000 | no | `768000` | the rate asked for |
| `drops` | integer | 0 to 1,000,000,000 | yes | `0` | samples or buffers dropped in the window |
| `ringHighPct` | integer | 0 to 100 | no | `12` | the ring's high-water mark |
| `usbErrors` | integer | 0 to 1,000,000,000 | no | `0` | USB transfer errors in the window |
| `stalls` | integer | 0 to 1,000,000 | no | `0` | stalls in the window |
| `dspLoadPct` | integer | 0 to 1,000 | no | `21` | DSP load of this radio's chain |
| `openSec` | integer | 0 to 2,592,000 | no | `60` | how long the radio has been open this session |
| `modes` | array | at most 8 of `^[A-Za-z0-9+-]{1,12}$` | no | `["WFM","NFM"]` | the demodulator modes used in the window |
| `plugins` | array | at most 16 of `^[a-z0-9._-]{1,48}$` | no | `["adsb-decoder"]` | the plugin ids attached to this radio |
| `pluginFaults` | integer | 0 to 1,000,000 | no | `0` | plugin faults on this radio in the window |
| `note` | string | `""` `sounds-wrong` `audio-silent` `audio-clipping` `nothing-decoded` | no | `sounds-wrong` | set when the user pressed "this sounds wrong" or the app saw silence, clipping, or a decoder with nothing decoded while the signal meter moves |

### 3.6 `radio_recovery`: a recovery

Sent when the app starts a recovery of a radio (`start`, written before the attempt), when it succeeds (`ok`) and when it gives up
(`gave-up`).

| Field | Type | Limit | Required | Example | Meaning |
|---|---|---|---|---|---|
| (3.1) | | | | | |
| `phase` | string | `start ok gave-up` | yes | `start` | |
| `what` | string | `^[a-z0-9]{3,24}$` | yes | `usbstuck` | the application's own recovered word (`reopen`, `usbstuck`, `streamrestart`, `vendorcall`, ...). The site shows the word; it need not know it |
| `attempt` | integer | 1 to 10,000 | yes | `1` | ordinal of this recovery of this radio in the session |
| `reasonClass` | string | the classes of 2.1 | no | `timeout` | |
| `reason` | string | 1 to 400 chars, section 5 | no | `the stream stopped for 5 s` | scrubbed as 2.1 |
| `ms` | integer | 0 to 3,600,000 | no | `15000` | how long the recovery took |

### 3.7 `radio_close`: a radio closed, and why

| Field | Type | Limit | Required | Example | Meaning |
|---|---|---|---|---|---|
| (3.1) | | | | | |
| `cause` | string | `user switch-radio quit died unplugged fault error other` | yes | `died` | `user`, `switch-radio` and `quit` are clean; the rest are the radio ending, which the site shows red and alerts on |
| `openSec` | integer | 0 to 2,592,000 | yes | `144` | how long the radio was open |
| `reason` | string | 1 to 400 chars, section 5 | no | | scrubbed as 2.1 |

### 3.8 `session_env`: the machine, once a session

Sent once per session, at the start (queued with the first events; a retry is the same message). `radioKey` is `""` or absent.

| Field | Type | Limit | Required | Example | Meaning |
|---|---|---|---|---|---|
| `v` | string | 1 to 48 | yes | `0.99.109` | |
| `os` | string | 1 to 40 | yes | `Windows 10.0.22631` | OS and build |
| `arch` | string | 1 to 8 | yes | `x64` | |
| `ch` | string | the channels of 2.1 | no | `installer` | install channel |
| `uch` | string | `stable beta` or `""` | no | `stable` | update channel |
| `window` | string | `new legacy` or `""` | no | `new` | which window the session used (also on the usage report, 3.11) |
| `launches` | integer | 1 to 1,000,000 | yes | `7` | launches since install |
| `cpu` | string | 1 to 64, section 5 | yes | `Intel Core i7-8700K` | the processor's model name WITHOUT its clock speed: a brand string such as `... @ 3.70GHz` is a hertz number and the text rules refuse it, so the sender strips the `@ ...` part |
| `cpuThreads` | integer | 1 to 1,024 | no | `12` | |
| `ramMb` | integer | 0 to 4,194,304 | no | `32768` | |
| `gpu` | string | 1 to 64, section 5 | no | `NVIDIA GeForce GTX 1060` | the graphics adapter's model name |
| `gpuDriver` | string | `^[A-Za-z0-9._+ -]{1,48}$` | no | `552.44` | its driver version |
| `usbHosts` | array | at most 8 strings, 1 to 64, section 5 | no | `["Intel USB 3.1 eXtensible Host Controller"]` | the USB host controllers' product names |
| `audioApi` | string | `mme dsound wasapi wdmks asio alsa jack oss coreaudio none other` | yes | `wasapi` | the audio API in use |
| `audioClass` | string | `speakers headphones usb bluetooth hdmi virtual other none` | no | `speakers` | the class of the output device; never its name (a person's headphones are named after them) |
| `displayCount` | integer | 0 to 16 | no | `2` | |
| `displayScalePct` | array | at most 8 integers, 25 to 1,000 | no | `[100,150]` | each display's scaling |
| `diagOn` | boolean | | yes | `true` | whether the Diagnostics switch was on |

### 3.9 Sending: the on-disk queue, the batches, the daily bound

* THE QUEUE (C2). Events are APPENDED to a file on disk as they happen, flushed (to the operating system and to the disk) before any call
  that could crash or hang, and sent from there; whatever was not sent is sent at the next start. It is a folder `radio-reports` BESIDE
  `config.json` (not the crash folder, which is empty when Diagnostics is off). A `radio_fail` is its own file (it may need the degraded
  resend of 2.2); journal events are lines of an append-only segment file per session (`journal-<sessionId>.jsonl`, each line a complete
  message with its `seq`, rotated at 256 KiB) with a cursor file of how many lines have been taken (`.pos`, written after each accepted
  send). The whole folder is bounded at 200 failure files, 4 MiB and 14 days; over a bound the OLDEST journal lines are dropped and counted
  in the next message's `dropped` (nothing is dropped silently).
* BATCHES. A batch is a run of requests, one message per request, in `seq` order, sent: at the start of the session (what the previous
  session left, then this session's `session_env` and first events), about every 5 minutes while the session runs, at once when a
  `radio_fail`, a `radio_close` with a cause other than `user`/`switch-radio`/`quit`, or a `radio_recovery` `gave-up` is written, and at
  quit (a bounded attempt: quit is never delayed by a stuck send). A batch is at most 50 requests and stops at the first 429, 5xx or
  network error (the rest stay queued). A `radio_batch` envelope that would carry many messages in one request is RESERVED, not
  defined: the Worker and the site cope with one message per request at the volumes in the record; the decision is the owner's if the
  request count becomes a cost.
* ORDER AND REPEATS. The site's idempotence is `(installId, sessionId, seq)` (or `messageId`): a message sent twice because an answer
  was lost is stored once.
* THE DAILY BOUND (C8). Per install per UTC day: 100 `radio_fail` (2.3) and 3,000 journal events (everything else except `session_env`,
  which is once per session and never bounded). `radio_stream` is made small first: at most one per radio per minute, and after a radio
  has sent 500 of them in a day the window widens to 5 minutes (`windowSec` 300), which loses nothing because each event states the span
  it covers. At the 3,000 bound: the FIRST event of each (`radioKey`, `kind`, phase or cause word) of the day is always sent; later ones of
  the same triple are not sent but counted; the NEWEST is kept as one pending message per triple, replaced by each later event, and sent
  when the day rolls over, at the next start or at quit, with `dropped` set on it to the number of events of that radio key not sent since
  its previous sent event. So the first and the last event of a failure loop are always kept, and the number lost is always said and shown.
* DIAGNOSTICS and USAGE. The journal is not gated by Diagnostics. It is gated by Usage reporting (off: nothing here is sent or queued)
  and by an endpoint being configured.

### 3.10 The running-now beat's extension

Today the beat is `{ "id", "v", "beat": 1 }` every five minutes, by design carrying only the install id and the version, and the Worker
writes `foxsdr_heartbeat` (index = id, blob1 = version). To show a radio as ACTIVE NOW the site needs to know which radios are open now.
The beat gains ONE optional field:

| Field | Type | Limit | Required | Example | Meaning |
|---|---|---|---|---|---|
| `radios` | array | 1 to 8 radio keys, each `^[a-z0-9-]{0,40}:(-|[A-Za-z0-9._-]{1,40})$` | no | `["airspy-hf:HF0001A2"]` | the keys (1.1) of the radios open right now. Omitted, not empty, when none is open |

Why that is the minimum: the site already knows each radio's model, serial and history from the journal; it only lacks "open at this
moment", and a key is the one identifier that matches a beat to a radio. No model text, no serial field, no settings, no session id, no
frequency, no count: the key is made of two things the contract already stores. The Worker validates it (a malformed list refuses the whole
beat: `refused: bad-field`, so an application bug is seen, and nothing is written); it still writes ONLY `foxsdr_heartbeat` (id, version:
no radio key reaches Analytics Engine); and, when a site is configured, it forwards `{ "schema": 2, "kind": "radio_beat", "installId",
"v", "radios" }` (duplicates removed) to the site, best effort and without a retry (the next beat is five minutes away). The site
keeps beats in memory only, never on disk. A beat from an install with Usage reporting off does not exist (no id).

### 3.11 The usage record's `window` word

Site lane 2's view (f) asked that the usage record say which window the session used. The usage record gains one optional field,
`window`: `new` or `legacy`; left out when the app cannot say. The Worker stores it as `blob16` of `foxsdr_usage`, ONLY when it is one of
those two words, and appends nothing otherwise, so a record without it keeps its 15 blobs. A reader asks for `blob16 = 'new'` or `'legacy'`
and treats every other row as "did not say". `first_session` records do not carry it (their columns are section 7's); `session_env` does.

## 4. Forbidden fields

Every object key in a message, at any depth, is lower-cased and tested against this list; a match REFUSES the message
(`forbidden-field`) whatever the value:

`freq` `hertz` `hz` (as a suffix) `centre` `center` `tuned` `bookmark` `position` `latitude` `longitude` `gps` `ipaddr`
`hostname` `username` `filename` `folder` `decoded` `callsign` `password` `secret`, and the whole keys `lat` `lon` `lng`
`ip` `user` `path` `file` `token`.

(The regular expression, which both sides use verbatim:
`freq|hertz|hz$|centre|center|tuned|bookmark|position|latitude|longitude|gps|ipaddr|hostname|username|filename|folder|decoded|callsign|password|secret|^(lat|lon|lng|ip|user|path|file|token)$`.)

An unknown key that is not forbidden is DROPPED from the stored form and the message is still accepted, so an app one
version ahead loses nothing but the field the Worker has not learned.

## 5. Text rules (the scrub, enforced again on arrival)

Applied to every field a table marks "section 5": `reason`, every `logLines` entry, `model`, `antenna`, `usbHost`, `cpu`, `gpu`, `usbHosts`
entries, and for `first_session` the `sdr` field. A string that fails is a refusal of the whole message, with the code in brackets. The
checks, in this order:

1. (`path-in-text`) a drive path: a letter, `:`, then `\` or `/`, where the letter is not preceded by a letter, digit or
   underscore (so `tcp://` is not one); a network path `\\host\share`; or `\Users\`, `/Users/`, `/home/` followed by anything
   other than `<user>`, case-insensitive, either slash.
2. (`address-in-text`) an IPv4 literal (four dot-separated numbers 0 to 255, not preceded by a letter, digit, underscore or
   dot, not followed by a letter, digit or underscore, nor by a dot and a digit), EXCEPT `127.x.x.x` and `0.0.0.0`, and
   except one that follows the word `version`, `firmware`, `build` or `package` (a four-part version is not an address).
3. (`serial-in-text`) the body's own non-empty `serial`, if 4 or more characters, appearing anywhere, case-insensitive; or
   one of the words `serial`, `serial_number`, `s/n`, `sn`, `_sn` (as a whole word, `_sn` as the end of a word such as
   `airspyhf_sn`), optionally followed by `:` or `=` and spaces, whose following value (up to a space, comma, semicolon,
   closing bracket or quote, after the word `number`, `no`, `no.` or `#` if one follows the key, and without trailing `.` `!` `?` `:`) is
   present and is not exactly `<stripped>`; the values `port`, `ports`, `bus`, `interface`, `cable`, `adapter`, `line`, `lines`, `connection` and
   `communication` are not serial numbers ("serial port COM#"). The reference is `serialInText` in `telemetry-worker/radio-reports.js`, and
   `test-fixtures/radio-text-rules.json` is the parity table every implementation is held to.
4. (`frequency-in-text`) a number with a hertz unit: digits (with `.` or `,`), optional space, then `Hz`, `kHz`, `MHz` or
   `GHz` as a whole word, case-insensitive. The app's scrub turns these into `#`. A processor's clock speed is such a number: `cpu` is sent
   without it.

Lengths: `reason` 400, a log line 240, anything clipped is a refusal, not a silent cut.

## 6. Refusal codes

Stable machine strings, in the order the validator checks: `too-large`, `bad-json`, `unknown-kind`, `reserved-kind`,
`bad-schema`, `forbidden-field`, `bad-install-id`, `missing-field`, `bad-field`, `key-mismatch`, `path-in-text`,
`address-in-text`, `serial-in-text`, `frequency-in-text`, `not-first-session`. The fixtures name the code each refusal
must give; Worker and site return it as `refused: <code>`. The Worker alone adds `site-refused` (the two validators disagree: a bug).

## 7. `first_session`: the record of a session, sent during the session (L7)

The usage record is sent at the NEXT start, so someone who runs FoxSDR once and leaves is never seen. This message is the
same anonymous record plus the health tokens, sent from inside the first session. (Unchanged from schema 1 except the schema number.)

When: only in the install's FIRST session (`launches` is 1), about 60 seconds after the window is up (`trigger`
`after-minute`), and again at once when a radio fails to open or anything is filed as an issue (`trigger` `on-failure`),
at most once a minute and at most 6 times in the session. Each message carries the health tokens counted SO FAR, so a later
one supersedes an earlier one. An install with Usage reporting off sends none.

It is NOT a usage report and the Worker never writes it where a usage row goes:

* One usage row is one launch, to every reader (`usage.ps1`, the widget, the site's reliability and health pages). A
  `first_session` message is written to its OWN dataset, `foxsdr_firstsession` (binding `FIRSTSESSION`), never to
  `foxsdr_usage`. A test proves it, with a break-it.
* It does not take the health ledger, the stall ledger or the unclean-exit counters: the usage record sent at the next start
  still carries all of them. A reader MUST NOT add the health tokens of a `first_session` row to those of a usage row; they
  describe the same session. Read per (`installId`, `sessionId`) taking the row with the highest `seq`.
* It uses `installId`, never `id`. A Worker that does not know this contract (the one deployed today) answers a body
  without `id` with 400 `bad id` and writes NOTHING, so a first-session record can never become an extra launch on an old
  Worker; the 0.99.95 Worker in `test-fixtures/` is the test of that.

Body fields (the names are the usage record's own): `v` `os` `arch` `launches` `crashes` `sessionSec` `sdr` `modes`
`panels` `plugins` `ch` `first` `fv` `uch` `stalls` `health` `exits_died` `exits_killed` `exits_ended` `exits_unknown`
with exactly the usage record's meaning, limits and "not reported" rules (the Worker uses the same functions), plus:

| Field | Type | Limit | Required | Meaning |
|---|---|---|---|---|
| `trigger` | string | `after-minute` or `on-failure` | yes | why it was sent now |
| `launches` | integer | exactly `1` | yes | the install's first session; anything else is refused (`not-first-session`) |
| `v`, `os`, `arch` | string | as the usage record | yes | |

Maximum body 8,192 bytes. The envelope's `radioKey` is `""` or absent. Response codes as 2.2; the Worker answers 503 when
the dataset binding is missing, so the app retries instead of being told 204 for nothing.

Stored columns (`foxsdr_firstsession`): `index1` = `installId`; `blob1`..`blob15` and `double1`..`double16` exactly as
`foxsdr_usage`, so a reader's SQL works against either; `blob16` = `trigger`, `blob17` = `sessionId`, `double17` = `seq`,
`double18` = `t`. The fixtures pin one row.

## 8. Where each message goes

```
app --(radio_fail, journal kinds, session_env)--> Worker: validate (this file) --forward--> site /api/radiofail: validate again, store
app --(beat with radios)--> Worker: heartbeat row as ever; radio_beat forwarded to the site (memory only, best effort)
app --(first_session)--> Worker: validate (this file) --> Analytics Engine dataset foxsdr_firstsession
```

* The Worker adds nothing about the sender and reads nothing derived from the connection. It forwards the CANONICAL
  message (unknown keys dropped), with `x-foxsdr-relay: <secret>` when the relay secret is set. The site skips only its
  per-address limiter for a request carrying the right secret (every Worker request comes from Cloudflare's addresses); the
  per-install, per-radio and total limits and the whole validation still apply. The site also accepts the same body directly,
  validated the same way, like `/api/crash`.
* The Worker never sees the Diagnostics switch and nothing in it depends on it.
* The Worker writes ONE counter row per `radio_fail` to `foxsdr_radiofail` (`index1` installId, or the word `anonymous` for the form of
  1.2, which a reader counting installs must leave out; blobs `driver`, `reasonClass`, `v`, `forwarded` = `ok` or `fail`; doubles 1): no
  serial, no model, no text, so a count exists even when the site is down, and the site's stored total can be compared against it.
  The journal kinds write no Analytics Engine row.
* The site keeps EVERY accepted body, forever (owner decision). The per-signature caps of the crash store (3 bodies, 100 for a named
  issue) do not apply. It keeps the recent months in its own store and moves older ones, compressed, to an archive directory named by
  one configuration value (decision 13: in production a mount of the NAS); both are read by the pages.
* Erasure. By install id: every message of that install in the store and the archive, and the radios only that install reported. By radio
  key: every message about that radio, from every install and the anonymous ones (the only way to remove an anonymous report).

## 9. Fixture files

`telemetry-worker/test-fixtures/radio-reports/*.json`, one case per file, written by `telemetry-worker/make-radio-fixtures.mjs` (every
accepted case's `canonical` and `row` is written there by hand from this file, never by the validator):

```
{ "name": "...", "about": "...", "expect": "accept" | "refuse",
  "code": "<refusal code>",            // refuse only
  "status": 204 | 400 | 413,
  "request": { ... },                  // the JSON body POSTed to the Worker (a legacy beat for the beat cases)
  "expand": { "path": "body.reason", "length": 20000 },   // optional: grow a string to this many characters first
  "canonical": { ... },                // accept: what is forwarded/stored when it is not the request itself (the beat cases: the radio_beat the site is sent)
  "row": { "indexes": [...], "blobs": [...], "doubles": [...] }   // accept, first_session: the dataset row
}
```

Names starting `valid-` must be accepted, names starting `refuse-` must be refused with exactly `code`. `MANIFEST.sha256`
beside them lists each file's SHA-256 (of the file with CRLF turned into LF, so an autocrlf checkout agrees); the site keeps a copy of the
directory (and of `radio-text-rules.json` and `radio-journal-spec.json`) and its test fails when the copy and the manifest differ.

## 10. What an install with Usage reporting OFF still sends

One list, because it is what the privacy page has to say. With Usage reporting switched off the journal, the environment record, the
first-session record, the running-now beat and the usage record are NOT sent. What IS still sent, always (decision 12), is a radio failure
report, and only when a radio does not open, containing: the driver word, the radio's model and its SERIAL NUMBER, its firmware and library
version where known, the driver's reason for failing with serial numbers, user names, host names, quoted names and frequencies removed and
every file path replaced by `<path>`, a class word for the reason, which call site opened it, how many times this radio has failed in this
run, how long the open took and the sample rate asked for, whether the user had already moved on, how many failures were not sent because of
the daily limit, whether Diagnostics was on, up to 40 radio-related lines of the log (scrubbed the same way), the FoxSDR version, the install
channel and the update channel, the operating system and build, the processor architecture, the seconds since FoxSDR started and a random
number made for that one message. It contains NO install identifier, session identifier or sequence number, so two such reports cannot be
tied together except by the radio's own model and serial; as with every report the network necessarily shows the sender's address to the
Worker, which does not read, log or store it. Never: a tuned or centre frequency, a bookmark, a file or recording name, anything decoded, a
position, an IP address or host name, a user or computer name, a path. Such a report is kept indefinitely and cannot be erased by install id
(it has none); it is erased by quoting the radio's serial.

## 11. Deploy order, and what the Worker deployed today answers

NOTHING here is deployed. The order is: the Worker (`wrangler deploy`, after `wrangler secret put RELAY_TOKEN`), then the site, then the app
that sends these. The Worker deployed today (test-fixtures `worker-0.99.95.js` stands for it; the real one is not read) answers:

| Message | Answer today | Writes |
|---|---|---|
| any message with `kind` and no `id` (every kind here) | `400 bad id` | nothing |
| a beat with `radios` | `204` | the beat row, id and version only: the extra field is ignored |
| a usage record with `window` | `204` | the usage row as ever; the field is ignored |
| a `radio_fail` without `installId` | `400 bad id` | nothing |

So an app released BEFORE the Worker loses every radio report with a visible 400 that the sender keeps and retries on a 5xx but deletes on a
400: the sender must treat `bad id` (the old Worker's literal text, not `refused: ...`) as "the Worker is not ready": keep and retry with
the back-off of 2.2, never delete. Once the new Worker is live and the site is not, the Worker answers 502 and the app keeps and retries.

## 12. What the app lane builds from this

See `docs/multi-radio-records/radio-reports-2.md`, "APP HAND-OFF" (and `radio-reports-1.md` section 5 for the failure call sites).
