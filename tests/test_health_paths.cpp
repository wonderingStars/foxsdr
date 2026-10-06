// THE FAILURE COUNTS, DRIVEN THROUGH THE FAILURES THAT REALLY HAPPEN
// (core/health_events.hpp, 0.99.64).
//
// test_health_events.cpp holds the vocabulary, the ledger and the documents. This
// file holds each event to the place it is counted: it makes the real thing fail -
// the real sound output with a device that is not there, the real drivers opened
// against radios that are not plugged in, the SDRplay driver against the fake API,
// the update check and download and the plugin catalogue and installer against a
// transport the test stands in for (PluginRepo::setTransportForTest), the plugin
// host loading a file that is not a module and a module built for another ABI, a
// recording started in a folder that cannot exist, the Store check and install
// through the environment seams that already exist - and reads what the ledger
// counted. None of them calls the counter.
//
// WHAT THIS CANNOT SEE, stated so a green run is not read as more: a radio that is
// THERE and refuses to open is not driven - the one dongle on the machine this was
// written on is never opened (another program may be using it), so what is driven is
// each driver's own sentence for a serial that is not there, which every driver
// returns through the same lastError(). And no network is touched: the transport
// seam is the only way a download or a catalogue "arrives".
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/frame_timing.hpp"
#include "core/health_events.hpp"
#include "core/plugin_host.hpp"
#include "core/plugin_repo.hpp"
#include "core/recorder.hpp"
#include "core/store_update.hpp"
#include "core/telemetry.hpp"
#include "core/updater.hpp"
#include "gui/record_start.hpp"
#include "sdrplay_fake_api.hpp"
#include "sink/audio_out.hpp"
#include "source/aor_source.hpp"
#include "source/airspy_source.hpp"
#include "source/airspyhf_source.hpp"
#include "source/hackrf_source.hpp"
#include "source/hydrasdr_source.hpp"
#include "source/mirisdr_source.hpp"
#include "source/rtlsdr_source.hpp"
#include "source/rx888_source.hpp"
#include "source/sdrplay_source.hpp"
#include "source/soapy_source.hpp"
#include "test_check.hpp"

#if defined(_WIN32)
#include <process.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace fs = std::filesystem;
namespace health = cascade::core::health;
using cascade::core::PluginCatalogEntry;
using cascade::core::PluginPlatform;
using cascade::core::PluginRepo;
using health::Counts;

namespace {

const char* g_abiFixture = nullptr;       // argv[1]: a plugin that declares another ABI
const char* g_declineFixture = nullptr;   // argv[2]: one whose query declines this host

unsigned long pidNow() {
#if defined(_WIN32)
    return ::GetCurrentProcessId();
#else
    return static_cast<unsigned long>(::getpid());
#endif
}

fs::path scratch(const char* tag) {
    const char* tmp = std::getenv("TEMP");
    const fs::path base = (tmp != nullptr && *tmp != '\0') ? fs::path(tmp) : fs::temp_directory_path();
    const fs::path d = base / (std::string("cascade-healthpaths-") + tag + "-" + std::to_string(pidNow()));
    std::error_code ec;
    fs::remove_all(d, ec);
    fs::create_directories(d, ec);
    return d;
}

void setEnv(const char* name, const char* value) {
#if defined(_WIN32)
    ::SetEnvironmentVariableA(name, value);
    _putenv_s(name, value != nullptr ? value : "");
#else
    if (value != nullptr) { ::setenv(name, value, 1); } else { ::unsetenv(name); }
#endif
}

// A fresh, armed, in-memory ledger for one case: the process-wide one every call
// site counts into, as it is in the application.
std::shared_ptr<health::HealthLedger> fresh() {
    auto g = health::globalLedger();
    g->reset();
    g->arm("", cascade::core::newInstallId(), false);
    return g;
}

std::string show(const Counts& c) { return health::encode(c); }

#define EXPECT_COUNTS(ledger, ...)                                                       \
    do {                                                                                 \
        const Counts want_ = __VA_ARGS__;                                                \
        const Counts got_ = (ledger)->counts();                                          \
        if (got_ != want_) {                                                             \
            std::printf("FAIL %s:%d counted \"%s\", wanted \"%s\"\n", __FILE__, __LINE__, \
                        show(got_).c_str(), show(want_).c_str());                        \
        }                                                                                \
        CHECK(got_ == want_);                                                            \
    } while (0)

// ---------------------------------------------------------------------------
// Nothing counts on the hot paths.
// ---------------------------------------------------------------------------
std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void testNothingOnTheHotPathsCounts() {
    const fs::path root(CASCADE_SOURCE_DIR);
    // The DSP library, the pipeline and the sink's callback never mention it.
    std::size_t files = 0;
    for (const auto& e : fs::recursive_directory_iterator(root / "src" / "dsp")) {
        if (!e.is_regular_file()) { continue; }
        ++files;
        const std::string text = readFile(e.path());
        if (text.find("health_events") != std::string::npos || text.find("health::") != std::string::npos) {
            std::printf("FAIL %s counts failures\n", e.path().string().c_str());
        }
        CHECK(text.find("health_events") == std::string::npos);
        CHECK(text.find("health::") == std::string::npos);
    }
    CHECK(files > 10);
    const std::string pipeline = readFile(root / "src" / "core" / "pipeline.cpp");
    CHECK(pipeline.find("health::") == std::string::npos);
    CHECK(pipeline.find("health_events") == std::string::npos);
    // audio_out.cpp counts ONLY in open(), never in the callback: pullBlock's body has no call.
    const std::string audio = readFile(root / "src" / "sink" / "audio_out.cpp");
    const std::size_t from = audio.find("std::size_t AudioOut::pullBlock(");
    const std::size_t to = audio.find("std::size_t AudioOut::ringFrames()");
    CHECK(from != std::string::npos && to != std::string::npos && to > from);
    CHECK(audio.substr(from, to - from).find("health::") == std::string::npos);
    CHECK(audio.substr(from, to - from).find("health_events") == std::string::npos);
    // ...and the one note in it sits after the lock is released (open()), not in openLocked().
    const std::size_t note = audio.find("noteSoundFail");
    const std::size_t locked = audio.find("bool AudioOut::openLocked(");
    CHECK(note != std::string::npos && locked != std::string::npos && note < locked);
}

// WHAT THE PROGRAM RECOVERED FROM (0.99.65), where it is counted and where it must NOT be.
//
//   - A signal thread (the source and DSP threads in pipeline.cpp, everything in src/dsp, the audio
//     callback, the plugin runner that runs on the DSP thread) never counts: its recoveries are COUNTERS
//     (Pipeline::abandonedSourceThreads / ringDroppedSamples / dspThreadExceptions, SoapySource::
//     driverCallsAbandoned) that a poll the window already makes reads (RecoveryWatch), or a FLAG raised
//     with one relaxed OR and counted by that poll (raiseRecovered / drainRecovered).
//   - The one place in the tree that raises a flag is the plugin host-API trampolines, and it raises only.
//   - Every word of the vocabulary has exactly the sites the survey (docs/DIAGNOSTICS.md) names - no word
//     is counted somewhere nobody listed, and none is listed and never counted.
std::string stripComments(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text.compare(i, 2, "//") == 0) {
            while (i < text.size() && text[i] != '\n') { ++i; }
            out += '\n';
        } else {
            out += text[i];
        }
    }
    return out;
}

