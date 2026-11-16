# Window gaps, lane 4: spectrum and waterfall, maps, satellites, tools and the 3D views (2026-11-14)

Branch `feat/window-gaps-4`, cut from `07e717e6` in worktree `D:/foxsdr-wt/en-sysmodels` (it was clean; its old branch `feat/wire-c` is merged), build folder `D:/foxsdr-build/en-sysmodels` (Slint ON, Release, configured fresh for this lane), scratch `D:/foxsdr-build/wg4-scratch/`. Nothing pushed.
Real `%APPDATA%/foxsdr/config.json` sha256 at the start: `c21ce98dc30e50327f7666f7ee29047c0983d130ca3f21716d0c0e20e04603c3`.
The audit file `D:/foxsdr-build/prep/not-built-audit.md` appeared while the lane was running; its verdicts were used as hints and every row was checked in the code first.

## Row table

Legend: BUILT = built here and proven as stated; ALREADY BUILT = found in the code with file:line; REPLACED ON PURPOSE; NEEDS DATA; NOT DONE.

| Rows | State | What / proof |
|---|---|---|
| 182 reception-gap break | BUILT | `wfrows_logic.cpp` `WfRowLog` / `wfChrome` (the classic's widest-jump-wider-than-a-label-step rule, `waterfall_view.cpp` 525-545) fed by `main.cpp` paneTick (`rv.wfRows.push` for each scrolled row, with its arrival time and dB pair) and published by `publishPicture` into `wf-gap-pos / -above / -below` (the component already drew them, `waterfall.slint:526`). `test_slint_host_wfrows`; drive `wf-gap.txt`. |
| 186 coverage key | BUILT | the run of newest rows that share the newest floor and ceiling (`WfRowLog::rangeRows`) -> "APPLIES TO TOP n LINES" in `key-coverage`. `test_slint_host_wfrows`. |
| 187 range-changed rail | BUILT | the same run's boundary as `wf-rail-pos` with the label "RANGE CHANGED" (`main.slint` now passes `rail-label`, which the component had and nothing passed). `test_slint_host_wfrows`. |
| 212, 264, 265, 266, 267 Airband on the plots | BUILT | the engine publishes it all (`fact.airband.*` and `AIRBAND_CHANNELS`, nothing needs data): `tools_logic` `AirbandModel::marks` (the live list's on-air rows), `airmarks_logic.cpp` (a mark is 3 px at least and only when the channel's centre is in view, the classic's `airbandMarkExtent`), the block sentence and "Hearing: names" captions, the tuned band, its grabs and the passband peak hidden while listening, Shift+wheel stays the zoom, and the picture does not follow a retune while listening; the waterfall tip loses its Shift+wheel line. `test_slint_host_airmarks`; drive `airband-spectrum.txt`. |
| 1698 Airband row level tooltip | BUILT | `AirbandRow.tooltip` and a `TipTimer` on the row. |
| 202, 213 4 s rest tips | BUILT | `rest-tip` and `rest-tip-shown` in `spectrum.slint` and `waterfall.slint` (a second 4 s clock; it replaces the gesture tip; a move, a press or leaving restarts it). The ledger audit called these REPLACED ON PURPOSE; the brief asked for them, so they were built. |
| 1529 Demod scope row tooltip | BUILT | the Section row's `tooltip` for bank 2 row 0, the classic's four lines. |
| 368 GPS row on the map page | BUILT | the Radar fold's own `GpsRowView` on the map bar (`MapExtras` global, set by `ToolsPublisher::publish`). The audit's "already built" was about the SYSTEM page; the Maps page had none. Drive `map-bar.txt`. |
| 372, 1719, 1720, 1721 "Stop following?" | BUILT | a drag while following sets `MapPage::followAsk`; the bar shows "The map is following X. Stop following it so the map can be moved freely?" with Stop following and Keep following. |
| 1713 trail style | ALREADY BUILT | `pages.slint` Line / Ribbon keys -> `KitEvents.page-map-trail-style` -> `kit_publish.cpp`; `d118f0f1` did build it. Still driven by `gaps-map-trail.txt`. |
| 1714 trail width | BUILT | a Width slider on the map bar over the Display body's own setting (`SystemEvents.display-trail-px`). Drive `map-bar.txt`. |
| 1731 distance footnote | BUILT | `MapModel.list-footnote`. |
| 493 aircraft silhouettes | BUILT | the classic's eleven pictures (`gui/aircraft_icon_pixels.hpp`) as `MapIcons.aircraft`, chosen by category (`mapicons_logic.cpp`, pinned over every category of the ABI by `test_slint_host_mapicons`), on an altitude halo. The registry's type code (light helicopter) is not asked on the map: a rotorcraft draws the five-blade helicopter. |
| 4902 Weather radar, Receiver map, Repeaters | NEEDS DATA | still dimmed and off; the tip of each now says exactly what is missing: a rain-radar picture or tile source; a directory of other people's receivers with positions; a repeater directory with positions and frequencies. Nothing in the tree is an owner-approved source of any of them, so nothing is fetched. |
| 1745 WHOLE WORLD, 1750 captions, 1752 position note, 1756 trail style, 1765 notes, 1766 map key | BUILT | satellite pages only: WHOLE WORLD key, "RECEIVER POSITION / LATITUDE / LONGITUDE", the position note, Line / Ribbon / Off, the coverage note and the NEXT PASSES / not-reported / hatched notes (the classic's own sentences), and the chart name and mark key on the picture. |
| 4091 the satellites deck | NOT DONE (part) | the rows above are the deck's parts the host has data for. NOT built: the Doppler read-out and GRANT RECEIVER CONTROL (see HAND-OFF). |
| 1588 knob scale numbers | BUILT | the five figures on the pointer's arc, the nearest lit. |
| 1967 basemap tiles and attribution | BUILT, UNVERIFIED with a real plugin | `radar_compositor.cpp` `paintTiles` / `radarTileZoom` from the kit's basemap client through `scope_publish.cpp`; the credit line is under the glass whenever tiles are drawn. Proven with a stand-in tile source (`test_slint_host_radar` `tileGround`: ready, pending, missing, short tile, zooms). |
| 1968 brass case | REPLACED ON PURPOSE | old look; the scale the user reads (rings, bearing ticks, knob figures) is on the tube and knob. |
| 4087 night side | BUILT | NIGHT SIDE switch on the satellites globe, saved in the 3D views' setting (inverted in `opt[2]`, so an old setting reads as on). Drive `globe-satellites.txt`. |
| 3075 EXPORT CSV | BUILT | `survey3d_logic` `surveyCsv`; written beside the other exports (no system dialog), never over a file. `test_slint_host_survey3d`; drive `survey-3d.txt`. |
| 3021 marker menu on the 3D surface | NOT DONE | needs the pointer unprojected onto the surface mesh (3042's `Scene3DWell.picked` gives the click; the frequency under it is the missing half). `scene3d.hpp` has `project` but no inverse; the cheap honest route is to project the surface's grid vertices and take the nearest to the pointer. Estimated half a day with its test. |
| 3042 click an aircraft in the 3D airspace | BUILT, click UNVERIFIED live | `Radar3dResult::picks` (each marker projected by the scene), `pickRadar3d` (nearest marker within 16 px), `Scene3DWell.picked` (a press that did not drag), `View3D.radar-picked`, `ScopePublisher::selectContact` (what a HOME row does). `test_slint_host_radar3d` `testPick`; the scripted input cannot aim at a marker, so the click itself was not driven. |
| 3054 to 3058, 3071 to 3074, 3076, 3094 to 3096, 4010, 4011, 4027, 4047 to 4049, 4088 to 4090 | NEEDS DATA | see HAND-OFF: each row's exact value and who must publish it. |
| U2 empty map says why | BUILT | the Maps row's note is now the classic's census sentence (`trackSourceAbsenceNote` over `trackCensus`): stopped by you, refused, did not start, or none fitted. |
| U3 zoom tips, MAP NOW | BUILT | the zoom keys carry the classic's four tips; the armed Set RX here key reads "CLICK THE MAP NOW". |
| U4 key ladders | BUILT | "ORBITAL ALTITUDE - km" (satellite markers take their orbit band) and "ALTITUDE - kft" with the grey "no altitude" row, highest at the top. The "MAP OVERLAYS" group is the deck's switch block, which the page's own bar already has as tick boxes. |
| U5 dial tips | BUILT | the squelch dial says "Airband is listening: this sets the Airband squelch" and, while listening, reads and sets the monitor's squelch (over -110 to 0); the gain dial says why no radio is open, with the saved radio's error when there is one. |

## New engine commands
None.

## SHARED FILE EDITS
`ui/main.slint` (properties, the instances' inputs, the export and import lists, two tooltips), `main.cpp` (paneTick, publishPicture, the squelch handler, the survey export hand-over, the drive read-backs), `ui/pages.slint`, `ui/map.slint`, `ui/scope_modes.slint`, `ui/scene3d_views.slint`, `ui/tools_bank.slint`, `ui/spectrum.slint`, `ui/waterfall.slint`, `tools_logic`, `tools_publish.cpp`, `system_publish.cpp` (one `MapExtras` write), `kit_publish.cpp`, `kit_logic.hpp`, `globe_*`, `view3d_publish.*`, `scope_publish.cpp`, `radar_compositor.*`, `mapj_logic.cpp` (three tips), `radio_view.hpp`, `CMakeLists.txt` (source list). All additions; nothing reordered or renamed. `patch_page.slint` and lanes 1 to 3's files were not touched.

## New strings (for the 34-language pass)
RANGE CHANGED, APPLIES TO and TOP %d LINES, "Right-click to change the range and display", "Shift + wheel: slide the tuned band", the Demod scope tooltip, "The map is following %s.", "Stop following it so the map can be moved freely?", "Keep following", "Width", "WHOLE WORLD", "Off", "Back out until the whole planet is in the window.", the satellites notes (all the classic's, except "Coverage measures distances from the receiver, so it needs a position first. Set one with Set RX here." which replaces "on the left"), "Draw targets only: no trails, ground tracks or footprints.", "NIGHT SIDE", "Shades the half of the Earth the Sun is not lighting", "EXPORT CSV", "Save the channel table as a CSV file", "There is no survey to save.", "ORBITAL ALTITUDE - km", "ALTITUDE - kft", "no altitude", the three blocked-layer tips (Rain radar ... "What is missing: ...") and the zoom-key tips. Most exist in the classic's catalogues already; the new English ones are the Width / WHOLE-less ones and the three tips.

## HAND-OFF (what a plugin or the engine must publish)
Checked first against `src/core/plugin_abi.h` (the new `symbol_rate` call is the USER's setting handed to a plugin, never a measurement, so it cannot feed a readout):
* 3054 EVM, 3055 SNR, 3056 SYMBOL RATE, 3057 LOCK, 3058 COLOUR BY Symbol: a decoder plugin must publish per tap, as typed fields: `evm_percent`, `snr_db`, `symbol_rate_baud`, `locked` (bool) and the symbol decision stream (one I/Q point per symbol). `CASCADE_INSTRUMENT_FLAG_LOCK` exists for an instrument but is the instrument's, not a tap's.
* 3071 to 3074, 3076 (survey): a time-binned occupancy by hour of day, the per-bin samples (or a percentile floor), the merge width and the sweep band: a plugin panel or typed fields `occupancy_by_hour[24]`, `floor_percentile_db`, `merge_width_hz`, `sweep_lo_hz`, `sweep_hi_hz`.
* 3094 landing, 3095 scrubber, 3096 launch: a radiosonde predictor must publish `launch_lat/lon/time` (a first fix on the ground), a predicted landing point with its model, and positions with times for a scrubber; nothing is invented from the track.
* 4010, 4011 (propagation): destinations with MUF and reliability per path (`dest_name, lat, lon, muf_mhz, reliability_percent`).
* 4027 FT8: the reports received about this station (`reporter, country, snr`) and a callsign-prefix table in the host.
* 4047 to 4049 GOES: the picture's `band` (1-16), `segment`, `segments_total` and the `spacecraft` id.
* 4088 to 4090 satellites: orbital elements (or an inertial path), predicted positions with times, and pass events (AOS, LOS, maximum elevation); the Doppler read-out needs the same plus the receiver's tuned frequency.
* 3021: no data needed, work not done (row above).

## Proof, with the break-it checks

Unit tests (all 0 failed): `test_slint_host_wfrows` 38 checks, `test_slint_host_airmarks` 18, `test_slint_host_mapicons` 41, `test_slint_host_survey3d` 89, `test_slint_host_radar` 650 (new `tileGround`), `test_slint_host_radar3d` 91 (new `testPick`), `test_slint_host_mapj` 712, `test_slint_host_globe` 1187, `test_slint_host_kit` 131, `test_slint_host_map` 754.
Drive scripts on the hidden desktop `FoxSDRGaps4` (all pass on the final binary): `wf-gap`, `airband-spectrum`, `map-bar` (new); `globe-satellites`, `survey-3d` (extended). `airband-spectrum` failed once on a single-tick read of the marks (a block lands and moves) before it read `wf.airmarksmax`; it passed on the runs before and after.

| Mutation (the behaviour removed) | Result | Restored |
|---|---|---|
| the gap break never drawn (`wfChrome`) | `test_slint_host_wfrows`: 38 checks, 3 failed | 0 failed |
| a mark has no 3 px floor (`airbandMarks`) | `test_slint_host_airmarks`: 18 checks, 2 failed | 0 failed |
| HEAVY draws the airliner (`aircraftIconIndex`) | `test_slint_host_mapicons`: 41 checks, 1 failed | 0 failed |
| CSV cells never quoted (`surveyCsv`) | `test_slint_host_survey3d`: 89 checks, 2 failed | 0 failed |
| a pending tile not reported (`paintTiles`) | `test_slint_host_radar`: 650 checks, 1 failed | 0 failed |
| the tuned band always shown while the monitor listens | drive `airband-spectrum` RED: `state wf.showvfo == 0 -> FAILED: it is 1` | green |
| the gap break always cleared | drive `wf-gap` RED: `state wf.gap > 0 -> FAILED: it is -1.000` | green |
| NIGHT SIDE always on | drive `globe-satellites` RED: `state globe.satellites contains night=0 -> FAILED` | green |

Not break-it checked: the rail and coverage note (the unit test holds them; a drive cannot change the display range), the tips, the map bar's new keys, the satellites notes, the knob figures, the squelch dial's Airband rule, `Scene3DWell.picked`.

## RESUMPTION (2026-11-16, after the usage-limit stop)
What was done, in order:
* Full rebuild at `5650185a`: clean. The age-figure filter edit in `main.cpp` that was never compiled compiled without a change. `wf-gap` re-run and its screenshot looked at: the break reads "3s" above and "13s" below with no overprinting of the age figures.
* The full headless gate (1151 tests) showed `test_i18n` red for real: ten new English strings missing and six stale keys in every catalogue. Fixed in `a6e5eca6` (all 33 catalogues merged, the ten strings machine-translated, `lang_assets.hpp` regenerated; `i18n_keys.py`: 3833 keys, 0 empty, 0 missing, 0 stale in each).
* Fixtures: `src/slint/tests/drive/fixtures/*.wav` are generated and git-ignored; this worktree had three of 25, so `test_engine_api_source_kinds` failed on a missing `scope.wav`. Generated with the folder's own `make_*.py` (ignored, not committed).
* The never-driven items were given tests (commits `a3d8998e`, `1e97da55`, `22c15891`): "Stop following?" (rules moved into `kit_logic` as `mapDragPans` / `mapFollowAskShown` / `mapFollowAnswer`; the drive cannot follow a target, so a logic test), the 4 s rest tips (render tests on the spectrum and waterfall specimens), the zoom-key limit tips (render test on the map specimen), the 3D click (render test on the well specimen: `picked` fires for a press that did not drag) and the squelch dial's Airband rule (new drive script `airband-dial`).
* The second headless gate showed one more real red: `test_i18n` on `surveyCsv` printing through a variable format into a fixed buffer (the project's own scanner). Fixed in `c625bfe8` (literal `"%.*f"` with the decimals as a parameter; same cell text).

## Proof added in this resumption, with the break-it checks
| Test | Break-it (behaviour removed) | Red result | After restoring |
|---|---|---|---|
| `test_slint_host_kit` (139 checks): a drag while following asks and does not pan; Keep following keeps the target; Stop following lets go | `mapDragPans` always pans | `test_slint_host_kit: 139 checks, 3 failed` | 0 failed |
| `test_slint_spectrum` (457 checks): the rest tip is not shown at 1.5 s, shown after 4 s, replaces the gesture tip, restarts on a move, ends on leaving | the 4 s threshold made unreachable | `test_slint_spectrum: 457 checks, 1 failed` | 0 failed |
| `test_slint_waterfall`: the same for the waterfall's rest tip (a wider face than the gesture tip) | not break-it checked | | passes |
| `test_slint_scene3d_well` (51 checks): a press that did not travel is a pick with its fractions; 12 px of travel is a turn, never a pick | the `!travelled` test removed | `test_slint_scene3d_well: 51 checks, 5 failed` | 0 failed |
| `test_slint_map` (233 checks): the zoom keys say the long sentence while they work and the short "as far as the map goes" at the limit (measured as the tip's width) | both tips made the long sentence | `zoom tips: plus 235 / 235`, `test_slint_map: 233 checks, 1 failed` | `zoom tips: plus 235 / 161, minus 243 / 169`, 0 failed |
| drive `airband-dial`: while LISTEN, the strip's Squelch dial sets `fact.airband.squelchDb` (0.8 gives about -22) and leaves `squelchDb` at -120; after STOP it is the receiver's | the redirect in `main.cpp`'s squelch handler disabled | `state fact.airband.squelchDb > -26 -> FAILED: it is -55.0`; `state squelchDb == -120 -> FAILED: it is -23.999992` | green |

Lesson: the first draft of `airband-dial` turned the dial to 0.5, the monitor's own default (-55 dB), so it passed in both the good and the broken build. The break-it check caught it; the script now turns the dial to 0.8.
Still not break-it checked: the rail and coverage note (unit test only), the map bar's new keys, the satellites notes, the knob figures, the waterfall rest tip.

## The multi-radio panes (the question the hand-over asked)
`ui/radio_pane.slint` has its own `SpectrumView` and `WaterfallView`, fed by `workspace_publish.cpp` from each radio's `RadioView::fig`. `main.cpp` paneTick computes `fig.wfChrome` for EVERY radio, but the Airband figures (`fig.airListening`, `airMarks`, `airBlock`, `airHearing`) are filled only `if (selectedRadio)`, and `publishPicture(selected())` is the only place that hands any of them to the window. Consequence for a person with two or more radios in a multi-radio layout while the Airband monitor listens: the pane of the selected radio draws no channel marks and no block sentence and still draws the tuned band and its grabs, and no pane shows the reception-gap break or the range-changed rail (layout One's picture does). It is NOT a few lines: it needs new fields on the pane's data struct (`radio_pane.slint`), their publication with change signatures in `workspace_publish.cpp`, the pane's `SpectrumView` / `WaterfallView` inputs, the pane specimens and layout tests, and a recompile of `main.slint` and every pane test. It is row P1 of the OPEN ROWS table.

## UNVERIFIED
* No real device and no real basemap plugin: the radar tiles were proven with a stand-in tile source only; the look of the tile pictures was not seen.
* A real pointer and a screen reader were not used. The 3D airspace click was run through render tests with a synthetic pointer and the pick's unit test, not on a live picture with aircraft; the exported survey CSV was not opened in a spreadsheet.
* All new strings' translations are machine-made and not checked by a speaker.
* Frame cost not measured; the clip sweep not re-run.
* The multi-radio panes do not carry the Airband plots or the gap break (row P1).

## Gate, config hash
Final tip: the commit that holds this record (`git log -1`). The tree built cleanly (`cmake --build D:/foxsdr-build/en-sysmodels --config Release --parallel 4`, no error) at `c625bfe8`, the last code commit.
* Headless, `ctest -L "headless|slint" -j3 --timeout 300` (1151 tests) at `22c15891`, hidden desktop `FoxSDRGaps4B`: `99% tests passed, 9 tests failed out of 1151`, `Total Test time (real) = 2293.74 sec`. Reds: `test_airband_data`, `test_band_plan`, `test_converter_call_sites`, `test_page_resize_id`, `test_png_decode` (the five known false failures); `test_i18n` (MINE: the `surveyCsv` format, fixed in `c625bfe8`, then `Passed 23.72 sec`); `test_pass_control` (load flake: failed with the GUI gate running beside it, `Passed 49.85 sec` alone later); `test_pipeline_spectrum` (load flake, `Passed 9.97 sec` alone); `test_health_app_stepped` (counts a `slow.startup.250ms` when the machine is busy: failed alone twice under load, `Passed 35.43 sec` on a quiet machine). The first headless run at `5650185a` also showed `test_engine_api_source_kinds` (missing generated fixture `scope.wav`, then `Passed 18.66 sec`), `test_engine_api_decode_radio` (Timeout under load, `Passed 36.72 sec` alone), `test_engine_api_transmit` and `test_engine_control_thread` (load, both passed alone).
* GUI, `ctest -L gui -j3 --timeout 600` (348 tests) at `22c15891`: `99% tests passed, 2 tests failed out of 348`, `Total Test time (real) = 2767.00 sec`. Reds: `test_diagnostics` (the known stale-stamp family on an uncommitted tree: the `surveyCsv` fix was uncommitted then; `Passed 3.71 sec` on the committed tree); `slint_drive_system-look` (`Passed 21.08 sec` alone; not reproduced, cause not found, load suspected). All the lane's drives (`wf-gap`, `airband-spectrum`, `map-bar`, `globe-satellites`, `survey-3d`, `airband-dial`) passed in the gate.
* Not re-run in full after `c625bfe8`: only `test_i18n`, `test_slint_host_survey3d`, `test_diagnostics`, `test_diagnostics_free`, `test_pass_control`, `test_health_app`, `test_health_app_stepped` and `slint_drive_system-look` were re-run (the change is one helper in `survey3d_logic.cpp`).
* Ledger: `ledger: 2379 rows, 725 asserted, 119 driven, 1535 not drivable, 0 unclassified`.
* Real `%APPDATA%/foxsdr/config.json` sha256 re-read at the end: `c21ce98dc30e50327f7666f7ee29047c0983d130ca3f21716d0c0e20e04603c3` (unchanged). Nothing pushed; no radio opened; no download; `FOXSDR_ALLOW_*` never set.
* Test expectations changed: none (no existing check was edited; new checks were added).

## OPEN ROWS FOR THE NEXT LANE
| Id | What is missing | Kind of lane |
|---|---|---|
| P1 | The multi-radio panes (`radio_pane.slint`) do not carry the Airband channel marks, block sentence, tuned-band hiding, the waterfall gap break or the range rail; needs new pane data fields, `workspace_publish.cpp` publication, pane view inputs, pane specimens and tests (section above) | screen |
| 3021 | Right-click marker menu on the 3D surface: needs the pointer unprojected onto the surface mesh (`scene3d.hpp` has `project` but no inverse; project the grid vertices and take the nearest to the pointer; the well's `picked` already gives the click); about half a day with its test | screen |
| 4091 (rest) | The satellites deck's Doppler read-out and GRANT RECEIVER CONTROL; the Doppler read-out needs orbital elements (or an inertial path), predicted positions with times, pass events (AOS, LOS, maximum elevation) and the receiver's tuned frequency | plugins (data), then screen |
| 3054, 3055, 3056, 3057, 3058 | EVM, SNR, SYMBOL RATE, LOCK, COLOUR BY Symbol: a decoder plugin must publish per tap `evm_percent`, `snr_db`, `symbol_rate_baud`, `locked` (bool) and the symbol decision stream (one I/Q point per symbol). The ABI `symbol_rate` call is the user's setting handed to a plugin, never a measurement | plugins |
| 3071, 3072, 3073, 3074, 3076 | Survey views: a plugin panel or typed fields `occupancy_by_hour[24]`, `floor_percentile_db`, `merge_width_hz`, `sweep_lo_hz`, `sweep_hi_hz` (time-binned occupancy by hour of day, per-bin samples or a percentile floor, the merge width, the sweep band) | plugins |
| 3094, 3095, 3096 | Radiosonde views: a predictor must publish `launch_lat/lon/time` (a first fix on the ground), a predicted landing point with its model, and positions with times for a scrubber; nothing is invented from the track | plugins |
| 4010, 4011 | Propagation views: per destination `dest_name, lat, lon, muf_mhz, reliability_percent` | plugins |
| 4027 | FT8 view: the reports received about this station (`reporter, country, snr`) and a callsign-prefix table in the host | plugins, then engine or screen (the prefix table) |
| 4047, 4048, 4049 | GOES view: the picture's `band` (1-16), `segment`, `segments_total` and the `spacecraft` id | plugins |
| 4088, 4089, 4090 | Satellites views: orbital elements (or an inertial path), predicted positions with times, and pass events (AOS, LOS, maximum elevation) | plugins |
| 4902 | The Weather radar, Receiver map and Repeaters layers stay dimmed: a rain-radar picture or tile source; a directory of other people's receivers with their positions; a repeater directory with positions and frequencies. No owner-approved source exists in the tree, so nothing is fetched | owner decision (sources), then Worker/site or plugins |
| 1967 | Radar basemap tiles proven only with a stand-in tile source: the look of real tiles and the credit line with a real basemap plugin | mouse drive |
| 3042 | The 3D airspace click on a live picture with a real pointer (render tests and the pick unit test hold the logic) | mouse drive |
| T1 | Hover tips drawn by a real pointer: the 4 s rest tips, the zoom-key tips, the Airband row and Demod scope row tooltips (held by render tests with a synthetic pointer) | mouse drive |
| T2 | "Stop following?" with a real drag on a followed target (the rules are held by `test_slint_host_kit`; the card and the drag are not driven) | mouse drive |
| T3 | Translations of the ten new strings, the satellites notes, the three blocked-layer tips and the zoom-key tips: machine-made, not read by a speaker | owner decision (translation pass) |
| T4 | Frame cost and the clip sweep not measured for the new overlays (Airband marks, gap break, silhouettes) | screen |
| T5 | `test_health_app_stepped` counts `slow.startup.250ms` when the machine is busy; `slint_drive_system-look` failed once under the GUI gate and passed alone | engine (test robustness) |
