// test_stop_ends_recordings.cpp - every way of stopping the receiver ends
// the recordings it was taping, through the real application.
//
// THE BUG (0.99.35, found by review of app_window.cpp). The desktop STOP dome
// and the Start/Stop key both ended any IQ or audio take before stopping the
// pipeline - "a take can never outlive the sample flow it was taping" - but
// two other stop paths only called pipeline_.stop():
//   * applyControlRequest with running=false, which is the stop used by the
//     WEB REMOTE, by CAT and by PLUGINS through the host API;
//   * the radar scope's POWER button.
// A stop from any of those left the take open: the REC state stayed on, the
// WAV header still declared zero samples on disk, and the next start from
// anywhere appended to the same file across the gap.
//
// TWO HALVES, because the paths are not equally reachable from a test:
//
// 1. END TO END, through the web remote (the entry point a user touches for
//    applyControlRequest). The application is started as a child with the
//    web server on a loopback port; the test starts the receiver and both
//    recorders over HTTP, stops the receiver over HTTP, and then looks at
//    what the application says AND at the files on disk: both takes must be
//    over, and each WAV header must declare exactly the bytes the file
//    holds. It then starts the receiver again and requires that no take
//    reopened and no file grew. The child runs on the signal generator only
//    (asserted before anything is recorded), with every profile directory,
//    the recording directory and every network endpoint pointed into
//    scratch or at a closed port.
//
// 2. EVERY STOP GOES THROUGH ONE ROUTINE. The POWER button and the key cannot
//    be pressed from here without driving pixel coordinates, so the source
//    of src/gui is read instead: a call to pipeline_.stop() anywhere except
//    AppWindow::stopReceiver() (and run()'s teardown, which ends the takes
//    itself further up) fails with its file and line, stopReceiver() must
//    end both takes BEFORE it stops the pipeline, and each of the four user
//    stop paths must call it. A fifth stop path added later without the
//    routine fails here the day it is written.
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

