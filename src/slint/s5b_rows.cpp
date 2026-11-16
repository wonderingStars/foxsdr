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
using slk::kErr;
using slk::kWarn;

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
    buildCat(v);
    buildSourceNotes(v);
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

// ---------------------------------------------------------------- G1-16 / G3-18a / G3-18b CAT: follow a rig, follow OmniRig, answer a logging program on a virtual COM port
// RECEIVE SIDE ONLY. The page says so in words, and what it sends is a setting (the engine's own cat.link.* keys); the link in the engine does the rest. The only messages FoxSDR ever sends to a rig are the reads FA; and MD;.
void S5Rows::buildCat(std::vector<Row>& v) {
    namespace b = foxsdr::slints5b;
    const auto wc = [this](const std::string& k) { return fact("wirec." + k); };
    const auto put = [this](const std::string& what, const std::string& k, const std::string& value) {
        Cmd c; c.op = FOXAPP_OP_WIREC_SETTING; c.text = k + std::string(1, static_cast<char>(0x1F)) + value; send(what, c);
    };
    const std::string mode = wc("cat.link.mode").empty() ? "off" : wc("cat.link.mode");
    v.push_back(rHeading(tr("CAT: follow a rig")));
    v.push_back(rNote(tr("FoxSDR reads a transceiver's frequency and mode and moves its own centre and mode to match (a panadapter), or answers a logging program on a virtual COM port. It never sends a command that keys the transmitter: the only messages it sends to a rig are the two reads FA; and MD;.")));
    choice(v, "b.cat.mode", tr("How"), b::catModeWords(), b::catModeIndex(mode), [this, put](int i) {
        put(tr("CAT: follow a rig"), "cat.link.mode", foxsdr::slints5b::catModeKey(i));
    }, tr("Off opens nothing. Nothing is opened until you choose a way and give what it needs."));
    if (mode == "follow" || mode == "emulate") {
        const std::string port = wc("cat.link.port");
        field(v, "b.cat.port", tr("Serial port"), port, tr("COM1 to COM256. For a virtual COM pair, the end FoxSDR uses."));
        key(v, "b.cat.port.set", tr("Use this port"), [this, put] {
            std::string norm;
            if (!foxsdr::slintlanel::validComPort(dv("b.cat.port"), norm)) { refuseHere(tr("Use this port"), tr("a serial port is COM1 to COM256")); return; }
            put(tr("Use this port"), "cat.link.port", norm);
        });
        const std::string dia = wc("cat.link.dialect");
        choice(v, "b.cat.dialect", tr("Rig's command set"), {tr("Kenwood"), tr("Yaesu")}, dia == "yaesu" ? 1 : 0, [put](int i) { put(tr("Rig's command set"), "cat.link.dialect", i == 1 ? "yaesu" : "kenwood"); },
               tr("The rig's own menu says which. Kenwood text commands or Yaesu newcat."));
        const std::string baud = wc("cat.link.baud");
        field(v, "b.cat.baud", tr("Baud rate"), baud == "0" ? std::string() : baud, tr("The rate the rig is set to in its menu: nothing is assumed"));
        key(v, "b.cat.baud.set", tr("Use this baud rate"), [this, put] {
            long long n = 0; std::string e;
            if (!foxsdr::slintlanel::parseInteger(dv("b.cat.baud"), 300, 4000000, n, e)) { refuseHere(tr("Use this baud rate"), e); return; }
            put(tr("Use this baud rate"), "cat.link.baud", std::to_string(n));
        });
        choice(v, "b.cat.stop", tr("Stop bits"), {"1", "2"}, wc("cat.link.stopbits") == "2" ? 1 : 0, [put](int i) { put(tr("Stop bits"), "cat.link.stopbits", i == 1 ? "2" : "1"); });
    }
    if (mode == "omnirig") {
        choice(v, "b.cat.rig", tr("OmniRig rig"), {tr("Rig 1"), tr("Rig 2")}, wc("cat.link.rig") == "2" ? 1 : 0, [put](int i) { put(tr("OmniRig rig"), "cat.link.rig", i == 1 ? "2" : "1"); },
               tr("OmniRig has two rig slots; choose the one you set up there."));
    }
    if (mode == "follow" || mode == "omnirig") {
        tick(v, "b.cat.ff", tr("Follow the rig's frequency"), wc("cat.link.follow_frequency") != "0", [put](bool on) { put(tr("Follow the rig's frequency"), "cat.link.follow_frequency", on ? "1" : "0"); });
        tick(v, "b.cat.fm", tr("Follow the rig's mode"), wc("cat.link.follow_mode") != "0", [put](bool on) { put(tr("Follow the rig's mode"), "cat.link.follow_mode", on ? "1" : "0"); });
    }
    if (mode != "off") {
        const std::string st = fact("cat.link.state");
        v.push_back(rReadout(tr("CAT link"), foxsdr::slints5b::catStateWords(st)));
        const std::string hz = fact("cat.link.rigHz");
        if (!hz.empty()) { v.push_back(rReadout(tr("The rig"), foxsdr::slints5b::catRigWords(hz, fact("cat.link.rigMode")))); }
        const std::string err = fact("cat.link.error");
        if (!err.empty()) { v.push_back(rNote(err, kErr)); }
        if (mode == "omnirig" && st == "error") { v.push_back(rNote(tr("OmniRig is other people's free program. FoxSDR does not download or install it. It must be installed and set up for your rig, then chosen here."), kWarn)); }
    }
}

// ---------------------------------------------------------------- what works today for the radios and servers that have no control of their own (G1-07m AntSDR, G1-03a SDR++ Server)
void S5Rows::buildSourceNotes(std::vector<Row>& v) {
    v.push_back(rHeading(tr("Other radios and servers")));
    v.push_back(rNote(tr("AntSDR: open it as an ADALM-Pluto. Give the Pluto source the AntSDR's own address and it is read like a Pluto.")));
    v.push_back(rNote(tr("SDR++ Server: start it in its rtl_tcp-compatible mode and open it with the rtl_tcp source, giving that address and port.")));
}
}  // namespace foxsdr::slints5
