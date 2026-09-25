// test_stop_ends_recordings.cpp - a recording belongs to one unbroken run of
// one source: every way the receiver stops, faults or changes source ends the
// takes it should, through the real application.
//
// THE BUGS (0.99.35, found by review of app_window.cpp).
//   * The desktop STOP dome and the Start/Stop key ended any IQ or audio take
//     before stopping the pipeline, but applyControlRequest with
//     running=false (the stop used by the WEB REMOTE and by PLUGINS through
//     the host API; CAT has no command that sets the run state) and the radar
//     scope's POWER button only called pipeline_.stop(). The take was left
//     open: REC still on, the WAV header still declaring zero samples on
//     disk, and the next start appended to the same file across the gap.
//   * A DRIVER FAULT drops the run flag, so nothing ended the takes at all:
//     the REC card kept its clock running over files nothing was reaching,
//     and a START (or the automatic reopen of a SoapySDR radio) restarted the
//     receiver with the recorders still hooked in, splicing the same take
//     across the fault.
//   * A SOURCE SWITCH AT THE SAME SAMPLE RATE (to the generator, or the patch
//     page borrowing the receiver's radio) kept an I/Q take open and filled
//     it with the NEW source's samples. Only a rate change ended it.
//
// TWO HALVES, because the paths are not equally reachable from a test:
//
// 1. END TO END. The application is started as a child with the web server
//    on a loopback port and driven over HTTP; what it reports AND the files
//    on disk are checked. Three sessions:
//      web stop   - on the generator: record, stop, restart; both takes must
//                   end at the stop with honest headers and stay ended.
//      fault      - on an I/Q file, which is the fault seam: the file is
//                   overwritten with a stub mid-take, the source's read fails
//                   and the pipeline latches the fault. Both takes must end
//                   on that alone, with the reason reported, and a START
//                   afterwards must not reopen or grow them.
//      switch     - on an I/Q file at the generator's own 2 MS/s, switched
//                   to the generator from the web remote: the I/Q take must
//                   end at the switch, the audio take carries on.
//    Every session runs with every profile directory, the recording
//    directory and every network endpoint in scratch or at a closed port,
//    asserts its source before recording anything (never a radio), and
//    removes its scratch tree on every path out.
//
// 2. EVERY STOP AND EVERY SOURCE SWAP GOES THROUGH ONE ROUTINE. POWER, the
//    key and the patch page cannot be pressed from here without pixel
//    coordinates, so the source of src/gui is read: pipeline_.stop() may
//    appear only in stopReceiver() and run()'s teardown, pipeline_.setSource
//    only in installSource(); stopReceiver() must end both takes before it
//    stops the pipeline; and each of the four user stop paths must call it.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "test_check.hpp"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace fs = std::filesystem;

