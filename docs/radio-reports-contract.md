# Radio reports: the contract (schema 1)

Status: DRAFT 1, written 2026-11-16 on `feat/radio-reports`. Nothing in it is deployed. The app, the Worker
(`telemetry-worker/worker.js`) and the site (`foxsdr-site`, `radiofail.go`) are all held to THIS file and to the
fixture files in `telemetry-worker/test-fixtures/radio-reports/`, which each side's tests load. If a rule here and a
fixture disagree, the fixture is wrong; if a side and a fixture disagree, the side is wrong.

Owner rules this implements (HANDOVER-FABLE-2026-11-16.md): D4 ("I want to know every fail of every radio, not an
accumulation", sent whether or not the Diagnostics switch is on), D5 (the radio's serial is its own field and the key
of the radio rows; C8: a daily bound must never drop silently), L7 (one-time users are seen during their first
session).

## 1. The two messages and the shared envelope

Both messages are `POST`ed as `application/json` to the usage endpoint the app already knows
(`telemetryEndpoint()`, `https://telemetry.foxsdr.com/`). The Worker tells them from a usage record and from a beat by
the `kind` field. A usage record has no `kind`; a beat has `beat: 1`; neither is changed.

The ENVELOPE (every message of this family, today and for the D5 radio events):

| Field | Type | Limit | Required | Meaning |
|---|---|---|---|---|
| `schema` | integer | exactly `1` | yes | contract version |
| `kind` | string | one of the kinds below | yes | which message |
| `installId` | string | `^[0-9a-f]{32}$` | yes | the install id the usage report uses (`newInstallId()`). NOT called `id`: see section 6, "the old Worker" |
| `sessionId` | string | `^[0-9a-f]{16}$` | yes | random per process start; not derived from anything |
| `seq` | integer | 0 to 1,000,000 | yes | per session, counts every message of this family the process made, in order, starting at 0; a gap says a message was dropped or lost |
| `t` | integer | 0 to 2,592,000 | yes | whole seconds since the session started (never a clock time) |
| `radioKey` | string | see 1.1 | yes for radio kinds, `""` or absent for `first_session` | the physical radio the message is about |
| `body` | object | per kind | yes | the kind's own fields |

Kinds defined now: `radio_fail`, `first_session`.

Kinds RESERVED for D5 (the radio journal: found, open, settings applied, stream health, recovery, close): `radio_found`,
`radio_open`, `radio_settings`, `radio_stream`, `radio_recovery`, `radio_close`. They will use this envelope, the radio
key (1.1), the text rules (section 4) and the forbidden-field rule (section 3). Until each has its own section here the
Worker refuses it with `reserved-kind`, which is different from `unknown-kind` so that an app one release ahead of the
Worker is told apart from a hostile one.

### 1.1 The radio key

`radioKey = <modelSlug> ":" <serialKey>`

* `modelSlug`: the body's `model`, lower-cased, every run of characters outside `a-z0-9` replaced by one `-`, leading and
  trailing `-` removed, cut to 40 characters.
* `serialKey`: the body's `serial` upper-cased, or `-` when the serial is empty.

Example: model `uhd b200`, serial `EDR04ZDB2` gives `uhd-b200:EDR04ZDB2`. A radio whose serial the app cannot read gives
`rtl-sdr:-`. The sender computes it; the Worker and the site COMPUTE IT AGAIN from `model` and `serial` and refuse a
message whose `radioKey` differs (`key-mismatch`). The key is how one physical radio seen from two installs becomes one
radio on the site; it is deliberately not derived from the driver word, because the same dongle can be opened by two
drivers.

## 2. `radio_fail`: one radio that did not open

ONE message per failed open, every time: the start-up restore, a radio the user picked, the saved-radio retry, a patch
page radio, the automatic reopen after a driver fault, the sound-card input. Sent whether or not the Diagnostics switch
is on (owner answer 2026-11-15, "Send them always"). Repeats are NOT folded by the sender: a busy-again retry is a new
message with a higher `attempt`. The only thing that ever stops a message is the daily bound (2.3), and that is counted.

