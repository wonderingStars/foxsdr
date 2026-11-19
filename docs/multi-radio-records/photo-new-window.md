# Photo and details card in the new window (branch feat/photo-new-window from release/0.99.111 287d78b2)

Local only. STATUS: built; tests green (photo_store 15/15, photo_card 25/25); break-it (picture key made inert): photo_card 10 of 25 fail.

Before: a click on a target on a plugin's map set the kit's detailsId_ and the DECODE bank's "Target details" section showed the built-in lines plus TRACK_INFO fields (TrackInfoCache is attached
in PluginKit::directiveAfter). Nothing asked for TRACK_PHOTO: attach() was called without d.trackPhoto, so photos().active() was false; no photo code in src/slint.
Now: PluginKit attaches the photo table; the photo cache's texture hooks store pixels in memory (PluginKit::photoStore_, id = "texture"); KitPublisher asks for the photo each tick a surface draws the
details (map card; DECODE drawer when open; Target details page) and calls photos().endFrame() at the end of publish(), so a photo nobody draws is released (cache count 0, pixels freed).
Card: TargetDetails (output.slint) gained photo / credit; MapModel.card at the foot of the list column (pages.slint) when pg->selectedId == detailsId_. Click on picture -> KitEvents.details-photo-clicked
-> openPhoto(): photoLinkIsSafe then the link UNCHANGED to the system browser. Close key on the card clears the selection.
New words: "Fetching photo...", "Open this photo's page in your browser".
Next: build, tests (src/slint/tests), picture, then item 2 (SHOW key on Fitted modules).

## Item 2: SHOW key on Fitted modules
fitted.slint: FittedRowModel.has-page; a SHOW key between the chip and START/STOP (greyed with "This module has no window of its own." when the module has no panel / picture / instrument /
map page); callback show-clicked -> pages.slint fitted-show -> KitEvents.page-fitted-show -> KitPublisher::showModule: openWindowsFor (an open tab stays one tab, a closed one comes back), then the first
of the module's panel, instrument, picture, map pages is brought to the front (several pages: that main one, not the page last closed: that is not remembered). A torn page is un-rolled, NOT raised above other
torn windows. A STOPPED module: SHOW starts it (kit_.startStop) and shows the pages in the publish tick in which the row stops being stopped (showPending_). Words: "SHOW", the two tooltips.
Existing pixel tests of test_slint_fitted shifted by one key (indices and element counts updated: 629/629 green).

## Item 3: serial-port chooser
Cause: ui/system_bank.slint DropKey (and its twin in tools_bank.slint, the Radar fold) opened only when items.length > 0, and the host read the ports only when the SYSTEM bank's Serial section was first
opened (system_publish.cpp sectionOpened, serialAsked_ once) - on a map page the list stayed empty and the click did nothing. Fix: DropKey `opening` callback on every click (GpsRowView -> SystemEvents.serial-refresh,
the Radar fold the same), `empty-text` "No serial port was found." as one line in the popup.

## Proof (all on D:/foxsdr-build/mr-dsp, Release)
test_slint_photo_store 15/15, test_slint_photo_card 25/25 (break-it: picture key inert -> 10 fail), test_slint_fitted_show 4/4, test_slint_gps_chooser 5/5 (break-it: empty-text "" = the old dead click -> A fails),
test_slint_fitted 629/629, test_slint_map_page 72/72, main_screen 84/84, tabs 299/299, host_kit 155/155, host_map 754/754, house_rules 392/392, core test_track_photo_cache 191/191, test_track_info_cache 2094/2094.
Picture: D:/foxsdr-build/stage-photo/shots/card-selected.png (map page, aircraft selected: card at the foot of the list column, details, photo, credit, Go to / Follow / Close).
Not done: Escape key for the card (Close key only); no test of the chooser inside the folded Map options; torn page not raised; no Fitted modules picture; the publisher-level behaviour (an open tab is not duplicated, a stopped module is
started then shown) is existing/untested code paths driven only by reading; no real mouse; photo seen only from the plugin's FOXSDR_SYNTHETIC stand-in, never from the real service.
