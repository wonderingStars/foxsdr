// THE SCRIPTED INPUT'S DRIVEN RUNS (src/slint/input_script.hpp; docs/engine-stage4.md 6.47): foxsdr-slint itself, on a scratch profile
// with a scripted pointer and keyboard, every step read back from the engine.
//
//   no argument   the first real driven run: the receiver stopped and started from the strip's key, a bookmark typed and added from the
//                 TOOLS bank, each effect read back through the table (running, count.bookmarks, the list's names) - the proof that a
//                 script reaches a real control and the engine answers;
//   <script>      one script of the drive suite (tests/drive/*.txt, registered one test each by src/slint/CMakeLists.txt): its "#!" header
//                 names the run (frames=, bank=, sections=, view=, open=, store= and any FOXSDR_* variable), the window plays it, and
//                 the run must say every step ran and none failed.
//
// Isolated like every test that starts the application (tests/test_bookmark_tick_app.cpp): APPDATA and the rest point into a scratch folder
// with this process's id in its name, CASCADE_CONFIG_TEST names the run's settings file, every network endpoint is a dead port. Runs on the
// hidden desktop under the gui label (it opens a window).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "test_check.hpp"

namespace fs = std::filesystem;

namespace {

int pid() {
#if defined(_WIN32)
    return static_cast<int>(::GetCurrentProcessId());
#else
    return static_cast<int>(::getpid());
#endif
}

void setEnv(const char* name, const std::string& value) {
#if defined(_WIN32)
    ::SetEnvironmentVariableA(name, value.empty() ? nullptr : value.c_str());
    _putenv_s(name, value.c_str());
#else
    if (value.empty()) { ::unsetenv(name); } else { ::setenv(name, value.c_str(), 1); }
#endif
}

// The run's output and its exit code.
struct Run {
    std::string out;
    int exitCode = -1;
};

Run run(const std::string& cmd) {
    Run r;
#if defined(_WIN32)
    FILE* p = _popen(("\"" + cmd + "\"").c_str(), "r");
#else
    FILE* p = popen(cmd.c_str(), "r");
#endif
    char buf[1024];
    while (p != nullptr && std::fgets(buf, sizeof(buf), p) != nullptr) { r.out += buf; }
#if defined(_WIN32)
    if (p != nullptr) { r.exitCode = _pclose(p); }
#else
    if (p != nullptr) { const int st = pclose(p); r.exitCode = WIFEXITED(st) ? WEXITSTATUS(st) : -1; }
#endif
    return r;
}

std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// The "#!" header lines of a script: key=value tokens. Known keys name the run; an upper-case key is an environment variable.
struct Header {
    int frames = 240;
    std::map<std::string, std::string> env;
};

Header parseHeader(const std::string& script) {
    Header h;
    std::istringstream in(script);
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("#!", 0) != 0) { continue; }
        std::istringstream toks(line.substr(2));
        std::string tok;
        while (toks >> tok) {
            const std::size_t eq = tok.find('=');
            if (eq == std::string::npos) { continue; }
            const std::string key = tok.substr(0, eq), value = tok.substr(eq + 1);
            if (key == "frames") { h.frames = std::atoi(value.c_str()); }
            else if (key == "bank") { h.env["FOXSDR_SLINT_BANK"] = value; }
            else if (key == "sections") { h.env["FOXSDR_SLINT_SECTIONS"] = value; }
            else if (key == "view") { h.env["FOXSDR_SLINT_VIEW"] = value; }
            else if (key == "open") { h.env["FOXSDR_SLINT_OPEN"] = value; }
            else if (key == "store") { h.env["FOXSDR_SLINT_STORE_PAGE"] = value; }
            else if (key == "size") { h.env["FOXSDR_SLINT_SIZE"] = value; }   // <w>x<h>: a taller window for a tall section body
            else if (key.rfind("FOXSDR_", 0) == 0 || key.rfind("CASCADE_", 0) == 0) { h.env[key] = value; }
        }
    }
    return h;
}

// The first real driven run (no argument).
const char* kBuiltIn =
    "#! frames=240 bank=2 sections=2\n"
    "# the TOOLS bank with its Bookmarks body open; the generator source\n"
    "10 dump\n"
    "state running == 1\n"
    "state deviceName == \"Signal generator\"\n"
    "state count.bookmarks == 0\n"
    "20 click \"Stop the receiver (Ctrl+F2)\"\n"
    "+15 state running == 0\n"
    "expect \"Start the receiver (Ctrl+F2)\" enabled equals \"true\"\n"
    "+1 click \"Start the receiver (Ctrl+F2)\"\n"
    "+25 state running == 1\n"
    "+1 type \"Bookmark name\" \"Probe\"\n"
    "+2 expect \"Bookmark name\" value equals \"Probe\"\n"
    "+1 click \"Add current\"\n"
    "+25 state count.bookmarks == 1\n"
    "state list.bookmarks.names contains Probe\n"
    "# a dropdown: the receiver strip's MODE key is labelled by its value; the popup row \"AM\" is clicked and the engine's mode read back\n"
    "+1 state mode == WFM\n"
    "+1 choose \"WFM\" \"AM\"\n"
    "+15 state mode == AM\n"
    "expect \"AM\" value equals \"AM\"\n"
    "+1 state cfg.uiTheme == today\n"
    "+1 shot \"drive-run.png\"\n";

}  // namespace

