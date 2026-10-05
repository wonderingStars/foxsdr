// A patch speaker's file is opened off the frame loop.
//
// WHAT WENT WRONG. AppWindow::patchPublishSets makes each patch speaker's output
// when its radio starts (and again when the speaker's output is changed), and it
// made a WAV file's by calling core::patch::makeWavDest, whose WavDest::start is
// Recorder::start: create the recordings folder, open the file, write and flush
// the 44 header bytes - three synchronous file-system calls, on the thread that
// draws the window, from the patch page's reconcile. The MP3 destination did the
// folder half of it (create_directories and is_directory) on the same thread
// before handing the file to its own worker. That is the freeze the 0.99.63
// Record fix removed from the Record button (a recordings folder that was slow to
// answer - a synchronised or network folder, a drive that had spun down, a
// scanner holding the path - and a window that stopped drawing for as long as it
// took), left standing in the page that makes up to five of these files at once.
// docs/DIAGNOSTICS.md, "The window does no disk work", has the audit.
//
// WHAT IS TESTED, and at which level. NOTHING HERE DEPENDS ON A REAL SLOW DISK:
// the blocking step of a WAV is Recorder::Opener and of an MP3 the folder maker,
// and a test binds ones that sleep and then do the REAL work - a slow disk as far
// as the frame loop can tell, with the real file behind it.
//
//   1. THE HARNESS CAN SEE THE FAULT. The same slow opener called on the thread
//      that heartbeats is reported by a real HangWatchdog.
//   2. THE REAL HANDLER, through a real AppWindow, under a real HangWatchdog
//      (800 ms threshold) and a frame loop: a speaker on a running patch radio,
//      patchPublishSets called every frame as the patch page calls it, a 2.5 s
//      open. Before the fix the frame loop stalled for the whole open and the
//      watchdog filed a report (worst frame gap 2511 ms for the WAV, 2510 ms for
//      the MP3); after it nothing waits - for the WAV or the MP3 - and the sound
//      that arrived while the file was opening is IN the file.
//   3. THE DESTINATION ON ITS OWN: a file opened late is byte for byte the file
//      an inline start makes; a late failure says what Recorder::start said; a
//      disk slower than the pre-roll drops the overflow and says so; a
//      destination destroyed while its open is wedged does not wait for it and
//      the file the open later makes is closed.
//   4. THE WIRING, because no test can stage every slow disk: patchPublishSets
//      and the destinations' factories reach the disk through a worker only.
//
// WHAT IS NOT COVERED. The face's drawing (the "waiting for the disk" words, the
// red error line) is ImGui output no test reads; the state it reads is checked.
// What a WAV's FINALISE does on the GUI thread when its set is retired (the header
// patch and the close in ~WavDest) is not moved and not tested: it is the next
// thing to move. Nothing here has met a genuinely slow disk.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <mutex>
#include <sstream>
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
#include "core/mp3_writer.hpp"
#include "core/patch_audio.hpp"
#include "core/patch_graph.hpp"
#include "core/patch_plan.hpp"
#include "core/patch_radio.hpp"
#include "core/recorder.hpp"
#include "gui/app_window.hpp"
#include "source/siggen_source.hpp"
#include "test_check.hpp"

namespace fs = std::filesystem;
namespace pc = cascade::core::patch;
using cascade::core::HangWatchdog;
using cascade::core::Recorder;

namespace cascade::gui {

// The friend AppWindow names for the test files that drive its private members.
struct AppWindowTestAccess {
    static void setSeams(AppWindow& a, pc::DestSeams s) { a.patchDestSeams_ = std::move(s); }
    static void setRecordDir(AppWindow& a, std::string d) { a.recordDir_ = std::move(d); }

