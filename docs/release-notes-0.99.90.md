# FoxSDR 0.99.90

**FoxSDR 0.99.90 starts on Windows computers that have no working graphics driver. Until now, a computer without an OpenGL driver - no graphics driver installed, a virtual machine without 3D, some remote desktop sessions - showed an error and FoxSDR closed. FoxSDR now carries its own software renderer and, when the graphics driver cannot be used, starts itself again on it.**

Nothing changes on a computer whose graphics driver works: the driver is always tried first. The Windows download is about 15 MB larger, because it carries the renderer. The Microsoft Store copy stays on its current version until the next submission is certified.

---

**SHA-256 of `foxsdr-setup-0.99.90.exe`:**
`bcf8d84bdfded60a5939967e2763d016824b291657bbd1143d87e344a45658a1`

**SHA-256 of `FoxSDR-0.99.90-x86_64.AppImage`:**
`52a41fe3623f087cca42142d36952a84322103fc2f523322cd4a68ec8a639786`

**SHA-256 of `foxsdr-0.99.90-linux-x64.tar.gz`:**
`16ac3ad321550641bc4f5d7e55b0ff53d8aca69109b44fc39cbe09cb57ffd652`

**SHA-256 of `FoxSDR-0.99.90-aarch64.AppImage`:**
`df67749418bb139607b8b349e621123c505dc27e2568fab76453976c8e018df7`

**SHA-256 of `foxsdr-0.99.90-linux-arm64.tar.gz`:**
`a27cc16334d4da64279b8e6c7dee07d185c9c0fda8063885bd082b3a28913e00`

## Features and fixes, 2026-10-25

**FoxSDR starts without a graphics driver.** FoxSDR draws its window with OpenGL, which the graphics card's driver normally provides. A tester's computer had no such driver, and FoxSDR could only say so and close. The Windows version now includes Mesa, a well-known open-source OpenGL that draws with the processor instead of the graphics card, in a `softgl` folder beside FoxSDR. When the graphics driver offers no usable OpenGL, or one too old for FoxSDR (older than OpenGL 3.0), FoxSDR starts itself again on Mesa a moment later, and the window looks and works the same. It uses more processor time, so on a slow computer the picture may be less smooth; installing the graphics card's own driver is still the better fix. On our test computer it drew about 85 frames a second.

**Choosing the renderer yourself.** Set the environment variable `FOXSDR_GL` before starting FoxSDR: `software` always uses Mesa (useful if a graphics driver crashes), `hardware` never does. The `window:` lines in the diagnostics log say which one drew the window.

**No more silent exit when the graphics driver is too old.** On a computer whose driver offered an OpenGL older than 3.0, FoxSDR used to close without a word. That case now moves to the software renderer, and if FoxSDR still cannot open its window it says so and the report it files says why.

**Licences.** The software renderer is Mesa 26.2.4 with LLVM, as built by the mesa-dist-win project; their licences are in `THIRD-PARTY-LICENSES.txt` beside FoxSDR.