### 2.1 Body fields

| Field | Type | Limit | Required | Example | Meaning |
|---|---|---|---|---|---|
| `driver` | string | one of the 14 driver words (`rtlsdr hackrf airspy airspyhf sdrplay mirisdr rx888 pluto aor hydrasdr rtltcp soapy soundcard other`) | yes | `airspyhf` | `health::driverWord(kind)` |
| `model` | string | 1 to 32 chars, unchanged by `scrubModel` (no token holding `=` `:` `/` `\` `@`, no dotted quad) | yes | `Airspy HF+` | the model as the usage report sends it (`sanitiseDevice` / `nativeFamilyModel`, then `scrubModel`) |
| `serial` | string | `""` or `^[A-Za-z0-9._-]{1,40}$` | yes (may be empty) | `HF0001A2` | the radio's hardware serial, AS ITS OWN FIELD: the owner decided it is stored (D5 correction 2026-11-15). Empty when the driver cannot read one (rtl_tcp, Pluto by address, sound card, a driver that fails before it can). NEVER an address or a device argument string |
| `firmware` | string | `""` or `^[A-Za-z0-9._+ -]{1,48}$` | no | `3.1.0` | radio firmware version where known |
| `library` | string | same | no | `libairspyhf 1.6.8` | driver or library version where known |
| `reason` | string | 1 to 400 chars, section 4 | yes | `the radio stopped answering while restoring the crystal trim: a control transfer failed (Windows error 31, serial <stripped>)` | the FULL reason sentence the driver gave, put through `scrubUploadLine` first (serials, user names in paths, hosts, quoted names and frequencies removed from the TEXT), and THEN every path (a drive path, a `\\host\share`, anything under `\Users\`, `/Users/` or `/home/`) replaced by `<path>`: `scrubUploadLine` keeps the rest of a path, the Worker and the site refuse any path (section 4), so a path must not reach them |
| `reasonClass` | string | `busy driver bind absent timeout vendor rate other` | yes | `other` | `health::radioReasonWord(classifyRadioOpen(kind, reason))`, computed on the unscrubbed text |
| `trigger` | string | `startup-restore user-pick saved-retry patch-radio auto-reopen other` | yes | `startup-restore` | which call site |
| `attempt` | integer | 1 to 10,000 | yes | `1` | ordinal of the failed attempts of THIS radio key in THIS session (1 for the first) |
| `openMs` | integer | 0 to 3,600,000 | no | `5230` | how long the open took before it failed |
| `sampleRateSps` | integer | 0 to 1,000,000,000 | no | `768000` | the sample rate asked for, in samples per second. A sample rate, never a tuned or centre frequency |
| `superseded` | boolean | | no | `false` | true when the user had already moved on to another source when the result arrived (the engine returns early for these; the report is still filed) |
| `dropped` | integer | 0 to 1,000,000 | no | `0` | section 2.3: failures of this radio key since the previous SENT report of this key that were not sent because of the daily bound |
| `diagOn` | boolean | | yes | `false` | whether the Diagnostics switch was on. It does not change what is sent; it tells the owner which kind of install this was |
| `logLines` | array of strings | at most 40 lines, each 1 to 240 chars, section 4 | yes (may be empty) | `["12:01:07.331 soapy: airspyhf open failed ..."]` | the radio lines of the log ring, newest last, each put through `scrubUploadLine`. A radio line is one whose first word (after an optional `HH:MM:SS.mmm ` stamp) is `source:` `soapy:` `vendor:` `radio:` `rtltcp:` `aor:` `rx888:` `pluto:` `mirisdr:` `hackrf:` `airspy:` `airspyhf:` `rtlsdr:` `sdrplay:` `hydrasdr:` `usb:` or `patch:`; any other line is refused (`bad-field`). The app lane confirms this list against the log's real prefixes |
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
| 400 | `refused: <code>` | this message can never be accepted (section 5) | if the code is `path-in-text`, `address-in-text`, `serial-in-text` or `frequency-in-text`: replace `reason` with `withheld: <code>` and `logLines` with `[]` and send it ONCE more (the failure itself, driver, model, serial and class, must still reach the owner; the fixture `valid-radio-fail-withheld` is that form). Any other code, or a refusal of the degraded form: delete it, count it locally, never retry |
| 413 | `refused: too-large` | over 16,384 bytes | as 400 |
| 429 | `retry-after` seconds | the site's per-install or total limit | keep, retry after the delay |
| 502, 503 | text | the Worker could not reach the site, or has no site configured | keep, retry with back-off |
| network error | | | keep, retry with back-off |

Back-off: 1 minute, 5 minutes, 1 hour, then every 6 hours; a queued message is dropped (and counted) after 14 days.

### 2.3 Sender rules

1. Queue first, send second. The message is written to a file in a `radio-reports` folder beside the log before any
   network call, so a crash or a quit does not lose it; whatever is left is sent at the next start (C2). The queue holds at
   most 200 messages and 2 MiB; over that the oldest are dropped and counted in the next message's `dropped`.
2. Not gated by Diagnostics. Gated by: the install having an install id (the Usage reporting switch is on) and an endpoint
   being configured. OWNER DECISION O1 (see the record): whether a install with Usage reporting OFF also sends. The schema
   already has the answer to either choice, except that `installId` is required; sending without one is a schema 2.
3. THE DAILY BOUND. At most 100 `radio_fail` messages are sent per install per UTC day. At the bound: (a) the FIRST failure
   of every distinct (`radioKey`, `reasonClass`) of the day is always sent, bound or not; (b) later failures of the same
   pair are not sent but are counted, and the NEWEST one is kept as one pending message for that pair, replaced by each
   later failure; (c) the pending message is sent when the day rolls over, at the next start, or at quit, with `dropped` set
   to the number of failures of that radio key that were not sent since its previous sent report, so the last failure of a
   loop is always kept and the number lost is always said. Nothing is dropped without being counted; the site shows
   "N reports dropped by the daily bound" per radio from the `dropped` values (C8).
4. The bound does not use `core::crash_upload`'s 24-hour signature dedup, its 5-a-day window, its persisted
   `UploadPolicyState` or the issue report's once-per-word rule. Those stay as they are for every other report.

### 2.4 NEVER in it

A tuned or centre frequency, a bookmark, a band name (v1 has no field for one), a file or recording name, anything
decoded, a position, an IP address or host name, a user or computer name, a path, the contents of the config. The text
rules (section 4) and the forbidden-field rule (section 3) are the enforcement, on the Worker and on the site; the
sender's own scrubbing is the first line, not the only one.

## 3. Forbidden fields

Every object key in a message, at any depth, is lower-cased and tested against this list; a match REFUSES the message
(`forbidden-field`) whatever the value:

`freq` `hertz` `hz` (as a suffix) `centre` `center` `tuned` `bookmark` `position` `latitude` `longitude` `gps` `ipaddr`
`hostname` `username` `filename` `folder` `decoded` `callsign` `password` `secret`, and the whole keys `lat` `lon` `lng`
`ip` `user` `path` `file` `token`.

(The regular expression, which both sides use verbatim:
`freq|hertz|hz$|centre|center|tuned|bookmark|position|latitude|longitude|gps|ipaddr|hostname|username|filename|folder|decoded|callsign|password|secret|^(lat|lon|lng|ip|user|path|file|token)$`.)

An unknown key that is not forbidden is DROPPED from the stored form and the message is still accepted, so an app one
version ahead loses nothing but the field the Worker has not learned.

## 4. Text rules (the scrub, enforced again on arrival)

Applied to `reason`, every `logLines` entry, `model`, and for `first_session` the `sdr` field. A string that fails is a
refusal of the whole message, with the code in brackets. The checks, in this order:

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
   `GHz` as a whole word, case-insensitive. The app's scrub turns these into `#`.

Lengths: `reason` 400, a log line 240, anything clipped is a refusal, not a silent cut.

## 5. Refusal codes

Stable machine strings, in the order the validator checks: `too-large`, `bad-json`, `unknown-kind`, `reserved-kind`,
`bad-schema`, `forbidden-field`, `bad-install-id`, `missing-field`, `bad-field`, `key-mismatch`, `path-in-text`,
`address-in-text`, `serial-in-text`, `frequency-in-text`, `not-first-session`. The fixtures name the code each refusal
must give; Worker and site return it as `refused: <code>`.

## 6. `first_session`: the record of a session, sent during the session (L7)

The usage record is sent at the NEXT start, so someone who runs FoxSDR once and leaves is never seen. This message is the
same anonymous record plus the health tokens, sent from inside the first session.

When: only in the install's FIRST session (`launches` is 1), about 60 seconds after the window is up (`trigger`
`after-minute`), and again at once when a radio fails to open or anything is filed as an issue (`trigger` `on-failure`),
at most once a minute and at most 6 times in the session. Each message carries the health tokens counted SO FAR, so a later
one supersedes an earlier one.

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

