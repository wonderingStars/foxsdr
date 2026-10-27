# Which tests run under ThreadSanitizer (-DCASCADE_SANITIZE=thread), and why the
# others do not. Included from tests/CMakeLists.txt in a thread build only.
#
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#
# ENGINE/STAGE4 BRANCH, MERGED WITH MASTER 0.99.84 (docs/engine-merge-0.99.84.md): the lists below are master's, plus the
# branch's own: the engine API's tests, the control thread's, the plugin UI's and the pump move's (the block marked in the IN
# list), the window tests registered a second time against the free-running pass (<name>_free), and four tests that run the
# application as a child. Before the merge the branch carried a smaller list and excluded 46 of master's thread-bearing tests
# as "not run yet" because it lacked master's fixes; those fixes came with the merge and the exclusion is gone.
#
# THE QUESTION THIS ANSWERS is "which tests put two of our threads against each
# other", because that is all a thread sanitizer can find: a data race, a lock-order
# inversion, a mutex misused. A test with one thread in it, and no thread inside the
# code it drives, cannot fail under TSan and costs a build, a link and several times
# its run time for nothing. So the job runs a SELECTION (the label `tsan`) and builds
# only that (the target cascade_tsan_tests); every other test is registered all the
# same, disabled and labelled `sanitize-excluded`, so ctest lists it and the reason
# is here, as under AddressSanitizer.
#
# A test is IN if it starts threads of its own against product code, or drives code
# that has threads (the pipeline's source and DSP threads, a patch radio's reader,
# the plugin runner, the config/bookmark/marker savers and the disk jobs, the
# watchdog, the health ledger's writer, the web and CAT servers, the telemetry
# sender, the audio sink's callback thread). It is judged by reading what the
# test starts, not by its name; the thread-bearing code of 0.99.64 is reviewed in
# docs/SANITIZERS-AND-FUZZING.md.
#
# A test with threads that is OUT is out for one of four reasons, written beside
# its name below: it starts the real application as a child, it faults a process
# on purpose, it measures against the wall clock, or it drives the whole window
# under a software GL context.
#
# THIS LIST WAS WRITTEN WITHOUT A LINUX RUN. The first log is what shows which of
# the "in" tests are too noisy or too slow, and which "out" tests are worth adding.

