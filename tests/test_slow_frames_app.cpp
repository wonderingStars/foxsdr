// test_slow_frames_app.cpp - the slow-frame record through the REAL application
// (core/frame_timing.hpp, 0.99.64): a frame of the real window that is made slow
// in a known place is attributed to the scope that place belongs to, in the right
// tier, with one log line; and what must not count does not.
//
// TWO HALVES, because the claims are about two different things.
//
//  1. THE WINDOW, as a child process. `cascade --frames N` is the real frame loop
//     on a real window. It prints its table at the end ("cascade: frame timing:
//     ...") and writes its log to a scratch diagnostics folder, and both are read
//     back. A frame is made slow where the test says by FOXSDR_INPUT_SCRIPT's
//     `sleep` step (an existing seam: it sits in the scope `frame-start`), by
//     --diag-stall (an existing seam: after the last scope, so `other`), and for
//     every other scope by FOXSDR_FRAME_STALL, a hook this feature adds that holds
//     the loop INSIDE a named scope at a named frame - the hook proves each scope
//     is entered where the frame says it is, which a sleep somewhere else could
//     not. The three things the operating system does that a test cannot make it
//     do - hide the window, change the display, run a modal loop - are played by
//     FOXSDR_FRAME_SITUATION into the SAME code the real events reach
//     (PresentGrace, the held frame, the modal-loop counter's reading).
//
//  2. THE REAL CODE BEHIND THREE OF THE SCOPES, in process. A plugin reload is
//     made slow through the existing seam in its path (the plugins folder's
//     signature read, tests/test_plugin_rescan_skip.cpp's) against a real module;
//     a shell call through the REAL watchdog bracket (AppWindow::watchdogShellHooks,
//     the one user-paced pause there is); and a recording's file on a disk that
//     takes 700 ms to answer, through the record-start opener seam
//     (tests/test_record_start.cpp's). The first must count; the second must not;
//     and the third must not either - the open is a worker's, so the window's
//     frames go on, which is the whole of what moving it off the thread was for.
//
// WHAT IS NOT ASSERTED, deliberately: that an unmodified frame is never slow, or
// how long one takes. This runs beside other agents' builds and test suites on a
// desktop that may not be pacing frames; what is asserted is what the
// instrumentation ATTRIBUTED, with margins, and what is printed (the table of a
// clean run, its mean and longest frame) is for whoever reads the log.
//
// Hermetic: the per-user directories are scratch folders with the process id in
// their names; no network, no radio, no real profile.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "core/diag_log.hpp"
#include "core/frame_timing.hpp"
#include "core/package_identity.hpp"
#include "core/plugin_dir_signature.hpp"
#include "core/plugin_host.hpp"
#include "core/recorder.hpp"
#include "gui/app_window.hpp"
#include "gui/shell_open.hpp"
#include "gui/win_frame.hpp"
#include "test_check.hpp"

#if defined(_WIN32)
#include <GLFW/glfw3.h>
#endif

namespace fs = std::filesystem;
using namespace cascade::core;

namespace {

fs::path g_dir;

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
    if (value.empty()) {
        ::unsetenv(name);
    } else {
        ::setenv(name, value.c_str(), 1);
    }
#endif
}

std::string exePath() {
#if defined(_WIN32)
    return std::string(CASCADE_APP_BINDIR) + "/cascade.exe";
#else
    return std::string(CASCADE_APP_BINDIR) + "/cascade";
#endif
}

std::string run(const std::string& cmd) {
    std::string out;
#if defined(_WIN32)
    FILE* p = _popen(("\"" + cmd + "\"").c_str(), "r");
#else
    FILE* p = popen(cmd.c_str(), "r");
#endif
    char buf[512];
    while (p != nullptr && std::fgets(buf, sizeof(buf), p) != nullptr) { out += buf; }
#if defined(_WIN32)
    if (p != nullptr) { _pclose(p); }
#else
    if (p != nullptr) { pclose(p); }
#endif
    return out;
}

// --- one run of the real application --------------------------------------------

struct Table {
    bool parsed = false;
    long frames = 0;
    double meanMs = 0.0;
    double longestMs = 0.0;
    std::string longestScope;
    double switchesPerFrame = 0.0;
    std::string text;  // "recorder 2/0/0, ..." or "none"
    long notCounted = 0;
    std::map<std::string, std::array<int, 3>> rows;

