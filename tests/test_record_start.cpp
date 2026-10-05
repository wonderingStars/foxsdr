// A recordings folder that will not answer must not freeze the window.
//
// THE FIELD REPORT THIS EXISTS FOR. A freeze report from 0.99.58 (Windows 11,
// 151 s into the session). The GUI thread's stack was
//
//   main -> AppWindow::run -> AppWindow::drawUi -> AppWindow::drawMenuColumn
//     -> AppWindow::drawRecorderSection -> core::Recorder::start
//       -> the C runtime's file open -> KERNELBASE -> ntdll
//
// and it stopped there for longer than the hang watchdog's five seconds. The
// Record button's handler called Recorder::start on the frame that drew the
// button, and start() creates the recordings directory and opens the file
// before it returns: any second in which that disk could not answer (a
// synchronised or network folder, a drive that has spun down, an antivirus
// holding the path) was a second in which the frame loop did not turn over. The
// same call was made by the Record audio button, the Record key and the web
// remote's record controls, so all four froze the same way.
//
// WHAT IS TESTED, and at which level. NOTHING HERE DEPENDS ON A REAL SLOW DISK:
// the blocking step of a start is Recorder::Opener, and a test binds one that
// sleeps, which is a slow disk as far as the frame loop can tell. Every case
// that opens a file calls the REAL Recorder::openFile after the sleep, so the
// file, its header and the bytes that follow are the real thing.
//
//   1. THE WIRING, because no test can stage every slow disk: no source under
//      src/gui may call a Recorder's start() (the blocking one) directly, the
//      two takes' requests go through gui::RecordStart, and the pipeline taps
//      are installed in exactly one place and only AFTER Recorder::begin().
//
//   2. THE HARNESS CAN SEE THE FAULT. The same slow opener called on the thread
//      that heartbeats is reported by a real HangWatchdog; every "no report"
//      below means something because of this.
//
//   3. gui::RecordStart ON ITS OWN, under a real HangWatchdog: a 2.5 s open
//      against an 800 ms threshold; the frame loop keeps beating, the answer
//      still arrives, and the take it makes is byte for byte the take an inline
//      Recorder::start makes (IQ and audio), so the data path is untouched.
//
//   4. THE EDGES, each against the class: an open that fails late says what an
//      inline start would have said; a second Record while one is pending is
//      refused and one worker is ever out; a Stop while it is opening leaves no
//      take, closes the file and does not leak a worker or a handle; quit does
//      not wait for a wedged open and the file it later opens is closed; a slow
//      open is said once in the log and not once a frame; a throw is contained.
//
//   5. THE REAL HANDLERS, through a real AppWindow with its recorders' opener
//      bound to the sleeping one, under a real HangWatchdog: the Record audio
//      path (button and key), the web remote's recordIq / recordAudio, Stop in
//      between, a late failure, a second Record, an IQ take whose input rate
//      changed while its file was opening, and destroying the window while an
//      open is wedged.
//
// WHAT IS NOT COVERED. The panel's drawing (the "Starting ... - click to
// cancel" button and the waiting line under it) is ImGui output no test here
// reads; the state it reads (RecordStart::starting/cancelled/pending/elapsedS)
// is checked, the drawing is not. Nothing here has met a genuinely slow disk.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
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
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "core/hang_watchdog.hpp"
#include "core/recorder.hpp"
#include "gui/app_window.hpp"
#include "gui/record_start.hpp"
#include "net/web_control.hpp"
#include "test_check.hpp"

namespace fs = std::filesystem;
using cascade::core::HangWatchdog;
using cascade::core::Recorder;
using cascade::core::RecordKind;
using cascade::gui::RecordStart;

namespace cascade::gui {

// The friend AppWindow names for the test files that drive its private members.
struct AppWindowTestAccess {
    // Both recorders get the same opener: the seam for the slow disk. Each
    // RecordStart copies its recorder's opener at request time.
    static void bindOpener(AppWindow& a, Recorder::Opener o) {
        a.iqRecorder_.bindOpener(o);
        a.audioRecorder_.bindOpener(std::move(o));
    }
    static void setRecordDir(AppWindow& a, std::string d) { a.recordDir_ = std::move(d); }

    // The handlers themselves, not stand-ins for them.
    static bool startAudio(AppWindow& a) { return a.startAudioRecording(); }
    static bool startIq(AppWindow& a) { return a.startIqRecording(); }
    // A Stop's file is closed on a worker since 0.99.65 (core/record_finish.hpp), and a
    // Record pressed meanwhile is remembered, not started (tests/test_record_finish_async.cpp
    // tests that). These tests are about the START, so a Stop here waits for its file
    // to be closed and collects it, as the user's next press, a moment later, would.
    static void settleFinishes(AppWindow& a) {
        cascade::core::RecordFinisher::drain(std::chrono::steady_clock::now() +
                                             std::chrono::seconds(10));
        a.pollRecordFinishes();
    }
    static void stopAudio(AppWindow& a) {
        a.stopAudioRecording();
        settleFinishes(a);
    }
    static void stopIq(AppWindow& a) {
        a.stopIqRecording();
        settleFinishes(a);
    }
    // What the web remote and the plugins' host API do (the Record key and the
    // buttons reach startAudioRecording/startIqRecording directly).
    static void applyRecord(AppWindow& a, std::optional<bool> iq, std::optional<bool> audio) {
        cascade::net::ControlRequest r;
        r.recordIq = iq;
        r.recordAudio = audio;
        a.applyControlRequest(r);
    }
    // One frame's worth of the poll.
    static void poll(AppWindow& a, double nowS) { a.pollRecordStarts(nowS); }

