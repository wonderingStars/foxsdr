# Unblocked desk lane (rows P17, W2, R8, N25, N2, R12)

Branch `feat/unblocked-desk`, from `engine/multi-radio` d3630d78. Build folder `D:/foxsdr-build/feat-desk`, Slint OFF. Every part is stand-alone
and nothing is wired; the one edit to existing code is part 5 (see its section). Owner rule that started the lane: whatever is not a patent or a
legal problem is built.

## 1. File drop onto the window (P17, W2) - `src/core/file_drop.{hpp,cpp}`, `src/core/file_kind.{hpp,cpp}`

**Built.** `FileDrop::attach(hwnd, callback, options)` registers a window to take files and hands the paths (UTF-8, at most `maxFiles` = 4096, with
the true total and a cut-off flag) to the callback on the window's own thread. `classifyFile` / `classifyDrop` name each file by CONTENT.

**The choice, justified.** Both Windows doors are built; the DEFAULT is OLE (`IDropTarget`): it needs no change to somebody else's window procedure
(a subclass of a window the Slint library owns is a lifetime hazard), it refuses a drag that holds no files while the pointer is still over the
window (the "no" cursor), and the CF_HDROP block it gets has no 260-character limit. WM_DROPFILES (`DragAcceptFiles` plus `SetWindowSubclass`) is
used where OLE cannot run on the thread (a multithreaded-apartment thread: `Auto` falls back, `Ole` refuses) or when asked for. A window that
already has ANOTHER owner's OLE target (winit registers one unless drag-and-drop is switched off in it) is a REFUSAL with a reason, not a fallback:
WM_DROPFILES never reaches a window whose OLE target belongs to somebody else, so falling back would "succeed" and receive nothing.
`replaceExisting` revokes the other target first (same thread).

**Kinds.** I/Q recording (RIFF/WAVE, 2 channels, PCM16 or float32; where the path is short plain ASCII the real `IqFileSource::open` is asked too
and its refusal wins), WAV, RF64, SigMF pair (a `.sigmf-meta` whose content is a metadata document with `global` holding `core:datatype`, beside its
`.sigmf-data`; a pair dropped as two files is one item), bookmark CSV and SDR# XML (`importFrequencyList`), plugin package (`checkModuleBytes`),
folder, missing, unreadable, unknown. The detail sentence never carries a path or a file name. Paths over 259 characters get the long-path prefix;
non-ASCII names work. `IqFileSource` itself opens with a narrow path (a non-ASCII or long path would fail there): the classifier does not use it for
such paths and says "I/Q recording" from the header alone. That limit of the existing player is NOT fixed here.

**Tests.** `tests/test_file_kind.cpp` 98 checks; `tests/test_file_drop.cpp` 68 checks on the hidden desktop (direct `IDropTarget` calls with a
hand-made data object, a REAL `WM_DROPFILES` with a real HDROP sent and posted, 3000 and 10000 files and the cap, a folder, a 300-character path read
by the classifier, a file that vanishes after the drop, unregister on detach, the window destroyed first in both modes, a callback that destroys its
own registration, a foreign target refused and then replaced, the wrong thread, an MTA thread, fuzz runs of the block parser, the WAV header parser
and the SigMF sniffer). Break-it: a wrong channel rule, no revoke on detach and no cap gave 13 failures.

**UNVERIFIED.** A real drag from Explorer (needs a pointer); behaviour beside winit's own target (not built here: Slint is OFF); an elevated window
with a non-elevated Explorer (UIPI blocks drops across integrity levels; `ChangeWindowMessageFilterEx` is not called).

**Hand-off (the one call the Slint window makes).** After the window exists, on its UI thread:
`auto drop = cascade::core::FileDrop::attach(hwnd, [](const DropEvent& e){ auto items = classifyDrop(e.paths); ... });` and keep `drop` for the
window's life (destroy it BEFORE the window; destroying after is also safe). English strings: "Drop files here to open them", "Opened as an I/Q
recording", "Imported N bookmarks", "Installing a plugin needs your OK" (the window decides: a plugin package must go through the plugin store's
checks, never straight in), "This file is not a kind this program opens", "More files were dropped than can be handled (N not looked at)".

## 2. ExtIO radios through an opt-in helper process (R8) - `src/source/extio_protocol.hpp`, `extio_client.{hpp,cpp}`, `tools/extio_helper/`, `tests/fixtures/extio_standin.cpp`

