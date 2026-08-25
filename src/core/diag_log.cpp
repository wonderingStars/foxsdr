// See diag_log.hpp for why the ring exists as well as the file, why the ring
// is fixed storage, and why the crash-path reader takes no lock.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/diag_log.hpp"

#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <system_error>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace cascade::core {

namespace {

// The environment override, honoured by every part of the diagnostics tree.
// It is what keeps the tests out of the user's real %LOCALAPPDATA%\FoxSDR -
// the same discipline CASCADE_CONFIG_TEST applies to the config file.
const char* diagDirOverride() {
    const char* v = std::getenv("FOXSDR_DIAG_DIR");
    return (v != nullptr && *v != '\0') ? v : nullptr;
}

// "12:34:56.789 " - local wall time, milliseconds, no date. The date is in
// the file name and in the report header; repeating it on 4000 lines would
// cost a fifth of the ring for nothing.
int formatStamp(char* out, std::size_t cap) {
#if defined(_WIN32)
    SYSTEMTIME st{};
    ::GetLocalTime(&st);
    return std::snprintf(out, cap, "%02u:%02u:%02u.%03u", st.wHour, st.wMinute, st.wSecond,
                         st.wMilliseconds);
#else
    const std::time_t t = std::time(nullptr);
    std::tm tmv{};
    localtime_r(&t, &tmv);
    return std::snprintf(out, cap, "%02d:%02d:%02d.000", tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
#endif
}

}  // namespace

std::string diagBaseDir() {
    if (const char* over = diagDirOverride()) { return std::string(over); }
#if defined(_WIN32)
    // LOCALAPPDATA, not APPDATA: a log and a crash dump describe THIS machine
    // and must not follow a roaming profile onto another one.
    const char* local = std::getenv("LOCALAPPDATA");
    if (local == nullptr || *local == '\0') { return std::string(); }
    return std::string(local) + "\\FoxSDR";
#else
    if (const char* xdg = std::getenv("XDG_STATE_HOME")) {
        if (*xdg != '\0') { return std::string(xdg) + "/foxsdr"; }
    }
    const char* home = std::getenv("HOME");
    if (home == nullptr || *home == '\0') { return std::string(); }
    return std::string(home) + "/.local/state/foxsdr";
#endif
}

std::string diagCrashDir() {
    const std::string base = diagBaseDir();
    if (base.empty()) { return std::string(); }
#if defined(_WIN32)
    return base + "\\crashes";
#else
    return base + "/crashes";
#endif
}

std::string diagLogDir() {
    const std::string base = diagBaseDir();
    if (base.empty()) { return std::string(); }
#if defined(_WIN32)
    return base + "\\logs";
#else
    return base + "/logs";
#endif
}

DiagLog& DiagLog::instance() {
    static DiagLog log;
    return log;
}

void DiagLog::configure(const std::string& dir, bool enabled) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (fp_ != nullptr) {
        std::fclose(fp_);
        fp_ = nullptr;
    }
    dir_.clear();
    path_.clear();
    fileBytes_ = 0;
    enabled_ = false;
    // OFF MEANS OFF: no directory is created, so a user who never turned this
    // on has no trace of it on disk at all - not an empty folder, not a
    // zero-byte file. Asserted in tests/test_diagnostics.cpp.
    if (!enabled || dir.empty()) { return; }

    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(dir), ec);
    if (ec && !std::filesystem::is_directory(std::filesystem::path(dir))) { return; }

    dir_ = dir;
    path_ = dir_ + "/foxsdr.log";
    fp_ = std::fopen(path_.c_str(), "ab");
    if (fp_ == nullptr) {
        dir_.clear();
        path_.clear();
        return;
    }
    std::error_code sec;
    const auto sz = std::filesystem::file_size(std::filesystem::path(path_), sec);
    fileBytes_ = sec ? 0u : static_cast<std::size_t>(sz);
    enabled_ = true;
}

bool DiagLog::fileEnabled() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return enabled_;
}

std::string DiagLog::filePath() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return path_;
}

void DiagLog::write(const char* level, const char* msg) {
    char line[kLineBytes];
    const int sn = formatStamp(line, sizeof(line));
    const std::size_t at = (sn > 0) ? static_cast<std::size_t>(sn) : 0u;
    const int n = std::snprintf(line + at, sizeof(line) - at, " %s %s",
                                (level != nullptr) ? level : "info",
                                (msg != nullptr) ? msg : "");
    std::size_t len = at + ((n > 0) ? static_cast<std::size_t>(n) : 0u);
    // Truncated, never grown: a log line is a diagnostic, not a data channel.
    const std::size_t maxLen = static_cast<std::size_t>(kLineBytes) - 1u;
    if (len > maxLen) { len = maxLen; }
    line[len] = '\0';

    std::lock_guard<std::mutex> lk(mutex_);
    const int slot = next_.load(std::memory_order_relaxed);
    std::memcpy(ring_[slot], line, len + 1);
    // RELEASE on both: copyRingRaw reads them with no lock at all, and the
    // line bytes above must be visible before the index that points at them.
    next_.store((slot + 1) % kRingLines, std::memory_order_release);
    written_.fetch_add(1, std::memory_order_release);
    appendToFileLocked(line, len);
}

void DiagLog::writef(const char* level, const char* fmt, ...) {
    char msg[kLineBytes];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(msg, sizeof(msg), (fmt != nullptr) ? fmt : "", ap);
    va_end(ap);
    write(level, msg);
}

