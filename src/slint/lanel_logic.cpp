// lanel_logic.cpp - see lanel_logic.hpp.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "lanel_logic.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstring>

#include <nlohmann/json.hpp>

#include "core/i18n.hpp"
#include "foxsdr_api.h"

namespace foxsdr::slintlanel {

namespace {
using nlohmann::json;
using cascade::i18n::tr;
using foxsdr::slintlanek::rHeading;
using foxsdr::slintlanek::rNote;
using foxsdr::slintlanek::rReadout;
using foxsdr::slintlanek::rField;
using foxsdr::slintlanek::rKey;
using foxsdr::slintlanek::kDim;
using foxsdr::slintlanek::kWarn;
using foxsdr::slintlanek::kGood;

std::string dumpOf(const json& j) { return j.dump(-1, ' ', false, json::error_handler_t::replace); }

Built bad(const std::string& why) { Built b; b.ok = false; b.err = why; return b; }
Built good(const json& j) { Built b; b.ok = true; b.text = dumpOf(j); return b; }

// one number field, with its name for the sentence that says what is wrong
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
}  // namespace

// ============================================================ typed text ============================================================
std::string trim(const std::string& s) {
    std::size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a])) != 0) { ++a; }
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1])) != 0) { --b; }
    return s.substr(a, b - a);
}

bool parseNumber(const std::string& text, double lo, double hi, double& out, std::string& err) {
    const std::string t = trim(text);
    if (t.empty()) { err = tr("type a number"); return false; }
    double v = 0.0;
    const char* first = t.data();
    const char* last = t.data() + t.size();
    if (*first == '+') { ++first; }
    const auto r = std::from_chars(first, last, v);
    if (r.ec != std::errc() || r.ptr != last || !std::isfinite(v)) { err = tr("that is not a number (use a dot for decimals)"); return false; }
    if (v < lo || v > hi) {
        char b[96];
        std::snprintf(b, sizeof(b), "%s %s to %s", tr("allowed:"), numberText(lo).c_str(), numberText(hi).c_str());
        err = b;
        return false;
    }
    out = v;
    return true;
}

bool parseInteger(const std::string& text, long long lo, long long hi, long long& out, std::string& err) {
    const std::string t = trim(text);
    if (t.empty()) { err = tr("type a whole number"); return false; }
    long long v = 0;
    const char* first = t.data();
    const char* last = t.data() + t.size();
    if (*first == '+') { ++first; }
    const auto r = std::from_chars(first, last, v);
    if (r.ec != std::errc() || r.ptr != last) { err = tr("that is not a whole number"); return false; }
    if (v < lo || v > hi) {
        char b[96];
        std::snprintf(b, sizeof(b), "%s %lld to %lld", tr("allowed:"), lo, hi);
        err = b;
        return false;
    }
    out = v;
    return true;
}

bool splitList(const std::string& text, std::size_t maxItems, std::size_t maxLen, std::vector<std::string>& out) {
    out.clear();
    std::string cur;
    const auto flush = [&]() {
        const std::string t = trim(cur);
        cur.clear();
        if (!t.empty()) { out.push_back(t); }
    };
    for (const char c : text) {
        if (c == ',' || c == ';' || c == '\n' || c == '\r') { flush(); } else { cur.push_back(c); }
    }
    flush();
    if (out.size() > maxItems) { out.resize(maxItems); }
    for (const auto& s : out) { if (s.size() > maxLen) { return false; } }
    return true;
}

std::string numberText(double v) {
    char b[48];
    if (std::fabs(v - std::round(v)) < 1e-9 && std::fabs(v) < 1e15) { std::snprintf(b, sizeof(b), "%.0f", v); }
    else { std::snprintf(b, sizeof(b), "%.6g", v); }
    return b;
}

bool validComPort(const std::string& text, std::string& normalised) {
    std::string t = trim(text);
    for (auto& c : t) { c = static_cast<char>(std::toupper(static_cast<unsigned char>(c))); }
    if (t.size() < 4 || t.size() > 6 || t.compare(0, 3, "COM") != 0) { return false; }
    long long n = 0;
    for (std::size_t i = 3; i < t.size(); ++i) {
        if (std::isdigit(static_cast<unsigned char>(t[i])) == 0) { return false; }
        n = n * 10 + (t[i] - '0');
    }
    if (n < 1 || n > 256 || (t[3] == '0')) { return false; }
    normalised = t;
    return true;
}

