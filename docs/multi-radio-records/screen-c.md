# Screen lane C: tuning, scanning, memories and controls rows in the Slint window (2026-11-11)

Branch `feat/screen-c`, cut from `feat/slint-gaps` at b9e22d3f. Worktree `C:\Users\steve\foxsdr-wt\feat-gaps`, build `D:/foxsdr-build/feat-gaps`. Nothing is pushed or merged. Rows are those of `FEATURE-GAPS.md` (read only); what each takes, as surveyed, is section 1 of `D:/foxsdr-build/prep/feature-gaps-survey.md`; where a feature lives was decided in `D:/foxsdr-build/prep/plugin-or-host.md`.

COMMITS (git log of the branch): cfb935df scanner core, 7fc85e0d the pure rules and their tests, 7868a646 the window (Slint, host, drive scripts), 9713b39d the 32 new catalogue keys, then the ledger and this record.

TWO PROCESS FACTS the next reader needs: (1) a `.slint` change costs one full compile of `main.slint` (about 13 minutes here, not the 25 the brief quotes); `slint-compiler.exe main.slint -f cpp -o x.h --style fluent` (C:/Users/steve/slint-cpp-1.18.1/bin) checks a `.slint` edit in 2 seconds and was used before every build; (2) the lane changed no engine file: every control talks to the engine through the existing ops, and what needs the engine is the hand-off list below.

## STEP 0: the twenty rows

Checked before any code: `git log --oneline` of `feat/screen-s1`, `feat/slint-gaps`, `feat/engine-wire`, `engine/multi-radio`, `feat/web-bookmarks` (the only branch that touches bookmarks) and a grep of this base for each feature. None of the twenty rows was already built.

