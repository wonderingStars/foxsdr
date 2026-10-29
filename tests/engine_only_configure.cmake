# The configure-level proof of CASCADE_BUILD_INTERFACE=OFF (engine extraction stage 7g).
#
# THE CLAIM. With the option OFF the project configures - and its resource staging works - on a
# machine that has none of OpenGL, X11, Wayland or xkbcommon, because the OFF path never reaches
# find_package(OpenGL), the GLFW subdirectory (whose configure is the one that wants the windowing
# headers), Dear ImGui, the shell, the interface, cascade_gui or the cascade executable.
#
# WHY IT IS PROVED THIS WAY. A test cannot uninstall this machine's OpenGL, and a machine that
# has no OpenGL is the thing under test. CMAKE_DISABLE_FIND_PACKAGE_OpenGL=ON is CMake's own
# switch for "pretend this package is not installed"; with it a find_package(OpenGL REQUIRED)
# that is REACHED fails the configure ("A REQUIRED package cannot be disabled"). So:
#
#   1. THE OFF CONFIGURE, in a scratch build directory, with OpenGL disabled, must EXIT 0: it never
#      reached the call. (GLFW's own find calls and the windowing libraries are behind the same
#      option, so they are not reached either: the generated project holds none of them, below.)
#   2. THE CONTROL: the same configure with the option ON (the default) and OpenGL disabled must
#      FAIL, and name OpenGL. Without this, step 1 could pass because the switch does nothing.
#   3. WHAT THE OFF TREE IS: no cascade, cascade_gui, cascade_interface, imgui or glfw
#      target (asking the build for each is an error), no OpenGL or GLFW cache entry, and the
#      targets there are: foxsdr-engine, cascade_engine, the link check, the reader.
#   4. THE STAGING, from that tree: building cascade_stage_resources (the target foxsdr-engine
#      depends on) puts resources/bandplans and resources/firmware where foxsdr-engine is built,
#      byte for byte what the source tree holds. This is the OFF build's whole staging story: no
#      cascade POST_BUILD copies anything there.
#
# What this does NOT prove: that the engine COMPILES and LINKS in the OFF tree (that is a build of
# the whole engine library; docs/engine-stage4.md section 6.20 records it done by hand, and the
# CI job linux-engine-only does it on every push), or that a bare Raspberry Pi image configures
# (the machine this runs on has OpenGL; it is disabled, not absent).
#
# Run by ctest as `engine_only_configure`:
#   cmake -DSOURCE_DIR=<repo> -DSCRATCH=<dir> -DGENERATOR=<name> [-DPLATFORM=x64] [-DTOOLCHAIN=<file>]
#         [-DTRIPLET=<triplet>] -DCONFIG=<Release> -P engine_only_configure.cmake
#
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

foreach(_v SOURCE_DIR SCRATCH GENERATOR CONFIG)
    if(NOT DEFINED ${_v} OR "${${_v}}" STREQUAL "")
        message(FATAL_ERROR "engine_only_configure: -D${_v}=... is required")
    endif()
endforeach()

set(_failed 0)
# A condition is TEXT here, evaluated as CMake code (a macro would hand if() one quoted argument).
macro(check cond what)
    cmake_language(EVAL CODE "if(${cond})\n set(_ok TRUE)\nelse()\n set(_ok FALSE)\nendif()")
    if(_ok)
        message(STATUS "  ok    ${what}")
    else()
        message(STATUS "  FAIL  ${what}")
        math(EXPR _failed "${_failed} + 1")
    endif()
endmacro()

# What every nested configure is given: the generator and platform of the build this test is in, the
# same toolchain and triplet (the vcpkg SoapySDR the engine needs), and the OpenGL switch.
set(_common -G "${GENERATOR}" -DCMAKE_BUILD_TYPE=${CONFIG} -DCMAKE_DISABLE_FIND_PACKAGE_OpenGL=ON)
if(PLATFORM)
    list(APPEND _common -A "${PLATFORM}")
endif()
if(TOOLCHAIN)
    list(APPEND _common "-DCMAKE_TOOLCHAIN_FILE=${TOOLCHAIN}")
endif()
if(TRIPLET)
    list(APPEND _common "-DVCPKG_TARGET_TRIPLET=${TRIPLET}")
endif()

set(_off "${SCRATCH}/off")
set(_on "${SCRATCH}/on")
file(REMOVE_RECURSE "${SCRATCH}")
file(MAKE_DIRECTORY "${SCRATCH}")

