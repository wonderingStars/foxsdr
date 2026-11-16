# Companions shelf: the Companion applications shelf, the check before a companion runs, the Store package (wave 8, 2026-11-16)

Branch `feat/companions-shelf`, cut from the merged engine tip `d4f855c5` in worktree `D:/foxsdr-wt/en-sigmeas` (the tree was clean; the
old branch `feat/window-gaps-3` was left where it was), build folder `D:/foxsdr-build/en-sigmeas` (Slint ON, Release, soft-GL from the local
archive), scratch `D:/foxsdr-build/cs-scratch/`, hidden desktop `FoxSDRShelf`. Nothing is pushed, published, submitted or downloaded. The real
`%APPDATA%/foxsdr/config.json` sha256 at the start and at the end: `c21ce98dc30e50327f7666f7ee29047c0983d130ca3f21716d0c0e20e04603c3`.
Read first: `common.md`, `companions-shelf.md` (the lane), `STANDARD-RULES.md`, `OWNER-ANSWERS-2026-11-16.md` (decision 3 CHANGED: the Store copy
carries the programs inside its package; decisions 8, 21, 22, 25), `errpath-gate.md`, `store-bundle-licences.md` (section 5 is part of the
requirements), the plugins repository's `companion-plugins.md` section 8 (READ ONLY) and `window-gaps-3.md`.

The base built in this tree before any change: `cmake --build` of the whole tree, log `D:/foxsdr-build/cs-scratch/build0.log`, no error line.
`engine-merge-6.md` did not exist yet.

## What a companion is, and how the pieces fit

A companion application is another project's open-source decoder program (GPL) that a small bridge plugin starts as a separate process and
talks to over a pipe. The plugins repository's catalogue has a `companions` shelf of 18 rows; each row has a `companion` object: upstream,
licence, why it is separate, source address, and `program` (file, url, sha256, status). The bridge looks for its program in the folder the user
named, its own folder, and a `helpers` folder under it (`helper_bridge.cpp findHelperFile`); this lane puts the verified program in
`<plugins folder>/helpers/`. All new core code is in one unit, `src/core/companion_programs.{hpp,cpp}`.

## The rows

