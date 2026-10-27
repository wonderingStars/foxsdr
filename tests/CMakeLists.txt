# One executable per test_*.cpp, discovered by glob. Agents add test files
# only; this file itself almost never needs editing for a new test — the one
# exception is a test that needs a command-line argument, handled below by
# name for test_json_dump_policy.
file(GLOB CASCADE_TESTS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/test_*.cpp")
# THE 0.99.84 MERGE'S PENDING TESTS: master's tests of window features not yet re-expressed are left out of the build and
# replaced by red placeholders (tests/merge_pending.cmake).
include("${CMAKE_CURRENT_SOURCE_DIR}/merge_pending.cmake")

# A REAL plugin module for test_plugin_settings_ui (CASCADE_CAP_SETTINGS_UI):
# built as a MODULE exactly as a third-party plugin is - plugin_abi.h only,
# never linked against cascade_lib - into a directory of its own, so the test
# can hand that directory to PluginHost::scan and nothing else is found there.
add_library(settings_ui_fixture MODULE "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/settings_ui_plugin.cpp")
target_include_directories(settings_ui_fixture PRIVATE "${CMAKE_SOURCE_DIR}/src/core")
set_target_properties(settings_ui_fixture PROPERTIES
    PREFIX ""
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/fixture_plugins/$<CONFIG>"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/fixture_plugins/$<CONFIG>")

# TWO REAL plugin modules for test_plugin_rescan_skip, from one source
# (tests/fixtures/rescan_probe_plugin.cpp): each writes a line to a file when
# the host maps it, unmaps it, creates a decoder and destroys one, so the test
# can prove that a skipped rescan tore nothing down and a full one tore it all
# down. Two, with different declared names, so one is never an old copy of the
# other. Output file names are the targets' own ("rescan_probe_a", "..._b").
foreach(probe a b)
    add_library(rescan_probe_${probe} MODULE
        "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/rescan_probe_plugin.cpp")
    target_include_directories(rescan_probe_${probe} PRIVATE "${CMAKE_SOURCE_DIR}/src/core")
    target_compile_definitions(rescan_probe_${probe} PRIVATE PROBE_ID="${probe}")
    set_target_properties(rescan_probe_${probe} PROPERTIES
        PREFIX ""
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/fixture_rescan_${probe}/$<CONFIG>"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/fixture_rescan_${probe}/$<CONFIG>")
endforeach()

# THE NEXT RELEASE OF PROBE "a" (0.99.72), for test_plugin_update_cleanup_app: the same
# source and the same declared name as rescan_probe_a, but version 1.1.0. An update from the
# one to the other is "a plugin updated", and the older file is what the update must take with
# it - with the same declared name the host takes the older for an old copy of the newer.
add_library(rescan_probe_a_newer MODULE
    "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/rescan_probe_plugin.cpp")
target_include_directories(rescan_probe_a_newer PRIVATE "${CMAKE_SOURCE_DIR}/src/core")
target_compile_definitions(rescan_probe_a_newer PRIVATE PROBE_ID="a" PROBE_VERSION="1.1.0")
set_target_properties(rescan_probe_a_newer PROPERTIES
    PREFIX ""
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/fixture_rescan_a_newer/$<CONFIG>"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/fixture_rescan_a_newer/$<CONFIG>")

# A REAL PLUGIN MODULE WHOSE OUTPUT IS A PICTURE, for test_decoder_window_app: an image decoder
# (CASCADE_CAP_IMAGE_DECODER) that never offers a picture, so the host opens the plugin's image
# window and that window says it is waiting. Its own directory, like the probes above.
add_library(image_probe_fixture MODULE "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/image_probe_plugin.cpp")
target_include_directories(image_probe_fixture PRIVATE "${CMAKE_SOURCE_DIR}/src/core")
set_target_properties(image_probe_fixture PROPERTIES
    PREFIX ""
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/fixture_image/$<CONFIG>"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/fixture_image/$<CONFIG>")

# A REAL PLUGIN MODULE WITH A PRESET, for test_plugin_inuse_app (0.99.73): a text decoder with one
# preset, so that the two gestures that tune a plugin - pressing the preset (which pins the plugin)
# and merely opening its window (which must not) - both have something to apply. Its own directory.
add_library(preset_probe_fixture MODULE "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/preset_probe_plugin.cpp")
target_include_directories(preset_probe_fixture PRIVATE "${CMAKE_SOURCE_DIR}/src/core")
set_target_properties(preset_probe_fixture PROPERTIES
    PREFIX ""
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/fixture_preset/$<CONFIG>"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/fixture_preset/$<CONFIG>")

# A REAL PLUGIN MODULE THAT TRANSMITS (0.99.74, CASCADE_CAP_TX_AUDIO), for test_tx_audio_app: a
# decoder that asks for the transmitter whenever its settings say so and hands over a 1 kHz tone
# when it is keyed, and counts every call the host makes to it. Its own directory, like the
# others, so nothing else is found there.
add_library(tx_audio_fixture MODULE "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/tx_audio_plugin.cpp")
target_include_directories(tx_audio_fixture PRIVATE "${CMAKE_SOURCE_DIR}/src/core")
set_target_properties(tx_audio_fixture PROPERTIES
    PREFIX ""
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/fixture_txaudio/$<CONFIG>"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/fixture_txaudio/$<CONFIG>")

# A real I/Q decoder with decoded PCM, for the automatic demod-mute regression.
add_library(programme_audio_fixture MODULE "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/programme_audio_plugin.cpp")
target_include_directories(programme_audio_fixture PRIVATE "${CMAKE_SOURCE_DIR}/src/core")
set_target_properties(programme_audio_fixture PROPERTIES
    PREFIX ""
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/fixture_programme_audio/$<CONFIG>"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/fixture_programme_audio/$<CONFIG>")

# A REAL PLUGIN MODULE THAT IS A TRACK SOURCE (0.99.75), for test_unplaced_list_app: an aircraft
# source that keeps the ADS-B plugin 1.9.0's contract - it asks the host's get_state for
# CASCADE_STATE_LISTS_UNPLACED_TRACKS and publishes aircraft it has heard without a position
# (CASCADE_TRACK_FLAG_NO_POSITION) only when the host says it understands. Its own directory.
add_library(track_probe_fixture MODULE "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/track_probe_plugin.cpp")
target_include_directories(track_probe_fixture PRIVATE "${CMAKE_SOURCE_DIR}/src/core")
set_target_properties(track_probe_fixture PROPERTIES
    PREFIX ""
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/fixture_track/$<CONFIG>"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/fixture_track/$<CONFIG>")

# A REAL PLUGIN MODULE WHOSE PANEL ROWS ARE KEYS (0.99.77), for test_row_keys_app: a programme
# chooser that marks its rows CASCADE_ROW_FLAG_KEY only when get_state says the host draws them
# (CASCADE_STATE_ROW_KEYS), registers its commands with add_command, and writes every press it is
# handed by poll_command to a file. Its own directory.
add_library(row_keys_fixture MODULE "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/row_keys_plugin.cpp")
target_include_directories(row_keys_fixture PRIVATE "${CMAKE_SOURCE_DIR}/src/core")
set_target_properties(row_keys_fixture PROPERTIES
    PREFIX ""
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/fixture_rowkeys/$<CONFIG>"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/fixture_rowkeys/$<CONFIG>")

# A REAL PLUGIN MODULE THAT DECLARES THE WRONG ABI, for test_health_paths: the
# plugin host loads it as any plugin and refuses it, which is the "abi" word of
# the anonymous failure count (core/health_events.hpp). Its own directory.
add_library(abi_mismatch_fixture MODULE "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/abi_mismatch_plugin.cpp")
target_include_directories(abi_mismatch_fixture PRIVATE "${CMAKE_SOURCE_DIR}/src/core")
set_target_properties(abi_mismatch_fixture PROPERTIES
    PREFIX ""
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/fixture_abi/$<CONFIG>"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/fixture_abi/$<CONFIG>")
# ...and the same source as the plugin a host that is NOT the one it was built for
# meets in the field: its query declines (returns null) for an ABI it does not know.
add_library(abi_decline_fixture MODULE "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/abi_mismatch_plugin.cpp")
target_include_directories(abi_decline_fixture PRIVATE "${CMAKE_SOURCE_DIR}/src/core")
target_compile_definitions(abi_decline_fixture PRIVATE ABI_FIXTURE_DECLINES)
set_target_properties(abi_decline_fixture PROPERTIES
    PREFIX ""
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/fixture_abi_decline/$<CONFIG>"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/fixture_abi_decline/$<CONFIG>")

# A REAL SoapySDR vendor module for test_soapy_enum_proc: put on
# SOAPY_SDR_PLUGIN_PATH and loaded by the real cascade.exe enumeration child,
# where it faults as it is detached at exit (the 2026-10-01 field fault) or in
# its find function. Its own directory, so nothing else is found there.
# Windows only, as the test blocks that use it are.
if(WIN32 AND TARGET SoapySDR)
    add_library(soapy_fault_fixture MODULE "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/soapy_fault_module.cpp")
    target_link_libraries(soapy_fault_fixture PRIVATE SoapySDR)
    set_target_properties(soapy_fault_fixture PROPERTIES
        PREFIX ""
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/fixture_soapy/$<CONFIG>"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/fixture_soapy/$<CONFIG>")
    # The same source registering "sdrplay", with SoapySDRPlay3's API-session
    # singleton in miniature: whether the child closes that session before it
    # ends (endEnumerateHelperProcess) is read off what it records. Stages
    # nothing, so it never faults; same directory, so one plugin path holds both.
    add_library(soapy_sdrplay_fixture MODULE "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/soapy_fault_module.cpp")
    target_link_libraries(soapy_sdrplay_fixture PRIVATE SoapySDR)
    target_compile_definitions(soapy_sdrplay_fixture PRIVATE
        FIXTURE_DRIVER="sdrplay" FIXTURE_LABEL="SDRplay fixture" FIXTURE_STAGES=0)
    set_target_properties(soapy_sdrplay_fixture PROPERTIES
        PREFIX ""
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/fixture_soapy/$<CONFIG>"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/fixture_soapy/$<CONFIG>")
    # A SECOND FAULTING DRIVER, in a module of another name and a directory of
    # its own: the same source registering "faultfixtureb". Two drivers that
    # fault on one machine fault in two different modules, so their reports
    # must hash to two different signatures - the check that one fault's
    # reports collapse into one upload and two faults' reports do not needs a
    # second module that really faults. A directory of its own, so the tests
    # that want ONE faulting driver put only the first on the plugin path.
    add_library(soapy_fault_fixture_b MODULE "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/soapy_fault_module.cpp")
    target_link_libraries(soapy_fault_fixture_b PRIVATE SoapySDR)
    target_compile_definitions(soapy_fault_fixture_b PRIVATE
        FIXTURE_DRIVER="faultfixtureb" FIXTURE_LABEL="Fault fixture B")
    set_target_properties(soapy_fault_fixture_b PROPERTIES
        PREFIX ""
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/fixture_soapy_b/$<CONFIG>"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/fixture_soapy_b/$<CONFIG>")
endif()

# A DLL that is mapped only when something loads it and faults when called
# (tests/fixtures/late_fault_dll.cpp): for the tests that prove the crash
# handler names code mapped AFTER its module table was last refreshed. Its own
# directory, and never on SOAPY_SDR_PLUGIN_PATH or the test's search path -
# either would map it up front and test nothing. Windows only, as they are.
if(WIN32)
    add_library(late_fault_fixture MODULE "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/late_fault_dll.cpp")
    set_target_properties(late_fault_fixture PROPERTIES
        PREFIX ""
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/fixture_late/$<CONFIG>"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/fixture_late/$<CONFIG>")
endif()

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
    # NO DWARF IN A TEST EXECUTABLE (Linux). The Release flags carry -g so that
    # the application's own link has line tables to archive (see CMakeLists.txt),
    # and this tree builds 265 test programs on the same flags. Measured on
    # GCC: one translation unit that includes nlohmann::json went from 0.4 MB to
    # 6.4 MB of object with -g, and every test links most of cascade_lib - so
    # without this the CI runner would be asked to hold hundreds of full-DWARF
    # executables nobody will ever read. -g0 negates -g for the test's own
    # source (target options come after the configuration flags on the command
    # line), and --strip-debug drops what the linked-in library objects carry.
    # The function symbol table is untouched, so a crash test still names code.
    #
    # Not in a sanitizer build (CASCADE_SANITIZE, cmake/sanitize.cmake): that one
    # compiles with -g1 so a report names file and line, and stripping would
    # undo it.
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT CASCADE_SANITIZE_ENABLED)
        target_compile_options(${name} PRIVATE -g0)
        target_link_options(${name} PRIVATE -Wl,--strip-debug)
    endif()
    # The AddressSanitizer runtime DLL beside the test (MSVC sanitizer build
    # only), for the reason SoapySDR.dll is staged below.
    if(CASCADE_SANITIZE_ENABLED)
        cascade_sanitize_stage_runtime(${name})
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
    elseif(name STREQUAL "test_tester_usage" OR name STREQUAL "test_tester_link" OR name STREQUAL "test_tester_host_only")
        # Scans src/gui/app_window.cpp and src/gui/app_window_tester.cpp, src/core/tester_usage*.cpp,
        # src/core/tester_link*.cpp, src/engine/process_boot.cpp and src/main.cpp for the
        # token-never-logged property (test_tester_host_only: the engine's and the servers' sources
        # for the means of sending a tester report), so all of them need the repository root too.
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
    elseif(name STREQUAL "test_plugin_settings_ui")
        # Loads the fixture module above through the real loader, so it takes
        # the module's directory; built first.
        add_dependencies(${name} settings_ui_fixture)
        add_test(NAME ${name} COMMAND ${name} "$<TARGET_FILE_DIR:settings_ui_fixture>")
    elseif(name STREQUAL "test_health_paths")
        # Loads the wrong-ABI fixture module through the real plugin host; built first.
        add_dependencies(${name} abi_mismatch_fixture abi_decline_fixture)
        add_test(NAME ${name} COMMAND ${name} "$<TARGET_FILE:abi_mismatch_fixture>"
                 "$<TARGET_FILE:abi_decline_fixture>")
    elseif(name STREQUAL "test_engine_api_txplugin")
        # A plugin may key the transmitter (stub campaign B13): the real engine, the transmitting fixture fitted in a scratch plugins folder, and a ledger radio -
        # no window and no real transmitter anywhere; built first.
        add_dependencies(${name} tx_audio_fixture)
        add_test(NAME ${name} COMMAND ${name} "$<TARGET_FILE:tx_audio_fixture>")
    elseif(name STREQUAL "test_tx_audio_app")
        # A real AppWindow with the transmitting fixture fitted in a scratch plugins folder, a
        # fake transmit radio and the real runner, driven through the page's own paths; built
        # first.
        add_dependencies(${name} tx_audio_fixture)
        # (the _stepped and _free variants the pass seam adds are handed the same fixture module)
        set(CASCADE_TEST_ARGS_${name} "$<TARGET_FILE:tx_audio_fixture>")
        add_test(NAME ${name} COMMAND ${name} ${CASCADE_TEST_ARGS_${name}})
    elseif(name STREQUAL "test_tx_audio_plate_app")
        # Runs the real cascade.exe (built first) with the transmitting fixture fitted and pinned to run,
        # opens its page in Fitted modules and presses GRANT TRANSMIT with the input script.
        add_dependencies(${name} tx_audio_fixture cascade)
        add_test(NAME ${name} COMMAND ${name} "$<TARGET_FILE:tx_audio_fixture>")
    elseif(name STREQUAL "test_tx_audio_page_app")
        # Runs the real cascade.exe (built first) with the TRANSMIT page opened by the bounded-run seam
        # and presses the plugin controls on it with the input script.
        add_dependencies(${name} cascade)
        add_test(NAME ${name} COMMAND ${name})
    elseif(name STREQUAL "test_display_menu_app")
        # Runs the real cascade.exe (built first) and right-clicks the spectrum, the waterfall and a
        # patch Spectrum part with the input script (0.99.76): the display range menu, the strength
        # key's MANUAL, and the tip that appears after four seconds of rest.
        add_dependencies(${name} cascade)
        add_test(NAME ${name} COMMAND ${name})
    elseif(name STREQUAL "test_passband_slide_app")
        # Runs the real cascade.exe (built first) on the signal generator and slides the passband with
        # Shift + wheel on the spectrum and the waterfall, using the input script (0.99.82): the step,
        # the carry past the end of the span, the picture following, the plain wheel's zoom, the truce
        # with a drag, and what a double-click does.
        add_dependencies(${name} cascade)
        add_test(NAME ${name} COMMAND ${name})
    elseif(name STREQUAL "test_window_platform_app")
        # Runs the real cascade.exe (built first) in bounded runs and reads the diagnostics log it
        # leaves for the `window:` lines (0.99.78): their order before the first frame, the value of
        # DISPLAY that is not in them, and FOXSDR_WINDOW_PLATFORM ignored when it names no platform.
        add_dependencies(${name} cascade)
        add_test(NAME ${name} COMMAND ${name})
    elseif(name STREQUAL "test_window_failed_app")
        # Runs the real cascade.exe (built first) with its window refused on purpose
        # (FOXSDR_TEST_WINDOW_FAIL) and the sentinel on, and reads the report the watcher writes
        # for it (field signature 85FF854AA2C772F9), including the hand-over to a second process on
        # the software renderer that is refused as well (0.99.90). No window is ever shown.
        add_dependencies(${name} cascade)
        add_test(NAME ${name} COMMAND ${name})
    elseif(name STREQUAL "test_software_gl_app")
        # Runs the real cascade.exe (built first, with softgl\ staged beside it) with the graphics
        # driver refused or taken to be too old, and with FOXSDR_GL=software, and reads the log both
        # processes leave (0.99.90): the hand-over to the software renderer, ending in a window.
        add_dependencies(${name} cascade)
        add_test(NAME ${name} COMMAND ${name})
    elseif(name STREQUAL "test_plugin_rescan_skip")
        # Copies the two probe modules into a plugins folder of its own and
        # drives the real AppWindow's rescan against it; built first.
        add_dependencies(${name} rescan_probe_a rescan_probe_b)
        # (the _stepped variant the pass seam adds is handed the same two modules: CASCADE_TEST_ARGS_<name>)
        set(CASCADE_TEST_ARGS_${name} "$<TARGET_FILE:rescan_probe_a>" "$<TARGET_FILE:rescan_probe_b>")
        add_test(NAME ${name} COMMAND ${name} ${CASCADE_TEST_ARGS_${name}})
    elseif(name STREQUAL "test_plugin_update_cleanup_app")
        # A real AppWindow updating a real plugin module (probe a 1.0.0 -> 1.1.0) beside an
        # unrelated one (probe b); all three built first, in that order on the command line.
        add_dependencies(${name} rescan_probe_a rescan_probe_a_newer rescan_probe_b)
        # (the _stepped variant the pass seam adds - the test is the engine's since stub batch B3 - is handed the same three modules)
        set(CASCADE_TEST_ARGS_${name} "$<TARGET_FILE:rescan_probe_a>" "$<TARGET_FILE:rescan_probe_a_newer>" "$<TARGET_FILE:rescan_probe_b>")
        add_test(NAME ${name} COMMAND ${name} ${CASCADE_TEST_ARGS_${name}})
    elseif(name STREQUAL "test_plugin_host_origin")
        # A plugin site's module never takes a declared name from an official one (0.99.74): the
        # probe module built twice (1.0.0 and 1.1.0, one declared name), scanned by the real host and
        # the real AppWindow with install records saying which is whose; both built first.
        add_dependencies(${name} rescan_probe_a rescan_probe_a_newer)
        set(CASCADE_TEST_ARGS_${name} "$<TARGET_FILE:rescan_probe_a>" "$<TARGET_FILE:rescan_probe_a_newer>")
        add_test(NAME ${name} COMMAND ${name} ${CASCADE_TEST_ARGS_${name}})
    elseif(name STREQUAL "test_plugin_dup_app")
        # Runs the real cascade.exe (built first) with two files of ONE plugin whose declared name
        # differs - probe b 1.0.0 and probe a 1.1.0, copied under file names that share a run id - in a
        # scratch plugins folder (0.99.77): one section in the DECODE bank, the older file REFUSED in
        # Fitted modules, the clean-up counting it, and no Dear ImGui ID-conflict report.
        add_dependencies(${name} rescan_probe_b rescan_probe_a_newer cascade)
        add_test(NAME ${name} COMMAND ${name} "$<TARGET_FILE:rescan_probe_b>"
                 "$<TARGET_FILE:rescan_probe_a_newer>")
    elseif(name STREQUAL "test_slow_frames_app")
        # Runs the real cascade.exe (built first) and, in process, a real plugin
        # reload against one of the probe modules above.
        add_dependencies(${name} rescan_probe_a cascade)
        # (named for the variants too: it includes tests/pass_seam.hpp since stub batch B18, so it is registered again as _stepped, which runs the in-process half only)
        set(CASCADE_TEST_ARGS_${name} "$<TARGET_FILE:rescan_probe_a>")
        add_test(NAME ${name} COMMAND ${name} ${CASCADE_TEST_ARGS_${name}})
    elseif(name STREQUAL "test_plugin_store_app")
        # Runs the real cascade.exe (built first) against a local catalogue of three fake
        # plugins, one of them installed as a real module: the probe fixture above, copied
        # into a scratch plugins folder as store-c 1.0.0.
        add_dependencies(${name} rescan_probe_a cascade)
        add_test(NAME ${name} COMMAND ${name} "$<TARGET_FILE:rescan_probe_a>")
    elseif(name STREQUAL "test_decoder_window_app")
        # Runs the real cascade.exe (built first) in one viewport, presses the probe decoder's
        # preset (the probe module above, copied into a scratch plugins folder) and reads where
        # the shared Decoder output window opened from the UI census; and, with the image
        # fixture beside it, where a plugin's picture window opens.
        add_dependencies(${name} rescan_probe_a image_probe_fixture cascade)
        add_test(NAME ${name} COMMAND ${name} "$<TARGET_FILE:rescan_probe_a>"
                 "$<TARGET_FILE:image_probe_fixture>")
    elseif(name STREQUAL "test_decoder_settings_fold_app")
        # Runs the real cascade.exe (built first) with the settings fixture fitted and the DECODE
        # bank up, and reads from the UI census whether the plugin's settings row is folded (0.99.74).
        add_dependencies(${name} settings_ui_fixture cascade)
        add_test(NAME ${name} COMMAND ${name} "$<TARGET_FILE:settings_ui_fixture>")
    elseif(name STREQUAL "test_unplaced_list_app")
        # Runs the real cascade.exe (built first) with the track fixture fitted and its map page opened
        # by FOXSDR_OPEN_MAP, and reads the target list's heading and rows from the UI census: aircraft
        # heard without a position are listed after the placed ones and never drawn (0.99.75).
        add_dependencies(${name} track_probe_fixture cascade)
        add_test(NAME ${name} COMMAND ${name} "$<TARGET_FILE:track_probe_fixture>")
    elseif(name STREQUAL "test_map_coverage_app")
        # Runs the real cascade.exe (built first) with the track fixture fitted (TRACK_PROBE_COVERAGE=1: two aircraft in line of
        # sight, one 2,740 km away, a satellite 115 km away) and its map page opened by FOXSDR_OPEN_MAP, and reads the coverage
        # line's numbers from the UI census: only what a receiver could hear is counted (0.99.58; stub campaign B16).
        add_dependencies(${name} track_probe_fixture cascade)
        add_test(NAME ${name} COMMAND ${name} "$<TARGET_FILE:track_probe_fixture>")
    elseif(name STREQUAL "test_plugin_settings_window")
        # The REAL window and the REAL engine with the settings fixture fitted (stub campaign B9): the form's wiring frame by frame in drawUi's order.
        # (the _stepped variant the pass seam adds is handed the same fixture module)
        add_dependencies(${name} settings_ui_fixture)
        set(CASCADE_TEST_ARGS_${name} "$<TARGET_FILE:settings_ui_fixture>")
        add_test(NAME ${name} COMMAND ${name} ${CASCADE_TEST_ARGS_${name}})
    elseif(name STREQUAL "test_programme_audio_app")
        add_dependencies(${name} programme_audio_fixture)
        # (the _stepped variant the pass seam adds - the test is the engine's since stub batch B9 - is handed the same fixture module)
        set(CASCADE_TEST_ARGS_${name} "$<TARGET_FILE:programme_audio_fixture>")
        add_test(NAME ${name} COMMAND ${name} ${CASCADE_TEST_ARGS_${name}})
    elseif(name STREQUAL "test_row_keys_app")
        # Runs the real cascade.exe (built first) with the row-keys fixture fitted and pinned to run,
        # opens its panel from the DECODE bank's row, presses its rows with the input script and reads
        # from the UI census and from the fixture's own log that a pressable row (0.99.77) is listed,
        # that a press reaches the plugin's poll_command with the id, and that sixteen commands are
        # sixteen keys on its page.
        add_dependencies(${name} row_keys_fixture cascade)
        add_test(NAME ${name} COMMAND ${name} "$<TARGET_FILE:row_keys_fixture>")
    elseif(name STREQUAL "test_plugin_inuse_app")
        # Runs the real cascade.exe (built first) with the same two modules fitted, and the preset
        # probe beside them, and reads from the modules' own event logs WHEN the host creates and
        # destroys their decoders (0.99.73: a plugin runs only while it is used).
        add_dependencies(${name} rescan_probe_a image_probe_fixture preset_probe_fixture cascade)
        add_test(NAME ${name} COMMAND ${name} "$<TARGET_FILE:rescan_probe_a>"
                 "$<TARGET_FILE:image_probe_fixture>" "$<TARGET_FILE:preset_probe_fixture>")
    elseif(name STREQUAL "test_plugin_pathpick_app")
        # Runs the real cascade.exe (built first) with the settings fixture module fitted, presses
        # the BROWSE key its PATH field draws (0.99.74) with the file dialog's answer given by the
        # FOXSDR_FILE_PICK seam, and reads the chosen path back from the config the run leaves.
        add_dependencies(${name} settings_ui_fixture cascade)
        add_test(NAME ${name} COMMAND ${name} "$<TARGET_FILE:settings_ui_fixture>")
    elseif(name STREQUAL "test_import_dialog_app")
        # Runs the real cascade.exe (built first) with TOOLS > Bookmarks open and a name in its
        # import box (FOXSDR_BOOKMARK_IMPORT), presses Import and the window's keys with the input
        # script, and reads the window's parts from the UI census, whether it is still up from the
        # screenshot's window list, and the rows from the bookmarks file the run leaves (0.99.80).
        add_dependencies(${name} cascade)
        add_test(NAME ${name} COMMAND ${name})
    elseif(name STREQUAL "test_soapy_enum_proc" AND TARGET soapy_fault_fixture)
        # Hands the fault fixture's directory to the real enumeration child;
        # cascade.exe is built first because that block runs it.
        add_dependencies(${name} soapy_fault_fixture soapy_fault_fixture_b soapy_sdrplay_fixture
                         late_fault_fixture cascade)
        # The second argument is the late-mapped DLL's full path, handed to the
        # fault fixture through the environment (its "late" stage). The third
        # is the second faulting driver's directory.
        add_test(NAME ${name} COMMAND ${name} "$<TARGET_FILE_DIR:soapy_fault_fixture>"
                 "$<TARGET_FILE:late_fault_fixture>" "$<TARGET_FILE_DIR:soapy_fault_fixture_b>")
    elseif(name STREQUAL "test_foreign_modules" AND TARGET late_fault_fixture)
        # The arrival test copies this DLL into a scratch folder - outside the test's own
        # folder and outside Windows' - and loads it for real; built first. The DLL is
        # mapped only when something loads it and does nothing when it is, which is all
        # a stand-in for another program's injected overlay has to be.
        add_dependencies(${name} late_fault_fixture)
        add_test(NAME ${name} COMMAND ${name} "$<TARGET_FILE:late_fault_fixture>")
    elseif(name STREQUAL "test_foreign_modules_app" AND TARGET late_fault_fixture)
        # The same DLL, mapped by the REAL cascade.exe on a frame of its own
        # (CASCADE_FOREIGN_MODULE_TEST) so the arrival reaches the real log through the
        # real frame loop; both built first.
        add_dependencies(${name} late_fault_fixture cascade)
        add_test(NAME ${name} COMMAND ${name} "$<TARGET_FILE:late_fault_fixture>")
    elseif(name STREQUAL "test_crash_late_module" AND TARGET late_fault_fixture)
        # Loads the late fixture by full path after arming its handlers.
        add_dependencies(${name} late_fault_fixture)
        add_test(NAME ${name} COMMAND ${name} "$<TARGET_FILE:late_fault_fixture>")
    else()
        add_test(NAME ${name} COMMAND ${name})
    endif()
    # A liveness bug (stuck spin loop, deadlock) must fail the suite, not hang
    # it forever — ctest's default per-test timeout is 1500 s.
    set_tests_properties(${name} PROPERTIES TIMEOUT 120)
    # For the sanitizer block at the end of this file: every test this loop
    # registers, by name.
    list(APPEND CASCADE_ALL_TEST_NAMES ${name})
