# Screen lane J: the map rows (2026-11-12)

Branch `feat/screen-j` in `D:/foxsdr-wt/en-screen-j`, started from `090e7039` (fast-forward; it already contains `feat/net-map-models` 428831c0). Build folder `D:/foxsdr-build/en-screen-j`. Scratch prefix `sj-`. Nothing pushed.

## 1. Step 0: the rows (what each takes; checked against the tree at 090e7039)

Checked with `git log` of the base and a grep of `src/` for the model headers, `~path`, `digipaths`, `heat`, `polar`, `replay`: **no model of this lane is used by any file outside `src/core/` and its own tests yet**, so no row is "already built" on the screen. The map pane (`ui/map.slint`, `map_compositor.*`, `kit_publish.cpp` fillMap, `MapToolbar` in `ui/pages.slint`) draws targets, the graticule, range rings, session coverage wedges and plugin polylines (`HostPath`, closed or dashed) only.

| Row | Class | What the screen half is | Engine or ABI hand-off |
|---|---|---|---|
| Q7a Antenna calculators | screen only | a page (kind 13) over `core/antenna_calc.hpp` | none |
| M8 Az/el tracker (Sun, Moon, five radio sources) | screen only | a page (kind 12): table + polar plot over `core/sky_position.hpp` and the receiver position the host holds | none. The Stellarium link is BLOCKED (third-party program) and is not built |
| Polar sky plot of a pass | screen only against a test double | the same polar plot, fed az/el samples of a pass | the satellites plugin publishes az/el (not a host track today): hand-off written |
| Unit choice on the map labels | screen only | a per-page choice (Auto, Metric, Imperial, Aviation, Nautical) over `core/access_units.hpp` (it IS in the tree at 090e7039, no stand-in needed), applied to the scale bar, target labels and tips, list notes | the `access` lane's setting key is not wired: the choice is held per map page; hand-off names the key to read |
| Aircraft map filter controls | screen only for altitude, hide-on-ground, callsign; test double for type / operator / military | a filter row + a predicate in `mapj_logic` | the ADS-B plugin lane `adsbout` owns the full predicate and the fields (type, operator, military): not in `CascadeTrack` (only the 5-bit category is) |
| M1a Map layers (weather radar, airspaces, receiver maps, repeaters) | screen only for the switches, legend, credit line and the closed paths already received; sources needing a key or terms stay OFF | layer switches with a "needs a source" state, legend rows, credit line | each source's terms and key: owner reading; hand-off lists the four |
| M1b APRS paths, locator-square marker style | screen only | thin faint polylines for ids ending `~path` (kind STATION), behind `aprs.digipaths`; locator squares | the APRS plugin hands the path over (`aprs::stationPath`); `aprs.digipaths` setting is read through the plugin settings the host already shows |
| Maidenhead overlay with worked / heard shading | screen only against a test double | a grid overlay (field, square, subsquare by zoom) with labels, and shaded cells | the ft8-decoder plugin's hand-off of worked / heard sets: hand-off written |
| M2 ADS-B heat map and 24 h range outline | screen only | a heat layer and an outline layer from `AdsbCoverage`, fed from the tracks the page receives | persistence (owner decision 5) and the feed in `core/plugin_api.cpp`: the screen feeds from the page's own tracks, session only |
| M3 Timeline replay | screen only | transport: play, pause, speed, scrub; the map shows `TrackStore::snapshot(T)` | feeding the store from all plugins and retention settings: the screen feeds from the page's tracks |
| Q5 RF heat map against GPS position | screen only against a generated NMEA track | a heat layer over `RfPositionLog::cells()` | `core/gps_reader.*` feeding `onNmeaLine` and the pass thread feeding `addReading`: hand-off |
| M9 Tile cache | screen half only | a "Map tile cache" line on the map page: size, entries, Clear cache, over `core/tile_cache.hpp` | the basemap plugin fetches tiles itself and returns decoded RGB: the cache needs the plugin's raw bytes and its response headers (a host API or the plugin): nothing can fill it from the screen. OSM policy: viewed tiles only, no offline use of stale tiles on tile.openstreetmap.org |

