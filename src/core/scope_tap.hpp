// scope_tap.hpp - a rolling window of the newest samples, written by the DSP
// thread and read by the GUI, where the READER is the one that gives way.
//
// WHY NOT SpscRing, AND WHY NOT Pipeline's EXISTING TAP. The two taps this
// product already has are both the wrong shape for an oscilloscope:
//
//   dsp/spsc_ring.hpp is a QUEUE. Its writer refuses to overwrite anything the
//   reader has not taken, which is exactly right for samples that must all be
//   processed and exactly wrong for a display - a scope that is not being
//   looked at would fill the ring, and from then on the DSP thread's writes
//   would be rejected and the picture would be a frozen 30 ms of history from
//   whenever the window was last polled.
//
//   Pipeline::audioTap is a rolling window of the right KIND, but it is held
//   under audioMutex_ - the mutex the DSP thread owns across the whole of a
//   block - and it is 4096 frames, 85 ms at 48 kHz. The longest sweep this
//   scope offers is 50 ms per division across ten divisions, which is half a
//   second: six times what that tap can hold. Widening it and taking its lock
//   once a frame from the GUI would put the render thread behind a block of
//   DSP for a picture.
//
// So: one writer, no lock, and a reader that may be told it was too slow. The
// DSP thread NEVER waits and never fails to write - it overwrites the oldest
// samples, which for a display is not data loss but the whole point. A reader
// that was overwritten mid-copy is told so and asks again, and if it keeps
// losing the race it gives up for this frame rather than spinning: one dropped
// frame of a trace is invisible, a stalled render thread is not.
//
// THE VALIDATION IS THE ONLY SUBTLE PART, and it takes TWO counters. push()
// advances `begun_` BEFORE it writes the elements and `written_` AFTER, both
// to the same value. A reader that has seen written_ has seen the elements
// (release/acquire). snapshot() reads written_, copies the newest `avail`
// elements, and then reads begun_: if a push has BEGUN that reaches back into
// the span just copied (begun2 - start > capacity), the copy may hold half of
// that push's elements and is thrown away. Anything less than that and the
// span is intact, because a power-of-two capacity makes the monotonic
// counters exact modulo the buffer size.
//
// WHY NOT READ written_ TWICE, which is what the first version did and what
// reads naturally: the writer's memcpy lands in the buffer BEFORE written_
// moves, so a reader whose copy overlapped an in-flight push saw the same
// written_ both times and accepted a torn window. The race test caught it at
// about two runs in thirty. The counter that guards the copy has to be the one
// that moves before the elements do - the ordinary seqlock shape, with the
// "sequence" split into its pre-write and post-write halves.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_CORE_SCOPE_TAP_HPP
#define CASCADE_CORE_SCOPE_TAP_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <type_traits>
#include <vector>

namespace cascade::core {

#if defined(_MSC_VER)
// C4324: "structure was padded due to alignment specifier" - that padding is
// exactly the point of the alignas on written_ below, not an accident to warn
// about. Same suppression, for the same reason, as dsp/spsc_ring.hpp.
#pragma warning(push)
#pragma warning(disable : 4324)
#endif

template <typename T>
class ScopeTap {
    // The bulk paths memcpy, which is only defined for trivially copyable
    // types. float and std::complex<float> - the two this is instantiated
    // with - both qualify.
    static_assert(std::is_trivially_copyable_v<T>,
                  "ScopeTap requires a trivially copyable element type");

public:
    // Capacity must be a nonzero power of two: a slot is `counter & (capacity
    // - 1)`, which is only a correct modulo-capacity reduction when capacity
    // divides the counter's own 2^64 wrap.
    explicit ScopeTap(std::size_t capacity) : buf_(capacity), mask_(capacity - 1) {
        if (capacity == 0 || (capacity & (capacity - 1)) != 0) {
            throw std::invalid_argument("ScopeTap capacity must be a power of two");
        }
    }

    ScopeTap(const ScopeTap&) = delete;
    ScopeTap& operator=(const ScopeTap&) = delete;

    std::size_t capacity() const { return buf_.size(); }

