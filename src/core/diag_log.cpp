// See diag_log.hpp for why the ring exists as well as the file, why the ring
// is fixed storage, and why the crash-path reader takes no lock.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/diag_log.hpp"

#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <chrono>
#include <filesystem>
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

}  // namespace

std::string scrubVendorLine(const std::string& line) {
    static constexpr char kStripped[] = "<stripped>";
    std::string out = line;

    // Serial numbers: "serial=EDR04ZDB2", "Serial: 00000001", "serial number
    // 1234". The value after the word is replaced; the word stays so a reader
    // still knows the driver named one. The lower-case shadow is rebuilt after
    // every replacement because the replacement changes the offsets.
    std::size_t from = 0;
    for (;;) {
        const std::string low = lowerAscii(out);
        const std::size_t at = low.find("serial", from);
        if (at == std::string::npos) { break; }
        std::size_t p = at + 6;
        auto skipSep = [&]() {
            while (p < out.size() && (out[p] == ' ' || out[p] == ':' || out[p] == '=' ||
                                      out[p] == '#' || out[p] == '\t')) {
                ++p;
            }
        };
        skipSep();
        // "serial number 1234" / "serial no 1234": the word between is not the
        // value.
        if (low.compare(p, 6, "number") == 0) {
            p += 6;
            skipSep();
        } else if (low.compare(p, 2, "no") == 0 &&
                   (p + 2 >= low.size() || !isTokenChar(low[p + 2]))) {
            p += 2;
            skipSep();
        }
        std::size_t e = p;
        while (e < out.size() && isTokenChar(out[e])) { ++e; }
        if (e > p) {
            out.replace(p, e - p, kStripped);
            from = p + (sizeof(kStripped) - 1);
        } else {
            from = p;
        }
    }

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
