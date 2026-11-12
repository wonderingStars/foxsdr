# oss-libs: radios reached through a library the user installed (run-time bridges) and the SDRconnect settings

Lane `osslibs`, branch `feat/oss-libs` (engine repository, cut from engine/multi-radio ba2a4679), worktree `D:/foxsdr-wt/en-osslibs`, build `D:/foxsdr-build/en-osslibs` (Release, `-DCASCADE_BUILD_SLINT=OFF -DCASCADE_SOFTGL=OFF`). 2026-11-12. Nothing pushed or merged. Every part is a stand-alone class with tests; NOTHING is registered with the engine, the Source menu or the settings: that is the wiring lane (hand-off below). None of these radios is on the desk: every bridge is proven against a stand-in library written in this lane and every family is UNVERIFIED ON HARDWARE.

## Step 0: the table (done first; what exists, where it belongs, what was read)

How the engine loads outside code at run time, found before writing anything: the SDRplay API is loaded in `sdrplay_source.cpp` by `LoadLibraryA` + `GetProcAddress` into a table (`loadInto`), with the API's own declarations in `sdrplay_api_decl.hpp`; SoapySDR modules are found and added to the search path by `soapy_modules.cpp` and every call into them is wrapped by `vendor_guard.hpp` (`callGuardingVendorFaults`); plugins are checked from their headers before loading (`core/plugin_module_check.hpp`) and loaded with a thread error mode (`core/plugin_host.cpp`); ExtIO drivers are loaded in a helper process chosen by the DLL's PE header (`extio_client.cpp`). None of these did all of (judge the file from its bytes, load silently, call on the one thread a maker requires, survive a crash or a hang inside the library); `vendor_lib` and `vendor_apartment` add that and reuse `vendor_guard`.

| Item | In the base? | Where it belongs | Read, and licence | Result |
|---|---|---|---|---|
| 1. SDRconnect remote source | BUILT already (`sdrconnect_source`, radio-sources lane, port inside `sdrconnect=host:port`) | host source | SDRplay's own PDF "SDRconnect WebSocket API" v1.0.3, re-read in full with `pdftotext` (no port, no stream rate in it; no licence text) | `iqrate=` setting added; port stays a setting |
| 2a. WiNRADiO G39DDC | no | host source + run-time bridge | WiNRADiO's HTML document `g39ddc_sdk-doc.htm`, read in full (no licence, no agreement) | BUILT |
| 2b. WiNRADiO G33DDC | no | host source + run-time bridge | `g33ddc_sdk-doc.htm`, same | BUILT |
| 2c. WiNRADiO G31DDC | no | - | `developer-g31ddc.htm`: "32-bit dynamic library", no 64-bit sentence | NOT BUILT (32-bit only, per the brief) |
| 2d. Fobos SDR (libfobos) | no | - | the maker's PDFs (FW update manual, user guide): API "descriptions, guides" are in the LGPL-2.1 repository only | NOT BUILT (owner decision 1) |
| 2e. Perseus (libperseus-sdr) | no | - | repository listing and licence only | NOT BUILT (owner decision 1) |
| 2f. RFNM (librfnm) | no | - | README only (MPL-2.0) | NOT BUILT (owner decision 2) |
| 2g. Harogic (HTRA API) | no | - | the maker's programming guide PDF (Lambda Photonics mirror, 7565 lines), read | NOT BUILT (owner decision 3) |
| 2h. RF Explorer sweep device | text import exists; native source no | - | repository listings; `pyRFExplorer` (Unlicense) `rfe.py` read | NOT BUILT as a bridge (nothing to load); see below |
| 3. Pluto / AntSDR, LimeSDR, BladeRF | Pluto native (`pluto_source`, IIOD); AntSDR through it | record only | none opened | recorded below; radio setup does not name them (finding) |

## What was built

### Part 1: `vendor_lib` (the loader every such bridge uses)

