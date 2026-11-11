# recplay lane record (2026-11-11)

Branch `feat/recplay`, cut from `engine/multi-radio` d3630d78. Local only, not pushed, not merged, no version bump.
New-file model lane: models, parsers and stores in new headers, tests in new test files. Nothing is wired.

## Step 0: what each row needs (reading only, done before any code)

Searched in this base for `speed`, `pause`, `step`, `daily`, `annotation`, `project`, `auxi`, `scrollback` under `src/core`, `src/source`, `src/dsp`;
read `docs/feature-records/recording-models.md`, `signal-blocks-2.md` and `core-formats.md` from the three sibling branches
(`feat/recording-models`, `feat/signal-blocks-2`, `feat/core-formats`). None of those three headers is in this base, so nothing here includes them:
every piece below is a separate file with its own small interface, and the join is listed in the hand-off.

| Row | Class | What exists already (not duplicated) | Decision |
|---|---|---|---|
| Playback speed, pause, step | new file model | `feat/recording-models` `source/iq_playback.hpp`: seek, A-B loop, generation rule, audio-only speed 0.25-4 by linear interpolation, I/Q refused anything but 1.0; **no pause, no step, no 0.25-8 for I/Q, no pitch policy** | `source/playback_transport.hpp`: speed 0.25-8 for I/Q by two stated policies, pause, step by N blocks, loop state, over a tiny `TransportSource` interface; the join to `PlaybackModel`'s `FrameSource` is a four-line adaptor (hand-off) |
| Lock the radio while recording | new file model (a pure predicate); the engine hook needs a forbidden file | nothing | `core/record_lock.hpp`; wiring hand-off |
| Daily subfolders | new file model | `feat/recording-models` `core/rec_naming.hpp`: templates with `{date}` and up to three folders, UTC/local by offset, `resolvePath` containment | `core/rec_daily.hpp`: date-folder styles and a containment-proving folder resolver of its own (the template engine is not in this base); the join is `recname::expand` output passed to `resolveInRoot` |
| Analyser project (crop, tags, click-to-sync) | new file model | nothing | `core/analyser_project.hpp` (JSON v1, SHA-256 of its own, linked cursors) |
| Time-frequency box export as filtered I/Q or demodulated samples | new file model | `feat/core-formats` `source/rf64_writer.hpp` writes I/Q WAV (not in this base); nothing filters a box | `dsp/box_export.hpp` with its own WAV writers |
| Annotations saved to SigMF | new file model | `feat/core-formats` `source/sigmf_writer.hpp` has `addAnnotation` while recording (spec keys: sample_start/count, freq edges, label, comment); `iq_readers.hpp` READS annotations | `source/sigmf_annotations.hpp`: edit annotations of an existing `.sigmf-meta` (merge, sort, atomic write, round trip); the writer's live path stays the writer's |
| Pause and scroll back the live waterfall | new file model; the screen is a forbidden file | nothing | `core/waterfall_history.hpp`; screen hand-off |
| Attach own screenshots, clips, notes to a signal record (Part B) | new file model | nothing | `core/record_attachments.hpp` |
| P16 LO metadata from third-party WAV (core file-source row) | new file model (reader of the header); the wiring into `iq_file_source.cpp` is a forbidden file | `iq_readers.hpp` (signal-blocks-2) guesses from the file NAME only; it does not read `auxi` | `source/wav_meta.hpp`: chunk walker + `auxi` + Spectrum Lab `inf1`; hand-off into the source |

