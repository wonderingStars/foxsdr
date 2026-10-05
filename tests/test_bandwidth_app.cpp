// THE BANDWIDTH LIST, THROUGH THE REAL AppWindow CALL, TO THE FILTER AND THE LOG.
//
// FIELD REPORT (0.99.59, three users, one marked major): "changing the
// bandwidth changes nothing" and "AM lets you hear a station +/- 30 kHz away".
// The log carried "mode: AM, bandwidth 10000" for every mode button and
// NOTHING for a bandwidth change, so there was no way to tell from a report
// whether the control had been used at all.
//
// This drives AppWindow::setModeIndex and AppWindow::setBandwidthIndex - the
// two members the mode buttons and the Bandwidth list call - with a real
// Pipeline running a synthetic tone behind them, and checks both halves of the
// repair:
//   * the DSP: the same pick that used to leave a tone 5 kHz away at -0.1 dB
//     now takes it down by at least 20 dB between 10 kHz and 3 kHz, read at the
//     S-meter (channel power after the channel filter);
//   * the log: one "bandwidth: N Hz in MODE (how)" line per CHANGE, none for a
//     pick of the step already in force, and the mode line it always wrote.
//
// Hermetic, like test_ppm_app: the per-user folders point at a scratch
// directory, no USB enumeration runs and no config is read or written.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <atomic>
#include <chrono>
#include <cmath>
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

#include "core/diag_log.hpp"
#include "gui/app_window.hpp"
#include "source/device_source.hpp"
#include "source/iq_source.hpp"
#include "test_check.hpp"

namespace {

constexpr double kTwoPi = 6.283185307179586476925286766559;

std::filesystem::path g_scratch;

void setEnv(const char* name, const std::string& value) {
#if defined(_WIN32)
    _putenv_s(name, value.c_str());
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
    g_scratch =
        std::filesystem::temp_directory_path() / ("foxsdr_bandwidth_app_" + std::to_string(pid));
    std::filesystem::create_directories(g_scratch);
    const std::string s = g_scratch.string();
    for (const char* v : {"LOCALAPPDATA", "APPDATA", "USERPROFILE", "HOME", "XDG_CONFIG_HOME",
                          "XDG_STATE_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME"}) {
        setEnv(v, s);
    }
    setEnv("FOXSDR_TELEMETRY_URL", "http://127.0.0.1:9");
}

std::vector<cascade::source::NativeDeviceInfo> noDevices() { return {}; }

// One complex tone at a settable offset from the tuned frequency.
class ToneSource final : public cascade::source::IqSource {
public:
    explicit ToneSource(double rateHz) : rate_(rateHz) {}
    bool start() override {
        running_ = true;
        return true;
    }
    void stop() override { running_ = false; }
    bool running() const override { return running_; }
    bool selfPaced() const override { return false; }
    double sampleRateHz() const override { return rate_; }
    bool setSampleRateHz(double) override { return false; }
    double centerFrequencyHz() const override { return centre_; }
    bool setCenterFrequencyHz(double hz) override {
        centre_ = hz;
        return true;
    }
    void setToneHz(double hz) { toneHz_.store(hz, std::memory_order_relaxed); }
    std::size_t read(std::complex<float>* dst, std::size_t n) override {
        const double step = toneHz_.load(std::memory_order_relaxed) / rate_;
        for (std::size_t i = 0; i < n; ++i) {
            const double a = kTwoPi * phase_;
            dst[i] = {static_cast<float>(std::cos(a)), static_cast<float>(std::sin(a))};
            phase_ += step;
            phase_ -= std::floor(phase_);
        }
        return n;
    }
    const char* name() const override { return "bandwidth app test tone"; }
    const char* lastError() const override { return ""; }

private:
    double rate_;
    double centre_ = 13720000.0;
    double phase_ = 0.0;
    std::atomic<double> toneHz_{0.0};
    bool running_ = false;
};

}  // namespace

namespace cascade::gui {

struct AppWindowTestAccess {
    static void installHooks() { AppWindow::testHooks_.nativeScan = &noDevices; }
    static void setMode(AppWindow& a, int i) { a.setModeIndex(i); }
    static void setBandwidth(AppWindow& a, int i) { a.setBandwidthIndex(i); }
    static void logBandwidth(AppWindow& a, const char* how) { a.logBandwidthChange(how); }
    static double bandwidth(AppWindow& a) { return a.vfoBandwidthHz_; }
    static void run(AppWindow& a, std::unique_ptr<cascade::source::IqSource> src) {
        a.installSource(std::move(src));
        a.followInputRate();  // what every source change in the application does
        // The application starts with the VFO 300 kHz off centre (where the
        // generator's signals are); the tones below are measured on it, so it
        // is put on the centre first.
        a.pipeline_.setVfoOffsetHz(0.0);
        a.pipeline_.start();
    }
    static void stop(AppWindow& a) { a.pipeline_.stop(); }
    static cascade::core::Pipeline& pipeline(AppWindow& a) { return a.pipeline_; }
};

}  // namespace cascade::gui

