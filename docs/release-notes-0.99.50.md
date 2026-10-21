**The plugin store now cleans up after its own updates.** A plugin's file name carries its version, so updating one used to leave the old file on disk beside the new one - and FoxSDR would then load both, silently ignoring the older copy. An update now removes the old copy it replaced as soon as the new one has loaded, and the store's updates banner gains a "CLEAN UP OLD VERSIONS (N)" key for anything left behind from before, with one confirmation listing every file before it goes. If a file cannot be removed right away (Windows can refuse to delete one still held by a second running copy of FoxSDR), it is queued and cleared automatically the next time FoxSDR starts.

**The FUNCTION SELECT panel can now be folded to a strip.** A "<<" key folds it down to 34 pixels with a ">>" key to reopen it, handing its width back to the patch page or the receiver view - useful on a small laptop screen where that column was a third of the whole window. The choice is remembered between sessions.

**The patch page's information pane has moved to the top right**, beside the transport and parts rows, instead of running the full height of the page down one side. This gives the canvas underneath the extra width that column used to take.

**The patch canvas can now be panned by dragging empty canvas, and a plain mouse wheel pans instead of zooming.** Previously the only way to move around a large patch was a middle-button drag, which a laptop touchpad cannot do at all. Now a left-button drag that starts on empty canvas pans the view (a drag on a node, port or wire still does what it always did), a two-finger touchpad scroll pans in both directions, and Ctrl+wheel (also what a touchpad pinch sends on Windows) zooms about the pointer as the plain wheel used to. New "+", "-" and "Fit" keys sit in the canvas's bottom-right corner for zooming and bringing every node back into view. **This is a behaviour change: a plain mouse wheel over the patch canvas now pans rather than zooms.**

**Two fixes for radios FoxSDR could already see, but reported wrongly.** The "No radio hardware found" warning could appear even while a native-driver radio (RTL-SDR, HackRF, Airspy and similar, reached without SoapySDR) was working perfectly, because the warning only checked the SoapySDR device list. It now only appears when neither list holds a radio. Separately, while a patch is running and holding the radios, the Source row used to just say "Signal generator" with no hint that a real receiver was behind it - it now names the radio the patch has, both in the Source combo and in the folded row's chip.

These fixes come from a beta tester's suggestions - thank you.

---

**SHA-256 of `foxsdr-setup-0.99.50.exe`:**
`TBD`

**SHA-256 of `FoxSDR-0.99.50-x86_64.AppImage`:**
`TBD`

**SHA-256 of `foxsdr-0.99.50-linux-x64.tar.gz`:**
`TBD`
