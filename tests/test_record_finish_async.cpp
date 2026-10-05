// A recording's file is closed off the frame loop.
//
// WHAT WENT WRONG. Recorder::stop() flushes the tail of the take, seeks back and
// patches the header's two size fields, and closes the file: three calls that wait
// for the disk, made on the thread that draws the window - on the Stop key (both
// takes), on a source change that ends a take, at quit, and, for a patch speaker's
// WAV, in ~WavDest when the set that holds its file is retired (the patch page's
// STOP, a radio's device changed, the page closed). 0.99.63 moved the START
// of a recording off that thread; this is the finish, the half it left
// (docs/DIAGNOSTICS.md, "The window does no disk work", still open there).
//
// WHAT IS TESTED, and at which level. NOTHING HERE DEPENDS ON A REAL SLOW DISK:
// Recorder::finishFile runs a hook (null in every shipped build) immediately
// before it touches the file, and a test binds one that sleeps - a slow disk as far
// as the frame loop can tell, with the real file, the real header and the real
// close behind it.
//
//   1. THE HARNESS CAN SEE THE FAULT: the same slow finish on the heartbeating
//      thread is reported by a real HangWatchdog.
//   2. THE REAL HANDLERS, through a real AppWindow, under a real HangWatchdog
//      (800 ms threshold) and a frame loop: the Stop audio key, the Stop IQ key, a
//      take ended by the source changing, and a patch speaker's set retired. After
//      each, the file is COMPLETE AND VALID: the header says what the file holds,
//      and what it holds is every sample the recorder accepted.
//   3. A RECORD WHILE THE PREVIOUS TAKE IS STILL BEING CLOSED is remembered and
//      started when the file is closed - and a Stop in between withdraws it.
//   4. QUIT drains against the deadline it is given and never past it, and
//      collects what lands inside it.
//   5. A FINISH THAT FAILS (the handle gone from under the stream) ends, once, with
//      the husk the opener flushed, one log line, nothing new on screen - which is
//      what the synchronous Stop said.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <atomic>
#include <chrono>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <io.h>
#include <process.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "core/diag_log.hpp"
#include "core/hang_watchdog.hpp"
#include "core/patch_audio.hpp"
#include "core/patch_graph.hpp"
#include "core/patch_plan.hpp"
#include "core/patch_radio.hpp"
#include "core/record_finish.hpp"
#include "core/recorder.hpp"
#include "gui/app_window.hpp"
#include "source/siggen_source.hpp"
#include "test_check.hpp"

namespace fs = std::filesystem;
namespace pc = cascade::core::patch;
using cascade::core::HangWatchdog;
using cascade::core::RecordFinisher;
using cascade::core::Recorder;
using cascade::core::RecordKind;

namespace cascade::gui {

// The friend AppWindow names for the test files that drive its private members.
struct AppWindowTestAccess {
    static void setRecordDir(AppWindow& a, std::string d) { a.recordDir_ = std::move(d); }
    static bool startAudio(AppWindow& a) { return a.startAudioRecording(); }
    static bool startIq(AppWindow& a) { return a.startIqRecording(); }
    static void stopAudio(AppWindow& a) { a.stopAudioRecording(); }
    static void stopIq(AppWindow& a) { a.stopIqRecording(); }
    static void poll(AppWindow& a, double nowS) { a.pollRecordStarts(nowS); }
    static bool audioRecording(AppWindow& a) { return a.audioRecorder_.recording(); }
    static bool iqRecording(AppWindow& a) { return a.iqRecorder_.recording(); }
    static bool finishing(AppWindow& a, bool iq) { return a.takeFinishing(iq); }
    static bool drainFinishes(AppWindow& a, std::chrono::steady_clock::time_point d) {
        return a.drainRecordFinishes(d);
    }
    static const std::string& recordError(AppWindow& a) { return a.recordError_; }
    static const std::string& recordNotice(AppWindow& a) { return a.recordNotice_; }
    static Recorder& audioRec(AppWindow& a) { return a.audioRecorder_; }
    static Recorder& iqRec(AppWindow& a) { return a.iqRecorder_; }
    // The source changing ends an I/Q take (installSource -> endTakes).
    static void sourceChanged(AppWindow& a) { a.endTakes(true, false, "the source changed", false); }

