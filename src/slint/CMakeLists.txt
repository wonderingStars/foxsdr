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
set(foxsdr_slint_sources main.cpp host_logic.cpp host_duties.cpp kit_logic.cpp plugin_kit.cpp kit_publish.cpp map_compositor.cpp signal_logic.cpp signal_publish.cpp system_logic.cpp system_publish.cpp tools_logic.cpp tools_publish.cpp extend_logic.cpp extend_publish.cpp patch_logic.cpp patch_publish.cpp
    "${CMAKE_SOURCE_DIR}/src/gui/track_info_cache.cpp"
    "${CMAKE_SOURCE_DIR}/src/gui/module_census.cpp")
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
target_include_directories(foxsdr-slint PRIVATE
    "${CMAKE_SOURCE_DIR}/src"
    "${CMAKE_SOURCE_DIR}/third_party/foxsdr_api"
    "${CMAKE_SOURCE_DIR}/third_party/glfw/deps")   # stb_image_write for the screenshot key
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

slint_target_sources(foxsdr-slint ui/main.slint)
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

# The translation generator's own tests (tools/test_slint_po.py): temp-dir fixtures, nothing of the build tree. -I keeps the
# interpreter out of the current directory and the user's site-packages (the script is standard-library only).
if(Python3_Interpreter_FOUND)
    add_test(NAME test_slint_po COMMAND "${Python3_EXECUTABLE}" -I "${CMAKE_SOURCE_DIR}/tools/test_slint_po.py")
    set_tests_properties(test_slint_po PROPERTIES LABELS "slint" TIMEOUT 60)
endif()