endforeach()

# The pending tests' red placeholders (tests/merge_pending.cmake).
cascade_merge_pending_register()

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
        test_snapshot_app test_soundcard_app_paths test_source_section_window test_tuning_surface
        # stub batch B13 (SAFETY): the plugin's key on the pass against the page's session, the TX thread and the DSP thread - inline 3, stepped 3, free 5 green
        test_tx_audio_app)
    foreach(src ${CASCADE_TESTS})
        get_filename_component(_name "${src}" NAME_WE)
        file(STRINGS "${src}" _seam REGEX "^[ \t]*#[ \t]*include[ \t]*\"pass_seam[.]hpp\"")
        if(_seam AND TARGET ${_name})
            # THE VARIANTS RUN THE TEST'S OWN COMMAND, arguments included: a test that is handed the probe modules it drives (the plugin
            # tests) names them in CASCADE_TEST_ARGS_<name> where it is registered, and every variant is handed them too.
            set(_base_cmd ${_name} ${CASCADE_TEST_ARGS_${_name}})
            if(NOT CASCADE_SANITIZE_ENABLED)
                add_test(NAME ${_name}_stepped COMMAND ${_base_cmd})
                set_tests_properties(${_name}_stepped PROPERTIES ENVIRONMENT "FOXSDR_TEST_PASS=stepped" TIMEOUT 120)
            endif()
            if(${_name} IN_LIST CASCADE_FREE_PASS_TESTS)
                add_test(NAME ${_name}_free COMMAND ${_base_cmd})
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
                           test_gps_app test_problem_report_app test_shutdown_budget test_startup_state
                           test_stop_ends_recordings test_theme_census test_tester_usage_app test_ui_scale_app test_soapy_enum_proc
                           test_soapy_source)
    if(TARGET ${_runs_app})
        add_dependencies(${_runs_app} cascade)
    endif()
