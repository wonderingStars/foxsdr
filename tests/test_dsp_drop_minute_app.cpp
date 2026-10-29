// SAMPLES LOST BETWEEN THE SOURCE AND THE DSP, SAID IN THE LOG ONCE A MINUTE
// (GitHub issue 6, 0.99.86).
//
// THE FIELD REPORT. A tester hears crackle from an RSPduo at 4 MS/s. His 0.99.85
// log shows the SDRplay service streaming cleanly through steady minutes - no
// service gaps, no link-ring overflows, no starved audio - which leaves one place
// samples could still be lost unseen: the pipeline's own ring between its source
// thread and its DSP thread. That ring drops what does not fit and counts it
// (Pipeline::ringDroppedSamples), but the count reached only the crash-report
// health events, never the log or the bundle a tester sends.
//
// WHAT IS PINNED HERE, through the real AppWindow:
//   1. a minute in which nothing was dropped writes NOTHING, however much was read;
//   2. a minute in which the DSP side stalled writes ONE line, with the pipeline's
//      own count of what it dropped and that count as a share of the minute's input;
//   3. the next clean minute is silent again (the minute is a difference, not a total);
//   4. the diagnostics bundle carries the running total and the samples read;
//   5. the frame loop closes the minute once a minute, on a clock of its own - not
//      inside the audio watchdog, which returns early on a machine with no sound
//      output, where a ring can still overflow.
//
// HOW THE CONDITION IS MADE. The source is the test's own: self-paced, it hands
// the pipeline exactly the samples the test budgets and then returns nothing, so
// "how many samples were read" has one answer whatever the scheduler does. The
// stall is the pipeline's own seam, Pipeline::holdLockForTest(Audio): the DSP
// thread takes that mutex for every block, so while the test holds it the DSP
// thread reads at most one more block and stops, and the source overruns the
// ring. The minute is closed by hand (AppWindow::closeDspMinute takes no clock).
//
// Hermetic like test_audio_lead_app: no config file, the per-user directories in
// a scratch folder, telemetry pointed at a black hole.
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

#include "core/crash_handler.hpp"
#include "core/diag_log.hpp"
#include "core/diag_report.hpp"
#include "core/issue_report.hpp"
#include "core/pipeline.hpp"
#include "gui/app_window.hpp"
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
    g_scratch = fs::temp_directory_path() / ("foxsdr_dsp_drop_minute_app_" + std::to_string(pid));
    std::error_code ec;
    fs::remove_all(g_scratch, ec);
    fs::create_directories(g_scratch, ec);
    const std::string s = g_scratch.string();
    for (const char* v : {"LOCALAPPDATA", "APPDATA", "USERPROFILE", "HOME", "XDG_CONFIG_HOME",
                          "XDG_STATE_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME"}) {
        setEnv(v, s);
    }
    setEnv("FOXSDR_TELEMETRY_URL", "http://127.0.0.1:9");
}

std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// `text` without its comments, as test_audio_lead_app strips them: a call that is
// commented out must not satisfy a scan for the call.
std::string stripComments(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size();) {
        if (text.compare(i, 2, "//") == 0) {
            while (i < text.size() && text[i] != '\n') { ++i; }
        } else if (text.compare(i, 2, "/*") == 0) {
            const std::size_t close = text.find("*/", i + 2);
            i = close == std::string::npos ? text.size() : close + 2;
            out += ' ';
        } else {
            out += text[i++];
        }
    }
    return out;
}

const char* kNeedle = "samples dropped between the source and the DSP in the last minute";
// The second line a lost minute writes (GitHub issue 6, the 0.99.90 report): what
// the loss looked like.
const char* kRingNeedle = "dsp: ring high water ";

// The log lines that are the minute's drop line.
std::vector<std::string> dropLines() {
    std::vector<std::string> out;
    for (const std::string& line : cascade::core::DiagLog::instance().ringSnapshot()) {
        if (line.find(kNeedle) != std::string::npos) { out.push_back(line); }
    }
    return out;
}

