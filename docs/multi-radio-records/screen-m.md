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

## What was built, where it was put, and the proof

Architecture: `ui/system_bank.slint` has a global `LaneM` (ten `[KRow]` lists and `act(id, text)`); `ui/main.slint` appends a `KPanel` to the body of the section the hand-off names (SIGNAL: Patch 0, Source 1, Radio 2, Audio filters 3, Recorder 6; TOOLS: Airband 4, Export 5; SYSTEM: Diagnostics 6, lane L's Sweep and Needed libraries sections). `lanem_logic.{hpp,cpp}` is pure (typed fields, the JSON or FOXAIRCH1 text of each operation, the fact readers); `lanem_publish.{hpp,cpp}` builds the rows from the engine's facts (twice a second and at once after an action), keeps only unsent drafts, and turns each answer into a sentence under the section. Only the Source panel is conditional (drawn while the selected radio plays a file).

| Rows | Where | Proof (real lines, hidden desktop `FoxSDRTestsSM`, the real engine and its test generator) |
|---|---|---|
| 1 transport | SIGNAL, Source | `lanem-file: input script: 53 steps, 53 ran, 0 failed, 0 not reached` (speed 2, 0.25, 1; policy block skip and back; pause, step refused at 0, step forward done, resume; all read back from `fact.playback.1.*`; nothing drawn before a file plays) |
| 2 RIT, 3 CW surround, 4 container, 5 patch edit, 20 symbol rate | SIGNAL, Radio / Audio filters / Recorder / Patch | `lanem-signal: input script: 98 steps, 98 ran, 0 failed, 0 not reached` (clarifier steps +100, +10, -100, clear; an offset over the limit refused on the screen; limits 500 then 9999; CW surround on, centre 800, invert, read pitch; WAV, RF64, SigMF read back; the engine refuses part 99999 in its own words; empty paste is "Nothing changed") |
| 7 airband channel, 8 level store | TOOLS, Airband / Export | `lanem-tools: input script: 59 steps, 59 ran, 0 failed, 0 not reached` (the model's own sentences refuse a high-pass above the low-pass and a stream with no port; a good set read back as `FOXAIRCH1\|118100000\|10000\|0\|0\|-600\|`; load fills the fields; clear one, clear all asks twice; level store OFF until ticked, 70 MiB refused, 8 MiB read back, off again) |
| 11 to 15, 19 | SYSTEM, Sweep / Diagnostics / Needed libraries | `lanem-system: input script: 53 steps, 53 ran, 0 failed, 0 not reached` (every field refused before sending; a REAL stitched sweep of the test generator ran and `fact.sweep.history` read `"lines":1` (778 points); a relative history path refused, a file that is not there fails in the engine's words, clear asks twice; no open request exists so nothing is drawn) |
| 6 four new parts | the Patch page's parts bin | `test_slint_host_patch`: eleven keys, labels, the two "not run yet" tooltips, four nodes of their own kinds placed, captions MIXER / IF FILTER / GENERATOR / SCOPE, the `TransmitRefused` sentence |
| 18 choices | plugin settings form (`forms.slint`) | `test_slint_forms: 350 checks, 0 failed` (section 12b); `test_slint_host_kit: 131 checks, 0 failed` (`parseChoices`, limits of 32) |
| 21 credit | the spectrum's mark tooltip | `test_slint_lanem_host` (`withCredit`); main.cpp reads `fact.plugin.marker.credit.<n>` |
| 9, 10 | lane L's Tuning and Sweep sections | marker / pack operations say "started"; heat-map tooltip corrected; lane L's own drive scripts still pass (gate below) |
| pure rules | | `test_slint_lanem_host: 126 checks, 0 failed` |

## Break-it checks

| Mutation | Red |
|---|---|
| step of 0 accepted; history path not required to be full; a "shell" open request accepted; the airband model's validate skipped | `test_slint_lanem_host: 126 checks, 6 failed`; restored, `0 failed` |
| the RIT step's sign flipped; the I/Q container always 0 | `lanem-signal: ... 98 ran, 5 failed`; restored, `0 failed` |
| the choice key commits its label instead of its value | `test_slint_forms: 350 checks, 2 failed`; restored, `0 failed` |

NOT break-it-checked: the Patch-section keys (only the engine's answer text is read), the sweep and airband panels' wiring beyond the facts they read back.

## Frame cost
Nothing is drawn every frame: the rows are rebuilt about twice a second (about 60 `get_setting` reads over ten sections) and at once after an action; an unchanged section keeps its model. Not measured; the clip sweep was not re-run. UNVERIFIED.

Folding: the Radio and Airband sections are open at every start, so the clarifier controls and the airband channel editor are folded behind one key each ("More clarifier controls", "Edit this channel's own settings"); the symbol rate stays visible.

## New English strings
225 new keys (every `tr("...")` of `lanem_logic.cpp`, `lanem_publish.cpp`, the patch captions and refusal) in all 33 catalogues, MACHINE-MADE and UNCHECKED by a speaker; `tools/i18n_keys.py`: 3823 keys, 0 empty, 0 missing, 0 stale per catalogue. Translators' notes: some agents kept decimal commas in ranges (bg, ca, cs, sv, tr, uk); the Vietnamese file leaves "clarifier", "CW surround", "Notch", "Varispeed" in English; Lithuanian and Latvian leave "Varispeed"; the bare key "of" is "/" in Turkish. The engine's own sentences (refusals, `problemText` of the airband model) are shown as the engine gives them.

## Rows not built, with the reason
* 16 (tracks and coverage shown as off): DUPLICATE, lane L's Outputs and tracks already reads `enabled:false` (lane L's drive scripts assert it).
* 17: no control.
* Patch chords Ctrl+Z / Y / C / V and a bypass key: the window's keys come from the saved key table (`key_dispatch`), which is the key-bindings page's; needs table rows (HAND-OFF).
* Settings text of the new patch parts (IF filter `low=;high=;notch=`, Mixer `gain=;mute=`): edited only through the patch draft graph in the inspector, which is the patch page's; not drawn (HAND-OFF).

## ENGINE / SCREEN HAND-OFF
* The patch canvas's own click-selection is never sent to the engine (`PATCH_SELECT`); the Patch section selects by part number. A canvas that sent `PATCH_SELECT` on each selection change would let Copy / Bypass work on what is highlighted. File: `patch_logic.cpp` / `patch_publish.cpp`.
* Key-table rows for patch undo, redo, copy, paste and bypass.
* Inspector forms for the IF filter and Mixer settings text.
* A save and a load of the sweep history need a typed full path (no file dialog may be raised); a "Save as" picker is a screen piece.

## Decisions made for the owner to confirm
1. No Sweep page exists in the window, so the stitched sweep and the history are a second panel in lane L's SYSTEM Sweep section, kept beside the device sweep.
2. The Diagnostics panel is read only; the GPU mode stays lane L's single control (a second one would be two mechanisms).
3. Option lists of a settings field are drawn as one small key per option (the stored one lit), not a pop-up: `forms.slint` has no pop-up component.
4. "Clear every channel's settings" and "Clear the history" ask for a second press.
5. The level store's size field applies when the store is turned on (engine rule), and the default size is untouched.

## UNVERIFIED
* No real device: no radio, port or sweep device opened; the stitched sweep ran on the test generator only. Saving and loading a history file, a real plugin's open request (pressing it would open a browser), a plugin that gives named choices, a plugin mark with a credit, and the Patch keys on a patch with parts were not exercised end to end.
* A real pointer and a screen reader were not used. One screenshot of the SIGNAL bank showed the Audio filters panel's last key drawn over the next section's header while the panel was still growing; not investigated (a layout animation, or a clipped height).
* Frame cost and the clip sweep not measured. All 33 translations unchecked.

## Gates (final tip, Release, `D:/foxsdr-build/mr-dsp`, hidden desktop `FoxSDRTestsSM`, `FOXSDR_FRAME_CAP_HZ=60`, a shared and slow machine)
* `ctest -L "headless|slint" -j4 --timeout 300` (1147 tests): `99% tests passed, 6 tests failed out of 1147`, `Total Test time (real) = 1516.02 sec`. Reds: test_converter_call_sites, test_airband_data, test_page_resize_id, test_band_plan, test_png_decode (the five known false failures) and test_crash_upload_free (a load flake: `100% tests passed out of 1` alone, 24.62 sec). Earlier full runs on earlier tips also showed test_webhook (flake, passed alone) and test_slint_patch (MINE, fixed: the band's part keys now follow the model) and test_diagnostics_free (known stale-stamp family, passed later).
* GUI gate `-L gui -j3` (344 tests): `99% tests passed, 1 tests failed out of 344`, `Total Test time (real) = 2617.10 sec`. All four `slint_drive_lanem-*` scripts passed, and so did `slint_drive_lanec-lockout` (which my duplicate "NFM" label had broken on an earlier tip) and `slint_drive_tools-airband`. The one red is `slint_drive_transmit-page`: `click "Transmit" -> FAILED: no control labelled "Transmit"` (31 following steps fail from it). **Not classified as not mine**: it passed in lane L's gate, and this lane made the SIGNAL bank's Radio section longer (it is open at every start) which may push the Transmit row out of the window; a scroll step added to the script did not find it either and was reverted. Not re-run on a baseline build; the owner or the merge lane should check whether the Transmit row is still reachable at the default window height (a row is the rail's "Transmit" section, SIGNAL index 5).
* Ledger: `ledger: 2379 rows, 716 asserted, 117 driven, 1546 not drivable, 0 unclassified`.
* Not re-run: the clip sweep and the frame-cost comparison.
* Real `%APPDATA%/foxsdr/config.json` sha256 at the start and at the end: `c21ce98dc30e50327f7666f7ee29047c0983d130ca3f21716d0c0e20e04603c3` (unchanged). Nothing pushed, nothing merged, no outside connection, no download, FOXSDR_ALLOW_* never set.
