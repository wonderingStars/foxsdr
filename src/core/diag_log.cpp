// See diag_log.hpp for why the ring exists as well as the file, why the ring
// is fixed storage, and why the crash-path reader takes no lock.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/diag_log.hpp"

#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <chrono>
#include <filesystem>
#include <initializer_list>
#include <system_error>
#include <thread>

#if defined(_WIN32)
#include <windows.h>

#include <fcntl.h>
#include <io.h>
#endif

namespace cascade::core {

namespace {

// Captured during static initialisation, which is as close to process start
// as C++ gets, so processUptimeSec() is a subtraction and nothing else - the
// crash handler calls it with the heap and the CRT already suspect.
#if defined(_WIN32)
const unsigned long long g_startTick = ::GetTickCount64();
#else
const std::chrono::steady_clock::time_point g_startTime = std::chrono::steady_clock::now();
#endif

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
    // LEAKED ON PURPOSE. The stderr reader thread (installStderrCapture) is
    // detached and can be handed a driver's line at any moment, including
    // while the CRT is running static destructors at exit; a function-local
    // static would be destroyed under it. Nothing is lost by never destroying
    // it: every file line is flushed as it is written, and the process's exit
    // closes the descriptor.
    static DiagLog* log = new DiagLog();
    return *log;
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

std::uint64_t processUptimeSec() {
#if defined(_WIN32)
    const unsigned long long now = ::GetTickCount64();
    return (now >= g_startTick) ? (now - g_startTick) / 1000ull : 0ull;
#else
    const auto d = std::chrono::steady_clock::now() - g_startTime;
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(d).count());
#endif
}

// ---------------------------------------------------------------------------
// Lines from code that is not ours
// ---------------------------------------------------------------------------
bool LineRateLimiter::admit(std::uint64_t nowMs, std::uint64_t& suppressedOut) {
    suppressedOut = 0;
    const std::uint64_t window = nowMs / 1000ull;
    if (window != window_) {
        // A new second. Whatever the last one dropped is reported now, once,
        // ahead of the first line of this second - which is the only moment a
        // count can be reported without a timer of its own.
        suppressedOut = suppressed_;
        suppressed_ = 0;
        count_ = 0;
        window_ = window;
    }
    if (count_ < limit_) {
        ++count_;
        return true;
    }
    ++suppressed_;
    return false;
}

namespace {

std::string lowerAscii(const std::string& s) {
    std::string out = s;
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') { c = static_cast<char>(c - 'A' + 'a'); }
    }
    return out;
}

bool isTokenChar(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           c == '-' || c == '_';
}

bool isDigitChar(char c) { return c >= '0' && c <= '9'; }
bool isAlphaChar(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
bool isAlnumChar(char c) { return isDigitChar(c) || isAlphaChar(c); }

constexpr char kStripped[] = "<stripped>";
constexpr std::size_t kStrippedLen = sizeof(kStripped) - 1;

// Where a serial VALUE ends: token characters, and a ':' only when more of
// them follow it. The native Airspy names itself "AIRSPY_SN:26A464DC28593E93"
// - one value - and a rule that stopped at the colon stripped the prefix and
// kept the serial (found in a field report, 0.99.33).
std::size_t serialValueEnd(const std::string& s, std::size_t p) {
    std::size_t e = p;
    while (e < s.size()) {
        if (isTokenChar(s[e])) {
            ++e;
        } else if (s[e] == ':' && e > p && e + 1 < s.size() && isTokenChar(s[e + 1])) {
            ++e;
        } else {
            break;
        }
    }
    return e;
}

// Serial numbers, wherever a line names one. The value is replaced by
// <stripped>; the word stays, so a reader still knows a serial was there.
// The lower-case shadow is rebuilt after every replacement because the
// replacement changes the offsets.
void stripSerials(std::string& out) {
    // 1. After the word: "serial=EDR04ZDB2", "Serial: 00000001", "serial
    //    number 1234", "serial_number=...", "(serial AIRSPY_SN:26A4...)".
    std::size_t from = 0;
    for (;;) {
        const std::string low = lowerAscii(out);
        const std::size_t at = low.find("serial", from);
        if (at == std::string::npos) { break; }
        std::size_t p = at + 6;
        auto skipSep = [&]() {
            while (p < out.size() && (out[p] == ' ' || out[p] == ':' || out[p] == '=' ||
                                      out[p] == '#' || out[p] == '\t' || out[p] == '"' ||
                                      out[p] == '\'')) {
                ++p;
            }
        };
        // The rest of a key the word starts: "serial_number=", "serialNumber:",
        // udev's "ID_SERIAL_SHORT=" - none of that is the value.
        for (;;) {
            if (p < out.size() && isAlphaChar(out[p])) {
                ++p;
            } else if (p + 1 < out.size() && (out[p] == '_' || out[p] == '-') &&
                       isAlphaChar(out[p + 1])) {
                p += 2;
            } else {
                break;
            }
        }
        // "serial number 1234", "serial no 1234": the word between, after a
        // space, is not the value either.
        std::size_t q = p;
        while (q < low.size() && low[q] == ' ') { ++q; }
        for (const char* w : {"number", "num", "no"}) {
            const std::size_t wl = std::strlen(w);
            if (low.compare(q, wl, w) == 0 && (q + wl >= low.size() || !isTokenChar(low[q + wl]))) {
                p = q + wl;
                break;
            }
        }
        skipSep();
        const std::size_t e = serialValueEnd(out, p);
        if (e > p) {
            out.replace(p, e - p, kStripped);
            from = p + kStrippedLen;
        } else {
            from = (p > at + 6) ? p : at + 6;
        }
    }

    // 2. A keyed serial with no word: "SN: 1234", "sn=1234", "S/N: 1234",
    //    and the Airspy's own "AIRSPY_SN:26A4..." when it appears bare. The key
    //    must start a word ('_' counts as a joiner, so AIRSPY_SN qualifies and
    //    "isn't" does not) and be followed by ':' or '='.
    from = 0;
    for (;;) {
        const std::string low = lowerAscii(out);
        std::size_t at = std::string::npos;
        std::size_t keyLen = 0;
        for (const char* k : {"s/n", "sn"}) {
            const std::size_t a = low.find(k, from);
            if (a < at) {
                at = a;
                keyLen = std::strlen(k);
            }
        }
        if (at == std::string::npos) { break; }
        from = at + keyLen;
        if (at > 0 && isAlnumChar(low[at - 1])) { continue; }
        std::size_t p = at + keyLen;
        while (p < out.size() && out[p] == ' ') { ++p; }
        if (p >= out.size() || (out[p] != ':' && out[p] != '=')) { continue; }
        ++p;
        while (p < out.size() && (out[p] == ' ' || out[p] == '"' || out[p] == '\'')) { ++p; }
        const std::size_t e = serialValueEnd(out, p);
        if (e > p) {
            out.replace(p, e - p, kStripped);
            from = p + kStrippedLen;
        }
    }
}

bool endsSegment(char c) {
    return c == '\\' || c == '/' || c == '#' || c == '\'' || c == '"' || c == ' ' || c == '\t' ||
           c == ')' || c == ']' || c == ',' || c == ';' || c == ':' || c == '{';
}

// A Windows device instance id: "USB\VID_0DB0&PID_0076\7E59240920A2", and
// the interface path form "\\?\usb#vid_0bda&pid_2838#00000001#{guid}". The
// segment after the hardware id is the device's serial number, or an id the
// bus derived from where it is plugged in; either way it names one physical
// device and becomes <stripped>. The hardware id (which kind of device) stays.
void maskUsbInstanceIds(std::string& s) {
    std::size_t from = 0;
    for (;;) {
        const std::string low = lowerAscii(s);
        const std::size_t v = low.find("vid_", from);
        if (v == std::string::npos) { break; }
        from = v + 4;
        std::size_t e = v;
        while (e < s.size() && !endsSegment(s[e])) { ++e; }
        const std::size_t pid = low.find("&pid_", v);
        if (pid == std::string::npos || pid >= e) { continue; }
        if (e >= s.size() || (s[e] != '\\' && s[e] != '#')) { continue; }
        const std::size_t p = e + 1;
        std::size_t q = p;
        while (q < s.size() && !endsSegment(s[q])) { ++q; }
        if (q > p) {
            s.replace(p, q - p, kStripped);
            from = p + kStrippedLen;
        }
    }
}

// SoapySDR's device label: "<product> :: <serial>" (rtlsdrSupport builds it
// from the product and serial strings with exactly that separator). Whatever
// follows " :: " up to the next separator becomes <stripped>.
void maskSoapyLabelSerials(std::string& s) {
    std::size_t from = 0;
    for (;;) {
        const std::size_t at = s.find(" :: ", from);
        if (at == std::string::npos) { break; }
        const std::size_t p = at + 4;
        std::size_t e = p;
        while (e < s.size() && !endsSegment(s[e])) { ++e; }
        if (e > p) {
            s.replace(p, e - p, kStripped);
            from = p + kStrippedLen;
        } else {
            from = p;
        }
    }
}

// A libusb info/debug line that names a device instance id is libusb LISTING
// THE MACHINE'S USB DEVICES - "no DeviceInterfaceGUID registered for
// 'USB\VID_046D&PID_C336...'", "The following device has no driver: ...". It
// inventories the keyboard, the mouse and everything else plugged in, none of
// which a radio report needs, so such lines are left out of uploads entirely.
// An error or a warning naming a device is kept, with its instance id masked.
bool isUsbInventoryLine(const std::string& line) {
    const std::string low = lowerAscii(line);
    const bool listing = low.find("libusb: info") != std::string::npos ||
                         low.find("libusb: debug") != std::string::npos;
    if (!listing) { return false; }
    const std::size_t v = low.find("vid_");
    return v != std::string::npos && low.find("&pid_", v) != std::string::npos;
}

// The account name in a path: C:\Users\<name>\..., /home/<name>/...,
// /Users/<name>/... The segment after the key is replaced by <user> up to the
// next separator or quote (a Windows account name can contain spaces, so a
// space does not end it).
void maskUserDirs(std::string& s) {
    static constexpr char kUser[] = "<user>";
    // Lowercased ONCE, then kept in step with `s` by applying every edit to
    // BOTH: `kUser` is already lowercase, so writing it into `low` at the
    // same span costs nothing further and needs no re-scan of bytes already
    // passed. Recomputing lowerAscii(s) on every match - the original shape
    // of this loop - is O(line length) per match, and on the whole-bundle
    // pass problem_report.cpp added for the report-attachment preview
    // (rather than one ~192-byte ring line at a time) that is no longer a
    // rounding error: a bundle with dozens of path-bearing lines measured
    // multiple milliseconds in it alone (2026-09-28 review).
    std::string low = lowerAscii(s);
    std::size_t from = 0;
    for (;;) {
        std::size_t at = std::string::npos;
        std::size_t keyLen = 0;
        for (const char* k : {"\\users\\", "/users/", "/home/"}) {
            const std::size_t a = low.find(k, from);
            if (a < at) {
                at = a;
                keyLen = std::strlen(k);
            }
        }
        if (at == std::string::npos) { break; }
        const std::size_t p = at + keyLen;
        std::size_t e = p;
        while (e < s.size() && s[e] != '\\' && s[e] != '/' && s[e] != '\'' && s[e] != '"' &&
               s[e] != ')' && s[e] != ']' && s[e] != ',' && s[e] != ';') {
            ++e;
        }
        if (e > p) {
            s.replace(p, e - p, kUser);
            low.replace(p, e - p, kUser);
        }
        from = p + (sizeof(kUser) - 1);
    }
}

// Anything in single quotes is a NAME - a patch node's, a speaker's, a
// preset's - and names are typed by the user. The text between the quotes
// becomes <name>. An apostrophe inside a word ("the tuner's PLL") is not a
// quote: an opening quote must not follow a letter or digit, and a closing
// one must not be followed by one. A quote the line was cut off inside masks
// the rest of the line, because the name is whatever came after it.
void maskQuotedNames(std::string& s) {
    static const std::string kName = "<name>";
    std::size_t i = 0;
    while (i < s.size()) {
        if (s[i] != '\'' || (i > 0 && isAlnumChar(s[i - 1]))) {
            ++i;
            continue;
        }
        std::size_t close = std::string::npos;
        for (std::size_t j = i + 1; j < s.size(); ++j) {
            if (s[j] == '\'' && (j + 1 >= s.size() || !isAlnumChar(s[j + 1]))) {
                close = j;
                break;
            }
        }
        if (close == std::string::npos) {
            if (i + 1 < s.size()) { s.replace(i + 1, std::string::npos, kName); }
            break;
        }
        if (close == i + 1) {
            i = close + 1;
            continue;
        }
        // A quoted USB hardware id, its instance segment already stripped,
        // is which KIND of device a driver could not reach - not a name.
        const std::string inner = lowerAscii(s.substr(i + 1, close - (i + 1)));
        if (inner.rfind("usb\\vid_", 0) == 0 || inner.rfind("\\\\?\\usb#vid_", 0) == 0) {
            i = close + 1;
            continue;
        }
        s.replace(i + 1, close - (i + 1), kName);
        i = i + 1 + kName.size() + 1;
    }
}

// A POSSESSIVE NAME inside a PARENTHESISED label a vendor API handed back
// verbatim - "Headset (Alice's AirPods Pro)", "Microphone (Bob's iPhone)".
// Windows hands FoxSDR the OS's own device-friendly-name text unchanged, and
// that name is very often someone's Bluetooth or paired-phone label, not
// anything this application chose. Only the word immediately before an "'s"
// is masked - "AirPods Pro" and "iPhone" stay, because the make and model are
// what a report is actually diagnosing; the person's name never is. Scoped to
// text inside parentheses (not the whole line) so an ordinary contraction in
// prose elsewhere is never touched - this project's log lines do not use any
// today, but nothing should depend on that staying true forever.
void maskPossessiveWordsInPlace(std::string& span) {
    static const std::string kName = "<name>";
    std::size_t i = 0;
    while (i < span.size()) {
        if (span[i] != '\'' || i == 0 || !isAlnumChar(span[i - 1])) {
            ++i;
            continue;
        }
        if (i + 1 >= span.size() || (span[i + 1] != 's' && span[i + 1] != 'S')) {
            ++i;
            continue;
        }
        std::size_t wordStart = i;
        while (wordStart > 0 && isAlnumChar(span[wordStart - 1])) { --wordStart; }
        const std::size_t wordLen = i - wordStart;
        span.replace(wordStart, wordLen, kName);
        i = wordStart + kName.size() + 2;  // past "<name>" + "'s"
    }
}

void maskPossessiveNamesInParens(std::string& s) {
    std::size_t i = 0;
    while (i < s.size()) {
        if (s[i] != '(') {
            ++i;
            continue;
        }
        const std::size_t close = s.find(')', i + 1);
        if (close == std::string::npos) { break; }
        std::string inner = s.substr(i + 1, close - (i + 1));
        maskPossessiveWordsInPlace(inner);
        s.replace(i + 1, close - (i + 1), inner);
        i = i + 1 + inner.size() + 1;
    }
}

// --- NETWORK ADDRESSES AND HOST NAMES (0.99.59) ------------------------------
//
// PRIVACY.md has always said a report never carries your IP address or your
// machine name, and until 0.99.59 nothing enforced it: the Pluto's open line
// printed the address the user typed, its source name ("Pluto: ADALM-Pluto at
// ip:pluto.local") reached the tune lines, and a network driver (SoapyRemote,
// rtl_tcp, SpyServer) writes addresses and URLs of its own. Every IPv4 and
// IPv6 literal, and a host name wherever FoxSDR or such a driver puts one,
// becomes <host>. A port after it is kept: it says which service, not who.

constexpr char kHost[] = "<host>";

bool oneOf(const std::string& w, std::initializer_list<const char*> set);  // below

bool isHostChar(char c) { return isAlnumChar(c) || c == '-' || c == '_' || c == '.'; }

// A dotted name's last label that is a FILE's extension, not a domain: a
// module ("SoapyUHD.dll", "cascade.exe+0x1A2B") or a file a line names after
// "at", "to" or "from" is not a host.
bool isFileExtension(const std::string& lowLabel) {
    return oneOf(lowLabel, {"dll", "exe", "so", "dylib", "sys", "pdb", "json", "txt", "log", "ini",
                            "cfg", "conf", "xml", "yaml", "yml", "csv", "tsv", "wav", "mp3", "flac",
                            "ogg", "iq", "raw", "bin", "hex", "img", "ihx", "rbf", "fw", "cu8", "cs8",
                            "cs16", "cf32", "sigmf", "png", "jpg", "jpeg", "bmp", "gif", "zip", "gz",
                            "tar", "msix", "appx", "lnk", "dat", "db", "sqlite", "py", "js", "html",
                            "htm", "css", "md", "tmp", "bak", "cache", "lua", "toml", "pem", "crt"});
}

// "pluto.local", "sdr.example.org": two or more labels, the last of them two
// or more letters and not a file extension. A version ("v0.38", "1.0.0") ends
// in digits and a sentence's "e.g." in one letter, so neither is a host.
bool looksLikeDottedHost(const std::string& tok) {
    if (tok.empty() || !isAlnumChar(tok[0]) || tok.find("..") != std::string::npos) { return false; }
    const std::size_t dot = tok.rfind('.');
    if (dot == std::string::npos || dot + 1 >= tok.size()) { return false; }
    const std::string last = lowerAscii(tok.substr(dot + 1));
    if (last.size() < 2) { return false; }
    for (const char c : last) {
        if (!isAlphaChar(c)) { return false; }
    }
    return !isFileExtension(last);
}

// Four dotted decimal octets starting at `i`, each 0-255: their length, or 0.
// Not four (a version "0.99.58", or five parts) and not an octet over 255 (an
// OS build "10.0.22631.4317") is not an address.
std::size_t ipv4Length(const std::string& s, std::size_t i) {
    std::size_t p = i;
    for (int part = 0; part < 4; ++part) {
        if (part > 0) {
            if (p >= s.size() || s[p] != '.') { return 0; }
            ++p;
        }
        const std::size_t start = p;
        unsigned value = 0;
        while (p < s.size() && isDigitChar(s[p]) && p - start < 4) {
            value = value * 10u + static_cast<unsigned>(s[p] - '0');
            ++p;
        }
        if (p == start || p - start > 3 || value > 255u) { return 0; }
    }
    if (p < s.size() && (isAlphaChar(s[p]) || isDigitChar(s[p]) || s[p] == '_')) { return 0; }
    if (p + 1 < s.size() && s[p] == '.' && isDigitChar(s[p + 1])) { return 0; }
    return p - i;
}

// One of the colon-separated groups of an IPv6 address: 1-4 hex digits.
bool ipv6Group(const std::string& g) {
    if (g.empty() || g.size() > 4) { return false; }
    for (const char c : g) {
        if (std::isxdigit(static_cast<unsigned char>(c)) == 0) { return false; }
    }
    return true;
}

// An IPv6 literal, whole: eight groups, or fewer with one "::", the last
// group allowed to be a dotted IPv4 address (counting as two). A time
// ("12:34:56"), a USB VID:PID ("0bda:2838"), a MAC address (six groups) and
// C++'s "::" between words are none of these.
bool validIpv6(const std::string& t) {
    if (std::count(t.begin(), t.end(), ':') < 2) { return false; }
    if (std::none_of(t.begin(), t.end(), [](char c) { return isDigitChar(c); })) { return false; }
    if (t.find(":::") != std::string::npos) { return false; }
    const std::size_t dc = t.find("::");
    if (dc != std::string::npos && t.find("::", dc + 1) != std::string::npos) { return false; }
    std::vector<std::string> groups;
    auto split = [&groups](const std::string& part) {
        if (part.empty()) { return; }
        std::size_t a = 0;
        for (;;) {
            const std::size_t c = part.find(':', a);
            groups.push_back(part.substr(a, c == std::string::npos ? std::string::npos : c - a));
            if (c == std::string::npos) { break; }
            a = c + 1;
        }
    };
    if (dc != std::string::npos) {
        split(t.substr(0, dc));
        split(t.substr(dc + 2));
    } else {
        split(t);
    }
    std::size_t count = 0;
    for (std::size_t k = 0; k < groups.size(); ++k) {
        const std::string& g = groups[k];
        if (g.find('.') != std::string::npos) {
            if (k + 1 != groups.size() || ipv4Length(g, 0) != g.size()) { return false; }
            count += 2;
        } else {
            if (!ipv6Group(g)) { return false; }
            ++count;
        }
    }
    return dc != std::string::npos ? count <= 7 : count == 8;
}

// This machine talking to itself names nobody, and whether a listener was
// bound to every interface or to loopback only is worth keeping.
bool isOwnMachine(const std::string& host) {
    const std::string h = lowerAscii(host);
    if (h == "localhost" || h == "0.0.0.0" || h == "::" || h == "::1" || h == "[::1]") { return true; }
    return h.rfind("127.", 0) == 0 && ipv4Length(h, 0) == h.size();
}

// The word just before `start`, past spaces, ':', '=' and '(' - for a
// version printed after its word ("version 1.99.58.0", "package 1.99.58.0").
bool followsVersionWord(const std::string& s, std::size_t start) {
    std::size_t e = start;
    while (e > 0 && (s[e - 1] == ' ' || s[e - 1] == ':' || s[e - 1] == '=' || s[e - 1] == '(')) { --e; }
    std::size_t b = e;
    while (b > 0 && isAlnumChar(s[b - 1])) { --b; }
    if (b == e) { return false; }
    return oneOf(lowerAscii(s.substr(b, e - b)),
                 {"version", "ver", "v", "firmware", "fw", "build", "package", "api", "rev",
                  "revision", "release", "driver", "foxsdr", "msix"});
}

// The host-name token at `v` (letters, digits, '-', '_', '.'), up to a ':' or
// anything else; a full stop that ends a sentence is not part of it.
std::size_t hostTokenEnd(const std::string& s, std::size_t v) {
    std::size_t e = v;
    while (e < s.size() && isHostChar(s[e])) { ++e; }
    while (e > v && s[e - 1] == '.') { --e; }
    return e;
}

// 1. After a URL scheme ("tcp://", "rtsp://", "http://"...): the whole
//    authority - a user name and password included - becomes <host>, the
//    port and the path after it kept.
void maskUrlHosts(std::string& s) {
    std::size_t from = 0;
    for (;;) {
        const std::size_t at = s.find("://", from);
        if (at == std::string::npos) { return; }
        from = at + 3;
        std::size_t b = at;
        while (b > 0 && (isAlnumChar(s[b - 1]) || s[b - 1] == '+' || s[b - 1] == '-')) { --b; }
        if (at - b < 2 || !isAlphaChar(s[b])) { continue; }
        const std::size_t p = at + 3;
        std::size_t e = p;
        bool bracket = false;
        while (e < s.size()) {
            const char c = s[e];
            if (c == '[') { bracket = true; }
            if (c == ']') { bracket = false; }
            if (!bracket && (c == ' ' || c == '\t' || c == '"' || c == '\'' || c == '<' || c == '>' ||
                             c == ',' || c == ';' || c == '(' || c == ')' || c == '/' || c == '?' ||
                             c == '#' || c == '\\' || c == '\x01' || c == '\x02')) {
                break;
            }
            ++e;
        }
        while (e > p && s[e - 1] == '.') { --e; }
        if (e == p) { continue; }
        const std::string authority = s.substr(p, e - p);
        const std::size_t atSign = authority.rfind('@');
        const std::string hostPort = atSign == std::string::npos ? authority : authority.substr(atSign + 1);
        std::string host = hostPort;
        std::string port;
        if (!hostPort.empty() && hostPort[0] == '[') {
            const std::size_t close = hostPort.find(']');
            if (close != std::string::npos) {
                host = hostPort.substr(1, close - 1);
                port = hostPort.substr(close + 1);
            }
        } else {
            const std::size_t colon = hostPort.rfind(':');
            if (colon != std::string::npos) {
                host = hostPort.substr(0, colon);
                port = hostPort.substr(colon);
            }
        }
        // A port is ":" and digits; anything else stays part of what is masked.
        bool portOk = port.empty();
        if (!port.empty() && port[0] == ':' && port.size() > 1) {
            portOk = std::all_of(port.begin() + 1, port.end(), [](char c) { return isDigitChar(c); });
        }
        if (!portOk) {
            host = hostPort;
            port.clear();
        }
        if (atSign == std::string::npos && isOwnMachine(host)) { continue; }
        const std::string replacement = std::string(kHost) + port;
        s.replace(p, e - p, replacement);
        from = p + replacement.size();
    }
}

// 2. After a key a device-argument string or a driver writes a host with:
//    "ip:" (FoxSDR's own "Pluto at ip:<host>" and libiio's "ip:<host>" URI),
//    "host=", "hostname=", "remote=", "rtltcp=", "server=", "uri=", "addr=",
//    "address=". A single label - a machine name - is masked here as well as
//    a dotted one. A value that is itself a scheme ("uri=ip:...",
//    "remote=tcp://...") is left to the rule for that scheme.
void maskKeyedHosts(std::string& s) {
    for (const char* key : {"ip:", "host=", "hostname=", "remote=", "rtltcp=", "server=", "uri=",
                            "addr=", "address="}) {
        const std::size_t keyLen = std::strlen(key);
        std::size_t from = 0;
        for (;;) {
            const std::string low = lowerAscii(s);
            const std::size_t at = low.find(key, from);
            if (at == std::string::npos) { break; }
            from = at + keyLen;
            if (at > 0 && isAlnumChar(s[at - 1])) { continue; }
            std::size_t v = at + keyLen;
            const bool quoted = v < s.size() && s[v] == '"';
            if (quoted) { ++v; }
            const std::size_t e = hostTokenEnd(s, v);
            if (e == v) { continue; }
            if (!quoted && e + 1 < s.size() && s[e] == ':' && !isDigitChar(s[e + 1])) { continue; }
            if (isOwnMachine(s.substr(v, e - v))) { continue; }
            s.replace(v, e - v, kHost);
            from = v + std::strlen(kHost);
        }
    }
}

// 3. A host in a sentence: after " at ", " to " or " from " ("could not reach
//    the Pluto at sdr.example.org:30431", "Connecting to spy.example.net:5555")
//    when it is plainly one - a dotted name, or a name with a port after it.
//    "at 2400000 S/s", "at the edge", "to WFM" and "at cascade.exe+0x1A2B"
//    are not. Also a double-quoted dotted name ("could not find
//    \"pluto.local\""), anything double-quoted after the word "host", and a
//    name under one of the local-network suffixes (".local", ".lan",
//    ".home", ".internal"...) wherever it stands.
void maskSentenceHosts(std::string& s) {
    for (const char* lead : {" at ", " to ", " from "}) {
        const std::size_t leadLen = std::strlen(lead);
        std::size_t from = 0;
        for (;;) {
            const std::size_t at = lowerAscii(s).find(lead, from);
            if (at == std::string::npos) { break; }
            const std::size_t v = at + leadLen;
            from = v;
            const std::size_t e = hostTokenEnd(s, v);
            if (e == v) { continue; }
            const std::string tok = s.substr(v, e - v);
            const bool hasLetter = std::any_of(tok.begin(), tok.end(), [](char c) { return isAlphaChar(c); });
            const bool withPort = e + 1 < s.size() && s[e] == ':' && isDigitChar(s[e + 1]);
            if (!(looksLikeDottedHost(tok) || (hasLetter && withPort)) || isOwnMachine(tok)) { continue; }
            s.replace(v, e - v, kHost);
            from = v + std::strlen(kHost);
        }
    }
    // Double quotes: a dotted name on its own, or anything after "host ".
    std::size_t i = 0;
    while (i < s.size()) {
        if (s[i] != '"') {
            ++i;
            continue;
        }
        const std::size_t close = s.find('"', i + 1);
        if (close == std::string::npos) { break; }
        const std::string inner = s.substr(i + 1, close - (i + 1));
        const bool afterHostWord = i >= 5 && lowerAscii(s.substr(i - 5, 5)) == "host " &&
                                   (i == 5 || !isAlnumChar(s[i - 6]));
        const bool dotted = hostTokenEnd(inner, 0) == inner.size() && looksLikeDottedHost(inner);
        if (!inner.empty() && inner.find('"') == std::string::npos && (afterHostWord || dotted) &&
            !isOwnMachine(inner)) {
            s.replace(i + 1, inner.size(), kHost);
            i = i + 1 + std::strlen(kHost) + 1;
            continue;
        }
        i = close + 1;
    }
    // Local-network names, anywhere.
    i = 0;
    while (i < s.size()) {
        if (!isHostChar(s[i]) || (i > 0 && (isHostChar(s[i - 1]) || s[i - 1] == '/' || s[i - 1] == '\\'))) {
            ++i;
            continue;
        }
        const std::size_t e = hostTokenEnd(s, i);
        if (e == i) {
            ++i;
            continue;
        }
        const std::string tok = s.substr(i, e - i);
        const std::size_t dot = tok.rfind('.');
        if (dot != std::string::npos && dot > 0 && looksLikeDottedHost(tok) &&
            oneOf(lowerAscii(tok.substr(dot + 1)), {"local", "lan", "home", "internal", "localdomain",
                                                    "intranet", "corp", "arpa", "localnet"})) {
            s.replace(i, e - i, kHost);
            i += std::strlen(kHost);
            continue;
        }
        i = e;
    }
}

// 4. The machine in a Windows network path: "\\NAS-01\radio\plugins". The
//    device prefixes "\\.\" and "\\?\" are not machines.
void maskUncHosts(std::string& s) {
    std::size_t from = 0;
    for (;;) {
        const std::size_t at = s.find("\\\\", from);
        if (at == std::string::npos) { return; }
        from = at + 2;
        if (at > 0 && s[at - 1] == '\\') { continue; }
        const std::size_t v = at + 2;
        std::size_t e = v;
        while (e < s.size() && isHostChar(s[e])) { ++e; }
        if (e == v || e >= s.size() || s[e] != '\\') { continue; }
        const std::string tok = s.substr(v, e - v);
        if (tok == "." || tok == "?" || isOwnMachine(tok)) { continue; }
        s.replace(v, e - v, kHost);
        from = v + std::strlen(kHost);
    }
}

// 5. IPv6 literals, then IPv4 literals, anywhere on the line. A dotted quad
//    glued to a word ("FoxSDR_1.99.58.0_x64", "UHD_4.6.0.0") or printed after
//    a version word ("version 1.99.58.0") is a version, and stays.
void maskIpLiterals(std::string& s) {
    std::size_t i = 0;
    while (i < s.size()) {
        const char c = s[i];
        const bool start = (std::isxdigit(static_cast<unsigned char>(c)) != 0 || c == ':') &&
                           (i == 0 || !(isAlnumChar(s[i - 1]) || s[i - 1] == '_' || s[i - 1] == ':' ||
                                        s[i - 1] == '.'));
        if (!start) {
            ++i;
            continue;
        }
        std::size_t j = i;
        while (j < s.size() && (std::isxdigit(static_cast<unsigned char>(s[j])) != 0 || s[j] == ':' ||
                                s[j] == '.')) {
            ++j;
        }
        std::size_t e = j;
        while (e > i && (s[e - 1] == '.' || (s[e - 1] == ':' && !(e >= i + 2 && s[e - 2] == ':')))) { --e; }
        std::string cand = s.substr(i, e - i);
        if (!validIpv6(cand)) {
            i = j > i ? j : i + 1;
            continue;
        }
        std::size_t z = e;
        if (z < s.size() && s[z] == '%') {
            ++z;
            while (z < s.size() && (isAlnumChar(s[z]) || s[z] == '_' || s[z] == '-')) { ++z; }
        }
        if (z < s.size() && (isAlnumChar(s[z]) || s[z] == '_')) {
            i = j;
            continue;
        }
        if (isOwnMachine(cand)) {
            i = z;
            continue;
        }
        s.replace(i, z - i, kHost);
        i += std::strlen(kHost);
    }
    i = 0;
    while (i < s.size()) {
        if (!isDigitChar(s[i]) ||
            (i > 0 && (isAlnumChar(s[i - 1]) || s[i - 1] == '_' || s[i - 1] == '.'))) {
            ++i;
            continue;
        }
        const std::size_t n = ipv4Length(s, i);
        if (n == 0 || followsVersionWord(s, i) || isOwnMachine(s.substr(i, n))) {
            while (i < s.size() && (isDigitChar(s[i]) || s[i] == '.')) { ++i; }
            continue;
        }
        s.replace(i, n, kHost);
        i += std::strlen(kHost);
    }
}

void maskNetworkAddresses(std::string& s) {
    maskUrlHosts(s);
    maskKeyedHosts(s);
    maskSentenceHosts(s);
    maskUncHosts(s);
    maskIpLiterals(s);
}

// Words that make a line one whose unlabelled numbers might be a frequency.
bool mentionsFrequency(const std::string& low) {
    for (const char* k : {"hz", "freq", "tune", "tuning", "centre", "center", "vfo", "asked for",
                          "answered", "range", "transmit", "keyed", "preset", "offset",
                          "carrier"}) {
        if (low.find(k) != std::string::npos) { return true; }
    }
    return false;
}

bool oneOf(const std::string& w, std::initializer_list<const char*> set) {
    for (const char* s : set) {
        if (w == s) { return true; }
    }
    return false;
}

// Units that say a number is NOT a frequency: sample rates, time, level,
// size, counts. A number carrying one survives on any line.
bool isSafeUnit(const std::string& u) {
    return oneOf(u, {"s/s",   "ks/s",   "ms/s",   "gs/s",    "sps",   "ksps",  "msps",   "gsps",
                     "ms",    "s",      "sec",    "secs",    "second", "seconds", "us",   "\xc2\xb5s",
                     "ns",    "min",    "mins",   "minutes", "db",    "dbm",   "dbfs",   "%",
                     "bytes", "byte",   "b",      "kb",      "kib",   "mb",    "mib",    "gb",
                     "gib",   "bit",    "bits",   "lines",   "line",  "blocks", "block", "samples",
                     "sample", "frames", "frame", "times",   "tries", "attempts", "tune", "tunes",
                     "ppm",   "x"});
}

bool isHertzUnit(const std::string& u) { return oneOf(u, {"hz", "khz", "mhz", "ghz", "thz"}); }

// A number directly after one of these words is an identifier or a code,
// never a frequency: "firmware 2.1", "board id 0", "tuner 2", "error 5".
bool followsSafeWord(const std::string& s, std::size_t start) {
    std::size_t e = start;
    while (e > 0 && (s[e - 1] == ' ' || s[e - 1] == ':' || s[e - 1] == '=')) { --e; }
    std::size_t b = e;
    while (b > 0 && isAlnumChar(s[b - 1])) { --b; }
    if (b == e) { return false; }
    const std::string w = lowerAscii(s.substr(b, e - b));
    return oneOf(w, {"firmware", "version", "ver", "api", "build", "rev", "revision", "id",
                     "tuner", "error", "err", "errno", "code", "exit", "status", "rc", "attempt",
                     "retry"});
}

// DEFAULT DENY, for a line that mentions a frequency: every free-standing
// number becomes "#" unless something says it is not a frequency - a safe
// unit, a safe word before it, a 0x prefix, or a small negative integer (a
// driver error code, "(-5)"). A number glued to a word is part of a name
// (B200, R820T, v1.0.0-rc10, AD9363) and is left alone. A number carrying a
// hertz unit is masked whatever else is true of it.
void maskNumbers(std::string& s) {
    std::string out;
    out.reserve(s.size());
    const std::size_t n = s.size();
    std::size_t i = 0;
    while (i < n) {
        const char c = s[i];
        const bool prevWordy = i > 0 && (isAlnumChar(s[i - 1]) || s[i - 1] == '_');
        const bool signStart = (c == '-' || c == '+') && i + 1 < n && isDigitChar(s[i + 1]) &&
                               !prevWordy && !(i > 0 && s[i - 1] == '.');
        if (!isDigitChar(c) && !signStart) {
            out += c;
            ++i;
            continue;
        }
        if (isDigitChar(c) && prevWordy) {
            // Inside an identifier: copy the rest of it untouched.
            std::size_t e = i;
            while (e < n && (isAlnumChar(s[e]) || s[e] == '_' || s[e] == '.' || s[e] == '-')) { ++e; }
            out.append(s, i, e - i);
            i = e;
            continue;
        }
        const std::size_t start = i;
        if (c == '0' && i + 2 < n && (s[i + 1] == 'x' || s[i + 1] == 'X') &&
            std::isxdigit(static_cast<unsigned char>(s[i + 2])) != 0) {
            std::size_t e = i + 2;
            while (e < n && std::isxdigit(static_cast<unsigned char>(s[e])) != 0) { ++e; }
            out.append(s, i, e - i);
            i = e;
            continue;
        }
        if (signStart) { ++i; }
        bool fraction = false;
        while (i < n && isDigitChar(s[i])) { ++i; }
        while (i + 1 < n && (s[i] == '.' || s[i] == ',') && isDigitChar(s[i + 1])) {
            fraction = true;
            ++i;
            while (i < n && isDigitChar(s[i])) { ++i; }
        }
        if (i < n && (s[i] == 'e' || s[i] == 'E')) {
            std::size_t j = i + 1;
            if (j < n && (s[j] == '+' || s[j] == '-')) { ++j; }
            if (j < n && isDigitChar(s[j])) {
                i = j;
                while (i < n && isDigitChar(s[i])) { ++i; }
                fraction = true;
            }
        }
        const std::size_t end = i;
        std::size_t u = end;
        if (u < n && s[u] == ' ') { ++u; }
        std::size_t ue = u;
        while (ue < n && (isAlphaChar(s[ue]) || s[ue] == '/' || s[ue] == '%' ||
                          static_cast<unsigned char>(s[ue]) == 0xC2 ||
                          static_cast<unsigned char>(s[ue]) == 0xB5)) {
            ++ue;
        }
        if (u == end && ue > u && ue < n && (isDigitChar(s[ue]) || s[ue] == '_')) {
            // Digits, letters, digits with no space: a hex id or a part
            // number ("651FD5EB77...", "2832U2"), not a quantity with a unit.
            std::size_t e = ue;
            while (e < n && (isAlnumChar(s[e]) || s[e] == '_')) { ++e; }
            out.append(s, start, e - start);
            i = e;
            continue;
        }
        const std::string unit = lowerAscii(s.substr(u, ue - u));
        const std::size_t digits = end - start - (signStart ? 1u : 0u);
        bool keep = false;
        if (isHertzUnit(unit)) {
            keep = false;
        } else if (isSafeUnit(unit)) {
            keep = true;
        } else if (s[start] == '-' && !fraction && digits <= 4) {
            keep = true;
        } else if (followsSafeWord(s, start)) {
            keep = true;
        }
        if (keep) {
            out.append(s, start, end - start);
        } else {
            out += '#';
        }
    }
    s.swap(out);
}

// "###.######" -> "#". A run of masked digits still says how many there were,
// and the count of digits in a frequency says which band it is in. Any run of
// '#' with only '.' and ',' between them becomes one '#'.
void collapseMasks(std::string& s) {
    std::string out;
    out.reserve(s.size());
    std::size_t i = 0;
    while (i < s.size()) {
        if (s[i] != '#') {
            out += s[i++];
            continue;
        }
        std::size_t last = i;
        std::size_t j = i + 1;
        while (j < s.size() && (s[j] == '#' || s[j] == '.' || s[j] == ',')) {
            if (s[j] == '#') { last = j; }
            ++j;
        }
        out += '#';
        i = last + 1;
    }
    s.swap(out);
}

// The DiagLog stamp, "HH:MM:SS.mmm", which is not data about anybody.
std::size_t stampLength(const std::string& s) {
    if (s.size() < 12) { return 0; }
    static const char kShape[] = "dd:dd:dd.ddd";
    for (std::size_t k = 0; k < 12; ++k) {
        if (kShape[k] == 'd' ? !isDigitChar(s[k]) : s[k] != kShape[k]) { return 0; }
    }
    return 12;
}

// THE NAMES A REPORT ALREADY LISTS, as scrubUploadLog(lines, inventory) keeps
// them: each "name version" entry and the name without its version, longest
// first so the whole entry wins over its own name. Only entries with a letter
// in them and at least three characters: a bare number is exactly what the
// rule below exists to mask, whoever claims it as a name.
std::vector<std::string> namesToKeep(const std::vector<std::string>& inventory) {
    std::vector<std::string> out;
    auto add = [&out](const std::string& s) {
        if (s.size() < 3) { return; }
        bool letter = false;
        for (const char c : s) { letter = letter || isAlphaChar(c); }
        if (!letter) { return; }
        if (std::find(out.begin(), out.end(), s) == out.end()) { out.push_back(s); }
    };
    for (const std::string& e : inventory) {
        add(e);
        const std::size_t sp = e.find_last_of(' ');
        if (sp != std::string::npos && sp > 0) { add(e.substr(0, sp)); }
    }
    std::stable_sort(out.begin(), out.end(),
                     [](const std::string& a, const std::string& b) { return a.size() > b.size(); });
    return out;
}

// A kept name is swapped for a token while the rules run and put back after.
// The token is \x01, letters from {Q,W,X,J} (the index in base 4) and \x02:
// no digit for the number rule to mask, and no letter run that could spell
// any of mentionsFrequency's words, so a token neither is masked nor makes
// its line look like one that names a frequency.
std::string keepToken(std::size_t index) {
    static const char kDigits[4] = {'Q', 'W', 'X', 'J'};
    std::string t(1, '\x01');
    do {
        t += kDigits[index % 4u];
        index /= 4u;
    } while (index != 0);
    t += '\x02';
    return t;
}

std::string scrubLineKeeping(const std::string& line, const std::vector<std::string>& keep) {
    const std::size_t stamp = stampLength(line);
    std::string body = line.substr(stamp);
    // THE FREQUENCY JUDGEMENT READS THIS, UNTOUCHED BY THE KEEP SUBSTITUTION
    // BELOW (0.99.44 repair). A kept name can itself carry one of
    // mentionsFrequency's words - "406 MHz Beacons" reads exactly like a tuned
    // frequency - and once that name is swapped for its token the word is
    // gone from `body`, so a line whose ONLY frequency word was inside the
    // kept name looked like it never mentioned one at all, and a genuine bare
    // frequency elsewhere on the same line ("... 406 MHz Beacons 1.0.0 ...
    // 144800000 ...") escaped masking entirely. Judging the ORIGINAL text
    // means the kept name still marks its line as frequency-bearing even
    // after its own token has taken its place.
    const std::string originalBody = body;
    // Only where the name stands on its own: "406 MHz Beacons" inside
    // "X406 MHz Beacons2" is somebody else's text.
    std::vector<std::pair<std::string, std::string>> held;
    for (const std::string& name : keep) {
        std::size_t at = 0;
        while ((at = body.find(name, at)) != std::string::npos) {
            const std::size_t end = at + name.size();
            const bool freeStart = at == 0 || !isAlnumChar(body[at - 1]);
            const bool freeEnd = end >= body.size() || !isAlnumChar(body[end]);
            if (!freeStart || !freeEnd) {
                ++at;
                continue;
            }
            const std::string token = keepToken(held.size());
            held.emplace_back(token, name);
            body.replace(at, name.size(), token);
            at += token.size();
        }
    }
    stripSerials(body);
    maskUsbInstanceIds(body);
    maskSoapyLabelSerials(body);
    maskUserDirs(body);
    maskNetworkAddresses(body);
    maskQuotedNames(body);
    maskPossessiveNamesInParens(body);
    // A line at the ring's width was CUT: whatever named its numbers may be
    // in the part that was lost ("... at 2048000 S/s, 127.825" with the
    // " MHz" gone), so it is treated as naming a frequency.
    const bool cut = line.size() >= static_cast<std::size_t>(DiagLog::kLineBytes) - 1u;
    if (cut || mentionsFrequency(lowerAscii(originalBody))) { maskNumbers(body); }
    collapseMasks(body);
    // The kept names back, each where its token still stands. A token some
    // rule consumed (a name inside quotes becomes '<name>') stays consumed,
    // which errs the safe way.
    for (const auto& [token, name] : held) {
        const std::size_t at = body.find(token);
        if (at != std::string::npos) { body.replace(at, token.size(), name); }
    }
    return line.substr(0, stamp) + body;
}

}  // namespace

std::string scrubUploadLine(const std::string& line) {
    return scrubLineKeeping(line, std::vector<std::string>());
}

std::string scrubUploadPath(const std::string& path) {
    std::string out = path;
    // 1. The base directory, written as the variable it came from. This finds
    //    the account name wherever the profile lives (a redirected profile on
    //    D:\ has no \Users\ in it for step 2 to find).
#if defined(_WIN32)
    const char* const vars[] = {"LOCALAPPDATA"};
#else
    const char* const vars[] = {"XDG_STATE_HOME", "HOME"};
#endif
    for (const char* var : vars) {
        const char* v = std::getenv(var);
        if (v == nullptr) { continue; }
        std::string base = v;
        while (base.size() > 1 && (base.back() == '\\' || base.back() == '/')) { base.pop_back(); }
        if (base.size() < 2 || out.size() < base.size()) { continue; }
#if defined(_WIN32)
        const bool prefix = lowerAscii(out.substr(0, base.size())) == lowerAscii(base);
        const std::string name = std::string("%") + var + "%";
#else
        const bool prefix = out.compare(0, base.size(), base) == 0;
        const std::string name = std::string("$") + var;
#endif
        const bool whole = out.size() == base.size() || out[base.size()] == '\\' ||
                           out[base.size()] == '/';
        if (prefix && whole) {
            out = name + out.substr(base.size());
            break;
        }
    }
    // 2. Whatever is left (a FOXSDR_DIAG_DIR override, say) is still shown,
    //    because where the files ARE is the point of the field - with the
    //    account name masked by the same rule a log line gets.
    maskUserDirs(out);
    return out;
}

std::string maskAccountNames(const std::string& text) {
    std::string out = text;
    maskUserDirs(out);
    return out;
}

std::vector<std::string> scrubUploadLog(const std::vector<std::string>& lines) {
    return scrubUploadLog(lines, std::vector<std::string>());
}

std::vector<std::string> scrubUploadLog(const std::vector<std::string>& lines,
                                        const std::vector<std::string>& inventory) {
    const std::vector<std::string> keep = namesToKeep(inventory);
    std::vector<std::string> out;
    out.reserve(lines.size());
    std::size_t i = 0;
    while (i < lines.size()) {
        if (!isUsbInventoryLine(lines[i])) {
            out.push_back(scrubLineKeeping(lines[i], keep));
            ++i;
            continue;
        }
        // A run of listing lines becomes ONE line that says how many were left
        // out, stamped like the first of them, so a reader knows the driver
        // was listing devices and that the report is not hiding anything else.
        const std::size_t first = i;
        while (i < lines.size() && isUsbInventoryLine(lines[i])) { ++i; }
        const std::size_t stamp = stampLength(lines[first]);
        std::string marker = lines[first].substr(0, stamp);
        if (stamp > 0) { marker += " info "; }
        marker += "vendor: libusb: " + std::to_string(i - first) +
                  ((i - first) == 1 ? " line" : " lines") +
                  " listing this machine's USB devices left out";
        out.push_back(marker);
    }
    return out;
}

std::string scrubVendorLine(const std::string& line) {
    std::string out = line;

    // Serial numbers - the same rule every uploaded line gets, see
    // stripSerials above.
    stripSerials(out);

    // Anything that names a frequency, tuning or hertz has its digits masked.
    // What somebody listens to must never reach a report, and a driver's
    // "Setting center freq: 101100000" is exactly that.
    const std::string low = lowerAscii(out);
    if (low.find("freq") != std::string::npos || low.find("tune") != std::string::npos ||
        low.find("hz") != std::string::npos) {
        for (char& c : out) {
            if (c >= '0' && c <= '9') { c = '#'; }
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// stderr capture
// ---------------------------------------------------------------------------
#if defined(_WIN32)
namespace {

std::mutex g_captureMutex;
bool g_captureActive = false;
HANDLE g_origStderr = nullptr;

void emitVendorLine(LineRateLimiter& limiter, const std::string& raw) {
    std::uint64_t suppressed = 0;
    const bool ok = limiter.admit(::GetTickCount64(), suppressed);
    if (suppressed != 0) {
        DiagLog::instance().writef("info", "vendor: %llu more lines suppressed",
                                   static_cast<unsigned long long>(suppressed));
    }
    if (ok) {
        DiagLog::instance().writef("info", "vendor: %s", scrubVendorLine(raw).c_str());
    }
}

// The reader. Blocks in ReadFile for the life of the process; the write end
// is fd 2 and is never closed, so this thread ends when the process does.
void stderrReaderMain(HANDLE rd) {
    LineRateLimiter limiter;
    std::string pending;
    char buf[1024];
    for (;;) {
        DWORD n = 0;
        if (::ReadFile(rd, buf, sizeof(buf), &n, nullptr) == 0 || n == 0) { break; }
        pending.append(buf, n);
        std::size_t at = 0;
        for (;;) {
            const std::size_t nl = pending.find('\n', at);
            if (nl == std::string::npos) { break; }
            std::string line = pending.substr(at, nl - at);
            at = nl + 1;
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
                line.pop_back();
            }
            if (!line.empty()) { emitVendorLine(limiter, line); }
        }
        pending.erase(0, at);
        // A driver that never sends a newline (UHD's "O" and "D" indicators
        // are single characters) must not be able to hold a line back for
        // ever, nor grow it without bound.
        if (pending.size() >= 512) {
            emitVendorLine(limiter, pending);
            pending.clear();
        }
    }
    ::CloseHandle(rd);
}

}  // namespace
#endif

bool stderrIsWatchedConsole() {
#if defined(_WIN32)
    HANDLE h = ::GetStdHandle(STD_ERROR_HANDLE);
    if (h == nullptr || h == INVALID_HANDLE_VALUE) { return false; }
    if (::GetFileType(h) != FILE_TYPE_CHAR) { return false; }
    DWORD mode = 0;
    // A character device that is not a console (NUL, a serial port) has no
    // reader either.
    if (::GetConsoleMode(h, &mode) == 0) { return false; }
    // THE DISTINCTION THAT MATTERS. cascade.exe is a console-subsystem binary,
    // so a Start Menu launch gets a console too - one Windows created for it,
    // with nothing else attached and nobody reading it. A terminal launch
    // shares the shell's console, so the shell is on the list as well. Only
    // the second is a person watching.
    DWORD pids[4] = {};
    const DWORD n = ::GetConsoleProcessList(pids, 4);
    return n > 1;
#else
    return false;
#endif
}

bool installStderrCapture(bool evenIfConsole) {
#if defined(_WIN32)
    std::lock_guard<std::mutex> lk(g_captureMutex);
    if (g_captureActive) { return true; }
    if (!evenIfConsole && stderrIsWatchedConsole()) { return false; }

    // The write end is INHERITABLE on purpose: the device-enumeration child
    // (soapy_enum_proc.cpp) is handed the parent's stderr so that UHD's
    // discovery errors go where they always went - which is now this log.
    // The read end must not be, or a child would hold it open.
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE rd = nullptr;
    HANDLE wr = nullptr;
    if (::CreatePipe(&rd, &wr, &sa, 0) == 0) { return false; }
    ::SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    // A PRIVATE duplicate of the old stderr, taken before the CRT's fd 2 is
    // replaced: _dup2 closes whatever fd 2 held, and the CRT's copy is the
    // same handle GetStdHandle returns. The crash handler writes its one
    // attribution line to this so it does not come back through the pipe.
    HANDLE orig = ::GetStdHandle(STD_ERROR_HANDLE);
    HANDLE origDup = nullptr;
    if (orig != nullptr && orig != INVALID_HANDLE_VALUE) {
        if (::DuplicateHandle(::GetCurrentProcess(), orig, ::GetCurrentProcess(), &origDup, 0,
                              FALSE, DUPLICATE_SAME_ACCESS) == 0) {
            origDup = nullptr;
        }
    }

    // The CRT first: fd 2 is what fprintf(stderr) in this process and in every
    // DLL sharing ucrtbase writes to. `wfd` is kept open deliberately - closing
    // it would close `wr`, which the std handle below hands to later-loaded
    // vendor modules with their own CRT.
    const int wfd = ::_open_osfhandle(reinterpret_cast<intptr_t>(wr), _O_WRONLY | _O_BINARY);
    if (wfd < 0) {
        ::CloseHandle(rd);
        ::CloseHandle(wr);
        if (origDup != nullptr) { ::CloseHandle(origDup); }
        return false;
    }
    if (::_dup2(wfd, 2) != 0) {
        ::_close(wfd);
        ::CloseHandle(rd);
        if (origDup != nullptr) { ::CloseHandle(origDup); }
        return false;
    }
    // Then the process std handle, which is what a vendor module with its own
    // statically linked CRT reads when it is loaded (all of them are loaded
    // after this point - at the first enumeration - so they inherit the pipe).
    ::SetStdHandle(STD_ERROR_HANDLE, wr);
    // stderr is unbuffered by contract, but a redirected one can be given a
    // buffer by code that does not know about this capture; a driver's line
    // must be in the pipe before the driver goes on to fault.
    std::setvbuf(stderr, nullptr, _IONBF, 0);

    g_origStderr = origDup;
    std::thread(stderrReaderMain, rd).detach();
    g_captureActive = true;
    diagLogf("diag: stderr capture on - driver and library lines are recorded as vendor:");
    return true;
#else
    (void)evenIfConsole;
    return false;
#endif
}

bool stderrCaptureActive() {
#if defined(_WIN32)
    std::lock_guard<std::mutex> lk(g_captureMutex);
    return g_captureActive;
#else
    return false;
#endif
}

void* originalStderrHandle() {
#if defined(_WIN32)
    // No lock: the crash handler reads this and must not wait on a mutex
    // another thread may hold. Both values are written once, before the
    // capture is announced active, and never again.
    return g_captureActive ? static_cast<void*>(g_origStderr) : nullptr;
#else
    return nullptr;
#endif
}

}  // namespace cascade::core
