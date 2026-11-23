// test_rtlsdr_repick_engine.cpp - A STOP THAT LANDS INSIDE A RE-OPEN, AND PICKING THE SAME ROW AGAIN, THROUGH THE REAL ENGINE (fix/rtlsdr-reopen, decision F1).
//
// The independent review found: a stop() inside the RTL-SDR's re-open left the radio neither working nor marked dead; the pipeline ignores start()'s result, so the window
// showed nothing, and picking the same row again was a no-op (the engine reopens a picked row only when the radio is dead or the pipeline faulted). With the fix the
// radio is closed and latched faulted with a true sentence. This drives the engine itself, with the SHIPPING RTL-SDR driver behind a thin DeviceSource on a fake USB
// transport (so every state below is the driver's own), and holds, in order:
//   1. the radio streams through the pipeline; its handle dies in a way that leaves the bus (Windows error 1167), the driver starts its re-open and waits for the radio;
//   2. the receiver is STOPPED while the re-open waits: the radio is faulted and dead with kReopenStoppedSentence, nothing streams;
//   3. the receiver is STARTED again (start()'s refusal is ignored by the pipeline, as always): the pipeline latches the fault through its poll, and the sentence the
//      window shows under "Device stopped" is the true one;
//   4. the SAME ROW is picked again: the engine closes the dead radio and opens it afresh (a second open, a new source object), the receiver starts, samples flow,
//      the pipeline is not faulted and the screen's error line is empty.
// The generator never stands in for it, no radio of the desk is opened (testHooks_.makeDevice, nativeScan), the config folders point at a scratch directory.
// Run it on the hidden desktop.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <atomic>
#include <chrono>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
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
#include "engine/engine.hpp"
#include "gui/app_window.hpp"
#include "test_host.hpp"
#include "pass_seam.hpp"
#include "source/device_source.hpp"
#include "source/rtlsdr_source.hpp"
#include "test_check.hpp"
#include "usb/usb_fake.hpp"

namespace fs = std::filesystem;
using cascade::usb::FakeUsbDevice;