// ============================================================ operation texts ============================================================
Built scanTriggerJson(int mode, const std::string& fixedDb, const std::string& marginDb, const std::string& hysteresisDb, const std::string& riseDb) {
    double f = 0, m = 0, h = 0, r = 0;
    std::string err;
    if (!num(fixedDb, tr("Level"), -160.0, 20.0, f, err) || !num(marginDb, tr("Margin"), 0.0, 60.0, m, err) || !num(hysteresisDb, tr("Hysteresis"), 0.0, 30.0, h, err) || !num(riseDb, tr("Floor rise"), 0.0, 10.0, r, err)) { return bad(err); }
    return good({{"version", 1}, {"mode", mode == 0 ? "fixed" : "relative"}, {"fixedDb", f}, {"marginDb", m}, {"hysteresisDb", h}, {"riseDbPerReading", r}});
}

Built scanPlanJson(const std::string& rangesText, int direction, const std::string& triggerJson) {
    json ranges = json::array();
    std::size_t pos = 0;
    int line = 0;
    while (pos <= rangesText.size()) {
        std::size_t nl = rangesText.find_first_of("\r\n;", pos);
        if (nl == std::string::npos) { nl = rangesText.size(); }
        const std::string ln = trim(rangesText.substr(pos, nl - pos));
        pos = nl + 1;
        if (ln.empty()) { continue; }
        ++line;
        std::vector<std::string> w;
        std::size_t i = 0;
        while (i < ln.size() && w.size() < 5) {
            while (i < ln.size() && std::isspace(static_cast<unsigned char>(ln[i])) != 0) { ++i; }
            std::size_t j = i;
            while (j < ln.size() && std::isspace(static_cast<unsigned char>(ln[j])) == 0) { ++j; }
            if (j > i) { w.push_back(ln.substr(i, j - i)); }
            i = j;
        }
        const std::string label = trim(ln.substr(std::min(i, ln.size())));
        char which[40];
        std::snprintf(which, sizeof(which), "%s %d", tr("Range"), line);
        if (w.size() < 5) { return bad(std::string(which) + ": " + tr("write start MHz, stop MHz, step kHz, mode, bandwidth kHz")); }
        double a = 0, b = 0, st = 0, bw = 0;
        std::string err;
        if (!num(w[0], which, 0.000001, 1e7, a, err) || !num(w[1], which, 0.000001, 1e7, b, err) || !num(w[2], which, 0.001, 1e6, st, err) || !num(w[4], which, 0.05, 1e4, bw, err)) { return bad(err); }
        if (b < a) { return bad(std::string(which) + ": " + tr("the stop is below the start")); }
        std::string mode = w[3];
        for (auto& c : mode) { c = static_cast<char>(std::toupper(static_cast<unsigned char>(c))); }
        ranges.push_back({{"startHz", a * 1e6}, {"stopHz", b * 1e6}, {"stepHz", st * 1e3}, {"mode", mode}, {"bandwidthHz", bw * 1e3}, {"label", label}});
        if (ranges.size() > 16) { return bad(tr("a plan has at most 16 ranges")); }
    }
    if (ranges.empty()) { return bad(tr("write at least one range")); }
    json trig = json::parse(triggerJson, nullptr, false);
    if (trig.is_discarded() || !trig.is_object()) { return bad(tr("the trigger is not valid")); }
    return good({{"version", 1}, {"direction", direction == 1 ? "backward" : "forward"}, {"trigger", trig}, {"ranges", ranges}});
}

Built fftScanJson(bool enabled, const std::string& startMHz, const std::string& stopMHz, const std::string& stepKHz, int mode, const std::string& marginDb, const std::string& fixedDb, bool merge) {
    double a = 0, b = 0, st = 0, m = 0, f = 0;
    std::string err;
    if (!num(startMHz, tr("Start (MHz)"), 0.000001, 1e7, a, err) || !num(stopMHz, tr("Stop (MHz)"), 0.000001, 1e7, b, err) || !num(stepKHz, tr("Step (kHz)"), 0.001, 1e6, st, err) ||
        !num(marginDb, tr("Margin"), 0.0, 60.0, m, err) || !num(fixedDb, tr("Level"), -160.0, 20.0, f, err)) { return bad(err); }
    if (b < a) { return bad(tr("the stop is below the start")); }
    return good({{"enabled", enabled}, {"startHz", a * 1e6}, {"stopHz", b * 1e6}, {"stepHz", st * 1e3}, {"mode", mode == 1 ? "fixed" : "relative"}, {"marginDb", m}, {"fixedDb", f}, {"merge", merge}});
}

