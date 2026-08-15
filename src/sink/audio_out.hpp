// PortAudio float32 audio output sink (mono or interleaved stereo).
//
// CHANNELS. The sink opens either one or two channels; the layout is fixed at
// open() and reported by channels(). Two channels are INTERLEAVED L,R,L,R…
// in the ring, exactly as PortAudio wants them in the callback buffer, so the
// realtime path stays a straight ring read with no de-interleaving. The
// producer side is what has to be careful: a partial frame accepted by the
// ring would shift every later sample by one and swap the channels for the
// rest of the session, so writeStereo() rounds its request down to whole
// frames against the ring's free space instead of letting write() split one.
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
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
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

    // Opens deviceIndex (-1 = system default output) as a float32 callback
    // stream at sampleRateHz with `channels` channels (1 = mono, 2 =
    // interleaved stereo; anything else is refused) and starts it. An
    // already-open stream is closed first, so open() doubles as "switch
    // device / rate / layout". Returns false on any PortAudio failure,
    // leaving the object closed.
    //
    // The default argument keeps the original two-argument mono call site
    // (and its tests) behaving exactly as before.
    bool open(int deviceIndex, double sampleRateHz, int channels = 1);

    // Stops and releases the stream. Idempotent: safe to call twice, or
    // without a successful open().
    void close();

    bool running() const { return running_; }

    // Channel layout of the most recent SUCCESSFUL open (1 until one
    // succeeds). Deliberately retained across close(): it describes how the
    // ring's contents are laid out, and the ring outlives the stream.
    int channels() const { return channels_; }

    // Non-blocking producer push of raw ring samples. Returns how many
    // samples the ring accepted (< n when the ring is full — the caller
    // drops or retries; this thread is never made to wait on the audio
    // device). On a stereo sink `samples` must be interleaved and n a
    // multiple of 2 — prefer writeStereo(), which cannot split a frame.
    std::size_t write(const float* samples, std::size_t n);

    // Interleaved stereo push, in FRAMES. Never accepts a partial frame (see
    // the CHANNELS note in the file header): the request is capped at the
    // ring's whole-frame free space, so channel order can never shift.
    // Returns the number of frames accepted.
    std::size_t writeStereo(const float* interleaved, std::size_t frames);

    // Cumulative count of starved callbacks (one per pullBlock that could
    // not fully fill its buffer), monotonic over the object's lifetime.
    std::uint64_t underruns() const {
        return underruns_.load(std::memory_order_relaxed);
    }

    // Output gain, clamped to [0, 1]. Stored in an atomic and applied inside
    // the callback, so the GUI thread can move a slider while audio runs
    // without a lock. One gain for the whole stream, applied by the same
    // per-sample multiply to every channel, so a stereo image can never be
    // tilted by the volume control.
    void setVolume(float v01);

    // The callback core. `frames` is what PortAudio hands the callback, so
    // the buffer it fills is frames * channels() floats: the ring is pulled
    // for that many samples, they are scaled by the current volume, and on
    // starvation the remainder is zero-filled (silence, not stale buffer
    // garbage — both channels of every unfilled frame) with the underrun
    // counter bumped once for the callback. Returns the number of SAMPLES
    // that came from the ring, which for a mono sink equals the frame count
    // (so the original mono contract is unchanged).
    // Static + void* so it is callable both from the C callback and from
    // tests; it must stay lock-free and allocation-free (see file header).
    static std::size_t pullBlock(void* self, float* dst, std::size_t frames);

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
    int channels_ = 1;     // layout of the last successful open (see channels())
};

}  // namespace cascade::sink
