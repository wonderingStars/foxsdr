# Photo and details card in the new window (branch feat/photo-new-window from release/0.99.111 287d78b2)

Local only. STATUS: code written (not yet built/tested at this commit).

Before: a click on a target on a plugin's map set the kit's detailsId_ and the DECODE bank's "Target details" section showed the built-in lines plus TRACK_INFO fields (TrackInfoCache is attached
in PluginKit::directiveAfter). Nothing asked for TRACK_PHOTO: attach() was called without d.trackPhoto, so photos().active() was false; no photo code in src/slint.
Now: PluginKit attaches the photo table; the photo cache's texture hooks store pixels in memory (PluginKit::photoStore_, id = "texture"); KitPublisher asks for the photo each tick a surface draws the
details (map card; DECODE drawer when open; Target details page) and calls photos().endFrame() at the end of publish(), so a photo nobody draws is released (cache count 0, pixels freed).
Card: TargetDetails (output.slint) gained photo / credit; MapModel.card at the foot of the list column (pages.slint) when pg->selectedId == detailsId_. Click on picture -> KitEvents.details-photo-clicked
-> openPhoto(): photoLinkIsSafe then the link UNCHANGED to the system browser. Close key on the card clears the selection.
New words: "Fetching photo...", "Open this photo's page in your browser".
Next: build, tests (src/slint/tests), picture, then item 2 (SHOW key on Fitted modules).
