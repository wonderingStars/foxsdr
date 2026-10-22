// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
//
// THE SPAWN HELPER OF THE foxsdr-engine TESTS (engine extraction stage 7b):
// start one of the two real executables beside the tests (CASCADE_APP_BINDIR),
// with its standard output and error going to files, in a working directory the
// test chose, and wait for it, time it, end it or look at what it loaded.
//
// WHY NOT popen, which the older launcher tests use. These tests need three
// things popen cannot give: stdout and stderr SEPARATELY (the engine's stdout
// is only what a mode prints; its stderr is the service's journal), a working
// directory that is not the test's, and a process the test can END on purpose
// (a no-flag engine runs until it is ended from outside, and the test must not
// leave one behind when it does not exit). Standard input is NUL, so a child
// that read it would see the end at once.
//
// The environment is the test's own, inherited: tests/test_engine_main_*.cpp
// point every per-user folder at a scratch directory first (isolate()), which is
// the whole of their hermeticity - nothing here names the real profile.
//
// Windows: CreateProcess, CREATE_NO_WINDOW (a console program started by a test
// must not put a console window on the desktop). POSIX: fork/execv. The POSIX
// half was written on a machine that cannot run it and is UNVERIFIED until the
// Linux jobs run it.
#pragma once

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#include <tlhelp32.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace engine_main_test {

namespace fs = std::filesystem;

inline std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// A child's captured stream as text: the Windows console runtime writes a line end as CR LF, and what the tests compare
// is the text, so the CRs are taken out (a stream that is not text - an image - is read with readFile).
inline std::string readText(const fs::path& p) {
    std::string s = readFile(p);
    s.erase(std::remove(s.begin(), s.end(), '\r'), s.end());
    return s;
}

inline bool contains(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

// The bytes lower-cased (ASCII), for a case-insensitive search of an image.
inline std::string lowered(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') { c = static_cast<char>(c - 'A' + 'a'); }
    }
    return s;
}