Built bandMemoryJson(bool enabled, bool volume, bool agc) {
    json fields = json::array();
    if (volume) { fields.push_back("volume"); }
    if (agc) { fields.push_back("agc"); }
    return good({{"enabled", enabled}, {"fields", fields}});
}

Built recordDailyJson(int style, bool utc) {
    if (style < 0 || style > 4) { return bad(tr("choose one of the five folder styles")); }
    return good({{"style", style}, {"utc", utc}, {"template", ""}});
}

Built overlayLoadJson(const std::string& path, int unit, const std::string& freqColumn, const std::string& columnsText, int delimiter) {
    const std::string p = trim(path);
    if (p.empty()) { return bad(tr("type the full path of your CSV file")); }
    std::vector<std::string> cols;
    if (!splitList(columnsText, 16, 64, cols) || cols.empty()) { return bad(tr("name one to sixteen columns, separated by commas")); }
    const std::string fc = trim(freqColumn);
    if (fc.empty()) { return bad(tr("name the frequency column")); }
    static const char* const units[3] = {"Hz", "kHz", "MHz"};
    static const char* const delims[3] = {",", ";", "\t"};
    return good({{"path", p}, {"config", {{"version", 1}, {"freqColumn", fc}, {"unit", units[std::clamp(unit, 0, 2)]}, {"columns", cols}, {"delimiter", delims[std::clamp(delimiter, 0, 2)]}}}});
}

Built packExportJson(const std::string& path, const std::string& name, const std::string& description) {
    const std::string p = trim(path), n = trim(name);
    if (p.empty()) { return bad(tr("type the full path of the file to write")); }
    if (n.empty() || n.size() > 64) { return bad(tr("the pack's name is 1 to 64 characters")); }
    return good({{"path", p}, {"name", n}, {"description", trim(description)}});
}

Built favouriteSaveJson(const std::string& name) {
    const std::string n = trim(name);
    if (n.empty() || n.size() > 64) { return bad(tr("the name is 1 to 64 characters")); }
    return good({{"name", n}});
}

Built bookmarkNoteJson(const std::string& note, const std::string& tagsText) {
    std::vector<std::string> tags;
    if (!splitList(tagsText, 16, 32, tags)) { return bad(tr("a tag is at most 32 characters")); }
    if (note.size() > 2000) { return bad(tr("a note is at most 2000 bytes")); }
    return good({{"note", note}, {"tags", tags}});
}

Built bookmarkFieldsJson(const std::string& squelchDb, const std::string& thresholdDb, const std::string& offsetKhz, const std::string& toneHz) {
    json j = json::object();
    double v = 0;
    std::string err;
    if (!trim(squelchDb).empty()) { if (!num(squelchDb, tr("Squelch"), -200.0, 20.0, v, err)) { return bad(err); } j["squelchDb"] = v; }
    if (!trim(thresholdDb).empty()) { if (!num(thresholdDb, tr("Threshold"), -200.0, 50.0, v, err)) { return bad(err); } j["thresholdDb"] = v; }
    if (!trim(offsetKhz).empty()) { if (!num(offsetKhz, tr("Offset"), -1000000.0, 1000000.0, v, err)) { return bad(err); } j["offsetKhz"] = v; }
    if (!trim(toneHz).empty()) { if (!num(toneHz, tr("Tone"), 0.001, 3000.0, v, err)) { return bad(err); } j["toneHz"] = v; }
    return good(j);
}

Built recordingsListJson(const std::string& textFilter, int sort) {
    static const char* const sorts[8] = {"name", "modified", "start", "duration", "size", "rate", "center", "kind"};
    return good({{"sort", sorts[std::clamp(sort, 0, 7)]}, {"ascending", true}, {"text", trim(textFilter)}, {"iq", true}, {"audio", true}, {"hideBroken", false}, {"offset", 0}, {"limit", 50}});
}

Built sigmeasJson(const std::string& durationS, const std::string& endAgoS) {
    double d = 0, e = 0;
    std::string err;
    if (!num(durationS, tr("Length (s)"), 0.001, 10.0, d, err) || !num(endAgoS, tr("Ends (s before now)"), 0.0, 60.0, e, err)) { return bad(err); }
    return good({{"duration_s", d}, {"end_ago_s", e}});
}