    // A patch with a radio running on the generator and a speaker (a WAV file).
    static pc::NodeId buildSpeakerPatch(AppWindow& a) {
        a.patchSeeded_ = true;
        const pc::NodeId r = a.patchGraph_.addNode(pc::NodeKind::Radio, "Radio", pc::PortType::Iq);
        const pc::NodeId c = a.patchGraph_.addNode(pc::NodeKind::Channel, "Tone", pc::PortType::Iq);
        const pc::NodeId d = a.patchGraph_.addNode(pc::NodeKind::Demod, "AM", pc::PortType::Iq);
        const pc::NodeId s = a.patchGraph_.addNode(pc::NodeKind::Sink, "Speaker", pc::PortType::Audio);
        auto gen = std::make_unique<cascade::source::SigGenSource>(2.4e6);
        gen->sigGen().setTone(0, 300000.0, -30.0f);
        a.patchRadios_[r] = std::make_unique<pc::PatchRadio>(r, std::move(gen), "generator");
        if (pc::Node* n = a.patchGraph_.mutableNode(c)) {
            n->freqHz = a.patchRadios_[r]->centreHz() + 300000.0;
        }
        if (pc::Node* n = a.patchGraph_.mutableNode(d)) { n->mode = 2; }
        CHECK(a.patchGraph_.connect(r, 0, c, 0) == pc::Connect::Ok);
        CHECK(a.patchGraph_.connect(c, 0, d, 0) == pc::Connect::Ok);
        CHECK(a.patchGraph_.connect(d, 0, s, 0) == pc::Connect::Ok);
        std::string err;
        CHECK(a.patchRadios_[r]->start(err));
        a.patchPlan_ = pc::compile(a.patchGraph_, a.patchRadioInfos(), nullptr, false);
        CHECK(a.patchPlan_.runnable);
        return s;
    }
    static void publish(AppWindow& a) { a.patchPublishSets(); }
    static std::shared_ptr<pc::AudioDest> destOf(AppWindow& a, pc::NodeId sink) {
        return pc::destFor(a.patchDests_, sink);
    }
    // The page closing, as patchStopAll does it for the speakers: radios stop and
    // are destroyed, then the outputs go.
    static void retireSpeakers(AppWindow& a) {
        for (auto& [id, radio] : a.patchRadios_) {
            (void)id;
            radio->stop();
        }
        a.patchRadios_.clear();
        a.patchRadioSig_.clear();
        a.patchDests_.clear();
        a.patchDestMadeFor_.clear();
        a.patchDestError_.clear();
    }
};

}  // namespace cascade::gui

using Access = cascade::gui::AppWindowTestAccess;
using cascade::gui::AppWindow;

