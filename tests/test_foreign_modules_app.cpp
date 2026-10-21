// The other software's DLLs, named by the REAL cascade.exe (0.99.69).
//
// tests/test_foreign_modules.cpp proves the classifier, the words and the loader
// notification in a test process. What only the real program can show is the WIRING:
// that AppWindow starts the watch once its window is up, that the line reaches the log
// FILE (the one the sentinel and a user read), and that the list reaches the two kinds of
// report a "died in present" report is - the context block an in-process writer renders
// (here: a freeze report, which shares its writer's context with a crash report), and the
// sentinel's report of a death nothing inside could report.
//
// Every run points FOXSDR_DIAG_DIR, CASCADE_CONFIG_TEST, APPDATA and LOCALAPPDATA at a
// scratch tree (the user's configuration, reports folder and plugin folder are never
// touched; loading a real plugin DLL would write its defaults into the real
// %LOCALAPPDATA%\foxsdr otherwise) and has no report endpoint, so nothing is uploaded.
// No radio hardware is opened: these are bounded `--frames` runs.
//
// WHAT THIS DESKTOP'S OWN cascade.exe REPORTED is printed, because the line is only
// worth anything if what it names is what is really in the process.
//
// Windows only: the module list and the notification are. Elsewhere it SKIPs by name.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "core/crash_upload.hpp"
#include "core/foreign_modules.hpp"
#include "test_check.hpp"

#if defined(_WIN32)
#include <windows.h>
#endif

namespace fs = std::filesystem;
using namespace cascade::core;

