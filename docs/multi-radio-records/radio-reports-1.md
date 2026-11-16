# Radio reports 1: the contract, the Worker side, the site side, and the hand-off to the app lane

Branch `feat/radio-reports` in `D:/foxsdr-wt/radio-reports`, cut from `e0d0c669` (engine/multi-radio). Site branch
`radio-reports` in `C:/Users/steve/foxsdr-site`, cut from `daylight-site` at `cd1cc48`. Written 2026-11-16.
NOTHING IS DEPLOYED, PUSHED OR SENT. No C++ was edited. No live Worker, site, feed or Pi was contacted; every id, serial and
report text in a fixture is invented.

## 1. What I read (and what it told me)

Application, read only:

* `src/engine/engine.cpp` 1560 to 1700 (`Engine::finishDeviceOpen`): a failed open reaches `noteRadioFail` at line 1641, but NOT when
  `retryBusyAgain` is true (a busy-again saved-radio retry adds no count), and the whole function returns early at the top when
  `asyncOpenStillWanted` is false (the user moved on), before anything is counted. The result carries `kind`, `args`, `error`,
  `recovery` (the automatic reopen) and `savedRadioRetry`.
* `src/engine/engine.cpp` about 10171 (`Engine::applyConfig`, the first radio's start-up restore), `src/engine/engine_radio_saved.cpp:212`
  (`Engine::restoreExtraRadioDevice`, radios 2 to 8 at start-up), `src/engine/engine_patch_radios.cpp:329` (patch radios; the generator
  and file nodes are excluded), `src/engine/engine_soundcard.cpp:201` (sound-card input). The handover's `app_window_patch_radios.cpp:461/:531`
  no longer holds a `noteRadioFail`: the code moved to the engine file above.
* `src/core/health_events.cpp:423` (`classifyRadioOpen`: eight classes, `other` as the fallback; it has no rule for "stopped answering ...
  control transfer failed", which is why the HF+ fault read as `other`) and `:1056` (`noteRadioFail`).
* `src/core/issue_report.hpp`: one report per recovered-from WORD per session, filed only when Diagnostics is on, into the crash folder.
* `src/core/crash_upload.hpp`: the 24 h per-signature dedup (`kDedupSeconds`), the 5-a-day window (`kMaxPerWindow`), `UploadPolicyState`
  persisted in the config, and `sweepCrashDir` doing nothing when the Diagnostics switch is off. A radio failure must use none of it.
* `src/core/telemetry.hpp`: the usage record is sent at the NEXT start by design (a network call at exit can hang, a record written at
  exit is lost by a crash); the install id exists only while Usage reporting is on and is deleted when it is turned off; the beat is a
  separate dataset on purpose. `scrubModel`, `sanitiseDevice`, `nativeFamilyModel` are the model rules.
* `src/core/diag_log.hpp`: `scrubUploadLine` (serials, user names in paths, hosts, quoted names, frequencies), `ringSnapshot()`, and
  `diagBaseDir()` (which is empty when the environment has no base: nothing on disk).
* `PRIVACY.md` ("What is never sent": frequencies, file names, decoded content, positions, IP, hardware serials, names and paths) and
  `docs/DIAGNOSTICS.md`.

Worker: `telemetry-worker/worker.js`, `worker.test.mjs` (67 tests), `README.md`, `wrangler.toml`, `test-fixtures/` (the 0.99.63, 0.99.67,
0.99.95 Workers kept byte for byte). The Worker only writes Analytics Engine rows; it does not forward anything today. Crash and issue
reports go from the app straight to the site (`/api/crash`), not through the Worker.

Site: `crash.go` (3 bodies per unknown signature, `crashIssueSamplesPerGroup` 100, limiters, `compactLocked`), `health.go` (the radios alert
`radiosFailingVersions`, the SQL builders), `alerts.go`, `alerts_events.go`, `admin.go`, `dashboard.go` (layout, `s.page`), `main.go`, `guipkg.go`
(a small page with a delete), `web/privacy.html`, and the privacy and i18n tests.

## 2. The contract and why it is the way it is

`docs/radio-reports-contract.md` (copied to the site as `docs/radio-reports-contract.md`), schema 1. The decisions:

* **One envelope for everything**: `schema, kind, installId, sessionId, seq, t, radioKey, body`. The D5 events (`radio_found`, `radio_open`,
  `radio_settings`, `radio_stream`, `radio_recovery`, `radio_close`) are RESERVED names the Worker refuses with `reserved-kind`, so an app one
  release ahead of the Worker is told apart from a hostile one.
* **`installId`, never `id`.** The Worker deployed today reads `body.id`; a message without it is answered 400 "bad id" and writes nothing.
  So a first-session record sent to an OLD Worker can never become a second launch. Proved against the 0.99.95 Worker kept in `test-fixtures/`.
* **`radioKey` = model slug + ":" + upper-cased serial** (`-` when there is none), computed again by the Worker and the site and refused on a
  mismatch. The serial is its own field. The key is not derived from the driver word, because one dongle can be opened by two drivers.
* **Every failure is its own message; repeats are not folded.** A busy-again retry is a new message with a higher `attempt`. The only thing that
  stops a message is the daily bound, and that is never silent: the first failure of each (radio, class) of the day is always sent, the newest
  later one is held and sent at the day's end or the next start, and `dropped` carries how many were not sent (C8).
* **Text rules enforced again on arrival** (path, address, serial, frequency), with codes the fixtures name. The app's `scrubUploadLine`
  keeps the rest of a path, so the contract adds one step to it: every path becomes `<path>`. A refusal on the text rules is not final for the
  failure: the sender resends once with the text withheld, so the radio, its serial and its class still reach the owner
  (`valid-radio-fail-withheld.json`).
* **Unknown keys are dropped, forbidden names refuse.** A newer app loses only the field the Worker has not learned; a frequency field refuses.
* **first_session**: the usage record's own fields and health tokens, sent in the install's FIRST session (about a minute in, and at once on a
  failure, at most 6), `launches` must be 1. It is written to its OWN dataset `foxsdr_firstsession` with the usage record's columns in the same
  positions plus `blob16` trigger, `blob17` session id, `double17` seq, `double18` t. It does not take the health ledger: the usage record at
  the next start still carries everything, so a reader must never add the two (read per install and session, highest `seq`).
* **Where a report from an install with Diagnostics OFF goes**: exactly where one with it on goes. I checked and nothing in the Worker (or in the
  site's intake) assumes the switch: it is a client-side gate only. What the Worker does depend on is the install id, which exists only while
  Usage reporting is on (owner decision O1 below).

## 3. The Worker, and its tests

Changed: `worker.js` (the usage data point is now ONE function used by both the usage record and the first-session record; a dispatch on
`kind`; `handleRadioFamily`), new `radio-reports.js` (the validator, no regular-expression look-around so the site's Go port means the same
thing), `wrangler.toml` (bindings `FIRSTSESSION` = `foxsdr_firstsession`, `RADIOFAIL` = `foxsdr_radiofail`, var `SITE_INTAKE_URL`; the relay
secret is a `wrangler secret`, not in the file), `README.md` (a section), `tests/CMakeLists.txt` (the `telemetry_worker` ctest entry now runs both
test files). `radio_fail` is validated and forwarded to the site with `x-foxsdr-relay` when the secret is set; one counter row (driver, class,
version, whether the site took it; no serial, model or text) goes to `foxsdr_radiofail` so a count survives the site being down.

Fixtures: 45 files in `telemetry-worker/test-fixtures/radio-reports/` (10 valid, 35 refused) plus `MANIFEST.sha256`, and the parity table
`test-fixtures/radio-text-rules.json` (557 strings: 58 handwritten, 499 pooled, 164 refused) which both the Worker and the site's Go port are held to.

Tests, real output of `node --test telemetry-worker/worker.test.mjs telemetry-worker/radio-reports.test.mjs` on 2026-11-16 (Node v24.20.0): `ℹ tests 89`, `ℹ pass 89`, `ℹ fail 0`, `ℹ duration_ms 24751`. Before this change the same command on the old tree gave `ℹ tests 67 / pass 67` (worker.test.mjs alone). The launch-count proof is the test `first-session records never add a launch: three of them leave the usage dataset empty, one usage record adds one row`; its break-it is `BREAK-IT: if a first-session record is written to the usage dataset, the launch-count proof goes red`.
`worker.test.mjs` is unchanged and still 67 of 67. `radio-reports.test.mjs` has 22 tests: every fixture, the manifest, every refusal code
covered, the Worker's vocabulary against the contract's, the launch-count proof, the first-session row layout against a usage row, the 0.99.95
Worker refusing every valid message with `bad id` and counting nothing, forwarding and the relay header, the counter row holding no serial,
the site's answers mapped (429 keeps its delay, 5xx and a dead network are 502, a site refusal is final), 503 when unconfigured, "the
Worker has no Diagnostics switch", usage and beat unchanged (including the 4,096-byte limit), the text rules, the radio key, the parity table,
a 4,000-case fuzz of the validator, and four BREAK-IT checks run on a mutated copy: first-session written to the usage dataset (red: the
launch-count proof and the fixtures), forbidden-field check removed (red), serial rule removed (red), path rule removed (red).

## 4. The site (details are in the site's `HANDOVER-daylight.md`, "Radio failure reports")

Intake `POST /api/radiofail`, store `data/radiofails/YYYY-MM.jsonl` keyed by physical radio, admin `/admin/radiofails` (+ `/admin/radiofail?n=`,
`.json`, and `POST /admin/radiofails/delete` by install id), a per-driver rule beside the all-driver radios alert, a DRAFT privacy section.

## 5. APP HAND-OFF: exactly what the app lane builds

The app lane starts from the contract and the fixtures, not from the handover's prose. Everything below is read from the code at `e0d0c669`;
anything I could not read is marked (UNVERIFIED).

### 5.1 New code (new files, then the wiring)

1. `src/core/radio_report.hpp/.cpp` (namespace `cascade::core::radioreport`):
   * `std::string radioKey(model, serial)`, `modelSlug`: contract 1.1, with the same results as `radioKey()` in `telemetry-worker/radio-reports.js`.
   * `struct RadioFailReport` and `std::string toJson(const RadioFailReport&)`: the canonical message; field names exactly as the contract.
   * `std::string scrubReason(const std::string&)`: `scrubUploadLine`, THEN every drive path, `\\host\share`, `\Users\x`, `/Users/x`, `/home/x`
     replaced by `<path>`, then cut to 400 characters on a character boundary.
   * `std::vector<std::string> radioLines(const std::vector<std::string>& ring)`: only lines whose first word (after an optional `HH:MM:SS.mmm`
     stamp) is one of the contract's radio prefixes, each through `scrubUploadLine` and the `<path>` step, newest 40, each cut to 240.
   * `std::string serialFromArgs(kind, args)`: the radio's serial from the device arguments (soapy `serial=...`, native args per driver) or empty for
     rtl_tcp, Pluto by address and the sound card. (UNVERIFIED: the argument format of each native driver; read each driver's
     `nativeLabelFor` / args builder and test one real args string per driver. NEVER pass an address or an argument string through as a serial.)
   * `model`: `sanitiseDevice(args)` for soapy, `modelFromNativeLabel(nativeLabelFor(kind, args))` otherwise, then `scrubModel`; both exist today and
     already give the model the usage report sends.
2. `src/core/radio_report_queue.hpp/.cpp`: the on-disk queue and the sender rules of contract 2.3.
   * A folder `radio-reports` BESIDE `config.json` (NOT `diagBaseDir()/crashes`, NOT `activeCrashDir()`): those are empty or unwritten when
     Diagnostics is off, which is exactly when this must still work. One file per message, written (and flushed) before any network call.
     At most 200 files and 2 MiB; the oldest dropped and counted in the next message's `dropped`.
   * The daily bound: 100 sent per UTC day, the first of every (radioKey, reasonClass) per day always sent, the newest later one per pair held
     and sent at the day's end, the next start or quit, with `dropped` set. State persisted beside the queue, not in `AppConfig`'s crash policy.
   * `seq` and `sessionId`: a per-process counter shared with the first-session sender (contract section 1).
   * Retry: 1 min, 5 min, 1 h, then every 6 h; 14 days then drop and count. 204 deletes; 400/413 deletes except the text-rule codes
     (`path-in-text`, `address-in-text`, `serial-in-text`, `frequency-in-text`), which resend ONCE with `reason` = `withheld: <code>` and
     `logLines` = `[]`; 429 honours `retry-after`; 502, 503 and a network error keep it.
   * Transport: `postBounded` (crash_upload.hpp) to `telemetryEndpoint()`, on a background thread, abortable (`UploadCancel`), never delaying quit
     (rule 2 of crash_upload.hpp). Plain http refused except loopback, as there. `FOXSDR_TELEMETRY_URL` points the tests at a local stub.
3. `src/core/first_session.hpp/.cpp`: builds the `first_session` message from the same `TelemetryReport` the usage record uses (`toJson` fields,
   `health::sanitise`), with `installId`, `sessionId`, `seq`, `t`, `trigger`, `launches` = 1. Sent about 60 s after the window is up
   (`after-minute`) and at once when a radio failure is filed or an issue is filed (`on-failure`), at most one a minute and 6 a session, ONLY when the
   install's launch count is 1. It must NOT call the ledger-clearing completion that the usage record's acceptance triggers (`HealthLedger`,
   `StallLedger`, exit counters): the usage record at the next start still carries all of it.
4. Where to call the first-session sender from: `AppWindow`'s per-frame poll that already drives `HeartbeatSender` (`src/core/telemetry.hpp`, `HeartbeatSender::poll`).

### 5.2 Call sites (each gets exactly one `radioreport::fileFailure(...)` call, BEFORE any early return or count-skip)

| # | Where (at `e0d0c669`) | trigger | Notes |
|---|---|---|---|
| 1 | `src/engine/engine.cpp:1641`, `Engine::finishDeviceOpen`, the `!r.dev` branch | `auto-reopen` when `r.recovery`; `saved-retry` when `r.savedRadioRetry`; else `user-pick` | File it BEFORE `if (!retryBusyAgain)` so a busy-again retry is filed (the count stays skipped, the report is not). File it also when `asyncOpenStillWanted` is false (the early return at the top of the function): set `superseded` true. `r.kind`, `r.args`, `r.error` are on `DeviceOpenResult` (`receiver.hpp:59`). |
| 2 | `src/engine/engine.cpp` about `10171`, `Engine::applyConfig`, the device branch's `else` | `startup-restore` | `kind`, `args` are locals there; the reason is `rx().sourceError_` BEFORE it is wrapped by `radioNotOpenedSentence`. |
| 3 | `src/engine/engine_radio_saved.cpp:212`, `Engine::restoreExtraRadioDevice` | `startup-restore` | Radios 2 to 8. |
| 4 | `src/engine/engine_patch_radios.cpp:329` | `patch-radio` | Only for `kind != "file" && kind != "siggen"` (already the condition). The line-322 `patchRadioError_[id]` is the reason. The generator failure at about line 522 is not a physical radio: no report. |
| 5 | `src/engine/engine_soundcard.cpp:201` | `startup-restore` when restoring, else `user-pick` | `driver` `soundcard`, `model` "sound card" (never the card's name: it is often a person's), `serial` empty. |

`noteRadioFail` stays exactly as it is (the anonymous count). The handover's `app_window_patch_radios.cpp:461/:531` are stale: that code now
lives in `engine_patch_radios.cpp`.

The attempt counter is per (radio key, session). The model and serial must be those of the radio that failed: take them from `r.kind`/`r.args`
(the requested device), never from the receiver's current device (the generator is what is running after a failure).

### 5.3 The existing caps the radio failure report must bypass, and how

| Cap | Where | How the report avoids it |
|---|---|---|
| Diagnostics switch gates every report | `activeCrashDir()` (`crash_handler.hpp:223`), `sweepCrashDir`'s `params.enabled` (`crash_upload.cpp:1410`), `fileIssueReport` | The queue is its own folder beside `config.json` and its own sender; it never calls those. Gate: install id present (Usage reporting on) and endpoint configured. |
| Once per word per session | `issue_report.hpp:25` | The failure does not go through `fileIssueReport` at all and is not a `Recovered` word. |
| 24 h per-signature dedup | `crash_upload.hpp:266` `kDedupSeconds`, `decideUpload` | Not used: no signature, no `UploadPolicyState`. Two failures of one radio are two messages (the site de-duplicates only an identical install, session and `seq`). |
| 5 reports per 24 h | `crash_upload.hpp:268` `kMaxPerWindow` | Its own bound: 100 per UTC day with the first-per-(radio, class) exemption and the `dropped` count. |
| `retryBusyAgain` skips the anonymous count | `engine.cpp:1641` | The count is untouched; the report is filed before the condition. |
| `asyncOpenStillWanted` early return | `engine.cpp` top of `finishDeviceOpen` | File with `superseded = true` before returning. |
| Site: 3 bodies per unknown signature, 100 per known issue | `crash.go:187/:195` | `/api/radiofail` is a different intake that keeps every body. |
| Site per-address limiter | `crash.go` `allowCrashAddr` | Skipped only for the Worker's relay secret; per-install and total limits still apply (10 burst, then one per 20 s per install). |

### 5.4 Tests the app lane must have (the handover's D4 list, plus what the contract adds)

1. One report per failure: two failures, two queued files; the serial scrubbed out of the reason text; the 24 h dedup not swallowing the second; each
   with a break-it (delete the behaviour, see red).
2. Every call site in 5.2 files a report (drive each with a fake driver that fails), including a busy-again saved-radio retry and the superseded
   open.
3. Sent with Diagnostics OFF (the queue is written and the stub receives it); not sent when there is no install id.
4. The shared fixtures: load every file in `telemetry-worker/test-fixtures/radio-reports/` and `radio-text-rules.json`: for each `valid-radio-fail-*`,
   build the same message from its inputs and compare field for field; for each `refuse-*` that the app could produce by mistake (a path, an
   address, a serial or a hertz number in the text), assert the app's own scrub never produces it from a realistic driver sentence (the D3 sentence
   included). The first-session fixtures pin the field set.
5. The text pipeline on the real D3 sentence "the radio stopped answering while restoring the crystal trim: a control transfer failed (Windows
   error 31, serial airspyhf_sn:...)": the sent `reason` holds no serial, and `classifyRadioOpen` says `other` today (L2: add the class).
6. The daily bound: 100 sent, the 101st of a pair held, the first of a new pair sent, `dropped` correct on the held one at the day change; nothing
   dropped without being counted.
7. The queue: written before the network call; survives a kill (the C2 rule); 200 files / 2 MiB bound; retry schedule; a 400 with a text-rule code
   resends once degraded; any other 400 deletes; 429 honours `retry-after`; 502 keeps it.
8. Quit is not delayed by a stuck send (`UploadCancel`); no send from inside a fault handler.
9. first_session: sent once about a minute in and again on a failure; never when `launches` is not 1; not more than 6; does not clear the health,
   stall or exit ledgers; the usage record at the next start still carries them. A test that runs one session and checks the usage dataset stand-in
   got exactly the usage record and the first-session stand-in got the first-session records.
10. A source scan: `src/core/radio_report*.cpp` and its callers never write a frequency, a path or an address into the message; the real-log search of
    `tests/test_diagnostics.cpp` extended to the queued files.
11. PRIVACY.md, `docs/DIAGNOSTICS.md` and the in-app Diagnostics wording say it before the release ships (draft text is in section 6 of the site's
    `HANDOVER-daylight.md`; the site page already carries the same draft).
12. Deploy order: the Worker (this branch) and the site first, then the app. The release preflight should ask for it like `-WorkerDeployed`.

## 6. Owner decisions (ask; do not decide)

* **O1. Does an install with Usage reporting OFF send radio failure reports?** The owner said "Send them always" about the DIAGNOSTICS switch.
  The install id exists only while Usage reporting is on, and D5 decision 2 says that switch still turns the journal off. The contract and the
  privacy draft assume NO (no id, nothing sent). If the answer is yes, the schema needs `installId` optional and the site's per-install limiter a
  per-session fallback: a schema 2.
* **O2. Privacy wording** (draft in the site's page source and its record): sent even with Diagnostics off; the serial is collected and why; kept
  indefinitely; deletion by writing to the support address quoting the install id. Read it; it is not legal advice.
* **O3. The daily bound of 100 a day per install**, and the 6 first-session messages a session: proposals.
* **O4. Alert thresholds** for the per-driver rule: failures outnumber opens, at least 5 failures, at least 3 installs, 7 days, repeated weekly.
* **O5. Disk.** The store keeps every body forever (D5). A typical report is 962 bytes, the contract's worst case 10,707 bytes. See the site record for
  the per-year table. At 1,000 failures a day typical it is about 335 MiB a year; at 10,000 a day about 3.3 GiB a year (37 GiB worst case), which the
  Pi cannot hold: that needs the monthly files archived to a store that grows (R2 or the NAS), decided on cost. The 1 GiB ceiling makes the intake
  answer 503 (the app keeps and retries) rather than fill the disk.
* **O6. Should the first-session record also be sent from sessions 2 and 3?** Today only launch 1 (a returning user is in the usage dataset).
* **O7. Deploy actions that are the owner's:** `wrangler secret put RELAY_TOKEN` and the same value as `FOXSDR_RELAY_TOKEN` in the site's
  environment file; the two new Analytics Engine datasets come with the next `wrangler deploy`.

## 7. UNVERIFIED

* Nothing was run against Cloudflare: `wrangler.toml` was not parsed by wrangler (no wrangler run), the Analytics Engine limits (20 blobs, 20
  doubles; first-session uses 17 and 18) are from `telemetry-worker/README.md`, and the Worker's `fetch()` to the site was only tested against a stub.
* Whether a Worker subrequest carries the end user's address to the site (`cf-connecting-ip`) is not verified. The Worker sends only a content type
  and the relay header, and the site stores no address either way; but the site's own access log (if any) is outside this change.
* The serial field's source per native driver (5.1), and whether the log ring holds lines when Diagnostics is off (it is preallocated memory, but I
  did not read where the "off" switch cuts in). If the ring is empty with Diagnostics off, `logLines` is empty and the report still carries driver,
  model, serial, class and reason: the contract allows an empty array.
* The radio-line prefix list was taken from a count of `diagLogf`/`diagWarnf` prefixes in `src/source`, `src/engine`, `src/usb`; the app lane
  confirms it against the live log.
* The Go port of the text rules was held to the Worker's JavaScript over 557 strings, not proved equal over all strings.
* The real HF+ failure was not reproduced (no device, and the owner's rule is no hardware use here).

## 6b. Privacy wording: DRAFT for the owner (also in the site's `web/privacy.html`, marked; not legal advice, not checked by a lawyer)

For PRIVACY.md, `docs/DIAGNOSTICS.md`, the in-app Diagnostics and Usage reporting text, the Store listing's privacy answers and the 0.99.109 release
notes. The four statements the owner's decisions require:

1. **Sent even with Diagnostics off.** "When a radio does not open, FoxSDR sends one report about it, every time it happens. This is the one report
   that is not governed by SYSTEM > Diagnostics: it is sent whether Diagnostics is on or off, because a radio that fails to open is the commonest way
   FoxSDR does not work and nobody can fix what they are not told about. It is governed by SYSTEM > Usage reporting: with that off there is no install
   identifier and nothing is sent." (O1: owner to confirm the last sentence.)
2. **The serial number is collected.** "A radio failure report carries the radio's serial number. It lets us see that two reports, from two computers or
   two days, are about the same physical radio, so a faulty unit can be told from a fault in FoxSDR or in a driver, and a radio can be found again when
   somebody quotes its serial. It identifies a radio, not you, but it is held with your install identifier, so we treat it as personal data. Serial
   numbers are still removed from everything else FoxSDR sends." PRIVACY.md "What is never sent" and its short version lose the "Hardware serial numbers"
   line, or gain "except in a radio failure report".
3. **Kept indefinitely.** "Unlike crash reports (90 days) these are not deleted on a schedule."
4. **Deletion.** "Write to support@foxsdr.com and quote your install identifier: every radio failure report from that install is erased from the server,
   together with any radio only that install ever reported." (The app lane adds where FoxSDR shows the install identifier: not verified here.)

What a report contains, for the field table: driver and model; serial; firmware and library version where known; the driver's reason sentence, scrubbed
(serials, account names, addresses, host names, frequencies removed, paths replaced by `<path>`); the radio lines of the log; FoxSDR version, install
channel, update channel; operating system and build; install identifier; seconds since FoxSDR started; how many times this radio had failed in this run;
whether Diagnostics was on. Never: a tuned or centre frequency, a file name, anything decoded, a position, an IP address, a user or computer name, a path.

## OPEN ROWS FOR THE NEXT LANE

| Row | Owner / lane | Where it is described |
|---|---|---|
| Build `src/core/radio_report*.cpp`, the five call sites, the queue, the daily bound, the first-session sender, the tests | app lane (after the six-branch merge) | section 5 |
| Add a `classifyRadioOpen` class for "stopped answering ... control transfer failed" (L2); make `other` itself a reported event | app lane | section 5.4 item 5 |
| Verify the serial source per native driver | app lane | section 5.1 |
| Decide O1 to O7 | owner | section 6 |
| `wrangler deploy` (Worker first), relay secret, site deploy | owner (nothing here deploys) | section 6, O7 |
| PRIVACY.md, DIAGNOSTICS.md, in-app Diagnostics wording, release notes, Store privacy answers | app lane, from the privacy draft | site `HANDOVER-daylight.md` |
| The D5 events and the radios page grow from the radio table and the envelope; the beat needs the radio identity (Worker first) | D5 lane | contract section 1 |
| Archive of old monthly files to R2 / NAS and an archive reader | D5 lane, if O5 says so | site record |
| Translate the privacy draft once the owner has approved it | site lane | site record |