endforeach()

# THE foxsdr-engine TESTS (stages 7b, 7c, 7d and 7e) spawn the engine executable - and the window application where they compare
# against it (--version, --selftest, the refused --seconds) - so both are built first, or an end-to-end half would
# drive whatever stale binary the build tree last held. They link the engine alone (no window include in them).
foreach(_t IN ITEMS test_engine_main_version test_engine_main_args test_engine_main_helpers test_engine_main_smoke
                    test_engine_main_links_headless test_engine_main_config_roundtrip test_engine_main_clean_exit_marker
                    test_engine_main_damaged_config test_engine_main_second_instance_refused
                    test_engine_main_sigterm test_engine_main_sigterm_wedged test_engine_main_provision
                    test_engine_main_password_not_in_argv test_tester_host_only)
    if(TARGET ${_t})
        add_dependencies(${_t} foxsdr-engine cascade)
    endif()
endforeach()
# STAGE 7g: what is beside the engine and what is kept of it (tests/test_engine_packaging.cpp). It reads the built
# executables, so both are built first; FOXSDR_ENGINE_BINDIR is where the build put foxsdr-engine, to be compared
# with CASCADE_APP_BINDIR (where cascade is): the staging is "beside both" only while they are one directory.
if(TARGET test_engine_packaging)
    add_dependencies(test_engine_packaging foxsdr-engine cascade)
    target_compile_definitions(test_engine_packaging PRIVATE
        FOXSDR_ENGINE_BINDIR="$<TARGET_FILE_DIR:foxsdr-engine>")