void testRecoveriesAreCountedOnlyWhereTheSurveySaysAndNeverOnASignalThread() {
    const fs::path root(CASCADE_SOURCE_DIR);
    // 1. A signal thread never counts, never takes the ledger's lock, and never raises a flag either
    //    (a counter it already keeps is read by the poll).
    std::vector<fs::path> signalFiles;
    for (const auto& e : fs::recursive_directory_iterator(root / "src" / "dsp")) {
        if (e.is_regular_file()) { signalFiles.push_back(e.path()); }
    }
    signalFiles.push_back(root / "src" / "core" / "pipeline.cpp");
    signalFiles.push_back(root / "src" / "core" / "pipeline.hpp");
    signalFiles.push_back(root / "src" / "core" / "plugin_runner.cpp");
    for (const fs::path& f : signalFiles) {
        const std::string code = stripComments(readFile(f));
        CHECK(!code.empty());
        for (const char* banned : {"noteRecovered", "raiseRecovered", "drainRecovered", "noteSlowFrame", "health::note",
                                   "health_events", "->note(", "globalLedger"}) {
            if (code.find(banned) != std::string::npos) {
                std::printf("FAIL %s mentions %s on a signal thread\n", f.string().c_str(), banned);
            }
            CHECK(code.find(banned) == std::string::npos);
        }
    }
    // the audio callback's body, comments aside
    {
        const std::string audio = readFile(root / "src" / "sink" / "audio_out.cpp");
        const std::size_t from = audio.find("std::size_t AudioOut::pullBlock(");
        const std::size_t to = audio.find("std::size_t AudioOut::ringFrames()");
        CHECK(from != std::string::npos && to != std::string::npos && to > from);
        const std::string body = stripComments(audio.substr(from, to - from));
        CHECK(body.find("Recovered") == std::string::npos);
        CHECK(body.find("raiseRecovered") == std::string::npos);
    }
    // The DSP thread's catch blocks keep a COUNTER and nothing else of this kind.
    {
        const std::string pipe = stripComments(readFile(root / "src" / "core" / "pipeline.cpp"));
        std::size_t at = 0;
        int blocks = 0;
        while ((at = pipe.find("noteThreadFault(\"DSP thread\"", at)) != std::string::npos) {
            ++blocks;
            CHECK(pipe.rfind("dspExceptions_.fetch_add", at) != std::string::npos &&
                  at - pipe.rfind("dspExceptions_.fetch_add", at) < 120);
            at += 10;
        }
        CHECK(blocks == 2);
    }
    // 2. The plugin host-API trampolines: raise a flag, and nothing that takes a lock.
    {
        const std::string ui = stripComments(readFile(root / "src" / "core" / "plugin_ui.cpp"));
        CHECK(ui.find("noteRecovered") == std::string::npos);
        CHECK(ui.find("health::note") == std::string::npos);
        CHECK(ui.find("globalLedger") == std::string::npos);
        std::size_t raises = 0;
        for (std::size_t at = 0; (at = ui.find("raiseHostFault();", at)) != std::string::npos; at += 10) { ++raises; }
        CHECK(raises == 6);            // the four host calls and the two level-1 helpers' catch blocks
        CHECK(ui.find("health::raiseRecovered(cascade::core::health::Recovered::PluginApi)") != std::string::npos);
    }
    // 3. Every word of the vocabulary has the sites the survey names, and no others.
    struct Site {
        const char* word;
        const char* file;
        int count;          // occurrences of `Recovered::<word>` in that file
    };
    const std::vector<Site> sites = {
        {"Audio", "src/gui/app_window.cpp", 1},      {"Reopen", "src/gui/app_window.cpp", 1},
        {"CfgSave", "src/gui/app_window.cpp", 1},    {"PatchLoad", "src/gui/app_window.cpp", 1},
        {"SdrEnum", "src/gui/app_window.cpp", 1},    {"SdrLost", "src/gui/app_window.cpp", 1},
        {"EnumChild", "src/source/soapy_enum_proc.cpp", 1},
        {"PluginApi", "src/core/plugin_ui.cpp", 1},  {"WebRoute", "src/net/web_server.cpp", 1},
    };
    // The four counters a poll reads: named in the window's poll, where the readings are made.
    const std::string window = readFile(root / "src" / "gui" / "app_window.cpp");
    for (const char* reading : {"now.sourceThreadsAbandoned = static_cast<unsigned long long>(pipeline_.abandonedSourceThreads())",
                                "now.vendorCallsAbandoned = cascade::source::SoapySource::driverCallsAbandoned()",
                                "now.ringDroppedSamples = pipeline_.ringDroppedSamples()",
                                "now.dspThreadExceptions = static_cast<unsigned long long>(pipeline_.dspThreadExceptions())",
                                "recoveryWatch_.poll(now)", "cascade::core::health::drainRecovered()"}) {
        if (window.find(reading) == std::string::npos) { std::printf("FAIL the window's poll lacks %s\n", reading); }
        CHECK(window.find(reading) != std::string::npos);
    }
    const std::set<std::string> watched = {"SrcThread", "VendorCall", "RingDrop", "DspExc"};
    CHECK(sites.size() + watched.size() == health::kRecoveredCount);
    // scan the whole tree for uses outside the vocabulary's own files
    std::map<std::string, std::map<std::string, int>> found;   // word -> file -> count
    for (const auto& e : fs::recursive_directory_iterator(root / "src")) {
        if (!e.is_regular_file()) { continue; }
        const std::string ext = e.path().extension().string();
        if (ext != ".cpp" && ext != ".hpp" && ext != ".h") { continue; }
        const std::string name = e.path().filename().string();
        if (name == "health_events.cpp" || name == "health_events.hpp") { continue; }
        const std::string code = stripComments(readFile(e.path()));
        const std::string rel = fs::relative(e.path(), root).generic_string();
        for (std::size_t at = 0; (at = code.find("Recovered::", at)) != std::string::npos; at += 11) {
            std::size_t end = at + 11;
            while (end < code.size() && (std::isalnum(static_cast<unsigned char>(code[end])) != 0)) { ++end; }
            ++found[code.substr(at + 11, end - at - 11)][rel];
        }
    }
    for (const Site& s : sites) {
        const auto it = found.find(s.word);
        CHECK(it != found.end());
        if (it == found.end()) { std::printf("FAIL Recovered::%s is counted nowhere\n", s.word); continue; }
        CHECK(it->second.size() == 1);
        const auto f = it->second.find(s.file);
        CHECK(f != it->second.end() && f->second == s.count);
        if (f == it->second.end() || f->second != s.count) {
            std::printf("FAIL Recovered::%s is used in:", s.word);
            for (const auto& kv : it->second) { std::printf(" %s x%d", kv.first.c_str(), kv.second); }
            std::printf(" (expected %s x%d)\n", s.file, s.count);
        }
    }
    // ...and nothing else is counted that the table does not know (the watched four have no direct use at all).
    std::size_t known = 0;
    for (const auto& kv : found) {
        const bool listed = std::any_of(sites.begin(), sites.end(), [&](const Site& s) { return kv.first == s.word; });
        if (!listed) { std::printf("FAIL Recovered::%s is used outside the survey's table\n", kv.first.c_str()); }
        CHECK(listed);
        known += listed ? 1 : 0;
    }
    CHECK(known == sites.size());
}

