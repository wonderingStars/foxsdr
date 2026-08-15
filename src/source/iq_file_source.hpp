// IQ file playback source: loops a 2-channel RIFF/WAVE recording (ch0 = I,
// ch1 = Q) forever, streaming from disk through a small fixed buffer so a
// multi-gigabyte capture costs the same RAM as a tiny one.
//
// Accepted sample formats, chosen because they are what SDR recorders emit:
//   - PCM 16-bit    scaled to [-1, 1) by 1/32768 (so -32768 maps to exactly
//                   -1.0 and +32767 to 32767/32768; the divisor is a power of
//                   two, making the int16 -> float mapping bit-reproducible)
//   - IEEE float32  passed through bit-exact, no clamping or rescaling
// Everything else (mono, 8/24-bit, ADPCM, extensible, float64, ...) is
// rejected by open() with a specific lastError() message, because silently
// guessing at an unsupported layout would render garbage spectra that look
// like a DSP bug instead of a file problem.
//
// Loop-seam contract: read() delivers the file's complete frames in order and
// wraps from the last frame straight to the first — no inserted zeros, no
// repeated frame, no dropped tail. A trailing partial frame (data bytes not a
// multiple of the frame size) is ignored entirely: it can never be delivered,
// so the seam stays sample-aligned on every pass.
//
// Pacing: this is a free-running source (selfPaced() == false). read() always
// fills the full request immediately; the Pipeline paces consumption with its
// real-time clock, exactly as it already does for SigGen.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <atomic>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

#include "source/iq_source.hpp"

namespace cascade::source {

class IqFileSource : public IqSource {
public:
    IqFileSource() = default;

    // Parses the WAV header and validates the format; does NOT start
    // playback. False on any failure, with the reason in lastError().
    // Reopening replaces the previous file and stops playback first.
    bool open(const std::string& wavPath);

    // start() rewinds to the first frame, so every start() is a
    // deterministic replay from the top (pause/resume is the caller's job:
    // it simply stops calling read()). Idempotent while running. Fails if
    // open() has not succeeded.
    bool start() override;
    void stop() override;  // idempotent; keeps the file open for restart
    bool running() const override;

    // Free-running: samples are synthesized (read) on demand, the caller
    // provides the clock.
    bool selfPaced() const override { return false; }

    double sampleRateHz() const override { return sampleRateHz_; }
    // Always false: the rate is a property of the recording, not a knob.
    bool setSampleRateHz(double hz) override;

    // Nominal only — a file has no tuner. Stored and reported so the
    // frequency display stays coherent.
    double centerFrequencyHz() const override { return centerFrequencyHz_; }
    bool setCenterFrequencyHz(double hz) override;

    // Fills dst with exactly n samples while running (looping seamlessly at
    // EOF); returns 0 when not running (including before open()), without
    // touching lastError() — polling a stopped source is not an error.
    // If the file fails underneath us mid-read (deleted, device error), the
    // remainder is zero-filled and lastError() set: a free-running source
    // must never return 0 for n > 0, or the pipeline's pacing loop would
    // misread it as a transient and spin.
    //
    // Threading: read() only from the pipeline's source thread; open()/
    // start() must not overlap an in-flight read() (the Pipeline serializes
    // this by parking the source thread across restarts).
    std::size_t read(std::complex<float>* dst, std::size_t n) override;

    // "IQ file: <basename>" after a successful open, "IQ file" before.
    const char* name() const override { return name_.c_str(); }

    const char* lastError() const override { return lastError_.c_str(); }

private:
    enum class SampleFormat { none, pcm16, float32 };

    // Central failure path so every parse error both records its reason and
    // leaves the source in a consistent not-opened state.
    bool fail(std::string msg);

    void convertFrames(const unsigned char* raw, std::complex<float>* dst,
                       std::size_t frames);

    std::ifstream file_;
    std::string name_ = "IQ file";
    std::string lastError_;
    SampleFormat format_ = SampleFormat::none;
    double sampleRateHz_ = 0.0;
    double centerFrequencyHz_ = 0.0;
    std::uint64_t dataOffset_ = 0;    // file byte offset of the first frame
    std::uint64_t frameCount_ = 0;    // complete frames in the data chunk
    std::uint64_t nextFrame_ = 0;     // next frame index read() will deliver
    std::size_t bytesPerFrame_ = 0;   // 4 (pcm16) or 8 (float32)
    bool opened_ = false;
    // Atomic because stop() arrives from the control thread while read()
    // polls it from the source thread; everything else is caller-serialized
    // per the threading note on read().
    std::atomic<bool> running_{false};
    std::vector<unsigned char> ioBuf_;  // small streaming buffer, never the file
};

}  // namespace cascade::source
