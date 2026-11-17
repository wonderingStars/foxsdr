# Must-fix lane E: the report contract, the Worker and the website agree with the application (nothing deployed)

Branch `feat/mustfix-e` cut from `3dc4637d` (the tip of `feat/mustfix-b`), worktree `D:/foxsdr-wt/en-sysmodels`, build folder `D:/foxsdr-build/en-sysmodels`
(incremental, only the targets named below, `--parallel 4`), scratch `D:/foxsdr-build/mfe-scratch/`. Website repository `C:/Users/steve/foxsdr-site`, branch
`site-lane-4` cut from `site-lane-3` at `0aaa83a` (its record is PART 9 of `HANDOVER-daylight.md`). Written 2026-11-17. NOTHING IS PUSHED, DEPLOYED OR SENT; no outside request
was made; no radio was opened; the app's `kRadioReportsLiveSend` and `kCompanionFaultReportsOn` are still false; `installer/cascade.iss` is untouched (the one-semicolon change
is NOT made: the script cannot be compiled here).

## The 17 contract issues, one answer each (made in the contract, the Worker, the site and, where the app's word changed, the app, in the same lane)

| # | Issue (record) | The one answer | Proof |
|---|---|---|---|
| 1 | ring lines carry a level word; two-word prefixes (app 1) | the contract is right and the app already conforms: the sender removes the level word and leaves two-word prefixes out; stated in contract 2.1 | fixtures `refuse-ring-line-with-level-word`, `refuse-ring-line-two-word-prefix` refused by the Worker (`bad-field`) and the site |
| 2 | no kind for a session that ended with no radio (app 2) | `session_end` ADDED (table, Worker, site: counted on the install, listed on its page, no phantom radio) | `valid-session-end-*`, `refuse-session-end-*`; the engine test now sends the real message through the Worker's validator (it was refused `unknown-kind` before); site `TestASessionThatEndedWithNoRadio...` |
| 3 | test and CI runs counted as users (app 3) | envelope member `synthetic: 1`; the Worker (and the site) DROP it after checking it, on usage records, beats and every radio kind; the app sets it from `syntheticRun()` on every message | `drop-*` fixtures (6); `radio-reports.test.mjs` 3 tests + 3 break-its; `test_radio_reporter` (every kind, anonymous form); site `TestAMessageMarkedSynthetic...` |
| 4 | `dropped` cannot be said without a radio key (app 4) | allowed on `session_env`, `session_end`, `session_fault`; the app counts keyless losses under the empty key and says them on the next keyless message | `valid-session-end-with-dropped`, `valid-session-env-with-dropped`; `test_radio_reporter` "keyless" block |
| 5 | `seq` order is not time order (app 5) | a page orders by `t` and arrival minus `age`, never `seq`; stated in 3.9 (the site's views were read: they order by event time) | contract only; no site view orders by `seq` |
| 6 | "serial port COM#" becomes "serial <stripped> COM#" (app 6) | accepted as harmless, stated in 2.1 | existing valid fixture `valid-radio-fail-serial-port-word` |
| 7 | band edges are the app's (app 7) | the edges are written into 3.3 | contract only |
| 8 | the `bad id` literal was read from the contract (app 8) | read from the Worker's own source (`new Response('bad id', { status: 400 })`) and the kept 0.99.95 copy; stated in 11 | existing test "answers every kind 400 bad id" (its list of kinds changed, below) |
| 9 | `refused` has no `frequency`, nothing for other controls (app-2 9) | `frequency` and `other` added; the app now sends `refused` for EVERY control | `valid-radio-settings-refused-frequency/-other`; `test_radio_events`, `test_fobos_source`, `test_mustfix_b` |
| 10 | refusal of the radio versus a typed out-of-range value (app-2 10) | `refusedClass` (`radio`, `range`) added; the app always sends it | `valid-radio-settings-refused-range`, `refuse-settings-refused-class-unknown`; `test_radio_events` |
| 11 | `what` words; `pllunlocked` sent as `gave-up` (app-2 11) | the seven words listed in 3.6; phase `degraded` added, the app sends it (reasonClass `tuner`, not urgent) for `pllunlocked`; the site shows it orange and does not alert | `valid-radio-recovery-degraded`, `refuse-recovery-phase-unknown`; `test_radio_events` PLL block; site `TestADegradedRecovery...` |
| 12 | a service state with no radio has nowhere to go (app-2 12) | `session_fault` ADDED (no radio key); the app sends it when no radio of that driver is open (two open stays ambiguous and dropped) | `valid-session-fault-service`, `refuse-session-fault-*`; `test_radio_events` "servicefault" block; site `TestAServiceFaultWithNoRadio...` |
| 13 | `dropped` also counts folded repeats (app-2 13) | the wording is "not sent"; the site's text now says so everywhere it said "dropped by the daily bound" | site `TestTheSiteSaysNotSent...` |
| 14 | `firmware` for the Airspy HF+ only (app-2 14) | stated in 2.1; nothing changes | contract only |
| 15 | no class for a USB transfer failure (b 15) | `transfer` added; the app stops sending it as `control` (`health::radioReportClassWord`; the usage record's closed health vocabulary is unchanged and still says `control`) | `valid-radio-fail-class-transfer`; `test_radio_events` class checks |
| 16 | no class for an RTL-SDR tuner/demodulator fault (b 16) | `tuner` added, same mechanism | `valid-radio-fail-class-tuner`; `test_radio_events` |
| 17 | a close with no open, the HackRF sweeper (b 17) | the Worker and the site accept it and show the radio red ("closed by the radio ending"); no open is invented | `valid-radio-close-without-open`; site `TestACloseWithNoOpenIsAcceptedAndShown...` |

An 18th disagreement found on the way and fixed: the SITE's failure-class list lacked `control` (it had eight words, the contract nine), so a failure the app files as `control` (the common
Airspy HF+ "control transfer failed") was refused by the site and accepted by the Worker. The site now reads ONE list, the table's (`radioFailClasses = radioJournalSpec.Lists["reasonClasses"]`).

## Items 2 to 6 of the brief

| Item | Verdict | Proof |
|---|---|---|
| 2a. `install_outcome` from the installer | BUILT in the contract (section 13), the Worker (`validateInstallOutcome`), the site (`installoutcome.go`: a count per UTC day and (outcome, appVersion, windows), the last 100,000 message ids, nothing else; shown on `/admin/installs`); `installer/cascade.iss` UNTOUCHED, the semicolon is noted for the release lane | 14 fixtures (3 valid, 10 refuse, 1 drop); 1 Worker test + 1 break-it; 5 site tests |
| 2b. the four companion fault words | ALREADY ACCEPTED by the Worker (`recovered.companioncrash/stall/break/gaveup` are in its vocabulary, `test_health_events` holds app and Worker equal); the SITE's issue-word list lacked them and ten more: ELEVEN words added | site `TestIssueWordsAreTheApplicationsRecoveredWordsAndTheWorkers` reads the app's `kRecovered` and the Worker's list and requires all three equal, in order |
| 2c. the reporter word for a failing output | NOT BUILT (see below) | |
| 3. stock serial | BUILT (site) | `radiostock.go` + 8 tests, 4 break-its; the list is `00000001` (named), all-zero serials, empty serials, each with its source in the code comment |
| 4. the old-version update answer | BUILT (site) | `update.go` `oldVersionAnswer`; `update_old_prompt_test.go` holds the answer to a Go port of the v0.99.63 reader including its throw on a field of the wrong type (a negative control proves the port throws) |
| 5. the updater's download address | PROVED from the code: `/download/u/<file>` passes every rule (finding below) | `TestTheUpdateAnswersAddressPassesEveryRuleOfAnInstalledUpdater`, `TestTheAddressServesTheSameBytes...` |
| 6. fit to go live | BUILT (site) | contact hidden (page, route, POST, links on every page, security.txt, sitemap); markers removed with a word-level diff; each deploy gate listed in PART 9 |

### 2c, the output-failure word: NOT BUILT, and why
The outputs lanes' own records (`screen-s4-wave8.md`, and `screen-s4b.md` on `feat/screen-s4b-sec`) ask for `noteOutputFailed(OutputKind, FailClass)` with 13 kinds and 5 classes. That is a new
event of the application's CLOSED health vocabulary (`health::vocabulary()` in `src/core/health_events.cpp`), and that vocabulary is held equal, in both directions, to the Worker's
`HEALTH_VOCABULARY` block AND to `PRIVACY.md` by `test_health_events` (114,874 checks): adding the word to the Worker alone turns that test red, and adding it to the application means a new
event, its token function, its bound, the PRIVACY.md list and the site's vocabulary copy (which is itself at 0.99.70, see PART 9 open rows). The output sites are on other branches that are not in this
lane's base, so there is nothing to call it from. The Worker already DROPS an unknown word and answers 204, so nothing breaks while it waits. What it takes: one lane that adds `output_fail.<kind>.<class>`
to all of the application, the Worker, PRIVACY.md and the site copy together, then calls it from the `PENDING:O-outputs` sites.

### Item 5, the updater's download address, in full
The released updater is `src/core/updater.cpp` at tag `v0.99.104` (read with `git show`), with `PluginRepo::fetchVerifiedFile` and `httpsGet` of `src/core/plugin_repo.cpp` at the same tag.
Rules an update address must pass, each with where it is:
1. scheme: `urlIsOnAllowedHost` and `PluginRepo::isHttpsUrl`: the text starts with `https://` (case-sensitive) and is longer than 8 characters. `/download/u/<file>` is under `https://foxsdr.com`: PASSES.
2. host: `urlIsOnAllowedHost`: the text between `https://` and the FIRST `/` is exactly `foxsdr.com` or ends in `.foxsdr.com` (a port or user-info in that text FAILS it: `https://foxsdr.com:443/...` is refused). The answer's host is `foxsdr.com`: PASSES.
3. path: NO rule. `crackHttpsUrl` (WinHttpCrackUrl) takes any path of at most 2,048 characters; the request is a plain GET. `/download/u/foxsdr-setup-0.99.109.exe` is 40 characters: PASSES.
4. file name: NO rule. The updater saves `foxsdr-setup-<manifest version>.exe` from the manifest's `version` (`downloadUpdate`), never from the address; what the path ends in does not matter to it.
5. version: `wellFormedVersion` on the manifest's `version` (one to four dotted numbers of one to nine digits, optional `-suffix`): `0.99.109` PASSES (the site tests the answer's version with a port of the function).
6. hash: `wellFormedSha256` on the manifest (64 hex digits), and `fetchVerifiedFile` hashes the bytes that arrive and refuses a mismatch BEFORE the file is named or run: the site serves the identical file at both paths (tested: same bytes at `/download/u/<name>` and `/download/<name>`), so the digest in the answer is the digest of what arrives. PASSES.
7. the answer: HTTP 200 required; a 3xx is followed ONLY to the same host and port over https (`resolveSameHostRedirect`, at most `kMaxRedirects`); at most 256 MiB, checked against the declared Content-Length and against the bytes received. The site answers 200 directly with `Content-Length` and no redirect (tested): PASSES.
The same check for the OLDEST release that has an updater, v0.56.0 (commit `d5b95a45`, the commit that added `core/updater.cpp`): its `kAllowedHost`, `urlIsOnAllowedHost`, `wellFormedSha256` and the call to `PluginRepo::fetchVerifiedFile` are the same lines (read with `git show d5b95a45:src/core/updater.cpp`); the transport's same-host redirect rule is in the same commit. The path is not read by any release. So the address STANDS; no fallback was needed.
LIMIT OF THE PROOF: this is reading the released code, not running an installed 0.99.104 against the site (no installed copy was run and no outside request was made); the exact WinHTTP behaviour on the path was not exercised.

