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
endif()

foreach(src ${CASCADE_TESTS})
    get_filename_component(name "${src}" NAME_WE)
    add_executable(${name} "${src}")
    target_link_libraries(${name} PRIVATE cascade_lib)
    target_include_directories(${name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
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
    elseif(name STREQUAL "test_soapy_enum_proc" AND TARGET soapy_fault_fixture)
        # Hands the fault fixture's directory to the real enumeration child;
        # cascade.exe is built first because that block runs it.
        add_dependencies(${name} soapy_fault_fixture soapy_sdrplay_fixture cascade)
        add_test(NAME ${name} COMMAND ${name} "$<TARGET_FILE_DIR:soapy_fault_fixture>")
    else()
        add_test(NAME ${name} COMMAND ${name})
    endif()
    # A liveness bug (stuck spin loop, deadlock) must fail the suite, not hang
    # it forever — ctest's default per-test timeout is 1500 s.
    set_tests_properties(${name} PROPERTIES TIMEOUT 120)
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
