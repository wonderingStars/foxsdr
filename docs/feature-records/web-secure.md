# Secure and multi-user web page: HTTPS with Schannel, owner and listeners, directory research (branch feat/web-secure)

Date 2026-11-11. Worktree `foxsdr-wt/feat-web`, branch `feat/web-secure` cut from `feat/web-bookmarks` 762ee55a, build `D:/foxsdr-build/feat-web` (configured `-DCASCADE_BUILD_SLINT=OFF -DCASCADE_SOFTGL=OFF`). Nothing pushed or merged. Rows N13 (HTTPS), N12 (multi-user), N14 (directory listing: research only). Brief: `D:/foxsdr-build/prep/queued-briefs/unblocked-web.md`. The brief said `src/web`; the web server lives in `src/net` (web_server, web_auth, web_policy, web_control, web_session, web_audio).

Real config.json sha256 after the lane: `c21ce98dc30e50327f7666f7ee29047c0983d130ca3f21716d0c0e20e04603c3` (unchanged). No radio opened, nothing downloaded, no account made, no outside connection (stand-in servers and loopback / this machine's own LAN address only). Windows leftovers checked at the end: 0 `FoxSDR` certificates in the current user's Personal store, 0 `FoxSDR` keys in the Software Key Storage Provider.

## What existed (read first)

* `WebServer` = a vendored cpp-httplib `Server` (plain HTTP). TLS in httplib needs OpenSSL, which the product does not ship (THIRD_PARTY.md); Windows builds link none.
* One account (`username` + a PBKDF2 `passwordRecord`), one `SessionStore` (cap 32, 12 h), a `LoginThrottle` keyed by `req.remote_addr`, the engine session shared by every browser, audio and live-update streams capped at 4 each, `kClientIoTimeout` = 10 s read/write on the server.
* Bind policy (`evaluateBind`): no off-machine bind without a password, etc.

## 1. HTTPS (N13) — design and why

**Choice: a TLS terminator of our own on the public port, in front of the unchanged plain server.** `src/net/web_tls.{hpp,cpp}`: Schannel (SSPI: `AcquireCredentialsHandle`, `AcceptSecurityContext` loop, `EncryptMessage`/`DecryptMessage`) and CNG. Accept thread plus one thread per connection (registered in the thread inventory), each doing the handshake with a deadline and then a `select` loop relaying plaintext to `127.0.0.1:<private port>`. Considered and rejected: patching httplib to take a custom stream layer (touches 15k lines of vendored code on every update, and the 747 KB header is in one TU by design); OpenSSL (not shipped, not allowed); `SslServer` from httplib on Windows (needs OpenSSL).

* While HTTPS is on, the plain server is bound to **127.0.0.1 on a port the system picks**: plain HTTP is never offered to the network. Tested: a plain request to the public port gets no HTTP answer; the private port answers on loopback and is refused on every LAN address of this machine (5 tried).
* **True client address.** The plain server sees only the relay. Nothing is passed in a header (a client writes those). The terminator records, per relayed connection, the connection's own loopback source port against the real peer it accepted from; `req.remote_port` is looked up (`peerOf`). The limiter and the ban list use it (`clientAddress`). An unknown port (a program on this machine talking to the private port directly) reads as 127.0.0.1, which is what it is. Tests: from this machine's LAN address with `X-Forwarded-For`/`X-Real-IP`/`Forwarded: 127.0.0.1` the LAN address is throttled / banned while loopback is not; from loopback claiming the LAN address nothing is inherited; banning loopback flips it.
* **Protocols.** `SCH_CREDENTIALS` with `TLS_PARAMETERS::grbitDisabledProtocols = SSL2|SSL3|TLS1.0|TLS1.1`: 1.2 is the floor, 1.3 whatever Windows offers (**this Windows 11 22631 negotiates TLS 1.3**: `stats.tls13`). A client's renegotiation request ends the connection.
* **Certificate.** Self-signed ECDSA P-256 made with CNG; SAN = this computer's DNS name, `localhost`, 127.0.0.1, ::1, every LAN IPv4 (`localInterfaceAddresses()`), plus any extra names; basicConstraints CA:false, keyUsage digitalSignature, EKU serverAuth, 2-year validity; **private key not exportable** (`NCRYPT_EXPORT_POLICY_PROPERTY = 0`; a test asks the key store to export the private blob and is refused). Two modes: no `tlsCertFile` → a key with a random name for this run, deleted when the credential is released (new fingerprint each start); `tlsCertFile` + `tlsKeyName` → key kept in the user's key store, public certificate kept in the file, reused until 30 days from expiry, regenerated (same key) if the file is damaged or belongs to another key. Or **picked by thumbprint** (40 hex SHA-1, the digits Windows shows) from Personal of CurrentUser then LocalMachine; needs a private key and a valid date. SHA-256 fingerprint of the DER is published: `WebServer::tlsInfo().fingerprintSha256` (also `thumbprintSha1`, `names`, `notAfter`, `selfSigned`, `keyExportable`, `stats`).
  * Two Windows facts found the hard way: `CertCreateSelfSignCertificate` refuses an ECDSA CNG key (`NTE_BAD_FLAGS`), so the certificate is built by hand and signed with `CryptSignAndEncodeCertificate`; Schannel cannot use a key that exists only as a handle (`SEC_E_NO_CREDENTIALS`), it finds a certificate's key by name in the key store, hence the named run key. **Residue:** if the process dies between creating and releasing, one `FoxSDR-Web-TLS-run-<hex>` key stays in the user's key store (small; not swept; see decisions).
* **Limits.** Handshake 10 s, idle 30 s (the live-update stream keeps a 15 s keep-alive, audio flows), write stall 10 s, 64 connections total, 16 per address; `WebServer::setTlsLimits` lets a test shorten them. `stop()` is prompt: the terminator is stopped first (closing the public connections ends the relays and with them the plain server's parked workers).
* **Cookie** gains `Secure` exactly when HTTPS is on. **OFF by default** (`tlsEnabled = false`).
* **Linux:** `web_tls.cpp` compiles to stubs (`available()` false, every call fails with "…UNPROVEN on Linux"); `evaluateBind` refuses `tlsEnabled` there (`RefusedTlsUnavailable`). **Not built or run on Linux.**
* Linked on Windows: `secur32 crypt32 ncrypt` (CMakeLists.txt, `cascade_engine`). New thread specs `kThreadWebTlsAccept`, `kThreadWebTlsConn` in `core/thread_beats.hpp` (the only edit outside `src/net` besides CMake and the test lists) and the inventory row.

## 2. Multi-user (N12) — design

Definition as decided: **OWNER** (the existing account) full control; **LISTENERS** look and listen. `src/net/web_users.{hpp,cpp}` (`WebRoom`: pure, injected clock, no I/O) wired into `web_server.cpp`.

* **Sign-in.** One door, one page form. The owner's account name + owner password = owner; with listeners on, **any other name** + the one shared **listener password** = a listener with that name as its handle (1–24 of ASCII letters, digits, `_ - .` and single inner spaces; a name equal to the owner's, in any case, is refused). A wrong answer is the same message and costs the same single PBKDF2 whichever door and whatever is wrong (tested: four kinds of wrong credential give byte-identical bodies). Address bans are checked before any password on the listener door only, so a ban can never lock the owner out (the throttle covers that door).
* **What a listener may do.** Every control request 403 (audited) unless `listenersMayTune`; then only `centerHz, vfoOffsetHz, mode, bandwidthHz, bookmarkTune` (`listenerMayTune()` = "after removing those fields nothing is left", built on `ControlRequest::empty()` so a field added later is refused until decided). The transmit key is never a listener's. **Finding:** `ControlRequest::empty()` does not list `acknowledgeNotice`; `listenerMayTune` names it. A listener's status poll is **not** the engine session's heartbeat (a frozen owner's page with a key down must not be kept alive by a listener): tested with a counting stub session.
* **A listener's view** (status JSON and the live-update stream) is redacted: antenna position, record folder, device labels/serials/args, antenna list, plugin file names, catalogue and installer text, raw error texts. Signals, decoded text and tracks stay (that is what looking is). Decision for the owner below.
* **Limits.** Cap default 4, ceiling 32 (clamped in the room, refused by the bind policy); idle limit 15 min (activity = a non-stream request; a stream re-check does not reset it, so an abandoned tab with audio still ends), session limit 8 h (0 = none; a cookie never outlives 12 h); stream caps rise to `max(4, cap + 2)` and the httplib pool is sized for them. 
* **Moderation** (owner only, `POST /api/admin`, `GET /api/admin/state`; also `WebServer::room()` for a settings panel): `kick`, `ban` (name, and the address the listener is on), `banAddress`, `unban`, `unbanAddress`, `note`. A kick/ban ends a live audio and live-update stream (re-checked every 250 ms; measured 182–308 ms). Bans live in memory until the server stops (see decisions). Listener-count accessor `WebServer::listenerCount()`.
* **Note:** one escaped line to all listeners (`/api/room`). **Chat:** OFF by default, text only, 200 bytes after sanitising (valid UTF-8 only, control, bidi-override and zero-width characters removed), 3 lines per 10 s per person, ring of 50, escaped (`& < > " ' \``) before it leaves `web_users.cpp`, the speaker is the session's never the body's.
* **Never on disk or in a report:** `web_users.{hpp,cpp}` are source-scanned for file/console calls; during the chat tests the process's stderr is redirected to a file and searched (no name, address or text); the audit trail is in memory, owner-only, kind + chosen name only (a test checks no address and no typed text appears in it).
* **Page** (kAppJs4 and HTML/CSS): a listener sees no control column / Start / Mute / Volume; without the tune switch the readout and spectrum take no clicks; a Listeners panel (count of cap, note banner, chat, and for the owner the list with Kick/Ban, ban lists with Allow, the note box); the login page says how a listener signs in. Looked at in the browser pane against a real server (`UW_SERVE=240 test_web_secure.exe <root>`): listener view, owner view; a note containing `<b>` and a chat line containing `<img onerror>` both appear as literal text with no element created. The only `innerHTML` in the room's script are the two lines that take the server's escaped html (a test counts them).
* **NOT BUILT: independent tuning per listener.** It needs: a demodulator/channel per listener fed from the shared I/Q (a channelizer; CPU grows per listener), an audio stream per listener from the engine (today one cursor per listener over one stream), engine ops scoped to a session instead of the global receiver state, per-listener spectrum zoom and status, a rule for plugins that replace the audio (they replace it for everyone), and the limit that listeners can only tune inside the sampled bandwidth. With the multi-radio engine the cheaper step is a listener choosing among open radios.

