# Radio reports 2: the radios page (contract schema 2, the Worker side, the site side) and the hand-off to the app lane

Engine branch `feat/radio-reports` in `D:/foxsdr-wt/radio-reports` (part 1's tip `6ae6072b`, part 2's code `cb725ade`, this record on top).
Site branch `radio-reports-2` in `C:/Users/steve/foxsdr-site`, cut from `site-lane-2` at `1abd9cf` (code `4c7358b`; its record is PART 7 of
`HANDOVER-daylight.md`). Written 2026-11-16. NOTHING IS DEPLOYED, PUSHED OR SENT. No C++ was edited. Every id, serial, model and text in a
fixture is invented. Brief: `D:/foxsdr-build/prep/queued-briefs/wave7/radio-reports-2.md`.

## 1. What was read

`STANDARD-RULES.md`; D5 and "D5 completeness" C1 to C8 of `HANDOVER-FABLE-2026-11-16.md`; `OWNER-ANSWERS-2026-11-16.md` (decisions 12, 13, 14, 15
honoured below; decision 14 and 15 are the app and installer lanes'); part 1's `docs/radio-reports-contract.md`, its record, the Worker
(`worker.js`, `radio-reports.js`, both test files, the 0.99.95 Worker kept as the "deployed today" stand-in); the site's `radiofail*.go`,
`alerts*.go`, `dashboard.go`, `main.go`, PART 5 and PART 6 of `HANDOVER-daylight.md`; the error-path gate's record (read only).

## 2. Schema 2 in three lines, and the correction

1. Part 1's schema 1 (never deployed) becomes schema 2: the six reserved kinds are defined (`radio_found`, `radio_open`, `radio_settings`,
   `radio_stream`, `radio_recovery`, `radio_close`) with a once-a-session `session_env`, every field's type, limit and example in the contract
   and as a data table (`telemetry-worker/radio-journal-spec.js`, JSON copy `test-fixtures/radio-journal-spec.json`) that the Worker and the
   site both read.
2. The envelope gains `age` (seconds a message waited in the on-disk queue, so a flushed queue is not new activity); the running-now beat
   gains optional `radios` (up to eight radio keys); the usage record gains optional `window` (`blob16`, only when `new` or `legacy`).
3. DECISION 12: a `radio_fail` is valid WITHOUT `installId`: then it has no install id, session id or sequence number (a message with
   `sessionId` or `seq` but no `installId` is refused) and carries a random `messageId`; "one usage row is one launch" still holds
   (a `radio_fail` never touches the usage dataset; a first-session record goes to its own).

### What an install with Usage reporting switched OFF still sends (the list the privacy page must say; contract section 10)

Not sent: the journal, the environment record, the first-session record, the beat, the usage record. Still sent, always, only when a radio does
not open: driver, model, SERIAL, firmware and library version, the scrubbed reason (serials, user names, hosts, quoted names, frequencies removed;
paths replaced by `<path>`), a class word, the call site, how many times this radio has failed this run, how long the open took, the sample rate
asked for, whether the user had moved on, how many reports the daily bound kept off the wire, whether Diagnostics was on, up to 40 scrubbed radio
lines of the log, FoxSDR version, install channel, update channel, OS and build, architecture, seconds since start, and one random per-message
number. No install id, session id or sequence number; the network shows the sender's address to the Worker, which neither reads nor stores it.
HONEST CAVEAT: the combination version + OS build + architecture + channel + radio serial can identify an install in practice; nothing here
uses it to link reports, and the site does not group by it.

## 3. The Worker (same branch), and its tests

`radio-reports.js` validates everything from the table; `worker.js` forwards radio failures, journal events and a beat that names radios to
`SITE_INTAKE_URL` (the counter row of part 1 is unchanged except its index is `anonymous` for the no-id form, which a reader counting installs
must leave out; the journal writes no Analytics Engine row; the beat row stays id and version). `make-radio-fixtures.mjs` regenerates the
88 shared fixtures and the manifest.

