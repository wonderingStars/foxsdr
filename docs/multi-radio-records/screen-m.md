# Screen lane M: the Slint controls for what the wiring second passes built (2026-11-13)

Branch `feat/screen-m`, started from `engine/multi-radio` `d1e54f28` (clean; `git switch -c feat/screen-m`), worktree `C:/Users/steve/foxsdr-wt/mr-dsp`, build `D:/foxsdr-build/mr-dsp` (Slint ON, Release), scratch `D:/foxsdr-build/sm-scratch/`. Nothing is pushed or merged.
Real `%APPDATA%/foxsdr/config.json` sha256 at the start: `c21ce98dc30e50327f7666f7ee29047c0983d130ca3f21716d0c0e20e04603c3` (read from the brief; re-proved at the end).
Read first: `STANDARD-RULES.md`, `screen-lanes-common.md`, `screen-handoff-M.md` (the 21 rows), `screen-l.md` (the generic row panel, the pending-operation pattern), `engine-merge-4.md` sections 5 to 8 (17 open owner decisions).

## Step 0: the table of rows (reading only, before code)

Classes: `build` = a control is drawn; `DUPLICATE: not built (kept: ...)`; `skipped (why)`. Row numbers are those of `screen-handoff-M.md`. Placement follows the hand-off's own "Page or panel" column, NOT the SYSTEM bank: every row is drawn as a lane K row panel (`KPanel`) appended to the body of the section the user would look in, from a new global `LaneM` and a new publisher (`lanem_publish.cpp`, over `lanem_logic.cpp`). Where the window has no such page, the record says so.

| Row | Class | Where it is put | Note |
|---|---|---|---|
| 1 Playback speed, policy, pause, resume, step | build | SIGNAL, Source section (the radio's own section); drawn only while the selected radio plays an I/Q file (`fact.playback.<n>.speed` exists) | position read-out from the radio's state; step is dead while playing (engine DENIES, shown) |
| 2 RIT (set, step, clear, limits) | build | SIGNAL, Radio section (the tuning section) | never saved (engine); OFF/0 until the person acts |
| 3 CW surround | build | SIGNAL, Audio filters section | off by default; the placement rule is the engine's |
| 4 I/Q take container WAV / RF64 / SigMF | build | SIGNAL, Recorder section | not a duplicate of lane L's "Audio file format" (a different operation) |
| 5 Patch undo, redo, copy, paste, select, bypass | build | SIGNAL, Patch section | keys and the engine's own selection; the chord keys and the canvas selection are NOT wired (hand-off below) |
| 6 Four new patch parts (Mixer, IF filter, Generator, Scope), captions, refusal sentence | decided after reading `patch_logic.cpp` / `patch_publish.cpp` (see "What was built") | | |
| 7 Airband per-channel settings | build | TOOLS, Airband section | text built over `core/airband_channel.hpp` so the model's own sentence refuses a bad value |
| 8 Level store (on/off, size, clear, read statistics) | build | TOOLS, Export section (the data / export page) | OFF by default; the size is only sent with "on"; RELATED to row 14, not merged |
| 9 Marker / pack import and export now asynchronous | build (wording) | lane L's Tuning section | the answer "done" became "started" for these four operations; the result is the job's fact |
| 10 Heat map and occupancy work with no device open | build (wording) | lane L's Sweep section | the stale tooltip that said "once a device is open" is corrected |
| 11 Sweep this radio, 12 Stop | build | the Sweep section (SYSTEM, lane L's; no other sweep page exists in the window) | a second panel under lane L's, so the sweep controls stay in one section |
| 13 Save the history, 14 Load, Clear, read-out | build | the same Sweep section | RELATED to row 8, not merged |
| 15 GPU spectrum read-out gains the block's counters | build (read-out) | SYSTEM, Diagnostics section | the mode control stays lane L's (Measurement); drawing a second control for `GPU_FFT_MODE` would be two mechanisms |
| 16 Track store / coverage grid shown as off | DUPLICATE: not built (kept: lane L's Outputs and tracks); only re-proved by a drive script | | lane L already reads `enabled:false` |
| 17 (no control) | no control | | |
| 18 Drop-down for a settings field with named choices | build | the plugin settings form (`forms.slint` `SettingCell`) | |
| 19 Open the download page / folder button | build | Needed libraries section (lane L's), second panel | nothing opens until pressed |
| 20 Symbol rate (baud) | build | SIGNAL, Radio section | key `sig.symbol_rate`; 0 = none |
| 21 Credit of a plugin mark | build | the mark's tooltip on the spectrum | |