namespace {

const std::string kArgs = "serial=00000001";

std::mutex g_mu;
std::deque<std::unique_ptr<FakeUsbDevice>> g_fakes;   // handed to the next sources that open, in order
std::atomic<int> g_opens{0};
std::atomic<long long> g_samples{0};                    // samples the pipeline's source thread has read from any radio
std::atomic<bool> g_faultedOnly{false};                 // the source reports faulted() but not deviceDead() (a family that latches one and not the other)
class Adapter;
std::atomic<Adapter*> g_adapter{nullptr};               // the source the engine holds now
std::atomic<FakeUsbDevice*> g_live{nullptr};            // its fake device (valid while the source is open)

// THE SHIPPING DRIVER on a fake wire, wrapped as the engine's device: open() takes the next fake from the queue and arms the re-open with a seam that never finds the
// radio (it left the bus), so a re-open waits its five seconds - the window in which the stop lands.
class Adapter final : public cascade::source::DeviceSource {
public:
    Adapter() { g_adapter = this; }
    ~Adapter() override {
        Adapter* self = this;
        g_adapter.compare_exchange_strong(self, nullptr);
    }
    bool start() override { return inner_.start(); }
    void stop() override { inner_.stop(); }
    bool running() const override { return inner_.running(); }
    bool selfPaced() const override { return inner_.selfPaced(); }
    double sampleRateHz() const override { return inner_.sampleRateHz(); }
    bool setSampleRateHz(double hz) override { return inner_.setSampleRateHz(hz); }
    double centerFrequencyHz() const override { return inner_.centerFrequencyHz(); }
    bool setCenterFrequencyHz(double hz) override { return inner_.setCenterFrequencyHz(hz); }
    std::size_t read(std::complex<float>* dst, std::size_t n) override {
        const std::size_t got = inner_.read(dst, n);
        g_samples.fetch_add(static_cast<long long>(got));
        return got;
    }
    const char* name() const override { return inner_.name(); }
    const char* lastError() const override { return inner_.lastError(); }
    bool faulted() const override { return inner_.faulted() || g_faultedOnly.load(); }
    const char* driverKey() const override { return "rtlsdr"; }
    bool open(const std::string&) override {
        ++g_opens;
        std::unique_ptr<FakeUsbDevice> f;
        {
            std::lock_guard<std::mutex> lk(g_mu);
            if (g_fakes.empty()) { return false; }
            f = std::move(g_fakes.front());
            g_fakes.pop_front();
        }
        FakeUsbDevice* raw = f.get();
        if (!inner_.openWithTransport(std::move(f), "fake R820T dongle")) { return false; }
        inner_.setReopenForTest([](std::string&, bool& present) -> std::unique_ptr<cascade::usb::UsbDevice> {
            present = false;
            return nullptr;
        });
        inner_.setReopenBoundsForTest(5000, 100);
        g_live = raw;
        return true;
    }
    void closeDevice() override { inner_.closeDevice(); }
    bool isOpen() const override { return inner_.isOpen(); }
    std::vector<cascade::source::GainInfo> gains() const override { return inner_.gains(); }
    bool setGainDb(const std::string& n, double db) override { return inner_.setGainDb(n, db); }
    double gainDb(const std::string& n) const override { return inner_.gainDb(n); }
    bool autoGainSupported() const override { return inner_.autoGainSupported(); }
    bool setAutoGain(bool on) override { return inner_.setAutoGain(on); }
    bool autoGain() const override { return inner_.autoGain(); }
    std::vector<std::string> antennas() const override { return inner_.antennas(); }
    bool setAntenna(const std::string& n) override { return inner_.setAntenna(n); }
    std::string antenna() const override { return inner_.antenna(); }
    std::vector<double> supportedSampleRatesHz() const override { return inner_.supportedSampleRatesHz(); }
    bool frequencyRangeHz(double& lo, double& hi) const override { return inner_.frequencyRangeHz(lo, hi); }
    bool deviceDead() const override { return inner_.deviceDead(); }
    std::string faultedWhile() const override { return inner_.faultedWhile(); }
    bool reopening() const { return inner_.reopeningForTest(); }

private:
    cascade::source::RtlSdrSource inner_;
};

std::unique_ptr<cascade::source::DeviceSource> makeFake(const std::string& kind) {
    if (kind != "rtlsdr") { return nullptr; }
    return std::make_unique<Adapter>();
}

std::vector<cascade::source::NativeDeviceInfo> fakeScan() { return {{"rtlsdr", "Generic RTL2832U", kArgs}}; }
std::vector<cascade::source::SoapyDeviceInfo> fakeSoapy() { return {}; }

std::unique_ptr<FakeUsbDevice> dongle() {
    auto f = std::make_unique<FakeUsbDevice>();
    f->answerIn(0, 0x0034, 0x0600, {0x69, 0x00, 0x02, 0x00, 0xA4});
    f->answerIn(0, 0x00a0, 0x0600, {0x00});
    f->onBeginBulk = [](FakeUsbDevice& d) {
        for (int i = 0; i < 4; ++i) { d.bulkQueue.push_back(std::vector<std::uint8_t>(2048, 0x80)); }
    };
    return f;
}

void setEnv(const char* name, const std::string& value) {
#if defined(_WIN32)
    _putenv_s(name, value.c_str());
    SetEnvironmentVariableA(name, value.c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}

void isolate() {
#if defined(_WIN32)
    const int pid = _getpid();
#else
    const int pid = static_cast<int>(getpid());
#endif
    const auto dir = fs::temp_directory_path() / ("foxsdr_rtlsdr_repick_engine_" + std::to_string(pid));
    fs::create_directories(dir);
    const std::string s = dir.string();
    for (const char* v : {"LOCALAPPDATA", "APPDATA", "USERPROFILE", "HOME", "XDG_CONFIG_HOME", "XDG_STATE_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME"}) { setEnv(v, s); }
    setEnv("FOXSDR_TELEMETRY_URL", "http://127.0.0.1:9");
}

bool waitFor(const std::function<bool()>& pred, int ms) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < until) {
        if (pred()) { return true; }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return pred();
}

}  // namespace

