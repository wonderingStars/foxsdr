# The fence: what the interface target (cascade_interface, src/gui) may and may not include.
#
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#
# Engine extraction stage 4, step 5 (docs/engine-stage4.md, the section that closes the stage). The interface is built as its own
# target and the COMPILER, not a test, refuses it the engine: every header listed below exists, for the interface's translation
# units alone, as a stub whose only content is an #error. The stubs live in a generated directory under the build tree
# (${CMAKE_BINARY_DIR}/interface_fence) which is put FIRST on the interface's include path, so `#include "engine/engine.hpp"`
# from anything the interface compiles - a src/gui file, or any header it reaches - finds the stub and stops the build, naming the
# header and why. The real headers are untouched and every other target (the engine, the tests) sees them as ever.
#
# WHY A STUB DIRECTORY AND NOT A MACRO IN EACH HEADER. The list is one place, in this file, with a reason per entry; a header does
# not have to know it is fenced; a new header under src/engine or src/net is DENIED BY DEFAULT (there is no allow list); and a
# denied header cannot be reached by an include order accident, since the stub shadows it before the real one is looked for. Every
# #include in src/ is spelled with its directory ("engine/engine.hpp", never "engine.hpp"), which the check at the foot of this
# file keeps true for the headers the interface may reach.
#
# WHAT THIS IS NOT. It is a COMPILE fence. cascade_interface still LINKS the engine library, because the views call into core/
# (i18n, the band plan, the config store ...) and those objects are the engine library's; what it cannot do is NAME the Engine, its
# pipeline, its plugin host and runner, its recorders or its transmitter. Link-level separation is a later phase's (the ledger below).
#
# WHAT THE INTERFACE MAY SEE IS WHAT IS NOT LISTED, and the allow side that matters is in the docs: foxsdr_api.h (the table), the
# command vocabulary (core/app_commands.hpp), the plugin UI port (core/plugin_ui_port.hpp: the one extension, data and pure virtuals
# only), the bounded run's two hooks (core/bounded_hooks.hpp), the data headers the port created (core/plugin_ui_data.hpp, the gui/ view
# headers, gui/tune_local.hpp, gui/scope_gain.hpp, gui/converter_lo_seed.hpp ...), the pure helpers that WERE src/engine's and are core/'s
# since shell retirement R10 (the twelve of the ledger below), Dear ImGui and GLFW. NOT ONE HEADER OF src/engine/ IS ON THAT SIDE: the one door
# (the shell header) is deleted, and the allow list that held twelve pure helpers and the door (13 headers, 2026-10-21 to
# 2026-10-28) is deleted with it.

# --- everything the interface may not include: header (relative to src/), then why. Add a line to fence a header; remove one only
# with the change that makes the interface not need it (the test that reads this file wants a dated reason in the docs).
set(CASCADE_INTERFACE_DENIED "")
function(cascade_interface_deny header reason)
    set(CASCADE_INTERFACE_DENIED "${CASCADE_INTERFACE_DENIED};${header}" PARENT_SCOPE)
    set("CASCADE_INTERFACE_DENY_REASON_${header}" "${reason}" PARENT_SCOPE)
endfunction()

# The engine and what it is made of: the interface asks it through the engine API's table (and the plugin UI port), nothing else.
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

# The machinery whose DATA the interface was reaching (the survey's R11a, shell retirement 2026-10-29): each was hoisted into a data header the interface
# includes, and the machinery is denied now, so the hoist cannot quietly be undone. The window draws the records and asks no question of the machine.
cascade_interface_deny(core/plugin_host.hpp "the module loader is the engine's; a module scan's records are core/plugin_host_data.hpp (LoadedPlugin, PluginOrigin, PluginRejection, pluginKey)")
cascade_interface_deny(core/plugin_repo.hpp "the catalogue fetcher and installer is the engine's; the catalogue's entry types and the address arithmetic are core/plugin_repo_data.hpp")
cascade_interface_deny(core/freq_manager.hpp "the bookmark list and its file are the engine's (the BOOKMARKS list and FOXAPI_OP_BOOKMARK_*); the entry type is core/bookmark.hpp")
cascade_interface_deny(core/mp3_writer.hpp "the MP3 file writer is the patch's file worker; whether this build can write MP3 is core/mp3_support.hpp (kMp3Supported)")
cascade_interface_deny(core/patch_audio.hpp "the patch's audio file and device outputs are the engine's (no src/gui file includes it; the page edits a draft of the graph)")

# --- src/engine: DENIED WHOLE (shell retirement R10, 2026-10-29; it was denied by default with an allow list of 13 until then). Every header of the
# engine's directory is the engine's: the Engine, its host and API, the service, the process boot, the bounded run's harness, the engine side of the
# plugin UI port, the vocabulary only the engine speaks (engine_types.hpp). The window takes the engine's table from foxsdr_api.h, the command vocabulary
# from core/app_commands.hpp and the port from core/plugin_ui_port.hpp; the twelve pure helpers it used to take from engine/ (tune_control, bias_tee,
# soundcard_panel, audio_open, device_scan_plan, receiver_tables, source_fallback, add_all_plan, running_view, rate_follow_status, plugin_store_reasons,
# tx_frequency) are core/ headers now (git mv, the same bytes), and what the shell header held that the window needed is core/engine_vocab.hpp,
# core/status_text.hpp and core/pass_part.hpp. There is NO allow list: a new header under src/engine is denied until someone decides otherwise, as one
# under src/net is.
file(GLOB _cascade_engine_headers RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}/src" CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/engine/*.hpp" "${CMAKE_CURRENT_SOURCE_DIR}/src/engine/*.h")
foreach(_h IN LISTS _cascade_engine_headers)
    cascade_interface_deny("${_h}" "engine/ is the engine's, denied whole: the interface asks the engine through the table (foxsdr_api.h) and the plugin UI port (core/plugin_ui_port.hpp), and the pure helpers it shares with the engine are core/ headers (shell retirement R10)")
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

