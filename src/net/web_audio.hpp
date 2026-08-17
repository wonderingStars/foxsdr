// Audio buffering for the browser client (P11): one ring of recent receiver
// audio, written by the application and read independently by each listening
// browser.
//
// WHY A RING WITH PER-READER CURSORS rather than reusing Pipeline::audioTap.
// That tap is a rolling 4096-frame WINDOW — 85 ms at 48 kHz — with no notion of
// what a given reader has already seen. Polling it produces overlapping or
// gapped audio depending on timing, which is fine for the level meter it was
// built for and useless as a stream. A listener needs "everything since I last
// asked", which is a cursor, and several listeners need cursors that do not
// interfere. So the application copies each frame's NEW audio in here once
// (using the pipeline's monotonic produced-counter to know exactly how much is
// new), and every browser reads from its own position.
//
// WHAT HAPPENS WHEN A READER FALLS BEHIND, which is the case that decides
// whether this is usable: a browser on a slow link, or one whose tab was
// backgrounded, stops consuming while the receiver keeps producing. Its cursor
// is eventually overrun. The choice made here is to RESYNC IT TO THE OLDEST
// AVAILABLE SAMPLE and report how much was skipped, rather than to block the
// producer or to disconnect the reader. Blocking the producer would let a
// stalled browser stall the application; disconnecting is a worse experience
// than a glitch. Live audio is the one kind of data where the newest matters
// and the missed part is worthless, so dropping is the right failure.
//
// THREADING. One writer (the application's GUI thread) and any number of
// readers (HTTP worker threads) under one mutex. A mutex rather than a
// lock-free ring because the contention is trivial — one short write per
// rendered frame against a handful of readers — and because a lock-free
// multi-reader ring with resync semantics is a great deal of subtlety to buy
// nothing measurable here.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace cascade::net {

// Audio the receiver produces, and therefore what this carries: mono, 48 kHz,
// float. Matches Pipeline::kAudioRateHz and the mono downmix audioTap emits.
inline constexpr double kWebAudioRateHz = 48000.0;

// Two seconds. Long enough that an ordinary hiccup — a garbage collection in
// the browser, a retransmit — costs nothing, short enough that a reader which
// resyncs after a real stall is not left minutes behind live.
inline constexpr std::size_t kWebAudioCapacity = 96000;

class AudioRing {
public:
    explicit AudioRing(std::size_t capacity = kWebAudioCapacity);

    // Appends `n` samples. A write larger than the capacity keeps only the
    // newest capacity samples — the older part could not be read by anyone
    // before being overwritten anyway.
    void write(const float* samples, std::size_t n);

    // Copies up to `max` samples newer than `cursor` into `dst`, advancing
    // `cursor` past them. Returns how many were copied; 0 means the reader is
    // already up to date.
    //
    // A cursor pointing at data that has since been overwritten is resynced to
    // the oldest sample still held, and `droppedOut` receives the number of
    // samples skipped (0 in the normal case). A cursor from the FUTURE — which
    // can only mean the ring was reset underneath the reader — is treated the
    // same way.
    std::size_t read(std::uint64_t& cursor, float* dst, std::size_t max,
                     std::uint64_t& droppedOut);

    // Total samples ever written. A new reader starts here to begin at live
    // rather than replaying whatever the buffer happens to hold.
    std::uint64_t written() const;

    // Samples currently readable (min(written, capacity)).
    std::size_t available() const;

    std::size_t capacity() const { return capacity_; }

    // Drops everything and restarts the counter. Used when the server stops,
    // so a later run cannot hand a stale cursor readable-looking data.
    void reset();

private:
    mutable std::mutex mutex_;
    std::vector<float> buf_;
    std::size_t capacity_;
    std::uint64_t written_ = 0;  // monotonic count of samples ever written
};

// Converts float samples in [-1, 1] to little-endian signed 16-bit PCM, the
// format the browser client decodes. Clamped, so a hot signal saturates
// instead of wrapping to the opposite polarity — a wrap is far more audible
// than a clip, and on a receiver it sounds like a fault rather than like a
// loud station.
void floatToPcm16le(const float* samples, std::size_t n, std::uint8_t* dst);

}  // namespace cascade::net