    // A patch as the parts bin builds it - a radio, a channel, a demodulator and a
    // speaker wired behind - with the radio RUNNING on the signal generator, as a
    // patch radio is, so its reader thread feeds the speaker for real. `device`
    // is the speaker's output key ("" is the default, a WAV file; "mp3").
    static pc::NodeId buildSpeakerPatch(AppWindow& a, const std::string& device, bool start) {
        a.patchSeeded_ = true;
        const pc::NodeId r = a.patchGraph_.addNode(pc::NodeKind::Radio, "Radio", pc::PortType::Iq);
        const pc::NodeId c = a.patchGraph_.addNode(pc::NodeKind::Channel, "Tone", pc::PortType::Iq);
        const pc::NodeId d = a.patchGraph_.addNode(pc::NodeKind::Demod, "AM", pc::PortType::Iq);
        const pc::NodeId s = a.patchGraph_.addNode(pc::NodeKind::Sink, "Speaker", pc::PortType::Audio);
        auto gen = std::make_unique<cascade::source::SigGenSource>(2.4e6);
        gen->sigGen().setTone(0, 300000.0, -30.0f);
        a.patchRadios_[r] = std::make_unique<pc::PatchRadio>(r, std::move(gen), "generator");
        // The channel sits 300 kHz above wherever the generator says its centre is.
        if (pc::Node* n = a.patchGraph_.mutableNode(c)) {
            n->freqHz = a.patchRadios_[r]->centreHz() + 300000.0;
        }
        if (pc::Node* n = a.patchGraph_.mutableNode(d)) { n->mode = 2; }
        if (pc::Node* n = a.patchGraph_.mutableNode(s)) { n->device = device; }
        CHECK(a.patchGraph_.connect(r, 0, c, 0) == pc::Connect::Ok);
        CHECK(a.patchGraph_.connect(c, 0, d, 0) == pc::Connect::Ok);
        CHECK(a.patchGraph_.connect(d, 0, s, 0) == pc::Connect::Ok);
        if (start) {
            std::string err;
            CHECK(a.patchRadios_[r]->start(err));
        }
        a.patchPlan_ = pc::compile(a.patchGraph_, a.patchRadioInfos(), nullptr, false);
        CHECK(a.patchPlan_.runnable);
        CHECK(a.patchPlan_.sinks.size() == 1u);
        return s;
    }

    // What the patch page calls once a frame, after the plan is compiled.
    static void publish(AppWindow& a) { a.patchPublishSets(); }

