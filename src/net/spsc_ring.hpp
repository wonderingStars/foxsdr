// spsc_ring.hpp - a wait-free single-producer / single-consumer ring of whole MESSAGES (length-prefixed byte records).
//
// This is the hand-over every network output part uses between the thread that has the data (the DSP or decoder thread) and the thread
// that owns the sockets. push() is wait-free: a fixed number of steps, no lock, no allocation, no system call, no loop that waits on
// another thread; when the ring cannot take the record it returns false and counts the drop, and the producer carries on. pop() is the
// consumer's side, the same.
//
// ONE producer thread and ONE consumer thread per ring. Two producers would race on `head_`; a caller with several producers gives each
// its own output, or serialises them itself (that serialisation is the caller's lock, not this class's).
//
// Layout: a power-of-two byte buffer, free-running 64-bit head and tail counters (so full and empty are never confused), each record a
// 4-byte length then the bytes, copied across the wrap. A record never exceeds half the capacity (push refuses it, counted as oversize).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_NET_SPSC_RING_HPP
#define CASCADE_NET_SPSC_RING_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace cascade::net {

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4324)  // padded for the cache-line alignment on purpose
#endif

class MessageRing {
public:
    // `capacityBytes` is rounded up to a power of two (at least 64).
    explicit MessageRing(std::size_t capacityBytes) {
        std::size_t c = 64;
        while (c < capacityBytes) { c <<= 1; }
        buf_.assign(c, 0);
        mask_ = c - 1;
    }
    MessageRing(const MessageRing&) = delete;
    MessageRing& operator=(const MessageRing&) = delete;

    std::size_t capacity() const { return buf_.size(); }
    // The longest record push() accepts.
    std::size_t maxMessage() const { return buf_.size() / 2 - kLenBytes; }

    // PRODUCER THREAD. Wait-free. False (and a counted drop) when the record is empty, oversize, or the ring is full.
    bool push(const void* data, std::size_t n) noexcept {
        if (n == 0 || n > maxMessage()) {
            oversize_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        const std::uint64_t head = head_.load(std::memory_order_relaxed);
        const std::uint64_t tail = tail_.load(std::memory_order_acquire);
        const std::uint64_t need = kLenBytes + n;
        if (buf_.size() - static_cast<std::size_t>(head - tail) < need) {
            dropped_.fetch_add(1, std::memory_order_relaxed);
            droppedBytes_.fetch_add(n, std::memory_order_relaxed);
            return false;
        }
        const std::uint32_t len = static_cast<std::uint32_t>(n);
        copyIn(head, &len, kLenBytes);
        copyIn(head + kLenBytes, data, n);
        head_.store(head + need, std::memory_order_release);
        return true;
    }

    // CONSUMER THREAD. Copies the oldest record into `out` (capacity `cap`) and returns its length; 0 when the ring is empty. With
    // cap >= maxMessage() a record always fits whole.
    std::size_t pop(void* out, std::size_t cap) noexcept {
        const std::uint64_t tail = tail_.load(std::memory_order_relaxed);
        const std::uint64_t head = head_.load(std::memory_order_acquire);
        if (head == tail) { return 0; }
        std::uint32_t len = 0;
        copyOut(tail, &len, kLenBytes);
        const std::size_t n = (len <= cap) ? len : cap;
        copyOut(tail + kLenBytes, out, n);
        tail_.store(tail + kLenBytes + len, std::memory_order_release);
        return n;
    }

    bool empty() const { return head_.load(std::memory_order_acquire) == tail_.load(std::memory_order_acquire); }
    std::uint64_t dropped() const { return dropped_.load(std::memory_order_relaxed); }
    std::uint64_t droppedBytes() const { return droppedBytes_.load(std::memory_order_relaxed); }
    std::uint64_t oversize() const { return oversize_.load(std::memory_order_relaxed); }

private:
    static constexpr std::size_t kLenBytes = 4;

    void copyIn(std::uint64_t pos, const void* src, std::size_t n) noexcept {
        const std::size_t i = static_cast<std::size_t>(pos) & mask_;
        const std::size_t first = (n < buf_.size() - i) ? n : buf_.size() - i;
        std::memcpy(&buf_[i], src, first);
        if (first < n) { std::memcpy(&buf_[0], static_cast<const std::uint8_t*>(src) + first, n - first); }
    }
    void copyOut(std::uint64_t pos, void* dst, std::size_t n) const noexcept {
        const std::size_t i = static_cast<std::size_t>(pos) & mask_;
        const std::size_t first = (n < buf_.size() - i) ? n : buf_.size() - i;
        std::memcpy(dst, &buf_[i], first);
        if (first < n) { std::memcpy(static_cast<std::uint8_t*>(dst) + first, &buf_[0], n - first); }
    }

    std::vector<std::uint8_t> buf_;
    std::size_t mask_ = 0;
    alignas(64) std::atomic<std::uint64_t> head_{0};
    alignas(64) std::atomic<std::uint64_t> tail_{0};
    alignas(64) std::atomic<std::uint64_t> dropped_{0};
    std::atomic<std::uint64_t> droppedBytes_{0};
    std::atomic<std::uint64_t> oversize_{0};
};

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

}  // namespace cascade::net

#endif