Built pseudoDopplerJson(const std::string& samplesPerDwell, const std::string& durationS) {
    long long n = 0;
    double d = 0;
    std::string err;
    if (!integer(samplesPerDwell, tr("Samples per antenna"), 8, 1000000, n, err) || !num(durationS, tr("Length (s)"), 0.001, 10.0, d, err)) { return bad(err); }
    return good({{"samples_per_dwell", n}, {"duration_s", d}});
}

Built maskConfigJson(bool enabled, const std::string& raiseFrames, const std::string& clearFrames, bool capture, const std::string& postRollS, const std::string& minIntervalS) {
    long long r = 0, c = 0;
    double p = 0, m = 0;
    std::string err;
    if (!integer(raiseFrames, tr("Raise after (frames)"), 1, 100, r, err) || !integer(clearFrames, tr("Clear after (frames)"), 1, 100, c, err) || !num(postRollS, tr("Keep recording after (s)"), 0.0, 60.0, p, err) ||
        !num(minIntervalS, tr("Least time between captures (s)"), 0.0, 3600.0, m, err)) { return bad(err); }
    return good({{"enabled", enabled}, {"raise_frames", r}, {"clear_frames", c}, {"capture", capture}, {"post_roll_s", p}, {"min_interval_s", m}});
}

Built sweepOpenJson(int kind, const std::string& port, const std::string& baud, const std::string& startMHz, const std::string& stopMHz, const std::string& points, const std::string& intervalMs,
                    const std::string& stepMHz, const std::string& lnaDb, const std::string& vgaDb, bool amp, const std::string& topDbm, const std::string& bottomDbm) {
    std::string err;
    double a = 0, b = 0;
    json j = json::object();
    if (kind == 2) {   // HackRF: whole MHz, no port
        if (!num(startMHz, tr("Start (MHz)"), 1.0, 6000.0, a, err) || !num(stopMHz, tr("Stop (MHz)"), 1.0, 6000.0, b, err)) { return bad(err); }
        if (a != std::floor(a) || b != std::floor(b)) { return bad(tr("the HackRF sweeps whole megahertz: type whole numbers")); }
        double st = 0, lna = 0, vga = 0;
        if (!num(stepMHz, tr("Step (MHz)"), 1.0, 20.0, st, err) || !num(lnaDb, tr("LNA gain (dB)"), 0.0, 40.0, lna, err) || !num(vgaDb, tr("VGA gain (dB)"), 0.0, 62.0, vga, err)) { return bad(err); }
        j = {{"kind", "hackrf"}, {"start_mhz", a}, {"stop_mhz", b}, {"step_mhz", st}, {"lna_db", lna}, {"vga_db", vga}, {"amp", amp}};
    } else {
        std::string p;
        if (!validComPort(port, p)) { return bad(tr("the port is COM1 to COM256")); }
        if (kind == 3) {   // RF Explorer
            long long bd = 0;
            double top = 0, bot = 0;
            if (!integer(baud, tr("Baud"), 1200, 500000, bd, err)) { return bad(err); }
            static const long long ok[9] = {500000, 115200, 57600, 38400, 19200, 9600, 4800, 2400, 1200};
            if (std::find(std::begin(ok), std::end(ok), bd) == std::end(ok)) { return bad(tr("the baud rate is one of 500000, 115200, 57600, 38400, 19200, 9600, 4800, 2400, 1200")); }
            if (!num(startMHz, tr("Start (MHz)"), 0.001, 1e7, a, err) || !num(stopMHz, tr("Stop (MHz)"), 0.001, 1e7, b, err) || !num(topDbm, tr("Top (dBm)"), -500.0, 0.0, top, err) || !num(bottomDbm, tr("Bottom (dBm)"), -500.0, 0.0, bot, err)) { return bad(err); }
            if (!(bot < top)) { return bad(tr("the bottom must be below the top")); }
            j = {{"kind", "rfexplorer"}, {"port", p}, {"baud", bd}, {"start_mhz", a}, {"stop_mhz", b}, {"top_dbm", top}, {"bottom_dbm", bot}};
        } else {
            long long pts = 0, iv = 0;
            if (!num(startMHz, tr("Start (MHz)"), 0.0, 1e7, a, err) || !num(stopMHz, tr("Stop (MHz)"), 0.0, 1e7, b, err) || !integer(points, tr("Points"), 2, 1048576, pts, err) || !integer(intervalMs, tr("Interval (ms)"), 0, 60000, iv, err)) { return bad(err); }
            if (!(b > a)) { return bad(tr("the stop must be above the start")); }
            j = {{"kind", kind == 1 ? "tinysa_ultra" : "tinysa"}, {"port", p}, {"start_mhz", a}, {"stop_mhz", b}, {"points", pts}, {"interval_ms", iv}};
        }
    }
    return good(j);
}

