// test_usage_diag_window.cpp - THE USAGE SWITCH AND THE DIAGNOSTICS BUNDLE READ THE WINDOW'S ENGINE API SESSION (window port,
// group 14, part c: drawUsageReportingSection and copyDiagnosticsBundle; API 0.3 section 8.13 - fact.telemetry.enabled,
// fact.telemetry.installId, fact.diag.launches and fact.diag.crashes, and the diagnostics context the engine renders itself).
//
// WHAT THEY DRAW FROM. The usage row's chip, its lamp, its install id and its checkbox come from two LOCAL facts (the switch and
// the id the engine mints at consent); the checkbox's click is FOXAPP_OP_TELEMETRY_CONSENT through the session, so it lands at
// the engine's next pass and the checkbox shows what was asked for until the fact says it (the id line and the chip follow a
// pass later). The bundle's context is the ENGINE's block - the very text a crash report carries, rendered by the engine's pass
// and taken as it is - its two counters are facts, and building it writes nothing process-global.
//
//   A  the usage row: off, the click, the checkbox ahead of the fact, the id when the fact arrives, off again (the id gone), and
//      a click the engine never answers (the checkbox gives the value back after half a second)
//   B  the bundle: the engine's context block whole, the counters from the facts, the last-run flag, the file under the crash
//      folder beside the clipboard; THE CRASH HANDLER'S COPY OF THE CONTEXT IS NOT TOUCHED (the old member replaced it with its
//      own re-derivation - proved red in tests/test_diagnostics.cpp), and the block follows the session within the pass's second
//   C  the members name no engine object
//
// THE ACCESSOR TRAP: a ported member reads the SESSION, so each frame here looks first (the engine's pass takes the session's
// queue, publishes, the window reads its state), draws, and lets the NEXT look take what the draw sent.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
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
#include "core/diag_log.hpp"
#include "core/diag_report.hpp"
#include "core/i18n.hpp"
#include "core/version.hpp"
#include "engine/engine.hpp"
#include "engine_api_fake_radio.hpp"
#include "gui/app_window.hpp"
#include "test_host.hpp"
#include "pass_seam.hpp"
#include "gui/fonts.hpp"
#include "imgui.h"
#include "imgui_internal.h"
#include "test_check.hpp"

#define CHECK_EQ(a, b) CHECK((a) == (b))

namespace cmd = cascade::core::cmd;
using cascade::gui::AppWindow;

namespace {
std::string g_pluginDir;
std::string pluginDir() { return g_pluginDir; }
}  // namespace

namespace cascade::gui {

struct AppWindowTestAccess {
    using Sent = std::vector<cmd::QueuedCommand>;
    using Engine = cascade::engine::Engine;