set(CASCADE_TSAN_TESTS
    # the pipeline, its source and DSP threads, and what hangs off them
    test_pipeline test_pipeline_getters test_pipeline_ring_rate test_pipeline_file_fault
    test_thread_fault test_source_swap test_source_overreport test_spsc_ring test_scope_tap
    test_audio_clip test_rate_follow test_scope_taps_wired
    # the two threads ask for priority at their start and put it back at their end (0.99.73)
    test_dsp_priority
    # the generator the pipeline's source thread reads while a caller retunes it
    test_siggen
    # patch radios: one reader thread each, and a speaker written from its own
    test_patch_radio test_patch_radio_abandon test_patch_dest_async test_patch_runner
    test_patch_decoders
    test_converter_app_paths test_converter_routing
    # the plugin runner and the host API plugins call from their own threads
    test_plugin_runner test_plugin_destroy_reentry test_plugin_api test_plugin_audio
    test_plugin_poll_bound test_plugin_ui test_plugin_abi3_compat test_plugin_repo
    # sources with threads of their own, and the vendor-call guard
    test_sdrplay_stall test_sdrplay_source test_sdrplay_service test_soapy_read_bound
    test_soapy_vendor_guard test_vendor_guard test_iq_file_source test_pluto_source
    test_pluto_tx test_aor_source test_one_chip_one_route test_ppm_app test_autogain_app
    # work moved off the window's thread (0.99.64 and before): the savers, the disk
    # jobs, the opens, the polls
    test_config_save test_bookmark_save_async test_gui_disk_audit test_gui_file_jobs
    test_iq_open_async test_link_request_poll test_record_start test_audio_open
    test_tester_link_app test_patch_presets_app test_soundcard_app_paths
    # the engine's once-a-minute audio close, counting into the health ledger (0.99.73),
    # and its pipeline-ring sibling read across the source thread (0.99.86)
    test_audio_lead_app test_dsp_drop_minute_app
    # the health ledger's writer, the frame timer's cross-thread reads, the watchdog
    test_health_events test_health_paths test_frame_timing test_hang_signature
    test_excuse_cap test_watchdog_startup test_telemetry
    # servers and senders
    test_web_server test_cat_server test_httplib_vendor test_updater test_feature_request
    test_gps_reader test_serial_port test_transmitter
    # the sound sink: PortAudio's callback thread against the writer
    test_audio_out test_audio_in test_audio_open_log
    # the engine API held to the reference checklist, and the API's own groups, driven
    # through the C table against a real headless Engine on its own control thread
    test_engine_api_conformance test_engine_api_03 test_engine_api_merge
    test_engine_api_transmit test_engine_api_hermetic
    # stage 5a: read_audio's feed against its readers, the internal sessions' profiles and SERVER_CONFIG's worker against the pass
    test_engine_api_05a
    # stub batch B3: the store's workers (a catalogue read, a picture, a clean-up) against the pass that collects them
    test_engine_api_store
    # stub batch B6: the Airband monitor's pass against the DSP thread whose squelch reports and block count it reads, and the live list's block
    test_engine_api_airband
    # stub batch B9: the settings edit (a command on the pass) against the plugin API's store a plugin's own thread writes
    test_engine_api_settings
    # stub batch B10: the marker ops (commands on the pass) against the worker that saves markers.json and the API threads that read the list
    test_engine_api_markers
    # stub batch B12: automatic squelch (the rule on the pass) against the API threads that read its facts and the worker that delivers the samples it measures
    test_engine_api_squelch
    # the control thread against the front end's thread: the command queue (submit from
    # any thread, drain on one), the pumping engine, what the host answers from a thread
    # that is not the window's, the shutdown order
    test_command_queue_threads test_engine_control_thread test_engine_host_threads
    test_engine_shutdown
    # the one receiver snapshot: a seqlock and a lock-free hand-over slot, readers
    # against a publishing writer
    test_receiver_snapshot test_snapshot_stress
    # every op the engine applies, on the real application
    test_apply_command
    # the transmit key across the front end, the control thread and the TX thread
    test_transmit_key_race test_transmit_liveness test_transmit_dead_man
    # the pump move's step P1 (the review's F3), re-pointed in stage 5d from the engine's pull to the web server's engine session: the engine applies the web remote's and CAT's requests in its own pass, and the key
    # rules are touched by the control thread alone - producers on the web session, two local sessions and a server whose session closes and opens again against the threaded
    # pass, the ordering, the 2 s hold, the latch's marking and the re-arm (test_transmit_key_race's case G is the same storm
    # against the hammer); and the same standing conditions through the window and a real web server with a real HTTP client (stage 5d: the PTT, the status poll, the stop)
    test_remote_control_pull test_remote_key_release
    # stage 5a: the same guarantee for the engine-internal sessions - the tier applied after the local and API queues whatever arrived
    # first, the same-pass release, the flood, the liveness and the storm with producers on the web and CAT sessions
    test_internal_session_order
    # stage 5b: the engine OWNS the servers - the real web server and CAT listener over loopback sockets, started, restarted and stopped by
    # SERVER_CONFIG on the host's worker thread against the pass, the HTTP and CAT threads reading the engine's publish and feeding the pull,
    # the password's hash against a restart, a stop that waits for a browser's open request while the pass keeps its rate, the teardown
    test_server_host
    # stage 5c: CAT is an internal engine-API session - the real CAT listener over loopback sockets against the engine's pass on its own
    # thread: a session opened on the acceptor thread and closed by the connection's, results collected by the connection against the pass
    # that produces them, a thousand commands and a burst past the session's cap, a client that hangs up with its commands still queued,
    # the listener stopped (and the engine torn down) with clients connected
    test_cat_engine
    # ...and the window's side of it: a real AppWindow starting the engine's server from its settings panel, with a browser's audio stream and
    # spectrum long-poll served by the engine while the window never frames (stage 5e: the page's reads are the table's)
    test_window_web_feed
    # stage 5e: the receiver plugins' pictures are the engine's - the one accumulator polled by the window's reads and by the feed's worker beside it,
    # the BMP encode and the served set on the worker, the headers published after the set (fake decoders), and the whole chain headless: a real
    # picture decoder in the runner, the pipeline feeding it, the web server serving its picture
    test_web_pictures
    # the pump move's step P3 (PluginUi's thread ownership): the directive the engine's pass posts and the window applies at the head
    # of its poll, the bounded unload handshake, the queued request_tune, the plugin UI's instances created, polled and destroyed on
    # the window's thread alone - the link's protocol with threads; a real window, a real engine with its pass on its own thread
    # and an instrumented plugin module (rebuild churn against a polling frame, epochs skipped, a reference held across a
    # rebuild, a Doppler corrector, a rescan releasing the basemap and the track-info client); and the handshake's timings (a
    # window inside the plugin, a window not framing, a stop mid-wait, the watchdog pause, no window at all)
    test_plugin_directive test_plugin_ui_threaded test_plugin_unload_handshake
    # the pump move's steps P4 and P5 (the pass on a thread of its own, driven and looked at): the wake, the floor, the stepped mode, runPassNow
    # and the work posted to the pass against the engine's thread (test_pass_control); a real window's frame - the look, the submits, the key -
    # against a free-running pass, three session threads and a snapshot reader (test_window_threaded_look); the order the look reads in against a
    # publish that is held between its two halves (test_look_order); and the unload handshake measured under live traffic
    # (test_pump_live_traffic
    # the real web server over a loopback socket, with a client on another thread
    fuzz_corpus_web_http)

