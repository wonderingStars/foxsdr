// THE CLASSIC WINDOW'S DIAGNOSTICS CLICK RUNS THE RULE R EDGE (0.99.115, brief 80, limit 2 of lane 79).
//
// The Classic window's checkbox reaches the engine's Diagnostics edge (Engine::diagnosticsFamilyEdge: the health ledger's gate, what the family kept, the unsent reports withheld)
// only through the interface's half: the click calls AppWindow::applyDiagnosticsEnabled, the frame loop's sendInterfaceHalf hands the half to the engine, the engine's
// applyInterfaceConfig runs the edge. test_off_period_engine D8 drives the last step; this test holds the first two, against the real window and a real engine:
//
//   C1  the checkbox's click handler (source pin: the Checkbox in drawDiagnosticsSection calls applyDiagnosticsEnabled(on), and run()'s frame loop calls sendInterfaceHalf)
//   C2  applyDiagnosticsEnabled(false), then the frame's send, then the engine's pass: the edge ran (the ledger's gate closed, the trail emptied, the family's counts gone,
//       the unsent report on disk withheld as the person's own copy, the file kept); applyDiagnosticsEnabled(true) the same the other way (gate open, counts start again,
//       the withheld report still withheld)
//
// Break-it: the Checkbox without applyDiagnosticsEnabled -> C1 RED; the frame's send without the half's Diagnostics field (or applyInterfaceConfig without the edge) -> C2 RED.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>

#if defined(_WIN32)
#include <process.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "core/app_commands.hpp"
#include "core/config.hpp"
#include "core/crash_upload.hpp"
#include "core/diag_log.hpp"
#include "core/event_trail.hpp"
#include "core/health_events.hpp"
#include "engine/engine.hpp"
#include "gui/app_window.hpp"
#include "test_host.hpp"
#include "pass_seam.hpp"
#include "imgui.h"
#include "test_check.hpp"

namespace fs = std::filesystem;

namespace cascade::gui {
struct AppWindowTestAccess {
    static void click(AppWindow& a, bool on) { a.applyDiagnosticsEnabled(on); }   // what the Checkbox's `if` runs
    static bool send(AppWindow& a, double nowS) { return a.sendInterfaceHalf(nowS); }
    static void pump(AppWindow& a) { passseam::whole(th::engine(a), [&] { th::engine(a).pump(); }); }
    static void setControlDiagDir(AppWindow& a, const std::string& d) { passseam::on(th::engine(a), [&] { th::engine(a).controlDiagDir_ = d; }); }
    static bool healthAllowed(AppWindow& a) { return passseam::read<bool>(th::engine(a), [&] { return th::engine(a).healthLedger_->allowed(); }); }
    static std::string healthEncoded(AppWindow& a) { return passseam::read<std::string>(th::engine(a), [&] { return th::engine(a).healthLedger_->encoded(); }); }
};
}  // namespace cascade::gui

using A = cascade::gui::AppWindowTestAccess;

namespace {

void setEnv(const char* n, const std::string& v) {
#if defined(_WIN32)
    ::SetEnvironmentVariableA(n, v.c_str());
    _putenv_s(n, v.c_str());
#else
    ::setenv(n, v.c_str(), 1);
#endif
}

std::string readText(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

std::string statusOf(const fs::path& report) {
    const std::string text = readText(cascade::core::uploadSidecarPath(report.string()));
    const std::size_t at = text.find("status: ");
    if (at == std::string::npos) { return std::string(); }
    const std::size_t end = text.find('\n', at);
    return text.substr(at + 8, end == std::string::npos ? std::string::npos : end - at - 8);
}

bool waitStatus(const fs::path& report, const char* want, int ms = 4000) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < until) {
        if (statusOf(report) == want) { return true; }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return statusOf(report) == want;
}

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_classic_diag_edge\n");
#if defined(_WIN32)
    const int pid = _getpid();
#else
    const int pid = static_cast<int>(getpid());
#endif
    const fs::path scratch = fs::temp_directory_path() / ("foxsdr_classic_diag_edge_" + std::to_string(pid));
    std::error_code ec;
    fs::remove_all(scratch, ec);
    fs::create_directories(scratch);
    for (const char* v : {"LOCALAPPDATA", "APPDATA", "USERPROFILE", "HOME", "XDG_CONFIG_HOME", "XDG_STATE_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME"}) { setEnv(v, scratch.string()); }
    setEnv("CASCADE_CONFIG_TEST", "1");
    setEnv("FOXSDR_TELEMETRY_URL", "http://127.0.0.1:9");
    setEnv("FOXSDR_NO_OUTSIDE_REPORTS", "1");
    setEnv("FOXSDR_DIAG_DIR", (scratch / "diag").string());
    ImGui::CreateContext();

