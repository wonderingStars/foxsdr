// PortAudio mono float32 audio output sink.
//
// Threading model, and why the pieces are shaped the way they are:
//
//   DSP / feed thread            PortAudio callback thread
//   ─────────────────            ─────────────────────────
//   write(samples, n) ──SPSC──▶  pullBlock(this, out, frames)
//                      ring
//
// write() is the producer side of a cascade::dsp::SpscRing<float> and never
// blocks: it accepts what fits and reports the count, so a slow or stalled
// audio device can never back-pressure the DSP thread into missing IQ input.
// The PortAudio callback is the consumer side and does nothing except call
// pullBlock() — a ring read, a volume multiply, and a zero-fill when the ring
// is starved. No locks, no allocation, no I/O on that path: the callback runs
// on a realtime audio thread where a blocked mutex or a heap call is an
// audible dropout.
//
// pullBlock() is public and static (taking the object through void*) so tests
// can exercise the exact code the callback runs — starvation, partial fills,
// volume — headless, without opening a real device.
//
// SPDX-License-Identifier: MIT
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "dsp/spsc_ring.hpp"

namespace cascade::sink {

// One selectable output device, as shown in the Sinks panel.
struct AudioDevice {
    int index;       // PortAudio device index, valid for open()
    std::string name;
    bool isDefault;  // true for the host API's default output device
};

class AudioOut {
public:
    // Calls Pa_Initialize(). PortAudio itself refcounts paired
    // Initialize/Terminate calls, so multiple AudioOut instances are safe;
    // the guard here is remembering whether OUR Initialize succeeded so the
    // destructor never calls Pa_Terminate() unmatched (the PortAudio docs
    // forbid Terminate after a failed Initialize).
    AudioOut();
    ~AudioOut();  // close()s any open stream, then releases PortAudio

    // The object owns a PaStream and live atomics; copying one would either
    // double-close the stream or split the ring between two owners.
    AudioOut(const AudioOut&) = delete;
    AudioOut& operator=(const AudioOut&) = delete;

    // Every device with at least one output channel, in PortAudio index
    // order. Empty if PortAudio failed to initialize.
    std::vector<AudioDevice> listOutputDevices();

    // Opens deviceIndex (-1 = system default output) as a mono float32
    // callback stream at sampleRateHz and starts it. An already-open stream
    // is closed first, so open() doubles as "switch device / rate". Returns
    // false on any PortAudio failure, leaving the object closed.
    bool open(int deviceIndex, double sampleRateHz);

    // Stops and releases the stream. Idempotent: safe to call twice, or
    // without a successful open().
    void close();

    bool running() const { return running_; }

    // Non-blocking producer push. Returns how many samples the ring
    // accepted (< n when the ring is full — the caller drops or retries;
    // this thread is never made to wait on the audio device).
    std::size_t write(const float* samples, std::size_t n);

    // Cumulative count of starved callbacks (one per pullBlock that could
    // not fully fill its buffer), monotonic over the object's lifetime.
    std::uint64_t underruns() const {
        return underruns_.load(std::memory_order_relaxed);
    }

    // Output gain, clamped to [0, 1]. Stored in an atomic and applied inside
    // the callback, so the GUI thread can move a slider while audio runs
    // without a lock.
    void setVolume(float v01);

    // The callback core. Pulls up to n samples from the ring into dst,
    // scales them by the current volume, and on starvation zero-fills the
    // remainder (silence, not stale buffer garbage) and bumps the underrun
    // counter once. Returns the number of samples that came from the ring.
    // Static + void* so it is callable both from the C callback and from
    // tests; it must stay lock-free and allocation-free (see file header).
    static std::size_t pullBlock(void* self, float* dst, std::size_t n);

private:
    // 32768 samples ≈ 0.68 s at 48 kHz: deep enough to ride out GUI-thread
    // hiccups on the producer side, shallow enough that a full ring is well
    // under a second of latency. Power of two as SpscRing requires.
    static constexpr std::size_t kRingCapacity = std::size_t{1} << 15;

    dsp::SpscRing<float> ring_;
    std::atomic<float> volume_{1.0f};
    std::atomic<std::uint64_t> underruns_{0};
    // PaStream*, stored as void* so this public header does not force
    // portaudio.h onto every includer; audio_out.cpp casts at the API line.
    void* stream_ = nullptr;
    bool paOk_ = false;    // did OUR Pa_Initialize() succeed?
    bool running_ = false;
};

}  // namespace cascade::sink
