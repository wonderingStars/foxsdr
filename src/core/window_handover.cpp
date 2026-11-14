// window_handover.cpp - see window_handover.hpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/window_handover.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "core/config_lock.hpp"
#include "core/diag_log.hpp"
#include "core/i18n.hpp"
#include "core/utf8_text.hpp"

namespace fs = std::filesystem;
using cascade::i18n::tr;

namespace cascade::core::handover {

namespace {

unsigned long currentPid() {
#if defined(_WIN32)
    return static_cast<unsigned long>(::GetCurrentProcessId());
#else
    return static_cast<unsigned long>(::getpid());
#endif
}

std::string envOr(const char* name) {
    const char* v = std::getenv(name);
    return v != nullptr ? std::string(v) : std::string();
}

void setEnvVar(const char* name, const std::string& value) {
#if defined(_WIN32)
    ::SetEnvironmentVariableA(name, value.empty() ? nullptr : value.c_str());
    _putenv_s(name, value.c_str());
#else
    if (value.empty()) { ::unsetenv(name); } else { ::setenv(name, value.c_str(), 1); }
#endif
}

// The folder this program runs from; where its sibling is.
fs::path ownDir() {
#if defined(_WIN32)
    std::vector<wchar_t> buf(32768);
    const DWORD n = ::GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    if (n == 0 || n >= buf.size()) { return fs::path(); }
    return fs::path(std::wstring(buf.data(), n)).parent_path();
#else
    std::error_code ec;
    return fs::read_symlink("/proc/self/exe", ec).parent_path();
#endif
}

fs::path targetExe(Window target) {
    const std::string over = envOr(target == Window::Legacy ? kLegacyExeEnv : kNewExeEnv);
    if (!over.empty()) { return fs::path(over); }
    return ownDir() / programName(target);
}

std::string lowerTrim(const std::string& s) {
    std::string t;
    for (const char c : s) {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { continue; }
        t.push_back(static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c));
    }
    return t;
}

// The three sentences, in the window the person is in NOW (the one that did not open the other).
std::string missingSentence(Window target) {
    return target == Window::Legacy
               ? std::string(tr("The Legacy window could not be opened: its program (cascade.exe) was not found beside this one. You are still in the new window."))
               : std::string(tr("The new window could not be opened: its program (foxsdr-slint.exe) was not found beside this one. You are still in the Legacy window."));
}
std::string failedSentence(Window target, unsigned long err) {
    return cascade::core::formatText(target == Window::Legacy ? tr("The Legacy window could not be started (Windows error %lu). You are back in the new window.")
                                                              : tr("The new window could not be started (Windows error %lu). You are back in the Legacy window."),
                                     err);
}
std::string lockedSentence(Window target) {
    return target == Window::Legacy ? std::string(tr("The settings file was still in use, so the Legacy window was not started. You are back in the new window."))
                                    : std::string(tr("The settings file was still in use, so the new window was not started. You are back in the Legacy window."));
}

#if defined(_WIN32)
// Starts `exe` with `args`, with the redirected standard handles passed on (a pipe or a file: a test, a script) and no other inheritable handle, and does not wait.
bool startProgram(const fs::path& exe, const std::string& args, const std::string& childLog, unsigned long& error) {
    std::wstring cmd = L"\"" + exe.wstring() + L"\"";
    if (!args.empty()) {
        const int n = ::MultiByteToWideChar(CP_UTF8, 0, args.c_str(), static_cast<int>(args.size()), nullptr, 0);
        std::wstring w(static_cast<std::size_t>(n > 0 ? n : 0), L'\0');
        if (n > 0) { ::MultiByteToWideChar(CP_UTF8, 0, args.c_str(), static_cast<int>(args.size()), w.data(), n); }
        cmd += L" " + w;
    }
    std::vector<HANDLE> pass;
    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof(si);
    HANDLE* slots[] = {&si.StartupInfo.hStdInput, &si.StartupInfo.hStdOutput, &si.StartupInfo.hStdError};
    const DWORD ids[] = {STD_INPUT_HANDLE, STD_OUTPUT_HANDLE, STD_ERROR_HANDLE};
    for (int i = 0; i < 3; ++i) {
        HANDLE h = ::GetStdHandle(ids[i]);
        const DWORD type = (h != nullptr && h != INVALID_HANDLE_VALUE) ? ::GetFileType(h) : FILE_TYPE_UNKNOWN;
        if (type != FILE_TYPE_DISK && type != FILE_TYPE_PIPE) { continue; }
        ::SetHandleInformation(h, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
        *slots[i] = h;
        bool seen = false;
        for (const HANDLE p : pass) { seen = seen || p == h; }
        if (!seen) { pass.push_back(h); }
    }
    // TEST SEAM (kChildLogEnv): the started program's output goes to this file, appended.
    HANDLE logHandle = INVALID_HANDLE_VALUE;
    if (!childLog.empty()) {
        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;
        logHandle = ::CreateFileA(childLog.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (logHandle != INVALID_HANDLE_VALUE) {
            si.StartupInfo.hStdOutput = logHandle;
            si.StartupInfo.hStdError = logHandle;
            pass.clear();
            pass.push_back(logHandle);
            if (si.StartupInfo.hStdInput != nullptr) { si.StartupInfo.hStdInput = nullptr; }
        }
    }
    std::vector<unsigned char> attrBuf;
    LPPROC_THREAD_ATTRIBUTE_LIST attrs = nullptr;
    DWORD flags = 0;
    if (!pass.empty()) {
        SIZE_T size = 0;
        ::InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
        attrBuf.resize(size);
        attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrBuf.data());
        if (::InitializeProcThreadAttributeList(attrs, 1, 0, &size) &&
            ::UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, pass.data(), pass.size() * sizeof(HANDLE), nullptr, nullptr)) {
            si.StartupInfo.dwFlags |= STARTF_USESTDHANDLES;
            si.lpAttributeList = attrs;
            flags |= EXTENDED_STARTUPINFO_PRESENT;
        } else {
            attrs = nullptr;
        }
    }
    PROCESS_INFORMATION pi{};
    const std::wstring dir = exe.parent_path().wstring();
    const BOOL ok = ::CreateProcessW(exe.wstring().c_str(), cmd.data(), nullptr, nullptr, attrs != nullptr ? TRUE : FALSE, flags, nullptr, dir.empty() ? nullptr : dir.c_str(),
                                     &si.StartupInfo, &pi);
    if (attrs != nullptr) { ::DeleteProcThreadAttributeList(attrs); }
    if (logHandle != INVALID_HANDLE_VALUE) { ::CloseHandle(logHandle); }
    if (!ok) {
        error = ::GetLastError();
        return false;
    }
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
    return true;
}
#else
bool startProgram(const fs::path&, const std::string&, const std::string&, unsigned long& error) {
    error = 1;   // nothing starts off Windows: the other window is Windows' own
    return false;
}
#endif

}  // namespace

