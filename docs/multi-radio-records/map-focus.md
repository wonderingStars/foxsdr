# Map focus: the map is the page (fix lane, branch fix/map-focus from release/0.99.110 2920fae8)

Local only: nothing pushed, published or released.

## What was wrong, and why

Owner's complaint: "the way you are presenting the map on all the plugins is wrong: the map should be the main focal point and it should be resizable".
Every plugin map page (kind 5) is one block, "// 5 map" in src/slint/ui/pages.slint (PageBody). In the released 1600 x 1600 picture the map sat in the bottom quarter under
about 700 px of empty space; in a 1600 x 900 window it was cut off.

CAUSE (reproduced before any change): `component MapToolbar inherits VerticalLayout` (pages.slint, was line 373) was not given its own height. A Slint layout's stretch is the
sum of its children's, and its GPS row (`GpsRowView`, system_bank.slint ~1380, a VerticalLayout with a horizontal-stretch DropKey and a stretching Rectangle) made the toolbar
claim a share of the page's spare height; the map row (`vertical-stretch: 1`) got only the rest. `MapJBar` (mapj.slint) already pins itself with `height: bar.preferred-height`
for exactly this reason (its comment: "a layout left to itself takes the page's spare height"); the toolbar did not.
Measured by the new test on the unchanged code (red.txt): page 1240 x 1373 (a 1600 x 1600 window): map 980 x 392 at y 981, 860 px between the toolbar row and the Layers row;
page 1240 x 673 (1600 x 900): map 159 high; page 664 x 413 (1024 x 640): map 46 high.

## What changed

* pages.slint `MapToolbar`: now a Rectangle exactly as tall as its rows (`height: bar.preferred-height`, as MapJBar). One compact row: Fit, WHOLE WORLD (satellites), Set RX here,
  the receiver position, "following X" with its Stop following key (a state the user must see, so it stays on the row), the target count, On screen only, and ONE fold key "Map options".
  Behind that key (closed by default): the Coverage / Trails / Altitude colours / Line-Ribbon(-Off) row, the Width / Reset coverage / coverage status row, and the GPS hint and
  GPS port row. The follow-interrupt card and the satellites' notes stay always visible (they are notices, not options). The fold is a toggle key (`on:` while open), the same
  pattern as the Layers / Replay / Filter keys on the next row.
* pages.slint PageBody map block: the list's width is `map-list-w` (260 px by default) with a 6 px divider (accessible slider "Resize target list") between the list and the map.
  The width follows the pointer from the press (absolute pointer x at the press and now, never "current width plus this move"), from 0 (list hidden) to half the row; a double-click
  puts it back to 260 px; the accessible increment / decrement actions move it 20 px. The list is `min(map-list-w, row / 2)` so a shrinking window cannot leave it larger than half.
* tabs.slint `TornWindow`: the right and bottom edges now resize like the corner grip (6 px strips, same `resize(dx, dy)` callback, host rule unchanged: kit_publish.cpp
  `on_page_chrome_resize`, minimum 240 x 140).
* tests: src/slint/tests/test_slint_map_page.cpp with ui/specimens/map_page_specimen.slint (PageBody, optionally inside a TornWindow), registered with the element-debug-info list in
  tests/CMakeLists.txt.

State that is remembered: the fold and the list width are properties of the PageBody instance, so they last while the window is open (docked pages share one instance, so the choice
is shared by the docked map pages; a torn-off page has its own). They are NOT saved between starts: there is no per-page place for such state in the saved settings and
adding one is more than this change. "Plugin settings" was already closed by default (`Page::settingsOpen = false`, kit_publish.hpp) on every page; unchanged.

New visible words (all through @tr, English only): "Map options", "Coverage, trails and the GPS receiver" (its tooltip), "Resize target list", "Drag to resize the list.
Double-click puts it back." (the divider's tooltip).

## Every place a plugin's map is shown

