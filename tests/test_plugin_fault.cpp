// test_plugin_fault.cpp - the guard around a plugin's non-signal entry points, its budgets, its counters and its crash-path attribution
// (plugin containment P1, docs/engine-stage4.md 6.28: rules 1-5 of core/plugin_fault.hpp).
//
// The plugin is a REAL module (tests/fixtures/fault_probe_plugin.cpp), loaded by the real PluginHost from a scratch folder, and the
// faults are real access violations raised inside its code:
//
//   A  THE TABLE: the entry points' budgets, words and order; every budget at least kMargin times the longest legitimate call it argues.
//   B  IDENTITY: an official plugin's id is public, any other is hashed (stable, 13 characters); slots are case-insensitive by run id.
//   C  AN ACCESS VIOLATION IN create() IS ABSORBED: the process is alive after it, the call reports Faulted, the plugin is quarantined,
//      the fault and the quarantine are counted once, the absorbed report is filed and is NOT the report of a death, and the plugin is
//      never called again (its own log shows one create and no more).
//   D  THE SAME FOR destroy(), poll_text(), poll_tracks() and poll_image(); and a C++ exception thrown across the C boundary.
//   E  A CALL PAST ITS BUDGET IS NAMED: with the existing hang watchdog running, a poll_text that sleeps past a (test-shortened) budget
//      files a hang report that names the plugin and the entry point, stage 2 quarantines the plugin, and the counters say so once.
//   F  A LATE RETURN THE MONITOR NEVER SAW (no watchdog in the process) is counted by the call's own end.
//   G  CONCURRENCY: eight threads calling a healthy plugin 2,000 times each lose nothing and leave no call in flight.
//   H  THE CRASH PATH: a plugin module on the faulting stack, or a call in flight on the faulting thread, is attributed; neither is not.
//   I  A FAULT IN THE HOST'S OWN IMAGE IS NOT ABSORBED (the predicate the guard asks), and the test seams are reachable from no product code.
//
// Windows has the structured exceptions the absorbing needs; on other platforms the faulting blocks SKIP and say so (the guard runs the
// plugin unguarded there, plugin_fault.hpp), and everything else runs.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "core/crash_handler.hpp"
#include "core/hang_watchdog.hpp"
#include "core/plugin_calls.hpp"
#include "core/plugin_fault.hpp"
#include "core/plugin_host.hpp"
#include "core/thread_beats.hpp"
#include "source/vendor_guard.hpp"
#include "test_check.hpp"
#include "fault_probe_names.hpp"

#if defined(_WIN32)
#include <windows.h>
#elif defined(__linux__)
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;
namespace fault = cascade::core::fault;
namespace calls = cascade::core::calls;
namespace beats = cascade::core::beats;
using cascade::core::HangWatchdog;
using cascade::core::LoadedPlugin;
using cascade::core::PluginHost;

