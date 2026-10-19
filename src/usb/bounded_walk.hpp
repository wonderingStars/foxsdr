// bounded_walk.hpp - a per-item walk the CALLER can wait on with a deadline,
// for work that touches a kernel or driver file and so has no timeout of its
// own.
//
// WHY THIS EXISTS (hang signature 40ABD0BB073D899C, Arch Linux, 0.99.72): the
// GUI thread froze for more than five seconds inside fopen/fread of a
// /sys/bus/usb/devices/<node>/<attribute> file. A sysfs attribute read goes
// into the USB core and can block for as long as the device is mid-reset or
// its driver is wedged; the read has no timeout and cannot be interrupted, and
// AppWindow::scanNative() runs it inline on the GUI thread. usb_device.hpp
// rule 3 (every wait is bounded) held for the transfers and not for the
// listing.
//
// WHAT IT DOES, the shape this project already accepted for a wedged
// third-party resource (source/vendor_guard.hpp, the SDRplay service scan):
// run the items on a worker, wait for it with a bound, and when the bound is
// hit ABANDON the worker rather than wait for it. A thread blocked in the
// kernel cannot be cancelled from outside, so the abandoned thread and the
// descriptor it holds are leaked until the read returns, which may be never.
// That is the whole cost, and it is paid once per wedged item, not per scan:
//
//   * THE BOUND IS A STALL, NOT A FLAT TIME. `stall` is how long ONE item may
//     take with no other item finishing; `total` caps the whole walk. A healthy
//     tree finishes in microseconds and never meets either.
//   * THE WEDGED ITEM IS NAMED AND SKIPPED. It goes into a WedgeRegistry, the
//     walk carries on with the items after it on a fresh worker (so one stuck
//     node costs one stall, not the rest of the list), and every later walk
//     skips it without starting a thread. The abandoned worker removes it from
//     the registry the moment its read does return, so a device that recovers
//     reappears on the next walk with nobody having to remember to retry.
//   * RESULTS KEEP THE ORDER OF THE KEYS, so a walk that lost no item returns
//     exactly what the inline loop it replaced returned.
//
// THE CALLER OWES the work function: it must not touch the caller's stack (it
// may outlive the call) and it must not throw out of the worker's contract - a
// throw is treated as "no result for this item". Nothing here is Linux-only; it
// is a header of its own so that Windows CI exercises the deadline logic
// (tests/test_usb_bounded_walk.cpp) and tests/test_usb_usbfs.cpp exercises it
// against a real blocked open().
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace cascade::usb {

struct WalkBudget {
    // One item with no other item finishing for this long is wedged. Far past
    // any healthy attribute read (microseconds) and a fifth of the 5 s hang
    // watchdog, so even several wedges in one scan stay inside it.
    std::chrono::milliseconds stall{300};
    // The whole walk, however many items are merely slow rather than stuck.
    std::chrono::milliseconds total{1000};
};

// Keys whose work is still blocked on an abandoned worker. Shared by every
// walk over the same resource; thread-safe.
class WedgeRegistry {
public:
    bool contains(const std::string& key) const {
        std::lock_guard<std::mutex> lk(m_);
        return keys_.count(key) != 0;
    }
    void add(const std::string& key) {
        std::lock_guard<std::mutex> lk(m_);
        keys_.insert(key);
    }
    void remove(const std::string& key) {
        std::lock_guard<std::mutex> lk(m_);
        keys_.erase(key);
    }
    std::size_t size() const {
        std::lock_guard<std::mutex> lk(m_);
        return keys_.size();
    }

private:
    mutable std::mutex m_;
    std::set<std::string> keys_;
};

template <class Result>
struct BoundedWalkResult {
    std::vector<Result> results;       // one per finished key, in key order
    std::vector<std::string> wedged;   // keys abandoned by THIS call
    std::size_t skipped = 0;           // keys left out: wedged by an earlier call
    bool complete = true;              // false: a key was abandoned or the budget ran out
};

namespace detail {

constexpr std::size_t kNoItem = static_cast<std::size_t>(-1);

// Everything the worker touches lives here, behind a shared_ptr, because the
// worker can outlive the call that started it.
template <class Result>
struct WalkRun {
    WalkRun(std::vector<std::string> k, std::function<Result(const std::string&)> w,
            std::shared_ptr<WedgeRegistry> r)
        : keys(std::move(k)), work(std::move(w)), registry(std::move(r)) {}

    const std::vector<std::string> keys;
    const std::function<Result(const std::string&)> work;
    const std::shared_ptr<WedgeRegistry> registry;

    std::mutex m;
    std::condition_variable cv;
    std::vector<Result> results;       // guarded by m
    std::size_t progress = 0;          // items finished; guarded by m
    std::size_t next = 0;              // first item not finished; guarded by m
    std::size_t reading = kNoItem;     // item inside work() now; guarded by m
    bool finished = false;             // guarded by m
    bool abandoned = false;            // guarded by m
};

template <class Result>
void walkWorker(std::shared_ptr<WalkRun<Result>> run, std::size_t first) {
    for (std::size_t i = first; i < run->keys.size(); ++i) {
        {
            std::lock_guard<std::mutex> lk(run->m);
            if (run->abandoned) { return; }
            run->reading = i;
        }
        bool have = false;
        Result value{};
        try {
            value = run->work(run->keys[i]);
            have = true;
        } catch (...) {
        }
        {
            std::lock_guard<std::mutex> lk(run->m);
            if (run->abandoned) {
                // The call gave up on this item while it was inside work(); it
                // is back now, so it is no longer wedged.
                run->registry->remove(run->keys[i]);
                return;
            }
            if (have) { run->results.push_back(std::move(value)); }
            run->reading = kNoItem;
            run->next = i + 1;
            ++run->progress;
        }
        run->cv.notify_all();
    }
    {
        std::lock_guard<std::mutex> lk(run->m);
        run->finished = true;
    }
    run->cv.notify_all();
}

}  // namespace detail

// Runs work(key) for every key not already in `registry`, on a worker, and
// waits at most `budget`. Never blocks on a wedged item; see the file header.
template <class Result>
BoundedWalkResult<Result> boundedWalk(const std::vector<std::string>& allKeys,
                                      const std::function<Result(const std::string&)>& work,
                                      const std::shared_ptr<WedgeRegistry>& registry,
                                      const WalkBudget& budget = WalkBudget{}) {
    using Clock = std::chrono::steady_clock;
    BoundedWalkResult<Result> out;
    std::vector<std::string> keys;
    keys.reserve(allKeys.size());
    for (const std::string& k : allKeys) {
        if (registry->contains(k)) {
            ++out.skipped;
        } else {
            keys.push_back(k);
        }
    }
    const Clock::time_point overall = Clock::now() + budget.total;
    std::size_t next = 0;
    while (next < keys.size()) {
        auto run = std::make_shared<detail::WalkRun<Result>>(keys, work, registry);
        try {
            std::thread(&detail::walkWorker<Result>, run, next).detach();
        } catch (const std::system_error&) {
            // No thread to be had: report the list incomplete. Reading inline
            // instead would put the hang back.
            out.complete = false;
            break;
        }
        std::unique_lock<std::mutex> lk(run->m);
        while (!run->finished) {
            const std::size_t seen = run->progress;
            const Clock::time_point until = std::min(Clock::now() + budget.stall, overall);
            if (!run->cv.wait_until(lk, until,
                                    [&] { return run->finished || run->progress != seen; })) {
                break;
            }
        }
        const bool finished = run->finished;
        if (!finished) { run->abandoned = true; }
        for (Result& r : run->results) { out.results.push_back(std::move(r)); }
        run->results.clear();
        if (finished) { break; }
        out.complete = false;
        if (run->reading != detail::kNoItem) {
            // Registered while the lock is held, so the worker's removal on
            // its way out cannot run before this add.
            const std::string& key = run->keys[run->reading];
            run->registry->add(key);
            out.wedged.push_back(key);
            next = run->reading + 1;
        } else {
            next = run->next;
        }
        lk.unlock();
        if (Clock::now() >= overall) { break; }
    }
    return out;
}

}  // namespace cascade::usb
