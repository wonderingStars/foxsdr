// signal_logic.hpp - THE SIGNAL BANK'S PURE LOGIC (docs/engine-stage4.md, the Slint port, phase 3 bank 1): what the open bodies under the SIGNAL PATH
// rows show - Source (and its eight variants), the Radio row's WFM extras, Audio filters, Sinks, Transmit, Recorder, Frequency correction, the
// Radio setup page and the import dialog - is computed from the C API table's lists, facts and state by these functions, and every control the
// operator touches becomes the exact command the ImGui window's own drawer sends. No Slint type and no engine header appears here (foxsdr_api.h,
// core/app_commands.hpp and the light core/gui headers named below), so the file is tested without a window or an engine
// (tests/test_slint_host_signal.cpp reads a stub table, as the plugin kit's test does).
//
// THE CONTRACT is ui/signal_bank.slint's (the UI family's): the models below are the Slint structs with the same fields in snake_case; the
// publisher (signal_publish.*, not this file) maps them 1:1 and wires each SignalEvents callback to the builder the table below names. A field
// marked "host extra" is not in the contract - the publisher may use it (the row chips, the lamps) or leave it.
//
// WHERE THE RULES COME FROM (the ImGui window, read for behaviour and for every sentence, never for drawing):
//   Source      AppWindow::drawSourceSection + sourceLists (app_window.cpp 7645-8726), app_window_source_row.cpp (the chip, the combo's preview),
//               app_window_airspy.cpp, app_window_soundcard.cpp, app_window_converter.cpp, app_window_ppm.cpp, app_window_radio_setup.cpp
//   Radio       drawStereoRdsControls (app_window.cpp 9295)     Audio filters  drawAudioFilterSection (9358)
//   Sinks       drawSinksSection + railLists (6701, 17404)       Transmit       drawTransmitSection (16811)
//   Recorder    drawRecorderSection + readTake (18858, 17378)    Import         app_window_import_dialog.cpp
// and the sentences are those drawers' own, through cascade::i18n::tr() (the catalogues already hold them; this file adds NO key).
//
// THE CALLBACKS -> COMMANDS (each builder returns the command(s) to submit on the window's session, with the grant the engine asks for; a
// builder that returns nothing sends nothing, exactly as the ImGui control would not; "[local]" is not a command - the publisher keeps it):
//
//   source-device-selected(i)      opDeviceSelected     FOXAPI_OP_SELECT_SOURCE 0x0302  text = fact.source.id.<devices[i].id> read NOW (never the cut detail);
//                                                       nothing when the fact is empty or the row is greyed (a patch holds the radios)      grant SETTINGS
//   source-refresh()               opRefresh            FOXAPI_OP_SCAN_DEVICES 0x0301                                                        grant SETTINGS
//   (the list opened)              opScanOnOpen         FOXAPP_OP_SCAN_DEVICES_ON_OPEN 0x8401 - ImGui sends it on the frame the combo opens; the contract has no callback for it
//   source-usrps(on)               opUsrps              FOXAPP_OP_SET_NETWORK_USRP_SCAN 0x8402  ival[0] = on                                 grant SETTINGS
//   source-setup()                 [local]              shows RadioSetupModel (the page) and starts the USB/driver probe
//   source-rate-selected(i)        opRateSelected       FOXAPI_OP_SET_SAMPLE_RATE 0x0303  num[0] = rates[i] Hz; nothing for the row already shown
//   source-antenna-selected(i)     opAntennaSelected    FOXAPI_OP_SET_ANTENNA 0x0306  text = antennas[i].id; nothing for the port already in use
//   source-auto-gain(on)           opAutoGain           FOXAPI_OP_SET_DEVICE_AGC 0x0305  ival[0] = on
//   source-gain-changed(s, norm)   opGainChanged        FOXAPI_OP_SET_GAIN 0x0304  text = stages[s].name, num[0] = lo + norm*(hi-lo) (a "step" stage rounded to a whole step)
//   source-bias(on)                opBiasTee            FOXAPI_OP_SET_BIAS_TEE 0x0307  ival[0] = on, text = fact.biasTee.radioKey ("" off). ON is sent by the bias sheet after
//                                                       its question is answered; OFF at once
//   ppm-toggled(on)                opPpmToggled         FOXAPP_OP_SET_PPM 0x8426  ival[0] = on, num[0] = the stored value
//   ppm-committed(text)            opPpmCommitted       FOXAPP_OP_SET_PPM 0x8426  ival[0] = 1, num[0] = sanitisePpm(text); nothing when it is the stored value or not a number
//   iq-path-committed(path)        [local]              the draft text (ReadOptions::iq_path is it, given back each publish)
//   iq-open()                      planIqOpen/opIqOpen  FOXAPI_OP_SELECT_SOURCE 0x0302  text = "file:" + path (a path past 255 bytes rides in the long text), the path being the box CLEANED
//                                                       (core::typedIqPath: whitespace and one pair of quotes off); an EMPTY box opens the file dialog (iq-browse) where the machine has
//                                                       one and otherwise says what the box is for (docs 6.63; the classic's openIqFile)
//   iq-browse()                    opIqRow + opIqOpen   [local] the system's file dialog (core::pickFile, "I/Q recordings (*.wav)" first); a choice is "row:iqfile" then "file:<path>"
//                                                       (the classic's openIqFileAt); a cancel changes nothing and says so in the log
//   iq-paste() -> bool             [local]              Ctrl+V in the box with a FILE on the clipboard (core::firstFileOnClipboard): the box takes its path; false = not a file, the box pastes text as ever
//   pluto-address-committed(s)     opPlutoAddress       FOXAPP_OP_SET_PLUTO_URI 0x8418  text = s, sent for each edit
//   pluto-open()                   opPlutoOpen          FOXAPI_OP_SELECT_SOURCE 0x0302  text = "open-pluto:uri=" + the typed address
//   rtltcp-address-committed(s)    opRtlTcpAddress      FOXAPP_OP_SET_RTLTCP_ADDR 0x8424  text = s
//   rtltcp-open()                  opRtlTcpOpen         FOXAPI_OP_SELECT_SOURCE 0x0302  text = "open-rtltcp:rtltcp=" + the typed address
//   card-selected(i)               opCardSelected       FOXAPP_OP_SOUND_CARD_PICK 0x8420  ival[0] = cards[i].id; nothing for the card already chosen
//   card-format(i)                 opCardFormat         FOXAPP_OP_SOUND_CARD_FORM 0x8412  text = "\x1F", ival[0] = (i == 1 ? 1 : 0), ival[1] = FOXAPP_SOUND_CARD_FIELD_FORMAT (2)
//   card-channel(i)                opCardChannel        FOXAPP_OP_SOUND_CARD_FORM 0x8412  text = "\x1F", ival[0] = (i == 1 ? 2 : 0), ival[1] = FIELD_CHANNEL (4)
//   card-swap(on)                  opCardSwap           FOXAPP_OP_SOUND_CARD_FORM 0x8412  text = "\x1F", ival[0] = (on ? 4 : 0), ival[1] = FIELD_SWAP_IQ (8)
//   card-centre-committed(mhz)     opCardCentre         TWO commands, in this order: FOXAPP_OP_SOUND_CARD_FORM 0x8412 (text "\x1F", ival[1] = FIELD_CENTRE 64, num[1] = Hz) then
//                                                       FOXAPP_OP_SOUNDCARD_IQ_CENTRE 0x8405 (num[0] = Hz); nothing when the text is not a finite number
//   card-rate-selected(i)          opCardRate           FOXAPP_OP_SOUND_CARD_FORM 0x8412  text = "\x1F", ival[1] = FIELD_RATE (32), num[0] = card_rates[i] Hz
//   card-open()                    opCardOpen           FOXAPI_OP_SELECT_SOURCE 0x0302  text = "soundcard:open"; nothing while no card is chosen (SourceModel::card_can_open)
//   airspy-decimation(i)           opAirspyDecimation   FOXAPP_OP_AIRSPY_DECIMATION 0x8415  ival[0] = the factor (airspy_decimations[i].id); nothing for the one in force
//   airspy-gain-mode(i)            opAirspyGainMode     FOXAPP_OP_AIRSPY_GAIN_MODE 0x8416  ival[0] = airspyGainModeCode(i) - i is the button order Sensitive 0, Linear 1, Free 2;
//                                                       nothing for the mode in force
//   airspy-gain(norm)              opAirspyGain         FOXAPI_OP_SET_GAIN 0x0304  text = the first stage's name, num[0] = norm * its top (whole steps)
//   airspy-lna-agc(on)             opAirspyAgc          FOXAPP_OP_AIRSPY_AGC 0x8417  ival[0] = 0, ival[1] = on
//   airspy-mixer-agc(on)           opAirspyAgc          FOXAPP_OP_AIRSPY_AGC 0x8417  ival[0] = 1, ival[1] = on
//   airspy-lna / -mixer / -vga(n)  opAirspyStage        FOXAPI_OP_SET_GAIN 0x0304  text = the stage named "LNA" / "MIXER" / "VGA" in airspy_stages, num[0] = norm * its top (whole steps)
//   option-toggled(i, on)          opOptionToggled      FOXAPI_OP_SET_DEVICE_OPTION 0x0308  text = options[i].name ("rf_notch", "dab_notch", "hdr", "dither", "randomiser"), ival[0] = on
//   converter-mode(code)           opConverterMode      FOXAPP_OP_SET_CONVERTER 0x8404  ival[0] = code (0 off 1 up 2 down; converter_modes lists the ones offered), ival[1] = inverted, num[0] = LO Hz
//                                                       (125 MHz when the mode goes on with no valid LO)
//   converter-lo-committed(text)   opConverterLo        FOXAPP_OP_SET_CONVERTER 0x8404  as above with the parsed LO; nothing when it is the stored LO; a text that does not parse sends nothing
//                                                       and the publisher shows converterLoError(text) in converter_error until the next good commit
//   converter-quick(i)             opConverterQuick     FOXAPP_OP_SET_CONVERTER 0x8404  LO = 125 MHz, 100 MHz, 2 MHz for i = 0, 1, 2; nothing for the one in force
//   converter-inverts(on)          opConverterInverts   FOXAPP_OP_SET_CONVERTER 0x8404  ival[1] = on
//   radio-stereo(on)               opStereo             FOXAPI_OP_SET_STEREO 0x010C  ival[0] = on
//   filter-nr(on)                  opNr                 FOXAPI_OP_SET_NR 0x010D  ival[0] = on, ival[1] = 0 (the strength is not set)
//   filter-nr-strength(n)          opNrStrength         FOXAPP_OP_SET_NR_STRENGTH 0x8203  num[0] = n (0..1)
//   filter-notch(on)               opNotch              FOXAPI_OP_SET_NOTCH 0x010E  ival[0] = on, ival[1] = 0
//   filter-notch-freq(norm)        opNotchFreq          FOXAPP_OP_SET_NOTCH_FREQUENCY 0x8204  num[0] = notchHzFromNorm(norm) (10 Hz .. 20 kHz, logarithmic)
//   filter-notch-q(norm)           opNotchQ             FOXAPP_OP_SET_NOTCH_Q 0x8205  num[0] = notchQFromNorm(norm) (1 .. 200)
//   filter-auto-notch(on)          opAutoNotch          FOXAPI_OP_SET_AUTO_NOTCH 0x010F  ival[0] = on
//   filter-agc-mode(index)         opAgcMode            FOXAPI_OP_SET_AGC 0x0111  ival[0] = index (0 Auto .. 4 Off), ival[1] = 0 (the fixed gain is not set)
//   filter-agc-fixed(norm)         opAgcFixed           FOXAPI_OP_SET_AGC 0x0111  ival[0] = the mode now in force (Off), ival[1] = 1, num[0] = agcFixedFromNorm(norm) (-20..+40 dB)
//   filter-nb(on)                  opNb                 FOXAPI_OP_SET_NB 0x0112  ival[0] = on, ival[1] = 0 (the threshold is not set)
//   filter-nb-threshold(norm)      opNbThreshold        FOXAPI_OP_SET_NB 0x0112  ival[0] = the switch now in force, ival[1] = 1, num[0] = nbThresholdFromNorm(norm) (6..30 dB)
//   sink-device-selected(i)        opSinkDevice         FOXAPI_OP_AUDIO_DEVICE 0x0E01  ival[0] = devices[i].id (the PortAudio index); sent for the row in use too (the Selectable does: it re-opens)
//   sink-buffer-selected(i)        opSinkBuffer         FOXAPP_OP_SET_AUDIO_BUFFER 0x8425  ival[0] = buffers[i].id (0 AUTOMATIC, 120, 240, 480, 960 ms)
//   transmit-clicked()             opTransmitPage       FOXAPP_OP_SET_TRANSMIT_PAGE_OPEN 0x8408  ival[0] = the page's NEW state (open flag is the publisher's)   grant TRANSMIT
//   recorder-iq()                  opRecorderIq         FOXAPI_OP_RECORD_IQ 0x0401  ival[0] = 1 when idle, 0 when recording or starting (a press on "Starting IQ - click to cancel"
//                                                       withdraws the start); nothing while the start is being cancelled
//   recorder-audio()               opRecorderAudio      FOXAPI_OP_RECORD_AUDIO 0x0402  ival[0] likewise
//   recorder-import()              [local]              opens the import dialog (ImportDialogModel::shown)
//   setup-key("check-again")       opSetupCheckAgain    FOXAPI_OP_SCAN_DEVICES 0x0301 - and the publisher restarts the USB probe                grant SETTINGS
//   setup-key("install-driver:<target>" | "dont-show" | "link:<url>")  [local]  the one-click WinUSB install (core/winusb_install), the saved tick, the browser
//   setup-closed()                 [local]
//   import-path-committed(path)    opImportPath         kind 0 (frequency list): FOXAPP_OP_BOOKMARK_IMPORT_FILE 0x8601  text = the cleaned path (LOCAL sessions only; a path with no root
//                                                       is looked for on a worker first - the publisher's, as importBookmarkFile does);
//                                                       kind 1 (I/Q recording): FOXAPI_OP_SELECT_SOURCE 0x0302  text = "file:" + path
//   import-kind(i) / import-open() / import-closed()  [local]   the dialog's kind, its Browse key (core::pickFile) and its Cancel
//
// SELECTITEM IDS: devices - the DEVICES entry's id (decimal; the key to fact.source.id.<id>); rates and card_rates - the rate in Hz ("%.17g"); antennas - the port's
// name; cards - the SOUND_CARDS entry's id (decimal); airspy_decimations - the factor; sinks.devices - the PortAudio index; sinks.buffers - the milliseconds;
// import kinds - "frequencies" and "iq".
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include "foxsdr_api.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "core/app_commands.hpp"
#include "kit_logic.hpp"