# --- THE HEADER LEDGER: what the interface still reaches that is engine-side LIBRARY code, and why. Measured with the fenced include closure of src/gui
# (every header a src/gui file reaches through its quoted includes, a script that only reads; shell retirement R10 and R11a, 2026-10-29: 302 files -
# 155 gui, 101 core, 34 source, 10 dsp, 2 usb, and ZERO engine and ZERO net, down from 13 engine headers at the survey, 2026-10-28). None of it is the Engine
# or an object the Engine owns; all of it is vocabulary or arithmetic the views share with the engine, and each group has the change that ends it:
#   core/ DATA and light helpers (allowed for good): app_commands, config, receiver_snapshot, plugin_ui_data, plugin_ui_port, bounded_hooks, pass_part,
#     engine_vocab, status_text, plugin_abi.h, spectrum_frame, scanner_params, tx_input, host_image, view_settings, patch_graph/plan/draft/io/levels/strip/
#     devices/device_label, band_plan, countries, freq_converter, user_presets, airspy_settings, bias_tee_memory, receiver_position, serial_port, i18n,
#     utf8_text, version, diag_log, seqlock_box, scripts, font_blobs, image_write, package_identity, engine_measure, hang_watchdog (the window's own),
#     and the twelve pure helpers that were src/engine's until R10 (tune_control, bias_tee, soundcard_panel, audio_open, device_scan_plan, receiver_tables,
#     source_fallback, add_all_plan, running_view, rate_follow_status, plugin_store_reasons, tx_frequency): the engine uses them too, which is why they
#     are core/ (their C++ namespace is cascade::gui or cascade::engine still; a namespace is not a directory and renaming 150 files' worth of names buys nothing).
#   core/ the INTERFACE'S AND HOST'S UTILITIES (allowed, named): the window's own senders and checkers - crash_upload, feature_request, problem_report,
#     updater, crash_handler, diag_report, telemetry (the usage report's types the config carries), store_update, tester_link/tester_usage, radio_setup,
#     winusb_install, file_pick, os_crash_record, config_writer (left the window's closure with the keeper, R8). They are the interface's, not the engine's.
#   core/ MACHINERY THAT WAS REACHED FOR ITS DATA: RESOLVED (R11a, the survey's items 3-7, 2026-10-29), each line by its own move, and the machinery DENIED above:
#     plugin_host.hpp -> core/plugin_host_data.hpp (LoadedPlugin, PluginOrigin, PluginRejection, pluginKey ...: the module scan's records; PluginHost and
#       PluginStopSet stay); the LoadedPlugin overloads of module_census/plugins_view are kept - six tests pin them - and now include the data header;
#     plugin_repo.hpp -> core/plugin_repo_data.hpp (the catalogue entry types and the seven pure statics as free functions: pluginSiteLabel,
#       pluginSiteDisplay, pluginSiteHost, normalisePluginSiteUrl, defaultPluginIndexUrl, pluginHostOs/Arch, pluginIsHttpsUrl; PluginRepo's statics of the
#       same names forward to them); core/config.hpp and core/plugin_cleanup.hpp include the data header;
#     freq_manager.hpp -> core/bookmark.hpp (the Bookmark struct; FreqManager stays);
#     mp3_writer.hpp -> core/mp3_support.hpp (kMp3Supported, a compile-time constant; Mp3Writer::available() returns it);
#     patch_audio.hpp: the ledger line was STALE (no src/gui file includes it; only patch_audio_rate.hpp is reached): the line is deleted and the header denied.
#   source/ and usb/ (34 + 2 headers): the device-kind vocabulary. app_window.hpp names ten driver headers (NativeDeviceInfo, GainUnit, SoundCardSettings,
#     AirspySource::GainMode ...) and core/bias_tee.hpp, core/tune_control.hpp, core/soundcard_panel.hpp and source/rsp_rows.hpp include the drivers for
#     their capability types; the window never opens a radio. Fate: a later phase hoists `source/device_kinds.hpp` for the five data types and
#     reclassifies the SDRplay service header (a Windows service query, OS glue) as an interface/host utility. STILL REACHED, and honestly so.
#   dsp/ (10): demod/fft/fir/modulator ... the views' arithmetic; no engine object. Allowed for good.
#   net/ : NOT REACHED, and DENIED (above). engine/ : NOT REACHED, and DENIED (above).
# The guard (tests/test_engine_shell_guard.cpp) holds that no src/gui file and no header the interface includes names a denied one, that the
# denied side is the whole of engine/ and net/, and that the allow side of engine/ is empty; the rest of the ledger is documentation until a hoist
# moves it.

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
        "#error \"cascade_interface may not include <${_h}>: ${CASCADE_INTERFACE_DENY_REASON_${_h}}. The interface sees the engine through the table (foxsdr_api.h) and the plugin UI port (core/plugin_ui_port.hpp) only - see cmake/interface_fence.cmake and docs/engine-stage4.md.\"
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
