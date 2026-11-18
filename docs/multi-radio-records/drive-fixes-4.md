# Drive fixes, group 4 (2026-11-18)

Branch `fix/drive-4`, cut from `4205a401` (engine merge 12), worktree `D:/foxsdr-wt/en-screen-j`, build `D:/foxsdr-build/en-screen-j` (Release, `--parallel 3`), hidden desktop `FoxSDRDriveFix4`, scratch `D:/foxsdr-build/df4-scratch/`. Nothing pushed. Source: drive handovers 12 to 14 and `D:/foxsdr-build/e2e-scratch/RESULT.md`.

| # | Reproduced | Cause (file) | Verdict | Proof |
|---|---|---|---|---|
| 1 beat | no beat was ever broken | the beat's transport `postJson` (telemetry.cpp) is https-only, like the update check; the e2e run used an http loopback Worker, so the beat was dropped before any connection. Schedule: armed at start-up (`window_service_options.cpp` heartbeat, `Engine::configureHeartbeat`), first beat on the first engine pass, then every 300 s; `kBeatCarriesRadios = false` only drops the radios from the body; the 300 s radio-report batch is another sender | NOT BROKEN; test added | `test_telemetry`: mock clock, beats at 0, 300, 600 s, body id/v/beat only; nothing with reporting off; http dropped. Break-it (interval doubled) RED 4 checks |
| 2 open phase | yes (F1) | contract 3.3: `radio_open` phase is `start ok` only; "a failed open is a `radio_fail`" | FOR THE SITE LANE | see report |
| 3 verdict | yes (test) | ADS-B on a 48 kHz Channel is not refused (create() succeeds); it prints its own "ADSB error:" line; `patch_logic.cpp infoModel` only knew refused plugins | FIXED | `test_slint_host_patch` RED 3 checks with the term removed |
| 4 globe options | by reading + test | `kit_publish.cpp keepPageModels` did not keep `PageTab.globe` lists; Satellites republishes them every tick | FIXED | new `test_slint_globe_body`: press, publish, release: fresh list 0 clicks, reused 1 |
| 5 S26 / S29 | S26 yes (by design); S29 no | S26: design lines 23-26, workspace.slint:10 say spectrum gestures act directly and do not select; drive deck-select pins it. S29: key lights, panes grow link keys, followers move | S26 PLAN WRONG; S29 NOT REPRODUCED (drive link-spectrum added) | drives pass |
| 6 number fields | no | all number fields already select on focus (fields.slint, tools/signal/system/patch/extend LineInput, digits); real press+pause+release+key replaces text | NOT REPRODUCED; guards added | new blocks in `test_slint_fields`, `test_slint_tools`; break-it RED both |
| 7a notice cut | yes | `main.cpp` fact read into `char buf[64]` | FIXED | drive pane-notice-whole (tail "is rtl_tcp running there?") |
| 7b DECODERS tip | yes | `TipLayer` clamped the tip into the window edge over its chip | FIXED | `test_slint_elided` RED 734 menu pixels over key, GREEN 0 |
| 7c memory block | yes, 20 px (699 / 679) | the "Transmit ..." Note existed only for a repeater entry | FIXED | drive lanec-memory-reserve, both readings 699 |
| 7d Remove key | yes | longer label wrapped the key 23 px | FIXED | `test_slint_tools` second press now at the first press's y |
| 8 keys | Shift+F1 in a field: yes; Ctrl+Up: only in a field (the field uses it); matrix double-click: by reading; slider track: no | `main.slint` shortcut FocusScope was a sibling, now parent; `lanek_publish.cpp` tiles emptied and refilled | Shift+F1 FIXED (drive keys-in-field RED then GREEN); Ctrl+Up BY DESIGN in a field (link-follow passes elsewhere); matrix FIXED by reading; slider NOT REPRODUCED | |
| 9 AOR | yes | firmware only under the program folder | FIXED | `test_aor_source`, break-it RED |

## Item 2: for the site lane
The contract defines no failed phase for `radio_open` (section 3.3). The site should treat a `radio_fail` for the same `radioKey` and session, whose `t` is not earlier than the `radio_open start`, as ending that open, whatever order they arrive (the app sends the fail first, then the journal start, seq 0 then 2).

## Report only
- 3D TUNNEL: TOOLS rail, "Demod scope" row, DEMOD SCOPE window, signal key I/Q or VECTOR (the VIEW row VECTOR / 3D TUNNEL shows only on a baseband input); needs I/Q samples flowing (a radio or file running).
- Survey TABLE / SURFACE: DECODE rail, Plugins, start the Survey Engine plugin, its panel tab in the Decode list; the toggle sits on that page (page title containing "survey"); draws once the plugin publishes channels.
- Radiosondes VIEW MAP / 3D TRACK: start the Radiosondes plugin, open its map page in the Decode list; draws the honest empty note until a balloon is heard.
- Solar system plugin: `plug-merge/src/solarsystem/solarsystem_plugin.cpp:119`, the sentence "fetching is switched off (FOXSDR_SOLARSYSTEM_NO_FETCH)"; drop the variable name.

## Changed expectations
| Test | Old | New |
|---|---|---|
| `test_aor_source` firmware path | contains "resources" | contains "firmware" |
| `test_slint_tools` Remove group second press | y Tb+273 | y Tb+250 |
| drive `tools-bookmarks-rows` | click "Yes, delete this group and its 1 bookmark(s)" | click "Remove this group" again |

Catalogues: no new sentence; 4892 keys x 33, 0 missing, 0 stale. Error-path pin 410, not touched.

## OPEN ROWS
Real-pointer proof of items 4, 6, 8 (matrix, slider), 7a to 7d; the https beat on a real Worker; Demod slider track click and S29 on the real pointer.
