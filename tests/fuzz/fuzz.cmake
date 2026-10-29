# Fuzz targets for the parsers and rate-dependent DSP, and their regression
# corpora. Included from tests/CMakeLists.txt (not add_subdirectory'd), so the
# tests registered here live in the same directory scope as every other test and
# the sanitizer build's timeout/environment pass in that file covers them.
#
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#
# TWO WAYS TO RUN ONE TARGET, ONE FUNCTION:
#
#   1. fuzz_corpus_replay <target> <corpus dir> - ALWAYS built, registered with
#      CTest as fuzz_corpus_<target>. Calls the target once per corpus file and
#      fails if it faults or a property check fails. This is the regression
#      suite: it runs on every platform in the normal suite, and under
#      AddressSanitizer in a sanitizer build, with no fuzzing engine at all.
#
#   2. fuzz_<target> - libFuzzer executables, built ONLY with -DCASCADE_FUZZ=ON.
#      That needs a build whose every object carries libFuzzer's coverage
#      counters, so it needs -DCASCADE_SANITIZE=address,fuzzer as well (and Clang
#      on Linux; MSVC's /fsanitize=fuzzer on Windows). Without the counters in
#      cascade_engine a fuzzer is blind to everything but the target's own file.
#
# ADDING A TARGET: write tests/fuzz/fuzz_<name>.cpp (extern "C" int
# LLVMFuzzerTestOneInput(const uint8_t*, size_t)), add <name> to the list below,
# and put at least one input in tests/fuzz/corpus/<name>/ - an empty corpus fails
# its replay test on purpose.

set(CASCADE_FUZZ_TARGETS
    plugin_index
    plugin_manifest
    plugin_strings
    update_manifest
    config
    freq_manager
    freq_markers
    band_plan
    freq_import
    iq_file
    cat_command
    control_request
    web_policy
    web_http
    crash_report
    report_feed
    nmea
    patch_document
    patch_rates
    resampler
    channel_chain
    plugin_module
    plugin_marker
    plugin_settings
    plugin_hostapi
    plugin_records)

option(CASCADE_FUZZ "Build the libFuzzer executables (needs CASCADE_SANITIZE=address,fuzzer)" OFF)

set(_fuzz_dir "${CMAKE_CURRENT_SOURCE_DIR}/fuzz")
set(_fuzz_gen "${CMAKE_CURRENT_BINARY_DIR}/fuzz")

# ---------------------------------------------------------------------------
# 1. The replay program: every target's object file in one executable.
# ---------------------------------------------------------------------------
set(_fuzz_inc "")
set(_fuzz_srcs "")
foreach(_t ${CASCADE_FUZZ_TARGETS})
    string(APPEND _fuzz_inc "CASCADE_FUZZ_TARGET(${_t})\n")
    list(APPEND _fuzz_srcs "${_fuzz_dir}/fuzz_${_t}.cpp")
    # Each file defines LLVMFuzzerTestOneInput; in the replay program they must
    # coexist, so each is renamed - for THIS object library only, because the
    # same files are compiled unrenamed into the libFuzzer executables below.
    set_source_files_properties("${_fuzz_dir}/fuzz_${_t}.cpp" PROPERTIES COMPILE_DEFINITIONS
        "$<$<STREQUAL:$<TARGET_PROPERTY:NAME>,cascade_fuzz_objects>:LLVMFuzzerTestOneInput=cascade_fuzz_${_t}>")
endforeach()
# Rewritten only when the list changes, so editing a target does not rebuild the
# replay driver.
file(CONFIGURE OUTPUT "${_fuzz_gen}/fuzz_targets.inc" CONTENT "${_fuzz_inc}" @ONLY)

add_library(cascade_fuzz_objects OBJECT ${_fuzz_srcs})
target_link_libraries(cascade_fuzz_objects PRIVATE cascade_engine)
target_include_directories(cascade_fuzz_objects PRIVATE "${_fuzz_dir}")

add_executable(fuzz_corpus_replay "${_fuzz_dir}/fuzz_replay_main.cpp"
               $<TARGET_OBJECTS:cascade_fuzz_objects>)
target_link_libraries(fuzz_corpus_replay PRIVATE cascade_engine)
target_include_directories(fuzz_corpus_replay PRIVATE "${_fuzz_gen}")

# The helpers for each fuzz executable, the same three the test loop applies.
function(cascade_fuzz_finish target)
    # No DWARF in the plain Linux build, for the reason given in the test loop.
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT CASCADE_SANITIZE_ENABLED)
        target_compile_options(${target} PRIVATE -g0)
        target_link_options(${target} PRIVATE -Wl,--strip-debug)
    endif()
    if(CASCADE_SANITIZE_ENABLED)
        cascade_sanitize_stage_runtime(${target})
    endif()
    # SoapySDR.dll beside the program (the tests link it normally; without it
    # Windows raises a modal "DLL not found" dialog and the run waits behind it).
    if(WIN32 AND TARGET SoapySDR)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    $<TARGET_FILE:SoapySDR> $<TARGET_FILE_DIR:${target}>)
    endif()