namespace {

std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

bool contains(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

std::size_t countOf(const std::string& hay, const std::string& needle) {
    std::size_t n = 0;
    for (std::size_t at = hay.find(needle); at != std::string::npos; at = hay.find(needle, at + 1)) { ++n; }
    return n;
}

std::vector<std::string> linesOf(const std::string& text) {
    std::vector<std::string> out;
    std::size_t pos = 0;
    while (pos < text.size()) {
        std::size_t eol = text.find('\n', pos);
        if (eol == std::string::npos) { eol = text.size(); }
        std::string l = text.substr(pos, eol - pos);
        if (!l.empty() && l.back() == '\r') { l.pop_back(); }
        out.push_back(l);
        pos = eol + 1;
    }
    return out;
}

// The one line of `text` that begins `key` (after a newline), without the newline.
std::string lineStarting(const std::string& text, const std::string& key) {
    const std::size_t at = text.find("\n" + key);
    if (at == std::string::npos) { return std::string(); }
    const std::size_t end = text.find('\n', at + 1);
    return text.substr(at + 1, end - at - 1);
}

std::set<std::string> namesIn(const std::string& fieldValue) {
    std::set<std::string> out;
    const std::string v = normaliseForeignField(fieldValue);
    std::string cur;
    for (std::size_t i = 0; i <= v.size(); ++i) {
        if (i == v.size() || v[i] == ',') {
            std::size_t a = 0;
            while (a < cur.size() && cur[a] == ' ') { ++a; }
            const std::string t = cur.substr(a);
            if (!t.empty() && t[0] != '+' && t != "(none)") { out.insert(t); }
            cur.clear();
        } else {
            cur.push_back(v[i]);
        }
    }
    return out;
}

#if defined(_WIN32)

fs::path scratch(const char* tag) {
    const char* tmp = std::getenv("TEMP");
    const fs::path base = (tmp != nullptr && *tmp != '\0') ? fs::path(tmp) : fs::path(".");
    const fs::path dir = base / (std::string("cascade-foreignapp-") + tag + "-" +
                                 std::to_string(::GetCurrentProcessId()));
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    return dir;
}

// The environment a run gets, set for the child and put back afterwards.
struct ScratchEnv {
    std::vector<std::pair<std::string, std::string>> saved;
    void set(const char* name, const std::string& value) {
        char buf[32768] = {};
        const DWORD n = ::GetEnvironmentVariableA(name, buf, sizeof(buf));
        saved.emplace_back(name, n > 0 && n < sizeof(buf) ? std::string(buf, n) : std::string("\x01unset"));
        ::SetEnvironmentVariableA(name, value.empty() ? nullptr : value.c_str());
    }
    ~ScratchEnv() {
        for (auto it = saved.rbegin(); it != saved.rend(); ++it) {
            ::SetEnvironmentVariableA(it->first.c_str(), it->second == "\x01unset" ? nullptr : it->second.c_str());
        }
    }
};

void prepare(ScratchEnv& env, const fs::path& dir, bool sentinel, const std::string& foreignDll = std::string()) {
    std::error_code ec;
    env.set("CASCADE_FOREIGN_MODULE_TEST", foreignDll);
    fs::create_directories(dir / "appdata", ec);
    fs::create_directories(dir / "localappdata", ec);
    env.set("FOXSDR_DIAG_DIR", dir.string());
    env.set("CASCADE_CONFIG_TEST", (dir / "config.json").string());
    env.set("CASCADE_SENTINEL_TEST", sentinel ? "1" : "");
    env.set("FOXSDR_CRASH_URL", "");
    env.set("APPDATA", (dir / "appdata").string());
    env.set("LOCALAPPDATA", (dir / "localappdata").string());
}

struct App {
    HANDLE process = nullptr;
    DWORD pid = 0;
    bool started = false;
};

App startApp(const std::string& exe, const std::string& args) {
    App a;
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE nul = ::CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    std::string cmd = "\"" + exe + "\" " + args;
    std::vector<char> mutableCmd(cmd.begin(), cmd.end());
    mutableCmd.push_back('\0');
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nul;
    si.hStdOutput = nul;
    si.hStdError = nul;
    PROCESS_INFORMATION pi{};
    const BOOL ok = ::CreateProcessA(nullptr, mutableCmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                                     nullptr, nullptr, &si, &pi);
    if (nul != INVALID_HANDLE_VALUE) { ::CloseHandle(nul); }
    if (ok) {
        ::CloseHandle(pi.hThread);
        a.process = pi.hProcess;
        a.pid = pi.dwProcessId;
        a.started = true;
    }
    return a;
}

bool waitForLog(const App& a, const fs::path& log, const std::string& needle, unsigned timeoutMs) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < until) {
        if (contains(readFile(log), needle)) { return true; }
        if (::WaitForSingleObject(a.process, 0) == WAIT_OBJECT_0) { return contains(readFile(log), needle); }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    return contains(readFile(log), needle);
}

std::vector<fs::path> filesMatching(const fs::path& dir, const std::string& prefix, const std::string& suffix) {
    std::vector<fs::path> out;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        const std::string n = e.path().filename().string();
        if (n.rfind(prefix, 0) == 0 && n.size() >= suffix.size() &&
            n.compare(n.size() - suffix.size(), suffix.size(), suffix) == 0) {
            out.push_back(e.path());
        }
    }
    return out;
}

// The start line's message, from a log file's text: `modules: ...`, as written after the
// stamp and level. Empty when there is none.
std::string startLineMessage(const std::string& log) {
    for (const std::string& l : linesOf(log)) {
        if (!isForeignModulesLogLine(l)) { continue; }
        const std::size_t at = l.find("modules: ");
        if (at != std::string::npos && l.find("module arrived") == std::string::npos) { return l.substr(at); }
    }
    return std::string();
}

#endif  // _WIN32

}  // namespace

