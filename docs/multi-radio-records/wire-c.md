# wire-c: system, accessibility, web, ham operations, plugin host (wiring lane C)

Branch `feat/wire-c` (from 090e7039). Worktree `D:\foxsdr-wt\en-sysmodels`, build `D:/foxsdr-build/en-sysmodels` (Slint OFF). Operation block `0x8A00-0x8AFF`.

## 0. SURVEY (Phase 0, written 2026-11-12 before any code; UNCOMMITTED until the merged tip arrives)

### 0.1 Facts checked at 090e7039 (by grep, not memory)

* **Op block free.** `grep -rnE "0x8[89A][0-9A-Fa-f]{2}"` over `src/` finds no `FOXAPP_OP_*` above `0x8709..0x870D` and `0x8619`; nothing in `0x8800-0x8AFF`. The only range tests in the dispatch are `(op & 0xFF00u) == 0x0A00u` (transmit grant, `fox_engine_api.cpp:1633`, `:2083`): `0x8A00 & 0xFF00` is `0x8A00`, so it does NOT match. An op is accepted by three places: the table row `kOps` in `src/core/app_commands.cpp` (name + "tested" flag; the name test and `covering(op)` in `tests/test_apply_command.cpp` hold every row), a `case` in `Engine::applyCommand` (`engine.cpp`, or a per-feature file such as `engine_macros.cpp`), and `docs/api-0.3-additions.md`. Per-feature engine files with their own `case` list are the existing pattern (`engine_macros.cpp`, `engine_recname.cpp`, `engine_markers.cpp`, `engine_schedule.cpp`): I follow it with `src/engine/wire_c_*.cpp`.
* **The plugin ABI header is NOT `include/plugin_abi.h` in this tree**: it is `src/core/plugin_abi.h` (ABI version 3, host API level 2; `CascadeHostApi` ends at `events_seq`, line ~1992; last capability bit `CASCADE_CAP_TRACK_PHOTO` 0x10000). `third_party/foxsdr_api/foxsdr_api.h` is the engine API (a different header; not touched by ABI work). `docs/PLUGIN-API.md` exists. I will treat `src/core/plugin_abi.h` as "the plugin ABI header" and list the exact diff for the plugins merge lane (its copy is `include/plugin_abi.h` in the plugins repository).
* **Nothing of my group is referenced by any `src/` file but its own model** (include-grep over every model header): CONFIRMED for all sys_*, access_*, web_viewport/clip/profiles/arbiter/cors, upnp_igd, store_sort, cabrillo, adif_*, band_map, contest_*, omnirig, radioinfo, antenna_genius, xplane, hid_*, transcript_store, cli_options, decode_db, alert_rules, metrics, webhook, mcp_*, cat_emulation, discord_presence. `web_bookmarks.hpp` is included only by `web_clip.cpp` and `web_profiles.cpp` (siblings). The Kenwood/Yaesu follower is `src/source/rig_cat.{hpp,cpp}`, referenced by nothing.
* **Boot sequence** (`src/engine/process_boot.cpp` `bootProcess`, one function for `cascade`, `foxsdr-engine` and the Slint window): step 1 .. step 14; the config path is decided in step 8 `wireConfig`; `armDiagnostics` (step 3) already reads `%LOCALAPPDATA%`. Portable mode must put its `_putenv_s` pair BEFORE step 3: a new step 2b, one call.
* **Updater**: `parseUpdateManifest(text, current, out, error)` in `src/core/updater.cpp:229` parses the body that `PluginRepo::fetchText` returned (`:406`); the signature check goes between the two (`:407-:415`).
* **Web server**: routes are registered in `src/net/web_server.cpp` (4,769 lines, web-secure merged) from line 3699 (`svr.Get("/")` ...), `/api/control` at 4166, `/api/admin` at 4381, `/api/spectrum` at 4447; the page's JS is embedded in the same file.
* **Not in this tree yet**: `engine-merge-2.md`, `engine-serial-a.md`, `oss-radios.md`, `oss-net.md`, `oss-libs.md` (the second merge is running); `docs/feature-records/merge-round-4.md` in `plug-merge` does not exist yet (merge-round-1..3 do).
* The finished lane's build folder had been configured Slint OFF; the Phase 0 warm-up build (`cmake --build ... --parallel 4`, log `D:/foxsdr-build/en-sysmodels/wc-build1.log`) runs against the new tip.

### 0.2 The survey table

Status words: **wire now** / **decision (whose)** / **blocked (why)** / **transmit: stays unwired** / **already built** / **not mine**. "Shared" lists existing files my entry must touch (each gets the smallest append, bracketed `// wire-c begin/end`); every new engine file is `src/engine/wire_c_*.cpp` or next to its model. "Tests" = the engine-entry test I will add (loopback/scratch stand-ins only), always with a failure path; "BI" = a break-it check.

#### Group 1: `sysmodels` (record `docs/feature-records/sysmodels.md`, hand-off steps 1-7 + screens table)

