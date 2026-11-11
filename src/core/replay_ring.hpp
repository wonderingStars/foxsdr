// The replay ring: the last N seconds of ONE radio's finished audio, mono, 16-bit, kept in a
// fixed-size ring so that "replay" and "save the last 30 s" and the pre-roll of a squelch clip
// can ask for a copy of it.
//
// STANDALONE BLOCK. Nothing here is wired into the pipeline or the engine
// (docs/feature-records/signal-blocks.md, "Wiring hand-off", says where it goes). Design:
// pre-release-features-design.md section 2 (replay) and 3 (clips).
//
// WHAT IT STORES. Mono int16 at the rate given to resize() (the pipeline's 48 kHz audio rate),
// quantised exactly as core::Recorder does for an audio take: clamp to [-1, 1], round(v * 32768),
// saturate to int16 (so +1.0 is 32767); a NaN is stored as 0. Beside the audio, ONE GATE BIT PER
// 1024-SAMPLE BLOCK: set when the squelch gate was open for any part of the block ("Skip silence"
// replays only those blocks).
//
// SAMPLE NUMBERS. Every sample ever written has an absolute number, counting from 0 at the first
// write after construction (resize() keeps the count going). head() is the number of the NEXT sample
// to be written, so "the last n samples" are [head() - n, head()). The ring holds [oldest(), head()).
//
// THREADS, AND WHY THIS NEVER ALLOCATES OR LOCKS WHERE IT MUST NOT.
//   control thread : resize(). It allocates and moves audio. It must not run while another
//                    thread is inside write() or a copy*: the owner installs the ring in the DSP
//                    thread with the same pointer hand-off the recorders use (pipeline.hpp: install after
//                    allocation, remove before free), so resize() is called on a ring nobody holds.
//   DSP thread     : write(). One writer only. No allocation, no lock, no file, no wait: a loop of
//                    quantising stores into storage allocated by resize(). A call longer than
//                    kMaxWriteChunk samples is processed in chunks, publishing head() after each.
//   any other thread (a worker writing a clip, the engine pass answering "save replay"): head(),
//                    oldest(), capacity(), gateOpenAt(), copyRange(), copyLast(), copyGated(). They
//                    read while the writer runs. A copy loads head() first, copies, loads head()
//                    again, and DISCARDS the front of what it copied that the writer may since have
//                    overwritten (the writer announces the end of the chunk it is about to write in
//                    reserved_ BEFORE storing it, and head_ after, in the manner of a seqlock); it
//                    returns only samples proven intact and says where they start. The sample
//                    storage is std::atomic<int16_t> with relaxed order, so a concurrent read is
//                    defined behaviour (no data race in the C++ sense) and ThreadSanitizer-clean;
//                    head() is the release/acquire edge.
//
// MEMORY. 96 kB per second per radio (30 s = 2.9 MB). resize(rate, 0) frees everything: "Off" holds
// no memory and write() is then a no-op.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace cascade::core {

class ReplayRing {
public:
    static constexpr std::size_t kGateBlock = 1024;
    static constexpr std::size_t kMaxWriteChunk = 16384;
    // The most the ring will hold: 10 minutes at 48 kHz (57.6 MB). A larger request is refused.
    static constexpr double kMaxSeconds = 600.0;

    ReplayRing() = default;
    ReplayRing(const ReplayRing&) = delete;
    ReplayRing& operator=(const ReplayRing&) = delete;