const char* windowWord(Window w) { return w == Window::Legacy ? "legacy" : "new"; }
const char* programName(Window w) {
#if defined(_WIN32)
    return w == Window::Legacy ? "cascade.exe" : "foxsdr-slint.exe";
#else
    return w == Window::Legacy ? "cascade" : "foxsdr-slint";
#endif
}
Window other(Window w) { return w == Window::Legacy ? Window::New : Window::Legacy; }
const char* lockWord(Lock l) { return l == Lock::Free ? "free" : l == Lock::HeldBySelf ? "held-by-self" : "held-by-other"; }

// ---- the remembered choice ---------------------------------------------------------------------------------------------------------------------------------------------

std::string choicePath(const std::string& configPath) {
    if (configPath.empty()) { return std::string(); }
    return (fs::path(configPath).parent_path() / "window-choice").string();
}

Window readChoice(const std::string& configPath) {
    const std::string p = choicePath(configPath);
    if (p.empty()) { return Window::New; }
    std::ifstream f(p, std::ios::binary);
    if (!f) { return Window::New; }
    std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (text.size() > 64) { return Window::New; }
    return lowerTrim(text) == "legacy" ? Window::Legacy : Window::New;
}

bool writeChoice(const std::string& configPath, Window w) {
    const std::string p = choicePath(configPath);
    if (p.empty()) { return false; }
    std::error_code ec;
    fs::create_directories(fs::path(p).parent_path(), ec);
    const std::string tmp = p + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) { return false; }
        f << windowWord(w) << "\n";
        f.flush();
        if (!f) { return false; }
    }
    fs::rename(tmp, p, ec);
    if (ec) {
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

// ---- the mark -----------------------------------------------------------------------------------------------------------------------------------------------------------------

Marker takeMarker() {
    Marker m;
    const char* v = std::getenv(kFromEnv);
    if (v == nullptr || *v == '\0') { return m; }
    const std::string s(v);
    setEnvVar(kFromEnv, "");   // removed from both copies of the environment, so nothing this process starts inherits it
    const std::size_t a = s.find(':');
    if (a == std::string::npos || a == 0) { return m; }   // a process id of at least one digit
    const std::size_t b = s.find(':', a + 1);
    unsigned long pid = 0;
    for (std::size_t i = 0; i < a && i < 10; ++i) {
        if (s[i] < '0' || s[i] > '9') { return m; }
        pid = pid * 10 + static_cast<unsigned long>(s[i] - '0');
    }
    const std::string from = s.substr(a + 1, b == std::string::npos ? std::string::npos : b - a - 1);
    if (from != "new" && from != "legacy") { return m; }
    m.present = true;
    m.parentPid = pid;
    m.from = from == "legacy" ? Window::Legacy : Window::New;
    if (b != std::string::npos) { m.note = s.substr(b + 1, 40); }
    return m;
}

std::string noteSentence(const Marker& m, Window now) {
    if (!m.present || m.note.empty()) { return std::string(); }
    const Window target = other(now);   // the window the person asked for and did not get
    if (m.note == "missing") { return missingSentence(target); }
    if (m.note == "locked") { return lockedSentence(target); }
    if (m.note.rfind("failed:", 0) == 0) {
        unsigned long err = 0;
        for (std::size_t i = 7; i < m.note.size() && i < 17; ++i) {
            if (m.note[i] < '0' || m.note[i] > '9') { break; }
            err = err * 10 + static_cast<unsigned long>(m.note[i] - '0');
        }
        return failedSentence(target, err);
    }
    return std::string();
}

// ---- before closing --------------------------------------------------------------------------------------------------------------------------------------------------------

std::string busyQuestion(Busy b, Window target) {
    if (!b.any()) { return std::string(); }
    if (target == Window::Legacy) {
        if (b.recording && b.patch) { return tr("A recording and a patch are running. Switching to the Legacy window finishes the recording and stops the patch first."); }
        if (b.recording) { return tr("A recording is running. Switching to the Legacy window finishes and saves it first."); }
        return tr("A patch is running. Switching to the Legacy window stops it first.");
    }
    if (b.recording && b.patch) { return tr("A recording and a patch are running. Switching to the new window finishes the recording and stops the patch first."); }
    if (b.recording) { return tr("A recording is running. Switching to the new window finishes and saves it first."); }
    return tr("A patch is running. Switching to the new window stops it first.");
}

std::string preflight(Window target) {
    std::error_code ec;
    const fs::path exe = targetExe(target);
    if (exe.empty() || !fs::is_regular_file(exe, ec)) { return missingSentence(target); }
    return std::string();
}

// ---- after closing -----------------------------------------------------------------------------------------------------------------------------------------------------------

Lock probeLock(const std::string& configPath) {
    if (configPath.empty()) { return Lock::Free; }
    ConfigLock probe;
    const ConfigLock::State st = probe.acquire(configPath, "handover");
    if (st == ConfigLock::State::Held) {
        probe.release();
        return Lock::Free;
    }
    if (st == ConfigLock::State::Contended) { return probe.holder().pid == currentPid() ? Lock::HeldBySelf : Lock::HeldByOther; }
    return Lock::Free;   // Unavailable: a folder that takes no lock file (the window carried on without one, as it always has)
}

namespace {

bool defaultStarter(const std::string& exe, const std::string& args, const std::string& marker, unsigned long& error) {
    // THE TEST SEAMS STOP HERE: what a drive script set for THIS process (choose the other window at tick n, the extra arguments) is not for the program it starts - inherited, it would
    // choose the other window again at its own tick n and the two would hand over to each other for ever. The arguments were read into `args` before this call.
    const std::string childLog = envOr(kChildLogEnv);
    setEnvVar(kAtEnv, "");
    setEnvVar(kChildArgsEnv, "");
    setEnvVar(kChildLogEnv, "");
    setEnvVar(kRecordEnv, "");
    setEnvVar(kFromEnv, marker);
    const bool ok = startProgram(fs::path(exe), args, childLog, error);
    setEnvVar(kFromEnv, "");   // this process is going: the variable is for the child alone
    return ok;
}
Starter g_starter = &defaultStarter;

std::string childArgs(const Request& r) {
    std::string a;
    if (r.safePlugins) { a += "--safe-plugins"; }
    const std::string extra = envOr(kChildArgsEnv);
    if (!extra.empty()) { a += (a.empty() ? "" : " ") + extra; }
    return a;
}

// Starts `w`'s program marked as coming from `from`, with `note`. True when started.
bool startMarked(Window w, Window from, const std::string& note, const Request& r, unsigned long& error) {
    const fs::path exe = targetExe(w);
    std::error_code ec;
    if (exe.empty() || !fs::is_regular_file(exe, ec)) {
        error = 2;   // ERROR_FILE_NOT_FOUND
        return false;
    }
    const std::string marker = std::to_string(currentPid()) + ":" + windowWord(from) + ":" + note;
    return g_starter(exe.string(), childArgs(r), marker, error);
}

void appendRecord(const std::string& path, const std::string& line) {
    std::ofstream f(path, std::ios::binary | std::ios::app);
    if (f) { f << line << "\n"; }
}

}  // namespace

void setStarterForTest(Starter s) { g_starter = s != nullptr ? s : &defaultStarter; }

Outcome launch(const Request& r) {
    Outcome out;
    try {
        // 1. THE LOCK, FIRST: the other program is started only when this one no longer holds the settings file.
        const Lock lock = probeLock(r.configPath);
        const std::string recordPath = envOr(kRecordEnv);
        out.line = std::string("launch target=") + windowWord(r.target) + " program=" + programName(r.target) + " from=" + windowWord(r.from) + " lock=" + lockWord(lock) +
                   " parent=" + std::to_string(currentPid()) + " settings=" + (r.configPath.empty() ? "none" : r.configPath) + " args=" + childArgs(r);
        unsigned long error = 0;
        std::string note;
        if (lock == Lock::HeldBySelf) {
            // The order was broken: this process still holds the file. Nothing is started; the person gets this window back with the sentence.
            out.sentence = lockedSentence(r.target);
            note = "locked";
            cascade::core::diagLogf("window: the other window was not started: the settings lock was still held by this process");
            if (!recordPath.empty()) {
                appendRecord(recordPath, "refused target=" + std::string(windowWord(r.target)) + " lock=" + lockWord(lock));
                out.recorded = true;
                return out;
            }
        } else if (!recordPath.empty()) {
            const std::string miss = preflight(r.target);
            if (!miss.empty()) {
                appendRecord(recordPath, "refused target=" + std::string(windowWord(r.target)) + " reason=missing");
                out.recorded = true;
                out.sentence = miss;
                return out;
            }
            appendRecord(recordPath, out.line);
            out.started = true;
            out.recorded = true;
            if (!r.configPath.empty()) { (void)writeChoice(r.configPath, r.target); }
            return out;
        } else if (startMarked(r.target, r.from, "", r, error)) {
            out.started = true;
            if (!r.configPath.empty() && !writeChoice(r.configPath, r.target)) { cascade::core::diagLogf("window: the choice of window could not be remembered"); }
            cascade::core::diagLogf("window: handed over to the %s window", windowWord(r.target));
            return out;
        } else {
            const bool missing = !fs::is_regular_file(targetExe(r.target));
            out.sentence = missing ? missingSentence(r.target) : failedSentence(r.target, error);
            note = missing ? std::string("missing") : "failed:" + std::to_string(error);
            cascade::core::diagLogf("window: the %s window could not be started (error %lu)", windowWord(r.target), error);
        }
        // THE OTHER WINDOW WAS NOT STARTED: this window's own program starts again, marked, and says the sentence. The remembered choice stays what it was.
        unsigned long again = 0;
        out.returned = startMarked(r.from, r.target, note, r, again);
        if (!out.returned) { cascade::core::diagLogf("window: this window's own program could not be started again (error %lu)", again); }
    } catch (...) {
        out.sentence = "the hand-over failed";
    }
    return out;
}

}  // namespace cascade::core::handover
