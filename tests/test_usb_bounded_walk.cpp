// THE DEADLINE BEHIND THE SYSFS LISTING (src/usb/bounded_walk.hpp), proven on
// every platform. The Linux half - a real blocked open() under a real fixture
// tree - is tests/test_usb_usbfs.cpp section 7; this file proves the logic
// that makes it work with nothing but a work function that will not return,
// so Windows CI exercises it too.
//
// The field failure (hang signature 40ABD0BB073D899C): a per-device file read
// with no timeout, run inline on the GUI thread, blocked for more than five
// seconds. What has to be true instead: the caller gets its answer inside the
// budget whatever one item does, the stuck item is named and left out, the
// items around it still come back in order, and nothing waits on it twice.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "test_check.hpp"
#include "usb/bounded_walk.hpp"

using cascade::usb::boundedWalk;
using cascade::usb::BoundedWalkResult;
using cascade::usb::WalkBudget;
using cascade::usb::WedgeRegistry;

namespace {

using Clock = std::chrono::steady_clock;
using std::chrono::milliseconds;

double msSince(Clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

// What the work functions share. Workers are abandoned by design and outlive
// the call that started them, so everything they touch is here, behind one
// object that main() keeps alive until every one of them has come back.
struct Shared {
    std::mutex m;
    std::condition_variable cv;
    bool release = false;                // lets the stuck item return
    std::atomic<int> inWork{0};          // workers currently inside work()
    std::atomic<int> stuckCalls{0};      // times the stuck key was entered
};

Shared g;

// Blocks until g.release, like a read that returns when the device recovers.
std::string work(const std::string& key) {
    ++g.inWork;
    struct Leave {
        ~Leave() { --g.inWork; }
    } leave;
    if (key == "stuck") {
        ++g.stuckCalls;
        std::unique_lock<std::mutex> lk(g.m);
        g.cv.wait(lk, [] { return g.release; });
    }
    if (key == "throws") { throw std::runtime_error("boom"); }
    return "v:" + key;
}

std::string slowWork(const std::string& key) {
    ++g.inWork;
    struct Leave {
        ~Leave() { --g.inWork; }
    } leave;
    std::this_thread::sleep_for(milliseconds(60));
    return "v:" + key;
}

std::vector<std::string> keys(std::initializer_list<const char*> k) {
    return std::vector<std::string>(k.begin(), k.end());
}

}  // namespace

int main() {
    WalkBudget budget;
    budget.stall = milliseconds(120);
    budget.total = milliseconds(2000);

    // --- 1. A healthy walk is the inline loop: all items, in order --------
    {
        const auto reg = std::make_shared<WedgeRegistry>();
        const BoundedWalkResult<std::string> r =
            boundedWalk<std::string>(keys({"a", "b", "c", "d"}), work, reg, budget);
        CHECK(r.complete);
        CHECK(r.wedged.empty());
        CHECK(r.skipped == 0u);
        CHECK(r.results == std::vector<std::string>({"v:a", "v:b", "v:c", "v:d"}));
        CHECK(reg->size() == 0u);

        // Nothing to walk is a walk that finished.
        const BoundedWalkResult<std::string> none =
            boundedWalk<std::string>(std::vector<std::string>(), work, reg, budget);
        CHECK(none.complete && none.results.empty());
    }

    // --- 2. ONE ITEM THAT NEVER RETURNS cannot hold the caller -----------
    //
    // The item is in the middle of the list, so the items after it prove the
    // walk went on past it instead of stopping where it stuck.
    const auto reg = std::make_shared<WedgeRegistry>();
    {
        const auto t0 = Clock::now();
        const BoundedWalkResult<std::string> r =
            boundedWalk<std::string>(keys({"a", "stuck", "c", "d"}), work, reg, budget);
        const double ms = msSince(t0);
        std::printf("walk with one item that never returns: %zu results after %.1f ms\n",
                    r.results.size(), ms);
        // One stall plus scheduling, nowhere near the time the item would take.
        CHECK(ms < 1500.0);
        CHECK(ms >= 100.0);  // it DID wait the stall out before giving up
        CHECK(!r.complete);
        CHECK(r.wedged == std::vector<std::string>({"stuck"}));
        CHECK(r.results == std::vector<std::string>({"v:a", "v:c", "v:d"}));
        CHECK(reg->contains("stuck"));
        CHECK(g.stuckCalls == 1);
    }

    // --- 3. The next walk skips it: no wait, no second blocked thread -----
    {
        // A long stall, so that "under it" cannot be a busy machine's doing.
        WalkBudget patient = budget;
        patient.stall = milliseconds(600);
        const auto t0 = Clock::now();
        const BoundedWalkResult<std::string> r =
            boundedWalk<std::string>(keys({"a", "stuck", "c"}), work, reg, patient);
        const double ms = msSince(t0);
        std::printf("walk with the item already quarantined: %zu results after %.1f ms\n",
                    r.results.size(), ms);
        CHECK(ms < 450.0);  // well under the stall: nothing was waited on
        CHECK(r.complete);
        CHECK(r.skipped == 1u);
        CHECK(r.wedged.empty());
        CHECK(r.results == std::vector<std::string>({"v:a", "v:c"}));
        CHECK(g.stuckCalls == 1);  // never entered again
    }

    // --- 4. When the stuck read returns, the item is back on its own -----
    {
        {
            std::lock_guard<std::mutex> lk(g.m);
            g.release = true;
        }
        g.cv.notify_all();
        for (int i = 0; i < 300 && reg->size() != 0u; ++i) {
            std::this_thread::sleep_for(milliseconds(10));
        }
        CHECK(reg->size() == 0u);
        const BoundedWalkResult<std::string> r =
            boundedWalk<std::string>(keys({"a", "stuck", "c"}), work, reg, budget);
        CHECK(r.complete);
        CHECK(r.skipped == 0u);
        CHECK(r.results == std::vector<std::string>({"v:a", "v:stuck", "v:c"}));
    }

    // --- 5. The total budget caps a walk of merely slow items ------------
    //
    // Ten items of 60 ms each against a 150 ms total: the caller is handed
    // what finished, flagged incomplete, long before the 600 ms the loop
    // would have taken.
    {
        WalkBudget tight;
        tight.stall = milliseconds(100);
        tight.total = milliseconds(150);
        const auto t0 = Clock::now();
        const BoundedWalkResult<std::string> r = boundedWalk<std::string>(
            keys({"1", "2", "3", "4", "5", "6", "7", "8", "9", "10"}), slowWork,
            std::make_shared<WedgeRegistry>(), tight);
        const double ms = msSince(t0);
        std::printf("ten 60 ms items against a 150 ms total: %zu results after %.1f ms\n",
                    r.results.size(), ms);
        CHECK(ms < 500.0);
        CHECK(!r.complete);
        CHECK(!r.results.empty() && r.results.size() < 10u);
        // What came back is the front of the list, in order.
        CHECK(r.results.front() == "v:1");
    }

    // --- 6. An item that throws is skipped, not fatal --------------------
    {
        const BoundedWalkResult<std::string> r = boundedWalk<std::string>(
            keys({"a", "throws", "c"}), work, std::make_shared<WedgeRegistry>(), budget);
        CHECK(r.complete);
        CHECK(r.results == std::vector<std::string>({"v:a", "v:c"}));
    }

    // Every abandoned worker has returned before the shared state goes away.
    for (int i = 0; i < 500 && g.inWork.load() != 0; ++i) {
        std::this_thread::sleep_for(milliseconds(10));
    }
    CHECK(g.inWork.load() == 0);

    return testSummary("test_usb_bounded_walk");
}
