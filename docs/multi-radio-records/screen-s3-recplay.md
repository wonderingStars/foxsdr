# Screen lane S3: recording and playback that a user could not reach (wave 8, 2026-11-16)

Branch `feat/screen-s3`, cut from `d4f855c5`, worktree `D:/foxsdr-wt/en-recplay`, build folder `D:/foxsdr-build/en-recplay` (moved from C: on the coordinator's order; the old folder
`C:/Users/steve/foxsdr-build/s3` was removed by its exact path, nothing else), scratch `D:/foxsdr-build/s3-scratch/`, hidden desktop `FoxSDRS3`. Nothing is pushed. The file name `screen-s3.md`
already holds the 2026-11-11 RadioWorkspace record of an earlier lane, so this record has its own name.

Real `%APPDATA%/foxsdr/config.json` sha256, read at the end: `c21ce98dc30e50327f7666f7ee29047c0983d130ca3f21716d0c0e20e04603c3 (unchanged; read after the gates)`.

## The rows

| Row | Verdict | What and proof |
|---|---|---|
| G3-08 file-name templates | BUILT | Recorder > "File names": the template is checked on the screen with the model's own sentence and an example name, sent with SET_RECORD_NAME, read back from `fact.record.nameTemplate`. `test_slint_lanes3_host` (names), drive `s3-record` (refused locally, sent, read back, cleared). |
| G3-10 size and time rollover | BUILT | `core::Recorder` swaps to a pre-opened next file between two blocks (no disk work on the DSP thread, try_lock, nothing lost). Settings `rec.roll_mb`, `rec.roll_min`, `rec.roll_align`. `test_record_rollover_s3` (147 checks: two threads, every file in order is one ramp; I/Q by bytes; the next file never arriving; a limit set late; an unused next file handed back). Drive `s3-rollover` (a real audio take on the engine rolls: `fact.radio.1.record.rolled >= 1`). |
| G3-07 sample-format choice | BUILT | I/Q 32-bit float / 16-bit, audio 16 / 24 / 32-bit float, with the cost per second; SET_RECORD_FORMAT; read back from the radio's state and `fact.radio.N.record.format`. Drive `s3-record`. |
| G3-03 squelch-triggered recording (main receiver) | BUILT | `ClipTrigger` / `ClipWriter` (in core, never called) run per radio on a worker over the replay ring and the carrier gate; one file per transmission in `<recordings>/clips`. Settings `rec.clip_*`. Drive `s3-clips` (a clip opens; the splits at the longest clip are saved with no failure). The clip files' content is covered by the existing `test_clip_recorder`, not re-proved here. |
| G3-09 one file per transmission | BUILT | The same path (hang time, shortest kept, longest). |
| G3-02 pre-roll and time-shift | PARTIAL | BUILT: a bounded audio replay buffer (`rec.replay_s`, 0 to 120 s, 96 kB a second per radio, 11.5 MB at most, stated in seconds and MB on the panel and in `fact.radio.N.record.replay`), the clips' time before, and an audio take that begins with the buffer. NOT BUILT: an I/Q pre-roll or time-shift buffer (the audio buffer only; I/Q at 2 MS/s would be 16 MB a second). The buffer's size reaching the radio is proved by drive `s3-clips`; the take beginning with the buffer (`Engine::recplayPreRoll`) has NO test (UNVERIFIED). |
| G3-01 scheduled recording | BUILT | The engine's SCHEDULE_* (already built) now has a panel: local date and time typed, stored as a UTC instant (tested across a clock change: a skipped hour is refused, a repeated one takes the first); list read back in local time with its state; run now, skip, switch off and on, remove with a second press; missed starts shown as a warning. No Windows task, service or wake timer. `test_slint_lanes3_host`, drive `s3-schedule`. |
| G3-04 IF recording | BUILT | Record and stop the passband take (IF_RECORD), read back from `fact.radio.N.ifrec`. Drive `s3-record` proves the stop key is dead while idle; the start itself was not driven (needs a running radio take). |
| G3-13 raw cu8, cs16, cf32, 24-bit and extensible WAV | BUILT | `IqFileSource` plays them through `source/iq_readers.hpp`; a raw file's format and rate are stated by the person (`file-raw:` source id, `core/raw_file_id.hpp`). `test_iq_file_s3` (10 065 checks: samples worked out from the bytes; zero length, one byte, odd byte count, 24-bit, truncated, a wrong guess, a rate of 0). Drive `s3-raw`. |
| G3-12b SigMF playback | BUILT | `open()` plays a `.sigmf-meta` or `.sigmf-data` (rate and centre from the metadata). `test_iq_file_s3`. |
| G3-14a seek bar and A-B loop | BUILT | Slider and typed place (seconds or m:ss), loop start and end, clear; read back from the radio's state. Drive `s3-play`. |
| G3-14b whole-file waterfall | BUILT | `core/overview_job` (own thread, progress, cancel); picture drawn in the Source section. `test_overview_s3` (28 checks). Drive `s3-overview`. A 2 147 483 648 byte file: 0.55 s and 0.57 s (1000 rows, 4096 bins; sampled, not every sample). |
| G3-15 LO metadata from SDRuno and SDRconnect files | PARTIAL | ALREADY BUILT: SDRuno's `auxi` chunk is read on open (`src/source/iq_file_source.cpp`, in `openRiff`: `readWavMeta`, `centerFromHeader_`; `src/source/wav_meta.hpp`). BUILT here: the Source section says "taken from the file's own header" (`fact.radio.N.file.header`). NOT BUILT: SDRconnect's header (`BLOCKED-ROWS.md:28`: no description exists in anything opened; it needs a real SDRconnect recording or SDRplay's description). The `FREQ` chunk stays unused (`BLOCKED-ROWS.md:20`). |