Built sweepHeatmapJson(const std::string& cols, const std::string& rows, int freqMode, bool occupancyOn, const std::string& startMHz, const std::string& stopMHz, const std::string& stepKHz,
                       const std::string& marginDb, const std::string& floorPercentile, const std::string& fixedFloorDb) {
    long long c = 0, r = 0;
    std::string err;
    if (!integer(cols, tr("Columns"), 1, 65536, c, err) || !integer(rows, tr("Rows"), 2, 65536, r, err)) { return bad(err); }
    json j = {{"heatmap", {{"cols", c}, {"rows", r}, {"freq_mode", freqMode == 1 ? "mean" : "max"}}}};
    if (!occupancyOn) { j["occupancy"] = nullptr; return good(j); }
    double a = 0, b = 0, st = 0, m = 0, fp = 0, ff = 0;
    if (!num(startMHz, tr("Start (MHz)"), 0.0, 1e7, a, err) || !num(stopMHz, tr("Stop (MHz)"), 0.0, 1e7, b, err) || !num(stepKHz, tr("Step (kHz)"), 0.001, 1e6, st, err) || !num(marginDb, tr("Margin"), 0.0, 200.0, m, err) ||
        !num(floorPercentile, tr("Floor percentile"), 0.0, 0.5, fp, err) || !num(fixedFloorDb, tr("Fixed floor (dB)"), -300.0, 100.0, ff, err)) { return bad(err); }
    if (!(b > a)) { return bad(tr("the stop must be above the start")); }
    j["occupancy"] = {{"start_mhz", a}, {"stop_mhz", b}, {"step_khz", st}, {"margin_db", m}, {"floor_percentile", fp}, {"fixed_floor_db", ff}};
    return good(j);
}

Built iqOutJson(int format, bool header, bool udp, const std::string& port) {
    long long p = 0;
    std::string err;
    if (!integer(port, tr("Port"), udp ? 1 : 0, 65535, p, err)) { return bad(err); }
    static const char* const formats[3] = {"u8", "s16", "f32"};
    json j = {{"format", formats[std::clamp(format, 0, 2)]}, {"header", header}, {"udp", udp}, {"port", p}};
    if (udp) { j["dest"] = "127.0.0.1"; }
    return good(j);
}

Built sbsOutJson(const std::string& port) {
    long long p = 0;
    std::string err;
    if (!integer(port, tr("Port"), 0, 65535, p, err)) { return bad(err); }
    return good({{"port", p}});
}

Built tracksConfigJson(bool enabled, const std::string& retentionS) {
    long long r = 0;
    std::string err;
    if (!integer(retentionS, tr("Keep for (s)"), 60, 2592000, r, err)) { return bad(err); }
    return good({{"enabled", enabled}, {"retention_s", r}});
}

Built coverageConfigJson(bool enabled, const std::string& rxLat, const std::string& rxLon, const std::string& cellDeg, const std::string& halfLifeS) {
    double la = 0, lo = 0, cell = 0, hl = 0;
    std::string err;
    if (!num(cellDeg, tr("Cell size (degrees)"), 0.001, 1.0, cell, err) || !num(halfLifeS, tr("Half life (s)"), 3600.0, 31536000.0, hl, err)) { return bad(err); }
    json j = {{"enabled", enabled}, {"cell_deg", cell}, {"half_life_s", hl}};
    if (enabled) {   // the receiver's position is the person's own typing: nothing reads the GPS or the machine's location
        if (!num(rxLat, tr("Your latitude"), -90.0, 90.0, la, err) || !num(rxLon, tr("Your longitude"), -180.0, 180.0, lo, err)) { return bad(err); }
        j["rx_lat"] = la;
        j["rx_lon"] = lo;
    }
    return good(j);
}

