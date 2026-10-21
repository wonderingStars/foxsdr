# Static analysis and warnings-as-errors for OUR code.
#
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#
# TWO OPTIONS, BOTH OFF BY DEFAULT, BOTH ABSENT FROM THE SHIPPING BUILD. With
# both off this file defines no target, sets no variable the rest of the build
# reads and edits no compile or link flag: the generated project files are what
# they were before it existed (cmake/sanitize.cmake makes the same promise, and
# the same proof - a diff of the generated files - holds for both).
#
#   -DCASCADE_ANALYZE=ON   MSVC /analyze over our own translation units.
#   -DCASCADE_WERROR=ON    warnings in OUR targets are errors.
#
# "OUR targets" is every target that links cascade_flags (cascade_lib, the
# application, the report reader, the tests). The vendored libraries (imgui,
# glfw, portaudio, pffft) do not link it and are compiled as they always were,
# and neither are the vcpkg headers or the compiler's own headers held to the
# same standard: see the flags below.
#
# WHAT /analyze IS GIVEN, AND WHY (MSVC 14.44, VS 2022 17.14; each switch was
# tried on a small program before it was used here):
#
#   /analyze                 the analyser itself: the C6xxx (PREfast: C6001
#                            uninitialised memory, C6011 null dereference, C6385
#                            and C6386 buffer over-read and overrun, ...) and
#                            C28xxx (SAL) families.
#   /analyze:external-       do not analyse code that comes from EXTERNAL headers.
#   /external:anglebrackets  ...and what is external is anything included with
#                            <angle brackets>: the standard library, the Windows
#                            SDK and UCRT, and every third-party header this
#                            tree reaches (<imgui.h>, <nlohmann/json.hpp>,
#                            <httplib.h>, <SoapySDR/...>). The tree includes its
#                            OWN headers with "quotes" everywhere (checked: no
#                            #include <core/...>, <net/...> or the like), so
#                            nothing of ours is hidden by this. CMake already
#                            passes the vendored and vcpkg include directories
#                            as /external:I (they are SYSTEM includes); this
#                            covers the part that route cannot, the compiler's
#                            and the SDK's directories, which come from the
#                            INCLUDE environment variable.
#   /external:W0             and warnings from external headers are silenced, so
#                            the count is OUR code's.
#   /analyze:plugin <dll>    loads EspXEngine.dll, the host for the extension
#                            checkers. Without it /analyze runs no concurrency
#                            checks at all: C26110 ("caller failing to hold
#                            lock"), C26115 ("failing to release lock") and
#                            their family come from ConcurrencyCheck.dll, which
#                            only that engine runs.
#
# WHICH EXTENSION RUNS. Left alone the engine loads every extension it can find,
# CppCoreCheck included, whose C264xx style rules are thousands of "can be
# marked noexcept" and "use gsl::at" - a different review from this one. The
# list is chosen by the Esp_Extensions environment variable (the Visual Studio
# IDE sets it from the same properties). With the Visual Studio generators this
# file writes a Directory.Build.props into the BUILD directory that sets it to
# ConcurrencyCheck.dll alone for every project beneath it, which is how
# Microsoft's own targets do it (Microsoft.CodeAnalysis.Extensions.targets).
# With another generator (Ninja + cl) set the variable yourself:
#   set Esp_Extensions=ConcurrencyCheck.dll

option(CASCADE_ANALYZE "Run MSVC /analyze over our own code (cmake/analyze.cmake)" OFF)
option(CASCADE_WERROR  "Make compiler warnings in our own targets errors (cmake/analyze.cmake)" OFF)
# Which extension checkers /analyze hosts (see "WHICH EXTENSION RUNS" above):
# ConcurrencyCheck.dll by default; a ';'-separated list of DLLs, or ALL for every
# one the engine finds (CppCoreCheck's thousands of style rules included).
set(CASCADE_ANALYZE_EXTENSIONS "ConcurrencyCheck.dll" CACHE STRING
    "Esp_Extensions for CASCADE_ANALYZE: ConcurrencyCheck.dll, a ;-list of DLLs, or ALL")

set(CASCADE_ANALYZE_ENABLED OFF)
set(CASCADE_WERROR_ENABLED OFF)

if(NOT CASCADE_ANALYZE AND NOT CASCADE_WERROR)
    return()
endif()

add_library(cascade_analyze INTERFACE)

if(CASCADE_ANALYZE)
    if(NOT MSVC)
        message(WARNING
            "CASCADE_ANALYZE: only MSVC's /analyze is wired in; it is ignored here. "
            "(GCC's -fanalyzer has no usable C++ support, and clang-tidy is a separate tool.)")
    else()
        get_filename_component(_cl_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
        set(_esp_engine "${_cl_dir}/EspXEngine.dll")
        if(NOT EXISTS "${_esp_engine}" OR NOT EXISTS "${_cl_dir}/ConcurrencyCheck.dll")
            message(FATAL_ERROR
                "CASCADE_ANALYZE: EspXEngine.dll / ConcurrencyCheck.dll are not beside ${CMAKE_CXX_COMPILER}. "
                "Install the C++ code analysis component of Visual Studio.")
        endif()
        set(CASCADE_ANALYZE_ENABLED ON)
        target_compile_options(cascade_analyze INTERFACE
            /analyze /analyze:external- /external:anglebrackets /external:W0
            "/analyze:plugin${_esp_engine}")

        if(CMAKE_GENERATOR MATCHES "Visual Studio")
            # Esp_Extensions=<list> for every project below the build directory
            # (MSBuild imports the nearest Directory.Build.props above a project
            # file). Verified on the real command line of an analysed compile:
            # the environment of cl.exe carries Esp_Extensions=ConcurrencyCheck.dll.
            if(NOT CASCADE_ANALYZE_EXTENSIONS STREQUAL "ALL")
                file(WRITE "${CMAKE_BINARY_DIR}/Directory.Build.props"
"<Project xmlns=\"http://schemas.microsoft.com/developer/msbuild/2003\">
  <!-- Generated by cmake/analyze.cmake (CASCADE_ANALYZE): which extension
       checkers /analyze hosts. -->
  <ItemGroup>
    <BuildMacro Include=\"Esp_Extensions\">
      <EnvironmentVariable>true</EnvironmentVariable>
      <Value>${CASCADE_ANALYZE_EXTENSIONS}</Value>
    </BuildMacro>
  </ItemGroup>
</Project>
")
            else()
                file(REMOVE "${CMAKE_BINARY_DIR}/Directory.Build.props")
            endif()
        else()
            message(STATUS "CASCADE_ANALYZE: set Esp_Extensions=${CASCADE_ANALYZE_EXTENSIONS} in the build environment, "
                           "or CppCoreCheck's style rules run as well")
        endif()
    endif()
endif()

if(CASCADE_WERROR)
    set(CASCADE_WERROR_ENABLED ON)
    # At the EXISTING warning level (/W4, -Wall -Wextra), never above it: this
    # makes a warning fail the build, it does not look for more.
    if(MSVC)
        target_compile_options(cascade_analyze INTERFACE /WX)
    else()
        target_compile_options(cascade_analyze INTERFACE -Werror)
    endif()
endif()

message(STATUS "cascade analysis: analyze=${CASCADE_ANALYZE_ENABLED} werror=${CASCADE_WERROR_ENABLED}")
