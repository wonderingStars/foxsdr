# Not wired after engine merge 3: the three wiring lanes' NOT DONE lists in one ordered list

Written by engine merge 3 (2026-11-12/13) from the NOT DONE lists of `wire-a.md` (section 6), `wire-b.md` (4.5) and `wire-c` (section 7 of the lane's final record, `D:/foxsdr-build/prep/lane-results/wire-c-final-record-pending.md`, not yet committed by that lane). Items are ordered by what they unblock: first those that need an edit inside `pipeline.cpp` or the recorder (nothing else can start without them), then the plugin ABI appends, then the staged-at-next-start mechanism, then the work that is complete in the engine and waits for a feed or a page, then the items that stay unwired on purpose. "Owner lane" is the lane that wrote the survey row and holds its context; a follow-up lane picks it up.

Items that were dropped or changed in this merge are marked MERGE 3.

| # | Item | What it needs | Owner lane |
|---|---|---|---|
| 1 | I/Q playback speed, pause, step (`source/playback_transport.hpp`; wire-a row 17) | `IqFileSource::read` to return a held block, or the pipeline to learn "paused" (the source's contract says `read` never returns 0); the varispeed / block-skip belongs inside `read`. Decision D4 made playback wire-a's. | wire-a (wire-b 4.5 item 2, S12) |
| 2 | CW surround (wire-a row 13) | the pipeline's CW audio stage to have a stereo output (`pipeline.cpp` audio stage) | wire-a |
| 3 | Receive clarifier RIT (wire-a row 10) | the receive oscillator shift inside the pipeline's VFO; a "virtual RIT" by retuning moves the dial and was judged not to be the feature | wire-a |
| 4 | RF64 and SigMF recording writers (wire-a rows 44, 45) | the recorder (or a second sink beside it); FLAC was done by conversion after the take, which cannot do I/Q takes; the record lock's "SigMF logs its own retunes" flag stays false for lack of the writer | wire-a |
| 5 | Patch nodes: mixer, IF filter, signal generator, scope, and the edit history (wire-a rows 29-33) | `patch_graph.hpp`, `patch_runner.hpp` and the patch file format; the signal-generator node must be refused on every transmit sink (a test is needed) | wire-a |
| 6 | Airband per-channel squelch, filter, outputs (wire-a row 34) | `airband_monitor.hpp buildMonitorSet` | wire-a |
| 7 | Per-radio level store for every open radio (wire-a row 39) | the S1 store (rows every 250 ms for an hour, segments per retune); needs a size decision (memory) | wire-a |
| 8 | The spectrum-block GPU path (wire-b 4.5 item 7) | `SET_SPECTRUM` with the large FFT sizes into `GpuSpectrumEngine` (the operation `GPU_FFT_MODE` exists and engages the GPU spectrum for the standard path) | wire-b |
| 9 | I/Q pre-roll for the limit-mask capture (wire-b 4.5 item 8) | an I/Q ring in the pipeline | wire-b |
| 10 | Stitched sweep of an ordinary radio (wire-b X2, item 3) | a retune-and-measure loop over a normal source, through the engine | wire-b |
| 11 | Beast and AIS network output (wire-b 4.5 item 12; the SBS output carries no callsign, squawk or label) | the plugin track structure to carry what those formats need (ABI) | wire-b |
| 12 | Frame-log feed (wire-b ABI request 1) and MFSK tone-energy hand-over (wire-b ABI request 2, S11) | host calls for a decoder to push a frame record (bytes, length, CRC state, protocol name) and per-symbol tone energies; until then `FRAMELOG_READ` shows an empty log in a running engine | wire-b |
| 13 | Symbol rate to the host (wire-a ABI request 1, rows 38 and 41), credit text on `CascadeMarker` and the list item (request 2), more than four numbers in `FoxListItem.value[]` (request 3, optional), an API read call for the waterfall lines (request 4, optional) | appends to the plugin ABI and the engine API on the level-3 bump; the plugins repository takes them (see PLUGIN HEADER HAND-OFF for the one append already made) | wire-a |
| 14 | More ABI appends: airline and route strings in `CascadeTrackInfo`, marker level, choice-list setting kind (wire-c P3, P4) | append only, to `src/core/plugin_abi.h` | wire-c |
| 15 | Settings profiles, per-radio map, folder-sync planner (`core/sys_profiles.hpp`) and the automatic backup at close (`sys.backup_on_close`) (wire-c S9; W5 web profiles follow) | both write `config.json`, so they need the staged-at-next-start mechanism that import and reset now have (`wire_c_sys.cpp`: a new `action` in `wirec-pending.json`). Screen lane K built its own pending operations for backup, profiles and reset in the host: reconcile the two mechanisms first (see DUPLICATE rows in `screen-handoff-L.md`) | wire-c |
| 16 | Units and number format in the readouts (wire-c A1, A2) | replace the literals named in `access.md` 8.2 (`track_metrics.hpp:1252...`, `map_view.cpp:89-103`, `host_logic.cpp:111-120,181-200`) with `quantify` / `formatNumber`, reading the keys; keep Automatic and dot byte-identical (a test pins it). Screen lane K did number format in the host; units remain open (K 1) | wire-c |
| 17 | Spoken readout and sonification through the engine (wire-c A3, A4) | the audio route question (Q2) is unanswered; screen lane K plays both through SAPI and waveOut in the host (nobody has listened) | wire-c |
| 18 | Web page scripts: W1, W4, W8 (wire-c item 4) | page-string edits in `web_server.cpp`'s embedded page (the model files carry the exact functions); no browser was available to prove them | wire-c |
| 19 | Clip in the browser (wire-c W3) | the recorder to accept a path, or a timed take through the engine's audio recorder | wire-c |
| 20 | Control arbiter (wire-c W6) | join / heartbeat / leave per session, `mayAct` before every tuning request, seven routes | wire-c |
| 21 | Contest score and band map (wire-c H4, H5) | both are pure models over the QSO book and the spot input | wire-c |
| 22 | Antenna Genius read-out (wire-c H8) | status queries only; "Select antenna" stays unwired (transmit side) | wire-c |
| 23 | OmniRig read of frequency and mode (wire-c H11) | stand-in port in every test | wire-c |
| 24 | Command-line options (wire-c L1) | `process_boot.cpp parseFlags`; start-up operations after the engine is up | wire-c |
| 25 | Decode database and alert rules (wire-c L2) | in memory only; disk is an owner decision | wire-c |
| 26 | Webhooks (wire-c L4) and the MCP server (wire-c L5) | webhooks: http only, off, a stand-in receiver; MCP: operation list `{SET_CENTRE, SET_MODE}` only, loopback, off | wire-c |
| 27 | Kenwood / Yaesu CAT follower (wire-c L6) | `source/rig_cat.*`; reads the rig, never writes | wire-c |
| 28 | Discord presence (wire-c L8) | off until an id is given; a local pipe | wire-c |
| 29 | ADIF UDP / TCP broadcaster (wire-c H3), X-Plane slots (H7), the serial variant of the CAT emulation, an undo of an applied reset, OpenAPI entries for `/api/mybookmarks*` and `/metrics` | the OpenAPI test scans `web_server.cpp` only: extend it to `web_wire_c_*.cpp` first | wire-c |
| 30 | Moving the small user-file reads and writes of marker export / import and packs (wire-a rows 3, 6, 16) and the daily-folder `ensureFolder` at a take's start onto the lane's job worker | they run on the control thread (files of at most 1 MiB / 4 MiB, at the user's request): a deviation from "the pass does no disk work" that `test_gui_disk_audit` does not scan | wire-a |
| 31 | Open ends inside done rows (wire-a): project save / open stay with the screen (the pure header), "retune by the span and repeat" of the whole-span scan, bookmark `offsetKhz` (stored, not applied), band-memory attenuator and antenna (device facts, no engine setting); rows 28 and 42 are screen only | a decision per item | wire-a |
| 32 | Symbol file save and bit editor (wire-b S3, B6, item 9) | screen side | wire-b |
| 33 | Persistence of network sources in the configuration (wire-b item 10) | a saved-radio kind per source (`sanitiseSavedRadios` `kKinds`) | wire-b |
| 34 | Success path through the engine for icr8600, funcube, g39ddc, g33ddc and the Hermes-Lite 2 profile (wire-b item 11) | stand-ins exist only at the model level; a loopback stand-in per source | wire-b |
| 35 | ExtIO helper (wire-b Z3, item 5): adapter, helper build lines, installer line | a separate program; its own lane | wire-b |
| 36 | SDRplay rate map and low-IF (wire-b Y6, Z4, X8, item 6) | no hardware; touches the existing driver | wire-b |
| 37 | Sweep history save / load (wire-b item 4); combiner and direction finding (wire-b Z9, item 13) | history: a file format decision; Z9: no coherent source | wire-b |
| 38 | Radio-bridges radios not built: G31DDC (32-bit only library), Fobos, Perseus, RFNM, Harogic | G31DDC: a 32-bit helper process (owner decision); the others: the SDK headers read (a lane each); no row exists in the table | radio-bridges (not a wiring lane) |
| 39 | Network I/Q-out, SBS-out and track-store ports wider than loopback (wire-b) | the owner's wider-bind rule (a firewall prompt is raised by a wider bind) | wire-b |
| 40 | Eight-cursor engine copy (wire-b S9; MERGE 3: dropped, the three operations 0x8919-0x891B no longer exist) | nothing: screen lane K draws the eight cursors in the host (decision D2). Re-add only if the web page or a plugin needs the engine's copy | wire-b |
| 41 | The RBW trace in the engine (wire-b R1) | decision D2: NO, screen lane K draws it | wire-b |
| 42 | Transmit-side rows (XIT, tone burst, split, contest macros, H9, H10, H12, H13) | stay unwired: this program is receive only | wire-a / wire-c |
| 43 | Blocked or owner-gated: code-signed setup (wire-c S12, a certificate), UPnP (W7b, left unwired), manifest signature seed (S6 is wired and inert until `kOwnerKeyHex` is filled), CORS with credentials (needs HTTPS), companion launcher (wire-a row 15, starts programs the user names) | owner decisions listed in `engine-merge-3.md` | wire-c / wire-a |
| 44 | Needed-library alignment (MERGE 3, not done) | see NEEDED-LIBRARY ALIGNMENT in `engine-merge-3.md`: one data type for the two shapes | merge 3 / next wiring lane |