endif()
# ...and the configure-level proof of CASCADE_BUILD_INTERFACE=OFF (tests/engine_only_configure.cmake): a nested configure of
# this project, option OFF and OpenGL disabled, in a scratch directory, must succeed; the same with the option ON must fail
# (so the first is not vacuous); and the OFF tree's staging must land. No build of the engine happens inside it (CI's
# linux-engine-only job and the stage's record do that). Its generator, platform, toolchain and triplet are this build's.
add_test(NAME engine_only_configure
    COMMAND ${CMAKE_COMMAND}
            -DSOURCE_DIR=${CMAKE_SOURCE_DIR}
            -DSCRATCH=${CMAKE_CURRENT_BINARY_DIR}/engine_only_configure
            "-DGENERATOR=${CMAKE_GENERATOR}"
            "-DPLATFORM=${CMAKE_GENERATOR_PLATFORM}"
            "-DTOOLCHAIN=${CMAKE_TOOLCHAIN_FILE}"
            "-DTRIPLET=${VCPKG_TARGET_TRIPLET}"
            -DCONFIG=$<CONFIG>
            -P ${CMAKE_CURRENT_SOURCE_DIR}/engine_only_configure.cmake)
set_tests_properties(engine_only_configure PROPERTIES TIMEOUT 600)

