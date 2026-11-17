# Send-on lane (2026-11-17): 0.99.109 sends radio failure reports, and no test run ever sends one

Branch `feat/send-on` (cut from `4229dbd3`). Worktree `D:/foxsdr-wt/en-sysmodels`, build `D:/foxsdr-build/en-sysmodels`, scratch `D:/foxsdr-build/so-scratch/`.
Nothing pushed, nothing deployed, no outside request made (every post in the tests is to 127.0.0.1 or vetoed by the observer before a socket exists).

## 1. Nothing a test, drive or CI run does can reach the real service

Routes found (read, not assumed): the only places a radio report's bytes reach the network are `radioreport::makeRealTransport` (used by `Reporter::runBatch`,
which `poll()`, `flushAtQuit()` and `runBatchNow()` all call) and, for the beat that used to carry radio keys, `TelemetryReporter::send`. The sender is armed only by
`Engine::configureHeartbeat()` (an interactive run: `ServiceOptions::heartbeat`, set by `window_service_options.cpp` for a non-bounded run); a bounded run
(`--frames`, a bounded script) never arms it. Every drive of `foxsdr-slint.exe` / `cascade.exe` IS an interactive run. Before this lane the only protection was that
the harnesses point `FOXSDR_TELEMETRY_URL` at `http://127.0.0.1:9/`: a harness that forgot it, a new test, or CI, would have posted.

**Mechanism (core/report_send_guard.cpp).** `outsideSendRefusal(url, profileDir)` is called inside the real transport. A destination on this computer (127.0.0.1,
localhost, ::1) is always allowed. Anything else is refused (attempted = false, no socket; the sender keeps the message and backs off) unless the run is a real
user's: refused if `FOXSDR_NO_OUTSIDE_REPORTS` is set (ctest sets it on every test of `tests/`, `src/slint/`, `src/slint/tests/` through
`cmake/NoOutsideReports.cmake`; the hermetic helpers and the four drive harnesses set it too), if `syntheticRun()` (FOXSDR_SYNTHETIC, CI, GITHUB_ACTIONS), if
`FOXSDR_DIAG_DIR` is set, or if the profile folder is not the one the operating system names for the user (SHGetKnownFolderPath(RoamingAppData) on Windows, the
passwd home on POSIX): a drive that redirects APPDATA or HOME, as every drive and hermetic test does, is stopped even when it forgot every marker. Portable mode and
the Store package are exempt from the last rule. The heartbeat's transport asks the same function.

(Checked with `ctest --show-only=json-v1`: 1651 of the 1656 registered tests carry the variable; the other 5 are root-level and `sdk/` tests, which start no application.)

**Proof.** `tests/test_radio_reports_send_on.cpp` installs a post observer (`setPostObserverForTest`, called at the top of `postBounded` and in the heartbeat's
transport, before any socket) that records the destination and VETOES it. It tries: the five markers; a redirected profile with no marker; an empty profile; the
whole `Reporter` with its default transport and the real endpoint; the heartbeat; and the positive control (a real user's profile asks for
`https://telemetry.foxsdr.com` and the observer vetoes it, so the guard is shown to let a real run through). Break-it: the body of `outsideSendRefusal` replaced by
`return ""` gave `test_radio_reports_send_on: 90 checks, 15 failed` (every marker way "reached the network layer", the redirected profile, the whole reporter, the
heartbeat); restored: green. Not covered by construction: a test that sets APPDATA to the real profile on purpose.

## 2. The switch

`kRadioReportsLiveSend = true` (`src/core/radio_report.hpp`). Changed expectations, both values:

| Test | Was | Now |
|---|---|---|
| `test_radio_report` | `CHECK(!kRadioReportsLiveSend)` | `CHECK(kRadioReportsLiveSend)` |
| `test_radio_reporter` "THE SWITCH" | `static_assert(!kRadioReportsLiveSend)`; `o.liveSend = kRadioReportsLiveSend` and nothing sent | `static_assert(kRadioReportsLiveSend)`, `CHECK(Options{}.liveSend)`; the "nothing sent, both queued" case passes `liveSend = false` as a bounded run does; the same queue still goes out when armed |
| `test_radio_reports_fields` beat | `kRadioReportsLiveSend == beat.contains("radios")`, 3 members while off | `kRadioReportsLiveSend && !kBeatCarriesRadios`, no `radios`, 3 members (4 with the synthetic marker) (see item 4) |
| `test_radio_reports_never_sent` | `o.liveSend = kRadioReportsLiveSend` (the queue as written) | `o.liveSend = false` (same meaning: the queue as written; `rep2` is the wire) |

Comments in `radio_report.hpp`, `radio_reporter.hpp`, `telemetry.hpp`, `engine.cpp`, `engine_radio_reports.cpp`, the contract's section 11 and `docs/DIAGNOSTICS.md`
(a new "Deployment order (radio reports)" note in its 0.99.109 block) now say: the Worker is deployed BEFORE any build with sending on is given to anyone; the app
tolerates the Worker of today and a site that is not up.

## 3. What a real user's copy does (real transport over loopback HTTP, mock clock; `test_radio_reports_send_on`)