namespace {

// --- Environment for the child ------------------------------------------------

// Set in BOTH the CRT and the OS copy on Windows: CreateProcess hands the
// child the OS block, and a later getenv in this process reads the CRT's.
void setEnv(const char* name, const std::string& value) {
#if defined(_WIN32)
    ::_putenv_s(name, value.c_str());
    ::SetEnvironmentVariableA(name, value.c_str());
#else
    ::setenv(name, value.c_str(), 1);
#endif
}

fs::path scratchDir() {
#if defined(_WIN32)
    const char* tmp = std::getenv("TEMP");
    const fs::path base = (tmp != nullptr && *tmp != '\0') ? fs::path(tmp) : fs::path(".");
    return base / (std::string("cascade-stoprec-") + std::to_string(::GetCurrentProcessId()));
#else
    return fs::path("/tmp") / (std::string("cascade-stoprec-") + std::to_string(::getpid()));
#endif
}

// A port nobody is listening on right now: bind one, read it back, CLOSE it.
// Raw sockets on purpose - an httplib::Server that was bound but never
// listened keeps its socket open, and Windows lets the child bind the same
// port beside it, so connections went to a listener nobody accepted on.
int freeLoopbackPort() {
#if defined(_WIN32)
    WSADATA wsa;
    if (::WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { return 0; }
    const SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) { return 0; }
#else
    const int s = ::socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) { return 0; }
#endif
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    int port = 0;
    if (::bind(s, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0) {
        socklen_t len = sizeof a;
        if (::getsockname(s, reinterpret_cast<sockaddr*>(&a), &len) == 0) {
            port = ntohs(a.sin_port);
        }
    }
#if defined(_WIN32)
    ::closesocket(s);
#else
    ::close(s);
#endif
    return port;
}

// --- The child application ------------------------------------------------------

class Child {
public:
    bool start(const fs::path& logPath) {
#if defined(_WIN32)
        const std::string exe = std::string(CASCADE_APP_BINDIR) + "/cascade.exe";
        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof sa;
        sa.bInheritHandle = TRUE;
        log_ = ::CreateFileW(logPath.wstring().c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (log_ == INVALID_HANDLE_VALUE) { return false; }
        // The child inherits the log handle and NOTHING else: sockets are
        // inheritable by default on Windows, so a plain bInheritHandles=TRUE
        // would hand it every socket this process holds.
        SIZE_T attrBytes = 0;
        ::InitializeProcThreadAttributeList(nullptr, 1, 0, &attrBytes);
        std::vector<unsigned char> attrBuf(attrBytes);
        auto* attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrBuf.data());
        if (!::InitializeProcThreadAttributeList(attrs, 1, 0, &attrBytes)) { return false; }
        HANDLE inherit[1] = {log_};
        ::UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit,
                                    sizeof inherit, nullptr, nullptr);
        STARTUPINFOEXA si{};
        si.StartupInfo.cb = sizeof si;
        si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        si.StartupInfo.hStdInput = nullptr;
        si.StartupInfo.hStdOutput = log_;
        si.StartupInfo.hStdError = log_;
        si.lpAttributeList = attrs;
        // A frame count far beyond anything this test needs: the child is
        // ended by closing its window once the checks are done, so the run
        // never races its own frame budget.
        std::string cmd = "\"" + exe + "\" --frames 200000";
        std::vector<char> buf(cmd.begin(), cmd.end());
        buf.push_back('\0');
        const BOOL ok = ::CreateProcessA(nullptr, buf.data(), nullptr, nullptr, TRUE,
                                         EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr,
                                         &si.StartupInfo, &pi_);
        ::DeleteProcThreadAttributeList(attrs);
        return ok != FALSE;
#else
        const std::string exe = std::string(CASCADE_APP_BINDIR) + "/cascade";
        posix_spawn_file_actions_t fa;
        posix_spawn_file_actions_init(&fa);
        posix_spawn_file_actions_addopen(&fa, 1, logPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC,
                                         0644);
        posix_spawn_file_actions_adddup2(&fa, 1, 2);
        std::string a0 = exe, a1 = "--frames", a2 = "200000";
        char* argv[] = {a0.data(), a1.data(), a2.data(), nullptr};
        const int rc = posix_spawn(&pid_, exe.c_str(), &fa, nullptr, argv, environ);
        posix_spawn_file_actions_destroy(&fa);
        return rc == 0;
#endif
    }

    bool alive() {
#if defined(_WIN32)
        return pi_.hProcess != nullptr && ::WaitForSingleObject(pi_.hProcess, 0) == WAIT_TIMEOUT;
#else
        if (pid_ <= 0) { return false; }
        int st = 0;
        const pid_t r = ::waitpid(pid_, &st, WNOHANG);
        if (r == pid_) {
            pid_ = -1;
            return false;
        }
        return r == 0;
#endif
    }

    // Ends THIS child and nothing else: addressed by the process id it was
    // started with, never by name. Windows closes the main window, which
    // runs the application's own teardown; returns false if that did not
    // end it and it had to be terminated. POSIX has no close to send to an
    // Xvfb window from here, so the child is signalled.
    bool finish() {
#if defined(_WIN32)
        if (pi_.hProcess == nullptr) { return true; }
        bool clean = true;
        if (alive()) {
            struct Ctx {
                DWORD pid;
                int posted;
            } ctx{pi_.dwProcessId, 0};
            ::EnumWindows(
                [](HWND h, LPARAM lp) -> BOOL {
                    auto* c = reinterpret_cast<Ctx*>(lp);
                    DWORD pid = 0;
                    ::GetWindowThreadProcessId(h, &pid);
                    if (pid != c->pid) { return TRUE; }
                    char title[128] = {};
                    ::GetWindowTextA(h, title, sizeof title);
                    if (std::strncmp(title, "FoxSDR ", 7) == 0) {
                        ::PostMessageA(h, WM_CLOSE, 0, 0);
                        ++c->posted;
                    }
                    return TRUE;
                },
                reinterpret_cast<LPARAM>(&ctx));
            if (ctx.posted == 0 || ::WaitForSingleObject(pi_.hProcess, 30000) != WAIT_OBJECT_0) {
                clean = false;
                ::TerminateProcess(pi_.hProcess, 1);
                ::WaitForSingleObject(pi_.hProcess, 10000);
            }
        }
        ::CloseHandle(pi_.hThread);
        ::CloseHandle(pi_.hProcess);
        pi_ = PROCESS_INFORMATION{};
        if (log_ != INVALID_HANDLE_VALUE) {
            ::CloseHandle(log_);
            log_ = INVALID_HANDLE_VALUE;
        }
        return clean;
#else
        if (pid_ > 0) {
            ::kill(pid_, SIGTERM);
            int st = 0;
            ::waitpid(pid_, &st, 0);
            pid_ = -1;
        }
        return true;
#endif
    }

