# Transports — how an interface reaches an engine (draft 0.2)

Status: the **in-process** transport is implemented and tested. The
**out-of-process** transport below is a design; nothing of it is built yet
(README, "What remains").

There is one API (docs/API.md) and two ways to carry it. The rule that makes
the second possible: **the remote client library implements the same
`FoxEngineApi` table** the engine module exports. An interface plugin is
handed a table and a session and cannot tell which transport is behind them,
so the standard interface runs unchanged in both.

```
 in-process                               out-of-process
 ┌───────────── one process ───────────┐  ┌──── interface machine ────┐        ┌──── engine machine ─────┐
 │ interface plugin                    │  │ interface plugin          │        │ engine                  │
 │   │ C calls (FoxEngineApi)          │  │   │ C calls (FoxEngineApi)│        │   ▲ FoxEngineApi        │
 │   ▼                                 │  │   ▼                       │  wss:// │   │                     │
 │ engine module                       │  │ foxsdr remote client ─────┼────────┼─► session server        │
 └─────────────────────────────────────┘  └───────────────────────────┘        └─────────────────────────┘
```

## 1. In-process: the C table

- **What it is.** An engine module (`.dll`/`.so`) exports
  `foxsdr_engine_query`; an interface module exports
  `foxsdr_interface_query`; a host (`foxsdr-host` here, the FoxSDR app after
  extraction) loads both, opens a LOCAL session and calls the interface's
  `run` on the process main thread.
- **Isolation between modules.** Modules load with `RTLD_LOCAL` / plain
  `LoadLibraryW`, build with hidden visibility and, on Linux,
  `--exclude-libs,ALL`, so each module's statically linked ImGui, GLFW or
  C++ runtime pieces never interpose on another's. Only pure C crosses the
  boundary - no exception ever does, and every function is `FOXAPI_CALL`
  (cdecl on 32-bit Windows) - and every buffer is caller-allocated, so the
  two sides need not share a C runtime or a compiler.
- **Cost.** Function calls and bounded copies. Measured for the standard
  interface against the mock engine: 5.2 µs per frame in API calls on
  Windows (heartbeat, one state read of ~700 bytes, one spectrum read of up
  to 1024 floats, one batched submit when something changed, a results and
  an events poll).
- **Failure model.** A crash in an in-process interface takes the engine
  with it. That is the trade for speed, and it is why the out-of-process
  mode exists.

## 2. Out-of-process: WebSocket

### 2.1 Connection and authentication

- **Transport:** WebSocket (RFC 6455) over TCP, subprotocol
  `foxsdr.api.v0`. On any non-loopback address **TLS is required** (`wss://`).
  Plain `ws://` is accepted on loopback only (a local crash-isolated
  interface) - and a loopback connection is **authenticated like any other**
  (below): being on the same machine proves nothing about who is connecting.
- **Every upgrade is checked twice before a session exists**, whatever the
  address and whatever the credential:
  1. **`Host`** must be one of the names this listener is configured to
     answer to: `localhost`, `127.0.0.1` or `[::1]` with its port on
     loopback, and on a remote bind the configured host name or literal
     address with its port. Anything else is refused with 421 and no
     session. This is what defeats **DNS rebinding**: a hostile page whose
     own name has been re-pointed at 127.0.0.1 still sends ITS name in
     `Host`.
  2. **`Origin`**, when present, must be on the listener's allow-list:
     empty by default, plus the engine's own web page origin when the web
     page is enabled, plus origins the local operator adds (LOCAL + ADMIN,
     `SERVER_CONFIG`). A browser always sends `Origin` on a WebSocket
     upgrade (Fetch standard §3.2, "append a request `Origin` header": for a
     request whose mode is "websocket" the header is appended
     unconditionally; RFC 6455 §4.1, client requirement 8: the request MUST
     include `Origin` "if the request is coming from a browser client"), so
     an arbitrary web page is refused with 403 even on loopback and even when
     it could guess or steal nothing else. **`Origin: null` is a browser**:
     the serialisation of an opaque origin is the string `null` (RFC 6454
     §6.2), and Fetch sends `null` for a request whose redirect-taint is not
     same-origin ("serializing a request origin"), so a sandboxed frame, a
     `data:` or `file:` page or a redirected request says `null`. `null` is
     never on the allow-list and cannot be added to it. A native client (the
     remote client library) sends no `Origin` - the header ABSENT, not empty
     and not `null` - and is judged by its credential alone. **Absence of
     `Origin` proves nothing**: any non-browser program can omit the header
     or send a false one (RFC 6455 §10.1), so the `Origin` check only keeps
     browsers out; it never admits anyone. The credential is the gate.
