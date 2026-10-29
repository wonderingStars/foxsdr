// tests/thread_inventory_rows.inc - the table tests/test_thread_inventory.cpp holds src/ to. Included once, inside its anonymous
// namespace. One row per file under src/ that creates a std::thread or calls std::async, or registers a heartbeat:
//
//   {file, threads, asyncs, regs, covered, "specs", "why"}
//
//   threads  `std::thread` constructions in the file     asyncs  `std::async` calls in the file
//   regs     `beats::Registration` constructions         covered how many of the spawns those registrations cover (a registration
//                                                          inside a thread's entry function covers every spawn of that function)
//   specs    the ThreadSpec identifiers the file registers, comma separated
//   why      for the spawns that are NOT covered: the reason, a sentence. Required when threads + asyncs > covered; refused when not.
//
// AN HONEST COLUMN. "Not covered" here is a statement of what this batch did and did not reach, not a claim that nothing can
// wedge there: the reasons that say NOT COVERED name a known gap (docs/engine-stage4.md 6.27 lists them), and the ones that do
// not are the ones where a heartbeat would be wrong (the monitor itself, a reap that only destroys a future, a thread that waits
// for a person).

#define R_REAP \
    "a detached helper that only waits on an abandoned job's future so that it can be destroyed; it runs no work of its own and has no loop to beat in"
#define R_NET \
    "a one-shot network exchange bounded by its own client timeouts and a cancel flag; short-lived, nothing to beat in - NOT COVERED by this batch (a known gap)"
#define R_DRIVER \
    "a native driver's own producer or helper thread: its consumer, the pipeline's source thread, is registered and reads it with a timeout, so a producer that stops starves a read() that returns 0 and the source thread keeps beating - a silent radio is the no-data lamp's finding, not this watchdog's - NOT COVERED (a stated limit)"

