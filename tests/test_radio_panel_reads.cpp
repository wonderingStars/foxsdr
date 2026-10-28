// test_radio_panel_reads.cpp - THE RADIO PANELS READ THEIR ENGINE API SESSION
// (window port, group 4: the Airspy controls, the deck's bias tee key and the
// Converter rows, engine-stage4.md 6.5).
//
// The panels no longer reach the engine: what they draw is the table's lists
// and facts, read when listSeq moves and kept. This holds the window's side of
// that against the real engine, with a byte-exact fake Airspy and the mock
// radio behind the device hook, so no radio on the desk is ever enumerated or
// opened:
//   A  the Airspy's cache follows the radio: the mode, the stages, an AGC on a
//      stage, the decimation (and the board's own rate, which it does not move);
//   B  the converter rows: the modes offered, the stored setting, the one in
//      force, and the status line built from the radio's own centre (read live);
//   C  WHAT A FRAME COSTS once nothing has changed: the three reads allocate
//      nothing, however many times a frame asks (the panels are drawn every
//      frame; a read per frame that allocated would be a leak of time);
//   D  the panels DRAWN in real ImGui frames: the Airspy panel answers true for
//      an Airspy and false for the generator, the Converter rows draw, and
//      drawing alone sends nothing.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <new>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "airspy_fake_usb.hpp"
#include "core/app_commands.hpp"
#include "core/freq_converter.hpp"
#include "engine/engine.hpp"
#include "engine_api_fake_radio.hpp"
#include "gui/app_window.hpp"
#include "test_host.hpp"
#include "pass_seam.hpp"
#include "gui/fonts.hpp"
#include "imgui.h"
#include "source/airspy_source.hpp"
#include "test_check.hpp"

#define CHECK_EQ(a, b) CHECK((a) == (b))

// --- allocations on THIS thread (the window and the test share it) ------------
namespace {
thread_local long t_allocs = 0;
}
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