- **Bind:** loopback by default. Binding any other address is refused
  unless a password record is configured (the app's `web_policy` rules,
  unchanged: literal addresses only, no leading-zero octets, port ≥ 1024, a
  corrupt password record refuses the bind on every address).
- **Certificates:** the engine creates a self-signed certificate the first
  time remote access is enabled and shows its SHA-256 fingerprint; the
  client pins it on first use and refuses a changed certificate unless the
  user accepts the new fingerprint. A user may install their own
  certificate instead.
- **Login (remote):** `POST /api/login` with user name and password (the
  app's existing PBKDF2-HMAC-SHA256 password record, 600 000 iterations, and
  its `LoginThrottle` — failures are rate limited per client address with
  `Retry-After`) is the transport form of the table's `login()`; the answer
  is a 256-bit random session token, valid at most 12 hours (the app's
  `kSessionTtlSeconds`), revoked by `POST /api/logout` (the table's
  `logout()`, which also detaches every session opened with it) or by
  changing the password. The token store keeps only a hash of each token,
  so a copy of the engine's memory or config cannot be replayed. The
  WebSocket upgrade carries `Authorization: Bearer <token>`; a browser
  client may use the existing `HttpOnly; SameSite=Strict` cookie instead
  (with the `Origin` check above, which applies to every upgrade, not only
  cookie ones).
- **Login (loopback), and what it makes the session:** a local
  out-of-process interface - the desktop app's crash-isolation mode, where
  the standard window runs in its own process - authenticates with a
  **local token file** instead of a password: when the listener starts, the
  engine writes a fresh 256-bit token to a file readable only by the user
  the engine runs as (the file rules are below), replaced on every start and
  deleted on stop. **Decision: a loopback connection that presents THIS
  token, and whose upgrade has NO `Origin` header at all (absent; an empty
  or `null` `Origin` is a browser and is refused), gets a
  `FOXAPI_SESSION_LOCAL` session** -
  it may latch, arm remote transmit and hold ADMIN, exactly as the same
  window would in-process. That is deliberate: a process that can read the
  file is already that user, who could equally load an interface plugin
  into the engine; the file grants nothing the user did not have. Without it
  the crash-isolated window would lose the latch and could never give the
  consent remote PTT needs. **The token is the only gate**: requiring no
  `Origin` keeps a browser from using a token it might somehow hold, but a
  missing `Origin` proves nothing about the caller (above), so nothing is
  ever granted for its absence alone. Everything else - a login token, even
  over loopback, and every browser (cookie) session - is
  `FOXAPI_SESSION_REMOTE`.
  The token still arrives as `Authorization: Bearer`, so the two share one
  code path, and the `Host` and `Origin` checks still apply to both.
