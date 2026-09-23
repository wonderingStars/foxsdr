// patch_audio.hpp - where a patch speaker's sound goes (0.99.17).
//
// THE OWNER'S RULE: "put all sound to mp3 or wav unless set to speakers or
// another device". So every audio Sink node has an output, and the default is
// a FILE. Each is one of these:
//
//   WAV       core::Recorder, 16-bit mono at 48 kHz, written on the radio's
//             own reader thread (the Recorder is built for exactly that: a
//             DSP-thread writer into a 256 KiB stdio buffer)
//   MP3       core::Mp3Writer on a writer thread of its own, fed through a
//             lock-free ring - an encoder call is not something a radio's
//             reader thread should ever wait on
//   Speakers  sink::AudioOut on the system's default output
//   Device    sink::AudioOut on a named output device
//
// Files go in the recordings folder as "patch-<node>-<name>_<date>_<time>",
// so several speakers recording at once each have a file of their own.
//
// THREADING. write() is called from ONE thread - the reader thread of the
// radio the speaker hangs off. Construction and destruction are on the GUI
// thread, and a destination is only destroyed once no running set holds it
// (the sets own it by shared_ptr and die on the GUI thread; see
// patch_runner.hpp), so a file is never closed under a write.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace cascade::core::patch {

// The rate every destination is written at: the product's audio rate, and one
// MPEG-1 Layer III supports.
inline constexpr double kOutRateHz = 48000.0;

class AudioDest {
public:
    virtual ~AudioDest() = default;

    // Mono samples at kOutRateHz. One thread only (see above).
    virtual void write(const float* samples, std::size_t n) = 0;

    // A line for the node's face: "WAV  patch-4-Airband_20260923_101500.wav".
    virtual std::string describe() const = 0;

    // Non-empty once the destination has stopped taking sound, with why.
    virtual std::string error() const = 0;

    // For the face's meter and counter, readable from any thread.
    float peak() const { return peak_.load(std::memory_order_relaxed); }
    std::uint64_t samples() const { return samples_.load(std::memory_order_relaxed); }

protected:
    void note(const float* s, std::size_t n) {
        float p = 0.0f;
        for (std::size_t i = 0; i < n; ++i) {
            const float a = s[i] < 0.0f ? -s[i] : s[i];
            if (a > p) { p = a; }
        }
        peak_.store(p, std::memory_order_relaxed);
        samples_.fetch_add(n, std::memory_order_relaxed);
    }

private:
    std::atomic<float> peak_{0.0f};
    std::atomic<std::uint64_t> samples_{0};
};

// A name safe to put in a file name: letters, digits, '-' and '_' kept, every
// other character turned into '_', at most 40 characters, never empty.
std::string fileSafeName(const std::string& name);

// "patch-<node>-<fileSafeName(name)>".
std::string patchFilePrefix(unsigned node, const std::string& name);

// Each returns null with a reason in `error` when the destination cannot be
// made. MP3 is null on a build without Windows' encoder - the caller then
// makes a WAV and says so.
std::shared_ptr<AudioDest> makeWavDest(const std::string& directory, const std::string& prefix,
                                       std::string& error);
std::shared_ptr<AudioDest> makeMp3Dest(const std::string& directory, const std::string& prefix,
                                       std::string& error);
// `deviceName` empty means the default output device.
std::shared_ptr<AudioDest> makeDeviceDest(const std::string& deviceName, std::string& error);

}  // namespace cascade::core::patch