Built qsoJson(const std::string& unixTime, const std::string& kHz, const std::string& mode, const std::string& call, const std::string& myCall, const std::string& sent, const std::string& received) {
    double f = 0;
    long long t = 0;
    std::string err;
    if (!num(kHz, tr("Frequency (kHz)"), 0.001, 1e9, f, err)) { return bad(err); }
    if (!trim(unixTime).empty() && !integer(unixTime, tr("Time (unix seconds)"), 0, 4102444800LL, t, err)) { return bad(err); }
    const std::string m = trim(mode), c = trim(call);
    if (m.empty()) { return bad(tr("type the mode (CW, SSB, FT8 ...)")); }
    if (c.empty()) { return bad(tr("type the other station's call sign")); }
    std::vector<std::string> s, r;
    if (!splitList(sent, 8, 32, s) || !splitList(received, 8, 32, r)) { return bad(tr("an exchange value is too long")); }
    json j = {{"f", f}, {"m", m}, {"c", c}, {"s", s}, {"r", r}};
    if (t > 0) { j["t"] = t; }
    if (!trim(myCall).empty()) { j["me"] = trim(myCall); }
    return good(j);
}

Built cabrilloHeaderJson(const std::string& contest, const std::string& callsign, const std::string& location, const std::string& name, const std::string& addressLines, const std::string& operators, const std::string& soapbox) {
    const std::string c = trim(contest);
    if (c.empty()) { return bad(tr("type the contest name (for example CQ-WW-CW)")); }
    std::vector<std::string> addr, ops, soap;
    splitList(addressLines, 8, 200, addr);
    splitList(operators, 64, 40, ops);
    splitList(soapbox, 16, 200, soap);
    json j = {{"contest", c}, {"callsign", trim(callsign)}, {"location", trim(location)}, {"name", trim(name)}, {"address", addr}, {"operators", ops}, {"soapbox", soap}};
    return good(j);
}

Built neededCheckJson(const std::string& id, const std::string& path) {
    json j = json::object();
    if (!trim(id).empty()) { j["id"] = trim(id); }
    if (!trim(path).empty()) { j["path"] = trim(path); }
    return good(j);
}

Built exportBoxJson(const std::string& input, const std::string& output, const std::string& t0, const std::string& t1, const std::string& loHz, const std::string& hiHz, int mode, int format) {
    double a = 0, b = 0, lo = 0, hi = 0;
    std::string err;
    if (trim(input).empty() || trim(output).empty()) { return bad(tr("type the full paths of the recording and of the new file")); }
    if (!num(t0, tr("From (s)"), 0.0, 1e7, a, err) || !num(t1, tr("To (s)"), 0.0, 1e7, b, err) || !num(loHz, tr("Lowest frequency (Hz)"), 0.0, 1e13, lo, err) || !num(hiHz, tr("Highest frequency (Hz)"), 0.0, 1e13, hi, err)) { return bad(err); }
    if (!(b > a) || !(hi > lo)) { return bad(tr("the box needs a later end than start and a higher top than bottom")); }
    static const char* const modes[5] = {"iq", "am", "fm", "usb", "lsb"};
    static const char* const formats[3] = {"float32", "pcm16", "cf32"};
    return good({{"input", trim(input)}, {"output", trim(output)}, {"t0", a}, {"t1", b}, {"loHz", lo}, {"hiHz", hi}, {"mode", modes[std::clamp(mode, 0, 4)]}, {"format", formats[std::clamp(format, 0, 2)]}});
}

const std::vector<std::string>& channelPlanIds() {
    static const std::vector<std::string> v = {"pmr446-analogue", "pmr446-digital-12k5", "pmr446-digital-6k25", "cb-cept-40", "frs-us", "gmrs-us", "murs-us", "marine-vhf-itu"};
    return v;
}
const std::vector<std::string>& sourceKinds() {
    static const std::vector<std::string> v = {"rfspace", "hpsdr1", "hpsdr2", "aaronia", "spyserver", "soapyremote", "sdrconnect", "icr8600", "funcube", "g39ddc", "g33ddc", "sdriq"};
    return v;
}
const std::vector<std::string>& sourceKindNames() {
    static const std::vector<std::string> v = {"RFspace NetSDR / SDR-IP / Afedri", "HPSDR protocol 1 (including Hermes-Lite 2)", "HPSDR protocol 2", "Aaronia RTSA", "SpyServer", "SoapyRemote", "SDRconnect",
                                               "Icom IC-R8600", "FUNcube Dongle Pro / Pro+", "WiNRADiO G39DDC", "WiNRADiO G33DDC", "RFspace SDR-IQ (USB; needs FTDI's D2XX driver)"};
    return v;
}

Built openRadioId(int kindIndex, const std::string& arguments) {
    const auto& k = sourceKinds();
    if (kindIndex < 0 || kindIndex >= static_cast<int>(k.size())) { return bad(tr("choose a source")); }
    const std::string a = trim(arguments);
    if (a.size() > 200) { return bad(tr("the arguments are too long")); }
    for (const char c : a) { if (static_cast<unsigned char>(c) < 0x20) { return bad(tr("the arguments hold a control character")); } }
    Built b;
    b.ok = true;
    b.text = "open-" + k[static_cast<std::size_t>(kindIndex)] + ":" + a;
    return b;
}