# The Linux release tarball's read-back (tools/check-linux-tarball.sh, called by .github/workflows/build.yml) proves it can FAIL:
# its --self-test builds small archives of stand-in scripts and shows the check accepting a good one and refusing one without the
# engine, without cascade, without a band plan, without the firmware notice, of the wrong version, and one that does not exist.
# Run through a bash when there is one: Git's on Windows (named, NOT the first on PATH: that can be WSL's launcher), the system's
# elsewhere. No bash, no test: it says so.
if(WIN32)
    find_program(CASCADE_TEST_BASH NAMES bash HINTS "$ENV{ProgramFiles}/Git/bin" "$ENV{ProgramFiles}/Git/usr/bin" NO_DEFAULT_PATH)
else()
    find_program(CASCADE_TEST_BASH NAMES bash)
endif()
if(CASCADE_TEST_BASH)
    add_test(NAME release_tarball_check_selftest
        COMMAND ${CASCADE_TEST_BASH} ${CMAKE_SOURCE_DIR}/tools/check-linux-tarball.sh --self-test)
    set_tests_properties(release_tarball_check_selftest PROPERTIES TIMEOUT 120)
else()
    message(STATUS "no bash: release_tarball_check_selftest is not registered")
endif()

# STAGE 7d: the wedged-stop test runs the production budget once (a stop wedged for 120 s, ended at 53 s + the 5 s grace),
# on top of its seven-second cases, so the default 120 s is not enough; a bound that holds with room under a loaded machine.
if(TARGET test_engine_main_sigterm_wedged)
    set_tests_properties(test_engine_main_sigterm_wedged PROPERTIES TIMEOUT 360)
endif()

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
# THE SETTINGS FIXTURE THROUGH THE TABLE (stub campaign B9, API 0.3 9.7): test_engine_api_settings fits tests/fixtures/settings_ui_plugin.cpp into a plugin
# directory of its own (Engine::testHooks_.pluginDir) and holds PLUGIN_SETTINGS, its facts and FOXAPP_OP_PLUGIN_SETTING_EDIT against the real engine.
if(TARGET test_engine_api_settings)
    add_dependencies(test_engine_api_settings settings_ui_fixture)
    target_compile_definitions(test_engine_api_settings PRIVATE
        FIXTURE_SETTINGS_UI_PLUGIN="$<TARGET_FILE:settings_ui_fixture>")
endif()
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
foreach(_t IN ITEMS test_engine_api_03 test_engine_publish_alloc test_patch_graph_draft_input test_plugin_window test_plugin_status_report test_web_pictures)
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
foreach(_t IN ITEMS test_map_window test_plugin_window test_plugin_status_report test_engine_api_03 test_fitted_modules_window)
    if(TARGET ${_t})
        add_dependencies(${_t} fixture_group14_plugin)
        target_compile_definitions(${_t} PRIVATE
            FIXTURE_GROUP14_PLUGIN="$<TARGET_FILE:fixture_group14_plugin>")
    endif()
endforeach()
# THE WAKE PROBE (stub campaign B2, PLUGIN-RUN; tests/fixture_wake_probe_plugin.cpp): an audio TEXT decoder that says one line per block it is
# handed, naming the block by its first sample - the module that shows WHICH block a plugin woken by a window's signal decoded first
# (test_engine_api_03 group N). Copied into a plugin directory of the test's own.
add_library(fixture_wake_probe_plugin SHARED fixture_wake_probe_plugin.cpp)
target_include_directories(fixture_wake_probe_plugin PRIVATE "${CMAKE_SOURCE_DIR}/src")
if(TARGET test_engine_api_03)
    # test_engine_api_03.cpp (nine thousand lines, group N the last) is past the 65,535 sections an ordinary MSVC object file holds
    # ("error C1128"): /bigobj for that one file, as src/gui/app_window.cpp has it (CMakeLists.txt).
    if(MSVC)
        set_source_files_properties(test_engine_api_03.cpp PROPERTIES COMPILE_OPTIONS "/bigobj")
    endif()
    add_dependencies(test_engine_api_03 fixture_wake_probe_plugin)
    target_compile_definitions(test_engine_api_03 PRIVATE
        FIXTURE_WAKE_PROBE_PLUGIN="$<TARGET_FILE:fixture_wake_probe_plugin>")
endif()
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

# test_tester_link spawns the real cascade.exe with a foxsdr: link and needs
# it to be the only copy in its single-instance scope. It used to be
# RUN_SERIAL for that, which only kept this ctest's own app_* smoke tests away
# and did nothing about another checkout's suite or the owner's FoxSDR. Each
# spawn now runs in a private per-run scope (FOXSDR_INSTANCE_SCOPE, honoured
# only with CASCADE_CONFIG_TEST - see core/tester_link.hpp), so it runs in
# parallel like everything else.

# The app doubles as a smoke test. The PASS_REGULAR_EXPRESSION makes the
# exact-count half of the --frames contract observable: rendering N+1 frames
# (or none) fails even though the exit code would still be 0.
# The theme census starts the application sixteen times (eight layouts at two
# window sizes), so it gets more than the loop's two minutes - most of all on a
# software-rendered Linux runner.
if(TEST test_theme_census)
    set_tests_properties(test_theme_census PROPERTIES TIMEOUT 600)
endif()
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
# The disk-work tests (stub campaign B19) are made of fixed waits - a 2.5 s "slow disk" a dozen times over - and run 50 - 95 s on the owner's desk, so the
# inherited 120 s bound leaves a shared runner no room; a timeout is not what they are about.
set_tests_properties(test_gui_file_jobs test_failure_files PROPERTIES TIMEOUT 300)
# The team-message test starts the real application fourteen times, 70 frames each.
# CI run 38564730541: x64 passed in 61.98 s; arm64 hit the inherited 120 s
# bound at 120.11 s without stdout (Store test: 35.27 s x64, 69.55 s arm64).
# Give only this test 300 s; unbuffered launch tags identify a child if it hangs.
if(TEST test_team_message_app)
    set_tests_properties(test_team_message_app PROPERTIES TIMEOUT 300)
endif()
# The report-page test starts the real application 26 times, 110 frames each (stub campaign B14): the same bound.
if(TEST test_problem_report_app)
    set_tests_properties(test_problem_report_app PROPERTIES TIMEOUT 300)
endif()
# The beta-tester section test starts the real application about twenty times (stub campaign B15), a few of them holding the frame loop for real time while a worker
# claims a link and a loopback site answers: the same bound. test_tester_host_only starts the engine and the window once each against a stand-in site.
if(TEST test_tester_usage_app)
    set_tests_properties(test_tester_usage_app PROPERTIES TIMEOUT 300)
endif()
if(TEST test_tester_host_only)
    set_tests_properties(test_tester_host_only PROPERTIES TIMEOUT 300)
endif()
# The deck-knobs test starts the real application dozens of times (as a child of
# itself, so that a fake radio can sit behind the window; the noise-floor and Airband
# runs hold a few seconds of real time each): 46 s on this desktop before the Airband
# scenarios, 140 to 150 s with them, and 321 s (x64) and 335 s (arm64) on the GitHub
# Linux runners (CI run 38288445561), so it gets the same room the census does.
if(TEST test_deck_knobs_app)
    set_tests_properties(test_deck_knobs_app PROPERTIES TIMEOUT 600)
endif()
# The TRANSMIT-grant plate test starts the real cascade eight times (the Fitted modules
# window with a real plugin module pinned to run and the receiver running, 1013 frames
# in all): 19 to 37 s on this desktop (about 4 s of each run is start-up, shutdown and its
# sleeps). Both GitHub Linux runners were still in it when the loop's two minutes ended
# (CI run 38288445561, x64 and arm64), its siblings that start the real application
# (test_tx_audio_page_app 24 s there against 8 s here) run at 3 times this desktop's
# time, and the deck-knobs test shows frames of 50 ms or more there. It prints each run's
# start and duration unbuffered, so a run stopped by this bound still says which one was
# in hand.
if(TEST test_tx_audio_plate_app)   # (pending while TX-PLUGIN is: tests/merge_pending.cmake)
    set_tests_properties(test_tx_audio_plate_app PROPERTIES TIMEOUT 600)
