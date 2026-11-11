# Feature 10: tuning knobs, the stand-alone device-input layer (2026-11-11)

Branch `feat/knob-input`, cut from `engine/multi-radio` b9208fba. Design: `D:/foxsdr-build/prep/pre-release-features-design.md` section 11 (feature 10), section 1, section 18.
Scope of this lane: NEW files only (`src/input/`, `tests/input/`) and one block appended to the root `CMakeLists.txt`. Nothing is linked into the engine or the window; no existing source file was changed.
Build: `D:/foxsdr-build/feat-knobs`, configured with `-DCASCADE_BUILD_INTERFACE=OFF -DCASCADE_BUILD_SLINT=OFF -DCASCADE_SOFTGL=OFF` (nothing downloaded), built only `cascade_input`, `cascade_input_probe` and the five tests, `--parallel 4`.

**No MIDI controller, ShuttleXpress or PowerMate is attached to this PC. Everything below is proved against injected bytes, a named pipe standing in for a HID handle, and the real enumeration APIs returning "nothing found". No code here has ever read a byte from a real knob.**

## 1. What is built

| File | Contents |
|---|---|
| `src/input/input_event.{hpp,cpp}` | `InputEvent` (device id, control id, kind Relative / Absolute / ButtonDown / ButtonUp / Disconnected, `raw`, `value` 0..1, `timeUs` steady clock), `deviceIdFor` (FNV-1a of a stable text), `EventQueue` (mutex-guarded ring; a push is a few stores under the lock and never waits for the consumer; full means the OLDEST is dropped and counted in `dropped()`). |
| `src/input/midi_parse.{hpp,cpp}` | pure: `unpackShortMessage` (winmm packed DWORD), `MidiStreamParser` (running status, real-time bytes inside messages, sysex skipped), `MidiDecoder` (note on/off with velocity 0 = off, CC absolute 7-bit, 14-bit MSB/LSB pairs, relative two's complement / signed bit / binary offset 64 per control with `invert`, pitch bend absolute, channel-mode CC 120-127 ignored), `EncodingLearner` ("Learn": guesses the encoding from a stream of values, reports confidence and `ambiguous`). |
| `src/input/midi_win.{hpp,cpp}` | winmm `midiIn*`: `enumerateMidiInputs`, `midiPortPresent`, `MidiInputPort::open/close` (close is bounded: done on a helper thread that `close()` waits for at most `timeoutMs`). |
| `src/input/hid_parse.{hpp,cpp}` | pure: `ShuttleXpressParser` (ring -7..+7 as Absolute, jog 8-bit counter as Relative with wrap, five buttons), `PowerMateParser` (turn, push), product table, `stableHidId`. |
| `src/input/hid_win.{hpp,cpp}` | `OverlappedReader` (worker thread, overlapped `ReadFile`, bounded `stop()`), `enumerateHid` (SetupDi + `HidD_GetAttributes` + `HidP_GetCaps`), `HidInputDevice` (open an interface, or `attach` any overlapped handle). |
| `src/input/mapping.{hpp,cpp}` | (device, control) -> action; acceleration; shuttle-ring dead zone and repeat rate; modifier button; JSON load/save (nlohmann, the settings file's format) that tolerates unknown fields, malformed entries and absent devices and REFUSES transmit/ptt; presets; `Mapper::process` / `tick`. |
| `src/input/input_probe.cpp` | `cascade_input_probe [--listen SECONDS]`: lists MIDI ports and the supported knobs, optionally prints events. |
| `tests/input/test_input_{event,midi,hid_parse,mapping,win}.cpp` | five test programs (ctest labels `headless;input`), registered by the CMake block, not by `tests/CMakeLists.txt` (see 4). |

## 2. Device facts and where each comes from

Verification level is stated per fact. "In session" means a page was fetched and read in this session; "snippet" means only a search-result summary was seen (the page could not be fetched); "memory" means not checked in this session.

### 2.1 MIDI

| Fact | Source | Level |
|---|---|---|
| winmm `MIM_DATA`: `dwParam1` packs status (low byte), data 1, data 2; `dwParam2` is a ms timestamp from `midiInStart`; "running status disabled; each message is expanded to include the MIDI status byte"; not sent for system exclusive | Microsoft Learn, "MIM_DATA message", https://learn.microsoft.com/en-us/windows/win32/multimedia/mim-data | in session |
| Pitch bend: LSB data byte first, centre 0x2000 | MIDI Association, "Summary of MIDI 1.0 Messages", https://midi.org/summary-of-midi-1-0-messages | in session |
| System real-time messages (clock, start, continue, stop, active sensing, reset) are 0xF8..0xFF; controllers 120-127 are channel-mode messages | same page | in session |
| Controllers 0-31 carry the MSB and 32-63 the matching LSB (LSB = MSB + 32) | MIDI Association, "MIDI 1.0 Control Change Messages", https://midi.org/midi-1-0-control-change-messages | in session (the page does not say how the two bytes combine; `msb << 7 | lsb` is the standard reading, memory) |
| Note-on with velocity 0 is a note-off; running status rules; real-time bytes may interleave inside a message | MIDI 1.0 Detailed Specification (members-only; the public pages above do not cover them) | memory, NOT verified in session |
| Relative encodings (two's complement: 1..63 up, 127..65 down; binary offset: value - 64; signed bit: bit 6 = sign, 0-5 = magnitude) | controller-vendor convention, not part of the MIDI spec. Seen described in the Ardour manual (https://manual.ardour.org/using-control-surfaces/generic-midi/working-with-encoders/), the Control-Surface library docs (https://tttapa.github.io/Control-Surface/Doxygen/d3/dd0/classRelativeCCSender.html) and DJ/Arturia forum threads | snippets; sources DISAGREE on the sign direction of the signed-bit form (one says bit set = positive). This code uses bit 6 set = negative; **sign direction UNVERIFIED**, hence the per-control `invert` |

Whether winmm delivers system real-time bytes (clock, active sensing) to `MIM_DATA` at all is not stated on the cited page: UNVERIFIED. The decoder ignores them either way. Sysex is not delivered because no `midiInAddBuffer` buffers are ever added. `szPname` truncates port names to 31 characters (winmm structure; memory).

### 2.2 Contour ShuttleXpress (USB 0x0B33 / 0x0020), report layout

No vendor document was reachable. The layout is community reverse engineering and **UNVERIFIED on hardware**:

- 5-byte report, report ID byte NOT counted: `[0]` shuttle ring, signed -7..+7 (0 centre); `[1]` jog counter, unsigned 8-bit free-running; `[2]` unused; `[3]` bits 4-7 = buttons 1-4; `[4]` bit 0 = button 5.
- Buttons 1-4 in byte 3 bits 4-7 and button 5 in byte 4 bit 0, from "a raw `hid_read()` capture" of a ShuttleXpress 0B33:0020; the same page says the Pro v2 layout is inferred, not tested. AetherSDR pull request 5931, https://github.com/aethersdr/AetherSDR/pull/5931 (fetched in session; the byte numbering there is 0-based without a report ID byte; the page does not state the report length).
- Ring clamped to +-7, 0 at centre, report is 5 bytes ("a reviewer's comment"): AetherSDR pull request 5933, https://github.com/aethersdr/AetherSDR/pull/5933 (in session). Which physical direction is positive is not stated: the parser reports the signed value as sent; `invert` is per binding.
- Jog 0..255 integer: npm package `shuttle-control-usb` description (snippet). Wrap behaviour of the counter is not described by any source seen; the parser takes `(int8)(now - before)`, which is the usual reading of a free-running 8-bit counter, and is UNVERIFIED.
- An AutoHotkey forum thread (https://www.autohotkey.com/board/topic/92213-shuttlepro2-simple-script/, snippet only, the page returned 403) numbers bytes from 1 and puts buttons in "byte 4" and "byte 5", with the ring and the jog in bytes 1 and 2: that is consistent with the layout above if its first byte is index 0.
- Whether the device uses a non-zero report ID, and whether the 5-byte report is one top-level collection or several: UNVERIFIED. The enumeration keeps every interface of the product and marks `plausible` the ones whose `InputReportByteLength` is at least 6 (5 + the ID byte Windows prepends); the reader strips byte 0.
- Whether the first report after open carries the current counter, and whether a plain `CreateFile(GENERIC_READ)` succeeds on this device (Windows refuses it for keyboard and mouse collections): UNVERIFIED.

### 2.3 Griffin PowerMate (USB 0x077D / 0x0410)

- VID/PID, 6-byte input report, byte 0 = button (0 released, 1 pressed), byte 1 = signed rotation delta, positive clockwise, typically +-1..+-7, bytes 2-5 not described, no speed field: README of the macOS driver https://github.com/jameslockman/Griffin-PowerMate-Driver (fetched in session).
- The Linux kernel driver `drivers/input/misc/powermate.c` was cited only from a search snippet (button = bit 0 of byte 0, rotation byte read as signed char); its page could not be fetched and **no code from it was read or used**.
- A feature report of `00 00 00 f2 01 04` and the LED command bytes seen in a gist are NOT used (the LED is out of scope).
- Parser uses bit 0 of byte 0 as the push and the second byte as a signed delta. All of it **UNVERIFIED on hardware**. Whether the plain Windows HID stack lets a user-mode program read it (community tools are reported to bind it to WinUSB with a driver swap) is UNVERIFIED; this layer never asks for a driver change: if `CreateFile` fails the error is returned as text.
- The product is discontinued; the design record marks PowerMate best-effort.

Not supported on purpose: Shuttle Pro v2 (0B33:0030 appears only in the design record's facts report; layout inferred only).

## 3. API choices

- **MIDI: winmm.** In every Windows, no SDK package, no window or message loop (`CALLBACK_FUNCTION` runs on a driver thread). Limits: MIDI 1.0 only, 31-character port names, no documented hot-unplug notice (found by `midiPortPresent` polling and by `MIM_CLOSE` if the driver sends one: UNVERIFIED that it does). `midiInClose` is never called from the callback. `close()` is bounded (2 s default) and returns false, leaking the port, if a driver does not return.
- **Running status and interleaved real-time bytes.** winmm expands running status before the callback (Microsoft page above), so the winmm path never sees running status. `MidiStreamParser` handles both for raw byte transports and is tested; whether winmm passes real-time bytes at all is unverified (2.1).
- **HID: SetupDi + `CreateFile` + overlapped `ReadFile` on a worker thread, not Raw Input.** Raw Input delivers `WM_INPUT` to a window's message loop (a hidden window and thread, or the UI thread), and parses only what Windows can describe; a worker with its own handle needs no window and can be cancelled with `CancelIoEx`. Cost: the handle is opened for read, which Windows refuses for keyboard and mouse collections (error reported, UNVERIFIED whether either knob presents as one). Enumeration opens with access 0 (allowed even for exclusive collections) and reads only vendor id, product id and capabilities: no serial number, no product string.
- **Bounded close (`OverlappedReader::stop`).** Signals a stop event, `CancelIoEx` on the pending read, waits 1 s for completion; if the driver does not complete a cancelled read it closes the handle and waits 1 s more; if even that fails `stop()` returns false after `timeoutMs`, forbids further callbacks and deliberately leaks the worker's state (the kernel may still write into its buffer). The abandon path itself could not be provoked in a test (a pipe always completes a cancel); only the stop and cancel paths are exercised.
- **Mapping file: JSON (nlohmann), as `config.json` is.** Read `src/core/config.cpp`: objects are read with `is_object()` guards and each field tolerated if absent; this layer follows that style but the config code was not edited.

## 4. Test layout

`tests/CMakeLists.txt` globs `tests/test_*.cpp` and links each against `cascade_engine`, and asserts a headless/gui label on everything it registers. The knob tests need only `cascade_input`, so they live in `tests/input/` (a subdirectory the glob does not read) and are registered by the appended root block with `add_test` and `LABELS "headless;input"`. They run in the `-L headless` pass. `ctest --test-dir <build> -C Release -L input` runs just these.

## 5. Results (real run, this session)

`ctest -C Release -L input`: 5 of 5 passed. Checks per program, from each program's own summary line: `test_input_event` 31, `test_input_midi` 88, `test_input_hid_parse` 72, `test_input_mapping` 89, `test_input_win` 106 (386 in all), 0 failed. The fuzz loops: 2,000,000 random bytes through the MIDI stream parser and decoder and 2,000,000 random packed words through the unpacker (178,545 events produced, all in range), and 1,000,000 random reports of random length 0..16 through both HID parsers (1,503,033 events, all in range: ring -7..7, value 0..1, control ids from the defined set, relative never 0, no Disconnected). A loop of 40 runs each of `test_input_win`, `test_input_event` and `test_input_mapping`: 0 failures in 120 runs.

The window-less pipe tests drive the real worker: reports reach the parsers and the queue with the report ID byte stripped; truncated and malformed reports are counted in `rejected()` and make no event; closing with a read pending and a silent device took 0 ms (asserted under 500 ms); the writer end closing (unplug) produced exactly one `Disconnected` event and `isOpen()` went false; close while a writer thread streams reports continuously; 60 open/close cycles in under 20 s.

### Break-it table

Each row: the behaviour was removed or broken in the source, only the named test rebuilt and run, and it went RED (a `FAIL` line or an abnormal exit); then the file was restored by `git checkout` and the run was green again.

| # | Behaviour broken | Test | First failing check (line) |
|---|---|---|---|
| 1 | queue drops the newest instead of the oldest | event | `out[0].raw == 7 ...` (41) |
| 2 | queue does not count drops | event | `q.dropped() == 6` (38) |
| 3 | two's complement read unsigned | midi | `decodeRelative(TwosComplement,127) == -1` (29), 6 failures |
| 4 | signed-bit sign flipped | midi | `decodeRelative(SignedBit,1) == 1` (34), 5 |
| 5 | binary offset without the 64 | midi | `decodeRelative(BinaryOffset,64) == 0` (39), 7 |
| 6 | note-on velocity 0 treated as down | midi | `run(0x91,60,0) ... ButtonUp` (120) |
| 7 | running status not kept | midi | three-message running-status case (73), 3 |
| 8 | real-time bytes break a message | midi | interleaved real-time case (77), 2 |
| 9 | 14-bit MSB shift wrong | midi | `raw == 16383` (14-bit pair) |
| 10 | pitch bend bytes swapped | midi | `raw == 8192` |
| 11 | Learn cannot recognise binary offset | midi | learn offset case (175) |
| 12 | Learn claims certainty on one direction | midi | `r.ambiguous && r.confidence < 0.5` (187) |
| 13 | jog 8-bit wrap not handled | hid_parse | wrap case (57), 5 |
| 14 | ring range not checked | hid_parse | `Malformed` for ring +8 (118), 5 |
| 15 | buttons read from the wrong nibble | hid_parse | button down count (87), 12 |
| 16 | truncation limit lowered | hid_parse | `Truncated` (115), 3 |
| 17 | PowerMate turn read unsigned | hid_parse | `raw == -3` (142) |
| 18 | transmit/ptt refusal removed | mapping | `refusedForbidden == 3 ...` (129), 2 |
| 19 | unknown fields dropped on save | mapping | `out.contains("future") ...` (83) |
| 20 | acceleration off | mapping | `amount - 5.0` (187), 3 |
| 21 | ring dead zone ignored | mapping | `ring(1).empty()` (256) |
| 22 | ring first detent missing | mapping | `a.size() == 1 ...` (260), 2 |
| 23 | invert ignored for relative | mapping | `amount == -2.0` (172) |
| 24 | modifier ignored | mapping | `amount == 20.0` (291) |
| 25 | reader ignores the stop event (close hangs) | win | `dev.close()` returns false after the 2 s bound (158), 4 failures |
| 26 | close does not cancel the pending read | win | `ms < 500` (186) |
| 27 | unplug not reported | win | `Disconnected` never arrives (199), 2 |
| 28 | rejected reports not counted | win | `rejected() >= 2` (155) |
| 29 | report ID byte not stripped | win | events never arrive (142), 4 |

Line numbers are those of the committed test files at the time. An earlier version of row 19's test crashed (const `operator[]` on a missing key, exit 0xC0000409) instead of failing a check; the test was changed to guard the lookups and re-run RED with a `FAIL` line. The 500 ms bound was tightened from 1500 ms after row 26 would have passed under the old bound.

## 6. This PC (task item 6)

`cascade_input_probe`, run once:

- MIDI input ports: 0. (No port to name; no real MIDI open was therefore exercised: the open/close-within-a-bound test for a real port is written but its body ran zero times.)
- HID: interfaces of the supported products (Contour ShuttleXpress, Griffin PowerMate): 0. Other HID interfaces: 19, counted and not described; 1 interface could not be opened for attributes.

So this PC has no MIDI port and neither knob. No `--listen` run was made.

## 7. WIRING HAND-OFF (what the engine / screen lane must do)

The input layer lives on the Slint host side, not in the engine (the engine has no key table; a headless engine has no use for a knob).

1. **Build.** Link `cascade_input` (target defined in the root `CMakeLists.txt` block at the end of the file) into the Slint host target. It brings `cascade_json`, `winmm`, `setupapi`, `hid` on Windows. The hid_win / midi_win units are Windows-only; the parsers, queue and mapping compile everywhere. Linux (ALSA sequencer or raw MIDI, hidraw) is not built and was not researched.
2. **Owner of the objects.** One `EventQueue` (capacity 1024 is the default), one `Mapper`, a `MidiInputPort` per MIDI port in use, a `HidInputDevice` per knob in use, all owned by the host's UI object; created after the window, destroyed before it.
3. **Start / hot-plug.** At start and about once a second: `enumerateMidiInputs()` and `enumerateHid()` (cheap; names and ids only). For each device a binding refers to (`Binding::device` text equals `MidiPortInfo::stableId` or `HidDeviceInfo::stableId`) that is present and not open: `MidiInputPort::open(info, queue, err)` / `HidInputDevice::open(info, queue, err)` (HID: only interfaces with `plausible == true`). After a MIDI open, hand the decoder its encodings: `for (auto& [ctl, cfg] : mapper.midiConfigsFor(stableId)) port.decoder()->setControl(ctl, cfg);` and re-do that whenever the mapper's settings change. A `Disconnected` event (or `midiPortPresent` false) means close the object and show the device as absent.
4. **UI tick** (the thread that owns Slint): `queue.drain(v)`; for each event `mapper.process(e)`; then `mapper.tick(nowMicros())`; run each returned `ActionOut`. Report `queue.dropped()` somewhere diagnosable. No device thread ever calls Slint or the engine; events cross only by the queue.
5. **Executing actions** (all through the same host functions a key press calls, so Link, band-follow and the tune-step label apply unchanged). `amount` is signed detents after acceleration and the modifier.

| Action name | Event kinds | Meaning |
|---|---|---|
| `tune` | relative, button | `amount` x the current tune step (`stepTune(amount, tuneStepHz)`); coalesce per tick through the retune coalescer |
| `tune.fine` | relative, button | `amount` x step / `params.divisor` (default 10) |
| `tune.shuttle` | ring (absolute) | the Mapper emits detents at the ring's repeat rate; execute like `tune` |
| `volume`, `squelch`, `zoom` | relative, button, absolute | relative: `amount` detents; `absolute == true`: `amount` is 0..1 (set the level) |
| `step.up`, `step.down` | button | change the tune step size one notch (`amount` sign) |
| `mode.next`, `mode.prev` | button | next / previous mode |
| `radio.select` | button | `params.n` 1..8, the selected radio (as Ctrl+1..8) |
| `command` | button | `params.name` = a key-table action id (`mute`, `record`, `startStop`, `screenshot`, ...): run that key's action |

   Unknown action names load and are returned unchanged: the host must ignore what it does not know.
6. **Transmit.** The Mapper refuses at load any action or `params.name` beginning with `transmit` or `ptt` (case-insensitive). The host must additionally never let a `command` reach `transmitPtt` or any transmit control (the key table says the PTT row "is never answered here"); a host-side test for that belongs with the wiring.
7. **Where settings live.** In the interface half of the settings, NOT the engine half. `view3d` is a `name:body;name:body` text whose bodies cannot contain `:` or `;`, which JSON does, so do not put the mapping there. Recommended: one new interface-half string field (name suggestion `controllers`) holding `settingsToText(mapper.settings())` verbatim, written and read beside the other interface-half fields; load with `settingsFromText(text, &report)` and log `LoadReport` (skipped, refused, clamped, unparseable). An empty or damaged value gives an empty `Settings`, which is valid. Because `Settings` keeps unknown fields, an older build does not erase a newer one's entries. The config code was not touched in this lane.
8. **Learn.** For a MIDI "Learn": collect `Absolute` events' `raw` (the 7-bit value) of the first moving control into an `EncodingLearner`, then show `guess()` (`encoding`, `confidence`, `ambiguous`, `note`); if `ambiguous` or confidence < 0.7 tell the user to turn the other way. The learner cannot tell direction; offer `invert`. To learn a binding: the first non-`Disconnected` event after pressing Learn gives (device, control).
9. **Presets.** `shuttleXpressPreset(deviceId)` and `powerMatePreset(deviceId)` in `mapping.hpp`.
10. **System bank page** "Controllers" (design 11(b)): list from the two enumerations; table from `Settings::bindings`; the shift-equivalent button is `Settings::modifier`.
11. **Files outside this lane the wiring will edit** (per the design record, section 11(e) and 18): `src/slint/{system_logic.*, system_publish.cpp, main.cpp, ui/system_bank.slint, input_script.*}`, `resources/lang`, the interface half of `config.*`. `kKeyActionCount` and its tests change only if key rows are added; no key row is required by this layer.

### New strings the screen will need (English; each is 33 more translations)

"Controllers" (section title); "No controllers found"; "MIDI input"; "Learn"; "Turn or press the control now"; "Learned: %s"; "Not sure: turn it the other way"; "Control", "Action" (table heads); "Invert direction"; "Acceleration"; "Dead zone"; "Repeat rate"; "Hold for 10x"; "Tune (one step per click)"; "Tune fast (shuttle ring)"; "Volume"; "Squelch"; "Zoom"; "Step size up", "Step size down"; "Next mode", "Previous mode"; "Select radio"; "Run command"; "Device not connected"; "Could not open %s: %s"; "Preset: ShuttleXpress"; "Preset: PowerMate". About 28; the design record estimated 25. `test_i18n` fails on a catalogue key with no source use, so they land with their code.

## 8. UNVERIFIED list

1. Every HID byte layout (2.2, 2.3) on a real device; the ShuttleXpress jog wrap direction; ring sign direction; report IDs; the number and shape of its top-level collections.
2. That Windows lets a user-mode program open either knob for read; that the PowerMate works without a driver swap.
3. Signed-bit relative sign direction; every relative encoding on a real controller.
4. Note-on velocity 0 = off, running status rules, real-time interleaving (MIDI 1.0 Detailed Specification not available in session).
5. Whether winmm delivers real-time bytes to the callback; whether it sends `MIM_CLOSE` or any other notice on unplug; a real MIDI port open/close (zero ports on this PC).
6. The abandon path of the bounded close (reader or MIDI) under a driver that never completes.
7. Linux backends (not built, not researched).
8. Nothing was run through the engine or the window; no drive script, no real-mouse pass, no catalogue strings, no `test_i18n`, no whole-application build.

## 9. Not done

Per the brief: no wiring, no UI, no strings added to the catalogues, no config edits, no push, no merge. The LED of the PowerMate and any output to a controller (MIDI out, LED rings) are not built.