namespace {

std::string g_probePath;
fs::path g_root;

void setEnvVar(const char* name, const char* value) {
#if defined(_WIN32)
    ::SetEnvironmentVariableA(name, value);
#else
    if (value == nullptr) { ::unsetenv(name); } else { ::setenv(name, value, 1); }
#endif
}

[[maybe_unused]] void sleepMs(unsigned ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

std::size_t countLines(const std::string& text, const std::string& line) {
    std::size_t n = 0;
    std::size_t at = 0;
    while ((at = text.find(line + "\n", at)) != std::string::npos) {
        if (at == 0 || text[at - 1] == '\n') { ++n; }
        at += line.size();
    }
    return n;
}

// The probe copied under a file name of its own (so: a plugin of its own) into the test's plugin folder.
const char* kCopies[] = {"fault-create", "fault-throw", "fault-destroy", "fault-text", "fault-tracks", "fault-image",
                         "fault-wedge",  "fault-late",  "fault-good",    "fault-stack"};
std::string copyFile(const char* id) { return std::string(id) + CASCADE_PROBE_SUFFIX; }

const LoadedPlugin* find(const PluginHost& host, const char* id) {
    for (const LoadedPlugin& p : host.plugins()) {
        if (cascade::core::pluginKey(p) == copyFile(id)) { return &p; }
    }
    return nullptr;
}

[[maybe_unused]] fs::path logFor(const char* id) { return g_root / (std::string(id) + ".log"); }

// ---- A ----------------------------------------------------------------------------------------------------------------------------
void testTable() {
    std::set<std::string> words;
    for (std::size_t i = 0; i < fault::kEntryCount; ++i) {
        const fault::EntrySpec& e = fault::kEntries[i];
        CHECK(static_cast<std::size_t>(e.entry) == i);  // the table is in the enum's order
        CHECK(fault::specOf(e.entry).word == e.word);
        CHECK(e.argued != nullptr && e.argued[0] != '\0');
        CHECK(e.legitMs > 0);
        // "budgets are generous": at least kMargin times the longest legitimate call the row argues.
        CHECK(e.budgetMs >= fault::kMargin * e.legitMs);
        CHECK(fault::wedgeMs(e.entry) == e.budgetMs * fault::kWedgeFactor);
        // a word that can be written into a report: lower case, digits, '-'
        CHECK(beats::reportWord(e.word) == e.word);
        words.insert(e.word);
    }
    CHECK(words.size() == fault::kEntryCount);  // distinct
    // Unload already waited 250 ms for the interface (the handshake); every plugin call is budgeted far above that.
    for (const fault::EntrySpec& e : fault::kEntries) { CHECK(e.budgetMs >= 5000); }
}

// ---- B ----------------------------------------------------------------------------------------------------------------------------
void testIdentity() {
    fault::resetForTest();
    fault::Slot* official = fault::slotFor("Pocsag-Decoder-1.0.2-abi3-win-x64.dll");
    CHECK(official == fault::slotFor("pocsag-decoder"));      // the run id, case-insensitive, versions stripped
    CHECK(official == fault::slotFor("POCSAG-DECODER.dll"));
    CHECK(std::string(fault::idOf(official)) == "pocsag-decoder");
    fault::setIdentity("pocsag-decoder", "1.0.2", fault::Origin::Official, nullptr, 0);
    CHECK(std::string(fault::publicIdOf(official)) == "pocsag-decoder");   // a catalogue id is public

    fault::Slot* site = fault::slotFor("secret-tool-0.1.0-abi3-win-x64.dll");
    fault::setIdentity("secret-tool", "0.1.0", fault::Origin::Site, nullptr, 0);
    const std::string pub = fault::publicIdOf(site);
    CHECK(pub.rfind("site-", 0) == 0 && pub.size() == 13);                 // hashed
    CHECK(pub.find("secret") == std::string::npos);
    fault::Slot* hand = fault::slotFor("mine");
    fault::setIdentity("mine", "9", fault::Origin::Unrecorded, nullptr, 0);
    CHECK(std::string(fault::publicIdOf(hand)).rfind("site-", 0) == 0);    // a hand-copied file's name is its owner's business
    CHECK(pub != fault::publicIdOf(hand));
    // stable: the same id hashes the same, however often it is asked
    CHECK(pub == fault::publicIdOf(fault::slotFor("secret-tool")));

    // an empty key is never quarantined and keeps no counters
    fault::Slot* none = fault::slotFor("");
    CHECK(!fault::quarantine(none, fault::Why::Fault, fault::Entry::Create));
    CHECK(!fault::isQuarantined(none));
}

// ---- C, D -------------------------------------------------------------------------------------------------------------------------
struct Fixture {
    PluginHost host;
    fs::path dir;
};

void* tryCreate(const LoadedPlugin& lp, fault::Outcome* outcome) {
    void* h = nullptr;
    *outcome = fault::call(lp.path, fault::Entry::Create, [&] { h = lp.decoder->create(48000u); });
    return h;
}

void testAbsorbedFaults(Fixture& fx) {
#if defined(_WIN32)
    const fs::path crashDir = g_root / "crash";
    std::error_code ec;
    fs::remove_all(crashDir, ec);
    fs::create_directories(crashDir);
    cascade::core::CrashHandlerConfig cfg;
    cfg.crashDir = crashDir.string();
    cfg.enabled = true;
    cascade::core::installCrashHandlers(cfg);

    // ---- C: create() dereferences null
    fault::resetForTest();
    const LoadedPlugin* a = find(fx.host, "fault-create");
    CHECK(a != nullptr && a->loaded);
    if (a == nullptr || !a->loaded) { return; }
    fault::setIdentity(a->path, a->version, fault::Origin::Official, a->nativeHandle, 0);
    setEnvVar("FAULT_PROBE", "create-av");
    setEnvVar("FAULT_PROBE_LOG", logFor("fault-create").string().c_str());
    fault::Outcome o = fault::Outcome::Ran;
    void* h = tryCreate(*a, &o);
    CHECK(o == fault::Outcome::Faulted);                          // THE PROCESS IS ALIVE: this line ran
    CHECK(h == nullptr);
    CHECK(fault::isQuarantinedKey(a->path));
    CHECK(fault::quarantineWhy(fault::slotFor(a->path)) == fault::Why::Fault);
    CHECK(fault::quarantineEntry(fault::slotFor(a->path)) == fault::Entry::Create);
    const fault::Totals t1 = fault::totals();
    CHECK(t1.faults == 1 && t1.quarantines == 1 && t1.wedges == 0 && t1.overruns == 0);
    const std::vector<fault::Event> events = fault::drainEvents();
    std::size_t faults = 0, quarantines = 0;
    for (const fault::Event& e : events) {
        faults += e.kind == fault::Event::Kind::Fault ? 1u : 0u;
        quarantines += e.kind == fault::Event::Kind::Quarantine ? 1u : 0u;
        CHECK(e.id == "fault-create");
        CHECK(e.entry == fault::Entry::Create);
    }
    CHECK(faults == 1 && quarantines == 1);
    CHECK(fault::drainEvents().empty());                           // drained
    // never called again: Refused, and the plugin's own log still shows the one create
    const std::string before = readFile(logFor("fault-create"));
    CHECK(countLines(before, "create") == 1);
    fault::Outcome again = fault::Outcome::Ran;
    CHECK(tryCreate(*a, &again) == nullptr);
    CHECK(again == fault::Outcome::Refused);
    CHECK(readFile(logFor("fault-create")) == before);
    CHECK(fault::totals().faults == 1);                            // a refusal is not a fault
    // the absorbed fault was FILED (rule 2 of the vendor guard, carried over): a report that says it was absorbed, names the plugin the
    // guard was inside (by its public id, found from the call in flight), and is not the report of a death
    {
        std::string report;
        for (const auto& e : fs::directory_iterator(crashDir)) {
            if (e.path().filename().string().rfind("crash-", 0) == 0) { report = readFile(e.path()); }
        }
        CHECK(!report.empty());
        CHECK(report.find("reason: fault in a plugin, absorbed") != std::string::npos);
        CHECK(countLines(report, "blamed-plugin: fault-create") == 1);
        CHECK(countLines(report, "blamed-plugin-version: 1.2.3") == 1);
        CHECK(countLines(report, "blamed-plugin-origin: official") == 1);
        CHECK(countLines(report, "blamed-plugin-how: in-flight") == 1);
        CHECK(!cascade::core::crashReportWrittenByProcess(crashDir.string(), static_cast<unsigned long>(::GetCurrentProcessId()),
                                                          std::chrono::system_clock::now() - std::chrono::hours(1)));
    }
    setEnvVar("FAULT_PROBE", nullptr);

    // ---- D1: destroy() faults. A healthy create first (the plugin is not quarantined), then a destroy that dereferences null.
    {
        const LoadedPlugin* d = find(fx.host, "fault-destroy");
        CHECK(d != nullptr && d->loaded);
        if (d != nullptr && d->loaded) {
            fault::Outcome oc = fault::Outcome::Ran;
            void* hd = tryCreate(*d, &oc);
            CHECK(oc == fault::Outcome::Ran && hd != nullptr);
            setEnvVar("FAULT_PROBE", "destroy-av");
            const bool ran = calls::destroy(fault::slotFor(d->path), d->decoder, hd);
            CHECK(!ran);
            CHECK(fault::quarantineEntry(fault::slotFor(d->path)) == fault::Entry::Destroy);
            CHECK(fault::isQuarantinedKey(d->path));
            setEnvVar("FAULT_PROBE", nullptr);
        }
    }
    // ---- D2: poll_text() faults: the ABI's own "failed permanently" (-1) comes back
    {
        const LoadedPlugin* p = find(fx.host, "fault-text");
        CHECK(p != nullptr && p->loaded);
        if (p != nullptr && p->loaded) {
            fault::Outcome oc = fault::Outcome::Ran;
            void* hp = tryCreate(*p, &oc);
            CHECK(hp != nullptr);
            char buf[64];
            CHECK(calls::pollText(fault::slotFor(p->path), p->decoder, hp, buf, sizeof(buf)) == 0);   // healthy: 0, nothing pending
            setEnvVar("FAULT_PROBE", "poll-text-av");
            CHECK(calls::pollText(fault::slotFor(p->path), p->decoder, hp, buf, sizeof(buf)) == -1);
            CHECK(fault::isQuarantinedKey(p->path));
            CHECK(fault::quarantineEntry(fault::slotFor(p->path)) == fault::Entry::PollText);
            setEnvVar("FAULT_PROBE", nullptr);
        }
    }
    // ---- D3: poll_tracks() and poll_image() fault
    {
        const LoadedPlugin* p = find(fx.host, "fault-tracks");
        CHECK(p != nullptr && p->loaded && p->trackSource != nullptr);
        if (p != nullptr && p->loaded && p->trackSource != nullptr) {
            fault::Slot* s = fault::slotFor(p->path);
            void* ht = calls::create(s, p->trackSource);
            CHECK(ht != nullptr);
            CascadeTrack tr[4];
            CHECK(calls::pollTracks(s, p->trackSource, ht, tr, 4) == 0);
            setEnvVar("FAULT_PROBE", "tracks-av");
            CHECK(calls::pollTracks(s, p->trackSource, ht, tr, 4) == -1);
            CHECK(fault::isQuarantined(s) && fault::quarantineEntry(s) == fault::Entry::PollTracks);
            setEnvVar("FAULT_PROBE", nullptr);
        }
        const LoadedPlugin* q = find(fx.host, "fault-image");
        CHECK(q != nullptr && q->loaded && q->imageDecoder != nullptr);
        if (q != nullptr && q->loaded && q->imageDecoder != nullptr) {
            fault::Slot* s = fault::slotFor(q->path);
            void* hi = calls::create(s, q->imageDecoder, 0.0, 0.0);
            CHECK(hi != nullptr);
            CascadeImage img{};
            img.structSize = sizeof(img);
            setEnvVar("FAULT_PROBE", "image-poll-av");
            CHECK(calls::pollImage(s, q->imageDecoder, hi, &img) == -1);
            CHECK(fault::isQuarantined(s) && fault::quarantineEntry(s) == fault::Entry::PollImage);
            setEnvVar("FAULT_PROBE", nullptr);
        }
    }
    cascade::core::setCrashCaptureEnabled(false, false);
#else
    (void)fx;
    SKIP_LINUX("structured exceptions: a plugin's fault cannot be absorbed off Windows (plugin_fault.hpp, the platform caveat)");
#endif
}

void testExceptionAcrossTheBoundary(Fixture& fx) {
    // A C++ exception thrown through the plugin's C entry point is absorbed on every platform (the trampoline catches it).
    fault::resetForTest();
    const LoadedPlugin* p = find(fx.host, "fault-throw");
    CHECK(p != nullptr && p->loaded);
    if (p == nullptr || !p->loaded) { return; }
    setEnvVar("FAULT_PROBE", "create-throw");
    fault::Outcome o = fault::Outcome::Ran;
    void* h = tryCreate(*p, &o);
    setEnvVar("FAULT_PROBE", nullptr);
    CHECK(o == fault::Outcome::Faulted);
    CHECK(h == nullptr);
    CHECK(fault::quarantineWhy(fault::slotFor(p->path)) == fault::Why::Exception);
    const fault::Totals t = fault::totals();
    CHECK(t.faults == 1 && t.quarantines == 1);
}

// ---- E ----------------------------------------------------------------------------------------------------------------------------
void testBudgetNamedByTheMonitor(Fixture& fx) {
    fault::resetForTest();
    fault::dropBudgetOverridesForTest();
    // The product's budgets are seconds; the test stands 200 ms in for poll_text's (the wedge limit follows: 800 ms) and the plugin
    // sleeps 2.5 s in it.
    fault::overrideBudgetForTest(fault::Entry::PollText, 200);
    const LoadedPlugin* p = find(fx.host, "fault-wedge");
    CHECK(p != nullptr && p->loaded);
    if (p == nullptr || !p->loaded) { return; }
    fault::setIdentity(p->path, p->version, fault::Origin::Official, p->nativeHandle, 0);
    fault::Slot* s = fault::slotFor(p->path);
    fault::Outcome oc = fault::Outcome::Ran;
    void* h = tryCreate(*p, &oc);
    CHECK(h != nullptr);

    const fs::path reports = g_root / "hang";
    std::error_code ec;
    fs::remove_all(reports, ec);
    fs::create_directories(reports);
    HangWatchdog::resetWedgeFileCountForTest();
    HangWatchdog w;
    w.setSuppressionForTest(HangWatchdog::SuppressionForTest::NeverSuppress);
    w.start(reports.string(), 600000);   // the GUI threshold is far away: only the call can be what is reported

    setEnvVar("FAULT_PROBE", "poll-text-wedge");
    setEnvVar("FAULT_PROBE_WEDGE_MS", "2500");
    char buf[64];
    const auto t0 = std::chrono::steady_clock::now();
    const std::int32_t r = calls::pollText(s, p->decoder, h, buf, sizeof(buf));
    const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    setEnvVar("FAULT_PROBE", nullptr);
    setEnvVar("FAULT_PROBE_WEDGE_MS", nullptr);
    w.stop();
    CHECK(took >= 2400);                                            // it really was inside the call that long
    CHECK(r == 0);                                                  // and it returned what the plugin said (it did return)

    // exactly one report file, naming the plugin and the entry point, with the calling thread's stack first
    std::vector<fs::path> files;
    for (const auto& e : fs::directory_iterator(reports)) { files.push_back(e.path()); }
    CHECK(files.size() == 1);
    if (files.size() == 1) {
        const std::string text = readFile(files[0]);
        CHECK(files[0].filename().string().find("-w") != std::string::npos);                // the worker family, not the frame loop's
        CHECK(countLines(text, "kind: hang") == 1);
        CHECK(countLines(text, "blamed-plugin: fault-wedge") == 1);
        CHECK(countLines(text, "blamed-plugin-entry: poll-text") == 1);
        CHECK(countLines(text, "thread: plugin-call") == 1);
        CHECK(countLines(text, "threshold-ms: 200") == 1);
        CHECK(text.find("a call into a plugin did not return within its budget") != std::string::npos);
        CHECK(text.find("(plugin-call, stalled)") != std::string::npos);
        CHECK(text.find("--- beats ---") != std::string::npos);
        CHECK(text.find("call fault-wedge poll-text age-ms=") != std::string::npos);        // the call is in the table at capture
    }
    // counted once each, and the stage-2 limit quarantined the plugin
    const fault::Totals t = fault::totals();
    CHECK(t.overruns == 1);
    CHECK(t.wedges == 1);
    CHECK(t.faults == 0);
    CHECK(fault::quarantineWhy(s) == fault::Why::Wedge);
    CHECK(fault::quarantineEntry(s) == fault::Entry::PollText);
    CHECK(beats::callsInFlight() == 0);                              // the table was left clean
    fault::dropBudgetOverridesForTest();
}

// ---- F ----------------------------------------------------------------------------------------------------------------------------
void testLateReturnCountedByTheCallItself(Fixture& fx) {
    fault::resetForTest();
    fault::dropBudgetOverridesForTest();
    fault::overrideBudgetForTest(fault::Entry::PollText, 100);     // wedge limit 400 ms
    const LoadedPlugin* p = find(fx.host, "fault-late");
    CHECK(p != nullptr && p->loaded);
    if (p == nullptr || !p->loaded) { return; }
    fault::Slot* s = fault::slotFor(p->path);
    fault::Outcome oc = fault::Outcome::Ran;
    void* h = tryCreate(*p, &oc);
    CHECK(h != nullptr);
    char buf[64];
    // 250 ms: past the budget, short of the wedge limit - an OVERRUN, and the plugin is not quarantined
    setEnvVar("FAULT_PROBE", "poll-text-wedge");
    setEnvVar("FAULT_PROBE_WEDGE_MS", "250");
    CHECK(calls::pollText(s, p->decoder, h, buf, sizeof(buf)) == 0);
    CHECK(fault::totals().overruns == 1 && fault::totals().wedges == 0);
    CHECK(!fault::isQuarantined(s));
    // 600 ms: past the wedge limit - a WEDGE: counted, and the plugin is switched off even though the call came back
    setEnvVar("FAULT_PROBE_WEDGE_MS", "600");
    CHECK(calls::pollText(s, p->decoder, h, buf, sizeof(buf)) == 0);
    CHECK(fault::totals().overruns == 2 && fault::totals().wedges == 1);
    CHECK(fault::quarantineWhy(s) == fault::Why::Wedge);
    // and now it is not called at all
    setEnvVar("FAULT_PROBE_WEDGE_MS", "10");
    CHECK(calls::pollText(s, p->decoder, h, buf, sizeof(buf)) == -1);
    CHECK(fault::totals().overruns == 2);
    setEnvVar("FAULT_PROBE", nullptr);
    setEnvVar("FAULT_PROBE_WEDGE_MS", nullptr);
    fault::dropBudgetOverridesForTest();
    // a healthy call under its budget counts nothing
    fault::resetForTest();
    CHECK(calls::pollText(s, p->decoder, h, buf, sizeof(buf)) == 0);
    const fault::Totals t = fault::totals();
    CHECK(t.overruns == 0 && t.wedges == 0 && t.faults == 0 && t.quarantines == 0);
}

// ---- G ----------------------------------------------------------------------------------------------------------------------------
void testConcurrency(Fixture& fx) {
    fault::resetForTest();
    const LoadedPlugin* p = find(fx.host, "fault-good");
    CHECK(p != nullptr && p->loaded);
    if (p == nullptr || !p->loaded) { return; }
    fault::Slot* s = fault::slotFor(p->path);
    fault::Outcome oc = fault::Outcome::Ran;
    void* h = tryCreate(*p, &oc);
    CHECK(h != nullptr);
    std::atomic<int> ran{0};
    std::atomic<int> notRan{0};
    std::vector<std::thread> pool;
    for (int t = 0; t < 8; ++t) {
        pool.emplace_back([&] {
            char buf[32];
            for (int i = 0; i < 2000; ++i) {
                if (calls::pollText(s, p->decoder, h, buf, sizeof(buf)) == 0) { ++ran; } else { ++notRan; }
            }
        });
    }
    for (std::thread& th : pool) { th.join(); }
    CHECK(ran.load() == 16000 && notRan.load() == 0);
    CHECK(beats::callsInFlight() == 0);
    const fault::Totals t = fault::totals();
    CHECK(t.faults == 0 && t.quarantines == 0 && t.overruns == 0 && t.wedges == 0);
    CHECK(!fault::isQuarantined(s));
}

// ---- H ----------------------------------------------------------------------------------------------------------------------------
void testAttribution() {
    fault::resetForTest();
    // a plugin module "mapped" at a made-up range (a test owns no real module here: the stack rule is arithmetic on addresses)
    fault::setIdentity("range-plugin", "4.5.6", fault::Origin::Official, reinterpret_cast<const void*>(0x10000000), 0x4000);
    fault::setIdentity("hidden-one", "0.0.1", fault::Origin::Site, reinterpret_cast<const void*>(0x20000000), 0x1000);
    const std::uintptr_t stack[] = {0x7FF000001000, 0x10000100, 0x7FF000002000};
    fault::Attribution a;
    CHECK(fault::attributeCrash(stack, 3, 1, a));
    CHECK(a.found && std::string(a.id) == "range-plugin" && std::string(a.version) == "4.5.6" && std::string(a.origin) == "official" &&
          std::string(a.how) == "stack");
    // a site plugin is named by its hash, never by its id
    const std::uintptr_t stack2[] = {0x20000040};
    fault::Attribution b;
    CHECK(fault::attributeCrash(stack2, 1, 1, b));
    CHECK(std::string(b.id).rfind("site-", 0) == 0 && std::string(b.origin) == "site");
    CHECK(std::string(b.id).find("hidden") == std::string::npos);
    // the range's end is exclusive; just outside is nobody's
    const std::uintptr_t stack3[] = {0x10004000, 0x0FFFFFFF};
    fault::Attribution c;
    CHECK(!fault::attributeCrash(stack3, 2, 1, c));
    CHECK(!c.found);
    // an unmapped module is attributed no more
    fault::clearModule("range-plugin");
    CHECK(!fault::attributeCrash(stack, 3, 1, c));
    // in flight: a call declared on THIS thread attributes a crash on this thread, and only this thread (the monitor keys threads by the operating
    // system's thread id: Windows' and Linux' are read here; elsewhere the registry has none and the block does not run)
#if defined(_WIN32) || defined(__linux__)
    fault::Slot* s = fault::slotFor("in-play");
    fault::setIdentity("in-play", "7.7.7", fault::Origin::Official, nullptr, 0);
    unsigned long tid = 0;
#if defined(_WIN32)
    tid = ::GetCurrentThreadId();
#elif defined(__linux__)
    tid = static_cast<unsigned long>(::syscall(SYS_gettid));
#endif
    fault::Attribution d;
    const std::uintptr_t none[] = {0x1};
    const fault::Outcome o = fault::call(s, fault::Entry::PollRows, [&] {
        CHECK(fault::attributeCrash(none, 1, tid, d));
        fault::Attribution other;
        CHECK(!fault::attributeCrash(none, 1, tid + 12345, other));
    });
    CHECK(o == fault::Outcome::Ran);
    CHECK(d.found && std::string(d.id) == "in-play" && std::string(d.how) == "in-flight");
    CHECK(!fault::attributeCrash(none, 1, tid, d));   // the call has ended
#endif
}

// ---- I ----------------------------------------------------------------------------------------------------------------------------
void testGuardDeclinesHostFaults() {
#if defined(_WIN32)
    // The predicate the guard's filter asks: a fault at an address in THIS program's own image is declined (it is a host bug and must
    // die with a report); so is an address in no module. An absorbable code at an address in a foreign image is taken.
    const void* ours = reinterpret_cast<const void*>(&testGuardDeclinesHostFaults);
    CHECK(!cascade::source::vendorGuardAbsorbs(0xC0000005u, ours));
    CHECK(!cascade::source::vendorGuardAbsorbs(0xC0000005u, nullptr));
    CHECK(!cascade::source::vendorGuardAbsorbs(0xC00000FDu, ours));    // a stack overflow is never absorbed
    HMODULE k32 = ::GetModuleHandleA("kernel32.dll");
    CHECK(cascade::source::vendorGuardAbsorbs(0xC0000005u, reinterpret_cast<const void*>(::GetProcAddress(k32, "Sleep"))));
#endif
}

void testSeamsAreTheTestsOnly() {
    // overrideBudgetForTest / dropBudgetOverridesForTest / resetForTest are reachable from no product code: nothing under src/ calls
    // them but their own file.
    std::error_code ec;
    for (const auto& e : fs::recursive_directory_iterator(fs::path(CASCADE_SOURCE_DIR) / "src", ec)) {
        if (!e.is_regular_file()) { continue; }
        const std::string name = e.path().filename().string();
        if (name == "plugin_fault.cpp" || name == "plugin_fault.hpp") { continue; }
        const std::string text = readFile(e.path());
        for (const char* seam : {"fault::overrideBudgetForTest(", "fault::dropBudgetOverridesForTest(", "fault::resetForTest("}) {
            if (text.find(seam) != std::string::npos) { std::printf("FAIL %s calls %s\n", name.c_str(), seam); }
            CHECK(text.find(seam) == std::string::npos);
        }
    }
}

// WHO MAY SWITCH A PLUGIN OFF, AND WHO MAY SWITCH IT ON (rule 3 of plugin_fault.hpp; "a REMOTE session can neither quarantine nor re-enable"):
// quarantining is the guard's alone - nothing under src/ outside core/plugin_fault.cpp calls fault::quarantine() - and the only caller of
// fault::release() is Engine::reenablePlugin, which is reached by one op (FOXAPP_OP_PLUGIN_REENABLE) that the API table refuses every remote
// session (tests/test_engine_api_plugin_fault.cpp holds the four locks on it).
void testWhoMaySwitchWhat() {
    std::size_t quarantineCallers = 0;
    std::size_t releaseCallers = 0;
    std::error_code ec;
    for (const auto& e : fs::recursive_directory_iterator(fs::path(CASCADE_SOURCE_DIR) / "src", ec)) {
        if (!e.is_regular_file()) { continue; }
        const std::string name = e.path().filename().string();
        if (name == "plugin_fault.cpp" || name == "plugin_fault.hpp") { continue; }
        const std::string text = readFile(e.path());
        if (text.find("fault::quarantine(") != std::string::npos) {
            ++quarantineCallers;
            std::printf("FAIL %s calls fault::quarantine(): only the guard may switch a plugin off\n", name.c_str());
        }
        if (text.find("fault::release(") != std::string::npos) {
            ++releaseCallers;
            if (name != "engine_plugin_fault.cpp") { std::printf("FAIL %s calls fault::release()\n", name.c_str()); }
            CHECK(name == "engine_plugin_fault.cpp");
        }
    }
    CHECK(quarantineCallers == 0u);
    CHECK(releaseCallers == 1u);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: test_plugin_fault <fault probe module>\n");
        return 2;
    }
    g_probePath = argv[1];
    g_root = fs::temp_directory_path() / ("foxsdr-test-plugin-fault-" + std::to_string(static_cast<unsigned long long>(
#if defined(_WIN32)
                                                                                           ::GetCurrentProcessId()
#else
                                                                                           1
#endif
                                                                                               )));
    std::error_code ec;
    fs::remove_all(g_root, ec);
    fs::create_directories(g_root / "plugins");
    for (const char* id : kCopies) {
        fs::copy_file(g_probePath, g_root / "plugins" / copyFile(id), fs::copy_options::overwrite_existing, ec);
        CHECK(!ec);
    }

    testTable();
    testIdentity();

    Fixture fx;
    fx.dir = g_root / "plugins";
    fx.host.scan(fx.dir.string());
    CHECK(fx.host.loadedCount() == sizeof(kCopies) / sizeof(kCopies[0]));
    if (fx.host.loadedCount() != sizeof(kCopies) / sizeof(kCopies[0])) {
        for (const LoadedPlugin& p : fx.host.plugins()) { std::printf("  %s: %s%s\n", p.path.c_str(), p.loaded ? "loaded" : "NOT LOADED: ", p.error.c_str()); }
    }
    for (const LoadedPlugin& p : fx.host.plugins()) {
        if (!p.loaded) { std::printf("not loaded: %s: %s\n", p.path.c_str(), p.error.c_str()); }
    }

    testAbsorbedFaults(fx);
    testExceptionAcrossTheBoundary(fx);
    testBudgetNamedByTheMonitor(fx);
    testLateReturnCountedByTheCallItself(fx);
    testConcurrency(fx);
    testAttribution();
    testGuardDeclinesHostFaults();
    testSeamsAreTheTestsOnly();
    testWhoMaySwitchWhat();

    fx.host.unloadAll();
    fs::remove_all(g_root, ec);
    return testSummary("test_plugin_fault");
}
