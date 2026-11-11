# The ExtIO helper process (row R8). Included from tests/CMakeLists.txt today (so the tests build it); to ship it, add
#   add_subdirectory(tools/extio_helper)
# to the root CMakeLists.txt and stage extio_helper64.exe (and extio_helper32.exe, built with -A Win32) beside cascade.exe.
# STANDALONE: it needs only the header source/extio_protocol.hpp and the Windows SDK, so a 32-bit build is configured on its own:
#   cmake -S tools/extio_helper -B build32 -A Win32 && cmake --build build32 --config Release
cmake_minimum_required(VERSION 3.20)
if(NOT CMAKE_PROJECT_NAME)
    project(extio_helper CXX)
endif()
if(WIN32)
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(_extio_name extio_helper64)
    else()
        set(_extio_name extio_helper32)
    endif()
    add_executable(${_extio_name} "${CMAKE_CURRENT_LIST_DIR}/extio_helper.cpp")
    target_include_directories(${_extio_name} PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../../src")
    target_compile_features(${_extio_name} PRIVATE cxx_std_20)
    target_compile_definitions(${_extio_name} PRIVATE UNICODE _UNICODE)
    if(MSVC)
        target_compile_options(${_extio_name} PRIVATE /W4 /EHsc)
        target_link_options(${_extio_name} PRIVATE /SUBSYSTEM:WINDOWS /ENTRY:wmainCRTStartup)   # no console window; wmain
    endif()
    set_target_properties(${_extio_name} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/extio/$<CONFIG>")
    set(CASCADE_EXTIO_HELPER_TARGET ${_extio_name} PARENT_SCOPE)
endif()
