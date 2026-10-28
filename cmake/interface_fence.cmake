# The fence: what the interface target (cascade_interface, src/gui) may and may not include.
#
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#
# Engine extraction stage 4, step 5 (docs/engine-stage4.md, the section that closes the stage). The interface is built as its own
# target and the COMPILER, not a test, refuses it the engine: every header listed below exists, for the interface's translation
# units alone, as a stub whose only content is an #error. The stubs live in a generated directory under the build tree
# (${CMAKE_BINARY_DIR}/interface_fence) which is put FIRST on the interface's include path, so `#include "engine/engine.hpp"`
# from anything the interface compiles - a src/gui file, or any header it reaches - finds the stub and stops the build, naming the
# header and why. The real headers are untouched and every other target (the engine, the shell, the tests) sees them as ever.
#
# WHY A STUB DIRECTORY AND NOT A MACRO IN EACH HEADER. The list is one place, in this file, with a reason per entry; a header does
# not have to know it is fenced; a new header under src/engine is DENIED BY DEFAULT (the allow list below is the only way in); and a
# denied header cannot be reached by an include order accident, since the stub shadows it before the real one is looked for. Every
# #include in src/ is spelled with its directory ("engine/engine.hpp", never "engine.hpp"), which the check at the foot of this
# file keeps true for the headers the interface may reach.
#
# WHAT THIS IS NOT. It is a COMPILE fence. cascade_interface still LINKS the engine library, because the views call into core/
# (i18n, the band plan, the config store ...) and those objects are the engine library's; what it cannot do is NAME the Engine, its
# pipeline, its plugin host and runner, its recorders or its transmitter. Link-level separation is stage 7's.
#
# WHAT THE INTERFACE MAY SEE IS WHAT IS NOT LISTED, and the allow side that matters is in the docs: foxsdr_api.h (the table), the
# command vocabulary (core/app_commands.hpp), the data headers the port created (core/plugin_ui_data.hpp, the gui/ view headers,
# gui/tune_local.hpp, gui/scope_gain.hpp, gui/converter_lo_seed.hpp ...), Dear ImGui and GLFW, the pure helpers of src/engine named
# in CASCADE_INTERFACE_ALLOWED_ENGINE_HEADERS, and engine/engine_shell.hpp - the one door.

# --- src/engine: DENIED BY DEFAULT. The headers of the engine's directory the interface may include: pure helpers that name no
# Engine (tests/test_engine_shell_guard.cpp holds that none of them includes a denied header) and the shell's door itself.
set(CASCADE_INTERFACE_ALLOWED_ENGINE_HEADERS
    add_all_plan.hpp
    audio_open.hpp
    bias_tee.hpp
    device_scan_plan.hpp
    engine_shell.hpp
    plugin_store_reasons.hpp
    rate_follow_status.hpp
    receiver_tables.hpp
    running_view.hpp
    soundcard_panel.hpp
    source_fallback.hpp
    tune_control.hpp
    tx_frequency.hpp)

# --- everything else the interface may not include: header (relative to src/), then why. Add a line to fence a header; remove one only
# with the change that makes the interface not need it (the test that reads this file wants a dated reason in the docs).
set(CASCADE_INTERFACE_DENIED "")
function(cascade_interface_deny header reason)
    set(CASCADE_INTERFACE_DENIED "${CASCADE_INTERFACE_DENIED};${header}" PARENT_SCOPE)
    set("CASCADE_INTERFACE_DENY_REASON_${header}" "${reason}" PARENT_SCOPE)
endfunction()

# The engine and what it is made of: the interface asks it through engine/engine_shell.hpp and the engine API's table, nothing else.
cascade_interface_deny(core/pipeline.hpp "the DSP pipeline is the engine's (read its state through the table)")
cascade_interface_deny(core/plugin_ui.hpp "the plugin UI machinery is the engine side of the plugin UI port (core/plugin_ui_port.hpp: PluginUiPort); the data is core/plugin_ui_data.hpp")
cascade_interface_deny(core/plugin_api.hpp "the plugin host API core is the engine's")
cascade_interface_deny(core/plugin_directive.hpp "the engine-to-front-end directive is the port's (core/plugin_ui_port.hpp: PluginDirectiveFacts)")