| Step | Row | Verdict | Proof |
|---|---|---|---|
| 1 | The "Companion applications" shelf in the store, from the catalogue's `companions` category | BUILT | `fact.store.<key>.companion.*` (engine_api_lists.cpp); the Browse grid has the shelf (after MAPS AND TOOLS, before OTHER) with its one-sentence note; only a row that carries a companion object is on it (`test_slint_host_extend` companionShelf; drives `companions-website-browse`, `companions-package-browse`). |
| 1 | Each entry shows, before anything is fetched: what it decodes, the upstream, the licence as the row states it, why it is separate, the "cannot hide identifiers" line where the row has one, a source address | BUILT | the page block COMPANION APPLICATION (always open): WHAT IT DECODES, UPSTREAM PROJECT, PROGRAM LICENCE, WHY IT IS A SEPARATE PROGRAM, IDENTIFIERS (only when the row has the line), SOURCE CODE, PROGRAM. Drive `companions-website-page` reads the model lines and the drawn controls by label (`UPSTREAM PROJECT: multimon-ng`, ...). The card carries the licence and a state badge. The line is cut out of the row's description (`identifierSentence`): the catalogue has no field of its own for it. |
| 1 | A link to its source | BUILT as selectable text | The address is a read-only text field (the store's existing way to show an address: no click opens anything, so no test can raise a browser). A "not stated" line when the row has none. Click-to-open is an OPEN row. |
| 1 | Installing a companion installs its bridge AND obtains its program | BUILT | `PluginRepo::install` stages and verifies the program, moves it into `helpers/`, records it, then moves the bridge in; any failure puts everything back (`test_companion_programs` C3, C4; `test_engine_api_companions` E3 through the real engine). |
| 1 | A row whose program address is empty or unpublished shows as not available, in plain words, no promise of when | BUILT | badge NOT AVAILABLE, the key greyed with "the program for this companion is not available in this copy of FoxSDR", the page says the same; ADD ALL passes it over for the same reason; nothing is fetched (E2: zero requests). No promise word in any of these sentences (checked by two tests). |
| 1 | Every state has a drive script against a stand-in catalogue | BUILT: three of the five states driven, two proved without the window | Drives (the states unavailable, download and package, in the website copy and in a fake Store package): website browse, website page, website unavailable, package browse, package page, package unavailable (all six Passed, hidden desktop). The states `installed` and `changed` are proved by `test_slint_host_extend` (badges, sentences), `test_companion_programs` C9 and `test_engine_api_companions` E3, but NOT driven: the drive harness copies only DLLs and the two catalogue files into a scratch profile, so a record and a helpers folder cannot be fitted (OPEN row). |
| 2 | A program runs only if its bytes match the SHA-256 the catalogue gave: checked at download | BUILT | `PluginRepo::fetchVerifiedFile` (existing) inside `companion::Txn::stage`; a wrong hash is "failed its integrity check and was discarded", nothing is left (C4). |
| 2 | ...and again each time before it is started | BUILT | `PluginRunner::buildPluginLocked` asks `companion::checkBeforeStart` before any instance is made, which is when the bridge starts its program. Swapped, truncated, deleted, a folder in its place, or recorded with no checksum: no instance, a plain sentence on the plugin's row ("... is not the file that was installed: it has changed on disk, so it was not started."), counted (`test_companion_programs` C5, `test_companion_runner` R1 to R5). |
| 2 | The refusal is reported through the existing companion fault path | BUILT; filing is held back by the switch | `companion::noteHostFault(CASCADE_FAULT_BREAK)`, drained by `companion::drain` with the plugins' own sums into `companion::file`, which is held back while `kCompanionFaultReportsOn` is false (the switch stays OFF, as briefed). Nothing but the kind and a count is kept. |
| 2 | What protects the hash, and what does not | WRITTEN (below), check BUILT and INERT | see "The trust statement". `companion.program.signature`, an envelope over `foxsdr-companion-program v1 / id / file / sha256` checked against `sys::ownerTrustedKeys()`, which is empty until the owner fills in `kOwnerKeyHex`. C7: no key, any signature: not enforced; a key: missing, wrong key, wrong file and garbage are each refused with the program never requested; the right one installs. |
| 2 | Tests: right hash runs; wrong hash refused; file swapped after install refused; an empty hash never runs anything; a break-it for each | BUILT | C2, C4, C5, R2 to R3; break-its below. |
| 3 | The Store copy carries them inside its package; the shelf shows what the package holds as already present | BUILT | state `package` / badge INCLUDED IN THIS COPY, "The program is included in this copy of FoxSDR. Fitting the plugin puts it in place; nothing is downloaded." Driven with a fake package (`FOXSDR_FAKE_PACKAGE` + the new seam `FOXSDR_FAKE_PACKAGE_COMPANIONS`, honoured only while a fake package is in force). |
| 3 | A companion's program is looked for INSIDE the package first | BUILT | the packaged path never consults the catalogue's address: `Txn::stage` reads `<package root>/companions/companions.json`, checks the package's program against its own hash, copies it to `helpers/` and re-checks the copy. In a packaged copy `checkBeforeStart` also puts a swapped or deleted helpers copy right from the package, never from the network (C6). The package, not the catalogue, decides: a row the catalogue calls unpublished is still installable when the package holds it (C6), and a published row the package lacks is NOT AVAILABLE (drive `companions-package-browse`). |
| 3 | The Store copy never downloads a program; a test that the packaged path performs no download call at all | BUILT | C6: a counting transport; zero requests for the program on install, on a refusal, and on a missing list; `test_engine_api_companions` E4 through the engine (`programHits` unchanged); C11 with the 18 real programs: exactly one request per bridge. Break-it: the packaged branch disabled -> 19 red. |
| 4 | `build-msix.ps1` packs the programs with licence texts, notices and statement of changes | BUILT | `-CompanionsDir`; real run on the 18 programs built from the public copy (4089d4c): 18 programs, 52 legal files, `companions/companions.json` (every hash and size re-checked against the package contents: 0 mismatches), `COMPANIONS.md`, no `hfserial` or `dvframes` (refused by name in both scripts). |
| 4 | `COMPANIONS.md` at the package root lists each program, its licence and the address of its source | BUILT | read back from the package. |
| 4 | FoxSDR's own licence terms must say the companions are under their own licences: a DRAFT in the record, marked as awaiting approval, put where the package's licence text lives | BUILT, draft below | appended to the package's `LICENSE.txt` at build time from `installer/msix/companions-licence-draft.txt` with the source address and the commit filled in; the repository's own `LICENSE` is untouched. |
| 4 | Measure the package size before and after | MEASURED | 36,105,786 bytes without, 41,640,277 bytes with the 18 programs and their 52 legal files: +5,534,491 bytes (+15.3 %); the programs are 14 MB uncompressed (js8 7.7 MB). The package is the tree's classic `cascade.exe` build (the script's existing payload), built unsigned. |
| 4 | Built unsigned or with the development certificate as today, never submitted; contents verified by listing | DONE unsigned | no `-TestSign`, so no certificate store was touched and `signtool` was not run; the package was listed through its zip directory. |
| 5 | The website copy downloads the program to the place the bridge plugins look, with the same progress, cancel and failure sentences the store has for a plugin; nothing is run that fails step 2 | BUILT | the program goes through `PluginRepo::fetchVerifiedFile` with the store's own progress and cancel flags; a failure reads `"<file>": connection reset`, `failed its integrity check and was discarded`, `cancelled`, exactly as a plugin's. A stand-in catalogue and a stand-in transport (the store's own `setTransportForTest`): nothing leaves the PC. A program that fails the check is never moved into place, never recorded and never started. |
| (own) | Companion rows from a plugin SITE | RULED OUT by the store's existing rule | a site's entry never carries a program (`mergeSite` clears it; `stage` ignores it). Tested (C4). |
| (own) | Chaining ("Send to") | NOT BUILT, as briefed (decision 8) | nothing for it is built or shown; the shelf and the page carry no such word. |

## The trust statement (what protects the program's hash, and what does not)

* **Today.** The catalogue is fetched over https from `raw.githubusercontent.com/wonderingStars/foxsdr-plugins/master/index.json` with the
  operating system's certificate checks left at their defaults (no pinning, `plugin_repo.cpp`). The catalogue itself is NOT signed. The program's
  hash and address come from it.
* **That protects against:** a damaged or truncated download, a wrong file served from the program's address, a program swapped on disk after
  install (checked again before every start), and a local file replaced by another with the same name.
* **That does not protect against:** anyone who can alter the catalogue in transit (an intercepting proxy whose certificate this PC trusts) or at
  its source (the account or repository that serves it): they can name another program and its hash, and the hash then proves only that the
  download is the file the catalogue named, not that the owner approved it. The plugin DLLs of the catalogue stand on exactly the same footing today
  (and run inside FoxSDR's own process, which is worse); the program check adds nothing to that and takes nothing from it.
* **What closes it: the owner's key (decision 2).** `companion.program.signature` and `sys::ownerTrustedKeys()` are built and tested, and INERT: the key
  list is empty (`kOwnerKeyHex` in `sys_manifest_sig.cpp`; the update manifest's check is inert for the same reason). With a key, a row without a valid
  signature over its own id, file and hash is refused before any request for the program. The Store copy needs none of this: its programs are in a
  package the Store signs.
* **A restart inside one running instance** (the bridge restarting its program after a crash) is the bridge's own and the host does not see it; the
  bridge checks the program's size and time every two seconds and stops with "broken" when they change (plugins repository, `helper_bridge`). The host's
  check is before every instance is made (a rebuild, a retune that rebuilds, a woken plugin).

## New engine commands

None. The install operation, the consent-free tick of the maker's notice and the store facts are the existing ones. New FACTS only:
`fact.store.<key>.companion` ("1" or absent), `.companion.upstream`, `.licence`, `.why`, `.source`, `.hide`, `.program`, `.state`
(`unavailable | package | download | installed | changed`). New record file `companions.json` beside `installed.json` (id, program file, SHA-256, size, via),
and in a package `companions/companions.json`.

## SHARED FILE EDITS (all small; nothing in `main.slint`, `main.cpp`, either `CMakeLists.txt`, or the plugins repository)

| File | Edit |
|---|---|
| `src/core/plugin_repo_data.hpp` | `ProgramInfo`, `CompanionInfo`, `PluginCatalogEntry::companion` |
| `src/core/plugin_repo.cpp` | one parse call; one stage/commit/record block in `install`; one removal call in `remove`; `mergeSite` clears a site entry's program |
| `src/core/plugin_runner.cpp` | the check before a bridge makes an instance (one block at the top of `buildPluginLocked`) |
| `src/core/companion_fault.cpp` | `drain` adds what the host refused (one line) |
| `src/engine/engine.cpp` | one rule in `pluginInstallBlockedReason` (the unavailable program) |
| `src/engine/engine_api_lists.cpp` | the `companion.*` facts |
| `src/slint/ui/plugin_body.slint`, `extend_bank.slint`, `extend_logic.{hpp,cpp}`, `extend_publish.cpp` | the shelf, the cards, the page block (all in the lane's own files) |
| `tests/errpath_allow.tsv` | 22 ALLOW lines (below) |
| `resources/lang/*.json` (33), `src/core/lang_assets.hpp` | 21 new keys, regenerated |
| `src/slint/tests/test_slint_host_extend.cpp` | the shelf test and the expectation change below |

## Expectation changed

`test_slint_host_extend` `everySentenceIsADrawers` required every `tr`/`T` sentence of `extend_logic.cpp` to appear in the Legacy window's drawers
(the logic adds no key of its own). The shelf exists only in the new window, so its 20 sentences have no Legacy drawer: before, 20 sentences were
"said by no drawer" (the check `missing == 0` would fail); now they are listed in the test (`kNewInTheNewWindow`) and `missing == 0` holds. A new
sentence outside that list still fails the test.

## Error-path gate

`test_error_paths`: the pinned PENDING number is unchanged (1126 before, 1126 after; the pin lives in `tests/test_error_paths.cpp`). 22 new ALLOW lines, each
for a sentence that is an install or removal step's error return (the engine files the failed install, `notePluginInstallFailed`) or the refusal that is
counted on the companion fault path (`companion_programs.cpp::checkBeforeStart`).

## New strings (21; machine translations in all 33 catalogues, marked machine as the catalogues are; not checked by a speaker)

COMPANION APPLICATION; COMPANION APPLICATIONS; DOWNLOADS WHEN FITTED; IDENTIFIERS; INCLUDED IN THIS COPY; NOT AVAILABLE; PROGRAM; PROGRAM CHANGED;
PROGRAM INSTALLED; PROGRAM LICENCE; SOURCE CODE; UPSTREAM PROJECT; WHAT IT DECODES; WHY IT IS A SEPARATE PROGRAM; the shelf note ("Separate open-source
programs that FoxSDR starts and shows the output of. ..."); five state sentences ("The program is included in this copy of FoxSDR. ...", "Fitting the plugin
downloads the program and checks it against the checksum ...", "The program is installed and is checked again every time before it starts.", "The installed
program is missing or is not the file that was installed. ...", "The program for this companion is not available in this copy of FoxSDR."); the store's stored
reason "the program for this companion is not available in this copy of FoxSDR". `tools/i18n_keys.py`: 3878 keys, 0 empty, 0 missing, 0 stale in each of
the 33. The words of the program's own refusals ("... has changed on disk, so it was not started.") are the engine's English sentences on the plugin's row,
like every other install and start sentence of the core.

## Break-it checks

| Mutation | Red | Restored |
|---|---|---|
| a program's hash always accepted | `test_companion_programs` 10 failed | 481 checks, 0 failed |
| the packaged branch ignored (the program is downloaded in a package) | 19 failed | green |
| an empty or malformed expected hash passes | 5 failed | green |
| a missing signature accepted when a key is present | 4 failed | green |
| `PluginRunner` does not ask the check | `test_companion_runner` 12 of 23 failed | 23, 0 failed |
| the engine's "program not available" rule off | `test_engine_api_companions` 15 failed | 51 checks, 0 failed |
| the companion block not drawn, and the package state ignored | four of the six drive scripts red (package browse, page, unavailable; website page) | six Passed |

## The draft licence terms (NOT legal advice; awaiting the owner's approval; also in the package's `LICENSE.txt`)

> COMPANION APPLICATIONS. This package also holds separate programs, the companion applications, listed in COMPANIONS.md in the package's root folder. Each companion application is a different project's open-source program under its own licence. The licence of each one is named next to it in COMPANIONS.md and its full text is in the program's own folder, under companions\<name>\legal.
> 1. FoxSDR's licence (the text above this section) does not apply to a companion application, and a companion application's licence does not apply to FoxSDR.
> 2. Nothing in FoxSDR's terms, and nothing in the terms under which the Microsoft Store supplies FoxSDR, limits the rights that a companion application's own licence gives you, including the right to receive its source code, to change it, and to pass it on under that licence.
> 3. The source code of each companion application, matching the program in this package, is published at https://github.com/wonderingStars/foxsdr-companions (commit 4089d4c) and stays available there for as long as this package is offered.
> 4. FoxSDR starts a companion application as a separate program and exchanges data with it through a pipe. A companion application is not combined with FoxSDR into one program.

The Store shows the licence the developer gives it at submission (App Developer Agreement 4(i)); the same text has to be given there too, or Microsoft's standard terms
apply (`store-bundle-licences.md` section 4). That is the owner's step, with the questions in `store-bundle-licences.md` section 6.

## store-bundle-licences.md section 5, point by point

| Checklist item | State |
|---|---|
| a copy of the full licence text for each program in its own folder (GPL 2, GPL 3, LGPL 2.1 for libfec) | DONE: `companions/<folder>/legal/...` (hfdl has `src/libfec/COPYING.LIB`); the build refuses a folder with none |
| each program's notices and the third-party notices (MIT libacars, liquid-dsp; BSD-2 asn1c; ...) | DONE: every file whose name says licence, copying, notice, third-party, trademark, additional-terms or upstream is copied from the public copy |
| a change notice with a date for every program changed from upstream | DONE through `UPSTREAM.md` (the project, the commit, the licence and the changes made, as the public copy states them); the build refuses a folder without it |
| the two OnAir programs: `ADDITIONAL-TERMS.md`, OnAir's NOTICE, THIRD_PARTY, TRADEMARKS | DONE (the build refuses a package without the terms file); the credit line must also be in the program's legal notices: the programs print it themselves (`--version`, per the public copy's own statement, NOT re-checked here) |
| hamdrm: QSSTV's COPYING and a "based on QSSTV" statement | COPYING DONE (the build refuses without it); the statement is printed by the program (public copy's statement, NOT re-checked here) |
| a legal-notices screen or file in FoxSDR that lists each companion, its licence and its source | FILE DONE (`COMPANIONS.md`); the store page shows the same for every companion; a legal-notices SCREEN is not built (OPEN) |
| the source offer: complete source inside the package, a written offer, or the designated place | the DESIGNATED-PLACE route (GPL 3 section 6(d)) is what `COMPANIONS.md` and the draft terms say; the owner chooses |
| the Corresponding Source of the exact build, public, matching, kept as long as the binaries are offered | the programs were built from commit 4089d4c of the public copy; the repository is public (OWNER-ANSWERS 107) |
| each shipped release matched to a tagged commit | NOT DONE: no tag exists; an owner/orchestrator step |
| the description wording and Store Policies 10.2.4 | the owner's listing text; not built |

## Gate

Build folder `D:/foxsdr-build/en-sigmeas`, Release, hidden desktop `FoxSDRShelf`, `FOXSDR_FRAME_CAP_HZ=60`, a shared and slow machine. Final tree = commit `e53f5737` plus this record. Whole-tree build: exit 0.

* This lane's own tests, all Passed: `test_companion_programs` (481 checks with `FOXSDR_COMPANIONS_COLLECTED` set to the 18 real programs; without it C11 says SKIPPED), `test_companion_runner` (23), `test_engine_api_companions` (51), `test_slint_host_extend` (480 + the shelf), the six `slint_drive_companions-*`, `test_error_paths` (pin unchanged), `test_i18n` (3878 keys, 33 catalogues), `test_slint_house_rules`, `slint_drive_ledger --check` (2379 rows, 0 unclassified).
* `ctest -L "headless|slint" -j3 --timeout 300` (1169 tests, 2 skipped: test_plugin_abi3_compat, test_rtlsdr_live: no hardware): `99% tests passed, 10 tests failed out of 1169`, 2587 s. Re-run alone, with `-j1`: test_slint_fitted, test_engine_api_zoom, test_kiwisdr_client, test_transmit_key_race, test_web_server PASSED (load flakes). The five that stayed red:
  * `test_slint_tools` (7 checks, the Markers sheet's pinned geometry): a BASE red, not this lane's. The unchanged build `D:/foxsdr-build/mr-dsp` (merge 6's) passes it (371 checks, 0 failed) and merge 6's commit `01a132b5` ("test_slint_tools: the Markers sheet's pinned geometry follows window gaps 1's widening") is not in `d4f855c5`; this lane touches no TOOLS file.
  * `test_health_app_stepped`: a BASE red (the same `bindOrAbsentCount` failure window gaps 3 recorded in the unchanged build); merge 6's `55adafe9` fixes it.
  * `test_pass_control`: timing assertions (`longest <= 24.0`, passes per second 50 to 70) failed alone while the machine was loaded; it Passed on re-run (see the stamp line).
  * `telemetry_worker`: the fixture manifest hashes (`telemetry-worker/radio-reports.test.mjs`, "fixture directory matches its manifest"); this lane touches nothing under `telemetry-worker`. NOT proved to fail on the base (not run there).
  * `test_diagnostics_free`: the build-stamp test (the binary's commit is not the tree's), the known stale-stamp family: Passed after the rebuild at the final tree (stamp line below).
* `ctest -L gui -j3 --timeout 600` (390 tests): `97% tests passed, 10 tests failed out of 390`, 3592 s. Re-run alone: slint_drive_lanel-scan, slint_drive_signal-recorder, slint_drive_system-look, slint_drive_wg3-store-page, test_health_app, test_slow_frames_app, test_tx_audio_plate_app PASSED (load). Still red alone: `slint_drive_layout-section-overlap-1600` and `-624` (BASE reds: merge 6's `55adafe9` "the section-overlap drives follow the Signal strength bar" is not in this base) and `test_diagnostics` (the stamp, below).
* Stamp tests after a rebuild at the final tree (commit `668e87cc` plus this line; `src/core/version.cpp` touched so the stamp object really recompiled, whole tree rebuilt, exit 0): `test_diagnostics` and `test_diagnostics_free` Passed. `test_pass_control` also Passed when re-run alone on the later, quieter machine, so that red was load.
* No red is this lane's.

## UNVERIFIED

* An installed, signed MSIX run: the package was built and its contents listed; it was NOT installed (that needs a certificate in a Windows store, which a lane may not touch) and a companion was never started from it. Whether Windows lets the packaged app start an executable from its own package folder (it is a full-trust Win32 app, which can) is untried.
* A real download of a program from a real address (no network was used). The address the catalogue will carry matters: **the store follows a redirect only within the same host and port** (`plugin_repo.cpp`, by design), and a GitHub release asset (`github.com/.../releases/download/...`) redirects to another host, so a release-area address would be refused by the store. A file served by `raw.githubusercontent.com` answers directly. This must be settled before the program rows are published (decision 22 says "release area").
* The look of the shelf and the page beyond the drive screenshots (`companions-*.png` under the drive output): no real pointer was used.
* That a real companion started from `helpers/` by its bridge DLL (a real plugin run through the real host) still passes the bridge's own checks; the bridge was not run here.
* The 21 translations were not read by a speaker.

## OPEN ROWS FOR THE NEXT LANE

| Id | What is missing | Lane kind |
|---|---|---|
| Real MSIX install | install the package with the development certificate, start FoxSDR from it, fit a companion and watch it start from `helpers/` (needs the certificate store, which no lane may use) | owner, with a mouse drive |
| Program address | decide where the programs are served (a same-host address, or the store gains a narrow rule for GitHub's asset host); then publish the 18 program rows (address, hash, status) in the plugins repository | owner + plugins lane |
| The owner's signing key | fill `kOwnerKeyHex` (and decide whether an unsigned row is refused once a key exists: today a key makes a missing signature refuse the download) | owner |
| Tag of the exact build | tag commit 4089d4c (or the commit of the build that ships) in the public repository so source and binary match | orchestrator |
| In-app legal-notices screen | the checklist's screen listing each companion, its licence and source (the package file and the store page exist) | screen |
| Source link click-to-open | the address is selectable text only; a button that opens it needs the real-pointer drive (it would raise the browser) | screen + mouse drive |
| `installed` and `changed` drives | the drive harness copies only DLLs and the two catalogue files; add `companions.json` and a helpers folder to the fixture plugins folder (a six-line change in `test_slint_drive_run.cpp`) and drive them | screen (harness) |
| GET EVERYTHING and companions | the bulk key includes every companion whose program is available (it would download up to 18 programs after one tick of the combined notices); the owner may want them left out (a flag in `add_all_plan.hpp`, both callers) | owner decision |
| Companion fault reports | the host's refusal is counted and filed only when `kCompanionFaultReportsOn` is true, after the telemetry Worker lists the four words | engine, after the Worker deploy |
| Catalogue "MIT" label (decision 7) | the bridge rows say licence MIT; the program's own licence is shown beside it now, but the label wording is the owner's | owner |
| Licence terms in the Store listing (ADA 4(i)) | the same text as the draft, delivered at submission; questions 1 to 14 of `store-bundle-licences.md` section 6 for a qualified person | owner |
| OnAir credit and QSSTV statement | confirm each program prints them (the public copy says so), and that FoxSDR's About needs nothing more | owner / lawyer |
| Linux and macOS | `validProgramName` and the package path are Windows-shaped (`.exe`); the catalogue rows are Windows x64 only | none until a Linux row exists |