inline void setEnv(const char* name, const std::string& value) {
#if defined(_WIN32)
    _putenv_s(name, value.c_str());
    SetEnvironmentVariableA(name, value.c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}

inline void unsetEnv(const char* name) {
#if defined(_WIN32)
    _putenv_s(name, "");
    SetEnvironmentVariableA(name, nullptr);
#else
    unsetenv(name);
#endif
}

// Every per-user folder the programs consult, pointed at `home`, and the
// variables that would redirect their diagnostics or config cleared: afterwards
// anything the child writes outside `home` is a path it chose from somewhere
// other than the environment, which the tests also look for.
inline void isolate(const fs::path& home) {
    const std::string h = home.string();
    for (const char* v : {"LOCALAPPDATA", "APPDATA", "USERPROFILE", "HOME", "XDG_CONFIG_HOME", "XDG_STATE_HOME",
                          "XDG_DATA_HOME", "XDG_CACHE_HOME"}) {
        setEnv(v, h);
    }
    const std::string tmp = (home / "tmp").string();
    setEnv("TEMP", tmp);
    setEnv("TMP", tmp);
    setEnv("TMPDIR", tmp);
    for (const char* v : {"FOXSDR_DIAG_DIR", "CASCADE_CONFIG_TEST", "CASCADE_SENTINEL_TEST", "FOXSDR_INSTANCE_SCOPE"}) {
        unsetEnv(v);
    }
    // No vendor driver is loaded from the real machine by a child that lists them.
    setEnv("SOAPY_SDR_PLUGIN_PATH", (home / "no-soapy-modules").string());
    setEnv("FOXSDR_TELEMETRY_URL", "http://127.0.0.1:9");
}

// "cascade" or "foxsdr-engine", in the directory the application binary is in.
inline fs::path appExe(const char* name) {
#if defined(_WIN32)
    return fs::path(CASCADE_APP_BINDIR) / (std::string(name) + ".exe");
#else
    return fs::path(CASCADE_APP_BINDIR) / name;
#endif
}

// A started child.
class Child {
public:
    Child() = default;
    Child(const Child&) = delete;
    Child& operator=(const Child&) = delete;
    ~Child() { kill(); }

    // Start `exe` with `args`, in `cwd`, its stdout and stderr in `outFile` and `errFile`.
    bool start(const fs::path& exe, const std::vector<std::string>& args, const fs::path& cwd, const fs::path& outFile,
               const fs::path& errFile) {
        t0_ = std::chrono::steady_clock::now();
#if defined(_WIN32)
        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;
        const auto openOut = [&sa](const fs::path& p) {
            return ::CreateFileW(p.wstring().c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                                 CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        };
        HANDLE hOut = openOut(outFile);
        HANDLE hErr = openOut(errFile);
        HANDLE hIn = ::CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING,
                                   FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hOut == INVALID_HANDLE_VALUE || hErr == INVALID_HANDLE_VALUE || hIn == INVALID_HANDLE_VALUE) {
            closeAll(hOut, hErr, hIn);
            return false;
        }
        std::wstring cmd = L"\"" + exe.wstring() + L"\"";
        for (const std::string& a : args) {
            cmd += L" \"";
            cmd += std::wstring(a.begin(), a.end());  // the tests' arguments are ASCII
            cmd += L"\"";
        }
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = hIn;
        si.hStdOutput = hOut;
        si.hStdError = hErr;
        PROCESS_INFORMATION pi{};
        const BOOL ok = ::CreateProcessW(exe.wstring().c_str(), cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                                         nullptr, cwd.wstring().c_str(), &si, &pi);
        closeAll(hOut, hErr, hIn);
        if (!ok) { return false; }
        ::CloseHandle(pi.hThread);
        process_ = pi.hProcess;
        pid_ = pi.dwProcessId;
        return true;
#else
        std::vector<std::string> argvStore;
        argvStore.push_back(exe.string());
        for (const std::string& a : args) { argvStore.push_back(a); }
        std::vector<char*> argv;
        for (std::string& a : argvStore) { argv.push_back(a.data()); }
        argv.push_back(nullptr);
        const pid_t p = ::fork();
        if (p < 0) { return false; }
        if (p == 0) {
            if (::chdir(cwd.c_str()) != 0) { ::_exit(126); }
            const int o = ::open(outFile.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
            const int e = ::open(errFile.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
            const int i = ::open("/dev/null", O_RDONLY);
            if (o < 0 || e < 0 || i < 0) { ::_exit(126); }
            ::dup2(i, 0);
            ::dup2(o, 1);
            ::dup2(e, 2);
            ::execv(exe.c_str(), argv.data());
            ::_exit(127);
        }
        pid_ = p;
        return true;
#endif
    }

    unsigned long pid() const { return static_cast<unsigned long>(pid_); }

    // Wait up to `timeoutMs` for the child to end by itself. True when it has (its exit code is then in code()).
    bool waitExit(int timeoutMs) {
        if (done_) { return true; }
#if defined(_WIN32)
        if (process_ == nullptr) { return false; }
        if (::WaitForSingleObject(process_, static_cast<DWORD>(timeoutMs)) != WAIT_OBJECT_0) { return false; }
        DWORD c = 0;
        ::GetExitCodeProcess(process_, &c);
        code_ = static_cast<int>(c);
        ::CloseHandle(process_);
        process_ = nullptr;
        done_ = true;
        return true;
#else
        const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        for (;;) {
            int status = 0;
            const pid_t r = ::waitpid(pid_, &status, WNOHANG);
            if (r == pid_) {
                code_ = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
                done_ = true;
                return true;
            }
            if (r < 0) { return false; }
            if (std::chrono::steady_clock::now() >= end) { return false; }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
#endif
    }

    // End the child now if it is still running (the exit code is then not its own).
    void kill() {
        if (done_) { return; }
#if defined(_WIN32)
        if (process_ != nullptr) {
            ::TerminateProcess(process_, 1);
            ::WaitForSingleObject(process_, 10000);
            ::CloseHandle(process_);
            process_ = nullptr;
        }
#else
        if (pid_ > 0) {
            ::kill(pid_, SIGKILL);
            int status = 0;
            ::waitpid(pid_, &status, 0);
        }
#endif
        done_ = true;
        killed_ = true;
    }

    int code() const { return code_; }
    bool killed() const { return killed_; }
    double elapsedS() const {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0_).count();
    }

    // The modules (DLLs / shared objects) mapped into the running child, lower-cased file names, or empty when
    // they cannot be listed. Windows: a toolhelp snapshot. Linux: /proc/<pid>/maps.
    std::vector<std::string> modules() const {
        std::vector<std::string> out;
#if defined(_WIN32)
        HANDLE snap = ::CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, static_cast<DWORD>(pid_));
        if (snap == INVALID_HANDLE_VALUE) { return out; }
        MODULEENTRY32W me{};
        me.dwSize = sizeof(me);
        for (BOOL more = ::Module32FirstW(snap, &me); more; more = ::Module32NextW(snap, &me)) {
            std::wstring w(me.szModule);
            std::string n;
            for (const wchar_t c : w) { n.push_back(c < 128 ? static_cast<char>(c) : '?'); }
            out.push_back(lowered(n));
        }
        ::CloseHandle(snap);
#else
        std::ifstream maps("/proc/" + std::to_string(pid_) + "/maps");
        std::string line;
        while (std::getline(maps, line)) {
            const std::size_t slash = line.rfind('/');
            if (slash == std::string::npos) { continue; }
            out.push_back(lowered(line.substr(slash + 1)));
        }
#endif
        return out;
    }

private:
#if defined(_WIN32)
    static void closeAll(HANDLE a, HANDLE b, HANDLE c) {
        for (HANDLE h : {a, b, c}) {
            if (h != INVALID_HANDLE_VALUE && h != nullptr) { ::CloseHandle(h); }
        }
    }
    HANDLE process_ = nullptr;
    unsigned long pid_ = 0;
#else
    pid_t pid_ = -1;
#endif
    bool done_ = false;
    bool killed_ = false;
    int code_ = -1;
    std::chrono::steady_clock::time_point t0_ = std::chrono::steady_clock::now();
};

struct RunResult {
    bool started = false;
    bool timedOut = false;  // did not end by itself within the timeout and was ended
    int exitCode = -1;
    double seconds = 0.0;
    std::string out;
    std::string err;
};

// Run to completion (or `timeoutMs`), reading both streams back.
inline RunResult run(const fs::path& exe, const std::vector<std::string>& args, const fs::path& cwd,
                     const fs::path& outDir, const std::string& tag, int timeoutMs = 60000) {
    RunResult r;
    std::error_code ec;
    fs::create_directories(outDir, ec);
    const fs::path outFile = outDir / (tag + ".out");
    const fs::path errFile = outDir / (tag + ".err");
    Child c;
    r.started = c.start(exe, args, cwd, outFile, errFile);
    if (!r.started) { return r; }
    if (!c.waitExit(timeoutMs)) {
        r.timedOut = true;
        c.kill();
    }
    r.exitCode = c.code();
    r.seconds = c.elapsedS();
    r.out = readText(outFile);
    r.err = readText(errFile);
    return r;
}

// Every file and directory under `root`, as "relative/path size" lines, sorted: the whole of what a run left.
inline std::vector<std::string> tree(const fs::path& root) {
    std::vector<std::string> out;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
         !ec && it != end; it.increment(ec)) {
        std::error_code e2;
        const std::string rel = fs::relative(it->path(), root, e2).generic_string();
        const bool dir = it->is_directory(e2);
        const std::uintmax_t size = dir ? 0 : it->file_size(e2);
        out.push_back(rel + (dir ? "/" : (" " + std::to_string(size))));
    }
    std::sort(out.begin(), out.end());
    return out;
}

// A private scratch directory for one test.
inline fs::path scratch(const char* tag) {
    const fs::path p = fs::temp_directory_path() /
                       (std::string("foxsdr-") + tag + "-" + std::to_string(std::rand()) + "-" +
                        std::to_string(static_cast<unsigned long long>(
                            std::chrono::steady_clock::now().time_since_epoch().count())));
    std::error_code ec;
    fs::create_directories(p, ec);
    return p;
}

}  // namespace engine_main_test