namespace {

fs::path g_scratch;

void setEnv(const char* name, const std::string& value) {
#if defined(_WIN32)
    _putenv_s(name, value.c_str());
    ::SetEnvironmentVariableA(name, value.c_str());
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
    g_scratch = fs::temp_directory_path() / ("foxsdr_record_finish_" + std::to_string(pid));
    std::error_code ec;
    fs::remove_all(g_scratch, ec);
    fs::create_directories(g_scratch, ec);
    const std::string s = g_scratch.string();
    for (const char* v : {"LOCALAPPDATA", "APPDATA", "USERPROFILE", "HOME", "XDG_CONFIG_HOME",
                          "XDG_STATE_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME"}) {
        setEnv(v, s);
    }
    setEnv("FOXSDR_TELEMETRY_URL", "http://127.0.0.1:9");
    setEnv("FOXSDR_TESTER_USAGE_URL", "http://127.0.0.1:9");
    setEnv("FOXSDR_BETA_API_URL", "http://127.0.0.1:9");
}

double nowMs() {
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

std::string dirFor(const char* tag) { return (g_scratch / "takes" / tag).string(); }

// --- a slow, or failing, disk ------------------------------------------------

std::atomic<int> g_hookMs{0};
std::atomic<int> g_hookCalls{0};
std::atomic<int> g_hookFinished{0};
std::atomic<bool> g_hookOnCaller{false};
// After the sleep, close the OS handle behind the stream: every write, seek and
// the close itself then fail, as they do when the device is gone or the disk
// refuses (the take's file keeps whatever the opener flushed).
std::atomic<bool> g_hookPullFile{false};
std::thread::id g_caller;

void slowFinish(std::FILE* f) {
    g_hookCalls.fetch_add(1);
    if (std::this_thread::get_id() == g_caller) { g_hookOnCaller.store(true); }
    std::this_thread::sleep_for(std::chrono::milliseconds(g_hookMs.load()));
    if (g_hookPullFile.load() && f != nullptr) {
#if defined(_WIN32)
        ::CloseHandle(reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(f))));
#endif
    }
    g_hookFinished.fetch_add(1);
}

void resetHook(int ms) {
    g_hookMs.store(ms);
    g_hookCalls.store(0);
    g_hookFinished.store(0);
    g_hookOnCaller.store(false);
    g_hookPullFile.store(false);
    g_caller = std::this_thread::get_id();
}

struct LoopStats {
    int frames = 0;
    double worstGapMs = 0.0;
};

