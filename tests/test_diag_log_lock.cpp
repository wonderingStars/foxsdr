// A log write that is waiting on a disk does not hold up what reads the log.
//
// WHAT WENT WRONG (found by the 0.99.64 audit of what the GUI thread still waits
// on, docs/DIAGNOSTICS.md, "The window does no disk work", item 13).
// DiagLog::write took ONE mutex, copied the line into the in-memory ring and then,
// STILL HOLDING IT, did the file's fwrite and fflush (and, when the file was full,
// the rotation's close, remove, rename and open). The readers that want the ring
// took the same mutex:
//
//   * ringSnapshot(), which is the log section of a FREEZE report
//     (hang_watchdog.cpp, both the Windows and the Linux capture) and of Copy
//     diagnostics;
//   * fileEnabled() and filePath(), which the Diagnostics panel asks every frame
//     it is open.
//
// So a log write that was stuck on a slow profile - the very disk that froze the
// window - held the freeze report's log section: the report had its thread stacks
// (flushed one by one) and then waited for the disk before it could write the
// lines that say what the application was doing. The crash report was never
// affected: it reads the ring with copyRingRaw(), which takes no lock at all.
//
// WHAT IS PROMISED NOW (option A of the audit's proposal, and only A). The ring
// and the file have a lock each. write() updates the ring under the ring's lock,
// releases it, and only then takes the file's lock for the append. So:
//
//   1. ringSnapshot(), fileEnabled(), filePath() and copyRingRaw() never wait for a
//      file write, and a line parked inside the file I/O is ALREADY in the ring.
//   2. Nothing about what reaches the file changes: every line is still flushed
//      when write() returns, and a caller of write() still waits for the disk, as
//      before.
//   3. THE FILE'S LINE ORDER is the order in which the callers reached the file's
//      lock. For one thread that is the order it logged in. For two threads that
//      race it can differ from the ring's order (the ring took them in the order
//      they reached the ring's lock). What holds either way - and is what is
//      tested below - is that every line is whole, none is lost and none is
//      duplicated, and each thread's own lines are in the order it wrote them.
//
// WHAT IS TESTED, and at which level. NOTHING HERE DEPENDS ON A REAL SLOW DISK: a
// test hook (DiagLog::setFileHookForTest, null in every shipped build) is called
// from inside the file append, and the test parks a write there - which is what a
// write waiting on a disk looks like to every other thread.
//
//   1. THE READERS, directly: ringSnapshot(), fileEnabled(), filePath() and the
//      crash path's copyRingRaw() all complete while a write is parked, and the
//      parked line - and a line another thread logs meanwhile - is in the ring.
//   2. A REAL FREEZE REPORT: a real HangWatchdog captures a stalled thread while a
//      write is parked, and the report it writes has its log section, with the
//      parked line in it, WHILE THE WRITE IS STILL PARKED.
//   3. THE ORDER PROPERTY.
//
// WHAT IS NOT COVERED. Nothing here has met a genuinely slow disk.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iterator>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "core/diag_log.hpp"
#include "core/hang_watchdog.hpp"
#include "test_check.hpp"

namespace fs = std::filesystem;
using cascade::core::DiagLog;
using cascade::core::HangWatchdog;
using cascade::core::diagLogf;

