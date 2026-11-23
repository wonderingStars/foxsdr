// THE ENGINE'S SIDE OF WHERE THE TIME GOES (2026-11-23, the ringdrop measurement; docs/DIAGNOSTICS.md).
//
// tests/test_dsp_phase_time.cpp proves the pipeline's half with a real-time source and a seam that burns microseconds in a chosen phase of every
// block. This test drives the same through the real engine (an AppWindow's, as tests/test_dsp_drop_minute_app.cpp does), at 2 MS/s with a block of
// 1024 samples lasting 512 microseconds, and pins what the minute says:
//
//   2. a loss minute (the demod costs more than the block lasts) writes the three accounting lines, a "dsp minute: phase_ms=" line that names
//      the loaded phase first, and the ringdrop report is FILED;
//   4. the BASELINE line is said once, 15 s after the first drain, and again after an input-rate change;
//   5. the machine line carries the processor clock ("clock_pct=N" or "clock_pct=unknown", a percentage, never MHz) and the memory words, and fits the log's line;
//   6. every line fits the log's line with its stamp and survives the upload scrub with every number intact.
//
// (This file was test_dsp_shed_app.cpp; its sections about shedding and the "not keeping up" note were removed with them, review 2026-11-23.)
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <atomic>
#include <chrono>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
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

#include "core/crash_handler.hpp"
#include "core/diag_log.hpp"
#include "core/diag_report.hpp"
#include "core/dsp_baseline.hpp"
#include "core/issue_report.hpp"
#include "core/machine_state.hpp"
#include "core/pipeline.hpp"
#include "engine/engine.hpp"
#include "gui/app_window.hpp"
#include "test_host.hpp"
#include "pass_seam.hpp"
#include "source/iq_source.hpp"
#include "test_check.hpp"

namespace fs = std::filesystem;

namespace {

void setEnv(const char* name, const std::string& value) {
#if defined(_WIN32)
    _putenv_s(name, value.c_str());
    SetEnvironmentVariableA(name, value.c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}

fs::path g_scratch;

void isolate() {
#if defined(_WIN32)
    const int pid = _getpid();
#else
    const int pid = static_cast<int>(getpid());
#endif
    g_scratch = fs::temp_directory_path() / ("foxsdr_dsp_phase_time_app_" + std::to_string(pid));
    std::error_code ec;
    fs::remove_all(g_scratch, ec);
    fs::create_directories(g_scratch, ec);
    const std::string s = g_scratch.string();
    for (const char* v : {"LOCALAPPDATA", "APPDATA", "USERPROFILE", "HOME", "XDG_CONFIG_HOME", "XDG_STATE_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME"}) {
        setEnv(v, s);
    }
    setEnv("FOXSDR_TELEMETRY_URL", "http://127.0.0.1:9");
}

std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// A device that delivers in REAL TIME (as tests/test_dsp_phase_time.cpp's).
class PacedSource final : public cascade::source::IqSource {
public:
    bool start() override { return true; }
    void stop() override {}
    bool running() const override { return true; }
    bool selfPaced() const override { return true; }
    double sampleRateHz() const override { return 2000000.0; }
    bool setSampleRateHz(double) override { return false; }
    double centerFrequencyHz() const override { return 100.0e6; }
    bool setCenterFrequencyHz(double) override { return true; }
    const char* name() const override { return "Paced test source"; }
    const char* lastError() const override { return ""; }
    std::size_t read(std::complex<float>* dst, std::size_t n) override {
        if (!started_) {
            started_ = true;
            begin_ = std::chrono::steady_clock::now();
        }
        delivered_ += n;
        std::this_thread::sleep_until(begin_ + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                                   std::chrono::duration<double>(static_cast<double>(delivered_) / 2000000.0)));
        for (std::size_t i = 0; i < n; ++i) { dst[i] = std::complex<float>(0.25f, -0.25f); }
        return n;
    }

private:
    bool started_ = false;
    std::chrono::steady_clock::time_point begin_{};
    std::uint64_t delivered_ = 0;
};

template <typename Pred>
bool waitFor(Pred pred, int timeoutMs = 10000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) { return true; }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return pred();
}

std::vector<std::string> linesWith(const char* needle) {
    std::vector<std::string> out;
    for (const std::string& line : cascade::core::DiagLog::instance().ringSnapshot()) {
        if (line.find(needle) != std::string::npos) { out.push_back(line); }
    }
    return out;
}

long long numberAfter(const std::string& line, const char* key) {
    const std::size_t at = line.find(key);
    return at == std::string::npos ? -1 : std::atoll(line.c_str() + at + std::strlen(key));
}

}  // namespace