endif()
# The exit-cause test starts the real application eleven times (a staged run per
# class and per case with nothing to read, then two real kills and the start that
# reads them) and leaves a second between each kill and the next start, because a
# report and a session start are stamped in whole seconds. 78 s on this desktop; the
# loop's two minutes is not enough on a loaded runner. Windows only (it SKIPs elsewhere).
if(TEST test_exit_cause_app)
    set_tests_properties(test_exit_cause_app PROPERTIES TIMEOUT 600)
endif()
# The radio-setup test starts the real application fifteen times (one per
# synthetic machine, the persisted tick, the key, CHECK AGAIN, and the real bus).
# 71 s on this desktop; the GitHub Linux runners run the application-launching
# tests at about half that speed (test_airband_app: 123 s here, 261 s there), so
# the loop's two minutes killed it on both Linux jobs of the 0.99.69 run.
if(TEST test_radio_setup_app)
    set_tests_properties(test_radio_setup_app PROPERTIES TIMEOUT 600)
endif()
# The airband test starts the application thirty-one times (a layout run and a
# scripted run per scenario: lookup, O'Hare's list listened to (0.99.71), listen,
# hand-back, the spectrum, the typed list, presets). Six runs took 68 s on the
# x64 runner and 101 s on arm64, so twenty-nine would not fit the loop's two
# minutes there (0.99.66: both Linux jobs timed out at 120 s). On this desktop
# it is 80 s.
if(TEST test_airband_app)
    set_tests_properties(test_airband_app PROPERTIES TIMEOUT 900)
endif()
# The update-cleanup test runs the real AppWindow's plugin rescan five times against real
# modules and waits on a worker for each transfer (a second or two here); the Linux runners
# are half this desktop's speed, so it is given its own bound instead of the loop's two minutes.
if(TEST test_plugin_update_cleanup_app)
    set_tests_properties(test_plugin_update_cleanup_app PROPERTIES TIMEOUT 300)
endif()
# The catalogue/pictures test builds the real AppWindow six times and waits on workers (a few
# hundred milliseconds each here); same reasoning, same bound.
if(TEST test_plugin_catalogue_app)
    set_tests_properties(test_plugin_catalogue_app PROPERTIES TIMEOUT 300)
endif()
# The store test starts the real application about a dozen times (a layout run and a scripted
# run per scenario: the Browse and Updates tabs, a page, the legal notice's tick, the window's
# remembered rectangle, the Fitted modules window and its page). Each is 120 to 200 frames.
# Linux runners are half this desktop's speed, so it is given the generous bound its siblings are.
if(TEST test_plugin_store_app)
    set_tests_properties(test_plugin_store_app PROPERTIES TIMEOUT 600)
endif()
# The Decoder output window test starts the real application nine times (the preset on three
# window sizes, the capture seam, two picture-window runs, and a layout, a drag and a reset run),
# 100 to 110 frames each.
# The same bound, for the same reason, as the store test.
if(TEST test_decoder_window_app)
    set_tests_properties(test_decoder_window_app PROPERTIES TIMEOUT 600)
endif()
# The settings-fold test starts the real application three times (folded as it opens, one click,
# two clicks), 40 to 60 frames each. The same bound, for the same reason, as the test above.
if(TEST test_decoder_settings_fold_app)
    set_tests_properties(test_decoder_settings_fold_app PROPERTIES TIMEOUT 600)
endif()
# The unplaced-list test starts the real application three times (the heard rows, a bare NaN, and
# "On screen only"), 80 frames each. The same bound, for the same reason, as the test above.
if(TEST test_unplaced_list_app)
    set_tests_properties(test_unplaced_list_app PROPERTIES TIMEOUT 600)
endif()
# The coverage test starts the real application twice (a receiver set, and none), 60 to 80 frames each. The same bound, for the same reason.
if(TEST test_map_coverage_app)
    set_tests_properties(test_map_coverage_app PROPERTIES TIMEOUT 300)
endif()
# The row-keys test starts the real application twelve times (a layout run, the window open, a
# press of HD2, of HD1 then HD2, the three that must press nothing and the control for the click's
# geometry, an instrument's memory rows opened and pressed, and the module's page with sixteen
# keys), 60 to 160 frames each with a half-second sleep in most: 28 s on this desktop. The same
# bound, for the same reason, as the tests above.
if(TEST test_row_keys_app)
    set_tests_properties(test_row_keys_app PROPERTIES TIMEOUT 600)
endif()
if(TEST test_programme_audio_app)
    set_tests_properties(test_programme_audio_app PROPERTIES TIMEOUT 120)
endif()
# The in-use test starts the real application about twenty times (the scenarios in its header: a
# layout run and a scripted run each, three of them paced at 16 ms a frame for a few seconds so an
# interval in milliseconds can pass). Linux runners are half this desktop's speed, so it is given
# the generous bound its siblings are.
if(TEST test_plugin_inuse_app)
    set_tests_properties(test_plugin_inuse_app PROPERTIES TIMEOUT 600)
endif()
# The duplicate-plugin test starts the real application five times (the bank's layout, a pointer sweep of
# 110 frames, the Fitted modules window with and without an install record, and a run with no hook), 12 to
# 110 frames each. The Linux runners are half this desktop's speed, so it is given the generous bound its
# siblings are.
if(TEST test_plugin_dup_app)
    set_tests_properties(test_plugin_dup_app PROPERTIES TIMEOUT 600)
endif()
# The BROWSE-key test starts the real application five times (a layout run, the press, its control,
# the cancelled press and a relaunch), 33 to 46 frames each. The Linux runners are half this
# desktop's speed, so it is given a bound well past the loop's two minutes.
if(TEST test_plugin_pathpick_app)
    set_tests_properties(test_plugin_pathpick_app PROPERTIES TIMEOUT 300)
endif()
# The Import-window test starts the real application sixteen times (a layout run, the failure and the
# empty box with a control each, Browse chosen with a control, cancelled, Escape, the Cancel key, a
# bare name with a control, and the AIRBAND section's layout, empty box and Browse), 23 to 90 frames
# each and a screenshot in most: 33 s for the first thirteen on this desktop and 111 s for all sixteen
# (the AIRBAND runs are the slow ones). The Linux runners are half this desktop's speed, so it is given
# the generous bound its siblings are.
if(TEST test_import_dialog_app)
    set_tests_properties(test_import_dialog_app PROPERTIES TIMEOUT 600)
endif()
# The display-menu test starts the real application twenty-three times (the menu on the waterfall and the
# spectrum, Manual with its sliders, the ten-decibel rule, Reset, a restart, a 0.99.75 config, a patch
# Spectrum part, and nine runs that hold the frame loop with `sleep` steps for the 4-second tip, 4 to
# 9 s each): 95 s on this desktop. The Linux runners are half this desktop's speed and the tip runs
# measure real time, so it is given the generous bound its siblings are rather than the loop's two
# minutes.
if(TEST test_display_menu_app)
    set_tests_properties(test_display_menu_app PROPERTIES TIMEOUT 600)
endif()
# The window-platform test starts the real application five times (a default run, =bad, =any, the
# platform Windows does not have, a forced one on Linux), a few seconds each; the Linux runners are
# half this desktop's speed and open the window slowly under a virtual display, so it is given the
# generous bound its siblings are rather than the loop's two minutes.
if(TEST test_window_platform_app)
    set_tests_properties(test_window_platform_app PROPERTIES TIMEOUT 600)
endif()
# Six starts of the real application, three of them a hand-over to a second process on llvmpipe.
if(TEST test_software_gl_app)
    set_tests_properties(test_software_gl_app PROPERTIES TIMEOUT 600)
endif()
# The passband-slide test starts the real application about twenty times, 40 to 90 frames each.
if(TEST test_passband_slide_app)
    set_tests_properties(test_passband_slide_app PROPERTIES TIMEOUT 600)
endif()
# The audio-buffer test builds the real AppWindow three times (its sink opened and then closed, which
# is a device open each) and makes a few hundred starved callbacks against it by hand. A few seconds
# here; the Linux runners are half this desktop's speed and open ALSA's `null` device slowly, so it is
# given the generous bound its siblings are rather than the loop's two minutes.
if(TEST test_audio_lead_app)
    set_tests_properties(test_audio_lead_app PROPERTIES TIMEOUT 300)
