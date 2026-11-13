# Screen lane L: the Slint controls for what the wiring lanes built (2026-11-13)

Branch `feat/screen-l`, started from `engine/multi-radio` `c85fa2f0` (clean; `git switch -c feat/screen-l`), worktree `C:/Users/steve/foxsdr-wt/mr-dsp`, build `D:/foxsdr-build/mr-dsp` (Slint ON, Release), scratch `D:/foxsdr-build/sl-scratch/`. Nothing is pushed or merged.
Real `%APPDATA%/foxsdr/config.json` sha256 at the start: `c21ce98dc30e50327f7666f7ee29047c0983d130ca3f21716d0c0e20e04603c3`.
Read first: `STANDARD-RULES.md`, `screen-lanes-common.md`, `screen-handoff-L.md` (the row list, sections A to F), `screen-k.md` (the generic row panel), `engine-merge-3.md` sections 5 to 7 and the owner-decision list, `engine-fixes-4.md`.

## Step 0: the table of rows (reading only, before code)

Classes: `build` = a control is drawn; `DUPLICATE: not built (kept: ...)` = the hand-off's "keep" sentence says another lane's control stays; `skipped (why)` = an owner decision is open, or the engine or the window lacks something this lane may not edit. Where the section column says "Tuning", "Scanning" and so on, it is one of the eleven new SYSTEM sections (after lane K's five) that this lane adds; they are drawn with lane K's row panel (`KRow` / `KPanel`), so a row costs C++ and no `.slint` recompile.

### A. Tuning, scanning, memories

| Row | Class | Where / note |
|---|---|---|
| A1 Back, Forward (frequency history) | build | Tuning and memories |
| A2 Channel plan, next / previous, go to, leave | build | Tuning and memories |
| A3 Export CSV / JSON, Import (markers) | build | Tuning; the path is typed (no file dialog may be raised) |
| A4 Bookmark note, tags, tag filter, search | build | Tuning; the bookmark is named by its number |
| A5 Save view as favourite, recall, rename, remove | build | Tuning; the picture's own six numbers are NOT sent or applied (hand-off) |
| A6 Export pack, Import pack | build | Tuning |
| A7 Scanner ranges, direction, scan back, save hit, trigger, level, margin, hysteresis, floor rise | build (what lane C lacks) | Scanning |
| A7b Scanner priority list, lock out, clear lock-outs, mute while scanning | DUPLICATE: not built (kept: lane C's block, rows 4800 and up) | |
| A7c Per-entry thresholds | build | Scanning |
| A8 Scan whole span without retuning | build | Scanning |
| A9 Station database (frequency column, unit, columns, delimiter) | build | Scanning |
| A10 Waterfall colour map Custom, gradient editor | DUPLICATE: not built (kept: lane K row 5230) | |
| A11 Average, smoothing, comb | DUPLICATE: not built (kept: lane K row 5227); the engine setting is the only one that smooths and de-combs, but offering a second "Average" next to K's is a choice between two mechanisms: owner decision | |
| A12 Waterfall history: pause, live, scroll back, "N s behind live", MB | partly built | Switch and size are not built either: the screen has no way to show the held lines (they are read in process through `WireA::waterfallCopy`, which the waterfall compositor, lane B's, would have to call): written as an ENGINE / SCREEN hand-off, no control that does nothing |
| A13 Trace mode (seven words) | DUPLICATE: not built (kept: lane K row 5231); the saved choice is a separate saved setting: owner decision | |
| A14 Bookmark fields (squelch, threshold, offset, tone) | build | Tuning |
| A15 Remember per band | build (volume and AGC only) | Tuning; the spectrum limits and zoom need the screen to file and apply them (hand-off); attenuator and antenna are never filed by the engine |
| A16 Keys: bandwidth up / down, cycle mode, centre signal, next / previous bookmark | build as keys | Tuning; binding them to chords is the key-bindings page's (hand-off) |
| A17 Daily folders, Use UTC | build | Recording and files |
| A18 Lock settings while recording, Also lock gain, greyed reasons | build | Recording; owner decision (default) open: the control shows and sets the engine's state and changes no default |
| A19 Audio file format WAV / FLAC, Keep the WAV | build | Recording |
| A20 Analyser: export selection, project hash / check, attachments, SigMF annotation, cancel | build | Recording |
| A21 Recordings browser | DUPLICATE: not built (kept: lane C's sheet, row 4819) | |
| A22 WAV header centre | no control | |

### B. Measurement, sources, sweep, networks

| Row | Class | Where / note |
|---|---|---|
| B1 Antenna factor table, cable loss table, unit | build | Measurement; the CSV is read from a typed path (1 MiB cap) |
| B2 Preamp gain, input ohm, Show | build | Measurement |
| B3 dBuV/m read-out | build | Measurement |
| B4 Nine signal measurements | build | Measurement |
| B5 Pseudo-Doppler bearing | build | Measurement |
| B6 Bit view | build (eight operations and the three file checks) | Measurement |
| B7 GPU spectrum CPU / Auto / GPU | build | Measurement; closes lane K's not-built row 4 |
| B8 Eight cursors | DUPLICATE: not built (dropped by the engine; kept: lane K row 5234) | |
| B9 Keep frames with CRC errors, frame log | build | Measurement |
| B10 Limit lines with alarm | build (the alarm only) | Measurement; K keeps the drawing |
| B11 Sweep device: tinySA, tinySA Ultra, HackRF; open, close | build | Sweep |
| B12 RF Explorer (fourth kind) and its help text | build | Sweep |
| B13 Heat map, occupancy | build | Sweep |
| B14 Sweep read, click-to-tune | build (the frequency at the position is shown; the tune key is not: no host call tunes from it, hand-off) | Sweep |
| B15 Network I/Q out | build | Outputs and tracks |
| B16 Follow a master, lock, keep distance, stop | build | Outputs and tracks (lane K's Matrix sheet is a table of rows; these are rows beside it) |
| B17 SBS output | build | Outputs and tracks |
| B18 Track store | build | Outputs and tracks (not on lane J's map page: that page is lane J's) |
| B19 Coverage grid | build | Outputs and tracks |
| B20 Network and vendor radios in the Source list | build (a section of its own: "Network radios") | the Source list of radio setup is not this lane's; the keys were never pressed in a test (they connect) |
| B21 SDR-IQ and RF Explorer radio-setup texts | build (as notes) | |
| B22 SDR-IQ status line (the source's `lastError()`) | skipped: no fact names it; engine hand-off | |
| B23 RF Explorer status sentences | build (the sweep page shows `last_error`) | Sweep |

### C. Needed libraries

| Row | Class | Note |
|---|---|---|
| C1 The list (name, why, files, steps, licence, "FoxSDR does not include or supply it.") | build | Needed libraries |
| C2 Check again (one row, every row) | build | |
| C3 Choose the file... | build as a path field and "Check this file" (a file dialog may not be raised) | the two copy-in rows only |
| C4 Open the download page | NOT built: hand-off (no host call; every row is `fetch allowed: no`) | |
| C5 Open that folder | NOT built: hand-off | |
| C6 FTDI D2XX text block and status line | build (it is the `ftdi-d2xx` row) | |
| C7 Cannot-check rows | build | |
| C8 Header-only `working` shown as "Found (not test-loaded)" | build | |

### D. System, accessibility, web, ham, plugin host

| Row | Class | Note |
|---|---|---|
| D1 Keep the PC awake while recording | build | System |
| D2 Look for radios again after the PC wakes | build | System |
| D3 Reduced effects, read-out CPU / Memory / GPU | DUPLICATE: not built (kept: lane K rows 5217, 5218) | |
| D4 Back up on close, Back up now, Export, Import, Reset, Sync folder, Cancel the pending change | DUPLICATE: not built (kept: lane K rows 5200 to 5212; two pending mechanisms: owner decision 13) | |
| D5 What the last start did | build | System |
| D6 First-run pages | DUPLICATE: not built (kept: lane K's wizard); radio selection is lane K's not-built item 6 | |
| D7 Order in the Plugin store | build (in System: the store page is the extend bank's) | |
| D8 Helper-plugin badge and pane, consent tick | skipped: the store page's rows are built from the extend bank's model, which has no helper fields: hand-off | |
| D9 /metrics page, allowed web origins, cross-origin sign-in | build (the last one as a note: only "off" is accepted) | Web |
| D10 Contest header, Log a QSO, remove, clear, exports | build | Ham tools |
| D11 Follow radio info (DXLab Commander, N1MM+) | build | Ham tools |
| D12 Answer CAT commands | build | Ham tools |
| D13 Units, number format, digits, colour set | DUPLICATE for number format and colour set (kept: lane K); UNITS skipped: `access.units` is read by nothing, a control would do nothing | |
| D14 Plugin recorder | nothing to draw | |

### E and F
Not reached: see "Not built" below.

## What was built, and the proof of each

Architecture: `ui/system_bank.slint` has one global `LaneL` (eleven `[KRow]` lists and `act(id, text)`), `ui/main.slint` places eleven `KPanel`s after lane K's five (logical rows 15 to 25 of the SYSTEM bank, from `system-extra-base + 5`), `main.cpp` has one construction block (a submit lambda that can carry a long text, the answer hook for refusals, the tick, `state ll.*`). `lanel_logic.{hpp,cpp}` is pure (typed fields, the JSON text of each operation, result readers, the needed-libraries panel); `lanel_publish.{hpp,cpp}` builds the rows from the facts the engine names (about twice a second and at once after an action), keeps only drafts of what the person typed and has not sent, and turns each engine answer into a sentence under the section. Draft fields that become a setting are dropped once the engine has taken them, so the row then shows the engine's fact. The sections: Tuning and memories, Scanning, Recording and files, Measurement, Sweep, Network radios, Outputs and tracks, Needed libraries, System, Web, Ham tools.

| Group | Proof (real result lines, hidden desktop, real engine and its test generator) |
|---|---|
| Tuning and memories (A1 to A6, A14 to A16) | `lanel-tuning: input script: 62 steps, 62 ran, 0 failed`: channel plan read back as `fact.channel.1.plan == marine-vhf-itu` and label 16; favourite saved, renamed, removed (`favourite.count` 1, then 0); band memory `bandmem.on` 1 then not 1; the engine's "no bookmark with that id" shown; a bookmark squelch of -300 refused on the screen with the allowed range |
| Scanning (A7, A7c, A8, A9) | `lanel-scan: 44 steps, 44 ran, 0 failed`: a malformed range refused before sending, a good plan set (`fact.scan.plan` holds 144000000), trigger on/off read back, a missing station file fails on the worker (`fact.job.state == failed`) |
| Recording and files (A17 to A20) | `lanel-record: 46 steps, 46 ran, 0 failed`: daily folder style and UTC from `fact.record.daily`; the lock is read back on and off and its default untouched; FLAC / WAV; Cancel dead with no job; a bad export box refused |
| Measurement (B1 to B7, B9, B10) | `lanel-measure: 36 steps, 36 ran, 0 failed`: field strength off until ticked and the read refused in the engine's words without a table; the occupied-bandwidth measurement of the generator answers and its fact holds `valid`; GPU spectrum Auto then CPU read back (GPU itself is not pressed: it makes a Direct3D device); the limit alarm cannot be enabled with no mask |
| Sweep and Network radios (B11 to B14, B20, B21, B23) | `lanel-sweep: 37 steps, 37 ran, 0 failed`: no port is opened and no radio added; a port outside COM1 to COM256 and a fractional HackRF start are refused before sending; Close with nothing open: "Nothing changed"; heat map refused by the engine without a device ("frequency range unusable") |
| Outputs and tracks (B15 to B19) | `lanel-outputs: 39 steps, 39 ran, 0 failed`: radio 2 follows radio 1 (`matrix.rows` shows follows 1, locked true / false); I/Q out and SBS out start and stop on loopback; track store and coverage grid publish nothing until ticked; the grid is refused without the person's own position |
| Needed libraries (C1 to C8) | `lanel-libs: 24 steps, 24 ran, 0 failed`: nine rows; four rows say "Cannot be checked from here"; no row reads "Working"; "Check this file" is dead until a path is typed and a missing file reads "Not found" |
| System, Web, Ham tools (D1, D2, D5, D7, D9 to D12) | `lanel-system: 63 steps, 63 ran, 0 failed`: keep-awake and resume switches read back from `fact.wirec.*`; store order; metrics switch; a bad web origin refused with the engine's words; a QSO added, removed, added and cleared only on the second press; CAT emulation not running with port 4532 |
| Pure rules | `test_slint_lanel_host: 180 checks, 0 failed` |

## Break-it checks

| Mutation | Red |
|---|---|
| a header-only "working" read as "Found and working" | `test_slint_lanel_host: 180 checks, 3 failed` |
| the coverage grid no longer needs the person's position | `test_slint_lanel_host: 180 checks, 3 failed` |

Both restored, rebuilt, `180 checks, 0 failed`. NOT break-it-checked: the window halves (the publisher's wiring and `.slint`); the drive scripts read each effect back from the engine's own facts, which a broken wiring would change.

## New strings

564 new English keys (every `tr("...")` literal of `lanel_logic.cpp` and `lanel_publish.cpp`; no new `@tr` in `.slint`) in all 33 catalogues; the translations are MACHINE-MADE (eleven Sonnet agents, three languages each) and UNCHECKED by a speaker. `python3 tools/i18n_keys.py`: 3597 keys, 0 empty, 0 missing, 0 stale per catalogue; `test_i18n` passes. Translators' notes: nb "Soapbox" is literal and one nb sentence has "Et" for "En"; the Russian catalogue translates "ms" and "s". The engine's own sentences (refusals, `last_error`, the check sentences of the needed-library table, the technical result names of a measurement) are the engine's English text and are shown as given.

## Not built, with the reason

* A10, A11, A13, A21, B8, D3, D4, D6 and the K-duplicate part of D13: DUPLICATE, kept as the hand-off says (see the step-0 table). A7b (priority list, lock-outs, mute) is lane C's.
* A11 average / smoothing / comb and A13 saved trace mode: a second control beside lane K's would be a choice between two mechanisms (owner decision).
* A12 waterfall pause / scroll back / history: no control, because the screen cannot show held lines (HOST hand-off below).
* D8 helper-plugin badge, pane and consent tick: the store page's model has no helper fields (SCREEN hand-off).
* D13 units: nothing reads `access.units`; a control would do nothing (K 1).
* Section E (serial-a's screen half, about twenty operations) and section F (units, right-to-left, counter sentence, first-run radio selection, record-window start / stop, Matrix cell grid, gradient pointer editing): not reached. The counter's `accessible-description` needs an accessible role on the counter in `digits.slint`, which is a decision for that file's owner.

## ENGINE HAND-OFF

* SDR-IQ status line (B22): the source's `lastError()` is not in any fact; a fact `radio.<n>.source.error` would let the radio-setup text show it.
* `tracks.state` and `coverage.state` vanish when disabled (the screen shows an empty state, which is correct, but a fact with `enabled:false` would say it).
* `SWEEP_CONFIG` before a device is open is refused ("frequency range unusable"); the hand-off text said it works while closed.
* Frame log (B9): there is no fact that reads back the keep-CRC-errors setting, so the two keys are keys, not a tick.
* Sweep read: a host call that tunes the selected radio to `click_hz` would make click-to-tune a key.
* `CAT port` 4532 is accepted as a setting and refused only when emulation is enabled (the hand-off says "port not 4532").

## HOST-CALL HAND-OFF

* "Open the download page" (C4) and "Open that folder" (C5) for the needed libraries: no host call exists and every row is `fetch allowed: no`; the registry address of a row is shown as text.
* A file chooser for the library file, the markers, packs, tables and masks (a dialog may not be raised by a lane): the fields take a typed full path.
* Waterfall history (A12): `WireA::waterfallCopy` is same-process; the waterfall compositor (lane B's) would read the held lines and `fact.wf.<n>.*`.
* Band memory's spectrum limits and zoom: the screen must send `BAND_MEMORY_NOTE` on a band change and apply `fact.bandmem.apply`.
* Key bindings for the six key actions (A16): the key-bindings page would map chords to `KEY_ACTION`; they are keys here.

## Decisions I made for the owner to confirm

1. The eleven sections sit after lane K's five in the SYSTEM bank (not on the Tuning, Scanner, Bookmarks, Map and Radio-setup pages the hand-off names: those pages belong to other lanes and need `.slint` work); moving a section is a change of one `KPanel` line.
2. Record lock (default OFF) is drawn as a tick that shows and sets the engine's state: no default changed.
3. The two copy-in library rows take a typed path ("Check this file") instead of "Choose the file...".
4. The needed-library key is labelled "Check again: <library name>" so a screen reader hears which row it belongs to.
5. "Clear the log" of the QSO log asks for a second press (it erases the person's own log).
6. Field labels were made unique (for example "Bookmark squelch", "Sweep start (MHz)") so a drive script and a screen reader can tell them apart.

## UNVERIFIED

* No real device: no sweep device, network radio, FTDI or WiNRADiO library, CAT logger or DXLab / N1MM+ was used. The keys that connect (Network radios, Open the sweep device, GPU spectrum "GPU") were never pressed.
* Keys whose result needs data the generator does not give: bit view beyond Keep and List, attachments, SigMF annotation, Cabrillo / ADIF export, marker / pack import, contest header.
* A real pointer and a screen reader were not used (scripted clicks only); every translation is unchecked.
* Frame cost of the rows (about two refreshes a second, a few dozen `get_setting` calls) was not measured; the clip sweep was not re-run.

## Gates (tip at the commit before this record; Release, `D:/foxsdr-build/mr-dsp`, hidden desktop `FoxSDRTestsEM`, `FOXSDR_FRAME_CAP_HZ=60`, a shared machine)

* `ctest -L "headless|slint" -I a,b -j4 --timeout 300` in three slices, 1123 tests: `99% tests passed, 3 tests failed out of 400` (test_airband_data, test_band_plan, test_converter_call_sites), `99% tests passed, 2 tests failed out of 400` (test_page_resize_id, test_png_decode), `100% tests passed out of 323`. The five reds are the known false failures; none is mine.
* GUI gate, `-L gui -j3`: `99% tests passed, 2 tests failed out of 340`, `Total Test time (real) = 2573.83 sec`. The two reds: `test_diagnostics` (the known stale build stamp) and `test_health_app` (the known intermittent). All eight `slint_drive_lanel-*` scripts and `gui_desktop_guard` passed in the gate.
* Ledger: `ledger: 2364 rows, 708 asserted, 115 driven, 1541 not drivable, 0 unclassified`.
* Not re-run: the clip sweep and the frame-cost comparison.
* Real `%APPDATA%/foxsdr/config.json` sha256 at the start and at the end: `c21ce98dc30e50327f7666f7ee29047c0983d130ca3f21716d0c0e20e04603c3` (unchanged; every run used a scratch profile). Nothing pushed, nothing merged, no outside connection, no download.