## The stock-serial list, with where each value came from
`00000001`: "the factory serial of many RTL-SDRs" (`src/core/bias_tee_memory.hpp`), "many ship as 00000001" (`src/core/bias_tee.hpp`), and the RTL2838UHIDIR's `USB\VID_0BDA&PID_2838\00000001` in `src/usb/winusb_device.cpp`; the mustfix-b record row 5 names it
UNVERIFIED on hardware. All-zero serials of any length: "what a board with no serial programmed reports, and the serial EVERY RX888 bootloader reports (`0000000000000000`)" (`bias_tee_memory.hpp`,
`biasTeeArgsNameARadio`). The empty serial: a driver that cannot read one sends `serial: ""` (`radio_report.hpp`). No other value is on the list: an invented list of suspicious serials would split real radios.

## Expectations changed (every one, both values)
| Where | Was | Now | Why |
|---|---|---|---|
| `radio-reports.test.mjs`, manifest test | the manifest split on `\n` only (fails `ENOENT ...json\r` on this CRLF working copy) | split on `\r?\n` | the CRLF manifest test must pass on a CRLF checkout |
| `radio-reports.test.mjs`, "same drivers and reason classes as the failure vocabulary" | report classes equal the health vocabulary's nine | the nine are the first nine, in order, and the two others are exactly `transfer`, `tuner` | issues 15, 16: the health vocabulary is closed and unchanged |
| `radio-reports.test.mjs`, "every kind ... answers 400 bad id" | 9 kinds | 12 (`install_outcome`, `session_end`, `session_fault` added) | new kinds |
| `test_radio_events` word lists | `controlRefusedWord(Frequency)` and `(Other)` empty | `"frequency"` and `"other"` | issue 9 |
| `test_radio_events`, `test_fobos_source`, `test_mustfix_b` | a refused retune has no `refused` field | `refused == "frequency"` (and `refusedClass` `radio` / `range`) | issues 9, 10 |
| `test_radio_events`, service state with no radio | nothing was sent | one `session_fault` (and still no `radio_recovery`) | issue 12 |
| site `radiojournal_test` field table | 7 kinds | 9 (`session_end`, `session_fault`) | new kinds |
| site `contact_test` (15 tests), `seo_test` | the contact page served with the form alone while unverified; 22 sitemap urls | 15 tests use a verified mailbox; the unverified page is 404 (new test); 21 urls while hidden | owner decision 14 |
| site `golive_wording_test`, `radiojournal_test` privacy test | the wording had to carry DRAFT markers and "awaiting approval" | no marker may remain; the two approved passages are pinned by hash; 4 wants that WERE the draft notice are gone | the owner approved it |
| site `issue_bodies_test` | 33 issue words | 44 | eleven words added |