- **The local token file - rules the engine enforces, on every platform:**
  1. **Where.** Windows: `%LOCALAPPDATA%\FoxSDR\engine.token` (the path from
     `SHGetKnownFolderPath(FOLDERID_LocalAppData)`, never from an
     environment variable). Linux: `$XDG_RUNTIME_DIR/foxsdr/engine.token`;
     when `XDG_RUNTIME_DIR` is unset, empty, not absolute, or fails check 3,
     the fallback is `$HOME/.local/state/foxsdr/run/engine.token` (each
     missing directory created 0700), and when that fails check 3 too the
     file is NOT written: out-of-process LOCAL mode is unavailable and the
     engine says so (the in-process window is unaffected). Never `/tmp` or
     any other shared directory. macOS: the per-user temporary directory
     from `confstr(_CS_DARWIN_USER_TEMP_DIR)` + `foxsdr/engine.token`, with
     the same checks and the same fallback under
     `$HOME/Library/Application Support/FoxSDR/run/`.
  2. **Who can read it.** Windows: the file is created with a security
     descriptor whose DACL is PROTECTED (no inherited ACEs; SDDL
     `D:P(A;;FA;;;<user SID>)`) and grants only the engine's own user
     SID - passed in `CreateFileW`'s `SECURITY_ATTRIBUTES`, so there is no
     moment at which it exists with an inherited ACL. Linux and macOS: the
     `foxsdr` directory is 0700 and the file is created with
     `open(O_CREAT | O_EXCL | O_NOFOLLOW, 0600)` under a 077 umask.
  3. **Where it refuses to write.** In a directory the engine's user does
     not own (Linux/macOS: `st_uid` of every directory it creates or uses
     under the base must equal `geteuid()` and its mode must be exactly
     0700; Windows: the directory's owner SID must be the user's), through
     a symbolic link or reparse point, or on a network filesystem (Windows:
     a UNC path or `GetDriveTypeW` = `DRIVE_REMOTE`; Linux: `statfs` type
     NFS, CIFS/SMB, 9P or FUSE; macOS: `statfs` without `MNT_LOCAL`). A
     refusal is logged (without the token) and LOCAL mode is unavailable.
  4. **Rotation.** A token is good for ONE successful LOCAL handshake: once
     an upgrade has presented it and been accepted, the engine writes a new
     token (a temporary file created by rules 2-3 in the same directory,
     then renamed over the old one) and the old one no longer opens
     anything. A reconnecting window reads the file again. It is also
     replaced on every start and deleted on stop.
  5. **Never anywhere else.** The token is never passed on a command line or
     in an environment variable (both are readable by other processes), and
     never written to a log, a crash dump, a problem report or telemetry;
     the engine holds it only as long as it takes to write the file.
  6. **Kept as a hash, compared in constant time.** The engine keeps only
     the SHA-256 of the current token and compares the SHA-256 of what an
     upgrade presents against it with a constant-time comparison, so
     neither a memory dump nor timing reveals it.
- **The session:** one WebSocket = one session, LOCAL for the local token
  file (above), otherwise `FOXAPI_SESSION_REMOTE`. The server opens a remote
  one with the token's grants (never ADMIN) and **closes it when
  the socket closes, errors, or its interface stops beating past the remote
  keep-alive** (`FOXAPI_REMOTE_KEEPALIVE_MS`, 2 s: the app's remote PTT
  hold) — which is what opens a transmit key held by a client whose network
  went away. Of the transmitter a remote session may only press and release
  the PTT, and only while a local session has armed remote transmit
  (docs/API.md 6.10).
- **A reconnect is a new session, but the same operator.** Because a
  stalled window is closed with its session and comes back with a new one
  (reading the token file again), the latch MARK (docs/API.md 6.10) is kept
  per PRINCIPAL, not per session: every LOCAL session - in-process or through
  the token file - is the principal `local`, a remote one is its login token.
  A window that re-sends `LATCH 1` every frame therefore cannot escape a
  mark by being closed and reconnecting (round-4 review: 5 latches in 12 s,
  one per reconnect); only a `LATCH 0` from one of the operator's sessions
  clears it.
- **A detached session's socket is closed by the server.** When a login is
  revoked or expires, the engine detaches every session on it and closes
  them itself on its next control pass (of what they left queued only the
  commands that make the transmitter safer are applied); the server then
  sends `bye` and closes each such socket, freeing its connection.

### 2.2 Messages

Text frames carry JSON (control, state, lists, results, events); binary
frames carry streams. Every JSON message has `"t"` (type) and, for
requests, `"id"` (echoed in the reply).

| Direction | `t` | Body | Maps to |
|---|---|---|---|
| C→S | `hello` | `{"api":[0,2],"client":{"name","version"},"grants":["view","tune",…]}` | version check + `open_session` |
| S→C | `welcome` | `{"api":[0,2],"engine":{"name","version"},"caps":[…],"grants":[…],"limits":{…}}` | the table's header fields and the limits below |
| C→S | `beat` | `{}` from the interface's own frame loop, at least every 500 ms | `heartbeat` |
| S→C | `state` | the `FoxReceiverState` fields by name, as JSON | `read_state` |
| C→S | `submit` | `{"batch":[{"id":17,"op":"SET_FREQUENCY","num":[94620000]},…]}` - `id` is the CLIENT's request id: per connection, from 1, increasing | `submit` |
| S→C | `accepted` | `{"answers":[{"id":17,"status":"OK","ticket":42},{"id":18,"status":"NO_CHANGE","ticket":40},{"id":19,"status":"BUSY"}]}` - the ENGINE's `FoxSubmitResult` for each request of one `submit`, in request order | `submit`'s per-command answers |
| S→C | `result` | `FoxCommandResult` fields with the ENGINE's ticket, one per engine ticket, in ticket order (a refusal has `"refused":true`) | `poll_results` |
| C→S | `subscribe` | `{"events":["tx_unkeyed","fault",…],"spectrum":{"bins":1024,"fps":25},"audio":{"rate":12000},"iq":null}` | `subscribe` + stream requests |
| S→C | `event` | `FoxEvent` fields | `poll_events` |
| C→S / S→C | `list` | `{"list":"DEVICES"}` → `{"list":"DEVICES","items":[…]}` | `read_list` |
| C→S / S→C | `setting` | `{"key":…}` → `{"key":…,"value":…}` | `get_setting` |
| S→C | `bye` | `{"reason":…}` before the server closes | — |

