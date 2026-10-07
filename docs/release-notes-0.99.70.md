**FoxSDR can now listen to an rtl_tcp server on the network.** rtl_tcp is the small program that ships with the RTL-SDR software: it holds a dongle on one computer and sends its samples over the network, which is how an RTL-SDR on a Raspberry Pi in the loft, or on another PC, is heard from here. The Source list has a new last row, "rtl_tcp server (network)". Type the server's address into its box - a name or a number, then a colon and the port if it is not 1234 - and press Open, the way the Pluto's row works: selecting the row contacts nothing, and a failed Open leaves whatever was running alone. The sample rate, one tuner gain, automatic gain, the frequency correction (whole ppm) and the bias tee work as on a local RTL-SDR; the samples arrive as 8-bit values, as from any RTL-SDR. Stop pauses the stream but keeps the connection, so the server stays busy until you close the source. There is no automatic reconnect: if the server goes away the receiver says "Device stopped" and you press Open again. Nothing is installed on this computer - FoxSDR speaks the protocol itself. It was checked against the real rtl_tcp program serving an RTL-SDR on the bench: FoxSDR connected, streamed 2.4 MS/s, and reported the lost connection when the server was stopped. A Stop followed by a Start, sent through the browser remote (which runs the same code as the window's Stop and Start keys), was also tried against the real server: the spectrum froze, the connection was kept, and the spectrum ran again after the Start. Pressing the keys in the window itself, and the Linux build, have not been tried. Asked for through the feature-request box on 2026-10-03.

<!-- Deploy telemetry-worker/worker.js BEFORE this release ships: a Worker that does not know the "rtltcp" driver word drops every radio count for it (telemetry-worker/worker.test.mjs pins this). -->

Still not fixed, and known: the two fast-fail deaths reported from the field (0.99.64 on an RSP1 after 48 minutes, 0.99.65 on an RTL-SDR after 12 seconds) are not explained - since 0.99.69 every report names the other programs' DLLs in the process, and since 0.99.66 the sentinel names the faulting module, so the next one says more; the other twenty-eight languages' airband strings were not reviewed, and Italian and Dutch call the frequency list "Segnalibri" and "Bladwijzers" where German, French and Spanish call it by a name of its own, a difference older than 0.99.65 that was followed, not resolved; a recording whose disk disappears part way ends as an empty file with nothing on screen saying so; an MP3 patch speaker whose folder is removed says nothing; a log file held open by another program at start-up is silently switched off.

---

**SHA-256 of `foxsdr-setup-0.99.70.exe`:**
`TBD`

**SHA-256 of `FoxSDR-0.99.70-x86_64.AppImage`:**
`TBD`

**SHA-256 of `foxsdr-0.99.70-linux-x64.tar.gz`:**
`TBD`

**SHA-256 of `FoxSDR-0.99.70-aarch64.AppImage`:**
`TBD`

**SHA-256 of `foxsdr-0.99.70-linux-arm64.tar.gz`:**
`TBD`