    // Control thread, ring not in use by any other thread. Makes the ring hold `seconds` seconds of
    // audio at `rateHz`: capacity = round(seconds * rate) rounded UP to a whole gate block. The newest
    // min(old held, new capacity) samples and their gate bits are kept with their sample numbers (head() is
    // unchanged), so growing or shrinking the length never loses what still fits. seconds <= 0 frees
    // everything (capacity 0, a write is then a no-op, head() is kept). Returns false and changes
    // nothing for a rate that is not finite or not positive, a length that is not finite, or more than
    // kMaxSeconds. A change of RATE with audio held discards the audio (the numbers no longer mean the
    // same time): head() is kept, oldest() becomes head().
    bool resize(double rateHz, double seconds) {
        if (!std::isfinite(rateHz) || rateHz <= 0.0 || !std::isfinite(seconds) || seconds > kMaxSeconds) {
            return false;
        }
        std::size_t cap = 0;
        if (seconds > 0.0) {
            cap = static_cast<std::size_t>(std::llround(seconds * rateHz));
            if (cap < 1) { cap = 1; }
            cap = ((cap + kGateBlock - 1) / kGateBlock) * kGateBlock;
        }
        const std::uint64_t h = head_.load(std::memory_order_acquire);
        const std::size_t oldCap = capacity_;
        const bool sameRate = rate_ == rateHz;
        std::unique_ptr<std::atomic<std::int16_t>[]> audio;
        std::unique_ptr<std::atomic<std::uint8_t>[]> gates;
        if (cap > 0) {
            audio.reset(new std::atomic<std::int16_t>[cap]);
            gates.reset(new std::atomic<std::uint8_t>[cap / kGateBlock]);
            for (std::size_t i = 0; i < cap; ++i) { audio[i].store(0, std::memory_order_relaxed); }
            for (std::size_t i = 0; i < cap / kGateBlock; ++i) { gates[i].store(0, std::memory_order_relaxed); }
        }
        std::uint64_t keepFrom = h;  // first sample number kept
        if (cap > 0 && oldCap > 0 && sameRate) {
            const std::uint64_t held = std::min<std::uint64_t>(h - std::min(h, floor_), oldCap);
            const std::uint64_t keep = std::min<std::uint64_t>(held, cap);
            keepFrom = h - keep;
            for (std::uint64_t s = keepFrom; s < h; ++s) {
                audio[s % cap].store(audio_[s % oldCap].load(std::memory_order_relaxed), std::memory_order_relaxed);
            }
            // Gate bits for the blocks that hold kept samples (the oldest block may be partial).
            for (std::uint64_t b = keepFrom / kGateBlock; b <= (h == 0 ? 0 : (h - 1) / kGateBlock); ++b) {
                if (h == 0) { break; }
                gates[b % (cap / kGateBlock)].store(gates_[b % (oldCap / kGateBlock)].load(std::memory_order_relaxed),
                                                    std::memory_order_relaxed);
            }
        }
        audio_ = std::move(audio);
        gates_ = std::move(gates);
        capacity_ = cap;
        rate_ = rateHz;
        floor_ = keepFrom;
        return true;
    }

    std::size_t capacity() const { return capacity_; }
    double rateHz() const { return rate_; }
    double seconds() const { return rate_ > 0.0 ? static_cast<double>(capacity_) / rate_ : 0.0; }

    // The number of the next sample to be written (= samples written so far).
    std::uint64_t head() const { return head_.load(std::memory_order_acquire); }
    // The number of the oldest sample still held (never below what resize() kept).
    std::uint64_t oldest() const {
        const std::uint64_t h = head();
        const std::uint64_t held = std::min<std::uint64_t>(h - std::min(h, floor_), capacity_);
        return h - held;
    }

    // DSP thread, one writer. Appends n samples of audio (float, nominally [-1, 1]); `gateOpen` is the squelch
    // gate for this call (true when the audio was audible). No-op for a ring with no capacity.
    void write(const float* in, std::size_t n, bool gateOpen) noexcept {
        if (capacity_ == 0 || in == nullptr) { return; }
        while (n > 0) {
            const std::size_t m = std::min(n, kMaxWriteChunk);
            const std::uint64_t h = head_.load(std::memory_order_relaxed);
            reserved_.store(h + m, std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_release);  // the announcement precedes the stores
            for (std::size_t i = 0; i < m; ++i) {
                audio_[(h + i) % capacity_].store(quantise(in[i]), std::memory_order_relaxed);
            }
            // Gate bits: a block's bit is cleared when its first sample is written, then set by any open write.
            const std::uint64_t firstBlock = h / kGateBlock, lastBlock = (h + m - 1) / kGateBlock;
            const std::size_t nBlocks = capacity_ / kGateBlock;
            for (std::uint64_t b = firstBlock; b <= lastBlock; ++b) {
                std::atomic<std::uint8_t>& g = gates_[b % nBlocks];
                if (b * kGateBlock >= h) { g.store(gateOpen ? 1 : 0, std::memory_order_relaxed); }  // block starts in this chunk
                else if (gateOpen) { g.store(1, std::memory_order_relaxed); }
            }
            head_.store(h + m, std::memory_order_release);
            in += m;
            n -= m;
        }
    }

    // True when the block holding sample `s` had the gate open for any of its samples so far. False for a
    // sample not held.
    bool gateOpenAt(std::uint64_t s) const {
        if (capacity_ == 0) { return false; }
        const std::uint64_t h = head();
        if (s >= h || s < oldest()) { return false; }
        return gates_[(s / kGateBlock) % (capacity_ / kGateBlock)].load(std::memory_order_relaxed) != 0;
    }