// ============================================================ reading results ============================================================
namespace {
void flatten(const json& j, const std::string& prefix, std::vector<std::pair<std::string, std::string>>& out, std::size_t maxLines) {
    if (out.size() >= maxLines) { return; }
    if (j.is_object()) {
        for (auto it = j.begin(); it != j.end(); ++it) {
            flatten(it.value(), prefix.empty() ? it.key() : prefix + "." + it.key(), out, maxLines);
            if (out.size() >= maxLines) { return; }
        }
    } else if (j.is_array()) {
        if (j.empty()) { out.emplace_back(prefix, "[]"); return; }
        if (j.size() <= 4 && std::all_of(j.begin(), j.end(), [](const json& e) { return !e.is_structured(); })) {
            std::string s;
            for (const auto& e : j) { if (!s.empty()) { s += ", "; } s += e.is_string() ? e.get<std::string>() : dumpOf(e); }
            out.emplace_back(prefix, s);
            return;
        }
        out.emplace_back(prefix, std::to_string(j.size()) + " items");
        for (std::size_t i = 0; i < j.size() && i < 3; ++i) { flatten(j[i], prefix + "[" + std::to_string(i + 1) + "]", out, maxLines); if (out.size() >= maxLines) { return; } }
    } else if (j.is_string()) {
        out.emplace_back(prefix, j.get<std::string>());
    } else if (j.is_number_float()) {
        char b[48];
        std::snprintf(b, sizeof(b), "%.6g", j.get<double>());
        out.emplace_back(prefix, b);
    } else {
        out.emplace_back(prefix, dumpOf(j));
    }
}
}  // namespace

std::vector<std::pair<std::string, std::string>> flattenJson(const std::string& text, std::size_t maxLines) {
    std::vector<std::pair<std::string, std::string>> out;
    const std::string t = trim(text);
    if (t.empty()) { return out; }
    const json j = json::parse(t, nullptr, false);
    if (j.is_discarded() || !(j.is_object() || j.is_array())) { out.emplace_back("", t.size() > 300 ? t.substr(0, 300) : t); return out; }
    flatten(j, "", out, maxLines);
    return out;
}

std::string jsonString(const std::string& text, const char* key) {
    const json j = json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object() || !j.contains(key) || !j[key].is_string()) { return std::string(); }
    return j[key].get<std::string>();
}
double jsonNumber(const std::string& text, const char* key, double def) {
    const json j = json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object() || !j.contains(key) || !j[key].is_number()) { return def; }
    return j[key].get<double>();
}
bool jsonBool(const std::string& text, const char* key, bool def) {
    const json j = json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object() || !j.contains(key) || !j[key].is_boolean()) { return def; }
    return j[key].get<bool>();
}

bool statusIsRefusal(int status) { return status != FOXAPI_OK && status != FOXAPI_NO_CHANGE; }

std::string refusalText(int status, const std::string& engineMessage) {
    std::string head;
    switch (status) {
        case FOXAPI_OK: return std::string();
        case FOXAPI_NO_CHANGE: head = tr("Nothing changed"); break;
        case FOXAPI_BUSY: head = tr("Busy"); break;
        default: head = tr("Refused"); break;
    }
    const std::string m = trim(engineMessage);
    return m.empty() ? head : head + ": " + m;
}

// ============================================================ needed libraries ============================================================
namespace {
std::string str(const json& j, const char* k) { return j.contains(k) && j[k].is_string() ? j[k].get<std::string>() : std::string(); }
std::vector<std::string> strs(const json& j, const char* k) {
    std::vector<std::string> v;
    if (j.contains(k) && j[k].is_array()) { for (const auto& e : j[k]) { if (e.is_string()) { v.push_back(e.get<std::string>()); } } }
    return v;
}
}  // namespace

