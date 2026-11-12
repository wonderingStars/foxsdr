// vendor_apartment.cpp - see vendor_apartment.hpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "source/vendor_apartment.hpp"

#include <algorithm>
#include <chrono>
#include <deque>

#include "core/health_events.hpp"
#include "source/vendor_guard.hpp"

namespace cascade::source::vendorapt {

namespace {

constexpr int kJoinMs = 1500;

struct TaskState {
    std::function<void()> fn;
    std::mutex m;
    std::condition_variable cv;
    bool done = false;
    bool faulted = false;
};

void trampoline(void* ctx) noexcept {
    auto* fn = static_cast<std::function<void()>*>(ctx);
    try {
        (*fn)();
    } catch (...) {
    }
}

}  // namespace

std::int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

struct Apartment::Shared {
    std::mutex m;
    std::condition_variable cv;   // work or stop
    std::condition_variable cv2;  // exited
    std::deque<std::shared_ptr<TaskState>> q;
    bool stop = false;
    bool exited = false;
};

Apartment::Apartment() : st_(std::make_shared<Shared>()) {
    auto st = st_;
    thread_ = std::thread([st] { loop(st); });
}

Apartment::~Apartment() { shutdown(); }

void Apartment::loop(std::shared_ptr<Shared> st) {
    for (;;) {
        std::shared_ptr<TaskState> t;
        {
            std::unique_lock<std::mutex> lk(st->m);
            st->cv.wait(lk, [&] { return st->stop || !st->q.empty(); });
            if (st->q.empty()) { break; }
            t = st->q.front();
            st->q.pop_front();
        }
        const bool ok = callGuardingVendorFaults(&trampoline, &t->fn);
        {
            std::lock_guard<std::mutex> lk(t->m);
            t->faulted = !ok;
            t->done = true;
        }
        t->cv.notify_all();
    }
    {
        std::lock_guard<std::mutex> lk(st->m);
        st->exited = true;
    }
    st->cv2.notify_all();
}

Apartment::Result Apartment::run(std::function<void()> fn, int timeoutMs) {
    if (dead_.load()) { return Result::Dead; }
    auto t = std::make_shared<TaskState>();
    t->fn = std::move(fn);
    {
        std::lock_guard<std::mutex> lk(st_->m);
        if (st_->stop) { return Result::Dead; }
        st_->q.push_back(t);
    }
    st_->cv.notify_one();
    std::unique_lock<std::mutex> lk(t->m);
    if (!t->cv.wait_for(lk, std::chrono::milliseconds(timeoutMs), [&] { return t->done; })) {
        dead_.store(true);
        return Result::TimedOut;
    }
    if (t->faulted) {
        dead_.store(true);
        return Result::Faulted;
    }
    return Result::Done;
}

bool Apartment::shutdown() {
    if (!thread_.joinable()) { return !abandoned_; }
    {
        std::lock_guard<std::mutex> lk(st_->m);
        st_->stop = true;
    }
    st_->cv.notify_all();
    std::unique_lock<std::mutex> lk(st_->m);
    const bool exited = st_->cv2.wait_for(lk, std::chrono::milliseconds(kJoinMs), [&] { return st_->exited; });
    lk.unlock();
    if (exited) {
        thread_.join();
        return true;
    }
    abandoned_ = true;
    core::health::raiseRecovered(core::health::Recovered::ReaderLeak);
    thread_.detach();
    return false;
}

void StreamLink::countBad() {
    callbacks.fetch_add(1, std::memory_order_relaxed);
    bad.fetch_add(1, std::memory_order_relaxed);
}

void StreamLink::deliver(const void* buffer, std::uint32_t n, std::uint32_t bits) {
    callbacks.fetch_add(1, std::memory_order_relaxed);
    if (buffer == nullptr || n == 0 || n > (1u << 20) || (bits != 16 && bits != 32)) {
        bad.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (!gate.load(std::memory_order_acquire)) { return; }
    std::complex<float> tmp[1024];
    std::uint32_t done = 0;
    const auto* b16 = static_cast<const std::int16_t*>(buffer);
    const auto* b32 = static_cast<const std::int32_t*>(buffer);
    while (done < n) {
        const std::uint32_t k = std::min<std::uint32_t>(n - done, 1024);
        for (std::uint32_t i = 0; i < k; ++i) {
            const std::size_t at = (static_cast<std::size_t>(done) + i) * 2;
            if (bits == 16) {
                tmp[i] = {static_cast<float>(b16[at]) * (1.0f / 32768.0f), static_cast<float>(b16[at + 1]) * (1.0f / 32768.0f)};
            } else {
                tmp[i] = {static_cast<float>(static_cast<double>(b32[at]) / 2147483648.0), static_cast<float>(static_cast<double>(b32[at + 1]) / 2147483648.0)};
            }
        }
        const std::size_t w = ring.write(tmp, k);
        pairs.fetch_add(w, std::memory_order_relaxed);
        if (w < k) { dropped.fetch_add(k - w, std::memory_order_relaxed); }
        done += k;
    }
    lastDataMs.store(nowMs(), std::memory_order_release);
    cv.notify_one();
}

}  // namespace cascade::source::vendorapt