Order built: Q7a, M8 + polar plot, units, filter, M1a, M1b + locators, M2, Q5, M9, M3 (smallest first).

## 2. What was built and how it was proved

Commits: b834447e (the logic, the compositor and its test), e86764eb (the window, the publisher, the drive scripts, the ledger rows, the catalogue keys), then this record. Files: `src/slint/mapj_logic.*` (pure rules), `mapj_state.hpp`, `kit_publish_mapj.cpp` (KitPublisher members), `ui/mapj.slint` (the bar, the polar plot, the Sky and Antenna pages), edits to `map_compositor.*`, `kit_publish.*`, `ui/map.slint`, `ui/pages.slint`, `ui/main.slint` (export of the new global and structs, appended), `main.cpp` (two App menu entries 650 and 651 and the `mapj.` state reader), and a `mapj` block in the tests' CMake. One global callback, `MapJEvents.act(page, op, arg)`, carries every gesture.

| Row | What the window now does | Proof (decisive output) |
|---|---|---|
| Q7a Antenna calculators | App menu > Antenna calculator...: a page (kind 13) with Frequency, Distance, Coax (wavelengths), Velocity factor and Yagi elements fields; wavelength, half-wave dipole, quarter-wave ground plane and radials, Yagi reflector / driven / directors / boom (starting lengths, said so), coax length and free-space path loss (warned inside ten wavelengths), each in metres and feet; a bad field shows why and the rows that do not need it stay | `test_slint_host_mapj`: 14.2 MHz gives dipole 10.07 m / 33.04 ft, ground plane 5.04 m, coax 3.48 m, loss 95.5 dB, every bad input refused. Drive `mapj-antenna` (passed): `state mapj.dipole contains "20.14"` after 7.1 MHz is typed, `state mapj.loss contains "89.5"`, `abc` and one Yagi element refused (`mapj.antennaError`); shot looked at |
| M8 Az/el tracker | App menu > Sky positions...: a page (kind 12) with the Sun, the Moon and the five radio sources for the receiver position: azimuth, elevation, "below the horizon" greyed, elevation said to be geometric. The Stellarium link is NOT built (needs Stellarium installed) and the page says so | test: the Sun at 0 N 0 E at the March equinox noon is above 80 degrees and under the horizon twelve hours later. Drive `mapj-sky` (passed): `state mapj.skyRows == 7` once the position is set from the TOOLS bank; shot looked at |
| Polar sky plot of a pass | the same page: zenith at the centre, horizon on the rim, north up, east right, the bodies as dots, a pass as a line with its rise marked. The pass comes from a provider seam (hand-off 2); a made-up pass in a bounded run | test: rim, centre, N and E points exact, below-horizon samples left out, summary (rise 215, peak 64, 600 s). Drive: `state mapj.passMax >= 60`; shot `mapj-sky.png` |
| Unit choice | a Units key on the map (Automatic, Metric, Imperial, Aviation, Nautical) over `core/access_units.hpp` (it is in the tree: no stand-in needed): the scale bar, the target tip and the list notes; Automatic keeps the old text byte for byte | test: 35 000 ft, mph, kt, nmi, the cycle, the scale bar (20 km, 10 mi, 10 nmi, 200 m, 200 ft, no bar at 0.3 m, never over 30 percent of the pane). Drive `mapj-replay-filter`: the key cycles through all five (`state mapj.units`) |
| Aircraft filter controls | Filter key: Filter aircraft, Hide on ground, Military or interesting, Callsign contains, Altitude from / to (ft), Type, Operator; summary "n conditions, k hidden". Altitude, ground and callsign filter the page's aircraft today; military, type and operator need the ADS-B plugin's fields (not in `CascadeTrack`), so those three are dimmed with a note until a provider is wired (hand-off 3) | test: every condition, the unknown altitude, the ground rule (under 50 ft and under 25 m/s), a double that supplies the plugin's fields. Drive: `ZZZ` hides the aircraft (`mapj.hidden >= 1`), `FOXSDR` shows it, altitude from 40000 hides it, 20000 shows it, `abc` drops the condition |
| M1a Layers | Layers key: nine switches. Airspaces (closed plugin paths, now washed faintly) is on; Weather radar, Receiver map and Repeaters are dimmed, off and listed with "this data source needs its terms and a key read first"; the legend gets rows for the layers that are on and the credit line gets their credits | test: the three cannot be turned on. Drive `mapj-layers`: `expect "Weather radar" enabled equals "false"` (and the other two), `state mapj.layers == "100000100"` at first; legend and credit in the shot |
| M1b APRS paths, locator-square style | ids ending `~path` of kind STATION are drawn 1 px and faint, behind everything, while the APRS paths switch is on (default on: the APRS plugin sends them only while its `aprs.digipaths` setting is on); locator squares are washed cells (next row) | test: the id rule, the 1 px width and low alpha, the compositor draws a trail of its own width (1 to 3 rows against 9 to 12 at a Display width of 10). NOT driven: no fixture sends a `~path` |
| Maidenhead overlay, worked / heard shading | Locator grid switch: lines (fields, squares, subsquares by zoom; never closer than 6 px), captions as Slint items (at most 120), heard and worked squares shaded (worked beats heard) from a provider (hand-off 4) | test: levels by span, captions (IO91 centred at 51.5 N 1.0 W), the cap, the antimeridian, cells of 2 x 1 degrees and 5 x 2.5 minutes, the pixels of the lines and the shading. Drive: `state mapj.grid >= 1`, `mapj.labels >= 1`; shot looked at (IO91wm and neighbours) |
| M2 ADS-B heat map and 24 h range outline | Heat map and 24 h range switches over `core/adsb_coverage.hpp`, fed once a second from the page's own aircraft (session only: owner decision 5); the range outline is a closed washed trail | test: 49 generated positions, the hottest cell has the hottest colour, 33 000 ft at 2.5 degrees east gives a range over 100 km in band 4 and none in band 0. Drive: `mapj.cells >= 1` after Heat map on; the status line read "1 heat cells | range 25 km" |
| Q5 RF heat map against position | Signal level switch: cells from `core/rf_position_log.hpp`, weak blue to strong red, "No GPS fix" when empty | test: a generated NMEA track (checksummed GGA sentences) gives cells whose colours span weak to strong, each 50 m across. Drive: `mapj.cells >= 3` with the generated track (a test seam). The GPS reader and the pass thread do not feed the log: hand-off 5 |
| M3 Timeline replay | Replay key: Play / Pause, speed 1x 4x 16x 64x, slider, clock and "5 min 30 s behind", Now; while it runs the map shows the recorder's tracks at that instant (interpolated, held 30 s, then gone) through the same list, targets and staleness rule | test: clock, speeds, scrub, the live edge, play from the end restarts, bad ranges; the recorder over the real track store (throttle, kinds, hostile ids, interpolation between two points). Drive: Play makes `mapj.replay` leave `live`, the speed key reads 4x then 16x, Now returns to live. The slider was not driven (no label in a bounded run) |
| M9 Tile cache | a line "N tiles, x of 256 MB; hits, misses" and a Clear tile cache key over `core/tile_cache.hpp`; the folder is never created from here; nothing fills it (hand-off 6) | test: the words. NOT driven (nothing fills the cache from the screen). Policy kept: only viewed tiles would be stored, no offline use of stale OSM tiles |