// ---------------------------------------------------------------------------
// SLOW FRAMES: counted where the frame timer commits one, by the PROCESS's
// timer only, and never by waiting on a disk.
// ---------------------------------------------------------------------------
namespace slowfake {
std::int64_t g_now = 5'000'000'000'000LL;
std::int64_t steady() { return g_now; }
std::int64_t awake() { return g_now; }
void advanceMs(std::int64_t ms) { g_now += ms * 1'000'000LL; }

std::atomic<int> g_stalls{0};
void stallTheWriter() {
    ++g_stalls;
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
}

// One frame in which `scope` took `ms`, then the next frame's top, which is where a held slow frame is
// settled (counted) - as the window's loop does it.
template <class Timer>
void slowFrame(Timer& t, long index, cascade::core::FrameScope scope, std::int64_t ms, bool discard = false) {
    t.beginFrame(index);
    t.settlePrevious(false);
    {
        cascade::core::FrameScopeGuard g(t, scope);
        advanceMs(ms);
    }
    t.endFrame();
    t.beginFrame(index + 1);
    t.settlePrevious(discard);
    t.endFrame();
}
}  // namespace slowfake

void testASlowFrameIsCountedByTheProcessTimerOnly() {
    using cascade::core::FrameScope;
    using cascade::core::FrameTimer;
    auto g = fresh();

    // THE PROCESS TIMER (the one the window drives): each slow frame is one count, in its scope and tier.
    {
        FrameTimer proc{FrameTimer::ProcessTimer{}};
        proc.setClocksForTest(&slowfake::steady, &slowfake::awake);
        proc.setSinkForTest([](bool, const char*) {});
        slowfake::slowFrame(proc, 40, FrameScope::Rail, 300);                  // a stutter
        slowfake::slowFrame(proc, 42, FrameScope::Rail, 300);                  // another, same cell
        slowfake::slowFrame(proc, 44, FrameScope::PluginsReload, 1200);        // a freeze the person noticed
        slowfake::slowFrame(proc, 46, FrameScope::Saves, 6000);                // what the watchdog would have said
        slowfake::slowFrame(proc, 48, FrameScope::Spectrum, 249);              // not slow: not counted
        slowfake::slowFrame(proc, 50, FrameScope::Render, 400, /*discard=*/true);  // display changed: dropped
        slowfake::slowFrame(proc, 5, FrameScope::Rail, 260);                   // the first 30 frames are start-up
        EXPECT_COUNTS(g, (Counts{{"slow.rail.250ms", 2}, {"slow.plugins-reload.1s", 1}, {"slow.saves.5s", 1},
                                 {"slow.startup.250ms", 1}}));
        // The timer's own table says the same thing.
        CHECK(proc.counts().count[static_cast<int>(FrameScope::Rail)][0] == 2);
        CHECK(proc.counts().count[static_cast<int>(FrameScope::PluginsReload)][1] == 1);
    }
    // THE PERSON'S OWN TIME is never counted: a Windows prompt answered inside the frame.
    {
        FrameTimer proc{FrameTimer::ProcessTimer{}};
        proc.setClocksForTest(&slowfake::steady, &slowfake::awake);
        proc.setSinkForTest([](bool, const char*) {});
        auto g2 = fresh();
        proc.beginFrame(60);
        proc.settlePrevious(false);
        proc.userWaitBegin();
        slowfake::advanceMs(4000);
        proc.userWaitEnd();
        proc.endFrame();
        proc.beginFrame(61);
        proc.settlePrevious(false);
        proc.endFrame();
        CHECK(g2->counts().empty());
    }
    // A TIMER A TEST BUILDS counts into its own table and into nothing else.
    {
        auto g3 = fresh();
        FrameTimer built(&slowfake::steady, &slowfake::awake);
        built.setSinkForTest([](bool, const char*) {});
        slowfake::slowFrame(built, 70, FrameScope::Toolbar, 500);
        slowfake::slowFrame(built, 72, FrameScope::Toolbar, 2000);
        CHECK(built.counts().count[static_cast<int>(FrameScope::Toolbar)][0] == 1);
        CHECK(built.counts().count[static_cast<int>(FrameScope::Toolbar)][1] == 1);
        CHECK(g3->counts().empty());
    }
    // Reporting off: the process timer counts its table and the ledger counts nothing.
    {
        auto g4 = health::globalLedger();
        g4->reset();
        g4->disarm();
        FrameTimer proc{FrameTimer::ProcessTimer{}};
        proc.setClocksForTest(&slowfake::steady, &slowfake::awake);
        proc.setSinkForTest([](bool, const char*) {});
        slowfake::slowFrame(proc, 80, FrameScope::Rail, 300);
        CHECK(proc.counts().count[static_cast<int>(FrameScope::Rail)][0] == 1);
        CHECK(g4->counts().empty());
    }
}