| Situation | Behaviour (tested) |
|---|---|
| The Worker of today answers `400 bad id` | kept and retried; 300 further attempts in the same second made no request; over 13 days 55 requests (1, then +1 min, +5 min, +1 h, then every 6 h); file still there on day 13, nothing refused, nothing sent; dropped only after 14 days and counted (`expired == 1`) |
| The new Worker with no site (503, then 502) | kept; gaps between requests exactly 60, 300, 3600, 21600, 21600 s; when the service answers 204 the message is delivered and deleted; down for 14 days: dropped and counted (`expired == 1`) |
| Nothing listening / the service never answers | `configure` 2-3 ms, `fileFailure` + `poll` 12 ms (the send is on its own thread); quit (`flushAtQuit(1500)` as the engine calls it, then `shutdown`) took 1528 ms with nothing listening and 1518 ms + 0 ms with a service that never answers. **Worst added shutdown time: about 1.5 s** (the engine's own cap; only when a message is queued and not in back-off; the transfer is cancelled and the message kept) |

## 4. Usage reporting and Diagnostics, case by case (held to PRIVACY.md "Radio failure reports and the radio journal")

| Case | Failure reports sent | Journal / machine description | First-session record | What a failure carries |
|---|---|---|---|---|
| Usage OFF, Diagnostics OFF | 1 | 0 | 0 | no installId, sessionId or seq; a random 16-hex `messageId`; `diagOn` false |
| Usage OFF, Diagnostics ON | 1 | 0 | 0 | same; `diagOn` true |
| Usage ON, Diagnostics OFF | 1 | 2 (open event, machine description) | 1 | installId, sessionId, seq; `diagOn` false |
| Usage ON, Diagnostics ON | 1 | 2 | 1 | same; `diagOn` true |

Also tested: turning Usage reporting off deletes every queued message that carries the install id (searched on disk: absent) and the failure stays, anonymous; the
beat is not configured without an install id.

**A difference found and fixed in the PROGRAM, not the text.** With the switch on, the usage heartbeat would have carried `radios` (the open radios' model slugs and
serials, with the install id, every five minutes). PRIVACY.md says the beat is "the install identifier, the version, and nothing else" and that serial numbers are
removed from everything but the radio reports. So `radioreport::kBeatCarriesRadios = false` (new constant, `nextBeatJson` honours it); the Worker's reader and
`HeartbeatSender::setRadios` are kept and tested. Sending the field needs the owner to approve a sentence first. The site's "running now" per-radio view therefore
has no data from this version.

## 5. The approved sentence

Replaced word for word in `PRIVACY.md` only; `git diff --word-diff` shows nothing else moved (the sentence before it, naming `https://telemetry.foxsdr.com`, is
untouched). Searched in this tree: the old sentence is in no other file. `docs/DIAGNOSTICS.md` does not repeat it (its block says the message is "written ... before it
is sent", which is now also true of the send; a clause about the 14 days was added to that block, which is explanatory text, not the approved wording); the in-app
Usage text ("It still sends one report whenever a radio does not open ...") and its help topic do not carry it; no language catalogue carries it (so no translation was
needed: catalogues 0 missing, 0 stale unchanged, `lang_assets.hpp` untouched; `test_i18n` passed); `STORE-LISTING.md` and `docs/release-wording-drafts.md` are not in this
tree (the second is on `site-lane-4`).

**Website copies (read-only, `C:/Users/steve/foxsdr-site`, branch `site-lane-4`; nothing changed there):** the site's draft does NOT contain the old sentence, so the
approved sentence has to be ADDED, not replaced: `web/privacy.html`, the "Where it goes." paragraph (after "...older months in a second, compressed store ...", about
line 389); `docs/release-wording-drafts.md` line 36 ("Where it goes: the telemetry service on foxsdr.com ..."). Also the site's page still says (line 202 area) "a beat
... skipped rather than queued", which is about beats, unrelated. `STORE-LISTING.md`: not found in either tree.

## 6. Release notes

`docs/release-notes-0.99.109.md`: a new item "What this version sends about radios" under the merged items, one paragraph, quoting the approved text for "whether or
not Usage reporting is on" and for the 14 days.

## What the release lane must do at deploy time, in order

1. `wrangler secret put RELAY_TOKEN`, then deploy the Worker (`telemetry-worker`, owner's word), prove it with real messages (a `radio_fail` with and without
   `installId`; `bad id` must be gone).
2. Deploy the site's intake (`radiofail.go`, `radiojournal.go`, `switchover.go`; relay token in its environment); prove with the same real messages; 502 must be gone.
3. Only then build the release candidate and give it to anyone (the tester pre-release included). The version string must change in the same commit.
4. Add the approved sentence to the site's `web/privacy.html` and `docs/release-wording-drafts.md` (item 5) before the site goes live.
5. After the pre-release is installed on a real machine, send ONE real failure report from a real (non-test) run to confirm the real path (UNVERIFIED here: this lane
   made no outside request).

## Tests run (all at the final commit unless stated)

`test_radio_reports_send_on` (90 checks), `test_radio_report`, `test_radio_reporter`, `test_radio_reports_never_sent`, `test_radio_reports_fields`,
`test_radio_reports_engine`, `test_radio_events`, `test_mustfix_b`, `test_error_paths` (pin 410 not raised: no site added), `test_i18n`, `telemetry_worker` (the Worker's whole
suite), and on the hidden desktop `test_crash_upload`, `test_health_events`, `test_stall_count`, `test_diagnostics`, `gui_desktop_guard`: all passed.
NOT RUN: the full headless and GUI gates, the drives (the four drive harnesses were edited to set one environment variable; they were not run), the Linux/macOS
builds of `report_send_guard.cpp` (the POSIX branch is UNVERIFIED), the installer test.

## UNVERIFIED

- The Store (MSIX) package: `runningInPackage()` exempts it from the redirected-profile rule by reading the package identity; not run packaged.
- Portable mode on a real stick: the exemption uses `resolvePortable(currentExeDir())`; not run on one.
- The real endpoint, the real Worker and the site: nothing was contacted.
- `kCompanionFaultReportsOn` stays false and the installer's failure report stays off (owner items 8 and 10).

Config hash at the end: `c21ce98dc30e50327f7666f7ee29047c0983d130ca3f21716d0c0e20e04603c3` (unchanged).
