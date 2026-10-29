// test_thread_beats_pipeline.cpp - the real Pipeline's two threads beat, and a wedge in either is reported by name.
//
// docs/engine-stage4.md 6.27. tests/test_thread_beats.cpp proves the registry and the monitor with threads the TEST makes; this
// file proves the PRODUCT's registrations: a real Pipeline (its real source thread, its real DSP thread), a watchdog with a
// report directory, and two honest wedges.
//
//   1. HEALTHY: a Pipeline that runs for a few seconds under tight budgets is never reported, and the registry holds a `dsp` and
//      a `source` registration whose phases are words the pipeline says (and whose beats are fresh).
//   2. THE SOURCE THREAD WEDGED IN A VENDOR READ: a self-paced source whose read() stops returning is reported as `source`,
//      phase `read`, within the budget plus a poll or two, with the source thread's stack first. Released, the thread beats
//      again, the pipeline stops cleanly, and no second report was filed.
//   3. THE DSP THREAD WEDGED IN A PLUGIN: an I/Q decoder plugin whose process() never returns is reported as `dsp`, phase
//      `plugin-iq` - the step the thread was in, not just "the DSP thread" - and, released, the pipeline carries on.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <atomic>
#include <chrono>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "core/hang_watchdog.hpp"
#include "core/pipeline.hpp"
#include "core/plugin_runner.hpp"
#include "core/thread_beats.hpp"
#include "source/iq_source.hpp"
#include "test_check.hpp"

#if defined(_WIN32)
#include <windows.h>
#endif

namespace fs = std::filesystem;
namespace beats = cascade::core::beats;
using cascade::core::HangWatchdog;
using cascade::core::LoadedPlugin;
using cascade::core::PluginRunner;
using Clock = std::chrono::steady_clock;

