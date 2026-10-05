# Which tests run under ThreadSanitizer (-DCASCADE_SANITIZE=thread), and why the
# others do not. Included from tests/CMakeLists.txt in a thread build only.
#
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
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
    # patch radios: one reader thread each, and a speaker written from its own
    test_patch_radio test_patch_radio_abandon test_patch_dest_async test_patch_runner
    test_patch_decoders test_patch_audio
    test_converter_app_paths test_converter_routing
    # the plugin runner and the host API plugins call from their own threads
    test_plugin_runner test_plugin_destroy_reentry test_plugin_api test_plugin_audio
    test_plugin_poll_bound test_plugin_ui test_plugin_abi3_compat test_plugin_repo
    # sources with threads of their own, and the vendor-call guard
    test_sdrplay_stall test_sdrplay_source test_sdrplay_service test_soapy_read_bound
    test_soapy_vendor_guard test_vendor_guard test_iq_file_source test_pluto_source
    test_pluto_tx test_aor_source test_one_chip_one_route test_ppm_app
    # work moved off the window's thread (0.99.64 and before): the savers, the disk
    # jobs, the opens, the polls
    test_config_save test_bookmark_save_async test_gui_disk_audit test_gui_file_jobs
    test_iq_open_async test_link_request_poll test_record_start test_audio_open
    test_tester_link_app test_patch_presets_app test_soundcard_app_paths
    # the health ledger's writer, the frame timer's cross-thread reads, the watchdog
    test_health_events test_health_paths test_frame_timing test_hang_signature
    test_excuse_cap test_watchdog_startup test_telemetry
    # servers and senders
    test_web_server test_cat_server test_httplib_vendor test_updater test_feature_request
    test_gps_reader test_serial_port test_transmitter
    # the sound sink: PortAudio's callback thread against the writer
    test_audio_out test_audio_in test_audio_open_log
    # the real web server over a loopback socket, with a client on another thread
    fuzz_corpus_web_http)

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
    app_cli_missing_value app_cli_unknown_arg)

_cascade_tsan_exclude(
    "it faults a process on purpose to prove the crash handler writes its report, and ThreadSanitizer's own handling of the fault signals is in the way"
    test_crash_capture test_crash_late_module test_crash_second_fault
    test_crash_absorbed_child)

_cascade_tsan_exclude(
    "it measures audio against a source paced by the wall clock: an instrumented DSP thread runs several times slower and falls behind it, so it fails with no race report (the same tests are excluded under AddressSanitizer for the same reason)"
    test_pipeline_audio test_pipeline_device_rate_audio test_channel_bandwidth
    test_soundcard_source test_airband_app)

_cascade_tsan_exclude(
    "it drives the whole window under a software GL context: the threads in it are the test's own helpers, and Mesa's thread pool is what ThreadSanitizer would mostly see"
    test_bias_key_app test_diag_context_app test_one_chip_app test_bandwidth_app
    test_transmit_page test_ui_census)

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
            if(TARGET ${_t})
                list(APPEND _tsan_deps ${_t})
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