    int total(const char* scope) const {
        const auto it = rows.find(scope);
        return it == rows.end() ? 0 : it->second[0] + it->second[1] + it->second[2];
    }
    int tier(const char* scope, int t) const {
        const auto it = rows.find(scope);
        return it == rows.end() ? 0 : it->second[static_cast<std::size_t>(t)];
    }
    int everything() const {
        int n = 0;
        for (const auto& [k, v] : rows) { n += v[0] + v[1] + v[2]; }
        return n;
    }
};

struct Run {
    bool ok = false;
    std::string out;  // the child's stdout and stderr
    std::string log;  // every foxsdr*.log it wrote
    Table table;
};

Table parseTable(const std::string& out) {
    Table t;
    const std::string key = "cascade: frame timing: ";
    const std::size_t at = out.find(key);
    if (at == std::string::npos) { return t; }
    std::string line = out.substr(at + key.size(), out.find('\n', at) - at - key.size());
    if (!line.empty() && line.back() == '\r') { line.pop_back(); }
    // "<N> frames, mean <x> ms, longest <y> ms (in <scope>), <s> scope switches a frame;
    //  slow frames: <text>; slow frames not counted: <e>"
    char scope[64] = {};
    double mean = 0, longest = 0, sw = 0;
    long frames = 0;
    if (std::sscanf(line.c_str(),
                    "%ld frames, mean %lf ms, longest %lf ms (in %63[^)]), %lf scope switches", &frames,
                    &mean, &longest, scope, &sw) != 5) {
        return t;
    }
    t.frames = frames;
    t.meanMs = mean;
    t.longestMs = longest;
    t.longestScope = scope;
    t.switchesPerFrame = sw;
    const std::string sf = "slow frames: ";
    const std::size_t a = line.find(sf);
    const std::size_t b = line.find("; slow frames not counted: ");
    if (a == std::string::npos || b == std::string::npos) { return t; }
    t.text = line.substr(a + sf.size(), b - a - sf.size());
    t.notCounted = std::atol(line.c_str() + b + std::strlen("; slow frames not counted: "));
    if (t.text != "none") {
        std::stringstream ss(t.text);
        std::string item;
        while (std::getline(ss, item, ',')) {
            while (!item.empty() && item.front() == ' ') { item.erase(0, 1); }
            const std::size_t sp = item.find(' ');
            if (sp == std::string::npos) { continue; }
            std::array<int, 3> v{0, 0, 0};
            std::sscanf(item.c_str() + sp + 1, "%d/%d/%d", &v[0], &v[1], &v[2]);
            t.rows[item.substr(0, sp)] = v;
        }
    }
    t.parsed = true;
    return t;
}

