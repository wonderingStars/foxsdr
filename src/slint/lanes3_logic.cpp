// lanes3_logic.cpp - see lanes3_logic.hpp.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "lanes3_logic.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <nlohmann/json.hpp>

#include "core/civil_time.hpp"
#include "core/i18n.hpp"
#include "core/raw_file_id.hpp"
#include "core/rec_naming.hpp"
#include "core/schedule.hpp"

namespace foxsdr::slintlanes3 {

namespace {
using nlohmann::json;
namespace sll = foxsdr::slintlanel;
namespace rn = cascade::core::recname;
using sll::numberText;
using sll::parseNumber;
using sll::trim;

std::string tr(const char* s) { return cascade::i18n::tr(s); }

std::string dumpJson(const json& j) { return j.dump(-1, ' ', false, json::error_handler_t::replace); }

std::string pad2(int v) { char b[16]; std::snprintf(b, sizeof(b), "%02d", v); return b; }
}  // namespace

// ============================================================ file names (G3-08) ============================================================
NameCheck checkNameTemplate(const std::string& text) {
    NameCheck r;
    rn::Fields sample;
    sample.unixMs = 1'793'787'000'000LL;   // a Thursday in November 2026, 10:10:00 UTC
    sample.frequencyHz = 100.5e6;
    sample.mode = "WFM";
    sample.radio = "Radio 1";
    sample.bookmark = "unknown";
    sample.counter = 1;
    const std::string t = trim(text);
    if (t.empty()) {
        r.ok = true;
        r.example = "iq_20261104_101000_2000000Hz.wav";
        return r;
    }
    const rn::Expanded e = rn::expand(t, sample, false);
    if (!e.ok) { r.error = e.error; return r; }
    r.ok = true;
    r.example = e.relPath + ".wav";
    return r;
}

std::string nameTokensHelp() {
    return tr("Tokens: {date} {time} {datetime} {year} {month} {day} {hour} {min} {sec} {ms} {tz} {freq} {freq_khz} {freq_mhz} {mode} {radio} {counter} {counter:3}. A slash starts a folder (up to three). The extension is added. Times are local.");
}

// ============================================================ sample formats (G3-07) ============================================================
const std::vector<std::string>& iqFormatNames() {
    static const std::vector<std::string> v = {tr("32-bit float"), tr("16-bit integer")};
    return v;
}
const std::vector<std::string>& audioFormatNames() {
    static const std::vector<std::string> v = {tr("16-bit integer"), tr("24-bit integer"), tr("32-bit float")};
    return v;
}

std::string costPerSecond(bool iq, int format, double rateHz) {
    double bytesPerFrame = 0.0;
    if (iq) { bytesPerFrame = format == 1 ? 4.0 : 8.0; }
    else { bytesPerFrame = format == 1 ? 3.0 : (format == 2 ? 4.0 : 2.0); }
    const double perSecond = bytesPerFrame * rateHz;
    char b[64];
    if (!(perSecond > 0.0)) { return std::string(); }
    if (perSecond >= 1e6) { std::snprintf(b, sizeof(b), "%.1f MB", perSecond / 1e6); }
    else { std::snprintf(b, sizeof(b), "%.0f kB", perSecond / 1e3); }
    return std::string(b) + " " + tr("a second");
}

bool parseFormatFact(const std::string& fact, int& iq, int& audio) {
    int a = -1, b = -1;
    if (std::sscanf(fact.c_str(), "iq=%d;audio=%d", &a, &b) != 2) { return false; }
    if (a < 0 || a > 1 || b < 0 || b > 2) { return false; }
    iq = a;
    audio = b;
    return true;
}

// ============================================================ rollover (G3-10) ============================================================
bool parseRolloverMB(const std::string& text, double& mb, std::string& err) {
    if (trim(text).empty()) { mb = 0.0; return true; }
    double v = 0.0;
    if (!parseNumber(text, 0.0, 100000.0, v, err)) { return false; }
    if (v != 0.0 && v < 1.0) { err = tr("allowed: 0 (no limit) or 1 - 100000"); return false; }
    mb = v;
    return true;
}

bool parseRolloverMinutes(const std::string& text, double& minutes, std::string& err) {
    if (trim(text).empty()) { minutes = 0.0; return true; }
    double v = 0.0;
    if (!parseNumber(text, 0.0, 1440.0, v, err)) { return false; }
    if (v != 0.0 && v < 0.1) { err = tr("allowed: 0 (no limit) or 0.1 - 1440"); return false; }
    minutes = v;
    return true;
}

Built rolloverJson(const std::string& mbText, const std::string& minutesText, bool alignToClock) {
    Built b;
    double mb = 0.0, minutes = 0.0;
    if (!parseRolloverMB(mbText, mb, b.err)) { return b; }
    if (!parseRolloverMinutes(minutesText, minutes, b.err)) { return b; }
    if (alignToClock && minutes == 0.0) { b.err = tr("A file on the clock needs a time limit."); return b; }
    json j;
    j["maxMB"] = mb;
    j["maxSeconds"] = std::round(minutes * 60.0 * 1000.0) / 1000.0;
    j["align"] = alignToClock;
    b.ok = true;
    b.text = dumpJson(j);
    return b;
}

std::string rolloverReadout(const std::string& fact) {
    const json j = json::parse(fact, nullptr, false);
    if (j.is_discarded() || !j.is_object()) { return std::string(); }
    const double mb = j.value("maxMB", 0.0);
    const double s = j.value("maxSeconds", 0.0);
    const bool align = j.value("align", false);
    std::string out;
    if (mb > 0.0) { out += numberText(mb) + " MB"; }
    if (s > 0.0) {
        if (!out.empty()) { out += " " + tr("or") + " "; }
        const double m = s / 60.0;
        out += numberText(std::round(m * 100.0) / 100.0) + " " + tr("minutes");
        if (align) { out += ", " + tr("on the clock"); }
    }
    return out;
}

// ============================================================ clips and the replay buffer (G3-03, G3-09, G3-02) ============================================================
Built clipsJson(const ClipFields& f) {
    Built b;
    double pre = 0, tail = 0, minS = 0, mx = 0;
    if (!parseNumber(f.preRollS, 0.0, 10.0, pre, b.err)) { b.err = tr("Time before the transmission") + ": " + b.err; return b; }
    if (!parseNumber(f.tailS, 0.0, 30.0, tail, b.err)) { b.err = tr("Time after it ends") + ": " + b.err; return b; }
    if (!parseNumber(f.minS, 0.0, 60.0, minS, b.err)) { b.err = tr("Shortest clip kept") + ": " + b.err; return b; }
    if (!parseNumber(f.maxClipS, 10.0, 3600.0, mx, b.err)) { b.err = tr("Longest clip") + ": " + b.err; return b; }
    json j;
    j["enabled"] = f.enabled;
    j["preRollS"] = pre;
    j["tailS"] = tail;
    j["minS"] = minS;
    j["maxClipS"] = mx;
    b.ok = true;
    b.text = dumpJson(j);
    return b;
}

bool clipFieldsFrom(const std::string& fact, ClipFields& out) {
    const json j = json::parse(fact, nullptr, false);
    if (j.is_discarded() || !j.is_object() || !j.contains("preRollS")) { return false; }
    out.enabled = j.value("enabled", false);
    out.preRollS = numberText(j.value("preRollS", 2.0));
    out.tailS = numberText(j.value("tailS", 3.0));
    out.minS = numberText(j.value("minS", 1.0));
    out.maxClipS = numberText(j.value("maxClipS", 600.0));
    return true;
}

bool parseReplaySeconds(const std::string& text, double maxSeconds, double& s, std::string& err) {
    if (trim(text).empty()) { s = 0.0; return true; }
    return parseNumber(text, 0.0, maxSeconds, s, err);
}

std::string replayCost(double seconds, double audioRateHz) {
    if (!(seconds > 0.0)) { return tr("off"); }
    const double bytes = seconds * audioRateHz * 2.0;   // 16-bit audio
    char b[96];
    std::snprintf(b, sizeof(b), "%s s = %.1f MB", numberText(seconds).c_str(), bytes / 1e6);
    return b;
}

std::string replayHeldText(const std::string& fact) {
    // The engine's fact is "seconds=5;bytes=480000;max=11520000"; a person reads "5 s of audio held (0.5 MB)".
    double seconds = -1.0, bytes = -1.0;
    std::size_t at = 0;
    while (at < fact.size()) {
        std::size_t end = fact.find(';', at);
        if (end == std::string::npos) { end = fact.size(); }
        const std::string part = fact.substr(at, end - at);
        const std::size_t eq = part.find('=');
        if (eq != std::string::npos) {
            const std::string k = part.substr(0, eq);
            const double v = std::atof(part.c_str() + eq + 1);
            if (k == "seconds") { seconds = v; } else if (k == "bytes") { bytes = v; }
        }
        at = end + 1;
    }
    if (seconds < 0.0 || bytes < 0.0) { return std::string(); }
    if (!(seconds > 0.0)) { return tr("nothing held"); }
    char b[160];
    std::snprintf(b, sizeof(b), tr("%s s of audio held (%.1f MB)").c_str(), numberText(seconds).c_str(), bytes / 1e6);
    return b;
}

Built replayJson(const std::string& secondsText, double maxSeconds) {
    Built b;
    double s = 0.0;
    if (!parseReplaySeconds(secondsText, maxSeconds, s, b.err)) { return b; }
    json j;
    j["seconds"] = s;
    b.ok = true;
    b.text = dumpJson(j);
    return b;
}

// ============================================================ timed recordings (G3-01) ============================================================
namespace {
bool parseDate(const std::string& text, int& y, int& m, int& d, std::string& err) {
    const std::string t = trim(text);
    if (std::sscanf(t.c_str(), "%d-%d-%d", &y, &m, &d) != 3 || t.size() < 8 || t.size() > 10) { err = tr("type the date as 2026-11-17"); return false; }
    if (y < 2000 || y > 2200 || m < 1 || m > 12) { err = tr("that date does not exist"); return false; }
    static const int dim[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const int last = (m == 2 && cascade::core::civil::isLeap(y)) ? 29 : dim[m - 1];
    if (d < 1 || d > last) { err = tr("that date does not exist"); return false; }
    return true;
}
bool parseClock(const std::string& text, int& h, int& mi, std::string& err) {
    const std::string t = trim(text);
    if (std::sscanf(t.c_str(), "%d:%d", &h, &mi) != 2 || t.size() < 3 || t.size() > 5) { err = tr("type the time as 14:30"); return false; }
    if (h < 0 || h > 23 || mi < 0 || mi > 59) { err = tr("that time does not exist"); return false; }
    return true;
}
}  // namespace

bool localToUnix(const std::string& date, const std::string& time, const OffsetAt& offsetAt, std::int64_t& unix, std::string& err) {
    int y = 0, mo = 0, d = 0, h = 0, mi = 0;
    if (!parseDate(date, y, mo, d, err) || !parseClock(time, h, mi, err)) { return false; }
    const std::int64_t asUtcMs = cascade::core::civil::toUnixMs(y, mo, d, h, mi, 0);
    // the zone's offset at that wall-clock time: tried at both ends of the doubt (an hour either side), so a time inside a clock change is found
    const auto off = [&](std::int64_t guessMs) { return offsetAt ? offsetAt(guessMs) : 0; };
    const int o1 = off(asUtcMs - 24LL * 3600 * 1000);
    const int o2 = off(asUtcMs + 24LL * 3600 * 1000);
    std::int64_t best = 0;
    int found = 0;
    for (const int o : {o1, o2}) {
        const std::int64_t cand = asUtcMs - static_cast<std::int64_t>(o) * 60 * 1000;
        if (off(cand) == o) {
            if (found == 0 || cand < best) { best = cand; }
            ++found;
        }
    }
    if (found == 0) {
        err = tr("That time does not exist here: the clocks skip it that day.");
        return false;
    }
    unix = best / 1000;
    return true;
}

std::string unixToLocal(std::int64_t unix, const OffsetAt& offsetAt) {
    const int off = offsetAt ? offsetAt(unix * 1000) : 0;
    const cascade::core::civil::DateTime t = cascade::core::civil::fromUnixMs(unix * 1000, off);
    std::string s = std::to_string(t.year) + "-" + pad2(t.month) + "-" + pad2(t.day) + " " + pad2(t.hour) + ":" + pad2(t.minute);
    if (t.second != 0) { s += ":" + pad2(t.second); }
    return s;
}

void localDateTimeParts(std::int64_t unix, const OffsetAt& offsetAt, std::string& date, std::string& time) {
    const int off = offsetAt ? offsetAt(unix * 1000) : 0;
    const cascade::core::civil::DateTime t = cascade::core::civil::fromUnixMs(unix * 1000, off);
    date = std::to_string(t.year) + "-" + pad2(t.month) + "-" + pad2(t.day);
    time = pad2(t.hour) + ":" + pad2(t.minute);
}

Built scheduleJson(const ScheduleFields& f, const OffsetAt& offsetAt) {
    Built b;
    std::int64_t start = 0;
    if (!localToUnix(f.date, f.time, offsetAt, start, b.err)) { return b; }
    double minutes = 0.0;
    if (!parseNumber(f.lengthMin, 1.0 / 60.0, 1440.0, minutes, b.err)) { b.err = tr("Length (minutes)") + ": " + b.err; return b; }
    const long long lengthS = static_cast<long long>(std::llround(minutes * 60.0));
    if (lengthS < 1 || lengthS > 86400) { b.err = tr("Length (minutes)") + ": " + tr("allowed:") + " 0.02 - 1440"; return b; }
    if (!f.recordAudio && !f.recordIq) { b.err = tr("Tick audio, I/Q, or both."); return b; }
    if (trim(f.name).size() > 40) { b.err = tr("Name: at most 40 characters."); return b; }
    json j;
    j["name"] = trim(f.name);
    j["radio"] = std::clamp(f.radio, 1, 8);
    j["startUnix"] = start;
    j["lengthS"] = lengthS;
    j["repeat"] = f.repeat == 1 ? "daily" : (f.repeat == 2 ? "weekly" : "once");
    j["tune"] = f.tune;
    if (f.tune) {
        double mhz = 0.0;
        if (!parseNumber(f.freqMHz, 0.001, 6000.0, mhz, b.err)) { b.err = tr("Frequency (MHz)") + ": " + b.err; return b; }
        j["freqHz"] = std::round(mhz * 1e6);
        if (f.mode >= 1 && f.mode <= 7) { j["mode"] = f.mode; }
        if (!trim(f.bandwidthKHz).empty()) {
            double khz = 0.0;
            if (!parseNumber(f.bandwidthKHz, 0.1, 10000.0, khz, b.err)) { b.err = tr("Bandwidth (kHz)") + ": " + b.err; return b; }
            j["bandwidthHz"] = std::round(khz * 1e3);
        }
    }
    j["recordAudio"] = f.recordAudio;
    j["recordIq"] = f.recordIq;
    j["returnToWhereIWas"] = f.returnToWhereIWas;
    b.ok = true;
    b.text = dumpJson(j);
    return b;
}

std::string stateWords(const std::string& state) {
    if (state == "waiting") { return tr("waiting"); }
    if (state == "starting") { return tr("starting"); }
    if (state == "recording") { return tr("recording"); }
    if (state == "missed") { return tr("missed"); }
    if (state == "refused") { return tr("refused"); }
    if (state == "skipped") { return tr("skipped"); }
    if (state == "disabled") { return tr("switched off"); }
    if (state == "done") { return tr("done"); }
    return state;
}

EntryView entryView(const std::string& entryJson, const OffsetAt& offsetAt) {
    EntryView v;
    const json j = json::parse(entryJson, nullptr, false);
    if (j.is_discarded() || !j.is_object()) { return v; }
    v.id = j.value("id", 0);
    v.name = j.value("name", std::string());
    v.state = j.value("state", std::string("waiting"));
    v.note = j.value("note", std::string());
    v.enabled = j.value("enabled", true);
    v.missed = j.value("missed", 0);
    const long long start = j.value("startUnix", 0LL);
    const long long next = j.value("nextUnix", -1LL);
    const int lengthS = j.value("lengthS", 0);
    std::string rep = j.value("repeat", std::string("once"));
    rep = rep == "daily" ? tr("daily") : (rep == "weekly" ? tr("weekly") : tr("once"));
    std::string what;
    if (j.value("recordAudio", false)) { what = tr("audio"); }
    if (j.value("recordIq", false)) { what += (what.empty() ? "" : " + ") + std::string("I/Q"); }
    std::string line = tr("Radio") + " " + std::to_string(j.value("radio", 1)) + ", ";
    line += unixToLocal(next > 0 ? next : start, offsetAt);
    char len[48];
    if (lengthS % 60 == 0) { std::snprintf(len, sizeof(len), "%d min", lengthS / 60); }
    else { std::snprintf(len, sizeof(len), "%.1f min", lengthS / 60.0); }
    line += std::string(", ") + len + ", " + rep;
    if (j.value("tune", false)) {
        char f[48];
        std::snprintf(f, sizeof(f), "%s MHz", numberText(j.value("freqHz", 0.0) / 1e6).c_str());
        line += std::string(", ") + f;
    }
    if (!what.empty()) { line += ", " + what; }
    line += ", " + stateWords(v.state);
    if (v.missed > 0) { line += " (" + std::to_string(v.missed) + " " + tr("missed") + ")"; }
    v.line = line;
    v.ok = v.id > 0;
    return v;
}

// ============================================================ playback (G3-13, G3-14a) ============================================================
bool parsePosition(const std::string& text, double lengthS, double& seconds, std::string& err) {
    const std::string t = trim(text);
    if (t.empty()) { err = tr("type a place in the recording"); return false; }
    double total = 0.0;
    if (t.find(':') == std::string::npos) {
        if (!parseNumber(t, 0.0, std::max(lengthS, 0.0), total, err)) { return false; }
        seconds = total;
        return true;
    }
    // h:mm:ss or m:ss, the last part may have a fraction
    std::vector<std::string> parts;
    std::size_t from = 0;
    while (true) {
        const std::size_t c = t.find(':', from);
        parts.push_back(t.substr(from, c == std::string::npos ? std::string::npos : c - from));
        if (c == std::string::npos) { break; }
        from = c + 1;
    }
    if (parts.size() > 3) { err = tr("type seconds, or minutes:seconds"); return false; }
    for (std::size_t i = 0; i < parts.size(); ++i) {
        double v = 0.0;
        std::string e;
        const bool last = i + 1 == parts.size();
        if (parts[i].empty() || !parseNumber(parts[i], 0.0, 1e9, v, e) || (!last && v != std::floor(v))) { err = tr("type seconds, or minutes:seconds"); return false; }
        total = total * 60.0 + v;
    }
    if (total > lengthS) { err = std::string(tr("allowed:")) + " 0 - " + positionText(lengthS); return false; }
    seconds = total;
    return true;
}

std::string positionText(double seconds) {
    if (!(seconds >= 0.0)) { seconds = 0.0; }
    const long long whole = static_cast<long long>(seconds);
    const double frac = seconds - static_cast<double>(whole);
    const long long h = whole / 3600, m = (whole / 60) % 60, s = whole % 60;
    char b[48];
    const int tenths = static_cast<int>(frac * 10.0);
    if (h > 0) { std::snprintf(b, sizeof(b), "%lld:%02lld:%02lld", h, m, s); }
    else if (tenths > 0) { std::snprintf(b, sizeof(b), "%lld:%02lld.%d", m, s, tenths); }
    else { std::snprintf(b, sizeof(b), "%lld:%02lld", m, s); }
    return b;
}

bool parseLoop(const std::string& aText, const std::string& bText, double lengthS, double& a, double& b, std::string& err) {
    double x = 0.0, y = 0.0;
    if (!parsePosition(aText, lengthS, x, err)) { err = tr("Loop start") + ": " + err; return false; }
    if (!parsePosition(bText, lengthS, y, err)) { err = tr("Loop end") + ": " + err; return false; }
    if (!(x < y)) { err = tr("The loop must start before it ends."); return false; }
    a = x;
    b = y;
    return true;
}

const std::vector<std::string>& rawFormatNames() {
    static const std::vector<std::string> v = {tr("8-bit unsigned (cu8)"), tr("16-bit signed (cs16)"), tr("32-bit float (cf32)")};
    return v;
}

namespace {
bool parseWithUnit(const std::string& text, double lo, double hi, double plainScale, double& out, std::string& err) {
    std::string t;
    for (const char c : trim(text)) { if (c != ' ' && c != '_' && c != ',' && c != '\'') { t.push_back(c); } }
    if (t.empty()) { err = tr("type a number"); return false; }
    double scale = plainScale;
    const char last = t.back();
    if (last == 'k' || last == 'K') { scale = 1e3; t.pop_back(); }
    else if (last == 'M' || last == 'm') { scale = 1e6; t.pop_back(); }
    else if (last == 'G' || last == 'g') { scale = 1e9; t.pop_back(); }
    double v = 0.0;
    std::string e;
    if (!parseNumber(t, 0.0, 1e15, v, e)) { err = e; return false; }
    const double hz = v * scale;
    if (hz < lo || hz > hi) { err = std::string(tr("allowed:")) + " " + numberText(lo) + " - " + numberText(hi) + " Hz"; return false; }
    out = hz;
    return true;
}
}  // namespace

bool parseRate(const std::string& text, double& hz, std::string& err) { return parseWithUnit(text, 1000.0, 1e9, 1.0, hz, err); }

bool parseCentre(const std::string& text, double& hz, std::string& err) {
    if (trim(text).empty()) { hz = 0.0; return true; }
    // a bare number with a decimal point or under 100000 is MHz; whole hertz are typed with their unit or as a long number
    std::string t = trim(text);
    const bool unit = !t.empty() && std::strchr("kKMmGg", t.back()) != nullptr;
    double v = 0.0;
    std::string e;
    if (!unit) {
        if (!parseNumber(t, 0.0, 1e13, v, e)) { err = e; return false; }
        hz = (v < 100000.0) ? v * 1e6 : v;
        return true;
    }
    return parseWithUnit(text, 0.0, 1e13, 1.0, hz, err);
}

Built rawFileId(const std::string& path, int formatIndex, const std::string& rateText, const std::string& centreText) {
    Built b;
    if (trim(path).empty()) { b.err = tr("type the full path of the file"); return b; }
    if (formatIndex < 0 || formatIndex > 2) { b.err = tr("choose the sample format"); return b; }
    double rate = 0.0, centre = 0.0;
    if (!parseRate(rateText, rate, b.err)) { b.err = tr("Sample rate") + ": " + b.err; return b; }
    if (!parseCentre(centreText, centre, b.err)) { b.err = tr("Centre frequency") + ": " + b.err; return b; }
    static const char* const names[3] = {"cu8", "cs16", "cf32"};
    char head[96];
    std::snprintf(head, sizeof(head), "file-raw:%s,%.0f,%.0f|", names[formatIndex], rate, centre);
    b.ok = true;
    b.text = std::string(head) + trim(path);
    return b;
}

bool parseRawFileId(const std::string& args, RawId& out) {
    cascade::core::RawFileId r;
    if (!cascade::core::parseRawFileId(args, r)) { return false; }
    out.format = r.format; out.path = r.path; out.rateHz = r.rateHz; out.centreHz = r.centreHz; out.hasCentre = r.hasCentre;
    return true;
}

// ============================================================ whole-file waterfall (G3-14b) ============================================================
const std::vector<std::string>& overviewFftNames() {
    static const std::vector<std::string> v = {"512", "1024", "2048", "4096"};
    return v;
}
int overviewFftSize(int index) { return 512 << std::clamp(index, 0, 3); }

Built overviewJson(const std::string& rowsText, int fftIndex) {
    Built b;
    long long rows = 0;
    if (!sll::parseInteger(rowsText, 50, 2000, rows, b.err)) { b.err = tr("Rows") + ": " + b.err; return b; }
    json j;
    j["action"] = "start";
    j["rows"] = rows;
    j["fft"] = overviewFftSize(fftIndex);
    b.ok = true;
    b.text = dumpJson(j);
    return b;
}

}  // namespace foxsdr::slintlanes3
