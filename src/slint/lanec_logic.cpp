// lanec_logic.cpp - see lanec_logic.hpp.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "lanec_logic.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <system_error>

#include <nlohmann/json.hpp>

#include "core/app_commands.hpp"

namespace foxsdr::slintlanec {

namespace {

using nlohmann::json;

std::string trim(const std::string& s) {
    std::size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a])) != 0) { ++a; }
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1])) != 0) { --b; }
    return s.substr(a, b - a);
}

std::optional<double> toNumber(const std::string& text) {
    const std::string t = trim(text);
    if (t.empty()) { return std::nullopt; }
    char* end = nullptr;
    const double v = std::strtod(t.c_str(), &end);
    if (end == t.c_str() || *end != '\0' || !std::isfinite(v)) { return std::nullopt; }
    return v;
}

std::string fmt(const char* f, double v) {   // one double into one of this unit's own literal formats (never a translated text)
    std::vector<char> b(64);
    const int n = std::snprintf(b.data(), b.size(), f, v);
    return n > 0 ? std::string(b.data(), static_cast<std::size_t>(std::min<int>(n, 63))) : std::string();
}

FoxCommand cmd(std::uint32_t op) {
    FoxCommand c{};
    c.structSize = sizeof(c);
    c.op = op;
    return c;
}

}  // namespace

