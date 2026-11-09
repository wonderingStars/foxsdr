// transmit_logic.hpp - THE TRANSMIT PAGE'S PURE LOGIC (docs/engine-stage4.md 6.58, the Slint port: the classic window's Transmit page in the new window). What the page shows - the
// radio, the frequency with SPLIT, the mode, the power, the input and its tone, the audio meter, the PTT and LATCH keys with their lamp, "Allow remote transmit", "Listen while
// transmitting", the notes, and the ARM FOR PLUGINS block with DATA (flat) - is computed from the receiver state and the table's facts by these functions, and every control the
// operator touches becomes the exact command the ImGui window's own control sends. No Slint type and no engine library appears here (foxsdr_api.h, core/app_commands.hpp and the
// ImGui-free headers gui/transmit_page.hpp, dsp/modulator.hpp), so the file is tested without a window or an engine (tests/test_slint_host_transmit.cpp).
//
// SAFETY, AND WHERE IT LIVES. This file holds no state that could key a radio and no function that could: it BUILDS COMMANDS FOR THE ENGINE'S SESSION, and the engine judges them
// under its own key rules (core/transmitter.hpp: a hold the interface must keep re-asserting, a latch that ends itself, consent that lapses with the page, a lease on this session).
// What this file adds to the classic page's rules is only what the new window needs to be as safe as the old one:
//   1. THE PAGE IS "LIVE" ONLY WHILE ITS CONTROLS ARE ON SCREEN (the host passes pageLive each tick: the page is open, is the current view or torn off, and is not rolled up). A page
//      that stops being live lets go of the PTT, the latch this window holds, its consent to remote transmit and the plugins' arming window, in the very next tick (txKeyCommands,
//      txPluginArmCommands - the classic's own functions, quoted, not copied).
//   2. THE PTT IS A HOLD THAT IS RE-ASSERTED EVERY TICK while the pointer or the bound chord is down AND a radio is open AND the page is live; the tick that finds any of the three
//      gone sends TX_PTT 0. A tick that stops (a frozen window) stops the re-assertion, and the engine opens the key within kKeyAliveWait (1 s) on its own thread.
//   3. NOTHING HERE SENDS A KEYING COMMAND FOR ANYTHING BUT A PRESS: TX_PTT 1 needs the pointer or the chord down on the page, TX_LATCH 1 needs a press of the LATCH key, TX_REMOTE_ARM 1
//      a click of the tick box, TX_PLUGIN_ARM 5/15/60 a press of a length key. Every one of those controls is greyed while no radio is open (the arming keys excepted: pressing one
//      with no radio is how the page says why it cannot, as the classic's do), and the functions here refuse the press as well - the control being greyed is not the only guard.
//   4. A BOUNDED RUN WITHHOLDS what could put RF out (the publisher's rule, transmit_publish.cpp): TX_OPEN always, and any keying op when a radio is open.
//
// THE SHAPE. TransmitPage owns the little the page remembers between ticks (the address draft, the key's memory, the presses of this tick, the note a refused LATCH or consent left)
// and is driven by: read() once a tick, one method per gesture, tick() once a tick (the key's batch) and onAnswer() for each answer the session polls.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include "foxsdr_api.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "core/app_commands.hpp"
#include "gui/transmit_page.hpp"
#include "kit_logic.hpp"
#include "signal_logic.hpp"

