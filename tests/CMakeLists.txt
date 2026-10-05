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
    elseif(name STREQUAL "test_plugin_rescan_skip")
        # Copies the two probe modules into a plugins folder of its own and
        # drives the real AppWindow's rescan against it; built first.
        add_dependencies(${name} rescan_probe_a rescan_probe_b)
        add_test(NAME ${name} COMMAND ${name} "$<TARGET_FILE:rescan_probe_a>"
                 "$<TARGET_FILE:rescan_probe_b>")
    elseif(name STREQUAL "test_slow_frames_app")
        # Runs the real cascade.exe (built first) and, in process, a real plugin
        # reload against one of the probe modules above.
        add_dependencies(${name} rescan_probe_a cascade)
        add_test(NAME ${name} COMMAND ${name} "$<TARGET_FILE:rescan_probe_a>")
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
        "test_pipeline_device_rate_audio|its pipeline cases measure audio against a real-time source and the instrumented DSP thread falls behind the clock (sink underruns) so tone share fails with no sanitizer report")

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
