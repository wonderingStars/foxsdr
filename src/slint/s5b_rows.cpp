// s5b_rows.cpp - SCREEN LANE S5b's ROWS (wave 8; docs/multi-radio-records/screen-s5b.md): the rows lane S5b adds to the Source section's panel, members of S5Rows (s5_rows.hpp) so they share its facts, drafts and sender.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdlib>

#include "core/app_commands.hpp"
#include "core/i18n.hpp"
#include "lanel_logic.hpp"
#include "s5_rows.hpp"
#include "s5b_logic.hpp"

namespace foxsdr::slints5 {

namespace {
namespace slk = foxsdr::slintlanek;
namespace sll = foxsdr::slintlanel;
using slk::Row;
using slk::rHeading;
using slk::rNote;
using slk::rReadout;

// the screen's words: every literal below goes through the string tables (tools/i18n_keys.py finds the tr("...") calls)
std::string tr(const char* s) { return cascade::i18n::tr(s); }
}  // namespace

void S5Rows::buildS5b(std::vector<Row>& v) {
    // ---------------------------------------------------------------- G1-14 analogue IF bandwidth
    {
        const std::string cur = fact("radio." + rn() + ".ifBandwidth");
        v.push_back(rHeading(tr("Analogue IF bandwidth")));
        const std::string words = foxsdr::slints5b::ifBandwidthWords(cur);
        if (words.empty()) {
            v.push_back(rNote(tr("This radio has no adjustable analogue filter. Only an RTL-SDR opened by FoxSDR's own driver has one.")));
        } else {
            v.push_back(rNote(tr("The RTL-SDR tuner's own filter in front of its converter. Automatic follows the sample rate; a narrower filter keeps a strong neighbour out, a wider one lets more of the band through.")));
            v.push_back(rReadout(tr("Now"), words));
            field(v, "b.ifbw", tr("Bandwidth (kHz)"), "", tr("300 to 8000; empty or 0 = automatic"));
            key(v, "b.ifbw.set", tr("Set the analogue bandwidth"), [this] {
                long long hz = 0; std::string e;
                if (!foxsdr::slints5b::parseIfBandwidthKHz(dv("b.ifbw"), hz, e)) { refuseHere(tr("Set the analogue bandwidth"), e); return; }
                Cmd c; c.op = FOXAPI_OP_SET_DEVICE_OPTION; c.i0 = hz; c.text = "if_bandwidth"; send(tr("Set the analogue bandwidth"), c);
            }, true, tr("Read back from the radio after each change."));
            key(v, "b.ifbw.auto", tr("Automatic (from the sample rate)"), [this] {
                draft_["b.ifbw"] = std::string();
                Cmd c; c.op = FOXAPI_OP_SET_DEVICE_OPTION; c.i0 = 0; c.text = "if_bandwidth"; send(tr("Automatic (from the sample rate)"), c);
            }, cur != "0");
        }
    }
    buildSoapy(v);
}


// ---------------------------------------------------------------- G1-15a / G1-15b SoapySDR driver bandwidth and settings
// The list is whatever the connected driver reported (the engine's radio.N.soapy.* facts): no setting is drawn that the driver did not name, and a value the driver refuses comes back as the driver's own sentence.
void S5Rows::buildSoapy(std::vector<Row>& v) {
    namespace b = foxsdr::slints5b;
    const auto factFn = [this](const std::string& n) { return fact(n); };
    const int radio = sel();
    double cur = 0.0, lo = 0.0, hi = 0.0;
    const bool hasBw = b::readSoapyBandwidth(factFn, radio, cur, lo, hi);
    const std::vector<b::SoapySettingView> settings = b::readSoapySettings(factFn, radio);
    v.push_back(rHeading(tr("Driver bandwidth and settings")));
    if (!hasBw && settings.empty()) {
        v.push_back(rNote(tr("This radio's driver names no bandwidth control and no settings of its own. Only a radio opened through a SoapySDR driver can have them.")));
        return;
    }
    if (hasBw) {
        v.push_back(rNote(tr("The filter bandwidth the radio's driver offers, within the range the driver reports.")));
        v.push_back(rReadout(tr("Bandwidth now"), b::soapyBandwidthWords(cur)));
        field(v, "b.sp.bw", tr("Bandwidth (kHz)"), "", tr("Within the driver's range:") + " " + b::soapyBandwidthWords(lo) + " - " + b::soapyBandwidthWords(hi));
        key(v, "b.sp.bw.set", tr("Set the bandwidth"), [this, lo, hi] {
            double hz = 0.0; std::string e;
            if (!foxsdr::slints5b::parseSoapyBandwidthKHz(dv("b.sp.bw"), lo, hi, hz, e)) { refuseHere(tr("Set the bandwidth"), e); return; }
            Cmd c; c.op = FOXAPI_OP_SET_DEVICE_OPTION; c.n0 = hz; c.text = "soapy_bandwidth"; send(tr("Set the bandwidth"), c, kSoapy);
        }, true, tr("Read back from the driver after each change."));
    }
    if (settings.empty()) { return; }
    v.push_back(rNote(tr("The settings below are the ones the connected driver lists. Each is read back from the driver after a change; a value the driver refuses is shown with its reason.")));
    for (std::size_t i = 0; i < settings.size(); ++i) {
        const b::SoapySettingView s = settings[i];
        const std::string id = "b.sp." + std::to_string(i);
        const std::string label = b::soapySettingLabel(s);
        const std::string tip = s.description.empty() ? b::soapyRangeWords(s) : (s.description + (s.hasRange ? " (" + b::soapyRangeWords(s) + ")" : std::string()));
        const auto write = [this, s, label](const std::string& typed) {
            std::string val, cmd, e;
            if (!foxsdr::slints5b::soapyValueText(s, typed, val, e) || !foxsdr::slints5b::soapyWriteCommand(s.key, val, cmd, e)) { refuseHere(label, e); return; }
            Cmd c; c.op = FOXAPI_OP_SET_DEVICE_OPTION; c.text = cmd; send(label, c, kSoapy);
        };
        if (!s.options.empty()) {
            int pick = 0;
            bool known = false;
            for (std::size_t k = 0; k < s.options.size(); ++k) { if (s.options[k] == s.value) { pick = static_cast<int>(k); known = true; } }
            if (!known) { v.push_back(rReadout(label, s.value)); }
            const std::vector<std::string> opts = s.options;
            choice(v, id, label, s.optionNames, pick, [opts, write](int k) { if (k >= 0 && k < static_cast<int>(opts.size())) { write(opts[static_cast<std::size_t>(k)]); } }, tip);
        } else if (s.type == "bool") {
            tick(v, id, label, s.value == "true" || s.value == "1", [write](bool on) { write(on ? "true" : "false"); }, tip);
        } else {
            field(v, id, label, s.value, tip);
            key(v, id + ".set", tr("Set") + " " + label, [this, id, write, s] { write(dv(id, s.value)); }, true, tip);
        }
    }
}
}  // namespace foxsdr::slints5
