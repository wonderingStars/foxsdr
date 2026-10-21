// What FoxSDR does when the disk fails UNDER A RECORDING (0.99.65).
//
// The companion of tests/test_failure_files.cpp for the files that are written for
// as long as a take lasts: the receiver's I/Q and audio takes, and a patch speaker's
// WAV and MP3. Each case is asserted whole - no crash, no hang (every call is timed),
// the failure SAID where it is said, the counters honest, the file left as it should
// be, and the finish (core/record_finish.hpp) neither blocked nor lost.
//
// HOW A DISK FAILS HERE. There is no unprivileged way to fill a volume or pull a
// drive under a test, so a take's file has its OS handle closed behind the stream
// (CloseHandle on the handle the CRT holds): from then on every write, seek and the
// close itself fail, which is what a full disk, a removed device and a share that went
// away all look like to the code above stdio. The recorders' file opener is a seam
// that exists already (Recorder::bindOpener, DestSeams::wavOpener): the test's opener
// opens the file through the production opener and keeps the stream. The MP3 is the
// one writer whose file Media Foundation owns, so it has a seam of its own
// (DestSeams::mp3WriteFails).
//
// A TAKE NEVER OPENS OVER A FILE THAT IS THERE (0.99.65, core/unique_file.hpp): Stop then
// Record inside one second leaves two whole files ("name.wav", "name-2.wav") each shown by
// the name it has on disk; two writers of this process asking in the same second (two
// recorders, two speakers' WAVs, two MP3s) get different names; a file another program left
// under the name is not overwritten; 99 names taken fail the take in the old words.
//
// WHAT COULD NOT BE REACHED. A folder removed or renamed under an open take: Windows
// refuses it while the file is open (asserted below, so it stays true). A volume that
// really fills up. A source that goes away mid-take is covered by an existing
// end-to-end test (tests/test_stop_ends_recordings.cpp, "a fault mid-take", which runs
// the real application against an I/Q file that is cut under it and checks both takes
// finish valid); and a start refused by the disk by tests/test_record_start.cpp and
// tests/test_health_paths.cpp (rec_fail counted once).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <set>
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

#include <imgui.h>

#include "failing_disk.hpp"
#include "core/diag_log.hpp"
#include "core/mp3_writer.hpp"
#include "core/patch_audio.hpp"
#include "core/patch_recordings.hpp"
#include "core/record_finish.hpp"
#include "core/recorder.hpp"
#include "gui/app_window.hpp"
#include "test_check.hpp"

namespace fs = std::filesystem;
using cascade::core::RecordFinisher;
using cascade::core::Recorder;

