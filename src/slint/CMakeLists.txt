# The Slint front end (spike): a SECOND window over the same C API table the ImGui window uses.
# Built only when the root passes CASCADE_BUILD_SLINT=ON AND the prebuilt Slint SDK is found
# (-DCMAKE_PREFIX_PATH=<Slint-cpp install>). It links nothing of the ImGui/GLFW stack and never
# changes the normal build: with the option OFF, or the SDK absent, this directory is a no-op.
#
# Slint integration (docs.slint.dev/latest/docs/cpp/cmake, verified 2026-11-02): the binary
# packages need no Rust toolchain; CMake >= 3.21, a C++20 compiler; find_package(Slint) then
# slint_target_sources() compiles the .slint files ahead of time into generated C++.

find_package(Slint QUIET)
if(NOT Slint_FOUND)
    message(STATUS "foxsdr-slint: Slint SDK not found (set CMAKE_PREFIX_PATH to the Slint-cpp install) - not built")
    return()
endif()
message(STATUS "foxsdr-slint: Slint ${Slint_VERSION} at ${Slint_DIR}")

add_executable(foxsdr-slint main.cpp)
target_compile_features(foxsdr-slint PRIVATE cxx_std_20)
# The engine (EngineService, the C API table) is the same library the ImGui window and foxsdr-engine use.
target_link_libraries(foxsdr-slint PRIVATE Slint::Slint cascade_engine)
target_include_directories(foxsdr-slint PRIVATE
    "${CMAKE_SOURCE_DIR}/src"
    "${CMAKE_SOURCE_DIR}/third_party/foxsdr_api")
slint_target_sources(foxsdr-slint ui/main.slint)

# The SDK's runtime DLL must sit beside the executable on Windows. Slint::Slint is an INTERFACE target
# (no TARGET_FILE), so the DLL is taken by its place in the SDK: <prefix>/lib/slint_cpp.dll, two levels
# above Slint_DIR (<prefix>/lib/cmake/Slint).
if(WIN32)
    set(_slint_dll "${Slint_DIR}/../../slint_cpp.dll")
    if(EXISTS "${_slint_dll}")
        add_custom_command(TARGET foxsdr-slint POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_slint_dll}" $<TARGET_FILE_DIR:foxsdr-slint>
            COMMENT "foxsdr-slint: copying slint_cpp.dll beside the executable")
    else()
        message(WARNING "foxsdr-slint: slint_cpp.dll not found at ${_slint_dll}; put it on PATH to run")
    endif()
endif()