Real output, `node --test telemetry-worker/worker.test.mjs telemetry-worker/radio-reports.test.mjs` (Node v24, the tree at this record's
commit): see section 9. `worker.test.mjs` is unchanged (67). New in `radio-reports.test.mjs`: every kind accepted from its fixture and forwarded
once; every forbidden name refused at the top and in the body of every kind; every free-text field of every kind refuses a path, an address, its
own serial and a hertz number; the size limits; the anonymous failure (no linking field forwarded, counted `anonymous`, two such reports share only
the radio); the beat (unchanged without radios, keys forwarded with radios, a dead site costs nothing, malformed list refused whole); the window
word; the "deployed today" answers; the contract names every field of the table; five new break-it checks (anonymous linking, journal text rules,
beat keys, window word, size limit) beside part 1's four.

### Deploy order, and what the Worker deployed today answers (contract section 11)

Worker first (after `wrangler secret put RELAY_TOKEN`), then the site (with `FOXSDR_RELAY_TOKEN`, and `FOXSDR_RADIO_ARCHIVE` when the NAS mount
exists), then the app. Today's Worker (the 0.99.95 stand-in) answers every new message `400 bad id` and writes nothing, a beat with `radios`
`204` and writes only id and version, a usage record with `window` `204` and ignores it. So the app must treat the literal body `bad id` as "the
Worker is not ready": keep and retry, never delete.

## 4. The site (details: PART 7 of `HANDOVER-daylight.md`)

Intake for all kinds; store keyed by physical radio; `/admin/radios`, `/admin/radio`, `/admin/installs`, `/admin/install`, `/admin/radios/storage`;
three alerts; the archive to `FOXSDR_RADIO_ARCHIVE`; erase by install id and by radio key; the extended privacy DRAFT.

## 5. Storage figures (measured on the reference sessions; the page shows them)

Typical session (30 minutes, one radio, a stream event a minute, two setting changes, one recovery): 40 events, 21.5 KiB stored, 853 B compressed.
Short (one minute): 7 events, 3.4 KiB, 222 B. Long (eight hours): 494 events, 275.8 KiB, 9.9 KiB.

| Sessions a day | Short a year | Typical a year | Typical, compressed | Long a year |
|---|---|---|---|---|
| 100 | 120.5 MiB | 765.4 MiB | 29.7 MiB | 9.6 GiB |
| 1,000 | 1.2 GiB | 7.5 GiB | 296.9 MiB | 96.0 GiB |
| 10,000 | 11.8 GiB | 74.7 GiB | 2.9 GiB | 960.2 GiB |

The Pi's data directory (current month plus the two before it, 1 GiB ceiling) overflows at 1,000 sessions a day typical: the archive is
needed from there. THE COMPRESSED FIGURES ARE OPTIMISTIC: the reference stream events are alike; real streams vary more. Re-measure on real
traffic (the page does, from five sessions on).

## 6. APP HAND-OFF: exactly what the app lane builds (everything read from the contract; the C++ was not read for this part except where part 1 did)

Part 1's section 5 (the five failure call sites, `radioreport::fileFailure`, the scrub, the first-session sender) stands, with these changes and
additions. Size: 1 new module pair for the journal queue, 8 event points, 1 environment collector, 1 beat field, 1 usage field, about 14 tests.

1. SCHEMA. Everything sends `schema: 2`. Take field names, limits and enums from `radio-journal-spec.json`; load it in the app's test and
   compare every sent message with the shared fixtures (`telemetry-worker/test-fixtures/radio-reports/`, 88 files: build the same message from
   a valid fixture's inputs, and assert the app's scrub never produces a refused one).
2. FAILURE WITHOUT AN INSTALL ID (decision 12). `fileFailure` no longer requires `installId`: with none it omits `installId`, `sessionId`, `seq`
   and sends a fresh random 16-hex `messageId`. Usage reporting off must not stop it. Everything else of part 1's sender stays. Treat the
   literal answer `bad id` as not-ready (keep, retry).
3. THE JOURNAL QUEUE (`src/core/radio_journal_queue.hpp/.cpp`, new). A `radio-reports` folder beside `config.json`: `journal-<sessionId>.jsonl`,
   one complete message (with its `seq` and `t`) per line, appended and flushed (`FlushFileBuffers`) BEFORE any call that can hang or crash, rotated
   at 256 KiB; `.pos` cursor written after each 2xx; folder bound 200 failure files, 4 MiB, 14 days (oldest dropped and counted in the next event's
   `dropped`). A test kills the process mid-session and finds the last events delivered at the next start (C2).
