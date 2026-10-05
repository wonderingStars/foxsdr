# Memory-error and undefined-behaviour checking for the whole tree.
#
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#
# WHY THIS EXISTS. The field crashes that reach users - a resampler reading
# past its input at a sample rate nobody tried, a vector read 21 KB past its
# end in a test that only failed one run in ten, a driver returning more
# samples than it was asked for - are all memory errors, and none of them
# fails an ordinary run of an ordinary build. AddressSanitizer turns every one
# of them into an immediate, located report.
#
# OFF BY DEFAULT, AND OFF MEANS UNTOUCHED. With CASCADE_SANITIZE empty this file
# defines no target, sets no variable the rest of the build reads, and edits no
# compile or link flag: the generated project files are what they were before
# this file existed. Nothing a user receives is built with it.
#
#   -DCASCADE_SANITIZE=address              AddressSanitizer (MSVC, GCC, Clang)
#   -DCASCADE_SANITIZE=undefined            UndefinedBehaviorSanitizer (GCC, Clang)
#   -DCASCADE_SANITIZE=address,undefined    both (on MSVC only "address" applies:
#                                           MSVC has no UBSan, so a request for
#                                           it there is a warning, not a silent
#                                           half-measure)
#   -DCASCADE_SANITIZE=address,fuzzer       additionally compile the coverage
#                                           instrumentation libFuzzer steers by
#                                           (see tests/fuzz/CMakeLists.txt)
#   -DCASCADE_SANITIZE=thread               ThreadSanitizer (GCC, Clang; MSVC has
#                                           none, so it is an error there; not
#                                           with address, which it cannot share
#                                           a process with; undefined may join)
#
# WHAT IT INSTRUMENTS: cascade_lib and everything that links it (the
# application, the report reader and every test), plus the vendored static
# libraries that end up inside those executables (imgui, glfw, portaudio,
# pffft). On MSVC the vendored C++ must match: the standard library's container
# annotations are checked at link time and a mixed set fails with LNK2038.
# What it deliberately does NOT instrument is the test fixtures built as
# MODULEs: they stand in for a third party's plugin, which is not built with
# our flags either.

set(CASCADE_SANITIZE "" CACHE STRING
    "Sanitizers to build with: address, undefined, thread, fuzzer, comma separated; empty = off (the shipping build)")
set_property(CACHE CASCADE_SANITIZE PROPERTY STRINGS "" "address" "address,undefined" "undefined" "address,fuzzer" "thread")

set(CASCADE_SANITIZE_ENABLED OFF)
set(CASCADE_SANITIZE_ADDRESS OFF)
set(CASCADE_SANITIZE_UNDEFINED OFF)
set(CASCADE_SANITIZE_FUZZER OFF)
set(CASCADE_SANITIZE_THREAD OFF)

string(TOLOWER "${CASCADE_SANITIZE}" _cascade_san_value)
string(REPLACE "," ";" _cascade_san_value "${_cascade_san_value}")
string(REPLACE "+" ";" _cascade_san_value "${_cascade_san_value}")
# -DCASCADE_SANITIZE=OFF / NO / 0 read the way every other option does.
if(_cascade_san_value MATCHES "^(off|no|false|0|n)$")
    set(_cascade_san_value "")
endif()

foreach(_tok IN LISTS _cascade_san_value)
    if(_tok STREQUAL "address")
        set(CASCADE_SANITIZE_ADDRESS ON)
    elseif(_tok STREQUAL "undefined")
        set(CASCADE_SANITIZE_UNDEFINED ON)
    elseif(_tok STREQUAL "fuzzer")
        set(CASCADE_SANITIZE_FUZZER ON)
    elseif(_tok STREQUAL "thread")
        set(CASCADE_SANITIZE_THREAD ON)
    elseif(NOT _tok STREQUAL "")
        message(FATAL_ERROR
            "CASCADE_SANITIZE: unknown sanitizer '${_tok}' (known: address, undefined, thread, fuzzer)")
    endif()
endforeach()