    ~Child() { finish(); }

private:
#if defined(_WIN32)
    PROCESS_INFORMATION pi_{};
    HANDLE log_ = INVALID_HANDLE_VALUE;
#else
    pid_t pid_ = -1;
#endif
};

// --- HTTP ---------------------------------------------------------------------

bool getStatus(httplib::Client& cli, nlohmann::json& out) {
    auto res = cli.Get("/api/status");
    if (!res || res->status != 200) { return false; }
    try {
        out = nlohmann::json::parse(res->body);
    } catch (const std::exception&) {
        return false;
    }
    return true;
}

bool control(httplib::Client& cli, const std::string& body) {
    auto res = cli.Post("/api/control", body, "application/json");
    return res && res->status == 202;
}

// Polls the status until `pred` holds or `ms` pass. The last status read is
// left in `s` either way, so a failed wait can still report what was seen.
bool waitStatus(httplib::Client& cli, nlohmann::json& s, int ms,
                const std::function<bool(const nlohmann::json&)>& pred) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
        nlohmann::json j;
        if (getStatus(cli, j)) {
            s = j;
            if (pred(j)) { return true; }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return false;
}

bool jb(const nlohmann::json& j, const char* k) {
    return j.contains(k) && j[k].is_boolean() && j[k].get<bool>();
}
std::uint64_t ju(const nlohmann::json& j, const char* k) {
    return (j.contains(k) && j[k].is_number()) ? j[k].get<std::uint64_t>() : 0u;
}

// --- The takes on disk ------------------------------------------------------------

struct Take {
    fs::path path;
    std::uintmax_t fileBytes = 0;
    std::uint32_t declaredData = 0;  // the data chunk size the header claims
    bool headerOk = false;
};

std::vector<Take> takes(const fs::path& dir, const char* prefix) {
    std::vector<Take> out;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        const std::string name = e.path().filename().string();
        if (name.rfind(prefix, 0) != 0 || e.path().extension() != ".wav") { continue; }
        Take t;
        t.path = e.path();
        t.fileBytes = fs::file_size(e.path(), ec);
        std::ifstream in(e.path(), std::ios::binary);
        unsigned char h[44] = {};
        if (in.read(reinterpret_cast<char*>(h), 44) && std::memcmp(h, "RIFF", 4) == 0 &&
            std::memcmp(h + 36, "data", 4) == 0) {
            t.headerOk = true;
            t.declaredData = static_cast<std::uint32_t>(h[40]) |
                             (static_cast<std::uint32_t>(h[41]) << 8) |
                             (static_cast<std::uint32_t>(h[42]) << 16) |
                             (static_cast<std::uint32_t>(h[43]) << 24);
        }
        out.push_back(t);
    }
    return out;
}

// A finished take: one file, whose header declares every byte after it and
// at least one of them.
void checkFinalised(const std::vector<Take>& t, const char* what) {
    CHECK(t.size() == 1);
    for (const Take& k : t) {
        std::printf("  %s: %s  file %llu bytes, header declares %u data bytes\n", what,
                    k.path.filename().string().c_str(),
                    static_cast<unsigned long long>(k.fileBytes), k.declaredData);
        CHECK(k.headerOk);
        CHECK(k.declaredData > 0);
        CHECK(static_cast<std::uintmax_t>(k.declaredData) + 44u == k.fileBytes);
    }
}

// --- An I/Q file for the file-source sessions ----------------------------------------

void putU16(std::vector<unsigned char>& v, std::uint16_t x) {
    v.push_back(static_cast<unsigned char>(x & 0xFFu));
    v.push_back(static_cast<unsigned char>((x >> 8) & 0xFFu));
}

void putU32(std::vector<unsigned char>& v, std::uint32_t x) {
    for (int s = 0; s < 32; s += 8) { v.push_back(static_cast<unsigned char>((x >> s) & 0xFFu)); }
}