namespace cascade::gui {

struct AppWindowTestAccess {
    static void installHooks() {
        cascade::engine::Engine::testHooks_.makeDevice = &makeFake;
        cascade::engine::Engine::testHooks_.nativeScan = &fakeScan;
        cascade::engine::Engine::testHooks_.soapyScan = &fakeSoapy;
    }
    static bool pending(AppWindow& a) { return passseam::read<bool>(th::engine(a), [&] { return th::engine(a).deviceOpenPending_; }); }
    static bool settle(AppWindow& a) {
        const auto t0 = std::chrono::steady_clock::now();
        while (pending(a)) {
            passseam::custom(th::engine(a), [&] { th::engine(a).pollSourceAsync(); });
            if (std::chrono::steady_clock::now() - t0 > std::chrono::seconds(20)) { return false; }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return true;
    }
    static bool deviceOpen(AppWindow& a) { return passseam::read<bool>(th::engine(a), [&] { return th::engine(a).device_ != nullptr; }); }
    static void scanNative(AppWindow& a) { passseam::on(th::engine(a), [&] { th::engine(a).scanNative(); }); }
    static void selectNative(AppWindow& a, int i) { passseam::on(th::engine(a), [&] { th::engine(a).selectSource(cascade::engine::kNativeRowBase + i); }); }
    static void startReceiver(AppWindow& a) { passseam::on(th::engine(a), [&] { th::engine(a).startReceiver(); }); }
    static void stopReceiver(AppWindow& a) { passseam::on(th::engine(a), [&] { th::engine(a).stopReceiver("a test"); }); }
    static bool faulted(AppWindow& a) { return passseam::read<bool>(th::engine(a), [&] { return th::engine(a).rx().pipeline_.faulted(); }); }
    static bool running(AppWindow& a) { return passseam::read<bool>(th::engine(a), [&] { return th::engine(a).rx().pipeline_.running(); }); }
    static std::string faultMessage(AppWindow& a) { return passseam::read<std::string>(th::engine(a), [&] { return th::engine(a).rx().pipeline_.faultMessage(); }); }
    static std::string sourceError(AppWindow& a) { return passseam::read<std::string>(th::engine(a), [&] { return th::engine(a).rx().sourceError_; }); }
};

}  // namespace cascade::gui

using Access = cascade::gui::AppWindowTestAccess;

int main() {
    std::printf("test_rtlsdr_repick_engine\n");
    setEnv("CASCADE_CONFIG_TEST", "1");
#if defined(_WIN32)
    const unsigned long ownPid = static_cast<unsigned long>(GetCurrentProcessId());
#else
    const unsigned long ownPid = static_cast<unsigned long>(getpid());
#endif
    setEnv("FOXSDR_INSTANCE_SCOPE", "rpe" + std::to_string(ownPid));
    isolate();
    Access::installHooks();

    {
        cascade::core::DiagLog::instance().resetForTest();
        {
            std::lock_guard<std::mutex> lk(g_mu);
            g_fakes.push_back(dongle());
            g_fakes.push_back(dongle());
            g_fakes.push_back(dongle());
        }
        th::TestWindow app;
        Access::scanNative(app);

        // 1. the first pick: the radio opens, the receiver starts, samples flow
        Access::selectNative(app, 0);
        CHECK(Access::settle(app));
        CHECK(Access::deviceOpen(app));
        CHECK(g_opens.load() == 1);
        Access::startReceiver(app);
        CHECK(waitFor([] { return g_samples.load() > 3000; }, 8000));   // (each stream start of the fake delivers four buffers: some thousand samples)
        std::printf("  streaming: %lld samples, faulted %d\n", g_samples.load(), Access::faulted(app) ? 1 : 0);
        CHECK(!Access::faulted(app));
        Adapter* first = g_adapter.load();
        CHECK(first != nullptr);
        FakeUsbDevice* live = g_live.load();
        CHECK(live != nullptr);

        // the radio leaves the bus: the driver closes the handle and waits for it (the seam never finds it)
        if (live != nullptr) {
            live->failBulkAfterText = "a bulk read failed (Windows error 1167)";
            live->failBulkAfter = live->bulkReads;
        }
        CHECK(waitFor([&] { return first != nullptr && first->reopening(); }, 6000));
        CHECK(!Access::faulted(app));   // the re-open is not a fault: nothing latched yet

        // 2. STOP while the re-open waits
        Access::stopReceiver(app);
        CHECK(waitFor([&] { return first != nullptr && first->deviceDead(); }, 4000));
        const std::string said = first != nullptr ? first->lastError() : std::string();
        std::printf("  stopped inside the re-open: dead %d faulted %d: %s\n", first && first->deviceDead() ? 1 : 0, first && first->faulted() ? 1 : 0, said.c_str());
        CHECK(first != nullptr && first->faulted());
        CHECK(said == cascade::source::kReopenStoppedSentence);
        CHECK(!Access::running(app));

        // 3. START again: the driver refuses (the pipeline ignores that), the pipeline's poll latches the fault, and the window's sentence is the true one
        Access::startReceiver(app);
        CHECK(waitFor([&] { return Access::faulted(app); }, 6000));
        const std::string shown = Access::faultMessage(app);
        std::printf("  after Start: pipeline fault: %s\n", shown.c_str());
        CHECK(shown.find(cascade::source::kReopenStoppedSentence) != std::string::npos);

        // 4. PICK THE SAME ROW AGAIN: a fresh open of the same radio, a new source object
        const long long samplesBefore = g_samples.load();
        Access::selectNative(app, 0);
        CHECK(Access::settle(app));
        CHECK(Access::deviceOpen(app));
        std::printf("  the same row picked again: opens %d, source %s the first\n", g_opens.load(), g_adapter.load() == first ? "is still" : "is not");
        CHECK(g_opens.load() == 2);
        CHECK(Access::sourceError(app).empty());
        Access::startReceiver(app);
        CHECK(waitFor([&] { return g_samples.load() > samplesBefore + 3000; }, 8000));
        CHECK(!Access::faulted(app));
        CHECK(Access::running(app));
        CHECK(g_adapter.load() != nullptr);
        CHECK(g_adapter.load() == nullptr || !g_adapter.load()->deviceDead());
        std::printf("  streaming again: %lld samples more, faulted %d\n", g_samples.load() - samplesBefore, Access::faulted(app) ? 1 : 0);
        Access::stopReceiver(app);

        // 5. A SOURCE THAT HAS LATCHED A FAULT without being "dead" (another family's latch, with nothing polling it because the receiver is stopped): picking its row again
        // opens it afresh, as it does for a dead one (the engine asks the source's faulted() as well)
        CHECK(g_opens.load() == 2);
        g_faultedOnly = true;
        Access::selectNative(app, 0);
        CHECK(Access::settle(app));
        g_faultedOnly = false;
        std::printf("  a faulted-only source, its row picked again: opens %d\n", g_opens.load());
        CHECK(g_opens.load() == 3);
        CHECK(Access::deviceOpen(app));
    }
    return testSummary("test_rtlsdr_repick_engine");
}
