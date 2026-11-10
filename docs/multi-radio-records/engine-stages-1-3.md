# Multi-radio engine, stages 1 to 3 (record)

Branch `mr/engine-stage1`, cut from `engine/multi-radio` at d094501b, built 2026-11-10 in the worktree `C:\Users\steve\foxsdr-wt\mr-engine1`, build folder
`D:/foxsdr-build/mr-engine1`. Design: `docs/multi-radio-engine-design.md` of the engine-stage4 worktree (untracked there, not copied). Its last section
("Stage 0 result and the limit of eight") supersedes every "four" and "five in total" above it: `kMaxReceivers` = 8, at most eight open radios in total.

Commits (oldest first):

| Commit | Subject |
|---|---|
| 17467c85 | Engine: Receiver struct and the receiver slot table (stage 1, group 0: the pipeline) |
| dffcad53 | Engine: the receiver's device, open, fault and retry state moves into Receiver (group 1) |
| 3d24a42d | Engine: the receiver's settings mirrors move into Receiver (group 2) |
| 77d33f83 | Engine: the receiver's recorders move into Receiver (group 3) |
| c97403f1 | Engine: the receiver's health counters move into Receiver (group 4) |
| a52e1ab9 | Engine: the receiver's published snapshot moves into Receiver (group 5) |
| fb5c59bb | Engine: DeviceLedger, and the patch radios' use of the one device through it (stage 2) |
| (stage 3) | see `git log` - Engine: addressing (session focus, target, RADIO_FOCUS, per-radio reads, fact.radio.*) |

## Stage 1: Receiver extracted from Engine (a pure move, slot 0 only)

### What moved where

`src/engine/receiver.hpp` (new) defines `kMaxReceivers = 8` (one constant; every structure that is per radio is sized by it; nothing assumes four) and
`struct Receiver`. `Engine` owns `std::array<std::unique_ptr<Receiver>, kMaxReceivers> receivers_`, declared right after `host_`; slot 0 is built in the
constructor (`Receiver::makeSlots`), the other slots stay empty until stage 4. 106 data members moved, with their declarations, initialisers and comments
unchanged (the moves were scripted, so no line was retyped):

| Group | Members | Moved |
|---|---|---|
| 0 pipeline | `pipeline_` | 1 |
| 1 device, open, fault and retry | `device_`, `soapyView_`, `deviceArgs_`, `sourceKind_`, `sourceError_`; the rate, gain, AGC, antenna and model mirrors; the RSP and RX888 switch mirrors; the open worker (`deviceOpenFuture_`, `deviceOpenPending_`, `deviceBusyLabel_`, `deviceOpenReqGen_`) and `sourceGen_`; `faultSeen_`, `apiFaultText_`; `soapyReopenAttemptSec_`, `savedRadioRetry*`; `restoreKeep_`, `restoreKeepLabel_`; the retune pacing (`retuneCoalescer_`, `pendingRetuneIsPreset_`, `tuneMismatchNote_`, `lastMismatch*`) | 44 |
| 2 settings mirrors | display range, `volume_`, `userMuted_`, `modeIndex_`, `vfoOffsetKhz_`, `bandwidthIndex_`, `vfoBandwidthHz_` (+ drag flag), `squelchDb_` and the automatic squelch (7), `deemphIndex_`, `stereoEnabled_`, NR (2), notch (3), `autoNotch_` | 25 |
| 3 recorders | the two `Recorder`s, their start workers, queue flags and finish tickets, `recordError_`, `recordNotice_`, the take start times and the I/Q header rate | 13 |
| 4 health counters | the audio watchdog, the starvation digest, the audio-lead state (`audioBufferAutoMs_`, `audioLeadNote*`, `audioLeadQuietMinutes_`) and the pipeline ring's minute (`lastDspLogSec_`, `dsp*AtMinute_`, `dsp*Session_`) | 22 |
| 5 published state | `receiverSnapshot_` | 1 |

Two types the moved members need were Engine's nested types and moved to namespace scope in `receiver.hpp`, with a `using` alias left in Engine so every
`Engine::DeviceOpenResult` / `Engine::AudioLeadNote` spelling still works: `DeviceOpenResult` and `AudioLeadNote`.

### How it is a pure move

