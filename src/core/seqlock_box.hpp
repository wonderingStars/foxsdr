// seqlock_box.hpp - a value one thread writes and any thread reads, without
// a lock.
//
// Moved out of core/plugin_api.hpp in engine stage 2 (docs/engine-stage2.md),
// unchanged: it now carries the receiver snapshot every reader shares
// (core/receiver_snapshot.hpp) as well as the plugin runner's stream clock.
//
// A sequence lock over a trivially copyable T, stored as relaxed atomic words
// so that a reader racing a writer is not undefined behaviour - it reads a
// torn copy, sees the sequence moved, and reads again. The writer never
// waits. A reader waits only while a write is IN PROGRESS, which is a copy of
// a few hundred bytes; it gives up after kMaxTries rather than spin for ever
// behind a writer that was preempted mid-copy, and says so by returning false.
//
// ONE WRITER AT A TIME. Two concurrent writers would interleave their words;
// every user of this class names the lock or the thread that serialises them.
//
// tests/test_receiver_snapshot.cpp races a writer against readers and fails
// on any torn copy; it goes red when the sequence re-check in load() is
// removed.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_CORE_SEQLOCK_BOX_HPP
#define CASCADE_CORE_SEQLOCK_BOX_HPP

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace cascade::core {

template <class T>
class SeqlockBox {
    static_assert(std::is_trivially_copyable_v<T>, "SeqlockBox holds plain data only");

public:
    static constexpr int kMaxTries = 4096;

    SeqlockBox() {
        T zero{};
        store(zero);
        seq_.store(0, std::memory_order_relaxed);
    }

    void store(const T& v) {
        std::uint64_t words[kWords] = {};
        std::memcpy(words, &v, sizeof(T));
        const std::uint64_t s = seq_.load(std::memory_order_relaxed);
        seq_.store(s + 1, std::memory_order_relaxed);   // odd: write in progress
        std::atomic_thread_fence(std::memory_order_release);
        for (std::size_t i = 0; i < kWords; ++i) {
            data_[i].store(words[i], std::memory_order_relaxed);
        }
        seq_.store(s + 2, std::memory_order_release);   // even: consistent
    }

    // True with `out` filled from one consistent write; false (out untouched)
    // only if kMaxTries attempts all overlapped a write.
    bool load(T& out) const {
        for (int attempt = 0; attempt < kMaxTries; ++attempt) {
            const std::uint64_t s1 = seq_.load(std::memory_order_acquire);
            if ((s1 & 1u) != 0u) { continue; }
            std::uint64_t words[kWords];
            for (std::size_t i = 0; i < kWords; ++i) {
                words[i] = data_[i].load(std::memory_order_relaxed);
            }
            std::atomic_thread_fence(std::memory_order_acquire);
            if (seq_.load(std::memory_order_relaxed) == s1) {
                std::memcpy(&out, words, sizeof(T));
                return true;
            }
        }
        return false;
    }

private:
    static constexpr std::size_t kWords = (sizeof(T) + 7u) / 8u;
    std::atomic<std::uint64_t> seq_{0};
    std::array<std::atomic<std::uint64_t>, kWords> data_{};
};

}  // namespace cascade::core

#endif  // CASCADE_CORE_SEQLOCK_BOX_HPP
