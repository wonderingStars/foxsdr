// The WIRING of the saved-radio retry inside the real AppWindow (0.99.92;
// the decision itself is held by test_saved_radio_retry).
//
// THE FIELD REPORT (0.99.91, RTL-SDR Blog V4): the in-app updater closed the old
// window and the installer started the new build while the old process was still
// letting go of the dongle. The start-up restore got "the radio is already in use
// by another program", the session went on running on the signal generator for
// good, and the log said only "did not reopen". What this holds, against a fake
// radio that answers that sentence a scripted number of times:
//
//   - the failed restore says WHY in a word ("(busy)") and, for busy alone, says
//     that it will try again - and the line carries neither the serial nor any
//     of the driver's own words;
//   - the first try is 2 s into the frame loop, not before; a try that finds the
//     radio still busy waits the next, longer delay counted from its own failure;
//   - the try that finds the radio free installs it, clears what the config was
//     remembering for it, and stops;
//   - four tries and no more against a radio that stays busy;
//   - a failure that is not "busy" (unplugged) is never retried;
//   - choosing another source stops the retries.
//
// Headless: it constructs an AppWindow (the window is made in run(), not the
// constructor). CONVERTED FOR THE ENGINE EXTRACTION (the 0.99.97 merge): the retry is the ENGINE's now
// (Engine::pollSavedRadioRetry, once a pass), so the test reaches it through the pass seam (tests/pass_seam.hpp) like every converted window
// test, and the clock master set on ImGui is the engine's retry clock (Engine::savedRadioRetryClockS_, a test-only override: while it is not
// negative the retry reads it, so the stamps and the polls sit at exact seconds). Every expectation of master's is kept as written; what changed
// is how the test reaches them, and that a try is counted by the fake's open() calls (g_opens) and not by "an open is pending", because the
// free-running variant has passes of its own beside the test's polls.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <atomic>
#include <chrono>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "core/config.hpp"
#include "core/diag_log.hpp"
#include "engine/engine.hpp"
#include "gui/app_window.hpp"
#include "test_host.hpp"
#include "pass_seam.hpp"
#include "source/device_source.hpp"
#include "test_check.hpp"
#include "usb/usb_device.hpp"

namespace {

const std::string kArgs = "serial=00000001";

// What the fake radio's open() does next: fail with the in-use sentence this many
// more times, or with `g_otherError` when that is set, and open after that.
std::atomic<int> g_busyOpens{0};
std::atomic<int> g_opens{0};  // every open() the fake was asked for
std::string g_otherError;

class FakeRadio final : public cascade::source::DeviceSource {
public:
    bool start() override {
        abort_ = false;
        return true;
    }
    void stop() override { abort_ = true; }
    bool running() const override { return !abort_; }
    bool selfPaced() const override { return true; }
    double sampleRateHz() const override { return rate_; }
    bool setSampleRateHz(double hz) override {
        rate_ = hz;
        return true;
    }
    double centerFrequencyHz() const override { return centre_; }
    bool setCenterFrequencyHz(double hz) override {
        centre_ = hz;
        return true;
    }
    std::size_t read(std::complex<float>* dst, std::size_t n) override {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        if (abort_) { return 0; }
        for (std::size_t i = 0; i < n; ++i) { dst[i] = {0.0f, 0.0f}; }
        return n;
    }
    const char* name() const override { return "Fake radio"; }
    const char* lastError() const override { return error_.c_str(); }
    const char* driverKey() const override { return "rtlsdr"; }
    bool open(const std::string&) override {
        ++g_opens;
        if (!g_otherError.empty()) {
            error_ = g_otherError;
            return false;
        }
        if (g_busyOpens.load() > 0) {
            --g_busyOpens;
            error_ = cascade::usb::kInUseError;
            return false;
        }
        open_ = true;
        return true;
    }
    void closeDevice() override { open_ = false; }
    bool isOpen() const override { return open_; }
    std::vector<cascade::source::GainInfo> gains() const override { return {}; }
    bool setGainDb(const std::string&, double) override { return false; }
    double gainDb(const std::string&) const override { return 0.0; }
    bool autoGainSupported() const override { return false; }
    bool setAutoGain(bool) override { return false; }
    bool autoGain() const override { return false; }
    std::vector<std::string> antennas() const override { return {}; }
    bool setAntenna(const std::string&) override { return false; }
    std::string antenna() const override { return {}; }
    std::vector<double> supportedSampleRatesHz() const override { return {2.4e6}; }
    bool frequencyRangeHz(double& lo, double& hi) const override {
        lo = 24.0e6;
        hi = 1766.0e6;
        return true;
    }
    bool deviceDead() const override { return false; }
    std::string faultedWhile() const override { return {}; }

private:
    std::atomic<bool> abort_{true};
    double rate_ = 2.4e6;
    double centre_ = 100.0e6;
    bool open_ = false;
    std::string error_;
};

std::unique_ptr<cascade::source::DeviceSource> makeFake(const std::string& kind) {
    if (kind != "rtlsdr") { return nullptr; }
    return std::make_unique<FakeRadio>();
}

std::vector<cascade::source::NativeDeviceInfo> fakeScan() {
    return {{"rtlsdr", "Generic RTL2832U", kArgs}};
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
    const auto dir = std::filesystem::temp_directory_path() /
                     ("foxsdr_saved_radio_retry_app_" + std::to_string(pid));
    std::filesystem::create_directories(dir);
    const std::string s = dir.string();
    for (const char* v : {"LOCALAPPDATA", "APPDATA", "USERPROFILE", "HOME", "XDG_CONFIG_HOME",
                          "XDG_STATE_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME"}) {
        setEnv(v, s);
    }
    setEnv("FOXSDR_TELEMETRY_URL", "http://127.0.0.1:9");
}

}  // namespace

