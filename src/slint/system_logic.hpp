// system_logic.hpp - THE SYSTEM BANK'S PURE LOGIC (docs/engine-stage4.md, the Slint port, phase 3 bank 2): what the open bodies under the SYSTEM rows
// show - Display, Language & country, Updates, Serial ports (with the GPS row), Key bindings, Diagnostics, Usage reporting, Beta tester and the two
// report pages - is computed from the INTERFACE HALF of the settings (core::AppConfig, held by HostDuties), the C API table's lists and facts, and the
// state the engine published, and every control the operator touches becomes the exact EFFECT the ImGui window's own control has: a setting changed,
// SAVED and applied, or an op, or a local state change, or a report to send. No Slint type and no engine header appears here (foxsdr_api.h,
// core/app_commands.hpp, core/config.hpp and the light core headers named below), so the file is tested without a window or an engine
// (tests/test_slint_host_system.cpp reads a stub table and a hand-built AppConfig, as the SIGNAL bank's test does).
//
// THE CONTRACT is ui/system_bank.slint's (the UI family's; the note slint-system-bank-contract.md): the models below are the Slint structs with the
// same fields in snake_case; the publisher (system_publish.*, not this file) maps them 1:1 and wires each SystemEvents callback to the effect function
// the table below names. A field marked "host extra" is not in the contract - the publisher may use it or leave it.
//
// WHERE THE RULES COME FROM (the ImGui window, read for behaviour and for every sentence, never for drawing):
//   Display    AppWindow::drawDisplaySection (app_window.cpp 6992), drawTrailWidthControl (6862), pickTheme (6916), setManualRange (app_window_display.cpp 237)
//   Language   drawLanguageSection (app_window_language.cpp 149)         Updates   drawUpdatesSection (app_window_updates.cpp 539)
//   Serial     drawSerialPortsSection (app_window.cpp 20758) + drawGpsPositionControl (11048) + readGpsRow (21928)
//   Keys       drawKeyBindingsSection (20871) + pollKeyCapture (19051) + gui/key_bindings.*   Diagnostics drawDiagnosticsSection (21068) + drawDiagnosticsOffer
//   Usage      drawUsageReportingSection (20626)                         Tester    drawTesterUsageSection (app_window_tester.cpp 98)
//   Reports    drawFeatureRequestPage / drawProblemReportPage (app_window_report.cpp 71, 344)
// and the sentences are those drawers' own, through cascade::i18n::tr() (the catalogues already hold them; this file adds NO key - see the note on the
// key-bindings footer below).
//
// THE KEY TABLE IS A HOST COPY. gui/key_bindings.cpp includes imgui.h and the Slint program links no ImGui, so the action table (ids, names, groups), the
// stock chords and the chord names are copied here, as strings; tests/test_slint_host_system.cpp READS gui/key_bindings.cpp and fails when a row, a
// default or a key name differs. The stored form is the ImGui window's own ("actionId=Ctrl+Shift+PageUp", only what differs from the stock table).
//
// THE CALLBACKS -> EFFECTS (every apply... returns an Effect; "save" = the whole changed interface half goes to FOXAPP_OP_INTERFACE_CONFIG 0x842F through
// HostDuties::saveInterfaceHalf; "apply" = the window-side application the publisher does after the save; "[local]" = no setting and no command, the
// publisher keeps the state; a function that returns an empty Effect does nothing, exactly as the ImGui control would not):
//
//   display-scale(i)             applyDisplayScale     save interfaceScale = "auto" (i = 0) or the percent ("100".."250", i = 1..6); apply Scale (uiscale::setChoice)
//   display-theme(i)             applyDisplayTheme     save uiTheme = the key AND the preset's counter and readings (pickTheme: counterScale, counterSwitches, readingsScale,
//                                                      tunerDisplayStyle); apply Skin (value = the key)
//   display-db-min(v)            applyDisplayDbMin     save dbMin/dbMax (clampManualRange, the other end gives way) and waterfallRangeMode = "manual"; op FOXAPI_OP_SET_DISPLAY_RANGE 0x0201
//   display-db-max(v)            applyDisplayDbMax     num[0] = floor, num[1] = ceiling; apply DbRange. Dragging either end is choosing a range: Manual from then on
//   display-freq-display(i)      applyDisplayFreqDisplay  save tunerDisplayStyle = nixie | neon | plain
//   display-figures(i)           applyDisplayFigures   save counterScale = 1 (Normal size) | 2 (Enlarged figures)
//   display-tuner-switches(on)   applyDisplayTunerSwitches  save counterSwitches
//   display-enlarge(on)          applyDisplayEnlarge   save readingsScale = 1.4 | 1.0 (kEnlargedReadings)
//   display-icon-px(v)           applyDisplayIconPx    save aircraftIconPx (16..96) and mapTrailWidthPx pulled down to the new wingspan (drawTrailWidthControl)
//   display-trail-px(v)          applyDisplayTrailPx   save mapTrailWidthPx (1 .. the wingspan of the icon in force)
//   display-trail-style(i)       applyDisplayTrailStyle  save mapTrailStyle = 0 (Line) | 1 (Ribbon); the map applies it through slintmap::mapLookOf (docs 6.44)
//   display-band-plan(on)        applyDisplayBandPlan  save bandPlanOverlay
//   display-band-size(i)         applyDisplayBandSize  save bandPlanSize = small | medium | large
//   display-band-colour(i)       applyDisplayBandColour  save bandPlanPalette = classic | vivid | mono
//   display-region(i)            applyDisplayRegion    op FOXAPI_OP_SET_BAND_PLAN 0x0202 text = regions[i].id; nothing for the plan already in force
//   language-selected(i)         applyLanguageSelected save language = "auto" (i = 0) or languages()[i-1].code; apply Language (setLanguage + select_bundled_translation); a
//                                                      language this machine cannot draw is greyed and sends nothing
//   language-suggest()           applyLanguageSuggest  USE <LANGUAGE>: save language = the country's catalogue; apply Language
//   (translation report)         applyLanguageTranslationReport  [local] opens the bug page; the draft text starts "Translation (<code>): " unless the person has one
//   country-filter(text)         applyCountryFilter    [local] the filter text (ReadOptions::country_filter is it, given back each publish)
//   country-selected(i)          applyCountrySelected  save country = the code ("" for the Not set row); op FOXAPI_OP_SET_BAND_PLAN text = that country's plan when it is not the
//                                                      one in force ("Not set" leaves the plan alone)
//   updates-check(on)            applyUpdatesCheck     save updateCheckEnabled; apply UpdateCheck (HostDuties::updateSettingChanged: off discards the answer, on asks again)
//   updates-channel(i)           applyUpdatesChannel   save updateChannel = stable | beta; apply UpdateCheck when it changed and the check is on; nothing for a Store copy
//   updates-team-message()       [local]               the publisher brings the team message cards forward (the cards are the duties', not this bank's)
//   (show it again)              applyUpdatesShowAgain apply UpdateShowAgain (HostDuties::updateShowAgain)
//   updates-open-site()          applyUpdatesOpenSite  [local] open foxsdr.com in the browser (Local::OpenUrl) - the withdrawn notice with no fixed build yet
//   updates-withdrawn-dismiss()  applyUpdatesWithdrawnNotNow  apply UpdateDismiss (HostDuties::updateDismissed): the classic's "Not now" on that notice
//   serial-refresh()             opSerialRefresh       FOXAPI_OP_SCAN_SERIAL_PORTS 0x0F03 (the answer is the SERIAL_PORTS list changing)
//   gps-port(i)                  applyGpsPort          save gpsPort = ports[i] (nothing to save for the port already chosen); ops FOXAPP_OP_CLEAR_STATUS 0x840C x2
//                                                      (FOXAPP_STATUS_GPS_REFUSAL 1, FOXAPP_STATUS_GPS_RESULT 6) - sent for any pick, as the Selectable fires; nothing while listening
//   gps-baud(i)                  applyGpsBaud          save gpsBaud = kSerialBaudRates[i]; the same two clears; nothing while listening
//   gps-toggle()                 applyGpsToggle        listening: FOXAPI_OP_GPS 0x0F02 ival[0] = 0 (Stop); idle with a port: FOXAPI_OP_GPS text = gpsPort, ival[0] = 1, num[0] = gpsBaud;
//                                                      idle with no port: nothing
//   keys-reset()                 applyKeysReset        save keyBindings = {} (the stock table); capture ends
//   keys-capture(id)             applyKeysCapture      [local] the row id starts listening; a second press on the listening row puts it back
//   keys-captured(id, chord)     applyKeysCaptured     save keyBindings with that action on that chord (a clash is shown, never refused); Escape (any modifiers) = cancel, Backspace
//                                                      = clear, a text that is no chord = cancel (pollKeyCapture answers those two first); capture ends
//   keys-cancel()                applyKeysCancel       [local] capture ends
//   keys-clear(id)               applyKeysClear        save keyBindings with that action unbound ("None")
//   diag-record(on)              applyDiagRecord       save diagnosticsEnabled; apply Diagnostics (HostDuties::diagnosticsChanged: setCrashCaptureEnabled(enabled, minidump))
//   diag-minidump(on)            applyDiagMinidump     save diagnosticsMinidump; apply Diagnostics
//   diag-what(on)                applyDiagWhat         [local] the field list is open (one flag shared with usage-what)
//   diag-open-log()              applyDiagOpenLog      [local] show the log file (Local::OpenLog, text = ReadOptions::log_path)
//   diag-open-reports()          applyDiagOpenReports  [local] the reports folder (openReportsFolder; text = ReadOptions::reports_path)
//   usage-send(on)               applyUsageSend        op FOXAPP_OP_TELEMETRY_CONSENT 0x841A ival[0] = on (grant ADMIN); the box shows the value asked for until the fact says it
//   usage-what(on)               applyUsageWhat        [local] the payload list is open
//   usage-privacy()              applyUsagePrivacy     [local] open core::kPrivacyPolicyUrl in the browser (Local::OpenUrl)
//   tester-unlink()              applyTesterUnlink     save testerAppToken = "", testerAppTokenName = "" (clearTesterAppToken)
//   tester-remove-code()         applyTesterRemoveCode save testerToken = "", testerTokenInvalid = false, and the app token and its name cleared with it (setTesterToken(""))
//   tester-code-committed(text)  applyTesterCodeCommitted  [local] the masked draft (ReadOptions::tester_draft); the error line is forgotten
//   tester-use()                 applyTesterUse        a 40-hex app token: Local::TesterResolve (text = the token; the confirm-by-name question is the link flow's); a 32-hex portal token
//                                                      or the whole link: save testerToken = it, testerTokenInvalid = false, the app token cleared; else Local::TesterError with the sentence
//   tester-show-sent(on)         applyTesterShowSent   [local] the masked preview is open
//   report-open(kind)            applyReportOpen       [local] the page opens (0 feature, 1 bug); never restored at start-up
//   report-text(text)            applyReportText       [local] the draft
//   (report contact)             applyReportContact    [local] the draft
//   (report kind)                applyReportKind       [local] bug | dislike; the attach box follows the kind's own default (ticked for a bug)
//   report-attach(on)            applyReportAttach     [local] (bug page)
//   report-preview(on)           applyReportPreview    [local] "Show what will be sent"
//   report-send()                applyReportSend       a ReportSend: the payload and which core sender takes it (FeatureRequestSender / ProblemReportSendFlow, on HostDuties' LiveWorld:
//                                                      featureSender() / problemSender(), send(endpoint, payload, nowEpoch)); nothing unless the verdict is "can send"
//   report-closed()              applyReportClosed     [local] the page closes
//   about-open()                 applyAboutOpen        [local] the About sheet
//
// WHAT IS NOT COVERED (the contract has no callback or model field for them; the ImGui section has them): the Diagnostics section's "Copy diagnostics", the pointer
// ledger tick and the SDRplay diagnostic with its confirmation sheet (window-bound state: the clipboard, the input ledger, a child process); the Updates section's
// Store-copy flow (the host's duties defer it); the beta tester's usage RECORDER and the "Link this FoxSDR?" prompt (the duties leave the usage report and the link poll out).
//
// THE KEY-BINDINGS FOOTER. The contract's "N actions, M clashes" is no sentence of the ImGui window and no catalogue holds it, so this file does not word it: the footer is
// the ImGui hint (F12 always saves a screenshot... / Esc always cancels...) and the two counts ride as numbers (KeyBindingsModel::action_count, clash_count).
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

