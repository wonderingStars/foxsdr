# The tests of master's window features that the 0.99.84 merge has NOT yet re-expressed (docs/engine-merge-0.99.84.md has the ledger).
#
# Each of these is a test master wrote against its own AppWindow - it reads or writes the window's engine fields through
# AppWindowTestAccess (testHooks_, pipeline_, iqRecorder_ ...) - and so cannot compile against this branch's window, whose only door
# to the engine is the EngineShell and the table. They are NOT dropped and NOT weakened. Each is left out of the build, and a RED
# placeholder takes its name: `merge_pending_<test>` fails, saying which feature it belongs to, until the feature is re-expressed and
# the test is CONVERTED (master's file is kept as it is in tests/, untouched, for the conversion to start from) - the house pattern is
# tests/pass_seam.hpp and AppWindowTestAccess over rawEngine(). Removing a name from the list below is the act of converting it.
#
# The placeholder is a test, not a comment, so that `ctest` counts what is owed and a release cannot forget it.
#
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

set(CASCADE_MERGE_PENDING_TESTS)
set(CASCADE_MERGE_PENDING_FEATURE_OF)

set(CASCADE_MERGE_PENDING_PARTS)
set(CASCADE_MERGE_PENDING_PART_FEATURE_OF)
set(CASCADE_MERGE_PENDING_PART_TEXT_OF)
# cascade_merge_pending_part(<ledger id> <name> <what is owed>): a PART of a test that was converted (the rest of it runs) which belongs to
# a feature that is not carried yet. The part is carved out of the converted test's source behind a note naming <name>; the red placeholder
# merge_pending_part_<name> keeps it counted.
macro(cascade_merge_pending_part feature name text)
    list(APPEND CASCADE_MERGE_PENDING_PARTS ${name})
    set(CASCADE_MERGE_PENDING_PART_FEATURE_OF_${name} "${feature}")
    set(CASCADE_MERGE_PENDING_PART_TEXT_OF_${name} "${text}")
endmacro()

# cascade_merge_pending(<ledger id> <tests...>): the tests belong to the feature the ledger names under that id.
macro(cascade_merge_pending feature)
    foreach(_t ${ARGN})
        list(APPEND CASCADE_MERGE_PENDING_TESTS ${_t})
        set(CASCADE_MERGE_PENDING_FEATURE_OF_${_t} "${feature}")
    endforeach()
endmacro()

cascade_merge_pending(RAIL-CHROME      # 0.99.74 dials (the interface size, the rail fold, the status fold and the TOOLS bank returned in stub batch B1)
    test_deck_knobs_app)
cascade_merge_pending(STORE-VIEW       # 0.99.50 / 0.99.72 / 0.99.74 plugin store and Fitted modules window on the table (the engine's half - the kept
                       # catalogue, the sites, the pictures, the clean-up of old copies - returned in stub batch B3: test_plugin_catalogue_app,
                       # test_plugin_cleanup_app and test_plugin_update_cleanup_app are the engine's now)
    test_plugin_store_window test_fitted_modules_window test_plugin_store_view test_fitted_modules_no_disk)
cascade_merge_pending(PLUGIN-SETTINGS-UI    # 0.99.43 CASCADE_CAP_SETTINGS_UI: fields the host draws for a plugin
    test_plugin_settings_ui)
cascade_merge_pending(PLUGIN-ROW-KEYS  # 0.99.77 a row in a plugin's panel as a key
    test_programme_audio_app)
cascade_merge_pending(TX-PLUGIN        # 0.99.74 a plugin may key the transmitter once granted and armed
    test_tx_audio_app test_tx_audio_page_app test_tx_audio_plate_app)
cascade_merge_pending_part(TX-PLUGIN transmit_page_plugin_key
    "tests/test_transmit_page.cpp section 10: the wiring of a plugin's transmit key in the window (applyPluginTransmitKey, setPluginKey, pluginMayKey, txArming_.arm, armTransmitPlugins/disarmTransmitPlugins). Restore master's section (git show origin/master:tests/test_transmit_page.cpp) when the feature is re-expressed.")
cascade_merge_pending_part(IMPORT-DIALOG excuse_cap_pickfile
    "tests/test_excuse_cap.cpp: the third native-wait call site, pickFile (the BROWSE key's modal file dialog, 0.99.74, used by the plugin settings' BROWSE FILES and the bookmark Import dialog). The expected count of runShellOpen(watchdogShellHooks(), call sites is 2 today and goes back to master's 3 with the dialog.")
cascade_merge_pending(TESTER-LINK      # 0.99.45 / 0.99.52 beta-tester portal link
    test_tester_link_app test_link_request_poll)
cascade_merge_pending(PATCH-PRESETS    # 0.99.49 whole-patch presets
    test_patch_presets_app)
cascade_merge_pending(DISK-WORK        # 0.99.63 - 0.99.65 the window does no disk work (recorders, saves, file opens on workers)
    test_gui_file_jobs test_failure_files
    test_iq_open_async test_gui_disk_audit)
cascade_merge_pending(HEALTH-DIAG      # 0.99.61 - 0.99.69 diagnostics context, health ledger, slow frames, previous-session bundle
    test_slow_frames_app)
cascade_merge_pending(SOURCE-ROWS      # 0.99.50 / 0.99.75 source row naming, one chip one route, per-radio manual gain
    test_source_row_app)
