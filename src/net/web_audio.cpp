// Implementation of the browser audio ring. See net/web_audio.hpp for why the
// overrun policy is "resync and report" rather than block or disconnect.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "net/web_audio.hpp"

#include <algorithm>
#include <cmath>

namespace cascade::net {

AudioRing::AudioRing(std::size_t capacity)
    : buf_(capacity == 0 ? 1 : capacity, 0.0f),
      capacity_(capacity == 0 ? 1 : capacity) {}

void AudioRing::write(const float* samples, std::size_t n) {
    if (samples == nullptr || n == 0) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);

    // A write bigger than the ring: only the newest capacity_ samples can
    // survive, so account for the rest as written-and-gone and copy the tail
    // through the ORDINARY wrapped path below.
    //
    // It is tempting to shortcut this by copying the tail straight to
    // buf_.begin(), and that is wrong: readers address the buffer as
    // `cursor % capacity`, so the tail only belongs at index 0 when written_
    // happens to be a multiple of the capacity. Doing it that way returns
    // samples rotated by written_ % capacity — the right COUNT of the wrong
    // data, which is exactly the kind of fault a test that only counted
    // samples would have missed.
    if (n >= capacity_) {
        const std::size_t skip = n - capacity_;
        written_ += skip;
        samples += skip;
        n = capacity_;
    }

    std::size_t pos = static_cast<std::size_t>(written_ % capacity_);
    const std::size_t firstChunk = std::min(n, capacity_ - pos);
    std::copy(samples, samples + firstChunk, buf_.begin() + static_cast<std::ptrdiff_t>(pos));
    if (firstChunk < n) {
        std::copy(samples + firstChunk, samples + n, buf_.begin());
    }
    written_ += n;
}

std::size_t AudioRing::read(std::uint64_t& cursor, float* dst, std::size_t max,
                            std::uint64_t& droppedOut) {
    droppedOut = 0;
    if (dst == nullptr || max == 0) {
        return 0;
    }
    std::lock_guard<std::mutex> lock(mutex_);

    const std::uint64_t held =
        std::min<std::uint64_t>(written_, static_cast<std::uint64_t>(capacity_));
    const std::uint64_t oldest = written_ - held;

    if (cursor < oldest) {
        // Overrun: this reader stalled long enough that what it wanted is gone.
        droppedOut = oldest - cursor;
        cursor = oldest;
    } else if (cursor > written_) {
        // Only possible if the ring was reset under the reader. Treat it as an
        // overrun of everything rather than reading uninitialised memory.
        droppedOut = 0;
        cursor = oldest;
    }

    const std::uint64_t availableU = written_ - cursor;
    const std::size_t n =
        static_cast<std::size_t>(std::min<std::uint64_t>(availableU, max));
    if (n == 0) {
        return 0;
    }

    std::size_t pos = static_cast<std::size_t>(cursor % capacity_);
    const std::size_t firstChunk = std::min(n, capacity_ - pos);
    std::copy(buf_.begin() + static_cast<std::ptrdiff_t>(pos),
              buf_.begin() + static_cast<std::ptrdiff_t>(pos + firstChunk), dst);
    if (firstChunk < n) {
        std::copy(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(n - firstChunk),
                  dst + firstChunk);
    }
    cursor += n;
    return n;
}

std::uint64_t AudioRing::written() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return written_;
}

std::size_t AudioRing::available() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<std::size_t>(
        std::min<std::uint64_t>(written_, static_cast<std::uint64_t>(capacity_)));
}

void AudioRing::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::fill(buf_.begin(), buf_.end(), 0.0f);
    written_ = 0;
}

void floatToPcm16le(const float* samples, std::size_t n, std::uint8_t* dst) {
    if (samples == nullptr || dst == nullptr) {
        return;
    }
    for (std::size_t i = 0; i < n; ++i) {
        float v = samples[i];
        // NaN must not become a random 16-bit pattern; silence it. Clamp to
        // [-1, 1] before scaling so a hot signal clips rather than wrapping.
        if (!(v > -1.0f)) {
            v = (v != v) ? 0.0f : -1.0f;  // v != v is the NaN test
        } else if (v > 1.0f) {
            v = 1.0f;
        }
        // 32767 rather than 32768: scaling by 32768 lets v == 1.0 land on
        // +32768, which does not exist in a signed 16-bit sample and wraps to
        // -32768 — a full-scale positive peak reproduced as a full-scale
        // negative one, i.e. an audible click on the loudest sample.
        const auto s = static_cast<std::int16_t>(std::lround(v * 32767.0f));
        const auto u = static_cast<std::uint16_t>(s);
        dst[2 * i] = static_cast<std::uint8_t>(u & 0xFF);
        dst[2 * i + 1] = static_cast<std::uint8_t>((u >> 8) & 0xFF);
    }
}

}  // namespace cascade::net