    // Copies up to n samples starting at sample number `start` into `out`. Returns how many were copied and
    // sets *firstOut to the number of the first one (>= start: the front is dropped when it is no longer
    // held, or may have been overwritten by the writer during the copy). The count is the run of samples
    // proven intact; 0 when nothing is. `out` may be null only with n == 0.
    std::size_t copyRange(std::uint64_t start, std::size_t n, std::int16_t* out, std::uint64_t* firstOut = nullptr) const {
        if (firstOut != nullptr) { *firstOut = start; }
        if (n == 0 || out == nullptr || capacity_ == 0) { return 0; }
        const std::uint64_t h1 = head_.load(std::memory_order_acquire);
        const std::uint64_t lo = oldestAt(h1);
        std::uint64_t from = std::max(start, lo);
        if (from >= h1) { if (firstOut != nullptr) { *firstOut = from; } return 0; }
        const std::uint64_t to = std::min<std::uint64_t>(start + n, h1);
        if (to <= from) { if (firstOut != nullptr) { *firstOut = from; } return 0; }
        for (std::uint64_t s = from; s < to; ++s) {
            out[s - from] = audio_[s % capacity_].load(std::memory_order_relaxed);
        }
        // The writer may have moved on: whatever it has announced it is writing (reserved_) may have
        // overwritten samples older than reserved_ - capacity_. (Seqlock: the fence orders the sample loads above
        // before this load; if any sample read came from a chunk, the announcement of that chunk is seen here.)
        std::atomic_thread_fence(std::memory_order_acquire);
        const std::uint64_t r2 = reserved_.load(std::memory_order_relaxed);
        const std::uint64_t safeFrom = (r2 > capacity_) ? (r2 - capacity_) : 0;
        if (safeFrom > from) {
            const std::uint64_t drop = std::min<std::uint64_t>(safeFrom - from, to - from);
            if (drop >= to - from) { if (firstOut != nullptr) { *firstOut = to; } return 0; }
            const std::size_t keep = static_cast<std::size_t>(to - from - drop);
            std::copy(out + drop, out + drop + keep, out);
            from += drop;
            if (firstOut != nullptr) { *firstOut = from; }
            return keep;
        }
        if (firstOut != nullptr) { *firstOut = from; }
        return static_cast<std::size_t>(to - from);
    }

    // The newest n samples (fewer if fewer are held), oldest first. Returns the count; *firstOut = the number
    // of the first one.
    std::size_t copyLast(std::size_t n, std::int16_t* out, std::uint64_t* firstOut = nullptr) const {
        const std::uint64_t h = head_.load(std::memory_order_acquire);
        const std::uint64_t start = h > n ? h - n : 0;
        return copyRange(start, n, out, firstOut);
    }

    // As copyRange, but leaves out every gate block whose gate was closed ("Skip silence"): the open blocks
    // are packed one after another into `out` (capacity outCap samples). Returns the samples written.
    // Partial blocks at the ends of the range count with their block's bit.
    std::size_t copyGated(std::uint64_t start, std::size_t n, std::int16_t* out, std::size_t outCap) const {
        if (n == 0 || out == nullptr || outCap == 0 || capacity_ == 0) { return 0; }
        std::size_t written = 0;
        std::uint64_t pos = start;
        const std::uint64_t end = start + n;
        while (pos < end && written < outCap) {
            const std::uint64_t blockEnd = std::min<std::uint64_t>(end, (pos / kGateBlock + 1) * kGateBlock);
            const std::size_t want = static_cast<std::size_t>(std::min<std::uint64_t>(blockEnd - pos, outCap - written));
            if (gateOpenAt(pos)) {
                std::uint64_t first = 0;
                const std::size_t got = copyRange(pos, want, out + written, &first);
                if (got > 0 && first == pos) { written += got; }
            }
            pos = blockEnd;
        }
        return written;
    }

private:
    static std::int16_t quantise(float v) noexcept {
        if (!(v == v)) { return 0; }  // NaN
        if (v > 1.0f) { v = 1.0f; }
        if (v < -1.0f) { v = -1.0f; }
        const long q = std::lround(static_cast<double>(v) * 32768.0);
        return static_cast<std::int16_t>(std::clamp<long>(q, -32768, 32767));
    }

    // The oldest sample number held when the head is h.
    std::uint64_t oldestAt(std::uint64_t h) const {
        const std::uint64_t held = std::min<std::uint64_t>(h - std::min(h, floor_), capacity_);
        return h - held;
    }

    std::unique_ptr<std::atomic<std::int16_t>[]> audio_;
    std::unique_ptr<std::atomic<std::uint8_t>[]> gates_;
    std::size_t capacity_ = 0;
    double rate_ = 0.0;
    std::uint64_t floor_ = 0;  // nothing older than this sample number is held (set by resize)
    std::atomic<std::uint64_t> head_{0};
    std::atomic<std::uint64_t> reserved_{0};  // end of the chunk the writer is writing (>= head_)
};

}  // namespace cascade::core