* Main window, docked page: PageBody via main.slint (`view-index >= 3`): the fixed block.
* Torn off ("+ Window" in the tab row / tab drag-out): PageBody inside TornWindow (main.slint, `for p[i] in pages`): the same block, so the same rule; the window now has edge resize.
  The map's `resized(w, h)` reaches `KitEvents.page-map-resized` -> `MapSurface::w/h` in kit_publish.cpp:1897, so the picture is re-composed at the new size (the test checks the
  event carries the pane's real size after an edge resize).
* One / two / four radio layouts: those panes (radio_pane.slint) hold the spectrum and waterfall, not plugin pages; no map page lives in a pane, so nothing there to change. The
  Patch page's own Map part (patch_page.slint, a MapPane under a face) is a different embedding and was not touched.
* Plugins: every map page comes from `syncMapPages(...)` over the plugins that publish tracks (kit_publish.cpp / plugin_kit.cpp) and is drawn by the one kind-5 block; there is no
  per-plugin map layout. From the plugins repository, plugins that declare or use the tracks capability: adsb, epirb, sonde (radiosondes); by name match on tracks and position
  data: aprs, ais, satellite, hfdl, vdl2, navtex. All use the shared block (UNVERIFIED by running each: the finding is from the code path, not from a window per plugin).

## Proof

See the hand-back table in the commit message; figures are from test_slint_map_page (72 checks): red before (15 of 31 failed), green after (72 of 72), break-it run (4 fail when
the toolbar's height pin is removed with the fold open).

## Not proved

* Not driven with a real mouse on the real window; the divider and edge drags are tested through the harness's dispatched pointer events.
* Torn-off window: left and top edges do not resize (only right, bottom and the corner); the window cannot be dragged larger than the main window's content.
* Fold and list width are not saved between starts.
* Other languages: the new words are English only; the catalogues have no entry for them yet.

## FINAL STATUS (map work)

Tests (Release, this tree, ctest -C Release): test_slint_map_page 72 checks 0 failed; test_slint_host_map, host_mapicons, host_mapj, host_kit, host_layouts, house_rules, po, instruments,
sliders, tabs, map, constraints, main_screen: all passed. The application target foxsdr-slint was rebuilt from scratch (its vc143.pdb had reached 4 GB, error C1090, so that target's
objects and PDB were deleted; this is a build-folder matter, not a source one).

Numbers (test_slint_map_page, page area of a 1600 x 1600 / 1600 x 900 / 1024 x 640 window, options folded): map 974 x 1246 / 974 x 546 / 398 x 283 px (was 980 x 392 / 980 x 159 / 404 x 46);
list 260 px; Fit row to Layers row 6 px, Layers row to map 6 px.

Pictures (staged copy, hidden desktop, generated ADS-B file only, no tile plugin): D:/foxsdr-build/stage-mapfocus/shots/adsb-map-main.png (1600 x 900), adsb-map-main-options-open.png,
adsb-map-1024x640.png. At 1024 x 640 the page itself is only about 660 x 410 after the left bank (360 px) and the window's own header, and the map is what is left of it (about 400 x 170
with the 260 px list; drag the divider to hide the list for more). The torn-off picture was NOT obtained: the input script's `drag` on the tab did not tear the page off.

## 2. The window searches for radios by itself once (separate commit)

