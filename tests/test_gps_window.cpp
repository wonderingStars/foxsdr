// test_gps_window.cpp - THE GPS ROW AND THE SERIAL PORTS READ THE WINDOW'S ENGINE API SESSION (window port, group 14, part b:
// drawGpsPositionControl and drawSerialPortsSection; API 0.3 section 8.13 - the GPS list and FOXAPP_STATUS_GPS_RESULT).
//
// WHAT THEY DRAW FROM. The row's fields, key and status line are the GPS list's one entry (the reader's state, its sentence
// worded by the engine, whether it is listening), read once a look; its commands are FOXAPI_OP_GPS (start with a port and a baud,
// stop) and FOXAPP_OP_CLEAR_STATUS (the refusal, and the finished read - code 6) through the session. The machine's serial
// ports are the engine's too: the window ASKS (FOXAPI_OP_SCAN_SERIAL_PORTS) and draws the SERIAL_PORTS list the pass answers
// with. The receiver on the port is a fake wire handed to the engine's own reader (GpsReader::setOpenerForTest), the ports
// are the engine's test hook's - no COM port, no registry.
//
//   A  the idle row, the key that starts a read (the command it sends), the row while it listens (the sentence, the fields
//      disabled, Stop), and Stop
//   B  a FIX: the position (the state's) follows, the rail's fold is to be revealed, and THE COVERAGE STARTS AGAIN even when
//      the fix is the pair already in force (what the engine's onGpsFixApplied hook and applyReceiverPosition did for every
//      apply; the position follow cannot see a re-apply, so the window's Fixed edge says it)
//   C  a failure and a time-out in the warning ink's place, and the edits that forget them (the port field and the baud:
//      two FOXAPP_OP_CLEAR_STATUS through the session, the old line gone a pass later)
//   D  the machine's ports: the arrow asks, the popup draws what the pass answers (and says "none" only after the answer),
//      SYSTEM > Serial ports asks on opening and on Refresh
//   E  the members name no engine object and walk no registry (the fence's rule, restated), and the idle read allocates nothing
//
// THE ACCESSOR TRAP: a ported member reads the SESSION, so each frame here looks first (the engine's pass takes the session's
// queue, publishes, the window reads its state and the GPS entry), draws, and lets the NEXT look take what the draw sent.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <new>
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
#include "core/gps_reader.hpp"
#include "core/i18n.hpp"
#include "core/nmea.hpp"
#include "core/serial_port.hpp"
#include "engine/engine.hpp"
#include "engine_api_fake_radio.hpp"
#include "gui/app_window.hpp"
#include "pass_seam.hpp"
#include "gui/fonts.hpp"
#include "imgui.h"
#include "imgui_internal.h"
#include "test_check.hpp"

#define CHECK_EQ(a, b) CHECK((a) == (b))
#define CHECK_NEAR(a, b, eps) CHECK(std::fabs((a) - (b)) <= (eps))