# --- 1. the OFF configure ---------------------------------------------------------------------------
message(STATUS "configure, CASCADE_BUILD_INTERFACE=OFF, OpenGL disabled")
execute_process(
    COMMAND "${CMAKE_COMMAND}" -S "${SOURCE_DIR}" -B "${_off}" ${_common} -DCASCADE_BUILD_INTERFACE=OFF
    RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
check("_rc EQUAL 0" "the OFF configure exits 0 (exit ${_rc})")
check("_out MATCHES \"window application OFF\"" "it said it was configuring without the window application")
if(NOT _rc EQUAL 0)
    message(STATUS "---- stdout ----\n${_out}\n---- stderr ----\n${_err}")
endif()
# CMake itself reports, as an unused-cli warning, that nothing read the disabling switch: that is the
# switch's own name in the output (and is, in fact, the proof find_package(OpenGL) was never called).
# Take the name out before asking whether OpenGL is mentioned anywhere else.
string(REPLACE "CMAKE_DISABLE_FIND_PACKAGE_OpenGL" "" _outf "${_out}")
string(REPLACE "CMAKE_DISABLE_FIND_PACKAGE_OpenGL" "" _errf "${_err}")
check("NOT (_outf MATCHES \"OpenGL\" OR _errf MATCHES \"OpenGL\")" "OpenGL is not looked for, found or complained of")

# --- 2. the control ----------------------------------------------------------------------------------
message(STATUS "control: configure, option ON, OpenGL disabled (must fail)")
execute_process(
    COMMAND "${CMAKE_COMMAND}" -S "${SOURCE_DIR}" -B "${_on}" ${_common}
    RESULT_VARIABLE _crc OUTPUT_VARIABLE _cout ERROR_VARIABLE _cerr)
check("NOT _crc EQUAL 0" "the same configure with the option ON FAILS (exit ${_crc})")
check("_cerr MATCHES \"OpenGL\"" "and it fails on OpenGL: the switch works, so step 1 is not vacuous")

# --- 3. what the OFF tree is -------------------------------------------------------------------------
file(STRINGS "${_off}/CMakeCache.txt" _cache REGEX "OPENGL|OpenGL_|GLFW_|glfw")
# the option's own help text names OpenGL and GLFW; nothing else may, and the disabling switch is ours
list(FILTER _cache EXCLUDE REGEX "CASCADE_BUILD_INTERFACE|CMAKE_DISABLE_FIND_PACKAGE_OpenGL|^//")
check("NOT _cache" "no OpenGL or GLFW cache entry (${_cache})")

foreach(_t cascade cascade_gui cascade_interface imgui glfw)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" --build "${_off}" --config ${CONFIG} --target ${_t}
        RESULT_VARIABLE _trc OUTPUT_QUIET ERROR_QUIET)
    check("NOT _trc EQUAL 0" "the OFF tree has no target ${_t} (build exit ${_trc})")
endforeach()

# --- 4. the staging, from that tree -------------------------------------------------------------------
message(STATUS "build cascade_stage_resources in the OFF tree")
execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${_off}" --config ${CONFIG} --target cascade_stage_resources
    RESULT_VARIABLE _src OUTPUT_VARIABLE _sout ERROR_VARIABLE _serr)
check("_src EQUAL 0" "cascade_stage_resources builds in the OFF tree (exit ${_src})")
if(NOT _src EQUAL 0)
    message(STATUS "---- stdout ----\n${_sout}\n---- stderr ----\n${_serr}")
endif()

foreach(_dir bandplans firmware)
    file(GLOB_RECURSE _want RELATIVE "${SOURCE_DIR}/resources/${_dir}" "${SOURCE_DIR}/resources/${_dir}/*")
    list(LENGTH _want _nwant)
    check("_nwant GREATER 0" "the source tree has files under resources/${_dir} (${_nwant})")
    # wherever the generator put foxsdr-engine's directory: the one place a resources/<dir> appears
    file(GLOB_RECURSE _got RELATIVE "${_off}" "${_off}/*/resources/${_dir}/*" "${_off}/resources/${_dir}/*")
    list(FILTER _got EXCLUDE REGEX "^CMakeFiles/")
    set(_landed 0)
    foreach(_f IN LISTS _want)
        set(_found "")
        foreach(_g IN LISTS _got)
            if(_g MATCHES "resources/${_dir}/${_f}$")
                set(_found "${_off}/${_g}")
            endif()
        endforeach()
        if(_found)
            file(SHA256 "${_found}" _h1)
            file(SHA256 "${SOURCE_DIR}/resources/${_dir}/${_f}" _h2)
            if(_h1 STREQUAL _h2)
                math(EXPR _landed "${_landed} + 1")
            endif()
        else()
            message(STATUS "        not staged: resources/${_dir}/${_f}")
        endif()
    endforeach()
    check("_landed EQUAL _nwant" "every file of resources/${_dir} landed, byte for byte (${_landed} of ${_nwant})")
endforeach()

if(_failed GREATER 0)
    message(FATAL_ERROR "engine_only_configure: ${_failed} check(s) failed")
endif()
message(STATUS "engine_only_configure: all checks passed")
file(REMOVE_RECURSE "${SCRATCH}")
