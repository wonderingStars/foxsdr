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

# cascade_merge_pending(<ledger id> <tests...>): the tests belong to the feature the ledger names under that id.
macro(cascade_merge_pending feature)
    foreach(_t ${ARGN})
        list(APPEND CASCADE_MERGE_PENDING_TESTS ${_t})
        set(CASCADE_MERGE_PENDING_FEATURE_OF_${_t} "${feature}")
    endforeach()
endmacro()

cascade_merge_pending(RAIL-CHROME      # 0.99.44 interface size, 0.99.50 rail fold, 0.99.74 dials / status fold / TOOLS bank
    test_app_rail test_rail_fold test_rail_fold_app test_status_fold test_status_fold_app test_deck_knobs_app)
cascade_merge_pending(STORE-VIEW       # 0.99.50 / 0.99.72 / 0.99.74 plugin store and Fitted modules window on the table
    test_plugin_store_window test_fitted_modules_window test_plugin_catalogue_app test_plugin_cleanup_app
    test_plugin_update_cleanup_app)
cascade_merge_pending(PLUGIN-RESCAN    # 0.99.63 / 0.99.73 a rescan that reloads nothing when nothing changed, a reload that logs its steps
    test_plugin_rescan_skip test_plugin_rescan_log test_plugin_host_origin)
cascade_merge_pending(PLUGIN-SETTINGS-UI    # 0.99.43 CASCADE_CAP_SETTINGS_UI: fields the host draws for a plugin
    test_plugin_settings_ui)
cascade_merge_pending(PLUGIN-ROW-KEYS  # 0.99.77 a row in a plugin's panel as a key
    test_programme_audio_app)
cascade_merge_pending(TX-PLUGIN        # 0.99.74 a plugin may key the transmitter once granted and armed
    test_tx_audio_app)
cascade_merge_pending(TESTER-LINK      # 0.99.45 / 0.99.52 beta-tester portal link
    test_tester_link_app test_link_request_poll)
cascade_merge_pending(PATCH-PRESETS    # 0.99.49 whole-patch presets
    test_patch_presets_app)
cascade_merge_pending(DISK-WORK        # 0.99.63 - 0.99.65 the window does no disk work (recorders, saves, file opens on workers)
    test_gui_file_jobs test_record_finish_async test_record_start test_failure_files test_failure_recording
    test_iq_open_async test_bookmark_save_async test_patch_dest_async)
cascade_merge_pending(HEALTH-DIAG      # 0.99.61 - 0.99.69 diagnostics context, health ledger, slow frames, previous-session bundle
    test_health_app test_slow_frames_app test_diag_context_app test_diag_audio_app test_diag_history)
cascade_merge_pending(PPM              # 0.99.56 PPM frequency correction
    test_ppm_app)
cascade_merge_pending(SOURCE-ROWS      # 0.99.50 / 0.99.75 source row naming, one chip one route, per-radio manual gain
    test_autogain_app test_one_chip_app test_source_row_app)
cascade_merge_pending(NEW-RADIOS      # 0.99.49 AOR, 0.99.66 HydraSDR, 0.99.70 rtl_tcp: the window's rows, boxes and Open keys
    test_hydrasdr_app)
cascade_merge_pending(AUDIO-LEAD       # 0.99.73 / 0.99.83 the audio buffer that deepens and steps back down
    test_audio_lead_app)
cascade_merge_pending(BANDWIDTH        # 0.99.61 the Bandwidth control narrows what you hear
    test_bandwidth_app)
cascade_merge_pending(METER-STYLES     # 0.99.48 bench meter styles
    test_meter_style_app)

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
endmacro()