namespace foxsdr::slinttransmit {

using foxsdr::slintkit::TableReader;
using foxsdr::slintsignal::Command;
using foxsdr::slintsignal::Tone;

constexpr int kModeCount = 5;                         // CW AM NFM USB LSB (dsp::kTxModeCount)
constexpr int kInputCount = 2;                        // MIC, TONE (core::kTxHandInputCount)
constexpr std::size_t kAddressBytes = 255;            // the address box's char[256] less the NUL (the classic's transmitArgsDraft_)
constexpr float kMeterDecay = 0.90f;                  // the audio meter's fall per tick: "a fast rise and a slow fall" (drawTransmitPage)
constexpr double kMinFrequencyHz = 0.0;               // a typed frequency must be above this ...
constexpr double kMaxFrequencyHz = 6.0e9;             // ... and no higher than this (the table refuses a tune outside (0, 6 GHz], and so does this page)

// ---- what the page reads ----

struct PluginReading {
    bool armed = false;                // fact.tx.plugin.armed == "1"
    int minutes = 0;                   // fact.tx.plugin.minutes (0 when not armed)
    int seconds_left = 0;              // fact.tx.plugin.secondsLeft
    std::string name;                  // fact.tx.plugin.name
    std::string keyed;                 // fact.tx.plugin.keyed ("" none; else the time on air)
    int refusal = 0;                   // fact.tx.plugin.refusal (cascade::core::TxArming::Refusal)
    std::string reason;                // fact.tx.plugin.reason (why the window last closed on its own)
    bool data_flat = true;             // fact.tx.plugin.dataFlat != "0"
};

struct Reading {
    std::uint32_t flags = 0, tx_flags = 0;
    int mode = 0, input = 0;
    double power_db = 0.0, power_min_db = -89.75, power_max_db = 0.0, frequency_hz = 0.0, split_hz = 0.0, tone_hz = 0.0, input_peak = 0.0, tuned_hz = 0.0;
    std::string device_name, tx_error, unkey_reason;
    std::string args;                  // fact.transmit.args (the address the engine uses: the one typed, else its seed)
    std::string transmit_error;        // fact.status.transmitError
    PluginReading plugin;
    bool have = false, on_air = false, latched = false, key_mine = false, remote_armed = false, faulted = false, split = false, monitor = false, mic_opening = false, has_range = false;
};

Reading readTransmit(TableReader& t, const FoxReceiverState& st);

// ---- the model the page shows (ui/transmit_page.slint's TransmitPageModel, snake_case) ----

struct Model {
    // the radio
    std::string address;               // the address box (the draft while it is being typed, else the engine's)
    bool address_enabled = true;       // greyed while a radio is open: the field would describe something other than what is connected
    bool have = false;                 // a radio is open: Close and the controls below are live
    bool open_enabled = false;         // Open: not while the box is empty
    bool close_enabled = false;        // Close: not while the key is down
    std::string open_tooltip;
    std::string radio_line;            // the open radio's name, the transmitter's error (red), or "No transmitter is open."
    Tone radio_tone = Tone::Neutral;   // Neutral for the name and the idle sentence, Err for the error
    // the frequency
    std::string frequency;             // "%.6f" MHz of where it would transmit
    bool frequency_enabled = false;    // only with a radio open AND SPLIT on
    bool split = false;
    bool split_enabled = false;
    std::string following_note;        // "following the receiver" while SPLIT is off
    // the mode
    int mode = 0;
    bool modes_enabled = false;
    // the power
    float power = 0.0f;                // 0..1 of the board's span, right = louder
    std::string power_text;            // "QUIET" or "-20.0 dB"
    bool power_enabled = false;
    std::string power_note;            // "(the board published no range)" with a radio open and no range
    // the input
    int input = 0;
    bool inputs_enabled = false;       // greyed while the microphone is still opening
    bool tone_shown = false;           // the Hz box, with the TONE input
    std::string tone_hz;               // "%.0f"
    bool tone_enabled = false;
    float meter = 0.0f;                // 0..1, decayed
    // the key
    std::string lamp_word;             // NO RADIO / READY / ON AIR / FAULT
    Tone lamp_tone = Tone::Neutral;    // Neutral NO RADIO, Warn READY, Err ON AIR, Err FAULT
    bool on_air = false;
    bool latched = false;
    bool key_enabled = false;          // PTT, LATCH, Allow remote transmit and Listen while transmitting: a radio is open
    bool remote_armed = false;
    bool monitor = false;
    std::string key_note;              // why a LATCH or the consent was refused (amber)
    std::string unkey_reason;          // why the key last opened on its own (amber)
    std::string error_line;            // the transmitter's error (red)
    // ARM FOR PLUGINS
    bool armed = false;
    int armed_minutes = 0;             // 5, 15 or 60 while armed
    std::string armed_line;            // "ARMED: <plugin> may key this transmitter, 4:32 left."
    std::string on_air_line;           // "ON AIR for <time>." while a plugin has the key
    bool plugin_on_air = false;
    std::string not_armed_line;        // "Not armed: no plugin can key this transmitter."
    std::string refusal_line;          // why the last ARM was refused (red)
    std::string reason_line;           // why the window last closed on its own (amber), while not armed
    bool data_flat = true;
    // the sentence
    std::string licence;
};

// ---- the words ----

const char* modeName(int mode);                       // "CW" ... (radio vocabulary, not translated)
std::string modeCaption(int mode);                    // the mode's sentence, translated
const char* inputName(int input);                     // "MIC", "TONE" (the classic's txInputName)
std::string inputLabel(int input);                    // ... translated
std::string armLabel(int minutes);                    // "5 MIN" / "15 MIN" / "60 MIN", translated

// ---- the gestures' pure parts ----

// A typed MHz figure (the frequency box): the Hz it means, false when it is not a finite number in (0, 6 GHz]. A comma is not a decimal point here.
bool parseMegahertz(const std::string& text, double& hz);
// A typed tone (the Hz box): false when it is not a finite positive number. The engine clamps the figure (TX_SET_TONE) and the page reads it back.
bool parseHertz(const std::string& text, double& hz);
// The address box's text, cut to the draft's room.
std::string cutAddress(const std::string& text);
bool addressBlank(const std::string& text);

// A key event of the page against the bound PTT chord ("Space", "Ctrl+T", "None"): does a press of `text` with these modifiers hold the key? The text is Slint's key text
// (" " for the spacebar, a letter, "\t" ...). An unbound chord ("None", "") never matches.
bool chordMatches(const std::string& chord, const std::string& keyText, bool ctrl, bool shift, bool alt);

// A BOUNDED RUN (a test or a capture on a machine that may have a radio) sends only what cannot put RF out or open something: TX_OPEN and the microphone input never; PTT, LATCH and
// the consent only to LET GO (0); an arming press only while no radio is open (the engine then refuses it - the reason the page shows); everything else, including the safer
// commands and the plugin lease's beat. The publisher applies this to every command it sends.
bool boundedMaySend(const FoxCommand& c, bool haveRadio);

// ---- the page ----

class TransmitPage {
public:
    // The tick's reads (call first). The meter decays here.
    void read(TableReader& t, const FoxReceiverState& st);
    const Reading& reading() const { return r_; }
    Model model() const;

