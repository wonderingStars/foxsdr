# Security lane S4b-sec: the network servers of screen lane S4b (2026-11-16)

Branch `feat/screen-s4b-sec`, cut from `feat/screen-s4b` at `27bc994e` in `D:/foxsdr-wt/en-dspmodels`; build folder `C:/Users/steve/foxsdr-build/s4`; scratch `D:/foxsdr-build/s4bsec-scratch/`. Nothing pushed. No outside request, no real network peer (loopback stand-ins only), no radio, no COM port, nothing on the visible desktop. Config hash at the end: `c21ce98dc30e50327f7666f7ee29047c0983d130ca3f21716d0c0e20e04603c3` (unchanged).
Read: common.md, screen-s4b.md, SECURITY-REVIEW-2.md (R7, R8, its method and its "still unread" list), security-fixes-1.md, screen-s4b's record.

## 1. R7 and R8 against the final code (27bc994e)

| Finding | Verdict | Where (27bc994e) |
|---|---|---|
| R7 rtl_tcp and audio over TCP: no authentication with "every address" | CONFIRMED | `engine/s4b_servers.cpp:97` and `:130` bind 0.0.0.0 on the tick, no password and no list; `net/rtl_tcp_server.cpp:198` one client holds the only place |
| R8a Host never checked, status and events readable by a rebinding page when no password | CONFIRMED | `net/api_server.cpp:241` refuses an Origin only; no Host check anywhere; `engine/s4b_servers.cpp:166` `record.empty() => Allowed` |
| R8b password hashing on the single IO thread | CONFIRMED | `net/api_server.cpp:249` calls the auth function on the IO thread, `engine/s4b_servers.cpp:190` runs the PBKDF2 |
| R8c credential cache outlives a password change | CONFIRMED | `engine/s4b_servers.cpp:194` caches 5 minutes; `:205` clears it only when the API itself is reconfigured |

## 2. Findings and fixes (one commit each)

| Id | Severity | Where | Finding | Fix | Test (break-it seen RED) |
|---|---|---|---|---|---|
| S1 | LOW | `engine/s4b_swl.cpp` `SwlBook::load` | a wrong-typed field (`"t":"x"`, `"m":5`) made nlohmann `value()` throw while the engine started: the whole application failed at launch | every field type-checked before reading; damaged = the existing sentence and an empty log; the file is renamed `swl-log.json.damaged` so the next save cannot destroy it | `test_s4b_sec_swl` 81 checks (15 damaged shapes); old loader RED |
| R8a | LOW | `net/api_server.cpp` | no Host check | without `requireAuth` a Host that is not localhost, 127.x, `[::1]` or the bound address (port allowed) is refused 403 on `/status`, `/events`, `/ws`; no Host (HTTP/1.0) and Origin rules as before | `test_s4b_sec_api`; check removed RED (35 reds) |
| R8b | LOW | `net/api_server.cpp` | PBKDF2 on the IO thread: each failed attempt stalls every client | a dedicated auth worker (`authLoop`); the request waits, bounded (`maxPendingAuth` 8, then 429); what a client sends meanwhile is kept, bounded | open stream keeps being pushed to and a request needing no sign-in is answered during a 700 ms sign-in; 429 bound; `stop()` with a stuck sign-in; synchronous call RED |
| R8c | LOW | `engine/s4b_servers.cpp` | cached login survives a password or user change; open streams stay | cache keyed to the account (user + stored record), discarded on the first request after a change; a login whose derivation straddled a change is refused; `pass()` notices a change within 0.5 s and ends the open WebSocket/SSE streams (`closeAllClients`) | `test_s4b_sec_servers` A, B; three break-its RED |
| R8d (found) | LOW | `engine/s4b_servers.cpp:166` | with "every address" on and the web password REMOVED, any Authorization header was let in (empty record = open) | beyond this computer an empty account lets nobody in | `test_s4b_sec_servers` B; RED |
| R8e (found) | LOW (availability) | `engine/s4b_servers.cpp` `checkApiAuth` | took the set's lock on the IO thread while `applyApi` holds that lock and joins that thread: a settings change racing a request could deadlock the control pass | the account hook has its own small lock and is read on the worker | `test_s4b_sec_servers` C (deterministic: the lock is held by a blocked audio hook, a sign-in must still be answered); RED |
| R8f (found) | LOW (availability) | `net/api_server.cpp` `acceptAll` | one address could hold every one of the 16 slots with requests it never finished, reconnecting for free | at most `maxPerAddressHead` (4) unfinished requests per address; the next is closed at once | `test_s4b_sec_api`; RED |
| R7 | LOW | `net/peer_allow.hpp` (new), `tcp_fanout`, `tcp_audio`, `rtl_tcp_server`, `engine/s4b_servers.cpp`, `engine/wire_c.cpp`, `slint/s4b_publish.cpp` | no authentication beyond this computer | the servers are widened beyond this computer only to a typed list of client addresses/networks (`srv.tcpaudio.allow`, `srv.rtltcp.allow`; prefix 8 to 32, no 0.x, no multicast, no /0..7, at most 16, 400 characters); a connection from any other address is closed before a byte; with "every address" and no usable list the server does not start and says why (checked before any radio or socket); the Outputs page shows the field and a truthful warning | `test_s4b_sec_peers` 124, `test_s4b_sec_servers` D, `test_engine_api_s4b_sec` 38; six break-its RED (list-parse break-its ran with the bind changed to loopback so that no test bound a wider address) |
| R7b (found) | LOW | `net/rtl_tcp_server.cpp` | a web page can send bytes to `http://127.0.0.1:1234` and pad a POST body so that a later 5-byte block is `0x01 + frequency`: a cross-protocol retune | a connection whose FIRST 5-byte block is not an rtl_tcp command (1..0x0E) is dropped (`refusedProtocol`); real programs start with a command; later unknown commands are counted and ignored as before | `test_s4b_sec_peers` R; RED |

