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
#include <cstdlib>
#include <cstring>
#include <ctime>
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

#include <imgui.h>

#include "core/diag_log.hpp"
#include "core/mp3_writer.hpp"
#include "core/patch_audio.hpp"
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
bool capturingOpener(const Recorder::OpenRequest& req, Recorder::OpenedFile& out, std::string& err) {
    const bool ok = Recorder::openFile(req, out, err);
    if (ok) { g_file.store(out.file.get()); }
    return ok;
}

// The OS handle behind the stream closed: every write, seek and close fails from here.
void pullTheFile() {
#if defined(_WIN32)
    std::FILE* f = g_file.load();
    if (f != nullptr) { ::CloseHandle(reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(f)))); }
#endif
}

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

// A folder cannot be removed, or renamed, from under an open take: Windows refuses.
void checkTheFolderCannotLeaveUnderAnOpenTake() {
    AppWindow app;
    Access::bindOpener(app, &capturingOpener);
    const fs::path dir = g_scratch / "folder-under-take";
    Access::setRecordDir(app, dir.string());
    CHECK(Access::startAudio(app));
    CHECK(pumpApp(app, 5000.0, [&] { return Access::audioRec(app).recording(); }));
    std::error_code ec;
    const std::uintmax_t removed = fs::remove_all(dir, ec);
    const bool gone = !fs::exists(dir, ec);
    std::printf("  folder removed under an open take: %s (removed %llu entries)\n",
                gone ? "IT WENT" : "refused by Windows, as expected",
                static_cast<unsigned long long>(removed));
    CHECK(!gone);
    const std::vector<float> s(4800, 0.25f);
    Access::audioRec(app).writeAudio(s.data(), s.size());
    CHECK(!Access::audioRec(app).writeFailed());
    Access::stopAudio(app);
    CHECK(pumpApp(app, 8000.0, [&] { return !Access::finishing(app, false); }));
    const std::vector<fs::path> files = filesIn(dir);
    CHECK(files.size() == 1u);
    if (files.size() == 1u) { CHECK(inspect(files[0]).headerData == 9600u); }
}

// A take is named for the SECOND it was asked for (audio_YYYYMMDD_HHMMSS_48000Hz.wav), and
// the open truncates an existing file of that name. Stop followed by Record inside one
// second therefore opens the same name over the take that was just finished.
void checkStopThenRecordInTheSameSecondOverwritesTheTake() {
    for (int attempt = 0; attempt < 4; ++attempt) {
        AppWindow app;
        const fs::path dir = g_scratch / ("same-second-" + std::to_string(attempt));
        Access::setRecordDir(app, dir.string());
        // Begin just after the wall clock ticks over, so the whole sequence sits in one second.
        const std::time_t tick = std::time(nullptr);
        while (std::time(nullptr) == tick) { std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
        const std::time_t second = std::time(nullptr);
        CHECK(Access::startAudio(app));
        CHECK(pumpApp(app, 5000.0, [&] { return Access::audioRec(app).recording(); }));
        const std::vector<float> first(24000, 0.25f);
        Access::audioRec(app).writeAudio(first.data(), first.size());  // 48000 bytes
        Access::stopAudio(app);
        CHECK(Access::startAudio(app));  // at once: remembered, started when the first file is closed
        CHECK(pumpApp(app, 5000.0, [&] { return Access::audioRec(app).recording(); }));
        const std::time_t secondAtStart = std::time(nullptr);
        const std::vector<float> next(4800, 0.25f);
        Access::audioRec(app).writeAudio(next.data(), next.size());  // 9600 bytes
        Access::stopAudio(app);
        CHECK(pumpApp(app, 8000.0, [&] { return !Access::finishing(app, false); }));
        CHECK(RecordFinisher::drain(std::chrono::steady_clock::now() + std::chrono::seconds(10)));
        if (secondAtStart != second) { continue; }  // the second ticked over: no collision to see
        const std::vector<fs::path> files = filesIn(dir);
        // FINDING, PINNED: one file, holding only the SECOND take. The first take's 48000 bytes
        // were closed, complete and valid - and then opened over.
        CHECK(files.size() == 1u);
        if (files.size() == 1u) {
            const Husk h = inspect(files[0]);
            CHECK(h.headerData == 9600u);
            std::printf("  FINDING Stop then Record inside one second: %zu file, %u data bytes - the first "
                        "take (48000 bytes, closed and valid) was opened over by the second\n",
                        files.size(), h.headerData);
        }
        return;
    }
    std::printf("  Stop then Record inside one second: NOT REACHED (the second ticked over in every attempt)\n");
}

// --- 2. A PATCH SPEAKER'S WAV ---------------------------------------------------------

void checkAPatchSpeakerWavWhenTheDiskGoes() {
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

int main() {
    isolate();
    ImGui::CreateContext();
    std::printf("test_failure_recording\n");
    checkTheTakesWhenTheDiskGoes();
    checkTheFolderCannotLeaveUnderAnOpenTake();
    checkStopThenRecordInTheSameSecondOverwritesTheTake();
    checkAPatchSpeakerWavWhenTheDiskGoes();
    checkAPatchSpeakerMp3WhenTheDiskGoes();
    ImGui::DestroyContext();
    std::error_code ec;
    if (g_checksFailed == 0) { fs::remove_all(g_scratch, ec); }
    return testSummary("test_failure_recording");
}