namespace {

constexpr const char* kSerial = "644866c83f1a51df";
const std::string kAirspyArgs = std::string("serial=") + kSerial;

// The Airspy's wire hands its device out only while this is open, so a test can
// hold an open on its worker (never more than ten seconds).
std::atomic<bool> g_gateOpen{true};

std::unique_ptr<cascade::source::DeviceSource> makeFake(const std::string& kind) {
    if (kind == "airspy") {
        auto src = std::make_unique<cascade::source::AirspySource>();
        cascade::usb::UsbDeviceInfo d;
        d.vid = 0x1D50;
        d.pid = 0x60A1;
        d.path = "\\\\?\\usb#vid_1d50&pid_60a1#fake#{a5dcbf10}";
        d.serial = kSerial;
        d.description = "AIRSPY";
        auto holder = std::make_shared<std::unique_ptr<cascade::usb::UsbDevice>>(
            std::make_unique<cascade::test::FakeAirspyUsb>());
        src->setTransportForTest({d}, [holder](const std::string&, std::string& error) {
            for (int i = 0; i < 10000 && !g_gateOpen.load(); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            if (*holder == nullptr) { error = "fake: already handed out"; }
            return std::move(*holder);
        });
        return src;
    }
    return engine_api_test::makeFakeRadio(kind);
}

std::vector<cascade::source::NativeDeviceInfo> fakeScan() {
    return {{"airspy", "Airspy R2", kAirspyArgs},
            {engine_api_test::kFakeRadioDriver, "Mock RTL-SDR (serial 00000001)", engine_api_test::kFakeRadioArgs}};
}

void setEnv(const char* name, const std::string& value) {
#if defined(_WIN32)
    _putenv_s(name, value.c_str());
    SetEnvironmentVariableA(name, value.c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}

}  // namespace

namespace cascade::gui {

struct AppWindowTestAccess {
    // The window's list caches are private types: named here, where the friend may.
    using AirspyCache = AppWindow::AirspyListCache;
    using ConverterCache = AppWindow::ConverterListCache;
    static void installHooks() {
        cascade::engine::Engine::testHooks_.makeDevice = &makeFake;
        cascade::engine::Engine::testHooks_.nativeScan = &fakeScan;
    }
    // THE ROW CHOSEN, with the pass out of the way (passseam::read: the scan, the search and selectSource are one step on the
    // pass's thread, as they were one step of the test's): -1 no such row, 1 the open is pending on its worker, 0 not pending.
    static int selectRow(AppWindow& a, const std::string& driver, const std::string& args) {
        return passseam::read<int>(th::engine(a), [&]() -> int {
            th::engine(a).scanNative();
            for (std::size_t i = 0; i < th::engine(a).nativeDevices_.size(); ++i) {
                if (th::engine(a).nativeDevices_[i].driver != driver || th::engine(a).nativeDevices_[i].args != args) { continue; }
                th::engine(a).selectSource(AppWindow::kNativeRowBase + static_cast<int>(i));
                return th::engine(a).deviceOpenPending_ ? 1 : 0;
            }
            return -1;
        });
    }
    static bool open(AppWindow& a, const std::string& driver, const std::string& args) {
        return selectRow(a, driver, args) >= 0 && finishOpen(a);
    }
    // An open started and left running on its worker (the Airspy's wire holds it
    // while the gate is shut); finishOpen waits for it as a frame's poll would.
    static bool startOpen(AppWindow& a, const std::string& driver, const std::string& args) {
        return selectRow(a, driver, args) == 1;
    }
    // The wait reads the engine's flags through passseam::read (1 pending, 2 open, 0 neither). Inline and stepped it runs the
    // poll a frame's workers run (pollSourceAsync, through passseam::on); free-running the pass runs its own workers, and the
    // wait only watches.
    static bool finishOpen(AppWindow& a) {
        const auto t0 = std::chrono::steady_clock::now();
        const auto state = [&] {
            return passseam::read<int>(th::engine(a), [&]() -> int {
                return th::engine(a).deviceOpenPending_ ? 1 : (th::engine(a).device_ != nullptr ? 2 : 0);
            });
        };
        int s = state();
        while (s == 1) {
            if (passseam::mode() != passseam::Mode::Free) { passseam::on(th::engine(a), [&] { th::engine(a).pollSourceAsync(); }); }
            if (std::chrono::steady_clock::now() - t0 > std::chrono::seconds(20)) { return false; }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            s = state();
        }
        return s == 2;
    }
    static void selectGenerator(AppWindow& a) { passseam::on(th::engine(a), [&] { th::engine(a).selectSource(0); }); }
    // The window looks, as a frame does: the engine publishes, the window reads.
    static void look(AppWindow& a) {
        passseam::on(th::engine(a), [&] { th::engine(a).publishReceiverState(); });
        a.refreshUiState();
    }
    // The engine's pass as far as a submit needs it (the queue applied, the
    // state published), the answers drained as a frame's pumpTransmitKey does:
    // how many commands the window's session had taken and the engine answered.
    static int pass(AppWindow& a) {
        passseam::pass(th::engine(a), [&] {
            th::engine(a).pumpFrameBegin();
            (void)th::engine(a).pumpPublish();
        });
        int answered = 0;
        for (;;) {
            FoxCommandResult r[16];
            for (auto& x : r) { x.structSize = sizeof(x); }
            const int32_t got = a.engineApi_->poll_results(a.uiSession_, r, 16);
            if (got <= 0) { break; }
            answered += got;
            if (got < 16) { break; }
        }
        return answered;
    }
    static void tune(AppWindow& a, double airCentreHz) {
        passseam::on(th::engine(a), [&] { th::engine(a).applyRetuneNow(airCentreHz); });
    }
    static const AirspyCache& airspy(AppWindow& a) { return a.airspyLists(); }
    static const ConverterCache& converter(AppWindow& a) { return a.converterLists(); }
    static const BiasTeePanel& key(AppWindow& a) { return a.biasKeyPanel(); }
    static std::string statusLine(AppWindow& a, bool shortForm) { return a.converterStatusLine(shortForm); }
    static bool drawAirspy(AppWindow& a) { return a.drawAirspyControls(); }
    static void drawConverter(AppWindow& a) { a.drawConverterControls(); }
    static void converterCommand(AppWindow& a, int mode, double loHz, int inverted) {
        FoxCommand c = cascade::core::cmd::makeInt(FOXAPP_OP_SET_CONVERTER, mode, inverted);
        c.num[0] = loHz;
        a.uiSubmit(c);
    }
    // The engine's side, for comparison (each made on the pass's thread with the pass out of the way: passseam::read).
    static bool agc(AppWindow& a, bool lna, bool on) {
        return passseam::read<bool>(th::engine(a), [&] { return th::engine(a).chooseAirspyAgc(lna, on); });
    }
    static bool decimate(AppWindow& a, unsigned d) {
        return passseam::read<bool>(th::engine(a), [&] { return th::engine(a).chooseAirspyDecimation(d); });
    }
    static bool mode(AppWindow& a, cascade::source::AirspySource::GainMode m) {
        return passseam::read<bool>(th::engine(a), [&] { return th::engine(a).chooseAirspyGainMode(m); });
    }
    static double chainRate(AppWindow& a) {
        return passseam::read<double>(th::engine(a), [&] { return th::engine(a).pipeline_.inputRateHz(); });
    }
    static double hardwareRate(AppWindow& a) {
        return passseam::read<double>(th::engine(a), [&] {
            auto* s = dynamic_cast<cascade::source::AirspySource*>(th::engine(a).device_);
            return s == nullptr ? 0.0 : s->hardwareSampleRateHz();
        });
    }
    static double radioCentre(AppWindow& a) {
        return passseam::read<double>(th::engine(a), [&] { return th::engine(a).pipeline_.rawSource().centerFrequencyHz(); });
    }
};

}  // namespace cascade::gui

using Access = cascade::gui::AppWindowTestAccess;
using cascade::gui::AppWindow;
using Mode = cascade::source::AirspySource::GainMode;
namespace cc = cascade::core;

namespace {

// One frame with `body` drawn inside a window; no engine pass (a draw alone
// sends commands to the session, nothing applies them until `Access::pass`).
template <class F>
void frame(F body) {
    ImGuiIO& io = ImGui::GetIO();
    io.DeltaTime = 1.0f / 60.0f;
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(520, 900));
    ImGui::Begin("panel", nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize);
    body();
    ImGui::End();
    ImGui::Render();
}

void testAirspyCache() {
    std::printf("  A: the Airspy panel's reads follow the radio\n");
    th::TestWindow app;
    CHECK(!Access::airspy(app).open);  // nothing looked at yet: nothing open
    CHECK(Access::open(app, "airspy", kAirspyArgs));
    Access::look(app);
    const Access::AirspyCache& a = Access::airspy(app);
    CHECK(a.open);
    CHECK_EQ(a.gainMode, 2);  // Free, as it opens
    CHECK_EQ(a.stages.size(), 3u);
    CHECK_EQ(a.stages[0].name, std::string("LNA"));
    CHECK_EQ(a.stages[1].name, std::string("MIXER"));
    CHECK_EQ(a.stages[2].name, std::string("VGA"));
    for (const auto& s : a.stages) {
        CHECK(!s.agc);
        CHECK(s.maxDb > 0.0f);
    }
    CHECK(!a.lnaAgc && !a.mixerAgc);
    CHECK_EQ(a.decimation, 1u);
    CHECK(a.choices.size() > 1);
    CHECK_EQ(a.choices.front(), 1u);
    const double board = Access::hardwareRate(app);
    CHECK(board > 0.0);
    CHECK_EQ(a.hardwareRateHz, board);
    CHECK_EQ(Access::chainRate(app), board);

    // An AGC on a stage: its own stage says so, the others do not.
    CHECK(Access::agc(app, true, true));
    Access::look(app);
    (void)Access::airspy(app);
    CHECK(a.lnaAgc && !a.mixerAgc);
    CHECK(a.stages[0].agc && !a.stages[1].agc && !a.stages[2].agc);

    // Decimation: the chain runs slower, the board does not.
    CHECK(Access::decimate(app, 2));
    Access::look(app);
    (void)Access::airspy(app);
    CHECK_EQ(a.decimation, 2u);
    CHECK_EQ(a.hardwareRateHz, board);
    CHECK_EQ(Access::chainRate(app), board / 2.0);

    // Linearity: one stage under another name.
    CHECK(Access::mode(app, Mode::Linearity));
    Access::look(app);
    (void)Access::airspy(app);
    CHECK_EQ(a.gainMode, 0);
    CHECK_EQ(a.stages.size(), 1u);
    CHECK_EQ(a.stages[0].name, std::string("LINEARITY"));
    CHECK(!a.lnaAgc && !a.mixerAgc);

    // The generator again: no panel.
    Access::selectGenerator(app);
    Access::look(app);
    (void)Access::airspy(app);
    CHECK(!a.open);
    CHECK(a.stages.empty());
}

void testConverterCache() {
    std::printf("  B: the Converter rows' reads, and the status line\n");
    th::TestWindow app;
    CHECK(Access::open(app, engine_api_test::kFakeRadioDriver, engine_api_test::kFakeRadioArgs));
    Access::look(app);
    const Access::ConverterCache& c = Access::converter(app);
    CHECK_EQ(c.offered.size(), 3u);
    CHECK(c.offered[0] == cc::ConverterMode::Off && c.offered[1] == cc::ConverterMode::Up &&
          c.offered[2] == cc::ConverterMode::Down);
    CHECK(c.stored.mode == cc::ConverterMode::Off);  // nothing set for this radio
    CHECK(!cc::converterActive(c.live));
    CHECK(Access::statusLine(app, false).empty());
    CHECK(Access::statusLine(app, true).empty());

    // An up-converter, as the rows submit it, applied by the engine's next pass.
    Access::converterCommand(app, 1, 125.0e6, 0);
    Access::pass(app);
    Access::look(app);
    (void)Access::converter(app);
    CHECK(c.stored.mode == cc::ConverterMode::Up);
    CHECK_EQ(c.stored.loHz, 125.0e6);
    CHECK(!c.stored.inverted);
    CHECK(c.live.mode == cc::ConverterMode::Up);
    CHECK_EQ(c.live.loHz, 125.0e6);
    // The status line: the radio's own centre (not the air figure), by name or by LO.
    const std::string radio = cc::converterHzText(Access::radioCentre(app));
    const std::string full = Access::statusLine(app, false);
    const std::string brief = Access::statusLine(app, true);
    std::printf("      \"%s\" / \"%s\"\n", full.c_str(), brief.c_str());
    CHECK_EQ(full, "via 125 MHz up-converter - radio at " + radio);
    CHECK_EQ(brief, "via 125 MHz LO - radio at " + radio);
    // ...and it follows a retune WITHOUT the window looking again: live.
    const double before = Access::radioCentre(app);
    Access::tune(app, 110.0e6);
    Access::pass(app);
    CHECK(Access::radioCentre(app) != before);
    CHECK_EQ(Access::statusLine(app, false), "via 125 MHz up-converter - radio at " +
                                                  cc::converterHzText(Access::radioCentre(app)));
    // Inverted: radio = LO - air, and the rows' stored copy says so.
    Access::converterCommand(app, 1, 250.0e6, 1);
    Access::pass(app);
    Access::look(app);
    (void)Access::converter(app);
    CHECK(c.stored.inverted && c.live.inverted);
    CHECK_EQ(c.live.loHz, 250.0e6);
    // Off again: the line goes, the setting is kept for next time.
    Access::converterCommand(app, 0, 250.0e6, 1);
    Access::pass(app);
    Access::look(app);
    (void)Access::converter(app);
    CHECK(c.stored.mode == cc::ConverterMode::Off);
    CHECK_EQ(c.stored.loHz, 250.0e6);
    CHECK(Access::statusLine(app, false).empty());
}

void testNothingAllocatesOnceSettled() {
    std::printf("  C: a settled frame's reads allocate nothing\n");
    th::TestWindow app;
    CHECK(Access::open(app, "airspy", kAirspyArgs));
    Access::look(app);
    // The first read of a fact allocates (it is cached for the lists' sequence); warm them all.
    (void)Access::airspy(app);
    (void)Access::converter(app);
    (void)Access::key(app);
    (void)Access::statusLine(app, false);
    const long before = t_allocs;
    for (int i = 0; i < 1000; ++i) {
        (void)Access::airspy(app);
        (void)Access::converter(app);
        (void)Access::key(app);
        (void)Access::statusLine(app, false);  // no converter on: nothing to build
    }
    const long spent = t_allocs - before;
    std::printf("      4000 reads of an unchanged state: %ld allocation(s)\n", spent);
    CHECK_EQ(spent, 0L);
    // A change IS a re-read (and allowed to allocate): the cache is not stuck.
    CHECK(Access::agc(app, false, true));
    Access::look(app);
    CHECK(Access::airspy(app).mixerAgc);
}

void testDrawn() {
    std::printf("  D: the panels drawn in real frames\n");
    th::TestWindow app;
    // The generator: no Airspy panel; no Converter rows hidden either (all three modes).
    Access::look(app);
    bool airspyDrawn = true;
    frame([&] { airspyDrawn = Access::drawAirspy(app); });
    CHECK(!airspyDrawn);
    frame([&] { Access::drawConverter(app); });

    // An Airspy: its panel is drawn, and drawing sends nothing.
    CHECK(Access::open(app, "airspy", kAirspyArgs));
    Access::look(app);
    for (int i = 0; i < 3; ++i) { frame([&] { airspyDrawn = Access::drawAirspy(app); }); }
    CHECK(airspyDrawn);
    CHECK_EQ(Access::pass(app), 0);  // drawing alone sent nothing: the session answered nothing
    Access::look(app);
    CHECK(Access::airspy(app).stages.size() == 3u);
    frame([&] { Access::drawConverter(app); });
    CHECK_EQ(Access::pass(app), 0);

    // The mock radio with a converter set: the rows draw (the LO field is seeded from the stored setting).
    CHECK(Access::open(app, engine_api_test::kFakeRadioDriver, engine_api_test::kFakeRadioArgs));
    Access::converterCommand(app, 2, 50.0e6, 0);
    Access::pass(app);
    Access::look(app);
    for (int i = 0; i < 3; ++i) { frame([&] { Access::drawConverter(app); }); }
    CHECK_EQ(Access::pass(app), 0);
    CHECK(Access::converter(app).stored.mode == cc::ConverterMode::Down);
    frame([&] { airspyDrawn = Access::drawAirspy(app); });
    CHECK(!airspyDrawn);  // a radio that is not an Airspy has no Airspy panel

    // THE ROWS ARE GREYED WHILE A RADIO IS BEING OPENED (FOXAPI_RX_DEVICE_OPENING):
    // a setting would land on whichever radio happened to be installed at that
    // instant. The mock radio has Down 50 MHz stored (above), so the rows draw
    // their whole set; the Airspy's open is held on its worker.
    const auto lastItemDisabled = [] { return (ImGui::GetItemFlags() & ImGuiItemFlags_Disabled) != 0; };
    g_gateOpen = false;
    CHECK(Access::startOpen(app, "airspy", kAirspyArgs));
    Access::look(app);
    bool greyedWhileOpening = false;
    frame([&] {
        Access::drawConverter(app);
        greyedWhileOpening = lastItemDisabled();
    });
    g_gateOpen = true;
    CHECK(Access::finishOpen(app));
    CHECK(greyedWhileOpening);
    // Opened: the same rows, on the Airspy with a Down stored for it, are live.
    Access::converterCommand(app, 2, 50.0e6, 0);
    CHECK_EQ(Access::pass(app), 1);
    Access::look(app);
    bool greyedAfter = true;
    frame([&] {
        Access::drawConverter(app);
        greyedAfter = lastItemDisabled();
    });
    CHECK(!greyedAfter);
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_radio_panel_reads\n");
#if defined(_WIN32)
    const int pid = _getpid();
#else
    const int pid = static_cast<int>(getpid());
#endif
    const auto scratch = std::filesystem::temp_directory_path() / ("foxsdr_radio_panel_reads_" + std::to_string(pid));
    std::filesystem::create_directories(scratch);
    for (const char* v : {"LOCALAPPDATA", "APPDATA", "USERPROFILE", "HOME", "XDG_CONFIG_HOME", "XDG_STATE_HOME",
                          "XDG_DATA_HOME", "XDG_CACHE_HOME"}) {
        setEnv(v, scratch.string());
    }
    setEnv("FOXSDR_TELEMETRY_URL", "http://127.0.0.1:9");
    Access::installHooks();

    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1000.0f, 1000.0f);
    io.IniFilename = nullptr;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    CHECK(cascade::gui::fonts::load());

    testAirspyCache();
    testConverterCache();
    testNothingAllocatesOnceSettled();
    testDrawn();

    ImGui::DestroyContext();
    std::error_code ec;
    std::filesystem::remove_all(scratch, ec);
    return testSummary("test_radio_panel_reads");
}