// 16-bit PCM I/Q, the layout test_pipeline_file_fault uses.
std::vector<unsigned char> iqWav(std::uint32_t rateHz, std::uint32_t frames) {
    std::vector<unsigned char> v;
    const std::uint32_t dataBytes = frames * 4u;
    v.insert(v.end(), {'R', 'I', 'F', 'F'});
    putU32(v, 36u + dataBytes);
    v.insert(v.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
    putU32(v, 16u);
    putU16(v, 1u);           // PCM
    putU16(v, 2u);           // I and Q
    putU32(v, rateHz);
    putU32(v, rateHz * 4u);  // byte rate
    putU16(v, 4u);           // block align
    putU16(v, 16u);          // bits
    v.insert(v.end(), {'d', 'a', 't', 'a'});
    putU32(v, dataBytes);
    for (std::uint32_t i = 0; i < frames; ++i) {
        putU16(v, static_cast<std::uint16_t>(1000 + (i % 97)));
        putU16(v, static_cast<std::uint16_t>(2000 + (i % 89)));
    }
    return v;
}

bool writeBytes(const fs::path& path, const std::vector<unsigned char>& bytes) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(f);
}

// The generator's rate, so a file at this rate makes a switch to the
// generator a SAME-RATE switch - the case the input-rate follow never caught.
constexpr std::uint32_t kFileRateHz = 2000000u;

// --- One child application, from start to scratch removed --------------------------