namespace {

fs::path g_scratch;

void isolate() {
#if defined(_WIN32)
    const int pid = _getpid();
#else
    const int pid = static_cast<int>(getpid());
#endif
    g_scratch = fs::temp_directory_path() / ("foxsdr_diag_lock_" + std::to_string(pid));
    std::error_code ec;
    fs::remove_all(g_scratch, ec);
    fs::create_directories(g_scratch, ec);
}

// --- the parked write --------------------------------------------------------

std::atomic<bool> g_park{false};
std::atomic<bool> g_gate{false};
std::atomic<int> g_entered{0};

// Called from inside the file append, holding whatever lock the append holds. A
// line that carries the marker waits at the gate: a write that is waiting for a
// disk.
void parkingHook(const char* line, std::size_t len) {
    (void)len;
    if (!g_park.load() || std::strstr(line, "PARK-ME") == nullptr) { return; }
    g_entered.fetch_add(1);
    while (!g_gate.load()) { std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
}

bool waitFor(const std::function<bool()>& cond, int ms) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < until) {
        if (cond()) { return true; }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return cond();
}

// Runs `fn` on its own thread and says whether it finished within `ms`. The thread
// is joined (or left to finish after the gate opens) by the future's destructor, so
// the caller opens the gate BEFORE this goes out of scope.
template <class Fn>
bool completesWithin(std::future<Fn>& f, int ms) {
    return f.wait_for(std::chrono::milliseconds(ms)) == std::future_status::ready;
}

bool contains(const std::vector<std::string>& lines, const char* needle) {
    for (const std::string& l : lines) {
        if (l.find(needle) != std::string::npos) { return true; }
    }
    return false;
}

std::string readAll(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void freshLog(const char* tag) {
    DiagLog& log = DiagLog::instance();
    log.resetForTest();
    log.configure((g_scratch / tag).string(), true);
    CHECK(log.fileEnabled());
}

// --- 1. THE READERS ----------------------------------------------------------

void checkTheReadersAreNotBehindAParkedWrite() {
    DiagLog& log = DiagLog::instance();
    freshLog("readers");
    diagLogf("before the park");
    g_gate.store(false);
    g_entered.store(0);
    g_park.store(true);
    DiagLog::setFileHookForTest(&parkingHook);

    std::thread parked([] { diagLogf("PARK-ME first"); });
    CHECK(waitFor([] { return g_entered.load() == 1; }, 5000));

    // The freeze report's and Copy diagnostics' reader.
    auto snap = std::async(std::launch::async, [] { return DiagLog::instance().ringSnapshot(); });
    const bool snapDone = completesWithin(snap, 1500);
    CHECK(snapDone);

    // What the Diagnostics panel asks every frame it is open.
    auto state = std::async(std::launch::async, [] {
        return DiagLog::instance().fileEnabled() &&
               !DiagLog::instance().filePath().empty();
    });
    const bool stateDone = completesWithin(state, 1500);
    CHECK(stateDone);

    // The crash path's reader takes no lock at all, parked write or not: this is
    // the property the 0.99.64 work leaves exactly as it was.
    char raw[16 * 1024];
    const std::size_t used = log.copyRingRaw(raw, sizeof(raw));
    CHECK(used > 0u);
    CHECK(std::strstr(raw, "PARK-ME first") != nullptr);
    CHECK(std::strstr(raw, "before the park") != nullptr);

    // A second thread logs while the first is parked: its line reaches the ring at
    // once (it then waits for the file, as a caller always has).
    std::atomic<bool> otherReturned{false};
    std::thread other([&] {
        diagLogf("OTHER while parked");
        otherReturned.store(true);
    });
    // Watched from a thread of its own: on code where the snapshot DOES wait for the
    // parked write, this thread is the one that is stuck, not the test.
    std::atomic<bool> otherSeen{false};
    std::atomic<bool> stopWatching{false};
    std::thread watcher([&] {
        while (!stopWatching.load()) {
            if (contains(DiagLog::instance().ringSnapshot(), "OTHER while parked")) {
                otherSeen.store(true);
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    });
    const bool otherInRing = waitFor([&] { return otherSeen.load(); }, 1500);
    CHECK(otherInRing);
    CHECK(!otherReturned.load());  // still waiting for the file: callers wait, as today

    // Open the gate BEFORE any future goes out of scope: a snapshot that WAS stuck
    // (the old code) can then finish and be joined.
    g_gate.store(true);
    stopWatching.store(true);
    parked.join();
    other.join();
    watcher.join();
    if (snapDone) {
        const std::vector<std::string> got = snap.get();
        CHECK(contains(got, "PARK-ME first"));  // the parked line was already in the ring
        CHECK(contains(got, "before the park"));
    } else {
        snap.wait();
    }
    if (!stateDone) { state.wait(); }
    if (stateDone) { CHECK(state.get()); }

    // Nothing about the file changed: both lines reached it, once, whole.
    DiagLog::setFileHookForTest(nullptr);
    g_park.store(false);
    const std::string text = readAll(fs::path(log.filePath()));
    auto count = [&text](const char* s) {
        std::size_t n = 0;
        for (std::size_t at = text.find(s); at != std::string::npos; at = text.find(s, at + 1)) { ++n; }
        return n;
    };
    CHECK(count("PARK-ME first") == 1u);
    CHECK(count("OTHER while parked") == 1u);
    CHECK(count("before the park") == 1u);
}

// --- 2. A REAL FREEZE REPORT ---------------------------------------------------

// The log section of a real hang report, written by a real watchdog while a log
// write is parked in the file I/O. Before the split the report stopped after its
// thread stacks and waited for the disk; now it has the log with the parked line.
void checkAFreezeReportCarriesItsLogWhileAWriteIsParked() {
    freshLog("freeze");
    const fs::path reports = g_scratch / "freeze-reports";
    std::error_code ec;
    fs::create_directories(reports, ec);

    HangWatchdog w;
    w.setSuppressionForTest(HangWatchdog::SuppressionForTest::NeverSuppress);
    w.start(reports.string(), 800);
    CHECK(w.running());
    for (int i = 0; i < 50; ++i) {
        w.heartbeat();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    diagLogf("a line before the freeze");

    g_gate.store(false);
    g_entered.store(0);
    g_park.store(true);
    DiagLog::setFileHookForTest(&parkingHook);
    std::thread parked([] { diagLogf("PARK-ME during the freeze"); });
    CHECK(waitFor([] { return g_entered.load() == 1; }, 5000));

    // This thread is the one that heartbeats, so it is "the GUI thread": it stops
    // beating and the watchdog (800 ms) captures it.
    std::string report;
    const bool gotLog = waitFor(
        [&] {
            for (const auto& e : fs::directory_iterator(reports, ec)) {
                const std::string t = readAll(e.path());
                report = t;  // whatever the report has so far
                if (t.find("--- log (last") != std::string::npos) { return true; }
            }
            return false;
        },
        8000);
    // THE PROPERTY: the report has its log section, with the line that was being
    // written when the application stalled, WHILE THAT WRITE IS STILL PARKED.
    CHECK(gotLog);
    CHECK(report.find("PARK-ME during the freeze") != std::string::npos);
    CHECK(report.find("a line before the freeze") != std::string::npos);
    // ...and the stacks are in it too (it is a freeze report, not a bare log).
    CHECK(report.find("--- thread ") != std::string::npos);
    std::printf("  freeze report with a write parked: stacks %s, log section %s, %zu bytes\n",
                report.find("--- thread ") != std::string::npos ? "present" : "ABSENT",
                gotLog ? "present" : "ABSENT", report.size());

    g_gate.store(true);
    parked.join();
    DiagLog::setFileHookForTest(nullptr);
    g_park.store(false);
    w.stop();
}

// --- 3. THE ORDER PROPERTY -----------------------------------------------------

// What the header promises about the file, and no more: every line whole, none
// lost, none duplicated, and each thread's own lines in the order it wrote them.
// (Not that the file's order across threads is the ring's: it need not be.)
void checkTheFileHoldsEveryLineOnceAndEachThreadsInOrder() {
    DiagLog& log = DiagLog::instance();
    freshLog("order");
    constexpr int kThreads = 6;
    constexpr int kLines = 250;
    std::atomic<int> go{0};
    std::vector<std::thread> ts;
    for (int t = 0; t < kThreads; ++t) {
        ts.emplace_back([t, &go] {
            while (go.load() == 0) { std::this_thread::yield(); }
            for (int i = 0; i < kLines; ++i) { diagLogf("T%d N%d end", t, i); }
        });
    }
    go.store(1);
    for (std::thread& th : ts) { th.join(); }

    const std::string text = readAll(fs::path(log.filePath()));
    std::istringstream is(text);
    std::string line;
    std::map<int, std::vector<int>> seen;
    int total = 0;
    int whole = 0;
    while (std::getline(is, line)) {
        ++total;
        int t = -1;
        int n = -1;
        char tail[8] = {};
        // "HH:MM:SS.mmm info T<t> N<n> end": whole means it parses to the end.
        const char* at = std::strstr(line.c_str(), " info T");
        if (at != nullptr && std::sscanf(at, " info T%d N%d %7s", &t, &n, tail) == 3 &&
            std::strcmp(tail, "end") == 0) {
            ++whole;
            seen[t].push_back(n);
        }
    }
    CHECK(total == kThreads * kLines);
    CHECK(whole == total);  // none torn, none interleaved mid-line
    CHECK(static_cast<int>(seen.size()) == kThreads);
    for (int t = 0; t < kThreads; ++t) {
        const std::vector<int>& v = seen[t];
        CHECK(static_cast<int>(v.size()) == kLines);  // none lost, none duplicated
        bool inOrder = true;
        for (int i = 0; i < static_cast<int>(v.size()); ++i) {
            if (v[static_cast<std::size_t>(i)] != i) { inOrder = false; }
        }
        CHECK(inOrder);  // each thread's own order
    }
    CHECK(log.linesWritten() == static_cast<std::uint64_t>(kThreads * kLines));
    std::printf("  %d threads x %d lines: %d lines in the file, every one whole\n", kThreads, kLines,
                total);
}

}  // namespace

int main() {
    isolate();
    checkTheReadersAreNotBehindAParkedWrite();
    checkAFreezeReportCarriesItsLogWhileAWriteIsParked();
    checkTheFileHoldsEveryLineOnceAndEachThreadsInOrder();
    DiagLog::instance().resetForTest();
    std::error_code ec;
    if (g_checksFailed == 0) { fs::remove_all(g_scratch, ec); }
    return testSummary("test_diag_log_lock");
}
