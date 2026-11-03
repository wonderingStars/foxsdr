# The Slint front end: a SECOND window over the same C API table the ImGui window uses (docs/engine-stage4.md
# 6.34 the spike, 6.36 phase 1 of the port). Built only when the root passes CASCADE_BUILD_SLINT=ON AND the
# prebuilt Slint SDK is found (-DCMAKE_PREFIX_PATH=<Slint-cpp install>). It links nothing of the ImGui/GLFW
# stack and never changes the normal build: with the option OFF, or the SDK absent, this directory is a no-op.
#
# Slint integration (docs.slint.dev/latest/docs/cpp/cmake, verified 2026-11-02): the binary packages need no
# Rust toolchain; CMake >= 3.21, a C++20 compiler; find_package(Slint) then slint_target_sources() compiles the
# .slint files ahead of time into generated C++.

find_package(Slint QUIET)
if(NOT Slint_FOUND)
    message(STATUS "foxsdr-slint: Slint SDK not found (set CMAKE_PREFIX_PATH to the Slint-cpp install) - not built")
    return()
endif()
message(STATUS "foxsdr-slint: Slint ${Slint_VERSION} at ${Slint_DIR}")
include("${CMAKE_CURRENT_SOURCE_DIR}/SlintDll.cmake")

# The Royalty-free licence's attribution: the AboutSlint widget in an About dialog reachable from the top-level
# menu (the window's brand menu > About FoxSDR). It is built into every build of this window - a distributed
# build without it would breach the licence - so it is not an option here.

add_executable(foxsdr-slint main.cpp host_logic.cpp host_duties.cpp)
target_compile_features(foxsdr-slint PRIVATE cxx_std_20)
# The engine (EngineService, the C API table) is the same library the ImGui window and foxsdr-engine use; the
# window itself only ever talks to the table.
target_link_libraries(foxsdr-slint PRIVATE Slint::Slint cascade_engine)
target_include_directories(foxsdr-slint PRIVATE
    "${CMAKE_SOURCE_DIR}/src"
    "${CMAKE_SOURCE_DIR}/third_party/foxsdr_api"
    "${CMAKE_SOURCE_DIR}/third_party/glfw/deps")   # stb_image_write for the screenshot key
# Fonts and icons are compiled INTO the executable (the SDK default would embed a path on this machine).
set_property(TARGET foxsdr-slint PROPERTY SLINT_EMBED_RESOURCES embed-files)
slint_target_sources(foxsdr-slint ui/main.slint)
foxsdr_slint_copy_dll(foxsdr-slint)

# The render and host-logic tests (ctest label `slint`); see tests/CMakeLists.txt.
add_subdirectory(tests)

# The host's housekeeping (host_duties.*): tested without Slint, a window, a network or the user's profile, so it is built and
# registered HERE, against the engine library, and not in tests/CMakeLists.txt, whose glob registers only the tests that need
# nothing but the Slint SDK (it skips test_slint_duties.cpp by name: there is no duties specimen sheet). It also reads main.cpp's
# text, to hold the order of the host's calls (the same way tests/test_engine_service_order.cpp holds src/main.cpp's).
add_executable(test_slint_duties tests/test_slint_duties.cpp host_duties.cpp)
target_compile_features(test_slint_duties PRIVATE cxx_std_20)
target_link_libraries(test_slint_duties PRIVATE cascade_engine)
target_include_directories(test_slint_duties PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_SOURCE_DIR}/src"
    "${CMAKE_SOURCE_DIR}/tests"
    "${CMAKE_SOURCE_DIR}/third_party/foxsdr_api")
target_compile_definitions(test_slint_duties PRIVATE FOXSDR_SLINT_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
add_test(NAME test_slint_duties COMMAND test_slint_duties)
set_tests_properties(test_slint_duties PROPERTIES LABELS "slint" TIMEOUT 120)