// ...and the lines that are its second line.
std::vector<std::string> ringLines() {
    std::vector<std::string> out;
    for (const std::string& line : cascade::core::DiagLog::instance().ringSnapshot()) {
        if (line.find(kRingNeedle) != std::string::npos) { out.push_back(line); }
    }
    return out;
}

// The number that follows `key` in `line`, or -1.
long long numberAfter(const std::string& line, const char* key) {
    const std::size_t at = line.find(key);
    if (at == std::string::npos) { return -1; }
    return std::atoll(line.c_str() + at + std::strlen(key));
}

// Self-paced, and only as generous as the test lets it be: each read hands over
// up to the budget the test granted and then nothing (the pipeline retries an
// empty read after 1 ms, as it does for a real device with nothing to deliver).
class BudgetSource final : public cascade::source::IqSource {
public:
    bool start() override { return true; }
    void stop() override {}
    bool running() const override { return true; }
    bool selfPaced() const override { return true; }
    double sampleRateHz() const override { return 2000000.0; }
    bool setSampleRateHz(double) override { return false; }
    double centerFrequencyHz() const override { return 100.0e6; }
    bool setCenterFrequencyHz(double) override { return true; }
    const char* name() const override { return "Budgeted test source"; }
    const char* lastError() const override { return ""; }

    std::size_t read(std::complex<float>* dst, std::size_t n) override {
        std::size_t give = 0;
        {
            std::lock_guard<std::mutex> lk(m_);
            give = static_cast<std::size_t>(std::min<std::uint64_t>(budget_, n));
            budget_ -= give;
        }
        for (std::size_t i = 0; i < give; ++i) { dst[i] = std::complex<float>(0.25f, -0.25f); }
        delivered_.fetch_add(give, std::memory_order_acq_rel);
        return give;
    }

    void grant(std::uint64_t samples) {
        std::lock_guard<std::mutex> lk(m_);
        budget_ += samples;
    }
    std::uint64_t delivered() const { return delivered_.load(std::memory_order_acquire); }

private:
    std::mutex m_;
    std::uint64_t budget_ = 0;
    std::atomic<std::uint64_t> delivered_{0};
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

// Waits until the DSP thread has drained the ring: with the source giving
// nothing, the spectrum stops advancing once the ring holds less than a block.
bool drained(cascade::core::Pipeline& p) {
    cascade::core::SpectrumFrame f;
    std::uint64_t lastSeq = ~0ULL;
    return waitFor([&] {
        p.getLatestFrame(f);
        const bool still = f.seq == lastSeq;
        lastSeq = f.seq;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        return still;
    });
}

// The share the line must print, worked out here rather than by the code under
// test: two decimals of a percent.
std::string expectedShare(std::uint64_t dropped, std::uint64_t read) {
    char buf[32] = {};
    std::snprintf(buf, sizeof(buf), "%.2f%%",
                  100.0 * static_cast<double>(dropped) / static_cast<double>(read));
    return buf;
}

}  // namespace

namespace cascade::gui {

struct AppWindowTestAccess {
    static cascade::core::Pipeline& pipeline(AppWindow& a) { return a.pipeline_; }
    static void closeDspMinute(AppWindow& a) { a.closeDspMinute(); }
    static std::string bundle(AppWindow& a) { return a.currentDiagnosticsBundle(); }
};

}  // namespace cascade::gui

