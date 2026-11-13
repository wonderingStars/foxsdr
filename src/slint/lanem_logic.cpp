// lanem_logic.cpp - see lanem_logic.hpp.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "lanem_logic.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>

#include <nlohmann/json.hpp>

#include "core/airband_channel.hpp"
#include "core/i18n.hpp"

namespace foxsdr::slintlanem {

namespace {
using nlohmann::json;
using cascade::i18n::tr;
namespace ab = cascade::core::airband;
using foxsdr::slintlanel::numberText;
using foxsdr::slintlanel::parseInteger;
using foxsdr::slintlanel::parseNumber;
using foxsdr::slintlanel::trim;

std::string dumpOf(const json& j) { return j.dump(-1, ' ', false, json::error_handler_t::replace); }
Built bad(const std::string& why) { Built b; b.ok = false; b.err = why; return b; }
Built good(const json& j) { Built b; b.ok = true; b.text = dumpOf(j); return b; }

bool num(const std::string& text, const char* what, double lo, double hi, double& out, std::string& err) {
    std::string e;
    if (parseNumber(text, lo, hi, out, e)) { return true; }
    err = std::string(what) + ": " + e;
    return false;
}
bool integer(const std::string& text, const char* what, long long lo, long long hi, long long& out, std::string& err) {
    std::string e;
    if (parseInteger(text, lo, hi, out, e)) { return true; }
    err = std::string(what) + ": " + e;
    return false;
}
// a full path: absolute, no control characters (the engine reads or writes it; a dialog is never raised)
bool fullPath(const std::string& text, std::string& err) {
    const std::string t = trim(text);
    if (t.empty()) { err = tr("type the full path of the file"); return false; }
    for (const char c : t) { if (static_cast<unsigned char>(c) < 0x20) { err = tr("that is not a full path"); return false; } }
    const std::filesystem::path p(std::u8string(t.begin(), t.end()));
    if (!p.is_absolute()) { err = tr("that is not a full path"); return false; }
    return true;
}
std::string numField(const json& j, const char* key) {
    if (!j.is_object() || !j.contains(key)) { return std::string(); }
    const json& v = j[key];
    if (v.is_number()) { return numberText(v.get<double>()); }
    if (v.is_string()) { return v.get<std::string>(); }
    if (v.is_boolean()) { return v.get<bool>() ? "1" : "0"; }
    return std::string();
}
}  // namespace

// ============================================================ I/Q file transport ============================================================
bool parseSpeed(const std::string& text, double& out, std::string& err) { return parseNumber(text, 0.25, 8.0, out, err); }

std::string speedText(double speed) { return numberText(speed) + "x"; }

std::string policyWords(const std::string& fact) {
    if (fact == "varispeed") { return tr("varispeed"); }
    if (fact == "blockskip") { return tr("block skip"); }
    return std::string();
}

bool parseStepBlocks(const std::string& text, long long& out, std::string& err) {
    long long v = 0;
    if (!parseInteger(text, -4096, 4096, v, err)) { return false; }
    if (v == 0) { err = tr("a step is at least one block, forward or back"); return false; }
    out = v;
    return true;
}

// ============================================================ RIT ============================================================
bool parseRitHz(const std::string& text, double limit, double& out, std::string& err) {
    const double lim = limit >= 1.0 && limit <= 20000.0 ? limit : 9999.0;
    return parseNumber(text, -lim, lim, out, err);
}

bool parseRitLimits(const std::string& maxText, const std::string& clearText, double& maxHz, double& clearHz, std::string& err) {
    std::string e;
    double a = 0, b = 0;
    if (!parseNumber(maxText, 1.0, 20000.0, a, e)) { err = std::string(tr("Largest offset")) + ": " + e; return false; }
    if (!parseNumber(clearText, 0.0, 1e9, b, e)) { err = std::string(tr("Clear on a retune of")) + ": " + e; return false; }
    maxHz = a;
    clearHz = b;
    return true;
}

std::string signedHzText(double hz) {
    const long long v = std::llround(hz);
    return std::string(v > 0 ? "+" : "") + std::to_string(v) + " Hz";
}

// ============================================================ CW surround ============================================================
Built cwSurroundJson(bool enabled, const std::string& centreHz, const std::string& spreadHz, bool invert, const std::string& toneRatio, const std::string& holdMs) {
    std::string err;
    double c = 0, s = 0, r = 0, h = 0;
    if (!num(centreHz, tr("Centre pitch (Hz)"), 200.0, 1500.0, c, err) || !num(spreadHz, tr("Spread (Hz)"), 50.0, 2000.0, s, err) || !num(toneRatio, tr("Tone ratio"), 2.0, 1000.0, r, err) ||
        !num(holdMs, tr("Hold (ms)"), 0.0, 5000.0, h, err)) { return bad(err); }
    return good({{"enabled", enabled}, {"centreHz", c}, {"spreadHz", s}, {"invert", invert}, {"toneRatio", r}, {"holdMs", h}});
}

// ============================================================ the stitched sweep and the sweep history ============================================================
Built stitchedSweepJson(const std::string& radio, const std::string& startMHz, const std::string& stopMHz, const std::string& stepFraction, const std::string& trimText, const std::string& settleMs,
                        const std::string& frames, const std::string& timeoutMs, const std::string& passes) {
    std::string err;
    long long rd = 0, fr = 0, to = 0, ps = 0, st = 0;
    double a = 0, b = 0, sf = 0, tr_ = 0;
    if (!integer(radio, tr("Radio to sweep"), 1, 8, rd, err) || !num(startMHz, tr("Radio sweep start (MHz)"), 0.001, 1e7, a, err) || !num(stopMHz, tr("Radio sweep stop (MHz)"), 0.001, 1e7, b, err) ||
        !num(stepFraction, tr("Step (part of the span)"), 0.3, 0.7, sf, err) || !num(trimText, tr("Edge trimmed (part)"), 0.0, 0.4, tr_, err) || !integer(settleMs, tr("Settle time (ms)"), 0, 5000, st, err) ||
        !integer(frames, tr("Frames averaged"), 1, 64, fr, err) || !integer(timeoutMs, tr("Give up after (ms)"), 500, 60000, to, err) || !integer(passes, tr("Passes"), 0, 1000000, ps, err)) { return bad(err); }
    if (!(b > a)) { return bad(tr("the stop must be above the start")); }
    return good({{"kind", "radio"}, {"radio", rd}, {"start_mhz", a}, {"stop_mhz", b}, {"step_fraction", sf}, {"trim", tr_}, {"settle_ms", st}, {"frames", fr}, {"timeout_ms", to}, {"passes", ps}});
}

Built sweepHistoryJson(const std::string& action, const std::string& path, int format) {
    if (action != "save" && action != "load" && action != "clear") { return bad(tr("that is not a history action")); }
    if (action == "clear") { return good({{"action", "clear"}}); }
    std::string err;
    if (!fullPath(path, err)) { return bad(err); }
    json j = {{"action", action}, {"path", trim(path)}};
    if (action == "save") { j["format"] = format == 1 ? "json" : format == 2 ? "xml" : "csv"; }
    return good(j);
}

std::string historyReadout(const std::string& historyFact) {
    if (historyFact.empty()) { return std::string(); }
    const json j = json::parse(historyFact, nullptr, false);
    if (!j.is_object()) { return std::string(); }
    const double lines = j.value("lines", 0.0), points = j.value("points", 0.0), keep = j.value("keep_points", 0.0);
    return numberText(lines) + " " + tr("sweeps") + ", " + numberText(points) + " " + tr("points of") + " " + numberText(keep);
}

std::string stitchProgress(const std::string& stitchFact) {
    if (stitchFact.empty()) { return std::string(); }
    const json j = json::parse(stitchFact, nullptr, false);
    if (!j.is_object() || !j.value("active", false)) { return std::string(); }
    return std::string(tr("step")) + " " + numberText(j.value("step", 0.0)) + " " + tr("of") + " " + numberText(j.value("centres", 0.0));
}

// ============================================================ airband channel ============================================================
bool airbandHz(const std::string& mhzText, long long& hz, std::string& err) {
    double v = 0;
    if (!parseNumber(mhzText, 0.1, 6000.0, v, err)) { return false; }
    hz = std::llround(v * 1e6);
    return true;
}

AirbandBuilt airbandChannelText(const AirbandFields& f) {
    AirbandBuilt r;
    std::string err;
    ab::AirbandChannelConfig c;
    long long hz = 0;
    if (!airbandHz(f.freqMHz, hz, err)) { r.err = std::string(tr("Channel frequency (MHz)")) + ": " + err; return r; }
    c.freqHz = hz;
    double bw = 0, lvl = 0, snr = 0, hyst = 0, hold = 0, hp = 0, lp = 0, mb = 0, port = 0;
    if (!num(f.bandwidthKHz, tr("Channel width (kHz)"), 1.0, 200.0, bw, err) || !num(f.levelDb, tr("Squelch level (dB)"), -120.0, 0.0, lvl, err) || !num(f.snrOpenDb, tr("Squelch opens at (dB SNR)"), 1.0, 60.0, snr, err) ||
        !num(f.snrHystDb, tr("Squelch hysteresis (dB)"), 0.0, 30.0, hyst, err) || !num(f.holdMs, tr("Squelch hold (ms)"), 0.0, 10000.0, hold, err) || !num(f.highPassHz, tr("High-pass (Hz, 0 = off)"), 0.0, 3000.0, hp, err) ||
        !num(f.lowPassHz, tr("Low-pass (Hz, 0 = off)"), 0.0, 20000.0, lp, err) || !num(f.maxFileMb, tr("File size limit (MB)"), 1.0, 2000.0, mb, err) || !num(f.udpPort, tr("Stream port"), 0.0, 65535.0, port, err)) { r.err = err; return r; }
    c.bandwidthHz = static_cast<int>(std::llround(bw * 1000.0));
    c.mode = f.mode == 1 ? 1 : 0;
    c.squelchKind = f.squelchKind == 1 ? ab::SquelchKind::Snr : ab::SquelchKind::Level;
    c.squelchTenthsDb = static_cast<int>(std::llround(lvl * 10.0));
    c.snrOpenTenthsDb = static_cast<int>(std::llround(snr * 10.0));
    c.snrHysteresisTenthsDb = static_cast<int>(std::llround(hyst * 10.0));
    c.holdMs = static_cast<int>(std::llround(hold));
    c.highPassHz = static_cast<int>(std::llround(hp));
    c.lowPassHz = static_cast<int>(std::llround(lp));
    c.filterOrder = f.filterOrder == 2 ? 2 : 4;
    // notches: "hz/Q; hz/Q"
    {
        std::string rest = f.notches;
        int n = 0;
        std::size_t pos = 0;
        while (pos <= rest.size()) {
            std::size_t end = rest.find_first_of(";,", pos);
            if (end == std::string::npos) { end = rest.size(); }
            const std::string item = trim(rest.substr(pos, end - pos));
            pos = end + 1;
            if (item.empty()) { continue; }
            if (n >= ab::kMaxNotches) { r.err = tr("at most three notches"); return r; }
            const std::size_t slash = item.find('/');
            double nh = 0, nq = 30.0;
            if (!num(item.substr(0, slash), tr("Notch (Hz)"), 50.0, 20000.0, nh, err)) { r.err = err; return r; }
            if (slash != std::string::npos && !num(item.substr(slash + 1), tr("Notch Q"), 3.0, 100.0, nq, err)) { r.err = err; return r; }
            c.notches[static_cast<std::size_t>(n)].hz = static_cast<int>(std::llround(nh));
            c.notches[static_cast<std::size_t>(n)].qTenths = static_cast<int>(std::llround(nq * 10.0));
            ++n;
        }
    }
    c.output = f.output == 1 ? ab::OutputKind::WavFile : f.output == 2 ? ab::OutputKind::Udp : ab::OutputKind::None;
    c.dailyFolder = f.dailyFolder;
    c.maxFileMb = static_cast<int>(std::llround(mb));
    c.udpPort = static_cast<int>(std::llround(port));
    const ab::Problem p = ab::validate(c);
    if (p != ab::Problem::None) { r.err = tr(ab::problemText(p)); return r; }
    r.ok = true;
    r.text = ab::serialise(c);
    r.freqHz = hz;
    return r;
}

bool airbandFieldsFrom(const std::string& line, AirbandFields& out) {
    const ab::ParseResult p = ab::parse(line);
    if (!p.ok) { return false; }
    const ab::AirbandChannelConfig& c = p.config;
    AirbandFields f;
    f.freqMHz = numberText(static_cast<double>(c.freqHz) / 1e6);
    f.bandwidthKHz = numberText(static_cast<double>(c.bandwidthHz) / 1000.0);
    f.mode = c.mode;
    f.squelchKind = c.squelchKind == ab::SquelchKind::Snr ? 1 : 0;
    f.levelDb = numberText(c.squelchTenthsDb / 10.0);
    f.snrOpenDb = numberText(c.snrOpenTenthsDb / 10.0);
    f.snrHystDb = numberText(c.snrHysteresisTenthsDb / 10.0);
    f.holdMs = std::to_string(c.holdMs);
    f.highPassHz = std::to_string(c.highPassHz);
    f.lowPassHz = std::to_string(c.lowPassHz);
    f.filterOrder = c.filterOrder;
    for (const ab::Notch& n : c.notches) {
        if (n.hz == 0) { continue; }
        if (!f.notches.empty()) { f.notches += "; "; }
        f.notches += std::to_string(n.hz) + "/" + numberText(n.qTenths / 10.0);
    }
    f.output = static_cast<int>(c.output);
    f.dailyFolder = c.dailyFolder;
    f.maxFileMb = std::to_string(c.maxFileMb);
    f.udpPort = std::to_string(c.udpPort);
    out = f;
    return true;
}

// ============================================================ the level store ============================================================
bool parseCapMiB(const std::string& text, double& out, std::string& err) { return parseNumber(text, 1.0, 64.0, out, err); }

std::string bytesText(double bytes) {
    char b[48];
    std::snprintf(b, sizeof(b), "%.1f MB", bytes / 1e6);
    return b;
}

// ============================================================ the spectrum's GPU counters ============================================================
GpuCounters parseGpuCounters(const std::string& fact) {
    GpuCounters g;
    if (fact.empty()) { return g; }
    const json j = json::parse(fact, nullptr, false);
    if (!j.is_object()) { return g; }
    g.have = true;
    g.backend = j.value("backend", std::string());
    g.engaged = j.value("engaged", false);
    g.error = j.value("error", std::string());
    g.hasBlock = j.contains("block_gpu_frames") || j.contains("block_cpu_frames");
    g.blockGpu = j.value("block_gpu_frames", 0.0);
    g.blockCpu = j.value("block_cpu_frames", 0.0);
    g.blockGpuUp = j.value("block_gpu_up", false);
    return g;
}

// ============================================================ a plugin's request to open a page or a folder ============================================================
bool parseOpenRequest(const std::string& fact, OpenRequest& out) {
    std::vector<std::string> parts;
    std::string cur;
    for (const char c : fact) {
        if (c == '\x1F' && parts.size() < 3) { parts.push_back(cur); cur.clear(); } else { cur.push_back(c); }
    }
    parts.push_back(cur);
    if (parts.size() != 4) { return false; }
    if (parts[0].empty() || (parts[2] != "url" && parts[2] != "folder") || parts[3].empty()) { return false; }
    out.moduleFile = parts[0];
    out.pluginName = parts[1];
    out.kind = parts[2];
    out.target = parts[3];
    return true;
}

std::string openConfirmText(const OpenRequest& r) { return r.moduleFile + '\x1F' + r.kind; }

// ============================================================ the patch edit ============================================================
Built patchIdsJson(const std::string& idsText) {
    json ids = json::array();
    std::string cur;
    const auto flush = [&]() -> bool {
        if (cur.empty()) { return true; }
        long long v = 0;
        std::string e;
        if (!parseInteger(cur, 1, 2000000000LL, v, e)) { return false; }
        ids.push_back(v);
        cur.clear();
        return true;
    };
    for (const char c : idsText) {
        if (c == ',' || c == ';' || std::isspace(static_cast<unsigned char>(c)) != 0) { if (!flush()) { return bad(tr("a part number is a whole number from 1")); } }
        else { cur.push_back(c); }
    }
    if (!flush()) { return bad(tr("a part number is a whole number from 1")); }
    if (ids.empty()) { return bad(tr("type the numbers of the parts")); }
    if (ids.size() > 64) { return bad(tr("at most 64 parts at a time")); }
    return good({{"ids", ids}});
}

bool parsePastePoint(const std::string& xText, const std::string& yText, double& x, double& y, std::string& err) {
    double a = 0, b = 0;
    std::string e;
    if (!parseNumber(xText, -100000.0, 100000.0, a, e)) { err = std::string(tr("Paste at x")) + ": " + e; return false; }
    if (!parseNumber(yText, -100000.0, 100000.0, b, e)) { err = std::string(tr("Paste at y")) + ": " + e; return false; }
    x = a;
    y = b;
    return true;
}

// ============================================================ the symbol rate ============================================================
bool parseSymbolRate(const std::string& text, double& out, std::string& err) { return parseNumber(text, 0.0, 1e8, out, err); }

std::string symbolRateText(double baud) {
    if (baud == std::floor(baud)) { return std::to_string(static_cast<long long>(baud)); }
    char b[48];
    std::snprintf(b, sizeof(b), "%.6f", baud);
    std::string s = b;
    while (!s.empty() && s.back() == '0') { s.pop_back(); }
    if (!s.empty() && s.back() == '.') { s.pop_back(); }
    return s;
}

// ============================================================ a mark's credit ============================================================
std::string withCredit(const std::string& tooltip, const std::string& credit) {
    const std::string c = trim(credit);
    if (c.empty()) { return tooltip; }
    return tooltip.empty() ? c : tooltip + "\n" + c;
}

}  // namespace foxsdr::slintlanem