namespace foxsdr::slintsignal {

using foxsdr::slintkit::TableReader;

// The tone of a chip, in the order of ui/chips.slint's ChipTone (neutral, good, warn, err).
enum class Tone { Neutral, Good, Warn, Err };

// ---- the models (ui/signal_bank.slint's structs, snake_case) ----
struct SelectItem {
    std::string label, id;
    bool enabled = true;
    std::string note;       // a dim suffix ("(busy)")
    std::string tooltip;    // host extra: the sentence the ImGui row hovers with ("" none)
};
struct GainStage {
    std::string name, text;   // "TUNER", "32.8 dB"
    float norm = 0.f;         // 0..1 over [lo, hi]
    bool enabled = true;      // greyed while Auto gain drives it
    std::string unit;         // "dB" or "step"
    double lo = 0.0, hi = 0.0, value = 0.0;   // host extra: the range the driver reports and the readback, for the command
};
struct OptionRow {
    std::string name, label;   // name: the option the command takes ("rf_notch"); label: translated
    bool on = false, enabled = true;
    std::string tooltip;
};

struct SourceModel {
    std::string device_label;              // the combo's closed text: the source, "(saved, not open)", "... - the patch has ..."
    std::vector<SelectItem> devices;
    int device_index = -1;                 // fact.source.row, -1 none
    bool refresh_enabled = true;
    bool look_for_usrps = false;
    std::string setup_key_label;           // "RADIO SETUP"
    std::string rate_text;
    std::vector<SelectItem> rates;
    std::string actual_rate;               // "actual 2.4 MS/s"
    std::string antenna_text;              // the port in use, or "Antenna: X" for a single port
    std::vector<SelectItem> antennas;
    bool single_port = false;
    bool auto_gain = false, auto_gain_shown = false;
    std::string agc_note;                  // "Auto gain: not supported" or ""
    std::vector<GainStage> stages;         // the generic sliders (empty for an Airspy: it draws its own)
    bool bias_shown = false, bias_on = false, bias_enabled = true;
    // Frequency correction
    bool ppm_on = false;
    std::string ppm_radio_line, ppm_text, ppm_note, ppm_refusal;
    std::string error_line, mismatch_note, advisory;
    Tone advisory_tone = Tone::Neutral;
    std::string stopped_line;
    int variant = 0;   // 0 none, 1 I/Q file, 2 Pluto, 3 rtl_tcp, 4 sound card, 5 Airspy, 6 radio extras (device options), 7 converter in force
    // the variant cards
    std::string iq_path, iq_note;
    bool iq_browse = false;                // the Browse key is drawn: this machine has a file dialog (ReadOptions::iq_browse_offered)
    bool iq_opening = false;               // the header is being read on a worker (fact.source.iqOpening): Browse and Open are greyed
    std::string pluto_address, pluto_help;
    std::string rtltcp_address, rtltcp_help;
    std::string card_busy, card_none;
    std::vector<SelectItem> cards;
    int card_index = -1, card_format_index = 0, card_channel_index = 0;
    bool card_swap_iq = false;
    std::string card_centre_mhz;
    std::vector<SelectItem> card_rates;
    int card_rate_index = -1;
    std::string card_receives, card_warning;
    std::vector<SelectItem> airspy_decimations;
    int airspy_decimation_index = 0;
    std::string airspy_hardware_rate;      // "radio at 10 MS/s, decimated by 4" or ""
    int airspy_gain_mode = 0;              // the button order: Sensitive 0, Linear 1, Free 2
    float airspy_gain_norm = 0.f;
    bool airspy_lna_agc = false, airspy_mixer_agc = false;
    float airspy_lna = 0.f, airspy_mixer = 0.f, airspy_vga = 0.f;
    std::vector<OptionRow> options;
    bool converter_shown = false;
    int converter_mode = 0;                // the code: 0 off, 1 up, 2 down
    std::string converter_alias;           // the amber notes under the mode (the SoapySDR fallback, an unusable up-converter), joined by a newline
    std::string converter_lo;              // "125 MHz"
    bool converter_inverts = false;
    std::string converter_error, converter_on_air;
    // ---- host extras (not in the contract) ----
    std::string chip;                      // the Source row's chip: "RTL-SDR", "Signal gen", "ON PATCH"
    Tone chip_tone = Tone::Neutral;
    bool lamp_lit = false;
    Tone lamp_tone = Tone::Good;           // Err (alarm) while faulted or the saved radio did not open, Good (phosphor) otherwise
    bool device_open = false;              // a radio is installed: the rate/antenna/gain panel is drawn
    bool opening = false;                  // a radio is being opened (the converter and ppm controls are disabled)
    std::string busy_line;                 // "Opening X..." / "Scanning for devices..." (also the first paragraph of the advisory)
    bool card_can_open = false;
    bool card_enabled = true;
    std::string card_text;                 // the sound card combo's closed text
    std::vector<SelectItem> converter_modes;   // the modes the open source takes (id = code)
    std::vector<GainStage> airspy_stages;  // the Airspy's stages in the list's order (name, norm, enabled = its AGC is not driving it)
    std::string ppm_chip;                  // the Frequency correction row's chip: "OFF", "ON" or "PPM +1.5"
    std::string ppm_stored;                // the stored value, "+1.5"
    double ppm_value = 0.0;                // ...and as a number, for opPpmToggled / opPpmCommitted
    double converter_lo_hz = 0.0;          // the stored LO as a number, for the converter commands
    bool ppm_enabled = true;
    bool ppm_shown_value = false;          // the value box and its notes are drawn (on, and the source has a crystal)
    // the ImGui tooltips of the controls (the same sentences, translated), so the UI need not word them again
    std::string usrps_tooltip, bias_tooltip, ppm_tooltip, converter_tooltip, converter_lo_tooltip, converter_inverts_tooltip;
    // THE KEY THAT RESTARTS THE SDRPLAY API SERVICE (app_window_sdrplay.cpp): the process-local state is the publisher's (ReadOptions::sdrplay_*); pressing it is [local] (an elevated helper)
    bool sdrplay_key_shown = false, sdrplay_key_enabled = true;
    std::string sdrplay_key_label, sdrplay_line;
};

struct RadioExtrasModel {
    bool wfm = false, stereo = false;
    std::string stereo_word;               // "ST", "ST (forced mono)", "MONO"
    Tone stereo_tone = Tone::Neutral;      // Good, Warn, Neutral
    bool rds_shown = false;
    std::string rds_ps, rds_rt, rds_pi;
    // host extras
    std::string stereo_tooltip, pilot_text, rds_status, rds_stats;
};

struct AudioFiltersModel {
    std::string chain_note;
    bool nr_on = false;
    float nr_strength = 0.f;
    std::string nr_text;
    bool notch_on = false;
    float notch_freq = 0.f;                // 0..1 on the logarithmic 10 Hz .. 20 kHz span
    std::string notch_freq_text;
    float notch_q = 0.f;                   // 0..1 over 1 .. 200
    std::string notch_q_text;
    bool auto_notch_on = false;
    std::string auto_notch_status;
    bool auto_notch_locked = false;
    // AGC choice and noise blanker (lane C): the selected radio's.
    std::string agc_text;                  // the chosen mode's word ("Auto", "Fast", "Medium", "Slow", "Off")
    std::vector<SelectItem> agc_modes;     // the five, in FOXAPI_AGC_* order; id = the mode number
    int agc_index = 0;
    float agc_fixed = 0.f;                 // 0..1 over -20..+40 dB
    std::string agc_fixed_text;            // "+12 dB"
    bool agc_fixed_on = false;             // the Fixed gain slider is live only in Off
    std::string agc_gain_text;             // the gain in use, "+12 dB"
    int agc_gain_int = 0;                  // ...whole dB
    std::string agc_gain_tip;              // "AGC gain now: 12 dB" (the readout's accessible name)
    bool nb_on = false;
    float nb_threshold = 0.f;              // 0..1 over 6..30 dB
    std::string nb_threshold_text;         // "15 dB"
    int nb_pct = 0;                        // share of the last 100 ms blanked
    std::string nb_read;                   // "Blanking 3 %", or "Standing down: signal too strong" while nb_standing; "" while the blanker is off
    bool nb_standing = false;              // letting everything through: a strong signal is not a click
    // FM de-emphasis (slint-gaps.md item 1): the engine's SET_DEEMPHASIS order, 0 = 50 us, 1 = 75 us, 2 = off; live in NFM and WFM only
    std::vector<SelectItem> deemph_modes;
    int deemph_index = 0;
    std::string deemph_text;
    bool deemph_enabled = false;
    // host extras
    std::string chip;                      // "OFF" or "2 ON"
    bool lamp_lit = false;
};

struct SinksModel {
    std::string device_text;
    std::vector<SelectItem> devices;
    int device_index = -1;
    std::string none_line, health_note;
    std::string buffer_text;
    std::vector<SelectItem> buffers;
    int buffer_index = 0;
    std::string lead_notice, muted_by, muted_note;
    // host extras
    std::string chip;                      // ON, OPENING, NO DEV, DEAD, MUTED
    Tone chip_tone = Tone::Good;
    bool lamp_lit = false;
    std::string buffer_tooltip;
};

struct TransmitModel {
    std::string chip;                      // NO RADIO, READY, ON AIR, FAULT
    Tone tone = Tone::Neutral;
    bool available = false, armed = false, keyed = false;
    std::string fault;
};

struct RecorderModel {
    std::string folder;
    std::string iq_label;
    bool iq_enabled = true;
    std::string iq_progress;
    bool iq_running = false;
    std::string audio_label;
    bool audio_enabled = true;
    std::string audio_progress;
    bool audio_running = false;
    std::string muted_warning, hint, error_line, notice, import_label;
    // host extras
    std::string chip;                      // OFF, REC, FULL, STALLED
    Tone chip_tone = Tone::Neutral;
    bool lamp_lit = false;
    int iq_start = 0, audio_start = 0;     // fact.recorder.start: 0 none out, 1 the file is opening, 2 the start was withdrawn
};

struct UnboundRow { std::string name, family, vid, pid, advice; };
struct SetupKey {
    std::string id, label;
    bool enabled = true;
    std::string tooltip;
    bool on = false;                       // host extra: the "Don't show this again" tick
};
struct RadioSetupModel {
    bool shown = false;
    std::string title;
    std::vector<std::string> lines;
    std::vector<UnboundRow> unbound;
    std::vector<SetupKey> keys;
    std::string busy;
};

struct ImportDialogModel {
    bool shown = false;
    std::string path, error;
    std::vector<SelectItem> kinds;
    int kind_index = 0;
    std::string preset;                    // ADDED (docs 6.61): the Airband preset the rows will join; "" for the plain import. The dialog says so, and offers no I/Q file
    std::string preset_line;               // ...and the sentence it says (the ImGui dialog's own, through tr(): "The rows will join the preset \"X\"."); "" for the plain import
    // host extras
    std::string reading_line;              // "Waiting for the disk to open the file: 2 s" after a second
};

// One finding of the Radio setup page (core/radio_setup.hpp's Finding, as the publisher copies it from rs::assess; this file does not link that probe).
struct SetupFinding {
    std::string title, explanation, install, url, install_target;
    bool usable = false, not_checked = false;
};

struct SignalModels {
    SourceModel source;
    RadioExtrasModel radio;
    AudioFiltersModel filters;
    SinksModel sinks;
    TransmitModel transmit;
    RecorderModel recorder;
    RadioSetupModel setup;
    ImportDialogModel import;
};

// What the publisher owns and hands in each time (the drafts, the clocks, the pages' open flags, the probe's answer).
struct ReadOptions {
    std::string iq_path;                         // the I/Q path box
    std::string iq_box_note;                     // the box's own line ("Type or paste the path of a WAV recording, then press Open."), "" none
    bool iq_browse_offered = false;              // a file dialog exists on this machine (core::filePickAvailable, or the bounded run's seam)
    double iq_open_wait_s = 0.0;                 // seconds since Open was pressed on the I/Q file (the line shows from 1 s); 0 none out
    std::vector<std::string> patch_held;         // names of the hardware radios a running patch holds
    std::string converter_lo_bad;                // the LO text the last commit could not read; "" none
    bool sdrplay_key_shown = false, sdrplay_restarting = false;   // the RESTART SDRPLAY SERVICE key is offered / its helper is running
    std::string sdrplay_note;                    // the outcome sentence of the last restart ("" none)
    double iq_start_wait_s = 0.0, audio_start_wait_s = 0.0;   // seconds the recorder's start has been out
    // the Radio setup page
    bool setup_shown = false, setup_checking = false, setup_have_answer = false, setup_show_page = false, setup_installing = false, setup_dont_show = false;
    std::string setup_install_status;
    std::vector<SetupFinding> findings;
    // the import dialog
    bool import_shown = false;
    std::string import_preset;             // the Airband preset the rows will join ("" = the plain frequency-list import), docs 6.61
    std::string import_path, import_reason;
    int import_kind = 0;
    bool import_reading = false;
    double import_wait_s = 0.0;
};

// ---- the read: lists and facts through the seam, the state as the engine published it ----
SourceModel readSource(TableReader& table, const FoxReceiverState& state, const ReadOptions& options);
RadioExtrasModel readRadioExtras(const FoxReceiverState& state);
AudioFiltersModel readAudioFilters(const FoxReceiverState& state);
SinksModel readSinks(TableReader& table, const FoxReceiverState& state);
TransmitModel readTransmit(const FoxReceiverState& state);
RecorderModel readRecorder(TableReader& table, const FoxReceiverState& state, const ReadOptions& options);
RadioSetupModel readRadioSetup(TableReader& table, const FoxReceiverState& state, const ReadOptions& options);
ImportDialogModel readImportDialog(const ReadOptions& options);
SignalModels readSignal(TableReader& table, const FoxReceiverState& state, const ReadOptions& options);

// ---- the small pure rules ----
// The Source row's chip: the device in use cut to its last ": " part and to ten characters; the built-in generator's own short word; "ON PATCH" while a running patch holds radios.
std::string sourceChipText(const std::string& deviceName);
std::string sourceChip(const std::string& deviceName, const std::vector<std::string>& patchHeld);
// The combo's closed text while the patch has radios: "Signal generator - the patch has NESDR SMArt v5".
std::string sourceRowText(const std::string& receiverSource, const std::vector<std::string>& patchRadios);
// The lamp's "the saved radio did not open" rule (core/source_fallback.hpp radioNotOpenLampFromSession).
bool radioNotOpenLamp(const std::string& keepKind, bool deviceOpen, bool generatorInUse, bool cardOpening);
// "No radio hardware found" (gui/source_row_status.hpp): a complete scan landed and neither list holds a radio.
bool noRadioHardwareFound(bool scanned, bool partial, bool busy, std::size_t soapyRows, std::size_t nativeRows);
// Which card the Source body shows under the combo (SourceModel::variant): by the combo's row, then the open radio.
enum Variant : int { kVariantNone = 0, kVariantIqFile = 1, kVariantPluto = 2, kVariantRtlTcp = 3, kVariantSoundCard = 4, kVariantAirspy = 5, kVariantOptions = 6, kVariantConverter = 7 };
int sourceVariant(int row, bool plutoRow, bool rtltcpRow, bool deviceOpen, bool airspy, bool haveOptions, bool converterInForce);

// gains: the slider's 0..1 over [lo, hi] (0..60 dB when the driver reports no range), its text ("32.8 dB", a step stage "7"), and the way back
float gainNorm(double lo, double hi, double value);
double gainFromNorm(double lo, double hi, float norm, bool steps);
std::string gainText(double value, bool steps);

// the Airspy: the gain mode buttons are Sensitive 0, Linear 1, Free 2; the engine's code is AirspySource::GainMode (Linearity 0, Sensitivity 1, Free 2)
int airspyGainModeCode(int buttonIndex);
int airspyGainModeIndex(int code);
std::string airspyModeWord(int buttonIndex);                 // "Sensitive", "Linear", "Free"
std::string airspyModeTooltip(int buttonIndex);
std::string airspyDecimationLabel(unsigned factor);          // "None", "2", "4" ...
std::string airspyHardwareLine(double hardwareRateHz, unsigned decimation);   // "" at 1

// the sound card
std::string soundCardHzText(double hz);                      // "192 kHz", "44.1 kHz", "7.0500 MHz"
std::string soundCardRateLabel(double hz, bool exclusive);   // "48 kHz (exclusive)"
std::string soundCardReceivesLine(double loHz, double hiHz); // "Receives 7.0000 MHz to 7.0480 MHz."

// the converter
std::string converterAliasNote(const std::string& state);    // fact.converter.alias: "notCarried" / "carried" / ""
std::string converterUnusableNote(bool upIgnored);
std::string converterOnAirLine(double airHz, double radioHz);
std::string converterLoError(const std::string& typed);      // could not read frequency "..."
constexpr double kConverterQuickLoHz[3] = {125.0e6, 100.0e6, 2.0e6};

// the Zadig / WinUSB advice by family (UNBOUND_RADIOS detail: "rtl", "rx888" or ""), the SDRplay advice paragraph, the network-USRP hints
std::string zadigAdvice(const std::string& name, const std::string& family);

// frequency correction
std::string ppmValueText(double ppm);                        // "+1.5 ppm"
bool parsePpm(const std::string& text, double& ppm);
std::string ppmHowText(const std::string& method, bool wholeRadio, int wholeValue);
std::string ppmChip(bool on, const std::string& card);

// audio filters
std::string autoNotchStatus(bool enabled, bool engaged, double hz);   // "on 1000 Hz", "searching", "" when off
float notchNormFromHz(double hz);
double notchHzFromNorm(float norm);
float notchQNorm(double q);
double notchQFromNorm(float norm);
std::string filtersChip(int filtersOn);
// AGC choice and noise blanker: the sliders' spans and the five modes' words (translated; FOXAPI_AGC_* order)
float agcFixedNorm(double db);
double agcFixedFromNorm(float norm);
float nbThresholdNorm(double db);
double nbThresholdFromNorm(float norm);
std::vector<SelectItem> agcModes();
std::vector<SelectItem> deemphModes();   // 50 us (EU/world), 75 us (Americas), Off: FOXAPI_OP_SET_DEEMPHASIS order

// sinks
std::vector<int> bufferChoices();                            // 0 (AUTOMATIC), 120, 240, 480, 960
std::string bufferLabel(int ms);                             // "AUTOMATIC", "240 ms"
std::string bufferPreview(int bufferMs, int leadMs);         // "AUTOMATIC  240 ms", "480 ms"
std::string audioLeadNotice(const std::string& leadNote);    // fact.audio.leadNote "raised|restored|lowered <ms> <count>"
struct SinkChip { std::string word; Tone tone = Tone::Good; bool lit = false; };
SinkChip sinkChip(const FoxReceiverState& state);

// recorder
enum class Take { Iq, Audio };
// "Record IQ" / "Stop IQ" / "Starting IQ - click to cancel" / "Cancelling IQ start..." (and the audio ones); start: fact.recorder.start's character (0, 1, 2)
std::string recorderLabel(Take take, int start, bool recording);
std::string takeProgress(Take take, double seconds, double bytes);   // "IQ 3.2 s | 3200000 samples | 25.6 MB"
std::string diskWaitLine(double waitedS);                            // "Waiting for the disk to open the file: 2 s" from one second, else ""
int recorderStartOf(const std::string& startFact, Take take);       // the take's character of fact.recorder.start, 0 when absent
struct RecorderChip { std::string word; Tone tone = Tone::Neutral; bool lit = false; };

// transmit
struct TxChip { std::string word; Tone tone = Tone::Neutral; };
TxChip transmitChip(bool haveRadio, bool faulted, bool onAir);

// ---- the commands ----
struct Command {
    cascade::core::cmd::QueuedCommand q;
    std::uint64_t grant = FOXAPI_GRANT_SETTINGS;
    bool valid = false;
};
std::uint64_t grantFor(std::uint32_t op);   // the grant the engine asks of an op (fox_engine_api.cpp grantFor)
std::string textOfCommand(const Command& c);

Command opDeviceSelected(TableReader& table, const SourceModel& m, int index);
Command opRefresh();
Command opScanOnOpen();
Command opUsrps(bool on);
Command opRateSelected(const SourceModel& m, int index);
Command opAntennaSelected(const SourceModel& m, int index);
Command opAutoGain(bool on);
Command opGainChanged(const SourceModel& m, int stage, float norm);
Command opBiasTee(bool on, const std::string& radioKey);
Command opPpmToggled(bool on, double stored);
Command opPpmCommitted(const std::string& text, double stored);
Command opIqOpen(const std::string& path);
Command opIqRow();   // SELECT_SOURCE "row:iqfile": the Source combo moves to the I/Q file row, so what follows is drawn where it happens
// WHAT OPEN DOES WITH THE BOX (the classic's AppWindow::openIqFile, docs 6.63), pure: `typed` is the box's text. The path is core::typedIqPath(typed) - Explorer's
// "Copy as path" quotes and a pasted line's trailing space come off, and the box shows what is tried. An EMPTY box is not a file that would not open:
// where a file dialog is offered the answer is `browse`; where none is, `note` says what the box is for (and `log` is the line for the log).
struct IqOpenPlan {
    std::string path;       // the box cleaned; "" = nothing to open
    bool browse = false;    // open the file dialog
    std::string note;       // the box's line when there is nothing to open and no dialog ("" otherwise)
    std::string log;        // the log line for that case ("" none)
    Command open;           // SELECT_SOURCE "file:<path>"; invalid when path is empty
};
IqOpenPlan planIqOpen(const std::string& typed, bool pickOffered);
Command opPlutoAddress(const std::string& text);
Command opPlutoOpen(const std::string& address);
Command opRtlTcpAddress(const std::string& text);
Command opRtlTcpOpen(const std::string& address);
Command opCardSelected(const SourceModel& m, int index);
Command opCardFormat(int index);
Command opCardChannel(int index);
Command opCardSwap(bool on);
std::vector<Command> opCardCentre(const std::string& mhzText);
Command opCardRate(const SourceModel& m, int index);
Command opCardOpen(const SourceModel& m);
Command opAirspyDecimation(const SourceModel& m, int index);
Command opAirspyGainMode(const SourceModel& m, int buttonIndex);
Command opAirspyGain(const SourceModel& m, float norm);
Command opAirspyAgc(bool mixer, bool on);
Command opAirspyStage(const SourceModel& m, const std::string& stageName, float norm);   // "LNA", "MIXER", "VGA"
Command opOptionToggled(const SourceModel& m, int index, bool on);
// The stored converter setting the three converter controls start from (mode code, LO Hz, inverted).
struct ConverterSetting { int mode = 0; double lo_hz = 0.0; bool inverted = false; };
ConverterSetting converterStored(const SourceModel& m);   // what the model carries of the stored setting
Command opConverterMode(const ConverterSetting& stored, int code);
Command opConverterLo(const ConverterSetting& stored, const std::string& text);
Command opConverterQuick(const ConverterSetting& stored, int index);
Command opConverterInverts(const ConverterSetting& stored, bool on);
Command opStereo(bool on);
Command opNr(bool on);
Command opNrStrength(float strength);
Command opNotch(bool on);
Command opNotchFreq(float norm);
Command opNotchQ(float norm);
Command opAutoNotch(bool on);
Command opAgcMode(int index);
Command opDeemph(int index);             // FOXAPI_OP_SET_DEEMPHASIS, ival[0] = 0 | 1 | 2; anything else is not valid
Command opAgcFixed(int modeNow, float norm);
Command opNb(bool on);
Command opNbThreshold(bool onNow, float norm);
Command opSinkDevice(const SinksModel& m, int index);
Command opSinkBuffer(const SinksModel& m, int index);
Command opTransmitPage(bool open);
Command opRecorderIq(const RecorderModel& m);
Command opRecorderAudio(const RecorderModel& m);
Command opSetupCheckAgain();
Command opImportPath(const std::string& path, int kind);

}  // namespace foxsdr::slintsignal
