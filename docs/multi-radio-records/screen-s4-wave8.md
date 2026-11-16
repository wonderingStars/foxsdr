# Wave 8, screen lane S4: scanner, outputs and alerts (2026-11-16)

Branch `feat/screen-s4`, cut from `3bef400b` (engine lane E1's tip) in the worktree `D:/foxsdr-wt/en-dspmodels`; build folder `C:/Users/steve/foxsdr-build/s4`; hidden desktop `FoxSDRS4`; scratch `D:/foxsdr-build/s4-scratch/`. Nothing pushed. (The file name is `screen-s4-wave8.md` because `screen-s4.md` is the record of the 2026-11-11 multi-radio screen stage S4.)
Briefs read: `STANDARD-RULES.md`, `OWNER-ANSWERS-2026-11-16.md`, `wave8/common.md`, `wave8/screen-s4.md`, `screen-m.md`, `errpath-gate.md`. E1's own record `engine-e1.md` did not exist yet; its four commit messages were read.
Real `%APPDATA%/foxsdr/config.json` sha256: see the end.

## What was built, in one paragraph
A new engine class (`engine/s4_outputs.{hpp,cpp}`, `Hub`) wires three stand-alone models that nothing called, to the host's decode-event ring: the MQTT publisher, the decode webhook and the keyword alert rules; plus an InfluxDB level writer. Settings are new rows of the wirec registry; the MQTT password, the webhook address and the Influx token live in the Windows protected store (E1's vault), never in `wirec.json`, never in a fact. A new window panel (`s4_publish.cpp`, `s4_logic.cpp`, `ui/s4_bank.slint`) draws the TCI and UDP-audio switches (the engine had them; no window did), MQTT, the web address, InfluxDB and the keyword alerts under SYSTEM > Outputs, and "save each signal the scan stops on as a bookmark" under TOOLS > Scanner. Everything is off until the person turns it on and sends to this computer only unless another address is typed; the window says plainly what is reachable from the network or unencrypted.

## The rows