FAULT: nothing in the new window sent the radio search; the Source list on a clean start offered only Signal generator, IQ file and Sound card until Refresh was pressed
(`opScanOnOpen` existed and was tested but had no caller; the contract has no "list opened" callback).
CHANGE: `SignalPublisher::publish` sends `opScanOnOpen()` (FOXAPP_OP_SCAN_DEVICES_ON_OPEN 0x8401) once, 1.5 s after its first publish (`scanOnStartDue`, signal_logic.cpp). The op chosen
is the ON_OPEN one, not Refresh's FOXAPI_OP_SCAN_DEVICES: in engine.cpp (~8316-8330) both run `scanNative()` (lists native radios, opens nothing), but ON_OPEN runs the SoapySDR scan only
if it never ran (or a partial one is owed and its gate allows it), whereas Refresh always asks `scanSoapy()`; the gate defers while a radio is open, so a running or restored radio is not
disturbed either way, but ON_OPEN never repeats a probe the engine already did. Refresh keeps working unchanged. Bounded (scripted) runs stay free of device probing unless they set
FOXSDR_SCAN_ON_START=1 (needed here to prove it in a scripted run); no existing test sets it.
TEST: test_slint_host_signal, 4 new checks on `scanOnStartDue` (not before 1500 ms, due after, only once, bounded only when opted in). RED with the function body forced to `return false`
(2 of 432 failed: lines 1037 and 1039), GREEN with the real body (432 checks, 0 failed). The wiring itself (the call in publish) is proved by the real run below, not by a unit test:
SignalPublisher cannot be built without the engine, and it has no test seam.
REAL RUN (hidden desktop, staged copy, fresh scratch profile with no saved radio, no key pressed, no SYNTHETIC flag, receive-only search, no radio opened): Source list before
(scan not opted in): 3 rows (devices=3): Signal generator, IQ file, Sound card. After (FOXSDR_SCAN_ON_START=1, list read about 12 s into the run through the input script's dump):
6 rows: Signal generator, IQ file, Sound card, "RTL2838UHIDIR (serial 00000001) (native)", "ADALM-Pluto (network)", "rtl_tcp server (network)". The RTL-SDR row appeared without a key press;
the receiver stayed STOPPED on the generator. The B200 did not appear in this list (UNVERIFIED why: the SoapySDR/UHD probe may not have found it, or may have been the "Pluto" / network
rows' neighbour; not investigated). Not run: a non-bounded launch (the bounded path differs only by the opt-in test).

## 3. Torn-off page windows: the main window's drag rule (separate commit)

FOUND: a torn-off page is not an operating-system window but a frame (TornWindow, tabs.slint) drawn inside the main window; its place and size are the host's (kit_publish.cpp Page::tx/ty/tw/th).
Both its title drag and its resize grip reported "the pointer's offset from the press in the area's own coordinates", and the host added every report to the place or size it had now
(`p.tw = max(240, p.tw + dx)`): the area sits on the frame that follows the pointer, so a host one republish late adds the same offset again - exactly the main window's drive-fixes-7 fault.
CHANGE: DragArea (tabs.slint) now also reports `began()` and the pointer's offset from the press in window coordinates (abs-dx / abs-dy); TornWindow passes `move-began / move-total` and
`resize-began(edge) / resize-total(edge, dx, dy)` up (edges 1 right, 2 bottom, 3 corner); the host (kit_publish.cpp) remembers the place at the press and a look_logic `EdgeDrag` begun at the
frame's size, and asks `tornResizeAt` / `tornMoveAt` (look_logic.cpp) - the same `EdgeDrag::at` the main window uses, so one rule; the old per-move handlers were removed.
No system-pointer follow is needed or used: the frame lives inside the window, so the pointer never leaves the window the way it did at the main window's edge.
TESTS: test_slint_host_look `tornWindow` (a thousand reports of one offset give one size, minimum 240 x 140, limit, the three edges, move clamped at 0; GREEN, 5500+ checks, 0 failed);
RED on the old path is not reachable (the old rule was an inline lambda in the publisher), so the old rule is reproduced in the test the way the main window's test does it. test_slint_map_page
(torn section) was moved to the anchored callbacks and stays green; test_slint_tabs and main_screen pass.
NOT PROVED: no real pointer on the hidden desktop. The owner's screen is needed: open the ADS-B map page, tear it off (drag its tab downward), then (1) drag its bottom-right corner
grip and its right and bottom edges quickly (about 0.3 s from one side of the screen to the other) and watch for the frame jumping or overshooting the pointer; (2) drag its title bar quickly
the same way. Expected: the frame edge stays under the pointer and does not jitter.
