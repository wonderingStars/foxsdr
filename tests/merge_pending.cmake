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
# (STORE-VIEW - 0.99.50 / 0.99.72 / 0.99.74 plugin store and Fitted modules window on the table - is whole: the engine's half returned in stub batch
# B3, the store window in B4, the Fitted modules window in B5: test_fitted_modules_window, test_fitted_modules_no_disk and test_plugin_dup_app.)
# (PLUGIN-SETTINGS-UI - the settings form of a plugin (0.99.43), BROWSE FILES beside a path setting and the folded "Plugin settings" line (0.99.74) - and
# PLUGIN-ROW-KEYS - a row in a plugin's panel as a key (0.99.77) - are whole: back in stub batch B9, test_plugin_settings_ui, test_decoder_settings_fold_app,
# test_plugin_pathpick_app, test_programme_audio_app and test_row_keys_app.)
cascade_merge_pending(TX-PLUGIN        # 0.99.74 a plugin may key the transmitter once granted and armed
    test_tx_audio_app test_tx_audio_page_app test_tx_audio_plate_app)
cascade_merge_pending_part(TX-PLUGIN transmit_page_plugin_key
    "tests/test_transmit_page.cpp section 10: the wiring of a plugin's transmit key in the window (applyPluginTransmitKey, setPluginKey, pluginMayKey, txArming_.arm, armTransmitPlugins/disarmTransmitPlugins). Restore master's section (git show origin/master:tests/test_transmit_page.cpp) when the feature is re-expressed.")
# (IMPORT-DIALOG - the "Import a frequency list" window, 0.99.80, and pickFile, the BROWSE key's file dialog - is whole: back in stub batch B8,
# test_import_dialog_app and the part excuse_cap_pickfile: tests/test_excuse_cap.cpp counts master's three native-wait call sites again.)
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

# --- ADDED 2026-10-21, after the first run of the gui-labelled tests (real-binary tests; they build and run as they are, and fail only because
# the feature they read the census of is not in the window yet). The ledger id says which feature.
# (The waterfall's markers - 0.99.57, with the press-hold rule that a long press tunes nothing - and the close bookmark names that stack - 0.99.54 - are back:
# stub batch B10, test_freq_markers_app and test_bookmark_stacking_app.)
# (MARKERS-SPECTRUM - the spectrum's peak hold and average (0.99.57) and Shift + wheel to slide the tuned band (0.99.82) - and DISPLAY-RANGE - the range set by hand,
# the display menu and its tip (0.99.76; the folded Display section returned in B1) - are whole: back in stub batch B11, test_spectrum_trace_app,
# test_passband_slide_app and test_display_menu_app.)
# (PLUGIN-RUN - 0.99.73 a plugin that is not used is dormant (FOXSDR_DORMANT_AFTER_MS), run modes - is whole: the engine's half returned in stub batch B2,
# test_plugin_inuse_app, which needs the Fitted modules window, in B5.)
cascade_merge_pending(UPDATES          # 0.99.7x the Store's own update notice and the site banner
    test_store_update_app)
# (RADIO-SETUP - the page, its INSTALL DRIVER key and the UHD images finding, 0.99.69 - 0.99.89 - is whole: back in stub batch B8,
# test_radio_setup_app.)
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