# The objects the engine OWNS (stage 4, group 14 onwards: the window holds none of them and reads what they publish through the table). Their
# headers declare the machinery, not data; the data the window draws is in the data headers (core/plugin_ui_data.hpp, core/receiver_snapshot.hpp ...).
cascade_interface_deny(core/plugin_runner.hpp "the receiver plugins' runner is the engine's (the pictures come through the plugin UI port)")
cascade_interface_deny(core/recorder.hpp "the IQ and audio recorders are the engine's (FOXAPI_OP_*_RECORD, the RECORDER facts)")
cascade_interface_deny(core/transmitter.hpp "the transmitter is the engine's (the Transmit page keys it through its API session)")
cascade_interface_deny(core/retune_coalescer.hpp "the retune coalescer is the engine's")
cascade_interface_deny(core/gps_reader.hpp "the GPS reader is the engine's (the GPS list and FOXAPP_STATUS_GPS_RESULT)")
cascade_interface_deny(core/patch_runner.hpp "the patch runtime is the engine's (the page edits a draft and sends the whole graph)")
cascade_interface_deny(core/patch_radio.hpp "the patch's radios are the engine's")
cascade_interface_deny(core/patch_recordings.hpp "the patch's recordings are the engine's")
cascade_interface_deny(core/scanner.hpp "the scanner is the engine's (FOXAPI_OP_SCANNER_CONFIG and the SCANNER facts)")

# The engine's directory, by default: see CASCADE_INTERFACE_ALLOWED_ENGINE_HEADERS above.
file(GLOB _cascade_engine_headers RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}/src" CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/engine/*.hpp" "${CMAKE_CURRENT_SOURCE_DIR}/src/engine/*.h")
foreach(_h IN LISTS _cascade_engine_headers)
    get_filename_component(_name "${_h}" NAME)
    list(FIND CASCADE_INTERFACE_ALLOWED_ENGINE_HEADERS "${_name}" _allowed)
    if(_allowed EQUAL -1)
        cascade_interface_deny("${_h}" "it is the engine's own (src/engine is denied by default; engine/engine_shell.hpp is the door)")
    endif()
endforeach()
# An allow-list entry that names no header is a typo that would quietly allow a future file of that name.
foreach(_name IN LISTS CASCADE_INTERFACE_ALLOWED_ENGINE_HEADERS)
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/engine/${_name}")
        message(FATAL_ERROR "interface fence: CASCADE_INTERFACE_ALLOWED_ENGINE_HEADERS names src/engine/${_name}, which does not exist")
    endif()
endforeach()

# --- src/net: DENIED WHOLE (stage 5f, 2026-10-22). The web server and the CAT listener are the engine's (engine/server_host.hpp, stage 5b); their
# control and their reads are sessions of the table (5c-5e); the window configures them with FOXAPI_OP_SERVER_CONFIG and reads them from the SERVERS
# list and the fact.web.* / fact.cat.* facts. There is NO allow list: the interface takes the port and address vocabulary from core/server_ports.hpp
# (the panels' constants, hoisted by 5b) and the mode names from core/demod_names.hpp (hoisted by 5f), and names nothing else of the directory. A
# new header under src/net is denied until someone decides otherwise, as one under src/engine is.
file(GLOB _cascade_net_headers RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}/src" CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/net/*.hpp" "${CMAKE_CURRENT_SOURCE_DIR}/src/net/*.h")
foreach(_h IN LISTS _cascade_net_headers)
    cascade_interface_deny("${_h}" "net/ is the engine's: the web server and the CAT listener are hosted by the engine (engine/server_host.hpp) and spoken to through the table's sessions and FOXAPI_OP_SERVER_CONFIG; the port vocabulary is core/server_ports.hpp, the mode names core/demod_names.hpp")
endforeach()