using Access = cascade::gui::AppWindowTestAccess;

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    isolate();

    // --- 0. THE SHARE AS A SENTENCE, by itself ----------------------------------------
    {
        using cascade::core::droppedShareText;
        CHECK(droppedShareText(412000, 240000000) == "0.17%");
        CHECK(droppedShareText(1, 2) == "50.00%");
        CHECK(droppedShareText(0, 1000) == "0.00%");
        // Something lost, but less than two decimals can show: never "0.00%".
        CHECK(droppedShareText(1, 240000000) == "under 0.01%");
        // Nothing read: no share to give (the counters were read a moment apart).
        CHECK(droppedShareText(5, 0).empty());
        // More dropped than read cannot happen, but two counters read a moment apart
        // can say so; the share is capped rather than printed as 150%.
        CHECK(droppedShareText(3, 2) == "100.00%");
        // THE LONGEST LINE the minute can write fits the log's line (DiagLog::kLineBytes,
        // NUL included): a cut line is masked whole by the upload scrub.
        char longest[cascade::core::DiagLog::kLineBytes * 2] = {};
        std::snprintf(longest, sizeof(longest), "dsp: %llu %s (%s)",
                      18446744073709551615ULL, kNeedle,
                      droppedShareText(18446744073709551615ULL, 18446744073709551615ULL).c_str());
        std::printf("the longest minute line is %zu characters\n", std::strlen(longest));
        CHECK(std::strlen(longest) < static_cast<std::size_t>(cascade::core::DiagLog::kLineBytes - 1));
        // ...AND THE SECOND LINE a lost minute writes, at its widest: the share is at
        // most three digits, the other three figures uint64.
        char widest[cascade::core::DiagLog::kLineBytes * 2] = {};
        std::snprintf(widest, sizeof(widest),
                      "%s100%% of %llu samples, %llu writes cut, longest DSP block %llu ms", kRingNeedle,
                      18446744073709551615ULL, 18446744073709551615ULL, 18446744073709551615ULL);
        std::printf("the longest second line is %zu characters\n", std::strlen(widest));
        CHECK(std::strlen(widest) < static_cast<std::size_t>(cascade::core::DiagLog::kLineBytes - 1));
    }

    // --- 0b. THE SEVERITY FLOOR FOR A FEED ROW (0.99.102), by itself -------------------
    // The feed's first day of `ringdrop` rows: six start-of-stream blips of a few
    // milliseconds, "under 0.01%". A minute is worth a row at 0.05% of its input.
    {
        using cascade::core::kRingDropReportShare;
        using cascade::core::ringDropWorthAReport;
        const std::uint64_t minute = 2048000ULL * 60;          // 2.048 MS/s for a minute
        CHECK(kRingDropReportShare == 0.0005);
        CHECK(!ringDropWorthAReport(7168, minute));            // the field blip: 3.5 ms
        CHECK(!ringDropWorthAReport(1, minute));
        CHECK(!ringDropWorthAReport(0, minute));               // nothing lost
        CHECK(!ringDropWorthAReport(5, 0));                    // nothing read: no share
        CHECK(!ringDropWorthAReport(0, 0));
        const std::uint64_t floor = minute / 2000;             // exactly 0.05%
        CHECK(!ringDropWorthAReport(floor - 1, minute));
        CHECK(ringDropWorthAReport(floor, minute));            // the boundary is in
        CHECK(ringDropWorthAReport(848000, minute));           // GitHub issue 6's 0.69%
        CHECK(ringDropWorthAReport(3, 2));                     // more dropped than read
    }

    // CAPTURE ON, so a minute that files the `ringdrop` report writes a file this test
    // can count (core/issue_report.hpp: capture off files nothing, and says nothing).
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
        CHECK(!cascade::core::activeCrashDir().empty());
        cascade::core::issue::resetIssueReportsForTest();
    }
    auto ringdropReports = [&] {
        std::size_t n = 0;
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(reports, ec)) {
            const std::string name = e.path().filename().string();
            if (name.rfind("issue-", 0) != 0 || name.size() < 4 ||
                name.compare(name.size() - 4, 4, ".txt") != 0) {
                continue;
            }
            if (readFile(e.path()).find("reason: issue: ringdrop - ") != std::string::npos) { ++n; }
        }
        return n;
    };

    cascade::gui::AppWindow app;
    cascade::core::Pipeline& p = Access::pipeline(app);
    auto owned = std::make_unique<BudgetSource>();
    BudgetSource* src = owned.get();
    p.setSource(std::move(owned));
    p.start();
    CHECK(p.running());
    const std::size_t ring = p.ringCapacity();
    std::printf("ring capacity %zu samples\n", ring);

    // A minute with nothing in it sets the marks: whatever the window did before the
    // test took the pipeline is not this test's minute.
    Access::closeDspMinute(app);
    const std::size_t linesAtStart = dropLines().size();
    const std::size_t ringLinesAtStart = ringLines().size();

    // --- 1. A CLEAN MINUTE: samples read, none dropped - nothing written ----------------
    {
        const std::uint64_t readBefore = p.sourceSamplesRead();
        const std::uint64_t droppedBefore = p.ringDroppedSamples();
        // Less than the ring holds, so not one can be dropped even if the DSP thread
        // never ran at all.
        const std::uint64_t grant = ring / 2;
        src->grant(grant);
        CHECK(waitFor([&] { return src->delivered() >= grant; }));
        CHECK(waitFor([&] { return p.sourceSamplesRead() - readBefore >= grant; }));
        CHECK(p.sourceSamplesRead() - readBefore == grant);
        CHECK(p.ringDroppedSamples() == droppedBefore);
        Access::closeDspMinute(app);
        CHECK(dropLines().size() == linesAtStart);
        CHECK(ringLines().size() == ringLinesAtStart);   // no second line without a first
        CHECK(ringdropReports() == 0);                   // and no feed row
    }

    // --- 2. THE DSP SIDE STALLS: one line, the pipeline's own count, its share ----------
    std::uint64_t stalledDropped = 0;
    {
        CHECK(drained(p));   // the stall starts from an (all but) empty ring
        const std::uint64_t readBefore = p.sourceSamplesRead();
        const std::uint64_t droppedBefore = p.ringDroppedSamples();
        std::atomic<bool> acquired{false};
        std::atomic<bool> released{false};
        std::thread holder([&] {
            p.holdLockForTest(cascade::core::Pipeline::LockForTest::Audio, 3000, &acquired);
            released.store(true, std::memory_order_release);
        });
        CHECK(waitFor([&] { return acquired.load(std::memory_order_acquire); }));
        // Three rings' worth: the DSP thread can take at most a block or two before it
        // stops at the held mutex, so most of this has nowhere to go.
        const std::uint64_t grant = 3 * static_cast<std::uint64_t>(ring);
        src->grant(grant);
        CHECK(waitFor([&] { return p.sourceSamplesRead() - readBefore >= grant; }));
        // ...all of it read WHILE the DSP side was still held, or the stall did not
        // stage what it claims to.
        CHECK(!released.load(std::memory_order_acquire));
        holder.join();
        // The source has nothing more to give, so both counts are final.
        const std::uint64_t read = p.sourceSamplesRead() - readBefore;
        stalledDropped = p.ringDroppedSamples() - droppedBefore;
        std::printf("stalled minute: %llu read, %llu dropped\n",
                    static_cast<unsigned long long>(read),
                    static_cast<unsigned long long>(stalledDropped));
        CHECK(read == grant);
        CHECK(stalledDropped > 0);
        // The ring keeps a whole ring (every slot is usable) less the part of a block
        // the drain left in it, plus whatever the DSP thread took before it reached
        // the held mutex - at most one FFT block each way, and no FFT this program
        // builds is longer than 65536.
        CHECK(stalledDropped <= grant - ring + 65536);
        CHECK(stalledDropped >= grant - ring - 65536);
        // The block that waited on the held mutex is recorded when it ENDS, on the
        // DSP thread, which the holder's release has only just let go: give it the
        // moment it needs before the minute is closed, or its 3 s lands in the next.
        CHECK(waitFor([&] { return p.dspBlockLongestNs() >= 2500000000ULL; }));

        Access::closeDspMinute(app);
        const std::vector<std::string> lines = dropLines();
        CHECK(lines.size() == linesAtStart + 1);
        if (!lines.empty()) {
            const std::string& line = lines.back();
            std::printf("the line: %s\n", line.c_str());
            const std::string want = "dsp: " + std::to_string(stalledDropped) + " " + kNeedle + " (" +
                                     expectedShare(stalledDropped, read) + ")";
            CHECK(line.find(want) != std::string::npos);
            CHECK(line.size() < static_cast<std::size_t>(cascade::core::DiagLog::kLineBytes - 1));
        }
        // THE SECOND LINE (GitHub issue 6, the 0.99.90 report): the ring filled to
        // the brim while the DSP thread was held, so its high water is 100% of the
        // ring's own capacity; at least one write was cut; and the longest DSP block
        // is the block that waited on the held mutex - the 3 s hold, give or take
        // the scheduler, and nothing like the microseconds a block normally takes.
        const std::vector<std::string> ring2 = ringLines();
        CHECK(ring2.size() == ringLinesAtStart + 1);
        if (!ring2.empty()) {
            const std::string& line = ring2.back();
            std::printf("the second line: %s\n", line.c_str());
            CHECK(line.find(std::string(kRingNeedle) + "100% of " + std::to_string(ring) + " samples, ") !=
                  std::string::npos);
            CHECK(numberAfter(line, " samples, ") >= 1);   // writes cut
            const long long blockMs = numberAfter(line, "longest DSP block ");
            CHECK(blockMs >= 2500);
            CHECK(blockMs <= 10000);
            CHECK(line.size() < static_cast<std::size_t>(cascade::core::DiagLog::kLineBytes - 1));
        }
        // THE FEED ROW (0.99.102): a minute that lost two rings of three is far over
        // the 0.05% floor, so the `ringdrop` issue report is filed - by this minute
        // close, not by the health poll, which now only counts.
        CHECK(cascade::core::ringDropWorthAReport(stalledDropped, read));
        CHECK(ringdropReports() == 1);
    }

    // --- 3. THE NEXT CLEAN MINUTE IS SILENT AGAIN ---------------------------------------
    {
        // Let the DSP thread drain what the stall left in the ring. Only then is half
        // a ring certain to fit.
        CHECK(drained(p));
        const std::uint64_t readBefore = p.sourceSamplesRead();
        const std::uint64_t droppedBefore = p.ringDroppedSamples();
        const std::uint64_t grant = ring / 2;
        src->grant(grant);
        CHECK(waitFor([&] { return p.sourceSamplesRead() - readBefore >= grant; }));
        CHECK(p.sourceSamplesRead() - readBefore == grant);
        CHECK(p.ringDroppedSamples() == droppedBefore);
        Access::closeDspMinute(app);
        CHECK(dropLines().size() == linesAtStart + 1);
        CHECK(ringLines().size() == ringLinesAtStart + 1);   // and no second line either
        // ...and a minute with nothing at all in it is silent too.
        Access::closeDspMinute(app);
        CHECK(dropLines().size() == linesAtStart + 1);
        CHECK(ringdropReports() == 1);                   // once a session, and no new row
    }

    // --- 4. THE BUNDLE carries the running total and what was read ----------------------
    {
        p.stop();   // both counters final for the comparison
        const std::string bundle = Access::bundle(app);
        const std::uint64_t dropped = p.ringDroppedSamples();
        const std::uint64_t read = p.sourceSamplesRead();
        const std::string want = "\ndsp-dropped: " + std::to_string(dropped) + " of " +
                                 std::to_string(read) + " samples (" +
                                 cascade::core::droppedShareText(dropped, read) + ")\n";
        std::printf("bundle wants:%s", want.c_str());
        CHECK(bundle.find(want) != std::string::npos);
        CHECK(bundle.find("\ndsp-dropped: ") < bundle.find("--- log ---"));
        CHECK(dropped >= stalledDropped);
        // ...AND THE RING ITSELF (GitHub issue 6, the 0.99.90 report): the real ring's
        // depth, in samples and in milliseconds of the 2 MS/s source (262), the
        // session's high water - the stall filled it - and the stalled block, still
        // the session's longest, to a tenth of a millisecond.
        const std::string ringWant = "\ndsp-ring: " + std::to_string(ring) + " samples (262 ms), high water 100%, longest DSP block ";
        std::printf("bundle wants:%s\n", ringWant.c_str());
        CHECK(bundle.find(ringWant) != std::string::npos);
        CHECK(numberAfter(bundle, "longest DSP block ") >= 2500);
        CHECK(bundle.find("\ndsp-ring: ") < bundle.find("--- log ---"));
    }

    // --- 5. THE FRAME LOOP closes the minute once a minute, on its own clock ------------
    {
        const std::string srcText =
            stripComments(readFile(fs::path(CASCADE_SOURCE_DIR) / "src" / "gui" / "app_window.cpp"));
        // Called by the frame loop beside the audio watchdog (the statements, at the
        // frame loop's indentation - not the definitions)...
        const std::size_t audio = srcText.find("    pollAudioHealth();");
        const std::size_t dsp = srcText.find("    pollDspMinute();");
        CHECK(audio != std::string::npos);
        CHECK(dsp != std::string::npos && dsp > audio && dsp - audio < 200);
        // ...and the poll closes a minute when sixty seconds have passed on its own mark.
        const std::size_t poll = srcText.find("void AppWindow::pollDspMinute()");
        const std::size_t end = poll == std::string::npos ? poll : srcText.find("\n}", poll);
        CHECK(poll != std::string::npos && end != std::string::npos);
        if (poll != std::string::npos && end != std::string::npos) {
            const std::string body = srcText.substr(poll, end - poll);
            const std::size_t minute = body.find("now - lastDspLogSec_ >= 60.0");
            CHECK(minute != std::string::npos);
            CHECK(minute != std::string::npos &&
                  body.find("closeDspMinute()", minute) != std::string::npos);
        }
        // Not inside the audio watchdog, whose first act is to return on a machine with
        // no sound output.
        const std::size_t audioBody = srcText.find("void AppWindow::pollAudioHealth()");
        const std::size_t audioEnd = audioBody == std::string::npos
                                         ? audioBody
                                         : srcText.find("void AppWindow::closeAudioMinute(", audioBody);
        CHECK(audioBody != std::string::npos && audioEnd != std::string::npos);
        if (audioBody != std::string::npos && audioEnd != std::string::npos) {
            CHECK(srcText.substr(audioBody, audioEnd - audioBody).find("closeDspMinute") ==
                  std::string::npos);
        }
        // THE ROW IS THE MINUTE'S, BEHIND THE FLOOR (0.99.102): closeDspMinute files the
        // report only inside the ringDropWorthAReport test, and the health poll's
        // ring branch counts without filing.
        const std::size_t closeFn = srcText.find("void AppWindow::closeDspMinute()");
        const std::size_t closeEnd = closeFn == std::string::npos ? closeFn : srcText.find("\n}", closeFn);
        CHECK(closeFn != std::string::npos && closeEnd != std::string::npos);
        if (closeFn != std::string::npos && closeEnd != std::string::npos) {
            const std::string body = srcText.substr(closeFn, closeEnd - closeFn);
            const std::size_t floor = body.find("if (cascade::core::ringDropWorthAReport(dropped, read))");
            const std::size_t file = body.find("fileIssueReport(cascade::core::health::Recovered::RingDrop)");
            CHECK(floor != std::string::npos && file != std::string::npos && floor < file);
        }
        const std::string health = stripComments(
            readFile(fs::path(CASCADE_SOURCE_DIR) / "src" / "core" / "health_events.cpp"));
        const std::size_t watch = health.find("void RecoveryWatch::poll(");
        const std::size_t watchEnd = watch == std::string::npos ? watch : health.find("\n}", watch);
        CHECK(watch != std::string::npos && watchEnd != std::string::npos);
        if (watch != std::string::npos && watchEnd != std::string::npos) {
            const std::string body = health.substr(watch, watchEnd - watch);
            CHECK(body.find("tokenRecovered(Recovered::RingDrop)") != std::string::npos);
            CHECK(body.find("fileIssueReport(Recovered::RingDrop)") == std::string::npos);
        }
    }

    std::error_code gone;
    fs::remove_all(g_scratch, gone);
    return testSummary("test_dsp_drop_minute_app");
}
