# Multi-radio screen, stage S3: the RadioWorkspace (2026-11-11)

Branch `mr/screen-s3`, cut from the merge of S0 at 78667639. Design: `docs/multi-radio-screen-design.md` (read-only copy in the engine-stage4 worktree), section 4 row S3 and sections 3.1 to 3.6 and 5. Four commits: the host logic and the test seam (A), the Slint component and its placement (B), the host wiring, drive scripts and ledger (C), the frame-time tool and this record (D). The strings go with the code that uses them (A and B).

THE LIVE MULTI-RADIO PATH IS UNVERIFIED. Engine stage 4 (radios 2 to 8 live, RADIO_ADD / RADIO_REMOVE) is another lane's and is not in this tree: the engine here has ONE radio and refuses a command aimed at radio 2. Everything below that shows two to eight radios was shown with made-up radios (the test seam) or made-up data (component tests); nothing was run against a real second radio.

## What the user can and cannot do now

With ONE radio the window is exactly today's: no bar, no new element, the receiver view has the geometry it had (the receiver rectangle gets explicit geometry equal to its implicit one when `ws-bar-h` is 0). All 154 existing drive scripts ran unchanged.

With more than one radio open (only possible here through the seam), in the Slint window, in both skins:
- a 30 px bar over the receiver view: "Layout", the keys One / Side by side / 2 x 2, and, when a radio lives beyond the first page, one page key per page ("Radios 1-4", "Radios 5-8"; in Side by side "Radios 1-2" ... "Radios 7-8"). The page shown is lit.
- One is today's receiver view of the selected radio. Side by side and 2 x 2 replace the spectrum and waterfall with a grid of `RadioPane`s (S2), 6 px gaps, never more than four at a time; radios 5 to 8 are the second page of four.
- the selected pane has the 2 px outline in its radio's colour; a click on a pane's tag, header or readout row selects it (RADIO_FOCUS; the window follows only when the engine says yes), and so does a right-click (its menu opens at the pointer). STOP/START and the spectrum and waterfall gestures (click to tune, band drag, edge drag, wheel zoom, shift-wheel step, pan, double-click) act on THAT pane's radio and leave the selection where it is.
- each pane's splitter is kept for its radio across page changes.
- below 1280 x 720 the window shows One and the bar says so; the chosen layout returns when the window is wide again.

Not reachable, and why: Focus + 3 (the brief asked for one, two and four); keyboard selection, Link, Start together, saved layout, the strip additions, the Audio mix block, status chips and the Add-a-radio card (S4 and S5; an empty cell draws only a quiet well named "Radio n"); a way to ADD a radio (stage 4); gain figure and decoder chip of a pane whose radio is not the selected one (see "Seams").

## The files