| # | Row (model files) | Hand-off found | Engine entry needed | Shared files | Tests | Status |
|---|---|---|---|---|---|---|
| S1 | Portable mode (`sys_portable.*`) | step 1: resolve before anything resolves a path; marker without usable folder = stop, never fall back | new `wire_c_portable.cpp` `applyPortable(exeDir, argv-free)`; `bootProcess` step 2b (before `armDiagnostics`); sets APPDATA/LOCALAPPDATA via `_putenv_s`; refusal text shown in the console + boot exit code | `process_boot.cpp` (+1 call) | `test_wire_c_portable`: boot seam with a scratch "exe dir" holding the marker: config path resolves inside it; marker + unusable folder refuses; no marker changes nothing. BI: remove the call | wire now |
| S2 | Device-arrival watcher (`sys_device_watch.*`) | step 2: engine owns `DeviceArrivalWatcher({1500,10000})`, `start(true)` at start, `stop()` at shutdown; on settle post the Rescan command | `Engine` member started in the engine's service start; `onSettled` posts `FOXAPI_OP_SCAN_DEVICES` through the command queue; if an open radio is among removals the existing close path runs first. Setting `sys.device_watch` (default ON: opens no device, no file, no port). Fact `fact.sys.devices.last` (count, no serials). ThreadSpec for the 3 exempt thread rows in `tests/thread_inventory_rows.inc` becomes a heartbeat or stays exempt with a reason. **Overlaps R10 of serial lane A: re-check after the merge, wire nothing twice** | `engine.cpp/.hpp` (member + 2 calls), `thread_inventory_rows.inc` | `test_engine_api_wire_c_sys`: synthetic `WM_DEVICECHANGE` to the watcher window produces ONE rescan command with a stub engine; shutdown stops it. BI: coalescer bypass | wire now (after reading serial-a's record for R10) |
| S3 | Resume watcher (`sys_resume_watch.*`) | step 3: `ResumeWatcher({4000,30000}, action)`; action = close and re-open the current radio as a saved-radio open does + re-open the audio device | engine owns it; action posts the saved-radio reopen + audio reopen through the command queue only while a radio was open and running; setting `sys.resume_reopen` default ON | `engine.cpp/.hpp` | injected `PBT_APMRESUMEAUTOMATIC` through the gate: one reopen command, a stopped radio is NOT started; suspend cancels pending. BI: gate rate-limit | wire now (the reopen path is UNVERIFIED on a real sleep) |
| S4 | Keep-awake (`sys_keep_awake.*`) | step 4: one `KeepAwake`; lease held by a recording and by a scheduled capture | one object in the engine; lease stored in the recorder's take object and the schedule runner's active entry; log "could not keep the PC awake" once; fact `fact.sys.keepawake` ("1"/"0"). Display never held | `engine.cpp/.hpp`, `engine_schedule.cpp`, `recorder.cpp` or the start/stop sites in `engine.cpp` (smallest) | start/stop an audio take and a scheduled entry in a hermetic engine with the OS table injected: lease count 1 during, 0 after, on every exit path (stop, error, shutdown); a refusing OS table logs once. BI: no release on stop | wire now |
| S5 | Resource meter + effects policy (`sys_resource_meter.*`, `sys_effects_policy.hpp`) | step 5: 1 Hz, facts `fact.perf.*`, op `perf.mode` | engine 1 Hz sample on the service thread (cheap reads; the GPU counter on a worker, never the control pass); facts `fact.perf.cpu_percent/memory_mb/system_memory_percent/gpu_percent/reduced` ("" = n/a); op `FOXAPP_OP_PERF_MODE` (ival 0 auto/1 on/2 off); `startLowEnd` once; the 6 switches published as `fact.perf.switch.<name>`; engine does NOT itself change the picture (screen lane reads the facts) | `engine.cpp`, `engine_api_lists.cpp` (fact list), `app_commands.*` | stub sources: high samples switch to reduced after 5, calm after 30; op on/off/auto; fact "" when a source is unavailable (never 0). BI: hysteresis | wire now |
| S6 | Manifest signature (`sys_manifest_sig.*`, `tools/manifest_sign.cpp`) | step 6: verify envelope before parsing, only when keys exist | `updater.cpp` between `fetchText` and `parseUpdateManifest`: `if (!keys.empty()) verifyEnvelope`; refusal "the update information is not signed by FoxSDR"; keys from `ownerTrustedKeys()` (empty constant: today's behaviour) PLUS a test seam (`FOXSDR_TEST` style injected key list, not an env var) | `updater.cpp` (+ header seam) | with a test key list: good envelope updates; wrong key, tampered, unsigned body, empty key list (=today) each give the expected verdict through `checkForUpdate` on the canned-manifest path. BI: the verify call removed | wire now (the release flow, the key and enforcement = owner decisions 1+2, listed) |
| S7 | Export / import / backup (`sys_bundle.*`) | screens table: Export settings..., Import settings... (plan + confirm token), Back up now, automatic backup at close | ops `FOXAPP_OP_SYS_EXPORT` (text: path), `SYS_IMPORT_PLAN` (text: path) -> facts `fact.sys.import.plan` (JSON) + token, `SYS_IMPORT_COMMIT` (text: token), `SYS_BACKUP` (ival keep); engine does the file reads/writes under the per-user folder rules; setting `sys.backup_on_close` default off; listeners imported OFF (model rule) | `app_commands.*`, `engine.cpp` case list (one `wire_c_apply` hook), `config.hpp` (1 key) | export a REAL saved config, import into a scratch profile: plan, wrong/stale token refused, rollback on failure; no secret in the bundle; listener switches OFF after import. BI: confirm token check | wire now |
| S8 | Whole-app reset (`sys_reset.*`) | Reset everything..., Undo the reset, checkboxes plugins/logs | ops `SYS_RESET_PLAN`, `SYS_RESET_EXEC` (text token), `SYS_RESET_UNDO` (text stamp); facts `fact.sys.reset.plan/last`; moves into `reset-<stamp>` (never deletes); the running engine then needs a restart flag (`fact.sys.restart_needed`), engine itself does NOT exit | `app_commands.*`, `engine.cpp` hook | in a scratch folder named `foxsdr`: plan, token, stale token, execute, undo, folder-name guard. The real profile is never an argument (hermetic profile only). BI: token + folder guard | wire now |
| S9 | Settings profiles, per-radio map, folder sync planner (`sys_profiles.*`) | Profile dropdown, Save as profile, Delete, Use with radio, shared folder | ops `SYS_PROFILE_SAVE/DELETE/SWITCH`, `SYS_PROFILE_RADIO_SET`, `SYS_SYNC_PLAN` (plan only; copying = a worker with the planner's decisions, push/pull, never delete, off unless the folder is set); facts `fact.sys.profiles` (names), `fact.sys.profile.active`; switch merges with this machine's secrets/identity/consent/network switches (model rule) | `app_commands.*`, `engine.cpp` hook, `config.hpp` (key `sys.sync_folder`, empty = off) | save/switch/delete in a scratch config dir; machine keys survive a switch; a serial-looking radio key refused; sync conflict never overwritten. BI: machine keys carried | wire now |
| S10 | First-run wizard + starter presets (`sys_first_run.*`) | pages 1-4, finish applies: source kind, `FreqManager::addMany`, usage switch, `markFirstRunDone` | ops `SYS_FIRSTRUN_CHOOSE_RADIO/PRESET/TELEMETRY`, `SYS_FIRSTRUN_FINISH`, `SYS_FIRSTRUN_SKIP`; fact `fact.sys.firstrun` (step, choices, done); marker file `first-run-done`; the wizard's usage-reports default ON is owner decision 4: I keep the program's existing default and do NOT change `telemetryEnabled` unless the user chooses | `app_commands.*`, `engine.cpp` hook | drive the wizard through ops in a hermetic engine: skip from every step; preset offered only where it fits the radio window; finish applies; done marker; second start does not re-run. BI: preset fit rule | wire now |
| S11 | Telemetry on/off model (`sys_telemetry_switch.hpp`) | already built in the engine (`engine.cpp:8442`, `system_logic.cpp:922`) | none; add ONE engine test of "off sends nothing, off deletes the id" through the real op if not already covered (check `test_telemetry*`) | - | possibly none | already built |
| S12 | Code-signed setup (`installer/sign-setup.ps1`) | hand-off 7: needs a certificate | none: dormant script is NEVER run; `cascade.iss` `SignTool=` only with a certificate | - | - | blocked (the owner's certificate; owner decision 3) |

#### Group 2: `access` (record section 8)

| # | Row | Hand-off found | Engine entry | Shared | Tests | Status |
|---|---|---|---|---|---|---|
| A1 | Units (`access_units.hpp`) | 8.2: replace literals at `track_metrics.hpp:1252,1273,1276,1378-1379` and the ladder text in `map_view.cpp:89-103` by `quantify`/`text`; key `access.units` | setting `access.units` in `AppConfig` (default Automatic = today's text, pinned by the model's test) + fact `fact.access.units`; op `FOXAPP_OP_ACCESS_SET` (text key, text value) | `config.hpp/.cpp`, `src/gui/track_metrics.hpp`, `src/gui/map_view.cpp` (call sites; these are GUI files but no `.slint`) | an existing-output test: Automatic prints byte-identical text to today for every kind; Metric/Imperial/Aviation/Nautical change the printed unit; a bad key refused. BI: Automatic path altered | wire now |
| A2 | Number format (`access_number_format.*`) | 8.2: `host_logic.cpp:111-120,181-200`; keys `access.number_format`, `access.digits` | settings + the same `ACCESS_SET`; `host_logic.cpp` `fmtMHz/fmtKHz/spanText/cursorText/fmt` call `formatNumber`; input rule of `parseFrequencyHz` kept; default "Follow Windows regional settings" would change today's output on a comma locale, so the DEFAULT is "Always use a dot" until the owner chooses (listed) | `config.*`, `src/slint/host_logic.cpp` | dot default byte-identical; a comma locale with the model's injected symbols; the frequency counter round trip. BI: formatter bypass | wire now (default = today's behaviour; the "follow Windows" default is an owner decision) |
| A3 | Speech (`access_speech.hpp`, `access_speech_sapi.*`) | 8.2: host owns one `Announcer`; poll with `SpokenState`; real-audio `Speaker` deliberately NOT built | settings `access.speech.*` (all in 8.1); engine owns the `Announcer`, polled from the 10 Hz service tick with the receiver's frequency/mode/level/squelch; ops `ACCESS_SPEAK_NOW`; output: `SapiMemorySpeaker` renders to a MEMORY stream (never a dialog, silent failure with no voice, per the model) and the PCM is handed to the app's own audio path as a second source mixed into the output. **Design choice for the orchestrator**: which audio route (see 0.4 Q2) | `engine.cpp` service tick, the audio sink (`src/sink/*` mixer entry or a small new sink class) | with `RecordingSpeaker`: announcement policy through the engine (settle, min interval, level step, interrupt); no voice installed = no output and one log line; SAPI never reaches a device in the test (source-scan test exists). BI: policy | wire now (the audio route is the only open design point) |
| A4 | Sonification (`access_sonify.hpp`) | 8.2: `Sonifier::update` per meter tick; `ToneRenderer` into the audio block | engine owns a `Sonifier`; same second-source audio route as A3; settings `access.sonify.*` default off; fact `fact.access.sonify.tone_hz` | audio sink (as A3), `config.*` | tone follows level in a hermetic engine fed a generated signal (noise, offset, rate error); silent when squelch closed; off by default. BI: squelch mute | wire now (after A3, same route) |
| A5 | Colour-blind-safe palettes (`access_palette.hpp`) | 8.2: lane K maps `Role` to the Theme; key `access.palette` | engine side only: setting + fact; the `.slint` fill is screen lane K's | `config.*` | key round trip, unknown id refused | wire now (engine half only) |
| A6 | RTL investigation; screen-reader exposure design (section 6, 7) | no code | none | - | - | not mine (screen lane K; no engine entry) |

#### Group 3: `webmodels` (`docs/webmodels-handoff.md`), into `src/net/web_server.cpp`

| # | Row | Hand-off found | Engine entry | Shared | Tests | Status |
|---|---|---|---|---|---|---|
| W1 | Waterfall zoom/pan (`web_viewport.*`) | page JS port, six functions + page hooks | page JS appended to the embedded script (no server code) | `web_server.cpp` (page string) | a node script (Node 24 is on the PC) evaluates the embedded functions against the model's table of cases; source-scan that the page carries them; UNVERIFIED in a real browser | wire now |
| W2 | Per-viewer bookmarks (`web_bookmarks.*`) | routes `/api/mybookmarks` (GET/POST/PUT/DELETE/export/import), `forgetViewer` on sign-out/expiry, localStorage restore | one `WebBookmarkBook` member; 6 routes behind `authorised`; page controls | `web_server.cpp/.hpp` | loopback httplib client: sign in, add/list/search/update/delete/export/import, other viewer sees nothing, sign-out forgets; 401 without the cookie. BI: viewer isolation | wire now |
| W3 | Clip in the browser (`web_clip.*`) | `POST /api/clip`, `GET /api/clips`, `GET /api/clip/<id>`; needs ONE engine call to record N s of demodulated audio to a path | the broker is wired; the recorder call uses the engine's existing audio record start/stop with a timer and an engine-owned path under the recordings root (`WebClipBroker` plans the path); `replay_ring`/`clip_recorder` stay unwired (W2 said blocked on that, not needed for a plain timed take) | `web_server.cpp`, `engine` (one op `FOXAPP_OP_WEB_CLIP` internal), recorder | a real take on the generator signal through the loopback server: WAV downloaded, size matches; listener role refused unless the switch allows; abandon on sign-out. BI: path containment | wire now (recorder reuse to be confirmed in Phase 1; if the recorder cannot take a path, then `decision (orchestrator)`) |
| W4 | Wheel bandpass + step selector | page arithmetic only | page JS | `web_server.cpp` | node evaluation + source scan | wire now |
| W5 | Named profiles (`web_profiles.*`) | status fields `profiles`/`profile`, `GET /api/profiles`, `POST /api/profile`, canonical name, passes the arbiter | needs S9's profile list + switch op | `web_server.cpp` | switch through the web with a stand-in engine switch; unknown name refused; viewer text never forwarded | wire now (after S9) |
| W6 | Control arbiter (`web_arbiter.*`) | join/heartbeat/leave/setRights, `mayAct` before every tuning request, routes `/api/control/{viewers,mode,grant,revoke,request,release,cancel}`, `controlEpoch` in status | one `ControlArbiter`; hook into `WebRoom` end-of-session; refusal text from `arbResultText` | `web_server.cpp` | two viewers over loopback: exclusive mode, request/grant/release; the owner is never locked out; viewer ids never sent to others. BI: `mayAct` check | wire now |
| W7a | CORS allow-list (`web_cors.*`) | pre-routing handler; settings `webCorsOrigins` (empty = off), `webCorsAllowCredentials` (false) | handler + 2 keys; credentials need HTTPS + SameSite change: NOT done (owner decision) | `web_server.cpp`, `config.*` | loopback: allowed origin gets headers, preflight 204, denied gets none, two Origin headers denied; default config sends none. BI: policy off | wire now |
| W7b | UPnP port mapping (`upnp_igd.*`) | record: OWNER DECISION, NOT WIRED | none | - | - | decision (owner) -> LEFT UNWIRED as the brief says |
| W8 | Media Session + wake lock (page only) | page JS block | page JS | `web_server.cpp` | node/source scan; browser behaviour UNVERIFIED (HTTP LAN page is not a secure context) | wire now |
| W9 | Store sort (`store_sort.*`) | `StoreStats` sort; ratings need a server (not built) | `engine_store.cpp` list ordering + op `FOXAPP_OP_STORE_SORT` (ival mode); no ratings | `engine_store.cpp`, `app_commands.*` | order of the store list per mode with a fake catalogue; unknown mode refused | wire now |

#### Group 4: `hamops` (record sections 5 and 7), receive side only

| # | Row | Hand-off found | Engine entry | Shared | Tests | Status |
|---|---|---|---|---|---|---|
| H1 | QSO log + Cabrillo export (`qso_log.hpp`, `cabrillo.hpp`) | screens: contest fields, "Export Cabrillo log..." | **there is no QSO store in the engine**: an engine-owned `QsoBook` (`qso-log.json` in the per-user folder, bounded, callsigns only because the user types them; never in a report or setting) + ops `QSO_ADD/REMOVE/CLEAR`, `CABRILLO_SET_HEADER`, `CABRILLO_EXPORT` (text: path) | `app_commands.*`, `engine.cpp` hook | add QSOs, export, re-read the file and check it against the model's golden lines; problems list returned; skipped counted. BI: chronological order | wire now (the QSO book is mine to define; see Q3) |
| H2 | Contest macros (`contest_macros.hpp`) | text expansion for sending | the product does not transmit | - | - | transmit: stays unwired |
| H3 | ADIF QSO record + broadcast (`adif_qso.hpp`, `net/adif_broadcast.hpp`) | "Broadcast logged QSOs (ADIF)" off; 127.0.0.1; UDP port, TCP port, "Allow addresses other than this computer" off | one broadcaster owned by the engine, started only when enabled; `publish` on each `QSO_ADD`; facts clients/sent/refused; keys `hamops.adif.*` (UDP port 0, TCP port blank, loopback only) | `engine.cpp`, `server_host.cpp` (listener-pattern row if it fits) | loopback UDP/TCP receiver: one record per QSO; off by default sends nothing; non-loopback address refused unless switched. BI: loopback guard | wire now |
| H4 | Contest score (`contest_score.hpp`) | read-only panel | engine computes from the `QsoBook`, op `CONTEST_SET_RULES` (the two example rule sets), fact `fact.contest.score` JSON | `engine.cpp` hook | score after known QSOs equals the model's expectation incl. dupes/multipliers | wire now |
| H5 | Band map (`band_map.hpp`) | spots from `dx-spots` via `addSpot(call,Hz,mode,now)` | engine owns a `BandMap`; ops `BANDMAP_ADD` (user/internal), `BANDMAP_WORKED`, `BANDMAP_CLEAR`; facts `fact.bandmap.spots`. Plugin spot FEED has no host call today: use the existing host event ring (`read_events`, kind `text`) ONLY if the dx-spots lines are parseable there, else list an ABI append "post_spot" (I own the ABI) | `engine.cpp`, `plugin_abi.h` (maybe) | add/age/merge/needed through ops with a clock seam; feed test via a stand-in plugin text line | wire now (ops + facts); feed route decided in Phase 1 |
| H6 | RadioInfo UDP listener (`radioinfo_udp.hpp`, `net/radioinfo_listener.hpp`) | "Follow radio info from DXLab Commander or N1MM+" off; port, radio 1..4 | listener on a worker (ThreadSpec); keys `hamops.radioinfo.*`; facts per radio (stale after 30 s); never retunes the receiver (display) | `engine.cpp`, thread rows | loopback UDP datagram of a real RadioInfo XML sample; stale rule; malformed + fuzz refused; off by default binds nothing. BI: bind guard | wire now |
| H7 | X-Plane UDP listener (`xplane_udp.hpp`, `net/xplane_listener.hpp`) | slots need a real dataref list (record section 4) | listener on a worker; keys `hamops.xplane.*` (off); `FlightTrack` into the engine's track list ONLY if a track source seam exists (W2 listed the track store as unwired too) | `engine.cpp` | loopback DATA/RREF packets; bound 64 aircraft; ageing. Real X-Plane UNVERIFIED | wire now (facts; map feed depends on the track list) |
| H8 | Antenna Genius read-out (`antenna_genius.hpp`, `net/antenna_genius_tcp.hpp`) | read-outs per port; "Select antenna" is a user action | the read-only status (`show`-class queries) on a worker, off, address typed by the user; **"Select antenna" and every command that sets state stays UNWIRED** (the brief says read-out; it can switch the antenna a transmitter uses) | `engine.cpp` | loopback fake device: read-out facts; any set command is refused by the engine entry. BI: command allow-list | wire now (read-out only); select antenna: transmit-adjacent, stays unwired |
| H9 | HID descriptor/map (`input_hid/*`) | needs the knob lane's `hid_win` and its `InputEvent` join | `feat/knob-input` is NOT merged: there is no HID backend to feed `parseReport` | - | - | blocked (no HID backend in the tree; knob-input not merged) |
| H10 | Transcript store (`transcript_store.hpp`) | nothing to feed it without a recogniser | none | - | - | blocked (speech-to-text is an owner decision about a download; the model waits) |
| H11 | OmniRig client, READ freq + mode (`omnirig_client.hpp`) | late-bound COM bridge `OmniRig.OmniRigX`, writes freq/mode in the model | the bridge as `OmniRigRigPort` (read-only: Status/Freq/Mode/Tx; the two writers NOT implemented: this product follows the rig, never sets it); off by default; created only on the user's switch, on a worker thread; with a stand-in port in every test; the REAL COM class is never created in a lane test (it can start the OmniRig program) | `engine.cpp`, `wire_c_omnirig_win.cpp` | stand-in port: follow frequency and mode into the receiver (`SET_FREQUENCY` path), disconnect, rig busy; compile-time test that no setter exists. Real OmniRig UNVERIFIED | wire now (stand-in only; real bridge UNVERIFIED) |
| H12 | FLRig, HRD, Commander TCP, UltraBeam | record section 5 | none | - | - | blocked (no free interface description; makers to be asked: owner) |
| H13 | Power/Tuner/Station Genius, SSTV/VARA transmit, Club Log/QRZ/eQSL uploads, MSFS, speech-to-text | record section 5 | none | - | - | transmit: stays unwired (PG/TG/SG, SSTV, VARA) / decision (owner: uploads need accounts) |

#### Group 5: `packaging`

| # | Row | Hand-off | Entry | Shared | Tests | Status |
|---|---|---|---|---|---|---|
| K1 | Release-script step | after the `github` step run `tools\make-package-manifests.ps1 -SetupFile <dist\foxsdr-setup-<v>.exe> -OutDir <release>\packages -RunWingetValidate`, compare the SHA-256 with `dist\sha256.txt`; publishing needs accounts | new step `package-manifests` in `tools/release.ps1`, never run by me; the script's own refusal when the setup file is absent | `tools/release.ps1` (a new step appended at the end of the step list) | `tools/release.ps1` has a dry-run / step-list check; I add a test that the step is in the list and its arguments are the hand-off's (source scan) and run the generator on a FAKE setup file in scratch (the lane's own test); `packaging_manifests` is not registered here (`pwsh` is the WindowsApps stub): run it by calling the script directly with Windows PowerShell | wire now (the release run itself is not mine) |

#### Group 6: plugin-host additions (I am the only lane that may change the ABI; path `src/core/plugin_abi.h`)

| # | Row | Source | Entry | Shared | Tests | Status |
|---|---|---|---|---|---|---|
| P1 | Recorder start/stop a plugin can call (per-talkgroup recording) | plugins-oss host hand-off 10 (signalling): `recorder_start(ctx, label)` / `recorder_stop(ctx)` under a RECORD grant | two appended `CascadeHostApi` members (after `events_seq`), `CASCADE_HOST_COVERS` gate, host host-API level stays 2 or goes to 3 (decide at the diff), grant = the existing RECORD-class grant if there is one else new capability bit 0x20000 asks the user; label sanitised (no path chars); the host's audio recorder with its own file name (naming template) | `plugin_abi.h`, `plugin_host.cpp`, `PLUGIN-API.md`, `plugin_api_core` | plugin-loading tests re-run; a stand-in plugin calls start/stop: file created with the label, refused without the grant, refused when the recorder is busy; **all 43 published plugins still load** (`test_plugin_abi3_compat`) | wire now |
| P2 | ABI REQUESTS of wire-a and wire-b | to be forwarded by the orchestrator when they report (symbol rate to a plugin; credit field on plugin marks) | `C6` symbol rate: appended `CascadeTrackInfo`-style or `set_symbol_rate` host->plugin call, level 3; marks credit: appended field | `plugin_abi.h` | as P1 | waiting (orchestrator) |
| P3 | Additive `airline` / `route` strings in `CascadeTrackInfo` (tables hand-off 5a); APT temperature readout call (5b) | plugins-oss hand-offs 5, 6 | appended struct members + `CASCADE_HOST_COVERS`; no consumer in the host except the engine target-details panel (screen) | `plugin_abi.h` | layout test: old plugins' struct size unchanged prefix; new fields default empty | wire now (small, after P1) |
| P4 | Depth-lane host rows left by W2 (B1 alert sound, B2 post-squelch capture, B4/B5 track fields, B6 airline lookup, B7 waterfall click bit 0x20000, B8 RTTY_SCOPE, B12 `start_capture`, B13 marker level, B14 choice-list setting) | `engine-wiring-2.md` 2.9 / section 6 item 8; digest `depth-digest.md` | "take the small append-only ones first; design nothing large": B13 marker level and B14 choice setting kind are struct/enum appends; B7 a capability bit; the rest touch audio paths | `plugin_abi.h` | as P1 | partly wire now (B4/B5, B13, B14 first); B1/B2/B6/B7/B8/B12 not done unless time |
| P5 | Large host asks: 802.11 radiotap capability / wideband channel follower (remoteid), host TLS service for plugins (misc), ProVoice/NXDN front ends | plugins-oss hand-offs 1, 9, 11 | designs, not appends | - | - | not done (design nothing large) |

#### Group 7: helper programs (store side)

| # | Row | Hand-off found | Entry | Shared | Tests | Status |
|---|---|---|---|---|---|---|
| G1 | Plugin store installs a helper's companion files | `docs/gpl-helper-store-handoff.md` (read from `oss/helperhost`): catalogue `helper` object + per-platform `companions[]` (role helper/manifest/licence, `installAs` bare name, sha256, sizeBytes, `executable`); install = ONE transaction (download all to temp, verify size + sha256 of each, validate names and the PE/ELF header of the helper, then move all; any failure leaves nothing); update replaces all or none; uninstall removes companions with the DLL; execute bit on the helper only (POSIX) | `PluginRepo` parse + install/uninstall extension in new `src/core/plugin_repo_helper.{hpp,cpp}` called from `plugin_repo.hpp` install/remove; store row lines "GPL helper - source: %s" and the three detail lines; consent: an install op argument (like STORE_SITE's consent tick) that the notice was shown (owner decision 1: separate tick, default REQUIRE it) | `plugin_repo.hpp`, `plugin_repo_data.hpp`, `engine_store.cpp`, `plugin_store_row` model (strings only) | local stand-in catalogue + file server on 127.0.0.1: good install places all three files beside the DLL; hash mismatch on companion 2 leaves nothing; a hostile `installAs` (`..\x`) refused; non-PE helper refused; uninstall removes all; the module scan ignores `.exe/.json/.txt` (verify, handoff says UNVERIFIED). No real helper, no real catalogue row, no download. BI: sha check | wire now |

#### Group 8: W2 leftovers (checked against `engine-wiring-2.md` sections 2.1-2.8, 3: nothing below was done there)

| # | Row | W2 state | Entry | Shared | Tests | Status |
|---|---|---|---|---|---|---|
| L1 | CLI options (`cli_options.hpp`) | wire now (M); `parseFlags` refuses unknown arguments; the model's pass-through list is incomplete (`--seconds --safe-plugins --selftest --version --web-* --cat-* --diag-* --update-check --record-check --rds-check`...) | a `cli` branch in `parseFlags`, start-up ops run once after the engine is up (via the command queue); the full existing flag list re-checked from `process_boot.cpp` | `process_boot.cpp` | every existing flag still accepted (table test from the source list); new options map to ops; unknown refused as today; `foxsdr-engine.exe` run with the new options on a hermetic profile | wire now |
| L2 | Decode database + alert rules (`decode_db.hpp`, `alert_rules.hpp`) | decision (privacy): text on disk | ring of host events (`PluginApiCore::hostEvents`, private events skipped) into an IN-MEMORY decode DB bounded; alert rules evaluated per event -> fact/toast; **nothing on disk** by default (the privacy rule); disk persistence = owner decision | `engine.cpp` pump | feed events: query, rule hit, private event never stored, bound; no file created | wire now in memory; persistence: decision (owner) |
| L3 | Metrics page (`metrics.hpp`) | wire now in `web_server.cpp` | `GET /metrics` with the web auth rule (never public), per-plugin decode tally; off unless the web server has a user | `web_server.cpp` | loopback: 401 unauthenticated; text format parse; counters move | wire now |
| L4 | Webhooks (`webhook.hpp`, `webhook_http.hpp`) | decision (cleartext user URL) | `ServerHost` listener-pattern key `webhook`; events from the host event ring; HTTPS refused (kept), only a user-typed http URL, loopback test only, off; no private data (never a decode text of a private plugin) | `server_host.cpp`, `config.*` | stand-in receiver on 127.0.0.1: event arrives, https URL refused, off by default connects nothing, retry bound. BI: https guard | wire now (off) |
| L5 | MCP server, never transmits (`net/mcp_server.hpp`, `mcp_http.hpp`) | wire now (L) | new `InternalKind::Mcp` session with the ops `{SET_CENTRE, SET_MODE}` + VIEW grant only; HTTP listener 127.0.0.1; off by default | `fox_engine_api.cpp`, `server_host.cpp`, thread rows | loopback JSON-RPC client: list tools, tune, mode; every other op (record, transmit, grant) refused; off by default opens no port. BI: op allow-list | wire now (largest engine item) |
| L6 | Kenwood/Yaesu CAT follower (`source/rig_cat.*`) | wire now (L); overlaps `net/cat_emulation` | worker over the serial port the user names (the serial transport of the serial lane A if merged, else a stand-in only): READS the rig frequency/mode and tunes the receiver to it; never writes to the rig; off | `engine.cpp`, thread rows | stand-in transport scripts a rig answering FA/IF: receiver follows; garbage ignored; no write ever (compile test). Real rig UNVERIFIED | wire now (stand-in) |
| L7 | CAT emulation, receive only (`net/cat_emulation.*`) | wire now (L): listeners beside `reconcileCat` in `server_host.cpp`; TX commands answer `E;`/`?;`; port must differ from 4532 | `ServerHost` rows `catemu-*`, off by default, 127.0.0.1 | `server_host.cpp`, `config.*` | loopback client: FA read/write tunes the receiver, MD, IF; `TX1;` answered `E;` and nothing keys; port 4532 refused. BI: TX refusal | wire now |
| L8 | Discord presence (`net/discord_presence.*`) | wire now (M): settings `discord_client_id`, `discord_show_frequency`; off until an ID is given | one object owned by the engine over the local named pipe `\\.\pipe\discord-ipc-0` (local IPC, no network, no dialog); frequency only if the second switch is on | `engine.cpp`, `config.*` | a stand-in named pipe server in the test (own pipe name, not Discord's): handshake + activity JSON; empty ID connects nothing. BI: empty ID guard | wire now |

### 0.3 Counts

56 rows in all (S 12, A 6, W 10, H 13, K 1, P 5, G 1, L 8). Some rows hold several items (H13, P4, P5).

| Status | Rows |
|---|---|
| wire now | 45 (S1-S10, A1-A5, W1-W6, W7a, W8, W9, H1, H3-H8, H11, K1, P1, P3, P4 partly, G1, L1-L8; W3 conditional on the recorder taking a path; L2 in memory only; H8 read-out only) |
| decision (owner), left unwired | 1 (W7b UPnP); plus decisions recorded inside rows (see 0.4) |
| waiting on the orchestrator | 1 (P2: ABI requests of wire-a and wire-b) |
| blocked | 5 (S12 certificate, H9 no HID backend, H10 no recogniser, H12 no free interface description, P5 designs too large) |
| transmit: stays unwired | 2 (H2 contest macros; H13 Power/Tuner/Station Genius, SSTV, VARA), plus H8's "Select antenna" |
| already built | 1 (S11) |
| not mine | 1 (A6: screen lane K) |

### 0.4 Questions and decisions I need (the orchestrator unless noted)

* **Q1 (path)**: the brief says `include/plugin_abi.h`; in this repository it is `src/core/plugin_abi.h` (the `include/` copy is the plugins repository's). I will edit `src/core/plugin_abi.h` and list the exact diff. Confirm.
* **Q2 (speech and tone audio route)**: the access hand-off deliberately left the real speaker out. My plan: SAPI renders into MEMORY (the model's `SapiMemorySpeaker`), the engine mixes that PCM as a second source into the app's existing audio output (no second device opened, no dialog). Confirm, or name another route.
* **Q3 (QSO store)**: the engine has no QSO log; Cabrillo/ADIF/score need one. I will add an engine-owned `QsoBook` (file `qso-log.json`, callsigns the user typed, bounded, never in a setting/report). Confirm that a user-typed QSO store is wanted (it is receive-side logging).
* **Q4 (serial lane A)**: S2 (device arrival) and L6 (CAT follower's serial transport) overlap serial lane A's R10 and its serial source; I will read `engine-serial-a.md` once the new tip exists and wire nothing twice.
* **Owner decisions listed in the rows** (nothing waits on them): manifest signing key + enforcement; certificate; first-run telemetry default; "follow Windows regional settings" as the number-format default (I keep "always a dot"); helper-install consent tick (I default to requiring it); decode database persistence; UPnP; CORS credentials (needs HTTPS cookie change); webhook cleartext URLs.
* **Passwords at rest** (the brief): NOT decided by the owner; I build no credential storage. Present behaviour (from `engine-wiring-2.md` section 6 item 11 and 9.2): a PASSWORD-kind plugin setting and the web password hash travel the same `config.json`; PASSWORD settings are written verbatim with dated backup copies; protection at rest UNVERIFIED. The export bundle (S7) already strips secrets. Webhook/MCP/Discord/CAT emulation add NO credential field.

### 0.5 Order of work in Phase 1 (smallest first)
S11 check, K1, A5, W7a, W1/W4/W8 (page JS), S1, S6, S4, S5, S2/S3, P1, then A1/A2, S7-S10, W2/W6/W5, G1, H3/H4/H1, H5-H8/H11, L1, L3, L4, L7, L8, L2, L5, L6, A3/A4, P3/P4, P2 when forwarded.

(Sections 1 onward are written in Phase 1: what was wired, break-it table, SCREEN HAND-OFF, ABI REQUESTS, operations and keys, not done, owner decisions, config hashes.)

---

## 1. FINAL STATE (Phase 1, branch `feat/wire-c`, base e7e25fc9; 16 commits)

Row statuses after the work (the survey in section 0.2 stays as written; this table is the outcome). "WIRED" = engine entry + a test through that entry, with the break-it checks of section 3.

| Row | Final | Commit / note |
|---|---|---|
| S1 portable mode | WIRED | e358b1cc, bootProcess STEP 2b |
| S2 device arrival | ALREADY WIRED by serial lane A (engine_hotplug.cpp, ef015b1e): not done twice | - |
| S3 resume | WIRED, narrower than the hand-off: looks for the radios again and tries a waiting saved radio; does NOT close and reopen a healthy open radio (not settled) | 50b70895 |
| S4 keep awake | WIRED | 50b70895 |
| S5 resource meter + effects policy | WIRED; figures read at the call, in coarse steps (a fingerprint fix) | 50b70895, 41dad270 |
| S6 manifest signature | WIRED (no owner key = today's behaviour) | 439a733d |
| S7 export / import / backup | WIRED; import staged for the next start | dd1aa3de |
| S8 reset | WIRED; staged for the next start; undo of an applied reset NOT wired | dd1aa3de |
| S9 settings profiles, per-radio map, folder sync | NOT DONE (needs the same staged mechanism; list item 1) | - |
| S10 first-run wizard | WIRED; opening the chosen radio is not (the choice is a fact) | dd1aa3de |
| S11 telemetry switch | already built | - |
| S12 code-signed setup | blocked (certificate) | - |
| A1 units, A2 number format | NOT DONE (call sites in track_metrics.hpp, map_view.cpp, host_logic.cpp; the keys `access.units`, `access.number_format`, `access.digits`, `access.palette` exist in the registry and persist; nothing reads them yet) | list item 2 |
| A3 speech, A4 sonification | NOT DONE (the audio route, question Q2, is unanswered) | list item 3 |
| A5 palette | engine half only: the key persists and is a fact; the `.slint` fill is screen lane K's | 17e87b39 |
| A6 | not mine | - |
| W1, W4, W8 page JavaScript | NOT DONE (edits of a page string that no browser was available to check) | list item 4 |
| W2 my bookmarks | WIRED | c65b0e0a |
| W3 clip in the browser | NOT DONE | list item 5 |
| W5 web profiles | NOT DONE (needs S9) | - |
| W6 control arbiter | NOT DONE | list item 6 |
| W7a CORS | WIRED | c0ec098c |
| W7b UPnP | left unwired as the record says | - |
| W9 store sort | WIRED (catalogue, name, updated; popular and rating need a server) | 41dad270 |
| H1 QSO log + Cabrillo, H3 ADIF export | WIRED as file exports (the ADIF UDP/TCP broadcaster is NOT wired) | 9bd99710 |
| H2 contest macros | transmit: stays unwired | - |
| H4 contest score, H5 band map | NOT DONE | list item 7 |
| H6 RadioInfo | WIRED, display only | 830deb1b |
| H7 X-Plane | NOT WIRED (no slot can be filled without a real dataref list) | - |
| H8 Antenna Genius | NOT DONE ("Select antenna" stays unwired) | list item 8 |
| H9, H10, H12, H13 | blocked / transmit as in the survey | - |
| H11 OmniRig read | NOT DONE | list item 9 |
| K1 packaging step | WIRED | 19e074d5 |
| P1 recorder start / stop | WIRED (ABI append) | c95a6bec |
| P2 ABI requests of wire-a / wire-b | none reached me | - |
| P3, P4 more ABI appends | NOT DONE | list item 10 |
| G1 helper programs in the store | WIRED | 0f74888c |
| L1 CLI options | NOT DONE | list item 11 |
| L2 decode database / alert rules | NOT DONE | list item 12 |
| L3 metrics page | WIRED | 36af574a |
| L4 webhooks, L5 MCP server | NOT DONE | list items 13, 14 |
| L6 CAT follower | NOT DONE | list item 15 |
| L7 CAT emulation | WIRED (TCP; the serial variant is not) | 1d07d036 |
| L8 Discord presence | NOT DONE | list item 16 |

Counts: of the 56 survey rows, 19 are wired (S1 S3 S4 S5 S6 S7 S8 S10, W2 W7a W9, H1/H3 H6, K1 P1 G1, L3 L7, and the foundation), 1 was already built by serial lane A, 4 are blocked, 3 are transmit-side, 1 left unwired by decision, 28 not done.

## 2. What was wired, with the decisive result lines (full text in each commit message)

| Commit | Item | Test (file, real result) |
|---|---|---|
| 19e074d5 | K1 `package-manifests` step; `packaging_manifests` no longer silently absent | `release_package_step` 14 checks 0 failed; `packaging_manifests` Passed 17.68 s |
| 17e87b39 | the block, `wirec.json`, `FOXAPP_OP_WIREC_SETTING` | `test_engine_api_wire_c` all checks pass |
| e358b1cc | S1 | `test_wire_c_portable` 65 checks, 0 failed |
| 439a733d | S6 | `test_wire_c_updater` 30 checks, 0 failed |
| 50b70895 | S3 S4 S5 | `test_engine_api_wire_c_power` 56 checks, 0 failed |
| c0ec098c | W7a | `test_engine_api_wire_c_web` (CORS part) |
| c65b0e0a | W2 | `test_engine_api_wire_c_web` 91 checks at that commit |
| c95a6bec | P1 | `test_plugin_recorder`; `test_engine_api_wire_c_recorder` 26 checks (40 of 40 clean runs) |
| 0f74888c | G1 | `test_plugin_repo_helper` 199 checks; `test_engine_api_wire_c_store` 40 checks |
| 41dad270 | W9, S5 fix | `test_engine_api_wire_c_store` 53 checks; `test_engine_api_03` Passed after the fix |
| 36af574a | L3 | `test_engine_api_wire_c_web` 107 checks |
| dd1aa3de | S7 S8 S10 | `test_engine_api_wire_c_sys` 116 checks |
| 9bd99710 | H1 H3 | `test_engine_api_wire_c_ham` 84 checks |
| 830deb1b | H6 | `test_engine_api_wire_c_ham` 111 checks |
| 1d07d036 | L7, the ~Engine ordering fix | `test_engine_api_wire_c_cat` 46 checks (12 of 12 clean runs) |

## 3. Break-it table (the behaviour removed, red seen, restored, green)

| Item | Mutation | Result |
|---|---|---|
| K1 | hash comparison replaced by true | `release_package_step`: 1 failed; restored 14/0 |
| S1 | the call in bootProcess removed | `test_wire_c_portable`: 7 failed; restored 65/0 |
| S6 | the key-present check always passes | `test_wire_c_updater`: 13 failed; restored 30/0 |
| S4 | the keep-awake pass removed | `test_engine_api_wire_c_power`: 9 failed |
| S3 | the resume handling disabled | same test: 3 failed; restored 56/0 |
| W7a | the policy never stored | `test_engine_api_wire_c_web`: 6 failed |
| W2 | the sign-in gate removed | same: 5 failed |
| P1 | the engine's drain removed | `test_engine_api_wire_c_recorder`: 6 failed |
| G1 | program-image check disabled; rollback removed; consent gate removed | `test_plugin_repo_helper`: 8 failed; 2 failed; `test_engine_api_wire_c_store`: 5 failed |
| W9 | name order disabled | `test_engine_api_wire_c_store`: 1 failed |
| L3 | sign-in gate removed | `test_engine_api_wire_c_web`: 1 failed |
| S7 / S8 | import token check removed; tool-flag skip removed | `test_engine_api_wire_c_sys`: 2 failed each |
| H1 | call-sign check removed | `test_engine_api_wire_c_ham`: 8 failed |
| H6 | the socket never closed on a change | same: 2 failed |
| L7 | the 4532 refusal removed | `test_engine_api_wire_c_cat`: 2 failed |

Not break-it-checked: the settings registry's range rules (covered by refusals, not by a mutant), the first-run preset facts, the metrics fact values.

## 4. SCREEN HAND-OFF (every control a screen lane must draw; English strings; none translated)

Settings: send `FOXAPP_OP_WIREC_SETTING` with text `<key>` + 0x1F + `<value>`; read `fact.wirec.<key>` (all LOCAL).

| Control (English label) | Op / fact | Range | Default |
|---|---|---|---|
| "Keep the PC awake while recording" | key `sys.keep_awake` | on/off | on |
| "Look for radios again after the PC wakes" | `sys.resume_reopen`; read-out `fact.sys.resume.count` | on/off | on |
| "Reduced effects" | `perf.mode` | auto / on / off | auto |
| read-out "CPU %s%%   Memory %s MB   GPU %s%%" ("n/a" when ""), "Reduced effects are on" | `fact.perf.cpu_percent`, `.memory_mb`, `.system_memory_percent`, `.gpu_percent`, `.reduced`; the six switches `fact.perf.switch.ui_frame_cap_hz`, `.waterfall_rows_per_second`, `.animations`, `.glow_and_shadows`, `.three_d_views_live`, `.background_map_redraw` (read at the call, LOCAL, coarse steps) | - | - |
| "Back up settings when FoxSDR closes" | key `sys.backup_on_close` (the closing-time backup itself is NOT wired: list item 1) | on/off | off |
| "Back up settings now" | `FOXAPP_OP_SYS_BACKUP` ival[0] keep (0 = key `sys.backup_keep`, 1..50, default 10); `fact.sys.backup.last` | - | - |
| "Export settings..." | `FOXAPP_OP_SYS_EXPORT` text path; `fact.sys.export.last` ("exported to the file you chose (N secret or identity item(s) left out)") | - | - |
| "Import settings..." then confirm "Replace %d file(s)? Your current ones are kept as .before-import copies." + button "Import" | `SYS_IMPORT_PLAN` text path, then `fact.sys.import.plan` (lines "<name>", a tab, New / Replace / Identical), `.token`, `.notes`; `SYS_IMPORT_COMMIT` text token; then "Restart FoxSDR to finish importing" while `fact.sys.pending` is "import" | - | - |
| "Reset everything to defaults..." | `SYS_RESET_PLAN` ival[0] plugins, ival[1] logs, then `fact.sys.reset.plan`, `.token`; `SYS_RESET_COMMIT` text token; "Restart FoxSDR to finish" while `fact.sys.pending` is "reset" | - | - |
| "Cancel the pending change" | `SYS_PENDING_CANCEL` (NO_CHANGE when none) | - | - |
| "What the last start did" | `fact.sys.lastboot` | - | - |
| First-run pages | `SYS_FIRSTRUN` text `radio:<n>` (n indexes `fact.sys.firstrun.radios`, newline separated labels), `preset:<id>` (`fact.sys.firstrun.presets`: "<id>", a tab, "<title>" per line; "" = none), `telemetry:0` or `telemetry:1`, `next`, `back`, `skip`; `fact.sys.firstrun.needed`, `.step` (welcome, radio, preset, privacy, done), `.done` | - | - |
| "Shared folder for syncing profiles" | key `sys.sync_folder` (stored, NOT used: S9 not done) | text | empty |
| "Order" in the Plugin store | key `store.sort` | catalogue / name / updated | catalogue |
| helper plugin badge and pane | `fact.store.<key>.helper` ("GPL helper - source: %s"), `.helper.detail` (newline separated: "This plugin uses a separate program, %s %s (%s). FoxSDR starts it and sends it audio; it is not part of FoxSDR and cannot see FoxSDR's data.", "Source code: %s", "Files that will be installed beside the plugin:", one line per file), `.helper.consent` | - | - |
| "I understand this plugin installs a separate program" (a tick before "Install") | `FOXAPP_OP_STORE_HELPER_CONSENT` text key, ival[0] 1/0; the refusal sentence is "this plugin runs a separate program; confirm that you want it installed before it is fetched" | - | off |
| "Show the /metrics page to signed-in browsers" | key `web.metrics` | on/off | off |
| "Allowed web origins (one per line)" | key `web.cors_origins` | up to 32 origins | empty |
| "Allow cross-origin sign-in" | key `web.cors_credentials`: only "off" is accepted (needs HTTPS: owner decision) | - | off |
| Contest log: header form | `CABRILLO_HEADER` JSON (contest, callsign, location, name, address[], operators[], soapbox[], categories); `fact.ham.cabrillo.contests` | - | - |
| "Log a QSO" | `QSO_ADD` JSON {t, f, m, c, me, s[], r[]}; `QSO_REMOVE` ival[0]; `QSO_CLEAR`; `fact.ham.qso.count`, `fact.ham.error` | 20,000 | - |
| "Export Cabrillo log..." and "Export ADIF log..." | `CABRILLO_EXPORT` / `ADIF_EXPORT` text path; `fact.ham.export.last`, `fact.ham.cabrillo.problems` | - | - |
| "Follow radio info from DXLab Commander or N1MM+" | keys `hamops.radioinfo.enabled`, `.address`, `.port` (1..65535), `.allow_other`; read-out `fact.ham.radioinfo.<1..4>.freq_hz`, `.tx_hz`, `.mode`, `.split`, `.transmitting`, `.app`, and `fact.ham.radioinfo.running`, `.port`, `.error` | - | off, 127.0.0.1, 0, off |
| "Answer CAT commands from a logging program" | keys `cat.emulation.enabled`, `.dialect` (kenwood-ts480, yaesu-ft891), `.port` (not 4532), `.bind_all`; `fact.cat.emulation.running`, `.port`, `.clients`, `.error` | - | off |
| Units, number format, digits, colour set | keys `access.units` (automatic, metric, imperial, aviation, nautical), `access.number_format` (dot, windows), `access.digits`, `access.palette`: stored and published; NOTHING READS THEM YET | - | automatic, dot, latin, normal |

## 5. ABI REQUESTS and the diff for the plugins merge lane

None reached me from wire-a or wire-b. The ABI header in this tree is `src/core/plugin_abi.h` (not `include/plugin_abi.h`). The exact append (`git diff e7e25fc9 HEAD -- src/core/plugin_abi.h`, 18 lines): `#define CASCADE_RECORDER_LABEL_CHARS 33`, its comment block "Recorder (appended after host API level 2; wiring lane C)", and two members appended at the END of `CascadeHostApi`, after `events_seq`: `int32_t (*recorder_start)(void *ctx, const char *label);` and `int32_t (*recorder_stop)(void *ctx);`. `apiLevel` is unchanged (the struct size is the version). `third_party/foxsdr_api/foxsdr_api.h` (the engine API) was NOT changed: every op of this lane is a FOXAPP op in `app_commands.hpp`. `docs/PLUGIN-API.md` has the "Recorder" section. Tests that asserted "events_seq is the last member" (`test_plugin_events`, `test_plugin_schedule_hint`) now expect the two new members after it. The plugins repository's copy must gain the same lines; there is still no byte-compare test (manual diff). `test_plugin_abi3_compat` against the 106 published binaries: `269 checks, 134 failed` in this build AND in the merge lane's build (the stale capability assertion): not caused by the append.

## 6. New operations and keys

Operations (each in `app_commands.hpp` and the `kOps` table, each named in `test_apply_command`'s coverage; all LOCAL sessions only, a REMOTE session is DENIED by the engine's rule for app ops): `0x8A00 WIREC_SETTING`, `0x8A01 STORE_HELPER_CONSENT`, `0x8A02 SYS_FIRSTRUN`, `0x8A03 SYS_EXPORT`, `0x8A04 SYS_IMPORT_PLAN`, `0x8A05 SYS_IMPORT_COMMIT`, `0x8A06 SYS_BACKUP`, `0x8A07 SYS_RESET_PLAN`, `0x8A08 SYS_RESET_COMMIT`, `0x8A09 SYS_PENDING_CANCEL`, `0x8A0A QSO_ADD`, `0x8A0B QSO_REMOVE`, `0x8A0C QSO_CLEAR`, `0x8A0D CABRILLO_HEADER`, `0x8A0E CABRILLO_EXPORT`, `0x8A0F ADIF_EXPORT`. The next free number in the block is `0x8A10`.

Settings keys (file `wirec.json` beside `config.json`; `config.json` is NOT touched by this lane, so it stays loadable by earlier versions; default after each key; a feature that opens a port, a device or a file is off): `sys.keep_awake` 1, `sys.resume_reopen` 1, `sys.backup_on_close` 0, `sys.backup_keep` 10, `sys.sync_folder` empty, `perf.mode` auto, `access.units` automatic, `access.number_format` dot, `access.digits` latin, `access.palette` normal, `store.sort` catalogue, `web.cors_origins` empty, `web.metrics` 0, `web.cors_credentials` off (the only value), `hamops.radioinfo.enabled` 0, `.address` 127.0.0.1, `.port` 0, `.allow_other` 0, `cat.emulation.enabled` 0, `.dialect` kenwood-ts480, `.port` 0, `.bind_all` 0.

Other files this lane writes (all beside the settings, each created only by the feature that needs it): `wirec.json`, `qso-log.json` (the person's own log), `wirec-pending.json` and `wirec-pending/` (a staged import or reset, removed when carried out or cancelled), `wirec-last-boot.txt`, `backups/`, `reset-<stamp>/`, `<name>.before-import-<stamp>`; beside the plugins: `helpers.json` (the companion files of each helper plugin) and a transient `.helper-stage-<pid>/`; beside the program: `foxsdr-portable-error.txt` on a refused portable start.

Facts in the list block: `fact.wirec.*`, `fact.sys.*`, `fact.ham.*`, `fact.cat.emulation.*`, `fact.store.<key>.helper*`. Read AT THE CALL (so the counter `listSeq` never moves for them): `fact.perf.*`.

## 7. Items not done, as an ORDERED list a follow-up can continue

1. S9 settings profiles, per-radio map, folder-sync planner (`core/sys_profiles.hpp`) and the automatic backup at close (`sys.backup_on_close`): both write config.json, so they need the staged-at-next-start mechanism S7 and S8 now have (`wire_c_sys.cpp`: a new `action` in `wirec-pending.json`); W5 web profiles follow.
2. A1 units, A2 number format: replace the literals named in `access.md` 8.2 (`track_metrics.hpp:1252...`, `map_view.cpp:89-103`, `host_logic.cpp:111-120,181-200`) with `quantify` / `formatNumber`, reading the keys; keep Automatic and dot byte-identical (a test pins it).
3. A3 speech, A4 sonification: the audio route (Q2) is the only open design point.
4. W1, W4, W8 page JavaScript in `web_server.cpp`'s embedded page (the model files carry the exact functions); no browser was available.
5. W3 clip in the browser (the recorder must accept a path, or a timed take through the engine's audio recorder).
6. W6 control arbiter (join, heartbeat, leave per session; `mayAct` before every tuning request; seven routes).
7. H4 contest score and H5 band map (pure models over the QSO book and spot input).
8. H8 Antenna Genius READ-OUT (status queries only; "Select antenna" stays unwired).
9. H11 OmniRig read (frequency and mode; a stand-in port in every test).
10. P3, P4 more ABI appends (airline and route strings in `CascadeTrackInfo`, marker level, choice-list setting kind); append only.
11. L1 CLI options (`process_boot.cpp parseFlags`; start-up ops after the engine is up).
12. L2 decode database and alert rules (in memory only; disk is an owner decision).
13. L4 webhooks (http only, off, a stand-in receiver).
14. L5 MCP server (op list SET_CENTRE and SET_MODE only, loopback, off).
15. L6 Kenwood / Yaesu CAT follower (`source/rig_cat.*`; reads the rig, never writes).
16. L8 Discord presence (off until an id is given; a local pipe).
17. The ADIF UDP/TCP broadcaster of H3, X-Plane slots (H7), the serial variant of the CAT emulation, an undo of an applied reset, and `docs/api/openapi.yaml` entries for `/api/mybookmarks*` and `/metrics` (its test scans `web_server.cpp` only: extend it to `web_wire_c_*.cpp` first).

## 8. Owner decisions (new or still open)

1. Manifest signature: who holds the Ed25519 seed and when `kOwnerKeyHex` is filled (S6 is wired and inert until then); and whether HTTPS alone suffices.
2. A code-signing certificate (S12).
3. First-run wizard: the usage-reports switch changes only if the person chooses it in the wizard, the program's default stays. Say if the wizard should ask.
4. The number-format default "follow Windows" (kept "always a dot" until A2 is wired).
5. Helper programs: the separate consent tick is built (required by default); code signing of helper executables (SmartScreen); where helpers are hosted; a store shelf for them.
6. The metrics page has no scraper token (no credential storage): a Prometheus scraper cannot sign in with a cookie. Say if a token is wanted and where it may be kept.
7. CORS credentials (needs HTTPS and a change of the session cookie's SameSite): `web.cors_credentials` accepts only "off".
8. UPnP (left unwired), the decode database on disk, webhook cleartext URLs, CAT emulation exposure (`bind_all`).
9. Passwords at rest: NOT decided; this lane built no credential storage. Present behaviour (unchanged): PASSWORD-kind plugin settings and the web password hash are written to `config.json` (dated backup copies included); protection at rest is unverified. The export bundle strips every key whose name looks like a secret.
10. S3 resume: whether a healthy open radio should be closed and reopened after a sleep.
11. The QSO log (call signs the person typed) is on disk in `qso-log.json`: confirm that a user-typed log file is acceptable (it is in no report, setting, log line or fact).

Questions put to the orchestrator in Phase 0 and not answered: Q2 (the audio route for speech and tones) and Q3 (the QSO store: built as proposed, please confirm).

## 9. Gates and the two config hashes

See section 10.

## 10. Gate results (headless label, 997 tests, Slint OFF, tip 7a5816ca, config Release, `-j4`)

The GUI gate was NOT run: this build has Slint OFF; the merge lane runs it.

| Slice | Result | Reds |
|---|---|---|
| 1-200 | 195/200 | test_airband_data, test_band_plan, test_converter_call_sites (the five false failures with the two below), test_engine_api_source_kinds (base red, missing fixture), test_engine_api_decode_radio (base red, timeout) |
| 201-400 | 199/200 | test_page_resize_id (known) |
| 401-600 | 197/200 | test_png_decode (known); test_rtlsdr_live and test_sdrplay_source failed at 26-29 s under load and PASSED alone (36 s each) |
| 601-800 | 199/200 | test_window_failed_app (known, Slint-off configuration) |
| 801-1100 | 197/197 | none |

Earlier first pass (before commit 7a5816ca) had one real regression: test_usage_diag_window_free (the resource sampler's late PDH handles moved the diagnostics bundle's handle counters). Fixed by making the first sample synchronous at the first pump; it passes in the final run. test_diagnostics_free failed once on an uncommitted tree (stale-stamp family) and passes after commit and rebuild.

Flakes seen and passing alone: test_engine_main_smoke (about 1 run in 6, also on the engine-merge lane's baseline binary D:/foxsdr-build/mr-dsp), test_engine_api_03 and test_engine_api_merge (under `-j4` load), test_plugin_store_window_free, test_record_start_stepped, test_remote_key_release_free, test_rtlsdr_live, test_sdrplay_source. test_plugin_abi3_compat against the 106 published plugin binaries: 269 checks, 134 failed, identical on the baseline binary (not caused by this lane).

Config hashes: real `%APPDATA%/foxsdr/config.json` sha256 before `c21ce98dc30e50327f7666f7ee29047c0983d130ca3f21716d0c0e20e04603c3`, after the same.

Stray processes: none of this lane's. Two foreign test processes exist (test_engine_api_03 from D:/foxsdr-build/en-tuning, started 2026-11-13 09:40; test_soapy_enum_proc, started 2026-11-12 00:28) and were left alone. Incidents: none.
