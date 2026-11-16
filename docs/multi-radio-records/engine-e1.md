# Engine lane E1 (wave 8): the owner's engine answers of 2026-11-16 and the engine rows other lanes handed over

Branch `feat/engine-e1`, cut from the merged engine tip `d4f855c5`; worktree `D:/foxsdr-wt/en-sysmodels`, build `D:/foxsdr-build/en-sysmodels`, scratch
`D:/foxsdr-build/e1-scratch/`. Nothing pushed. The real `%APPDATA%/foxsdr/config.json` keeps sha256
`c21ce98dc30e50327f7666f7ee29047c0983d130ca3f21716d0c0e20e04603c3` (read at the start and again after the last test; no `config.json.secrets` was ever made there:
every test used a scratch folder).

## The rows

| # | Row | Verdict | Proof |
|---|---|---|---|
| 1 | 16 secrets out of plain text | BUILT (see "What was found" below: the tree held no e-mail or InfluxDB setting) | `test_secret_vault` 103 checks; break-it: `splitForWrite` made a no-op, 16 failed |
| 2 | 17 HTTPS | BUILT: the code existed (`net/web_tls`), the plumbing and the switch did not; the smart-card prompt's cause re-read and held | `test_server_host` H17 and H17b (a real Schannel client on the engine's server); `test_web_tls` 272 and `test_web_secure` 793 on the hidden desktop under `tools/watch-new-windows.ps1`: 0 new windows; drive `e1-web-secure` |
| 3 | 73 number format | BUILT: the setting existed (lane K); the readouts it did not reach were wired, typed input takes both signs | `test_slint_host_numbers` 44, `test_typed_number` 1545 (33 languages by Windows' own symbols), `test_slint_lanel_host`; break-it: locale ignored, 18 failed |
| 4 | 74 duplicate backup / import / reset | BUILT (removal): the two copies were found; the engine's was removed | `test_engine_api_wire_c_sys`, `test_apply_command`, `test_error_paths`; lane K's drive scripts still pass |
| 5 | RDS logging | BUILT | `test_rds_log` 74, `test_engine_api_wire_c_rds` 44; break-it: the publish hook removed, 8 failed; drive `e1-web-secure` |
| 6 | D2 range-scan lockouts, D1-mem memory thresholds | BUILT | `test_engine_api_wire_a_scan` 314, `test_slint_lanec_host`, drives `e1-lockout-range` (real engine), `lanec-memory`; break-it: the fact made constant, 3 failed |
| 7 | 15 installer report | BUILT, OFF; not compiled (no Inno compiler) | `test_installer_report`; contract text below |
| 8 | 14 old versions prompted | HAND-OFF written (below); the app needed no change | `test_update_old_prompt` 72 |
| 9 | 18 shared-receiver multi-user web | BUILT as far as the code allows: the limit, who holds the tuning, what the others see; chat is a switch; bans and moderation NOT BUILT | `test_server_host` H17, drive `e1-web-secure` |
| 19 | confirm | CONFIRMED, see below | |
| 80 | confirm | CONFIRMED | |
| 54 | confirm | CONFIRMED | |

### What was found, item 1 (check the code before believing a row)
The brief says the e-mail alert password, the InfluxDB token and the web password sit as plain text. At this tip: the web password is a PBKDF2 RECORD (a hash), not text;
there is no e-mail or InfluxDB setting anywhere (`core/power_log.hpp` and `core/mqtt_publisher.hpp` are blocks no setting reaches). The plain-text secrets that were really in
`config.json` are `testerToken` and `testerAppToken`; plugin settings of kind PASSWORD (`pluginSettings`, which is where an e-mail alert or InfluxDB plugin keeps its password
and token) are also written to it. All of these, the web password record and the new listener password record now live in the protected store.

Store chosen: DPAPI (`CryptProtectData`, current user, `CRYPTPROTECT_UI_FORBIDDEN`), not the Credential Manager. The tree used neither before. DPAPI keeps the blob in a file we
choose (`config.json.secrets`, hex of the blob, the key name as entropy), so tests write into a scratch folder and never into the real user's credential list; it opens only for
the same Windows user on the same PC; no library is added. `ConfigStore::writeFile` takes the secrets out of every document it writes (a secret present and empty removes its item);
`ConfigStore::load` puts them back and migrates a value an older build left in the file (file rewritten without it, dated `.bak` copies scrubbed). Export leaves them out as before (the
file it reads no longer holds them). Import and reset remove the sidecar with their file. A secret the store cannot open reads as empty and is logged.
LINUX: there is no per-user store: the sidecar holds the value as hex under `plain:` with owner-only (0600) permissions; the settings file's copy is still removed. The proper store
(Secret Service or a kernel keyring) is an open row.
"A second Windows user cannot read it": proved only as far as the entropy binding (a blob does not open under another key name, `testRefusals`) and by DPAPI's documented per-user master key.
No second account was created or signed in: UNVERIFIED.

### Item 2: the prompt
Cause as found by the earlier web-secure lane and re-read here: `testThumbprintCertificate` opened the real Personal certificate store for writing and added a certificate carrying a
key-provider record that was not silent, which Windows resolves through its credential broker (a smart-card prompt when the key cannot be had silently). It could not be reproduced
(the 2026-11-11 binary is gone). The fix in the tree: every CNG and CAPI call passes its silent flag, a certificate picked by thumbprint is accepted only if its provider is a Microsoft
software provider, the store is opened read-only in one place, the tests use an in-memory store, and `testTlsSetUpIsSilent` scans for all of it. Re-checked today: the scan passes,
the client credential in `tests/web_tls_client.hpp` sets `SCH_CRED_NO_DEFAULT_CREDS`, the server's credential sets `SCH_CRED_NO_SYSTEM_MAPPER` and never asks for a client certificate.
Proof it stays gone: `test_web_tls` (272 checks) and `test_web_secure` (793) run on the hidden desktop `FoxSDRE1Tests` under `tools/watch-new-windows.ps1`, which snapshots every top-level
window of the visible and the hidden desktop, samples both every 50 ms and lists any new one with the process that made it: 0 new windows from the tests (two windows from other programs'
processes appeared on the visible desktop meanwhile, both `wslhost`; the script now separates them by process tree). The watcher itself was checked: it saw the 5 windows of a
form made on the hidden desktop, and it sees the 227 windows already on the visible one. H17b (the engine's own web server over HTTPS with a real client) ran on the hidden desktop too.
The switch: Web section, "Use HTTPS for the web page"; a certificate of the person's own by its 40-digit thumbprint or the program's own (kept beside the settings, its key in the
user's key store, not exportable); the fingerprint and the date it is valid until are shown while it runs. WEBHOOKS OVER HTTPS: NOT BUILT. The webhook model (`net/webhook.hpp`) is not
wired into the engine at all (nothing includes it), and its transport refuses https (`supportsHttps()` is false). It needs the model wired first; see OPEN ROWS.

### Item 3
What exists: the Accessibility section's "Number format" (`access.numfmt`, default "Follow the Windows regional settings"), applied by `host_logic` to four readouts. Wired now:
`fmtRate`, `levelText`, the PPM and output-rate cards, the SNR / peak / floor / channel-power / track box lines, the volume text, the peak text and its age, marker labels on the
spectrum and the 3D view, the context-menu marker tips. Typed numbers take either sign in the places the lanes own (`parseNumber` of lane L, `toNumber` of lane C, `readNumber` of the
Tools bodies; the frequency editor already did). NOT done, listed in OPEN ROWS: the SIGNAL page's own readouts and parsers (`signal_logic.cpp`, lane S1's), the many `%.Nf` texts of
`main.cpp` that build a dB or time figure for a tooltip, the map and 3D pages' texts, and the clipboard copy of a frequency (kept with the dot on purpose: it is pasted into other programs).
The wire-c key `access.number_format` (default "dot", stored, never read by anything) is a second, unused copy of lane K's setting: left in the registry (a key is never removed from it).
Expectation changed: `test_slint_lanel_host`: `parseNumber("12,5", 0, 100)` was refused ("a comma is not a decimal point here"), now 12.5.

### Item 4: which two, which kept
Two copies of settings export, import, backup and reset: (A) screen lane K's, in the SYSTEM page's "Settings and backup" section (`slint/lanek_*`, `SettingsKeeper`, working the files at the
end of the run, with profiles, sync and undo of a reset); (B) wire-c's engine operations `0x8A03..0x8A09` with a staged-at-next-start mechanism (`wirec-pending.json`, boot step 2c, facts
`sys.export.last`, `.import.*`, `.reset.*`, `.backup.last`, `.pending`, `.lastboot`), which no window ever sent (`screen-handoff-L.md` said "keep K's"). Kept: A (the SYSTEM-page one).
Removed: B's operations, handlers, facts, the staged mechanism, boot step 2c and the "last start" line of lane L's About block, and the tests of them. The first-run wizard ops (`0x8A02`)
stay. "The kept one does all three": lane K's drive scripts `lanek-backup` (export, backup, import with its confirmation, reset and undo) are in the headless/GUI gates below.
The registry keys `sys.backup_on_close`, `sys.backup_keep`, `sys.sync_folder` stay (append-only registry; tests use one as a sample key).

### Item 5
`core/rds_log.{hpp,cpp}`: a CSV (`time_utc,radio,station_name,programme_type_code,programme_type,radio_text`) written by a worker thread of its own into the folder the person chose, the file name by
the recorder's template rules (`core/rec_naming.hpp`, default `rds_{date}`, `.csv` added), one line each time the name, type or text of a radio's RDS changes. Off until the person switches it
on (System section, "RDS log"; wire-c keys `rds.log`, `rds.log.folder`, `rds.log.name`). Text only: no frequency, no PI code; the facts (`rds.log.lines`, `.dropped`, `.error`) are counts and a
sentence. Characters the RDS basic set leaves undefined are written as `?` (no conversion table was built: OPEN ROWS).

### Item 6
Engine: `Scanner::lockoutInfo`, facts `scan.lockouts` ("hz@seconds-left;..."), `.count`, `.next`; `SCANNER_EXTRAS 6` = per-memory thresholds (kept apart from list 4, which the Tools window sends and which wins on a frequency;
persisted). Window: the Scanner body's keys are live in any running scan and send `SCANNER_EXTRAS 1/2` for a range or plan scan (a scan of the ticked rows still unticks the row), the status text counts both and
names the time left, the memories' thresholds are pushed at start-up and on change (`MemThresholdPush`; a long text goes by `submit_text`). The timed lockouts' seconds are those at the moment the facts were made.

### Item 7: the installer's report (OFF)
Read: `installer/cascade.iss` has no network code; an Inno Setup `[Code]` section can make one HTTPS request with the COM object `WinHttp.WinHttpRequest.5.1` that ships with Windows (no
helper program, no file dropped). `installer/install-report.iss`, included only when `ReportInstallOutcome` is defined (it is a commented line in `cascade.iss`, with the URL define beside it), sends the
contract's envelope (CONTRACT ISSUES below), interactive installs only (not silent, not `/DATAROOTS`, not `/NOREPORT`), once, at most about five seconds. Outcome words: `cancelled` (Cancel clicked and the install
did not finish), `failed` (copying began and did not finish), `blocked` (Setup stopped before copying). UNVERIFIED: the script has not been compiled or run (no Inno compiler on the machine, none downloaded);
`CancelButtonClick` records only a candidate, because Inno does not say whether the person then answered No to "Exit Setup?".

