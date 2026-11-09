// The TRANSMIT page's pure logic (src/slint/transmit_logic.hpp) against a STUB TABLE: what the page reads from the receiver state and the facts, what each answer does to it, the
// exact command each control sends - which are the ImGui window's own - and, above all, THE KEY: the interlocks the classic window enforces (a request is rebuilt every tick, a page
// that is not live holds nothing, every keying control is refused with no radio open) and the bounded-run filter that keeps every test and capture from putting RF out.
// No Slint, no engine, no radio: NOTHING IN THIS FILE KEYS A TRANSMITTER, and the commands it builds go nowhere.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "transmit_logic.hpp"
#include "test_check.hpp"

#include <cmath>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "dsp/modulator.hpp"

using namespace foxsdr::slinttransmit;

namespace {

class StubTable : public TableReader {
public:
    std::map<std::string, std::string> facts;
    std::int32_t readList(std::uint32_t, std::uint32_t, FoxListItem*) override { return 0; }
    bool fact(const std::string& key, std::string& out) override {
        const auto it = facts.find(key);
        if (it == facts.end()) { out.clear(); return false; }
        out = it->second;
        return true;
    }
};

void setText(char* dst, std::size_t cap, const std::string& text) { std::snprintf(dst, cap, "%s", text.c_str()); }

// A state with a radio open (the dummy a table-level test would use) or none.
FoxReceiverState state(bool radio) {
    FoxReceiverState s{};
    s.structSize = sizeof(s);
    s.tunedHz = 145.5e6;
    s.txFrequencyHz = 145.5e6;
    s.txPowerMinDb = -89.75;
    s.txPowerMaxDb = 0.0;
    s.txPowerDb = -89.75;
    s.txToneHz = 1000.0;
    if (radio) {
        s.flags |= FOXAPI_RX_TX_AVAILABLE;
        s.txFlags |= FOXAPI_TX_POWER_RANGE;
        setText(s.txDeviceName, sizeof(s.txDeviceName), "PlutoSDR (test)");
    }
    return s;
}

struct Rig {
    StubTable t;
    FoxReceiverState s = state(false);
    TransmitPage p;
    void read() { p.read(t, s); }
};

std::string show(const std::string& s) { return "\"" + s + "\""; }
std::string show(double d) { char b[48]; std::snprintf(b, sizeof(b), "%.9g", d); return b; }
std::string show(float d) { return show(static_cast<double>(d)); }
std::string show(int d) { return std::to_string(d); }
std::string show(unsigned d) { return std::to_string(d); }
std::string show(long long d) { return std::to_string(d); }
std::string show(std::size_t d) { return std::to_string(d); }
std::string show(bool b) { return b ? "true" : "false"; }
#define EQ(a, b)                                                                                              \
    do {                                                                                                      \
        ++g_checksRun;                                                                                        \
        if (!((a) == (b))) {                                                                                  \
            ++g_checksFailed;                                                                                 \
            std::printf("FAIL %s:%d  %s == %s\n      got: %s\n", __FILE__, __LINE__, #a, #b, show(a).c_str()); \
        }                                                                                                     \
    } while (0)

// What a batch of commands says, in the order sent: "PTT1 LATCH0 LATCH1 ARM-1".
std::string name(const FoxCommand& c) {
    switch (c.op) {
        case FOXAPI_OP_TX_PTT: return "PTT" + std::to_string(c.ival[0]);
        case FOXAPI_OP_TX_LATCH: return "LATCH" + std::to_string(c.ival[0]);
        case FOXAPI_OP_TX_REMOTE_ARM: return "CONSENT" + std::to_string(c.ival[0]);
        case FOXAPP_OP_TX_PLUGIN_ARM: return "ARM" + std::to_string(c.ival[0]) + (c.ival[1] != 0 ? "/" + std::to_string(c.ival[1]) : "");
        default: return "op" + std::to_string(c.op);
    }
}
std::string batch(const std::vector<FoxCommand>& v) {
    std::string s;
    for (const FoxCommand& c : v) { s += (s.empty() ? "" : " ") + name(c); }
    return s;
}

std::string slurp(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
std::string srcPath(const char* rel) {
    std::string p = __FILE__;
    for (int i = 0; i < 3; ++i) {
        const std::size_t cut = p.find_last_of("/\\");
        if (cut == std::string::npos) { break; }
        p.resize(cut);
    }
    return p + "/" + rel;
}
bool has(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }

FoxCommandResult answer(std::uint32_t op, std::int32_t status, const char* message) {
    FoxCommandResult r{};
    r.structSize = sizeof(r);
    r.op = op;
    r.status = status;
    setText(r.message, sizeof(r.message), message);
    return r;
}

}  // namespace

int main() {
    // ============================= the words and the tables =============================
    EQ(kModeCount, cascade::dsp::kTxModeCount);
    EQ(std::string(modeName(0)), std::string("CW"));
    EQ(std::string(modeName(2)), std::string("NFM"));
    EQ(std::string(modeName(4)), std::string("LSB"));
    for (int i = 0; i < kModeCount; ++i) {
        EQ(std::string(modeName(i)), std::string(cascade::dsp::txModeName(cascade::dsp::txModeFromIndex(i))));
        EQ(modeCaption(i), std::string(cascade::dsp::txModeCaption(cascade::dsp::txModeFromIndex(i))));   // English in force: tr() hands the key back
    }
    EQ(std::string(inputName(0)), std::string("MIC"));
    EQ(std::string(inputName(1)), std::string("TONE"));
    EQ(armLabel(5), std::string("5 MIN"));
    EQ(armLabel(15), std::string("15 MIN"));
    EQ(armLabel(60), std::string("60 MIN"));
    // the classic's own tables, read from its source: the input names, the lamp words, the arming choices
    {
        const std::string tx = slurp(srcPath("core/transmitter.cpp"));
        CHECK(has(tx, "FOX_TR_NOOP(\"MIC\")") && has(tx, "FOX_TR_NOOP(\"TONE\")"));
        const std::string win = slurp(srcPath("gui/app_window_transmit.cpp")) + slurp(srcPath("gui/app_window.cpp"));
        CHECK(has(win, "\"5 MIN##txarm5\"") && has(win, "\"15 MIN##txarm15\"") && has(win, "\"60 MIN##txarm60\""));
        CHECK(has(win, "tr(\"No transmitter is open.\")") && has(win, "tr(\"following the receiver\")") && has(win, "tr(\"(the board published no range)\")"));
        CHECK(has(win, "Not armed: no plugin can key this transmitter.") && has(win, "ARMED: %s may key this transmitter, %s left.") && has(win, "ON AIR for %s."));
        CHECK(has(win, "Type the board's address first."));
    }

    // ============================= the pure parts =============================
    double hz = 0.0;
    CHECK(parseMegahertz("145.5", hz) && std::fabs(hz - 145.5e6) < 1.0);
    CHECK(parseMegahertz("  0.000001 ", hz) && std::fabs(hz - 1.0) < 1e-6);
    CHECK(parseMegahertz("6000", hz) && std::fabs(hz - 6.0e9) < 1.0);
    CHECK(!parseMegahertz("6000.0001", hz));      // above 6 GHz: the table refuses it
    CHECK(!parseMegahertz("0", hz));
    CHECK(!parseMegahertz("-1", hz));
    CHECK(!parseMegahertz("", hz));
    CHECK(!parseMegahertz("   ", hz));
    CHECK(!parseMegahertz("abc", hz));
    CHECK(!parseMegahertz("145,5", hz));          // a comma is not a decimal point here
    CHECK(!parseMegahertz("145.5 MHz", hz));
    CHECK(!parseMegahertz("nan", hz));
    CHECK(!parseMegahertz("inf", hz));
    CHECK(parseHertz("1000", hz) && hz == 1000.0);
    CHECK(parseHertz(" 440.5 ", hz) && hz == 440.5);
    CHECK(!parseHertz("0", hz) && !parseHertz("-5", hz) && !parseHertz("", hz) && !parseHertz("x", hz) && !parseHertz("nan", hz) && !parseHertz("1e999", hz));
    EQ(cutAddress("uri=ip:192.168.2.1"), std::string("uri=ip:192.168.2.1"));
    EQ(cutAddress(std::string(300, 'a')).size(), kAddressBytes);
    {
        // never in the middle of a UTF-8 character: 254 ASCII bytes then a two-byte letter would end at 256
        const std::string s = std::string(254, 'a') + "\xC3\xA9" + "tail";
        const std::string c = cutAddress(s);
        EQ(c.size(), static_cast<std::size_t>(254));
    }
    CHECK(addressBlank("") && addressBlank("  \t ") && !addressBlank(" a "));

    // the chord: the bound one against a key event
    CHECK(chordMatches("Space", " ", false, false, false));
    CHECK(!chordMatches("Space", " ", true, false, false));      // modifiers must be exactly the chord's
    CHECK(!chordMatches("Space", " ", false, true, false));
    CHECK(!chordMatches("Space", "a", false, false, false));
    CHECK(chordMatches("Ctrl+T", "t", true, false, false) && chordMatches("Ctrl+T", "T", true, false, false));
    CHECK(!chordMatches("Ctrl+T", "t", false, false, false));
    CHECK(chordMatches("Ctrl+Shift+Alt+K", "k", true, true, true));
    CHECK(!chordMatches("None", " ", false, false, false) && !chordMatches("", " ", false, false, false));   // an unbound chord never matches
    CHECK(!chordMatches("Space", "", false, false, false));                                                   // the focus leaving sends an empty text: not a press
    CHECK(!chordMatches("Bogus+Space", " ", false, false, false));
    CHECK(chordMatches("Tab", "\t", false, false, false));

    // ============================= the reads and the model: NO RADIO =============================
    {
        Rig r;
        r.t.facts["fact.transmit.args"] = "uri=ip:192.168.2.1";
        r.read();
        const Model m = r.p.model();
        EQ(m.address, std::string("uri=ip:192.168.2.1"));
        CHECK(!m.have && m.address_enabled && m.open_enabled && !m.close_enabled);
        EQ(m.radio_line, std::string("No transmitter is open."));
        CHECK(m.radio_tone == Tone::Neutral);
        EQ(m.frequency, std::string("145.500000"));
        CHECK(!m.frequency_enabled && !m.split && !m.split_enabled);
        EQ(m.following_note, std::string("following the receiver"));
        CHECK(!m.modes_enabled && !m.power_enabled && !m.inputs_enabled && !m.tone_enabled && !m.key_enabled);
        EQ(m.power_text, std::string("QUIET"));
        EQ(m.power, 0.0f);
        EQ(m.power_note, std::string());              // no range note without a radio
        EQ(m.lamp_word, std::string("NO RADIO"));
        CHECK(m.lamp_tone == Tone::Neutral && !m.on_air && !m.latched);
        EQ(m.not_armed_line, std::string("Not armed: no plugin can key this transmitter."));
        EQ(m.armed_line, std::string());
        EQ(m.refusal_line, std::string());
        CHECK(m.data_flat);                            // on by default (the fact is absent)
        CHECK(has(m.licence, "This transmits."));
        EQ(m.open_tooltip, std::string("Connects to the board's iiod daemon and leaves it QUIET:\nmaximum attenuation, transmit oscillator down. Nothing goes\nout until the key below is held."));
        // a blank address: Open greyed, with the other tooltip
        r.t.facts["fact.transmit.args"] = "";
        r.read();
        const Model b = r.p.model();
        CHECK(!b.open_enabled);
        EQ(b.open_tooltip, std::string("Type the board's address first."));
        // the engine's last transmitter error is the radio line, red
        r.t.facts["fact.status.transmitError"] = "audio device busy - still opening";
        r.read();
        const Model e = r.p.model();
        EQ(e.radio_line, std::string("audio device busy - still opening"));
        CHECK(e.radio_tone == Tone::Err);
        // the plugins' block: a refusal, a reason
        r.t.facts["fact.tx.plugin.refusal"] = "1";
        r.t.facts["fact.tx.plugin.reason"] = "the arming window ran out";
        r.t.facts["fact.tx.plugin.dataFlat"] = "0";
        r.read();
        const Model a = r.p.model();
        EQ(a.refusal_line, std::string("Not armed: no transmitter is open. Open the radio above first."));
        EQ(a.reason_line, std::string("the arming window ran out"));
        CHECK(!a.data_flat);
        r.t.facts["fact.tx.plugin.refusal"] = "2";
        r.read();
        EQ(r.p.model().refusal_line, std::string("Not armed: no plugin has been granted TRANSMIT. Grant one on its page in Fitted modules first."));
        r.t.facts["fact.tx.plugin.refusal"] = "3";
        r.read();
        EQ(r.p.model().refusal_line, std::string("Not armed: that is not one of the offered times."));
        r.t.facts["fact.tx.plugin.refusal"] = "0";
        r.read();
        EQ(r.p.model().refusal_line, std::string());
    }

    // ============================= a RADIO OPEN =============================
    {
        Rig r;
        r.s = state(true);
        r.s.txFlags |= FOXAPI_TX_SPLIT;
        r.s.txFrequencyHz = 145.6e6;
        r.s.txMode = 2;
        r.s.txInput = 1;
        r.s.txPowerDb = -44.875;       // half way up the span
        r.s.txInputPeak = 0.5;
        r.read();
        Model m = r.p.model();
        CHECK(m.have && !m.address_enabled && !m.open_enabled && m.close_enabled);
        EQ(m.radio_line, std::string("PlutoSDR (test)"));
        EQ(m.frequency, std::string("145.600000"));
        CHECK(m.frequency_enabled && m.split && m.split_enabled);
        EQ(m.following_note, std::string());
        EQ(m.mode, 2);
        CHECK(m.modes_enabled && m.power_enabled && m.inputs_enabled && m.tone_shown && m.tone_enabled && m.key_enabled);
        EQ(m.tone_hz, std::string("1000"));
        EQ(m.power_text, std::string("-44.9 dB"));
        CHECK(std::fabs(m.power - 0.5f) < 1e-4f);
        EQ(m.power_note, std::string());
        EQ(m.lamp_word, std::string("READY"));
        CHECK(m.lamp_tone == Tone::Warn);
        CHECK(m.meter > 0.3f && m.meter < 1.0f);
        // the board published no range
        r.s.txFlags &= ~FOXAPI_TX_POWER_RANGE;
        r.read();
        EQ(r.p.model().power_note, std::string("(the board published no range)"));
        r.s.txFlags |= FOXAPI_TX_POWER_RANGE;
        // on air; faulted outranks on air
        r.s.flags |= FOXAPI_RX_TX_KEYED;
        r.read();
        m = r.p.model();
        EQ(m.lamp_word, std::string("ON AIR"));
        CHECK(m.lamp_tone == Tone::Err && m.on_air);
        EQ(m.close_enabled, false);                   // Close is refused while the key is down
        r.s.txFlags |= FOXAPI_TX_FAULTED;
        r.read();
        EQ(r.p.model().lamp_word, std::string("FAULT"));
        r.s.txFlags &= ~FOXAPI_TX_FAULTED;
        r.s.flags &= ~FOXAPI_RX_TX_KEYED;
        // the microphone opening greys the input keys
        r.s.txFlags |= FOXAPI_TX_INPUT_OPENING;
        r.read();
        CHECK(!r.p.model().inputs_enabled);
        r.s.txFlags &= ~FOXAPI_TX_INPUT_OPENING;
        // latch, consent, monitor, notes from the state
        r.s.flags |= FOXAPI_RX_TX_LATCHED | FOXAPI_RX_TX_KEY_MINE | FOXAPI_RX_TX_REMOTE_ARMED;
        r.s.txFlags |= FOXAPI_TX_MONITOR;
        setText(r.s.txUnkeyReason, sizeof(r.s.txUnkeyReason), "the latch timed out");
        setText(r.s.txError, sizeof(r.s.txError), "the radio would not key");
        r.read();
        m = r.p.model();
        CHECK(m.latched && m.remote_armed && m.monitor);
        EQ(m.unkey_reason, std::string("the latch timed out"));
        EQ(m.error_line, std::string("the radio would not key"));
        // an armed window
        r.t.facts["fact.tx.plugin.armed"] = "1";
        r.t.facts["fact.tx.plugin.minutes"] = "15";
        r.t.facts["fact.tx.plugin.secondsLeft"] = "272";
        r.t.facts["fact.tx.plugin.name"] = "Packet TNC";
        r.read();
        m = r.p.model();
        CHECK(m.armed && m.armed_minutes == 15);
        EQ(m.armed_line, std::string("ARMED: Packet TNC may key this transmitter, 4:32 left."));
        EQ(m.on_air_line, std::string());
        EQ(m.not_armed_line, std::string());
        r.t.facts["fact.tx.plugin.keyed"] = "0:07";
        r.read();
        m = r.p.model();
        CHECK(m.plugin_on_air);
        EQ(m.on_air_line, std::string("ON AIR for 0:07."));
        // a reason is shown only while not armed
        r.t.facts["fact.tx.plugin.reason"] = "you took the key";
        r.read();
        EQ(r.p.model().reason_line, std::string());
    }

    // the meter falls slowly
    {
        Rig r;
        r.s = state(true);
        r.s.txInputPeak = 1.0;
        r.read();
        const float top = r.p.model().meter;
        r.s.txInputPeak = 0.0;
        r.read();
        const float after = r.p.model().meter;
        CHECK(after < top && after > 0.0f);
        for (int i = 0; i < 400; ++i) { r.read(); }
        EQ(r.p.model().meter, 0.0f);
    }

    // ============================= the address box =============================
    {
        Rig r;
        r.t.facts["fact.transmit.args"] = "uri=ip:10.0.0.9";
        r.read();
        Command c = r.p.addressEdited("uri=ip:10.0.0.7");
        CHECK(c.valid && c.q.c.op == FOXAPP_OP_SET_TRANSMIT_ARGS && c.grant == FOXAPI_GRANT_TRANSMIT);
        EQ(std::string(c.q.c.text), std::string("uri=ip:10.0.0.7"));
        EQ(r.p.model().address, std::string("uri=ip:10.0.0.7"));
        // the engine's seed does not overwrite what is being typed while the box has the focus; it does once the focus has gone
        r.p.addressFocus(true);
        r.read();
        EQ(r.p.model().address, std::string("uri=ip:10.0.0.7"));
        r.p.addressFocus(false);
        r.read();
        EQ(r.p.model().address, std::string("uri=ip:10.0.0.9"));
        // Open: the draft, nothing when blank
        r.p.addressFocus(true);
        (void)r.p.addressEdited("uri=ip:10.0.0.7");
        c = r.p.open();
        CHECK(c.valid && c.q.c.op == FOXAPI_OP_TX_OPEN);
        EQ(std::string(c.q.c.text), std::string("uri=ip:10.0.0.7"));
        (void)r.p.addressEdited("   ");
        CHECK(!r.p.open().valid);
        // a long address rides in the long text, cut to the draft's room
        (void)r.p.addressEdited(std::string(300, 'x'));
        c = r.p.open();
        CHECK(c.valid);
        EQ((c.q.longText.empty() ? std::string(c.q.c.text) : c.q.longText).size(), kAddressBytes);
        // with a radio open the box is fixed and nothing is sent
        r.s = state(true);
        r.p.addressFocus(false);
        r.read();
        CHECK(!r.p.addressEdited("uri=ip:1.2.3.4").valid);
        CHECK(!r.p.open().valid);
    }

    // ============================= the controls' commands =============================
    {
        Rig r;      // NO RADIO: every one of them is nothing
        r.read();
        CHECK(!r.p.close().valid && !r.p.frequencyCommitted("145.5").valid && !r.p.frequencyStepped(1).valid && !r.p.splitPressed().valid && !r.p.modeSelected(2).valid &&
              !r.p.powerChanged(0.5f).valid && !r.p.inputSelected(1).valid && !r.p.toneCommitted("1000").valid && !r.p.monitorToggled(true).valid);
        // DATA (flat) is the engine's setting and works with no radio, as the classic's box does
        Command c = r.p.dataFlatToggled(false);
        CHECK(c.valid && c.q.c.op == FOXAPP_OP_TX_DATA_FLAT && c.q.c.ival[0] == 0 && c.grant == FOXAPI_GRANT_TRANSMIT);
        EQ(r.p.dataFlatToggled(true).q.c.ival[0], static_cast<long long>(1));
    }
    {
        Rig r;      // A RADIO OPEN, SPLIT OFF
        r.s = state(true);
        r.read();
        Command c = r.p.close();
        CHECK(c.valid && c.q.c.op == FOXAPI_OP_TX_CLOSE && c.grant == FOXAPI_GRANT_TRANSMIT);
        CHECK(!r.p.frequencyCommitted("145.5").valid && !r.p.frequencyStepped(1).valid);       // the box is greyed while the transmitter follows the receiver
        c = r.p.splitPressed();                                                                  // going INTO split starts from the receiver's dial
        CHECK(c.valid && c.q.c.op == FOXAPI_OP_TX_SET_SPLIT && c.q.c.ival[0] == 1 && c.q.c.num[0] == 145.5e6);
        c = r.p.modeSelected(3);
        CHECK(c.valid && c.q.c.op == FOXAPI_OP_TX_SET_MODE && c.q.c.ival[0] == 3);
        CHECK(!r.p.modeSelected(-1).valid && !r.p.modeSelected(5).valid);
        c = r.p.powerChanged(1.0f);
        CHECK(c.valid && c.q.c.op == FOXAPI_OP_TX_SET_POWER && c.q.c.num[0] == 0.0);            // right is louder; 0 dB is full output
        c = r.p.powerChanged(0.0f);
        CHECK(c.q.c.num[0] == -89.75);
        c = r.p.powerChanged(0.5f);
        CHECK(std::fabs(c.q.c.num[0] - (-44.875)) < 1e-9);
        CHECK(!r.p.powerChanged(std::nanf("")).valid);
        c = r.p.inputSelected(1);
        CHECK(c.valid && c.q.c.op == FOXAPI_OP_TX_SET_INPUT && c.q.c.ival[0] == 1);
        CHECK(!r.p.inputSelected(2).valid && !r.p.inputSelected(-1).valid);
        CHECK(!r.p.toneCommitted("1000").valid);                                                 // the tone box exists only on the TONE input
        c = r.p.monitorToggled(true);
        CHECK(c.valid && c.q.c.op == FOXAPI_OP_TX_SET_MONITOR && c.q.c.ival[0] == 1);
        // SPLIT on: the frequency
        r.s.txFlags |= FOXAPI_TX_SPLIT;
        r.s.txFrequencyHz = 145.6e6;
        r.read();
        c = r.p.frequencyCommitted("146.52");
        CHECK(c.valid && c.q.c.op == FOXAPI_OP_TX_SET_FREQUENCY && std::fabs(c.q.c.num[0] - 146.52e6) < 1.0);
        CHECK(!r.p.frequencyCommitted("not a number").valid && !r.p.frequencyCommitted("0").valid && !r.p.frequencyCommitted("7000").valid);
        c = r.p.frequencyStepped(1);
        CHECK(c.valid && std::fabs(c.q.c.num[0] - 145.601e6) < 1.0);
        c = r.p.frequencyStepped(-1);
        CHECK(c.valid && std::fabs(c.q.c.num[0] - 145.599e6) < 1.0);
        CHECK(!r.p.frequencyStepped(0).valid);
        c = r.p.splitPressed();                                                                  // coming OUT of split
        CHECK(c.valid && c.q.c.ival[0] == 0);
        // the TONE input: the tone box
        r.s.txInput = 1;
        r.read();
        c = r.p.toneCommitted("1500");
        CHECK(c.valid && c.q.c.op == FOXAPI_OP_TX_SET_TONE && c.q.c.num[0] == 1500.0);
        CHECK(!r.p.toneCommitted("loud").valid && !r.p.toneCommitted("-3").valid);
        // the microphone is still opening: the input keys are dead
        r.s.txFlags |= FOXAPI_TX_INPUT_OPENING;
        r.read();
        CHECK(!r.p.inputSelected(0).valid);
        // Close is refused while keyed
        r.s.flags |= FOXAPI_RX_TX_KEYED;
        r.read();
        CHECK(!r.p.close().valid);
    }

    // ============================= THE KEY =============================
    {
        // ---- no radio: nothing a press can do reaches the engine ----
        Rig r;
        r.read();
        r.p.pttPointer(true);
        r.p.pttChord(true);
        r.p.latchPressed();
        r.p.remoteArmToggled(true);
        EQ(batch(r.p.tick(true, 1.0)), std::string());
        EQ(batch(r.p.tick(true, 1.1)), std::string());
        CHECK(!r.p.pttHeld() && !r.p.consentHeld() && !r.p.latchPending());
    }
    {
        // ---- the PTT: re-asserted every tick it is held, let go once ----
        Rig r;
        r.s = state(true);
        r.read();
        EQ(batch(r.p.tick(true, 1.0)), std::string());
        r.p.pttPointer(true);
        EQ(batch(r.p.tick(true, 1.1)), std::string("PTT1"));
        EQ(batch(r.p.tick(true, 1.2)), std::string("PTT1"));
        EQ(batch(r.p.tick(true, 1.3)), std::string("PTT1"));
        CHECK(r.p.pttHeld());
        r.p.pttPointer(false);
        EQ(batch(r.p.tick(true, 1.4)), std::string("PTT0"));
        EQ(batch(r.p.tick(true, 1.5)), std::string());
        CHECK(!r.p.pttHeld());
        // the chord is the same hold
        r.p.pttChord(true);
        EQ(batch(r.p.tick(true, 1.6)), std::string("PTT1"));
        r.p.pttChord(false);
        EQ(batch(r.p.tick(true, 1.7)), std::string("PTT0"));
        // both sources down: still one hold; one let go: still held
        r.p.pttPointer(true);
        r.p.pttChord(true);
        EQ(batch(r.p.tick(true, 1.8)), std::string("PTT1"));
        r.p.pttPointer(false);
        EQ(batch(r.p.tick(true, 1.9)), std::string("PTT1"));
        r.p.pttChord(false);
        EQ(batch(r.p.tick(true, 2.0)), std::string("PTT0"));
    }
    {
        // ---- THE PAGE STOPS BEING LIVE with the key down: let go at once, and the level does not come back ----
        Rig r;
        r.s = state(true);
        r.read();
        r.p.pttPointer(true);
        EQ(batch(r.p.tick(true, 1.0)), std::string("PTT1"));
        EQ(batch(r.p.tick(false, 1.1)), std::string("PTT0"));         // the page closed / rolled up / not the view showing
        EQ(batch(r.p.tick(false, 1.2)), std::string());
        // the page opens again: the stale level was dropped with the page, nothing is held until a NEW press
        EQ(batch(r.p.tick(true, 1.3)), std::string());
        CHECK(!r.p.pttHeld());
        r.p.pttPointer(true);
        EQ(batch(r.p.tick(true, 1.4)), std::string("PTT1"));
    }
    {
        // ---- THE RADIO GOES AWAY with the key down ----
        Rig r;
        r.s = state(true);
        r.read();
        r.p.pttPointer(true);
        EQ(batch(r.p.tick(true, 1.0)), std::string("PTT1"));
        r.s = state(false);
        r.read();
        EQ(batch(r.p.tick(true, 1.1)), std::string("PTT0"));
        EQ(batch(r.p.tick(true, 1.2)), std::string());                 // the level went with the radio: not re-asserted when a radio comes back
        r.s = state(true);
        r.read();
        EQ(batch(r.p.tick(true, 1.3)), std::string());
    }
    {
        // ---- the LATCH: edge-triggered, "release then press" ----
        Rig r;
        r.s = state(true);
        r.read();
        r.p.latchPressed();
        EQ(batch(r.p.tick(true, 1.0)), std::string("LATCH0 LATCH1"));
        CHECK(r.p.latchPending());
        EQ(batch(r.p.tick(true, 1.1)), std::string());                 // an event, not a level: never re-sent
        // it landed: the state shows this window's latch
        r.s.flags |= FOXAPI_RX_TX_LATCHED | FOXAPI_RX_TX_KEY_MINE;
        r.read();
        EQ(batch(r.p.tick(true, 1.2)), std::string());
        CHECK(!r.p.latchPending());
        // a press on a key shown latched releases it
        r.p.latchPressed();
        EQ(batch(r.p.tick(true, 1.3)), std::string("LATCH0"));
        // the page goes away while this window's latch stands: LATCH 0
        EQ(batch(r.p.tick(false, 1.4)), std::string("LATCH0"));
        EQ(batch(r.p.tick(false, 1.5)), std::string());
    }
    {
        // ---- a latch pressed whose answer has not come back, and the page goes away ----
        Rig r;
        r.s = state(true);
        r.read();
        r.p.latchPressed();
        EQ(batch(r.p.tick(true, 1.0)), std::string("LATCH0 LATCH1"));
        EQ(batch(r.p.tick(false, 1.1)), std::string("LATCH0"));        // A PAGE CLOSE ENDS ANY PENDING PRESS
        // ... but ANOTHER window's latch is left alone
        Rig o;
        o.s = state(true);
        o.s.flags |= FOXAPI_RX_TX_LATCHED;                              // latched, not mine
        o.read();
        EQ(batch(o.p.tick(true, 1.0)), std::string());
        EQ(batch(o.p.tick(false, 1.1)), std::string());
    }
    {
        // ---- the consent to remote transmit ----
        Rig r;
        r.s = state(true);
        r.read();
        r.p.remoteArmToggled(true);
        EQ(batch(r.p.tick(true, 1.0)), std::string("CONSENT1"));
        CHECK(r.p.consentHeld());
        EQ(batch(r.p.tick(true, 1.1)), std::string());
        r.p.remoteArmToggled(false);
        EQ(batch(r.p.tick(true, 1.2)), std::string("CONSENT0"));
        CHECK(!r.p.consentHeld());
        // the page goes away with the consent given: withdrawn
        r.p.remoteArmToggled(true);
        EQ(batch(r.p.tick(true, 1.3)), std::string("CONSENT1"));
        EQ(batch(r.p.tick(false, 1.4)), std::string("CONSENT0"));
        EQ(batch(r.p.tick(false, 1.5)), std::string());
        // a click on a page that is not live is not remembered
        r.p.remoteArmToggled(true);
        EQ(batch(r.p.tick(false, 1.6)), std::string());
        EQ(batch(r.p.tick(true, 1.7)), std::string());
    }
    {
        // ---- everything at once: the page goes away holding PTT, latch and consent ----
        Rig r;
        r.s = state(true);
        r.read();
        r.p.pttPointer(true);
        r.p.latchPressed();
        r.p.remoteArmToggled(true);
        EQ(batch(r.p.tick(true, 1.0)), std::string("PTT1 LATCH0 LATCH1 CONSENT1"));
        EQ(batch(r.p.tick(false, 1.1)), std::string("PTT0 LATCH0 CONSENT0"));
        EQ(batch(r.p.tick(false, 1.2)), std::string());
    }

    // ============================= ARM FOR PLUGINS =============================
    {
        Rig r;                                   // no radio: the keys still work (the engine refuses, the fact says why)
        r.read();
        r.p.pluginArmPressed(15);
        EQ(batch(r.p.tick(true, 10.0)), std::string("ARM15"));
        // for three seconds after an ARM was sent the page says "still here" (-1) before the fact that it opened has come back
        EQ(batch(r.p.tick(true, 11.0)), std::string("ARM-1"));
        EQ(batch(r.p.tick(true, 12.9)), std::string("ARM-1"));
        EQ(batch(r.p.tick(true, 13.1)), std::string());                // refused (the fact never says armed): the beat stops
        r.p.pluginArmPressed(0);                                         // OFF
        EQ(batch(r.p.tick(true, 20.0)), std::string("ARM0"));
        r.p.pluginArmPressed(7);                                         // not one of the offered times: nothing is sent
        EQ(batch(r.p.tick(true, 21.0)), std::string());
        r.p.pluginArmPressed(-1);
        EQ(batch(r.p.tick(true, 22.0)), std::string());
        for (int m : {5, 15, 60}) {
            r.p.pluginArmPressed(m);
            EQ(batch(r.p.tick(true, 30.0 + m)), std::string("ARM") + std::to_string(m));
        }
    }
    {
        // ---- armed: the lease beat every tick the page is live; OFF with reason 1 the tick it is not ----
        Rig r;
        r.s = state(true);
        r.t.facts["fact.tx.plugin.armed"] = "1";
        r.t.facts["fact.tx.plugin.minutes"] = "5";
        r.read();
        EQ(batch(r.p.tick(true, 50.0)), std::string("ARM-1"));
        EQ(batch(r.p.tick(true, 50.033)), std::string("ARM-1"));
        EQ(batch(r.p.tick(true, 500.0)), std::string("ARM-1"));        // armed: as long as it stands
        EQ(batch(r.p.tick(false, 500.1)), std::string("ARM0/1"));      // OFF, "the TRANSMIT page was closed"
        // the fact still says armed until the engine closes it: the page keeps saying so
        EQ(batch(r.p.tick(false, 500.2)), std::string("ARM0/1"));
        r.t.facts["fact.tx.plugin.armed"] = "0";
        r.read();
        EQ(batch(r.p.tick(false, 500.3)), std::string());
        EQ(batch(r.p.tick(true, 500.4)), std::string());
        // pressing OFF while armed is a press: sent alone, not a lease beat
        r.t.facts["fact.tx.plugin.armed"] = "1";
        r.read();
        r.p.pluginArmPressed(0);
        EQ(batch(r.p.tick(true, 501.0)), std::string("ARM0"));
    }
    {
        // ---- the commands themselves: op, grant, args ----
        Rig r;
        r.s = state(true);
        r.read();
        r.p.pttPointer(true);
        r.p.latchPressed();
        r.p.remoteArmToggled(true);
        r.p.pluginArmPressed(5);
        const std::vector<FoxCommand> b = r.p.tick(true, 1.0);
        EQ(b.size(), static_cast<std::size_t>(5));
        for (const FoxCommand& c : b) { EQ(c.structSize, static_cast<unsigned>(sizeof(FoxCommand))); }
        EQ(b[0].op, static_cast<unsigned>(FOXAPI_OP_TX_PTT));
        EQ(b[1].op, static_cast<unsigned>(FOXAPI_OP_TX_LATCH));
        EQ(b[3].op, static_cast<unsigned>(FOXAPI_OP_TX_REMOTE_ARM));
        EQ(b[4].op, static_cast<unsigned>(FOXAPP_OP_TX_PLUGIN_ARM));
        EQ(b[4].ival[0], static_cast<long long>(5));
    }

    // ============================= the answers =============================
    {
        Rig r;
        r.s = state(true);
        r.read();
        r.p.onAnswer(answer(FOXAPI_OP_TX_LATCH, FOXAPI_BUSY, "another interface holds the key"));
        EQ(r.p.keyNote(), std::string("another interface holds the key"));
        EQ(r.p.model().key_note, std::string("another interface holds the key"));
        r.p.onAnswer(answer(FOXAPI_OP_TX_LATCH, FOXAPI_NO_CHANGE, "already latched"));        // a NO_CHANGE neither sets nor clears
        EQ(r.p.keyNote(), std::string("another interface holds the key"));
        r.p.onAnswer(answer(FOXAPI_OP_TX_PTT, FOXAPI_NO_DEVICE, "there is no transmitter open"));   // a refused PTT is not lettered (the classic does not)
        EQ(r.p.keyNote(), std::string("another interface holds the key"));
        r.p.onAnswer(answer(FOXAPI_OP_TX_REMOTE_ARM, FOXAPI_DENIED, "this session does not hold the grant for that"));
        EQ(r.p.keyNote(), std::string("this session does not hold the grant for that"));
        r.p.onAnswer(answer(FOXAPI_OP_TX_LATCH, FOXAPI_OK, ""));                                    // the LATCH 0 of a press clears an old note
        EQ(r.p.keyNote(), std::string());
        r.p.onAnswer(answer(FOXAPI_OP_TX_LATCH, FOXAPI_DENIED, "the latch ended (the latch timed out): this interface must send LATCH 0 first"));
        CHECK(!r.p.keyNote().empty());
        // the page stops being live: the note goes
        r.p.pttPointer(true);
        (void)r.p.tick(true, 1.0);
        (void)r.p.tick(false, 1.1);
        EQ(r.p.keyNote(), std::string());
        // a consent refused (DENIED) is lettered too
        r.p.onAnswer(answer(FOXAPI_OP_TX_REMOTE_ARM, FOXAPI_FAILED, "the consent was not taken"));
        EQ(r.p.keyNote(), std::string("the consent was not taken"));
    }

    // ============================= THE BOUNDED RUN'S FILTER =============================
    {
        const auto cmdOf = [](std::uint32_t op, long long v0 = 0, long long v1 = 0) {
            FoxCommand c{};
            c.structSize = sizeof(c);
            c.op = op;
            c.ival[0] = v0;
            c.ival[1] = v1;
            return c;
        };
        for (const bool radio : {false, true}) {
            CHECK(!boundedMaySend(cmdOf(FOXAPI_OP_TX_OPEN), radio));                 // never opens a board
            CHECK(!boundedMaySend(cmdOf(FOXAPI_OP_TX_SET_INPUT, 0), radio));         // never opens a microphone
            CHECK(!boundedMaySend(cmdOf(FOXAPI_OP_TX_PTT, 1), radio));               // never keys
            CHECK(!boundedMaySend(cmdOf(FOXAPI_OP_TX_LATCH, 1), radio));
            CHECK(!boundedMaySend(cmdOf(FOXAPI_OP_TX_REMOTE_ARM, 1), radio));
            CHECK(boundedMaySend(cmdOf(FOXAPI_OP_TX_PTT, 0), radio));                // letting go always goes
            CHECK(boundedMaySend(cmdOf(FOXAPI_OP_TX_LATCH, 0), radio));
            CHECK(boundedMaySend(cmdOf(FOXAPI_OP_TX_REMOTE_ARM, 0), radio));
            CHECK(boundedMaySend(cmdOf(FOXAPI_OP_TX_CLOSE), radio));
            CHECK(boundedMaySend(cmdOf(FOXAPP_OP_TX_PLUGIN_ARM, 0, 1), radio));      // OFF
            CHECK(boundedMaySend(cmdOf(FOXAPP_OP_TX_PLUGIN_ARM, -1), radio));        // the lease beat
            CHECK(boundedMaySend(cmdOf(FOXAPP_OP_SET_TRANSMIT_PAGE_OPEN, 1), radio));
            CHECK(boundedMaySend(cmdOf(FOXAPP_OP_TX_DATA_FLAT, 0), radio));
        }
        CHECK(boundedMaySend(cmdOf(FOXAPP_OP_TX_PLUGIN_ARM, 15), false));            // no radio: the engine refuses, and the refusal is the evidence
        CHECK(!boundedMaySend(cmdOf(FOXAPP_OP_TX_PLUGIN_ARM, 15), true));            // a radio open: an ARM could be honoured
        CHECK(!boundedMaySend(cmdOf(FOXAPP_OP_TX_PLUGIN_ARM, 5), true) && !boundedMaySend(cmdOf(FOXAPP_OP_TX_PLUGIN_ARM, 60), true));
    }

    return testSummary("test_slint_host_transmit");
}
