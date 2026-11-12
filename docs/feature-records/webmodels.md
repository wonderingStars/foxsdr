# Engine lane `webmodels`: standalone models for the web remote and the plugin store

Branch `feat/webmodels` (worktree `D:\foxsdr-wt\en-webmodels`), cut from `engine/multi-radio` d3630d78. Build folder `D:/foxsdr-build/en-webmodels`, configured with
`-DCASCADE_BUILD_SLINT=OFF -DCASCADE_SOFTGL=OFF` and the other arguments of `D:/foxsdr-build/feat-web/CMakeCache.txt` (the generator, the vcpkg toolchain and triple, the
`/DWIN32 /D_WINDOWS /EHsc` flags, the node and bash paths). Nothing pushed, nothing merged. No version string changed (versions: none).
Date 2026-11-11/12. This is a NEW-FILE MODEL LANE: no edit to `engine.cpp`, `pipeline.cpp`, `receiver.hpp`, `demod.cpp`, `plugin_abi.h`, `fox_engine_api.cpp`, the core recorder, `src/net/web_server.cpp` or any `.slint` file. The one existing file edited is `tests/CMakeLists.txt` (seven header names at the END of the fence-probe list).

## Step 0: row table (read before building)

Read first: `D:/foxsdr-build/prep/queued-briefs/unblocked-web.md`; `C:/Users/steve/foxsdr-wt/feat-web/docs/feature-records/web-bookmarks.md` (branch `feat/web-bookmarks`: web mute, live updates over server-sent events, CHIRP and SDR++ bookmark IMPORT into the HOST list; it does not touch per-viewer bookmarks); the files of the running `feat/web-secure` lane in `C:/Users/steve/foxsdr-wt/feat-web` (uncommitted, no record yet at the time of reading): `src/net/web_users.hpp` and its header comment.

