// rds_log.cpp - see core/rds_log.hpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/rds_log.hpp"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>

#include "core/rec_naming.hpp"

namespace fs = std::filesystem;

namespace cascade::core::rdslog {

const char* programmeTypeName(int pty) {
    static const char* const kNames[32] = {"None",          "News",          "Current affairs", "Information", "Sport",     "Education", "Drama",
                                           "Culture",       "Science",       "Varied",          "Pop music",   "Rock music", "Easy listening music",
                                           "Light classical", "Serious classical", "Other music", "Weather",   "Finance",   "Children's programmes",
                                           "Social affairs", "Religion",     "Phone-in",        "Travel",      "Leisure",   "Jazz music", "Country music",
                                           "National music", "Oldies music", "Folk music",      "Documentary", "Alarm test", "Alarm"};
    return pty >= 0 && pty < 32 ? kNames[pty] : "";
}

std::string csvField(const std::string& text) {
    std::string clean;
    clean.reserve(text.size());
    for (const unsigned char c : text) {
        if (clean.size() >= kMaxTextBytes) { break; }
        clean.push_back(c < 0x20 || c == 0x7F || c >= 0x80 ? '?' : static_cast<char>(c));
    }
    bool quote = false;
    for (const char c : clean) { quote = quote || c == ',' || c == '"'; }
    if (!quote) { return clean; }
    std::string out = "\"";
    for (const char c : clean) {
        if (c == '"') { out += '"'; }
        out += c;
    }
    out += '"';
    return out;
}

std::string headerLine() { return "time_utc,radio,station_name,programme_type_code,programme_type,radio_text\n"; }

std::string formatLine(std::int64_t unixMs, int radio, const std::string& ps, int pty, const std::string& radioText) {
    const std::time_t secs = static_cast<std::time_t>(unixMs / 1000);
    std::tm t{};
#ifdef _WIN32
    gmtime_s(&t, &secs);
#else
    gmtime_r(&secs, &t);
#endif
    char when[32];
    std::strftime(when, sizeof when, "%Y-%m-%dT%H:%M:%SZ", &t);
    // the station name: trailing spaces (RDS pads to 8) are not part of the name
    std::string name = ps;
    while (!name.empty() && name.back() == ' ') { name.pop_back(); }
    std::string line = when;
    line += ',' + std::to_string(radio) + ',' + csvField(name) + ',' + (pty >= 0 ? std::to_string(pty) : std::string()) + ',' + csvField(programmeTypeName(pty)) + ',' +
            csvField(radioText) + '\n';
    return line;
}

bool changed(const Last& last, const dsp::RdsState& st) {
    const bool haveName = st.psValid && !st.ps.empty();
    const bool haveText = !st.radioText.empty();
    if (!haveName && !haveText) { return false; }
    if (haveName && st.ps != last.ps) { return true; }
    if (haveText && st.radioText != last.rt) { return true; }
    if (haveName && static_cast<int>(st.pty) != last.pty) { return true; }
    return false;
}

bool Logger::configure(const Options& o, std::string& why) {
    why.clear();
    if (o.enabled) {
        if (o.folder.empty() || !fs::path(o.folder).is_absolute()) { why = "choose the folder the RDS log is written to (a full path)"; }
        else {
            // The template is proved when it is set, by expanding it for a sample: the recorder's own rules.
            recname::Fields f;
            f.unixMs = 1'790'000'000'000LL;
            f.frequencyHz = 100.0e6;
            f.radio = "1";
            const recname::Expanded e = recname::expand(o.nameTemplate.empty() ? std::string(kDefaultTemplate) : o.nameTemplate, f);
            if (!e.ok) { why = "the RDS log's file name was refused: " + e.error; }
        }
    }
    {
        std::lock_guard<std::mutex> lk(m_);
        if (!why.empty()) {
            error_ = why;
            opt_.enabled = false;
            return false;
        }
        const bool wasOn = opt_.enabled;
        opt_ = o;
        if (opt_.nameTemplate.empty()) { opt_.nameTemplate = kDefaultTemplate; }
        if (!o.enabled || !wasOn) { last_.clear(); }   // a fresh start logs what is on the air now
        error_.clear();
        if (o.enabled && !worker_.joinable()) {
            stopping_ = false;
            worker_ = std::thread([this] { workerMain(); });
        }
    }
    return true;
}

void Logger::observe(int radio, const dsp::RdsState& st, std::int64_t unixMs, double frequencyHz) {
    std::lock_guard<std::mutex> lk(m_);
    if (!opt_.enabled) { return; }
    Last& l = last_[radio];
    if (!changed(l, st)) { return; }
    if (st.psValid && !st.ps.empty()) { l.ps = st.ps; }
    if (!st.radioText.empty()) { l.rt = st.radioText; }
    l.pty = static_cast<int>(st.pty);
    if (queue_.size() >= kMaxQueued) { ++dropped_; return; }
    queue_.push_back({unixMs, radio, formatLine(unixMs, radio, l.ps, l.pty, l.rt), frequencyHz});
    cv_.notify_one();
}

void Logger::flushForTest() {
    std::unique_lock<std::mutex> lk(m_);
    cv_.wait_for(lk, std::chrono::seconds(10), [this] { return queue_.empty() && !busy_; });
}

void Logger::stop() {
    {
        std::unique_lock<std::mutex> lk(m_);
        if (!worker_.joinable()) { return; }
        cv_.wait_for(lk, std::chrono::seconds(5), [this] { return queue_.empty() && !busy_; });
        stopping_ = true;
        opt_.enabled = false;
        cv_.notify_all();
    }
    worker_.join();
}

std::string Logger::error() const {
    std::lock_guard<std::mutex> lk(m_);
    return error_;
}

std::string Logger::lastFileName() const {
    std::lock_guard<std::mutex> lk(m_);
    return lastFile_;
}

void Logger::workerMain() {
    for (;;) {
        Item it;
        {
            std::unique_lock<std::mutex> lk(m_);
            cv_.wait(lk, [this] { return stopping_ || !queue_.empty(); });
            if (queue_.empty()) { if (stopping_) { return; } continue; }
            it = std::move(queue_.front());
            queue_.pop_front();
            busy_ = true;
        }
        std::string why;
        const bool ok = writeOne(it, why);
        {
            std::lock_guard<std::mutex> lk(m_);
            busy_ = false;
            if (ok) { ++written_; error_.clear(); } else { error_ = why; }
            cv_.notify_all();
        }
    }
}

bool Logger::writeOne(const Item& it, std::string& why) {
    Options o;
    std::uint64_t counter = 0;
    {
        std::lock_guard<std::mutex> lk(m_);
        o = opt_;
        counter = ++counter_;
    }
    if (o.folder.empty()) { why = "no folder is chosen for the RDS log"; return false; }
    recname::Fields f;
    f.unixMs = it.unixMs;
    f.frequencyHz = it.frequencyHz;
    f.radio = std::to_string(it.radio);
    f.counter = counter;
    const recname::Expanded e = recname::expand(o.nameTemplate.empty() ? std::string(kDefaultTemplate) : o.nameTemplate, f);
    if (!e.ok) { why = "the RDS log's file name was refused: " + e.error; return false; }
    std::string perr;
    const std::string path = recname::resolvePath(o.folder, e.relPath, "csv", &perr);
    if (path.empty()) { why = "the RDS log's file name was refused: " + perr; return false; }
    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);
    if (ec) { why = "the RDS log's folder could not be made: " + ec.message(); return false; }
    const bool fresh = !fs::exists(path, ec);
    std::ofstream f1(path, std::ios::binary | std::ios::app);
    if (!f1) { why = "the RDS log file could not be opened for writing"; return false; }
    if (fresh) { f1 << headerLine(); }
    f1 << it.line;
    f1.flush();
    if (!f1) { why = "the RDS log file could not be written (the disk may be full)"; return false; }
    {
        std::lock_guard<std::mutex> lk(m_);
        lastFile_ = fs::path(path).filename().string();
    }
    return true;
}

}  // namespace cascade::core::rdslog
