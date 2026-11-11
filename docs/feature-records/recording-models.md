# Recording and operating-aid models (2026-11-11)

Branch `feat/recording-models` (local, cut from `266926bf`, not pushed, not merged, no version bump). Nine stand-alone parts for the "Recording and playback",
"Tuning and operating aids" and "Scanning and memories" sections of the owner's gap list (`FEATURE-GAPS.md`) and their tests. **Nothing is wired**: no
existing source file was touched, and not even `tests/CMakeLists.txt` (the test glob picks the new files up after a re-configure). Conventions follow
`signal-blocks.md` and `signal-blocks-2.md`: header-only, tests with `test_check.hpp`, `AllocProbe` from `tests/alloc_probe.hpp`, generators from
`tests/signal_block_gen.hpp`, a break-it table per behaviour.

| Part | Header | Test | Checks |
|---|---|---|---|
| 1 FLAC writer | `src/core/flac_writer.hpp` | `tests/test_flac_writer.cpp` | 590 |
| 2 MP3 | none: report below (an MP3 writer already exists) | none | n/a |
| 3 File names and rollover | `src/core/rec_naming.hpp`, `src/core/civil_time.hpp` | `tests/test_rec_naming.cpp` | 494 |
| 4 Recordings browser | `src/core/rec_browser.hpp` | `tests/test_rec_browser.cpp` | 125 |
| 5 Playback position model | `src/source/iq_playback.hpp` | `tests/test_iq_playback.cpp` | 214 |
| 6 IF (channel) recording | `src/core/if_recorder.hpp` | `tests/test_if_recorder.cpp` | 1207 |
| 7 Frequency / step lock, band stacking, VFO A/B, per-mode memory | `src/core/operating_aids.hpp` | `tests/test_operating_aids.cpp` | 25497 |
| 8 Priority channels, lockouts, scan log, mute while scanning | `src/core/scan_aids.hpp` | `tests/test_scan_aids.cpp` | 2255 |
| 9 Command-line parser, UTC / zone clock | `src/core/cli_options.hpp`, `src/core/status_clock.hpp` | `tests/test_cli_clock.cpp` | 200266 |

## What existed already, or is another lane's (searched before each part)

Searched in this worktree and, read-only, in `mr-engine1`, `mr-screen1`, `feat-gaps`, `feat-blocks`, `feat-knobs`, `feat-net`, `feat-web` and `feat-kiwi` for
`flac`, band stacking, lockout, priority channel, scan log, recordings browser, IF / channel recorder, A-B loop, file-name template, VFO B / swap, per-mode
memory, command-line parsing and a status clock. The only hits are unrelated and identical in every tree: `diag_log.cpp` lists `flac` among upload-able
extensions, `feature_request.*` and `fox_engine_api.cpp` have a LOGIN lockout, `engine.cpp` has a plugin-rescan log, `pipeline.*` matched `vfoBandwidthHz`.
`RELEASE-CHECKLIST.md` E2 to E5 were read first; none of the nine parts is listed as built in a lane.

| Feature | What exists | Decision |
|---|---|---|
| One file per transmission (squelch open / close, minimum length, hang time) | `core/clip_recorder.hpp` `ClipTrigger` / `ClipWriter` (signal-blocks lane): pre-roll, hang tail, minimum length, maximum clip length, `index.csv` | **SKIPPED, not duplicated.** `recname::expand()` can name its clips (hand-off 3). |
| Replay (time shift), timed recordings, satellite-pass capture, listening log, bookmark import, knobs, layouts | other lanes (`core/replay_ring.hpp`, `core/schedule.hpp` in `mr-engine1`, `core/adif.hpp`, ...) | not touched |
| MP3 | `core/mp3_writer.*` + `core/mp3_support.hpp`: Windows only, through Media Foundation's own encoder, used by the patch Speaker page only; test `test_mp3_writer` | **No encoder written.** See Part 2 |
| Raw / WAV / RF64 / SigMF readers | `source/iq_readers.hpp` (signal-blocks-2) | built on, not changed |
| Present recorder | `core/recorder.*`: float32 I/Q WAV and PCM16 mono WAV, local-time name `<prefix>_<YYYYMMDD>_<HHMMSS>_<rate>Hz.wav`, a name reused within the same second, data capped just under 4 GiB | not changed; Parts 1, 3 and 6 are what could replace its naming and formats |

## Sources and what is and is not verified

| Fact | Source read this session | Status |
|---|---|---|
| FLAC: STREAMINFO, SEEKTABLE, frame header and its code tables, UTF-8-like coded number, CRC-8 0x07 and CRC-16 0x8005 (init 0), subframe types, fixed predictors, Rice coding (4- and 5-bit parameters, folding), stereo decorrelation, MD5 definition | RFC 9639 "Free Lossless Audio Codec (FLAC)", https://www.rfc-editor.org/rfc/rfc9639.html, read 2026-11-11 through a fetch tool: its answer covered characters 0 to 100000 of 179404, and was a summary with short quotes | the part read agrees with ffmpeg on 24 files (below); the last 79404 characters were **not read** |
| MD5 | RFC 1321 test suite (seven vectors) typed from memory and the Windows CNG MD5 as a second implementation | verified by running both |
| RF64 / ds64, JUNK reserved as ds64 | EBU Tech 3306 v1.0, read for the previous batch (see `signal-blocks-2.md`) | as before |
| MP3 patents, the Windows MP3 encoder | `https://en.wikipedia.org/wiki/MP3` (patent section) and `https://learn.microsoft.com/en-us/windows/win32/medfound/mp3-audio-encoder` ("MP3 Audio Encoder - Win32 apps", page dated 2018-05-31, updated 2021-08-19), both read 2026-11-11 through the fetch tool | see Part 2; the legal reading is not advice |
| What other programs do (lock, band stacking, VFO A/B, priority, lockouts, scan metrics, mute while scanning) | the one-line rows of `FEATURE-GAPS.md` only | **no program was read**; every default is marked as the owner's to change |
| ffmpeg as a FLAC-reading program already on this PC | `ffmpeg version 9.0.2-full_build-www.gyan.dev`, found on the PATH; nothing was installed or downloaded | used as an independent decoder; see "A claim corrected" below |
| The machine's time zone | `[TimeZoneInfo]::Local` in PowerShell: `GMT Standard Time`, offset 0 min on 2026-11-11 and 60 min on 2026-07-01 | equals what `systemUtcOffsetMinutes` returned (0 and 60) |