const Row kRows[] = {
    {"src/core/audio_open.hpp", 1, 1, 0, 0, "",
     "AudioOpen's open worker (a PortAudio / WASAPI call the window bounds itself, kOpenBound, under a WatchdogPause) and the reap that abandons it - NOT COVERED by this batch (the window's own bound is its watch)"},
    {"src/core/config_writer.hpp", 1, 1, 1, 1, "kThreadConfigWrite", R_REAP},
    {"src/core/crash_handler.cpp", 1, 0, 0, 0, "",
     "the crash handler's self-test thread, which throws on purpose; never started in a product run"},
    {"src/core/crash_upload.cpp", 1, 0, 0, 0, "", R_NET},
    {"src/core/diag_history.cpp", 1, 0, 0, 0, "",
     "a one-shot scan of the report folders at start whose result is polled, nothing waits on it - NOT COVERED by this batch (a known gap)"},
    {"src/core/diag_log.cpp", 1, 0, 0, 0, "",
     "the diagnostics log's reader of a redirected stderr pipe: its blocking read IS its job and it ends at end of file"},
    {"src/core/disk_job.hpp", 1, 1, 1, 1, "kThreadDiskJob", R_REAP},
    {"src/core/feature_request.cpp", 1, 0, 0, 0, "", R_NET},
    {"src/core/foreign_modules.cpp", 1, 0, 0, 0, "",
     "the module census's one-shot walk of the loaded modules (Windows), detached and short-lived - NOT COVERED by this batch (a known gap)"},
    {"src/core/gps_reader.cpp", 1, 0, 1, 1, "kThreadGps", ""},
    {"src/core/hang_watchdog.cpp", 1, 0, 0, 0, "",
     "the monitor itself: a thread that watches the others cannot beat for itself; its poll wait is interruptible and its stop() is joined (tests/test_diag_hang.cpp)"},
    {"src/core/health_events.cpp", 2, 0, 0, 0, "",
     "the health ledger's writer loop and the removal of a disarmed ledger's file, both detached disk helpers that end by themselves - NOT COVERED by this batch (the ledger writes the profile folder; a known gap)"},
    {"src/core/os_crash_record.cpp", 1, 0, 0, 0, "",
     "a one-shot lookup of the operating system's event log with its own retry interval, bounded by the caller's wait - NOT COVERED by this batch (a known gap)"},
    {"src/core/patch_audio.cpp", 3, 0, 1, 1, "kThreadDiskJob",
     "the other two spawns: the orphaned file's close (a one-line detached helper) and the MP3 destination's writer loop (Mp3Dest::work) - NOT COVERED by this batch (a known gap)"},
    {"src/core/patch_radio.cpp", 1, 0, 1, 1, "kThreadPatchSource", ""},
    {"src/core/pipeline.cpp", 4, 0, 2, 4, "kThreadDsp,kThreadSource", ""},
    {"src/core/radio_setup.cpp", 1, 0, 0, 0, "",
     "the radio-setup inventory's one-shot SetupAPI probe, detached and polled, bounded by the window's own timeout state - NOT COVERED by this batch (a known gap)"},
    {"src/core/record_finish.cpp", 1, 0, 1, 1, "kThreadDiskJob", ""},
    {"src/core/sentinel.cpp", 1, 0, 0, 0, "",
     "the sentinel's exit deadline: a thread that sleeps and then ends the process, the last resort and not a worker"},
    {"src/core/telemetry.cpp", 4, 0, 0, 0, "",
     "the telemetry senders (usage record, health, the still-running beat) and the removal of a disarmed ledger's file: one-shot exchanges bounded by their own timeouts - NOT COVERED by this batch (a known gap)"},
    {"src/core/tester_link_net.cpp", 2, 0, 0, 0, "", R_NET},
    {"src/core/tester_usage_session.cpp", 1, 0, 0, 0, "", R_NET},
    {"src/core/transmitter.cpp", 1, 0, 1, 1, "kThreadTransmit", ""},
    {"src/core/updater.hpp", 1, 1, 0, 0, "",
     "UpdateCheckTask's check / download worker (WinHTTP, its own timeouts) and its reap - NOT COVERED by this batch (a known gap)"},
    {"src/engine/engine.cpp", 4, 2, 2, 2, "kThreadDeviceOpen,kThreadDeviceScan",
     "the other four spawns: the control thread (its own HangWatchdog, started with it, watches the pass), the hold-the-audio-lock thread of bounded runs only, and the two reaps of an abandoned open / scan"},
    {"src/engine/engine_disk_work.cpp", 1, 0, 0, 0, "",
     "letGo: a detached close of a replaced I/Q file source (a file-system call kept off the pass); one-shot, no loop - NOT COVERED by this batch (a known gap)"},
    {"src/engine/engine_patch_radios.cpp", 1, 2, 2, 2, "kThreadPatchOpen", R_REAP},
    {"src/engine/engine_service.cpp", 1, 0, 1, 1, "kThreadKeeper", ""},
    {"src/engine/engine_soundcard.cpp", 2, 2, 2, 2, "kThreadSoundCardOpen,kThreadSoundCardScan", R_REAP},
    {"src/engine/engine_store.cpp", 0, 6, 0, 0, "",
     "the plugin store's jobs (catalogue read, a picture, an install, a clean-up): downloads over WinHTTP with their own timeouts, collected by pumpWorkers; an install is minutes by design - NOT COVERED by this batch (a known gap)"},
    {"src/engine/image_feed.cpp", 1, 0, 1, 1, "kThreadImageFeed", ""},
    {"src/engine/record_start.hpp", 1, 1, 1, 1, "kThreadRecordOpen", R_REAP},
    {"src/engine/server_host.cpp", 1, 1, 2, 2, "kThreadPasswordHash,kThreadServerHost", ""},
    {"src/engine/shutdown_guard.cpp", 1, 0, 0, 0, "",
     "the headless stop's budget guard: the watcher of the stop, which ends the process when the budget is spent; a watcher is not a worker"},
    {"src/gui/app_window.cpp", 0, 1, 0, 0, "",
     "the update download (WinHTTP, its own timeouts) - NOT COVERED by this batch (a known gap)"},
    {"src/gui/app_window_radio_setup.cpp", 1, 0, 1, 1, "kThreadDriverInstall", ""},
    {"src/gui/fonts.cpp", 1, 0, 0, 0, "",
     "the font scanner: a one-shot walk of the system's font files at start, with a cancel flag - NOT COVERED by this batch (a known gap)"},
    {"src/gui/link_request_poll.hpp", 1, 1, 1, 1, "kThreadLinkPoll", R_REAP},
    {"src/gui/shell_open.cpp", 1, 0, 0, 0, "",
     "the responsive shell call: a worker the GUI thread waits on while taking messages, under the user-paced pause; waiting for a person is its job"},
    {"src/net/cat_server.cpp", 2, 0, 2, 2, "kThreadCatAccept,kThreadCatClient", ""},
    {"src/net/web_server.cpp", 2, 0, 2, 2, "kThreadWebIdle,kThreadWebListen", ""},
    {"src/source/airspy_source.cpp", 1, 0, 0, 0, "", R_DRIVER},
    {"src/source/airspyhf_source.cpp", 1, 0, 0, 0, "", R_DRIVER},
    {"src/source/aor_source.cpp", 1, 0, 0, 0, "", R_DRIVER},
    {"src/source/hackrf_source.cpp", 1, 0, 0, 0, "", R_DRIVER},
    {"src/source/mirisdr_source.cpp", 1, 0, 0, 0, "", R_DRIVER},
    {"src/source/pluto_source.cpp", 1, 0, 0, 0, "", R_DRIVER},
    {"src/source/pluto_tx.cpp", 1, 0, 0, 0, "",
     "the Pluto transmit sink's writer thread: the transmitter's own thread (registered) hands it blocks and bounds its join (kWriterJoinWait) - NOT COVERED by this batch (a known gap)"},
    {"src/source/rtl_tcp_source.cpp", 1, 0, 0, 0, "", R_DRIVER},
    {"src/source/rtlsdr_source.cpp", 1, 0, 0, 0, "", R_DRIVER},
    {"src/source/rx888_source.cpp", 1, 0, 0, 0, "", R_DRIVER},
    {"src/source/sdrplay_probe.cpp", 1, 0, 0, 0, "",
     "the SDRplay scan probe: a one-shot call into the vendor service with its own abandon bound (Recovered::SdrEnum) - NOT COVERED by this batch (a known gap)"},
    {"src/source/sdrplay_service.cpp", 1, 0, 0, 0, "",
     "the SDRplay service enumeration worker: abandoned past its bound by design (Recovered::SdrEnum) - NOT COVERED by this batch (a known gap)"},
    {"src/source/sdrplay_source.cpp", 4, 0, 0, 0, "", R_DRIVER},
    {"src/source/soapy_enum_proc.cpp", 3, 0, 0, 0, "",
     "the SoapySDR enumeration CHILD process's pipe and wait threads: the child is contained by a process boundary and counted (Recovered::EnumChild)"},
    {"src/source/soapy_source.cpp", 2, 1, 0, 0, "",
     "SoapySDR's abandonable vendor calls and their reaps: each is bounded by kVendorCallWait / kControlLockWait and counted (Recovered::VendorCall)"},
    {"src/source/soundcard_source.cpp", 1, 0, 0, 0, "", R_DRIVER},
    {"src/usb/bounded_walk.hpp", 1, 0, 0, 0, "",
     "the bounded USB walk's worker: abandoned past its own bound by design, the walk being a call that may never return - NOT COVERED by this batch (a known gap)"},
};
