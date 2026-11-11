# Web mute, live updates, CHIRP and SDR++ bookmark import (branch feat/web-bookmarks)

Date 2026-11-11. Worktree foxsdr-wt/feat-web, branch feat/web-bookmarks cut from engine/multi-radio b9208fba, build D:/foxsdr-build/feat-web (configured with `-DCASCADE_BUILD_SLINT=OFF -DCASCADE_SOFTGL=OFF`; SOFTGL off because the default configure downloads Mesa, which this lane may not do). Nothing pushed or merged. Survey rows 33 (mute half), 35, 23b, 23a (`D:/foxsdr-build/prep/second-feature-list-survey.md`).

## What existed (verified in this worktree)

* Start/Stop: button `#playstop`, request field `running` -> `FOXAPI_OP_RUN`. Worked; now proved by `testStartStopAndMuteGoToTheSession`. The MANUAL claimed the page had no Start/Stop and no mute; that was wrong for Start/Stop and is rewritten.
* Mute: read only (`audioMutedBy` badge, MUTE lamp for plugin mutes). The op exists (`FOXAPI_OP_SET_MUTED`, the window's MUTE key), `ControlRequest` had no `muted`.
* Polling: `setInterval` status poll at 250 ms, no `EventSource`.
* Import: `core/freq_import` read SDR# XML and a generic CSV. One content-detecting door, `importFrequencyList`, used by every caller (engine disk work, classic window, Slint tools).
* Bookmark has no tone or notes field: `name, freqHz, mode, bandwidthHz, group, favourite, scan` (src/core/bookmark.hpp).

## What was built

1. **Web mute.** `muted` in the control request (strict boolean), `FOXAPI_OP_SET_MUTED` in `control_ops.cpp`, `RadioStatus::muted` from `FOXAPI_RX_MUTED` (`status_compose.cpp`), `"muted"` in `/api/status`, a Mute/Unmute button (state read back every status, so a desk mute shows), MUTE lamp for either mute. **PARTLY: see HAND-OFF, the engine refuses the op for the web session until one profile line is added.**
2. **Live updates.** `GET /api/events`, server-sent events on httplib's chunked provider. Same cookie gate as every route, session re-checked each look (sign-out, expiry, revoke end the stream), `retry: 3000`, `event: status` + one-line JSON when it changed (first at once), `: keep-alive` after 15 s of silence, a look every 250 ms on a condition variable `stop()` signals, cap `kMaxEventListeners` = 4 (then 503). Not a heartbeat (design below). Page: `showStatus()` shared by poll and push, EventSource with fallback to the fast poll on any error and a retry every 10 s.
3. **CHIRP CSV** (`importChirpCsv`, `looksLikeChirpCsv`, new `src/core/freq_import_formats.cpp`).
4. **SDR++ JSON** (`importSdrppJson`, `looksLikeSdrppJson`, same file). Both chosen by content in `importFrequencyList`.

Design decision worth knowing: the engine judges the remote transmit key's keep-alive from the web session's heartbeat, which is the authorised `GET /api/status` and nothing else (web_session.hpp, api-0.3-additions 8.16, test_web_server `testOnlyTheStatusPollIsTheHeartbeat`), with a 2 s window. A stream held open by a frozen tab must not count as a live page, so `/api/events` does not heartbeat and the page keeps a 500 ms status poll while the stream is up (250 ms without it). The saving is therefore the readout traffic and latency, not the request count.

### CHIRP: columns and mode table

Header taken as CHIRP's: `Location,Name,Frequency,Duplex,Offset,Tone,rToneFreq,cToneFreq,DtcsCode,DtcsPolarity,RxDtcsCode,CrossMode,Mode,TStep,Skip,Power,Comment,URCALL,RPT1CALL,RPT2CALL,DVCODE`. Detected when the first row has `Location` and `Frequency` and at least two more of the CHIRP names (Duplex, Offset, Tone, rToneFreq, cToneFreq, DtcsCode, DtcsPolarity, TStep, Skip, Power, Comment, CrossMode, RxDtcsCode), any case and order. Used: Frequency (MHz; a value of 100000 or more is taken as Hz), Name (empty gives "146.5200 MHz"), Mode, and Tone with rToneFreq / cToneFreq / DtcsCode. Not used: Duplex, Offset (FoxSDR listens, so the receive frequency is the bookmark), Skip, Power, Comment and the rest. Group stays empty, favourite and scan false, bandwidth is the mode's default.

| CHIRP Mode | FoxSDR mode | counted in modeDefaulted |
|---|---|---|
| FM, NFM | NFM | no |
| WFM | WFM | no |
| AM, NAM | AM | no |
| USB, LSB | USB, LSB | no |
| CW, CWR | CW | no |
| empty or column missing | NFM | no |
| anything else (DV, DMR, P25, Auto ...) | NFM | yes |

Tone: no field exists, so it is kept in the name: Tone mode `Tone` -> `Name [CTCSS <rToneFreq> Hz]`, `TSQL` -> `[CTCSS <cToneFreq> Hz]`, `DTCS` -> `[DCS <DtcsCode>]`, `Cross` -> `[CTCSS/DCS cross <rToneFreq> Hz]`; counted in `toned`. Result counters: `entries` (rows read), `skipped` (no usable frequency), `modeDefaulted`, `toned`. Tolerated: quoted fields (commas, doubled quotes, line breaks inside), byte-order mark, CRLF / LF / lone CR, empty rows, a missing optional column, last row without a line break. Refused (error text starts "not a CHIRP CSV file", nothing imported): anything whose first row is not a CHIRP header, and an empty file.

### SDR++: fields and mode map

Read to `lists{<name>:{bookmarks{<name>:{frequency,bandwidth,mode}}}}` only (selectedList, bookmarkDisplayMode, showOnWaterfall are ignored). Each list is a bookmark group (the list name); the bookmark key is the name (empty key gives the frequency). Mode index map **UNVERIFIED**: 0 NFM, 1 WFM, 2 AM, 3 DSB, 4 USB, 5 CW, 6 LSB, 7 RAW; an index outside 0..7, a non-integer or a missing mode is NFM and counted in `modeDefaulted`. A missing or non-positive bandwidth takes the mode's default. Entries with no numeric positive frequency are skipped and counted. Refused: not JSON, JSON without a `lists` object, nothing usable.

## Sources (clean-room)

I read no SDR++, CHIRP or other radio-program source. File formats come from the survey appendix (`second-feature-list-survey.md`, "SDR++ bookmark JSON" and "CHIRP CSV", read by the survey on 2026-11-11), which itself records:

* CHIRP: repository https://github.com/kk7ds/chirp (GPL-3.0, per the appendix; not opened by me); the official CSV spec was NOT found (wiki CHIRP_CSV_Format 404), the header is from a secondary source. I cross-checked the semantics of the tone columns against the CHIRP wiki page "MemoryEditorColumns", https://www.chirpmyradio.com/projects/chirp/wiki/MemoryEditorColumns, fetched 2026-11-11: Frequency is MHz; Tone Mode is None/Tone/TSQL/DTCS/Cross; Tone is the CTCSS tone sent, ToneSql the squelch tone, DTCS Code; Duplex and Offset; Mode examples FM, NFM, WFM, DV, DN; Skip S or P. That page names the editor's columns, not the CSV's, and does not mention CSV.
* SDR++: licence https://raw.githubusercontent.com/AlexandreRouma/SDRPlusPlus/master/license (GPL-3.0); the key structure and mode index were recorded by the survey from a GPL source file (frequency_manager main.cpp, https://raw.githubusercontent.com/AlexandreRouma/SDRPlusPlus/master/misc_modules/frequency_manager/src/main.cpp); I did not open it. No documentation page exists.
* All fixtures in `tests/test_freq_import_formats.cpp` are written by hand to that description. No sign-up, login, account or download was used (the web fetch was a read of one public page).

## Results

Real result lines, build D:/foxsdr-build/feat-web, Release:

| Test | Result |
|---|---|
| test_freq_import_formats (new) | `1970 checks, 0 failed` |
| test_freq_import (existing) | `602 checks, 0 failed` |
| test_web_control | `264 checks, 0 failed` |
| test_control_ops | `295 checks, 0 failed` |
| test_web_server | `1183 checks, 0 failed`, ~27 s (one run of 1184 before the shutdown-budget row; the count differs by the opt-in measurement block) |
| test_receiver_snapshot | `802 checks, 0 failed` |
| test_engine_api_05a | `956 checks passed, 0 failed` (does not set `muted`: see HAND-OFF) |
| test_shutdown_budget (gui) and test_shutdown_budget_free | Passed, 80.4 s and 81.1 s |
| i18n | `tools/i18n_keys.py`: every catalogue 2522 translated, 0 empty, 0 missing, 0 stale |

`stop()` with four live-update listeners attached: 0 or 1 ms (printed by the test), against the existing budget `kClientIoTimeout` = 10 s.

### CPU, 60 s each (FOXSDR_WEB_MEASURE=1 test_web_server, in-process clients, status changing on every look so the stream pushes every look)

| Case | Process CPU | Events / polls received |
|---|---|---|
| 0 listeners, no poller | 0.047 s = 0.08 % of one core | none |
| 1 listener | 0.000 s = 0.00 % | 235 events (3.9 per s) |
| 4 listeners (the cap) | 0.047 s = 0.08 % | 936 events |
| 0 listeners, one page polling /api/status at 4 Hz | 0.094 s = 0.16 % | 231 polls |

The process clock ticks in 15.6 ms steps (GetProcessTimes), so these are bounds, not precise figures: every case is under 0.2 % of one core, and four streams cost no more than an idle server. The status provider here is a stub; a real one costs more per look, in proportion to the looks (4 per listener per second, the same as one poller).

### Break-it table (remove the behaviour, see the named test RED, restore; script `web-breakit.py`, results in `D:/foxsdr-build/web-breakit.log`)

| # | Behaviour removed | RED |
|---|---|---|
| I1 | CHIRP chosen by content | test_freq_import_formats:206 `format == "CHIRP CSV"` (5 fails) |
| I2 | SDR++ chosen by content | :345 (3) |
| I3 | CHIRP tone kept in name | :75 `GB3WH [CTCSS 103.5 Hz]` (5) |
| I4 | CHIRP unknown mode counted | :84 `modeDefaulted == 1` (5) |
| I5 | CHIRP header refusal | :187 `!r.error.empty()` (5) |
| I6 | byte-order mark dropped | :117 (6) |
| I7 | quoted fields | :118 (3) |
| I8 | FM -> NFM | :84 (4) |
| I9 | SDR++ list -> group | :275 (8) |
| I10 | SDR++ mode map (0/1 swapped) | :276 (4) |
| I11 | SDR++ `lists` check | :332 (2) |
| W1 | `muted` boolean field parsed | test_web_control:74 (6) |
| W2 | `muted` -> SET_MUTED | test_control_ops `muted -> 0 command(s), want 1` (27 checks); test_web_server:3054 (2) |
| W3 | `"muted"` in status JSON | test_web_server:3077 (3) |
| W3b | `muted` composed from FOXAPI_RX_MUTED | first run: NOT caught (0 fails). Added the member to test_receiver_snapshot's tables; now RED at test_receiver_snapshot:1014 and :1290 |
| W4 | events auth gate | test_web_server:3198 `status == 401` (3) |
| W5 | listener cap | :3310 (6) |
| W6 | stream checks running_ | NOT caught (0 fails): redundant with the condition-variable wake and httplib closing the sockets; kept as defence in depth, not claimed as tested |
| W6b | stop() wakes the streams | :3368 `took < 200` (1) |
| W7 | send only on change | :3266 `events() == 1` (5) |
| W8 | session re-checked every look | :3226 stream still open after logout (2) |
| W9 | keep-alive comment | :3389 (1) |
| W10 | stream counts as heartbeat | :3279 `heartbeats == 0` (1) |
| W11 | page falls back on error | node test `closedA=1 rateDown=250 retryMs=10000` (1) |
| W12 | page slows poll while up | node test `rateUp=500` (2) |
| W13 | Mute button wired | :3099 (1) |

## Findings along the way (fixed in this branch)

* `kAppJs3` hit MSVC's single-literal limit (C2026): the new page code is in a new literal `kAppJs3b`, joined in `appJs()`.
* The ptt401 node driver extracts `stopPolling`; it now also gets `stopEvents` and the `es` variables.
* `test_receiver_snapshot` enumerates every `RadioStatus` member and flag: `muted` added to its tables.
* `test_shutdown_budget` discovers bounded waits by scanning for constants and refused `kEventHeartbeat` and `kEventTick` until each had a row (zero on the shutdown path, with the reason).

## HAND-OFF

1. **Web mute needs an engine line** (this lane may not edit src/engine). In `src/engine/fox_engine_api.cpp`, add `FOXAPI_OP_SET_MUTED` to `kWebProfile` (it holds 38 ops, none of them SET_MUTED; `test_web_server` prints `HAND-OFF: the engine's web profile does NOT hold FOXAPI_OP_SET_MUTED`). Then in `tests/test_engine_api_05a.cpp` P1 (`opsOfEveryRequest`) set `r.muted = true` and change `CHECK_EQ(web.size(), 38)` to 39. Until then the page's Mute is answered DENIED by the engine (a result the web session collects), the button does nothing, and the status never shows a mute from the page. Nothing else is missing. Not run: an end-to-end press against a real engine (the stub session proves the op and value, the profile gap is proved by reading the table and the probe line).
2. **Slint import dialog** (another lane): it needs no code for the formats (they are chosen by content). If its file picker filters by extension, add `json` (and `csv` is already there) to the filter string for the import dialog (the one in `src/slint/tools_logic.cpp` / `signal_logic.hpp`, whichever holds the import filter; I did not open the Slint files). The result's `format` string ("CHIRP CSV", "SDR++ JSON") and the new counters `modeDefaulted` and `toned` can be shown in its result line; the classic window's dialog is untouched.

## MERGE NOTES: existing files edited

`src/core/freq_import.hpp`, `src/core/freq_import.cpp` (two lines in `importFrequencyList`), `src/net/web_control.hpp`, `src/net/web_control.cpp`, `src/net/control_ops.cpp`, `src/net/status_compose.cpp`, `src/net/web_server.hpp`, `src/net/web_server.cpp`, `docs/MANUAL.md`, `docs/api-0.3-additions.md` (new 9.23, before section 10), `tests/test_control_ops.cpp`, `tests/test_web_control.cpp`, `tests/test_web_server.cpp`, `tests/test_receiver_snapshot.cpp`, `tests/test_shutdown_budget.cpp`. New: `src/core/freq_import_formats.cpp`, `tests/test_freq_import_formats.cpp`, this record. No CMake edit (both are globbed; reconfigure once). Hot spots: `web_server.cpp` is also expected to be touched by the TCI/UDP lane (check at merge that it added nothing there); `api-0.3-additions.md` section 9.23 may collide with another lane's 9.23 (rename); `test_shutdown_budget.cpp` kKnownWaits is append-style; A2 (files and bookmarks) was planned to edit `freq_import.*` for 23a/23b: this lane replaces those two rows. Commits: 18fe5d08 (importers), bfe9661d (mute and live updates), e5a24a97, ab8cc1d5 (test table updates), and the record.

## UNVERIFIED

* Both file formats against a real export. The CHIRP header is from a secondary source; the SDR++ mode index map and key names from the survey's reading of GPL source. One real CHIRP CSV and one real `frequency_manager_config.json` from the owner would settle both.
* Live updates and the Mute button in a real browser: the page logic is run in node against a fake EventSource and the server over real HTTP, but no browser was opened.
* The tone-in-name choice for CHIRP rows and its wording.
* A 15 s idle stream through a real reverse proxy (keep-alive is tested on loopback only).
* The `-L gui` window gate was not run (see Gate).

## Owner decisions

1. Tone text in the bookmark name, or a real tone field / notes on `Bookmark` (touches bookmark.hpp, FreqManager save and the engine; belongs with the CTCSS feature, survey row 13). Chosen here: the name, because it needs no format change.
2. Approve the one-line engine change (HAND-OFF 1) for the web mute.
3. Whether a page that polls status at 500 ms next to the stream is acceptable, or the transmit key's heartbeat should move to the stream (an engine session rule, API 0.2).

## Gate

Build: `cmake --build D:/foxsdr-build/feat-web --config Release --parallel 4` (everything but the Slint window, which is not configured here: `CASCADE_BUILD_SLINT=OFF`, so the `slint` label has no tests in this tree), exit 0, then "up to date". Gate: `ctest -C Release -L "headless|slint" -j4 --timeout 300`, 685 tests, run in two halves.

* `-I 1,340`: `99% tests passed, 5 tests failed out of 340`, 897 s. The five are the known false failures of a D: build: test_airband_data, test_band_plan, test_converter_call_sites, test_page_resize_id, test_png_decode. Run alone with the worktree root as working directory: 115, 5174, 5, 22 and 49 checks, 0 failed.
* `-I 341,685`: `99% tests passed, 2 tests failed out of 345`, 651 s (test_diagnostics, test_diagnostics_free, test_engine_api_transmit passed in this run).
  * **test_state_snapshot_golden: REAL, caused by this branch, fixed.** The golden file holds the web status JSON; the new `"muted"` member added a line's worth to 28 of its 1022 lines. Regenerated with `FOXSDR_WRITE_GOLDEN=1`; the diff is 28 lines changed, all of them the added `"muted":false` (one `"muted":true` row where the test sets the mute), the line count unchanged. Passes after: `test_state_snapshot_golden: 53 checks, 0 failed`.
  * **test_window_failed_app: configuration, not this branch.** It needs `softgl/opengl32.dll` and `libgallium_wgl.dll`, which a build configured with `CASCADE_SOFTGL=OFF` does not stage (I turned it off because the default configure downloads Mesa, which this lane may not do). Failed alone with the same missing-file lines (test line 221). Not run with the DLLs: UNVERIFIED either way.
* The ctest label count includes the opt-in tests in the lists of the two halves; fuzz corpus tests (`fuzz_corpus_freq_import`, `fuzz_corpus_web_policy`, `fuzz_corpus_web_http`) ran and passed in half 2.
* The earlier targeted run of the touched areas (`-R "web|freq_import|control_ops|engine_api_05a|shutdown_budget|receiver_snapshot"`) found test_shutdown_budget(_free) RED for kEventHeartbeat / kEventTick (fixed, commit ab8cc1d5; both Passed afterwards, 80 s each).
* Not run: the window gate (`-L gui`, hidden desktop) and any live radio or browser.