    static std::shared_ptr<pc::AudioDest> destOf(AppWindow& a, pc::NodeId sink) {
        return pc::destFor(a.patchDests_, sink);
    }
    static bool published(AppWindow& a) { return !a.patchRadioSig_.empty(); }
    static auto& radios(AppWindow& a) { return a.patchRadios_; }
    static std::string destError(AppWindow& a, pc::NodeId sink) {
        const auto it = a.patchDestError_.find(sink);
        return it == a.patchDestError_.end() ? std::string() : it->second;
    }
    // The page closing: radios stop and are destroyed, then the outputs go.
    static void closeRadios(AppWindow& a) {
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

// Per-user directories pointed at a scratch folder (process id in its name) and
// every network-facing URL at a port nothing listens on.
void isolate() {
#if defined(_WIN32)
    const int pid = _getpid();
#else
    const int pid = static_cast<int>(getpid());
#endif
    g_scratch = fs::temp_directory_path() / ("foxsdr_patch_dest_" + std::to_string(pid));
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

// --- a slow disk -------------------------------------------------------------

struct SlowDisk {
    std::atomic<int> blockMs{0};
    std::atomic<int> calls{0};
    std::atomic<bool> askedFromCaller{false};
    const std::thread::id caller = std::this_thread::get_id();
};

std::shared_ptr<SlowDisk> makeDisk(int blockMs) {
    auto d = std::make_shared<SlowDisk>();
    d->blockMs.store(blockMs);
    return d;
}

// An opener that behaves like a slow disk, then does the REAL open.
Recorder::Opener slowOpener(std::shared_ptr<SlowDisk> d) {
    return [d](const Recorder::OpenRequest& req, Recorder::OpenedFile& out,
               std::string& error) -> bool {
        if (std::this_thread::get_id() == d->caller) { d->askedFromCaller.store(true); }
        d->calls.fetch_add(1);
        std::this_thread::sleep_for(std::chrono::milliseconds(d->blockMs.load()));
        return Recorder::openFile(req, out, error);
    };
}

// A folder maker that behaves like a slow disk, then does the REAL work.
std::function<bool(const std::string&)> slowMakeDirectory(std::shared_ptr<SlowDisk> d) {
    return [d](const std::string& dir) -> bool {
        if (std::this_thread::get_id() == d->caller) { d->askedFromCaller.store(true); }
        d->calls.fetch_add(1);
        std::this_thread::sleep_for(std::chrono::milliseconds(d->blockMs.load()));
        std::error_code ec;
        fs::create_directories(dir, ec);
        return !ec && fs::is_directory(dir, ec);
    };
}

// --- the frame loop, as test_record_start drives it --------------------------

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

// --- 1. THE HARNESS CAN SEE THE FAULT -----------------------------------------

// THE CONTROL. The same slow opener, called where WavDest::start used to call it:
// on the thread that heartbeats.
void checkTheSynchronousOpenIsReported() {
    auto disk = makeDisk(2500);
    HangWatchdog w;
    startWatchdog(w, reportDir("control"));
    Recorder rec;
    rec.bindOpener(slowOpener(disk));
    std::string err;
    CHECK(rec.start(cascade::core::RecordKind::Audio, dirFor("control"), 48000.0, err));
    rec.stop();
    CHECK(w.reportsWritten() == 1u);
    CHECK(disk->askedFromCaller.load());
    w.stop();
}

// --- 2. THE REAL HANDLER, THROUGH A REAL WINDOW --------------------------------

std::vector<unsigned char> readBytes(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<unsigned char>((std::istreambuf_iterator<char>(in)),
                                      std::istreambuf_iterator<char>());
}

// The regular files in `dir` with the extension `ext`.
std::vector<fs::path> filesIn(const std::string& dir, const char* ext) {
    std::vector<fs::path> out;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        if (e.is_regular_file(ec) && e.path().extension() == ext) { out.push_back(e.path()); }
    }
    return out;
}

// THE FIELD HANG, through the code the patch page reaches: a 2.5 s open against
// an 800 ms threshold. The loop must keep beating, the watchdog must write
// nothing, and the file must still arrive WITH the sound that was offered while it
// was opening - a fix that merely dropped the speaker's file would pass everything
// but the last of these.
void checkAppWindowWavNeverHoldsAFrame() {
    AppWindow app;
    auto disk = makeDisk(2500);
    pc::DestSeams seams;
    seams.wavOpener = slowOpener(disk);
    Access::setSeams(app, seams);
    Access::setRecordDir(app, dirFor("app-wav"));
    const pc::NodeId sink = Access::buildSpeakerPatch(app, std::string{}, /*start=*/true);

    HangWatchdog w;
    startWatchdog(w, reportDir("app-wav"));
    const LoopStats s = runFrameLoop(w, 3600.0, [&] { Access::publish(app); });
    CHECK(w.reportsWritten() == 0u);
    CHECK(s.frames > 100);
    CHECK(s.worstGapMs < 800.0);
    // The set was published on the first frame, with the speaker's output in it,
    // not held back until the disk answered.
    CHECK(Access::published(app));
    std::shared_ptr<pc::AudioDest> dest = Access::destOf(app, sink);
    CHECK(dest != nullptr);
    CHECK(Access::destError(app, sink).empty());
    CHECK(disk->calls.load() == 1);
    CHECK(!disk->askedFromCaller.load());
    std::printf("  AppWindow WAV speaker: %d frames in 3.6 s, worst gap %.0f ms, hang reports %u\n",
                s.frames, s.worstGapMs, w.reportsWritten());
    w.stop();

    // THE RADIO RAN THE WHOLE TIME and fed the speaker for real: stop it (its
    // reader is joined, so nothing is offered after this), and every sample it
    // offered must be in the file - the 2.5 s that arrived while the file was
    // opening included.
    for (auto& [id, radio] : Access::radios(app)) {
        (void)id;
        radio->stop();
    }
    const std::uint64_t offered = dest ? dest->samples() : 0u;
    CHECK(offered > 48000u * 2u);  // not vacuous: several seconds of real sound
    CHECK(dest && dest->error().empty());
    Access::closeRadios(app);
    dest.reset();  // the last reference: the file is finalised here
    const std::vector<fs::path> files = filesIn(dirFor("app-wav"), ".wav");
    CHECK(files.size() == 1u);
    if (files.size() == 1u) {
        const std::vector<unsigned char> bytes = readBytes(files[0]);
        CHECK(bytes.size() == 44u + 2u * offered);
        std::printf("  the file holds %zu bytes for %llu samples offered\n", bytes.size(),
                    static_cast<unsigned long long>(offered));
    }
}

void checkAppWindowMp3NeverHoldsAFrame() {
    if (!cascade::core::Mp3Writer::available()) {
        SKIP_LINUX("no MP3 encoder on this build: the patch writes WAV instead");
        return;
    }
    AppWindow app;
    auto disk = makeDisk(2500);
    pc::DestSeams seams;
    seams.makeDirectory = slowMakeDirectory(disk);
    Access::setSeams(app, seams);
    Access::setRecordDir(app, dirFor("app-mp3"));
    const pc::NodeId sink = Access::buildSpeakerPatch(app, "mp3", /*start=*/true);

    HangWatchdog w;
    startWatchdog(w, reportDir("app-mp3"));
    const LoopStats s = runFrameLoop(w, 3600.0, [&] { Access::publish(app); });
    CHECK(w.reportsWritten() == 0u);
    CHECK(s.frames > 100);
    CHECK(s.worstGapMs < 800.0);
    CHECK(Access::published(app));
    CHECK(Access::destOf(app, sink) != nullptr);
    CHECK(disk->calls.load() == 1);
    CHECK(!disk->askedFromCaller.load());
    std::printf("  AppWindow MP3 speaker: %d frames in 3.6 s, worst gap %.0f ms, hang reports %u\n",
                s.frames, s.worstGapMs, w.reportsWritten());
    w.stop();
    Access::closeRadios(app);
    // The folder was made (late) and the file is there and has sound in it.
    const std::vector<fs::path> files = filesIn(dirFor("app-mp3"), ".mp3");
    CHECK(files.size() == 1u);
    if (files.size() == 1u) { CHECK(fs::file_size(files[0]) > 0u); }
}

// --- 3. THE DESTINATION ON ITS OWN ---------------------------------------------

// The deterministic samples both takes are fed.
std::vector<float> testSignal(std::size_t n) {
    std::uint32_t lcg = 0x2468ACE1u;
    std::vector<float> v(n);
    for (float& x : v) {
        lcg = lcg * 1664525u + 1013904223u;
        x = (static_cast<float>(static_cast<std::int32_t>(lcg)) / 2147483648.0f) * 1.2f;
    }
    return v;
}

// A take whose file was opened late is byte for byte the take an inline Recorder
// makes - the sound that arrived while the file was opening is written ahead of
// the rest, in order, with nothing missing and nothing twice.
void checkALateWavIsTheSameFileAsAnInlineOne() {
    const std::vector<float> signal = testSignal(48000);  // one second
    constexpr std::size_t kBlock = 960;                   // 20 ms

    // The reference: the same samples through an inline Recorder.
    const std::string refDir = dirFor("late-ref");
    Recorder ref;
    std::string err;
    CHECK(ref.start(cascade::core::RecordKind::Audio, refDir, 48000.0, err));
    for (std::size_t at = 0; at < signal.size(); at += kBlock) {
        ref.writeAudio(signal.data() + at, std::min(kBlock, signal.size() - at));
    }
    ref.stop();

    // The late one: a 400 ms open, blocks offered at the pace of real time from
    // this thread (standing in for the radio's reader), so about twenty arrive
    // before the file is there.
    auto disk = makeDisk(400);
    pc::DestSeams seams;
    seams.wavOpener = slowOpener(disk);
    const std::string lateDir = dirFor("late");
    std::string derr;
    std::shared_ptr<pc::AudioDest> d =
        pc::makeWavDestAsync(lateDir, pc::patchFilePrefix(9, "Late"), derr, &seams);
    CHECK(d != nullptr);
    if (!d) { return; }
    CHECK(derr.empty());
    CHECK(d->describe().rfind("WAV  patch-9-Late_", 0) == 0);
    CHECK(d->error().empty());
    for (std::size_t at = 0; at < signal.size(); at += kBlock) {
        d->write(signal.data() + at, std::min(kBlock, signal.size() - at));
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    CHECK(d->samples() == signal.size());
    CHECK(d->error().empty());
    d.reset();  // finalises the file

    const std::vector<fs::path> a = filesIn(lateDir, ".wav");
    const std::vector<fs::path> b = filesIn(refDir, ".wav");
    CHECK(a.size() == 1u && b.size() == 1u);
    if (a.size() == 1u && b.size() == 1u) {
        const std::vector<unsigned char> late = readBytes(a[0]);
        const std::vector<unsigned char> inl = readBytes(b[0]);
        CHECK(late.size() == 44u + 2u * signal.size());
        CHECK(late == inl);
    }
    CHECK(disk->calls.load() == 1);
}

// A late failure says what an inline start would have said, in the same words,
// and is not available before the open has come back; the destination is harmless
// to write to meanwhile and afterwards.
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
    CHECK(!inlineRec.start(cascade::core::RecordKind::Audio, badDir, 48000.0, inlineErr,
                           pc::patchFilePrefix(4, "Bad")));
    CHECK(!inlineErr.empty());

    // The same, through the application's own factory, behind a 400 ms answer.
    auto disk = makeDisk(400);
    pc::DestSeams seams;
    seams.wavOpener = slowOpener(disk);
    std::string err;
    std::shared_ptr<pc::AudioDest> d =
        pc::makeWavDestAsync(badDir, pc::patchFilePrefix(4, "Bad"), err, &seams);
    CHECK(d != nullptr);  // never null for the disk's sake: it says so through error()
    CHECK(err.empty());
    if (!d) { return; }
    CHECK(d->error().empty());  // not before the open has come back
    const std::vector<float> blip(960, 0.25f);
    d->write(blip.data(), blip.size());  // harmless while it is opening
    const double until = nowMs() + 5000.0;
    while (nowMs() < until && d->error().empty()) {
        d->write(blip.data(), blip.size());
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(d->error() == inlineErr);
    std::printf("  late failure: \"%s\"\n", d->error().c_str());
    d->write(blip.data(), blip.size());  // and harmless once it has failed
    CHECK(d->error() == inlineErr);
    CHECK(d->describe().rfind("WAV  patch-4-Bad_", 0) == 0);
    // The blocking factory still refuses outright, as it always did.
    std::string blockingErr;
    CHECK(pc::makeWavDest(badDir, pc::patchFilePrefix(4, "Bad"), blockingErr) == nullptr);
    CHECK(blockingErr == inlineErr);
}

// A disk slower than the pre-roll holds: the overflow is dropped and SAID, the
// rest is in the file, in order.
void checkADiskSlowerThanThePreRollDropsAndSaysSo() {
    auto disk = makeDisk(600);
    pc::DestSeams seams;
    seams.wavOpener = slowOpener(disk);
    const std::string dir = dirFor("preroll");
    std::string err;
    std::shared_ptr<pc::AudioDest> d = pc::makeWavDestAsync(dir, "p", err, &seams);
    CHECK(d != nullptr);
    if (!d) { return; }
    // 25 s offered at once while the file is opening: 20 s fit.
    const std::vector<float> long25 = testSignal(25u * 48000u);
    d->write(long25.data(), long25.size());
    CHECK(d->error().find("took too long to open") != std::string::npos);
    // The open answers; the next block arms the recorder and writes the pre-roll.
    std::this_thread::sleep_for(std::chrono::milliseconds(900));
    const std::vector<float> tail(960, 0.5f);
    d->write(tail.data(), tail.size());
    CHECK(d->error().find("took too long to open") != std::string::npos);  // still said
    d.reset();
    const std::vector<fs::path> files = filesIn(dir, ".wav");
    CHECK(files.size() == 1u);
    if (files.size() == 1u) {
        const std::vector<unsigned char> bytes = readBytes(files[0]);
        // 20 s of pre-roll, then the block that armed it.
        CHECK(bytes.size() == 44u + 2u * (20u * 48000u + tail.size()));
    }
}

std::atomic<int> g_closes{0};
void countingClose(std::FILE* f) {
    if (f != nullptr) {
        g_closes.fetch_add(1);
        std::fclose(f);
    }
}

// An opener that hands back the real file with its close counted, so a test can
// see that the file an abandoned worker eventually opens is closed.
Recorder::Opener countingOpener(std::shared_ptr<SlowDisk> d) {
    return [d](const Recorder::OpenRequest& req, Recorder::OpenedFile& out,
               std::string& error) -> bool {
        d->calls.fetch_add(1);
        std::this_thread::sleep_for(std::chrono::milliseconds(d->blockMs.load()));
        if (!Recorder::openFile(req, out, error)) { return false; }
        std::FILE* raw = out.file.release();
        out.file = Recorder::FilePtr(raw, &countingClose);
        return true;
    };
}

// A destination destroyed while its open is wedged does not wait for the disk
// (the window is being closed, or the patch stopped), and the file the open makes
// when it finally answers is closed - no handle leaks, nothing writes to it.
void checkADestroyedWavDoesNotWaitForItsOpen() {
    g_closes.store(0);
    auto disk = makeDisk(2500);
    pc::DestSeams seams;
    seams.wavOpener = countingOpener(disk);
    std::string err;
    const double t0 = nowMs();
    {
        std::shared_ptr<pc::AudioDest> d =
            pc::makeWavDestAsync(dirFor("destroyed"), "q", err, &seams);
        CHECK(d != nullptr);
        const std::vector<float> blip(960, 0.25f);
        d->write(blip.data(), blip.size());
    }
    CHECK(nowMs() - t0 < 500.0);  // not the 2500 ms the disk is taking
    const double until = nowMs() + 8000.0;
    while (nowMs() < until && g_closes.load() == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    CHECK(g_closes.load() == 1);

    // Opened but never written to before it went: the file is closed too (on a
    // thread of its own), and is a valid zero-sample WAV.
    g_closes.store(0);
    disk->blockMs.store(0);
    {
        std::shared_ptr<pc::AudioDest> d =
            pc::makeWavDestAsync(dirFor("destroyed-open"), "q", err, &seams);
        CHECK(d != nullptr);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }
    const double until2 = nowMs() + 5000.0;
    while (nowMs() < until2 && g_closes.load() == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    CHECK(g_closes.load() == 1);
    const std::vector<fs::path> files = filesIn(dirFor("destroyed-open"), ".wav");
    CHECK(files.size() == 1u);
    if (files.size() == 1u) { CHECK(fs::file_size(files[0]) == 44u); }
}

// APPLICATION EXIT WITH A SPEAKER'S OPEN WEDGED: the window is destroyed without
// waiting for it.
void checkExitWhileASpeakerFileIsOpening() {
    g_closes.store(0);
    auto disk = makeDisk(2500);
    double destroyMs = 0.0;
    {
        auto app = std::make_unique<AppWindow>();
        pc::DestSeams seams;
        seams.wavOpener = countingOpener(disk);
        Access::setSeams(*app, seams);
        Access::setRecordDir(*app, dirFor("app-exit"));
        Access::buildSpeakerPatch(*app, std::string{}, /*start=*/true);
        Access::publish(*app);
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        const double t0 = nowMs();
        Access::closeRadios(*app);  // what the patch's stop does
        app.reset();
        destroyMs = nowMs() - t0;
    }
    CHECK(destroyMs < 1500.0);
    std::printf("  exit with a speaker's open wedged: closing the patch and the window took %.0f ms\n",
                destroyMs);
    const double until = nowMs() + 8000.0;
    while (nowMs() < until && g_closes.load() == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    CHECK(g_closes.load() == 1);
}

// --- 4. THE WIRING ------------------------------------------------------------

std::string readFileText(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    std::string t = ss.str();
    t.erase(std::remove(t.begin(), t.end(), '\r'), t.end());
    return t;
}

std::string withoutLineComments(const std::string& text) {
    std::string out;
    std::size_t i = 0;
    while (i < text.size()) {
        const std::size_t eol = text.find('\n', i);
        const std::size_t end = eol == std::string::npos ? text.size() : eol;
        std::string line = text.substr(i, end - i);
        const std::size_t slashes = line.find("//");
        if (slashes != std::string::npos) { line.erase(slashes); }
        out += line;
        out += '\n';
        i = end + 1;
    }
    return out;
}

// The text from `signature` to the closing brace in column 0.
std::string functionBody(const std::string& text, const std::string& signature) {
    const std::size_t at = text.find(signature);
    if (at == std::string::npos) { return std::string(); }
    const std::size_t end = text.find("\n}\n", at);
    return text.substr(at, end == std::string::npos ? std::string::npos : end - at);
}

// The text of the class `signature` opens, to the closing "};" in column 0.
std::string classBody(const std::string& text, const std::string& signature) {
    const std::size_t at = text.find(signature);
    if (at == std::string::npos) { return std::string(); }
    const std::size_t end = text.find("\n};\n", at);
    return text.substr(at, end == std::string::npos ? std::string::npos : end - at);
}

// What would put a file-system call back on the calling thread.
const std::vector<std::string>& blockingCalls() {
    static const std::vector<std::string> calls = {
        "std::filesystem::", "create_directories", "is_directory", "rec_.start(", "Recorder::start",
        "openFile(", "std::ofstream", "fopen(", "makeWavDest(",
    };
    return calls;
}

std::vector<std::string> blockingCallsIn(const std::string& body) {
    std::vector<std::string> found;
    const std::string code = withoutLineComments(body);
    for (const std::string& call : blockingCalls()) {
        if (code.find(call) != std::string::npos) { found.push_back(call); }
    }
    return found;
}

void checkNothingOpensASpeakerFileOnTheGuiThread() {
    // THE CONTROL: the scan sees the call it exists to forbid, on the code as it
    // was before the fix.
    const std::string before =
        "bool start(const std::string& dir, const std::string& prefix, std::string& error) {\n"
        "    return rec_.start(RecordKind::Audio, dir, kOutRateHz, error, prefix);\n"
        "}\n"
        "};\n";
    CHECK(!blockingCallsIn(classBody(before, "bool start(")).empty());
    CHECK(blockingCallsIn("void f() {\n    // rec_.start(x) was here\n    int x = 1;\n}\n").empty());

    const fs::path root(CASCADE_SOURCE_DIR);
    const std::string audio = readFileText(root / "src" / "core" / "patch_audio.cpp");
    const std::string radios = readFileText(root / "src" / "gui" / "app_window_patch_radios.cpp");
    CHECK(!audio.empty() && !radios.empty());

    // The page's reconcile reaches the disk through no call of its own...
    const std::string publish = functionBody(radios, "void AppWindow::patchPublishSets(");
    CHECK(publish.size() > 500);
    const std::vector<std::string> hits = blockingCallsIn(publish);
    for (const std::string& h : hits) { std::printf("  patchPublishSets asks the disk: %s\n", h.c_str()); }
    CHECK(hits.empty());
    // ...and makes a WAV only through the call that never waits.
    CHECK(publish.find("makeWavDestAsync(") != std::string::npos);

    // The destinations' own constructors and factories: WavDest::start hands the
    // open to a worker, makeMp3Dest makes no folder.
    const std::string wavClass = classBody(audio, "class WavDest");
    const std::size_t startAt = wavClass.find("bool start(");
    const std::size_t waitAt = wavClass.find("bool waitOpened(");
    CHECK(startAt != std::string::npos && waitAt != std::string::npos && startAt < waitAt);
    const std::string wavStart = startAt != std::string::npos && waitAt != std::string::npos
                                     ? wavClass.substr(startAt, waitAt - startAt)
                                     : std::string();
    CHECK(wavStart.size() > 200);
    const std::vector<std::string> wavHits = blockingCallsIn(wavStart);
    for (const std::string& h : wavHits) { std::printf("  WavDest::start asks the disk: %s\n", h.c_str()); }
    CHECK(wavHits.empty());
    CHECK(wavStart.find("std::thread(") != std::string::npos);
    const std::string mp3 = functionBody(audio, "std::shared_ptr<AudioDest> makeMp3Dest(");
    CHECK(mp3.size() > 100);
    const std::vector<std::string> mp3Hits = blockingCallsIn(mp3);
    for (const std::string& h : mp3Hits) { std::printf("  makeMp3Dest asks the disk: %s\n", h.c_str()); }
    CHECK(mp3Hits.empty());
}

}  // namespace

int main() {
    isolate();
    checkNothingOpensASpeakerFileOnTheGuiThread();
    checkTheSynchronousOpenIsReported();
    checkALateWavIsTheSameFileAsAnInlineOne();
    checkALateFailureSaysWhatAnInlineStartWould();
    checkADiskSlowerThanThePreRollDropsAndSaysSo();
    checkADestroyedWavDoesNotWaitForItsOpen();
    checkAppWindowWavNeverHoldsAFrame();
    checkAppWindowMp3NeverHoldsAFrame();
    checkExitWhileASpeakerFileIsOpening();
    std::error_code ec;
    if (g_checksFailed == 0) { fs::remove_all(g_scratch, ec); }
    return testSummary("test_patch_dest_async");
}
