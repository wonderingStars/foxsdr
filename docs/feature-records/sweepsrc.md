# sweepsrc: sweep devices as a source, wide-band stitcher and heatmap, trace history files, occupancy model

Lane `sweepsrc`, branch `feat/sweepsrc` (from engine/multi-radio d3630d78). New-file model lane: nothing
existing is edited, `tests/CMakeLists.txt` included (the tests are found by its glob). No version string
changes. No radio, serial port or network was opened.

## Step 0: the table (reading only, done first)

Date of the reading: 2026-11-11. "Free-source check" = the maker's own published description, its terms, and
whether it sits inside a GPL/LGPL repository (closed to this lane).

| Row | Verdict | Why |
|---|---|---|
| tinySA sweep device as a source | **new file, built** (`scanraw` only) | The maker's USB-interface page (tinysa.org wiki, no GPL repository, no licence text on it) gives the exact `scanraw` request and binary reply. The text `scan` reply layout is NOT described there, so it is not built. |
| RF Explorer sweep device as a source | **blocked** (device frames) + **alternative built** | The only maker description of the serial protocol is the "RF Explorer UART API interface specification", a page in the wiki of the RFExplorer-for-.NET repository, which GitHub lists as LGPL-3.0. `rf-explorer.com/API` and the user manual both point only there. Closed to this lane. The manual (61 pages) holds no frame description. Alternative that IS built: `importFreqLevelPairs`, the import of the device's exported "frequency (MHz), amplitude (dBm)" text (format named by the maker's file-formats page). |
| Trace / sweep history as CSV, XML, JSON | **new file, built** | Our own documented schema (versioned), `src/core/sweep_history.*`. |
| Wide stitched sweep heatmap, click-to-tune | **new file, built** (model); **needs a forbidden file** for the screen and for feeding it from a live radio | `src/core/sweep_stitch.*`. Wiring into the main spectrum / a radio tuning loop touches `pipeline.cpp`/`engine.cpp`/`.slint`: hand-off below. |
| Occupancy / duty-cycle report | **new file, built** (host-side model, CSV export) | `src/core/sweep_occupancy.*`. |
| HackRF sweep mode | **blocked**, alternative built | See below. |
| Registering the tinySA as a source kind | **needs a forbidden file** (device registry/ledger) | The standalone source is built; the exact registration hand-off is below. |

### FREE-SOURCE CHECK, device by device

**tinySA / tinySA Ultra.** Opened: `https://tinysa.org/wiki/pmwiki.php?n=Main.USBInterface` (maker's wiki; no
licence or copyright notice on the page, only a "last modified" line; not inside a GPL repository). Used from
it, quoted exactly in `src/source/tinysa_protocol.hpp`: the `scanraw` usage line, the reply frame
`'{' ('x' LSB MSB)*points '}'` (16-bit little endian), the option word (1 unbuffered, 2 continuous), the
conversion "divide by 32 and subtract 128 for the tinySA and 174 for the tinySA Ultra". The tinySA firmware
(GPL) and every community wrapper were not read. NOT in the page, so not guessed and UNVERIFIED ON HARDWARE:
command terminator, echo and prompt before the `{` (the parser discards all bytes before it), reply format of
`version`/`info` (so the model is named by the caller), a scanraw point limit (we cap at 1 048 576), baud rate.
There is no checksum in the frame; its checks are the framing bytes and the exact point count asked for.

**RF Explorer.** Opened: `https://j3.rf-explorer.com/rf-explorer-file-formats` (copyright "All rights
reserved" RF Explorer Technologies SL, no licence text; says the formats may change; names "Export Single Signal
CSV" as one `[frequency (MHZ), amplitude (dBm)]` pair per line and the "Cumulative CSV" whose header contents it
does not show; the .rfe layout is not described); the user manual PDF (`version.rf-explorer.com/docs/RF Explorer
Spectrum Analyzer User Manual.pdf`: licence section, says the firmware is copyrighted, the Windows client is
open source (GPL v3 in that text, LGPL elsewhere), "protected by multiple patents", no frame description);
`https://j3.rf-explorer.com/rf-api-scpi.html` (links to the API page; client LGPL); `https://www.rf-explorer.com/API`
(301 redirect to the GitHub wiki, redirect only, the wiki was NOT fetched); `https://github.com/RFExplorer/RFExplorer-for-.NET`
(licence field only: LGPL-3.0). DISCLOSURE: a web search result summary quoted a few words of that wiki (that
sweep bytes are halved and negated, start/end in kHz, step in Hz); nothing from it was used, and no RF Explorer
decoder is built. The "patents" line in the manual is noted; it is not analysed. Patent-free alternative for the
same result: the user exports a sweep from RF Explorer's own program and imports it (built); the cumulative CSV
is not importable because its header is undocumented.

**HackRF sweep.** Opened: `https://hackrf.readthedocs.io/en/latest/hackrf_one.html` and `.../hackrf_tools.html`
(no licence text on the pages; the documentation is built from the project's repository, GPL-2 firmware / BSD
host, so it is treated as closed for protocol details). Neither page describes the sweep USB request or its
block framing (the tools page gives only the command-line usage and the `hackrf_sweep` CSV line layout);
`.../libhackrf_api.html` returned 404. A web search only turned up the GPL firmware file and third-party host
crates (not opened, not read). So the sweep request is described only inside GPL source: **blocked**, driver not
edited. The same result for the user (a spectrum wider than the sample rate) comes from the stitcher below,
fed by ordinary retuned captures through the driver as it is.

(The remaining sections of this record are completed in the last commit.)