## 3. Read and found sound ("none" per area)

* `net/tcp_audio.cpp`, `net/tcp_fanout.cpp` limits: none beyond R7. Bounded queue (64 KiB), 5 s stall drop, 32 KiB send buffer, client reads discarded in 64 KiB turns, one producer, 4 to 64 clients; a client that never reads is dropped, a thousand connects are refused at the cap (existing tests).
* `net/rtl_tcp_server.cpp` I/O loop: none beyond R7/R7b. 12-byte header, 5-byte commands, reads in 64 KiB turns, whole commands only, 1 MiB queue, 10 s stall drop, one client; the host function is only called for 1..0x0E and the engine's function uses 0x01 alone.
* `net/control_hub.cpp` token bucket: none. One bucket for all clients (10 a second, burst 20) means one client can starve the others of tunes: availability only, accepted. Text from decoders is cleaned and cut; frequency checked finite and 1 kHz to 7.2 GHz; modes from a closed set.
* `net/ws_frame.cpp`: none. Masking required, reserved bits and opcodes refused, minimal lengths enforced, control frames under 126 bytes, fragmentation bounded by `maxMessageBytes`, UTF-8 validated, the buffer cannot exceed one message plus a header.
* API HTTP parsing (`net/api_server.cpp`): none beyond R8. Head capped (8 KiB, 64 header lines), only GET, version checked, one request per connection, no body is read, Origin refused, path allow-list of three routes.
* `engine/s4b_notify_win.cpp`: text cut to 63/255 UTF-16 units, control characters replaced, invalid UTF-8 replaced, rate limit 6 a minute and a queue of 8. Observation, not fixed: if the message window cannot be created, every call waits up to 2 s on the caller and the queue is never drained (6 a minute at most, only when Windows refuses the window; UNVERIFIED because a test never creates the real window).
* Rotator (`core/rotator.hpp` builders and `TcpTransport`, `engine/s4b_rotator.cpp`): none. Bearing checked finite and in range before any command is built, three digits clamped (GS-232) or fixed to 2 decimals (rotctld); lines capped at 512 bytes; error sentences are built from numbers, never from the peer's text; the serial name must be `COMn`; the host is typed by the person and the page says it is sent unencrypted. Read-only commands plus turn, stop, park: nothing keys a transmitter.
* `SwlBook::load`: S1 above. Export writes only to a full path ending `.adi`/`.adif` and only from a local session (the macro recorder denies the op).
* MCP (`engine/s4b_mcp.cpp`): none (Host and Origin handled by the model, loopback only, optional bearer token in the protected store).
* The settings bundle (`core/sys_bundle.cpp`) allow-lists three files; `wirec.json` (where every `srv.*` switch lives) is not one of them, so an imported bundle cannot switch these servers on.

## 4. What a client can do, re-proved by test over the commands each server accepts

* rtl_tcp: all 256 command bytes sent; the host function sees only 1..0x0E (14 numbers); the engine's retune uses 0x01 alone (a parameter of 999 MHz on every other number never tunes); no mode, gain, rate or setting is reachable. (`test_s4b_sec_peers` S, `test_s4b_sec_servers` E)
* API/SSE/WebSocket: more than 30 operation names that would transmit, key, read or write a file, change a setting, start/stop, install, connect or run a program are each answered "unknown operation" and change nothing; a tune message that also carries a path and a command tunes and nothing else. Five operations exist (status, bookmarks, recent decodes, tune, set mode) plus subscribe.
* TCP audio and SSE: nothing a client sends is parsed.
* MCP: five tools (existing `test_mcp_server`, `test_engine_api_s4b_control` A4).