# THE PUMP MOVE'S P6: the window tests against the FREE-RUNNING pass (tests/CMakeLists.txt, CASCADE_FREE_PASS_TESTS; each is registered as
# <name>_free, FOXSDR_TEST_PASS=free). A hermetic window test is the engine's front end and its pass on two threads, which is exactly what a
# thread sanitizer exists for; they were not in the selection before because nothing but the engine-API tests were threaded. The one that
# drives the window under a software GL context (test_bias_key_app) stays out, with the reason below.
foreach(_t ${CASCADE_FREE_PASS_TESTS})
    if(NOT _t STREQUAL "test_bias_key_app")
        list(APPEND CASCADE_TSAN_TESTS ${_t}_free)
    endif()
endforeach()

set(CASCADE_TSAN_EXCLUDED_TESTS)

macro(_cascade_tsan_exclude reason)
    foreach(_n ${ARGN})
        list(APPEND CASCADE_TSAN_EXCLUDED_TESTS "${_n}|${reason}")
    endforeach()
endmacro()

_cascade_tsan_exclude(
    "it starts the real application as a child process: the child is a separate process whose own report would not reach this one's, and what the test claims is about the child"
    test_tester_link test_tester_usage test_soapy_source test_sentinel test_sentinel_app
    test_sentinel_proc test_stall_count test_diag_hang test_crash_upload test_health_app
    test_gps_app test_shutdown_budget test_display_stall test_main_view
    test_clean_exit_marker test_diagnostics test_slow_frames_app test_soapy_enum_proc
    test_theme_census app_smoke app_selftest app_version app_cli_bad_value app_cli_negative
    app_cli_missing_value app_cli_unknown_arg
    # run the application as a child too (CMakeLists.txt, _runs_app); master's list does not name them
    test_bias_key_run test_startup_state test_stop_ends_recordings test_plugin_status_report)

_cascade_tsan_exclude(
    "it faults a process on purpose to prove the crash handler writes its report, and ThreadSanitizer's own handling of the fault signals is in the way"
    test_crash_capture test_crash_late_module test_crash_second_fault
    test_crash_absorbed_child)

_cascade_tsan_exclude(
    "it measures audio against a source paced by the wall clock: an instrumented DSP thread runs several times slower and falls behind it, so it fails with no race report (the same tests are excluded under AddressSanitizer for the same reason)"
    test_pipeline_audio test_pipeline_device_rate_audio test_channel_bandwidth
    test_soundcard_source test_airband_app
    # (0.99.73) reads the drift matcher's correction against the real output's clock
    test_pipeline_audio_lead
    # (branch, judged by reading, not run) decodes a modulated FM carrier through the real
    # pipeline: the DSP thread is paced by the generator's clock like test_pipeline_audio
    test_engine_api_rds_real
    # Found by the first run (37355910809): its 1 kHz patch tone read 0.0047 where it
    # must exceed 0.01, "patch starved frames: 1", and no race report at all.
    test_patch_audio)

