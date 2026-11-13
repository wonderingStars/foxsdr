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
Not started when this table was written (E and F only if time remains; see the end of the record).