    // --- C1: the click handler ----------------------------------------------------------------------------------------------------------------------
    {
        std::printf("  C1  the Classic checkbox calls applyDiagnosticsEnabled, and the frame loop sends the half\n");
        const fs::path root = (argc > 1) ? fs::path(argv[1]) : fs::path(CASCADE_SOURCE_DIR);
        const std::string text = readText(root / "src" / "gui" / "app_window.cpp");
        CHECK(!text.empty());
        const std::size_t box = text.find("ImGui::Checkbox(trId(\"Record crashes and freezes, and send the report next time\"), &on)");
        CHECK(box != std::string::npos);
        const std::size_t call = box == std::string::npos ? std::string::npos : text.find("applyDiagnosticsEnabled(on);", box);
        CHECK(call != std::string::npos && call - box < 200);     // the very next statement of the click's block
        const std::size_t runAt = text.find("int AppWindow::run(int frames) {");
        CHECK(runAt != std::string::npos && text.find("(void)sendInterfaceHalf(glfwGetTime());", runAt) != std::string::npos);
    }

    // --- C3: TWO RUNNING COPIES (brief 83, review 4 D1 and D5): the Classic window's side, held as source pins (a window is not run here) ---------------------------------
    {
        std::printf("  C3  the Classic window follows what the engine adopted from the file, keeps both switches off in a copy without the lock, and sends the emptied tester queue at once\n");
        const fs::path root = (argc > 1) ? fs::path(argv[1]) : fs::path(CASCADE_SOURCE_DIR);
        const std::string text = readText(root / "src" / "gui" / "app_window.cpp");
        const std::string tester = readText(root / "src" / "gui" / "app_window_tester.cpp");
        const std::string updates = readText(root / "src" / "gui" / "app_window_updates.cpp");
        CHECK(!text.empty() && !tester.empty() && !updates.empty());
        const std::size_t runAt = text.find("int AppWindow::run(int frames) {");
        const std::size_t followCall = text.find("followConsent();", runAt);
        const std::size_t sendCall = text.find("(void)sendInterfaceHalf(glfwGetTime());", runAt);
        CHECK(runAt != std::string::npos && followCall != std::string::npos && sendCall != std::string::npos && followCall < sendCall && sendCall - followCall < 400);   // followed before the half is sent
        const std::size_t def = text.find("void AppWindow::followConsent() {");
        CHECK(def != std::string::npos);
        const std::size_t defEnd = def == std::string::npos ? std::string::npos : text.find("bool AppWindow::sendInterfaceHalf", def);
        const std::string body = defEnd == std::string::npos ? std::string() : text.substr(def, defEnd - def);
        CHECK(body.find(R"(readFactText("fact.consent.generation", generation))") != std::string::npos);
        CHECK(body.find("applyDiagnosticsEnabled(parsed.diagnosticsEnabled)") != std::string::npos);
        CHECK(body.find("updateCheckEnabled_ = parsed.updateCheckEnabled") != std::string::npos);
        CHECK(text.find(R"(readFactText("fact.consent.holder", holder) && holder == "0")") != std::string::npos);   // the copy without the lock is known at the start
        const std::size_t apply = text.find("void AppWindow::applyDiagnosticsEnabled(bool on) {");
        const std::size_t applyEnd = apply == std::string::npos ? std::string::npos : text.find("void AppWindow::drawDiagnosticsSection() {", apply);
        const std::string applyBody = applyEnd == std::string::npos ? std::string() : text.substr(apply, applyEnd - apply);
        CHECK(applyBody.find("const bool eff = on && !consentSecond_;") != std::string::npos);
        CHECK(applyBody.find("setCrashCaptureEnabled(eff, diagnosticsMinidump_)") != std::string::npos);
        CHECK(applyBody.find("sentinelSetEnabled(eff)") != std::string::npos);
        CHECK(applyBody.find("if (!eff) {") != std::string::npos);                       // the sweep follows what is armed, not what is shown
        CHECK(text.find("if (!diagnosticsEnabled_ || consentSecond_ || diagCrashDir_.empty()) { return true; }") != std::string::npos);   // crashUploadStart
        CHECK(updates.find("if (consentSecond_) { return; }") != std::string::npos);     // startUpdateCheck: no check of its own in a copy without the lock
        // D5: the emptied tester queue goes to the engine at once, both edges, and the send honours the flag
        std::size_t at = 0;
        int forced = 0;
        while ((at = tester.find("interfaceHalfForce_ = true;", at)) != std::string::npos) { ++forced; at += 10; }
        CHECK(forced == 2);
        CHECK(text.find("&& !interfaceHalfForce_) { return false; }") != std::string::npos);
    }

