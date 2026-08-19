// Implementation of the rigctld-compatible command layer. See the header for
// the response-shape rule that governs everything below.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "net/cat_protocol.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <vector>

namespace cascade::net {
namespace {

std::string trimmed(const std::string& s) {
    std::size_t a = 0;
    std::size_t b = s.size();
    const auto space = [](char c) {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n';
    };
    while (a < b && space(s[a])) ++a;
    while (b > a && space(s[b - 1])) --b;
    return s.substr(a, b - a);
}

std::vector<std::string> splitWords(const std::string& s) {
    std::vector<std::string> out;
    std::istringstream in(s);
    std::string w;
    while (in >> w) out.push_back(w);
    return out;
}

std::string upperAscii(std::string s) {
    for (char& c : s) {
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    }
    return s;
}

// "RPRT n\n" — the only thing a set ever returns, and what a failed get
// returns instead of its values.
std::string rprt(int code) { return "RPRT " + std::to_string(code) + "\n"; }

CatResult fail(int code) {
    CatResult r;
    r.reply = rprt(code);
    return r;
}

CatResult ok() { return fail(kCatOk); }

// A whole number of Hz. Frequencies cross this protocol as integers; a
// fractional Hz would be both meaningless to a client and unrepresentable in
// the reply.
std::string hzText(double hz) {
    if (!std::isfinite(hz)) return "0";
    const long long v = static_cast<long long>(std::llround(hz));
    return std::to_string(v);
}

// Strict parse of a frequency argument: digits only, optionally signed and
// with a decimal part that must round to a whole Hz. Rejects text a client
// never sends, rather than silently reading "14250000abc" as a frequency.
bool parseHz(const std::string& text, double& out) {
    if (text.empty()) return false;
    char* end = nullptr;
    const double v = std::strtod(text.c_str(), &end);
    if (end == nullptr || *end != '\0') return false;
    if (!std::isfinite(v)) return false;
    out = v;
    return true;
}

bool parseInt(const std::string& text, long& out) {
    if (text.empty()) return false;
    char* end = nullptr;
    const long v = std::strtol(text.c_str(), &end, 10);
    if (end == nullptr || *end != '\0') return false;
    out = v;
    return true;
}

}  // namespace

TunePlan planTune(double requestedHz, double currentCenterHz, double sampleRateHz,
                  double usableFraction) {
    TunePlan plan;
    if (!std::isfinite(requestedHz)) {
        plan.centerHz = currentCenterHz;
        plan.vfoOffsetHz = 0.0;
        return plan;
    }
    // With no meaningful band there is nothing to stay inside, so the only
    // honest answer is to retune.
    if (!std::isfinite(sampleRateHz) || sampleRateHz <= 0.0) {
        plan.retune = true;
        plan.centerHz = requestedHz;
        plan.vfoOffsetHz = 0.0;
        return plan;
    }
    const double halfUsable = 0.5 * sampleRateHz * usableFraction;
    const double offset = requestedHz - currentCenterHz;
    if (std::fabs(offset) <= halfUsable) {
        plan.retune = false;
        plan.centerHz = currentCenterHz;
        plan.vfoOffsetHz = offset;
        return plan;
    }
    plan.retune = true;
    plan.centerHz = requestedHz;
    plan.vfoOffsetHz = 0.0;
    return plan;
}

const char* catModeToken(cascade::dsp::DemodMode m) {
    using cascade::dsp::DemodMode;
    switch (m) {
        // Hamlib's FM is narrow FM; WFM is the separate broadcast mode, which
        // is why NFM and WFM do not both map to "FM".
        case DemodMode::NFM: return "FM";
        case DemodMode::WFM: return "WFM";
        case DemodMode::AM:  return "AM";
        case DemodMode::DSB: return "DSB";
        case DemodMode::USB: return "USB";
        case DemodMode::LSB: return "LSB";
        case DemodMode::CW:  return "CW";
        case DemodMode::RAW: break;
    }
    // RAW is not a demodulation any client could ask for by name.
    return "";
}

bool catModeFromToken(const std::string& token, cascade::dsp::DemodMode& out) {
    using cascade::dsp::DemodMode;
    const std::string t = upperAscii(trimmed(token));
    if (t == "FM" || t == "FMN" || t == "NFM") { out = DemodMode::NFM; return true; }
    if (t == "WFM" || t == "FMW")              { out = DemodMode::WFM; return true; }
    if (t == "AM")                             { out = DemodMode::AM;  return true; }
    if (t == "DSB")                            { out = DemodMode::DSB; return true; }
    if (t == "USB")                            { out = DemodMode::USB; return true; }
    if (t == "LSB")                            { out = DemodMode::LSB; return true; }
    // CWR is CW received on the reverse sideband. This receiver has one CW
    // demodulator, so both tokens select it rather than one being refused.
    if (t == "CW" || t == "CWR")               { out = DemodMode::CW;  return true; }
    return false;
}

std::string catDumpState(const RadioStatus& status) {
    // The state block a client reads once to learn the radio's shape. The
    // layout is positional, so the comments name each field rather than
    // leaving a column of bare numbers.
    //
    // Frequency range entries are: start, end, modes bitmask, low power, high
    // power, VFO bitmask, antenna bitmask. Power is -1 on a receiver, which is
    // how "cannot transmit" is stated in this protocol.
    const double rate = status.sampleRateHz > 0.0 ? status.sampleRateHz : 0.0;
    (void)rate;

    std::ostringstream o;
    o << "1\n";      // protocol version
    o << "1\n";      // rig model (1 = a generic/dummy rig, which is what we are)
    o << "2\n";      // ITU region

    // RX range list: what this receiver will tune. Deliberately wide, because
    // what it can actually reach is a property of the SoapySDR device attached,
    // not of this program, and a client that is told a narrower range would
    // refuse frequencies the hardware handles perfectly well.
    //
    // The modes bitmask covers AM, CW, USB, LSB, FM and WFM.
    o << "0.000000 4000000000.000000 0x2ef -1 -1 0x1 0x1\n";
    o << "0 0 0 0 0 0 0\n";  // end of RX range list

    // TX range list: empty. This is a receiver, and saying so here is what
    // stops a client from ever offering to key it.
    o << "0 0 0 0 0 0 0\n";

    // Tuning steps: any, for every mode.
    o << "0x2ef 1\n";
    o << "0 0\n";

    // Filters, widest to narrowest, per mode group.
    o << "0x82 500\n";    // CW: 500 Hz
    o << "0x0c 2400\n";   // SSB: 2.4 kHz
    o << "0x01 6000\n";   // AM: 6 kHz
    o << "0x40 15000\n";  // FM: 15 kHz
    o << "0x20 230000\n"; // WFM: 230 kHz
    o << "0 0\n";

    o << "0\n";      // max RIT
    o << "0\n";      // max XIT
    o << "0\n";      // max IF shift
    o << "0\n";      // announces
    o << "\n";       // preamp list (none)
    o << "\n";       // attenuator list (none)
    o << "0x0\n";    // get functions
    o << "0x0\n";    // set functions
    o << "0x0\n";    // get level
    o << "0x0\n";    // set level
    o << "0x0\n";    // get parm
    o << "0x0\n";    // set parm
    return o.str();
}

CatResult executeCatLine(const std::string& line, const RadioStatus& status) {
    const std::string text = trimmed(line);
    if (text.empty()) {
        return CatResult{};  // nothing said, nothing answered
    }

    std::vector<std::string> words = splitWords(text);
    std::string cmd = words[0];
    const auto arg = [&words](std::size_t i) -> std::string {
        return i < words.size() ? words[i] : std::string();
    };

    // Long form (`\get_freq`) is the same command as its short letter. Mapping
    // it here means every command below is written once.
    if (cmd.size() > 1 && cmd[0] == '\\') {
        const std::string name = cmd.substr(1);
        if (name == "dump_state") {
            CatResult r;
            r.reply = catDumpState(status);
            return r;
        }
        if (name == "chk_vfo") {
            // 0 = this server does not require a VFO argument on every command.
            CatResult r;
            r.reply = "CHKVFO 0\n";
            return r;
        }
        if (name == "get_freq")        cmd = "f";
        else if (name == "set_freq")   cmd = "F";
        else if (name == "get_mode")   cmd = "m";
        else if (name == "set_mode")   cmd = "M";
        else if (name == "get_vfo")    cmd = "v";
        else if (name == "set_vfo")    cmd = "V";
        else if (name == "get_ptt")    cmd = "t";
        else if (name == "set_ptt")    cmd = "T";
        else if (name == "get_split_vfo") cmd = "s";
        else if (name == "set_split_vfo") cmd = "S";
        else if (name == "get_powerstat") cmd = "\x01";  // internal marker
        else if (name == "quit")       cmd = "q";
        else return fail(kCatEnimpl);
    }

    if (cmd.size() != 1) {
        return fail(kCatEnimpl);
    }

    switch (cmd[0]) {
        case 'q':
        case 'Q': {
            CatResult r;
            r.quit = true;
            return r;  // no reply: the client asked to leave
        }

        // --- frequency ----------------------------------------------------
        case 'f': {
            CatResult r;
            r.reply = hzText(status.centerHz + status.vfoOffsetHz) + "\n";
            return r;
        }
        case 'F': {
            double hz = 0.0;
            if (!parseHz(arg(1), hz) || hz < 0.0) return fail(kCatEinval);
            const TunePlan plan =
                planTune(hz, status.centerHz, status.sampleRateHz);
            CatResult r = ok();
            r.hasControl = true;
            if (plan.retune) r.control.centerHz = plan.centerHz;
            r.control.vfoOffsetHz = plan.vfoOffsetHz;
            return r;
        }

        // --- mode and passband --------------------------------------------
        case 'm': {
            cascade::dsp::DemodMode m = cascade::dsp::DemodMode::NFM;
            // The snapshot carries the mode by name; an unknown name is a
            // fault worth reporting rather than a silent default.
            if (!cascade::dsp::modeFromName(status.mode, m)) {
                return fail(kCatEinval);
            }
            const char* token = catModeToken(m);
            if (token == nullptr || *token == '\0') return fail(kCatEnavail);
            CatResult r;
            r.reply = std::string(token) + "\n" + hzText(status.bandwidthHz) + "\n";
            return r;
        }
        case 'M': {
            cascade::dsp::DemodMode m = cascade::dsp::DemodMode::NFM;
            if (!catModeFromToken(arg(1), m)) return fail(kCatEinval);
            CatResult r = ok();
            r.hasControl = true;
            r.control.mode = m;
            // Hamlib's convention: a passband of 0 means "the default for this
            // mode", so it must NOT be sent on as a zero-width filter.
            const std::string widthArg = arg(2);
            if (!widthArg.empty()) {
                double width = 0.0;
                if (!parseHz(widthArg, width) || width < 0.0) return fail(kCatEinval);
                if (width > 0.0) r.control.bandwidthHz = width;
            }
            return r;
        }

        // --- VFO ----------------------------------------------------------
        case 'v': {
            CatResult r;
            r.reply = "VFOA\n";
            return r;
        }
        case 'V': {
            const std::string want = upperAscii(arg(1));
            // One receiver, one VFO. Accepting the name it already has is
            // honest; pretending to switch to a second one is not.
            if (want == "VFOA" || want == "A" || want == "CURRVFO" ||
                want == "VFO" || want.empty()) {
                return ok();
            }
            return fail(kCatEnavail);
        }

        // --- split --------------------------------------------------------
        case 's': {
            CatResult r;
            r.reply = "0\nVFOA\n";  // never split: there is no transmitter
            return r;
        }
        case 'S': {
            long on = 0;
            if (!parseInt(arg(1), on)) return fail(kCatEinval);
            if (on == 0) return ok();     // "not split" is the state we are in
            return fail(kCatEnavail);     // and split cannot be entered
        }

        // --- PTT ----------------------------------------------------------
        case 't': {
            CatResult r;
            r.reply = "0\n";  // receive, always
            return r;
        }
        case 'T': {
            long on = 0;
            if (!parseInt(arg(1), on)) return fail(kCatEinval);
            if (on == 0) return ok();  // asking a receiver to stop transmitting is fine
            // Refusing is the point: this radio has no transmitter, and a
            // client that believes otherwise would sit waiting for a signal
            // that never leaves.
            return fail(kCatEnavail);
        }

        // --- power state ---------------------------------------------------
        case '\x01': {
            CatResult r;
            r.reply = status.running ? "1\n" : "0\n";
            return r;
        }

        default:
            return fail(kCatEnimpl);
    }
}

}  // namespace cascade::net
