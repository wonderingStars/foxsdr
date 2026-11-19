# Field fixes A (2026-11-19) - the 42958e install's journal

Branch fix/field-a from 03ac57b7. Local commits only. STATE AT THE CHECKPOINT (weekly-usage stop) is at the end of this file.

## Evidence (radio journal 2026-11, install 42958e, two NESDR SMArt v5: serials SDR_CONTROL and SDR_VOICE)
Event times in the journal are flush times; the real time of an event is `at` minus `age`. Real timeline: 03:53 first copy restores SDR_CONTROL; 04:02:43 its read stops
("reading samples: the radio stopped delivering samples (it may have been unplugged)"); 04:38 the user picks SDR_VOICE; 04:51:05-04:51:27 three more copies start against SDR_VOICE, each "busy"
with the five-try retry series and one report per try (14 radio_fail in about 90 s); 04:51:14 the first copy's SDR_VOICE closes "source thread: this radio has faulted; reopen it" (the fault's own words
overwritten, see fault 1); the install then drops the radio four more times (05:09, 05:17, 05:24, 08:51), each followed by the user picking it again.

## Fault 1 - the RTL-SDR source (commit 6f568092)
Cause: src/source/rtlsdr_source.cpp readerLoop turned the FIRST negative bulk read into a permanent fault (no retry, no recovery; the pipeline does not reopen a native radio). usb/winusb_device.cpp
(beginBulkStream) records that one halted-endpoint read fails with Windows error 31 and the next delivers data. Also: noteFault threw the transport's own text (the Windows error number) away, and RtlSdrSource::setError
let every later refused control call replace the fault's words with "this radio has faulted; reopen it".
Change: four failed reads in a row (10 ms apart) before the fault; the fault text carries the transport's text; setError keeps a latched fault's text until clearError.
Proof: tests/test_rtlsdr_source.cpp block 23 (RED 7 checks, GREEN; 278/0), direct/ifbw/rate_report/usb_winusb green.
NOT PROVED: that the user's dongle was failing single reads (the journal has no Windows error; a USB power drop or real unplug fails every read and is faulted as before). The next report says which.
NOT PROVED: that the new copies' failed opens did NOT disturb the first copy's stream at 04:51:14 (the open is exclusive, share mode 0, so a failed second CreateFile cannot touch the first handle by design; not reproduced on hardware here).
What the user sees when a radio faults: "Device stopped: <reason>. Reconnect it and pick the source again, or switch to the signal generator." (src/slint/signal_logic.cpp), so the window does not look dead; nothing tells them a SECOND copy is the wrong move.

## Fault 3 - one report per retry (commit a5d2e7cf)
Cause: Engine::finishDeviceOpen filed a radio_fail for every busy-again retry (comment: "every time, a busy-again retry included"); contract section 2 said the same. The series itself is bounded (2+4+8+16 s, attempts 1..5, then "giving up"; a hot-plug arrival resets it).
Change: a busy-again try inside the series is counted in `attempt` (FailureInput::countOnly) and not sent; the restore's failure, the series' last failure and any other-reason failure are sent. Contract section 2 PROSE amended; no field, no value, no Worker/site change.
Proof: tests/test_radio_reports_engine.cpp (RED 4 checks; GREEN 140/0 x3), saved_radio_retry 39/0, _app 74/0, radio_reporter 228/0, fields 48/0, never_sent 9/0, send_on 105/0, radio_report 215/0.

## Fault 2 - a second copy finds the radio taken (owner's words: "a message pop up when the radio is taken by another copy and an option to disconnect that running ver and move the radio over to this ver")
Design as found: FoxSDR has always allowed several windows (core/tester_link.hpp "multi-instance design"; core/config_lock.hpp "a second window has always been allowed to start"); the Legacy window did the same. So no single-instance block.
Built: core/radio_handoff.{hpp,cpp} (a local named pipe per window, current-user-only access list, PIPE_REJECT_REMOTE_CLIENTS, no socket; HOLDS/RELEASE one-line protocol; the pid comes from the OS), engine/engine_handoff.cpp
(asker: one search per failed open, never per retry, cancels the retry loop when a FoxSDR holder is found; holder: releases through the control pass, REFUSES while an I/Q or audio take is running or queued - the app ends a take only for a source the user chose
at that window, and a stranger's request cannot be asked in the time - else puts the radio on the generator and leaves a notice), FOXAPP_OP_RADIO_HANDOFF 0x8626 (in no web/CAT/TCI profile), facts radio.handoff / radio.handoffMoved,
src/slint/radio_move_host.hpp + main.cpp wiring over the existing Handover sheet.
Tests so far: tests/test_radio_handoff.cpp (26/0: keys, holder detection over real pipes, release, refusal, no-longer-held, the 300 ms timeout, own pid never "another copy"); tests/test_radio_reports_engine.cpp holds the engine
scenarios (a child process plays the other window): move (one question, one report, no retry loop, radio opens, no second report), other program (plain sentence, retries continue), refusal, no answer in time, and the holder side (refused while a take is queued, then released with the notice).
Note: one run of the refusal/timeout scenario failed twice before a rebuild and passed after (177/0); rerun several times before trusting it.

## WHERE I STOPPED (checkpoint) AND WHAT IS LEFT
Done and green: faults 1 and 3; core pipe module; engine side and its scenarios.
NOT yet done: (1) src/slint/main.cpp (RadioMoveHost wiring, startHandoffServer before restore) is written but NOT COMPILED; the foxsdr-slint target has not been built. (2) tests for RadioMoveHost (shown once per sequence, keys send 1/0) and for core::handoff::sheetFor words.
(3) tests/test_apply_command.cpp needs a `covering(FOXAPP_OP_RADIO_HANDOFF)` case (its coverage check fails without it) and test_engine_api_05a's profile test needs a look. (4) the docs entry for the op (docs/api-0.3-additions.md style). (5) full runs of the touched test targets
and the headless label. (6) Legacy window: the holder side is NOT started in cascade.exe (it would need the same one call in src/main.cpp and an on-screen notice); as asker it has no popup. Say so in the report; the popup says "another FoxSDR window" only when the holder answered the pipe, which only a window that started the server can do.
(7) real two-window run on the hidden desktop with the real dongle: NOT done.