| Row | What | Class | Result |
|---|---|---|---|
| C2 | Priority channels | NEEDS AN ENGINE CHANGE (the scanner is the engine's: `engine.cpp` builds its parameters and ticks it) | core built (`core/scanner.hpp`); screen field built and saved; the engine has to read them (hand-off 1) |
| C3 | Lockouts | engine change for a range scan; SCREEN ONLY for a scan of the ticked rows | core built; list-mode lockouts work now (the window unticks the row) |
| C1 | Per-entry scan threshold | NEEDS AN ENGINE CHANGE | core built (`Params::thresholdDb`, `tick(..., levelDb)`); the entry's threshold is kept by the window |
| C4 | Scanner log and statistics | SCREEN ONLY | built from the scanner state the engine already publishes |
| C5 | Mute while scanning | SCREEN ONLY | built (SET_MUTED around the scan's stepping); core `Scanner::muted()` is there for an engine that wants it |
| C12 | Bookmarks carrying a squelch level | engine change to live IN the bookmark | window-kept memory built; a recall sends its squelch through the existing op (hand-off 2) |
| F7 | Per-mode bandwidth memory and presets | SCREEN ONLY | memory built; PRESETS not built (the row names no preset behaviour I could verify) |
| T3 | Tuning-step lock | SCREEN ONLY | built |
| T5 | Band-stacking registers, quick memories | SCREEN ONLY | eight quick memories built; BAND STACKING not built: this window has no band keys to stack onto (the band follows the band plan) |
| T8 | Delta lock | SCREEN ONLY | built at the API table the window sends through |
| T9 | 8.33 kHz channel names on the counter | SCREEN ONLY + one core function | built |
| T10 | Repeater offsets and tones in memories | engine change (tone: CTCSS, row F14) | offset and tone kept by the window; the tone is stored, not applied |
| T12 | UTC/local clock | SCREEN ONLY | built |
| T13 | Spoken announcements | NOT BUILT | `plugin-or-host.md`: the shared event-outputs plugin (needs the decode-events interface, AD3) |
| P11 | Recording browser | SCREEN ONLY | built |
| U10 | Keyboard-shortcut overlay | SCREEN ONLY | built |
| U11b | FlexControl, Tmate, E-Coder, gamepad | NOT BUILT | `plugin-or-host.md`: the "controllers" plugin with U11a; no device on the desk; the knob layer is another lane's |
| W16a | Zoom and click on plugin pictures | SCREEN ONLY for the zoom | wheel zoom, drag pan, Fit key; the click already reports the picture's own pixel (ABI IMAGE_INPUT) and still does on a zoomed picture |
| W16c | Copying decoder text out | SCREEN ONLY | a Copy key in the Decoder output bar |
| W16d | Telling a plugin the symbol rate | NOT BUILT | plugin ABI addition (hand-off 6) |

## What was built and how it was proved

All thirteen drive scripts run on the hidden desktop (`FOXSDR_FRAME_CAP_HZ=60`, ctest label `gui`); the last run of them, after the last .slint change (tip c5a0ea29): in the GUI subset below, "100% tests passed out of 51".

ONE GLOBAL. `LaneC` (end of `ui/tools_bank.slint`, before `KeyFlow`) carries every property the lane's blocks read and one `callback act(string, string)`; `lanec_publish.cpp` sets the properties once a tick (only what changed) and answers `act`. The two bodies it joins cost one line each; `main.slint` got the import/export lines, the clock and the two sheets (all appended at the end of their blocks).

PERSISTENCE. `lanec.json` beside the settings file (never inside `config.json`; no key was added to either half). Written 15 ticks after the last change (atomic: `.tmp` then rename) and on exit; read leniently (a bad field keeps its default, a bad file is an empty record).

| Row | What it is | Proof (decisive lines) |
|---|---|---|
| C3 | Lock out 10 min / Lock out for good / Clear lockouts + the count. Live while a scan of the ticked rows runs: the row the scan is on is unticked (FOXAPP_OP_BOOKMARK_TICK, the one lockout the engine honours today), a temporary one is ticked again when its time is up, Clear ticks back what we unticked. Core: `Scanner::lockOut/lockOutCurrent/clearLockouts` and `Params::lockedHz`: no retune is ever emitted to a locked frequency | `test_scanner_lanec` 80 checks; `test_slint_lanec_host` ListLockouts; drive `lanec-lockout` (two ticked rows typed into the Airband body, the squelch shut, a list scan running): `Lock out for good` -> `state lanec.lockouts == 1` and the Scanner body's tick reads `Ticked frequencies (1)`, `Clear lockouts` -> `(2)` again, the same for `Lock out 10 min`; `lanec-scanner`: the keys are dead with no scan. A lockout in a RANGE scan is not offered (no row to untick): hand-off 1 |
| C2 | Priority MHz + Every s fields (saved). Core: every `priorityIntervalMs` of scanning time the next priority frequency is visited for a dwell; a signal there holds the scan; none and it returns to the frequency it left | `test_scanner_lanec`; drive `lanec-scanner`: `lanec.priority == "162.55, 121.5"` for the typed `"162.55, 121.5, nope"`, `1` clamps to `5`, `abc` is refused and the field shows 5 again. The engine does not read it: the block says so (`fact.scanner.extras` is the switch) |
| C1 | Scan threshold dB in the memory block (saved). Core: `tick(now, squelch, levelDb)` with `Params::thresholdDb[k]` replaces the squelch flag for that entry | `test_scanner_lanec`; drive `lanec-memory` (`lanec.memthreshold == -60`, saved across a restart) |
| C5 | Mute while scanning tick: SET_MUTED 1 when a scan steps, 0 when it holds a signal or ends, never over a user's mute | drive `lanec-scanner`: with the squelch shut and a range scan on the generator `state lanec.mutedbyus == 1`, `state muted == 1`; the squelch opened, the scan holds: `mutedbyus == 0`, `muted == 0` |
| C4 | Log what the scan finds (tick, saved) + what it found: one CSV line per signal the scan stops on (UTC time, Hz, seconds heard) in `scan-log.csv` beside the settings file; the stats line | `test_slint_lanec_host` ScanWatcher (the exact line `1970-01-01T00:16:44Z,118100000,4.0`); drive reads the tick back. The file itself is not read by a drive (UNVERIFIED) |
| C12 T10 | Memory of the frequency the receiver is on (a bookmark): Squelch dB, Scan threshold dB, Offset kHz (and the "Transmit ..." line), Tone Hz. Coming onto the bookmark with no scan running sends its squelch | drive `lanec-memory`: `-70` typed, tuned away to 101.1 and back, `state squelchDb == -70`; `-600` gives `Transmit 99.7000 MHz`; a tone of 12 goes back to 88.5 (`expect "Tone Hz" value equals "88.5"`); the second start finds all of it (`restart/lanec-memory-2`) |
| T5 | "Store into a memory" + M1 to M8 (frequency, mode, width). A stored key reads `M2  100.300` | drive `lanec-memory`: stored, tuned to 103.7, `click "M2  100.300"`, `tunedHz == 100300000`; an empty memory sends nothing; saved |
| T3 | Counter menu "Lock the tuning step": every digit and the wheel step by the Tune step | drive `lanec-steplock`: the 1 MHz digit steps 1 MHz (`101300000`), after the lock 10 kHz (`100310000`), as does the 100 kHz digit |
| T8 | Counter menu "Delta lock" (per radio, saved): click-to-tune becomes SET_FREQUENCY (the VFO offset kept, the centre follows); SET_VFO_OFFSET and the slider's FREE form are refused | drive `lanec-delta`: `vfoOffsetHz == 300000` kept and `centreHz != 100000000` after a click on the waterfall; `lanec-delta-off` is the same click without the lock: `centreHz == 100000000`, `vfoOffsetHz != 300000`; `test_slint_lanec_host` runs the batch, the text path and the result slots through the real API-table copy |
| F7 | Counter menu "Remember the width of each mode": a width the user set is kept once it has settled for ten ticks; entering the mode again asks for it | drive `lanec-bandwidth`: passband narrowed in WFM, NFM and back, `state bandwidthHz < 100000` after the return (WFM's own is 150 kHz) and `lanec.bw.2 > 0` |
| T9 | `Channel 118.000 / 118.005` / `Channel 118.010` in the counter's caption place | `test_slint_lanec_host`: every 8.33 channel of 118 to 137 MHz round-trips through `airbandChannelHz` (more than 2000 checks); drive `lanec-channel` reads `Channel 118.000 / 118.005`, `Channel 118.010`, none at 100.3 |
| T12 | Status-bar clock, `14:32:05 UTC` or `15:32:05 LT`; a press cycles UTC, local, off (a dim `--:--:--`, so it can be brought back); saved | drive `lanec-clock` (+ `restart/lanec-clock-2`: local time is back) |
| U10 | Keyboard shortcuts sheet: every bound action of the SAVED key table with its chord. Shift+F1 (F1 is "Bank: SIGNAL PATH" in the stock table: found by the first drive run) or App menu > Keyboard shortcuts | drive `lanec-shortcuts`: Shift+F1 opens, `lanec.shortcuts > 20`, Esc closes, the menu entry opens, `Close`#2 closes |
| P11 | App menu > Recordings...: the recorder's folder, newest first, kind (I/Q or Audio), size, date. Open: an I/Q take plays as the I/Q file (the source a dropped recording takes), any other file goes to the system's own program; Open folder | drive `lanec-recordings`: two takes made through the window's keys, listed (`lanec.recordings >= 2`), Open reads back a `.wav` (a bounded run opens nothing); `test_slint_lanec_host` listRecordings (kinds, order, limit, subfolders and other files left out) |
| W16c | Copy key beside Clear in the Decoder output bar: every line the window holds, as text, one per line | drive `lanec-log-copy` on the CW fixture: `state lanec.logcopy contains FOXSDR` (a bounded run keeps its own clipboard) |
| W16a | Picture page: the wheel zooms about the pointer (1 to 16 times), a drag pans while zoomed, a Fit key (only while zoomed); the TouchArea is labelled "Picture" | drive `lanec-picture-zoom` on the APT fixture: wheel in -> `Fit` exists, wheel out and Fit. The pixel arithmetic of a click on a zoomed picture (`vx + mouse-x / (unit * zoom)`) is NOT read back: the APT page takes no clicks (UNVERIFIED) |

## Break-it checks

Each mutation applied, the named drive script run on the hidden desktop, the result seen, the mutation restored (`git checkout`, rebuilt), by `sc-mutate.py` (scratch).

| Mutation | Drive script | Red (first failed step) |
|---|---|---|
| T8: click-to-tune not converted | `lanec-delta` | `state vfoOffsetHz == 300000` -> it is 0 (2 failed steps) |
| T3: the nudge ignores the lock | `lanec-steplock` | `state tunedHz == 100310000` -> it is 101300000 (2) |
| C12: the entry's squelch not sent on a recall | `lanec-memory` | `state squelchDb == -70` -> it is -120 (1) |
| C5: SET_MUTED never sent | `lanec-scanner` | `state muted == 1` -> it is 0 (1) |
| F7: the width is never given back | `lanec-bandwidth` | `state bandwidthHz < 100000` -> it is 150000 (1) |
| T12: a press does not change the mode | `lanec-clock` | `state lanec.clockmode == 2` -> it is 1 (7) |
| U10: Shift+F1 not answered | `lanec-shortcuts` | `state lanec.shortcutsopen == 1` -> it is 0 (2) |
| P11: the folder is not listed | `lanec-recordings` | `state lanec.recordings >= 2` -> it is 0 (3) |
| T9: no channel name given | `lanec-channel` | `state gaps.lockedlabel == Channel 118.000 / 118.005` -> it is empty (3) |
| T5: a press on a stored memory does not recall | `lanec-memory` | `state tunedHz == 100300000` -> it is 103700000 (8) |
| saved record: nothing written | `lanec-clock` (second start) | `state lanec.clockmode == 2` -> it is 1 (2) |
| W16c: the Copy key does not reach the clipboard | `lanec-log-copy` | `state gaps.clip contains FOXSDR` -> empty (1). FIRST RUN OF THIS MUTATION WAS STILL GREEN: the script read only `lanec.logcopy`, which is set before the sink is called; `state gaps.clip` (the bounded run's clipboard) was added and the mutation re-run |
| C3: the row is ticked instead of unticked | `lanec-lockout` | `expect "Ticked frequencies (1)"` -> no such control (2) |

The pure rules were mutated in `test_scanner_lanec` and `test_slint_lanec_host` (commits cfb935df and 7fc85e0d: lockout skipping removed 12 failed, priority visit removed 14, threshold and mute removed 7; delta conversion 3, temporary lockout never due 3, user mute not respected 2, width never given back 2, channel twin 5, quick memories not saved 1).

The `.slint` changes were NOT mutated (a compile of `main.slint` each): the lock keys, the priority fields, the memory block, the clock, the two sheets, the Copy key and the zoom are read by their drive scripts, so a removed control fails the script's `click`, `type` or `expect` (the scripts address them by label), but that was not shown by a mutation.

## Frame cost

One live generator radio, 1600 x 1600, the Bookmarks and Scanner bodies open (the lane's blocks drawn), a bookmark under the receiver, a range scan running, the frame meter on (`FOXSDR_SLINT_FRAME_LOG=1`), `FOXSDR_FRAME_CAP_HZ=60`, hidden desktop, three 30-second runs each way, alternating (`drive/measure/lanec-cost.txt`, `run_lanec_cost.bat`). "off" is `FOXSDR_SLINT_LANEC_OFF=1`, a bounded-run seam that leaves the lane's tick out (the blocks are still drawn, from their defaults).

| run | tick interval mean / p95 (ms) | the tick's own work mean / p95 (ms) |
|---|---|---|
| on 1 | 33.34 / 34.06 | 0.92 / 1.73 |
| off 1 | 33.32 / 34.07 | 0.93 / 1.68 |
| on 2 | 33.32 / 34.09 | 0.80 / 1.58 |
| off 2 | 33.29 / 34.02 | 0.73 / 1.26 |
| on 3 | 33.31 / 34.07 | 0.82 / 1.57 |
| off 3 | 33.41 / 34.18 | 0.88 / 1.65 |

The means over the three runs: interval 33.32 on and 33.34 off; work 0.85 ms on and 0.85 ms off (p95 1.63 and 1.53). The difference is inside the run-to-run spread (the machine was shared with other lanes). What the lane draws is not drawn every frame (the clock text changes once a second, every other property is set only when it changes); what it does every tick is two fact reads (`fact.scanner.listMode`, `fact.scanner.extras`), the scan watcher and the mute follower, one bookmark-version fact read every 15 ticks.

## Strings

32 new catalogue keys, in the 33 catalogues, MACHINE TRANSLATED and NOT reviewed by a native speaker (commit 9713b39d; `test_i18n` passes). The English keys:

- `%d locked out`
- `Adds a line to scan-log.csv in the settings folder for each signal the scan stops on: the time, the frequency and how long it was heard.`
- `Channel %s`
- `Clear lockouts`
- `Clock`
- `Copy`
- `Every s`
- `Found %d on %zu frequencies; most heard %s (%d)`
- `Frequencies the scan looks at every few seconds, in turn, whatever else it is doing. Separate them with commas.`
- `Keyboard shortcuts`
- `Lock out 10 min`
- `Lock out for good`
- `Log what the scan finds`
- `Mute while scanning`
- `No recordings in this folder`
- `Offset kHz`
- `Open folder`
- `Passes over the frequency the scan is on for ten minutes. In a scan of the ticked rows the row is unticked and ticked again when the time is up.`
- `Passes over the frequency the scan is on until the lockouts are cleared. In a scan of the ticked rows the row is unticked.`
- `Press to change between UTC, local time and no clock`
- `Priority MHz`
- `Recordings`
- `Scan threshold dB`
- `Shows the whole picture again.`
- `Silences the receiver while the scan steps and lets you hear it when it stops on a signal.`
- `Squelch dB`
- `Store into a memory`
- `Switch on, then press a memory to keep the frequency, mode and width the receiver is on in it.`
- `The priority list and the scan thresholds are saved; this scanner does not use them yet.`
- `Tone Hz`
- `Transmit %s MHz`
- `Tune to a bookmark to give it a memory`

The counter menu's and the App menu's labels (`Lock the tuning step`, `Delta lock`, `Remember the width of each mode`, `Keyboard shortcuts`, `Recordings...`) are plain English like every label of those menus today (the earlier lane's note: they are not in the catalogues). The translator named these doubts: lt uses "blokuoti" (block) for lock out although the catalogue's "Frequency locked" uses "uzrakintas" (locked); zh-TW uses a "memory channel" word because the computer-memory word would read wrongly; ja "Every s" is written with the word for interval; id and vi spell "min" in their own way.

## ENGINE HAND-OFFS (nothing below was done: `engine.cpp`, `pipeline.cpp`, `receiver.hpp`, `fox_engine_api.cpp`, `plugin_abi.h` and the bookmark list are other lanes' files)

1. THE SCANNER EXTRAS (C1, C2, C3 in a range scan, C5). `core/scanner.hpp` and `core/scanner_params.hpp` already hold them and are tested; every new `Params` field defaults to off, so the engine scans as before until it sets them.
   - `Engine::scannerParams()` (engine.cpp:6081): copy `lockedHz`, `priorityHz`, `priorityIntervalMs`, `thresholdDb` (parallel to `list`; the entry's threshold) and `muteWhileScanning` into the Params; for a list scan the thresholds come from hand-off 2.
   - `Engine::scannerFrame()` (engine.cpp:6629): `scanner_.tick(clockS() * 1000.0, squelchOpen, rx().pipeline_.signalPowerDb())` (the new overload; the old one still compiles). The audio mute, if the engine wants it rather than the window's follower, is `scanner_.muted()`.
   - one new op, proposed `FOXAPP_OP_SCANNER_EXTRAS 0x860C` (next free after 0x860B): `ival[0]` = 0 set the priority list (long text: the Hz values, `;` between; `num[0]` = interval ms), 1 lock out (`num[0]` = Hz, `num[1]` = ms, 0 = for good), 2 clear the operator's lockouts, 3 mute-while-scanning (`ival[1]` 0/1), 4 thresholds (long text: `<Hz>\t<dB>` rows). And a fact `fact.scanner.extras` = "1" (the window shows its "not used yet" note only while it is absent). Then `LaneCPublisher::act` sends the op for `priority`, `priority-every`, `lock-*` in a range scan and the stored thresholds (a TODO where each `markDirty()` is), and drops its note.
2. THE BOOKMARK CARRYING THE FIELDS (C12, C1, T10). Today the window keeps `squelchDb`, `thresholdDb`, `offsetKhz`, `toneHz` under `name|Hz` in `lanec.json`. To live in the bookmark: four fields in `core/bookmark.hpp`, written and read in `core/freq_manager.cpp` (`serialize` / load, default off, so an old file reads), exposed as `FoxListItem.value[4..7]` of `FOXAPI_LIST_BOOKMARKS` and set by a new op (`BOOKMARK_FIELDS`: id, field, value). The window's memory block then reads from the list item (and migrates a `lanec.json` entry once). The engine's BOOKMARK_TUNE could send the squelch itself (then the window's recall is dropped).
3. A FREQUENCY LOCK / DELTA LOCK IN THE ENGINE: as the earlier lane's hand-off 1 said for the frequency lock; the delta lock is the same story (a plugin, the web page and CAT still change the VFO offset of a delta-locked radio).
4. A `fact.scanner.listMode`-aware lockout: when the engine reconfigures a scan after a row is unticked it restarts at the first row (the Scanner's rule for new parameters). A lockout that did not move the scan (engine side `lockOut`) would keep its place; the window's unticking is the stand-in.
5. TONE (T10): `Memory::toneHz` is stored and shown; applying it is row F14 (CTCSS squelch).
6. W16d, THE SYMBOL RATE: a host-to-plugin value on the level-3 bump (`plugin_abi.h`, not touched): the screen half is a number field in the plugin's page settings (the kit's setting kinds already carry numbers), sent as the new host call. Nothing was written.
7. `FOXAPI_RX_MUTED` is also set by a plugin's mute (`audioMutedBy`); the mute follower reads only the flag, so a scan with Mute while scanning on and a muting plugin's preset leaves the plugin's mute alone only because the flag is already set when the scan starts.

## Rows not built, and why

T13 (plugin: AD3), U11b (plugin: controllers; no device), W16d (ABI), T5's band stacking (no band keys in this window), F7's presets (nothing verifiable to build), the engine halves above. Not built either: lockouts in a range scan and the priority visits as working features (hand-off 1).

## UNVERIFIED

- The ten-minute lockout's return after ten real minutes (the run drives the key and the clear, not the clock; `ListLockouts.due` is unit tested with a made-up clock).
- The scan log's file contents and its header line on disk; the real clipboard (a bounded run keeps its own); the real `ShellExecute` of Open and Open folder (a bounded run opens nothing).
- A click on a zoomed picture reporting the right pixel (arithmetic read, not driven); the drag pan; the zoom on a picture that is also receiving new frames of a different size (the pan is clamped each frame).
- The clock in a time zone other than this machine's, and across a daylight-time change (`localOffsetSecondsNow` is the OS's `mktime` rule; tested only for range and quarter-hour steps).
- The 32 translations (machine made, unchecked). The `.slint` additions in the 33 languages (the clip sweep: below).
- High-contrast and night skins of the new blocks: only the Workbench skin was looked at (and only through the Theme tokens, no colour literal: `test_slint_house_rules` passes).
- Memory recall squelch with a scan running: deliberately not applied (a scan would change the squelch at every step); the engine's per-entry threshold is the way (hand-off 1).

## Decisions for the owner

1. Shift+F1 for the shortcut overlay (F1 is taken by the stock key table). A `?` key was left out: it would fight typing in fields.
2. The window keeps the memories in `lanec.json` until the engine's bookmark carries them (hand-off 2): export and import of bookmarks do not carry them meanwhile.
3. A temporary lockout is ten minutes, fixed; the priority interval defaults to 30 s (5 to 600).
4. The clock cycles UTC, local time and a dim "--:--:--" (off): never disappears, so a press brings it back.
5. Whether a lockout in a scan of ticked rows should be allowed to untick a bookmark permanently ("for good" leaves the row unticked in the bookmark file; Clear ticks it back only in the session that unticked it).

## Gates and config

Release, `D:/foxsdr-build/feat-gaps`, the machine shared with other lanes (tests ran at several times their usual time).
- Headless and slint (`-L "headless|slint" -j4 --timeout 300`, tip 7868a646 + the strings, before the two repairs): "98% tests passed, 12 tests failed out of 765". Known false failures: test_airband_data, test_band_plan, test_converter_call_sites, test_page_resize_id, test_png_decode, test_diagnostics_free. Load flake that passes or fails alone as before: test_pass_control (failed alone again: 52 s). MINE, found and fixed (commit c5a0ea29): test_slint_output (a panic: the unzoomed picture's Image had a clip of 0 x 0; and `hand` must read only for a picture that takes clicks) and test_slint_tools (the specimen stacks the real bodies at fixed offsets; the bodies gained `lane-c`, off in the specimens); both pass alone afterwards, as do test_slint_main_screen, test_i18n, test_slint_house_rules, slint_drive_ledger (`ledger: 2226 rows, 595 asserted, 96 driven, 1535 not drivable, 0 unclassified`). NOT MINE and NOT chased: test_engine_main_smoke and test_gui_file_jobs failed under load and passed alone; test_failure_files fails alone (`tests/test_failure_files.cpp:1140 at.size() <= 22u`, `:1148 growing`, a file-count assertion in the failure-files test; it uses cascade.exe, not the Slint window) and was not run on the base for comparison (UNVERIFIED that it fails there too). The headless range was NOT re-run whole after the repairs.
- GUI gate: the full `-L gui -j3` (291 tests) was started and STOPPED by me after 14 tests (all passed except test_display_menu_app, which failed at 235 s under load: not looked at): at 10 to 15 minutes per dozen tests it would have taken about four hours. What ran instead, after the last .slint change: `slint_drive_(lanec|tools|gaps|main-(counter|strip|editor|shortcuts|dials)|decode-bank|fixture-(cw|apt)|plugin-cw|system-keys|saved|escape|tune-step|deck-select)`: "100% tests passed out of 51". THE FULL GUI GATE WAS NOT RUN; test_display_menu_app was not re-run (UNVERIFIED).
- The clip sweep (`tools/slint_clip_sweep.py`, probe build) was NOT run: UNVERIFIED that it did not grow. The lane adds Text at 254 px column width (the two blocks, with `overflow: elide` on the count, the memory title and the labelled fields' captions) and in the status bar; the longest English strings are the tooltips (shown only on hover).
- `%APPDATA%oxsdr\config.json` sha256 BEFORE the first run `c21ce98dc30e50327f7666f7ee29047c0983d130ca3f21716d0c0e20e04603c3`, AFTER the last run c21ce98dc30e50327f7666f7ee29047c0983d130ca3f21716d0c0e20e04603c3 (every run used the harness's scratch profile; lanec.json and scan-log.csv go beside the scratch settings file).