Op, list and event names are the header's names without the prefix
(`FOXAPI_OP_SET_FREQUENCY` → `"SET_FREQUENCY"`), so the JSON vocabulary is
the C vocabulary. **Unknown keys in a request are an error**, not ignored —
the same rule the app's `/api/control` already enforces, for the same
reason (a client sending a key the server silently skipped would believe it
had done something).

**Nothing the client library does waits on the network.** `read_state`
returns the last `state` received (a local snapshot); `read_spectrum` the
last spectrum frame; `poll_*` drain local queues. `submit` sends one `submit`
message and returns at once, so a click never waits a round trip and a PTT
re-asserted every frame costs one small message per frame.

**Two ticket spaces, and only the engine decides the engine's.** The
interface sees CLIENT tickets; the engine's session has ENGINE tickets; the
library joins them from what the server tells it and never predicts what
the engine will do (round-5 review: 0.2.2 had the client number the engine's
tickets and answer merges itself, which disagreed with the engine whenever
the engine queued a command afresh, refused a malformed one, or had already
answered the ticket the client merged into).
- **The client's own answer.** `submit` gives each command the connection's
  next request id and answers the interface by in-process rules applied to
  the CLIENT's own books, which bound what it holds for an interface that
  stops reading: `FOXAPI_OK` and a new client ticket when the message is
  queued for the socket; `FOXAPI_BUSY` when the interface already has 256
  ordinary client tickets unread; and for a safer command (`TX_PTT 0`,
  `TX_LATCH 0`, `RUN 0`, `TX_CLOSE`, `TX_REMOTE_ARM 0`) whose kind has a
  client ticket the interface has not yet READ, `FOXAPI_NO_CHANGE` and that
  client ticket - never one whose result `poll_results` has already returned.
  Safer commands are never `BUSY`. Every command is SENT, with its own
  request id, whatever the client answered: whether the engine merges it is
  the engine's decision.
- **The engine's answer.** The server hands each `submit` to the engine and
  sends back `accepted` - the engine's `FoxSubmitResult` for every request id
  (docs/API.md 7.2): `OK` and a new engine ticket, `NO_CHANGE` and the engine
  ticket it was merged into, or `BUSY`. The server writes `accepted` before it
  next polls results, on the same ordered socket, so a `result` never
  arrives for an engine ticket the client has not yet seen in an `accepted`.
  The library maps each engine ticket to the request ids answered with it,
  from these answers only.
- **Already forwarded.** The engine never merges into a ticket whose result
  `poll_results` has already returned - and the server's forwarding of a
  `result` IS its `poll_results` - so `accepted` never names an engine ticket
  whose `result` has already been sent: such a request gets a NEW engine
  ticket and its own `result`. A client that is nonetheless told
  `NO_CHANGE` with an engine ticket it has already had a `result` for (a
  broken server) must treat the request as answered by nothing, report
  `FOXAPI_FAILED` for its client ticket and close the connection: it cannot
  know whether the command was applied.
- **Results.** A `result` for engine ticket T answers every request id
  mapped to T. A client ticket has exactly one result, and when its requests
  map to more than one engine ticket (the client merged sends that the
  engine kept apart, or the other way round) that result is the ENGINE
  RESULT OF ITS LATEST REQUEST - the one sent last (for one kind the
  engine's outstanding ticket only moves forward, so that engine ticket is
  also the highest of them). Results of the
  client ticket's earlier engine tickets are consumed for the books and not
  delivered. The library delivers client results in client-ticket order,
  holding a client ticket until its latest request's engine result has
  arrived; if a request is merged into a client ticket whose result is
  already waiting unread, the later engine result replaces it in place (the
  in-process rule, docs/API.md 7.2), and once the interface has read a client
  ticket's result it is never answered again - a later send of that kind
  takes a new client ticket. An engine `BUSY` for a request the client
  answered `OK` becomes that client ticket's result: `BUSY`, refused,
  nothing changed.