    // --- C2: the window's two calls reach the engine's edge -------------------------------------------------------------------------------------------
    {
        std::printf("  C2  the click, the frame's send and the engine's pass run the Diagnostics edge (both directions)\n");
        const std::string path = (scratch / "config.json").string();
        {
            cascade::core::AppConfig seed;
            seed.diagnosticsEnabled = true;
            std::string err;
            CHECK(cascade::core::ConfigStore::save(path, seed, err));
        }
        cascade::core::health::globalLedger()->reset();
        cascade::core::trail::resetForTest();
        cascade::core::DiagLog::instance().resetForTest();
        th::TestWindow app(path);
        const fs::path crashDir = scratch / "reports";
        fs::create_directories(crashDir, ec);
        A::setControlDiagDir(app, crashDir.string());
        const fs::path rep = crashDir / "crash-20261124-101010-77-1.txt";
        { std::ofstream f(rep, std::ios::binary | std::ios::trunc); f << "FoxSDR report body\nkind: crash\n"; }
        A::pump(app);
        CHECK(A::healthAllowed(app));
        cascade::core::health::noteRadioFail("rtlsdr", "the radio is already in use by another program");
        cascade::core::trail::noteMode(2);
        CHECK(!A::healthEncoded(app).empty());
        CHECK(!cascade::core::trail::formatNow().empty());

        A::click(app, false);                                      // the checkbox's click
        CHECK(A::send(app, 100.0));                                // the frame's send (the Diagnostics switch is urgent)
        A::pump(app);                                              // the engine's pass applies the half
        CHECK(!A::healthAllowed(app));                             // (delete the edge, or the half's field, and the gate stays open)
        CHECK(A::healthEncoded(app).empty());
        CHECK(cascade::core::trail::formatNow().empty());
        CHECK(waitStatus(rep, "local-only"));
        CHECK(fs::exists(rep));

        cascade::core::health::noteRadioFail("rtlsdr", "the radio is already in use by another program");   // an OFF-period failure: not counted
        CHECK(A::healthEncoded(app).empty());

        A::click(app, true);
        CHECK(A::send(app, 101.0));
        A::pump(app);
        CHECK(A::healthAllowed(app));
        CHECK(A::healthEncoded(app).empty());                      // starts from empty
        cascade::core::health::noteRadioFail("rtlsdr", "the radio is already in use by another program");
        CHECK(!A::healthEncoded(app).empty());
        CHECK(statusOf(rep) == "local-only");                      // still the person's own copy
        fs::remove(rep, ec);
        fs::remove(cascade::core::uploadSidecarPath(rep.string()), ec);
    }

    fs::remove_all(scratch, ec);
    return testSummary("test_classic_diag_edge");
}