## 3. Public directory listing (N14): research only, nothing connects

Read-only web reading on 2026-11-11 (sources below). **Findings:**
* **Receiverbook** (`receiverbook.de`, operated by the OpenWebRX author per its imprint) lists receivers of three software types, named on its pages: OpenWebRX, WebSDR, KiwiSDR. "Report a new receiver" (`/receivers/new`) is one field, **URL**, and a submit button; the page does not say whether login is needed, what is fetched from the URL, or any requirement beyond the three software types. No protocol, API or key is documented on any page I could open (`/about`, `/help`, `/faq`-style paths are 404). The only legal text is an imprint with a liability disclaimer; **no terms of use and no privacy statement** are stated.
* The OpenWebRX site says a documentation page exists on the project's GitHub wiki; that wiki belongs to an AGPL project and, under the lane's clean-room rule, I did **not** open it.
* KiwiSDR's own public list (`kiwisdr.com/public`) refused my connection (ECONNREFUSED) twice; WebSDR's (`websdr.org`) redirected http↔https in a loop. Not read.
* sdr.hu is shut down (rtl-sdr.com).
* **Conclusion.** FoxSDR's page is none of the three software types, so it cannot be listed there as itself today, and nothing was found that says Receiverbook accepts anything else; whether it probes the submitted URL for one of those protocols is **UNVERIFIED**. Nothing here is a patent or legal block: the missing thing is documentation. Free routes to unblock: ask the operator (contact is on the site's imprint; mailing lists and GitHub are what it points to) whether a generic listing exists; or read the OpenWebRX registration documentation with the owner's agreement that this is acceptable under the clean-room rule; or list the page by hand on a general directory that takes a plain URL. **Nothing was built that connects**, and a listing would put a receiver's location and address in a third party's hands: it would have to be opt-in with the page's HTTPS and listener password on (the manual's warning applies).

Sources opened (all read-only, no login): https://www.receiverbook.de/ (status: public page, no licence stated), https://www.receiverbook.de/receivers/new, https://www.receiverbook.de/map, https://www.receiverbook.de/impressum (imprint, disclaimer only), https://openwebrx.de/ (no licence stated on the page), https://www.dxzone.com/dx36934/receiverbook-online-directory-of-websdr-and-kiwisdr-receivers.html (a third-party blurb), https://www.rtl-sdr.com/openwebrx-directory-sdr-hu-has-now-shut-down/ (news). Not opened by rule: any OpenWebRX/KiwiSDR/WebSDR source or wiki. Also opened for the Windows API facts: none (Schannel/CNG behaviour was established by running it; no documentation page was fetched beyond the lane's own memory of the SDK headers, which were read locally: `schannel.h`, `sspi.h`, `ncrypt.h`, `wincrypt.h`).

## 4. Manual

`docs/MANUAL.md`: new section **HTTPS and listeners** (plain warning about the internet first; status line that the settings screen does not carry the switches yet), and the old "There is no TLS" bullet rewritten. **The stale Start/Stop line the brief mentioned (near line 1159) was already corrected by feat/web-bookmarks** (the toolbar paragraph now says the page has Start/Stop and Mute); nothing left to fix there.

## Results (real lines, Release, D:/foxsdr-build/feat-web; the machine was shared and loaded)

| Test | Result |
|---|---|
| test_web_users (new) | `625 checks, 0 failed` |
| test_web_tls (new; Schannel client written in the test + curl.exe 8.13 Schannel + hand-made hellos + fuzz) | `195 checks, 0 failed` (4 consecutive runs after the load-tolerance fixes) |
| test_web_secure (new; real WebServer, stub engine session, plain and HTTPS) | `797 checks, 0 failed` (2 consecutive runs) |
| test_web_policy | `192 checks, 0 failed` (149 before, +43) |
| test_web_server | `1184 checks, 0 failed` (the count moves by ±2 with the opt-in measurement block and timing rows, as the previous record says) |
| test_web_control / test_web_audio / test_web_pictures / test_web_auth / test_receiver_snapshot | `264` / `133` / `48` / `409` / `802`, all `0 failed` |
| test_thread_inventory | `9881 checks, 0 failed` (25 specs, all registered) |
| test_thread_beats | `832 checks, 0 failed` |
| test_shutdown_budget | `204 checks, 0 failed`, 90 s |
| fence probes `interface_fence_denies_net_web_users_hpp`, `..._web_tls_hpp` (ctest) | Passed |

Measurements printed by the tests: silent half-open client dropped after 1259–1296 ms against a 1200 ms limit; terminator `stop()` with 3 idle + 1 streaming + 1 half-open connection 27 / 123 ms; `WebServer::stop()` with HTTPS on and the same plus a live audio stream 119–559 ms (budget unchanged: the product charges `kClientIoTimeout` = 10 s once for the plain server and this adds nothing to the teardown path); a kick, ban and address ban end a listener's audio and live-update streams in 182–308 ms; 12 concurrent listener sign-ins at cap 4 → exactly 4 admitted and 8 refused (`503`), repeated at the ceiling of 32 in the module test.

Not run: the full ctest suite (about 2500 tests, shared machine); only the web tests above, the thread/budget/fence tests and the policy.

## Break-it table (script `D:/foxsdr-build/uw-breakit.py`, log `D:/foxsdr-build/uw-logs/breakit.log`: one line mutated, targets rebuilt, the named test run, file restored with `git checkout --`, then everything rebuilt and re-run)

| Behaviour removed | Test that went RED |
|---|---|
| TLS 1.0/1.1 left enabled in the credential | test_web_tls OldProtocols (the TLS 1.1 hello got a ServerHello) |
| handshake time limit | test_web_tls Stalled (`c.peerClosedWithin(4000)`) |
| idle limit | test_web_tls Stalled (`peerClosedWithin(5000)`) |
| write-stall limit | test_web_tls Stalled (`droppedWriteStall` never rises) |
| key made exportable | test_web_tls Certificate (`!keyExportable`, the export refusal) |
| per-address connection cap | test_web_tls Caps |
| real peer address never recorded | test_web_tls TrueClient (`peer` empty) |
| plain server bound to every interface under HTTPS | test_web_secure ServerOverHttps (private port reachable on the LAN) |
| true client address ignored (`req.remote_addr` used) | test_web_secure TrueAddress |
| `Secure` cookie attribute | test_web_secure ServerOverHttps |
| listener role gate on /api/control | test_web_secure EveryOpAsAListener (13 failed) |
| tuning-only filter | test_web_secure EveryOpAsAListener (81 failed) |
| listener status poll counts as the heartbeat | test_web_secure CraftedEscalation (heartbeat count) |
| owner-only gate on the admin routes | test_web_secure CraftedEscalation (24 failed) |
| status redaction for listeners | test_web_secure Redacted (10 failed) |
| audio stream re-check of the session | test_web_secure Moderation (hung until killed at 300 s = RED) |
| live-update stream re-check of the session | test_web_secure Moderation (hung until killed at 300 s = RED) |
| HTML escaping of `<` | test_web_users (4 failed) and test_web_secure ChatNote (3 failed) |
| listener cap | test_web_users (10 failed) and test_web_secure ConcurrentJoins |
| chat rate limit | test_web_users and test_web_secure ChatNote |
| a name ban ending the live session | test_web_users (`ended == 1`); test_web_secure stayed green because the server's `ban` also bans the address, which ends the session (two layers) |
| owner name reserved against listeners | test_web_users (ReservedName) and test_web_secure SignIn |
| `listenerMayTune` returns true | test_web_users (80 failed) |
| listener idle limit | test_web_users and test_web_secure IdleAndSession |
| **banned address refused at the server's door** | **GREEN: an equivalent mutant**, `WebRoom::joinListener` refuses a banned address too (two layers, same behaviour); not a test gap, but the door's own check is unproven alone |
| **`authRequired` forced on by listeners** | **GREEN: an equivalent mutant**, a listener config needs a usable owner password and `recordUsable` already makes `authRequired` true |

After restoring, test_web_users, test_web_policy and test_web_secure were green; test_web_tls was red once with three timing rows (a positive-control hello, the connection-count row, a fuzz counter) under load, which is why those rows now poll or wait longer; four runs after that were green.

## UNVERIFIED / not done

* **Linux:** nothing built or run (stubs and the policy refusal only).
* **A real browser over HTTPS** (Chrome, Edge): only the lane's Schannel client and curl.exe were used; the browser pane was used on plain HTTP only. The first-visit certificate warning and "continue" flow are as described, not observed.
* IPv6 listening of the terminator (`::`), a picked certificate from the **LocalMachine** store, a picked **RSA** certificate (TLS 1.3 with RSA-PSS), a client-initiated TLS 1.3 key update (it ends the connection), session resumption behaviour, very slow clients that read a byte at a time.
* The real engine behind the page: the tests use a stub `WebSession` (heartbeat/submit counting, 0.5-level audio). The heartbeat rule is proved against the stub; the transmit dead-man's reaction to it is the existing engine tests' business and was not re-run.
* Certificate names go stale if the machine's LAN address changes after a kept certificate was made (names are fixed at creation; regeneration is by deleting the file). Not handled.
* The settings screen and config plumbing (below). The window cannot show the fingerprint or the listeners until then.
* Facts `fact.web.*` are not published (they live in `engine/fox_engine_api.cpp`, which this lane may not edit).
* Idle/session limits are tested with an injected clock; a real hour was not waited.
* The look of the room panel at real widths (the pane is 0×0 hidden here): logic was driven by script, layout was not seen.
* Full ctest suite not run.

## Wiring and screen hand-off (every control: English label, op or fact, range, default)

Nothing below exists in the window, `AppConfig` or the engine table yet; the web server is complete without them.

**A. ServerHost (engine, other lane's files):** copy these new `WebServerConfig` fields from the config on every `start()`: `tlsEnabled, tlsCertThumbprint, tlsCertFile, tlsKeyName, listenersEnabled, listenerPasswordRecord, maxListeners, listenerIdleSeconds, listenerSessionSeconds, listenersMayTune, chatEnabled`. Show `WebServer::start`'s error text verbatim (it now begins "HTTPS: " for a certificate problem and names the port for a bind problem). The default `tlsCertFile` should be a file beside `config.json` (e.g. `web-tls.cer`) so the fingerprint is stable between runs.

**B. AppConfig keys** (all hashed/never plaintext for passwords): `webTlsEnabled` false; `webTlsCertThumbprint` ""; `webTlsCertFile` "" (the host fills a default); `webListenersEnabled` false; `webListenerPassword` (PBKDF2 record, same format and the same "password-hash" worker as the owner's); `webMaxListeners` 4 (1–32); `webListenerIdleSeconds` 900 (0 = none); `webListenerSessionSeconds` 28800 (0 = none); `webListenersMayTune` false; `webChatEnabled` false. **Do not persist the ban lists** (addresses on disk; owner decision below).

**C. Web access section controls (English strings):**
| Label | Writes | Range / default |
|---|---|---|
| "Use HTTPS" | webTlsEnabled | off. Windows only; elsewhere shown disabled with "HTTPS is not available on this platform" |
| "Certificate" (choice: "Make one for me" / "Use a Windows certificate") | thumbprint empty / set | "Make one for me" |
| "Certificate thumbprint" (text, shown for the second choice) | webTlsCertThumbprint | 40 hexadecimal digits, no spaces |
| "Certificate fingerprint (SHA-256)" (read-only, copy button) | fact web.tls.fingerprint | shown while the server runs |
| "Certificate valid until" (read-only) | fact web.tls.notAfter | |
| "Allow listeners" | webListenersEnabled | off; refused with the policy's sentence while the owner password is unset |
| "Listener password" + "Set" | webListenerPassword | at least 8 characters (kMinPasswordLength) |
| "Listener limit" | webMaxListeners | 1 to 32, default 4 |
| "Sign a listener out after (minutes of nothing)" | webListenerIdleSeconds/60 | 0 to 1440, default 15, 0 = never |
| "Sign a listener out after (hours)" | webListenerSessionSeconds/3600 | 0 to 12, default 8 |
| "Listeners may tune" | webListenersMayTune | off |
| "Chat between listeners" | webChatEnabled | off |
| "Listeners now: N" (read-only) | fact web.listeners | |
| the plain warning text from the manual, under "Use HTTPS" | | always shown when the bind address is not loopback |

**D. Facts to publish** (from `WebServer::tlsInfo()` / `listenerCount()` on the engine's side): `fact.web.tls.active` (0/1), `fact.web.tls.fingerprint` (64 hex), `fact.web.tls.thumbprint` (40 hex), `fact.web.tls.notAfter` (epoch s), `fact.web.tls.selfSigned` (0/1), `fact.web.tls.port`, `fact.web.listeners` (count). No names or addresses are ever a fact (they would reach reports).

**E. Moderation from the window** needs engine ops (the interface must not include `net/`): `FOXAPI_OP_WEB_KICK(name)`, `..._WEB_BAN(name)`, `..._WEB_UNBAN(name|address)`, `..._WEB_NOTE(text ≤200 bytes)`, and a read of the listener list (owner-local only, never into a report). The page itself already has all of it.

**F. Tests to keep running with the port:** test_web_users, test_web_tls, test_web_secure (they take the source root as argument; the CMake branch is in tests/CMakeLists.txt next to test_font_assets).

## Decisions for the owner

1. **Persist bans?** Addresses on disk contradicts "nothing personal on disk"; I kept bans in memory (cleared at every server start). A persisted *name* list would be less sensitive.
2. **Listener accounts:** one shared listener password plus a free-text handle (a ban by name is only as strong as the handle; the ban by address is the sturdier half). Per-listener accounts would need a user store on disk.
3. **What a listener sees:** I redacted position, folders, device identifiers, plugin file names, error texts. Decoded text and tracks stay visible. Say if listeners should see less (for instance no decoded text) or more.
4. **Owner exempt from address bans** (so a shared NAT address cannot lock the owner out). The throttle still covers the owner's door.
5. **Run key residue** after a crash (one small key in the user's key store); a sweep of stale `FoxSDR-Web-TLS-run-*` keys at start would risk deleting a sibling process's key. Leave, or sweep keys older than a day?
6. **Certificate lifetime and names:** 2 years, names fixed at creation. Regenerate when the LAN address changes (a new fingerprint each time, which makes browsers warn again)?
7. **Directory listing:** ask the Receiverbook operator whether a generic listing exists, and whether to read the OpenWebRX registration documentation (AGPL project) under the clean-room rule.
8. **Merge hot spots:** `src/net/web_server.cpp` (big, mostly additive; the login/session block, status/events/audio/control handlers and `start`/`stop` changed), `src/net/web_policy.{hpp,cpp}` (new fields and verdicts), `src/core/thread_beats.hpp` (two specs), `CMakeLists.txt` (one `target_link_libraries` line), `tests/CMakeLists.txt` (the source-root branch and the fence list), `tests/thread_inventory_rows.inc` (one row). `test_web_server`'s golden status file is untouched (no new status field: the room has its own routes).
