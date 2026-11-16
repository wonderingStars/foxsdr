# Security fixes 1 (wave 8, lane C1): the core side of the two security reviews

Branch `feat/security-fixes-1`, cut from `66072154`. Worktree `D:/foxsdr-wt/en-sysmodels`, build folder `D:/foxsdr-build/en-sysmodels`,
scratch `D:/foxsdr-build/sec1-scratch/`. Nothing was pushed. Reviews: `D:/foxsdr-build/prep/SECURITY-REVIEW-1.md` and `-2.md`.
The machine was kept quiet: `--parallel 3`, only the suites that cover the changed code; NO full headless or GUI gate (see NOT RUN).

## Rows

| Finding | Verdict | Fix | Proof |
|---|---|---|---|
| R1 (Host and Origin never checked; password-less server drivable) | FIXED | `src/net/web_guard.{hpp,cpp}` (pure rules) called from the server's pre-routing handler: a foreign `Host` is 421, a foreign `Origin` 403, on every route. A server with no password is read-only (`/api/control` and the four bookmark change routes answer 403 with a sentence; `/api/session` carries `readOnly`; the page shows the sentence in a badge). | `test_web_guard` (52 checks, pure rules + fuzz); `test_web_server` R1 tests (foreign Host on ten GET routes and a POST; foreign and `null` Origin; no Origin from a tool still works signed in; listed origin reads but never changes; read-only on nine control bodies and four bookmark routes; with a password not read-only). Break-it: guard call removed -> 5 reds (foreign Host 421 checks); read-only rule removed -> 33 reds. Both restored, green (1291 checks). |

## R1: is 0.99.104 affected?

Yes, in the same settings. `v0.99.104:src/net/web_server.cpp` has no read of `Host` or `Origin` anywhere (grep of the whole file), its
`authRequired` is `!loopback || recordUsable` (`web_policy.cpp:180`), so with the web remote switched on (off by default), bound to the default
`127.0.0.1` and with NO password, every caller is the owner, and `POST /api/control` (a same-origin script after DNS rebinding may send
`application/json`) takes `recordIq`, `recordAudio`, `pluginInstall`, `acknowledgeNotice`, `pluginRemove`, `pluginTuneAllowed` and `transmitPtt`
(`web_control.cpp:25-29`; the transmit key is still gated by the desk consent). `/api/status` and `/api/audio` are readable the same way. The
cross-site bookmark routes of review 2 do not exist at 0.99.104 (they arrived with wiring lane C). With a password set, 0.99.104 is not affected
by the rebinding attack (cookie `SameSite=Strict`, sign-in needs the password). Whether the plugin-install path at 0.99.104 refuses third-party
sites was not read.

## Behaviour a user will notice (release notes)

* A web page served with NO password is read-only: it shows status, spectrum and audio, and says "This page is read-only because no web
  password is set..." in a badge; tune, start, stop, record, plugin install or removal and bookmark changes are refused until a password is set.
* The page answers only to this computer's own addresses (any IP address, `localhost`, the computer's name and `name.local`) and to the host
  names of the owner's "Allowed web origins" list. A person who reaches the page by a dynamic-DNS name must put that address in "Allowed web
  origins" (the page's own origin).
* A request that carries another web site's `Origin` is refused (403), unless the site is on the "Allowed web origins" list, which may then READ
  (GET, OPTIONS) but never change anything.

## Expectations changed

| Test | Was | Now | Why |
|---|---|---|---|
| `test_engine_api_wire_c_web.cpp` (testCors, first request) | `GET /api/session` with a foreign Origin and no list: status 200 and no CORS header | status 403 and no CORS header | R1: a request from another site is refused outright |

Fixtures changed (no expectation touched): `test_web_server.cpp` `startOnFreePort`, `test_remote_key_release.cpp` `startOnFreePort` and
`test_server_host.cpp` `Rig` call `WebServer::allowOpenControlForTests(true)` (the tests there are about what a control does on an open server;
the R1 tests pass `false`); `test_stop_ends_recordings.cpp` now runs its child application WITH a web password and signs in; `web_tls_client.hpp`
sends `Host: 127.0.0.1` instead of `Host: test`.

## SHARED FILE EDITS

* `tests/CMakeLists.txt`: `net/web_guard.hpp` added to the fence probe list (one token).
* `src/net/web_server.cpp/.hpp`, `web_wire_c.hpp`, `web_wire_c_bookmarks.cpp`: the guard, the read-only rule, the hook `mayChange`.

## OPEN ROWS FOR THE NEXT LANE

| Row | Kind of lane |
|---|---|
| R7, R8 (rtl_tcp, TCP audio and API servers of `feat/screen-s4b`; also R8's missing Host check on the API server) | a lane cut after `feat/screen-s4b` is merged |
| R11, R12, L2, L9, L12 | hardening lane (not tonight) |
| L13 | withdrawn by review 2 (not a finding) |
