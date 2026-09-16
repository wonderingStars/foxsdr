# One executable per test_*.cpp, discovered by glob. Agents add test files
# only; this file itself almost never needs editing for a new test — the one
# exception is a test that needs a command-line argument, handled below by
# name for test_json_dump_policy.
#
# Under CASCADE_ANDROID this file takes a different branch entirely (below):
# the full glob pulls in test_*.cpp files that need GLFW/ImGui/PortAudio/
# SoapySDR/OpenSSL, none of which this configuration builds, so only a
# curated subset — the tests that exercise cascade_core and nothing else —
# is built instead. See the curated list's own comment for exactly which
# tests and why.
if(NOT CASCADE_ANDROID)

file(GLOB CASCADE_TESTS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/test_*.cpp")
foreach(src ${CASCADE_TESTS})
    get_filename_component(name "${src}" NAME_WE)
    if(name STREQUAL "test_audio_out_aaudio")
        # This test drives src/sink/audio_out_aaudio.cpp's ring/priming/
        # underrun core and its two AAudio callback cores directly, NOT
        # through cascade_lib: cascade_lib already carries audio_out.cpp's
        # PortAudio-backed cascade::sink::AudioOut, and linking both
        # backends' definitions of the same class into one binary is a
        # straight ODR violation (see audio_out_aaudio.cpp's file header).
        # Compiling it here with NO __ANDROID__ defined — this is a host
        # build — means the AAudio-touching methods (open/close's real body/
        # streamAlive/listOutputDevices, which need <aaudio/AAudio.h> and
        # libaaudio) are never emitted; only the platform-independent half
        # this test exercises is.
        add_executable(${name} "${src}" "${CMAKE_SOURCE_DIR}/src/sink/audio_out_aaudio.cpp")
        target_include_directories(${name} PRIVATE "${CMAKE_SOURCE_DIR}/src")
    else()
        add_executable(${name} "${src}")
        target_link_libraries(${name} PRIVATE cascade_lib)
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

# The app doubles as a smoke test. The PASS_REGULAR_EXPRESSION makes the
# exact-count half of the --frames contract observable: rendering N+1 frames
# (or none) fails even though the exit code would still be 0.
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

else()  # CASCADE_ANDROID

# ---------------------------------------------------------------------------
# The host-subset / Android test list.
#
# Every name here was picked because it exercises ONLY what cascade_core
# still builds under CASCADE_ANDROID (src/dsp, most of src/core, the native
# SDR drivers, src/usb, and the non-TLS half of src/net) — no GLFW, no
# ImGui, no PortAudio, no SoapySDR, no OpenSSL. This same list is built in
# THREE configurations by the Android port's build scripts:
#   1. natively on Linux x86-64 with no NDK (CASCADE_ANDROID=ON, the "host
#      subset" build) — registered below with add_test() and run by ctest,
#      proving the exclusion set is coherent without any device;
#   2. cross-compiled for the x86-64 Android ABI (the emulator) — not
#      runnable by ctest (wrong ABI for this host), pushed to a device or
#      emulator with adb instead;
#   3. cross-compiled for arm64-v8a (the real device ABI) — compile-and-link
#      only, same reason.
# CASCADE_ANDROID_CROSS_TARGET (set above in the top-level CMakeLists.txt)
# tells cases 2/3 apart from case 1: it is the ONLY thing that decides
# whether ctest gets these tests, never whether they get BUILT.
#
# Left OUT, and why — every omission is a test that needs a unit this
# configuration does not build, not a weakened test:
#   - Anything under tests/test_app_*, test_win_frame, test_key_bindings,
#     test_instrument_*, test_scope_*, test_waterfall_view, test_map_view,
#     test_plugin_ui, test_plugins_view, test_plugin_store_view,
#     test_transmit_page, test_list_pick, test_page_geometry,
#     test_viewports, test_rail_banks, test_track_info_cache,
#     test_bandwidth_readout, test_bench_text_fits, test_font_assets,
#     test_fonts, test_coastline, test_freq_scale, test_shell_open(_posix),
#     test_display_stall, test_gps_app, test_startup_state: GUI (ImGui/GLFW).
#   - test_audio_in, test_audio_out, test_plugin_audio: PortAudio.
#   - test_soapy_*, test_vendor_guard, test_vendor_lines, test_sdrplay_source:
#     SoapySDR (sdrplay_source.hpp has no Soapy #include of its own, but its
#     test exercises SoapySource-shaped comparison helpers that pull the
#     rest of the Soapy surface in).
#   - test_config: builds against core/config.cpp fine, but the TEST FILE
#     itself (not config.cpp) also exercises gui/key_bindings.cpp's
#     defaultKeyBindings()/keyBindingsToConfig()/keyBindingsFromConfig()/
#     formatChord() to prove key-binding round-trips through AppConfig — all
#     four are GUI (excluded). config.cpp's OWN behaviour is still covered
#     indirectly: every other curated test links the same config.cpp
#     translation unit inside cascade_lib.
#   - test_web_server, test_web_auth, test_web_policy: exercise
#     hashPassword()/verifyLogin()/SessionStore, which
#     net/web_auth.cpp stubs to "not available" under CASCADE_ANDROID (no
#     OpenSSL on the NDK — see its ANDROID-TODO(auth-crypto) comment).
#     test_web_control and test_web_audio below cover the rest of src/net
#     that doesn't touch auth.
#   - test_crash_upload, test_telemetry, test_plugin_repo, test_httplib_vendor,
#     test_crash_capture, test_crash_absorbed_child, test_diag_hang,
#     test_diagnostics, test_thread_fault, test_updater, test_report_reader,
#     test_gps_reader, test_scanner: need the excluded HTTPS transports, the
#     Windows/libunwind crash+hang capture internals, or gps_reader.cpp
#     (excluded because it pulls gui/scope_view.hpp for one constant).
#   - test_rtlsdr_live: NOT excluded for a unit reason — it explicitly
#     supports "no hardware attached" as a passing outcome — but it is left
#     out of ANDROID_TEST_NAMES anyway because opening a USB device node from
#     an adb shell has a real chance of a permission failure that says
#     nothing about this port; add it back once real hardware access is
#     verified on-device.
# ---------------------------------------------------------------------------
set(CASCADE_ANDROID_TEST_NAMES
    # dsp
    test_fft test_fir test_nco test_vfo test_demod test_demod_agc
    test_modulator test_noise_reduction test_notch test_rds test_resampler
    test_spectrum test_squelch test_stereo_fm test_spsc_ring test_window
    # band plans (config itself has no dedicated test buildable here — see
    # the test_config exclusion note above; config.cpp is still compiled
    # into cascade_lib and exercised transitively by every test below)
    test_band_plan
    # plugin host
    test_plugin_host
    # usb fake/usbfs
    test_usb_usbfs test_usb_winusb
    # native driver tests (over the usb fake, or a real bus if present)
    test_rtlsdr_source test_hackrf_source test_airspy_source
    test_airspyhf_source test_rx888_source test_mirisdr_source
    test_pluto_source test_pluto_tx
    # cat_server
    test_cat_server test_cat_protocol
    # net, minus auth/TLS
    test_web_control test_web_audio
    # bonus: pure-logic, zero risk, exercises code this port also keeps
    test_serial_port test_json_dump_policy
)

foreach(name IN LISTS CASCADE_ANDROID_TEST_NAMES)
    set(src "${CMAKE_CURRENT_SOURCE_DIR}/${name}.cpp")
    if(NOT EXISTS "${src}")
        message(FATAL_ERROR "cascade: CASCADE_ANDROID_TEST_NAMES names ${name}, but ${src} does not exist")
    endif()
    add_executable(${name} "${src}")
    target_link_libraries(${name} PRIVATE cascade_lib)
    target_include_directories(${name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

    if(NOT CASCADE_ANDROID_CROSS_TARGET)
        if(name STREQUAL "test_json_dump_policy")
            add_test(NAME ${name} COMMAND ${name} "${CMAKE_SOURCE_DIR}/src")
        else()
            add_test(NAME ${name} COMMAND ${name})
        endif()
        set_tests_properties(${name} PROPERTIES TIMEOUT 120)
    endif()
endforeach()

endif()  # CASCADE_ANDROID