int main(int argc, char** argv) {
    std::printf("test_slint_drive_run\n");
    std::string script = kBuiltIn;
    std::string tag = "builtin";
    if (argc > 1) {
        script = readFile(argv[1]);
        tag = fs::path(argv[1]).stem().string();
        if (script.empty()) {
            std::printf("  cannot read the script %s\n", argv[1]);
            CHECK(!script.empty());
            return testSummary("test_slint_drive_run");
        }
    }
    const Header h = parseHeader(script);

    const fs::path dir = fs::temp_directory_path() / ("foxsdr-slint-drive-" + tag + "-" + std::to_string(pid()));
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir / "appdata", ec);
    fs::create_directories(dir / "local", ec);
    fs::create_directories(dir / "home", ec);
    fs::create_directories(dir / "diag", ec);
    setEnv("APPDATA", (dir / "appdata").string());
    setEnv("LOCALAPPDATA", (dir / "local").string());
    setEnv("XDG_CONFIG_HOME", (dir / "appdata").string());
    setEnv("XDG_DATA_HOME", (dir / "local").string());
    setEnv("XDG_STATE_HOME", (dir / "local").string());
    setEnv("FOXSDR_DIAG_DIR", (dir / "diag").string());
    setEnv("USERPROFILE", (dir / "home").string());
    setEnv("HOME", (dir / "home").string());
    for (const char* url : {"FOXSDR_TELEMETRY_URL", "FOXSDR_CRASH_URL", "FOXSDR_UPDATE_URL", "FOXSDR_REPORTS_URL", "FOXSDR_FEATURE_URL", "FOXSDR_PROBLEM_URL"}) {
        setEnv(url, "http://127.0.0.1:9/");
    }
    const fs::path cfg = dir / "config.json";
    {
        std::ofstream f(cfg, std::ios::binary | std::ios::trunc);
        f << "{ \"telemetryEnabled\": false, \"updateCheckEnabled\": false, \"sourceKind\": \"siggen\", \"mainView\": \"receiver\", \"bandPlanOverlay\": false }\n";
    }
    const fs::path scriptPath = dir / "input.txt";
    {
        std::ofstream f(scriptPath, std::ios::binary | std::ios::trunc);
        f << script;
    }
    setEnv("CASCADE_CONFIG_TEST", cfg.string());
    setEnv("FOXSDR_INPUT_SCRIPT", scriptPath.string());
    // A bounded run keeps the receiver STOPPED unless CASCADE_DECODE_TEST asks (engine/window_service_options.cpp); a driven run wants the
    // engine running as a session has it - the generator fed, the scanner able to scan, STOP meaning something. A header line
    // "CASCADE_DECODE_TEST=" (empty) keeps the receiver stopped for a script that wants that.
    setEnv("CASCADE_DECODE_TEST", "1");
    for (const auto& [k, v] : h.env) { setEnv(k.c_str(), v); }

    const std::string exe = FOXSDR_SLINT_EXE;
    const Run r = run("\"" + exe + "\" --frames " + std::to_string(h.frames) + " 2>&1");
    const std::string ticks = std::to_string(h.frames) + " of " + std::to_string(h.frames) + " ticks, exit 0";
    const bool finished = r.out.find(ticks) != std::string::npos;
    std::string summary;
    {
        const std::size_t at = r.out.find("input script: ");
        if (at != std::string::npos) {
            const std::size_t end = r.out.find('\n', at);
            summary = r.out.substr(at, end == std::string::npos ? std::string::npos : end - at);
            // the summary is the LAST such line (the first is "input script: N steps from <file>")
            std::size_t more = r.out.find("input script: ", at + 1);
            while (more != std::string::npos) {
                const std::size_t e2 = r.out.find('\n', more);
                const std::string cand = r.out.substr(more, e2 == std::string::npos ? std::string::npos : e2 - more);
                if (cand.find(" steps, ") != std::string::npos) { summary = cand; }
                more = r.out.find("input script: ", more + 1);
            }
        }
    }
    const bool clean = summary.find(", 0 failed, 0 not reached") != std::string::npos;
    CHECK(r.exitCode == 0);
    CHECK(finished);
    CHECK(clean);
    // FOXSDR_DRIVE_VERBOSE=1 prints the whole run (a script author reads its `dump` from here); a failed run prints it anyway.
    const char* verbose = std::getenv("FOXSDR_DRIVE_VERBOSE");
    if (r.exitCode != 0 || !finished || !clean || (verbose != nullptr && *verbose == '1')) {
        std::printf("  %s: exit %d; %s\n--- the run's output ---\n%s\n--- end ---\n", tag.c_str(), r.exitCode, summary.c_str(), r.out.c_str());
    }
    if (r.exitCode == 0 && finished && clean) {
        std::printf("  %s: %s\n", tag.c_str(), summary.c_str());
        // the step lines, for the record
        std::istringstream lines(r.out);
        std::string line;
        while (std::getline(lines, line)) {
            if (line.find("input script line ") != std::string::npos) { std::printf("  %s\n", line.c_str()); }
        }
    }
    if (tag == "builtin") {
        CHECK(r.out.find("input script line 3: dump -> ok") != std::string::npos);
        CHECK(r.out.find("labelled controls") != std::string::npos);
        CHECK(fs::exists(dir / "screenshots" / "drive-run.png"));
    }
    return testSummary("test_slint_drive_run");
}