endif()
# A null ALSA sink cannot prove sustained playback. Report the integration test's
# explicit skip as Skipped in CTest, while real-output runs still test the join.
set_tests_properties(test_pipeline_audio_lead PROPERTIES SKIP_REGULAR_EXPRESSION "SKIP the pipeline's target")
# The priority test starts a real pipeline twice (a stop and a start is a new thread and a new request).
if(TEST test_dsp_priority)
    set_tests_properties(test_dsp_priority PROPERTIES TIMEOUT 120)
endif()

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

# The usage Worker and its reader (telemetry-worker/): the Worker's handler is
# imported and driven with old-format, new-format and malformed records, and
# usage.ps1 is run against a stand-in for Cloudflare's SQL API on loopback. Plain
# Node, no wrangler, no account, no network. Registered only where node is found.
find_program(CASCADE_NODE node)
if(CASCADE_NODE)
    add_test(NAME telemetry_worker
             COMMAND ${CASCADE_NODE} --test "${CMAKE_SOURCE_DIR}/telemetry-worker/worker.test.mjs")
    set_tests_properties(telemetry_worker PROPERTIES TIMEOUT 240)
endif()

# CLI error contract: each malformed invocation must exit nonzero.
add_test(NAME app_cli_bad_value COMMAND cascade --frames abc)
add_test(NAME app_cli_negative COMMAND cascade --frames -1)
add_test(NAME app_cli_missing_value COMMAND cascade --frames)
add_test(NAME app_cli_unknown_arg COMMAND cascade --bogus)
set_tests_properties(app_cli_bad_value app_cli_negative app_cli_missing_value
                     app_cli_unknown_arg PROPERTIES WILL_FAIL TRUE TIMEOUT 30)

# ---------------------------------------------------------------------------
# THE FENCE'S COMPILE PROOF (engine extraction stage 4, step 5; cmake/interface_fence.cmake).
#
# The interface target (cascade_interface) cannot include the engine: a stub of every header it may not name is first on its include path.
# That is a claim about the COMPILER, so it is proved by the compiler. Each probe below is one translation unit that includes ONE denied
# header and nothing else, compiled with exactly the interface's fence (cascade_apply_interface_fence); a ctest builds it and PASSES only
# if the build FAILS and says why (tests/fence_probe.cmake). Two controls keep it honest: the same translation units compiled WITHOUT the
# fence must build (so the failure is the fence's, not a broken probe), and a probe that includes only what the interface may see - the
# door, the command vocabulary, the table, the data headers - must build WITH it.
#
# The probes are EXCLUDE_FROM_ALL (they are built by their tests), except the two controls, which are part of every build. The tests share
# one resource lock: each runs a nested build of the same build tree.
# ---------------------------------------------------------------------------
set(CASCADE_FENCE_PROBE_DIR "${CMAKE_CURRENT_BINARY_DIR}/fence_probes")
set(CASCADE_FENCE_DENIED_PROBES
    engine/engine.hpp engine/engine_host.hpp engine/fox_engine_api.hpp engine/pass_stats.hpp
    core/pipeline.hpp core/plugin_ui.hpp core/plugin_api.hpp core/plugin_runner.hpp core/recorder.hpp core/transmitter.hpp
    # src/net, denied whole by stage 5f (the servers are the engine's): every header of the directory, one probe each
    net/cat_protocol.hpp net/cat_server.hpp net/control_ops.hpp net/status_compose.hpp net/web_audio.hpp net/web_auth.hpp
    net/web_control.hpp net/web_policy.hpp net/web_server.hpp net/web_session.hpp)
function(_cascade_fence_probe_source out_var id body)
    set(_src "${CASCADE_FENCE_PROBE_DIR}/${id}.cpp")
    set(_old "")
    if(EXISTS "${_src}")
        file(READ "${_src}" _old)
    endif()
    if(NOT _old STREQUAL body)
        file(WRITE "${_src}" "${body}")
    endif()
    set(${out_var} "${_src}" PARENT_SCOPE)
endfunction()
foreach(_h IN LISTS CASCADE_FENCE_DENIED_PROBES)
    string(MAKE_C_IDENTIFIER "${_h}" _id)
    _cascade_fence_probe_source(_src "probe_${_id}" "// GENERATED by tests/CMakeLists.txt: includes one header the interface may not.\n#include \"${_h}\"\nint main() { return 0; }\n")
    add_library(interface_fence_probe_${_id} OBJECT EXCLUDE_FROM_ALL "${_src}")
    target_link_libraries(interface_fence_probe_${_id} PRIVATE cascade_engine)
    cascade_apply_interface_fence(interface_fence_probe_${_id})
    # the control: the same unit, no fence, must compile
    add_library(interface_fence_control_${_id} OBJECT EXCLUDE_FROM_ALL "${_src}")
    target_link_libraries(interface_fence_control_${_id} PRIVATE cascade_engine)
    add_test(NAME interface_fence_denies_${_id}
        COMMAND ${CMAKE_COMMAND} -DMODE=DENIED -DBUILD_DIR=${CMAKE_BINARY_DIR} -DCONFIG=$<CONFIG>
                -DTARGET=interface_fence_probe_${_id} -DHEADER=${_h}
                -P ${CMAKE_CURRENT_SOURCE_DIR}/fence_probe.cmake)
    add_test(NAME interface_fence_control_${_id}
        COMMAND ${CMAKE_COMMAND} -DMODE=BUILDS -DBUILD_DIR=${CMAKE_BINARY_DIR} -DCONFIG=$<CONFIG>
                -DTARGET=interface_fence_control_${_id} -DHEADER=${_h}
                -P ${CMAKE_CURRENT_SOURCE_DIR}/fence_probe.cmake)
    set_tests_properties(interface_fence_denies_${_id} interface_fence_control_${_id} PROPERTIES
        TIMEOUT 300 RESOURCE_LOCK interface_fence_probes)
endforeach()
# What the interface MAY see compiles behind the fence: the door, the command vocabulary, the table, the data headers, ImGui.
_cascade_fence_probe_source(_allowed_src "probe_allowed"
    "// GENERATED by tests/CMakeLists.txt: includes what the interface may see and nothing it may not.\n#include \"foxsdr_api.h\"\n#include \"core/app_commands.hpp\"\n#include \"core/plugin_ui_data.hpp\"\n#include \"core/server_ports.hpp\"\n#include \"core/demod_names.hpp\"\n#include \"engine/engine_shell.hpp\"\n#include \"engine/receiver_tables.hpp\"\n#include \"gui/tune_local.hpp\"\n#include \"gui/scope_gain.hpp\"\n#include \"gui/converter_lo_seed.hpp\"\n#include \"imgui.h\"\nint main() { return 0; }\n")
add_library(interface_fence_allows OBJECT "${_allowed_src}")
target_link_libraries(interface_fence_allows PRIVATE cascade_engine imgui)
cascade_apply_interface_fence(interface_fence_allows)
add_test(NAME interface_fence_allows
    COMMAND ${CMAKE_COMMAND} -DMODE=BUILDS -DBUILD_DIR=${CMAKE_BINARY_DIR} -DCONFIG=$<CONFIG>
            -DTARGET=interface_fence_allows -DHEADER=engine/engine_shell.hpp
            -P ${CMAKE_CURRENT_SOURCE_DIR}/fence_probe.cmake)
set_tests_properties(interface_fence_allows PROPERTIES TIMEOUT 300 RESOURCE_LOCK interface_fence_probes)

# Fuzz targets for the parsers and the rate-dependent DSP, and the regression
# corpus each one replays as an ordinary test (fuzz_corpus_<target>). See
# tests/fuzz/fuzz.cmake.
include("${CMAKE_CURRENT_SOURCE_DIR}/fuzz/fuzz.cmake")

