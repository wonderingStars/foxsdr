// test_snapshot_stress.cpp - a concurrency stress of core::ReceiverSnapshot,
// the one receiver snapshot every reader answers from (engine stages 2 and 3;
// docs/engine-stage2.md section 5, docs/engine-stage3.md).
//
// Adopted from the stage-2 concurrency re-check (the orchestrator's
// snap_stress.cpp, which passed 150 s runs under TSan and ASan on Linux and in
// a Release build on Windows, and went red on four mutants), and tightened for
// stage 3's lock-free hand-over slot: a publish is now DELIVERED the moment
// publish() returns, so every readFull() started after that must see it.
//
// One writer thread plays the publishing thread (the GUI thread in stage 3a,
// the engine's control thread from 3b): retryInstall() at the top of every
// "frame", publish(), sometimes a readFull() of its own (applyControlRequest
// resolving web bookmark rows), sometimes a long pause with no frame at all
// (the Windows modal move/resize loop). N reader threads call readFull() and
// read() with randomized holds and sleeps.
//
// Checked on every read:
//   torn      - the state, the lists payload and the bookmark ids of one block
//               all carry that block's own generation; read()'s state is
//               self-consistent.
//   lost      - a readFull() that started after publish(g) returned gives a
//               generation >= g (no block waits for the writer's next pass).
//   regressed - a readFull() started after ANY thread observed g gives >= g;
//               per-thread generations never go down (readFull and read()).
//   lifetime  - every payload freed exactly once and none left after the
//               snapshot is destroyed; a block held past the snapshot's
//               destruction stays whole.
//   deadlock  - a watchdog: any thread without progress for 10 s aborts.
//
// Bounded: 10 s by default so it runs in ctest; argv[1] = seconds, argv[2] =
// readers, argv[3] = 0 makes every reader an HTTP-like poller.
//
// Seen red (docs/engine-stage3.md): the writer drops its block instead of
// handing it over; a reader never takes the slot; the writer keeps (leaks)
// the older block the slot hands back; readFull installs without the lock.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/receiver_snapshot.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

using cascade::core::PublishedState;
using cascade::core::ReceiverSnapshot;
using Clock = std::chrono::steady_clock;

