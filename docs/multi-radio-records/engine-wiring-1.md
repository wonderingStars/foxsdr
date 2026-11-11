# Engine wiring lane 1: the signal-block branches merged, a KiwiSDR radio and the tone squelch wired (record)

Branch `feat/engine-wire`, cut from `feat/engine-a` (d3630d78 = `engine/multi-radio`), worktree `C:\Users\steve\foxsdr-wt\mr-engine1`, build folder `D:/foxsdr-build/mr-engine1`, 2026-11-11. Nothing is pushed or merged
anywhere else. Earlier record: `engine-features-a.md`. The checklist worked from is `docs/agc-noise-blanker-design.md` section 4 (engine-stage4 worktree, read only); the worked example is AGC (0x0111) and the
noise blanker (0x0112), whose wiring is `agc-nb-wiring.md`.

**How far it got.** Step 1 (six merges) is done. Group A (the KiwiSDR radio) is wired and proved. In group B the tone squelch (CTCSS and DCS) with its tone remover is wired and proved; S-meter calibration
(the rest of B) was NOT reached, and nor were C, D, E, F and G (their order and what each needs is at the end). The lane stopped at a feature boundary because every build and gate on this PC ran under the load of about
twenty other lanes (a headless gate was measured at 41 of 813 tests in 70 minutes and was stopped: see "Gates").

## Commits (oldest first)

| Commit | What |
|---|---|
| c341792f | merge `feat/web-bookmarks` |
| 39c3aa4a | merge `feat/signal-blocks` |
| c10023f6 | merge `feat/signal-blocks-2` |
| 620ffbbe | merge `feat/signal-blocks-3` |
| df454716 | merge `feat/signal-blocks-4` |
| 98a33221 | merge `feat/kiwi-client` |
| 4dffee73 | A: a KiwiSDR receiver as a radio kind |
| 58a1aebc | A: the VFO-offset check hardened (found by break-it K1) |
| 265c70d4 | B1: tone squelch and tone remover |
| (last) | this record |

## Step 1: the merges

| Branch | Conflicts | Done |
|---|---|---|
| `feat/web-bookmarks` (762ee55a) | `docs/api-0.3-additions.md`: both sides added a section 9.23 at the same place. Kept both: the multi-radio 9.23 and 9.24 stay, the web page's section is 9.25. | `FOXAPI_OP_SET_MUTED` added to `kWebProfile`; `test_engine_api_05a` P1 sets `muted` and counts 39 (it was 38); its P3 list of ops NOT on the web profile no longer names SET_MUTED (it is on it now); the new source `freq_import_formats.cpp` added to `test_slint_host_tools`' sources in `src/slint/tests/CMakeLists.txt` (four unresolved externals otherwise). The state golden and the shutdown-budget rows for `kEventTick` and `kEventHeartbeat` arrived with the branch and pass unchanged. |
| `feat/signal-blocks` (4a654a44) | none (header-only blocks and their tests) | `test_adif test_clip_recorder test_ctcss test_dcs test_replay_ring test_smeter_cal` pass |
| `feat/signal-blocks-2` (266926bf) | none | `test_iq_readers test_nr_methods test_passband test_power_log test_sam` pass |
| `feat/signal-blocks-3` (59458f1e) | none | all eight new tests pass |
| `feat/signal-blocks-4` (51367821) | none | all ten new tests pass |
| `feat/kiwi-client` (2ea2c0dd) | none | the row the record asks for in `tests/thread_inventory_rows.inc`; `test_shutdown_budget_free` was red because the static scan found four unknown bounded waits in `kiwisdr_client.hpp` (the record had left it "not investigated"): four rows added (`kJoinWait`, `kKeepAliveInterval`, `kConnectWait`, `kStallWait`), all zero on the shutdown path, each with its reason. |

Two things the earlier merges needed and nobody had written: `test_thread_inventory` also found `src/core/mqtt_publisher.hpp` and `src/core/power_log.hpp` (one worker thread each, from signal-blocks-3 and -2); both got exempt rows
saying the engine does not start them yet. Whoever wires them must turn those rows into heartbeats.