Files `src/source/vendor_lib.{hpp,cpp}`, test `tests/test_vendor_lib.cpp`. The file is judged from its BYTES before the loader is asked (`checkBytes`: DOS and PE headers, optional header, section table, every section's raw data inside the file, DLL bit, machine) with a fixed verdict word per refusal (`not-found`, `unreadable`, `not-an-image`, `truncated`, `not-a-library`, `wrong-32-bit`, `wrong-machine`, `load-failed`, `symbol-missing`) and one sentence. A 32-bit file in this 64-bit program is named plainly and is not loaded. `findLibrary` looks only at a path or folder the person gave, or at the absolute entries of PATH (never the current directory, never beside the executable; a name with a separator is refused). `Library::open` loads with the thread's error mode set to fail silently and restored afterwards and with only the DLL's own folder and System32 as places for its dependencies. Sentences carry the file's name, never its folder.

### Part 2: `vendor_apartment` (shared by the bridges)

Files `src/source/vendor_apartment.{hpp,cpp}`. The apartment is the ONE thread that makes every call into a maker's library (the WiNRADiO documents: a receiver "can be controlled from a single user thread only", the handle "can only be used in the same thread"). Callers hand it a task and wait a bounded time; a task that does not finish marks it dead, the thread is abandoned past its join bound (`Recovered::ReaderLeak`, row added to `test_health_paths`, thread row added to `thread_inventory_rows.inc`) and the owner leaks the library; every task runs under `callGuardingVendorFaults`, so an access violation inside the vendor's code is a refusal. Tasks capture by value. `StreamLink` is the ring the library's callback thread writes: it drops and counts impossible arguments and converts 16- and 32-bit I/Q.

### Part 3: WiNRADiO G39DDC, `G39DdcSource` (key `g39ddc`)

Files `g39ddc_api.hpp` (the thirteen calls, declared from the document), `g39ddc_source.{hpp,cpp}`, test `tests/test_g39ddc_source.cpp`, stand-in `tests/fixtures/g39ddc_standin.cpp`. Opens a receiver by the serial read from `GetDeviceList` (the document defines `G39DDC_OPEN_FIRST` and the attenuator constants only by name; their numbers are in the maker's header, which was not opened, so they are not used and no attenuator is offered); refuses a library whose device record is not 315 bytes; powers on; registers the DDC1 callback; lists the DDC types (skipping rate 0 and widths other than 16 and 32); streams DDC1 of one channel; tuning puts the nearest multiple of the front-end step on the front end and the rest on DDC1 (the document's formula, absolute DDC1 = front end + relative DDC1); a rate change stops, sets the DDC type (the document: it fails while streaming) and restarts; the preamplifier (a plain BOOL) is the one gain; a silent stream and a disconnect (`IsDeviceConnected` zero with no error) are faults with their own sentences.

### Part 4: WiNRADiO G33DDC, `G33DdcSource` (key `g33ddc`)

Files `g33ddc_api.hpp`, `g33ddc_source.{hpp,cpp}`, test `tests/test_g33ddc_source.cpp`, stand-in `tests/fixtures/g33ddc_standin.cpp`. Same design, differences from the two documents: `GetDeviceList` returns the count and reports no item size (so the list is read into a buffer with slack and a sentinel and every record is checked; a library that overruns the buffer or describes receivers in another layout is refused), no channel numbers, one absolute DDC1 frequency (no front end), a callback without a channel. The document gives no tuning range, so none is claimed; the first tuning at open is best effort and a later refusal comes back as a sentence.

### Part 5: SDRconnect `iqrate=`

`sdrconnect_source.{hpp,cpp}`, test `test_sdrconnect_source.cpp`. SDRplay's document gives neither the TCP port nor the I/Q stream's rate (it lists `device_sample_rate` as the hardware rate and an "IQ Lite" mode that may decimate). The port was already a setting; the stream rate now is: `iqrate=<Hz>` (1000 to 1e9) fixes the rate the stream is treated as having, `setSampleRateHz` accepts only that value, the device's own rate is untouched. Without it nothing changes.

## Not built, and why

All four are bridges to a library whose only description is its own LGPL-2.1 / MPL-2.0 source or a vendor document that does not give the C ABI; the lane rule forbids opening LGPL source, and a signature guessed from another program's use is not a declaration (a wrong one corrupts memory in the real library).

* **Fobos SDR (libfobos, LGPL-2.1).** Searched: `gh api search/code fobos_rx_*` (every hit is the maker's LGPL repositories, GPL programs and forks of them (SDR++, SatDump, SDRangel, ka9q-radio: not opened), or sdroxide and sweeppp, whose licences the API showed as GPL-3.0), `search/repositories "fobos sdr"` / `libfobos` (all LGPL or GPL; the SDR# plugin has no licence). The maker's own documents (Fobos SDR FW update manual, user guide, datasheet; https://www.rigexpert.com/files/product_documentation/Fobos%20SDR/ lists five files) say the API's "source codes, descriptions, guides" are in https://github.com/rigexpert/libfobos . Existing routes with no new code: `rigexpert/SoapyFobosSDR` (LGPL-2.1, listed by the GitHub API) as a SoapySDR module the user installs; `rigexpert/ExtIO_FobosSDR` (the maker's ExtIO plug-in, listed by the GitHub API; its release files were NOT opened) through the existing ExtIO client. UNVERIFIED that either works with FoxSDR.
* **Perseus (libperseus-sdr, LGPL-3.0).** Permissive code that calls the library does exist: `nmaster2042/SoapyPerseus` (MIT, 2026, one author) and `Cloud-SDR/CloudSDR_PerseusSDR` (BSD-2-Clause, 2016, which carries a copy of the LGPL library inside its tree). The MIT wrapper's source (commit 6df567e8) was read: it shows the function names and their order of use (`perseus_init`, `perseus_open`, `perseus_firmware_download`, `perseus_set_sampling_rate`, `perseus_set_ddc_center_freq`, `perseus_start_async_input`...) but not the structure types, the callback's exact signature, the enumerations or the sample layout (its own comment points at the LGPL `perseus_test.c` for the layout). That is a hypothesis, not a declaration, so nothing was built. Existing route: that same wrapper as a SoapySDR module (MIT), not tested.
* **RFNM (librfnm, MPL-2.0).** Its README (documentation, read) says "THE API DEFINITION IS NOT YET STABLE", that the driver is C++ (it builds with spdlog) and that a versioned wire protocol gates mismatched pairs. A C++ class library with an unstable API is not loadable through a small C bridge, and MPL-2.0 is outside the lane's list of readable licences. `rfnm/soapy-rfnm` (MPL-2.0) exists as a SoapySDR module. UNBLOCK: RFNM's decision to publish a stable C interface.
* **Harogic (HTRA API).** The maker's programming guide (read) documents the C functions (`Device_Open(void**, int, BootProfile*, BootInfo*)`, `IQS_EZProfileDeInit`, `IQS_Configuration`, `IQS_GetIQStream`...) and lists the members of the structures it uses, but not their packing or the numeric values of its enumerations (`1) USB`... are list numbers, not values), and the I/Q stream structures are large. A bridge would pass structures built from guesses to a real library. The maker's header comes with the SDK the user installs; `HAROGIC-Technologies/soapy-htra` has NO licence (all rights reserved: not read) and `gr-htra` is GPL-3.0 (not read). UNBLOCK: the owner lets the lane read the maker's header from the user's own SDK folder (it would then be a declaration the user's own file backs) or Harogic publishes the values.
* **G31DDC.** See Step 0. The G31DDC calls match the G33DDC's, so a 64-bit file from the maker is a one-line table entry; today the loader would refuse the 32-bit file with its own sentence.
* **RF Explorer sweep device.** The maker's libraries are Python (`RFExplorer-for-Python`) and .NET (`RFExplorer-for-Windows`, `-for-.NET`), both LGPL-3.0: nothing a native bridge can load. The one permissive client, `RFExplorer/pyRFExplorer` (Unlicense, commit 59de6166, `pyrfe/rfe.py` read), speaks the 2013 serial protocol of the 500 kbit/s units (`$S` + count byte + `-dBm*2` bytes, `#C2-F:` with 13 fields marked partly "UNDOCUMENTED"); a source built from it would only suit that old firmware and is UNVERIFIED against anything current, so it was not built. The text-import route stays.

## Item 3: Pluto / AntSDR, LimeSDR, BladeRF

No native driver built, as instructed. What each needs:

| Radio | Route | The Soapy module it needs (the user installs it) | Checked |
|---|---|---|---|
| ADALM-Pluto | native `PlutoSource` (IIOD) in the base; no module needed | `SoapyPlutoSDR` (pothosware, LGPL-2.1) if the person prefers SoapySDR; its driver key `plutosdr` is already in `device_scan_plan.hpp` | repository exists (GitHub API) |
| AntSDR (E200, E310, E316) | `PlutoSource` with `host=192.168.1.10` (radio-families record: firmware forked from PlutoSDR's; UNVERIFIED that the IIO device names match) | same `SoapyPlutoSDR`, UNVERIFIED | - |
| LimeSDR | SoapySDR module of Lime Suite | `SoapyLMS7`, a folder of the `myriadrf/LimeSuite` repository (Apache-2.0; the GitHub API lists the folder, with Registration.cpp); installed with Lime Suite or PothosSDR (the existing radio-setup text says so) | the folder exists; the built module's file name was not checked |
| BladeRF | SoapySDR module | `SoapyBladeRF` (pothosware; its repository carries `LICENSE.LGPLv2.1`), which needs Nuand's libbladeRF | repository exists |

**Finding: the "radio setup" text names only Lime and USRP.** `core/radio_setup.cpp` has the family list rtlsdr, airspy, airspyhf, hackrf, sdrplay, usrp, lime and a Soapy-module sentence for usrp and lime ("Install PothosSDR or radioconda (both carry the SoapySDR modules)"); there is no family, no download address and no text for Pluto, AntSDR or BladeRF, so a BladeRF owner gets no guidance. Not edited here (the file belongs to the radio-setup owner and has translated strings); the exact English strings to add are in the hand-off. Whether PothosSDR / radioconda carry `SoapyBladeRF` and `SoapyPlutoSDR` was NOT verified (their package lists were not opened).

## Tests (real result lines)

`D:/foxsdr-build/en-osslibs/tests/Release/<name>.exe`, Release; also run through `ctest -C Release -R ... --timeout 300`:

| Test | Result line |
|---|---|
| test_vendor_lib | `test_vendor_lib: 54656 checks, 0 failed` (5 consecutive runs green) |
| test_g39ddc_source | `test_g39ddc_source: 1277 checks, 0 failed` (5 consecutive green; ctest 15 s) |
| test_g33ddc_source | `test_g33ddc_source: 406 checks, 0 failed` (5 consecutive green; ctest 14 s) |
| test_sdrconnect_source | `test_sdrconnect_source: 343 checks, 0 failed` (was 298; 5 consecutive green) |
| test_thread_inventory, test_health_paths, test_shutdown_budget | Passed (rows added: `vendor_apartment.cpp` thread, `ReaderLeak` once) |
| test_json_dump_policy | FAILED on the base commit: `VIOLATION ... src\core\bookmark_pack.hpp (line 82)`; not touched here |

What they cover. vendor_lib: every truncation point of a good image refused and none accepted, a 30000-case fuzz of the headers, 32-bit/64-bit/ARM/executable/inconsistent images, find rules including a relative PATH entry and the current directory, real loads of the stand-in libraries, text and empty and cut files refused without the loader being asked (the silent-load counter is unchanged), a library whose dependency is missing (error 126, thread error mode unchanged, the same file loads when the dependency is beside it). G39/G33: a tone at +5 kHz and -3.45 kHz found where the 1500 ppm clock error and the 12.3 Hz static error put it (and shown NOT to be where the nominal rate would put it), 16- and 32-bit, retune and rate change while running, the preamplifier, statistics, the one-thread rule counted INSIDE the library (one thread, never two calls at once, also with four threads calling concurrently while another reads), 40 start/stop cycles and 30 open/close cycles with opens balanced against closes, every refusal listed above, a silent stream, a disconnect, three kinds of bad callback arguments, a refused start that leaves the source usable, a crash inside the library (reported; a fresh source opens afterwards) and a call that never returns (the source gives up inside the bound, says so, and closes within 4 s), a scan that no new file writes to the console or error stream or into a setting.

### Break-it table (change the named line, see RED, restore, GREEN; script `D:/foxsdr-build/eol-breakit.sh`)

| Part | Changed | Result with it changed |
|---|---|---|
| G39 | the device-record size check removed | 8 failed (the 316-byte record is accepted: the sentence, the open/close balance) |
| G39 | DDC1 offset forced to 0 | 250 failed (`toneHz 5004.77 vs expectedHz 1559.96`) |
| vendor_lib | the 32-bit verdict branch removed | 4 failed (the sentence and the verdict) |
| G39 | the call run inline instead of on the apartment | the test process crashes (exit 139: the crash-containment case runs on the caller's thread); restored: 1277/0 |
| G33 | the list sentinel check removed | 5 failed |
| G33 | the record plausibility check removed | 5 failed |
| SDRconnect | the `iqrate` override removed from `sampleRateHz()` | 2 failed (`sampleRateHz()=2000000 vs 250000`) |

A slip of my own while doing this: the break-it script restores a file with `git checkout`, which also threw away the then uncommitted `iqrate` edit of `sdrconnect_source.cpp` after the first break-it run; it was redone and the commit amended before anything else was built on it.

## UNVERIFIED (everything not run)

* Every bridge against a real library and a real receiver: the stand-ins were written from the same documents, so they show the documents were followed, not that they were read rightly. In particular the real behaviour of `SetDDC1` while streaming, the order requirements around `SetPower`, `SetCallbacks` and `StartDDC1`, and what a real `GetDeviceList` returns.
* That WiNRADiO ships a 64-bit `G39DDCAPI.dll` / `G33DDCAPI.dll`: the pages say "32-bit dynamic library" and also that 64-bit applications can use it; which is true was not determinable without the files. If only a 32-bit file exists, the loader refuses it with the "32-bit library" sentence and the receivers stay reachable through WiNRADiO's ExtIO driver and the existing ExtIO client (also unverified).
* The G39DDC's `FrontEndWindowWidth` / bandwidth rule for the DDC1 offset (taken from the document's remark on `SetDDC1Frequency`) and its minimum/maximum front-end frequencies (read from the receiver).
* The SDRconnect port, WebSocket path, stream rate and scaling, as before.
* The full headless suite was NOT run (the brief says not to).

## Wiring and screen hand-off (nothing is registered)

Driver keys and args (`argValue` style):

* `g39ddc`: `g39ddc` args `lib=<folder or file>,serial=<9 chars>,channel=0|1,rate=<Hz>,timeout=<ms>,silence=<ms>`.
* `g33ddc`: `lib=<folder or file>,serial=<9 chars>,rate=<Hz>,timeout=<ms>,silence=<ms>`.
* `sdrconnect`: existing args plus `iqrate=<Hz>`.

Screen, per WiNRADiO source (English label, what it does, range, default): "WiNRADiO library folder" (`lib=`; empty = look in the PATH directories); "Receiver serial number" (`serial=`; empty = the first listed); "Channel" (G39DDC only: 0 or 1, default 0); "Sample rate" (the list from `supportedSampleRatesHz()`, default the entry nearest 192000); "Preamp" (a 0/1 step from `gains()`); "Call timeout" (advanced, `timeout=`, 100 to 120000 ms, default 5000); "Silence limit" (advanced, `silence=`, 200 to 600000 ms, default 5000). SDRconnect: "I/Q stream rate" (advanced, `iqrate=`, 1000 to 1e9 Hz, default empty = the device's rate). Health row: `G39DdcSource::stats()` / `G33DdcSource::stats()` (callbacks, sample pairs, pairs dropped, bad callbacks), `deviceDead()`, `faultedWhile()`, `openVerdictWord()`, `serialMasked()`. Threads: one per open bridge (the apartment) plus the library's own callback thread. Both sources are OFF until chosen; constructing one loads nothing (asserted). A person who picks one must be shown that the library runs with their rights (the same line as the ExtIO warning).

English strings to add to the radio-setup text (not edited here): for BladeRF, "Install Nuand's bladeRF driver and the SoapySDR module for it (SoapyBladeRF), for example through PothosSDR or radioconda, then unplug the radio and plug it back in."; for ADALM-Pluto, "FoxSDR talks to the Pluto directly over its network address; no driver is needed. To use SoapySDR instead, install the SoapyPlutoSDR module."; for AntSDR, "Give FoxSDR the AntSDR's network address (the factory address is 192.168.1.10) in the ADALM-Pluto source."; and the `lastError()`/sentence texts quoted in the sources.

## Sources opened (2026-11-12)

| URL | Licence or status | Used for |
|---|---|---|
| https://winradio.com/home/g39ddc_sdk-doc.htm (read in full, saved to a scratch folder outside the repository), https://winradio.com/home/developer-g39ddc.htm | no licence, no agreement stated | the whole G39DDC bridge |
| https://winradio.com/home/g33ddc_sdk-doc.htm, https://winradio.com/home/developer-g33ddc.htm | same | the G33DDC bridge |
| https://winradio.com/home/g31ddc_sdk-doc.htm, https://www.winradio.com/home/developer-g31ddc.htm | same | G31DDC decision |
| https://api.sdrplay.com/docs/SDRconnect_WebSocket_API.pdf (v1.0.3) | no licence text | confirming what the document does not give |
| https://lambdaphoto.co.uk/pdfs/HAROGIC/LAMBDA_API_ProgGuide.pdf | no licence text seen | Harogic decision |
| https://www.rigexpert.com/files/product_documentation/Fobos%20SDR/ (listing), the FW update manual and the user guide | manufacturer documents | Fobos decision |
| https://github.com/rigexpert/libfobos and `libfobos-sdr-agile` (README text through a fetch tool, LICENSE named by the page) | LGPL-2.1 | README and licence only; no source file was opened |
| https://github.com/nmaster2042/SoapyPerseus (LICENCE, `SoapyPerseus.hpp/.cpp`, `Registration.cpp`, README, commit 6df567e8) | MIT, copyright 2026 Nicolas | Perseus decision; nothing copied |
| https://github.com/rfnm/librfnm (README only) | MPL-2.0 | RFNM decision |
| https://github.com/RFExplorer/pyRFExplorer (LICENSE.md, `pyrfe/rfe.py`, commit 59de6166) | Unlicense | RF Explorer decision; nothing copied |
| GitHub API listings: `search/code` for `fobos_rx_*`, `perseus_start_async_input`; `search/repositories` for fobos, libfobos, perseus, libperseus, rfnm, librfnm, harogic htra, rf explorer; `orgs/rfnm`, `orgs/RFExplorer`; licences of rigexpert/*, pothosware/SoapyBladeRF, SoapyPlutoSDR, myriadrf/LimeSuite, LimeSuiteNG | as listed in the text | the search rule (no Chrome, no Google: the web search tool and `gh api` were used) |

Disclosures. (1) While checking the SoapyPlutoSDR repository a `grep` over its registration file (LGPL-2.1) printed two lines of that file to the screen (a comment and the line that registers the driver key `plutosdr`). Nothing was taken from it; the same key is already in this tree (`device_scan_plan.hpp`). (2) The WiNRADiO pages' SDK header zips (`g39ddcapi_h.zip` etc.) and the vendor DLL installers were not downloaded or opened. (3) No software, library, driver or archive was downloaded; the documents were fetched as text into the session scratch folder. (4) `%APPDATA%/foxsdr/config.json` was never opened for writing: sha256 at the end `c21ce98dc30e50327f7666f7ee29047c0983d130ca3f21716d0c0e20e04603c3` (unchanged). `C:/Users/steve/FoxSDR` and the other worktrees were not touched. No Windows dialog was raised; no test left a process running.

## Owner decisions

1. **LGPL libraries' public headers (Fobos, Perseus):** may the lane read `fobos.h` / `perseus-sdk.h` for function and structure declarations (not copy them), given the library is loaded at run time and never shipped? Without it both stay on their Soapy / ExtIO routes.
2. **RFNM:** MPL-2.0 is outside the readable list, and the library is an unstable C++ API; the board also has a network machine API (`rfnm/rfnmctl`, MPL-2.0) that a network source could use. Ask RFNM for a stable C interface or a written network-API description?
3. **Harogic:** may the lane read `htraapi.h` from a user's own SDK folder, or does Harogic publish the enumeration values and packing?
4. **WiNRADiO bitness:** does a 64-bit `G39DDCAPI.dll` / `G33DDCAPI.dll` exist? (Someone with the installer can say in seconds: it is the file's PE header; `checkFile` prints the verdict.)
5. **Radio setup text** for BladeRF / Pluto / AntSDR: add the strings above?