cascade_merge_pending_part(AIRBAND display_fold_airband_section
    "tests/test_display_fold_app.cpp, scenario 'tools': the TOOLS bank must keep the Airband section ('section:Airband###airband' in the list of names the TOOLS bank keeps - master's list is in git show origin/master:tests/test_display_fold_app.cpp). The Airband section (0.99.60-0.99.71, src/gui_pending/app_window_airband.cpp) is not on this window yet, so the one name was carved out when the rest of the test returned in stub batch B1. Restore the name when drawAirbandSection returns (B7).")

# --- ADDED 2026-10-21, after the first run of the gui-labelled tests (real-binary tests; they build and run as they are, and fail only because
# the feature they read the census of is not in the window yet). The ledger id says which feature.
cascade_merge_pending(AIRBAND          # 0.99.7x the airband monitor
    test_airband_app)
cascade_merge_pending(MARKERS-SPECTRUM # 0.99.5x - 0.99.82 waterfall markers, peak hold / average / trace modes, close names stack, Shift+wheel slide
    test_bookmark_stacking_app test_freq_markers_app test_spectrum_trace_app test_passband_slide_app)
cascade_merge_pending(DISPLAY-RANGE    # 0.99.7x the spectrum's and waterfall's range set by hand, the display menu and its tip (the folded Display section returned in B1)
    test_display_menu_app)
cascade_merge_pending(PLUGIN-SETTINGS-UI
    test_decoder_settings_fold_app test_plugin_pathpick_app)
cascade_merge_pending(PLUGIN-RUN       # 0.99.7x a plugin that is not used is dormant (FOXSDR_DORMANT_AFTER_MS), run modes
    test_plugin_inuse_app)
cascade_merge_pending(PLUGIN-ROW-KEYS
    test_row_keys_app)
cascade_merge_pending(STORE-VIEW
    test_plugin_dup_app test_plugin_store_app)
cascade_merge_pending(UPDATES          # 0.99.7x the Store's own update notice and the site banner
    test_store_update_app)
cascade_merge_pending(IMPORT-DIALOG
    test_import_dialog_app)
cascade_merge_pending(RADIO-SETUP      # 0.99.7x the radio setup page
    test_radio_setup_app)
cascade_merge_pending(TESTER-USAGE     # 0.99.44 - 0.99.52 the team's messages to a tester, the usage section
    test_team_message_app)
cascade_merge_pending(TESTER-LINK
    test_tester_link)
cascade_merge_pending(MAP-WINDOWS      # 0.99.7x the ADS-B map's lists
    test_unplaced_list_app)
cascade_merge_pending(PATCH-VIEW       # 0.99.7x the patch page's view keys, information pane and inspector
    test_patch_canvas_nav test_patch_info_pane)
cascade_merge_pending(RAIL-CHROME
    test_theme_census)
cascade_merge_pending(WINDOW-PLACEMENT # 0.99.73 the Decoder output window and the plugin picture windows open inside the main window
    test_decoder_window_app)

# Only the names that exist as files here are kept (a name that is not a test is a typo that would be a quiet gap).
set(_cascade_pending_kept)
foreach(_t ${CASCADE_MERGE_PENDING_TESTS})
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_t}.cpp")
        list(APPEND _cascade_pending_kept ${_t})
    else()
        message(FATAL_ERROR "tests/merge_pending.cmake names '${_t}', which is not a test file")
    endif()
endforeach()
set(CASCADE_MERGE_PENDING_TESTS ${_cascade_pending_kept})

# Leave them out of the build...
foreach(_t ${CASCADE_MERGE_PENDING_TESTS})
    list(REMOVE_ITEM CASCADE_TESTS "${CMAKE_CURRENT_SOURCE_DIR}/${_t}.cpp")
endforeach()
list(LENGTH CASCADE_MERGE_PENDING_TESTS _n_pending)
message(STATUS "merge 0.99.84: ${_n_pending} tests of master's window features are pending re-expression (red placeholders, ctest -L merge-pending)")

# ...and put a red placeholder in the name's place.
macro(cascade_merge_pending_register)
    foreach(_t ${CASCADE_MERGE_PENDING_TESTS})
        add_test(NAME merge_pending_${_t}
            COMMAND ${CMAKE_COMMAND} -DTEST=${_t} -DFEATURE=${CASCADE_MERGE_PENDING_FEATURE_OF_${_t}}
                    -P ${CMAKE_CURRENT_SOURCE_DIR}/merge_pending_fail.cmake)
        set_tests_properties(merge_pending_${_t} PROPERTIES LABELS "merge-pending" TIMEOUT 30)
    endforeach()
    foreach(_p ${CASCADE_MERGE_PENDING_PARTS})
        add_test(NAME merge_pending_part_${_p}
            COMMAND ${CMAKE_COMMAND} -DPART=${_p} -DFEATURE=${CASCADE_MERGE_PENDING_PART_FEATURE_OF_${_p}}
                    "-DTEXT=${CASCADE_MERGE_PENDING_PART_TEXT_OF_${_p}}"
                    -P ${CMAKE_CURRENT_SOURCE_DIR}/merge_pending_part_fail.cmake)
        set_tests_properties(merge_pending_part_${_p} PROPERTIES LABELS "merge-pending" TIMEOUT 30)
    endforeach()
endmacro()

