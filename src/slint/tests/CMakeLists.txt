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
    # The TRANSMIT page's test finds its controls by their accessible labels through the element tree (as the window's scripted input does), which is empty unless the
    # compiler emitted the element debug info (src/slint/CMakeLists.txt says why the variable is handed over).
    if(name STREQUAL "test_slint_transmit")
        set(SLINT_COMPILER_ENV_VARS SLINT_EMIT_DEBUG_INFO=1)
    endif()
    slint_target_sources(${name} "${slint_file}")
    unset(SLINT_COMPILER_ENV_VARS)
    foxsdr_slint_copy_dll(${name})
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES LABELS "slint")
endfunction()

# The interface-language lookup (src/core/i18n.cpp, the engine's own, with its compiled-in catalogues): the host's sentences go through
# cascade::i18n::tr(), which hands its argument back while English is in force, so the logic tests below keep comparing English.
# Built once as a library so the three host tests share one compile of the 35 MB catalogue header.
add_library(slint_i18n STATIC "${FOXSDR_REPO_ROOT}/src/core/i18n.cpp")
target_compile_features(slint_i18n PUBLIC cxx_std_20)
target_include_directories(slint_i18n PUBLIC "${FOXSDR_REPO_ROOT}/src")
target_include_directories(slint_i18n SYSTEM PRIVATE "${FOXSDR_REPO_ROOT}/third_party/nlohmann_json/include")
if(MSVC)
    target_compile_options(slint_i18n PRIVATE /utf-8)
    target_compile_definitions(slint_i18n PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
endif()

# A logic test: no Slint at all - the host's pure functions, or a scan of the sources. Extra arguments after the
# name are more sources; FOXSDR_LOGIC_TEST_ARGS_<name> are its command-line arguments.
function(foxsdr_slint_logic_test name)
    add_executable(${name} ${name}.cpp ${ARGN})
    target_compile_features(${name} PRIVATE cxx_std_20)
    if(name MATCHES "^test_slint_host_")
        target_link_libraries(${name} PRIVATE slint_i18n)
    endif()
    target_include_directories(${name} PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/.."
        "${FOXSDR_REPO_ROOT}/src"
        "${FOXSDR_REPO_ROOT}/tests"
        "${FOXSDR_REPO_ROOT}/third_party/foxsdr_api"
        "${FOXSDR_REPO_ROOT}/third_party/imgui")   # header only: gui/scope_view.hpp includes imgui.h through gui/aircraft_icons.hpp (docs 6.57); nothing of ImGui is linked
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
        if(unit STREQUAL "system" OR unit STREQUAL "keydispatch")
            # The keyboard's dispatch (docs 6.56, ../key_dispatch.cpp) stands on the SYSTEM bank's key table, so both tests build the SYSTEM bank's logic with it.
            list(APPEND _extra "${CMAKE_CURRENT_SOURCE_DIR}/../key_dispatch.cpp")
            # The window's look (docs 6.61, ../look_logic.cpp): the Display body's effects are read through the rules that apply them.
            list(APPEND _extra "${CMAKE_CURRENT_SOURCE_DIR}/../look_logic.cpp")
            if(unit STREQUAL "keydispatch")
                list(APPEND _extra "${CMAKE_CURRENT_SOURCE_DIR}/../system_logic.cpp")
            endif()
            # The SYSTEM bank's logic (docs 6.39) quotes the core's pure units rather than copying them: the grants of the SIGNAL family's commands, the
            # country table, the feature-request and problem-report rules, the tester-code rules and the serial-port name rule. None of them includes ImGui.
            list(APPEND _extra
                "${CMAKE_CURRENT_SOURCE_DIR}/../signal_logic.cpp"
                "${FOXSDR_REPO_ROOT}/src/core/countries.cpp"
                "${FOXSDR_REPO_ROOT}/src/core/feature_request.cpp"
                "${FOXSDR_REPO_ROOT}/src/core/problem_report.cpp"
                "${FOXSDR_REPO_ROOT}/src/core/pasted_report.cpp"
                "${FOXSDR_REPO_ROOT}/src/core/tester_usage.cpp"
                "${FOXSDR_REPO_ROOT}/src/core/serial_port.cpp"
                "${FOXSDR_REPO_ROOT}/src/core/plugin_repo_data.cpp"
                "${FOXSDR_REPO_ROOT}/src/core/diag_log.cpp"
                "${CMAKE_CURRENT_SOURCE_DIR}/system_core_stubs.cpp")
        endif()
        if(unit STREQUAL "tools")
            # The TOOLS bank's logic (docs 6.40) quotes the core's pure units: the SIGNAL family's commands and grants, the airport table (airband_data, with its compiled-in
            # 8 MB table), the frequency-list rules (freq_import: cleanImportPath) with the list manager and file reader they stand on, the markers' text rules, and
            # the module census that says why the radar scope would be empty.
            list(APPEND _extra
                "${CMAKE_CURRENT_SOURCE_DIR}/../signal_logic.cpp"
                "${FOXSDR_REPO_ROOT}/src/core/airband_data.cpp"
                "${FOXSDR_REPO_ROOT}/src/core/freq_import.cpp"
                "${FOXSDR_REPO_ROOT}/src/core/freq_markers.cpp"
                "${FOXSDR_REPO_ROOT}/src/core/freq_manager.cpp"
                "${FOXSDR_REPO_ROOT}/src/core/file_read.cpp"
                "${FOXSDR_REPO_ROOT}/src/gui/module_census.cpp"
                "${CMAKE_CURRENT_SOURCE_DIR}/tools_core_stubs.cpp")
        endif()
        if(unit STREQUAL "extend")
            # The EXTEND bank's logic (phase 3, bank 4) quotes the core's pure units rather than copying them: the grants of the SIGNAL family's commands, the site-address validator and
            # labels (plugin_repo_data), the store's reasons (plugin_store_reasons: trStoredReason), with the add-all rule (add_all_plan.hpp), the first-open rule (store_first_open.hpp)
            # and the port range (server_ports.hpp) which are header-only. gui/plugin_store_view.cpp is NOT here: it draws with ImGui (extend_logic.hpp says what is mirrored).
            list(APPEND _extra
                "${CMAKE_CURRENT_SOURCE_DIR}/../signal_logic.cpp"
                "${FOXSDR_REPO_ROOT}/src/core/plugin_repo_data.cpp"
                "${FOXSDR_REPO_ROOT}/src/core/plugin_store_reasons.cpp")
        endif()
        if(unit STREQUAL "input")
            # The scripted input's parser and driver (docs 6.47): ../input_script.cpp, no Slint - the fake target in the test stands for the window.
            list(APPEND _extra "${CMAKE_CURRENT_SOURCE_DIR}/../input_script.cpp")
        endif()
        if(unit STREQUAL "transmit")
            # The TRANSMIT page's logic (docs 6.58) quotes the classic's ImGui-free key rules rather than copying them: gui/transmit_page (the key's frame rules are inline in the header,
            # formatTxPower and txMeterFraction are the .cpp) and the mode table (dsp/modulator: the names and captions), with the SIGNAL family's commands and grants.
            list(APPEND _extra
                "${CMAKE_CURRENT_SOURCE_DIR}/../signal_logic.cpp"
                "${FOXSDR_REPO_ROOT}/src/gui/transmit_page.cpp"
                "${FOXSDR_REPO_ROOT}/src/dsp/modulator.cpp")
        endif()
        if(unit STREQUAL "radar")
            # The radar scope's painter draws with the demod scope's Canvas (scope_compositor.cpp), which paints the demod tube with scope_logic.cpp's state object.
            list(APPEND _extra "${CMAKE_CURRENT_SOURCE_DIR}/../scope_logic.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/../scope_compositor.cpp")
        endif()
        if(unit STREQUAL "globe")
            # The globe views' rules (docs 6.65): globe_logic.cpp stands on the geometry (globe_scene.cpp, over the flat map's coastline rings in map_compositor.cpp) and the
            # planets (solar_ephemeris.cpp); nothing of Slint.
            list(APPEND _extra
                "${CMAKE_CURRENT_SOURCE_DIR}/../globe_scene.cpp"
                "${CMAKE_CURRENT_SOURCE_DIR}/../globe_draw.cpp"
                "${CMAKE_CURRENT_SOURCE_DIR}/../scene3d.cpp"
                "${CMAKE_CURRENT_SOURCE_DIR}/../view3d_prefs.cpp"
                "${CMAKE_CURRENT_SOURCE_DIR}/../solar_ephemeris.cpp"
                "${CMAKE_CURRENT_SOURCE_DIR}/../map_compositor.cpp")
        endif()
        if(unit STREQUAL "glyphs")
            # The glyph coverage (docs 6.62): the CJK face chooser over the classic's scripts.cpp and its stb_truetype probe, against the catalogues the engine carries (slint_i18n) and the
            # typefaces the window embeds. Nothing of Slint.
            list(APPEND _extra
                "${CMAKE_CURRENT_SOURCE_DIR}/../cjk_fonts.cpp"
                "${FOXSDR_REPO_ROOT}/src/core/scripts.cpp"
                "${FOXSDR_REPO_ROOT}/src/gui/font_probe.cpp")
        endif()
        if(unit STREQUAL "patch")
            # The PATCH page's logic (phase 3, the patch allowance) quotes the core's pure units rather than copying them: the grants of the SIGNAL family's commands, the airport table
            # (airband_data, for the tower blueprints), the presets' rules (patch_presets with the diagnostic log it reports a dropped entry to). patch_graph, patch_io, patch_plan,
            # patch_draft, patch_blueprints, patch_devices, patch_device_label, freq_converter and gui/patch_view_math.hpp are header-only and pure (no imgui.h).
            list(APPEND _extra
                "${CMAKE_CURRENT_SOURCE_DIR}/../signal_logic.cpp"
                "${FOXSDR_REPO_ROOT}/src/core/airband_data.cpp"
                "${FOXSDR_REPO_ROOT}/src/core/patch_presets.cpp"
                "${FOXSDR_REPO_ROOT}/src/core/diag_log.cpp")
        endif()
        if(unit MATCHES "3d$")
            # THE 3D VIEWS (docs 6.64): test_slint_host_<view>3d.cpp and test_slint_host_view3d.cpp compile the shared renderer and the remembered-view text with the view's own
            # <view>3d_logic.cpp / <view>3d_compositor.cpp (found above). The renderer's ramps quote host_logic's heat(), which is already in the list.
            list(APPEND _extra "${CMAKE_CURRENT_SOURCE_DIR}/../scene3d.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/../view3d_prefs.cpp")
        endif()
        foxsdr_slint_logic_test(${name} ${_extra})
        if(unit STREQUAL "glyphs")
            target_include_directories(${name} PRIVATE "${FOXSDR_REPO_ROOT}/third_party/imgui")
            target_compile_definitions(${name} PRIVATE FOXSDR_GLYPH_FONT_DIR="${FOXSDR_REPO_ROOT}/third_party/fonts")
        endif()
        if(unit STREQUAL "tools")
            target_include_directories(${name} SYSTEM PRIVATE "${FOXSDR_REPO_ROOT}/third_party/nlohmann_json/include")
            if(MSVC)
                target_compile_options(${name} PRIVATE /utf-8)
                target_compile_definitions(${name} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
            endif()
        endif()
        if(unit STREQUAL "patch")
            target_include_directories(${name} SYSTEM PRIVATE "${FOXSDR_REPO_ROOT}/third_party/nlohmann_json/include")
            if(MSVC)
                target_compile_options(${name} PRIVATE /utf-8)
                target_compile_definitions(${name} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
            endif()
        endif()
        if(unit STREQUAL "extend")
            target_include_directories(${name} SYSTEM PRIVATE "${FOXSDR_REPO_ROOT}/third_party/nlohmann_json/include")
            if(MSVC)
                target_compile_options(${name} PRIVATE /utf-8)
                target_compile_definitions(${name} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
            endif()
        endif()
        if(unit STREQUAL "system" OR unit STREQUAL "keydispatch")
            target_include_directories(${name} SYSTEM PRIVATE "${FOXSDR_REPO_ROOT}/third_party/nlohmann_json/include")
            if(MSVC)
                target_compile_options(${name} PRIVATE /utf-8)
                target_compile_definitions(${name} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
            endif()
            if(WIN32)
                target_link_libraries(${name} PRIVATE winhttp)
            endif()
        endif()
    elseif(family STREQUAL "globe_scene")
        # The globe views' geometry (docs 6.65, ../globe_scene.cpp): pure maths over the flat map's coastline rings (../map_compositor.cpp), no Slint.
        foxsdr_slint_logic_test(${name} "${CMAKE_CURRENT_SOURCE_DIR}/../globe_scene.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/../map_compositor.cpp")
    elseif(family STREQUAL "scene3d")
        # THE SHARED 3D SCENE FRAMEWORK (docs 6.64): the CPU renderer, headless. Its colour ramps quote host_logic's heat(), which speaks through i18n.
        foxsdr_slint_logic_test(${name} "${CMAKE_CURRENT_SOURCE_DIR}/../scene3d.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/../host_logic.cpp")
        target_link_libraries(${name} PRIVATE slint_i18n)
    elseif(family STREQUAL "scene3d_well")
        # The 3D well in a window (docs 6.64): the specimen sheet is the Slint half, the renderer and its adapter (scene3d_slint.hpp) the C++ half.
        foxsdr_slint_ui_test(${name} "${FOXSDR_SLINT_UI_DIR}/specimens/scene3d_well_specimen.slint")
        target_sources(${name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../scene3d.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/../host_logic.cpp")
        target_include_directories(${name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/.." "${FOXSDR_REPO_ROOT}/src" "${FOXSDR_REPO_ROOT}/third_party/foxsdr_api")
        target_link_libraries(${name} PRIVATE slint_i18n)
    elseif(family STREQUAL "house_rules")
        foxsdr_slint_logic_test(${name})
    elseif(family STREQUAL "main_screen")
        foxsdr_slint_ui_test(${name} "${FOXSDR_SLINT_UI_DIR}/main.slint")
    elseif(family STREQUAL "cjk")
        # The CJK glyphs of the window (docs 6.62): the specimen's one line of text rendered per character, once per language (the registered faces accumulate in a process, so each
        # language is its own run). The unit chooses the system faces exactly as main.cpp does (cjk_fonts.cpp over the classic's scripts.cpp and the stb_truetype probe).
        add_executable(${name} ${name}.cpp "${CMAKE_CURRENT_SOURCE_DIR}/../cjk_fonts.cpp" "${FOXSDR_REPO_ROOT}/src/core/scripts.cpp" "${FOXSDR_REPO_ROOT}/src/gui/font_probe.cpp")
        target_link_libraries(${name} PRIVATE slint_render_harness slint_i18n)
        target_include_directories(${name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/.." "${FOXSDR_REPO_ROOT}/src" "${FOXSDR_REPO_ROOT}/third_party/imgui")
        set_property(TARGET ${name} PROPERTY SLINT_EMBED_RESOURCES embed-files)
        slint_target_sources(${name} "${FOXSDR_SLINT_UI_DIR}/specimens/${family}_specimen.slint")
        foxsdr_slint_copy_dll(${name})
        foreach(lang ja ko zh-CN zh-TW)
            add_test(NAME ${name}_${lang} COMMAND ${name} ${lang})
            set_tests_properties(${name}_${lang} PROPERTIES LABELS "slint")
        endforeach()
    elseif(EXISTS "${FOXSDR_SLINT_UI_DIR}/specimens/${family}_specimen.slint")
        foxsdr_slint_ui_test(${name} "${FOXSDR_SLINT_UI_DIR}/specimens/${family}_specimen.slint")
    else()
        message(STATUS "foxsdr-slint tests: ${name} has no specimens/${family}_specimen.slint yet - not registered")
    endif()
endforeach()
