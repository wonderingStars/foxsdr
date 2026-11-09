// transmit_logic.cpp - see transmit_logic.hpp. The sentences are the ImGui window's own (app_window.cpp drawTransmitPage, app_window_transmit.cpp, gui/transmit_page.hpp), through
// cascade::i18n::tr() so the catalogues already hold them.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "transmit_logic.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "core/i18n.hpp"
#include "core/utf8_text.hpp"
#include "dsp/modulator.hpp"

namespace foxsdr::slinttransmit {

using cascade::i18n::tr;
namespace cmd = cascade::core::cmd;

namespace {

Command make(const FoxCommand& f) {
    Command c;
    c.q.c = f;
    c.grant = foxsdr::slintsignal::grantFor(f.op);
    c.valid = true;
    return c;
}
Command make(const cmd::QueuedCommand& q) {
    Command c;
    c.q = q;
    c.grant = foxsdr::slintsignal::grantFor(q.c.op);
    c.valid = true;
    return c;
}

int factInt(const std::string& text) { return text.empty() ? 0 : std::atoi(text.c_str()); }

std::string factOf(TableReader& t, const char* key) {
    std::string v;
    if (!t.fact(key, v)) { v.clear(); }
    return v;
}

}  // namespace

// ---- the words ----

const char* modeName(int mode) { return cascade::dsp::txModeName(cascade::dsp::txModeFromIndex(mode)); }
std::string modeCaption(int mode) { return tr(cascade::dsp::txModeCaption(cascade::dsp::txModeFromIndex(mode))); }
const char* inputName(int input) { return input == 1 ? "TONE" : "MIC"; }
std::string inputLabel(int input) { return tr(inputName(input)); }
std::string armLabel(int minutes) { return minutes == 5 ? tr("5 MIN") : minutes == 15 ? tr("15 MIN") : tr("60 MIN"); }

// ---- the pure parts ----

bool parseMegahertz(const std::string& text, double& hz) {
    const char* s = text.c_str();
    while (*s == ' ' || *s == '\t') { ++s; }
    if (*s == '\0') { return false; }
    char* end = nullptr;
    const double mhz = std::strtod(s, &end);
    if (end == s) { return false; }
    while (*end == ' ' || *end == '\t') { ++end; }
    if (*end != '\0') { return false; }
    const double v = mhz * 1.0e6;
    if (!std::isfinite(v) || !(v > kMinFrequencyHz) || v > kMaxFrequencyHz) { return false; }
    hz = v;
    return true;
}

bool parseHertz(const std::string& text, double& hz) {
    const char* s = text.c_str();
    while (*s == ' ' || *s == '\t') { ++s; }
    if (*s == '\0') { return false; }
    char* end = nullptr;
    const double v = std::strtod(s, &end);
    if (end == s) { return false; }
    while (*end == ' ' || *end == '\t') { ++end; }
    if (*end != '\0') { return false; }
    if (!std::isfinite(v) || !(v > 0.0)) { return false; }
    hz = v;
    return true;
}

std::string cutAddress(const std::string& text) {
    if (text.size() <= kAddressBytes) { return text; }
    std::size_t n = kAddressBytes;
    while (n > 0 && (static_cast<unsigned char>(text[n]) & 0xC0u) == 0x80u) { --n; }   // never in the middle of a UTF-8 character
    return text.substr(0, n);
}

bool addressBlank(const std::string& text) { return text.find_first_not_of(" \t") == std::string::npos; }

bool chordMatches(const std::string& chord, const std::string& keyText, bool ctrl, bool shift, bool alt) {
    if (chord.empty() || chord == "None" || keyText.empty()) { return false; }
    bool wantCtrl = false, wantShift = false, wantAlt = false;
    std::string key;
    std::size_t start = 0;
    for (;;) {
        const std::size_t plus = chord.find('+', start);
        // A chord whose key is "Plus" is spelt out; a bare '+' cannot be told from a separator, so it never matches (the key table spells it "Plus").
        const std::string part = chord.substr(start, plus == std::string::npos ? std::string::npos : plus - start);
        if (plus == std::string::npos) { key = part; break; }
        if (part == "Ctrl") { wantCtrl = true; }
        else if (part == "Shift") { wantShift = true; }
        else if (part == "Alt") { wantAlt = true; }
        else { return false; }
        start = plus + 1;
    }
    if (wantCtrl != ctrl || wantShift != shift || wantAlt != alt) { return false; }
    std::string got;
    if (keyText == " ") { got = "Space"; }
    else if (keyText == "\t") { got = "Tab"; }
    else if (keyText == "\n" || keyText == "\r") { got = "Enter"; }
    else if (keyText.size() == 1 && std::isalnum(static_cast<unsigned char>(keyText[0]))) { got = std::string(1, static_cast<char>(std::toupper(static_cast<unsigned char>(keyText[0])))); }
    else { return false; }   // an arrow, a function key or the like: Slint's private-use texts are not matched here (the PTT chord is a plain key)
    return got == key;
}

// ---- the reads ----

Reading readTransmit(TableReader& t, const FoxReceiverState& st) {
    Reading r;
    r.flags = st.flags;
    r.tx_flags = st.txFlags;
    r.mode = static_cast<int>(st.txMode);
    r.input = static_cast<int>(st.txInput);
    r.power_db = st.txPowerDb;
    r.power_min_db = st.txPowerMinDb;
    r.power_max_db = st.txPowerMaxDb;
    r.frequency_hz = st.txFrequencyHz;
    r.split_hz = st.txSplitHz;
    r.tone_hz = st.txToneHz;
    r.input_peak = st.txInputPeak;
    r.tuned_hz = st.tunedHz;
    r.device_name = st.txDeviceName;
    r.tx_error = st.txError;
    r.unkey_reason = st.txUnkeyReason;
    r.have = (st.flags & FOXAPI_RX_TX_AVAILABLE) != 0u;
    r.on_air = (st.flags & FOXAPI_RX_TX_KEYED) != 0u;
    r.latched = (st.flags & FOXAPI_RX_TX_LATCHED) != 0u;
    r.key_mine = (st.flags & FOXAPI_RX_TX_KEY_MINE) != 0u;
    r.remote_armed = (st.flags & FOXAPI_RX_TX_REMOTE_ARMED) != 0u;
    r.faulted = r.have && (st.txFlags & FOXAPI_TX_FAULTED) != 0u;
    r.split = (st.txFlags & FOXAPI_TX_SPLIT) != 0u;
    r.monitor = (st.txFlags & FOXAPI_TX_MONITOR) != 0u;
    r.mic_opening = (st.txFlags & FOXAPI_TX_INPUT_OPENING) != 0u;
    r.has_range = r.have && (st.txFlags & FOXAPI_TX_POWER_RANGE) != 0u;
    r.args = factOf(t, "fact.transmit.args");
    r.transmit_error = factOf(t, "fact.status.transmitError");
    PluginReading& p = r.plugin;
    p.armed = factOf(t, "fact.tx.plugin.armed") == "1";
    p.minutes = p.armed ? factInt(factOf(t, "fact.tx.plugin.minutes")) : 0;
    p.seconds_left = factInt(factOf(t, "fact.tx.plugin.secondsLeft"));
    p.name = factOf(t, "fact.tx.plugin.name");
    p.keyed = factOf(t, "fact.tx.plugin.keyed");
    p.refusal = factInt(factOf(t, "fact.tx.plugin.refusal"));
    p.reason = factOf(t, "fact.tx.plugin.reason");
    p.data_flat = factOf(t, "fact.tx.plugin.dataFlat") != "0";
    return r;
}

void TransmitPage::read(TableReader& t, const FoxReceiverState& st) {
    r_ = readTransmit(t, st);
    // THE ADDRESS DRAFT is re-seeded from the engine's address whenever the box is not being typed in (the classic's rule: the page opens on a real address, never blank).
    if (!draft_active_) { draft_ = r_.args; }
    // DECAYED, NOT REPLACED: a fast rise and a slow fall.
    const float peak = static_cast<float>(std::isfinite(r_.input_peak) ? std::clamp(r_.input_peak, 0.0, 1.0) : 0.0);
    peak_ = std::max(peak, peak_ * kMeterDecay);
    // A radio that went away takes every level the page was holding with it (a press cannot outlive the radio it was for).
    if (!r_.have) {
        ptt_pointer_ = false;
        ptt_chord_ = false;
        latch_pressed_ = false;
        arm_request_ = -1;
    }
}

Model TransmitPage::model() const {
    Model m;
    const Reading& r = r_;
    m.address = draft_;
    m.have = r.have;
    m.address_enabled = !r.have;
    m.open_enabled = !r.have && !addressBlank(draft_);
    m.close_enabled = r.have && !r.on_air;
    m.open_tooltip = addressBlank(draft_) ? tr("Type the board's address first.")
                                          : tr("Connects to the board's iiod daemon and leaves it QUIET:\n"
                                               "maximum attenuation, transmit oscillator down. Nothing goes\n"
                                               "out until the key below is held.");
    if (r.have) {
        m.radio_line = r.device_name;
    } else if (!r.transmit_error.empty()) {
        m.radio_line = tr(r.transmit_error.c_str());
        m.radio_tone = Tone::Err;
    } else {
        m.radio_line = tr("No transmitter is open.");
    }
    // the frequency: where it would transmit
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.6f", r.frequency_hz / 1.0e6);
    m.frequency = buf;
    m.split = r.split;
    m.frequency_enabled = r.have && r.split;
    m.split_enabled = r.have;
    if (!r.split) { m.following_note = tr("following the receiver"); }
    m.mode = r.mode;
    m.modes_enabled = r.have;
    // the power
    m.power = cascade::gui::txPowerFraction(r.power_db, r.power_min_db, r.power_max_db);
    char power[32];
    cascade::gui::formatTxPower(power, sizeof(power), r.power_db, r.power_min_db);
    m.power_text = power;
    m.power_enabled = r.have;
    if (r.have && !r.has_range) { m.power_note = tr("(the board published no range)"); }
    // the input
    m.input = r.input;
    m.inputs_enabled = r.have && !r.mic_opening;
    m.tone_shown = r.input == 1;
    std::snprintf(buf, sizeof(buf), "%.0f", r.tone_hz);
    m.tone_hz = buf;
    m.tone_enabled = r.have;
    m.meter = cascade::gui::txMeterFraction(peak_);
    // the key
    const cascade::gui::TxLamp lamp = cascade::gui::txLampState(r.have, r.faulted, r.on_air);
    m.lamp_word = tr(cascade::gui::txLampText(lamp));
    switch (lamp) {
        case cascade::gui::TxLamp::NoRadio: m.lamp_tone = Tone::Neutral; break;
        case cascade::gui::TxLamp::Ready: m.lamp_tone = Tone::Warn; break;
        case cascade::gui::TxLamp::Transmitting: m.lamp_tone = Tone::Err; break;
        case cascade::gui::TxLamp::Fault: m.lamp_tone = Tone::Err; break;
    }
    m.on_air = r.on_air;
    m.latched = r.latched;
    m.key_enabled = r.have;
    m.remote_armed = r.remote_armed;
    m.monitor = r.monitor;
    m.key_note = key_note_;
    m.unkey_reason = r.unkey_reason;
    m.error_line = r.tx_error;
    // ARM FOR PLUGINS
    const PluginReading& p = r.plugin;
    m.armed = p.armed;
    m.armed_minutes = p.armed ? p.minutes : 0;
    if (p.armed) {
        char left[16];
        cascade::gui::formatTxArmCountdown(left, sizeof(left), p.seconds_left);
        cascade::core::formatUtf8(m.armed_line, tr("ARMED: %s may key this transmitter, %s left."), p.name.c_str(), left);
        if (!p.keyed.empty()) {
            m.plugin_on_air = true;
            cascade::core::formatUtf8(m.on_air_line, tr("ON AIR for %s."), p.keyed.c_str());
        }
    } else {
        m.not_armed_line = tr("Not armed: no plugin can key this transmitter.");
        if (!p.reason.empty()) { m.reason_line = p.reason; }
    }
    if (p.refusal != 0) { m.refusal_line = tr(cascade::gui::txArmRefusalText(static_cast<cascade::core::TxArming::Refusal>(p.refusal))); }
    m.data_flat = p.data_flat;
    m.licence = tr(cascade::gui::kTxLicenceNotice);
    return m;
}

// ---- the gestures ----

Command TransmitPage::addressEdited(const std::string& text) {
    if (r_.have) { return {}; }
    draft_ = cutAddress(text);
    return make(cmd::makeText(FOXAPP_OP_SET_TRANSMIT_ARGS, draft_));
}

Command TransmitPage::open() const {
    if (r_.have || addressBlank(draft_)) { return {}; }
    return make(cmd::makeText(FOXAPI_OP_TX_OPEN, draft_));
}

Command TransmitPage::close() const {
    if (!r_.have || r_.on_air) { return {}; }
    return make(cmd::make(FOXAPI_OP_TX_CLOSE));
}

Command TransmitPage::frequencyCommitted(const std::string& text) const {
    double hz = 0.0;
    if (!r_.have || !r_.split || !parseMegahertz(text, hz)) { return {}; }
    return make(cmd::makeNum(FOXAPI_OP_TX_SET_FREQUENCY, hz));
}

Command TransmitPage::frequencyStepped(int direction) const {
    if (!r_.have || !r_.split || direction == 0) { return {}; }
    const double hz = r_.frequency_hz + (direction > 0 ? 1000.0 : -1000.0);
    if (!std::isfinite(hz) || !(hz > kMinFrequencyHz) || hz > kMaxFrequencyHz) { return {}; }
    return make(cmd::makeNum(FOXAPI_OP_TX_SET_FREQUENCY, hz));
}

Command TransmitPage::splitPressed() const {
    if (!r_.have) { return {}; }
    // Coming OUT of split puts the transmitter back on the receiver; going INTO it starts from wherever the receiver is, so the first thing that happens is never a jump.
    return make(cmd::makeNumInt(FOXAPI_OP_TX_SET_SPLIT, std::max(0.0, r_.tuned_hz), r_.split ? 0 : 1));
}

Command TransmitPage::modeSelected(int mode) const {
    if (!r_.have || mode < 0 || mode >= kModeCount) { return {}; }
    return make(cmd::makeInt(FOXAPI_OP_TX_SET_MODE, mode));
}

Command TransmitPage::powerChanged(float fraction) const {
    if (!r_.have || !std::isfinite(fraction)) { return {}; }
    return make(cmd::makeNum(FOXAPI_OP_TX_SET_POWER, cascade::gui::txPowerFromFraction(fraction, r_.power_min_db, r_.power_max_db)));
}

Command TransmitPage::inputSelected(int input) const {
    if (!r_.have || r_.mic_opening || input < 0 || input >= kInputCount) { return {}; }
    return make(cmd::makeInt(FOXAPI_OP_TX_SET_INPUT, input));
}

Command TransmitPage::toneCommitted(const std::string& text) const {
    double hz = 0.0;
    if (!r_.have || r_.input != 1 || !parseHertz(text, hz)) { return {}; }
    return make(cmd::makeNum(FOXAPI_OP_TX_SET_TONE, hz));
}

Command TransmitPage::monitorToggled(bool on) const {
    if (!r_.have) { return {}; }
    return make(cmd::makeInt(FOXAPI_OP_TX_SET_MONITOR, on ? 1 : 0));
}

Command TransmitPage::dataFlatToggled(bool on) const { return make(cmd::makeInt(FOXAPP_OP_TX_DATA_FLAT, on ? 1 : 0)); }

void TransmitPage::pluginArmPressed(int minutes) {
    if (minutes != 0 && minutes != 5 && minutes != 15 && minutes != 60) { return; }
    plugin_press_ = minutes;
}

// ---- the key ----

std::vector<FoxCommand> TransmitPage::tick(bool pageLive, double nowS) {
    std::vector<FoxCommand> out;
    // A PAGE THAT IS NOT LIVE HOLDS NOTHING. Its controls are gone (the page closed, rolled up or not the view showing) and a release the controls never reported - a key that was
    // destroyed while down - must not be remembered into the next time the page opens: every level and every pending press goes with the page.
    if (!pageLive) {
        ptt_pointer_ = false;
        ptt_chord_ = false;
        latch_pressed_ = false;
        arm_request_ = -1;
    }
    cascade::gui::TxKeyFrame f;
    f.pageLive = pageLive;
    f.pttHeld = pageLive && r_.have && (ptt_pointer_ || ptt_chord_);
    f.latchPressed = pageLive && r_.have && latch_pressed_;
    f.armRequest = (pageLive && r_.have) ? arm_request_ : -1;
    f.latchedShown = r_.latched;
    f.latchMine = r_.latched && r_.key_mine;
    if (!f.pageLive && key_memory_.pageWasLive) { key_note_.clear(); }
    FoxCommand batch[cascade::gui::kTxKeyMaxCommands];
    const std::uint32_t n = cascade::gui::txKeyCommands(f, key_memory_, batch);
    for (std::uint32_t i = 0; i < n; ++i) { out.push_back(batch[i]); }

    cascade::gui::TxPluginArmFrame a;
    a.pageLive = pageLive;
    a.pressed = plugin_press_;
    a.armedShown = r_.plugin.armed;
    a.nowS = nowS;
    FoxCommand arm[cascade::gui::kTxPluginArmMaxCommands];
    const std::uint32_t na = cascade::gui::txPluginArmCommands(a, plugin_memory_, arm);
    for (std::uint32_t i = 0; i < na; ++i) { out.push_back(arm[i]); }

    // the presses are events of this tick
    latch_pressed_ = false;
    arm_request_ = -1;
    plugin_press_ = -2;
    return out;
}

void TransmitPage::onAnswer(const FoxCommandResult& r) {
    const bool keyOp = r.op == FOXAPI_OP_TX_LATCH || r.op == FOXAPI_OP_TX_REMOTE_ARM;
    if (keyOp && r.status != FOXAPI_OK && r.status != FOXAPI_NO_CHANGE) {
        key_note_ = r.message;
    } else if (keyOp && r.status == FOXAPI_OK && r.op == FOXAPI_OP_TX_LATCH) {
        key_note_.clear();
    }
}

bool boundedMaySend(const FoxCommand& c, bool haveRadio) {
    switch (c.op) {
        // OPENS A CONNECTION to a board at the typed address, or a microphone: never in a bounded run.
        case FOXAPI_OP_TX_OPEN:
        case FOXAPI_OP_TX_SET_INPUT:
            return false;
        // THE KEYING ASKS: never in a bounded run, whatever the page believes (the page cannot make them without a radio, and a bounded run has none).
        case FOXAPI_OP_TX_PTT:
        case FOXAPI_OP_TX_LATCH:
        case FOXAPI_OP_TX_REMOTE_ARM:
            return c.ival[0] == 0;
        // The arming window: a refusal with no radio is the point of the press; with a radio open an ARM could be honoured, so it is withheld.
        case FOXAPP_OP_TX_PLUGIN_ARM:
            return c.ival[0] <= 0 || !haveRadio;
        default:
            return true;
    }
}

}  // namespace foxsdr::slinttransmit