std::string memoryKey(const std::string& name, double hz) {
    return name + "|" + std::to_string(static_cast<long long>(std::llround(hz)));
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------- Store

std::string Store::toJson() const {
    json j;
    j["v"] = 1;
    if (stepLock) { j["stepLock"] = true; }
    if (deltaMask != 0) { j["delta"] = deltaMask; }
    if (bwMemory) { j["bwMemory"] = true; }
    if (!bwByMode.empty()) { json o = json::object(); for (const auto& [m, bw] : bwByMode) { o[std::to_string(m)] = bw; } j["bwByMode"] = o; }
    if (!priorityHz.empty()) { j["priority"] = priorityHz; }
    if (priorityEveryS != 30.0) { j["priorityEvery"] = priorityEveryS; }
    if (muteScan) { j["muteScan"] = true; }
    if (logScan) { j["logScan"] = true; }
    if (clockMode != 1) { j["clock"] = clockMode; }
    json q = json::array();
    bool anyQuick = false;
    for (const QuickMemory& m : quick) {
        if (m.set) { anyQuick = true; q.push_back({{"hz", m.hz}, {"mode", m.mode}, {"bw", m.bandwidthHz}}); } else { q.push_back(nullptr); }
    }
    if (anyQuick) { j["quick"] = q; }
    if (!memories.empty()) {
        json o = json::object();
        for (const auto& [k, m] : memories) {
            if (m.empty()) { continue; }
            json e = json::object();
            if (!std::isnan(m.squelchDb)) { e["sq"] = m.squelchDb; }
            if (!std::isnan(m.thresholdDb)) { e["thr"] = m.thresholdDb; }
            if (m.offsetKhz != 0.0) { e["off"] = m.offsetKhz; }
            if (m.toneHz != 0.0) { e["tone"] = m.toneHz; }
            o[k] = e;
        }
        if (!o.empty()) { j["mem"] = o; }
    }
    return j.dump(2, ' ', false, json::error_handler_t::replace) + "\n";
}

Store Store::fromJson(const std::string& text) {
    Store s;
    json j = json::parse(text, nullptr, false);
    if (!j.is_object()) { return s; }
    const auto num = [&](const json& o, const char* k, double& out) { const auto it = o.find(k); if (it != o.end() && it->is_number() && std::isfinite(it->get<double>())) { out = it->get<double>(); } };
    if (const auto it = j.find("stepLock"); it != j.end() && it->is_boolean()) { s.stepLock = it->get<bool>(); }
    if (const auto it = j.find("delta"); it != j.end() && it->is_number_unsigned()) { s.deltaMask = it->get<std::uint32_t>() & 0xFFu; }
    if (const auto it = j.find("bwMemory"); it != j.end() && it->is_boolean()) { s.bwMemory = it->get<bool>(); }
    if (const auto it = j.find("bwByMode"); it != j.end() && it->is_object()) {
        for (const auto& [k, v] : it->items()) {
            const long m = std::strtol(k.c_str(), nullptr, 10);
            if (m >= 1 && m <= 8 && v.is_number() && std::isfinite(v.get<double>()) && v.get<double>() > 0.0) { s.bwByMode[static_cast<std::uint32_t>(m)] = v.get<double>(); }
        }
    }
    if (const auto it = j.find("priority"); it != j.end() && it->is_array()) {
        for (const auto& v : *it) { if (v.is_number() && std::isfinite(v.get<double>()) && v.get<double>() > 0.0 && s.priorityHz.size() < 16) { s.priorityHz.push_back(v.get<double>()); } }
    }
    num(j, "priorityEvery", s.priorityEveryS);
    s.priorityEveryS = std::clamp(s.priorityEveryS, 5.0, 600.0);
    if (const auto it = j.find("muteScan"); it != j.end() && it->is_boolean()) { s.muteScan = it->get<bool>(); }
    if (const auto it = j.find("logScan"); it != j.end() && it->is_boolean()) { s.logScan = it->get<bool>(); }
    if (const auto it = j.find("clock"); it != j.end() && it->is_number_integer()) { const int c = it->get<int>(); if (c >= 0 && c <= 2) { s.clockMode = c; } }
    if (const auto it = j.find("quick"); it != j.end() && it->is_array()) {
        for (std::size_t i = 0; i < it->size() && i < s.quick.size(); ++i) {
            const json& e = (*it)[i];
            if (!e.is_object()) { continue; }
            QuickMemory m;
            double hz = 0, bw = 0, mode = 0;
            num(e, "hz", hz); num(e, "bw", bw); num(e, "mode", mode);
            if (hz > 0.0 && mode >= 1 && mode <= 8) { m.set = true; m.hz = hz; m.mode = static_cast<std::uint32_t>(mode); m.bandwidthHz = bw > 0.0 ? bw : 0.0; s.quick[i] = m; }
        }
    }
    if (const auto it = j.find("mem"); it != j.end() && it->is_object()) {
        for (const auto& [k, e] : it->items()) {
            if (!e.is_object() || k.empty() || s.memories.size() >= 4096) { continue; }
            Memory m;
            num(e, "sq", m.squelchDb); num(e, "thr", m.thresholdDb); num(e, "off", m.offsetKhz); num(e, "tone", m.toneHz);
            if (!m.empty()) { s.memories[k] = m; }
        }
    }
    return s;
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------- fields

std::vector<double> parsePriorityList(const std::string& text) {
    std::vector<double> out;
    std::string tok;
    const auto flush = [&] {
        if (const auto v = toNumber(tok); v && *v > 0.0 && *v < 1e5 && out.size() < 16) { out.push_back(*v * 1e6); }
        tok.clear();
    };
    for (const char ch : text) {
        if (ch == ',' || ch == ';' || std::isspace(static_cast<unsigned char>(ch)) != 0) { flush(); } else { tok.push_back(ch); }
    }
    flush();
    return out;
}

std::string priorityText(const std::vector<double>& hz) {
    std::string s;
    for (const double v : hz) {
        if (!s.empty()) { s += ", "; }
        std::string t = fmt("%.4f", v / 1e6);
        while (t.size() > 1 && t.back() == '0') { t.pop_back(); }
        if (!t.empty() && t.back() == '.') { t.pop_back(); }
        s += t;
    }
    return s;
}

std::optional<double> parsePriorityEvery(const std::string& text) {
    const auto v = toNumber(text);
    if (!v) { return std::nullopt; }
    return std::clamp(*v, 5.0, 600.0);
}

std::optional<double> parseDbField(const std::string& text, bool& unset) {
    unset = false;
    const std::string t = trim(text);
    std::string low;
    for (const char ch : t) { low.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch)))); }
    if (low.empty() || low == "off" || low == "none" || low == "-") { unset = true; return kNaN; }
    const auto v = toNumber(t);
    if (!v) { return std::nullopt; }
    return std::clamp(*v, -200.0, 20.0);
}

std::string dbFieldText(double v) { return std::isnan(v) ? std::string() : fmt("%.0f", v); }