namespace cascade::gui {

struct AppWindowTestAccess {
    static cascade::core::Pipeline& pipeline(AppWindow& a) { return th::engine(a).pipeline_; }
    static void closeDspMinute(AppWindow& a) { passseam::on(th::engine(a), [&] { th::engine(a).closeDspMinute(); }); }
    static void pollDspMinute(AppWindow& a) { passseam::on(th::engine(a), [&] { th::engine(a).pollDspMinute(); }); }
    static void ageSession(AppWindow& a, double seconds) {
        passseam::on(th::engine(a), [&] {
            th::engine(a).clockOrigin_ -= std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(seconds));
        });
    }
    static void unarm(AppWindow& a) {
        passseam::on(th::engine(a), [&] {
            th::engine(a).rx().dspMinuteArmed_ = false;
            th::engine(a).rx().firstDrainSeenS_ = -1.0;
            th::engine(a).rx().baselineStage_ = 0;
        });
    }
};

}  // namespace cascade::gui

using Access = cascade::gui::AppWindowTestAccess;

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    isolate();
    using cascade::core::Pipeline;

    // CAPTURE ON, so a loss minute that files the `ringdrop` report writes a file this test can count.
    const fs::path reports = g_scratch / "reports";
    {
        std::error_code ec;
        fs::create_directories(reports, ec);
        cascade::core::CrashHandlerConfig on;
        on.crashDir = reports.string();
        on.enabled = true;
        on.minidump = false;
        on.exitAfterReport = false;
        cascade::core::installCrashHandlers(on);
        cascade::core::issue::resetIssueReportsForTest();
    }
    auto ringdropReports = [&] {
        std::size_t n = 0;
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(reports, ec)) {
            const std::string name = e.path().filename().string();
            if (name.rfind("issue-", 0) != 0 || name.size() < 4 || name.compare(name.size() - 4, 4, ".txt") != 0) { continue; }
            if (readFile(e.path()).find("reason: issue: ringdrop - ") != std::string::npos) { ++n; }
        }
        return n;
    };

    th::TestWindow app;
    Pipeline& p = Access::pipeline(app);
    p.setSource(std::make_unique<PacedSource>());
    p.start();
    CHECK(p.running());

    // THE MARKS: a minute with nothing in it, so what follows is this test's own.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    Access::closeDspMinute(app);

    // --- 2. A LOSS MINUTE: counted, accounted per phase, filed --------------------------------------------------------------------
    std::vector<std::string> accounting;
    {
        const std::size_t first0 = linesWith("dsp minute: dropped=").size();
        Pipeline::setDspPhaseLoadForTest("demod", 1200);
        std::this_thread::sleep_for(std::chrono::milliseconds(3000));
        Access::closeDspMinute(app);
        const std::vector<std::string> a = linesWith("dsp minute: dropped=");
        const std::vector<std::string> ph = linesWith("dsp minute: phase_ms=");
        CHECK(a.size() == first0 + 1u);
        CHECK(ph.size() == 1u);
        if (!a.empty()) {
            std::printf("minute line A: %s\n", a.back().c_str());
            CHECK(numberAfter(a.back(), "dropped=") > 0);
            accounting.push_back(a.back());
        }
        if (!ph.empty()) {
            std::printf("phase line: %s\n", ph.back().c_str());
            CHECK(ph.back().find("dsp minute: phase_ms=demod:") != std::string::npos);   // the loaded phase, first
            CHECK(numberAfter(ph.back(), "phase_ms=demod:") >= 1000);                     // a minute of 3 s with 1.2 ms in most blocks
            accounting.push_back(ph.back());
        }
        CHECK(ringdropReports() == 1u);
    }

    Pipeline::setDspPhaseLoadForTest("demod", 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));   // the load is gone: the ring drains
    Access::closeDspMinute(app);
    Access::closeDspMinute(app);

    // --- 4. THE BASELINE: once, 15 s after the first drain; again after a rate change ---------------------------------------------------------
    {
        Pipeline::setDspPhaseLoadForTest("demod", 200);   // a known cost in a known phase
        Access::unarm(app);
        const std::size_t b0 = linesWith("dsp baseline:").size();
        Access::pollDspMinute(app);   // sees the stream: the digest clock arms
        Access::pollDspMinute(app);   // the baseline arms from the first drain
        CHECK(linesWith("dsp baseline:").size() == b0);
        Access::ageSession(app, 10.5);
        Access::pollDspMinute(app);   // the marks
        CHECK(linesWith("dsp baseline:").size() == b0);
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));   // a stretch of blocks between the marks and the line
        Access::ageSession(app, 5.0);
        Access::pollDspMinute(app);
        const std::vector<std::string> b = linesWith("dsp baseline:");
        CHECK(b.size() == b0 + 1u);
        if (b.size() == b0 + 1u) {
            std::printf("baseline line: %s\n", b.back().c_str());
            CHECK(b.back().find("dsp baseline: rate=2000000 block=") != std::string::npos);
            const long long block = numberAfter(b.back(), " block=");
            CHECK(block == static_cast<long long>(p.dspBlockSamples()));
            CHECK(numberAfter(b.back(), " budget_us=") == static_cast<long long>(1.0e6 * static_cast<double>(block) / 2.0e6 + 0.5));
            CHECK(numberAfter(b.back(), " cost_us=") >= 200);   // the 200 microseconds burned in every block
            CHECK(b.back().find(" headroom=") != std::string::npos);
            CHECK(b.back().find(" top=demod:") != std::string::npos);
            accounting.push_back(b.back());
        }
        Access::pollDspMinute(app);
        Access::ageSession(app, 20.0);
        Access::pollDspMinute(app);
        CHECK(linesWith("dsp baseline:").size() == b0 + 1u);   // once for this rate
        // A NEW RATE: said again, with the new rate and the new budget.
        CHECK(p.setInputRateHz(1000000.0));
        Access::pollDspMinute(app);        // sees the new rate, arms again
        Access::ageSession(app, 10.5);
        Access::pollDspMinute(app);        // the marks
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        Access::ageSession(app, 5.0);
        Access::pollDspMinute(app);
        const std::vector<std::string> b2 = linesWith("dsp baseline:");
        CHECK(b2.size() == b0 + 2u);
        if (b2.size() == b0 + 2u) {
            std::printf("baseline line after the rate change: %s\n", b2.back().c_str());
            CHECK(b2.back().find("dsp baseline: rate=1000000 block=") != std::string::npos);
            CHECK(numberAfter(b2.back(), " budget_us=") == static_cast<long long>(1.0e6 * static_cast<double>(p.dspBlockSamples()) / 1.0e6 + 0.5));
            accounting.push_back(b2.back());
        }
        Pipeline::setDspPhaseLoadForTest("demod", 0);
    }

    // --- 5. THE MACHINE LINE CARRIES THE CLOCK AND THE MEMORY, and they fit --------------------------------------------------------------
    {
        const std::size_t before = linesWith("machine:").size();
        cascade::core::resetMachineStateLogForTest();
        cascade::core::logMachineStateIfChanged("above-normal: raised", "above-normal: raised", true);
        const std::vector<std::string> ml = linesWith("machine:");
        std::string joined;
        for (std::size_t i = before; i < ml.size(); ++i) {
            std::printf("%s\n", ml[i].c_str());
            joined += ml[i] + "\n";
            CHECK(ml[i].size() < static_cast<std::size_t>(cascade::core::DiagLog::kLineBytes - 1));
            CHECK(cascade::core::scrubUploadLine(ml[i]) == ml[i]);
        }
        const std::size_t at = joined.find(" clock_pct=");
        CHECK(at != std::string::npos);
        const std::string clock = at == std::string::npos ? std::string() : joined.substr(at + 11);
        CHECK(clock.rfind("unknown", 0) == 0 || std::atoi(clock.c_str()) > 0);
        CHECK(joined.find("MHz") == std::string::npos && joined.find("mhz") == std::string::npos);
        const long long load = numberAfter(joined, " mem_load=");
        const long long avail = numberAfter(joined, " mem_avail_mb=");
        const long long commit = numberAfter(joined, " commit_mb=");
        const std::size_t slash = joined.find('/', joined.find(" commit_mb="));
        const long long limit = slash == std::string::npos ? -1 : std::atoll(joined.c_str() + slash + 1);
        if (joined.find("mem_load=unknown") == std::string::npos) {
            CHECK(load >= 1 && load <= 100);
            CHECK(avail > 0);
            CHECK(commit > 0 && limit >= commit);
        }
        CHECK(joined.find("mem_load=") != std::string::npos && joined.find("mem_avail_mb=") != std::string::npos && joined.find("commit_mb=") != std::string::npos);
        // THE SECOND LINE ONLY WHEN A VALUE CHANGED: a loss minute asks without forcing, and with the steady words and the clock and memory load as they
        // were just said nothing more is written (it used to say both machine lines in every loss minute: 256 lines of ring, ten per minute).
        const std::size_t after = linesWith("machine:").size();
        for (int i = 0; i < 3; ++i) { cascade::core::logMachineStateIfChanged("above-normal: raised", "above-normal: raised", false); }
        CHECK(linesWith("machine:").size() == after);
    }

    // --- 5b. A LOSS MINUTE SAYS HOW THE PROCESS FARED IN MEMORY; A DEGRADED MACHINE SAYS ITS BASELINE AGAIN ------------------------------------------
    {
        Access::closeDspMinute(app);
        const std::size_t baselines0 = linesWith("dsp baseline:").size();
        const std::size_t again0 = linesWith("dsp baseline (again, cost doubled):").size();
        const std::size_t mem0 = linesWith("dsp minute: page_faults=").size();
        Pipeline::setDspPhaseLoadForTest("demod", 1500);   // far more than double the 200-microsecond baseline of section 4
        std::this_thread::sleep_for(std::chrono::milliseconds(3000));
        Pipeline::setDspPhaseLoadForTest("demod", 0);
        Access::closeDspMinute(app);
        const std::vector<std::string> mem = linesWith("dsp minute: page_faults=");
        CHECK(mem.size() == mem0 + 1u);
        if (!mem.empty()) {
            std::printf("memory line: %s\n", mem.back().c_str());
            const long long ws = numberAfter(mem.back(), " working_set_mb=");
            const long long load = numberAfter(mem.back(), " mem_load=");
            if (mem.back().find("page_faults=unknown") == std::string::npos) {
                CHECK(numberAfter(mem.back(), "page_faults=") >= 0);
                CHECK(ws > 0);
            }
            if (mem.back().find("mem_load=unknown") == std::string::npos) { CHECK(load >= 1 && load <= 100); }
            CHECK(mem.back().size() < static_cast<std::size_t>(cascade::core::DiagLog::kLineBytes - 1));
            CHECK(cascade::core::scrubUploadLine(mem.back()) == mem.back());
            accounting.push_back(mem.back());
        }
        const std::vector<std::string> b = linesWith("dsp baseline (again, cost doubled):");
        CHECK(b.size() == again0 + 1u);   // said again in this first loss minute whose cost more than doubled, and MARKED as the repeat
        CHECK(linesWith("dsp baseline:").size() == baselines0);   // ...so the plain line is still only the first baselines
        if (!b.empty()) {
            std::printf("baseline again: %s\n", b.back().c_str());
            CHECK(numberAfter(b.back(), " cost_us=") >= 1000);
            CHECK(b.back().find(" headroom=0.") != std::string::npos);   // below 1: the case that most needs to be seen
        }
        // ...and only once per session
        Pipeline::setDspPhaseLoadForTest("demod", 1500);
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        Pipeline::setDspPhaseLoadForTest("demod", 0);
        Access::closeDspMinute(app);
        CHECK(linesWith("dsp baseline (again, cost doubled):").size() == again0 + 1u);
    }

    // --- 6. THE NEW LINES AT THEIR WIDEST fit the log's line with its stamp and survive the scrub --------------------------------------
    {
        const auto fits = [](const std::string& text) {
            const std::string logged = "12:34:56.789 info " + text;
            return logged.size() < static_cast<std::size_t>(cascade::core::DiagLog::kLineBytes - 1) && cascade::core::scrubUploadLine(logged) == logged;
        };
        for (const std::string& l : accounting) {
            // as the log holds them: stamp and level in front; a real line of this run
            CHECK(l.size() < static_cast<std::size_t>(cascade::core::DiagLog::kLineBytes - 1));
            CHECK(cascade::core::scrubUploadLine(l) == l);
        }
        char a[256];
        std::snprintf(a, sizeof(a), "dsp minute: dropped=%llu pct=%s len_ms=%llu ring_high_pct=%llu events=%s [radio 8]", 9999999999ULL, "100.00",
                      9999999999ULL, 100ULL, "rate|switch|plugins|audio");
        // a phase line with every phase over 1% and the widest numbers, cut to what fits
        std::uint64_t ns[Pipeline::kDspPhases];
        const char* names[Pipeline::kDspPhases];
        for (unsigned i = 0; i < Pipeline::kDspPhases; ++i) {
            ns[i] = (1000000000000ull) - i * 1000ull;
            names[i] = Pipeline::dspPhaseName(i);
        }
        const std::string phases = cascade::core::dspPhaseMsText(ns, names, Pipeline::kDspPhases,
                                                                 cascade::core::kDspLineTextBudget - 21u - cascade::core::kDspRadioTagBudget);
        const std::string ph = "dsp minute: phase_ms=" + phases + " [radio 8]";
        const std::string base = cascade::core::dspBaselineText(192000000.0, 1048576, 123456789ull, 9999999999999ull, "plugin-audio:100,plugin-chain:100") + " [radio 8]";
        std::printf("widest: A %zu, phase %zu, baseline %zu characters\n", std::strlen(a), ph.size(), base.size());
        CHECK(fits(a));
        CHECK(fits(ph));
        CHECK(fits(base));
    }

    std::error_code gone;
    fs::remove_all(g_scratch, gone);
    return testSummary("test_dsp_phase_time_app");
}