# ---------------------------------------------------------------------------
# Sanitizer build (-DCASCADE_SANITIZE=..., cmake/sanitize.cmake). Nothing below
# runs, and nothing above changes, when it is off. AT THE END OF THE FILE ON
# PURPOSE: it reads each test's TIMEOUT to scale it, so every property set
# above must already be set.
#
# Master's block (merged at 0.99.84), plus the branch's: the exclusion lists are filtered to
# the tests THIS tree registers (set_tests_properties on a test that is not there is a
# configure error), and the window tests registered a second time against the
# free-running pass (_free) take FOXSDR_TEST_PASS=free with the sanitizer's options.
# ---------------------------------------------------------------------------
if(CASCADE_SANITIZE_ENABLED)
    list(APPEND CASCADE_ALL_TEST_NAMES engine_link_check app_smoke app_selftest app_version
         app_cli_bad_value app_cli_negative app_cli_missing_value app_cli_unknown_arg)

    # TESTS THAT CANNOT RUN UNDER A SANITIZER, AND WHY. Each is still registered,
    # so ctest lists it - "Not Run (Disabled)" in its summary, and
    # `ctest -N -L sanitize-excluded` prints the set - rather than the suite
    # quietly getting smaller. One "name|reason" per entry (no semicolons).
    set(_cascade_san_excluded_master
        # A case inside it corrupts the heap on purpose; the rest of the file is
        # the same crash-handler territory as the three below.
        "test_crash_second_fault|its heap-trashed case writes into freed heap blocks on purpose to prove the crash handler survives a damaged heap and AddressSanitizer reports that write as a heap-use-after-free and ends the process before the handler under test runs"
        # Real-time audio: the DSP thread must keep pace with a source paced by
        # the wall clock. Instrumented it does not (measured: the channel filter
        # alone costs 40 to 85 percent of real time, the sink counts underruns),
        # so tone and RDS measurements fail with NO sanitizer report. They pass
        # in the plain build, which is where their timing claims are made.
        "test_channel_bandwidth|its pipeline cases measure audio against a real-time source and the instrumented DSP thread falls behind the clock (sink underruns) so tone levels fail with no sanitizer report"
        "test_pipeline_audio|its pipeline cases measure audio against a real-time source and the instrumented DSP thread falls behind the clock (sink underruns) so RDS and stereo measurements fail with no sanitizer report"
        "test_pipeline_device_rate_audio|its pipeline cases measure audio against a real-time source and the instrumented DSP thread falls behind the clock (sink underruns) so tone share fails with no sanitizer report"
        # The same cause, found by the first Linux run (37355910809): the generator
        # is paced by the wall clock, a DSP thread that is behind lets the ring fill,
        # the source drops the overflow (Pipeline::sourceThreadBody) and every drop is
        # a phase jump in the tone being measured - 773 Hz where the CW sidetone is
        # 700, a 1 kHz patch tone at 0.006 where it must exceed 0.01.
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
        string(REGEX REPLACE "_(free|stepped)$" "" _tbase "${t}")
        if(t IN_LIST CASCADE_MERGE_PENDING_TESTS OR _tbase IN_LIST CASCADE_MERGE_PENDING_TESTS)
            continue()   # a test the 0.99.84 merge took out of the build (tests/merge_pending.cmake): nothing to exclude
        endif()
        set_tests_properties(${t} PROPERTIES DISABLED TRUE LABELS "sanitize-excluded")
        message(STATUS "sanitizer: ${t} is excluded - ${why}")
    endforeach()
endif()

# ---------------------------------------------------------------------------
# THE REAL-TIME AUDIO MEASUREMENTS DO NOT SHARE A MACHINE. Each of these runs
# a pipeline against a source paced by the wall clock and asserts tone levels,
# RDS, fades or drift corrections; a DSP thread that falls behind the clock
# drops samples and the measurement fails with nothing else wrong - the exact
# mechanism that keeps them out of the sanitizer builds (see
# CASCADE_SANITIZE_EXCLUDED_TESTS above). Seven siblings running beside them
# under `ctest -j8` is the same starvation by another route: measured here,
# test_pipeline_audio and test_tx_audio fail under -j8 and pass alone. They
# stay in the headless pass but each takes the machine to itself.
set(CASCADE_REALTIME_AUDIO_TESTS
    app_selftest
    test_channel_bandwidth
    test_patch_audio
    test_pipeline_audio
    test_pipeline_audio_lead
    test_pipeline_device_rate_audio
    test_plugin_audio
    test_tx_audio
)
foreach(t ${CASCADE_REALTIME_AUDIO_TESTS})
    set_tests_properties(${t} PROPERTIES RUN_SERIAL TRUE)
endforeach()

# ---------------------------------------------------------------------------
# EVERY TEST IS "gui" OR "headless", SO THE SUITE CAN RUN IN TWO PASSES:
#
#   ctest -L headless -j8       # the bulk of the suite, in parallel
#   ctest -L gui                # the window-openers, serial as before
#
# "gui" is every test that puts a real window on the desktop: it spawns
# cascade with --frames (a bounded AppWindow::run, which is glfwCreateWindow),
# calls AppWindow::run itself (test_deck_knobs_app, in a child of itself), or
# creates a GLFW window of its own (test_mouse_passthrough_cache,
# test_viewports). Those cannot run in parallel on a desktop - they fight for
# focus and the input script's pointer - and they are what kept the whole
# 40-minute suite serial. Everything else never opens a window and is safe
# under -j: that includes the tests that only CONSTRUCT an AppWindow (the
# window is made in run(), not the constructor), test_window_failed_app
# (every spawn sets FOXSDR_TEST_WINDOW_FAIL, so glfwCreateWindow - or, for
# =imgui, the backend behind a still-hidden window - fails by design and no
# window is ever shown), and test_soapy_enum_proc (the real
# enumeration child is windowless).
#
# The list below is maintained BY HAND and checked twice at configure time:
# a name that is not a registered test is a hard error (a rename must update
# this list), and a registered test that ends up with neither label - or both -
# is a hard error too, so the partition stays total as tests are added. A new
# test is headless by default; add it here only if it opens a window.
set(CASCADE_GUI_TESTS
    app_smoke
    test_airband_app
    test_bias_key_run
    test_bookmark_stacking_app
    test_bookmark_tick_app
    test_clean_exit_marker
    test_console_owner_app
    test_crash_upload
    test_deck_knobs_app
    test_decoder_settings_fold_app
    test_decoder_window_app
    test_diag_hang
    test_diagnostics
    test_display_fold_app
    test_display_menu_app
    test_display_restart_app
    test_exit_cause_app
    test_foreign_modules_app
    test_freq_markers_app
    test_gps_app
    test_health_app
    test_import_dialog_app
    test_main_view
    test_map_coverage_app
    test_markers_window_app
    test_mouse_passthrough_cache
    test_passband_slide_app
    test_patch_canvas_nav
    test_patch_info_pane
    test_patch_map_credit
    test_plugin_dup_app
    test_plugin_inuse_app
    test_plugin_pathpick_app
    test_plugin_store_app
    test_problem_report_app
    test_radio_setup_app
    test_rail_fold_app
    test_receiver_layout_app
    test_row_keys_app
    test_sentinel_app
    test_shutdown_budget
    test_slow_frames_app
    test_software_gl_app
    test_spectrum_trace_app
    test_stall_count
    test_startup_state
    test_status_fold_app
    test_stop_ends_recordings
    test_store_update_app
    test_team_message_app
    test_tester_host_only
    test_tester_link
    test_tester_usage
    test_tester_usage_app
    test_theme_census
    test_tx_audio_page_app
    test_tx_audio_plate_app
    test_typed_band_mode_app
    test_ui_scale_app
    test_unplaced_list_app
    test_viewports
    test_window_platform_app
)

# Label APPENDED, not set: the sanitizer and TSan passes above put their own
# labels on some tests (sanitize-excluded, tsan, fuzz on the corpus replays)
# and those must survive.
get_directory_property(_cascade_registered_tests TESTS)
foreach(t ${CASCADE_GUI_TESTS})
    if(NOT t IN_LIST _cascade_registered_tests AND NOT t IN_LIST CASCADE_MERGE_PENDING_TESTS)
        message(FATAL_ERROR "CASCADE_GUI_TESTS names '${t}' but no such test is "
                "registered - renamed or removed? Update the list above.")
    endif()
endforeach()
foreach(t ${_cascade_registered_tests})
    if(t IN_LIST CASCADE_GUI_TESTS)
        set_property(TEST ${t} APPEND PROPERTY LABELS gui)
    else()
        set_property(TEST ${t} APPEND PROPERTY LABELS headless)
    endif()
endforeach()

# The partition is total, asserted: every registered test carries exactly one
# of the two labels. This cannot fail for the loop above alone - it is the
# guard against a later edit that sets LABELS outright (wiping gui/headless)
# or labels a test both ways.
foreach(t ${_cascade_registered_tests})
    get_test_property(${t} LABELS _labels)
    set(_n 0)
    if("gui" IN_LIST _labels)
        math(EXPR _n "${_n} + 1")
    endif()
    if("headless" IN_LIST _labels)
        math(EXPR _n "${_n} + 1")
    endif()
    if(NOT _n EQUAL 1)
        message(FATAL_ERROR "test '${t}' carries ${_n} of the labels gui/headless "
                "(labels: '${_labels}'); every test must carry exactly one.")
    endif()
endforeach()