## 3. Break-it table (a mutation of a committed file, `test_slint_host_mapj` rebuilt and run, restored with git checkout, rebuilt)

Scratch script `sj-mutate.py`; the restored run is green (712 checks, 0 failed).

| Mutation | Result |
|---|---|
| a blocked layer can be turned on | red: 6 checks |
| an APRS path needs no station kind | red: 1 |
| replay does not stop at the newest instant | red: 1 |
| the filter ignores hide on ground | red: 1 |
| the filter reads metres as feet | red: 2 |
| the callsign condition is ignored | red: 1 |
| the scale bar never falls to the smaller unit | red: 3 |
| south up on the polar plot | red: 2 |
| the grid never shows fields | red: 1 |
| the recorder takes every point (no throttle) | red: 1 |
| cells are not drawn | red: 6 |
| the Maidenhead lines are not drawn | red: 1 |
| a closed trail is not washed | red: 1 |
| a trail of its own width is the Display width | red: 1 |
| setTrails does not see a changed width or wash | red: 3 |
| the heat ramp runs hot to cold | FIRST RUN GREEN: the test compared channels the swap kept in order. Two checks added (cold is bluer than red, hot is redder than blue); re-run red: 2 |

The `.slint` and publisher changes were not mutated (each is a recompile of `main.slint`); the drive scripts address controls by label, so a removed control fails the script's `click` or `expect`, which was not shown by a mutation.