std::optional<double> parseKhzField(const std::string& text) {
    if (trim(text).empty()) { return 0.0; }
    const auto v = toNumber(text);
    if (!v || std::fabs(*v) > 100000.0) { return std::nullopt; }
    return *v;
}

std::optional<double> parseToneField(const std::string& text) {
    if (trim(text).empty()) { return 0.0; }
    const auto v = toNumber(text);
    if (!v) { return std::nullopt; }
    if (*v == 0.0) { return 0.0; }
    if (*v < 60.0 || *v > 260.0) { return std::nullopt; }
    return std::round(*v * 10.0) / 10.0;
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------- C3

void ListLockouts::add(int bookmarkId, double hz, double nowMs, double forMs) {
    const double until = forMs > 0.0 ? nowMs + forMs : 0.0;
    for (ListLockout& r : rows_) { if (r.bookmarkId == bookmarkId) { r.hz = hz; r.untilMs = until; return; } }
    rows_.push_back({bookmarkId, hz, until});
}
std::vector<int> ListLockouts::due(double nowMs) {
    std::vector<int> ids;
    for (auto it = rows_.begin(); it != rows_.end();) {
        if (it->untilMs > 0.0 && it->untilMs <= nowMs) { ids.push_back(it->bookmarkId); it = rows_.erase(it); } else { ++it; }
    }
    return ids;
}
std::vector<int> ListLockouts::clear() {
    std::vector<int> ids;
    for (const ListLockout& r : rows_) { ids.push_back(r.bookmarkId); }
    rows_.clear();
    return ids;
}
bool ListLockouts::has(int bookmarkId) const {
    return std::any_of(rows_.begin(), rows_.end(), [bookmarkId](const ListLockout& r) { return r.bookmarkId == bookmarkId; });
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------- C5

std::optional<int> ScanMuteFollower::update(bool enabled, std::uint32_t st, bool userMuted) {
    const bool stepping = enabled && st == FOXAPI_SCANNER_SCANNING;
    if (stepping && !weMuted_ && !userMuted) { weMuted_ = true; return 1; }
    if (!stepping && weMuted_) { weMuted_ = false; return 0; }   // a signal was found, the scan ended, or the option was turned off
    return std::nullopt;
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------- C4

std::string ScanWatcher::observe(double nowMs, std::uint32_t st, double hz, std::int64_t unixSeconds) {
    const bool heard = st == FOXAPI_SCANNER_PAUSED || st == FOXAPI_SCANNER_HOLDING;
    std::string line;
    if (heard && !listening_) {
        listening_ = true;
        hz_ = hz;
        sinceMs_ = nowMs;
        ++found_;
        auto it = std::find_if(hits_.begin(), hits_.end(), [hz](const ScanHit& h) { return std::fabs(h.hz - hz) < 1.0; });
        if (it == hits_.end()) { hits_.push_back({hz, 0, 0.0, nowMs}); it = hits_.end() - 1; }
        ++it->count;
        it->lastMs = nowMs;
    } else if (!heard && listening_) {
        listening_ = false;
        const double ms = std::max(0.0, nowMs - sinceMs_);
        totalHeardMs_ += ms;
        for (ScanHit& h : hits_) { if (std::fabs(h.hz - hz_) < 1.0) { h.heardMs += ms; } }
        std::tm tmv{};
        const std::time_t t = static_cast<std::time_t>(unixSeconds);
#ifdef _WIN32
        gmtime_s(&tmv, &t);
#else
        gmtime_r(&t, &tmv);
#endif
        char b[96];
        std::snprintf(b, sizeof b, "%04d-%02d-%02dT%02d:%02d:%02dZ,%.0f,%.1f", tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min, tmv.tm_sec, hz_, ms / 1000.0);
        line = b;
    }
    return line;
}
const ScanHit* ScanWatcher::busiest() const {
    const ScanHit* best = nullptr;
    for (const ScanHit& h : hits_) { if (best == nullptr || h.count > best->count) { best = &h; } }
    return best;
}
void ScanWatcher::reset() { *this = ScanWatcher(); }

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------- T5, C12, T10

std::vector<FoxCommand> recallCommands(const QuickMemory& q) {
    std::vector<FoxCommand> out;
    if (!q.set) { return out; }
    FoxCommand f = cmd(FOXAPI_OP_SET_FREQUENCY);
    f.num[0] = q.hz;
    out.push_back(f);
    FoxCommand m = cmd(FOXAPI_OP_SET_MODE);
    m.ival[0] = static_cast<std::int64_t>(q.mode);
    out.push_back(m);
    if (q.bandwidthHz > 0.0) {
        FoxCommand b = cmd(FOXAPI_OP_SET_BANDWIDTH);
        b.num[0] = q.bandwidthHz;
        out.push_back(b);
    }
    return out;
}

std::string quickLabel(int index, const QuickMemory& q) {
    std::string s = "M" + std::to_string(index + 1);
    if (q.set) { s += "  " + fmt("%.3f", q.hz / 1e6); }
    return s;
}

std::vector<FoxCommand> memoryRecallCommands(const Memory& m) {
    std::vector<FoxCommand> out;
    if (!std::isnan(m.squelchDb)) {
        FoxCommand c = cmd(FOXAPI_OP_SET_SQUELCH);
        c.num[0] = std::clamp(m.squelchDb, -200.0, 20.0);
        out.push_back(c);
    }
    return out;
}

std::string offsetText(double hz, const Memory& m) {
    if (m.offsetKhz == 0.0) { return std::string(); }
    return fmt("%.4f", (hz + m.offsetKhz * 1000.0) / 1e6);
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------- T8

namespace {
std::atomic<long> g_deltaRefusals{0};
std::atomic<long> g_deltaConversions{0};
const FoxEngineApi* g_deltaReal = nullptr;
DeltaMaskFn g_mask = nullptr;
DeltaRadioFn g_radioOf = nullptr;

bool deltaOn(const FoxCommand& c) {
    if (g_mask == nullptr || g_radioOf == nullptr) { return false; }
    const std::uint32_t m = g_mask();
    if (m == 0u) { return false; }
    const int r = g_radioOf(c);
    return r >= 1 && r <= 8 && ((m >> (r - 1)) & 1u) != 0u;
}

int32_t FOXAPI_CALL deltaSubmit(FoxSession* session, const FoxCommand* commands, uint32_t count, FoxSubmitResult* results) {
    if (commands == nullptr || count == 0u || g_mask == nullptr || g_mask() == 0u) { return g_deltaReal->submit(session, commands, count, results); }
    std::vector<FoxCommand> pass;
    std::vector<std::uint32_t> passIndex;
    pass.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        FoxCommand c = commands[i];
        if (deltaOn(c)) {
            const DeltaVerdict v = decideDelta(c);
            if (v == DeltaVerdict::Refused) {
                if (results != nullptr) { results[i].structSize = sizeof(FoxSubmitResult); results[i].status = FOXAPI_DENIED; results[i].ticket = 0; }
                continue;
            }
        }
        pass.push_back(c);
        passIndex.push_back(i);
    }
    if (pass.empty()) { return 0; }
    std::vector<FoxSubmitResult> sub(pass.size());
    for (FoxSubmitResult& s : sub) { s.structSize = sizeof(FoxSubmitResult); }
    const int32_t rc = g_deltaReal->submit(session, pass.data(), static_cast<uint32_t>(pass.size()), results != nullptr ? sub.data() : nullptr);
    if (rc >= 0 && results != nullptr) { for (std::size_t k = 0; k < pass.size(); ++k) { results[passIndex[k]] = sub[k]; } }
    return rc;
}

int32_t FOXAPI_CALL deltaSubmitText(FoxSession* session, const FoxCommand* command, const char* text, uint32_t len, FoxSubmitResult* result) {
    if (command != nullptr && deltaOn(*command)) {
        FoxCommand c = *command;
        if (decideDelta(c) == DeltaVerdict::Refused) {
            if (result != nullptr) { result->structSize = sizeof(FoxSubmitResult); result->status = FOXAPI_DENIED; result->ticket = 0; }
            return 0;
        }
        return g_deltaReal->submit_text(session, &c, text, len, result);
    }
    return g_deltaReal->submit_text(session, command, text, len, result);
}
}  // namespace

DeltaVerdict decideDelta(FoxCommand& c) {
    if (c.op == FOXAPP_OP_VFO_TO_ABSOLUTE) {
        c.op = FOXAPI_OP_SET_FREQUENCY;   // the counter's rule: the VFO offset kept, the centre follows
        g_deltaConversions.fetch_add(1, std::memory_order_relaxed);
        return DeltaVerdict::Converted;
    }
    if (c.op == FOXAPI_OP_SET_VFO_OFFSET || c.op == FOXAPP_OP_SET_VFO_OFFSET_FREE) {
        g_deltaRefusals.fetch_add(1, std::memory_order_relaxed);
        return DeltaVerdict::Refused;
    }
    return DeltaVerdict::Pass;
}

const FoxEngineApi* deltaLockApi(const FoxEngineApi* real, DeltaMaskFn mask, DeltaRadioFn radioOf) {
    static FoxEngineApi copy{};
    static std::once_flag once;
    std::call_once(once, [&] {
        copy = *real;
        g_deltaReal = real;
        copy.submit = &deltaSubmit;
        copy.submit_text = &deltaSubmitText;
    });
    g_mask = mask;
    g_radioOf = radioOf;
    return &copy;
}
long deltaRefusals() { return g_deltaRefusals.load(std::memory_order_relaxed); }
long deltaConversions() { return g_deltaConversions.load(std::memory_order_relaxed); }

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------- F7

std::optional<double> BandwidthMemory::update(bool enabled, std::uint32_t mode, double bw, long tick, std::map<std::uint32_t, double>& memory, bool& changed) {
    changed = false;
    if (!enabled || mode == 0u) { seeded_ = false; expect_.reset(); userMoved_ = false; return std::nullopt; }
    if (!seeded_) { seeded_ = true; mode_ = mode; lastBw_ = bw; modeSince_ = tick; bwSince_ = tick; userMoved_ = false; return std::nullopt; }   // the width found at start is the mode's own: nothing recorded until the user moves it
    if (mode != mode_) {
        // a mode was entered: the engine has put the mode's default width in; give back the remembered one, if any
        mode_ = mode;
        modeSince_ = tick;
        lastBw_ = bw;
        bwSince_ = tick;
        userMoved_ = false;
        expect_.reset();
        const auto it = memory.find(mode);
        if (it != memory.end() && std::fabs(it->second - bw) > 0.5) { expect_ = it->second; return it->second; }
        return std::nullopt;
    }
    if (expect_) {
        if (std::fabs(bw - *expect_) < 1.0 || tick - modeSince_ > 60) { expect_.reset(); lastBw_ = bw; bwSince_ = tick; }   // the width asked for arrived (or the engine clamped it): from here on the user's
        return std::nullopt;
    }
    if (tick - modeSince_ < 6) { lastBw_ = bw; bwSince_ = tick; return std::nullopt; }   // the engine's own settling after a mode change
    if (std::fabs(bw - lastBw_) > 0.5) { lastBw_ = bw; bwSince_ = tick; userMoved_ = true; return std::nullopt; }
    if (userMoved_ && tick - bwSince_ >= 10) {   // settled for ten ticks at a width the user chose
        userMoved_ = false;
        const auto it = memory.find(mode);
        if (it == memory.end() || std::fabs(it->second - bw) > 0.5) { memory[mode] = bw; changed = true; }
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------- T12

std::string clockText(int mode, std::int64_t unixSeconds, int localOffsetSeconds) {
    if (mode != 1 && mode != 2) { return std::string(); }
    const std::int64_t t = unixSeconds + (mode == 2 ? localOffsetSeconds : 0);
    std::int64_t day = t % 86400;
    if (day < 0) { day += 86400; }
    char b[32];
    std::snprintf(b, sizeof b, "%02d:%02d:%02d %s", static_cast<int>(day / 3600), static_cast<int>((day / 60) % 60), static_cast<int>(day % 60), mode == 1 ? "UTC" : "LT");
    return b;
}

int localOffsetSecondsNow(std::int64_t unixSeconds) {
    const std::time_t t = static_cast<std::time_t>(unixSeconds);
    std::tm utc{};
#ifdef _WIN32
    if (gmtime_s(&utc, &t) != 0) { return 0; }
#else
    if (gmtime_r(&t, &utc) == nullptr) { return 0; }
#endif
    // mktime reads the broken-down UTC time as LOCAL time (daylight rule decided by the OS): what it gives back differs from `t` by the offset
    utc.tm_isdst = -1;
    const std::time_t back = std::mktime(&utc);
    if (back == static_cast<std::time_t>(-1)) { return 0; }
    return static_cast<int>(t - back);
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------- U10

std::vector<ShortcutRow> shortcutRows(const foxsdr::slintsystem::KeyTable& table) {
    std::vector<ShortcutRow> out;
    for (int i = 0; i < foxsdr::slintsystem::kKeyActionCount; ++i) {
        const auto& c = table[static_cast<std::size_t>(i)];
        if (!c.bound()) { continue; }
        out.push_back({foxsdr::slintsystem::formatChord(c), foxsdr::slintsystem::keyActionName(i)});
    }
    return out;
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------- P11

namespace {
std::string lowerExt(const std::string& name) {
    const auto dot = name.find_last_of('.');
    if (dot == std::string::npos) { return std::string(); }
    std::string e = name.substr(dot);
    for (char& ch : e) { ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch))); }
    return e;
}
}  // namespace

bool isIqWav(const std::string& path) { return lowerExt(path) == ".wav"; }

std::vector<RecordingFile> listRecordings(const std::string& dir, std::size_t limit) {
    namespace fs = std::filesystem;
    std::vector<RecordingFile> out;
    std::error_code ec;
    if (dir.empty() || !fs::is_directory(dir, ec)) { return out; }
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code e2;
        if (!it->is_regular_file(e2)) { continue; }
        const std::string name = it->path().filename().string();
        const std::string ext = lowerExt(name);
        if (ext != ".wav" && ext != ".flac" && ext != ".raw" && ext != ".cu8" && ext != ".cs16" && ext != ".cf32" && ext != ".csv" && ext != ".png" && ext != ".bmp") { continue; }
        RecordingFile r;
        r.name = name;
        r.path = it->path().string();
        r.bytes = static_cast<std::uint64_t>(it->file_size(e2));
        const auto ft = it->last_write_time(e2);
        // file_time_type -> unix seconds without C++20 clock_cast (not in every standard library): the offset from "now" in both clocks
        const auto nowFile = fs::file_time_type::clock::now();
        const auto nowSys = std::chrono::system_clock::now();
        r.unixSeconds = std::chrono::duration_cast<std::chrono::seconds>((nowSys + (ft - nowFile)).time_since_epoch()).count();
        r.kind = name.rfind("iq_", 0) == 0 ? "I/Q" : (ext == ".wav" || ext == ".flac") ? "Audio" : "Other";
        out.push_back(std::move(r));
    }
    std::sort(out.begin(), out.end(), [](const RecordingFile& a, const RecordingFile& b) { return a.unixSeconds != b.unixSeconds ? a.unixSeconds > b.unixSeconds : a.name < b.name; });
    if (out.size() > limit) { out.resize(limit); }
    return out;
}

std::string sizeText(std::uint64_t b) {
    if (b < 1024) { return std::to_string(b) + " B"; }
    const double v = static_cast<double>(b);
    if (b < 1024ull * 1024) { return fmt("%.1f KB", v / 1024.0); }
    if (b < 1024ull * 1024 * 1024) { return fmt("%.1f MB", v / (1024.0 * 1024.0)); }
    return fmt("%.1f GB", v / (1024.0 * 1024.0 * 1024.0));
}

std::string dateText(std::int64_t unixSeconds) {
    std::tm tmv{};
    const std::time_t t = static_cast<std::time_t>(unixSeconds);
#ifdef _WIN32
    gmtime_s(&tmv, &t);
#else
    gmtime_r(&t, &tmv);
#endif
    char b[32];
    std::snprintf(b, sizeof b, "%04d-%02d-%02d %02d:%02d", tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min);
    return b;
}

}  // namespace foxsdr::slintlanec