template <class Frame>
LoopStats runFrameLoop(HangWatchdog& w, double forMs, Frame&& frame) {
    LoopStats s;
    double last = nowMs();
    const double until = last + forMs;
    while (nowMs() < until) {
        w.heartbeat();
        frame();
        const double t = nowMs();
        if (t - last > s.worstGapMs) { s.worstGapMs = t - last; }
        last = t;
        ++s.frames;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return s;
}

fs::path reportDir(const std::string& tag) {
    const fs::path d = g_scratch / ("reports-" + tag);
    std::error_code ec;
    fs::create_directories(d, ec);
    return d;
}

void startWatchdog(HangWatchdog& w, const fs::path& dir) {
    w.setSuppressionForTest(HangWatchdog::SuppressionForTest::NeverSuppress);
    w.start(dir.string(), 800);
    CHECK(w.running());
    for (int i = 0; i < 50; ++i) {
        w.heartbeat();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(w.reportsWritten() == 0u);
}

template <class Cond>
bool pumpApp(AppWindow& app, double ms, Cond&& cond) {
    const double t0 = nowMs();
    while (nowMs() < t0 + ms) {
        Access::poll(app, (nowMs() - t0) / 1000.0);
        if (cond()) { return true; }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return cond();
}

std::vector<unsigned char> readAll(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<unsigned char>((std::istreambuf_iterator<char>(in)),
                                      std::istreambuf_iterator<char>());
}

std::vector<fs::path> filesIn(const std::string& dir) {
    std::vector<fs::path> out;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        if (e.is_regular_file(ec)) { out.push_back(e.path()); }
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::uint32_t le32(const std::vector<unsigned char>& b, std::size_t at) {
    return static_cast<std::uint32_t>(b[at]) | (static_cast<std::uint32_t>(b[at + 1]) << 8) |
           (static_cast<std::uint32_t>(b[at + 2]) << 16) | (static_cast<std::uint32_t>(b[at + 3]) << 24);
}

// What a finished take's header says against what the file holds.
struct Wav {
    bool valid = false;         // "RIFF", and both size fields agree with the file's length
    std::uint64_t data = 0;     // the data chunk's size field
    std::uint64_t fileData = 0; // the file's length less the 44-byte header
};
Wav inspect(const fs::path& p) {
    Wav w;
    const std::vector<unsigned char> b = readAll(p);
    if (b.size() < 44 || std::memcmp(b.data(), "RIFF", 4) != 0) { return w; }
    w.data = le32(b, 40);
    w.fileData = b.size() - 44;
    w.valid = le32(b, 4) == b.size() - 8 && w.data == w.fileData;
    return w;
}

// How many lines in the diagnostic log (its in-memory ring) contain `needle`.
int logCount(const char* needle) {
    int n = 0;
    for (const std::string& l : cascade::core::DiagLog::instance().ringSnapshot()) {
        if (l.find(needle) != std::string::npos) { ++n; }
    }
    return n;
}

// A take armed through the real handler, with `n` known samples written into it.
void takeAudio(AppWindow& app, const char* tag, std::size_t n) {
    Access::setRecordDir(app, dirFor(tag));
    CHECK(Access::startAudio(app));
    CHECK(pumpApp(app, 5000.0, [&] { return Access::audioRecording(app); }));
    std::vector<float> s(n);
    for (std::size_t i = 0; i < n; ++i) { s[i] = 0.5f * static_cast<float>((i % 100)) / 100.0f; }
    Access::audioRec(app).writeAudio(s.data(), s.size());
}

void takeIq(AppWindow& app, const char* tag, std::size_t n) {
    Access::setRecordDir(app, dirFor(tag));
    CHECK(Access::startIq(app));
    CHECK(pumpApp(app, 5000.0, [&] { return Access::iqRecording(app); }));
    std::vector<std::complex<float>> s(n);
    for (std::size_t i = 0; i < n; ++i) { s[i] = {0.25f, static_cast<float>(i % 7) * 0.1f}; }
    Access::iqRec(app).writeIq(s.data(), s.size());
}

// --- 1. THE HARNESS CAN SEE THE FAULT -----------------------------------------

void checkTheSynchronousStopIsReported() {
    resetHook(2500);
    Recorder::setFinishHookForTest(&slowFinish);
    HangWatchdog w;
    startWatchdog(w, reportDir("control"));
    Recorder rec;
    std::string err;
    CHECK(rec.start(RecordKind::Audio, dirFor("control"), 48000.0, err));
    const std::vector<float> s(480, 0.25f);
    rec.writeAudio(s.data(), s.size());
    rec.stop();  // what the Stop key used to do
    CHECK(w.reportsWritten() == 1u);
    CHECK(g_hookOnCaller.load());
    w.stop();
    Recorder::setFinishHookForTest(nullptr);
}

// --- 2. THE REAL HANDLERS ------------------------------------------------------

// `expected` is what the recorder had accepted before the press (0: not known,
// only that the file holds something). Run the press under a heartbeating frame
// loop and a real watchdog; then, once the worker is done, the file must be whole.
template <class Press>
void stopCase(const char* what, const std::string& dir, std::uint64_t expected, Press&& press) {
    resetHook(2500);
    Recorder::setFinishHookForTest(&slowFinish);
    HangWatchdog w;
    startWatchdog(w, reportDir(what));
    bool first = true;
    const LoopStats s = runFrameLoop(w, 3200.0, [&] {
        if (first) {
            first = false;
            press();
        }
    });
    CHECK(w.reportsWritten() == 0u);
    CHECK(s.frames > 100);
    CHECK(s.worstGapMs < 800.0);
    CHECK(!g_hookOnCaller.load());
    // The finish really ran, on a thread of its own, and landed.
    CHECK(g_hookCalls.load() == 1);
    CHECK(RecordFinisher::drain(std::chrono::steady_clock::now() + std::chrono::seconds(10)));
    CHECK(g_hookFinished.load() == 1);
    CHECK(RecordFinisher::inFlight() == 0u);
    // COMPLETE AND VALID.
    const std::vector<fs::path> files = filesIn(dir);
    CHECK(files.size() == 1u);
    if (files.size() == 1u) {
        const Wav wav = inspect(files[0]);
        CHECK(wav.valid);
        CHECK(wav.data > 0u);
        if (expected != 0u) { CHECK(wav.data == expected); }
        std::printf("  %s: %d frames in 3.2 s, worst gap %.0f ms, hang reports %u, file %llu data "
                    "bytes (header %llu), valid=%d\n",
                    what, s.frames, s.worstGapMs, w.reportsWritten(),
                    static_cast<unsigned long long>(wav.fileData),
                    static_cast<unsigned long long>(wav.data), wav.valid ? 1 : 0);
    }
    w.stop();
    Recorder::setFinishHookForTest(nullptr);
}

void checkTheStopKeysNeverHoldAFrame() {
    {
        AppWindow app;
        takeAudio(app, "stop-audio", 48000);
        const std::uint64_t bytes = Access::audioRec(app).bytesWritten();
        CHECK(bytes == 96000u);
        stopCase("Stop audio", dirFor("stop-audio"), bytes, [&] { Access::stopAudio(app); });
        CHECK(!Access::finishing(app, false));
    }
    {
        AppWindow app;
        takeIq(app, "stop-iq", 20000);
        const std::uint64_t bytes = Access::iqRec(app).bytesWritten();
        CHECK(Access::iqRec(app).samplesWritten() == 20000u);
        stopCase("Stop IQ", dirFor("stop-iq"), bytes, [&] { Access::stopIq(app); });
    }
    {
        AppWindow app;
        takeIq(app, "source-changed", 20000);
        const std::uint64_t bytes = Access::iqRec(app).bytesWritten();
        stopCase("source change ends the I/Q take", dirFor("source-changed"), bytes,
                 [&] { Access::sourceChanged(app); });
        CHECK(!Access::iqRecording(app));
    }
}

void checkRetiringASpeakerNeverHoldsAFrame() {
    AppWindow app;
    Access::setRecordDir(app, dirFor("speaker"));
    const pc::NodeId sink = Access::buildSpeakerPatch(app);
    Access::publish(app);
    CHECK(Access::destOf(app, sink) != nullptr);
    // Let the radio's reader feed the speaker so the file is armed and has sound.
    std::this_thread::sleep_for(std::chrono::milliseconds(800));
    CHECK(Access::destOf(app, sink)->samples() > 0u);
    stopCase("a patch speaker's set retired", dirFor("speaker"), 0, [&] { Access::retireSpeakers(app); });
}

// --- 3. A RECORD WHILE THE PREVIOUS TAKE IS STILL BEING CLOSED -----------------

void checkARecordBehindAFinishIsRemembered() {
    resetHook(1200);
    Recorder::setFinishHookForTest(&slowFinish);
    AppWindow app;
    const std::string dir = dirFor("queued");
    takeAudio(app, "queued", 24000);
    const std::uint64_t first = Access::audioRec(app).bytesWritten();

    Access::stopAudio(app);
    CHECK(Access::finishing(app, false));
    CHECK(!Access::audioRecording(app));
    // The press comes back as accepted - the take is on its way - but nothing is
    // recording and no second file exists beside the one still being finalised.
    CHECK(Access::startAudio(app));
    CHECK(!Access::audioRecording(app));
    CHECK(filesIn(dir).size() == 1u);
    CHECK(Access::recordError(app).empty());

    // It starts by itself on the frame the first file is closed.
    CHECK(pumpApp(app, 8000.0, [&] { return Access::audioRecording(app); }));
    CHECK(!Access::finishing(app, false));
    CHECK(g_hookFinished.load() == 1);
    CHECK(filesIn(dir).size() == 2u);
    {
        const std::vector<fs::path> files = filesIn(dir);
        if (!files.empty()) {
            const Wav w = inspect(files[0]);
            CHECK(w.valid);
            CHECK(w.data == first);
        }
    }
    // And the second take is a take: it records and finishes like any other.
    const std::vector<float> s(4800, 0.25f);
    Access::audioRec(app).writeAudio(s.data(), s.size());
    Access::stopAudio(app);
    CHECK(RecordFinisher::drain(std::chrono::steady_clock::now() + std::chrono::seconds(10)));
    {
        const std::vector<fs::path> files = filesIn(dir);
        CHECK(files.size() == 2u);
        if (files.size() == 2u) {
            const Wav a = inspect(files[0]);
            const Wav b = inspect(files[1]);
            CHECK(a.valid && b.valid);
            CHECK(a.data == first);
            CHECK(b.data == 9600u);
            std::printf("  Record behind a finish: first take %llu bytes, second %llu bytes, both valid\n",
                        static_cast<unsigned long long>(a.data), static_cast<unsigned long long>(b.data));
        }
    }
    Recorder::setFinishHookForTest(nullptr);
}

void checkAStopWithdrawsTheRememberedRecord() {
    resetHook(1200);
    Recorder::setFinishHookForTest(&slowFinish);
    AppWindow app;
    const std::string dir = dirFor("withdrawn");
    takeIq(app, "withdrawn", 8000);
    Access::stopIq(app);
    CHECK(Access::finishing(app, true));
    CHECK(Access::startIq(app));   // remembered
    Access::stopIq(app);           // withdrawn
    // Wait the finish out, then a few frames more: nothing starts.
    CHECK(pumpApp(app, 8000.0, [&] { return !Access::finishing(app, true); }));
    pumpApp(app, 600.0, [&] { return false; });
    CHECK(!Access::iqRecording(app));
    CHECK(filesIn(dir).size() == 1u);
    const std::vector<fs::path> files = filesIn(dir);
    if (files.size() == 1u) { CHECK(inspect(files[0]).valid); }
    Recorder::setFinishHookForTest(nullptr);
}

// --- 4. QUIT -----------------------------------------------------------------

void checkQuitDrainsWithinTheDeadline() {
    // A finish that outlasts the deadline: the drain gives up AT the deadline, says
    // so in the log, and the file is closed by the worker afterwards, whole.
    {
        resetHook(2500);
        Recorder::setFinishHookForTest(&slowFinish);
        AppWindow app;
        takeAudio(app, "quit-late", 24000);
        const std::uint64_t bytes = Access::audioRec(app).bytesWritten();
        Access::stopAudio(app);
        const int before = logCount("still being closed at exit");
        const double t0 = nowMs();
        const bool landed = Access::drainFinishes(
            app, std::chrono::steady_clock::now() + std::chrono::milliseconds(300));
        const double tookMs = nowMs() - t0;
        std::printf("  quit with a finish 2.5 s long and a 300 ms deadline: drain returned %s after %.0f ms\n",
                    landed ? "true" : "false", tookMs);
        CHECK(!landed);
        CHECK(tookMs >= 250.0);
        CHECK(tookMs < 900.0);
        CHECK(logCount("still being closed at exit") == before + 1);
        // Left to finish by itself: it lands, and the file is whole.
        CHECK(RecordFinisher::drain(std::chrono::steady_clock::now() + std::chrono::seconds(10)));
        const std::vector<fs::path> files = filesIn(dirFor("quit-late"));
        CHECK(files.size() == 1u);
        if (files.size() == 1u) {
            const Wav w = inspect(files[0]);
            CHECK(w.valid);
            CHECK(w.data == bytes);
        }
    }
    // A finish that lands inside the deadline is collected: the drain says so and
    // the file is whole at once.
    {
        resetHook(150);
        AppWindow app;
        takeIq(app, "quit-ok", 16000);
        const std::uint64_t bytes = Access::iqRec(app).bytesWritten();
        Access::stopIq(app);
        const double t0 = nowMs();
        const bool landed = Access::drainFinishes(
            app, std::chrono::steady_clock::now() + std::chrono::seconds(3));
        const double tookMs = nowMs() - t0;
        std::printf("  quit with a finish 150 ms long and a 3 s deadline: drain returned %s after %.0f ms\n",
                    landed ? "true" : "false", tookMs);
        CHECK(landed);
        CHECK(tookMs < 1500.0);
        CHECK(!Access::finishing(app, true));
        const std::vector<fs::path> files = filesIn(dirFor("quit-ok"));
        CHECK(files.size() == 1u);
        if (files.size() == 1u) {
            const Wav w = inspect(files[0]);
            CHECK(w.valid);
            CHECK(w.data == bytes);
        }
    }
    // Nothing out: a zero deadline returns at once.
    {
        AppWindow app;
        const double t0 = nowMs();
        CHECK(Access::drainFinishes(app, std::chrono::steady_clock::now()));
        CHECK(nowMs() - t0 < 200.0);
    }
    Recorder::setFinishHookForTest(nullptr);
}

// --- 5. A FINISH THAT FAILS --------------------------------------------------

void checkAFailingFinishSaysWhatTheSynchronousStopSaid() {
    resetHook(100);
    g_hookPullFile.store(true);
    Recorder::setFinishHookForTest(&slowFinish);
    AppWindow app;
    takeAudio(app, "failing", 24000);
    const std::uint64_t accepted = Access::audioRec(app).bytesWritten();
    CHECK(accepted == 48000u);
    const int before = logCount("could not be finalised");
    Access::stopAudio(app);
    CHECK(pumpApp(app, 8000.0, [&] { return !Access::finishing(app, false); }));
    CHECK(g_hookFinished.load() == 1);
    // ONE log line, and nothing on screen: no error, no notice - what the
    // synchronous stop said when the same thing happened (it ignored every result).
    CHECK(logCount("could not be finalised") == before + 1);
    CHECK(Access::recordError(app).empty());
    CHECK(Access::recordNotice(app).empty());
    CHECK(!Access::audioRecording(app));
    // The file is the husk the opener flushed, and parses: the header's sizes are
    // zero, not wrong. (The tail the stream still held is gone with the handle: a
    // finding, docs/DIAGNOSTICS.md.)
    const std::vector<fs::path> files = filesIn(dirFor("failing"));
    CHECK(files.size() == 1u);
    if (files.size() == 1u) {
        const std::vector<unsigned char> b = readAll(files[0]);
        CHECK(b.size() >= 44u);
        CHECK(std::memcmp(b.data(), "RIFF", 4) == 0);
        CHECK(le32(b, 40) == 0u);
        std::printf("  failing finish: file %zu bytes, header data field %u, accepted by the recorder %llu\n",
                    b.size(), b.size() >= 44u ? le32(b, 40) : 0u,
                    static_cast<unsigned long long>(accepted));
    }
    // A Record after a failed finish is not blocked by it.
    Recorder::setFinishHookForTest(nullptr);
    CHECK(!Access::finishing(app, false));
    CHECK(Access::startAudio(app));
    CHECK(pumpApp(app, 5000.0, [&] { return Access::audioRecording(app); }));
    Access::stopAudio(app);
    CHECK(RecordFinisher::drain(std::chrono::steady_clock::now() + std::chrono::seconds(10)));
}

}  // namespace

int main() {
    isolate();
    checkTheSynchronousStopIsReported();
    checkTheStopKeysNeverHoldAFrame();
    checkRetiringASpeakerNeverHoldsAFrame();
    checkARecordBehindAFinishIsRemembered();
    checkAStopWithdrawsTheRememberedRecord();
    checkQuitDrainsWithinTheDeadline();
    checkAFailingFinishSaysWhatTheSynchronousStopSaid();
    std::error_code ec;
    if (g_checksFailed == 0) { fs::remove_all(g_scratch, ec); }
    return testSummary("test_record_finish_async");
}