    static bool iqRecording(AppWindow& a) { return a.iqRecorder_.recording(); }
    static bool audioRecording(AppWindow& a) { return a.audioRecorder_.recording(); }
    static bool iqPending(AppWindow& a) { return a.iqStart_.pending(); }
    static bool audioPending(AppWindow& a) { return a.audioStart_.pending(); }
    static bool audioStarting(AppWindow& a) { return a.audioStart_.starting(); }
    static bool audioCancelled(AppWindow& a) { return a.audioStart_.cancelled(); }
    static std::string recordError(AppWindow& a) { return a.recordError_; }
    static std::string recordNotice(AppWindow& a) { return a.recordNotice_; }
    static double audioStartS(AppWindow& a) { return a.audioRecordStartS_; }
    static double iqStartS(AppWindow& a) { return a.iqRecordStartS_; }
    // The input rate moving while an IQ file is opening: a source switch or a
    // SoapySDR rate change would do it; this is the same state change.
    static void moveIqRate(AppWindow& a, double deltaHz) { a.iqRecordRateHz_ += deltaHz; }
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

// Per-user directories pointed at a scratch folder (process id in its name)
// and every network-facing URL at a port nothing listens on: constructing an
// AppWindow must not touch the owner's real FoxSDR folders or reach a real
// endpoint, and no take may land in the real recordings folder.
void isolate() {
#if defined(_WIN32)
    const int pid = _getpid();
#else
    const int pid = static_cast<int>(getpid());
#endif
    g_scratch = fs::temp_directory_path() / ("foxsdr_record_start_" + std::to_string(pid));
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

std::vector<unsigned char> readAll(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<unsigned char>((std::istreambuf_iterator<char>(in)),
                                      std::istreambuf_iterator<char>());
}

// The regular files in `dir`, whatever the wall clock named them.
std::vector<fs::path> filesIn(const std::string& dir) {
    std::vector<fs::path> out;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        if (e.is_regular_file(ec)) { out.push_back(e.path()); }
    }
    return out;
}

// --- a slow disk -------------------------------------------------------------

// How many files an opened take has had closed: the counting closer below. A
// plain function pointer is the deleter type of Recorder::FilePtr, so the count
// has to be global; every case that reads it resets it first.
std::atomic<int> g_closes{0};
void countingClose(std::FILE* f) {
    if (f != nullptr) {
        g_closes.fetch_add(1);
        std::fclose(f);
    }
}

// An opener that behaves like a slow disk: it takes `blockMs` to answer, records
// how many times and from which thread it was asked, notices being run twice at
// once (a second worker piling up behind a dead directory), can fail or throw
// when it finally answers, and otherwise hands back the REAL file the real opener
// made, its close counted. Held by shared_ptr and captured by value, never by
// `this`: an abandoned worker outlives the test that started it.
struct SlowDisk {
    std::atomic<int> blockMs{0};
    std::atomic<int> calls{0};
    std::atomic<int> concurrent{0};
    std::atomic<int> maxConcurrent{0};
    std::atomic<bool> askedFromCaller{false};
    std::atomic<bool> fails{false};
    std::atomic<bool> throws{false};
    const std::thread::id caller = std::this_thread::get_id();
};

Recorder::Opener slowOpener(std::shared_ptr<SlowDisk> d) {
    return [d](const Recorder::OpenRequest& req, Recorder::OpenedFile& out,
               std::string& error) -> bool {
        if (std::this_thread::get_id() == d->caller) { d->askedFromCaller.store(true); }
        const int live = d->concurrent.fetch_add(1) + 1;
        int seen = d->maxConcurrent.load();
        while (live > seen && !d->maxConcurrent.compare_exchange_weak(seen, live)) {}
        d->calls.fetch_add(1);
        std::this_thread::sleep_for(std::chrono::milliseconds(d->blockMs.load()));
        d->concurrent.fetch_sub(1);
        if (d->throws.load()) { throw std::runtime_error("the disk went away"); }
        if (d->fails.load()) {
            error = "recorder: cannot create \"" + req.path + "\"";
            return false;
        }
        if (!Recorder::openFile(req, out, error)) { return false; }
        std::FILE* raw = out.file.release();
        out.file = Recorder::FilePtr(raw, &countingClose);
        return true;
    };
}

std::shared_ptr<SlowDisk> makeDisk(int blockMs) {
    auto d = std::make_shared<SlowDisk>();
    d->blockMs.store(blockMs);
    return d;
}

// --- the frame loop, as test_link_request_poll drives it ---------------------

struct LoopStats {
    int frames = 0;
    double worstGapMs = 0.0;
};

// What AppWindow::run does between two presents: beat, do the frame's work,
// wait out the rest of the frame.
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
    // Healthy frames first, so a report afterwards cannot be blamed on the
    // loop never having started.
    for (int i = 0; i < 50; ++i) {
        w.heartbeat();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(w.reportsWritten() == 0u);
}

// Polls the class until a finished open is collected or `ms` pass.
bool pollUntil(RecordStart& rs, RecordStart::Result& r, double ms) {
    const double until = nowMs() + ms;
    while (nowMs() < until) {
        if (rs.poll(r)) { return true; }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

// Waits for `cond` while feeding the real window's per-frame poll.
template <class Cond>
bool pumpApp(AppWindow& app, double ms, Cond&& cond) {
    const double t0 = nowMs();
    const double until = t0 + ms;
    while (nowMs() < until) {
        Access::poll(app, (nowMs() - t0) / 1000.0);
        if (cond()) { return true; }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return cond();
}

Recorder::OpenRequest requestFor(const Recorder& rec, RecordKind kind, const std::string& dir,
                                 double rateHz) {
    Recorder::OpenRequest req;
    std::string err;
    CHECK(rec.prepare(kind, dir, rateHz, std::string{}, req, err));
    return req;
}

// --- 1. THE WIRING ----------------------------------------------------------

std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// The lines of `text` with // and /* */ comments blanked.
std::vector<std::string> codeLines(const std::string& text) {
    std::vector<std::string> out;
    std::string line;
    bool inBlock = false;
    std::istringstream is(text);
    while (std::getline(is, line)) {
        if (!line.empty() && line.back() == '\r') { line.pop_back(); }
        std::string code;
        for (std::size_t i = 0; i < line.size(); ++i) {
            if (inBlock) {
                if (line.compare(i, 2, "*/") == 0) {
                    inBlock = false;
                    ++i;
                }
                continue;
            }
            if (line.compare(i, 2, "//") == 0) { break; }
            if (line.compare(i, 2, "/*") == 0) {
                inBlock = true;
                ++i;
                continue;
            }
            code += line[i];
        }
        out.push_back(code);
    }
    return out;
}

// The text of AppWindow::<fn> as defined in `lines`: from its column-0
// definition line to the closing brace in column 0.
std::string memberBody(const std::vector<std::string>& lines, const std::string& signature) {
    std::string body;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (lines[i].find(signature) != 0) { continue; }
        for (std::size_t k = i; k < lines.size(); ++k) {
            body += lines[k] + "\n";
            if (k > i && !lines[k].empty() && lines[k][0] == '}') { break; }
        }
        break;
    }
    return body;
}

std::size_t countOf(const std::string& hay, const std::string& needle) {
    std::size_t n = 0;
    for (std::size_t at = hay.find(needle); at != std::string::npos; at = hay.find(needle, at + 1)) {
        ++n;
    }
    return n;
}

void checkTheOpenIsNotMadeOnTheGuiThread() {
    const fs::path gui = fs::path(__FILE__).parent_path().parent_path() / "src" / "gui";
    std::error_code ec;
    CHECK(fs::is_directory(gui, ec));
    int scanned = 0;
    int directStarts = 0;
    int tapInstalls = 0;
    int requests = 0;
    std::string finishBody;
    for (const auto& entry : fs::directory_iterator(gui, ec)) {
        const fs::path p = entry.path();
        if (p.extension() != ".cpp" && p.extension() != ".hpp") { continue; }
        const std::vector<std::string> lines = codeLines(readFile(p));
        ++scanned;
        for (std::size_t i = 0; i < lines.size(); ++i) {
            const std::string& l = lines[i];
            // The blocking start, on either of the application's recorders.
            for (const char* rec : {"iqRecorder_", "audioRecorder_"}) {
                if (l.find(std::string(rec) + ".start(") != std::string::npos) {
                    std::printf("  %s:%zu starts a recording on the calling thread\n",
                                p.filename().string().c_str(), i + 1);
                    ++directStarts;
                }
            }
            if (l.find("setIqRecorder(&") != std::string::npos ||
                l.find("setAudioRecorder(&") != std::string::npos) {
                ++tapInstalls;
            }
            if (p.filename() == "app_window.cpp") {
                if (l.find("iqStart_.request(") != std::string::npos ||
                    l.find("audioStart_.request(") != std::string::npos) {
                    ++requests;
                }
            }
        }
        if (p.filename() == "app_window.cpp") {
            finishBody = memberBody(lines, "void AppWindow::finishRecordStart(");
        }
    }
    CHECK(scanned > 10);
    CHECK(directStarts == 0);
    // Both takes ask through the worker.
    CHECK(requests == 2);
    // THE ORDER CONTRACT (Pipeline::set*Recorder: start FIRST, tap second): the
    // two taps go in in exactly one function, and there only after begin().
    CHECK(tapInstalls == 2);
    CHECK(!finishBody.empty());
    const std::size_t begin = finishBody.find(".begin(");
    const std::size_t iqTap = finishBody.find("setIqRecorder(&");
    const std::size_t audioTap = finishBody.find("setAudioRecorder(&");
    CHECK(begin != std::string::npos);
    CHECK(iqTap != std::string::npos && begin < iqTap);
    CHECK(audioTap != std::string::npos && begin < audioTap);
    CHECK(countOf(finishBody, ".begin(") == 1);
}

// --- 2. THE HARNESS CAN SEE THE FAULT ----------------------------------------

// THE CONTROL. The same slow opener, called where Recorder::start used to call
// it: on the thread that heartbeats. If the harness cannot see THIS, a green
// result below means nothing.
void checkTheSynchronousStartIsReported() {
    auto disk = makeDisk(2500);
    HangWatchdog w;
    startWatchdog(w, reportDir("control"));
    Recorder rec;
    rec.bindOpener(slowOpener(disk));
    std::string err;
    CHECK(rec.start(RecordKind::Audio, dirFor("control"), 48000.0, err));  // the old Record press
    rec.stop();
    CHECK(w.reportsWritten() == 1u);
    CHECK(disk->askedFromCaller.load());
    w.stop();
}

// --- 3. THE CLASS UNDER A WATCHDOG, AND THE TAKE IT MAKES --------------------

// THE FIELD HANG, against the class: a 2.5 s answer against an 800 ms
// threshold. The loop must keep beating, the watchdog must write nothing, and
// the answer must still arrive - a fix that merely dropped the take would pass
// everything but the last of these.
void checkSlowOpenDoesNotStallTheFrameLoop() {
    auto disk = makeDisk(2500);
    HangWatchdog w;
    startWatchdog(w, reportDir("class"));
    Recorder rec;
    const Recorder::OpenRequest req = requestFor(rec, RecordKind::Audio, dirFor("class"), 48000.0);
    RecordStart rs;
    bool requested = false;
    int collected = 0;
    RecordStart::Result got;
    const LoopStats s = runFrameLoop(w, 3200.0, [&] {
        // Asked EVERY frame until it has an answer, as a caller with no memory
        // of having asked would: the disk must still be asked once.
        if (collected == 0) {
            const bool accepted = rs.request(slowOpener(disk), req);
            if (!requested) { CHECK(accepted); }
            requested = true;
        }
        RecordStart::Result r;
        if (rs.poll(r)) {
            ++collected;
            got = std::move(r);
        }
    });
    CHECK(w.reportsWritten() == 0u);
    CHECK(s.frames > 100);
    CHECK(s.worstGapMs < 800.0);
    // The answer arrived, once, on a later frame - and the disk was asked once,
    // from a thread that is not this one.
    CHECK(collected == 1);
    CHECK(got.ok);
    CHECK(!got.cancelled);
    CHECK(got.file.file != nullptr);
    CHECK(disk->calls.load() == 1);
    CHECK(!disk->askedFromCaller.load());
    std::printf("  slow open: %d frames in 3.2 s, worst gap %.0f ms, hang reports %u\n", s.frames,
                s.worstGapMs, w.reportsWritten());
    w.stop();
}

// THE DATA PATH IS UNTOUCHED: a take whose file was opened late, on a worker, is
// byte for byte the take an inline Recorder::start makes - both kinds - and the
// crash contract (a complete zero-length header on disk BEFORE the first sample)
// holds across the split.
void checkALateTakeIsTheSameTakeAsAnInlineOne() {
    for (const RecordKind kind : {RecordKind::BasebandIq, RecordKind::Audio}) {
        const bool iq = kind == RecordKind::BasebandIq;
        const double rate = iq ? 250000.0 : 48000.0;
        const std::string lateDir = dirFor(iq ? "late-iq" : "late-audio");
        const std::string inlineDir = dirFor(iq ? "inline-iq" : "inline-audio");

        // The same deterministic samples into both: a fixed-seed LCG, values
        // spanning past +-1 so the audio clamp and the IQ passthrough both run.
        std::uint32_t lcg = 0x2468ACE1u;
        const auto next = [&lcg] {
            lcg = lcg * 1664525u + 1013904223u;
            return (static_cast<float>(static_cast<std::int32_t>(lcg)) / 2147483648.0f) * 1.5f;
        };
        std::vector<std::complex<float>> iqSamples(10000);
        for (auto& v : iqSamples) { v = {next(), next()}; }
        std::vector<float> audioSamples(5000);
        for (auto& v : audioSamples) { v = next(); }
        const auto feed = [&](Recorder& r) {
            if (iq) {
                r.writeIq(iqSamples.data(), 6000);
                r.writeIq(iqSamples.data() + 6000, 4000);
            } else {
                r.writeAudio(audioSamples.data(), audioSamples.size());
            }
        };

        // Inline, as every caller that can wait still does it.
        Recorder inlineRec;
        std::string err;
        CHECK(inlineRec.start(kind, inlineDir, rate, err));
        feed(inlineRec);
        inlineRec.stop();

        // Late, on a worker.
        auto disk = makeDisk(300);
        Recorder rec;
        RecordStart rs;
        CHECK(rs.request(slowOpener(disk), requestFor(rec, kind, lateDir, rate)));
        RecordStart::Result r;
        CHECK(pollUntil(rs, r, 5000.0));
        CHECK(r.ok);
        CHECK(!rec.recording());  // opened is not armed
        // The file is on disk with its complete zero-length header BEFORE the
        // recorder has been armed: what a crash from here would leave behind.
        const std::vector<fs::path> opened = filesIn(lateDir);
        CHECK(opened.size() == 1u);
        if (opened.size() == 1u) {
            const std::vector<unsigned char> husk = readAll(opened[0]);
            CHECK(husk.size() == 44u);
            if (husk.size() == 44u) {
                CHECK(std::memcmp(husk.data(), "RIFF", 4) == 0);
                CHECK(std::memcmp(husk.data() + 36, "data", 4) == 0);
                CHECK(husk[40] == 0 && husk[41] == 0 && husk[42] == 0 && husk[43] == 0);
            }
        }
        CHECK(rec.begin(std::move(r.file), err));
        CHECK(rec.recording());
        CHECK(rec.kind() == kind);
        CHECK(rec.samplesWritten() == 0u);
        feed(rec);
        rec.stop();
        CHECK(!rec.recording());
        CHECK(rec.samplesWritten() == inlineRec.samplesWritten());
        CHECK(rec.bytesWritten() == inlineRec.bytesWritten());
        CHECK(rec.bytesWritten() == (iq ? 80000u : 10000u));
        CHECK(opened.size() == 1u && rec.path() == opened[0].string());

        const std::vector<fs::path> a = filesIn(lateDir);
        const std::vector<fs::path> b = filesIn(inlineDir);
        CHECK(a.size() == 1u && b.size() == 1u);
        if (a.size() == 1u && b.size() == 1u) {
            const std::vector<unsigned char> late = readAll(a[0]);
            const std::vector<unsigned char> inl = readAll(b[0]);
            CHECK(late.size() == 44u + rec.bytesWritten());
            CHECK(late == inl);
        }
    }
}

// --- 4. THE EDGES ------------------------------------------------------------

// A late failure says what an inline start would have said, in the same words,
// and is not available before the open has come back.
void checkALateFailureSaysWhatAnInlineStartWould() {
    // A plain FILE squatting on the directory's path: the real failure.
    const fs::path blocker = g_scratch / "takes" / "blocker";
    std::error_code ec;
    fs::create_directories(blocker.parent_path(), ec);
    {
        std::ofstream f(blocker, std::ios::binary);
        f << "x";
    }
    const std::string badDir = (blocker / "sub").string();

    Recorder inlineRec;
    std::string inlineErr;
    CHECK(!inlineRec.start(RecordKind::Audio, badDir, 48000.0, inlineErr));
    CHECK(!inlineErr.empty());

    // Late: a delay, then the REAL opener's refusal.
    auto disk = makeDisk(400);
    Recorder rec;
    rec.bindOpener([disk](const Recorder::OpenRequest& req, Recorder::OpenedFile& out,
                          std::string& error) {
        std::this_thread::sleep_for(std::chrono::milliseconds(disk->blockMs.load()));
        return Recorder::openFile(req, out, error);
    });
    RecordStart rs;
    CHECK(rs.request(rec.opener(), requestFor(rec, RecordKind::Audio, badDir, 48000.0)));
    RecordStart::Result r;
    CHECK(!rs.poll(r));  // not before the open has come back
    CHECK(pollUntil(rs, r, 5000.0));
    CHECK(!r.ok);
    CHECK(!r.cancelled);
    CHECK(r.file.file == nullptr);
    CHECK(r.error == inlineErr);
    std::printf("  late failure: \"%s\"\n", r.error.c_str());
    // And the recorder it never armed is idle and startable.
    CHECK(!rec.recording());
    std::string err;
    CHECK(rec.start(RecordKind::Audio, dirFor("after-failure"), 48000.0, err));
    rec.stop();
}

// A second Record while one is pending: refused, one worker ever out, and the
// one that is out is not disturbed.
void checkOnlyOneOpenIsEverOut() {
    auto disk = makeDisk(1200);
    Recorder rec;
    RecordStart rs;
    CHECK(rs.request(slowOpener(disk), requestFor(rec, RecordKind::Audio, dirFor("one"), 48000.0)));
    CHECK(rs.pending());
    CHECK(rs.starting());
    int started = 0;
    for (int i = 0; i < 300; ++i) {
        if (rs.request(slowOpener(disk),
                       requestFor(rec, RecordKind::Audio, dirFor("one-second"), 48000.0))) {
            ++started;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(started == 0);
    CHECK(disk->calls.load() == 1);
    RecordStart::Result r;
    CHECK(pollUntil(rs, r, 5000.0));
    CHECK(r.ok);
    CHECK(!rs.pending());
    CHECK(disk->maxConcurrent.load() == 1);
    // The second request never made a directory.
    std::error_code ec;
    CHECK(!fs::exists(dirFor("one-second"), ec));
    // Free again once it has answered.
    CHECK(rs.request(slowOpener(disk), requestFor(rec, RecordKind::Audio, dirFor("one-again"), 48000.0)));
    CHECK(pollUntil(rs, r, 5000.0));
    CHECK(r.ok);
    CHECK(disk->maxConcurrent.load() == 1);
}

// STOP WHILE IT IS OPENING: no take, the file closed, nothing leaked, and no
// second open allowed to start alongside the one that cannot be interrupted.
void checkAStopWhileOpeningLeavesNoTake() {
    g_closes.store(0);
    auto disk = makeDisk(700);
    Recorder rec;
    RecordStart rs;
    CHECK(rs.request(slowOpener(disk), requestFor(rec, RecordKind::Audio, dirFor("cancel"), 48000.0)));
    CHECK(rs.starting());
    rs.cancel();
    // What the panel reads: no longer "starting", now "cancelling" - and the
    // worker still owns the path.
    CHECK(!rs.starting());
    CHECK(rs.cancelled());
    CHECK(rs.pending());
    CHECK(!rs.request(slowOpener(disk), requestFor(rec, RecordKind::Audio, dirFor("cancel-2"), 48000.0)));
    RecordStart::Result r;
    CHECK(!rs.poll(r));  // still out
    CHECK(pollUntil(rs, r, 5000.0));
    CHECK(r.cancelled);
    CHECK(!r.ok);
    CHECK(r.error.empty());
    CHECK(r.file.file == nullptr);
    // The file the worker opened was closed, once, and nothing was armed.
    CHECK(g_closes.load() == 1);
    CHECK(!rec.recording());
    CHECK(!rs.pending());
    CHECK(!rs.cancelled());
    // Left as Record-then-Stop always left it: a valid zero-sample WAV.
    const std::vector<fs::path> left = filesIn(dirFor("cancel"));
    CHECK(left.size() == 1u);
    if (left.size() == 1u) { CHECK(readAll(left[0]).size() == 44u); }
    CHECK(disk->calls.load() == 1);

    // A cancel is a mark on the open that is out, not on the next one: a Stop
    // with nothing opening must not eat the take after it.
    rs.cancel();
    CHECK(rs.request(slowOpener(disk), requestFor(rec, RecordKind::Audio, dirFor("cancel-next"), 48000.0)));
    CHECK(rs.starting());
    CHECK(pollUntil(rs, r, 5000.0));
    CHECK(r.ok);
    CHECK(!r.cancelled);
    CHECK(g_closes.load() == 1);  // the second file is open, not closed

    // A withdrawn take whose open then FAILS is withdrawn, not an error.
    disk->fails.store(true);
    CHECK(rs.request(slowOpener(disk), requestFor(rec, RecordKind::Audio, dirFor("cancel-fail"), 48000.0)));
    rs.cancel();
    CHECK(pollUntil(rs, r, 5000.0));
    CHECK(r.cancelled);
    CHECK(r.error.empty());
}

// QUIT does not wait for an open that is not coming back, and the file it opens
// when it finally does is closed.
void checkQuitAbandonsAWedgedOpenAndItsFileIsClosed() {
    g_closes.store(0);
    auto disk = makeDisk(2500);
    Recorder rec;
    double reapMs = 0.0;
    {
        RecordStart rs;
        CHECK(rs.request(slowOpener(disk), requestFor(rec, RecordKind::BasebandIq, dirFor("quit"), 2.0e6)));
        rs.cancel();  // what ~AppWindow's stop*Recording does first
        const double t0 = nowMs();
        rs.reap();
        reapMs = nowMs() - t0;
        CHECK(!rs.pending());
        rs.reap();  // twice is a no-op, not a second detached thread
    }
    // Bounded by kQuitGrace, not by the 2500 ms the disk is taking.
    CHECK(reapMs < 1200.0);
    CHECK(disk->concurrent.load() == 1);  // still inside the disk: abandoned, not joined
    CHECK(g_closes.load() == 0);
    // The abandoned worker finishes on its own and lets the file go - no leaked
    // handle, and the opener it ran never touched anything that was destroyed.
    const double until = nowMs() + 6000.0;
    while (nowMs() < until && g_closes.load() == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    CHECK(g_closes.load() == 1);
    CHECK(disk->concurrent.load() == 0);
    // Destroying the class with an open out does the same as reap().
    g_closes.store(0);
    auto disk2 = makeDisk(1500);
    const double t1 = nowMs();
    {
        RecordStart rs;
        CHECK(rs.request(slowOpener(disk2), requestFor(rec, RecordKind::Audio, dirFor("quit-dtor"), 48000.0)));
    }
    CHECK(nowMs() - t1 < 1200.0);
    const double until2 = nowMs() + 6000.0;
    while (nowMs() < until2 && g_closes.load() == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    CHECK(g_closes.load() == 1);
}

// "Still opening" has to be visible somewhere: said once when the open has been
// out too long, once when it comes back - never once a frame, and never for a
// disk that answered at once.
void checkASlowOpenIsReportedOnceNotEveryFrame() {
    auto disk = makeDisk(700);
    Recorder rec;
    RecordStart rs;
    rs.setStuckAfterForTest(std::chrono::milliseconds(200));
    double s = -1.0;
    CHECK(rs.request(slowOpener(disk), requestFor(rec, RecordKind::Audio, dirFor("notice"), 48000.0)));
    CHECK(rs.takeNotice(s) == RecordStart::Notice::None);
    std::this_thread::sleep_for(std::chrono::milliseconds(350));
    CHECK(rs.elapsedS() >= 0.3);
    CHECK(rs.takeNotice(s) == RecordStart::Notice::Stuck);
    CHECK(s >= 0.2);
    for (int i = 0; i < 40; ++i) { CHECK(rs.takeNotice(s) == RecordStart::Notice::None); }
    RecordStart::Result r;
    CHECK(pollUntil(rs, r, 5000.0));
    CHECK(rs.takeNotice(s) == RecordStart::Notice::Recovered);
    CHECK(s >= 0.6);
    CHECK(rs.takeNotice(s) == RecordStart::Notice::None);
    CHECK(rs.elapsedS() == 0.0);

    // A disk that answers at once is never mentioned.
    disk->blockMs.store(0);
    CHECK(rs.request(slowOpener(disk), requestFor(rec, RecordKind::Audio, dirFor("notice-fast"), 48000.0)));
    CHECK(pollUntil(rs, r, 3000.0));
    CHECK(rs.takeNotice(s) == RecordStart::Notice::None);
}

// A throw on the worker must not be rethrown out of the frame loop.
void checkAThrowingDiskIsContained() {
    auto disk = makeDisk(0);
    disk->throws.store(true);
    Recorder rec;
    RecordStart rs;
    CHECK(rs.request(slowOpener(disk), requestFor(rec, RecordKind::Audio, dirFor("throws"), 48000.0)));
    RecordStart::Result r;
    bool got = false;
    try {
        got = pollUntil(rs, r, 3000.0);
    } catch (...) {
        CHECK(false);  // reached the frame loop
    }
    CHECK(got);
    CHECK(!r.ok);
    CHECK(!r.error.empty());
    CHECK(r.file.file == nullptr);
    // An empty opener is refused the same way, never called.
    CHECK(rs.request(Recorder::Opener{}, requestFor(rec, RecordKind::Audio, dirFor("throws"), 48000.0)));
    CHECK(pollUntil(rs, r, 3000.0));
    CHECK(!r.ok);
    CHECK(!r.error.empty());
}

// --- 5. THE REAL HANDLERS, THROUGH A REAL WINDOW -----------------------------

// THE FIELD HANG, through the code the button, the key and the browser reach: a
// 2.5 s open per take against an 800 ms threshold, both takes at once. The loop
// keeps beating, nothing is recording until the file is there, the take then
// starts on the frame that finds the file, and a take that was asked for is
// never lost.
void checkAppWindowStartsNeverHoldAFrame() {
    AppWindow app;
    auto disk = makeDisk(2500);
    Access::bindOpener(app, slowOpener(disk));
    Access::setRecordDir(app, dirFor("app-slow"));

    HangWatchdog w;
    startWatchdog(w, reportDir("appwindow"));
    const double t0 = nowMs();
    bool first = true;
    bool recordingTooEarly = false;
    const LoopStats s = runFrameLoop(w, 3400.0, [&] {
        const double nowS = (nowMs() - t0) / 1000.0;
        if (first) {
            first = false;
            // The Record audio button and key, and the web remote's recordIq.
            CHECK(Access::startAudio(app));
            Access::applyRecord(app, true, std::nullopt);
            CHECK(Access::audioPending(app));
            CHECK(Access::iqPending(app));
        }
        Access::poll(app, nowS);
        // Nothing is recording while the disk has not answered.
        if (nowS < 2.2 && (Access::audioRecording(app) || Access::iqRecording(app))) {
            recordingTooEarly = true;
        }
    });
    CHECK(w.reportsWritten() == 0u);
    CHECK(s.frames > 100);
    CHECK(s.worstGapMs < 800.0);
    CHECK(!recordingTooEarly);
    // Both asked once, from workers; both takes now running.
    CHECK(disk->calls.load() == 2);
    CHECK(!disk->askedFromCaller.load());
    CHECK(Access::audioRecording(app));
    CHECK(Access::iqRecording(app));
    CHECK(!Access::audioPending(app));
    CHECK(!Access::iqPending(app));
    CHECK(Access::recordError(app).empty());
    // The elapsed clock starts when the take does, not when it was asked for.
    CHECK(Access::audioStartS(app) > 2.4);
    CHECK(Access::iqStartS(app) > 2.4);
    std::printf("  AppWindow starts: %d frames in 3.4 s, worst gap %.0f ms, hang reports %u\n",
                s.frames, s.worstGapMs, w.reportsWritten());
    w.stop();

    // Both takes end as they always did: headers on disk, valid.
    Access::stopAudio(app);
    Access::applyRecord(app, false, std::nullopt);
    CHECK(!Access::audioRecording(app));
    CHECK(!Access::iqRecording(app));
    const std::vector<fs::path> files = filesIn(dirFor("app-slow"));
    CHECK(files.size() == 2u);
    for (const fs::path& f : files) {
        const std::vector<unsigned char> b = readAll(f);
        CHECK(b.size() == 44u);
        CHECK(b.size() < 4 || std::memcmp(b.data(), "RIFF", 4) == 0);
    }
}

// The rest of the edges, on one window and a fast-enough disk, one after
// another. Each leaves both recorders idle.
void checkAppWindowEdges() {
    AppWindow app;
    auto disk = makeDisk(0);
    Access::bindOpener(app, slowOpener(disk));
    const auto idle = [&] {
        return !Access::audioRecording(app) && !Access::iqRecording(app) &&
               !Access::audioPending(app) && !Access::iqPending(app);
    };

    // --- a failure that comes late: reported late, in the words of an inline
    //     start, and a Record after it works and clears it.
    {
        Access::setRecordDir(app, dirFor("app-fail"));
        disk->blockMs.store(400);
        disk->fails.store(true);
        CHECK(Access::startAudio(app));
        Access::poll(app, 0.0);
        CHECK(Access::recordError(app).empty());  // not before the open is back
        CHECK(pumpApp(app, 5000.0, [&] { return !Access::audioPending(app); }));
        CHECK(Access::recordError(app).rfind("recorder: cannot create", 0) == 0);
        CHECK(!Access::audioRecording(app));
        // ...and the ordinary refusal, which needs no disk, is still at once.
        disk->fails.store(false);
        disk->blockMs.store(0);
        CHECK(Access::startAudio(app));
        CHECK(pumpApp(app, 5000.0, [&] { return Access::audioRecording(app); }));
        CHECK(Access::recordError(app).empty());  // a take that starts clears it
        Access::stopAudio(app);
        CHECK(idle());
    }

    // --- a second Record while one is pending changes nothing: one open, one
    //     file, however it is asked (button, key, browser).
    {
        g_closes.store(0);
        Access::setRecordDir(app, dirFor("app-twice"));
        disk->calls.store(0);
        disk->blockMs.store(800);
        CHECK(Access::startAudio(app));
        CHECK(Access::startAudio(app));                           // the button again, the key
        Access::applyRecord(app, std::nullopt, true);              // the browser
        Access::applyRecord(app, std::nullopt, true);
        CHECK(Access::audioStarting(app));
        CHECK(!Access::audioRecording(app));
        CHECK(pumpApp(app, 5000.0, [&] { return Access::audioRecording(app); }));
        CHECK(disk->calls.load() == 1);
        CHECK(disk->maxConcurrent.load() == 1);
        // Recording already: a Record is still a no-op.
        CHECK(Access::startAudio(app));
        CHECK(disk->calls.load() == 1);
        Access::stopAudio(app);
        CHECK(filesIn(dirFor("app-twice")).size() == 1u);
        CHECK(idle());
    }

    // --- Stop while it is opening, by every route: no take, the file closed,
    //     nothing said, and Record works again afterwards.
    {
        Access::setRecordDir(app, dirFor("app-stop"));
        disk->blockMs.store(600);
        struct Route {
            const char* name;
            bool iq;
            std::function<void()> stop;
        };
        const std::vector<Route> routes = {
            {"Stop audio / toolbar / receiver stop", false, [&] { Access::stopAudio(app); }},
            {"web recordAudio=false", false,
             [&] { Access::applyRecord(app, std::nullopt, false); }},
            {"web recordIq=false", true, [&] { Access::applyRecord(app, false, std::nullopt); }},
            {"Stop IQ", true, [&] { Access::stopIq(app); }},
        };
        int n = 0;
        for (const Route& route : routes) {
            g_closes.store(0);
            disk->calls.store(0);
            Access::setRecordDir(app, dirFor("app-stop") + std::to_string(n++));
            CHECK(route.iq ? Access::startIq(app) : Access::startAudio(app));
            // The disk has been asked, so this is a Stop pressed while the open
            // is in progress rather than before its worker has begun.
            const double asked = nowMs() + 3000.0;
            while (nowMs() < asked && disk->calls.load() < 1) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            route.stop();
            if (!route.iq) {
                CHECK(!Access::audioStarting(app));
                CHECK(Access::audioCancelled(app));  // "Cancelling..." on the panel
                // A Record in the gap is not a take: the worker still owns the path.
                CHECK(!Access::startAudio(app));
                CHECK(disk->calls.load() == 1);
            }
            CHECK(pumpApp(app, 5000.0, [&] { return idle(); }));
            CHECK(g_closes.load() == 1);
            CHECK(Access::recordError(app).empty());
            CHECK(Access::recordNotice(app).empty());
            std::printf("  stop while opening, %s: no take, file closed\n", route.name);
        }
        // A start after a withdrawn one works.
        Access::setRecordDir(app, dirFor("app-stop-after"));
        disk->blockMs.store(0);
        CHECK(Access::startAudio(app));
        CHECK(pumpApp(app, 5000.0, [&] { return Access::audioRecording(app); }));
        Access::stopAudio(app);
        CHECK(idle());
    }

    // --- an IQ take whose input rate moved while its file was opening is not
    //     started: its header would disagree with its samples.
    {
        g_closes.store(0);
        Access::setRecordDir(app, dirFor("app-rate"));
        disk->blockMs.store(300);
        CHECK(Access::startIq(app));
        Access::moveIqRate(app, 1.0);
        CHECK(pumpApp(app, 5000.0, [&] { return !Access::iqPending(app); }));
        CHECK(!Access::iqRecording(app));
        CHECK(Access::recordNotice(app).find("input rate") != std::string::npos);
        CHECK(Access::recordError(app).empty());
        CHECK(g_closes.load() == 1);
        // The next one, at a steady rate, starts.
        disk->blockMs.store(0);
        Access::setRecordDir(app, dirFor("app-rate-after"));
        CHECK(Access::startIq(app));
        CHECK(pumpApp(app, 5000.0, [&] { return Access::iqRecording(app); }));
        CHECK(Access::recordNotice(app).empty());
        Access::stopIq(app);
        CHECK(idle());
    }
}

// APPLICATION EXIT WHILE ONE IS PENDING: the window is destroyed without
// waiting for a wedged open (bounded by the quit grace, not by the disk), the
// file the open later produces is closed, and nothing the abandoned worker
// uses was destroyed under it.
void checkExitWhileAStartIsPending() {
    g_closes.store(0);
    auto disk = makeDisk(2500);
    double destroyMs = 0.0;
    {
        auto app = std::make_unique<AppWindow>();
        Access::bindOpener(*app, slowOpener(disk));
        Access::setRecordDir(*app, dirFor("app-exit"));
        CHECK(Access::startAudio(*app));
        CHECK(Access::startIq(*app));
        CHECK(Access::audioPending(*app));
        CHECK(Access::iqPending(*app));
        const double t0 = nowMs();
        app.reset();
        destroyMs = nowMs() - t0;
    }
    // Two wedged opens, each given the 250 ms grace in ~AppWindow and then let
    // go: well inside the 2.5 s the disk is taking.
    CHECK(destroyMs < 1800.0);
    std::printf("  exit with two opens wedged: ~AppWindow took %.0f ms\n", destroyMs);
    // The abandoned workers finish on their own and let go of both files.
    const double until = nowMs() + 8000.0;
    while (nowMs() < until && g_closes.load() < 2) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    CHECK(g_closes.load() == 2);
    CHECK(disk->concurrent.load() == 0);
}

}  // namespace

int main() {
    isolate();
    checkTheOpenIsNotMadeOnTheGuiThread();
    checkTheSynchronousStartIsReported();
    checkSlowOpenDoesNotStallTheFrameLoop();
    checkALateTakeIsTheSameTakeAsAnInlineOne();
    checkALateFailureSaysWhatAnInlineStartWould();
    checkOnlyOneOpenIsEverOut();
    checkAStopWhileOpeningLeavesNoTake();
    checkQuitAbandonsAWedgedOpenAndItsFileIsClosed();
    checkASlowOpenIsReportedOnceNotEveryFrame();
    checkAThrowingDiskIsContained();
    checkAppWindowStartsNeverHoldAFrame();
    checkAppWindowEdges();
    checkExitWhileAStartIsPending();
    std::error_code ec;
    if (g_checksFailed == 0) { fs::remove_all(g_scratch, ec); }
    return testSummary("test_record_start");
}
