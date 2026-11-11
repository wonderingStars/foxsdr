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

# THE PLUGIN KIT (docs 6.37): kit_logic (the pure rules), plugin_kit (the port, the duties, the commands), map_compositor (the map's
# picture), kit_publish (the page models). The track-info cache is the ImGui window's own GL-free class, compiled in here rather than
# copied (src/gui/track_info_cache.cpp includes nothing of ImGui); the basemap client is the kit's, because BasemapCache calls GL.
# THE WINDOWS SUBSYSTEM AND THE ICON, as cascade.exe has them (the root CMakeLists.txt, "THE WINDOWS SUBSYSTEM (0.99.69)"):
# foxsdr-slint.exe is the application the installer ships (installer/cascade.iss, SlintIsApp), so a launch from the Start menu or
# the desktop must not bring a console window with it - the image is WIN32 (/SUBSYSTEM:WINDOWS) and keeps `int main` through
# /ENTRY:mainCRTStartup; the terminal comes back for --version, the tool modes and the bounded runs through the same bootProcess
# and its AttachConsole (core/console_owner.hpp), so a harness that reads pipes (ctest, Git Bash, release.ps1) sees every line.
# The icon resource is the one file cascade.exe embeds, so Explorer, the shortcuts and Add/Remove Programs show the fox.
set(foxsdr_slint_sources main.cpp host_logic.cpp host_duties.cpp kit_logic.cpp plugin_kit.cpp kit_publish.cpp map_compositor.cpp signal_logic.cpp signal_publish.cpp system_logic.cpp system_publish.cpp key_dispatch.cpp look_logic.cpp cjk_fonts.cpp tools_logic.cpp tools_publish.cpp extend_logic.cpp extend_publish.cpp patch_logic.cpp patch_publish.cpp transmit_logic.cpp transmit_publish.cpp scope_logic.cpp scope_compositor.cpp scope_publish.cpp radar_logic.cpp radar_compositor.cpp scene3d.cpp view3d_prefs.cpp waterfall3d_logic.cpp waterfall3d_compositor.cpp radar3d_logic.cpp radar3d_compositor.cpp tunnel3d_logic.cpp tunnel3d_compositor.cpp survey3d_logic.cpp survey3d_compositor.cpp sonde3d_logic.cpp sonde3d_compositor.cpp view3d_publish.cpp input_script.cpp
    "${CMAKE_SOURCE_DIR}/src/gui/track_info_cache.cpp"
    "${CMAKE_SOURCE_DIR}/src/gui/transmit_page.cpp"
    "${CMAKE_SOURCE_DIR}/src/gui/track_photo_cache.cpp"
    "${CMAKE_SOURCE_DIR}/src/gui/module_census.cpp"
    "${CMAKE_SOURCE_DIR}/src/gui/font_probe.cpp")
# THE GLOBE VIEWS (docs 6.65): the geometry, the planets, the rules, what each view draws, its picture and its publisher. Their tests are registered by tests/CMakeLists.txt.
# THE RADIO WORKSPACE (docs/multi-radio-records/screen-s3.md): the logic, the publisher into ui/workspace.slint, and the test seam (made-up radios for a bounded run only)
list(APPEND foxsdr_slint_sources workspace_logic.cpp workspace_publish.cpp workspace_testseam.cpp)
list(APPEND foxsdr_slint_sources globe_scene.cpp solar_ephemeris.cpp globe_logic.cpp globe_draw.cpp globe_compositor.cpp globe_publish.cpp)
if(WIN32)
    enable_language(RC)
    add_executable(foxsdr-slint WIN32 ${foxsdr_slint_sources} "${CMAKE_SOURCE_DIR}/resources/icon/foxsdr.rc")
    if(MSVC)
        target_link_options(foxsdr-slint PRIVATE /ENTRY:mainCRTStartup)
    endif()
else()
    add_executable(foxsdr-slint ${foxsdr_slint_sources})
endif()
target_compile_features(foxsdr-slint PRIVATE cxx_std_20)
# The engine (EngineService, the C API table) is the same library the ImGui window and foxsdr-engine use; the
# window itself only ever talks to the table.
target_link_libraries(foxsdr-slint PRIVATE Slint::Slint cascade_engine)
# TrackInfoCache owns a TrackPhotoCache (the target photos, 0.99.98), whose default texture hooks are OpenGL calls: this window never asks the
# plugin for a photo, so none runs, but the class links them.
if(WIN32)
    target_link_libraries(foxsdr-slint PRIVATE opengl32)
else()
    find_package(OpenGL QUIET)
    if(OpenGL_FOUND)
        target_link_libraries(foxsdr-slint PRIVATE OpenGL::GL)
    endif()