`test_kiwisdr_live_probe` runs only against the stand-in unless given `--live`; the lane never gave it `--live`.

## Op allocation

`0x0113` to `0x011F` were reserved for this lane (receiver group of `foxsdr_api.h`).

| Op | Number | What | Remote (web) profile |
|---|---|---|---|
| `FOXAPI_OP_SET_TONE_SQUELCH` | 0x0113 | per radio: kind (off, CTCSS, DCS) and, with `ival[1]` = 1, the values | no: the page has no control, and `test_engine_api_05a` P1 requires the web profile to be exactly what the page can send. A public REMOTE session with the SETTINGS grant may send it, like the other audio settings. |
| `FOXAPI_OP_SET_TONE_REMOVE` | 0x0114 | per radio: the tone remover | no (same reason) |
| 0x0115 to 0x011F | free | next: `SET_METER_UNITS` (S-meter display), then the notch, noise-reduction method and the rest as they are wired; when 0x011F is reached continue at 0x0120 in the same receiver group (the next unused block of the group; nothing else of `foxsdr_api.h` uses 0x0120 to 0x01FF) | |

The KiwiSDR radio needs no op (it rides `SELECT_SOURCE` and `RADIO_ADD` text ids).

## A. The KiwiSDR audio-only radio

**What it is.** A new radio kind `kiwisdr`: `KiwiSdrSource` (`src/source/kiwisdr_source.*`) is a `DeviceSource` over the merged `KiwiSdrClient`. A KiwiSDR sends DEMODULATED 12 kS/s audio, so the source gives the
pipeline the audio as a real signal (I = the sample, Q = 0) and the radio's demodulator is kept in RAW: volume, squelch, noise reduction, AGC, route, solo, the recorders all work on it unchanged. It is listen only.
Decisions made, each noted so the screen lane and the owner can overturn them:
* the radio's centre frequency is the receiver's tuned frequency (a retune is a command to the receiver, in kHz); the VFO offset stays 0 (an offset op is answered CLAMPED with 0; a click on the display moves the
  centre); the mode the user picks is the receiver's mode (`am usb lsb cw nbfm`; WFM, DSB and RAW are kept as the user's pick and send nothing); the bandwidth control is accepted and stored but the local channel filter is
  left open (the receiver applies the passband). A bandwidth control on a KiwiSDR radio therefore does nothing audible; the screen should hide or grey it.
