// Single-producer single-consumer lock-free ring buffer.
//
// One thread calls write(), a different thread calls read(); neither blocks
// the other. head_ and tail_ are monotonic element COUNTERS, not wrapped
// indices: a counter is masked down to a slot index only at access time.
// Because the capacity is a power of two it divides 2^64, so the counters
// stay congruent modulo capacity even across (theoretical) counter overflow,
// and `head - tail` is always the exact fill level. This also makes the full
// capacity usable — no "keep one slot empty" sentinel is needed to tell a
// full ring from an empty one.
//
// SPDX-License-Identifier: MIT
#pragma once

#include <atomic>
#include <cstddef>
#include <cstring>
#include <stdexcept>
#include <type_traits>
#include <vector>

namespace cascade::dsp {

#if defined(_MSC_VER)
// C4324: "structure was padded due to alignment specifier" — that padding is
// exactly the point of the alignas below, not an accident to warn about.
#pragma warning(push)
#pragma warning(disable : 4324)
#endif

template <typename T>
class SpscRing {
    // The bulk paths memcpy elements in and out, which is only defined for
    // trivially copyable types. (float and std::complex<float> both qualify.)
    static_assert(std::is_trivially_copyable_v<T>,
                  "SpscRing requires a trivially copyable element type");

public:
    // Capacity must be a nonzero power of two: the slot index is
    // `counter & (capacity - 1)`, which is only a correct modulo-capacity
    // reduction when capacity divides the counter's 2^64 wrap.
    explicit SpscRing(std::size_t capacity) : buf_(capacity), mask_(capacity - 1) {
        if (capacity == 0 || (capacity & (capacity - 1)) != 0) {
            throw std::invalid_argument("SpscRing capacity must be a power of two");
        }
    }

    // Atomics pin the object in place; copying a live ring is a race by
    // construction, so forbid it outright.
    SpscRing(const SpscRing&) = delete;
    SpscRing& operator=(const SpscRing&) = delete;

    std::size_t capacity() const { return buf_.size(); }

    // Producer side only. Copies up to n elements from src into the ring and
    // returns how many were accepted. Acceptance is capped at the free space
    // observed on entry, so unread data is never overwritten.
    std::size_t write(const T* src, std::size_t n) {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        // Acquire pairs with the consumer's release store in read(): it
        // guarantees the consumer finished copying OUT of any slot this call
        // is about to reuse before we store new data into it.
        const std::size_t tail = tail_.load(std::memory_order_acquire);
        const std::size_t space = buf_.size() - (head - tail);
        if (n > space) { n = space; }
        if (n == 0) { return 0; }  // also keeps memcpy away from a null src on n==0 calls

        // At most two contiguous segments: up to the end of the array, then
        // the wrapped remainder from the start.
        const std::size_t idx = head & mask_;
        std::size_t first = buf_.size() - idx;
        if (first > n) { first = n; }
        std::memcpy(&buf_[idx], src, first * sizeof(T));
        std::memcpy(buf_.data(), src + first, (n - first) * sizeof(T));

        // Release pairs with the consumer's acquire load of head_: the element
        // copies above become visible to the consumer no later than the new
        // head does. Relaxed here would let the publish overtake the data.
        head_.store(head + n, std::memory_order_release);
        return n;
    }

    // Consumer side only. Copies up to n elements out of the ring into dst
    // and returns how many were produced.
    std::size_t read(T* dst, std::size_t n) {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        // Acquire pairs with the producer's release store in write(): any
        // element the new head covers is fully written before we copy it out.
        const std::size_t head = head_.load(std::memory_order_acquire);
        const std::size_t avail = head - tail;
        if (n > avail) { n = avail; }
        if (n == 0) { return 0; }

        const std::size_t idx = tail & mask_;
        std::size_t first = buf_.size() - idx;
        if (first > n) { first = n; }
        std::memcpy(dst, &buf_[idx], first * sizeof(T));
        std::memcpy(dst + first, buf_.data(), (n - first) * sizeof(T));

        // Release pairs with the producer's acquire load of tail_: our copies
        // out of the freed slots complete before the producer may reuse them.
        tail_.store(tail + n, std::memory_order_release);
        return n;
    }

    // Snapshot fill level. Exact when called from either endpoint about its
    // own progress; when the other thread is mid-flight the staleness is
    // conservative in the safe direction (a consumer may see fewer readable
    // elements than exist, a producer may see less free space than exists).
    std::size_t size() const {
        const std::size_t head = head_.load(std::memory_order_acquire);
        const std::size_t tail = tail_.load(std::memory_order_acquire);
        return head - tail;
    }

    std::size_t freeSpace() const { return buf_.size() - size(); }

private:
    std::vector<T> buf_;
    std::size_t mask_;
    // Producer- and consumer-owned counters live on separate cache lines so
    // the two threads' publishes do not false-share a line and ping-pong it.
    alignas(64) std::atomic<std::size_t> head_{0};  // written by producer
    alignas(64) std::atomic<std::size_t> tail_{0};  // written by consumer
};

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

}  // namespace cascade::dsp