`Engine` keeps, for every moved member, a REFERENCE of the same name bound to slot 0 (`float& volume_ = receivers_[0]->volume_;`). Every function of the engine,
every friend class (`ApiHostImpl`, `BoundedHost`, the window's) and every test that reaches a private member through a friend struct names exactly what it did,
so no engine function and no test source was edited for stage 1. The data has one home (`Receiver`). The engine's own code is converted to name a slot only where
a command or read is addressed to one (stage 3 onwards); a member that is one radio's is added to `Receiver`, never to `Engine`. The `Receiver` declares the
`Pipeline` first, so it is destroyed last, after the open future and the other state that reads it, as before; recorders sit after it, as before.

### What was left in Engine on purpose

Per-device maps keyed by device (ppm, converters, bias tee, gain memories) and the saved-config mirrors (`cfgSoapyArgs_`, `cfgNativeArgs_`): they belong to the
saved settings (stage 5) or are keyed by device. `recordDir_` (one folder for every receiver's takes), `audioBufferMs_` (the user's one buffer setting), the
patch speakers' audio-lead marks, the output device list. `bandModeFollow_` (global). The scanner, airband monitor, transmitter, plugin machinery (one each; stage 6).

### The API host's spectrum and audio rings are NOT in Receiver (deviation from the design's group list)

The design lists "its spectrum and audio rings" under the published-state group. They are owned by the API host (`ApiHostImpl`: `SpectrumRing spectrum_`, the audio
ring and its feed state), are created and discarded with the host (`Engine::configureApi` replaces the host and so empties them) and are read through the host's
own locks; their types are private to `fox_engine_api.cpp`. Moving them would change that lifetime and expose host-private types. Stage 3 reads the spectrum by
radio number inside the host (only radio 1 has a ring); stage 4 gives each added radio its own ring there.

### Proofs

* Build: `cmake --build D:/foxsdr-build/mr-engine1 --config Release --parallel` exit 0 at every commit (groups 3 and 4 were committed after a build and the
  targeted ctest runs named in their messages; the full gates ran on group 5, which is the stage's tip).
* `ctest --test-dir D:/foxsdr-build/mr-engine1 -C Release -L "headless|slint" -j4 --timeout 300`, stage 1 tip: `99% tests passed, 8 tests failed out of 736`, all
  explained: 5 tests (test_airband_data, test_band_plan, test_converter_call_sites, test_page_resize_id, test_png_decode) find the source tree by walking up from the
  current directory, which ctest sets to the out-of-tree build folder; they pass when run from the worktree root (115, 5174, 5, 22, 49 checks, 0 failed;
  `D:/foxsdr-build/mr-engine1/envtests.sh`). The other 3 (test_link_request_poll, test_record_finish_async, test_record_finish_async_stepped) failed together at 35 to
  39 s while other lanes were building and passed alone (29 s each).
* `ctest -L gui` on the hidden desktop (`FOXSDR_FRAME_CAP_HZ=60`, desktop `FoxSDRTests-mr1`): `99% tests passed, 1 tests failed out of 225` (the baseline names 224;
  the parent gained slint_drive_plugin-settings-back-to-back since), the one failure test_diagnostics, which compares the binary's embedded git commit with HEAD and
  was stale after the commits; passes after a rebuild on the clean tree (`100% tests passed out of 1`).
* No test source was edited.

## Stage 2: DeviceLedger and the patch radio borrow through it

`src/engine/device_ledger.hpp` (new, pure, no threads): "who holds this radio?". A holder is a receiver slot (spelled Radio 1, Radio 2, ...), a patch radio node (patch
radio 3) or the transmitter. The rule is the old one: one physical radio, one holder, the generator exempt, two keys naming one dongle by its serial across stacks are one
radio (`core::patch::sameDevice`, unchanged). New: a refusal names the holder ("in use by Radio 1", "in use by patch radio 3"); ONE limit on radios open in total,
receivers and patch radios together, sized by `kMaxReceivers` (a claim past it is refused with a sentence that states the limit and names every open holder); a
receiver's loan to the patch is on the ledger's books (`lend` / `lent`). A claim that was refused still names its device for later claims (the plan's DeviceTwice rule
looks at every earlier radio and `sameDevice` is not transitive), and the refusal names the holder behind it.

Engine: `deviceLedger_` is rebuilt by `Engine::rebuildDeviceLedger()` on every `patchReconcile`, after the receiver's radio has been lent and before the radios that should
run are chosen, from the state the engine holds: every receiver slot; the lent radio; every patch Radio node that names a device, in graph order, whether or not its
switch is on (a node counts toward the limit only while it is on and the patch runs). It replaces `deviceTakenEarlier` (deleted). A node whose claim is refused is never
opened; the words are `patchLedgerRefusal_`, published as `fact.patch.refusal.<node id>` (absent when not refused, cleared while the patch page is closed; the lists'
fingerprint hashes it). The patch borrow itself is unchanged and still decided by `gui::receiverSourceForPatch`: the receiver runs on the generator while the patch runs
and takes its radio back when it stops; the ledger records it. File manifest: `src/engine/device_ledger.hpp`, `engine.hpp`, `engine_patch_radios.cpp`,
`engine_api_lists.cpp`, `tests/test_device_ledger.cpp`, `tests/test_engine_api_device_ledger.cpp`.

Behaviour impact: with one receiver and up to five patch radios the total (six) is under the limit of eight, and the receiver's radio is lent before any node can claim
it, so no radio that opened before is refused now. The one new refusal reachable today is a node naming the receiver's open radio while the patch is stopped (the node is
not wanted then anyway). The refusal texts of the plan, the window and the drive scripts were not edited.

Not covered: the transmitter is a holder type of the ledger but the engine does not claim its device yet (the transmit device's key does not map onto a receive key without
a read of `transmitArgs_` that the stage did not make); slots 1 and up never claim (they do not exist); a mid-open receiver holds nothing (as before: the loan waits for the
open to finish).

### Proofs

* New `tests/test_device_ledger.cpp` (93 checks) and `tests/test_engine_api_device_ledger.cpp` (20 checks, a fake radio, the C table, the engine's passes: a node naming the
  receiver's open radio is refused naming Radio 1; the patch runs and the receiver lends, the node is granted; a second node under the other driver stack is refused naming
  the first node; the patch stops, the loan ends, both are refused naming Radio 1).
* Break-it checks, each red then restored: a claim that never refuses (`if (false && !exempt)`): test_device_ledger 18 failures, test_engine_api_device_ledger 6 failures; a
  ledger that never records the loan: 1 failure (`Access::receiverLent`).
* `ctest -L "headless|slint"`: `99% tests passed, 6 tests failed out of 738` (the 5 source-locating tests above and test_engine_api_03, which failed at 93 s under load and
  passed alone at 56 s). test_patch_radio and the rest of the patch tests passed unchanged. `ctest -L gui`: `99% tests passed, 2 tests failed out of 225`: test_deck_knobs_app
  and test_window_platform_app; both pass alone (116.7 s, 2.9 s). The patch drive scripts (`slint_drive_*patch*`) passed unchanged.

## Stage 3: addressing (still one live receiver)

Files: `third_party/foxsdr_api/foxsdr_api.h`, `src/engine/radio_scope.hpp` (new, pure), `src/engine/fox_engine_api.cpp`, `engine.hpp`, `engine.cpp`,
`engine_api_lists.cpp`, `src/core/app_commands.cpp`, `docs/api-0.3-additions.md` (9.19), `third_party/foxsdr_api/reference/{README.md, docs/API.md (6.18), mock_engine/mock_engine.cpp,
tests/test_mock_engine.cpp}`, `tests/test_radio_scope.cpp`, `tests/test_engine_api_radios.cpp`. API 0.3 and plugin ABI 3 are unchanged (nothing in `plugin_api` was touched).

* `FoxCommand::target`: for the 47 ops `isRadioScopedOp` lists, 0 = the session's focused radio, 1..N = that radio, `FOXAPI_TARGET_ALL_RADIOS` (0xFFFFFFFF) for RUN alone
  (BAD_ARGUMENT for any other op); a number that names no radio is refused `FOXAPI_NO_DEVICE` ("there is no radio with that number"), at the submit (rule 2, as a refused result)
  and again where the command is applied; `Engine::applyCommand` holds every command to the same rule, with radio 1 as the focus of a command with no session. Every other op ignores
  the target. The `sourceGen` guard is per radio (`Pending::radio`, `Engine::receiverSourceGen`). The list of scoped ops was settled by name, then by finding each op's case in
  `Engine::applyCommand` and checking that the body reaches the pipeline, the device, the mirrors, a recorder or the scanner (all 47 found; `BOOKMARK_TUNE` and `SET_SQUELCH_AUTO`
  read by hand). Read each handler once more when it first gets a second receiver to act on: the scan proves the handler touches the receiver, not that it touches nothing else.
* `FOXAPI_OP_RADIO_FOCUS` (0x1201, grant VIEW): per SESSION (`ApiSession::focus`, `focusBump`), R1 until set; NO_DEVICE / OUT_OF_RANGE / NO_CHANGE as in the header. It is a session's
  op applied by the API host, so its `kOps` entry is `implemented = false` (like SETTING_SET and the key ops), which keeps `test_apply_command`'s "every implemented op has a case"
  rule untouched. A change moves every seq group of that session's `read_state` (`bumpSeqs`).
* `read_radio_state(session, radio, out)` and `read_radio_spectrum(session, radio, since, info, bins, cap)`, appended after `get_local_extension`, tested with `FOXAPI_HAS`. NO CAPABILITY
  BIT: `test_engine_api_05a` pins `caps >> 30 == 0` and every bit 0..29, and the lane rule is "behind FOXAPI_HAS", so adding bit 30 would have meant editing that test for nothing.
  `read_state` and `read_spectrum` follow the session focus.
* Facts: `fact.radio.count`, `fact.radio.max` (= `kMaxReceivers`), and for each radio that exists `fact.radio.<n>.state` (opening, running, stopped, faulted, lent), `.device`, `.label`,
  `.audio` (always `both`), `.plugins`; a radio that is not there has none. The lists' fingerprint hashes the radio mask and each state word. `fact.radio.<n>.label` is untranslated English.
* `Engine::receiverMask_` (an atomic word, bit i = slot i) lets the API host answer from the submitting thread whether a number names a radio.

### Proofs

* New `tests/test_radio_scope.cpp` (130 checks: the rule, the 47-op list and the global ops it must not take, the focus bump) and `tests/test_engine_api_radios.cpp` (88 checks, real engine,
  C table, passes: the facts incl. `radio.max` = 8 and radio 2 absent; target 2, 9, 0xFFFFFFFE refused NO_DEVICE with the message and nothing changed; targets 0 and 1 land; ALL for RUN
  accepted, for SET_VOLUME BAD_ARGUMENT; global ops ignore the target; the local `applyCommand` path; RADIO_FOCUS out of range / no such radio / no change with only the VIEW grant;
  `read_radio_state` 1 and 0 equal `read_state`, 2 NO_DEVICE; `read_radio_spectrum` 1 and 0 equal `read_spectrum`, 2 NO_DEVICE).
* BREAK-IT CHECK (stage rule: removing the target handling turns a test red), done and restored: with `pickRadio` ignoring the target (`(void)live;`) test_radio_scope fails 5 checks and
  test_engine_api_radios fails 3 (the local `applyCommand` path: `refused.status == FOXAPI_NO_DEVICE (0 vs -3)` and two more); the host's second layer (the refusal where the command is applied) then
  still caught the API cases, so with that removed as well the test process crashes (a null slot): red either way, not a pass.
* The mock engine: edited and compiled against three small stand-ins for the `common/*` headers it includes (not vendored here, so nothing under `reference/` builds in this tree;
  the stand-ins are in the build folder, not committed): `test_mock_engine` 393 checks passed, 2 failed - the two version-gate checks (`engineApi(0, 2)` expected non-null, `engineApi(0, 3)`
  expected null), which expect API 0.2 and fail against the 0.3 header vendored before this change. The new `testRadios` passes.
* Headless and window gates: see the report of the lane (the numbers are in its final message and in the commit message of the stage 3 commit).

## What is not covered, in one list

* More than one live receiver: nothing here runs a second radio (stage 4). The focus change, the focus-bump in a real two-radio read and the per-slot spectrum rings are therefore exercised
  only as pure functions (`bumpSeqs`) and through their refusals; the first real two-radio test is stage 4's.
* `read_audio`, `read_iq` and the single-radio lists follow the session focus only in the sense that there is one radio; nothing branches on the focus there yet.
* The transmitter is not claimed on the ledger. No real hardware was used for stages 1 to 3 (fake radios and the generator only).
* The window (classic and Slint) was not changed; it is a one-radio client of radio 1, as the design says for stages 0 to 6.
* Whether the 47-op list is complete for ops added later: it is the one place to add them (`radio_scope.hpp`).
