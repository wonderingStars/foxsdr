# The SDK's runtime DLL must sit beside every executable that links Slint on Windows. Slint::Slint is an
# INTERFACE target (no TARGET_FILE), so the DLL is taken by its place in the SDK: <prefix>/lib/slint_cpp.dll,
# two levels above Slint_DIR (<prefix>/lib/cmake/Slint).
function(foxsdr_slint_copy_dll target)
    if(NOT WIN32)
        return()
    endif()
    set(_slint_dll "${Slint_DIR}/../../slint_cpp.dll")
    if(EXISTS "${_slint_dll}")
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_slint_dll}" $<TARGET_FILE_DIR:${target}>
            COMMENT "${target}: copying slint_cpp.dll beside the executable")
    else()
        message(WARNING "${target}: slint_cpp.dll not found at ${_slint_dll}; put it on PATH to run")
    endif()
endfunction()