std::vector<std::string> DiagLog::ringSnapshot() const {
    std::lock_guard<std::mutex> lk(mutex_);
    const std::uint64_t total = written_.load(std::memory_order_relaxed);
    const int count =
        (total < static_cast<std::uint64_t>(kRingLines)) ? static_cast<int>(total) : kRingLines;
    int idx = next_.load(std::memory_order_relaxed) - count;
    if (idx < 0) { idx += kRingLines; }
    std::vector<std::string> out;
    out.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) { out.emplace_back(ring_[(idx + i) % kRingLines]); }
    return out;
}

std::uint64_t DiagLog::linesWritten() const { return written_.load(std::memory_order_relaxed); }

std::size_t DiagLog::copyRingRaw(char* out, std::size_t cap) const {
    // NO LOCK, NO ALLOCATION, NO CRT FORMATTING. See the header: a crash can
    // happen while another thread holds mutex_, and blocking here would turn a
    // crash into a hang with no report at all. The bounded cost is one
    // possibly torn line, and it is the newest one.
    if (out == nullptr || cap == 0) { return 0; }
    out[0] = '\0';
    const std::uint64_t total = written_.load(std::memory_order_acquire);
    const int next = next_.load(std::memory_order_acquire);
    const int count =
        (total < static_cast<std::uint64_t>(kRingLines)) ? static_cast<int>(total) : kRingLines;
    int idx = next - count;
    if (idx < 0) { idx += kRingLines; }

    std::size_t used = 0;
    for (int i = 0; i < count; ++i) {
        const char* line = ring_[(idx + i) % kRingLines];
        std::size_t len = 0;
        while (len + 1 < static_cast<std::size_t>(kLineBytes) && line[len] != '\0') { ++len; }
        if (used + len + 2 > cap) { break; }  // +1 newline, +1 terminator
        std::memcpy(out + used, line, len);
        used += len;
        out[used++] = '\n';
    }
    out[used] = '\0';
    return used;
}

void DiagLog::resetForTest() {
    std::lock_guard<std::mutex> lk(mutex_);
    if (fp_ != nullptr) {
        std::fclose(fp_);
        fp_ = nullptr;
    }
    dir_.clear();
    path_.clear();
    enabled_ = false;
    fileBytes_ = 0;
    written_.store(0, std::memory_order_relaxed);
    next_.store(0, std::memory_order_relaxed);
    std::memset(ring_, 0, sizeof(ring_));
}

void DiagLog::appendToFileLocked(const char* line, std::size_t len) {
    if (!enabled_ || fp_ == nullptr) { return; }
    rotateIfNeededLocked(len + 1);
    if (fp_ == nullptr) { return; }
    std::fwrite(line, 1, len, fp_);
    std::fputc('\n', fp_);
    // Flushed every line on purpose: the run that most needs the log is the
    // one that does not reach a clean exit, and a buffered tail is exactly the
    // part that would be lost.
    std::fflush(fp_);
    fileBytes_ += len + 1;
}

void DiagLog::rotateIfNeededLocked(std::size_t incoming) {
    if (fileBytes_ + incoming <= kRotateBytes) { return; }
    std::fclose(fp_);
    fp_ = nullptr;

    namespace fs = std::filesystem;
    std::error_code ec;
    // kKeptFiles COUNTS THE LIVE FILE: 3 means foxsdr.log, .1 and .2, and the
    // highest numbered file that may exist is therefore foxsdr.<kKeptFiles-1>.
    //
    // That last file is DROPPED here rather than shifted up, and getting this
    // wrong keeps one more file than the header promises - about a megabyte of
    // somebody's profile per extra file. It also stays invisible for two
    // rotations, because the extra file only appears once .2 has something in
    // it to be shifted from, which is why the test rotates three times.
    const fs::path oldest =
        fs::path(dir_) / (std::string("foxsdr.") + std::to_string(kKeptFiles - 1) + ".log");
    fs::remove(oldest, ec);
    // Then shift the rest up, oldest first, so nothing is overwritten before it
    // has been moved.
    for (int i = kKeptFiles - 2; i >= 1; --i) {
        const fs::path from =
            fs::path(dir_) / (std::string("foxsdr.") + std::to_string(i) + ".log");
        const fs::path to =
            fs::path(dir_) / (std::string("foxsdr.") + std::to_string(i + 1) + ".log");
        if (fs::exists(from, ec)) { fs::rename(from, to, ec); }
    }
    const fs::path live(path_);
    if (kKeptFiles >= 2) {
        const fs::path first = fs::path(dir_) / "foxsdr.1.log";
        fs::remove(first, ec);
        fs::rename(live, first, ec);
    } else {
        // One file kept means exactly one file: no history, and specifically
        // not a foxsdr.1.log that the count above does not allow for.
        fs::remove(live, ec);
    }

    fp_ = std::fopen(path_.c_str(), "wb");
    fileBytes_ = 0;
    if (fp_ == nullptr) { enabled_ = false; }
}

void diagLogf(const char* fmt, ...) {
    char msg[DiagLog::kLineBytes];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(msg, sizeof(msg), (fmt != nullptr) ? fmt : "", ap);
    va_end(ap);
    DiagLog::instance().write("info", msg);
}

void diagWarnf(const char* fmt, ...) {
    char msg[DiagLog::kLineBytes];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(msg, sizeof(msg), (fmt != nullptr) ? fmt : "", ap);
    va_end(ap);
    DiagLog::instance().write("warn", msg);
}

}  // namespace cascade::core