4. THE SENDER. One message per request in `seq` order: at the start (leftovers, then this session's `session_env` and first events), every 5 minutes,
   at once on a `radio_fail`, an unclean `radio_close` or a `radio_recovery` `gave-up`, and at quit (bounded, abortable, never delays quit). Batch
   of at most 50, stop at the first 429, 5xx or network error. `age` is recomputed at every attempt. Back-off 1 min, 5 min, 1 h, then 6 h; 14 days
   then drop and count. A journal event refused for a text-rule code is resent once without the offending text field.
5. THE BOUND (C8), contract 3.9: 100 `radio_fail` and 3,000 journal events per UTC day; stream events at most one per radio per minute, window
   widening to 300 s after 500 in a day; at the bound the first event of each (radioKey, kind, phase or cause word) is always sent, the newest
   later one is held and sent at the day's end, the next start or quit with `dropped` set. Persist the day's counts beside the queue.
6. EVENT POINTS (each emits one message, with the radio key from the REQUESTED device, never the receiver's current one):
   `radio_found` at the start-up enumeration, hot-plug rescans and the first-run radio check (every USB id FoxSDR supports, C3: take them from each
   driver's own match table); `radio_open` start immediately before the driver call and ok after it; `radio_settings` after open applies
   them, on each user or automatic change, and with `refused` when the radio refuses one; `radio_stream` from the stream-health sampler once a
   minute per open radio, plus `note` from the "this sounds wrong" button and the silence, clipping and nothing-decoded detectors (C4);
   `radio_recovery` start, ok, gave-up around every recovery (the application's own `recovered` word); `radio_close` at every close with the
   cause (`died` from the device-dead latch, `unplugged` from a removal, `fault` from a driver fault); `session_env` once per session.
7. `session_env` COLLECTION: OS and build; CPU brand string WITHOUT the `@ x.xxGHz` part (a hertz number is refused); threads; RAM; GPU model and
   driver version; USB host controller product names (never device paths or ids); audio API and output CLASS (never the device name); display
   count and each display's scaling; channels; launches; `window`; Diagnostics.
8. THE BEAT. `{"id","v","beat":1}` gains `"radios":[keys]` of the radios open now (at most eight; omit when none). Worker first.
9. THE USAGE RECORD gains `"window":"new"|"legacy"` (omit when unknown). Worker first; site lane 2's view (f) fills in.
10. SERIAL SOURCE per native driver and the model: as part 1 (UNVERIFIED there). Never pass an address or an argument string through as a serial.
11. THE ERROR-PATH GATE. Each new event kind closes these PENDING groups of `errpath-gate.md` once its sites call the reporter (the mapping is from
    the group names and sizes, NOT from each site: confirm site by site): `radio_fail` and the open-path `setError` in `reports-not-in-function`
    (610, the part that reaches `noteRadioFail`), `G1-device-dead` (26) and `G7-patch-radio` (4) through `radio_fail`/`radio_close`; `radio_found`:
    `S6-scan` (4), `S4-first-run-check` (2); `radio_open`: `S1-startup` (3); `radio_settings`: `G3-control-refused` (138), `G5-rtl-pll` (3), `G6-route-fallback`
    (3); `radio_stream`: `G2-stream-health` (29), `A-audio` (7); `radio_recovery`: `G8-sdrplay-lifecycle` (5), `S8-sdrplay-service` (5), `G9-vendor-fault` (10);
    `radio_close`: `G1-device-dead`; `session_env`: `S2-window` (10), `S1b-headless` (7), part of `G10-gpu-fft` (16). NOT closed by these: `R-record`, `D-gps-serial`,
    `F-files`, `T-tx`, `P-plugin`, `P-store`, `S3-plugin-ui`, `S5-driver-install`, `S9-report-upload`, `PL2-companion`, `O-outputs`, `S7-ui-engine`, `unsure`, `unmatched`.
12. PRIVACY TEXT: PRIVACY.md, `docs/DIAGNOSTICS.md`, the in-app Diagnostics and Usage wording, the Store privacy answers and the release notes from
    the site's draft (`web/privacy.html`, block marked DRAFT) once the owner has read it.
13. TESTS (in addition to part 1 section 5.4): shared fixtures; the queue survives a kill; the bound (first and last kept, `dropped` exact, nothing
    dropped uncounted); usage off sends only failures, without an id; `bad id` is kept and retried; every event point drives one message; session_env
    holds none of the never-collected items (real-log search extended to the queue files).

## 7. Owner decisions still to make

| # | Decision | Recommendation |
|---|---|---|
| R1 | Read and approve the privacy DRAFT (journal on by default, failures always, serial, never-collected list, indefinite, deletion); then translate | read it; nothing goes live without it |
| R2 | Is the combination version + OS + architecture + channel + serial acceptable for the switched-off failure report (section 2 caveat)? | yes, it is the minimum to make the report useful |
| R3 | Daily bounds: 100 failures and 3,000 journal events per install per day; stream events 1 a minute, 5-minute windows after 500 | accept |
| R4 | Where older months go: a NAS mount in `FOXSDR_RADIO_ARCHIVE` (this build) or R2; re-decide once bytes per session are measured on real traffic (section 5) | NAS first, as decided |
| R5 | A batched envelope (`radio_batch`, reserved) if one request per event proves costly | not yet; revisit with real request counts |
| R6 | Alert thresholds: model rule (more failures than opens, at least 5, from at least 3 installs, 7 days, journal installs only); died mid-session once per radio per day | accept |
| R7 | The status rules and 15-minute "active now" (section 8 of the site record lists them) | accept; they are one table in `radiojournal.go` |
| R8 | A way for a user to find their install id and serial in the app, for deletion requests | app lane: show both on the Usage reporting page |
| R9 | Deploy: `wrangler secret put RELAY_TOKEN`, `FOXSDR_RELAY_TOKEN`, `FOXSDR_RADIO_ARCHIVE`, the Worker, the site, then the app | owner's; nothing here deploys |

## 8. UNVERIFIED

Nothing ran against Cloudflare (`wrangler.toml` not parsed by wrangler; no new binding was added). The deployed Worker was NOT read: the 0.99.95 copy
stands for it. The NAS archive was a scratch folder, not a mount (a slow or absent mount is untested: a failed move leaves months where they are).
Start-up reads every line of every file (live and archive) to build the picture: fine at today's volume, not measured at 10,000 sessions a day, where
it needs a checkpoint. The compressed-size figures (section 5) are optimistic. The app side is not built, so no real event has been seen. The status
thresholds are proposals tested against invented sequences, not real radios. Wording of the privacy draft is English only and unread by the owner.
DEVIATION: starting the site binary on 127.0.0.1 to look at the pages made its TLE mirror fetch from celestrak.org once (the site's own start-up job,
not part of this work); a stamp now prevents another for 12 hours, and no other outside request was made.

## 9. Test results

`node --test telemetry-worker/worker.test.mjs telemetry-worker/radio-reports.test.mjs` on the final tree (2026-11-16, Node v24), last lines: `ℹ tests 107`, `ℹ pass 107`, `ℹ fail 0`, `ℹ duration_ms 15851`.

## OPEN ROWS FOR THE NEXT LANE

| Row | Owner / lane | Where |
|---|---|---|
| Build the app side (queue, sender, bound, event points, session_env, beat field, window, anonymous failure) | app lane | section 6 |
| Decide R1 to R9 | owner | section 7 |
| Checkpoint the start-up read of the store if volume reaches thousands of sessions a day | site lane | section 8 |
| Translate the approved privacy text into the 33 catalogues | site lane | site record |
| Real-traffic re-measure of bytes per session; decide NAS or R2 for good | owner, site lane | section 5 |
| `radio_batch` if request count matters | Worker, site | contract 3.9 |
| Wire the model alert's thresholds to settings if the owner wants them changed without a build | site lane | `radiojournal_alert.go` |