int main(int argc, char** argv) {
#if defined(_WIN32)
    const std::string exe = std::string(CASCADE_APP_BINDIR) + "\\cascade.exe";
    CHECK(fs::exists(exe));
    // The DLL that stands in for another program's injected hook (tests/fixtures/late_fault_dll.cpp),
    // as built; argv[1].
    const std::string fixture = argc > 1 ? std::string(argv[1]) : std::string();
    const bool haveFixture = !fixture.empty() && fs::exists(fixture);
    if (!haveFixture) {
        std::printf("SKIP: no fixture DLL was given (argv[1]); the real arrival case did not run\n");
        ++g_checksSkipped;
    }

    // =======================================================================
    // A SENTINEL REPORT of a real session ended from outside while it was drawing: the
    // log FILE has the start line, a REAL ARRIVAL (a DLL the window's own thread maps on
    // frame 90, as an injected hook is) has its line, and the report's context block
    // carries both.
    // =======================================================================
    {
        const fs::path dir = scratch("sentinel");
        const fs::path log = dir / "logs" / "foxsdr.log";
        std::string injected;
        if (haveFixture) {
            // Outside the program's folder and Windows': the scratch tree.
            std::error_code cp;
            fs::copy_file(fixture, dir / "ForeignInjected.dll", fs::copy_options::overwrite_existing, cp);
            CHECK(!cp);
            injected = (dir / "ForeignInjected.dll").string();
        }
        App app;
        {
            ScratchEnv env;
            prepare(env, dir, true, injected);
            app = startApp(exe, "--frames 1000000");
        }
        CHECK(app.started);
        CHECK(waitForLog(app, log, "frame loop starting (bounded)", 60000));
        CHECK(waitForLog(app, log, "modules: ", 30000));

        const std::string logText = readFile(log);
        const std::string start = startLineMessage(logText);
        std::printf("this desktop's own cascade.exe, start line: %s\n", start.c_str());
        CHECK(!start.empty());
        // ONE start line, in the form the documents quote, within the width it is given.
        CHECK(countOf(logText, " modules: ") == 1);
        CHECK(start.rfind("modules: none foreign", 0) == 0 || start.rfind("modules: ", 0) == 0);
        CHECK(start.size() <= kForeignStartLineMaxChars);
        if (start != "modules: none foreign") { CHECK(contains(start, " foreign - ")); }
        // The start line is logged AFTER the sentinel and the diagnostics were armed, and
        // BEFORE the first frame's work is over: it is in the file already.
        CHECK(logText.find("frame loop starting (bounded)") != std::string::npos);

        // THE REAL ARRIVAL, written by the real frame loop's poll(): once, after the start
        // line, in the form the documents quote, with the seconds since the process started.
        if (haveFixture) {
            CHECK(waitForLog(app, log, "module arrived: ForeignInjected.dll", 60000));
            const std::string after = readFile(log);
            CHECK(countOf(after, "module arrived: ForeignInjected.dll (") == 1);
            CHECK(countOf(after, "module arrived: ") == 1);  // and nothing else arrived
            const std::size_t startAt = after.find(" modules: ");
            const std::size_t arrivedAt = after.find("module arrived: ");
            CHECK(startAt != std::string::npos && arrivedAt != std::string::npos && startAt < arrivedAt);
            const std::size_t at = after.find("module arrived: ForeignInjected.dll (");
            if (at != std::string::npos) {
                const std::size_t eol = after.find('\n', at);
                std::printf("a real arrival in the real program: %s\n", after.substr(at, eol - at).c_str());
                const double secs = std::atof(after.c_str() + at + std::strlen("module arrived: ForeignInjected.dll ("));
                CHECK(secs > 0.0 && secs < 120.0);
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(haveFixture ? 500 : 2500));  // frames, real ones
        ::TerminateProcess(app.process, 1);
        ::WaitForSingleObject(app.process, 30000);

        // The sentinel writes its report within a second or so of the death.
        std::vector<fs::path> reports;
        for (int i = 0; i < 200 && reports.empty(); ++i) {
            reports = filesMatching(dir / "crashes", "crash-", "-999999.txt");
            if (reports.empty()) { std::this_thread::sleep_for(std::chrono::milliseconds(50)); }
        }
        CHECK(reports.size() == 1);
        if (reports.size() == 1) {
            const std::string text = readFile(reports[0]);
            const std::string field = lineStarting(text, "foreign-modules: ");
            std::printf("the sentinel's report: %s\n", field.c_str());
            CHECK(!field.empty());
            // The context line is the list the start line named (the log's cap may have
            // cut it to "+K more"; every name the line did list is in the field).
            const std::set<std::string> fromLog = namesIn(start.substr(start.find(" - ") == std::string::npos
                                                                           ? start.size()
                                                                           : start.find(" - ") + 3));
            const std::set<std::string> fromReport = namesIn(field.substr(std::strlen("foreign-modules: ")));
            // The start line's names, and the arrival after it when there was one.
            std::set<std::string> expected = fromLog;
            if (haveFixture) { expected.insert("ForeignInjected.dll"); }
            CHECK(expected == fromReport);
            if (start == "modules: none foreign" && !haveFixture) { CHECK(field == "foreign-modules: (none)"); }
            // The report's own log tail carries the start line too (a short session), and
            // the arrival's.
            CHECK(contains(text, start));
            if (haveFixture) { CHECK(contains(text, "module arrived: ForeignInjected.dll (")); }
            // And what the uploader would send of it.
            ParsedReport p;
            CHECK(parseReportText(text, p));
            CHECK(!p.foreignModules.empty());
            CHECK(namesIn(p.foreignModules) == fromReport);
            if (haveFixture) { CHECK(namesIn(p.foreignModules).count("ForeignInjected.dll") == 1); }
        }
        std::error_code ec;
        if (g_checksFailed == 0) { fs::remove_all(dir, ec); }
    }

    // =======================================================================
    // A FREEZE REPORT from the real process: the in-process writer's context block (the
    // same bytes a crash report carries) has the line, and it is the list the log named.
    // =======================================================================
    {
        const fs::path dir = scratch("freeze");
        const fs::path log = dir / "logs" / "foxsdr.log";
        std::string out;
        {
            ScratchEnv env;
            prepare(env, dir, false);
            const std::string cmd = "\"\"" + exe + "\" --frames 240 --diag-stall 7000 2>&1\"";
            FILE* p = _popen(cmd.c_str(), "r");
            CHECK(p != nullptr);
            char buf[512];
            while (p != nullptr && std::fgets(buf, sizeof(buf), p) != nullptr) { out += buf; }
            if (p != nullptr) { _pclose(p); }
        }
        CHECK(contains(out, "rendered 240 frames"));
        const std::string logText = readFile(log);
        const std::string start = startLineMessage(logText);
        CHECK(!start.empty());
        const std::vector<fs::path> hangs = filesMatching(dir / "crashes", "hang-", ".txt");
        CHECK(!hangs.empty());
        if (!hangs.empty()) {
            const std::string text = readFile(hangs[0]);
            CHECK(contains(text, "kind: hang"));
            const std::size_t ctx = text.find("--- context ---");
            const std::size_t stack = text.find("--- thread ");
            const std::string field = lineStarting(text, "foreign-modules: ");
            std::printf("the freeze report: %s\n", field.c_str());
            CHECK(!field.empty());
            // In the context block, before the first stack.
            const std::size_t at = text.find("foreign-modules: ");
            CHECK(ctx != std::string::npos && at != std::string::npos && at > ctx);
            CHECK(stack == std::string::npos || at < stack);
            // Not "(not scanned yet)": the scan had long finished when the stall began.
            CHECK(field != "foreign-modules: (not scanned yet)");
            CHECK(field != "foreign-modules: (not recorded)");
            const std::set<std::string> fromLog = namesIn(start.substr(start.find(" - ") == std::string::npos
                                                                           ? start.size()
                                                                           : start.find(" - ") + 3));
            const std::set<std::string> fromReport = namesIn(field.substr(std::strlen("foreign-modules: ")));
            for (const std::string& n : fromLog) { CHECK(fromReport.count(n) == 1); }
            if (start == "modules: none foreign") { CHECK(field == "foreign-modules: (none)"); }
        }
        std::error_code ec;
        if (g_checksFailed == 0) { fs::remove_all(dir, ec); }
    }
#else
    (void)argc;
    (void)argv;
    SKIP_LINUX("the module list and the loader notification are Windows only");
#endif
    return testSummary("test_foreign_modules_app");
}