namespace cascade::gui {

struct AppWindowTestAccess {
    static void bindOpener(AppWindow& a, Recorder::Opener o) {
        a.iqRecorder_.bindOpener(o);
        a.audioRecorder_.bindOpener(std::move(o));
    }
    static void setRecordDir(AppWindow& a, std::string d) { a.recordDir_ = std::move(d); }
    static bool startAudio(AppWindow& a) { return a.startAudioRecording(); }
    static bool startIq(AppWindow& a) { return a.startIqRecording(); }
    static void stopAudio(AppWindow& a) { a.stopAudioRecording(); }
    static void stopIq(AppWindow& a) { a.stopIqRecording(); }
    static void poll(AppWindow& a, double nowS) { a.pollRecordStarts(nowS); }
    static bool finishing(AppWindow& a, bool iq) { return a.takeFinishing(iq); }
    static Recorder& audioRec(AppWindow& a) { return a.audioRecorder_; }
    static Recorder& iqRec(AppWindow& a) { return a.iqRecorder_; }
    static const std::string& recordError(AppWindow& a) { return a.recordError_; }
    static const std::string& recordNotice(AppWindow& a) { return a.recordNotice_; }
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
    g_scratch = fs::temp_directory_path() / ("foxsdr_failure_recording_" + std::to_string(pid));
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

std::vector<unsigned char> readAll(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<unsigned char>((std::istreambuf_iterator<char>(in)),
                                      std::istreambuf_iterator<char>());
}

std::vector<fs::path> filesIn(const fs::path& dir) {
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

int logCount(const char* needle) {
    int n = 0;
    for (const std::string& l : cascade::core::DiagLog::instance().ringSnapshot()) {
        if (l.find(needle) != std::string::npos) { ++n; }
    }
    return n;
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

// --- the disk that fails -----------------------------------------------------------

std::atomic<std::FILE*> g_file{nullptr};

// The production opener, keeping the stream so the test can pull the file from under it.
//
// HOW A DISK IS FAILED depends on the platform (tests/failing_disk.hpp): on WINDOWS the OS handle behind
// the stream is closed once the take has run healthily for a while (pullTheFile), on LINUX the take's
// stream is /dev/full from the start (a real ENOSPC; pullTheFile does nothing). Closing a descriptor
// behind a stream is never done on POSIX, where the number is reused by the next open in the process.
bool capturingOpener(const Recorder::OpenRequest& req, Recorder::OpenedFile& out, std::string& err) {
    return failing_disk::open(req, out, err, &g_file);
}

void pullTheFile() { failing_disk::pull(g_file.load()); }

struct Husk {
    bool parses = false;
    std::uint32_t headerData = 0;
    std::size_t fileBytes = 0;
};
Husk inspect(const fs::path& p) {
    Husk h;
    const std::vector<unsigned char> b = readAll(p);
    h.fileBytes = b.size();
    if (b.size() >= 44 && std::memcmp(b.data(), "RIFF", 4) == 0 && std::memcmp(b.data() + 8, "WAVE", 4) == 0) {
        h.parses = true;
        h.headerData = le32(b, 40);
    }
    return h;
}

// --- 1. THE RECEIVER'S TAKES: a write that fails part way ---------------------------

template <class Write>
void stalledTake(const char* what, bool iq, const char* tag, Write&& write, std::size_t perBlock) {
    if (!failing_disk::available()) {
        std::printf("  %s, disk goes mid-take: NOT REACHED (no way to fail a take's disk on this platform)\n",
                    what);
        return;
    }
    AppWindow app;
    Access::bindOpener(app, &capturingOpener);
    const fs::path dir = g_scratch / tag;
    Access::setRecordDir(app, dir.string());
    g_file.store(nullptr);
    Recorder& rec = iq ? Access::iqRec(app) : Access::audioRec(app);
    CHECK(iq ? Access::startIq(app) : Access::startAudio(app));
    CHECK(pumpApp(app, 5000.0, [&] { return rec.recording(); }));
    CHECK(g_file.load() != nullptr);

    // A healthy stretch: what the take has accepted so far sits in the stream's buffer.
    for (int i = 0; i < 4; ++i) { write(rec, perBlock); }
    const std::uint64_t before = rec.bytesWritten();
    CHECK(before > 0u);
    CHECK(!rec.writeFailed());

    // THE DISK GOES. Keep writing, as the DSP thread does, until the stream's buffer
    // fills and the refusal reaches the recorder: no call may hang, and the take must
    // LATCH the failure (the Recorder card's "disk refused the file").
    pullTheFile();
    double worstCallMs = 0.0;
    int blocks = 0;
    while (!rec.writeFailed() && blocks < 4000) {
        const double t0 = nowMs();
        write(rec, perBlock);
        worstCallMs = std::max(worstCallMs, nowMs() - t0);
        ++blocks;
    }
    CHECK(rec.writeFailed());
    CHECK(worstCallMs < 500.0);
    // STILL "recording": the status card's STALLED state is writeFailed() on a take that is
    // still recording(), and it is left for the user to stop.
    CHECK(rec.recording());
    const std::uint64_t frozen = rec.bytesWritten();
    for (int i = 0; i < 10; ++i) { write(rec, perBlock); }
    CHECK(rec.bytesWritten() == frozen);  // nothing more is "kept", and nothing says it is

    // THE USER STOPS IT. The finish cannot patch the header or close cleanly: it ends, once,
    // and says so in the log - and nothing on screen, as the synchronous stop said nothing.
    const int logged = logCount("could not be finalised");
    if (iq) {
        Access::stopIq(app);
    } else {
        Access::stopAudio(app);
    }
    CHECK(pumpApp(app, 8000.0, [&] { return !Access::finishing(app, iq); }));
    CHECK(logCount("could not be finalised") == logged + 1);
    CHECK(Access::recordError(app).empty());
    CHECK(Access::recordNotice(app).empty());
    CHECK(!rec.recording());

    const std::vector<fs::path> files = filesIn(dir);
    CHECK(files.size() == 1u);
    if (files.size() == 1u) {
        const Husk h = inspect(files[0]);
        CHECK(h.parses);
        // THE HUSK: the header the opener flushed, sizes zero. What the take accepted before
        // the failure is in the stream's buffer, and goes with the handle.
        CHECK(h.headerData == 0u);
        std::printf("  %s, disk goes mid-take: %d blocks after the failure until it was seen, worst call "
                    "%.1f ms, bytes accepted %llu then frozen; Stop: file %zu bytes, header says %u "
                    "data bytes (a husk), one log line, nothing on screen\n",
                    what, blocks, worstCallMs, static_cast<unsigned long long>(frozen), h.fileBytes,
                    h.headerData);
    }
}

void checkTheTakesWhenTheDiskGoes() {
    stalledTake(
        "audio take", false, "stall-audio",
        [](Recorder& r, std::size_t n) {
            const std::vector<float> s(n, 0.25f);
            r.writeAudio(s.data(), s.size());
        },
        4800);
    stalledTake(
        "I/Q take", true, "stall-iq",
        [](Recorder& r, std::size_t n) {
            const std::vector<std::complex<float>> s(n, std::complex<float>(0.25f, -0.25f));
            r.writeIq(s.data(), s.size());
        },
        4096);
}

// A FOLDER REMOVED FROM UNDER AN OPEN TAKE: each platform's own truth.
//   WINDOWS refuses to remove it while the file is open, so the take is never at risk.
//   POSIX removes it: the file is unlinked and goes on receiving writes, nothing fails, and Stop
//   closes a file that no longer has a name - there is no file afterwards, and nothing said it was
//   lost. Pinned as a FINDING of the same kind as the MP3 speaker's (a destination that disappears
//   under a take that carries on writing, silently).
void checkTheFolderUnderAnOpenTake() {
    AppWindow app;  // the production opener: this case is about the folder, not a failing disk
    const fs::path dir = g_scratch / "folder-under-take";
    Access::setRecordDir(app, dir.string());
    CHECK(Access::startAudio(app));
    CHECK(pumpApp(app, 5000.0, [&] { return Access::audioRec(app).recording(); }));
    std::error_code ec;
    const std::uintmax_t removed = fs::remove_all(dir, ec);
    const bool gone = !fs::exists(dir, ec);
    const int logged = logCount("could not be finalised");
    const std::vector<float> s(4800, 0.25f);
#if defined(_WIN32)
    std::printf("  folder removed under an open take: %s (removed %llu entries)\n",
                gone ? "IT WENT" : "refused by Windows, as expected",
                static_cast<unsigned long long>(removed));
    CHECK(!gone);
    Access::audioRec(app).writeAudio(s.data(), s.size());
    CHECK(!Access::audioRec(app).writeFailed());
    Access::stopAudio(app);
    CHECK(pumpApp(app, 8000.0, [&] { return !Access::finishing(app, false); }));
    const std::vector<fs::path> files = filesIn(dir);
    CHECK(files.size() == 1u);
    if (files.size() == 1u) { CHECK(inspect(files[0]).headerData == 9600u); }
#else
    (void)removed;
    CHECK(gone);  // POSIX lets an open file's folder go
    Access::audioRec(app).writeAudio(s.data(), s.size());
    CHECK(!Access::audioRec(app).writeFailed());  // the take runs on: writes to the unlinked file succeed
    CHECK(Access::audioRec(app).recording());
    Access::stopAudio(app);
    CHECK(pumpApp(app, 8000.0, [&] { return !Access::finishing(app, false); }));
    CHECK(logCount("could not be finalised") == logged);  // the finish of the unlinked file succeeds...
    CHECK(Access::recordError(app).empty());              // ...and nothing is said
    CHECK(filesIn(dir).empty());                           // ...and there is no file afterwards
    std::printf("  FINDING folder removed under an open take (POSIX): the folder went, the take ran on "
                "(%llu bytes accepted), Stop said nothing, and there is no file\n",
                static_cast<unsigned long long>(Access::audioRec(app).bytesWritten()));
#endif
}

// A TAKE NEVER OPENS OVER AN EXISTING FILE (0.99.65). A take is named for the SECOND it was
// asked for (audio_YYYYMMDD_HHMMSS_48000Hz.wav); when that name is taken - by the take just
// finished, one still being finished, one another writer of this process just made, or a file
// another program left there - the worker's exclusive create steps to "name-2.wav", "name-3.wav"
// and so on, so no check can be separated from the open. Before this, the open was `fopen("wb")`,
// and Stop followed by Record inside one second truncated the take that was just finished.

// Waits for the wall clock's next whole second, so what follows sits inside one second.
std::time_t alignToTheSecond() {
    const std::time_t tick = std::time(nullptr);
    while (std::time(nullptr) == tick) { std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
    return std::time(nullptr);
}

std::string stampOf(std::time_t t) {
    std::tm tmv{};
#if defined(_WIN32)
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    char buf[32];
    std::snprintf(buf, sizeof buf, "_%04d%02d%02d_%02d%02d%02d", tmv.tm_year + 1900, tmv.tm_mon + 1,
                  tmv.tm_mday, tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    return buf;
}

std::vector<std::string> leafNames(const std::vector<fs::path>& files) {
    std::vector<std::string> out;
    for (const fs::path& p : files) { out.push_back(p.filename().string()); }
    std::sort(out.begin(), out.end());
    return out;
}

std::string leafOf(const std::string& path) { return fs::path(path).filename().string(); }

void checkStopThenRecordInTheSameSecondKeepsBothTakes() {
    for (int attempt = 0; attempt < 4; ++attempt) {
        AppWindow app;
        const fs::path dir = g_scratch / ("same-second-" + std::to_string(attempt));
        Access::setRecordDir(app, dir.string());
        const std::time_t second = alignToTheSecond();
        CHECK(Access::startAudio(app));
        CHECK(pumpApp(app, 5000.0, [&] { return Access::audioRec(app).recording(); }));
        const std::string firstShown = Access::audioRec(app).path();
        const std::vector<float> first(24000, 0.25f);
        Access::audioRec(app).writeAudio(first.data(), first.size());  // 48000 bytes
        Access::stopAudio(app);
        CHECK(Access::startAudio(app));  // at once: remembered, started when the first file is closed
        CHECK(pumpApp(app, 5000.0, [&] { return Access::audioRec(app).recording(); }));
        const std::time_t secondAtStart = std::time(nullptr);
        const std::string secondShown = Access::audioRec(app).path();
        const std::vector<float> next(4800, 0.25f);
        Access::audioRec(app).writeAudio(next.data(), next.size());  // 9600 bytes
        Access::stopAudio(app);
        CHECK(pumpApp(app, 8000.0, [&] { return !Access::finishing(app, false); }));
        CHECK(RecordFinisher::drain(std::chrono::steady_clock::now() + std::chrono::seconds(10)));
        if (secondAtStart != second) { continue; }  // the second ticked over: no collision to see
        const std::vector<fs::path> files = filesIn(dir);
        // TWO WHOLE FILES, each with the right header: the first take is still 48000 bytes.
        CHECK(files.size() == 2u);
        // THE NAME SHOWN IS THE NAME ON DISK, for both takes, and the two differ.
        CHECK(firstShown != secondShown);
        const std::string wantFirst = "audio" + stampOf(second) + "_48000Hz.wav";
        const std::string wantSecond = "audio" + stampOf(second) + "_48000Hz-2.wav";
        CHECK(leafOf(firstShown) == wantFirst);
        CHECK(leafOf(secondShown) == wantSecond);
        for (const fs::path& f : files) {
            const Husk h = inspect(f);
            CHECK(h.parses);
            if (f.filename().string() == leafOf(firstShown)) { CHECK(h.headerData == 48000u); }
            if (f.filename().string() == leafOf(secondShown)) { CHECK(h.headerData == 9600u); }
        }
        std::printf("  Stop then Record inside one second: %zu files (%s, %s), 48000 and 9600 data bytes, "
                    "each shown by the name it has on disk\n",
                    files.size(), leafOf(firstShown).c_str(), leafOf(secondShown).c_str());
        return;
    }
    std::printf("  Stop then Record inside one second: NOT REACHED (the second ticked over in every attempt)\n");
}

// Two I/Q takes inside one second, and the patch page's recordings list: both are listed, by the
// names they have on disk.
void checkTwoIqTakesInOneSecondAreBothListed() {
    for (int attempt = 0; attempt < 4; ++attempt) {
        AppWindow app;
        const fs::path dir = g_scratch / ("same-second-iq-" + std::to_string(attempt));
        Access::setRecordDir(app, dir.string());
        const std::time_t second = alignToTheSecond();
        CHECK(Access::startIq(app));
        CHECK(pumpApp(app, 5000.0, [&] { return Access::iqRec(app).recording(); }));
        const std::string firstShown = Access::iqRec(app).path();
        const std::vector<std::complex<float>> s(2000, std::complex<float>(0.25f, -0.25f));
        Access::iqRec(app).writeIq(s.data(), s.size());
        Access::stopIq(app);
        CHECK(Access::startIq(app));
        CHECK(pumpApp(app, 5000.0, [&] { return Access::iqRec(app).recording(); }));
        const std::time_t secondAtStart = std::time(nullptr);
        const std::string secondShown = Access::iqRec(app).path();
        Access::iqRec(app).writeIq(s.data(), s.size());
        Access::stopIq(app);
        CHECK(pumpApp(app, 8000.0, [&] { return !Access::finishing(app, true); }));
        CHECK(RecordFinisher::drain(std::chrono::steady_clock::now() + std::chrono::seconds(10)));
        if (secondAtStart != second) { continue; }
        const std::vector<cascade::core::patch::RecordingInfo> listed =
            cascade::core::patch::listIqRecordings({dir.string()});
        CHECK(listed.size() == 2u);
        std::vector<std::string> listedNames;
        for (const auto& r : listed) { listedNames.push_back(leafOf(r.path)); }
        std::sort(listedNames.begin(), listedNames.end());
        CHECK(listedNames == leafNames(filesIn(dir)));
        std::vector<std::string> shown = {leafOf(firstShown), leafOf(secondShown)};
        std::sort(shown.begin(), shown.end());
        CHECK(listedNames == shown);
        std::printf("  two I/Q takes inside one second: the recordings list shows %zu files, the names on "
                    "disk\n",
                    listed.size());
        return;
    }
    std::printf("  two I/Q takes inside one second: NOT REACHED (the second ticked over in every attempt)\n");
}

// TWO WRITERS OF THIS PROCESS asking in the same second (the main recorder's two takes, two patch
// speakers named alike) get different names, whichever worker opens first.
void checkTwoWritersInOneSecondGetDifferentNames() {
    for (int attempt = 0; attempt < 4; ++attempt) {
        const fs::path dir = g_scratch / ("two-writers-" + std::to_string(attempt));
        const std::time_t second = alignToTheSecond();
        std::string err;
        // Two recorders, the same kind, rate and folder.
        Recorder a;
        Recorder b;
        CHECK(a.start(cascade::core::RecordKind::Audio, dir.string(), 48000.0, err));
        CHECK(b.start(cascade::core::RecordKind::Audio, dir.string(), 48000.0, err));
        // Two speakers' WAVs with the same prefix, and (where the encoder exists) two MP3s.
        std::shared_ptr<cascade::core::patch::AudioDest> w1 =
            cascade::core::patch::makeWavDest(dir.string(), "patch-1-Speaker", err);
        std::shared_ptr<cascade::core::patch::AudioDest> w2 =
            cascade::core::patch::makeWavDest(dir.string(), "patch-1-Speaker", err);
        const bool mp3 = cascade::core::Mp3Writer::available();
        std::shared_ptr<cascade::core::patch::AudioDest> m1;
        std::shared_ptr<cascade::core::patch::AudioDest> m2;
        if (mp3) {
            m1 = cascade::core::patch::makeMp3Dest(dir.string(), "patch-2-Speaker", err);
            m2 = cascade::core::patch::makeMp3Dest(dir.string(), "patch-2-Speaker", err);
        }
        const std::time_t after = std::time(nullptr);
        CHECK(w1 != nullptr && w2 != nullptr);
        if (!w1 || !w2) { return; }
        const std::vector<float> block(4800, 0.2f);
        for (int i = 0; i < 20; ++i) {
            w1->write(block.data(), block.size());
            w2->write(block.data(), block.size());
            if (mp3) {
                m1->write(block.data(), block.size());
                m2->write(block.data(), block.size());
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        // What each writer SAYS it is writing, before anything is closed.
        const std::string shownA = a.path();
        const std::string shownB = b.path();
        const std::string shownW1 = w1->describe();
        const std::string shownW2 = w2->describe();
        const std::string shownM1 = mp3 ? m1->describe() : std::string();
        const std::string shownM2 = mp3 ? m2->describe() : std::string();
        a.stop();
        b.stop();
        w1.reset();
        w2.reset();
        m1.reset();
        m2.reset();
        CHECK(RecordFinisher::drain(std::chrono::steady_clock::now() + std::chrono::seconds(10)));
        if (after != second) { continue; }
        const std::vector<std::string> names = leafNames(filesIn(dir));
        const std::size_t want = mp3 ? 6u : 4u;
        CHECK(names.size() == want);
        CHECK(std::set<std::string>(names.begin(), names.end()).size() == names.size());
        CHECK(shownA != shownB);
        CHECK(std::find(names.begin(), names.end(), leafOf(shownA)) != names.end());
        CHECK(std::find(names.begin(), names.end(), leafOf(shownB)) != names.end());
        // describe() is "WAV  <name>" / "MP3  <name>": the name on disk, for each of the pair.
        const auto onDisk = [&](const std::string& described) {
            return std::find(names.begin(), names.end(), described.substr(5)) != names.end();
        };
        CHECK(shownW1 != shownW2);
        CHECK(onDisk(shownW1));
        CHECK(onDisk(shownW2));
        if (mp3) {
            CHECK(shownM1 != shownM2);
            CHECK(onDisk(shownM1));
            CHECK(onDisk(shownM2));
        }
        for (const fs::path& f : filesIn(dir)) {
            if (f.extension() == ".wav") { CHECK(inspect(f).parses); }
            if (f.extension() == ".mp3") { CHECK(fs::file_size(f) > 0u); }
        }
        std::printf("  %s writers started inside one second: %zu different names, each shown as it is on "
                    "disk\n",
                    mp3 ? "six" : "four", names.size());
        return;
    }
    std::printf("  two writers inside one second: NOT REACHED (the second ticked over in every attempt)\n");
}

// A FILE ANOTHER PROGRAM LEFT WITH THE NAME A TAKE WOULD GET is never opened over.
void checkAForeignFileIsNeverOverwritten() {
    const std::string kForeign = "FOREIGN-FILE-CONTENT";
    const auto write = [](const fs::path& p, const std::string& text) {
        std::error_code ec;
        fs::create_directories(p.parent_path(), ec);
        std::ofstream f(p, std::ios::binary | std::ios::trunc);
        f << text;
    };
    const auto read = [](const fs::path& p) {
        std::ifstream f(p, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    };
    for (int attempt = 0; attempt < 4; ++attempt) {
        const fs::path dir = g_scratch / ("foreign-" + std::to_string(attempt));
        const std::time_t second = alignToTheSecond();
        const std::string stamp = stampOf(second);
        const fs::path wavName = dir / ("audio" + stamp + "_48000Hz.wav");
        const fs::path speakerName = dir / ("patch-3-Speaker" + stamp + ".wav");
        const fs::path mp3Name = dir / ("patch-4-Speaker" + stamp + ".mp3");
        write(wavName, kForeign);
        write(speakerName, kForeign);
        write(mp3Name, kForeign);
        std::string err;
        Recorder rec;
        CHECK(rec.start(cascade::core::RecordKind::Audio, dir.string(), 48000.0, err));
        std::shared_ptr<cascade::core::patch::AudioDest> w =
            cascade::core::patch::makeWavDest(dir.string(), "patch-3-Speaker", err);
        std::shared_ptr<cascade::core::patch::AudioDest> m;
        const bool mp3 = cascade::core::Mp3Writer::available();
        if (mp3) { m = cascade::core::patch::makeMp3Dest(dir.string(), "patch-4-Speaker", err); }
        const std::time_t after = std::time(nullptr);
        CHECK(w != nullptr);
        const std::vector<float> block(4800, 0.2f);
        for (int i = 0; i < 10 && w; ++i) {
            w->write(block.data(), block.size());
            if (m) { m->write(block.data(), block.size()); }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        const std::string shownRec = rec.path();
        const std::string shownW = w ? w->describe() : std::string();
        rec.stop();
        w.reset();
        m.reset();
        CHECK(RecordFinisher::drain(std::chrono::steady_clock::now() + std::chrono::seconds(10)));
        if (after != second) { continue; }
        CHECK(read(wavName) == kForeign);
        CHECK(read(speakerName) == kForeign);
        CHECK(read(mp3Name) == kForeign);
        CHECK(shownRec != wavName.string());
        CHECK(leafOf(shownRec) == leafOf(wavName.string()).substr(0, leafOf(wavName.string()).size() - 4) + "-2.wav");
        CHECK(shownW.size() > 5 && fs::exists(dir / shownW.substr(5)));
        CHECK(shownW.substr(5) != leafOf(speakerName.string()));
        std::printf("  a file another program left under a take's name: the recorder, a speaker's WAV%s each "
                    "went to the next free name; the foreign files are byte for byte what they were\n",
                    mp3 ? " and its MP3" : "");
        return;
    }
    std::printf("  a foreign file under a take's name: NOT REACHED (the second ticked over in every attempt)\n");
}

// THE SEARCH IS CAPPED at 99: past it the take fails in the words a refused create always had, and
// nothing is overwritten.
void checkTheSuffixSearchIsCapped() {
    for (int attempt = 0; attempt < 4; ++attempt) {
        const fs::path dir = g_scratch / ("capped-" + std::to_string(attempt));
        std::error_code ec;
        fs::create_directories(dir, ec);
        const std::time_t second = alignToTheSecond();
        const std::string base = "audio" + stampOf(second) + "_48000Hz";
        const auto fill = [&](int n) {
            const fs::path p = dir / (n == 1 ? base + ".wav" : base + "-" + std::to_string(n) + ".wav");
            std::ofstream f(p, std::ios::binary | std::ios::trunc);
            f << "TAKEN-" << n;
        };
        for (int n = 1; n <= 99; ++n) { fill(n); }
        std::string err;
        Recorder rec;
        const bool started = rec.start(cascade::core::RecordKind::Audio, dir.string(), 48000.0, err);
        const std::time_t after = std::time(nullptr);
        if (after != second) { continue; }
        CHECK(!started);
        CHECK(err.rfind("recorder: cannot create", 0) == 0);
        CHECK(!rec.recording());
        CHECK(filesIn(dir).size() == 99u);
        for (const int n : {1, 2, 50, 99}) {
            const fs::path p = dir / (n == 1 ? base + ".wav" : base + "-" + std::to_string(n) + ".wav");
            std::ifstream f(p, std::ios::binary);
            std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            CHECK(text == "TAKEN-" + std::to_string(n));
        }
        std::printf("  99 names taken: the 100th take fails (\"%s...\") and every file is untouched\n",
                    err.substr(0, 24).c_str());
        return;
    }
    std::printf("  capped search: NOT REACHED (the second ticked over in every attempt)\n");
}

// --- 2. A PATCH SPEAKER'S WAV ---------------------------------------------------------

void checkAPatchSpeakerWavWhenTheDiskGoes() {
    if (!failing_disk::available()) {
        std::printf("  patch speaker WAV, disk goes: NOT REACHED (no way to fail a take's disk here)\n");
        return;
    }
    const fs::path dir = g_scratch / "speaker-wav";
    cascade::core::patch::DestSeams seams;
    seams.wavOpener = &capturingOpener;
    g_file.store(nullptr);
    std::string err;
    auto dest = cascade::core::patch::makeWavDestAsync(dir.string(), "patch-1-fail", err, &seams);
    CHECK(dest != nullptr);
    if (!dest) { return; }
    const std::vector<float> block(480, 0.2f);  // 10 ms at 48 kHz
    // The reader thread's writes: the file opens on the worker, and the first write after it
    // is open arms the recorder.
    for (int i = 0; i < 400 && g_file.load() == nullptr; ++i) {
        dest->write(block.data(), block.size());
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(g_file.load() != nullptr);
    for (int i = 0; i < 20; ++i) { dest->write(block.data(), block.size()); }
    CHECK(dest->error().empty());

    pullTheFile();
    double worstCallMs = 0.0;
    int blocks = 0;
    while (dest->error().empty() && blocks < 4000) {
        const double t0 = nowMs();
        dest->write(block.data(), block.size());
        worstCallMs = std::max(worstCallMs, nowMs() - t0);
        ++blocks;
    }
    // REPORTED, in the words the speaker's face shows.
    CHECK(dest->error() == "the disk refused a write - is it full?");
    CHECK(worstCallMs < 500.0);

    // The destination goes (the radio stopped, the page closed): returns at once, the file is
    // finished by the worker, and what is left is the husk.
    const double t0 = nowMs();
    dest.reset();
    const double destroyMs = nowMs() - t0;
    CHECK(destroyMs < 500.0);
    CHECK(RecordFinisher::drain(std::chrono::steady_clock::now() + std::chrono::seconds(10)));
    CHECK(RecordFinisher::inFlight() == 0u);
    const std::vector<fs::path> files = filesIn(dir);
    CHECK(files.size() == 1u);
    if (files.size() == 1u) {
        const Husk h = inspect(files[0]);
        CHECK(h.parses);
        CHECK(h.headerData == 0u);
        std::printf("  patch speaker WAV, disk goes: error() says \"%s\" after %d blocks (worst write "
                    "%.1f ms); destroying it took %.1f ms; file %zu bytes, header data %u (a husk)\n",
                    "the disk refused a write - is it full?", blocks, worstCallMs, destroyMs,
                    h.fileBytes, h.headerData);
    }
}

// --- 3. A PATCH SPEAKER'S MP3 -----------------------------------------------------------

void checkAPatchSpeakerMp3WhenTheDiskGoes() {
    if (!cascade::core::Mp3Writer::available()) {
        std::printf("  patch speaker MP3: NOT REACHED (no Media Foundation MP3 encoder on this machine)\n");
        return;
    }
    const fs::path dir = g_scratch / "speaker-mp3";
    std::atomic<bool> fail{false};
    cascade::core::patch::DestSeams seams;
    seams.mp3WriteFails = [&fail] { return fail.load(); };
    std::string err;
    auto dest = cascade::core::patch::makeMp3Dest(dir.string(), "patch-2-fail", err, &seams);
    CHECK(dest != nullptr);
    if (!dest) { return; }
    std::vector<float> block(480);
    for (std::size_t i = 0; i < block.size(); ++i) {
        block[i] = 0.3f * std::sin(0.05f * static_cast<float>(i));
    }
    for (int i = 0; i < 100; ++i) {  // a second of sound, paced, so the worker has the file open
        dest->write(block.data(), block.size());
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(dest->error().empty());

    // THE FOLDER REMOVED UNDER THE ENCODER'S OPEN FILE: whatever Windows does, nothing may crash
    // or hang, and the sound still goes in.
    std::error_code ec;
    fs::remove_all(dir, ec);
    const bool folderGone = !fs::exists(dir, ec);
    for (int i = 0; i < 50; ++i) {
        dest->write(block.data(), block.size());
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    // FINDING, PINNED: the encoder holds no lock on its file, so a folder removed under it
    // goes, and the speaker says NOTHING - the take carries on to a file that no longer exists.
    const std::string errAfterRemoval = dest->error();
    if (folderGone) { CHECK(errAfterRemoval.empty()); }
    std::printf("  patch speaker MP3, folder removed under the open file: %s; error() afterwards: \"%s\"%s\n",
                folderGone ? "IT WENT (the encoder holds no lock)" : "refused by Windows",
                errAfterRemoval.c_str(), errAfterRemoval.empty() ? " (silent)" : "");

    // THE ENCODER'S WRITE REFUSED (a full disk).
    fail.store(true);
    double worstCallMs = 0.0;
    for (int i = 0; i < 100; ++i) {
        const double t0 = nowMs();
        dest->write(block.data(), block.size());
        worstCallMs = std::max(worstCallMs, nowMs() - t0);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(worstCallMs < 100.0);
    bool said = false;
    for (int i = 0; i < 200 && !said; ++i) {
        said = dest->error() == "the MP3 encoder refused a write";
        if (!said) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
    }
    CHECK(said);
    const double t0 = nowMs();
    dest.reset();  // the worker flushes and closes the file by itself: nobody waits for it here
    const double destroyMs = nowMs() - t0;
    CHECK(destroyMs < 500.0);
    CHECK(RecordFinisher::drain(std::chrono::steady_clock::now() + std::chrono::seconds(10)));
    const std::vector<fs::path> files = filesIn(dir);
    std::printf("  patch speaker MP3, encoder write refused: error() says \"the MP3 encoder refused a "
                "write\"; destroying it took %.0f ms; files left: %zu%s\n",
                destroyMs, files.size(), folderGone ? " (folder was removed)" : "");
}

}  // namespace

// A case that THROWS is a failed check with its message, never a terminate: an uncaught exception
// ended test_failure_files on Linux in 0.08 s and took every later case with it (0.99.65).
#define GUARDED(call)                                                              \
    do {                                                                           \
        try {                                                                      \
            call;                                                                  \
        } catch (const std::exception& e) {                                        \
            std::printf("  EXCEPTION in %s: %s\n", #call, e.what());                \
            CHECK(false);                                                          \
        } catch (...) {                                                            \
            std::printf("  EXCEPTION in %s: (not a std::exception)\n", #call);      \
            CHECK(false);                                                          \
        }                                                                          \
    } while (0)

int main() {
    isolate();
    ImGui::CreateContext();
    std::printf("test_failure_recording\n");
    GUARDED(checkTheTakesWhenTheDiskGoes());
    GUARDED(checkTheFolderUnderAnOpenTake());
    GUARDED(checkStopThenRecordInTheSameSecondKeepsBothTakes());
    GUARDED(checkTwoIqTakesInOneSecondAreBothListed());
    GUARDED(checkTwoWritersInOneSecondGetDifferentNames());
    GUARDED(checkAForeignFileIsNeverOverwritten());
    GUARDED(checkTheSuffixSearchIsCapped());
    GUARDED(checkAPatchSpeakerWavWhenTheDiskGoes());
    checkAPatchSpeakerMp3WhenTheDiskGoes();
    ImGui::DestroyContext();
    std::error_code ec;
    if (g_checksFailed == 0) { fs::remove_all(g_scratch, ec); }
    return testSummary("test_failure_recording");
}