void testACommittedSlowFrameNeverWaitsOnTheDisk() {
    using cascade::core::FrameScope;
    using cascade::core::FrameTimer;
    // The window's thread commits a slow frame (FrameTimer::commit). With the ledger's WRITER made to
    // take 600 ms - a slow share, a disk spinning up - the commits still return at once, and the file
    // arrives afterwards: the time the window's thread spent is what moves with a synchronous write.
    const fs::path dir = scratch("slowdisk");
    const std::string id = cascade::core::newInstallId();
    const std::string file = health::HealthLedger::pathIn(dir.string());
    auto g = health::globalLedger();
    g->reset();
    g->arm(file, id, false);
    slowfake::g_stalls = 0;
    health::HealthLedger::setWriteHookForTest(&slowfake::stallTheWriter);

    FrameTimer proc{FrameTimer::ProcessTimer{}};
    proc.setClocksForTest(&slowfake::steady, &slowfake::awake);
    proc.setSinkForTest([](bool, const char*) {});
    double worstMs = 0.0, totalMs = 0.0;
    for (int i = 0; i < 40; ++i) {
        // the commit happens in settlePrevious at the top of the NEXT frame: time that call
        proc.beginFrame(100 + 4 * i);
        proc.settlePrevious(false);
        { cascade::core::FrameScopeGuard s(proc, FrameScope::Rail); slowfake::advanceMs(300); }
        proc.endFrame();
        const auto t0 = std::chrono::steady_clock::now();
        proc.beginFrame(101 + 4 * i);
        proc.settlePrevious(false);
        const double ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        proc.endFrame();
        worstMs = std::max(worstMs, ms);
        totalMs += ms;
        if (totalMs > 2000.0) { break; }    // a commit that waits on the disk: report it, do not sit out 40 stalls
    }
    std::printf("health: 40 slow frames committed with the writer stalled 600 ms: total %.3f ms, worst %.3f ms\n",
                totalMs, worstMs);
    CHECK(totalMs < 300.0);
    CHECK(g->flush(10000));
    health::HealthLedger::setWriteHookForTest(nullptr);
    CHECK(slowfake::g_stalls.load() >= 1);
    Counts onDisk;
    CHECK(health::HealthLedger::parseFileText(readFile(file), id, onDisk));
    CHECK(onDisk == (Counts{{"slow.rail.250ms", 40}}));
    g->reset();
}

// ---------------------------------------------------------------------------
// SOUND: the real output, refused.
// ---------------------------------------------------------------------------
void testASoundOutputThatCannotBeOpenedIsCounted() {
    auto g = fresh();
    cascade::sink::AudioOut out;

    // No such device: PortAudio is never asked, the host API is not reached.
    CHECK(!out.open(99999, 48000.0, 1));
    EXPECT_COUNTS(g, (Counts{{"sound_fail.none.nodevice", 1}}));

    // The request itself unusable: a rate of zero.
    CHECK(!out.open(-1, 0.0, 1));
    EXPECT_COUNTS(g, (Counts{{"sound_fail.none.nodevice", 1}, {"sound_fail.none.format", 1}}));

    // A repeated refusal - the audio watchdog retries once a second - is one count a session.
    for (int i = 0; i < 5; ++i) { CHECK(!out.open(99999, 48000.0, 1)); }
    EXPECT_COUNTS(g, (Counts{{"sound_fail.none.nodevice", 1}, {"sound_fail.none.format", 1}}));

    // A REFUSAL FROM THE HOST API ITSELF, when this machine has an output to refuse
    // with: a rate of 1 Hz is turned away by the real PortAudio, and the count names
    // the host API (never the device) and a reason read off the error CODE.
    const std::vector<cascade::sink::AudioDevice> devices = out.listOutputDevices();
    if (devices.empty()) {
        std::printf("SKIP the host API's own refusal: no output device on this machine\n");
        ++g_checksSkipped;
    } else {
        g->reset();
        g->arm("", cascade::core::newInstallId(), false);
        const bool opened = out.open(-1, 1.0, 1);
        if (opened) {
            out.close();
            std::printf("SKIP the host API's own refusal: this host API took a 1 Hz stream\n");
            ++g_checksSkipped;
        } else {
            const Counts c = g->counts();
            CHECK(c.size() == 1);
            const std::string token = c.empty() ? std::string() : c.begin()->first;
            std::printf("health: the host API refused a 1 Hz stream as %s\n", token.c_str());
            CHECK(token.rfind("sound_fail.", 0) == 0);
            CHECK(token.rfind("sound_fail.none.", 0) != 0);   // a host API WAS reached
            // never a device name: it is all vocabulary
            CHECK(health::validToken(token));
        }
    }
    g->reset();
}