_cascade_tsan_exclude(
    "it drives the whole window under a software GL context: the threads in it are the test's own helpers, and Mesa's thread pool is what ThreadSanitizer would mostly see"
    test_bias_key_app test_bias_key_app_free test_diag_context_app test_one_chip_app test_bandwidth_app
    test_transmit_page test_ui_census)

_cascade_tsan_exclude(
    "NOT RUN YET: it is this branch's own and starts threads or drives code that has them, but it is not engine-threading proper (the window's frames, a signal-delivered stack capture, the sound card's list lock, the engine's per-pass publish) and has never been run under ThreadSanitizer, so the pump move's threaded window tests will say which of these join"
    test_engine_headless test_engine_publish_alloc test_snapshot_app
    test_hang_capture_concurrent test_pa_stream_list_guard test_patch_graph_draft_input)

# Everything else starts no thread of its own and drives no code that has one: a
# pure function, a parser, a model, a widget's arithmetic. It cannot race with
# anything, so it is not built or run in this job. (Reason shared by all of them,
# so it is not repeated per test in the configure output.)
set(CASCADE_TSAN_SINGLE_THREADED_REASON
    "it starts no thread and drives no code that has one, so there is nothing for ThreadSanitizer to find")

# Applies the lists above to the tests registered so far (CASCADE_ALL_TEST_NAMES),
# in the directory that registered them. A macro rather than a function so that it
# sets the exclusion list the sanitizer block of tests/CMakeLists.txt goes on to
# apply.
macro(cascade_tsan_select)
    set(CASCADE_SANITIZE_EXCLUDED_TESTS ${CASCADE_TSAN_EXCLUDED_TESTS})
    set(CASCADE_SANITIZE_FAULT_TESTS)
    set(_tsan_named_out)
    foreach(_entry ${CASCADE_TSAN_EXCLUDED_TESTS})
        string(REPLACE "|" ";" _entry "${_entry}")
        list(GET _entry 0 _n)
        list(APPEND _tsan_named_out ${_n})
    endforeach()
    # A name that is not a test is a typo that would otherwise be a quiet gap.
    foreach(_n ${CASCADE_TSAN_TESTS} ${_tsan_named_out})
        # (A pending test's _free / _stepped variants are not registered either: the name is the base test's.)
        string(REGEX REPLACE "_(free|stepped)$" "" _base "${_n}")
        if(NOT _n IN_LIST CASCADE_ALL_TEST_NAMES AND NOT _n IN_LIST CASCADE_MERGE_PENDING_TESTS
           AND NOT _base IN_LIST CASCADE_MERGE_PENDING_TESTS)
            message(FATAL_ERROR "tests/tsan.cmake names '${_n}', which is not a test")
        endif()
    endforeach()
    set(_tsan_deps)
    set(_tsan_single 0)
    foreach(_t ${CASCADE_ALL_TEST_NAMES})
        if(_t IN_LIST CASCADE_TSAN_TESTS)
            set_tests_properties(${_t} PROPERTIES LABELS "tsan")
            # (a <name>_free variant is the same binary as <name>: build that)
            string(REGEX REPLACE "_free$" "" _tsan_tgt "${_t}")
            if(TARGET ${_tsan_tgt})
                list(APPEND _tsan_deps ${_tsan_tgt})
            elseif(_t MATCHES "^fuzz_corpus_")
                list(APPEND _tsan_deps fuzz_corpus_replay)
            endif()
        elseif(NOT _t IN_LIST _tsan_named_out)
            set_tests_properties(${_t} PROPERTIES DISABLED TRUE LABELS "sanitize-excluded")
            math(EXPR _tsan_single "${_tsan_single} + 1")
        endif()
    endforeach()
    list(REMOVE_DUPLICATES _tsan_deps)
    add_custom_target(cascade_tsan_tests DEPENDS ${_tsan_deps})
    list(LENGTH CASCADE_TSAN_TESTS _tsan_in)
    list(LENGTH _tsan_named_out _tsan_out)
    message(STATUS "thread sanitizer: ${_tsan_in} tests run, ${_tsan_out} excluded one by one, "
                   "${_tsan_single} excluded as single-threaded (${CASCADE_TSAN_SINGLE_THREADED_REASON})")
endmacro()