using Access = cascade::gui::AppWindowTestAccess;

namespace {

// Indices into the GUI's bandwidth table (200k, 150k, 12.5k, 10k, 6k, 3k) and
// its mode buttons (NFM, WFM, AM, DSB, USB, CW, LSB, RAW).
constexpr int kBw10k = 3;
constexpr int kBw3k = 5;
constexpr int kModeAm = 2;

// How many lines in the log ring contain `needle`.
std::size_t countLines(const std::string& needle) {
    std::size_t n = 0;
    for (const std::string& l : cascade::core::DiagLog::instance().ringSnapshot()) {
        if (l.find(needle) != std::string::npos) { ++n; }
    }
    return n;
}

template <typename Pred>
bool waitFor(Pred pred, int timeoutMs) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) { return true; }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return pred();
}

double settledMeterDb(cascade::core::Pipeline& p) {
    const std::uint64_t mark = p.audioSamplesProduced();
    (void)waitFor([&] { return p.audioSamplesProduced() - mark >= 19200u; }, 20000);
    double acc = 0.0;
    for (int i = 0; i < 6; ++i) {
        acc += static_cast<double>(p.signalPowerDb());
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }
    return acc / 6.0;
}

}  // namespace

int main() {
    std::printf("test_bandwidth_app\n");
    isolate();
    Access::installHooks();

    cascade::gui::AppWindow app;

    // --- The log --------------------------------------------------------------
    Access::setMode(app, kModeAm);
    CHECK(countLines("mode: AM, bandwidth 10000") >= 1);  // always written
    CHECK(Access::bandwidth(app) == 10000.0);

    const std::size_t before3k = countLines("bandwidth: 3000 Hz in AM");
    Access::setBandwidth(app, kBw3k);
    CHECK(Access::bandwidth(app) == 3000.0);
    CHECK(countLines("bandwidth: 3000 Hz in AM") == before3k + 1);
    // The step already in force: nothing changes, nothing is logged.
    Access::setBandwidth(app, kBw3k);
    CHECK(countLines("bandwidth: 3000 Hz in AM") == before3k + 1);
    // The paths that set the width themselves ask for the line by name.
    Access::logBandwidth(app, "dragged on the spectrum");
    CHECK(countLines("bandwidth: 3000 Hz in AM (dragged on the spectrum)") == 1);
    // And a change back is a new line.
    const std::size_t before10k = countLines("bandwidth: 10000 Hz in AM");
    Access::setBandwidth(app, kBw10k);
    CHECK(countLines("bandwidth: 10000 Hz in AM") == before10k + 1);
    // A list index out of range is ignored, changes nothing and logs nothing.
    const std::size_t bandwidthLines = countLines("bandwidth: ");
    Access::setBandwidth(app, 99);
    Access::setBandwidth(app, -1);
    CHECK(Access::bandwidth(app) == 10000.0);
    CHECK(countLines("bandwidth: ") == bandwidthLines);

    // --- The DSP behind the same call ------------------------------------------
    auto owned = std::make_unique<ToneSource>(2400000.0);
    ToneSource* tone = owned.get();
    Access::run(app, std::move(owned));
    cascade::core::Pipeline& p = Access::pipeline(app);
    CHECK(waitFor([&] { return p.audioSamplesProduced() > 4800u; }, 30000));
    CHECK(p.inputRateHz() == 2400000.0);

    const auto readAt = [&](int bwIndex, double offHz) {
        Access::setBandwidth(app, bwIndex);  // the Bandwidth list's own call
        tone->setToneHz(0.0);
        const double ref = settledMeterDb(p);
        tone->setToneHz(offHz);
        return settledMeterDb(p) - ref;
    };
    const double at10k = readAt(kBw10k, 5000.0);
    const double at3k = readAt(kBw3k, 5000.0);
    const double far10k = readAt(kBw10k, 30000.0);
    std::printf("  through setBandwidthIndex: +5 kHz is %.1f dB at 10 kHz and %.1f dB at "
                "3 kHz; +30 kHz at 10 kHz is %.1f dB\n",
                at10k, at3k, far10k);
    CHECK(at10k - at3k >= 20.0);
    CHECK(far10k <= -40.0);

    Access::stop(app);
    std::error_code ec;
    std::filesystem::remove_all(g_scratch, ec);
    return testSummary("test_bandwidth_app");
}