NOT BUILT: the I/Q half of G3-02; SDRconnect in G3-15.

## New engine commands
None. New facts: `record.schedule.ids`, `radio.N.record.format`, `radio.N.file.header`, `radio.N.record.rolled`, `radio.N.record.replay`, `radio.N.record.clips`. New settings (wire-c registry, saved with the other settings): `rec.roll_mb`,
`rec.roll_min`, `rec.roll_align`, `rec.clip_on`, `rec.clip_pre`, `rec.clip_tail`, `rec.clip_min`, `rec.clip_max`, `rec.replay_s`. New source id `file-raw:<format>,<rate>,<centre>|<path>`.

## SHARED FILE EDITS
* `src/slint/ui/main.slint`: one import, one export, five mount lines (four KPanels and the `S3Overview` picture) beside lane M's.
* `src/slint/main.cpp`: two includes, two members, one creation block, one tick line, two answer lines, one `s3.` state line, one line copying the radio's state.
* `src/slint/CMakeLists.txt`: one list line. `src/slint/tests/CMakeLists.txt`: the `lanes3_host` branch (a copy of lane M's).
* `src/engine/engine.hpp`: four declarations, `openIqFile` takes an optional raw id; `engine.cpp`: the `file-raw` source case, one call in the pass, one call in `finishRecordStart`; `engine_api_lists.cpp`: two fact calls; `engine_disk_work.cpp`: the raw open; `receiver.hpp`: one member (declared before the pipeline); `wire_c.cpp`: nine settings rows.
* `src/core/recorder.hpp/.cpp` (rollover, the existing callers' behaviour unchanged), `pipeline.hpp/.cpp` (the replay ring tap beside the audio recorder's), `source/iq_readers.hpp` (four read-only getters), `source/iq_file_source.hpp/.cpp` (open paths; `open()` of a WAV the original reader plays is unchanged).
* `tests/errpath_allow.tsv`: 61 allow lines, reason `validation`/`ui-text` (typed-field and file refusals shown on the screen); the file-name code that E1 calls was not changed.
* `resources/lang/*.json` (159 keys each) and `src/core/lang_assets.hpp`.

## Expectations changed
`tests/test_iq_file_source.cpp`, "24-bit PCM": a STEREO 24-bit file was refused ("bit depth 24"); it now plays. The case now uses a MONO 24-bit file, still refused (by its channel count, "2 channels").

## Error-path gate
Pinned PENDING before: 1126. After: 1126 (never raised; no pending site fixed). The 61 new sites are validation sentences shown on the screen or counted at their reporter; each allow line carries its reason.
A write failure of a rolled file or a clip is said in words (`recordError_`) and counted (`RecordStart::poll` / `noteRecordFailed`); a full disk, a missing folder and an unwritable folder end the extra file with that sentence and the take goes on in the file it has.

## New strings
159 keys in all 33 catalogues, machine-translated by agents and NOT checked by a speaker (catalogue-level `machine: true` as before). Several are sentence fragments that a value follows ("A take would be called", "Times are in", "Skip the next start of", "timed recordings were due while FoxSDR was closed and did not happen...");
agreement with the number may be off in some languages. `tools/i18n_keys.py`: 4016 keys, 0 missing, 0 stale in each; `lang_assets.hpp` regenerated.

## Break-it checks
| Mutation | Red |
|---|---|
| decoding through the readers switched off in `IqFileSource::convertFrames` | `test_iq_file_s3: 10065 checks, 10 failed`; restored, 0 failed |
| `Recorder::rotate` never called | `test_record_rollover_s3: 22 checks, 3 failed`; restored, 147 checks, 0 failed |
| `SET_RECORD_NAME` not sent (drive level) | `s3-record` RED: `state s3.fact.record.nameTemplate == {freq_mhz}/{mode}_{datetime}` -> it is "" ; restored, green |
| `FILE_SEEK` not sent (drive level) | `s3-play` RED: `state s3.play.pos > 7.99` -> it is 1.52 (the file is paused first, so only the seek can move the position); restored, green |
A defect the drive break-it exposed in my own panel: the playing position changed the whole Source list every tick, so a field being typed into lost its text. The position and slider are now a list of their own (`rows-play`).

## Gates
Build folder `D:/foxsdr-build/en-recplay`, Release, all targets built (1079 projects, exit 0), hidden desktop `FoxSDRS3`, `FOXSDR_FRAME_CAP_HZ=60`, a busy shared machine.
* Headless/slint gate `ctest -L "headless|slint" -j3 --timeout 300` (1170 tests, at the part-4 tip): `99% tests passed, 5 tests failed out of 1170`, 2170.83 s. Reds: `test_converter_call_sites` (a known false failure); `test_thread_inventory` and `test_health_paths` (MINE: the new threads and the `TakeHeader` site had no row; fixed, both pass); `test_slint_tools` (a pixel check of the markers sheet geometry in `tools_specimen.slint`: fails alone, renders nothing of mine, UNVERIFIED at the base commit; not mine); `telemetry_worker` (`the fixture directory matches its manifest` fails: the Worker's fixture folder, not mine).
* After the fixes, re-run alone and together: `test_thread_inventory`, `test_health_paths`, `test_error_paths`, `test_i18n`, `test_slint_lanes3_host`, `test_iq_file_s3`, `test_iq_file_source`, `test_overview_s3`, `test_record_rollover_s3`: all pass; `test_slint_tools` and `test_converter_call_sites` fail alone, as above.
* GUI gate `ctest -L gui -j3 --timeout 600` (391 tests, at the part-5 tip): `99% tests passed, 5 tests failed out of 391`, 3277.07 s. Reds: `test_health_app` (a load flake: passes alone, 32.63 s); `slint_drive_patch-band` and `slint_drive_transmit-page` (MINE: my always-open Source rows pushed the Radio and Transmit rows out of reach; fixed by folding the raw-file and waterfall rows behind one tick, "Raw files and the whole-file waterfall": both pass alone afterwards, and the Transmit row that lane M's record left unclassified is reachable again); `slint_drive_layout-section-overlap-1600` and `-624` (`"Read the pitch now" bottom <= 2028` reads 2058: the CW-surround key sits in the Audio filters section, above every row of mine; the script pins absolute pixels that the rows of other lanes also move; fails alone; UNVERIFIED at the base commit, so NOT classified as not mine with proof).
* All eight `slint_drive_s3-*` scripts pass at the final tip (clips, overview, play, raw, record, rollover, schedule; run together with the layout scripts).
* The stamp tests were not re-run after the last rebuild (they are inside the headless gate, which ran at the part-4 tip).
* The ledger test is inside the headless gate; no new row was added to the pointer ledger (the S3 scripts carry no `coverage-*.tsv`).

## UNVERIFIED
* A real pointer, a real screen reader and the real file chooser: none was used (no chooser is offered by these rows; paths are typed).
* The audio take that begins with the replay buffer (`recplayPreRoll`): written, not tested.
* A clip file's audio and the clips' file names against a real transmission: the counters were driven, the files were not opened.
* A rollover on a real slow or full disk: the failure path is written (sentence and count) and was not driven; the I/Q rollover through the engine (only audio was driven; the recorder is the same code for both and was tested for both).
* The 2 GB figure is for a sampled overview on this PC's disk, once it is warm.
* Machine translations: unchecked; fragments may read oddly beside their values.

## OPEN ROWS FOR THE NEXT LANE
| Row | What is left | Kind of lane |
|---|---|---|
| G3-02 I/Q | an I/Q pre-roll / time-shift buffer in the pipeline (bounded, stated in seconds and MB) | engine |
| G3-15 SDRconnect | its header layout needs a real recording or SDRplay's description | needs the owner (a sample) |
| Mouse drive | every new control with a real pointer and a screen reader; the pre-roll start of an audio take; a clip's audio; a full-disk rollover | mouse drive |
| Translations | a speaker's check of the 159 strings in 32 languages, the fragments first | translation check |
| Layout scripts | `slint_drive_layout-section-overlap-1600` / `-624` red (2058 against 2028): find whether the base commit already has it, then move the pinned pixel | screen, small |
| `test_slint_tools` | markers-sheet pixel geometry red alone: not touched by this lane, not checked at the base | screen, small |
| Clip names | clips are named `radioN_clip_...` and do not follow the file-name template | engine, small |