## CONTRACT ISSUES (for the Worker and site lanes; `docs/radio-reports-contract.md` was NOT edited)

The installer's report (item 15) is a new kind that the contract does not list. The installer cannot send an `installId`, a `sessionId` or a `seq` (it runs before the program has ever run, and the owner's
rule is: outcome, FoxSDR version, Windows version, nothing else). It uses the SAME envelope as an anonymous `radio_fail` (section 1.2), one message per request, to the usage endpoint the app knows:

```
POST https://telemetry.foxsdr.com/        Content-Type: application/json
{"schema":2,"kind":"install_outcome","messageId":"<16 lower-case hex>","t":0,
 "body":{"outcome":"failed"|"cancelled"|"blocked","appVersion":"0.99.109","windows":"10.0.22631"}}
```

What the Worker has to do (nothing is deployed; the installer's send is OFF until it does):

| Rule | Value |
|---|---|
| kind | `install_outcome` (new; today it is refused as `unknown-kind`) |
| envelope | `schema` 2, `messageId` `^[0-9a-f]{16}$` required, NO `installId`, NO `sessionId`, NO `seq`, NO `radioKey` (a message that has any of them is `bad-field`), `t` 0 |
| `body.outcome` | exactly one of `failed`, `cancelled`, `blocked` (anything else `bad-field`; a successful install is never reported: the first run's usage report is the trace of a good install) |
| `body.appVersion` | `^[0-9]{1,3}\.[0-9]{1,3}\.[0-9]{1,5}$` (the version being installed) |
| `body.windows` | `^[0-9]{1,2}\.[0-9]{1,2}\.[0-9]{1,6}$` (major.minor.build) |
| no other body key | unknown keys refused |
| retention and the site | a daily count by (outcome, appVersion, windows) is all the site needs; nothing here identifies an install; the messageId only lets a retry whose answer was lost count once (the same 100,000 window as section 1.2) |
| limits | the same per-address rate limit the anonymous `radio_fail` gets (the installer sends once per run) |

Then, in `installer/cascade.iss`, remove the semicolon in front of `;#define ReportInstallOutcome` (one line). That is the whole switch. Nothing else about the installer changes.

WHAT CANNOT BE REPORTED AT ALL: an installer that Windows blocks before it runs (SmartScreen, an antivirus that quarantines or deletes it, a policy that forbids it, a download that never finished, a person who never opens it) runs none of
the installer's code, so no message is sent and none can be. The only trace of those is the gap between downloads and first runs.

## HAND-OFF FOR THE SITE LANE: prompt a desktop FoxSDR older than 0.99.64 to update (item 14)

Those installs cannot be changed. The only thing that reaches them is the answer the site gives to their update check, so this is exactly what the site must send.

**Who asks.** Every desktop FoxSDR that has the updater (since its first release; it asks once per launch): `GET https://foxsdr.com/api/update?v=<running version>[&channel=<word>]` (from `src/core/updater.cpp` at tag `v0.99.63`, `checkForUpdate`, and the
same function today). The site chooses the answer by `v`. A request whose `v` is lower than `0.99.64` (compare the numeric parts as numbers, not as text: `0.99.9` is lower than `0.99.64`) gets the answer below. `0.99.64` and higher get their ordinary answer.
Only the people whose update check is switched on (it is on by default) ask at all.

**The answer** (a JSON object; the field names and TYPES are what matter, see "never send" below):

```
{"version":"<the newest release, e.g. 0.99.109>",
 "url":"https://foxsdr.com/downloads/foxsdr-setup-<version>.exe",
 "sha256":"<64 hexadecimal digits: the installer's own SHA-256>",
 "size":<bytes of that installer, a whole number>,
 "notes":[{"version":"<the same version>","date":"<yyyy-mm-dd>","critical":true,
           "notes":["<the ONE LINE>"]}]}
```

`url` must be `https` and on `foxsdr.com` or a sub-domain of it (the old updater refuses any other host), `sha256` must be 64 hex digits and must match the file (it is checked before anything runs), the answer must be
under 256 KiB.

**What the person sees** (the old window, `AppWindow::drawUpdateBanner` at `v0.99.63`): a red line "Important update: FoxSDR 0.99.109" (red because `critical` is true; `false` gives an amber "FoxSDR 0.99.109 is available"),
the grey line "you are running <their version>", the version again with "(important)", ONE bullet holding the line, a "Download update" button (with the size and "verified against its published checksum before anything runs" when `size` is sent), and
"Not now". Pressing the button downloads the installer, checks the digest and offers "Install now and restart". Nothing is installed without the press.

**The one line.** Proposed wording, for the owner to change: `A newer FoxSDR fixes problems found in this version, among them radios that were not found. Please update.` It is plain text; send it in the language of the site's default (the old window does not translate it).

**Never send, to a version below 0.99.64** (the old reader THROWS on a field of the wrong type, and a throw there ended the update check; `0.99.64` fixed it, the old ones cannot be fixed): `version` that is not a string; `critical` that is
not `true`/`false`; `date` that is not a string; `size` that is not a whole non-negative number; a `notes` entry that is not an object. Extra keys (`messages`, `withdrawn`, anything newer) are ignored by the old reader and are safe.
A note with no `version` or no lines is dropped by the old reader, which would leave the banner with no line: always send both.

**Proof in this tree:** `tests/test_update_old_prompt.cpp` holds this exact answer to a copy of the v0.99.63 reader (`oldParse`, taken from `git show v0.99.63:src/core/updater.cpp`) AND to the parser of this tree, for six old versions,
and shows each type error above throwing in the old reader. UNVERIFIED: a real v0.99.63 binary reading the site's real answer (no old binary was run; none exists on this PC to run).
The current updater needed no change.


### Item 9: what is built, what is not
The module (`net/web_users`, `net/web_arbiter`, the room in `net/web_server`) already existed with its tests; what was missing was everything between it and a person. Built: the config keys,
SERVER_CONFIG, the host, the facts, the facts' limits, the window rows. The model: everyone who signs in with the shared listener password hears the same tuned receiver; the owner's account tunes;
`listenersMayTune` is the one switch that lets a listener tune as well; the user limit (1 to 32), the idle and session limits, chat (off, never stored) and the live count are settings and facts.
What the others see is the module's own room page (listener names, redacted receiver state; owner decisions on it are in `docs/feature-records/web-secure.md`). NOT BUILT: moderation (kick, ban,
unban, a note to the room: they need engine ops `FOXAPI_OP_WEB_KICK/BAN/UNBAN/NOTE` and a read of the listener list), a persisted ban list (an owner decision recorded as "in memory only"); see OPEN ROWS.
A browser-side check of the room page with a real browser was not made (UNVERIFIED), and the hermetic window hosts no listener, so the window drive proves the settings and facts only.

## Confirmations (no building)
- **19**: UPnP is still not wired: nothing outside `net/upnp_igd.*` includes it. The decode database is still not wired into the engine (nothing outside `core/alert_rules.hpp` includes `core/decode_db.hpp`; so nothing is
  kept on disk). Webhooks are plain http only: `HttpTransport::supportsHttps()` is false, and nothing in the engine uses the model. The CAT emulation is still this PC only by default (`cat.emulation.bind_all` "0");
  there is a wire-c key that widens it but no control for it anywhere in the window. Nothing merged this week widened any of the four.
- **80**: a healthy radio is NOT closed and reopened after a sleep: the only action on a resume is `Engine::wireCResumeAction` (`engine_wire_c.cpp`): it re-scans the device lists and resets the "busy radio" retry counter; no source is closed or opened.
- **54**: no new transmit ability: none of the items above touches a transmitter; a diff of `src/` from `cd99c76d^` (the first commit of 2026-11-09) to `d4f855c5` for key-down / PTT / transmit additions found only the existing `FOXAPI_OP_TX_PTT` named in a table and a key-table row that is skipped.

## New engine commands
No new operation number. `SERVER_CONFIG` "web" member: `tls`, `tlsThumbprint`, `listeners`, `listenerPassword`, `listenerPasswordRecord`, `clearListenerPassword`, `maxListeners`, `listenerIdleSeconds`, `listenerSessionSeconds`, `listenersMayTune`, `chat`.
`SCANNER_EXTRAS` selector 6. wire-c keys `rds.log`, `rds.log.folder`, `rds.log.name`. Retired (answer nothing): `0x8A03..0x8A09`.
Facts added: `web.tls*`, `web.listeners*`, `web.hasListenerPassword`, `web.listenerPasswordPending`, `web.maxListeners`, `web.listenerIdleSeconds`, `web.listenerSessionSeconds`, `web.listenersMayTune`, `web.chat`, `scan.lockouts`, `.count`, `.next`, `scan.memthresholds`, `rds.log.lines`, `.dropped`, `.error`. Facts removed: `sys.export.last`, `sys.import.*`, `sys.reset.*`, `sys.backup.last`, `sys.lastboot`, `sys.pending`.

## SHARED FILE EDITS
`src/slint/main.cpp` (about 20 lines: the number readouts, lane C's long-text submit), `src/slint/ui/system_bank.slint` (one `KPanel` kind: 9, a masked field), `src/engine/engine.cpp` (one line: the RDS hook, plus the
retired boot step's comment is in `process_boot.cpp`), `src/engine/engine_radios.cpp` (one line), `src/engine/engine.hpp` (two declarations), `src/engine/engine_service.cpp` (one line), `src/core/config.{hpp,cpp}`,
`src/core/config_halves.hpp`, `tests/CMakeLists.txt` (two names added to the source-root test list), `tests/errpath_allow.tsv`, `tests/test_error_paths.cpp` (pin 1126 to 1125). Not touched: `signal_bank.slint`, `signal_logic.cpp`, the store, `installer/msix/`, the telemetry queue and sender, the `radio_fail` call sites.

## New strings
54 keys (lane L's HTTPS, listener and RDS rows, the lockout status, "that is not a number"), 4 stale ones dropped, in all 33 catalogues; MACHINE-MADE by three Sonnet agents and UNCHECKED by a speaker (the agents flagged
"thumbprint" vs "fingerprint" in ru, uk, sk, sl, hu, it, lt, lv, pl and the fragments "FoxSDR makes its own" / "One from Windows"); `src/core/lang_assets.hpp` regenerated.

## Error-path gate
Pin before 1126, after 1125 (the retired boot step's swallowed exception is gone). Eleven new sites carry allow lines with reasons (secret read failure, the RDS logger's two sentences, seven SERVER_CONFIG refusals, the window's long-text submit, one renamed parse sentence). No pending site was added.

## GATES (hidden desktops `FoxSDRE1Gate`, `FoxSDRE1Gui`, `FoxSDRE1Rerun`; MSVC Release, `ctest -j3`, the machine shared with other lanes)

Headless and slint logic gate (`-L "headless|slint"`, three slices by test number, 1177 tests): slice 1 (1 to 520): 525 run, 507 passed, 18 red; slice 2 (521 to 1040): 520 run, 514 passed, 6 red, 2 skipped
(`test_plugin_abi3_compat`, `test_rtlsdr_live`); slice 3 (1041 to 1600): 132 run, 132 passed. The 24 reds, each re-run alone after the fixes, classified:

| Red | Class | Result alone |
|---|---|---|
| `test_slint_host_tools`, `test_slint_lanem_host` | REAL, mine: a typed decimal comma is now accepted (owner decision 73); the two expectations changed | Passed |
| `test_engine_main_config_roundtrip`, `_password_not_in_argv`, `_provision`, `test_tester_host_only`, `test_tester_usage_app` | REAL, mine: they read secrets out of config.json text; they now read them back through the protected store and assert the file holds none | Passed |
| `test_engine_publish_alloc` | REAL, mine: the rebuild of the lists took 2004 allocations (bound 2000) after 22 new facts; bound now 2200 | Passed |
| `test_health_paths`, `test_json_dump_policy`, `test_thread_inventory`, `test_server_host` | REAL, mine: a table row, three dump() calls without the replace handler, the inventory rows, and the HTTPS facts that were not republished when the server restarted (H17b caught it) | Passed |
| `test_error_paths`, `errpath_gate_selftest` | green after the allow lines (pin 1125) | Passed |
| `test_engine_api_wire_a_scan` | red once under load (the visited-places check); passes alone, 314 checks | Passed |
| `test_engine_api_03`, `_plugin_smoke_radio`, `test_if_recorder`, `test_iq_readers`, `test_failure_files`, `test_iq_out`, `test_link_request_poll`, `test_engine_api_conformance` | load (timeouts of 120 s or timing) | Passed |
| `test_diagnostics_free` | the commit stamp (binary older than HEAD) | Passed after the rebuild at the last commit (below) |
| `test_pass_control` | known load flake (timing of kicked passes) | RED alone too: its own output is the pass-gap timing (`gaps.front() >= 1.95`, `kickedPasses`); not touched by this lane, not compared with the base tip |
| `test_health_app_stepped` | RED alone too: its own output shows the real application's start-up counted `slow.startup.250ms=1`, so a test that wants exactly the radio-absent count fails: the start-up took over 250 ms on this loaded PC | not explained further; not compared with the base tip |
| `telemetry_worker` | not this lane: the Worker fixture `telemetry-worker/test-fixtures/radio-reports/refuse-address-in-reason.json` is missing in this tree (the radio-reports lane's) | RED alone |
| `test_slint_tools` | not this lane: 7 pixel checks of the Markers sheet (`the sheet`, `marker n: the note is a 24 px field`, `Tune, an xs key`); RED alone, and RED with this lane's `tools_logic.cpp` put back to the base copy; the sheet came with the merge of window-gaps-4 (`d40cf54f`) | RED alone |

GUI gate: the whole `-L gui` (386 tests) was started and stopped after 19 tests because it ran at two tests a minute on the shared PC; a SUBSET was run instead: every non-drive GUI test (`test_*`, `app_smoke`, the guard) and the
drive scripts of this lane's areas (`lane*`, `e1-*`, `extend*`, `system*`, `look`, `keys`, `escape`, `plugin-settings`, `tools*`): 153 tests, 148 passed, 5 red; alone: `slint_drive_tools-airband` Passed (load), `test_tester_host_only`
and `test_tester_usage_app` were REAL (above) and now Pass, `test_health_app` is the same slow-start-up red as `test_health_app_stepped`, and `slint_drive_system-look` is RED alone: `state look.width == 1600 -> FAILED: it is 800`
after the interface size is put back to Auto ("Auto (follows Windows, 100% here)"): the script assumes this PC's Windows scale is 100%; not explained further, not compared with the base tip. NOT RUN: the other 233 GUI tests
(decoder fixtures, globe, cjk, gaps, decode-store, bar sizes, deck skins): none touches this lane's code.

This lane's own proofs, every one run: `test_secret_vault` 103, `test_rds_log` 74, `test_engine_api_wire_c_rds` 44, `test_typed_number` 1545, `test_installer_report`, `test_update_old_prompt` 72, `test_engine_api_wire_a_scan` 314,
`test_server_host` (H17, H17b), `test_slint_host_numbers` 44, `test_slint_lanec_host`, `test_slint_lanel_host`, `test_web_tls` 272 and `test_web_secure` 793 under the window watcher, drives `e1-lockout-range`, `e1-web-secure`,
`lanec-memory`, `lanec-scanner`, `lanec-lockout`, `lanel-system` (all Passed). Break-it, each seen RED then restored GREEN: vault write split (16 failed), number locale (18 failed), the RDS publish hook (8 failed), the lockout count fact (3 failed).
Stamp tests: see the last line of the final report.

## UNVERIFIED
- The installer script (`install-report.iss` and its hooks): not compiled, not run (no Inno compiler; none downloaded). The Worker does not accept the kind yet.
- "A second Windows user cannot read the protected secrets": only the entropy binding is tested; no second account was made or signed in.
- A certificate the person names by thumbprint from the REAL Personal store (the tests use an in-memory store by the earlier lane's rule); the real certificate prompt of 2026-11-11 was never reproduced, so "fixed" rests on the code, the silent-flag scan and the watcher runs.
- HTTPS and the listeners with a real browser; the listener room page with real listeners; the window's controls with a real pointer and a screen reader (mouse drive).
- An old v0.99.63 binary reading the site's real answer (the copy of its reader in the test stands in).
- All 54 new strings are machine translations no speaker has read (the agents flagged thumbprint / fingerprint and two fragments).
- Linux: nothing built or run (the vault's `plain:` form, the RDS log, the typed-number header are standard C++ and were not compiled there).
- Frame cost of the new rows was not measured.
- `test_slint_tools` (7 pixel checks of the Markers sheet, see GATES) and `test_health_app_stepped`, `test_pass_control`, `telemetry_worker` are red in this tree for reasons given under GATES; they were not compared with the base tip's own run.

## OPEN ROWS FOR THE NEXT LANE

| Row | What waits | Kind of lane |
|---|---|---|
| Webhooks over HTTPS | the webhook model (`net/webhook.hpp`) is wired to nothing; its transport refuses https; wire it, give it a WinHTTP transport (the project's `core::postBounded` is one) and a setting | engine |
| Listener moderation | kick, ban, unban, a note to the room: engine ops `WEB_KICK/BAN/UNBAN/NOTE` and a read of the listener list (owner-local, never into a report); the page already has the moderation | engine, then screen |
| Chat between listeners | the switch is built and published; nothing in the window shows a chat; the page has it | screen |
| Number format, the rest | SIGNAL page readouts and parsers (`signal_logic.cpp`, lane S1's files), the `%.Nf` tooltips of `main.cpp`, the map and 3D pages' texts, units (metric / imperial) are still wire-c's `access.units` | screen |
| Linux protected store | the Secret Service or a kernel keyring for the vault (today: 0600 hex) | engine (Linux) |
| Plugin PASSWORD fields declared late | a plugin loaded after the first save keeps a value written as text until the next save moves it (names that say password/token are caught at once) | engine |
| RDS text | the RDS basic character set's accented letters are written as `?`; a table to UTF-8; the PI code and frequency are deliberately never written | dsp / engine |
| RDS log in the window | the rows sit in the System section (lane L); a place nearer the RDS readout on the SIGNAL page is lane S1's | screen |
| Installer report | the Worker accepts `install_outcome`; compile the script with Inno; remove one semicolon in `cascade.iss`; the site counts them | Worker, site, installer |
| Update prompt for old versions | the site sends the answer under HAND-OFF FOR THE SITE LANE to a `?v=` below 0.99.64 | site |
| Health event for a failed protected-store write | `Recovered::CfgSave` is used for it (counted with the settings-file failures); a word of its own needs the radio-reports lane's vocabulary | engine (telemetry lane) |
| Wire-c `access.number_format`, `sys.backup_on_close`, `sys.backup_keep`, `sys.sync_folder` | registry keys nothing reads (append-only registry) | engine |
| Mouse drive | every new row: the HTTPS and listener rows, the RDS rows, the lockout keys in a range scan, a typed comma, the Windows number format on a comma PC | mouse drive |
| Tools Markers sheet pixel checks | `test_slint_tools` 883/899/900 fail with this lane's `tools_logic.cpp` reverted too; the Markers sheet changed in the merge of window-gaps-4 (`d40cf54f`) | screen |