# --- THE HEADER LEDGER: what the interface still reaches that is engine-side LIBRARY code, and why (stage 4 step 5; measured with the fenced
# include closure of src/gui, 2026-10-21: 232 files - 105 gui, 49 core, 31 source, 13 engine, 9 dsp, 8 net, 1 usb). None of it is the Engine or
# an object the Engine owns; all of it is vocabulary or arithmetic the views share with the engine, and each group has the change that ends it:
#   core/ DATA and light helpers (allowed for good): app_commands, config, receiver_snapshot, plugin_ui_data, plugin_abi.h, spectrum_frame,
#     scanner_params, tx_input, host_image, view_settings, patch_graph/plan/draft/io/levels/strip/devices/device_label, band_plan, countries,
#     freq_converter, user_presets, airspy_settings, bias_tee_memory, receiver_position, serial_port, i18n, utf8_text, version, diag_log,
#     seqlock_box, scripts, font_blobs, image_write, package_identity, engine_measure (a shell seam's type), hang_watchdog (the window's own).
#   core/ the WINDOW'S OWN senders and checkers (allowed): crash_upload, feature_request, problem_report, updater, crash_handler, diag_report,
#     telemetry (the usage report's types the config carries). They are the interface's, not the engine's.
#   core/ engine-side classes reached for their DATA (the next hoists, each a pure move like spectrum_frame): plugin_host.hpp (LoadedPlugin; module_census
#     and plugins_view draw it), plugin_repo.hpp (the catalogue entry types; core/config.hpp includes it), freq_manager.hpp (Bookmark; freq_import
#     and the bookmark section), patch_audio.hpp (the patch page's output kinds) and mp3_writer.hpp (Mp3Writer::available(), a static capability query the output chooser calls). Stage 7 splits each into data and
#     machinery; until then they are reached, not called into by the window except the data.
#   source/ and usb/ (32 headers): the device-kind vocabulary. engine/bias_tee.hpp, engine/tune_control.hpp, engine/soundcard_panel.hpp and
#     source/rsp_rows.hpp include the drivers for their capability types, and app_window.hpp names ten of them; the window never opens a radio.
#     Fate: stage 7 (the headless foxsdr-engine owns the drivers; the pure helpers take a device-kind header instead).
#   dsp/ (9): demod/fft/fir/modulator ... the views' arithmetic.
#   net/ (8 at the first measurement): NOT REACHED ANY MORE, and DENIED (above). Stage 5 moved the servers into the engine; 5b hoisted the panels'
#     port vocabulary (core/server_ports.hpp) and 5f the last thing reached, the mode names engine/receiver_tables.hpp checks the window's
#     buttons against (core/demod_names.hpp; it reached net/status_compose.hpp for them, and through it the other seven).
# The guard (tests/test_engine_shell_guard.cpp) holds the engine/ allow list's size and that no src/gui file and no allowed header names a denied
# one; the rest of the ledger is documentation until a hoist moves it.

# --- the stubs ---------------------------------------------------------------------------------------------------------------------
set(CASCADE_INTERFACE_FENCE_DIR "${CMAKE_BINARY_DIR}/interface_fence")
foreach(_h IN LISTS CASCADE_INTERFACE_DENIED)
    if(_h STREQUAL "")
        continue()
    endif()
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/${_h}")
        message(FATAL_ERROR "interface fence: the deny list names src/${_h}, which does not exist")
    endif()
    string(CONCAT _text
        "// GENERATED by cmake/interface_fence.cmake - do not edit. The interface target (cascade_interface) may not include this header.
"
        "#error \"cascade_interface may not include <${_h}>: ${CASCADE_INTERFACE_DENY_REASON_${_h}}. The interface sees the engine through engine/engine_shell.hpp and the table (foxsdr_api.h) only - see cmake/interface_fence.cmake and docs/engine-stage4.md.\"
")
    set(_path "${CASCADE_INTERFACE_FENCE_DIR}/${_h}")
    # Written only when it differs, so an unchanged stub keeps its time stamp and the interface does not rebuild for nothing.
    set(_old "")
    if(EXISTS "${_path}")
        file(READ "${_path}" _old)
    endif()
    if(NOT _old STREQUAL _text)
        file(WRITE "${_path}" "${_text}")
    endif()
endforeach()
# A stub for a header the list no longer names would keep denying it: sweep the directory.
file(GLOB_RECURSE _cascade_fence_files "${CASCADE_INTERFACE_FENCE_DIR}/*")
foreach(_f IN LISTS _cascade_fence_files)
    file(RELATIVE_PATH _rel "${CASCADE_INTERFACE_FENCE_DIR}" "${_f}")
    list(FIND CASCADE_INTERFACE_DENIED "${_rel}" _still)
    if(_still EQUAL -1)
        file(REMOVE "${_f}")
    endif()
endforeach()

# Puts `target` behind the fence: the stubs FIRST on its include path (private - no consumer inherits them). Every compile of the
# interface, and every probe of the fence's own test, goes through here.
function(cascade_apply_interface_fence target)
    target_include_directories(${target} BEFORE PRIVATE "${CASCADE_INTERFACE_FENCE_DIR}")
    target_compile_definitions(${target} PRIVATE CASCADE_INTERFACE_TU=1)
endfunction()