// ---------------------------------------------------------------------------
// RADIOS: the real drivers' sentences for a radio that is not there.
// ---------------------------------------------------------------------------
template <typename Source>
void expectNativeDriverRefusal(const char* kind) {
    Source src;
    const bool opened = src.open("serial=HEALTHTESTNOSUCHRADIO");
    CHECK(!opened);
    const std::string said = src.lastError();
    CHECK(!said.empty());
    const health::RadioReason why = health::classifyRadioOpen(kind, said);
    // A serial that is not there: the radio is not bound (the transport sees no device
    // of that kind) or not present (it sees others, but not this serial) - never "other".
    if (why != health::RadioReason::Bind && why != health::RadioReason::Absent) {
        std::printf("FAIL %s said \"%s\" -> %s\n", kind, said.c_str(), health::radioReasonWord(why));
    }
    CHECK(why == health::RadioReason::Bind || why == health::RadioReason::Absent);
    // And what is counted is vocabulary only, whatever the sentence quoted back.
    auto g = fresh();
    health::noteRadioFail(kind, said);
    CHECK(g->counts().size() == 1);
    const std::string token = g->counts().begin()->first;
    CHECK(token.rfind(std::string("radio_fail.") + kind + ".", 0) == 0);
    CHECK(token.find("HEALTHTEST") == std::string::npos);
    g->reset();
}

void testEveryNativeDriversRefusalIsClassified() {
    expectNativeDriverRefusal<cascade::source::RtlSdrSource>("rtlsdr");
    expectNativeDriverRefusal<cascade::source::HackRfSource>("hackrf");
    expectNativeDriverRefusal<cascade::source::AirspySource>("airspy");
    expectNativeDriverRefusal<cascade::source::AirspyHfSource>("airspyhf");
    expectNativeDriverRefusal<cascade::source::HydraSdrSource>("hydrasdr");
    expectNativeDriverRefusal<cascade::source::MiriSdrSource>("mirisdr");
    expectNativeDriverRefusal<cascade::source::Rx888Source>("rx888");
}

void testTheAorDriversRefusalIsClassified() {
    cascade::source::AorSource src;
    CHECK(!src.open("serial=HEALTHTESTNOSUCHRADIO"));
    const std::string said = src.lastError();
    CHECK(!said.empty());
    const health::RadioReason why = health::classifyRadioOpen("aor", said);
    if (why != health::RadioReason::Absent && why != health::RadioReason::Bind) {
        std::printf("FAIL aor said \"%s\" -> %s\n", said.c_str(), health::radioReasonWord(why));
    }
    CHECK(why == health::RadioReason::Absent || why == health::RadioReason::Bind);
}

void testASoapyDriverThatIsNotThereIsClassified() {
    cascade::source::SoapySource src;
    CHECK(!src.open("driver=healthtestnosuchdriver"));
    const std::string said = src.lastError();
    CHECK(!said.empty());
    const health::RadioReason why = health::classifyRadioOpen("soapy", said);
    std::printf("health: SoapySDR said \"%s\" -> %s\n", said.c_str(), health::radioReasonWord(why));
    // The vendor stack's own words: not found, or its own error. Never "other" for soapy.
    CHECK(why == health::RadioReason::Absent || why == health::RadioReason::Vendor ||
          why == health::RadioReason::Driver);
}

void testTheSdrPlayDriversThreeRefusals() {
    namespace abi = cascade::source::sdrplay_abi;
    // 1. NO API INSTALLED: the table resolved nothing.
    {
        abi::Api absent;
        cascade::source::SdrPlaySource src;
        src.setApiForTest(&absent);
        CHECK(!src.open(""));
        CHECK(health::classifyRadioOpen("sdrplay", src.lastError()) == health::RadioReason::Driver);
    }
    // 2. THE SERVICE DID NOT ANSWER: Open() fails.
    {
        fakesdrplay::FakeSdrPlayApi fake;
        fake.openResult = abi::Fail;
        cascade::source::SdrPlaySource src;
        src.setApiForTest(&fake.table);
        CHECK(!src.open(""));
        const std::string said = src.lastError();
        if (health::classifyRadioOpen("sdrplay", said) != health::RadioReason::Driver) {
            std::printf("FAIL sdrplay said \"%s\"\n", said.c_str());
        }
        CHECK(health::classifyRadioOpen("sdrplay", said) == health::RadioReason::Driver);
    }
    // 3. THE SERVICE IS UP AND NO RSP IS PLUGGED IN.
    {
        fakesdrplay::FakeSdrPlayApi fake;
        cascade::source::SdrPlaySource src;
        src.setApiForTest(&fake.table);
        CHECK(!src.open(""));
        const std::string said = src.lastError();
        if (health::classifyRadioOpen("sdrplay", said) != health::RadioReason::Absent) {
            std::printf("FAIL sdrplay said \"%s\"\n", said.c_str());
        }
        CHECK(health::classifyRadioOpen("sdrplay", said) == health::RadioReason::Absent);
    }
}

// ---------------------------------------------------------------------------
// UPDATES, through the transport seam.
// ---------------------------------------------------------------------------
struct Served {
    std::string body;
    bool fail = false;
    std::string error = "the server returned HTTP 500";
    std::atomic<int> hits{0};
};

void serve(const std::shared_ptr<Served>& s) {
    PluginRepo::setTransportForTest([s](const std::string&, std::uint64_t,
                                        const std::function<bool(const void*, std::size_t)>& sink,
                                        std::string& error) {
        ++s->hits;
        if (s->fail) {
            error = s->error;
            return false;
        }
        return sink(s->body.data(), s->body.size());
    });
}

std::string sha256Of(const std::string& bytes) {
    std::string hex, err;
    CHECK(PluginRepo::sha256Hex(bytes.data(), bytes.size(), hex, err));
    return hex;
}

