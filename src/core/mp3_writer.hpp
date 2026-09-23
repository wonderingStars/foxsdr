// mp3_writer.hpp - 16-bit PCM in, an .mp3 file out.
//
// WINDOWS ONLY, THROUGH WINDOWS' OWN ENCODER. Media Foundation ships an MP3
// encoder with Windows 8 and later, and an IMFSinkWriter pointed at a ".mp3"
// path wraps it in the MP3 file sink - so FoxSDR carries no encoder of its own
// and no codec licence of its own. Everywhere else available() is false and
// open() refuses with a sentence saying so; the patch page then writes WAV.
//
// MPEG-1 Layer III runs at 32, 44.1 or 48 kHz, which is why callers hand this
// 48 kHz audio - the product's own audio rate.
//
// THREADING. Every call on one Mp3Writer from ONE thread. The COM apartment is
// joined by open() on the calling thread and left by close(), so open, write
// and close must all be made from the same thread (the patch's file worker).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace cascade::core {

class Mp3Writer {
public:
    Mp3Writer() = default;
    ~Mp3Writer();  // close()
    Mp3Writer(const Mp3Writer&) = delete;
    Mp3Writer& operator=(const Mp3Writer&) = delete;

    // Whether this build and this machine can write MP3 at all.
    static bool available();

    // Creates `path` for `channels` of 16-bit PCM at `sampleRateHz`, encoded
    // at `bitrateKbps`. False with a reason in `error`.
    bool open(const std::string& path, unsigned sampleRateHz, unsigned channels,
              unsigned bitrateKbps, std::string& error);

    // Interleaved frames. False once the encoder has refused a write; the file
    // is still finalised by close().
    bool write(const std::int16_t* interleaved, std::size_t frames);

    // Finalises the file (the encoder flushes its last frame and the sink
    // writes the header). Idempotent.
    void close();

    bool isOpen() const { return impl_ != nullptr; }
    std::uint64_t framesWritten() const { return frames_; }

private:
    struct Impl;
    Impl* impl_ = nullptr;
    std::uint64_t frames_ = 0;
};

}  // namespace cascade::core