namespace {

constexpr unsigned kBudgetMs = 1000;

void sleepMs(unsigned ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

bool lineOf(const std::string& text, const std::string& line) {
    return text.find("\n" + line + "\n") != std::string::npos || text.rfind(line + "\n", 0) == 0;
}

// (a developer's hook: THREAD_BEATS_SAMPLE_DIR=<dir> keeps a copy of each report, for the record in docs/engine-stage4.md)
void keepSample(const char* name, const std::string& text) {
    if (const char* keep = std::getenv("THREAD_BEATS_SAMPLE_DIR")) {
        std::ofstream(fs::path(keep) / (std::string("pipeline-wedge-") + name + ".txt"), std::ios::binary) << text;
    }
}

fs::path scratchDir(const std::string& tag) {
    const char* tmp = std::getenv("TEMP");
    if (tmp == nullptr || *tmp == '\0') { tmp = std::getenv("TMPDIR"); }
    const fs::path base = (tmp != nullptr && *tmp != '\0') ? fs::path(tmp) : fs::path(".");
#if defined(_WIN32)
    const unsigned long pid = ::GetCurrentProcessId();
#else
    const unsigned long pid = 0;
#endif
    return base / (std::string("cascade-beatspipe-") + tag + "-" + std::to_string(pid));
}

cascade::core::Pipeline::Config testConfig() {
    cascade::core::Pipeline::Config cfg;
    cfg.sampleRateHz = 2000000.0;
    cfg.fftSize = 1024;
    cfg.averagingAlpha = 1.0f;
    cfg.audioEnabled = false;  // no sound device needed in CI
    return cfg;
}

template <typename Pred>
bool waitFor(Pred pred, int timeoutMs) {
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
    while (Clock::now() < deadline) {
        if (pred()) { return true; }
        sleepMs(5);
    }
    return pred();
}

// A self-paced source that delivers for a while and then stops returning from read() until it is released (or stopped).
class StuckSource : public cascade::source::IqSource {
public:
    explicit StuckSource(int readsBeforeStuck) : budget_(readsBeforeStuck) {}
    bool start() override { running_ = true; return true; }
    void stop() override { release(); running_ = false; }
    bool running() const override { return running_; }
    bool selfPaced() const override { return true; }
    double sampleRateHz() const override { return 1000000.0; }
    bool setSampleRateHz(double) override { return false; }
    double centerFrequencyHz() const override { return 100000000.0; }
    bool setCenterFrequencyHz(double) override { return true; }
    const char* name() const override { return "Stuck test source"; }
    const char* lastError() const override { return ""; }

    std::size_t read(std::complex<float>* dst, std::size_t n) override {
        if (budget_.fetch_sub(1, std::memory_order_relaxed) <= 0) {
            while (!released_.load(std::memory_order_acquire)) { sleepMs(2); }
            budget_.store(1 << 30, std::memory_order_relaxed);
        }
        for (std::size_t i = 0; i < n; ++i) { dst[i] = std::complex<float>(0.0f, 0.0f); }
        sleepMs(2);  // a device's own pacing
        return n;
    }
    void release() { released_.store(true, std::memory_order_release); }

private:
    std::atomic<int> budget_;
    std::atomic<bool> released_{false};
    bool running_ = false;
};

// An I/Q decoder plugin whose process() can be made never to return.
std::atomic<bool> g_blockIq{false};
std::atomic<bool> g_inIq{false};
int g_handle = 0;
void* iqCreate(double, double) { return &g_handle; }
void iqProcess(void*, const float*, std::size_t) {
    if (g_blockIq.load(std::memory_order_acquire)) {
        g_inIq.store(true, std::memory_order_release);
        while (g_blockIq.load(std::memory_order_acquire)) { sleepMs(2); }
        g_inIq.store(false, std::memory_order_release);
    }
}
void iqDestroy(void*) {}
int32_t noText(void*, char*, std::size_t) { return 0; }

}  // namespace

int main() {
    auto& reg = beats::Registry::instance();
    const std::set<std::string> dspPhases = {"ring", "spectrum", "audio-lock", "taps", "plugin-iq", "patch", "demod",
                                             "plugin-audio", "plugin-chain", "sink"};

    // ------------------------------------------------------------------------------------------------ 1. healthy
    {
        const fs::path dir = scratchDir("healthy");
        std::error_code ec;
        fs::remove_all(dir, ec);
        fs::create_directories(dir);
        HangWatchdog w;
        w.setSuppressionForTest(HangWatchdog::SuppressionForTest::NeverSuppress);
        w.start(dir.string(), 600000);
        reg.setBudgetOverrideForTest(beats::Kind::Dsp, kBudgetMs);
        reg.setBudgetOverrideForTest(beats::Kind::Source, kBudgetMs);
        {
            cascade::core::Pipeline pipeline(testConfig());
            pipeline.setSource(nullptr);  // the built-in generator
            pipeline.start();
            cascade::core::SpectrumFrame frame;
            CHECK(waitFor([&] { return pipeline.getLatestFrame(frame) || frame.seq > 0; }, 15000));
            std::set<std::string> seenDsp;
            bool sawDsp = false;
            bool sawSource = false;
            const auto until = Clock::now() + std::chrono::milliseconds(3000);
            while (Clock::now() < until) {
                std::istringstream snap(reg.snapshotText());
                std::string line;
                while (std::getline(snap, line)) {
                    // "  name age-ms=N phase=P wait=W"
                    std::istringstream ls(line);
                    std::string name, age, phase, wait;
                    ls >> name >> age >> phase >> wait;
                    if (name == "dsp") {
                        sawDsp = true;
                        seenDsp.insert(phase.substr(phase.find('=') + 1));
                        CHECK(std::atoll(age.c_str() + age.find('=') + 1) < 900);
                    } else if (name == "source") {
                        sawSource = true;
                        CHECK(std::atoll(age.c_str() + age.find('=') + 1) < 900);
                    }
                }
                sleepMs(7);
            }
            CHECK(sawDsp);
            CHECK(sawSource);
            for (const std::string& p : seenDsp) {
                if (dspPhases.count(p) == 0) { std::printf("FAIL the dsp thread says phase \"%s\", which is not one the pipeline says\n", p.c_str()); }
                CHECK(dspPhases.count(p) == 1);
            }
            std::printf("healthy pipeline: dsp phases seen: %zu\n", seenDsp.size());
            CHECK(!seenDsp.empty());
            pipeline.stop();
        }
        CHECK(w.reportsWritten() == 0);  // nothing late in 3 s under 1 s budgets
        w.stop();
        reg.clearOverridesForTest();
        fs::remove_all(dir, ec);
        CHECK(reg.size() == 0);
    }

    // ------------------------------------------------------------------------------------------------ 2. the source thread
    {
        HangWatchdog::resetWedgeFileCountForTest();
        const fs::path dir = scratchDir("source");
        std::error_code ec;
        fs::remove_all(dir, ec);
        fs::create_directories(dir);
        HangWatchdog w;
        w.setSuppressionForTest(HangWatchdog::SuppressionForTest::NeverSuppress);
        w.start(dir.string(), 600000);
        reg.setBudgetOverrideForTest(beats::Kind::Source, kBudgetMs);
        {
            cascade::core::Pipeline pipeline(testConfig());
            auto src = std::make_unique<StuckSource>(20);
            StuckSource* raw = src.get();
            pipeline.setSource(std::move(src));
            const auto t0 = Clock::now();
            pipeline.start();
            CHECK(waitFor([&] { return w.reportsWritten() >= 1; }, 20000));
            const double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
            std::printf("source wedge: report after %.0f ms (budget %u ms)\n", ms, kBudgetMs);
            CHECK(ms >= static_cast<double>(kBudgetMs) - 30.0);
            sleepMs(300);
            const std::string text = readFile(fs::path(w.lastReportPath()));
            keepSample("source", text);
            CHECK(lineOf(text, "kind: hang"));
            CHECK(lineOf(text, "thread: source"));
            CHECK(lineOf(text, "thread-phase: read"));
            CHECK(text.find(", stalled) ---") != std::string::npos);
            CHECK(text.find("(source, stalled)") != std::string::npos);
            CHECK(pipeline.running());  // the session goes on
            raw->release();
            sleepMs(HangWatchdog::kPollMs * 3);
            CHECK(w.reportsWritten() == 1);
            pipeline.stop();
        }
        w.stop();
        reg.clearOverridesForTest();
        fs::remove_all(dir, ec);
    }

    // ------------------------------------------------------------------------------------------------ 3. the DSP thread
    {
        HangWatchdog::resetWedgeFileCountForTest();
        const fs::path dir = scratchDir("dsp");
        std::error_code ec;
        fs::remove_all(dir, ec);
        fs::create_directories(dir);
        HangWatchdog w;
        w.setSuppressionForTest(HangWatchdog::SuppressionForTest::NeverSuppress);
        w.start(dir.string(), 600000);
        reg.setBudgetOverrideForTest(beats::Kind::Dsp, kBudgetMs);

        CascadeIqDecoderApi iq{};
        iq.structSize = static_cast<uint32_t>(sizeof(CascadeIqDecoderApi));
        iq.requiredRateHz = 0.0;
        iq.preferredRateHz = 0.0;
        iq.create = &iqCreate;
        iq.process = &iqProcess;
        iq.retune = nullptr;
        iq.poll_text = &noText;
        iq.destroy = &iqDestroy;
        LoadedPlugin p;
        p.loaded = true;
        p.name = "BlockingIq";
        p.version = "1.0.0";
        p.path = "BlockingIq.dll";
        p.iqDecoder = &iq;
        PluginRunner runner;
        runner.rebuild({p}, 48000.0, 2000000.0, 100000000.0);
        CHECK(runner.activeCount() == 1u);
        {
            cascade::core::Pipeline pipeline(testConfig());
            pipeline.setPluginRunner(&runner);
            pipeline.setSource(nullptr);
            pipeline.start();
            cascade::core::SpectrumFrame frame;
            CHECK(waitFor([&] { return pipeline.getLatestFrame(frame) || frame.seq > 0; }, 15000));
            // the plugin stops returning
            g_blockIq.store(true, std::memory_order_release);
            const auto t0 = Clock::now();
            CHECK(waitFor([&] { return g_inIq.load(std::memory_order_acquire); }, 5000));
            CHECK(waitFor([&] { return w.reportsWritten() >= 1; }, 20000));
            const double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
            std::printf("dsp wedge in a plugin: report after %.0f ms (budget %u ms)\n", ms, kBudgetMs);
            sleepMs(300);
            const std::string text = readFile(fs::path(w.lastReportPath()));
            keepSample("dsp", text);
            CHECK(lineOf(text, "thread: dsp"));
            CHECK(lineOf(text, "thread-phase: plugin-iq"));
            CHECK(text.find("(dsp, stalled)") != std::string::npos);
            CHECK(pipeline.running());
            // released: the thread carries on, beating, and the pipeline stops cleanly
            g_blockIq.store(false, std::memory_order_release);
            cascade::core::SpectrumFrame later;
            const unsigned long long seqThen = frame.seq;
            CHECK(waitFor([&] { pipeline.getLatestFrame(later); return later.seq > seqThen + 3; }, 15000));
            sleepMs(HangWatchdog::kPollMs * 3);
            CHECK(w.reportsWritten() == 1);
            pipeline.stop();
            pipeline.setPluginRunner(nullptr);
        }
        w.stop();
        reg.clearOverridesForTest();
        fs::remove_all(dir, ec);
    }

    return testSummary("test_thread_beats_pipeline");
}
