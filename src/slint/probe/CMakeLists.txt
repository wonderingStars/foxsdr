# THE CLIP PROBE (docs/engine-stage4.md 6.62): foxsdr-slint built from a COPY of ui/ whose Text elements print a line through Slint's debug() when the text does not fit its box
# (tools/slint-clip-probe.py says exactly when). Built only on request - cmake -DFOXSDR_SLINT_CLIP_PROBE=ON, then --target foxsdr-slint-probe - and never part of ALL, the installer or the
# release: it exists for tools/slint_clip_sweep.py, which runs it through the drive harness (FOXSDR_DRIVE_EXE) for every page in every language and reports the items that do not fit.
#
# It is a directory of its own because slint_target_sources writes <binary dir>/main.h and the window's own target already owns that name in the parent directory: here the same name is the
# probe's header, found first by the probe's translation units through this directory's include path. The probe copy is made at CONFIGURE time and CMAKE_CONFIGURE_DEPENDS makes an edit to
# any ui/*.slint (or to the transform) re-run the configure, so a build of the probe is never older than the ui it was made from.
set(probe_ui "${CMAKE_CURRENT_BINARY_DIR}/ui")
file(GLOB probe_ui_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/../ui/*.slint")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${probe_ui_sources} "${CMAKE_SOURCE_DIR}/tools/slint-clip-probe.py")
execute_process(COMMAND "${Python3_EXECUTABLE}" -I "${CMAKE_SOURCE_DIR}/tools/slint-clip-probe.py" "${CMAKE_CURRENT_SOURCE_DIR}/../ui" "${probe_ui}"
                RESULT_VARIABLE probe_transform_result OUTPUT_VARIABLE probe_transform_out ERROR_VARIABLE probe_transform_err)
if(NOT probe_transform_result EQUAL 0)
    message(FATAL_ERROR "foxsdr-slint-probe: the ui transform failed: ${probe_transform_out} ${probe_transform_err}")
endif()
string(STRIP "${probe_transform_out}" probe_transform_out)
message(STATUS "${probe_transform_out}")

# The window's own sources, by absolute path (the list in the parent is relative to the parent).
set(probe_sources "")
foreach(s ${foxsdr_slint_sources})
    if(IS_ABSOLUTE "${s}")
        list(APPEND probe_sources "${s}")
    else()
        list(APPEND probe_sources "${CMAKE_CURRENT_SOURCE_DIR}/../${s}")
    endif()
endforeach()
if(WIN32)
    add_executable(foxsdr-slint-probe EXCLUDE_FROM_ALL WIN32 ${probe_sources} "${CMAKE_SOURCE_DIR}/resources/icon/foxsdr.rc")
    if(MSVC)
        target_link_options(foxsdr-slint-probe PRIVATE /ENTRY:mainCRTStartup)
    endif()
else()
    add_executable(foxsdr-slint-probe EXCLUDE_FROM_ALL ${probe_sources})
endif()
target_compile_features(foxsdr-slint-probe PRIVATE cxx_std_20)
target_compile_definitions(foxsdr-slint-probe PRIVATE FOXSDR_SLINT_PROBE=1)   # main.cpp reads every Text's description at the end of a bounded run
target_link_libraries(foxsdr-slint-probe PRIVATE Slint::Slint cascade_engine)
# the same OpenGL the window links (TrackInfoCache's photo cache: src/slint/CMakeLists.txt, "whose default texture hooks are OpenGL calls"); the probe's link line had not followed
if(WIN32)
    target_link_libraries(foxsdr-slint-probe PRIVATE opengl32)
else()
    find_package(OpenGL QUIET)
    if(OpenGL_FOUND)
        target_link_libraries(foxsdr-slint-probe PRIVATE OpenGL::GL)
    endif()
endif()
target_include_directories(foxsdr-slint-probe PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/.."
    "${CMAKE_SOURCE_DIR}/src"
    "${CMAKE_SOURCE_DIR}/third_party/foxsdr_api"
    "${CMAKE_SOURCE_DIR}/third_party/glfw/deps"
    "${CMAKE_SOURCE_DIR}/third_party/imgui")   # HEADER ONLY, as the window's own target has it since the radar scope (gui/aircraft_icons.hpp includes imgui.h); the probe's include list had not followed
set_source_files_properties("${CMAKE_SOURCE_DIR}/src/gui/font_probe.cpp" PROPERTIES INCLUDE_DIRECTORIES "${CMAKE_SOURCE_DIR}/third_party/imgui")
set_property(TARGET foxsdr-slint-probe PROPERTY SLINT_EMBED_RESOURCES embed-files)
if(Python3_Interpreter_FOUND)
    # The Slint compiler looks for <target name>.po: the same catalogues under the probe's own name, in a folder of their own.
    set(probe_po_dir "${CMAKE_CURRENT_BINARY_DIR}/translations")
    set(probe_po_stamp "${probe_po_dir}/translations.stamp")
    add_custom_command(
        OUTPUT "${probe_po_stamp}"
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/slint-po.py"
                "${CMAKE_CURRENT_SOURCE_DIR}/../ui" "${CMAKE_SOURCE_DIR}/resources/lang" "${probe_po_dir}"
                --domain foxsdr-slint-probe --stamp "${probe_po_stamp}" --quiet
        DEPENDS "${CMAKE_SOURCE_DIR}/tools/slint-po.py" ${foxsdr_slint_catalogues} ${probe_ui_sources}
        COMMENT "foxsdr-slint-probe: translations from resources/lang"
        VERBATIM)
    add_custom_target(foxsdr-slint-probe-translations DEPENDS "${probe_po_stamp}")
    add_dependencies(foxsdr-slint-probe foxsdr-slint-probe-translations)
    set_property(TARGET foxsdr-slint-probe PROPERTY SLINT_BUNDLE_TRANSLATIONS "${probe_po_dir}")
    set_property(TARGET foxsdr-slint-probe PROPERTY SLINT_NO_DEFAULT_TRANSLATION_CONTEXT ON)
endif()
set(SLINT_COMPILER_ENV_VARS SLINT_EMIT_DEBUG_INFO=1)
slint_target_sources(foxsdr-slint-probe "${probe_ui}/main.slint")
unset(SLINT_COMPILER_ENV_VARS)
if(Python3_Interpreter_FOUND)
    add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/main.h" APPEND DEPENDS "${probe_po_stamp}")
endif()
# Beside foxsdr-slint.exe, so the probe finds the same slint_cpp.dll, SoapySDR.dll, softgl and resources/bandplans the window does.
set_target_properties(foxsdr-slint-probe PROPERTIES RUNTIME_OUTPUT_DIRECTORY "$<TARGET_FILE_DIR:foxsdr-slint>")
foxsdr_slint_copy_dll(foxsdr-slint-probe)
add_dependencies(foxsdr-slint-probe foxsdr_slint_stage_resources)