Clean room: no source of another SDR program or of a GPL / LGPL project was read; no file was downloaded; no outside service was contacted (web pages
were read for the three documents above only).

## Part 1: FLAC writer (`core/flac_writer.hpp`)

**What it is.** 16- and 24-bit, mono and stereo. Per channel and frame: constant subframe, verbatim, or fixed predictor order 0 to 4 with partitioned Rice
coding (partition order 0 to 8, 4-bit parameters, 5-bit parameters when a parameter exceeds 14, exact bit counts for three candidate parameters per
partition), wasted-bits detection, and for stereo the cheapest of independent / left-side / side-right / mid-side by exact coded size (the side channel
carries one more bit). STREAMINFO is patched at the end with minimum and maximum frame size, total samples and MD5. Optional SEEKTABLE: `seekPoints`
placeholders at open, filled with evenly spaced frames at the end. Block size 256 to 32768 fixed (default 4096), sample rates by the header's code table or
the 8-bit kHz / 16-bit Hz / 16-bit tens-of-Hz forms or STREAMINFO. `AsyncRecorder`: `push()` is wait-free and allocation-free into a single-producer ring,
a worker thread converts (clamp the double, then round; NaN and infinity read as 0), encodes and writes; a full ring drops and counts.

**Crash contract.** `open()` writes `fLaC`, a STREAMINFO with total samples 0 and MD5 0 (both legal "unknown"), the seek-table placeholders, and flushes; every
frame is flushed. A file copied after four blocks without `finish()` decodes 16384 samples in the test decoder and in ffmpeg.

**Proof (real runs, `test_flac_writer`).** The decoder in the test is written from the RFC, not from the writer (own bit reader, bitwise CRCs, own frame
parser, own MD5 plus the Windows CNG MD5).
- **24 round-trip cases** (mono / stereo, 16 / 24 bit, rates 5000, 8000, 12345, 16000, 22050, 24000, 32000, 44100, 48000, 88200, 96000, 176400, 192000, 700001
  Hz, block sizes 256 to 32768 including the 576 / 1152 / 2304 / 4608 and the 16-bit uncommon forms, lengths 0, 1, 15, 255, 4095, 4096, 4097 and up to 70000):
  every sample identical, CRC-8 and CRC-16 pass, STREAMINFO min / max frame size equal the real frame sizes, MD5 equal in three implementations.
- **ffmpeg** decodes the same 24 files with CRC checking on and an empty error output, PCM byte-identical, and **ffmpeg's own MD5 of its decoded PCM equals the
  file's STREAMINFO MD5**. ffmpeg also decodes the seek-table file and the crash snapshot.
- Full-scale square and random-rail input at both depths, a DC rail (constant subframes), a constant side channel wider than the sample depth (`bps + 1`
  bits), 16-bit samples with three low bits zero (wasted bits: 5041 bytes down to 3700), L = R (15927 bytes against 15915 for the mono file; the writer picks a
  decorrelated assignment), all four stereo assignments seen on correlated input.
- **300 of 300 single-bit corruptions of a valid file are refused by the test decoder** (so the round trips mean something); seek table of 10 points on 100000
  frames (every offset lands on a frame header with the stated first sample), 8 placeholders last on a short file.
- **Compression (10 s mono 16-bit 48 kHz, size as a percentage of the raw PCM):** speech-like 23.5 % (ffmpeg's FLAC encoder on the same input: level 0 29.9 %,
  level 5 21.8 %; pinned within 10 % of level 5), full-scale white noise 100.12 % (verbatim plus about 16 bytes of frame overhead per 4096 samples), uniform
  +-2048 noise 78.2 % (12 bits of entropy of 16 = 75 %), sine at 0.5 22.5 %, silence 0.140 %, 24-bit stereo speech-like 20.9 %.
- **Speed:** 10 s of 48 kHz stereo 16-bit speech-like in 0.115 s including disk (87 times real time, one run under load).
- **Async:** 6 s stereo from a producer thread in 480-frame pushes: 0 dropped, 0 allocations in 600 `push()` calls, slowest push 40.7 us (136 us in a later,
  more loaded run; pinned below 3 ms), file equals the expected quantisation including NaN, infinity and the rails; a 200000-frame burst into a 1024-frame
  ring accepts 1024, drops 198976, returns in 0.001 ms.

**Break-it** (patch, rebuild, check RED, `git checkout`):

| Removed or broken | RED |
|---|---|
| CRC-16 last byte wrong | 73 checks |
| MD5 never updated | 46 |
| Wasted-bits detection off | 2 |
| Stereo decorrelation off (cost starts at 0 for independent) | 7 |
| Mid-side with `/ 2` instead of `>> 1` | 8 |
| Side channel written at `bps` instead of `bps + 1` | 30 (**first test set had 0**: the constant-wide-side case was added after this row) |
| Predictor orders limited to 0 | 2 (compression pins) |
| Overflow drops not counted | 1 |
| STREAMINFO never patched | 124 |
| Clamp after rounding instead of before (see "A bug found") | 2 |

**Limits.** Fixed predictors only (no LPC): 23.5 % against ffmpeg's best-level 21.8 % on speech-like input. Not tested: a real recording, subset-conformance
flags, seeking by a player, a full disk.

**WIRING HAND-OFF.** Files the wiring lane changes: `src/core/recorder.hpp/.cpp` (a third audio format beside PCM16 WAV; the recorder's finish path is
`core/record_finish.*`), `src/core/config.hpp` (settings below), `src/engine/fox_engine_api.*` (ops), the recording page and its host logic under `src/slint/`
(`ui/pages.slint`, `host_logic.cpp`; which page holds the recording controls was not checked). Ops: `SET_RECORD_FORMAT` (0 WAV, 1 FLAC, 2 MP3 where available),
`SET_RECORD_BITS` (16 / 24). Settings: `recordFormat`, `recordBits` (default 16), `recordFlacSeekPoints` (default 0). The audio thread calls `AsyncRecorder::push`
and nothing else; `start()` and `stop()` on the control thread; read back `droppedFrames()` and `failed()` and show them like the existing "Saving state".
A FLAC file is named by Part 3 with extension `flac`. Strings: "FLAC (lossless)", "Bits per sample", "Recording stopped: the disk is full or the folder is gone",
"N audio frames were dropped while recording".