| Row | Verdict | Proof / reason |
|---|---|---|
| D1 / G2-42 scan priority | ALREADY BUILT (engine fixes 8, commit 3b86f68c) | `lanec_publish.cpp:331-340` sends the list with `SCANNER_EXTRAS` 0 and reads `fact.scan.priority` back; engine uses it (`wire_a_scan.cpp` `adjustScannerParams`, `core/scanner.hpp:320-331`). Drive `lanec-scanner.txt` reads the engine's `fact.scan.priority` after typing. The "does not use them yet" note shows only while the engine's list differs. Nothing left to build. |
| G2-43 lockouts | ALREADY BUILT (E1, e452e65a) except two parts | Lockout keys, range and plan scans, status text and the time left: drive `e1-lockout-range.txt`. NOT on screen: the per-frequency list (`fact.scan.lockouts` carries it; the window shows the count and the time to the first end) and the configured locked list (`SCANNER_EXTRAS` 5 has no sender). Both are in OPEN ROWS. |
| G2-37 repeater offsets applied | NOT BUILT | The offset is stored (`BOOKMARK_FIELDS` `offsetKhz`) and shown ("Transmit 99.7000 MHz"); its only use would be a transmit shift. Owner decision 54 (no new transmit ability) rules it out; the tone half is applied on recall already (FEATURE-GAPS-STATUS G2-37). |
| G2-46 automatic bookmarks | BUILT | `s4_logic.cpp` `AutoBookmarks` + `s4_publish.cpp`: when the scan has just stopped on a signal and the switch is on (off at every start), `SCAN_SAVE_HIT`; a range scan has no scan plan so the engine answers "there is no scan plan..."; the window then adds the bookmark from the tuned frequency (`BOOKMARK_ADD`, named "Scan hit <MHz>"). Drive `s4-scan.txt`: bookmark count 0 with the switch off, >= 1 after a scan with it on, read from the engine. `test_slint_s4_host` (38 checks). Break-it: the send removed -> `s4-scan` red (`state s4.saved >= 1 -> FAILED: it is 0`, `count.bookmarks` 0); restored green. |
| G2-47c FindMatch by time | NOT BUILT | The lookup is the EiBi `station-labels` plugin's (plugins repo, plugin-first rule); the in-tree part is the user's own CSV overlay (`lanel_publish.cpp` "Station database"), which has no time column. Takes: a "what is on this frequency at HH:MM UTC" query in that plugin. |
| G2-48d Perseus memory import | NOT BUILT | No open description of the file. Searched 2026-11-16: web search "Perseus SDR memory file format .mem frequency list import documentation" (no Perseus page; only SDR-Console's CSV/XML, which the CSV importer already reads); the tree's own note `core/freq_import_foreign.hpp:21-23` records the 2026-11-11 search. Code-host search (site:github.com) NOT run in this lane: UNVERIFIED. |
| G2-38 command-line options | ALREADY BUILT | `src/engine/process_boot.cpp` (`bootProcess`): unknown arguments print "unknown argument '%s' (usage: ...)" and return 1 (`:869-891`); orphan `--web-*` flags refused. Read, not re-run. |
| G2-40 spoken announcements | ALREADY BUILT | `lanek_publish.cpp:357-369` (page "Spoken readout"), `core/access_speech.hpp` `Announcer`, `SapiDeviceSpeaker` (`lanek_audio.cpp:101`). Real speech UNVERIFIED (none made here). |
| G3-17 TCI on/off | BUILT | SYSTEM > Outputs: "Serve the TCI interface", "Reachable from other computers", "TCI port" (`SERVER_CONFIG {"tci":{...}}`), status from `fact.tci.*`, the no-password warning when reachable. Drive `s4-outputs.txt` reads each value back from the engine; a port outside 1024..65535 is refused with a sentence and the engine keeps its own. Break-it: the switch sent as `bindAll` -> `s4-outputs` red (line 24 `state s4.fact.tci.enabled == 1 -> FAILED: it is 0`); restored green. The reach switch is set only while the server is off in the drive (no listener opened to the network). |
| G3-22 UDP/TCP audio switch | BUILT (UDP) / NOT BUILT (TCP) | "Send the audio over UDP", address, port, radio, packet length from `SERVER_CONFIG {"udp":{...}}`; warning when the address leaves this computer. There is no TCP audio output in the engine; building one would be a new server in `server_host.cpp`. |
| G3-20 rotator | NOT BUILT | `core/rotator.hpp` is a header with no caller; needs an engine op set, a serial/TCP seam and a stand-in. |
| G3-21 DDE client | NOT BUILT | Nothing in the tree. Needs a Windows DDE client module and a stand-in DDE server for the test. |
| G3-36b PostgreSQL | NOT BUILT | No libpq in the host; it is a plugin in the plugins repo (docs/MANUAL.md:2590). The host event feed it waited for exists and is now consumed by the Hub, so a plugin can read the same ring; nothing to add in the host. |
| G3-37b desktop notifications | PARTLY BUILT | The alert goes to a `NotificationSink`; the in-window list shows the last alerts. NO sink is installed in the product (a Windows notification needs an app identity and a Start-menu shortcut: `alert_rules.hpp` header), so nothing is ever shown on the desktop by this lane. The setting `alerts.notify` exists and has no control on the screen, on purpose (a control must do what it says). |
| G3-35 MQTT | BUILT | Engine `Hub` + wirec keys `out.mqtt.*` + the SYSTEM > Outputs rows. `test_s4_outputs` runs a scripted loopback broker: connection, user name, every line published as the decode JSON, a private plugin's line held, switch off stops it, alerts-only mode, bad host/topic refused. |
| G3-36a InfluxDB | BUILT | `out.influx.*`: the selected radio's channel power and frequency as line protocol, UDP or HTTP write, every 1..3600 s (default 10), token in the protected store. `test_s4_outputs`: stand-in sender (line, token reaches the sender and never the stats, interval, failure sentence, a record offered while one is in flight is dropped and counted), bad settings, the real UDP sender to a loopback socket. No TLS (the model has none); the window says the link is unencrypted when the host is not this computer. HTTP write against a real InfluxDB UNVERIFIED. |
| G3-36d webhooks | BUILT | `out.webhook.*`: https anywhere, plain http to this computer only (the client refuses a LAN http address: decision 19 kept). Tests: stand-in client, failing client, bad address, alerts only, the real bounded client to a loopback stub, bounded queue (1000 events into a stalled client: >= 800 dropped and counted). |
| G3-49 ADIF, QSO, SWL log | ALREADY BUILT (QSO, ADIF) / NOT BUILT (SWL) | `lanel_publish.cpp:960-1000` (SYSTEM > Ham tools, drive lanel scripts). The SWL log writer `core/adif.hpp` has no producer and no engine op. |
| G3-50 frame log and decode database | ALREADY BUILT (frame log controls) / NOT BUILT (decode DB) | Frame log keys at `lanel_publish.cpp:631-670`. `core/decode_db.hpp` is not wired and stays so (owner decision 19: the decode database stays as limited as it is). |
| G3-51 keyword alerts | BUILT | `alerts.*` keys, Hub, rows "Raise an alert when a decoded line holds a keyword", keywords, repeat gap, the recent alerts. Tests: `test_s4_outputs` (private line raises none, repeat gap, off, bad list, refusing and throwing sinks), `test_engine_api_s4_outputs` through the real engine and its event ring. |
| G3-34 rebindable keys and macros | ALREADY BUILT (keys) / NOT BUILT (macro recording and a macro page) | Keys: `system_bank.slint:1602`, `key_dispatch.cpp`. Macros play from a hand-edited `macros.json` (`engine_macros.cpp`); no recorder, no window page. |
| G3-24 rtl_tcp server | NOT BUILT | Nothing serves the protocol. The nearest piece is network I/Q out (`IQ_OUT_START`, loopback only). Needs a server in `server_host.cpp`: 12-byte header, the five-byte command set, a stand-in client. |
| G3-25a/c, G3-44a/b Beast, AIS outputs | NOT BUILT | `net/aircraft_out.hpp` (BeastOut) and `net/ais_out.hpp` exist; only the SBS text output is wired (`engine_wire_b.cpp:462`, loopback). Needs the same wiring for the two. |
| G3-31 WebSocket / SSE API | NOT BUILT | The web server has SSE (`/api/events`) and no WebSocket. |
| G3-32 MCP server | NOT BUILT | `net/mcp_server.hpp` + `mcp_http.hpp` are not included by any source; wiring means choosing which engine commands its five tools may send. |
| G3-30 Receiverbook | NOT BUILT | `WebServer::setReceiverListing` exists and nothing calls it. Nothing is ever sent to Receiverbook by this lane; the page that says exactly what would be published, and the user's own press, are not built. |

Decision 19 re-checked in code: UPnP (`net/upnp_igd.hpp`) has no caller, so nothing is forwarded; the CAT emulation defaults to this computer and has a "listen on every address" switch that is off (`wire_c.cpp`), unchanged; webhooks plain http only to this computer (above); decode database not wired. Nothing was widened.

## New engine surface
No new FOXAPP op. 22 wirec keys (`out.mqtt.*` 7, `out.webhook.*` 3, `out.influx.*` 8, `alerts.*` 4) through the existing `WIREC_SETTING`; new facts `out.seen`, `out.held_private`, `out.mqtt.{state,offered,problem,scope}`, `out.webhook.{queued,ok,failed,dropped,problem,scope}`, `out.influx.{ok,failed,problem,scope}`, `alerts.{raised,notified,notify_failed,problem,notify_problem,recent}`. `fact.wirec.<secret key>` now reads "set" or "", never the value. `Settings::save/reload` go through the vault (new `secretKeys()` entries: `out.mqtt.password`, `out.webhook.url`, `out.influx.token`), a plain-text value from an older file is migrated on first read. `alerts.recent` (the matched keyword and the text, cut to 120 bytes) is a LOCAL-only fact like every wirec fact; nothing of any decoded text goes into a report.

## SHARED FILE EDITS
- `src/engine/wire_c.cpp`, `wire_c.hpp`, `engine_wire_c.cpp`, `src/core/secret_vault.cpp`: registry rows, vault read/write for wirec, State members, the pass block and facts (E1's files; additions only).
- `src/slint/ui/main.slint`: one import line, one export line, two mount lines (`LaneS4.rows-scan` at TOOLS section 3, `LaneS4.rows-outputs` at SYSTEM `system-extra-base + 11`).
- `src/slint/main.cpp`: include, member, the Env block (copy of lane M's), one tick line, two answer lines, one `s4.` state line, one line setting `lastScannerState` once a tick.
- `src/slint/CMakeLists.txt`: one `list(APPEND ... s4_logic.cpp s4_publish.cpp)`. `src/slint/tests/CMakeLists.txt`: one `s4_host` family block (a copy of `lanem_host`'s). `tests/CMakeLists.txt`: not edited (tests are picked up by glob).
- `resources/lang/*.json` (33), `src/core/lang_assets.hpp` (regenerated by `tools/embed-lang.py`).

## Expectations changed
None.

## New strings
86 keys, in all 33 catalogues, machine-made (the files are marked `"machine": true`) and unchecked by a speaker; `tools/i18n_keys.py`: 3993 keys, 0 empty, 0 missing, 0 stale per catalogue. Translators' notes from the three translating agents: "Scan hit" / "Scan hits" were left as English literals in the zh/ru/uk/vi/tr group because the bookmark name is built from that text, and chosen freely in the others; "bucket", "broker" left in English in several. The engine's own sentences (`fact.out.*.problem`, the scope sentences, a refusal's text) are shown as the engine gives them, in English.

## Error-path gate
`test_error_paths` before (E1 base, this worktree): 1125 PENDING pinned. After: 1125 (unchanged; passes). The Hub's one `catch (...)` counts a failure and says it; no `setError`/`warn` sites were added. The MQTT and webhook models' own sites stay `PENDING:O-outputs` (55). A failed output is therefore shown to the person (rows and facts) but is NOT reported to the owner: the reporter vocabulary has no output-failure word. Listed in OPEN ROWS.

## Tests (real runs, Release, this worktree)
- `test_s4_outputs`: 102 checks, 0 failed (pure rules and boundaries, off does nothing, alerts, webhook x6, MQTT against a scripted loopback broker, InfluxDB, configuration change). Break-it: letting a private plugin's events through turned it red (5 failed); restored.
- `test_s4_settings`: 66 checks, 0 failed (defaults, ranges, secrets absent from the file and from facts, back after a restart, cleared, plain-text migration).
- `test_engine_api_s4_outputs`: passes (the real engine: off does nothing, alerts through the real ring with a recording sink, the webhook to a loopback stub with no backlog and no private line, the secrets, refusals).
- `test_slint_s4_host`: 38 checks, 0 failed.
- Drives on the hidden desktop `FoxSDRS4`: `slint_drive_s4-outputs` and `slint_drive_s4-scan` pass; both go red under their break-its (above).

GATES: see the section appended at the end of this file.

## UNVERIFIED
- No real radio, COM port or device was opened. No outside request: every network test is a loopback stand-in (a scripted MQTT broker, a stub web server, a UDP socket, a stand-in sender).
- Real desktop notifications: none exist. Real speech: not made. A real MQTT broker, a real InfluxDB, a real web hook: not tried. A real mouse and a screen reader: not used. TLS: none for MQTT and Influx (the models have none).
- Perseus: the code-host search was not run (only a web search).
- The rows appear on the screen only after the person opens SYSTEM > Outputs; looked at by script (labels, read-backs), not by eye (no screenshot taken).
- Translations unchecked by a speaker.

## OPEN ROWS FOR THE NEXT LANE

| Row | What waits | Kind of lane |
|---|---|---|
| G2-43 | per-frequency lockout list on screen; a sender for the configured locked list (`SCANNER_EXTRAS` 5) | screen |
| G2-47c | time lookup in the station-labels plugin | plugin |
| G2-48d | Perseus memory import: code-host search, then a format description or a user-supplied sample | research |
| G3-22 | TCP audio output (a server in `server_host.cpp`) | engine |
| G3-20, G3-21 | rotator (engine + stand-in serial), DDE client (+ stand-in DDE server) | engine |
| G3-24 | rtl_tcp server | engine |
| G3-25a/c, G3-44a/b | Beast and AIS outputs (wire the two net models like SBS) | engine, then screen |
| G3-30 | Receiverbook listing: caller of `setReceiverListing`, the page that states what would be published, stand-in service | engine + screen |
| G3-31, G3-32 | WebSocket on the API; MCP server (which commands its tools may send) | engine |
| G3-34 | macro recorder and macro page | engine + screen |
| G3-36b | PostgreSQL stays a plugin (plugins repo) | plugin |
| G3-37b | a real Windows notification sink (app identity, shortcut) and a control for `alerts.notify`; verify with the mouse drive | engine + mouse drive |
| G3-49, G3-50 | SWL log producer; decode database (decision 19 limits) | engine |
| errpath O-outputs | a way to tell the owner when an output fails (a reporter word for outputs) | engine |
| Mouse drive | read every new row by eye at default window size; a real MQTT broker, InfluxDB and web hook | mouse drive / owner |
| G2-37 | stays out unless decision 54 is lifted | owner |