## Tests run (Release, `D:/foxsdr-build/en-sysmodels`; the full headless and GUI gates were NOT started: a merge agent runs them elsewhere)
`node --test` in `telemetry-worker`: 117 pass, 0 fail (107 before). `test_radio_reporter` 227 checks, `test_radio_events` 111, `test_fobos_source` 444, `test_mustfix_b` 112,
`test_radio_reports_engine` 146 (hidden desktop `FoxSDRMFE`; the Worker's validator saw 55 messages, 0 refused; `test_radio_reporter` saw 68, 0 refused), `test_health_events` 114,874,
`test_error_paths` 35 checks (the pin is untouched: no `PENDING` site was added), `test_installer_report` 57, `test_rtlsdr_ifbw`, `test_soapy_settings`, `test_radio_reports_fields`,
`test_radio_reports_never_sent`: all 0 failed. `test_fobos_source` failed once at line 249 (a tone frequency, "load flake") under load and passed three times alone; not caused by this lane.
Break-its: Worker 3 (the marker in the family, the marker on usage and beat, `session_end` removed from the kinds) + 1 (installer report may carry an install id), each RED then restored.

## UNVERIFIED
* Nothing ran against a real radio, the real Worker runtime (no `wrangler`, no workerd), the real Analytics Engine or the live site; the Worker was run by Node only.
* `installer/cascade.iss` and `installer/install-report.iss` were not compiled (no Inno compiler); the installer's message was only held to the contract text and the fixtures.
* The updater-address proof is by reading the released code (above); no installed 0.99.104 or 0.99.63 was run.
* The stock-serial list rests on three comments in the drivers' code, not on hardware.
* `noteServiceFault` reaches the new `session_fault` only through the SDRplay trouble check, which runs when the vendor's API is in the process; whether any install without the SDRplay software ever reaches it was read, not measured.
* The approved privacy wording does NOT mention the installer's report (`PRIVACY-DRAFT-0.99.109-FOR-APPROVAL.md` has no word "installer"): the report stays off until the wording covers it.
* No page was viewed in a browser (the site binary was not started).

## OPEN ROWS FOR THE NEXT LANE
| Row | Kind of lane |
|---|---|
| `output_fail.<kind>.<class>` (item 2c) in the application, Worker, PRIVACY.md and the site together | engine health-events lane |
| The site's health vocabulary copy is the Worker's 0.99.70 list; the Worker has 100 more tokens (`reached` 7, `setup` 48, `control` 14, `recovered` 31). Syncing it makes the published privacy page's list of `recovered` places (13 of 44) disagree with the code: the owner's wording | owner decision, then a site lane |
| Remove the semicolon in `installer/cascade.iss` (`#define ReportInstallOutcome`) after compiling the script and after the privacy wording covers the installer's report | release lane |
| The application's `PRIVACY.md` and `docs/DIAGNOSTICS.md` draft markers (on `feat/radio-reports-app-2`, not in this base) | app lane, same rule: markers only |
| Translate the approved privacy text into the 33 catalogues (machine, labelled; owner answer 6) | site i18n lane |
| The privacy page's older sentences that the approved block replaces ("The reports hold nothing that can be connected to you...", "it collects no personal information from anyone") still stand beside it: removing them is a change the owner has to confirm | owner |