## Part 2: MP3 (report only; no encoder written)

**An MP3 writer already exists** and needs no encoder of ours: `core/mp3_writer.hpp/.cpp` (244 lines) feeds 16-bit PCM to a Media Foundation sink writer on a
`.mp3` path, so Windows' own encoder does the work (`MFTranscodeGetAudioOutputAvailableTypes`, `MFCreateSinkWriterFromURL`). It is used by the patch Speaker
page only; the main recorder writes WAV. Its test passes on this PC: `test_mp3_writer: 118 checks, 0 failed` (it decodes the written file back: 92784 samples
at 48 kHz, a 1 kHz tone with peak 16000).

- **What Windows offers** (Microsoft Learn, "MP3 Audio Encoder", read 2026-11-11): a Media Foundation Transform, class id `CLSID_MP3ACMCodecWrapper`; input only
  16-bit integer PCM (no float), mono or stereo; MPEG-1 at 32 / 44.1 / 48 kHz with 32 to 320 kbps (32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256,
  320), MPEG-2 at 8 to 24 kHz; defaults 128 kbps mono and 320 kbps stereo; "Minimum supported client: Windows 8 [desktop apps only]". It does not say VBR is
  supported (constant bit rates are listed). Windows only: Linux and macOS builds have no system MP3 encoder this application could call.
- **Size of writing an encoder ourselves:** not estimated from a source (the ISO/IEC 11172-3 text was not read and is a paid standard; its price is not
  verified). A bare constant-bit-rate layer III encoder with a psychoacoustic model is a large piece of work and its quality would be the owner's to judge;
  this is a judgement, not a measurement. Not recommended while Windows has one and FLAC covers lossless.
- **Patent position** (Wikipedia "MP3", read 2026-11-11, which cites Technicolor and Sisvel material; the primary documents were not read): the page says MP3
  became substantially patent-free in the US on 16 April 2017 when US 6,009,399 expired; Sisvel's last three US patents expired in February 2017
  (5,878,080 and 5,850,456) and 9 April 2017 (5,960,037); in the EU all relevant patents had expired by 2012; Sisvel's licensing program "has become a legacy" by
  about the first quarter of 2023. This is what a web page says, not legal advice.
