# radio-bridges: radios through a vendor library the user installed, and the RF Explorer over its serial link

Lane `radio-bridges`, branch `feat/radio-bridges` (engine repository, cut from `engine/multi-radio` e7e25fc9), worktree `D:/foxsdr-wt/en-osslibs`, build `D:/foxsdr-build/en-osslibs` (Release, `-DCASCADE_BUILD_SLINT=OFF -DCASCADE_SOFTGL=OFF`). 2026-11-12. Nothing pushed. Everything is in NEW files except three one-place edits (listed under "Files"). NOTHING is registered with the engine, the Source menu or the settings: that is the merge lane's job (the REGISTRATION HAND-OFF below). None of these radios is on the desk and no radio, COM port or driver was opened: every bridge is proven against a STAND-IN written in this lane and is reported UNVERIFIED ON HARDWARE.

Owner decision (2026-11-12 21:50) that this lane works under: FoxSDR MAY load a vendor library the USER installed; it bundles, downloads and redistributes no vendor library and no vendor header. Declarations are written here from the vendor's published interface description, kept minimal, in our own header. Where that is not possible the radio is not built and the reason is given.

## Result per radio

| Radio | Result | What it needs | Evidence |
|---|---|---|---|
| 1. RFspace SDR-IQ | **built** | the user's FTDI D2XX driver (`ftd2xx.dll`, then `ftd2xx64.dll`), found at run time | `test_sdriq_d2xx` 287 checks, 0 failed; `test_sdriq_source` passes |
| 2. WiNRADiO G31DDC | **not built** (32-bit library only, per the maker's own page) | n/a | quotes below |
| 3. Fobos SDR | **not built** (no published interface description that is not LGPL/GPL source) | n/a | searches below |
| 4. Perseus | **not built** (same) | n/a | below |
| 5. RFNM | **not built** (unstable C++ API, no installable library exists) | n/a | below |
| 6. Harogic | **not built** (the guide gives neither enumeration values nor structure packing) | n/a | below |
| 7. RF Explorer | **built, as a serial sweep source (not a library bridge)** | its COM port; no library | `test_rfexplorer_source` 7406 checks, 0 failed |

## Step 0: what was known, what was read

Read first: `D:/foxsdr-build/prep/lane-results/engine-oss.md`, `docs/feature-records/oss-radios.md` and `oss-libs.md` (in the tree), the route notes under `D:/foxsdr-build/prep/oss-recheck/` (`USER-LIBRARY-LIST.md`, `BUILD-LIST.md`, `OWNER-DECISIONS.md`, `STILL-BLOCKED.md`, `second-pass.md` L-215/L-325), `vendor_lib` / `vendor_apartment` / `g33ddc_*` (the pattern: search rules, silent load, byte check, refusal of unsafe paths, one-thread apartment), and, read-only, `feat/wire-b:src/engine/wire_b_sources.{hpp,cpp}` and `wire_b_sweep.hpp` (the registration table and the sweep host that the hand-off is written against).

## What was built

### 1. SDR-IQ over FTDI's D2XX (`source/ftd2xx_api.hpp`, `source/sdriq_d2xx.{hpp,cpp}`)

* **Declarations** (`ftd2xx_api.hpp`): the eight calls used (`FT_ListDevices`, `FT_OpenEx`, `FT_Close`, `FT_Read`, `FT_Write`, `FT_SetTimeouts`, `FT_Purge`, `FT_GetQueueStatus`), the `FT_STATUS` values, the open/list flags and `FT_PURGE_RX/TX`, written from FTDI's own "D2XX Programmer's Guide" 2.01 (prototypes and the appendix of values, as printed). FTDI's header was not read. Receive side only: no EEPROM, baud-rate, modem-line, bit-bang or reset call is declared (a source-scan test pins it).
* **Library** (`openD2xxSdriq`): found as `ftd2xx.dll`, then `ftd2xx64.dll` through `vendorlib::findLibrary` (a folder or file the person gave, or the absolute PATH entries; never the current directory, never beside the executable), judged from its bytes (`checkFile`: a 32-bit or non-DLL file is refused before anything is mapped), loaded under the thread error mode that suppresses the system box, exports resolved with `resolveAll`. Absent, too old or wrong bitness: one plain sentence that names FTDI's D2XX driver and says to install it from FTDI's own web site (nothing is opened, downloaded or bundled by FoxSDR on its own: the page's buttons act only on the user's click), and never names the folder. The test shows the loader is not even asked for a missing or non-image file and is asked once, with the silent mode set, for a too-old library and for one with a missing dependency (error 126).
* **Which device**: with a serial given it is opened as given (the source then asks the device its name and refuses anything that is not an "SDR-IQ"). With none given, only a device whose D2XX description contains "SDR-IQ" (any case) is chosen, so FoxSDR never writes to another FTDI device unless the person named its serial. A list string that overruns its buffer (canary bytes after it) or is unterminated refuses the library.
* **Link** (`D2xxLink`): `read` asks `FT_GetQueueStatus`, then reads what is queued or one byte with the timeout (a D2XX timeout of 0 means "wait forever" and is never passed: clamped to 1..60000 ms); a library that reports more bytes than were asked for is distrusted; a failed status makes the link dead (-1) for good; a fault inside FTDI's code is caught by `callGuardingVendorFaults` (the link is dead, the module is leaked, the application lives); `write` loops over partial writes, gives up after three zero-byte writes; `close` waits a bounded time (by the clock) for calls in flight, calls `FT_Close` (unless the library faulted) and leaks the module rather than unloading it under a call that never returned.
* **Edit to an existing file**: `defaultSdriqOpener()` in `sdriq_source.cpp` now calls `openD2xxSdriq` (so a default-constructed `SdriqSource` just works once the user's driver is on PATH); the header comments in `sdriq_source.hpp` say so; the production-opener case of `test_sdriq_source` empties PATH for its duration so it never touches a real driver.

### 7. RF Explorer as a sweep source (`source/rfexplorer_protocol.{hpp,cpp}`, `source/rfexplorer_serial_win.{hpp,cpp}`)

The RF Explorer is a serial sweep device, not a library: the maker's libraries are Python, .NET and Arduino code under the LGPL and nothing a native bridge can load (as `oss-libs.md` found). So it follows the tinySA pattern (`tinysa::SerialLink` + sweep lines), with:

* **The messages** (`rfexplorer_protocol`): written from the maker's "RF Explorer UART API interface specification" (read as a document) and cross-checked against the maker's 2013 `pyRFExplorer` client (Unlicense). Written: `#<size>C0` (request configuration), `D0` (disable screen dump), `C2-F:` start, end, top, bottom (32 bytes, the page's field widths and its own size figure), `CH` (hold). **Nothing else is ever written** (no generator, RF-power, baud, shutdown, reboot, wifi or tracking command: the source-scan test extracts every `frameCommand(` body and requires exactly `C0`, `CH`, `D0` and the one configuration format). Read: `#C2-M:` setup, `#C2-F:` configuration (three layouts by field count: 10, 11, 13), sweep frames `$S`, `$s`, `$z`, the `$D` screen dump (skipped), and any other `#` line (counted, ignored).
* **Parser** (`Demux`): incremental, any chunking; frames are delimited by their count and must end with one CR and one LF in either order (the page prints `0x0A, 0x0D` and says "CR + LF"); payload bytes that look like framing do not break a frame; a `#` that does not start one of the page's messages is noise (a stray one cannot swallow the next real line); a sweep before any configuration is never interpreted; bounded buffer; a 40000-chunk fuzz.
* **The documents disagree and both readings are accepted**: the `$S` count byte is the point count in the 2013 client but "increased by one and multiplied by 16" on the page (firmware column blank, word for word the same as for `$s`). The reading that equals the point count of the device's last configuration is used; a count that matches neither is a bad frame.
* **Sweeper**: `connect` (refuses a signal generator, a model 60, or a device not in spectrum-analyser mode, each by name; a silent device gives a sentence naming the 500000-baud default), `configure` (the range and span are checked against the minimum, maximum and span the device reported, before anything is written; sweeps of the old span still in flight are dropped when the new configuration arrives; a device that does not take the range is `Refused`), `nextLine` (frequencies and point count are the device's own configuration: start, step and points; the stop frequency is `start + step * (points - 1)`, NOT the requested end), `hold`. Every wait is bounded by idle reads and a byte budget.
* **Link** (`RfeSerialLink`): a Windows COM port opened at the baud of the page's list (default 500000; `serial_port_win.hpp` fixes 115200 and `core/serial_port.hpp` refuses above 115200, so neither can be used), overlapped bounded reads and writes with cancel on timeout. Its I/O is tested over a LOCAL NAMED PIPE (`openTestPipe` accepts only `\\.\pipe\foxsdr-rfe-` names; the same call sequence as `core/serial_port.cpp` uses for the GPS reader); no real COM port is opened by any test. The baud, 8N1 and DTR/RTS settings are therefore never exercised.

## Not built, and why

**G31DDC.** WiNRADiO's G31DDC pages (`g31ddc_sdk-doc.htm` and `developer-g31ddc.htm`, read 2026-11-12): "The WR-G31DDC API SDK is implemented as a 32-bit dynamic library (G31DDCAPI.dll)" and "The API can be used by any 32-bit application under Microsoft Windows 7, Windows 8, Windows 10 and Windows 11 (including their 64-bit versions)". The G33DDC page, by contrast, adds "The API can be used also by any 64-bit application but only under 64-bit versions of the supported operating systems"; the G31DDC page has no such sentence. FoxSDR is a 64-bit process and `vendor_lib` refuses a 32-bit library by design, so there is nothing to load: stopped, as the brief says. The calls match the G33DDC's (`oss-libs.md`), so a 64-bit file from WiNRADiO would be a table entry. Routes that do not need it: WiNRADiO's ExtIO driver through the existing ExtIO client (which loads 32-bit drivers in a helper process; UNVERIFIED), or a 32-bit helper process for this DLL (a new piece of work: owner decision 1).

**Fobos SDR.** The maker's own documents (data sheet, user guide, firmware-update manual on rigexpert.com) carry no API description; they point to the repositories. Every repository of the maker is LGPL-2.1 (`rigexpert/libfobos`, `libfobos-sdr-agile`) or GPL-3.0 (`rigexpert/fobos-sdr`), checked with the GitHub API (`.license.spdx_id`), and the standard rules forbid opening LGPL/GPL source or headers in this lane. Searches: `gh api repos/rigexpert/{fobos-sdr,libfobos,libfobos-sdr-agile}` (licence, file listing); web search `site:github.com "fobos_rx_read_sync" MIT OR BSD OR Apache` and `Fobos SDR API documentation fobos_rx_open` (nothing but those repositories and forks of them). Existing routes: `SoapyFobosSDR` as a SoapySDR module and `ExtIO_FobosSDR` through the ExtIO client (neither tested).

**Perseus.** Web search for a programmer's guide, `perseus-sdk` documentation and the function names (`site:github.com "perseus_set_ddc_center_freq" "perseus_start_async_input"`): no document. The only open descriptions are the LGPL library and GPL programs (SDRangel, gr-osmosdr: not opened). `nmaster2042/SoapyPerseus` (MIT) was re-read (the permissive wrapper): it shows the call names and the order of use but not the callback type, the sample layout (it points at the LGPL `perseus_test.c`), the argument types of the tuning call or the enumerations, so a declaration from it would be a guess that corrupts memory in the real library if wrong.

**RFNM.** The `rfnm/librfnm` README (documentation; MPL-2.0, outside the readable licences): "THE API DEFINITION IS NOT YET STABLE", a C++ driver built with vcpkg's libusb and spdlog, and a GitHub release list that is empty (`gh api repos/rfnm/librfnm/releases` returns 0): there is no installable library for a user to have. A C++ class library with an unstable API cannot be bridged through a small C declaration. Unblock: RFNM publishing a stable C interface and binaries.

**Harogic.** The maker's programming guide (`LAMBDA_API_ProgGuide.pdf`, 7565 lines, read again for this lane) documents the C functions (`int Device_Open(void** Device, int DeviceNum, const BootProfile_TypeDef*, BootInfo_TypeDef*)`, `IQS_*`, `SWP_*`, `RTA_*`) and lists structure members, but (a) the enumeration choices are printed as a numbered list ("1) USB ... 5) VIRTUAL") while its own example writes `const IPVersion_TypeDef ETH_IPVersion = 0;` for the first entry "1) IPv4", so the printed numbers are not the values; (b) no structure packing or alignment is stated; (c) the I/Q stream structures are large. A bridge would pass guessed values and layouts to a real library. A web search `site:github.com Harogic "Device_Open" "IQS_GetIQStream"` found no permissive header. Unblock: Harogic publishing the values, or the owner letting a lane read `htraapi.h` from the user's own SDK folder.

## Tests (real result lines)

`D:/foxsdr-build/en-osslibs/tests/Release/<name>.exe`, Release; also through `ctest -C Release -R ... --timeout 300`.

| Test | Result line |
|---|---|
| test_sdriq_d2xx | `test_sdriq_d2xx: 287 checks, 0 failed` (220 before the needed-library data, 67 after) |
| test_rfexplorer_source | `test_rfexplorer_source: 7406 checks, 0 failed` (4 consecutive green; ctest 1.4 s) |
| test_sdriq_source | Passed (production-opener case now with PATH emptied) |

What `test_sdriq_d2xx` covers (stand-in `ftd2xx.dll` built from the same guide, `tests/fixtures/ftd2xx_standin.cpp`, with a small SDR-IQ behind it): absent library (empty folder, nonexistent path, empty PATH) with the loader not asked and no folder in the sentence; a text file; a 32-bit image; a too-old library; a library with a missing dependency (126); the second file name and a file path; no devices; a failing list call; a driver that writes past its buffer or leaves a string unterminated; other FTDI devices only (counts in the sentence); the SDR-IQ third of three; a named serial opened as given; a refused open; refused timeouts; handles balanced after every refusal; the name request and its byte-for-byte reply; a read of nothing; the timeout-0 and timeout-huge clamps; `interrupt`; write after close; double close; a failing write; a write that writes nothing (gives up inside 2 s); a write that takes 3 bytes at a time (40 bytes arrive); a failing read, a read that lies, a pulled cable; a read that faults (the link fails, the process lives, the next link works); the finished `SdriqSource` tuning, streaming and finding its tone where the 1500 ppm sample-clock error puts it (`12345.6 / 1.0015`, and not at 12345.6), 10 start/stop cycles, no forbidden item ever written, no read larger than the reader's buffer; a wrong serial; a device that calls itself something else; a radio that answers nothing; the cable pulled mid-stream (fault reported, clean close); the production opener through PATH and with PATH empty; the source scan; and a read that never returns (close is bounded, 1.5 s wait).

What `test_rfexplorer_source` covers (stand-in device with noise, an off-grid tone and a step that is not the nominal one): the 32-byte command of the page byte for byte, rounding to kHz, nine refused ranges; the three configuration layouts, blanks, eight malformed configurations, a 20000-case fuzz; setup parsing; the page's `0x11 = -8.5 dBm`; every frame kind (`$S` both count readings, `$s`, `$z`), both end-of-line orders, four chunkings; a sweep before a configuration; payload bytes that look like framing; noise, other `#` lines, generator lines and a screen dump; wrong-count frames and frames without their end of line; a 40000-chunk fuzz; the sweeper through connect / configure / 21 lines (tone within one step of where it was put, stop computed from the device's step), stale sweeps dropped, optional setup, a silent device, a generator, a wrong mode, model 60, a failing write, a failing read, a pulled cable, out-of-range and over-wide spans refused with nothing written, a device that ignores the command, a flood; the serial link over a pipe (names and bauds refused before the OS is asked, 15 lines end to end, a quiet read returns 0 at its timeout, the pipe closed by the device end, a 100 kB write split across the pipe's buffer); the source scan.

### Break-it table (change the named line, see RED, restore, GREEN; `D:/foxsdr-build/rb/breakit.py`)

| Part | Changed | Result with it changed |
|---|---|---|
| SDR-IQ link | the read timeout clamp (`std::clamp(timeoutMs, 1u, 60000u)`) removed | 2 failed (a zero timeout reaches the library; the huge one is not clamped) |
| SDR-IQ link | the "described as an SDR-IQ" filter made `return true` | 10 failed (a device that is not an SDR-IQ is chosen; handles not balanced) |
| RF Explorer | the CR LF end-of-line check removed | 1 failed (`badFrames >= 3`) |
| RF Explorer | the `$S` direct-count reading removed | 10+ failed (the first sweep is never decoded) |
| RF Explorer | the generator / model-60 refusal removed | 1 failed (`sw2.connect ... NotAnalyzer`) |
| SDR-IQ needed-library check | the "does not offer all the calls" (too old) branch removed | 1 failed (`TooOld` and the missing list) |

A slip of my own: the first run of the `$S` break-it crashed the test process (exit 139) instead of failing a check, because a test indexed `line.db[peak]` of an empty line; the test now guards the indexing, and the mutant then fails with named checks.

## UNVERIFIED (everything not run)

* **Every bridge on real hardware and a real driver.** No SDR-IQ, FTDI driver or RF Explorer was touched; the stand-ins were written from the same documents, so they show the documents were followed, not that they were read rightly.
* **D2XX**: the handle is declared pointer sized (the 2002 guide says 4 bytes; a 64-bit program needs more); the guide read is version 2.01 (FTDI's own site refused every current edition to this tool: 403), so newer editions were not compared; that the 64-bit driver installs `ftd2xx.dll` (or `ftd2xx64.dll`) in a PATH directory; that a real SDR-IQ's FTDI description contains "SDR-IQ" (the interface specification does not print it; `sdriq=<serial>` is the escape).
* **RF Explorer**: the `$S` count reading (both are accepted), 8N1 and DTR/RTS (the page gives only the baud rate), how a positive `<Amp_Top>` is written (positive amplitudes are refused), that every firmware answers Request_Config with a configuration, the 500000-baud default on every model, and the whole serial link on a real COM port.

## Gate

The full headless gate of the build folder, `ctest -C Release -L "headless|slint" --parallel 4 --timeout 600`, run in the FOREGROUND in five slices (`-I 1,200`, `201,420`, `421,640`, `641,860`, `861,1100`) on the tip before this record, 986 tests in all (200 + 220 + 220 + 220 + 126), after a full `cmake --build . --config Release --parallel 4` (exit 0). Every red, judged alone:

| Test | Slice result | Judged |
|---|---|---|
| test_airband_data, test_band_plan, test_converter_call_sites, test_page_resize_id, test_png_decode | Failed | the known false failures of the brief |
| test_window_failed_app | Failed | configuration (this build folder has Slint OFF) |
| test_engine_api_zoom | Failed in the slice | passes alone (6.5 s): load flake |
| test_engine_main_smoke | Failed in the slice | passes alone (7.1 s): load flake |
| test_health_app_stepped | Failed in the slice | passes alone (54.8 s): load flake (on the brief's list) |
| test_engine_api_decode_radio (Timeout), test_engine_api_source_kinds | Failed | the git-ignored WAV fixtures of `src/slint/tests/drive/fixtures` were missing in this worktree (`the fixture scope.wav is not in ...`; `engine-merge.md` lists the same two for the same reason). They are generated by the repository's own `make_<name>.py` (`python -I make_scope.py`, `make_cw-decoder.py`, `make_pocsag-decoder.py`, `make_rtty-decoder.py`, `make_twotone-decoder.py`; standard library only, gitignored output). After generating them both pass: `test_engine_api_decode_radio` Passed 38.0 s, `test_engine_api_source_kinds` Passed 17.6 s. |

Everything else passed, including `test_shutdown_budget_free`, `test_thread_inventory`, `test_json_dump_policy`, `test_health_paths`, `test_engine_shell_guard` (no new file in `src/net`) and `test_sdriq_source`. No red is attributable to this lane. The needed-library data (`needed_library.*`, `checkFtd2xx`, 67 more checks in `test_sdriq_d2xx`) was added AFTER this gate; after it the five tests that cover the touched files were re-run and pass (`test_sdriq_d2xx` 287 checks, `test_sdriq_source`, `test_rfexplorer_source`, `test_json_dump_policy`, `test_thread_inventory`); the full gate was not repeated. `%APPDATA%/foxsdr/config.json`: sha256 `c21ce98dc30e50327f7666f7ee29047c0983d130ca3f21716d0c0e20e04603c3` at the end (unchanged; hashed only).

## Files

New: `src/source/needed_library.{hpp,cpp}` (the shared structured data), `src/source/ftd2xx_api.hpp`, `src/source/sdriq_d2xx.{hpp,cpp}`, `src/source/rfexplorer_protocol.{hpp,cpp}`, `src/source/rfexplorer_serial_win.{hpp,cpp}`, `tests/test_sdriq_d2xx.cpp`, `tests/test_rfexplorer_source.cpp`, `tests/fixtures/ftd2xx_standin.cpp`, this record. Edited: `src/source/sdriq_source.cpp` (the default opener calls the D2XX link: 5 lines) and `sdriq_source.hpp` (comments), `tests/test_sdriq_source.cpp` (the production-opener case empties PATH), `tests/CMakeLists.txt` (appended at the END between `# radio-bridges begin` / `end`: the stand-in `ftd2xx.dll` target and the two source-directory definitions). `CMakeLists.txt` was not touched (`src/source/*.cpp` is a glob). No file of the registry, `engine.cpp`, `pipeline.cpp`, `fox_engine_api.cpp`, `app_commands.*`, the config files or any `.slint` was touched; no operation number was added; `src/core/plugin_abi.h` untouched.

## REGISTRATION HAND-OFF (for the merge lane; nothing here is registered)

Modelled on `git show feat/wire-b:src/engine/wire_b_sources.cpp` (the I/Q table) and `wire_b_sweep.hpp` (the sweep host). Both sources are OFF until a person sends the id; constructing one loads and opens nothing.

### A. The I/Q table (`wire_b_sources.cpp`): one row and one include

```cpp
#include "source/sdriq_source.hpp"
...
    {"sdriq", "RFspace SDR-IQ (USB; needs FTDI's D2XX driver)", false, 196078.0, &makeOne<src::SdriqSource>},
```

| Field | Value |
|---|---|
| kind (driver key, `open-sdriq:`) | `sdriq` (`SdriqSource::driverKey()`) |
| display name | "RFspace SDR-IQ (USB; needs FTDI's D2XX driver)" |
| `needsAddress` | `false` (arguments are options only; may be empty) |
| `requestRateHz` | 196078 (the radio's power-up rate; the supported rates are 8138, 16276, 37793, 55556, 111111, 158730, 196078 from `supportedSampleRatesHz()`) |
| settings keys | `sdriq=<serial>` (empty or absent = the first device described as an SDR-IQ), `timeout=<ms>` (100 to 60000, default 3000: how long a control reply may take) |
| other controls | frequency 0 to 30 MHz (range from the radio), "RF gain" 0, -10, -20, -30 dB, "IF gain" 0 to 24 dB in 6 dB steps (from `gains()`) |
| health | `dataBlocks()`, `otherBlocks()`, `overloadReports()`; `lastError()` carries the sentences below |
| remote sessions | refuse the id (it loads a library on this machine) |
| threads | the one reader thread of `NetSampleSource` (no new thread kind) |

The library is found through PATH only (System32 is on it). If a "folder of the driver" setting is wanted, the row's `make` must see the arguments: `static_cast<SdriqSource*>(s)->setOpener(d2xxSdriqOpener(<folder>))` before `open()` (the opener takes the folder by value; the source's own `open` ignores unknown keys, so a `lib=` key can sit in the argument list). Not done here: it needs the registration code.

English text for the radio setup (`core/radio_setup.cpp` was not edited; no translation was made): "The SDR-IQ needs FTDI's D2XX driver (ftd2xx.dll). Install it from FTDI's web site, ftdichip.com (FoxSDR does not include it and does not download it), plug the SDR-IQ in, then choose it." The sentences the source can return, English, for the status line: "The SDR-IQ talks through FTDI's D2XX driver (ftd2xx.dll). FoxSDR did not find it. Install FTDI's D2XX driver from FTDI's own web site (ftdichip.com): FoxSDR does not include it and does not download it." / the same with "this is a 32-bit library and FoxSDR is 64-bit" (from `vendor_lib`) / "That ftd2xx.dll does not export what FoxSDR needs (...): it is not a D2XX library this build knows." / "No SDR-IQ was found: FTDI's D2XX driver lists no FTDI USB device. Plug the SDR-IQ in and try again." / "No SDR-IQ was found: FTDI's driver lists N FTDI USB device(s), none described as an SDR-IQ. If yours is listed under another name, give its serial number (sdriq=<serial>)." / "No FTDI device with that serial number is connected." / "FTDI's driver could not open the SDR-IQ (...): is another program using it?" / "FTDI's driver opened the SDR-IQ but would not accept its settings (...)." / "FTDI's D2XX driver failed while listing / opening the SDR-IQ; unplug it, plug it in again and retry." / "FTDI's D2XX driver wrote outside the buffer it was given; it is not used."

### B. The sweep host (`wire_b_sweep.{hpp,cpp}`): one more kind, like `startHackRf`

| Field | Value |
|---|---|
| kind | `rfexplorer` |
| display name | "RF Explorer (spectrum analyser, sweep)" |
| settings | "Port" (`COM1` to `COM256`, no default: nothing opens until it is typed; never logged or put in a fact), "Baud" (default 500000; only the page's list: 500000, 115200, 57600, 38400, 19200, 9600, 4800, 2400, 1200), "Start (MHz)" / "Stop (MHz)" (whole kHz after rounding; start below stop; limits come from the device and are refused with a sentence), "Top (dBm)" default -10 and "Bottom (dBm)" default -120 (-500 to 0, bottom below top). The number of points is the device's own (shown, not set). |
| worker | `RfeSerialLink link; link.open(port, baud, 100, why)`; `Sweeper sw(link); sw.connect(30, &why)`; `sw.configure(Request{startHz, stopHz, top, bottom, 30}, &why)`; loop `sw.nextLine(line, hostClockSeconds, 30, &why)`; feed the line to the heatmap and occupancy exactly as the tinySA lines are; on `Result::Ok` count a sweep, on `Refused` count and go on, on `TimedOut` / `LinkFailed` / `NotAnalyzer` close and set ONE health sentence (`resultWord()` plus `why`; no port name); at stop `sw.hold()` then close the link |
| factory | the engine's default builds `RfeSerialLink` (`open` validates the name and the baud before the OS is asked); tests give a stand-in `tinysa::SerialLink` |
| health | `sw.stats()` (`sweeps`, `badFrames`, `discardedBytes`, `badConfigs`, `ignoredLines`, `screenDumps`, `generatorMessages`) |
| remote sessions | refuse (it opens a port on this machine) |
| threads | the sweep host's one worker (no new thread kind) |

English text for the radio setup: "Connect the RF Explorer by USB and choose its COM port (Windows shows it as a USB serial device; if no COM port appears, install the maker's USB driver). FoxSDR talks to it at 500000 baud, the maker's default, and uses the spectrum analyser models only." The sentences the sweeper returns: "no RF Explorer answered: check the port, that it is switched on and that its baud rate is the default (500000)" / "this is an RF Explorer signal generator; FoxSDR uses the spectrum analyser models" / "the RF Explorer is not in spectrum analyser mode; switch it to the spectrum analyser on the device" / "that range is outside what this RF Explorer covers (A to B kHz)" / "that span is wider than this RF Explorer's maximum of N kHz" / "the RF Explorer did not take that range" / "the serial port failed (the RF Explorer was unplugged or the port closed)" / "the sweep is not usable: ..." and from the link: "the port name must be COM1 to COM256" / "there is no such port" / "the port is in use by another program".

### C. Radios not built: nothing to register

G31DDC, Fobos, Perseus, RFNM, Harogic: no row. (If the owner decides a 32-bit helper process for G31DDC or the SDK-header reading for Fobos / Perseus / Harogic, each is a new lane.)

## NEEDED LIBRARIES (owner rule 2026-11-12 22:00, `NEEDED-LIBRARY-RULE.md`; replaces the "text only" sentence of the brief)

Added after the lane's first report, on the orchestrator's message. Nothing is downloaded, bundled or opened by FoxSDR on its own; the buttons below act only on the user's click and are drawn by the radio-setup page (a screen lane's work).

| Feature | Library id | Library | File names looked for | Minimum version | Official page | Install kind | Copy-in folder | Steps (English) | Licence | fetch allowed |
|---|---|---|---|---|---|---|---|---|---|---|
| SDR-IQ radio (`sdriq`) | `ftdi-d2xx` | FTDI D2XX driver, 64-bit | `ftd2xx.dll`, then `ftd2xx64.dll` (PATH directories, or the folder or file the user chose) | none known: the eight classic calls are the oldest ones, so "too old" means one of them is missing; 64-bit | `https://www.ftdichip.com` is the only address read in FTDI's own documentation (the D2XX Programmer's Guide says "our web site www.ftdichip.com"); the D2XX download page itself was NOT read (FTDI's site refused the tool, 403): **(unverified)**, not given | installer | none | 1. Open FTDI's web site (button) and find the D2XX driver for Windows, the 64-bit one. 2. Download it and run FTDI's installer (Windows may ask for permission: that is FTDI's installer, not FoxSDR). 3. Unplug the SDR-IQ and plug it in again. 4. Press "Check again"; FoxSDR does not need to be restarted. | (unverified: FTDI's driver licence was not read); FoxSDR does not include or supply it | `no (unverified)` (terms not read) |
| RF Explorer sweep source (`rfexplorer`) | none | no library: a COM port. Whether Windows needs a USB-serial driver for it was not read in the maker's documents: (unverified), nothing is claimed | - | - | - | - | - | - | - | n/a |
| G31DDC, Fobos, Perseus, RFNM, Harogic | (not built) | G31DDCAPI.dll (32-bit only), libfobos, libperseus, librfnm, the Harogic HTRA library | - | - | - | - | - | - | LGPL-2.1 / LGPL-3.0 / MPL-2.0 / the maker's own; none read in full | `no (unverified)` |

**Structured data from the bridge** (`source/needed_library.hpp`, filled by `ftd2xxNeededInfo()` and `checkFtd2xx()` in `sdriq_d2xx`): `Info` (static: `libraryId`, `name`, `fileNames`, `minVersion`, `needs64Bit`, `why`, `publisher`, `officialAddress` (empty when unverified), `installKind`, `copyInFolder`, `steps`, `licence`, `notSuppliedLine`, `fetchAllowed`) and `Check` (the answer of "Check again": `state` as one of `working`, `not-found`, `too-old`, `wrong-32-bit`, `would-not-load`, `not-a-library`; `fileName`; `version` read from the file's own version resource without running its code; `lookedIn` ("the folders on your PATH (System32 is one of them)" or "the folder or file you chose": categories, never folders); `foundFolder` (structured only, for "Open that folder"; never in a sentence); `missing` (the calls a too-old file lacks); `sentence`). The address shown and opened must come from the link registry by `libraryId` `ftdi-d2xx`; `officialAddress` here is only the seed for the registry's built-in copy.

**Check again works without restarting FoxSDR**: each call finds, judges, loads silently and unloads the library, keeping nothing. Tested (`testNeededLibrary`): library absent -> `not-found`; the stand-in copied in ("installed") -> the same call says `working` and the radio then opens in the same process; the file can be deleted afterwards (nothing keeps it loaded) and the next check says `not-found` again; also through PATH; a check opens no device and calls none of the library's functions (`kOpens == 0`, `kCalls == 0`); each wrong state (text file, 32-bit image, a library that offers too little, one with a missing dependency) has its own state and a sentence naming the file and no folder; the file version of `kernel32.dll` is read.

**SCREEN HAND-OFF (the radio-setup page; the controls are the shared component's, not this lane's)** for a radio whose bridge has a `needed::Info`:

| Control | English label | What it does | Data it uses |
|---|---|---|---|
| Text block | "Needs: FTDI D2XX driver (ftd2xx.dll or ftd2xx64.dll), 64-bit" / "Why: ..." / licence line / "FoxSDR does not include or supply this driver." | shows items 1, 2 and 6 of the rule | `Info.name`, `fileNames`, `needs64Bit`, `minVersion`, `why`, `licence`, `notSuppliedLine` |
| Text | the official page as copyable text | shows the address from the registry; when the registry marks the link broken or moved, says it may be out of date, names the publisher and falls back to the steps | registry lookup by `Info.libraryId`; `Info.publisher` |
| Button | "Open the download page" | opens the user's browser on the registry address, on the click only | registry address; hidden while the address is unverified |
| Numbered list | the steps | shows `Info.steps` | `Info.steps` |
| Button | "Open that folder" | for a copy-in library only; D2XX is an installer, so it is not shown for this row (`copyInFolder` is empty) | `Info.copyInFolder`, `Check.foundFolder` |
| Button | "Choose the file..." | for a copy-in library only; not shown for this row. (If the owner wants it for D2XX, the page checks the chosen file with `checkFtd2xx(<file>)` before copying: it already accepts a file path and refuses with a sentence.) | `checkFtd2xx` |
| Button | "Check again" | calls `checkFtd2xx()` (with the chosen folder or file when there is one) and shows `Check.sentence` with its state word | `Check` |
| Status line | "Found and working: ftd2xx.dll (version ...), 64-bit ..." / "Not found: ..." / "... too old ..." / "... 32-bit ..." / "... would not load ..." | the result of the last check | `Check.state`, `sentence`, `version` |

The same Info/Check shape is what any later bridge fills in (`needed_library.hpp` is not SDR-IQ specific). There is no ABI request from this lane: the page is in the engine's own screen, not a plugin.

## Sources opened (2026-11-12)

| URL | Licence or status | Used for |
|---|---|---|
| https://www.mikrocontroller.net/attachment/57942/D2XXPG21.pdf (a copy of FTDI's "D2XX Programmer's Guide" 2.01, 2002; saved as a document to the scratch folder, read with `pdftotext`) | FTDI's own document, copyright FTDI, no licence for redistribution seen; read only | every D2XX prototype, status code, flag and purge value |
| https://www.ftdichip.com/Support/Documents/ProgramGuides/D2XX_Programmer's_Guide(FT_000071).pdf and `ftdichip.com/wp-content/uploads/2023/09/D2XX_Programmers_Guide.pdf` | HTTP 403 to both the fetch tool and `curl`; NOT circumvented | (the current guide, 1.5, was therefore not opened) |
| https://winradio.com/home/g31ddc_sdk-doc.htm, `developer-g31ddc.htm`, `developer-g33ddc.htm` | WiNRADiO's documents, no licence or agreement stated | the 32-bit statements quoted above |
| https://lambdaphoto.co.uk/pdfs/HAROGIC/LAMBDA_API_ProgGuide.pdf (7565 lines) | maker's guide, no licence text seen | the Harogic decision |
| https://github.com/RFExplorer/RFExplorer-for-.NET/wiki/RF-Explorer-UART-API-interface-specification (raw text of the wiki page) | the maker's interface document, in the wiki of an LGPL-3.0 repository; the page states no licence ("believe to be accurate" disclaimer only); read as a DOCUMENT, no source file of the repository was opened | every RF Explorer message, field width and unit |
| https://github.com/RFExplorer/pyRFExplorer (`LICENSE.md`, `pyrfe/rfe.py`, commit 59de6166, 2013) | Unlicense | cross-check of 500000 baud, command framing and the `$S` frame; nothing copied |
| https://www.site.uottawa.ca/~sloyka/elg4179/RF%20Explorer%20Spectrum%20Analyzer.pdf (the maker's manual, a course copy) | maker's manual | "USB Communication protocol is open"; baud default 500 Kbps; points to the interface page |
| https://github.com/rfnm/librfnm (README, listing, releases) | MPL-2.0, README read as documentation only | the RFNM decision |
| https://github.com/nmaster2042/SoapyPerseus (`SoapyPerseus.hpp/.cpp`, `Registration.cpp`) | MIT | the Perseus decision; nothing copied |
| GitHub API: `repos/rigexpert/{fobos-sdr,libfobos,libfobos-sdr-agile}` (licence, listing); `repos/rfnm/librfnm/releases` | as listed | Fobos and RFNM decisions |
| Web searches (standard): `FTDI D2XX Programmer's Guide ...`; `Fobos SDR API documentation fobos_rx_open ...`; `Perseus SDR programmer's guide perseus_open ...`; `"perseus-sdk" OR "perseus_sdk" ...`; `Harogic HTRA API programming guide ...`; `RF Explorer serial protocol documentation ...`; `RFNM librfnm C API documentation ...`; `site:github.com "fobos_rx_read_sync" ...`; `site:github.com "perseus_set_ddc_center_freq" ...`; `site:github.com Harogic "Device_Open" ...` | search results only | the search rule (Chrome / Google was not driven: the web search tool and `gh api` were used, as the previous lane did) |

Disclosures. (1) Documents were fetched into the session scratch folder (outside the repository) as files to read: the D2XX guide, the Harogic guide, the RF Explorer manual, the WiNRADiO pages, the RF Explorer wiki text and `rfe.py`; no software, library, driver, archive or script was downloaded, and nothing third-party was executed. (2) No LGPL, GPL or MPL source file was opened; the one LGPL-repository page read is the RF Explorer wiki (a document). (3) No radio, COM port, FTDI device or real driver was opened by any test; the real `%APPDATA%/foxsdr/config.json` was only hashed. (4) No Windows dialog was raised; every library load in a test goes through the silent loader.

## Owner decisions still open

1. **G31DDC**: WiNRADiO ships it as a 32-bit library only (per its own page). A 32-bit helper process for `G31DDCAPI.dll` (like the ExtIO helper) is a new piece of work; or point G31DDC owners at WiNRADiO's ExtIO driver. Someone with the installer can confirm in seconds whether a 64-bit `G31DDCAPI.dll` exists (its PE header).
2. **Fobos and Perseus**: may a lane read the public header (`fobos.h`, `perseus-sdk.h`) of the library the USER installed, for function and structure declarations (not copy them)? Without it both stay on their SoapySDR / ExtIO routes.
3. **Harogic**: may a lane read `htraapi.h` from the user's own SDK folder, or will Harogic publish the enumeration values and structure packing?
4. **RFNM**: waits for a stable C interface and binaries from RFNM (its network machine API, `rfnm/rfnmctl`, MPL-2.0, could be a separate lane).
5. **SDR-IQ driver page, licence and folder**: the D2XX download page and FTDI's driver licence were not read (FTDI's site refused the tool), so the registry row and the table cells say (unverified); PATH only, or a "driver folder" setting (needs the registration code to pass it to `d2xxSdriqOpener`)?
6. **SDR-IQ and RF Explorer radio-setup text** (English above) for the radio-setup owner to add and translate.
