# Field fixes E (branch fix/field-e, from 03ac57b7, local only)

Source: D:/foxsdr-build/prep/FIELD-CRASHES-2026-11-19.md. Build folder D:/foxsdr-build/field-e.

## STATUS: DONE (all three faults committed; final)

- Fault 1 committed 6a6f2e01. 34/34 pipeline, rate, source-swap and error-path targets passed.
- Fault 3 committed b4be570b. 16 audio, pipeline-audio and error-path targets passed.
- Fault 2 committed 40bd3599 (code and sentinel test) plus the unit test of the tail reader (tests/test_vendor_lines.cpp) in the following commit. Diag/sentinel suites: 18 headless targets passed (test_sentinel, _proc 278/0, _beat_source, engine_main_sentinel, diag_*, vendor_lines, process_boot_order, upload_scrub, error_paths) and 5 gui targets on the hidden desktop passed (gui_desktop_guard, crash_upload, diag_hang, diagnostics, sentinel_app).
- Not proved: a real Slint panic in the field; Diagnostics off (pipe path, no tail); diagnostics switched on mid-session (no file, so no tail); a real device re-open starving one callback; real RSPduo rate change. Slint's trigger itself is still unknown; this only captures the next message.

## Fault 1: cause and change
setInputRateHz left the ring alone; only start()/setSource() sized it. After 2 -> 6 MS/s the ring stayed 524288. Now a live change on a running pipeline gives the depth ringCapacityForActiveSourceLocked() gives a start at that rate: quiesce the source thread (the stop()/setSource bounded quiesce), start the source, resize, respawn (pipeline.cpp, end of setInputRateHz). A thread the driver will not release becomes a pipeline fault, never a second thread in the driver. Test: tests/test_pipeline_ring_rate.cpp [R6] (red: 3 checks failed; green: 49/0).
Not proved: real RSPduo; reverse direction not run red separately; a rate call equal to the chain's current rate still returns early without resizing.

## Fault 3: cause and change
Each sink open starves a callback (diagnosis reading, not measured on a device). AudioOut::opens() counts successful opens; closeAudioMinute discounts one starved callback per open together with stops. Mark taken at end of Engine::initialise. Test: test_audio_lead_app section 9e (red: 5 failed with the term removed).

## Fault 2: cause and change
Slint's Rust panic prints to stderr then fast-fails; stderr was a pipe read by a thread in the dying process. Now, with Diagnostics on, fd 2 / STD_ERROR_HANDLE is an append-only write-through file `stderr-<pid>.txt` in the log folder (replaced per run, files older than 72 h of other pids deleted, emptied by the reader thread when read to the end past 256 KiB); the same thread feeds the vendor: log lines as before (20 ms poll). With Diagnostics off or no folder the pipe is used as before. The sentinel (finishSentinelWatch) appends readStderrTailForReport(): last 40 lines, each wrapped at 170 chars, printable ASCII only, every line through scrubVendorLine then scrubUploadLine, prefixed `stderr: `, at the END of the report's log section (older ordinary lines make room, total stays at most 256 lines, the report reader's cap; the site keeps the last 100 lines of at most 300 characters each, so the tail survives; max about 40 x 178 bytes = 7 KB).
Size limits checked: site crash.go crashMaxBody 64 KiB, crashMaxLogLines 100, crashMaxLogLine 300. The client ceiling kMaxUploadBytes is 128 KiB, above the site's 64 KiB (pre-existing mismatch, not changed). telemetry-worker/worker.js (MAX_BODY 4096) is the usage-counter endpoint, not the report path; unchanged.
Test: test_sentinel_proc scenario failfast-stderr (child prints a recognisable line and a line with a user path and an address, then __fastfail). Note: with the old pipe the line sometimes still reached the log, so the red failure is "no stderr: tail", and the pipe path is timing luck.

### What the tail can contain (privacy)
Whatever the process or any library in it wrote to stderr in its last lines before it died, after scrubbing: account names in paths, network addresses/host names, serial numbers, quoted names in single quotes and numbers on frequency lines are masked; non-ASCII becomes `?`. It can still contain: other file names and folder names outside \Users\, library source paths, panic messages and plugin or library names, and any free text a library prints. The unscrubbed raw file stays on the PC in the log folder (up to 256 KiB, until replaced or 72 h) - it is not uploaded.
Candidate privacy sentence (NOT edited into PRIVACY.md): "If FoxSDR ends in a way it cannot report itself, the crash report also carries the last lines (at most 40) that FoxSDR or a library inside it printed to its error output, with account names, network addresses, serial numbers and quoted names masked; this is typically a library's own error message and can name files or components, but never signal content."
