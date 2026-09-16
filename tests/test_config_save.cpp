// Tests for gui/config_writer.hpp (ConfigWriter) and the two halves of
// core::ConfigStore::save() it composes (serialize()/writeFile()).
//
// THE FIELD REPORT THIS EXISTS FOR. "hang ntdll.dll @
// cascade::core::ConfigStore::save" (0.96.3, Windows 10.0.26200). The log
// tail shows a mode change, two "gui thread recovered after a stall"
// warnings and a plugin rescan just before the report, and the symbolised
// GUI-thread stack is
//
//   AppWindow::run -> AppWindow::maybeSaveConfig -> ConfigStore::save
//     -> std::ofstream write/flush -> ucrtbase's buffered file I/O -> ntdll
//
// i.e. the PERIODIC debounced save, called once a frame, blocked the GUI
// thread inside the file write itself - a slow or cloud-synced %APPDATA%, an
// antivirus holding the file, or a locked target, all of them things the
// application has to survive.
//
// WHAT IS TESTED, AND AT WHICH LEVEL. The blocking write itself is the
// filesystem's and no test can stage it directly, so gui::ConfigWriter takes
// the writer as a parameter (exactly as gui::AudioOpen takes its opener),
// and a writer that sleeps IS a slow disk as far as the frame loop can tell.
// Block 1 below drives a REAL HangWatchdog exactly as the frame loop does -
// heartbeat, request a save that takes 2.5 s, keep beating - and is the red
// one: against the synchronous call it replaced
// (`ConfigStore::save(path, cfg, err)` straight from maybeSaveConfig, which
// is what requestAsync() calling the writer inline on the caller's thread,
// rather than through std::async, amounts to) the loop stops beating and the
// watchdog writes a report. Proved red by hand against that inline call
// during this file's own development - see the commit message for the
// count - before std::async replaced it. The rest of this file holds the
// properties the fix must not lose while achieving it, ending with the
// atomicity and the real ConfigStore::serialize()/writeFile() pair, which no
// mock can stand in for.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "gui/config_writer.hpp"

#include "core/config.hpp"
#include "core/hang_watchdog.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include "test_check.hpp"

namespace fs = std::filesystem;
using cascade::core::ConfigStore;
using cascade::core::HangWatchdog;
using cascade::gui::ConfigWriter;