void testUpdateChecksThatFailAreCounted() {
    auto g = fresh();
    auto s = std::make_shared<Served>();
    serve(s);
    cascade::core::UpdateInfo info;
    std::string err;

    // The service cannot be reached.
    s->fail = true;
    CHECK(!cascade::core::checkForUpdate("https://foxsdr.com/api/update", "0.99.64", "", info, err));
    EXPECT_COUNTS(g, (Counts{{"upd_check", 1}}));
    // It answers with something that is not a manifest.
    s->fail = false;
    s->body = "<html>not a manifest</html>";
    CHECK(!cascade::core::checkForUpdate("https://foxsdr.com/api/update", "0.99.64", "", info, err));
    EXPECT_COUNTS(g, (Counts{{"upd_check", 2}}));
    // A manifest this build refuses (a download from somewhere else) is a check that failed too.
    s->body = "{\"version\":\"9.9.9\",\"url\":\"https://elsewhere.example/x.exe\",\"sha256\":\"" +
              std::string(64, 'a') + "\"}";
    CHECK(!cascade::core::checkForUpdate("https://foxsdr.com/api/update", "0.99.64", "", info, err));
    EXPECT_COUNTS(g, (Counts{{"upd_check", 3}}));

    // A good manifest counts nothing - and neither does an old build being told nothing is newer.
    s->body = "{\"version\":\"9.9.9\",\"url\":\"https://foxsdr.com/download/foxsdr-setup-9.9.9.exe\","
              "\"sha256\":\"" + std::string(64, 'a') + "\"}";
    CHECK(cascade::core::checkForUpdate("https://foxsdr.com/api/update", "0.99.64", "", info, err));
    CHECK(info.newer);
    EXPECT_COUNTS(g, (Counts{{"upd_check", 3}}));
    PluginRepo::setTransportForTest(nullptr);
    g->reset();
}

void testUpdateDownloadsThatFailAreCounted() {
    auto g = fresh();
    auto s = std::make_shared<Served>();
    serve(s);
    const std::string version = "0.0.1-healthtest." + std::to_string(pidNow());
    const std::string payload = "pretend this is an installer";
    cascade::core::UpdateInfo info;
    info.newer = true;
    info.version = version;
    info.url = "https://foxsdr.com/download/foxsdr-setup-" + version + ".exe";
    info.sha256 = sha256Of(payload);
    std::string path, err;

    // THE BYTES ARRIVE AND ARE NOT THE PUBLISHED ONES: verification fails, and the
    // file is not left behind as an installer.
    s->body = payload + " (tampered)";
    CHECK(!cascade::core::downloadUpdate(info, path, err));
    CHECK(err.find("sha256 mismatch") == 0);
    CHECK(path.empty());
    EXPECT_COUNTS(g, (Counts{{"upd_verify", 1}}));

    // THE DOWNLOAD DOES NOT COMPLETE.
    s->fail = true;
    CHECK(!cascade::core::downloadUpdate(info, path, err));
    EXPECT_COUNTS(g, (Counts{{"upd_verify", 1}, {"upd_dl", 1}}));

    // A CANCEL IS NEITHER: a quit, or the user withdrawing.
    s->fail = false;
    s->body = payload;
    std::atomic<bool> cancel{true};
    CHECK(!cascade::core::downloadUpdate(info, path, err, nullptr, &cancel));
    CHECK(err == "cancelled");
    s->fail = true;
    s->error = "cancelled";
    cancel = false;
    CHECK(!cascade::core::downloadUpdate(info, path, err, nullptr, &cancel));
    // ...and a refusal before any transfer (a URL that is not foxsdr.com's) is not a download that failed.
    cascade::core::UpdateInfo hostile = info;
    hostile.url = "https://elsewhere.example/x.exe";
    CHECK(!cascade::core::downloadUpdate(hostile, path, err));
    EXPECT_COUNTS(g, (Counts{{"upd_verify", 1}, {"upd_dl", 1}}));

    // A download that matches counts nothing.
    s->fail = false;
    s->error.clear();
    s->body = payload;
    CHECK(cascade::core::downloadUpdate(info, path, err));
    CHECK(!path.empty());
    EXPECT_COUNTS(g, (Counts{{"upd_verify", 1}, {"upd_dl", 1}}));
    std::error_code ec;
    fs::remove(path, ec);
    PluginRepo::setTransportForTest(nullptr);
    g->reset();
}

void testTheStoresCheckAndInstallThroughTheEnvironmentSeams() {
    auto g = fresh();
    // THE CHECK THAT DOES NOT COMPLETE.
    setEnv("FOXSDR_FAKE_STORE_UPDATE", "error");
    CHECK(!cascade::core::checkStoreForUpdates(nullptr).ok);
    EXPECT_COUNTS(g, (Counts{{"upd_check", 1}}));
    // An answer, whatever it says, is not a failure.
    setEnv("FOXSDR_FAKE_STORE_UPDATE", "none");
    CHECK(cascade::core::checkStoreForUpdates(nullptr).ok);
    setEnv("FOXSDR_FAKE_STORE_UPDATE", "available");
    CHECK(cascade::core::checkStoreForUpdates(nullptr).ok);
    EXPECT_COUNTS(g, (Counts{{"upd_check", 1}}));

    // THE INSTALL REQUEST THAT FAILS - not one the user declined, and not one that succeeded.
    setEnv("FOXSDR_FAKE_STORE_INSTALL", "cancelled");
    CHECK(cascade::core::requestStoreUpdateInstall(nullptr).result ==
          cascade::core::StoreInstallResult::Cancelled);
    setEnv("FOXSDR_FAKE_STORE_INSTALL", "installed");
    CHECK(cascade::core::requestStoreUpdateInstall(nullptr).result ==
          cascade::core::StoreInstallResult::Installed);
    EXPECT_COUNTS(g, (Counts{{"upd_check", 1}}));
    setEnv("FOXSDR_FAKE_STORE_INSTALL", "failed");
    CHECK(cascade::core::requestStoreUpdateInstall(nullptr).result ==
          cascade::core::StoreInstallResult::Failed);
    EXPECT_COUNTS(g, (Counts{{"upd_check", 1}, {"upd_run", 1}}));
    setEnv("FOXSDR_FAKE_STORE_UPDATE", nullptr);
    setEnv("FOXSDR_FAKE_STORE_INSTALL", nullptr);
    g->reset();
}

// ---------------------------------------------------------------------------
// PLUGINS: the catalogue and the installer, through the same seam.
// ---------------------------------------------------------------------------
#if defined(_WIN32)
const std::string kExt = ".dll";
#else
const std::string kExt = ".so";
#endif

PluginCatalogEntry entryFor(const std::string& stem, const std::string& sha) {
    PluginCatalogEntry e;
    e.id = "healthprobe";
    e.name = "Health Probe";
    e.version = "1.0.0";
    e.abiVersion = static_cast<std::uint32_t>(CASCADE_PLUGIN_ABI_VERSION);
    e.compatible = true;
    PluginPlatform p;
    p.os = PluginRepo::hostOs();
    p.arch = PluginRepo::hostArch();
    p.file = stem + kExt;
    p.url = "https://example.invalid/" + stem + kExt;
    p.sha256 = sha;
    e.platforms.push_back(p);
    return e;
}

