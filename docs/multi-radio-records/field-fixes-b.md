# Field fixes B (application): 0.99.109 faults reported after release

Branch fix/field-b from engine/multi-radio 03ac57b7. Local only: nothing pushed, nothing published.

## Fault 2: the VOR decoder's "OBS course" control ignored a click (drive row L596)

CAUSE. It is not a slider and not the plugin's settings row. The VOR decoder (vor-decoder 1.0.1) declares an instrument face; the
application draws it as NavBearingFace, whose only control is ObsKnob (src/slint/ui/instruments.slint, ObsKnob, 40 px round).
ObsKnob turned only by wheel, by a RELATIVE press-drag and by Left / Right. Its press handler recorded the starting angle and
nothing else, and there was no release handler, so a click without a drag never produced a course. It carries
accessible-role: slider and the label "OBS course", which is why the drive (and a reader of the accessibility tree) called it a
slider. The scripted `slider "OBS course" 0.5` clicks the exact centre of the 40 px knob, where there is no bearing at all.

Slider (src/slint/ui/sliders.slint) is NOT affected: a press on its track already jumps the value there. Checked with
test_slint_sliders (254 checks, green; track presses at the middle, both overhangs and the Gain lane of SliderGroup).

CHANGE. ObsKnob: a left press that moves 3 px or less and is released takes the course to the bearing of the pointer from the
knob's middle (0 up, clockwise, whole degrees). A click within 6 px of the middle does nothing (no bearing). A drag stays relative,
as before (it never jumps to where the hand took it, and its release does not jump either).

PROOF. src/slint/tests/test_slint_instruments.cpp, checkObs: clicks at 3, 6, 9, 12 o'clock and half past four give 090, 180, 270,
000, 135; the middle gives no event; a press-drag ending where it began adds no jump. RED before the change (4 of 1725 failed:
lines 872, 875, 878, 881: the clicks at 180, 270, 000 and 135 produced no event), GREEN after (1725 checks, 0 failed).

NOT PROVED. Not driven with the real mouse on the VOR page in the real window. The drive script
src/slint/tests/drive/plugin-vor-decoder.txt still presses the knob's centre (`slider "OBS course" 0.5`), which by design still
does nothing; a real-pointer pass needs a click off-centre (for example on the ring at the right, expect 090).

## Fault 1: Map Imagery (OpenStreetMap) drew no tiles on the Satellites map page (drive row N7)

CAUSE. By design, not a defect in the path between the plugin and the new window. The Slint map page chooses its ground in
`KitPublisher` (src/slint/kit_publish.cpp, the compositor choice near line 523, now `mapDrawsTiles(bm.active(), pg->satellite)`):
a page whose tracks are all satellites (`MapPage::satellite`, src/slint/kit_logic.hpp, set in src/slint/plugin_kit.cpp ~line 213)
never takes the basemap's tiles and draws the compiled-in Natural Earth coastline, equirectangular. The attribution then reads
"Coastline: Natural Earth (public domain)" (kit_publish.cpp ~line 692), and "No map tiles are kept on disk" is the tile-cache
line of the map page (src/slint/kit_publish_mapj.cpp:331: nothing fills the folder because the plugin keeps its own cache).
The classic window does the same: `MapView::drawSatellitePanel` calls `setProjection(MapProjection::Equirectangular)`
(src/gui/map_view.cpp ~3548; the note in map_view.hpp says why: tiles are Web Mercator, a ground track would be torn apart).
The 3D globe draws its own land picture and has no tile path. So the report's FLAT MAP and 3D GLOBE of the Satellites plugin
showed the coastline exactly as built. The plugin's chain to the window was read and is intact: engine
`chooseBasemapAndTrackInfo` (src/engine/engine_plugin_run.cpp:199) -> directive -> `PluginKit::directiveAfter`
(plugin_kit.cpp:~171) -> `BasemapClient::attach` -> `rawTile`. There is no consent or setting in the way: the plugin fetches by
itself (tile.openstreetmap.org by default) when the compositor asks it for a tile; with the plugin not installed or stopped
nothing is asked and nothing is fetched.

WHAT WAS MISSING: the page said nothing about it, and the catalogue promised tiles "under the map". No default is changed here:
nothing starts fetching tiles that did not before.

