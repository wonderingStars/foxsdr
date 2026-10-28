/*
 * THE SDRPLAY DIAGNOSTIC'S KEY AND DIALOG, IN THE REAL WINDOW (0.99.51, stub campaign B18, item U1).
 *
 * "Run SDRplay diagnostic" (SYSTEM > Diagnostics, or `cascade --sdrplay-probe <file>`) steps an RSP through every sample rate, band, LNA state and antenna once,
 * times every call to the SDRplay API and writes one text file to attach to a bug report (source/sdrplay_probe.hpp). The probe itself and the command line
 * are carried and tested (test_sdrplay_probe, test_process_boot_*); the WINDOW half - the key, the dialog that asks first, the child process and the status
 * line, and the row on the bug page that attaches the file - was never put back by the merge, and no placeholder said so: the ledger's 0.99.51 line says
 * "carried (source/)" and no test drew the key.
 *
 * This DRAWS the real window's Diagnostics section and the probe's dialog (AppWindow::drawDiagnosticsSection and drawSdrPlayProbeDialog, in bare ImGui windows
 * of this test's own) and reads what they drew from the UI census (gui/ui_census.hpp). The key's press, the dialog's keys and the bias tee tick are real ImGui
 * mouse events at the rectangles the census reported. THE CHILD PROCESS IS THE TEST'S OWN EXECUTABLE: SdrPlayProbeChild starts "this executable again with
 * --sdrplay-probe <file>", and when this executable is started so it plays the probe - it holds until the test lets it go (a file appears), writes a file that
 * says what it was asked (the bias tee switch) and counts itself in a log of its own - so how many probes were started, whether the bias tee reached the
 * child, and for how long one runs are the test's to decide, with no RSP, no service and no wait.
 *
 *   idle        the key and the sentence about what the diagnostic is, enabled; no dialog; nothing started
 *   dialog      the key opens the dialog: the warning about the radio, the bias tee tick, Run and Cancel; Cancel closes it and starts nothing
 *   bias        ticking the bias tee only ASKS - the separate question, with its own Yes and Cancel, and no Run until it is answered; Cancel leaves it off
 *   running     Run starts ONE child (bias tee off) and the status line says so; the key is disabled while it runs, and a press on it and a press where Run
 *               was start no second one; when it ends the status line names the file, the file is the child's, and the window holds its text (the bug page's
 *               "Attach the SDRplay diagnostic too" row draws from it)
 *   confirmed   the bias tee reaches the child only after the question was answered Yes
 *   no file     a child that wrote nothing is not "finished": the status line says it could not start, with its exit code
 *   radio open  FoxSDR holding an SDRplay radio disables the key and says why (the service gives a radio to one owner, and the probe must be it)
 *
 * Windows or POSIX: SdrPlayProbeChild starts the executable on both. Hermetic: scratch per-user directories with the process id in the name, no RSP, no
 * network, no real reports folder.
 *
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <set>
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

#include "core/app_commands.hpp"
#include "engine/engine.hpp"
#include "engine_api_fake_radio.hpp"
#include "gui/app_window.hpp"
#include "test_host.hpp"
#include "gui/fonts.hpp"
#include "gui/ui_census.hpp"
#include "imgui.h"
#include "pass_seam.hpp"
#include "source/sdrplay_probe.hpp"
#include "test_check.hpp"

namespace fs = std::filesystem;

namespace {

fs::path g_scratch;
fs::path g_census;

void setEnv(const char* name, const std::string& value) {
#if defined(_WIN32)
    _putenv_s(name, value.c_str());
    SetEnvironmentVariableA(name, value.c_str());
#else
    if (value.empty()) {
        unsetenv(name);
    } else {
        setenv(name, value.c_str(), 1);
    }
#endif
}

std::string readFile(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// --- THE CHILD: this executable, started as the diagnostic ------------------------------------------------------------------------------------------------
//
// argv: --sdrplay-probe <file> [--sdrplay-probe-bias-tee]. It logs that it started (one line, appended, to the file FOXSDR_TEST_PROBE_LOG names), waits for the
// file FOXSDR_TEST_PROBE_GATE names to exist (bounded), then - unless FOXSDR_TEST_PROBE_MODE is "nofile" - writes <file> and says in it what it was asked.
int childMain(int argc, char** argv) {
    const char* logPath = std::getenv("FOXSDR_TEST_PROBE_LOG");
    const char* gate = std::getenv("FOXSDR_TEST_PROBE_GATE");
    const char* mode = std::getenv("FOXSDR_TEST_PROBE_MODE");
    bool bias = false;
    for (int i = 3; i < argc; ++i) { bias = bias || std::strcmp(argv[i], "--sdrplay-probe-bias-tee") == 0; }
    if (logPath != nullptr) {
        std::ofstream log(logPath, std::ios::binary | std::ios::app);
        log << "started bias=" << (bias ? "on" : "off") << "\n";
    }
    if (gate != nullptr) {
        for (int i = 0; i < 6000 && !fs::exists(gate); ++i) { std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
    }
    if (mode != nullptr && std::strcmp(mode, "nofile") == 0) { return 7; }
    std::ofstream out(argv[2], std::ios::binary | std::ios::trunc);
    out << "FoxSDR SDRplay diagnostic (the test's stand-in)\nbias tee asked: " << (bias ? "on" : "off") << "\n";
    return out ? 0 : 2;
}

struct Rect {
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    float cx() const { return 0.5f * (x0 + x1); }
    float cy() const { return 0.5f * (y0 + y1); }
};

struct Seen {
    std::set<std::string> items;
    std::map<std::string, Rect> rects;
    bool has(const std::string& s) const { return items.count(s) != 0; }
    bool anyWith(const std::string& prefix) const {
        for (const std::string& s : items) {
            if (s.rfind(prefix, 0) == 0) { return true; }
        }
        return false;
    }
    std::string firstWith(const std::string& prefix) const {
        for (const std::string& s : items) {
            if (s.rfind(prefix, 0) == 0) { return s.substr(prefix.size()); }
        }
        return std::string();
    }
    const Rect* rect(const std::string& n) const {
        const auto it = rects.find(n);
        return it == rects.end() ? nullptr : &it->second;
    }
};

}  // namespace

namespace cascade::gui {

struct AppWindowTestAccess {
    using Engine = cascade::engine::Engine;

    // A scan whose one radio is an RSP (the driver key the diagnostic's "FoxSDR has the SDRplay radio open" rule reads), and a radio of the fake's own for it.
    static std::vector<cascade::source::NativeDeviceInfo> sdrplayScan() { return {{"sdrplay", "RSP1A (serial 1234567890)", "serial=1234567890"}}; }
    static void installHooks() {
        Engine::testHooks_.makeDevice = &engine_api_test::makeFakeRadio;
        Engine::testHooks_.nativeScan = &sdrplayScan;
        Engine::testHooks_.soapyScan = &engine_api_test::noSoapyRadios;
    }
    static void look(AppWindow& a) {
        passseam::pass(th::engine(a), [&] {
            th::engine(a).pumpFrameBegin();
            th::engine(a).pumpInputs();
            th::engine(a).pumpPlugins();
            (void)th::engine(a).pumpPublish();
        });
        a.statusLines_ = th::engine(a).statusText();
        a.refreshUiState();
        // ...and the workers' collect, the way a frame ends (a radio's open is on a worker and lands here): a custom subset of the pass's phases, so that no audio is opened.
        passseam::custom(th::engine(a), [&] { th::engine(a).pumpWorkers(); });
    }
    // The Diagnostics section, and the probe's own dialog as the frame's Dialogs scope draws it (drawUi). The dialog's absence from the window is what the
    // first RED run of this test is against: before stub campaign B18 there is neither the key nor the dialog.
    template <class W>
    static void draw(W& a) {
        ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
        ImGui::SetNextWindowSize(ImVec2(460.0f, 1500.0f));
        ImGui::Begin("##diag_test", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize);
        a.drawDiagnosticsSection();
        ImGui::End();
        if constexpr (requires { a.drawSdrPlayProbeDialog(); }) { a.drawSdrPlayProbeDialog(); }
    }
    static std::string report(AppWindow& a) { return a.sdrplayProbeReport_; }
    static bool attachFlag(AppWindow& a) { return a.problemReportAttachProbe_; }
    static void markAttach(AppWindow& a) { a.problemReportAttachProbe_ = true; }
    // The Source combo's own click on the RSP's row: SELECT_SOURCE with the row's id as the table gives it whole (fact.source.id.<entry id>), after a scan.
    static void scan(AppWindow& a) { a.uiSubmit(cascade::core::cmd::make(FOXAPI_OP_SCAN_DEVICES)); }
    static bool selectRsp(AppWindow& a) {
        const AppWindow::SourceListCache& c = a.sourceLists();
        for (std::size_t i = static_cast<std::size_t>(AppWindow::kNativeRowBase); i < c.rows.size(); ++i) {
            if (c.rows[i].driver != "sdrplay") { continue; }
            char key[48];
            std::snprintf(key, sizeof(key), "fact.source.id.%llu", static_cast<unsigned long long>(c.rows[i].id));
            std::string id;
            a.readFactText(key, id);
            if (id.empty()) { return false; }
            a.uiSubmit(cascade::core::cmd::makeText(FOXAPI_OP_SELECT_SOURCE, id));
            return true;
        }
        return false;
    }
    static bool deviceOpen(AppWindow& a) { return (a.uiState_.flags & FOXAPI_RX_DEVICE_OPEN) != 0u; }
};

}  // namespace cascade::gui

using Access = cascade::gui::AppWindowTestAccess;
using cascade::gui::AppWindow;

namespace {

Seen frame(AppWindow& app) {
    Access::look(app);
    cascade::gui::census::clear();
    ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
    ImGui::NewFrame();
    Access::draw(app);
    ImGui::Render();
    Seen s;
    if (!cascade::gui::census::write()) { return s; }
    std::ifstream in(g_census);
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("item ", 0) == 0) { s.items.insert(line.substr(5)); }
        if (line.rfind("rect ", 0) != 0) { continue; }
        std::istringstream ss(line.substr(5));
        std::string name;
        Rect rc;
        ss >> name >> rc.x0 >> rc.y0 >> rc.x1 >> rc.y1;
        s.rects[name] = rc;
    }
    return s;
}

Seen settle(AppWindow& app, int frames = 3) {
    Seen s;
    for (int i = 0; i < frames; ++i) { s = frame(app); }
    return s;
}

// A press and release on the middle of the named rectangle of what was just drawn; false (and nothing pressed) when the census has no such rectangle.
bool click(AppWindow& app, Seen& s, const std::string& name) {
    const Rect* at = s.rect(name);
    if (at == nullptr) { return false; }
    const Rect r = *at;
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(r.cx(), r.cy());
    (void)frame(app);
    io.AddMouseButtonEvent(0, true);
    (void)frame(app);
    io.AddMouseButtonEvent(0, false);
    s = frame(app);
    return true;
}

Seen until(AppWindow& app, const std::function<bool(const Seen&)>& what, int ms = 15000) {
    Seen s;
    for (int i = 0; i < ms / 5; ++i) {
        s = frame(app);
        if (what(s)) { return s; }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return s;
}

fs::path g_log;   // the child's own log: one line a probe started
fs::path g_gate;  // the file whose existence lets a child go on
int startedProbes() {
    std::ifstream f(g_log, std::ios::binary);
    int n = 0;
    std::string line;
    while (std::getline(f, line)) { n += line.rfind("started", 0) == 0 ? 1 : 0; }
    return n;
}
std::string lastStartedLine() {
    std::ifstream f(g_log, std::ios::binary);
    std::string line, last;
    while (std::getline(f, line)) { last = line; }
    return last;
}
void holdChildren() {
    std::error_code ec;
    fs::remove(g_gate, ec);
}
void releaseChildren() {
    std::ofstream f(g_gate, std::ios::binary | std::ios::trunc);
    f << "go\n";
}

// The dialog open: the key pressed on what was drawn.
Seen openDialog(AppWindow& app) {
    Seen s = settle(app);
    CHECK(click(app, s, "diag:sdrplay-key"));
    s = settle(app);
    return s;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc >= 3 && std::strcmp(argv[1], "--sdrplay-probe") == 0) { return childMain(argc, argv); }
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_sdrplay_probe_app\n");
#if defined(_WIN32)
    const int pid = _getpid();
#else
    const int pid = static_cast<int>(getpid());
#endif
    g_scratch = fs::temp_directory_path() / ("foxsdr_sdrplay_probe_app_" + std::to_string(pid));
    std::error_code ec;
    fs::remove_all(g_scratch, ec);
    fs::create_directories(g_scratch, ec);
    for (const char* v : {"LOCALAPPDATA", "APPDATA", "USERPROFILE", "HOME", "XDG_CONFIG_HOME", "XDG_STATE_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME"}) {
        setEnv(v, g_scratch.string());
    }
    for (const char* url : {"FOXSDR_TELEMETRY_URL", "FOXSDR_CRASH_URL", "FOXSDR_UPDATE_URL"}) { setEnv(url, "http://127.0.0.1:9"); }
    // The reports folder (where the diagnostic's file goes) is this test's own.
    setEnv("FOXSDR_DIAG_DIR", (g_scratch / "diag").string());
    g_census = g_scratch / "census.txt";
    setEnv("FOXSDR_UI_CENSUS", g_census.string());
    setEnv("FOXSDR_OPEN_SECTIONS", "Diagnostics");
    g_log = g_scratch / "children.log";
    g_gate = g_scratch / "gate";
    setEnv("FOXSDR_TEST_PROBE_LOG", g_log.string());
    setEnv("FOXSDR_TEST_PROBE_GATE", g_gate.string());
    setEnv("FOXSDR_TEST_PROBE_MODE", "");
    holdChildren();

    Access::installHooks();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1000.0f, 1600.0f);
    io.IniFilename = nullptr;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    CHECK(cascade::gui::fonts::load());

    {
        th::TestWindow app;

        // --- idle -----------------------------------------------------------------------------------------------------------------------------------
        std::printf("  idle\n");
        Seen s = settle(app, 4);
        CHECK(s.has("section:Diagnostics"));
        CHECK(s.has("diag:sdrplay-text"));  // what the diagnostic is, for owners of an RSP
        CHECK(s.has("diag:sdrplay-key"));
        CHECK(s.has("diag:sdrplay-key:enabled"));
        CHECK(!s.has("diag:sdrplay-key:disabled"));
        CHECK(s.rect("diag:sdrplay-key") != nullptr);
        CHECK(!s.has("sdrplay:probe-dialog"));
        CHECK(!s.anyWith("diag:sdrplay-status:"));
        CHECK(startedProbes() == 0);

        // --- dialog ---------------------------------------------------------------------------------------------------------------------------------
        std::printf("  dialog\n");
        s = openDialog(app);
        CHECK(s.has("sdrplay:probe-dialog"));
        CHECK(s.has("sdrplay:probe-warning"));  // "This takes the SDRplay radio for a minute or two..."
        CHECK(s.rect("sdrplay:probe-run") != nullptr);
        CHECK(s.rect("sdrplay:probe-cancel") != nullptr);
        CHECK(s.rect("sdrplay:probe-bias") != nullptr);
        CHECK(!s.has("sdrplay:probe-bias-ask"));
        CHECK(!s.has("sdrplay:probe-bias:on"));
        CHECK(click(app, s, "sdrplay:probe-cancel"));
        s = settle(app);
        CHECK(!s.has("sdrplay:probe-dialog"));
        CHECK(startedProbes() == 0);  // Cancel starts nothing

        // --- bias -----------------------------------------------------------------------------------------------------------------------------------
        std::printf("  bias: the separate question\n");
        s = openDialog(app);
        CHECK(click(app, s, "sdrplay:probe-bias"));
        s = settle(app);
        CHECK(s.has("sdrplay:probe-bias-ask"));  // ticking only ASKS
        CHECK(!s.has("sdrplay:probe-bias:on"));
        CHECK(s.rect("sdrplay:probe-bias-yes") != nullptr);
        CHECK(s.rect("sdrplay:probe-cancel") != nullptr);
        CHECK(s.rect("sdrplay:probe-run") == nullptr);  // no Run until the question is answered
        CHECK(click(app, s, "sdrplay:probe-cancel"));   // the question's Cancel: the box stays unticked
        s = settle(app);
        CHECK(s.has("sdrplay:probe-dialog"));
        CHECK(!s.has("sdrplay:probe-bias-ask"));
        CHECK(!s.has("sdrplay:probe-bias:on"));
        CHECK(s.rect("sdrplay:probe-run") != nullptr);
        CHECK(startedProbes() == 0);

        // --- running --------------------------------------------------------------------------------------------------------------------------------
        std::printf("  running: one child, the bias tee off\n");
        holdChildren();
        CHECK(click(app, s, "sdrplay:probe-run"));
        s = settle(app);
        CHECK(!s.has("sdrplay:probe-dialog"));  // Run closes it
        CHECK(s.has("diag:sdrplay-status:SDRplay diagnostic running - this takes a minute or two..."));
        CHECK(s.has("diag:sdrplay-key:disabled"));
        CHECK(!s.has("diag:sdrplay-key:enabled"));
        for (int i = 0; i < 2000 && startedProbes() < 1; ++i) { std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
        CHECK(startedProbes() == 1);
        CHECK(lastStartedLine() == "started bias=off");
        // A PRESS ON THE DISABLED KEY opens no dialog, and so starts no second probe: the key is the only way to the dialog, and the dialog the only way to Run.
        CHECK(click(app, s, "diag:sdrplay-key"));
        s = settle(app);
        CHECK(!s.has("sdrplay:probe-dialog"));
        if (s.has("sdrplay:probe-dialog")) { (void)click(app, s, "sdrplay:probe-run"); }  // (what a key that was not disabled would have let the person do)
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        CHECK(startedProbes() == 1);
        // The child ends.
        releaseChildren();
        s = until(app, [](const Seen& x) { return x.anyWith("diag:sdrplay-status:SDRplay diagnostic finished: "); });
        CHECK(s.anyWith("diag:sdrplay-status:SDRplay diagnostic finished: "));
        CHECK(s.has("diag:sdrplay-key:enabled"));  // free to run another
        const std::string text = Access::report(app);
        CHECK(text.find("bias tee asked: off") != std::string::npos);
        CHECK(text.find("the test's stand-in") != std::string::npos);
        // ...and it is the file the status line names, in the reports folder.
        {
            const std::string named = s.firstWith("diag:sdrplay-status:SDRplay diagnostic finished: ");
            CHECK(!named.empty() && readFile(named) == text);
            CHECK(named.find("sdrplay-diagnostic-") != std::string::npos);
            CHECK(fs::path(named).string().rfind((g_scratch / "diag").string(), 0) == 0);
        }
        CHECK(!Access::attachFlag(app));  // the bug page's row starts unticked

        // --- confirmed ------------------------------------------------------------------------------------------------------------------------------
        std::printf("  confirmed: the bias tee reaches the child only after Yes\n");
        Access::markAttach(app);
        s = openDialog(app);
        CHECK(click(app, s, "sdrplay:probe-bias"));
        s = settle(app);
        CHECK(click(app, s, "sdrplay:probe-bias-yes"));
        s = settle(app);
        CHECK(s.has("sdrplay:probe-bias:on"));
        CHECK(!s.has("sdrplay:probe-bias-ask"));
        CHECK(startedProbes() == 1);
        CHECK(click(app, s, "sdrplay:probe-run"));
        s = until(app, [](const Seen& x) { return x.anyWith("diag:sdrplay-status:SDRplay diagnostic finished: "); });
        CHECK(startedProbes() == 2);
        CHECK(lastStartedLine() == "started bias=on");
        CHECK(Access::report(app).find("bias tee asked: on") != std::string::npos);
        // A new diagnostic's report replaces the last one's, and the page's tick is let go: it was for the other file.
        CHECK(!Access::attachFlag(app));

        // The NEXT one starts with the box unticked again (a confirmation answers one run, not every run after it).
        s = openDialog(app);
        CHECK(!s.has("sdrplay:probe-bias:on"));
        CHECK(click(app, s, "sdrplay:probe-cancel"));
        s = settle(app);

        // --- no file --------------------------------------------------------------------------------------------------------------------------------
        std::printf("  no file: a child that wrote nothing\n");
        // (The report's name is the second it was started in: a probe started in the same second as the last would find that one's file and read it. The window does not guard
        // against it - two diagnostics inside one second is not a thing a person does - so the test does not make one.)
        std::this_thread::sleep_for(std::chrono::milliseconds(1100));
        setEnv("FOXSDR_TEST_PROBE_MODE", "nofile");
        s = openDialog(app);
        CHECK(click(app, s, "sdrplay:probe-run"));
        s = until(app, [](const Seen& x) { return x.anyWith("diag:sdrplay-status:SDRplay diagnostic could not start: "); });
        CHECK(s.has("diag:sdrplay-status:SDRplay diagnostic could not start: no file was written (exit 7)"));
        CHECK(!s.anyWith("diag:sdrplay-status:SDRplay diagnostic finished: "));
        setEnv("FOXSDR_TEST_PROBE_MODE", "");
        CHECK(startedProbes() == 3);

        // --- radio open -----------------------------------------------------------------------------------------------------------------------------
        std::printf("  radio open: FoxSDR holds an SDRplay radio\n");
        Access::scan(app);
        (void)settle(app, 4);
        CHECK(Access::selectRsp(app));
        {
            bool open = false;
            for (int i = 0; i < 2000 && !open; ++i) {
                s = frame(app);
                open = Access::deviceOpen(app);
                if (!open) { std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
            }
            CHECK(open);
        }
        s = settle(app);
        CHECK(s.has("diag:sdrplay-key:disabled"));
        CHECK(s.has("diag:sdrplay-in-use"));  // "FoxSDR has the SDRplay radio open - choose another source first."
        CHECK(startedProbes() == 3);
    }

    ImGui::DestroyContext();
    fs::remove_all(g_scratch, ec);
    return testSummary("test_sdrplay_probe_app");
}