- **LAME and other encoders** (LGPL source or binaries): not downloaded, not read (owner rule: nothing third-party is installed).
- **Options for the owner:** (a) wire the existing writer into the main recorder as an MP3 choice, Windows only, 48 kHz audio (the product's rate) 16-bit; (b)
  nothing more: FLAC (Part 1) for lossless on every platform; (c) a bundled encoder: needs your licensing decision and a download, so it is not started.

## Part 3: file names and rollover (`core/rec_naming.hpp`, `core/civil_time.hpp`)

**Templates.** Tokens `{date} {time} {datetime} {year} {month} {day} {hour} {min} {sec} {ms} {tz} {freq} {freq_khz} {freq_mhz} {mode} {radio} {bookmark}
{counter}` and `{counter:N}` (1 to 9 digits); `{{` and `}}` are literal braces; `/` and `\` separate up to three folders. UTC or local time (the caller passes the
offset; this layer is pure). A template is **refused whole** with a message for: characters Windows forbids (`< > : " | ? *` and control characters), a
leading separator or a drive / UNC form, `.` and `..` components, empty components, a trailing dot or space (Windows strips them), reserved device names
(`CON PRN AUX NUL COM0..9 LPT0..9`, any case, with or without an extension), unknown tokens, unclosed or stray braces, a bad counter width, more than three
folders, a name over 200 bytes or a component over 120. **Run-time values** (a bookmark called `BBC R4 (LW)/test`) are sanitised, not refused: forbidden
characters and separators become `_`, `.` / `..` become `_`, a reserved name gets a `_` prefix, trailing dots and spaces and leading spaces go, the value is cut
at 64 bytes without splitting a UTF-8 sequence, an empty value becomes `unknown`. `resolvePath()` joins base, name and extension and proves the result is inside
the base and under 240 characters; `uniquePath()` appends `-2`, `-3` ... and `createExclusive()` creates the file only if it does not exist (the present recorder
reuses a name within the same second).

**Rollover (`Rollover`).** By size (the header counts), by time, both, or on a wall-clock boundary (the first file ends at the next multiple of the period since the
epoch, UTC; later files are full periods). It cuts the frames of each write into segments, so the hand-over is sample exact. For compressed output the size limit
applies to the raw size (an upper bound). `kWavMaxBytes` is the 32-bit RIFF ceiling the present recorder works to.

**Proof (`test_rec_naming`, 494 checks).** The calendar (days to date and back, weekday, offsets) agrees with `std::chrono` for **109938 days, 1900 to 2200**.
20 golden template rows worked out by hand (including local time across midnight and the year), 43 hostile templates refused with a message and an empty
result, 18 run-time value rows, the UTF-8-safe cut. **Fuzz: 200000 random templates over a hostile alphabet with random hostile values: 13748 accepted, every one a
safe relative path (no forbidden character, no `..`, no empty or dotted or reserved component, at most three folders), none escapes the base folder; 186252
refused.** `createExclusive`: four threads racing for one name, 50 rounds, exactly one winner each time; a losing call leaves the existing file's bytes
alone. **Rollover: 10500 frames in 333-frame writes give 11 files (10 x 1000 and 500), each within the byte limit including the 44-byte header; 200 random
policies (frame size, header, slack under one frame, size and time limits, random write sizes): the files in order reproduce the input exactly and every file
is full except the last.** Clock alignment: start 12:34:50 with a 600 s period gives a first file of 310 s; a start on the boundary gives a full period; 1 ms
before it gives 48 frames; negative times align.

**Break-it:**

| Removed or broken | RED |
|---|---|
| Colon allowed | 11 |
| `.` / `..` and trailing-dot rules both removed (each alone: 0, they overlap on `..`) | 22 |
| Reserved device names allowed | 24 |
| Trailing dot allowed | 13 |
| Run-time values keep `/` and `\` | 5 (rows and the fuzz) |
| UTF-8-unsafe cut | 1 |
| Size cap ignores the header | 188 |
| Clock alignment ignored | 4 |
| Floor division replaced by truncation (negative times) | 5 |
| `createExclusive` opens with `wb` (overwrites) | 4 |
| Containment check removed | 2 |

**WIRING HAND-OFF.** `core/recorder.cpp` `makeFilename` is replaced by `recname::expand()` + `resolvePath()` + `createExclusive()`; `core/clip_recorder.hpp` names
its clips the same way; the rollover splitter sits in the file worker that already writes (`core/record_finish.*`), one `Segment` per write. Ops:
`SET_RECORD_TEMPLATE`, `SET_RECORD_FOLDER`, `SET_RECORD_ROLLOVER` (megabytes, minutes, align to the clock). Settings: `recordTemplate`, `recordFolder`,
`recordRolloverMb`, `recordRolloverMinutes`, `recordRolloverAlign`, `recordNameUtc`. Suggested default template (the owner's call):
`{radio}_{datetime}_{freq_mhz}MHz_{mode}`. The template field shows the expand error text verbatim. Strings: the token list "Date, time, frequency, mode, radio,
bookmark, counter", and every error text in `rec_naming.hpp` verbatim.

## Part 4: recordings browser (`core/rec_browser.hpp`)

**What it is.** `RecordingsBrowser::start(folder, options)` scans on a worker thread (links and junctions not followed, depth 3, 20000 entries), reads only
headers and publishes snapshots (a shared pointer under a short mutex) that the screen polls. Per file: kind (I/Q or audio; **guessed** for two-channel WAV from
an `iq` token or a recorder prefix in the name and a rate above 48 kHz), container, format, sample rate, centre (SigMF first capture, a raw file's name, or a
FoxSDR IF recording's header), frames, duration, **start time** (SigMF `core:datetime`, an IF header, a file-name date-time with `Z` or local, else modified time
minus duration, with the source recorded), truncated and broken flags with the error text. A `.sigmf-data` next to its `.sigmf-meta` is not listed twice; a raw
file with no rate in its name is listed with the length from its size and no duration. `sortAndFilter()` is pure: eight sort keys, both directions, **unknown
values last in both directions**, stable ties; text, kind, centre range, duration range, start range, hide broken.

**Proof (`test_rec_browser`, 125 checks).** 19 file-name time rows (compact, dashed, leap day, impossible dates), the SigMF ISO 8601 forms; 22 files in one folder
(WAV mono, stereo, float, 24-bit, ADPCM, junk, empty, truncated, a lying size, a directory named `x.wav`, an exclusively locked file, raw with and without a
guessable name, SigMF pair and orphan, FLAC and a crash-snapshot FLAC); a sparse RF64 of 1,200,000,000 frames (4.80 GB) lists in 0.022 s with duration 600.0 s;
**4400 files in 40 x 3 folders (400 of them random junk) scanned in 0.44 s with 26 partial snapshots seen and the slowest `snapshot()` call 0.001 ms**; sorting
those 4400 entries takes 0.7 to 0.9 ms; cancel right after start returns in 0.1 to 0.3 ms; a second `start()` replaces the first (scan id 2); destruction while
running joins cleanly; entry cap, missing folder, a file where a folder is expected, an empty folder. A directory junction and a junction cycle are not followed
(the first version followed them: found by this test).

**Break-it:** unknown values not last 4 RED; junctions followed 1; `.sigmf-data` listed twice 1; truncated flag off 1; entry cap off 1; a file name beating the SigMF
datetime 2.

**Limits.** Symlinks (as opposed to junctions) could not be created without a privilege here, so the symlink branch is the same code path but was not exercised.
The name patterns are the conventions I recall for SDR#, HDSDR, SDRuno and SDRconnect file names (UNVERIFIED which of them write what).

**WIRING HAND-OFF.** Replaces the I/Q file row's folder logic and `core/patch_recordings.*` (which lists `.wav` only and accepts what `IqFileSource` accepts) with
one list; the screen shows `Entry`, calls `snapshot()` from its timer and `sortAndFilter()` on a copy. Ops: `SCAN_RECORDINGS` (folder), `OPEN_RECORDING`
(path, with rate and centre for raw). Settings: `recordingsFolder`, `recordingsSortKey`, `recordingsSortAscending`, `recordingsFilter*`. Strings: columns "Name",
"Started", "Length", "Rate", "Centre", "Kind: I/Q / Audio", "(guessed)", "Start from: header / file name / file date", "Broken", "Cut short", "Scanning N of M".

## Part 5: playback position model (`source/iq_playback.hpp`)

**What it is.** `PlaybackModel` over a `FrameSource` (`IqReaderSource` wraps the previous batch's readers; `WavAudioSource` reads WAV audio of 1 to 8 channels
as the mean of the channels). Seek by frame, seconds and fraction (clamped; NaN refused without moving anything), A-B loop that wraps **inside one read call**
(frame B-1 is followed by frame A), mark A / mark B from the playing position, loop in seconds, audio speed 0.25 to 4.0 by linear interpolation (varispeed: the
pitch follows the speed; I/Q sources accept 1.0 only), `position()` for a seek bar (frame, seconds, fraction, duration, loop, speed, generation, at-end), 64-bit
everywhere. **Seek while playing:** one reader thread calls `read()`, any thread seeks; a seek records the request under a short mutex, bumps the generation and
moves the published position at once; `read()` applies a pending seek before it produces anything, returns the generation it read under, and publishes the
position only if that generation is still current. A queue between reader and pipeline drops blocks whose generation differs from `generation()`.

**Proof (`test_iq_playback`, 214 checks).** Raw cf32 files hold a ramp (I = frame, Q = -frame / 2) so every frame names its own position. 1,000,000 frames in
403 blocks of odd sizes read in order, end flagged once; each seek form's first block verified; **a block held inside `read()` while a seek arrives comes back
with the old generation, the bar stays at the target, and the next block starts exactly there; stress: 300 random seeks against a reader running flat out: 145859
blocks kept (all contiguous ramps), 33 dropped as stale, the first block kept after the last seek starts at its target.** Loop [1000, 2500) read in 700-frame
blocks over 8400 frames: gapless across five wraps, every block full; seek past B restarts at A; one-frame loop; loop ending at the end of the file never reports
the end. Speed 0.25, 0.5, 1, 1.5, 2, 3, 4 equal linear interpolation to 2e-6 and advance the source by speed x frames; no jump when the speed changes; a seek resets
the interpolator. WAV source: 8 / 16 / 24 / 32-bit PCM, float32, mono, stereo and three channels; junk, ADPCM, a lying size field, zero frames and **a file that
shrinks under the reader**. **Over 4 GB:** a sparse RF64 of 1.2e9 frames: fraction 0.75 gives frame 900,000,000 at 450.0 s, the last frames read their known
bytes, a loop across the 4 GiB byte offset plays full blocks.

**A bug found.** The first `WavAudioSource` used a buffered stream, which served stale data from a file that had shrunk (the read returned 1000 frames where 500
existed); it now reads unbuffered with `fread` on a `setvbuf(_IONBF)` stream. The previous batch's `IqFileReader` uses a buffered `ifstream` and may have the same
property for a file that shrinks while playing (not tested, not changed).

**Break-it:**

| Removed or broken | RED |
|---|---|
| Seek does not bump the generation | 3 |
| Position published without the generation check | 1 |
| Loop wrap missing | **hang**: the unguarded read loop never ends (killed after 120 s); the model's own wrap was written twice (before the loop and inside it), removing only the first gives 0 RED |
| Interpolation replaced by the previous sample | 3 |
| Fraction not clamped | 1 |
| Speed allowed on I/Q | 1 |
| WAV reads buffered again | 3 |
| Seek past B not honoured | 2 |

**WIRING HAND-OFF.** `source/iq_file_source.*` (a looping 2-channel reader with no seek) is replaced or fronted by `PlaybackModel`; its file-pacing thread is the
reader thread. Ops: `SEEK_PLAYBACK` (fraction or seconds), `SET_PLAYBACK_LOOP` (a, b in seconds or clear), `SET_PLAYBACK_SPEED`; state: `position()` fields.
Settings: `playbackSpeed` (default 1), `playbackLoopEnabled` (not saved across files). The screen's seek bar reads `fraction`. **FLAC playback is not built**: there is
no FLAC decoder in `src/` (the test decoder handles only what this writer produces); a FLAC recording would need a decoder with LPC subframes, or the owner may
prefer to hand FLAC to the system. Strings: "Loop A", "Loop B", "Clear loop", "Speed", "Position", "Seek".

## Part 6: IF (channel) recording (`core/if_recorder.hpp`)

**What it is.** The channel-rate I/Q after the channel filter as a WAV (float32 bit-exact or PCM16), two channels, that **every I/Q reader of the previous batch opens
unchanged**, plus one private chunk `fxif` (JSON, at most 4096 bytes, even-padded) with sample rate, the RF frequency of the channel centre, mode (cut at 32
bytes), bandwidth, optional pass-band edges, radio name (cut at 64 bytes), start time and the tuner centre. **No size cap:** a reserved `JUNK` chunk of 28 bytes
is rewritten at `finish()` as `ds64` and the header as `RF64` (EBU Tech 3306) when the data passes 4 GiB, and back to plain RIFF for a smaller claim. Crash
contract: a zero-length header at open (a reader that treats size 0 as "to the end" salvages every flushed frame), one flush per `write()`, and
`checkpoint()` (the async recorder calls it every two seconds) rewrites the sizes so a crash loses at most one interval. `readIfHeader()` is bounded and refuses
hostile headers. `AsyncIfRecorder`: wait-free allocation-free `push()`, one worker, drops counted. The browser (Part 4) lists these files as I/Q, not guessed, with
their centre, mode, bandwidth, radio and start time.

**Proof (`test_if_recorder`, 1207 checks).** 100000 frames in 1234-frame calls: metadata exact, the previous batch's reader returns all frames bit for bit,
header bytes laid out by hand (RIFF, JUNK at 12, `fmt ` at 48 with tag 3, 2 channels, byte rate and align, `fxif` at 74, data at an even offset, sizes = file size - 8).
PCM16: rounding half away from zero, saturation at both rails, NaN and infinity written as 0, **huge finite values (1e30, 3.4e38) written as the rails**. Metadata text:
quotes, backslash, control characters, unicode, invalid UTF-8, odd and even padding. Refusals: rate 0, negative, NaN, over 1 GHz, bad centre, bandwidth, edges, tuner
centre, missing folder, double open. **Crash: a copy taken before the first checkpoint declares 0 and the reader salvages all 3000 frames; after a checkpoint it
declares exactly 3000 of the 5000 written.** **Over 4 GB:** a header rewritten for 5.0e9 data bytes is `RF64` with `ds64` (riffSize, dataSize 5e9, sampleCount 6.25e8,
data field 0xFFFFFFFF); extended sparsely to that size it reads back as 625,000,000 frames with the last frame intact; riffSize exactly at the limit stays RIFF, 2
bytes more is RF64. **Hostile headers:** every truncation of the header refused until complete (304 refused, 21 ok), ten corrupt-JSON rows, a plain WAV without
the chunk, 4000 random header mutations (532 still read, 3468 refused, every accepted one valid), no crash. Async: 3 s at 48 kHz: 0 dropped, 0 allocations in 300
pushes, slowest push 44.1 us, 5 checkpoints at 100 ms, file bit exact, a snapshot taken mid-recording (23520 frames) is a true prefix.

**A bug found.** The PCM16 converter (and the FLAC one) clamped after rounding. The Windows C runtime's `lround` / `llround` return **0** (not the rail) for NaN, infinity
and every value outside the integer range (`ucrtbase` `lround(1e30)` = 0, checked through ctypes), so a huge finite sample became silence. Both now clamp the double
first; the tests gained 1e30 and 3.4e38 rows and were shown RED against the old code (3 and 2 checks). The NaN / infinity guard is redundant on this runtime (removing it
gives 0 RED) and is kept for the platforms where those calls are undefined.

**Break-it:**

| Removed or broken | RED |
|---|---|
| No flush per write | 2 |
| RF64 never chosen | 7 |
| PCM16 not clamped | 2 |
| Clamp after rounding (the old code) | 3 |
| NaN / infinity guard removed | **0** (redundant on this runtime, see above) |
| Checkpoint writes nothing | 2 |
| Drops not counted | 1 |
| Pad byte after odd-length JSON missing | 41 |
| Rate not validated | 10 |

**WIRING HAND-OFF.** `core/recorder.*` gains a `RecordKind::ChannelIq` beside `BasebandIq`; the DSP thread pushes the channel samples after the channel filter
into `AsyncIfRecorder::push` (where the channel filter output is available per receiver: `core/pipeline.cpp`, not read in detail). Ops: `SET_RECORD_IF`
(on / off, sample format), meta from the receiver's state at start (frequency, mode, bandwidth). Settings: `recordIfFormat` (0 float32, 1 PCM16). Replay: the
reader already gives rate and total; the centre, mode and bandwidth come from `readIfHeader()` (and the browser). Strings: "Record the channel (IF)", "32-bit
float / 16-bit", "Channel recording".

## Part 7: operating aids (`core/operating_aids.hpp`)

**Built.** (1) `TuneLock`: frequency lock with a per-source policy (wheel, click, drag, step keys, knob and typed entry blocked by default; bookmark, band button,
scanner, remote and program calls pass), a tuning-step lock, step up / down through a list, invalid requests refused whether locked or not, JSON. (2) `BandStacking`:
a band plan, three registers per band (frequency, mode, filter), `track()` keeps the active register current, `pressBand()` recalls the active register when coming
from another band and cycles 0, 1, 2 when already in the band, per-band reset, JSON matched by band name. (3) `VfoPair`: A and B, selected letter, split (receive A,
transmit B), swap, copy A to B and B to A, split-up by an offset, JSON. (4) `ModeMemory`: bandwidth, step and AGC remembered per mode (`changeMode(from, to,
current)`), built-in defaults, 64 modes at most, JSON.

**Proof (`test_operating_aids`, 25497 checks).** 11 sources x lock on / off (6 blocked by default); band edges inclusive; the cycle, the leave-and-return rule, tracking
outside every band refused; **11 hostile band-stack documents leave the state unchanged**, a plan that lost a band loads the rest by name; **20000 random VFO operations
equal a plain reference model**; **20000 mutated mode-memory documents: 8397 accepted (every accepted one valid), 11603 refused.**

**Break-it:**

| Removed or broken | RED |
|---|---|
| Lock ignores the source (blocks everything) | 30 |
| Step lock ignored | 1 |
| No upper frequency bound | 3 |
| Band edges exclusive | 1 |
| No register cycle | 5 |
| `track()` stores nothing | 5 |
| Swap does nothing | 3 |
| Receive ignores split | 2 |
| Mode not remembered on leaving | 9 |
| Register outside its band accepted by the loader | 2 |
| AGC range unchecked | 4 |

**Defaults that are mine, not verified** (all for the owner to change): the lock policy; the example band plan (7 bands, edges from memory: the wiring lane should use
the World plan data instead); the per-mode defaults (NFM 12.5 kHz, WFM 180 kHz, AM 9 kHz, USB and LSB 2.7 kHz, CW 500 Hz, ...); AGC numbering is the engine's
(`0 Auto, 1 Fast, 2 Medium, 3 Slow, 4 Off`, `docs/multi-radio-records/agc-nb-wiring.md`, quoted from `config.hpp`).

**WIRING HAND-OFF.** The tune path in the engine (`core/pipeline.*`, `engine/engine_*`) asks `TuneLock::requestFrequency` before applying a tune and tags every call
with its `TuneSource`; the frequency counter on the screen shows a lock lamp. Ops: `SET_FREQ_LOCK`, `SET_STEP_LOCK`, `PRESS_BAND`, `STORE_BAND_REGISTER`, `SELECT_VFO`,
`SWAP_VFO`, `COPY_VFO`, `SET_SPLIT`, `SPLIT_UP`. Settings (one JSON blob each, saved through the `toJson()` of the class): `tuneLock`, `bandStacking`, `vfoPair`,
`modeMemory`, `modeMemoryEnabled`. Strings: "Lock frequency", "Lock step", "frequency locked", "tuning step locked", "not a valid frequency", "not a valid step", "VFO A",
"VFO B", "A <-> B", "A => B", "B => A", "Split", "Split up", "Band", "Register 1 / 2 / 3", "Remember settings per mode".

## Part 8: scanner additions (`core/scan_aids.hpp`)

**Built.** (1) `PriorityScanner`: up to 16 priority channels checked every N seconds (default 2 s) for a dwell each (200 ms); a signal holds the radio on the priority
channel until it has been quiet for the hang time (2000 ms); a check may or may not interrupt a hold on an ordinary signal; a stall causes one check, not a burst; a backward
clock counts as zero elapsed (the scanner's rule). (2) `LockoutList`: ranges (a frequency is a range of half a step each side), temporary (until cleared, until a time, until
exit) and permanent (saved), permanent entries merge, temporary entries never stretch another's lifetime, unlock at a frequency or by range, 1024 per kind. (3) `ScanLog`:
what stopped the scan (signal, priority, manual), when (UTC), how long, peak and time-weighted mean level, how it ended; a ring of 10000; per-frequency statistics; CSV
with RFC 4180 quoting and a leading apostrophe on fields that start with `= + - @`. (4) `ScanMute`: audio muted only while the scanner is stepping, up at once on a signal, a
150 ms tail after a stop, none from Idle. `core/scanner.hpp` is **not changed**.

**Proof (`test_scan_aids`, 2255 checks).** Priority timing worked out by hand (check at 2.000 s, 200 ms per channel, hold, hang restarts on new activity); lockout edges
inclusive, ttl expiring exactly at now + ttl, **0.001 ms per query with 2048 entries (1033 ns, one run), 20000 mutated lockout documents: 544 accepted, all valid**; a golden CSV
with a comma, a quote and a newline in a label and a formula-injection label; 10000 events to CSV in 13.5 ms; ring of 5 keeps the newest 5 in order and counts 7 dropped.
**Integration with the real `core::Scanner`:** a list scan over three channels with a chatty signal on one, a priority channel active from 10 to 11 s, a simulated radio and
squelch: 18 priority visits (17 returns), the priority signal found at 11000 ms (the first visit that overlapped it), audio muted on 1005 of 1089 stepping ticks and never on a
signal, 13 scan-log stops all on the right frequency.

**Break-it:**

| Removed or broken | RED |
|---|---|
| Priority dwell ignored | 11 |
| Priority hang ignored | 3 |
| Interrupt-a-hold rule ignored | 4 |
| Backward clock counted as elapsed | 1 |
| Lockout ttl off by one | 3 |
| Lockout low edge exclusive | 4 |
| Permanent entries not merged | 3 |
| CSV formula guard removed | 1 |
| Ring overwrites slot 0 | 2 |
| Mean not time-weighted | 2 |
| No mute tail | 3 |

**Defaults that are mine:** interval 2 s, dwell 200 ms, hang 2 s, interrupt on, mute tail 150 ms.

**WIRING HAND-OFF.** The scanner tick in `engine/engine.cpp` (where `core/scanner.hpp` is driven) calls `PriorityScanner::update(now, squelchOpen, scanner holding)` first and
retunes on `GoPriority` / `ReturnToScan` (remembering the scanner's own frequency, as the integration test does); skips a locked lattice point with `Scanner::skip()` when
`LockoutList::isLocked`; opens a log entry when the scanner leaves Scanning and closes it on its next transition; feeds `ScanMute::update(scanner.state(), now)` into the audio
mute. Ops: `SET_PRIORITY` (channels, interval, dwell, hang, interrupt), `LOCKOUT` (frequency, permanent or not, ttl), `UNLOCK`, `EXPORT_SCAN_LOG`, `SET_SCAN_MUTE`. Settings:
`priorityChannels`, `priorityIntervalS`, `lockoutsPermanent`, `scanMuteWhileScanning`, `scanMuteTailMs`. The log is kept in memory and written by a file worker on export (never
the scan tick). Strings: "Priority channels", "Check every (s)", "Lock out", "Lock out for now", "Unlock", "Scan log", "Mute while scanning", the reasons "signal / priority / manual" and
"signal gone / skipped / listen limit / stopped / superseded / locked".

## Part 9: command line and clock (`core/cli_options.hpp`, `core/status_clock.hpp`)

**Command line.** Options `--freq/-f`, `--mode/-m`, `--radio/-r`, `--start/-s`, `--iq-file/-i` (or one bare file name), `--iq-rate`, `--iq-center` (raw files only),
`--profile/-p`, `--help/-h`; `--opt value` and `--opt=value`, `-fVALUE`, `--` ends the options. **Strict:** every problem is reported, not just the first; unknown options (with a
"did you mean" when within two edits), duplicates, missing values, flags with values, a file and a radio together, rate and centre without a raw file, an extension that is not an
I/Q one, control characters, Windows-forbidden path characters (a colon only as `C:\`), 64 arguments and 4096 bytes at most. Frequencies and rates: digits, one optional point, a
unit (Hz, k, M, G, any case, `m` is MHz as in radio practice), 1 Hz to 10 THz, **parsed as the exact decimal typed** (`2.4M` is exactly 2400000: digit-by-digit floating arithmetic
gave a value not equal to 2400000 in the first version, found by a test). Mode names match the engine's canonical ones in any case. The help text and the parser read one option table.
Optional `fileExists` / `dirExists` callbacks add existence checks; `passThrough` lets the start-up's own options through (see the hand-off).

**Clock.** `format(unixMs, offsetMinutes, style)`: 24-hour or 12-hour, with or without seconds, date and zone name (`UTC`, `UTC+01:00`, `UTC+05:30`, `UTC+05:45`, `UTC-03:30`); offsets
beyond +-14:00 read as 0; `dual()` for the status bar; `msToNextSecond()` so a timer stays on the second; `systemUtcOffsetMinutes(unixMs)` asks the C library for the local time at
that instant (daylight saving included).

**Proof (`test_cli_clock`, 200266 checks).** 57 frequency rows (22 accepted with all units, 35 refused: signs, exponents, separators, spaces, zero, over 10 THz, NaN, hex, a non-ASCII
digit); every option in long, `=`, short and attached forms; every mode in any case; **every option of the help table appears in the help text and is accepted by the parser**;
**200000 random command lines with hostile bytes: 33891 accepted, every accepted one satisfies all option invariants (ranges, no file with a radio, a known extension, known mode),
166109 refused with at least one message, no crash.** Clock: UTC and nine offsets, midnight and year rollover, 12-hour edge rows, before the epoch, 5000 simulated timer ticks all land
on the second, **100000 consecutive seconds equal an independent hour / minute / second count**; the system offset with the C library's time zone forced by `TZ` (UTC0 gives 0,
EST5EDT gives -300 in January and -240 in July, JST-9 gives 540, IST-5:30 gives 330) and, for this PC, **0 and 60 minutes at 2026-11-11 and 2026-07-01, equal to PowerShell's
`[TimeZoneInfo]::Local` (GMT Standard Time)**.

**Break-it:**

| Removed or broken | RED |
|---|---|
| Minimum frequency (1 Hz) unchecked | 57 |
| Duplicates allowed | 7 |
| File and radio allowed together | 253 |
| `<` allowed in a path | 1 |
| Mode matching case-sensitive | 12 |
| `msToNextSecond` with truncating modulo | 1 |
| 12-hour midnight shows 00 | 2 |
| Out-of-range offset used | 2 |
| Zone minutes dropped | 4 |

**WIRING HAND-OFF.** `main.cpp` and `engine_main.cpp` call `bootProcess(kind, argc, argv)` (`engine/process_boot.cpp`), which already reads `--config PATH`, `--use-config`, `--frames` and
the helper modes (`--enumerate-json`, `--sdrplay-probe`, `--update-check`, ...) from argv. **The parser must run AFTER the helper modes and with `passThrough = {{"--config", true},
{"--use-config", false}, {"--frames", true}}`**, or those would be refused as unknown. The wiring lane passes the result to the engine as start-up ops (tune, mode, open radio or
file, start) once the engine is up; on Windows `argv` must be converted from UTF-16 to UTF-8 first. `--help` prints `helpText()` and exits. The clock lives in the status bar: the
screen's one-second timer calls `dual(now, systemUtcOffsetMinutes(now))` and sleeps `msToNextSecond(now)`. Settings: `statusClockStyle` (24 / 12 hour, seconds, date), `statusClockShowUtc`,
`statusClockShowLocal`. Strings: the `description` texts in `cli::optionTable()`, "UTC", "Local".

## A claim corrected during the work

I first wrote that ffmpeg "decodes with CRC and MD5 checking on". Checked by hand on a file with one STREAMINFO MD5 byte flipped: **ffmpeg does not report a wrong STREAMINFO MD5** (it
does report a flipped audio bit: `header crc mismatch`). The MD5 is therefore proved by three computations (the test's own MD5 and decoder, the Windows CNG MD5, and **ffmpeg's own
MD5 of its decoded PCM equal to the file's STREAMINFO MD5**), and the test and its comments say so.

A process slip: the break-it script ends each row with `git checkout -- <file>`, which restored an UNCOMMITTED source fix to the old code before it was committed; the first commit of the
clamp fix therefore carried only its tests. Found when the FLAC test failed again; fixed in commit `recording: the clamp-before-rounding fix ...`. The rule from now on in this lane: commit
before a break-it row.

## Tests run (this machine, Release, 3 jobs)

Build directory `D:/foxsdr-build/feat-rec`, configured with `-DCASCADE_BUILD_SLINT=OFF -DCASCADE_SOFTGL=OFF` (VS 2022 x64, the vcpkg toolchain, triplet `x64-windows`, manifest mode off),
`cmake --build ... --config Release --parallel 3 --target <the eight tests>` (one re-configure per new test file for the glob). Only the engine library and the eight test executables were built;
the window was not built, the application was not run, no radio was opened, nothing was installed or downloaded.

Own checks: flac 590, naming 494, browser 125, playback 214, IF 1207, aids 25497, scan 2255, cli and clock 200266; all 0 failed.

`ctest -C Release -j3 -R "^(test_flac_writer|test_rec_naming|test_rec_browser|test_iq_playback|test_if_recorder|test_operating_aids|test_scan_aids|test_cli_clock)$"`: **`100% tests passed out of 8`**
in both runs (33.20 s and 33.29 s; the longest tests are `test_if_recorder` at 33 s, `test_flac_writer` at 13 to 17 s and `test_rec_browser` / `test_iq_playback` at about 5 s). The existing
`test_mp3_writer` was also built and run once: 118 checks, 0 failed.

No timing-sensitive test failed in any run. `test_iq_playback` (a stress test with two threads) and `test_if_recorder` / `test_flac_writer` (worker threads) were each run at least five times
during development (including every break-it row) and in both final runs; they were not run 50 times in a loop, and ThreadSanitizer was not run (MSVC build).

## UNVERIFIED list

- RFC 9639: the last 79404 of its 179404 characters were not read (the fetch answered from the first 100000), and the answers were a summary with short quotes; the writer was checked against two
  decoders instead. Subset-conformance flags, seeking by a player, a FLAC file from a real recording and a full disk were not tried.
- FLAC decoding **inside the application** does not exist (no decoder in `src/`).
- MP3: the patent dates come from a Wikipedia page (read through a fetch tool), not from the patent documents or the licensors; the price and scope of ISO/IEC 11172-3 are not verified; whether
  the Media Foundation encoder offers VBR is not stated on the page read.
- Name patterns of SDR#, HDSDR, SDRuno and SDRconnect files are from memory; the stereo-WAV kind guess is a heuristic.
- Playback: speed is varispeed (pitch follows), real-time pacing and an audio device were not tried; the previous batch's `IqFileReader` may serve stale buffered data from a file that
  shrinks (not tested, not changed).
- IF recording: the over-4 GB path is proved by a header rewrite and a sparse file, not by recording 4 GB; no other program (Audacity, GNU Radio, SDR#) was tried on these files; SigMF was not
  chosen as the container (an owner decision below).
- The defaults of Parts 7 and 8 (lock policy, example band plan, mode defaults, priority interval, dwell, hang, mute tail) are mine; no program was read for them.
- The scanner integration is against a simulated radio and squelch only.
- The system clock offset is verified for this PC's zone and four `TZ` strings in the Windows C runtime; other zones and other operating systems were not tried. Linux and macOS: none of the
  nine parts was built there (the code is portable C++ with Windows-only parts guarded: sparse files, CNG, `_wfopen`, `localtime_s`).
- Thread safety by test only (no ThreadSanitizer).

## Owner decisions needed

1. **FLAC in the main recorder:** offer it as a format (16 / 24 bit)? Default format?
2. **MP3 in the main recorder:** wire the existing Windows-only writer? (Nothing new to build.)
3. **FLAC playback in the application:** build a decoder with LPC (a few hundred lines), or leave playback of FLAC to other programs?
4. **IF recording container:** the WAV with the `fxif` chunk (reads in every reader here and in most audio tools) or a SigMF pair (other SDR programs read the metadata, two files per recording).
5. **File names:** default template, UTC or local time by default.
6. **Defaults of Parts 7 and 8:** the frequency-lock policy (should bookmarks and typed entry be blocked?), the per-mode defaults, the band plan's source, priority interval / dwell / hang, the mute tail.
7. **Command line:** strict (unknown options are errors, as built) or tolerant for forward compatibility?
8. **Where the clock goes** in the status bar and whether both UTC and local are shown.