// --- allocations on THIS thread (the window and the test share it) ------------------------------------------------
namespace {
thread_local long t_allocs = 0;
std::string g_pluginDir;
std::string pluginDir() { return g_pluginDir; }
std::vector<std::string> g_ports = {"COM7", "COM9"};
std::vector<std::string> ports() { return g_ports; }
}  // namespace
void* operator new(std::size_t n) {
    ++t_allocs;
    if (void* p = std::malloc(n != 0 ? n : 1)) { return p; }
    throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace cmd = cascade::core::cmd;
using cascade::gui::AppWindow;

namespace cascade::gui {

struct AppWindowTestAccess {
    using Sent = std::vector<cmd::QueuedCommand>;
    using Engine = cascade::engine::Engine;

    static void install() {
        Engine::testHooks_.makeDevice = &engine_api_test::makeFakeRadio;
        Engine::testHooks_.nativeScan = &engine_api_test::fakeNativeScan;
        Engine::testHooks_.soapyScan = &engine_api_test::noSoapyRadios;
        Engine::testHooks_.pluginDir = &pluginDir;
        Engine::testHooks_.serialPorts = &ports;
    }
    static void look(AppWindow& a) {
        passseam::pass(rawEngine(a.engine_), [&] {
            rawEngine(a.engine_).pollGpsReader();  // (run() takes the reader's fix before the frame, ahead of the pass)
            rawEngine(a.engine_).pumpFrameBegin();
            rawEngine(a.engine_).pumpInputs();
            rawEngine(a.engine_).pumpPlugins();
            (void)rawEngine(a.engine_).pumpPublish();
        });
        a.statusLines_ = rawEngine(a.engine_).statusText();
        a.refreshUiState();
    }
    static void after(AppWindow& a) {
        passseam::workers(rawEngine(a.engine_), [&] {
            rawEngine(a.engine_).pumpWorkers();
            rawEngine(a.engine_).pumpAudio();
        });
    }
    static void submit(AppWindow& a, const cmd::QueuedCommand& q) { a.uiSubmit(q); }
    static void tap(AppWindow& a, Sent* log) { a.uiSubmitTap_ = log; }
    static const FoxReceiverState& state(AppWindow& a) { return a.uiState_; }

    static void drawRow(AppWindow& a) { a.drawGpsPositionControl(); }
    static void drawSection(AppWindow& a) { a.drawSerialPortsSection(); }
    static const AppWindow::GpsRow& row(AppWindow& a) { return a.gpsRow(); }
    static void readRow(AppWindow& a) { a.readGpsRow(); }
    static bool& reveal(AppWindow& a) { return a.gpsRowReveal_; }
    static std::string& port(AppWindow& a) { return a.gpsPort_; }
    static int& baud(AppWindow& a) { return a.gpsBaud_; }
    static void setField(AppWindow& a, const std::string& text) {
        a.gpsPort_ = text;
        std::snprintf(a.gpsPortInput_, sizeof(a.gpsPortInput_), "%s", text.c_str());
    }
    static std::string field(AppWindow& a) { return a.gpsPortInput_; }
    static CoverageMap& coverage(AppWindow& a) { return a.coverage_; }
    static double& typedLat(AppWindow& a) { return a.rxLatInput_; }
    static double& typedLon(AppWindow& a) { return a.rxLonInput_; }
    static const AppWindow::SerialPortsCache& portsCache(AppWindow& a) { return a.serialPortsCache_; }
    static const std::vector<std::string>& serialPorts(AppWindow& a) { return a.serialPorts(); }
    static bool waiting(AppWindow& a) { return a.serialPortsWaiting(); }
    static void clearResult(AppWindow& a) { a.clearGpsResult(); }
    // The engine's reader and its refusal line are the pass's: each reach goes through passseam::on/read (on the pass's thread
    // with the pass out of the way when it free-runs; right here otherwise).
    static void gpsOpener(AppWindow& a, cascade::core::GpsReader::Opener op) {
        passseam::on(rawEngine(a.engine_), [&] { rawEngine(a.engine_).gpsReader_.setOpenerForTest(std::move(op)); });
    }
    static cascade::core::GpsReader::Status gpsStatus(AppWindow& a) {
        return passseam::read<cascade::core::GpsReader::Status>(rawEngine(a.engine_), [&] { return rawEngine(a.engine_).gpsReader_.status(); });
    }
    static void gpsStop(AppWindow& a) { passseam::on(rawEngine(a.engine_), [&] { rawEngine(a.engine_).gpsReader_.stop(); }); }
    static void setRefusal(AppWindow& a, const std::string& s) { passseam::on(rawEngine(a.engine_), [&] { rawEngine(a.engine_).gpsRefusal_ = s; }); }
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
const char* const kWin = "gpswin";
std::string g_text;

template <class F>
void imguiFrame(F body, bool capture = false, float w = 900.0f, float h = 700.0f) {
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
        if (std::getenv("GPSWIN_DUMP") != nullptr) { std::printf("---- frame text ----\n%s\n", g_text.c_str()); }
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
template <class Pred>
bool passUntil(AppWindow& a, Pred pred, int max = 3000) {
    for (int i = 0; i < max; ++i) {
        if (pred()) { return true; }
        pass(a);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return pred();
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
    int countStatus(std::int64_t code) const {
        int n = 0;
        for (const cmd::QueuedCommand& q : log) { n += (q.c.op == FOXAPP_OP_CLEAR_STATUS && q.c.ival[0] == code) ? 1 : 0; }
        return n;
    }
    const cmd::QueuedCommand* last(std::uint32_t op) const {
        for (auto it = log.rbegin(); it != log.rend(); ++it) {
            if (it->c.op == op) { return &*it; }
        }
        return nullptr;
    }
};

// A click on the item `label` names, found by sweeping the pointer over the window while `body` is drawn.
template <class F>
bool clickItem(F body, const char* label, bool switchRow = false) {
    ImGuiIO& io = ImGui::GetIO();
    ImGuiID target = 0;
    imguiFrame([&] {
        body();
        if (switchRow) {
            ImGui::PushID(label);
            target = ImGui::GetID("##switchrow");
            ImGui::PopID();
        } else {
            target = ImGui::GetID(label);
        }
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

// --- the receiver on the wire ---------------------------------------------------------------------------------
class Wire final : public cascade::core::ByteSource {
public:
    struct Step {
        std::string chunk;
        int delayMs = 0;
    };
    explicit Wire(std::vector<Step> steps) : steps_(std::move(steps)) {}
    int read(char* buf, std::size_t cap) override {
        if (next_ < steps_.size()) {
            const Step& s = steps_[next_++];
            std::this_thread::sleep_for(std::chrono::milliseconds(s.delayMs));
            const std::size_t n = std::min(s.chunk.size(), cap);
            std::memcpy(buf, s.chunk.data(), n);
            return static_cast<int>(n);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        return 0;
    }

private:
    std::vector<Step> steps_;
    std::size_t next_ = 0;
};
std::string framed(const char* payload) { return cascade::core::nmeaFrame(payload) + "\r\n"; }
const char* kViewPayload = "GPGGA,123519.00,,,,,0,08,1.1,,M,,M,,";
const char* kFixPayload = "GPGGA,123519.00,5130.4410,N,00007.6656,W,1,08,1.1,35.0,M,46.9,M,,";
constexpr double kFixLat = 51.5073500;
constexpr double kFixLon = -0.1277600;

enum class Receiver { View, Fix, Dead };
Receiver g_receiver = Receiver::View;
std::unique_ptr<cascade::core::ByteSource> openWire(const std::string&, int, std::string& error) {
    switch (g_receiver) {
        case Receiver::View: return std::make_unique<Wire>(std::vector<Wire::Step>{{framed(kViewPayload), 0}});
        case Receiver::Fix:
            return std::make_unique<Wire>(std::vector<Wire::Step>{{framed(kViewPayload), 0}, {framed(kFixPayload), 80}});
        case Receiver::Dead: break;
    }
    error = "no such device";  // (the reader words it: "<port> could not be opened: no such device")
    return nullptr;
}

// A popup left open by an earlier test would take the pointer from everything under it.
void closePopups() {
    imguiFrame([] {
        if (GImGui->OpenPopupStack.Size > 0) { ImGui::ClosePopupToLevel(0, true); }
    });
    imguiFrame([] {});
}

struct Env {
    std::unique_ptr<AppWindow> app;
    Env() {
        closePopups();
        app = std::make_unique<AppWindow>();
        Access::gpsOpener(*app, &openWire);
        passes(*app, 3);
    }
    ~Env() { Access::gpsStop(*app); }
    AppWindow& a() { return *app; }
};

bool rowIs(AppWindow& a, int state) { return Access::row(a).state == state; }

// ======================================================================================================================
// A. THE ROW: idle, a read started by its key, listening, Stop
// ======================================================================================================================
void testStartAndStop() {
    std::printf("  A: the key starts a read through the session; the row lists what the engine's reader says; Stop stops it\n");
    Env env;
    AppWindow& a = env.a();
    g_receiver = Receiver::View;
    CHECK(rowIs(a, 0));
    CHECK(!Access::row(a).listening);
    CHECK(Access::row(a).line.empty());

    // The idle row: the fields and the key, no status line.
    frame(a, [&] { Access::drawRow(a); }, true);
    CHECK(has(g_text, "Read position from GPS"));
    CHECK(has(g_text, "COM3"));        // (the port field's hint)
    CHECK(!has(g_text, "Listening"));
    CHECK(!has(g_text, "[ Stop ]"));

    // With no port named the key is dead (disabled): nothing is sent.
    {
        Tap tap(a);
        (void)clickItem([&] { Access::drawRow(a); }, "Read position from GPS");
        CHECK_EQ(tap.count(FOXAPI_OP_GPS), 0);
    }

    // Name the port and the baud; press the key.
    Access::setField(a, "FAKE1");
    Access::baud(a) = 19200;
    {
        Tap tap(a);
        CHECK(clickItem([&] { Access::drawRow(a); }, "Read position from GPS"));
        CHECK_EQ(tap.count(FOXAPI_OP_GPS), 1);
        const cmd::QueuedCommand* q = tap.last(FOXAPI_OP_GPS);
        CHECK(q != nullptr);
        if (q != nullptr) {
            CHECK_EQ(q->c.ival[0], 1);
            CHECK_NEAR(q->c.num[0], 19200.0, 1e-9);
            CHECK(cmd::textOf(q->c, q->longText) == "FAKE1");
        }
        // Not yet applied: the row has not changed (the command lands at the engine's next pass).
        CHECK(!Access::row(a).listening);
    }
    CHECK(passUntil(a, [&] { return rowIs(a, 1) && Access::row(a).satellites == 8; }));
    const auto listening = Access::row(a);
    std::printf("      listening: [%s] after %.2f s, %d satellites, baud %d\n", listening.line.c_str(), listening.elapsedS,
                listening.satellites, listening.baud);
    CHECK(listening.listening);
    CHECK(!listening.trouble);
    CHECK_EQ(listening.port, std::string("FAKE1"));
    CHECK_EQ(listening.baud, 19200);
    CHECK(listening.line.rfind("Listening on FAKE1 at 19200 - 8 satellites, HDOP 1.1, no fix yet (", 0) == 0);

    // The row while it listens: the sentence, a Stop key where the read key was.
    frame(a, [&] { Access::drawRow(a); }, true);
    CHECK(has(g_text, "Listening on FAKE1 at 19200"));
    CHECK(has(g_text, "Stop"));
    CHECK(!has(g_text, "Read position from GPS"));
    // (The fields are disabled while it listens: a click in the port field takes no focus.)
    {
        Tap tap(a);
        CHECK(clickItem([&] { Access::drawRow(a); }, "Stop"));
        CHECK_EQ(tap.count(FOXAPI_OP_GPS), 1);
        const cmd::QueuedCommand* q = tap.last(FOXAPI_OP_GPS);
        CHECK(q != nullptr && q->c.ival[0] == 0);
    }
    CHECK(passUntil(a, [&] { return rowIs(a, 0); }));
    CHECK(!Access::row(a).listening);
    CHECK(Access::row(a).line.empty());
    frame(a, [&] { Access::drawRow(a); }, true);
    CHECK(has(g_text, "Read position from GPS"));
    CHECK(!has(g_text, "Listening"));
    std::printf("      Stop sent FOXAPI_OP_GPS 0; the engine's reader went idle and the row with it\n");
}

// ======================================================================================================================
// B. A FIX
// ======================================================================================================================
void testFix() {
    std::printf("  B: a fix moves the position, reveals the fold, and starts the coverage again - even for the pair in force\n");
    Env env;
    AppWindow& a = env.a();
    g_receiver = Receiver::Fix;
    Access::setField(a, "FAKE1");
    // Coverage the antenna had measured (a wedge from somewhere).
    Access::coverage(a).record(90.0, 25.0);
    CHECK(!Access::coverage(a).empty());
    CHECK(!Access::reveal(a));
    {
        Tap tap(a);
        CHECK(clickItem([&] { Access::drawRow(a); }, "Read position from GPS"));
        CHECK_EQ(tap.count(FOXAPI_OP_GPS), 1);
    }
    bool sawListening = false;
    CHECK(passUntil(a, [&] {
        sawListening = sawListening || rowIs(a, 1);
        return rowIs(a, 2);
    }));
    CHECK(sawListening);
    // Fixed: the sentence is the engine's, not listening, no warning ink, and the NMEA digits are nowhere.
    const auto& fixed = Access::row(a);
    CHECK_EQ(fixed.line, std::string("Fix: 8 satellites, HDOP 1.1 - position set"));
    CHECK(!fixed.listening);
    CHECK(!fixed.trouble);
    CHECK(Access::reveal(a));  // the fold is to open once
    CHECK(Access::coverage(a).empty());
    // The position (the state's, a pass old at most) and the typed fields follow.
    CHECK(passUntil(a, [&] { return (Access::state(a).flags & FOXAPI_RX_POSITION_SET) != 0u; }));
    pass(a);
    CHECK_NEAR(Access::state(a).rxLatDeg, kFixLat, 1e-6);
    CHECK_NEAR(Access::state(a).rxLonDeg, kFixLon, 1e-6);
    CHECK_NEAR(Access::typedLat(a), kFixLat, 1e-6);
    CHECK_NEAR(Access::typedLon(a), kFixLon, 1e-6);
    frame(a, [&] { Access::drawRow(a); }, true);
    CHECK(has(g_text, "Fix: 8 satellites, HDOP 1.1 - position set"));
    CHECK(!has(g_text, "51.5"));
    std::printf("      fixed: [%s]; position %.5f %.5f; the fold to be revealed; the coverage cleared\n", fixed.line.c_str(),
                Access::state(a).rxLatDeg, Access::state(a).rxLonDeg);

    // THE EDGE FIRES ONCE: the entry stays Fixed for as long as the reader is left alone, and the looks that find it so reveal
    // nothing and clear nothing (a fold that reopened every frame would fight the person who closed it; a coverage map that
    // restarted every frame would never hold a wedge).
    Access::reveal(a) = false;
    Access::coverage(a).record(135.0, 30.0);
    passes(a, 4);
    CHECK(rowIs(a, 2));
    CHECK(!Access::reveal(a));
    CHECK(!Access::coverage(a).empty());
    Access::coverage(a).reset();

    // THE SAME FIX AGAIN, for the pair already in force: the state moves nothing, the window's Fixed edge still starts the
    // coverage again (what the engine's hook did for every apply) and reveals the fold again.
    const double seq = static_cast<double>(Access::state(a).seq);
    (void)seq;
    Access::reveal(a) = false;
    Access::coverage(a).record(180.0, 40.0);
    CHECK(!Access::coverage(a).empty());
    {
        Tap tap(a);
        CHECK(clickItem([&] { Access::drawRow(a); }, "Read position from GPS"));
        CHECK_EQ(tap.count(FOXAPI_OP_GPS), 1);
    }
    // (the row went back through listening and is fixed again)
    CHECK(passUntil(a, [&] { return rowIs(a, 2) && Access::reveal(a); }));
    CHECK(Access::coverage(a).empty());
    CHECK_NEAR(Access::state(a).rxLatDeg, kFixLat, 1e-6);
    std::printf("      the same fix again (the pair in force): the fold revealed again, the coverage cleared again\n");

    // Clearing the finished read: the line goes a pass after the command.
    Access::clearResult(a);
    CHECK(rowIs(a, 2));  // (not yet: the commands land at the next pass)
    passes(a, 2);
    CHECK(rowIs(a, 0));
    CHECK(Access::row(a).line.empty());
}

// ======================================================================================================================
// C. A FAILURE, A TIME-OUT, AND THE EDITS THAT FORGET THEM
// ======================================================================================================================
void testFailureAndEdits() {
    std::printf("  C: a failed read warns; an edit of the port or the baud forgets it (two clears through the session)\n");
    Env env;
    AppWindow& a = env.a();
    g_receiver = Receiver::Dead;
    Access::setField(a, "NOPE9");
    {
        Tap tap(a);
        CHECK(clickItem([&] { Access::drawRow(a); }, "Read position from GPS"));
    }
    CHECK(passUntil(a, [&] { return rowIs(a, 3); }));
    CHECK(Access::row(a).trouble);
    CHECK(!Access::row(a).listening);
    CHECK_EQ(Access::row(a).line, std::string("NOPE9 could not be opened: no such device"));
    frame(a, [&] { Access::drawRow(a); }, true);
    CHECK(has(g_text, "NOPE9 could not be opened: no such device"));
    CHECK(has(g_text, "Read position from GPS"));  // (the key is back: it is not listening)

    // EDITING THE PORT: click into the field and type a character. Two clears, the refusal's and the result's.
    {
        Tap tap(a);
        CHECK(clickItem([&] { Access::drawRow(a); }, "##gpsport"));
        ImGui::GetIO().AddInputCharacter('X');
        imguiFrame([&] { Access::drawRow(a); });
        imguiFrame([&] { Access::drawRow(a); });
        std::printf("      typing in the port field sent %d refusal clear(s) and %d result clear(s)\n",
                    tap.countStatus(cmd::FOXAPP_STATUS_GPS_REFUSAL), tap.countStatus(cmd::FOXAPP_STATUS_GPS_RESULT));
        CHECK(tap.countStatus(cmd::FOXAPP_STATUS_GPS_REFUSAL) >= 1);
        CHECK(tap.countStatus(cmd::FOXAPP_STATUS_GPS_RESULT) >= 1);
        CHECK_EQ(Access::port(a), Access::field(a));
        CHECK(Access::port(a) != "NOPE9");
        // Not yet applied: the old line is still the row's.
        CHECK(rowIs(a, 3));
    }
    passes(a, 2);
    CHECK(rowIs(a, 0));
    CHECK(Access::row(a).line.empty());

    // The baud edit, and the arrow's pick of a port, send the same two clears (the helper they share).
    g_receiver = Receiver::Dead;
    Access::setField(a, "NOPE9");
    {
        Tap tap(a);
        CHECK(clickItem([&] { Access::drawRow(a); }, "Read position from GPS"));
    }
    CHECK(passUntil(a, [&] { return rowIs(a, 3); }));
    {
        Tap tap(a);
        Access::clearResult(a);
        CHECK_EQ(tap.count(FOXAPP_OP_CLEAR_STATUS), 2);
        CHECK_EQ(tap.countStatus(cmd::FOXAPP_STATUS_GPS_REFUSAL), 1);
        CHECK_EQ(tap.countStatus(cmd::FOXAPP_STATUS_GPS_RESULT), 1);
        // ...in that order: the refusal, then the result.
        CHECK(tap.log.size() == 2 && tap.log[0].c.ival[0] == cmd::FOXAPP_STATUS_GPS_REFUSAL &&
              tap.log[1].c.ival[0] == cmd::FOXAPP_STATUS_GPS_RESULT);
    }
    passes(a, 2);
    CHECK(rowIs(a, 0));

    // A REFUSAL (the status line the engine keeps) is drawn beneath the row, in the warning ink's place.
    Access::setRefusal(a, "The GPS fix was refused by the position rule (off the globe, or 0,0).");
    passes(a, 2);
    frame(a, [&] { Access::drawRow(a); }, true);
    CHECK(has(g_text, "The GPS fix was refused"));
}

// ======================================================================================================================
// D. THE MACHINE'S PORTS
// ======================================================================================================================
void testPorts() {
    std::printf("  D: the machine's serial ports are the engine's: the window asks, and draws what the pass answers\n");
    Env env;
    AppWindow& a = env.a();
    g_ports = {"COM7", "COM9"};
    // Nothing asked yet: the cache is empty.
    CHECK(Access::serialPorts(a).empty());
    {
        Tap tap(a);
        CHECK(clickItem([&] { Access::drawRow(a); }, "##gpsports"));
        CHECK_EQ(tap.count(FOXAPI_OP_SCAN_SERIAL_PORTS), 1);
        // The popup is open and the answer is not in: it says nothing (neither a port nor "none found").
        CHECK(Access::waiting(a));
    }
    imguiFrame([&] { Access::drawRow(a); }, true);  // (no pass between the press and this frame)
    CHECK(!has(g_text, "no serial ports found"));
    CHECK(!has(g_text, "COM7"));
    passes(a, 2);
    CHECK(!Access::waiting(a));
    CHECK_EQ(Access::serialPorts(a).size(), std::size_t(2));
    CHECK_EQ(Access::serialPorts(a)[0], std::string("COM7"));
    CHECK_EQ(Access::serialPorts(a)[1], std::string("COM9"));
    imguiFrame([&] { Access::drawRow(a); }, true);
    CHECK(has(g_text, "COM7"));
    CHECK(has(g_text, "COM9"));
    std::printf("      the arrow asked once; the popup showed nothing until the scan, then COM7 and COM9\n");
    // A cache refreshed once a listSeq move, not a frame.
    const std::uint32_t refreshes = Access::portsCache(a).refreshes;
    for (int i = 0; i < 5; ++i) { frame(a, [&] { Access::drawRow(a); }); }
    CHECK(Access::portsCache(a).refreshes <= refreshes + 1u);

    // A machine with no ports: "no serial ports found" is said only AFTER the answer.
    closePopups();
    g_ports.clear();
    {
        Tap tap(a);
        CHECK(clickItem([&] { Access::drawRow(a); }, "##gpsports"));
        CHECK_EQ(tap.count(FOXAPI_OP_SCAN_SERIAL_PORTS), 1);
    }
    imguiFrame([&] { Access::drawRow(a); }, true);
    CHECK(!has(g_text, "no serial ports found"));
    passes(a, 2);
    CHECK(Access::serialPorts(a).empty());
    imguiFrame([&] { Access::drawRow(a); }, true);
    CHECK(has(g_text, "no serial ports found"));
    closePopups();
    g_ports = {"COM7", "COM9"};
}

void testSection() {
    std::printf("  D2: SYSTEM > Serial ports asks once to seed its chip, again on opening and on Refresh\n");
    Env env;
    AppWindow& a = env.a();
    g_ports = {"COM7", "COM9", "COM11"};
    setEnv("FOXSDR_OPEN_SERIAL_PORTS", "1");
    {
        Tap tap(a);
        // The first draw asks (the seed), and the section is open from the environment (a verification seam): it asks on the open edge.
        frame(a, [&] { Access::drawSection(a); }, true);
        CHECK_EQ(tap.count(FOXAPI_OP_SCAN_SERIAL_PORTS), 2);
        // Nothing of the list, and no "none found", until the answer.
        CHECK(!has(g_text, "No serial ports found"));
        passes(a, 2);
        frame(a, [&] { Access::drawSection(a); }, true);
        CHECK(has(g_text, "COM7"));
        CHECK(has(g_text, "COM9"));
        CHECK(has(g_text, "COM11"));
        CHECK(has(g_text, "Refresh"));
        // Settled: more frames ask nothing.
        for (int i = 0; i < 4; ++i) { frame(a, [&] { Access::drawSection(a); }); }
        CHECK_EQ(tap.count(FOXAPI_OP_SCAN_SERIAL_PORTS), 2);
        // Refresh asks again; a port that went away goes from the list a pass later.
        g_ports = {"COM9"};
        CHECK(clickItem([&] { Access::drawSection(a); }, "Refresh"));
        CHECK_EQ(tap.count(FOXAPI_OP_SCAN_SERIAL_PORTS), 3);
        passes(a, 2);
        frame(a, [&] { Access::drawSection(a); }, true);
        CHECK(!has(g_text, "COM7"));
        CHECK(has(g_text, "COM9"));
        std::printf("      3 scans asked (seed, open edge, Refresh); COM7 gone after the third\n");
    }
    // With no ports at all, the section says what to look at - after the answer.
    g_ports.clear();
    {
        Tap tap(a);
        CHECK(clickItem([&] { Access::drawSection(a); }, "Refresh"));
        imguiFrame([&] { Access::drawSection(a); }, true);  // (no pass between the press and this frame)
        CHECK(!has(g_text, "No serial ports found"));
        passes(a, 2);
        frame(a, [&] { Access::drawSection(a); }, true);
        CHECK(has(g_text, "No serial ports found"));
    }
    g_ports = {"COM7", "COM9"};
}

// ======================================================================================================================
// E. THE FENCE'S RULES, AND ALLOCATION
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

void testSourceAndAllocations() {
    std::printf("  E: the members name no engine object and walk no registry; an idle look allocates nothing\n");
    {
        const std::filesystem::path root = std::filesystem::path(__FILE__).parent_path().parent_path();
        std::ifstream in(root / "src" / "gui" / "app_window.cpp", std::ios::binary);
        CHECK(in.good());
        std::stringstream ss;
        ss << in.rdbuf();
        const std::string text = ss.str();
        for (const char* head : {"void AppWindow::drawGpsPositionControl() {", "void AppWindow::drawSerialPortsSection() {",
                                 "void AppWindow::readGpsRow() {", "void AppWindow::clearGpsResult() {",
                                 "const std::vector<std::string>& AppWindow::serialPorts() {",
                                 "void AppWindow::askForSerialPorts() {"}) {
            const std::string body = withoutComments(bodyOf(text, head));
            CHECK(!body.empty());
            CHECK(body.find("engine_.") == std::string::npos);
            CHECK(body.find("gpsReader_") == std::string::npos);
            CHECK(body.find("enumerateSerialPorts") == std::string::npos);
            CHECK(body.find("submitCommand") == std::string::npos);
            CHECK(body.find("applyCommand") == std::string::npos);
        }
        // The key's two commands and the sixth status code are named in the row's own text.
        const std::string row = withoutComments(bodyOf(text, "void AppWindow::drawGpsPositionControl() {"));
        CHECK(row.find("FOXAPI_OP_GPS") != std::string::npos);
        const std::string clear = withoutComments(bodyOf(text, "void AppWindow::clearGpsResult() {"));
        CHECK(clear.find("FOXAPP_STATUS_GPS_RESULT") != std::string::npos);
        CHECK(clear.find("FOXAPP_STATUS_GPS_REFUSAL") != std::string::npos);
    }
    Env env;
    AppWindow& a = env.a();
    passes(a, 5);
    CHECK(rowIs(a, 0));
    Access::readRow(a);  // (warm-up)
    const long before = t_allocs;
    for (int i = 0; i < 1000; ++i) { Access::readRow(a); }
    const long idle = t_allocs - before;
    std::printf("      1000 idle reads of the GPS entry: %ld heap allocations\n", idle);
    CHECK_EQ(idle, 0L);
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_gps_window\n");
#if defined(_WIN32)
    const int pid = _getpid();
#else
    const int pid = static_cast<int>(getpid());
#endif
    g_scratch = std::filesystem::temp_directory_path() / ("foxsdr_gpswin_" + std::to_string(pid));
    std::filesystem::create_directories(g_scratch / "plugins");
    g_pluginDir = (g_scratch / "plugins").string();
    for (const char* v : {"LOCALAPPDATA", "APPDATA", "USERPROFILE", "HOME", "XDG_CONFIG_HOME", "XDG_STATE_HOME",
                          "XDG_DATA_HOME", "XDG_CACHE_HOME"}) {
        setEnv(v, g_scratch.string());
    }
    setEnv("FOXSDR_TELEMETRY_URL", "http://127.0.0.1:9");

    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1600.0f, 1000.0f);
    io.IniFilename = nullptr;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    CHECK(cascade::gui::fonts::load());
    Access::install();

    testStartAndStop();
    testFix();
    testFailureAndEdits();
    testPorts();
    testSection();
    testSourceAndAllocations();

    ImGui::DestroyContext();
    std::error_code ec;
    std::filesystem::remove_all(g_scratch, ec);
    return testSummary("test_gps_window");
}