#include "core/app_commands.hpp"
#include "core/config.hpp"
#include "core/display_range.hpp"
#include "core/feature_request.hpp"
#include "core/problem_report.hpp"
#include "core/serial_port.hpp"
#include "core/tune_control.hpp"
#include "gui/ui_scale.hpp"
#include "kit_logic.hpp"
#include "signal_logic.hpp"

namespace foxsdr::slintsystem {

using foxsdr::slintkit::TableReader;
using foxsdr::slintsignal::Command;
using foxsdr::slintsignal::SelectItem;
using foxsdr::slintsignal::Tone;

// ---- the constants the ImGui window states in its drawers (the ones that live in a header with no ImGui in it are quoted, not copied) ----
constexpr float kEnlargedReadings = 1.4f;                    // app_window.cpp kEnlargedReadings (a test reads the source)
constexpr int kScaleStepCount = cascade::gui::uiscale::kStepCount;   // gui/ui_scale.hpp kSteps: 100 125 150 175 200 250
using cascade::core::DbPair;                                 // core/display_range.hpp: floorDb, ceilingDb
constexpr float kFloorMinDb = cascade::core::kFloorMinDb, kFloorMaxDb = cascade::core::kFloorMaxDb;
constexpr float kCeilingMinDb = cascade::core::kCeilingMinDb, kCeilingMaxDb = cascade::core::kCeilingMaxDb;
constexpr int kAircraftIconMinPx = 16, kAircraftIconMaxPx = 96, kAircraftIconDefaultPx = 48;   // gui/aircraft_icons.hpp includes imgui.h: copied, a test reads the source
constexpr int kTrailWidthMinPx = 1, kTrailWidthDefaultPx = 4;
constexpr int kSerialBaudCount = static_cast<int>(sizeof(cascade::core::kSerialBaudRates) / sizeof(cascade::core::kSerialBaudRates[0]));
constexpr int kSystemRowCount = 9;   // the rail's rows: Display, Language, Frequency correction, Updates, Serial ports, Key bindings, Diagnostics, Usage, Beta tester
enum Row : int { kRowDisplay = 0, kRowLanguage, kRowFrequency, kRowUpdates, kRowSerial, kRowKeys, kRowDiagnostics, kRowUsage, kRowTester };

// ---- the models (ui/system_bank.slint's structs, snake_case) ----
struct DisplayModel {
    std::vector<SelectItem> scales;        // "Auto (follows Windows, 100% here)", "100%" ... "250%"
    int scale_index = 0;
    std::vector<SelectItem> themes;        // the six looks, id = the stored key
    int theme_index = 0;
    float db_min = -110.f;
    std::string db_min_text;               // "%.0f" (the slider's format)
    float db_max = 0.f;
    std::string db_max_text;
    std::vector<SelectItem> freq_displays; // Nixie tubes, Neon, Plain (id = nixie | neon | plain)
    int freq_display_index = 0;
    std::vector<SelectItem> figures;       // Normal size, Enlarged figures
    int figures_index = 0;
    std::vector<SelectItem> digit_groupings;   // slint-gaps.md item 2: 1.090.000.000 | 1090.000.000 (the numerals are the labels); saved in the workspace entry of the window half (`dg=`), not here
    int digit_grouping_index = 0;
    bool high_contrast = false;            // slint-gaps.md item 11 (the host's own record, like the grouping)
    bool tuner_switches = true;
    bool enlarge_readings = false;
    float icon_px = 48.f;
    std::string icon_text;                 // "48 px"
    float trail_px = 4.f;
    std::string trail_text;
    std::vector<SelectItem> trail_styles;  // Line, Ribbon (id = line | ribbon): mapTrailStyle 0 | 1 (docs 6.52, tick row 364)
    int trail_style_index = 0;
    bool band_plan = true;
    std::vector<SelectItem> band_sizes;    // Small, Medium, Large
    int band_size_index = 0;
    std::vector<SelectItem> band_colours;  // Classic, Vivid, Mono
    int band_colour_index = 0;
    std::vector<SelectItem> regions;       // the installed plans (id = what SET_BAND_PLAN takes)
    int region_index = -1;                 // the plan in force, -1: none ("(all installed)")
    std::string plan_status;               // the error, "no band plan installed" or "<name> (N bands)"; "" while the overlay is off
    Tone plan_status_tone = Tone::Neutral; // Err for the error, Neutral otherwise
    // ---- host extras ----
    bool band_options_shown = true;        // the Size, Colour, Region rows and the status: only while the overlay is ticked
    bool regions_shown = false;            // more than one plan is installed
    std::string region_preview;            // the Region box's closed text: the plan's name, or "(all installed)"
    float icon_min = 16.f, icon_max = 96.f;
    float trail_min = 1.f, trail_max = 48.f;   // the wingspan of the icon in force
    float db_min_lo = -160.f, db_min_hi = -20.f, db_max_lo = -100.f, db_max_hi = 20.f;
    bool range_manual = false;             // waterfallRangeMode == "manual"
    std::string scale_tooltip, switches_tooltip, icon_tooltip, trail_tooltip;
};

struct LanguageModel {
    std::vector<SelectItem> languages;     // "Automatic (X)" first, then English and each catalogue by its own name; one that cannot be drawn is disabled
    int language_index = 0;
    std::string machine_hint;              // "This language was translated by machine and may read oddly." ("" when the language in force is a person's)
    std::string suggest_label;             // "USE DEUTSCH" ("" when the country's language is in force or not carried)
    std::vector<SelectItem> countries;     // "Not set" (id "") first while the filter is empty, then every country by its shown name in the order of the language in force
    int country_index = -1;                // the chosen country's row in the list as filtered, -1 when it is filtered out
    std::string country_filter;
    std::string country_hint;              // "Type to narrow the list"
    // ---- host extras ----
    std::string country_text;              // the Country box's closed text: the name, or "Not set"
    std::string plan_line;                 // "Band plan: <name>" once a country is chosen
    std::string plan_changed_hint;         // "Display has since been set to a different band plan." (ReadOptions::plan_changed)
    std::string translation_label;         // "SUGGEST A BETTER TRANSLATION" (shown with machine_hint)
    std::string suggest_code;              // the catalogue USE <LANGUAGE> switches to
    std::string language_text;             // the Language box's closed text
};

struct UpdatesModel {
    bool check_at_startup = true;
    int channel_index = 0;                 // 0 stable, 1 beta
    std::string channel_help;              // "Stable is the release the FoxSDR team recommends..." ("" for a Store copy)
    std::string privacy;                   // what is asked, and what is sent
    std::string version_line;              // "this build: 0.99.105" (a Store copy adds a line "package: ...")
    std::string team_message;              // "2 message(s) from the FoxSDR team" ("" when none or not shown)
    std::string status_line;
    Tone status_tone = Tone::Neutral;      // Warn: an update; Err: an important one
    // ---- host extras ----
    std::string check_label;               // "Check for updates at startup" / "Ask the Microsoft Store for updates at startup"
    bool packaged = false;                 // a Microsoft Store copy: no channel radios
    bool show_again = false;               // "Show it again" is offered (an update is offered and the banner was dismissed)
    std::string show_again_label;
    std::string stable_label, beta_label;  // "Stable releases", "Every release (beta)"
    // ---- the kill switch (docs 6.63): the version RUNNING was withdrawn by the service. The status line above says so (warning tone); these are the rest ----
    std::string withdrawn_advice;          // "We recommend updating to FoxSDR 0.99.109." / "A fixed version is not ready yet. foxsdr.com will have it as soon as it is." ("" when not withdrawn)
    std::string site_label;                // "Open foxsdr.com" - only while no good build is offered yet ("" otherwise)
    std::string dismiss_label;             // "Not now" - same condition, and only while the notice has not been put away
};

struct GpsRow {
    std::vector<SelectItem> ports;         // the machine's serial ports, with the saved port added when the machine does not list it
    int port_index = -1;
    std::vector<SelectItem> bauds;         // 4800 ... 115200
    int baud_index = 1;
    bool listening = false;
    std::string key_label;                 // "Read position from GPS" / "Stop"
    std::string status;                    // the engine's sentence (core::gpsStatusLine), "" idle
    Tone status_tone = Tone::Neutral;      // Warn when the read failed or timed out
    std::string satellites;                // the count in view as digits, "" before the first
    // ---- host extras ----
    bool key_enabled = true;               // a port is named (or a read is running: Stop)
    std::string key_tooltip, port_tooltip, baud_tooltip;
    std::string refusal;                   // fact.status.gpsRefusal ("" none), the warning line under the status
    int state = 0;                         // 0 idle, 1 listening, 2 fixed, 3 failed, 4 timed out
};

struct SerialPortsModel {
    std::string intro;
    std::string empty_line;                // "No serial ports found. If a device is plugged in, check Device Manager..." ("" until the scan has answered, or ports exist)
    std::vector<std::string> ports;
    GpsRow gps;
    std::string gps_note;
    // ---- host extras ----
    std::string refresh_label, column_label, gps_heading;   // "Refresh", "Port", "GPS receiver"
    std::string gps_hint;                                    // "or read it from a GPS receiver on a serial port:"
};

struct KeyRow {
    std::string id;                        // the config id ("startStop")
    std::string action;                    // the action's words, translated
    std::string chord;                     // "Ctrl+Shift+PageUp", "None" when unbound
    bool capturing = false;
    std::string clash;                     // "also Mute the sound" ("" none)
};
struct KeyGroup {
    std::string heading;                   // translated
    std::vector<KeyRow> rows;
};
struct KeyBindingsModel {
    std::string help;
    std::string reset_label;               // "Reset to defaults"
    std::vector<KeyGroup> groups;
    std::string footer;                    // the ImGui hint (see the header note)
    bool changed = false;                  // any binding differs from the stock table
    // ---- host extras ----
    std::string prompt;                    // "press a key": what a listening cap says
    int action_count = 0, clash_count = 0, changed_count = 0;   // changed_count: the lines the config would carry ("%zu SET")
};

struct DiagnosticsModel {
    std::string explanation;
    bool record = false;
    bool what_open = false;
    std::string what_text;                 // the field list of "What exactly is in a report?"
    bool minidump = false;
    std::string log_path;                  // "Log: <path>" / "Log: off (nothing is being written)."
    std::string reports_path;              // "Reports: <path>" / "Reports: (unavailable)"
    std::string offer;                     // the "did not close normally" sentence while the offer is due, else ""
    // ---- host extras ----
    std::string record_label, minidump_label, what_label;
    std::string sent_text;                 // "Sent: ... NEVER sent: ... Off means off: ..."
    std::string dump_note;                 // "A memory dump can contain file names..."
};

struct UsageModel {
    std::string disclosure;
    bool send = false;
    std::string install_id_line;           // "Install id: <id>" while on, "" off
    std::string what_label;                // "What exactly is sent?"
    std::string privacy_label;             // "Privacy policy (foxsdr.com)"
    std::vector<std::string> payload;      // the four paragraphs of what is sent
    bool what_open = false;
    std::string what_text;                 // the same paragraphs joined by a newline
    // ---- host extras ----
    std::string state_line;                // the two-messages sentence while on, "Off. Nothing is transmitted." while off
    std::string send_label;                // "Send anonymous usage reports"
    std::string privacy_url;               // core::kPrivacyPolicyUrl (the button's tooltip)
};

struct TesterModel {
    bool shown = false;
    std::string explanation;
    std::string linked_line;               // "Linked to <name>" (an app token is set)
    std::string code_line;                 // "Code: abcd....wxyz" (a legacy portal token is set; the code masked)
    std::string invalid_line;              // the "no longer valid" sentence while testerTokenInvalid
    std::string code_draft;                // the masked draft
    std::string use_label;                 // "Use this code"
    bool sent_open = false;
    std::string sent_preview;              // the masked JSON, "Nothing - no code is set.", and the queue line
    // ---- host extras ----
    bool field_shown = false;              // the code box and its key: no app token, and (no token or an invalid one)
    std::string error_line;                // "that does not look like a tester code or link" (ReadOptions::tester_error)
    std::string sent_text;                 // "What is sent: the app version, ..."
    std::string field_label, unlink_label, remove_label, show_label, hide_label;
};

struct ReportPageModel {
    bool shown = false;
    int kind = 0;                          // 0 feature, 1 bug
    std::string title;                     // "FEATURE REQUEST" / "REPORT A BUG / DISLIKE"
    std::string text;                      // the draft
    bool attach = false;
    std::string attach_note;               // what is sent: the whole sentence, by contact and by log (four variants on the bug page)
    std::vector<std::string> evidence;     // the attachment's lines a reader leans on: last-run-unclean, recent-faults, recovered
    bool preview_open = false;
    std::string preview_text;              // the exact text that goes
    bool send_enabled = false;
    std::string refusal;                   // "(write at least 10 characters - 0 so far)" ("" when the key is live)
    std::string no_email_note;
    std::string sent_line;                 // Sending... / Thank you... / the failure / the cool-down, and the dropped-log note
    // ---- host extras ----
    std::string prompt;                    // "What would you like FoxSDR to do?" / the bug or dislike question
    std::string counter;                   // "12 / 100000 characters"
    std::string contact;                   // the draft
    std::string contact_label;             // "Email or callsign (optional)"
    std::string kind_question;             // "What are you reporting?" (bug page)
    std::vector<SelectItem> problem_kinds; // Something is broken (bug), Something I dislike (id bug | dislike)
    int problem_kind_index = -1;           // neither chosen when the page opens
    std::string recognised_line;           // "A report was recognised and attached as <class>"
    std::string probe_label;               // "Attach the SDRplay diagnostic too" (while a probe ran and the log is attached)
    bool probe_shown = false, probe_on = false;
    std::string attach_label, preview_label, send_label;   // "Attach the diagnostics log", "Show what will be sent", "SEND"
    std::string local_only_line;           // the newest report's reason and where the file is (while the preview is open)
    Tone sent_tone = Tone::Neutral;        // Good: sent, Warn: sending or cooling, Err: failed
    bool sending = false;
};

struct RowChip {
    std::string word;
    Tone tone = Tone::Neutral;
    bool lit = false;
};

struct SystemModels {
    DisplayModel display;
    LanguageModel language;
    UpdatesModel updates;
    SerialPortsModel serial;
    KeyBindingsModel keys;
    DiagnosticsModel diagnostics;
    UsageModel usage;
    TesterModel tester;
    ReportPageModel feature, bug;
    std::array<RowChip, kSystemRowCount> chips;   // the rail's rows (the Frequency correction row's chip is the SIGNAL family's: left empty here)
};

// ---- what the publisher owns and hands in each time ----
enum class SendState { Idle, Sending, Sent, Failed, CoolingDown };
struct SenderView {
    SendState state = SendState::Idle;
    std::string failure;                   // the sender's failureMessage()
    std::uint64_t blocked_until = 0;       // blockedUntil(): the epoch second a cool-down ends
    bool diagnostics_dropped = false;      // the older site rejected the log field (problem reports)
};
struct ReportInputs {
    bool shown = false;
    std::string text, contact;
    std::string problem_kind;              // "" none, "bug", "dislike" (the bug page)
    bool attach = false, attach_probe = false, preview_open = false;
    std::string diagnostics;               // the prepared bundle (core::prepareDiagnosticsForReport(currentDiagnosticsBundle())), "" until built
    std::string probe_report;              // the SDRplay probe file's text, "" when none ran this session
    std::string local_only_why;            // the newest report's localOnlyReason(), "" none
    std::string reports_dir;               // core::diagCrashDir(), "" unavailable
    SenderView sender;
    std::uint64_t now_epoch = 0;
    std::string version;                   // cascade::versionString(): the payload's version (core/version.hpp is the engine's, not linked into the logic test)
};
struct UpdatesFacts {                       // HostDuties::updateStatus() (+ the Store flow's, which the duties defer)
    bool packaged = false, started = false, pending = false;
    bool newer = false, critical = false, dismissed = false;
    bool running_withdrawn = false;         // UpdateInfo::runningWithdrawn: the service withdrew the version running (the classic's update_.runningWithdrawn)
    bool offer_withdrawn = false;           // UpdateInfo::offerWithdrawn: the version offered was withdrawn - it is NOT offered, whatever `newer` says
    bool store_offered = false, store_mandatory = false, store_installing = false;
    std::string new_version, error, build_version, package_name;
    int unseen_messages = 0;
};
struct ReadOptions {
    int monitor_percent = 100;             // uiscale::dpiToPercent(monitor dpi): "Auto (follows Windows, N% here)"
    std::string system_locale;             // cascade::i18n::systemLocale(), read once ("" = none: Automatic lands on English)
    std::string country_filter;
    std::string plan_changed_note;         // "Display has since been set to a different band plan." while the publisher's bandPlanNoteDue says so ("" none)
    std::vector<std::pair<std::string, std::string>> language_reason;   // code -> the English reason it cannot be drawn (fonts::unavailableReason), shown beside the name
    std::string capturing_id;              // the key row that listens, "" none
    bool what_open = false;                // ONE flag for Diagnostics' and Usage reporting's field lists
    std::string log_path, reports_path;    // core::DiagLog filePath(), core::diagCrashDir(); "" = off / unavailable
    bool last_run_unclean = false;         // the offer is due (the last run did not end normally AND diagnostics were on)
    bool telemetry_pending = false, telemetry_pending_on = false;   // what the box has just asked for, until the fact says it
    UpdatesFacts updates;
    bool gps_ports_waiting = false;        // a scan was asked for and has not answered: the empty line is not said yet
    std::string gps_draft_port;            // "" = the saved port
    std::string tester_draft, tester_error, tester_preview_json;   // the masked draft; the error line; the masked JSON of the session's record ("" none built)
    int tester_queued = 0;                 // reports queued to retry
    bool tester_revealed = false;          // the section is shown for a link/migration in progress
    bool tester_sent_open = false;
    ReportInputs feature, bug;
};

// ---- the read: lists and facts through the seam, the interface half as the duties hold it, the state as the engine published it ----
DisplayModel readDisplay(TableReader& table, const cascade::core::AppConfig& iface, const FoxReceiverState& state, const ReadOptions& options);
LanguageModel readLanguage(TableReader& table, const cascade::core::AppConfig& iface, const ReadOptions& options);
UpdatesModel readUpdates(const cascade::core::AppConfig& iface, const ReadOptions& options);
SerialPortsModel readSerialPorts(TableReader& table, const cascade::core::AppConfig& iface, const ReadOptions& options);
KeyBindingsModel readKeyBindings(const cascade::core::AppConfig& iface, const ReadOptions& options);
DiagnosticsModel readDiagnostics(const cascade::core::AppConfig& iface, const ReadOptions& options);
UsageModel readUsage(TableReader& table, const ReadOptions& options);
TesterModel readTester(const cascade::core::AppConfig& iface, const ReadOptions& options);
ReportPageModel readFeatureRequest(const ReportInputs& in);
ReportPageModel readProblemReport(const ReportInputs& in);
SystemModels readSystem(TableReader& table, const cascade::core::AppConfig& iface, const FoxReceiverState& state, const ReadOptions& options);

// ---- the small pure rules ----
// Interface size: index 0 is Auto, 1..6 the steps; the stored text is "auto" or the percent. A hand-edited value degrades to Auto (uiscale::normalizeChoice).
int scaleIndexOf(const std::string& stored);
std::string scaleChoiceOf(int index);                       // "" for an index outside the list
std::string scaleLabel(int index, int monitorPercent);      // "Auto (follows Windows, 150% here)", "125%"
// The six themes (gui/theme.cpp): key, label, and what picking one brings with it.
struct ThemePreset { const char* key; const char* label; const char* face; float counterScale; bool switches; float readings; };
const ThemePreset& themePreset(int index);
int themeIndexOf(const std::string& key);                   // an unknown or empty key is Today's bench (0)
constexpr int kThemeCount = 6;
// The dB window: core::clampManualRange (core/display_range.hpp) is what a drag leaves - the end the hand moved is kept, the other gives way.
std::string dbText(float db);                               // "%.0f"
int wingspanPx(int iconPx);                                 // gui::aircraftWingspanPx
int clampIconPx(int px);
std::string pxText(int px);                                 // "48 px"
// The tuner style names in the menu's order and their labels.
const char* tunerStyleName(int index);                      // nixie, neon, plain
int tunerStyleIndexOf(const std::string& stored);           // unknown: Nixie
const char* bandPlanSizeKey(int index);                     // small, medium, large
int bandPlanSizeIndexOf(const std::string& stored);
const char* bandPlanPaletteKey(int index);                  // classic, vivid, mono
int bandPlanPaletteIndexOf(const std::string& stored);
std::string displayPlanName(const std::string& name);       // core::displayPlanName: each " + " part through tr()
// The band plan status line (app_window.cpp drawDisplaySection): the error, "no band plan installed", "<name> (N bands)".
struct PlanStatus { std::string text; Tone tone = Tone::Neutral; };
PlanStatus planStatus(const std::string& error, int bands, const std::string& planName);
// The Language box: the name of one language as the list letters it (its own name + the machine note; a language that cannot be drawn is named in English with the reason).
std::string languageLabel(const std::string& code, bool drawable, const std::string& reason);
std::string autoLanguageLabel(const std::string& systemLocale);   // "Automatic (Deutsch)"; English is the one name said in the language in force
std::string useLanguageLabel(const std::string& languageName);    // "USE DEUTSCH"
// The Country list: every country by its shown name in the order of the language in force, the filter applied (shown name, English name, or the code).
struct CountryRow { std::string code, shown; };
std::vector<CountryRow> countryRows(const std::string& filter);
bool countryMatches(const std::string& filter, const std::string& shown, const std::string& english, const std::string& code);
// Updates: the chip and the status line, in the order of drawUpdatesSection.
RowChip updatesChip(bool enabled, const UpdatesFacts& f);
struct UpdatesStatus { std::string text; Tone tone = Tone::Neutral; };
UpdatesStatus updatesStatus(bool enabled, const UpdatesFacts& f);
// Serial ports.
std::string serialPortsChip(std::size_t count);             // NONE, 1 PORT, N PORTS
std::string cleanPortName(const std::string& typed);        // core::sanitiseSerialPortName
std::string gpsKeyLabel(bool listening);
// Key bindings (the host copy of gui/key_bindings.*).
// The first kClassicKeyActionCount rows are the ImGui window's table (gui/key_bindings.*, held equal by test_slint_host_system); the rows after them are THIS window's own (screen stage S5, docs/multi-radio-records/screen-s5.md):
// selecting a radio (Ctrl+1 to Ctrl+8) and turning the page of radios (Alt+PageUp / Alt+PageDown). The ImGui window neither lists nor answers them and keeps what it finds in a saved file.
constexpr int kClassicKeyActionCount = 32;
constexpr int kSelectRadioActions = 8;
constexpr int kKeyActionCount = kClassicKeyActionCount + kSelectRadioActions + 2;
// `number` is the radio of a "Select Radio %d" row (1-based), 0 for every other row: the row's name is a sentence with the number in it (formatText over the translated name).
struct KeyActionInfo { const char* id; const char* name; const char* group; const char* stock; int number = 0; };
std::string keyActionName(int index);                       // the row's name in the interface language ("Select Radio 3" for a numbered row)
const KeyActionInfo& keyAction(int index);                  // by the enum's order
int keyActionIndexOf(const std::string& id);                // -1 when the id table does not carry it
struct Chord {
    std::string key;                                        // the canonical name ("PageUp", "F2", "Plus", "A"), "" = unbound
    bool ctrl = false, shift = false, alt = false;
    bool bound() const { return !key.empty(); }
    bool operator==(const Chord&) const = default;
};
std::string formatChord(const Chord& c);                    // "Ctrl+Shift+PageUp" - modifiers in that order; "None" when unbound
bool parseChord(const std::string& text, Chord& out);       // tolerant of case and spaces; "None" and "" parse to unbound; "Ctrl+" or two keys fail
bool chordKeyKnown(const std::string& name, std::string& canonical);   // the name table plus its aliases (Esc, Del, Return, PgUp ...)
std::vector<std::string> chordKeyNames();                   // the canonical names in the table's order
using KeyTable = std::array<Chord, kKeyActionCount>;
KeyTable defaultKeyTable();
KeyTable keyTableFromConfig(const std::vector<std::string>& lines);   // keyBindingsFromConfig: bad lines dropped one by one, the first line for an action wins
std::vector<std::string> keyTableToConfig(const KeyTable& t);         // only what differs from the stock table, "id=chord"
// Two actions on one chord: the first other action (in enum order) that holds the same bound chord, -1 none.
int clashOf(const KeyTable& t, int index);
int clashCount(const KeyTable& t);                          // the rows that clash
// Diagnostics / usage / tester / report sentences.
std::string diagnosticsChip(bool enabled, bool minidump);   // OFF, ON, ON+DMP
std::string keyBindingsChip(std::size_t changed);           // STOCK, "N SET"
std::string testerChip(bool active, bool invalid);          // OFF, ON, INVALID
enum class TesterKind { None, AppToken, PortalToken };
TesterKind classifyTesterCode(const std::string& pasted, std::string& token);
std::string testerNameOrPlaceholder(const std::string& name);   // "(no name set)"
struct SendVerdict { bool can_send = false; std::string reason; };
SendVerdict featureVerdict(const std::string& text, const std::string& contact, const SenderView& sender, std::uint64_t nowEpoch);
SendVerdict problemVerdict(const std::string& kind, const std::string& text, const std::string& contact, const SenderView& sender, std::uint64_t nowEpoch);
std::string featureSentence(bool haveContact);
std::string problemSentence(bool attach, bool haveContact);
std::string senderStatusLine(const SenderView& s, bool feature, Tone& tone);
std::vector<std::string> evidenceLines(const std::string& diagnostics);

// ---- the effects ----
// The window-side application of a saved setting, a bit mask in Effect::apply.
enum Apply : unsigned {
    kApplyNone = 0,
    kApplyScale = 1u << 0,        // uiscale::setChoice(Effect::value)
    kApplySkin = 1u << 1,         // the theme key in Effect::value -> Theme.skin
    kApplyDbRange = 1u << 2,      // the display range is the command's; the window follows the state
    kApplyLanguage = 1u << 3,     // cascade::i18n::setLanguage(Effect::value) + slint::select_bundled_translation
    kApplyDiagnostics = 1u << 4,  // HostDuties::diagnosticsChanged(enabled, minidump)
    kApplyUpdateCheck = 1u << 5,  // HostDuties::updateSettingChanged()
    kApplyUpdateShowAgain = 1u << 6,   // HostDuties::updateShowAgain()
    kApplyUpdateDismiss = 1u << 7,     // HostDuties::updateDismissed(): "Not now" on the withdrawn notice
};
// What is local (the publisher keeps the state) or leaves the program without a command.
enum class Local {
    None,
    CaptureStart, CaptureCancel,            // text = the key row id ("" for a cancel from the keyboard); CaptureCancel also rides every Effect that ends a capture
    CountryFilter,                          // text = the filter
    TesterDraft,                            // text = the (masked) draft; flag = it was CLEARED (and its error line forgotten): Use this code / Remove code
    ToggleWhat, ToggleTesterSent,           // flag = the new state
    OpenLog, OpenReports,                   // text = the path
    OpenUrl,                                // text = the address
    OpenReport, CloseReport,                // number = the page kind (0 feature, 1 bug); text = a draft the page starts with ("" none)
    ReportText, ReportContact,              // number = the page kind; text = the draft
    ReportKind,                             // text = bug | dislike; flag = the attach box's own default for that kind (ticked for a bug)
    ReportAttach, ReportPreview,            // number = the page kind; flag = the new state
    ReportProbe,                            // flag = the new state
    TesterResolve,                          // text = the app token to put to confirm-by-name; flag = the draft is cleared
    TesterError,                            // text = the sentence (the draft is kept)
    OpenAbout,
    OpenTeamMessages,
};
// A report to send: the core call and its payload. Exactly one of the two payloads is meaningful, by `kind`.
struct ReportSend {
    bool send = false;
    int kind = 0;                           // 0 feature, 1 bug
    std::string endpoint;                   // featureRequestEndpoint() / problemReportEndpoint()
    cascade::core::FeatureRequestPayload feature;
    cascade::core::ProblemReportPayload problem;
    std::size_t chars = 0;                  // the count the log line carries (never the words)
};
struct Effect {
    bool save = false;                      // the interface half to hand to HostDuties::saveInterfaceHalf
    cascade::core::AppConfig half;
    unsigned apply = kApplyNone;            // Apply bits
    std::string value;                      // Apply::Scale/Skin/Language: the setting or key
    std::vector<Command> commands;          // ops to submit on the window's session, in order
    Local local = Local::None;
    std::string text;
    bool flag = false;
    int number = 0;
    ReportSend report;
    bool empty() const { return !save && apply == kApplyNone && commands.empty() && local == Local::None && !report.send; }
};

Effect applyDisplayScale(const cascade::core::AppConfig& cur, int index);
Effect applyDisplayTheme(const cascade::core::AppConfig& cur, int index);
Effect applyDisplayDbMin(const cascade::core::AppConfig& cur, const FoxReceiverState& state, float value);
Effect applyDisplayDbMax(const cascade::core::AppConfig& cur, const FoxReceiverState& state, float value);
Effect applyDisplayFreqDisplay(const cascade::core::AppConfig& cur, int index);
Effect applyDisplayFigures(const cascade::core::AppConfig& cur, int index);
Effect applyDisplayTunerSwitches(const cascade::core::AppConfig& cur, bool on);
Effect applyDisplayEnlarge(const cascade::core::AppConfig& cur, bool on);
Effect applyDisplayIconPx(const cascade::core::AppConfig& cur, float px);
Effect applyDisplayTrailPx(const cascade::core::AppConfig& cur, float px);
Effect applyDisplayTrailStyle(const cascade::core::AppConfig& cur, int index);
Effect applyDisplayBandPlan(const cascade::core::AppConfig& cur, bool on);
Effect applyDisplayBandSize(const cascade::core::AppConfig& cur, int index);
Effect applyDisplayBandColour(const cascade::core::AppConfig& cur, int index);
Effect applyDisplayRegion(const DisplayModel& m, int index);

Effect applyLanguageSelected(const cascade::core::AppConfig& cur, int index);
Effect applyLanguageSuggest(const cascade::core::AppConfig& cur);
Effect applyLanguageTranslationReport(const std::string& currentDraft);
Effect applyCountryFilter(const std::string& text);
Effect applyCountrySelected(TableReader& table, const cascade::core::AppConfig& cur, const std::string& filter, int index);

Effect applyUpdatesCheck(const cascade::core::AppConfig& cur, bool on);
Effect applyUpdatesChannel(const cascade::core::AppConfig& cur, int index, bool packaged);
Effect applyUpdatesShowAgain();
Effect applyUpdatesTeamMessage();
Effect applyUpdatesOpenSite();         // [local] open foxsdr.com (the withdrawn notice, while no fixed build is out yet)
Effect applyUpdatesWithdrawnNotNow();  // apply UpdateDismiss

Command opSerialRefresh();
Effect applySerialRefresh();
Effect applyGpsPort(const cascade::core::AppConfig& cur, const GpsRow& row, int index);
Effect applyGpsBaud(const cascade::core::AppConfig& cur, const GpsRow& row, int index);
Effect applyGpsToggle(const cascade::core::AppConfig& cur, const GpsRow& row);

Effect applyKeysReset(const cascade::core::AppConfig& cur);
Effect applyKeysCapture(const std::string& capturingNow, const std::string& id);
Effect applyKeysCaptured(const cascade::core::AppConfig& cur, const std::string& id, const std::string& chordText);
Effect applyKeysCancel();
Effect applyKeysClear(const cascade::core::AppConfig& cur, const std::string& id);

Effect applyDiagRecord(const cascade::core::AppConfig& cur, bool on);
Effect applyDiagMinidump(const cascade::core::AppConfig& cur, bool on);
Effect applyDiagWhat(bool on);
Effect applyDiagOpenLog(const std::string& logPath);
Effect applyDiagOpenReports(const std::string& reportsPath);

Effect applyUsageSend(bool on);
Effect applyUsageWhat(bool on);
Effect applyUsagePrivacy();

Effect applyTesterUnlink(const cascade::core::AppConfig& cur);
Effect applyTesterRemoveCode(const cascade::core::AppConfig& cur);
Effect applyTesterCodeCommitted(const std::string& text);
Effect applyTesterUse(const cascade::core::AppConfig& cur, const std::string& draft);
Effect applyTesterShowSent(bool on);

Effect applyReportOpen(int kind);
Effect applyReportText(int kind, const std::string& text);
Effect applyReportContact(int kind, const std::string& contact);
Effect applyReportKind(int index);
Effect applyReportAttach(int kind, bool on);
Effect applyReportPreview(int kind, bool on);
Effect applyReportProbe(bool on);
Effect applyReportSend(int kind, const ReportInputs& in);
Effect applyReportClosed(int kind);
Effect applyAboutOpen();

}  // namespace foxsdr::slintsystem
