# FoxSDR 0.99.97

**FoxSDR 0.99.97 stops drawing while Windows has switched your screen off, and brings every page back inside the main window when the graphics driver will not give it a window of its own - two fixes for a laptop where FoxSDR, left running on ADS-B, appeared to freeze and its map vanished.**

This is a fix release; nothing else changes. The Microsoft Store copy stays on its current version until the next submission is certified.

---

**SHA-256 of `foxsdr-setup-0.99.97.exe`:**
`19caa6082147a510dee9884c40bc1993383206a3c396be2447180d2852d8f6a1`

**SHA-256 of `FoxSDR-0.99.97-x86_64.AppImage`:**
`0c33e069c7c8d2c0b9f698594e04ca9dca85bfd2165af2929dccb6e1ed32d6c2`

**SHA-256 of `foxsdr-0.99.97-linux-x64.tar.gz`:**
`4befc37cc5cfcb1a88ad3dca3126d226018b2eb4aacc58764c2dc6b02ad67612`

**SHA-256 of `FoxSDR-0.99.97-aarch64.AppImage`:**
`a2be3fa6586024813ada8aaaea663af4f19dde690d572ccd8419ac208c1c39de`

**SHA-256 of `foxsdr-0.99.97-linux-arm64.tar.gz`:**
`1746304034913b482490549eb156c26ea9c6cfd4e15899565cf8bdb9375188b9`

## Fixes, 2026-10-27

**Nothing is drawn while your screen is off.** FoxSDR used to go on drawing its window sixty times a second after Windows had switched the screen off. On one tester's laptop with Intel graphics, left running on ADS-B, that ended with the window no longer showing anything new - it looked frozen, and went black after a minimise - while FoxSDR itself kept running underneath. FoxSDR now listens for Windows saying your session's display is off, stops drawing until it comes back, and picks up where it was; the radio, the audio and the decoders carry on as before. A dimmed screen is still drawn. The log says when the display went off and when it came back. Whether this cures that laptop is still being confirmed with the tester.

**A page that loses its own window stays where you can see it.** When the graphics driver refuses FoxSDR a separate window part-way through a session, FoxSDR keeps every page inside its main window for the rest of it. A page that had been a window of its own - the ADS-B map, for one - used to be left where it had been, past the main window's right edge, with a thin strip showing and its title rail out of reach, so it looked as if it had closed. Every page is now moved and, if it has to be, shrunk to fit inside the main window, and a page opened afterwards opens inside it too. The log names each page it moved.