class Session {
public:
    // `sourceCfg` is extra config JSON (with a trailing comma) naming the
    // source; `wantKind` is the source the status must report before anything
    // is recorded.
    bool open(const char* name, const std::string& sourceCfg, const char* wantKind) {
        failedAtStart_ = g_checksFailed;
        dir_ = scratchDir() / name;
        std::error_code ec;
        fs::remove_all(dir_, ec);
        for (const char* sub : {"appdata", "localappdata", "diag", "home", "xdg"}) {
            fs::create_directories(dir_ / sub, ec);
        }
        recDir_ = dir_ / "home" / "Documents" / "SDR-recordings";

        const int port = freeLoopbackPort();
        CHECK(port > 0);
        if (port <= 0) { return false; }

        const fs::path cfgPath = dir_ / "config.json";
        {
            std::ofstream cfg(cfgPath, std::ios::binary | std::ios::trunc);
            cfg << "{\n" << sourceCfg
                << "  \"webEnabled\": true,\n"
                   "  \"webBindAddress\": \"127.0.0.1\",\n"
                   "  \"webPort\": "
                << port
                << ",\n"
                   "  \"telemetryEnabled\": false,\n"
                   "  \"updateCheckEnabled\": false\n"
                   "}\n";
        }

        setEnv("CASCADE_CONFIG_TEST", cfgPath.string());
        setEnv("APPDATA", (dir_ / "appdata").string());
        setEnv("LOCALAPPDATA", (dir_ / "localappdata").string());
        setEnv("FOXSDR_DIAG_DIR", (dir_ / "diag").string());
        // The recording directory is <profile>/Documents/SDR-recordings, so
        // the profile goes into scratch too. Nothing here opens a radio, so
        // hiding vendor SDKs under the real profile costs nothing.
        setEnv("USERPROFILE", (dir_ / "home").string());
        setEnv("HOME", (dir_ / "home").string());
        setEnv("XDG_CONFIG_HOME", (dir_ / "xdg" / "config").string());
        setEnv("XDG_DATA_HOME", (dir_ / "xdg" / "data").string());
        setEnv("XDG_CACHE_HOME", (dir_ / "xdg" / "cache").string());
        setEnv("XDG_STATE_HOME", (dir_ / "xdg" / "state").string());
        for (const char* url : {"FOXSDR_TELEMETRY_URL", "FOXSDR_CRASH_URL", "FOXSDR_UPDATE_URL",
                                "FOXSDR_REPORTS_URL", "FOXSDR_FEATURE_URL",
                                "FOXSDR_PROBLEM_URL"}) {
            setEnv(url, "http://127.0.0.1:9/");
        }

        const bool started = child_.start(dir_ / "child.log");
        CHECK(started);
        if (!started) { return false; }

        cli_ = std::make_unique<httplib::Client>("127.0.0.1", port);
        cli_->set_connection_timeout(2, 0);
        cli_->set_read_timeout(5, 0);

        nlohmann::json s;
        bool up = false;
        const auto upBy = std::chrono::steady_clock::now() + std::chrono::seconds(60);
        while (std::chrono::steady_clock::now() < upBy && child_.alive()) {
            // The server answers from startup, before the first frame has
            // published a snapshot; an empty recordDir is that default.
            if (getStatus(*cli_, s) && !s.value("recordDir", std::string()).empty()) {
                up = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        CHECK(up);
        if (!up) { return false; }
        const std::string kind = s.value("sourceKind", std::string());
        const std::string shownDir = s.value("recordDir", std::string());
        std::printf("  [%s] source %s, recording into %s\n", name, kind.c_str(), shownDir.c_str());
        CHECK(kind == wantKind);
        const bool inScratch =
            fs::weakly_canonical(fs::path(shownDir), ec) == fs::weakly_canonical(recDir_, ec);
        CHECK(inScratch);
        // Never record anywhere but scratch, never from anything but the
        // source this session asked for.
        return kind == wantKind && inScratch;
    }

    // Receiver on, both takes running and growing.
    bool startTaping(nlohmann::json& s) {
        CHECK(control(*cli_, R"({"running":true})"));
        CHECK(waitStatus(*cli_, s, 20000, [](const nlohmann::json& j) { return jb(j, "running"); }));
        CHECK(control(*cli_, R"({"recordIq":true,"recordAudio":true})"));
        const bool taping = waitStatus(*cli_, s, 20000, [](const nlohmann::json& j) {
            return jb(j, "iqRecording") && jb(j, "audioRecording") && ju(j, "iqBytes") > 0 &&
                   ju(j, "audioBytes") > 0;
        });
        CHECK(taping);
        std::printf("  recording: iq %llu bytes, audio %llu bytes\n",
                    static_cast<unsigned long long>(ju(s, "iqBytes")),
                    static_cast<unsigned long long>(ju(s, "audioBytes")));
        return taping;
    }

    void print(const char* when, const nlohmann::json& s) const {
        std::printf("  %s: running=%d faulted=%d iqRecording=%d audioRecording=%d iq %llu audio "
                    "%llu\n",
                    when, jb(s, "running") ? 1 : 0, jb(s, "faulted") ? 1 : 0,
                    jb(s, "iqRecording") ? 1 : 0, jb(s, "audioRecording") ? 1 : 0,
                    static_cast<unsigned long long>(ju(s, "iqBytes")),
                    static_cast<unsigned long long>(ju(s, "audioBytes")));
        const std::string err = s.value("recordError", std::string());
        if (!err.empty()) { std::printf("  %s: recordError \"%s\"\n", when, err.c_str()); }
    }

    // A few frames more, so a take that is going to end has had every chance.
    nlohmann::json settle(int ms = 500) {
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
        nlohmann::json s;
        CHECK(getStatus(*cli_, s));
        return s;
    }

    // Closes the child through its own window, then removes the scratch tree.
    // Runs on EVERY path out: the destructor calls it too, so an early return
    // cannot leave a child running or a directory behind.
    void close() {
        if (closed_) { return; }
        closed_ = true;
        cli_.reset();
        const bool clean = child_.finish();
        CHECK(clean);
        if (g_checksFailed > failedAtStart_) {
            std::ifstream in(dir_ / "child.log", std::ios::binary);
            std::stringstream ss;
            ss << in.rdbuf();
            std::printf("--- child output ---\n%s\n--------------------\n", ss.str().c_str());
        }
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }

    ~Session() { close(); }

    httplib::Client& cli() { return *cli_; }
    const fs::path& dir() const { return dir_; }
    const fs::path& recDir() const { return recDir_; }

private:
    fs::path dir_;
    fs::path recDir_;
    Child child_;
    std::unique_ptr<httplib::Client> cli_;
    int failedAtStart_ = 0;
    bool closed_ = false;
};

bool sameSize(const std::vector<Take>& a, const std::vector<Take>& b) {
    return a.size() == 1 && b.size() == 1 && a[0].fileBytes == b[0].fileBytes;
}

// --- Session 1: a stop from the web remote ----------------------------------------------

void webStopEndsTakes() {
    Session ss;
    if (!ss.open("webstop", "", "siggen")) { return; }
    httplib::Client& cli = ss.cli();
    nlohmann::json s;
    if (!ss.startTaping(s)) { return; }

    // STOP, from the web remote: applyControlRequest with running=false.
    CHECK(control(cli, R"({"running":false})"));
    CHECK(waitStatus(cli, s, 20000, [](const nlohmann::json& j) { return !jb(j, "running"); }));
    s = ss.settle();
    ss.print("after web stop", s);
    CHECK(!jb(s, "iqRecording"));
    CHECK(!jb(s, "audioRecording"));
    // A user's own stop is not news: no reason line for it.
    CHECK(s.value("recordError", std::string()).empty());
    // ...and on disk: both files closed with honest headers.
    const std::vector<Take> iq1 = takes(ss.recDir(), "iq_");
    const std::vector<Take> au1 = takes(ss.recDir(), "audio_");
    checkFinalised(iq1, "iq take after stop");
    checkFinalised(au1, "audio take after stop");

    // START again. No take may reopen, and no file may grow: the old bug
    // appended the second run to the first take across the gap.
    CHECK(control(cli, R"({"running":true})"));
    CHECK(waitStatus(cli, s, 20000, [](const nlohmann::json& j) { return jb(j, "running"); }));
    s = ss.settle(700);
    ss.print("after restart", s);
    CHECK(!jb(s, "iqRecording"));
    CHECK(!jb(s, "audioRecording"));
    CHECK(sameSize(takes(ss.recDir(), "iq_"), iq1));
    CHECK(sameSize(takes(ss.recDir(), "audio_"), au1));

    // A stop sent to a receiver that is ALREADY stopped must not end a take
    // armed while stopped ("press Play to feed the recorders") - the dome
    // cannot do that either, since it reads START on a stopped receiver.
    CHECK(control(cli, R"({"running":false})"));
    CHECK(waitStatus(cli, s, 20000, [](const nlohmann::json& j) { return !jb(j, "running"); }));
    CHECK(control(cli, R"({"recordAudio":true})"));
    CHECK(waitStatus(cli, s, 20000,
                     [](const nlohmann::json& j) { return jb(j, "audioRecording"); }));
    CHECK(control(cli, R"({"running":false})"));
    s = ss.settle();
    std::printf("  armed while stopped, then stop again: audioRecording=%d\n",
                jb(s, "audioRecording") ? 1 : 0);
    CHECK(jb(s, "audioRecording"));
}

// --- Session 2: a fault mid-take ---------------------------------------------------------

void faultEndsTakes() {
    // The I/Q file lives BESIDE the session tree, which open() recreates, and
    // exists before open() because the config restore opens it at startup.
    // Declared before the session so it is removed AFTER the child has gone.
    const fs::path src = scratchDir() / "fault-src";
    struct RemoveSrc {
        fs::path p;
        ~RemoveSrc() {
            std::error_code e;
            fs::remove_all(p, e);
        }
    } removeSrc{src};
    std::error_code ec;
    fs::create_directories(src, ec);
    const fs::path in = src / "in.wav";
    CHECK(writeBytes(in, iqWav(kFileRateHz, 1000000u)));
    const std::string cfg = "  \"sourceKind\": \"file\",\n  \"iqFilePath\": \"" +
                            in.generic_string() + "\",\n";
    Session ss;
    if (!ss.open("fault", cfg, "file")) { return; }
    httplib::Client& cli = ss.cli();
    nlohmann::json s;
    if (!ss.startTaping(s)) { return; }

    // THE FAULT: the file shrinks under the source's read position, the read
    // fails, the source latches it and the pipeline goes down with it. Nobody
    // presses anything.
    CHECK(writeBytes(in, iqWav(kFileRateHz, 8u)));
    const bool faulted =
        waitStatus(cli, s, 20000, [](const nlohmann::json& j) { return jb(j, "faulted"); });
    CHECK(faulted);
    s = ss.settle();
    ss.print("after the fault", s);
    CHECK(jb(s, "faulted"));
    CHECK(!jb(s, "running"));
    CHECK(!jb(s, "iqRecording"));
    CHECK(!jb(s, "audioRecording"));
    // Said, not left to be noticed.
    CHECK(s.value("recordError", std::string()).find("fault") != std::string::npos);
    const std::vector<Take> iq1 = takes(ss.recDir(), "iq_");
    const std::vector<Take> au1 = takes(ss.recDir(), "audio_");
    checkFinalised(iq1, "iq take after the fault");
    checkFinalised(au1, "audio take after the fault");

    // A take armed AFTER the fault is the user's choice and survives the
    // frames that follow (the fault is acted on at its edge, not every frame
    // it stays latched)... Armed a second later so its file cannot take the
    // first one's name (names are to the second, and a same-second name
    // truncates).
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    CHECK(control(cli, R"({"recordAudio":true})"));
    CHECK(waitStatus(cli, s, 20000,
                     [](const nlohmann::json& j) { return jb(j, "audioRecording"); }));
    s = ss.settle();
    std::printf("  armed after the fault: audioRecording=%d\n", jb(s, "audioRecording") ? 1 : 0);
    CHECK(jb(s, "audioRecording"));
    // ...and a STOP sent to the faulted receiver ends it: a latched fault is
    // proof the receiver was running, so this stop stops something.
    CHECK(control(cli, R"({"running":false})"));
    s = ss.settle();
    std::printf("  stop on the faulted receiver: audioRecording=%d\n",
                jb(s, "audioRecording") ? 1 : 0);
    CHECK(!jb(s, "audioRecording"));

    // START after the fault. A faulted I/Q file stays faulted until it is
    // opened again (its latch clears only in open(), which a browser cannot
    // reach), so the user does what the FAIL lamp invites: picks the
    // generator and presses START. The pipeline's fault latch is still up
    // when START arrives - a source swap never clears it - so this is the
    // START-after-a-fault path, and the receiver really runs again. No take
    // may come back with it; on 0.99.35 the same take resumed here, the
    // file's samples before the fault spliced to the generator's after it.
    CHECK(control(cli, R"({"sourceKind":"siggen"})"));
    CHECK(waitStatus(cli, s, 20000, [](const nlohmann::json& j) {
        return j.value("sourceKind", std::string()) == "siggen";
    }));
    CHECK(control(cli, R"({"running":true})"));
    CHECK(waitStatus(cli, s, 20000, [](const nlohmann::json& j) { return jb(j, "running"); }));
    s = ss.settle(700);
    ss.print("after START", s);
    CHECK(!jb(s, "iqRecording"));
    CHECK(!jb(s, "audioRecording"));
    CHECK(sameSize(takes(ss.recDir(), "iq_"), iq1));
    // The first audio take by its own path: the armed one above is a second
    // file beside it.
    for (const Take& t : au1) {
        std::error_code e;
        CHECK(fs::file_size(t.path, e) == t.fileBytes);
    }
    ss.close();
}

// --- Session 3: a source switch at the same rate -------------------------------------------

void sameRateSwitchEndsIqTake() {
    // The I/Q file lives BESIDE the session tree, which open() recreates, and
    // exists before open() because the config restore opens it at startup.
    // Declared before the session so it is removed AFTER the child has gone.
    const fs::path src = scratchDir() / "switch-src";
    struct RemoveSrc {
        fs::path p;
        ~RemoveSrc() {
            std::error_code e;
            fs::remove_all(p, e);
        }
    } removeSrc{src};
    std::error_code ec;
    fs::create_directories(src, ec);
    const fs::path in = src / "in.wav";
    CHECK(writeBytes(in, iqWav(kFileRateHz, 1000000u)));
    const std::string cfg = "  \"sourceKind\": \"file\",\n  \"iqFilePath\": \"" +
                            in.generic_string() + "\",\n";
    Session ss;
    if (!ss.open("switch", cfg, "file")) { return; }
    httplib::Client& cli = ss.cli();
    nlohmann::json s;
    if (!ss.startTaping(s)) { return; }
    const double rateBefore = s.value("sampleRateHz", 0.0);

    // To the generator, from the web remote: the same selectSource(0) the
    // patch page uses when it borrows the receiver's radio.
    CHECK(control(cli, R"({"sourceKind":"siggen"})"));
    CHECK(waitStatus(cli, s, 20000, [](const nlohmann::json& j) {
        return j.value("sourceKind", std::string()) == "siggen";
    }));
    s = ss.settle();
    ss.print("after the switch", s);
    const double rateAfter = s.value("sampleRateHz", 0.0);
    std::printf("  rate %.0f -> %.0f S/s\n", rateBefore, rateAfter);
    // The case under test: nothing about the RATE changed.
    CHECK(rateBefore == static_cast<double>(kFileRateHz));
    CHECK(rateAfter == rateBefore);
    CHECK(jb(s, "running"));
    CHECK(!jb(s, "iqRecording"));
    CHECK(s.value("recordError", std::string()).find("source") != std::string::npos);
    const std::vector<Take> iq1 = takes(ss.recDir(), "iq_");
    checkFinalised(iq1, "iq take after the switch");
    // The audio take carries on: it is what the speaker plays, and a source
    // change does not change its format any more than a retune does.
    CHECK(jb(s, "audioRecording"));
    const std::uint64_t audioAt = ju(s, "audioBytes");
    s = ss.settle(700);
    CHECK(jb(s, "audioRecording"));
    CHECK(ju(s, "audioBytes") > audioAt);
    CHECK(sameSize(takes(ss.recDir(), "iq_"), iq1));
    ss.close();
}

// --- Half 2: the source ----------------------------------------------------------

std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// The lines of `text` with // and /* */ comments blanked (string literals are
// not parsed; none of the patterns looked for appears in one).
std::vector<std::string> codeLines(const std::string& text) {
    std::vector<std::string> out;
    std::string line;
    bool inBlock = false;
    std::istringstream is(text);
    while (std::getline(is, line)) {
        if (!line.empty() && line.back() == '\r') { line.pop_back(); }
        std::string code;
        for (std::size_t i = 0; i < line.size(); ++i) {
            if (inBlock) {
                if (line.compare(i, 2, "*/") == 0) {
                    inBlock = false;
                    ++i;
                }
                continue;
            }
            if (line.compare(i, 2, "//") == 0) { break; }
            if (line.compare(i, 2, "/*") == 0) {
                inBlock = true;
                ++i;
                continue;
            }
            code += line[i];
        }
        out.push_back(code);
    }
    return out;
}

// The AppWindow member a line belongs to: the nearest preceding definition
// that starts in column 0 and names AppWindow::<fn>(.
std::string enclosingMember(const std::vector<std::string>& lines, std::size_t at) {
    for (std::size_t i = at + 1; i-- > 0;) {
        const std::string& l = lines[i];
        if (l.empty() || l[0] == ' ' || l[0] == '\t' || l[0] == '}' || l[0] == '#') { continue; }
        const std::size_t p = l.find("AppWindow::");
        if (p == std::string::npos) { continue; }
        const std::size_t b = p + std::strlen("AppWindow::");
        const std::size_t e = l.find('(', b);
        if (e == std::string::npos) { continue; }
        return l.substr(b, e - b);
    }
    return "?";
}

void everyStopUsesTheRoutine() {
    const fs::path gui = fs::path(CASCADE_SOURCE_DIR) / "src" / "gui";
    std::error_code ec;
    CHECK(fs::is_directory(gui, ec));

    int inRoutine = 0;
    int inTeardown = 0;
    int elsewhere = 0;
    int swapsInInstall = 0;
    int swapsElsewhere = 0;
    std::vector<std::string> callers;  // members that call stopReceiver()
    std::string routineBody;
    for (const auto& e : fs::directory_iterator(gui, ec)) {
        const std::string ext = e.path().extension().string();
        if (ext != ".cpp" && ext != ".hpp") { continue; }
        const std::vector<std::string> lines = codeLines(readFile(e.path()));
        for (std::size_t i = 0; i < lines.size(); ++i) {
            const std::string& l = lines[i];
            if (l.find("pipeline_.stop()") != std::string::npos) {
                const std::string m = enclosingMember(lines, i);
                if (m == "stopReceiver") {
                    ++inRoutine;
                } else if (m == "run") {
                    ++inTeardown;
                } else {
                    ++elsewhere;
                    std::printf("FAIL: pipeline_.stop() outside stopReceiver(), in "
                                "AppWindow::%s at %s:%zu\n",
                                m.c_str(), e.path().filename().string().c_str(), i + 1);
                }
            }
            if (l.find("pipeline_.setSource(") != std::string::npos) {
                const std::string m = enclosingMember(lines, i);
                if (m == "installSource") {
                    ++swapsInInstall;
                } else {
                    ++swapsElsewhere;
                    std::printf("FAIL: pipeline_.setSource() outside installSource(), in "
                                "AppWindow::%s at %s:%zu\n",
                                m.c_str(), e.path().filename().string().c_str(), i + 1);
                }
            }
            if (l.find("stopReceiver()") != std::string::npos &&
                l.find("AppWindow::stopReceiver()") == std::string::npos &&
                l.find("void stopReceiver()") == std::string::npos) {
                callers.push_back(enclosingMember(lines, i));
            }
            if (l.rfind("void AppWindow::stopReceiver()", 0) == 0) {
                for (std::size_t k = i; k < lines.size(); ++k) {
                    routineBody += lines[k] + "\n";
                    if (k > i && lines[k].rfind("}", 0) == 0) { break; }
                }
            }
        }
    }
    std::printf("  pipeline_.stop(): %d in stopReceiver, %d in run() teardown, %d elsewhere\n",
                inRoutine, inTeardown, elsewhere);
    CHECK(inRoutine == 1);
    CHECK(inTeardown == 1);
    CHECK(elsewhere == 0);
    std::printf("  pipeline_.setSource(): %d in installSource, %d elsewhere\n", swapsInInstall,
                swapsElsewhere);
    CHECK(swapsInInstall == 1);
    CHECK(swapsElsewhere == 0);

    // The routine ends both takes, and does it BEFORE the pipeline stops.
    const std::size_t iq = routineBody.find("stopIqRecording()");
    const std::size_t au = routineBody.find("stopAudioRecording()");
    const std::size_t st = routineBody.find("pipeline_.stop()");
    CHECK(!routineBody.empty());
    CHECK(iq != std::string::npos);
    CHECK(au != std::string::npos);
    CHECK(st != std::string::npos);
    CHECK(iq < st);
    CHECK(au < st);

    // The four user stop paths: the dome, the key, the web remote/plugin
    // request, and the radar scope's POWER button.
    for (const char* m : {"drawToolbar", "applyKeyAction", "applyControlRequest", "drawScopeMode"}) {
        bool found = false;
        for (const std::string& c : callers) { found = found || c == m; }
        if (!found) { std::printf("FAIL: AppWindow::%s does not call stopReceiver()\n", m); }
        CHECK(found);
    }
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);  // a hang still shows how far it got
    everyStopUsesTheRoutine();
    webStopEndsTakes();
    faultEndsTakes();
    sameRateSwitchEndsIqTake();
    std::error_code ec;
    fs::remove_all(scratchDir(), ec);
    return testSummary("test_stop_ends_recordings");
}
