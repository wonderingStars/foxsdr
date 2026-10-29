# FoxSDR 0.99.103

**FoxSDR 0.99.103 stops a tooltip or menu from ever being given a window and graphics context of its own - which, over a map page, happened on every blink of an aircraft's hover detail and is what a tester's laptop most likely spent its half-second stalls on before its graphics driver ran out of contexts - and it makes the log say so either way: which part of the drawing a slow frame went to, which page has a window of its own on which monitor, how many pop-up windows a session made, and what the program is holding of the things Windows hands out in limited numbers.**

This is a drawing-path change plus diagnostics; nothing changes in how FoxSDR tunes, demodulates or decodes. The Microsoft Store copy stays on its current version until the next submission is certified.

---

**SHA-256 of `foxsdr-setup-0.99.103.exe`:**
`TBD`

**SHA-256 of `FoxSDR-0.99.103-x86_64.AppImage`:**
`TBD`

**SHA-256 of `foxsdr-0.99.103-linux-x64.tar.gz`:**
`TBD`

**SHA-256 of `FoxSDR-0.99.103-aarch64.AppImage`:**
`TBD`

**SHA-256 of `foxsdr-0.99.103-linux-arm64.tar.gz`:**
`TBD`

## Features and fixes, 2026-10-29

**A tooltip or menu stays inside the window it belongs to.** A tester's laptop (Intel UHD Graphics 630) spent 81 minutes drawing one frame in every twenty-six at 430 to 520 ms, all of it in the part of the frame that draws the pages' own windows, only while the ADS-B map page had a window of its own - and twice that day the graphics driver refused to make another shared graphics context, answering that the system was out of resources, although FoxSDR had asked for what looked like the first one of the session. The map, plugin store and fitted-modules pages are windows of their own from the moment they open, and the interface library let a tooltip or menu extend over the whole monitor: one that poked past the page's edge was given an operating system window and a shared graphics context of its own, made inside the frame and destroyed the frame after the tooltip went. On a busy map with the pointer resting on the chart, every target that drifted under it made and unmade a graphics context. From this release a tooltip or menu is placed inside the window it belongs to and never has a window of its own; a menu at a window's bottom edge now flips up instead of spilling over the desktop. Whether this was that laptop's whole story, the next of its logs will say (below).

**A slow frame in the drawing says which part.** The log's slow-frame line for the `render` part of the frame now names what inside it took the time - the main window's draw, the pages' windows being created, moved and destroyed, or one page's own window's draw or swap, by the window's id: `frame: 455 ms, 455 ms of it in render; largest part: window-swap 440 ms (torn-off window 0x8a3f2b1c of 2)`.

**The log says which pages have windows of their own, and where.** `viewports: page "ADS-B Map" in its own window 0x8a3f2b1c - 1026x699 at 588,202 on monitor 2 of 2 (DELL U2720Q, 100%); window 3 this session, 1 outside`, and the line that says when it is back in the main window. The monitor is named by its model, as the display driver reports it, never a serial number. Pop-up windows over such a page are counted, not listed: `viewports: 41 pop-up windows ... since the last line; 42 this session`, at most once every thirty seconds, and at a refusal the session's totals, including the interface library's own count of every window it ever created.

**The log says what the program is holding.** Once the window is up, whenever a page's window comes or goes, at a refusal and every ten minutes: `resources: gdi 412, user 97, handles 1234, memory 812 MB, video 356 of 3900 MB` - FoxSDR's own counts of the objects, handles and memory Windows hands out in limited numbers, and its own video memory as Windows accounts it. Nothing about any other program; PRIVACY.md lists the line.