std::vector<NeededRow> parseNeeded(const std::string& text) {
    std::vector<NeededRow> out;
    const json j = json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_array()) { return out; }
    for (const auto& e : j) {
        if (!e.is_object()) { continue; }
        NeededRow r;
        r.id = str(e, "library_id");
        r.sourceKind = str(e, "source_kind");
        r.name = str(e, "name");
        r.kind = str(e, "kind");
        r.why = str(e, "why");
        r.publisher = str(e, "publisher");
        r.officialPage = str(e, "official_page");
        r.pageStatus = str(e, "page_status");
        r.installKind = str(e, "install_kind");
        r.folderNote = str(e, "folder_note");
        r.licence = str(e, "licence");
        r.notSupplied = str(e, "not_supplied");
        r.fetchAllowed = str(e, "fetch_allowed");
        r.minVersionNote = str(e, "min_version_note");
        r.fileNames = strs(e, "file_names");
        r.steps = strs(e, "steps");
        r.bits64 = !e.contains("bits64") || !e["bits64"].is_boolean() || e["bits64"].get<bool>();
        if (e.contains("check") && e["check"].is_object()) {
            const json& c = e["check"];
            r.checked = true;
            r.state = str(c, "state");
            r.sentence = str(c, "sentence");
            r.depth = str(c, "depth");
            r.version = str(c, "version");
            r.foundIn = str(c, "found_in");
            r.file = str(c, "file");
            r.missing = strs(c, "missing");
        }
        if (!r.id.empty()) { out.push_back(std::move(r)); }
    }
    return out;
}

std::string neededStateWords(const NeededRow& r) {
    if (!r.checked || r.state.empty()) { return tr("Not checked yet"); }
    if (r.state == "working") { return r.depth == "loaded" ? tr("Found and working") : tr("Found (not test-loaded)"); }
    if (r.state == "not-found") { return tr("Not found"); }
    if (r.state == "too-old") { return tr("Found, but too old"); }
    if (r.state == "wrong-32-bit") { return tr("Found, but it is 32-bit (a 64-bit one is needed)"); }
    if (r.state == "would-not-load") { return tr("Found, but it would not load"); }
    if (r.state == "not-a-library") { return tr("Found, but it is not a library"); }
    if (r.state == "cannot-check") { return tr("Cannot be checked from here"); }
    return tr("Not checked yet");
}

int neededStateTone(const NeededRow& r) {
    if (!r.checked) { return kDim; }
    if (r.state == "working") { return r.depth == "loaded" ? kGood : kDim; }
    if (r.state == "cannot-check" || r.state.empty()) { return kDim; }
    return kWarn;
}

bool neededTakesFile(const NeededRow& r) { return r.kind == "library-file" && r.installKind == "copy-in"; }

void neededRows(const NeededRow& r, const std::string& draftPath, std::vector<Row>& out) {
    out.push_back(rHeading(r.name));
    if (!r.why.empty()) { out.push_back(rNote(r.why)); }
    if (!r.fileNames.empty()) {
        std::string f;
        for (const auto& n : r.fileNames) { if (!f.empty()) { f += ", "; } f += n; }
        out.push_back(rReadout(tr("Looks for"), f));
    }
    out.push_back(rReadout(tr("State"), neededStateWords(r)));
    if (r.checked && !r.sentence.empty()) { out.push_back(rNote(r.sentence, neededStateTone(r))); }
    if (r.checked && !r.version.empty()) { out.push_back(rReadout(tr("Version"), r.version)); }
    if (!r.officialPage.empty()) {   // shown as TEXT, never a link; an address nobody read in the publisher's own documentation says so
        out.push_back(rReadout(tr("Where to get it"), r.pageStatus == "verified" ? r.officialPage : r.officialPage + " " + tr("(unverified)")));
    } else {
        out.push_back(rReadout(tr("Where to get it"), r.publisher.empty() ? tr("from its maker (no address is known here)") : r.publisher));
    }
    for (std::size_t i = 0; i < r.steps.size(); ++i) { out.push_back(rNote(std::to_string(i + 1) + ". " + r.steps[i])); }
    if (!r.licence.empty()) { out.push_back(rNote(r.licence)); }
    out.push_back(rNote(tr("FoxSDR does not include or supply it.")));
    out.push_back(rKey("l.check-" + r.id, std::string(tr("Check again")) + ": " + r.name, true, tr("Looks again now, without a restart. A file is found by reading its headers; nothing is loaded, downloaded or opened.")));
    if (neededTakesFile(r)) {
        out.push_back(rField("l.path-" + r.id, std::string(tr("Path of the file you downloaded")) + ": " + r.name, draftPath, tr("The full path of the library file; it is judged where it is and never copied")));
        out.push_back(rKey("l.checkfile-" + r.id, std::string(tr("Check this file")) + ": " + r.name, !trim(draftPath).empty()));
    }
}

}  // namespace foxsdr::slintlanel
