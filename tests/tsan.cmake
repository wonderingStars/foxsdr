# Which tests run under ThreadSanitizer (-DCASCADE_SANITIZE=thread), and why the
# others do not. Included from tests/CMakeLists.txt in a thread build only.
#
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#
# ENGINE/STAGE4 BRANCH VERSION. Ported from origin/master's tests/tsan.cmake ahead of
# the full merge, as groundwork for the pump move's threaded tests (docs/engine-stage4.md).
# The mechanism below the lists - the macro, the typo check, the label, the target - is
# master's, unchanged. The LISTS are the branch's own, because the branch forked near
# 0.99.42 and has 154 tests master has since added, and 52 tests master does not have.
#
# THE QUESTION THIS ANSWERS is "which tests put two of our threads against each
# other", because that is all a thread sanitizer can find: a data race, a lock-order
# inversion, a mutex misused. A test with one thread in it, and no thread inside
# the code it drives, cannot fail under TSan and costs a build, a link and several times
# its run time for nothing. So the job runs a SELECTION (the label `tsan`) and builds
# only that (the target cascade_tsan_tests); every other test is registered all the
# same, disabled and labelled `sanitize-excluded`, so ctest lists it and the reason
# is here, as under AddressSanitizer.
#
# A test is IN if it starts threads of its own against product code, or drives code
# that has threads. It is judged by reading what the test starts, not by its name.
#
# THE FIRST SELECTION IS THE ENGINE'S THREADS, NOT MASTER'S WHOLE LIST. Master's list
# (70 tests, written against the tree at 0.99.64+) found its first races in the signal
# generator and in tests, and fixed them there; this branch does not have those fixes
# yet, so running master's list here would mostly re-report what master already
# repaired, in a job whose purpose is to be quiet enough to read when the pump move's
# own threaded tests go in. So: IN is what the engine's control thread, the front end's
# thread and the transmitter's thread do to each other (below); the 46 of master's
# thread-bearing tests that this branch also has, and 7 of this branch's own that are
# not engine-threading proper, are excluded BY NAME with the reason "not run yet" and
# come in as the master merge brings the fixes. Moving one is a one-word change.
#
# A test with threads that is OUT is out for one of five reasons, written beside its
# name below: it starts the real application as a child, it faults a process on
# purpose, it measures against the wall clock, it drives the whole window under a
# software GL context, or it is not run yet (above).
#
# THIS LIST WAS WRITTEN WITHOUT A LINUX RUN ON THIS BRANCH. The first log is what shows
# which of the "in" tests are too noisy or too slow, and which "out" tests are worth adding.
# Of the "in" tests, test_engine_api_transmit and test_engine_api_merge run at real
# timers (RUN_SERIAL, TIMEOUT 600 before the x10 scale of cmake/sanitize.cmake): a probe's
# heartbeat starving on a slow runner is the rule working, not a race, and reads as a
# failure with no TSan report.

set(CASCADE_TSAN_TESTS
    # the engine API held to the reference checklist, and the API's own groups, driven
    # through the C table against a real headless Engine on its own control thread
    test_engine_api_conformance test_engine_api_03 test_engine_api_merge
    test_engine_api_transmit
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
    # the pump move's step P1 (the review's F3): the engine PULLS the web remote's and CAT's requests in its own pass, and the key
    # rules are touched by the control thread alone - producers, two local sessions and a flapping server flag against the threaded
    # pass, the ordering, the 2 s hold, the latch's marking and the re-arm (test_transmit_key_race's case G is the same storm
    # against the hammer); and the same standing conditions through the window's own source and a real web server
    test_remote_control_pull test_remote_key_release
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
    # (test_pump_live_traffic)
    test_pass_control test_window_threaded_look test_look_order test_pump_live_traffic)

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
    test_soapy_source test_diag_hang test_crash_upload test_gps_app test_shutdown_budget
    test_display_stall test_main_view test_diagnostics test_soapy_enum_proc test_theme_census
    # run the application as a child too (CMakeLists.txt, _runs_app); master's list does not name them
    test_bias_key_run test_startup_state test_stop_ends_recordings test_plugin_status_report
    app_smoke app_selftest app_version app_cli_bad_value app_cli_negative app_cli_missing_value
    app_cli_unknown_arg)

_cascade_tsan_exclude(
    "it faults a process on purpose to prove the crash handler writes its report, and ThreadSanitizer's own handling of the fault signals is in the way"
    test_crash_capture test_crash_second_fault test_crash_absorbed_child)

_cascade_tsan_exclude(
    "it measures audio against a source paced by the wall clock: an instrumented DSP thread runs several times slower and falls behind it, so it fails with no race report (the same tests are excluded under AddressSanitizer for the same reason)"
    test_pipeline_audio test_soundcard_source
    # Found by master's first run (37355910809): its 1 kHz patch tone read 0.0047 where it
    # must exceed 0.01, "patch starved frames: 1", and no race report at all.
    test_patch_audio
    # (branch, judged by reading, not run) decodes a modulated FM carrier through the real
    # pipeline: the DSP thread is paced by the generator's clock like test_pipeline_audio
    test_engine_api_rds_real)

_cascade_tsan_exclude(
    "it drives the whole window under a software GL context: the threads in it are the test's own helpers, and Mesa's thread pool is what ThreadSanitizer would mostly see"
    test_bias_key_app test_bias_key_app_free test_transmit_page test_ui_census)

_cascade_tsan_exclude(
    "NOT RUN YET: it is on master's ThreadSanitizer list (threads of its own against product code) and this branch has it, but master found its first races in code and tests this branch does not have the fixes for yet, so it joins the selection with the master merge (or earlier, one test at a time)"
    test_pipeline test_pipeline_getters test_pipeline_ring_rate test_pipeline_file_fault
    test_thread_fault test_source_swap test_spsc_ring test_scope_tap test_rate_follow
    test_scope_taps_wired test_siggen
    test_patch_radio test_patch_radio_abandon test_patch_runner test_patch_decoders
    test_converter_app_paths test_converter_routing
    test_plugin_runner test_plugin_destroy_reentry test_plugin_api test_plugin_audio
    test_plugin_poll_bound test_plugin_ui test_plugin_abi3_compat test_plugin_repo
    test_sdrplay_stall test_sdrplay_source test_soapy_vendor_guard test_vendor_guard
    test_iq_file_source test_pluto_source test_pluto_tx
    test_config_save test_audio_open test_soundcard_app_paths test_telemetry
    test_web_server test_cat_server test_httplib_vendor test_updater test_feature_request
    test_gps_reader test_serial_port test_transmitter test_audio_out test_audio_in)

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
        if(NOT _n IN_LIST CASCADE_ALL_TEST_NAMES)
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