namespace cascade::gui {

struct AppWindowTestAccess {
    static void installHooks() {
        cascade::engine::Engine::testHooks_.makeDevice = &makeFake;
        cascade::engine::Engine::testHooks_.nativeScan = &fakeScan;
    }
    static void restore(AppWindow& a, const cascade::core::AppConfig& c) { passseam::on(th::engine(a), [&] { a.applyConfig(c); }); }
    // The retry's clock, in seconds: set before the restore (the failure's stamp) and before every poll.
    static void clock(AppWindow& a, double t) { passseam::on(th::engine(a), [&] { th::engine(a).savedRadioRetryClockS_ = t; }); }
    static void poll(AppWindow& a) { passseam::on(th::engine(a), [&] { th::engine(a).pollSavedRadioRetry(); }); }
    static bool pending(AppWindow& a) {
        return passseam::read<bool>(th::engine(a), [&] { return th::engine(a).deviceOpenPending_; });
    }
    // The frame loop's own call, until the open in flight has been applied.
    static bool settle(AppWindow& a) {
        const auto t0 = std::chrono::steady_clock::now();
        while (pending(a)) {
            passseam::custom(th::engine(a), [&] { th::engine(a).pollSourceAsync(); });
            if (std::chrono::steady_clock::now() - t0 > std::chrono::seconds(20)) { return false; }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return true;
    }
    static bool deviceOpen(AppWindow& a) {
        return passseam::read<bool>(th::engine(a), [&] { return th::engine(a).device_ != nullptr; });
    }
    static bool armed(AppWindow& a) {
        return passseam::read<bool>(th::engine(a), [&] { return th::engine(a).savedRadioRetryBusy_; });
    }
    static int attempts(AppWindow& a) {
        return passseam::read<int>(th::engine(a), [&] { return th::engine(a).savedRadioRetryAttempts_; });
    }
    static bool kept(AppWindow& a) {
        return passseam::read<bool>(th::engine(a), [&] { return th::engine(a).restoreKeep_.valid(); });
    }
    static std::string sourceError(AppWindow& a) {
        return passseam::read<std::string>(th::engine(a), [&] { return th::engine(a).sourceError_; });
    }
    static void selectGenerator(AppWindow& a) { passseam::on(th::engine(a), [&] { th::engine(a).selectSource(0); }); }
};

}  // namespace cascade::gui

using Access = cascade::gui::AppWindowTestAccess;

namespace {

// The diagnostics ring, joined: what a field report carries.
std::string ring() {
    std::string all;
    for (const std::string& l : cascade::core::DiagLog::instance().ringSnapshot()) {
        all += l;
        all += '\n';
    }
    return all;
}
int count(const std::string& hay, const std::string& needle) {
    int n = 0;
    for (std::size_t p = hay.find(needle); p != std::string::npos; p = hay.find(needle, p + 1)) {
        ++n;
    }
    return n;
}


// One try: the poll at `t`, then the open it launched applied as the frame loop
// applies it. True when a try was launched (the fake was asked to open).
bool tryAt(cascade::gui::AppWindow& app, double t) {
    const int before = g_opens.load();
    Access::clock(app, t);
    Access::poll(app);
    CHECK(Access::settle(app));
    return g_opens.load() > before;
}

cascade::core::AppConfig savedRadio() {
    cascade::core::AppConfig cfg;
    cfg.sourceKind = "rtlsdr";
    cfg.nativeArgs = kArgs;
    cfg.sampleRateHz = 2.4e6;
    cfg.centerHz = 100.0e6;
    return cfg;
}

}  // namespace

int main() {
    std::printf("test_saved_radio_retry_app\n");
    isolate();
    Access::installHooks();

    // --- a radio that is busy at start-up and free two tries later -----------------
    {
        std::printf("  busy at start-up, busy on the first try, free on the second\n");
        cascade::core::DiagLog::instance().resetForTest();
        g_otherError.clear();
        g_busyOpens = 2;
        g_opens = 0;
        th::TestWindow app;
        Access::clock(app, 0.0);
        Access::restore(app, savedRadio());

        // The failed restore: on the generator, the radio kept, and the log says why.
        CHECK(g_opens == 1);
        CHECK(!Access::deviceOpen(app));
        CHECK(Access::kept(app));
        CHECK(Access::armed(app));
        CHECK(Access::attempts(app) == 0);
        const std::string first = ring();
        CHECK(first.find("did not reopen (busy)") != std::string::npos);
        CHECK(first.find("trying it again in 2, 4, 8 and 16 s") != std::string::npos);
        // Neither the serial nor the driver's own sentence is in the ring.
        CHECK(first.find("00000001") == std::string::npos);
        CHECK(first.find(cascade::usb::kInUseError) == std::string::npos);
        // BEFORE THE DELAY: nothing is launched, however many frames pass.
        Access::clock(app, 0.5);
        for (int i = 0; i < 5; ++i) { Access::poll(app); }
        CHECK(!Access::pending(app));
        CHECK(g_opens == 1);
        Access::clock(app, 1.999);
        Access::poll(app);
        CHECK(!Access::pending(app));

        // THE FIRST TRY, 2 s in: still busy. The next delay (4 s) runs from this failure.
        CHECK(tryAt(app, 2.0));
        CHECK(g_opens == 2);
        CHECK(Access::attempts(app) == 1);
        CHECK(Access::armed(app));
        CHECK(!Access::deviceOpen(app));
        CHECK(Access::kept(app));
        CHECK(ring().find("trying the saved radio again (attempt 1 of 4)") != std::string::npos);
        // The screen still says it did not open (the ordinary sentence).
        CHECK(Access::sourceError(app).find("did not open") != std::string::npos);
        // 4 s after the failure at 2.0 is 6.0: not at 5.999.
        Access::clock(app, 5.999);
        Access::poll(app);
        CHECK(!Access::pending(app));

        // THE SECOND TRY: free. Installed, the memory cleared, the retries over.
        CHECK(tryAt(app, 6.0));
        CHECK(g_opens == 3);
        CHECK(Access::deviceOpen(app));
        CHECK(!Access::kept(app));
        CHECK(!Access::armed(app));
        CHECK(Access::attempts(app) == 2);
        const std::string done = ring();
        CHECK(done.find("the saved radio came free") != std::string::npos);
        CHECK(done.find("opened on attempt 2") != std::string::npos);
        CHECK(done.find("00000001") == std::string::npos);
        // And nothing tries again with a radio open.
        Access::clock(app, 1000.0);
        Access::poll(app);
        CHECK(!Access::pending(app));
        CHECK(g_opens == 3);
    }

    // --- a radio that stays busy: four tries, then the ordinary message stands -------
    {
        std::printf("  busy throughout: four tries and no more\n");
        cascade::core::DiagLog::instance().resetForTest();
        g_otherError.clear();
        g_busyOpens = 1000;
        g_opens = 0;
        th::TestWindow app;
        Access::clock(app, 0.0);
        Access::restore(app, savedRadio());
        // Each delay counted from the failure before it: 2, 4, 8, 16.
        double t = 0.0;
        for (const double delay : {2.0, 4.0, 8.0, 16.0}) {
            Access::clock(app, t + delay - 0.01);
            Access::poll(app);
            CHECK(!Access::pending(app));
            t += delay;
            CHECK(tryAt(app, t));
        }
        CHECK(g_opens == 5);  // the restore and four tries
        CHECK(Access::attempts(app) == 4);
        CHECK(!Access::deviceOpen(app));
        CHECK(Access::kept(app));  // the radio stays saved for the next start
        CHECK(ring().find("still in use by another program after 4 more tries") != std::string::npos);
        CHECK(count(ring(), "trying the saved radio again") == 4);
        // A long time later: no fifth.
        Access::clock(app, t + 100000.0);
        for (int i = 0; i < 5; ++i) { Access::poll(app); }
        CHECK(!Access::pending(app));
        CHECK(g_opens == 5);
    }

    // --- a radio that is simply not there is not waited for ------------------------
    {
        std::printf("  not busy: never retried\n");
        cascade::core::DiagLog::instance().resetForTest();
        g_busyOpens = 0;
        g_otherError = "no RTL-SDR with serial 00000001 is present";
        g_opens = 0;
        th::TestWindow app;
        Access::clock(app, 0.0);
        Access::restore(app, savedRadio());
        CHECK(g_opens == 1);
        CHECK(!Access::armed(app));
        CHECK(Access::kept(app));
        const std::string l = ring();
        CHECK(l.find("did not reopen (absent)") != std::string::npos);
        CHECK(l.find("trying it again") == std::string::npos);
        Access::clock(app, 100000.0);
        for (int i = 0; i < 5; ++i) { Access::poll(app); }
        CHECK(!Access::pending(app));
        CHECK(g_opens == 1);
        g_otherError.clear();
    }

    // --- a retry that finds a different problem ends the retries --------------------
    {
        std::printf("  busy, then unplugged: the retries stop at the new reason\n");
        cascade::core::DiagLog::instance().resetForTest();
        g_otherError.clear();
        g_busyOpens = 1;
        g_opens = 0;
        th::TestWindow app;
        Access::clock(app, 0.0);
        Access::restore(app, savedRadio());
        CHECK(Access::armed(app));
        g_otherError = "no RTL-SDR with serial 00000001 is present";
        CHECK(tryAt(app, 2.0));
        CHECK(!Access::armed(app));
        CHECK(Access::kept(app));
        CHECK(ring().find("did not open (absent)") != std::string::npos);
        Access::clock(app, 100000.0);
        Access::poll(app);
        CHECK(!Access::pending(app));
        CHECK(g_opens == 2);
        g_otherError.clear();
    }

    // --- the user chose something else ----------------------------------------------
    {
        std::printf("  choosing another source stops the retries\n");
        cascade::core::DiagLog::instance().resetForTest();
        g_otherError.clear();
        g_busyOpens = 1000;
        g_opens = 0;
        th::TestWindow app;
        Access::clock(app, 0.0);
        Access::restore(app, savedRadio());
        CHECK(Access::armed(app));
        Access::selectGenerator(app);
        CHECK(!Access::armed(app));
        CHECK(!Access::kept(app));
        Access::clock(app, 100000.0);
        for (int i = 0; i < 5; ++i) { Access::poll(app); }
        CHECK(!Access::pending(app));
        CHECK(g_opens == 1);
    }

    return testSummary("test_saved_radio_retry_app");
}
