// tools_logic.hpp - THE TOOLS BANK'S PURE LOGIC (docs/engine-stage4.md, the Slint port, phase 3 bank 3): what the open bodies under the TOOLS rows show -
// the Demod scope row's chip, Radar (the position offers and the scope key), Bookmarks, Scanner, Airband and the Markers sheet - is computed from the
// C API table's lists and facts and the state the engine published by these functions, and every control the operator touches becomes the exact
// EFFECT the ImGui window's own control has: an op (with the grant the engine asks for), or a local change the publisher keeps, or both. No Slint type
// and no engine header appears here (foxsdr_api.h, core/app_commands.hpp and the light core/gui headers named below), so the file is tested without
// a window or an engine (tests/test_slint_host_tools.cpp reads a stub table, as the SIGNAL and SYSTEM banks' tests do).
//
// THE CONTRACT is ui/tools_bank.slint's (the UI family's; the note slint-tools-bank-contract.md): the models below are the Slint structs with the same
// fields in snake_case; the publisher (tools_publish.*, not this file) maps them 1:1 and wires each ToolsEvents callback to the effect function the
// table below names. A field marked "host extra" is not in the contract - the publisher may use it or leave it.
//
// WHERE THE RULES COME FROM (the ImGui window, read for behaviour and for every sentence, never for drawing):
//   Demod scope  drawDemodScopeSection (app_window.cpp 13336)           Radar     drawRadarSection (11188) + drawScopeModeControl (13361) +
//                                                                                 drawReceiverPositionOffers (10941) + drawRxPositionEntry (10855)
//   Bookmarks    drawBookmarksSection (19409) + rebuildBookmarkView (19365) + gui/section_reads.hpp (the view, the groups)
//   Scanner      drawScannerSection (19950) + commitScannerDraft + gui/section_reads.hpp (the form values and the three commands)
//   Airband      app_window_airband.cpp (the whole section)             Markers   app_window_markers.cpp (drawMarkerWindow, markerLists)
// and the sentences are those drawers' own, through cascade::i18n::tr() (the catalogues already hold them; this file adds NO key).
//
// THE DEFERRED PART (the brief: "the patch canvas and scope mode are bespoke, next allowance"): the DEMOD SCOPE window and the RADAR SCOPE full-window mode.
// Their rail rows keep their chips and their keys; the keys are [local] "Deferred" effects the publisher logs.
//
// WHERE THE ENGINE ANSWERS LATE. Several ImGui controls send an op and settle their note or the group on show when the answer comes (airbandOnAnswer). The
// Effect says which answer it waits for (Effect::ask); the publisher keeps the ticket and does what airbandOnAnswer does. The disk work (the read of a
// frequency list, the lookup of a path with no root) is the publisher's worker too: the Effect says so (Local::ImportLookup, Local::PresetImportLookup).
//
// THE CALLBACKS -> EFFECTS (every apply... returns an Effect; "[local]" = no op: the publisher keeps the state; a function that returns an empty Effect does
// nothing, exactly as the ImGui control would not; "ask" = the answer the op waits for):
//
//   scope-clicked()              applyScopeClicked        [local] Deferred "demod-scope" (the window is not built yet; the publisher logs it)
//   radar-scope-clicked()        applyRadarScopeClicked   [local] Deferred "radar-scope"
//   position-heard()             applyPositionHeard       FOXAPI_OP_SET_POSITION 0x0F01 num[0] = lat, num[1] = lon of the middle of the aircraft heard (the publisher's mean, from
//                                                         scopeTrafficCentre); nothing below three aircraft or from a pair receiverPositionAcceptable refuses; coverage_reset
//   position-map()               applyPositionMap         SET_POSITION from the map page's centre (ReadOptions::map_*); nothing from an unacceptable centre; coverage_reset
//   position-lat(text)           applyPositionLat         [local] PositionLat = the typed latitude (ReadOptions::lat_text is it, given back each publish)
//   position-lon(text)           applyPositionLon         [local] PositionLon
//   position-set()               applyPositionSet         SET_POSITION from the typed pair (a field emptied is 0; text that is no number sends nothing), only when
//                                                         receiverPositionAcceptable says so (0,0 and out-of-range are refused); coverage_reset
//   bookmark-name(text)          applyBookmarkName        [local] BookmarkName
//   bookmark-add()               applyBookmarkAdd         FOXAPI_OP_BOOKMARK_ADD 0x0501 text = name when the group box (trimmed) is empty; else FOXAPP_OP_BOOKMARK_GROUP 0x860B ival[0] = 0,
//                                                         text = group + 0x1F + name. The name stays (Add current does not clear it)
//   bookmark-group(text)         applyBookmarkGroup       [local] BookmarkGroup (picking from the arrow types the name)
//   bookmark-import-path(text)   applyBookmarkImportPath  [local] BookmarkImportPath
//   bookmark-import()            applyBookmarkImport      nothing while an import is out; the path cleaned (core::cleanImportPath): empty = [local] OpenImportDialog; a path with a root =
//                                                         FOXAPP_OP_BOOKMARK_IMPORT_FILE 0x8601 text = the path (the engine reads it on its worker; fact.bookmarks.import.* answers); any other =
//                                                         [local] ImportLookup text = the path (the publisher looks for it on a worker, as importBookmarkFile does)
//   bookmark-search(text)        applyBookmarkSearch      [local] BookmarkSearch
//   bookmark-filter(i)           applyBookmarkFilter      [local] BookmarkFilter text = the group's name ("" = every group): the choice is kept BY NAME, as rebuildBookmarkView keeps it
//   bookmark-favourites(on)      applyBookmarkFavourites  [local] BookmarkFavourites
//   bookmark-on-spectrum(on)     applyBookmarkOnSpectrum  [local] BookmarkOnSpectrum
//   bookmark-stack(on)           applyBookmarkStack       [local] BookmarkStack; nothing while "On the spectrum" is off (the box is disabled)
//   bookmark-export()            applyBookmarkExport      FOXAPP_OP_BOOKMARK_EXPORT 0x8603 long text = the ids of the rows SHOWN, each followed by one space; nothing for an empty view or while
//                                                         an export is being written
//   bookmark-remove-group()      applyBookmarkRemoveGroup FOXAPI_OP_BOOKMARK_REMOVE_GROUP 0x0507 text = the group chosen; [local] BookmarkFilter "" (back to every group), the rename field closes;
//                                                         nothing with no group chosen
//   bookmark-rename-draft(text)  applyBookmarkRenameDraft [local] BookmarkRenameDraft
//   bookmark-rename()            applyBookmarkRename      FOXAPP_OP_BOOKMARK_GROUP 0x860B ival[0] = 2, text = from + 0x1F + to (to = the draft trimmed; nothing when it is empty or the same as from, or no
//                                                         group is chosen); [local] BookmarkRenamed text = to (the filter follows the group to its new name; the field closes).
//   bookmark-rename-open(open)  applyBookmarkRenameOpen  [local] BookmarkRenameOpen: the "Rename this group" key showed (true) or hid (false) the field; the draft is seeded with
//                                                         the group's own name every time (the ImGui key's rule), so an abandoned name does not come back (docs 6.61). The key
//                                                         TOGGLES in the effect, so the publisher hands it the model with rename_open = !open
//   bookmark-tick(id, on)        applyBookmarkTick        FOXAPP_OP_BOOKMARK_TICK 0x8604 ival[0] = id, ival[1] = on; nothing for an id the rows shown do not hold
//   bookmark-favourite(id, on)   applyBookmarkFavourite   FOXAPI_OP_BOOKMARK_FAVOURITE 0x0505 ival[0] = id, ival[1] = on
//   bookmark-tune(id)            applyBookmarkTune        FOXAPI_OP_BOOKMARK_TUNE 0x0502 ival[0] = id (grant TUNE)
//   bookmark-delete(id)          applyBookmarkDelete      FOXAPI_OP_BOOKMARK_REMOVE 0x0503 ival[0] = id
//   bookmark-menu(id)            applyBookmarkMenu        [local] BookmarkMenu number = the row whose menu is open (the right-click); -1 closes it
//   bookmark-move(id, group)     applyBookmarkMove        FOXAPP_OP_BOOKMARK_GROUP 0x860B ival[0] = 1, ival[1] = id, text = the group trimmed ("" = no group); nothing when the row is
//                                                         already in that group; [local] BookmarkMenu -1 (the menu closes)
//   scanner-ticked(on)           applyScannerTicked       FOXAPP_OP_SCANNER_LIST 0x860A ival[0] = on (list mode / range mode; grant TUNE)
//   scanner-start / -stop / -step / -dwell / -hold / -resume / -listen (text)
//                                applyScannerField        the field COMMITTED (the contract's callbacks are commits; the ImGui box stores as it is typed while idle and on deactivate while
//                                                         running - both end in commitScannerDraft): a timing field (dwell, hold, resume, listen) = FOXAPI_OP_SCANNER_CONFIG 0x0603 num[0..3] = the
//                                                         four timings (the edited one from the text, the rest the engine's); a range field (start, stop, step) = FOXAPP_OP_SCANNER_RANGE 0x8602
//                                                         ival[0] = its bit (1 start, 2 stop, 4 step) | 8 while the scan RUNS, num[0..2] = start Hz, stop Hz, step Hz (the edited one from the text,
//                                                         the rest the engine's); text that is no number, or empty, sends nothing. [local] ScannerDraft number = the field
//   scanner-run()                applyScannerRun          list mode: FOXAPP_OP_SCANNER_LIST ival[0] = 2 (nothing with no ticked row); else FOXAPI_OP_SCANNER_RUN 0x0601 ival[0] = 1,
//                                                         num[0..2] = the range the engine stores (scannerStartCommand)
//   scanner-halt()               applyScannerHalt         FOXAPI_OP_SCANNER_RUN ival[0] = 0 (the key is drawn only while scanning: nothing otherwise)
//   scanner-skip()               applyScannerSkip         FOXAPI_OP_SCANNER_SKIP 0x0602 (while scanning)
//   airband-code(text)           applyAirbandCode         [local] AirbandCode = the text in capitals (the box's CharsUppercase)
//   airband-lookup()             applyAirbandLookup       nothing with no code. No airport: set_note "No airport with the code..." . One airport: FOXAPP_OP_BOOKMARK_ADD_ROWS 0x8605 ival[0] = 1,
//                                                         long text = the airport's rows (one line each, "<Hz>\t<mode>\t<bw>\t<flags>\t<group>\t<name>"), ask AirportRows. Several: set_choices +
//                                                         set_note "\"%s\" names %zu airports - pick one:". Always clear_choices first (the previous choices go)
//   airband-nearest()            applyAirbandNearest      set_nearest = the six airports nearest the receiver (core::nearestAirports), clear_choices; nothing while the position is unknown
//   airband-hide()               applyAirbandHide         clear_nearest
//   airband-airport(i)           applyAirbandAirport      i < 1000: the i-th NEAREST airport; 1000 + i: the i-th CHOICE (the ImGui rows' ids). The add as a lookup of one airport does it
//                                                         (BOOKMARK_ADD_ROWS, ival[0] = 1, ask AirportRows); clear_choices, and clear_nearest for a nearest one
//   airband-ambiguous(i)         applyAirbandAirport      (the UI family added it: a row of the lookup choices) applyAirbandAirport(options, kChoiceBase + i)
//   airband-tune(id)             applyAirbandTune         (the UI family added it: a click on a row) FOXAPP_OP_AIRBAND ival[0] = 6, ival[1] = id - the monitor stops and the receiver tunes there alone
//   airband-preset(i)            applyAirbandPreset       [local] AirbandPresetName = presets[i] as presetFieldText cuts it (128-byte field: a name that does not fit gives "")
//   airband-preset-name(text)    applyAirbandPresetName   [local] AirbandPresetName
//   airband-freq / -name (text), -mode(i)   applyAirbandFreq / Name / Mode   [local] AirbandFreq / AirbandName / AirbandMode
//   airband-add()                applyAirbandAdd          nothing with no frequency. A frequency core::parseMhz refuses: set_note "could not read frequency \"%s\"". Else FOXAPP_OP_BOOKMARK_ADD_ROWS
//                                                         ival[0] = 0, text = ONE row (AM or NFM at the default bandwidth, the preset's group, ticked), ask TypedRow
//   airband-import-path(text)    applyAirbandImportPath   [local] AirbandImportPath
//   airband-import()             applyAirbandImport       nothing while a read is out. Empty box: [local] OpenImportDialog (the preset's, flag set). Else clear_preset_note, clear_folder_key and
//                                                         [local] PresetImportLookup text = the cleaned path, text2 = the preset (the worker reads the file; the publisher sends ADD_ROWS chunks)
//   airband-export()             applyAirbandExport       FOXAPP_OP_BOOKMARK_EXPORT ival[0] = 1, text = the preset, ask ExportPreset, clear_folder_key; nothing without rows or while one is being written
//   airband-remove-preset()      applyAirbandRemovePreset FOXAPI_OP_BOOKMARK_REMOVE_GROUP text = the preset, ask RemoveGroup, clear_folder_key; nothing without rows
//   airband-folder()             applyAirbandFolder       [local] OpenFolder text = fact.recorder.dir
//   airband-group(i)             applyAirbandGroup        [local] AirbandGroup text = the group on show (index 0 = every ticked row); text2 = the preset field's new text when a group was picked
//                                                         (flag); and, when the group CHANGED while the monitor listens or waits, FOXAPP_OP_AIRBAND 0x8429 ival[0] = 1 text = the group (ask Listen)
//   airband-tick-all / -untick-all()   applyAirbandTickAll   [local] TickRun ids = the rows listed that differ (every one when `cover_all`: a run is out), flag = on; only with a group on show
//                                                         and rows listed
//   airband-busiest(on)          applyAirbandBusiest      [local] AirbandBusiest
//   airband-tick(id, on)         applyAirbandTick         FOXAPP_OP_BOOKMARK_TICK ival[0] = id, ival[1] = on
//   airband-squelch(norm)        applyAirbandSquelch      FOXAPP_OP_AIRBAND ival[0] = 4, num[0] = dB (-110 + norm * 110, clamped); [local] SquelchHeld value = dB. Sent on RELEASE
//   airband-above-noise()        applyAirbandAboveNoise   FOXAPP_OP_AIRBAND ival[0] = 3; nothing unless the monitor listens
//   airband-hold(text)           applyAirbandHold         FOXAPP_OP_AIRBAND ival[0] = 5, num[0] = seconds clamped 0..60 (the first number in the text; "hold 2.0 s" reads as 2.0); [local] HoldHeld value
//   airband-listen()             applyAirbandListen       idle: FOXAPP_OP_AIRBAND ival[0] = 1, text = the group on show (ask Listen); listening or waiting: ival[0] = 0 (STOP)
//   airband-next()               applyAirbandNext         FOXAPP_OP_AIRBAND ival[0] = 2; nothing unless the plan has more than one block
//   markers-open()               applyMarkersOpen         [local] MarkersOpen
//   markers-note(number, text)   applyMarkerNote          FOXAPP_OP_MARKER_NOTE 0x8609 ival[0] = number, text (or long text) = the note as typed; [local] MarkerNoteHold number, text (the box keeps it
//                                                         thirty frames, until the list has it back)
//   markers-remove(number)       applyMarkerRemove        FOXAPP_OP_MARKER_REMOVE 0x8607 ival[0] = number
//   markers-clear()              applyMarkersClear        FOXAPP_OP_MARKER_CLEAR 0x8608 (the Slint key asks "Yes, remove all N" first); nothing for an empty list
//   markers-tune(number)         applyMarkerTune          FOXAPI_OP_SET_FREQUENCY 0x0103 num[0] = the marker's frequency, as the window's Tune key does (grant TUNE). The brief said VFO_TO_ABSOLUTE; the
//                                                         ImGui key sends SET_FREQUENCY, which is absolute already
//   markers-bookmark(number)     applyMarkerBookmark      (the UI family added it) FOXAPP_OP_BOOKMARK_ADD_ROWS ival[0] = 1, one row (the note or "M<n>" names it, the current mode and bandwidth)
//   markers-copy()               applyMarkersCopy         (the UI family added it) [local] Clipboard text = core::formatMarkersClipboard of the list; nothing for an empty list
//   markers-closed()             applyMarkersClosed       [local] MarkersClosed
//
// THE RADAR FOLD GPS ROW is the SYSTEM bank's (system_logic.hpp readSerialPorts, the GpsRow struct the UI family imports): ReadOptions::gps carries it and the offers pass it
// on; gps-port, gps-baud and gps-toggle are the SystemEvents callbacks and the SYSTEM bank effects (applyGpsPort / applyGpsBaud / applyGpsToggle), not this file's.
//
// WHAT IS NOT COVERED: the Bookmarks spectrum markers and the drop-a-file import (the window's), the import dialog (the SIGNAL bank's), the radar scope's data panel and
// the demod scope's faces (deferred).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include "foxsdr_api.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "core/airband_data.hpp"
#include "core/app_commands.hpp"
#include "core/freq_markers.hpp"
#include "gui/module_census.hpp"
#include "gui/tuning_reads.hpp"
#include "kit_logic.hpp"
#include "signal_logic.hpp"
#include "system_logic.hpp"