## 4. Frame cost (the method of screen-s1.md)

Compose cost of one full frame, 1024 x 768, median of 15 (printed by `test_slint_host_mapj`): base 1.13 ms | heat 20 000 cells 8.96 ms | Maidenhead lines 1.12 ms | a washed 72-point outline 3.20 ms | 60 thin lines 4.38 ms. A compose runs only when something changed (a view move, a layer, new cells), not every frame.

Window: the ADS-B map page open with the receiver position set, 1600 x 900, the frame meter on, hidden desktop, default renderer, three runs each (`drive/measure/mapj-off.txt`, `mapj-on.txt`, `run_mapj.bat`); "on" = Heat map, 24 h range, Signal level and Locator grid on, and the Replay and Filter bars open:

| run | tick interval mean / p95 (ms) | the tick's own work mean / p95 (ms) | process CPU (s) |
|---|---|---|---|
| off 1, 2, 3 | 33.39 / 34.10, 33.37 / 34.11, 37.17 / 74.13 | 1.00 / 1.78, 0.92 / 1.64, 1.10 / 2.00 | 5.50, 2.81, 9.30 |
| on 1, 2, 3 | 33.37 / 34.07, 33.39 / 34.10, 34.16 / 39.13 | 1.07 / 1.82, 1.11 / 1.99, 1.26 / 2.22 | 4.25, 4.12, 8.45 |

Means: work 1.01 ms off and 1.15 ms on (about 0.14 ms more: the once-a-second feed, the model signature and the label placement); the intervals are the 33.3 ms frame cap except where other lanes loaded the machine (off 3, on 3). The compositing itself happens only on a change. These figures include no presentation of a frame (hidden desktop).

## 5. New strings