**Built.** The application never loads an ExtIO DLL. `tools/extio_helper/extio_helper.cpp` loads the DLL the user chose, calls the documented entry
points and forwards samples and events over two named pipes (one per direction: a synchronous handle serialises its I/O, so one duplex handle
deadlocks reader against writer). `ExtioClient` starts it in a kill-on-close job object, picks the 32- or 64-bit helper by the DLL's PE machine
field, and turns what comes back into samples and refusal FACTS; `ExtioSource` is the `IqSource` the engine uses. The helper runs the DLL on its
main thread inside a message loop (drivers open windows), converts samples on the driver's own thread behind a structured-exception guard, and queues
with a bounded wait (a slow application slows a flooding driver down instead of growing memory).

**Interface source.** "Winrad - specifications for the external I/O DLL", Alberto I2PHD, read as text from https://www.i2phd.org/code/Winrad_Extio.pdf
(no licence or copyright terms on it or on https://www.i2phd.org/winrad/). No header of any vendor or of HDSDR was opened, so no header licence
question arose; the prototypes are written from the document. Used: the entry points and their signatures, HW types 3/4/5/6/7, the callback
`void (*)(int cnt, int status, float IQoffs, void* IQdata)`, statuses 100 to 115, StartHW's "at least 512 or a multiple" rule, the decorated names a
32-bit stdcall DLL exports without a .DEF file. No vendor DLL was downloaded. The GPL ExtIO_RTL repository was not read (its README was opened for its
licence and links only).

**Facts a refusal carries** (closed words, no path): `extio-not-chosen`, `extio-helper-missing`, `extio-helper-start-failed`, `extio-helper-exited`
(with the exit code), `extio-timeout`, `extio-protocol`, `extio-dll-refused`, `extio-unsupported-type`, `extio-bad-size`, `extio-flood`,
`extio-wrong-bitness`, `extio-silent`.

**Tests.** `tests/test_extio.cpp`, 19437 checks (19429 without the optional 32-bit run), on the hidden desktop three runs in a row. Eighteen stand-in
drivers written for it (tone with noise and a 40 ppm rate error; a crash on the driver's thread and inside a call; a hang in StartHW and in SetHWLO;
a flood; block sizes 700, 0x7fffffff, -3 and 0; a wild callback pointer; silence; no entry points; InitHW false; a sound-card driver; an unknown
sample format; non-finite floats; status callbacks). The application survived each with the matching fact; a crash shows as exit code 0xC0000005 in
the sentence, a hang kills the helper, a wild pointer is counted and the good blocks after it arrive, the flood is stopped by rate. Also: the tone's
spectrum (right sign, amplitude 0.5 through the 16-bit path), pacing, retune answers (0, minimum negated, maximum), status 100 updating the rate, a
second start after stop, retuning 60 times while another thread reads, wire fuzz (30000 mutated frames).
Break-it: the StartHW size rule and the exception guard removed gave 18 failures.

**32-bit.** REPORT: a 32-bit helper CAN be built in this tree without restructuring it. `tools/extio_helper` is standalone (it needs only
`source/extio_protocol.hpp` and the Windows SDK): `cmake -S tools/extio_helper -B build32 -A Win32` then `cmake --build build32 --config Release`
produced `extio_helper32.exe` (no warnings). A 32-bit stand-in tone driver built with no .DEF file (decorated `_InitHW@12` names) ran end to end
through that helper from the 64-bit test (spectrum correct, retune, close): that is the optional last section of `test_extio`
(arguments 3 and 4). The test tree builds only the 64-bit helper (`tests/CMakeLists.txt`); shipping both needs `add_subdirectory(tools/extio_helper)`
in the root CMakeLists and a 32-bit configure in the release script (not done: the lane may not edit them).

**UNVERIFIED.** Any real vendor ExtIO DLL (none downloaded); drivers that need their window to configure (the helper has a message loop and the
`ShowGui` / `HideGui` commands exist, but no real driver window was driven); HW type 4 (samples through a sound card) is refused by design;
other-process access: the job object kills the helper with the application but does not sandbox it.

**Decision for the owner.** The driver runs with the user's rights. The text every screen that picks a DLL must show is `extio::kExtioWarning`:
"An ExtIO driver is a program made by the radio's maker. It runs in a separate helper process with YOUR rights: it can read and change anything you
can. Choose only a driver you trust."

**Hand-off.** Setting `extio_dll` (path, EMPTY = off, default empty). Source row "ExtIO driver": "Choose an ExtIO driver (DLL)", "Open the driver's
window" (ShowGui), the warning above, the driver's name (`driverName()`; it may be a serial number: show it, never log it). Engine: construct an
`ExtioSource` with the helper paths (`extio_helper64.exe` / `extio_helper32.exe` beside the executable) when the setting is non-empty; log the fact
codes. Ship both helpers next to `cascade.exe`.

## 3. Discord rich presence (N25) - `src/net/discord_presence.{hpp,cpp}`

**Built.** Local named-pipe client (`\\.\pipe\discord-ipc-0` to `-9`). No network connection is made by this program (Discord's own client talks to
its servers). The client ID is a setting, EMPTY by default, which means off (no thread, no pipe); an ID that is not 17 to 20 digits is refused
(`bad-id`). Default line: details "Listening to the radio" and nothing else; with the box ticked the state line adds frequency and mode ("7.050 MHz
USB"; the mode is reduced to letters, digits and a few marks). One SET_ACTIVITY per five seconds at most (Discord allows five per twenty), unchanged
lines not resent, newest wins, ping answered, CLOSE or a broken stream reconnects with a growing wait and says the line again, the line is removed
when the object ends. The pipe is opened with an anonymous impersonation level.

**Source.** https://docs.discord.com/developers/topics/rpc (the old address redirects there) and
https://docs.discord.com/developers/events/gateway-events (Activity object): neither states a licence. Facts used: the pipe names; opcodes 0 to 4;
the 8-byte header (the page's example bytes are little endian but it does not say so in words); the handshake `{"v":1,"client_id":...}`; READY;
SET_ACTIVITY with `pid` and `activity`; activity types 0, 2, 3, 5 allowed (2 Listening used); error 4006 before authentication; five updates per
twenty seconds. NOT stated by the page and so CHOSEN here (and UNVERIFIED against a live Discord): the length limit of details and state (128 bytes,
cut on a character boundary), how an activity is cleared (the `activity` member omitted), and that no timestamps are sent.

**Tests.** `tests/test_discord_presence.cpp` 109 checks against a stand-in Discord server on a test pipe (handshake validated, READY, PING answered
by PONG, refusal with ERROR 4000 and its retries, a CLOSE frame and the reconnect that says the line again, garbage from the server, Discord starting
late, coalescing of twenty changes into at most a few frames, unchanged line not resent, clear and clear at the end, off and bad-id never connecting,
prompt destruction). Break-it: no interval check, always-show frequency, no text cap: 9 failures.

**UNVERIFIED.** A real Discord client; the clear form above; that an application the owner registers shows the "Listening" type as expected.
**Owner:** register a Discord application and put its numeric ID in the setting; nothing else is needed.

**Hand-off.** Settings: `discord_client_id` (text, empty = off), `discord_show_frequency` (bool, default false). Strings: "Show what I am listening
to on Discord", "Discord application ID", "Also show the frequency and mode", state words "off", "bad-id", "searching", "connecting", "connected",
"refused", "lost" for a status line. Engine: one `DiscordPresence` per process; `update({listening, frequencyHz, mode})` on receiver changes
(it coalesces), `clear()` when audio stops.

## 4. CAT emulation (N2) - `src/net/cat_emulation.{hpp,cpp}`

**Built.** The application answers as ONE KENWOOD (TS-480) and ONE YAESU (FT-891) radio, receive only, on a serial port the user names and on a TCP
port (127.0.0.1 unless the user asks for all interfaces). **Why these two:** published, complete, short semicolon-terminated ASCII references; the two
families (Kenwood-style and Yaesu-style ASCII CAT) are the ones logging and digital-mode programs implement first. That is a judgement; no client
list was consulted, so "the most widely supported" is UNVERIFIED.

**Sources** (makers' documents, no licence, "for the user's convenience" notices): Kenwood "PC CONTROL COMMAND REFERENCE FOR THE TS-480HX/SAT
TRANSCEIVER" (2003/11/28) read from https://www.radioamatore.info/attachments/059_TS-480_pc_control_command.pdf (kenwood.com answered 403 to the
automated fetch of its own page https://www.kenwood.com/i/products/info/amateur/ts_480/command_system.html); Yaesu "FT-891 CAT Operation Reference
Book" read from the copy at rigreference.com (URL in the header of `cat_emulation.hpp`; the book names yaesu.com as the maker's site).

**Commands.** Both: FA, FB (set and read frequency), MD (mode), SM (signal), ID, IF (38 and 28 characters, from the manuals' tables), PS, AI
(accepted, reads 0), unknown = `?;`. Kenwood: FR (receive VFO A/B; FT read, set ignored). Yaesu: SV, AB, BA, ST0. **Receive only:** Kenwood TX, KY,
AC answer `E;`, SR and PS0 answer `E;` (the receiver is not stopped); Yaesu TX1/TX2, ST1/ST2, KY, MX, PS0 answer `?;`; all counted
(`transmitRefused`); the fake receiver in the tests is never touched by one.

**Chosen where the documents are silent (UNVERIFIED against a real client):** S-meter Kenwood round(level x 20) (the document's 0000 to 0020),
Yaesu round(level x 255); modes with no equivalent (FSK, RTTY, C4FM, reversed CW) refused; WFM reads as FM, DSB as AM, RAW as USB; Yaesu
frequency is nine digits (clamped at 999999999, accepted from 30000 Hz up where the real radio stops at 56 MHz); AI never sends unsolicited data.

**Tests.** `tests/test_cat_emulation.cpp`, 160 checks: hand-written expected bytes for every command, split and merged reads, dummy data, an over-long
command, the receive-only guarantee for both dialects, a null backend, a fuzz run (80000 inputs, every reply `;`-terminated, the receiver only ever
sees legal frequencies), the stream pump, the SERIAL service over a named pipe opened by the same `SerialPort` class (the seam its header
documents), its REFUSAL path on real absent ports (`COM98`, no name, a baud it does not offer, a typed path never echoed, already running), the TCP
service (a second client sharing the VFOs, the four-client limit, port in use, 127.0.0.1 only: this machine's other address refused, all interfaces
only when asked, a hard stop that closes clients, restart). Break-it: transmit pass-through, all-interfaces bind, wrong S-meter scale: 9 failures.

**UNVERIFIED.** A real serial port pair: it needs a virtual-COM driver that is not installed here (and none was downloaded). The Linux side of the
TCP service (stubbed to a refusal).

**OmniRig (research only).** OmniRig is a freeware COM component for CAT control (https://www.dxatlas.com/omnirig/, Afreet Software, VE3NEA); rigs
are described by external INI files; its source is MIT-licensed (https://github.com/VE3NEA/OmniRig, `OmniRig.ridl`). A client program uses the
coclass `OmniRigX` (CLSID 0839E8C6-ED30-4950-8087-966F970F0CAE, interface `IOmniRigX` with `Rig1`, `Rig2`) and gets an `IRigX` with `Freq`, `FreqA`,
`FreqB`, `Mode`, `Vfo`, `Split`, `Tx`, `Status`, `RigType`, `SetSimplexMode`, `SetSplitMode`, plus the events `StatusChange`, `ParamsChange`. The
dxatlas pages for the developer notes (`/Dev/`) and the INI structure (`Files/IniStru.txt`) answered 404, so the ProgID and the INI layout were not
read. **What a client needs, in the two ways to be seen by an OmniRig client:** (a) FoxSDR is a RADIO on a serial port that OmniRig drives with a
Kenwood TS-480 or Yaesu FT-891 rig file: this part gives the radio side; the port pair needs a virtual-COM driver (third-party, not installed), and
OmniRig must be configured by the user. (b) FoxSDR IS the OmniRig server: it would implement `IOmniRigX`/`IRigX` itself from the MIT type library;
that conflicts with a real OmniRig registered under the same CLSID and was not started. No recommendation beyond: (a) needs nothing more from this
program than this part.

**Hand-off.** Settings (all off by default): `cat_emu_model` ("Kenwood TS-480" / "Yaesu FT-891"), `cat_emu_serial_port` (text, empty = off),
`cat_emu_serial_baud` (4800 to 115200, default 9600), `cat_emu_tcp_port` (0 = off, default 0; suggested 4532 is NOT assumed), `cat_emu_tcp_all`
("Allow other computers (all network interfaces)", default off). Strings: "CAT emulation (receive only)", "Radio model", "Serial port", "Baud rate",
"TCP port", the fact texts of `CatEmuFact`. Engine wiring: build a `RigBackend` from the real receiver's calls (frequency, set frequency, mode name,
set mode, a 0..1 signal figure, running) and construct `CatSerialService` / `CatTcpService`; start only on the user's request.

## 5. RTL-SDR offset tuning (R12) - `src/source/rtl_tcp_source.{hpp,cpp}` (EDITED), `tests/test_rtl_tcp_source.cpp` (EDITED)

**Built.** `RtlTcpSource::hasOffsetTuning()`, `setOffsetTuning(bool)`, `offsetTuning()`. Offered only when the connection is open and the server's
header names an E4000 (tuner type 1); on any other tuner `setOffsetTuning` refuses with a sentence naming the tuner and sends nothing. Wire: command
0x0a, 0/1, big-endian (rtl_tcp protocol overview, https://k3xec.com/rtl-tcp, no licence stated, its explanation of 0x0a is a TODO; the GPL sources
of rtl_tcp and librtlsdr were not read). The connection sends it explicitly (0 unless asked) at the end of the initial state for an E4000 only,
because the server keeps its state between clients.

**Why existing files were edited.** The brief asks for the call to be exposed as a source option and tested with the existing stand-in; the option
lives in the source class and the stand-in (`FakeRtlTcp`) in the existing test. A merge hot spot: if the radio-sources lane touches
`rtl_tcp_source.*` the two meet here (no diff to these files on `feat/radio-sources` at the time of writing). The existing E4000 open test expected
seven commands; it now expects the eighth (0x0a = 0) and its gain-by-index check moved from command 7 to 8.

**Tests.** 518 checks, 0 failed: not offered while closed; the explicit off; set true and false with exact bytes; offered again only after reopening;
tuner numbers 5, 6, 3, 0 and 9 never sent the command and refused with "E4000" in the sentence. Break-it: guard removed and send-to-all gave failures.

**Not done / UNVERIFIED.** The NATIVE WinUSB driver (`rtlsdr_source`) has no E4000 tuner support in this tree (R820T / R828D only), so the option
exists for the rtl_tcp source only; no radio was opened; whether a real E4000 server honours 0x0a as expected is UNVERIFIED (the protocol page says
the explanation is a TODO); that librtlsdr refuses offset tuning on R820T-class tuners is not relied on and was not verified from a primary page.
**Hand-off.** A toggle "OFFSET TUNING" (default off) beside the bias-tee control, shown only when `hasOffsetTuning()`; reach it with the same
`dynamic_cast` chain `gui/bias_tee.hpp` uses.

## Sources opened (every URL) and their licence or status

| URL | Status |
|---|---|
| https://www.i2phd.org/winrad/ ; https://www.i2phd.org/code/Winrad_Extio.pdf | the ExtIO specification; no licence or copyright statement |
| https://www.hdsdr.de/hardware.html | no specification there, no licence statement; https://www.hdsdr.de/ExtIO_Specification.html answered 404 |
| https://github.com/hayguen/ExtIO_RTL/blob/master/README.md | README only, to read its licence (GPL-2.0) and links; no code read |
| https://discord.com/developers/docs/topics/rpc (redirects to https://docs.discord.com/developers/topics/rpc) ; https://docs.discord.com/developers/events/gateway-events | no documentation licence stated |
| https://www.kenwood.com/i/products/info/amateur/ts_480/command_system.html | 403 to the automated fetch |
| https://www.radioamatore.info/attachments/059_TS-480_pc_control_command.pdf | Kenwood's TS-480 reference; "for the user's convenience" notice |
| https://www.rigreference.com/storage/manuals/yaesu/FT-891 CAT Reference Book.pdf--5a6a4c01735456.71645851.pdf | Yaesu's FT-891 CAT Operation Reference Book; no licence |
| https://www.dxatlas.com/omnirig/ ; /omnirig/Dev/ and /omnirig/Files/IniStru.txt (404, tried at two spellings) | OmniRig freeware page |
| https://github.com/VE3NEA/OmniRig ; its tree via the GitHub API ; raw `OmniRig.ridl` | MIT licence |
| https://k3xec.com/rtl-tcp | no licence stated; "work in progress, may contain errors" |

Web search result pages were read for links only (they also listed GPL code hosts: not opened).

## Owner decisions

1. **ExtIO warning text** (above) is shown wherever a DLL is chosen; confirm the wording and that the feature stays off until a DLL is picked.
2. **Ship the helpers**: `extio_helper64.exe` and `extio_helper32.exe` next to `cascade.exe`; the root CMakeLists and the release script need two lines (not edited here).
3. **Discord**: register an application and give its numeric ID.
4. **CAT models** (TS-480 and FT-891) are a judgement, not a measured popularity: say if a different pair is wanted.
5. **Plugin drops**: a dropped plugin file must go through the store's checks, not straight into the plugins folder.
6. **`rtl_tcp_source` edit** (part 5) is the one change to existing code; two existing-test expectations moved.

## Final gates

Build: `cmake --build D:/foxsdr-build/feat-desk --config Release --parallel 3`, Slint OFF, SOFTGL OFF, no compile errors.

Full suite, 782 tests, on the hidden desktop in four `ctest -j 3 -I` chunks (before the three late fixes below), 22 failures, all accounted for:

| Failure | Cause | Result |
|---|---|---|
| test_airband_data, test_band_plan, test_converter_call_sites, test_page_resize_id, test_png_decode | the known false failures | unchanged |
| test_window_failed_app, test_software_gl_app | configuration (Slint OFF, SOFTGL OFF: no `softgl` renderer in this build) | not this lane |
| test_engine_api_source_kinds, test_engine_api_decode_radio | the fixture `src/slint/tests/drive/fixtures/scope.wav` (and the other wavs) is generated by `make_*.py` and not in the tree ("the fixture scope.wav is not in ...fixtures"); decode_radio then waits out its 120 s | not this lane (baseline build not run: UNVERIFIED as baseline, cause read from the test's own message) |
| test_health_app, test_health_app_stepped, test_pass_control, test_usage_diag_window_free, test_failure_files, test_failure_recording, test_crash_upload_free | load flakes (the list in the standard rules plus two more) | all PASSED on a re-run alone |
| test_diagnostics_free (and test_diagnostics) | the known "uncommitted tree" failure | PASSED once the tree was committed |
| test_extio | MINE: it timed out at 120 s under ctest on the hidden desktop | fixed in 4d982759 (forward-slash DLL paths; a fake .exe stalling) |
| test_thread_inventory | MINE: three new files create threads without a row | fixed in a02d009a |

Final re-run after the fixes (hidden desktop, `ctest -j 2`): `test_thread_inventory`, `test_engine_shell_guard`, `test_rtl_tcp_source`,
`test_discord_presence`, `test_file_kind`, `test_extio`, `test_diagnostics`, `test_cat_emulation`, `test_file_drop`, `test_engine_main_links_headless`,
`test_plugin_validate`, `test_diagnostics_free`: "100% tests passed out of 12". Real result lines of the five new binaries:
`test_file_kind: 98 checks, 0 failed`, `test_file_drop: 68 checks, 0 failed`, `test_extio: 19435 checks, 0 failed`,
`test_discord_presence: 109 checks, 0 failed`, `test_cat_emulation: 160 checks, 0 failed`; `test_rtl_tcp_source: 518 checks, 0 failed` (this one
includes the new offset-tuning blocks); `test_thread_inventory: 10180 checks, 0 failed`.

The full suite was NOT re-run end to end after the three late fixes; the dependent tests were rebuilt (a full build with no errors) and the twelve
above were run.

Real `%APPDATA%/foxsdr/config.json` sha256 at the end: `c21ce98dc30e50327f7666f7ee29047c0983d130ca3f21716d0c0e20e04603c3` (unchanged; no test used it).

## Merge hot spots

* `src/source/rtl_tcp_source.{hpp,cpp}` and `tests/test_rtl_tcp_source.cpp` (part 5; the E4000 open test now expects eight commands).
* `tests/CMakeLists.txt` (the helper, eighteen stand-in drivers, one branch and a 300 s timeout for `test_extio`) and `tests/thread_inventory_rows.inc` (three rows).
* Nothing in `engine.cpp`, `pipeline.cpp`, `fox_engine_api.cpp` or any `.slint` file.
