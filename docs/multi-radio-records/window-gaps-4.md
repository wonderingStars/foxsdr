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
| 3021 marker menu on the 3D surface | NOT DONE | needs the pointer unprojected onto the surface mesh. `scene3d.hpp` has `project` but no inverse; the cheap honest route is to project the surface's grid vertices and take the nearest to the pointer. Estimated half a day with its test. |
| 3042 click an aircraft in the 3D airspace | NOT DONE | needs the aircraft labels to be clickable and the label-to-contact mapping kept by `view3d_publish`; the HOME list still selects. |
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
* 3021, 3042: no data needed, work not done (rows above).

## Gate, UNVERIFIED and the config hash
(filled in at the end of the lane)