## 5. Behaviour a user will notice (release notes)

* With "Let other computers connect" ticked for the audio or for rtl_tcp, a field "Computers allowed to connect ..." appears; until it holds at least one address or network, the server does not start beyond this computer and says so. Only the listed computers are served.
* An rtl_tcp connection that does not begin with an rtl_tcp command is dropped (no real program is affected).
* The program interface (WebSocket/SSE): a request whose Host is not this computer's own is refused 403 (a program using `127.0.0.1` or `localhost` is unaffected); changing or clearing the web page's password ends open connections and refuses the old login at once; at most four unfinished requests per address.
* A damaged `swl-log.json` no longer stops the program from starting; the file is kept as `swl-log.json.damaged`.

## 6. Expectations changed (both values)

* `tests/test_engine_publish_alloc.cpp` F12: `rebuild < 2000` -> `rebuild < 2010`. Measured 1992 before, 2004 to 2005 after (two new settings are published as facts, six allocations each). Set once. F3's 2500 untouched.
* Test INPUTS (not expectations): `tests/test_api_server.cpp` (15 requests) and `tests/test_engine_api_s4b_control.cpp` (1 request) said `Host: x`; they now say `Host: 127.0.0.1` because the server now judges Host. Every expected status is unchanged.
* `tests/thread_inventory_rows.inc`: `net/api_server.cpp` 1 -> 2 threads (the auth worker), with its reason.
* `ApiConfig` gains `checkHost` (default true), `maxPendingAuth`, `maxPerAddressHead`; `Config` gains `apiBindForTest` (tests only, never a setting).

## 7. Tests run (Release, `C:/Users/steve/foxsdr-build/s4`, `--parallel 3`, foreground)

New: `test_s4b_sec_swl` 81, `test_s4b_sec_api` 87, `test_s4b_sec_servers` 61, `test_s4b_sec_peers` 124, `test_engine_api_s4b_sec` 38; all green, every break-it seen RED and restored.
Existing, green: `test_api_server` 92, `test_engine_api_s4b_control` 54, `test_engine_api_s4b_servers` 36, `test_engine_api_s4b_rtltcp` 40, `test_engine_api_s4b_swl` 39, `test_engine_api_s4b_listing` 37, `test_engine_api_s4b_macros` 62, `test_engine_api_s4b_notify` 24, `test_engine_api_s4b_rotator` 56, `test_engine_api_s4_outputs` 47, `test_engine_api_wire_c` 138, `test_rtl_tcp_server` 51, `test_tcp_audio` 40, `test_tcp_fanout` 1401, `test_ws_frame` 136, `test_mcp_server` 522, `test_macros` 261, `test_s4_settings` 109, `test_wire_c_portable` 65, `test_slint_s4b_host` 43, `test_i18n` 410225, `test_engine_publish_alloc` 869 (after the bound), `test_error_paths` (pin 1125, unchanged: no new allow line, none raised).
`test_thread_inventory`: 4 reds, all base reds (rds_log.cpp, server_host.cpp) that the S4b record already names; the api_server row is fixed.

## 8. NOT RUN / UNVERIFIED

Not run: the full headless gate, the full GUI gate, the `s4b-outputs` drive (the two new rows appear only when "every address" is ticked, which no drive does), the stamp tests, the clip sweep (the machine's gate for another branch was running). UNVERIFIED: the two new rows by eye; the 160 new translations (machine-made, unchecked by a speaker); the real desktop notification; rtl_tcp against a real client; the sign-in race window of a few microseconds between a worker's final account check and the IO thread applying its verdict (a stream opened in it survives until the next change).

## OPEN ROWS FOR THE NEXT LANE

| Row | What waits | Kind |
|---|---|---|
| New Outputs rows | read the two allow-list fields and the warnings by eye at the default window size | mouse drive |
| Translations | five sentences x 33 catalogues are machine-made | speaker review |
| TCI server (`s4_outputs`), UDP audio | the same "every address without a password" rule is not applied to them in this lane (not on its read list); their own warning sentences exist | security lane |
| Notifier | creation failure of the message window blocks callers 2 s each (observation) | engine |
| ControlHub | one token bucket for all clients (a client can starve the others) | engine, optional |
