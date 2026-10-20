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
endforeach()

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