## 7. Where each message goes

```
app --(radio_fail)-----> Worker: validate (this file) --forward--> site /api/radiofail: validate again, store
app --(first_session)--> Worker: validate (this file) --> Analytics Engine dataset foxsdr_firstsession
```

* The Worker adds nothing about the sender and reads nothing derived from the connection. It forwards the CANONICAL
  message (unknown keys dropped), with `x-foxsdr-relay: <secret>` when the relay secret is set. The site skips only its
  per-address limiter for a request carrying the right secret (every Worker request comes from Cloudflare's addresses); the
  per-install and total limits and the whole validation still apply. The site also accepts the same body directly,
  validated the same way, like `/api/crash`.
* The Worker never sees the Diagnostics switch and nothing in it depends on it. The only switch that matters at the Worker
  is that an install with Usage reporting off has no install id and so cannot send either message (O1).
* The Worker writes ONE counter row per `radio_fail` to `foxsdr_radiofail` (`index1` installId; blobs `driver`,
  `reasonClass`, `v`, `forwarded` = `ok` or `fail`; doubles 1): no serial, no model, no text, so a count exists even when
  the site is down, and the site's stored total can be compared against it.
* The site keeps EVERY accepted body. The per-signature caps of the crash store (3 bodies, 100 for a named issue) do not
  apply to this kind.

## 8. Fixture files

`telemetry-worker/test-fixtures/radio-reports/*.json`, one case per file:

```
{ "name": "...", "about": "...", "expect": "accept" | "refuse",
  "code": "<refusal code>",            // refuse only
  "status": 204 | 400 | 413,
  "request": { ... },                  // the JSON body POSTed
  "expand": { "path": "body.reason", "length": 20000 },   // optional: grow a string to this many characters first
  "canonical": { ... },                // accept, radio_fail: what is forwarded/stored when it is not the request itself
  "row": { "indexes": [...], "blobs": [...], "doubles": [...] }   // accept, first_session: the dataset row
}
```

Names starting `valid-` must be accepted, names starting `refuse-` must be refused with exactly `code`. `MANIFEST.sha256`
beside them lists each file's SHA-256 (of the file with CRLF turned into LF, so an autocrlf checkout agrees); the site keeps a copy of the directory and its test fails when the copy and the
manifest differ.

## 9. What the app lane builds from this

See `docs/multi-radio-records/radio-reports-1.md`, "APP HAND-OFF".