| File | What |
|---|---|
| `src/slint/workspace_logic.hpp/.cpp` | `Workspace` (radios from the engine's facts, layout, pages, shown layout and the small-window fallback, cells, selecting, the slots to tick, fitting pictures), `PaneGestures`, `paneTexts`, `panePlot` / `panePictures`, the page and notice sentences |
| `src/slint/workspace_testseam.hpp/.cpp` | TEST ONLY: `SeamRadios`, made-up radios 2 to N for a bounded run |
| `src/slint/workspace_publish.hpp/.cpp` | `WorkspacePublisher`: the model of `ui/workspace.slint` from the host's state |
| `src/slint/ui/workspace.slint` | `WorkspaceCell`, `WorkspaceModel` (global), `WorkspaceBar`, `RadioWorkspace` |
| `src/slint/ui/radio_pane.slint`, `spectrum.slint`, `theme.slint`, `main.slint` | the pane's new drop order, lamp tips, split callback, tag ink; the compact axis unit; `Skin.radio-ink`; the placement |
| `src/slint/radio_view.hpp` | `RadioView::resizePictures`, `paneSigs` |
| `src/slint/main.cpp` | the tick split, selection, gestures, the seam, the `workspace.*` read-backs |
| `tests/test_slint_workspace_host.cpp`, `test_slint_radio_workspace.cpp`, `test_slint_radio_pane.cpp` (changed), specimen `radio_workspace_specimen.slint` | tests |
| `tests/drive/workspace-{layouts,pages,select,small,edge,empty,skins}.txt`, `coverage-workspace.tsv` | drive scripts and ledger classes |
| `tests/measure_radio_workspace.cpp`, `ui/specimens/radio_workspace_s3.slint` | the frame-time tool |

## Decisions and deviations

1. THE ONE-RADIO RULE is held by construction: the bar and the grid exist only while `WorkspaceModel.radio-count > 1`; with one radio the tick takes today's path (selected radio painted at today's 1024 x 340 and 1024 x 238 into the window's own properties); layout One with several radios still shows the receiver view (under the bar).
2. WHERE THE KEYS ARE. The design puts the layout keys in the toolbar. The tab row's right group is already clipped at narrow widths, so the keys are in their own 30 px bar over the receiver view, shown only with more than one radio (design 3.4: the strip additions are "shown only when more than one radio exists"). With several radios and layout One the bar is the one added line.
3. PAGES FOLLOW THE LAYOUT'S PANE COUNT (deviation). The brief says pages of four, radios 5 to 8 the second page. In 2 x 2 that is exactly so (1-4, 5-8). In Side by side a page of four would show two panes and hide radios 3 and 4 from that layout, so there a page is a pair (1-2, 3-4, 5-6, 7-8): every radio stays reachable and never more than four panes are on screen. Changing layout keeps the page that holds the selected radio.
4. THE PAGE KEYS APPEAR WHEN A RADIO LIVES BEYOND THE FIRST PAGE (deviation from "when fact.radio.max is above four"): with three radios and max 8 a second page would be empty. When S4's Add-a-radio card exists an empty page may be worth offering; `Workspace::pageCount` is the one place to change.
5. PICTURES FITTED, per the lane's decision 1: in a multi-pane layout each radio's pictures are `panePictures(cellW, cellH, split)` - the pane's plot width (pane width less the 44 px gutter), the trace the spectrum well less ribbon (8 + 15) and axis (18), the waterfall the well less its foot (32). At 1440 x 900 a pane is 563 x 353 and the pictures are 519 x 89 and 519 x 127; at 1600 x 900, 643 x 353 and 599 x 89 / 599 x 127. The history restarts only when the size moved by more than 8 px (a window drag does not restart it on every step); a splitter drag does refit (the split is a parameter), so dragging the splitter restarts that radio's waterfall history in 8 px steps. Layout One gets today's size back (`restorePictures`). The pane's plot size for axes and gestures is `rv.plot`. The ribbon allowance assumes the shipped `ribbon-size` 1; with the ribbon off the pictures are a little short and Slint scales them.
6. READOUT ROW ORDER, per the lane's decision 2: PEAK, BIAS, RATE, VOL, SQL, the decoder chip, GAIN are dropped in that order, SPAN stays. A figure is shown only when it and every figure that outlasts it fit left of SPAN; the shown ones are placed from the left in the hand-over's order (GAIN, SQL, VOL, RATE, BIAS, chip), so no hole is left. The S2 test did not fail with the new order (it never pinned the chip against BIAS); it gained the assertion that pane 5 (420 px) keeps its chip (it did not in S2: S2's record says "At 420 px the decoder chip and PEAK are gone"). The test records the figure masks at 22 widths, with and without a decoder: with one, 640 down to 220 px, `3f 2f 27 23 21 01` (all; no BIAS; no RATE; no VOL; chip and GAIN; GAIN alone); without one `1f 0f 07 03 01`.
7. THE R-TAG'S INK, per the lane's decision 3, measured by `test_slint_radio_workspace` (it prints the ratios and fails under 4.5): Workbench, ink `#15171b` (its own ground) on R1 `#4da3ff` 6.83 : 1, R2 `#f5a524` 8.79, R3 `#3ddc84` 10.06, R4 `#b48cff` 6.95. Night Watch, ink `#070303` on R1 `#ff6a4c` 7.25, R2 `#ffd2c6` 14.94, R3 `#d9442b` 4.70, R4 `#ff9a80` 9.95. DEVIATION: Night Watch's R3 was `#b83a26` (S2); against `#070303` that is 3.59 : 1, and no ink that is red-led (the skin's rule, `test_slint_theme`) can reach 4.5 on it (the brightest red-led ink is below 4.5 against it, a white one would break the skin). It moved to `#d9442b`, the darkest red clearing 4.5 : 1 with the skin's ground; it is still distinct from R1 (`#ff6a4c`, 1.54 : 1 between them, and the tag's number tells radios apart in that skin anyway).
8. THE SPLITTER AND "PER-PANE STATE". Panes are made per radio slot while their radio is on the page; a pane's splitter share lives in `WorkspaceModel.splits[radio - 1]` (the pane reports `split-changed`), so it is back when the page comes back (component test: dragged in pane 1, page 5-8 and back, same position; pane 2 unmoved).
9. THE CELL SIZE IS WRITTEN BY THE WORKSPACE (`WorkspaceModel.cell-w` / `cell-h`) for the host to fit pictures to. First version used `changed` only; in the real window (the grid is made by an `if`) the first value arrives with no change and the host saw 0 x 0 and never fitted (found by the window gate: `workspace.pic.1` stayed 1024x340); `init` now writes it too. The component test holds the size (a break-it: removing both turns 4 checks red).
10. RIGHT-CLICK POSITIONS. The pane's menu callbacks now give the position relative to the PANE (they gave it relative to the well); the workspace adds the pane's absolute position so the host opens its menu at the pointer in window coordinates (the receiver view's own menu code has the offsets 360 and 172 hard-coded; a pane needs none). Held by the component test (both menus, two panes).
11. THE COMPACT AXIS UNIT (found in the real window, not by a test): in a pane's narrow plot the last frequency tick sat under the right-hand corner and overlapped the unit word ("101.0MHz2"). In compact mode the unit sits in the left gutter. The receiver view is unchanged. No pixel test holds this one (looked at in the shots); the test sheet's pictures are in the build folder.
12. SELECTION IS HOST-LOCAL. The engine has no read-back of the session's focus; `RadioSet::selected` is set when RADIO_FOCUS is accepted. The strip, panels and banks read `read_state`, which the engine design says follows the session focus; that part is the engine's and is unverified here (in the seam the host puts the selected made-up radio's state in the strip's `state`).
13. STRINGS. See "Strings".
14. THE SEAM IS BOUNDED-RUN ONLY (FOXSDR_SLINT_TEST_RADIOS, read only when `bounded.bounded()`; the drive script header key is `testradios=N`, because `radios=` already means the user's SoapySDR modules - a first version of this lane's runner shadowed it and was corrected before it was committed). No control in the window reaches it.

## Seams left for engine stage 4 (and S4/S5)

- `EngineReads` in `main.cpp` (`engineReads`): the facts and RADIO_FOCUS; with the seam they are answered from the made-up radios, otherwise from `get_setting` and `submit`. When stage 4 is merged nothing here changes: the facts name the radios it opens and RADIO_FOCUS moves the focus.
- `readSpectrum` / the per-pane state read: `read_radio_spectrum(radio)` and `read_radio_state(radio)` behind `FOXAPI_HAS`, for every radio; in this tree they exist and answer for radio 1 only. The seam answers for radios 2 to N.
- NOT READ PER RADIO, because the engine gives no per-radio list: a non-selected pane's GAIN shows AUTO (device AGC) or "--" (the strip's gain figure comes from the focused radio's gain list), and its decoder chip is empty (the seam draws "APRS" on radio 2 only so the chip is seen). `fact.radio.<n>.plugins` is not read. S4/S5 or the engine's plugin stage fills these.
- An empty cell: `EmptySlot` in `ui/workspace.slint` is the place for S4's "Add a radio" card (RADIO_ADD); `Workspace::cells` marks which cells are empty; `Workspace::pageCount` decides whether an empty page is offered.
- Layout and page are the host's to set (`Workspace::setLayout` / `setPage`); S5's keys (Ctrl+1 to Ctrl+8 select, page keys) call `Workspace::select` / `setPage`. NOTE FOR S5: Ctrl+PageUp and Ctrl+PageDown are already taken by "tune one screen width" (`src/slint/system_logic.cpp:641`).
- The strip's MASTER lamps, STOP Rn, the radio selector and the Audio mix are S4's: they will want `WorkspaceModel.selected-radio` and the same `engineReads.focus`.
- `PaneGestures` is the one place a pane's gesture becomes a command; S6's `state radio <n> <field>` can read `RadioSet::at(n - 1).state`; the host already exposes `state workspace.tuned.<n>` and `workspace.running.<n>`.

## Strings

Taken from `D:/foxsdr-build/prep/new-strings-34-languages.json`, added to the 33 catalogues ONLY where the code uses them (23 keys, 759 lines), `src/core/lang_assets.hpp` regenerated with `tools/embed-lang.py`:
- C++ (`workspace_logic.cpp`, with `%d` and `formatText`): Radio %d, Select Radio %d, Stop Radio %d, Start Radio %d, Radio %d spectrum, Radio %d waterfall, Radio %d is running, A decoder is running on Radio %d, Radio %d is muted, Radio %d has failed, Radios %d-%d, Show radios %d to %d, Several radios need a window of at least %d x %d. Showing one radio.
- Slint (`@tr`): Layout, One, Side by side, Show one radio, as before, Show two radios side by side, Show four radios in a 2 x 2 grid, SQL, VOL, RATE, BIAS. The S2 stop-gaps are replaced: the pane's captions are now the short SQL / VOL / RATE / BIAS and the label sentences ("Stop Radio 2" ...) are the host's `tr()` sentences.
- English only, needs translation: "Resize Radio n" (the splitter's accessible name; not in the prepared set) and the key caption "2 x 2" (not translated by design, as `new-strings.md` says). PEAK keeps the existing catalogue word PEAK IN PASSBAND (a short PEAK is not in the set). Not built, so not added: Focus + 3 and its tip, Link tuning, Start together, STOP ALL ... (S4/S5).
`test_i18n` passes.

## Tests, and what they show

- `test_slint_workspace_host` (new, 138 checks, made-up engine): the count from the engine (gaps in the numbers, eight, none, count-only, a count above the limit), the shown layout at exactly 1280 x 720 and one pixel under either side, cells and pages in both multi-pane layouts, the page following the selection through layout changes, RADIO_FOCUS on select (and the engine's refusals leaving the selection alone, NO_CHANGE accepted), only the visible radios ticked, pictures fitted (smaller than today's, a few pixels no restart, restored in One), the pane's words, gestures aimed at the right radio (`aimAt` then gives the target), the seam (a tone of its own per radio; commands change only their radio; only in a bounded run).
- `test_slint_radio_workspace` (new, 500 checks, Slint element tree and real pointer events): both skins; 2 x 2 page one and two (R5 to R8 wear the colours of R1 to R4); Side by side and its four pages; the gaps; the selection outline moving; an empty slot; the layout keys lit and clicked; the page keys lit and clicked, absent with one page; the notice; every pane control named with its radio, unique across four panes; a click, wheel and right-click in the right pane; the splitter kept across a page change; the drop order at 22 widths x 2; PEAK only beside a full row; contrast of the tag's ink.
- `test_slint_radio_pane` (S2, one assertion added): the decoder chip outlives BIAS, RATE, VOL and SQL.
- Drive scripts, run on the hidden desktop with the seam (all pass): `workspace-layouts` (One, 2 x 2, Side by side, One again: cells, picture sizes fitted and restored, only the radios on screen ticked), `workspace-pages` (eight radios: page one reads only 1 to 4, page two 5 to 8, selecting radio 6 moves the strip to it, the pair pages), `workspace-select` (a click on a tag selects; a press in pane 4's spectrum retunes radio 4 only; in pane 3's waterfall radio 3 only; START in pane 3 starts radio 3 only; STOP in pane 1 stops the engine's own radio), `workspace-small` (1000 x 640: One shown, notice, only radio 1 read), `workspace-edge` (1280 x 720: 2 x 2 shown), `workspace-empty` (three radios in 2 x 2: the fourth cell empty and not read), `workspace-skins` (shots in both skins).
- Ledger: rows 4200-4211 DEFERRED -> PRESENT and classified (4 asserted, 6 driven, 2 not drivable: the lamps share their words across panes, and the splitter needs a drag verb), rows 4212-4216 added; `ledger: 2153 rows, 529 asserted, 92 driven, 1532 not drivable, 0 unclassified`.

### Break-it checks

Each change made, the test run, the change undone (full detail in the commit messages of A and B):
- host logic (`test_slint_workspace_host`, 138 checks): every radio ticked 7 failed; the selection following a refused focus 4; no small-window fallback 3; pictures never fitted 4; a click aimed at radio 1 2; the count not the engine's 45; the seam read outside a bounded run 1; the page not following the selection 3. Restored: 0 failed.
- Slint (`test_slint_radio_workspace` / `test_slint_radio_pane`): the decoder chip dropped before BIAS 1 / 1; Night R3 back to `#b83a26` (contrast) 1 / 0; the splitter not kept 2 / 0; the selected pane not marked 4 / 0; the pane size not reported 4 / 0; the right-click relative to the pane 1 / 0. Restored: both green.
- ONE DID NOT TURN RED: removing "PEAK only when every other figure is shown" left every test green. PEAK is 163 px wide, so whenever it fits every figure to its left already fits: it goes first by geometry; the rule in the .slint is belt and braces and no test can separate it.
- Not break-it-checked: the compact axis unit (looked at only), the empty-slot well's look, the drive scripts' own steps (a script is its own test; the fallback and tick-counts they read are the logic the host test breaks).

## Frame time

`measure_radio_workspace` (hidden desktop, 600 frames after 120 warm-up, three runs a cell, the harness's software platform, one frame = the host's picture work for four panes then one `render()` of the whole window; four fake spectra of 4096 bins as in S0; pictures fitted by `Workspace::fitPictures`). It is not the live host: `paneTick`'s engine reads and the publisher's signature compares are not repeated and the chrome around the workspace is not drawn. Total CPU sampled before each run: 2 to 13 percent (the other lane's builds ran at times).

| window | pane | pictures | mean ms (3 runs) | p95 ms (3 runs) | host work | render call |
|---|---|---|---|---|---|---|
| 1440 x 900 | 563 x 353 | 519 x 89 and 519 x 127 | 3.95 / 4.15 / 4.24 | 4.28 / 4.37 / 4.47 | 0.31 to 0.36 | 3.64 to 3.89 |
| 1600 x 900 | 643 x 353 | 599 x 89 and 599 x 127 | 4.70 / 4.67 / 4.60 | 5.19 / 5.08 / 4.85 | 0.39 to 0.40 | 4.22 to 4.30 |

Against S0 (bare panes, same renderer): today's picture size 4.7 and 4.9 ms (p95 5.1 and 5.3), fitted pictures 3.9 and 4.2 ms. The real workspace with the bar costs 0.1 to 0.5 ms more than the bare fitted panes and is within S0's range for today's pictures. One machine, discrete GPU, no integrated-GPU or slow-CPU run; the renderer the product ships is not this one (S0's open question about the skia default's 24 ms cadence is not answered here).

## Screenshots (build folder `D:/foxsdr-build/mr-screen1-shots-s3/`; looked at)

- The real window on the hidden desktop with the seam, `window/`: `skin-workbench-four.png`, `skin-workbench-two.png`, `skin-workbench-page2.png`, `skin-night-four.png`, `skin-night-two.png`, `skin-night-page2.png`, `workspace-four.png`, `workspace-two.png`, `workspace-page2.png`, `workspace-selected-2.png`, `workspace-empty-slot.png`, `workspace-small.png`, `workspace-edge.png`.
- The component sheet, both skins: `workspace-{workbench,night}-four-page1.png`, `-four-page2.png`, `-two.png`, `-empty-slots.png`; the frame-time tool's window: `measure-1440-1.png`.
- Only TWO skins exist (Workbench, Night Watch): the brief's "at least three" is not possible; the digit faces (Nixie, Neon, Plain) are not skins.

What I see: four panes in a 2 x 2 grid with 6 px gaps, each with R-tag in its colour, device, frequency, mode chip, four lamps, STOP/START, the readout row (GAIN SQL VOL RATE BIAS, the APRS chip on radio 2, PEAK, SPAN), the compact spectrum with its axis, the splitter and the waterfall with age strip and foot; the real radio's pane draws the generator's spectrum and the VFO band, the made-up radios one tone each at different places. Selected pane: blue 2 px outline (Workbench), red (Night Watch). The bar sits over the grid: "LAYOUT One | Side by side | 2 x 2" and "Radios 1-4 | Radios 5-8" (or four pair keys), the lit key in the accent. Radio 3 (stopped) shows START and "STOPPED" in its foot. Second page: R5 to R8 wear blue, amber, green, violet (red, pale, dark orange, salmon in Night Watch). The empty cell is a dark well with "Radio 4" in dim mono. At 1000 x 640 One is shown with the amber notice beside the layout keys. Anything wrong: (1) before the fix, "MHz" overlapped the last frequency tick at each pane's right corner (fixed, compact unit in the left gutter); (2) in the toolbar row of a 1000 px window the right group is cut at "Auto" - that is today's behaviour at that width, not this lane's; (3) in Night Watch the decoder chip's tone is the muted red, less obvious than Workbench's green; (4) on page two a band-plan name near 108 MHz in pane R8's top-right is clipped to "TOB Yo..." (the ribbon's own elision of a narrow segment's name, existing behaviour); (5) the "GAIN --" of a pane without a gain list is the seam above, not a bug.

## Gates

All on the tip of commit C (the sources of commit D are the tool and this record only); `D:/foxsdr-build/mr-screen1`, Release.

- Build: `cmake --build D:/foxsdr-build/mr-screen1 --config Release --parallel`, no error (the generated code of `main.slint` is about 470 000 lines and takes 15 to 25 minutes to compile; a compiler limit, "blocks nested too deeply", stopped a first test specimen with ten probe panes, so the specimen has one probe whose width the test sets).
- Headless gate, `ctest -C Release -L "headless|slint" -j4 --timeout 300`: 749 tests, 744 passed, 5 failed, and all five are the known false failures of a build folder outside the repository (`test_airband_data`, `test_band_plan`, `test_converter_call_sites`, `test_page_resize_id`, `test_png_decode`: they look for the repository from their working directory); run as plain executables from the worktree root they pass (115, 5174, 5, 22 and 49 checks, 0 failed). No load flake appeared. 1248 s.
- Window gate, hidden desktop `FoxSDRTestsS3`, `FOXSDR_FRAME_CAP_HZ=60`, `ctest -C Release -L gui -j3 --timeout 300` (a `.bat` started with `run_on_hidden_desktop.ps1`): `100% tests passed out of 234` (the 225 of S2, the two before them in the tree, and the seven new `slint_drive_workspace-*`). 1484 s. No window was shown on the visible desktop.
- Ledger: `ledger: 2153 rows, 529 asserted, 92 driven, 1532 not drivable, 0 unclassified`.
- New and changed tests alone: `test_slint_workspace_host` 138 checks 0 failed, `test_slint_radio_workspace` 500 checks 0 failed, `test_slint_radio_pane` 177 checks 0 failed, `test_slint_theme` 86 checks 0 failed, `test_i18n` passed.
- `%APPDATA%/foxsdr/config.json` sha256 before and after: `c21ce98dc30e50327f7666f7ee29047c0983d130ca3f21716d0c0e20e04603c3`.

## Not covered / unverified

- Everything with a second REAL radio: the facts of a live engine, RADIO_FOCUS moving the session's focus and the strip following it, per-radio state and spectrum reads, the load of four live pipelines, audio, any command aimed at radio 2 to 8 (the engine here refuses them). The seam proves the host's logic, not the engine's.
- The live frame time with real frames and the product's renderer (S0's open question).
- The real-mouse drive over the panes (the release gate) and the elide/clip sweeps at 1280 x 720 in the longest languages (S6): the pane's header and readout row were checked at 420 and 556 px in English only; the new strings were not rendered in the CJK or German faces.
- Keyboard use of the keys (focus, Tab order) and their accessible hints beyond the labels.
- The strings are the prepared machine translations (`"machine": true`); nobody read them.
- A splitter drag restarts that radio's waterfall history in 8 px steps (decision 5); a smoother refit is not built.
- The seam's gestures: a made-up radio ignores any command but the five it knows (SET_FREQUENCY, VFO_TO_ABSOLUTE, SET_VFO_OFFSET, STEP_TUNE, RUN); the bandwidth drag on a made-up radio changes nothing.