**Liveness is the interface's, never the transport's.** Only a `beat`
message calls `heartbeat`; the WebSocket's own ping/pong (which a TCP stack
or a proxy answers while the interface's frame loop is wedged) keeps the
socket open but is NOT interface liveness and never extends a key. A
`submit`, including a PTT re-assertion, is not a beat either.

### 2.3 Binary frames

Every binary frame starts with a 16-byte header, little-endian:

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | kind: 1 spectrum, 2 audio, 3 I/Q, 4 image |
| 1 | 1 | header version (1) |
| 2 | 2 | flags |
| 4 | 4 | payload bytes |
| 8 | 8 | sequence (spectrum seq, or stream cursor of the first frame) |

- **Spectrum:** `f64 centreHz, f64 spanHz, i64 unixMs, u16 bins, u16 reserved,
  f32 dbFloor, f32 dbStep` (36 bytes, so 52 with the frame header), then
  `bins` × `u8`: each bin quantised as `floor + value × step` (the app's
  `/api/spectrum` already sends 8-bit bins, base64 in JSON; here raw). Bins
  are reduced by the maximum of each group, as in-process.
- **Audio:** `u32 rateHz, u16 channels, u16 codec` then samples. Codec 1 =
  16-bit PCM. Default 12 kHz mono; the client may ask for 24 or 48 kHz on a
  link that can carry it. (Opus is the obvious next codec; not in 0.2.)
- **I/Q:** only the tuned CHANNEL (after the VFO), never the device band:
  `f64 rateHz, f64 centreHz` then interleaved `i16` I/Q, at most 250 kS/s.
  Off unless the session holds `GRANT_IQ` and asks.
- **Image:** `u64 imageId, u32 width, u32 height, u32 format` then pixels,
  on request after a `DECODER_IMAGE` event.

### 2.4 What goes over the wire, and how big

| Stream | Rate | Size | Bandwidth |
|---|---|---|---|
| `state` JSON | when `seq` moves, plus measurements at 10 Hz | ~1.5 KB | 15 KB/s typical, ≤ 45 KB/s (30 Hz cap) |
| spectrum | 25 fps × 1024 bins | 1 024 + 52 B | ~27 KB/s |
| spectrum, small | 10 fps × 512 bins | 564 B | ~6 KB/s |
| audio | 12 kHz PCM16 mono | — | 24 KB/s |
| audio, full | 48 kHz PCM16 stereo | — | 192 KB/s |
| I/Q channel | 250 kS/s i16 | — | 1 MB/s (opt-in) |
| commands, results, events | on change | < 0.5 KB each | negligible |

A typical remote interface (state, 25 fps spectrum, 12 kHz audio) needs about
**70 KB/s**. Full-rate device samples (2–61 MS/s, 16–490 MB/s) are **never**
sent: that would move the heavy work off the engine, which is the opposite
of the design.

### 2.5 Back-pressure, rate limits and sizes

- The server never queues unbounded data for a slow client: it keeps the
  newest spectrum frame only (older ones are replaced), coalesces `state`,
  and keeps a bounded audio ring per client whose overruns are reported in
  the audio frame's flags.
- **Sizes** (announced in `welcome.limits`; a message over a limit closes
  the socket with 1009, it is never truncated):

  | Limit | Value |
  |---|---|
  | one text message (JSON) | 64 KiB |
  | one binary message (client → server; none are sent today) | 0 |
  | commands in one `submit` | `FOXAPI_MAX_BATCH` = 1024 (more: the whole message is refused, `LIMIT`) |
  | commands outstanding per session (queued, applying, or answered but not yet read) | `FOXAPI_MAX_PENDING` = 256 ordinary commands (then `BUSY`, as in-process; no result is ever dropped), plus ONE per kind of command that can only make the transmitter safer (`TX_PTT 0`, `TX_LATCH 0`, `RUN 0`, `TX_CLOSE`, `TX_REMOTE_ARM 0`; `FOXAPI_SAFETY_RESERVE` = 5): a newer one of a kind is merged with the outstanding one, never `BUSY` (a safer command refused at submit - malformed, no grant - is an ordinary command here: its own ticket, counted in the 256) |
  | events queued per session | 256 (then `OVERFLOW`) |
  | sessions per engine (all transports) | 64 open in all, of which the last 4 only for LOCAL sessions (so remote logins can never lock the local window out) and at most 16 ATTACHED REMOTE; a session detached by logout or expiry is closed by the engine on its next control pass and holds no place (the server closes its socket); open_session answers `LIMIT` past any of them (the mock enforces all three) |
  | a session's life | no longer than its login token (12 h at most): an expired or revoked token detaches it, and nothing it had queued is applied |
  | WebSocket connections per client address | 4 |
  | audio listeners per engine | 4 (the app already caps listeners) |
  | pending, unauthenticated upgrades | 8, each closed after 10 s |

