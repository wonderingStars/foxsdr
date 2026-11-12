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