namespace {

fs::path scratchDir(const std::string& tag) {
    const char* tmp = std::getenv("TEMP");
    const fs::path base = (tmp != nullptr && *tmp != '\0') ? fs::path(tmp) : fs::path(".");
#if defined(_WIN32)
    const unsigned long pid = ::GetCurrentProcessId();
#else
    const unsigned long pid = 0;
#endif
    return base / (std::string("cascade-config-save-") + tag + "-" + std::to_string(pid));
}

double nowMs() {
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

std::string readAll(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

// A writer that behaves like a slow disk: it takes `blockMs` to answer,
// records every (path, text) it was given, and refuses to be running twice
// at once - which is the property that makes a second concurrent write
// detectable rather than merely unlikely (ConfigWriter must never start one
// while another is in flight; that is the whole reason requests coalesce).
struct FakeDisk {
    std::atomic<int> blockMs{0};
    std::atomic<bool> answer{true};
    std::atomic<int> calls{0};
    std::atomic<int> concurrent{0};
    std::atomic<int> maxConcurrent{0};
    std::mutex textsMutex;
    std::vector<std::string> texts;  // every text this was asked to write, in order

    ConfigWriter::Writer writer() {
        return [this](const std::string& path, const std::string& text, std::string& error) {
            (void)path;
            const int live = concurrent.fetch_add(1) + 1;
            int seen = maxConcurrent.load();
            while (live > seen && !maxConcurrent.compare_exchange_weak(seen, live)) {}
            {
                std::lock_guard<std::mutex> lock(textsMutex);
                texts.push_back(text);
            }
            calls.fetch_add(1);
            std::this_thread::sleep_for(std::chrono::milliseconds(blockMs.load()));
            concurrent.fetch_sub(1);
            if (!answer.load()) { error = "fake disk refused"; }
            return answer.load();
        };
    }
};

// --- 1. THE FIELD HANG ------------------------------------------------------
//
// A frame loop, a real watchdog, and a save that takes 2.5 s against an
// 800 ms threshold. The loop must keep beating and the watchdog must write
// nothing - requestAsync() must not so much as pause the watchdog, unlike
// AudioOpen::request(): there is no user waiting on a config save the way
// there is on a device open, so the periodic caller must never cost the
// frame loop anything, bounded or not.
void checkBlockingSaveDoesNotStallTheFrameLoop() {
    const fs::path dir = scratchDir("hang");
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);

    FakeDisk disk;
    disk.blockMs.store(2500);

    HangWatchdog w;
    w.setSuppressionForTest(HangWatchdog::SuppressionForTest::NeverSuppress);
    w.start(dir.string(), 800);
    CHECK(w.running());

    ConfigWriter writer;
    writer.bind(disk.writer());

    // Healthy frames first, so a report afterwards cannot be blamed on the
    // loop never having started.
    for (int i = 0; i < 100; ++i) {
        w.heartbeat();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(w.reportsWritten() == 0u);

    // THE SAVE. The frame that requests it must not wait for the answer at
    // all - not even bounded, unlike a device open.
    const double t0 = nowMs();
    writer.requestAsync((dir / "config.json").string(), "{\"volume\":0.5}");
    const double requestMs = nowMs() - t0;
    CHECK(writer.inFlight());
    CHECK(requestMs < 50.0);  // truly non-blocking, not merely bounded

    // Frames keep turning while the disk thinks, and the longest gap between
    // two of them stays a frame rather than a stall.
    double worstGap = 0.0;
    double last = nowMs();
    int frames = 0;
    bool collected = false;
    const double until = nowMs() + 2900.0;
    while (nowMs() < until) {
        w.heartbeat();
        if (writer.poll()) { collected = true; }
        const double t = nowMs();
        if (t - last > worstGap) { worstGap = t - last; }
        last = t;
        ++frames;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // THE CHECK THE FIELD REPORT IS. A synchronous save of the same content
    // writes one hang report here; this must write none.
    CHECK(w.reportsWritten() == 0u);
    CHECK(frames > 150);
    CHECK(worstGap < 800.0);

    // And the write did land, on a later frame, without anyone blocking for
    // it - a fix that merely dropped the save would pass everything above.
    CHECK(collected);
    CHECK(!writer.inFlight());
    CHECK(writer.completed() == 1u);
    CHECK(writer.lastOk());
    CHECK(disk.calls.load() == 1);

    w.stop();
    if (g_checksFailed == 0) { fs::remove_all(dir, ec); }
}

// --- 2. THE COMMON CASE IS UNCHANGED ----------------------------------------
//
// A write that lands in milliseconds is collected on a frame shortly after,
// with no change to what ends up on disk or to lastOk()/lastError().
void checkFastSaveIsCollectedPromptly() {
    FakeDisk disk;
    disk.blockMs.store(0);

    ConfigWriter writer;
    writer.bind(disk.writer());
    writer.requestAsync("path.json", "{\"a\":1}");

    const double until = nowMs() + 2000.0;
    while (nowMs() < until && writer.inFlight()) {
        writer.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(!writer.inFlight());
    CHECK(writer.completed() == 1u);
    CHECK(writer.lastOk());
    CHECK(disk.calls.load() == 1);

    // A refusal is a result, not a retry: ConfigWriter never re-issues a
    // failed write on its own - the CALLER decides whether to (AppWindow
    // does, via the debounce restarting - see app_window.cpp).
    disk.answer.store(false);
    writer.requestAsync("path.json", "{\"a\":2}");
    while (nowMs() < until && writer.inFlight()) {
        writer.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(writer.completed() == 2u);
    CHECK(!writer.lastOk());
    CHECK(writer.lastError() == "fake disk refused");
}

// --- 3. A BURST COALESCES TO THE LAST CONTENT -------------------------------
//
// A dragged slider, or a burst of debounced saves that all land while the
// disk is still busy with the first one, must not queue N writes: only the
// LAST content asked for reaches the disk, and the ones superseded in
// between are never written at all - the same "last click wins" AudioOpen's
// queued device opens give a burst of clicks.
void checkBurstCoalescesToLastContent() {
    FakeDisk disk;
    disk.blockMs.store(500);

    ConfigWriter writer;
    writer.bind(disk.writer());

    writer.requestAsync("path.json", "{\"n\":1}");  // starts immediately
    CHECK(writer.inFlight());
    // Wait for the worker to have actually STARTED (not merely been handed
    // to std::async - thread creation races the calling thread's very next
    // statement, the same scheduling note test_audio_open.cpp's
    // checkPauseOrderIsDeterministic makes) before asserting none of the
    // coalesced ones has run: 500 ms of blockMs leaves ample room for the
    // three requests below once this returns.
    while (disk.calls.load() == 0) { std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
    // Every one of these arrives while the first write is still running and
    // must coalesce, not queue.
    writer.requestAsync("path.json", "{\"n\":2}");
    writer.requestAsync("path.json", "{\"n\":3}");
    writer.requestAsync("path.json", "{\"n\":4}");
    CHECK(writer.hasQueued());
    CHECK(disk.calls.load() == 1);  // none of the coalesced ones has run yet

    const double until = nowMs() + 4000.0;
    int collects = 0;
    while (nowMs() < until && collects < 2) {
        if (writer.poll()) { ++collects; }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(collects == 2);  // the first write, then the coalesced one
    CHECK(!writer.inFlight());
    CHECK(writer.completed() == 2u);
    CHECK(disk.calls.load() == 2);  // "n":2 and "n":3 never reached the disk

    std::lock_guard<std::mutex> lock(disk.textsMutex);
    CHECK(disk.texts.size() == 2u);
    CHECK(disk.texts[0] == "{\"n\":1}");
    CHECK(disk.texts[1] == "{\"n\":4}");  // the LAST one asked for, not the middle
}

// --- 4. THE SHUTDOWN DRAIN: A HEALTHY WRITE LANDS WITHIN THE BOUND ----------
void checkShutdownDrainCollectsAHealthyWrite() {
    FakeDisk disk;
    disk.blockMs.store(50);

    ConfigWriter writer;
    writer.bind(disk.writer());
    writer.requestAsync("path.json", "{\"final\":true}");

    const double t0 = nowMs();
    const bool landed = writer.finishOrAbandon(std::chrono::milliseconds(1500));
    const double drainMs = nowMs() - t0;
    CHECK(landed);
    CHECK(drainMs < 500.0);  // nowhere near the 1500 ms bound
    CHECK(!writer.inFlight());
    CHECK(writer.lastOk());
    CHECK(writer.completed() == 1u);
}

// --- 5. THE SHUTDOWN DRAIN ALSO WAITS FOR WHAT WAS COALESCED BEHIND IT ------
//
// The case that matters most for AppWindow::run(): the final-state save and
// the clean-exit marker request in quick succession, so the marker is
// usually still QUEUED when the drain runs. Both must land inside one bound.
void checkShutdownDrainCollectsTheQueuedWriteToo() {
    FakeDisk disk;
    disk.blockMs.store(200);

    ConfigWriter writer;
    writer.bind(disk.writer());
    writer.requestAsync("path.json", "{\"final\":true}");
    writer.requestAsync("path.json", "{\"final\":true,\"cleanExit\":true}");  // coalesced
    CHECK(writer.hasQueued());

    const bool landed = writer.finishOrAbandon(std::chrono::milliseconds(1500));
    CHECK(landed);
    CHECK(!writer.inFlight());
    CHECK(writer.completed() == 2u);
    CHECK(writer.lastOk());
    CHECK(disk.calls.load() == 2);

    std::lock_guard<std::mutex> lock(disk.textsMutex);
    CHECK(disk.texts.back() == "{\"final\":true,\"cleanExit\":true}");
}

// --- 6. THE SHUTDOWN DRAIN ABANDONS PAST ITS BOUND, AND SAYS SO -------------
//
// A save that outlives the budget must be abandoned rather than waited for
// (the whole point: a slow disk must cost a log line, never the exit) - and
// the abandoned worker must not corrupt anything once it does finish, later,
// on its own detached thread.
void checkShutdownDrainAbandonsAWedgedWrite() {
    FakeDisk disk;
    disk.blockMs.store(3000);

    auto sink = std::make_shared<int>(7);  // stands for a resource the real
                                            // writer might capture; must
                                            // survive the abandonment
    std::weak_ptr<int> watch = sink;

    double drainMs = 0.0;
    bool landed = true;
    {
        ConfigWriter writer;
        writer.bind([&disk, sink](const std::string& p, const std::string& t, std::string& e) {
            return disk.writer()(p, t, e);
        });
        sink.reset();
        writer.requestAsync("path.json", "{\"n\":1}");
        CHECK(writer.inFlight());
        CHECK(!watch.expired());

        const double t0 = nowMs();
        landed = writer.finishOrAbandon(std::chrono::milliseconds(300));
        drainMs = nowMs() - t0;
        CHECK(!writer.inFlight());  // abandoned, not merely still running
    }
    CHECK(!landed);
    // Bounded by the 300 ms drain, nowhere near the 3000 ms disk.
    CHECK(drainMs < 1000.0);
    // The worker was still running when the ConfigWriter that started it was
    // destroyed, and the object it captured was NOT destroyed under it.
    CHECK(!watch.expired());

    const double until = nowMs() + 5000.0;
    while (nowMs() < until && !watch.expired()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    CHECK(watch.expired());  // and once it finishes, the last reference goes
}

// --- 7. NEVER TWO WRITES AT ONCE --------------------------------------------
void checkCoalescedWritesAreNeverConcurrent() {
    FakeDisk disk;
    disk.blockMs.store(150);

    ConfigWriter writer;
    writer.bind(disk.writer());
    for (int i = 0; i < 6; ++i) {
        writer.requestAsync("path.json", "{\"n\":" + std::to_string(i) + "}");
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    while (writer.inFlight() || writer.hasQueued()) {
        writer.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(disk.maxConcurrent.load() == 1);
}

// --- 8. AN UNBOUND WRITER REFUSES RATHER THAN CRASHES -----------------------
void checkUnboundWriterRefuses() {
    ConfigWriter writer;
    writer.requestAsync("path.json", "{}");
    const double until = nowMs() + 2000.0;
    while (nowMs() < until && writer.inFlight()) {
        writer.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(writer.completed() == 1u);
    CHECK(!writer.lastOk());
}

// --- 9. ATOMICITY: A READER NEVER SEES A PARTIAL FILE -----------------------
//
// ConfigWriter's worker calls core::ConfigStore::writeFile() - the exact
// atomic temp-file-then-rename save() has always used (core/config.hpp) -
// on a thread that is not the GUI thread's. This proves that guarantee holds
// under REAL concurrent writing and reading: one thread hammers writeFile()
// alternating between two large, easily distinguished payloads; another
// opens and reads the target in a tight loop throughout. Every read must be
// either the whole of one payload or the whole of the other - length AND
// content - never a mix, and never anything else (a half-renamed target
// briefly missing is fine and skipped).
void checkAtomicRenameNeverExposesAPartialFile() {
    const fs::path dir = scratchDir("atomic");
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    const std::string path = (dir / "config.json").string();

    // Large enough that a non-atomic write (direct truncate + rewrite, say)
    // would give a reader a real window to catch a half-written file; small
    // JSON configs in test_config.cpp's own atomicity block are too quick to
    // race reliably, which is why this uses a bigger payload instead.
    const std::string payloadA(400000, 'A');
    const std::string payloadB(400000, 'B');

    std::atomic<bool> stop{false};
    std::atomic<int> writes{0};
    std::atomic<int> writeFailures{0};

    std::thread writerThread([&] {
        bool useA = true;
        while (!stop.load()) {
            std::string err;
            // A FAILED writeFile() here is not itself a defect: this
            // reader, like ConfigStore::load() in production, opens the
            // target with the CRT's default share flags - which
            // tests/test_config.cpp's own atomicity block documents as NOT
            // including FILE_SHARE_DELETE - so a reader caught mid-open at
            // the exact instant of the rename can transiently block it. The
            // contract writeFile() keeps in that case (documented in
            // core/config.hpp: "the previous target content — if any —
              // intact") is what this test actually needs to prove, together
            // with badReads below - never that every write wins a race
            // against a reader hammering the same path with zero delay,
            // which nothing in the real product does (config.json is read
            // once at startup, before any writer thread exists).
            if (!ConfigStore::writeFile(path, useA ? payloadA : payloadB, err)) {
                writeFailures.fetch_add(1);
            }
            useA = !useA;
            writes.fetch_add(1);
        }
    });

    std::atomic<int> reads{0};
    std::atomic<int> badReads{0};
    std::thread readerThread([&] {
        while (!stop.load()) {
            const std::string text = readAll(path);
            if (text.empty()) { continue; }  // momentarily missing: fine, skip
            ++reads;
            const bool isA = text.size() == payloadA.size() && text == payloadA;
            const bool isB = text.size() == payloadB.size() && text == payloadB;
            if (!isA && !isB) { ++badReads; }
            // A brief gap between reads, so this thread is not simply
            // holding the path open continuously - which is a self-inflicted
            // denial of service nothing in the real product does (config.json
            // has one reader, once, at startup, before any writer thread
            // exists) and would make nearly every rename collide with it
            // rather than exercising the race this test actually wants.
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    stop.store(true);
    writerThread.join();
    readerThread.join();

    std::printf("test_config_save: atomicity race - %d writes (%d failed transiently), "
                "%d reads, %d bad\n",
                writes.load(), writeFailures.load(), reads.load(), badReads.load());
    CHECK(writes.load() > 5);   // the race actually ran many times
    CHECK(reads.load() > 5);    // and the reader actually caught some of them
    // THE PROPERTY THAT MATTERS: never a mix, never anything but a whole
    // payload. This is what "atomic" means here - not that a write can never
    // be transiently refused by a reader holding the path open.
    CHECK(badReads.load() == 0);
    // And writes are not COMPLETELY broken - a sanity floor, not a target:
    // the transient-refusal rate against a reader hammering the same path is
    // high (measured 40-55% here) because of the Windows sharing-mode fact
    // test_config.cpp's own atomicity block documents (the CRT's default
    // ifstream open lacks FILE_SHARE_DELETE, so it can transiently block a
    // rename in flight) and nothing in the real product ever reads
    // config.json while it is being written - see the writerThread comment
    // above. What must never happen is EVERY write failing, which is what a
    // regression that broke the rename outright, rather than merely racing a
    // reader, would look like.
    CHECK(writeFailures.load() < writes.load());

    if (g_checksFailed == 0) { fs::remove_all(dir, ec); }
}

// --- 10. THE REAL SERIALIZE()/WRITEFILE() PAIR, THROUGH A REAL WORKER -------
//
// EVERYTHING ABOVE IS A MOCK, and a mock-only verification of an I/O feature
// is a hypothesis. This drives the actual functions AppWindow binds:
// core::ConfigStore::serialize() to build the text (on the "GUI thread" -
// this one) and core::ConfigStore::writeFile() to write it (on
// ConfigWriter's worker) - and then loads the result back with
// core::ConfigStore::load() to prove the round trip, not just the bytes on
// disk, still works.
void checkRealSerializeAndWriteFileThroughAWorker() {
    const fs::path dir = scratchDir("real");
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    const std::string path = (dir / "config.json").string();

    cascade::core::AppConfig cfg;
    cfg.volume = 0.73f;
    cfg.mode = "NFM";
    cfg.bandwidthHz = 12500.0;
    cfg.sourceKind = "rtlsdr";

    ConfigWriter writer;
    writer.bind(ConfigStore::writeFile);
    writer.requestAsync(path, ConfigStore::serialize(cfg));

    const double until = nowMs() + 2000.0;
    while (nowMs() < until && writer.inFlight()) {
        writer.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(!writer.inFlight());
    CHECK(writer.completed() == 1u);
    CHECK(writer.lastOk());

    cascade::core::AppConfig loaded;
    std::string err;
    CHECK(ConfigStore::load(path, loaded, err));
    CHECK(err.empty());
    CHECK(loaded.volume == 0.73f);
    CHECK(loaded.mode == "NFM");
    CHECK(loaded.bandwidthHz == 12500.0);
    CHECK(loaded.sourceKind == "rtlsdr");

    // And it is BYTE-IDENTICAL to what synchronous save() would have
    // written, because writeFile() is the same function save() calls.
    std::string syncErr;
    const std::string syncPath = (dir / "sync.json").string();
    CHECK(ConfigStore::save(syncPath, cfg, syncErr));
    CHECK(readAll(path) == readAll(syncPath));

    if (g_checksFailed == 0) { fs::remove_all(dir, ec); }
}

}  // namespace

int main() {
    checkBlockingSaveDoesNotStallTheFrameLoop();
    checkFastSaveIsCollectedPromptly();
    checkBurstCoalescesToLastContent();
    checkShutdownDrainCollectsAHealthyWrite();
    checkShutdownDrainCollectsTheQueuedWriteToo();
    checkShutdownDrainAbandonsAWedgedWrite();
    checkCoalescedWritesAreNeverConcurrent();
    checkUnboundWriterRefuses();
    checkAtomicRenameNeverExposesAPartialFile();
    checkRealSerializeAndWriteFileThroughAWorker();
    return testSummary("test_config_save");
}
