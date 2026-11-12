// access_speech_sapi.hpp - the Windows speech engine (SAPI 5) as a Speaker whose
// output is ALWAYS a memory stream.
//
// SAFETY, stated as the design: SapiMemorySpeaker attaches an in-memory stream
// as the voice's output BEFORE anything is spoken, then reads the output back
// from the voice (ISpVoice::GetOutputStream) and compares it with the stream it
// attached. Only if both hold is speak() allowed to call the engine; otherwise
// it returns false and the engine is never invoked, so a failure to redirect
// can NEVER fall back to the default audio device. Nothing is played on any
// speaker by this class. The audio it renders is available as PCM
// (pcm(), format()) for a caller that wants to examine it or hand it to
// another sink.
//
// The application's own use of a real audio output (the spoken-state feature as
// the owner would use it) is a separate Speaker that the audio lane / screen
// lane K hands the PCM to; it is not in this file on purpose.
//
// COM is initialised on the constructing thread (multithreaded apartment unless
// the thread already has another mode, which is respected) and released in the
// destructor. Not thread-safe: use from one thread.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/access_speech.hpp"

namespace cascade::core::access {

struct PcmFormat {
    std::uint32_t sampleRate = 0;
    std::uint16_t channels = 0;
    std::uint16_t bitsPerSample = 0;
};

class SapiMemorySpeaker : public Speaker {
public:
    SapiMemorySpeaker();
    ~SapiMemorySpeaker() override;
    SapiMemorySpeaker(const SapiMemorySpeaker&) = delete;
    SapiMemorySpeaker& operator=(const SapiMemorySpeaker&) = delete;

    // True when a voice exists AND the memory-stream output is attached and
    // confirmed. False on non-Windows builds and when SAPI is missing.
    bool available() const;
    bool outputIsMemory() const;

    // Render `utf8` into the memory stream (appending to pcm()). Waits up to
    // timeoutMs() for the engine; on timeout the speech is purged and false is
    // returned. interrupt = discard anything not yet rendered first.
    bool speak(const std::string& utf8, bool interrupt) override;
    void stop() override;
    std::string name() const override { return "sapi-memory"; }

    // Everything rendered since construction or the last clearPcm().
    std::vector<std::uint8_t> pcm() const;
    void clearPcm();
    PcmFormat format() const;
    std::string voiceName() const;     // the voice's description, "" if unknown
    std::string lastError() const;
    void setTimeoutMs(std::uint32_t ms);
    std::uint32_t timeoutMs() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace cascade::core::access
