// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "input_event.hpp"

#include <algorithm>
#include <chrono>

namespace cascade::input {

std::uint32_t deviceIdFor(const std::string& s) {
    std::uint32_t h = 2166136261u;
    for (unsigned char c : s) {
        h ^= c;
        h *= 16777619u;
    }
    return h;
}

std::int64_t nowMicros() {
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

EventQueue::EventQueue(std::size_t capacity) : cap_(std::max<std::size_t>(1, capacity)), ring_(std::max<std::size_t>(1, capacity)) {}

bool EventQueue::push(const InputEvent& e) {
    std::lock_guard<std::mutex> lk(m_);
    bool dropped = false;
    if (count_ == cap_) {
        head_ = (head_ + 1) % cap_;  // drop the oldest
        --count_;
        dropped_.fetch_add(1, std::memory_order_relaxed);
        dropped = true;
    }
    ring_[(head_ + count_) % cap_] = e;
    ++count_;
    return !dropped;
}

std::size_t EventQueue::drain(std::vector<InputEvent>& out, std::size_t max) {
    out.reserve(out.size() + std::min(cap_, max));  // before the lock: the copy below then never allocates while a pusher waits
    std::lock_guard<std::mutex> lk(m_);
    const std::size_t n = std::min(count_, max);
    for (std::size_t i = 0; i < n; ++i) { out.push_back(ring_[(head_ + i) % cap_]); }
    head_ = (head_ + n) % cap_;
    count_ -= n;
    return n;
}

std::size_t EventQueue::size() const {
    std::lock_guard<std::mutex> lk(m_);
    return count_;
}

}  // namespace cascade::input