endfunction()

# The bounded, deterministic mutation pass (plugin containment P2, tests/fuzz/fuzz_smoke_main.cpp): the corpus as seeds, a fixed random seed, a
# fixed number of mutants. Label `fuzz_smoke`; the plugin-surface targets only (CASCADE_FUZZ_SMOKE_ITERATIONS says how many mutants each).
add_executable(fuzz_smoke "${_fuzz_dir}/fuzz_smoke_main.cpp" $<TARGET_OBJECTS:cascade_fuzz_objects>)
target_link_libraries(fuzz_smoke PRIVATE cascade_engine)
target_include_directories(fuzz_smoke PRIVATE "${_fuzz_gen}")

cascade_fuzz_finish(fuzz_corpus_replay)
cascade_fuzz_finish(fuzz_smoke)
# The objects of the replay program carry the same per-target treatment.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT CASCADE_SANITIZE_ENABLED)
    target_compile_options(cascade_fuzz_objects PRIVATE -g0)
endif()

foreach(_t ${CASCADE_FUZZ_TARGETS})
    add_test(NAME fuzz_corpus_${_t}
             COMMAND fuzz_corpus_replay ${_t} "${_fuzz_dir}/corpus/${_t}")
    set_tests_properties(fuzz_corpus_${_t} PROPERTIES TIMEOUT 120 LABELS fuzz)
    list(APPEND CASCADE_ALL_TEST_NAMES fuzz_corpus_${_t})
endforeach()

# fuzz_smoke_<target>: the targets that read what a PLUGIN or a plugin's files hand the host (plugin containment P2, docs 6.29). Each is bounded
# (seconds) and deterministic; the long run is libFuzzer, by hand or nightly (docs/SANITIZERS-AND-FUZZING.md).
# <target>:<mutants>. The two that read FILES (the module and the marker/store) write each mutant to disk first, which on Windows is slow
# (a scanner looks at every file), so they get fewer; the rest are in-memory and run tens of thousands a second.
set(CASCADE_FUZZ_SMOKE_TARGETS
    plugin_module:400 plugin_marker:200 plugin_settings:20000 plugin_hostapi:20000 plugin_records:5000
    plugin_index:5000 plugin_manifest:5000 plugin_strings:20000)
foreach(_spec ${CASCADE_FUZZ_SMOKE_TARGETS})
    string(REPLACE ":" ";" _parts "${_spec}")
    list(GET _parts 0 _t)
    list(GET _parts 1 _n)
    add_test(NAME fuzz_smoke_${_t}
             COMMAND fuzz_smoke ${_t} ${_n} 1 "${_fuzz_dir}/corpus/${_t}")
    set_tests_properties(fuzz_smoke_${_t} PROPERTIES TIMEOUT 240 LABELS "fuzz;fuzz_smoke")
    list(APPEND CASCADE_ALL_TEST_NAMES fuzz_smoke_${_t})
endforeach()

# ---------------------------------------------------------------------------
# 2. libFuzzer executables, only when asked for.
# ---------------------------------------------------------------------------
if(CASCADE_FUZZ)
    if(NOT CASCADE_SANITIZE_FUZZER)
        message(FATAL_ERROR
            "CASCADE_FUZZ=ON needs -DCASCADE_SANITIZE=address,fuzzer: a fuzzer over "
            "a library with no coverage counters is blind to the code it is for.")
    endif()
    if(NOT MSVC AND NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "CASCADE_FUZZ=ON: libFuzzer needs Clang on this platform "
                "(found ${CMAKE_CXX_COMPILER_ID}); the corpus replay tests work with any compiler.")
    endif()
    foreach(_t ${CASCADE_FUZZ_TARGETS})
        add_executable(fuzz_${_t} "${_fuzz_dir}/fuzz_${_t}.cpp")
        target_link_libraries(fuzz_${_t} PRIVATE cascade_engine)
        target_include_directories(fuzz_${_t} PRIVATE "${_fuzz_dir}")
        if(NOT MSVC)
            # libFuzzer's main(); the objects were compiled with
            # -fsanitize=fuzzer-no-link (cmake/sanitize.cmake).
            target_link_options(fuzz_${_t} PRIVATE -fsanitize=fuzzer)
        endif()
        cascade_fuzz_finish(fuzz_${_t})
    endforeach()
    message(STATUS "libFuzzer targets: ${CASCADE_FUZZ_TARGETS}")
endif()