endif()
target_include_directories(foxsdr-slint PRIVATE
    "${CMAKE_SOURCE_DIR}/src"
    "${CMAKE_SOURCE_DIR}/third_party/foxsdr_api"
    "${CMAKE_SOURCE_DIR}/third_party/glfw/deps"     # stb_image_write for the screenshot key
    "${CMAKE_SOURCE_DIR}/third_party/imgui")        # HEADER ONLY: gui/scope_view.hpp (the radar scope's pure maths, docs 6.57) includes gui/aircraft_icons.hpp which includes imgui.h; nothing of ImGui is linked
# The CJK face probe (docs 6.62) is the classic window's own stb_truetype reader, which includes the vendored header by its bare name.
set_source_files_properties("${CMAKE_SOURCE_DIR}/src/gui/font_probe.cpp" PROPERTIES INCLUDE_DIRECTORIES "${CMAKE_SOURCE_DIR}/third_party/imgui")
# Fonts and icons are compiled INTO the executable (the SDK default would embed a path on this machine).
set_property(TARGET foxsdr-slint PROPERTY SLINT_EMBED_RESOURCES embed-files)

# THE 33 LANGUAGES, through Slint's bundled translations. The ImGui window's catalogues (resources/lang/<code>.json, keyed by the
# English text) are the one source of the words: tools/slint-po.py reads every @tr(...) in ui/ (not the specimens' test sheets), looks
# each one up in each catalogue and writes <build>/translations/<code>/LC_MESSAGES/foxsdr-slint.po, and the Slint compiler bundles those
# into the executable (--bundle-translations), so a translation is compiled in exactly as the ImGui window's catalogues are and no
# packager has a loose file to leave out. SLINT_NO_DEFAULT_TRANSLATION_CONTEXT is ON because the .po files carry no msgctxt: the
# compiler would otherwise key every string on its component name and no catalogue entry would ever match. The domain is the target's
# own name, foxsdr-slint, which is the file name the generator writes. Without Python the window is built English-only (the bundle
# property is left unset; select_bundled_translation then answers "not bundled"), said once here at configure time.
find_package(Python3 QUIET COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    set(foxsdr_slint_po_dir "${CMAKE_CURRENT_BINARY_DIR}/translations")
    set(foxsdr_slint_po_stamp "${foxsdr_slint_po_dir}/translations.stamp")
    # CONFIGURE_DEPENDS: a catalogue or a .slint file added later re-runs the glob (and so this command's DEPENDS) at the next build.
    file(GLOB foxsdr_slint_catalogues CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/resources/lang/*.json")
    file(GLOB_RECURSE foxsdr_slint_ui_files CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/ui/*.slint")
    list(FILTER foxsdr_slint_ui_files EXCLUDE REGEX "/ui/specimens/")
    # The generator touches the stamp only when a .po's bytes changed (or the stamp is missing), so an edit to a catalogue or a .slint
    # file that changes no foxsdr-slint string leaves the stamp alone and the Slint compiler below does not run again.
    add_custom_command(
        OUTPUT "${foxsdr_slint_po_stamp}"
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/slint-po.py"
                "${CMAKE_CURRENT_SOURCE_DIR}/ui" "${CMAKE_SOURCE_DIR}/resources/lang" "${foxsdr_slint_po_dir}"
                --stamp "${foxsdr_slint_po_stamp}" --quiet
        DEPENDS "${CMAKE_SOURCE_DIR}/tools/slint-po.py" ${foxsdr_slint_catalogues} ${foxsdr_slint_ui_files}
        COMMENT "foxsdr-slint: translations from resources/lang"
        VERBATIM)
    add_custom_target(foxsdr-slint-translations DEPENDS "${foxsdr_slint_po_stamp}")
    add_dependencies(foxsdr-slint foxsdr-slint-translations)
    set_property(TARGET foxsdr-slint PROPERTY SLINT_BUNDLE_TRANSLATIONS "${foxsdr_slint_po_dir}")
    set_property(TARGET foxsdr-slint PROPERTY SLINT_NO_DEFAULT_TRANSLATION_CONTEXT ON)
else()
    message(STATUS "foxsdr-slint: Python 3 not found - the window is built English-only (no bundled translations)")
endif()

# THE ELEMENT DEBUG INFO (docs 6.47). The scripted input (input_target.hpp) finds a control by its accessible label through the
# testing API's element tree, and that tree is EMPTY unless the Slint compiler emitted the element debug info (the runtime's
# collect_elements counts 0 elements per item and logs "The use of the ElementHandle API requires the presence of debug info").
# SlintMacro.cmake sets SLINT_EMIT_DEBUG_INFO=1 only for an SDK built with SYSTEM_TESTING, which this one is not (its features:
# TESTING without SYSTEM_TESTING), so the variable is handed to the macro here: it reads the caller's SLINT_COMPILER_ENV_VARS.
# The cost is the element names and counts in the generated code; nothing of the window's behaviour changes.
set(SLINT_COMPILER_ENV_VARS SLINT_EMIT_DEBUG_INFO=1)
slint_target_sources(foxsdr-slint ui/main.slint)
unset(SLINT_COMPILER_ENV_VARS)
if(Python3_Interpreter_FOUND)
    # THE LIMIT. slint_target_sources registers the generated header's command with DEPENDS on the compiler and main.slint only (its
    # depfile adds the imported .slint files, never the .po files it bundles) and the macro offers no hook for more. A changed
    # translation would therefore not re-run the compiler. add_custom_command(... APPEND DEPENDS) may add inputs to a command made
    # earlier in the SAME directory scope, which this is, so main.h also depends on the stamp. What it cannot do: the compiler is
    # still run on ALL of main.slint when a single word changes (the header is one unit), and the dependency exists only for a
    # build through this CMake - running slint-compiler by hand knows nothing of it.
    add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/main.h" APPEND DEPENDS "${foxsdr_slint_po_stamp}")
endif()
foxsdr_slint_copy_dll(foxsdr-slint)

# THE ENGINE'S BAND PLANS BESIDE THIS EXECUTABLE TOO (2026-11-09). BandPlan::defaultDir() is executable-relative and foxsdr-slint
# is built into its own directory, so the root's cascade_stage_resources (beside cascade and foxsdr-engine) never reached it: a
# build-tree foxsdr-slint ran with NO band plan - the Display section's plan picker empty and nothing for "Follow the band plan's
# mode" to follow (found by src/slint/tests/drive_retune_run.cpp). The installer puts every executable in one folder, so a release
# was never short. One target, as the root's is (two copies into one place by a parallel build would race).
add_custom_target(foxsdr_slint_stage_resources ALL
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/resources/bandplans"
            "$<TARGET_FILE_DIR:foxsdr-slint>/resources/bandplans"
    COMMENT "Staging resources/bandplans beside foxsdr-slint"
    VERBATIM)
add_dependencies(foxsdr-slint foxsdr_slint_stage_resources)

# THE CLIP PROBE (docs 6.62), on request only: cmake -DFOXSDR_SLINT_CLIP_PROBE=ON adds the target foxsdr-slint-probe (never part of ALL); see probe/CMakeLists.txt.
option(FOXSDR_SLINT_CLIP_PROBE "Also configure foxsdr-slint-probe, the window built from a copy of ui/ whose Text elements report an overflow (tools/slint_clip_sweep.py)" OFF)
if(FOXSDR_SLINT_CLIP_PROBE AND Python3_Interpreter_FOUND)
    add_subdirectory(probe)
endif()

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

# THE SCRIPTED INPUT'S DRIVEN RUNS (input_script.hpp, docs 6.47): test_slint_drive_run spawns foxsdr-slint itself on a scratch profile
# with FOXSDR_INPUT_SCRIPT, so it is built after the window and carries the gui label (it opens a window: the hidden-desktop pass) next to
# drive (so the drive suite can be run on its own: ctest -L drive - not "slint-drive", which ctest -L slint would match and open windows). Each script of tests/drive/*.txt is one more test,
# slint_drive_<name>, run by the same program; a script added later is picked up at the next build (CONFIGURE_DEPENDS).
add_executable(test_slint_drive_run tests/test_slint_drive_run.cpp)
target_compile_features(test_slint_drive_run PRIVATE cxx_std_20)
target_include_directories(test_slint_drive_run PRIVATE "${CMAKE_SOURCE_DIR}/tests")
target_compile_definitions(test_slint_drive_run PRIVATE FOXSDR_SLINT_EXE="$<TARGET_FILE:foxsdr-slint>" FOXSDR_DRIVE_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/drive/fixtures")
if(Python3_Interpreter_FOUND)
    target_compile_definitions(test_slint_drive_run PRIVATE FOXSDR_DRIVE_PYTHON="${Python3_EXECUTABLE}")   # makes a missing fixture WAV from its make_<name>.py
else()
    target_compile_definitions(test_slint_drive_run PRIVATE FOXSDR_DRIVE_PYTHON="python3")
endif()
add_dependencies(test_slint_drive_run foxsdr-slint)
add_test(NAME test_slint_drive_run COMMAND test_slint_drive_run)
set_tests_properties(test_slint_drive_run PROPERTIES LABELS "gui;drive" TIMEOUT 300)
file(GLOB _foxsdr_slint_drive_scripts CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/drive/*.txt")
foreach(_script ${_foxsdr_slint_drive_scripts})
    get_filename_component(_drive_name "${_script}" NAME_WE)
    add_test(NAME slint_drive_${_drive_name} COMMAND test_slint_drive_run "${_script}")
    set_tests_properties(slint_drive_${_drive_name} PROPERTIES LABELS "gui;drive" TIMEOUT 600)
endforeach()

# THE COVERAGE LEDGER (tools/slint_drive_ledger.py, docs 6.47): every row of docs/slint-port-tick.md classified from tests/drive/coverage.tsv
# into docs/slint-drive-coverage.md; the check fails when the committed ledger is stale or a control row is unclassified - no silent gaps.
if(Python3_Interpreter_FOUND)
    add_test(NAME slint_drive_ledger COMMAND Python3::Interpreter -I "${CMAKE_SOURCE_DIR}/tools/slint_drive_ledger.py" --check)
    set_tests_properties(slint_drive_ledger PROPERTIES LABELS "headless;slint" TIMEOUT 60)
endif()

# THE SETTINGS ROUND TRIP (tests/test_slint_config_roundtrip.cpp, docs 6.48): a real-shaped profile through foxsdr-slint and cascade for one
# bounded run each, untouched and with one scripted change, compared field by field. It builds the seed with ConfigStore (the engine library)
# and compares the documents with the JSON library; it starts both windows, so it is a gui test (the hidden-desktop pass).
add_executable(test_slint_config_roundtrip tests/test_slint_config_roundtrip.cpp)
target_compile_features(test_slint_config_roundtrip PRIVATE cxx_std_20)
target_include_directories(test_slint_config_roundtrip PRIVATE "${CMAKE_SOURCE_DIR}/tests" "${CMAKE_SOURCE_DIR}/src")
target_link_libraries(test_slint_config_roundtrip PRIVATE cascade_engine cascade_json)
target_compile_definitions(test_slint_config_roundtrip PRIVATE FOXSDR_SLINT_EXE="$<TARGET_FILE:foxsdr-slint>")
add_dependencies(test_slint_config_roundtrip foxsdr-slint)
if(TARGET cascade)
    target_compile_definitions(test_slint_config_roundtrip PRIVATE FOXSDR_CASCADE_EXE="$<TARGET_FILE:cascade>")
    add_dependencies(test_slint_config_roundtrip cascade)
endif()
add_test(NAME test_slint_config_roundtrip COMMAND test_slint_config_roundtrip)
set_tests_properties(test_slint_config_roundtrip PROPERTIES LABELS "gui" TIMEOUT 600)

# THE RETUNE DRIVE (2026-11-09, a tester's band switch; src/slint/tests/drive_retune_run.cpp): spawns foxsdr-slint bounded on a
# scratch profile with FOXSDR_SLINT_RETUNE_PROBE and reads its summary line - a zoomed picture follows a typed retune into another
# band at its width, and the mode follows the band plan, or stays when the Radio body's checkbox is off. A window is opened, so it
# carries the GUI gate's label (`gui`, run on the hidden desktop), never `slint`. Named outside the test_slint_*.cpp glob above on
# purpose: that glob builds render tests against the specimen sheets.
add_executable(test_slint_retune_run tests/drive_retune_run.cpp)
target_compile_features(test_slint_retune_run PRIVATE cxx_std_20)
target_link_libraries(test_slint_retune_run PRIVATE cascade_engine)
target_include_directories(test_slint_retune_run PRIVATE
    "${CMAKE_SOURCE_DIR}/src"
    "${CMAKE_SOURCE_DIR}/tests"
    "${CMAKE_SOURCE_DIR}/third_party/foxsdr_api")
target_compile_definitions(test_slint_retune_run PRIVATE FOXSDR_SLINT_EXE="$<TARGET_FILE:foxsdr-slint>")
add_dependencies(test_slint_retune_run foxsdr-slint)
add_test(NAME test_slint_retune_run COMMAND test_slint_retune_run)
set_tests_properties(test_slint_retune_run PROPERTIES LABELS "gui" TIMEOUT 300)

# The translation generator's own tests (tools/test_slint_po.py): temp-dir fixtures, nothing of the build tree. -I keeps the
# interpreter out of the current directory and the user's site-packages (the script is standard-library only).
if(Python3_Interpreter_FOUND)
    add_test(NAME test_slint_po COMMAND "${Python3_EXECUTABLE}" -I "${CMAKE_SOURCE_DIR}/tools/test_slint_po.py")
    set_tests_properties(test_slint_po PROPERTIES LABELS "slint" TIMEOUT 60)
    # The clip probe's source transform and the sweep's line parser (docs 6.62; tools/test_slint_clip_probe.py), the same way.
    add_test(NAME test_slint_clip_probe COMMAND "${Python3_EXECUTABLE}" -I "${CMAKE_SOURCE_DIR}/tools/test_slint_clip_probe.py")
    set_tests_properties(test_slint_clip_probe PROPERTIES LABELS "slint" TIMEOUT 60)
endif()