void testACatalogueThatCannotBeHadIsCounted() {
    auto g = fresh();
    auto s = std::make_shared<Served>();
    serve(s);
    PluginRepo repo;
    std::string err;

    s->fail = true;
    CHECK(!repo.fetchIndex("https://example.invalid/index.json", err));
    EXPECT_COUNTS(g, (Counts{{"plug_cat", 1}}));
    s->fail = false;
    s->body = "this is not a catalogue";
    CHECK(!repo.fetchIndex("https://example.invalid/index.json", err));
    EXPECT_COUNTS(g, (Counts{{"plug_cat", 2}}));
    CHECK(!repo.fetchIndex("http://example.invalid/index.json", err));   // refused before any socket
    EXPECT_COUNTS(g, (Counts{{"plug_cat", 3}}));

    // A cancel (a quit while it was fetching) is not a catalogue that failed.
    PluginRepo::setTransportForTest([&repo](const std::string&, std::uint64_t,
                                            const std::function<bool(const void*, std::size_t)>&,
                                            std::string& error) {
        repo.cancel();
        error = "cancelled";
        return false;
    });
    CHECK(!repo.fetchIndex("https://example.invalid/index.json", err));
    EXPECT_COUNTS(g, (Counts{{"plug_cat", 3}}));

    // A catalogue counts nothing.
    serve(s);
    s->body = "{\"schemaVersion\":1,\"plugins\":[]}";
    CHECK(repo.fetchIndex("https://example.invalid/index.json", err));
    EXPECT_COUNTS(g, (Counts{{"plug_cat", 3}}));
    PluginRepo::setTransportForTest(nullptr);
    g->reset();
}

void testAPluginInstallThatFailsIsCountedByClass() {
    auto g = fresh();
    auto s = std::make_shared<Served>();
    serve(s);
    const fs::path dir = scratch("install");
    PluginRepo repo;
    std::string path, err;
    const std::string payload = "pretend this is a plugin module";
    const std::string rightHash = sha256Of(payload);
    const std::string wrongHash(64, 'a');

    // THE HASH: the bytes arrive, do not match, and nothing is left in the plugins folder.
    s->body = payload;
    CHECK(!repo.install(entryFor("healthprobe", wrongHash), (dir / "plugins").string(), path, err));
    CHECK(err.find("integrity check") != std::string::npos);
    EXPECT_COUNTS(g, (Counts{{"plug_inst.hash", 1}}));
    CHECK(!fs::exists(dir / "plugins" / ("healthprobe" + kExt)));
    std::size_t debris = 0;
    for (const auto& e : fs::directory_iterator(dir / "plugins")) { (void)e; ++debris; }
    CHECK(debris == 0);

    // THE NETWORK.
    s->fail = true;
    CHECK(!repo.install(entryFor("healthprobe", rightHash), (dir / "plugins").string(), path, err));
    EXPECT_COUNTS(g, (Counts{{"plug_inst.hash", 1}, {"plug_inst.net", 1}}));

    // THE DISK: the plugins folder cannot be made (its parent is a file).
    s->fail = false;
    {
        std::ofstream(dir / "afile") << "x";
    }
    CHECK(!repo.install(entryFor("healthprobe", rightHash), (dir / "afile" / "plugins").string(), path, err));
    EXPECT_COUNTS(g, (Counts{{"plug_inst.hash", 1}, {"plug_inst.net", 1}, {"plug_inst.write", 1}}));
    // ...and the temporary file cannot be created (a directory is where it would go).
    fs::create_directories(dir / "plugins2");
    // (not empty: install() clears a leftover temporary first, and removes an empty directory too)
    const fs::path blocker = dir / "plugins2" / ("healthprobe" + kExt + "." + std::to_string(pidNow()) + ".part");
    fs::create_directories(blocker);
    {
        std::ofstream(blocker / "keep") << "x";
    }
    CHECK(!repo.install(entryFor("healthprobe", rightHash), (dir / "plugins2").string(), path, err));
    EXPECT_COUNTS(g, (Counts{{"plug_inst.hash", 1}, {"plug_inst.net", 1}, {"plug_inst.write", 2}}));

    // REFUSED BEFORE ANY DOWNLOAD: built for another ABI, no build for this machine, an unsafe file name.
    PluginCatalogEntry wrongAbi = entryFor("healthprobe", rightHash);
    wrongAbi.abiVersion += 1;
    CHECK(!repo.install(wrongAbi, (dir / "plugins").string(), path, err));
    PluginCatalogEntry unsafe = entryFor("..", rightHash);
    CHECK(!repo.install(unsafe, (dir / "plugins").string(), path, err));
    const int hitsBefore = s->hits.load();
    EXPECT_COUNTS(g, (Counts{{"plug_inst.hash", 1}, {"plug_inst.net", 1}, {"plug_inst.write", 2},
                             {"plug_inst.other", 2}}));
    CHECK(s->hits.load() == hitsBefore);   // no transfer was attempted for those

    // A CANCEL is not an install that failed.
    PluginRepo::setTransportForTest([](const std::string&, std::uint64_t,
                                       const std::function<bool(const void*, std::size_t)>&,
                                       std::string& error) {
        error = "cancelled";
        return false;
    });
    CHECK(!repo.install(entryFor("healthprobe", rightHash), (dir / "plugins").string(), path, err));
    EXPECT_COUNTS(g, (Counts{{"plug_inst.hash", 1}, {"plug_inst.net", 1}, {"plug_inst.write", 2},
                             {"plug_inst.other", 2}}));

    // AN INSTALL THAT WORKS counts nothing.
    serve(s);
    s->body = payload;
    CHECK(repo.install(entryFor("healthprobe", rightHash), (dir / "plugins").string(), path, err));
    CHECK(fs::exists(path));
    EXPECT_COUNTS(g, (Counts{{"plug_inst.hash", 1}, {"plug_inst.net", 1}, {"plug_inst.write", 2},
                             {"plug_inst.other", 2}}));
    PluginRepo::setTransportForTest(nullptr);
    g->reset();
}