CHANGE (application). `mapDrawsTiles` and `mapImageryUnused` (kit_logic.hpp / .cpp) state the rule once; kit_publish.cpp uses
the first for the compositor and, on a satellites page while the imagery plugin is running, adds one key line under the chart:
"MAP IMAGERY: NOT USED HERE - ITS TILES ARE MERCATOR, THIS MAP IS NOT". The plugin half (catalogue text, privacy sentence) is
openmaptiles-basemap 1.3.1 in the plugins repository (docs/feature-records/field-fixes-b.md there).

WHAT A USER NOW SEES after installing and starting the imagery plugin: the aircraft, ship and station map pages (and any
non-satellite map page) draw OpenStreetMap tiles with the OpenStreetMap credit; the Satellites page keeps the coastline and says
in its chart key that the imagery is not used there. FETCHED, FROM WHERE, WHEN: by the plugin itself, from
tile.openstreetmap.org unless the user configured another server in openmaptiles.conf, one request per visible tile that is not in
its disk cache, only while a tile-drawing map is on screen (the compositor asks per frame) or the browser map page asks; each
request tells that server roughly where the map is looking. The catalogue text now says so.

PROOF. test_slint_host_kit: the new block calls the two functions for all four combinations; it did not link before the functions
existed (RED), and passes after. See the test run in the commit message. The existing compositor tile path is covered by
test_slint_host_map with stand-in tiles.

NOT PROVED. Tiles actually drawn on an aircraft, ship or station page of the new window with the real plugin and the real
network (the application was not started for this; stand-in tiles only, as window-gaps-4 row 1967 already says). The new key line
was not looked at in a rendered window, and it has no translations (it falls back to English). A privacy decision remains the
owner's: whether the Satellites page, or the first run of the imagery plugin, should carry a visible on/off switch for fetching.

## Checks run (both faults)
`ctest -C Release -R "test_slint_(house_rules|host_kit|host_map|host_mapj|instruments|sliders|extend|po)$"`: 8 of 8 passed (test_slint_host_kit 155 checks,
test_slint_instruments 1725, test_slint_sliders 254). `foxsdr-slint` rebuilt and linked with the change. The GUI drive scripts (`slint_drive_*`) were not run
(they need the hidden desktop and the guard test refuses the real one); the application was not started.

## Part 3 (orchestrator, 2026-11-19): the map page said "No map tiles are kept on disk." while the plugin kept them

PROVED FIRST (tile check, released 0.99.109 build, hidden desktop, real network, generated ADS-B file only; evidence in
D:/foxsdr-build/tile-check/run): the aircraft map DOES draw OpenStreetMap tiles under the released build. 0 tiles on disk before
the page opened, 15 tiles (585,941 bytes, PNG) after, four connections to the tile host, and the saved picture shows the streets
of central London under the generated aircraft. So the "not proved" paragraph above is closed for the aircraft page. Not run:
the ship and station pages, and the Satellites page in the same run.

FAULT FOUND IN THAT RUN. The page's own line under the map read "No map tiles are kept on disk." with 15 tiles on disk, and the
"Clear tile cache" key was dark. CAUSE: the line read only the host's cache (core/tile_cache.hpp, `MapJShared::tileCacheRoot`),
which nothing sets and nothing fills from the screen; the Map Imagery plugin keeps its tiles in its OWN folder
(LOCALAPPDATA/foxsdr/tiles/.../<y>.img; plugins repository src/openmaptiles/openmaptiles_plugin.cpp, cacheRoot). A statement about
what is kept on disk that is false is a must-fix.

CHANGE. mapj_logic: `imageryTileRoot` (the plugin's folder rule, held equal by the test), `imageryTilesOnDisk` (counts *.img
below it), `imageryTilesClear` (deletes *.img below it and nothing else: not *.part, not a folder), `imageryTilesText` and
`tileCacheLine` (the whole line: host cache, plugin tiles, or that nothing is kept). kit_publish_mapj.cpp uses them every five
seconds for the line and for whether the key is lit; the key now clears both. New sentence (English only until translated):
"%zu map tiles are kept on disk by the Map Imagery plugin (%.2f MB)."

PROOF. test_slint_host_mapj: the new block did not compile before the functions existed (RED: C3861 'imageryTileRoot':
identifier not found); after: 748 checks, 0 failed. The eight tests named under "Checks run" pass again (8 of 8).

NOT PROVED. The new line and the lit key were not looked at in a rendered window. Clearing while the plugin is writing a tile
was not tried (a *.part is never touched; the plugin fetches a deleted tile again when a map next asks).