    // The address box: each edit is kept as the draft and sent as it is typed (FOXAPP_OP_SET_TRANSMIT_ARGS, so config.json keeps what was typed). Nothing while a radio is open.
    Command addressEdited(const std::string& text);
    // Open: TX_OPEN of the draft. Nothing for a blank draft or while a radio is open.
    Command open() const;
    // Close: TX_CLOSE. Nothing while no radio is open or while the key is down.
    Command close() const;
    // The frequency box, committed: TX_SET_FREQUENCY (Hz). Nothing without a radio, with SPLIT off, or for a text that is not a frequency.
    Command frequencyCommitted(const std::string& text) const;
    // The - / + keys beside the box: one kHz down or up from where it would transmit (the classic's InputDouble steps 0.001 MHz).
    Command frequencyStepped(int direction) const;
    // SPLIT: TX_SET_SPLIT of the receiver's dial and the flipped state. Nothing without a radio.
    Command splitPressed() const;
    // A mode key (0..4): TX_SET_MODE. Nothing without a radio or outside the five.
    Command modeSelected(int mode) const;
    // The power slider (0..1, right = louder): TX_SET_POWER in the board's own decibels. Nothing without a radio.
    Command powerChanged(float fraction) const;
    // MIC / TONE (0, 1): TX_SET_INPUT. Nothing without a radio, while the microphone is opening, or outside the two.
    Command inputSelected(int input) const;
    // The tone box, committed: TX_SET_TONE (Hz). Nothing without a radio, off the TONE input, or for a text that is not a number.
    Command toneCommitted(const std::string& text) const;
    // Listen while transmitting: TX_SET_MONITOR. Nothing without a radio.
    Command monitorToggled(bool on) const;
    // DATA (flat): the engine's setting, session-only; the classic's box is live whatever the radio.
    Command dataFlatToggled(bool on) const;

    // THE KEY. The PTT's two sources are levels the page sets while they are down; the LATCH press, the consent click and the arming press are events kept for the next tick. Every one
    // is ignored without a radio open (arming excepted), whatever the control drew.
    void pttPointer(bool down) { ptt_pointer_ = down && r_.have; }
    void pttChord(bool down) { ptt_chord_ = down && r_.have; }
    void latchPressed() { if (r_.have) { latch_pressed_ = true; } }
    void remoteArmToggled(bool on) { if (r_.have) { arm_request_ = on ? 1 : 0; } }
    // 5, 15 or 60 arm for that many minutes; 0 is OFF. Anything else is not sent (the engine would refuse it as BadMinutes).
    void pluginArmPressed(int minutes);

    // The key's batch for this tick (txKeyCommands and txPluginArmCommands over what happened since the last), to be submitted in ONE submit, in order. `pageLive`: the page's controls
    // are on screen. `nowS`: a clock in seconds (the plugin lease's hold).
    std::vector<FoxCommand> tick(bool pageLive, double nowS);

    // An answer the session polled: a refused LATCH or consent is lettered in the key note, a latch that took clears it.
    void onAnswer(const FoxCommandResult& r);
    const std::string& keyNote() const { return key_note_; }
    // What the page believes it holds: for the tests and the bounded run's summary.
    bool pttHeld() const { return key_memory_.pttSent; }
    bool consentHeld() const { return key_memory_.armedByUs; }
    bool latchPending() const { return key_memory_.latchPending; }

private:
    Reading r_;
    std::string draft_;                    // the address as typed (seeded from the engine's whenever the box is not being typed in)
    bool draft_active_ = false;
    float peak_ = 0.0f;
    bool ptt_pointer_ = false, ptt_chord_ = false, latch_pressed_ = false;
    int arm_request_ = -1;
    int plugin_press_ = -2;
    std::string key_note_;
    cascade::gui::TxKeyMemory key_memory_;
    cascade::gui::TxPluginArmMemory plugin_memory_;

public:
    // The address box has the focus: the draft is the operator's, the engine's seed does not overwrite it.
    void addressFocus(bool focused) { draft_active_ = focused; }
};

}  // namespace foxsdr::slinttransmit