void endToEndWebStop() {
    const fs::path dir = scratchDir();
    std::error_code ec;
    fs::remove_all(dir, ec);
    const fs::path home = dir / "home";
    for (const char* sub : {"appdata", "localappdata", "diag", "home", "xdg"}) {
        fs::create_directories(dir / sub, ec);
    }
    const fs::path recDir = home / "Documents" / "SDR-recordings";

    const int port = freeLoopbackPort();
    CHECK(port > 0);
    if (port <= 0) { return; }

    const fs::path cfgPath = dir / "config.json";
    {
        std::ofstream cfg(cfgPath, std::ios::binary | std::ios::trunc);
        cfg << "{\n"
               "  \"webEnabled\": true,\n"
               "  \"webBindAddress\": \"127.0.0.1\",\n"
               "  \"webPort\": "
            << port
            << ",\n"
               "  \"telemetryEnabled\": false,\n"
               "  \"updateCheckEnabled\": false\n"
               "}\n";
    }

    setEnv("CASCADE_CONFIG_TEST", cfgPath.string());
    setEnv("APPDATA", (dir / "appdata").string());
    setEnv("LOCALAPPDATA", (dir / "localappdata").string());
    setEnv("FOXSDR_DIAG_DIR", (dir / "diag").string());
    // The recording directory is <profile>/Documents/SDR-recordings, so the
    // profile goes into scratch too. Nothing here opens a radio, so hiding
    // vendor SDKs under the real profile costs nothing.
    setEnv("USERPROFILE", home.string());
    setEnv("HOME", home.string());
    setEnv("XDG_CONFIG_HOME", (dir / "xdg" / "config").string());
    setEnv("XDG_DATA_HOME", (dir / "xdg" / "data").string());
    setEnv("XDG_CACHE_HOME", (dir / "xdg" / "cache").string());
    setEnv("XDG_STATE_HOME", (dir / "xdg" / "state").string());
    setEnv("FOXSDR_TELEMETRY_URL", "http://127.0.0.1:9/");
    setEnv("FOXSDR_CRASH_URL", "http://127.0.0.1:9/");
    setEnv("FOXSDR_UPDATE_URL", "http://127.0.0.1:9/");
    setEnv("FOXSDR_REPORTS_URL", "http://127.0.0.1:9/");

    const fs::path logPath = dir / "child.log";
    Child child;
    const bool started = child.start(logPath);
    CHECK(started);
    if (!started) { return; }

    httplib::Client cli("127.0.0.1", port);
    cli.set_connection_timeout(2, 0);
    cli.set_read_timeout(5, 0);

    const auto dumpLog = [&]() {
        std::ifstream in(logPath, std::ios::binary);
        std::stringstream ss;
        ss << in.rdbuf();
        std::printf("--- child output ---\n%s\n--------------------\n", ss.str().c_str());
    };

    // 1. The server is up and the radio is the signal generator.
    nlohmann::json s;
    bool up = false;
    const auto upBy = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (std::chrono::steady_clock::now() < upBy && child.alive()) {
        // The server answers from startup, before the first frame has
        // published a snapshot; an empty recordDir is that default.
        if (getStatus(cli, s) && !s.value("recordDir", std::string()).empty()) {
            up = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    CHECK(up);
    if (!up) {
        dumpLog();
        child.finish();
        return;
    }
    const std::string kind = s.value("sourceKind", std::string());
    const std::string shownDir = s.value("recordDir", std::string());
    std::printf("  child: source %s, recording into %s\n", kind.c_str(), shownDir.c_str());
    CHECK(kind == "siggen");
    CHECK(fs::weakly_canonical(fs::path(shownDir), ec) == fs::weakly_canonical(recDir, ec));
    if (kind != "siggen" ||
        fs::weakly_canonical(fs::path(shownDir), ec) != fs::weakly_canonical(recDir, ec)) {
        child.finish();
        return;  // never record anywhere but scratch, never from a radio
    }

    // 2. Receiver on, both takes running and growing.
    CHECK(control(cli, R"({"running":true})"));
    CHECK(waitStatus(cli, s, 20000, [](const nlohmann::json& j) { return jb(j, "running"); }));
    CHECK(control(cli, R"({"recordIq":true,"recordAudio":true})"));
    const bool taping = waitStatus(cli, s, 20000, [](const nlohmann::json& j) {
        return jb(j, "iqRecording") && jb(j, "audioRecording") && ju(j, "iqBytes") > 0 &&
               ju(j, "audioBytes") > 0;
    });
    CHECK(taping);
    std::printf("  recording: iq %llu bytes, audio %llu bytes\n",
                static_cast<unsigned long long>(ju(s, "iqBytes")),
                static_cast<unsigned long long>(ju(s, "audioBytes")));

    // 3. STOP, from the web remote: applyControlRequest with running=false.
    CHECK(control(cli, R"({"running":false})"));
    const bool stopped =
        waitStatus(cli, s, 20000, [](const nlohmann::json& j) { return !jb(j, "running"); });
    CHECK(stopped);
    // A few frames more, so a take that is going to end has had every chance.
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    CHECK(getStatus(cli, s));
    const std::uint64_t iqAtStop = ju(s, "iqBytes");
    const std::uint64_t audioAtStop = ju(s, "audioBytes");
    std::printf("  after web stop: running=%d iqRecording=%d audioRecording=%d iq %llu audio %llu\n",
                jb(s, "running") ? 1 : 0, jb(s, "iqRecording") ? 1 : 0,
                jb(s, "audioRecording") ? 1 : 0, static_cast<unsigned long long>(iqAtStop),
                static_cast<unsigned long long>(audioAtStop));
    CHECK(!jb(s, "iqRecording"));
    CHECK(!jb(s, "audioRecording"));
    // ...and on disk: both files closed with honest headers.
    const std::vector<Take> iq1 = takes(recDir, "iq_");
    const std::vector<Take> au1 = takes(recDir, "audio_");
    checkFinalised(iq1, "iq take after stop");
    checkFinalised(au1, "audio take after stop");

    // 4. START again. No take may reopen, and no file may grow: the old bug
    //    appended the second run to the first take across the gap.
    CHECK(control(cli, R"({"running":true})"));
    CHECK(waitStatus(cli, s, 20000, [](const nlohmann::json& j) { return jb(j, "running"); }));
    std::this_thread::sleep_for(std::chrono::milliseconds(700));
    CHECK(getStatus(cli, s));
    std::printf("  after restart: running=%d iqRecording=%d audioRecording=%d iq %llu audio %llu\n",
                jb(s, "running") ? 1 : 0, jb(s, "iqRecording") ? 1 : 0,
                jb(s, "audioRecording") ? 1 : 0,
                static_cast<unsigned long long>(ju(s, "iqBytes")),
                static_cast<unsigned long long>(ju(s, "audioBytes")));
    CHECK(!jb(s, "iqRecording"));
    CHECK(!jb(s, "audioRecording"));
    const std::vector<Take> iq2 = takes(recDir, "iq_");
    const std::vector<Take> au2 = takes(recDir, "audio_");
    CHECK(iq2.size() == 1 && iq1.size() == 1 && iq2[0].fileBytes == iq1[0].fileBytes);
    CHECK(au2.size() == 1 && au1.size() == 1 && au2[0].fileBytes == au1[0].fileBytes);

    // 5. A stop sent to a receiver that is ALREADY stopped must not end a take
    //    armed while stopped ("press Play to feed the recorders") - the dome
    //    cannot do that either, since it reads START on a stopped receiver.
    CHECK(control(cli, R"({"running":false})"));
    CHECK(waitStatus(cli, s, 20000, [](const nlohmann::json& j) { return !jb(j, "running"); }));
    CHECK(control(cli, R"({"recordAudio":true})"));
    CHECK(waitStatus(cli, s, 20000,
                     [](const nlohmann::json& j) { return jb(j, "audioRecording"); }));
    CHECK(control(cli, R"({"running":false})"));
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    CHECK(getStatus(cli, s));
    std::printf("  armed while stopped, then stop again: audioRecording=%d\n",
                jb(s, "audioRecording") ? 1 : 0);
    CHECK(jb(s, "audioRecording"));

    const bool clean = child.finish();
    CHECK(clean);
    if (g_checksFailed > 0) { dumpLog(); }
    fs::remove_all(dir, ec);
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

    // The four user stop paths: the dome, the key, the remote/CAT/plugin
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
    endToEndWebStop();
    return testSummary("test_stop_ends_recordings");
}
