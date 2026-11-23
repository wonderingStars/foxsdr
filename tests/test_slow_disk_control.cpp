// test_slow_disk_control.cpp - THE ENGINE'S CONTROL PASS MAKES NO FILE-SYSTEM CALL THAT A SLOW DISK COULD HOLD (freeze diagnosis P4).
//
// Three field freezes were the control thread inside a file call on a machine whose disk or scanner answered slowly:
//   B058596B  companion::stateForEntry (companions.json and the install record, read per companion row on every list rebuild)
//   FA0B39E3  issue::fileIssueReport's ofstream (the ring-drop minute)
//   A488498B  radioreport::Reporter::poll's directory listing (queueEmpty)
// Each now has a seam that runs the file call on a worker (StateCache, fileIssueReportOffThread, Reporter's in-memory "maybe queued" bit). The seams of
// each are bound here to a call that takes five seconds - a slow disk as far as the caller can tell, with the real files and the real code behind it -
// and the caller's own time is measured: no more than 50 ms. Each also proves the work still HAPPENS (the state arrives, the file appears, the batch runs)
// and that the hooked call ran on another thread.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "core/companion_state_cache.hpp"
#include "core/crash_handler.hpp"
#include "core/issue_report.hpp"
#include "core/plugin_repo.hpp"
#include "core/radio_report.hpp"
#include "core/radio_reporter.hpp"
#include "test_check.hpp"

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace {

constexpr int kBudgetMs = 50;           // what the caller may spend with the disk five seconds slow
// The injected disk: five seconds. FOXSDR_TEST_SLOW_MS shortens it for the break-it runs only (with the fix removed every pass would pay it).
int slowMs() {
    const char* e = std::getenv("FOXSDR_TEST_SLOW_MS");
    return e != nullptr && *e != '\0' ? std::max(100, std::atoi(e)) : 5000;
}
std::atomic<bool> g_release{false};     // lets the test end the "slow disk" early once it has measured

// A five-second file call that the test can cut short.
void slowDisk() {
    for (int i = 0; i < slowMs() / 5 && !g_release.load(); ++i) { std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
}

double msSince(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }

template <class Pred>
bool waitFor(Pred p, int ms) {
    const auto until = Clock::now() + std::chrono::milliseconds(ms);
    while (Clock::now() < until) {
        if (p()) { return true; }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return p();
}

fs::path scratch(const char* tag) {
    const fs::path d = fs::temp_directory_path() / (std::string("foxsdr-slowdisk-") + tag + "-" + std::to_string(Clock::now().time_since_epoch().count()));
    std::error_code ec;
    fs::create_directories(d, ec);
    return d;
}

// ---------------------------------------------------------------- B058596B: the companion shelf's states
void testCompanionStates() {
    std::printf("companion states: a list rebuild with 16 companion rows costs the pass no file call while the disk takes five seconds\n");
    using namespace cascade::core;
    std::vector<PluginCatalogEntry> catalog;
    for (int i = 0; i < 16; ++i) {
        PluginCatalogEntry e;
        e.id = "companion-row-" + std::to_string(i);
        e.companion.present = true;
        catalog.push_back(e);
    }
    const fs::path plugins = scratch("companion");

    g_release = false;
    std::mutex mu;
    std::set<std::thread::id> hookThreads;
    companion::StateCache::setDiskHookForTest([&] {
        { std::lock_guard<std::mutex> lk(mu); hookThreads.insert(std::this_thread::get_id()); }
        slowDisk();
    });

    companion::StateCache cache;
    const std::thread::id passThread = std::this_thread::get_id();
    double worst = 0.0;
    // The "pass": 40 ticks and 16 lookups each, as a rebuild does, while the worker sits in the slow call.
    const auto first = Clock::now();
    for (int pass = 0; pass < 40; ++pass) {
        const auto t = Clock::now();
        cache.tick(catalog, plugins.string());
        companion::State s;
        for (const PluginCatalogEntry& e : catalog) { (void)cache.lookup(e.id, s); }
        worst = std::max(worst, msSince(t));
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    std::printf("  worst pass %.2f ms over %.0f ms\n", worst, msSince(first));
    CHECK(worst < kBudgetMs);
    CHECK(cache.refreshing());                       // the worker is out, in the slow call
    companion::State probe;
    CHECK(!cache.lookup(catalog[0].id, probe));      // nothing computed yet: a miss, not a wait

    g_release = true;                                // the disk answers
    const bool landed = waitFor([&] { cache.tick(catalog, plugins.string()); return cache.generation() != 0; }, 3000);
    CHECK(landed);
    CHECK(cache.lookup(catalog[0].id, probe));
    CHECK(cache.lookup(catalog[15].id, probe));
    {
        std::lock_guard<std::mutex> lk(mu);
        CHECK(!hookThreads.empty());
        CHECK(hookThreads.count(passThread) == 0);   // the file call ran on the worker, never on the pass
    }
    companion::StateCache::setDiskHookForTest(nullptr);
    std::error_code ec;
    fs::remove_all(plugins, ec);
}

// ---------------------------------------------------------------- FA0B39E3: the issue report
void testIssueReport() {
    std::printf("issue report: filing it on the pass costs no file call while the disk takes five seconds\n");
    using namespace cascade::core;
    const fs::path dir = scratch("issue");
    CrashHandlerConfig on;
    on.crashDir = dir.string();
    on.enabled = true;
    on.exitAfterReport = false;
    installCrashHandlers(on);
    CHECK(!activeCrashDir().empty());
    issue::resetIssueReportsForTest();
    (void)issue::takeNewlyFiled();

    g_release = false;
    std::mutex mu;
    std::set<std::thread::id> hookThreads;
    issue::setDiskHookForTest([&] {
        { std::lock_guard<std::mutex> lk(mu); hookThreads.insert(std::this_thread::get_id()); }
        slowDisk();
    });
    const auto t = Clock::now();
    const std::string path = issue::fileIssueReportOffThread(health::Recovered::RingDrop);
    const double took = msSince(t);
    std::printf("  returned in %.2f ms\n", took);
    CHECK(!path.empty());
    CHECK(took < kBudgetMs);
    CHECK(!fs::exists(path));                                // nothing on disk yet: the worker is in the slow call
    CHECK(issue::takeNewlyFiled() == 0);
    // ...and the once-a-session bit is already taken: the same word, met again on the pass, files nothing and costs nothing.
    const auto t2 = Clock::now();
    CHECK(issue::fileIssueReportOffThread(health::Recovered::RingDrop).empty());
    CHECK(msSince(t2) < kBudgetMs);

    g_release = true;
    CHECK(issue::waitForWritesForTest(3000));
    CHECK(fs::exists(path));                                 // the report appears afterwards
    CHECK(issue::takeNewlyFiled() == 1);
    {
        std::ifstream in(path, std::ios::binary);
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        CHECK(text.find("kind: ") != std::string::npos);
        CHECK(text.find("--- log") != std::string::npos);
    }
    {
        std::lock_guard<std::mutex> lk(mu);
        CHECK(hookThreads.size() == 1);
        CHECK(hookThreads.count(std::this_thread::get_id()) == 0);
    }

    // A write that FAILS (the folder is gone) gives the once-a-session bit back, as the synchronous call does.
    issue::resetIssueReportsForTest();
    issue::setDiskHookForTest(nullptr);
    std::error_code ec;
    fs::remove_all(dir, ec);
    const std::string lost = issue::fileIssueReportOffThread(health::Recovered::Audio);
    CHECK(!lost.empty());
    CHECK(issue::waitForWritesForTest(3000));
    CHECK(issue::takeNewlyFiled() == 0);
    fs::create_directories(dir, ec);
    CHECK(!issue::fileIssueReportOffThread(health::Recovered::Audio).empty());   // the word was given back, so it files again
    CHECK(issue::waitForWritesForTest(3000));
    CHECK(issue::takeNewlyFiled() == 1);
    issue::setDiskHookForTest(nullptr);
    fs::remove_all(dir, ec);
}

// ---------------------------------------------------------------- A488498B: the radio reporter's poll
void testReporterPoll() {
    std::printf("radio reporter: poll() lists no folder on the calling thread while the disk takes five seconds\n");
    using namespace cascade::core;
    using namespace cascade::core::radioreport;
    const fs::path tmp = scratch("reporter");
    std::atomic<std::int64_t> clock{1794830400};
    std::atomic<int> posts{0};
    Reporter rep;
    Reporter::Options o;
    o.dir = (tmp / "radio-reports").string();
    o.endpoint = "http://127.0.0.1:9/";
    o.nowEpoch = [&] { return clock.load(); };
    o.transport = [&](const std::string&, const std::string&) { ++posts; return PostResult{true, 204, 0, ""}; };
    o.liveSend = true;
    o.identity.installId = "a1b2c3d4e5f60718293a4b5c6d7e8f90";
    o.identity.version = "0.99.112";
    o.identity.os = "Windows 10.0.22631";
    o.identity.arch = "x64";
    rep.configure(o);

    g_release = false;
    std::mutex mu;
    std::set<std::thread::id> hookThreads;
    Reporter::setDiskHookForTest([&] {
        { std::lock_guard<std::mutex> lk(mu); hookThreads.insert(std::this_thread::get_id()); }
        slowDisk();
    });

    // Something to send, queued BEFORE the hook is slow in the sense that matters: the event's own durable append is the journal's business (a later
    // change), so it is queued first with the hook on only for listings - admit() lists the folder every 64th append and for failures; one event does not.
    Event e;
    e.kind = "radio_open";
    e.radio = identify("rtlsdr", "serial=00000001", "Generic RTL2832U");
    e.word = "start";
    e.set("phase", "start").set("trigger", "user-pick");
    e.urgent = true;                                   // due at once
    rep.event(e);

    const std::thread::id passThread = std::this_thread::get_id();
    double worst = 0.0;
    for (int pass = 0; pass < 60; ++pass) {
        const auto t = Clock::now();
        rep.poll();
        worst = std::max(worst, msSince(t));
        std::this_thread::sleep_for(std::chrono::milliseconds(3));
        clock += 1;
    }
    std::printf("  worst poll %.2f ms\n", worst);
    CHECK(worst < kBudgetMs);
    {
        std::lock_guard<std::mutex> lk(mu);
        CHECK(hookThreads.count(passThread) == 0);     // no listing ran on the pass; the batch thread's own is in the slow call
    }

    g_release = true;
    // The batch thread finishes its listing and sends: the work still happens.
    const bool sent = waitFor([&] { rep.poll(); return posts.load() >= 1; }, 5000);
    CHECK(sent);
    Reporter::setDiskHookForTest(nullptr);
    rep.shutdown();
    std::error_code ec;
    fs::remove_all(tmp, ec);
}

// ---------------------------------------------------------------- the engine uses them
std::string codeOf(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::string raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::string out;
    std::size_t i = 0;
    while (i < raw.size()) {
        const std::size_t eol = raw.find('\n', i);
        std::string line = raw.substr(i, eol == std::string::npos ? std::string::npos : eol - i);
        const std::size_t c = line.find("//");
        if (c != std::string::npos) { line.erase(c); }
        out += line + "\n";
        if (eol == std::string::npos) { break; }
        i = eol + 1;
    }
    return out;
}

void testEngineUsesTheSeams() {
    std::printf("the engine's pass code calls the seams, not the file-reading functions\n");
#ifdef CASCADE_SOURCE_DIR
    const fs::path src = fs::path(CASCADE_SOURCE_DIR) / "src";
#else
    const fs::path src = fs::current_path() / "src";
#endif
    const std::string lists = codeOf(src / "engine" / "engine_api_lists.cpp");
    const std::string engine = codeOf(src / "engine" / "engine.cpp");
    CHECK(!lists.empty() && !engine.empty());
    CHECK(lists.find("stateForEntry") == std::string::npos);
    CHECK(engine.find("stateForEntry") == std::string::npos);
    CHECK(lists.find("companionStates_.lookup(") != std::string::npos);
    CHECK(lists.find("companionStates_.tick(") != std::string::npos);
    CHECK(engine.find("fileIssueReportOffThread(cascade::core::health::Recovered::RingDrop)") != std::string::npos);
    CHECK(engine.find("issue::fileIssueReport(") == std::string::npos);
    const std::string health = codeOf(src / "core" / "health_events.cpp");
    CHECK(health.find("issue::fileIssueReport(") == std::string::npos);   // every health word files off the thread that met it
    CHECK(health.find("issue::fileIssueReportOffThread(") != std::string::npos);
}

}  // namespace

int main() {
    std::printf("test_slow_disk_control\n");
    testCompanionStates();
    testIssueReport();
    testReporterPoll();
    testEngineUsesTheSeams();
    return testSummary("test_slow_disk_control");
}