    static void install() {
        Engine::testHooks_.makeDevice = &engine_api_test::makeFakeRadio;
        Engine::testHooks_.nativeScan = &engine_api_test::fakeNativeScan;
        Engine::testHooks_.soapyScan = &engine_api_test::noSoapyRadios;
        Engine::testHooks_.pluginDir = &pluginDir;
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
    static void after(AppWindow& a) {
        passseam::workers(th::engine(a), [&] {
            th::engine(a).pumpWorkers();
            th::engine(a).pumpAudio();
        });
    }
    static void submit(AppWindow& a, const cmd::QueuedCommand& q) { a.uiSubmit(q); }
    static void submit(AppWindow& a, const FoxCommand& c) { a.uiSubmit(c); }
    static void tap(AppWindow& a, Sent* log) { a.uiSubmitTap_ = log; }

    static void drawUsage(AppWindow& a) { a.drawUsageReportingSection(); }
    static void copyBundle(AppWindow& a) { a.copyDiagnosticsBundle(); }
    static AppWindow::TelemetryPending& pending(AppWindow& a) { return a.telemetryPending_; }
    static const std::string& bundleStatus(AppWindow& a) { return a.diagBundleStatus_; }
    static void setUnclean(AppWindow& a, bool v) { a.lastRunUnclean_ = v; }
    static const std::string& fact(AppWindow& a, const char* key) { return a.uiFact(key); }
    // The engine's own fields, for what the engine says and the window must agree with. They are the pass's: each read and
    // write goes through passseam::read/on (on the pass's thread with the pass out of the way when it free-runs; right here
    // otherwise) - or, the "Held" form, from this thread while a PassHold parks the pass.
    static bool engineOnHeld(AppWindow& a) { return th::engine(a).telemetryEnabled_; }
    static bool engineOn(AppWindow& a) { return passseam::read<bool>(th::engine(a), [&] { return engineOnHeld(a); }); }
    static std::string engineId(AppWindow& a) {
        return passseam::read<std::string>(th::engine(a), [&] { return th::engine(a).telemetryInstallId_; });
    }
    static void setCounters(AppWindow& a, std::uint64_t launches, std::uint64_t crashes) {
        passseam::on(th::engine(a), [&] {
            th::engine(a).telemetryLaunches_ = launches;
            th::engine(a).telemetryCrashes_ = crashes;
        });
    }
    // THE PASS HELD, free-running only: a piece of work posted to the pass (Engine::postToPass) that waits until the hold is
    // let go. A pass runs posted work at the head of its front part AFTER it has taken its queues, so once the work has STARTED
    // the pass that runs it has taken everything it will take and no other pass can start until it ends: what the window sends
    // meanwhile is taken by the NEXT pass, after the release, and what the test reads meanwhile PRECEDES that pass. Inline and
    // stepped no pass runs unless the test asks for one, so there is nothing to hold. Nothing may ask for a pass (on/read/pass)
    // while it is held: the window's own session reads and submits, and the Held forms, only.
    class PassHold {
    public:
        explicit PassHold(AppWindow& a) {
            if (passseam::mode() != passseam::Mode::Free) { return; }
            passseam::attach(th::engine(a));
            s_ = std::make_shared<State>();
            std::shared_ptr<State> s = s_;
            th::engine(a).postToPass([s] {
                std::unique_lock<std::mutex> lk(s->m);
                s->running = true;
                s->cv.notify_all();
                s->cv.wait(lk, [&] { return s->release; });
            });
            std::unique_lock<std::mutex> lk(s_->m);
            const bool took = s_->cv.wait_for(lk, std::chrono::seconds(10), [&] { return s_->running; });
            CHECK(took);
        }
        ~PassHold() {
            if (s_ == nullptr) { return; }
            {
                std::lock_guard<std::mutex> lk(s_->m);
                s_->release = true;
            }
            s_->cv.notify_all();
        }
        PassHold(const PassHold&) = delete;
        PassHold& operator=(const PassHold&) = delete;

    private:
        struct State {
            std::mutex m;
            std::condition_variable cv;
            bool running = false;
            bool release = false;
        };
        std::shared_ptr<State> s_;
    };
};

}  // namespace cascade::gui

using Access = cascade::gui::AppWindowTestAccess;

namespace {

void setEnv(const char* name, const std::string& value) {
#if defined(_WIN32)
    _putenv_s(name, value.c_str());
    SetEnvironmentVariableA(name, value.c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}

std::filesystem::path g_scratch;
const char* const kWin = "usagewin";
std::string g_text;

template <class F>
void imguiFrame(F body, bool capture = false, float w = 900.0f, float h = 900.0f) {
    ImGuiIO& io = ImGui::GetIO();
    io.DeltaTime = 1.0f / 60.0f;
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(w, h), ImGuiCond_Always);
    ImGui::Begin(kWin, nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings);
    if (capture) { ImGui::LogToBuffer(); }
    body();
    if (capture) {
        g_text = GImGui->LogBuffer.c_str();
        ImGui::LogFinish();
        if (std::getenv("USAGEWIN_DUMP") != nullptr) { std::printf("---- frame text ----\n%s\n", g_text.c_str()); }
    }
    ImGui::End();
    ImGui::Render();
}

template <class F>
void frame(AppWindow& a, F body, bool capture = false) {
    Access::look(a);
    imguiFrame(body, capture);
    Access::after(a);
}
void pass(AppWindow& a) {
    Access::look(a);
    Access::after(a);
}
void passes(AppWindow& a, int n) {
    for (int i = 0; i < n; ++i) { pass(a); }
}
bool has(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }

struct Tap {
    AppWindow& a;
    Access::Sent log;
    explicit Tap(AppWindow& w) : a(w) { Access::tap(a, &log); }
    ~Tap() { Access::tap(a, nullptr); }
    int count(std::uint32_t op) const {
        int n = 0;
        for (const cmd::QueuedCommand& q : log) { n += q.c.op == op ? 1 : 0; }
        return n;
    }
    const cmd::QueuedCommand* last(std::uint32_t op) const {
        for (auto it = log.rbegin(); it != log.rend(); ++it) {
            if (it->c.op == op) { return &*it; }
        }
        return nullptr;
    }
};

template <class F>
bool clickItem(F body, const char* label) {
    ImGuiIO& io = ImGui::GetIO();
    ImGuiID target = 0;
    imguiFrame([&] {
        body();
        target = ImGui::GetID(label);
    });
    bool found = false;
    float hx = 0.0f, hy = 0.0f;
    for (float y = 2.0f; y < 560.0f && !found; y += 4.0f) {
        for (float x = 8.0f; x < 700.0f && !found; x += 12.0f) {
            io.AddMousePosEvent(x, y);
            imguiFrame(body);
            imguiFrame(body);
            if (GImGui->HoveredId == target) {
                found = true;
                hx = x;
                hy = y;
            }
        }
    }
    if (!found) {
        io.AddMousePosEvent(2.0f, 2.0f);
        imguiFrame(body);
        return false;
    }
    io.AddMousePosEvent(hx, hy);
    imguiFrame(body);
    io.AddMouseButtonEvent(0, true);
    imguiFrame(body);
    io.AddMouseButtonEvent(0, false);
    imguiFrame(body);
    io.AddMousePosEvent(2.0f, 2.0f);
    imguiFrame(body);
    imguiFrame(body);
    return true;
}

struct Env {
    std::unique_ptr<th::TestWindow> app;
    Env() {
        app = std::make_unique<th::TestWindow>();
        passes(*app, 3);
    }
    AppWindow& a() { return *app; }
};

// The drawing of the row with the checkbox's state read from the text the log keeps ("[x]" or "[ ]" before the label).
bool checkboxTicked(const std::string& text) {
    const std::size_t at = text.find("Send anonymous usage reports");
    if (at == std::string::npos) { return false; }
    const std::size_t line = text.rfind('\n', at);
    const std::string head = text.substr(line == std::string::npos ? 0 : line + 1, at - (line == std::string::npos ? 0 : line + 1));
    return head.find('x') != std::string::npos || head.find('X') != std::string::npos || head.find('*') != std::string::npos;
}

// ======================================================================================================================
// A. THE USAGE ROW
// ======================================================================================================================
void testUsageRow() {
    std::printf("  A: the usage row: the switch and the id are the engine's facts; the click lands a pass later\n");
    Env env;
    AppWindow& a = env.a();
    const auto draw = [&] { Access::drawUsage(a); };

    frame(a, draw, true);
    CHECK(has(g_text, "Send anonymous usage reports"));
    CHECK(has(g_text, "Off. Nothing is transmitted."));
    CHECK(!has(g_text, "Install id"));
    CHECK(!checkboxTicked(g_text));
    CHECK(!Access::engineOn(a));
    CHECK_EQ(Access::fact(a, "fact.telemetry.enabled"), std::string("0"));
    CHECK_EQ(Access::fact(a, "fact.telemetry.installId"), std::string());

    // THE CLICK: one FOXAPP_OP_TELEMETRY_CONSENT 1 through the session, applied at the next pass and not before.
    // (The pass is held from before the click to the read of the engine: free-running, a pass of its own would take the
    // session's queue within milliseconds, and "not before" is about what precedes the next pass.)
    {
        Access::PassHold hold(a);
        Tap tap(a);
        CHECK(clickItem(draw, "Send anonymous usage reports"));
        CHECK_EQ(tap.count(FOXAPP_OP_TELEMETRY_CONSENT), 1);
        const cmd::QueuedCommand* q = tap.last(FOXAPP_OP_TELEMETRY_CONSENT);
        CHECK(q != nullptr && q->c.ival[0] == 1);
        CHECK(!Access::engineOnHeld(a));  // (not applied: the engine has not had a pass)
    }
    // The checkbox shows what was asked for AT ONCE (the fact has not caught up): no snap-back; no id yet.
    imguiFrame(draw, true);
    CHECK(checkboxTicked(g_text));
    CHECK(Access::pending(a).valid);
    CHECK(!has(g_text, "Install id"));
    CHECK(has(g_text, "Off. Nothing is transmitted."));  // (the label follows the fact, a pass later)
    std::printf("      the click: CONSENT 1 sent; the checkbox ticked ahead of the fact, no id yet\n");

    // The pass: the engine mints the id; the next frame reads the fact - the id line, the notice, the pending value released.
    pass(a);
    CHECK(Access::engineOn(a));
    frame(a, draw, true);
    CHECK(!Access::pending(a).valid);
    CHECK(checkboxTicked(g_text));
    const std::string id = Access::engineId(a);
    CHECK(!id.empty());
    CHECK_EQ(Access::fact(a, "fact.telemetry.installId"), id);
    CHECK(has(g_text, "Install id: " + id));
    CHECK(has(g_text, "One report at start-up"));
    CHECK(!has(g_text, "Off. Nothing is transmitted."));
    std::printf("      a pass later: the fact says on, the id line reads %zu characters, the pending value is released\n", id.size());

    // OFF: the id goes (it does not exist while reporting is off) and the row says nothing is transmitted.
    {
        Tap tap(a);
        CHECK(clickItem(draw, "Send anonymous usage reports"));
        CHECK_EQ(tap.count(FOXAPP_OP_TELEMETRY_CONSENT), 1);
        const cmd::QueuedCommand* q = tap.last(FOXAPP_OP_TELEMETRY_CONSENT);
        CHECK(q != nullptr && q->c.ival[0] == 0);
    }
    imguiFrame(draw, true);
    CHECK(!checkboxTicked(g_text));  // (ahead of the fact again: asked off, still on in the fact)
    CHECK(has(g_text, "Install id: " + id));
    pass(a);
    frame(a, draw, true);
    CHECK(!Access::engineOn(a));
    CHECK(Access::engineId(a).empty());
    CHECK(!has(g_text, "Install id"));
    CHECK(has(g_text, "Off. Nothing is transmitted."));
    CHECK(!checkboxTicked(g_text));

    // A CLICK THE ENGINE NEVER ANSWERS (a command that was refused, or lost): the checkbox gives the value back after half a
    // second - it does not stay ticked on a switch that is off. Simulated by a pending value the fact will never match.
    imguiFrame([&] {
        Access::pending(a).valid = true;
        Access::pending(a).on = true;
        Access::pending(a).at = ImGui::GetTime();
        Access::drawUsage(a);
    }, true);
    CHECK(checkboxTicked(g_text));
    for (int i = 0; i < 20; ++i) { imguiFrame(draw); }  // 20 frames of 1/60 s: a third of a second
    imguiFrame(draw, true);
    CHECK(Access::pending(a).valid);
    CHECK(checkboxTicked(g_text));
    for (int i = 0; i < 20; ++i) { imguiFrame(draw); }
    imguiFrame(draw, true);
    CHECK(!Access::pending(a).valid);
    CHECK(!checkboxTicked(g_text));
    std::printf("      an unanswered click: ticked for a third of a second, given back after half of one\n");
}

// ======================================================================================================================
// B. THE BUNDLE
// ======================================================================================================================
// THE TEST'S OWN CLIPBOARD. ImGui's default on Windows is the OPERATING SYSTEM'S, which every process on the desktop shares: this test, its
// _stepped and _free registrations (ctest -j runs them at once), and the user's own copy and paste. Found by the P6 soak: with eight copies
// at once, 3-13 runs in 100 read another copy's bundle (the real version where the foreign 7.7.7 had just been copied) - and a test run
// overwrote whatever the person at the keyboard had copied. The bundle is built by the window and handed to ImGui::SetClipboardText, which
// is all this test needs to see, so the clipboard is a string of this process.
std::string g_privateClipboard;
const char* privateClipboardGet(ImGuiContext*) { return g_privateClipboard.c_str(); }
void privateClipboardSet(ImGuiContext*, const char* text) { g_privateClipboard = text != nullptr ? text : ""; }

std::string clipboard() {
    const char* c = ImGui::GetClipboardText();
    return c != nullptr ? std::string(c) : std::string();
}

void testBundle() {
    std::printf("  B: the bundle takes the engine's context block and the facts' counters, and writes nothing process-global\n");
    Env env;
    AppWindow& a = env.a();
    // The engine's pass renders the block; the window never asked it to.
    pass(a);
    const std::string engines = cascade::core::diagContextBlock();
    CHECK(has(engines, std::string("version: ") + cascade::versionString() + "\n"));

    Access::setCounters(a, 41, 5);
    Access::setUnclean(a, true);
    passes(a, 2);  // (the facts follow the engine in the next publish)
    imguiFrame([&] { Access::copyBundle(a); });
    const std::string bundle = clipboard();
    CHECK(!bundle.empty());
    CHECK(has(bundle, engines));  // the engine's block, whole
    CHECK(has(bundle, "launches: 41\n"));
    CHECK(has(bundle, "crashes: 5\n"));
    CHECK(has(bundle, "last-run-unclean: yes\n"));
    CHECK(has(Access::bundleStatus(a), "Copied to the clipboard"));
    // ...and the file under the crash folder holds the same bundle.
    const std::string dir = cascade::core::diagCrashDir();
    CHECK(!dir.empty());
    std::ifstream in(std::filesystem::path(dir) / "diagnostics.txt", std::ios::binary);
    CHECK(in.good());
    std::stringstream ss;
    ss << in.rdbuf();
    CHECK(ss.str() == bundle);
    CHECK(has(Access::bundleStatus(a), "saved as"));
    std::printf("      the clipboard and %s/diagnostics.txt: %zu bytes, the engine's context whole, launches 41, crashes 5\n",
                "the crash folder", bundle.size());

    // THE CRASH HANDLER'S COPY IS NOT TOUCHED. A foreign block stands in the process-global (what a crash would print); the
    // old member rendered its own copy of the context over it. The bundle now carries whatever block stands, and leaves it be.
    {
        // (The engine's pass keeps the process-global block true to the session - that is the next part of this test - so a free-running
        // pass would render over the foreign block between the two lines that matter: the pass is parked for exactly this stretch.)
        Access::PassHold hold(a);
        cascade::core::DiagContext foreign;
        foreign.version = "7.7.7";
        foreign.commit = "decafbad";
        foreign.mode = "CW";
        foreign.sourceKind = "iqfile";
        foreign.plugins.push_back("Foreign 1.0.0");
        cascade::core::setDiagContext(foreign);
        const std::string foreignBlock = cascade::core::diagContextBlock();
        imguiFrame([&] { Access::copyBundle(a); });
        CHECK(cascade::core::diagContextBlock() == foreignBlock);
        CHECK(has(clipboard(), "version: 7.7.7\n"));
        CHECK(!has(clipboard(), std::string("version: ") + cascade::versionString() + "\n"));
    }
    std::printf("      a foreign context in the crash handler's slot: carried by the bundle, left in place by it\n");

    // THE BLOCK FOLLOWS THE SESSION through the engine's pass alone: a new mode is in the block within the second, and
    // therefore in the next bundle - no call from the window.
    Access::setCounters(a, 41, 5);
    const bool wasAm = has(engines, "mode: AM\n");
    Access::submit(a, cmd::makeInt(FOXAPI_OP_SET_MODE, wasAm ? FOXAPI_DEMOD_NFM : FOXAPI_DEMOD_AM));
    const char* want = wasAm ? "mode: NFM\n" : "mode: AM\n";
    bool followed = false;
    for (int i = 0; i < 1500 && !followed; ++i) {
        pass(a);
        followed = has(cascade::core::diagContextBlock(), want);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    std::printf("      after SET_MODE the engine's block says [%s]: %s\n", std::string(want, std::strlen(want) - 1).c_str(), followed ? "yes" : "NO");
    CHECK(followed);
    imguiFrame([&] { Access::copyBundle(a); });
    CHECK(has(clipboard(), want));
}

// ======================================================================================================================
// C. THE MEMBERS NAME NO ENGINE OBJECT
// ======================================================================================================================
std::string bodyOf(const std::string& text, const std::string& head) {
    const std::size_t at = text.find(head);
    if (at == std::string::npos) { return {}; }
    std::size_t open = text.find('{', at);
    int depth = 0;
    for (std::size_t i = open; i < text.size(); ++i) {
        if (text[i] == '{') { ++depth; }
        if (text[i] == '}' && --depth == 0) { return text.substr(open, i - open + 1); }
    }
    return {};
}
std::string withoutComments(const std::string& s) {
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s.compare(i, 2, "//") == 0) {
            while (i < s.size() && s[i] != '\n') { ++i; }
            out += '\n';
        } else {
            out += s[i];
        }
    }
    return out;
}

void testSource() {
    std::printf("  C: the members name no engine object, send through the session, and the window never renders the context\n");
    const std::filesystem::path root = std::filesystem::path(__FILE__).parent_path().parent_path();
    std::ifstream in(root / "src" / "gui" / "app_window.cpp", std::ios::binary);
    CHECK(in.good());
    std::stringstream ss;
    ss << in.rdbuf();
    const std::string text = ss.str();
    for (const char* head : {"void AppWindow::drawUsageReportingSection() {", "void AppWindow::copyDiagnosticsBundle() {"}) {
        const std::string body = withoutComments(bodyOf(text, head));
        CHECK(!body.empty());
        CHECK(body.find("engine_.") == std::string::npos);
        CHECK(body.find("applyCommand") == std::string::npos);
        CHECK(body.find("submitCommand") == std::string::npos);
        CHECK(body.find("setDiagContext") == std::string::npos);
        CHECK(body.find("refreshDiagContext") == std::string::npos);
    }
    const std::string usage = withoutComments(bodyOf(text, "void AppWindow::drawUsageReportingSection() {"));
    CHECK(usage.find("FOXAPP_OP_TELEMETRY_CONSENT") != std::string::npos);
    CHECK(usage.find("uiSubmit") != std::string::npos);
    // The window as a whole never calls the engine's context refresh (the pass owns it).
    CHECK(withoutComments(text).find("refreshDiagContext") == std::string::npos);
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_usage_diag_window\n");
#if defined(_WIN32)
    const int pid = _getpid();
#else
    const int pid = static_cast<int>(getpid());
#endif
    g_scratch = std::filesystem::temp_directory_path() / ("foxsdr_usagewin_" + std::to_string(pid));
    std::filesystem::create_directories(g_scratch / "plugins");
    g_pluginDir = (g_scratch / "plugins").string();
    for (const char* v : {"LOCALAPPDATA", "APPDATA", "USERPROFILE", "HOME", "XDG_CONFIG_HOME", "XDG_STATE_HOME",
                          "XDG_DATA_HOME", "XDG_CACHE_HOME"}) {
        setEnv(v, g_scratch.string());
    }
    setEnv("FOXSDR_TELEMETRY_URL", "http://127.0.0.1:9");
    setEnv("FOXSDR_OPEN_SECTIONS", "Usage reporting");

    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1600.0f, 1000.0f);
    io.IniFilename = nullptr;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    ImGui::GetPlatformIO().Platform_GetClipboardTextFn = &privateClipboardGet;
    ImGui::GetPlatformIO().Platform_SetClipboardTextFn = &privateClipboardSet;
    CHECK(cascade::gui::fonts::load());
    Access::install();

    testUsageRow();
    testBundle();
    testSource();

    ImGui::DestroyContext();
    std::error_code ec;
    std::filesystem::remove_all(g_scratch, ec);
    return testSummary("test_usage_diag_window");
}