- **Rate limits, per session, by class** (token buckets; a command over its
  class's rate is answered as a result with `LIMIT`, `FOXAPI_RESULT_REFUSED`,
  and never applied; other classes are unaffected, so a flood of one kind
  cannot starve the PTT):

  | Class | Ops | Rate (burst) |
  |---|---|---|
  | key | `TX_PTT` | 50/s (50): a re-assertion every frame at 60 Hz fits with room |
  | tune | `SET_CENTRE`, `SET_FREQUENCY`, `SET_VFO_OFFSET`, `STEP_TUNE`, `BOOKMARK_TUNE`, `SCANNER_*` | 100/s (100) |
  | settings | the receiver, display, source, gain, audio and decoder ops | 50/s (100) |
  | lists and edits | `BOOKMARK_ADD/REMOVE/FAVOURITE/REMOVE_GROUP`, `SCAN_DEVICES`, `PLUGIN_RESCAN` | 5/s (20) |
  | heavy | `BOOKMARK_IMPORT`, `PATCH_LOAD`, `STORE_*`, `RECORD_*` | 1/s (5) |
  | reports | `PROBLEM_REPORT`, `FEATURE_REQUEST` | 1/min (3) |
  | reads | `list`, `setting` messages | 200/s (400) |
  | beats | `beat` | 20/s (40); more is ignored, not an error |

- **Per server:** login throttling per client address (the app's
  `LoginThrottle`, with `Retry-After`), and at most 5 failed logins per
  address per minute before every login from it is refused for a minute,
  right password or wrong.
- A client that stops reading for longer than the remote keep-alive is
  closed, and its session with it. (That is why the latch mark belongs to the
  principal, not the session: the client's reconnect is a new session.)

### 2.6 Relation to the existing web remote, and CAT

The app's `/api/status`, `/api/control`, `/api/spectrum`, `/api/audio`,
`/api/login`, `/api/session`, `/api/logout` map one to one onto `state`,
`submit`, spectrum frames, audio frames and the login above. During
extraction they stay as a compatibility layer implemented on a REMOTE
session (docs/ENGINE-EXTRACTION.md, stage 5), so the browser page keeps
working.

**CAT (rigctld) is not a remote session.** Earlier drafts called each CAT
connection "a REMOTE (or loopback) session", which contradicted the rule
that a REMOTE session needs a login token: the rigctld protocol has no
login, and CAT clients (logging programs, WSJT-X) cannot present one. The
resolution: the CAT listener is **part of the engine**, like the web
server, and it is authorised by the LOCAL operator's configuration rather
than by a token - enabling it (`SERVER_CONFIG`, LOCAL + ADMIN only) is the
consent, loopback-only unless "reachable from other machines" is
separately switched on, exactly as in the app. Each CAT connection gets an
engine-internal session with the flag `FOXAPI_SESSION_REMOTE` (so it can
never latch, hold ADMIN, name a path or be armed for transmit) and the
grants TUNE + SETTINGS only - never TRANSMIT (the app's CAT answers `T 1`
with "not available" today, and keeps doing so). It never goes through
`open_session`'s token check because it is not opened through the table by
an outside client; it counts against the session limit like any other, and
the listener closes it with its TCP connection.

## 3. What remains

Nothing in section 2 is implemented. The in-process side of it is: the
0.2 submit contract (refusals as results, per-session tickets), `login` and
`logout` with detaching, the remote keep-alive, remote PTT-only with local
consent, the batch and session limits - all in the mock engine and tested.
In order: the session server on the engine side (on top of the C table,
with the `Host`/`Origin` checks, the loopback token file and the rate
classes above), the client library that implements `FoxEngineApi`, TLS and
tokens, the binary stream encoders, then running
`foxsdr-host --engine foxsdr_remote_client` against an engine on another
machine, with the same render tests the in-process path passes.
