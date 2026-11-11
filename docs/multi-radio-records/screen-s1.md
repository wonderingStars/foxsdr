# Screen lane S1: the S6 leftovers, one-click layouts, the waterfall as a file with a persistence trace, decluttered plugin-mark names (2026-11-11)

Branch `feat/screen-s1`, cut from `mr/screen-s6` at 9d907b68 (the S6 record's last commit). Worktree `C:\Users\steve\foxsdr-wt\mr-screen1`, build `D:/foxsdr-build/mr-screen1`. Nothing is pushed or merged.
Records read first: `screen-s6.md` (its sweep, drive-suite, ledger and hidden-desktop tooling), the design `D:/foxsdr-build/prep/pre-release-features-design.md` (sections 1, 6 screen part, 12, 13, 18, 20).

| Commit | What |
|---|---|
| 51ccca8d | item 1: the status bar's keys and chips, the One layout's dB labels, the `expect ... right <= "n"` position reads, six drive scripts, ledger rows 4500 to 4502 |
| a2545cf9 | items 2 to 4, the logic: layouts_prefs, export_logic, mark_layout, the persistence trace in host_logic, their four host tests, view3d_prefs, the settings text's size limit |
| 378c609e | items 2 to 4, the window: the Layouts key and sheet, the TOOLS bank's Export section, the Persistence key and menu items, the "+N" tabs and the names' tooltips, 45 strings in 33 catalogues, drive scripts, ledger rows 4503 to 4522 |
| 7099260a | `test_slint_waterfall` holds the line and the new names (225 checks) |
| (this record) | docs |

The three screen items share hunks of `main.cpp` and `main.slint`, so they are committed as their logic first and their window second, not one commit each; each item's break-it table is below.

## What the user can do now

- Item 1: with several radios open the status bar's three keys (Add a radio, Request a feature, Report a bug / dislike) are always whole and inside the window; the chips give way. The One layout's dB labels are laid out over the plot the window really gives the spectrum, so at 800 x 560 they no longer run into one smear.
- Item 2: a "Layouts" key in the title bar opens a menu: the two stock layouts (Airband, HF DX), the user's own, "Save the current layout..." and "Manage layouts...". The sheet lists every layout with Apply, and Rename and Delete on the user's own, and has a name field, "Include tuning" and "Save the current layout". A layout is saved in the settings (interface half, the `view3d` text) and an older settings file loads.
- Item 3: the TOOLS bank has an Export section: a path box, the data length (1, 5, 15, 60 minutes), the data format (CSV or FSW, the FoxSDR spectrum file) and two keys, "Save waterfall picture" (a PNG with a frequency scale, a time scale and a caption) and "Save waterfall data". The spectrum's Trace choices gain Persistence (the trace of the last seconds as a decaying density; Fast, Medium, Slow in the right-click Trace menu).
- Item 4: the names of plugin marks on the spectrum and the waterfall no longer draw over each other: they are stacked into up to three rows, what fits no row is counted into a "+N" tab whose click lists the names and whose pick tunes there, and a name's tooltip says what it is, its frequency and the plugin it came from.

## Item 1. The S6 leftovers

THE STATUS BAR. With several radios open the three keys were pushed out by chips that cannot shrink below their lamp, key and value. `main.slint` now splits the row: the keys are a group sized to their words (a `Rectangle` whose min, preferred and max width are the group's `preferred-width`) and the chips live in a `chips-area` that takes what the keys leave and clips there; a chip whose cell does not fit (`self.x + self.width <= chips-area.width`) is not drawn and the first chip always is. Shown by `status-bar-keys` (English, 1280 x 720, four radios), `status-bar-keys-fr`, `status-bar-keys-800` (one radio and four) and `status-bar-keys-800-ja`, which read the keys' right edge against the window's width through the new position reads (`expect "<label>" left|top|right|bottom|width|height <cmp> "<n>"`, parsed and tested in `test_slint_host_input`). Break-it: with the old bar put back, `status-bar-keys-fr` reads 1364 against 1280, `status-bar-keys-800` 820 against 800 and `status-bar-keys-800-ja` 845 against 800 (red); the English 1280 script stays green, English fits there.

THE ONE LAYOUT'S dB LABELS. `main.slint` reports the spectrum's real plot size (`one-plot-h`, `one-plot-w`, from SpectrumView's `plot-h` / `plot-w`) and `main.cpp` lays the labels and the frequency ticks over it instead of over a fixed 980 x 330. `plot-db-labels-800` and `plot-db-labels-1280` read the plot's size, the number of labelled dB ticks and the smallest gap between two of them (state `figure.plotheight`, `figure.dbticks`, `figure.dbpitch`: the first run found a gap of 10.7 px because the unlabelled ticks were counted, the read counts labelled ones now).

THE CLIP SWEEP, the eight multi-radio views (`r4-panes` and the one-radio `r4-one`, both skins, 1280 x 720 and 800 x 560, 34 languages, 272 runs, 0 not finished):

| | before (S6 tip) | after (51ccca8d) |
|---|---|---|
| unexplained items, all eight views | 871 | 738 |
| the one-radio baseline (`r4-one`, 1280 + 800) | 433 | 369 |
| items that exist with several radios and not with one | 63 | 4 (`waterfall.slint:515` / `:544` at 800 x 560) |
| `r4-panes` 1280 x 720 / 800 x 560 | 160 / 278 | 135 / 234 |

THE PRE-EXISTING ONE-RADIO BASELINE (433, now 369). The status-bar fix took 64 of them away with one radio as well. What is left is the receiver strip (`keys.slint:58` / `:285`), the tab row and spectrum toolbar (`spectrum.slint:426` / `:433`), the title bar (`main.slint:397`, 68 items) and the tooltips: at 800 px the strip, the tab row and the title bar are wider than the window in 20 to 30 languages. Reducing it needs a compact strip and a compact tab row for windows under about 1000 px (the strip already has a `compact-strip` mode; the tab row and title bar have none): not a cheap change and not done.

## Item 2. One-click layouts

WHAT A LAYOUT HOLDS (`layouts_prefs.hpp`): the bank and its open sections, whether the rail is folded, the tab (RECEIVER / PATCH / SCOPE; a plugin page is not part of a layout), the split between the spectrum and the waterfall, the workspace's layout (One / Side by side / 2 x 2), Link, the selected radio, the 3D waterfall, and only when saved with "Include tuning" the frequency and mode (of the selected radio). Restored in a fixed order (`stepsOf`): the bank, then its sections, the rail, the tab, the split, the workspace layout, Link, the selected radio, the 3D waterfall, the tuning last.

WHAT IT NEVER DOES. It never starts or stops a radio, a recording, a decoder, a patch or a scan: the `Actions` struct the window hands it has no such member (`test_slint_host_layouts` records every call a layout makes), and `main.cpp` builds its actions from the window's own setters. It does not leave the PATCH tab while a patch runs (leaving that tab stops the patch): that step is skipped and the sentence says so. Proven live: `layout-airband` reads `running == 1`, the same `tunedHz` and one radio before and after; `layout-multi-radio` and `layout-degrade` read every radio's `running` after.

WITH ONE RADIO the workspace layout a layout chooses (Airband is Side by side) is kept but One is shown (the S5 rule: the chosen layout comes back with the radios). WITH SEVERAL RADIOS a layout captures the workspace layout, Link and which radio is selected; it does NOT capture which radios are open, their sources, their frequencies (except the selected radio's with "Include tuning") or their pages. A layout that selects a radio that is not open leaves the selection alone and says "The radio that layout names is not open, so the selection stays" (`layout-degrade`, then the same layout with three radios selects radio 3). Applying a layout with Link on turns Link on without retuning anything (Link only follows later moves).

STOCK LAYOUTS (the design leaves their contents to the owner; these are decisions). Airband: the TOOLS bank with the Airband section open, split 40, Side by side. HF DX: the SIGNAL bank with the Source and Radio sections open, split 25 (a wide waterfall), One. Neither changes the frequency, the mode or the selected radio. (The design's HF DX mentions the DECODE bank's FT8 rows; the SIGNAL bank was chosen because a layout cannot start the FT8 decoder.)

WHERE IT IS SAVED. In the interface half's `view3d` text as the raw entry `layouts:n=<name>,b=<bank>,s=<open sections mask>,f=<rail>,v=<tab>,p=<split>,w=<workspace>,k=<link>,d=<3D>[,r=<radio>][,t=<Hz>,m=<mode>]|...` (`view3d_prefs` keeps it verbatim, so no `AppConfig` field was added, the S5 decision). Twelve of the user's own at most, names up to 24 bytes (separators and control characters become spaces, the same name in any case replaces, a built-in layout's name is refused). SHARED FILE TOUCHED: `src/core/config.cpp` now reads at most 2048 bytes of the `view3d` text (it was 1024, which twelve layouts and the workspace entry exceed; over the limit the whole text is dropped); `view3d_prefs` allows 1024 bytes for the `layouts` entry. An older settings file (no entry) loads with the two stock layouts (every other drive script runs on such a seed, and `test_slint_host_layouts` loads one); a newer file's unknown keys are skipped and every value is clamped (`layout-degrade` loads a seed with two layouts).

NOT DONE FROM THE DESIGN: the key-table rows "Layout: <name>" (the design says none by default; no row was added, so `kKeyActionCount` and its tests are unchanged); the layouts menu is reachable from the title bar key and the command palette does not list layouts.

## Item 3. The waterfall as a file, and the persistence trace

NO ENGINE CHANGE. The screen's own data is enough: a per-radio store of levels is filled where the picture is painted.
- THE STORE (`SpectrumStore`): a row every 250 ms per radio, one byte a column (dBFS in 0.6 dB steps from -150 dBFS, so -150 to +3), at most 1024 columns (the strongest bin of each group), a new segment at every change of centre, span or column count, kept for an hour: measured 14.8 MB for an hour of 1024 columns (test output). Only a radio whose pane is shown is stored (the host paints, and so stores, the shown panes: the selected radio in One, the page's radios in a multi-pane layout); a radio on another page has no rows until it is shown. The design asked 16-bit values for the data file; the file keeps the store's one byte (0.6 dB), which is what a picture or a table of a minutes-long capture needs.
- THE PICTURE: the waterfall as the window draws it (1024 x 238, the colours and the zoomed window it shows) with a margin: a caption (radio, centre, span, UTC time of the newest line), a frequency scale above and a time scale at the left, in a 5 x 7 face of the lane's own (the digits, a few signs and the capitals the words need: no font file, no translation). PNG, dependency-free (stored deflate blocks), with a text chunk `FoxSDR` holding `radio=...;centre_hz=...;span_hz=...;newest_utc=...;db_min=...;db_max=...;seconds_per_line=...`. It shows the 8 seconds the picture holds (30 lines a second); the data file is what goes back minutes.
- THE DATA: CSV (`#` header lines, `# segment,...` lines with the centre, span and column count, then `UTC,db0,db1,...` rows in dBFS to a decimal) or FSW, the FoxSDR spectrum file (little-endian, magic `FXSP`, segments of rows of bytes; `export_logic.hpp` has the layout and a reader). 1, 5, 15 or 60 minutes.
- WHERE IT GOES: the path box (typed; a relative path goes under the profile's `exports` folder next to `config.json`, a missing extension is added, quotes and spaces off the ends are taken away). There is NO native "Save as" dialog: the repo's `core::pickFile` is an open-file dialog only. A file that exists is never overwritten ("wf (2).png"), the file is written under `.part` and renamed, and the folder is made.
- OFF THE FRAME: a click copies what the file needs (the picture's pixels, or the store's rows: up to 14 MB for an hour, a memcpy) and a `core::DiskJob` writes it on a worker; one file at a time (the keys are dimmed while one is written, a second click does nothing); the result is collected once a tick.
- PROOF: `export-wf` fills radio 2 (a generator with its carrier 60 kHz above the centre) for ten seconds and saves the picture, the picture again ("wf (2).png"), the FSW and the CSV to a typed relative path; the host reads each file back (state `export.fileinfo`, `export.peakoffset`): the PNG's chunk names radio 2, the centre and `span_hz=2000000.0`, the FSW's strongest column of the newest row is 58 to 62 kHz from the centre (the column is 1953 Hz wide), the CSV has rows. `test_slint_host_export` (547 checks) writes the files to a temporary folder, decodes the PNG with the repo's own reader pixel for pixel and finds the carrier's column in the FSW and the CSV's rows.

PERSISTENCE (`TraceMode.persistence`): a float per cell of the trace picture, multiplied by the decay every frame and incremented along the trace; a cell visited on every frame settles fully lit, one visited on a third of them a third as bright (the strength key's ramp with a rising alpha); the decay a frame is `exp(-1 / (tau * frames a second))`, tau 0.3 s (Fast), 1 s (Medium), 3 s (Slow), chosen in the right-click Trace menu (the scripted input cannot open that menu: NOT DRIVABLE, the arithmetic is `test_slint_host_persistence`). Cost: 0.16 ms a frame at 1024 x 340 against 0.02 ms for the plain trace (test output). The mode is the window's, not saved with the settings (the trace mode is not saved in this window today; the classic's saved `spectrumTraceMode` has no persistence value and `core/config.cpp` is a shared file). The picture's decay needs the panes' own picture size: it follows a pane refit (the density is reallocated with the picture).

## Item 4. The names of plugin marks

FROM THE DESIGN'S FINDING 5: names were one line of text at each mark's line. `mark_layout.hpp`: names are placed left to right into up to three rows (the first row a name fits in after the name before it), what fits no row is counted into a "+N" tab (names within 30 px share a tab; the tab sits on the row under the last name row), a mark with no name or outside the window draws no name, and the mark's line is never touched. On the spectrum (`spectrum.slint`) and on the waterfall (`waterfall.slint`, new: the waterfall drew no names before) and in the panes (`RadioPaneData.mark-tabs`, `wf-mark-tabs`, `WorkspaceModel.mark-tab-clicked`). A name at the plot's right end is moved in, never cut by the window's edge (the first sweep found 136 + 204 + 136 such items). A tab's click opens a menu of the names it stands for (name and frequency) and a pick tunes there (selecting the pane's radio first); `marks-many` clicks the 89-name tab and picks one.
CREDIT / SOURCE LINE: a name's tooltip is `<name>  <frequency>` and, when the mark came from a named plugin, a second line "from <plugin>" (the list item's `detail`, which the engine fills with the module's name). The marker ABI has no credit field (a 32-byte label only), so the data source's credit a plugin owes (HamQTH asks for one) cannot travel with a mark: see ENGINE CHANGES WANTED.
THE CAP AND `markerLimit` were NOT touched (an ABI matter). The host's cap is 64 marks a plugin; the tests use made-up marks below the ABI (`FOXSDR_SLINT_TEST_MARKS=<n>`, a bounded-run seam, a fixed sequence with a third of them crowded at one place) at 4, 64 and 256. A real plugin through the ABI with 64 marks was NOT run (no fixture plugin sets that many; `fixture_group8_plugin` sets two).

## Tick cost with made-up marks (item 4)

One radio, 1600 x 900, the frame meter on (`FOXSDR_SLINT_FRAME_LOG=1`), hidden desktop, default renderer, three 30-second runs each (`drive/measure/marks-<n>.txt`, `run_marks.bat`); the marks are made up below the plugin ABI (a fixed sequence, a third crowded at one place, one in five dashed). The first measurement (marks-256, 24 dashed lines allowed) read 35.6 ms and the first one before any cap (all 51 dashed lines of 256) read 40.0 ms against 33.3 ms with no marks: the dashed lines are what costs the frame (each is a stack of small rectangles), so past 8 dashed marks the host draws the rest solid (`main.cpp`, paneTick). After that:

| made-up marks | tick interval mean / p95 | the tick's own work mean / p95 | process CPU in 30 s |
|---|---|---|---|
| 0 | 33.35 / 34.06 ms | 0.82 / 1.54 ms | 5.2 s |
| 64 | 33.56 / 34.12 ms | 1.02 / 1.78 ms | 6.4 s |
| 256 (one in five dashed, 8 drawn dashed) | 33.96 / 37.99 ms | 1.51 / 2.33 ms | 10.4 s |
| 256, none dashed | 33.48 / 34.57 ms | 1.42 / 2.29 ms | 7.8 s |

The declutter rule alone costs 0.00001 / 0.0031 / 0.0126 ms at 0 / 64 / 256 marks (`test_slint_host_marks`). THE 256 CASE IS WITHIN THE FRAME BUDGET ON THE MEAN (33.96 ms against 33.35) BUT NOT AT p95 (38 ms against 34): the machine was shared with other lanes at times (the CPU column), and these are hidden-desktop figures that include no presentation of a frame, as in S6.

## Break-it checks (each mutation applied, the named test or script run, red seen, the mutation restored)

| Mutation | Red |
|---|---|
| Layouts: the sections put back before the bank | `test_slint_host_layouts` (4 checks, `at(Step::Bank) < at(Step::Sections)`) |
| Layouts: the PATCH tab left although a patch runs | `test_slint_host_layouts` (1: the held-view case) |
| Layouts: a stock layout's name may be saved over | `test_slint_host_layouts` (6) |
| Layouts, window: a layout's sections not put back (`main.cpp`) | `layout-airband` (`layout.sections == 16`, it is 28) |
| Export: no new segment at a window change | `test_slint_host_export` (the store crashes on the mismatched row: access violation) |
| Export: no quarter-second gap between rows | `test_slint_host_export` (4) |
| Export: the picture without its frequency scale | `test_slint_host_export` (6; the first version of the test did not notice: it was strengthened) |
| Export, window: the store never filled (`main.cpp`) | `export-wf` (`export.rows >= 30`, it is 0; and no spectrum file to read back) |
| Persistence: no decay | `test_slint_host_persistence` (4) |
| Persistence: the density kept on leaving the mode | `test_slint_host_persistence` (1) |
| Persistence, window: the mode paints the plain trace (`main.cpp`) | `trace-persistence` (`trace.density > 100`, it is 0) |
| Marks: names that fit no row drawn on row 0 anyway | `test_slint_host_marks` (10; the first version of the test crashed on it and was hardened) |
| Marks, window: no declutter (`main.cpp`) | `marks-many` (`marks.named < 256`, it is 256; no tabs) |
| Item 1: the old status bar | `status-bar-keys-fr`, `status-bar-keys-800`, `status-bar-keys-800-ja` (right edges 1364, 820, 845) |
| Item 1: the fixed 980 x 330 plot (`main.cpp`) | `plot-db-labels-800` (`figure.plotheight < 330`, it is 330) |

Not break-it-checked: the `.slint` side of the marks (names' rows, tabs, tooltips: a `.slint` mutation is one 25-minute compile; the scripts read the host's figures and the tab click, the screenshots show the drawing), the Layouts sheet's keys (each is operated by `layout-save` and read back), the export keys (operated by `export-wf`), `restart` of layouts across a restart (the settings text round trip is in `test_slint_host_layouts`; no script restarts the window with a saved layout).

## Sweeps of the new views (`tools/slint_clip_sweep.py`, views `r4-layouts`, `r4-export`, `r4-marks`)

The sweep gained three view kinds, each in both skins at 1280 x 720 and 800 x 560 over the 34 languages (408 runs, 0 did not finish): `layouts` (the sheet opened by the capture seam `FOXSDR_SLINT_LAYOUTS_SHEET` over a settings seed with two layouts of the user's own, with the longest note the sheet can show), `export` (the TOOLS bank with the Export section open) and `marks` (64 made-up marks crowding the window). The persistence key is in the spectrum toolbar of every view; the `tools` view of the sweep now opens the Export section too.

| | unexplained items |
|---|---|
| first sweep, all 12 views | 2018 (of them 544 from the new names and tabs at the plot's right end: `spectrum.slint` names 136 + 204, `waterfall.slint` names 136, the spectrum's tabs 68; the sheet and the Export section: none) |
| after moving the names in (and the Export keys one to a row, the Data length label above its keys) | 1338 = 446 in each of the three kinds, the same sites in each: the pre-existing receiver strip, tab row, title bar and tooltips (the baseline of item 1). No item comes from the Layouts sheet, the Export section or the marks. |

The screenshots (the German Export section, the Finnish sheet at 800 x 560, the 256-mark crowd, the panes) were looked at; the Export section's "Data length" keys were wider than the panel in German before the label went above them.

## ENGINE CHANGES WANTED (none was made; nothing in `src/engine`, `src/dsp`, `src/core/pipeline` or the plugin ABI was touched)

1. A credit / source field on a plugin mark. `CascadeMarker` (plugin_abi.h) carries a 32-byte `label` and nothing else; `FoxListItem.detail` carries the plugin's module name. A data source that asks for a visible credit (HamQTH: "Data provided by HamQTH.com") cannot send it with a mark; the tooltip's second line is the module's name. Wanted: a short `credit` text on `CascadeMarker` (an addition at the end, API level bump) and a second text on the `FOXAPI_LIST_MARKERS` item.
2. The marker cap and a `markerLimit` member. `CASCADE_MAX_MARKERS_PER_PLUGIN` is 64 (src/core/plugin_abi.h and the plugins repository's copy); the design wants 256 and a `CASCADE_API_LIMIT` read so a plugin can ask. Not touched. The screen is ready for 256 (measured above) and the names are decluttered whatever the number.
3. A level store for every open radio. The screen stores only the shown panes' frames (the host paints only those), so the data file of a radio on another page has no rows until it is shown. An engine-side per-radio spectrum ring (the engine's own ring has four slots) would cover every open radio.
4. A saved trace mode with persistence. `AppConfig::spectrumTraceMode` (`core/config.cpp`: `traceModeName` / `traceModeFromName`) has no persistence value, so the choice is the window's and is not saved; the classic window shares that setting. A shared-file edit for the owner to decide.
5. A "Save as" dialog. `core::pickFile` is an open-file dialog only; the Export section takes a typed path. A save variant of `core/file_pick.*` would give the key a Browse.

## Deviations from the design, and decisions

1. The design's file name `src/core/spectrum_file.*` became `src/slint/export_logic.*` (nothing outside the Slint host reads or writes the file today).
2. Level bytes at 0.6 dB for both the picture and the data (the design: 16 bits for data). Stated above.
3. The picture is the waterfall as drawn (8 seconds, the zoomed window) with scales; the minutes-long history is the data file.
4. The Export section's path is a typed path with a default under the profile's `exports` folder, not a dialog.
5. HF DX opens the SIGNAL bank (a layout cannot start the FT8 decoder); Airband the TOOLS bank with Airband open, side by side.
6. No key-table rows for layouts (the design: none by default).
7. The persistence trace and its decay are not saved with the settings.
8. Dashed marks beyond the eighth are drawn solid (the frame cost found by the measurement).
9. The `layouts` settings entry needed `config.cpp`'s limit for the `view3d` text raised from 1024 to 2048 bytes (a one-line edit in a shared file; the conflict table lists config.* for the engine lanes: they should take that line).

## New strings (machine translated by the lane, not checked by a native speaker)

45 new keys in all 33 catalogues (each also in `src/core/lang_assets.hpp`, regenerated with `tools/embed-lang.py`); `tools/i18n_keys.py`: 0 missing, 0 stale in every catalogue; `test_i18n` passes. "Saved %s", "Saving..." and "from %s" were already in the catalogues and keep their translations. The keys, in the order of their use (`{}` is Slint's placeholder, `%s` / `%d` / `%.1f` the C++ ones):

- `Layouts`
- `A layout arranges the window: the bank, the sections, the split, the radio layout. It never starts or stops a radio, a recording or a decoder.`
- `{} (built in)`
- `Done`
- `Delete`
- `Apply layout {}`
- `Rename layout {}`
- `Finish renaming layout {}`
- `Delete layout {}`
- `Layout name`
- `A name for this arrangement`
- `Include tuning (the frequency and mode of the selected radio)`
- `Save the current layout`
- `Save the current layout...`
- `Manage layouts...`
- `Saved the layout %s`
- `Replaced the layout %s`
- `Type a name for the layout first`
- `That is the name of a built-in layout`
- `You can keep %d layouts of your own: delete one first`
- `Deleted the layout %s`
- `Renamed to %s`
- `Another layout has that name`
- `Applied the layout %s`
- `The patch is running, so the window stays on its tab`
- `The radio that layout names is not open, so the selection stays`
- `Show the layouts`
- `Save to`
- `Folder and file name`
- `Data length`
- `Data format`
- `{} min`
- `Data length {} minutes`
- `FoxSDR spectrum file: compact, one byte for every level`
- `Save waterfall picture`
- `Save waterfall data`
- `Radio %d at %s, span %s: %d min stored, %.1f MB`
- `This radio has shown no spectrum yet, so there is nothing to save`
- `The file could not be written to %s`
- `Type where to save first`
- `Export`
- `Persistence`
- `Persistence decay`
- `Show the {} names that did not fit`
- `Names that did not fit`

## Gates (Release, `D:/foxsdr-build/mr-screen1`)

- Build: `cmake --build D:/foxsdr-build/mr-screen1 --config Release --parallel 4`, exit 0 at 378c609e (`main.slint` compiled about 25 minutes each time, seven times in this lane; a host-side change builds in minutes). The probe (`foxsdr-slint-probe`) built the same way.
- Headless gate `ctest -L "headless|slint" -j4 --timeout 300`, 764 tests in three ranges (1-340: 734 s, 341-680: 939 s, 681-1200: 126 s). 13 failed, all named: the five known false failures of a D: build (`test_airband_data`, `test_band_plan`, `test_converter_call_sites`, `test_page_resize_id`, `test_png_decode`), `test_diagnostics_free` (the stale commit stamp of a tree with a later commit or uncommitted files), `test_slint_waterfall` (MINE: the waterfall now draws the names of plugin marks and the old checks built the marks with the new `row` field at 0; fixed in 7099260a, 225 checks, 0 failed alone) and six that failed under load and PASSED when re-run alone once (`test_engine_api_transmit`, `test_pass_control`, `test_patch_dest_async`, `test_plugin_store_window`, `test_tx_audio`, `test_usb_bounded_walk`). `test_i18n`, `test_i18n_glyphs`, `test_slint_house_rules`, `test_slint_constraints`, `test_config` and the ledger check pass.
- Window gate on the hidden desktop (`FoxSDRTestsS1`, `FOXSDR_FRAME_CAP_HZ=60`, nothing on the visible desktop), `ctest -L gui -j3 --timeout 300`: 284 tests (the 267 of S6 and the 17 new scripts), 279 passed, 5 failed, 2912 s: `slint_drive_fixture-weather-decoder`, `slint_drive_system-look` and `slint_drive_together` failed under load and passed when re-run alone once; `test_diagnostics` (the stale stamp); `test_health_app` (INTERMITTENT: 252 checks, one fails, `bindOrAbsentCount(... , 1)` of the health ledger's file at tests/test_health_app.cpp:1240; alone it passed once and failed twice in three runs; the engine-stage-4 record names it as the one failing window test of that gate too, and nothing this lane changed is in the health ledger (src/core got a size limit in config.cpp and the regenerated language assets)).
- The stamp tests refreshed: with the build refreshed at 7099260a (the tip before this record) `test_diagnostics` (43.0 s) and `test_diagnostics_free` (38.0 s) both PASSED; they compare the build's commit stamp with HEAD, so this record's commit makes them stale again until the next refresh.
- Ledger: `ledger: 2210 rows, 582 asserted, 92 driven, 1536 not drivable, 0 unclassified` (the check runs inside the slint label and alone).
- The lane's own tests alone at the tip: `test_slint_host_layouts` 96 checks, `test_slint_host_marks` 38, `test_slint_host_export` 547, `test_slint_host_persistence` 20, `test_slint_host_input` 94, `test_slint_waterfall` 225, all 0 failed; the drive scripts `status-bar-keys`, `-fr`, `-800`, `-800-ja`, `plot-db-labels-800`, `-1280`, `layout-airband`, `layout-hfdx`, `layout-save`, `layout-degrade`, `layout-multi-radio`, `export-wf`, `trace-persistence`, `marks-many`, `marks-64`, `marks-few`, `marks-panes` pass inside the window gate.
- `%APPDATA%/foxsdr/config.json` sha256 before and after: `c21ce98dc30e50327f7666f7ee29047c0983d130ca3f21716d0c0e20e04603c3` both times (every run used a scratch profile; the exports of the drive runs went to the scratch profiles' `exports` folders or to `D:/foxsdr-build/s1-work`).


## UNVERIFIED

- Real mouse and keyboard, Windows' own handling of the title bar key, and every rendering on a visible desktop: all windows ran on the hidden desktop (`FoxSDRTestsS1`) and the frame figures include no presentation of a frame.
- A real plugin setting 64 marks through the ABI (the 64 and the 256 are made up below it); a real EiBi or DX-spot plugin.
- A layout applied while a patch is running (held in `test_slint_host_layouts` by the Actions record; no live patch was run).
- Layouts across a restart of the window (the text round trip is tested; no script restarts the window with a saved layout).
- The decay choice in the right-click Trace menu and the tooltips of mark names (the scripted input has no right-click and no hover verb); the persistence look was judged from one screenshot of a static generator spectrum.
- Export from a real device (a generator was the source); an hour of eight radios (8 x 14.8 MB by arithmetic, not run).
- Native speakers: none of the 45 strings was read by one.
- The exported PNG in an external viewer: it was decoded by the repo's reader and shown by the image reader of this session (the frequency and time scales and the caption are legible, the carrier is where it should be).

