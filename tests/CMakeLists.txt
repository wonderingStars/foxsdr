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