void testPluginsRefusedAtLoadAreCounted() {
    auto g = fresh();
    const fs::path dir = scratch("load");
    // A FILE THAT IS NOT A MODULE AT ALL.
    {
        std::ofstream(dir / ("notamodule" + kExt)) << "this is text, not a module";
    }
    // A REAL MODULE THAT DECLARES ANOTHER PLUGIN ABI.
    CHECK(g_abiFixture != nullptr && g_declineFixture != nullptr);
    const auto scanRefusals = [&](const fs::path& folder) {
        cascade::core::PluginHost host;
        host.scan(folder.string());
        std::size_t refused = 0;
        for (const cascade::core::LoadedPlugin& p : host.plugins()) {
            if (!p.loaded) { ++refused; }
        }
        return refused;
    };
    if (g_abiFixture != nullptr) {
        std::error_code ec;
        fs::copy_file(g_abiFixture, dir / ("abimismatch" + kExt), fs::copy_options::overwrite_existing, ec);
        CHECK(!ec);
    }
    CHECK(scanRefusals(dir) == 2);
    EXPECT_COUNTS(g, (Counts{{"plug_load.abi", 1}, {"plug_load.load", 1}}));

    // A rescan meets the same refusals again: still one of each this session.
    CHECK(scanRefusals(dir) == 2);
    EXPECT_COUNTS(g, (Counts{{"plug_load.abi", 1}, {"plug_load.load", 1}}));

    // A REAL MODULE THAT DECLINES THIS HOST (its query returns null): what a plugin built for
    // an OLDER host does, and the same word - checked alone, because a session counts each once.
    {
        g->reset();
        g->arm("", cascade::core::newInstallId(), false);
        const fs::path only = scratch("decline");
        std::error_code ec;
        fs::copy_file(g_declineFixture, only / ("declines" + kExt), fs::copy_options::overwrite_existing, ec);
        CHECK(!ec);
        CHECK(scanRefusals(only) == 1);
        EXPECT_COUNTS(g, (Counts{{"plug_load.abi", 1}}));
    }

#if defined(_WIN32)
    // A LIBRARY THAT IS NOT A PLUGIN (it exports no plugin entry point) is a stray file, not a failure.
    {
        g->reset();
        g->arm("", cascade::core::newInstallId(), false);
        const fs::path stray = scratch("stray");
        char sys[MAX_PATH] = {0};
        ::GetSystemDirectoryA(sys, MAX_PATH);
        std::error_code ec;
        fs::copy_file(fs::path(sys) / "version.dll", stray / "version.dll", fs::copy_options::overwrite_existing, ec);
        CHECK(!ec);
        cascade::core::PluginHost host;
        host.scan(stray.string());
        EXPECT_COUNTS(g, (Counts{}));
    }
#endif
    g->reset();
}

// ---------------------------------------------------------------------------
// RECORDING: the real opener, in a folder that cannot exist.
// ---------------------------------------------------------------------------
bool waitFor(cascade::gui::RecordStart& rs, cascade::gui::RecordStart::Result& out) {
    for (int i = 0; i < 1000; ++i) {
        if (rs.poll(out)) { return true; }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

void testARecordingThatCannotBeStartedIsCounted() {
    auto g = fresh();
    const fs::path dir = scratch("record");
    {
        std::ofstream(dir / "afile") << "x";
    }
    cascade::core::Recorder::OpenRequest req;
    req.directory = (dir / "afile" / "sub").string();   // a folder under a FILE: cannot be made
    req.path = (dir / "afile" / "sub" / "take.wav").string();
    cascade::gui::RecordStart rs;
    CHECK(rs.request(&cascade::core::Recorder::openFile, req));
    cascade::gui::RecordStart::Result r;
    CHECK(waitFor(rs, r));
    CHECK(!r.ok && !r.cancelled && !r.error.empty());
    EXPECT_COUNTS(g, (Counts{{"rec_fail", 1}}));

    // A TAKE THE USER WITHDREW while it opened is not a start that failed.
    CHECK(rs.request(
        [](const cascade::core::Recorder::OpenRequest&, cascade::core::Recorder::OpenedFile&,
           std::string& error) {
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            error = "disk full";
            return false;
        },
        req));
    rs.cancel();
    CHECK(waitFor(rs, r));
    CHECK(r.cancelled);
    EXPECT_COUNTS(g, (Counts{{"rec_fail", 1}}));

    // A start that works counts nothing.
    cascade::core::Recorder::OpenRequest good;
    good.directory = (dir / "good").string();
    good.path = (dir / "good" / "take.wav").string();
    CHECK(rs.request(&cascade::core::Recorder::openFile, good));
    CHECK(waitFor(rs, r));
    CHECK(r.ok);
    EXPECT_COUNTS(g, (Counts{{"rec_fail", 1}}));
    g->reset();
}

}  // namespace

int main(int argc, char** argv) {
    if (argc > 1) { g_abiFixture = argv[1]; }
    if (argc > 2) { g_declineFixture = argv[2]; }
    testNothingOnTheHotPathsCounts();
    testRecoveriesAreCountedOnlyWhereTheSurveySaysAndNeverOnASignalThread();
    testASlowFrameIsCountedByTheProcessTimerOnly();
    testACommittedSlowFrameNeverWaitsOnTheDisk();
    testASoundOutputThatCannotBeOpenedIsCounted();
    testEveryNativeDriversRefusalIsClassified();
    testTheAorDriversRefusalIsClassified();
    testASoapyDriverThatIsNotThereIsClassified();
    testTheSdrPlayDriversThreeRefusals();
    testUpdateChecksThatFailAreCounted();
    testUpdateDownloadsThatFailAreCounted();
    testTheStoresCheckAndInstallThroughTheEnvironmentSeams();
    testACatalogueThatCannotBeHadIsCounted();
    testAPluginInstallThatFailsIsCountedByClass();
    testPluginsRefusedAtLoadAreCounted();
    testARecordingThatCannotBeStartedIsCounted();
    return testSummary("test_health_paths");
}
