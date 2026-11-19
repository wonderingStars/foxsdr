# Field fixes A (2026-11-19) - the 42958e install's journal

Branch fix/field-a from 03ac57b7. Local commits only. Final state: all three faults done; "what is not proved" is at the end.

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

## WHAT A USER SEES (word for word; the title is the sheet's heading, the keys in brackets)
* The radio is held by another FoxSDR window (answered on the pipe): [Radio in use by another FoxSDR window] "NESDR SMArt v5 (serial ending 0001) is in use by another FoxSDR window on this PC. Move it here? The other window will stop using it and show a notice." [Move it here] [Leave it there].
* Move it here, the other window lets go: the radio opens here (the ordinary open); the other window shows [Radio moved] "NESDR SMArt v5 (serial ending 0001) was moved to another FoxSDR window. This window is on the signal generator now." [OK] (and the same sentence under its source control).
* The other window is recording: [Radio not moved] "NESDR SMArt v5 (serial ending 0001) is recording in the other FoxSDR window. Stop the recording there first, then try again." [OK]. Nothing changes.
* The other window does not answer in 5 s: [Radio not moved] "The other FoxSDR window did not answer in time, so NESDR SMArt v5 (serial ending 0001) was not moved. Nothing was changed: it is still in use there." [OK].
* Another program holds it (or the radio has no serial, so no holder can be named): [Radio in use] "NESDR SMArt v5 (serial ending 0001) is in use by another program. Close that program, then pick the radio again." [OK]; no move key; the 2, 4, 8, 16 s retries go on as before (the updater's old copy letting go is what they are for).
* A radio that stops delivering samples (fault 1): unchanged sentence "Device stopped: the radio stopped delivering samples (it may have been unplugged) [<Windows text>]. Reconnect it and pick the source again, or switch to the signal generator." - but four failed reads in a row are now needed first.
All strings go through cascade::i18n::tr; the sheet is the existing Handover sheet. Serial: only its last four characters are shown (the pipe carries the whole key, locally, to this user's processes only).

## WHAT IS NOT PROVED / NOT DONE
* NOT built for the Legacy window (cascade.exe): the holder side is not started there (it would need `startHandoffServer("legacy")` in src/main.cpp and an on-screen notice there), and Legacy as the asker has no popup. A Legacy holder is not found, so the new window says "another program" for it, which is the old, honest answer.
* NOT run: a real two-window session on the hidden desktop with the real dongle (the foxsdr-slint target builds; the host wiring is exercised only through RadioMoveHost's test and the engine scenarios).
* A radio with no serial in its arguments ("index=0") cannot be matched to a holder: the "another program" sentence.
* The pipe server's first version served one client at a time and, rarely (about 1 run in 8 of the refusal scenario), a second asker found it between instances and got no answer; fixed by making the next instance before serving the current one. 10 runs of the engine test passed after.
* Proof that the first copy's death at 04:51:14 was or was not caused by the extra copies: not obtained.
* The Legacy window and the web/CAT/TCI profiles do not carry the op.