123 new catalogue keys in the 33 catalogues, MACHINE TRANSLATED by one Sonnet agent and NOT reviewed by a native speaker (`tools/i18n_keys.py --merge`, then `tools/embed-lang.py`; `python tools/i18n_keys.py` reports 0 missing, 0 empty, 0 stale in all 33; `test_i18n` in the gates below). The English keys are those of `kit_publish_mapj.cpp`, `mapj_logic.cpp` and the `@tr` of `ui/mapj.slint`, `map.slint` and `pages.slint`. The translator's doubts: German "Elevation" is "Erhebung" (the catalogue's own word); Catalan "plugin" is "connector" (the catalogue's); "Range" as in coverage uses language-natural words; "Body", "Heard square" and "Worked square" are its own phrasings; "Imperial" in Russian, Ukrainian and Turkish is a guess; the radio-source names are localised where a standard name exists. The App menu labels (`Sky positions...`, `Antenna calculator...`) are plain English like every label of that menu today.

## 6. Engine and wiring hand-offs (none is done)

1. **Feed from every plugin, not only open map pages.** The recorder, the coverage model and the replay clock run for each OPEN map page (`KitPublisher::feedMapSurfaces`, once a second; aircraft, vessel and station kinds, because the store has no satellite kind). A page never opened records nothing. Persistence (`TrackStore::save`, `AdsbCoverage::save`) is not wired: both are session only (owner decisions 5 and 7 are open). Files: `kit_publish_mapj.cpp`, or `core/plugin_api.cpp` if the feed should live in the host.
2. **Pass az/el.** `MapJShared::passProvider` returns the samples (`PassSample{tSec, azDeg, elDeg}`) of the next pass. The satellites plugin does not hand az/el to the host (a track carries latitude, longitude and altitude only): a host API or a track field (`plugin_abi.h`, not touched by this lane).
3. **Aircraft type, operator and military.** `MapJShared::filterDetail(id, subject)` fills `FilterSubject` from the ADS-B plugin lane (`adsbout`); once it is set the three controls enable themselves and `filterPasses` uses them. The ABI carries only a 5-bit category.
4. **Worked and heard locators.** `MapJShared::locatorMarks` (locator, Heard or Worked) from the ft8-decoder plugin's hand-off; the DX-spots plugin's PSK Reporter spots could fill Heard.
5. **Signal level against GPS position.** `MapJShared::rfLog`: `core/gps_reader.*` must call `onNmeaLine(line, hostTime)` and the pass thread `addReading(t, level, freq)` next to the S-meter fill (`docs/feature-records/net-map-models.md`, hand-off 7). The layer is off by default; the privacy rule is the owner's decision 6. `rfLog.cells()` is what the layer draws.
6. **Tile cache.** `MapJShared::tileCacheRoot` (a folder under the per-user data folder) is not set by `main.cpp`, so the line says no tiles are kept. The basemap plugin fetches its own tiles and returns decoded RGB, so a `put()` needs the plugin's raw bytes and the response's Cache-Control / Expires / Date: a host API call or a cache inside the plugin. Honour the OSM tile policy (https://operations.osmfoundation.org/policies/tiles/, read by the models lane): viewed tiles only, no prefetch, no offline use on tile.openstreetmap.org, a unique User-Agent.
7. **Weather radar, receiver maps, repeaters.** `LayerInfo::needsSource` stays true until one source is built; each needs its terms and usually a key read by the owner (Met Office, RainViewer, OpenAIP, Receiverbook, RepeaterBook), a source in the plugin kit, a credit string and its drawing.
8. **Units setting.** The choice is per map page and not saved. When the `access` lane wires its `unitSystem` setting, `MapJSurface::units` should start from it (`access::unitSystemFromName`).
9. **APRS paths.** The layer draws any path of kind STATION whose id ends `~path`; the APRS plugin must send them (`aprs::stationPath`, `core/aprs_path.hpp`) while its `aprs.digipaths` setting is on. No fixture does today.

## 7. Rows not built

- **Stellarium link** (the second half of M8): needs Stellarium installed (a third-party download). The in-window table and polar plot give the same positions without it.
- **Weather radar, receiver maps, repeaters**: the switches, the legend and credit line exist and the sources are OFF and listed; the data sources are the owner's reading (hand-off 7).
- The feeds named in the hand-offs (pass, filter fields, locator sets, GPS log, tile bytes): screen halves built against doubles.

## 8. Gates (Release, build `D:/foxsdr-build/en-screen-j`)

- `test_slint_host_mapj`: 712 checks, 0 failed (the break-it table above).
- Headless and slint, `ctest -L "headless|slint" -j4 --timeout 300` (tip e86764eb): "98% tests passed, 21 tests failed out of 1023". Two were MINE and are fixed (the commit after e86764eb): `test_slint_house_rules` (23 string literals in `mapj.slint` that are the host's operation names, now marked `// not-translated: ...` as lane C marks its own) and `test_i18n` (five fixed-buffer formats in `mapj_logic.cpp`, now `formatText`; and the Japanese "Signal strong" / "Signal weak" had a space between two CJK characters, now "信号強" / "信号弱"). Both pass alone afterwards, as do `slint_drive_ledger` and `test_i18n_glyphs`. Of the other 19, the known false failures failed again alone (test_airband_data, test_band_plan, test_converter_call_sites, test_page_resize_id, test_png_decode); `test_diagnostics_free` passed alone; 9 more passed alone (test_iq_open_async, test_iq_out, test_kiwisdr_client, test_record_start, test_sdrplay_source, test_web_server, test_webhook, test_health_app_stepped, test_diag_hang_free: load); NOT MINE and not chased: `test_engine_api_decode_radio` (timeout alone), `test_engine_api_transmit` (6 checks red alone after 116 s: the engine-merge record calls it a load flake that was green at the tip), `test_engine_api_source_kinds` (the fixture `scope.wav` is not in `src/slint/tests/drive/fixtures` of this worktree), `test_failure_files` (failed in screen lane C's record too). The diff of this branch against 090e7039 touches nothing in `src/core` or `tests` except the regenerated `lang_assets.hpp`.
- GUI, hidden desktop `FoxSDRTests`-style launcher, `FOXSDR_FRAME_CAP_HZ=60`, `ctest -L gui -j3 --timeout 300`: 323 tests, 319 passed, 4 failed in 2653 s (`slint_drive_add-kinds`, `slint_drive_deck-skins`, `slint_drive_look-waterfall-speed`, `test_health_app`); all four passed when re-run alone. The four `slint_drive_mapj-*` scripts passed in that run.
- Config: `%APPDATA%/foxsdr/config.json` sha256 before and after: `c21ce98dc30e50327f7666f7ee29047c0983d130ca3f21716d0c0e20e04603c3` both (the live runs used the test harness's own profiles).

## 9. Unverified

- Every translation (machine made, 123 keys x 33 languages) and the layout in languages other than English: the clip sweep (`tools/slint_clip_sweep.py`, probe build) was NOT run, so it is not shown that it did not grow. The bar's rows are Checkboxes and Keys with `overflow: elide` or the layout's own width; the 5-column layer grid and the five filter fields may be tight at 800 px in long languages.
- The Replay slider and the Layers legend's colours were not driven (the slider has no label in a bounded run); the replay was proved by the clock's arithmetic, the recorder test and the Play, speed and Now keys.
- APRS path lines were not drawn from a real plugin; ADS-B coverage from a recording was proved only on the one synthetic aircraft of the ADS-B fixture (one heat cell, a 25 km range).
- Real rendering on a visible desktop: all windows ran on the hidden desktop; the frame figures include no presentation of a frame.
- The compose cost with a real tile source (the tile path) was not measured with the new layers.
- A map page that is not open records nothing, so a replay shows only what was held since the page was opened.

## 10. Decisions for the owner

1. Coverage persistence (owner decision 5 of the models lane): the heat map and range outline are session only here. Keep, or persist the grid to the per-user data folder (it holds the receiver position)?
2. Track retention for the replay: the store's 6 hours and 500 000 points are the defaults; the replay starts ten minutes behind the newest instant, paused.
3. Weather radar, receiver maps, repeaters: which of the four sources (Met Office / RainViewer, OpenAIP, Receiverbook, RepeaterBook) may be built, after their terms are read.
4. The Stellarium link: leave unbuilt (the Sky page covers the positions), or accept a link that opens Stellarium only when the user has it.
5. Default of the APRS paths and Airspaces switches: on (they draw only what a plugin hands over). Signal level, Heat map, 24 h range and Locator grid default off.
6. Unit choice per map page (not saved) until the `access` lane's setting is wired.
7. The Airspaces layer now also washes closed footprints (satellite footprints, boundaries) faintly (alpha 28 of 255): a change to what a closed path looks like on the map.

## 11. After the gates: one layout fix, and what was re-run

The first look at `mapj-layers.png` showed the Layers panel pushing the map to the bottom of the page (a layout left to itself takes the page's spare height). The bar is now a Rectangle exactly as tall as its rows (`ui/mapj.slint`, `MapJBar`). The long credit line also ran past the pane's edge: `HaloText` (map.slint) now gives every copy of its text a width, so it is elided. After that change (and the house-rule and catalogue repairs of section 8) the following were re-run and PASSED: the four `slint_drive_mapj-*` scripts on the hidden desktop, `test_slint_main_screen`, `test_slint_map`, `test_slint_host_map`, `test_slint_host_mapj`, `test_slint_host_kit`, `test_slint_house_rules`, `test_i18n`, `test_i18n_glyphs`, `slint_drive_ledger`. The whole GUI gate (323 tests) and the whole headless range were NOT re-run after those last changes; the GUI gate of section 8 ran at the commit before them. UNVERIFIED that no other window script is affected by the `HaloText` width (it changes only a caption longer than its `max-w`).
