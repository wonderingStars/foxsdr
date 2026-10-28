/*
 * THE SDRPLAY API SERVICE CARD AND ITS RESTART KEY, IN THE REAL WINDOW (0.99.55, stub campaign B18, item P2 / ledger SDRPLAY-SERVICE).
 *
 * An RSP is driven through sdrplay_api.dll, and the DLL talks to a Windows SERVICE that holds the radio. When the service is stopped or wedged every Open
 * answers "fail" in no time at all, and the owner behind 0.99.55 (an RSP2 Pro, seven launches, never found the service) was told only to "check that the
 * SDRplay API service is running". Since then the Source section carries what Windows says the service is doing (source/sdrplay_service.hpp), and - while
 * the driver has trouble reaching it - a RESTART SDRPLAY SERVICE key that restarts it through one Windows administrator prompt, and the sentence for what
 * the restart came to.
 *
 * The ledger split the item: the engine counts the trouble (Engine::pollSdrPlayTrouble, test_health_app) and the pure parts have tests (test_sdrplay_service,
 * test_sdrplay_source), but the CARD - which is the window's - had no test, and the window the merge produced drew none of it: a stopped service read as a
 * radio that was simply not there. This DRAWS the real Source section (AppWindow::drawSourceSection, in a bare ImGui window of this test's own) over the real
 * engine and reads what it drew from the UI census (gui/ui_census.hpp). The service is the process's own seams: what Windows says (setSdrPlayServiceQueryForTest),
 * the driver's record of why its last call failed (the SDRplay API table's atomic serviceTrouble, which is where the engine's own counting reads it), and the
 * launcher the elevated command goes through (SdrPlayServiceRestart::setLauncherForTest) - so no service on this desk is touched, and nothing is elevated.
 *
 *   no trouble     the service is stopped and the driver has no complaint: no key, no note
 *   no service     the driver has trouble and Windows knows of no such service: no key (a restart must name one)
 *   shown          trouble and a service: the key, enabled, naming the service Windows found; no sentence about a restart that has not happened
 *   busy           pressed: disabled and "Restarting ... answer the Windows prompt." while the launcher runs; a second press starts no second restart
 *   outcomes       cancelled at the prompt; Windows would not run it (the error); the 45 s limit; a command that failed (its exit code); the service back and
 *                  the session lost (restart FoxSDR); the service back and the session clear (the radios are listed again; the key goes, the trouble being cleared)
 *   gone           no Windows service at all (a machine without SDRplay): never a key
 *
 * Windows only: off Windows the restart key is not drawn (sdrPlayRestartKeyShown), which the last section asserts.
 *
 * Hermetic: scratch per-user directories with the process id in the name, no radio, no network, and not one call into the real Service Control Manager.
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
#include <map>
#include <mutex>
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
#include "source/sdrplay_service.hpp"
#include "source/sdrplay_source.hpp"
#include "test_check.hpp"

namespace fs = std::filesystem;
namespace src = cascade::source;

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

// WHAT WINDOWS SAYS THE SERVICE IS DOING, as the test sets it. Read by the real query seam, on whatever thread asks (the restart's worker asks too).
std::mutex g_serviceMutex;
src::SdrPlayServiceStatus g_service;
src::SdrPlayServiceStatus serviceNow() {
    std::lock_guard<std::mutex> lk(g_serviceMutex);
    return g_service;
}
void setService(src::SdrPlayServiceState state, const char* name = "SDRplayAPIService") {
    std::lock_guard<std::mutex> lk(g_serviceMutex);
    g_service = src::SdrPlayServiceStatus{};
    g_service.state = state;
    g_service.startType = src::SdrPlayServiceStart::Auto;
    g_service.serviceName = (state == src::SdrPlayServiceState::NotInstalled || state == src::SdrPlayServiceState::NotApplicable) ? "" : name;
}

// THE LAUNCHER: what the elevated command comes to, and how long it takes. `calls` counts the restarts that reached it; `gate` holds one open until the test
// lets it go (bounded, so a failed test cannot hang the suite).
std::mutex g_launchMutex;
src::SdrPlayRestartOutcome g_outcome;
std::atomic<int> g_calls{0};
std::atomic<bool> g_gate{true};
std::string g_lastName;

src::SdrPlayRestartOutcome launcher(const std::string& serviceName, std::chrono::milliseconds, const std::atomic<bool>&) {
    {
        std::lock_guard<std::mutex> lk(g_launchMutex);
        g_lastName = serviceName;
    }
    ++g_calls;
    for (int i = 0; i < 3000 && !g_gate.load(); ++i) { std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
    std::lock_guard<std::mutex> lk(g_launchMutex);
    return g_outcome;
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

    static void installHooks() {
        Engine::testHooks_.makeDevice = &engine_api_test::makeFakeRadio;
        Engine::testHooks_.nativeScan = &engine_api_test::fakeNativeScan;
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
    }
    static void draw(AppWindow& a) {
        ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
        ImGui::SetNextWindowSize(ImVec2(520.0f, 1400.0f));
        ImGui::Begin("##source_test", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize);
        a.drawSourceSection();
        ImGui::End();
    }
    // The once-a-frame poll the frame's Polls scope makes (drawUi), when the window has one. (Absent before stub campaign B18: the first RED run of this test
    // is against a window that has neither the poll nor the card.)
    template <class W>
    static void poll(W& a) {
        if constexpr (requires { a.pollSdrPlayService(); }) { a.pollSdrPlayService(); }
    }
    template <class W>
    static bool setLauncher(W& a) {
        if constexpr (requires { a.sdrPlayRestart_.setLauncherForTest(&launcher); }) {
            a.sdrPlayRestart_.setLauncherForTest(&launcher);
            return true;
        } else {
            return false;
        }
    }
    static void tap(AppWindow& a, std::vector<cascade::core::cmd::QueuedCommand>* log) { a.uiSubmitTap_ = log; }
};

}  // namespace cascade::gui

using Access = cascade::gui::AppWindowTestAccess;
using cascade::gui::AppWindow;

namespace {

Seen frame(AppWindow& app) {
    Access::poll(app);
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

void click(AppWindow& app, const Rect& at) {
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(at.cx(), at.cy());
    (void)frame(app);
    io.AddMouseButtonEvent(0, true);
    (void)frame(app);
    io.AddMouseButtonEvent(0, false);
    (void)frame(app);
}

// Frames until `what` holds of what the section drew (the restart is a worker's, and the window reads its answer on the poll).
Seen until(AppWindow& app, const std::function<bool(const Seen&)>& what, int ms = 10000) {
    Seen s;
    for (int i = 0; i < ms / 5; ++i) {
        s = frame(app);
        if (what(s)) { return s; }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return s;
}

void trouble(src::SdrPlayServiceTrouble t) { src::processSdrPlayApi().serviceTrouble.store(static_cast<int>(t), std::memory_order_release); }

// THE SESSION THE DRIVER KEEPS: lost (a worker of ours parked in the vendor DLL for ever) or clear. The table is the process's own and no RSP was ever opened
// through it here, so "clear" is what it holds already.
void sessionLost(bool lost) {
    const src::sdrplay_abi::Api& api = src::processSdrPlayApi();
    std::lock_guard<std::mutex> lk(api.sessionMutex);
    api.sessionLost = lost;
}

// One press on the key as it is drawn now.
bool press(AppWindow& app, Seen& s) {
    const Rect* key = s.rect("sdrplay:service-key");
    if (key == nullptr) { return false; }
    click(app, *key);
    s = frame(app);
    return true;
}

// A restart that comes to `outcome` with the service in `after`, pressed and waited for: what the section drew once the window had read its answer.
Seen restartTo(AppWindow& app, src::SdrPlayRestartPhase phase, unsigned long exitCode, unsigned long win32, src::SdrPlayServiceState after) {
    {
        std::lock_guard<std::mutex> lk(g_launchMutex);
        g_outcome = src::SdrPlayRestartOutcome{};
        g_outcome.phase = phase;
        g_outcome.exitCode = exitCode;
        g_outcome.win32Error = win32;
    }
    // Until the command has run the service is stopped and the driver cannot reach it.
    trouble(src::SdrPlayServiceTrouble::OpenFailed);
    setService(src::SdrPlayServiceState::Stopped);
    Seen s = settle(app);
    CHECK(s.has("sdrplay:service-key"));
    g_gate = false;
    const int callsBefore = g_calls.load();
    CHECK(press(app, s));
    // The worker is out and holding: the section says so.
    for (int i = 0; i < 400 && g_calls.load() == callsBefore; ++i) { std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
    s = settle(app);
    CHECK(s.has("sdrplay:service-busy"));
    // What Windows says once the command has run: the restart's worker asks for it on the way out.
    setService(after);
    g_gate = true;
    (void)until(app, [](const Seen& x) { return !x.has("sdrplay:service-busy"); });
    // The worker publishes its outcome and then its phase, and the window reads the outcome on the poll at the head of a frame: the frame that first sees the phase move may have polled just
    // before it did. Two more frames and the outcome has certainly been read.
    return settle(app, 2);
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_sdrplay_service_app\n");
#if defined(_WIN32)
    const int pid = _getpid();
#else
    const int pid = static_cast<int>(getpid());
#endif
    g_scratch = fs::temp_directory_path() / ("foxsdr_sdrplay_service_app_" + std::to_string(pid));
    std::error_code ec;
    fs::remove_all(g_scratch, ec);
    fs::create_directories(g_scratch, ec);
    for (const char* v : {"LOCALAPPDATA", "APPDATA", "USERPROFILE", "HOME", "XDG_CONFIG_HOME", "XDG_STATE_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME"}) {
        setEnv(v, g_scratch.string());
    }
    for (const char* url : {"FOXSDR_TELEMETRY_URL", "FOXSDR_CRASH_URL", "FOXSDR_UPDATE_URL"}) { setEnv(url, "http://127.0.0.1:9"); }
    g_census = g_scratch / "census.txt";
    setEnv("FOXSDR_UI_CENSUS", g_census.string());
    setEnv("FOXSDR_OPEN_SECTIONS", "Source");

    // WHAT WINDOWS SAYS: the test's own answer, for every caller in the process (the window's poll, the restart's worker, the engine's).
    setService(src::SdrPlayServiceState::Stopped);
    src::setSdrPlayServiceQueryForTest(&serviceNow);
    src::sdrPlayServiceResetNoteForTest();
    trouble(src::SdrPlayServiceTrouble::None);
    sessionLost(false);

    Access::installHooks();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1000.0f, 1500.0f);
    io.IniFilename = nullptr;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    CHECK(cascade::gui::fonts::load());

#if !defined(_WIN32)
    SKIP_LINUX("the SDRplay API Service is a Windows service: off Windows the restart key is never drawn");
#endif

    {
        th::TestWindow app;
        const bool launcherSet = Access::setLauncher(app);
        CHECK(launcherSet);
        std::vector<cascade::core::cmd::QueuedCommand> sent;
        Access::tap(app, &sent);

        // --- no trouble -----------------------------------------------------------------------------------------------------------------------------
        std::printf("  no trouble\n");
        Seen s = settle(app, 4);
        CHECK(s.has("section:Source"));  // the section is drawn at all
        CHECK(!s.has("sdrplay:service-key"));
        CHECK(!s.anyWith("sdrplay:service-note:"));
        CHECK(!s.has("sdrplay:service-busy"));

        // --- no service -----------------------------------------------------------------------------------------------------------------------------
        std::printf("  no service: trouble, and Windows knows of none\n");
        setService(src::SdrPlayServiceState::NotInstalled);
        trouble(src::SdrPlayServiceTrouble::OpenFailed);
        s = settle(app, 4);
#if defined(_WIN32)
        CHECK(!s.has("sdrplay:service-key"));
#endif

        // --- shown ----------------------------------------------------------------------------------------------------------------------------------
        std::printf("  shown\n");
        // The service is asked after once per CHANGE of the driver's trouble (and when a restart ends), never per frame: the trouble clears when an Open succeeds, and the next
        // one asks again. So the service is mended while the driver has no complaint, and the complaint comes after.
        trouble(src::SdrPlayServiceTrouble::None);
        (void)settle(app, 2);
        setService(src::SdrPlayServiceState::Stopped);
        trouble(src::SdrPlayServiceTrouble::OpenFailed);
        s = settle(app, 4);
#if defined(_WIN32)
        CHECK(s.has("sdrplay:service-key"));
        CHECK(s.has("sdrplay:service-key:enabled"));
        CHECK(!s.has("sdrplay:service-key:disabled"));
        CHECK(s.rect("sdrplay:service-key") != nullptr);
        // NO SENTENCE ABOUT A RESTART THAT HAS NOT HAPPENED.
        CHECK(!s.anyWith("sdrplay:service-note:"));
        CHECK(!s.has("sdrplay:service-busy"));

        // --- busy -----------------------------------------------------------------------------------------------------------------------------------
        std::printf("  busy: one restart at a time\n");
        {
            std::lock_guard<std::mutex> lk(g_launchMutex);
            g_outcome = src::SdrPlayRestartOutcome{};
            g_outcome.phase = src::SdrPlayRestartPhase::Cancelled;
        }
        g_gate = false;
        g_calls = 0;
        CHECK(press(app, s));
        for (int i = 0; i < 400 && g_calls.load() == 0; ++i) { std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
        s = settle(app);
        CHECK(g_calls.load() == 1);
        CHECK(s.has("sdrplay:service-busy"));
        CHECK(s.has("sdrplay:service-key:disabled"));
        CHECK(!s.has("sdrplay:service-key:enabled"));
        {
            std::lock_guard<std::mutex> lk(g_launchMutex);
            CHECK(g_lastName == "SDRplayAPIService");  // the name Windows reported is the one the restart is given
        }
        // A press on the disabled key starts no second restart.
        CHECK(press(app, s));
        s = settle(app);
        CHECK(g_calls.load() == 1);
        g_gate = true;
        s = until(app, [](const Seen& x) { return !x.has("sdrplay:service-busy"); });

        // --- outcomes -------------------------------------------------------------------------------------------------------------------------------
        std::printf("  outcomes\n");
        // Cancelled at the Windows prompt: said, and nothing else done.
        s = restartTo(app, src::SdrPlayRestartPhase::Cancelled, 0, 0, src::SdrPlayServiceState::Stopped);
        CHECK(s.firstWith("sdrplay:service-note:") == "Restart cancelled.");
        // Windows would not run it.
        s = restartTo(app, src::SdrPlayRestartPhase::NotStarted, 0, 5, src::SdrPlayServiceState::Stopped);
        CHECK(s.firstWith("sdrplay:service-note:") == "Windows would not run the restart (error 5).");
        // The 45 s limit, and where the service is left: its state in words after the sentence.
        s = restartTo(app, src::SdrPlayRestartPhase::TimedOut, 0, 0, src::SdrPlayServiceState::Stopped);
        {
            const std::string note = s.firstWith("sdrplay:service-note:");
            CHECK(note.rfind("The restart did not finish within 45 seconds.", 0) == 0);
            CHECK(note.find("The SDRplay API Service is stopped") != std::string::npos);
        }
        // A command that ran and failed.
        s = restartTo(app, src::SdrPlayRestartPhase::Failed, 3, 0, src::SdrPlayServiceState::Stopped);
        {
            const std::string note = s.firstWith("sdrplay:service-note:");
            CHECK(note.rfind("The restart ended with exit code 3.", 0) == 0);
            CHECK(note.find("The SDRplay API Service is stopped") != std::string::npos);
        }
        // The service is back and the driver's session is lost for good: FoxSDR has to be restarted, and the sentence says so.
        sessionLost(true);
        s = restartTo(app, src::SdrPlayRestartPhase::Done, 0, 0, src::SdrPlayServiceState::Running);
        sessionLost(false);
        CHECK(s.firstWith("sdrplay:service-note:") ==
              "The SDRplay API Service was restarted and is running. Restart FoxSDR to use the SDRplay radio again.");
        // The service is back and nothing of ours is inside the vendor DLL: the radios are listed again, here and now, and the driver's trouble is cleared
        // (so the key, which exists for the trouble, goes).
        sent.clear();
        s = restartTo(app, src::SdrPlayRestartPhase::Done, 0, 0, src::SdrPlayServiceState::Running);
        bool scanned = false;
        for (const auto& q : sent) { scanned = scanned || q.c.op == FOXAPI_OP_SCAN_DEVICES; }
        CHECK(scanned);
        CHECK(src::sdrPlayServiceTrouble(src::processSdrPlayApi()) == src::SdrPlayServiceTrouble::None);
        s = settle(app, 3);
        CHECK(!s.has("sdrplay:service-key"));

        // --- gone -----------------------------------------------------------------------------------------------------------------------------------
        std::printf("  gone: no service at all\n");
        setService(src::SdrPlayServiceState::NotInstalled);
        trouble(src::SdrPlayServiceTrouble::OpenFailed);
        s = settle(app, 4);
        CHECK(!s.has("sdrplay:service-key"));
#endif
        trouble(src::SdrPlayServiceTrouble::None);
    }

    src::setSdrPlayServiceQueryForTest(nullptr);
    ImGui::DestroyContext();
    fs::remove_all(g_scratch, ec);
    return testSummary("test_sdrplay_service_app");
}
