# fence_probe.cmake - runs one nested build of a fence probe and judges it (tests/CMakeLists.txt, "THE FENCE'S COMPILE PROOF").
#
#   MODE=DENIED   the build must FAIL, and the compiler's output must say the interface may not include HEADER - not merely "failed":
#                 a probe that broke for any other reason (a typo, a missing include path) is not the fence working.
#   MODE=BUILDS   the build must succeed (the controls: the same unit without the fence, and what the interface may see with it).
#
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
foreach(_v MODE BUILD_DIR TARGET HEADER)
    if(NOT DEFINED ${_v})
        message(FATAL_ERROR "fence_probe.cmake: ${_v} is not set")
    endif()
endforeach()
set(_args --build "${BUILD_DIR}" --target "${TARGET}")
if(CONFIG AND NOT CONFIG STREQUAL "")
    list(APPEND _args --config "${CONFIG}")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" ${_args}
    RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
set(_all "${_out}\n${_err}")
if(MODE STREQUAL "DENIED")
    if(_rc EQUAL 0)
        message(FATAL_ERROR "FENCE BROKEN: ${TARGET} compiled, so the interface can include <${HEADER}>\n${_all}")
    endif()
    if(NOT _all MATCHES "cascade_interface may not include <${HEADER}>")
        message(FATAL_ERROR "${TARGET} failed, but not with the fence's own error for <${HEADER}> - the probe is broken, not the fence working:\n${_all}")
    endif()
    string(REGEX MATCH "[^\n]*cascade_interface may not include <${HEADER}>[^\n]*" _line "${_all}")
    message(STATUS "denied, as it must be: ${_line}")
elseif(MODE STREQUAL "BUILDS")
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "${TARGET} did not build (exit ${_rc}):\n${_all}")
    endif()
    message(STATUS "${TARGET} builds")
else()
    message(FATAL_ERROR "fence_probe.cmake: unknown MODE ${MODE}")
endif()