std::string readLogs(const fs::path& dir) {
    std::string all;
    std::error_code ec;
    for (const auto& e : fs::recursive_directory_iterator(dir, ec)) {
        if (!e.is_regular_file(ec)) { continue; }
        const std::string name = e.path().filename().string();
        if (name.rfind("foxsdr", 0) != 0 || e.path().extension() != ".log") { continue; }
        std::ifstream in(e.path(), std::ios::binary);
        all.append((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    return all;
}

int countLines(const std::string& log, const std::string& what) {
    int n = 0;
    std::stringstream ss(log);
    std::string line;
    while (std::getline(ss, line)) {
        if (line.find(what) != std::string::npos) { ++n; }
    }
    return n;
}

// The first line containing `what`, or "".
std::string findLine(const std::string& log, const std::string& what) {
    std::stringstream ss(log);
    std::string line;
    while (std::getline(ss, line)) {
        if (line.find(what) != std::string::npos) { return line; }
    }
    return std::string();
}

struct Setup {
    std::string tag;
    int frames = 100;
    std::string script;       // empty: none
    std::string stalls;       // FOXSDR_FRAME_STALL
    std::string situation;    // FOXSDR_FRAME_SITUATION
    std::string extraArgs;    // e.g. "--diag-stall 400"
    bool patchView = false;   // FOXSDR_OPEN_PATCH
};

Run once(const Setup& s) {
    Run r;
    const fs::path base = g_dir / s.tag;
    std::error_code ec;
    fs::create_directories(base / "diag", ec);
    const fs::path cfg = base / "cfg.json";
    {
        std::ofstream f(cfg, std::ios::binary | std::ios::trunc);
        f << "{ \"telemetryEnabled\": false, \"updateCheckEnabled\": false, "
             "\"sourceKind\": \"siggen\", \"uiTheme\": \"today\", \"interfaceScale\": \"100\", "
             "\"mainView\": \"receiver\", \"railBank\": 2, \"bandPlanOverlay\": false }\n";
    }
    setEnv("CASCADE_CONFIG_TEST", cfg.string());
    setEnv("FOXSDR_DIAG_DIR", (base / "diag").string());
    if (s.script.empty()) {
        setEnv("FOXSDR_INPUT_SCRIPT", "");
    } else {
        const fs::path sp = base / "run.script";
        std::ofstream f(sp, std::ios::binary | std::ios::trunc);
        f << s.script;
        f.close();
        setEnv("FOXSDR_INPUT_SCRIPT", sp.string());
    }
    setEnv("FOXSDR_FRAME_STALL", s.stalls);
    setEnv("FOXSDR_FRAME_SITUATION", s.situation);
    setEnv("FOXSDR_OPEN_PATCH", s.patchView ? "1" : "");
    r.out = run("\"" + exePath() + "\" --frames " + std::to_string(s.frames) +
                (s.extraArgs.empty() ? "" : " " + s.extraArgs) + " 2>&1");
    r.ok = r.out.find("rendered " + std::to_string(s.frames) + " frames") != std::string::npos;
    r.table = parseTable(r.out);
    r.log = readLogs(base / "diag");
    if (!r.ok || !r.table.parsed) {
        std::printf("  %s: the run did not finish cleanly or printed no table:\n%s\n", s.tag.c_str(),
                    r.out.c_str());
    }
    setEnv("FOXSDR_FRAME_STALL", "");
    setEnv("FOXSDR_FRAME_SITUATION", "");
    setEnv("FOXSDR_OPEN_PATCH", "");
    return r;
}

// --- the real code behind two scopes, in process --------------------------------

// What the capture sink collects.
std::vector<std::pair<bool, std::string>> g_lines;
void captureSink(bool warn, const char* line) { g_lines.emplace_back(warn, line); }

// The plugins folder's signature read, slowed: it is called INSIDE a rescan, so a
// pause here is a pause inside a plugin reload.
bool g_slowRead = false;
bool slowSignatureRead(const std::string& dir, PluginDirSignature& out) {
    if (g_slowRead) { std::this_thread::sleep_for(std::chrono::milliseconds(450)); }
    return readPluginDirSignature(dir, out);
}

}  // namespace

namespace cascade::gui {

struct AppWindowTestAccess {
    static void installHooks() { AppWindow::testHooks_.pluginDirSignature = &slowSignatureRead; }
    static void rescan(AppWindow& a) { a.rescanPlugins(); }
    static void rescanIfChanged(AppWindow& a) { a.rescanPluginsIfChanged(); }
    static ShellPauseHooks shellHooks(AppWindow& a) { return a.watchdogShellHooks(); }
    static unsigned pauses(AppWindow& a) { return a.watchdog_.pausesTaken(); }
    static std::size_t loaded(const AppWindow& a) { return a.pluginHost_.loadedCount(); }
    static std::string bundle(AppWindow& a) { return a.currentDiagnosticsBundle(); }
    // The recorders and their start, as tests/test_record_start.cpp reaches them.
    static void setRecordDir(AppWindow& a, std::string d) { a.recordDir_ = std::move(d); }
    static Recorder::Opener opener(AppWindow& a) { return a.audioRecorder_.opener(); }
    static void bindOpener(AppWindow& a, Recorder::Opener o) {
        a.iqRecorder_.bindOpener(o);
        a.audioRecorder_.bindOpener(std::move(o));
    }
    static bool startAudio(AppWindow& a) { return a.startAudioRecording(); }
    static void stopAudio(AppWindow& a) { a.stopAudioRecording(); }
    static void poll(AppWindow& a, double nowS) { a.pollRecordStarts(nowS); }
    static bool audioRecording(AppWindow& a) { return a.audioRecorder_.recording(); }
};

}  // namespace cascade::gui

using Access = cascade::gui::AppWindowTestAccess;

namespace {

#if defined(_WIN32)
const char* const kExt = ".dll";
#else
const char* const kExt = ".so";
#endif

void isolate(const std::string& probeModule) {
    g_dir = fs::temp_directory_path() / ("cascade-slowframes-" + std::to_string(pid()));
    std::error_code ec;
    fs::remove_all(g_dir, ec);
    fs::create_directories(g_dir / "appdata", ec);
    fs::create_directories(g_dir / "local", ec);
    const std::string appdata = (g_dir / "appdata").string();
    const std::string local = (g_dir / "local").string();
    setEnv("APPDATA", appdata);
    setEnv("LOCALAPPDATA", local);
    setEnv("USERPROFILE", local);
    setEnv("HOME", local);
    setEnv("XDG_CONFIG_HOME", appdata);
    setEnv("XDG_DATA_HOME", local);
    setEnv("XDG_STATE_HOME", local);
    setEnv("XDG_CACHE_HOME", local);
    for (const char* url : {"FOXSDR_TELEMETRY_URL", "FOXSDR_CRASH_URL", "FOXSDR_UPDATE_URL",
                            "FOXSDR_REPORTS_URL", "FOXSDR_FEATURE_URL", "FOXSDR_PROBLEM_URL"}) {
        setEnv(url, "http://127.0.0.1:9/");
    }
    setEnv("FOXSDR_SINGLE_VIEWPORT", "1");
    setEnv("FOXSDR_WINDOW_SIZE", "1600x1000");
    setEnv("FOXSDR_PATCH_START", "");
    setEnv("FOXSDR_PATCH_FILE", "");
    setEnv("CASCADE_DECODE_TEST", "");
    setEnv("SOAPY_SDR_PLUGIN_PATH", "");

    // The in-process half: a package identity makes the plugins folder the
    // per-user one in the scratch tree, never the build tree's, and the real
    // probe module is put in it so a reload has a module to unmap and map.
    PackageIdentity id;
    id.packaged = true;
    id.fullName = "slow-frames-test";
    setPackageIdentityForTest(id);
    const fs::path plugins = fs::path(PluginHost::userPluginDir());
    fs::create_directories(plugins, ec);
    fs::copy_file(probeModule, plugins / (std::string("probe_a") + kExt),
                  fs::copy_options::overwrite_existing, ec);
    CHECK(!ec);
}

// ------------------------------------------------------------------------------

void testACleanRun() {
    std::printf("a scripted run of the unmodified window\n");
    Setup s;
    s.tag = "clean";
    s.frames = 150;
    s.script = "40 screen 300 300\n60 screen 500 400\n80 screen 200 200\n";
    const Run r = once(s);
    CHECK(r.ok && r.table.parsed);
    std::printf("    %ld frames, mean %.2f ms, longest %.1f ms (in %s), %.1f scope switches a "
                "frame; slow frames: %s; not counted: %ld\n",
                r.table.frames, r.table.meanMs, r.table.longestMs, r.table.longestScope.c_str(),
                r.table.switchesPerFrame, r.table.text.c_str(), r.table.notCounted);
    CHECK(r.table.frames == 150);
    // The guards are in the frame: two dozen scope switches a frame, give or take.
    // (Against an application without them this is zero, and there is no table.)
    CHECK(r.table.switchesPerFrame >= 20.0 && r.table.switchesPerFrame <= 80.0);
    CHECK(r.table.meanMs > 0.0);
}

void testAScriptsSleepIsFrameStartAndIsRateLimited() {
    std::printf("a script's sleep: its scope, its tier, one log line for two\n");
    Setup s;
    s.tag = "sleep";
    s.frames = 100;
    // Two sleeps ten frames apart: far inside the 30 s a scope's lines are
    // limited to. The step runs before ImGui::NewFrame, in `frame-start`.
    s.script = "40 sleep 400\n50 sleep 400\n";
    const Run r = once(s);
    CHECK(r.ok && r.table.parsed);
    // Both are counted: each one is a slow frame of 400 ms or a little more.
    CHECK(r.table.total("frame-start") == 2);
    CHECK(r.table.tier("frame-start", 2) == 0);  // nowhere near five seconds
    // ...and ONE line says so: the first. (An exact count of this scope's lines
    // and nothing else: another scope may have had a hitch on a busy machine.)
    const int lines = countLines(r.log, "of it in frame-start");
    CHECK(lines == 1);
    const std::string line = findLine(r.log, "of it in frame-start");
    std::printf("    log: %s\n", line.c_str());
    // "frame: <n> ms, <m> ms of it in frame-start": 400 ms of sleep and a frame.
    const std::size_t at = line.find("frame: ");
    long ms = -1;
    if (at != std::string::npos) { ms = std::atol(line.c_str() + at + 7); }
    CHECK(ms >= 390 && ms < 2500);
    // The summary at shutdown, once, naming it.
    CHECK(countLines(r.log, "frame: slow frames this session - ") == 1);
    CHECK(findLine(r.log, "frame: slow frames this session - ").find("frame-start") !=
          std::string::npos);
    // Fixed words only: no path, no name of anything.
    CHECK(line.find('\\') == std::string::npos && line.find('/') == std::string::npos);
}

void testEveryScopeIsEnteredWhereTheFrameSaysItIs() {
    std::printf("every scope the frame enters, made slow in place\n");
    // One scope a frame, three frames apart, 300 ms each - a stutter, and clear of
    // the tier above it. Frame 60 is --diag-stall's, which sits after the last
    // scope of the frame: `other`. Frame 10 is start-up.
    const std::vector<std::pair<const char*, int>> plan = {
        {"events", 40},    {"frame-start", 43}, {"pre-draw", 46}, {"plugin-panels", 49},
        {"toolbar", 52},   {"rail", 55},        {"spectrum", 58}, {"status", 64},
        {"dialogs", 67},   {"polls", 70},       {"saves", 73},    {"render", 76},
        {"present", 79},   {"recorder", 82}};
    std::string stalls = "rail=300@10";  // start-up
    for (const auto& [scope, frame] : plan) {
        stalls += std::string(",") + scope + "=300@" + std::to_string(frame);
    }
    Setup s;
    s.tag = "scopes";
    s.frames = 100;
    s.stalls = stalls;
    s.extraArgs = "--diag-stall 400";
    const Run r = once(s);
    CHECK(r.ok && r.table.parsed);
    std::printf("    table: %s\n", r.table.text.c_str());
    for (const auto& [scope, frame] : plan) {
        (void)frame;
        // Attributed to the scope that was made slow - not to its neighbour.
        const int n = r.table.total(scope);
        if (n < 1) { std::printf("    NOT ATTRIBUTED: %s\n", scope); }
        CHECK(n >= 1);
        // And logged, in the line's own words.
        CHECK(countLines(r.log, std::string("of it in ") + scope) >= 1);
    }
    // The existing seam: a deliberate wedge after the swap is time in no scope.
    CHECK(r.table.total("other") >= 1);
    CHECK(countLines(r.log, "of it in other") >= 1);
    // START-UP is its own scope and says what part of the frame took the time.
    CHECK(r.table.total("startup") == 1);
    CHECK(r.table.total("rail") >= 1);  // frame 55
    const std::string startup = findLine(r.log, "of it in startup");
    std::printf("    log: %s\n", startup.c_str());
    CHECK(startup.find("largest part: rail") != std::string::npos);
    // A 300 ms stall is a stutter, not a freeze: the first tier, in the ones it
    // was placed in (a loaded machine may add a second to another scope).
    CHECK(r.table.tier("rail", 2) == 0 && r.table.tier("polls", 2) == 0);
}

void testThePatchPage() {
    std::printf("the patch page, when it is the main view\n");
    Setup s;
    s.tag = "patch";
    s.frames = 80;
    s.patchView = true;
    s.stalls = "patch=300@50";
    const Run r = once(s);
    CHECK(r.ok && r.table.parsed);
    CHECK(r.table.total("patch") >= 1);
    CHECK(countLines(r.log, "of it in patch") >= 1);
}

void testWhatMustNotCount() {
    std::printf("a hidden window, a display change, a modal loop: not slow frames\n");
    {
        // 35: counted (the control). 50: inside the hidden stretch 40-60. 65: after
        // it - counted again. 99: the swap that stalls just before the display
        // change is reported at 100 (held, then dropped). 103: inside the grace
        // the change opens (ten seconds - the rest of the run).
        Setup s;
        s.tag = "away";
        s.frames = 125;
        s.situation = "hidden@40-60,display@100";
        s.stalls = "rail=400@35,rail=400@50,polls=400@65,present=400@99,status=400@103";
        const Run r = once(s);
        CHECK(r.ok && r.table.parsed);
        std::printf("    table: %s; not counted: %ld\n", r.table.text.c_str(), r.table.notCounted);
        // THE CONTROLS: the same stall, outside the situation, is counted.
        CHECK(r.table.total("rail") == 1);   // 35 and not 50
        CHECK(r.table.total("polls") == 1);  // 65 - the hidden stretch ended
        // THE EXCLUSIONS.
        CHECK(r.table.total("present") == 0);
        CHECK(r.table.total("status") == 0);
        // They WERE slow: the three stalls are counted as slow-and-excluded, so
        // the zeros above are not a stall that never ran.
        CHECK(r.table.notCounted >= 3);
        CHECK(countLines(r.log, "of it in present") == 0);
        CHECK(countLines(r.log, "of it in status") == 0);
        CHECK(countLines(r.log, "of it in rail") == 1);
        CHECK(countLines(r.log, "display changed") >= 1);
    }
    {
        // A modal loop in frame 80's message pump: 700 ms of the person holding
        // the window. A pump of the same length in frame 90, with none, counts.
        Setup s;
        s.tag = "modal";
        s.frames = 110;
        s.situation = "modal@80";
        s.stalls = "events=700@80,events=700@90";
        const Run r = once(s);
        CHECK(r.ok && r.table.parsed);
        std::printf("    table: %s; not counted: %ld\n", r.table.text.c_str(), r.table.notCounted);
        CHECK(r.table.total("events") == 1);
        CHECK(countLines(r.log, "of it in events") == 1);
    }
}

// --- in process -----------------------------------------------------------------

void testAPluginReloadUnderItsPauseCounts() {
    std::printf("a plugin reload, slowed in its own path, under its watchdog pause\n");
    cascade::gui::AppWindow app;  // its own start-up scan maps the probe module
    CHECK(Access::loaded(app) == 1u);

    FrameTimer& t = frameTimer();
    t.resetForTest();
    g_lines.clear();
    t.setSinkForTest(&captureSink);

    g_slowRead = true;
    const unsigned pausesBefore = Access::pauses(app);
    t.beginFrame(100);  // past start-up
    {
        // As pollPluginAsync() reaches it from the frame's polls.
        FrameScopeGuard polls(FrameScope::Polls);
        Access::rescan(app);
    }
    t.endFrame();
    t.finish();
    g_slowRead = false;

    // The application-paced pause was held (it is what excuses the WATCHDOG), and
    // the frame counted all the same: it is its own scope.
    CHECK(Access::pauses(app) > pausesBefore);
    const SlowFrameCounts n = slowFrameCounts();
    std::printf("    table: %s\n", slowFramesText(n).c_str());
    CHECK(n.count[static_cast<int>(FrameScope::PluginsReload)][0] == 1);
    CHECK(n.total() == 1);
    bool lineOk = false;
    for (const auto& [warn, text] : g_lines) {
        (void)warn;
        if (text.find("of it in plugins-reload") != std::string::npos) {
            const long ms = std::atol(text.c_str() + std::strlen("frame: "));
            lineOk = ms >= 440;  // the 450 ms the seam held it, less nothing
        }
    }
    CHECK(lineOk);

    // The path that checks first and skips (a catalogue refresh with nothing to
    // find) is the same scope: it is slow only if the listing it makes is.
    t.resetForTest();
    g_lines.clear();
    t.setSinkForTest(&captureSink);
    g_slowRead = true;
    t.beginFrame(101);
    Access::rescanIfChanged(app);
    t.endFrame();
    t.finish();
    g_slowRead = false;
    CHECK(slowFrameCounts().count[static_cast<int>(FrameScope::PluginsReload)][0] == 1);
    t.resetForTest();
}

void testAShellCallIsAPersonsTimeNotAFrames() {
    std::printf("a shell call through the real watchdog bracket: a person's time\n");
    cascade::gui::AppWindow app;
    FrameTimer& t = frameTimer();
    t.resetForTest();
    g_lines.clear();
    t.setSinkForTest(&captureSink);

    const unsigned pausesBefore = Access::pauses(app);
    FrameScope inside = FrameScope::Other;
    const auto wall0 = std::chrono::steady_clock::now();
    t.beginFrame(100);
    {
        FrameScopeGuard rail(FrameScope::Rail);
        // What AppWindow::shellOpen and launchInstaller are made of: a call run
        // between the bracket's two ends - here, a person reading a prompt.
        const bool r = cascade::gui::runShellOpen(Access::shellHooks(app), [&]() -> bool {
            inside = currentFrameScope();
            std::this_thread::sleep_for(std::chrono::milliseconds(700));
            return true;
        });
        CHECK(r);
    }
    t.endFrame();
    t.finish();
    const double wallMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - wall0).count();

    CHECK(wallMs >= 690.0);                        // the frame really did take that long
    CHECK(inside == FrameScope::UserWait);         // and the bracket was what said so
    CHECK(Access::pauses(app) == pausesBefore + 1);  // the watchdog's side is intact
    CHECK(t.scopeNanos(FrameScope::UserWait) >= 650'000'000LL);
    CHECK(t.longestFrameNs() < 250'000'000LL);
    CHECK(!slowFrameCounts().any());
    CHECK(g_lines.empty());
    CHECK(t.excludedSlowFrames() == 0);
    t.resetForTest();
}

void testASlowDiskIsNotASlowFrame() {
    std::printf("a recording's file on a slow disk: the frames go on\n");
    cascade::gui::AppWindow app;
    const fs::path recDir = g_dir / "recordings";
    std::error_code ec;
    fs::create_directories(recDir, ec);
    Access::setRecordDir(app, recDir.string());

    // THE RECORD-START OPENER SEAM (tests/test_record_start.cpp's): the recorder's
    // own opener behind a disk that takes 700 ms to answer.
    const Recorder::Opener real = Access::opener(app);
    Access::bindOpener(app, [real](const Recorder::OpenRequest& req, Recorder::OpenedFile& out,
                                   std::string& error) -> bool {
        std::this_thread::sleep_for(std::chrono::milliseconds(700));
        return real(req, out, error);
    });

    FrameTimer& t = frameTimer();
    t.resetForTest();
    g_lines.clear();
    t.setSinkForTest(&captureSink);

    const auto wall0 = std::chrono::steady_clock::now();
    auto elapsedMs = [&] {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - wall0)
            .count();
    };
    long frame = 100;
    // The Record press: the window's thread, in a frame, in the rail.
    t.beginFrame(frame++);
    {
        FrameScopeGuard rail(FrameScope::Rail);
        CHECK(Access::startAudio(app));
    }
    t.endFrame();
    // And the frames that follow, each polling for the file, until it arrives.
    while (!Access::audioRecording(app) && elapsedMs() < 8000.0) {
        t.beginFrame(frame++);
        {
            FrameScopeGuard polls(FrameScope::Polls);
            Access::poll(app, elapsedMs() / 1000.0);
        }
        t.endFrame();
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
    }
    CHECK(Access::audioRecording(app));  // it did start, late
    CHECK(elapsedMs() >= 690.0);         // because the disk was slow
    t.beginFrame(frame++);
    Access::stopAudio(app);
    t.endFrame();
    t.finish();
    // ...and not one frame was: the wait was a worker's, and the recorder scope
    // saw a press, a poll and a stop.
    std::printf("    %llu frames while the disk took 700 ms, longest %.1f ms (in %s); table: %s\n",
                static_cast<unsigned long long>(t.framesTimed()),
                static_cast<double>(t.longestFrameNs()) / 1e6, frameScopeName(t.longestFrameScope()),
                slowFramesText(slowFrameCounts()).c_str());
    CHECK(t.framesTimed() > 20);
    CHECK(t.longestFrameNs() < 250'000'000LL);
    CHECK(!slowFrameCounts().any());
    CHECK(g_lines.empty());

    // THE CONTROL, which is what the table would have said had the open run on the
    // window's thread as Recorder::start did before 0.99.63: the same wait inside
    // the recorder scope is a slow frame, and `recorder` is its name.
    t.beginFrame(frame++);
    {
        FrameScopeGuard inlineOpen(FrameScope::Recorder);
        std::this_thread::sleep_for(std::chrono::milliseconds(700));
    }
    t.endFrame();
    t.finish();
    CHECK(slowFrameCounts().count[static_cast<int>(FrameScope::Recorder)][0] == 1);
    t.resetForTest();
}

void testTheBundleCarriesTheTable() {
    std::printf("the bundle's slow-frames line, from the real window\n");
    cascade::gui::AppWindow app;
    FrameTimer& t = frameTimer();
    t.resetForTest();

    const std::string none = Access::bundle(app);
    const std::size_t logAt = none.find("\n--- log ---\n");
    const std::size_t lineAt = none.find("\nslow-frames: none\n");
    CHECK(logAt != std::string::npos && lineAt != std::string::npos && lineAt < logAt);

    g_lines.clear();
    t.setSinkForTest(&captureSink);
    t.beginFrame(100);
    { FrameScopeGuard g(FrameScope::Recorder); std::this_thread::sleep_for(std::chrono::milliseconds(300)); }
    t.endFrame();
    t.beginFrame(101);
    { FrameScopeGuard g(FrameScope::PluginsReload); std::this_thread::sleep_for(std::chrono::milliseconds(1100)); }
    t.endFrame();
    t.finish();
    const std::string some = Access::bundle(app);
    std::printf("    %s\n", findLine(some, "slow-frames: ").c_str());
    CHECK(some.find("\nslow-frames: recorder 1/0/0, plugins-reload 0/1/0\n") != std::string::npos);
    t.resetForTest();
}

#if defined(_WIN32)
// The window procedure's side of the modal-loop exclusion: the messages the
// operating system sends from INSIDE its move-and-size loop and its menu loop are
// counted, and a message that is not one of them is not. The frame loop reads this
// counter around the pump (the situation hook above plays its reading).
void testTheWindowProcedureCountsModalLoops() {
    std::printf("the window procedure counts the operating system's modal loops\n");
    if (!glfwInit()) {
        std::printf("  (no window system here: skipped)\n");
        return;
    }
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    GLFWwindow* w = glfwCreateWindow(200, 200, "slow-frames", nullptr, nullptr);
    if (w == nullptr) {
        glfwTerminate();
        std::printf("  (no window could be made here: skipped)\n");
        return;
    }
    CHECK(cascade::gui::frame::install(w));
    HWND hwnd = static_cast<HWND>(cascade::gui::frame::nativeHandle(w));
    CHECK(hwnd != nullptr);
    const unsigned before = cascade::gui::frame::modalLoopCount();
    ::SendMessageW(hwnd, WM_ENTERSIZEMOVE, 0, 0);
    CHECK(cascade::gui::frame::modalLoopCount() == before + 1);
    ::SendMessageW(hwnd, WM_ENTERMENULOOP, 0, 0);
    CHECK(cascade::gui::frame::modalLoopCount() == before + 2);
    // Leaving one, and anything else, is not entering one.
    ::SendMessageW(hwnd, WM_EXITSIZEMOVE, 0, 0);
    ::SendMessageW(hwnd, WM_EXITMENULOOP, 0, 0);
    ::SendMessageW(hwnd, WM_ACTIVATE, 0, 0);
    CHECK(cascade::gui::frame::modalLoopCount() == before + 2);
    glfwDestroyWindow(w);
    glfwTerminate();
}
#endif

}  // namespace

int main(int argc, char** argv) {
    std::printf("test_slow_frames_app\n");
    if (argc < 2) {
        std::printf("usage: test_slow_frames_app <probe module>\n");
        return 2;
    }
    isolate(argv[1]);
    Access::installHooks();

    // The real window first - it needs the cleanest machine it can get.
    testACleanRun();
    testAScriptsSleepIsFrameStartAndIsRateLimited();
    testEveryScopeIsEnteredWhereTheFrameSaysItIs();
    testThePatchPage();
    testWhatMustNotCount();

    // Then the real code behind two of the scopes, in this process.
    testAPluginReloadUnderItsPauseCounts();
    testAShellCallIsAPersonsTimeNotAFrames();
    testASlowDiskIsNotASlowFrame();
    testTheBundleCarriesTheTable();
#if defined(_WIN32)
    testTheWindowProcedureCountsModalLoops();
#endif

    std::error_code ec;
    fs::remove_all(g_dir, ec);
    return testSummary("test_slow_frames_app");
}
