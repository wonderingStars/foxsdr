# Source scan: the plugin must not write to stdout or stderr (the host captures process stderr into its log and its
# uploaded reports) and must not write to the host's settings (every settings_set value goes to the config file).
# Run by CTest:  cmake -DSOURCE=<file> -P scan_sinks.cmake
# The comment text of the source is removed first, so a sentence that names one of these is not a finding.
#
# SPDX-License-Identifier: MIT
if(NOT DEFINED SOURCE)
    message(FATAL_ERROR "scan_sinks.cmake: pass -DSOURCE=<file to scan>")
endif()
file(READ "${SOURCE}" _text)
# line comments, then block comments
string(REGEX REPLACE "//[^\n]*" "" _text "${_text}")
string(REGEX REPLACE "/\\*([^*]|\\*+[^*/])*\\*+/" "" _text "${_text}")

set(_forbidden
    "stderr" "stdout" "[^n]printf *\\(" "fprintf" "puts *\\(" "fputs" "perror" "std::cout" "std::cerr" "std::clog"
    "settings_set" "settings_get" "OutputDebugString")
set(_found "")
foreach(_pattern IN LISTS _forbidden)
    if(_text MATCHES "${_pattern}")
        list(APPEND _found "${_pattern}")
    endif()
endforeach()
if(_found)
    message(FATAL_ERROR "scan_sinks: ${SOURCE} uses a sink the host captures or persists: ${_found}")
endif()
message(STATUS "scan_sinks: ${SOURCE} has no stdout/stderr/settings sink")
