# One executable per test_*.cpp, discovered by glob. Agents add test files
# only; this file itself almost never needs editing for a new test — the one
# exception is a test that needs a command-line argument, handled below by
# name for test_json_dump_policy.
file(GLOB CASCADE_TESTS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/test_*.cpp")
foreach(src ${CASCADE_TESTS})
    get_filename_component(name "${src}" NAME_WE)
    add_executable(${name} "${src}")
    # The library a test links follows what it includes: a test that reaches
    # into the window (a gui/ header, imgui, GLFW) links cascade_gui, which
    # brings the engine with it; every other test links the ENGINE ALONE - so
    # each of those is also a small proof that the engine code it exercises
    # needs nothing of the window to link.
    file(STRINGS "${src}" _window_includes REGEX
         "^[ \t]*#[ \t]*include[ \t]*[<\"](gui/|imgui|GLFW/|backends/imgui)")
    if(_window_includes)
        target_link_libraries(${name} PRIVATE cascade_gui)
    else()
        target_link_libraries(${name} PRIVATE cascade_engine)
    endif()
    target_include_directories(${name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    # The engine-API conformance ports (engine-stage4.md) use the reference
    # tests' own harness (check.hpp, engine_client.hpp), vendored beside the
    # mock engine they were written against.
    if(name MATCHES "^test_engine_api_")
        target_include_directories(${name} PRIVATE
            "${CMAKE_SOURCE_DIR}/third_party/foxsdr_api/reference/tests")
    endif()
    # Put SoapySDR.dll beside the test binary EXPLICITLY.
    #
    # The application delay-loads it, so a missing runtime there is a readable
    # message; the tests link it normally, so a missing DLL is a Windows modal
    # ("The code execution cannot proceed because SoapySDR.dll was not found")
    # that blocks the run until someone clicks OK — which on an unattended
    # build means a hang, not a failure.
    #
    # This used to be supplied by vcpkg's implicit applocal-deps step, which
    # made the suite pass in the established build tree and fail in any FRESH
    # one: the first nightly build hit exactly that modal. Copying it here
    # depends on nothing implicit and works in a clean clone and in CI.
    if(WIN32 AND TARGET SoapySDR)
        add_custom_command(TARGET ${name} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    $<TARGET_FILE:SoapySDR> $<TARGET_FILE_DIR:${name}>)
    endif()

    if(name STREQUAL "test_font_assets")
        # Compares something compiled INTO the binary against the files it was
        # generated from - the typefaces against third_party/fonts - so it
        # needs the repository root on its command line.
        add_test(NAME ${name} COMMAND ${name} "${CMAKE_SOURCE_DIR}")
    elseif(name STREQUAL "test_i18n" OR name STREQUAL "test_i18n_glyphs" OR
           name STREQUAL "test_countries")
        # The translation tests read resources/lang, resources/bandplans and
        # scan src/ for the keys the code asks for, so they take the
        # repository root too. Run by hand without it, each walks up from its
        # working directory instead.
        add_test(NAME ${name} COMMAND ${name} "${CMAKE_SOURCE_DIR}")
    elseif(name STREQUAL "test_json_dump_policy")
        # The only test in this loop that takes an argument: it scans src/
        # for nlohmann `.dump(` sites missing the error_handler_t::replace
        # guard (see tests/test_json_dump_policy.cpp), so it needs the
        # absolute src/ directory on its command line. This branch touches
        # only this one test's registration — every other name still gets
        # the plain `add_test(NAME ${name} COMMAND ${name})` below unchanged,
        # so adding this cannot register any test's NAME twice (CMake treats
        # that as a hard configure error, not a warning).
        add_test(NAME ${name} COMMAND ${name} "${CMAKE_SOURCE_DIR}/src")
    else()
        add_test(NAME ${name} COMMAND ${name})
    endif()
    # A liveness bug (stuck spin loop, deadlock) must fail the suite, not hang
    # it forever — ctest's default per-test timeout is 1500 s.
    set_tests_properties(${name} PROPERTIES TIMEOUT 120)
    # For the sanitizer block at the end of this file: every test this loop
    # registers, by name.
    list(APPEND CASCADE_ALL_TEST_NAMES ${name})
    # The AddressSanitizer runtime DLL beside the test (MSVC sanitizer build
    # only), for the reason SoapySDR.dll is staged above.
    if(CASCADE_SANITIZE_ENABLED)
        cascade_sanitize_stage_runtime(${name})
    endif()
endforeach()

# THE WINDOW TESTS AGAINST THE ENGINE'S PASS ON ITS OWN THREAD (the pump move's step P5; tests/pass_seam.hpp). A hermetic window test is its
# own pump: it never starts the engine's control thread and runs one frame's engine part by hand. Every test that says "one pass" through
# the seam is registered again, with FOXSDR_TEST_PASS set, so the same test runs against the pass on the control thread:
#   _stepped  the thread exists and runs a whole pass (its three parts in turn) each time the test asks for one, and the test waits for it -
#             the same order as inline, on the real thread (onControlThread(), the directive handshake and the table's thread gating are real)
#   _free     the thread free-runs (3 ms) and "one pass" is Engine::runPassNow() - passes of its own beside the test's. Only for the tests
#             that touch the engine through the session and the published state; a test that reads or writes an engine field directly races
#             the pass in this mode (that is a finding about the test, listed beside the names below, not something to paper over)
# (Under a ThreadSanitizer build only the _free variants are registered, and tests/tsan.cmake selects them: that is where a window test's
# thread meets the free-running pass. The stepped ones order threads and cannot race, so a sanitizer build does not register them.)
if(NOT CASCADE_SANITIZE_ENABLED OR CASCADE_SANITIZE_THREAD)
    # THE FREE-RUNNING SET (the pump move's P6): every window test that reaches the engine only through the session, the published state,
    # the seam's passseam::on/read (the work runs ON THE PASS THREAD with the pass out of the way) or a documented thread-safe accessor.
    #   P5 had 11 in: test_airspy_app test_config_snapshot_app test_gps_window test_map_window test_panel_note_once
    #       test_patch_graph_draft_input test_plugin_window test_rail_reads test_remote_key_release test_scanner_draft_input
    #       test_usage_diag_window.
    #   P6 CONVERTED the 14 that P5 left out (each read or wrote engine fields from the test's thread beside the pass: crashes, 7/8, 2/8):
    #       test_apply_command test_bias_key_app test_converter_app_paths test_decoders_mute_grants test_fitted_modules_window
    #       test_patch_add_radio_part test_plugin_store_window test_radio_panel_reads test_recorder_bookmarks_scanner test_scope_window
    #       test_snapshot_app test_soundcard_app_paths test_source_section_window test_tuning_surface
    #     by moving every engine touch onto passseam::on/read and, where an assertion is about what happens BETWEEN two passes ("the click
    #     only queued it", "nothing is applied yet"), holding the pass for that stretch only (a posted job that blocks the pass thread, or
    #     Engine::TestHooks::betweenPublishes parking it between the two publishes) - the pass free-runs everywhere else. Each ran inline 3,
    #     stepped 5, free solo 8 and free under load 24 (six copies at once) green, with a break-it. ONE CHECK stays stepped-only, in
    #     test_recorder_bookmarks_scanner D: the scanner's "Scanning | 144.0000 MHz" right after START is tick-exact (a free pass ticks on
    #     its own clock); in free mode it is skipped, said so, and the readout is checked against the scanner's own frequency under a hold.
    #   (docs/engine-stage4.md, the P6 record, has the per-test detail.)
    set(CASCADE_FREE_PASS_TESTS test_airspy_app test_config_snapshot_app test_gps_window test_map_window
        test_panel_note_once test_patch_graph_draft_input test_plugin_window test_rail_reads test_remote_key_release
        test_scanner_draft_input test_usage_diag_window
        test_apply_command test_bias_key_app test_converter_app_paths test_decoders_mute_grants test_fitted_modules_window
        test_patch_add_radio_part test_plugin_store_window test_radio_panel_reads test_recorder_bookmarks_scanner test_scope_window
        test_snapshot_app test_soundcard_app_paths test_source_section_window test_tuning_surface)
    foreach(src ${CASCADE_TESTS})
        get_filename_component(_name "${src}" NAME_WE)
        file(STRINGS "${src}" _seam REGEX "^[ \t]*#[ \t]*include[ \t]*\"pass_seam[.]hpp\"")
        if(_seam AND TARGET ${_name})
            if(NOT CASCADE_SANITIZE_ENABLED)
                add_test(NAME ${_name}_stepped COMMAND ${_name})
                set_tests_properties(${_name}_stepped PROPERTIES ENVIRONMENT "FOXSDR_TEST_PASS=stepped" TIMEOUT 120)
            endif()
            if(${_name} IN_LIST CASCADE_FREE_PASS_TESTS)
                add_test(NAME ${_name}_free COMMAND ${_name})
                set_tests_properties(${_name}_free PROPERTIES ENVIRONMENT "FOXSDR_TEST_PASS=free" TIMEOUT 120)
                if(CASCADE_SANITIZE_ENABLED)
                    # (the sanitizer block below sets the runtime options and adds FOXSDR_TEST_PASS=free back for the names ending _free)
                    list(APPEND CASCADE_ALL_TEST_NAMES ${_name}_free)
                endif()
            endif()
        endif()
    endforeach()
endif()

# THE REAL-BINARY TESTS ALSO RUN WITH THE ENGINE'S PASS FREE-RUNNING (the pump move's step P5, adopted decision 3). A bounded run (--frames N)
# STEPS the pass from the frame - so N frames is the same N frames and a picture at a frame is reproducible - and these eleven assert
# invariants, not frame-exact states, so they hold with the pass running on its own thread beside the frame as it does in the product:
# FOXSDR_PASS=free (read by AppWindow::run; the child process inherits it). Each ran six more times on top of its first green (11 of 11
# 6/6). OUT, because they are frame-exact: test_bias_key_run (a scripted click at a fixed frame expects the dialog and the lamp a few frames
# later: 5 of 6 - a pass at 60 a second has not answered by then at 1300 frames a second) and test_main_view (the UI census walks the rail's
# banks by frame number: 5 of 6); test_theme_census (sixteen layouts photographed at fixed frames) was not run free at all. The picture
# campaigns stay stepped.
if(NOT CASCADE_SANITIZE_ENABLED)
    foreach(_t IN ITEMS test_config test_crash_upload test_diag_hang test_diagnostics test_gps_app test_patch_map_credit
                       test_plugin_status_report test_shell_open test_shutdown_budget test_startup_state test_stop_ends_recordings)
        if(TARGET ${_t})
            add_test(NAME ${_t}_free COMMAND ${_t})
            set_tests_properties(${_t}_free PROPERTIES ENVIRONMENT "FOXSDR_PASS=free" TIMEOUT 300)
        endif()
    endforeach()
endif()

# TESTS THAT RUN THE APPLICATION ITSELF (CASCADE_APP_BINDIR/cascade.exe, or
# the exe beside the test binaries' parent for the Soapy child tests). Building
# one of them alone must rebuild the application too, or its end-to-end half
# drives whatever stale cascade.exe the build tree last held.
foreach(_runs_app IN ITEMS test_bias_key_run test_crash_upload test_diag_hang test_diagnostics test_plugin_status_report
                           test_gps_app test_shutdown_budget test_startup_state
                           test_stop_ends_recordings test_theme_census test_soapy_enum_proc
                           test_soapy_source)
    if(TARGET ${_runs_app})
        add_dependencies(${_runs_app} cascade)
    endif()
endforeach()

# THE STAGE-1 FIXTURE PLUGIN (engine extraction, docs/engine-stage1.md): a
# real module - a decoder, two presets and a command key - so
# test_apply_command applies the plugin ops to a loaded plugin. Built beside
# the tests; the test copies it into its OWN scratch plugin directory
# (AppWindow::testHooks_.pluginDir), never where other tests look.
add_library(fixture_stage1_plugin SHARED fixture_stage1_plugin.cpp)
target_include_directories(fixture_stage1_plugin PRIVATE "${CMAKE_SOURCE_DIR}/src")
add_dependencies(test_apply_command fixture_stage1_plugin)
target_compile_definitions(test_apply_command PRIVATE
    FIXTURE_STAGE1_PLUGIN="$<TARGET_FILE:fixture_stage1_plugin>")
# THE GROUP-8 FIXTURE (window port group 8, API 0.3 section 8.7): a second real module - an I/Q
# decoder (mute-while-running defaults ON), a DEVICE-CENTRE preset, a preset the host drops, a
# preset with no label, and two marks on the spectrum - loaded beside the stage-1 fixture by the
# tests that read the plugin lists (test_engine_api_03 groups M, test_decoders_mute_grants). Each
# copies them into a plugin directory of its own (Engine::testHooks_.pluginDir).
add_library(fixture_group8_plugin SHARED fixture_group8_plugin.cpp)
target_include_directories(fixture_group8_plugin PRIVATE "${CMAKE_SOURCE_DIR}/src")
foreach(_t IN ITEMS test_engine_api_03 test_engine_publish_alloc test_decoders_mute_grants test_fitted_modules_window test_plugin_store_window test_plugin_window test_plugin_status_report)
    if(TARGET ${_t})
        add_dependencies(${_t} fixture_stage1_plugin fixture_group8_plugin)
        target_compile_definitions(${_t} PRIVATE
            FIXTURE_STAGE1_PLUGIN="$<TARGET_FILE:fixture_stage1_plugin>"
            FIXTURE_GROUP8_PLUGIN="$<TARGET_FILE:fixture_group8_plugin>")
    endif()
endforeach()
# THE GROUP-9 FIXTURE (window port group 9, API 0.3 section 8.8): a third real module that is NOT a
# decoder - an in-chain audio processor with two command keys and a descriptor of its own (a version,
# an author, a licence) - for the tests of the fitted-modules window's reads (test_engine_api_03 group
# H, test_engine_publish_alloc case F3) and of the window that draws them (test_fitted_modules_window, which
# also loads the two fixtures above). Copied into a plugin directory of the test's own.
add_library(fixture_group9_plugin SHARED fixture_group9_plugin.cpp)
target_include_directories(fixture_group9_plugin PRIVATE "${CMAKE_SOURCE_DIR}/src")
foreach(_t IN ITEMS test_engine_api_03 test_engine_publish_alloc test_fitted_modules_window test_plugin_store_window)
    if(TARGET ${_t})
        add_dependencies(${_t} fixture_group9_plugin)
        target_compile_definitions(${_t} PRIVATE
            FIXTURE_GROUP9_PLUGIN="$<TARGET_FILE:fixture_group9_plugin>")
    endif()
endforeach()
# THE GROUP-13 FIXTURE (window port group 13, part c, API 0.3 section 8.12.7): a REAL picture decoder - the one module that
# produces a picture through the plugin ABI, deterministically, from the audio a patch on the signal generator hands it - for
# the tests of the patch decoders' pictures (test_engine_api_03 G13-5, test_engine_publish_alloc F8) and of the face that
# draws them (test_patch_graph_draft_input scenario W). Copied into a plugin directory of the test's own.
add_library(fixture_group13_plugin SHARED fixture_group13_plugin.cpp)
target_include_directories(fixture_group13_plugin PRIVATE "${CMAKE_SOURCE_DIR}/src")
foreach(_t IN ITEMS test_engine_api_03 test_engine_publish_alloc test_patch_graph_draft_input test_plugin_window test_plugin_status_report)
    if(TARGET ${_t})
        add_dependencies(${_t} fixture_group13_plugin)
        target_compile_definitions(${_t} PRIVATE
            FIXTURE_GROUP13_PLUGIN="$<TARGET_FILE:fixture_group13_plugin>")
    endif()
endforeach()
# THE GROUP-14 FIXTURE (window port group 14, part a): a REAL track source - four aircraft at fixed places (one of them
# stale) and a trail - for the tests of the map pages, the target register, the details window and the offer to take the
# middle of the traffic (test_map_window). Before it no hermetic frame could put a track in the window's plugin UI: the
# aircraft of the radar scope and of the Patch page's Map part were held by code reading. Copied into a plugin directory of
# the test's own. SINCE PART D it also declares a PANEL and an INSTRUMENT (a fixed table and a fixed reading), so the plugin
# windows the plugin UI feeds besides the map have something to draw (test_plugin_window, which loads this fixture with the
# stage-1 text decoder and the group-13 picture decoder).
add_library(fixture_group14_plugin SHARED fixture_group14_plugin.cpp)
target_include_directories(fixture_group14_plugin PRIVATE "${CMAKE_SOURCE_DIR}/src")
foreach(_t IN ITEMS test_map_window test_plugin_window test_plugin_status_report)
    if(TARGET ${_t})
        add_dependencies(${_t} fixture_group14_plugin)
        target_compile_definitions(${_t} PRIVATE
            FIXTURE_GROUP14_PLUGIN="$<TARGET_FILE:fixture_group14_plugin>")
    endif()
endforeach()
# ...and a SECOND track source from the same file under another name and id prefix ("Group Fourteen Tracks B", ids G14B...), for
# the tests of one map page per plugin (test_plugin_window: the per-page filter of tracks and paths, a page pruned beside a
# live one).
add_library(fixture_group14_plugin_b SHARED fixture_group14_plugin.cpp)
target_include_directories(fixture_group14_plugin_b PRIVATE "${CMAKE_SOURCE_DIR}/src")
target_compile_definitions(fixture_group14_plugin_b PRIVATE FIXTURE_NAME="Group Fourteen Tracks B" FIXTURE_PREFIX="G14B")
# (window port group 14, part e: test_plugin_status_report fits four of these fixtures to hold the bounded-run status report the
# engine prints, line by line.)
foreach(_t IN ITEMS test_plugin_window test_plugin_status_report)
    if(TARGET ${_t})
        add_dependencies(${_t} fixture_group14_plugin_b)
        target_compile_definitions(${_t} PRIVATE
            FIXTURE_GROUP14B_PLUGIN="$<TARGET_FILE:fixture_group14_plugin_b>")
    endif()
endforeach()
# THE PUMP MOVE'S STEP P3 FIXTURE (tests/fixture_pump3_plugin.cpp): an INSTRUMENTED track source that is also a host client (it can
# Doppler-churn request_tune from its poll), with a basemap and a track-info table, which counts every create and destroy, the
# thread each ran on, any poll or destroy through a dead handle and any destroy beside a poll, can be HELD inside poll_tracks, and
# writes how many handles were alive when its image was unmapped. For the tests of the plugin UI's thread ownership: the directive
# the engine posts and the window applies, and the unload handshake. Copied into a plugin directory of the test's own.
add_library(fixture_pump3_plugin SHARED fixture_pump3_plugin.cpp)
target_include_directories(fixture_pump3_plugin PRIVATE "${CMAKE_SOURCE_DIR}/src")
foreach(_t IN ITEMS test_plugin_ui_threaded test_plugin_unload_handshake test_pump_live_traffic)
    if(TARGET ${_t})
        add_dependencies(${_t} fixture_pump3_plugin)
        target_compile_definitions(${_t} PRIVATE
            FIXTURE_PUMP3_PLUGIN="$<TARGET_FILE:fixture_pump3_plugin>")
    endif()
endforeach()
# THE TWO THREADED PLUGIN-UI TESTS RUN ALONE (RUN_SERIAL), for the reason the transmit suite does: they hold REAL bounds (the unload
# handshake's 250 ms, a stop that must come back in milliseconds, a frame that must acknowledge within one) and spin a frame thread
# against the engine's pass for seconds, so beside seven other tests on a loaded runner a bound can be missed by the machine and not by
# the code - and they would add their own load to the audio-timing tests beside them.
if(TARGET test_plugin_ui_threaded AND TARGET test_plugin_unload_handshake)
    set_tests_properties(test_plugin_ui_threaded test_plugin_unload_handshake PROPERTIES RUN_SERIAL TRUE)
endif()
# THE PUMP MOVE'S STEP P5 TESTS THAT HOLD REAL TIMES RUN ALONE TOO: test_pump_live_traffic measures the handshake and the transmit key
# against a framing window (and a silent one) at real bounds - 250 ms waits, 400 ms silences, a 1 s keep-alive - and test_pass_control
# holds a wake to a few milliseconds and a floor to a count of passes in a second; test_window_threaded_look spins four threads against
# the pass for three seconds and test_look_order two more for several - beside seven other tests on a loaded runner a bound can be missed
# by the machine and not by the code.
foreach(_t IN ITEMS test_pump_live_traffic test_pass_control test_window_threaded_look test_look_order)
    if(TARGET ${_t})
        set_tests_properties(${_t} PROPERTIES RUN_SERIAL TRUE)
    endif()
endforeach()
# test_snapshot_app loads three decoders at once (engine stage 2): the same
# fixture built under three descriptor names, since the host runs one plugin
# per name.
foreach(_fx IN ITEMS a b c)
    add_library(fixture_snap_${_fx} SHARED fixture_stage1_plugin.cpp)
    target_include_directories(fixture_snap_${_fx} PRIVATE "${CMAKE_SOURCE_DIR}/src")
    target_compile_definitions(fixture_snap_${_fx} PRIVATE FIXTURE_NAME="Snapshot Fixture ${_fx}")
    add_dependencies(test_snapshot_app fixture_snap_${_fx})
endforeach()
target_compile_definitions(test_snapshot_app PRIVATE
    FIXTURE_SNAP_A="$<TARGET_FILE:fixture_snap_a>"
    FIXTURE_SNAP_B="$<TARGET_FILE:fixture_snap_b>"
    FIXTURE_SNAP_C="$<TARGET_FILE:fixture_snap_c>")

# THE STAGE-2 GOLDEN RECORD (docs/engine-stage2.md): what every reader of the
# receiver's state was told before stage 2, written by that code, compared by
# this one. The test reads it from the source tree.
target_compile_definitions(test_state_snapshot_golden PRIVATE
    GOLDEN_FILE="${CMAKE_CURRENT_SOURCE_DIR}/golden/state_snapshot.golden")

# The engine, whole archive, linked with nothing of the window (top-level
# CMakeLists.txt, "the line between them"). The build is the real check; this
# confirms the binary it produced runs.
add_test(NAME engine_link_check COMMAND cascade_engine_link_check)
set_tests_properties(engine_link_check PROPERTIES
    TIMEOUT 30
    PASS_REGULAR_EXPRESSION "cascade_engine links on its own: FoxSDR")

# The app doubles as a smoke test. The PASS_REGULAR_EXPRESSION makes the
# exact-count half of the --frames contract observable: rendering N+1 frames
# (or none) fails even though the exit code would still be 0.
# The theme census starts the application sixteen times (eight layouts at two
# window sizes), so it gets more than the loop's two minutes - most of all on a
# software-rendered Linux runner.
set_tests_properties(test_theme_census PROPERTIES TIMEOUT 600)
# The reference's transmit safety suite, ported, runs its rules at the real
# timers (holds of 2 s, keep-alives of 1-2 s, a re-arm of 1 s, over and over):
# about 95 s here. The merge sweep is 5450 cases of one pass each (about 40 s;
# FOXSDR_MERGE_FULL=1 runs all 70450, about 10 minutes, by hand). Both get
# room for a slow or shared runner - and each runs ALONE (RUN_SERIAL): the
# transmit suite holds real 1 s keep-alives on up to 15 threads at once and the
# sweep keeps a worker per core busy, so beside each other (and two more
# jobs) on a 4-core runner a probe's heartbeat can starve past its keep-alive
# and the key ends "because the interface stopped answering" - the rule
# working, on a machine too busy for the probe to hold up its end.
set_tests_properties(test_engine_api_transmit test_engine_api_merge PROPERTIES TIMEOUT 600 RUN_SERIAL TRUE)

add_test(NAME app_smoke COMMAND cascade --frames 3)
set_tests_properties(app_smoke PROPERTIES
    TIMEOUT 60
    PASS_REGULAR_EXPRESSION "rendered 3 frames")

# End-to-end DSP proof, headless: tone in at +300 kHz must peak at the right
# fftshifted bin. This is the wiring test the unit suites cannot provide.
add_test(NAME app_selftest COMMAND cascade --selftest)
set_tests_properties(app_selftest PROPERTIES
    TIMEOUT 60
    PASS_REGULAR_EXPRESSION "selftest PASS")

# --version is read back by tools/build-nightly.ps1 to check the binary agrees
# with the name its installer is about to be given, so its FORMAT is a contract,
# not a convenience. The regex pins the injected version too: a build whose
# CASCADE_VERSION_STRING never reached version.cpp reports the
# "0.0.0-unconfigured" fallback and fails here rather than shipping.
add_test(NAME app_version COMMAND cascade --version)
set_tests_properties(app_version PROPERTIES
    TIMEOUT 30
    PASS_REGULAR_EXPRESSION "^FoxSDR ${CASCADE_VERSION_STRING}\n$")

# CLI error contract: each malformed invocation must exit nonzero.
add_test(NAME app_cli_bad_value COMMAND cascade --frames abc)
add_test(NAME app_cli_negative COMMAND cascade --frames -1)
add_test(NAME app_cli_missing_value COMMAND cascade --frames)
add_test(NAME app_cli_unknown_arg COMMAND cascade --bogus)
set_tests_properties(app_cli_bad_value app_cli_negative app_cli_missing_value
                     app_cli_unknown_arg PROPERTIES WILL_FAIL TRUE TIMEOUT 30)

# ---------------------------------------------------------------------------
# Sanitizer build (-DCASCADE_SANITIZE=..., cmake/sanitize.cmake). Nothing below
# runs, and nothing above changes, when it is off. AT THE END OF THE FILE ON
# PURPOSE: it reads each test's TIMEOUT to scale it, so every property set
# above must already be set.
#
# Ported from origin/master ahead of the full merge (docs/SANITIZERS-AND-FUZZING.md,
# "Engine/stage4 branch"). Master's block, with two differences: the exclusion
# lists are filtered to the tests THIS branch registers (set_tests_properties on
# a test that is not there is a configure error), and the ThreadSanitizer list in
# tests/tsan.cmake is the branch's own.
# ---------------------------------------------------------------------------
if(CASCADE_SANITIZE_ENABLED)
    list(APPEND CASCADE_ALL_TEST_NAMES engine_link_check app_smoke app_selftest app_version
         app_cli_bad_value app_cli_negative app_cli_missing_value app_cli_unknown_arg)

    # TESTS THAT CANNOT RUN UNDER A SANITIZER, AND WHY. Each is still registered,
    # so ctest lists it - "Not Run (Disabled)" in its summary, and
    # `ctest -N -L sanitize-excluded` prints the set - rather than the suite
    # quietly getting smaller. One "name|reason" per entry (no semicolons). These
    # are master's AddressSanitizer exclusions; only the ones whose test exists
    # here are kept. (A ThreadSanitizer build replaces this list: tests/tsan.cmake.)
    set(_cascade_san_excluded_master
        "test_crash_second_fault|its heap-trashed case writes into freed heap blocks on purpose to prove the crash handler survives a damaged heap and AddressSanitizer reports that write as a heap-use-after-free and ends the process before the handler under test runs"
        "test_channel_bandwidth|its pipeline cases measure audio against a real-time source and the instrumented DSP thread falls behind the clock (sink underruns) so tone levels fail with no sanitizer report"
        "test_pipeline_audio|its pipeline cases measure audio against a real-time source and the instrumented DSP thread falls behind the clock (sink underruns) so RDS and stereo measurements fail with no sanitizer report"
        "test_pipeline_device_rate_audio|its pipeline cases measure audio against a real-time source and the instrumented DSP thread falls behind the clock (sink underruns) so tone share fails with no sanitizer report"
        "app_selftest|it measures the CW sidetone of the built-in generator through the real pipeline and the instrumented DSP thread falls behind the clock (ring overflow drops samples) so the tone reads 773 Hz instead of 700 with no sanitizer report"
        "test_patch_audio|its pipeline cases measure tone levels against a real-time source and the instrumented DSP thread falls behind the clock (ring overflow drops samples, the patch's own ring starves) so levels fail with no sanitizer report"
        "test_pipeline_audio_lead|it reads the drift matcher's correction while a pipeline plays to the real output, and the instrumented DSP thread falls behind the clock (sink underruns un-prime the ring, where the matcher holds) so the correction is never seen to move, with no sanitizer report"
        "test_plugin_audio|its takeover case measures the length of a changeover in the real pipeline's audio against a real-time source and the instrumented DSP thread falls behind the clock: the fade read 68 frames where it is 240 with no sanitizer report (1896 of its 1897 checks passed, so this one is a loss of coverage worth revisiting)")
    set(CASCADE_SANITIZE_EXCLUDED_TESTS)
    foreach(_entry ${_cascade_san_excluded_master})
        string(REPLACE "|" ";" _parts "${_entry}")
        list(GET _parts 0 _n)
        if(TEST ${_n})
            list(APPEND CASCADE_SANITIZE_EXCLUDED_TESTS "${_entry}")
        endif()
    endforeach()

    # TESTS THAT FAULT A CHILD PROCESS ON PURPOSE, to prove the crash handler
    # writes its report. They run under AddressSanitizer with ASan's OWN handlers
    # for access violations, aborts, illegal instructions and arithmetic faults
    # switched off: with them on, the sanitizer's handler runs first, prints its
    # report and ends the process (exit 1), and the product's handler - the thing
    # under test - never sees the fault. Memory-error CHECKS (heap, stack, use
    # after free) are instrumentation, not handlers, and stay on.
    set(CASCADE_SANITIZE_FAULT_TESTS)
    foreach(_n test_crash_capture test_crash_late_module test_soapy_enum_proc)
        if(TEST ${_n})
            list(APPEND CASCADE_SANITIZE_FAULT_TESTS ${_n})
        endif()
    endforeach()
    set(_cascade_fault_env ${CASCADE_SANITIZE_TEST_ENV})
    list(TRANSFORM _cascade_fault_env REPLACE "^ASAN_OPTIONS=(.*)$"
         "ASAN_OPTIONS=\\1:handle_segv=0:handle_abort=0:handle_sigill=0:handle_sigfpe=0")

    if(CASCADE_SANITIZE_THREAD)
        # THREADSANITIZER RUNS A SELECTION, NOT THE SUITE. Only a test that puts
        # two of our threads against each other can find a race, so the job builds
        # and runs those (label `tsan`, target cascade_tsan_tests) and every other
        # test is disabled and labelled `sanitize-excluded`, with its reason, as
        # above. The lists and the judgement behind them are in tests/tsan.cmake.
        include("${CMAKE_CURRENT_SOURCE_DIR}/tsan.cmake")
        cascade_tsan_select()
    endif()

    foreach(t ${CASCADE_ALL_TEST_NAMES})
        # The per-test limits were chosen for a plain build.
        get_test_property(${t} TIMEOUT _limit)
        if(_limit)
            math(EXPR _limit "${_limit} * ${CASCADE_SANITIZE_TIMEOUT_SCALE}")
            set_tests_properties(${t} PROPERTIES TIMEOUT ${_limit})
        endif()
        # Runtime options travel with the test, not with whoever runs ctest.
        if(t IN_LIST CASCADE_SANITIZE_FAULT_TESTS)
            set_tests_properties(${t} PROPERTIES ENVIRONMENT "${_cascade_fault_env}")
        elseif(t MATCHES "_free$")
            # a window test against the free-running pass (above): the sanitizer's options and the seam's mode
            set_tests_properties(${t} PROPERTIES ENVIRONMENT "${CASCADE_SANITIZE_TEST_ENV};FOXSDR_TEST_PASS=free")
        else()
            set_tests_properties(${t} PROPERTIES ENVIRONMENT "${CASCADE_SANITIZE_TEST_ENV}")
        endif()
    endforeach()

    foreach(entry ${CASCADE_SANITIZE_EXCLUDED_TESTS})
        string(REPLACE "|" ";" entry "${entry}")
        list(GET entry 0 t)
        list(GET entry 1 why)
        set_tests_properties(${t} PROPERTIES DISABLED TRUE LABELS "sanitize-excluded")
        message(STATUS "sanitizer: ${t} is excluded - ${why}")
    endforeach()
endif()
