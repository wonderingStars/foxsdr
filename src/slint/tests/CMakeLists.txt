# The Slint front end's tests (docs/engine-stage4.md 6.36): headless renders of every component specimen and
# of the main screen, probed against the artboards, plus the host's own logic. They need ONLY the Slint SDK -
# not the engine - so this directory configures on its own as well as under the root build:
#
#   cmake -S src/slint/tests -B build-ui -G "Visual Studio 17 2022" -A x64 -DCMAKE_PREFIX_PATH=C:/Users/steve/slint-cpp-1.18.1
#   cmake --build build-ui --config Release --target test_slint_theme
#   ctest --test-dir build-ui -C Release -L slint
#
# Every test here carries the ctest label `slint`. Under the root build they are registered only when
# CASCADE_BUILD_SLINT is ON and the SDK was found, so the normal build never sees them.
#
# Registration is by name, so several people can add tests without editing this file:
#   test_slint_<family>.cpp     renders ../ui/specimens/<family>_specimen.slint (skipped, with a note, until the
#                               specimen exists)
#   test_slint_main_screen.cpp  renders ../ui/main.slint
#   test_slint_host_*.cpp       the host's pure logic (../host_logic.cpp), no Slint
#   test_slint_house_rules.cpp  scans ../ui for the house rules (no colour literals outside the theme, ...)
cmake_minimum_required(VERSION 3.21)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(foxsdr-slint-tests CXX)
    find_package(Slint REQUIRED)
    enable_testing()
    include("${CMAKE_CURRENT_SOURCE_DIR}/../SlintDll.cmake")
endif()

set(FOXSDR_SLINT_UI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../ui")
set(FOXSDR_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../..")

# The harness: the headless platform, pixel probes and the PNG writer (stb_image_write, public domain, as vendored
# with GLFW). The repo's own CHECK macros come from tests/test_check.hpp.
add_library(slint_render_harness STATIC render_harness.cpp)
target_compile_features(slint_render_harness PUBLIC cxx_std_20)
target_link_libraries(slint_render_harness PUBLIC Slint::Slint)
target_include_directories(slint_render_harness PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${FOXSDR_REPO_ROOT}/tests"
    "${FOXSDR_REPO_ROOT}/third_party/glfw/deps")

# A render test: one executable, one .slint entry file (its specimen sheet or the main screen), label `slint`.
function(foxsdr_slint_ui_test name slint_file)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE slint_render_harness)
    set_property(TARGET ${name} PROPERTY SLINT_EMBED_RESOURCES embed-files)   # fonts and icons inside the test binary, as in the product
    slint_target_sources(${name} "${slint_file}")
    foxsdr_slint_copy_dll(${name})
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES LABELS "slint")
endfunction()

# A logic test: no Slint at all - the host's pure functions, or a scan of the sources. Extra arguments after the
# name are more sources; FOXSDR_LOGIC_TEST_ARGS_<name> are its command-line arguments.
function(foxsdr_slint_logic_test name)
    add_executable(${name} ${name}.cpp ${ARGN})
    target_compile_features(${name} PRIVATE cxx_std_20)
    target_include_directories(${name} PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/.."
        "${FOXSDR_REPO_ROOT}/src"
        "${FOXSDR_REPO_ROOT}/tests"
        "${FOXSDR_REPO_ROOT}/third_party/foxsdr_api")
    add_test(NAME ${name} COMMAND ${name} ${FOXSDR_LOGIC_TEST_ARGS_${name}})
    set_tests_properties(${name} PROPERTIES LABELS "slint")
endfunction()

set(FOXSDR_LOGIC_TEST_ARGS_test_slint_house_rules "${FOXSDR_SLINT_UI_DIR}")

file(GLOB _foxsdr_slint_test_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/test_slint_*.cpp")
foreach(src ${_foxsdr_slint_test_sources})
    get_filename_component(name "${src}" NAME_WE)
    string(REGEX REPLACE "^test_slint_" "" family "${name}")
    if(family MATCHES "^host_")
        # test_slint_host_<x>.cpp compiles the host's logic and, when one exists, the kit unit of that name (the plugin kit,
        # docs 6.37): ../<x>_logic.cpp (kit_logic for host_kit) or ../<x>_compositor.cpp (map_compositor for host_map).
        string(REGEX REPLACE "^host_" "" unit "${family}")
        set(_extra "${CMAKE_CURRENT_SOURCE_DIR}/../host_logic.cpp")
        if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../${unit}_logic.cpp")
            list(APPEND _extra "${CMAKE_CURRENT_SOURCE_DIR}/../${unit}_logic.cpp")
        endif()
        if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../${unit}_compositor.cpp")
            list(APPEND _extra "${CMAKE_CURRENT_SOURCE_DIR}/../${unit}_compositor.cpp")
        endif()
        foxsdr_slint_logic_test(${name} ${_extra})
    elseif(family STREQUAL "house_rules")
        foxsdr_slint_logic_test(${name})
    elseif(family STREQUAL "main_screen")
        foxsdr_slint_ui_test(${name} "${FOXSDR_SLINT_UI_DIR}/main.slint")
    elseif(EXISTS "${FOXSDR_SLINT_UI_DIR}/specimens/${family}_specimen.slint")
        foxsdr_slint_ui_test(${name} "${FOXSDR_SLINT_UI_DIR}/specimens/${family}_specimen.slint")
    else()
        message(STATUS "foxsdr-slint tests: ${name} has no specimens/${family}_specimen.slint yet - not registered")
    endif()
endforeach()