# ThreadSanitizer is GCC and Clang only - MSVC has none - and cannot share a
# process with AddressSanitizer (each wants the whole shadow memory to itself;
# the compilers refuse the combination). UBSan alongside it is fine. libFuzzer's
# coverage build is built on ASan, so it is out too.
if(CASCADE_SANITIZE_THREAD)
    if(MSVC)
        message(FATAL_ERROR "CASCADE_SANITIZE: MSVC has no ThreadSanitizer; 'thread' needs GCC or Clang (the Linux CI job)")
    endif()
    if(CASCADE_SANITIZE_ADDRESS)
        message(FATAL_ERROR "CASCADE_SANITIZE: 'address' and 'thread' cannot be combined; use two build directories")
    endif()
    if(CASCADE_SANITIZE_FUZZER)
        message(FATAL_ERROR "CASCADE_SANITIZE: 'fuzzer' needs 'address' and so cannot be combined with 'thread'")
    endif()
endif()

if(CASCADE_SANITIZE_UNDEFINED AND MSVC)
    message(WARNING
        "CASCADE_SANITIZE: MSVC has no UndefinedBehaviorSanitizer; 'undefined' is ignored in this build.")
    set(CASCADE_SANITIZE_UNDEFINED OFF)
endif()

# libFuzzer's coverage instrumentation is only useful - and on MSVC only legal -
# on top of AddressSanitizer.
if(CASCADE_SANITIZE_FUZZER AND NOT CASCADE_SANITIZE_ADDRESS)
    message(FATAL_ERROR "CASCADE_SANITIZE: 'fuzzer' needs 'address' alongside it")
endif()

if(CASCADE_SANITIZE_ADDRESS OR CASCADE_SANITIZE_UNDEFINED OR CASCADE_SANITIZE_FUZZER OR CASCADE_SANITIZE_THREAD)
    set(CASCADE_SANITIZE_ENABLED ON)
endif()

if(NOT CASCADE_SANITIZE_ENABLED)
    return()
endif()

# ---------------------------------------------------------------------------
# From here on a sanitizer was asked for.
# ---------------------------------------------------------------------------

add_library(cascade_sanitize INTERFACE)

# Scale for every test's TIMEOUT, applied in tests/CMakeLists.txt. An
# instrumented build runs several times slower and the per-test limits were
# chosen for the plain one; ctest's own --timeout would not help, because it
# only supplies a default for tests that have none.
#
# ThreadSanitizer is the slowest of them (commonly 5 to 15 times), so its scale
# is the larger.
if(CASCADE_SANITIZE_THREAD)
    set(_cascade_default_scale 10)
else()
    set(_cascade_default_scale 5)
endif()
set(CASCADE_SANITIZE_TIMEOUT_SCALE ${_cascade_default_scale} CACHE STRING
    "Multiplier applied to every test's TIMEOUT in a sanitizer build")

# NAME=VALUE pairs every test runs under (tests/CMakeLists.txt). Runtime
# options live HERE, not in the environment of whoever runs ctest, so a failure
# is the same on every machine and in CI.
set(CASCADE_SANITIZE_TEST_ENV "")