namespace foxsdr::slinttools {

using foxsdr::slintkit::TableReader;
using foxsdr::slintsignal::Command;
using foxsdr::slintsignal::SelectItem;
using foxsdr::slintsignal::Tone;

// ---- the constants the ImGui window states in its drawers ----
constexpr int kScannerFieldCount = 7;                 // start MHz, stop MHz, step kHz, dwell, hold, resume, listen ms (section_reads.hpp's order)
enum ScannerField : int { kFieldStart = 0, kFieldStop, kFieldStep, kFieldDwell, kFieldHold, kFieldResume, kFieldListen };
constexpr double kSquelchMinDb = -110.0, kSquelchMaxDb = 0.0;   // the slider's span
constexpr double kHoldMinS = 0.0, kHoldMaxS = 60.0;
constexpr int kTrafficCentreMinAircraft = 3;           // gui/scope_view.hpp kScopeTrafficCentreMinAircraft (that header draws: a test reads the source)
constexpr int kNearestAirports = 6;
constexpr int kChoiceBase = 1000;                      // airband-airport(i): 1000 + i is the i-th ambiguous choice (the ImGui rows' ids)
constexpr int kNoteHoldFrames = 30;                    // markerNoteHold_: the box keeps what was typed this many frames
constexpr std::size_t kPresetFieldBytes = 128;         // airband_.presetName's size

// ---- the models (ui/tools_bank.slint's structs, snake_case) ----
struct PositionOffers {
    bool known = false;
    std::string known_text;                // "51.50000N 0.12000W" once a position is set
    std::string heard_label;               // "Use the middle of the 4 aircraft heard  (51.50N 0.12W)" / "... (2 heard, need 3)" / "... (none heard yet)"
    bool heard_enabled = false;
    std::string map_label;                 // "Use the ADS-B map's centre  (51.50N 0.12W)"; "" when no map page offers an acceptable centre
    bool map_enabled = false;
    std::string hint;                      // "or type it, or click SET FROM MAP CLICK on a map page:"
    std::string lat_text, lon_text;        // the typed entry
    std::string error;                     // why the Set RX here key is dead: "" when it is live or nothing was typed
    // ---- host extras (the UI family added these to the struct: set-label, set-disabled, set-tooltip, gps-shown, gps-hint, gps, footnote) ----
    bool set_enabled = false;              // the typed pair is acceptable (receiverPositionAcceptable)
    bool set_disabled = true;              // ...the same, the other way up
    std::string set_label;                 // "Set RX here"
    std::string set_tooltip;               // why the key is dead (the ImGui key has two tooltips; the Gulf of Guinea one while nothing is typed); "" while it is live
    bool gps_shown = true;                 // the GPS row is drawn under the offers and in the fold (drawGpsPositionControl)
    std::string gps_hint;                  // "or read it from a GPS receiver on a serial port:"
    foxsdr::slintsystem::GpsRow gps;       // the SYSTEM bank row, as readSerialPorts made it (ReadOptions::gps): its callbacks are the SystemEvents
    std::string footnote;                  // "All serial-port settings are also under SYSTEM > Serial ports."
    std::string serial_note;               // (the same sentence, by the name this file first gave it)
    std::string heard_tooltip, map_tooltip;
};

struct RadarModel {
    std::string warning;                   // "The scope needs the receiver's position first." while there is none
    PositionOffers offers;
    std::string scope_label;               // "Radar scope" / "Leave radar scope"
    bool scope_on = false;
    std::string scope_note;                // "The scope will be empty. <why>" while no track source is live
    // ---- host extras ----
    std::string scope_tooltip;
};

struct BookmarkRow {
    int id = 0;                            // the engine's id (the commands name it)
    bool ticked = false;
    bool favourite = false;
    std::string text;                      // "name  145.5000 MHz  [group]" as the ImGui row prints it
    std::string group;
    std::string hz_text;                   // "145.5000 MHz"
    std::string mode;                      // "NFM", "AM" ... "" for a code no name carries
    // ---- host extras ----
    double hz = 0.0;
    std::string name;
};

struct BookmarksModel {
    std::string name_draft;
    std::string add_label;                 // "Add current"
    std::string group_draft;
    std::vector<SelectItem> groups;        // the groups the list holds, sorted (the arrow beside the group box and the row menu's list); label = id = the name
    std::string import_path;
    std::string import_label;              // "Import"
    std::string import_note;               // fact.status.bookmarkNote: the result of an import, rename or removal
    std::string disk_wait;                 // "Waiting for the disk to open the file: 2 s" from one second of a read
    std::string search;
    std::vector<SelectItem> filter_groups; // "Every group" first, then each group; EMPTY while the list holds no group (the ImGui combo is not drawn)
    int filter_index = 0;
    bool favourites_only = false;
    bool on_spectrum = true;
    bool stack_names = true;
    bool stack_enabled = true;             // the box is disabled while "On the spectrum" is off
    std::string count_line;                // "5 shown of 7"
    std::string export_label;              // "Export for SDR#" ("" for an empty view)
    std::string remove_group_label;        // "Remove this group" ("" with no group chosen)
    std::string rename_group_label;        // "Rename this group" ("" with no group chosen)
    std::string rename_draft;
    std::vector<BookmarkRow> rows;
    int menu_row = -1;                     // the row id whose right-click menu is open, -1 none
    std::string error;                     // fact.status.bookmarkError
    // ---- host extras ----
    std::string chip;                      // the rail row's chip: how many are saved
    bool lamp_lit = false;
    bool import_enabled = true;            // no read is out
    bool export_enabled = true;            // no export is being written
    bool import_busy = false, export_busy = false;   // the same, the other way up (the UI names)
    bool rename_open = false;              // the rename field is drawn (a group is chosen and the key opened it)
    std::string filter_group;              // the group the list is filtered to ("" every group): the publisher keeps it BY NAME
    std::size_t total = 0, shown = 0;
};

struct ScannerModel {
    bool ticked_mode = false;              // fact.scanner.listMode
    std::string ticked_label;              // "Ticked frequencies (3)"
    std::string start_mhz, stop_mhz, step_khz, dwell_ms, hold_ms, resume_ms, listen_ms;
    bool range_enabled = true;             // the range's three fields stand aside in list mode
    bool scanning = false;
    std::string start_label;               // "Start scan"
    std::string stop_label;                // "Stop scan"
    std::string skip_label;                // "Skip"
    std::string status;                    // "Scanning | 145.5000 MHz"
    Tone status_tone = Tone::Neutral;      // Idle neutral, Scanning and Paused good (the scanner is working), Holding warn (waiting for the signal to come back)
    // ---- host extras ----
    bool start_enabled = true;             // not in list mode with nothing ticked
    bool start_disabled = false;           // the same, the other way up (the UI name)
    std::size_t ticked_count = 0;
    std::string chip;                      // SCAN / IDLE
    bool lamp_lit = false;
    unsigned draft_bits = 0;               // the fields that show a typed draft and not the engine's value
};

struct AirbandRow {
    int id = 0;
    bool ticked = false;
    bool heard = false;                    // the lamp: the channel's squelch is open now (needs the monitor listening)
    std::string text;                      // "118.1000  TWR 118.100"
    std::string heard_time;                // "1h 02m", "12m 30s", "45s", "-"
    bool on_air = false;                   // the channel is in the block on the air
    // ---- host extras ----
    std::string tooltip;                   // "Level -42 dB (squelch -55 dB)" while the channel is on the live list
    double hz = 0.0;
};

struct AirportRow {
    std::string code;                      // the ident
    std::string name;
    std::string distance;                  // "34 km" (the nearest list)
    std::string note;                      // "(US)" (the ambiguous list)
};

struct AirbandModel {
    std::string code_draft;
    std::string lookup_label;              // "Look up"
    std::string nearest_label;             // "Nearest airports"
    std::string hide_label;                // "Hide" (while the nearest list is shown)
    std::vector<AirportRow> airports;      // the nearest list
    std::vector<AirportRow> ambiguous;     // the lookup's choices
    std::string preset_name;
    std::vector<SelectItem> presets;       // the groups that hold a row the monitor plays (the group list's own, without the "every ticked" row)
    int preset_index = -1;                 // the preset field's group in `presets`, -1 when it names none
    std::string freq_draft;
    std::vector<SelectItem> modes;         // AM, NFM
    int mode_index = 0;
    std::string name_draft;
    std::string add_label;                 // "Add"
    std::string note;
    std::string import_path;
    std::string import_label;              // "Import"
    std::string export_label;              // "Export CSV"
    std::string remove_preset_label;       // "Remove preset"
    std::string preset_note;
    std::string folder_label;              // "Open the folder" after an export that wrote its file, else ""
    std::vector<SelectItem> groups;        // "Every ticked AM or NFM frequency" first (id ""), then each playable group
    int group_index = 0;
    std::string count_line;                // "3 ticked of 7"
    std::string tick_all_label;            // "Tick all" ("" unless a group is on show and has rows)
    std::string untick_all_label;
    bool busiest_first = false;
    std::string hearing;                   // the two status lines joined by a newline: the block on the air and "Hearing: ..." ("Hearing: -" when quiet); the pending sentence while waiting
    std::vector<AirbandRow> rows;
    float squelch_norm = 0.5f;             // (dB + 110) / 110
    std::string squelch_text;              // "squelch -55 dB"
    std::string above_noise_label;         // "Above the noise"
    std::string hold_text;                 // "hold 2.0 s"
    std::string listen_label;              // "LISTEN" / "STOP"
    bool listening = false;
    std::string next_label;                // "Next block" while the plan has more than one block, else ""
    // ---- host extras ----
    std::string chip;                      // LISTEN / IDLE
    bool lamp_lit = false;
    std::string block, hearing_line;       // the two lines of `hearing`
    bool talking = false;                  // someone is heard (the second line is amber)
    bool pending = false;                  // a LISTEN waits for the patch to hand the radio back
    std::string fact_note;                 // fact.airband.note: the publisher mirrors it into `note` when it changes (airbandFrame)
    bool nearest_disabled = true;          // no receiver position (the UI name for !nearest_enabled)
    bool import_busy = false, export_busy = false;   // a read is out / the Export CSV key is dead (no rows in the preset, or one is being written)
    bool lookup_enabled = false, nearest_enabled = false, add_enabled = false, import_enabled = true, export_enabled = false, remove_enabled = false;
    bool above_noise_enabled = false;
    bool squelch_from_engine = true;       // the slider shows the engine's figure, not a hand's
    double squelch_db = -55.0, hold_s = 2.0;
    std::string disk_wait;                 // "Waiting for the disk to open the file: 2 s"
    std::string group;                     // the group on show ("" every ticked row)
    std::string preset;                    // the preset the keys act on (the field trimmed, "Manual" when empty)
    std::size_t preset_rows = 0;           // every row of that preset, whatever its mode or tick
    int blocks = 0;
};

struct MarkerRow {
    int number = 0;
    std::string hz_text;                   // "145.500" (core::markerMhzText), the sheet adds " MHz"
    std::string note;
    std::string dropped;                   // "2026-09-30 14:03:22" local time
    // ---- host extras ----
    double hz = 0.0;
    std::int64_t dropped_unix = 0;
    std::string label;                     // "M3"
};

struct MarkersModel {
    bool shown = false;
    std::vector<MarkerRow> rows;           // in frequency order (the sheet is for working through the band)
    std::string error;                     // fact.markers.error
    std::string clear_label;               // "Clear all markers" ("" for an empty list)
    std::string empty_line;                // "No markers yet. Right-click the waterfall and choose Drop marker here." while the list is empty
    // ---- host extras ----
    std::string clear_confirm;             // "Yes, remove all 3"
    std::string clear_yes_label;           // (the same sentence, by the UI name)
    std::string copy_label;                // "Copy markers to clipboard" ("" for an empty list: the key is dead)
    std::size_t count = 0;
};

// The Demod scope row and the other four rows' chips.
struct RowChip {
    std::string word;
    Tone tone = Tone::Good;
    bool lit = false;
};
enum Row : int { kRowDemodScope = 0, kRowRadar, kRowBookmarks, kRowScanner, kRowAirband };
constexpr int kToolsRowCount = 5;

struct ToolsModels {
    RadarModel radar;
    BookmarksModel bookmarks;
    ScannerModel scanner;
    AirbandModel airband;
    MarkersModel markers;
    std::array<RowChip, kToolsRowCount> chips;   // Demod scope (AUDIO / SPEC / I/Q / VECTOR / MPX), Radar (OFF), Bookmarks (the count), Scanner (SCAN / IDLE), Airband (LISTEN / IDLE)
};

// ---- what the publisher keeps between reads: the copies the ImGui window keeps (tuningLists, markerCache_) ----
struct BookmarkStore {
    std::string version;                   // fact.bookmarks.version the rows were read at; "" never
    std::vector<cascade::gui::BookmarkRow> rows;
    unsigned copies = 0;
};
struct MarkerStore {
    std::string version;                   // fact.markers.version the rows were read at; "" never
    std::vector<cascade::core::FreqMarker> rows;
    std::string error;
    unsigned copies = 0;
};
struct ToolsCache {
    BookmarkStore bookmarks;
    MarkerStore markers;
};

struct NoteHold {                          // markerNoteHold_
    int number = 0;
    std::string text;
    int frames = 0;
};
using NearestAirport = std::pair<const cascade::core::Airport*, double>;

// What the publisher owns and hands in each time (the drafts, the open menu, the pages' flags, the facts the window itself computes).
struct ReadOptions {
    ToolsCache* cache = nullptr;           // the copies kept between reads; null = read fresh each time (the tests'; a list of tens of thousands costs)
    // the Demod scope row
    int scope_signal = 0;                  // demodScope_.signal (ScopeSignal: AUDIO, SPEC, I/Q, VECTOR, MPX)
    bool scope_open = false, scope_live = false;
    // Radar
    bool radar_scope_on = false;
    cascade::gui::ModuleCensus track_census;    // the census of the track sources (gui::censusOf over the PLUGINS rows with CASCADE_CAP_TRACK_SOURCE): live == 0 says the scope will be empty
    std::string lat_text, lon_text;        // the typed position entry (kept in step with the state by followPosition)
    foxsdr::slintsystem::GpsRow gps;       // the GPS row of the Radar fold: slintsystem::readSerialPorts(...).gps (the publisher reads the SYSTEM bank once and hands it to both)
    int traffic_heard = 0;                 // scopeTrafficCentre's count over the host's tracks, and the mean it wrote
    double traffic_lat = 0.0, traffic_lon = 0.0;
    bool map_have = false;                 // a map page with a view exists (the open one first)
    std::string map_plugin;
    double map_lat = 0.0, map_lon = 0.0;
    // Bookmarks
    std::string name_draft, group_draft, import_path, search, rename_draft;
    std::string filter_group;              // the group the list is filtered to, by name ("" every group)
    std::string rename_want;               // a group just renamed: the filter follows it once the list has it (bookmarkRenameWant_)
    bool favourites_only = false, on_spectrum = true, stack_names = true;
    bool rename_open = false;
    int menu_row = -1;
    bool import_lookup_pending = false;    // the lookup of a path with no root is on its worker
    double import_elapsed_s = 0.0;         // how long the read has been out
    // Scanner
    std::array<std::string, kScannerFieldCount> scan_draft;
    unsigned scan_draft_bits = 0;          // bit i: field i shows its draft (typed, or an edit waiting for a running scan)
    // Airband
    std::string air_code, air_preset = "Manual", air_freq, air_name, air_import_path, air_group, air_note, air_preset_note;
    int air_mode = 0;
    bool air_busiest = false, air_folder_key = false, air_export_waiting = false, air_import_pending = false;
    double air_import_elapsed_s = 0.0;
    std::vector<NearestAirport> nearest;
    std::vector<const cascade::core::Airport*> choices;
    bool squelch_held = false;
    float squelch_edit = -55.0f;
    bool hold_held = false;
    double hold_edit = 2.0;
    // Markers
    bool markers_shown = false;
    NoteHold note_hold;
};

// ---- the read: lists and facts through the seam, the state as the engine published it ----
RowChip readScopeChip(const ReadOptions& options);
RadarModel readRadar(const FoxReceiverState& state, const ReadOptions& options);
PositionOffers readPositionOffers(const FoxReceiverState& state, const ReadOptions& options);
BookmarksModel readBookmarks(TableReader& table, const ReadOptions& options);
ScannerModel readScanner(TableReader& table, const FoxReceiverState& state, const ReadOptions& options);
AirbandModel readAirband(TableReader& table, const FoxReceiverState& state, const ReadOptions& options);
MarkersModel readMarkers(TableReader& table, const ReadOptions& options);
ToolsModels readTools(TableReader& table, const FoxReceiverState& state, const ReadOptions& options);

// The copies, refreshed by the version rule of tuningLists / markerLists (the rows are read again only when the store's own counter moved).
void refreshBookmarks(TableReader& table, BookmarkStore& store);
void refreshMarkers(TableReader& table, MarkerStore& store);

// ---- the small pure rules ----
// The position entry: "%.5f" of a coordinate, and the state's position into the typed fields when it moved (followReceiverPosition); true when it moved.
std::string coordinateText(double deg);
struct PositionSeen { bool set = false; double lat = 0.0, lon = 0.0; };
bool followPosition(const FoxReceiverState& state, PositionSeen& seen, std::string& latText, std::string& lonText);
// A typed coordinate as InputDouble reads it: surrounding blanks dropped, an empty box is not a change (false), a whole number or nothing (false).
bool parseCoordinate(const std::string& text, double& out);
// "51.50N 0.12W" with the language's hemisphere letters, in the offers' own format ("%.2f%s %.2f%s").
std::string offerPlace(double lat, double lon);
std::string knownPlace(double lat, double lon);                  // "51.50000N 0.12000W"
// Why the Set RX here key is dead, in the tooltip's sentence ("" when it is live, or when both boxes are empty).
std::string positionError(const std::string& latText, const std::string& lonText);
// The track sources' census over the kit's PLUGINS rows (gui::censusOf's rule): what ReadOptions::track_census is.
cascade::gui::ModuleCensus trackCensus(const std::vector<foxsdr::slintkit::PluginRow>& rows);
// The chip word of the scope row.
std::string scopeChipWord(int signalIndex);
// The scanner's state word ("Idle", "Scanning", "Paused (signal)", "Holding", "?"), its tone, and the status line.
std::string scannerStateWord(std::uint32_t state);
Tone scannerTone(std::uint32_t state);
std::string scannerStatusLine(std::uint32_t state, double currentHz);
// The seven field texts of the form, in the ImGui formats ("%.4f", "%.4f", "%.2f", "%.0f" x4), from the state.
std::array<std::string, kScannerFieldCount> scannerFieldTexts(const FoxReceiverState& state);
// A bookmark group name as typed, less the blanks around it (app_window.cpp trimmedGroupName).
std::string trimmedGroupName(const std::string& typed);
// "1h 02m", "12m 30s", "45s"; "-" for never.
std::string heardText(double seconds);
// The rows a frequency list gives the engine: "<Hz>\t<mode>\t<bandwidth Hz>\t<flags>\t<group>\t<name>\n" (a tab or a break in a field is a space).
std::string bookmarkRowLine(const cascade::core::Bookmark& b, bool tick);
std::vector<std::string> bookmarkRowChunks(const std::vector<cascade::core::Bookmark>& items);   // cut at 512 KiB on a line end
// The markers as the clipboard takes them.
std::string markersClipboard(const MarkersModel& m);
// What the monitor can play: AM and NFM.
bool airbandPlays(std::uint32_t modeCode);

// ---- the effects ----
// What is local (the publisher keeps the state) or leaves the program without an op.
enum class Local {
    None,
    Deferred,                              // text = which: "demod-scope", "radar-scope"
    PositionLat, PositionLon,              // text = the typed draft
    BookmarkName, BookmarkGroup, BookmarkImportPath, BookmarkSearch,   // text
    BookmarkFilter,                        // text = the group's name, "" every group
    BookmarkFavourites, BookmarkOnSpectrum, BookmarkStack,             // flag
    BookmarkRenameDraft,                   // text
    BookmarkRenameOpen,                    // flag = open, text = the draft it starts with (the group's name)
    BookmarkRenamed,                       // text = the new name: the filter follows it, the field closes
    BookmarkMenu,                          // number = the row id whose menu is open, -1 none
    OpenImportDialog,                      // flag = for a preset (text = its name)
    ImportLookup,                          // text = the cleaned path of a bookmark import to look for on a worker
    PresetImportLookup,                    // text = the cleaned path, text2 = the preset
    ScannerDraft,                          // number = the field, text = what was typed
    AirbandCode, AirbandPresetName, AirbandFreq, AirbandName, AirbandImportPath,   // text
    AirbandMode,                           // number
    AirbandGroup,                          // text = the group on show; flag = the preset field takes text2
    AirbandBusiest,                        // flag
    OpenFolder,                            // text = fact.recorder.dir
    TickRun,                               // ids = the rows to send, flag = tick or untick
    SquelchHeld, HoldHeld,                 // value
    MarkersOpen, MarkersClosed,
    MarkerNoteHold,                        // number, text
    Clipboard,                             // text = what the clipboard takes
};
// The answer an op waits for (AirbandAsk::Kind): the publisher keeps the ticket and settles the note and the group on show when it comes.
enum class AskKind { None, TypedRow, AirportRows, ImportRows, RemoveGroup, Listen, ExportPreset };
struct Ask {
    AskKind kind = AskKind::None;
    std::string group;                     // the preset / the airport's group the ask is about
    std::size_t total = 0, ticked = 0;     // an airport's rows, as sent, and the ones ticked
};
struct Effect {
    std::vector<Command> commands;         // ops to submit on the window's session, in order
    Local local = Local::None;
    std::string text, text2;
    bool flag = false;
    int number = 0;
    double value = 0.0;
    std::vector<std::uint64_t> ids;
    Ask ask;
    bool coverage_reset = false;           // the position was set: the coverage starts again (submitReceiverPosition)
    // the Airband section's lists and notes
    bool clear_choices = false, clear_nearest = false, clear_folder_key = false, clear_preset_note = false;
    bool set_choices = false, set_nearest = false, set_note = false;
    std::vector<const cascade::core::Airport*> choices;
    std::vector<NearestAirport> nearest;
    std::string note;
    bool empty() const {
        return commands.empty() && local == Local::None && !coverage_reset && !clear_choices && !clear_nearest && !clear_folder_key && !clear_preset_note &&
               !set_choices && !set_nearest && !set_note;
    }
};

Effect applyScopeClicked();
Effect applyRadarScopeClicked();
Effect applyPositionHeard(const ReadOptions& options);
Effect applyPositionMap(const ReadOptions& options);
Effect applyPositionLat(const std::string& text);
Effect applyPositionLon(const std::string& text);
Effect applyPositionSet(const std::string& latText, const std::string& lonText);

Effect applyBookmarkName(const std::string& text);
// `tunedHz`: the receiver's tune, which NAMES the bookmark when the name box is empty ("100.3000 MHz", the list's own figure) - the engine
// takes no bookmark with an empty name and the classic window's key did nothing visible (docs 6.46 finding 1, closed in 6.52); 0 = unknown, the
// empty name goes as typed.
Effect applyBookmarkAdd(const std::string& name, const std::string& group, double tunedHz = 0.0);
Effect applyBookmarkGroup(const std::string& text);
Effect applyBookmarkImportPath(const std::string& text);
Effect applyBookmarkImport(const BookmarksModel& m);
Effect applyBookmarkSearch(const std::string& text);
Effect applyBookmarkFilter(const BookmarksModel& m, int index);
Effect applyBookmarkFavourites(bool on);
Effect applyBookmarkOnSpectrum(bool on);
Effect applyBookmarkStack(const BookmarksModel& m, bool on);
Effect applyBookmarkExport(const BookmarksModel& m);
Effect applyBookmarkRemoveGroup(const BookmarksModel& m);
Effect applyBookmarkRenameDraft(const std::string& text);
Effect applyBookmarkRenameOpen(const BookmarksModel& m);
Effect applyBookmarkRename(const BookmarksModel& m);
Effect applyBookmarkTick(const BookmarksModel& m, int id, bool on);
Effect applyBookmarkFavourite(const BookmarksModel& m, int id, bool on);
Effect applyBookmarkTune(const BookmarksModel& m, int id);
Effect applyBookmarkDelete(const BookmarksModel& m, int id);
Effect applyBookmarkMenu(int id);
Effect applyBookmarkMove(const BookmarksModel& m, int id, const std::string& group);

Effect applyScannerTicked(bool on);
Effect applyScannerField(const ScannerModel& m, const FoxReceiverState& state, int field, const std::string& text);
Effect applyScannerRun(const ScannerModel& m, const FoxReceiverState& state);
Effect applyScannerHalt(const ScannerModel& m);
Effect applyScannerSkip(const ScannerModel& m);

Effect applyAirbandCode(const std::string& text);
Effect applyAirbandLookup(const std::string& code);
Effect applyAirbandNearest(const FoxReceiverState& state);
Effect applyAirbandHide();
Effect applyAirbandAirport(const ReadOptions& options, int index);
Effect applyAirbandPreset(const AirbandModel& m, int index);
Effect applyAirbandPresetName(const std::string& text);
Effect applyAirbandFreq(const std::string& text);
Effect applyAirbandName(const std::string& text);
Effect applyAirbandMode(int index);
Effect applyAirbandAdd(const AirbandModel& m);
Effect applyAirbandImportPath(const std::string& text);
Effect applyAirbandImport(const AirbandModel& m);
// A path typed in the import DIALOG that was opened for a preset (docs 6.61): the same lookup the Airband Import key starts for a typed path, for the preset the dialog names.
// Nothing for an empty path or no preset.
Effect applyAirbandImportInto(const std::string& path, const std::string& preset);
Effect applyAirbandExport(const AirbandModel& m);
Effect applyAirbandRemovePreset(const AirbandModel& m);
Effect applyAirbandFolder(TableReader& table);
Effect applyAirbandGroup(const AirbandModel& m, int index);
Effect applyAirbandTickAll(const AirbandModel& m, bool on, bool coverAll);
Effect applyAirbandBusiest(bool on);
Effect applyAirbandTick(int id, bool on);
Effect applyAirbandTune(int id);
Effect applyAirbandSquelch(float norm);
Effect applyAirbandAboveNoise(const AirbandModel& m);
Effect applyAirbandHold(const std::string& text);
Effect applyAirbandListen(const AirbandModel& m);
Effect applyAirbandNext(const AirbandModel& m);

Effect applyMarkersOpen();
Effect applyMarkerNote(int number, const std::string& text);
Effect applyMarkerRemove(int number);
Effect applyMarkersClear(const MarkersModel& m);
Effect applyMarkerTune(const MarkersModel& m, int number);
Effect applyMarkerBookmark(const MarkersModel& m, int number, const FoxReceiverState& state);
Effect applyMarkersCopy(const MarkersModel& m);
Effect applyMarkersClosed();

}  // namespace foxsdr::slinttools
