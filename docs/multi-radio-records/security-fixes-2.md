# Security fixes 2 (wave 8, second security lane)

Worktree `D:/foxsdr-wt/en-dspmodels`, branch `feat/security-fixes-2`, cut from `4a4d9c28` (tip of `engine/multi-radio`, every lane merged). Build folder
`C:/Users/steve/foxsdr-build/s4` (Slint ON, Release, `--parallel 4`, `ctest -j2`). Scratch `D:/foxsdr-build/sec2-scratch/`. Nothing pushed, downloaded or deployed;
no radio opened; every socket on 127.0.0.1 (the one wide bind, in `test_tci_server`, is the one the program already had a firewall rule for and is unchanged in kind);
the hidden desktop was not needed (no window was started). The real `%APPDATA%/foxsdr/config.json` still has sha256
`c21ce98dc30e50327f7666f7ee29047c0983d130ca3f21716d0c0e20e04603c3`.

## Verdicts

| # | Item | Verdict | Proof |
|---|---|---|---|
| 1a | TCI beyond this computer has no list (S4bS-tci-udp) | FIXED | `TciServer::start(port, bindAll, error, PeerAllow)` refuses `bindAll` with no list (no socket is made); the acceptor closes a connection from an unlisted address before a byte is read or sent; `ServerConfig::Tci::allow`, the `"allow"` member of the SERVER_CONFIG `tci` object, the `tciAllow` config key, `fact.tci.allow`, a scope fact, and an Outputs-page field. `test_tci_server` 289 checks (no list = no start; list not used on loopback; only the other address listed = loopback client closed with no byte and no session, the listed address served; a network entry); `test_tci_engine` `testBeyondThisComputer` (refusal sentence, seven unreadable lists refused with the list's sentence, list kept and shown, saved and dropped when over 400 characters). BREAK-IT: the accept-time list check removed gave 4 failures in `test_tci_server`; restored. |
| 1b | TCI on loopback: a web page must not connect (the Origin rule) | NOT A FINDING (rule is on the merged tree and complete) | `tci_server.cpp` refuses any handshake with an `Origin` header (`hs.hasOrigin`, set by `ws_frame.cpp` for any case of the name, even an empty value) with 403 before a session exists. TCI has ONE entry point, the WebSocket handshake (`serveClient`); a request that is not a valid handshake is refused by `parseHandshake`. `test_tci_server` `testHostile` sends five forms (`https://evil.example`, `http://127.0.0.1:8073`, `null`, lower-case `origin`, upper-case empty `ORIGIN:`) and checks 403, no 101, no backend opened, and a client with no Origin still connects. |
| 1c | UDP audio: what is sent to whom; broadcast or multicast | FIXED (stricter than asked) | The header of `net/udp_audio.hpp` now states it: the finished audio of the one chosen radio, nothing else, to the one typed address, nothing read from the network, an emptied address resets to 127.0.0.1. `UdpAudioSender::start` refuses 255.255.255.255, any 224.0.0.0 and up, and 0.x.x.x, typed or reached through a name ("the audio goes to one computer: type that computer's address, not a broadcast or multicast address"). A subnet broadcast (192.168.1.255) cannot be recognised without the mask; the socket never sets SO_BROADCAST, so the system refuses to send to it and the failed sends are counted. Chosen deliberately: refused outright, not "allowed with a warning" (one fewer sentence to translate, and UDP audio is "one radio to one address"). `test_tci_engine`: four forbidden destinations give the refusal and `udp.sending` 0; 127.0.0.1 still sends. The Outputs page already warns that audio sent to another computer is not encrypted (unchanged). |
| 2 | Import guard's local radio kinds (M8-import-usb-kinds) | FIXED | `icr8600`, `funcube`, `g39ddc`, `g33ddc`, `sdriq` added to `localSourceKind`: they are the rows of `engine/wire_b_sources.cpp` `kTable` whose `needsAddress` is false (read from the table, not from memory); the seven `needsAddress` rows stay out. FOUND ON THE WAY: a saved radio of these kinds was STILL dropped, because each saves `timeout=1500` (or `index=0`, ...) as its default arguments and the argument classifier did not know that option; `classifyDeviceArgs` now accepts the whole-number options `timeout`, `rate`, `silence`, the SDR-IQ's `sdriq` serial and the Fobos' `count`, `direct`, `clock` words (fixed word sets); a `lib` or `firmware` PATH is still refused. Tests: one function per kind in `test_sys_bundle` (source kind, native arguments and a saved radio 2 all come back, nothing reported as left out), `testWireBPathsAndNetworkKindsStayOut` (a path or a non-number drops that entry; twelve network or file kinds are left out whole), and K7 in `test_wire_b_saved_kinds`, which ties the guard's list to the table's `needsAddress` column in both directions and checks every local row's default arguments survive. BREAK-IT: the five kinds removed gave 26 failures in `test_sys_bundle` and 5 in `test_wire_b_saved_kinds`; restored. |
| 3 | Plain web server slow client (R4) | FIXED within the library's own extension point; residual stated | See the next section. `test_web_conn_guard` 42 checks. |
| 4 | API Host rule vs the page's Host rule (M9-api-host-rule) | FIXED (names only; nothing loosened) | See below. |
| 5 | Web password change or removal ends the API's streams, with the real configuration (M9-api-account-e2e) | PROVEN end to end | `test_engine_api_s4b_account` (new, 52 checks), real `Engine`, real SERVER_CONFIG op, real password hashing worker, real wire-c settings. See below. |
| 6a | L9: helper lookup follows relative PATH entries | FIXED (portable part) | `findOnPath` skips every entry that is not an absolute directory (`.`, `bin`, `./x`, `../x`, `a/../a`); the clipboard lookup uses the same function. `test_slint_host_desk` (80 checks, five spellings, each also proven to find an absolute entry that follows it; clipboard lookup of `.` finds nothing). BREAK-IT: the check removed gave 6 failures; restored. |
| 6b | L9: clipboard and opener pipes without a time limit | NOT FIXED | The code is `realRun` in `desk_logic.cpp`, POSIX only (`posix_spawn`, `read`, `write`, `poll` would be needed). Nothing of it compiles on this Windows machine and no Linux toolchain is here; an unbuilt rewrite of a pipe loop is a risk, not a fix. Plan for the Linux lane: make both fds non-blocking, loop on `poll` with a 2 s total deadline for the write and a 2 s total for the read, close both and `waitpid` with the existing 3 s reaper on expiry. Row below. |
| 6c | L12: the installer report's DNS time limit | NOT CHANGED (read only) | `installer/install-report.iss` `ReportSend`: `Req.SetTimeouts(1000, 1500, 1500, 1500)` is ResolveTimeout 1000, ConnectTimeout 1500, SendTimeout 1500, ReceiveTimeout 1500 ms and the request is synchronous (`Open('POST', url, False)`), so on paper it is 5.5 s in all, not "about five" as the comment says. Whether WinHTTP stops a stalled DNS query at ResolveTimeout (it hands the lookup to the system resolver) is NOT VERIFIED here (no network, no Inno compiler, the file has never been compiled or run). Bounded by the `try`/`except` (a failure to report never fails the install) and by the feature being OFF (`#define ReportInstallOutcome` is commented out in `cascade.iss`). Nothing in the installer script was changed. |

## R4 in detail (the half the first lane could not fix)

The library (cpp-httplib 0.53.1, `third_party/cpp_httplib`) was read for timeouts and connection limits. What it offers: a read and a write timeout, per `recv`
(`set_read_timeout`; the web server sets 10 s), a keep-alive timeout (5 s) and count (100), a thread-pool size, `set_socket_options` (applied to the listening socket only),
`max_timeout_msec` on `SocketStream` (used by the library's client side, never by its server side) and NO hook on an accepted connection. A head dripped a byte every 9 s
therefore holds a worker for ever.

`net/web_conn_guard.hpp` (new) adds `GuardedServer`, an `httplib::Server` that overrides the library's own private virtual `process_and_close_socket` (a `static_assert` on the
library version makes an upgrade re-read the copy), and `WebServer` now creates one:

1. Each request is read through the library's own `SocketStream` built WITH `max_timeout_msec` (20 s). In the server's stream that total applies to reads only
   (`wait_readable`), so a request head or body must ARRIVE within it however it is dripped, while a response (the audio and live-update streams) is only written and is not
   touched. A request that cannot be read now also ends the connection: before, the library answered 400 and went back to wait for the next request on the same connection,
   which a client that kept dripping kept for ever (this was found by the test, see below).
2. A per-address cap (16) on connections that have shown no request head, counted from accept and again from the end of each answer, released from the pre-routing handler;
   the 17th is closed before a byte is read. Loopback is not counted (the HTTPS relay's connections come from it and it has its own 64/16 limits).

`test_web_conn_guard` (42 checks): a drip of one byte every 300 ms is cut at 1505 ms with 1500 allowed; with the bound off (`requestRead` 0) the same drip is still open after
4 s (so it is the bound that ends it); the second request of a kept-alive connection is bounded the same way (1502 ms); an answer that takes 3.2 s to send under a 1.5 s bound
arrives whole (three chunks and the end); five silent connections with a cap of 3 leave three open and close two at once (`refusedPending` 2), a head on the first frees its
place for a new connection; twelve loopback connections with a cap of 3 are not counted. BREAK-ITS (each seen RED, restored): cap off, three failures; the close-on-read-error
line off, two failures (the cut came at 3070 ms instead of 1505 ms, once the range was tightened to 2500 ms).

WHAT REMAINS (said plainly): an attacker with many different addresses, each holding up to 16 connections for up to 20 s at a time; a client that sends a complete valid
request at the speed it is answered (that is a client). The other httplib servers of the program (`mcp_http.hpp`) are bound to this computer only and were not changed.
The TLS relay's own idle rule (`web_tls.cpp`, a byte every 29 s) is unchanged: behind it the relayed connection now meets the 20 s total on the plain side, which ends the
relay with it (not separately tested: a real HTTPS handshake is not made by the new test).

## M9-api-host-rule in detail

Read both. The page (`net/web_guard.hpp`): any IPv4 or IPv6 literal, `localhost`, this computer's name and `name.local`, and the hosts of "Allowed web origins". The
interface (`net/api_server.cpp` `hostAllowed`, applied with no password, i.e. this computer only): `localhost`, 127.x, `[::1]` and its own bound address; beyond this computer
every request signs in and the Host is not checked. A real inconsistency a user reaches: a program that reaches the interface through a name the owner gave the page (a hosts
alias or a local reverse proxy for `radio.home`, or the computer's own name) is answered by the page and refused 403 by the interface. Fix, in the direction the brief names and
never loosening either: `ApiServer::setHostNames` takes the page's DNS NAMES (`guardHostNames`: the computer's name and `name.local` plus the hosts of the owner's list; IP
literals and the wildcard are not names and give none); the engine supplies them from the same two places the page uses (`sock::computerName()` and the `web.cors_origins`
setting, re-read every second); `WebServer` now builds its own machine names with the same `guardMachineNames`. The interface is NOT widened to "any IP literal" (the page's
rule), to any other name, or to a request with an Origin. Tests: `test_web_guard` (`guardHostNames` is exactly the page's set and the page's own check agrees with each name),
`test_s4b_sec_api` (`testHostNamesOfThePage`: listed names answered in any case, with a port or a trailing dot; `evil.example`, `mypc.local.evil.example`, a literal that is not
its own, an Origin still refused; removing the names removes the permission), `test_engine_api_s4b_account` (`testHostNamesFromTheEngine`: through the real engine, the
computer's own name works at once, a name typed into "Allowed web origins" works within a second, and not before or after). BREAK-IT: the name lookup disabled gave 4 failures
in `test_s4b_sec_api`; restored.

## M9-api-account-e2e in detail

New seam, not a setting: `ServerSet::setApiBindForTest` (the Config already had `apiBindForTest`; the engine builds its Config from settings, which never carry it, so a test
that ticks "every address" through the real engine could not bind loopback). `test_engine_api_s4b_account` drives a real `Engine` (`wire_c_rig.hpp`): E1 "every address" with no
password does not start and says to set one; E2 the password is set with the engine's own SERVER_CONFIG op (hashed by the engine's worker), the interface starts, no credentials
= 401, the login = 200, a WebSocket and an event stream open; E3 the password changed: the old login is refused at once, the new one is let in, both streams end on the engine's
control passes, new streams open under the new password; E4 the password removed (`clearPassword`): the new streams end, the stored login is not honoured (401 for old, new and
no credentials); E5 a password set again, a stream open, then only the user name changed: the stream ends and the old name's login is refused. BREAK-IT: the
`api_->closeAllClients()` of `ServerSet::controlPass` disabled gave three failures (E3, E4, E5 streams stayed open); restored.

## Behaviour changes a user will notice (for the release notes)

- TCI "Reachable from other computers" now also needs "Computers allowed to connect for TCI" (addresses such as 192.168.1.20, networks of 8 to 32 bits, at most 16, separated by
  commas). Without a list the interface does not start and says what to do; connections from other addresses are closed at once. Programs on this computer are unaffected.
  Anyone who had ticked "Reachable from other computers" before this build must type the list once.
- Audio over UDP refuses a broadcast, multicast or 0.x.x.x destination with a sentence.
- A backup that contains an IC-R8600, FUNcube, G39DDC, G33DDC or SDR-IQ radio now comes back on import (before, the radio was left out).
- A client that takes more than 20 seconds to deliver one web request is disconnected; more than 16 connections from one other computer that have not yet sent a request
  are refused. A normal browser is unaffected.
- The program interface (this computer only) also answers a request whose Host is this computer's name or a name in "Allowed web origins".
- Two new sentences in all 33 catalogues (machine-made, not read by a speaker): "Computers allowed to connect for TCI" and the TCI warning, which replaces "The TCI interface has
  no password. Anyone on your network can connect and read what you receive." (that sentence also under-stated what a TCI client can do: it can retune the radio).

## Shared file edits and files outside the lane's own

`src/core/config.hpp/.cpp`, `config_halves.hpp` (one key, `tciAllow`), `sys_bundle.cpp/.hpp`, `sys_profiles.cpp`, `engine/server_host.cpp/.hpp`, `engine/engine_api_lists.cpp`,
`engine/engine_wire_c.cpp` (one hook), `engine/s4b_servers.cpp/.hpp`, `net/web_server.cpp` (the server class, one handler line, the machine names), `net/net_sock.cpp/.hpp`
(`computerName`), `slint/s4_logic.*`, `slint/s4_publish.cpp` (one field, one sentence), `slint/desk_logic.cpp`, `resources/lang/*.json`, `core/lang_assets.hpp` (regenerated),
`tests/errpath_allow.tsv`. No `.slint` file changed.

## Expectations changed (both values)

- `test_sys_bundle`: `importLeftOutKeys().size() == 9` became `== 10` (`tciAllow` joins the keys an import never takes); the hostile bundle now also carries `tciAllow`.
- `test_tci_server` `testLoopbackByDefault`: the bind-all case now starts the server with a list naming the address it connects from (before: no list, which is no longer
  allowed); the assertion (the other address is reachable) is unchanged.
- `test_engine_main_config_roundtrip`: `tciAllow` added to the list of keys the engine writes.
- `tests/test_web_conn_guard.cpp` (new): the range of the two drip tests is `1400..2500 ms`, tightened from a first `1400..4000 ms` that proved too loose to see a break (the
  first break-it passed); no existing expectation was weakened.

## Error-path gate

`test_error_paths`: pending pin 410 before, 410 after (never raised). Four new refusals that hand a sentence to the person carry allow lines with reasons: the TCI list
sentence (`TciServer::start`, `ServerHost::reconcileTci`, which also logs it with `diagWarnf`), the unreadable list typed into SERVER_CONFIG, and the UDP destination.

## Tests run (all `Passed`, `ctest -j2`, this machine)

`test_tci_server`, `test_tci_engine`, `test_web_conn_guard`, `test_web_server`, `test_web_secure`, `test_web_tls`, `test_server_host`, `test_api_server`, `test_s4b_sec_api`,
`test_s4b_sec_servers`, `test_engine_api_s4b_sec`, `test_engine_api_s4b_control`, `test_engine_api_s4b_listing`, `test_engine_api_s4b_account` (new), `test_web_guard`,
`test_sys_bundle`, `test_wire_b_saved_kinds`, `test_slint_host_desk`, `test_engine_main_config_roundtrip`, `test_i18n`, `test_error_paths`; `foxsdr-slint` built (compiles
`s4_publish.cpp`; 26 minutes on a busy machine) and `slint_drive_s4-outputs` run through the hidden-desktop launcher on desktop `FoxSDRSec2`: `Passed` (39.64 s). That drive ticks "Reachable from other computers" with the TCI switch off, so the new field was on the page; it does not type into it (row `SF2-tci-ui-drive`).

## NOT RUN

The full headless gate; the full GUI gate; every GUI drive except `slint_drive_s4-outputs`; `test_web_touch` and `gui_desktop_guard` (they need the hidden-desktop launcher; `gui_desktop_guard` is red without it, by design);
the stamp tests; a real browser against the page; a Linux or macOS build.

## UNVERIFIED

- The two new translations in 32 languages (machine-made from the rtl_tcp sentence of the same shape; Hungarian article adjusted by hand; not read by a speaker).
- The new TCI field and warning by eye in the window.
- The HTTPS relay path behind the new 20 s request total (no real TLS handshake in the new test).
- The Linux `desk_logic.cpp` (only the portable `findOnPath` change was compiled; it is portable code).
- Whether WinHTTP honours ResolveTimeout (L12).
- A subnet-directed broadcast destination of UDP audio: the claim that the system refuses it rests on the socket never setting SO_BROADCAST; no datagram was sent to one.

## OPEN ROWS FOR THE NEXT LANE

| Row | Taken by | What |
|---|---|---|
| S4bS-tci-udp | CLOSED by this lane | TCI list, UDP one-computer rule (above) |
| M8-import-usb-kinds | CLOSED by this lane | five kinds + the option classifier |
| M9-api-host-rule | CLOSED by this lane | names of the page's Host rule |
| M9-api-account-e2e | CLOSED by this lane | `test_engine_api_s4b_account` |
| SF1-R4 | CLOSED for the plain server and for the HTTPS relay's plain side | the many-addresses residual stays (above) |
| SF2-L9-pipes | Linux lane | poll-with-deadline for the opener and clipboard pipes in `desk_logic.cpp realRun` (needs a Linux compile) |
| SF2-L12 | owner / installer lane | if the install report is ever switched on: verify on a real Windows the time to fail with a dead resolver; lower the numbers or time the whole call by a thread |
| SF2-cat-bindall | engine | the CAT server's `catBindAll` has no password and no list of client addresses either (not in this lane's read list); apply `PeerAllow` the same way |
| SF2-tci-ui-drive | screen | a drive that ticks "Reachable from other computers", types a list and reads `fact.tci.allow` back (the field appears only while that tick is on) |
| SF2-translations | speaker review | the two new sentences x 33 catalogues |
| SF2-tls-relay-total | engine | a test with a real TLS handshake, dripped, to see the relay and the plain side end together |
| SF2-api-multi-host | engine | the page refuses a request with two Host headers; `ApiServer::handleHead` looks the header up in a map, so what it does with two was NOT checked here. Read it and make the interface refuse two as well |
