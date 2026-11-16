// s5_logic.cpp - see s5_logic.hpp.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "s5_logic.hpp"

#include <cctype>
#include <cmath>
#include <cstdio>

#include "core/i18n.hpp"

namespace foxsdr::slints5 {

namespace {
// the screen's words: every literal below goes through the string tables (tools/i18n_keys.py finds the tr("...") calls)
std::string tr(const char* s) { return cascade::i18n::tr(s); }
using foxsdr::slintlanel::numberText;
using foxsdr::slintlanel::parseInteger;
using foxsdr::slintlanel::parseNumber;
using foxsdr::slintlanel::trim;

Built bad(const std::string& why) { Built b; b.ok = false; b.err = why; return b; }
constexpr double kMinRateHz = 1000.0;      // the engine's own bounds (source/raw_iq_net_source.hpp kMinRateHz / kMaxRateHz); the engine judges again
constexpr double kMaxRateHz = 100e6;
}  // namespace

// ============================================================ raw I/Q ============================================================
std::vector<std::string> rawIqFormatWords() { return {tr("32-bit float"), tr("16-bit integer"), tr("8-bit unsigned")}; }

bool validHost(const std::string& hostIn, std::string& err) {
    const std::string host = trim(hostIn);
    if (host.empty()) { err = tr("type the address of the stream's computer"); return false; }
    if (host.size() > 253) { err = tr("that address is too long"); return false; }
    for (const char c : host) {
        const unsigned char u = static_cast<unsigned char>(c);
        const bool ok = std::isalnum(u) != 0 || c == '.' || c == '-' || c == '_' || c == ':' || c == '[' || c == ']';
        if (!ok) { err = tr("an address has letters, digits, dots, dashes and colons only"); return false; }
    }
    return true;
}

Built rawIqOpenText(const RawIqFields& f) {
    std::string e;
    if (!validHost(f.host, e)) { return bad(e); }
    long long port = 0;
    if (!parseInteger(f.port, 1, 65535, port, e)) { return bad(tr("Port") + ": " + e); }
    std::string q;
    const std::string fmt = f.format == 1 ? "s16" : f.format == 2 ? "u8" : "f32";
    if (!f.header) {
        double rate = 0;
        if (!parseNumber(f.rateHz, kMinRateHz, kMaxRateHz, rate, e)) { return bad(tr("Sample rate (Hz)") + ": " + e); }
        double centre = 0;
        if (!parseNumber(f.centreHz.empty() ? "0" : f.centreHz, 0.0, 1e13, centre, e)) { return bad(tr("Centre frequency shown (Hz)") + ": " + e); }
        q = "?fmt=" + fmt + "&rate=" + numberText(rate) + "&freq=" + numberText(centre);
    } else {
        q = "?hdr=1";
    }
    long long timeout = 3000;
    if (!f.timeoutMs.empty() && !parseInteger(f.timeoutMs, 100, 600000, timeout, e)) { return bad(tr("Silence allowed (ms)") + ": " + e); }
    q += "&timeout=" + std::to_string(timeout);
    Built b;
    b.ok = true;
    b.text = std::string(f.udp != 0 ? "udp://" : "tcp://") + trim(f.host) + ":" + std::to_string(port) + q;
    return b;
}

std::string rawIqProtocolNote(const RawIqFields& f) {
    if (f.udp != 0) {
        const std::string h = trim(f.host);
        const bool local = h == "127.0.0.1" || h == "localhost" || h == "::1" || h == "[::1]";
        return local ? tr("UDP: FoxSDR listens on this computer only, on that port, until you stop it or open another source.")
                     : tr("UDP: FoxSDR listens on that address and port, so other computers on the network can send to it, until you stop it or open another source.");
    }
    return tr("TCP: FoxSDR connects to that address and port and reads the stream. Nothing is sent to it.");
}

// ============================================================ automatic ppm ============================================================
bool parsePpmSeconds(const std::string& text, double& out, std::string& err) {
    if (trim(text).empty()) { out = 0.0; return true; }
    double v = 0.0;
    if (!parseNumber(text, 0.0, 300.0, v, err)) { return false; }
    if (v != 0.0 && v < 10.0) { err = tr("allowed:") + " 10 - 300 (0 = 30)"; return false; }
    out = v;
    return true;
}

std::string ppmStateWords(const std::string& state) {
    if (state == "measuring") { return tr("measuring"); }
    if (state == "done") { return tr("done"); }
    if (state == "failed") { return tr("no usable pilot found"); }
    if (state == "idle") { return tr("not measuring"); }
    return std::string();
}

std::string ppmReading(double ppm, double unc, int readings) {
    if (readings <= 0) { return std::string(); }
    char a[32], u[32];
    std::snprintf(a, sizeof(a), "%+.2f", ppm);
    std::snprintf(u, sizeof(u), "%.2f", unc);
    return std::string(a) + " ppm, " + tr("plus or minus") + " " + u + " (" + std::to_string(readings) + " " + tr("readings") + ")";
}

bool replacesTypedPpm(const PpmInForce& now, double measured) {
    if (!now.on) { return false; }
    return std::fabs(now.ppm - measured) > 0.005;
}

std::string ppmReference() {
    return tr("Reference: the 19 kHz stereo pilot of the FM broadcast station this radio is tuned to, taken as exactly 19 kHz. A station's own pilot is not perfect: measure on two or three stations and trust the ones that agree. Nothing is changed until you press Use this correction.");
}

// ============================================================ tuner pooling ============================================================
std::vector<std::string> claimModeWords() {
    return {tr("Leave the mode"), "NFM", "WFM", "AM", "DSB", "USB", "CW", "LSB", "RAW"};
}

bool parseClaimMHz(const std::string& text, double& hz, std::string& err) {
    double mhz = 0.0;
    if (!parseNumber(text, 0.000001, 1.0e6, mhz, err)) { return false; }
    hz = mhz * 1.0e6;
    return true;
}

bool parseClaimKHz(const std::string& text, double& hz, std::string& err) {
    if (trim(text).empty()) { hz = 0.0; return true; }
    double khz = 0.0;
    if (!parseNumber(text, 0.0, 1.0e6, khz, err)) { return false; }
    hz = khz * 1.0e3;
    return true;
}

std::string claimAnswerText(double radio, double how) {
    const std::string head = tr("Radio") + " " + std::to_string(static_cast<int>(radio + 0.5)) + ": ";
    return head + (how > 1.5 ? tr("its centre moved to cover it") : tr("it already covered that frequency, so only its tuning moved"));
}

// ============================================================ direct sampling ============================================================
std::vector<std::string> directSamplingWords() { return {tr("Automatic"), tr("Never"), tr("I branch"), tr("Q branch")}; }

int directSamplingIndex(const std::string& fact) {
    if (fact.size() == 1 && fact[0] >= '0' && fact[0] <= '3') { return fact[0] - '0'; }
    return -1;
}

}  // namespace foxsdr::slints5