* opening BLOCKS (connect, the opening commands, a first tune, then up to 4 s for the receiver's first word) and runs on the engine's existing device-open worker. A refusal (`too_busy`, a password, `down`, a redirect,
  no audio in 4 s) or an unreachable receiver makes the open fail with a sentence, published as `fact.radio.<n>.refusal` exactly as the other network sources are, and the radio stays on the generator. A connection lost
  later is a fault of the radio with the client's sentence.
* the address is validated (host name or number, optional port, never a web address) BEFORE anything is closed, so a typo cannot take a live radio away; the same receiver twice is refused by the device ledger naming the
  holder; `kiwisdr=host:port` is the saved argument; NO PASSWORD is saved or sent (a receiver that wants one is refused in words; adding one is future work).
* a REMOTE session may not open one (`DENIED`: an outward connection to an address the remote client chose).
* cpp-httplib decisions as the record of `feat/kiwi-client` states: plain `ws://` only (the vendored httplib is built without OpenSSL on Windows), `close()` sends no WebSocket close frame, and the frame-header
  short-read defect is NOT fixed (the live test did not see it); `KiwiSdrClient::threadsAbandoned()` raises the engine's `ReaderLeak` notice.

**Files.** New: `src/source/kiwisdr_source.{hpp,cpp}`, `tests/test_engine_api_kiwisdr.cpp`, `tests/wire1_rig.hpp` (the engine rig shared by this lane's tests). Changed: `src/engine/engine.cpp` (`makeDeviceSource`,
`selectSourceById`, `openKiwiAt`, `adoptKiwiRadio`, `applyChannelBandwidth`, `commitModeIndex`, the offset guards), `engine.hpp`, `engine_radios.cpp` (RADIO_ADD key), `engine_radio_saved.cpp` (restore),
`engine_api_lists.cpp` (facts and the fingerprint), `fox_engine_api.cpp` (remote refusal), `core/config.cpp` (the two kind lists), `core/tune_control.hpp` (`isNativeSourceKind`), `core/device_scan_plan.hpp`,
`core/receiver_tables.hpp`, API.md, `docs/api-0.3-additions.md` 9.26, `tests/test_shutdown_budget.cpp` (the `kJoinWait` row now says it is wired).

**Config keys.** None new: `sourceKind` "kiwisdr" with `nativeArgs` "kiwisdr=host:port" for radio 1 and the same kind and `args` in a `radios` entry. An older reader does not know the kind and falls back to the generator
(`sanitiseSavedRadios` / the loader's kind list; see "Old reader").

**State fields.** None. **Facts:** `fact.radio.<n>.source` = `kiwisdr`; `fact.radio.<n>.kiwi.state` (`idle connecting playing busy password down redirect failed dropped malformed closed`), `.detail` (a sentence),
`.mode` (the receiver's mode word), `.rssi` (dBm, once the receiver has sent a level; the fact block is rebuilt when it moves 3 dB), `.dropped` (audio samples the engine could not take in time); absent on a radio that is not a KiwiSDR.

**Tests (real results, this session).** `test_engine_api_kiwisdr`: `196 checks passed, 0 failed` (address rules; radio 2 on the stand-in receiver: the audio's 1 kHz tone in its spectrum at 12 kS/s, the opening commands
and the tune the receiver saw, a retune to `freq=7100.000`, USB sent with `low_cut=300 high_cut=2700`, WFM sends nothing, a VFO offset refused with a narrow bandwidth in force, the free-offset op, a click on the display
moving the centre, the same receiver twice refused naming Radio 2 in both spellings, remove and re-add; busy, password, down, unreachable, a bad address, a REMOTE session and a receiver that accepts and says nothing;
save, in-process restart, the receiver gone at restart, an unknown kind; radio 1). `test_kiwisdr_client`, `_protocol`, `_replay`, `_live_probe` (stand-in only), `test_engine_api_source_kinds`, `test_engine_api_radios`,
`test_engine_api_saved_radios`, `test_config`, `test_device_scan_plan`, `test_tune_control`, `test_thread_inventory`, `test_state_snapshot_golden`, `test_event_trail`, `test_telemetry`, `test_health_events`: all 26 of
the targeted run passed (`100% tests passed out of 26`).

**Break-it.** Four removals, each RED, restored, rebuilt (`tools/mutation_check.sh`, `D:/foxsdr-build/mr-engine1/lane/mut-K*.log`):
K1 the VFO-offset guard removed: GREEN at first (the span clamp alone gave 0), so the test was hardened (a narrow bandwidth first, the free-offset op, a click on the display) and then RED (`vfoOffsetHz == 0.0 (2000 vs 0)`);
K2 the remote refusal removed: `status == FOXAPI_DENIED (0 vs -1)`; K3 the receiver's mode not sent: four FAILs (`SET mod=usb` never seen, `kiwi.mode` stays `am`, no `low_cut`/`high_cut`); K4 both `radioIsKiwi()` branches
(mode and click-to-tune) turned off: RED.

**UNVERIFIED.** No real KiwiSDR was contacted by this lane (nothing outside this machine was). Everything about the receiver rests on the stand-in server and on the live test the branch recorded. Mode words other than
`am`, and every passband but AM's ±4900 Hz, are the usual defaults and were not confirmed on the air. Nobody has listened to a KiwiSDR radio through the engine: the audio path (RAW demodulator, the channel filter left open,
AGC on audio that is already levelled) was proved by spectrum and by commands, not by ear. The tune is a network round trip, so a retune is heard a moment later. A radio on a receiver that wants a password is refused.

## B1. Tone squelch (CTCSS and DCS) and the tone remover

**Ops** 0x0113 and 0x0114 (above). `SET_TONE_SQUELCH`: `ival[0]` 0 off / 1 CTCSS / 2 DCS; with `ival[1]` = 1 the values follow: `num[0]` CTCSS Hz (one of the 50, or 0 = any tone), `num[1]` the DCS code written as digits
(`23` for 023; one of the 83, or 0 = any code), `num[2]` polarity 0 any / 1 normal / 2 inverted; without them the radio's last choice is used. Checked at the API, in `applyCommand` and in the pipeline. NFM only.

**Behaviour.** The chosen detector listens to the NFM discriminator output at the channel rate, before the AGC and the squelch; while a tone squelch is on the squelch's gate needs the tone as well as the carrier (the ramp's
target, so the audio fades in and out like the carrier gate's, and a missing tone is exact digital silence). `Squelch::carrierOpen()` is the carrier alone and `squelchOpen()` stays that (the auto-squelch and the heard-time
counters read it, as the hand-off says); `audioGateOpen()` is carrier AND tone. The remover is a 300 Hz high-pass on the heard audio (the decoders are fed upstream of it). Outside NFM the setting is kept and does nothing.
A retune does not reset a detector (a stale lock decays by itself: 150 ms for CTCSS, two words for DCS).

**State fields (appended after `nbStandingDown`):** `toneKind toneHz toneCode tonePolarity toneRemove` (settings: they move `modeSeq`) and `toneDetectedHz toneDetectedCode toneDetectedPolarity toneGate audioGate`
(measurements: they move nothing). `FOXAPI_MIN_RECEIVER_STATE` is unchanged. **Config keys:** `toneSquelchKind toneSquelchHz toneSquelchCode toneSquelchPolarity toneRemove` (radio 1: plain keys; radios 2 to 8: in `radios`), clamped
on load (a kind out of range goes to the nearest end; a tone or code not in the tables, or a polarity out of range, goes to "any"). The names are not `toneKind`/`toneHz` because `SavedRadio` already has a `toneHz` (a test radio's
generator tone). **Facts:** none new (the state carries it).

**Files.** New: `src/dsp/tone_squelch.hpp` (the rules), `tests/test_tone_squelch.cpp`, `tests/test_pipeline_tone.cpp`, `tests/test_engine_api_tone.cpp`. Changed: `dsp/squelch.hpp`, `core/pipeline.{hpp,cpp}`,
`third_party/foxsdr_api/foxsdr_api.h` (the vendored copy: the plugins repository's copy must be made identical), `core/app_commands.cpp`, `engine/radio_scope.hpp` (49 to 51 ops), `engine/fox_engine_api.cpp`,
`engine/receiver.hpp`, `engine/engine.cpp` (state fill, the two ops, `applyConfig`, `fillConfig`, `initialise`), `engine/engine_radio_saved.cpp` (save and restore through the op), `core/config.{hpp,cpp}`, `core/config_halves.hpp`,
`core/receiver_snapshot.cpp`, the mock engine, API.md 6.19, `docs/api-0.3-additions.md` 9.27, and the tests that count things (`test_radio_scope`, `test_receiver_snapshot`, `test_config`, `test_apply_command`,
`test_engine_main_config_roundtrip`, the fuzz seed `seed-defaults.json`).

**Tests (real results).**
* `test_tone_squelch`: `2143 checks, 0 failed` (all 50 tones and no tone between them; 512 codes through both conversions; exactly 83 valid; the gate: not required is bit for bit the old squelch, a missing tone is exact
  silence, the ramp has no step at the tone's edges, the tone cannot open a squelch the carrier has closed).
* `test_pipeline_tone`: `71 checks, 0 failed`: a generated narrow-FM transmission (speech-like audio through the 300 Hz high-pass plus a 100.0 Hz tone, or a DCS 023 stream) through the real chain: Off changes nothing;
  the right CTCSS tone opens the gate (detected 100.0 Hz), another tone leaves the carrier open and the audio at exactly 0.0 RMS, any tone opens, Off restores; DCS 023 detected as code 23 polarity 1, code 025 closed,
  the wrong polarity closed, any code opens; outside NFM nothing is required; the remover took the 100 Hz tone down `41.0 dB` and `39.2 dB` in two runs (a high-pass 40 dB there) with the speech above 300 Hz intact.
* `test_engine_api_tone`: `222 checks passed, 0 failed` (defaults; radio 1 and radio 2 each hold their own, set on one and read on both; targets 0, 1, 2, 9 and ALL; the settings reach each radio's pipeline; a generator radio
  in NFM with a CTCSS tone required reports `toneGate` 0 and nothing detected, Off reports 1; ten refusals at the API and the engine's own path; save, in-process restart of both radios in the engine and in their pipelines;
  the web, CAT and TCI profiles do not carry the ops).
* `test_receiver_snapshot` `842 checks, 0 failed`; `test_config` `3736 checks, 0 failed`; `test_radio_scope` `134 checks, 0 failed`; `test_config_halves` `2456 checks, 0 failed`; `test_apply_command` `1753 checks, 0 failed`
  (and `_free`, `_stepped`); `test_engine_main_config_roundtrip` `253 checks, 0 failed`.
* The mock engine (compiled as stage 3 did, against the three stand-in headers copied to `D:/foxsdr-build/w1-mock`, not committed): `437 checks passed, 2 failed`, the same two version-gate checks that fail on the 0.3 header
  before this change.

**Break-it.** Four removals, each RED, restored, rebuilt; the table is at the end of this section. The ranges are checked in three places (the API, the engine, the pipeline), so no single removal can show them: that
break-it (all three removed at once) was NOT run; the refusals are proved by the tests on the intact tree only.

**UNVERIFIED.** Everything is proved on generated signals only: no real repeater audio, no real CTCSS or DCS transmission went through it. The detectors' detect and release times on the air are the blocks' own measured
numbers (CTCSS 67.0 Hz about 0.25 s to lock and up to 0.8 s to release; 250.3 Hz 0.11 s and 0.2 s; DCS about 0.5 to 0.7 s). The owner decisions the block record left open stand: how the screen shows the 82 DCS pairs
("023 N" is "047 I"; the engine takes any of the 83 codes with a polarity and treats a chosen polarity strictly), and whether the CTCSS latency is acceptable. The detector runs at the channel rate (150 to 300 kHz) and
decimates inside; its CPU cost in a running chain was not measured separately. Bookmark tone fields and `tuneToBookmark` (design section 7) are not touched.

### Break-it results for B1

| Removal (`tools/mutation_check.sh`, logs in `D:/foxsdr-build/mr-engine1/lane/mut-B*.log`) | RED in |
|---|---|
| B1: the engine no longer hands the op to the pipeline (`if (false)` in place of the `setToneSquelch` call) | `test_engine_api_tone` (`Access::pipeline(r.eng, 0).toneKind() == 1 (0 vs 1)`), `test_apply_command` (two checks) |
| B3: the squelch's gate is told the tone is never required (`setToneGate(false, true)`) | `test_pipeline_tone` (the audio gate stays open with the wrong tone chosen, four checks, the audio is not silent) |
| B5: the tone is not written to the settings file (`toneSquelchHz`) | `test_config` (`a.toneSquelchHz == b.toneSquelchHz`), `test_engine_api_tone` (three checks: the file, the restored radio, its pipeline) |
| B6: the op is not receiver-scoped (the `case` removed from `radio_scope.hpp`) | `test_radio_scope` (`isRadioScopedOp`), `test_engine_api_tone` (a command aimed at radio 2 lands on radio 1: four checks) |

## Old reader

NOT RUN. The proof the previous lane made (`tools/old_reader_probe`, the v0.99.104 sources of `config.hpp` and `config.cpp`) was not repeated for the new keys (`toneSquelch*`, `toneRemove`, and the new `sourceKind`
"kiwisdr"). The loader of this tree ignores unknown keys by construction (`test_config` "an older reader" checks are for THIS tree's loader), and an older build's loader is the same code path the `radios` keys of stage 5
already crossed, but that is an argument and not a run: UNVERIFIED.

## Gates

* **After step 1** the headless gate (`ctest -L "headless|slint" -j4 --timeout 300`) was started on the merged tree and STOPPED after 41 of 813 tests in about 70 minutes (the machine ran roughly 25 other compiler and test
  processes: CPU at 100 %). Of the 41 that had finished, `test_failure_files` (107 s) and `test_engine_api_txplugin` (54 s) were red; the second passes alone (exit 0, "stopped after 2.0 s (111 passes)"); the first was not re-run.
  The merges themselves were proved test by test (above). Nothing was waved through: the full gates below are on the later tips.
* Final gates: see the lane's final message; the numbers are repeated in the addendum at the end of this file when they exist.

## SCREEN HAND-OFF (no `.slint`, no catalogue, no drive script and no ledger row was touched)

Every control the Slint window needs, English label, bank, op or fact, range, default. Strings are not translated here.

**SOURCE (the radio's source section), "KiwiSDR receiver".** A text field "Receiver address" (placeholder "host name or address, port if not 8073") and an "Open" button: `SELECT_SOURCE` with the text
`open-kiwisdr:<host>[:<port>]` on the selected radio; for a further radio `RADIO_ADD` with the same text. The field is never saved by the window (the engine saves the address). While opening, the radio shows the generator
and the existing "opening" note; on failure the sentence is `fact.radio.<n>.refusal` (already shown for the other network sources). A radio on a KiwiSDR shows, from `fact.radio.<n>.kiwi.*`: "KiwiSDR: playing" /
"busy" / "wants a password" / "down" / "connection lost" (the `.state` word), the `.detail` sentence under a refusal, the `.rssi` as "-86.9 dBm" (absent until the first level), and the plain line "Audio only: the
receiver tunes itself". On such a radio: hide or grey the BANDWIDTH control, the VFO offset and the gain controls (the source has none), keep MODE to AM, USB, LSB, CW and NFM (WFM, DSB and RAW do nothing), and draw
the spectrum as an audio spectrum (it is mirrored about 0 and spans ±6 kHz; there is no RF to show). No range is claimed (`fact.radio.<n>.range` is absent).

**SIGNAL bank, beside "Squelch": "Tone squelch" (per radio; only live in NFM).**
* A selector "Tone squelch": Off / CTCSS / DCS: `SET_TONE_SQUELCH` `ival[0]` 0 / 1 / 2 (state `toneKind`), default Off. Greyed with the line "Tone squelch works in NFM" outside NFM.
* CTCSS: a picker "Tone (Hz)" with "Any tone" first, then the 50 tones (`ctcssToneHz(0..49)`, the 39 standard ones marked), sent as `num[0]` with `ival[1]` = 1 (state `toneHz`; 0 = any), default Any.
* DCS: a picker "Code" with "Any code" first, then the 83 codes written 023 ... 754, and a tick "Inverted" (polarity: Any / Normal / Inverted if the owner wants all three), sent as `num[1]` (the digits as a number) and
  `num[2]` (state `toneCode`, `tonePolarity`), default Any. The block's owner question stands: 82 of the 83 codes have a partner ("023 N" is "047 I"); the engine takes the code and the polarity as given.
  Send the three values together (they replace all three); a kind change alone (`ival[1]` = 0) keeps the radio's last choice.
* A readout "Heard": "100.0 Hz" (`toneDetectedHz`) or "D023N" / "D023I" (`toneDetectedCode` with `toneDetectedPolarity` 1 / 2) or "-", read-only, works with the squelch Off.
* A lamp "Tone": lit while `toneGate` is 1 and the kind is not Off; amber "Waiting for the tone" while the carrier is open (`squelchOpen`) and `toneGate` is 0. `audioGate` is the gate the listener hears.
* A tick "Remove tone from audio": `SET_TONE_REMOVE`, state `toneRemove`, default off. Tooltip: "A 300 Hz high-pass: the sub-audible tone is taken out of what you hear."
* New English strings (about 14): "Tone squelch", "Off", "CTCSS", "DCS", "Tone (Hz)", "Any tone", "Code", "Any code", "Inverted", "Heard", "Tone", "Waiting for the tone", "Remove tone from audio", "Tone squelch works in NFM".

## WHAT THE TELEMETRY WORKER WOULD HAVE TO LEARN (not touched)

* a new radio kind word `kiwisdr` in the driver vocabulary (`health::vocabulary()`'s driver list, `core/event_trail.cpp` `kDrivers`, `core/telemetry.cpp` `nativeFamilyModel`, and the Worker's own copies). Until it does the
  application reports the kind as `other` (`nativeFamilyModel` answers "other"; the health driver word falls to "other") and the equality tests stay green: so there is NO ordering hazard now, but the Worker must learn
  the word BEFORE the application is changed to say it. The receiver's address is never reported (the diagnostics log hides `kiwisdr=` like `rtltcp=` only where the existing scrubber already knows the key: it does
  not yet, see below).
* no new usage-report field and no new health word was added by A or B1.
* A PRIVACY NOTE the lane did not close: `core/diag_log.cpp`'s address scrubber lists the keys it hides (`host=`, `rtltcp=`, `uri=` ...); `kiwisdr=` is not among them. The engine never logs the address (the log lines name
  the kind only) and the refusal sentences do not carry it, but a future line that prints the args would leak it: add `kiwisdr=` to the scrubber when the Worker word is added.

## WHAT WAS NOT REACHED, in order

1. B: S-meter calibration (`smeter_cal.hpp`): `FOXAPP_OP_CAL_MEASURE` / `CAL_CLEAR`, a `CalTable` per device key persisted as one string, `SET_METER_UNITS` 0x0115, state `meterUnits signalDbm sUnits calibrated`.
   The block, its design and the hand-off are in `docs/feature-records/signal-blocks.md` (D).
2. C (batch 2): synchronous AM (`sam.hpp`, behind its setting, default OFF; its C-QUAM relative `cquam.hpp` is in batch 4 and UNVERIFIED), IF shift / pass-band tuning (`passband.hpp`), manual notch bank
   (`notch_bank.hpp` is batch 3) and automatic notches, noise-reduction method choice (`nr_methods.hpp`; the CW method only in CW and carrier modes).
3. D (batch 3): CW pitch (`kCwToneHz` in `src/dsp/demod.cpp` near line 100; CW today uses the voice SSB filter), the manual notch bank, the spectrum block (FFT size default 8,192, ceiling 131,072, and the window).
4. E (batch 4): noise squelch (guard its denormal behaviour on exact digital silence: add the all-zero input test and measure the CPU time), AFC, I/Q correction, equaliser and audio filters, CW peak filter,
   stereo placement, symbol tap, zoom FFT, SNR tap.
5. F: the plugin host changes (bookmark and marker cap 256 with a `markerLimit` fact and the visible span published to plugins; the EVENTS interface appended to the host table).
6. G: whatever else the six records hand to "the wiring lane". The ones seen and NOT done: MQTT publisher and rotator and decode webhook (batch 3: they have worker threads, whose inventory rows say so), power and SNR
   logging (batch 2), I/Q readers behind `IqFileSource::open` (batch 2), replay ring, clips and the listening log / ADIF export (batch 1: the brief names these as other lanes').

## Files and tools left in the build folder (not committed)

`D:/foxsdr-build/mr-engine1/lane/mut-K*.log`, `mut-B*.log` (the break-it runs), `D:/foxsdr-build/w1-*` (scratch: edit scripts, build and gate logs, the mock build in `w1-mock`).