namespace {

enum Role { kMain = 0, kWriter = 1, kReader = 2 };
thread_local int tlRole = kMain;
std::atomic<long> gLive{0};
std::atomic<long> gCreated{0};
std::atomic<long> gFreedBy[3];

struct Payload {
    std::uint64_t gen = 0;
    ~Payload() {
        gLive.fetch_sub(1, std::memory_order_relaxed);
        gFreedBy[tlRole].fetch_add(1, std::memory_order_relaxed);
    }
};

std::shared_ptr<const cascade::net::RadioStatus> makeLists(std::uint64_t g) {
    auto p = std::make_shared<Payload>();
    p->gen = g;
    gLive.fetch_add(1, std::memory_order_relaxed);
    gCreated.fetch_add(1, std::memory_order_relaxed);
    // Aliasing constructor: the snapshot only carries the pointer, never
    // dereferences it, so an opaque payload stands in for RadioStatus.
    return std::shared_ptr<const cascade::net::RadioStatus>(
        p, reinterpret_cast<const cascade::net::RadioStatus*>(p.get()));
}
std::uint64_t payloadGen(const std::shared_ptr<const cascade::net::RadioStatus>& l) {
    return reinterpret_cast<const Payload*>(l.get())->gen;
}

std::atomic<std::uint64_t> gDelivered{0};  // newest gen whose publish() has returned
std::atomic<std::uint64_t> gMaxSeen{0};    // newest gen any readFull returned
std::atomic<bool> gStop{false};
std::atomic<long> gFail{0};

void fail(const char* what, unsigned long long a, unsigned long long b) {
    if (gFail.fetch_add(1) < 20) { std::fprintf(stderr, "FAIL %s: %llu vs %llu\n", what, a, b); }
}

void atomicMax(std::atomic<std::uint64_t>& a, std::uint64_t v) {
    std::uint64_t cur = a.load(std::memory_order_relaxed);
    while (cur < v && !a.compare_exchange_weak(cur, v, std::memory_order_acq_rel)) {}
}

void checkFull(const std::shared_ptr<const ReceiverSnapshot::Full>& f) {
    if (!f) {
        fail("null block", 0, 0);
        return;
    }
    const std::uint64_t g = f->generation;
    if (g == 0) { return; }  // the initial block
    if (static_cast<std::uint64_t>(f->state.rx.centreHz) != g) {
        fail("torn state.centre", static_cast<unsigned long long>(f->state.rx.centreHz), g);
    }
    if (f->state.app.iqBytes != g) { fail("torn state.iqBytes", f->state.app.iqBytes, g); }
    if (!f->lists || payloadGen(f->lists) != g) {
        fail("torn lists", f->lists ? payloadGen(f->lists) : 0, g);
    }
    if (f->bookmarkIds.size() != (g % 7u) + 1u) { fail("torn ids size", f->bookmarkIds.size(), g); }
    for (std::uint64_t id : f->bookmarkIds) {
        if (id != g) {
            fail("torn ids", id, g);
            break;
        }
    }
    if (f->state.rx.tuneSeq != g) { fail("tuneSeq != gen", f->state.rx.tuneSeq, g); }
}

struct Stats {
    std::atomic<std::uint64_t> maxNs{0};
    std::atomic<std::uint64_t> over1ms{0};
    std::atomic<std::uint64_t> over10ms{0};
    std::atomic<std::uint64_t> n{0};
    void add(Clock::duration d) {
        const auto ns = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(d).count());
        atomicMax(maxNs, ns);
        n.fetch_add(1, std::memory_order_relaxed);
        if (ns > 1000000u) { over1ms.fetch_add(1, std::memory_order_relaxed); }
        if (ns > 10000000u) { over10ms.fetch_add(1, std::memory_order_relaxed); }
    }
    void print(const char* name) const {
        std::printf("  %-28s n=%llu max=%.3f ms  >1ms=%llu  >10ms=%llu\n", name,
                    static_cast<unsigned long long>(n.load()), static_cast<double>(maxNs.load()) / 1e6,
                    static_cast<unsigned long long>(over1ms.load()),
                    static_cast<unsigned long long>(over10ms.load()));
    }
};
Stats sPublish, sRetry, sWriterReadFull, sReaderReadFull;

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const int seconds = argc > 1 ? std::atoi(argv[1]) : 10;
    const int nReaders = argc > 2 ? std::atoi(argv[2]) : 6;
    std::printf("test_snapshot_stress: %d s, %d readers + 1 writer\n", seconds, nReaders);

    auto snap = std::make_unique<ReceiverSnapshot>();
    std::vector<std::atomic<std::uint64_t>> progress(static_cast<std::size_t>(nReaders) + 1);
    std::atomic<std::uint64_t> finalGen{0};
    std::atomic<std::uint64_t> modalPauses{0}, slotFullAtPause{0};
    std::atomic<long> readerSeqlockMiss{0};

    std::thread writer([&] {
        tlRole = kWriter;
        std::mt19937_64 rng(1);
        std::uint64_t g = 0;
        while (!gStop.load(std::memory_order_relaxed)) {
            // top of frame
            auto t0 = Clock::now();
            (void)snap->retryInstall();
            sRetry.add(Clock::now() - t0);
            // the publishing thread's own readFull (applyControlRequest)
            if (rng() % 4 == 0) {
                const std::uint64_t d = gDelivered.load(std::memory_order_acquire);
                t0 = Clock::now();
                auto f = snap->readFull();
                sWriterReadFull.add(Clock::now() - t0);
                checkFull(f);
                if (f->generation < d) { fail("writer's readFull lost", f->generation, d); }
                if (f->generation > g) { fail("writer's readFull from the future", f->generation, g); }
            }
            // publish
            ++g;
            PublishedState ps{};
            ps.rx.centreHz = static_cast<double>(g);  // tuneSeq advances every publish
            ps.app.iqBytes = g;
            std::vector<std::uint64_t> ids((g % 7u) + 1u, g);
            t0 = Clock::now();
            snap->publish(ps, makeLists(g), std::move(ids));
            sPublish.add(Clock::now() - t0);
            // DELIVERED ON RETURN: installed, or in the slot for the next reader.
            gDelivered.store(g, std::memory_order_release);
            progress[0].fetch_add(1, std::memory_order_relaxed);
            // frame pacing / the modal loop
            const auto r = rng() % 1000;
            if (r < 2) {
                modalPauses.fetch_add(1);
                if (snap->installPending()) { slotFullAtPause.fetch_add(1); }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));  // no frames at all
            } else if (r < 100) {
                std::this_thread::sleep_for(std::chrono::microseconds(rng() % 2000));
            } else if (r < 400) {
                std::this_thread::yield();
            }
        }
        int spins = 0;
        while (!snap->retryInstall()) {
            ++spins;
            std::this_thread::yield();
        }
        finalGen.store(g);
        std::printf("  writer: %llu publishes, final retry spins %d\n", static_cast<unsigned long long>(g),
                    spins);
    });

    std::vector<std::thread> readers;
    for (int i = 0; i < nReaders; ++i) {
        readers.emplace_back([&, i] {
            tlRole = kReader;
            std::mt19937_64 rng(100 + static_cast<unsigned>(i));
            std::uint64_t lastFull = 0, lastSeq = 0;
            // Half the readers never sleep; argv[3]=0 makes every reader an
            // HTTP-like poller (a read, then 1-5 ms of other work).
            const bool hammer = (argc > 3 ? std::atoi(argv[3]) != 0 : true) && (i % 2) == 0;
            const bool poller = argc > 3 && std::atoi(argv[3]) == 0;
            while (!gStop.load(std::memory_order_relaxed)) {
                const std::uint64_t d = gDelivered.load(std::memory_order_acquire);
                const std::uint64_t m = gMaxSeen.load(std::memory_order_acquire);
                auto t0 = Clock::now();
                std::shared_ptr<const ReceiverSnapshot::Full> f = snap->readFull();
                sReaderReadFull.add(Clock::now() - t0);
                checkFull(f);
                const std::uint64_t fg = f->generation;
                if (fg < d) { fail("lost: readFull older than delivered", fg, d); }
                if (fg < m) { fail("regressed: readFull older than seen elsewhere", fg, m); }
                if (fg < lastFull) { fail("regressed: per-thread readFull", fg, lastFull); }
                lastFull = fg;
                atomicMax(gMaxSeen, fg);

                PublishedState s;
                if (snap->read(s)) {
                    if (s.app.published) {
                        const std::uint64_t sg = s.app.iqBytes;
                        if (static_cast<std::uint64_t>(s.rx.centreHz) != sg) {
                            fail("torn read()", static_cast<unsigned long long>(s.rx.centreHz), sg);
                        }
                        if (sg < lastSeq) { fail("regressed read()", sg, lastSeq); }
                        if (sg < d) { fail("read() older than delivered", sg, d); }
                        lastSeq = sg;
                    }
                } else {
                    readerSeqlockMiss.fetch_add(1);
                }
                progress[static_cast<std::size_t>(i) + 1].fetch_add(1, std::memory_order_relaxed);
                if (poller) {
                    std::this_thread::sleep_for(std::chrono::microseconds(1000 + rng() % 4000));
                } else if (!hammer) {
                    const auto r = rng() % 100;
                    if (r < 30) {
                        std::this_thread::sleep_for(std::chrono::microseconds(rng() % 500));
                    } else if (r < 60) {
                        std::this_thread::yield();
                    }
                }
                // `f` released here, on this reader: often the LAST owner.
            }
        });
    }

    // Watchdog and clock.
    const auto end = Clock::now() + std::chrono::seconds(seconds);
    std::vector<std::uint64_t> last(progress.size(), 0);
    std::vector<Clock::time_point> lastMove(progress.size(), Clock::now());
    while (Clock::now() < end) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        for (std::size_t k = 0; k < progress.size(); ++k) {
            const auto v = progress[k].load();
            if (v != last[k]) {
                last[k] = v;
                lastMove[k] = Clock::now();
            } else if (Clock::now() - lastMove[k] > std::chrono::seconds(10)) {
                std::fprintf(stderr, "DEADLOCK? thread %zu no progress for 10 s\n", k);
                std::abort();
            }
        }
    }
    gStop.store(true);
    writer.join();
    for (auto& t : readers) { t.join(); }

    // After the writer's last pass: every reader sees the final block.
    auto f = snap->readFull();
    if (f->generation != finalGen.load()) { fail("final block not visible", f->generation, finalGen.load()); }
    std::printf("  final readFull gen %llu == final publish %llu\n",
                static_cast<unsigned long long>(f->generation),
                static_cast<unsigned long long>(finalGen.load()));
    const std::uint64_t throughSlot = snap->deferredInstalls();
    std::printf("  publishes through the hand-over slot %llu\n",
                static_cast<unsigned long long>(throughSlot));
    std::printf("  modal pauses %llu, of which the slot held a block at the pause: %llu (a reader installs it)\n",
                static_cast<unsigned long long>(modalPauses.load()),
                static_cast<unsigned long long>(slotFullAtPause.load()));
    std::printf("  read() gave up (overlapped kMaxTries writes): %ld\n", readerSeqlockMiss.load());

    // A block outlives the snapshot: destroy the snapshot on another thread
    // while this one still holds `f`, then read `f` whole.
    tlRole = kMain;
    std::thread killer([&] {
        tlRole = kReader;
        snap.reset();
    });
    killer.join();
    checkFull(f);
    std::printf("  block held past the snapshot's destruction: gen %llu still whole\n",
                static_cast<unsigned long long>(f->generation));
    f.reset();

    sPublish.print("publish()");
    sRetry.print("retryInstall()");
    sWriterReadFull.print("writer's own readFull()");
    sReaderReadFull.print("reader readFull()");
    std::printf("  payloads created %ld, live after teardown %ld, freed on writer %ld / readers %ld / main %ld\n",
                gCreated.load(), gLive.load(), gFreedBy[kWriter].load(), gFreedBy[kReader].load(),
                gFreedBy[kMain].load());
    if (gLive.load() != 0) { fail("payload leak", static_cast<unsigned long long>(gLive.load()), 0); }
    if (gFreedBy[0].load() + gFreedBy[1].load() + gFreedBy[2].load() != gCreated.load()) {
        fail("payload freed count", 0, 0);
    }
    // The run must have raced something: enough publishes, and the slot used.
    if (finalGen.load() < 1000u) { fail("too few publishes to prove anything", finalGen.load(), 1000u); }
    if (throughSlot == 0u) { fail("the hand-over slot was never used", 0, 0); }
    const long fails = gFail.load();
    std::printf("RESULT: %s (%ld failures)\n", fails == 0 ? "PASS" : "FAIL", fails);
    std::printf("test_snapshot_stress: %s\n", fails == 0 ? "all checks passed" : "FAILED");
    return fails == 0 ? 0 : 1;
}