if(MSVC)
    if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8 OR CMAKE_GENERATOR_PLATFORM STREQUAL "ARM64")
        message(FATAL_ERROR "CASCADE_SANITIZE: MSVC's AddressSanitizer is x64-only here")
    endif()
    if(NOT CASCADE_SANITIZE_ADDRESS)
        message(FATAL_ERROR "CASCADE_SANITIZE: on MSVC the only sanitizer is 'address'")
    endif()

    # /bigobj: the instrumentation adds sections per function and global, and
    # src/gui/app_window.cpp (26,000 lines) exceeds the default object-file
    # limit once it has them: "error C1128: number of sections exceeded object
    # file format limit".
    target_compile_options(cascade_sanitize INTERFACE /fsanitize=address /bigobj)
    # libFuzzer's edge counters, for the fuzz build only: /fsanitize=fuzzer on
    # a translation unit instruments it AND leaves a default-library directive
    # for libFuzzer in its object. A linker pulls an archive member only to
    # resolve an undefined symbol, so a test that has its own main() never takes
    # libFuzzer's; the fuzz executables, which have none, do (verified with this
    # toolchain, VS 2022 17.14 / MSVC 14.44).
    if(CASCADE_SANITIZE_FUZZER)
        target_compile_options(cascade_sanitize INTERFACE /fsanitize=fuzzer)
    endif()

    # WHAT /fsanitize=address CANNOT BE COMBINED WITH (MSVC documents all
    # three; the compiler or linker refuses the build rather than warning):
    #   /RTC1 (run-time checks) - in CMake's Debug flags
    #   /ZI   (edit and continue)
    #   /INCREMENTAL linking     - in CMake's Debug linker flags
    # The Release build this is normally run on has none of them; a Debug build
    # has them by default, so they are removed here rather than left to fail.
    foreach(_lang C CXX)
        foreach(_cfg DEBUG)
            string(REGEX REPLACE "/RTC[1csu]+" "" CMAKE_${_lang}_FLAGS_${_cfg} "${CMAKE_${_lang}_FLAGS_${_cfg}}")
            string(REGEX REPLACE "/ZI( |$)" "/Zi\\1" CMAKE_${_lang}_FLAGS_${_cfg} "${CMAKE_${_lang}_FLAGS_${_cfg}}")
        endforeach()
    endforeach()
    string(REGEX REPLACE "/RTC[1csu]+" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
    string(REGEX REPLACE "/RTC[1csu]+" "" CMAKE_C_FLAGS "${CMAKE_C_FLAGS}")
    foreach(_kind EXE SHARED MODULE)
        foreach(_cfg DEBUG RELWITHDEBINFO)
            string(REGEX REPLACE "/INCREMENTAL(:NO)?" "" _lf "${CMAKE_${_kind}_LINKER_FLAGS_${_cfg}}")
            set(CMAKE_${_kind}_LINKER_FLAGS_${_cfg} "${_lf} /INCREMENTAL:NO")
        endforeach()
    endforeach()

    # THE RUNTIME DLL. The build uses the dynamic C runtime (/MD), so the
    # AddressSanitizer runtime is the dynamic one too, and Windows must find it
    # when the executable starts. A test that cannot find it does not fail
    # quietly: Windows raises a "code execution cannot proceed" dialog on the
    # desktop of whoever is logged in and the test hangs behind it. So it is
    # copied beside every executable, exactly as SoapySDR.dll is (see
    # tests/CMakeLists.txt), instead of relying on PATH.
    get_filename_component(_cl_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
    # ONE DLL for every configuration: a Debug build (/MDd) imports the same
    # clang_rt.asan_dynamic-x86_64.dll, not the asan_dbg_ variant that sits
    # beside it (checked with a Debug, Release and RelWithDebInfo build of a
    # small program).
    find_file(CASCADE_ASAN_RUNTIME_DLL NAMES clang_rt.asan_dynamic-x86_64.dll
              PATHS "${_cl_dir}" NO_DEFAULT_PATH)
    if(NOT CASCADE_ASAN_RUNTIME_DLL)
        message(FATAL_ERROR
            "CASCADE_SANITIZE: clang_rt.asan_dynamic-x86_64.dll is not beside ${CMAKE_CXX_COMPILER}. "
            "Install the 'MSVC AddressSanitizer' component of Visual Studio.")
    endif()

    # ASan's options for the Windows runtime. LeakSanitizer does not exist
    # there, so no detect_leaks. halt_on_error is the default; stated so it is
    # visible. A report goes to the test's stderr, which ctest prints on
    # failure (--output-on-failure).
    list(APPEND CASCADE_SANITIZE_TEST_ENV
         "ASAN_OPTIONS=halt_on_error=1:print_stacktrace=1:symbolize=1:strict_string_checks=1:allocator_may_return_null=0")
else()
    set(_san_list "")
    if(CASCADE_SANITIZE_ADDRESS)
        list(APPEND _san_list address)
    endif()
    if(CASCADE_SANITIZE_UNDEFINED)
        list(APPEND _san_list undefined)
    endif()
    if(CASCADE_SANITIZE_THREAD)
        list(APPEND _san_list thread)
    endif()
    list(JOIN _san_list "," _san_csv)
    if(CASCADE_SANITIZE_FUZZER)
        # Coverage only: the libFuzzer main() is linked into the fuzz
        # executables alone (tests/fuzz/CMakeLists.txt).
        set(_cov "-fsanitize=fuzzer-no-link")
    else()
        set(_cov "")
    endif()
    # -g1: line tables and no variable information. A report with file:line is
    # what makes a sanitizer finding actionable from a CI log; full -g across
    # 270 test executables is gigabytes (tests/CMakeLists.txt explains why the
    # plain build strips them), and a later -g option wins over the Release
    # flags' -g.
    target_compile_options(cascade_sanitize INTERFACE
        -fsanitize=${_san_csv} ${_cov} -fno-omit-frame-pointer -g1)
    target_link_options(cascade_sanitize INTERFACE -fsanitize=${_san_csv})

    # detect_leaks is on (LeakSanitizer is part of ASan on Linux x86-64/arm64).
    # halt_on_error and a stack on every report: a CI log is the only evidence.
    if(CASCADE_SANITIZE_ADDRESS)
        list(APPEND CASCADE_SANITIZE_TEST_ENV
             "ASAN_OPTIONS=detect_leaks=1:halt_on_error=1:abort_on_error=0:print_stacktrace=1:symbolize=1:strict_string_checks=1:detect_stack_use_after_return=1:allocator_may_return_null=0"
             "LSAN_OPTIONS=print_suppressions=0:suppressions=${CMAKE_CURRENT_SOURCE_DIR}/tools/sanitize/lsan.supp")
    endif()
    if(CASCADE_SANITIZE_UNDEFINED)
        list(APPEND CASCADE_SANITIZE_TEST_ENV
             "UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1:symbolize=1")
    endif()
    # halt_on_error: the first race ends the process with TSan's exit code.
    # second_deadlock_stack: a lock-order report shows both orders. history_size
    # 4 keeps the "previous access" stack of a race that happened a while ago
    # (the default of 2 loses it on a busy thread). The suppression file holds
    # third-party libraries only; nothing of ours is in it.
    if(CASCADE_SANITIZE_THREAD)
        list(APPEND CASCADE_SANITIZE_TEST_ENV
             "TSAN_OPTIONS=halt_on_error=1:second_deadlock_stack=1:history_size=4:symbolize=1:print_suppressions=0:suppressions=${CMAKE_CURRENT_SOURCE_DIR}/tools/sanitize/tsan.supp")
    endif()
endif()

if(CASCADE_SANITIZE_ADDRESS)
    target_compile_definitions(cascade_sanitize INTERFACE CASCADE_SANITIZE_ADDRESS=1)
endif()
if(CASCADE_SANITIZE_UNDEFINED)
    target_compile_definitions(cascade_sanitize INTERFACE CASCADE_SANITIZE_UNDEFINED=1)
endif()
if(CASCADE_SANITIZE_THREAD)
    target_compile_definitions(cascade_sanitize INTERFACE CASCADE_SANITIZE_THREAD=1)
endif()

# Beside-the-executable staging of the ASan runtime (MSVC); nothing elsewhere.
function(cascade_sanitize_stage_runtime target)
    if(MSVC)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${CASCADE_ASAN_RUNTIME_DLL}"
                    "$<TARGET_FILE_DIR:${target}>"
            VERBATIM)
    endif()
endfunction()

# Instrument a vendored static library compiled from source by this tree. Its
# own CMake files may use either signature of target_link_libraries, and the two
# cannot be mixed on one target, so the compile options are copied rather than
# linked in; a static library needs no link options, the executable carries them.
function(cascade_sanitize_target target)
    get_target_property(_opts cascade_sanitize INTERFACE_COMPILE_OPTIONS)
    target_compile_options(${target} PRIVATE ${_opts})
endfunction()

message(STATUS "cascade sanitizers: address=${CASCADE_SANITIZE_ADDRESS} undefined=${CASCADE_SANITIZE_UNDEFINED} thread=${CASCADE_SANITIZE_THREAD} fuzzer=${CASCADE_SANITIZE_FUZZER}")