**What `feat/web-secure` already does (so it was NOT rebuilt here):** the role system (`Role { None, Owner, Listener }`, `WebRoom`): sign-in as OWNER or LISTENER, listener cap (default 4, ceiling 32), idle and session limits, ban by name and by address, kick, an owner broadcast note, a listener list (names), a listener count, optional chat, the audit trail, and `listenersMayTune` (one server switch lets listeners tune). **What it does not do and this lane built:** an exclusive control mode among several viewers who MAY tune, a request / queue / grant / release / hand-over protocol, admin override and idle hand-back of control, and a connected-viewer list that carries only opaque ids. `net/web_arbiter.hpp` takes `isAdmin` and `mayTune` from the caller (WebRoom's owner and `listenersMayTune`) and decides nothing about roles.

| # | Row (DEPTH-GAPS A2 / REMAINING-GAPS) | Class | What was done |
|---|---|---|---|
| 1 | Waterfall zoom and pan in the browser | new file model, page = needs web_server.cpp (hand-off) | `net/web_viewport`: BUILT, tested. Page JavaScript (ported functions and hooks) in the hand-off |
| 2 | Browser bookmarks: group, mode, search, personal set per viewer | new file model, routes and page = needs web_server.cpp (hand-off) | `net/web_bookmarks`: BUILT, tested. Routes and page controls in the hand-off. Host list unchanged (it is `feat/web-bookmarks`' area) |
| 3 | Recording in the browser and downloading a clip | new file model; the recording = core clip recorder (hand-off); routes and page = web_server.cpp (hand-off) | `net/web_clip`: BUILT, tested (request, path and ownership model). The one host call the recorder must offer and the routes are in the hand-off |
| 4 | Scroll-wheel bandpass and a tuning-step control | needs web_server.cpp (hand-off); page arithmetic over existing control fields, no model | Behaviour written exactly in the hand-off (Shift+wheel = bandwidth 5 % per notch; step list and default). No code, nothing to test here |
| 5 | Named profiles to switch from the browser | new file model; the status field that publishes the names = engine/status (hand-off) | `net/web_profiles`: BUILT, tested. Wiring in the hand-off |
| 6 | Control arbitration (exclusive mode, admin hand-over, connected-user list) | partly already built by `feat/web-secure` (roles, listener list, kick, ban, caps); the rest new file model | `net/web_arbiter`: BUILT, tested, with the split above |
| 7a | CORS allow-list (VRS) | new file model; the pre-routing hook = web_server.cpp (hand-off) | `net/web_cors`: BUILT, tested, rules from the Fetch Standard |
| 7b | UPnP port mapping (VRS) | new file model; OWNER DECISION to wire | `net/upnp_igd`: BUILT, tested ONLY against a stand-in on 127.0.0.1; off by default, not called by anything |
| 8 | Plugin store: roll back a plugin version | by choice (security) | NOT BUILT, reason below |
| 9 | Plugin store: ratings and popularity sort | sort = new file model; collecting ratings = blocked on a server and accounts | `core/store_sort`: sort by name / updated / popularity / rating BUILT; rating COLLECTION not built (listed) |
| 10 | Plugin store: release-note history | new file model | `core/store_sort` `ReleaseNoteHistory`: BUILT. Limitation below |
| 11 | Lock-screen media controls and a screen wake-lock in the browser | needs web_server.cpp (hand-off), docs only | Exact JavaScript and hooks written and run in node (31 checks); facts and the one UNVERIFIED point in the hand-off |

No row was classified `already built` or `blocked` outright; row 8 is `by choice`, and the collection half of row 9 needs a service this lane may not create (see "Not built").

## What was built

All new files; none is called by the application yet. The model header comments are the specification; the tests are the proof.

| File | Purpose |
|---|---|
| `src/net/web_viewport.{hpp,cpp}` | Zoom and pan maths: clamped views, zoom about the pointer, wheel factor, pan by pixels, pointer to Hz and back, visible bin range |
| `src/net/web_bookmarks.{hpp,cpp}` | `WebBookmarkBook`: per-viewer sets keyed by an opaque token, group, mode, tags, search, JSON export and import, caps |
| `src/net/web_clip.{hpp,cpp}` | `WebClipBroker`: clip request limits, one per viewer, path under the recordings root only, sanitised unique names, size cap, ownership of downloads |
| `src/net/web_profiles.{hpp,cpp}` | `WebProfileBook`: validated list of profile names, exact-match switch gate, one in flight, rate limit |
| `src/net/web_arbiter.{hpp,cpp}` | `ControlArbiter`: open and exclusive modes, request queue, release, grant, revoke, admin override, idle and timeout hand-back, opaque ids, epoch |
| `src/net/web_cors.{hpp,cpp}` | `CorsPolicy`: exact-origin allow-list, preflight, credentials rules |
| `src/net/upnp_igd.{hpp,cpp}` | SSDP + IGD description + SOAP parsers, socket transport, `UpnpMapper` |
| `src/core/store_sort.{hpp,cpp}` | `sortStoreEntries`, `dampedRating`, `ReleaseNoteHistory` |
| `tests/test_web_viewport.cpp`, `test_web_bookmarks.cpp`, `test_web_clip.cpp`, `test_web_profiles.cpp`, `test_web_arbiter.cpp`, `test_web_cors.cpp`, `test_upnp_igd.cpp`, `test_store_sort.cpp` | one test program per model, picked up by the `tests/test_*.cpp` glob |
| `docs/webmodels-handoff.md`, `tools/webmodels_handoff_check.js` | the page and server hand-off, and a node check of its JavaScript |

### Decisions made (minor, noted)

* **Admin override** in exclusive mode: an owner may always act (flagged `viaOverride`; it does not refresh the holder's idle clock). Alternative: the owner must also request. Chosen so the person who owns the receiver is never locked out by a guest.
* **Revoke** hands control to the next in the queue (like a release); to leave nobody in control the owner switches to OPEN and back.
* **Rating damping**: `(5 x 3.0 + sum) / (5 + votes)`, so one five-star vote does not outrank a hundred votes at 4.6.
* **Release notes: first seen wins.** A catalogue that rewrites an old version's notes changes nothing (same stance as the no-downgrade rule).
* **Bookmark modes** are the engine's eight names (NFM WFM AM DSB USB CW LSB RAW).
* **Clip length** 1 to 300 s, size cap 64 MiB, one per viewer, four at once, eight kept per viewer. **Switch gap** 2 s, **switch timeout** 15 s.

## Tests (real result lines; build Release, ctest, `--timeout 300`)

| Test | Result line |
|---|---|
| test_web_viewport | `test_web_viewport: 90 checks, 0 failed` |
| test_web_bookmarks | `test_web_bookmarks: 712 checks, 0 failed` |
| test_web_clip | `test_web_clip: 334 checks, 0 failed` (the link-escape check ran through a directory junction; 0 skipped) |
| test_web_profiles | `test_web_profiles: 76 checks, 0 failed` |
| test_web_arbiter | `test_web_arbiter: 298 checks, 0 failed` |
| test_web_cors | `test_web_cors: 153 checks, 0 failed` |
| test_upnp_igd | `test_upnp_igd: 363 checks, 0 failed` (about 5 to 9 s: the stand-in router and the timeouts) |
| test_store_sort | `test_store_sort: 2492 checks, 0 failed` |
| `node tools/webmodels_handoff_check.js` | `webmodels-jscheck: 31 checks, 0 failed` |

Fuzz (in-test, seeded, deterministic): bookmark JSON 6000 mutations + 2000 random inputs; CORS header values 20000 mutations; SSDP / URL / description / SOAP / HTTP parsers 30000 mutations + 5000 random inputs; release-note JSON 8000 mutations + 3000 random inputs. Each asserts no throw, the caps, and the rule the caller relies on (for example: an allowed CORS answer exists only for exactly one origin value that is in the list; an accepted description yields a control path that starts with a single `/`). These are not libFuzzer harnesses (`CASCADE_FUZZ` is off in this build); a corpus harness would need new files under `tests/fuzz/` and the CMake fuzz list, which this lane does not edit.

Identifier hygiene: every per-viewer model has a source-scan test that its `.cpp` contains no `printf`, `cout`, `cerr`, `stderr`, `stdout`, file stream, `settings_set` or `getenv`; the UPnP scan also bans every name lookup (`getaddrinfo`, `gethostbyname`): the client only talks to IPv4 literals.

### Concurrency
Bookmarks (8 threads, 4 sharing a viewer, unique ids), clips (8 threads: in-flight never over 4), profiles (8 threads: two switches never in flight), arbiter (8 threads of random calls with a shared clock plus a watcher thread checking the snapshot invariants: at most one holder, the holder may tune, queue positions 1..n distinct, ids unique, viewers within the cap). Run once each in the gate; the arbiter and profile tests also run with the break-it mutations below.

## Break-it table (remove the behaviour, see the named test RED, restore, see green)

Script `webmodels-breakit.py` (mutates one line, rebuilds only that test, runs it with ctest, restores the file byte for byte and touches it; results in `D:/foxsdr-build/webmodels-breakit.log`). 27 mutations; every one RED except one equivalent mutant.

| # | Behaviour removed | First failing check |
|---|---|---|
| V1 | zoom keeps the bin under the pointer | test_web_viewport (zoom anchor) |
| V2 | single wheel event capped at x4 | test_web_viewport:87 |
| B1 | personal sets (list ignores the viewer) | test_web_bookmarks (isolation) |
| B2 | per-viewer bookmark cap | test_web_bookmarks |
| B3 | mode normalisation | test_web_bookmarks:47 |
| C1 | clip ownership (another viewer's clip downloadable) | test_web_clip:225 |
| C2 | real-path check (link planted in the root) | test_web_clip:328 (the junction case) |
| C3 | one clip per viewer | test_web_clip:192 |
| C4 | reserved device names in a clip name | test_web_clip:58 |
| P1 | one profile switch at a time | test_web_profiles:90 |
| P2 | exact-match of the profile name (falls back to the first) | test_web_profiles:70 |
| A1 | queue promotion | test_web_arbiter:117 (16 checks) |
| A2 | admin check on the mode switch | test_web_arbiter:140 |
| A3 | holder idle hand-back | test_web_arbiter:192 |
| A4 | grant to a listener refused | test_web_arbiter:148 |
| K1 | wildcard refused with credentials | test_web_cors:139 |
| K2 | echo the configured origin, not the request's | **NOT CAUGHT, equivalent mutant**: a request is allowed only when its strict canonical form equals the configured string, so the two strings are always identical; the property is structural (and the fuzz asserts `Allow-Origin == request origin == a listed origin`). Not claimed as tested |
| K3 | default port refused in a request | test_web_cors:65 |
| U1 | only named ports | test_upnp_igd:514 (17 checks) |
| U2 | mapping removed on shutdown | test_upnp_igd:503 |
| U3 | off by default | test_upnp_igd:387 |
| U4 | location must be the host that answered | test_upnp_igd:616 |
| U5 | DOCTYPE refused | first run NOT caught (a later check in the reader refused the document anyway); added a test with a DOCTYPE and no subset, now RED at test_upnp_igd:664 |
| U6 | finite lease | test_upnp_igd:486 (11 checks) |
| S1 | first seen wins (release notes) | first run build error in the mutation (const); fixed, RED at test_store_sort:199 |
| S2 | rating damping | test_store_sort:123 |
| S3 | undated entries last | test_store_sort:92 |
| JS1 | hand-off zoom anchor (doc text) | `tools/webmodels_handoff_check.js`: FAIL "zoom anchor 0.5" |
| JS2 | hand-off wake-lock re-acquire (doc text) | FAIL "visible again: re-acquired" |

After the run every source file equals its committed form (`git status` clean) and all eight tests are green again (lines above).

## Gates

Build: `cmake --build D:/foxsdr-build/en-webmodels --config Release --parallel 3` (all targets), exit 0 (614 targets). Build: `cmake --build D:/foxsdr-build/en-webmodels --config Release --parallel 3` (all targets, 614 projects), exit 0, twice (the second after the rename below).

**Own tests, after the final code:** the eight model tests pass (lines above), `tools/webmodels_handoff_check.js` 31 checks, 0 failed.

**Headless set** (`ctest -C Release -L headless -j3 --timeout 300`, 725 tests, four slices `-I 1,180`, `181,360`, `361,540`, `541,725`; wall times 1355 s, 803 s, 749 s, 1341 s on a machine shared with about twenty other lanes, so each slice is longer than the 10-minute target; every foreground wait was under 10 minutes). First pass, BEFORE the ODR fix below: 687 passed, 38 failed (12 + 10 + 12 + 4 across the slices). Solo re-run of those 38 before the fix: 16 passed, 22 failed, among them **`test_usb_bounded_walk` SEGFAULT**.

**A real defect of this branch, found by that gate and fixed (commit 4bd4328e): an ODR violation.** `cascade::core::ReleaseNote` in `core/store_sort.hpp` has the same qualified name, with a different layout, as `cascade::core::ReleaseNote` in `core/updater.hpp`. Two class definitions with one name make the linker merge the inline special members and `std::vector<ReleaseNote>` instantiations, so one translation unit runs the other's code and memory is corrupted in tests that have nothing to do with this branch. Found by scanning every public type of the eight headers against `src/` (also `net::View`, `net::Decision`, `UrlParts`, which share names with types elsewhere; renamed too, so that a later move cannot collide). Renamed `StoreReleaseNote`, `WaterfallView`, `ControlDecision`, `IgdUrl`; the scan now finds no duplicate type name and no duplicate `inline constexpr` name. After the rebuild `test_usb_bounded_walk: 30 checks, 0 failed`, and `test_bounded_hooks`, `test_excuse_cap`, `test_engine_shell_guard`, `test_plugin_hostile`, `test_sentinel`, `test_server_host`, `test_crash_upload_free` and the other load-sensitive ones passed on the second solo run. LESSON for the next model lane: never put a new type in `cascade::core` / `cascade::net` without grepping `src/` for the name first; the compiler and linker do not report it.

**Solo re-run of the 38 first-pass reds after the fix** (`-j1`, own `--timeout 300`): 26 passed, 12 failed on that run; four of the 12 (failure_files, pass_control, transmit, merge) passed on a later solo run. The 12, classified (after the extra runs below, only the five known false failures and the three configuration cases are still red):

| Test | Class |
|---|---|
| test_airband_data, test_band_plan, test_converter_call_sites, test_page_resize_id, test_png_decode | the five known false failures of a D: build (they find the tree from the working directory); not re-run from the worktree root here |
| test_window_failed_app | configuration: `CASCADE_SOFTGL=OFF` (needs `softgl/opengl32.dll`, `libgallium_wgl.dll`) |
| test_engine_api_source_kinds, test_engine_api_decode_radio | configuration: they need `scope.wav` and the decoder `.wav` files that the slint drive harness generates "at test time" (`src/slint/tests/drive/fixtures/.gitignore`); this worktree has only the generators. Messages: "the fixture scope.wav is not in ..." and `radio.1.source` never "file" |
| test_failure_files, test_pass_control | load flakes: red in the gate and the first solo run, **passed alone on a third run** (66 s, 49 s) |
| test_engine_api_transmit | load flake: red in the gate and two solo runs, **passed alone on a third run** (136 s) |
| test_engine_api_merge | load flake: a large time-judged sweep (it prints `RERUN (a run outlasted the 900 ms the rules judge time by: merged 3210 ms ...)`); red in the gate (239 s), the first solo run (322 s), the second (timeout 600 s) and a third (266 s: `failures 4`, `unclean 14`), then **passed alone on a fourth run in 58.8 s: `5450 cases ... 0 differ, 0 with a missing result or refusal at setup, 2 rerun for a stall`, `4 checks passed, 0 failed`** |

None of the 12 uses a file of this branch (no `web_*`, `upnp_igd`, `store_sort` symbol is referenced by any of them), and the branch changes no existing source file, so the reds are not attributable to a behaviour change; but a clean BASELINE run of the base commit (d3630d78) was not made (a second 614-project build in another folder was judged too costly), so "not caused by this branch" rests on that argument and on the later passes and on the sibling record (`docs/multi-radio-records/engine-features-a.md`: 774 of 779 passed, only the five known false failures red, on a quieter machine).


**Real `%APPDATA%/foxsdr/config.json`:** sha256 `c21ce98dc30e50327f7666f7ee29047c0983d130ca3f21716d0c0e20e04603c3`, read at the start and again at the end (unchanged). No test of this lane reads or writes it; the clip test works in a `foxsdr_web_clip_<pid>_rec` folder under the temp directory and removes it. No Windows prompt was possible: no certificate store, key provider, signtool, firewall-triggering wide bind (every listening socket in the UPnP test is 127.0.0.1) or file dialog is used; the only process spawned is `cmd /c mklink /J` inside the clip test's junction case.

## Sources opened, with licence or status

* UPnP Device Architecture 1.0 (document revision 15 October 2008), https://openconnectivity.org/wp-content/uploads/2015/11/UPnP-arch-DeviceArchitecture-v1.0-20081015.pdf: read sections 1.2.2 (M-SEARCH), 1.2.3 (search response), the SOAP control request / response / UPnPError forms, the description's `serviceList` / `controlURL` / `URLBase`. UPnP Forum / Open Connectivity Foundation document, published free; the text says use by compliant devices is subject to the UPnP Forum membership terms. Only interface facts were taken (message formats, argument names).
* WANIPConnection:1 Service Template Version 1.01 (12 November 2001), https://openconnectivity.org/wp-content/uploads/2015/11/UPnP-gw-WANIPConnection-v1-Service.pdf: AddPortMapping (NewRemoteHost, NewExternalPort, NewProtocol, NewInternalPort, NewInternalClient, NewEnabled, NewPortMappingDescription, NewLeaseDuration), DeletePortMapping (NewRemoteHost, NewExternalPort, NewProtocol), GetExternalIPAddress, error codes 402 501 714 715 716 718 724 725 726 727. "Standardized DCP ... provided AS IS"; the document says the UPnP Forum takes no position on IP rights. The IGD:2 service (ISO/IEC 29341-24-10) was not opened.
* Fetch Standard, https://fetch.spec.whatwg.org/ (character ranges 100000 to 400000 of the page text read: 3.2 Origin header, 4.8 CORS-preflight fetch, 4.9 cache, 4.10 CORS check). The page footer with the licence was not reached in the read; WHATWG standards are published under Creative Commons Attribution 4.0 (from general knowledge, NOT verified here). Nothing was copied; the rules are paraphrased. `Vary: Origin` is HTTP caching practice and is NOT in the Fetch text read.
* MDN, Screen Wake Lock API (https://developer.mozilla.org/en-US/docs/Web/API/Screen_Wake_Lock_API) and Media Session API (https://developer.mozilla.org/en-US/docs/Web/API/Media_Session_API): facts for the hand-off only; MDN did not state the secure-context rule or the HTMLMediaElement requirement for the Media Session page (marked UNVERIFIED in the hand-off).
* Repo files read for facts: `src/core/plugin_repo.hpp` (lines 78-112 VERSION POLICY, 640-660, 795-830 no-downgrade), `src/core/plugin_repo_data.hpp`, `src/net/web_server.cpp` (page code locations, cookie), `src/net/web_policy.hpp`, `tests/CMakeLists.txt`, the `feat-web` worktree's `web_users.hpp` and `web-bookmarks.md`.
* No GPL / LGPL / AGPL source and no other radio program's source was read. No download of software, no account, no outside connection other than the read-only page fetches above (the harness saved the two UPnP PDFs it fetched to its own tool-results folder; they were read with the local `pdftotext`, not run or installed).

## Not built, by name

1. **Roll back a plugin version** (row 8): by choice and the reason is SECURITY. `plugin_repo.hpp:806-812`: "NEVER a downgrade ... A catalogue that has gone backwards is either a mistake or an attack, and neither is a reason to replace a working newer plugin with an older one", and the VERSION POLICY block (lines 78-112): no automatic download of native code ("the shape of the xz and event-stream supply-chain attacks"), and the `minSupportedVersion` floor RETIRES old versions so they are no longer loaded. A rollback is a downgrade path to the versions those rules keep out. Not built. (If wanted later, the safe form is re-enabling a previous file kept locally, never downloading an older build.)
2. **Collecting plugin ratings** (row 9, collection half): needs a server that stores votes and an identity to stop repeat voting (an accounts system or signed install ids, plus a Cloudflare Worker). The sort over ratings is built and waits for data. Popularity data could come from the existing usage telemetry (the usage dataset); that is an owner decision.
3. **Anything in the page or the server routes** (rows 1, 2, 3, 4, 5, 6, 7a, 11): `web_server.cpp` is forbidden to this lane; each has an exact hand-off.
4. Not built because it needs data the catalogue does not carry: a release-note history of versions the install has NOT met. The catalogue has one `whatsNew` for one version; the history can only grow from catalogues this install sees (one entry per version seen). A catalogue field `releases: [{version, published, notes}]` would give the whole history at once (a catalogue data change, then `observe()` would take it).

## UNVERIFIED

* Nothing was run in a real browser: the page code is node-checked against fake objects only.
* UPnP against a REAL router (by rule: never done). The stand-in follows the two documents read; real routers deviate (many answer a permanent-lease-only error 725, some reject an `AddPortMapping` with a description, many put a name in `LOCATION`, which this client refuses by design), so a real router may refuse where the stand-in accepts. IGD:2's `AddAnyPortMapping` is not implemented.
* CORS with a real browser; the credential path additionally needs the cookie change described in the hand-off.
* Whether a lock-screen control needs an `HTMLMediaElement` (MDN silent).
* The core clip recorder's host call (`start_clip`) does not exist yet: the broker was tested with a simulated recorder (`finished()` called by the test).
* No GUI or hidden-desktop test was run (the models have no window).
* Windows-only behaviour: `test_web_clip`'s junction check uses `mklink /J`; on other platforms it tries a symlink and counts a skip if neither can be made.
* Linux build of these files: not compiled here (the code is portable C++20 with the project's own socket guard pattern); UNPROVEN on Linux.

## Wiring and screen hand-off

`docs/webmodels-handoff.md` is the hand-off (routes, JSON, session hooks, page JavaScript). English strings to add (not translated here, so the i18n catalogue is unchanged; the translated-strings check was not run because nothing was added to the catalogue): `Fit`, `Zoom in`, `Zoom out`, `My bookmarks`, `Save here`, `Name`, `Group`, `Tags (comma separated)`, `Search`, `All groups`, `All modes`, `Export`, `Import`, `Replace my list`, `Restore my bookmarks`, `Record clip`, `Length (seconds, 1-300)`, `Recording...`, `Download`, `Step`, `Bandwidth`, `Profile`, `Switch`, `Exclusive control`, `Request control`, `Release control`, `Hand over to...`, `Take control back`, `You have control`, `Waiting (n)`, `Someone else has control`, `Connected: n`, `Allowed web origins (one per line)`, `Allow cross-origin sign-in (needs HTTPS)`, `Open this port on my router automatically (UPnP)`, `Keep screen on`, `Screen stays on`.

| Control | Op or fact | Range | Default |
|---|---|---|---|
| Waterfall zoom / pan | none (client side) | min 16 bins, wheel x1.25 per notch, one event at most x4 | whole row |
| Step selector | none (client side) | 1 Hz .. 100 kHz (list in the hand-off) | 1 kHz |
| Bandwidth wheel | existing `bandwidthHz` | 50 Hz .. mode maximum, 5 % per notch | unchanged |
| Clip | `POST /api/clip`, host `start_clip` (new) | 1 to 300 s, 64 MiB | 10 s |
| Profiles | `POST /api/profile`, status fields `profiles` / `profile` (new) | 32 names of 48 bytes | none |
| Exclusive control | `POST /api/control/mode` etc. (new) | queue 8, viewers 32, idle 60 s, timeout 30 s | OPEN |
| CORS origins | setting `webCorsOrigins` (new) | 32 origins | empty (off) |
| UPnP | setting (new), owner decision | one named port, lease 3600 s | off |

## Owner decisions

1. **Wire UPnP at all?** It opens the home router to the internet. Built and proven only on a stand-in; recommendation: do not wire it before the web page has HTTPS and a password, and only as an opt-in switch that shows the warning.
2. **CORS with credentials**: needs `SameSite=None; Secure` on the session cookie, i.e. HTTPS first. Without credentials a cross-origin page reads only unauthenticated replies. Recommendation: allow the list (no credentials) first.
3. **Admin override in exclusive mode** (the owner may always act): confirm, or require the owner to take control like anyone else.
4. **Popularity source** for the store sort: the existing usage telemetry (aggregate install counts per plugin, published in the catalogue as a new field) or nothing. And whether ratings are wanted at all (server + identity).
5. **Plugin rollback**: not built; confirm that is the intended answer (a security rule), or ask for the local re-enable form.
6. **Bookmark persistence**: per-viewer sets live in server memory and in the viewer's own browser storage (export/import); writing viewer sets to disk on the PC was not chosen because they are keyed by a session token (an identifier). Confirm.
7. The UPnP Forum / OCF terms note ("use in compliant devices subject to membership terms") for shipping an IGD client: take advice if it matters to the release.