    // How many elements have EVER been written. Monotonic, and the reader's
    // way to tell a live tap from a stalled one - two frames at the same
    // number means nothing arrived, which is a different statement from "the
    // samples are all zero" and has to stay tellable apart from it.
    std::uint64_t written() const { return written_.load(std::memory_order_acquire); }

    // PRODUCER SIDE ONLY, and it never fails, never waits and never allocates.
    // A push longer than the buffer keeps only its last `capacity` elements -
    // the older ones could not have survived the same push anyway.
    void push(const T* src, std::size_t n) {
        if (src == nullptr || n == 0) { return; }
        const std::size_t cap = buf_.size();
        if (n > cap) {
            src += (n - cap);
            n = cap;
        }
        const std::uint64_t head = written_.load(std::memory_order_relaxed);
        const std::size_t idx = static_cast<std::size_t>(head) & mask_;
        std::size_t first = cap - idx;
        if (first > n) { first = n; }
        // "A push has begun that ends at head + n" - stored BEFORE any element,
        // and the release fence keeps it ahead of the element stores below, so
        // a reader that copied an element of this push cannot then read a
        // begun_ that predates it.
        begun_.store(head + n, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        std::memcpy(&buf_[idx], src, first * sizeof(T));
        if (n > first) { std::memcpy(buf_.data(), src + first, (n - first) * sizeof(T)); }
        // Release pairs with the acquire in written()/snapshot(): the element
        // stores above are visible to the reader no later than the counter is.
        written_.store(head + n, std::memory_order_release);
    }

    // CONSUMER SIDE. Copies the newest min(n, written, capacity) elements into
    // dst in chronological order (oldest first, so dst[count-1] is the most
    // recent sample) and returns how many. Zero means either nothing has been
    // written yet or the writer lapped this reader on every attempt - the two
    // are told apart by written(), and a caller that draws nothing on zero is
    // correct for both.
    //
    // `attempts` bounds the retry so a reader can never spin behind a writer
    // that is faster than the copy. Three is empirically generous: at 48 kHz
    // the DSP thread pushes about a thousand samples a millisecond and this
    // copy is a memcpy of at most a buffer's worth.
    std::size_t snapshot(T* dst, std::size_t n, int attempts = 3) const {
        if (dst == nullptr || n == 0) { return 0; }
        const std::size_t cap = buf_.size();
        if (n > cap) { n = cap; }
        for (int attempt = 0; attempt < attempts; ++attempt) {
            const std::uint64_t end = written_.load(std::memory_order_acquire);
            std::size_t avail = n;
            if (end < static_cast<std::uint64_t>(avail)) {
                avail = static_cast<std::size_t>(end);
            }
            if (avail == 0) { return 0; }
            const std::uint64_t start = end - avail;
            const std::size_t idx = static_cast<std::size_t>(start) & mask_;
            std::size_t first = cap - idx;
            if (first > avail) { first = avail; }
            std::memcpy(dst, &buf_[idx], first * sizeof(T));
            if (avail > first) {
                std::memcpy(dst + first, buf_.data(), (avail - first) * sizeof(T));
            }
            // Has a push BEGUN that reaches back into what was just copied? It
            // has iff begun_ is more than a whole buffer past `start`. The
            // acquire fence keeps the element loads above ahead of this load,
            // the mirror of the writer's release fence.
            std::atomic_thread_fence(std::memory_order_acquire);
            const std::uint64_t begun2 = begun_.load(std::memory_order_relaxed);
            if (begun2 - start <= static_cast<std::uint64_t>(cap)) { return avail; }
        }
        return 0;
    }

private:
    std::vector<T> buf_;
    std::size_t mask_;
    // Monotonic element counter. alignas keeps it off the reader's cache line
    // for the buffer itself; the two are touched by different threads every
    // block.
    alignas(64) std::atomic<std::uint64_t> written_{0};
    // Where the newest push will END, stored before its first element lands.
    // Only ever ahead of or equal to written_.
    alignas(64) std::atomic<std::uint64_t> begun_{0};
};

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

}  // namespace cascade::core

#endif  // CASCADE_CORE_SCOPE_TAP_HPP
