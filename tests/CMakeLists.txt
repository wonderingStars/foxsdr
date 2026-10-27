# One executable per test_*.cpp, discovered by glob. Agents add test files
# only; this file itself almost never needs editing for a new test — the one
# exception is a test that needs a command-line argument, handled below by
# name for test_json_dump_policy.
file(GLOB CASCADE_TESTS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/test_*.cpp")

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
    target_link_libraries(${name} PRIVATE cascade_lib)
    target_include_directories(${name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
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
    elseif(name STREQUAL "test_tester_usage" OR name STREQUAL "test_tester_link")
        # Scans src/gui/app_window.cpp, src/core/tester_usage.cpp,
        # src/core/tester_link.cpp and src/main.cpp for the
        # token-never-logged property, so both need the repository root too.
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
    elseif(name STREQUAL "test_tx_audio_app")
        # A real AppWindow with the transmitting fixture fitted in a scratch plugins folder, a
        # fake transmit radio and the real runner, driven through the page's own paths; built
        # first.
        add_dependencies(${name} tx_audio_fixture)
        add_test(NAME ${name} COMMAND ${name} "$<TARGET_FILE:tx_audio_fixture>")
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
        add_test(NAME ${name} COMMAND ${name} "$<TARGET_FILE:rescan_probe_a>"
                 "$<TARGET_FILE:rescan_probe_b>")
    elseif(name STREQUAL "test_plugin_update_cleanup_app")
        # A real AppWindow updating a real plugin module (probe a 1.0.0 -> 1.1.0) beside an
        # unrelated one (probe b); all three built first, in that order on the command line.
        add_dependencies(${name} rescan_probe_a rescan_probe_a_newer rescan_probe_b)
        add_test(NAME ${name} COMMAND ${name} "$<TARGET_FILE:rescan_probe_a>"
                 "$<TARGET_FILE:rescan_probe_a_newer>" "$<TARGET_FILE:rescan_probe_b>")
    elseif(name STREQUAL "test_plugin_host_origin")
        # A plugin site's module never takes a declared name from an official one (0.99.74): the
        # probe module built twice (1.0.0 and 1.1.0, one declared name), scanned by the real host and
        # the real AppWindow with install records saying which is whose; both built first.
        add_dependencies(${name} rescan_probe_a rescan_probe_a_newer)
        add_test(NAME ${name} COMMAND ${name} "$<TARGET_FILE:rescan_probe_a>"
                 "$<TARGET_FILE:rescan_probe_a_newer>")
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
        add_test(NAME ${name} COMMAND ${name} "$<TARGET_FILE:rescan_probe_a>")
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
    elseif(name STREQUAL "test_viewport_fallback_app")
        # Runs the real cascade.exe (built first) with the track fixture's map page, the plugin store
        # and the Fitted modules window open as separate windows, has the backend refuse the next
        # window part-way through the run (FOXSDR_TEST_VIEWPORT_REFUSE_AT), and reads from the
        # self-capture's window list that every page is then wholly inside the main window.
        add_dependencies(${name} track_probe_fixture cascade)
        add_test(NAME ${name} COMMAND ${name} "$<TARGET_FILE:track_probe_fixture>")
    elseif(name STREQUAL "test_programme_audio_app")
        add_dependencies(${name} programme_audio_fixture)
        add_test(NAME ${name} COMMAND ${name} "$<TARGET_FILE:programme_audio_fixture>")
    elseif(name STREQUAL "test_patch_blueprints_app")
        # Fits the preset probe (text and picture decoder, one preset) in a real AppWindow, stops
        # it, places its blueprints and runs one on the signal generator - built first.
        add_dependencies(${name} preset_probe_fixture)
        add_test(NAME ${name} COMMAND ${name} "$<TARGET_FILE:preset_probe_fixture>")
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
    list(APPEND CASCADE_ALL_TEST_NAMES ${name})
endforeach()

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
set_tests_properties(test_theme_census PROPERTIES TIMEOUT 600)
# The team-message test starts the real application fourteen times, 70 frames each.
# CI run 38564730541: x64 passed in 61.98 s; arm64 hit the inherited 120 s
# bound at 120.11 s without stdout (Store test: 35.27 s x64, 69.55 s arm64).
# Give only this test 300 s; unbuffered launch tags identify a child if it hangs.
set_tests_properties(test_team_message_app PROPERTIES TIMEOUT 300)
# The deck-knobs test starts the real application dozens of times (as a child of
# itself, so that a fake radio can sit behind the window; the noise-floor and Airband
# runs hold a few seconds of real time each): 46 s on this desktop before the Airband
# scenarios, 140 to 150 s with them, and 321 s (x64) and 335 s (arm64) on the GitHub
# Linux runners (CI run 38288445561), so it gets the same room the census does.
set_tests_properties(test_deck_knobs_app PROPERTIES TIMEOUT 600)
# The TRANSMIT-grant plate test starts the real cascade eight times (the Fitted modules
# window with a real plugin module pinned to run and the receiver running, 1013 frames
# in all): 19 to 37 s on this desktop (about 4 s of each run is start-up, shutdown and its
# sleeps). Both GitHub Linux runners were still in it when the loop's two minutes ended
# (CI run 38288445561, x64 and arm64), its siblings that start the real application
# (test_tx_audio_page_app 24 s there against 8 s here) run at 3 times this desktop's
# time, and the deck-knobs test shows frames of 50 ms or more there. It prints each run's
# start and duration unbuffered, so a run stopped by this bound still says which one was
# in hand.
set_tests_properties(test_tx_audio_plate_app PROPERTIES TIMEOUT 600)
# The exit-cause test starts the real application eleven times (a staged run per
# class and per case with nothing to read, then two real kills and the start that
# reads them) and leaves a second between each kill and the next start, because a
# report and a session start are stamped in whole seconds. 78 s on this desktop; the
# loop's two minutes is not enough on a loaded runner. Windows only (it SKIPs elsewhere).
set_tests_properties(test_exit_cause_app PROPERTIES TIMEOUT 600)
# The radio-setup test starts the real application fifteen times (one per
# synthetic machine, the persisted tick, the key, CHECK AGAIN, and the real bus).
# 71 s on this desktop; the GitHub Linux runners run the application-launching
# tests at about half that speed (test_airband_app: 123 s here, 261 s there), so
# the loop's two minutes killed it on both Linux jobs of the 0.99.69 run.
set_tests_properties(test_radio_setup_app PROPERTIES TIMEOUT 600)
# The airband test starts the application thirty-one times (a layout run and a
# scripted run per scenario: lookup, O'Hare's list listened to (0.99.71), listen,
# hand-back, the spectrum, the typed list, presets). Six runs took 68 s on the
# x64 runner and 101 s on arm64, so twenty-nine would not fit the loop's two
# minutes there (0.99.66: both Linux jobs timed out at 120 s). On this desktop
# it is 80 s.
set_tests_properties(test_airband_app PROPERTIES TIMEOUT 900)
# The update-cleanup test runs the real AppWindow's plugin rescan five times against real
# modules and waits on a worker for each transfer (a second or two here); the Linux runners
# are half this desktop's speed, so it is given its own bound instead of the loop's two minutes.
set_tests_properties(test_plugin_update_cleanup_app PROPERTIES TIMEOUT 300)
# The catalogue/pictures test builds the real AppWindow six times and waits on workers (a few
# hundred milliseconds each here); same reasoning, same bound.
set_tests_properties(test_plugin_catalogue_app PROPERTIES TIMEOUT 300)
# The store test starts the real application about a dozen times (a layout run and a scripted
# run per scenario: the Browse and Updates tabs, a page, the legal notice's tick, the window's
# remembered rectangle, the Fitted modules window and its page). Each is 120 to 200 frames.
# Linux runners are half this desktop's speed, so it is given the generous bound its siblings are.
set_tests_properties(test_plugin_store_app PROPERTIES TIMEOUT 600)
# The Decoder output window test starts the real application nine times (the preset on three
# window sizes, the capture seam, two picture-window runs, and a layout, a drag and a reset run),
# 100 to 110 frames each.
# The same bound, for the same reason, as the store test.
set_tests_properties(test_decoder_window_app PROPERTIES TIMEOUT 600)
# The settings-fold test starts the real application three times (folded as it opens, one click,
# two clicks), 40 to 60 frames each. The same bound, for the same reason, as the test above.
set_tests_properties(test_decoder_settings_fold_app PROPERTIES TIMEOUT 600)
# The unplaced-list test starts the real application three times (the heard rows, a bare NaN, and
# "On screen only"), 80 frames each. The same bound, for the same reason, as the test above.
set_tests_properties(test_unplaced_list_app PROPERTIES TIMEOUT 600)
# The viewport-fallback test starts the real application three times (refused mid-run, one viewport
# from the start, and the control that keeps separate windows), 110 frames each.
set_tests_properties(test_viewport_fallback_app PROPERTIES TIMEOUT 600)
# The row-keys test starts the real application twelve times (a layout run, the window open, a
# press of HD2, of HD1 then HD2, the three that must press nothing and the control for the click's
# geometry, an instrument's memory rows opened and pressed, and the module's page with sixteen
# keys), 60 to 160 frames each with a half-second sleep in most: 28 s on this desktop. The same
# bound, for the same reason, as the tests above.
set_tests_properties(test_row_keys_app PROPERTIES TIMEOUT 600)
set_tests_properties(test_programme_audio_app PROPERTIES TIMEOUT 120)
# The in-use test starts the real application about twenty times (the scenarios in its header: a
# layout run and a scripted run each, three of them paced at 16 ms a frame for a few seconds so an
# interval in milliseconds can pass). Linux runners are half this desktop's speed, so it is given
# the generous bound its siblings are.
set_tests_properties(test_plugin_inuse_app PROPERTIES TIMEOUT 600)
# The duplicate-plugin test starts the real application five times (the bank's layout, a pointer sweep of
# 110 frames, the Fitted modules window with and without an install record, and a run with no hook), 12 to
# 110 frames each. The Linux runners are half this desktop's speed, so it is given the generous bound its
# siblings are.
set_tests_properties(test_plugin_dup_app PROPERTIES TIMEOUT 600)
# The BROWSE-key test starts the real application five times (a layout run, the press, its control,
# the cancelled press and a relaunch), 33 to 46 frames each. The Linux runners are half this
# desktop's speed, so it is given a bound well past the loop's two minutes.
set_tests_properties(test_plugin_pathpick_app PROPERTIES TIMEOUT 300)
# The Import-window test starts the real application sixteen times (a layout run, the failure and the
# empty box with a control each, Browse chosen with a control, cancelled, Escape, the Cancel key, a
# bare name with a control, and the AIRBAND section's layout, empty box and Browse), 23 to 90 frames
# each and a screenshot in most: 33 s for the first thirteen on this desktop and 111 s for all sixteen
# (the AIRBAND runs are the slow ones). The Linux runners are half this desktop's speed, so it is given
# the generous bound its siblings are.
set_tests_properties(test_import_dialog_app PROPERTIES TIMEOUT 600)
# The display-menu test starts the real application twenty-three times (the menu on the waterfall and the
# spectrum, Manual with its sliders, the ten-decibel rule, Reset, a restart, a 0.99.75 config, a patch
# Spectrum part, and nine runs that hold the frame loop with `sleep` steps for the 4-second tip, 4 to
# 9 s each): 95 s on this desktop. The Linux runners are half this desktop's speed and the tip runs
# measure real time, so it is given the generous bound its siblings are rather than the loop's two
# minutes.
set_tests_properties(test_display_menu_app PROPERTIES TIMEOUT 600)
# The window-platform test starts the real application five times (a default run, =bad, =any, the
# platform Windows does not have, a forced one on Linux), a few seconds each; the Linux runners are
# half this desktop's speed and open the window slowly under a virtual display, so it is given the
# generous bound its siblings are rather than the loop's two minutes.
set_tests_properties(test_window_platform_app PROPERTIES TIMEOUT 600)
# Six starts of the real application, three of them a hand-over to a second process on llvmpipe.
set_tests_properties(test_software_gl_app PROPERTIES TIMEOUT 600)
# The passband-slide test starts the real application about twenty times, 40 to 90 frames each.
set_tests_properties(test_passband_slide_app PROPERTIES TIMEOUT 600)
# The audio-buffer test builds the real AppWindow three times (its sink opened and then closed, which
# is a device open each) and makes a few hundred starved callbacks against it by hand. A few seconds
# here; the Linux runners are half this desktop's speed and open ALSA's `null` device slowly, so it is
# given the generous bound its siblings are rather than the loop's two minutes.
set_tests_properties(test_audio_lead_app PROPERTIES TIMEOUT 300)
# A null ALSA sink cannot prove sustained playback. Report the integration test's
# explicit skip as Skipped in CTest, while real-output runs still test the join.
set_tests_properties(test_pipeline_audio_lead PROPERTIES SKIP_REGULAR_EXPRESSION "SKIP the pipeline's target")
# The priority test starts a real pipeline twice (a stop and a start is a new thread and a new request).
set_tests_properties(test_dsp_priority PROPERTIES TIMEOUT 120)

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

# Fuzz targets for the parsers and the rate-dependent DSP, and the regression
# corpus each one replays as an ordinary test (fuzz_corpus_<target>). See
# tests/fuzz/fuzz.cmake.
include("${CMAKE_CURRENT_SOURCE_DIR}/fuzz/fuzz.cmake")

# ---------------------------------------------------------------------------
# Sanitizer build (-DCASCADE_SANITIZE=..., cmake/sanitize.cmake). Nothing below
# runs, and nothing above changes, when it is off.
# ---------------------------------------------------------------------------
if(CASCADE_SANITIZE_ENABLED)
    list(APPEND CASCADE_ALL_TEST_NAMES app_smoke app_selftest app_version
         app_cli_bad_value app_cli_negative app_cli_missing_value app_cli_unknown_arg)

    # TESTS THAT CANNOT RUN UNDER A SANITIZER, AND WHY. Each is still registered,
    # so ctest lists it - "Not Run (Disabled)" in its summary, and
    # `ctest -N -L sanitize-excluded` prints the set - rather than the suite
    # quietly getting smaller. One "name|reason" per entry (no semicolons).
    set(CASCADE_SANITIZE_EXCLUDED_TESTS
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

    # TESTS THAT FAULT A CHILD PROCESS ON PURPOSE, to prove the crash handler
    # writes its report. They run under AddressSanitizer with ASan's OWN handlers
    # for access violations, aborts, illegal instructions and arithmetic faults
    # switched off: with them on, the sanitizer's handler runs first, prints its
    # report and ends the process (exit 1), and the product's handler - the thing
    # under test - never sees the fault. Memory-error CHECKS (heap, stack, use
    # after free) are instrumentation, not handlers, and stay on.
    set(CASCADE_SANITIZE_FAULT_TESTS test_crash_capture test_crash_late_module
        test_soapy_enum_proc)
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
    test_bookmark_groups_app
    test_bookmark_stacking_app
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
    test_exit_cause_app
    test_foreign_modules_app
    test_freq_markers_app
    test_gps_app
    test_health_app
    test_import_dialog_app
    test_main_view
    test_mouse_passthrough_cache
    test_passband_slide_app
    test_patch_canvas_nav
    test_patch_info_pane
    test_patch_map_credit
    test_plugin_dup_app
    test_plugin_inuse_app
    test_plugin_pathpick_app
    test_plugin_store_app
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
    test_tester_link
    test_tester_usage
    test_theme_census
    test_tx_audio_page_app
    test_tx_audio_plate_app
    test_unplaced_list_app
    test_viewport_fallback_app
    test_viewports
    test_window_platform_app
)

# Label APPENDED, not set: the sanitizer and TSan passes above put their own
# labels on some tests (sanitize-excluded, tsan, fuzz on the corpus replays)
# and those must survive.
get_directory_property(_cascade_registered_tests TESTS)
foreach(t ${CASCADE_GUI_TESTS})
    if(NOT t IN_LIST _cascade_registered_tests)
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
